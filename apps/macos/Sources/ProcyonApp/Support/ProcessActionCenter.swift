import AppKit
import ProcyonDesign
import ProcyonKit
import SwiftUI

/// Every action on a process, whoever asks for it (the Processes table, the menu bar commands, the
/// command palette): confirmation for risky ones, the failure alert, and the Get Info sheet.
@MainActor
@Observable
final class ProcessActionCenter {
    enum Kind: Hashable {
        case end, forceQuit, endTree, suspend, resume
        case signal(ProcessSignal)
        case priority(ProcessPriority)
    }

    struct Pending: Identifiable {
        let kind: Kind
        let row: ProcessRow
        var id: String { "\(kind)-\(row.id)" }
    }

    struct Failure: Identifiable {
        let id = UUID()
        let title: String
        let message: String
    }

    var pending: Pending?
    var failure: Failure?
    /// The process shown in the Get Info sheet.
    var inspecting: ProcessRow?

    private let store: SystemStore

    init(store: SystemStore) { self.store = store }

    var capabilities: Capabilities { store.capabilities }

    func request(_ kind: Kind, _ row: ProcessRow) {
        let pending = Pending(kind: kind, row: row)
        if needsConfirmation(pending) {
            self.pending = pending
        } else {
            perform(pending)
        }
    }

    func inspect(_ row: ProcessRow) { inspecting = row }

    private func needsConfirmation(_ action: Pending) -> Bool {
        let row = action.row
        switch action.kind {
        // Plain "End Task" on a user process needs no confirmation, like the OS task managers.
        case .end: return row.isSystem || row.kind == .group
        case .forceQuit, .endTree: return true
        case .suspend: return true
        case .resume, .priority: return false
        case .signal(let signal): return signal.isDisruptive || row.isSystem
        }
    }

    func perform(_ action: Pending) {
        Task {
            let row = action.row
            let result: ActionResult
            switch action.kind {
            case .end: result = await store.end(row, force: false)
            case .forceQuit: result = await store.end(row, force: true)
            case .endTree: result = await store.endTree(row)
            case .suspend: result = await store.suspend(row)
            case .resume: result = await store.resume(row)
            case .signal(let signal): result = await store.signal(row, signal)
            case .priority(let priority): result = await store.setPriority(row, priority)
            }
            guard result.isFailure else { return }
            var message = result.message
            if result == .permissionDenied, !store.fullAccess.isOn {
                message += " Unlock Full Access in Settings to change processes of other users and to raise priority."
            }
            failure = Failure(title: "Couldn't \(action.verb) “\(row.name)”", message: message)
        }
    }

    // MARK: - Menus

    /// The context menu of a process row.
    func menuItems(for row: ProcessRow) -> [NSMenuItem] {
        let enabled = !row.isProtected
        var items: [NSMenuItem] = [
            ActionMenuItem(row.kind == .group ? "End \(row.processCount) Processes" : "End Task", isEnabled: enabled) {
                self.request(.end, row)
            },
            ActionMenuItem("Force Quit", isEnabled: enabled) { self.request(.forceQuit, row) },
        ]
        if row.kind == .process && row.hasChildren {
            items.append(ActionMenuItem("End Process Tree", isEnabled: enabled) { self.request(.endTree, row) })
        }
        items.append(.separator())
        if capabilities.contains(.suspend) {
            items.append(
                row.isSuspended
                    ? ActionMenuItem("Resume", isEnabled: enabled) { self.request(.resume, row) }
                    : ActionMenuItem("Suspend", isEnabled: enabled) { self.request(.suspend, row) })
        }
        if capabilities.contains(.priority) { items.append(priorityMenu(for: row, isEnabled: enabled)) }
        if capabilities.contains(.signals) { items.append(signalMenu(for: row, isEnabled: enabled)) }
        items.append(.separator())
        items.append(ActionMenuItem("Get Info") { self.inspect(row) })
        if !row.path.isEmpty {
            items.append(ActionMenuItem("Show in Finder") { Self.showInFinder(row) })
        }
        items.append(ActionMenuItem("Copy Name") { Self.copy(row.name) })
        items.append(ActionMenuItem("Copy PID") { Self.copy(String(row.pid)) })
        if !row.path.isEmpty { items.append(ActionMenuItem("Copy Path") { Self.copy(row.path) }) }
        return items
    }

