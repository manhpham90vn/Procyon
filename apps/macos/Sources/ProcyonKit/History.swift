import Foundation
import SQLite3

/// Machine-wide figures for one minute: averages, plus the CPU peak.
public struct MachineMinute: Sendable, Hashable, Identifiable {
    /// Unix time of the minute's start.
    public var minute: Int
    public var cpu = 0.0
    public var cpuPeak = 0.0
    /// Fraction of physical memory in use.
    public var memory = 0.0
    public var diskRead = 0.0
    public var diskWrite = 0.0
    public var networkReceive = 0.0
    public var networkSend = 0.0
    public var gpu: Double?
    public var cpuTemperature: Double?
    public var appPower: Double?

    public var id: Int { minute }
    public var date: Date { Date(timeIntervalSince1970: TimeInterval(minute)) }

    public init(minute: Int) { self.minute = minute }
}

/// One app's average use over a minute, or over a range of minutes when summed by the database.
public struct AppUsage: Sendable, Hashable, Identifiable {
    public var appID: String
    public var name: String
    /// Percent of one core.
    public var cpu = 0.0
    public var memory = 0.0
    public var disk = 0.0
    public var network = 0.0
    public var gpu = 0.0
    public var power = 0.0

    public var id: String { appID }
    /// The app bundle for its icon, when it is one.
    public var bundlePath: String? { appID.hasSuffix(".app") ? appID : nil }

    public init(appID: String, name: String) {
        self.appID = appID
        self.name = name
    }
}

/// What one minute adds to the history.
public struct MinuteRecord: Sendable, Hashable {
    public var machine: MachineMinute
    public var apps: [AppUsage]
}

/// Averages the samples of the current minute; hands out a record when the minute is over.
public struct HistoryRecorder: Sendable {
    private var minute: Int?
    private var machine = MachineMinute(minute: 0)
    private var samples = 0
    private var gpuSamples = 0
    private var temperatureSamples = 0
    private var powerSamples = 0
    private var apps: [String: AppUsage] = [:]
    private var appSamples = 0

    public init() {}

    /// `busiest`: the top apps of this sample by each metric (grouped rows, possibly overlapping).
    public mutating func add(_ sample: SystemSample, busiest: [ProcessRow]) -> MinuteRecord? {
        let current = Int(sample.timestamp) / 60 * 60
        var finished: MinuteRecord?
        if let minute, minute != current { finished = record() }
        if minute != current { reset(current) }

        samples += 1
        machine.cpu += sample.cpuUsage
        machine.cpuPeak = max(machine.cpuPeak, sample.cpuUsage)
        machine.memory += sample.memoryFraction
        machine.diskRead += sample.diskReadRate
        machine.diskWrite += sample.diskWriteRate
        machine.networkReceive += sample.networkReceiveRate
        machine.networkSend += sample.networkSendRate
        if let gpu = sample.gpuUsage {
            machine.gpu = (machine.gpu ?? 0) + gpu
            gpuSamples += 1
        }
        if let temperature = sample.cpuTemperature {
            machine.cpuTemperature = (machine.cpuTemperature ?? 0) + temperature
            temperatureSamples += 1
        }
        if let power = sample.appPower {
            machine.appPower = (machine.appPower ?? 0) + power
            powerSamples += 1
        }

        guard !busiest.isEmpty else { return finished }
        appSamples += 1
        var seen = Set<String>()
        for row in busiest where row.depth == 0 && seen.insert(row.appID).inserted {
            var usage = apps[row.appID] ?? AppUsage(appID: row.appID, name: row.name)
            usage.cpu += row.cpu ?? 0
            usage.memory += Double(row.memory ?? 0)
            usage.disk += row.diskTotal
            usage.network += row.networkTotal
            usage.gpu += row.gpu ?? 0
            usage.power += row.power ?? 0
            apps[row.appID] = usage
        }
        return finished
    }

    private mutating func reset(_ minute: Int) {
        self.minute = minute
        machine = MachineMinute(minute: minute)
        samples = 0
        gpuSamples = 0
        temperatureSamples = 0
        powerSamples = 0
        apps = [:]
        appSamples = 0
    }

