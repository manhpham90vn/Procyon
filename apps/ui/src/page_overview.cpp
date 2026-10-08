// Overview: the hero card with this PC's gauges, a metric card for every resource, and the
// busiest apps by CPU and memory (the macOS OverviewView, laid out the same way).
#include <algorithm>
#include <cmath>

#include "pages.hpp"

namespace procyon::ui {
namespace {

constexpr float kCardHeight = 184;
constexpr size_t kTopRows = 6;

struct CardSpec {
    MetricKind kind;
    PageId page;
    std::wstring title;
    std::wstring value;
    std::wstring unit;
    std::wstring caption;
    const Series *series = nullptr;
    const Series *secondary = nullptr;
    float series_max = 1;
};

class OverviewPage : public Page {
public:
    PageId id() const override { return PageId::Overview; }
    std::wstring title() const override { return L"Overview"; }

    void activate(Host &host) override { volumes_ = host.store().volumes(); }
    void tick(Host &host) override {
        if (host.store().ticks() % 10 == 0) volumes_ = host.store().volumes();
    }

    void paint(Host &host, const Rect &bounds) override {
        Renderer &r = host.renderer();
        Store &store = host.store();
        const Snapshot &s = store.snapshot();
        const History &h = store.history();
        cards_.clear();
        app_rows_.clear();
        const Rect area = screen_area(bounds);
        r.push_clip(bounds);
        float y = area.y - scroll_.offset;

        // Hero.
        const Rect hero{area.x, y, area.w, 190};
        paint_hero(host, hero);
        y = hero.bottom() + kSectionGap;

        // Metric cards: adaptive grid, at least 200 wide.
        std::vector<CardSpec> specs;
        const auto cpu = fmt::split_unit(fmt::percent(s.cpu_usage));
        specs.push_back({MetricKind::Cpu, PageId::Cpu, L"CPU", fmt::number(s.cpu_usage * 100, 0), L"%",
                         L"User " + fmt::percent(s.cpu_user) + L" · System " + fmt::percent(s.cpu_system), &h.cpu,
                         nullptr, 1});
        const auto used = fmt::split_unit(fmt::bytes(static_cast<int64_t>(s.memory_used)));
        specs.push_back({MetricKind::Memory, PageId::Memory, L"Memory", used.first, used.second,
                         L"of " + fmt::bytes(static_cast<int64_t>(s.memory_total)) + L" · Pressure " +
                             pressure_title(s.memory_pressure),
                         &h.memory_fraction, nullptr, 1});
        if (store.has(PC_CAP_GPU) && !s.gpus.empty()) {
            const pc_gpu &g = s.gpus[0];
            specs.push_back({MetricKind::Gpu, PageId::Gpu, L"GPU",
                             g.utilization >= 0 ? fmt::number(g.utilization * 100, 0) : std::wstring(fmt::unavailable),
                             g.utilization >= 0 ? L"%" : L"",
                             fmt::from_utf8(g.name) + L" · " + fmt::bytes(g.memory_used) + L" in use",
                             h.gpu.empty() ? nullptr : &h.gpu[0], nullptr, 1});
        }
        const auto read = fmt::split_unit(fmt::rate(s.disk_read_bps));
        specs.push_back({MetricKind::Disk, PageId::Disk, L"Disk", read.first, read.second + L" read",
                         L"Write " + fmt::rate(s.disk_write_bps), &h.disk_read, &h.disk_write,
                         nice_max(std::max(h.disk_read.max_recent(), h.disk_write.max_recent()))});
        const auto down = fmt::split_unit(fmt::rate(s.net_rx_bps));
        specs.push_back({MetricKind::Network, PageId::Network, L"Network", down.first, down.second + L" down",
                         L"Up " + fmt::rate(s.net_tx_bps), &h.net_rx, &h.net_tx,
                         nice_max(std::max(h.net_rx.max_recent(), h.net_tx.max_recent()))});
        const int columns = std::max(1, static_cast<int>((area.w + tokens::space::lg) / (200 + tokens::space::lg)));
        const float card_w = (area.w - tokens::space::lg * (columns - 1)) / columns;
        // Hover lifts a card by 1% (MetricCard's scaleEffect), eased over Motion.normal.
        const double now = host.now();
        const float dt = static_cast<float>(std::clamp(now - last_frame_, 0.0, 0.1));
        last_frame_ = now;
        card_lift_.resize(specs.size(), 0.0f);
        bool animating = false;
        for (size_t i = 0; i < specs.size(); ++i) {
            const int col = static_cast<int>(i) % columns;
            if (i > 0 && col == 0) y += kCardHeight + tokens::space::lg;
            const Rect card{area.x + col * (card_w + tokens::space::lg), y, card_w, kCardHeight};
            const float target = card.contains(host.mouse_x(), host.mouse_y()) ? 1.0f : 0.0f;
            float &lift = card_lift_[i];
            lift += (target - lift) * std::min(1.0f, dt * (4 / tokens::motion::normal));
            if (std::fabs(target - lift) < 0.01f) lift = target;
            animating |= lift != target;
            if (lift > 0) r.canvas().push_scale(1 + 0.01f * lift, card.center());
            paint_card(r, card, specs[i]);
            if (lift > 0) r.canvas().pop_transform();
            cards_.push_back({card, specs[i].page});
        }
        if (animating) host.request_frame();
        y += kCardHeight + kSectionGap;

        // Top apps, side by side.
        const float panel_h = top_apps_height(kTopRows);
        const float half = (area.w - tokens::space::lg) / 2;
        const bool wide = area.w >= 640;
        const Rect left{area.x, y, wide ? half : area.w, panel_h};
        const Rect right{wide ? area.x + half + tokens::space::lg : area.x, wide ? y : y + panel_h + tokens::space::lg,
                         wide ? half : area.w, panel_h};
        for (const TopAppRow &row : top_apps_panel(
                 host, left, L"Top CPU", Renderer::Symbol::Flame, MetricKind::Cpu, PC_COLUMN_CPU, false, kTopRows,
                 [](const Row &row) { return fmt::cpu(row.cpu_percent); },
                 [](const Row &row) { return static_cast<float>(std::max(0.0, row.cpu_percent) / 100); }))
            app_rows_.push_back(row);
        const double total = s.memory_total ? static_cast<double>(s.memory_total) : 1;
        for (const TopAppRow &row : top_apps_panel(
                 host, right, L"Top Memory", Renderer::Symbol::Stack, MetricKind::Memory, PC_COLUMN_MEMORY, false,
                 kTopRows, [](const Row &row) { return fmt::bytes(row.memory_bytes); },
                 [&](const Row &row) { return static_cast<float>(std::max<int64_t>(0, row.memory_bytes) / total); }))
            app_rows_.push_back(row);
        y = right.bottom() + tokens::space::xxl;
        r.pop_clip();
        scroll_.viewport = bounds.h;
        scroll_.content = y + scroll_.offset - bounds.y;
        scroll_.clamp();
    }

