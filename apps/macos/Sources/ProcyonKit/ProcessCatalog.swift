import Foundation

/// What a common process is, in plain words, and whether ending it is safe.
public struct ProcessExplanation: Sendable, Hashable {
    public enum Advice: Sendable, Hashable {
        /// Part of the OS: ending it breaks features or logs the user out.
        case keep
        /// Safe to end: the OS starts it again.
        case restarts
        /// An app or a helper of one: safe to quit once work is saved.
        case quit

        public var title: String {
            switch self {
            case .keep: "Don't end it"
            case .restarts: "Safe to end, macOS restarts it"
            case .quit: "Safe to quit, save your work first"
            }
        }
    }

    public let advice: Advice
    public let summary: String
}

/// Explanations of common processes, from data/process-catalog.json (see scripts/gen-catalog.py).
public enum ProcessCatalog {
    /// The explanation of a process by its name, or of the app it belongs to.
    public static func explain(name: String, appName: String = "") -> ProcessExplanation? {
        entries[name] ?? (appName.isEmpty ? nil : entries[appName])
    }
}
