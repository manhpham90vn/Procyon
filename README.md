# Procyon

A lightweight, open-source task manager for macOS. See what is using your CPU, memory, disk, network,
GPU and battery, find out what a process is, and stop the ones you don't need.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/overview-dark.png">
  <img src="docs/screenshots/overview-light.png" alt="Procyon's Overview screen">
</picture>

- **Processes**: every running process with CPU, memory, disk, network, GPU and power, as a flat list,
  grouped by app or as a tree. Search, sort, end, force quit, suspend, change priority.
- **Live charts**: CPU (total and per core), memory and swap, disk, network, GPU, temperatures.
- **Startup and Services**: login items, launch agents and daemons. Turn them on or off, start, stop and restart them.
- **Energy and Battery**: power use per app, battery health and cycles, and which apps keep your Mac awake.
- **History**: the last 24 hours of every metric. Click a spike to see which apps caused it.
- **Files & Ports**: listening ports, network connections, open files, and **Who Is Using…** for a file or disk.
- **Plain-language explanations** for about 110 common macOS processes: what each one is, and whether it is safe to end.
- **Menu bar widget**, **alerts** with your own thresholds, a **⌘K command palette**, and light and dark mode.

Procyon is light on resources: sampling uses about 0.5% of one core.

## Screenshots

<table>
<tr>
<td width="50%">

**Processes**

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/processes-dark.png">
  <img src="docs/screenshots/processes-light.png" alt="Processes screen">
</picture>

</td>
<td width="50%">

**CPU**

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/cpu-dark.png">
  <img src="docs/screenshots/cpu-light.png" alt="CPU screen">
</picture>

</td>
</tr>
<tr>
<td width="50%">

**Energy**

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/energy-dark.png">
  <img src="docs/screenshots/energy-light.png" alt="Energy screen">
</picture>

</td>
<td width="50%">

**History**

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/history-dark.png">
  <img src="docs/screenshots/history-light.png" alt="History screen">
</picture>

</td>
</tr>
<tr>
<td width="50%">

**Files & Ports**

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/inspect-dark.png">
  <img src="docs/screenshots/inspect-light.png" alt="Files & Ports screen">
</picture>

</td>
<td width="50%">

**Startup**

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/startup-dark.png">
  <img src="docs/screenshots/startup-light.png" alt="Startup screen">
</picture>

</td>
</tr>
</table>

## Requirements

- macOS 14 Sonoma or later
- Apple Silicon or Intel (the release is a universal app)

## Install

### Homebrew

```sh
brew install --cask manhpham90vn/tap/procyon
```

Update with `brew upgrade --cask procyon`.

### Download

1. Download the latest `Procyon-x.y.z.dmg` from the [Releases page](https://github.com/manhpham90vn/Procyon/releases/latest).
2. Open the DMG and drag **Procyon** into **Applications**.
3. Open Procyon from Applications or Spotlight.

Releases are signed with a Developer ID and notarized by Apple, so they open without Gatekeeper warnings.
To check your download, compare it with the `.sha256` file from the same release:

```sh
shasum -a 256 Procyon-x.y.z.dmg
```

## Using Procyon

### Getting around

Pick a screen in the sidebar, or use the keyboard:

| Shortcut | Action |
| --- | --- |
| `⌘1` … `⌘9` | Overview, Processes, CPU, Memory, GPU, Disk, Network, Startup, Services |
| `⌘K` | Command palette: jump to a screen, run a command, act on an app |
| `⌘F` | Find a process |
| `⌘I` | Get Info on the selected process |
| `⌘⌫` | End Task |
| `⌥⌘⌫` | Force Quit |
| `⇧⌥⌘⌫` | End Process Tree |
| `⇧⌘P` | Pause or resume updates |
| `⌘,` | Settings |

### Ending a process

Select a process in **Processes** and press `⌘⌫` (or use the **Process** menu or the ⌘K palette). Procyon asks for confirmation before
ending system processes and shows what the process does, if it knows. The kernel, `launchd` and Procyon
itself are protected and can't be ended.

### Full access

Procyon runs as your normal user. Processes owned by root show `—` with a lock icon until you click
**Unlock Full Access** (the banner on Processes, Startup or Services, or in the ⌘K palette). macOS then asks
you to allow Procyon's helper in **System Settings → General → Login Items**. You only do this once, and you
need an administrator account.

Full access lets Procyon show every process, act on other users' processes, raise priority, and manage
system services. To take it back, open **Settings → Full access**: **Turn Off Full Access** disconnects for now and
keeps the helper approved, **Remove Administrator Helper** also unregisters it from Login Items. Both are in the ⌘K
palette too.

### Menu bar

Closing the window keeps Procyon running in the menu bar instead of the Dock. Choose what the menu bar
shows (CPU, memory, network, GPU, temperature, battery) in **Settings → Menu bar**. Click the menu bar item and
choose **Open Procyon** to bring the window back, or **Quit** to exit.

### Alerts

**Settings → Alerts** sends a notification when CPU, memory, memory pressure, CPU temperature, or a single
app's CPU or memory stays over a threshold for a time you choose.

### History

**History** keeps one sample a minute for 24 hours, so you can see what was slowing your Mac down earlier.
Pick a metric and a range (1, 6 or 24 hours), then click the chart or a peak to see the busiest apps at that
moment. You can turn recording off or clear it in Settings. It uses a few MB in
`~/Library/Application Support/Procyon`.

## Uninstall

1. If you unlocked full access, open **Settings → Full access** and click **Remove Administrator Helper**.
2. Quit Procyon and move it from Applications to the Trash, or run `brew uninstall --cask procyon`.
3. Optionally, delete its history: `~/Library/Application Support/Procyon`
   (`brew uninstall --cask --zap procyon` removes it along with the settings).

## Known limitations

- macOS only for now. The core is cross-platform C++, and Windows and Linux front ends are planned.
- The interface is English only.
- Not available on macOS: CPU affinity, GPU encode/decode usage, fan speeds, and closing another process's
  network connection.

## Building from source

You need Xcode 27 on macOS 14 or later.

```sh
git clone https://github.com/manhpham90vn/Procyon.git
cd Procyon
make run      # build dist/Procyon.app and open it
make test     # run the unit tests
make help     # all targets
```

Builds from source are ad-hoc signed. Full access then works through a password prompt each launch
instead of the one-time approval.

## Contributing

Bug reports, ideas and pull requests are welcome on
[GitHub Issues](https://github.com/manhpham90vn/Procyon/issues). Before opening a pull request, run
`make format` and `make ci`, which runs the same checks as CI.

The repo layout, CI and release process, how the privileged helper works, and feature status against the
[product spec](docs/procyon-spec.md) are in [docs/development.md](docs/development.md).

## License

Procyon is released under the [MIT License](LICENSE).
