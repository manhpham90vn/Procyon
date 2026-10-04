// History: the last 24 hours minute by minute, when the machine was busy and which apps made it
// busy (the macOS HistoryView, panel for panel).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

#include "pages.hpp"
#include "widgets.hpp"

namespace procyon::ui {
namespace {

constexpr int64_t kRanges[] = {3600, 21600, 86400};
const wchar_t *const kRangeTitles[] = {L"1 Hour", L"6 Hours", L"24 Hours"};
constexpr float kChartHeight = 260;
constexpr float kAppRow = 36;
constexpr float kPeakRow = 28;
constexpr size_t kTopApps = 10;

enum class Metric { Cpu, Memory, Disk, Network, Gpu, Temperature };

struct MetricInfo {
    Metric id;
    const wchar_t *title;
    MetricKind kind;
    AppOrder order;
};

const MetricInfo kMetrics[] = {
    {Metric::Cpu, L"CPU", MetricKind::Cpu, AppOrder::Cpu},
    {Metric::Memory, L"Memory", MetricKind::Memory, AppOrder::Memory},
    {Metric::Disk, L"Disk", MetricKind::Disk, AppOrder::Disk},
    {Metric::Network, L"Network", MetricKind::Network, AppOrder::Network},
    {Metric::Gpu, L"GPU", MetricKind::Gpu, AppOrder::Gpu},
    {Metric::Temperature, L"Temperature", MetricKind::Cpu, AppOrder::Cpu},  // what heats the chip
};

int64_t now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::wstring weekday_time(int64_t unix_seconds) {
    const time_t t = static_cast<time_t>(unix_seconds);
    tm local{};
    if (localtime_s(&local, &t) != 0) return fmt::unavailable;
    wchar_t buffer[32];
    (void)wcsftime(buffer, 32, L"%a %H:%M", &local);
    return buffer;
}

class HistoryPage : public Page {
public:
    PageId id() const override { return PageId::History; }
    std::wstring title() const override { return L"History"; }

    void activate(Host &host) override {
        host_ = &host;
        load(true);
    }
    void tick(Host &host) override {
        host_ = &host;
        if (now_seconds() - loaded_at_ >= 60) load(false);
    }

