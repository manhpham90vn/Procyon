import AppKit
import ProcyonDesign
import ProcyonKit
import SwiftUI

struct ProcessesView: View {
    @Environment(SystemStore.self) private var store
    @State private var selection: ProcessRow.ID?
    /// Rows whose expansion differs from the current mode's default.
    @State private var toggled: Set<ProcessRow.ID> = []
    @State private var pending: PendingAction?
    @State private var failure: String?
    @SceneStorage("processTableColumns") private var columns = TableColumnCustomization<ProcessRow>()
    @FocusState private var searchFocused: Bool
    /// A focused table draws its selection in the accent color instead of a faint gray.
    @FocusState private var tableFocused: Bool
    /// Set by a view switch: select the pinned app once the rows of the new view arrive.
    @State private var reselectPinned = false

    private var visibleRows: [ProcessRow] {
        // While searching, show every match.
        let expand = { (rows: [ProcessRow]) in store.filter.isEmpty ? rows.visible(isExpanded: isExpanded) : rows }
        guard let app = store.pinnedAppID else { return expand(store.rows) }
        let (pinned, rest) = store.rows.pinning(appID: app)
        return expand(pinned) + expand(rest)
    }

    private var selectedRow: ProcessRow? {
        guard let selection else { return nil }
        return store.rows.first { $0.id == selection }
    }

