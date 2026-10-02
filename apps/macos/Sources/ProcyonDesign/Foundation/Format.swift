import Foundation

/// Display formatting shared by every component. Unknown values render as an em dash.
public enum Format {
    public static let unavailable = "—"

    public static func percent(_ fraction: Double?, digits: Int = 0) -> String {
        guard let fraction, fraction.isFinite else { return unavailable }
        return String(format: "%.\(digits)f%%", fraction * 100)
    }

    /// CPU percent where 100 = one full core.
    public static func cpu(_ percent: Double?) -> String {
        guard let percent, percent >= 0 else { return unavailable }
        if percent >= 100 { return String(format: "%.0f%%", percent) }
        return String(format: "%.1f%%", percent)
    }

    public static func bytes<T: BinaryInteger>(_ value: T?) -> String {
        guard let value, value >= 0 else { return unavailable }
        return bytes(Double(value))
    }

    public static func bytes(_ value: Double) -> String {
        let units = ["B", "KB", "MB", "GB", "TB", "PB"]
        var amount = value
        var unit = 0
        while amount >= 1024, unit < units.count - 1 {
            amount /= 1024
            unit += 1
        }
        if unit == 0 { return "\(Int(amount)) B" }
        return String(format: amount >= 100 ? "%.0f %@" : amount >= 10 ? "%.1f %@" : "%.2f %@", amount, units[unit])
    }

    /// Bytes per second.
    public static func rate(_ value: Double?) -> String {
        guard let value, value >= 0 else { return unavailable }
        if value < 1 { return "0 KB/s" }
        if value < 1024 { return "\(Int(value)) B/s" }
        return bytes(value) + "/s"
    }

    /// Bytes per second split into number and unit for large typographic layouts.
    public static func rateParts(_ value: Double?) -> (value: String, unit: String) {
        let text = rate(value)
        guard let space = text.lastIndex(of: " ") else { return (text, "") }
        return (String(text[..<space]), String(text[text.index(after: space)...]))
    }

    public static func bytesParts<T: BinaryInteger>(_ value: T?) -> (value: String, unit: String) {
        let text = bytes(value)
        guard let space = text.lastIndex(of: " ") else { return (text, "") }
        return (String(text[..<space]), String(text[text.index(after: space)...]))
    }

    public static func count(_ value: Int?) -> String {
        guard let value, value >= 0 else { return unavailable }
        return value.formatted(.number)
    }

    public static func duration(_ seconds: TimeInterval) -> String {
        let total = Int(max(0, seconds))
        let days = total / 86_400, hours = total % 86_400 / 3600, minutes = total % 3600 / 60
        if days > 0 { return "\(days)d \(hours)h \(minutes)m" }
        if hours > 0 { return "\(hours)h \(minutes)m" }
        return "\(minutes)m \(total % 60)s"
    }

    /// Celsius in, shown in the unit chosen in Settings (`TemperatureUnit`).
    public static func temperature(_ celsius: Double?, unit: TemperatureUnit = .current) -> String {
        guard let celsius, celsius.isFinite else { return unavailable }
        let measurement = Measurement(value: celsius, unit: UnitTemperature.celsius)
        let number = FloatingPointFormatStyle<Double>.number.precision(.fractionLength(0))
        switch unit {
        // The locale's unit: °F in the US, °C almost everywhere else.
        case .system:
            return measurement.formatted(.measurement(width: .abbreviated, usage: .weather, numberFormatStyle: number))
        case .celsius: return "\(celsius.formatted(number))°C"
        case .fahrenheit: return "\(measurement.converted(to: .fahrenheit).value.formatted(number))°F"
        }
    }

    /// Watts with one decimal.
    public static func watts(_ value: Double?) -> String {
        guard let value, value.isFinite else { return unavailable }
        return String(format: "%.1f W", value)
    }

    public static func interval(_ seconds: Double) -> String {
        seconds < 1 ? String(format: "%.1fs", seconds) : String(format: "%.0fs", seconds)
    }
}

/// How temperatures are shown; stored in the user defaults under `storageKey`.
public enum TemperatureUnit: String, CaseIterable, Identifiable, Sendable {
    /// Follows the region: °F in the US, °C elsewhere.
    case system, celsius, fahrenheit

    public static let storageKey = "temperatureUnit"

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .system: "Region default (\(Locale.current.measurementSystem == .us ? "°F" : "°C"))"
        case .celsius: "Celsius (°C)"
        case .fahrenheit: "Fahrenheit (°F)"
        }
    }

    public static var current: TemperatureUnit {
        UserDefaults.standard.string(forKey: storageKey).flatMap(TemperatureUnit.init(rawValue:)) ?? .system
    }
}
