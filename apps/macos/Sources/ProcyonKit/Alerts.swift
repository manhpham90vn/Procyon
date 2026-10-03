import Foundation

/// A condition worth a notification: the machine or one app over a threshold for a while.
public struct AlertRule: Codable, Sendable, Hashable, Identifiable {
    public enum Kind: String, Codable, Sendable, CaseIterable {
        /// Whole-machine CPU, percent of every core.
        case cpu
        /// Memory in use, percent of physical memory.
        case memory
        /// The OS reports critical memory pressure (threshold unused).
        case memoryPressure
        /// Hottest CPU die sensor, Celsius.
        case temperature
        /// One app's CPU, percent of one core.
        case appCPU
        /// One app's memory, gigabytes.
        case appMemory

        public var title: String {
            switch self {
            case .cpu: "CPU usage"
            case .memory: "Memory usage"
            case .memoryPressure: "Memory pressure is critical"
            case .temperature: "CPU temperature"
            case .appCPU: "An app's CPU"
            case .appMemory: "An app's memory"
            }
        }

        /// Whether it watches individual apps (needs per-process sampling).
        public var isPerApp: Bool { self == .appCPU || self == .appMemory }

        public var defaultThreshold: Double {
            switch self {
            case .cpu: 90
            case .memory: 90
            case .memoryPressure: 0
            case .temperature: 95
            case .appCPU: 150
            case .appMemory: 8
            }
        }

        public var thresholdRange: ClosedRange<Double> {
            switch self {
            case .cpu, .memory: 50...100
            case .memoryPressure: 0...0
            case .temperature: 60...110
            case .appCPU: 50...800
            case .appMemory: 1...64
            }
        }

        public var step: Double {
            switch self {
            case .cpu, .memory, .temperature: 5
            case .memoryPressure: 1
            case .appCPU: 50
            case .appMemory: 1
            }
        }
    }

    public var kind: Kind
    public var isEnabled: Bool
    public var threshold: Double
    /// How long the condition must hold before notifying, seconds.
    public var duration: TimeInterval

    public var id: Kind { kind }

    public init(kind: Kind, isEnabled: Bool = false, threshold: Double? = nil, duration: TimeInterval = 60) {
        self.kind = kind
        self.isEnabled = isEnabled
        self.threshold = threshold ?? kind.defaultThreshold
        self.duration = duration
    }
}

/// Every rule, persisted as JSON in the user defaults.
public struct AlertSettings: Codable, Sendable, Hashable {
    public static let storageKey = "alerts"
    public static let durations: [TimeInterval] = [10, 30, 60, 300, 900]

    public var rules: [AlertRule]

    public init(rules: [AlertRule] = AlertRule.Kind.allCases.map { AlertRule(kind: $0) }) {
        self.rules = rules
    }

    public var isAnyEnabled: Bool { rules.contains(where: \.isEnabled) }
    public var watchesApps: Bool { rules.contains { $0.isEnabled && $0.kind.isPerApp } }

    public static func load(_ defaults: UserDefaults = .standard) -> AlertSettings {
        guard let data = defaults.data(forKey: storageKey),
            var settings = try? JSONDecoder().decode(AlertSettings.self, from: data)
        else { return AlertSettings() }
        // Kinds added after the settings were saved start off.
        for kind in AlertRule.Kind.allCases where !settings.rules.contains(where: { $0.kind == kind }) {
            settings.rules.append(AlertRule(kind: kind))
        }
        return settings
    }

    public func save(_ defaults: UserDefaults = .standard) {
        if let data = try? JSONEncoder().encode(self) { defaults.set(data, forKey: Self.storageKey) }
    }
}

/// One notification to show.
public struct AlertEvent: Sendable, Hashable, Identifiable {
    public let id = UUID()
    public let kind: AlertRule.Kind
    public let title: String
    public let message: String
    public let date: Date
    /// The app it is about, for per-app rules.
    public let appID: String?
}

