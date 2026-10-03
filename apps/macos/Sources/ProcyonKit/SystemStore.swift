import Foundation
import Observation

/// Owns the core monitor; all calls into C happen on this actor, off the main thread.
actor MonitorWorker {
    /// Created on first use, on this actor: `pc_monitor_create` primes every counter (a full
    /// refresh), which must not hold up the main thread at launch.
    private lazy var monitor = Monitor()
    private let capabilities = Monitor.capabilities
    private var hasProcessNetwork: Bool { capabilities.contains(.processNetwork) }
    private var samplesProcesses = true
    /// Energy each app used while processes were sampled, by app id.
    private var energy: [String: AppEnergy] = [:]
    /// When each app in `energy` last drew power.
    private var energySeen: [String: TimeInterval] = [:]
    private var lastTimestamp: TimeInterval?
    private var lastEnergyPrune: TimeInterval = 0
    /// An app unseen this long is dropped from `energy`; more than `energyCap` apps drops the smallest.
    static let energyRetention: TimeInterval = 24 * 3600
    static let energyCap = 1000

    struct Tick: Sendable {
        var sample: SystemSample
        var rows: [ProcessRow] = []
        var topCPU: [ProcessRow] = []
        var topMemory: [ProcessRow] = []
        var topDisk: [ProcessRow] = []
        var topNetwork: [ProcessRow] = []
        var topGPU: [ProcessRow] = []
        var topPower: [ProcessRow] = []
        var topEnergy: [AppEnergy] = []
        var battery: Battery?
        var helperConnected: Bool
    }

    func systemInfo() -> SystemInfo { Monitor.systemInfo() }

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
        // Every "top apps" list and the energy tally come from one by-app view (each view regroups
        // and sorts every process in the core): the user's own rows when they are that view already.
        let apps: [ProcessRow]
        if query.mode == .grouped, query.filter.isEmpty, query.limit == 0 {
            apps = tick.rows.filter { $0.depth == 0 }
        } else {
            apps = monitor.buildView(.init(mode: .grouped, column: .cpu, descending: true, filter: "", topLevelOnly: true))
        }
        tick.topCPU = Self.top(apps, by: \.cpu)
        tick.topMemory = Self.top(apps, by: \.memory)
        tick.topDisk = Self.top(apps, by: \.diskTotal, over: 0)
        tick.topNetwork = hasProcessNetwork ? Self.top(apps, by: \.networkTotal, over: 0) : []
        tick.topGPU = capabilities.contains(.processGPU) ? Self.top(apps, by: \.gpu, over: 0) : []
        if capabilities.contains(.processEnergy) {
            tick.topPower = Self.top(apps, by: \.power, over: 0.005)
            tick.topEnergy = accumulateEnergy(apps, at: sample.timestamp)
        }
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

    func end(pids: [Int32], force: Bool) -> ActionResult { each(pids) { monitor.end(pid: $0, force: force) } }

    func endTree(pid: Int32) -> ActionResult { monitor.endTree(pid: pid) }

    /// Ends every process in `pids` with all its descendants (whatever app those belong to).
    func endTree(pids: [Int32]) -> ActionResult { monitor.endTree(pids: pids) }

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
    func openFiles(pid: Int32?) -> HandleList<OpenFile> { monitor.openFiles(pid: pid) }
    func processRow(pid: Int32) -> ProcessRow? { monitor.processRow(pid: pid) }
    func connections(pid: Int32?) -> HandleList<NetworkConnection> { monitor.connections(pid: pid) }
    func pids(forApps paths: Set<String>) -> [String: Int32] { monitor.pids(forApps: paths) }
    func managedStartupItems() -> (items: [StartupItem], ready: Bool)? { monitor.managedStartupItems() }
    func setEnabled(_ item: StartupItem, _ enabled: Bool) -> ActionResult { monitor.setEnabled(item, enabled) }

    /// The `SystemStore.topCount` apps with the biggest known `value`, biggest first; apps whose value
    /// is unknown (a first sample after sampling resumed) or not above `floor` are left out.
    static func top<Value: Comparable>(
        _ apps: [ProcessRow], by value: KeyPath<ProcessRow, Value?>, over floor: Value? = nil
    ) -> [ProcessRow] {
        let known: [(row: ProcessRow, value: Value)] = apps.compactMap { row in
            guard let value = row[keyPath: value], floor.map({ value > $0 }) ?? true else { return nil }
            return (row, value)
        }
        return known.sorted { $0.value > $1.value }.prefix(SystemStore.topCount).map(\.row)
    }

    static func top<Value: Comparable>(
        _ apps: [ProcessRow], by value: KeyPath<ProcessRow, Value>, over floor: Value? = nil
    ) -> [ProcessRow] {
        let kept = floor.map { floor in apps.filter { $0[keyPath: value] > floor } } ?? apps
        return Array(kept.sorted { $0[keyPath: value] > $1[keyPath: value] }.prefix(SystemStore.topCount))
    }

    /// Adds each app's power over the time since the last sample; returns the biggest consumers.
    private func accumulateEnergy(_ apps: [ProcessRow], at time: TimeInterval) -> [AppEnergy] {
        defer { lastTimestamp = time }
        // A gap (paused, window closed) isn't attributed to anyone.
        guard let last = lastTimestamp, case let elapsed = time - last, elapsed > 0, elapsed < 10 else {
            return topEnergy()
        }
        for row in apps {
            guard let watts = row.power, watts > 0 else { continue }
            energy[row.appID, default: AppEnergy(row: row)].add(row, joules: watts * elapsed)
            energySeen[row.appID] = time
        }
        if time - lastEnergyPrune >= 60 {
            lastEnergyPrune = time
            pruneEnergy(now: time)
        }
        return topEnergy()
    }

    /// Forgets apps that haven't drawn power for `energyRetention`, then the smallest consumers
    /// beyond `energyCap`, so the tally doesn't grow with every app ever launched.
    private func pruneEnergy(now: TimeInterval) {
        for (app, seen) in energySeen where now - seen >= Self.energyRetention {
            energy[app] = nil
            energySeen[app] = nil
        }
        guard energy.count > Self.energyCap else { return }
        let excess = energy.values.sorted { $0.joules < $1.joules }.prefix(energy.count - Self.energyCap)
        for app in excess {
            energy[app.id] = nil
            energySeen[app.id] = nil
        }
    }

    private func topEnergy() -> [AppEnergy] {
        Array(energy.values.sorted { $0.joules > $1.joules }.prefix(SystemStore.topCount))
    }
}

