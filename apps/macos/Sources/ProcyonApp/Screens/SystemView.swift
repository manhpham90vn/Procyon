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
            PageHeader("Settings", subtitle: "Updates, menu bar, appearance and processes")
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
        }
        .formStyle(.grouped)
        .scrollContentBackground(.hidden)
        .frame(maxWidth: 720, alignment: .leading)
        .padding(.horizontal, Tokens.Space.lg)
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
