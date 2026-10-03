#include "widgets.hpp"

#include <algorithm>

namespace procyon::ui {

// ---------------------------------------------------------------------------------------------
// TextField (SearchField)
// ---------------------------------------------------------------------------------------------

void TextField::paint(Renderer &r, bool search_icon) {
    const Theme &theme = r.theme();
    const Rect &b = bounds;
    const float radius = tokens::radius::sm + 1;
    r.fill_round(b, radius, theme.surface_sunken());
    r.stroke_round(b, radius, focused ? with_alpha(theme.accent(), 0.6f) : theme.border(), 1);
    Rect inner = b.inset(tokens::space::sm + 2, 0);
    if (search_icon) {
        const Rect icon = inner.take_left(12 + tokens::space::xs + 2);
        r.symbol(Renderer::Symbol::Search, Rect{icon.x, icon.cy() - 6, 12, 12},
                 focused ? theme.accent() : theme.text_tertiary(), 1.5f);
    }
    // Trailing: a clear button while there is text, else the key cap hint.
    if (!text.empty()) {
        const Rect clear = inner.take_right(16);
        r.fill_circle(clear.cx(), clear.cy(), 7, theme.text_tertiary());
        r.symbol(Renderer::Symbol::Close, Rect{clear.cx() - 3.5f, clear.cy() - 3.5f, 7, 7}, theme.surface_sunken(),
                 1.5f);
    } else if (!keycap.empty() && !focused) {
        const float w = r.measure(keycap, Font::Caption) + 10;
        const Rect cap = inner.take_right(w).inset(0, (inner.h - 16) / 2);
        r.stroke_round(cap, tokens::radius::xs, theme.border_strong(), 1);
        TextStyle style;
        style.font = Font::Caption;
        style.halign = HAlign::Center;
        r.text(keycap, cap, style, theme.text_tertiary());
    }
    TextStyle style;
    style.font = Font::Body;
    if (text.empty()) {
        r.text(placeholder, inner, style, theme.text_tertiary());
    } else {
        r.text(text, inner, style, theme.text());
    }
    if (focused) {
        const float caret_x = inner.x + r.measure(text.substr(0, caret_), Font::Body);
        if (caret_x < inner.right()) r.line(caret_x, inner.y + 7, caret_x, inner.bottom() - 7, theme.accent(), 1);
    }
}

bool TextField::mouse_down(float x, float y, bool &cleared) {
    cleared = false;
    if (!bounds.contains(x, y)) {
        focused = false;
        return false;
    }
    const Rect clear{bounds.right() - tokens::space::sm - 2 - 16, bounds.y, 16, bounds.h};
    if (!text.empty() && clear.contains(x, y)) {
        this->clear();
        cleared = true;
    }
    focused = true;
    return true;
}

bool TextField::key(const KeyEvent &e, bool &consumed) {
    consumed = false;
    if (!focused) return false;
    bool changed = false;
    switch (e.vk) {
        case VK_ESCAPE:
            changed = !text.empty();
            clear();
            focused = false;
            consumed = true;
            break;
        case VK_BACK:
            if (caret_ > 0) {
                if (e.ctrl) {
                    size_t start = caret_;
                    while (start > 0 && text[start - 1] == L' ') --start;
                    while (start > 0 && text[start - 1] != L' ') --start;
                    text.erase(start, caret_ - start);
                    caret_ = start;
                } else {
                    text.erase(caret_ - 1, 1);
                    --caret_;
                }
                changed = true;
            }
            consumed = true;
            break;
        case VK_DELETE:
            if (caret_ < text.size()) {
                text.erase(caret_, 1);
                changed = true;
            }
            consumed = true;
            break;
        case VK_LEFT:
            if (caret_ > 0) --caret_;
            consumed = true;
            break;
        case VK_RIGHT:
            if (caret_ < text.size()) ++caret_;
            consumed = true;
            break;
        case VK_HOME:
            caret_ = 0;
            consumed = true;
            break;
        case VK_END:
            caret_ = text.size();
            consumed = true;
            break;
        case 'A':
            if (e.ctrl) consumed = true;  // select-all is a no-op: the field is tiny
            break;
        case 'V':
            if (e.ctrl && OpenClipboard(nullptr)) {
                if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto chars = static_cast<const wchar_t *>(GlobalLock(data))) {
                        std::wstring pasted(chars);
                        for (wchar_t &c : pasted)
                            if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
                        text.insert(caret_, pasted);
                        caret_ += pasted.size();
                        changed = true;
                        GlobalUnlock(data);
                    }
                }
                CloseClipboard();
                consumed = true;
            }
            break;
        default: break;
    }
    return changed;
}

bool TextField::character(wchar_t c) {
    if (!focused || c < 0x20 || c == 0x7F) return false;
    text.insert(caret_, 1, c);
    ++caret_;
    return true;
}

