# Developing Procyon

Notes for contributors: repo layout, build commands, CI/CD, feature status against the spec and how the privileged helper works. For installing and using the app, see the [README](../README.md).

## Layout

```
core/                       C++20 core with a stable C ABI (shared by every UI)
  include/procyon/procyon.h   the API: snapshot, views (flat/grouped/tree), actions, capabilities
  src/monitor.cpp             rates and deltas, C ABI
  src/view.cpp                filter, sort, group-by-app, process tree (portable)
  src/platform/macos.cpp      macOS adapter (libproc, mach, sysctl, IOKit)
  src/platform/macos_netstat.cpp  per-process network via NetworkStatistics (dlopen'd)
  src/platform/macos_gpu.cpp      GPU usage and per-process GPU time (IOAccelerator)
  src/platform/macos_power.cpp    battery, sleep assertions, temperature sensors (IOHID, dlsym'd)
  src/platform/macos_launchd.cpp  services and startup items (launchd jobs via /bin/launchctl)
  src/platform/macos_handles.cpp  open files and TCP/UDP sockets per process (libproc, like lsof)
  src/helper_server.cpp       privileged helper (procyon-helper), src/helper_client.cpp its client
  helper/main.c               procyon-helper entry point
  tools/procyon_cli.cpp       headless prototype and overhead measurement
  tests/core_tests.cpp        helper wire format, validation and refusal checks (ctest)
design/
  tokens.json               design tokens: single source of truth for every platform
  components.md             component contract that the Win32 and GTK UIs must follow
apps/macos/Sources/
  ProcyonKit                Swift bridge to the core + observable store (no UI)
  ProcyonDesign             generated tokens + reusable SwiftUI components
  ProcyonApp                screens: Overview, Processes, CPU, Memory, Disk, Network, GPU, Energy,
                            Startup, Services, History, Files & Ports, Battery, System; ⌘K palette;
                            menu bar widget
data/
  process-catalog.json      plain-language explanations of common processes
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
| `make screenshots` | Retake the README screenshots, light and dark, into `docs/screenshots`. Any other screen: `swift scripts/screenshots.swift --pages <page> --out <dir>` (`--help` for options). Needs Screen Recording permission for the terminal. |
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
  and notarizes; without them the build is ad-hoc signed. With `HOMEBREW_TAP_TOKEN` (contents write access to
  [manhpham90vn/homebrew-tap](https://github.com/manhpham90vn/homebrew-tap)), stable releases also update the `procyon`
  cask there through `scripts/publish-homebrew.sh`, rendered from `packaging/homebrew/procyon.rb`.

The `VERSION` file is the single source of the version: `scripts/build-macos-app.sh` writes it (with the build
number and git commit) into Info.plist, and the app shows it in Settings → About. The release job refuses a tag
that doesn't match it.

```sh
echo 0.1.0 > VERSION
git commit -am "release: 0.1.0"
git tag v0.1.0 && git push origin main v0.1.0
```

## P0 status (macOS)

| Spec item | Status |
| --- | --- |
| Process list: name, PID, user, CPU, RAM, disk I/O, network | Done. Per-process network comes from the private NetworkStatistics framework (what `nettop` uses), loaded at runtime; if it is missing, `PC_CAP_PROCESS_NETWORK` is off and the columns are hidden. |
| Optional columns with saved configuration | Done (table column customization, saved per window). |
| Flat / by app / tree views | Done in the core (`pc_monitor_build_view`). |
| Search, filter, sort by every column | Done. `⌘F` focuses search; sorting runs in the core. |
| End task / force quit / end process tree | Done. Confirmation for system processes and destructive actions; kernel, launchd and Procyon itself are protected. |
| Live charts: CPU total and per core, RAM/swap, disk, network | Done. Keeps the last 60 seconds. |
| System info: CPU, RAM, disks, OS, uptime | Done. |
| Light/dark following the system, update speed 0.5–5 s, pause | Done (Settings page in the sidebar or `⌘,`, View menu; pause also at the sidebar bottom). |

## P1 status (macOS)

| Spec item | Status |
| --- | --- |
| GPU: usage, VRAM, temperature, encode/decode; per-process GPU | Done: device, renderer and tiler usage, memory in use, GPU column and top apps (from each process's accumulated GPU time). Apple Silicon has no separate GPU sensor, so the GPU page shows the chip temperature labelled as shared with the CPU. Encode/decode usage isn't published by macOS and stays hidden. |
| Priority (nice), CPU affinity, suspend/resume, signals | Done except affinity: macOS has no CPU affinity (`PC_CAP_CPU_AFFINITY` off, nothing shown). Raising priority and acting on other users' processes go through the helper. |
| Process details: path, command line, environment, running time, threads | Done (**Get Info**, `⌘I`): show in Finder, copy info. Other users' processes are read through the helper. |
| Startup apps with impact, enable/disable | Done: what System Settings → Login Items lists. Launch agents and daemons outside `/System` (switchable here, enabled state from launchd's overrides), plus, with full access, apps that open at login and apps' background items from the Background Task Management database. The helper reads it (`sfltool dumpbtm` asks a normal user for an administrator password) and caches it, refreshing in the background (about 3 s per read). macOS has no API to switch those for another app, so their switch opens System Settings. Impact comes from the running copy's CPU and memory. |
| Services: start/stop/restart | Done: every launchd job in the system and user domains, plus enable/disable. System-domain changes need full access; SIP-protected Apple services are reported as such. |
| Command palette | Done (`⌘K`): screens, commands, and per-app actions (end, force quit, suspend, priority, Get Info, open location). |
| Tray / menu bar widget, selectable modules | Done: closing the window moves Procyon to the menu bar (out of the Dock); opening the window hides the menu bar item again. Modules: CPU, memory, network, GPU, temperature, battery (Settings → Menu bar). With the window closed, sampling skips per-process data. |
| Battery: level, health, apps preventing sleep | Done: charge, time remaining, power draw, maximum capacity, cycles, temperature, and sleep assertions attributed to the app they were taken for. |

## P2 status (macOS)

| Spec item | Status |
| --- | --- |
| History: every resource per app, look back at peaks | Done for 24 hours (the spec's lower bound; longer retention is open): one row a minute for the machine (CPU average and peak, memory, disk, network, GPU, temperature, app power) and one per busy app (the union of each tick's top-10 lists), in SQLite under `~/Library/Application Support/Procyon`, older minutes deleted as new ones arrive (a few MB). **History** charts any metric over 1, 6 or 24 hours; clicking the chart or a peak lists the busiest apps of that minute. Per-app rows are only written while processes are sampled (window open, or an app alert on). Recording can be switched off or cleared. |
| Energy tab like Activity Monitor | Done: the **Power** column in Processes and the **Energy** screen (apps now, energy per app since launch, total app power chart) from `ri_energy_nj` (`proc_pid_rusage` v6, no root): measured on Apple Silicon, modelled on Intel. Activity Monitor's 8-hour average comes from History once there is per-app energy for that long. |
| Deep analysis: files held, ports, connections | Done: **Files & Ports** lists listening ports (and whether they're reachable from the network), connections and open files, with **Who Is Using…** for a file, folder or disk; Get Info has Files and Network tabs. Other users' processes go through the helper. Closing another process's connection isn't possible on macOS (no public API), so it isn't offered. |
| Sensors: temperatures, fans | CPU, SSD and battery temperatures since P1. Fan speeds: not yet (SMC). |
| Alerts with custom thresholds | Done (Settings → Alerts): machine CPU, memory, memory pressure, CPU temperature, one app's CPU or memory, each over a threshold for a chosen time, as notifications; 15 minutes of quiet per alert. Per-app alerts keep process sampling on in the menu bar. |
| Explanations: "what is this process, can I end it" | Done for about 110 common macOS process names (63 explanations) (`data/process-catalog.json` → `scripts/gen-catalog.py` → Swift; `make lint` checks it is up to date). Shown in Get Info and in the End Task / Force Quit confirmation. |
| Plugins | Not started. |

### Measured overhead (M3, about 590 processes, 1 s updates, release build)

- Core sampling: about 0.5% of one core before P1, 0.6% with GPU and temperature sampling (`procyon-cli`).
- Whole app with the window open: 1.4–2.5% on dashboard screens, about 5–6% on Processes
  (SwiftUI `Table` re-sorting rows each tick). Spec target: under 1–2%.

### Full access (privileged helper)

The app runs as a normal user. About 160 root-owned processes stay locked (`—` with a lock icon) until the
user clicks **Unlock Full Access** (the banner on Processes, Startup or Services, or the ⌘K palette). The same `procyon-helper` binary and socket
protocol serve two modes:

**Developer ID builds (releases): background helper, approved once.**

1. `HelperDaemon` registers `Contents/Library/LaunchDaemons/dev.procyon.helper.plist` with
   `SMAppService.daemon`. macOS asks the user to allow it in System Settings → General → Login Items; the app
   connects as soon as they do, and on every later launch without asking.
2. launchd owns `/var/run/dev.procyon.helper.sock` and starts `procyon-helper --daemon` on the first
   connection. The helper exits after a minute without clients.
3. Each client must be `dev.procyon.app` signed by the helper's own team (checked via its audit token) and run
   by an administrator. **Settings → Full access** (and the ⌘K palette) can turn full access off for the session or
   **Remove Administrator Helper**, which unregisters it.

**Ad-hoc builds (development): password prompt per launch.** The plist is only embedded when
`SIGN_IDENTITY` is a real identity, because `SMAppService` needs one.

1. `HelperLauncher` shows the macOS password prompt (`osascript … with administrator privileges`) and starts
   `Contents/Helpers/procyon-helper` as root.
2. The helper creates a 0600 Unix socket owned by the user and accepts only the launching app
   (checks peer uid and `LOCAL_PEERPID`).
3. The helper exits when the app quits, disconnects, or nobody connects within 30 seconds.

In both modes the core (`pc_monitor_attach_helper`) only talks to a root-owned peer, asks it for restricted
processes on each refresh, and falls back to it when End Task is denied. The helper can only read counters and
process details, list the OS-managed startup items, send signals, change priority and run fixed `launchctl` verbs on
validated labels in the system domain; it refuses pids 0 and 1 and its client.

Test without the UI: `sudo PROCYON_HELPER=$PWD/build/core/procyon-helper build/core/procyon-cli 2`.

**Threat model.** In daemon mode, any administrator who runs the signed Procyon and approved it once in Login Items
gets, without a password, root-level signal/suspend/renice on every pid above 1, `launchctl kickstart/kill/enable/disable`
on any system-domain label (including Apple daemons not under SIP) and read access to every process's command line
and environment. That is about the power of a restricted passwordless `sudo`, gated by the code-signature and
admin-group checks; it is acceptable because an administrator can `sudo` anyway, but it is why the helper refuses to
run unsigned and why every new helper verb must stay a fixed, validated action rather than a general command. The
protected-process policy (`PC_PROC_PROTECTED`: kernel, launchd, Procyon itself) is enforced in the core on the client
side; the helper only refuses pids 0, 1, itself and its client. Private frameworks (`NetworkStatistics`, IOAccelerator
registry keys, `sfltool dumpbtm`) are probed at runtime and reported through capability flags; when one disappears in
a macOS release the feature hides instead of showing wrong numbers.

### Known limits
- English only: the UI and every text in it, by design.
