import AppKit
import ProcyonDesign
import ProcyonKit
import SwiftUI
import UniformTypeIdentifiers

/// Which app listens on which port, who talks to whom, and who holds a file or folder open.
struct InspectView: View {
    @Environment(SystemStore.self) private var store
    @Environment(ProcessActionCenter.self) private var actions
    @State private var tab: Tab = .ports
    @State private var connections = HandleList<NetworkConnection>(items: [], isComplete: true)
    @State private var files = HandleList<OpenFile>(items: [], isComplete: true)
    @State private var filesLoaded = false
    @State private var connectionsLoaded = false
    @State private var processes: [Int32: ProcessSummary] = [:]
    @State private var search = ""
    @State private var hideLoopback = false
    /// "Who is using…": a file or folder picked by the user; open files are filtered to it.
    @State private var target: URL?
    @State private var selection: String?
    @FocusState private var searchFocused: Bool
    /// The current tab's rows, filtered and sorted once per change of data, search or sort order
    /// (not on every body pass, which reads them several times).
    @State private var ports: [SocketRow] = []
    @State private var activeConnections: [SocketRow] = []
    @State private var visibleFiles: [FileRow] = []
    /// Bumped by every load, so the rows are rebuilt from the new data.
    @State private var dataVersion = 0

    enum Tab: String, CaseIterable, Identifiable {
        case ports = "Listening Ports", connections = "Connections", files = "Open Files"
        var id: String { rawValue }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Tokens.Space.lg) {
            PageHeader("Files & Ports", subtitle: summary) {
                Picker("Show", selection: $tab) {
                    ForEach(Tab.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .fixedSize()
            }

            HStack(spacing: Tokens.Space.md) {
                SearchField(text: $search, prompt: searchPrompt, shortcutHint: nil, focus: $searchFocused)
                    .frame(maxWidth: 340)
                switch tab {
                case .ports, .connections:
                    Toggle("Hide local-only", isOn: $hideLoopback)
                        .toggleStyle(.checkbox)
                        .help("Hide ports that only accept connections from this Mac (127.0.0.1, ::1)")
                case .files:
                    Button("Who Is Using…", systemImage: "doc.viewfinder") { pickTarget() }
                        .help("Pick a file, folder or disk to see which processes keep it open")
                    if let target {
                        Badge(target.lastPathComponent, tone: .accent, symbol: "scope")
                        Button("Clear", systemImage: "xmark.circle.fill") { self.target = nil }
                            .labelStyle(.iconOnly)
                            .buttonStyle(.borderless)
                    }
                }
                Spacer()
                Button("Refresh", systemImage: "arrow.clockwise") { Task { await load(tab, force: true) } }
            }

            if !isComplete {
                FullAccessNotice(
                    title: "Processes of other users are hidden",
                    message:
                        "System services and other users' processes need administrator access to show their files and sockets.")
            }

            Group {
                switch tab {
                case .ports: portsTable
                case .connections: connectionsTable
                case .files: filesTable
                }
            }
            .overlay { overlay }
            .cardSurface(padding: 0)
        }
        .padding(.horizontal, Tokens.Space.xxl)
        .padding(.top, Tokens.Space.lg)
        .padding(.bottom, Tokens.Space.xl)
        // Sockets change all the time: keep them fresh while shown. Open files are a bigger scan
        // (thousands of descriptors) and load on demand.
        .task(id: tab) {
            selection = nil
            await load(tab, force: tab == .files && !filesLoaded)
            guard tab != .files else { return }
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(3))
                guard !Task.isCancelled else { return }
                await load(tab, force: true)
            }
        }
        .onChange(of: store.fullAccess) { Task { await load(tab, force: true) } }
        .onChange(of: rowInputs, initial: true) { rebuildRows() }
    }

    // MARK: - Data

    private var isComplete: Bool { tab == .files ? files.isComplete : connections.isComplete }

    private var searchPrompt: String {
        tab == .files ? "Search by path or app" : "Search by app, port or address"
    }

    private var summary: String {
        switch tab {
        case .ports: connectionsLoaded ? "\(ports.count) listening · \(exposedCount) reachable from the network" : "Loading…"
        case .connections: connectionsLoaded ? "\(activeConnections.count) connections" : "Loading…"
        case .files: filesLoaded ? "\(Format.count(visibleFiles.count)) open files" : "Loading…"
        }
    }

    private var exposedCount: Int { ports.filter(\.connection.isExposed).count }

    private func name(_ pid: Int32) -> String {
        guard let process = processes[pid] else { return "PID \(pid)" }
        return process.appName.isEmpty ? process.name : process.appName
    }

    private func matches(_ pid: Int32, _ fields: String...) -> Bool {
        guard !search.isEmpty else { return true }
        let process = processes[pid]
        return ([process?.name, process?.appName, "\(pid)"].compactMap(\.self) + fields).contains {
            $0.localizedCaseInsensitiveContains(search)
        }
    }

    struct SocketRow: Identifiable, Hashable {
        let connection: NetworkConnection
        let process: String
        var id: String { connection.id }
        var port: Int { connection.localPort }
        var remote: String { connection.remoteEndpoint }
        var stateTitle: String { connection.state.title }
    }

    private func socketRows(_ include: (NetworkConnection) -> Bool) -> [SocketRow] {
        connections.items
            .filter { include($0) && !(hideLoopback && $0.isLoopbackOnly) }
            .filter { matches($0.pid, $0.localEndpoint, $0.remoteEndpoint, "\($0.localPort)") }
            .map { SocketRow(connection: $0, process: name($0.pid)) }
    }

    @State private var portOrder = [KeyPathComparator(\SocketRow.port)]
    @State private var connectionOrder = [KeyPathComparator(\SocketRow.process, comparator: .localizedStandard)]
    @State private var fileOrder = [KeyPathComparator(\FileRow.path, comparator: .localizedStandard)]

    struct FileRow: Identifiable, Hashable {
        let file: OpenFile
        let process: String
        var id: String { file.id }
        var path: String { file.path }
        var kind: String { file.kind.title }
    }

    /// Everything the rows are derived from.
    private struct RowInputs: Equatable {
        var tab: Tab
        var dataVersion: Int
        var search: String
        var hideLoopback: Bool
        var target: URL?
        var portOrder: [KeyPathComparator<SocketRow>]
        var connectionOrder: [KeyPathComparator<SocketRow>]
        var fileOrder: [KeyPathComparator<FileRow>]
    }

    private var rowInputs: RowInputs {
        RowInputs(
            tab: tab, dataVersion: dataVersion, search: search, hideLoopback: hideLoopback, target: target,
            portOrder: portOrder, connectionOrder: connectionOrder, fileOrder: fileOrder)
    }

    /// Filters and sorts the shown tab's rows.
    private func rebuildRows() {
        switch tab {
        case .ports: ports = socketRows(\.isListening).sorted(using: portOrder)
        case .connections: activeConnections = socketRows { !$0.isListening }.sorted(using: connectionOrder)
        case .files:
            visibleFiles =
                files.items
                .filter { file in target.map { file.isWithin($0.path) } ?? true }
                .filter { matches($0.pid, $0.path) }
                .map { FileRow(file: $0, process: name($0.pid)) }
                .sorted(using: fileOrder)
        }
    }

    private func load(_ tab: Tab, force: Bool) async {
        switch tab {
        case .ports, .connections:
            guard force || !connectionsLoaded else { return }
            connections = await store.connections()
            connectionsLoaded = true
        case .files:
            guard force || !filesLoaded else { return }
            files = await store.openFiles()
            filesLoaded = true
        }
        await refreshNames()
        dataVersion += 1
    }

    /// Names for every pid shown, read again from the latest sample on each load: a pid can belong to
    /// another process by the next refresh.
    private func refreshNames() async {
        let pids = Set(connections.items.map(\.pid)).union(files.items.map(\.pid))
        processes = pids.isEmpty ? [:] : await store.summaries(for: pids)
    }

    private func pickTarget() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.treatsFilePackagesAsDirectories = true
        panel.directoryURL = URL(fileURLWithPath: "/Volumes")
        panel.message = "Choose a file, folder or disk to find the processes using it"
        panel.prompt = "Find"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        // The kernel reports resolved paths (/private/tmp, not /tmp).
        target = url.resolvingSymlinksInPath()
        Task { await load(.files, force: true) }
    }

    // MARK: - Tables

    @ViewBuilder
    private var overlay: some View {
        let loaded = tab == .files ? filesLoaded : connectionsLoaded
        let empty =
            switch tab {
            case .ports: ports.isEmpty
            case .connections: activeConnections.isEmpty
            case .files: visibleFiles.isEmpty
            }
        if !loaded {
            ProgressView()
        } else if empty {
            if tab == .files, let target {
                EmptyState(
                    symbol: "checkmark.circle", title: "Nothing is using “\(target.lastPathComponent)”",
                    message: isComplete
                        ? "No process holds it open. If it still won't eject or delete, try again in a moment."
                        : "None of your processes hold it open. Unlock full access to check system processes too.")
            } else {
                EmptyState(symbol: "magnifyingglass", title: "Nothing to show", message: "Nothing matches the search.")
            }
        }
    }

    private var portsTable: some View {
        Table(ports, selection: $selection, sortOrder: $portOrder) {
            TableColumn("App", value: \.process, comparator: .localizedStandard) { processCell($0.connection.pid, $0.process) }
                .width(min: 180, ideal: 260)
            TableColumn("Port", value: \.port) { row in
                Text(verbatim: "\(row.port)").font(Tokens.Typography.mono).monospacedDigit()
            }
            .width(min: 56, ideal: 70)
            TableColumn("Protocol", value: \.connection.transport.rawValue) { row in
                Text("\(row.connection.transport.rawValue) · IPv\(row.connection.ipVersion)")
                    .foregroundStyle(Tokens.Palette.textSecondary)
            }
            .width(min: 90, ideal: 110)
            TableColumn("Address", value: \.connection.localAddress) { row in
                Text(row.connection.localAddress == "*" ? "All addresses" : row.connection.localAddress)
                    .font(Tokens.Typography.mono)
                    .foregroundStyle(Tokens.Palette.textSecondary)
            }
            .width(min: 110, ideal: 160)
            TableColumn("Reachable From") { (row: SocketRow) in
                if row.connection.isExposed {
                    Badge("Network", tone: .warning, symbol: "globe")
                } else {
                    Badge("This Mac", tone: .neutral, symbol: "laptopcomputer")
                }
            }
        }
        .contextMenu(forSelectionType: String.self) { ids in
            if let row = ports.first(where: { ids.contains($0.id) }) { socketMenu(row) }
        }
        .scrollContentBackground(.hidden)
    }

    private var connectionsTable: some View {
        Table(activeConnections, selection: $selection, sortOrder: $connectionOrder) {
            TableColumn("App", value: \.process, comparator: .localizedStandard) { processCell($0.connection.pid, $0.process) }
                .width(min: 180, ideal: 240)
            TableColumn("Remote", value: \.remote) { row in
                Text(row.remote.isEmpty ? "—" : row.remote)
                    .font(Tokens.Typography.mono)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(row.remote)
            }
            .width(min: 160, ideal: 260)
            TableColumn("Local", value: \.connection.localPort) { row in
                Text(row.connection.localEndpoint)
                    .font(Tokens.Typography.mono)
                    .foregroundStyle(Tokens.Palette.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            .width(min: 120, ideal: 180)
            TableColumn("Protocol", value: \.connection.transport.rawValue) { row in
                Text(row.connection.transport.rawValue).foregroundStyle(Tokens.Palette.textSecondary)
            }
            .width(min: 50, ideal: 60)
            TableColumn("State", value: \.stateTitle) { row in
                Text(row.stateTitle.isEmpty ? "—" : row.stateTitle).foregroundStyle(Tokens.Palette.textSecondary)
            }
        }
        .contextMenu(forSelectionType: String.self) { ids in
            if let row = activeConnections.first(where: { ids.contains($0.id) }) { socketMenu(row) }
        }
        .scrollContentBackground(.hidden)
    }

    private var filesTable: some View {
        Table(visibleFiles, selection: $selection, sortOrder: $fileOrder) {
            TableColumn("App", value: \.process, comparator: .localizedStandard) { processCell($0.file.pid, $0.process) }
                .width(min: 180, ideal: 230)
            TableColumn("Kind", value: \.kind) { row in
                Text(row.kind).foregroundStyle(Tokens.Palette.textSecondary)
            }
            .width(min: 70, ideal: 100)
            TableColumn("Path", value: \.path, comparator: .localizedStandard) { row in
                Text(row.path)
                    .font(Tokens.Typography.mono)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(row.path)
            }
        }
        .contextMenu(forSelectionType: String.self) { ids in
            if let row = visibleFiles.first(where: { ids.contains($0.id) }) {
                Button("Show in Finder") {
                    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: row.path)])
                }
                Button("Copy Path") { ProcessActionCenter.copy(row.path) }
                Divider()
                processMenu(row.file.pid)
            }
        }
        .scrollContentBackground(.hidden)
    }

    private func processCell(_ pid: Int32, _ title: String) -> some View {
        HStack(spacing: Tokens.Space.sm) {
            SummaryIcon(summary: processes[pid], size: 18)
            VStack(alignment: .leading, spacing: 0) {
                Text(title).lineLimit(1)
                Text(verbatim: "PID \(pid)").font(Tokens.Typography.caption).foregroundStyle(Tokens.Palette.textTertiary)
            }
        }
        .padding(.vertical, 1)
    }

    @ViewBuilder
    private func socketMenu(_ row: SocketRow) -> some View {
        Button("Copy Local Address") { ProcessActionCenter.copy(row.connection.localEndpoint) }
        if !row.remote.isEmpty {
            Button("Copy Remote Address") { ProcessActionCenter.copy(row.remote) }
        }
        Divider()
        processMenu(row.connection.pid)
    }

    /// Get Info and ending the process that owns the handle.
    @ViewBuilder
    private func processMenu(_ pid: Int32) -> some View {
        Button("Get Info") { withRow(pid) { actions.inspect($0) } }
        Button("End Task") { withRow(pid) { actions.request(.end, $0) } }
        Button("Force Quit") { withRow(pid) { actions.request(.forceQuit, $0) } }
    }

    private func withRow(_ pid: Int32, _ action: @escaping (ProcessRow) -> Void) {
        Task {
            if let row = await store.processRow(pid: pid) {
                action(row)
            } else {
                actions.failure = .init(title: "The process has exited", message: "PID \(pid) is no longer running.")
            }
        }
    }
}

/// App icon of a process known only by its summary.
struct SummaryIcon: View {
    let summary: ProcessSummary?
    var size: CGFloat = 16

    var body: some View { AppIconView(bundlePath: summary?.bundlePath, size: size) }
}
