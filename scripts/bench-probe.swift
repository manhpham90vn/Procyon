// Launches Procyon on one screen, times its first window, then samples its CPU time and memory
// footprint (the "Memory" column of Activity Monitor). Prints one JSON object.
//   swiftc -O scripts/bench-probe.swift -o bench-probe
//   bench-probe <Procyon.app> <page> <warmup seconds> <duration seconds>
// The page `menubar` opens the window, closes it and samples Procyon running in the menu bar only.
// Alongside, it times a fixed piece of work once a second (`calibration_ns`): on a shared CI runner
// the same work takes more CPU time when the host is busy or slower, which inflates Procyon's CPU too.
// Used by scripts/bench-macos.py.
import AppKit
import Darwin

struct Result: Encodable {
    var page: String
    var startupSeconds: Double?
    var cpuPercentOfCore: Double
    var cpuPercentOfMachine: Double
    var memoryAverageBytes: UInt64
    var memoryP90Bytes: UInt64
    var memoryPeakBytes: UInt64
    /// Median CPU time of the fixed calibration work while measuring; lower is a faster machine.
    var calibrationNs: UInt64

    enum CodingKeys: CodingKey {
        case page, startupSeconds, cpuPercentOfCore, cpuPercentOfMachine, memoryAverageBytes, memoryP90Bytes,
            memoryPeakBytes, calibrationNs
    }

    // Written out by hand so that a missing startup (no window within the deadline) is `null`, as the
    // Windows and Linux probes print it; the synthesized encoder would drop the key instead.
    func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encode(page, forKey: .page)
        try container.encode(startupSeconds, forKey: .startupSeconds)
        try container.encode(cpuPercentOfCore, forKey: .cpuPercentOfCore)
        try container.encode(cpuPercentOfMachine, forKey: .cpuPercentOfMachine)
        try container.encode(memoryAverageBytes, forKey: .memoryAverageBytes)
        try container.encode(memoryP90Bytes, forKey: .memoryP90Bytes)
        try container.encode(memoryPeakBytes, forKey: .memoryPeakBytes)
        try container.encode(calibrationNs, forKey: .calibrationNs)
    }
}

let arguments = CommandLine.arguments
guard arguments.count == 5, let warmup = Double(arguments[3]), let duration = Double(arguments[4]) else {
    FileHandle.standardError.write(Data("usage: bench-probe <Procyon.app> <page> <warmup s> <duration s>\n".utf8))
    exit(2)
}
let page = arguments[2]
let menuBarOnly = page == "menubar"

var timebase = mach_timebase_info_data_t()
mach_timebase_info(&timebase)
func nanoseconds(_ ticks: UInt64) -> Double { Double(ticks) * Double(timebase.numer) / Double(timebase.denom) }
func now() -> Double { nanoseconds(mach_absolute_time()) / 1e9 }

/// CPU seconds and footprint of `pid`.
func usage(_ pid: pid_t) -> (cpu: Double, footprint: UInt64)? {
    var info = rusage_info_v2()
    let status = withUnsafeMutablePointer(to: &info) {
        $0.withMemoryRebound(to: rusage_info_t?.self, capacity: 1) { proc_pid_rusage(pid, RUSAGE_INFO_V2, $0) }
    }
    guard status == 0 else { return nil }
    return (nanoseconds(info.ri_user_time + info.ri_system_time) / 1e9, info.ri_phys_footprint)
}

/// CPU time of a fixed piece of work (about a millisecond), the best of three: preemption and
/// interrupts only ever add to it.
func calibrate() -> UInt64 {
    var best = UInt64.max
    for _ in 0..<3 {
        let start = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)
        var x: UInt64 = 0x9E37_79B9_7F4A_7C15
        for i in 0..<400_000 { x = (x ^ UInt64(i)) &* 0xBF58_476D_1CE4_E5B9 &+ (x >> 31) }
        let elapsed = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - start
        if x == 0 { print("") }  // keeps the loop from being optimized away
        best = min(best, elapsed)
    }
    return best
}

func median(_ values: [UInt64]) -> UInt64 { values.isEmpty ? 0 : values.sorted()[values.count / 2] }

