import ProcyonDesign
import ProcyonKit
import SwiftUI

enum BatteryGlyph {
    static func symbol(for battery: Battery) -> String {
        if battery.isCharging { return "battery.100percent.bolt" }
        switch battery.level {
        case ..<0.13: return "battery.0percent"
        case ..<0.38: return "battery.25percent"
        case ..<0.63: return "battery.50percent"
        case ..<0.88: return "battery.75percent"
        default: return "battery.100percent"
        }
    }
}

struct BatteryView: View {
    @Environment(SystemStore.self) private var store
    @AppStorage(TemperatureUnit.storageKey) private var temperatureUnit: TemperatureUnit = .system
    @State private var assertions: [PowerAssertion] = []
    @State private var holders: [Int32: ProcessSummary] = [:]
    @State private var loaded = false

    var body: some View {
        let style = Metric.battery.style
        ScreenScroll {
            if let battery = store.battery {
                PageHeader("Battery", subtitle: battery.stateTitle, style: style) {
                    ValueText(
                        Format.percent(battery.level).dropLast().description, unit: "%", font: Tokens.Typography.display)
                }

                HStack(alignment: .top, spacing: Tokens.Space.lg) {
                    Panel("Charge", symbol: BatteryGlyph.symbol(for: battery), tint: style.start) {
                        HStack(spacing: Tokens.Space.xl) {
                            RingGauge(value: battery.level, style: style, lineWidth: 10) {
                                Image(
                                    systemName: battery.isCharging
                                        ? "bolt.fill" : battery.onACPower ? "powerplug.fill" : "leaf.fill"
                                )
                                .font(.system(size: 20, weight: .semibold))
                                .foregroundStyle(style.end)
                            }
                            .frame(width: 96, height: 96)
                            StatGrid(chargeItems(battery), columns: 2)
                        }
                    }
                    Panel("Health", symbol: "heart.text.square") {
                        StatGrid(healthItems(battery), columns: 2)
                    }
                }
                .equalHeightPanels()

                sleepPanel
            } else {
                EmptyState(symbol: "battery.0percent", title: "No battery", message: "This Mac runs on external power.")
            }
        }
        .task {
            // Assertions come and go (a video starts, a download ends): poll while the page is open.
            while !Task.isCancelled {
                await loadAssertions()
                try? await Task.sleep(for: .seconds(5))
            }
        }
    }

    private func chargeItems(_ b: Battery) -> [StatItem] {
        // The adapter name is long ("70W USB-C Power Adapter"): a caption under the state, not a value.
        var items = [StatItem("State", value: b.stateTitle, detail: b.adapter.isEmpty ? nil : b.adapter)]
        if b.isCharging {
            items.append(StatItem("Until full", value: b.timeToFull.map(Format.duration) ?? "Calculating…"))
        } else if !b.onACPower {
            items.append(StatItem("Remaining", value: b.timeToEmpty.map(Format.duration) ?? "Calculating…"))
        }
        if let power = b.power {
            items.append(StatItem(power < 0 ? "Drawing" : "Charging at", value: Format.watts(abs(power))))
        }
        return items
    }

    private func healthItems(_ b: Battery) -> [StatItem] {
        var items: [StatItem] = []
        if let health = b.health {
            items.append(
                StatItem(
                    "Maximum capacity", value: Format.percent(health),
                    tint: health >= 0.8 ? Tokens.Palette.success : Tokens.Palette.warning))
        }
        if !b.condition.isEmpty { items.append(StatItem("Condition", value: b.condition)) }
        if let cycles = b.cycleCount { items.append(StatItem("Cycle count", value: Format.count(cycles))) }
        if let full = b.maximumCapacity {
            items.append(StatItem("Full charge", value: "\(full) mAh", detail: b.designCapacity.map { "Design \($0) mAh" }))
        }
        if let temperature = b.temperature {
            items.append(StatItem("Temperature", value: Format.temperature(temperature, unit: temperatureUnit)))
        }
        return items
    }

    // MARK: - Sleep

    private var sleepPanel: some View {
        Panel("Apps Preventing Sleep", symbol: "moon.zzz.fill") {
            VStack(alignment: .leading, spacing: Tokens.Space.sm) {
                if !loaded {
                    ProgressView().frame(maxWidth: .infinity, minHeight: 60)
                } else if assertions.isEmpty {
                    Text("Nothing is keeping your Mac awake.")
                        .font(Tokens.Typography.body)
                        .foregroundStyle(Tokens.Palette.textTertiary)
                        .frame(maxWidth: .infinity, minHeight: 60)
                }
                ForEach(groupedAssertions, id: \.pid) { group in
                    assertionRow(group)
                }
            }
        }
    }

    /// One row per app: assertions a daemon holds for an app (audio for a player) count as the app's.
    private var groupedAssertions: [(pid: Int32, items: [PowerAssertion])] {
        Dictionary(grouping: assertions) { $0.onBehalfOf ?? $0.pid }
            .map { (pid: $0.key, items: $0.value) }
            .sorted { name(for: $0.pid, $0.items) < name(for: $1.pid, $1.items) }
    }

    private func name(for pid: Int32, _ items: [PowerAssertion]) -> String {
        if let summary = holders[pid] { return summary.appName.isEmpty ? summary.name : summary.appName }
        return items.first(where: { $0.pid == pid })?.processName ?? "PID \(pid)"
    }

    private func assertionRow(_ group: (pid: Int32, items: [PowerAssertion])) -> some View {
        let display = group.items.contains(where: \.preventsDisplaySleep)
        let via = Set(group.items.filter { $0.onBehalfOf != nil }.map(\.processName)).sorted()
        let reasons = Array(Set(group.items.map(\.reason).filter { !$0.isEmpty })).sorted()
        return HStack(alignment: .top, spacing: Tokens.Space.sm + 2) {
            icon(for: group.pid)
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: Tokens.Space.sm) {
                    Text(name(for: group.pid, group.items))
                        .font(Tokens.Typography.headline)
                        .foregroundStyle(Tokens.Palette.textPrimary)
                    Badge(display ? "Keeps display on" : "Prevents sleep", tone: display ? .warning : .accent)
                    if !via.isEmpty {
                        Text("via \(via.joined(separator: ", "))")
                            .font(Tokens.Typography.caption)
                            .foregroundStyle(Tokens.Palette.textTertiary)
                    }
                }
                Text(reasons.prefix(3).joined(separator: " · "))
                    .font(Tokens.Typography.label)
                    .foregroundStyle(Tokens.Palette.textSecondary)
                    .lineLimit(2)
                    .textSelection(.enabled)
            }
            Spacer()
            Text("PID \(group.pid)")
                .font(Tokens.Typography.caption.monospacedDigit())
                .foregroundStyle(Tokens.Palette.textTertiary)
        }
        .padding(.vertical, Tokens.Space.xs)
    }

    @ViewBuilder
    private func icon(for pid: Int32) -> some View {
        if let bundle = holders[pid]?.bundlePath {
            Image(nsImage: IconCache.icon(for: bundle, size: 22)).frame(width: 22, height: 22)
        } else {
            Image(systemName: "gearshape.2.fill")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(Tokens.Palette.textTertiary)
                .frame(width: 22, height: 22)
                .background(Tokens.Palette.surfaceSunken, in: RoundedRectangle(cornerRadius: 5, style: .continuous))
        }
    }

    private func loadAssertions() async {
        let list = await store.powerAssertions()
        let pids = Set(list.flatMap { [$0.pid] + ($0.onBehalfOf.map { [$0] } ?? []) })
        holders = await store.summaries(for: pids)
        assertions = list
        loaded = true
    }
}
