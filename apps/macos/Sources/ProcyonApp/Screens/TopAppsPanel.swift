import ProcyonDesign
import ProcyonKit
import SwiftUI

extension EnvironmentValues {
    /// Switches to Processes with the row revealed and selected; installed by `RootView`.
    @Entry var showInProcesses: (ProcessRow) -> Void = { _ in }
}

/// The busiest apps for one metric. Clicking an app reveals it in Processes.
struct TopAppsPanel<Value: View>: View {
    let title: String
    let symbol: String
    var tint: Color?
    let rows: [ProcessRow]
    let isLoading: Bool
    let style: MetricStyle
    var emptyText = "No app is busy right now."
    /// Bar length, 0...1.
    let fraction: (ProcessRow) -> Double
    @ViewBuilder let value: (ProcessRow) -> Value

    @Environment(\.showInProcesses) private var showInProcesses

    var body: some View {
        Panel(title, symbol: symbol, tint: tint) {
            VStack(spacing: Tokens.Space.xs) {
                if isLoading {
                    ProgressView().frame(maxWidth: .infinity, minHeight: 80)
                } else if rows.isEmpty {
                    Text(emptyText)
                        .font(Tokens.Typography.body)
                        .foregroundStyle(Tokens.Palette.textTertiary)
                        .frame(maxWidth: .infinity, minHeight: 80)
                }
                ForEach(rows) { row in
                    Button {
                        showInProcesses(row)
                    } label: {
                        label(row)
                    }
                    .buttonStyle(TopAppRowStyle())
                    .help("Show “\(row.name)” in Processes")
                }
            }
        }
    }

    private func label(_ row: ProcessRow) -> some View {
        HStack(spacing: Tokens.Space.sm + 2) {
            ProcessIcon(row: row, size: 22)
            VStack(alignment: .leading, spacing: Tokens.Space.xs) {
                HStack(spacing: Tokens.Space.md) {
                    Text(row.name)
                        .font(Tokens.Typography.headline)
                        .foregroundStyle(Tokens.Palette.textPrimary)
                        .lineLimit(1)
                    if row.processCount > 1 {
                        Text("\(row.processCount)")
                            .font(Tokens.Typography.caption)
                            .foregroundStyle(Tokens.Palette.textTertiary)
                    }
                    Spacer()
                    value(row)
                }
                UsageBar(value: min(max(fraction(row), 0), 1), style: style, height: 4)
            }
        }
        .contentShape(Rectangle())
    }
}

extension TopAppsPanel where Value == TopAppValue {
    /// One figure per app, such as CPU % or memory.
    init(
        title: String, symbol: String, tint: Color? = nil, rows: [ProcessRow], isLoading: Bool, style: MetricStyle,
        text: @escaping (ProcessRow) -> String, fraction: @escaping (ProcessRow) -> Double
    ) {
        self.init(
            title: title, symbol: symbol, tint: tint, rows: rows, isLoading: isLoading, style: style, fraction: fraction
        ) { TopAppValue(text: text($0)) }
    }
}

struct TopAppValue: View {
    let text: String

    var body: some View {
        Text(text)
            .font(Tokens.Typography.headline.monospacedDigit())
            .foregroundStyle(Tokens.Palette.textPrimary)
    }
}

/// Two directional rates (read/write, down/up) for one app.
struct TopAppRates: View {
    let primary: Double?
    let secondary: Double?
    let primarySymbol: String
    let secondarySymbol: String
    let style: MetricStyle

    var body: some View {
        rate(primarySymbol, primary, style.start)
        rate(secondarySymbol, secondary, style.end)
    }

    private func rate(_ symbol: String, _ value: Double?, _ tint: Color) -> some View {
        HStack(spacing: 3) {
            Image(systemName: symbol).font(.system(size: 9, weight: .bold)).foregroundStyle(tint)
            Text(Format.rate(value))
                .font(Tokens.Typography.headline.monospacedDigit())
                .foregroundStyle(Tokens.Palette.textPrimary)
        }
        .frame(minWidth: 84, alignment: .trailing)
    }
}

/// Plain row with a soft highlight on hover and press, so it reads as clickable.
private struct TopAppRowStyle: ButtonStyle {
    @State private var hovering = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .padding(.horizontal, Tokens.Space.sm)
            .padding(.vertical, Tokens.Space.xs + 1)
            .background(
                Tokens.Palette.textPrimary.opacity(configuration.isPressed ? 0.1 : hovering ? 0.05 : 0),
                in: RoundedRectangle(cornerRadius: Tokens.Radius.sm, style: .continuous)
            )
            .padding(.horizontal, -Tokens.Space.sm)
            .onHover { hovering = $0 }
            .animation(.easeOut(duration: Tokens.Motion.fast), value: hovering)
    }
}
