// Screen patterns shared by every page: header, panels, live chart, stat grids, banners, buttons.
#include <algorithm>
#include <cmath>

#include "pages.hpp"

namespace procyon::ui {

Rect screen_area(const Rect &bounds) {
    Rect area = bounds.inset(tokens::space::xxl, tokens::space::lg, tokens::space::xxl, tokens::space::xxl);
    if (area.w > 1280) area.w = 1280;
    return area;
}

Rect page_header(Renderer &r, const Rect &bounds, std::wstring_view title, std::wstring_view subtitle,
                 std::optional<MetricKind> icon, Rect *trailing) {
    const Theme &theme = r.theme();
    Rect area = bounds;
    Rect header = area.take_top(48);
    if (icon) {
        const Rect i = header.take_left(40 + tokens::space::md);
        r.metric_icon(Rect{i.x, i.y + 4, 40, 40}, *icon);
    }
    TextStyle t;
    t.font = Font::Title;
    t.valign = VAlign::Top;
    const float title_w = r.measure(title, Font::Title);
    r.text(title, Rect{header.x, header.y, header.w, 28}, t, theme.text());
    TextStyle s;
    s.font = Font::Body;
    s.valign = VAlign::Top;
    const float subtitle_w = subtitle.empty() ? 0 : r.measure(subtitle, Font::Body);
    r.text(subtitle, Rect{header.x, header.y + 29, header.w, 18}, s, theme.text_secondary());
    if (trailing) {
        const float text_w = std::max(title_w, subtitle_w) + tokens::space::lg;
        *trailing = Rect{header.x + text_w, header.y, std::max(0.0f, header.w - text_w), header.h};
    }
    area.take_top(kSectionGap);
    return area;
}

Rect panel(Renderer &r, const Rect &card, std::wstring_view caption, std::optional<Renderer::Symbol> symbol,
           std::optional<MetricKind> tint, Rect *accessory) {
    r.card(card, tint);
    Rect inner = card.inset(tokens::space::lg, tokens::space::lg);
    Rect row = inner.take_top(20);
    if (accessory) *accessory = row;
    r.panel_caption(row, caption, symbol);
    inner.take_top(tokens::space::md);
    return inner;
}

void live_chart(Renderer &r, const Rect &bounds, const std::vector<ChartSeries> &series, float max, MetricKind kind,
                const std::function<std::wstring(float)> &axis_label) {
    const Theme &theme = r.theme();
    const tokens::MetricStyle &style = metric_style(kind);
    Rect area = bounds;
    // Legend: capsule swatch, label, value.
    Rect legend = area.take_top(18);
    float x = legend.x;
    for (const ChartSeries &s : series) {
        const D2D1_COLOR_F color = s.color ? *s.color : rgba(s.secondary ? style.end : style.start);
        r.fill_round(Rect{x, legend.cy() - 2, 10, 4}, tokens::radius::pill, color);
        x += 10 + tokens::space::xs + 2;
        TextStyle label;
        label.font = Font::Label;
        const float lw = r.measure(s.label, Font::Label);
        r.text(s.label, Rect{x, legend.y, lw + 2, legend.h}, label, theme.text_secondary());
        x += lw + tokens::space::xs + 2;
        TextStyle value;
        value.font = Font::Headline;
        value.tabular = true;
        const float vw = r.measure(s.value, Font::Headline);
        r.text(s.value, Rect{x, legend.y, vw + 2, legend.h}, value, theme.text());
        x += vw + tokens::space::lg;
    }
    area.take_top(tokens::space::md);
    Rect footer = area.take_bottom(14);
    area.take_bottom(tokens::space::md);
    Rect axis = area.take_right(44);
    area.take_right(tokens::space::sm);
    const Rect chart = area;
    const float lw = 2;
    TextStyle axis_style;
    axis_style.font = Font::Caption;
    axis_style.tabular = true;
    axis_style.valign = VAlign::Top;
    r.text(axis_label(max), Rect{axis.x, chart.y, axis.w, 14}, axis_style, theme.text_tertiary());
    r.text(axis_label(max / 2), Rect{axis.x, chart.cy() - 7, axis.w, 14}, axis_style, theme.text_tertiary());
    r.text(axis_label(0), Rect{axis.x, chart.bottom() - 14, axis.w, 14}, axis_style, theme.text_tertiary());
    // Grid once, then every series over it (the first fills an area).
    Renderer::SparklineOptions grid_only;
    grid_only.grid = true;
    grid_only.line_width = lw;
    r.sparkline(chart, nullptr, 0, kHistoryWindow, max, kind, grid_only);
    for (size_t i = 0; i < series.size(); ++i) {
        const ChartSeries &s = series[i];
        if (!s.series) continue;  // legend-only entry (a peak)
        Renderer::SparklineOptions options;
        options.area = !s.secondary && i == 0;
        options.end_dot = true;
        options.halo = true;
        options.line_width = lw;
        D2D1_COLOR_F color = s.color ? *s.color : rgba(s.secondary ? style.end : style.start);
        options.color_override = &color;
        r.sparkline(chart, s.series->data(), s.series->size(), kHistoryWindow, max, kind, options);
    }
    TextStyle foot;
    foot.font = Font::Caption;
    r.text(L"60 seconds", footer, foot, theme.text_tertiary());
    foot.halign = HAlign::Right;
    r.text(L"now", Rect{footer.x, footer.y, chart.w, footer.h}, foot, theme.text_tertiary());
}

float value_text(Renderer &r, const Rect &bounds, std::wstring_view value, std::wstring_view unit, Font font,
                 D2D1_COLOR_F color, HAlign align) {
    TextStyle big;
    big.font = font;
    big.tabular = true;
    big.trim = false;
    big.valign = VAlign::Bottom;
    const float vw = r.measure(value, font);
    const float uw = unit.empty() ? 0 : r.measure(unit, Font::Headline) + 3;
    float x = bounds.x;
    if (align == HAlign::Right)
        x = bounds.right() - vw - uw;
    else if (align == HAlign::Center)
        x = bounds.cx() - (vw + uw) / 2;
    r.text(value, Rect{x, bounds.y, vw + 4, bounds.h}, big, color);
    if (!unit.empty()) {
        // The unit sits on the value's baseline: a little above the bottom of the big glyphs.
        TextStyle unit_style;
        unit_style.font = Font::Headline;
        unit_style.valign = VAlign::Bottom;
        r.text(unit, Rect{x + vw + 3, bounds.y, uw + 4, bounds.h - std::round(r.line_height(font) * 0.16f)}, unit_style,
               r.theme().text_secondary());
    }
    return vw + uw;
}

namespace {
constexpr float kStatPitch = 66;  // label + value + detail, plus the grid's row spacing
}

float stat_grid_height(float width, size_t count, float min_column, int fixed_columns) {
    const int columns = fixed_columns > 0 ? fixed_columns : std::max(1, static_cast<int>(width / min_column));
    const int rows = (static_cast<int>(count) + columns - 1) / columns;
    return rows * kStatPitch - tokens::space::lg;
}

float stat_grid(Renderer &r, const Rect &bounds, const std::vector<Stat> &stats, float min_column, int fixed_columns) {
    const Theme &theme = r.theme();
    if (stats.empty()) return 0;
    const int columns = fixed_columns > 0 ? fixed_columns : std::max(1, static_cast<int>(bounds.w / min_column));
    const float column_w = (bounds.w - tokens::space::md * (columns - 1)) / columns;
    for (size_t i = 0; i < stats.size(); ++i) {
        const Stat &s = stats[i];
        const int col = static_cast<int>(i) % columns, row = static_cast<int>(i) / columns;
        Rect cell{bounds.x + col * (column_w + tokens::space::md), bounds.y + row * kStatPitch, column_w, kStatPitch};
        TextStyle label;
        label.font = Font::Label;
        label.valign = VAlign::Top;
        r.text(s.label, Rect{cell.x, cell.y, cell.w, 14}, label, theme.text_secondary());
        Rect value_rect{cell.x, cell.y + 17, cell.w, 22};
        if (s.tint) {
            r.fill_circle(value_rect.x + 3.5f, value_rect.cy(), 3.5f, *s.tint);
            value_rect.x += 13;
            value_rect.w -= 13;
        }
        TextStyle value;
        value.font = Font::Stat;
        value.tabular = true;
        r.text(s.value, value_rect, value, theme.text());
        if (!s.detail.empty()) {
            TextStyle detail;
            detail.font = Font::Caption;
            detail.valign = VAlign::Top;
            r.text(s.detail, Rect{cell.x, cell.y + 42, cell.w, 14}, detail, theme.text_tertiary());
        }
    }
    return stat_grid_height(bounds.w, stats.size(), min_column, fixed_columns);
}

Banner action_banner(Renderer &r, const Rect &bounds, std::wstring_view title, std::wstring_view message,
                     std::wstring_view button_label, Renderer::Tone tone, bool button_hovered) {
    const Theme &theme = r.theme();
    const D2D1_COLOR_F color = Renderer::tone_color(theme, tone);
    Banner banner;
    banner.bounds = bounds;
    r.fill_round(bounds, tokens::radius::lg, with_alpha(color, 0.07f));
    r.stroke_round(bounds, tokens::radius::lg, with_alpha(color, 0.22f));
    Rect inner = bounds.inset(tokens::space::md, tokens::space::md);
    const Rect tile = inner.take_left(32);
    r.fill_round(Rect{tile.x, tile.cy() - 16, 32, 32}, tokens::radius::sm + 2, with_alpha(color, 0.14f));
    r.symbol(tone == Renderer::Tone::Warning ? Renderer::Symbol::Warning : Renderer::Symbol::Lock,
             Rect{tile.x + 8, tile.cy() - 8, 16, 16}, color, 1.5f);
    inner.take_left(tokens::space::md);
    if (!button_label.empty()) {
        const float w = r.measure(button_label, Font::Headline) + tokens::space::md * 2;
        const Rect b = inner.take_right(w);
        const Rect rect{b.x, b.cy() - 14, w, 28};
        r.fill_round(rect, tokens::radius::sm + 1, button_hovered ? mix(color, theme.text(), 0.1f) : color);
        TextStyle style;
        style.font = Font::Headline;
        style.halign = HAlign::Center;
        r.text(button_label, rect, style, D2D1::ColorF(D2D1::ColorF::White));
        banner.button = rect;
        inner.take_right(tokens::space::md);
    }
    TextStyle t;
    t.font = Font::Headline;
    t.valign = VAlign::Top;
    r.text(title, Rect{inner.x, inner.cy() - 17, inner.w, 18}, t, theme.text());
    TextStyle m;
    m.font = Font::Label;
    m.valign = VAlign::Top;
    r.text(message, Rect{inner.x, inner.cy() + 2, inner.w, 16}, m, theme.text_secondary());
    return banner;
}

Banner info_banner(Renderer &r, const Rect &bounds, std::wstring_view message, std::wstring_view button_label,
                   Renderer::Tone tone) {
    const Theme &theme = r.theme();
    const D2D1_COLOR_F color = Renderer::tone_color(theme, tone);
    Banner banner;
    banner.bounds = bounds;
    r.fill_round(bounds, tokens::radius::md, with_alpha(color, 0.08f));
    r.stroke_round(bounds, tokens::radius::md, with_alpha(color, 0.18f));
    Rect inner = bounds.inset(tokens::space::md, 0);
    const Rect icon = inner.take_left(16 + tokens::space::sm);
    r.symbol(tone == Renderer::Tone::Warning || tone == Renderer::Tone::Danger ? Renderer::Symbol::Warning
                                                                               : Renderer::Symbol::Info,
             Rect{icon.x, icon.cy() - 7, 14, 14}, color, 1.5f);
    if (!button_label.empty()) {
        const float w = r.measure(button_label, Font::BodyMedium) + tokens::space::lg * 2;
        const Rect b = inner.take_right(w);
        banner.button = button(r, b.x, b.cy() - kControlHeight / 2, button_label, true, false, false, w);
        inner.w -= tokens::space::sm;
    }
    TextStyle style;
    style.font = Font::Body;
    r.text(message, inner, style, theme.text_secondary());
    return banner;
}

Rect button(Renderer &r, float x, float y, std::wstring_view label, bool primary, bool destructive, bool hovered,
            float min_width, Font font) {
    const Theme &theme = r.theme();
    const float width = std::max(min_width, r.measure(label, font) + tokens::space::md * 2);
    const Rect b{x, y, width, kControlHeight};
    D2D1_COLOR_F fill = theme.surface_raised();
    D2D1_COLOR_F text = theme.text();
    if (destructive) {
        fill = theme.danger();
        text = D2D1::ColorF(D2D1::ColorF::White);
    } else if (primary) {
        fill = theme.accent();
        text = D2D1::ColorF(D2D1::ColorF::White);
    }
    if (hovered)
        fill = mix(fill, theme.dark ? D2D1::ColorF(D2D1::ColorF::White) : D2D1::ColorF(D2D1::ColorF::Black), 0.08f);
    r.fill_round(b, tokens::radius::sm + 1, fill);
    if (!primary && !destructive) r.stroke_round(b, tokens::radius::sm + 1, theme.border_strong());
    TextStyle style;
    style.font = font;
    style.halign = HAlign::Center;
    r.text(label, b, style, text);
    return b;
}

Rect end_task_button(Renderer &r, float right, float y, bool enabled, bool hovered) {
    const Theme &theme = r.theme();
    const float width = r.measure(L"End Task", Font::Headline) + 14 + tokens::space::md * 2 + 4;
    const Rect b{right - width, y, width, kControlHeight};
    if (enabled) {
        // Soft red shadow under the active button.
        for (int i = 3; i >= 1; --i)
            r.fill_round(b.offset(0, 2).inset(-i * 1.5f, -i * 1.5f), tokens::radius::sm + 1 + i * 1.5f,
                         with_alpha(theme.danger(), 0.35f / 3));
        r.fill_gradient_round(b, tokens::radius::sm + 1,
                              hovered ? mix(theme.danger(), theme.text(), 0.1f) : theme.danger(),
                              with_alpha(theme.danger(), 0.85f));
    } else {
        r.fill_round(b, tokens::radius::sm + 1, theme.track());
    }
    const D2D1_COLOR_F fg = enabled ? D2D1::ColorF(D2D1::ColorF::White) : theme.text_tertiary();
    r.fill_circle(b.x + tokens::space::md + 7, b.cy(), 7, with_alpha(fg, enabled ? 0.25f : 0.15f));
    r.symbol(Renderer::Symbol::Close, Rect{b.x + tokens::space::md + 3, b.cy() - 4, 8, 8}, fg, 1.6f);
    TextStyle style;
    style.font = Font::Headline;
    r.text(L"End Task", Rect{b.x + tokens::space::md + 18, b.y, b.w - tokens::space::md - 18, b.h}, style, fg);
    return b;
}

void empty_state(Renderer &r, const Rect &bounds, std::wstring_view title, std::wstring_view message) {
    const Theme &theme = r.theme();
    TextStyle t;
    t.font = Font::Headline;
    t.halign = HAlign::Center;
    r.text(title, Rect{bounds.x, bounds.cy() - 24, bounds.w, 22}, t, theme.text());
    TextStyle m;
    m.font = Font::Body;
    m.halign = HAlign::Center;
    r.text(message, Rect{bounds.x, bounds.cy() + 2, bounds.w, 20}, m, theme.text_secondary());
}

std::wstring process_display_name(const pc_process &p) { return fmt::from_utf8(p.name[0] ? p.name : p.app_name); }

std::wstring display_model(const pc_system_info &info) {
    std::wstring model = fmt::from_utf8(info.model_name);
    // Manufacturer strings carry legal suffixes that don't belong in a headline.
    for (const wchar_t *suffix : {L" Co., Ltd.", L" Co.,Ltd.", L" Co., Ltd", L", Inc.", L" Inc.", L" Corporation",
                                  L" Corp.", L" Ltd.", L" LLC", L" Limited", L" Technology", L" Technologies"}) {
        const std::wstring tail = suffix;
        if (model.size() > tail.size() && model.compare(model.size() - tail.size(), tail.size(), tail) == 0)
            model.resize(model.size() - tail.size());
    }
    if (model.empty()) model = fmt::from_utf8(info.model_id);
    if (model.empty()) model = fmt::from_utf8(info.hostname);
    return model;
}

float top_apps_height(size_t rows) { return kPanelChrome + std::max<size_t>(rows, 1) * kTopAppRowHeight; }

std::vector<TopAppRow> top_apps_panel(Host &host, const Rect &card, std::wstring_view title, Renderer::Symbol symbol,
                                      MetricKind kind, int32_t column, bool tint, size_t max_rows,
                                      const std::function<std::wstring(const Row &)> &value,
                                      const std::function<float(const Row &)> &fraction) {
    Renderer &r = host.renderer();
    const Theme &theme = r.theme();
    std::vector<TopAppRow> hits;
    Rect inner = panel(r, card, title, symbol, tint ? std::optional(kind) : std::nullopt);
    ViewQuery query;
    query.mode = PC_VIEW_GROUPED;
    query.sort_column = column;
    query.descending = true;
    query.limit = static_cast<int32_t>(max_rows);
    const std::vector<Row> rows = host.store().build_view(query);
    const Snapshot &s = host.store().snapshot();
    const float mx = host.mouse_x(), my = host.mouse_y();
    float y = inner.y;
    size_t shown = 0;
    for (const Row &row : rows) {
        if (row.depth > 0 || shown >= max_rows) continue;
        const pc_process *p = row.process_index >= 0 && row.process_index < static_cast<int32_t>(s.processes.size())
                                  ? &s.processes[static_cast<size_t>(row.process_index)]
                                  : nullptr;
        const std::wstring name = row.is_group() ? row.group_name
                                  : p            ? fmt::from_utf8(p->app_name[0] ? p->app_name : p->name)
                                                 : L"";
        const int32_t pid = row.is_group() ? row.group_pid : p ? p->pid : 0;
        const std::string app_id = row.is_group() ? row.group_id : p ? std::string(p->app_id) : std::string();
        const Rect rr{inner.x - tokens::space::sm, y, inner.w + tokens::space::sm * 2, kTopAppRowHeight};
        if (rr.contains(mx, my)) r.fill_round(rr, tokens::radius::sm, with_alpha(theme.text(), 0.05f));
        Rect line = rr.inset(tokens::space::sm, tokens::space::xs + 1);
        // App tile: the metric icon stands in for the app icon macOS shows.
        const Rect icon = line.take_left(22 + tokens::space::sm + 2);
        r.fill_round(Rect{icon.x, icon.y + 1, 22, 22}, tokens::radius::sm, theme.surface_sunken());
        r.glyph(kind, Rect{icon.x + 5, icon.y + 6, 12, 12}, theme.text_tertiary());
        Rect top = line;
        top.h = 20;
        TextStyle headline;
        headline.font = Font::Headline;
        headline.tabular = true;
        headline.halign = HAlign::Right;
        const std::wstring v = value(row);
        // Tabular figures run a little wider than the measured proportional ones.
        const float vw = r.measure(v, Font::Headline) + 6;
        r.text(v, Rect{top.right() - vw - 2, top.y, vw + 2, top.h}, headline,
               v == fmt::unavailable ? theme.text_tertiary() : theme.text());
        headline.halign = HAlign::Left;
        const float nw =
            r.text(name, Rect{top.x, top.y, top.w - vw - tokens::space::md, top.h}, headline, theme.text());
        if (row.process_count > 1) {
            TextStyle count;
            count.font = Font::Caption;
            r.text(std::to_wstring(row.process_count), Rect{top.x + nw + tokens::space::md, top.y, 40, top.h}, count,
                   theme.text_tertiary());
        }
        const float f = fraction(row);
        r.usage_bar(Rect{line.x, line.bottom() - 6, line.w, 4}, f < 0 ? 0 : f, kind);
        hits.push_back({rr, pid, app_id});
        y += kTopAppRowHeight;
        ++shown;
    }
    if (!shown) {
        TextStyle style;
        style.font = Font::Body;
        style.halign = HAlign::Center;
        r.text(host.store().has_snapshot() ? L"No app is busy right now." : L"Collecting…",
               Rect{inner.x, inner.y, inner.w, kTopAppRowHeight}, style, theme.text_tertiary());
    }
    return hits;
}

}  // namespace procyon::ui
