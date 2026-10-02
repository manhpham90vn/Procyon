import ProcyonDesign
import ProcyonKit
import SwiftUI

/// Actions the focused processes screen exposes to the menu bar.
struct ProcessActions {
    var focusSearch: () -> Void
    var endTask: (() -> Void)?
    var forceQuit: (() -> Void)?
    var endTree: (() -> Void)?
    var suspendOrResume: (() -> Void)?
    var isSuspended = false
    var getInfo: (() -> Void)?
}

extension FocusedValues {
    @Entry var processActions: ProcessActions?
}

struct AppCommands: Commands {
    let store: SystemStore
    @Binding var page: Page
    @Binding var showsPalette: Bool
    @FocusedValue(\.processActions) private var processActions
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        CommandGroup(replacing: .appSettings) {
            Button("Settings…") {
                page = .settings
                openWindow(id: "main")
            }
            .keyboardShortcut(",")
        }

        CommandGroup(after: .textEditing) {
            Button("Find Process…") {
                page = .processes
                // The processes screen may not exist yet; let it appear before focusing.
                DispatchQueue.main.async { processActions?.focusSearch() }
            }
            .keyboardShortcut("f")
            Button("Command Palette…") { showsPalette.toggle() }
                .keyboardShortcut("k")
        }

        CommandGroup(before: .sidebar) {
            ForEach(Page.allCases.filter { $0 != .settings && $0.isAvailable(store.capabilities) }) { item in
                if let shortcut = item.shortcut {
                    Button(item.title) { page = item }
                        .keyboardShortcut(shortcut)
                } else {
                    Button(item.title) { page = item }
                }
            }
            Divider()
        }

        CommandMenu("Process") {
            Button("End Task") { processActions?.endTask?() }
                .keyboardShortcut(.delete)
                .disabled(processActions?.endTask == nil)
            Button("Force Quit") { processActions?.forceQuit?() }
                .keyboardShortcut(.delete, modifiers: [.command, .option])
                .disabled(processActions?.forceQuit == nil)
            Button("End Process Tree") { processActions?.endTree?() }
                .keyboardShortcut(.delete, modifiers: [.command, .option, .shift])
                .disabled(processActions?.endTree == nil)
            Divider()
            Button(processActions?.isSuspended == true ? "Resume" : "Suspend") { processActions?.suspendOrResume?() }
                .disabled(processActions?.suspendOrResume == nil)
            Button("Get Info") { processActions?.getInfo?() }
                .keyboardShortcut("i")
                .disabled(processActions?.getInfo == nil)
            Divider()
            Picker("View As", selection: Binding(get: { store.viewMode }, set: { store.viewMode = $0 })) {
                ForEach(ViewMode.allCases) { mode in
                    Text(mode.title).tag(mode)
                }
            }
        }

        CommandGroup(after: .toolbar) {
            Button(store.isPaused ? "Resume Updates" : "Pause Updates") { store.isPaused.toggle() }
                .keyboardShortcut("p", modifiers: [.command, .shift])
            Picker("Update Speed", selection: Binding(get: { store.interval }, set: { store.interval = $0 })) {
                ForEach(SystemStore.refreshIntervals, id: \.self) { interval in
                    Text(Format.interval(interval)).tag(interval)
                }
            }
            Divider()
        }
    }
}