// ---------------------------------------------------------------------------------------------
// ScrollState
// ---------------------------------------------------------------------------------------------

void ScrollState::paint(Renderer &r, const Rect &track) const {
    if (content <= viewport || viewport <= 0) return;
    const float thumb_h = std::max(24.0f, track.h * viewport / content);
    const float thumb_y = track.y + (track.h - thumb_h) * (offset / max_offset());
    r.fill_round(Rect{track.x, thumb_y, track.w, thumb_h}, tokens::radius::pill,
                 with_alpha(r.theme().text_tertiary(), 0.5f));
}

// ---------------------------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------------------------

namespace {
constexpr float kCellPad = 7;  // intercell spacing 6 + the label's own inset, like the AppKit table
}

void Table::layout(const Rect &bounds) {
    bounds_ = bounds;
    body_ = bounds;
    if (show_header) body_.take_top(kHeaderHeight);
    widths_.assign(columns.size(), 0);
    float fixed = 0, flex = 0;
    for (const Column &c : columns) {
        fixed += c.width;
        flex += c.flex;
    }
    const float spare = std::max(0.0f, bounds.w - fixed);
    // When the fixed widths alone don't fit, every column gives up the same share.
    const float squeeze = fixed > bounds.w && fixed > 0 ? bounds.w / fixed : 1;
    for (size_t i = 0; i < columns.size(); ++i)
        widths_[i] = columns[i].width * squeeze + (flex > 0 ? spare * columns[i].flex / flex : 0);
    scroll.viewport = body_.h;
    scroll.content = static_cast<float>(row_count ? row_count() : 0) * row_height;
    scroll.clamp();
}

float Table::column_x(size_t index) const {
    float x = bounds_.x;
    for (size_t i = 0; i < index && i < widths_.size(); ++i) x += widths_[i];
    return x;
}

float Table::column_width(size_t index) const { return index < widths_.size() ? widths_[index] : 0; }

int Table::column_at(float x) const {
    for (size_t i = 0; i < columns.size(); ++i)
        if (x >= column_x(i) && x < column_x(i) + column_width(i)) return static_cast<int>(i);
    return -1;
}

int Table::row_at(float y) const {
    if (y < body_.y || y >= body_.bottom()) return -1;
    const int row = static_cast<int>((y - body_.y + scroll.offset) / row_height);
    const int count = row_count ? row_count() : 0;
    return row >= 0 && row < count ? row : -1;
}

Rect Table::row_rect(int row) const {
    return Rect{body_.x, body_.y + row * row_height - scroll.offset, body_.w, row_height};
}

