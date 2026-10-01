import ProcyonDesign
import ProcyonKit
import SwiftUI

struct RootView: View {
    @Binding var page: Page
    @Environment(SystemStore.self) private var store

    var body: some View {
        NavigationSplitView {
            Sidebar(page: $page)
                .navigationSplitViewColumnWidth(min: 220, ideal: 240, max: 300)
        } detail: {
            Group {
                switch page {
                case .overview: OverviewView(page: $page)
                case .processes: ProcessesView()
                case .cpu: CPUView()
                case .memory: MemoryView()
                case .disk: DiskView()
                case .network: NetworkView()
                case .system: SystemView()
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .pageBackground()
            .transition(.opacity)
            .animation(.easeOut(duration: Tokens.Motion.fast), value: page)
        }
    }
}

/// Scrollable page scaffold shared by the dashboard-style screens.
struct ScreenScroll<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Tokens.Space.xl) {
                content
            }
            .padding(.horizontal, Tokens.Space.xxl)
            .padding(.top, Tokens.Space.lg)
            .padding(.bottom, Tokens.Space.xxl)
            .frame(maxWidth: 1280, alignment: .leading)
            .frame(maxWidth: .infinity)
        }
        .scrollContentBackground(.hidden)
    }
}
