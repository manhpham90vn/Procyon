// CPU, Memory, Disk, Network, GPU and Battery: the macOS PerformanceViews, panel for panel.
#include <algorithm>
#include <cmath>

#include "pages.hpp"

namespace procyon::ui {
namespace {

std::wstring percent_axis(float v) { return fmt::number(v * 100, 0) + L"%"; }
std::wstring rate_axis(float v) { return v <= 0 ? L"0 B/s" : fmt::rate(v); }

// Lays a screen out as a vertical stack inside a scrollable area.
struct Stack {
    Rect area;
    float y;
    Rect next(float height) {
        Rect r{area.x, y, area.w, height};
        y += height + kSectionGap;
        return r;
    }
};

// Two stacked rates for page headers (read/write, down/up).
void rate_headline(Renderer &r, const Rect &trailing, double primary, double secondary, const wchar_t *primary_label,
                   const wchar_t *secondary_label, MetricKind kind) {
    const Theme &theme = r.theme();
    const tokens::MetricStyle &style = metric_style(kind);
    float right = trailing.right();
    const auto entry = [&](const wchar_t *label, double value, D2D1_COLOR_F color) {
        const auto parts = fmt::split_unit(fmt::rate(value));
        const float vw = r.measure(parts.first, Font::Metric) + r.measure(parts.second, Font::Headline) + 3;
        const float lw = r.measure(label, Font::Label) + 11;
        const float w = std::max(vw, lw);
        value_text(r, Rect{right - w, trailing.y + 16, w, 32}, parts.first, parts.second, Font::Metric, theme.text(),
                   HAlign::Right);
        TextStyle l;
        l.font = Font::Label;
        l.halign = HAlign::Right;
        l.valign = VAlign::Top;
        r.text(label, Rect{right - w, trailing.y, w, 16}, l, theme.text_secondary());
        r.fill_circle(right - lw + 3.5f, trailing.y + 8, 3.5f, color);
        right -= w + tokens::space::xl;
    };
    entry(secondary_label, secondary, rgba(style.end));
    entry(primary_label, primary, rgba(style.start));
}

class PerformancePage : public Page {
public:
    explicit PerformancePage(PageId id) : id_(id) {}
    PageId id() const override { return id_; }
    std::wstring title() const override {
        switch (id_) {
            case PageId::Cpu: return L"CPU";
            case PageId::Memory: return L"Memory";
            case PageId::Disk: return L"Disk";
            case PageId::Network: return L"Network";
            case PageId::Gpu: return L"GPU";
            case PageId::Battery: return L"Battery";
            default: return L"";
        }
    }

    void activate(Host &host) override {
        if (id_ == PageId::Disk) volumes_ = host.store().volumes();
        if (id_ == PageId::Battery) assertions_ = host.store().power_assertions();
    }
    void tick(Host &host) override {
        if (host.store().ticks() % 5 == 0) activate(host);
    }

    void paint(Host &host, const Rect &bounds) override {
        app_rows_.clear();
        switch (id_) {
            case PageId::Cpu: paint_cpu(host, bounds); break;
            case PageId::Memory: paint_memory(host, bounds); break;
            case PageId::Disk: paint_disk(host, bounds); break;
            case PageId::Network: paint_network(host, bounds); break;
            case PageId::Gpu: paint_gpu(host, bounds); break;
            case PageId::Battery: paint_battery(host, bounds); break;
            default: break;
        }
    }

    void wheel(Host &, const MouseEvent &e) override { scroll_.wheel(e.wheel); }
    void mouse_down(Host &host, const MouseEvent &e, bool right) override {
        if (right) return;
        for (const TopAppRow &row : app_rows_)
            if (row.rect.contains(e.x, e.y)) host.show_in_processes(row.app_id, row.pid);
    }

private:
    // Whether the temperature history holds one unchanging value (a firmware placeholder, not a
    // sensor): a sensor jitters within seconds at tenths of a degree.
    static bool fixed_temperature(const Series &series) {
        if (series.size() < 20) return false;
        const float *values = series.data();
        const size_t start = series.size() > kHistoryWindow ? series.size() - kHistoryWindow : 0;
        for (size_t i = start + 1; i < series.size(); ++i)
            if (values[i] != values[start]) return false;
        return true;
    }

    std::wstring core_summary(const pc_system_info &info) const {
        if (info.performance_cores > 0 && info.efficiency_cores > 0)
            return std::to_wstring(info.performance_cores) + L"P + " + std::to_wstring(info.efficiency_cores) +
                   L"E cores";
        return std::to_wstring(info.physical_cores) + L" cores";
    }

    void top_apps(Host &host, Stack &stack, const wchar_t *title, Renderer::Symbol symbol, MetricKind kind,
                  int32_t column, const std::function<std::wstring(const Row &)> &value,
                  const std::function<float(const Row &)> &fraction) {
        const Rect card = stack.next(top_apps_height(8));
        for (const TopAppRow &row : top_apps_panel(host, card, title, symbol, kind, column, true, 8, value, fraction))
            app_rows_.push_back(row);
    }

    void finish(Renderer &r, const Rect &bounds, float end_y) {
        r.pop_clip();
        scroll_.viewport = bounds.h;
        scroll_.content = end_y + scroll_.offset - bounds.y;
        scroll_.clamp();
    }