/// Whether `pid` shows a normal window on screen. Owner and bounds need no screen-recording permission.
func hasWindow(_ pid: pid_t) -> Bool {
    let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] ?? []
    return windows.contains { window in
        guard window[kCGWindowOwnerPID as String] as? pid_t == pid, window[kCGWindowLayer as String] as? Int == 0,
            let bounds = window[kCGWindowBounds as String] as? [String: Double]
        else { return false }
        return (bounds["Width"] ?? 0) > 200 && (bounds["Height"] ?? 0) > 200
    }
}

let configuration = NSWorkspace.OpenConfiguration()
configuration.createsNewApplicationInstance = true
configuration.activates = true
configuration.addsToRecentItems = false
// Argument-domain defaults: the spec's 1 s refresh, no admin helper, no restored window state.
configuration.arguments = [
    "-initialPage", menuBarOnly ? "overview" : page, "-refreshInterval", "1", "-fullAccessEnabled", "NO",
    "-ApplePersistenceIgnoreState", "YES",
]
if menuBarOnly { configuration.arguments += ["-launchInMenuBar", "YES", "-menuBarEnabled", "YES"] }

// Calibrate before launching: a zero duration (startup only) has no sampling loop.
var calibrations = (0..<5).map { _ in calibrate() }
let launched = now()
nonisolated(unsafe) var opened: NSRunningApplication?
nonisolated(unsafe) var openError: Error?
let done = DispatchSemaphore(value: 0)
NSWorkspace.shared.openApplication(at: URL(fileURLWithPath: arguments[1]), configuration: configuration) {
    opened = $0
    openError = $1
    done.signal()
}
done.wait()
guard let app = opened else {
    FileHandle.standardError.write(Data("bench-probe: can't open the app: \(String(describing: openError))\n".utf8))
    exit(1)
}
let pid = app.processIdentifier

var startup: Double?
while now() - launched < 20 {
    if hasWindow(pid) {
        startup = now() - launched
        break
    }
    usleep(5_000)
}
if menuBarOnly {
    let shown = now()
    while hasWindow(pid), now() - shown < 10 { usleep(50_000) }
    if hasWindow(pid) {
        FileHandle.standardError.write(Data("bench-probe: the window didn't close\n".utf8))
        app.forceTerminate()
        exit(1)
    }
}

Thread.sleep(forTimeInterval: warmup)
guard let first = usage(pid) else {
    FileHandle.standardError.write(Data("bench-probe: can't read usage of pid \(pid)\n".utf8))
    app.forceTerminate()
    exit(1)
}
let start = now()
var footprints: [UInt64] = []
var last = first
while now() - start < duration {
    Thread.sleep(forTimeInterval: 1)
    guard let sample = usage(pid) else { break }
    footprints.append(sample.footprint)
    last = sample
    calibrations.append(calibrate())
}
let elapsed = now() - start

// In the menu bar only, a plain quit request is cancelled (Procyon stays in the menu bar).
if menuBarOnly { app.forceTerminate() } else { app.terminate() }
for _ in 0..<30 where !app.isTerminated { Thread.sleep(forTimeInterval: 0.1) }
if !app.isTerminated { app.forceTerminate() }

// A zero duration only times the launch.
let cpu = footprints.isEmpty ? 0 : (last.cpu - first.cpu) / elapsed * 100
let result = Result(
    page: page, startupSeconds: startup, cpuPercentOfCore: cpu,
    cpuPercentOfMachine: cpu / Double(ProcessInfo.processInfo.activeProcessorCount),
    memoryAverageBytes: footprints.isEmpty ? 0 : footprints.reduce(0, +) / UInt64(footprints.count),
    memoryP90Bytes: footprints.isEmpty ? 0 : footprints.sorted()[(footprints.count - 1) * 9 / 10],
    memoryPeakBytes: footprints.max() ?? 0, calibrationNs: median(calibrations))
let encoder = JSONEncoder()
encoder.keyEncodingStrategy = .convertToSnakeCase
print(String(decoding: try encoder.encode(result), as: UTF8.self))
