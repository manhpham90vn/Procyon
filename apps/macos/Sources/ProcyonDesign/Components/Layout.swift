import SwiftUI

/// Large page title with optional metric icon, subtitle and trailing accessory.
public struct PageHeader<Trailing: View>: View {
    var title: String
    var subtitle: String?
    var style: MetricStyle?
    var trailing: Trailing

    public init(_ title: String, subtitle: String? = nil, style: MetricStyle? = nil, @ViewBuilder trailing: () -> Trailing) {
        self.title = title
        self.subtitle = subtitle
        self.style = style
        self.trailing = trailing()
    }

    public var body: some View {
        HStack(alignment: .center, spacing: Tokens.Space.md) {
            if let style { MetricIcon(style, size: 40) }
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(Tokens.Typography.title)
                    .foregroundStyle(Tokens.Palette.textPrimary)
                if let subtitle {
                    Text(subtitle)
                        .font(Tokens.Typography.body)
                        .foregroundStyle(Tokens.Palette.textSecondary)
                        .lineLimit(1)
                }
            }
            Spacer(minLength: Tokens.Space.lg)
            trailing
        }
    }
}

public extension PageHeader where Trailing == EmptyView {
    init(_ title: String, subtitle: String? = nil, style: MetricStyle? = nil) {
        self.init(title, subtitle: subtitle, style: style) { EmptyView() }
    }
}

/// Titled card section.
public struct Panel<Content: View, Accessory: View>: View {
    var title: String
    var symbol: String?
    var tint: Color?
    var accessory: Accessory
    var content: Content

    public init(
        _ title: String, symbol: String? = nil, tint: Color? = nil,
        @ViewBuilder accessory: () -> Accessory, @ViewBuilder content: () -> Content
    ) {
        self.title = title
        self.symbol = symbol
        self.tint = tint
        self.accessory = accessory()
        self.content = content()
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: Tokens.Space.md) {
            HStack(spacing: Tokens.Space.xs + 2) {
                if let symbol {
                    Image(systemName: symbol).font(.system(size: 11, weight: .semibold)).foregroundStyle(
                        Tokens.Palette.textTertiary)
                }
                Text(title.uppercased())
                    .font(Tokens.Typography.caption)
                    .tracking(0.8)
                    .foregroundStyle(Tokens.Palette.textTertiary)
                Spacer()
                accessory
            }
            content
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .cardSurface(tint: tint)
    }
}

public extension Panel where Accessory == EmptyView {
    init(_ title: String, symbol: String? = nil, tint: Color? = nil, @ViewBuilder content: () -> Content) {
        self.init(title, symbol: symbol, tint: tint, accessory: { EmptyView() }, content: content)
    }
}

/// Key/value statistic used in grids.
public struct StatItem: Identifiable, Sendable {
    public let id: String
    public let title: String
    public let value: String
    public let detail: String?
    public let tint: Color?

    public init(_ title: String, value: String, detail: String? = nil, tint: Color? = nil) {
        self.id = title
        self.title = title
        self.value = value
        self.detail = detail
        self.tint = tint
    }
}

public struct StatGrid: View {
    var items: [StatItem]
    var minimumWidth: CGFloat

    public init(_ items: [StatItem], minimumWidth: CGFloat = 140) {
        self.items = items
        self.minimumWidth = minimumWidth
    }

    public var body: some View {
        LazyVGrid(
            columns: [GridItem(.adaptive(minimum: minimumWidth), spacing: Tokens.Space.md, alignment: .topLeading)],
            alignment: .leading, spacing: Tokens.Space.lg
        ) {
            ForEach(items) { item in
                VStack(alignment: .leading, spacing: 3) {
                    Text(item.title)
                        .font(Tokens.Typography.label)
                        .foregroundStyle(Tokens.Palette.textSecondary)
                    HStack(spacing: Tokens.Space.xs + 2) {
                        if let tint = item.tint { Circle().fill(tint).frame(width: 7, height: 7) }
                        Text(item.value)
                            .font(.system(size: 17, weight: .semibold, design: .rounded).monospacedDigit())
                            .foregroundStyle(Tokens.Palette.textPrimary)
                            .lineLimit(1)
                            .minimumScaleFactor(0.7)
                    }
                    if let detail = item.detail {
                        Text(detail).font(Tokens.Typography.caption).foregroundStyle(Tokens.Palette.textTertiary).lineLimit(1)
                    }
                }
                .textSelection(.enabled)
            }
        }
    }
}
