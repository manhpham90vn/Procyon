import Foundation

public struct SystemInfo: Sendable, Hashable {
    public var osName = ""
    public var osVersion = ""
    public var osBuild = ""
    public var kernel = ""
    public var hostname = ""
    public var modelID = ""
    public var modelName = ""
    public var cpuBrand = ""
    public var architecture = ""
    public var physicalCores = 0
    public var logicalCores = 0
    public var performanceCores = 0
    public var efficiencyCores = 0
    public var cpuFrequencyHz: UInt64 = 0
    public var memoryTotal: UInt64 = 0
    public var bootTime = Date()

    public init() {}

    public var displayModel: String { modelName.isEmpty ? modelID : modelName }
    public var isLaptop: Bool { displayModel.localizedCaseInsensitiveContains("book") }
}

public struct Volume: Sendable, Hashable, Identifiable {
    public var id: String { mountPoint }
    public var name: String
    public var mountPoint: String
    public var fileSystem: String
    public var totalBytes: UInt64
    public var availableBytes: UInt64
    public var isRoot: Bool
    public var isRemovable: Bool

    public var usedBytes: UInt64 { totalBytes > availableBytes ? totalBytes - availableBytes : 0 }
    public var usedFraction: Double { totalBytes > 0 ? Double(usedBytes) / Double(totalBytes) : 0 }
}

/// Elevated access through the privileged helper.
public enum FullAccess: Sendable, Equatable {
    case off
    case starting
    /// The background helper is registered but waits for the user to allow it in System Settings.
    case needsApproval
    case on
    case failed(String)

    public var isOn: Bool { self == .on }
}

public enum MemoryPressure: Int, Sendable {
    case unknown = -1, normal = 0, warning = 1, critical = 2
}

public struct Capabilities: OptionSet, Sendable, Hashable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    public static let processDiskIO = Capabilities(rawValue: 1 << 0)
    public static let processNetwork = Capabilities(rawValue: 1 << 1)
    public static let memoryCompressed = Capabilities(rawValue: 1 << 2)
    public static let memoryPressure = Capabilities(rawValue: 1 << 3)
    public static let swap = Capabilities(rawValue: 1 << 4)
    public static let hybridCores = Capabilities(rawValue: 1 << 5)
}

/// Whole-machine metrics for one refresh.
public struct SystemSample: Sendable, Hashable {
    public var timestamp: TimeInterval = 0
    public var cpuUsage = 0.0
    public var cpuUser = 0.0
    public var cpuSystem = 0.0
    public var coreUsage: [Double] = []
    public var loadAverage: [Double] = [0, 0, 0]

    public var memoryTotal: UInt64 = 0
    public var memoryUsed: UInt64 = 0
    public var memoryApp: UInt64 = 0
    public var memoryWired: UInt64 = 0
    public var memoryCompressed: UInt64 = 0
    public var memoryCached: UInt64 = 0
    public var memoryFree: UInt64 = 0
    public var swapTotal: UInt64 = 0
    public var swapUsed: UInt64 = 0
    public var memoryPressure: MemoryPressure = .unknown

    public var diskReadRate = 0.0
    public var diskWriteRate = 0.0
    public var diskReadTotal: UInt64 = 0
    public var diskWriteTotal: UInt64 = 0
    public var networkReceiveRate = 0.0
    public var networkSendRate = 0.0
    public var networkReceiveTotal: UInt64 = 0
    public var networkSendTotal: UInt64 = 0

    public var processCount = 0
    public var threadCount = 0
    /// Processes whose metrics need administrator privileges (zero once full access is on).
    public var restrictedCount = 0

    public init() {}

    public var memoryFraction: Double { memoryTotal > 0 ? Double(memoryUsed) / Double(memoryTotal) : 0 }
    public var swapFraction: Double { swapTotal > 0 ? Double(swapUsed) / Double(swapTotal) : 0 }
}

public enum ViewMode: Int, CaseIterable, Sendable, Identifiable {
    case flat = 0, grouped = 1, tree = 2

    public var id: Int { rawValue }

    public var title: String {
        switch self {
        case .flat: "All Processes"
        case .grouped: "By App"
        case .tree: "Tree"
        }
    }

    public var symbol: String {
        switch self {
        case .flat: "list.bullet"
        case .grouped: "square.stack.3d.up"
        case .tree: "list.bullet.indent"
        }
    }

    /// Whether rows with children start expanded in this mode.
    public var expandsByDefault: Bool { self == .tree }
}

public enum ProcessColumn: Int, CaseIterable, Sendable {
    case name = 0, pid, user, cpu, memory, diskRead, diskWrite, networkReceive, networkSend, threads

    /// Natural first sort direction when a column header is clicked.
    public var prefersDescending: Bool {
        switch self {
        case .name, .user, .pid: false
        default: true
        }
    }
}

public struct ProcessFlags: OptionSet, Sendable, Hashable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    public static let system = ProcessFlags(rawValue: 1 << 0)
    public static let restricted = ProcessFlags(rawValue: 1 << 1)
    public static let protected = ProcessFlags(rawValue: 1 << 2)
    public static let appBundle = ProcessFlags(rawValue: 1 << 3)
}

/// One display row: a process or an application group.
public struct ProcessRow: Identifiable, Sendable, Hashable {
    public enum Kind: Sendable, Hashable { case process, group }

    public let id: String
    public let kind: Kind
    public let pid: Int32
    public let parentID: String?
    public let depth: Int
    public let childCount: Int
    public let processCount: Int
    public let name: String
    public let user: String
    public let path: String
    public let appID: String
    public let appName: String
    public let flags: ProcessFlags
    public let cpu: Double?
    public let memory: Int64?
    public let diskRead: Double?
    public let diskWrite: Double?
    public let networkReceive: Double?
    public let networkSend: Double?
    public let threads: Int?
    public let startTime: Date?
    /// Every pid this row stands for (a group lists all its members).
    public let memberPIDs: [Int32]