    var body: some View {
        @Bindable var store = store
        VStack(alignment: .leading, spacing: Tokens.Space.lg) {
            PageHeader("Processes", subtitle: summary) {
                if store.fullAccess.isOn {
                    Badge("Full access", tone: .success, symbol: "lock.open.fill")
                        .help("System processes are read through the administrator helper")
                }
                Picker("View", selection: $store.viewMode) {
                    ForEach(ViewMode.allCases) { mode in
                        Label(mode.title, systemImage: mode.symbol).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .fixedSize()
                .help("Show processes as a flat list, grouped by app, or as a parent–child tree")
            }

            HStack(spacing: Tokens.Space.md) {
                SearchField(text: $store.filter, prompt: "Search by name, PID or user", focus: $searchFocused)
                    .frame(maxWidth: 340)
                if !store.filter.isEmpty {
                    Text("\(store.rows.count) matches")
                        .font(Tokens.Typography.label)
                        .foregroundStyle(Tokens.Palette.textTertiary)
                }
                Spacer()
                if store.viewMode != .flat {
                    Button(
                        allExpanded ? "Collapse All" : "Expand All",
                        systemImage: allExpanded ? "rectangle.compress.vertical" : "rectangle.expand.vertical"
                    ) {
                        setAll(expanded: !allExpanded)
                    }
                    .labelStyle(.iconOnly)
                    .buttonStyle(.borderless)
                    .help(allExpanded ? "Collapse all" : "Expand all")
                }
                Button(role: .destructive) {
                    if let row = selectedRow { request(.end, row) }
                } label: {
                    Label("End Task", systemImage: "xmark.octagon.fill")
                }
                .buttonStyle(EndTaskButtonStyle())
                .disabled(selectedRow == nil || selectedRow?.isProtected == true)
                .help("End the selected task (⌘⌫)")
            }

            FullAccessBanner()

            ScrollViewReader { proxy in
                table
                    .focused($tableFocused)
                    .onChange(of: store.focusedRow?.id, initial: true) { revealFocusedRow(proxy, final: false) }
                    // Clearing the search rebuilds the rows; the wanted row may only appear then.
                    .onChange(of: store.rows) {
                        revealFocusedRow(proxy, final: true)
                        keepPinnedSelection()
                    }
            }
        }
        .padding(.horizontal, Tokens.Space.xxl)
        .padding(.top, Tokens.Space.lg)
        .padding(.bottom, Tokens.Space.xl)
        .onChange(of: store.viewMode) {
            toggled = []
            // The selected app stays selected and moves to the top in the new view.
            if let row = selectedRow { store.pinnedAppID = row.appID }
            reselectPinned = store.pinnedAppID != nil
        }
        .onChange(of: selection) { unpinIfElsewhere() }
        .focusedSceneValue(\.processActions, actions)
        .confirmationDialog(
            pending?.title ?? "", isPresented: Binding(get: { pending != nil }, set: { if !$0 { pending = nil } }),
            presenting: pending
        ) { action in
            Button(action.confirmTitle, role: .destructive) { perform(action) }
            Button("Cancel", role: .cancel) {}
        } message: { action in
            Text(action.message)
        }
        .alert("Couldn't end process", isPresented: Binding(get: { failure != nil }, set: { if !$0 { failure = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(failure ?? "")
        }
    }

    private var hasNetwork: Bool { store.capabilities.contains(.processNetwork) }

    private var table: some View {
        let memoryTotal = Double(max(store.sample.memoryTotal, 1))
        // Columns can't be conditional before macOS 14.4, so unsupported ones are hidden and locked.
        let network: TableColumnCustomizationBehavior = hasNetwork ? [] : .visibility
        return Table(visibleRows, selection: $selection, sortOrder: sortOrder, columnCustomization: $columns) {
            TableColumn("Name", value: \.sortName) { row in
                NameCell(
                    row: row, expanded: isExpanded(row), pinned: isPinnedRoot(row), unpin: { store.pinnedAppID = nil }
                ) { toggle(row) }
            }
            .width(min: 180, ideal: 250)
            .customizationID("name")
            .disabledCustomizationBehavior(.visibility)

            TableColumn("PID", value: \.sortPID) { row in
                Text(String(row.pid))
                    .font(Tokens.Typography.mono.monospacedDigit())
                    .foregroundStyle(Tokens.Palette.textSecondary)
            }
            .width(min: 50, ideal: 64)
            .customizationID("pid")

            TableColumn("User", value: \.sortUser) { row in
                Text(row.user).foregroundStyle(Tokens.Palette.textSecondary).lineLimit(1)
            }
            .width(min: 56, ideal: 72)
            .customizationID("user")

            TableColumn("CPU", value: \.sortCPU) { row in
                HeatCell(text: Format.cpu(row.cpu), intensity: (row.cpu ?? 0) / 100, metric: .cpu, restricted: row.cpu == nil)
            }
            .width(min: 56, ideal: 70)
            .alignment(.trailing)
            .customizationID("cpu")

            TableColumn("Memory", value: \.sortMemory) { row in
                HeatCell(
                    text: Format.bytes(row.memory), intensity: Double(row.memory ?? 0) / memoryTotal * 6,
                    metric: .memory, restricted: row.memory == nil)
            }
            .width(min: 64, ideal: 82)
            .alignment(.trailing)
            .customizationID("memory")

            TableColumn("Disk Read", value: \.sortDiskRead) { row in
                HeatCell(
                    text: Format.rate(row.diskRead), intensity: (row.diskRead ?? 0) / 20_000_000, metric: .disk,
                    restricted: row.diskRead == nil)
            }
            .width(min: 64, ideal: 78)
            .alignment(.trailing)
            .customizationID("diskRead")

            TableColumn("Disk Write", value: \.sortDiskWrite) { row in
                HeatCell(
                    text: Format.rate(row.diskWrite), intensity: (row.diskWrite ?? 0) / 20_000_000, metric: .disk,
                    restricted: row.diskWrite == nil)
            }
            .width(min: 64, ideal: 78)
            .alignment(.trailing)
            .customizationID("diskWrite")

            TableColumn("Net ↓", value: \.sortNetworkReceive) { row in
                HeatCell(
                    text: Format.rate(row.networkReceive), intensity: (row.networkReceive ?? 0) / 10_000_000,
                    metric: .network, restricted: row.networkReceive == nil)
            }
            .width(min: 64, ideal: 78)
            .alignment(.trailing)
            .customizationID("networkReceive")
            .defaultVisibility(hasNetwork ? .automatic : .hidden)
            .disabledCustomizationBehavior(network)

            TableColumn("Net ↑", value: \.sortNetworkSend) { row in
                HeatCell(
                    text: Format.rate(row.networkSend), intensity: (row.networkSend ?? 0) / 10_000_000,
                    metric: .network, restricted: row.networkSend == nil)
            }
            .width(min: 64, ideal: 78)
            .alignment(.trailing)
            .customizationID("networkSend")
            .defaultVisibility(hasNetwork ? .automatic : .hidden)
            .disabledCustomizationBehavior(network)

            TableColumn("Threads", value: \.sortThreads) { row in
                Text(Format.count(row.threads))
                    .font(Tokens.Typography.body.monospacedDigit())
                    .foregroundStyle(Tokens.Palette.textSecondary)
            }
            .width(min: 50, ideal: 64)
            .alignment(.trailing)
            .customizationID("threads")
            .defaultVisibility(.hidden)
        }
        .tableStyle(.inset(alternatesRowBackgrounds: false))
        .scrollContentBackground(.hidden)
        .contextMenu(forSelectionType: ProcessRow.ID.self) { ids in
            if let id = ids.first, let row = store.rows.first(where: { $0.id == id }) {
                contextMenu(for: row)
            }
        } primaryAction: { ids in
            if let id = ids.first, let row = store.rows.first(where: { $0.id == id }), row.hasChildren { toggle(row) }
        }
        .overlay {
            if store.hasSample && visibleRows.isEmpty {
                EmptyState(
                    symbol: "magnifyingglass", title: "No matching processes",
                    message: "Nothing matches “\(store.filter)”. Try a name, PID or user.")
            } else if !store.hasSample {
                ProgressView()
            }
        }
        .cardSurface(padding: 0)
        .onAppear {
            // A saved layout from a session that had per-app network must not show empty columns.
            if !hasNetwork {
                columns[visibility: "networkReceive"] = .hidden
                columns[visibility: "networkSend"] = .hidden
            }
        }
        .onKeyPress(.leftArrow) { setSelected(expanded: false) }
        .onKeyPress(.rightArrow) { setSelected(expanded: true) }
    }

    @ViewBuilder
    private func contextMenu(for row: ProcessRow) -> some View {
        Button(row.kind == .group ? "End \(row.processCount) Processes" : "End Task") { request(.end, row) }
            .disabled(row.isProtected)
        Button("Force Quit") { request(.forceQuit, row) }
            .disabled(row.isProtected)
        if row.kind == .process && row.hasChildren {
            Button("End Process Tree") { request(.endTree, row) }
                .disabled(row.isProtected)
        }
        Divider()
        if !row.path.isEmpty {
            Button("Show in Finder") {
                NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: row.bundlePath ?? row.path)])
            }
        }
        Button("Copy Name") { copy(row.name) }
        Button("Copy PID") { copy(String(row.pid)) }
        if !row.path.isEmpty { Button("Copy Path") { copy(row.path) } }
    }

    // MARK: - Summary & sorting

    private var summary: String {
        guard store.hasSample else { return "Collecting…" }
        let s = store.sample
        return
            "\(Format.count(s.processCount)) processes · \(Format.count(s.threadCount)) threads · CPU \(Format.percent(s.cpuUsage)) · Memory \(Format.bytes(s.memoryUsed))"
    }

    private static let columnKeys: [(ProcessColumn, PartialKeyPath<ProcessRow>)] = [
        (.name, \ProcessRow.sortName), (.pid, \ProcessRow.sortPID), (.user, \ProcessRow.sortUser),
        (.cpu, \ProcessRow.sortCPU), (.memory, \ProcessRow.sortMemory), (.diskRead, \ProcessRow.sortDiskRead),
        (.diskWrite, \ProcessRow.sortDiskWrite), (.networkReceive, \ProcessRow.sortNetworkReceive),
        (.networkSend, \ProcessRow.sortNetworkSend), (.threads, \ProcessRow.sortThreads),
    ]

    private var sortOrder: Binding<[KeyPathComparator<ProcessRow>]> {
        Binding {
            [Self.comparator(for: store.sortColumn, descending: store.sortDescending)]
        } set: { order in
            guard let first = order.first,
                let column = Self.columnKeys.first(where: { $0.1 == first.keyPath })?.0
            else { return }
            // A newly clicked column starts in its natural direction (e.g. highest CPU first).
            let descending = column == store.sortColumn ? first.order == .reverse : column.prefersDescending
            store.sort(by: column, descending: descending)
        }
    }

    private static func comparator(for column: ProcessColumn, descending: Bool) -> KeyPathComparator<ProcessRow> {
        let order: SortOrder = descending ? .reverse : .forward
        switch column {
        case .name: return KeyPathComparator(\.sortName, order: order)
        case .pid: return KeyPathComparator(\.sortPID, order: order)
        case .user: return KeyPathComparator(\.sortUser, order: order)
        case .cpu: return KeyPathComparator(\.sortCPU, order: order)
        case .memory: return KeyPathComparator(\.sortMemory, order: order)
        case .diskRead: return KeyPathComparator(\.sortDiskRead, order: order)
        case .diskWrite: return KeyPathComparator(\.sortDiskWrite, order: order)
        case .networkReceive: return KeyPathComparator(\.sortNetworkReceive, order: order)
        case .networkSend: return KeyPathComparator(\.sortNetworkSend, order: order)
        case .threads: return KeyPathComparator(\.sortThreads, order: order)
        }
    }

    // MARK: - Expansion

    private func isExpanded(_ row: ProcessRow) -> Bool {
        store.viewMode.expandsByDefault != toggled.contains(row.id)
    }

    private func toggle(_ row: ProcessRow) {
        withAnimation(.snappy(duration: Tokens.Motion.fast)) {
            if toggled.contains(row.id) { toggled.remove(row.id) } else { toggled.insert(row.id) }
        }
    }

    /// Selects, expands and scrolls to the app another screen asked for, so all its processes show.
    /// `final`: give up if it isn't there (the app has exited).
    private func revealFocusedRow(_ proxy: ScrollViewProxy, final: Bool) {
        guard let wanted = store.focusedRow else { return }
        // The switch to the by-app view rebuilds the rows asynchronously: wait for grouped rows.
        guard store.viewMode == .grouped, store.rows.contains(where: { $0.kind == .group }) else { return }
        guard let target = store.rows.first(where: { $0.id == wanted.id }) else {
            if final { store.focusedRow = nil }
            return
        }
        store.focusedRow = nil
        store.pinnedAppID = target.appID
        if target.hasChildren, !isExpanded(target) { toggle(target) }
        selection = target.id
        tableFocused = true
        // After the expanded rows are laid out.
        Task { proxy.scrollTo(target.id, anchor: .center) }
    }

    // MARK: - Pinning

    /// Rows that carry the pin: the pinned app's group, processes or subtree roots.
    private func isPinnedRoot(_ row: ProcessRow) -> Bool {
        row.depth == 0 && store.pinnedAppID == row.appID
    }

    /// Choosing a row outside the pinned app lets the pin go.
    private func unpinIfElsewhere() {
        guard !reselectPinned, let app = store.pinnedAppID, let selection else { return }
        if !store.rows.pinning(appID: app).pinned.contains(where: { $0.id == selection }) { store.pinnedAppID = nil }
    }

    /// After a view switch the app's rows have new ids (group vs processes): select its first row again.
    private func keepPinnedSelection() {
        guard reselectPinned, let app = store.pinnedAppID else { return }
        // Rows from before the switch can still arrive; wait for the new view's.
        let isGrouped = store.rows.contains { $0.kind == .group }
        guard isGrouped == (store.viewMode == .grouped) else { return }
        reselectPinned = false
        guard let first = store.rows.pinning(appID: app).pinned.first else { return }
        if store.viewMode == .grouped, first.hasChildren, !isExpanded(first) { toggle(first) }
        selection = first.id
    }

    private var allExpanded: Bool {
        store.rows.contains { $0.hasChildren } && store.rows.allSatisfy { !$0.hasChildren || isExpanded($0) }
    }

    private func setAll(expanded: Bool) {
        let parents = store.rows.filter(\.hasChildren).map(\.id)
        withAnimation(.snappy(duration: Tokens.Motion.fast)) {
            toggled = expanded == store.viewMode.expandsByDefault ? [] : Set(parents)
        }
    }

    private func setSelected(expanded: Bool) -> KeyPress.Result {
        guard let row = selectedRow, row.hasChildren, isExpanded(row) != expanded else { return .ignored }
        toggle(row)
        return .handled
    }

    // MARK: - Actions

    private var actions: ProcessActions {
        let row = selectedRow.flatMap { $0.isProtected ? nil : $0 }
        return ProcessActions(
            focusSearch: { searchFocused = true },
            endTask: row.map { row in { request(.end, row) } },
            forceQuit: row.map { row in { request(.forceQuit, row) } },
            endTree: row.flatMap { row in row.kind == .process && row.hasChildren ? { request(.endTree, row) } : nil }
        )
    }

    private func request(_ kind: PendingAction.Kind, _ row: ProcessRow) {
        let action = PendingAction(kind: kind, row: row)
        // Plain "End Task" on a user process needs no confirmation, like the OS task managers.
        if kind == .end && !row.isSystem && row.kind == .process {
            perform(action)
        } else {
            pending = action
        }
    }

    private func perform(_ action: PendingAction) {
        Task {
            let result: EndResult
            switch action.kind {
            case .end: result = await store.end(action.row, force: false)
            case .forceQuit: result = await store.end(action.row, force: true)
            case .endTree: result = await store.endTree(action.row)
            }
            if result != .ok && result != .notFound { failure = result.message }
        }
    }

    private func copy(_ text: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }
}

/// Offers the privileged helper while system processes are unreadable.
struct FullAccessBanner: View {
    @Environment(SystemStore.self) private var store

    var body: some View {
        let restricted = store.sample.restrictedCount
        switch store.fullAccess {
        case .on:
            EmptyView()
        case .failed(let message):
            ActionBanner(
                symbol: "exclamationmark.triangle.fill", title: "Full access stopped", message: message,
                actionTitle: "Try Again", tone: .warning
            ) { Task { await store.enableFullAccess() } }
        case .needsApproval:
            ActionBanner(
                symbol: "lock.shield.fill", title: "Allow the Procyon helper",
                message:
                    "Turn on Procyon in System Settings → General → Login Items. Procyon connects as soon as you do, and won't ask again.",
                actionTitle: "Open System Settings"
            ) { store.openHelperApproval() }
        case .off, .starting:
            if restricted > 0 {
                ActionBanner(
                    symbol: "lock.shield.fill",
                    title: "\(restricted) system processes are locked",
                    message: store.usesBackgroundHelper
                        ? "Their CPU, memory and disk usage need administrator access. Allow the Procyon helper once and it stays available."
                        : "Their CPU, memory and disk usage need administrator access. A small helper runs only while Procyon is open.",
                    actionTitle: store.fullAccess == .starting ? "Waiting…" : "Unlock Full Access",
                    isBusy: store.fullAccess == .starting
                ) { Task { await store.enableFullAccess() } }
            }
        }
    }
}

private struct PendingAction: Identifiable {
    enum Kind { case end, forceQuit, endTree }

    let kind: Kind
    let row: ProcessRow
    var id: String { "\(kind)-\(row.id)" }

    var title: String {
        switch kind {
        case .end: row.kind == .group ? "End all \(row.processCount) “\(row.name)” processes?" : "End “\(row.name)”?"
        case .forceQuit: "Force quit “\(row.name)”?"
        case .endTree: "End “\(row.name)” and all its child processes?"
        }
    }

    var confirmTitle: String {
        switch kind {
        case .end: "End Task"
        case .forceQuit: "Force Quit"
        case .endTree: "End Process Tree"
        }
    }

    var message: String {
        var text =
            switch kind {
            case .end: "The process will be asked to quit."
            case .forceQuit: "The process stops immediately. Unsaved changes will be lost."
            case .endTree: "Every descendant process stops immediately. Unsaved changes will be lost."
            }
        if row.isSystem { text += "\n\n“\(row.name)” is a system process. Ending it can make macOS unstable." }
        return text
    }
}

private struct NameCell: View {
    let row: ProcessRow
    let expanded: Bool
    let pinned: Bool
    let unpin: () -> Void
    let toggle: () -> Void

    var body: some View {
        HStack(spacing: Tokens.Space.xs + 2) {
            Color.clear.frame(width: CGFloat(row.depth) * 16)
            Group {
                if row.hasChildren {
                    Button(action: toggle) {
                        Image(systemName: "chevron.right")
                            .font(.system(size: 9, weight: .bold))
                            .foregroundStyle(Tokens.Palette.textTertiary)
                            .rotationEffect(.degrees(expanded ? 90 : 0))
                            .frame(width: 14, height: 14)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(expanded ? "Collapse" : "Expand")
                } else {
                    Color.clear
                }
            }
            .frame(width: 14)

            ProcessIcon(row: row, size: 18)
            Text(row.name)
                .font(row.kind == .group ? Tokens.Typography.headline : Tokens.Typography.body)
                .foregroundStyle(row.isRestricted ? Tokens.Palette.textSecondary : Tokens.Palette.textPrimary)
                .lineLimit(1)
                .truncationMode(.middle)
            if row.kind == .group {
                Text("\(row.processCount)")
                    .font(Tokens.Typography.caption.monospacedDigit())
                    .foregroundStyle(Tokens.Palette.textSecondary)
                    .padding(.horizontal, 5)
                    .padding(.vertical, 1)
                    .background(Tokens.Palette.track, in: Capsule())
            }
            if row.isRestricted {
                Image(systemName: "lock.fill")
                    .font(.system(size: 9))
                    .foregroundStyle(Tokens.Palette.textTertiary)
                    .help("Owned by the system. Unlock full access to read its CPU, memory and disk usage.")
            }
            if pinned {
                Spacer(minLength: 0)
                Button(action: unpin) {
                    Image(systemName: "pin.fill")
                        .font(.system(size: 10, weight: .semibold))
                        .foregroundStyle(Tokens.Palette.accent)
                        .rotationEffect(.degrees(45))
                        .frame(width: 16, height: 16)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .help("Pinned to the top in every view. Click to unpin.")
                .accessibilityLabel("Unpin")
            }
        }
    }
}

/// Task-Manager-style heat cell: background intensity follows usage.
private struct HeatCell: View {
    let text: String
    let intensity: Double
    let metric: Metric
    let restricted: Bool

    var body: some View {
        let heat = min(max(intensity, 0), 1)
        Text(text)
            .font(Tokens.Typography.body.monospacedDigit())
            .foregroundStyle(restricted ? Tokens.Palette.textTertiary : Tokens.Palette.textPrimary)
            .frame(maxWidth: .infinity, alignment: .trailing)
            .padding(.horizontal, Tokens.Space.xs + 2)
            .padding(.vertical, 1)
            .background {
                if heat > 0.01 {
                    RoundedRectangle(cornerRadius: Tokens.Radius.xs, style: .continuous)
                        .fill(metric.style.start.opacity(0.08 + 0.42 * heat))
                }
            }
    }
}

private struct EndTaskButtonStyle: ButtonStyle {
    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(Tokens.Typography.headline)
            .foregroundStyle(isEnabled ? .white : Tokens.Palette.textTertiary)
            .padding(.horizontal, Tokens.Space.md)
            .frame(height: 28)
            .background {
                RoundedRectangle(cornerRadius: Tokens.Radius.sm + 1, style: .continuous)
                    .fill(
                        isEnabled
                            ? AnyShapeStyle(
                                LinearGradient(
                                    colors: [Tokens.Palette.danger, Tokens.Palette.danger.opacity(0.85)], startPoint: .top,
                                    endPoint: .bottom))
                            : AnyShapeStyle(Tokens.Palette.track))
            }
            .shadow(color: isEnabled ? Tokens.Palette.danger.opacity(0.35) : .clear, radius: 6, y: 2)
            .opacity(configuration.isPressed ? 0.8 : 1)
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .animation(.easeOut(duration: Tokens.Motion.fast), value: configuration.isPressed)
    }
}