    void paint(Host &host, const Rect &bounds) override {
        host_ = &host;
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        Store &store = host.store();
        const float mx = host.mouse_x(), my = host.mouse_y();
        Rect area = screen_area(bounds);
        r.push_clip(bounds);
        area.y -= scroll_.offset;
        Rect trailing;
        area = page_header(r, area, L"History", L"The last 24 hours, minute by minute", std::nullopt, &trailing);
        area.y -= kSectionGap - tokens::space::lg;
        area.h += kSectionGap - tokens::space::lg;

        // Range in the header, metric below it (both segmented, like the SwiftUI pickers).
        const std::vector<std::wstring> ranges(std::begin(kRangeTitles), std::end(kRangeTitles));
        const float rw = r.segmented_width(ranges);
        range_rects_ = r.segmented(Rect{trailing.right() - rw, trailing.y + 2, rw, kControlHeight}, ranges, range_);
        const std::vector<const MetricInfo *> metrics = available_metrics(store);
        std::vector<std::wstring> labels;
        int selected = 0;
        for (size_t i = 0; i < metrics.size(); ++i) {
            labels.emplace_back(metrics[i]->title);
            if (metrics[i]->id == metric_) selected = static_cast<int>(i);
        }
        const MetricInfo &metric = *metrics[static_cast<size_t>(selected)];
        metric_ = metric.id;
        Rect toolbar = area.take_top(kControlHeight);
        metric_rects_ =
            r.segmented(Rect{toolbar.x, toolbar.y, r.segmented_width(labels), kControlHeight}, labels, selected);
        area.take_top(kSectionGap);

        banner_button_ = {};
        if (!store.records_history()) {
            const Banner b = action_banner(r, area.take_top(kActionBannerHeight), L"History is off",
                                           L"Procyon isn't recording. Turn it on to keep the last 24 hours on this PC.",
                                           L"Turn On", Renderer::Tone::Warning, banner_button_.contains(mx, my));
            banner_button_ = b.button;
            area.take_top(kSectionGap);
        }

        // The chart.
        const Rect chart_card = area.take_top(kPanelChrome + kChartHeight);
        Rect inner = panel(r, chart_card, metric.title, Renderer::Symbol::Waveform, metric.kind);
        paint_chart(host, inner, metric);
        if (loaded_ && minutes_.empty())
            empty_state(r, inner, L"No history yet",
                        L"Procyon saves one point a minute while it runs. Come back in a few minutes.");
        area.take_top(kSectionGap);

        // Busiest apps and peaks, side by side.
        const float panels_h = kPanelChrome + std::max(kTopApps * kAppRow, 5 * kPeakRow + 20);
        Rect row = area.take_top(panels_h);
        Rect peaks_card = row.take_right(std::min(380.0f, row.w * 0.4f));
        row.take_right(tokens::space::lg);
        paint_apps(host, row, metric);
        paint_peaks(host, peaks_card, metric);
        area.take_top(kSectionGap);

        // Record toggle, size on disk, Clear History.
        Rect footer = area.take_top(kControlHeight);
        toggle_rect_ = Rect{footer.x, footer.cy() - 10, 36, 20};
        r.toggle_switch(toggle_rect_.x, toggle_rect_.y, store.records_history(), true);
        TextStyle label;
        label.font = Font::Body;
        const float lw = r.text(L"Record history", Rect{footer.x + 36 + tokens::space::sm, footer.y, 200, footer.h},
                                label, theme.text());
        TextStyle caption;
        caption.font = Font::Caption;
        r.text(L"Kept on this PC for 24 hours · " + fmt::bytes(size_) + L" on disk",
               Rect{footer.x + 36 + tokens::space::sm + lw + tokens::space::md, footer.y, 400, footer.h}, caption,
               theme.text_tertiary());
        const float cw = r.measure(L"Clear History…", Font::BodyMedium) + tokens::space::md * 2;
        clear_button_ = button(r, footer.right() - cw, footer.y, L"Clear History…", false, true,
                               clear_button_.contains(mx, my), cw);
        const float end_y = footer.bottom() + tokens::space::xxl;

        r.pop_clip();
        scroll_.viewport = bounds.h;
        scroll_.content = end_y + scroll_.offset - bounds.y;
        scroll_.clamp();
    }

    void mouse_move(Host &, const MouseEvent &e) override {
        mouse_x_ = e.x;
        mouse_y_ = e.y;
    }
    void mouse_leave(Host &) override { mouse_x_ = mouse_y_ = -1; }
    void wheel(Host &, const MouseEvent &e) override { scroll_.wheel(e.wheel); }
    Cursor cursor() const override {
        for (const auto &peak : peak_rects_)
            if (peak.first.contains(mouse_x_, mouse_y_)) return Cursor::Hand;
        if (whole_range_button_.contains(mouse_x_, mouse_y_) || clear_button_.contains(mouse_x_, mouse_y_) ||
            toggle_rect_.contains(mouse_x_, mouse_y_) || banner_button_.contains(mouse_x_, mouse_y_))
            return Cursor::Hand;
        return Cursor::Arrow;
    }

