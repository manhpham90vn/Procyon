import ProcyonDesign
import ProcyonKit
import SwiftUI

struct CPUView: View {
    @Environment(SystemStore.self) private var store

    var body: some View {
        let s = store.sample
        let style = Metric.cpu.style
        ScreenScroll {
            PageHeader("CPU", subtitle: "\(store.info.cpuBrand) · \(store.info.coreSummary)", style: style) {
                ValueText(Format.percent(s.cpuUsage).dropLast().description, unit: "%", font: Tokens.Typography.display)
            }

            Panel("Utilization", symbol: "waveform.path.ecg", tint: style.start) {
                LiveChart(
                    series: [
                        ChartSeries(id: "total", samples: store.history.cpu.samples, color: style.start),
                        ChartSeries(id: "system", samples: store.history.cpuSystem.samples, color: style.end, fills: false),
                    ],
                    legend: [
                        .init("Total", value: Format.percent(s.cpuUsage), color: style.start),
                        .init("System", value: Format.percent(s.cpuSystem), color: style.end),
                    ],
                    maxValue: 1, axisLabel: { Format.percent($0) }
                )
                .frame(height: 240)
            }

            Panel("Details", symbol: "list.bullet.rectangle") {
                StatGrid([
                    StatItem("Utilization", value: Format.percent(s.cpuUsage, digits: 1), tint: style.start),
                    StatItem("User", value: Format.percent(s.cpuUser, digits: 1)),
                    StatItem("System", value: Format.percent(s.cpuSystem, digits: 1), tint: style.end),
                    StatItem("Idle", value: Format.percent(max(0, 1 - s.cpuUsage), digits: 1)),
                    StatItem(
                        "Load average", value: s.loadAverage.map { String(format: "%.2f", $0) }.joined(separator: " · "),
                        detail: "1 · 5 · 15 minutes"),
                    StatItem("Processes", value: Format.count(s.processCount)),
                    StatItem("Threads", value: Format.count(s.threadCount)),
                    StatItem("Uptime", value: Format.duration(store.uptime)),
                    StatItem("Cores", value: store.info.coreSummary, detail: "\(store.info.logicalCores) logical"),
                ])
            }

            Panel("Cores", symbol: "square.grid.3x3.fill") {
                CoreGrid(
                    cores: s.coreUsage.indices.map { index in
                        CoreGrid.Core(
                            id: index, label: "Core \(index + 1)", usage: s.coreUsage[index],
                            samples: index < store.history.cores.count ? store.history.cores[index].samples : [])
                    }, style: style)
            }
        }
    }
}

struct MemoryView: View {
    @Environment(SystemStore.self) private var store

