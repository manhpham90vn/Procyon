import Foundation
import ProcyonCore

/// Thin Swift wrapper over the C core. Not thread-safe: only use it from `MonitorWorker`.
final class Monitor {
    private let handle: OpaquePointer

    init() {
        handle = pc_monitor_create()
    }

    deinit {
        pc_monitor_destroy(handle)
    }

    /// Probes the hardware (GPUs, sensors, battery): read once.
    static let capabilities = Capabilities(rawValue: pc_capabilities())

    static func systemInfo() -> SystemInfo {
        var raw = pc_system_info()
        var info = SystemInfo()
        guard pc_system_info_get(&raw) else { return info }
        info.osName = string(raw.os_name)
        info.osVersion = string(raw.os_version)
        info.osBuild = string(raw.os_build)
        info.kernel = string(raw.kernel)
        info.hostname = string(raw.hostname)
        info.modelID = string(raw.model_id)
        info.modelName = string(raw.model_name)
        info.cpuBrand = string(raw.cpu_brand)
        info.architecture = string(raw.arch)
        info.physicalCores = Int(raw.physical_cores)
        info.logicalCores = Int(raw.logical_cores)
        info.performanceCores = Int(raw.performance_cores)
        info.efficiencyCores = Int(raw.efficiency_cores)
        info.cpuFrequencyHz = raw.cpu_frequency_hz
        info.memoryTotal = raw.memory_total
        info.bootTime = Date(timeIntervalSince1970: TimeInterval(raw.boot_time))
        let kinds = withUnsafeBytes(of: raw.core_kinds) { Array($0.prefix(Int(max(raw.logical_cores, 0)))) }
        if kinds.contains(where: { $0 != 0 }) { info.coreKinds = kinds.map { CoreKind(rawValue: $0) ?? .unknown } }
        return info
    }

    func refresh() -> SystemSample {
        guard let snap = pc_monitor_refresh(handle)?.pointee else { return SystemSample() }
        var sample = SystemSample()
        sample.timestamp = snap.timestamp
        sample.cpuUsage = snap.cpu_usage
        sample.cpuUser = snap.cpu_user
        sample.cpuSystem = snap.cpu_system
        sample.coreUsage = Array(UnsafeBufferPointer(start: snap.core_usage, count: Int(snap.core_count)))
        sample.loadAverage = [snap.load_average.0, snap.load_average.1, snap.load_average.2]
        sample.memoryTotal = snap.memory_total
        sample.memoryUsed = snap.memory_used
        sample.memoryApp = snap.memory_app
        sample.memoryWired = snap.memory_wired
        sample.memoryCompressed = snap.memory_compressed
        sample.memoryCached = snap.memory_cached
        sample.memoryFree = snap.memory_free
        sample.swapTotal = snap.swap_total
        sample.swapUsed = snap.swap_used
        sample.memoryPressure = MemoryPressure(rawValue: Int(snap.memory_pressure)) ?? .unknown
        sample.diskReadRate = snap.disk_read_bps
        sample.diskWriteRate = snap.disk_write_bps
        sample.diskReadTotal = snap.disk_read_total
        sample.diskWriteTotal = snap.disk_write_total
        sample.networkReceiveRate = snap.net_rx_bps
        sample.networkSendRate = snap.net_tx_bps
        sample.networkReceiveTotal = snap.net_rx_total
        sample.networkSendTotal = snap.net_tx_total
        if Monitor.capabilities.contains(.processEnergy), snap.process_count > 0 {
            // Unknown (-1) processes don't count; nil when no process reports power (a first sample).
            var total = 0.0
            var known = false
            for process in UnsafeBufferPointer(start: snap.processes, count: Int(snap.process_count))
            where process.power_watts >= 0 {
                total += process.power_watts
                known = true
            }
            sample.appPower = known ? total : nil
        }
        sample.processCount = Int(snap.process_count)
        sample.threadCount = Int(snap.thread_count)
        sample.restrictedCount = Int(snap.restricted_count)
        sample.cpuTemperature = snap.cpu_temperature >= 0 ? snap.cpu_temperature : nil
        sample.diskTemperature = snap.disk_temperature >= 0 ? snap.disk_temperature : nil
        if snap.gpu_count > 0, let gpus = snap.gpus {
            sample.gpus = UnsafeBufferPointer(start: gpus, count: Int(snap.gpu_count)).enumerated().map { index, raw in
                var gpu = GPUInfo(id: index)
                gpu.name = Monitor.string(raw.name)
                gpu.vendor = Monitor.string(raw.vendor)
                gpu.cores = Int(raw.cores)
                gpu.unifiedMemory = raw.unified_memory
                gpu.utilization = Monitor.known(raw.utilization)
                gpu.rendererUtilization = Monitor.known(raw.renderer_utilization)
                gpu.tilerUtilization = Monitor.known(raw.tiler_utilization)
                gpu.encoderUtilization = Monitor.known(raw.encoder_utilization)
                gpu.decoderUtilization = Monitor.known(raw.decoder_utilization)
                gpu.memoryUsed = raw.memory_used >= 0 ? raw.memory_used : nil
                gpu.memoryTotal = raw.memory_total > 0 ? raw.memory_total : nil
                gpu.temperature = Monitor.known(raw.temperature)
                return gpu
            }
        }
        return sample
    }

