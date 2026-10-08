// Linux-only additions to platform.hpp, for the pieces of the app that feed it.
#pragma once

namespace procyon::ui::platform {

// Set by the tray as a StatusNotifierWatcher comes and goes; read through tray_available().
void set_tray_available(bool available);

}  // namespace procyon::ui::platform