/// Observable app state shared by every screen.
@MainActor
@Observable
public final class SystemStore {
    public static let refreshIntervals: [Double] = [0.5, 1, 2, 5]
    /// Length of the "top apps" lists.
    public nonisolated static let topCount = 10

    /// Static machine facts. Read off the main thread once sampling starts: empty strings and zero
    /// counts (and an `uptime` of zero) until then, which takes a few milliseconds after `start()`.
    public private(set) var info = SystemInfo()
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
    /// Apps drawing the most power now; empty without `Capabilities.processEnergy`.
    public private(set) var topPower: [ProcessRow] = []
    /// Apps that used the most energy since Procyon started (while it sampled processes).
    public private(set) var topEnergy: [AppEnergy] = []
    /// When `topEnergy` started counting.
    public let energySince = Date()

    /// Alert rules; changing them saves them and starts or stops watching apps in the background.
    public var alertSettings: AlertSettings {
        didSet {
            guard alertSettings != oldValue else { return }
            alertSettings.save(defaults)
            // An edited rule starts its clock over, so a lowered threshold doesn't fire at once.
            for rule in alertSettings.rules where oldValue.rules.first(where: { $0.kind == rule.kind }) != rule {
                alertEvaluator.reset(rule.kind)
            }
            needsProcesses("alerts", alertSettings.watchesApps)
        }
    }
    /// Alerts raised this session, newest first.
    public private(set) var recentAlerts: [AlertEvent] = []
    /// The last 24 hours, minute by minute, on disk.
    public let historyDatabase = HistoryDatabase()
    /// Whether samples are written to `historyDatabase`.
    public var recordsHistory: Bool {
        didSet { if recordsHistory != oldValue { defaults.set(recordsHistory, forKey: Keys.history) } }
    }
    private var historyRecorder = HistoryRecorder()

