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
    @FocusState private var searchFocused: Bool
    @State private var tableController = ProcessTableController()
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

            table
                .onChange(of: store.focusedRow?.id, initial: true) { revealFocusedRow(final: false) }
                // Clearing the search rebuilds the rows; the wanted row may only appear then.
                .onChange(of: store.rows) {
                    revealFocusedRow(final: true)
                    keepPinnedSelection()
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
        ProcessTable(
            rows: visibleRows, selection: $selection, sortColumn: store.sortColumn, sortDescending: store.sortDescending,
            hasNetwork: hasNetwork, memoryTotal: Double(max(store.sample.memoryTotal, 1)), controller: tableController,
            isExpanded: isExpanded, isPinnedRoot: isPinnedRoot,
            onSort: { store.sort(by: $0, descending: $1) },
            onToggle: toggle, onUnpin: { store.pinnedAppID = nil }, menuItems: menuItems
        )
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
    }

    private func menuItems(for row: ProcessRow) -> [NSMenuItem] {
        var items: [NSMenuItem] = [
            ActionMenuItem(row.kind == .group ? "End \(row.processCount) Processes" : "End Task", isEnabled: !row.isProtected) {
                request(.end, row)
            },
            ActionMenuItem("Force Quit", isEnabled: !row.isProtected) { request(.forceQuit, row) },
        ]
        if row.kind == .process && row.hasChildren {
            items.append(ActionMenuItem("End Process Tree", isEnabled: !row.isProtected) { request(.endTree, row) })
        }
        items.append(.separator())
        if !row.path.isEmpty {
            items.append(
                ActionMenuItem("Show in Finder") {
                    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: row.bundlePath ?? row.path)])
                })
        }
        items.append(ActionMenuItem("Copy Name") { copy(row.name) })
        items.append(ActionMenuItem("Copy PID") { copy(String(row.pid)) })
        if !row.path.isEmpty { items.append(ActionMenuItem("Copy Path") { copy(row.path) }) }
        return items
    }

    // MARK: - Summary

    private var summary: String {
        guard store.hasSample else { return "Collecting…" }
        let s = store.sample
        return
            "\(Format.count(s.processCount)) processes · \(Format.count(s.threadCount)) threads · CPU \(Format.percent(s.cpuUsage)) · Memory \(Format.bytes(s.memoryUsed))"
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
    private func revealFocusedRow(final: Bool) {
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
        // After the expanded rows reach the table.
        Task { tableController.reveal(target.id) }
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
