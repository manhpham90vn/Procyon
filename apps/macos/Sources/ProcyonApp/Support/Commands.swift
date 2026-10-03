import ProcyonDesign
import ProcyonKit
import SwiftUI

/// What the focused Processes screen exposes to the main menu: the selected row and the center that
/// acts on it. Equal while the state the menu depends on is unchanged, so the menu isn't re-evaluated
/// on every sample.
struct ProcessActions: Equatable {
    /// The selected row, if any (as of the last change of selection; actions re-read it live).
    var row: ProcessRow?
    var canSuspend: Bool
    /// While the search field is being typed in, ⌘⌫ belongs to the text, not to End Task.
    var searchFocused: Bool
    let center: ProcessActionCenter

    private var editable: ProcessRow? { row.flatMap { $0.isProtected ? nil : $0 } }
    var canEnd: Bool { editable != nil && !searchFocused }
    var canSuspendOrResume: Bool { canSuspend && editable != nil }
    var isSuspended: Bool { row?.isSuspended ?? false }
    var canInspect: Bool { row != nil }

    @MainActor func endTask() { if let row = editable { center.request(.end, row) } }
    @MainActor func forceQuit() { if let row = editable { center.request(.forceQuit, row) } }
    @MainActor func endTree() { if let row = editable { center.request(.endTree, row) } }
    @MainActor func suspendOrResume() {
        if let row = editable { center.request(row.isSuspended ? .resume : .suspend, row) }
    }
    @MainActor func getInfo() { if let row { center.inspect(row) } }

    static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.row?.id == rhs.row?.id && lhs.row?.isProtected == rhs.row?.isProtected
            && lhs.row?.isSuspended == rhs.row?.isSuspended && lhs.canSuspend == rhs.canSuspend
            && lhs.searchFocused == rhs.searchFocused && lhs.center === rhs.center
    }
}

extension FocusedValues {
    @Entry var processActions: ProcessActions?
}

struct AppCommands: Commands {
    let store: SystemStore
    let ui: ProcessesUIState
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
                // The Processes screen picks the request up once it is on screen.
                ui.wantsSearchFocus = true
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
            // Disabled while the search field has focus, so ⌘⌫ deletes text there instead of ending
            // the selected process.
            Button("End Task") { processActions?.endTask() }
                .keyboardShortcut(.delete)
                .disabled(processActions?.canEnd != true)
            Button("Force Quit") { processActions?.forceQuit() }
                .keyboardShortcut(.delete, modifiers: [.command, .option])
                .disabled(processActions?.canEnd != true)
            Button("End Process Tree") { processActions?.endTree() }
                .keyboardShortcut(.delete, modifiers: [.command, .option, .shift])
                .disabled(processActions?.canEnd != true)
            Divider()
            Button(processActions?.isSuspended == true ? "Resume" : "Suspend") { processActions?.suspendOrResume() }
                .disabled(processActions?.canSuspendOrResume != true)
            Button("Get Info") { processActions?.getInfo() }
                .keyboardShortcut("i")
                .disabled(processActions?.canInspect != true)
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
