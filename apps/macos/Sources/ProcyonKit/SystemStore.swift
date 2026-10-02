import Foundation
import Observation

/// Owns the core monitor; all calls into C happen on this actor, off the main thread.
actor MonitorWorker {
    private let monitor = Monitor()
    private let capabilities = Monitor.capabilities
    private var hasProcessNetwork: Bool { capabilities.contains(.processNetwork) }
    private var samplesProcesses = true

    struct Tick: Sendable {
        var sample: SystemSample
        var rows: [ProcessRow] = []
        var topCPU: [ProcessRow] = []
        var topMemory: [ProcessRow] = []
        var topDisk: [ProcessRow] = []
        var topNetwork: [ProcessRow] = []
        var topGPU: [ProcessRow] = []
        var battery: Battery?
        var helperConnected: Bool
    }

    /// `processes` false: only machine-wide metrics (no window open, the menu bar still updates).
    func tick(query: Monitor.Query, processes: Bool, battery: Bool) -> Tick {
        if processes != samplesProcesses {
            samplesProcesses = processes
            monitor.setProcessSampling(processes)
        }
        let sample = monitor.refresh()
        var tick = Tick(sample: sample, helperConnected: monitor.helperConnected)
        if battery && capabilities.contains(.battery) { tick.battery = Monitor.battery() }
        guard processes else { return tick }
        tick.rows = monitor.buildView(query)
        tick.topCPU = top(.cpu)
        tick.topMemory = top(.memory)
        tick.topDisk = topDisk()
        tick.topNetwork = topNetwork()
        tick.topGPU = capabilities.contains(.processGPU) ? top(.gpu).filter { ($0.gpu ?? 0) > 0 } : []
        return tick
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

    func end(pids: [Int32], force: Bool) -> ActionResult {
        var outcome = ActionResult.ok
        for pid in pids {
            let result = monitor.end(pid: pid, force: force)
            if result != .ok, result != .notFound { outcome = result }
        }
        return outcome
    }

    func endTree(pid: Int32) -> ActionResult { monitor.endTree(pid: pid) }

    /// Applies `action` to every pid and reports the first real failure.
    func each(_ pids: [Int32], _ action: (Int32) -> ActionResult) -> ActionResult {
        var outcome = ActionResult.ok
        for pid in pids {
            let result = action(pid)
            if result.isFailure, !outcome.isFailure { outcome = result }
        }
        return outcome
    }

    func signal(pids: [Int32], _ signal: Int32) -> ActionResult { each(pids) { monitor.signal(pid: $0, signal) } }
    func suspend(pids: [Int32]) -> ActionResult { each(pids) { monitor.suspend(pid: $0) } }
    func resume(pids: [Int32]) -> ActionResult { each(pids) { monitor.resume(pid: $0) } }
    func setPriority(pids: [Int32], nice: Int32) -> ActionResult { each(pids) { monitor.setPriority(pid: $0, nice: nice) } }
    func details(pid: Int32) -> ProcessDetails? { monitor.details(pid: pid) }
    func summaries(for pids: Set<Int32>) -> [Int32: ProcessSummary] { monitor.summaries(for: pids) }
    func powerAssertions() -> [PowerAssertion] { monitor.powerAssertions() }
    func services() -> [Service] { monitor.services() }
    func control(_ service: Service, _ action: ServiceAction) -> ActionResult { monitor.control(service, action) }
    func startupItems() -> [StartupItem] { monitor.startupItems() }
    func pids(forApps paths: Set<String>) -> [String: Int32] { monitor.pids(forApps: paths) }
    func managedStartupItems() -> (items: [StartupItem], ready: Bool)? { monitor.managedStartupItems() }
    func setEnabled(_ item: StartupItem, _ enabled: Bool) -> ActionResult { monitor.setEnabled(item, enabled) }

    private func top(_ column: ProcessColumn) -> [ProcessRow] {
        monitor.buildView(.init(mode: .grouped, column: column, descending: true, filter: "", limit: SystemStore.topCount))
            .filter { $0.depth == 0 }
    }

    /// Busiest apps by read plus write; the core sorts by one direction, so merge both.
    private func topDisk() -> [ProcessRow] {
        merged(top(.diskRead) + top(.diskWrite), by: \.diskTotal)
    }

    /// Busiest apps by download plus upload.
    private func topNetwork() -> [ProcessRow] {
        guard hasProcessNetwork else { return [] }
        return merged(top(.networkReceive) + top(.networkSend), by: \.networkTotal)
    }

    private func merged(_ rows: [ProcessRow], by total: KeyPath<ProcessRow, Double>) -> [ProcessRow] {
        var seen = Set<String>()
        let active = rows.filter { $0[keyPath: total] > 0 && seen.insert($0.id).inserted }
        return Array(active.sorted { $0[keyPath: total] > $1[keyPath: total] }.prefix(SystemStore.topCount))
    }
}

/// Observable app state shared by every screen.
@MainActor
@Observable
public final class SystemStore {
    public static let refreshIntervals: [Double] = [0.5, 1, 2, 5]
    /// Length of the "top apps" lists.
    public nonisolated static let topCount = 10

    public let info: SystemInfo
    public let capabilities: Capabilities

    public private(set) var sample = SystemSample()
    public private(set) var history = HistoryBank()
    public private(set) var rows: [ProcessRow] = []
    public private(set) var topCPU: [ProcessRow] = []
    public private(set) var topMemory: [ProcessRow] = []
    /// Apps reading or writing the disk right now.
    public private(set) var topDisk: [ProcessRow] = []
    /// Empty without `Capabilities.processNetwork` or while nothing uses the network.
    public private(set) var topNetwork: [ProcessRow] = []
    /// Apps using the GPU; empty without `Capabilities.processGPU`.
    public private(set) var topGPU: [ProcessRow] = []
    /// nil on machines without a battery.
    public private(set) var battery: Battery?
    public private(set) var volumes: [Volume] = []
    public private(set) var hasSample = false
    /// A row the Processes screen should reveal and select once it shows (set by `showInProcesses`).
    public var focusedRow: ProcessRow?
    /// The app kept at the top of Processes, in every view, above the sorted rows.
    public var pinnedAppID: String?
    /// Whether restricted (system-owned) processes are read through the privileged helper.
    public private(set) var fullAccess: FullAccess = .off
    /// Signed builds keep the helper as an approved background item; others ask for a password each launch.
    public let usesBackgroundHelper = HelperDaemon.isAvailable
    /// Whether the background helper is registered, so it can be removed from Settings.
    public private(set) var backgroundHelperRegistered = false

    public var viewMode: ViewMode {
        didSet {
            guard viewMode != oldValue else { return }
            if persistsViewMode { defaults.set(viewMode.rawValue, forKey: Keys.viewMode) }
            rebuild()
        }
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
    /// Off while nothing on screen needs per-process data (only the menu bar label is showing):
    /// sampling then reads machine-wide metrics only, which is several times cheaper.
    public private(set) var samplesProcesses = true
    private var processConsumers: Set<String> = []

    /// Registers whether a window or panel that shows processes is open.
    public func needsProcesses(_ consumer: String, _ active: Bool) {
        if active { processConsumers.insert(consumer) } else { processConsumers.remove(consumer) }
        let wanted = !processConsumers.isEmpty
        guard wanted != samplesProcesses else { return }
        samplesProcesses = wanted
        if wanted, !isPaused { Task { await refreshNow() } }
    }

    /// Off while another screen switches the view temporarily, so the saved default stays.
    private var persistsViewMode = true
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

    /// Asks the Processes screen to reveal and select the app `row` (a grouped row) with all its
    /// processes: switches to the by-app view without changing the saved default, clears the search.
    public func showInProcesses(_ row: ProcessRow) {
        filter = ""
        persistsViewMode = false
        viewMode = .grouped
        persistsViewMode = true
        focusedRow = row
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

    public func end(_ row: ProcessRow, force: Bool) async -> ActionResult {
        let result = await worker.end(pids: row.kind == .group ? row.memberPIDs : [row.pid], force: force)
        await refreshNow()
        return result
    }

    /// Sends `signal` to the process, or to every process of an app group.
    public func signal(_ row: ProcessRow, _ signal: ProcessSignal) async -> ActionResult {
        await refreshing { await worker.signal(pids: row.memberPIDs, signal.rawValue) }
    }

    public func suspend(_ row: ProcessRow) async -> ActionResult {
        await refreshing { await worker.suspend(pids: row.memberPIDs) }
    }

    public func resume(_ row: ProcessRow) async -> ActionResult {
        await refreshing { await worker.resume(pids: row.memberPIDs) }
    }

    public func setPriority(_ row: ProcessRow, _ priority: ProcessPriority) async -> ActionResult {
        await refreshing { await worker.setPriority(pids: row.memberPIDs, nice: priority.rawValue) }
    }

    public func details(pid: Int32) async -> ProcessDetails? { await worker.details(pid: pid) }

    /// Apps (and lone processes) matching `text`, busiest first, from the latest sample.
    public func searchApps(_ text: String, limit: Int = 8) async -> [ProcessRow] {
        await worker.rows(query: .init(mode: .grouped, column: .cpu, descending: true, filter: text, limit: limit))
            .filter { $0.depth == 0 }
    }

    /// Live processes by pid, from the latest sample.
    public func summaries(for pids: Set<Int32>) async -> [Int32: ProcessSummary] { await worker.summaries(for: pids) }

    public func powerAssertions() async -> [PowerAssertion] { await worker.powerAssertions() }

    /// Every service; reads the OS on each call, so screens load it on demand.
    public func services() async -> [Service] { await worker.services() }

    public func control(_ service: Service, _ action: ServiceAction) async -> ActionResult {
        await worker.control(service, action)
    }

    public func startupItems() async -> [StartupItem] { await worker.startupItems() }

    /// What System Settings lists under Login Items that launchd plists don't cover (apps opening
    /// at login, apps' background items). nil without full access: reading it as a normal user makes
    /// macOS ask for a password, so only the helper reads it. Waits (without blocking sampling) for
    /// the helper's first read, which takes a few seconds.
    public func managedStartupItems() async -> [StartupItem]? {
        var result = await worker.managedStartupItems()
        for _ in 0..<40 where result?.ready == false {
            try? await Task.sleep(for: .milliseconds(500))
            result = await worker.managedStartupItems()
        }
        guard let items = result?.items else { return nil }
        // Apps that open at login have no launchd job: find them among running processes.
        let apps = Set(items.filter { $0.scope == .openAtLogin && $0.pid == nil }.compactMap(\.appPath))
        let pids = await worker.pids(forApps: apps)
        return items.map { item in
            guard item.pid == nil, item.scope == .openAtLogin, let app = item.appPath, let pid = pids[app] else { return item }
            return item.running(pid)
        }
    }

    public func setEnabled(_ item: StartupItem, _ enabled: Bool) async -> ActionResult {
        await worker.setEnabled(item, enabled)
    }

    private func refreshing(_ action: () async -> ActionResult) async -> ActionResult {
        let result = await action()
        await refreshNow()
        return result
    }

    public func endTree(_ row: ProcessRow) async -> ActionResult {
        let result: ActionResult
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
        let processes = samplesProcesses
        let tick = await worker.tick(query: requested, processes: processes, battery: ticks % 5 == 0)
        guard !Task.isCancelled || !hasSample else { return }
        sample = tick.sample
        history.record(tick.sample)
        if let battery = tick.battery { self.battery = battery }
        hasSample = true
        if processes {
            rows = tick.rows
            // The filter or sort changed while sampling: rebuild against the new query.
            if requested != query { rebuild() }
            topCPU = tick.topCPU
            topMemory = tick.topMemory
            topDisk = tick.topDisk
            topNetwork = tick.topNetwork
            topGPU = tick.topGPU
        }
        if fullAccess.isOn && !tick.helperConnected { await reconnectHelper() }
        // The user may allow the helper in System Settings at any moment.
        if fullAccess == .needsApproval, ticks % 2 == 0, HelperDaemon.state == .enabled { await attachBackgroundHelper() }
        if processes, ticks % 10 == 0 { volumes = await worker.volumes() }
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
