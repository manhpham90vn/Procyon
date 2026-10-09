// The smallest SwiftUI app: one empty window. scripts/bench-probe.swift launches it just before
// Procyon and times its first window; that is the cost of launching any SwiftUI app on the machine
// (dyld, AppKit, SwiftUI, the window server) with none of Procyon's own work. On a shared CI runner
// it goes up with the host's load the same way Procyon's startup does, which a CPU-bound calibration
// does not catch, so bench-macos.py uses it to tell a slow runner from a slow Procyon.
//   swiftc -O -parse-as-library scripts/bench-reference.swift -o BenchReference.app/Contents/MacOS/BenchReference
import SwiftUI

@main
struct BenchReference: App {
    var body: some Scene {
        Window("Bench reference", id: "main") {
            Color.clear.frame(minWidth: 400, minHeight: 300)
        }
    }
}
