import AppKit
import ProcyonKit
import SwiftUI

@main
struct ProcyonApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var store: SystemStore
    @State private var actions: ProcessActionCenter
    @State private var processesUI = ProcessesUIState()
    // `-initialPage processes` on the command line opens a specific screen (handy for profiling).
    @State private var page: Page = UserDefaults.standard.string(forKey: "initialPage").flatMap(Page.init(rawValue:)) ?? .overview
    @State private var showsPalette = false
    @AppStorage(Appearance.storageKey) private var appearance: Appearance = .system
    @AppStorage(MenuBarSettings.enabledKey) private var menuBarEnabled = true
    /// The menu bar item shows only while the window is closed: Procyon lives either in its window
    /// or in the menu bar, never both.
    @State private var windowOpen = false

    init() {
        let store = SystemStore()
        AlertNotifier.shared.install()
        store.onAlert = { AlertNotifier.shared.post($0) }
        _store = State(initialValue: store)
        _actions = State(initialValue: ProcessActionCenter(store: store))
        AppDelegate.store = store
    }

    var body: some Scene {
        Window("Procyon", id: "main") {
            RootView(page: $page, showsPalette: $showsPalette)
                .environment(store)
                .environment(actions)
                .environment(processesUI)
                .frame(minWidth: 940, minHeight: 600)
                .onAppear {
                    store.start()
                    appearance.apply()
                }
                // A closed `Window` keeps its views alive (no onDisappear), so follow the NSWindow.
                .trackingWindow { isOpen in
                    windowOpen = isOpen
                    store.needsProcesses("window", isOpen)
                    // Closed: Procyon lives in the menu bar (out of the Dock) and samples machine-wide
                    // metrics only.
                    NSApp.setActivationPolicy(isOpen || !MenuBarSettings.isEnabled ? .regular : .accessory)
                }
                .onChange(of: appearance) { _, value in value.apply() }
        }
        .defaultSize(width: 1220, height: 800)
        .windowStyle(.hiddenTitleBar)
        .commands { AppCommands(store: store, ui: processesUI, page: $page, showsPalette: $showsPalette) }
        // Settings is a page of the main window (sidebar, ⌘,), not a separate window.

        MenuBarExtra(
            isInserted: Binding(get: { menuBarEnabled && !windowOpen }, set: { if !$0 && !windowOpen { menuBarEnabled = false } })
        ) {
            MenuBarPanel(page: $page)
                .environment(store)
                .onAppear { store.start() }
        } label: {
            MenuBarLabel(isShown: menuBarEnabled && !windowOpen)
                .environment(store)
                .onAppear { store.start() }
        }
        .menuBarExtraStyle(.window)

    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    /// The app's store, so quitting can flush what it hasn't written yet.
    @MainActor static weak var store: SystemStore?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Launched as a bare executable (swift run): make it a regular foreground app.
        NSApp.setActivationPolicy(.regular)
        NSApp.activate()
        // `-launchInMenuBar YES` closes the window once it is up, as the user would, leaving Procyon in
        // the menu bar (scripts/bench-macos.py measures that state).
        if UserDefaults.standard.bool(forKey: "launchInMenuBar") { Self.closeWindowOnceShown() }
    }

    @MainActor private static func closeWindowOnceShown(attempts: Int = 100) {
        if let window = NSApp.windows.first(where: { $0.isVisible && $0.canBecomeMain }) {
            // Give the window tracking a moment to find its NSWindow, or it misses the close.
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { window.close() }
            return
        }
        guard attempts > 0 else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { closeWindowOnceShown(attempts: attempts - 1) }
    }

    /// Set by explicit Quit commands that run while no window is open (the menu bar panel).
    @MainActor static var quitRequested = false

    /// With the menu bar widget on, closing the window leaves Procyon running in the menu bar.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { !MenuBarSettings.isEnabled }

    /// SwiftUI quits by itself when its only `Window` closes, whatever the method above says. Cancel
    /// that one quit and stay in the menu bar; a real quit (⌘Q with the window open, Quit in the
    /// menu bar panel, logout or shutdown, which arrive as a quit Apple event) goes through.
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        let windowOpen = sender.windows.contains { $0.isVisible && $0.canBecomeMain }
        let quitEvent = NSAppleEventManager.shared().currentAppleEvent?.eventID == kAEQuitApplication
        guard MenuBarSettings.isEnabled, !windowOpen, !quitEvent, !Self.quitRequested else { return .terminateNow }
        sender.setActivationPolicy(.accessory)
        return .terminateCancel
    }

    /// Saves the history minute in progress, which would otherwise be lost with the process. A write
    /// takes milliseconds; the wait is bounded so a quit never hangs.
    func applicationWillTerminate(_ notification: Notification) {
        Self.store?.shutdown(waitingUpTo: 2)
    }
}

