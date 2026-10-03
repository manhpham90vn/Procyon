// Startup (Run keys and Startup folders) and Services (Service Control Manager), laid out like
// the macOS StartupView and ServicesView: header with controls, notice, table card, footnote.
#include <algorithm>

#include "pages.hpp"
#include "widgets.hpp"

#include <shellapi.h>

namespace procyon::ui {
namespace {

enum Columns { ColName = 1, ColStarts, ColStatus, ColImpact, ColEnabled, ColDomain, ColProgram };

enum MenuIds {
    MenuEnable = 1,
    MenuDisable,
    MenuStart,
    MenuStop,
    MenuRestart,
    MenuOpenLocation,
    MenuShowProcess,
    MenuCopy
};

bool contains_ci(const std::wstring &haystack, const std::wstring &needle) {
    if (needle.empty()) return true;
    std::wstring a = haystack, b = needle;
    for (wchar_t &c : a) c = static_cast<wchar_t>(towlower(c));
    for (wchar_t &c : b) c = static_cast<wchar_t>(towlower(c));
    return a.find(b) != std::wstring::npos;
}

// The table card below the header, down to the page's bottom padding (minus a footnote).
Rect table_area(const Rect &bounds, Rect &area, float footnote) {
    area.h = bounds.bottom() - tokens::space::xl - footnote - area.y;
    return area;
}

// ---------------------------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------------------------

class StartupPage : public Page {
public:
    StartupPage() {
        table_.columns = {{ColName, L"Name", 320, 1, HAlign::Left, true, false},
                          {ColStarts, L"Starts", 110, 0, HAlign::Left, true, false},
                          {ColStatus, L"Status", 150, 0, HAlign::Left, true, false},
                          {ColImpact, L"Impact", 110, 0, HAlign::Left},
                          {ColEnabled, L"Enabled", 64, 0, HAlign::Left, true, false}};
        table_.sort_column = ColName;
        table_.sort_descending = false;
        table_.row_height = 48;
        table_.row_count = [this] { return static_cast<int>(filtered_.size()); };
        table_.cell = [this](int row, int column) { return cell(row, column); };
        table_.on_sort = [this](int) { sort(); };
        table_.on_context = [this](int row, float x, float y) { context(row, x, y); };
        table_.on_activate = [this](int row) {
            if (host_ && item(row).pid > 0) host_->show_info(item(row).pid);
        };
        table_.on_switch = [this](int row, int) { toggle(row); };
    }
    PageId id() const override { return PageId::Startup; }
    std::wstring title() const override { return L"Startup"; }

    void activate(Host &host) override {
        host_ = &host;
        items_ = host.store().startup_items();
        loaded_ = true;
        sort();
    }
    void tick(Host &host) override {
        if (host.store().ticks() % 5 == 0) activate(host);
    }