    void mouse_down(Host &host, const MouseEvent &e, bool right) override {
        if (right) return;
        for (size_t i = 0; i < range_rects_.size(); ++i) {
            if (range_rects_[i].contains(e.x, e.y)) {
                range_ = static_cast<int>(i);
                selected_minute_ = -1;
                load(true);
                return;
            }
        }
        const std::vector<const MetricInfo *> metrics = available_metrics(host.store());
        for (size_t i = 0; i < metric_rects_.size() && i < metrics.size(); ++i) {
            if (metric_rects_[i].contains(e.x, e.y)) {
                metric_ = metrics[i]->id;
                load_apps();
                return;
            }
        }
        if (!banner_button_.empty() && banner_button_.contains(e.x, e.y)) {
            set_recording(host, true);
            return;
        }
        if (toggle_rect_.contains(e.x, e.y)) {
            set_recording(host, !host.store().records_history());
            return;
        }
        if (clear_button_.contains(e.x, e.y)) {
            if (host.confirm(L"Clear the history?", L"Every saved minute is deleted from this PC.", L"Clear History",
                             true)) {
                host.store().history_database().clear();
                selected_minute_ = -1;
                load(true);
            }
            return;
        }
        if (!whole_range_button_.empty() && whole_range_button_.contains(e.x, e.y)) {
            selected_minute_ = -1;
            load_apps();
            return;
        }
        for (const auto &[rect, minute] : peak_rects_) {
            if (rect.contains(e.x, e.y)) {
                selected_minute_ = minute;
                load_apps();
                return;
            }
        }
        if (chart_rect_.contains(e.x, e.y)) {
            // A click pins the hovered minute (the apps panel follows it); a second click frees it.
            const int64_t minute = minute_at(e.x);
            selected_minute_ = selected_minute_ == minute ? -1 : minute;
            load_apps();
        }
    }

private:
    std::vector<const MetricInfo *> available_metrics(const Store &store) const {
        std::vector<const MetricInfo *> out;
        for (const MetricInfo &m : kMetrics) {
            if (m.id == Metric::Gpu && !store.has(PC_CAP_GPU)) continue;
            if (m.id == Metric::Temperature && !store.has(PC_CAP_TEMPERATURE)) continue;
            out.push_back(&m);
        }
        return out;
    }

    static double value_of(Metric metric, const MachineMinute &m) {
        switch (metric) {
            case Metric::Cpu: return m.cpu;
            case Metric::Memory: return m.memory;
            case Metric::Disk: return m.disk_read + m.disk_write;
            case Metric::Network: return m.net_rx + m.net_tx;
            case Metric::Gpu: return m.gpu;
            case Metric::Temperature: return m.cpu_temperature;
        }
        return -1;
    }

    std::wstring format(Metric metric, double value) const {
        switch (metric) {
            case Metric::Cpu:
            case Metric::Memory:
            case Metric::Gpu: return fmt::percent(value);
            case Metric::Disk:
            case Metric::Network: return fmt::rate(value);
            case Metric::Temperature: return fmt::temperature(value, host_ && host_->settings().fahrenheit);
        }
        return fmt::unavailable;
    }

    static float ceiling(Metric metric, double peak) {
        switch (metric) {
            case Metric::Cpu:
            case Metric::Memory:
            case Metric::Gpu: return 1;
            case Metric::Temperature: return static_cast<float>(std::max(100.0, peak));
            default: return nice_max(static_cast<float>(std::max(peak, 1e-6)));
        }
    }

    static double app_value(Metric metric, const AppUsage &app) {
        switch (metric) {
            case Metric::Cpu:
            case Metric::Temperature: return app.cpu;
            case Metric::Memory: return app.memory;
            case Metric::Disk: return app.disk;
            case Metric::Network: return app.network;
            case Metric::Gpu: return app.gpu;
        }
        return 0;
    }

    static std::wstring app_format(Metric metric, double value) {
        switch (metric) {
            case Metric::Cpu:
            case Metric::Temperature:
            case Metric::Gpu: return fmt::cpu(value);
            case Metric::Memory: return fmt::bytes(static_cast<int64_t>(value));
            case Metric::Disk:
            case Metric::Network: return fmt::rate(value);
        }
        return fmt::unavailable;
    }

    const MetricInfo &info(Metric metric) const {
        for (const MetricInfo &m : kMetrics)
            if (m.id == metric) return m;
        return kMetrics[0];
    }

    void set_recording(Host &host, bool enabled) {
        host.settings().records_history = enabled;
        host.store().set_records_history(enabled);
        host.repaint();
    }

