import ProcyonDesign
import ProcyonKit
import SwiftUI

struct SystemView: View {
    @Environment(SystemStore.self) private var store

    var body: some View {
        let info = store.info
        ScreenScroll {
            HStack(spacing: Tokens.Space.xl) {
                Image(systemName: info.deviceSymbol)
                    .font(.system(size: 72, weight: .ultraLight))
                    .foregroundStyle(
                        LinearGradient(
                            colors: [Metric.cpu.style.start, Metric.memory.style.end],
                            startPoint: .topLeading, endPoint: .bottomTrailing)
                    )
                    .shadow(color: Metric.cpu.style.start.opacity(0.3), radius: 18)
                    .frame(width: 120)
                VStack(alignment: .leading, spacing: Tokens.Space.xs) {
                    Text(info.displayModel)
                        .font(Tokens.Typography.display)
                        .foregroundStyle(Tokens.Palette.textPrimary)
                    Text(info.cpuBrand)
                        .font(Tokens.Typography.title)
                        .foregroundStyle(Metric.cpu.style.horizontalGradient)
                    Text("\(info.osName) \(info.osVersion) (\(info.osBuild)) · \(info.hostname)")
                        .font(Tokens.Typography.body)
                        .foregroundStyle(Tokens.Palette.textSecondary)
                        .textSelection(.enabled)
                }
                Spacer()
            }
            .padding(Tokens.Space.xl)
            .cardSurface(radius: Tokens.Radius.xl, padding: 0, tint: Metric.cpu.style.start)

            HStack(alignment: .top, spacing: Tokens.Space.lg) {
                Panel("Hardware", symbol: "cpu") {
                    StatGrid(
                        [
                            StatItem("Model", value: info.displayModel, detail: info.modelID),
                            StatItem("Chip", value: info.cpuBrand, detail: info.architecture),
                            StatItem("Cores", value: info.coreSummary, detail: "\(info.logicalCores) logical"),
                            StatItem("Memory", value: Format.bytes(info.memoryTotal)),
                        ]
                            + (info.cpuFrequencyHz > 0
                                ? [StatItem("Frequency", value: String(format: "%.2f GHz", Double(info.cpuFrequencyHz) / 1e9))]
                                : []),
                        columns: 2)
                }
                Panel("Software", symbol: "apple.logo") {
                    StatGrid(
                        [
                            StatItem(
                                "Operating system", value: "\(info.osName) \(info.osVersion)", detail: "Build \(info.osBuild)"),
                            StatItem("Hostname", value: info.hostname),
                            StatItem(
                                "Uptime", value: Format.duration(store.uptime),
                                detail: "Since \(info.bootTime.formatted(date: .abbreviated, time: .shortened))"),
                            StatItem(
                                "Processes", value: Format.count(store.sample.processCount),
                                detail: "\(Format.count(store.sample.threadCount)) threads"),
                        ], columns: 2)
                }
            }
            .equalHeightPanels()

            Panel("Kernel", symbol: "terminal") {
                Text(info.kernel)
                    .font(Tokens.Typography.mono)
                    .foregroundStyle(Tokens.Palette.textSecondary)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }

            VolumesPanel(volumes: store.volumes)
        }
        .onAppear { store.refreshVolumes() }
    }
}

struct SettingsView: View {
    @Environment(SystemStore.self) private var store
    @AppStorage(Appearance.storageKey) private var appearance: Appearance = .system
    @AppStorage(MenuBarSettings.enabledKey) private var menuBarEnabled = true
    @AppStorage(TemperatureUnit.storageKey) private var temperatureUnit: TemperatureUnit = .system

    var body: some View {
        @Bindable var store = store
        VStack(alignment: .leading, spacing: Tokens.Space.lg) {
            PageHeader("Settings", subtitle: "Updates, full access, menu bar, alerts, appearance, processes and about")
                .padding(.horizontal, Tokens.Space.xxl)
            form
        }
        .padding(.top, Tokens.Space.lg)
    }

