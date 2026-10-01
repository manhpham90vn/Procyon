import AppKit
import SwiftUI

public extension Color {
    /// `0xRRGGBBAA` literal.
    init(hex: UInt32) {
        self.init(
            .sRGB,
            red: Double((hex >> 24) & 0xFF) / 255,
            green: Double((hex >> 16) & 0xFF) / 255,
            blue: Double((hex >> 8) & 0xFF) / 255,
            opacity: Double(hex & 0xFF) / 255
        )
    }

    /// A color that resolves per appearance, like an asset-catalog color.
    init(light: UInt32, dark: UInt32) {
        self.init(
            nsColor: NSColor(name: nil) { appearance in
                let isDark =
                    appearance.bestMatch(from: [.darkAqua, .aqua, .vibrantDark, .vibrantLight])
                    .map { $0 == .darkAqua || $0 == .vibrantDark } ?? false
                return NSColor(Color(hex: isDark ? dark : light))
            })
    }
}

/// Identity of a resource family (CPU, memory, …): gradient + symbol.
public struct MetricStyle: Sendable, Hashable {
    public let start: Color
    public let end: Color
    public let symbol: String

    public init(start: Color, end: Color, symbol: String) {
        self.start = start
        self.end = end
        self.symbol = symbol
    }

    public var gradient: LinearGradient {
        LinearGradient(colors: [start, end], startPoint: .topLeading, endPoint: .bottomTrailing)
    }

    public var horizontalGradient: LinearGradient {
        LinearGradient(colors: [start, end], startPoint: .leading, endPoint: .trailing)
    }

    /// Color used for a secondary series (e.g. disk write vs. read).
    public var secondary: Color { end }
}

public struct ShadowStyle: Sendable {
    public struct Layer: Sendable {
        public let color: Color
        public let opacity: Double
        public let radius: CGFloat
        public let y: CGFloat
    }

    public let light: Layer
    public let dark: Layer

    public func layer(for scheme: ColorScheme) -> Layer { scheme == .dark ? dark : light }
}

public enum Metric: String, CaseIterable, Sendable, Identifiable {
    case cpu, memory, disk, network, gpu

    public var id: String { rawValue }

    public var style: MetricStyle {
        switch self {
        case .cpu: Tokens.MetricPalette.cpu
        case .memory: Tokens.MetricPalette.memory
        case .disk: Tokens.MetricPalette.disk
        case .network: Tokens.MetricPalette.network
        case .gpu: Tokens.MetricPalette.gpu
        }
    }

    public var title: String {
        switch self {
        case .cpu: "CPU"
        case .memory: "Memory"
        case .disk: "Disk"
        case .network: "Network"
        case .gpu: "GPU"
        }
    }
}
