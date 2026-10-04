#!/usr/bin/env python3
"""Measures Procyon for Windows against the non-functional targets in docs/procyon-spec.md.

    python scripts\\bench-windows.py                  build what is missing, measure, exit 1 on a missed target
    python scripts\\bench-windows.py --report-only    always exit 0
    python scripts\\bench-windows.py --attempts 3     measure a missed CPU or startup target again (CI)
    python scripts\\bench-windows.py --pages overview processes --duration 30

The counterpart of scripts/bench-macos.py. Measures the release app (dist\\windows\\Procyon.exe) and the
C++ core CLI (build\\windows\\procyon-cli.exe) through build\\windows\\apps\\windows\\procyon-bench-probe.exe:
startup time, CPU and private working set at a 1 s refresh on every screen and with only the tray icon
running, the release zip's size, and how far the numbers are from the OS's own counters. Writes
dist\\bench-windows.json and, on GitHub Actions, a summary table.

Shared CI runners are noisy: when the host is busy, the same work takes longer, so Procyon's CPU and
startup go up with no change in the code. The probe times a fixed piece of work alongside each
measurement; its slowdown against the fastest one of the run says how slow the runner was. A missed
CPU or startup target whose every attempt ran on a slow runner is reported as NOISY and does not fail.
"""

import argparse
import ctypes
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "dist/windows/Procyon.exe"
BUILD = ROOT / "build/windows"
CLI = BUILD / "procyon-cli.exe"
PROBE = BUILD / "apps/windows/procyon-bench-probe.exe"
MB = 1024 * 1024

# Every screen of the main window. The CPU target holds whichever one is open; a screen the machine
# lacks (GPU, Battery on a VM) shows its empty state and is measured all the same. `tray` is the
# window closed, Procyon running in the tray only, which has its own targets.
PAGES = [
    "overview", "processes", "cpu", "memory", "gpu", "disk", "network", "battery", "startup", "services",
    "history", "inspect", "system", "settings", "tray",
]

# Targets from "Non-functional requirements" in docs/procyon-spec.md. A value must stay below its limit.
STARTUP_SECONDS = 1.0  # launch to first window, median
CPU_LIMIT = 2.0  # % of a 4-core machine with the window open at a 1 s refresh
# Memory is the 90th percentile of the per-second private working set: the peak alone swings between
# runs of the same screen (transient allocations during a table rebuild).
MEMORY_MB = 80  # private working set with the window open
TRAY_CPU_LIMIT = 0.5  # % of a 4-core machine with only the tray icon running
TRAY_MEMORY_MB = 30  # private working set running in the background (tray only)
INSTALLER_MB = 20  # the release zip
ACCURACY_PERCENT = 5  # vs the OS's own counters; percentage points for CPU
# An attempt ran on a slow runner when the calibration work took this much longer than the run's best.
# A quiet machine stays within about 7%.
NOISY_SLOWDOWN = 1.15

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, cwd=ROOT, **kwargs).stdout


def ensure_built():
    if not (APP.is_file() and CLI.is_file() and PROBE.is_file()):
        print("==> building dist\\windows\\Procyon.exe, the core CLI and the probe", flush=True)
        subprocess.run(["cmd", "/c", r"scripts\build-windows.cmd", "release", "--no-tests"], check=True, cwd=ROOT)