    Stack begin(Renderer &r, const Rect &bounds, Rect &area) {
        area = screen_area(bounds);
        r.push_clip(bounds);
        area.y -= scroll_.offset;
        return Stack{area, area.y};
    }

    void paint_cpu(Host &host, const Rect &bounds) {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        Store &store = host.store();
        const Snapshot &s = store.snapshot();
        const History &h = store.history();
        const pc_system_info &info = store.system_info();
        Rect area;
        Stack stack = begin(r, bounds, area);
        Rect trailing;
        stack.area = page_header(r, area, L"CPU", fmt::from_utf8(info.cpu_brand) + L" · " + core_summary(info),
                                 MetricKind::Cpu, &trailing);
        stack.y = stack.area.y;
        value_text(r, Rect{trailing.x, trailing.y + 6, trailing.w, 40}, fmt::number(s.cpu_usage * 100, 0), L"%",
                   Font::Display, theme.text(), HAlign::Right);

        Rect inner =
            panel(r, stack.next(kPanelChrome + 240), L"Utilization", Renderer::Symbol::Waveform, MetricKind::Cpu);
        live_chart(r, inner,
                   {{&h.cpu, L"Total", fmt::percent(s.cpu_usage)},
                    {&h.cpu_system, L"System", fmt::percent(s.cpu_system), true}},
                   1, MetricKind::Cpu, percent_axis);

        // Windows has no user-mode way to the die sensor: the hottest ACPI zone stands in. A zone that
        // has never moved over the last minute is the board's fixed reading, and says so.
        const bool fixed_zone = fixed_temperature(h.cpu_temperature);
        const wchar_t *zone_label = fixed_zone ? L"Board zone (fixed reading)" : L"Hottest ACPI zone";
        if (store.has(PC_CAP_TEMPERATURE) && s.cpu_temperature >= 0) {
            inner = panel(r, stack.next(kPanelChrome + 160), L"Temperature", Renderer::Symbol::Thermo);
            D2D1_COLOR_F warning = theme.warning();
            const bool f = host.settings().fahrenheit;
            const float peak = std::max(h.cpu_temperature.max_recent(), static_cast<float>(s.cpu_temperature));
            live_chart(r, inner,
                       {{&h.cpu_temperature, zone_label, fmt::temperature(s.cpu_temperature, f), false, warning},
                        {nullptr, L"Peak", fmt::temperature(peak, f), false, theme.danger()}},
                       110, MetricKind::Cpu, [f](float v) { return fmt::temperature(v, f); });
        }

        const double uptime = s.timestamp > 0 && info.boot_time > 0 ? s.timestamp - info.boot_time : 0;
        const tokens::MetricStyle &style = tokens::metric::cpu;
        std::vector<Stat> stats = {
            {L"Utilization", fmt::percent(s.cpu_usage, 1), L"", rgba(style.start)},
            {L"User", fmt::percent(s.cpu_user, 1), L""},
            {L"System", fmt::percent(s.cpu_system, 1), L"", rgba(style.end)},
            {L"Idle", fmt::percent(std::max(0.0, 1 - s.cpu_usage), 1), L""},
            {L"Processes", fmt::count(s.process_count), L""},
            {L"Threads", fmt::count(s.thread_count), L""},
            {L"Uptime", fmt::duration(uptime), L""},
            {L"Cores", core_summary(info), std::to_wstring(info.logical_cores) + L" logical"},
        };
        if (info.cpu_frequency_hz)
            stats.push_back({L"Base speed", fmt::number(info.cpu_frequency_hz / 1e9, 2) + L" GHz", L""});
        if (store.has(PC_CAP_TEMPERATURE) && s.cpu_temperature >= 0)
            stats.push_back({L"Temperature", fmt::temperature(s.cpu_temperature, host.settings().fahrenheit),
                             fixed_zone ? L"Board's ACPI zone, not the CPU die" : L"Hottest ACPI zone"});
        const float content_w = stack.area.w - tokens::space::lg * 2;
        inner = panel(r, stack.next(kPanelChrome + stat_grid_height(content_w, stats.size())), L"Details",
                      Renderer::Symbol::Bullets);
        stat_grid(r, inner, stats);

        // Cores: performance first, numbered within their kind.
        const int cores = static_cast<int>(s.core_usage.size());
        if (cores > 0) {
            const bool hybrid = store.has(PC_CAP_HYBRID_CORES);
            std::vector<int> order(static_cast<size_t>(cores));
            for (int i = 0; i < cores; ++i) order[static_cast<size_t>(i)] = i;
            const auto kind_of = [&](int i) { return i < 256 ? info.core_kinds[i] : PC_CORE_UNKNOWN; };
            const auto rank = [&](int i) {
                return kind_of(i) == PC_CORE_PERFORMANCE ? 0 : kind_of(i) == PC_CORE_EFFICIENCY ? 1 : 2;
            };
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return rank(a) < rank(b); });
            const int columns =
                std::max(1, static_cast<int>((content_w + tokens::space::sm) / (128 + tokens::space::sm)));
            const int rows = (cores + columns - 1) / columns;
            const float tile_h = 70, gap = tokens::space::sm;
            Rect accessory;
            inner = panel(r, stack.next(kPanelChrome + rows * (tile_h + gap) - gap), L"Cores", Renderer::Symbol::Grid,
                          std::nullopt, &accessory);
            if (hybrid) {
                // Legend: P Performance  E Efficiency, right-aligned in the caption row.
                float x = accessory.right();
                TextStyle cap;
                cap.font = Font::Caption;
                cap.halign = HAlign::Right;
                const float ew = r.measure(L"Efficiency", Font::Caption);
                r.text(L"Efficiency", Rect{x - ew, accessory.y, ew, accessory.h}, cap, theme.text_secondary());
                x -= ew + tokens::space::xs;
                x -= r.badge(0, 0, L"E", Renderer::Tone::Success, true);
                r.badge(x, accessory.cy() - 8.5f, L"E", Renderer::Tone::Success);
                x -= tokens::space::sm;
                const float pw = r.measure(L"Performance", Font::Caption);
                r.text(L"Performance", Rect{x - pw, accessory.y, pw, accessory.h}, cap, theme.text_secondary());
                x -= pw + tokens::space::xs;
                x -= r.badge(0, 0, L"P", Renderer::Tone::Accent, true);
                r.badge(x, accessory.cy() - 8.5f, L"P", Renderer::Tone::Accent);
            }
            const float tile_w = (inner.w - gap * (columns - 1)) / columns;
            int numbers[3] = {0, 0, 0};
            for (int n = 0; n < cores; ++n) {
                const int i = order[static_cast<size_t>(n)];
                const int col = n % columns, row = n / columns;
                const Rect tile{inner.x + col * (tile_w + gap), inner.y + row * (tile_h + gap), tile_w, tile_h};
                r.fill_round(tile, tokens::radius::md, theme.surface_sunken());
                Rect t = tile.inset(tokens::space::sm, tokens::space::sm);
                Rect head = t.take_top(17);
                float x = head.x;
                const int kind = kind_of(i);
                if (hybrid && kind != PC_CORE_UNKNOWN) {
                    const bool perf = kind == PC_CORE_PERFORMANCE;
                    x += r.badge(x, head.y, perf ? L"P" : L"E",
                                 perf ? Renderer::Tone::Accent : Renderer::Tone::Success) +
                         tokens::space::xs;
                }
                TextStyle label;
                label.font = Font::Caption;
                r.text(L"Core " + std::to_wstring(++numbers[rank(i)]), Rect{x, head.y, head.w - (x - head.x), head.h},
                       label, theme.text_secondary());
                TextStyle value;
                value.font = Font::Label;
                value.halign = HAlign::Right;
                value.tabular = true;
                r.text(fmt::percent(s.core_usage[static_cast<size_t>(i)]), head, value, theme.text());
                t.take_top(tokens::space::xs);
                const Rect spark = t.take_top(34);
                if (i < static_cast<int>(h.cores.size())) {
                    Renderer::SparklineOptions options;
                    options.end_dot = true;
                    options.halo = false;
                    options.line_width = 1.25f;
                    D2D1_COLOR_F color = rgba(s.core_usage[static_cast<size_t>(i)] > 0.85 ? style.end : style.start);
                    options.color_override = &color;
                    r.sparkline(spark, h.cores[static_cast<size_t>(i)].data(), h.cores[static_cast<size_t>(i)].size(),
                                kHistoryWindow, 1, MetricKind::Cpu, options);
                }
                r.usage_bar(Rect{tile.x + tokens::space::sm, tile.bottom() - 2, tile.w - tokens::space::sm * 2, 2},
                            static_cast<float>(s.core_usage[static_cast<size_t>(i)]), MetricKind::Cpu);
            }
        }