    void mouse_move(Host &, const MouseEvent &e) override {
        hover_ = false;
        for (const auto &[rect, page] : cards_) hover_ |= rect.contains(e.x, e.y);
        for (const TopAppRow &row : app_rows_) hover_ |= row.rect.contains(e.x, e.y);
    }

    void mouse_down(Host &host, const MouseEvent &e, bool right) override {
        if (right) return;
        for (const auto &[rect, page] : cards_)
            if (rect.contains(e.x, e.y)) {
                host.navigate(page);
                return;
            }
        for (const TopAppRow &row : app_rows_)
            if (row.rect.contains(e.x, e.y)) {
                host.show_in_processes(row.app_id, row.pid);
                return;
            }
    }

    void wheel(Host &, const MouseEvent &e) override { scroll_.wheel(e.wheel); }
    Cursor cursor() const override { return hover_ ? Cursor::Hand : Cursor::Arrow; }

private:
    static std::wstring pressure_title(int32_t pressure) {
        switch (pressure) {
            case PC_PRESSURE_NORMAL: return L"Normal";
            case PC_PRESSURE_WARNING: return L"Elevated";
            case PC_PRESSURE_CRITICAL: return L"Critical";
            default: return L"Unknown";
        }
    }

    void paint_hero(Host &host, const Rect &card) {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        Store &store = host.store();
        const Snapshot &s = store.snapshot();
        const pc_system_info &info = store.system_info();
        r.card(card, std::nullopt, tokens::radius::xl);
        // Two soft radial tints: CPU blue top-left, memory pink bottom-right.
        r.push_clip(card.inset(1, 1));
        const tokens::MetricStyle &cpu = tokens::metric::cpu;
        const tokens::MetricStyle &memory = tokens::metric::memory;
        for (int i = 6; i >= 1; --i) {
            const float radius = 420.0f * i / 6;
            r.fill_circle(card.x, card.y, radius, with_alpha(rgba(cpu.start), 0.18f / 6));
            r.fill_circle(card.right(), card.bottom(), radius * 0.9f, with_alpha(rgba(memory.end), 0.12f / 6));
        }
        r.pop_clip();
        Rect inner = card.inset(tokens::space::xl, tokens::space::xl);

        // Device glyph in a gradient circle.
        const Rect glyph = inner.take_left(78 + tokens::space::xl);
        const float gcx = glyph.x + 39, gcy = glyph.cy();
        r.fill_circle(gcx, gcy, 39, with_alpha(rgba(cpu.start), 0.25f));
        r.fill_circle(gcx + 20, gcy + 20, 30, with_alpha(rgba(memory.start), 0.12f));
        r.symbol(Renderer::Symbol::Desktop, Rect{gcx - 18, gcy - 18, 36, 36}, rgba(cpu.start), 1.5f);

        // Gauges on the right.
        std::vector<std::pair<std::wstring, std::pair<float, MetricKind>>> gauges = {
            {L"CPU", {static_cast<float>(s.cpu_usage), MetricKind::Cpu}},
            {L"Memory", {s.memory_total ? static_cast<float>(s.memory_used) / s.memory_total : 0, MetricKind::Memory}}};
        const pc_volume *root = nullptr;
        for (const pc_volume &v : volumes_)
            if (v.is_root || !root) root = &v;
        if (root && root->total_bytes)
            gauges.push_back(
                {L"Storage", {1 - static_cast<float>(root->available_bytes) / root->total_bytes, MetricKind::Disk}});
        const float gauge_w = 86, gap = tokens::space::xl;
        const Rect gauges_rect = inner.take_right(gauge_w * gauges.size() + gap * (gauges.size() - 1));
        inner.take_right(tokens::space::lg);
        for (size_t i = 0; i < gauges.size(); ++i) {
            const float gx = gauges_rect.x + i * (gauge_w + gap);
            const float gy = inner.cy() - 12;
            r.ring_gauge(gx + gauge_w / 2, gy, gauge_w / 2 - 4.5f, gauges[i].second.first, gauges[i].second.second, 9);
            TextStyle v;
            v.font = Font::Stat;
            v.halign = HAlign::Center;
            v.tabular = true;
            r.text(fmt::percent(gauges[i].second.first), Rect{gx, gy - 12, gauge_w, 24}, v, theme.text());
            TextStyle l;
            l.font = Font::Label;
            l.halign = HAlign::Center;
            r.text(gauges[i].first, Rect{gx, gy + gauge_w / 2 + tokens::space::sm, gauge_w, 16}, l,
                   theme.text_secondary());
        }

        // Model, facts, badges.
        Rect text = inner;
        const float block_h = 40 + tokens::space::xs + 18 + tokens::space::xs + 4 + 17;
        text.y += (text.h - block_h) / 2;
        const std::wstring model = display_model(info);
        TextStyle display;
        display.font = r.measure(model, Font::Display) > text.w ? Font::Title : Font::Display;
        display.valign = VAlign::Top;
        const float mw = r.text(model, Rect{text.x, text.y, text.w, 40}, display, theme.text());
        // The product id follows in a lighter weight when there is room for all of it.
        const std::wstring product = L"(" + fmt::from_utf8(info.model_id) + L")";
        if (info.model_name[0] && info.model_id[0] &&
            mw + tokens::space::sm + r.measure(product, Font::Title) <= text.w) {
            TextStyle detail;
            detail.font = Font::Title;
            detail.valign = VAlign::Bottom;
            r.text(product, Rect{text.x + mw + tokens::space::sm, text.y, text.w - mw - 8, 38}, detail,
                   theme.text_tertiary());
        }
        TextStyle body;
        body.font = Font::Body;
        body.valign = VAlign::Top;
        // The chip, memory and OS on one line, or two where they don't fit (as the macOS hero wraps),
        // the badges below whichever it took.
        const std::wstring summary = fmt::from_utf8(info.cpu_brand) + L" · " +
                                     fmt::bytes(static_cast<int64_t>(info.memory_total)) + L" · " +
                                     fmt::from_utf8(info.os_name) + L" " + fmt::from_utf8(info.os_version);
        const bool two_lines = r.measure(summary, Font::Body) > text.w;
        body.wrap = two_lines;
        const float summary_h = two_lines ? 36 : 18;
        r.text(summary, Rect{text.x, text.y + 40 + tokens::space::xs, text.w, summary_h}, body, theme.text_secondary());
        const double uptime = s.timestamp > 0 && info.boot_time > 0 ? s.timestamp - info.boot_time : 0;
        float bx = text.x;
        const float by = text.y + 40 + tokens::space::xs + summary_h + tokens::space::xs + 4;
        bx += r.badge(bx, by, L"Up " + fmt::duration(uptime), Renderer::Tone::Accent) + tokens::space::xs + 2;
        bx += r.badge(bx, by, fmt::count(s.process_count) + L" processes", Renderer::Tone::Neutral) +
              tokens::space::xs + 2;
        if (s.memory_pressure != PC_PRESSURE_UNKNOWN)
            r.badge(bx, by, L"Memory " + pressure_title(s.memory_pressure),
                    s.memory_pressure == PC_PRESSURE_NORMAL    ? Renderer::Tone::Success
                    : s.memory_pressure == PC_PRESSURE_WARNING ? Renderer::Tone::Warning
                                                               : Renderer::Tone::Danger);
    }

