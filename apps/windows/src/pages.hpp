// Screens and what they need from the window that hosts them.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "store.hpp"
#include "ui.hpp"
#include "widgets.hpp"

namespace procyon::ui {

enum class PageId {
    Overview,
    Processes,
    Cpu,
    Memory,
    Disk,
    Network,
    Gpu,
    Battery,
    Startup,
    Services,
    History,
    Inspect,
    System,
    Settings,
};

struct MenuItem {
    int id = 0;
    std::wstring label;
    bool enabled = true;
    bool checked = false;
    bool separator = false;
    bool destructive = false;
    std::vector<MenuItem> children;
};

struct Settings {
    double interval = 1.0;
    int theme = 0;  // 0 system, 1 light, 2 dark
    bool fahrenheit = false;
    bool minimize_to_tray = true;
    int tray_modules = 1 | 2;            // figures in the tray tooltip: 1 CPU, 2 memory, 4 network, 8 GPU
    bool records_history = true;         // the last 24 hours on disk (History screen)
    int default_view = PC_VIEW_GROUPED;  // the Processes screen's view at launch (pc_view_mode)
};

// What a page may ask of the window.
class Host {
public:
    virtual ~Host() = default;
    virtual Store &store() = 0;
    virtual Renderer &renderer() = 0;
    virtual Settings &settings() = 0;
    virtual HWND hwnd() = 0;
    virtual void repaint() = 0;
    virtual void navigate(PageId page) = 0;
    // Modal confirmation; true when the user chose `action`.
    virtual bool confirm(const std::wstring &title, const std::wstring &message, const std::wstring &action,
                         bool destructive) = 0;
    virtual void alert(const std::wstring &title, const std::wstring &message) = 0;
    // Shows a dialog for a failed action, offering to unlock full access on a permission error.
    virtual void report(pc_result result, const std::wstring &action) = 0;
    // Pops a context menu at window coordinates; returns the chosen id, 0 when dismissed.
    virtual int popup_menu(const std::vector<MenuItem> &items, float x, float y) = 0;
    virtual void show_info(int32_t pid) = 0;
    // Opens Processes on `app_id` (by-app view, search cleared, the app pinned and selected), like
    // the macOS showInProcesses.
    virtual void show_in_processes(const std::string &app_id, int32_t pid) = 0;
    virtual bool elevated() = 0;
    virtual void relaunch_elevated() = 0;
    virtual void copy_to_clipboard(const std::wstring &text) = 0;
    virtual void open_in_explorer(const std::wstring &path) = 0;
    virtual void set_paused(bool paused) = 0;
    virtual bool paused() = 0;
    // Where the mouse is, in page coordinates (-1 when outside the window).
    virtual float mouse_x() = 0;
    virtual float mouse_y() = 0;
};

class Page {
public:
    virtual ~Page() = default;
    virtual PageId id() const = 0;
    virtual std::wstring title() const = 0;
    virtual void paint(Host &host, const Rect &bounds) = 0;
    virtual void mouse_move(Host &, const MouseEvent &) {}
    virtual void mouse_down(Host &, const MouseEvent &, bool right) { (void)right; }
    virtual void mouse_up(Host &, const MouseEvent &) {}
    virtual void double_click(Host &, const MouseEvent &) {}
    virtual void mouse_leave(Host &) {}
    virtual void wheel(Host &, const MouseEvent &) {}
    // Return true when the key was consumed.
    virtual bool key(Host &, const KeyEvent &) { return false; }
    virtual bool character(Host &, wchar_t) { return false; }
    // The page became visible (reload on-demand listings) / a new snapshot arrived.
    virtual void activate(Host &) {}
    virtual void tick(Host &) {}
    virtual int32_t selected_pid() const { return 0; }
    // A menu/accelerator command (IDM_* in res/resource.h); true when the page handled it.
    virtual bool command(Host &, int id) {
        (void)id;
        return false;
    }
    virtual void focus_search() {}
    virtual bool search_focused() const { return false; }
    // Processes only: show and select `app_id` (see Host::show_in_processes).
    virtual void reveal(const std::string &app_id, int32_t pid) {
        (void)app_id;
        (void)pid;
    }
    // Cursor to show for the hovered element.
    virtual LPCWSTR cursor() const { return IDC_ARROW; }
};

std::unique_ptr<Page> make_overview_page();
std::unique_ptr<Page> make_processes_page();
std::unique_ptr<Page> make_performance_page(PageId id);  // Cpu, Memory, Disk, Network, Gpu, Battery
std::unique_ptr<Page> make_startup_page();
std::unique_ptr<Page> make_services_page();
std::unique_ptr<Page> make_inspect_page();
std::unique_ptr<Page> make_history_page();
std::unique_ptr<Page> make_system_page();
std::unique_ptr<Page> make_settings_page();

// ---- screen patterns (the macOS app's ScreenScroll, PageHeader, Panel, StatGrid, …) ----

// The scroll scaffold every screen uses: 32 pt at the sides, 16 on top, at most 1280 wide.
Rect screen_area(const Rect &bounds);
// Vertical gap between the sections of a screen.
constexpr float kSectionGap = tokens::space::xl;
// Height of a card's chrome around its content: padding, caption row and the gap below it.
constexpr float kPanelChrome = tokens::space::lg * 2 + 20 + tokens::space::md;

// Page header: optional 40 pt metric icon, title, subtitle; `trailing` receives the rect to the
// right of the title (48 tall) for accessories. Returns the area below, after the section gap.
Rect page_header(Renderer &r, const Rect &bounds, std::wstring_view title, std::wstring_view subtitle,
                 std::optional<MetricKind> icon, Rect *trailing = nullptr);
// Titled card: draws the card and its caption, returns the content rect inside.
Rect panel(Renderer &r, const Rect &card, std::wstring_view caption, std::optional<Renderer::Symbol> symbol,
           std::optional<MetricKind> tint = std::nullopt, Rect *accessory = nullptr);
// Legend row + sparkline with grid and right axis labels + "60 seconds … now" (LiveChart).
struct ChartSeries {
    const Series *series;  // null for a legend-only entry (a peak)
    std::wstring label;
    std::wstring value;
    bool secondary = false;  // drawn with the gradient's end color, no area
    std::optional<D2D1_COLOR_F> color;
};
void live_chart(Renderer &r, const Rect &bounds, const std::vector<ChartSeries> &series, float max, MetricKind kind,
                const std::function<std::wstring(float)> &axis_label);
// Big number + unit (ValueText); returns the width used.
float value_text(Renderer &r, const Rect &bounds, std::wstring_view value, std::wstring_view unit, Font font,
                 D2D1_COLOR_F color, HAlign align = HAlign::Left);
// Label / value facts in an adaptive grid (StatGrid). Returns the height used.
struct Stat {
    std::wstring label;
    std::wstring value;
    std::wstring detail;
    std::optional<D2D1_COLOR_F> tint;
};
float stat_grid(Renderer &r, const Rect &bounds, const std::vector<Stat> &stats, float min_column = 140,
                int fixed_columns = 0);
float stat_grid_height(float width, size_t count, float min_column = 140, int fixed_columns = 0);
// Notice strips.
struct Banner {
    Rect bounds;
    Rect button;
};
// ActionBanner: 32 pt icon tile, title + message, one accent button on the right (64 tall).
Banner action_banner(Renderer &r, const Rect &bounds, std::wstring_view title, std::wstring_view message,
                     std::wstring_view button_label, Renderer::Tone tone = Renderer::Tone::Accent,
                     bool button_hovered = false);
constexpr float kActionBannerHeight = 64;
// InfoBanner: symbol + text, no button (44 tall).
Banner info_banner(Renderer &r, const Rect &bounds, std::wstring_view message, std::wstring_view button_label,
                   Renderer::Tone tone = Renderer::Tone::Accent);
// Buttons (28 tall).
Rect button(Renderer &r, float x, float y, std::wstring_view label, bool primary, bool destructive, bool hovered,
            float min_width = 0, Font font = Font::BodyMedium);
// End Task: red gradient with an x glyph; grey when nothing can be ended.
Rect end_task_button(Renderer &r, float right, float y, bool enabled, bool hovered);
void empty_state(Renderer &r, const Rect &bounds, std::wstring_view title, std::wstring_view message);
std::wstring process_display_name(const pc_process &p);
// The PC's name for headlines: the model without the manufacturer's legal suffix, else the
// product id, else the host name.
std::wstring display_model(const pc_system_info &info);

// TopAppsPanel: the busiest apps for one metric, name + count + value + usage bar. Returns the hit
// rects of the rows with their pids (for Get Info).
struct TopAppRow {
    Rect rect;
    int32_t pid;
    std::string app_id;  // the core's grouping key, for show_in_processes
};
std::vector<TopAppRow> top_apps_panel(Host &host, const Rect &card, std::wstring_view title, Renderer::Symbol symbol,
                                      MetricKind kind, int32_t column, bool tint, size_t max_rows,
                                      const std::function<std::wstring(const Row &)> &value,
                                      const std::function<float(const Row &)> &fraction);
constexpr float kTopAppRowHeight = 44;  // headline + bar, with the row padding
float top_apps_height(size_t rows);

}  // namespace procyon::ui
