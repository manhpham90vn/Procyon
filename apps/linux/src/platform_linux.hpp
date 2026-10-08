// Linux-only additions to platform.hpp, for the pieces of the app that feed it.
#pragma once

#include <functional>

namespace procyon::ui::platform {

// Makes GTK's own widgets (the window frame and its controls, menus, dialogs) light or dark with
// Procyon's appearance, also when Settings forces one against the desktop's.
void set_gtk_dark(bool dark);

// Calls `changed` on the main loop when the desktop's colour scheme or GTK theme changes, so a
// "System" appearance follows it at once rather than at the next repaint.
void watch_color_scheme(std::function<void()> changed);

// Set by the tray as a StatusNotifierWatcher comes and goes; read through tray_available().
void set_tray_available(bool available);

}  // namespace procyon::ui::platform
