import AppKit
import ProcyonDesign
import ProcyonKit
import SwiftUI

/// The process list as a plain AppKit table.
///
/// Not a SwiftUI `Table`: that hosts a SwiftUI view graph in every cell and turns each re-sort into
/// animated row moves, which cost ~10% of a core and ~40 MB with hundreds of processes refreshing
/// every second. Here a refresh only rewrites the text of the visible cells.
struct ProcessTable: NSViewRepresentable {
    /// Rows to show, in display order.
    var rows: [ProcessRow]
    @Binding var selection: ProcessRow.ID?
    var sortColumn: ProcessColumn
    var sortDescending: Bool
    var hasNetwork: Bool
    var hasGPU: Bool
    var memoryTotal: Double
    var controller: ProcessTableController
    var isExpanded: (ProcessRow) -> Bool
    var isPinnedRoot: (ProcessRow) -> Bool
    var onSort: (ProcessColumn, Bool) -> Void
    var onToggle: (ProcessRow) -> Void
    var onUnpin: () -> Void
    /// Context menu items for a row.
    var menuItems: (ProcessRow) -> [NSMenuItem]

    func makeCoordinator() -> Coordinator { Coordinator(parent: self) }

    /// A column order saved before the GPU column existed puts it last; move it after Memory once,
    /// later the user's order wins.
    private func placeGPUColumnOnce(in table: NSTableView) {
        let key = "ProcessTable.gpuColumnPlaced"
        guard !UserDefaults.standard.bool(forKey: key) else { return }
        UserDefaults.standard.set(true, forKey: key)
        guard let gpu = table.tableColumns.firstIndex(where: { $0.identifier.rawValue == ColumnSpec.gpuID }),
            let memory = table.tableColumns.firstIndex(where: { $0.identifier.rawValue == "memory" })
        else { return }
        let target = gpu > memory ? memory + 1 : memory
        if gpu != target { table.moveColumn(gpu, toColumn: target) }
    }

    func makeNSView(context: Context) -> NSScrollView {
        let table = ProcessTableView()
        table.style = .inset
        table.rowHeight = 24
        table.backgroundColor = .clear
        table.usesAlternatingRowBackgroundColors = false
        table.allowsMultipleSelection = false
        table.allowsEmptySelection = true
        table.allowsColumnReordering = true
        table.columnAutoresizingStyle = .firstColumnOnlyAutoresizingStyle
        table.intercellSpacing = NSSize(width: 6, height: 0)

        for spec in ColumnSpec.all {
            let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(spec.id))
            column.title = spec.title
            column.minWidth = spec.minWidth
            column.width = spec.width
            column.resizingMask = .userResizingMask
            column.isHidden = spec.hiddenByDefault
            column.sortDescriptorPrototype = NSSortDescriptor(key: spec.id, ascending: !spec.column.prefersDescending)
            if spec.trailing { column.headerCell.alignment = .right }
            table.addTableColumn(column)
        }
        // Restores and then keeps saving widths, order and visibility in the user defaults.
        table.autosaveName = "ProcessTable"
        table.autosaveTableColumns = true
        placeGPUColumnOnce(in: table)

        let coordinator = context.coordinator
        coordinator.tableView = table
        table.dataSource = coordinator
        table.delegate = coordinator
        table.target = coordinator
        table.doubleAction = #selector(Coordinator.doubleClicked)
        table.onArrow = { [weak coordinator] expand in coordinator?.setSelected(expanded: expand) ?? false }

        let rowMenu = NSMenu()
        rowMenu.autoenablesItems = false
        rowMenu.delegate = coordinator
        table.menu = rowMenu
        let headerMenu = NSMenu()
        headerMenu.autoenablesItems = false
        headerMenu.delegate = coordinator
        table.headerView?.menu = headerMenu
        coordinator.headerMenu = headerMenu