enum Page: String, CaseIterable, Identifiable, Hashable {
    case overview, processes, cpu, memory, gpu, disk, network, energy, startup, services, history, inspect, battery,
        system, settings

    var id: String { rawValue }

    var title: String {
        switch self {
        case .overview: "Overview"
        case .processes: "Processes"
        case .cpu: "CPU"
        case .memory: "Memory"
        case .disk: "Disk"
        case .network: "Network"
        case .gpu: "GPU"
        case .startup: "Startup"
        case .services: "Services"
        case .energy: "Energy"
        case .history: "History"
        case .inspect: "Files & Ports"
        case .battery: "Battery"
        case .system: "System"
        case .settings: "Settings"
        }
    }

    var symbol: String {
        switch self {
        case .overview: "square.grid.2x2.fill"
        case .processes: "list.bullet.rectangle.fill"
        case .startup: "power.circle.fill"
        case .services: "gearshape.2.fill"
        case .inspect: "point.3.connected.trianglepath.dotted"
        case .energy: "bolt.fill"
        case .history: "clock.arrow.circlepath"
        case .battery: "battery.75percent"
        case .system: "info.circle.fill"
        case .settings: "gearshape.fill"
        default: ""
        }
    }

    /// ⌘1 … ⌘9 in sidebar order; Battery and System have none, Settings has ⌘,.
    var shortcut: KeyEquivalent? {
        let numbered: [Page] = [.overview, .processes, .cpu, .memory, .gpu, .disk, .network, .startup, .services]
        return numbered.firstIndex(of: self).map { KeyEquivalent(Character("\($0 + 1)")) }
    }

    /// Whether this machine has what the page shows.
    func isAvailable(_ capabilities: Capabilities) -> Bool {
        switch self {
        case .gpu: capabilities.contains(.gpu)
        case .battery: capabilities.contains(.battery)
        case .startup: capabilities.contains(.startup)
        case .services: capabilities.contains(.services)
        case .inspect: capabilities.contains(.openFiles) || capabilities.contains(.connections)
        case .energy: capabilities.contains(.processEnergy)
        default: true
        }
    }
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

extension View {
    /// Calls `change(true)` when the hosting window opens or comes back, `change(false)` when it closes.
    func trackingWindow(_ change: @escaping (Bool) -> Void) -> some View {
        modifier(WindowTracking(change: change))
    }
}

private struct WindowTracking: ViewModifier {
    let change: (Bool) -> Void
    @State private var window: NSWindow?

    func body(content: Content) -> some View {
        content
            .background(WindowReader { window = $0 })
            .onChange(of: window) { _, window in if window != nil { change(true) } }
            .onReceive(NotificationCenter.default.publisher(for: NSWindow.willCloseNotification)) { note in
                if let window, note.object as? NSWindow === window { change(false) }
            }
            .onReceive(NotificationCenter.default.publisher(for: NSWindow.didBecomeMainNotification)) { note in
                if let window, note.object as? NSWindow === window { change(true) }
            }
    }
}

/// Hands over the NSWindow that hosts the view.
private struct WindowReader: NSViewRepresentable {
    let found: (NSWindow?) -> Void

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        DispatchQueue.main.async { found(view.window) }
        return view
    }

    func updateNSView(_ view: NSView, context: Context) {}
}
