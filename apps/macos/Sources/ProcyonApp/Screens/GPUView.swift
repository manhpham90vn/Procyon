import ProcyonDesign
import ProcyonKit
import SwiftUI

struct GPUView: View {
    @Environment(SystemStore.self) private var store
    @AppStorage(TemperatureUnit.storageKey) private var temperatureUnit: TemperatureUnit = .system

    var body: some View {
        let s = store.sample
        let style = Metric.gpu.style
        let gpu = s.gpus.first
        ScreenScroll {
            PageHeader("GPU", subtitle: subtitle(gpu), style: style) {
                ValueText(Format.percent(s.gpuUsage).dropLast().description, unit: "%", font: Tokens.Typography.display)
            }

            Panel("Utilization", symbol: "waveform.path.ecg", tint: style.start) {
                LiveChart(
                    series: [ChartSeries(id: "gpu", samples: store.history.gpu.samples, color: style.start)],
                    legend: [.init("Device", value: Format.percent(s.gpuUsage), color: style.start)],
                    maxValue: 1, axisLabel: { Format.percent($0) }
                )
                .frame(height: 220)
            }

            temperaturePanel(s)

            ForEach(s.gpus) { gpu in
                HStack(alignment: .top, spacing: Tokens.Space.lg) {
                    Panel(s.gpus.count > 1 ? gpu.name : "Details", symbol: "list.bullet.rectangle") {
                        StatGrid(details(gpu), columns: 3)
                    }
                    Panel("Memory", symbol: "memorychip") {
                        memory(gpu, style: style)
                    }
                    .frame(maxWidth: 360)
                }
                .equalHeightPanels()
            }

            if store.capabilities.contains(.processGPU) {
                TopAppsPanel(
                    title: "Top Apps", symbol: "flame.fill", tint: style.start, rows: store.topGPU,
                    isLoading: !store.hasSample, style: style, emptyText: "No app is using the GPU right now.",
                    fraction: { ($0.gpu ?? 0) / 100 }
                ) { TopAppValue(text: Format.cpu($0.gpu)) }
            } else {
                InfoBanner("This GPU's driver doesn't report usage per app.", symbol: "info.circle")
            }
        }
    }

    /// The GPU's own sensor when it has one; on Apple Silicon the chip's, which the GPU shares.
    @ViewBuilder
    private func temperaturePanel(_ s: SystemSample) -> some View {
        let own = s.gpus.compactMap(\.temperature).max()
        let shared = s.gpus.contains(where: \.unifiedMemory) ? s.cpuTemperature : nil
        if let current = own ?? shared {
            let history = own != nil ? store.history.gpuTemperature : store.history.cpuTemperature
            let color = Metric.gpu.style.end
            Panel("Temperature", symbol: "thermometer.medium", tint: color) {
                LiveChart(
                    series: [ChartSeries(id: "temp", samples: history.samples, color: color)],
                    legend: [
                        .init(
                            own != nil ? "GPU" : "Chip (shared with the CPU)",
                            value: Format.temperature(current, unit: temperatureUnit), color: color),
                        .init(
                            "Peak", value: Format.temperature(history.peak, unit: temperatureUnit), color: Tokens.Palette.danger),
                    ],
                    // Celsius in the history; the axis shows the unit chosen in Settings.
                    maxValue: 110, axisLabel: { Format.temperature($0, unit: temperatureUnit) }
                )
                .frame(height: 160)
            }
        }
    }

    private func subtitle(_ gpu: GPUInfo?) -> String {
        guard let gpu else { return "No GPU found" }
        var parts = [gpu.name]
        if gpu.cores > 0 { parts.append("\(gpu.cores) cores") }
        if gpu.unifiedMemory { parts.append("Unified memory") }
        return parts.joined(separator: " · ")
    }

    private func details(_ gpu: GPUInfo) -> [StatItem] {
        var items = [StatItem("Utilization", value: Format.percent(gpu.utilization, digits: 1), tint: Metric.gpu.style.start)]
        if let renderer = gpu.rendererUtilization {
            items.append(StatItem("Renderer", value: Format.percent(renderer, digits: 1), detail: "3D and compute"))
        }
        if let tiler = gpu.tilerUtilization {
            items.append(StatItem("Tiler", value: Format.percent(tiler, digits: 1), detail: "Geometry"))
        }
        if let encoder = gpu.encoderUtilization { items.append(StatItem("Video encode", value: Format.percent(encoder))) }
        if let decoder = gpu.decoderUtilization { items.append(StatItem("Video decode", value: Format.percent(decoder))) }
        if let temperature = gpu.temperature {
            items.append(StatItem("Temperature", value: Format.temperature(temperature, unit: temperatureUnit)))
        } else if let chip = store.sample.cpuTemperature, gpu.unifiedMemory {
            // Apple Silicon has no separate GPU sensor; the die is shared.
            items.append(
                StatItem(
                    "Chip temperature", value: Format.temperature(chip, unit: temperatureUnit), detail: "Shared with the CPU"))
        }
        items.append(StatItem("Vendor", value: gpu.vendor.isEmpty ? Format.unavailable : gpu.vendor))
        if gpu.cores > 0 { items.append(StatItem("Cores", value: "\(gpu.cores)")) }
        return items
    }

    @ViewBuilder
    private func memory(_ gpu: GPUInfo, style: MetricStyle) -> some View {
        VStack(alignment: .leading, spacing: Tokens.Space.md) {
            let used = Format.bytesParts(gpu.memoryUsed)
            HStack(alignment: .firstTextBaseline) {
                ValueText(used.value, unit: used.unit)
                if let total = gpu.memoryTotal {
                    Text("of \(Format.bytes(total))").font(Tokens.Typography.body).foregroundStyle(Tokens.Palette.textTertiary)
                }
            }
            if let total = gpu.memoryTotal, let usedBytes = gpu.memoryUsed {
                UsageBar(value: Double(usedBytes) / Double(max(total, 1)), style: style)
            } else if gpu.unifiedMemory, let usedBytes = gpu.memoryUsed {
                UsageBar(value: Double(usedBytes) / Double(max(store.sample.memoryTotal, 1)), style: style)
                Text("System memory the GPU holds, out of \(Format.bytes(store.sample.memoryTotal)) shared with the CPU.")
                    .font(Tokens.Typography.caption)
                    .foregroundStyle(Tokens.Palette.textTertiary)
            }
            Sparkline(
                series: [ChartSeries(id: "mem", samples: store.history.gpuMemory.samples, color: style.end)],
                lineWidth: 1.5
            )
            // Takes the height the Details card leaves, so both cards end level.
            .frame(minHeight: 40, maxHeight: .infinity)
        }
    }
}