    /// Delivers each alert (the app shows a notification).
    public var onAlert: ((AlertEvent) -> Void)?
    private var alertEvaluator = AlertEvaluator()
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
    /// Pausing stops every update, alerts included (the spec's pause pauses the whole app; a paused
    /// Procyon watches nothing). The time paused doesn't count toward any alert's duration.
    public var isPaused = false {
        didSet {
            guard isPaused != oldValue else { return }
            if isPaused { alertEvaluator.reset() }
            restart()
        }
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
        guard wanted, started, !isPaused else { return }
        Task {
            await refreshNow()
            // The first sample after sampling resumes has no previous counters, so its rates are
            // unknown ("—"): take another soon rather than after a whole interval.
            try? await Task.sleep(for: .milliseconds(500))
            guard samplesProcesses, !isPaused else { return }
            await refreshNow()
        }
    }

    /// Off while another screen switches the view temporarily, so the saved default stays.
    private var persistsViewMode = true
    private let worker = MonitorWorker()
    private let defaults: UserDefaults
    private var started = false
    private var loop: Task<Void, Never>?
    private var rebuildTask: Task<Void, Never>?
    /// The latest write of a partial minute (see `flushHistory`), so `shutdown` can wait for it.
    private var historyWrite: Task<Void, Never>?
    private var ticks = 0
    private var lastApprovalCheck: TimeInterval = -.infinity

