// Command palette (Ctrl+K) and the Get Info sheet.
#include "overlays.hpp"

#include <algorithm>

#include "widgets.hpp"

namespace procyon::ui {
namespace {

std::wstring lower(std::wstring s) {
    for (wchar_t &c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// Scrim behind a floating panel; a click on it closes the overlay.
void scrim(Renderer &r, const Rect &window) {
    r.fill(window, with_alpha(D2D1::ColorF(D2D1::ColorF::Black), r.theme().dark ? 0.5f : 0.25f));
}

void floating_panel(Renderer &r, const Rect &panel) {
    const Theme &theme = r.theme();
    for (int i = 6; i >= 1; --i)
        r.fill_round(panel.offset(0, 6).inset(-i * 2.0f, -i * 2.0f), tokens::radius::xl + i * 2.0f,
                     with_alpha(D2D1::ColorF(D2D1::ColorF::Black), 0.05f));
    r.fill_round(panel, tokens::radius::xl, theme.surface());
    r.stroke_round(panel, tokens::radius::xl, theme.border_strong());
}

// ---------------------------------------------------------------------------------------------
// Command palette
// ---------------------------------------------------------------------------------------------

class CommandPalette : public Overlay {
public:
    explicit CommandPalette(std::vector<PaletteItem> items) : items_(std::move(items)) {
        field_.placeholder = L"Jump to a screen, run a command, or find an app";
        field_.keycap = L"";
        field_.focused = true;
        filter();
    }

    void paint(Host &host, const Rect &window) override {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        scrim(r, window);
        const float width = std::min(560.0f, window.w - 2 * tokens::space::xl);
        const float rows = static_cast<float>(std::min<size_t>(matches_.size(), 9));
        const float height = 60 + rows * 40 + (rows ? tokens::space::md : 0);
        panel_ = Rect{window.cx() - width / 2, window.y + std::min(120.0f, window.h * 0.15f), width, height};
        floating_panel(r, panel_);
        Rect inner = panel_.inset(tokens::space::md, tokens::space::md);
        field_.bounds = inner.take_top(36);
        field_.paint(r);
        inner.take_top(tokens::space::sm);
        row_rects_.clear();
        for (size_t i = 0; i < matches_.size() && i < 9; ++i) {
            const PaletteItem &item = items_[matches_[i]];
            const Rect row = inner.take_top(40);
            row_rects_.push_back(row);
            if (static_cast<int>(i) == selected_)
                r.fill_round(row, tokens::radius::md, with_alpha(theme.accent(), 0.15f));
            Rect line = row.inset(tokens::space::md, 0);
            const Rect kind = line.take_left(64);
            const wchar_t *kind_label = item.kind == PaletteItem::Kind::Screen    ? L"Screen"
                                        : item.kind == PaletteItem::Kind::Command ? L"Command"
                                                                                  : L"App";
            r.badge(kind.x, kind.cy() - 9, kind_label,
                    item.kind == PaletteItem::Kind::App ? Renderer::Tone::Accent : Renderer::Tone::Neutral);
            line.take_left(tokens::space::sm);
            if (!item.shortcut.empty()) {
                const float w = r.measure(item.shortcut, Font::Caption) + tokens::space::sm;
                const Rect cap = line.take_right(w).inset(0, 11);
                r.fill_round(cap, tokens::radius::xs, theme.surface_raised());
                r.stroke_round(cap, tokens::radius::xs, theme.border_strong());
                TextStyle s;
                s.font = Font::Caption;
                s.halign = HAlign::Center;
                r.text(item.shortcut, cap, s, theme.text_secondary());
            }
            TextStyle t;
            t.font = Font::BodyMedium;
            t.valign = item.subtitle.empty() ? VAlign::Center : VAlign::Top;
            r.text(item.title, item.subtitle.empty() ? line : Rect{line.x, line.y + 5, line.w, 18}, t, theme.text());
            if (!item.subtitle.empty()) {
                TextStyle s;
                s.font = Font::Caption;
                s.valign = VAlign::Top;
                r.text(item.subtitle, Rect{line.x, line.y + 22, line.w, 14}, s, theme.text_secondary());
            }
        }
        if (matches_.empty()) {
            TextStyle s;
            s.font = Font::Body;
            s.halign = HAlign::Center;
            r.text(L"No matches", Rect{inner.x, inner.y, inner.w, 0}, s, theme.text_tertiary());
        }
    }

    void mouse_move(Host &, const MouseEvent &e) override {
        for (size_t i = 0; i < row_rects_.size(); ++i)
            if (row_rects_[i].contains(e.x, e.y)) selected_ = static_cast<int>(i);
    }

    void mouse_down(Host &host, const MouseEvent &e, bool) override {
        if (!panel_.contains(e.x, e.y)) {
            close();
            return;
        }
        for (size_t i = 0; i < row_rects_.size(); ++i)
            if (row_rects_[i].contains(e.x, e.y)) run(host, static_cast<int>(i));
        bool cleared = false;
        field_.mouse_down(e.x, e.y, cleared);
        field_.focused = true;
        if (cleared) filter();
    }

    bool key(Host &host, const KeyEvent &e) override {
        switch (e.vk) {
            case VK_ESCAPE: close(); return true;
            case VK_DOWN:
                selected_ = std::min(static_cast<int>(std::min<size_t>(matches_.size(), 9)) - 1, selected_ + 1);
                return true;
            case VK_UP: selected_ = std::max(0, selected_ - 1); return true;
            case VK_RETURN: run(host, selected_); return true;
            default: break;
        }
        bool consumed = false;
        if (field_.key(e, consumed)) filter();
        return consumed;
    }

    bool character(Host &, wchar_t c) override {
        if (field_.character(c)) {
            filter();
            return true;
        }
        return false;
    }

private:
    void filter() {
        matches_.clear();
        const std::wstring needle = lower(field_.text);
        for (size_t i = 0; i < items_.size(); ++i) {
            if (needle.empty() && items_[i].kind == PaletteItem::Kind::App) continue;  // apps only on demand
            if (needle.empty() || lower(items_[i].title).find(needle) != std::wstring::npos ||
                lower(items_[i].subtitle).find(needle) != std::wstring::npos)
                matches_.push_back(i);
        }
        // Screens, then commands, then apps; apps with more CPU first stay in their given order.
        std::stable_sort(matches_.begin(), matches_.end(), [&](size_t a, size_t b) {
            const bool pa = lower(items_[a].title).rfind(needle, 0) == 0,
                       pb = lower(items_[b].title).rfind(needle, 0) == 0;
            if (pa != pb) return pa;
            return static_cast<int>(items_[a].kind) < static_cast<int>(items_[b].kind);
        });
        selected_ = 0;
    }

    void run(Host &, int index) {
        if (index < 0 || index >= static_cast<int>(matches_.size())) return;
        const PaletteItem item = items_[matches_[static_cast<size_t>(index)]];
        close();
        if (item.run) item.run();
    }

    std::vector<PaletteItem> items_;
    std::vector<size_t> matches_;
    TextField field_;
    Rect panel_;
    std::vector<Rect> row_rects_;
    int selected_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Get Info
// ---------------------------------------------------------------------------------------------

class InfoSheet : public Overlay {
public:
    explicit InfoSheet(int32_t pid) : pid_(pid) {}

    void paint(Host &host, const Rect &window) override {
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        if (!loaded_) load(host);
        scrim(r, window);
        const float width = std::min(620.0f, window.w - 2 * tokens::space::xl);
        const float height = std::min(640.0f, window.h - 2 * tokens::space::xl);
        panel_ = Rect{window.cx() - width / 2, window.cy() - height / 2, width, height};
        floating_panel(r, panel_);
        Rect inner = panel_.inset(tokens::space::xl, tokens::space::lg);

        // Header: name, pid, close.
        Rect head = inner.take_top(44);
        close_ = head.take_right(28);
        r.symbol(Renderer::Symbol::Close, Rect{close_.cx() - 7, close_.cy() - 7, 14, 14}, theme.text_secondary());
        TextStyle t;
        t.font = Font::Title;
        t.valign = VAlign::Top;
        r.text(name_, Rect{head.x, head.y, head.w, 28}, t, theme.text());
        TextStyle s;
        s.font = Font::Caption;
        s.valign = VAlign::Top;
        r.text(subtitle_, Rect{head.x, head.y + 28, head.w, 16}, s, theme.text_secondary());
        inner.take_top(tokens::space::md);

        // Tabs.
        Rect tabs = inner.take_top(kControlHeight);
        tab_rects_.clear();
        const wchar_t *labels[] = {L"Info", L"Threads", L"Files", L"Network"};
        float x = tabs.x;
        for (int i = 0; i < 4; ++i) {
            const Rect b = button(r, x, tabs.y, labels[i], tab_ == i, false, false, 0, Font::Label);
            tab_rects_.push_back(b);
            x += b.w + tokens::space::xs;
        }
        // Actions on the right.
        const float cw = r.measure(L"Copy", Font::Label) + tokens::space::lg * 2;
        copy_ = button(r, tabs.right() - cw, tabs.y, L"Copy", false, false, false, cw, Font::Label);
        location_ = {};
        if (!path_.empty()) {
            const float lw = r.measure(L"Open File Location", Font::Label) + tokens::space::lg * 2;
            location_ = button(r, copy_.x - tokens::space::xs - lw, tabs.y, L"Open File Location", false, false, false,
                               lw, Font::Label);
        }
        inner.take_top(tokens::space::md);
        content_ = inner;
        content_.w -= 10;  // room for the scrollbar
        r.push_clip(content_);
        float y = content_.y - scroll_.offset;
        switch (tab_) {
            case 0: y = paint_info(r, content_, y); break;
            case 1: y = paint_threads(r, content_, y); break;
            case 2: y = paint_files(r, content_, y); break;
            case 3: y = paint_network(r, content_, y); break;
            default: break;
        }
        r.pop_clip();
        scroll_.viewport = content_.h;
        scroll_.content = y + scroll_.offset - content_.y;
        scroll_.clamp();
        scroll_.paint(r, Rect{content_.right() - 4, content_.y, 4, content_.h});
    }

    void mouse_down(Host &host, const MouseEvent &e, bool) override {
        if (!panel_.contains(e.x, e.y) || close_.contains(e.x, e.y)) {
            close();
            return;
        }
        for (size_t i = 0; i < tab_rects_.size(); ++i) {
            if (tab_rects_[i].contains(e.x, e.y)) {
                tab_ = static_cast<int>(i);
                scroll_.offset = 0;
                if (tab_ >= 2 && !handles_loaded_) load_handles(host);
            }
        }
        if (copy_.contains(e.x, e.y)) host.copy_to_clipboard(copy_text());
        if (!location_.empty() && location_.contains(e.x, e.y)) host.open_in_explorer(path_);
    }

    void wheel(Host &, const MouseEvent &e) override { scroll_.wheel(e.wheel, 60); }

    bool key(Host &host, const KeyEvent &e) override {
        switch (e.vk) {
            case VK_ESCAPE: close(); return true;
            case VK_NEXT: scroll_.page(1); return true;
            case VK_PRIOR: scroll_.page(-1); return true;
            case VK_DOWN: scroll_.wheel(-120, 60); return true;
            case VK_UP: scroll_.wheel(120, 60); return true;
            case 'C':
                if (e.ctrl) {
                    host.copy_to_clipboard(copy_text());
                    return true;
                }
                return false;
            case VK_TAB:
                tab_ = (tab_ + (e.shift ? 3 : 1)) % 4;
                scroll_.offset = 0;
                if (tab_ >= 2 && !handles_loaded_) load_handles(host);
                return true;
            default: return false;
        }
    }

    void tick(Host &host) override {
        // Threads and the live numbers refresh every few seconds; files and sockets on demand.
        if (++ticks_ % 3 == 0) load(host);
    }

private:
    void load(Host &host) {
        loaded_ = true;
        Store &store = host.store();
        const pc_process *p = store.snapshot().find(pid_);
        if (p) {
            process_ = *p;
            name_ = process_display_name(*p);
            path_ = fmt::from_utf8(p->path);
            subtitle_ = L"PID " + std::to_wstring(p->pid);
            if (p->user[0]) subtitle_ += L" · " + fmt::from_utf8(p->user);
            if (p->start_time > 0) subtitle_ += L" · started " + fmt::date_time(p->start_time);
            gone_ = false;
        } else if (name_.empty()) {
            name_ = L"PID " + std::to_wstring(pid_);
            subtitle_ = L"This process has ended.";
            gone_ = true;
        } else {
            subtitle_ = L"This process has ended.";
            gone_ = true;
        }
        details_ = store.details(pid_);
    }

    void load_handles(Host &host) {
        handles_loaded_ = true;
        files_ = host.store().open_files(pid_, files_complete_);
        connections_ = host.store().connections(pid_, connections_complete_);
    }

    float row(Renderer &r, const Rect &area, float y, std::wstring_view label, std::wstring_view value,
              bool mono = false) {
        const Theme &theme = r.theme();
        TextStyle l;
        l.font = Font::Caption;
        l.uppercase = true;
        l.tracking = 0.6f;
        l.valign = VAlign::Top;
        r.text(label, Rect{area.x, y, 130, 16}, l, theme.text_tertiary());
        TextStyle v;
        v.font = mono ? Font::Mono : Font::Body;
        v.valign = VAlign::Top;
        v.wrap = true;
        v.trim = false;
        const float width = area.w - 140;
        // Height of wrapped text: lay it out at the width and measure.
        float lines = 1;
        const float text_w = r.measure(value, v.font);
        if (text_w > width) lines = std::ceil(text_w / width);
        const float height = std::max(18.0f, lines * (mono ? 16.0f : 18.0f));
        r.text(value.empty() ? std::wstring_view(fmt::unavailable) : value, Rect{area.x + 140, y, width, height}, v,
               value.empty() ? theme.text_tertiary() : theme.text());
        return y + height + tokens::space::sm;
    }

    float paint_info(Renderer &r, const Rect &area, float y) {
        const Theme &theme = r.theme();
        const pc_process &p = process_;
        y = row(r, area, y, L"Name", name_);
        y = row(r, area, y, L"PID",
                std::to_wstring(pid_) + (p.ppid > 0 ? L"  (parent " + std::to_wstring(p.ppid) + L")" : L""));
        y = row(r, area, y, L"User", p.user[0] ? fmt::from_utf8(p.user) : std::wstring(fmt::unavailable));
        y = row(r, area, y, L"Path", path_, true);
        y = row(r, area, y, L"State",
                p.state == PC_STATE_RUNNING    ? L"Running"
                : p.state == PC_STATE_STOPPED  ? L"Suspended"
                : p.state == PC_STATE_ZOMBIE   ? L"Exited"
                : p.state == PC_STATE_SLEEPING ? L"Waiting"
                                               : L"Unknown");
        y = row(r, area, y, L"Priority",
                p.nice <= -15 ? L"High"
                : p.nice < 0  ? L"Above normal"
                : p.nice == 0 ? L"Normal"
                : p.nice <= 8 ? L"Below normal"
                              : L"Low");
        y = row(r, area, y, L"CPU", fmt::cpu(p.cpu_percent));
        y = row(r, area, y, L"Memory", fmt::bytes(p.memory_bytes));
        y = row(r, area, y, L"Disk",
                L"read " + fmt::rate(p.disk_read_bps) + L" · write " + fmt::rate(p.disk_write_bps));
        if (p.gpu_percent >= 0) y = row(r, area, y, L"GPU", fmt::cpu(p.gpu_percent));
        y = row(r, area, y, L"Threads", fmt::count(p.threads));
        y = row(r, area, y, L"Started",
                p.start_time > 0 ? fmt::date_time(p.start_time) : std::wstring(fmt::unavailable));
        if (p.flags & PC_PROC_SYSTEM)
            y = row(r, area, y, L"Note", L"Windows system process: ending it needs confirmation.");
        y += tokens::space::sm;
        if (details_) {
            y = row(r, area, y, L"Working dir", details_->cwd, true);
            std::wstring command;
            for (const std::wstring &a : details_->arguments) command += (command.empty() ? L"" : L" ") + a;
            if (details_->info.arguments_known)
                y = row(r, area, y, L"Command line", command, true);
            else
                y = row(r, area, y, L"Command line", L"Not readable without administrator rights");
            if (!details_->environment.empty()) {
                r.panel_caption(Rect{area.x, y + 4, area.w, 20}, L"Environment");
                y += 28;
                TextStyle mono;
                mono.font = Font::Mono;
                mono.valign = VAlign::Top;
                for (const std::wstring &e : details_->environment) {
                    r.text(e, Rect{area.x, y, area.w, 16}, mono, theme.text_secondary());
                    y += 16;
                }
            }
        }
        return y;
    }

    float paint_threads(Renderer &r, const Rect &area, float y) {
        const Theme &theme = r.theme();
        if (!details_ || !details_->info.threads_known) {
            empty_state(r, area, L"Threads not readable", L"Administrator rights are needed for this process.");
            return y;
        }
        Rect head{area.x, y, area.w, 22};
        TextStyle h;
        h.font = Font::LabelSemibold;
        r.text(L"Thread", Rect{head.x, head.y, 100, head.h}, h, theme.text_secondary());
        r.text(L"Name", Rect{head.x + 100, head.y, head.w - 320, head.h}, h, theme.text_secondary());
        h.halign = HAlign::Right;
        r.text(L"CPU time", Rect{head.right() - 220, head.y, 110, head.h}, h, theme.text_secondary());
        r.text(L"Priority", Rect{head.right() - 100, head.y, 50, head.h}, h, theme.text_secondary());
        r.text(L"State", Rect{head.right() - 50, head.y, 50, head.h}, h, theme.text_secondary());
        y += 24;
        std::vector<ThreadCopy> threads = details_->threads;
        std::sort(threads.begin(), threads.end(), [](const ThreadCopy &a, const ThreadCopy &b) {
            return a.user_time_ns + a.system_time_ns > b.user_time_ns + b.system_time_ns;
        });
        for (const ThreadCopy &t : threads) {
            if (y + 22 < area.y) {
                y += 22;
                continue;
            }
            if (y > area.bottom()) {
                y += 22;
                continue;
            }
            TextStyle b;
            b.font = Font::Body;
            b.tabular = true;
            r.text(std::to_wstring(t.id), Rect{area.x, y, 100, 22}, b, theme.text());
            r.text(t.name.empty() ? std::wstring(fmt::unavailable) : t.name, Rect{area.x + 100, y, area.w - 320, 22}, b,
                   t.name.empty() ? theme.text_tertiary() : theme.text());
            b.halign = HAlign::Right;
            r.text(fmt::duration((t.user_time_ns + t.system_time_ns) / 1e9), Rect{area.right() - 220, y, 110, 22}, b,
                   theme.text_secondary());
            r.text(std::to_wstring(t.priority), Rect{area.right() - 100, y, 50, 22}, b, theme.text_secondary());
            r.text(t.state == PC_STATE_RUNNING    ? L"Run"
                   : t.state == PC_STATE_STOPPED  ? L"Susp"
                   : t.state == PC_STATE_SLEEPING ? L"Wait"
                                                  : L"",
                   Rect{area.right() - 50, y, 50, 22}, b, theme.text_secondary());
            y += 22;
        }
        return y;
    }

    float paint_files(Renderer &r, const Rect &area, float y) {
        const Theme &theme = r.theme();
        if (!handles_loaded_) return y;
        if (files_.empty()) {
            empty_state(r, area, files_complete_ ? L"No files open" : L"Not readable",
                        files_complete_ ? L"" : L"Administrator rights are needed for this process.");
            return y;
        }
        if (!files_complete_) {
            info_banner(r, Rect{area.x, y, area.w, 36}, L"Some handles could not be read without administrator rights.",
                        L"", Renderer::Tone::Warning);
            y += 44;
        }
        for (const OpenFileCopy &f : files_) {
            if (y + 20 >= area.y && y <= area.bottom()) {
                TextStyle k;
                k.font = Font::Caption;
                r.text(f.kind == PC_FILE_CWD         ? L"CWD"
                       : f.kind == PC_FILE_DIRECTORY ? L"DIR"
                       : f.kind == PC_FILE_OTHER     ? L"DEV"
                                                     : L"FILE",
                       Rect{area.x, y, 44, 20}, k, theme.text_tertiary());
                TextStyle m;
                m.font = Font::Mono;
                r.text(f.path, Rect{area.x + 48, y, area.w - 48, 20}, m, theme.text());
            }
            y += 20;
        }
        return y;
    }

    float paint_network(Renderer &r, const Rect &area, float y) {
        const Theme &theme = r.theme();
        if (!handles_loaded_) return y;
        if (connections_.empty()) {
            empty_state(r, area, L"No sockets", L"The process has no TCP or UDP endpoints.");
            return y;
        }
        for (const pc_connection &c : connections_) {
            if (y + 22 >= area.y && y <= area.bottom()) {
                TextStyle b;
                b.font = Font::Body;
                b.tabular = true;
                const std::wstring proto = c.protocol == PC_PROTOCOL_TCP ? (c.family == 6 ? L"TCP6" : L"TCP")
                                                                         : (c.family == 6 ? L"UDP6" : L"UDP");
                r.text(proto, Rect{area.x, y, 56, 22}, b, theme.text_secondary());
                r.text(fmt::from_utf8(c.local_address) + L":" + std::to_wstring(c.local_port),
                       Rect{area.x + 56, y, 190, 22}, b, theme.text());
                r.text(c.remote_address[0] ? fmt::from_utf8(c.remote_address) + L":" + std::to_wstring(c.remote_port)
                                           : std::wstring(fmt::unavailable),
                       Rect{area.x + 250, y, area.w - 350, 22}, b,
                       c.remote_address[0] ? theme.text() : theme.text_tertiary());
                const wchar_t *state = c.state == PC_TCP_LISTEN        ? L"Listening"
                                       : c.state == PC_TCP_ESTABLISHED ? L"Established"
                                       : c.state == PC_TCP_TIME_WAIT   ? L"Time wait"
                                       : c.state == PC_TCP_CLOSE_WAIT  ? L"Close wait"
                                       : c.state == PC_TCP_NONE        ? L""
                                                                       : L"Closing";
                b.halign = HAlign::Right;
                r.text(state, Rect{area.right() - 100, y, 100, 22}, b, theme.text_secondary());
            }
            y += 22;
        }
        return y;
    }

    std::wstring copy_text() const {
        std::wstring text = name_ + L"\nPID: " + std::to_wstring(pid_) + L"\nPath: " + path_ + L"\nUser: " +
                            fmt::from_utf8(process_.user);
        if (details_) {
            text += L"\nWorking directory: " + details_->cwd + L"\nCommand line:";
            for (const std::wstring &a : details_->arguments) text += L" " + a;
        }
        return text;
    }

    int32_t pid_;
    bool loaded_ = false, gone_ = false, handles_loaded_ = false;
    bool files_complete_ = true, connections_complete_ = true;
    int ticks_ = 0;
    int tab_ = 0;
    pc_process process_{};
    std::wstring name_, subtitle_, path_;
    std::optional<DetailsCopy> details_;
    std::vector<OpenFileCopy> files_;
    std::vector<pc_connection> connections_;
    Rect panel_, close_, copy_, location_, content_;
    std::vector<Rect> tab_rects_;
    ScrollState scroll_;
};

}  // namespace

std::unique_ptr<Overlay> make_command_palette(std::vector<PaletteItem> items) {
    return std::make_unique<CommandPalette>(std::move(items));
}

std::unique_ptr<Overlay> make_info_sheet(int32_t pid) { return std::make_unique<InfoSheet>(pid); }

}  // namespace procyon::ui