    void load(bool force) {
        if (!host_) return;
        const int64_t now = now_seconds();
        if (!force && now - loaded_at_ < 60) return;
        HistoryDatabase &db = host_->store().history_database();
        minutes_ = db.machine(now - kRanges[range_]);
        size_ = db.size();
        loaded_at_ = now;
        loaded_ = true;
        load_apps();
    }

    void load_apps() {
        if (!host_) return;
        HistoryDatabase &db = host_->store().history_database();
        const int64_t now = now_seconds();
        const AppOrder order = info(metric_).order;
        if (selected_minute_ >= 0)
            apps_ = db.apps(selected_minute_, selected_minute_ + 60, order, kTopApps);
        else
            apps_ = db.apps(now - kRanges[range_], now, order, kTopApps);
    }

    // The busiest minutes, at least 10 minutes apart so one long spike shows once.
    std::vector<MachineMinute> peaks() const {
        std::vector<const MachineMinute *> sorted;
        for (const MachineMinute &m : minutes_)
            if (value_of(metric_, m) >= 0) sorted.push_back(&m);
        std::sort(sorted.begin(), sorted.end(), [&](const MachineMinute *a, const MachineMinute *b) {
            return value_of(metric_, *a) > value_of(metric_, *b);
        });
        std::vector<MachineMinute> chosen;
        for (const MachineMinute *m : sorted) {
            if (std::all_of(chosen.begin(), chosen.end(),
                            [&](const MachineMinute &c) { return std::llabs(c.minute - m->minute) >= 600; }))
                chosen.push_back(*m);
            if (chosen.size() == 5) break;
        }
        return chosen;
    }

    const MachineMinute *minute(int64_t start) const {
        for (const MachineMinute &m : minutes_)
            if (m.minute == start) return &m;
        return nullptr;
    }

    // The minute nearest to `x` in the chart, -1 when none is within reach.
    int64_t minute_at(float x) const {
        if (minutes_.empty() || chart_rect_.w <= 0) return -1;
        const int64_t range = kRanges[range_];
        const int64_t start = chart_now_ - range;
        const auto x_of = [&](int64_t t) {
            return chart_rect_.x + static_cast<float>(t - start) / static_cast<float>(range) * chart_rect_.w;
        };
        int64_t best = -1;
        float best_distance = 12;
        for (const MachineMinute &m : minutes_) {
            const float d = std::fabs(x_of(m.minute + 30) - x);
            if (d < best_distance) {
                best_distance = d;
                best = m.minute;
            }
        }
        return best;
    }

