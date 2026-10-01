import SwiftUI

/// Circular gauge with a gradient arc and a soft glow. `value` is 0…1.
public struct RingGauge<Center: View>: View {
    var value: Double
    var style: MetricStyle
    var lineWidth: CGFloat
    var center: Center

    public init(
        value: Double, style: MetricStyle, lineWidth: CGFloat = Tokens.Chart.gaugeLineWidth,
        @ViewBuilder center: () -> Center
    ) {
        self.value = value
        self.style = style
        self.lineWidth = lineWidth
        self.center = center()
    }

    private var clamped: Double { min(max(value.isFinite ? value : 0, 0), 1) }

    public var body: some View {
        ZStack {
            Circle()
                .stroke(Tokens.Palette.track, lineWidth: lineWidth)
            // Halo: a wider translucent arc (cheaper than a shadow on an animating shape).
            Circle()
                .trim(from: 0, to: max(clamped, 0.001))
                .stroke(style.end.opacity(0.18), style: StrokeStyle(lineWidth: lineWidth * 1.9, lineCap: .round))
                .rotationEffect(.degrees(-90))
            Circle()
                .trim(from: 0, to: max(clamped, 0.001))
                .stroke(
                    AngularGradient(
                        colors: [style.start, style.end], center: .center,
                        startAngle: .degrees(0), endAngle: .degrees(360 * max(clamped, 0.05))),
                    style: StrokeStyle(lineWidth: lineWidth, lineCap: .round)
                )
                .rotationEffect(.degrees(-90))
            center
        }
        .padding(lineWidth / 2)
        .accessibilityElement(children: .combine)
        .accessibilityValue(Format.percent(clamped))
    }
}

public extension RingGauge where Center == EmptyView {
    init(value: Double, style: MetricStyle, lineWidth: CGFloat = Tokens.Chart.gaugeLineWidth) {
        self.init(value: value, style: style, lineWidth: lineWidth) { EmptyView() }
    }
}
