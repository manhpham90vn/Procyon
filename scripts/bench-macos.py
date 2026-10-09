#!/usr/bin/env python3
"""Measures Procyon against the non-functional targets in docs/procyon-spec.md.

    scripts/bench-macos.py                  build what is missing, measure, exit 1 on a missed target
    scripts/bench-macos.py --report-only    always exit 0
    scripts/bench-macos.py --attempts 3     measure a missed CPU or startup target again (CI)
    scripts/bench-macos.py --pages overview processes --duration 30
    scripts/bench-macos.py --shard 2/3       one third of the screens (CI runs the three thirds in parallel)

Measures the release app bundle (dist/Procyon.app) and the C++ core CLI (build/core/procyon-cli):
startup time, CPU and memory at a 1 s refresh on every screen and with only the menu bar widget
running, DMG size, and how far the numbers are from the OS's own tools. Writes dist/bench.json and, on GitHub Actions, a summary table.

Shared CI runners are noisy: when the host is busy, the same work costs more CPU time, so Procyon's
CPU and startup go up with no change in the code. The probe times a fixed piece of work alongside each
measurement; its slowdown against the fastest one of the run says how slow the runner was. A missed
CPU or startup target whose every attempt ran on a slow runner is reported as NOISY and does not fail.
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "dist/Procyon.app"
CLI = ROOT / "build/core/procyon-cli"
MB = 1024 * 1024

# Every screen of the main window. The CPU target holds whichever one is open; a screen the machine
# lacks (GPU, Battery on a VM) shows its empty state and is measured all the same. `menubar` is the
# window closed, Procyon running in the menu bar only, which has its own targets.
PAGES = [
    "overview", "processes", "cpu", "memory", "gpu", "disk", "network", "energy", "battery", "startup", "services",
    "history", "inspect", "system", "settings", "menubar",
]

# Targets from "Non-functional requirements" in docs/procyon-spec.md. A value must stay below its limit.
STARTUP_SECONDS = 1.0  # launch to first window, median
CPU_LIMIT = 2.0  # % of a 4-core machine with the window open at a 1 s refresh
# Memory is the 90th percentile of the per-second footprint: the peak alone swings by 20 MB between
# runs of the same screen (transient allocations during a table rebuild).
MEMORY_MB = 80  # footprint with the window open
MENU_BAR_CPU_LIMIT = 0.5  # % of a 4-core machine with only the menu bar widget running
MENU_BAR_MEMORY_MB = 30  # footprint running in the background (menu bar only)
INSTALLER_MB = 20  # compressed DMG
ACCURACY_PERCENT = 5  # vs the OS's own tools; percentage points for CPU
# An attempt ran on a slow runner when the calibration work took this much longer than the run's best.
# A quiet machine stays within about 7%.
NOISY_SLOWDOWN = 1.15


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, cwd=ROOT, **kwargs).stdout


def ensure_built(cli=True):
    if not APP.is_dir():
        print("==> building dist/Procyon.app", flush=True)
        subprocess.run(["scripts/build-macos-app.sh", "release"], check=True, cwd=ROOT)
    if cli and not CLI.is_file():
        print("==> building the core CLI", flush=True)
        subprocess.run(["make", "core"], check=True, cwd=ROOT)


def dmg_size(tmp):
    """Compressed DMG like scripts/package-macos.sh builds, without touching dist/."""
    staging = Path(tmp, "staging")
    staging.mkdir()
    run("cp", "-R", str(APP), str(staging))
    dmg = Path(tmp, "Procyon.dmg")
    # hdiutil on CI runners now and then fails with "Resource busy" while something else holds the
    # image; it goes through on a later try.
    for attempt in range(5):
        try:
            run("hdiutil", "create", "-volname", "Procyon", "-srcfolder", str(staging), "-ov", "-format", "UDZO", str(dmg))
            break
        except subprocess.CalledProcessError as e:
            print(f"hdiutil create failed: {e.stderr.strip()}", file=sys.stderr, flush=True)
            if attempt == 4:
                raise
            time.sleep(2 * (attempt + 1))
    return dmg.stat().st_size


def bundle_size():
    return int(run("du", "-sk", str(APP)).split()[0]) * 1024


def limits(page):
    """(CPU limit, memory limit in MB) for a screen."""
    return (MENU_BAR_CPU_LIMIT, MENU_BAR_MEMORY_MB) if page == "menubar" else (CPU_LIMIT, MEMORY_MB)


def describe(page):
    return "menu bar only" if page == "menubar" else f"{page} open"


def probe(binary, page, warmup, duration):
    return json.loads(run(str(binary), str(APP), page, str(warmup), str(duration)))


def best(tries):
    """The attempt with the lowest CPU: runner noise only ever adds CPU."""
    return min(tries, key=lambda r: r["cpu_percent_of_core"])


def median_startup(launches):
    measured = [launch.get("startup_seconds") for launch in launches if launch.get("startup_seconds") is not None]
    return statistics.median(measured) if measured else None


def vm_stat_used():
    """Memory used as Activity Monitor counts it: app (anonymous − purgeable) + wired + compressed."""
    text = run("vm_stat")
    page = int(re.search(r"page size of (\d+) bytes", text).group(1))
    pages = {key.strip(): int(value.strip().rstrip(".")) for key, value in re.findall(r"^([^:]+):\s+(\d+\.)$", text, re.M)}
    used = (
        pages["Anonymous pages"]
        - pages["Pages purgeable"]
        + pages["Pages wired down"]
        + pages["Pages occupied by compressor"]
    )
    return used * page


def swap_used():
    text = run("sysctl", "-n", "vm.swapusage")
    return float(re.search(r"used = ([\d.]+)M", text).group(1)) * MB


def accuracy(seconds):
    """Runs the core CLI and the OS tools over the same window and compares their numbers."""
    iostat = {}

    def os_cpu():
        # Starts with the CLI's second sample: its first one averages since boot.
        time.sleep(1)
        lines = run("iostat", "-n", "0", "-w", str(seconds), "-c", "2").strip().splitlines()
        us, sy, _idle = (int(field) for field in lines[-1].split()[:3])
        iostat["cpu"] = us + sy

    thread = threading.Thread(target=os_cpu)
    thread.start()
    output = run(str(CLI), str(seconds + 1), "1000")
    thread.join()
    memory, swap, processes = vm_stat_used(), swap_used(), len(run("ps", "-A", "-o", "pid=").split())

    samples = re.findall(
        r"^cpu ([\d.]+)% .*?mem ([\d.]+)/[\d.]+ GB .*?swap ([\d.]+) GB .*? (\d+) procs", output, re.M
    )
    if len(samples) < 2:
        sys.exit(f"bench: unexpected procyon-cli output:\n{output}")
    cpu = statistics.mean(float(s[0]) for s in samples[1:])
    _, mem_gb, swap_gb, procs = samples[-1]

    def relative(ours, theirs):
        return abs(ours - theirs) / theirs * 100 if theirs else (0.0 if ours == 0 else 100.0)

    return {
        "cpu": {"procyon": cpu, "os": iostat["cpu"], "diff_points": abs(cpu - iostat["cpu"])},
        "memory_used": {
            "procyon": float(mem_gb) * 1024 * MB,
            "os": memory,
            "diff_percent": relative(float(mem_gb) * 1024 * MB, memory),
        },
        "swap_used": {
            "procyon": float(swap_gb) * 1024 * MB,
            "os": swap,
            # The CLI prints two decimals of a GB; ignore differences below that.
            "diff_percent": 0.0 if abs(float(swap_gb) * 1024 * MB - swap) < 10 * MB else relative(float(swap_gb) * 1024 * MB, swap),
        },
        "process_count": {"procyon": int(procs), "os": processes, "diff_percent": relative(int(procs), processes)},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pages", nargs="+", default=PAGES, help="screens to measure")
    parser.add_argument("--warmup", type=float, default=5, help="seconds after the window appears")
    parser.add_argument("--duration", type=float, default=20, help="seconds of sampling per screen")
    parser.add_argument("--launches", type=int, default=3, help="extra launches for the startup median")
    parser.add_argument("--attempts", type=int, default=1, help="measure a screen up to this often while its CPU misses")
    parser.add_argument("--report-only", action="store_true", help="exit 0 even when a target is missed")
    parser.add_argument(
        "--shard",
        default="1/1",
        help="K/N: measure one Nth of the screens (CI splits them across N runners); "
        "startup, size and accuracy are measured by shard 1 only",
    )
    parser.add_argument(
        "--known-misses", nargs="+", default=[], metavar="PAGE",
        help="screens whose missed targets are reported but don't fail the run",
    )
    parser.add_argument("--output", type=Path, default=ROOT / "dist/bench.json")
    args = parser.parse_args()
    try:
        shard, shards = (int(n) for n in args.shard.split("/"))
        assert 1 <= shard <= shards
    except (ValueError, AssertionError):
        parser.error("--shard takes K/N with 1 <= K <= N")
    # Every Nth screen; shard 1, which also measures startup, size and accuracy, takes a shorter share.
    pages = args.pages[shard % shards :: shards]
    whole = shard == 1  # the machine-wide measurements run once per run, on shard 1
    title = f" {shard}/{shards}" if shards > 1 else ""

    ensure_built(cli=whole)  # the CLI is for the accuracy checks
    with tempfile.TemporaryDirectory() as tmp:
        prober = Path(tmp, "bench-probe")
        print("==> compiling the probe", flush=True)
        run("swiftc", "-O", "scripts/bench-probe.swift", "-o", str(prober))

        sizes = {}
        if whole:
            print("==> sizes", flush=True)
            sizes = {"app_bytes": bundle_size(), "dmg_bytes": dmg_size(tmp)}

        # Every attempt of a screen; the reported one is its lowest CPU.
        attempts = {}
        for page in pages:
            print(f"==> {page}: {args.warmup:g} s warm-up, {args.duration:g} s sampling", flush=True)
            attempts[page] = [probe(prober, page, args.warmup, args.duration)]
            while len(attempts[page]) < args.attempts and best(attempts[page])["cpu_percent_of_core"] / 4 >= limits(page)[0]:
                print(f"==> {page}: CPU missed, measuring again", flush=True)
                attempts[page].append(probe(prober, page, args.warmup, args.duration))
        # Batches of launches; each screen's first window counts towards the first batch.
        # A probe whose window never showed has no startup; it counts as a missed launch, not a crash.
        launches = [[{"startup_seconds": a[0].get("startup_seconds"), "calibration_ns": a[0]["calibration_ns"]} for a in attempts.values()]]
        while whole:
            for i in range(args.launches):
                print(f"==> launch {i + 1}/{args.launches}", flush=True)
                launches[-1].append(probe(prober, "overview", 0, 0))
            if len(launches) >= args.attempts or (median_startup(launches[-1]) or STARTUP_SECONDS) < STARTUP_SECONDS:
                break
            print("==> startup missed, measuring again", flush=True)
            launches.append([])

        acc = {}
        if whole:
            print("==> accuracy against vm_stat, iostat, sysctl and ps", flush=True)
            acc = accuracy(10)

    # The fastest calibration of the run stands for the runner at its quietest.
    calibrations = [a["calibration_ns"] for tries in attempts.values() for a in tries]
    calibrations += [launch["calibration_ns"] for batch in launches for launch in batch]
    baseline = min((c for c in calibrations if c), default=1)

    def slowdown(calibration_ns):
        return calibration_ns / baseline

    for page, tries in attempts.items():
        for a in tries:
            a["runner_slowdown"] = slowdown(a["calibration_ns"])
    screens = [best(tries) for tries in attempts.values()]
    batches = [
        {"median_seconds": median_startup(batch), "runner_slowdown": slowdown(statistics.median(launch["calibration_ns"] for launch in batch)),
         "startup_seconds": [launch.get("startup_seconds") for launch in batch]}
        for batch in launches
    ]
    startup_batch = min(batches, key=lambda b: b["median_seconds"] if b["median_seconds"] is not None else float("inf"))
    startup = startup_batch["median_seconds"]

    def runner(slowdowns):
        """How slow the runner was, and whether every attempt ran on a slow one."""
        return f", runner {min(slowdowns):.2f}× slower" if min(slowdowns) >= 1.05 else "", min(slowdowns) > NOISY_SLOWDOWN

    # (target, value, limit, shown value, shown limit, every attempt noisy); value None = couldn't measure.
    note, noisy = runner([b["runner_slowdown"] for b in batches])
    checks = []
    if whole:
        checks.append(("Startup, median to first window", startup, STARTUP_SECONDS, f"{startup:.2f} s{note}" if startup else "no window", f"< {STARTUP_SECONDS:g} s", noisy))
    known_misses = set()
    for s in screens:
        if s["page"] in args.known_misses:
            known_misses |= {f"CPU, {describe(s['page'])} at 1 s refresh", f"Memory, {describe(s['page'])}"}
        cpu_limit, memory_limit = limits(s["page"])
        note, noisy = runner([a["runner_slowdown"] for a in attempts[s["page"]]])
        checks.append(
            (
                f"CPU, {describe(s['page'])} at 1 s refresh",
                s["cpu_percent_of_core"] / 4,
                cpu_limit,
                f"{s['cpu_percent_of_core'] / 4:.2f}% of a 4-core Mac ({s['cpu_percent_of_core']:.1f}% of one core{note})",
                f"< {cpu_limit:g}% on 4 cores",
                noisy,
            )
        )
        checks.append(
            (
                f"Memory, {describe(s['page'])}",
                s["memory_p90_bytes"] / MB,
                memory_limit,
                f"{s['memory_p90_bytes'] / MB:.1f} MB p90 (peak {s['memory_peak_bytes'] / MB:.1f}, average {s['memory_average_bytes'] / MB:.1f})",
                f"< {memory_limit} MB",
                False,
            )
        )
    if whole:
        checks.append(("Installer (DMG)", sizes["dmg_bytes"] / MB, INSTALLER_MB, f"{sizes['dmg_bytes'] / MB:.1f} MB (app {sizes['app_bytes'] / MB:.1f} MB)", f"< {INSTALLER_MB} MB", False))
        cpu = acc["cpu"]
        checks.append(("Accuracy: CPU vs iostat", cpu["diff_points"], ACCURACY_PERCENT, f"{cpu['procyon']:.1f}% vs {cpu['os']}% ({cpu['diff_points']:.1f} points)", f"< {ACCURACY_PERCENT} points", False))
        for key, label in (("memory_used", "memory used vs vm_stat"), ("swap_used", "swap used vs sysctl")):
            a = acc[key]
            checks.append((f"Accuracy: {label}", a["diff_percent"], ACCURACY_PERCENT, f"{a['procyon'] / MB:,.0f} vs {a['os'] / MB:,.0f} MB ({a['diff_percent']:.1f}%)", f"< {ACCURACY_PERCENT}%", False))
        p = acc["process_count"]
        checks.append(("Accuracy: process count vs ps", p["diff_percent"], ACCURACY_PERCENT, f"{p['procyon']} vs {p['os']} ({p['diff_percent']:.1f}%)", f"< {ACCURACY_PERCENT}%", False))

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
    args.output.write_text(
        json.dumps(
            {"sizes": sizes, "screens": screens, "attempts": attempts, "startup": batches, "calibration_baseline_ns": baseline,
             "accuracy": acc, "checks": [{"name": n, "value": s, "target": l, "result": r} for n, s, l, r in rows]},
            indent=2,
        )
    )
    print(f"\nWrote {os.path.relpath(args.output, ROOT)}")

    noisy = [r for r in rows if r[3] == "NOISY"]
    if os.environ.get("GITHUB_ACTIONS"):
        for name, shown, limit, _ in noisy:
            print(f"::warning title=Performance target missed on a slow runner::{name}: {shown} (target {limit})")
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        marks = {"PASS": "✅", "KNOWN": "⚠️ known", "NOISY": "⚠️ noisy", "FAIL": "❌"}
        with open(summary, "a") as out:
            out.write(f"### Performance targets{title}\n\n| | Target | Measured | Spec |\n| --- | --- | --- | --- |\n")
            for name, shown, limit, result in rows:
                out.write(f"| {marks[result]} | {name} | {shown} | {limit} |\n")
            out.write(
                "\nShared CI runners are noisy: treat CPU and startup as trends, not exact numbers. "
                f"⚠️ noisy: missed, but every attempt ran on a runner over {NOISY_SLOWDOWN:g}× slower than its best.\n"
            )

    failed = [r for r in rows if r[3] == "FAIL"]
    if noisy:
        print(f"\n{len(noisy)} target(s) missed on a slow runner, not counted", file=sys.stderr)
    if failed and not args.report_only:
        sys.exit(f"\n{len(failed)} target(s) missed")


if __name__ == "__main__":
    main()
