import AppKit
import ProcyonDesign
import ProcyonKit
import SwiftUI

/// Get Info: path, command line, environment and threads of one process.
struct ProcessInfoSheet: View {
    let row: ProcessRow
    @Environment(SystemStore.self) private var store
    @Environment(\.dismiss) private var dismiss
    @State private var details: ProcessDetails?
    @State private var loaded = false
    @State private var tab: Tab = .general
    @State private var filter = ""

    private enum Tab: String, CaseIterable, Identifiable {
        case general = "General", environment = "Environment", threads = "Threads"
        var id: String { rawValue }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Tokens.Space.lg) {
            header
            Picker("Section", selection: $tab) {
                ForEach(Tab.allCases) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            Group {
                if !loaded {
                    ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if let details {
                    switch tab {
                    case .general: general(details)
                    case .environment: environment(details)
                    case .threads: threads(details)
                    }
                } else {
                    EmptyState(
                        symbol: "xmark.octagon", title: "The process has exited",
                        message: "“\(row.name)” (PID \(row.pid)) is no longer running.")
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)

            HStack {
                Button("Copy Info") { ProcessActionCenter.copy(summary) }
                    .disabled(details == nil)
                Spacer()
                Button("Refresh") { Task { await load() } }
                Button("Done") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(Tokens.Space.xl)
        .frame(width: 680, height: 540)
        .task { await load() }
    }

    private var header: some View {
        HStack(spacing: Tokens.Space.md) {
            ProcessIcon(row: row, size: 40)
            VStack(alignment: .leading, spacing: 2) {
                Text(row.name).font(Tokens.Typography.title).foregroundStyle(Tokens.Palette.textPrimary)
                Text("PID \(row.pid) · \(row.user)" + (row.kind == .group ? " · main process of \(row.processCount)" : ""))
                    .font(Tokens.Typography.body)
                    .foregroundStyle(Tokens.Palette.textSecondary)
            }
            Spacer()
            if let state = details?.state, state != .unknown {
                Badge(state.title, tone: state == .stopped ? .warning : state == .running ? .success : .neutral)
            }
            if !row.path.isEmpty {
                Button("Show in Finder", systemImage: "folder") { ProcessActionCenter.showInFinder(row) }
            }
        }
    }

    // MARK: - Tabs

    private func general(_ d: ProcessDetails) -> some View {
        ScrollView {
            Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: Tokens.Space.lg, verticalSpacing: Tokens.Space.sm) {
                field("Path", d.path.isEmpty ? Format.unavailable : d.path, mono: true)
                field("Working directory", d.workingDirectory.isEmpty ? Format.unavailable : d.workingDirectory, mono: true)
                field("Command line", d.commandLine ?? "Needs full access", mono: d.commandLine != nil)
                Divider().gridCellUnsizedAxes(.horizontal)
                field("PID", "\(d.pid)")
                field("Parent PID", d.parentPID > 0 ? "\(d.parentPID)" : Format.unavailable)
                field("User", d.user)
                field("Started", d.startTime.map { $0.formatted(date: .abbreviated, time: .standard) } ?? Format.unavailable)
                field("Running for", d.runningTime.map(Format.duration) ?? Format.unavailable)
                field("Priority", "\(ProcessPriority(nice: d.nice).title) (nice \(d.nice))")
                field("State", d.state.title)
                field("Threads", d.threads.map { "\($0.count)" } ?? Format.count(row.threads))
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            restrictedNote(d.arguments == nil)
        }
    }

    @ViewBuilder
    private func field(_ title: String, _ value: String, mono: Bool = false) -> some View {
        GridRow {
            Text(title)
                .font(Tokens.Typography.label)
                .foregroundStyle(Tokens.Palette.textSecondary)
                .gridColumnAlignment(.trailing)
            Text(value)
                .font(mono ? Tokens.Typography.mono : Tokens.Typography.body)
                .foregroundStyle(Tokens.Palette.textPrimary)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private struct NumberedThread: Identifiable {
        let number: Int
        let thread: ThreadInfo
        var id: UInt64 { thread.id }
    }

    private struct Variable: Identifiable {
        let id: Int
        let key: String
        let value: String
    }

    @ViewBuilder
    private func environment(_ d: ProcessDetails) -> some View {
        if let environment = d.environment {
            let variables = environment.enumerated().map { index, entry in
                let parts = entry.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
                return Variable(id: index, key: String(parts.first ?? ""), value: parts.count > 1 ? String(parts[1]) : "")
            }
            .filter {
                filter.isEmpty || $0.key.localizedCaseInsensitiveContains(filter)
                    || $0.value.localizedCaseInsensitiveContains(filter)
            }
            VStack(alignment: .leading, spacing: Tokens.Space.sm) {
                TextField("Filter", text: $filter).textFieldStyle(.roundedBorder).frame(maxWidth: 260)
                Table(variables) {
                    TableColumn("Variable") { Text($0.key).font(Tokens.Typography.mono).textSelection(.enabled) }
                        .width(min: 120, ideal: 180)
                    TableColumn("Value") { Text($0.value).font(Tokens.Typography.mono).textSelection(.enabled) }
                }
            }
        } else {
            restrictedNote(true)
        }
    }

    @ViewBuilder
    private func threads(_ d: ProcessDetails) -> some View {
        if let threads = d.threads {
            let numbered = threads.enumerated().map { NumberedThread(number: $0.offset + 1, thread: $0.element) }
            Table(numbered) {
                TableColumn("#") { Text("\($0.number)").monospacedDigit() }.width(36)
                TableColumn("Name") { Text($0.thread.name.isEmpty ? "—" : $0.thread.name).lineLimit(1) }
                TableColumn("State") { Text($0.thread.state.title) }.width(80)
                TableColumn("CPU") { Text(Format.cpu($0.thread.cpu)).monospacedDigit() }.width(60)
                TableColumn("User time") { Text(Self.seconds($0.thread.userTime)).monospacedDigit() }.width(80)
                TableColumn("System time") { Text(Self.seconds($0.thread.systemTime)).monospacedDigit() }.width(80)
                TableColumn("Priority") { Text("\($0.thread.priority)").monospacedDigit() }.width(56)
            }
        } else {
            restrictedNote(true)
        }
    }

    @ViewBuilder
    private func restrictedNote(_ restricted: Bool) -> some View {
        if restricted {
            VStack(alignment: .leading, spacing: Tokens.Space.sm) {
                InfoBanner(
                    store.fullAccess.isOn
                        ? "macOS doesn't share this for “\(row.name)”, even with full access."
                        : "This process belongs to another user. Unlock Full Access to read its command line, environment and threads.",
                    symbol: "lock.shield", tone: .warning)
                if !store.fullAccess.isOn {
                    Button("Unlock Full Access…") {
                        Task {
                            await store.enableFullAccess()
                            await load()
                        }
                    }
                }
            }
            .padding(.top, Tokens.Space.md)
        }
    }

    // MARK: - Data

    private func load() async {
        details = await store.details(pid: row.pid)
        loaded = true
    }

    private static func seconds(_ value: TimeInterval) -> String {
        value >= 60 ? Format.duration(value) : String(format: "%.2fs", value)
    }

    /// Plain-text report for the clipboard.
    private var summary: String {
        guard let d = details else { return "" }
        var lines = [
            "\(d.name) (PID \(d.pid))", "Path: \(d.path)", "Working directory: \(d.workingDirectory)",
            "Command line: \(d.commandLine ?? "unavailable")", "Parent PID: \(d.parentPID)", "User: \(d.user)",
            "Priority: nice \(d.nice)", "State: \(d.state.title)",
        ]
        if let start = d.startTime { lines.append("Started: \(start.formatted(date: .abbreviated, time: .standard))") }
        if let threads = d.threads { lines.append("Threads: \(threads.count)") }
        return lines.joined(separator: "\n")
    }
}
