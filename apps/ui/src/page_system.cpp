// System (facts about this PC) and Settings, laid out like the macOS SystemView and SettingsView.
#include <algorithm>

#include "os.hpp"
#include "pages.hpp"
#include "platform.hpp"
#include "version.h"

namespace procyon::ui {
namespace {

const wchar_t *const kRepository = L"https://github.com/manhpham90vn/Procyon";
const wchar_t *const kLatestRelease = L"https://github.com/manhpham90vn/Procyon/releases/latest";
const wchar_t *const kIssues = L"https://github.com/manhpham90vn/Procyon/issues";

class SystemPage : public Page {
public:
    PageId id() const override { return PageId::System; }
    std::wstring title() const override { return L"System"; }

    void activate(Host &host) override { volumes_ = host.store().volumes(); }
    void tick(Host &host) override {
        if (host.store().ticks() % 10 == 0) activate(host);
    }

    void paint(Host &host, const Rect &bounds) override {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        Store &store = host.store();
        const pc_system_info &info = store.system_info();
        const Snapshot &s = store.snapshot();
        Rect area = screen_area(bounds);
        r.push_clip(bounds);
        area.y -= scroll_.offset;
        float y = area.y;

        // Hero: the device, its name in display type, the chip in the CPU gradient, the OS line.
        {
            const Rect card{area.x, y, area.w, 160};
            r.card(card, MetricKind::Cpu, tokens::radius::xl);
            Rect inner = card.inset(tokens::space::xl, tokens::space::xl);
            const Rect device = inner.take_left(120);
            inner.take_left(tokens::space::xl);
            const Color start = rgba(tokens::metric::cpu.start);
            for (int i = 3; i >= 1; --i)
                r.fill_circle(device.cx(), device.cy(), 34 + i * 6.0f, with_alpha(start, 0.3f / 6));
            r.symbol(Renderer::Symbol::Desktop, Rect{device.cx() - 36, device.cy() - 36, 72, 72}, start, 2.2f);
            TextStyle display;
            display.font = Font::Display;
            display.valign = VAlign::Top;
            r.text(display_model(info), Rect{inner.x, inner.y, inner.w, 44}, display, theme.text());
            TextStyle chip;
            chip.font = Font::Title;
            chip.valign = VAlign::Top;
            r.text(fmt::from_utf8(info.cpu_brand), Rect{inner.x, inner.y + 48, inner.w, 28}, chip,
                   mix(start, rgba(tokens::metric::cpu.end), 0.35f));
            TextStyle os;
            os.font = Font::Body;
            os.valign = VAlign::Top;
            r.text(fmt::from_utf8(info.os_name) + L" " + fmt::from_utf8(info.os_version) + L" (" +
                       fmt::from_utf8(info.os_build) + L") · " + fmt::from_utf8(info.hostname),
                   Rect{inner.x, inner.y + 80, inner.w, 20}, os, theme.text_secondary());
            y = card.bottom() + kSectionGap;
        }

        // Hardware and Software side by side, two stat columns each, equal heights.
        const double uptime = s.timestamp > 0 && info.boot_time > 0 ? s.timestamp - info.boot_time : 0;
        std::vector<Stat> hardware = {
            {L"Model", display_model(info), fmt::from_utf8(info.model_id)},
            {L"Chip", fmt::from_utf8(info.cpu_brand), fmt::from_utf8(info.arch)},
            {L"Cores", core_summary(info, store), std::to_wstring(info.logical_cores) + L" logical"},
            {L"Memory", fmt::bytes(static_cast<int64_t>(info.memory_total)), L""},
        };
        if (info.cpu_frequency_hz)
            hardware.push_back({L"Frequency", fmt::number(info.cpu_frequency_hz / 1e9, 2) + L" GHz", L""});
        const std::vector<Stat> software = {
            {L"Operating system", fmt::from_utf8(info.os_name) + L" " + fmt::from_utf8(info.os_version),
             L"Build " + fmt::from_utf8(info.os_build)},
            {L"Hostname", fmt::from_utf8(info.hostname), L""},
            {L"Uptime", fmt::duration(uptime), L"Since " + fmt::date_time(info.boot_time)},
            {L"Processes", fmt::count(s.process_count), fmt::count(s.thread_count) + L" threads"},
        };
        {
            const float w = (area.w - tokens::space::lg) / 2;
            const float content_w = w - tokens::space::lg * 2;
            const float h = kPanelChrome + std::max(stat_grid_height(content_w, hardware.size(), 140, 2),
                                                    stat_grid_height(content_w, software.size(), 140, 2));
            Rect left{area.x, y, w, h};
            Rect right{area.x + w + tokens::space::lg, y, w, h};
            stat_grid(r, panel(r, left, L"Hardware", Renderer::Symbol::Desktop), hardware, 140, 2);
            stat_grid(r, panel(r, right, L"Software", Renderer::Symbol::Grid), software, 140, 2);
            y += h + kSectionGap;
        }

        // Kernel: the version string in monospace.
        {
            const Rect card{area.x, y, area.w, kPanelChrome + 20};
            Rect inner = panel(r, card, L"Kernel", Renderer::Symbol::Bullets);
            TextStyle mono;
            mono.font = Font::Mono;
            r.text(fmt::from_utf8(info.kernel), inner, mono, theme.text_secondary());
            y = card.bottom() + kSectionGap;
        }

        // Volumes, like the Disk screen.
        const float pitch = 34 + tokens::space::lg;
        const Rect card{area.x, y, area.w,
                        kPanelChrome + std::max<size_t>(1, volumes_.size()) * pitch - tokens::space::lg};
        Rect inner = panel(r, card, L"Volumes", Renderer::Symbol::Drive);
        if (volumes_.empty()) empty_state(r, inner, L"No volumes", L"");
        for (const pc_volume &vol : volumes_) {
            Rect row = inner.take_top(34);
            inner.take_top(tokens::space::lg);
            const Rect icon = row.take_left(32 + tokens::space::md);
            r.symbol(Renderer::Symbol::Drive, Rect{icon.x + 6, icon.y + 7, 20, 20}, rgba(tokens::metric::disk.start),
                     1.6f);
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
        y = card.bottom() + tokens::space::xxl;
        r.pop_clip();
        scroll_.viewport = bounds.h;
        scroll_.content = y + scroll_.offset - bounds.y;
        scroll_.clamp();
    }

    void wheel(Host &, const MouseEvent &e) override { scroll_.wheel(e.wheel); }

private:
    static std::wstring core_summary(const pc_system_info &info, const Store &store) {
        if (store.has(PC_CAP_HYBRID_CORES))
            return std::to_wstring(info.performance_cores) + L"P + " + std::to_wstring(info.efficiency_cores) +
                   L"E cores";
        return std::to_wstring(info.physical_cores) + L" cores";
    }

    ScrollState scroll_;
    std::vector<pc_volume> volumes_;
};

// Settings: a grouped form like the macOS one, at most 720 wide; each row a label on the left
// and its control on the right, captions in tertiary type below a section's rows.
class SettingsPage : public Page {
public:
    PageId id() const override { return PageId::Settings; }
    std::wstring title() const override { return L"Settings"; }