    void paint(Host &host, const Rect &bounds) override {
        host_ = &host;
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        const float mx = host.mouse_x(), my = host.mouse_y();
        const int enabled = static_cast<int>(
            std::count_if(items_.begin(), items_.end(), [](const pc_startup_item &i) { return i.enabled; }));
        const int running = static_cast<int>(
            std::count_if(items_.begin(), items_.end(), [](const pc_startup_item &i) { return i.pid > 0; }));
        Rect area = screen_area(bounds);
        Rect trailing;
        area = page_header(r, area, L"Startup",
                           loaded_ ? std::to_wstring(items_.size()) + L" items · " + std::to_wstring(enabled) +
                                         L" enabled · " + std::to_wstring(running) + L" running"
                                   : L"Loading…",
                           std::nullopt, &trailing);
        area.y -= kSectionGap - tokens::space::lg;
        area.h += kSectionGap - tokens::space::lg;
        // Header controls: Task Manager's startup settings, refresh.
        refresh_button_ = Rect{trailing.right() - 28, trailing.y + 2, 28, 28};
        r.icon_button(refresh_button_, Renderer::Symbol::Refresh, theme.text_secondary(),
                      refresh_button_.contains(mx, my));
        settings_button_ =
            button(r, 0, trailing.y + 2, L"Startup Apps Settings…", false, false, settings_button_.contains(mx, my));
        settings_button_ = button(r, refresh_button_.x - tokens::space::sm - settings_button_.w, trailing.y + 2,
                                  L"Startup Apps Settings…", false, false, settings_button_.contains(mx, my));

        banner_button_ = {};
        if (!host.elevated()) {
            const Banner b =
                action_banner(r, area.take_top(kActionBannerHeight), L"Machine-wide startup entries are locked",
                              L"Entries for all users (HKLM and the common Startup folder) can only be switched with "
                              L"administrator access.",
                              L"Unlock Full Access", Renderer::Tone::Accent, banner_button_.contains(mx, my));
            banner_button_ = b.button;
            area.take_top(tokens::space::lg);
        }
        const float footnote = 36;
        area = table_area(bounds, area, footnote + tokens::space::md);
        r.card(area);
        const Rect inner = area.inset(1, 1);
        table_.layout(inner);
        r.push_clip(inner);
        table_.paint(r);
        r.pop_clip();
        if (filtered_.empty() && loaded_)
            empty_state(r, inner, L"Nothing starts automatically", L"No startup entries are registered for this user.");
        TextStyle note;
        note.font = Font::Caption;
        note.wrap = true;
        note.valign = VAlign::Top;
        r.text(
            L"Turning an item off takes effect at the next sign-in; a running copy keeps running. Procyon writes the "
            L"same StartupApproved setting as Task Manager, so both agree.",
            Rect{area.x, area.bottom() + tokens::space::md, area.w, footnote}, note, theme.text_tertiary());
    }

