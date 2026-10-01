import Foundation
import ProcyonCore
import ServiceManagement

/// `procyon-helper --daemon` registered once with `SMAppService`: the user approves it in System Settings
/// and it stays available across launches, started by launchd whenever the app connects.
///
/// Only Developer ID builds embed its launchd plist (`scripts/build-macos-app.sh`); development builds
/// fall back to `HelperLauncher` and a password prompt per launch.
enum HelperDaemon {
    enum State: Equatable {
        case notRegistered
        case requiresApproval
        case enabled
    }

    static let socketPath = PC_HELPER_DAEMON_SOCKET
    private static let plistName = "\(PC_HELPER_DAEMON_LABEL).plist"
    private static var service: SMAppService { .daemon(plistName: plistName) }

    static var isAvailable: Bool {
        let plist = Bundle.main.bundleURL.appendingPathComponent("Contents/Library/LaunchDaemons/\(plistName)")
        return FileManager.default.fileExists(atPath: plist.path)
    }

    static var state: State {
        switch service.status {
        case .enabled: .enabled
        case .requiresApproval: .requiresApproval
        default: .notRegistered
        }
    }

    /// Registers the daemon if needed. The first time, macOS asks the user to allow it in System Settings.
    static func register() throws -> State {
        if state == .notRegistered {
            do {
                try service.register()
            } catch {
                // Registration that still awaits approval is reported as an error too.
                if state == .notRegistered { throw error }
            }
        }
        return state
    }

    static func unregister() async throws {
        try await service.unregister()
    }

    /// System Settings → General → Login Items & Extensions, where the user allows the helper.
    static func openApproval() {
        SMAppService.openSystemSettingsLoginItems()
    }
}