        let scroll = NSScrollView()
        scroll.documentView = table
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        controller.coordinator = coordinator
        coordinator.apply()
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        context.coordinator.parent = self
        controller.coordinator = context.coordinator
        context.coordinator.apply()
    }

    @MainActor
    final class Coordinator: NSObject, NSTableViewDataSource, NSTableViewDelegate, NSMenuDelegate {
        var parent: ProcessTable
        weak var tableView: ProcessTableView?
        weak var headerMenu: NSMenu?
        private var rows: [ProcessRow] = []
        /// Expansion and pin state the cells were last drawn with.
        private var decoration: [Bool] = []
        /// Set while the table is changed from SwiftUI state, so delegate callbacks don't echo back.
        private var applying = false

        init(parent: ProcessTable) { self.parent = parent }

        func apply() {
            guard let table = tableView else { return }
            applying = true
            defer { applying = false }

            for id in ColumnSpec.networkIDs where !parent.hasNetwork {
                table.tableColumn(withIdentifier: .init(id))?.isHidden = true
            }
            if !parent.hasGPU { table.tableColumn(withIdentifier: .init(ColumnSpec.gpuID))?.isHidden = true }

            let wanted = [NSSortDescriptor(key: ColumnSpec.id(for: parent.sortColumn), ascending: !parent.sortDescending)]
            if table.sortDescriptors.first != wanted.first { table.sortDescriptors = wanted }

            let newDecoration = parent.rows.map { parent.isExpanded($0) } + parent.rows.map { parent.isPinnedRoot($0) }
            if parent.rows != rows || newDecoration != decoration {
                update(table, to: parent.rows)
                decoration = newDecoration
            }

            let index = parent.selection.flatMap(index(of:))
            if let index {
                if table.selectedRow != index { table.selectRowIndexes([index], byExtendingSelection: false) }
            } else if table.selectedRow >= 0 {
                table.deselectAll(nil)
            }
        }

        /// Rows are addressed by position: grow or shrink the table at the end, then rewrite the cells
        /// that exist. No reloadData: it throws every row view away and the next pass builds them again.
        private func update(_ table: NSTableView, to newRows: [ProcessRow]) {
            let oldCount = rows.count
            rows = newRows
            if newRows.count > oldCount {
                table.insertRows(at: IndexSet(oldCount..<newRows.count), withAnimation: [])
            } else if newRows.count < oldCount {
                table.removeRows(at: IndexSet(newRows.count..<oldCount), withAnimation: [])
            }
            let columns = table.tableColumns
            table.enumerateAvailableRowViews { rowView, index in
                guard self.rows.indices.contains(index) else { return }
                for (position, column) in columns.enumerated() where !column.isHidden {
                    guard let spec = ColumnSpec.byID[column.identifier.rawValue],
                        let cell = rowView.view(atColumn: position) as? NSView
                    else { continue }
                    self.configure(cell, spec, self.rows[index])
                }
            }
        }

        func index(of id: ProcessRow.ID) -> Int? { rows.firstIndex { $0.id == id } }

        /// Left/right arrow on the selected row; false when there is nothing to do.
        func setSelected(expanded: Bool) -> Bool {
            guard let table = tableView, rows.indices.contains(table.selectedRow) else { return false }
            let row = rows[table.selectedRow]
            guard row.hasChildren, parent.isExpanded(row) != expanded else { return false }
            parent.onToggle(row)
            return true
        }

        @objc func doubleClicked() {
            guard let table = tableView, rows.indices.contains(table.clickedRow) else { return }
            let row = rows[table.clickedRow]
            if row.hasChildren { parent.onToggle(row) }
        }

        // MARK: Data source & delegate

        func numberOfRows(in tableView: NSTableView) -> Int { rows.count }

        func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row index: Int) -> NSView? {
            guard let id = tableColumn?.identifier, let spec = ColumnSpec.byID[id.rawValue] else { return nil }
            let cell =
                tableView.makeView(withIdentifier: id, owner: nil)
                ?? {
                    let cell: NSView =
                        switch spec.column {
                        case .name: NameCell(frame: .zero)
                        case .pid, .user, .threads: TextCell(frame: .zero)
                        default: HeatCell(frame: .zero)
                        }
                    cell.identifier = id
                    return cell
                }()
            configure(cell, spec, rows[index])
            return cell
        }

        private func configure(_ cell: NSView, _ spec: ColumnSpec, _ row: ProcessRow) {
            switch (spec.column, cell) {
            case (.name, let cell as NameCell):
                cell.configure(
                    row, expanded: parent.isExpanded(row), pinned: parent.isPinnedRoot(row),
                    toggle: { [weak self] in self?.parent.onToggle(row) }, unpin: { [weak self] in self?.parent.onUnpin() })
            case (.pid, let cell as TextCell):
                cell.configure(String(row.pid), font: CellStyle.mono, color: CellStyle.secondary, trailing: false)
            case (.user, let cell as TextCell):
                cell.configure(row.user, font: CellStyle.body, color: CellStyle.secondary, trailing: false)
            case (.threads, let cell as TextCell):
                cell.configure(Format.count(row.threads), font: CellStyle.digits, color: CellStyle.secondary, trailing: true)
            case (.cpu, let cell as HeatCell):
                cell.configure(Format.cpu(row.cpu), intensity: (row.cpu ?? 0) / 100, metric: .cpu, restricted: row.cpu == nil)
            case (.memory, let cell as HeatCell):
                cell.configure(
                    Format.bytes(row.memory), intensity: Double(row.memory ?? 0) / parent.memoryTotal * 6, metric: .memory,
                    restricted: row.memory == nil)
            case (.diskRead, let cell as HeatCell):
                cell.configure(
                    Format.rate(row.diskRead), intensity: (row.diskRead ?? 0) / 20_000_000, metric: .disk,
                    restricted: row.diskRead == nil)
            case (.diskWrite, let cell as HeatCell):
                cell.configure(
                    Format.rate(row.diskWrite), intensity: (row.diskWrite ?? 0) / 20_000_000, metric: .disk,
                    restricted: row.diskWrite == nil)
            case (.networkReceive, let cell as HeatCell):
                cell.configure(
                    Format.rate(row.networkReceive), intensity: (row.networkReceive ?? 0) / 10_000_000, metric: .network,
                    restricted: row.networkReceive == nil)
            case (.networkSend, let cell as HeatCell):
                cell.configure(
                    Format.rate(row.networkSend), intensity: (row.networkSend ?? 0) / 10_000_000, metric: .network,
                    restricted: row.networkSend == nil)
            case (.gpu, let cell as HeatCell):
                cell.configure(Format.cpu(row.gpu), intensity: (row.gpu ?? 0) / 100, metric: .gpu, restricted: row.gpu == nil)
            default:
                break
            }
        }

        func tableViewSelectionDidChange(_ notification: Notification) {
            guard !applying, let table = tableView else { return }
            parent.selection = rows.indices.contains(table.selectedRow) ? rows[table.selectedRow].id : nil
        }

        func tableView(_ tableView: NSTableView, sortDescriptorsDidChange oldDescriptors: [NSSortDescriptor]) {
            guard !applying, let first = tableView.sortDescriptors.first, let key = first.key,
                let spec = ColumnSpec.byID[key]
            else { return }
            parent.onSort(spec.column, !first.ascending)
        }

        // MARK: Menus

        func menuNeedsUpdate(_ menu: NSMenu) {
            menu.removeAllItems()
            guard let table = tableView else { return }
            if menu === headerMenu {
                for spec in ColumnSpec.all where spec.column != .name {
                    guard let column = table.tableColumn(withIdentifier: .init(spec.id)) else { continue }
                    let item = ActionMenuItem(spec.menuTitle) { column.isHidden.toggle() }
                    item.state = column.isHidden ? .off : .on
                    item.isEnabled =
                        (parent.hasNetwork || !ColumnSpec.networkIDs.contains(spec.id))
                        && (parent.hasGPU || spec.id != ColumnSpec.gpuID)
                    menu.addItem(item)
                }
            } else if rows.indices.contains(table.clickedRow) {
                for item in parent.menuItems(rows[table.clickedRow]) { menu.addItem(item) }
            }
        }
    }
}

