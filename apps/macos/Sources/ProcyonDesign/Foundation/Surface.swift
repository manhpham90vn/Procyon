import SwiftUI

/// Base surface treatment for cards and panels.
public struct CardSurface: ViewModifier {
    @Environment(\.colorScheme) private var scheme
    var radius: CGFloat
    var padding: CGFloat?
    var tint: Color?

    public func body(content: Content) -> some View {
        let shadow = Tokens.Shadow.card.layer(for: scheme)
        let shape = RoundedRectangle(cornerRadius: radius, style: .continuous)
        content
            .padding(padding ?? 0)
            .background {
                shape.fill(Tokens.Palette.surface)
                if let tint {
                    shape.fill(
                        LinearGradient(
                            colors: [tint.opacity(scheme == .dark ? 0.14 : 0.08), .clear],
                            startPoint: .topLeading, endPoint: .bottomTrailing
                        )
                    )
                }
            }
            .clipShape(shape)
            .overlay { shape.strokeBorder(Tokens.Palette.border, lineWidth: 1) }
            // The shadow sits on a separate static shape behind the card: shadowing the card itself
            // would re-rasterize its blur on the CPU every time live content changes.
            .background {
                shape.fill(Tokens.Palette.surface)
                    .shadow(color: shadow.color.opacity(shadow.opacity), radius: shadow.radius, y: shadow.y)
            }
    }
}

public extension View {
    /// Card background, hairline border and soft shadow from the design tokens.
    func cardSurface(radius: CGFloat = Tokens.Radius.lg, padding: CGFloat? = Tokens.Space.lg, tint: Color? = nil) -> some View {
        modifier(CardSurface(radius: radius, padding: padding, tint: tint))
    }

    /// Floating control chrome: Liquid Glass where available, material otherwise.
    @ViewBuilder
    func glassChrome(radius: CGFloat = Tokens.Radius.md) -> some View {
        if #available(macOS 26.0, *) {
            glassEffect(.regular, in: RoundedRectangle(cornerRadius: radius, style: .continuous))
        } else {
            background(.regularMaterial, in: RoundedRectangle(cornerRadius: radius, style: .continuous))
        }
    }

    /// Page background used behind scrolling content.
    func pageBackground() -> some View {
        background(Tokens.Palette.background.ignoresSafeArea())
    }
}
