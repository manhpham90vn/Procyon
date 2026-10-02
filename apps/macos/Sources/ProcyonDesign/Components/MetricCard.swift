import SwiftUI

/// Dashboard card: metric identity, headline value, optional ring gauge and a live sparkline.
public struct MetricCard: View {
    var title: String
    var style: MetricStyle
    var value: String
    var unit: String
    var caption: String
    var gauge: Double?
    var series: [ChartSeries]
    var maxValue: Double?

    @State private var hovering = false

    public init(
        title: String, style: MetricStyle, value: String, unit: String = "", caption: String,
        gauge: Double? = nil, series: [ChartSeries], maxValue: Double? = nil
    ) {
        self.title = title
        self.style = style
        self.value = value
        self.unit = unit
        self.caption = caption
        self.gauge = gauge
        self.series = series
        self.maxValue = maxValue
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(alignment: .top) {
                VStack(alignment: .leading, spacing: Tokens.Space.sm) {
                    HStack(spacing: Tokens.Space.sm) {
                        MetricIcon(style, size: 24)
                        Text(title).font(Tokens.Typography.headline).foregroundStyle(Tokens.Palette.textSecondary)
                    }
                    ValueText(value, unit: unit)
                    Text(caption)
                        .font(Tokens.Typography.label)
                        .foregroundStyle(Tokens.Palette.textTertiary)
                        .lineLimit(1)
                }
                Spacer(minLength: Tokens.Space.sm)
                if let gauge {
                    RingGauge(value: gauge, style: style, lineWidth: 7)
                        .frame(width: 54, height: 54)
                }
            }
            .padding([.horizontal, .top], Tokens.Space.lg)

            Sparkline(series: series, maxValue: maxValue)
                .frame(height: 64)
                .padding(.top, Tokens.Space.md)
        }
        .cardSurface(padding: 0, tint: style.start)
        .scaleEffect(hovering ? 1.01 : 1)
        .animation(.spring(duration: Tokens.Motion.normal), value: hovering)
        .onHover { hovering = $0 }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(title) \(value) \(unit)")
    }
}

/// Grid of per-core mini charts.
public struct CoreGrid: View {
    public struct Core: Identifiable, Sendable {
        public let id: Int
        public let label: String
        public let usage: Double
        public let samples: [ChartSample]
        /// Short tag shown next to the label ("P", "E"); nil for none.
        public let badge: String?
        public let badgeTone: Badge.Tone

        public init(
            id: Int, label: String, usage: Double, samples: [ChartSample], badge: String? = nil,
            badgeTone: Badge.Tone = .neutral
        ) {
            self.id = id
            self.label = label
            self.usage = usage
            self.samples = samples
            self.badge = badge
            self.badgeTone = badgeTone
        }
    }

    var cores: [Core]
    var style: MetricStyle

    public init(cores: [Core], style: MetricStyle) {
        self.cores = cores
        self.style = style
    }

    public var body: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 128), spacing: Tokens.Space.sm)], spacing: Tokens.Space.sm) {
            ForEach(cores) { core in
                VStack(alignment: .leading, spacing: Tokens.Space.xs) {
                    HStack(spacing: Tokens.Space.xs) {
                        if let badge = core.badge { Badge(badge, tone: core.badgeTone) }
                        Text(core.label).font(Tokens.Typography.caption).foregroundStyle(Tokens.Palette.textSecondary)
                        Spacer()
                        Text(Format.percent(core.usage))
                            .font(Tokens.Typography.label.monospacedDigit())
                            .foregroundStyle(Tokens.Palette.textPrimary)
                    }
                    Sparkline(
                        samples: core.samples, color: core.usage > 0.85 ? style.end : style.start,
                        maxValue: 1, lineWidth: 1.25, glow: false
                    )
                    .frame(height: 34)
                }
                .padding(Tokens.Space.sm)
                .background(
                    Tokens.Palette.surfaceSunken, in: RoundedRectangle(cornerRadius: Tokens.Radius.md, style: .continuous)
                )
                .overlay(alignment: .bottom) {
                    UsageBar(value: core.usage, style: style, height: 2)
                        .padding(.horizontal, Tokens.Space.sm)
                }
            }
        }
    }
}