    void paint_card(Renderer &r, const Rect &card, const CardSpec &spec) {
        const Theme &theme = r.theme();
        r.card(card, spec.kind);
        Rect inner = card.inset(tokens::space::lg, tokens::space::lg, tokens::space::lg, 0);
        Rect head = inner.take_top(24);
        const Rect icon = head.take_left(24 + tokens::space::sm);
        r.metric_icon(Rect{icon.x, icon.y, 24, 24}, spec.kind);
        TextStyle t;
        t.font = Font::Headline;
        r.text(spec.title, head, t, theme.text_secondary());
        inner.take_top(tokens::space::sm);
        value_text(r, inner.take_top(34), spec.value, spec.unit, Font::Metric, theme.text());
        inner.take_top(tokens::space::sm);
        TextStyle caption;
        caption.font = Font::Label;
        caption.valign = VAlign::Top;
        r.text(spec.caption, inner.take_top(16), caption, theme.text_tertiary());
        // Full-bleed sparkline at the bottom of the card.
        const Rect spark{card.x + 1, card.bottom() - 64 - 1, card.w - 2, 64};
        r.push_clip(spark);
        if (spec.series && spec.series->size() > 1) {
            Renderer::SparklineOptions options;
            options.end_dot = true;
            options.halo = true;
            r.sparkline(spark.inset(0, 4), spec.series->data(), spec.series->size(), history_window(), spec.series_max,
                        spec.kind, options);
        }
        if (spec.secondary && spec.secondary->size() > 1) {
            Renderer::SparklineOptions options;
            options.area = false;
            options.halo = false;
            Color end = rgba(metric_style(spec.kind).end);
            options.color_override = &end;
            r.sparkline(spark.inset(0, 4), spec.secondary->data(), spec.secondary->size(), history_window(),
                        spec.series_max, spec.kind, options);
        }
        r.pop_clip();
    }

    std::vector<std::pair<Rect, PageId>> cards_;
    std::vector<float> card_lift_;  // 0…1 hover progress per card
    double last_frame_ = 0;
    std::vector<TopAppRow> app_rows_;
    std::vector<pc_volume> volumes_;
    ScrollState scroll_;
    bool hover_ = false;
};

}  // namespace

std::unique_ptr<Page> make_overview_page() { return std::make_unique<OverviewPage>(); }

}  // namespace procyon::ui
