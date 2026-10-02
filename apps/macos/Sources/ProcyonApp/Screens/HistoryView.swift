import Charts
import ProcyonDesign
import ProcyonKit
import SwiftUI

/// The last 24 hours minute by minute: when the machine was busy, and which apps made it busy.
struct HistoryView: View {
    @Environment(SystemStore.self) private var store
    @AppStorage(TemperatureUnit.storageKey) private var temperatureUnit: TemperatureUnit = .system
    @State private var range: Range = .hour
    @State private var metric: HistoryMetric = .cpu
    @State private var minutes: [MachineMinute] = []
    @State private var apps: [AppUsage] = []
    @State private var loaded = false
    @State private var selected: Date?
    @State private var size: Int64 = 0
    @State private var confirmClear = false

    enum Range: Int, CaseIterable, Identifiable {
        case hour = 3600, sixHours = 21600, day = 86400
        var id: Int { rawValue }
        var title: String {
            switch self {
            case .hour: "1 Hour"
            case .sixHours: "6 Hours"
            case .day: "24 Hours"
            }
        }
    }

    var body: some View {
        @Bindable var store = store
        ScreenScroll {
            PageHeader("History", subtitle: "The last 24 hours, minute by minute") {
                Picker("Range", selection: $range) {
                    ForEach(Range.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .fixedSize()
            }

            Picker("Metric", selection: $metric) {
                ForEach(availableMetrics) { Text($0.title).tag($0) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .fixedSize()

            if !store.recordsHistory {
                ActionBanner(
                    symbol: "pause.circle.fill", title: "History is off",
                    message: "Procyon isn't recording. Turn it on to keep the last 24 hours on this Mac.",
                    actionTitle: "Turn On"
                ) { store.recordsHistory = true }
            }

            Panel(metric.title, symbol: "chart.xyaxis.line", tint: metric.style.start) {
                chart
                    .frame(height: 260)
                    .overlay {
                        if loaded && points.isEmpty {
                            EmptyState(
                                symbol: "clock.arrow.circlepath", title: "No history yet",
                                message: "Procyon saves one point a minute while it runs. Come back in a few minutes.")
                        }
                    }
            }

            HStack(alignment: .top, spacing: Tokens.Space.lg) {
                appsPanel
                peaksPanel.frame(maxWidth: 380)
            }
            .equalHeightPanels()

            HStack(spacing: Tokens.Space.md) {
                Toggle("Record history", isOn: $store.recordsHistory)
                Text("Kept on this Mac for 24 hours · \(Format.bytes(size)) on disk")
                    .font(Tokens.Typography.caption)
                    .foregroundStyle(Tokens.Palette.textTertiary)
                Spacer()
                Button("Clear History…", role: .destructive) { confirmClear = true }
            }
        }
        .task(id: range) {
            while !Task.isCancelled {
                await load()
                try? await Task.sleep(for: .seconds(60))
            }
        }
        .task(id: AppQuery(metric: metric, minute: selectedMinute?.minute, range: range, loaded: minutes.count)) {
            await loadApps()
        }
        .confirmationDialog("Clear the history?", isPresented: $confirmClear) {
            Button("Clear History", role: .destructive) {
                Task {
                    await store.historyDatabase.clear()
                    selected = nil
                    await load()
                }
            }
        } message: {
            Text("Every saved minute is deleted from this Mac.")
        }
    }

    private struct AppQuery: Hashable {
        let metric: HistoryMetric
        let minute: Int?
        let range: Range
        let loaded: Int
    }

    private var availableMetrics: [HistoryMetric] {
        HistoryMetric.allCases.filter { metric in
            switch metric {
            case .gpu: store.capabilities.contains(.gpu)
            case .temperature: store.capabilities.contains(.temperature)
            case .power: store.capabilities.contains(.processEnergy)
            default: true
            }
        }
    }

    // MARK: - Chart

    private struct Point: Identifiable {
        let date: Date
        let value: Double
        let series: String
        var id: String { "\(series)-\(date.timeIntervalSince1970)" }
    }

    private var points: [Point] {
        minutes.flatMap { minute -> [Point] in
            guard let value = metric.value(minute) else { return [] }
            var result = [Point(date: minute.date, value: value, series: "Average")]
            if metric == .cpu { result.append(Point(date: minute.date, value: minute.cpuPeak, series: "Peak")) }
            return result
        }
    }

    private var selectedMinute: MachineMinute? {
        guard let selected else { return nil }
        let target = Int(selected.timeIntervalSince1970)
        return minutes.min { abs($0.minute - target) < abs($1.minute - target) }
    }

    private var chart: some View {
        let style = metric.style
        let start = Date().addingTimeInterval(-TimeInterval(range.rawValue))
        return Chart {
            ForEach(points.filter { $0.series == "Average" }) { point in
                AreaMark(x: .value("Time", point.date), y: .value(metric.title, point.value))
                    .foregroundStyle(
                        LinearGradient(
                            colors: [style.start.opacity(0.35), style.start.opacity(0.02)], startPoint: .top, endPoint: .bottom)
                    )
                    .interpolationMethod(.monotone)
                LineMark(x: .value("Time", point.date), y: .value(metric.title, point.value), series: .value("Series", "Average"))
                    .foregroundStyle(style.start)
                    .lineStyle(StrokeStyle(lineWidth: 1.5))
                    .interpolationMethod(.monotone)
            }
            ForEach(points.filter { $0.series == "Peak" }) { point in
                LineMark(x: .value("Time", point.date), y: .value(metric.title, point.value), series: .value("Series", "Peak"))
                    .foregroundStyle(style.end.opacity(0.6))
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [3, 3]))
                    .interpolationMethod(.monotone)
            }
            if let minute = selectedMinute, let value = metric.value(minute) {
                RuleMark(x: .value("Selected", minute.date))
                    .foregroundStyle(Tokens.Palette.textTertiary)
                    .annotation(position: .top, overflowResolution: .init(x: .fit(to: .chart), y: .disabled)) {
                        VStack(spacing: 2) {
                            Text(minute.date.formatted(date: .omitted, time: .shortened))
                                .font(Tokens.Typography.caption)
                                .foregroundStyle(Tokens.Palette.textSecondary)
                            Text(metric.format(value, temperatureUnit)).font(Tokens.Typography.headline.monospacedDigit())
                        }
                        .padding(.horizontal, Tokens.Space.sm)
                        .padding(.vertical, Tokens.Space.xs)
                        .background(Tokens.Palette.surfaceRaised, in: RoundedRectangle(cornerRadius: Tokens.Radius.sm))
                    }
            }
        }
        .chartXScale(domain: start...Date())
        .chartYScale(domain: 0...metric.ceiling(points.map(\.value).max() ?? 0))
        .chartYAxis {
            AxisMarks(position: .trailing, values: .automatic(desiredCount: 4)) { value in
                AxisGridLine().foregroundStyle(Tokens.Palette.border)
                AxisValueLabel {
                    if let number = value.as(Double.self) { Text(metric.format(number, temperatureUnit)) }
                }
            }
        }
        .chartXSelection(value: $selected)
    }

    // MARK: - Panels

    private var appsPanel: some View {
        let title =
            selectedMinute.map { "Busiest Apps at \($0.date.formatted(date: .omitted, time: .shortened))" }
            ?? "Busiest Apps, Last \(range.title)"
        return Panel(title, symbol: "flame.fill", tint: metric.style.start) {
            HStack {
                Spacer()
                if selected != nil {
                    Button("Show Whole Range") { selected = nil }.buttonStyle(.link)
                }
            }
        } content: {
            VStack(alignment: .leading, spacing: Tokens.Space.sm) {
                if apps.isEmpty {
                    Text(metric.appOrder == nil ? "Apps aren't tracked for this metric." : "No app activity saved here.")
                        .font(Tokens.Typography.body)
                        .foregroundStyle(Tokens.Palette.textTertiary)
                        .frame(maxWidth: .infinity, minHeight: 80)
                }
                let top = apps.first.flatMap { metric.appValue($0) } ?? 1
                ForEach(apps) { app in
                    HStack(spacing: Tokens.Space.sm + 2) {
                        AppIconView(bundlePath: app.bundlePath, size: 22)
                        VStack(alignment: .leading, spacing: Tokens.Space.xs) {
                            HStack {
                                Text(app.name).font(Tokens.Typography.headline).lineLimit(1)
                                Spacer()
                                Text(metric.appFormat(metric.appValue(app) ?? 0))
                                    .font(Tokens.Typography.headline.monospacedDigit())
                            }
                            UsageBar(value: min((metric.appValue(app) ?? 0) / max(top, 1e-9), 1), style: metric.style, height: 4)
                        }
                    }
                }
            }
        }
    }

    private var peaks: [MachineMinute] {
        // The busiest minutes, at least 10 minutes apart so one long spike shows once.
        var chosen: [MachineMinute] = []
        for minute in minutes.sorted(by: { (metric.value($0) ?? 0) > (metric.value($1) ?? 0) }) {
            guard metric.value(minute) != nil else { continue }
            if chosen.allSatisfy({ abs($0.minute - minute.minute) >= 600 }) { chosen.append(minute) }
            if chosen.count == 5 { break }
        }
        return chosen
    }

    private var peaksPanel: some View {
        Panel("Peaks", symbol: "arrow.up.to.line") {
            VStack(alignment: .leading, spacing: Tokens.Space.xs) {
                if peaks.isEmpty {
                    Text("Nothing saved yet.")
                        .font(Tokens.Typography.body)
                        .foregroundStyle(Tokens.Palette.textTertiary)
                        .frame(maxWidth: .infinity, minHeight: 80)
                }
                ForEach(peaks) { minute in
                    Button {
                        selected = minute.date
                    } label: {
                        HStack {
                            Text(minute.date.formatted(.dateTime.weekday(.abbreviated).hour().minute()))
                                .foregroundStyle(Tokens.Palette.textSecondary)
                            Spacer()
                            Text(metric.format(metric.value(minute) ?? 0, temperatureUnit))
                                .font(Tokens.Typography.headline.monospacedDigit())
                        }
                        .padding(.vertical, Tokens.Space.xs)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .help("Show the busiest apps of that minute")
                }
            }
        }
    }

    // MARK: - Data

    private func load() async {
        let start = Int(Date().timeIntervalSince1970) - range.rawValue
        minutes = await store.historyDatabase.machine(since: start)
        size = await store.historyDatabase.size()
        loaded = true
    }

    private func loadApps() async {
        guard let order = metric.appOrder else {
            apps = []
            return
        }
        let now = Int(Date().timeIntervalSince1970)
        if let minute = selectedMinute {
            apps = await store.historyDatabase.apps(from: minute.minute, to: minute.minute + 60, orderBy: order)
        } else {
            apps = await store.historyDatabase.apps(from: now - range.rawValue, to: now, orderBy: order)
        }
    }
}

/// What the History chart can show.
enum HistoryMetric: String, CaseIterable, Identifiable {
    case cpu, memory, disk, network, gpu, temperature, power

    var id: String { rawValue }

    var title: String {
        switch self {
        case .cpu: "CPU"
        case .memory: "Memory"
        case .disk: "Disk"
        case .network: "Network"
        case .gpu: "GPU"
        case .temperature: "Temperature"
        case .power: "Energy"
        }
    }

    var style: MetricStyle {
        switch self {
        case .cpu, .temperature: Metric.cpu.style
        case .memory: Metric.memory.style
        case .disk: Metric.disk.style
        case .network: Metric.network.style
        case .gpu: Metric.gpu.style
        case .power: Metric.energy.style
        }
    }

    func value(_ m: MachineMinute) -> Double? {
        switch self {
        case .cpu: m.cpu
        case .memory: m.memory
        case .disk: m.diskRead + m.diskWrite
        case .network: m.networkReceive + m.networkSend
        case .gpu: m.gpu
        case .temperature: m.cpuTemperature
        case .power: m.appPower
        }
    }

    func format(_ value: Double, _ unit: TemperatureUnit) -> String {
        switch self {
        case .cpu, .memory, .gpu: Format.percent(value)
        case .disk, .network: Format.rate(value)
        case .temperature: Format.temperature(value, unit: unit)
        case .power: Format.power(value)
        }
    }

    func ceiling(_ peak: Double) -> Double {
        switch self {
        case .cpu, .memory, .gpu: 1
        case .temperature: max(100, peak)
        default: Sparkline.niceCeiling(max(peak, 1e-6))
        }
    }

    var appOrder: AppUsageOrder? {
        switch self {
        case .cpu: .cpu
        case .memory: .memory
        case .disk: .disk
        case .network: .network
        case .gpu: .gpu
        case .power: .power
        case .temperature: .cpu  // what heats the chip
        }
    }

    func appValue(_ app: AppUsage) -> Double? {
        switch self {
        case .cpu, .temperature: app.cpu
        case .memory: app.memory
        case .disk: app.disk
        case .network: app.network
        case .gpu: app.gpu
        case .power: app.power
        }
    }

    func appFormat(_ value: Double) -> String {
        switch self {
        case .cpu, .temperature, .gpu: Format.cpu(value)
        case .memory: Format.bytes(Int64(value))
        case .disk, .network: Format.rate(value)
        case .power: Format.power(value)
        }
    }
}

/// App icon by bundle path, or a generic glyph.
struct AppIconView: View {
    let bundlePath: String?
    var size: CGFloat = 16

    var body: some View {
        if let bundlePath {
            Image(nsImage: IconCache.icon(for: bundlePath))
                .resizable()
                .interpolation(.high)
                .frame(width: size, height: size)
        } else {
            RoundedRectangle(cornerRadius: size * 0.25, style: .continuous)
                .fill(Tokens.Palette.surfaceSunken)
                .overlay {
                    Image(systemName: "terminal.fill")
                        .font(.system(size: size * 0.52, weight: .semibold))
                        .foregroundStyle(Tokens.Palette.textTertiary)
                }
                .frame(width: size, height: size)
        }
    }
}