    public var hasChildren: Bool { childCount > 0 }
    public var isSystem: Bool { flags.contains(.system) }
    public var isRestricted: Bool { flags.contains(.restricted) }
    public var isProtected: Bool { flags.contains(.protected) }
    /// Bundle path used for the icon when the row belongs to an app.
    public var bundlePath: String? { flags.contains(.appBundle) || kind == .group && appID.hasSuffix(".app") ? appID : nil }

    // Non-optional keys so SwiftUI Table columns can declare sort comparators.
    public var sortName: String { name }
    public var sortPID: Int32 { pid }
    public var sortUser: String { user }
    public var sortCPU: Double { cpu ?? -1 }
    public var sortMemory: Int64 { memory ?? -1 }
    public var sortDiskRead: Double { diskRead ?? -1 }
    public var sortDiskWrite: Double { diskWrite ?? -1 }
    public var sortNetworkReceive: Double { networkReceive ?? -1 }
    public var sortNetworkSend: Double { networkSend ?? -1 }
    public var sortThreads: Int { threads ?? -1 }
    /// Download plus upload, for ranking by overall network activity.
    public var networkTotal: Double { (networkReceive ?? 0) + (networkSend ?? 0) }
    /// Read plus write, for ranking by overall disk activity.
    public var diskTotal: Double { (diskRead ?? 0) + (diskWrite ?? 0) }

    public init(
        id: String, kind: Kind, pid: Int32, parentID: String?, depth: Int, childCount: Int, processCount: Int,
        name: String, user: String, path: String, appID: String, appName: String, flags: ProcessFlags,
        cpu: Double?, memory: Int64?, diskRead: Double?, diskWrite: Double?, networkReceive: Double?,
        networkSend: Double?, threads: Int?, startTime: Date?, memberPIDs: [Int32]
    ) {
        self.id = id
        self.kind = kind
        self.pid = pid
        self.parentID = parentID
        self.depth = depth
        self.childCount = childCount
        self.processCount = processCount
        self.name = name
        self.user = user
        self.path = path
        self.appID = appID
        self.appName = appName
        self.flags = flags
        self.cpu = cpu
        self.memory = memory
        self.diskRead = diskRead
        self.diskWrite = diskWrite
        self.networkReceive = networkReceive
        self.networkSend = networkSend
        self.threads = threads
        self.startTime = startTime
        self.memberPIDs = memberPIDs
    }
}

extension ProcessRow {
    /// The same row moved to another place in the hierarchy.
    func moved(depth: Int, parentID: String?, childCount: Int? = nil) -> ProcessRow {
        ProcessRow(
            id: id, kind: kind, pid: pid, parentID: parentID, depth: depth, childCount: childCount ?? self.childCount,
            processCount: processCount, name: name, user: user, path: path, appID: appID, appName: appName,
            flags: flags, cpu: cpu, memory: memory, diskRead: diskRead, diskWrite: diskWrite,
            networkReceive: networkReceive, networkSend: networkSend, threads: threads, startTime: startTime,
            memberPIDs: memberPIDs)
    }
}

public extension Array where Element == ProcessRow {
    /// Splits pre-ordered rows into the app `appID` and everything else, both still in pre-order.
    /// Pinned: every row of the app with its subtree (the group and its members, the app's processes in
    /// the flat list, each of its subtrees in the tree), re-rooted at depth 0. Parents left behind
    /// lose those children.
    func pinning(appID: String) -> (pinned: [ProcessRow], rest: [ProcessRow]) {
        var pinned: [ProcessRow] = []
        var rest: [ProcessRow] = []
        rest.reserveCapacity(count)
        var movedChildren: [String: Int] = [:]
        var subtreeDepth: Int?
        for row in self {
            if let depth = subtreeDepth, row.depth > depth {
                pinned.append(row.moved(depth: row.depth - depth, parentID: row.parentID))
                continue
            }
            subtreeDepth = nil
            if row.appID == appID {
                subtreeDepth = row.depth
                pinned.append(row.moved(depth: 0, parentID: nil))
                if let parentID = row.parentID { movedChildren[parentID, default: 0] += 1 }
            } else {
                rest.append(row)
            }
        }
        if !movedChildren.isEmpty {
            rest = rest.map { row in
                guard let moved = movedChildren[row.id] else { return row }
                return row.moved(depth: row.depth, parentID: row.parentID, childCount: row.childCount - moved)
            }
        }
        return (pinned, rest)
    }

    /// Rows whose ancestors are all expanded. Rows must be in pre-order (as the core returns them).
    func visible(isExpanded: (ProcessRow) -> Bool) -> [ProcessRow] {
        var result: [ProcessRow] = []
        result.reserveCapacity(count)
        var collapsedDepth: Int?
        for row in self {
            if let depth = collapsedDepth {
                if row.depth > depth { continue }
                collapsedDepth = nil
            }
            result.append(row)
            if row.hasChildren && !isExpanded(row) { collapsedDepth = row.depth }
        }
        return result
    }
}

public enum EndResult: Sendable, Equatable {
    case ok, notFound, permissionDenied, protected, failed

    public var message: String {
        switch self {
        case .ok: "Done"
        case .notFound: "The process no longer exists."
        case .permissionDenied: "You don't have permission to end this process. System processes need administrator privileges."
        case .protected: "This process is critical to the system and can't be ended."
        case .failed: "The process could not be ended."
        }
    }
}
