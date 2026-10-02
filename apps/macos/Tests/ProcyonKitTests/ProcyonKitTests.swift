import Foundation
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

@Suite struct ProcessControlModelTests {
    @Test func priorityStepsRoundToTheNearestNice() {
        #expect(ProcessPriority(nice: 0) == .normal)
        #expect(ProcessPriority(nice: -20) == .high)
        #expect(ProcessPriority(nice: 7) == .belowNormal)
        #expect(ProcessPriority(nice: 19) == .lowest)
        #expect(ProcessPriority.high.needsPrivileges && !ProcessPriority.low.needsPrivileges)
    }

    @Test func commandLineQuotesArgumentsWithSpaces() {
        var details = ProcessDetails()
        details.arguments = ["/bin/echo", "hello world", ""]
        #expect(details.commandLine == "/bin/echo \"hello world\" \"\"")
        details.arguments = nil
        #expect(details.commandLine == nil)
    }

    @Test func startupImpactFollowsTheRunningCopy() {
        #expect(StartupImpact(memory: 1 << 30, cpu: 50, isRunning: false) == .notRunning)
        #expect(StartupImpact(memory: 10 << 20, cpu: 0.5, isRunning: true) == .low)
        #expect(StartupImpact(memory: 120 << 20, cpu: 0, isRunning: true) == .medium)
        #expect(StartupImpact(memory: 10 << 20, cpu: 40, isRunning: true) == .high)
    }
}

/// Real actions, only ever on a child process the test starts itself.
@Suite(.serialized) struct ProcessActionTests {
    private func sleeper() throws -> Process {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/sleep")
        process.arguments = ["30"]
        try process.run()
        return process
    }

    private func row(_ monitor: Monitor, pid: Int32) -> ProcessRow? {
        _ = monitor.refresh()
        return monitor.buildView(.init(mode: .flat, column: .pid, descending: false, filter: "\(pid)"))
            .first { $0.pid == pid }
    }

    @Test func suspendResumeAndLowerPriority() throws {
        let child = try sleeper()
        defer { child.terminate() }
        let monitor = Monitor()
        let pid = child.processIdentifier

        #expect(monitor.suspend(pid: pid) == .ok)
        #expect(row(monitor, pid: pid)?.state == .stopped)
        #expect(monitor.resume(pid: pid) == .ok)
        #expect(row(monitor, pid: pid)?.state != .stopped)

        #expect(monitor.setPriority(pid: pid, nice: 10) == .ok)
        #expect(row(monitor, pid: pid)?.nice == 10)
    }

    @Test func signalEndsTheChild() throws {
        let child = try sleeper()
        let monitor = Monitor()
        _ = monitor.refresh()
        #expect(monitor.signal(pid: child.processIdentifier, ProcessSignal.terminate.rawValue) == .ok)
        child.waitUntilExit()
        #expect(child.terminationReason == .uncaughtSignal)
        #expect(monitor.signal(pid: 1, ProcessSignal.terminate.rawValue) == .protected)
    }

    @Test func detailsOfOwnProcess() throws {
        let monitor = Monitor()
        _ = monitor.refresh()
        let details = try #require(monitor.details(pid: getpid()))
        #expect(details.arguments?.isEmpty == false)
        #expect(details.environment?.isEmpty == false)
        #expect(details.threads?.isEmpty == false)
        #expect(!details.workingDirectory.isEmpty)
    }
}

@Suite struct P1MonitorTests {
    @Test func processGPUFollowsCapability() {
        let monitor = Monitor()
        _ = monitor.refresh()
        let rows = monitor.buildView(.init(mode: .flat, column: .gpu, descending: true, filter: ""))
        let known = Monitor.capabilities.contains(.processGPU)
        for row in rows { #expect((row.gpu != nil) == known) }
        // Sorted by GPU, descending.
        let values = rows.compactMap(\.gpu)
        #expect(values == values.sorted(by: >))
    }

    @Test func gpusFollowCapability() {
        let sample = Monitor().refresh()
        #expect(sample.gpus.isEmpty == !Monitor.capabilities.contains(.gpu))
        #expect((sample.cpuTemperature != nil) == Monitor.capabilities.contains(.temperature))
    }

    @Test func servicesIncludeRunningJobs() {
        guard Monitor.capabilities.contains(.services) else { return }
        let services = Monitor().services()
        #expect(!services.isEmpty)
        #expect(services.contains { $0.isRunning })
        #expect(Set(services.map(\.id)).count == services.count)
    }

    @Test func batteryMatchesCapability() {
        #expect((Monitor.battery() != nil) == Monitor.capabilities.contains(.battery))
        if let battery = Monitor.battery() { #expect((0...1).contains(battery.level)) }
    }

    @Test func processSamplingCanBeTurnedOff() {
        let monitor = Monitor()
        monitor.setProcessSampling(false)
        let sample = monitor.refresh()
        #expect(sample.processCount == 0)
        #expect(sample.memoryTotal > 0)
        monitor.setProcessSampling(true)
        #expect(monitor.refresh().processCount > 0)
    }
}

@Suite struct CoreKindTests {
    /// Hybrid chips report every core's kind, matching the performance/efficiency counts.
    @Test func coreKindsMatchCoreCounts() {
        let info = Monitor.systemInfo()
        guard Monitor.capabilities.contains(.hybridCores) else {
            #expect(info.coreKinds.allSatisfy { $0 == .unknown })
            return
        }
        #expect(info.coreKinds.count == info.logicalCores)
        #expect(info.coreKinds.filter { $0 == .performance }.count == info.performanceCores)
        #expect(info.coreKinds.filter { $0 == .efficiency }.count == info.efficiencyCores)
    }
}
