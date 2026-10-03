// The main window: sidebar, pages, overlays, tray icon, dialogs. Entry point for main.cpp.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace procyon::ui {

// Creates the window and runs the message loop; returns the exit code.
//   args: "--page <overview|processes|cpu|…>" opens that screen (used when relaunching elevated)
int run_app(HINSTANCE instance, const std::wstring &args, int show_command);

}  // namespace procyon::ui