    func setProcessSampling(_ enabled: Bool) { pc_monitor_set_process_sampling(handle, enabled) }

    /// Name, path, memory and CPU of the given pids from the last snapshot.
    func summaries(for pids: Set<Int32>) -> [Int32: ProcessSummary] {
        guard !pids.isEmpty, let snap = pc_monitor_snapshot(handle)?.pointee, snap.process_count > 0 else { return [:] }
        var result: [Int32: ProcessSummary] = [:]
        for process in UnsafeBufferPointer(start: snap.processes, count: Int(snap.process_count))
        where pids.contains(process.pid) {
            let path = Monitor.string(process.path)
            let appID = Monitor.string(process.app_id)
            result[process.pid] = ProcessSummary(
                pid: process.pid, name: Monitor.string(process.name), appName: Monitor.string(process.app_name),
                path: path, bundlePath: appID.hasSuffix(".app") ? appID : nil,
                memory: process.memory_bytes >= 0 ? process.memory_bytes : nil, cpu: Monitor.known(process.cpu_percent))
        }
        return result
    }

    func attachHelper(socketPath: String) -> Bool {
        pc_monitor_attach_helper(handle, socketPath)
    }

    func detachHelper() {
        pc_monitor_detach_helper(handle)
    }

    var helperConnected: Bool {
        pc_monitor_helper_state(handle) == Int32(PC_HELPER_CONNECTED.rawValue)
    }

    func volumes() -> [Volume] {
        var pointer: UnsafePointer<pc_volume>?
        let count = Int(pc_monitor_volumes(handle, &pointer))
        guard let pointer else { return [] }
        return UnsafeBufferPointer(start: pointer, count: count).map { raw in
            Volume(
                name: Monitor.string(raw.name),
                mountPoint: Monitor.string(raw.mount_point),
                fileSystem: Monitor.string(raw.file_system),
                totalBytes: raw.total_bytes,
                availableBytes: raw.available_bytes,
                isRoot: raw.is_root,
                isRemovable: raw.is_removable
            )
        }
    }

    struct Query: Sendable, Hashable {
        var mode: ViewMode
        var column: ProcessColumn
        var descending: Bool
        var filter: String
        var limit: Int = 0
        /// Skip rows below the top level (a grouped view's member processes): the core still builds
        /// them, but converting them is most of the Swift-side cost.
        var topLevelOnly = false
    }

