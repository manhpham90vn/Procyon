import AppKit
import ProcyonDesign
import ProcyonKit
import SwiftUI

/// Background services (launchd jobs): status, start, stop, restart, enable, disable.
struct ServicesView: View {
    @Environment(SystemStore.self) private var store
    @State private var services: [Service] = []
    @State private var loaded = false
    @State private var selection: Service.ID?
    @State private var search = ""
    @State private var show: Filter = .running
    @State private var domain: DomainFilter = .all
    @State private var sortOrder = [KeyPathComparator(\Service.name, comparator: .localizedStandard)]
    @State private var pending: Pending?
    @State private var failure: String?
    @FocusState private var searchFocused: Bool

    private enum Filter: String, CaseIterable, Identifiable {
        case all = "All", running = "Running", thirdParty = "Third-Party", disabled = "Disabled"
        var id: String { rawValue }
    }

    private enum DomainFilter: String, CaseIterable, Identifiable {
        case all = "All Domains", system = "System", user = "User"
        var id: String { rawValue }
    }

    private struct Pending: Identifiable {
        let service: Service
        let action: ServiceAction
        var id: String { "\(service.id)-\(action.rawValue)" }
    }

    private var visible: [Service] {
        services.filter { service in
            switch show {
            case .all: break
            case .running: if !service.isRunning { return false }
            case .thirdParty: if service.isApple { return false }
            case .disabled: if service.isEnabled { return false }
            }
            switch domain {
            case .all: break
            case .system: if service.domain != .system { return false }
            case .user: if service.domain != .user { return false }
            }
            return search.isEmpty || service.name.localizedCaseInsensitiveContains(search)
                || service.label.localizedCaseInsensitiveContains(search) || service.pid.map { "\($0)" == search } == true
        }
        .sorted(using: sortOrder)
    }

    private var selected: Service? { services.first { $0.id == selection } }

