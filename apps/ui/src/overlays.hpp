// Floating panels drawn over the current page: the ⌘K-style command palette and Get Info.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pages.hpp"

namespace procyon::ui {

class Overlay {
public:
    virtual ~Overlay() = default;
    virtual void paint(Host &host, const Rect &window) = 0;
    virtual void mouse_move(Host &, const MouseEvent &) {}
    virtual void mouse_down(Host &, const MouseEvent &, bool right) { (void)right; }
    virtual void wheel(Host &, const MouseEvent &) {}
    virtual bool key(Host &, const KeyEvent &) { return false; }
    virtual bool character(Host &, wchar_t) { return false; }
    virtual void tick(Host &) {}
    virtual bool is_palette() const { return false; }
    bool closed() const { return closed_; }
    void close() { closed_ = true; }

protected:
    bool closed_ = false;
};

struct PaletteItem {
    enum class Kind { Screen, Command, App };
    Kind kind;
    std::wstring title;
    std::wstring subtitle;
    std::wstring shortcut;
    std::function<void()> run;
    std::wstring icon_path;  // App rows: the app's icon (see Renderer::app_icon)
    bool icon_system = false;
    // A row with children opens them as the palette's next level (an app's actions, the priority
    // steps) instead of running; Escape or Backspace in an empty field goes back.
    std::vector<PaletteItem> children;
};

std::unique_ptr<Overlay> make_command_palette(std::vector<PaletteItem> items);
// The macOS palette's second level for an app: End Task, Force Quit, Suspend/Resume, Priority, Get Info,
// Show in Processes, Open File Location, Copy Path, Copy PID. `command` runs a window command
// (IDM_END_TASK, IDM_FORCE_QUIT) on the Processes screen once the app is selected there.
std::vector<PaletteItem> app_palette_actions(Host &host, const Row &row, const pc_process *process,
                                             const std::wstring &name, std::function<void(int)> command);
std::unique_ptr<Overlay> make_info_sheet(int32_t pid);

}  // namespace procyon::ui