    void mouse_move(Host &, const MouseEvent &e) override { table_.mouse_move(e); }
    void mouse_leave(Host &) override { table_.mouse_leave(); }
    void mouse_down(Host &host, const MouseEvent &e, bool right) override {
        if (!right && refresh_button_.contains(e.x, e.y)) {
            activate(host);
            return;
        }
        if (!right && settings_button_.contains(e.x, e.y)) {
            ShellExecuteW(host.hwnd(), L"open", L"ms-settings:startupapps", nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (!banner_button_.empty() && banner_button_.contains(e.x, e.y)) {
            host.relaunch_elevated();
            return;
        }
        table_.mouse_down(e, right);
    }
    void double_click(Host &, const MouseEvent &e) override { table_.double_click(e); }
    void wheel(Host &, const MouseEvent &e) override { table_.wheel(e); }
    bool key(Host &, const KeyEvent &e) override {
        if (e.vk == VK_SPACE && table_.focused && table_.selected >= 0) {
            toggle(table_.selected);
            return true;
        }
        return table_.key(e);
    }
    int32_t selected_pid() const override {
        return table_.selected >= 0 && table_.selected < static_cast<int>(filtered_.size()) ? item(table_.selected).pid
                                                                                            : 0;
    }

private:
    const pc_startup_item &item(int row) const { return items_[filtered_[static_cast<size_t>(row)]]; }

    static std::wstring scope_name(int32_t scope) {
        switch (scope) {
            case PC_STARTUP_USER_AGENT: return L"At your sign-in";
            case PC_STARTUP_GLOBAL_AGENT: return L"At every sign-in";
            default: return L"At startup";
        }
    }

    const pc_process *process(const pc_startup_item &i) const {
        return host_ && i.pid > 0 ? host_->store().snapshot().find(i.pid) : nullptr;
    }

    double impact(const pc_startup_item &i) const {
        const pc_process *p = process(i);
        return p ? std::max(0.0, p->cpu_percent) + (p->memory_bytes > 0 ? p->memory_bytes / 1e9 : 0) : -1;
    }

    Cell cell(int row, int column) {
        const pc_startup_item &i = item(row);
        Cell c;
        switch (column) {
            case ColName:
                c.text = fmt::from_utf8(i.name);
                c.bold = true;
                c.subtitle = fmt::from_utf8(i.config_path);
                break;
            case ColStarts:
                c.text = scope_name(i.scope);
                c.color = host_ ? std::optional(host_->renderer().theme().text_secondary()) : std::nullopt;
                break;
            case ColStatus:
                c.text = i.pid > 0 ? L"Running · PID " + std::to_wstring(i.pid) : L"Not running";
                c.dim = i.pid <= 0;
                c.color = host_ ? std::optional(host_->renderer().theme().text_secondary()) : std::nullopt;
                break;
            case ColImpact: {
                const pc_process *p = process(i);
                if (!p) {
                    c.text = fmt::unavailable;
                    c.dim = true;
                } else {
                    const double cpu = std::max(0.0, p->cpu_percent);
                    const int64_t mem = std::max<int64_t>(0, p->memory_bytes);
                    const bool high = cpu >= 10 || mem >= 500ll * 1024 * 1024;
                    const bool medium = cpu >= 1 || mem >= 100ll * 1024 * 1024;
                    c.text = high ? L"High" : medium ? L"Medium" : L"Low";
                    c.badge = high     ? Renderer::Tone::Danger
                              : medium ? Renderer::Tone::Warning
                                       : Renderer::Tone::Success;
                }
                break;
            }
            case ColEnabled:
                c.toggle = i.enabled;
                c.toggle_enabled =
                    !i.managed_by_os && (host_ && (host_->elevated() || i.scope == PC_STARTUP_USER_AGENT));
                break;
            default: break;
        }
        return c;
    }

    void sort() {
        filtered_.clear();
        for (size_t i = 0; i < items_.size(); ++i) filtered_.push_back(i);
        const int column = table_.sort_column;
        const bool desc = table_.sort_descending;
        std::stable_sort(filtered_.begin(), filtered_.end(), [&](size_t a, size_t b) {
            const pc_startup_item &x = items_[a], &y = items_[b];
            int order = 0;
            switch (column) {
                case ColStarts: order = x.scope - y.scope; break;
                case ColStatus: order = (x.pid > 0 ? 0 : 1) - (y.pid > 0 ? 0 : 1); break;
                case ColImpact: {
                    const double ia = impact(x), ib = impact(y);
                    order = ia < ib ? 1 : ia > ib ? -1 : 0;
                    break;
                }
                case ColEnabled: order = (x.enabled ? 0 : 1) - (y.enabled ? 0 : 1); break;
                default: order = _stricmp(x.name, y.name); break;
            }
            if (order == 0) order = _stricmp(x.name, y.name);
            return desc ? order > 0 : order < 0;
        });
        table_.select(std::min(table_.selected, static_cast<int>(filtered_.size()) - 1));
    }

    void toggle(int row) {
        if (!host_ || row < 0 || row >= static_cast<int>(filtered_.size())) return;
        const pc_startup_item i = item(row);
        if (i.managed_by_os) {
            host_->alert(L"Managed by Windows", L"This item can only be changed in Settings.");
            return;
        }
        if (i.enabled && !host_->confirm(L"Turn off “" + fmt::from_utf8(i.name) + L"”?",
                                         L"It won't start the next time you sign in. A running copy keeps running.",
                                         L"Turn Off", true))
            return;
        const pc_result result = host_->store().startup_set_enabled(i.scope, i.label, !i.enabled);
        if (result != PC_OK)
            host_->report(result, i.enabled ? L"disable the startup item" : L"enable the startup item");
        activate(*host_);
    }

    void context(int row, float x, float y) {
        if (!host_) return;
        const pc_startup_item i = item(row);
        std::vector<MenuItem> items;
        items.push_back({i.enabled ? MenuDisable : MenuEnable, i.enabled ? L"Disable" : L"Enable", !i.managed_by_os});
        items.push_back({0, L"", true, false, true});
        items.push_back({MenuOpenLocation, L"Show in Explorer", i.app_path[0] != 0});
        items.push_back({MenuShowProcess, L"Show Process", i.pid > 0});
        items.push_back({MenuCopy, L"Copy Program Path"});
        switch (host_->popup_menu(items, x, y)) {
            case MenuEnable:
            case MenuDisable: toggle(row); break;
            case MenuOpenLocation: host_->open_in_explorer(fmt::from_utf8(i.app_path)); break;
            case MenuShowProcess: host_->show_info(i.pid); break;
            case MenuCopy: host_->copy_to_clipboard(fmt::from_utf8(i.program)); break;
            default: break;
        }
    }

    Host *host_ = nullptr;
    Table table_;
    Rect banner_button_, refresh_button_, settings_button_;
    bool loaded_ = false;
    std::vector<pc_startup_item> items_;
    std::vector<size_t> filtered_;
};

// ---------------------------------------------------------------------------------------------
// Services
// ---------------------------------------------------------------------------------------------

class ServicesPage : public Page {
public:
    ServicesPage() {
        table_.columns = {{ColName, L"Name", 320, 1, HAlign::Left, true, false},
                          {ColDomain, L"Domain", 90, 0, HAlign::Left, true, false},
                          {ColStatus, L"Status", 170, 0, HAlign::Left, true, false},
                          {ColProgram, L"Program", 240, 1, HAlign::Left, true, false}};
        table_.sort_column = ColName;
        table_.sort_descending = false;
        table_.row_height = 48;
        table_.row_count = [this] { return static_cast<int>(filtered_.size()); };
        table_.cell = [this](int row, int column) { return cell(row, column); };
        table_.on_sort = [this](int) { sort(); };
        table_.on_context = [this](int row, float x, float y) { context(row, x, y); };
        table_.on_activate = [this](int row) {
            if (host_ && service(row).pid > 0) host_->show_info(service(row).pid);
        };
        search_.placeholder = L"Search by name, service or PID";
        search_.keycap = L"";
    }
    PageId id() const override { return PageId::Services; }
    std::wstring title() const override { return L"Services"; }

    void activate(Host &host) override {
        host_ = &host;
        services_ = host.store().services();
        loaded_ = true;
        sort();
    }
    void tick(Host &host) override {
        if (host.store().ticks() % 5 == 0) activate(host);
    }

    void paint(Host &host, const Rect &bounds) override {
        host_ = &host;
        Renderer &r = host.renderer();
        const float mx = host.mouse_x(), my = host.mouse_y();
        const int running = static_cast<int>(
            std::count_if(services_.begin(), services_.end(), [](const pc_service &s) { return s.pid > 0; }));
        Rect area = screen_area(bounds);
        Rect trailing;
        area = page_header(r, area, L"Services",
                           loaded_ ? std::to_wstring(services_.size()) + L" services · " + std::to_wstring(running) +
                                         L" running · " + std::to_wstring(filtered_.size()) + L" shown"
                                   : L"Loading…",
                           std::nullopt, &trailing);
        area.y -= kSectionGap - tokens::space::lg;
        area.h += kSectionGap - tokens::space::lg;
        const std::vector<std::wstring> filters = {L"All", L"Running", L"Third-Party", L"Disabled"};
        const float sw = r.segmented_width(filters);
        filter_rects_ = r.segmented(Rect{trailing.right() - sw, trailing.y + 2, sw, kControlHeight}, filters, filter_);

        Rect toolbar = area.take_top(kControlHeight);
        search_.bounds = toolbar.take_left(std::min(340.0f, toolbar.w * 0.5f));
        search_.paint(r);
        const std::vector<std::wstring> domains = {L"All Domains", L"System", L"User"};
        const float dw = r.segmented_width(domains);
        toolbar.take_left(tokens::space::md);
        domain_rects_ = r.segmented(Rect{toolbar.x, toolbar.y, dw, kControlHeight}, domains, domain_);
        refresh_button_ = Rect{toolbar.right() - 28, toolbar.y, 28, 28};
        r.icon_button(refresh_button_, Renderer::Symbol::Refresh, r.theme().text_secondary(),
                      refresh_button_.contains(mx, my));
        area.take_top(tokens::space::lg);

        banner_button_ = {};
        if (!host.elevated()) {
            const Banner b = action_banner(
                r, area.take_top(kActionBannerHeight), L"Services are read-only without full access",
                L"Starting, stopping, restarting, enabling and disabling services need administrator access.",
                L"Unlock Full Access", Renderer::Tone::Accent, banner_button_.contains(mx, my));
            banner_button_ = b.button;
            area.take_top(tokens::space::lg);
        }
        area = table_area(bounds, area, 0);
        r.card(area);
        const Rect inner = area.inset(1, 1);
        table_.layout(inner);
        r.push_clip(inner);
        table_.paint(r);
        r.pop_clip();
        if (filtered_.empty())
            empty_state(r, inner, loaded_ ? L"No matching services" : L"Loading services…",
                        loaded_ ? L"Try another name or filter." : L"");
    }

    void mouse_move(Host &, const MouseEvent &e) override { table_.mouse_move(e); }
    void mouse_leave(Host &) override { table_.mouse_leave(); }
    void mouse_down(Host &host, const MouseEvent &e, bool right) override {
        bool cleared = false;
        if (search_.mouse_down(e.x, e.y, cleared)) {
            table_.focused = false;
            if (cleared) sort();
            return;
        }
        for (size_t i = 0; i < filter_rects_.size(); ++i) {
            if (filter_rects_[i].contains(e.x, e.y)) {
                filter_ = static_cast<int>(i);
                sort();
                return;
            }
        }
        for (size_t i = 0; i < domain_rects_.size(); ++i) {
            if (domain_rects_[i].contains(e.x, e.y)) {
                domain_ = static_cast<int>(i);
                sort();
                return;
            }
        }
        if (!right && refresh_button_.contains(e.x, e.y)) {
            activate(host);
            return;
        }
        if (!banner_button_.empty() && banner_button_.contains(e.x, e.y)) {
            host.relaunch_elevated();
            return;
        }
        table_.mouse_down(e, right);
    }
    void double_click(Host &, const MouseEvent &e) override { table_.double_click(e); }
    void wheel(Host &, const MouseEvent &e) override { table_.wheel(e); }
    bool key(Host &, const KeyEvent &e) override {
        bool consumed = false;
        if (search_.key(e, consumed)) sort();
        if (consumed) return true;
        return table_.key(e);
    }
    bool character(Host &, wchar_t c) override {
        if (search_.character(c)) {
            sort();
            return true;
        }
        return false;
    }
    void focus_search() override {
        search_.focused = true;
        table_.focused = false;
    }
    bool search_focused() const override { return search_.focused; }
    int32_t selected_pid() const override {
        return table_.selected >= 0 && table_.selected < static_cast<int>(filtered_.size())
                   ? service(table_.selected).pid
                   : 0;
    }

private:
    const pc_service &service(int row) const { return services_[filtered_[static_cast<size_t>(row)]]; }

    Cell cell(int row, int column) {
        const pc_service &s = service(row);
        const Theme &theme = host_->renderer().theme();
        Cell c;
        switch (column) {
            case ColName:
                c.text = fmt::from_utf8(s.name);
                c.bold = true;
                c.subtitle = fmt::from_utf8(s.label);
                break;
            case ColDomain:
                c.text = s.domain == PC_DOMAIN_USER ? L"User" : L"System";
                c.color = theme.text_secondary();
                break;
            case ColStatus:
                if (s.pid > 0) {
                    c.text = L"Running · PID " + std::to_wstring(s.pid);
                    c.color = theme.text_secondary();
                } else if (!s.enabled) {
                    c.text = L"Disabled";
                    c.dim = true;
                } else if (s.last_exit != 0) {
                    c.text = L"Failed · exit " + std::to_wstring(s.last_exit);
                    c.color = theme.danger();
                } else {
                    c.text = L"Stopped";
                    c.dim = true;
                }
                break;
            case ColProgram:
                c.text = fmt::from_utf8(s.program);
                c.mono = true;
                c.color = theme.text_secondary();
                break;
            default: break;
        }
        return c;
    }

    void sort() {
        filtered_.clear();
        for (size_t i = 0; i < services_.size(); ++i) {
            const pc_service &s = services_[i];
            if (filter_ == 1 && s.pid <= 0) continue;
            if (filter_ == 2 && s.apple) continue;
            if (filter_ == 3 && s.enabled) continue;
            if (domain_ == 1 && s.domain != PC_DOMAIN_SYSTEM) continue;
            if (domain_ == 2 && s.domain != PC_DOMAIN_USER) continue;
            if (!contains_ci(fmt::from_utf8(s.name), search_.text) &&
                !contains_ci(fmt::from_utf8(s.label), search_.text) &&
                !(s.pid > 0 && std::to_wstring(s.pid) == search_.text))
                continue;
            filtered_.push_back(i);
        }
        const int column = table_.sort_column;
        const bool desc = table_.sort_descending;
        std::stable_sort(filtered_.begin(), filtered_.end(), [&](size_t a, size_t b) {
            const pc_service &x = services_[a], &y = services_[b];
            int order = 0;
            switch (column) {
                case ColDomain: order = x.domain - y.domain; break;
                case ColStatus:
                    order = (x.pid > 0 ? 0 : x.enabled ? 1 : 2) - (y.pid > 0 ? 0 : y.enabled ? 1 : 2);
                    break;
                case ColProgram: order = _stricmp(x.program, y.program); break;
                default: order = _stricmp(x.name, y.name); break;
            }
            if (order == 0) order = _stricmp(x.name, y.name);
            return desc ? order > 0 : order < 0;
        });
        table_.select(std::min(table_.selected, static_cast<int>(filtered_.size()) - 1));
    }

    void act(const pc_service &s, int32_t action, const wchar_t *verb) {
        if (!host_) return;
        if ((action == PC_SERVICE_STOP || action == PC_SERVICE_DISABLE) && s.apple &&
            !host_->confirm(std::wstring(verb) + L" " + fmt::from_utf8(s.name) + L"?",
                            L"This service is part of Windows. Stopping it may make features unavailable until it is "
                            L"started again.",
                            verb, true))
            return;
        const pc_result result = host_->store().service_control(s.domain, s.label, action);
        if (result != PC_OK) host_->report(result, std::wstring(verb) + L" the service");
        activate(*host_);
    }

    void context(int row, float x, float y) {
        if (!host_) return;
        const pc_service s = service(row);
        std::vector<MenuItem> items = {
            {MenuStart, L"Start", s.pid <= 0 && s.enabled},
            {MenuStop, L"Stop", s.pid > 0},
            {MenuRestart, L"Restart", s.pid > 0},
            {0, L"", true, false, true},
            {s.enabled ? MenuDisable : MenuEnable, s.enabled ? L"Disable" : L"Enable"},
            {0, L"", true, false, true},
            {MenuShowProcess, L"Show Process", s.pid > 0},
            {MenuOpenLocation, L"Show in Explorer", s.program[0] != 0},
            {MenuCopy, L"Copy Service Name"},
        };
        switch (host_->popup_menu(items, x, y)) {
            case MenuStart: act(s, PC_SERVICE_START, L"Start"); break;
            case MenuStop: act(s, PC_SERVICE_STOP, L"Stop"); break;
            case MenuRestart: act(s, PC_SERVICE_RESTART, L"Restart"); break;
            case MenuEnable: act(s, PC_SERVICE_ENABLE, L"Enable"); break;
            case MenuDisable: act(s, PC_SERVICE_DISABLE, L"Disable"); break;
            case MenuShowProcess: host_->show_info(s.pid); break;
            case MenuOpenLocation: {
                std::wstring program = fmt::from_utf8(s.program);
                if (!program.empty() && program[0] == L'"') {
                    const size_t end = program.find(L'"', 1);
                    program = program.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
                } else if (const size_t exe = program.find(L".exe"); exe != std::wstring::npos) {
                    program = program.substr(0, exe + 4);
                }
                host_->open_in_explorer(program);
                break;
            }
            case MenuCopy: host_->copy_to_clipboard(fmt::from_utf8(s.label)); break;
            default: break;
        }
    }

    Host *host_ = nullptr;
    Table table_;
    TextField search_;
    Rect banner_button_, refresh_button_;
    std::vector<Rect> filter_rects_, domain_rects_;
    int filter_ = 1;  // Running, like the macOS default
    int domain_ = 0;
    bool loaded_ = false;
    std::vector<pc_service> services_;
    std::vector<size_t> filtered_;
};

}  // namespace

std::unique_ptr<Page> make_startup_page() { return std::make_unique<StartupPage>(); }
std::unique_ptr<Page> make_services_page() { return std::make_unique<ServicesPage>(); }

}  // namespace procyon::ui
