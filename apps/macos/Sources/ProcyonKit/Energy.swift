import Foundation

/// Energy one app used over a period, with its latest row for display.
public struct AppEnergy: Sendable, Hashable, Identifiable {
    public var id: String { row.appID }
    public private(set) var row: ProcessRow
    public private(set) var joules: Double = 0

    init(row: ProcessRow) { self.row = row }

    mutating func add(_ row: ProcessRow, joules: Double) {
        self.row = row
        self.joules += joules
    }

    public var wattHours: Double { joules / 3600 }
}
