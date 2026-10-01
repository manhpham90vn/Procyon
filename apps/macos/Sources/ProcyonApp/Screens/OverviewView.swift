import ProcyonDesign
import ProcyonKit
import SwiftUI

struct OverviewView: View {
    @Binding var page: Page
    @Environment(SystemStore.self) private var store

    private var sample: SystemSample { store.sample }
    private var rootVolume: Volume? { store.volumes.first(where: \.isRoot) ?? store.volumes.first }

    var body: some View {
        ScreenScroll {
            hero

            LazyVGrid(columns: [GridItem(.adaptive(minimum: 200), spacing: Tokens.Space.lg)], spacing: Tokens.Space.lg) {
                Button {
                    page = .cpu
                } label: {
                    MetricCard(
                        title: "CPU", style: Metric.cpu.style,
                        value: Format.percent(sample.cpuUsage).dropLast().description, unit: "%",
                        caption: "User \(Format.percent(sample.cpuUser)) · System \(Format.percent(sample.cpuSystem))",
                        series: [ChartSeries(id: "cpu", samples: store.history.cpu.samples, color: Metric.cpu.style.start)],
                        maxValue: 1)
                }
                Button {
                    page = .memory
                } label: {
                    let used = Format.bytesParts(sample.memoryUsed)
                    MetricCard(
                        title: "Memory", style: Metric.memory.style, value: used.value, unit: used.unit,
                        caption: "of \(Format.bytes(sample.memoryTotal)) · Pressure \(sample.memoryPressure.title)",
                        series: [ChartSeries(id: "mem", samples: store.history.memory.samples, color: Metric.memory.style.start)],
                        maxValue: 1)
                }
                Button {
                    page = .disk
                } label: {
                    let read = Format.rateParts(sample.diskReadRate)
                    MetricCard(
                        title: "Disk", style: Metric.disk.style, value: read.value, unit: read.unit + " read",
                        caption: "Write \(Format.rate(sample.diskWriteRate))",
                        series: [
                            ChartSeries(id: "r", samples: store.history.diskRead.samples, color: Metric.disk.style.start),
                            ChartSeries(
                                id: "w", samples: store.history.diskWrite.samples, color: Metric.disk.style.end, fills: false),
                        ])
                }
                Button {
                    page = .network
                } label: {
                    let down = Format.rateParts(sample.networkReceiveRate)
                    MetricCard(
                        title: "Network", style: Metric.network.style, value: down.value, unit: down.unit + " down",
                        caption: "Up \(Format.rate(sample.networkSendRate))",
                        series: [
                            ChartSeries(
                                id: "rx", samples: store.history.networkReceive.samples, color: Metric.network.style.start),
                            ChartSeries(
                                id: "tx", samples: store.history.networkSend.samples, color: Metric.network.style.end,
                                fills: false),
                        ])
                }
            }
            .buttonStyle(.plain)

            HStack(alignment: .top, spacing: Tokens.Space.lg) {
                TopList(title: "Top CPU", symbol: "flame.fill", rows: store.topCPU, metric: .cpu) { row in
                    (Format.cpu(row.cpu), min((row.cpu ?? 0) / 100, 1))
                }
                TopList(title: "Top Memory", symbol: "memorychip.fill", rows: store.topMemory, metric: .memory) { row in
                    (Format.bytes(row.memory), Double(row.memory ?? 0) / Double(max(sample.memoryTotal, 1)))
                }
            }
        }
    }

