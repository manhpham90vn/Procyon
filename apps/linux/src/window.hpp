// The Linux app's main window and event loop (GTK 4).
#pragma once

namespace procyon::ui {

// Runs Procyon. Options: --page <name> (the screen to open), --no-settings (defaults, nothing
// saved), --screenshot <file.png> [--dark|--light] (render the page offscreen at 2x and quit),
// --version. Returns the exit status.
int run_app(int argc, char **argv);

}  // namespace procyon::ui