    var body: some View {
        VStack(alignment: .leading, spacing: Tokens.Space.lg) {
            PageHeader("Services", subtitle: summary) {
                Picker("Show", selection: $show) {
                    ForEach(Filter.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .fixedSize()
            }

            HStack(spacing: Tokens.Space.md) {
                SearchField(text: $search, prompt: "Search by name, label or PID", shortcutHint: nil, focus: $searchFocused)
                    .frame(maxWidth: 340)
                Picker("Domain", selection: $domain) {
                    ForEach(DomainFilter.allCases) { Text($0.rawValue).tag($0) }
                }
                .labelsHidden()
                .fixedSize()
                Spacer()
                actionButtons
            }

            if domain != .user {
                FullAccessNotice(
                    title: "System services are read-only",
                    message:
                        "Starting, stopping or switching off a system service needs administrator access. Your own (user) services work without it."
                )
            }

            table
                .overlay {
                    if !loaded {
                        ProgressView()
                    } else if visible.isEmpty {
                        EmptyState(symbol: "magnifyingglass", title: "No services", message: "Nothing matches the filters.")
                    }
                }
                .cardSurface(padding: 0)
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
        .confirmationDialog(
            pending.map { "\($0.action.title) “\($0.service.name)”?" } ?? "",
            isPresented: Binding(get: { pending != nil }, set: { if !$0 { pending = nil } }), presenting: pending
        ) { request in
            Button(request.action.title, role: .destructive) { Task { await perform(request) } }
            Button("Cancel", role: .cancel) {}
        } message: { request in
            Text(confirmationMessage(request))
        }
        .alert("Couldn't change the service", isPresented: Binding(get: { failure != nil }, set: { if !$0 { failure = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(failure ?? "")
        }
    }

    /// System services run as root: changing them waits for full access.
    private func locked(_ service: Service?) -> Bool {
        service?.domain == .system && !store.fullAccess.isOn
    }

    @ViewBuilder
    private var actionButtons: some View {
        let service = selected
        let locked = locked(service)
        Group {
            Button("Start", systemImage: "play.fill") { if let service { request(.start, service) } }
                .disabled(locked || service == nil || service?.isRunning == true || service?.isEnabled == false)
            Button("Stop", systemImage: "stop.fill") { if let service { request(.stop, service) } }
                .disabled(locked || service?.isRunning != true)
            Button("Restart", systemImage: "arrow.clockwise") { if let service { request(.restart, service) } }
                .disabled(locked || service == nil || service?.isEnabled == false)
        }
        .help(locked ? "System services need full access: use Unlock Full Access above" : "")
    }

    private var table: some View {
        Table(visible, selection: $selection, sortOrder: $sortOrder) {
            TableColumn("Name", value: \.name, comparator: .localizedStandard) { service in
                VStack(alignment: .leading, spacing: 0) {
                    HStack(spacing: Tokens.Space.xs) {
                        Text(service.name).font(Tokens.Typography.headline).lineLimit(1)
                        if service.isApple {
                            Image(systemName: "apple.logo")
                                .font(.system(size: 9))
                                .foregroundStyle(Tokens.Palette.textTertiary)
                                .help("Part of macOS")
                        }
                    }
                    Text(service.label).font(Tokens.Typography.caption).foregroundStyle(Tokens.Palette.textTertiary).lineLimit(1)
                }
                .padding(.vertical, 2)
            }
            .width(min: 220, ideal: 340)
            TableColumn("Domain", value: \.domainTitle) { Text($0.domain.title).foregroundStyle(Tokens.Palette.textSecondary) }
                .width(min: 56, ideal: 70)
            TableColumn("Status", value: \.statusRank) { service in
                HStack(spacing: Tokens.Space.xs + 2) {
                    Circle()
                        .fill(
                            service.isRunning
                                ? Tokens.Palette.success
                                : service.isEnabled ? Tokens.Palette.textTertiary : Tokens.Palette.warning
                        )
                        .frame(width: 7, height: 7)
                    Text(service.statusTitle).foregroundStyle(Tokens.Palette.textSecondary).lineLimit(1)
                }
            }
            .width(min: 110, ideal: 150)
            TableColumn("Program", value: \.program) { service in
                Text(service.program.isEmpty ? "—" : service.program)
                    .font(Tokens.Typography.mono)
                    .foregroundStyle(Tokens.Palette.textTertiary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(service.program)
            }
        }
        .contextMenu(forSelectionType: Service.ID.self) { ids in
            if let service = services.first(where: { ids.contains($0.id) }) {
                let locked = locked(service)
                Button("Start") { request(.start, service) }.disabled(locked || service.isRunning || !service.isEnabled)
                Button("Stop") { request(.stop, service) }.disabled(locked || !service.isRunning)
                Button("Restart") { request(.restart, service) }.disabled(locked || !service.isEnabled)
                Divider()
                Button(service.isEnabled ? "Disable" : "Enable") { request(service.isEnabled ? .disable : .enable, service) }
                    .disabled(locked)
                if locked {
                    Text("System services need full access")
                }
                Divider()
                if !service.configPath.isEmpty {
                    Button("Show Definition in Finder") {
                        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: service.configPath)])
                    }
                }
                Button("Copy Label") { ProcessActionCenter.copy(service.label) }
            }
        }
        .scrollContentBackground(.hidden)
    }

    private var summary: String {
        guard loaded else { return "Loading…" }
        let running = services.filter(\.isRunning).count
        return "\(services.count) services · \(running) running · \(services.filter { !$0.isApple }.count) third-party"
    }

    // MARK: - Actions

    private func request(_ action: ServiceAction, _ service: Service) {
        let request = Pending(service: service, action: action)
        guard !locked(service) else { return }
        if action == .start || action == .enable {
            Task { await perform(request) }
        } else {
            pending = request
        }
    }

    private func confirmationMessage(_ request: Pending) -> String {
        var text =
            switch request.action {
            case .stop: "The service gets a termination signal. launchd may start it again if it is set to stay alive."
            case .restart: "The running copy is killed and started again."
            case .disable: "It won't start again until you enable it, including after a restart."
            case .start, .enable: ""
            }
        if request.service.isApple { text += "\n\nThis service is part of macOS. Changing it can break system features." }
        return text
    }

    private func perform(_ request: Pending) async {
        let result = await store.control(request.service, request.action)
        if result.isFailure {
            failure =
                result == .protected
                ? "macOS protects this service (System Integrity Protection); it can't be changed." : result.message
        }
        await load()
    }

    private func load() async {
        services = await store.services()
        loaded = true
    }
}

extension Service {
    var domainTitle: String { domain.title }
    /// Running first, then enabled, then disabled.
    var statusRank: Int { isRunning ? 2 : isEnabled ? 1 : 0 }
}
