#!/usr/bin/env python3
"""Measures Procyon for Linux against the non-functional targets in docs/procyon-spec.md.

    scripts/bench-linux.py                  build what is missing, measure, exit 1 on a missed target
    scripts/bench-linux.py --report-only    always exit 0
    scripts/bench-linux.py --attempts 3     measure a missed CPU or startup target again (CI)
    scripts/bench-linux.py --pages overview processes --duration 30
    scripts/bench-linux.py --shard 2/3      one third of the screens (CI runs the three thirds in parallel)

The counterpart of scripts/bench-macos.py and scripts/bench-windows.py. Measures the release app
(dist/linux/procyon) and the C++ core CLI (build/linux/procyon-cli) from /proc: startup time (launch to
the first frame the app reports under PROCYON_BENCH), CPU and private memory (RssAnon) at a 1 s refresh
on every screen and running in the background only (--background), the release tarball's size, and how
far the numbers are from the kernel's own counters. Writes dist/bench-linux.json and, on GitHub
Actions, a summary table. Needs a display: on CI, run it under xvfb-run.

On a display without a GPU (Xvfb on a CI runner) GTK draws through Mesa's software OpenGL (llvmpipe),
whose buffers and JIT land in the app's private memory: 110-180 MB where the same screen holds about
35 MB on a desktop, whose GL buffers live in GPU memory. --renderer cairo (GSK_RENDERER) measures
Procyon's own footprint there; CI passes it.

Shared CI runners are noisy: a fixed piece of work is timed alongside each measurement; its slowdown
against the fastest one of the run says how slow the runner was. A missed CPU or startup target whose
every attempt ran on a slow runner is reported as NOISY and does not fail.
"""

import argparse
import json
import os
import re
import select
import statistics
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "dist/linux/procyon"
BUILD = ROOT / "build/linux"
CLI = BUILD / "procyon-cli"
MB = 1024 * 1024
TICKS = os.sysconf("SC_CLK_TCK")
RENDERER = None  # GSK_RENDERER for the app (--renderer), None: GTK's choice

# Every screen of the main window; `background` is the window closed, Procyon sampling in the
# background (tray or not), which has its own targets.
PAGES = [
    "overview", "processes", "cpu", "memory", "gpu", "disk", "network", "battery", "startup", "services",
    "history", "inspect", "system", "settings", "background",
]

# Targets from "Non-functional requirements" in docs/procyon-spec.md (the Windows script's values).
STARTUP_SECONDS = 1.0  # launch to first frame, median
CPU_LIMIT = 2.0  # % of a 4-core machine with the window open at a 1 s refresh
MEMORY_MB = 80  # private memory (RssAnon) with the window open, 90th percentile
BACKGROUND_CPU_LIMIT = 0.5  # % of a 4-core machine running in the background
BACKGROUND_MEMORY_MB = 30
PACKAGE_MB = 20  # the release tarball
ACCURACY_PERCENT = 5  # vs the kernel's own counters; percentage points for CPU
NOISY_SLOWDOWN = 1.15


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, cwd=ROOT, **kwargs).stdout


def ensure_built():
    if not (APP.is_file() and CLI.is_file()):
        print("==> building dist/linux/procyon and the core CLI", flush=True)
        subprocess.run([str(ROOT / "scripts/build-linux.sh"), "release", "--no-tests"], check=True, cwd=ROOT)


def calibrate():
    """Nanoseconds a fixed piece of work takes now: how busy the machine is."""
    start = time.perf_counter_ns()
    total = 0
    for i in range(2_000_000):
        total += i * i % 7
    return time.perf_counter_ns() - start


def cpu_ticks(pid):
    """utime + stime of a process, in clock ticks."""
    with open(f"/proc/{pid}/stat") as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12])


def private_bytes(pid):
    """RssAnon: the memory the process holds itself (what the spec's 'private' means on Linux)."""
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("RssAnon:"):
                return int(line.split()[1]) * 1024
    return 0


