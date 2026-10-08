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
// Machine-wide services and startup entries only change from an elevated Procyon.
inline constexpr bool system_services_need_elevation = true;
inline constexpr bool machine_startup_needs_elevation = true;
// What the per-app network figures leave out, shown under them; null when they count everything.
inline constexpr const wchar_t *process_network_note = nullptr;
// Where the chosen figures show, and whether the tray icon is there while the window is open too.
inline constexpr const wchar_t *tray_figures =
    L"Closing the window moves Procyon to the notification area, with the chosen figures in the icon's tooltip "
    L"and the busiest apps one click away. Opening the window brings it back.";
inline constexpr bool tray_while_open = false;
#else
inline constexpr const wchar_t *name = L"Linux";
inline constexpr const wchar_t *admin = L"root";
inline constexpr const wchar_t *elevation = L"Procyon restarts once as root, after a password prompt.";
inline constexpr const wchar_t *file_manager = L"Files";
// systemctl asks polkit (the desktop's password dialog) for each system-service change, and a
// machine-wide autostart entry is switched by a per-user override: neither needs Procyon as root.
inline constexpr bool system_services_need_elevation = false;
inline constexpr bool machine_startup_needs_elevation = false;
inline constexpr const wchar_t *process_network_note =
    L"Per-app figures count TCP only: QUIC and other UDP traffic (much of a browser's video) isn't attributed to "
    L"apps, and other users' apps need full access.";
inline constexpr const wchar_t *tray_figures =
    L"The chosen figures show next to the tray icon (in its tooltip where the panel shows no text, as on KDE). "
    L"Closing the window keeps Procyon there; the icon's menu opens it again.";
inline constexpr bool tray_while_open = true;
#endif

}  // namespace procyon::ui::os