    func buildView(_ query: Query) -> [ProcessRow] {
        guard let snap = pc_monitor_snapshot(handle)?.pointee, snap.process_count > 0 else { return [] }
        let processes = UnsafeBufferPointer(start: snap.processes, count: Int(snap.process_count))

        var rowsPointer: UnsafePointer<pc_row>?
        let count: Int32 = query.filter.withCString { filter in
            var raw = pc_view_query(
                mode: Int32(query.mode.rawValue), sort_column: Int32(query.column.rawValue),
                descending: query.descending, filter: filter, limit: Int32(query.limit))
            return pc_monitor_build_view(handle, &raw, &rowsPointer)
        }
        guard let rowsPointer, count > 0 else { return [] }
        let rawRows = UnsafeBufferPointer(start: rowsPointer, count: Int(count))

        // Group members (all of them, not just the ones matching the filter) for group actions, and
        // where each group's main process is, both built once for every group row.
        var members: [String: [Int32]] = [:]
        var indexOfPID: [Int32: Int] = [:]
        if query.mode == .grouped {
            indexOfPID.reserveCapacity(processes.count)
            for (index, process) in processes.enumerated() {
                members[Monitor.string(process.app_id), default: []].append(process.pid)
                indexOfPID[process.pid] = index
            }
        }

        var ids: [String] = []
        ids.reserveCapacity(rawRows.count)
        var rows: [ProcessRow] = []
        rows.reserveCapacity(rawRows.count)

        for raw in rawRows {
            // Row indices stay aligned with `ids` even when a row is skipped.
            if query.topLevelOnly, raw.depth > 0 {
                ids.append("")
                continue
            }
            let parentID = raw.parent_row >= 0 ? ids[Int(raw.parent_row)] : nil
            let row: ProcessRow
            if raw.process_index >= 0 {
                // A process row carries the process's own values.
                row = Monitor.row(
                    for: processes[Int(raw.process_index)], parentID: parentID, depth: Int(raw.depth),
                    childCount: Int(raw.child_count))
            } else {
                let groupID = raw.group_id.map { String(cString: $0) } ?? ""
                let name = raw.group_name.map { String(cString: $0) } ?? ""
                let main = indexOfPID[raw.group_pid].map { processes[$0] }
                var flags = ProcessFlags()
                if groupID.hasSuffix(".app") { flags.insert(.appBundle) }
                if let main, ProcessFlags(rawValue: main.flags).contains(.system) { flags.insert(.system) }
                row = ProcessRow(
                    id: "g:\(groupID)", kind: .group, pid: raw.group_pid, parentID: parentID, depth: Int(raw.depth),
                    childCount: Int(raw.child_count), processCount: Int(raw.process_count),
                    name: name, user: main.map { Monitor.string($0.user) } ?? "",
                    path: main.map { Monitor.string($0.path) } ?? "", appID: groupID, appName: name, flags: flags,
                    cpu: Monitor.known(raw.cpu_percent), memory: raw.memory_bytes >= 0 ? raw.memory_bytes : nil,
                    diskRead: Monitor.known(raw.disk_read_bps), diskWrite: Monitor.known(raw.disk_write_bps),
                    networkReceive: Monitor.known(raw.net_rx_bps), networkSend: Monitor.known(raw.net_tx_bps),
                    threads: raw.threads >= 0 ? Int(raw.threads) : nil,
                    startTime: main.flatMap {
                        $0.start_time > 0 ? Date(timeIntervalSince1970: TimeInterval($0.start_time)) : nil
                    },
                    memberPIDs: members[groupID] ?? [raw.group_pid], gpu: Monitor.known(raw.gpu_percent),
                    nice: main?.nice ?? 0, state: main.flatMap { ProcessState(rawValue: $0.state) } ?? .unknown,
                    power: Monitor.known(raw.power_watts)
                )
            }
            ids.append(row.id)
            rows.append(row)
        }
        return rows
    }

    /// The process `pid` as a top-level row of the flat list, straight from the last snapshot (no view
    /// is built or sorted).
    func processRow(pid: Int32) -> ProcessRow? {
        guard let snap = pc_monitor_snapshot(handle)?.pointee, snap.process_count > 0 else { return nil }
        let processes = UnsafeBufferPointer(start: snap.processes, count: Int(snap.process_count))
        guard let process = processes.first(where: { $0.pid == pid }) else { return nil }
        return Monitor.row(for: process, parentID: nil, depth: 0, childCount: 0)
    }