    void paint(Host &host, const Rect &bounds) override {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        Settings &settings = host.settings();
        Store &store = host.store();
        const float mx = host.mouse_x(), my = host.mouse_y();
        mouse_x_ = mx;
        mouse_y_ = my;
        Rect area = screen_area(bounds);
        r.push_clip(bounds);
        area.y -= scroll_.offset;
        area = page_header(r, area, L"Settings",
                           platform::tray_available()
                               ? L"Updates, full access, notification area, alerts, appearance, processes "
                                 L"and about"
                               : L"Updates, full access, background, alerts, appearance, processes and about",
                           std::nullopt);
        area.w = std::min(area.w, 720.0f);
        hits_.clear();
        links_.clear();
        float y = area.y;
        constexpr float kRow = 40;
        constexpr float kCaption = 18;

        auto label = [&](const Rect &row, std::wstring_view text, bool secondary = false) {
            TextStyle l;
            l.font = Font::Body;
            r.text(text, row, l, secondary ? theme.text_secondary() : theme.text());
        };
        auto caption = [&](Rect &inner, std::wstring_view text, int lines = 1) {
            const Rect row = inner.take_top(kCaption * lines);
            TextStyle c;
            c.font = Font::Caption;
            c.valign = VAlign::Top;
            c.wrap = lines > 1;
            r.text(text, Rect{row.x, row.y + 2, row.w, row.h}, c, theme.text_tertiary());
        };
        auto choices = [&](const Rect &row, const std::vector<std::wstring> &labels, int current, int group,
                           const std::vector<int> &values) {
            const float w = r.segmented_width(labels);
            const std::vector<Rect> rects =
                r.segmented(Rect{row.right() - w, row.cy() - 14, w, kControlHeight}, labels, current);
            for (size_t i = 0; i < rects.size(); ++i) hits_.push_back({rects[i], group, values[i]});
        };
        auto toggle = [&](const Rect &row, bool on, int group, int value) {
            r.toggle_switch(row.right() - 36, row.cy() - 10, on, true);
            hits_.push_back({Rect{row.right() - 36, row.cy() - 10, 36, 20}, group, value});
        };
        auto link = [&](const Rect &row, std::wstring_view text, const wchar_t *url) {
            TextStyle l;
            l.font = Font::Body;
            l.halign = HAlign::Right;
            const float w = r.measure(text, Font::Body) + 4;
            const Rect rect{row.right() - w, row.y, w, row.h};
            r.text(text, rect, l, theme.accent());
            if (rect.contains(mx, my))
                r.line(rect.x + 2, rect.bottom() - 10, rect.right() - 2, rect.bottom() - 10, theme.accent());
            links_.push_back({rect, url});
        };
        auto section = [&](std::wstring_view title, Renderer::Symbol symbol, float content_h,
                           const std::function<void(Rect &)> &body) {
            const Rect card{area.x, y, area.w, kPanelChrome + content_h - tokens::space::sm};
            Rect inner = panel(r, card, title, symbol);
            body(inner);
            y = card.bottom() + kSectionGap;
        };

        section(L"Updates", Renderer::Symbol::Refresh, kRow, [&](Rect &inner) {
            const Rect row = inner.take_top(kRow);
            label(row, L"Update speed");
            choices(row, {L"Every 0.5 s", L"Every 1 s", L"Every 2 s", L"Every 5 s"},
                    settings.interval <= 0.5 ? 0
                    : settings.interval <= 1 ? 1
                    : settings.interval <= 2 ? 2
                                             : 3,
                    GroupInterval, {500, 1000, 2000, 5000});
        });

        section(
            L"Full access", Renderer::Symbol::Lock, kRow + kCaption * 3 + tokens::space::sm + kRow, [&](Rect &inner) {
                Rect row = inner.take_top(kRow);
                label(row, L"Status");
                const bool on = host.elevated();
                const float bw =
                    r.badge(0, 0, on ? L"On" : L"Off", on ? Renderer::Tone::Success : Renderer::Tone::Neutral, true);
                r.badge(row.right() - bw, row.cy() - 8.5f, on ? L"On" : L"Off",
                        on ? Renderer::Tone::Success : Renderer::Tone::Neutral);
                caption(inner,
                        std::wstring(L"Full access runs Procyon as ") + os::admin + L". " + os::full_access_caption, 3);
                inner.take_top(tokens::space::sm);
                row = inner.take_top(kRow);
                if (on) {
                    label(row, std::wstring(L"Procyon is running as ") + os::admin + L" for this session.", true);
                } else {
                    label(row, os::elevation, true);
                    const float w = r.measure(L"Unlock Full Access", Font::BodyMedium) + tokens::space::md * 2;
                    const Rect b = button(r, row.right() - w, row.cy() - kControlHeight / 2, L"Unlock Full Access",
                                          true, false, Rect{row.right() - w, row.cy() - 14, w, 28}.contains(mx, my), w);
                    hits_.push_back({b, GroupUnlock, 0});
                }
            });

        // Notification area: the macOS "Menu bar" section, module by module.
        {
            struct Module {
                const wchar_t *title;
                int bit;
                bool available;
            };
            const std::vector<Module> modules = {{L"CPU", 1, true},
                                                 {L"Memory", 2, true},
                                                 {L"Network", 4, true},
                                                 {L"GPU", 8, store.has(PC_CAP_GPU)},
                                                 {L"Temperature", 16, store.has(PC_CAP_TEMPERATURE)},
                                                 {L"Battery", 32, store.has(PC_CAP_BATTERY)}};
            // Without a tray there is nowhere to put figures: only the keep-running switch.
            const bool figures = platform::tray_available() && (settings.minimize_to_tray || os::tray_while_open);
            size_t shown = 0;
            for (const Module &m : modules) shown += figures && m.available ? 1 : 0;
            section(platform::tray_available() ? L"Notification area" : L"Background", Renderer::Symbol::Grid,
                    kRow * (1 + shown) + kCaption * 2, [&](Rect &inner) {
                        Rect row = inner.take_top(kRow);
                        label(row, platform::tray_available()
                                       ? L"Keep running in the tray when the window is closed"
                                       : L"Keep running in the background when the window is closed");
                        toggle(row, settings.minimize_to_tray, GroupTray, settings.minimize_to_tray ? 0 : 1);
                        if (figures) {
                            for (const Module &m : modules) {
                                if (!m.available) continue;
                                row = inner.take_top(kRow);
                                label(Rect{row.x + tokens::space::lg, row.y, row.w, row.h}, m.title, true);
                                toggle(row, (settings.tray_modules & m.bit) != 0, GroupTrayModule, m.bit);
                            }
                        }
                        caption(
                            inner,
                            !settings.minimize_to_tray ? L"Closing the window quits Procyon."
                            : platform::tray_available()
                                ? os::tray_figures
                                : L"Closing the window keeps Procyon sampling for alerts and History. Open it again "
                                  L"from the app grid; Ctrl+Q quits.",
                            2);
                    });
        }

        // Alerts: a switch per rule; an enabled rule unfolds its threshold and duration.
        {
            const std::vector<AlertKind> kinds = alert_kinds(store);
            const AlertSettings &alerts = store.alert_settings();
            const std::vector<AlertEvent> &recent = store.recent_alerts();
            float content = kCaption * 2 + (recent.empty() ? 0 : kRow);
            for (AlertKind kind : kinds) {
                const AlertRule &rule = alerts.rule(kind);
                content += kRow;
                if (rule.enabled) content += kRow * (kind == AlertKind::MemoryPressure ? 1 : 2);
            }
            section(L"Alerts", Renderer::Symbol::Bell, content, [&](Rect &inner) {
                const std::vector<std::wstring> durations = {L"10 s", L"30 s", L"1 min", L"5 min", L"15 min"};
                for (size_t k = 0; k < kinds.size(); ++k) {
                    const AlertKind kind = kinds[k];
                    const AlertRule &rule = alerts.rule(kind);
                    Rect row = inner.take_top(kRow);
                    label(row, alert_title(kind));
                    toggle(row, rule.enabled, GroupAlertToggle, static_cast<int>(k));
                    if (!rule.enabled) continue;
                    const Rect indent{row.x + tokens::space::lg, 0, row.w - tokens::space::lg, 0};
                    if (kind != AlertKind::MemoryPressure) {
                        row = inner.take_top(kRow);
                        label(Rect{indent.x, row.y, indent.w, row.h}, L"Over", true);
                        const Rect plus{row.right() - 24, row.cy() - 12, 24, 24};
                        r.icon_button(plus, Renderer::Symbol::Plus, theme.text_secondary(), plus.contains(mx, my),
                                      rule.threshold < alert_threshold_max(kind));
                        hits_.push_back({plus, GroupAlertPlus, static_cast<int>(k)});
                        TextStyle value;
                        value.font = Font::BodyMedium;
                        value.halign = HAlign::Center;
                        value.tabular = true;
                        r.text(alert_threshold_text(kind, rule.threshold, settings.fahrenheit),
                               Rect{plus.x - 72, row.y, 72, row.h}, value, theme.text());
                        const Rect minus{plus.x - 72 - 24, row.cy() - 12, 24, 24};
                        r.icon_button(minus, Renderer::Symbol::Minus, theme.text_secondary(), minus.contains(mx, my),
                                      rule.threshold > alert_threshold_min(kind));
                        hits_.push_back({minus, GroupAlertMinus, static_cast<int>(k)});
                    }
                    row = inner.take_top(kRow);
                    label(Rect{indent.x, row.y, indent.w, row.h}, L"For at least", true);
                    int selected = 2;
                    for (size_t i = 0; i < std::size(AlertSettings::kDurations); ++i)
                        if (AlertSettings::kDurations[i] == rule.duration) selected = static_cast<int>(i);
                    std::vector<int> values;
                    for (size_t i = 0; i < durations.size(); ++i) values.push_back(static_cast<int>(k * 16 + i));
                    choices(row, durations, selected, GroupAlertDuration, values);
                }
                caption(inner,
                        platform::tray_available()
                            ? L"An alert fires when the condition lasts for the chosen time, then stays quiet for 15 "
                              L"minutes. Watching apps keeps Procyon reading every process while it sits in the "
                              L"notification area, which costs a little more CPU."
                            : L"An alert fires when the condition lasts for the chosen time, then stays quiet for 15 "
                              L"minutes. Watching apps keeps Procyon reading every process while it runs in the "
                              L"background, which costs a little more CPU.",
                        2);
                if (!recent.empty()) {
                    const Rect row = inner.take_top(kRow);
                    label(row, L"Last alert");
                    TextStyle last;
                    last.font = Font::Body;
                    last.halign = HAlign::Right;
                    r.text(recent.front().title + L" · " + fmt::time_of_day(static_cast<int64_t>(recent.front().time)),
                           Rect{row.x + 100, row.y, row.w - 100, row.h}, last, theme.text_secondary());
                }
            });
        }

        section(L"Appearance", Renderer::Symbol::Sparkle, kRow * 2, [&](Rect &inner) {
            Rect row = inner.take_top(kRow);
            label(row, L"Theme");
            choices(row, {L"System", L"Light", L"Dark"}, settings.theme, GroupTheme, {0, 1, 2});
            row = inner.take_top(kRow);
            label(row, L"Temperature");
            choices(row, {L"Celsius (°C)", L"Fahrenheit (°F)"}, settings.fahrenheit ? 1 : 0, GroupTemperature, {0, 1});
        });

        section(L"Processes", Renderer::Symbol::List, kRow * 2, [&](Rect &inner) {
            Rect row = inner.take_top(kRow);
            label(row, L"Default view");
            choices(row, {L"All Processes", L"By App", L"Tree"},
                    settings.default_view == PC_VIEW_FLAT   ? 0
                    : settings.default_view == PC_VIEW_TREE ? 2
                                                            : 1,
                    GroupDefaultView, {PC_VIEW_FLAT, PC_VIEW_GROUPED, PC_VIEW_TREE});
            row = inner.take_top(kRow);
            label(row, L"CPU %");
            TextStyle note;
            note.font = Font::Body;
            note.halign = HAlign::Right;
            r.text(L"Share of one core; 100% = one fully busy core", row, note, theme.text_secondary());
        });

        section(L"About", Renderer::Symbol::Info, kRow * 4 + kCaption * 2, [&](Rect &inner) {
            Rect row = inner.take_top(kRow);
            label(row, L"Version");
            TextStyle v;
            v.font = Font::Body;
            v.halign = HAlign::Right;
            std::wstring version = L"" PROCYON_VERSION_STRING;
            if (PROCYON_BUILD_COMMIT[0] != '\0') version += L" · " + fmt::from_utf8(PROCYON_BUILD_COMMIT);
            version += L" · core API v" + std::to_wstring(PC_API_VERSION);
            r.text(version, row, v, theme.text_secondary());
            row = inner.take_top(kRow);
            label(row, L"Source code");
            link(row, L"github.com/manhpham90vn/Procyon", kRepository);
            row = inner.take_top(kRow);
            label(row, L"Releases");
            link(row, L"Latest release", kLatestRelease);
            row = inner.take_top(kRow);
            label(row, L"Feedback");
            link(row, L"Report an issue", kIssues);
            caption(inner,
                    L"Procyon is open source under the MIT license. Fonts: Inter and Nunito (SIL OFL 1.1). "
                    L"Icons: Lucide (ISC).",
                    2);
        });

        y += tokens::space::xxl - kSectionGap;
        r.pop_clip();
        scroll_.viewport = bounds.h;
        scroll_.content = y + scroll_.offset - bounds.y;
        scroll_.clamp();
    }

