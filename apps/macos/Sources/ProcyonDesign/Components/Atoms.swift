import AppKit
import SwiftUI

/// Gradient tile with a white SF Symbol: the visual signature of a metric.
public struct MetricIcon: View {
    var style: MetricStyle
    var size: CGFloat

    public init(_ style: MetricStyle, size: CGFloat = 28) {
        self.style = style
        self.size = size
    }

    public var body: some View {
        RoundedRectangle(cornerRadius: size * 0.28, style: .continuous)
            .fill(style.gradient)
            .overlay {
                RoundedRectangle(cornerRadius: size * 0.28, style: .continuous)
                    .strokeBorder(.white.opacity(0.25), lineWidth: 0.5)
            }
            .overlay {
                Image(systemName: style.symbol)
                    .font(.system(size: size * 0.5, weight: .semibold))
                    .foregroundStyle(.white)
                    .shadow(color: .black.opacity(0.2), radius: 1, y: 0.5)
            }
            .frame(width: size, height: size)
            .shadow(color: style.start.opacity(0.35), radius: size * 0.18, y: size * 0.06)
            .accessibilityHidden(true)
    }
}

public struct Badge: View {
    public enum Tone: Sendable { case neutral, accent, success, warning, danger }

    var text: String
    var tone: Tone
    var symbol: String?

    public init(_ text: String, tone: Tone = .neutral, symbol: String? = nil) {
        self.text = text
        self.tone = tone
        self.symbol = symbol
    }

    private var color: Color {
        switch tone {
        case .neutral: Tokens.Palette.textSecondary
        case .accent: Tokens.Palette.accent
        case .success: Tokens.Palette.success
        case .warning: Tokens.Palette.warning
        case .danger: Tokens.Palette.danger
        }
    }

    public var body: some View {
        HStack(spacing: 3) {
            if let symbol { Image(systemName: symbol).font(.system(size: 8, weight: .bold)) }
            Text(text)
        }
        .font(Tokens.Typography.caption)
        .foregroundStyle(color)
        .padding(.horizontal, 6)
        .padding(.vertical, 2)
        .background(color.opacity(0.12), in: Capsule())
        .overlay(Capsule().strokeBorder(color.opacity(0.2), lineWidth: 0.5))
    }
}

/// Pulsing "live" dot. The pulse is a Core Animation layer animation, which the render
/// server runs on its own: a SwiftUI animation here re-ran the window's display cycle at
/// 120 Hz and cost ~9% CPU on its own.
public struct LiveIndicator: View {
    var isLive: Bool

    public init(isLive: Bool) { self.isLive = isLive }

    public var body: some View {
        let color = isLive ? Tokens.Palette.success : Tokens.Palette.textTertiary
        ZStack {
            PulseLayer(color: color, isAnimating: isLive)
            Circle().fill(color).frame(width: 7, height: 7)
        }
        .frame(width: 14, height: 14)
        .accessibilityLabel(isLive ? "Live" : "Paused")
    }
}

private struct PulseLayer: NSViewRepresentable {
    var color: Color
    var isAnimating: Bool

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        view.wantsLayer = true
        let ring = CAShapeLayer()
        ring.path = CGPath(ellipseIn: CGRect(x: 0, y: 0, width: 14, height: 14), transform: nil)
        ring.bounds = CGRect(x: 0, y: 0, width: 14, height: 14)
        ring.position = CGPoint(x: 7, y: 7)
        ring.opacity = 0
        view.layer?.addSublayer(ring)
        return view
    }

    func updateNSView(_ view: NSView, context: Context) {
        guard let ring = view.layer?.sublayers?.first as? CAShapeLayer else { return }
        view.effectiveAppearance.performAsCurrentDrawingAppearance {
            ring.fillColor = NSColor(color).withAlphaComponent(0.4).cgColor
        }
        if isAnimating, ring.animation(forKey: "pulse") == nil {
            let scale = CABasicAnimation(keyPath: "transform.scale")
            scale.fromValue = 0.4
            scale.toValue = 1
            let fade = CABasicAnimation(keyPath: "opacity")
            fade.fromValue = 1
            fade.toValue = 0
            let group = CAAnimationGroup()
            group.animations = [scale, fade]
            group.duration = 1.6
            group.timingFunction = CAMediaTimingFunction(name: .easeOut)
            group.repeatCount = .infinity
            ring.add(group, forKey: "pulse")
        } else if !isAnimating {
            ring.removeAnimation(forKey: "pulse")
        }
    }
}

public struct KeyCap: View {
    var text: String
    public init(_ text: String) { self.text = text }

    public var body: some View {
        Text(text)
            .font(.system(size: 10, weight: .medium, design: .rounded))
            .foregroundStyle(Tokens.Palette.textTertiary)
            .padding(.horizontal, 5)
            .padding(.vertical, 1)
            .background(RoundedRectangle(cornerRadius: 4).strokeBorder(Tokens.Palette.borderStrong, lineWidth: 1))
    }
}