    private static func row(for process: pc_process, parentID: String?, depth: Int, childCount: Int) -> ProcessRow {
        ProcessRow(
            id: "p:\(process.pid):\(process.start_time)", kind: .process, pid: process.pid, parentID: parentID,
            depth: depth, childCount: childCount, processCount: 1,
            name: string(process.name), user: string(process.user), path: string(process.path),
            appID: string(process.app_id), appName: string(process.app_name), flags: ProcessFlags(rawValue: process.flags),
            cpu: known(process.cpu_percent), memory: process.memory_bytes >= 0 ? process.memory_bytes : nil,
            diskRead: known(process.disk_read_bps), diskWrite: known(process.disk_write_bps),
            networkReceive: known(process.net_rx_bps), networkSend: known(process.net_tx_bps),
            threads: process.threads >= 0 ? Int(process.threads) : nil,
            startTime: process.start_time > 0 ? Date(timeIntervalSince1970: TimeInterval(process.start_time)) : nil,
            memberPIDs: [process.pid], gpu: known(process.gpu_percent), nice: process.nice,
            state: ProcessState(rawValue: process.state) ?? .unknown, power: known(process.power_watts)
        )
    }

    func end(pid: Int32, force: Bool) -> ActionResult {
        Monitor.result(pc_process_end(handle, pid, force))
    }

    func endTree(pid: Int32) -> ActionResult {
        Monitor.result(pc_process_end_tree(handle, pid))
    }

    /// Force-kills every process in `pids` and all their descendants (an app group's members and the
    /// children they spawned, whatever app those belong to). Reports the first real failure.
    func endTree(pids: [Int32]) -> ActionResult {
        var parentOf: [Int32: Int32] = [:]
        if let snap = pc_monitor_snapshot(handle)?.pointee, snap.process_count > 0 {
            for process in UnsafeBufferPointer(start: snap.processes, count: Int(snap.process_count)) {
                parentOf[process.pid] = process.ppid
            }
        }
        var outcome = ActionResult.ok
        for root in Monitor.treeRoots(pids, parentOf: parentOf) {
            let result = endTree(pid: root)
            if result.isFailure, !outcome.isFailure { outcome = result }
        }
        return outcome
    }

    /// The members of `pids` that no other member is an ancestor of: ending their trees ends every
    /// member and every descendant exactly once. Keeps the order of `pids`.
    static func treeRoots(_ pids: [Int32], parentOf: [Int32: Int32]) -> [Int32] {
        let set = Set(pids)
        return pids.filter { pid in
            var current = pid
            var hops = 0
            while let parent = parentOf[current], parent != current, hops < 1024 {
                if set.contains(parent) { return false }
                current = parent
                hops += 1
            }
            return true
        }
    }

    func signal(pid: Int32, _ signal: Int32) -> ActionResult {
        Monitor.result(pc_process_signal(handle, pid, signal))
    }

    func suspend(pid: Int32) -> ActionResult { Monitor.result(pc_process_suspend(handle, pid)) }

    func resume(pid: Int32) -> ActionResult { Monitor.result(pc_process_resume(handle, pid)) }

    func setPriority(pid: Int32, nice: Int32) -> ActionResult {
        Monitor.result(pc_process_set_priority(handle, pid, nice))
    }

    func details(pid: Int32) -> ProcessDetails? {
        var pointer: UnsafePointer<pc_process_details>?
        guard pc_process_details_get(handle, pid, &pointer) == PC_OK, let raw = pointer?.pointee else { return nil }
        var details = ProcessDetails()
        details.pid = raw.pid
        details.parentPID = raw.ppid
        details.name = Monitor.string(raw.name)
        details.user = Monitor.string(raw.user)
        details.path = Monitor.string(raw.path)
        details.workingDirectory = Monitor.string(raw.cwd)
        details.nice = raw.nice
        details.state = ProcessState(rawValue: raw.state) ?? .unknown
        details.startTime = raw.start_time > 0 ? Date(timeIntervalSince1970: TimeInterval(raw.start_time)) : nil
        func strings(_ base: UnsafePointer<UnsafePointer<CChar>?>?, _ count: Int32) -> [String] {
            guard let base else { return [] }
            return UnsafeBufferPointer(start: base, count: Int(count)).map { $0.map { String(cString: $0) } ?? "" }
        }
        if raw.arguments_known {
            details.arguments = strings(raw.arguments, raw.argument_count)
            details.environment = strings(raw.environment, raw.environment_count)
        }
        if raw.threads_known {
            details.threads = UnsafeBufferPointer(start: raw.threads, count: Int(raw.thread_count)).map { thread in
                ThreadInfo(
                    id: thread.id, name: Monitor.string(thread.name), cpu: Monitor.known(thread.cpu_percent),
                    userTime: TimeInterval(thread.user_time_ns) / 1e9, systemTime: TimeInterval(thread.system_time_ns) / 1e9,
                    priority: thread.priority, state: ProcessState(rawValue: thread.state) ?? .unknown)
            }
        }
        return details
    }

