// The few operating-system services the shared UI needs. Each app (apps/windows, apps/linux)
// implements them next to its window and canvas.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "ui.hpp"

namespace procyon::ui::platform {

// The icon of the executable or app bundle at `path`, rendered at `pixels` square; null when the
// OS has none for it. Called on the UI thread; the Renderer caches the result.
std::shared_ptr<const Image> app_icon(std::wstring_view path, int pixels);

// Whether the OS asks apps for a dark appearance.
bool system_prefers_dark();

// A file in Procyon's per-user data folder (%LOCALAPPDATA%\Procyon, ~/.local/share/procyon).
std::wstring data_file(std::wstring_view name);
// Creates the folders a file path needs before it is written.
void create_parent_directories(std::wstring_view path);

// Called after Procyon wrote `path` in its data or settings folder. The Linux root copy (pkexec)
// writes into the desktop user's folders and hands what it wrote back to them; elsewhere a no-op.
void file_written(std::wstring_view path);

// The clipboard's text, empty when it holds none.
std::wstring clipboard_text();

// Whether closing the window can leave an icon to come back from (the notification area on Windows;
// a StatusNotifierItem host on Linux, which not every desktop runs). Read on the UI thread.
bool tray_available();

// Called by the sampler thread as it starts and ends (COM apartments on Windows).
void sampler_thread_begin();
void sampler_thread_end();

// Small per-user settings store of integers: the registry under HKCU\Software\Procyon on
// Windows, a key file on Linux. `group` is a sub-key or section ("", "Alerts").
std::optional<int64_t> read_setting(std::wstring_view group, std::wstring_view name);
void write_setting(std::wstring_view group, std::wstring_view name, int64_t value);

}  // namespace procyon::ui::platform
