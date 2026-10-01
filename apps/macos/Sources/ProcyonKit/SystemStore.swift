import Foundation
import Observation

/// Owns the core monitor; all calls into C happen on this actor, off the main thread.
actor MonitorWorker {
    private let monitor = Monitor()

    struct Tick: Sendable {
        var sample: SystemSample
        var rows: [ProcessRow]
        var topCPU: [ProcessRow]
        var topMemory: [ProcessRow]
        var helperConnected: Bool
    }

    func tick(query: Monitor.Query) -> Tick {
        let sample = monitor.refresh()
        return Tick(
            sample: sample, rows: monitor.buildView(query), topCPU: top(.cpu), topMemory: top(.memory),
            helperConnected: monitor.helperConnected)
    }

    /// The helper needs a moment to create its socket after the password prompt closes.
    func attachHelper(socketPath: String) async -> Bool {
        for _ in 0..<50 {
            if monitor.attachHelper(socketPath: socketPath) { return true }
            try? await Task.sleep(for: .milliseconds(100))
        }
        return false
    }

    func detachHelper() { monitor.detachHelper() }

    func rows(query: Monitor.Query) -> [ProcessRow] {
        monitor.buildView(query)
    }

    func volumes() -> [Volume] { monitor.volumes() }

    func end(pids: [Int32], force: Bool) -> EndResult {
        var outcome = EndResult.ok
        for pid in pids {
            let result = monitor.end(pid: pid, force: force)
            if result != .ok, result != .notFound { outcome = result }
        }
        return outcome
    }

    func endTree(pid: Int32) -> EndResult { monitor.endTree(pid: pid) }

    private func top(_ column: ProcessColumn) -> [ProcessRow] {
        monitor.buildView(.init(mode: .grouped, column: column, descending: true, filter: "", limit: 6))
            .filter { $0.depth == 0 }
    }
}

/// Observable app state shared by every screen.
@MainActor
@Observable
public final class SystemStore {
    public static let refreshIntervals: [Double] = [0.5, 1, 2, 5]

    public let info: SystemInfo
    public let capabilities: Capabilities

    public private(set) var sample = SystemSample()
    public private(set) var history = HistoryBank()
    public private(set) var rows: [ProcessRow] = []
    public private(set) var topCPU: [ProcessRow] = []
    public private(set) var topMemory: [ProcessRow] = []
    public private(set) var volumes: [Volume] = []
    public private(set) var hasSample = false
    /// Whether restricted (system-owned) processes are read through the privileged helper.
    public private(set) var fullAccess: FullAccess = .off

    public var viewMode: ViewMode {
        didSet { if viewMode != oldValue { defaults.set(viewMode.rawValue, forKey: Keys.viewMode); rebuild() } }
    }
    public var sortColumn: ProcessColumn {
        didSet { if sortColumn != oldValue { rebuild() } }
    }
    public var sortDescending: Bool {
        didSet { if sortDescending != oldValue { rebuild() } }
    }
    public var filter = "" {
        didSet { if filter != oldValue { rebuild() } }
    }
    public var interval: Double {
        didSet {
            interval = min(max(interval, 0.5), 5)
            defaults.set(interval, forKey: Keys.interval)
            if interval != oldValue { restart() }
        }
    }
    public var isPaused = false {
        didSet { if isPaused != oldValue { restart() } }
    }

    private let worker = MonitorWorker()
    private let defaults: UserDefaults
    private var loop: Task<Void, Never>?
    private var rebuildTask: Task<Void, Never>?
    private var ticks = 0

    private enum Keys {
        static let interval = "refreshInterval"
        static let viewMode = "processViewMode"
    }

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        info = Monitor.systemInfo()
        capabilities = Monitor.capabilities
        let storedInterval = defaults.double(forKey: Keys.interval)
        interval = storedInterval > 0 ? storedInterval : 1
        viewMode = (defaults.object(forKey: Keys.viewMode) as? Int).flatMap(ViewMode.init(rawValue:)) ?? .grouped
        sortColumn = .cpu
        sortDescending = true
    }

    public var uptime: TimeInterval { Date().timeIntervalSince(info.bootTime) }

    public func start() {
        guard loop == nil else { return }
        restart()
    }

    public func stop() {
        loop?.cancel()
        loop = nil
    }

    /// Re-sorts or re-filters the current snapshot without sampling again.
    public func sort(by column: ProcessColumn, descending: Bool) {
        guard column != sortColumn || descending != sortDescending else { return }
        sortColumn = column
        sortDescending = descending
    }

    /// Asks for an administrator password and starts the privileged helper.
    public func enableFullAccess() async {
        guard fullAccess != .starting, !fullAccess.isOn else { return }
        fullAccess = .starting
        let socketPath = HelperLauncher.makeSocketPath()
        do {
            try await HelperLauncher.launch(socketPath: socketPath)
        } catch {
            fullAccess = error == .cancelled ? .off : .failed(error.message)
            return
        }
        guard await worker.attachHelper(socketPath: socketPath) else {
            fullAccess = .failed("The helper started but didn't answer.")
            return
        }
        fullAccess = .on
        await refreshNow()
    }

    /// Disconnects; the helper exits on its own.
    public func disableFullAccess() {
        guard fullAccess.isOn else { fullAccess = .off; return }
        fullAccess = .off
        Task {
            await worker.detachHelper()
            await refreshNow()
        }
    }

    public func refreshVolumes() {
        Task { volumes = await worker.volumes() }
    }

    public func end(_ row: ProcessRow, force: Bool) async -> EndResult {
        let result = await worker.end(pids: row.kind == .group ? row.memberPIDs : [row.pid], force: force)
        await refreshNow()
        return result
    }

    public func endTree(_ row: ProcessRow) async -> EndResult {
        let result: EndResult
        if row.kind == .group {
            result = await worker.end(pids: row.memberPIDs, force: true)
        } else {
            result = await worker.endTree(pid: row.pid)
        }
        await refreshNow()
        return result
    }

    // MARK: - Sampling

    private var query: Monitor.Query {
        .init(mode: viewMode, column: sortColumn, descending: sortDescending, filter: filter)
    }

    private func restart() {
        loop?.cancel()
        guard !isPaused else {
            loop = nil
            return
        }
        loop = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                await self.refreshNow()
                try? await Task.sleep(for: .seconds(self.interval))
            }
        }
    }

    private func refreshNow() async {
        let requested = query
        let tick = await worker.tick(query: requested)
        guard !Task.isCancelled || !hasSample else { return }
        sample = tick.sample
        history.record(tick.sample)
        rows = tick.rows
        // The filter or sort changed while sampling: rebuild against the new query.
        if requested != query { rebuild() }
        topCPU = tick.topCPU
        topMemory = tick.topMemory
        hasSample = true
        if fullAccess.isOn && !tick.helperConnected { fullAccess = .failed("The helper stopped responding.") }
        if ticks % 10 == 0 { volumes = await worker.volumes() }
        ticks += 1
    }

    private func rebuild() {
        rebuildTask?.cancel()
        let query = query
        rebuildTask = Task {
            let rows = await worker.rows(query: query)
            guard !Task.isCancelled else { return }
            self.rows = rows
        }
    }
}
