import Foundation

/// The running build: its version, where the code lives and where to get a newer one.
/// `scripts/build-macos-app.sh` writes the version (from the `VERSION` file), the build number and the git
/// commit into Info.plist; a bare `swift run` has no bundle and reports itself as a development build.
enum AppInfo {
    static let repository = URL(string: "https://github.com/manhpham90vn/Procyon")!
    static let releases = repository.appending(path: "releases")
    static let latestRelease = releases.appending(path: "latest")
    static let issues = repository.appending(path: "issues")

    static let version = plistString("CFBundleShortVersionString")
    static let build = plistString("CFBundleVersion")
    static let commit = plistString("ProcyonCommit")

    /// "0.1.0 (42)" for a packaged app, "Development build" otherwise.
    static var versionDescription: String {
        guard let version else { return "Development build" }
        guard let build, build != version else { return version }
        return "\(version) (\(build))"
    }

    private static func plistString(_ key: String) -> String? {
        guard let value = Bundle.main.object(forInfoDictionaryKey: key) as? String,
            !value.trimmingCharacters(in: .whitespaces).isEmpty
        else { return nil }
        return value
    }
}