    private var form: some View {
        @Bindable var store = store
        return Form {
            Section("Updates") {
                Picker("Update speed", selection: $store.interval) {
                    ForEach(SystemStore.refreshIntervals, id: \.self) { Text("Every \(Format.interval($0))").tag($0) }
                }
            }
            FullAccessSection()
            Section("Menu bar") {
                Toggle("Keep running in the menu bar when the window is closed", isOn: $menuBarEnabled)
                if menuBarEnabled {
                    ForEach(MenuBarSettings.Module.allCases.filter { $0.isAvailable(store.capabilities) }) { module in
                        MenuBarModuleToggle(module: module)
                    }
                }
                Text(
                    menuBarEnabled
                        ? "Closing the window moves Procyon to the menu bar (and out of the Dock), with live figures and the busiest apps one click away. Opening the window hides it from the menu bar again."
                        : "Closing the window quits Procyon."
                )
                .font(.caption)
                .foregroundStyle(.secondary)
            }
            AlertsSection()
            Section("Appearance") {
                Picker("Theme", selection: $appearance) {
                    ForEach(Appearance.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
                Picker("Temperature", selection: $temperatureUnit) {
                    ForEach(TemperatureUnit.allCases) { Text($0.title).tag($0) }
                }
            }
            Section("Processes") {
                Picker("Default view", selection: $store.viewMode) {
                    ForEach(ViewMode.allCases) { Label($0.title, systemImage: $0.symbol).tag($0) }
                }
                LabeledContent("CPU %") {
                    Text("Share of one core; 100% = one fully busy core")
                        .foregroundStyle(.secondary)
                }
            }
            AboutSection()
        }
        .formStyle(.grouped)
        .scrollContentBackground(.hidden)
        .frame(maxWidth: 720, alignment: .leading)
        .padding(.horizontal, Tokens.Space.lg)
    }
}

/// Which build this is and where Procyon lives on GitHub.
private struct AboutSection: View {
    var body: some View {
        Section("About") {
            LabeledContent("Version") {
                VStack(alignment: .trailing, spacing: Tokens.Space.xs) {
                    Text(AppInfo.versionDescription).foregroundStyle(.secondary)
                    if let commit = AppInfo.commit {
                        Text(commit).font(Tokens.Typography.mono).foregroundStyle(.tertiary)
                    }
                }
                .textSelection(.enabled)
            }
            LabeledContent("Source code") {
                Link("github.com/manhpham90vn/Procyon", destination: AppInfo.repository)
            }
            LabeledContent("Releases") {
                Link("Latest release", destination: AppInfo.latestRelease)
                    .help("Release notes and downloads on GitHub")
            }
            LabeledContent("Feedback") {
                Link("Report an issue", destination: AppInfo.issues)
            }
            Text("Procyon is open source under the MIT license.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }
}

private struct MenuBarModuleToggle: View {
    let module: MenuBarSettings.Module
    @AppStorage private var isOn: Bool

    init(module: MenuBarSettings.Module) {
        self.module = module
        _isOn = AppStorage(wrappedValue: module.defaultOn, module.key)
    }

    var body: some View {
        Toggle(module.title, isOn: $isOn)
    }
}

/// Notifications when the machine or an app stays over a threshold.
/// Shows what the administrator helper is allowed to do and takes that permission back: turn full
/// access off for now, or remove the helper from Login Items so it has to be approved again.
private struct FullAccessSection: View {
    @Environment(SystemStore.self) private var store
    @State private var confirmingRemoval = false

    var body: some View {
        Section("Full access") {
            LabeledContent("Status") { status }
            Text(
                "Full access runs the Procyon helper as an administrator. It lets Processes read and manage system processes, Files & Ports list every process's files and connections, and Startup and Services switch system-wide launch daemons. Without it, those rows show a lock."
            )
            .font(.caption)
            .foregroundStyle(.secondary)

            switch store.fullAccess {
            case .on:
                Button("Turn Off Full Access") { store.disableFullAccess() }
                    .help(turnOffHelp)
            case .starting:
                Button("Turn Off Full Access") {}.disabled(true)
            case .needsApproval:
                Button("Open System Settings") { store.openHelperApproval() }
                    .help("Allow Procyon in System Settings → General → Login Items")
            case .off, .failed:
                Button("Unlock Full Access") { Task { await store.enableFullAccess() } }
                    .help(
                        store.usesBackgroundHelper
                            ? "Registers the helper as a background item; macOS asks you to allow it once"
                            : "Asks for an administrator password and starts the helper for this session")
            }

            if store.usesBackgroundHelper && store.backgroundHelperRegistered {
                Button("Remove Administrator Helper…", role: .destructive) { confirmingRemoval = true }
                    .help("Unregisters the helper from Login Items; unlocking again asks for approval")
                    .confirmationDialog(
                        "Remove the administrator helper?", isPresented: $confirmingRemoval, titleVisibility: .visible
                    ) {
                        Button("Remove Helper", role: .destructive) {
                            Task { await store.removeBackgroundHelper() }
                        }
                        Button("Cancel", role: .cancel) {}
                    } message: {
                        Text(
                            "Procyon loses its administrator rights right away and the helper is removed from System Settings → Login Items. Processes, Startup, Services and Files & Ports go back to showing a lock on system items until you unlock full access again, which asks for your approval once more."
                        )
                    }
                Text(
                    "Turning full access off keeps the helper approved in Login Items, so you can turn it back on without being asked. Removing the helper takes that approval back."
                )
                .font(.caption)
                .foregroundStyle(.secondary)
            }
        }
        .onAppear { store.refreshHelperRegistration() }
    }

    @ViewBuilder private var status: some View {
        switch store.fullAccess {
        case .on:
            Badge("On", tone: .success, symbol: "lock.open.fill")
        case .starting:
            Badge("Starting…", tone: .accent, symbol: "hourglass")
        case .needsApproval:
            Badge("Waiting for approval", tone: .warning, symbol: "lock.shield")
        case .off:
            Badge(
                store.usesBackgroundHelper && store.backgroundHelperRegistered ? "Off, helper approved" : "Off",
                tone: .neutral, symbol: "lock.fill")
        case .failed(let message):
            VStack(alignment: .trailing, spacing: Tokens.Space.xs) {
                Badge("Stopped", tone: .danger, symbol: "exclamationmark.triangle.fill")
                Text(message).font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private var turnOffHelp: String {
        store.usesBackgroundHelper
            ? "Disconnects from the helper; it stays approved in Login Items"
            : "Disconnects from the helper, which then quits; unlocking again asks for a password"
    }
}

private struct AlertsSection: View {
    @Environment(SystemStore.self) private var store
    @AppStorage(TemperatureUnit.storageKey) private var temperatureUnit: TemperatureUnit = .system
    @State private var permissionDenied = false

    var body: some View {
        Section("Alerts") {
            ForEach(store.alertSettings.rules.filter { isAvailable($0.kind) }) { rule in
                row(rule)
            }
            if permissionDenied {
                Label(
                    "Notifications are off for Procyon. Turn them on in System Settings → Notifications.",
                    systemImage: "bell.slash"
                )
                .foregroundStyle(Tokens.Palette.warning)
            }
            Text(
                "An alert fires when the condition lasts for the chosen time, then stays quiet for 15 minutes. Watching apps keeps Procyon reading every process while it sits in the menu bar, which costs a little more CPU."
            )
            .font(.caption)
            .foregroundStyle(.secondary)
            if let last = store.recentAlerts.first {
                LabeledContent("Last alert") {
                    Text("\(last.title) · \(last.date.formatted(date: .omitted, time: .shortened))")
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    private func isAvailable(_ kind: AlertRule.Kind) -> Bool {
        switch kind {
        case .temperature: store.capabilities.contains(.temperature)
        case .memoryPressure: store.capabilities.contains(.memoryPressure)
        default: true
        }
    }

    private func binding(_ kind: AlertRule.Kind) -> Binding<AlertRule> {
        Binding {
            store.alertSettings.rules.first { $0.kind == kind } ?? AlertRule(kind: kind)
        } set: { rule in
            guard let index = store.alertSettings.rules.firstIndex(where: { $0.kind == kind }) else { return }
            let enabling = rule.isEnabled && !store.alertSettings.rules[index].isEnabled
            store.alertSettings.rules[index] = rule
            if enabling { Task { permissionDenied = !(await AlertNotifier.shared.requestPermission()) } }
        }
    }

    @ViewBuilder
    private func row(_ rule: AlertRule) -> some View {
        let rule = binding(rule.kind)
        let kind = rule.wrappedValue.kind
        Toggle(kind.title, isOn: rule.isEnabled)
        if rule.wrappedValue.isEnabled {
            Group {
                if kind != .memoryPressure {
                    LabeledContent("Over") {
                        Stepper(value: rule.threshold, in: kind.thresholdRange, step: kind.step) {
                            Text(threshold(rule.wrappedValue)).monospacedDigit()
                        }
                    }
                }
                Picker("For at least", selection: rule.duration) {
                    ForEach(AlertSettings.durations, id: \.self) { Text(Self.duration($0)).tag($0) }
                }
            }
            .padding(.leading, Tokens.Space.lg)
            .foregroundStyle(.secondary)
        }
    }

    private func threshold(_ rule: AlertRule) -> String {
        switch rule.kind {
        case .cpu, .memory: "\(Int(rule.threshold))%"
        case .appCPU: "\(Int(rule.threshold))% CPU"
        case .appMemory: "\(Int(rule.threshold)) GB"
        case .temperature: Format.temperature(rule.threshold, unit: temperatureUnit)
        case .memoryPressure: ""
        }
    }

    static func duration(_ seconds: TimeInterval) -> String {
        seconds >= 60 ? "\(Int(seconds / 60)) min" : "\(Int(seconds)) s"
    }
}