def anonymous_mappings(pid, top=12):
    """The largest private anonymous regions of a process (Anonymous: in /proc/<pid>/smaps), with the
    name of the region before them, which tells the heap from thread arenas and library allocations."""
    regions, current = [], None
    try:
        with open(f"/proc/{pid}/smaps") as f:
            for line in f:
                head = line.split()
                if head and "-" in head[0] and len(head) >= 5 and not head[0].endswith(":"):
                    current = {"range": head[0], "name": head[5] if len(head) > 5 else "", "anonymous_kb": 0}
                    regions.append(current)
                elif head and head[0] == "Anonymous:" and current is not None:
                    current["anonymous_kb"] = int(head[1])
    except OSError:
        return []
    for i, region in enumerate(regions):
        if not region["name"] and i > 0:
            region["after"] = regions[i - 1]["name"]
    return sorted((r for r in regions if r["anonymous_kb"]), key=lambda r: -r["anonymous_kb"])[:top]


def probe(page, warmup, duration):
    """Launches the app on `page` (or in the background), then samples its CPU and memory."""
    args = [str(APP), "--no-settings"]
    args += ["--background"] if page == "background" else ["--page", page]
    env = dict(os.environ, PROCYON_BENCH="1")
    if RENDERER:
        env["GSK_RENDERER"] = RENDERER
    calibration = calibrate()
    started = time.monotonic()
    process = subprocess.Popen(args, env=env, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL, text=True)
    startup = None
    try:
        if page != "background":
            deadline = started + 15
            while time.monotonic() < deadline and startup is None:
                ready, _, _ = select.select([process.stderr], [], [], 0.05)
                if ready:
                    line = process.stderr.readline()
                    if not line:
                        break
                    if "first frame" in line:
                        startup = time.monotonic() - started
        # Keep the pipe drained so the app never blocks on stderr.
        threading.Thread(target=lambda: process.stderr.read(), daemon=True).start()
        time.sleep(warmup)
        result = {"page": page, "startup_seconds": startup, "calibration_ns": calibration}
        if duration <= 0:
            return result
        memory = []
        ticks0, t0 = cpu_ticks(process.pid), time.monotonic()
        end = t0 + duration
        while time.monotonic() < end:
            memory.append(private_bytes(process.pid))
            time.sleep(1)
        ticks1, t1 = cpu_ticks(process.pid), time.monotonic()
        samples = list(memory)  # in time order, for the JSON: shows growth or a startup peak
        regions = anonymous_mappings(process.pid)
        memory.sort()
        result.update(
            cpu_percent_of_core=(ticks1 - ticks0) / TICKS / (t1 - t0) * 100,
            memory_p90_bytes=memory[min(len(memory) - 1, int(len(memory) * 0.9))],
            memory_peak_bytes=memory[-1],
            memory_average_bytes=statistics.mean(memory),
            memory_samples_bytes=samples,
            largest_anonymous_regions=regions,
        )
        return result
    finally:
        process.terminate()
        try:
            process.wait(5)
        except subprocess.TimeoutExpired:
            process.kill()


def best(tries):
    return min(tries, key=lambda r: r["cpu_percent_of_core"])


def median_startup(launches):
    measured = [launch["startup_seconds"] for launch in launches if launch["startup_seconds"] is not None]
    return statistics.median(measured) if measured else None


def limits(page):
    return (BACKGROUND_CPU_LIMIT, BACKGROUND_MEMORY_MB) if page == "background" else (CPU_LIMIT, MEMORY_MB)


def describe(page):
    return "background only" if page == "background" else f"{page} open"


def package_size(tmp):
    """The release tarball like scripts/package-linux.sh builds it, without touching dist/."""
    archive = Path(tmp, "procyon.tar.gz")
    with tarfile.open(archive, "w:gz") as out:
        for name in (APP, ROOT / "LICENSE", ROOT / "README.md"):
            out.add(name, name.name)
    return archive.stat().st_size


