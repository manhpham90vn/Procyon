# Procyon: Cross-platform task manager spec

Oct 1, 2026 · @Manh Pham Van

## Product overview

A GUI task manager for Windows, macOS and Linux, as deep as the Windows Task Manager but lighter.

**Problem:** no app is cross-platform, has a modern graphical interface, and goes deep enough. People who use several operating systems have to learn 3 different tools; the only popular cross-platform tool (btop) runs in a terminal.

**Target users:**

| Group | Main needs | Priority |
| --- | --- | --- |
| Everyday users | Find the app slowing the machine down, close hung apps, see which apps drain the battery | High (MVP) |
| Developers / power users | Use several OSes; need GPU, ports, locked files, child processes | High |
| Gamers, graphics professionals | Temperatures, GPU, monitoring while gaming/rendering | Medium |
| IT / managing a few machines | See several machines at once, alerts | After v1 |

Status (Oct 3, 2026): the macOS app implements P0, P1 and most of P2 (`docs/development.md` tracks each item);
the Windows app covers P0 plus startup, services, GPU, battery and Files & Ports (status in `docs/development.md`);
Linux is not started.

**Design principles:**

1. Lightness is feature number one: a resource monitor must not eat resources itself.
2. One consistent experience on every OS; where an OS doesn't support something, hide it or say so clearly, never show wrong numbers.
3. Easy to understand for everyday users, deeper for those who need it (progressive disclosure).
4. Modules can be switched on and off: each module has its own CPU cost.
5. No user data collection; everything runs locally.

## Competitor analysis

None of the 6 representative apps is both a GUI and available on all three operating systems; each one is strong in its own area.

