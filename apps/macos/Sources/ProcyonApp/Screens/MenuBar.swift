import AppKit
import ProcyonDesign
import ProcyonKit
import SwiftUI

/// What the menu bar widget shows; each module is an `@AppStorage` key.
enum MenuBarSettings {
    /// Closing the window keeps Procyon running in the menu bar (on unless turned off).
    static let enabledKey = "menuBarEnabled"
    static var isEnabled: Bool { UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? true }

    enum Module: String, CaseIterable, Identifiable {
        case cpu, memory, network, gpu, temperature, battery

        var id: String { rawValue }
        var key: String { "menuBar.\(rawValue)" }
        var defaultOn: Bool { self == .cpu || self == .memory }

        var title: String {
            switch self {
            case .cpu: "CPU"
            case .memory: "Memory"
            case .network: "Network"
            case .gpu: "GPU"
            case .temperature: "Temperature"
            case .battery: "Battery"
            }
        }

        func isAvailable(_ capabilities: Capabilities) -> Bool {
            switch self {
            case .gpu: capabilities.contains(.gpu)
            case .temperature: capabilities.contains(.temperature)
            case .battery: capabilities.contains(.battery)
            default: true
            }
        }
    }
}

/// The menu bar item: one narrow two-line column per enabled module (name over value), like other
/// menu bar monitors. One line of text per module made the item about 300 pt wide with every module
/// on; on a notched MacBook that slid it under the notch, where it can't be seen.
struct MenuBarLabel: View {
    @Environment(SystemStore.self) private var store
    @AppStorage(MenuBarSettings.Module.cpu.key) private var cpu = true
    @AppStorage(MenuBarSettings.Module.memory.key) private var memory = true
    @AppStorage(MenuBarSettings.Module.network.key) private var network = false
    @AppStorage(MenuBarSettings.Module.gpu.key) private var gpu = false
    @AppStorage(MenuBarSettings.Module.temperature.key) private var temperature = false
    @AppStorage(MenuBarSettings.Module.battery.key) private var battery = false
    @AppStorage(TemperatureUnit.storageKey) private var temperatureUnit: TemperatureUnit = .system

    var body: some View {
        let columns = self.columns
        if columns.isEmpty {
            Image(systemName: "gauge.with.dots.needle.33percent")
        } else if let image = Self.render(columns) {
            Image(nsImage: image)
        }
    }

    struct Column: Hashable {
        let top: String
        let bottom: String
    }

    private var columns: [Column] {
        let s = store.sample
        let caps = store.capabilities
        guard store.hasSample else { return [] }
        var columns: [Column] = []
        if cpu { columns.append(Column(top: "CPU", bottom: Format.percent(s.cpuUsage))) }
        if memory { columns.append(Column(top: "MEM", bottom: Format.percent(s.memoryFraction))) }
        if network {
            columns.append(
                Column(top: "↓\(Self.compact(s.networkReceiveRate))", bottom: "↑\(Self.compact(s.networkSendRate))"))
        }
        if gpu, caps.contains(.gpu) { columns.append(Column(top: "GPU", bottom: Format.percent(s.gpuUsage))) }
        if temperature, caps.contains(.temperature) {
            columns.append(Column(top: "TEMP", bottom: Format.temperature(s.cpuTemperature, unit: temperatureUnit)))
        }
        if battery, let level = store.battery?.level { columns.append(Column(top: "BAT", bottom: Format.percent(level))) }
        return columns
    }

    /// A template image, so the menu bar tints it for light, dark and highlighted states.
    @MainActor
    static func render(_ columns: [Column]) -> NSImage? {
        let content = HStack(spacing: 5) {
            ForEach(columns, id: \.self) { column in
                VStack(alignment: .center, spacing: -1) {
                    Text(column.top).font(.system(size: 7.5, weight: .semibold))
                    Text(column.bottom).font(.system(size: 9.5, weight: .medium).monospacedDigit())
                }
                .fixedSize()
            }
        }
        .foregroundStyle(.black)
        .frame(height: 22)
        let renderer = ImageRenderer(content: content)
        renderer.scale = NSScreen.main?.backingScaleFactor ?? 2
        guard let image = renderer.nsImage else { return nil }
        image.isTemplate = true
        return image
    }

    /// "1.2M", "640K": rates short enough for the menu bar.
    static func compact(_ bytesPerSecond: Double) -> String {
        switch bytesPerSecond {
        case ..<1024: "0K"
        case ..<(1024 * 1024): String(format: "%.0fK", bytesPerSecond / 1024)
        case ..<(1024 * 1024 * 1024): String(format: "%.1fM", bytesPerSecond / 1_048_576)
        default: String(format: "%.1fG", bytesPerSecond / 1_073_741_824)
        }
    }
}

