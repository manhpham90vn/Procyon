// What differs between the operating systems the shared UI runs on, in words and in behaviour:
// chosen at compile time, so the screens stay free of platform headers.
#pragma once

namespace procyon::ui::os {

#if defined(_WIN32)
inline constexpr const wchar_t *name = L"Windows";
// "need administrator access", "running as administrator".
inline constexpr const wchar_t *admin = L"administrator";
inline constexpr const wchar_t *elevation = L"Procyon restarts once, with a UAC prompt.";
inline constexpr const wchar_t *file_manager = L"Explorer";
// How paths are spelt: the separator, and whether two spellings differing in case are one file.
inline constexpr wchar_t path_separator = L'\\';
inline constexpr bool paths_ignore_case = true;
// Machine-wide services and startup entries only change from an elevated Procyon.
inline constexpr bool system_services_need_elevation = true;
inline constexpr bool machine_startup_needs_elevation = true;
// What the per-app network figures leave out, shown under them; null when they count everything.
inline constexpr const wchar_t *process_network_note = nullptr;
// Per-app network needs Procyon elevated (an ETW session takes administrator rights).
inline constexpr bool process_network_needs_elevation = true;
// The CPU temperature's source: Windows has no user-mode way to the die sensor, so the hottest ACPI
// zone stands in, and a zone that never moves is the board's fixed reading (said so on the CPU screen).
inline constexpr bool cpu_sensor_is_firmware_zone = true;
inline constexpr const wchar_t *cpu_sensor_label = L"Hottest ACPI zone";
inline constexpr const wchar_t *cpu_sensor_detail = L"Hottest ACPI zone";
// Memory composition and swap in the words of the OS's own tools (Task Manager here).
inline constexpr const wchar_t *memory_wired = L"Kernel";
inline constexpr const wchar_t *memory_wired_detail = L"Kernel (non-paged)";
inline constexpr const wchar_t *memory_cached = L"Standby";
inline constexpr const wchar_t *memory_cached_detail = L"Standby (cached)";
inline constexpr const wchar_t *swap_name = L"Page file";
// What Full Access changes, after "Full access runs Procyon as <admin>." in Settings.
inline constexpr const wchar_t *full_access_caption =
    L"It lets Processes read and manage system processes, Files & Ports list every process's files and "
    L"connections, Startup and Services switch machine-wide entries, and shows which apps keep the PC awake and "
    L"how much network each process uses. Without it, those rows show a lock.";
// Where the chosen figures show, and whether the tray icon is there while the window is open too.
// The icon stays while the window is open too: alerts are shown as its notifications.
inline constexpr const wchar_t *tray_figures =
    L"The chosen figures show in the notification-area icon's tooltip. Closing the window keeps Procyon "
    L"there; click the icon to open it again.";
inline constexpr bool tray_while_open = true;
// Whether a process's nice value is its own (Linux) or stands for a priority class (Windows).
inline constexpr bool nice_is_exact = false;
// The name of a pid that owns sockets without being in the process list (the kernel's own).
inline constexpr const wchar_t *unlisted_process_name(int pid) {
    return pid == 4 ? L"System" : pid == 0 ? L"System Idle Process" : nullptr;
}
#else
inline constexpr const wchar_t *name = L"Linux";
inline constexpr const wchar_t *admin = L"root";
inline constexpr const wchar_t *elevation = L"Procyon restarts once as root, after a password prompt.";
inline constexpr const wchar_t *file_manager = L"Files";
inline constexpr wchar_t path_separator = L'/';
inline constexpr bool paths_ignore_case = false;
// systemctl asks polkit (the desktop's password dialog) for each system-service change, and a
// machine-wide autostart entry is switched by a per-user override: neither needs Procyon as root.
inline constexpr bool system_services_need_elevation = false;
inline constexpr bool machine_startup_needs_elevation = false;
inline constexpr const wchar_t *process_network_note =
    L"Per-app figures count TCP only: QUIC and other UDP traffic (much of a browser's video) isn't attributed to "
    L"apps, and other users' apps need full access.";
inline constexpr bool process_network_needs_elevation = false;
inline constexpr bool cpu_sensor_is_firmware_zone = false;
inline constexpr const wchar_t *cpu_sensor_label = L"CPU (hottest sensor)";
inline constexpr const wchar_t *cpu_sensor_detail = L"Hottest die sensor";
// Unreclaimable kernel memory and the page cache, in macOS's words.
inline constexpr const wchar_t *memory_wired = L"Wired";
inline constexpr const wchar_t *memory_wired_detail = L"Wired";
inline constexpr const wchar_t *memory_cached = L"Cached files";
inline constexpr const wchar_t *memory_cached_detail = L"Cached files";
inline constexpr const wchar_t *swap_name = L"Swap";
// Services ask polkit per action, autostart entries are per-user overrides, and per-app network and
// sleep inhibitors are readable by every user: root only opens up other users' processes.
inline constexpr const wchar_t *full_access_caption =
    L"It lets Processes end and reprioritize other users' processes and read their command lines and disk use, "
    L"raise any process's priority, and lets Files & Ports list every process's files and connections. Without "
    L"it, those rows show a lock.";
inline constexpr const wchar_t *tray_figures =
    L"The chosen figures show next to the tray icon (in its tooltip where the panel shows no text, as on KDE). "
    L"Closing the window keeps Procyon there; the icon's menu opens it again.";
inline constexpr bool tray_while_open = true;
inline constexpr bool nice_is_exact = true;
inline constexpr const wchar_t *unlisted_process_name(int) { return nullptr; }
#endif

}  // namespace procyon::ui::os
