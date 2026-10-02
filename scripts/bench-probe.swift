// Launches Procyon on one screen, times its first window, then samples its CPU time and memory
// footprint (the "Memory" column of Activity Monitor). Prints one JSON object.
//   swiftc -O scripts/bench-probe.swift -o bench-probe
//   bench-probe <Procyon.app> <page> <warmup seconds> <duration seconds>
// Used by scripts/bench-macos.py.
import AppKit
import Darwin

struct Result: Encodable {
    var page: String
    var startupSeconds: Double?
    var cpuPercentOfCore: Double
    var cpuPercentOfMachine: Double
    var memoryAverageBytes: UInt64
    var memoryPeakBytes: UInt64
}

let arguments = CommandLine.arguments
guard arguments.count == 5, let warmup = Double(arguments[3]), let duration = Double(arguments[4]) else {
    FileHandle.standardError.write(Data("usage: bench-probe <Procyon.app> <page> <warmup s> <duration s>\n".utf8))
    exit(2)
}
let page = arguments[2]

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
    "-initialPage", page, "-refreshInterval", "1", "-fullAccessEnabled", "NO", "-ApplePersistenceIgnoreState", "YES",
]

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
}
let elapsed = now() - start

app.terminate()
for _ in 0..<30 where !app.isTerminated { Thread.sleep(forTimeInterval: 0.1) }
if !app.isTerminated { app.forceTerminate() }

// A zero duration only times the launch.
let cpu = footprints.isEmpty ? 0 : (last.cpu - first.cpu) / elapsed * 100
let result = Result(
    page: page, startupSeconds: startup, cpuPercentOfCore: cpu,
    cpuPercentOfMachine: cpu / Double(ProcessInfo.processInfo.activeProcessorCount),
    memoryAverageBytes: footprints.isEmpty ? 0 : footprints.reduce(0, +) / UInt64(footprints.count),
    memoryPeakBytes: footprints.max() ?? 0)
let encoder = JSONEncoder()
encoder.keyEncodingStrategy = .convertToSnakeCase
print(String(decoding: try encoder.encode(result), as: UTF8.self))
