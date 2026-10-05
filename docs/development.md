Core counts aren't published. Temperature: NVIDIA through NVML (`nvml.dll`, installed by the driver, answers any user, matched to the DXGI adapter by product name); AMD (ADLX) and Intel (IGCL) not yet. |# Developing Procyon

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
  src/platform/windows.cpp        Windows adapter (NtQuerySystemInformation, PDH-free counters, PEB reads)
  src/platform/windows_services.cpp  services (SCM) and startup items (Run keys, Startup folders, StartupApproved)
  src/platform/windows_handles.cpp   open files (system handle table) and TCP/UDP sockets (IP helper)
  src/platform/windows_gpu.cpp       GPUs (DXGI) and usage per adapter and process ("GPU Engine" counters)
  src/platform/windows_power.cpp     battery (battery class driver), ACPI and drive temperatures
  src/platform/windows_internal.hpp  shared native declarations and helpers for the Windows files
  src/helper_server.cpp       privileged helper (procyon-helper), src/helper_client.cpp its client
                              (a stub on Windows: the app relaunches itself elevated instead)
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
apps/ui/                    the shared UI of the non-Apple apps (C++20, no platform headers)
  CMakeLists.txt            procyon_ui static library, version.h
  src/ui.hpp                geometry, Color, Theme, Path, the Canvas interface a backend implements,
                            the Renderer (components of design/components.md over a Canvas), input
  src/render.cpp            the components: cards with real shadows, continuous (squircle) corners,
                            smooth sparklines, ring gauges, badges, icons
  src/icons.*, icons_data.hpp  Lucide icons (ISC) parsed from SVG into paths
  src/platform.hpp          the OS services the UI needs (dark mode, data folder, settings, clipboard)
  src/store.*               sampler thread around pc_monitor, snapshot copies, 60 s histories
  src/widgets.*             search field, scrolling, virtual table
  src/pages.hpp, page_*.cpp screens: Overview, Processes, CPU, Memory, Disk, Network, GPU, Battery,
                            Startup, Services, History, Files & Ports, System, Settings
  src/overlays.*            Ctrl+K palette and Get Info
  src/history.*, alerts.*   the 24-hour history file and alert rules
  src/commands.hpp          command ids shared with the Windows accelerator table
  src/Tokens.generated.h    generated from design/tokens.json
apps/windows/
  CMakeLists.txt            Procyon.exe (Win32 + Direct2D/DirectWrite, no other dependencies)
  src/canvas_d2d.*          the Direct2D/DirectWrite Canvas on a DirectComposition swap chain: paths,
                            gradients, Shadow effect, images, text
  src/platform_windows.cpp  platform.hpp for Windows (registry, %LOCALAPPDATA%, clipboard, COM, shell icons)
  src/window.cpp            main window: custom caption, sidebar, routing, tray icon, dialogs,
                            elevation, settings
  res/                      icon (from the shared PNG), manifest, accelerators, version resource
design/fonts/               InterVariable.ttf and NunitoVariable.ttf (SIL OFL 1.1), embedded as RCDATA
design/icons/               the Lucide license
data/
  process-catalog.json      plain-language explanations of common processes
scripts/
  gen-tokens.py             tokens.json → Tokens.generated.swift and apps/ui's Tokens.generated.h
  build-macos-app.sh        builds dist/Procyon.app
  build-windows.cmd         builds build\windows (core, CLI, tests, app) and dist\windows\Procyon.exe
  make-icon.swift           renders the app icon; make-icon-windows.ps1 converts it to Procyon.ico
