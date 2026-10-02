import Foundation

/// Fixed time window of samples (the spec keeps the last 60 seconds).
public struct MetricHistory: Sendable, Hashable {
    public struct Point: Sendable, Hashable {
        public let time: TimeInterval
        public let value: Double
    }

    public let window: TimeInterval
    public private(set) var points: [Point] = []

    public init(window: TimeInterval = 60) {
        self.window = window
    }

    public var latest: Double { points.last?.value ?? 0 }
    public var peak: Double { points.map(\.value).max() ?? 0 }

    public mutating func append(_ value: Double, at time: TimeInterval) {
        points.append(Point(time: time, value: value))
        // Keep one point beyond the window so lines reach the left edge.
        let cutoff = time - window
        if let firstInside = points.firstIndex(where: { $0.time >= cutoff }), firstInside > 1 {
            points.removeFirst(firstInside - 1)
        }
    }
}

/// Every chart series the UI shows.
public struct HistoryBank: Sendable, Hashable {
    public var cpu = MetricHistory()
    public var cpuSystem = MetricHistory()
    public var cores: [MetricHistory] = []
    public var memory = MetricHistory()
    public var swap = MetricHistory()
    public var diskRead = MetricHistory()
    public var diskWrite = MetricHistory()
    public var networkReceive = MetricHistory()
    public var networkSend = MetricHistory()
    /// Busiest GPU, 0...1.
    public var gpu = MetricHistory()
    public var gpuMemory = MetricHistory()
    public var cpuTemperature = MetricHistory()
    /// Hottest GPU sensor, Celsius; only GPUs with their own sensor (not Apple Silicon).
    public var gpuTemperature = MetricHistory()
    /// Internal SSD, Celsius.
    public var diskTemperature = MetricHistory()
    /// Watts drawn by all apps together.
    public var appPower = MetricHistory()

    public init() {}

    mutating func record(_ sample: SystemSample) {
        let t = sample.timestamp
        cpu.append(sample.cpuUsage, at: t)
        cpuSystem.append(sample.cpuSystem, at: t)
        if cores.count != sample.coreUsage.count {
            cores = Array(repeating: MetricHistory(), count: sample.coreUsage.count)
        }
        for (index, usage) in sample.coreUsage.enumerated() {
            cores[index].append(usage, at: t)
        }
        memory.append(sample.memoryFraction, at: t)
        swap.append(Double(sample.swapUsed), at: t)
        diskRead.append(sample.diskReadRate, at: t)
        diskWrite.append(sample.diskWriteRate, at: t)
        networkReceive.append(sample.networkReceiveRate, at: t)
        networkSend.append(sample.networkSendRate, at: t)
        if !sample.gpus.isEmpty {
            gpu.append(sample.gpuUsage ?? 0, at: t)
            gpuMemory.append(Double(sample.gpus.compactMap(\.memoryUsed).reduce(0, +)), at: t)
        }
        if let temperature = sample.cpuTemperature { cpuTemperature.append(temperature, at: t) }
        if let temperature = sample.gpus.compactMap(\.temperature).max() { gpuTemperature.append(temperature, at: t) }
        if let temperature = sample.diskTemperature { diskTemperature.append(temperature, at: t) }
        if let power = sample.appPower { appPower.append(power, at: t) }
    }
}
