// Takes a screenshot of every screen of Procyon, in light and dark mode. Each screen gets its own
// launch (`-initialPage`) with a fixed window size, waits for the charts to fill, then captures the
// window alone with `screencapture -l`. Needs Screen Recording permission for the terminal.
//   swift scripts/screenshots.swift [--app dist/Procyon.app] [--out docs/screenshots]
//                                   [--pages overview processes …] [--appearances light dark]
//                                   [--wait 8] [--size 1280x800] [--width 1600]
// Writes <out>/<page>-<appearance>.png, scaled down to --width pixels (0 keeps the full Retina size,
// which is about 2 MB a screen). Run through `make screenshots`.
import AppKit
import CoreGraphics

let allPages = [
    "overview", "processes", "cpu", "memory", "gpu", "disk", "network", "energy", "battery", "startup", "services",
    "history", "inspect", "system", "settings",
]

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data("screenshots: \(message)\n".utf8))
    exit(1)
}

// MARK: Options

var appPath = "dist/Procyon.app"
var outDir = "docs/screenshots"
var pages = allPages
var appearances = ["light", "dark"]
var wait = 8.0
var size = (width: 1280.0, height: 800.0)
var width = 1600

var rest = CommandLine.arguments.dropFirst()
func values() -> [String] {
    var list: [String] = []
    while let next = rest.first, !next.hasPrefix("--") { list.append(rest.removeFirst()) }
    return list
}
while let option = rest.popFirst() {
    let list = values()
    switch (option, list.count) {
    case ("--app", 1): appPath = list[0]
    case ("--out", 1): outDir = list[0]
    case ("--pages", 1...): pages = list
    case ("--appearances", 1...): appearances = list
    case ("--wait", 1):
        guard let seconds = Double(list[0]) else { fail("--wait takes seconds") }
        wait = seconds
    case ("--width", 1):
        guard let pixels = Int(list[0]), pixels >= 0 else { fail("--width takes pixels, or 0 for full size") }
        width = pixels
    case ("--size", 1):
        let parts = list[0].split(separator: "x").compactMap { Double($0) }
        guard parts.count == 2 else { fail("--size takes WIDTHxHEIGHT, e.g. 1280x800") }
        size = (parts[0], parts[1])
    case ("--help", 0), ("-h", 0):
        print("usage: swift scripts/screenshots.swift [--app PATH] [--out DIR] [--pages PAGE…]")
        print("         [--appearances light dark] [--wait SECONDS] [--size WIDTHxHEIGHT] [--width PIXELS]")
        print("pages: \(allPages.joined(separator: " "))")
        exit(0)
    default: fail("bad option \(option) \(list.joined(separator: " ")) (see --help)")
    }
}
if let unknown = pages.first(where: { !allPages.contains($0) }) { fail("unknown page \(unknown)") }
if let unknown = appearances.first(where: { !["light", "dark"].contains($0) }) {
    fail("unknown appearance \(unknown) (light or dark)")
}
guard FileManager.default.fileExists(atPath: appPath) else { fail("\(appPath) not found; run `make app` first") }
if !CGPreflightScreenCaptureAccess() {
    CGRequestScreenCaptureAccess()
    fail("allow Screen Recording for your terminal in System Settings → Privacy & Security, then run again")
}
try FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)

// MARK: Capture

/// The id of `pid`'s main window once it is on screen.
func mainWindow(_ pid: pid_t) -> CGWindowID? {
    let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] ?? []
    let window = windows.first { window in
        guard window[kCGWindowOwnerPID as String] as? pid_t == pid, window[kCGWindowLayer as String] as? Int == 0,
            let bounds = window[kCGWindowBounds as String] as? [String: Double]
        else { return false }
        return (bounds["Width"] ?? 0) > 200 && (bounds["Height"] ?? 0) > 200
    }
    return window?[kCGWindowNumber as String] as? CGWindowID
}

func launch(page: String, appearance: String) -> NSRunningApplication {
    let configuration = NSWorkspace.OpenConfiguration()
    configuration.createsNewApplicationInstance = true
    configuration.activates = true
    configuration.addsToRecentItems = false
    // Argument-domain defaults: they override the user's settings for this launch only.
    configuration.arguments = [
        "-initialPage", page, "-appearance", appearance, "-refreshInterval", "1", "-fullAccessEnabled", "NO",
        "-ApplePersistenceIgnoreState", "YES", "-NSWindow Frame main", "80 80 \(size.width) \(size.height)",
    ]
    var opened: NSRunningApplication?
    var openError: Error?
    let done = DispatchSemaphore(value: 0)
    NSWorkspace.shared.openApplication(at: URL(fileURLWithPath: appPath), configuration: configuration) {
        opened = $0
        openError = $1
        done.signal()
    }
    done.wait()
    guard let app = opened else { fail("can't open \(appPath): \(String(describing: openError))") }
    return app
}

/// Runs a tool and returns its exit status.
func run(_ tool: String, _ arguments: [String]) throws -> Int32 {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: tool)
    process.arguments = arguments
    process.standardOutput = FileHandle.nullDevice
    try process.run()
    process.waitUntilExit()
    return process.terminationStatus
}

func quit(_ app: NSRunningApplication) {
    app.terminate()
    for _ in 0..<30 where !app.isTerminated { Thread.sleep(forTimeInterval: 0.1) }
    if !app.isTerminated { app.forceTerminate() }
    for _ in 0..<30 where !app.isTerminated { Thread.sleep(forTimeInterval: 0.1) }
}

var failures = 0
for appearance in appearances {
    for page in pages {
        let file = "\(outDir)/\(page)-\(appearance).png"
        let app = launch(page: page, appearance: appearance)
        var window: CGWindowID?
        for _ in 0..<200 {
            window = mainWindow(app.processIdentifier)
            if window != nil { break }
            Thread.sleep(forTimeInterval: 0.1)
        }
        guard let window else {
            print("✗ \(page) (\(appearance)): no window")
            failures += 1
            quit(app)
            continue
        }
        // Let the charts collect a few samples and the window finish appearing.
        Thread.sleep(forTimeInterval: wait)
        // Silent, no shadow, this window only.
        let status = try run("/usr/sbin/screencapture", ["-x", "-o", "-l", String(window), file])
        quit(app)
        guard status == 0 else {
            print("✗ \(page) (\(appearance)): screencapture exited with \(status)")
            failures += 1
            continue
        }
        if width > 0, try run("/usr/bin/sips", ["--resampleWidth", String(width), file]) != 0 {
            print("✗ \(page) (\(appearance)): can't resize \(file)")
            failures += 1
            continue
        }
        print("✓ \(file)")
    }
}
exit(failures == 0 ? 0 : 1)
