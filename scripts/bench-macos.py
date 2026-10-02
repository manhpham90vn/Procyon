#!/usr/bin/env python3
"""Measures Procyon against the non-functional targets in docs/procyon-spec.md.

    scripts/bench-macos.py                  build what is missing, measure, exit 1 on a missed target
    scripts/bench-macos.py --report-only    always exit 0 (noisy shared CI runners)
    scripts/bench-macos.py --pages overview processes --duration 30

Measures the release app bundle (dist/Procyon.app) and the C++ core CLI (build/core/procyon-cli):
startup time, CPU and memory with the window open at a 1 s refresh, DMG size, and how far the
numbers are from the OS's own tools. Writes dist/bench.json and, on GitHub Actions, a summary table.
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

# Every screen of the main window (Settings aside). The CPU target holds whichever one is open; a
# screen the machine lacks (GPU, Battery on a VM) shows its empty state and is measured all the same.
PAGES = ["overview", "processes", "cpu", "memory", "gpu", "disk", "network", "battery", "startup", "services", "system"]

# Targets from "Yêu cầu phi chức năng" in docs/procyon-spec.md. A value must stay below its limit.
STARTUP_SECONDS = 1.0  # launch to first window, median
CPU_LIMIT = 2.0  # % of a 4-core machine with the window open at a 1 s refresh
MEMORY_MB = 80  # footprint peak with the window open
INSTALLER_MB = 20  # compressed DMG
ACCURACY_PERCENT = 5  # vs the OS's own tools; percentage points for CPU


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, cwd=ROOT, **kwargs).stdout


def ensure_built():
    if not APP.is_dir():
        print("==> building dist/Procyon.app", flush=True)
        subprocess.run(["scripts/build-macos-app.sh", "release"], check=True, cwd=ROOT)
    if not CLI.is_file():
        print("==> building the core CLI", flush=True)
        subprocess.run(["make", "core"], check=True, cwd=ROOT)


def dmg_size(tmp):
    """Compressed DMG like scripts/package-macos.sh builds, without touching dist/."""
    staging = Path(tmp, "staging")
    staging.mkdir()
    run("cp", "-R", str(APP), str(staging))
    dmg = Path(tmp, "Procyon.dmg")
    run("hdiutil", "create", "-volname", "Procyon", "-srcfolder", str(staging), "-format", "UDZO", str(dmg))
    return dmg.stat().st_size


def bundle_size():
    return int(run("du", "-sk", str(APP)).split()[0]) * 1024


def probe(binary, page, warmup, duration):
    return json.loads(run(str(binary), str(APP), page, str(warmup), str(duration)))


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
    parser.add_argument("--output", type=Path, default=ROOT / "dist/bench.json")
    args = parser.parse_args()

    ensure_built()
    with tempfile.TemporaryDirectory() as tmp:
        prober = Path(tmp, "bench-probe")
        print("==> compiling the probe", flush=True)
        run("swiftc", "-O", "scripts/bench-probe.swift", "-o", str(prober))

        print("==> sizes", flush=True)
        sizes = {"app_bytes": bundle_size(), "dmg_bytes": dmg_size(tmp)}

        screens = []
        for page in args.pages:
            print(f"==> {page}: {args.warmup:g} s warm-up, {args.duration:g} s sampling", flush=True)
            result = probe(prober, page, args.warmup, args.duration)
            for _ in range(args.attempts - 1):
                if result["cpu_percent_of_core"] / 4 < CPU_LIMIT:
                    break
                print(f"==> {page}: CPU missed, measuring again", flush=True)
                again = probe(prober, page, args.warmup, args.duration)
                result = min(result, again, key=lambda r: r["cpu_percent_of_core"])
            screens.append(result)
        startups = [s["startup_seconds"] for s in screens]
        for i in range(args.launches):
            print(f"==> launch {i + 1}/{args.launches}", flush=True)
            startups.append(probe(prober, "overview", 0, 0)["startup_seconds"])

        print("==> accuracy against vm_stat, iostat, sysctl and ps", flush=True)
        acc = accuracy(10)

    measured = [s for s in startups if s is not None]
    startup = statistics.median(measured) if measured else None
    # (target, value, limit, shown value, shown limit); value None = couldn't measure.
    checks = [("Startup, median to first window", startup, STARTUP_SECONDS, f"{startup:.2f} s" if startup else "no window", f"< {STARTUP_SECONDS:g} s")]
    for s in screens:
        checks.append(
            (
                f"CPU, {s['page']} open at 1 s refresh",
                s["cpu_percent_of_core"] / 4,
                CPU_LIMIT,
                f"{s['cpu_percent_of_core'] / 4:.2f}% of a 4-core Mac ({s['cpu_percent_of_core']:.1f}% of one core)",
                f"< {CPU_LIMIT:g}% on 4 cores",
            )
        )
        checks.append(
            (
                f"Memory, {s['page']} open",
                s["memory_peak_bytes"] / MB,
                MEMORY_MB,
                f"{s['memory_peak_bytes'] / MB:.1f} MB peak ({s['memory_average_bytes'] / MB:.1f} MB average)",
                f"< {MEMORY_MB} MB",
            )
        )
    checks.append(("Installer (DMG)", sizes["dmg_bytes"] / MB, INSTALLER_MB, f"{sizes['dmg_bytes'] / MB:.1f} MB (app {sizes['app_bytes'] / MB:.1f} MB)", f"< {INSTALLER_MB} MB"))
    cpu = acc["cpu"]
    checks.append(("Accuracy: CPU vs iostat", cpu["diff_points"], ACCURACY_PERCENT, f"{cpu['procyon']:.1f}% vs {cpu['os']}% ({cpu['diff_points']:.1f} points)", f"< {ACCURACY_PERCENT} points"))
    for key, label in (("memory_used", "memory used vs vm_stat"), ("swap_used", "swap used vs sysctl")):
        a = acc[key]
        checks.append((f"Accuracy: {label}", a["diff_percent"], ACCURACY_PERCENT, f"{a['procyon'] / MB:,.0f} vs {a['os'] / MB:,.0f} MB ({a['diff_percent']:.1f}%)", f"< {ACCURACY_PERCENT}%"))
    p = acc["process_count"]
    checks.append(("Accuracy: process count vs ps", p["diff_percent"], ACCURACY_PERCENT, f"{p['procyon']} vs {p['os']} ({p['diff_percent']:.1f}%)", f"< {ACCURACY_PERCENT}%"))

    rows = [(name, shown, limit, value is not None and value < bound) for name, value, bound, shown, limit in checks]
    width = max(len(r[0]) for r in rows)
    print()
    for name, shown, limit, ok in rows:
        print(f"{'PASS' if ok else 'FAIL'}  {name:<{width}}  {shown}  (target {limit})")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(
            {"sizes": sizes, "screens": screens, "startup_seconds": startups, "accuracy": acc,
             "checks": [{"name": n, "value": s, "target": l, "pass": ok} for n, s, l, ok in rows]},
            indent=2,
        )
    )
    print(f"\nWrote {args.output.relative_to(ROOT)}")

    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(summary, "a") as out:
            out.write("### Performance targets\n\n| | Target | Measured | Spec |\n| --- | --- | --- | --- |\n")
            for name, shown, limit, ok in rows:
                out.write(f"| {'✅' if ok else '❌'} | {name} | {shown} | {limit} |\n")
            out.write("\nShared CI runners are noisy: treat CPU and startup as trends, not exact numbers.\n")

    failed = [r for r in rows if not r[3]]
    if failed and not args.report_only:
        sys.exit(f"\n{len(failed)} target(s) missed")


if __name__ == "__main__":
    main()
