import Foundation

public enum ServiceDomain: Int32, Sendable, Hashable {
    /// Machine-wide; changing these needs full access.
    case system = 0
    /// The current user's session.
    case user = 1

    public var title: String { self == .system ? "System" : "User" }
}

public enum ServiceAction: Int32, Sendable {
    case start = 0, stop, restart, enable, disable

    public var title: String {
        switch self {
        case .start: "Start"
        case .stop: "Stop"
        case .restart: "Restart"
        case .enable: "Enable"
        case .disable: "Disable"
        }
    }
}

/// A background service (a launchd job on macOS).
public struct Service: Sendable, Hashable, Identifiable {
    public var id: String { "\(domain.rawValue):\(label)" }
    public let label: String
    public let name: String
    public let program: String
    public let configPath: String
    public let domain: ServiceDomain
    /// nil when not running.
    public let pid: Int32?
    public let lastExitStatus: Int32
    public let isEnabled: Bool
    /// Part of the OS.
    public let isApple: Bool

    public var isRunning: Bool { pid != nil }

    public var statusTitle: String {
        if let pid { return "Running · PID \(pid)" }
        if !isEnabled { return "Disabled" }
        return lastExitStatus != 0 ? "Stopped · exit \(lastExitStatus)" : "Stopped"
    }
}

public enum StartupScope: Int32, Sendable, Hashable {
    case userAgent = 0, globalAgent, daemon
    /// An app macOS opens at login (System Settings → Login Items → Open at Login).
    case openAtLogin
    /// A helper, agent, daemon or background task an app registered ("Allow in the Background").
    case appBackground

    public var title: String {
        switch self {
        case .userAgent: "At your login"
        case .globalAgent: "At every login"
        case .daemon: "At startup"
        case .openAtLogin: "Opens at login"
        case .appBackground: "In the background"
        }
    }

    /// Daemons run as root; changing them needs full access.
    public var domain: ServiceDomain { self == .daemon ? .system : .user }
}

/// An app or program that starts with the system or at login.
public struct StartupItem: Sendable, Hashable, Identifiable {
    public var id: String { "\(scope.rawValue):\(label)" }
    public let label: String
    public let name: String
    public let program: String
    public let configPath: String
    /// The app that installed it, for its icon; nil when none.
    public let appPath: String?
    public let scope: StartupScope
    public let pid: Int32?
    public let isEnabled: Bool
    public let runsAtLoad: Bool
    public let keepsAlive: Bool
    /// The app or developer it belongs to; empty when none.
    public var parentName = ""
    /// Only System Settings can switch it (items apps register with ServiceManagement).
    public var isManagedByOS = false

    /// The same item with the pid of its running copy.
    public func running(_ pid: Int32?) -> StartupItem {
        var item = StartupItem(
            label: label, name: name, program: program, configPath: configPath, appPath: appPath, scope: scope, pid: pid,
            isEnabled: isEnabled, runsAtLoad: runsAtLoad, keepsAlive: keepsAlive)
        item.parentName = parentName
        item.isManagedByOS = isManagedByOS
        return item
    }
}

/// How much a startup item costs right now, from its running instance.
public enum StartupImpact: Int, Sendable, Comparable {
    case notRunning = 0, low, medium, high

    public static func < (a: Self, b: Self) -> Bool { a.rawValue < b.rawValue }

    public var title: String {
        switch self {
        case .notRunning: "Not running"
        case .low: "Low"
        case .medium: "Medium"
        case .high: "High"
        }
    }

    /// From the running process's memory and CPU. Coarse on purpose: macOS doesn't measure what a
    /// login item cost at startup, so this reflects its weight now.
    public init(memory: Int64?, cpu: Double?, isRunning: Bool) {
        guard isRunning else {
            self = .notRunning
            return
        }
        let mb = Double(memory ?? 0) / 1_048_576
        let cpu = cpu ?? 0
        if mb >= 300 || cpu >= 20 {
            self = .high
        } else if mb >= 80 || cpu >= 5 {
            self = .medium
        } else {
            self = .low
        }
    }
}