/// Imperative actions on the table that SwiftUI state can't express.
@MainActor
final class ProcessTableController {
    fileprivate weak var coordinator: ProcessTable.Coordinator?

    /// Scrolls `id` to the middle of the table and gives the table keyboard focus.
    func reveal(_ id: ProcessRow.ID) {
        guard let coordinator, let table = coordinator.tableView, let index = coordinator.index(of: id),
            let scroll = table.enclosingScrollView
        else { return }
        let clip = scroll.contentView
        let rect = table.rect(ofRow: index)
        let top = -clip.contentInsets.top
        let bottom = max(top, table.frame.height - clip.bounds.height)
        clip.scroll(to: NSPoint(x: clip.bounds.minX, y: min(max(rect.midY - clip.bounds.height / 2, top), bottom)))
        scroll.reflectScrolledClipView(clip)
        table.window?.makeFirstResponder(table)
    }
}

/// Collapses and expands the selected row with the arrow keys, like an outline view.
final class ProcessTableView: NSTableView {
    var onArrow: ((Bool) -> Bool)?
    private var fitted = false

    override func layout() {
        super.layout()
        // Once the table has its real width, give the name column what the other columns leave so
        // the last one isn't cut off. Only once: later widths are the user's, and firstColumnOnly
        // autoresizing keeps them fitting when the window resizes.
        guard !fitted, let clip = enclosingScrollView?.contentView, clip.bounds.width > 0,
            let name = tableColumns.first(where: { $0.identifier.rawValue == "name" })
        else { return }
        fitted = true
        let visible = tableColumns.indices.filter { !tableColumns[$0].isHidden }
        guard let first = visible.first, let last = visible.last else { return }
        // The inset style pads both edges by the leading inset of the first column.
        let excess = rect(ofColumn: last).maxX + rect(ofColumn: first).minX - clip.bounds.width
        name.width = max(name.minWidth, name.width - excess)
    }