| App | Platforms | Biggest strength | What to learn from it |
| --- | --- | --- | --- |
| [Windows Task Manager](https://www.bleepingcomputer.com/news/microsoft/closer-look-at-windows-11s-new-task-manager/) | Windows | The familiar standard, every tab | Efficiency mode, Startup impact, Ctrl+K command palette, adjustable update speed |
| [System Informer](https://www.neowin.net/software/system-informer-3225011/) | Windows | Deep analysis, open source | Find the process holding a file, close network connections, service management, stack traces, portable |
| [Activity Monitor](https://www.howtogeek.com/227240/how-to-monitor-your-macs-health-with-activity-monitor) | macOS | Energy tab | Current Energy Impact + 8-hour average, apps preventing sleep, chart on the Dock icon |
| [Stats](https://github.com/exelban/stats) | macOS | Always visible in the menu bar | 9 modules, customizable widgets, sensors/fans, remote web dashboard, turn modules off to save resources |
| [Mission Center](https://linuxiac.com/mission-center-system-monitoring-app/) | Linux | Beautiful interface, Rust + GTK4 | Per-thread CPU, GPU encode/decode, OpenGL-drawn charts to reduce load |
| [btop](https://tracker.pardus.org.tr/yirmibir/btop) | Linux, macOS, BSD (Windows via btop4win) | Light, nearly cross-platform | Filtering, process tree, sending signals, presets, full configuration menu |

**Market gap:** a cross-platform GUI as deep as System Informer, with an Activity Monitor-style energy tab, Stats-style widgets, and long-term history for every resource, which no app has yet.

## Desktop features

Features come in 3 tiers: P0 is the required MVP, P1 reaches parity with good apps, P2 is what sets us apart.

### P0: MVP

| Module | Feature | Notes |
| --- | --- | --- |
| Processes | Process list: name, PID, user, CPU %, RAM, disk I/O, network | Optional columns, saved column configuration |
| Processes | Group by app (e.g. every Chrome process under 1 row) and parent-child tree | Switch between flat, grouped and tree |
| Processes | Search, filter, sort by every column | Keyboard shortcut to focus the search field |
| Processes | End task (graceful) and End process tree (forced) | Ask for confirmation on system processes |
| Performance | Real-time charts: CPU (total and per core), RAM/swap, disk, network | Keep the last 60 seconds |
| System | Machine info: CPU, RAM, disks, OS, uptime |  |
| Interface | Light/dark following the system, adjustable update speed (0.5–5 seconds, pause) |  |

### P1: Parity with competitors

| Module | Feature | Notes |
| --- | --- | --- |
| GPU | Total usage, VRAM, temperature, encode/decode; per-process GPU where the OS allows | NVIDIA, AMD, Intel, Apple Silicon |
| Processes | Set priority/nice, CPU affinity, suspend/resume, send signals (Unix) | Some need admin rights |
| Processes | Process details: path, command line, environment variables, running time, threads | Open file location, copy info |
| Startup | Apps that start with the system, their impact, enable/disable | A different mechanism per OS |
| Services | View and start/stop/restart services (Windows Services, systemd, launchd) |  |
| Quick commands | Ctrl+K-style command palette: end task, change priority, open file location |  |
| Tray / menu bar | Small widget showing CPU, RAM, network, temperature | Selectable modules |
| Battery | Battery level, battery health, apps preventing sleep | Laptops |

### P2: Differentiators

| Module | Feature | Notes |
| --- | --- | --- |
| History | Keep history of every resource per app (24 hours to 30 days), look back at peaks | Stored locally, compressed, size-capped |
| Energy | Activity Monitor-style energy tab for Windows and Linux too | Estimated where the OS has no exact numbers |
| Deep analysis | Which process is holding a file/folder; which app has a port open, where it connects, close connections | Learned from System Informer |
| Sensors | CPU/GPU/disk temperatures, fan speeds | Hardware-dependent |
| Alerts | Notify when an app uses unusual RAM/CPU or the machine runs too hot | Custom thresholds |
| Explanations | Plain-language "what is this process, should I end it" | Database of common processes |
| Extensions | Plugin/module system, each switchable on its own | Keep the core small |

## Non-functional requirements

The app must be lighter than the built-in Task Manager when running in the background; the numbers below are proposed targets to be re-measured on real machines.

| Area | Requirement | Proposed target |
| --- | --- | --- |
| Performance | CPU with the window open, 1-second updates | < 1–2% on a 4-core machine |
| Performance | CPU with only the tray widget running | < 0.5% |
| Performance | RAM | < 80 MB open, < 30 MB in the background |
| Performance | Startup time | < 1 second |
| Performance | Installer size | < 20 MB |
| Accuracy | Deviation from the OS's own tools | < 5% |
| Permissions | Run as a normal user; only ask for admin/root when an action needs it (through a separate helper) |  |
| Security | Dangerous actions (ending system processes) require confirmation |  |
| Privacy | No data sent out; telemetry, if any, is opt-in |  |
| Accessibility | Fully keyboard-navigable, screen reader support |  |
| Internationalization | English only (decided Oct 2, 2026); units follow the locale |  |
| Distribution | Windows: MSI/winget; macOS: DMG/Homebrew, notarized; Linux: Flatpak, AppImage, .deb |  |

## Platform support

The basics work on all three OSes; per-process GPU and per-process network are the two hardest parts, especially on macOS.

| Data / action | Windows | Linux | macOS |
| --- | --- | --- | --- |
| Processes, CPU, RAM | NtQuerySystemInformation, PDH | /proc | libproc, host\_statistics |
| Per-process disk I/O | Yes | /proc/\[pid\]/io (needs permission for other processes) | Yes (rusage) |
| Per-process network | GetExtendedTcpTable + ETW | /proc/net + eBPF (needs permission) | NetworkStatistics (the private framework `nettop` uses, loaded at runtime; hidden when it is missing) |
| Total GPU | DXGI, PDH | NVML, sysfs (AMD/Intel) | IOKit |
| Per-process GPU | PDH GPU Engine counters | NVML, fdinfo (DRM) | Very limited |
| Temperatures, fans | WMI, often needs a vendor driver | hwmon/lm-sensors | SMC, IOHID (Apple Silicon) |
| Per-app energy | Estimated | Estimated (RAPL if available) | Built in (powermetrics needs root) |
| Startup apps | Registry Run, Startup folder, Task Scheduler | XDG autostart, systemd user | Login Items, LaunchAgents |
| Services | Service Control Manager | systemd (D-Bus) | launchd |
| Ending processes | TerminateProcess | kill/signal | kill/signal |
| Processes holding a file | Restart Manager API, handle enumeration | /proc/\[pid\]/fd | lsof/libproc |

The "Estimated" and "Very limited" entries need a prototype to confirm them before promising them to users.

**Private API risk (macOS):** per-process network (`NetworkStatistics`), per-process GPU time (IOAccelerator
registry) and the Background Task Management list (`sfltool dumpbtm`) are not public API and can change with any
macOS release. Each is loaded or probed at runtime and reported through a capability flag; when one is missing the
app hides the feature rather than showing wrong numbers (principle 2). They must stay behind capability checks and
be re-verified on every macOS beta.

## Architecture

A shared C++ core talks to the OS; each platform has its own native interface for the best performance and the most natural feel.

| Layer | Technology | Role |
| --- | --- | --- |
| Core | C++ | Collect metrics, act on processes/services, history, alerts; no UI dependency |
| OS adapter | C++ per OS | Call the APIs in the "Platform support" table, return shared data structures |
| Windows UI | Win32 | Main window, tray widget |
| macOS UI | SwiftUI | Main window, menu bar widget; calls the core through Swift–C++ interop |
| Linux UI | GTK | Main window, widget/indicator |
| Privileged helper | C++ | Separate process running as admin/root for actions that need it |

**Principles:**

1. The core exposes a stable API (C++ or C ABI) that all three UIs use; business logic does not live in the UI.
2. The core collects and computes metrics once per cycle; the UI only renders what is visible.
3. Every feature is built 3 times at the UI layer, so the UI stays thin and everything shared goes down into the core.

## Licensing and distribution

Procyon is free and open source under the MIT license (decided Oct 3, 2026; this replaces the earlier freemium/Pro
proposal). Every feature in P0, P1 and P2 ships in the same build; there is no paid tier, no license check and no
call home.

**Principles:**

1. No ads, no upgrade nagging, no telemetry. Everything runs locally.
2. Distributed outside the app stores: GitHub releases and Homebrew on macOS (see "Distribution"); the Mac App Store
   is not a target because the privileged helper and the private frameworks the app relies on are not allowed there.
3. Contributions are welcome; `docs/development.md` describes the layout, build and the feature status against this spec.

## Roadmap and open questions

Start with a prototype that validates the adapters, because per-process GPU and network may not be possible on every OS.

| Phase | Scope | Gate to the next phase |
| --- | --- | --- |
| 1. Prototype | Validate the adapters on 3 operating systems, measure CPU/RAM overhead | Meets the performance targets |
| 2. MVP | P0 features on the 2 main OSes, closed beta | Stable beta, user feedback |
| 3. v1 | P1 features, full Windows, macOS, Linux, public release | v1 runs well on all 3 OSes |
| 4. v2 | P2 features: history, energy, alerts | |

No dates set yet; each phase only starts once it passes the gate before it.

**Decided:**

- macOS goes first: the macOS app has reached P2 (status in `docs/development.md`); Windows and Linux start from the
  shared core once it is stable.
- Minimum macOS version: 14 (Sonoma), for the SwiftUI `Table` and Swift Charts APIs the app uses.
- Open source under MIT; no Pro tier (see "Licensing and distribution").

**Open questions:**

- [x] Windows came second (Oct 3, 2026): Win32 + Direct2D over the shared core, elevation through UAC instead of a helper.
- [ ] GTK: use the C API (GTK4) directly, or gtkmm?
- [ ] Fan speeds on macOS need the SMC; worth the private-API risk?

## Sources

- [The new Windows 11 Task Manager (BleepingComputer)](https://www.bleepingcomputer.com/news/microsoft/closer-look-at-windows-11s-new-task-manager/)
- [Windows 11 24H2 Task Manager: Ctrl+K, GPU/NPU (kluczesoft)](https://kluczesoft.pl/wiedza/poradniki/task-manager-menedzer-zadan-windows-11-zaawansowane)
- [System Informer (Neowin)](https://www.neowin.net/software/system-informer-3225011/)
- [Activity Monitor (How-To Geek)](https://www.howtogeek.com/227240/how-to-monitor-your-macs-health-with-activity-monitor)
- [Stats (GitHub)](https://github.com/exelban/stats) and [mac-stats.com](https://mac-stats.com/)
- [Mission Center (Linuxiac)](https://linuxiac.com/mission-center-system-monitoring-app/) and [Mission Center 1.0 (OMG! Ubuntu)](https://www.omgubuntu.co.uk/2025/05/mission-center-1-0-adds-new-features)
- [btop (Pardus)](https://tracker.pardus.org.tr/yirmibir/btop)

The per-platform API table is based on general knowledge and has not been checked against official documentation.