/// The panel under the menu bar item: live figures, the busiest apps and a way back to the window.
struct MenuBarPanel: View {
    @Binding var page: Page
    @Environment(SystemStore.self) private var store
    @AppStorage(TemperatureUnit.storageKey) private var temperatureUnit: TemperatureUnit = .system
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        let s = store.sample
        VStack(alignment: .leading, spacing: Tokens.Space.md) {
            HStack {
                Text("Procyon").font(Tokens.Typography.headline)
                Spacer()
                LiveIndicator(isLive: !store.isPaused)
                Text(store.isPaused ? "Paused" : "Live")
                    .font(Tokens.Typography.caption)
                    .foregroundStyle(Tokens.Palette.textSecondary)
            }

            meter("CPU", Format.percent(s.cpuUsage), s.cpuUsage, .cpu, page: .cpu)
            meter(
                "Memory", "\(Format.bytes(s.memoryUsed)) · \(Format.percent(s.memoryFraction))", s.memoryFraction, .memory,
                page: .memory)
            if store.capabilities.contains(.gpu) {
                meter("GPU", Format.percent(s.gpuUsage), s.gpuUsage ?? 0, .gpu, page: .gpu)
            }
            row(
                "Network", "↓ \(Format.rate(s.networkReceiveRate))  ↑ \(Format.rate(s.networkSendRate))", style: .network,
                page: .network)
            if let temperature = s.cpuTemperature {
                row(
                    "Chip temperature", Format.temperature(temperature, unit: temperatureUnit), symbol: "thermometer.medium",
                    page: .cpu)
            }
            if let battery = store.battery {
                row(
                    "Battery", "\(Format.percent(battery.level)) · \(battery.stateTitle)",
                    symbol: BatteryGlyph.symbol(for: battery), page: .battery)
            }

            Divider()
            Text("TOP APPS").font(Tokens.Typography.caption).tracking(0.8).foregroundStyle(Tokens.Palette.textTertiary)
            if store.topCPU.isEmpty {
                ProgressView().controlSize(.small).frame(maxWidth: .infinity)
            }
            ForEach(store.topCPU.prefix(4)) { app in
                HStack(spacing: Tokens.Space.sm) {
                    ProcessIcon(row: app, size: 16)
                    Text(app.name).lineLimit(1)
                    Spacer()
                    Text(Format.cpu(app.cpu)).monospacedDigit().foregroundStyle(Tokens.Palette.textSecondary)
                }
                .font(Tokens.Typography.body)
            }

            Divider()
            HStack {
                Button("Open Procyon") { open(nil) }
                Spacer()
                Button("Settings…") { open(.settings) }
                Button("Quit") {
                    AppDelegate.quitRequested = true
                    NSApp.terminate(nil)
                }
            }
            .buttonStyle(.borderless)
        }
        .padding(Tokens.Space.lg)
        .frame(width: 300)
        // The panel lists apps: sample processes while it is open.
        .onAppear { store.needsProcesses("menuBarPanel", true) }
        .onDisappear { store.needsProcesses("menuBarPanel", false) }
    }

    private func open(_ target: Page?) {
        if let target { page = target }
        openWindow(id: "main")
        NSApp.activate()
    }

    private func meter(_ title: String, _ value: String, _ fraction: Double, _ metric: Metric, page: Page) -> some View {
        Button {
            open(page)
        } label: {
            VStack(alignment: .leading, spacing: Tokens.Space.xs) {
                HStack {
                    MetricIcon(metric.style, size: 16)
                    Text(title).font(Tokens.Typography.headline)
                    Spacer()
                    Text(value).font(Tokens.Typography.body.monospacedDigit()).foregroundStyle(Tokens.Palette.textSecondary)
                }
                UsageBar(value: min(max(fraction, 0), 1), style: metric.style, height: 4)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private func row(_ title: String, _ value: String, style: Metric? = nil, symbol: String? = nil, page: Page) -> some View {
        Button {
            open(page)
        } label: {
            HStack {
                if let style {
                    MetricIcon(style.style, size: 16)
                } else if let symbol {
                    Image(systemName: symbol).frame(width: 16).foregroundStyle(Tokens.Palette.textSecondary)
                }
                Text(title).font(Tokens.Typography.headline)
                Spacer()
                Text(value).font(Tokens.Typography.body.monospacedDigit()).foregroundStyle(Tokens.Palette.textSecondary)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}