        top_apps(
            host, stack, L"Top Apps", Renderer::Symbol::Flame, MetricKind::Cpu, PC_COLUMN_CPU,
            [](const Row &row) { return fmt::cpu(row.cpu_percent); },
            [](const Row &row) { return static_cast<float>(std::max(0.0, row.cpu_percent) / 100); });
        finish(r, bounds, stack.y);
    }

    void paint_memory(Host &host, const Rect &bounds) {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        Store &store = host.store();
        const Snapshot &s = store.snapshot();
        const History &h = store.history();
        const tokens::MetricStyle &style = tokens::metric::memory;
        const wchar_t *pressure = s.memory_pressure == PC_PRESSURE_CRITICAL  ? L"Critical"
                                  : s.memory_pressure == PC_PRESSURE_WARNING ? L"Elevated"
                                  : s.memory_pressure == PC_PRESSURE_NORMAL  ? L"Normal"
                                                                             : L"Unknown";
        Rect area;
        Stack stack = begin(r, bounds, area);
        Rect trailing;
        stack.area = page_header(
            r, area, L"Memory", fmt::bytes(static_cast<int64_t>(s.memory_total)) + L" installed · Pressure " + pressure,
            MetricKind::Memory, &trailing);
        stack.y = stack.area.y;
        const auto used = fmt::split_unit(fmt::bytes(static_cast<int64_t>(s.memory_used)));
        const std::wstring of = L"of " + fmt::bytes(static_cast<int64_t>(s.memory_total));
        const float ofw = r.measure(of, Font::Body) + tokens::space::xs;
        value_text(r, Rect{trailing.x, trailing.y + 6, trailing.w - ofw, 40}, used.first, used.second, Font::Display,
                   theme.text(), HAlign::Right);
        TextStyle body;
        body.font = Font::Body;
        body.halign = HAlign::Right;
        body.valign = VAlign::Bottom;
        r.text(of, Rect{trailing.right() - ofw + tokens::space::xs, trailing.y + 6, ofw, 34}, body,
               theme.text_tertiary());

        Rect inner =
            panel(r, stack.next(kPanelChrome + 220), L"Memory used", Renderer::Symbol::Waveform, MetricKind::Memory);
        live_chart(r, inner,
                   {{&h.memory_fraction, L"Used",
                     fmt::percent(s.memory_total ? static_cast<double>(s.memory_used) / s.memory_total : 0)}},
                   1, MetricKind::Memory, percent_axis);

        // Composition: a stacked bar and its legend.
        struct Part {
            const wchar_t *title;
            uint64_t value;
            D2D1_COLOR_F color;
        };
        const std::vector<Part> parts = {
            {L"App", s.memory_app, rgba(style.start)},
            {L"Kernel", s.memory_wired, rgba(style.end)},
            {L"Compressed", s.memory_compressed, rgba(tokens::metric::cpu.start)},
            {L"Standby", s.memory_cached, with_alpha(theme.text_tertiary(), 0.6f)},
            {L"Free", s.memory_free, theme.track()},
        };
        const float content_w = stack.area.w - tokens::space::lg * 2;
        const int legend_columns = std::max(1, static_cast<int>(content_w / 130));
        const int legend_rows = (static_cast<int>(parts.size()) + legend_columns - 1) / legend_columns;
        inner = panel(r, stack.next(kPanelChrome + 14 + tokens::space::md + legend_rows * 34 - tokens::space::sm),
                      L"Composition", Renderer::Symbol::Stack);
        double total = 0;
        for (const Part &p : parts) total += static_cast<double>(p.value);
        std::vector<Renderer::Segment> segments;
        for (const Part &p : parts) segments.push_back({total > 0 ? static_cast<float>(p.value / total) : 0, p.color});
        r.stacked_bar(inner.take_top(14), segments.data(), segments.size());
        inner.take_top(tokens::space::md);
        const float legend_w = inner.w / legend_columns;
        for (size_t i = 0; i < parts.size(); ++i) {
            const int col = static_cast<int>(i) % legend_columns, row = static_cast<int>(i) / legend_columns;
            const Rect cell{inner.x + col * legend_w, inner.y + row * 34, legend_w, 30};
            r.fill_round(Rect{cell.x, cell.y + 10, 10, 10}, 3, parts[i].color);
            TextStyle label;
            label.font = Font::Label;
            label.valign = VAlign::Top;
            r.text(parts[i].title, Rect{cell.x + 18, cell.y, cell.w - 18, 14}, label, theme.text_secondary());
            TextStyle value;
            value.font = Font::Headline;
            value.tabular = true;
            value.valign = VAlign::Top;
            r.text(fmt::bytes(static_cast<int64_t>(parts[i].value)), Rect{cell.x + 18, cell.y + 14, cell.w - 18, 16},
                   value, theme.text());
        }

        // Details and the page file, side by side.
        std::vector<Stat> stats = {
            {L"Pressure", pressure, L"",
             s.memory_pressure == PC_PRESSURE_CRITICAL  ? theme.danger()
             : s.memory_pressure == PC_PRESSURE_WARNING ? theme.warning()
                                                        : theme.success()},
            {L"Used", fmt::bytes(static_cast<int64_t>(s.memory_used)),
             fmt::percent(s.memory_total ? static_cast<double>(s.memory_used) / s.memory_total : 0)},
            {L"App memory", fmt::bytes(static_cast<int64_t>(s.memory_app)), L""},
            {L"Kernel (non-paged)", fmt::bytes(static_cast<int64_t>(s.memory_wired)), L""},
            {L"Compressed", fmt::bytes(static_cast<int64_t>(s.memory_compressed)), L""},
            {L"Standby (cached)", fmt::bytes(static_cast<int64_t>(s.memory_cached)), L""},
        };
        const bool wide = stack.area.w >= 720;
        const float swap_w = wide ? std::min(360.0f, stack.area.w * 0.35f) : stack.area.w;
        const float details_w = wide ? stack.area.w - swap_w - tokens::space::lg : stack.area.w;
        const float details_h =
            kPanelChrome + stat_grid_height(details_w - tokens::space::lg * 2, stats.size(), 140, 3);
        const float row_h = wide ? std::max(details_h, kPanelChrome + 120) : details_h;
        const Rect row = stack.next(row_h);
        inner = panel(r, Rect{row.x, row.y, details_w, row_h}, L"Details", Renderer::Symbol::Bullets);
        stat_grid(r, inner, stats, 140, 3);
        const Rect swap_card = wide ? Rect{row.right() - swap_w, row.y, swap_w, row_h} : stack.next(kPanelChrome + 120);
        inner = panel(r, swap_card, L"Page file", Renderer::Symbol::Drive);
        const auto swap = fmt::split_unit(fmt::bytes(static_cast<int64_t>(s.swap_used)));
        const float vw = value_text(r, inner.take_top(32), swap.first, swap.second, Font::Metric, theme.text());
        TextStyle of_label;
        of_label.font = Font::Label;
        of_label.valign = VAlign::Bottom;
        r.text(L"of " + fmt::bytes(static_cast<int64_t>(s.swap_total)),
               Rect{inner.x + vw + tokens::space::xs, inner.y - 32, inner.w - vw, 28}, of_label, theme.text_tertiary());
        inner.take_top(tokens::space::md);
        r.usage_bar(inner.take_top(6), s.swap_total ? static_cast<float>(s.swap_used) / s.swap_total : 0,
                    MetricKind::Memory);
        inner.take_top(tokens::space::md);
        if (inner.h > 20) {
            Renderer::SparklineOptions options;
            options.halo = false;
            D2D1_COLOR_F end = rgba(style.end);
            options.color_override = &end;
            r.sparkline(inner, h.swap_used.data(), h.swap_used.size(), kHistoryWindow,
                        nice_max(std::max(1.0f, h.swap_used.max_recent())), MetricKind::Memory, options);
        }

        const double mem_total = s.memory_total ? static_cast<double>(s.memory_total) : 1;
        top_apps(
            host, stack, L"Top Apps", Renderer::Symbol::Stack, MetricKind::Memory, PC_COLUMN_MEMORY,
            [](const Row &row) { return fmt::bytes(row.memory_bytes); },
            [mem_total](const Row &row) {
                return static_cast<float>(std::max<int64_t>(0, row.memory_bytes) / mem_total);
            });
        finish(r, bounds, stack.y);
    }