    private func priorityMenu(for row: ProcessRow, isEnabled: Bool) -> NSMenuItem {
        let item = NSMenuItem(title: "Priority", action: nil, keyEquivalent: "")
        let menu = NSMenu()
        menu.autoenablesItems = false
        for priority in ProcessPriority.allCases {
            let option = ActionMenuItem(priority.title, isEnabled: isEnabled) { self.request(.priority(priority), row) }
            option.state = row.nice == priority.rawValue ? .on : .off
            if priority.needsPrivileges && !store.fullAccess.isOn { option.toolTip = "Needs full access" }
            menu.addItem(option)
        }
        if ProcessPriority(rawValue: row.nice) == nil {
            menu.addItem(.separator())
            let custom = NSMenuItem(title: "Current: nice \(row.nice)", action: nil, keyEquivalent: "")
            custom.isEnabled = false
            menu.addItem(custom)
        }
        item.submenu = menu
        item.isEnabled = isEnabled
        return item
    }

    private func signalMenu(for row: ProcessRow, isEnabled: Bool) -> NSMenuItem {
        let item = NSMenuItem(title: "Send Signal", action: nil, keyEquivalent: "")
        let menu = NSMenu()
        menu.autoenablesItems = false
        for signal in ProcessSignal.allCases {
            let option = ActionMenuItem("\(signal.name) — \(signal.meaning)", isEnabled: isEnabled) {
                self.request(.signal(signal), row)
            }
            menu.addItem(option)
        }
        item.submenu = menu
        item.isEnabled = isEnabled
        return item
    }

    static func showInFinder(_ row: ProcessRow) {
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: row.bundlePath ?? row.path)])
    }

    static func copy(_ text: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }
}

extension ProcessActionCenter.Pending {
    var title: String {
        switch kind {
        case .end: row.kind == .group ? "End all \(row.processCount) “\(row.name)” processes?" : "End “\(row.name)”?"
        case .forceQuit: "Force quit “\(row.name)”?"
        case .endTree: "End “\(row.name)” and all its child processes?"
        case .suspend: "Suspend “\(row.name)”?"
        case .resume: "Resume “\(row.name)”?"
        case .signal(let signal): "Send \(signal.name) to “\(row.name)”?"
        case .priority(let priority): "Set “\(row.name)” to \(priority.title) priority?"
        }
    }

    var confirmTitle: String {
        switch kind {
        case .end: "End Task"
        case .forceQuit: "Force Quit"
        case .endTree: "End Process Tree"
        case .suspend: "Suspend"
        case .resume: "Resume"
        case .signal(let signal): "Send \(signal.name)"
        case .priority: "Change Priority"
        }
    }

    var message: String {
        var text =
            switch kind {
            case .end: "The process will be asked to quit."
            case .forceQuit: "The process stops immediately. Unsaved changes will be lost."
            case .endTree: "Every descendant process stops immediately. Unsaved changes will be lost."
            case .suspend: "It stops running until you resume it. A suspended app looks frozen."
            case .resume: "It continues where it stopped."
            case .signal(let signal): "\(signal.meaning)."
            case .priority: "The scheduler gives it more or less CPU time when the system is busy."
            }
        if row.kind == .group { text += "\n\nThis applies to all \(row.processCount) processes of the app." }
        if row.isSystem { text += "\n\n“\(row.name)” is a system process. Changing it can make macOS unstable." }
        // What the process is, when it is a well-known one, so the user knows what they are ending.
        switch kind {
        case .end, .forceQuit, .endTree:
            if let explanation = ProcessCatalog.explain(name: row.name, appName: row.appName) {
                text += "\n\n\(explanation.summary) \(explanation.advice.title)."
            }
        default: break
        }
        return text
    }

    /// For "Couldn't … “name”".
    var verb: String {
        switch kind {
        case .end, .forceQuit, .endTree: "end"
        case .suspend: "suspend"
        case .resume: "resume"
        case .signal(let signal): "send \(signal.name) to"
        case .priority: "change the priority of"
        }
    }
}

extension View {
    /// Confirmation dialog, failure alert and Get Info sheet of `center`.
    func processActionDialogs(_ center: ProcessActionCenter) -> some View {
        modifier(ProcessActionDialogs(center: center))
    }
}

private struct ProcessActionDialogs: ViewModifier {
    @Bindable var center: ProcessActionCenter

    func body(content: Content) -> some View {
        content
            .confirmationDialog(
                center.pending?.title ?? "",
                isPresented: Binding(get: { center.pending != nil }, set: { if !$0 { center.pending = nil } }),
                presenting: center.pending
            ) { action in
                Button(action.confirmTitle, role: .destructive) { center.perform(action) }
                Button("Cancel", role: .cancel) {}
            } message: { action in
                Text(action.message)
            }
            .alert(
                center.failure?.title ?? "",
                isPresented: Binding(get: { center.failure != nil }, set: { if !$0 { center.failure = nil } }),
                presenting: center.failure
            ) { _ in
                Button("OK", role: .cancel) {}
            } message: { failure in
                Text(failure.message)
            }
            .sheet(item: $center.inspecting) { row in
                ProcessInfoSheet(row: row)
            }
    }
}
