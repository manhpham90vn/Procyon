import Foundation

public struct Battery: Sendable, Hashable {
    /// 0...1
    public var level = 0.0
    public var onACPower = false
    public var isCharging = false
    public var isFullyCharged = false
    public var timeToEmpty: TimeInterval?
    public var timeToFull: TimeInterval?
    public var cycleCount: Int?
    /// Maximum capacity relative to new, 0...1.
    public var health: Double?
    /// "Normal", "Service Recommended", … (empty when the OS doesn't say).
    public var condition = ""
    public var designCapacity: Int?
    public var maximumCapacity: Int?
    /// Celsius
    public var temperature: Double?
    /// Negative while discharging; nil when unknown.
    public var power: Double?
    public var adapter = ""

    public init() {}

    public var stateTitle: String {
        if isCharging { return "Charging" }
        if isFullyCharged { return "Fully charged" }
        return onACPower ? "On power adapter" : "On battery"
    }
}

/// A process keeping the Mac (or its display) awake.
public struct PowerAssertion: Sendable, Hashable, Identifiable {
    public var id: String { "\(pid)-\(type)-\(reason)-\(onBehalfOf ?? 0)" }
    public let pid: Int32
    /// The app the holder acts for (coreaudiod plays audio for a player, for instance).
    public let onBehalfOf: Int32?
    public let processName: String
    public let type: String
    public let reason: String
    public let preventsDisplaySleep: Bool
    public let created: Date?

    public var kindTitle: String { preventsDisplaySleep ? "Keeps display on" : "Prevents sleep" }
}
