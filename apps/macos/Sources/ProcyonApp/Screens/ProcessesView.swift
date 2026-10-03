import AppKit
import ProcyonDesign
import ProcyonKit
import SwiftUI

/// Processes screen state that outlives the screen, so leaving the page and coming back keeps the
/// selection and the expanded rows, and a ⌘F pressed on another page reaches the search field.
@MainActor
@Observable
final class ProcessesUIState {
    var selection: ProcessRow.ID?
    /// Rows whose expansion differs from the current mode's default.
    var toggled: Set<ProcessRow.ID> = []
    /// The view mode `toggled` belongs to.
    private(set) var toggledMode: ViewMode?
    /// Set by Find Process (⌘F); the Processes screen focuses its search field and clears it.
    var wantsSearchFocus = false

    /// A new view mode starts from its own expansion defaults.
    func syncExpansion(to mode: ViewMode) {
        guard toggledMode != mode else { return }
        toggledMode = mode
        toggled = []
    }
}

extension ProcessRow {
    /// Whether the row itself matches the search (an app group by its name; a process by name, pid or
    /// user), as opposed to being listed for context as a member or an ancestor of a match.
    func matches(search text: String) -> Bool {
        if name.localizedCaseInsensitiveContains(text) { return true }
        guard kind == .process else { return false }
        return String(pid).contains(text) || user.localizedCaseInsensitiveContains(text)
    }
}

struct ProcessesView: View {
    @Environment(SystemStore.self) private var store
    @Environment(ProcessesUIState.self) private var ui
    @Environment(ProcessActionCenter.self) private var actionCenter
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
        guard let selection = ui.selection else { return nil }
        return store.rows.first { $0.id == selection }
    }

    /// Rows that match the search themselves, not the members and ancestors shown with them.
    private var matchCount: Int {
        store.rows.count { $0.matches(search: store.filter) }
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
                    let matches = matchCount
                    Text(matches == 1 ? "1 match" : "\(matches) matches")
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
                Button("Get Info", systemImage: "info.circle") {
                    if let row = selectedRow { actionCenter.inspect(row) }
                }
                .labelStyle(.iconOnly)
                .buttonStyle(.borderless)
                .disabled(selectedRow == nil)
                .help("Path, command line, environment and threads of the selected process (⌘I)")
                Button(role: .destructive) {
                    if let row = selectedRow { actionCenter.request(.end, row) }
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
        .onAppear { ui.syncExpansion(to: store.viewMode) }
        .onChange(of: store.viewMode) {
            ui.syncExpansion(to: store.viewMode)
            // The selected app stays selected and moves to the top in the new view.
            if let row = selectedRow { store.pinnedAppID = row.appID }
            reselectPinned = store.pinnedAppID != nil
        }
        .onChange(of: ui.selection) { unpinIfElsewhere() }
        // ⌘F from any page: the request waits in `ui` until this screen is there to take it.
        .onChange(of: ui.wantsSearchFocus, initial: true) {
            guard ui.wantsSearchFocus else { return }
            ui.wantsSearchFocus = false
            // Let the field appear before focusing it.
            DispatchQueue.main.async { searchFocused = true }
        }
        .focusedSceneValue(\.processActions, actions)
    }

    private var hasNetwork: Bool { store.capabilities.contains(.processNetwork) }

    private var table: some View {
        @Bindable var ui = ui
        return ProcessTable(
            rows: visibleRows, selection: $ui.selection, sortColumn: store.sortColumn, sortDescending: store.sortDescending,
            hasNetwork: hasNetwork, hasGPU: store.capabilities.contains(.processGPU),
            hasPower: store.capabilities.contains(.processEnergy),
            memoryTotal: Double(max(store.sample.memoryTotal, 1)), controller: tableController,
            isExpanded: isExpanded, isPinnedRoot: isPinnedRoot,
            onSort: { store.sort(by: $0, descending: $1) },
            onToggle: toggle, onUnpin: { store.pinnedAppID = nil }, menuItems: actionCenter.menuItems(for:)
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

    // MARK: - Summary

    private var summary: String {
        guard store.hasSample else { return "Collecting…" }
        let s = store.sample
        return
            "\(Format.count(s.processCount)) processes · \(Format.count(s.threadCount)) threads · CPU \(Format.percent(s.cpuUsage)) · Memory \(Format.bytes(s.memoryUsed))"
    }

    // MARK: - Expansion

    private func isExpanded(_ row: ProcessRow) -> Bool {
        store.viewMode.expandsByDefault != ui.toggled.contains(row.id)
    }

    private func toggle(_ row: ProcessRow) {
        withAnimation(.snappy(duration: Tokens.Motion.fast)) {
            if ui.toggled.contains(row.id) { ui.toggled.remove(row.id) } else { ui.toggled.insert(row.id) }
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
        ui.selection = target.id
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
        guard !reselectPinned, let app = store.pinnedAppID, let selection = ui.selection else { return }
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
        ui.selection = first.id
    }

    private var allExpanded: Bool {
        store.rows.contains { $0.hasChildren } && store.rows.allSatisfy { !$0.hasChildren || isExpanded($0) }
    }

    private func setAll(expanded: Bool) {
        let parents = store.rows.filter(\.hasChildren).map(\.id)
        withAnimation(.snappy(duration: Tokens.Motion.fast)) {
            ui.toggled = expanded == store.viewMode.expandsByDefault ? [] : Set(parents)
        }
    }

    // MARK: - Actions

    private var actions: ProcessActions {
        ProcessActions(
            row: selectedRow, canSuspend: store.capabilities.contains(.suspend), searchFocused: searchFocused,
            center: actionCenter)
    }
}

/// Says up front what needs full access on a page and offers to unlock it, so a control never fails
/// (or prompts) out of the blue. Hidden once full access is on.
struct FullAccessNotice: View {
    let title: String
    let message: String
    @Environment(SystemStore.self) private var store

    var body: some View {
        switch store.fullAccess {
        case .on:
            EmptyView()
        case .failed(let error):
            ActionBanner(
                symbol: "exclamationmark.triangle.fill", title: "Full access stopped", message: error,
                actionTitle: "Try Again", tone: .warning
            ) { Task { await store.enableFullAccess() } }
        case .needsApproval:
            ActionBanner(
                symbol: "lock.shield.fill", title: "Allow the Procyon helper",
                message: "Turn on Procyon in System Settings → General → Login Items, then come back here.",
                actionTitle: "Open System Settings"
            ) { store.openHelperApproval() }
        case .off, .starting:
            ActionBanner(
                symbol: "lock.shield.fill", title: title, message: message,
                actionTitle: store.fullAccess == .starting ? "Waiting…" : "Unlock Full Access",
                isBusy: store.fullAccess == .starting
            ) { Task { await store.enableFullAccess() } }
        }
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
