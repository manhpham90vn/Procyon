import Foundation

/// Scheduling priority as a Unix nice value, in the steps the menus offer.
public enum ProcessPriority: Int32, CaseIterable, Sendable, Identifiable {
    case high = -10, aboveNormal = -5, normal = 0, belowNormal = 5, low = 10, lowest = 20

    public var id: Int32 { rawValue }

    public var title: String {
        switch self {
        case .high: "High"
        case .aboveNormal: "Above Normal"
        case .normal: "Normal"
        case .belowNormal: "Below Normal"
        case .low: "Low"
        case .lowest: "Lowest"
        }
    }

    /// The step a nice value falls into.
    public init(nice: Int32) {
        self = Self.allCases.min { abs($0.rawValue - nice) < abs($1.rawValue - nice) } ?? .normal
    }

    /// Raising priority above normal needs administrator rights on Unix.
    public var needsPrivileges: Bool { rawValue < 0 }
}

/// POSIX signals offered by "Send Signal".
public enum ProcessSignal: Int32, CaseIterable, Sendable, Identifiable {
    case hangup = 1, interrupt = 2, quit = 3, kill = 9, user1 = 30, user2 = 31, terminate = 15, stop = 17, `continue` = 19

    public var id: Int32 { rawValue }

    public var name: String {
        switch self {
        case .hangup: "SIGHUP"
        case .interrupt: "SIGINT"
        case .quit: "SIGQUIT"
        case .kill: "SIGKILL"
        case .user1: "SIGUSR1"
        case .user2: "SIGUSR2"
        case .terminate: "SIGTERM"
        case .stop: "SIGSTOP"
        case .continue: "SIGCONT"
        }
    }

    public var meaning: String {
        switch self {
        case .hangup: "Hang up / reload configuration"
        case .interrupt: "Interrupt (like Control-C)"
        case .quit: "Quit and dump core"
        case .kill: "Kill immediately"
        case .user1: "User-defined 1"
        case .user2: "User-defined 2"
        case .terminate: "Ask to terminate"
        case .stop: "Suspend"
        case .continue: "Resume"
        }
    }

    /// Signals that end or freeze the process; the UI confirms them.
    public var isDisruptive: Bool { self != .user1 && self != .user2 && self != .continue && self != .hangup }
}

public struct ThreadInfo: Sendable, Hashable, Identifiable {
    public let id: UInt64
    public let name: String
    /// Percent of one core, recent.
    public let cpu: Double?
    public let userTime: TimeInterval
    public let systemTime: TimeInterval
    public let priority: Int32
    public let state: ProcessState
}

/// Everything "Get Info" shows about one process.
public struct ProcessDetails: Sendable, Hashable {
    public var pid: Int32 = 0
    public var parentPID: Int32 = 0
    public var name = ""
    public var user = ""
    public var path = ""
    public var workingDirectory = ""
    public var nice: Int32 = 0
    public var state: ProcessState = .unknown
    public var startTime: Date?
    /// nil when unreadable without full access.
    public var arguments: [String]?
    public var environment: [String]?
    public var threads: [ThreadInfo]?

    public init() {}

    /// The command line as a shell would show it, quoting arguments with spaces.
    public var commandLine: String? {
        arguments.map { args in
            args.map { $0.contains(" ") || $0.isEmpty ? "\"\($0)\"" : $0 }.joined(separator: " ")
        }
    }

    public var runningTime: TimeInterval? { startTime.map { Date().timeIntervalSince($0) } }
}

/// A process as other screens reference it (startup items, sleep assertions).
public struct ProcessSummary: Sendable, Hashable {
    public let pid: Int32
    public let name: String
    public let appName: String
    public let path: String
    public let bundlePath: String?
    public let memory: Int64?
    /// Percent of one core.
    public let cpu: Double?
}