    void paint_disk(Host &host, const Rect &bounds) {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        Store &store = host.store();
        const Snapshot &s = store.snapshot();
        const History &h = store.history();
        const tokens::MetricStyle &style = tokens::metric::disk;
        Rect area;
        Stack stack = begin(r, bounds, area);
        Rect trailing;
        stack.area = page_header(r, area, L"Disk", L"All internal and external drives", MetricKind::Disk, &trailing);
        stack.y = stack.area.y;
        rate_headline(r, trailing, s.disk_read_bps, s.disk_write_bps, L"Read", L"Write", MetricKind::Disk);

        const float max = nice_max(std::max(h.disk_read.max_recent(), h.disk_write.max_recent()));
        Rect inner =
            panel(r, stack.next(kPanelChrome + 240), L"Activity", Renderer::Symbol::Waveform, MetricKind::Disk);
        live_chart(r, inner,
                   {{&h.disk_read, L"Read", fmt::rate(s.disk_read_bps)},
                    {&h.disk_write, L"Write", fmt::rate(s.disk_write_bps), true}},
                   max, MetricKind::Disk, rate_axis);

        if (store.has(PC_CAP_TEMPERATURE) && s.disk_temperature >= 0) {
            const bool f = host.settings().fahrenheit;
            const D2D1_COLOR_F tint = rgba(style.end);
            const float peak = std::max(h.disk_temperature.max_recent(), static_cast<float>(s.disk_temperature));
            inner = panel(r, stack.next(kPanelChrome + 160), L"SSD Temperature", Renderer::Symbol::Thermo);
            live_chart(r, inner,
                       {{&h.disk_temperature, L"System drive", fmt::temperature(s.disk_temperature, f), false, tint},
                        {nullptr, L"Peak", fmt::temperature(peak, f), false, theme.danger()}},
                       90, MetricKind::Disk, [f](float v) { return fmt::temperature(v, f); });
        }

        std::vector<Stat> stats = {
            {L"Read", fmt::rate(s.disk_read_bps), L"", rgba(style.start)},
            {L"Write", fmt::rate(s.disk_write_bps), L"", rgba(style.end)},
            {L"Peak read (60s)", fmt::rate(h.disk_read.max_recent()), L""},
            {L"Peak write (60s)", fmt::rate(h.disk_write.max_recent()), L""},
            {L"Read since boot", fmt::bytes(static_cast<int64_t>(s.disk_read_total)), L""},
            {L"Written since boot", fmt::bytes(static_cast<int64_t>(s.disk_write_total)), L""},
        };
        if (store.has(PC_CAP_TEMPERATURE) && s.disk_temperature >= 0)
            stats.push_back({L"Drive temperature", fmt::temperature(s.disk_temperature, host.settings().fahrenheit),
                             L"System drive"});
        const float content_w = stack.area.w - tokens::space::lg * 2;
        inner = panel(r, stack.next(kPanelChrome + stat_grid_height(content_w, stats.size())), L"Details",
                      Renderer::Symbol::Bullets);
        stat_grid(r, inner, stats);

        // Volumes.
        const float pitch = 34 + tokens::space::lg;
        inner = panel(r, stack.next(kPanelChrome + std::max<size_t>(1, volumes_.size()) * pitch - tokens::space::lg),
                      L"Volumes", Renderer::Symbol::Drive);
        if (volumes_.empty()) empty_state(r, inner, L"No volumes", L"");
        for (const pc_volume &vol : volumes_) {
            Rect row = inner.take_top(34);
            inner.take_top(tokens::space::lg);
            const Rect icon = row.take_left(32 + tokens::space::md);
            r.symbol(Renderer::Symbol::Drive, Rect{icon.x + 6, icon.y + 7, 20, 20}, rgba(style.start), 1.6f);
            Rect head = row.take_top(20);
            TextStyle name;
            name.font = Font::Headline;
            const float nw = r.text(fmt::from_utf8(vol.name), head, name, theme.text());
            TextStyle detail;
            detail.font = Font::Caption;
            detail.valign = VAlign::Bottom;
            std::wstring fs = fmt::from_utf8(vol.file_system);
            for (wchar_t &c : fs) c = static_cast<wchar_t>(towupper(c));
            r.text(fmt::from_utf8(vol.mount_point) + L" · " + fs,
                   Rect{head.x + nw + tokens::space::sm, head.y, head.w - nw - 8, head.h - 3}, detail,
                   theme.text_tertiary());
            TextStyle right;
            right.font = Font::Label;
            right.halign = HAlign::Right;
            right.tabular = true;
            r.text(fmt::bytes(static_cast<int64_t>(vol.available_bytes)) + L" available of " +
                       fmt::bytes(static_cast<int64_t>(vol.total_bytes)),
                   head, right, theme.text_secondary());
            row.take_top(tokens::space::xs + 2);
            const uint64_t used = vol.total_bytes > vol.available_bytes ? vol.total_bytes - vol.available_bytes : 0;
            r.usage_bar(row.take_top(8), vol.total_bytes ? static_cast<float>(used) / vol.total_bytes : 0,
                        MetricKind::Disk);
        }

        const float peak = std::max(1.0f, max);
        top_apps(
            host, stack, L"Top Apps", Renderer::Symbol::Drive, MetricKind::Disk, PC_COLUMN_DISK_READ,
            [](const Row &row) {
                if (row.disk_read_bps < 0) return std::wstring(fmt::unavailable);
                return L"↓ " + fmt::rate(row.disk_read_bps) + L"  ↑ " + fmt::rate(std::max(0.0, row.disk_write_bps));
            },
            [peak](const Row &row) {
                return row.disk_read_bps < 0
                           ? 0.0f
                           : static_cast<float>((row.disk_read_bps + std::max(0.0, row.disk_write_bps)) / peak);
            });
        finish(r, bounds, stack.y);
    }