    override func keyDown(with event: NSEvent) {
        let left: UInt16 = 123, right: UInt16 = 124
        if event.keyCode == left || event.keyCode == right,
            event.modifierFlags.intersection([.command, .option, .control, .shift]).isEmpty,
            onArrow?(event.keyCode == right) == true
        {
            return
        }
        super.keyDown(with: event)
    }
}

/// A menu item that runs a closure.
final class ActionMenuItem: NSMenuItem {
    private let handler: () -> Void

    init(_ title: String, isEnabled: Bool = true, handler: @escaping () -> Void) {
        self.handler = handler
        super.init(title: title, action: #selector(run), keyEquivalent: "")
        target = self
        self.isEnabled = isEnabled
    }

    @available(*, unavailable)
    required init(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    @objc private func run() { handler() }
}

// MARK: - Columns

private struct ColumnSpec {
    let column: ProcessColumn
    let id: String
    let title: String
    var menuTitle: String { column == .networkReceive ? "Network Received" : column == .networkSend ? "Network Sent" : title }
    let minWidth: CGFloat
    let width: CGFloat
    var trailing = true
    var hiddenByDefault = false

    static let all: [ColumnSpec] = [
        ColumnSpec(column: .name, id: "name", title: "Name", minWidth: 180, width: 250, trailing: false),
        ColumnSpec(column: .pid, id: "pid", title: "PID", minWidth: 50, width: 64, trailing: false),
        ColumnSpec(column: .user, id: "user", title: "User", minWidth: 56, width: 72, trailing: false),
        ColumnSpec(column: .cpu, id: "cpu", title: "CPU", minWidth: 56, width: 70),
        ColumnSpec(column: .memory, id: "memory", title: "Memory", minWidth: 64, width: 82),
        ColumnSpec(column: .gpu, id: "gpu", title: "GPU", minWidth: 50, width: 64),
        ColumnSpec(column: .diskRead, id: "diskRead", title: "Disk Read", minWidth: 64, width: 78),
        ColumnSpec(column: .diskWrite, id: "diskWrite", title: "Disk Write", minWidth: 64, width: 78),
        ColumnSpec(column: .networkReceive, id: "networkReceive", title: "Net ↓", minWidth: 64, width: 78),
        ColumnSpec(column: .networkSend, id: "networkSend", title: "Net ↑", minWidth: 64, width: 78),
        ColumnSpec(column: .threads, id: "threads", title: "Threads", minWidth: 50, width: 64, hiddenByDefault: true),
    ]
    static let byID = Dictionary(uniqueKeysWithValues: all.map { ($0.id, $0) })
    static let networkIDs: Set<String> = ["networkReceive", "networkSend"]
    static let gpuID = "gpu"

    static func id(for column: ProcessColumn) -> String { all.first { $0.column == column }?.id ?? "cpu" }
}

// MARK: - Cells

/// Fonts and colors of the cells, resolved once. The palette colors follow the appearance.
@MainActor
private enum CellStyle {
    static let body = NSFont.systemFont(ofSize: 13)
    static let headline = NSFont.systemFont(ofSize: 13, weight: .semibold)
    static let digits = NSFont.monospacedDigitSystemFont(ofSize: 13, weight: .regular)
    static let mono = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
    static let count = NSFont.monospacedDigitSystemFont(ofSize: 10, weight: .medium)
    static let primary = NSColor(Tokens.Palette.textPrimary)
    static let secondary = NSColor(Tokens.Palette.textSecondary)
    static let tertiary = NSColor(Tokens.Palette.textTertiary)
    static let accent = NSColor(Tokens.Palette.accent)
    static let track = NSColor(Tokens.Palette.track)
    static let sunken = NSColor(Tokens.Palette.surfaceSunken)
    static let border = NSColor(Tokens.Palette.border)
    static let heat: [Metric: NSColor] = Dictionary(
        uniqueKeysWithValues: [Metric.cpu, .memory, .disk, .network, .gpu].map { ($0, NSColor($0.style.start)) })

    static func lineHeight(_ font: NSFont) -> CGFloat { ceil(font.ascender - font.descender + font.leading) }

    private static var symbols: [String: NSImage] = [:]

    /// The pin tilted 45° like a pushpin. The image is drawn tilted: rotating the view would move
    /// its frame off the place it is laid out at.
    static let pin: NSImage? = {
        guard let base = symbol("pin.fill", size: 10, weight: .semibold) else { return nil }
        let image = NSImage(size: NSSize(width: 16, height: 16), flipped: false) { rect in
            let transform = NSAffineTransform()
            transform.translateX(by: rect.midX, yBy: rect.midY)
            transform.rotate(byDegrees: -45)
            transform.concat()
            base.draw(in: NSRect(origin: NSPoint(x: -base.size.width / 2, y: -base.size.height / 2), size: base.size))
            return true
        }
        image.isTemplate = true
        return image
    }()

    static func symbol(_ name: String, size: CGFloat, weight: NSFont.Weight = .regular) -> NSImage? {
        let key = "\(name)-\(size)-\(weight.rawValue)"
        if let cached = symbols[key] { return cached }
        let image = NSImage(systemSymbolName: name, accessibilityDescription: nil)?
            .withSymbolConfiguration(.init(pointSize: size, weight: weight))
        symbols[key] = image
        return image
    }

    static func label() -> NSTextField {
        let label = NSTextField(labelWithString: "")
        label.lineBreakMode = .byTruncatingTail
        label.maximumNumberOfLines = 1
        return label
    }
}

/// A layer-backed rounded rectangle whose colors follow the appearance.
private final class ChromeView: NSView {
    var fill: NSColor? { didSet { needsDisplay = true } }
    var stroke: NSColor? { didSet { needsDisplay = true } }
    var radius: CGFloat = 0 { didSet { needsDisplay = true } }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layerContentsRedrawPolicy = .onSetNeedsDisplay
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    override var wantsUpdateLayer: Bool { true }

    override func updateLayer() {
        guard let layer else { return }
        layer.cornerRadius = radius
        layer.cornerCurve = .continuous
        layer.backgroundColor = fill?.cgColor
        layer.borderColor = stroke?.cgColor
        layer.borderWidth = stroke == nil ? 0 : 0.5
    }
}

private final class TextCell: NSTableCellView {
    private let label = CellStyle.label()

    override init(frame: NSRect) {
        super.init(frame: frame)
        addSubview(label)
        textField = label
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    func configure(_ text: String, font: NSFont, color: NSColor, trailing: Bool) {
        if label.stringValue != text { label.stringValue = text }
        if label.font != font {
            label.font = font
            needsLayout = true
        }
        if label.textColor != color { label.textColor = color }
        let alignment: NSTextAlignment = trailing ? .right : .left
        if label.alignment != alignment { label.alignment = alignment }
    }

    override func layout() {
        super.layout()
        let height = CellStyle.lineHeight(label.font ?? CellStyle.body)
        label.frame = NSRect(x: 0, y: (bounds.height - height) / 2, width: bounds.width, height: height)
    }
}

/// Task-Manager-style heat cell: the background intensity follows usage.
private final class HeatCell: NSTableCellView {
    private let label = CellStyle.label()
    private let background = CALayer()

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        background.cornerRadius = Tokens.Radius.xs
        background.cornerCurve = .continuous
        // No implicit animations: values change every second.
        background.actions = ["backgroundColor": NSNull(), "bounds": NSNull(), "position": NSNull(), "hidden": NSNull()]
        layer?.addSublayer(background)
        label.font = CellStyle.digits
        label.alignment = .right
        addSubview(label)
        textField = label
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    func configure(_ text: String, intensity: Double, metric: Metric, restricted: Bool) {
        if label.stringValue != text { label.stringValue = text }
        let color = restricted ? CellStyle.tertiary : CellStyle.primary
        if label.textColor != color { label.textColor = color }
        let heat = min(max(intensity, 0), 1)
        background.isHidden = heat <= 0.01
        if heat > 0.01 { background.backgroundColor = CellStyle.heat[metric]?.withAlphaComponent(0.08 + 0.42 * heat).cgColor }
    }

    override func layout() {
        super.layout()
        let height = CellStyle.lineHeight(CellStyle.digits)
        let y = (bounds.height - height) / 2
        background.frame = NSRect(x: 0, y: y - 1, width: bounds.width, height: height + 2)
        // 4 pt plus the label's own 2 pt inset: the 6 pt padding of the design.
        label.frame = NSRect(x: 4, y: y, width: max(0, bounds.width - 8), height: height)
    }
}

private final class NameCell: NSTableCellView {
    private let disclosure = ClickableImage()
    private let icon = IconView()
    private let label = CellStyle.label()
    private let countPill = ChromeView()
    private let countLabel = CellStyle.label()
    private let lock = NSImageView()
    private let paused = NSImageView()
    private let pin = ClickableImage()
    /// What the frames depend on; layout runs only when it changes.
    private struct Layout: Equatable {
        var depth: Int
        var hasChildren: Bool
        var name: String
        var font: NSFont
        var count: String?
        var locked: Bool
        var suspended: Bool
        var pinned: Bool
    }

    private var current: Layout?
    private var toggle: (() -> Void)?
    private var unpin: (() -> Void)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        disclosure.contentTintColor = CellStyle.tertiary
        disclosure.onClick = { [weak self] in self?.toggle?() }

        label.lineBreakMode = .byTruncatingMiddle

        countPill.fill = CellStyle.track
        countPill.radius = 7
        countLabel.font = CellStyle.count
        countLabel.textColor = CellStyle.secondary
        countLabel.alignment = .center
        countPill.addSubview(countLabel)

        lock.image = CellStyle.symbol("lock.fill", size: 9)
        lock.contentTintColor = CellStyle.tertiary
        lock.toolTip = "Owned by the system. Unlock full access to read its CPU, memory and disk usage."

        paused.image = CellStyle.symbol("pause.circle.fill", size: 11)
        paused.contentTintColor = NSColor(Tokens.Palette.warning)
        paused.toolTip = "Suspended. Choose Resume to let it run again."
        paused.setAccessibilityLabel("Suspended")

        pin.image = CellStyle.pin
        pin.contentTintColor = CellStyle.accent
        pin.toolTip = "Pinned to the top in every view. Click to unpin."
        pin.setAccessibilityLabel("Unpin")
        pin.onClick = { [weak self] in self?.unpin?() }

        for view in [disclosure, icon, label, countPill, lock, paused, pin] { addSubview(view) }
        textField = label
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    func configure(
        _ row: ProcessRow, expanded: Bool, pinned: Bool, toggle: @escaping () -> Void, unpin: @escaping () -> Void
    ) {
        self.toggle = toggle
        self.unpin = unpin
        let count = row.kind == .group ? "\(row.processCount)" : nil
        let font = row.kind == .group ? CellStyle.headline : CellStyle.body
        let layout = Layout(
            depth: row.depth, hasChildren: row.hasChildren, name: row.name, font: font, count: count,
            locked: row.isRestricted, suspended: row.isSuspended, pinned: pinned)
        if layout != current {
            current = layout
            disclosure.isHidden = !row.hasChildren
            if label.stringValue != row.name { label.stringValue = row.name }
            label.font = font
            countPill.isHidden = count == nil
            countLabel.stringValue = count ?? ""
            lock.isHidden = !row.isRestricted
            paused.isHidden = !row.isSuspended
            pin.isHidden = !pinned
            needsLayout = true
        }

        let chevron = CellStyle.symbol(expanded ? "chevron.down" : "chevron.right", size: 9, weight: .bold)
        if disclosure.image !== chevron {
            disclosure.image = chevron
            disclosure.setAccessibilityLabel(expanded ? "Collapse" : "Expand")
        }
        icon.configure(row)
        let color = row.isRestricted ? CellStyle.secondary : CellStyle.primary
        if label.textColor != color { label.textColor = color }
    }

    override func layout() {
        super.layout()
        let midY = bounds.height / 2
        var x = CGFloat(current?.depth ?? 0) * 16
        disclosure.frame = NSRect(x: x, y: midY - 7, width: 14, height: 14)
        x += 14 + 6
        icon.frame = NSRect(x: x, y: midY - 9, width: 18, height: 18)
        x += 18 + 6

        let trailingEdge = pin.isHidden ? bounds.width : bounds.width - 16 - 4
        pin.frame = NSRect(x: bounds.width - 16, y: midY - 8, width: 16, height: 16)

        let countWidth = countPill.isHidden ? 0 : ceil(countLabel.intrinsicContentSize.width) + 10
        let lockWidth: CGFloat = lock.isHidden ? 0 : 10
        let pausedWidth: CGFloat = paused.isHidden ? 0 : 12
        let extras =
            (countWidth > 0 ? countWidth + 6 : 0) + (lockWidth > 0 ? lockWidth + 6 : 0) + (pausedWidth > 0 ? pausedWidth + 6 : 0)
        let height = CellStyle.lineHeight(label.font ?? CellStyle.body)
        // The label cell insets its text by 2 pt per side; without them the text truncates.
        let width = min(ceil(label.intrinsicContentSize.width) + 4, max(0, trailingEdge - x - extras))
        label.frame = NSRect(x: x, y: midY - height / 2, width: width, height: height)
        x += width + 6

        if countWidth > 0 {
            countPill.frame = NSRect(x: x, y: midY - 7, width: countWidth, height: 14)
            let countHeight = CellStyle.lineHeight(CellStyle.count)
            countLabel.frame = NSRect(x: 0, y: (14 - countHeight) / 2, width: countWidth, height: countHeight)
            x += countWidth + 6
        }
        lock.frame = NSRect(x: x, y: midY - 5, width: lockWidth, height: 10)
        if lockWidth > 0 { x += lockWidth + 6 }
        paused.frame = NSRect(x: x, y: midY - 6, width: pausedWidth, height: 12)
    }
}

/// App icon, or a symbol on a sunken tile for processes without a bundle.
private final class IconView: NSView {
    private let tile = ChromeView()
    private let image = NSImageView()

    override init(frame: NSRect) {
        super.init(frame: frame)
        tile.fill = CellStyle.sunken
        tile.stroke = CellStyle.border
        tile.radius = 4.5
        image.imageScaling = .scaleProportionallyUpOrDown
        addSubview(tile)
        addSubview(image)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    private var shown: String?

    func configure(_ row: ProcessRow) {
        let key = row.bundlePath ?? (row.isSystem ? "system" : "process")
        guard key != shown else { return }
        shown = key
        if let bundle = row.bundlePath {
            tile.isHidden = true
            image.image = IconCache.icon(for: bundle, size: 18)
            image.contentTintColor = nil
            image.imageScaling = .scaleProportionallyUpOrDown
        } else {
            tile.isHidden = false
            image.image = CellStyle.symbol(row.isSystem ? "gearshape.2.fill" : "terminal.fill", size: 9.5, weight: .semibold)
            image.contentTintColor = CellStyle.tertiary
            image.imageScaling = .scaleNone
        }
        needsLayout = true
    }

    override func layout() {
        super.layout()
        tile.frame = bounds
        image.frame = bounds
    }
}

/// An image that acts on a click, without a button's text measuring and bezel layout.
private final class ClickableImage: NSImageView {
    var onClick: (() -> Void)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        imageScaling = .scaleNone
        setAccessibilityRole(.button)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    override func mouseDown(with event: NSEvent) { onClick?() }
    override func accessibilityPerformPress() -> Bool {
        onClick?()
        return true
    }
}
