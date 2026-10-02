import ProcyonDesign
import ProcyonKit
import SwiftUI

struct Sidebar: View {
    @Binding var page: Page
    @Environment(SystemStore.self) private var store

    var body: some View {
        // A plain stack instead of List: NSOutlineView re-measures every row on each live update,
        // which cost ~30% CPU. The pages scroll in an AppKit scroll view (SwiftUI's ScrollView
        // offsets the rows' hit areas from where they are drawn in this hidden-title-bar window);
        // the controls at the bottom stay put.
        VStack(alignment: .leading, spacing: 0) {
            AppKitScrollView {
                pages
                    .environment(store)
            }

            Divider()
                .padding(.horizontal, Tokens.Space.md)
            VStack(alignment: .leading, spacing: Tokens.Space.sm) {
                LiveControls()
                SidebarItem(page: .settings, selection: $page) {
                    PageRow(page: .settings, detail: "Updates, menu bar, appearance")
                }
            }
            .padding(.horizontal, Tokens.Space.sm + 2)
            .padding(.top, Tokens.Space.sm)
            .padding(.bottom, Tokens.Space.md)
        }
    }

    private var pages: some View {
        VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 2) {
                Brand()
                    .padding(.horizontal, Tokens.Space.sm)
                    .padding(.bottom, Tokens.Space.sm)

                SidebarItem(page: .overview, selection: $page) {
                    PageRow(page: .overview, detail: store.info.displayModel)
                }
                SidebarItem(page: .processes, selection: $page) {
                    PageRow(
                        page: .processes, detail: store.hasSample ? "\(Format.count(store.sample.processCount)) running" : " ")
                }

                SectionLabel("Performance")
                SidebarItem(page: .cpu, selection: $page) {
                    MetricRow(
                        metric: .cpu, value: Format.percent(store.sample.cpuUsage),
                        series: [ChartSeries(id: "cpu", samples: store.history.cpu.samples, color: Metric.cpu.style.start)],
                        maxValue: 1)
                }
                SidebarItem(page: .memory, selection: $page) {
                    MetricRow(
                        metric: .memory,
                        value: "\(Format.bytes(store.sample.memoryUsed)) · \(Format.percent(store.sample.memoryFraction))",
                        series: [ChartSeries(id: "mem", samples: store.history.memory.samples, color: Metric.memory.style.start)],
                        maxValue: 1)
                }
                if store.capabilities.contains(.gpu) {
                    SidebarItem(page: .gpu, selection: $page) {
                        MetricRow(
                            metric: .gpu, value: Format.percent(store.sample.gpuUsage),
                            series: [ChartSeries(id: "gpu", samples: store.history.gpu.samples, color: Metric.gpu.style.start)],
                            maxValue: 1)
                    }
                }
                SidebarItem(page: .disk, selection: $page) {
                    MetricRow(
                        metric: .disk,
                        value: Format.rate(store.sample.diskReadRate + store.sample.diskWriteRate),
                        series: [
                            ChartSeries(id: "r", samples: store.history.diskRead.samples, color: Metric.disk.style.start),
                            ChartSeries(
                                id: "w", samples: store.history.diskWrite.samples, color: Metric.disk.style.end, fills: false),
                        ])
                }
                SidebarItem(page: .network, selection: $page) {
                    MetricRow(
                        metric: .network,
                        value:
                            "↓ \(Format.bytes(store.sample.networkReceiveRate)) ↑ \(Format.bytes(store.sample.networkSendRate))",
                        series: [
                            ChartSeries(
                                id: "rx", samples: store.history.networkReceive.samples, color: Metric.network.style.start),
                            ChartSeries(
                                id: "tx", samples: store.history.networkSend.samples, color: Metric.network.style.end,
                                fills: false),
                        ])
                }

                if Page.energy.isAvailable(store.capabilities) {
                    SidebarItem(page: .energy, selection: $page) {
                        MetricRow(
                            metric: .energy, value: "Apps · \(Format.power(store.sample.appPower))",
                            series: [
                                ChartSeries(
                                    id: "power", samples: store.history.appPower.samples, color: Metric.energy.style.start)
                            ])
                    }
                }

                if Page.startup.isAvailable(store.capabilities) || Page.services.isAvailable(store.capabilities) {
                    SectionLabel("Manage")
                    if Page.startup.isAvailable(store.capabilities) {
                        SidebarItem(page: .startup, selection: $page) {
                            PageRow(page: .startup, detail: "Apps that start with your Mac")
                        }
                    }
                    if Page.services.isAvailable(store.capabilities) {
                        SidebarItem(page: .services, selection: $page) {
                            PageRow(page: .services, detail: "Background services")
                        }
                    }
                }

                SectionLabel("Analyze")
                SidebarItem(page: .history, selection: $page) {
                    PageRow(page: .history, detail: store.recordsHistory ? "The last 24 hours" : "Off")
                }
                if Page.inspect.isAvailable(store.capabilities) {
                    SidebarItem(page: .inspect, selection: $page) {
                        PageRow(page: .inspect, detail: "Who uses a port or a file")
                    }
                }

                SectionLabel("Machine")
                if let battery = store.battery {
                    SidebarItem(page: .battery, selection: $page) {
                        PageRow(
                            page: .battery, symbol: BatteryGlyph.symbol(for: battery),
                            detail: "\(Format.percent(battery.level)) · \(battery.stateTitle)")
                    }
                }
                SidebarItem(page: .system, selection: $page) {
                    PageRow(page: .system, detail: "\(store.info.osName) \(store.info.osVersion)")
                }
            }
            .padding(.horizontal, Tokens.Space.sm + 2)
            .padding(.top, Tokens.Space.xs)
            .padding(.bottom, Tokens.Space.md)
        }
    }
}