    void paint_network(Host &host, const Rect &bounds) {
        Renderer &r = host.renderer();
        Store &store = host.store();
        const Snapshot &s = store.snapshot();
        const History &h = store.history();
        const tokens::MetricStyle &style = tokens::metric::network;
        Rect area;
        Stack stack = begin(r, bounds, area);
        Rect trailing;
        stack.area = page_header(r, area, L"Network", L"All physical interfaces", MetricKind::Network, &trailing);
        stack.y = stack.area.y;
        rate_headline(r, trailing, s.net_rx_bps, s.net_tx_bps, L"Down", L"Up", MetricKind::Network);

        const float max = nice_max(std::max(h.net_rx.max_recent(), h.net_tx.max_recent()));
        Rect inner =
            panel(r, stack.next(kPanelChrome + 240), L"Throughput", Renderer::Symbol::Waveform, MetricKind::Network);
        live_chart(
            r, inner,
            {{&h.net_rx, L"Received", fmt::rate(s.net_rx_bps)}, {&h.net_tx, L"Sent", fmt::rate(s.net_tx_bps), true}},
            max, MetricKind::Network, rate_axis);

        const std::vector<Stat> stats = {
            {L"Download", fmt::rate(s.net_rx_bps), L"", rgba(style.start)},
            {L"Upload", fmt::rate(s.net_tx_bps), L"", rgba(style.end)},
            {L"Peak down (60s)", fmt::rate(h.net_rx.max_recent()), L""},
            {L"Peak up (60s)", fmt::rate(h.net_tx.max_recent()), L""},
            {L"Received since boot", fmt::bytes(static_cast<int64_t>(s.net_rx_total)), L""},
            {L"Sent since boot", fmt::bytes(static_cast<int64_t>(s.net_tx_total)), L""},
        };
        const float content_w = stack.area.w - tokens::space::lg * 2;
        inner = panel(r, stack.next(kPanelChrome + stat_grid_height(content_w, stats.size())), L"Details",
                      Renderer::Symbol::Bullets);
        stat_grid(r, inner, stats);

        if (store.has(PC_CAP_PROCESS_NETWORK)) {
            const float peak = std::max(1.0f, max);
            top_apps(
                host, stack, L"Top Apps", Renderer::Symbol::Ports, MetricKind::Network, PC_COLUMN_NET_RX,
                [](const Row &row) {
                    if (row.net_rx_bps < 0) return std::wstring(fmt::unavailable);
                    return L"↓ " + fmt::rate(row.net_rx_bps) + L"  ↑ " + fmt::rate(std::max(0.0, row.net_tx_bps));
                },
                [peak](const Row &row) {
                    return row.net_rx_bps < 0
                               ? 0.0f
                               : static_cast<float>((row.net_rx_bps + std::max(0.0, row.net_tx_bps)) / peak);
                });
        } else {
            info_banner(r, stack.next(44),
                        L"Procyon can't read per-app network usage on Windows yet, so it hides it instead of showing "
                        L"estimates.",
                        L"", Renderer::Tone::Neutral);
        }
        finish(r, bounds, stack.y);
    }