    /// Open files of `pid`, or of every process when nil.
    func openFiles(pid: Int32?) -> HandleList<OpenFile> {
        var pointer: UnsafePointer<pc_open_file>?
        var complete = false
        let count = Int(pc_monitor_open_files(handle, pid ?? -1, &pointer, &complete))
        guard let pointer else { return HandleList(items: [], isComplete: complete) }
        let items = UnsafeBufferPointer(start: pointer, count: count).map { raw in
            OpenFile(
                pid: raw.pid, descriptor: raw.fd, kind: OpenFile.Kind(rawValue: raw.kind) ?? .other,
                path: raw.path.map { String(cString: $0) } ?? "")
        }
        return HandleList(items: items, isComplete: complete)
    }

    /// TCP and UDP sockets of `pid`, or of every process when nil.
    func connections(pid: Int32?) -> HandleList<NetworkConnection> {
        var pointer: UnsafePointer<pc_connection>?
        var complete = false
        let count = Int(pc_monitor_connections(handle, pid ?? -1, &pointer, &complete))
        guard let pointer else { return HandleList(items: [], isComplete: complete) }
        let items = UnsafeBufferPointer(start: pointer, count: count).map { raw in
            NetworkConnection(
                pid: raw.pid, transport: raw.protocol == Int32(PC_PROTOCOL_UDP.rawValue) ? .udp : .tcp,
                ipVersion: Int(raw.family), state: NetworkConnection.State(rawValue: raw.state) ?? .closed,
                localAddress: Monitor.string(raw.local_address), localPort: Int(raw.local_port),
                remoteAddress: Monitor.string(raw.remote_address), remotePort: Int(raw.remote_port))
        }
        return HandleList(items: items, isComplete: complete)
    }

    static func battery() -> Battery? {
        var raw = pc_battery()
        guard pc_battery_get(&raw), raw.present else { return nil }
        var battery = Battery()
        battery.level = raw.level
        battery.onACPower = raw.on_ac_power
        battery.isCharging = raw.charging
        battery.isFullyCharged = raw.fully_charged
        battery.timeToEmpty = raw.minutes_to_empty >= 0 ? TimeInterval(raw.minutes_to_empty) * 60 : nil
        battery.timeToFull = raw.minutes_to_full >= 0 ? TimeInterval(raw.minutes_to_full) * 60 : nil
        battery.cycleCount = raw.cycle_count >= 0 ? Int(raw.cycle_count) : nil
        battery.health = known(raw.health)
        battery.condition = string(raw.condition)
        battery.designCapacity = raw.design_capacity_mah > 0 ? Int(raw.design_capacity_mah) : nil
        battery.maximumCapacity = raw.max_capacity_mah > 0 ? Int(raw.max_capacity_mah) : nil
        battery.temperature = known(raw.temperature)
        battery.power = raw.power_watts != 0 ? raw.power_watts : nil
        battery.adapter = string(raw.adapter)
        return battery
    }

    func powerAssertions() -> [PowerAssertion] {
        var pointer: UnsafePointer<pc_power_assertion>?
        let count = Int(pc_monitor_power_assertions(handle, &pointer))
        guard let pointer else { return [] }
        return UnsafeBufferPointer(start: pointer, count: count).map { raw in
            PowerAssertion(
                pid: raw.pid, onBehalfOf: raw.on_behalf_of > 0 ? raw.on_behalf_of : nil,
                processName: Monitor.string(raw.process_name), type: Monitor.string(raw.type),
                reason: Monitor.string(raw.reason),
                preventsDisplaySleep: raw.kind & PC_ASSERT_DISPLAY_SLEEP.rawValue != 0,
                created: raw.created > 0 ? Date(timeIntervalSince1970: TimeInterval(raw.created)) : nil)
        }
    }

