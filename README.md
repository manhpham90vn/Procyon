# Procyon

A lightweight, cross-platform task manager. The product spec is in [`docs/procyon-spec.md`](docs/procyon-spec.md).
This repo holds the **P0 MVP for macOS**.

## Layout

```
core/                       C++20 core with a stable C ABI (shared by every UI)
  include/procyon/procyon.h   the API: snapshot, views (flat/grouped/tree), actions, capabilities
  src/monitor.cpp             rates and deltas, C ABI
  src/view.cpp                filter, sort, group-by-app, process tree (portable)
  src/platform/macos.cpp      macOS adapter (libproc, mach, sysctl, IOKit)
  src/helper_server.cpp       privileged helper (procyon-helper), src/helper_client.cpp its client
  helper/main.c               procyon-helper entry point
  tools/procyon_cli.cpp       headless prototype and overhead measurement
design/
  tokens.json               design tokens: single source of truth for every platform
  components.md             component contract that the Win32 and GTK UIs must follow
apps/macos/Sources/
  ProcyonKit                Swift bridge to the core + observable store (no UI)
  ProcyonDesign             generated tokens + reusable SwiftUI components
  ProcyonApp                screens: Overview, Processes, CPU, Memory, Disk, Network, System
scripts/
  gen-tokens.py             tokens.json → Tokens.generated.swift
  build-macos-app.sh        builds dist/Procyon.app
  make-icon.swift           renders the app icon
```

## Build and run (macOS 14+, Xcode 27)

```sh
make run          # build dist/Procyon.app and open it
make test         # unit tests
make core         # C++ core, procyon-cli and procyon-helper via CMake
make help         # everything else
```

Open a specific screen at launch: `dist/Procyon.app/Contents/MacOS/Procyon -initialPage processes`.

## Development

| Command | What it does |
| --- | --- |
| `make format` | `swift format` + `clang-format` in place (configs: `.swift-format`, `.clang-format`, `.editorconfig`) |
| `make lint` | Read-only: stale tokens, swift-format, clang-format, clang-tidy (`.clang-tidy`), shellcheck. `SKIP_TIDY=1` skips clang-tidy. |
| `make tokens` | Regenerate `Tokens.generated.swift` after editing `design/tokens.json` |
| `make ci` | lint + test + core + app, same as CI |

Tools: Xcode 27 provides `swift format`; `brew install clang-format llvm shellcheck` for the rest
(the scripts also find Homebrew's keg-only LLVM).

### CI/CD (GitHub Actions)

- **`ci.yml`** runs on every push to `main` and every PR: lint, then unit tests, a CMake core build with a
  `procyon-cli` smoke run, and the .app uploaded as an artifact.
- **`release.yml`** runs on tags `v*`, or manually: tests, universal (arm64 + x86_64) build, DMG + zip +
  SHA-256, GitHub release (versions with `-` are marked prerelease). With the secrets `MACOS_CERTIFICATE_P12`,
  `MACOS_CERTIFICATE_PASSWORD`, `APPLE_ID`, `APPLE_TEAM_ID` and `APPLE_APP_PASSWORD`, it signs with Developer ID
  and notarizes; without them the build is ad-hoc signed.

```sh
git tag v0.1.0 && git push origin v0.1.0
```

## P0 status (macOS)

| Spec item | Status |
| --- | --- |
| Process list: name, PID, user, CPU, RAM, disk I/O | Done. Network per process is hidden (`PC_CAP_PROCESS_NETWORK` is unsupported on macOS). |
| Optional columns with saved configuration | Done (table column customization, saved per window). |
| Flat / by app / tree views | Done in the core (`pc_monitor_build_view`). |
| Search, filter, sort by every column | Done. `⌘F` focuses search; sorting runs in the core. |
| End task / force quit / end process tree | Done. Confirmation for system processes and destructive actions; kernel, launchd and Procyon itself are protected. |
| Live charts: CPU total and per core, RAM/swap, disk, network | Done. Keeps the last 60 seconds. |
| System info: CPU, RAM, disks, OS, uptime | Done. |
| Light/dark following the system, update speed 0.5–5 s, pause | Done (Settings `⌘,`, View menu, sidebar). |

### Measured overhead (M3, about 590 processes, 1 s updates, release build)

- Core sampling: about 0.5% of one core (`procyon-cli`).
- Whole app with the window open: 1.4–2.5% on dashboard screens, about 5–6% on Processes
  (SwiftUI `Table` re-sorting rows each tick). Spec target: under 1–2%.

### Full access (privileged helper)

The app runs as a normal user. About 160 root-owned processes stay locked (`—` with a lock icon) until the
user clicks **Unlock Full Access** (Processes banner or Settings). The same `procyon-helper` binary and socket
protocol serve two modes:

**Developer ID builds (releases): background helper, approved once.**

1. `HelperDaemon` registers `Contents/Library/LaunchDaemons/dev.procyon.helper.plist` with
   `SMAppService.daemon`. macOS asks the user to allow it in System Settings → General → Login Items; the app
   connects as soon as they do, and on every later launch without asking.
2. launchd owns `/var/run/dev.procyon.helper.sock` and starts `procyon-helper --daemon` on the first
   connection. The helper exits after a minute without clients.
3. Each client must be `dev.procyon.app` signed by the helper's own team (checked via its audit token) and run
   by an administrator. **Remove Helper** in Settings unregisters it.

**Ad-hoc builds (development): password prompt per launch.** The plist is only embedded when
`SIGN_IDENTITY` is a real identity, because `SMAppService` needs one.

1. `HelperLauncher` shows the macOS password prompt (`osascript … with administrator privileges`) and starts
   `Contents/Helpers/procyon-helper` as root.
2. The helper creates a 0600 Unix socket owned by the user and accepts only the launching app
   (checks peer uid and `LOCAL_PEERPID`).
3. The helper exits when the app quits, disconnects, or nobody connects within 30 seconds.

In both modes the core (`pc_monitor_attach_helper`) only talks to a root-owned peer, asks it for restricted
processes on each refresh, and falls back to it when End Task is denied. The helper can only read counters and
send signals, and refuses pids 0 and 1 and its client.

Test without the UI: `sudo PROCYON_HELPER=$PWD/build/core/procyon-helper build/core/procyon-cli 2`.

### Known limits
- English UI only so far; Vietnamese is planned for v1.