    void paint_gpu(Host &host, const Rect &bounds) {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        Store &store = host.store();
        const Snapshot &s = store.snapshot();
        const History &h = store.history();
        const tokens::MetricStyle &style = tokens::metric::gpu;
        Rect area;
        Stack stack = begin(r, bounds, area);
        Rect trailing;
        std::wstring subtitle = s.gpus.empty() ? L"No GPU found" : fmt::from_utf8(s.gpus[0].name);
        if (!s.gpus.empty() && s.gpus[0].vendor[0]) subtitle += L" · " + fmt::from_utf8(s.gpus[0].vendor);
        stack.area = page_header(r, area, L"GPU", subtitle, MetricKind::Gpu, &trailing);
        stack.y = stack.area.y;
        if (!s.gpus.empty() && s.gpus[0].utilization >= 0)
            value_text(r, Rect{trailing.x, trailing.y + 6, trailing.w, 40}, fmt::number(s.gpus[0].utilization * 100, 0),
                       L"%", Font::Display, theme.text(), HAlign::Right);

        if (s.gpus.empty()) {
            empty_state(r, stack.next(200), L"No GPU", L"Windows reports no display adapter.");
        }
        for (size_t i = 0; i < s.gpus.size(); ++i) {
            const pc_gpu &g = s.gpus[i];
            Rect inner =
                panel(r, stack.next(kPanelChrome + 220), s.gpus.size() > 1 ? fmt::from_utf8(g.name) : L"Utilization",
                      Renderer::Symbol::Waveform, MetricKind::Gpu);
            if (i < h.gpu.size())
                live_chart(r, inner, {{&h.gpu[i], L"Busiest engine", fmt::percent(g.utilization)}}, 1, MetricKind::Gpu,
                           percent_axis);
            if (i == 0) {
                double hottest = -1;
                for (const pc_gpu &gpu : s.gpus) hottest = std::max(hottest, gpu.temperature);
                if (hottest >= 0) {
                    const bool f = host.settings().fahrenheit;
                    const D2D1_COLOR_F tint = rgba(style.end);
                    const float peak = std::max(h.gpu_temperature.max_recent(), static_cast<float>(hottest));
                    inner = panel(r, stack.next(kPanelChrome + 160), L"Temperature", Renderer::Symbol::Thermo);
                    live_chart(r, inner,
                               {{&h.gpu_temperature, L"GPU", fmt::temperature(hottest, f), false, tint},
                                {nullptr, L"Peak", fmt::temperature(peak, f), false, theme.danger()}},
                               110, MetricKind::Gpu, [f](float v) { return fmt::temperature(v, f); });
                }
            }

            std::vector<Stat> stats = {
                {L"Utilization", fmt::percent(g.utilization, 1), L"busiest engine", rgba(style.start)}};
            if (g.renderer_utilization >= 0)
                stats.push_back({L"3D", fmt::percent(g.renderer_utilization, 1), L"Graphics and compute"});
            if (g.encoder_utilization >= 0)
                stats.push_back({L"Video encode", fmt::percent(g.encoder_utilization), L""});
            if (g.decoder_utilization >= 0)
                stats.push_back({L"Video decode", fmt::percent(g.decoder_utilization), L""});
            stats.push_back({L"Vendor", g.vendor[0] ? fmt::from_utf8(g.vendor) : std::wstring(fmt::unavailable), L""});
            stats.push_back({L"Memory", g.unified_memory ? L"Shared with the system" : L"Dedicated", L""});
            const bool wide = stack.area.w >= 720;
            const float mem_w = wide ? std::min(320.0f, stack.area.w * 0.32f) : stack.area.w;
            const float details_w = wide ? stack.area.w - mem_w - tokens::space::lg : stack.area.w;
            const float details_h = kPanelChrome + stat_grid_height(details_w - tokens::space::lg * 2, stats.size());
            const float row_h = wide ? std::max(details_h, kPanelChrome + 80) : details_h;
            const Rect row = stack.next(row_h);
            inner = panel(r, Rect{row.x, row.y, details_w, row_h}, L"Details", Renderer::Symbol::Bullets);
            stat_grid(r, inner, stats);
            const Rect mem_card = wide ? Rect{row.right() - mem_w, row.y, mem_w, row_h} : stack.next(kPanelChrome + 80);
            inner = panel(r, mem_card, L"Memory", Renderer::Symbol::Stack);
            const auto used = fmt::split_unit(fmt::bytes(g.memory_used));
            const float vw = value_text(r, inner.take_top(32), used.first, used.second, Font::Metric, theme.text());
            if (g.memory_total > 0) {
                TextStyle of;
                of.font = Font::Label;
                of.valign = VAlign::Bottom;
                r.text(L"of " + fmt::bytes(g.memory_total),
                       Rect{inner.x + vw + tokens::space::xs, inner.y - 32, inner.w - vw, 28}, of,
                       theme.text_tertiary());
                inner.take_top(tokens::space::md);
                r.usage_bar(
                    inner.take_top(6),
                    static_cast<float>(static_cast<double>(std::max<int64_t>(0, g.memory_used)) / g.memory_total),
                    MetricKind::Gpu);
            }
        }
        if (store.has(PC_CAP_PROCESS_GPU))
            top_apps(
                host, stack, L"Top Apps", Renderer::Symbol::Flame, MetricKind::Gpu, PC_COLUMN_GPU,
                [](const Row &row) { return fmt::cpu(row.gpu_percent); },
                [](const Row &row) { return static_cast<float>(std::max(0.0, row.gpu_percent) / 100); });
        finish(r, bounds, stack.y);
    }