void Table::paint(Renderer &r) {
    const Theme &theme = r.theme();
    const int count = row_count ? row_count() : 0;

    if (show_header) {
        Rect header = bounds_;
        header.h = kHeaderHeight;
        r.fill(header, theme.surface_raised());
        r.line(header.x, header.bottom() - 0.5f, header.right(), header.bottom() - 0.5f, theme.border());
        for (size_t i = 0; i < columns.size(); ++i) {
            const Column &c = columns[i];
            Rect cell_rect{column_x(i), header.y, column_width(i), header.h};
            if (i > 0) r.line(cell_rect.x, header.y + 7, cell_rect.x, header.bottom() - 7, theme.border());
            if (static_cast<int>(i) == hovered_header_ && c.sortable)
                r.fill(cell_rect, with_alpha(theme.text(), 0.04f));
            Rect inner = cell_rect.inset(kCellPad, 0);
            const bool sorted = sort_column == c.id;
            TextStyle style;
            style.font = sorted ? Font::LabelSemibold : Font::Label;
            style.halign = c.align;
            r.text(c.title, inner, style, sorted ? theme.text() : theme.text_secondary());
            if (sorted) {
                // The indicator sits right next to the title, whichever way it is aligned.
                const float title_w = std::min(inner.w - 12, r.measure(c.title, Font::LabelSemibold));
                const float ax = c.align == HAlign::Right ? inner.right() - title_w - 14 : inner.x + title_w + 4;
                const float ay = inner.cy();
                if (sort_descending) {
                    r.line(ax + 2, ay - 2, ax + 5, ay + 2, theme.text_secondary(), 1.5f);
                    r.line(ax + 5, ay + 2, ax + 8, ay - 2, theme.text_secondary(), 1.5f);
                } else {
                    r.line(ax + 2, ay + 2, ax + 5, ay - 2, theme.text_secondary(), 1.5f);
                    r.line(ax + 5, ay - 2, ax + 8, ay + 2, theme.text_secondary(), 1.5f);
                }
            }
        }
    }

    r.push_clip(body_);
    const int first = std::max(0, static_cast<int>(scroll.offset / row_height));
    const int last = std::min(count - 1, static_cast<int>((scroll.offset + body_.h) / row_height) + 1);
    const bool two_line = row_height >= 40;
    for (int row = first; row <= last; ++row) {
        const Rect rr = row_rect(row);
        const bool is_selected = row == selected;
        if (is_selected)
            r.fill(rr, with_alpha(theme.accent(), focused ? 0.22f : 0.12f));
        else if (row == hovered)
            r.fill(rr, with_alpha(theme.text(), 0.035f));
        else if (two_line && row % 2 == 1)
            r.fill(rr, with_alpha(theme.text(), 0.02f));
        const int depth = indent ? indent(row) : 0;
        const int exp = expander ? expander(row) : 0;
        for (size_t i = 0; i < columns.size(); ++i) {
            const Column &c = columns[i];
            Rect cell_rect{column_x(i), rr.y, column_width(i), rr.h};
            Cell data = this->cell ? this->cell(row, c.id) : Cell{};
            if (data.heat >= 0.01f && data.heat_kind) {
                // Heat cell: the metric color at 8–50% opacity, scaled by load.
                const float alpha = 0.08f + 0.42f * std::clamp(data.heat, 0.0f, 1.0f);
                r.fill_round(cell_rect.inset(3, 1), tokens::radius::xs,
                             with_alpha(rgba(metric_style(*data.heat_kind).start), alpha));
            }
            Rect inner = cell_rect.inset(kCellPad, 0);
            if (i == 0) {
                inner.x += depth * 20.0f;
                inner.w -= depth * 20.0f;
                if (expander) {
                    const Rect toggle = inner.take_left(14);
                    if (exp)
                        r.symbol(exp == 2 ? Renderer::Symbol::ChevronDown : Renderer::Symbol::ChevronRight,
                                 Rect{toggle.x, toggle.cy() - 6, 12, 12}, theme.text_tertiary(), 1.5f);
                    inner.x += 4;
                    inner.w -= 4;
                }
            }
            if (data.toggle) {
                const float tx = c.align == HAlign::Right ? inner.right() - 36 : inner.x;
                r.toggle_switch(tx, inner.cy() - 10, *data.toggle, data.toggle_enabled);
                continue;
            }
            if (data.badge) {
                const float w = r.badge(0, 0, data.text, *data.badge, true);
                const float x = c.align == HAlign::Right ? inner.right() - w : inner.x;
                r.badge(x, inner.cy() - 8.5f, data.text, *data.badge);
                continue;
            }
            D2D1_COLOR_F color = data.color ? *data.color : theme.text();
            if (data.dim) color = theme.text_tertiary();
            TextStyle style;
            style.font = data.mono ? Font::Mono : data.bold ? Font::Headline : Font::Body;
            style.halign = c.align;
            style.tabular = c.numeric && i != 0;
            // Trailing decorations on the first column: count pill, lock, suspended mark.
            float trailing = 0;
            if (data.count > 0) trailing += r.measure(std::to_wstring(data.count), Font::Caption) + 10 + 6;
            if (data.lock) trailing += 10 + 6;
            if (data.paused) trailing += 12 + 6;
            if (data.pinned) trailing += 12 + 6;
            if (two_line) {
                Rect top = inner;
                top.y += 4;
                top.h = inner.h / 2;
                const float used = r.text(data.text, Rect{top.x, top.y, top.w - trailing, top.h}, style, color);
                TextStyle sub;
                sub.font = Font::Caption;
                sub.halign = c.align;
                r.text(data.subtitle, Rect{inner.x, inner.y + inner.h / 2 - 2, inner.w, inner.h / 2}, sub,
                       theme.text_tertiary());
                (void)used;
            } else if (data.bar) {
                Rect text_rect = inner;
                text_rect.h = rr.h - 6;
                r.text(data.text, text_rect, style, color);
                r.usage_bar(Rect{inner.x, rr.bottom() - 5, inner.w, 3}, *data.bar,
                            data.bar_kind ? *data.bar_kind : MetricKind::Cpu);
            } else {
                const float text_w = std::min(inner.w - trailing, r.measure(data.text, style.font));
                r.text(data.text, Rect{inner.x, inner.y, inner.w - trailing, inner.h}, style, color);
                float x = inner.x + text_w + 6;
                if (data.count > 0) {
                    const std::wstring n = std::to_wstring(data.count);
                    const float w = r.measure(n, Font::Caption) + 10;
                    r.fill_round(Rect{x, inner.cy() - 7, w, 14}, tokens::radius::pill, theme.track());
                    TextStyle cs;
                    cs.font = Font::Caption;
                    cs.halign = HAlign::Center;
                    r.text(n, Rect{x, inner.cy() - 7, w, 14}, cs, theme.text_tertiary());
                    x += w + 6;
                }
                if (data.lock) {
                    r.symbol(Renderer::Symbol::Lock, Rect{x, inner.cy() - 5, 10, 10}, theme.text_tertiary(), 1.2f);
                    x += 16;
                }
                if (data.paused) {
                    r.symbol(Renderer::Symbol::Pause, Rect{x, inner.cy() - 6, 12, 12}, theme.warning());
                    x += 18;
                }
                if (data.pinned) r.symbol(Renderer::Symbol::Pin, Rect{x, inner.cy() - 6, 12, 12}, theme.accent(), 1.2f);
            }
        }
    }
    r.pop_clip();
    scroll.paint(r, Rect{bounds_.right() - 6, body_.y + 2, 4, body_.h - 4});
}