    private func record() -> MinuteRecord? {
        guard samples > 0 else { return nil }
        var m = machine
        let n = Double(samples)
        m.cpu /= n
        m.memory /= n
        m.diskRead /= n
        m.diskWrite /= n
        m.networkReceive /= n
        m.networkSend /= n
        m.gpu = m.gpu.map { $0 / Double(max(gpuSamples, 1)) }
        m.cpuTemperature = m.cpuTemperature.map { $0 / Double(max(temperatureSamples, 1)) }
        m.appPower = m.appPower.map { $0 / Double(max(powerSamples, 1)) }
        // An app missing from a sample's top lists used little then: count it as zero.
        let a = Double(max(appSamples, 1))
        let usage = apps.values.map { app in
            var app = app
            app.cpu /= a
            app.memory /= a
            app.disk /= a
            app.network /= a
            app.gpu /= a
            app.power /= a
            return app
        }
        return MinuteRecord(machine: m, apps: usage)
    }
}

/// The history on disk: one SQLite database, one row per minute and per busy app per minute.
public actor HistoryDatabase {
    /// How far back the history goes; older minutes are deleted as new ones arrive.
    public static let retention: TimeInterval = 24 * 3600

    public static var defaultURL: URL {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return support.appendingPathComponent("Procyon", isDirectory: true).appendingPathComponent("history.sqlite")
    }

    private let url: URL
    nonisolated(unsafe) private var db: OpaquePointer?
    private var writes = 0

    public init(url: URL = HistoryDatabase.defaultURL) {
        self.url = url
    }

    deinit { sqlite3_close(db) }

    private func open() -> OpaquePointer? {
        if let db { return db }
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        var handle: OpaquePointer?
        guard sqlite3_open(url.path, &handle) == SQLITE_OK else {
            sqlite3_close(handle)
            return nil
        }
        db = handle
        execute(
            """
            PRAGMA auto_vacuum = INCREMENTAL;
            PRAGMA journal_mode = WAL;
            CREATE TABLE IF NOT EXISTS machine (
                minute INTEGER PRIMARY KEY, cpu REAL, cpu_peak REAL, memory REAL, disk_read REAL, disk_write REAL,
                net_rx REAL, net_tx REAL, gpu REAL, cpu_temp REAL, power REAL);
            CREATE TABLE IF NOT EXISTS apps (
                minute INTEGER, app_id TEXT, name TEXT, cpu REAL, memory REAL, disk REAL, network REAL, gpu REAL,
                power REAL, PRIMARY KEY (minute, app_id)) WITHOUT ROWID;
            """)
        return handle
    }

    @discardableResult
    private func execute(_ sql: String) -> Bool {
        sqlite3_exec(db, sql, nil, nil, nil) == SQLITE_OK
    }

    private func prepare(_ sql: String) -> OpaquePointer? {
        guard let db = open() else { return nil }
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(db, sql, -1, &statement, nil) == SQLITE_OK else { return nil }
        return statement
    }

    public func write(_ record: MinuteRecord) {
        guard open() != nil else { return }
        execute("BEGIN")
        let m = record.machine
        if let s = prepare("INSERT OR REPLACE INTO machine VALUES (?,?,?,?,?,?,?,?,?,?,?)") {
            sqlite3_bind_int64(s, 1, Int64(m.minute))
            for (index, value) in [
                m.cpu, m.cpuPeak, m.memory, m.diskRead, m.diskWrite, m.networkReceive, m.networkSend,
            ].enumerated() {
                sqlite3_bind_double(s, Int32(index + 2), value)
            }
            for (index, value) in [m.gpu, m.cpuTemperature, m.appPower].enumerated() {
                if let value { sqlite3_bind_double(s, Int32(index + 9), value) } else { sqlite3_bind_null(s, Int32(index + 9)) }
            }
            sqlite3_step(s)
            sqlite3_finalize(s)
        }
        if let s = prepare("INSERT OR REPLACE INTO apps VALUES (?,?,?,?,?,?,?,?,?)") {
            for app in record.apps {
                sqlite3_reset(s)
                sqlite3_bind_int64(s, 1, Int64(m.minute))
                bind(s, 2, app.appID)
                bind(s, 3, app.name)
                for (index, value) in [app.cpu, app.memory, app.disk, app.network, app.gpu, app.power].enumerated() {
                    sqlite3_bind_double(s, Int32(index + 4), value)
                }
                sqlite3_step(s)
            }
            sqlite3_finalize(s)
        }
        let cutoff = m.minute - Int(Self.retention)
        execute("DELETE FROM machine WHERE minute < \(cutoff); DELETE FROM apps WHERE minute < \(cutoff);")
        execute("COMMIT")
        writes += 1
        // Give space freed by deleted minutes back now and then.
        if writes % 360 == 0 { execute("PRAGMA incremental_vacuum; PRAGMA wal_checkpoint(TRUNCATE);") }
    }

    /// Minutes from `start` (unix seconds) on, oldest first.
    public func machine(since start: Int) -> [MachineMinute] {
        guard let s = prepare("SELECT * FROM machine WHERE minute >= ? ORDER BY minute") else { return [] }
        defer { sqlite3_finalize(s) }
        sqlite3_bind_int64(s, 1, Int64(start))
        var result: [MachineMinute] = []
        while sqlite3_step(s) == SQLITE_ROW {
            var m = MachineMinute(minute: Int(sqlite3_column_int64(s, 0)))
            m.cpu = sqlite3_column_double(s, 1)
            m.cpuPeak = sqlite3_column_double(s, 2)
            m.memory = sqlite3_column_double(s, 3)
            m.diskRead = sqlite3_column_double(s, 4)
            m.diskWrite = sqlite3_column_double(s, 5)
            m.networkReceive = sqlite3_column_double(s, 6)
            m.networkSend = sqlite3_column_double(s, 7)
            m.gpu = optional(s, 8)
            m.cpuTemperature = optional(s, 9)
            m.appPower = optional(s, 10)
            result.append(m)
        }
        return result
    }

    /// Each app's average over the recorded minutes in `start ..< end` (a minute it wasn't busy
    /// counts as zero), biggest first by `order`.
    public func apps(from start: Int, to end: Int, orderBy order: AppUsageOrder, limit: Int = 10) -> [AppUsage] {
        var minutes = 1
        if let count = prepare("SELECT COUNT(*) FROM machine WHERE minute >= ? AND minute < ?") {
            sqlite3_bind_int64(count, 1, Int64(start))
            sqlite3_bind_int64(count, 2, Int64(end))
            if sqlite3_step(count) == SQLITE_ROW { minutes = max(Int(sqlite3_column_int64(count, 0)), 1) }
            sqlite3_finalize(count)
        }
        let sql = """
            SELECT app_id, MAX(name), SUM(cpu), SUM(memory), SUM(disk), SUM(network), SUM(gpu), SUM(power)
            FROM apps WHERE minute >= ? AND minute < ? GROUP BY app_id ORDER BY SUM(\(order.rawValue)) DESC LIMIT ?
            """
        guard let s = prepare(sql) else { return [] }
        defer { sqlite3_finalize(s) }
        sqlite3_bind_int64(s, 1, Int64(start))
        sqlite3_bind_int64(s, 2, Int64(end))
        sqlite3_bind_int64(s, 3, Int64(limit))
        var result: [AppUsage] = []
        let n = Double(minutes)
        while sqlite3_step(s) == SQLITE_ROW {
            var app = AppUsage(appID: text(s, 0), name: text(s, 1))
            app.cpu = sqlite3_column_double(s, 2) / n
            app.memory = sqlite3_column_double(s, 3) / n
            app.disk = sqlite3_column_double(s, 4) / n
            app.network = sqlite3_column_double(s, 5) / n
            app.gpu = sqlite3_column_double(s, 6) / n
            app.power = sqlite3_column_double(s, 7) / n
            result.append(app)
        }
        return result
    }

    /// Bytes the history takes on disk.
    public func size() -> Int64 {
        let paths = [url.path, url.path + "-wal", url.path + "-shm"]
        return paths.reduce(0) { total, path in
            total + ((try? FileManager.default.attributesOfItem(atPath: path)[.size] as? Int64) ?? 0)
        }
    }

    public func clear() {
        guard open() != nil else { return }
        execute("DELETE FROM machine; DELETE FROM apps; VACUUM;")
    }

    private func bind(_ s: OpaquePointer?, _ index: Int32, _ text: String) {
        // SQLITE_TRANSIENT: SQLite copies the string.
        sqlite3_bind_text(s, index, text, -1, unsafeBitCast(-1, to: sqlite3_destructor_type.self))
    }

    private func text(_ s: OpaquePointer?, _ column: Int32) -> String {
        sqlite3_column_text(s, column).map { String(cString: $0) } ?? ""
    }

    private func optional(_ s: OpaquePointer?, _ column: Int32) -> Double? {
        sqlite3_column_type(s, column) == SQLITE_NULL ? nil : sqlite3_column_double(s, column)
    }
}

public enum AppUsageOrder: String, Sendable {
    case cpu, memory, disk, network, gpu, power
}