    void paint_chart(Host &host, const Rect &bounds, const MetricInfo &metric) {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        const tokens::MetricStyle &style = metric_style(metric.kind);
        const Color color = rgba(style.start);
        Rect area = bounds;
        chart_now_ = now_seconds();
        const int64_t range = kRanges[range_];
        const int64_t start = chart_now_ - range;

        double peak = 0;
        for (const MachineMinute &m : minutes_) {
            peak = std::max(peak, value_of(metric_, m));
            if (metric_ == Metric::Cpu) peak = std::max(peak, m.cpu_peak);
        }
        const float max = ceiling(metric_, peak);

        // Legend: the average (the hovered or selected minute when there is one) and, for CPU,
        // the peak.
        const int64_t hovered = chart_rect_.contains(mouse_x_, mouse_y_) ? minute_at(mouse_x_) : -1;
        const int64_t focus = hovered >= 0 ? hovered : selected_minute_;
        const MachineMinute *focused = focus >= 0 ? minute(focus) : nullptr;
        const MachineMinute *latest = minutes_.empty() ? nullptr : &minutes_.back();
        const MachineMinute *shown = focused ? focused : latest;
        Rect legend = area.take_top(18);
        float x = legend.x;
        auto legend_entry = [&](std::wstring_view label, std::wstring_view value, Color swatch) {
            r.fill_round(Rect{x, legend.cy() - 2, 10, 4}, tokens::radius::pill, swatch);
            x += 10 + tokens::space::xs + 2;
            TextStyle l;
            l.font = Font::Label;
            const float lw = r.measure(label, Font::Label);
            r.text(label, Rect{x, legend.y, lw + 2, legend.h}, l, theme.text_secondary());
            x += lw + tokens::space::xs + 2;
            TextStyle v;
            v.font = Font::Headline;
            v.tabular = true;
            const float vw = r.measure(value, Font::Headline);
            r.text(value, Rect{x, legend.y, vw + 2, legend.h}, v, theme.text());
            x += vw + tokens::space::lg;
        };
        legend_entry(L"Average",
                     shown && value_of(metric_, *shown) >= 0 ? format(metric_, value_of(metric_, *shown))
                                                             : std::wstring(fmt::unavailable),
                     color);
        if (metric_ == Metric::Cpu)
            legend_entry(L"Peak", shown ? format(metric_, shown->cpu_peak) : std::wstring(fmt::unavailable),
                         with_alpha(rgba(style.end), 0.8f));
        if (shown) {
            TextStyle when;
            when.font = Font::Caption;
            when.halign = HAlign::Right;
            r.text(focused ? fmt::time_of_day(shown->minute) + (selected_minute_ == focus ? L" · pinned" : L"")
                           : L"latest " + fmt::time_of_day(shown->minute),
                   legend, when, theme.text_tertiary());
        }
        area.take_top(tokens::space::md);
        Rect footer = area.take_bottom(14);
        area.take_bottom(tokens::space::md);
        Rect axis = area.take_right(44);
        area.take_right(tokens::space::sm);
        const Rect chart = area;
        chart_rect_ = chart;

        TextStyle axis_style;
        axis_style.font = Font::Caption;
        axis_style.tabular = true;
        axis_style.valign = VAlign::Top;
        r.text(format(metric_, max), Rect{axis.x, chart.y, axis.w, 14}, axis_style, theme.text_tertiary());
        r.text(format(metric_, max / 2), Rect{axis.x, chart.cy() - 7, axis.w, 14}, axis_style, theme.text_tertiary());
        r.text(format(metric_, 0), Rect{axis.x, chart.bottom() - 14, axis.w, 14}, axis_style, theme.text_tertiary());
        // Grid: four dashed lines and a solid baseline, like LiveChart.
        for (int i = 0; i < 4; ++i) {
            const float gy = chart.y + chart.h * i / 4;
            r.line(chart.x, gy, chart.right(), gy, theme.chart_grid(), 1, true);
        }
        r.line(chart.x, chart.bottom() - 0.5f, chart.right(), chart.bottom() - 0.5f, theme.chart_grid());

        // Minutes as x; the line breaks where minutes are missing (Procyon wasn't running).
        const auto x_of = [&](int64_t t) {
            return chart.x + static_cast<float>(t - start) / static_cast<float>(range) * chart.w;
        };
        const auto y_of = [&](double v) {
            return chart.bottom() - static_cast<float>(std::clamp(v / max, 0.0, 1.0)) * (chart.h - 2) - 1;
        };
        r.push_clip(chart.inset(-4, -4));
        // One figure per run of consecutive minutes.
        std::vector<std::vector<const MachineMinute *>> runs;
        for (const MachineMinute &m : minutes_) {
            if (value_of(metric_, m) < 0) continue;
            if (runs.empty() || m.minute - runs.back().back()->minute > 120) runs.emplace_back();
            runs.back().push_back(&m);
        }
        const Gradient gradient{{chart.x, chart.y},
                                {chart.x, chart.bottom()},
                                with_alpha(color, tokens::chart::fillOpacityTop),
                                with_alpha(color, tokens::chart::fillOpacityBottom)};
        for (const auto &run : runs) {
            if (run.size() == 1) {
                r.fill_circle(x_of(run[0]->minute + 30), y_of(value_of(metric_, *run[0])), 2.5f, color);
                continue;
            }
            const auto points_for = [&](bool peak_line) {
                std::vector<Point> points;
                points.reserve(run.size());
                for (const MachineMinute *m : run)
                    points.push_back({x_of(m->minute + 30), y_of(peak_line ? m->cpu_peak : value_of(metric_, *m))});
                return points;
            };
            const std::vector<Point> points = points_for(false);
            r.fill_path(Path::smooth(points, true, chart.bottom()), gradient);
            Stroke pen;
            pen.width = 1.5f;
            r.stroke_path(Path::smooth(points), color, pen);
            if (metric_ == Metric::Cpu) {
                pen.width = 1;
                pen.dash = Dash::Dashed;
                r.stroke_path(Path::smooth(points_for(true)), with_alpha(rgba(style.end), 0.6f), pen);
            }
        }
        // The selected and the hovered minute: a rule and a value bubble.
        auto rule = [&](const MachineMinute &m, bool pinned) {
            const float rx = x_of(m.minute + 30);
            r.line(rx, chart.y, rx, chart.bottom(), pinned ? theme.accent() : theme.text_tertiary(), 1, !pinned);
            const std::wstring time = fmt::time_of_day(m.minute);
            const std::wstring value = format(metric_, value_of(metric_, m));
            const float w =
                std::max(r.measure(time, Font::Caption), r.measure(value, Font::Headline)) + tokens::space::sm * 2;
            const float bx = std::clamp(rx - w / 2, chart.x, chart.right() - w);
            const Rect bubble{bx, chart.y, w, 38};
            r.fill_round(bubble, tokens::radius::sm, theme.surface_raised());
            r.stroke_round(bubble, tokens::radius::sm, theme.border());
            TextStyle t;
            t.font = Font::Caption;
            t.halign = HAlign::Center;
            t.valign = VAlign::Top;
            r.text(time, Rect{bubble.x, bubble.y + 4, bubble.w, 14}, t, theme.text_secondary());
            TextStyle v;
            v.font = Font::Headline;
            v.halign = HAlign::Center;
            v.valign = VAlign::Top;
            v.tabular = true;
            r.text(value, Rect{bubble.x, bubble.y + 18, bubble.w, 18}, v, theme.text());
            r.fill_circle(rx, y_of(value_of(metric_, m)), 3, pinned ? theme.accent() : color);
        };
        if (const MachineMinute *selected = selected_minute_ >= 0 ? minute(selected_minute_) : nullptr)
            rule(*selected, true);
        if (focused && focus != selected_minute_) rule(*focused, false);
        r.pop_clip();

        TextStyle foot;
        foot.font = Font::Caption;
        r.text(fmt::time_of_day(start), footer, foot, theme.text_tertiary());
        foot.halign = HAlign::Right;
        r.text(L"now", Rect{footer.x, footer.y, chart.w, footer.h}, foot, theme.text_tertiary());
    }

