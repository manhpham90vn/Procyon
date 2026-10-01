import Foundation

/// Starts `procyon-helper` with administrator rights via the standard macOS password prompt.
///
/// Development fallback for builds without a Developer ID signature, which can't use `HelperDaemon`.
/// The helper lives only as long as this app: it exits when the app quits or disconnects.
enum HelperLauncher {
    enum Failure: Error, Equatable {
        case helperMissing
        case cancelled
        case launchFailed(String)

        var message: String {
            switch self {
            case .helperMissing: "The procyon-helper binary is missing from the app bundle."
            case .cancelled: "Administrator access was not granted."
            case .launchFailed(let detail): "Couldn't start the helper. \(detail)"
            }
        }
    }

    /// `Contents/Helpers/procyon-helper` in the .app; next to the executable for `swift run`.
    static var helperURL: URL? {
        let candidates = [
            Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/procyon-helper"),
            Bundle.main.executableURL?.deletingLastPathComponent().appendingPathComponent("procyon-helper"),
        ].compactMap { $0 }
        return candidates.first { FileManager.default.isExecutableFile(atPath: $0.path) }
    }

    /// Per-user private temp dir keeps the socket path short and out of shared /tmp.
    static func makeSocketPath() -> String {
        let directory = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("dev.procyon", isDirectory: true)
        try? FileManager.default.createDirectory(
            at: directory, withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700])
        return directory.appendingPathComponent("helper-\(getpid()).sock").path
    }

    /// Shows the system authorization prompt and starts the helper in the background.
    static func launch(socketPath: String) async throws(Failure) {
        guard let helper = helperURL else { throw .helperMissing }

        // Arguments go through `argv` + `quoted form of`, never string interpolation into the script.
        let script = """
            on run argv
                do shell script (quoted form of item 1 of argv) & " --socket " & (quoted form of item 2 of argv) & ¬
                    " --parent " & (item 3 of argv) & " --uid " & (item 4 of argv) & " > /dev/null 2>&1 &" ¬
                    with prompt "Procyon needs administrator access to show CPU, memory and disk usage of system processes and to end them." ¬
                    with administrator privileges
            end run
            """
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = ["-e", script, helper.path, socketPath, String(getpid()), String(getuid())]
        let errors = Pipe()
        process.standardError = errors
        process.standardOutput = FileHandle.nullDevice

        let status: Int32 = await withCheckedContinuation { continuation in
            process.terminationHandler = { continuation.resume(returning: $0.terminationStatus) }
            do {
                try process.run()
            } catch {
                process.terminationHandler = nil
                continuation.resume(returning: -1)
            }
        }
        guard status == 0 else {
            let detail = String(decoding: errors.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
            if detail.contains("-128") { throw .cancelled }  // user pressed Cancel
            throw .launchFailed(detail.trimmingCharacters(in: .whitespacesAndNewlines))
        }
    }
}