    private var hero: some View {
        HStack(spacing: Tokens.Space.xl) {
            ZStack {
                Circle()
                    .fill(
                        LinearGradient(
                            colors: [Metric.cpu.style.start.opacity(0.25), Metric.memory.style.start.opacity(0.25)],
                            startPoint: .topLeading, endPoint: .bottomTrailing))
                Image(systemName: store.info.deviceSymbol)
                    .font(.system(size: 34, weight: .light))
                    .foregroundStyle(
                        LinearGradient(
                            colors: [Metric.cpu.style.start, Metric.memory.style.end],
                            startPoint: .topLeading, endPoint: .bottomTrailing))
            }
            .frame(width: 78, height: 78)

            VStack(alignment: .leading, spacing: Tokens.Space.xs) {
                let model = store.info.displayModel.split(separator: " (", maxSplits: 1).map(String.init)
                HStack(alignment: .firstTextBaseline, spacing: Tokens.Space.sm) {
                    Text(model.first ?? "")
                        .font(Tokens.Typography.display)
                        .foregroundStyle(Tokens.Palette.textPrimary)
                    if model.count > 1 {
                        Text("(" + model[1])
                            .font(Tokens.Typography.title)
                            .foregroundStyle(Tokens.Palette.textTertiary)
                    }
                }
                .lineLimit(1)
                .minimumScaleFactor(0.7)
                Text(
                    "\(store.info.cpuBrand) · \(Format.bytes(store.info.memoryTotal)) · \(store.info.osName) \(store.info.osVersion)"
                )
                .font(Tokens.Typography.body)
                .foregroundStyle(Tokens.Palette.textSecondary)
                HStack(spacing: Tokens.Space.xs + 2) {
                    Badge("Up \(Format.duration(store.uptime))", tone: .accent, symbol: "clock")
                    Badge("\(Format.count(sample.processCount)) processes", symbol: "square.stack.3d.up")
                    if sample.memoryPressure != .unknown {
                        Badge(
                            "Memory \(sample.memoryPressure.title)",
                            tone: sample.memoryPressure == .normal
                                ? .success : sample.memoryPressure == .warning ? .warning : .danger,
                            symbol: "gauge.with.dots.needle.50percent")
                    }
                }
                .padding(.top, Tokens.Space.xs)
            }

            Spacer(minLength: Tokens.Space.lg)

            HStack(spacing: Tokens.Space.xl) {
                HeroGauge(title: "CPU", value: sample.cpuUsage, style: Metric.cpu.style)
                HeroGauge(title: "Memory", value: sample.memoryFraction, style: Metric.memory.style)
                if let rootVolume {
                    HeroGauge(title: "Storage", value: rootVolume.usedFraction, style: Metric.disk.style)
                }
            }
        }
        .padding(Tokens.Space.xl)
        .background {
            ZStack {
                RadialGradient(
                    colors: [Metric.cpu.style.start.opacity(0.18), .clear], center: .topLeading, startRadius: 0, endRadius: 420)
                RadialGradient(
                    colors: [Metric.memory.style.end.opacity(0.12), .clear], center: .bottomTrailing, startRadius: 0,
                    endRadius: 380)
            }
        }
        .cardSurface(radius: Tokens.Radius.xl, padding: 0)
    }
}

private struct HeroGauge: View {
    let title: String
    let value: Double
    let style: MetricStyle

    var body: some View {
        VStack(spacing: Tokens.Space.sm) {
            RingGauge(value: value, style: style, lineWidth: 9) {
                Text(Format.percent(value))
                    .font(.system(size: 17, weight: .semibold, design: .rounded).monospacedDigit())
                    .foregroundStyle(Tokens.Palette.textPrimary)
            }
            .frame(width: 86, height: 86)
            Text(title).font(Tokens.Typography.label).foregroundStyle(Tokens.Palette.textSecondary)
        }
    }
}

private struct TopList: View {
    let title: String
    let symbol: String
    let rows: [ProcessRow]
    let metric: Metric
    let value: (ProcessRow) -> (String, Double)

    var body: some View {
        Panel(title, symbol: symbol) {
            VStack(spacing: Tokens.Space.md) {
                if rows.isEmpty {
                    ProgressView().frame(maxWidth: .infinity, minHeight: 120)
                }
                ForEach(rows) { row in
                    let (text, fraction) = value(row)
                    HStack(spacing: Tokens.Space.sm + 2) {
                        ProcessIcon(row: row, size: 22)
                        VStack(alignment: .leading, spacing: Tokens.Space.xs) {
                            HStack {
                                Text(row.name)
                                    .font(Tokens.Typography.headline)
                                    .foregroundStyle(Tokens.Palette.textPrimary)
                                    .lineLimit(1)
                                if row.processCount > 1 {
                                    Text("\(row.processCount)")
                                        .font(Tokens.Typography.caption)
                                        .foregroundStyle(Tokens.Palette.textTertiary)
                                }
                                Spacer()
                                Text(text)
                                    .font(Tokens.Typography.headline.monospacedDigit())
                                    .foregroundStyle(Tokens.Palette.textPrimary)
                            }
                            UsageBar(value: fraction, style: metric.style, height: 4)
                        }
                    }
                }
            }
        }
    }
}