def zip_size(tmp):
    """The release zip like .github/workflows/release.yml builds it, without touching dist\\."""
    archive = Path(tmp, "Procyon-windows-x64.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
        for name in (APP, ROOT / "LICENSE", ROOT / "README.md"):
            out.write(name, name.name)
    return archive.stat().st_size


def limits(page):
    """(CPU limit, memory limit in MB) for a screen."""
    return (TRAY_CPU_LIMIT, TRAY_MEMORY_MB) if page == "tray" else (CPU_LIMIT, MEMORY_MB)


def describe(page):
    return "tray only" if page == "tray" else f"{page} open"


def probe(page, warmup, duration):
    return json.loads(run(str(PROBE), str(APP), page, str(warmup), str(duration)))


def best(tries):
    """The attempt with the lowest CPU: runner noise only ever adds CPU."""
    return min(tries, key=lambda r: r["cpu_percent_of_core"])


def median_startup(launches):
    measured = [launch["startup_seconds"] for launch in launches if launch["startup_seconds"] is not None]
    return statistics.median(measured) if measured else None


def filetime(value):
    return (value.dwHighDateTime << 32) | value.dwLowDateTime


def system_times():
    """(idle, kernel, user) in 100 ns units, summed over every processor; kernel includes idle."""
    idle, kernel, user = wintypes.FILETIME(), wintypes.FILETIME(), wintypes.FILETIME()
    if not kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
        raise ctypes.WinError(ctypes.get_last_error())
    return filetime(idle), filetime(kernel), filetime(user)


class MemoryStatusEx(ctypes.Structure):
    _fields_ = [
        ("dwLength", wintypes.DWORD), ("dwMemoryLoad", wintypes.DWORD), ("ullTotalPhys", ctypes.c_uint64),
        ("ullAvailPhys", ctypes.c_uint64), ("ullTotalPageFile", ctypes.c_uint64), ("ullAvailPageFile", ctypes.c_uint64),
        ("ullTotalVirtual", ctypes.c_uint64), ("ullAvailVirtual", ctypes.c_uint64), ("ullAvailExtendedVirtual", ctypes.c_uint64),
    ]


def memory_in_use():
    """Memory in use as Task Manager counts it: physical memory minus what is available (free + standby)."""
    status = MemoryStatusEx(dwLength=ctypes.sizeof(MemoryStatusEx))
    if not kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        raise ctypes.WinError(ctypes.get_last_error())
    return status.ullTotalPhys - status.ullAvailPhys


def pagefile_in_use():
    """Page file usage from WMI (Win32_PageFileUsage.CurrentUsage, in MB); 0 without a page file."""
    text = run(
        "powershell", "-NoProfile", "-Command",
        "(Get-CimInstance Win32_PageFileUsage | Measure-Object -Property CurrentUsage -Sum).Sum",
    ).strip()
    return float(text or 0) * MB


def process_count():
    """Processes as Task Manager counts them: every pid but the idle process (0)."""
    size = 4096
    while True:
        pids = (wintypes.DWORD * size)()
        returned = wintypes.DWORD()
        if not psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(returned)):
            raise ctypes.WinError(ctypes.get_last_error())
        count = returned.value // ctypes.sizeof(wintypes.DWORD)
        if count < size:
            return sum(1 for pid in pids[:count] if pid != 0)
        size *= 2


