import Testing

@testable import ProcyonKit

@Suite struct ProcessRowVisibilityTests {
    private func row(_ id: String, depth: Int, children: Int = 0, app: String = "") -> ProcessRow {
        ProcessRow(
            id: id, kind: .process, pid: 1, parentID: nil, depth: depth, childCount: children, processCount: 1,
            name: id, user: "", path: "", appID: app, appName: "", flags: [], cpu: nil, memory: nil,
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

    @Test func pinningMovesTheAppsSubtreesToTheTop() {
        // Tree: launchd > [x > [safari > [helper (other app)]], safari2]
        let rows = [
            row("launchd", depth: 0, children: 2, app: "sys"),
            row("x", depth: 1, children: 1, app: "x").moved(depth: 1, parentID: "launchd"),
            row("safari", depth: 2, children: 1, app: "S").moved(depth: 2, parentID: "x"), row("helper", depth: 3, app: "h"),
            row("safari2", depth: 1, app: "S").moved(depth: 1, parentID: "launchd"), row("y", depth: 1, app: "y"),
        ]
        let (pinned, rest) = rows.pinning(appID: "S")
        #expect(pinned.map(\.id) == ["safari", "helper", "safari2"])
        #expect(pinned.map(\.depth) == [0, 1, 0])
        #expect(rest.map(\.id) == ["launchd", "x", "y"])
        #expect(rest.map(\.childCount) == [1, 0, 0])
    }

    @Test func pinningUnknownAppKeepsEverything() {
        let rows = [row("a", depth: 0, app: "A"), row("b", depth: 0, app: "B")]
        let (pinned, rest) = rows.pinning(appID: "Z")
        #expect(pinned.isEmpty)
        #expect(rest.map(\.id) == ["a", "b"])
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

    /// Per-process network is all-or-nothing: known for every process with the capability, never without it.
    @Test func processNetworkFollowsCapability() {
        let monitor = Monitor()
        _ = monitor.refresh()
        let rows = monitor.buildView(.init(mode: .flat, column: .networkReceive, descending: true, filter: ""))
        let known = Monitor.capabilities.contains(.processNetwork)
        #expect(!rows.isEmpty)
        for row in rows {
            #expect((row.networkReceive != nil) == known)
            #expect((row.networkSend != nil) == known)
        }
    }
}
