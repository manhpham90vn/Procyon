import AppKit
import ProcyonDesign
import ProcyonKit
import SwiftUI

/// ⌘K: type to jump to a screen, run a command, or find an app and act on it (end, suspend,
/// change priority, open its location) without leaving the keyboard.
struct CommandPalette: View {
    @Binding var page: Page
    @Binding var isPresented: Bool
    @Environment(SystemStore.self) private var store
    @Environment(ProcessActionCenter.self) private var actions
    @AppStorage(MenuBarSettings.enabledKey) private var menuBarEnabled = true

    @State private var query = ""
    /// The app whose actions are listed; nil on the first level.
    @State private var target: ProcessRow?
    @State private var apps: [ProcessRow] = []
    @State private var highlighted = 0
    @FocusState private var focused: Bool

    struct Item: Identifiable {
        let id: String
        let title: String
        var subtitle: String?
        var symbol: String
        var row: ProcessRow?
        var isDestructive = false
        /// Closes the palette unless it opens a second level.
        let run: () -> Void
    }

    var body: some View {
        ZStack(alignment: .top) {
            Color.black.opacity(0.18)
                .ignoresSafeArea()
                .onTapGesture { close() }

            VStack(spacing: 0) {
                field
                Divider()
                results
            }
            .frame(width: 580)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: Tokens.Radius.xl, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: Tokens.Radius.xl, style: .continuous).strokeBorder(Tokens.Palette.borderStrong)
            )
            .shadow(color: .black.opacity(0.25), radius: 24, y: 12)
            .padding(.top, 90)
        }
        .onAppear { focused = true }
        .task(id: query) { await searchApps() }
        .onChange(of: query) { highlighted = 0 }
    }

    private var field: some View {
        HStack(spacing: Tokens.Space.sm) {
            if let target {
                Button {
                    back()
                } label: {
                    HStack(spacing: Tokens.Space.xs) {
                        ProcessIcon(row: target, size: 16)
                        Text(target.name).lineLimit(1)
                        Image(systemName: "xmark").font(.system(size: 8, weight: .bold))
                    }
                    .font(Tokens.Typography.label)
                    .padding(.horizontal, Tokens.Space.sm)
                    .padding(.vertical, 3)
                    .background(Tokens.Palette.accent.opacity(0.14), in: Capsule())
                }
                .buttonStyle(.plain)
                .help("Back to all commands")
            } else {
                Image(systemName: "command").foregroundStyle(Tokens.Palette.textTertiary)
            }
            TextField(target == nil ? "Search commands and apps" : "Search actions", text: $query)
                .textFieldStyle(.plain)
                .font(.system(size: 17))
                .focused($focused)
                .onKeyPress(.downArrow) { move(1) }
                .onKeyPress(.upArrow) { move(-1) }
                .onKeyPress(.return) {
                    runHighlighted()
                    return .handled
                }
                .onKeyPress(.escape) {
                    if target != nil { back() } else { close() }
                    return .handled
                }
                .onKeyPress(.delete) {
                    guard query.isEmpty, target != nil else { return .ignored }
                    back()
                    return .handled
                }
            KeyCap("esc")
        }
        .padding(.horizontal, Tokens.Space.lg)
        .padding(.vertical, Tokens.Space.md)
    }

    private var results: some View {
        let items = self.items
        return ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    if items.isEmpty {
                        Text("No matches")
                            .font(Tokens.Typography.body)
                            .foregroundStyle(Tokens.Palette.textTertiary)
                            .frame(maxWidth: .infinity, minHeight: 60)
                    }
                    ForEach(Array(items.enumerated()), id: \.element.id) { index, item in
                        PaletteRow(item: item, isHighlighted: index == highlighted)
                            .id(index)
                            .onTapGesture { item.run() }
                            .onHover { if $0 { highlighted = index } }
                    }
                }
                .padding(Tokens.Space.sm)
            }
            .frame(maxHeight: 380)
            .fixedSize(horizontal: false, vertical: true)
            .onChange(of: highlighted) { _, index in proxy.scrollTo(index) }
        }
    }

    // MARK: - Items

    private var items: [Item] {
        let list = target.map(processItems(for:)) ?? (commandItems + appItems)
        guard !query.isEmpty else { return list }
        // Apps are already filtered by the core; filter commands here.
        return list.filter { $0.row != nil && target == nil || matches($0) }
    }

    private func matches(_ item: Item) -> Bool {
        let words = query.lowercased().split(separator: " ")
        let haystack = (item.title + " " + (item.subtitle ?? "")).lowercased()
        return words.allSatisfy { haystack.contains($0) }
    }

    private var commandItems: [Item] {
        var items = Page.allCases.filter { $0.isAvailable(store.capabilities) }.map { destination in
            Item(
                id: "page-\(destination.rawValue)", title: "Go to \(destination.title)", subtitle: shortcut(destination),
                symbol: destination.symbol.isEmpty ? metricSymbol(destination) : destination.symbol
            ) {
                page = destination
                close()
            }
        }
        items.append(
            Item(
                id: "pause", title: store.isPaused ? "Resume Updates" : "Pause Updates", subtitle: "⇧⌘P",
                symbol: store.isPaused ? "play.fill" : "pause.fill"
            ) {
                store.isPaused.toggle()
                close()
            })
        for interval in SystemStore.refreshIntervals where interval != store.interval {
            items.append(
                Item(id: "speed-\(interval)", title: "Update Every \(Format.interval(interval))", symbol: "speedometer") {
                    store.interval = interval
                    close()
                })
        }
        if !store.fullAccess.isOn {
            items.append(
                Item(id: "access", title: "Unlock Full Access", subtitle: "Read and manage system processes", symbol: "lock.open")
                {
                    close()
                    Task { await store.enableFullAccess() }
                })
        } else {
            items.append(
                Item(
                    id: "access-off", title: "Turn Off Full Access", subtitle: "Stop using the administrator helper",
                    symbol: "lock"
                ) {
                    close()
                    store.disableFullAccess()
                })
        }
        if store.usesBackgroundHelper && store.backgroundHelperRegistered {
            items.append(
                Item(
                    id: "helper-remove", title: "Remove Administrator Helper", subtitle: "Unregister it from Login Items",
                    symbol: "trash"
                ) {
                    close()
                    Task { await store.removeBackgroundHelper() }
                })
        }
        items.append(
            Item(
                id: "menubar", title: menuBarEnabled ? "Quit When the Window Closes" : "Keep in Menu Bar When the Window Closes",
                symbol: "menubar.rectangle"
            ) {
                menuBarEnabled.toggle()
                close()
            })
        return items
    }

    private var appItems: [Item] {
        apps.map { app in
            Item(
                id: "app-\(app.id)", title: app.name,
                subtitle: [
                    app.processCount > 1 ? "\(app.processCount) processes" : "PID \(app.pid)", "CPU \(Format.cpu(app.cpu))",
                    Format.bytes(app.memory),
                ].joined(separator: " · "),
                symbol: "app", row: app
            ) {
                target = app
                query = ""
                highlighted = 0
            }
        }
    }

    private func processItems(for row: ProcessRow) -> [Item] {
        let caps = store.capabilities
        let editable = !row.isProtected
        var items: [Item] = []
        func action(
            _ id: String, _ title: String, _ symbol: String, destructive: Bool = false, _ run: @escaping () -> Void
        )
            -> Item
        {
            Item(id: id, title: title, symbol: symbol, isDestructive: destructive) {
                close()
                run()
            }
        }
        if editable {
            items.append(
                action(
                    "end", row.kind == .group ? "End All \(row.processCount) Processes" : "End Task", "xmark.octagon",
                    destructive: true
                ) {
                    actions.request(.end, row)
                })
            items.append(
                action("force", "Force Quit", "bolt.horizontal.circle", destructive: true) { actions.request(.forceQuit, row) })
            if caps.contains(.suspend) {
                items.append(
                    row.isSuspended
                        ? action("resume", "Resume", "play.circle") { actions.request(.resume, row) }
                        : action("suspend", "Suspend", "pause.circle") { actions.request(.suspend, row) })
            }
            if caps.contains(.priority) {
                for priority in ProcessPriority.allCases where priority.rawValue != row.nice {
                    items.append(
                        action("priority-\(priority.rawValue)", "Set Priority: \(priority.title)", "dial.medium") {
                            actions.request(.priority(priority), row)
                        })
                }
            }
        }
        items.append(action("info", "Get Info", "info.circle") { actions.inspect(row) })
        items.append(
            action("reveal", "Show in Processes", "list.bullet.rectangle") {
                store.showInProcesses(row)
                page = .processes
            })
        if !row.path.isEmpty {
            items.append(action("finder", "Open File Location", "folder") { ProcessActionCenter.showInFinder(row) })
            items.append(action("copy-path", "Copy Path", "doc.on.doc") { ProcessActionCenter.copy(row.path) })
        }
        items.append(action("copy-pid", "Copy PID", "number") { ProcessActionCenter.copy(String(row.pid)) })
        return items
    }

    private func shortcut(_ page: Page) -> String? {
        page == .settings ? "⌘," : page.shortcut.map { "⌘\($0.character)" }
    }

    private func metricSymbol(_ page: Page) -> String {
        switch page {
        case .cpu: Metric.cpu.style.symbol
        case .memory: Metric.memory.style.symbol
        case .disk: Metric.disk.style.symbol
        case .network: Metric.network.style.symbol
        case .gpu: Metric.gpu.style.symbol
        default: "circle"
        }
    }

    // MARK: - Behavior

    private func searchApps() async {
        guard target == nil, !query.trimmingCharacters(in: .whitespaces).isEmpty else {
            apps = []
            return
        }
        try? await Task.sleep(for: .milliseconds(80))  // debounce typing
        guard !Task.isCancelled else { return }
        apps = await store.searchApps(query.trimmingCharacters(in: .whitespaces))
    }

    private func move(_ delta: Int) -> KeyPress.Result {
        let count = items.count
        guard count > 0 else { return .handled }
        highlighted = (highlighted + delta + count) % count
        return .handled
    }

    private func runHighlighted() {
        let items = self.items
        guard items.indices.contains(highlighted) else { return }
        items[highlighted].run()
    }

    private func back() {
        target = nil
        query = ""
        highlighted = 0
    }

    private func close() { isPresented = false }
}

