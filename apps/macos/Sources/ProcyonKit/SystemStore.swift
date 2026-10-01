import Foundation
import Observation

/// Owns the core monitor; all calls into C happen on this actor, off the main thread.
actor MonitorWorker {
    private let monitor = Monitor()
    private let hasProcessNetwork = Monitor.capabilities.contains(.processNetwork)

    struct Tick: Sendable {
        var sample: SystemSample
        var rows: [ProcessRow]
        var topCPU: [ProcessRow]
        var topMemory: [ProcessRow]
        var topNetwork: [ProcessRow]
        var helperConnected: Bool
    }

    func tick(query: Monitor.Query) -> Tick {
        let sample = monitor.refresh()
        return Tick(
            sample: sample, rows: monitor.buildView(query), topCPU: top(.cpu), topMemory: top(.memory),
            topNetwork: topNetwork(), helperConnected: monitor.helperConnected)
    }

    /// The helper needs a moment to create its socket after the password prompt closes, and launchd
    /// a moment to start the daemon.
    func attachHelper(socketPath: String, attempts: Int = 50) async -> Bool {
        for _ in 0..<attempts {
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

    /// Busiest apps by download plus upload; the core sorts by one direction, so merge both.
    private func topNetwork() -> [ProcessRow] {
        guard hasProcessNetwork else { return [] }
        var seen = Set<String>()
        let rows = (top(.networkReceive) + top(.networkSend)).filter { $0.networkTotal > 0 && seen.insert($0.id).inserted }
        return Array(rows.sorted { $0.networkTotal > $1.networkTotal }.prefix(6))
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
    /// Empty without `Capabilities.processNetwork` or while nothing uses the network.
    public private(set) var topNetwork: [ProcessRow] = []
    public private(set) var volumes: [Volume] = []
    public private(set) var hasSample = false
    /// Whether restricted (system-owned) processes are read through the privileged helper.
    public private(set) var fullAccess: FullAccess = .off
    /// Signed builds keep the helper as an approved background item; others ask for a password each launch.
    public let usesBackgroundHelper = HelperDaemon.isAvailable
    /// Whether the background helper is registered, so it can be removed from Settings.
    public private(set) var backgroundHelperRegistered = false

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
        static let fullAccess = "fullAccessEnabled"
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
        resumeFullAccess()
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

    /// Registers the background helper (signed builds) or asks for an administrator password and
    /// starts the helper for this session (development builds).
    public func enableFullAccess() async {
        guard fullAccess != .starting, !fullAccess.isOn else { return }
        if usesBackgroundHelper {
            await enableBackgroundHelper()
            return
        }
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

    /// Disconnects; the helper exits on its own. A registered background helper stays approved.
    public func disableFullAccess() {
        defaults.set(false, forKey: Keys.fullAccess)
        guard fullAccess.isOn else { fullAccess = .off; return }
        fullAccess = .off
        Task {
            await worker.detachHelper()
            await refreshNow()
        }
    }

    /// Unregisters the background helper; turning full access on again asks for approval again.
    public func removeBackgroundHelper() async {
        disableFullAccess()
        do {
            try await HelperDaemon.unregister()
        } catch {
            fullAccess = .failed("Couldn't remove the helper. \(error.localizedDescription)")
        }
        backgroundHelperRegistered = HelperDaemon.state != .notRegistered
    }

    /// Opens System Settings where the user allows the background helper.
    public func openHelperApproval() { HelperDaemon.openApproval() }

    private func enableBackgroundHelper() async {
        fullAccess = .starting
        defaults.set(true, forKey: Keys.fullAccess)
        let state: HelperDaemon.State
        do {
            state = try HelperDaemon.register()
        } catch {
            fullAccess = .failed("Couldn't register the helper. \(error.localizedDescription)")
            return
        }
        backgroundHelperRegistered = true
        switch state {
        case .enabled: await attachBackgroundHelper()
        case .requiresApproval:
            fullAccess = .needsApproval
            HelperDaemon.openApproval()
        case .notRegistered: fullAccess = .failed("The helper isn't registered.")
        }
    }

    private func attachBackgroundHelper() async {
        fullAccess = .starting
        guard await worker.attachHelper(socketPath: HelperDaemon.socketPath) else {
            fullAccess = .failed("The helper didn't answer.")
            return
        }
        fullAccess = .on
        await refreshNow()
    }

    /// Reconnects at launch when the user left full access on and the helper is still approved.
    private func resumeFullAccess() {
        guard usesBackgroundHelper else { return }
        let state = HelperDaemon.state
        backgroundHelperRegistered = state != .notRegistered
        guard defaults.bool(forKey: Keys.fullAccess) else { return }
        switch state {
        case .enabled: Task { await attachBackgroundHelper() }
        case .requiresApproval: fullAccess = .needsApproval
        case .notRegistered: break  // removed in System Settings
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
        topNetwork = tick.topNetwork
        hasSample = true
        if fullAccess.isOn && !tick.helperConnected { await reconnectHelper() }
        // The user may allow the helper in System Settings at any moment.
        if fullAccess == .needsApproval, ticks % 2 == 0, HelperDaemon.state == .enabled { await attachBackgroundHelper() }
        if ticks % 10 == 0 { volumes = await worker.volumes() }
        ticks += 1
    }

    /// launchd restarts the background helper on demand (after an app update, for instance).
    private func reconnectHelper() async {
        if usesBackgroundHelper, await worker.attachHelper(socketPath: HelperDaemon.socketPath, attempts: 10) { return }
        fullAccess = .failed("The helper stopped responding.")
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
