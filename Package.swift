// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "Procyon",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "Procyon", targets: ["ProcyonApp"]),
        .executable(name: "procyon-helper", targets: ["ProcyonHelper"]),
        .library(name: "ProcyonDesign", targets: ["ProcyonDesign"]),
    ],
    targets: [
        // Shared C++ core with a C ABI (also built by core/CMakeLists.txt for other platforms).
        .target(
            name: "ProcyonCore",
            path: "core",
            exclude: ["CMakeLists.txt", "tools", "helper"],
            sources: ["src"],
            publicHeadersPath: "include",
            cxxSettings: [.headerSearchPath("src")],
            linkerSettings: [
                .linkedFramework("CoreFoundation"), .linkedFramework("IOKit"),
                // helper: client code-signature checks and audit tokens
                .linkedFramework("Security"), .linkedLibrary("bsm"),
            ]
        ),
        // Privileged helper (runs as root: a LaunchDaemon in signed builds, else launched by the app).
        .executableTarget(
            name: "ProcyonHelper",
            dependencies: ["ProcyonCore"],
            path: "core/helper"
        ),
        // Swift-facing models and the observable store; no UI.
        .target(
            name: "ProcyonKit",
            dependencies: ["ProcyonCore"],
            path: "apps/macos/Sources/ProcyonKit"
        ),
        // Design tokens (generated from design/tokens.json) and reusable components.
        .target(
            name: "ProcyonDesign",
            path: "apps/macos/Sources/ProcyonDesign"
        ),
        .executableTarget(
            name: "ProcyonApp",
            dependencies: ["ProcyonKit", "ProcyonDesign"],
            path: "apps/macos/Sources/ProcyonApp"
        ),
        .testTarget(
            name: "ProcyonKitTests",
            dependencies: ["ProcyonKit"],
            path: "apps/macos/Tests/ProcyonKitTests"
        ),
    ],
    cxxLanguageStandard: .cxx20
)