    void paint_apps(Host &host, const Rect &card, const MetricInfo &metric) {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        const std::wstring title = selected_minute_ >= 0 ? L"Busiest Apps at " + fmt::time_of_day(selected_minute_)
                                                         : L"Busiest Apps, Last " + std::wstring(kRangeTitles[range_]);
        Rect accessory;
        Rect inner = panel(r, card, title, Renderer::Symbol::Flame, metric.kind, &accessory);
        whole_range_button_ = {};
        if (selected_minute_ >= 0) {
            TextStyle link;
            link.font = Font::Label;
            link.halign = HAlign::Right;
            const float w = r.measure(L"Show Whole Range", Font::Label) + 4;
            whole_range_button_ = Rect{accessory.right() - w, accessory.y, w, accessory.h};
            r.text(L"Show Whole Range", whole_range_button_, link, theme.accent());
        }
        if (apps_.empty()) {
            TextStyle style;
            style.font = Font::Body;
            style.halign = HAlign::Center;
            r.text(L"No app activity saved here.", Rect{inner.x, inner.y, inner.w, 80}, style, theme.text_tertiary());
            return;
        }
        const double top = std::max(app_value(metric_, apps_.front()), 1e-9);
        float y = inner.y;
        for (const AppUsage &app : apps_) {
            if (y + kAppRow > inner.bottom() + 1) break;
            Rect row{inner.x, y, inner.w, kAppRow};
            Rect line = row.inset(0, tokens::space::xs);
            const Rect icon = line.take_left(22 + tokens::space::sm + 2);
            r.app_icon(app_icon_path(app.app_id), Rect{icon.x, icon.y + 1, 22, 22}, app.app_id.rfind("exe:", 0) == 0);
            Rect head = line;
            head.h = 20;
            TextStyle headline;
            headline.font = Font::Headline;
            headline.tabular = true;
            headline.halign = HAlign::Right;
            const std::wstring v = app_format(metric_, app_value(metric_, app));
            const float vw = r.measure(v, Font::Headline) + 6;
            r.text(v, Rect{head.right() - vw - 2, head.y, vw + 2, head.h}, headline, theme.text());
            headline.halign = HAlign::Left;
            r.text(app.name.empty() ? fmt::from_utf8(app.app_id) : app.name,
                   Rect{head.x, head.y, head.w - vw - tokens::space::md, head.h}, headline, theme.text());
            r.usage_bar(Rect{line.x, line.bottom() - 6, line.w, 4},
                        static_cast<float>(std::min(app_value(metric_, app) / top, 1.0)), metric.kind);
            y += kAppRow;
        }
    }

