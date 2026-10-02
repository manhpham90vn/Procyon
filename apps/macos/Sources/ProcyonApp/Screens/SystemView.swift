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