    void mouse_move(Host &, const MouseEvent &e) override {
        mouse_x_ = e.x;
        mouse_y_ = e.y;
    }
    Cursor cursor() const override {
        for (const Link &l : links_)
            if (l.rect.contains(mouse_x_, mouse_y_)) return Cursor::Hand;
        for (const Hit &h : hits_)
            if (h.rect.contains(mouse_x_, mouse_y_)) return Cursor::Hand;
        return Cursor::Arrow;
    }

    void mouse_down(Host &host, const MouseEvent &e, bool right) override {
        if (right) return;
        Settings &settings = host.settings();
        for (const Link &l : links_) {
            if (!l.rect.contains(e.x, e.y)) continue;
            host.open_url(l.url);
            return;
        }
        for (const Hit &h : hits_) {
            if (!h.rect.contains(e.x, e.y)) continue;
            switch (h.group) {
                case GroupInterval:
                    settings.interval = h.value / 1000.0;
                    host.store().set_interval(settings.interval);
                    break;
                case GroupTheme: settings.theme = h.value; break;
                case GroupTemperature: settings.fahrenheit = h.value == 1; break;
                case GroupTray: settings.minimize_to_tray = h.value == 1; break;
                case GroupTrayModule: settings.tray_modules ^= h.value; break;
                case GroupDefaultView: settings.default_view = h.value; break;
                case GroupUnlock: host.relaunch_elevated(); break;
                case GroupAlertToggle:
                case GroupAlertPlus:
                case GroupAlertMinus:
                case GroupAlertDuration: {
                    const std::vector<AlertKind> kinds = alert_kinds(host.store());
                    const size_t k = static_cast<size_t>(h.group == GroupAlertDuration ? h.value / 16 : h.value);
                    if (k >= kinds.size()) break;
                    AlertSettings alerts = host.store().alert_settings();
                    AlertRule &rule = alerts.rule(kinds[k]);
                    const double step = alert_step(rule.kind);
                    switch (h.group) {
                        case GroupAlertToggle: rule.enabled = !rule.enabled; break;
                        case GroupAlertPlus:
                            rule.threshold = std::min(rule.threshold + step, alert_threshold_max(rule.kind));
                            break;
                        case GroupAlertMinus:
                            rule.threshold = std::max(rule.threshold - step, alert_threshold_min(rule.kind));
                            break;
                        default: rule.duration = AlertSettings::kDurations[h.value % 16]; break;
                    }
                    host.store().set_alert_settings(alerts);
                    break;
                }
                default: break;
            }
            host.repaint();
            return;
        }
    }