/// Turns samples into alerts: a rule fires once its condition has held for its duration, then stays
/// quiet for `cooldown` (per app for per-app rules) so a busy machine doesn't flood the user.
public struct AlertEvaluator: Sendable {
    public var cooldown: TimeInterval = 15 * 60
    /// When each condition (rule, or rule + app) started holding.
    private var since: [String: TimeInterval] = [:]
    private var lastFired: [String: TimeInterval] = [:]

    public init() {}

    /// `apps`: the busiest apps of this sample (grouped rows).
    public mutating func evaluate(
        _ sample: SystemSample, apps: [ProcessRow], settings: AlertSettings, now: TimeInterval
    ) -> [AlertEvent] {
        var events: [AlertEvent] = []
        var holding = Set<String>()
        for rule in settings.rules where rule.isEnabled {
            for (key, value, appID, name) in candidates(rule, sample, apps) where exceeds(rule, value) {
                holding.insert(key)
                let start = since[key] ?? now
                since[key] = start
                guard now - start >= rule.duration, now - (lastFired[key] ?? -.infinity) >= cooldown else { continue }
                lastFired[key] = now
                events.append(event(rule, value: value, appID: appID, name: name, now: now))
            }
        }
        // A condition that stopped holding starts over.
        since = since.filter { holding.contains($0.key) }
        return events
    }

    /// Forgets how long every condition has held: after a pause, the time it was not watched must
    /// not count toward a rule's duration.
    public mutating func reset() { since = [:] }

    /// Forgets how long `kind`'s conditions have held (for every app, for a per-app rule): a changed
    /// threshold or duration starts the clock over instead of firing at once.
    public mutating func reset(_ kind: AlertRule.Kind) {
        let prefix = "\(kind.rawValue):"
        since = since.filter { $0.key != kind.rawValue && !$0.key.hasPrefix(prefix) }
    }

    private func exceeds(_ rule: AlertRule, _ value: Double) -> Bool {
        rule.kind == .memoryPressure ? value >= 1 : value >= rule.threshold
    }

    /// (key, value in the rule's unit, app id, app name) for each thing the rule watches.
    private func candidates(
        _ rule: AlertRule, _ s: SystemSample, _ apps: [ProcessRow]
    )
        -> [(String, Double, String?, String?)]
    {
        let kind = rule.kind.rawValue
        switch rule.kind {
        case .cpu: return [(kind, s.cpuUsage * 100, nil, nil)]
        case .memory: return s.memoryTotal > 0 ? [(kind, s.memoryFraction * 100, nil, nil)] : []
        case .memoryPressure: return [(kind, s.memoryPressure == .critical ? 1 : 0, nil, nil)]
        case .temperature: return s.cpuTemperature.map { [(kind, $0, nil, nil)] } ?? []
        case .appCPU:
            return apps.compactMap { row in row.cpu.map { ("\(kind):\(row.appID)", $0, row.appID, row.name) } }
        case .appMemory:
            return apps.compactMap { row in
                row.memory.map { ("\(kind):\(row.appID)", Double($0) / 1_073_741_824, row.appID, row.name) }
            }
        }
    }

    private func event(_ rule: AlertRule, value: Double, appID: String?, name: String?, now: TimeInterval) -> AlertEvent {
        let minutes = rule.duration >= 60 ? "\(Int(rule.duration / 60)) min" : "\(Int(rule.duration)) s"
        let (title, message): (String, String) =
            switch rule.kind {
            case .cpu: ("CPU is busy", "\(Int(value))% of the CPU has been in use for \(minutes).")
            case .memory: ("Memory is almost full", "\(Int(value))% of memory has been in use for \(minutes).")
            case .memoryPressure:
                ("Memory pressure is critical", "macOS is short of memory: apps may slow down. Quit apps you don't need.")
            case .temperature: ("Your Mac is hot", "The CPU has been at \(Int(value))°C for \(minutes).")
            case .appCPU:
                ("“\(name ?? "An app")” is using a lot of CPU", "\(Int(value))% CPU for \(minutes). It may be stuck.")
            case .appMemory:
                ("“\(name ?? "An app")” is using a lot of memory", String(format: "%.1f GB for %@.", value, minutes))
            }
        return AlertEvent(
            kind: rule.kind, title: title, message: message, date: Date(timeIntervalSince1970: now), appID: appID)
    }
}
