import AppKit
import SwiftUI

/// Vertical scrolling for SwiftUI content through an AppKit scroll view.
///
/// SwiftUI's ScrollView in the hidden-title-bar window offsets clicks from where rows are drawn
/// (and insets the content by the title bar). Here AppKit does the hit testing, and the content
/// starts at the top edge.
struct AppKitScrollView<Content: View>: NSViewRepresentable {
    @ViewBuilder var content: Content

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSScrollView()
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.horizontalScrollElasticity = .none
        scroll.automaticallyAdjustsContentInsets = false
        scroll.contentInsets = NSEdgeInsetsZero

        let hosting = NSHostingView(rootView: content)
        hosting.sizingOptions = [.intrinsicContentSize]
        hosting.translatesAutoresizingMaskIntoConstraints = false
        scroll.documentView = hosting
        let clip = scroll.contentView
        NSLayoutConstraint.activate([
            hosting.leadingAnchor.constraint(equalTo: clip.leadingAnchor),
            hosting.trailingAnchor.constraint(equalTo: clip.trailingAnchor),
            hosting.topAnchor.constraint(equalTo: clip.topAnchor),
        ])
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        (scroll.documentView as? NSHostingView<Content>)?.rootView = content
    }
}