    var body: some View {
        let s = store.sample
        let style = Metric.memory.style
        let used = Format.bytesParts(s.memoryUsed)
        ScreenScroll {
            PageHeader(
                "Memory", subtitle: "\(Format.bytes(s.memoryTotal)) installed · Pressure \(s.memoryPressure.title)", style: style
            ) {
                HStack(alignment: .firstTextBaseline, spacing: Tokens.Space.xs) {
                    ValueText(used.value, unit: used.unit, font: Tokens.Typography.display)
                    Text("of \(Format.bytes(s.memoryTotal))")
                        .font(Tokens.Typography.body)
                        .foregroundStyle(Tokens.Palette.textTertiary)
                }
            }

            Panel("Memory used", symbol: "waveform.path.ecg", tint: style.start) {
                LiveChart(
                    series: [ChartSeries(id: "used", samples: store.history.memory.samples, color: style.start)],
                    legend: [.init("Used", value: Format.percent(s.memoryFraction), color: style.start)],
                    maxValue: 1, axisLabel: { Format.percent($0) }
                )
                .frame(height: 220)
            }

            Panel("Composition", symbol: "chart.bar.fill") {
                StackedBar(segments: [
                    .init("App", value: Double(s.memoryApp), detail: Format.bytes(s.memoryApp), color: style.start),
                    .init("Wired", value: Double(s.memoryWired), detail: Format.bytes(s.memoryWired), color: style.end),
                    .init(
                        "Compressed", value: Double(s.memoryCompressed), detail: Format.bytes(s.memoryCompressed),
                        color: Metric.cpu.style.start),
                    .init(
                        "Cached files", value: Double(s.memoryCached), detail: Format.bytes(s.memoryCached),
                        color: Tokens.Palette.textTertiary.opacity(0.6)),
                    .init("Free", value: Double(s.memoryFree), detail: Format.bytes(s.memoryFree), color: Tokens.Palette.track),
                ])
            }

            HStack(alignment: .top, spacing: Tokens.Space.lg) {
                Panel("Details", symbol: "list.bullet.rectangle") {
                    StatGrid(
                        [
                            StatItem("Pressure", value: s.memoryPressure.title, tint: s.memoryPressure.color),
                            StatItem("Used", value: Format.bytes(s.memoryUsed), detail: Format.percent(s.memoryFraction)),
                            StatItem("App memory", value: Format.bytes(s.memoryApp)),
                            StatItem("Wired", value: Format.bytes(s.memoryWired)),
                            StatItem("Compressed", value: Format.bytes(s.memoryCompressed)),
                            StatItem("Cached files", value: Format.bytes(s.memoryCached)),
                        ], minimumWidth: 120)
                }
                Panel("Swap", symbol: "arrow.left.arrow.right") {
                    VStack(alignment: .leading, spacing: Tokens.Space.md) {
                        HStack(alignment: .firstTextBaseline) {
                            let swap = Format.bytesParts(s.swapUsed)
                            ValueText(swap.value, unit: swap.unit)
                            Text("of \(Format.bytes(s.swapTotal))")
                                .font(Tokens.Typography.label)
                                .foregroundStyle(Tokens.Palette.textTertiary)
                        }
                        UsageBar(value: s.swapFraction, style: style)
                        Sparkline(samples: store.history.swap.samples, color: style.end)
                            .frame(height: 56)
                    }
                }
                .frame(maxWidth: 340)
            }
        }
    }
}

struct DiskView: View {
    @Environment(SystemStore.self) private var store

    var body: some View {
        let s = store.sample
        let style = Metric.disk.style
        ScreenScroll {
            PageHeader("Disk", subtitle: "All internal and external drives", style: style) {
                RateHeadline(
                    primary: s.diskReadRate, secondary: s.diskWriteRate, primaryLabel: "Read", secondaryLabel: "Write",
                    style: style)
            }

            Panel("Activity", symbol: "waveform.path.ecg", tint: style.start) {
                LiveChart(
                    series: [
                        ChartSeries(id: "read", samples: store.history.diskRead.samples, color: style.start),
                        ChartSeries(id: "write", samples: store.history.diskWrite.samples, color: style.end, fills: false),
                    ],
                    legend: [
                        .init("Read", value: Format.rate(s.diskReadRate), color: style.start),
                        .init("Write", value: Format.rate(s.diskWriteRate), color: style.end),
                    ],
                    axisLabel: { Format.rate($0) }
                )
                .frame(height: 240)
            }

            Panel("Details", symbol: "list.bullet.rectangle") {
                StatGrid([
                    StatItem("Read", value: Format.rate(s.diskReadRate), tint: style.start),
                    StatItem("Write", value: Format.rate(s.diskWriteRate), tint: style.end),
                    StatItem("Peak read (60s)", value: Format.rate(store.history.diskRead.peak)),
                    StatItem("Peak write (60s)", value: Format.rate(store.history.diskWrite.peak)),
                    StatItem("Read since boot", value: Format.bytes(s.diskReadTotal)),
                    StatItem("Written since boot", value: Format.bytes(s.diskWriteTotal)),
                ])
            }

            VolumesPanel(volumes: store.volumes)
        }
        .onAppear { store.refreshVolumes() }
    }
}

struct NetworkView: View {
    @Environment(SystemStore.self) private var store

