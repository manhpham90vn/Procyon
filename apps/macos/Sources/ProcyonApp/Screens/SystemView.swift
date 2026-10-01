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
                        minimumWidth: 150)
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
                        ], minimumWidth: 150)
                }
            }

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

    var body: some View {
        @Bindable var store = store
        Form {
            Section("Updates") {
                Picker("Update speed", selection: $store.interval) {
                    ForEach(SystemStore.refreshIntervals, id: \.self) { Text("Every \(Format.interval($0))").tag($0) }
                }
                Toggle("Pause updates", isOn: $store.isPaused)
            }
            Section("Full access") {
                LabeledContent("Administrator helper") {
                    switch store.fullAccess {
                    case .on: Label("Running", systemImage: "lock.open.fill").foregroundStyle(Tokens.Palette.success)
                    case .starting: ProgressView().controlSize(.small)
                    case .needsApproval: Text("Waiting for approval").foregroundStyle(Tokens.Palette.warning)
                    case .failed(let message): Text(message).foregroundStyle(Tokens.Palette.warning)
                    case .off: Text("Off").foregroundStyle(.secondary)
                    }
                }
                if store.fullAccess.isOn {
                    Button("Turn Off Full Access") { store.disableFullAccess() }
                } else if store.fullAccess == .needsApproval {
                    Button("Open System Settings…") { store.openHelperApproval() }
                } else {
                    Button("Unlock Full Access…") { Task { await store.enableFullAccess() } }
                        .disabled(store.fullAccess == .starting)
                }
                if store.usesBackgroundHelper && store.backgroundHelperRegistered {
                    Button("Remove Helper", role: .destructive) { Task { await store.removeBackgroundHelper() } }
                }
                Text(
                    store.usesBackgroundHelper
                        ? "Reads CPU, memory and disk usage of system processes and lets you end them. You allow the helper once in System Settings → General → Login Items; it starts only when Procyon connects and talks only to Procyon."
                        : "Reads CPU, memory and disk usage of system processes and lets you end them. The helper asks for your password, talks only to this app and quits with it."
                )
                .font(.caption)
                .foregroundStyle(.secondary)
            }
            Section("Appearance") {
                Picker("Theme", selection: $appearance) {
                    ForEach(Appearance.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
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
        .frame(width: 460)
        .fixedSize()
    }
}
