import SwiftUI

/// Full-size live chart: grid, y-axis scale, time axis and legend around a `Sparkline`.
public struct LiveChart: View {
    public struct Legend: Identifiable, Sendable {
        public let id: String
        public let title: String
        public let value: String
        public let color: Color

        public init(_ title: String, value: String, color: Color) {
            self.id = title
            self.title = title
            self.value = value
            self.color = color
        }
    }

    var series: [ChartSeries]
    var legend: [Legend]
    var window: TimeInterval
    var maxValue: Double?
    var axisLabel: (Double) -> String

    public init(
        series: [ChartSeries], legend: [Legend] = [], window: TimeInterval = 60, maxValue: Double? = nil,
        axisLabel: @escaping (Double) -> String
    ) {
        self.series = series
        self.legend = legend
        self.window = window
        self.maxValue = maxValue
        self.axisLabel = axisLabel
    }

    private var scale: Double {
        maxValue ?? Sparkline.niceCeiling(series.flatMap { $0.samples.map(\.value) }.max() ?? 0)
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: Tokens.Space.md) {
            if !legend.isEmpty {
                HStack(spacing: Tokens.Space.lg) {
                    ForEach(legend) { item in
                        HStack(spacing: Tokens.Space.xs + 2) {
                            Capsule().fill(item.color).frame(width: 10, height: 4)
                            Text(item.title).font(Tokens.Typography.label).foregroundStyle(Tokens.Palette.textSecondary)
                            Text(item.value).font(Tokens.Typography.headline.monospacedDigit()).foregroundStyle(
                                Tokens.Palette.textPrimary)
                        }
                    }
                    Spacer()
                }
            }

            HStack(alignment: .top, spacing: Tokens.Space.sm) {
                Sparkline(series: series, window: window, maxValue: scale, gridLines: 4, lineWidth: 2)
                VStack(alignment: .leading) {
                    Text(axisLabel(scale))
                    Spacer()
                    Text(axisLabel(scale / 2))
                    Spacer()
                    Text(axisLabel(0))
                }
                .font(Tokens.Typography.caption.monospacedDigit())
                .foregroundStyle(Tokens.Palette.textTertiary)
                .frame(minWidth: 44, alignment: .leading)
            }

            HStack {
                Text("\(Int(window)) seconds")
                Spacer()
                Text("now")
                    .padding(.trailing, 52)
            }
            .font(Tokens.Typography.caption)
            .foregroundStyle(Tokens.Palette.textTertiary)
        }
    }
}