private struct SectionLabel: View {
    let title: String
    init(_ title: String) { self.title = title }

    var body: some View {
        Text(title)
            .font(Tokens.Typography.caption)
            .foregroundStyle(Tokens.Palette.textTertiary)
            .padding(.horizontal, Tokens.Space.sm)
            .padding(.top, Tokens.Space.lg)
            .padding(.bottom, Tokens.Space.xs)
    }
}

private struct SidebarItem<Content: View>: View {
    let page: Page
    @Binding var selection: Page
    @ViewBuilder var content: Content

    var body: some View {
        // No onHover here: hover tracking areas get rebuilt on every live update and can miss
        // the mouse-exit event, leaving a row highlighted where the pointer isn't.
        Button {
            selection = page
        } label: {
            content
        }
        .buttonStyle(SidebarItemStyle(selected: selection == page))
        .accessibilityAddTraits(selection == page ? .isSelected : [])
    }
}

private struct SidebarItemStyle: ButtonStyle {
    let selected: Bool

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .padding(.horizontal, Tokens.Space.sm)
            .padding(.vertical, Tokens.Space.xs + 1)
            .frame(maxWidth: .infinity, alignment: .leading)
            .contentShape(RoundedRectangle(cornerRadius: Tokens.Radius.md, style: .continuous))
            .background {
                RoundedRectangle(cornerRadius: Tokens.Radius.md, style: .continuous)
                    .fill(
                        selected
                            ? Tokens.Palette.accent.opacity(0.16)
                            : configuration.isPressed ? Tokens.Palette.track : .clear)
            }
            .overlay {
                if selected {
                    RoundedRectangle(cornerRadius: Tokens.Radius.md, style: .continuous)
                        .strokeBorder(Tokens.Palette.accent.opacity(0.28), lineWidth: 1)
                }
            }
    }
}

private struct Brand: View {
    var body: some View {
        HStack(spacing: Tokens.Space.sm) {
            ZStack {
                Circle().fill(
                    LinearGradient(
                        colors: [Metric.cpu.style.start, Metric.memory.style.start],
                        startPoint: .topLeading, endPoint: .bottomTrailing))
                Image(systemName: "sparkle")
                    .font(.system(size: 11, weight: .bold))
                    .foregroundStyle(.white)
            }
            .frame(width: 22, height: 22)
            .shadow(color: Metric.cpu.style.start.opacity(0.5), radius: 6)
            Text("Procyon")
                .font(.system(size: 15, weight: .bold, design: .rounded))
                .foregroundStyle(Tokens.Palette.textPrimary)
        }
    }
}

private struct PageRow: View {
    let page: Page
    var symbol: String?
    let detail: String

    var body: some View {
        HStack(spacing: Tokens.Space.sm + 2) {
            Image(systemName: symbol ?? page.symbol)
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(Tokens.Palette.accent)
                .frame(width: 22, height: 22)
            VStack(alignment: .leading, spacing: 0) {
                Text(page.title).font(Tokens.Typography.headline).foregroundStyle(Tokens.Palette.textPrimary)
                Text(detail).font(Tokens.Typography.caption).foregroundStyle(Tokens.Palette.textSecondary).lineLimit(1)
            }
        }
    }
}

private struct MetricRow: View {
    let metric: Metric
    let value: String
    let series: [ChartSeries]
    var maxValue: Double?

    var body: some View {
        HStack(spacing: Tokens.Space.sm + 2) {
            MetricIcon(metric.style, size: 22)
            VStack(alignment: .leading, spacing: 0) {
                Text(metric.title).font(Tokens.Typography.headline).foregroundStyle(Tokens.Palette.textPrimary)
                Text(value)
                    .font(Tokens.Typography.caption.monospacedDigit())
                    .foregroundStyle(Tokens.Palette.textSecondary)
                    .lineLimit(1)
            }
            Spacer(minLength: Tokens.Space.xs)
            Sparkline(series: series, maxValue: maxValue, lineWidth: 1.25, glow: false)
                .frame(width: 46, height: 22)
        }
    }
}

/// Live/paused status pinned to the sidebar bottom; the update speed is in Settings.
private struct LiveControls: View {
    @Environment(SystemStore.self) private var store

    var body: some View {
        HStack(spacing: Tokens.Space.sm) {
            LiveIndicator(isLive: !store.isPaused)
            Text(store.isPaused ? "Paused" : "Live")
                .font(Tokens.Typography.label)
                .foregroundStyle(Tokens.Palette.textPrimary)
            Spacer()
            Button {
                store.isPaused.toggle()
            } label: {
                Image(systemName: store.isPaused ? "play.fill" : "pause.fill")
                    .frame(width: 16)
            }
            .buttonStyle(.borderless)
            .help(store.isPaused ? "Resume updates (⇧⌘P)" : "Pause updates (⇧⌘P)")
        }
        .padding(.horizontal, Tokens.Space.md)
        .padding(.vertical, Tokens.Space.sm)
        .glassChrome(radius: Tokens.Radius.lg)
    }
}
