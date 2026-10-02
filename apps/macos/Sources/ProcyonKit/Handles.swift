import Foundation

/// What a scan of open files or sockets found; incomplete when some processes couldn't be read
/// (other users' processes without full access).
public struct HandleList<Item: Sendable & Hashable>: Sendable, Hashable {
    public var items: [Item]
    public var isComplete: Bool

    public init(items: [Item], isComplete: Bool) {
        self.items = items
        self.isComplete = isComplete
    }
}

/// A file or directory a process holds open (or works in).
public struct OpenFile: Sendable, Hashable, Identifiable {
    public enum Kind: Int32, Sendable, Hashable {
        case file = 0, directory, workingDirectory, other

        public var title: String {
            switch self {
            case .file: "File"
            case .directory: "Folder"
            case .workingDirectory: "Working folder"
            case .other: "Device"
            }
        }
    }

    public var id: String { "\(pid):\(descriptor):\(path)" }
    public let pid: Int32
    /// -1 for the working directory.
    public let descriptor: Int32
    public let kind: Kind
    public let path: String

    public init(pid: Int32, descriptor: Int32, kind: Kind, path: String) {
        self.pid = pid
        self.descriptor = descriptor
        self.kind = kind
        self.path = path
    }

    public var name: String { (path as NSString).lastPathComponent }

    /// Whether this handle is `path` itself or something inside it (a folder or volume).
    public func isWithin(_ path: String) -> Bool {
        let prefix = path.hasSuffix("/") ? path : path + "/"
        return self.path == path || self.path.hasPrefix(prefix)
    }
}

/// A TCP or UDP socket of a process.
public struct NetworkConnection: Sendable, Hashable, Identifiable {
    public enum Transport: String, Sendable, Hashable { case tcp = "TCP", udp = "UDP" }

    public enum State: Int32, Sendable, Hashable {
        case none = 0, listen, synSent, synReceived, established, closeWait, closing, timeWait, closed

        public var title: String {
            switch self {
            case .none: ""
            case .listen: "Listening"
            case .synSent: "Connecting"
            case .synReceived: "Accepting"
            case .established: "Connected"
            case .closeWait: "Closing (peer)"
            case .closing: "Closing"
            case .timeWait: "Time wait"
            case .closed: "Closed"
            }
        }
    }

    public var id: String {
        "\(pid)-\(transport.rawValue)-\(localAddress):\(localPort)-\(remoteAddress):\(remotePort)-\(state.rawValue)"
    }
    public let pid: Int32
    public let transport: Transport
    /// 4 or 6.
    public let ipVersion: Int
    public let state: State
    /// "*" when bound to every address.
    public let localAddress: String
    public let localPort: Int
    /// Empty when not connected.
    public let remoteAddress: String
    public let remotePort: Int

    public init(
        pid: Int32, transport: Transport, ipVersion: Int, state: State, localAddress: String, localPort: Int,
        remoteAddress: String, remotePort: Int
    ) {
        self.pid = pid
        self.transport = transport
        self.ipVersion = ipVersion
        self.state = state
        self.localAddress = localAddress
        self.localPort = localPort
        self.remoteAddress = remoteAddress
        self.remotePort = remotePort
    }

    /// Accepts connections (TCP listening, or UDP bound without a peer).
    public var isListening: Bool { state == .listen || transport == .udp && remoteAddress.isEmpty }
    /// Reachable from other machines: bound to every address or a non-loopback one.
    public var isExposed: Bool {
        isListening && !Self.isLoopback(localAddress)
    }
    public var isLoopbackOnly: Bool { Self.isLoopback(localAddress) && (remoteAddress.isEmpty || Self.isLoopback(remoteAddress)) }

    public var localEndpoint: String { Self.endpoint(localAddress, localPort) }
    public var remoteEndpoint: String { remoteAddress.isEmpty ? "" : Self.endpoint(remoteAddress, remotePort) }

    static func isLoopback(_ address: String) -> Bool {
        address == "::1" || address.hasPrefix("127.") || address == "localhost"
    }

    /// "[::1]:443" for IPv6, "1.2.3.4:443" otherwise.
    static func endpoint(_ address: String, _ port: Int) -> String {
        address.contains(":") ? "[\(address)]:\(port)" : "\(address):\(port)"
    }
}
