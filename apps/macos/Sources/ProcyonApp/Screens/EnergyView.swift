import ProcyonDesign
import ProcyonKit
import SwiftUI

/// Which apps use the most power now and since Procyon started (the Energy tab of Activity Monitor).
struct EnergyView: View {
    @Environment(SystemStore.self) private var store

    var body: some View {
        let style = Metric.energy.style
        let power = store.sample.appPower
        ScreenScroll {
            PageHeader("Energy", subtitle: "Power the system attributes to each app", style: style) {
                ValueText(
                    Format.power(power).replacingOccurrences(of: " W", with: ""), unit: "W", font: Tokens.Typography.display)
            }

            Panel("All Apps", symbol: "bolt.fill", tint: style.start) {
                LiveChart(
                    series: [ChartSeries(id: "power", samples: store.history.appPower.samples, color: style.start)],
                    legend: [
                        .init("Now", value: Format.power(power), color: style.start),
                        .init("Peak", value: Format.power(store.history.appPower.peak), color: style.end),
                    ],
                    axisLabel: { Format.power($0) }
                )
                .frame(height: 200)
            }

            StatGrid(stats, minimumWidth: 180)

            HStack(alignment: .top, spacing: Tokens.Space.lg) {
                TopAppsPanel(
                    title: "Using Power Now", symbol: "bolt.fill", tint: style.start, rows: store.topPower,
                    isLoading: !store.hasSample, style: style, emptyText: "No app is drawing noticeable power.",
                    fraction: { ($0.power ?? 0) / max(store.topPower.first?.power ?? 1, 0.01) }
                ) { TopAppValue(text: Format.power($0.power)) }

                TopAppsPanel(
                    title: "Most Energy Since \(store.energySince.formatted(date: .omitted, time: .shortened))",
                    symbol: "sum", tint: style.end, rows: store.topEnergy.map(\.row), isLoading: !store.hasSample,
                    style: style, emptyText: "Nothing measured yet.",
                    fraction: { row in energy(row) / max(store.topEnergy.first?.wattHours ?? 1, 0.0001) }
                ) { TopAppValue(text: Self.wattHours(energy($0))) }
            }
            .equalHeightPanels()

            InfoBanner(
                "macOS measures this energy on Apple Silicon (CPU, GPU and other blocks the app keeps busy) and estimates it on Intel Macs. Displays, Wi‑Fi and other shared hardware aren't billed to any app. Energy since start counts while Procyon's window is open.",
                symbol: "info.circle")
        }
    }

    private var stats: [StatItem] {
        var items = [
            StatItem("Apps now", value: Format.power(store.sample.appPower), detail: "Sum of every process"),
            StatItem(
                "Apps since start", value: Self.wattHours(store.topEnergy.reduce(0) { $0 + $1.wattHours }),
                detail: "Top \(SystemStore.topCount) apps"),
        ]
        // The whole machine's draw, from the battery gauge (unknown on desktops and when fully charged).
        if let battery = store.battery, let draw = battery.power {
            items.append(StatItem("Whole Mac", value: Format.watts(abs(draw)), detail: battery.stateTitle))
        }
        return items
    }

    private func energy(_ row: ProcessRow) -> Double {
        store.topEnergy.first { $0.id == row.appID }?.wattHours ?? 0
    }

    static func wattHours(_ value: Double) -> String {
        switch value {
        case ..<0.0001: "0 mWh"
        case ..<0.01: String(format: "%.1f mWh", value * 1000)
        case ..<1: String(format: "%.0f mWh", value * 1000)
        default: String(format: "%.2f Wh", value)
        }
    }
}
