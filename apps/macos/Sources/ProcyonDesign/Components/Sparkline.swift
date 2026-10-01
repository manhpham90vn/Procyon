import SwiftUI

public struct ChartSample: Sendable, Hashable {
    public var time: TimeInterval
    public var value: Double

    public init(time: TimeInterval, value: Double) {
        self.time = time
        self.value = value
    }
}

public struct ChartSeries: Identifiable, Sendable {
    public let id: String
    public var samples: [ChartSample]
    public var color: Color
    public var fills: Bool

    public init(id: String, samples: [ChartSample], color: Color, fills: Bool = true) {
        self.id = id
        self.samples = samples
        self.color = color
        self.fills = fills
    }
}

/// Lightweight time-series chart drawn with Canvas (no per-point views).
/// The newest sample sits on the right edge; `window` seconds are visible.
public struct Sparkline: View {
    var series: [ChartSeries]
    var window: TimeInterval
    var maxValue: Double?
    var gridLines: Int
    var lineWidth: CGFloat
    var glow: Bool

    public init(
        series: [ChartSeries],
        window: TimeInterval = 60,
        maxValue: Double? = nil,
        gridLines: Int = 0,
        lineWidth: CGFloat = Tokens.Chart.lineWidth,
        glow: Bool = true
    ) {
        self.series = series
        self.window = window
        self.maxValue = maxValue
        self.gridLines = gridLines
        self.lineWidth = lineWidth
        self.glow = glow
    }

    /// Single-series convenience.
    public init(
        samples: [ChartSample], color: Color, window: TimeInterval = 60, maxValue: Double? = nil,
        gridLines: Int = 0, lineWidth: CGFloat = Tokens.Chart.lineWidth, glow: Bool = true
    ) {
        self.init(
            series: [ChartSeries(id: "value", samples: samples, color: color)], window: window,
            maxValue: maxValue, gridLines: gridLines, lineWidth: lineWidth, glow: glow)
    }

    public var body: some View {
        Canvas { context, size in
            let end = series.compactMap { $0.samples.last?.time }.max() ?? 0
            let scale = maxValue ?? Sparkline.niceCeiling(series.flatMap { $0.samples.map(\.value) }.max() ?? 0)

            if gridLines > 0 {
                for line in 0...gridLines {
                    let y = (size.height - lineWidth) * CGFloat(line) / CGFloat(gridLines) + lineWidth / 2
                    var grid = Path()
                    grid.move(to: CGPoint(x: 0, y: y))
                    grid.addLine(to: CGPoint(x: size.width, y: y))
                    context.stroke(
                        grid, with: .color(Tokens.Palette.chartGrid),
                        style: StrokeStyle(lineWidth: 1, dash: line == gridLines ? [] : [3, 4]))
                }
            }

            for item in series {
                let points = item.samples.map { sample in
                    CGPoint(
                        x: size.width - CGFloat((end - sample.time) / window) * size.width,
                        y: size.height - CGFloat(min(max(sample.value / max(scale, .leastNonzeroMagnitude), 0), 1))
                            * (size.height - lineWidth) - lineWidth / 2
                    )
                }
                guard points.count > 1 else { continue }
                let line = Sparkline.smoothPath(points)

                if item.fills {
                    var area = line
                    area.addLine(to: CGPoint(x: points.last!.x, y: size.height))
                    area.addLine(to: CGPoint(x: points.first!.x, y: size.height))
                    area.closeSubpath()
                    context.fill(
                        area,
                        with: .linearGradient(
                            Gradient(colors: [
                                item.color.opacity(Tokens.Chart.fillOpacityTop),
                                item.color.opacity(Tokens.Chart.fillOpacityBottom),
                            ]),
                            startPoint: .zero, endPoint: CGPoint(x: 0, y: size.height)))
                }

                let stroke = StrokeStyle(lineWidth: lineWidth, lineCap: .round, lineJoin: .round)
                if glow {
                    // Halo from wide translucent strokes; a blur filter would rasterize on the CPU.
                    for (width, opacity) in [(Tokens.Chart.glowRadius * 1.6, 0.08), (Tokens.Chart.glowRadius * 0.8, 0.14)] {
                        context.stroke(
                            line, with: .color(item.color.opacity(opacity)),
                            style: StrokeStyle(lineWidth: lineWidth + width, lineCap: .round, lineJoin: .round))
                    }
                }
                context.stroke(line, with: .color(item.color), style: stroke)

                if let last = points.last {
                    let dot = Path(
                        ellipseIn: CGRect(
                            x: last.x - lineWidth * 1.4, y: last.y - lineWidth * 1.4, width: lineWidth * 2.8,
                            height: lineWidth * 2.8))
                    context.fill(dot, with: .color(item.color))
                }
            }
        }
        .accessibilityHidden(true)
    }

    /// Rounds up to 1, 2, 2.5 or 5 × 10ⁿ so axes don't jitter with every sample.
    public static func niceCeiling(_ value: Double) -> Double {
        guard value > 0, value.isFinite else { return 1 }
        let exponent = pow(10, floor(log10(value)))
        for step in [1, 2, 2.5, 5, 10] where value <= step * exponent { return step * exponent }
        return 10 * exponent
    }

    static func smoothPath(_ points: [CGPoint]) -> Path {
        var path = Path()
        path.move(to: points[0])
        for index in 1..<points.count {
            let previous = points[index - 1], current = points[index]
            let mid = CGPoint(x: (previous.x + current.x) / 2, y: (previous.y + current.y) / 2)
            path.addQuadCurve(to: mid, control: previous)
            if index == points.count - 1 { path.addLine(to: current) }
        }
        return path
    }
}