    void paint_peaks(Host &host, const Rect &card, const MetricInfo &metric) {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        Rect inner = panel(r, card, L"Peaks", Renderer::Symbol::Peak);
        peak_rects_.clear();
        const std::vector<MachineMinute> top = peaks();
        if (top.empty()) {
            TextStyle style;
            style.font = Font::Body;
            style.halign = HAlign::Center;
            r.text(L"Nothing saved yet.", Rect{inner.x, inner.y, inner.w, 80}, style, theme.text_tertiary());
            return;
        }
        float y = inner.y;
        for (const MachineMinute &m : top) {
            const Rect row{inner.x - tokens::space::sm, y, inner.w + tokens::space::sm * 2, kPeakRow};
            const bool hover = row.contains(mouse_x_, mouse_y_);
            if (hover || m.minute == selected_minute_)
                r.fill_round(
                    row, tokens::radius::sm,
                    m.minute == selected_minute_ ? with_alpha(theme.accent(), 0.12f) : with_alpha(theme.text(), 0.05f));
            TextStyle when;
            when.font = Font::Body;
            r.text(weekday_time(m.minute), Rect{inner.x, y, inner.w / 2, kPeakRow}, when, theme.text_secondary());
            TextStyle value;
            value.font = Font::Headline;
            value.tabular = true;
            value.halign = HAlign::Right;
            r.text(format(metric_, value_of(metric_, m)), Rect{inner.x, y, inner.w, kPeakRow}, value, theme.text());
            peak_rects_.emplace_back(row, m.minute);
            y += kPeakRow;
        }
        (void)metric;
    }

    Host *host_ = nullptr;
    ScrollState scroll_;
    int range_ = 0;
    Metric metric_ = Metric::Cpu;
    std::vector<MachineMinute> minutes_;
    std::vector<AppUsage> apps_;
    bool loaded_ = false;
    int64_t loaded_at_ = 0;
    int64_t size_ = 0;
    int64_t selected_minute_ = -1;
    int64_t chart_now_ = 0;
    float mouse_x_ = -1, mouse_y_ = -1;
    std::vector<Rect> range_rects_, metric_rects_;
    Rect banner_button_, toggle_rect_, clear_button_, whole_range_button_, chart_rect_;
    std::vector<std::pair<Rect, int64_t>> peak_rects_;
};

}  // namespace

std::unique_ptr<Page> make_history_page() { return std::make_unique<HistoryPage>(); }

}  // namespace procyon::ui
