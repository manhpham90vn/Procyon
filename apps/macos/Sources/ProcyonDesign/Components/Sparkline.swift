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

/// Lightweight time-series chart drawn with shapes (no per-point views).
/// Not a Canvas: a single Canvas makes SwiftUI render the whole window through Metal, which keeps
/// ~50 MB of window-sized buffers around; shapes stay in Core Animation layers.
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
        let end = series.compactMap { $0.samples.last?.time }.max() ?? 0
        let scale = maxValue ?? Sparkline.niceCeiling(series.flatMap { $0.samples.map(\.value) }.max() ?? 0)
        ZStack {
            if gridLines > 0 {
                GridLines(count: gridLines, lineWidth: lineWidth, dashed: true)
                    .stroke(Tokens.Palette.chartGrid, style: StrokeStyle(lineWidth: 1, dash: [3, 4]))
                GridLines(count: gridLines, lineWidth: lineWidth, dashed: false)
                    .stroke(Tokens.Palette.chartGrid, lineWidth: 1)
            }

            ForEach(series) { item in
                SeriesPlot(
                    item: item, plot: Plot(samples: item.samples, end: end, window: window, scale: scale, lineWidth: lineWidth),
                    lineWidth: lineWidth, glow: glow)
            }
        }
        .accessibilityHidden(true)
    }

    /// Rounds up to 1, 2, 2.5 or 5 × 10ⁿ so axes don't jitter with every sample.
    nonisolated public static func niceCeiling(_ value: Double) -> Double {
        guard value > 0, value.isFinite else { return 1 }
        let exponent = pow(10, floor(log10(value)))
        for step in [1, 2, 2.5, 5, 10] where value <= step * exponent { return step * exponent }
        return 10 * exponent
    }

    nonisolated static func smoothPath(_ points: [CGPoint]) -> Path {
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

/// Horizontal grid: the dashed inner lines, or the solid baseline.
private struct GridLines: Shape {
    var count: Int
    var lineWidth: CGFloat
    var dashed: Bool

    func path(in rect: CGRect) -> Path {
        var path = Path()
        for line in dashed ? Array(0..<count) : [count] {
            let y = (rect.height - lineWidth) * CGFloat(line) / CGFloat(count) + lineWidth / 2
            path.move(to: CGPoint(x: 0, y: y))
            path.addLine(to: CGPoint(x: rect.width, y: y))
        }
        return path
    }
}

private struct SeriesPlot: View {
    let item: ChartSeries
    let plot: Plot
    let lineWidth: CGFloat
    let glow: Bool

    var body: some View {
        if item.fills {
            plot.part(.area).fill(
                LinearGradient(
                    colors: [
                        item.color.opacity(Tokens.Chart.fillOpacityTop), item.color.opacity(Tokens.Chart.fillOpacityBottom),
                    ], startPoint: .top, endPoint: .bottom))
        }
        if glow {
            // Halo from wide translucent strokes; a blur filter would rasterize on the CPU.
            halo(width: Tokens.Chart.glowRadius * 1.6, opacity: 0.08)
            halo(width: Tokens.Chart.glowRadius * 0.8, opacity: 0.14)
        }
        plot.stroke(item.color, style: StrokeStyle(lineWidth: lineWidth, lineCap: .round, lineJoin: .round))
        plot.part(.dot).fill(item.color)
    }

    private func halo(width: CGFloat, opacity: Double) -> some View {
        plot.stroke(
            item.color.opacity(opacity), style: StrokeStyle(lineWidth: lineWidth + width, lineCap: .round, lineJoin: .round))
    }
}

/// One part of a series: its line (the default), the area under it, or the dot on the newest sample.
private struct Plot: Shape {
    enum Part { case area, line, dot }

    var samples: [ChartSample]
    var end: TimeInterval
    var window: TimeInterval
    var scale: Double
    var lineWidth: CGFloat
    var part: Part = .line

    func part(_ part: Part) -> Plot {
        var copy = self
        copy.part = part
        return copy
    }

    func path(in rect: CGRect) -> Path {
        let points = samples.map { sample in
            CGPoint(
                x: rect.width - CGFloat((end - sample.time) / window) * rect.width,
                y: rect.height - CGFloat(min(max(sample.value / max(scale, .leastNonzeroMagnitude), 0), 1))
                    * (rect.height - lineWidth) - lineWidth / 2
            )
        }
        guard points.count > 1, let first = points.first, let last = points.last else { return Path() }
        switch part {
        case .line:
            return Sparkline.smoothPath(points)
        case .area:
            var area = Sparkline.smoothPath(points)
            area.addLine(to: CGPoint(x: last.x, y: rect.height))
            area.addLine(to: CGPoint(x: first.x, y: rect.height))
            area.closeSubpath()
            return area
        case .dot:
            let radius = lineWidth * 1.4
            return Path(ellipseIn: CGRect(x: last.x - radius, y: last.y - radius, width: radius * 2, height: radius * 2))
        }
    }
}