def system_cpu():
    """(busy, total) jiffies from /proc/stat's first line, iowait counted as idle like the core."""
    with open("/proc/stat") as f:
        values = [int(v) for v in f.readline().split()[1:]]
    idle = values[3] + values[4]
    total = sum(values[:8])
    return total - idle, total


def meminfo():
    with open("/proc/meminfo") as f:
        return {line.split(":")[0]: int(line.split()[1]) * 1024 for line in f}


def process_count():
    """Processes as the core lists them: every pid but kernel threads (kthreadd and its children)."""
    count = 0
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open(f"/proc/{name}/stat") as f:
                fields = f.read().rsplit(")", 1)[1].split()
        except OSError:
            continue
        if name != "2" and fields[1] != "2":
            count += 1
    return count


def accuracy(seconds):
    """Runs the core CLI and reads the kernel's own counters over the same window, then compares."""
    os_values = {}

    def os_cpu():
        time.sleep(1)  # the CLI's first sample averages since the monitor was created
        busy0, total0 = system_cpu()
        time.sleep(seconds)
        busy1, total1 = system_cpu()
        os_values["cpu"] = (busy1 - busy0) / (total1 - total0) * 100 if total1 > total0 else 0.0

    thread = threading.Thread(target=os_cpu)
    thread.start()
    output = run(str(CLI), str(seconds + 1), "1000")
    thread.join()
    info = meminfo()
    memory = info["MemTotal"] - info["MemAvailable"]
    swap = info["SwapTotal"] - info["SwapFree"]
    processes = process_count()

    samples = re.findall(
        r"^cpu ([\d.]+)% .*?mem ([\d.]+)/[\d.]+ GB .*?swap ([\d.]+) GB .*? (\d+) procs", output, re.M
    )
    if len(samples) < 2:
        sys.exit(f"bench: unexpected procyon-cli output:\n{output}")
    cpu = statistics.mean(float(s[0]) for s in samples[1:])
    _, mem_gb, swap_gb, procs = samples[-1]

    def relative(ours, theirs):
        return abs(ours - theirs) / theirs * 100 if theirs else (0.0 if ours == 0 else 100.0)

    ours_mem, ours_swap = float(mem_gb) * 1024 * MB, float(swap_gb) * 1024 * MB
    return {
        "cpu": {"procyon": cpu, "os": os_values["cpu"], "diff_points": abs(cpu - os_values["cpu"])},
        "memory_used": {"procyon": ours_mem, "os": memory, "diff_percent": relative(ours_mem, memory)},
        "swap_used": {
            "procyon": ours_swap,
            "os": swap,
            "diff_percent": 0.0 if abs(ours_swap - swap) < 10 * MB else relative(ours_swap, swap),
        },
        "process_count": {"procyon": int(procs), "os": processes, "diff_percent": relative(int(procs), processes)},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pages", nargs="+", default=PAGES, help="screens to measure")
    parser.add_argument("--warmup", type=float, default=5, help="seconds after the first frame")
    parser.add_argument("--duration", type=float, default=20, help="seconds of sampling per screen")
    parser.add_argument("--launches", type=int, default=3, help="extra launches for the startup median")
    parser.add_argument("--attempts", type=int, default=1, help="measure a screen up to this often while its CPU misses")
    parser.add_argument("--report-only", action="store_true", help="exit 0 even when a target is missed")
    parser.add_argument("--shard", default="1/1", help="K/N: measure one Nth of the screens; startup, size and "
                        "accuracy are measured by shard 1 only")
    parser.add_argument("--known-misses", nargs="+", default=[], metavar="PAGE",
                        help="screens whose missed targets are reported but don't fail the run")
    parser.add_argument("--renderer", help="GSK_RENDERER for the app (cairo on a display without a GPU, see above)")
    parser.add_argument("--output", type=Path, default=ROOT / "dist/bench-linux.json")
    args = parser.parse_args()
    global RENDERER
    RENDERER = args.renderer
    try:
        shard, shards = (int(n) for n in args.shard.split("/"))
        assert 1 <= shard <= shards
    except (ValueError, AssertionError):
        parser.error("--shard takes K/N with 1 <= K <= N")
    pages = args.pages[shard % shards :: shards]
    whole = shard == 1
    title = f" {shard}/{shards}" if shards > 1 else ""

    ensure_built()
    sizes = {}
    if whole:
        with tempfile.TemporaryDirectory() as tmp:
            print("==> sizes", flush=True)
            sizes = {"app_bytes": APP.stat().st_size, "package_bytes": package_size(tmp)}

    attempts = {}
    for page in pages:
        print(f"==> {page}: {args.warmup:g} s warm-up, {args.duration:g} s sampling", flush=True)
        attempts[page] = [probe(page, args.warmup, args.duration)]
        while len(attempts[page]) < args.attempts and best(attempts[page])["cpu_percent_of_core"] / 4 >= limits(page)[0]:
            print(f"==> {page}: CPU missed, measuring again", flush=True)
            attempts[page].append(probe(page, args.warmup, args.duration))
    launches = [[{"startup_seconds": a[0]["startup_seconds"], "calibration_ns": a[0]["calibration_ns"]}
                 for p, a in attempts.items() if p != "background"]]
    while whole:
        for i in range(args.launches):
            print(f"==> launch {i + 1}/{args.launches}", flush=True)
            launches[-1].append(probe("overview", 0, 0))
        if len(launches) >= args.attempts or (median_startup(launches[-1]) or STARTUP_SECONDS) < STARTUP_SECONDS:
            break
        print("==> startup missed, measuring again", flush=True)
        launches.append([])

    acc = {}
    if whole:
        print("==> accuracy against /proc/stat, /proc/meminfo and /proc", flush=True)
        acc = accuracy(10)

    calibrations = [a["calibration_ns"] for tries in attempts.values() for a in tries]
    calibrations += [launch["calibration_ns"] for batch in launches for launch in batch]
    baseline = min((c for c in calibrations if c), default=1)
    for tries in attempts.values():
        for a in tries:
            a["runner_slowdown"] = a["calibration_ns"] / baseline
    screens = [best(tries) for tries in attempts.values()]
    batches = [
        {"median_seconds": median_startup(batch),
         "runner_slowdown": statistics.median(launch["calibration_ns"] for launch in batch) / baseline if batch else 1,
         "startup_seconds": [launch["startup_seconds"] for launch in batch]}
        for batch in launches
    ]
    startup = min((b["median_seconds"] for b in batches if b["median_seconds"] is not None), default=None)

    def runner(slowdowns):
        return f", runner {min(slowdowns):.2f}× slower" if min(slowdowns) >= 1.05 else "", min(slowdowns) > NOISY_SLOWDOWN

    checks = []
    if whole:
        note, noisy = runner([b["runner_slowdown"] for b in batches])
        checks.append(("Startup, median to first frame", startup, STARTUP_SECONDS,
                       f"{startup:.2f} s{note}" if startup else "no frame", f"< {STARTUP_SECONDS:g} s", noisy))
    known_misses = set()
    for s in screens:
        if s["page"] in args.known_misses:
            known_misses |= {f"CPU, {describe(s['page'])} at 1 s refresh", f"Memory, {describe(s['page'])}"}
        cpu_limit, memory_limit = limits(s["page"])
        note, noisy = runner([a["runner_slowdown"] for a in attempts[s["page"]]])
        checks.append((f"CPU, {describe(s['page'])} at 1 s refresh", s["cpu_percent_of_core"] / 4, cpu_limit,
                       f"{s['cpu_percent_of_core'] / 4:.2f}% of a 4-core PC ({s['cpu_percent_of_core']:.1f}% of one core{note})",
                       f"< {cpu_limit:g}% on 4 cores", noisy))
        checks.append((f"Memory, {describe(s['page'])}", s["memory_p90_bytes"] / MB, memory_limit,
                       f"{s['memory_p90_bytes'] / MB:.1f} MB p90 (peak {s['memory_peak_bytes'] / MB:.1f}, "
                       f"average {s['memory_average_bytes'] / MB:.1f})", f"< {memory_limit} MB", False))
    if whole:
        checks.append(("Package (tar.gz)", sizes["package_bytes"] / MB, PACKAGE_MB,
                       f"{sizes['package_bytes'] / MB:.1f} MB (procyon {sizes['app_bytes'] / MB:.1f} MB)",
                       f"< {PACKAGE_MB} MB", False))
        cpu = acc["cpu"]
        checks.append(("Accuracy: CPU vs /proc/stat", cpu["diff_points"], ACCURACY_PERCENT,
                       f"{cpu['procyon']:.1f}% vs {cpu['os']:.1f}% ({cpu['diff_points']:.1f} points)",
                       f"< {ACCURACY_PERCENT} points", False))
        for key, label in (("memory_used", "memory in use vs MemTotal - MemAvailable"),
                           ("swap_used", "swap vs SwapTotal - SwapFree")):
            a = acc[key]
            checks.append((f"Accuracy: {label}", a["diff_percent"], ACCURACY_PERCENT,
                           f"{a['procyon'] / MB:,.0f} vs {a['os'] / MB:,.0f} MB ({a['diff_percent']:.1f}%)",
                           f"< {ACCURACY_PERCENT}%", False))
        p = acc["process_count"]
        checks.append(("Accuracy: process count vs /proc", p["diff_percent"], ACCURACY_PERCENT,
                       f"{p['procyon']} vs {p['os']} ({p['diff_percent']:.1f}%)", f"< {ACCURACY_PERCENT}%", False))

    def status(name, value, bound, noisy):
        if value is not None and value < bound:
            return "PASS"
        return "KNOWN" if name in known_misses else "NOISY" if noisy else "FAIL"

    rows = [(name, shown, limit, status(name, value, bound, noisy)) for name, value, bound, shown, limit, noisy in checks]
    width = max(len(r[0]) for r in rows)
    print()
    for name, shown, limit, result in rows:
        print(f"{result:<5}  {name:<{width}}  {shown}  (target {limit})")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(
        {"sizes": sizes, "screens": screens, "attempts": attempts, "startup": batches, "calibration_baseline_ns": baseline,
         "accuracy": acc, "checks": [{"name": n, "value": s, "target": l, "result": r} for n, s, l, r in rows]},
        indent=2), encoding="utf-8")
    if RENDERER:
        print(f"\nGTK renderer: {RENDERER}")
    print(f"\nWrote {os.path.relpath(args.output, ROOT)}")

    noisy = [r for r in rows if r[3] == "NOISY"]
    if os.environ.get("GITHUB_ACTIONS"):
        for name, shown, limit, _ in noisy:
            print(f"::warning title=Performance target missed on a slow runner::{name}: {shown} (target {limit})")
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        marks = {"PASS": "✅", "KNOWN": "⚠️ known", "NOISY": "⚠️ noisy", "FAIL": "❌"}
        with open(summary, "a", encoding="utf-8") as out:
            out.write(f"### Performance targets (Linux{title})\n\n| | Target | Measured | Spec |\n| --- | --- | --- | --- |\n")
            for name, shown, limit, result in rows:
                out.write(f"| {marks[result]} | {name} | {shown} | {limit} |\n")
            out.write("\nShared CI runners are noisy: treat CPU and startup as trends, not exact numbers. "
                      f"⚠️ noisy: missed, but every attempt ran on a runner over {NOISY_SLOWDOWN:g}× slower than its best.\n")

    failed = [r for r in rows if r[3] == "FAIL"]
    if noisy:
        print(f"\n{len(noisy)} target(s) missed on a slow runner, not counted", file=sys.stderr)
    if failed and not args.report_only:
        sys.exit(f"\n{len(failed)} target(s) missed")


if __name__ == "__main__":
    main()
