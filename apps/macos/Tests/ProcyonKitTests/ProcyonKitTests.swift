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

@Suite struct AlertEvaluatorTests {
    private func settings(_ kind: AlertRule.Kind, threshold: Double? = nil, duration: TimeInterval = 30) -> AlertSettings {
        AlertSettings(rules: [AlertRule(kind: kind, isEnabled: true, threshold: threshold, duration: duration)])
    }

    private func sample(cpu: Double) -> SystemSample {
        var sample = SystemSample()
        sample.cpuUsage = cpu
        return sample
    }

    private func app(_ id: String, cpu: Double) -> ProcessRow {
        ProcessRow(
            id: "g:\(id)", kind: .group, pid: 1, parentID: nil, depth: 0, childCount: 0, processCount: 1, name: id,
            user: "", path: "", appID: id, appName: id, flags: [], cpu: cpu, memory: 0, diskRead: nil, diskWrite: nil,
            networkReceive: nil, networkSend: nil, threads: nil, startTime: nil, memberPIDs: [1])
    }

    @Test func firesOnlyAfterTheConditionHoldsForItsDuration() {
        var evaluator = AlertEvaluator()
        let rules = settings(.cpu, threshold: 90, duration: 30)
        #expect(evaluator.evaluate(sample(cpu: 0.95), apps: [], settings: rules, now: 0).isEmpty)
        #expect(evaluator.evaluate(sample(cpu: 0.95), apps: [], settings: rules, now: 20).isEmpty)
        #expect(evaluator.evaluate(sample(cpu: 0.95), apps: [], settings: rules, now: 30).count == 1)
    }

    @Test func dropBelowThresholdRestartsTheClock() {
        var evaluator = AlertEvaluator()
        let rules = settings(.cpu, threshold: 90, duration: 30)
        _ = evaluator.evaluate(sample(cpu: 0.95), apps: [], settings: rules, now: 0)
        _ = evaluator.evaluate(sample(cpu: 0.10), apps: [], settings: rules, now: 20)
        #expect(evaluator.evaluate(sample(cpu: 0.95), apps: [], settings: rules, now: 40).isEmpty)
        #expect(evaluator.evaluate(sample(cpu: 0.95), apps: [], settings: rules, now: 70).count == 1)
    }

    @Test func staysQuietDuringTheCooldown() {
        var evaluator = AlertEvaluator()
        evaluator.cooldown = 600
        let rules = settings(.cpu, threshold: 90, duration: 0)
        #expect(evaluator.evaluate(sample(cpu: 0.95), apps: [], settings: rules, now: 0).count == 1)
        #expect(evaluator.evaluate(sample(cpu: 0.95), apps: [], settings: rules, now: 300).isEmpty)
        #expect(evaluator.evaluate(sample(cpu: 0.95), apps: [], settings: rules, now: 600).count == 1)
    }

    @Test func perAppRulesTrackEachAppSeparately() {
        var evaluator = AlertEvaluator()
        let rules = settings(.appCPU, threshold: 100, duration: 10)
        _ = evaluator.evaluate(SystemSample(), apps: [app("a", cpu: 150)], settings: rules, now: 0)
        let events = evaluator.evaluate(
            SystemSample(), apps: [app("a", cpu: 150), app("b", cpu: 150)], settings: rules, now: 10)
        #expect(events.map(\.appID) == ["a"])
    }

    @Test func disabledRulesNeverFire() {
        var evaluator = AlertEvaluator()
        let rules = AlertSettings(rules: [AlertRule(kind: .cpu, isEnabled: false, threshold: 10, duration: 0)])
        #expect(evaluator.evaluate(sample(cpu: 1), apps: [], settings: rules, now: 0).isEmpty)
    }
}

@Suite struct HistoryTests {
    private func sample(at time: TimeInterval, cpu: Double) -> SystemSample {
        var sample = SystemSample()
        sample.timestamp = time
        sample.cpuUsage = cpu
        sample.memoryTotal = 100
        sample.memoryUsed = 50
        return sample
    }

    private func app(_ id: String, cpu: Double) -> ProcessRow {
        ProcessRow(
            id: "g:\(id)", kind: .group, pid: 1, parentID: nil, depth: 0, childCount: 0, processCount: 1, name: id,
            user: "", path: "", appID: id, appName: id, flags: [], cpu: cpu, memory: 1000, diskRead: nil, diskWrite: nil,
            networkReceive: nil, networkSend: nil, threads: nil, startTime: nil, memberPIDs: [1])
    }

    @Test func recorderAveragesAMinuteAndCountsMissingAppsAsZero() {
        var recorder = HistoryRecorder()
        #expect(recorder.add(sample(at: 600, cpu: 0.2), busiest: [app("a", cpu: 40)]) == nil)
        #expect(recorder.add(sample(at: 630, cpu: 0.4), busiest: [app("b", cpu: 20)]) == nil)
        let record = recorder.add(sample(at: 660, cpu: 0.9), busiest: [])
        #expect(record?.machine.minute == 600)
        #expect(abs((record?.machine.cpu ?? 0) - 0.3) < 1e-9)
        #expect(record?.machine.cpuPeak == 0.4)
        #expect(record?.machine.memory == 0.5)
        let apps = Dictionary(uniqueKeysWithValues: (record?.apps ?? []).map { ($0.appID, $0.cpu) })
        #expect(apps == ["a": 20, "b": 10])
    }

    @Test func databaseKeepsMinutesAndRanksApps() async {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("procyon-history-\(UUID()).sqlite")
        defer { try? FileManager.default.removeItem(at: url) }
        let database = HistoryDatabase(url: url)
        for minute in 0..<3 {
            var machine = MachineMinute(minute: 6000 + minute * 60)
            machine.cpu = Double(minute) / 10
            var a = AppUsage(appID: "a", name: "A")
            a.cpu = 10
            var b = AppUsage(appID: "b", name: "B")
            b.cpu = minute == 2 ? 90 : 0
            await database.write(MinuteRecord(machine: machine, apps: [a, b]))
        }
        let minutes = await database.machine(since: 6060)
        #expect(minutes.map(\.minute) == [6060, 6120])
        #expect(minutes.last?.cpu == 0.2)
        let top = await database.apps(from: 6000, to: 6180, orderBy: .cpu)
        #expect(top.map(\.appID) == ["b", "a"])
        #expect(top.first?.cpu == 30)
        await database.clear()
        #expect(await database.machine(since: 0).isEmpty)
    }

    @Test func databaseDropsMinutesOlderThanTheRetention() async {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("procyon-history-\(UUID()).sqlite")
        defer { try? FileManager.default.removeItem(at: url) }
        let database = HistoryDatabase(url: url)
        await database.write(MinuteRecord(machine: MachineMinute(minute: 0), apps: []))
        await database.write(MinuteRecord(machine: MachineMinute(minute: Int(HistoryDatabase.retention) + 60), apps: []))
        #expect(await database.machine(since: 0).map(\.minute) == [Int(HistoryDatabase.retention) + 60])
    }
}

@Suite struct ProcessCatalogTests {
    @Test func knownProcessesAreExplained() {
        #expect(ProcessCatalog.explain(name: "kernel_task")?.advice == .keep)
        #expect(ProcessCatalog.explain(name: "mdworker_shared")?.advice == .restarts)
        #expect(ProcessCatalog.explain(name: "unknown-thing") == nil)
        // A helper falls back to its app's entry.
        #expect(ProcessCatalog.explain(name: "helper", appName: "Docker Desktop") != nil)
    }
}