/// Value with a smaller unit, e.g. "12.4" + "GB".
public struct ValueText: View {
    var value: String
    var unit: String
    var font: Font
    var unitFont: Font

    public init(
        _ value: String, unit: String = "", font: Font = Tokens.Typography.metric, unitFont: Font = Tokens.Typography.headline
    ) {
        self.value = value
        self.unit = unit
        self.font = font
        self.unitFont = unitFont
    }

    public var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 3) {
            Text(value)
                .font(font.monospacedDigit())
                .foregroundStyle(Tokens.Palette.textPrimary)
            if !unit.isEmpty {
                Text(unit).font(unitFont).foregroundStyle(Tokens.Palette.textSecondary)
            }
        }
    }
}

public struct InfoBanner: View {
    var symbol: String
    var text: String
    var tone: Badge.Tone

    public init(_ text: String, symbol: String = "info.circle", tone: Badge.Tone = .neutral) {
        self.text = text
        self.symbol = symbol
        self.tone = tone
    }

    public var body: some View {
        let color: Color =
            tone == .warning ? Tokens.Palette.warning : tone == .danger ? Tokens.Palette.danger : Tokens.Palette.accent
        HStack(alignment: .firstTextBaseline, spacing: Tokens.Space.sm) {
            Image(systemName: symbol).foregroundStyle(color)
            Text(text).font(Tokens.Typography.body).foregroundStyle(Tokens.Palette.textSecondary)
            Spacer(minLength: 0)
        }
        .padding(Tokens.Space.md)
        .background(color.opacity(0.08), in: RoundedRectangle(cornerRadius: Tokens.Radius.md, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: Tokens.Radius.md, style: .continuous).strokeBorder(color.opacity(0.18)))
    }
}

public struct EmptyState: View {
    var symbol: String
    var title: String
    var message: String

    public init(symbol: String, title: String, message: String) {
        self.symbol = symbol
        self.title = title
        self.message = message
    }

    public var body: some View {
        VStack(spacing: Tokens.Space.sm) {
            Image(systemName: symbol)
                .font(.system(size: 34, weight: .light))
                .foregroundStyle(Tokens.Palette.textTertiary)
            Text(title).font(Tokens.Typography.headline).foregroundStyle(Tokens.Palette.textPrimary)
            Text(message).font(Tokens.Typography.body).foregroundStyle(Tokens.Palette.textSecondary)
                .multilineTextAlignment(.center)
        }
        .padding(Tokens.Space.xxl)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// Call-to-action strip: icon, title + message, one primary button (with a busy state).
public struct ActionBanner: View {
    var symbol: String
    var title: String
    var message: String
    var actionTitle: String
    var tone: Badge.Tone
    var isBusy: Bool
    var action: () -> Void

    public init(
        symbol: String, title: String, message: String, actionTitle: String, tone: Badge.Tone = .accent,
        isBusy: Bool = false, action: @escaping () -> Void
    ) {
        self.symbol = symbol
        self.title = title
        self.message = message
        self.actionTitle = actionTitle
        self.tone = tone
        self.isBusy = isBusy
        self.action = action
    }

    public var body: some View {
        let color: Color =
            tone == .warning ? Tokens.Palette.warning : tone == .danger ? Tokens.Palette.danger : Tokens.Palette.accent
        HStack(spacing: Tokens.Space.md) {
            Image(systemName: symbol)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(color)
                .frame(width: 32, height: 32)
                .background(color.opacity(0.14), in: RoundedRectangle(cornerRadius: Tokens.Radius.sm + 2, style: .continuous))
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(Tokens.Typography.headline).foregroundStyle(Tokens.Palette.textPrimary)
                Text(message).font(Tokens.Typography.label).foregroundStyle(Tokens.Palette.textSecondary).lineLimit(2)
            }
            Spacer(minLength: Tokens.Space.md)
            Button(action: action) {
                HStack(spacing: Tokens.Space.xs + 2) {
                    if isBusy { ProgressView().controlSize(.small) }
                    Text(actionTitle)
                }
                .font(Tokens.Typography.headline)
                .foregroundStyle(.white)
                .padding(.horizontal, Tokens.Space.md)
                .frame(height: 28)
                .background(color, in: RoundedRectangle(cornerRadius: Tokens.Radius.sm + 1, style: .continuous))
            }
            .buttonStyle(.plain)
            .disabled(isBusy)
        }
        .padding(Tokens.Space.md)
        .background(color.opacity(0.07), in: RoundedRectangle(cornerRadius: Tokens.Radius.lg, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: Tokens.Radius.lg, style: .continuous).strokeBorder(color.opacity(0.22)))
    }
}