    var body: some View {
        let s = store.sample
        let style = Metric.network.style
        ScreenScroll {
            PageHeader("Network", subtitle: "All interfaces except loopback", style: style) {
                RateHeadline(
                    primary: s.networkReceiveRate, secondary: s.networkSendRate, primaryLabel: "Down", secondaryLabel: "Up",
                    style: style)
            }

            Panel("Throughput", symbol: "waveform.path.ecg", tint: style.start) {
                LiveChart(
                    series: [
                        ChartSeries(id: "rx", samples: store.history.networkReceive.samples, color: style.start),
                        ChartSeries(id: "tx", samples: store.history.networkSend.samples, color: style.end, fills: false),
                    ],
                    legend: [
                        .init("Received", value: Format.rate(s.networkReceiveRate), color: style.start),
                        .init("Sent", value: Format.rate(s.networkSendRate), color: style.end),
                    ],
                    axisLabel: { Format.rate($0) }
                )
                .frame(height: 240)
            }

            Panel("Details", symbol: "list.bullet.rectangle") {
                StatGrid([
                    StatItem("Download", value: Format.rate(s.networkReceiveRate), tint: style.start),
                    StatItem("Upload", value: Format.rate(s.networkSendRate), tint: style.end),
                    StatItem("Peak down (60s)", value: Format.rate(store.history.networkReceive.peak)),
                    StatItem("Peak up (60s)", value: Format.rate(store.history.networkSend.peak)),
                    StatItem("Received since boot", value: Format.bytes(s.networkReceiveTotal)),
                    StatItem("Sent since boot", value: Format.bytes(s.networkSendTotal)),
                ])
            }

            if !store.capabilities.contains(.processNetwork) {
                InfoBanner(
                    "Per-app network usage isn't available on macOS without a privileged helper, so Procyon hides it instead of showing estimates."
                )
            }
        }
    }
}

/// Two stacked rates for page headers (read/write, down/up).
private struct RateHeadline: View {
    let primary: Double
    let secondary: Double
    let primaryLabel: String
    let secondaryLabel: String
    let style: MetricStyle

    var body: some View {
        HStack(spacing: Tokens.Space.xl) {
            entry(primaryLabel, primary, style.start)
            entry(secondaryLabel, secondary, style.end)
        }
    }

    private func entry(_ label: String, _ value: Double, _ color: Color) -> some View {
        let parts = Format.rateParts(value)
        return VStack(alignment: .trailing, spacing: 0) {
            HStack(spacing: Tokens.Space.xs) {
                Circle().fill(color).frame(width: 7, height: 7)
                Text(label).font(Tokens.Typography.label).foregroundStyle(Tokens.Palette.textSecondary)
            }
            ValueText(parts.value, unit: parts.unit)
        }
    }
}

struct VolumesPanel: View {
    let volumes: [Volume]

    var body: some View {
        Panel("Volumes", symbol: "externaldrive.fill") {
            VStack(spacing: Tokens.Space.lg) {
                if volumes.isEmpty { ProgressView().frame(maxWidth: .infinity) }
                ForEach(volumes) { volume in
                    HStack(spacing: Tokens.Space.md) {
                        Image(systemName: volume.isRemovable ? "externaldrive.fill" : "internaldrive.fill")
                            .font(.system(size: 20))
                            .foregroundStyle(Metric.disk.style.gradient)
                            .frame(width: 32)
                        VStack(alignment: .leading, spacing: Tokens.Space.xs + 2) {
                            HStack(alignment: .firstTextBaseline) {
                                Text(volume.name).font(Tokens.Typography.headline).foregroundStyle(Tokens.Palette.textPrimary)
                                Text("\(volume.mountPoint) · \(volume.fileSystem.uppercased())")
                                    .font(Tokens.Typography.caption)
                                    .foregroundStyle(Tokens.Palette.textTertiary)
                                Spacer()
                                Text("\(Format.bytes(volume.availableBytes)) available of \(Format.bytes(volume.totalBytes))")
                                    .font(Tokens.Typography.label.monospacedDigit())
                                    .foregroundStyle(Tokens.Palette.textSecondary)
                            }
                            UsageBar(value: volume.usedFraction, style: Metric.disk.style, height: 8)
                        }
                    }
                }
            }
        }
    }
}
