import SwiftUI

/// Search input with a clear button and a shortcut hint.
public struct SearchField: View {
    @Binding var text: String
    var prompt: String
    var shortcutHint: String?
    var focus: FocusState<Bool>.Binding

    public init(text: Binding<String>, prompt: String = "Search", shortcutHint: String? = "⌘F", focus: FocusState<Bool>.Binding) {
        self._text = text
        self.prompt = prompt
        self.shortcutHint = shortcutHint
        self.focus = focus
    }

    public var body: some View {
        HStack(spacing: Tokens.Space.xs + 2) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(focus.wrappedValue ? Tokens.Palette.accent : Tokens.Palette.textTertiary)
            TextField(prompt, text: $text)
                .textFieldStyle(.plain)
                .font(Tokens.Typography.body)
                .focused(focus)
                .onExitCommand { clear() }
            if !text.isEmpty {
                Button {
                    text = ""
                } label: {
                    Image(systemName: "xmark.circle.fill").foregroundStyle(Tokens.Palette.textTertiary)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Clear search")
            } else if let shortcutHint, !focus.wrappedValue {
                KeyCap(shortcutHint)
            }
        }
        .padding(.horizontal, Tokens.Space.sm + 2)
        .frame(height: 28)
        .background(Tokens.Palette.surfaceSunken, in: RoundedRectangle(cornerRadius: Tokens.Radius.sm + 1, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: Tokens.Radius.sm + 1, style: .continuous)
                .strokeBorder(focus.wrappedValue ? Tokens.Palette.accent.opacity(0.6) : Tokens.Palette.border, lineWidth: 1)
        )
        .animation(.easeOut(duration: Tokens.Motion.fast), value: focus.wrappedValue)
    }

    private func clear() {
        text = ""
        focus.wrappedValue = false
    }
}