```

## Build and run (macOS 14+, Xcode 27)

```sh
make run          # build dist/Procyon.app and open it
make test         # unit tests
make core         # C++ core, procyon-cli and procyon-helper via CMake
make help         # everything else
```

Open a specific screen at launch: `dist/Procyon.app/Contents/MacOS/Procyon -initialPage processes`.

## Build and run (Windows 10 1809+ / 11, Visual Studio 2022)

The Makefile has the same targets on both platforms; on Windows it runs its recipes through `cmd.exe`, so
`make` works from cmd, PowerShell and Git Bash alike. GNU Make comes from `winget install ezwinports.make`.

```bat
make tools        rem once: CMake, Ninja, clang-format (and python when the machine has none) into build\tools
make run          rem release build, dist\windows\Procyon.exe, open it
make test         rem release build + core tests (make core is the same)
make build        rem debug build into build\windows-debug
make bench        rem measure every screen and the tray state against the spec's targets (scripts\bench-windows.py)
make lint         rem clang-format --dry-run, generated files and python checks (the last two need python)
make format       rem clang-format in place
make ci           rem lint + test + app, what the Windows CI job runs
make help         rem everything else
```

Underneath, `scripts\build-windows.cmd [release|debug] [--no-tests]` finds Visual Studio through `vswhere`, runs
`vcvars64.bat`, puts `build\tools` first on PATH, then falls back to the CMake and Ninja of the *C++ CMake tools*
component or PATH. Everything builds from `core/CMakeLists.txt`, which adds `apps/windows` on Windows. Headless:
`build\windows\procyon-cli.exe 3 1000`; a screen at launch: `dist\windows\Procyon.exe --page processes`.
`scripts\lint-windows.ps1` and `scripts\format-windows.ps1` are the Windows counterparts of `lint.sh`/`format.sh`
(no Swift, no clang-tidy, no shellcheck); `scripts\tools-windows.ps1` fetches the pinned tool versions.

`scripts\bench-windows.py` is the counterpart of `bench-macos.py`: `build\windows\apps\windows\procyon-bench-probe.exe`
(`apps/windows/tools/bench_probe.cpp`) launches `dist\windows\Procyon.exe --page <page> --no-settings` (the
defaults, 1 s refresh, nothing read from or written to `HKCU\Software\Procyon`), times the first window, then
samples CPU time and the private working set (Task Manager's *Memory* column) once a second; `tray` closes the
window to the tray first. The CLI's numbers are compared with `GetSystemTimes`, `GlobalMemoryStatusEx`,
`Win32_PageFileUsage` and `EnumProcesses`, and the release zip is rebuilt in a temp folder for its size. Output:
`dist\bench-windows.json`. Options: `--pages`, `--duration`, `--attempts`, `--known-misses`, `--report-only`.

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
  `procyon-cli` smoke run, and the .app uploaded as an artifact. A `windows-latest` job runs
  `scripts\lint-windows.ps1` (after `scripts\tools-windows.ps1` fetched clang-format; the runner's Python checks
  the generated token header), then `scripts\build-windows.cmd` (core tests included), smoke-runs
  `procyon-cli.exe`, uploads `Procyon.exe`, and runs `scripts\bench-windows.py` against the spec's targets
  (`dist\bench-windows.json` is uploaded).
- **`release.yml`** runs on tags `v*`, or manually: tests, universal (arm64 + x86_64) build, DMG + zip +
  SHA-256, GitHub release (versions with `-` are marked prerelease). With the secrets `MACOS_CERTIFICATE_P12`,
  `MACOS_CERTIFICATE_PASSWORD`, `APPLE_ID`, `APPLE_TEAM_ID` and `APPLE_APP_PASSWORD`, it signs with Developer ID
  and notarizes; without them the build is ad-hoc signed. With `HOMEBREW_TAP_TOKEN` (contents write access to
  [manhpham90vn/homebrew-tap](https://github.com/manhpham90vn/homebrew-tap)), stable releases also update the `procyon`
  cask there through `scripts/publish-homebrew.sh`, rendered from `packaging/homebrew/procyon.rb`. A second job
  then builds `Procyon-x.y.z-windows-x64.zip` (unsigned; Authenticode signing and winget are open) with its
  SHA-256 and attaches both to the same release.

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

## Windows

The Windows app reuses the core unchanged (the same `pc_monitor` C ABI, views, deltas and refusals) and the shared
UI of `apps/ui`, and adds only what is Windows: a Win32 window (`window.cpp`), a Direct2D/DirectWrite `Canvas`
(`canvas_d2d.cpp`) and `platform_windows.cpp`. No framework, no runtime beyond Windows 10 1809.

**The shared UI (`apps/ui`).** Every screen, component and widget is written once against the `Canvas`
interface in `ui.hpp` (rectangles, rounded rectangles, ellipses, lines, vector `Path`s with gradients, Gaussian
shadows, clips, masks, scale transforms, text) and the small set of OS services in `platform.hpp`. A new
platform implements those two and a window; the Linux app is a Cairo/Pango `Canvas` and a GTK window. The
`Renderer` turns `design/components.md` into Canvas calls: cards with a real shadow (`tokens.shadow`),
continuous corners (the squircle of SwiftUI's `.continuous` style, after Figma's corner smoothing) on every radius
of 8 or more, sparklines smoothed through midpoints as `Sparkline.swift` does, ring gauges with a halo,
`MetricIcon` tiles with a coloured shadow, and Lucide icons (`icons_data.hpp`, parsed from their SVG into paths at
first use) in place of SF Symbols: `cpu`, `memory-stick`, `hard-drive`, `network`, `box`, `battery-full`, `zap`
for the metrics, and `Renderer::Symbol` for the rest. Pages never see a platform type: colours are `Color`, keys
are `Key`, cursors are `Cursor`, and the window answers `Host::open_url`, `pick_folder`, `request_frame`.

**Text.** Inter (bundled, loaded through an in-memory DirectWrite font set with its weight and optical-size axes
per text style) stands in for SF Pro, Nunito for SF Rounded (the `rounded` design of the tokens: display, title,
metric and stat figures, the wordmark); both ship in `design/fonts` under the OFL. Segoe UI Variable (Text below
20, Display from 20) is the fallback when a resource cannot load, Cascadia Mono / Consolas serve monospace. Text
is antialiased in grayscale, like macOS, rather than ClearType, whose colour fringes show on dark surfaces.
`IDWriteTextLayout`s are cached by text, style and box and dropped after three frames unused: a table redraws
hundreds of unchanged cells a second, and laying each out again was most of the Processes screen's CPU (8% of a
core on the CI runner, at the spec's limit; about half that with the cache). The ellipsis sign and the tabular
typography are shared per font.

**Window.** The canvas draws into a DirectComposition swap chain (premultiplied alpha, `WS_EX_NOREDIRECTIONBITMAP`),
and the window asks for the Mica backdrop (`DWMWA_SYSTEMBACKDROP_TYPE`, Windows 11 22H2+): the sidebar is painted
at 55–62% opacity over it, the way the macOS sidebar's material shows the desktop through, while the page stays
opaque; where Mica is refused the sidebar is opaque. The standard caption is removed (`WM_NCCALCSIZE` keeps the
resize borders only), as the macOS app hides its title bar: a 32-DIP strip at the top drags the window, the Windows
11 caption buttons (46×32, Lucide glyphs, red close on hover) are drawn by the app and hit-tested as
`HTMINBUTTON`/`HTMAXBUTTON`/`HTCLOSE`, so Snap Layouts still appear on the maximize button; the strip above the
first sidebar row drags too. App icons come from the shell (`IShellItemImageFactory` at the size they are shown,
`platform::app_icon`), cached by the `Renderer`; a process without one gets `ProcessIcon`'s tile with a terminal
or cog glyph. Each screen is laid out like
its SwiftUI counterpart (`apps/macos/Sources/ProcyonApp/Screens`): the same sidebar (brand, page rows with a detail
line, metric rows with a 46×22 sparkline, Performance / Manage / Analyze / Machine sections, the Live pill and
Settings at the bottom), the same `ScreenScroll` paddings, `PageHeader` with a trailing accessory, `Panel`
captions, `LiveChart`, `StatGrid`, `MetricCard` (which lifts 1% on hover, eased over `motion.normal`),
`TopAppsPanel`, `ActionBanner`, segmented controls and 24 pt table rows with heat cells. The folder picker stands
in for the macOS open panel in **Who Is Using…**.

| Spec item | Status |
| --- | --- |
| Process list: name, PID, user, CPU, RAM, disk I/O, network | Done except network. `NtQuerySystemInformation` gives every process's counters without privileges, so nothing is `PC_PROC_RESTRICTED`; the path, user and "critical process" flag come from a limited-rights handle, which an unelevated Procyon cannot open for the OS's own processes (csrss, services, lsass, …): their user shows as unknown ("—") until Full Access. Memory is the private working set, like Task Manager. Per-process network comes from the `Microsoft-Windows-Kernel-Network` ETW provider in a real-time session of Procyon's own (`windows_network.cpp`): starting a session takes administrator rights, so `PC_CAP_PROCESS_NETWORK` and the network columns are on only with Full Access. |
| Flat / by app / tree views, search, sort, pin | Done (core). Apps are grouped by executable path; the app name is the executable's `FileDescription` ("Google Chrome"), what Task Manager shows. What lives under the Windows directory (svchost, dwm, …) is the OS, not an app: it groups by name, like the daemons of /System on macOS, and is not `PC_PROC_APP_BUNDLE`. **Pin to Top** keeps one app above the sorted rows in every view, as on macOS; a "top apps" row on Overview or a performance screen opens Processes on that app (by-app view, search cleared, the app pinned and selected), like the macOS `showInProcesses`. |
| End task / force quit / end process tree | Done. End Task posts `WM_CLOSE` to a process's visible windows and terminates a process that has none; Force Quit and End Process Tree terminate. The idle process, `System`, processes flagged critical (`ProcessBreakOnTermination`) and, since that flag needs a handle an unelevated Procyon cannot get, the OS's own smss, csrss, wininit, winlogon, services, lsass, LsaIso, Registry, Memory Compression and Secure System by name are protected. End Process Tree follows the kernel's parent links only where the parent is older than the child: a reused parent pid adopts no strangers (macOS reparents orphans to launchd, so its tree never had this hole). |
| Live charts, per-core usage, system info | Done. Hybrid cores (P/E) from `GetSystemCpuSetInformation`. |
| Memory composition, pressure, page file | Done: in use / standby / free from `SystemMemoryListInformation`, non-paged kernel memory as "wired", the Memory Compression process's working set as "compressed" (`PC_CAP_MEMORY_COMPRESSED` only while that process exists), page files from `SystemPagefileInformation`, pressure from the low-memory notification plus the available share. |
| Disk and network totals, volumes | Done: `IOCTL_DISK_PERFORMANCE` per physical drive, physical interfaces from `GetIfTable2` (virtual adapters skipped, like macOS). |
| GPU: usage, memory, encode/decode; per-process GPU | Done through the `GPU Engine` and `GPU Adapter Memory` performance counters (what Task Manager reads): utilization is the busiest engine type, 3D/decode/encode separately; per-process GPU time adds up, interval by interval, the time of the process's busiest engine in that interval (so the cumulative figure never runs backwards when the busiest engine changes). NVIDIA temperatures through NVML; core counts aren't published. |
| Priority, CPU affinity, suspend/resume, signals | Done: priority classes (never realtime), `SetProcessAffinityMask`, `NtSuspendProcess`/`NtResumeProcess`. No POSIX signals (`PC_CAP_SIGNALS` off). |
| Process details: path, command line, environment, threads | Done: command line through `ProcessCommandLineInformation` (works for every bitness), working directory and environment from the PEB of 64-bit processes, threads from the system process table with names from `GetThreadDescription`. Other users' processes need administrator rights. |
| Startup apps with impact, enable/disable | Done for the Run keys (user, machine, 32-bit) and both Startup folders, with the enabled state Task Manager keeps under `StartupApproved`; entries are named after their executable's `FileDescription` ("Microsoft Edge"), not the registry value. Task Scheduler tasks with a logon or boot trigger outside `\Microsoft` (the OS's own) are listed too, through the Task Scheduler COM API, with their `Enabled` flag switched from the Startup screen; their label is `task:` plus the task path with `|` for `\`. |
| Services: start/stop/restart, enable/disable | Done through the Service Control Manager. Disable remembers the start type (Automatic, Manual) under `HKCU\Software\Procyon\ServiceStartTypes` and Enable puts it back (Automatic when nothing was remembered), the way `launchctl enable` restores a job's definition. "Part of the OS" means the binary lives under the Windows directory. |
| Command palette, tray icon | Done: `Ctrl+K`; closing or minimizing the window keeps Procyon in the notification area (per-process sampling is skipped meanwhile, unless an alert rule watches apps; the Direct2D/DirectWrite stack and the embedded fonts are released and the working set trimmed, so the private working set in the tray is a few MB, well under the spec's 30 MB, and recreated when the window opens again), the tooltip shows the modules switched on in Settings (CPU, memory, network, GPU, like the macOS menu bar modules). Settings is the macOS form (at most 720 wide): Updates, Full access with its status and explanation, Notification area, Alerts (an enabled rule unfolds its threshold and duration), Appearance, Processes (default view), About with links. System has the hero card, Hardware and Software side by side, Kernel and Volumes. |
| Battery | Done: level and times from `GetSystemPowerStatus`, capacity, cycles, rate and temperature from the battery class driver. Apps preventing sleep are read from `powercfg /requests` (run hidden, 5 s timeout; the kernel's `GetPowerRequestList` has no public layout and answers STATUS_INVALID_PARAMETER on Windows 11 25H2), parsed by section order (Display, System, Away mode, Execution, …) so localized headers don't matter; `[PROCESS]` entries are mapped from their NT path to a running pid, `[DRIVER]` entries carry the device description. powercfg needs administrator rights, so the list is empty without Full Access. |
| Temperatures | ACPI thermal zones through the `Thermal Zone Information` performance counters (readable by every user; `MSAcpi_ThermalZoneTemperature` over WMI is the fallback, but it refuses non-administrators on most machines). Those are the firmware's zones, not the CPU die: die temperature is only reachable through a signed kernel driver reading MSRs. Many desktop boards publish a zone with a fixed placeholder (27.8 °C is common) and nothing behind it: a zone that has moved is preferred, and when none has, the CPU screen shows the board's reading labelled "Board zone (fixed reading)" rather than hiding the panel. The first physical drive through `IOCTL_STORAGE_QUERY_PROPERTY`: the generic temperature property, then the NVMe SMART / Health Information log page (the inbox NVMe driver rejects the generic property but serves the log page to any user). SATA SMART needs an administrator and is not read. Fans: not yet. |
| Files & Ports | Done: listening ports, connections and **Who Is Using…** from the system handle table (named pipes are skipped before `NtQueryObject`, which would block on them); working directories are read from each process's PEB alone. IPv4 peers of dual-stack sockets (`::ffff:a.b.c.d` in the IPv6 table) are reported as IPv4, like macOS. Other users' processes need administrator rights. |
| History | Done: one record a minute (machine averages, the CPU peak, the busiest apps) in `%LOCALAPPDATA%\Procyon\history.bin`, a flat record file compacted now and then (no SQLite on Windows); the History screen shows 1 / 6 / 24 hours per metric with the busiest apps and the peaks, a hovered or pinned minute narrows the apps to that minute. The minute in progress is written when the window closes. |
| Alerts | Done: the same rules as macOS (machine CPU, memory, critical pressure, temperature where a sensor exists, an app's CPU or memory) with threshold and duration in Settings, a 15-minute cooldown, shown as Windows notifications through the tray icon. Rules that watch apps keep per-process sampling on while Procyon sits in the tray. |
| Energy, process explanations, plugins | Not started on Windows (energy has no OS counters). |

### Full access (elevation)

There is no helper on Windows: `helper_supported()` is false, `HelperClient` is a stub, and the core acts with the
rights the process has. **Unlock Full Access** restarts `Procyon.exe` through `ShellExecuteEx("runas")` with
`--page <current screen>`, so the UAC prompt replaces the macOS helper approval. The protected-process policy
(`protected_pid`: pids 0 and 4, critical processes, Procyon itself) is enforced in the core as on macOS.

### Measured (i9-14900K, 32 threads, about 235 processes, 1 s updates, release build)

- Core sampling: well under 1% of one core (`procyon-cli`).
- Whole app with the window open: about 1.5% on dashboard screens, 4–5% on Processes (the table lays out
  text for every visible cell each tick). Spec target: under 1–2%.

### Checks

`scripts/lint.sh` formats and lints the Windows sources with the core (clang-format), but clang-tidy skips them on
macOS because they aren't in that compile database; the Windows CI job compiles them with `/W4`. Local builds are
unsigned, so a machine with Smart App Control on refuses to run them; CI runs the tests on a clean runner.

Two kernel quirks worth keeping in mind: `SystemProcessorPerformanceInformation` wants a buffer of exactly one record
per processor (a larger one is refused), and the system tick must be computed as `kernel - idle` on the raw 100 ns
values, because the difference of two separately rounded counters can go backwards and wrap the 32-bit tick delta.
