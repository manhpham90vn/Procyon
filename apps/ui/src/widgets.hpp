// Reusable controls drawn with the renderer: search field, scrolling, and the virtual table used
// by Processes, Startup, Services and Files & Ports.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ui.hpp"

namespace procyon::ui {

// SearchField: magnifier, text, clear button or a key cap hint; Escape clears.
class TextField {
public:
    std::wstring text;
    std::wstring placeholder = L"Search";
    std::wstring keycap = L"Ctrl+F";
    bool focused = false;
    Rect bounds;

    void paint(Renderer &r, bool search_icon = true);
    // Returns true when the click landed in the field (focus) or on its clear button (cleared).
    bool mouse_down(float x, float y, bool &cleared);
    // Returns true when the text changed; `consumed` tells the caller the key was handled.
    bool key(const KeyEvent &e, bool &consumed);
    bool character(wchar_t c);
    void clear() {
        text.clear();
        caret_ = 0;
    }
    void set_text(const std::wstring &value) {
        text = value;
        caret_ = text.size();
    }

private:
    size_t caret_ = 0;
};

struct ScrollState {
    float offset = 0;
    float content = 0;
    float viewport = 0;

    float max_offset() const { return content > viewport ? content - viewport : 0; }
    void clamp() {
        if (offset > max_offset()) offset = max_offset();
        if (offset < 0) offset = 0;
    }
    void wheel(int delta, float step = kRowHeight * 3) {
        offset -= delta / 120.0f * step;
        clamp();
    }
    void page(int direction) {
        offset += direction * viewport * 0.9f;
        clamp();
    }
    void reveal(float top, float height) {
        if (top < offset) offset = top;
        if (top + height > offset + viewport) offset = top + height - viewport;
        clamp();
    }
    void paint(Renderer &r, const Rect &track) const;
};

// A table column: fixed width, or flexible (`flex` > 0 shares the leftover width).
struct Column {
    int id = 0;
    std::wstring title;
    float width = 80;
    float flex = 0;
    HAlign align = HAlign::Left;
    bool sortable = true;
    bool numeric = true;  // tabular figures
};

struct Cell {
    std::wstring text;
    std::wstring subtitle;  // second line in caption, tertiary (rows taller than 24 only)
    std::optional<Color> color;
    float heat = 0;  // 0..1 tint of the cell background with `heat_kind`
    std::optional<MetricKind> heat_kind;
    bool dim = false;  // restricted / unknown
    bool mono = false;
    bool bold = false;         // headline weight (group rows, names)
    int count = 0;             // a small count pill after the text (group rows)
    std::optional<float> bar;  // usage bar under the text, 0..1
    std::optional<MetricKind> bar_kind;
    std::optional<Renderer::Tone> badge;
    std::optional<bool> toggle;  // a switch instead of text
    bool toggle_enabled = true;
    bool lock = false;  // lock symbol after the text (restricted)
    bool paused = false;
    bool pinned = false;       // pin symbol after the text (the app kept at the top)
    bool icon = false;         // a 16 pt app icon before the text (first column)
    bool icon_system = false;  // the stand-in shows a cog instead of a terminal
    std::wstring icon_path;    // the executable or bundle whose icon to show; empty for the stand-in
};

class Table {
public:
    std::vector<Column> columns;
    int sort_column = -1;
    bool sort_descending = true;
    int selected = -1;
    int hovered = -1;
    ScrollState scroll;
    bool focused = false;
    bool show_header = true;
    float row_height = 24;

    // Data callbacks.
    std::function<int()> row_count;
    std::function<Cell(int row, int column)> cell;
    std::function<int(int row)> indent;    // depth, 0 for top level
    std::function<int(int row)> expander;  // 0 none, 1 collapsed, 2 expanded
    std::function<void(int row)> on_toggle;
    std::function<void(int column)> on_sort;
    std::function<void(int row)> on_activate;  // double click / Enter
    std::function<void(int row, float x, float y)> on_context;
    std::function<void(int row)> on_select;
    std::function<void(int row, int column)> on_switch;  // a toggle cell was clicked

    void layout(const Rect &bounds);
    void paint(Renderer &r);
    void mouse_move(const MouseEvent &e);
    void mouse_leave();
    bool mouse_down(const MouseEvent &e, bool right);
    void double_click(const MouseEvent &e);
    void wheel(const MouseEvent &e);
    bool key(const KeyEvent &e);
    void select(int row);
    int row_at(float y) const;
    const Rect &bounds() const { return bounds_; }
    Rect row_rect(int row) const;
    static constexpr float kHeaderHeight = 28;

private:
    float column_x(size_t index) const;
    float column_width(size_t index) const;
    int column_at(float x) const;
    void ensure_visible(int row);

    Rect bounds_;
    Rect body_;
    std::vector<float> widths_;
    int hovered_header_ = -1;
};

}  // namespace procyon::ui
