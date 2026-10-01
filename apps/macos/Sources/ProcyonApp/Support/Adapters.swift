import AppKit
import ProcyonDesign
import ProcyonKit
import SwiftUI

extension MetricHistory {
    var samples: [ChartSample] { points.map { ChartSample(time: $0.time, value: $0.value) } }
}

extension MemoryPressure {
    var title: String {
        switch self {
        case .normal: "Normal"
        case .warning: "Elevated"
        case .critical: "Critical"
        case .unknown: "Unknown"
        }
    }

    var color: Color {
        switch self {
        case .normal: Tokens.Palette.success
        case .warning: Tokens.Palette.warning
        case .critical: Tokens.Palette.danger
        case .unknown: Tokens.Palette.textTertiary
        }
    }
}

extension SystemInfo {
    var coreSummary: String {
        performanceCores > 0
            ? "\(performanceCores)P + \(efficiencyCores)E cores"
            : "\(physicalCores) cores · \(logicalCores) threads"
    }

    var deviceSymbol: String { isLaptop ? "laptopcomputer" : "desktopcomputer" }
}

/// App icons resolved through NSWorkspace, cached by path.
@MainActor
enum IconCache {
    private static let cache = NSCache<NSString, NSImage>()

    static func icon(for path: String) -> NSImage {
        if let cached = cache.object(forKey: path as NSString) { return cached }
        let image = NSWorkspace.shared.icon(forFile: path)
        cache.setObject(image, forKey: path as NSString)
        return image
    }
}

struct ProcessIcon: View {
    let row: ProcessRow
    var size: CGFloat = 16

    var body: some View {
        if let bundle = row.bundlePath {
            Image(nsImage: IconCache.icon(for: bundle))
                .resizable()
                .interpolation(.high)
                .frame(width: size, height: size)
        } else {
            RoundedRectangle(cornerRadius: size * 0.25, style: .continuous)
                .fill(Tokens.Palette.surfaceSunken)
                .overlay {
                    Image(systemName: row.isSystem ? "gearshape.2.fill" : "terminal.fill")
                        .font(.system(size: size * 0.52, weight: .semibold))
                        .foregroundStyle(Tokens.Palette.textTertiary)
                }
                .overlay {
                    RoundedRectangle(cornerRadius: size * 0.25, style: .continuous)
                        .strokeBorder(Tokens.Palette.border, lineWidth: 0.5)
                }
                .frame(width: size, height: size)
        }
    }
}