    void paint_battery(Host &host, const Rect &bounds) {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        Store &store = host.store();
        std::optional<pc_battery> b = store.battery();
        Rect area;
        Stack stack = begin(r, bounds, area);
        Rect trailing;
        stack.area = page_header(r, area, L"Battery",
                                 b ? (b->charging      ? L"Charging"
                                      : b->on_ac_power ? L"On AC power"
                                                       : L"On battery")
                                   : L"No battery",
                                 MetricKind::Battery, &trailing);
        stack.y = stack.area.y;
        if (!b) {
            empty_state(r, stack.next(200), L"No battery", L"This PC runs on mains power.");
            finish(r, bounds, stack.y);
            return;
        }
        value_text(r, Rect{trailing.x, trailing.y + 6, trailing.w, 40}, fmt::number(b->level * 100, 0), L"%",
                   Font::Display, theme.text(), HAlign::Right);
        std::vector<Stat> stats = {
            {L"Charge", fmt::percent(b->level),
             b->charging      ? L"Charging"
             : b->on_ac_power ? L"On AC power"
                              : L"On battery",
             rgba(tokens::metric::battery.start)},
            {L"Time remaining",
             b->on_ac_power ? (b->minutes_to_full > 0 ? fmt::duration(b->minutes_to_full * 60.0) + L" to full"
                               : b->fully_charged     ? L"Fully charged"
                                                      : std::wstring(fmt::unavailable))
             : b->minutes_to_empty > 0 ? fmt::duration(b->minutes_to_empty * 60.0)
                                       : std::wstring(fmt::unavailable),
             L""},
            {L"Power", b->power_watts != 0 ? fmt::watts(std::fabs(b->power_watts)) : std::wstring(fmt::unavailable),
             b->power_watts < 0   ? L"discharging"
             : b->power_watts > 0 ? L"charging"
                                  : L""},
            {L"Health", b->health >= 0 ? fmt::percent(b->health) : std::wstring(fmt::unavailable),
             fmt::from_utf8(b->condition)},
            {L"Cycles", b->cycle_count >= 0 ? fmt::count(b->cycle_count) : std::wstring(fmt::unavailable), L""},
            {L"Capacity",
             b->max_capacity_mah > 0 ? fmt::count(b->max_capacity_mah) + L" mAh" : std::wstring(fmt::unavailable),
             b->design_capacity_mah > 0 ? L"design " + fmt::count(b->design_capacity_mah) + L" mAh" : L""},
            {L"Temperature", fmt::temperature(b->temperature, host.settings().fahrenheit), L""},
        };
        const float content_w = stack.area.w - tokens::space::lg * 2 - 110;
        Rect inner = panel(r, stack.next(kPanelChrome + std::max(110.0f, stat_grid_height(content_w, stats.size()))),
                           L"Details", Renderer::Symbol::Bullets, MetricKind::Battery);
        const Rect gauge = inner.take_left(110);
        r.ring_gauge(gauge.x + 43, gauge.y + 43, 38, static_cast<float>(b->level), MetricKind::Battery, 9);
        TextStyle v;
        v.font = Font::Stat;
        v.halign = HAlign::Center;
        r.text(fmt::percent(b->level), Rect{gauge.x, gauge.y + 31, 86, 24}, v, theme.text());
        stat_grid(r, inner, stats);
        if (!assertions_.empty()) {
            inner = panel(r, stack.next(kPanelChrome + assertions_.size() * 24), L"Preventing sleep",
                          Renderer::Symbol::Clock);
            for (const pc_power_assertion &pa : assertions_) {
                Rect row = inner.take_top(24);
                TextStyle name;
                name.font = Font::Body;
                // A driver holds no process: its device description stands in for the name.
                const std::wstring who = pa.process_name[0] ? fmt::from_utf8(pa.process_name)
                                         : pa.pid > 0       ? L"PID " + std::to_wstring(pa.pid)
                                                            : L"Driver";
                const float nw = r.text(who, row, name, theme.text());
                TextStyle kind;
                kind.font = Font::Caption;
                r.text((pa.kind & PC_ASSERT_DISPLAY_SLEEP) ? L"display" : L"sleep",
                       Rect{row.x + nw + tokens::space::sm, row.y, 60, row.h}, kind, theme.text_tertiary());
                TextStyle reason;
                reason.font = Font::Caption;
                reason.halign = HAlign::Right;
                r.text(fmt::from_utf8(pa.reason), Rect{row.x + nw + 70, row.y, row.w - nw - 70, row.h}, reason,
                       theme.text_secondary());
            }
        }
        finish(r, bounds, stack.y);
    }

    PageId id_;
    ScrollState scroll_;
    std::vector<pc_volume> volumes_;
    std::vector<pc_power_assertion> assertions_;
    std::vector<TopAppRow> app_rows_;
};

}  // namespace

std::unique_ptr<Page> make_performance_page(PageId id) { return std::make_unique<PerformancePage>(id); }

}  // namespace procyon::ui
