import AppKit
import ProcyonDesign
import ProcyonKit
import SwiftUI

/// Apps and programs that start with the Mac or at login: launchd agents and daemons (switchable
/// here) and what apps registered with macOS (Open at Login, background items; switched in System
/// Settings).
struct StartupView: View {
    @Environment(SystemStore.self) private var store
    /// launchd jobs, re-read every few seconds.
    @State private var jobs: [StartupItem] = []
    /// Items registered with macOS; reading them takes seconds, so less often.
    @State private var managed: [StartupItem] = []
    @State private var loadingManaged = false
    /// Apps' login and background items aren't shown until full access is on.
    @State private var managedHidden = false
    @State private var usage: [Int32: ProcessSummary] = [:]
    @State private var loaded = false
    @State private var selection: StartupItem.ID?
    @State private var sortOrder = [KeyPathComparator(\Entry.name, comparator: .localizedStandard)]
    @State private var failure: String?

    /// A table row: the item with its current impact.
    struct Entry: Identifiable {
        let item: StartupItem
        let impact: StartupImpact
        var id: String { item.id }
        var name: String { item.name }
        var scopeTitle: String { item.scope.title }
        var enabledRank: Int { item.isEnabled ? 1 : 0 }
        var runningRank: Int { item.pid == nil ? 0 : 1 }
    }

    private var items: [StartupItem] { jobs + managed }

