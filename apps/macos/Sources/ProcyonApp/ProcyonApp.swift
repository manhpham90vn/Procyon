import AppKit
import ProcyonKit
import SwiftUI

@main
struct ProcyonApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var store = SystemStore()
    // `-initialPage processes` on the command line opens a specific screen (handy for profiling).
    @State private var page: Page = UserDefaults.standard.string(forKey: "initialPage").flatMap(Page.init(rawValue:)) ?? .overview
    @AppStorage(Appearance.storageKey) private var appearance: Appearance = .system

    var body: some Scene {
        Window("Procyon", id: "main") {
            RootView(page: $page)
                .environment(store)
                .frame(minWidth: 940, minHeight: 600)
                .onAppear {
                    store.start()
                    appearance.apply()
                }
                .onChange(of: appearance) { _, value in value.apply() }
        }
        .defaultSize(width: 1220, height: 800)
        .windowStyle(.hiddenTitleBar)
        .commands { AppCommands(store: store, page: $page) }

        Settings {
            SettingsView()
                .environment(store)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Launched as a bare executable (swift run): make it a regular foreground app.
        NSApp.setActivationPolicy(.regular)
        NSApp.activate()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

enum Page: String, CaseIterable, Identifiable, Hashable {
    case overview, processes, cpu, memory, disk, network, system

    var id: String { rawValue }

    var title: String {
        switch self {
        case .overview: "Overview"
        case .processes: "Processes"
        case .cpu: "CPU"
        case .memory: "Memory"
        case .disk: "Disk"
        case .network: "Network"
        case .system: "System"
        }
    }

    var symbol: String {
        switch self {
        case .overview: "square.grid.2x2.fill"
        case .processes: "list.bullet.rectangle.fill"
        case .system: "info.circle.fill"
        default: ""
        }
    }

    var shortcut: KeyEquivalent { KeyEquivalent(Character("\(Page.allCases.firstIndex(of: self)! + 1)")) }
}

enum Appearance: String, CaseIterable, Identifiable {
    case system, light, dark

    static let storageKey = "appearance"
    var id: String { rawValue }

    var title: String {
        switch self {
        case .system: "System"
        case .light: "Light"
        case .dark: "Dark"
        }
    }

    @MainActor func apply() {
        switch self {
        case .system: NSApp.appearance = nil
        case .light: NSApp.appearance = NSAppearance(named: .aqua)
        case .dark: NSApp.appearance = NSAppearance(named: .darkAqua)
        }
    }
}
