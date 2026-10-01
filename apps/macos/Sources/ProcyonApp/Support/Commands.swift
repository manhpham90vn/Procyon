import ProcyonDesign
import ProcyonKit
import SwiftUI

/// Actions the focused processes screen exposes to the menu bar.
struct ProcessActions {
    var focusSearch: () -> Void
    var endTask: (() -> Void)?
    var forceQuit: (() -> Void)?
    var endTree: (() -> Void)?
}

extension FocusedValues {
    @Entry var processActions: ProcessActions?
}

struct AppCommands: Commands {
    let store: SystemStore
    @Binding var page: Page
    @FocusedValue(\.processActions) private var processActions

    var body: some Commands {
        CommandGroup(after: .textEditing) {
            Button("Find Process…") {
                page = .processes
                // The processes screen may not exist yet; let it appear before focusing.
                DispatchQueue.main.async { processActions?.focusSearch() }
            }
            .keyboardShortcut("f")
        }

        CommandGroup(before: .sidebar) {
            ForEach(Page.allCases) { item in
                Button(item.title) { page = item }
                    .keyboardShortcut(item.shortcut)
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