    private var entries: [Entry] {
        items.map { item in
            let process = item.pid.flatMap { usage[$0] }
            return Entry(
                item: item, impact: StartupImpact(memory: process?.memory, cpu: process?.cpu, isRunning: item.pid != nil))
        }
        .sorted(using: sortOrder)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Tokens.Space.lg) {
            PageHeader("Startup", subtitle: summary) {
                Button("Login Items Settings…", systemImage: "gear") { Self.openLoginItemsSettings() }
                    .help("Apps that open at login and apps allowed in the background are managed in System Settings")
                if loadingManaged { ProgressView().controlSize(.small) }
                Button("Refresh", systemImage: "arrow.clockwise") {
                    Task {
                        await load()
                        await loadManaged()
                    }
                }
                .labelStyle(.iconOnly)
                .help("Refresh")
            }

            if managedHidden || jobs.contains(where: needsFullAccess) {
                FullAccessNotice(
                    title: "Some startup items are hidden or locked",
                    message:
                        "macOS shares apps' login and background items, and lets you switch items that start with your Mac, only with administrator access."
                )
            }

            table
                .overlay {
                    if !loaded {
                        ProgressView()
                    } else if items.isEmpty {
                        EmptyState(
                            symbol: "power.circle", title: "Nothing starts automatically",
                            message: "No third-party launch agents or daemons are installed.")
                    }
                }
                .cardSurface(padding: 0)

            Text(
                "Turning an item off takes effect the next time it would start (login or restart); a running copy keeps running. Items that start at startup run as root and need full access to change. Apps that open at login and apps' background items can only be switched in System Settings → General → Login Items."
            )
            .font(Tokens.Typography.caption)
            .foregroundStyle(Tokens.Palette.textTertiary)
        }
        .padding(.horizontal, Tokens.Space.xxl)
        .padding(.top, Tokens.Space.lg)
        .padding(.bottom, Tokens.Space.xl)
        .task {
            while !Task.isCancelled {
                await load()
                try? await Task.sleep(for: .seconds(5))
            }
        }
        .task(id: store.fullAccess.isOn) {
            while !Task.isCancelled {
                await loadManaged()
                try? await Task.sleep(for: .seconds(30))
            }
        }
        .alert(
            "Couldn't change the startup item", isPresented: Binding(get: { failure != nil }, set: { if !$0 { failure = nil } })
        ) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(failure ?? "")
        }
    }

    /// Daemons run as root: switching them waits for full access.
    private func needsFullAccess(_ item: StartupItem) -> Bool {
        !item.isManagedByOS && item.scope.domain == .system && !store.fullAccess.isOn
    }

    private var table: some View {
        Table(entries, selection: $selection, sortOrder: $sortOrder) {
            TableColumn("Name", value: \.name, comparator: .localizedStandard) { entry in
                HStack(spacing: Tokens.Space.sm) {
                    StartupIcon(appPath: entry.item.appPath)
                    VStack(alignment: .leading, spacing: 0) {
                        Text(entry.item.name).font(Tokens.Typography.headline).lineLimit(1)
                        Text(
                            entry.item.parentName.isEmpty || entry.item.parentName == entry.item.name
                                ? entry.item.label : "\(entry.item.parentName) · \(entry.item.label)"
                        )
                        .font(Tokens.Typography.caption)
                        .foregroundStyle(Tokens.Palette.textTertiary)
                        .lineLimit(1)
                    }
                }
                .padding(.vertical, 2)
            }
            .width(min: 220, ideal: 320)
            TableColumn("Starts", value: \.scopeTitle) { Text($0.scopeTitle).foregroundStyle(Tokens.Palette.textSecondary) }
                .width(min: 90, ideal: 110)
            TableColumn("Status", value: \.runningRank) { entry in
                if let pid = entry.item.pid {
                    Text("Running · PID \(pid)").foregroundStyle(Tokens.Palette.textSecondary)
                } else {
                    Text("Not running").foregroundStyle(Tokens.Palette.textTertiary)
                }
            }
            .width(min: 100, ideal: 130)
            TableColumn("Impact", value: \.impact) { entry in
                ImpactBadge(impact: entry.impact)
            }
            .width(min: 70, ideal: 90)
            TableColumn("Enabled", value: \.enabledRank) { entry in
                Toggle(
                    "Enabled",
                    isOn: Binding(get: { entry.item.isEnabled }, set: { _ in Task { await toggle(entry.item) } })
                )
                .toggleStyle(.switch)
                .controlSize(.small)
                .labelsHidden()
                .disabled(entry.item.isManagedByOS || needsFullAccess(entry.item))
                .help(
                    entry.item.isManagedByOS
                        ? "Switch this in System Settings → General → Login Items"
                        : needsFullAccess(entry.item)
                            ? "Needs full access: use Unlock Full Access above"
                            : entry.item.isEnabled ? "Don't start automatically" : "Start automatically")
            }
            .width(64)
        }
        .contextMenu(forSelectionType: StartupItem.ID.self) { ids in
            if let item = items.first(where: { ids.contains($0.id) }) {
                if item.isManagedByOS {
                    Button("Manage in Login Items Settings…") { Self.openLoginItemsSettings() }
                } else {
                    Button(item.isEnabled ? "Disable" : "Enable") { Task { await toggle(item) } }
                        .disabled(needsFullAccess(item))
                }
                Divider()
                if let app = item.appPath {
                    Button("Show App in Finder") { reveal(app) }
                }
                if !item.configPath.isEmpty { Button("Show Definition in Finder") { reveal(item.configPath) } }
                Button("Copy Label") { ProcessActionCenter.copy(item.label) }
                if !item.program.isEmpty { Button("Copy Program Path") { ProcessActionCenter.copy(item.program) } }
            }
        }
        .scrollContentBackground(.hidden)
    }

    private var summary: String {
        guard loaded else { return "Loading…" }
        let enabled = items.filter(\.isEnabled).count
        return "\(items.count) items · \(enabled) enabled · \(items.filter { $0.pid != nil }.count) running"
    }

    private func load() async {
        jobs = await store.startupItems()
        usage = await store.summaries(for: Set(items.compactMap(\.pid)))
        loaded = true
    }

    private func loadManaged() async {
        guard store.fullAccess.isOn else {
            managed = []
            managedHidden = true
            return
        }
        loadingManaged = true
        let list = await store.managedStartupItems()
        managedHidden = list == nil
        managed = list ?? []
        usage = await store.summaries(for: Set(items.compactMap(\.pid)))
        loadingManaged = false
    }

    static func openLoginItemsSettings() {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension")!)
    }

    private func toggle(_ item: StartupItem) async {
        if item.isManagedByOS {
            Self.openLoginItemsSettings()
            return
        }
        guard !needsFullAccess(item) else { return }
        let result = await store.setEnabled(item, !item.isEnabled)
        if result.isFailure { failure = result.message }
        await load()
    }

    private func reveal(_ path: String) {
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
    }
}

struct StartupIcon: View {
    let appPath: String?

    var body: some View {
        if let appPath {
            Image(nsImage: IconCache.icon(for: appPath, size: 22)).frame(width: 22, height: 22)
        } else {
            Image(systemName: "terminal.fill")
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(Tokens.Palette.textTertiary)
                .frame(width: 22, height: 22)
                .background(Tokens.Palette.surfaceSunken, in: RoundedRectangle(cornerRadius: 5, style: .continuous))
        }
    }
}

private struct ImpactBadge: View {
    let impact: StartupImpact

    var body: some View {
        switch impact {
        case .notRunning: Text("—").foregroundStyle(Tokens.Palette.textTertiary)
        case .low: Badge("Low", tone: .success)
        case .medium: Badge("Medium", tone: .warning)
        case .high: Badge("High", tone: .danger)
        }
    }
}
