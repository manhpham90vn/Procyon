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

    static var capabilities: Capabilities { Capabilities(rawValue: pc_capabilities()) }

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
        sample.processCount = Int(snap.process_count)
        sample.threadCount = Int(snap.thread_count)
        sample.restrictedCount = Int(snap.restricted_count)
        return sample
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

        // Group members (all of them, not just the ones matching the filter) for group actions.
        var members: [String: [Int32]] = [:]
        if query.mode == .grouped {
            for process in processes {
                members[Monitor.string(process.app_id), default: []].append(process.pid)
            }
        }

        var ids: [String] = []
        ids.reserveCapacity(rawRows.count)
        var rows: [ProcessRow] = []
        rows.reserveCapacity(rawRows.count)

        for raw in rawRows {
            let parentID = raw.parent_row >= 0 ? ids[Int(raw.parent_row)] : nil
            let row: ProcessRow
            if raw.process_index >= 0 {
                let process = processes[Int(raw.process_index)]
                let id = "p:\(process.pid):\(process.start_time)"
                row = ProcessRow(
                    id: id, kind: .process, pid: process.pid, parentID: parentID, depth: Int(raw.depth),
                    childCount: Int(raw.child_count), processCount: 1,
                    name: Monitor.string(process.name), user: Monitor.string(process.user),
                    path: Monitor.string(process.path), appID: Monitor.string(process.app_id),
                    appName: Monitor.string(process.app_name), flags: ProcessFlags(rawValue: process.flags),
                    cpu: Monitor.known(raw.cpu_percent), memory: raw.memory_bytes >= 0 ? raw.memory_bytes : nil,
                    diskRead: Monitor.known(raw.disk_read_bps), diskWrite: Monitor.known(raw.disk_write_bps),
                    networkReceive: Monitor.known(raw.net_rx_bps), networkSend: Monitor.known(raw.net_tx_bps),
                    threads: raw.threads >= 0 ? Int(raw.threads) : nil,
                    startTime: process.start_time > 0 ? Date(timeIntervalSince1970: TimeInterval(process.start_time)) : nil,
                    memberPIDs: [process.pid]
                )
            } else {
                let groupID = raw.group_id.map { String(cString: $0) } ?? ""
                let name = raw.group_name.map { String(cString: $0) } ?? ""
                let main = processes.first { $0.pid == raw.group_pid }
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
                    memberPIDs: members[groupID] ?? [raw.group_pid]
                )
            }
            ids.append(row.id)
            rows.append(row)
        }
        return rows
    }

    func end(pid: Int32, force: Bool) -> EndResult {
        Monitor.result(pc_process_end(handle, pid, force))
    }

    func endTree(pid: Int32) -> EndResult {
        Monitor.result(pc_process_end_tree(handle, pid))
    }

    // MARK: - Helpers

    private static func result(_ raw: pc_result) -> EndResult {
        switch raw {
        case PC_OK: .ok
        case PC_ERR_NOT_FOUND: .notFound
        case PC_ERR_PERMISSION: .permissionDenied
        case PC_ERR_PROTECTED: .protected
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