def accuracy(seconds):
    """Runs the core CLI and reads the OS's own counters over the same window, then compares."""
    os_values = {}

    def os_cpu():
        # Starts with the CLI's second sample: its first one averages since the monitor was created.
        time.sleep(1)
        idle0, kernel0, user0 = system_times()
        time.sleep(seconds)
        idle1, kernel1, user1 = system_times()
        total = (kernel1 - kernel0) + (user1 - user0)
        os_values["cpu"] = (total - (idle1 - idle0)) / total * 100 if total else 0.0

    thread = threading.Thread(target=os_cpu)
    thread.start()
    output = run(str(CLI), str(seconds + 1), "1000")
    thread.join()
    memory, swap, processes = memory_in_use(), pagefile_in_use(), process_count()

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
        "cpu": {"procyon": cpu, "os": os_values["cpu"], "diff_points": abs(cpu - os_values["cpu"])},
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
        "--known-misses", nargs="+", default=[], metavar="PAGE",
        help="screens whose missed targets are reported but don't fail the run",
    )
    parser.add_argument("--output", type=Path, default=ROOT / "dist/bench-windows.json")
    args = parser.parse_args()

    ensure_built()
    with tempfile.TemporaryDirectory() as tmp:
        print("==> sizes", flush=True)
        sizes = {"app_bytes": APP.stat().st_size, "zip_bytes": zip_size(tmp)}

    # Every attempt of a screen; the reported one is its lowest CPU.
    attempts = {}
    for page in args.pages:
        print(f"==> {page}: {args.warmup:g} s warm-up, {args.duration:g} s sampling", flush=True)
        attempts[page] = [probe(page, args.warmup, args.duration)]
        while len(attempts[page]) < args.attempts and best(attempts[page])["cpu_percent_of_core"] / 4 >= limits(page)[0]:
            print(f"==> {page}: CPU missed, measuring again", flush=True)
            attempts[page].append(probe(page, args.warmup, args.duration))
    # Batches of launches; each screen's first window counts towards the first batch.
    launches = [[{"startup_seconds": a[0]["startup_seconds"], "calibration_ns": a[0]["calibration_ns"]} for a in attempts.values()]]
    while True:
        for i in range(args.launches):
            print(f"==> launch {i + 1}/{args.launches}", flush=True)
            launches[-1].append(probe("overview", 0, 0))
        if len(launches) >= args.attempts or (median_startup(launches[-1]) or STARTUP_SECONDS) < STARTUP_SECONDS:
            break
        print("==> startup missed, measuring again", flush=True)
        launches.append([])

    print("==> accuracy against GetSystemTimes, GlobalMemoryStatusEx, Win32_PageFileUsage and EnumProcesses", flush=True)
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
         "startup_seconds": [launch["startup_seconds"] for launch in batch]}
        for batch in launches
    ]
    startup_batch = min(batches, key=lambda b: b["median_seconds"] if b["median_seconds"] is not None else float("inf"))
    startup = startup_batch["median_seconds"]

    def runner(slowdowns):
        """How slow the runner was, and whether every attempt ran on a slow one."""
        return f", runner {min(slowdowns):.2f}× slower" if min(slowdowns) >= 1.05 else "", min(slowdowns) > NOISY_SLOWDOWN

    # (target, value, limit, shown value, shown limit, every attempt noisy); value None = couldn't measure.
    note, noisy = runner([b["runner_slowdown"] for b in batches])
    checks = [("Startup, median to first window", startup, STARTUP_SECONDS, f"{startup:.2f} s{note}" if startup else "no window", f"< {STARTUP_SECONDS:g} s", noisy)]
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
                f"{s['cpu_percent_of_core'] / 4:.2f}% of a 4-core PC ({s['cpu_percent_of_core']:.1f}% of one core{note})",
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
    checks.append(("Installer (zip)", sizes["zip_bytes"] / MB, INSTALLER_MB, f"{sizes['zip_bytes'] / MB:.1f} MB (Procyon.exe {sizes['app_bytes'] / MB:.1f} MB)", f"< {INSTALLER_MB} MB", False))
    cpu = acc["cpu"]
    checks.append(("Accuracy: CPU vs GetSystemTimes", cpu["diff_points"], ACCURACY_PERCENT, f"{cpu['procyon']:.1f}% vs {cpu['os']:.1f}% ({cpu['diff_points']:.1f} points)", f"< {ACCURACY_PERCENT} points", False))
    for key, label in (("memory_used", "memory in use vs GlobalMemoryStatusEx"), ("swap_used", "page file vs Win32_PageFileUsage")):
        a = acc[key]
        checks.append((f"Accuracy: {label}", a["diff_percent"], ACCURACY_PERCENT, f"{a['procyon'] / MB:,.0f} vs {a['os'] / MB:,.0f} MB ({a['diff_percent']:.1f}%)", f"< {ACCURACY_PERCENT}%", False))
    p = acc["process_count"]
    checks.append(("Accuracy: process count vs EnumProcesses", p["diff_percent"], ACCURACY_PERCENT, f"{p['procyon']} vs {p['os']} ({p['diff_percent']:.1f}%)", f"< {ACCURACY_PERCENT}%", False))

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
        ),
        encoding="utf-8",
    )
    print(f"\nWrote {os.path.relpath(args.output, ROOT)}")

    noisy = [r for r in rows if r[3] == "NOISY"]
    if os.environ.get("GITHUB_ACTIONS"):
        for name, shown, limit, _ in noisy:
            print(f"::warning title=Performance target missed on a slow runner::{name}: {shown} (target {limit})")
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        marks = {"PASS": "✅", "KNOWN": "⚠️ known", "NOISY": "⚠️ noisy", "FAIL": "❌"}
        with open(summary, "a", encoding="utf-8") as out:
            out.write("### Performance targets (Windows)\n\n| | Target | Measured | Spec |\n| --- | --- | --- | --- |\n")
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