private struct PaletteRow: View {
    let item: CommandPalette.Item
    let isHighlighted: Bool

    var body: some View {
        HStack(spacing: Tokens.Space.md) {
            if let row = item.row {
                ProcessIcon(row: row, size: 20)
            } else {
                Image(systemName: item.symbol)
                    .font(.system(size: 13, weight: .medium))
                    .foregroundStyle(item.isDestructive ? Tokens.Palette.danger : Tokens.Palette.textSecondary)
                    .frame(width: 20)
            }
            Text(item.title)
                .font(Tokens.Typography.body)
                .foregroundStyle(item.isDestructive ? Tokens.Palette.danger : Tokens.Palette.textPrimary)
                .lineLimit(1)
            Spacer()
            if let subtitle = item.subtitle {
                Text(subtitle)
                    .font(Tokens.Typography.label.monospacedDigit())
                    .foregroundStyle(Tokens.Palette.textTertiary)
                    .lineLimit(1)
            }
            if item.row != nil {
                Image(systemName: "chevron.right").font(.system(size: 9, weight: .bold)).foregroundStyle(
                    Tokens.Palette.textTertiary)
            }
        }
        .padding(.horizontal, Tokens.Space.md)
        .padding(.vertical, Tokens.Space.sm)
        .background(
            isHighlighted ? Tokens.Palette.accent.opacity(0.16) : .clear,
            in: RoundedRectangle(cornerRadius: Tokens.Radius.md, style: .continuous)
        )
        .contentShape(Rectangle())
    }
}