    private enum Keys {
        static let interval = "refreshInterval"
        static let viewMode = "processViewMode"
        static let fullAccess = "fullAccessEnabled"
        static let history = "historyEnabled"
    }

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        capabilities = Monitor.capabilities
        let storedInterval = defaults.double(forKey: Keys.interval)
        interval = min(max(storedInterval > 0 ? storedInterval : 1, 0.5), 5)
        viewMode = (defaults.object(forKey: Keys.viewMode) as? Int).flatMap(ViewMode.init(rawValue:)) ?? .grouped
        sortColumn = .cpu
        sortDescending = true
        recordsHistory = defaults.object(forKey: Keys.history) as? Bool ?? true
        alertSettings = AlertSettings.load(defaults)
        if alertSettings.watchesApps { processConsumers.insert("alerts") }
    }

    /// Zero until `info` has been read.
    public var uptime: TimeInterval { info.logicalCores > 0 ? Date().timeIntervalSince(info.bootTime) : 0 }

    /// Starts sampling (every screen calls it as it appears; only the first call does anything).
    /// While paused, nothing is sampled until the user resumes.
    public func start() {
        guard !started else { return }
        started = true
        Task { info = await worker.systemInfo() }
        restart()
        resumeFullAccess()
    }

    /// Stops sampling and writes the minute of history in progress (`start()` starts again).
    public func stop() {
        loop?.cancel()
        loop = nil
        started = false
        flushHistory()
    }

    /// Stops sampling and waits for the history on disk to be complete: call before the process
    /// exits, e.g. `await store.shutdown()` from a task that `NSApplicationDelegate` waits for
    /// (`applicationShouldTerminate` returning `.terminateLater`, then `reply(toApplicationShouldTerminate:)`).
    public func shutdown() async {
        stop()
        await historyWrite?.value
    }

    /// `shutdown()` for a synchronous caller such as `applicationWillTerminate`: blocks the main
    /// thread until the history is written, at most `timeout` seconds (a write takes milliseconds).
    public func shutdown(waitingUpTo timeout: TimeInterval) {
        stop()
        guard let write = historyWrite else { return }
        let done = DispatchSemaphore(value: 0)
        // Detached: a task inheriting this actor would wait for the main thread, which is blocked here.
        Task.detached {
            await write.value
            done.signal()
        }
        _ = done.wait(timeout: .now() + timeout)
    }

    /// Writes the minute in progress so no history is lost when sampling stops or the app quits; a
    /// later sample of the same minute replaces it.
    private func flushHistory() {
        guard recordsHistory, let record = historyRecorder.flush() else { return }
        let previous = historyWrite
        historyWrite = Task.detached { [historyDatabase] in
            await previous?.value
            await historyDatabase.write(record)
        }
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

    /// Re-reads whether the background helper is still registered: the user can remove it in
    /// System Settings → Login Items while Procyon runs. Settings calls this when it appears.
    public func refreshHelperRegistration() {
        guard usesBackgroundHelper else { return }
        let state = HelperDaemon.state
        backgroundHelperRegistered = state != .notRegistered
        if state == .notRegistered, fullAccess == .needsApproval { fullAccess = .off }
    }

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

    /// Files open right now in `pid`, or in every process when nil. Walks every descriptor: load on
    /// demand. Incomplete without full access (other users' processes are skipped).
    public func openFiles(pid: Int32? = nil) async -> HandleList<OpenFile> { await worker.openFiles(pid: pid) }

    /// The process `pid` as a row of the flat list (for actions and Get Info), from the latest sample.
    public func processRow(pid: Int32) async -> ProcessRow? { await worker.processRow(pid: pid) }

    /// Network sockets of `pid`, or of every process when nil.
    public func connections(pid: Int32? = nil) async -> HandleList<NetworkConnection> {
        await worker.connections(pid: pid)
    }

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

    /// Force-kills the process (or every process of an app group) and all its descendants, including
    /// children that belong to other apps (a shell or node an app spawned).
    public func endTree(_ row: ProcessRow) async -> ActionResult {
        let result: ActionResult
        if row.kind == .group {
            result = await worker.endTree(pids: row.memberPIDs)
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
        // Not started yet (a setting changed before the first screen appeared), or paused: no loop.
        guard started, !isPaused else {
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
            // The filter or sort changed while sampling: these rows answer the old query, so leave
            // them out and rebuild against the new one.
            if requested == query { rows = tick.rows } else { rebuild() }
            topCPU = tick.topCPU
            topMemory = tick.topMemory
            topDisk = tick.topDisk
            topNetwork = tick.topNetwork
            topGPU = tick.topGPU
            topPower = tick.topPower
            topEnergy = tick.topEnergy
        }
        checkAlerts(tick)
        if recordsHistory {
            let busiest =
                processes ? tick.topCPU + tick.topMemory + tick.topDisk + tick.topNetwork + tick.topGPU + tick.topPower : []
            if let record = historyRecorder.add(tick.sample, busiest: busiest) { await historyDatabase.write(record) }
        }
        if fullAccess.isOn && !tick.helperConnected { await reconnectHelper() }
        // The user may allow the helper in System Settings at any moment; asking launchd costs an XPC
        // round trip, so look every few seconds, not every tick.
        if fullAccess == .needsApproval, tick.sample.timestamp - lastApprovalCheck >= 5 {
            lastApprovalCheck = tick.sample.timestamp
            if HelperDaemon.state == .enabled { await attachBackgroundHelper() }
        }
        if processes, ticks % 10 == 0 { volumes = await worker.volumes() }
        ticks += 1
    }

    private func checkAlerts(_ tick: MonitorWorker.Tick) {
        guard alertSettings.isAnyEnabled else { return }
        // The busiest apps by CPU and by memory cover every app that can cross a per-app threshold
        // unless more than `topCount` do at once.
        var seen = Set<String>()
        let apps = (tick.topCPU + tick.topMemory).filter { seen.insert($0.id).inserted }
        let events = alertEvaluator.evaluate(tick.sample, apps: apps, settings: alertSettings, now: tick.sample.timestamp)
        for event in events {
            recentAlerts.insert(event, at: 0)
            onAlert?(event)
        }
        if recentAlerts.count > 20 { recentAlerts.removeLast(recentAlerts.count - 20) }
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
