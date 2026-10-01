import Testing

@testable import ProcyonKit

@Suite struct ProcessRowVisibilityTests {
    private func row(_ id: String, depth: Int, children: Int = 0) -> ProcessRow {
        ProcessRow(
            id: id, kind: .process, pid: 1, parentID: nil, depth: depth, childCount: children, processCount: 1,
            name: id, user: "", path: "", appID: "", appName: "", flags: [], cpu: nil, memory: nil,
            diskRead: nil, diskWrite: nil, networkReceive: nil, networkSend: nil, threads: nil,
            startTime: nil, memberPIDs: [1])
    }

    @Test func collapsedRowHidesItsDescendantsOnly() {
        let rows = [
            row("a", depth: 0, children: 1), row("a1", depth: 1, children: 1), row("a1x", depth: 2),
            row("b", depth: 0, children: 1), row("b1", depth: 1),
        ]
        let visible = rows.visible { $0.id != "a" }
        #expect(visible.map(\.id) == ["a", "b", "b1"])
    }

    @Test func nestedCollapseKeepsSiblings() {
        let rows = [
            row("a", depth: 0, children: 2), row("a1", depth: 1, children: 1), row("a1x", depth: 2),
            row("a2", depth: 1),
        ]
        let visible = rows.visible { $0.id != "a1" }
        #expect(visible.map(\.id) == ["a", "a1", "a2"])
    }
}

@Suite struct MonitorTests {
    @Test func snapshotHasProcessesAndMemory() {
        let monitor = Monitor()
        let sample = monitor.refresh()
        #expect(sample.processCount > 0)
        #expect(sample.memoryTotal > 0)
        #expect(sample.coreUsage.count == Monitor.systemInfo().logicalCores)
    }

    @Test func groupedViewAggregatesMembers() {
        let monitor = Monitor()
        _ = monitor.refresh()
        let rows = monitor.buildView(.init(mode: .grouped, column: .memory, descending: true, filter: ""))
        for group in rows where group.kind == .group {
            #expect(group.processCount >= group.childCount)
            #expect(group.memberPIDs.count == group.processCount)
        }
    }

    @Test func treeFilterKeepsAncestors() {
        let monitor = Monitor()
        _ = monitor.refresh()
        let rows = monitor.buildView(.init(mode: .tree, column: .pid, descending: false, filter: "launchd"))
        #expect(rows.contains { $0.name == "launchd" })
        let ids = Set(rows.map(\.id))
        for row in rows { if let parent = row.parentID { #expect(ids.contains(parent)) } }
    }
}
