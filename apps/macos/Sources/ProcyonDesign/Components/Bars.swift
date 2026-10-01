import SwiftUI

/// Horizontal capsule meter. `value` is 0…1.
public struct UsageBar: View {
    var value: Double
    var style: MetricStyle
    var height: CGFloat

    public init(value: Double, style: MetricStyle, height: CGFloat = 6) {
        self.value = value
        self.style = style
        self.height = height
    }

    public var body: some View {
        GeometryReader { proxy in
            let fraction = min(max(value.isFinite ? value : 0, 0), 1)
            ZStack(alignment: .leading) {
                Capsule().fill(Tokens.Palette.track)
                Capsule()
                    .fill(style.horizontalGradient)
                    .frame(width: max(fraction > 0 ? height : 0, proxy.size.width * fraction))
            }
        }
        .frame(height: height)
        .accessibilityValue(Format.percent(value))
    }
}

/// Proportional bar made of labelled segments, with a legend underneath.
public struct StackedBar: View {
    public struct Segment: Identifiable, Sendable {
        public let id: String
        public let title: String
        public let value: Double
        public let detail: String
        public let color: Color

        public init(_ title: String, value: Double, detail: String, color: Color) {
            self.id = title
            self.title = title
            self.value = value
            self.detail = detail
            self.color = color
        }
    }

    var segments: [Segment]
    var height: CGFloat

    public init(segments: [Segment], height: CGFloat = 14) {
        self.segments = segments
        self.height = height
    }

    public var body: some View {
        let total = max(segments.reduce(0) { $0 + max($1.value, 0) }, .leastNonzeroMagnitude)
        VStack(alignment: .leading, spacing: Tokens.Space.md) {
            GeometryReader { proxy in
                HStack(spacing: 2) {
                    ForEach(segments) { segment in
                        RoundedRectangle(cornerRadius: Tokens.Radius.xs, style: .continuous)
                            .fill(segment.color)
                            .frame(width: max(0, (proxy.size.width - CGFloat(segments.count - 1) * 2) * segment.value / total))
                    }
                }
            }
            .frame(height: height)
            .clipShape(RoundedRectangle(cornerRadius: Tokens.Radius.sm, style: .continuous))

            LazyVGrid(
                columns: [GridItem(.adaptive(minimum: 130), alignment: .leading)], alignment: .leading, spacing: Tokens.Space.sm
            ) {
                ForEach(segments) { segment in
                    HStack(spacing: Tokens.Space.sm) {
                        RoundedRectangle(cornerRadius: 3).fill(segment.color).frame(width: 10, height: 10)
                        VStack(alignment: .leading, spacing: 0) {
                            Text(segment.title).font(Tokens.Typography.label).foregroundStyle(Tokens.Palette.textSecondary)
                            Text(segment.detail).font(Tokens.Typography.headline.monospacedDigit()).foregroundStyle(
                                Tokens.Palette.textPrimary)
                        }
                    }
                }
            }
        }
    }
}