    func services() -> [Service] {
        var pointer: UnsafePointer<pc_service>?
        let count = Int(pc_monitor_services(handle, &pointer))
        guard let pointer else { return [] }
        return UnsafeBufferPointer(start: pointer, count: count).map { raw in
            Service(
                label: Monitor.string(raw.label), name: Monitor.string(raw.name), program: Monitor.string(raw.program),
                configPath: Monitor.string(raw.config_path), domain: ServiceDomain(rawValue: raw.domain) ?? .user,
                pid: raw.pid > 0 ? raw.pid : nil, lastExitStatus: raw.last_exit, isEnabled: raw.enabled, isApple: raw.apple)
        }
    }

    func control(_ service: Service, _ action: ServiceAction) -> ActionResult {
        Monitor.result(pc_service_control(handle, service.domain.rawValue, service.label, action.rawValue))
    }

    func startupItems() -> [StartupItem] {
        var pointer: UnsafePointer<pc_startup_item>?
        let count = Int(pc_monitor_startup_items(handle, &pointer))
        guard let pointer else { return [] }
        return UnsafeBufferPointer(start: pointer, count: count).map(Monitor.startupItem)
    }

    /// Open at Login and "Allow in the Background" items, from the helper's cache (instant). nil
    /// without the helper: macOS shares this list only with administrators. `ready` is false while
    /// the helper is still reading it.
    func managedStartupItems() -> (items: [StartupItem], ready: Bool)? {
        var pointer: UnsafePointer<pc_startup_item>?
        var ready = false
        let count = Int(pc_monitor_startup_managed_items(handle, &pointer, &ready))
        guard count >= 0 else { return nil }
        guard let pointer else { return ([], ready) }
        return (UnsafeBufferPointer(start: pointer, count: count).map(Monitor.startupItem), ready)
    }

    private static func startupItem(_ raw: pc_startup_item) -> StartupItem {
        let app = string(raw.app_path)
        var item = StartupItem(
            label: string(raw.label), name: string(raw.name), program: string(raw.program),
            configPath: string(raw.config_path), appPath: app.isEmpty ? nil : app,
            scope: StartupScope(rawValue: raw.scope) ?? .userAgent, pid: raw.pid > 0 ? raw.pid : nil,
            isEnabled: raw.enabled, runsAtLoad: raw.run_at_load, keepsAlive: raw.keep_alive)
        item.parentName = string(raw.parent_name)
        item.isManagedByOS = raw.managed_by_os
        return item
    }

    /// pid of a running process of each app bundle, from the last snapshot.
    func pids(forApps paths: Set<String>) -> [String: Int32] {
        guard !paths.isEmpty, let snap = pc_monitor_snapshot(handle)?.pointee, snap.process_count > 0 else { return [:] }
        var result: [String: Int32] = [:]
        for process in UnsafeBufferPointer(start: snap.processes, count: Int(snap.process_count)) {
            let app = Monitor.string(process.app_id)
            if paths.contains(app), result[app] == nil || process.ppid <= 1 { result[app] = process.pid }
        }
        return result
    }

    func setEnabled(_ item: StartupItem, _ enabled: Bool) -> ActionResult {
        Monitor.result(pc_startup_set_enabled(handle, item.scope.rawValue, item.label, enabled))
    }

    // MARK: - Helpers

    private static func result(_ raw: pc_result) -> ActionResult {
        switch raw {
        case PC_OK: .ok
        case PC_ERR_NOT_FOUND: .notFound
        case PC_ERR_PERMISSION: .permissionDenied
        case PC_ERR_PROTECTED: .protected
        case PC_ERR_UNSUPPORTED: .unsupported
        case PC_ERR_INVALID: .invalid
        default: .failed
        }
    }

    private static func known(_ value: Double) -> Double? { value >= 0 ? value : nil }

    /// Reads a fixed-size C char array (imported as a tuple) as UTF-8.
    static func string<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) { buffer in
            guard let base = buffer.baseAddress?.assumingMemoryBound(to: CChar.self) else { return "" }
            let length = strnlen(base, buffer.count)
            return String(decoding: UnsafeRawBufferPointer(start: base, count: length), as: UTF8.self)
        }
    }
}