    void wheel(Host &, const MouseEvent &e) override { scroll_.wheel(e.wheel); }

private:
    enum Group {
        GroupInterval = 1,
        GroupTheme,
        GroupTemperature,
        GroupTray,
        GroupTrayModule,
        GroupDefaultView,
        GroupUnlock,
        GroupAlertToggle,
        GroupAlertPlus,
        GroupAlertMinus,
        GroupAlertDuration,
    };

    // The rules this machine can judge: no temperature rule without a sensor.
    static std::vector<AlertKind> alert_kinds(const Store &store) {
        std::vector<AlertKind> kinds;
        for (AlertKind kind : kAlertKinds) {
            if (kind == AlertKind::Temperature && !store.has(PC_CAP_TEMPERATURE)) continue;
            if (kind == AlertKind::MemoryPressure && !store.has(PC_CAP_MEMORY_PRESSURE)) continue;
            kinds.push_back(kind);
        }
        return kinds;
    }

    struct Hit {
        Rect rect;
        int group;
        int value;
    };
    struct Link {
        Rect rect;
        const wchar_t *url;
    };
    std::vector<Hit> hits_;
    std::vector<Link> links_;
    ScrollState scroll_;
    float mouse_x_ = -1, mouse_y_ = -1;
};

}  // namespace

std::unique_ptr<Page> make_system_page() { return std::make_unique<SystemPage>(); }
std::unique_ptr<Page> make_settings_page() { return std::make_unique<SettingsPage>(); }

}  // namespace procyon::ui