void Table::mouse_move(const MouseEvent &e) {
    hovered = row_at(e.y);
    hovered_header_ = -1;
    if (show_header && e.y >= bounds_.y && e.y < bounds_.y + kHeaderHeight) hovered_header_ = column_at(e.x);
}

void Table::mouse_leave() {
    hovered = -1;
    hovered_header_ = -1;
}

bool Table::mouse_down(const MouseEvent &e, bool right) {
    if (!bounds_.contains(e.x, e.y)) {
        focused = false;
        return false;
    }
    focused = true;
    if (show_header && e.y < bounds_.y + kHeaderHeight) {
        if (right) return true;
        const int column = column_at(e.x);
        if (column >= 0 && columns[static_cast<size_t>(column)].sortable) {
            const Column &c = columns[static_cast<size_t>(column)];
            if (sort_column == c.id)
                sort_descending = !sort_descending;
            else {
                sort_column = c.id;
                sort_descending = c.align == HAlign::Right;  // numbers start high to low
            }
            if (on_sort) on_sort(c.id);
        }
        return true;
    }
    const int row = row_at(e.y);
    if (row < 0) {
        select(-1);
        return true;
    }
    // The expander toggle sits at the start of the first column.
    if (!right && expander && expander(row) != 0) {
        const int depth = indent ? indent(row) : 0;
        const float x0 = column_x(0) + kCellPad + depth * 20.0f;
        if (e.x >= x0 - 4 && e.x < x0 + 16) {
            if (on_toggle) on_toggle(row);
            return true;
        }
    }
    // A switch cell flips on click, without changing the selection.
    if (!right && on_switch && this->cell) {
        const int column = column_at(e.x);
        if (column >= 0 && this->cell(row, columns[static_cast<size_t>(column)].id).toggle) {
            on_switch(row, columns[static_cast<size_t>(column)].id);
            return true;
        }
    }
    select(row);
    if (right && on_context) on_context(row, e.x, e.y);
    return true;
}

void Table::double_click(const MouseEvent &e) {
    const int row = row_at(e.y);
    if (row >= 0 && on_activate) on_activate(row);
}

void Table::wheel(const MouseEvent &e) {
    if (!bounds_.contains(e.x, e.y)) return;
    scroll.wheel(e.wheel, row_height * 3);
}

void Table::select(int row) {
    const int count = row_count ? row_count() : 0;
    if (row >= count) row = count - 1;
    if (row < -1) row = -1;
    selected = row;
    if (row >= 0) ensure_visible(row);
    if (on_select) on_select(row);
}

void Table::ensure_visible(int row) { scroll.reveal(row * row_height, row_height); }

bool Table::key(const KeyEvent &e) {
    if (!focused) return false;
    const int count = row_count ? row_count() : 0;
    const int page_rows = std::max(1, static_cast<int>(body_.h / row_height) - 1);
    switch (e.vk) {
        case VK_DOWN: select(std::min(count - 1, selected + 1)); return true;
        case VK_UP: select(std::max(0, selected - 1)); return true;
        case VK_HOME: select(count ? 0 : -1); return true;
        case VK_END: select(count - 1); return true;
        case VK_NEXT: select(std::min(count - 1, std::max(0, selected) + page_rows)); return true;
        case VK_PRIOR: select(std::max(0, selected - page_rows)); return true;
        case VK_RETURN:
            if (selected >= 0 && on_activate) on_activate(selected);
            return true;
        case VK_RIGHT:
            if (selected >= 0 && expander && expander(selected) == 1 && on_toggle) on_toggle(selected);
            return true;
        case VK_LEFT:
            if (selected >= 0 && expander && expander(selected) == 2 && on_toggle) on_toggle(selected);
            return true;
        case VK_APPS:
            if (selected >= 0 && on_context) {
                const Rect rr = row_rect(selected);
                on_context(selected, rr.x + 40, rr.cy());
            }
            return true;
        default: return false;
    }
}

}  // namespace procyon::ui
