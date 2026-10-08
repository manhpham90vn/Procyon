#include "window.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

#include "canvas_d2d.hpp"
#include "overlays.hpp"
#include "pages.hpp"
#include "platform.hpp"
#include "resource.h"
#include "version.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace procyon::ui {
namespace {

constexpr UINT WM_APP_SNAPSHOT = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT_PTR kTrayId = 1;
constexpr UINT_PTR kFrameTimer = 2;  // one more frame for a running animation
// The window draws its own caption, as the macOS app hides its title bar: a 32-DIP strip that
// drags the window, with the three Windows 11 caption buttons (46x32) at its right end.
constexpr float kCaptionHeight = 32;
constexpr float kCaptionButtonWidth = 46;
constexpr float kSidebarWidth = 240;
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmSystemBackdropType = 38;       // DWMWA_SYSTEMBACKDROP_TYPE (Windows 11 22H2)
constexpr DWORD kDwmSystemBackdropMainWindow = 2;  // DWMSBT_MAINWINDOW: Mica
// How much of the sidebar surface sits over the Mica backdrop (the rest shows the desktop tint).
constexpr float kSidebarOpacityDark = 0.62f;
constexpr float kSidebarOpacityLight = 0.55f;
const wchar_t *const kClassName = L"ProcyonMainWindow";
const wchar_t *const kSettingsKey = L"Software\\Procyon";

// A sidebar entry: a page row (symbol, title, detail), a metric row (icon, title, value,
// sparkline) or a section label.
struct NavItem {
    enum class Kind { Page, Metric, Section };
    Kind kind = Kind::Page;
    PageId page = PageId::Overview;
    std::wstring label;
    Renderer::Symbol symbol = Renderer::Symbol::Grid;
    MetricKind metric = MetricKind::Cpu;
    Rect rect;
};

const wchar_t *page_name(PageId id) {
    switch (id) {
        case PageId::Overview: return L"overview";
        case PageId::Processes: return L"processes";
        case PageId::Cpu: return L"cpu";
        case PageId::Memory: return L"memory";
        case PageId::Disk: return L"disk";
        case PageId::Network: return L"network";
        case PageId::Gpu: return L"gpu";
        case PageId::Battery: return L"battery";
        case PageId::Startup: return L"startup";
        case PageId::Services: return L"services";
        case PageId::History: return L"history";
        case PageId::Inspect: return L"inspect";
        case PageId::System: return L"system";
        case PageId::Settings: return L"settings";
    }
    return L"overview";
}

std::optional<PageId> page_from_name(const std::wstring &name) {
    for (PageId id : {PageId::Overview, PageId::Processes, PageId::Cpu, PageId::Memory, PageId::Disk, PageId::Network,
                      PageId::Gpu, PageId::Battery, PageId::Startup, PageId::Services, PageId::History, PageId::Inspect,
                      PageId::System, PageId::Settings})
        if (name == page_name(id)) return id;
    return std::nullopt;
}

class MainWindow : public Host {
public:
    MainWindow(HINSTANCE instance, std::optional<PageId> initial, bool persist)
        : instance_(instance), initial_page_(initial), persist_(persist) {}

    bool create(int show_command) {
        load_settings();
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &MainWindow::static_proc;
        wc.hInstance = instance_;
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_PROCYON));
        wc.hIconSm = wc.hIcon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;
        // No redirection bitmap: the DirectComposition swap chain is all the window shows, and
        // its transparent pixels reveal the Mica backdrop.
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClassName, L"Procyon", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance_, this);
        if (!hwnd_) return false;
        restore_placement(show_command);
        return true;
    }

    HWND hwnd() const { return hwnd_; }
    bool text_input_active() const { return !overlays_.empty() || (current_ && current_->search_focused()); }

    // ---- Host ----
    Store &store() override { return store_; }
    Renderer &renderer() override { return renderer_; }
    Settings &settings() override { return settings_; }
    void repaint() override { InvalidateRect(hwnd_, nullptr, FALSE); }
    void request_frame() override { SetTimer(hwnd_, kFrameTimer, 16, nullptr); }
    double now() override {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
    float mouse_x() override { return overlays_.empty() ? mouse_x_ : -1; }
    float mouse_y() override { return overlays_.empty() ? mouse_y_ : -1; }

    void navigate(PageId id) override {
        for (auto &page : pages_) {
            if (page->id() != id) continue;
            current_ = page.get();
            current_->activate(*this);
            repaint();
            return;
        }
    }

    bool confirm(const std::wstring &title, const std::wstring &message, const std::wstring &action,
                 bool destructive) override {
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hwndParent = hwnd_;
        config.hInstance = instance_;
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
        config.pszWindowTitle = L"Procyon";
        config.pszMainInstruction = title.c_str();
        config.pszContent = message.empty() ? nullptr : message.c_str();
        config.pszMainIcon = destructive ? TD_WARNING_ICON : TD_INFORMATION_ICON;
        TASKDIALOG_BUTTON buttons[] = {{IDOK, action.c_str()}, {IDCANCEL, L"Cancel"}};
        config.pButtons = buttons;
        config.cButtons = 2;
        config.nDefaultButton = destructive ? IDCANCEL : IDOK;
        int pressed = 0;
        if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) return false;
        return pressed == IDOK;
    }

    void alert(const std::wstring &title, const std::wstring &message) override {
        TaskDialog(hwnd_, instance_, L"Procyon", title.c_str(), message.empty() ? nullptr : message.c_str(),
                   TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
    }

    void report(pc_result result, const std::wstring &action) override {
        if (result == PC_OK) return;
        const std::wstring title = L"Couldn't " + action;
        const std::wstring message = fmt::from_utf8(pc_result_message(result));
        if (result == PC_ERR_PERMISSION && !elevated()) {
            if (confirm(title, message + L"\n\nRun Procyon as administrator to act on every process and service.",
                        L"Run as administrator", false))
                relaunch_elevated();
            return;
        }
        TaskDialog(hwnd_, instance_, L"Procyon", title.c_str(), message.c_str(), TDCBF_OK_BUTTON, TD_ERROR_ICON,
                   nullptr);
    }

    int popup_menu(const std::vector<MenuItem> &items, float x, float y) override {
        HMENU menu = build_menu(items);
        POINT pt{to_px(x), to_px(y)};
        ClientToScreen(hwnd_, &pt);
        const int chosen = static_cast<int>(TrackPopupMenuEx(
            menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd_, nullptr));
        DestroyMenu(menu);
        return chosen;
    }

    void show_info(int32_t pid) override {
        if (pid <= 0) return;
        overlays_.push_back(make_info_sheet(pid));
        repaint();
    }

    void show_in_processes(const std::string &app_id, int32_t pid) override {
        navigate(PageId::Processes);
        if (current_) current_->reveal(app_id, pid);
        repaint();
    }

    bool elevated() override {
        static const bool value = [] {
            HANDLE raw = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
            TOKEN_ELEVATION elevation{};
            DWORD size = 0;
            const bool ok = GetTokenInformation(raw, TokenElevation, &elevation, sizeof(elevation), &size) &&
                            elevation.TokenIsElevated;
            CloseHandle(raw);
            return ok;
        }();
        return value;
    }

    void relaunch_elevated() override {
        if (elevated()) return;
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const std::wstring params = std::wstring(L"--page ") + (current_ ? page_name(current_->id()) : L"overview");
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.hwnd = hwnd_;
        info.lpVerb = L"runas";
        info.lpFile = path;
        info.lpParameters = params.c_str();
        info.nShow = SW_SHOWNORMAL;
        save_settings();
        if (ShellExecuteExW(&info) && info.hProcess) {
            CloseHandle(info.hProcess);
            quit_ = true;
            DestroyWindow(hwnd_);
        }
    }

    void copy_to_clipboard(const std::wstring &text) override {
        if (!OpenClipboard(hwnd_)) return;
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void *p = GlobalLock(memory)) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(memory);
                SetClipboardData(CF_UNICODETEXT, memory);
            }
        }
        CloseClipboard();
    }

    void open_in_explorer(const std::wstring &path) override {
        if (path.empty()) return;
        const std::wstring params = L"/select,\"" + path + L"\"";
        ShellExecuteW(hwnd_, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    }

    void open_url(const std::wstring &url) override {
        if (url.empty()) return;
        ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    std::wstring pick_folder(const std::wstring &title) override {
        std::wstring result;
        IFileOpenDialog *dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return result;
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(title.c_str());
        if (SUCCEEDED(dialog->Show(hwnd_))) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
        return result;
    }

    void set_paused(bool paused) override {
        store_.set_paused(paused);
        update_tray();
        repaint();
    }
    bool paused() override { return store_.paused(); }

private:
    // ---- lifecycle ----

    static LRESULT CALLBACK static_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        MainWindow *self = nullptr;
        if (message == WM_NCCREATE) {
            self = static_cast<MainWindow *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        } else {
            self = reinterpret_cast<MainWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        return self->proc(message, wparam, lparam);
    }

    void on_create() {
        dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
        canvas_.init(hwnd_);
        canvas_.set_dpi(dpi_);
        // Mica behind the sidebar, the material the macOS sidebar shows through to the desktop.
        // Windows 11 22H2 and later answer S_OK; older systems get an opaque sidebar.
        const MARGINS whole{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd_, &whole);
        const DWORD backdrop = kDwmSystemBackdropMainWindow;
        mica_ = SUCCEEDED(DwmSetWindowAttribute(hwnd_, kDwmSystemBackdropType, &backdrop, sizeof(backdrop)));
        apply_theme();
        pages_.push_back(make_overview_page());
        pages_.push_back(make_processes_page());
        for (PageId id : {PageId::Cpu, PageId::Memory, PageId::Disk, PageId::Network, PageId::Gpu, PageId::Battery})
            pages_.push_back(make_performance_page(id));
        pages_.push_back(make_startup_page());
        pages_.push_back(make_services_page());
        pages_.push_back(make_history_page());
        pages_.push_back(make_inspect_page());
        pages_.push_back(make_system_page());
        pages_.push_back(make_settings_page());
        build_nav();
        store_.set_interval(settings_.interval);
        store_.set_records_history(settings_.records_history);
        store_.set_alert_handler([this](const AlertEvent &event) { notify(event); });
        // The sampler thread hands each snapshot over through the message queue.
        store_.start([this](Snapshot *snapshot) {
            return PostMessageW(hwnd_, WM_APP_SNAPSHOT, 0, reinterpret_cast<LPARAM>(snapshot)) != FALSE;
        });
        navigate(initial_page_.value_or(PageId::Overview));
        add_tray();
    }

    void build_nav() {
        nav_.clear();
        auto page = [&](PageId id, const wchar_t *label, Renderer::Symbol symbol) {
            NavItem item;
            item.kind = NavItem::Kind::Page;
            item.page = id;
            item.label = label;
            item.symbol = symbol;
            nav_.push_back(item);
        };
        auto metric = [&](PageId id, const wchar_t *label, MetricKind kind) {
            NavItem item;
            item.kind = NavItem::Kind::Metric;
            item.page = id;
            item.label = label;
            item.metric = kind;
            nav_.push_back(item);
        };
        auto section = [&](const wchar_t *label) {
            NavItem item;
            item.kind = NavItem::Kind::Section;
            item.label = label;
            nav_.push_back(item);
        };
        page(PageId::Overview, L"Overview", Renderer::Symbol::Grid);
        page(PageId::Processes, L"Processes", Renderer::Symbol::List);
        section(L"Performance");
        metric(PageId::Cpu, L"CPU", MetricKind::Cpu);
        metric(PageId::Memory, L"Memory", MetricKind::Memory);
        if (store_.has(PC_CAP_GPU)) metric(PageId::Gpu, L"GPU", MetricKind::Gpu);
        metric(PageId::Disk, L"Disk", MetricKind::Disk);
        metric(PageId::Network, L"Network", MetricKind::Network);
        if (store_.has(PC_CAP_STARTUP) || store_.has(PC_CAP_SERVICES)) {
            section(L"Manage");
            if (store_.has(PC_CAP_STARTUP)) page(PageId::Startup, L"Startup", Renderer::Symbol::Power);
            if (store_.has(PC_CAP_SERVICES)) page(PageId::Services, L"Services", Renderer::Symbol::Gears);
        }
        section(L"Analyze");
        page(PageId::History, L"History", Renderer::Symbol::Clock);
        if (store_.has(PC_CAP_CONNECTIONS)) page(PageId::Inspect, L"Files & Ports", Renderer::Symbol::Ports);
        section(L"Machine");
        if (store_.has(PC_CAP_BATTERY)) page(PageId::Battery, L"Battery", Renderer::Symbol::Gauge);
        page(PageId::System, L"System", Renderer::Symbol::Info);  // info.circle.fill on macOS
    }

    // ---- theme, DPI ----

    void apply_theme() {
        theme_.dark = settings_.theme == 2 || (settings_.theme == 0 && platform::system_prefers_dark());
        const BOOL dark = theme_.dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
        repaint();
    }

    float to_dip(int px) const { return px * 96.0f / dpi_; }
    int to_px(float dip) const { return static_cast<int>(dip * dpi_ / 96.0f + 0.5f); }

    MouseEvent mouse_event(LPARAM lparam, WPARAM wparam, int wheel = 0) const {
        MouseEvent e;
        e.x = to_dip(GET_X_LPARAM(lparam));
        e.y = to_dip(GET_Y_LPARAM(lparam));
        e.ctrl = (wparam & MK_CONTROL) != 0;
        e.shift = (wparam & MK_SHIFT) != 0;
        e.wheel = wheel;
        return e;
    }

    static Key translate_key(WPARAM vk) {
        switch (vk) {
            case VK_ESCAPE: return Key::Escape;
            case VK_BACK: return Key::Back;
            case VK_DELETE: return Key::Delete;
            case VK_RETURN: return Key::Return;
            case VK_SPACE: return Key::Space;
            case VK_TAB: return Key::Tab;
            case VK_LEFT: return Key::Left;
            case VK_RIGHT: return Key::Right;
            case VK_UP: return Key::Up;
            case VK_DOWN: return Key::Down;
            case VK_HOME: return Key::Home;
            case VK_END: return Key::End;
            case VK_PRIOR: return Key::PageUp;
            case VK_NEXT: return Key::PageDown;
            case VK_APPS: return Key::Menu;
            default: return Key::None;
        }
    }

    KeyEvent key_event(WPARAM vk) const {
        KeyEvent e;
        e.key = translate_key(vk);
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) e.ch = static_cast<wchar_t>(vk);
        e.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        e.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        e.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        return e;
    }

    // ---- painting ----

    Rect client_rect() const {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        return Rect{0, 0, to_dip(rc.right), to_dip(rc.bottom)};
    }

    void paint() {
        // The theme follows Settings, which may have changed since the last frame.
        const bool dark = settings_.theme == 2 || (settings_.theme == 0 && platform::system_prefers_dark());
        if (dark != theme_.dark) apply_theme();
        renderer_.set_theme(theme_);
        if (!canvas_.begin(mica_ ? colors::transparent : theme_.background())) return;
        const Rect window = client_rect();
        sidebar_ = Rect{0, 0, kSidebarWidth, window.h};
        content_ = Rect{kSidebarWidth, kCaptionHeight, window.w - kSidebarWidth, window.h - kCaptionHeight};
        // The page and its caption strip are opaque; only the sidebar shows the backdrop.
        renderer_.fill(Rect{kSidebarWidth, 0, window.w - kSidebarWidth, window.h}, theme_.background());
        paint_sidebar();
        if (current_) {
            renderer_.push_clip(content_);
            current_->paint(*this, content_);
            renderer_.pop_clip();
        }
        paint_caption(window);
        for (auto &overlay : overlays_) overlay->paint(*this, window);
        if (!canvas_.end()) repaint();
    }

    // The caption buttons, Windows 11 style: a flat strip the page shows through, each button
    // tinted on hover (the close button red), with Lucide glyphs.
    void paint_caption(const Rect &window) {
        Renderer &r = renderer_;
        const Theme &theme = theme_;
        const bool zoomed = IsZoomed(hwnd_) != FALSE;
        for (int i = 0; i < 3; ++i) {
            caption_buttons_[i] =
                Rect{window.right() - kCaptionButtonWidth * (3 - i), 0, kCaptionButtonWidth, kCaptionHeight};
        }
        const Renderer::Symbol glyphs[3] = {Renderer::Symbol::WindowMinimize,
                                            zoomed ? Renderer::Symbol::WindowRestore : Renderer::Symbol::WindowMaximize,
                                            Renderer::Symbol::Close};
        for (int i = 0; i < 3; ++i) {
            const Rect &b = caption_buttons_[i];
            const bool hovered = caption_hover_ == i;
            const bool pressed = hovered && caption_pressed_ == i;
            Color glyph = theme.text_secondary();
            if (hovered && i == 2) {
                r.fill(b, with_alpha(rgba(0xC42B1CFF), pressed ? 0.85f : 1.0f));
                glyph = colors::white;
            } else if (hovered) {
                r.fill(b, with_alpha(theme.text(), pressed ? 0.04f : 0.07f));
                glyph = theme.text();
            }
            r.symbol(glyphs[i], Rect{b.cx() - 5, b.cy() - 5, 10, 10}, glyph, 1.0f);
        }
    }

    // Which caption button (0 minimize, 1 maximize, 2 close) is at a client-area point, -1 for none.
    int caption_button_at(float x, float y) const {
        for (int i = 0; i < 3; ++i)
            if (caption_buttons_[i].contains(x, y)) return i;
        return -1;
    }

    // The window's own hit test for the strip that replaces the caption: resize edges first, then
    // the buttons, then anything draggable above the first sidebar row or the page.
    LRESULT caption_hit_test(LPARAM lparam) {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd_, &pt);
        const float x = to_dip(pt.x), y = to_dip(pt.y);
        const bool zoomed = IsZoomed(hwnd_) != FALSE;
        if (!zoomed) {
            const UINT dpi = static_cast<UINT>(dpi_);
            const int frame =
                GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            if (pt.y < frame) {
                if (pt.x < frame) return HTTOPLEFT;
                if (to_dip(pt.x) > client_rect().w - to_dip(frame)) return HTTOPRIGHT;
                return HTTOP;
            }
        }
        switch (caption_button_at(x, y)) {
            case 0: return HTMINBUTTON;
            case 1: return HTMAXBUTTON;
            case 2: return HTCLOSE;
            default: break;
        }
        if (!overlays_.empty()) return HTCLIENT;
        if (sidebar_.contains(x, y)) {
            // Above the first row: the brand, a drag handle like the macOS sidebar's top.
            float first_row = sidebar_.h;
            for (const NavItem &item : nav_)
                if (item.kind != NavItem::Kind::Section && !item.rect.empty()) {
                    first_row = item.rect.y;
                    break;
                }
            return y < first_row - tokens::space::sm ? HTCAPTION : HTCLIENT;
        }
        return y < kCaptionHeight ? HTCAPTION : HTCLIENT;
    }

    void set_caption_hover(int button) {
        if (caption_hover_ == button) return;
        caption_hover_ = button;
        repaint();
    }

    void caption_button_action(int button) {
        switch (button) {
            case 0: ShowWindow(hwnd_, SW_MINIMIZE); break;
            case 1: ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); break;
            case 2: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
            default: break;
        }
    }

    // The detail text of a page row, as the macOS sidebar shows it.
    std::wstring page_detail(PageId id) {
        const Snapshot &s = store_.snapshot();
        const pc_system_info &info = store_.system_info();
        switch (id) {
            case PageId::Overview: return display_model(info);
            case PageId::Processes: return store_.has_snapshot() ? fmt::count(s.process_count) + L" running" : L" ";
            case PageId::Startup: return L"Apps that start with Windows";
            case PageId::Services: return L"Background services";
            case PageId::History: return store_.records_history() ? L"The last 24 hours" : L"Off";
            case PageId::Inspect: return L"Who uses a port or a file";
            case PageId::Battery: {
                if (auto b = store_.battery())
                    return fmt::percent(b->level) + L" · " +
                           (b->charging      ? L"Charging"
                            : b->on_ac_power ? L"On AC power"
                                             : L"On battery");
                return L"";
            }
            case PageId::System: return fmt::from_utf8(info.os_name) + L" " + fmt::from_utf8(info.os_version);
            case PageId::Settings: return L"Updates, tray, alerts, appearance";
            default: return L"";
        }
    }

    void paint_sidebar() {
        Renderer &r = renderer_;
        const Theme &theme = theme_;
        const Snapshot &s = store_.snapshot();
        const History &h = store_.history();
        r.fill(sidebar_,
               mica_ ? with_alpha(theme.surface_raised(), theme.dark ? kSidebarOpacityDark : kSidebarOpacityLight)
                     : theme.surface_raised());
        r.line(sidebar_.right() - 0.5f, 0, sidebar_.right() - 0.5f, sidebar_.h, theme.border());
        const float pad = tokens::space::sm + 2;
        Rect area = sidebar_.inset(pad, 0);
        area.take_top(tokens::space::lg);

        // Brand: gradient circle with a sparkle, the wordmark.
        Rect brand = area.take_top(26);
        {
            const Rect b = brand.inset(tokens::space::sm, 0);
            const Rect disc{b.x, b.cy() - 11, 22, 22};
            r.canvas().shadow(disc, 11, Shadow{with_alpha(rgba(tokens::metric::cpu.start), 0.5f), 6, 0});
            r.fill_gradient_round(disc, 11, rgba(tokens::metric::cpu.start), rgba(tokens::metric::memory.start));
            r.symbol(Renderer::Symbol::Sparkle, Rect{b.x + 5.5f, b.cy() - 5.5f, 11, 11}, colors::white);
            TextStyle title;
            title.font = Font::Brand;
            r.text(L"Procyon", Rect{b.x + 22 + tokens::space::sm, b.y, b.w - 30, b.h}, title, theme.text());
        }
        area.take_top(tokens::space::sm + tokens::space::xs);

        // Bottom block first, so the list knows where to stop: divider, live pill, settings row.
        Rect bottom = area.take_bottom(tokens::space::md + 40 + tokens::space::sm + 32 + tokens::space::sm);
        area.take_bottom(tokens::space::md);
        // The list scrolls when the window is short (the bottom controls stay put).
        const Rect list = area;
        sidebar_scroll_.viewport = list.h;
        sidebar_scroll_.clamp();
        area.y -= sidebar_scroll_.offset;
        r.push_clip(Rect{sidebar_.x, list.y, sidebar_.w, list.h});
        for (NavItem &item : nav_) {
            if (item.kind == NavItem::Kind::Section) {
                Rect caption = area.take_top(tokens::space::md + 14 + tokens::space::xs);
                TextStyle c;
                c.font = Font::Caption;
                c.valign = VAlign::Bottom;
                r.text(item.label,
                       Rect{caption.x + tokens::space::sm, caption.y, caption.w, caption.h - tokens::space::xs}, c,
                       theme.text_tertiary());
                item.rect = {};
                continue;
            }
            Rect row = area.take_top(40);
            area.take_top(2);
            item.rect = row;
            paint_nav_row(item, row, s, h);
        }
        sidebar_scroll_.content = area.y + sidebar_scroll_.offset - list.y + tokens::space::md;
        r.pop_clip();

        // Divider.
        r.line(bottom.x + tokens::space::xs, bottom.y + 0.5f, bottom.right() - tokens::space::xs, bottom.y + 0.5f,
               theme.border());
        bottom.take_top(tokens::space::md);
        // Live / paused chrome.
        const bool paused = store_.paused();
        pause_button_ = bottom.take_top(32);
        bottom.take_top(tokens::space::sm);
        r.fill_round(pause_button_, tokens::radius::lg, theme.dark ? with_alpha(theme.text(), 0.06f) : theme.surface());
        r.stroke_round(pause_button_, tokens::radius::lg, theme.border_strong());
        Rect pill = pause_button_.inset(tokens::space::md, 0);
        r.live_indicator(pill.x + 7, pill.cy(), !paused);
        TextStyle pl;
        pl.font = Font::Label;
        r.text(paused ? L"Paused" : L"Live", Rect{pill.x + 14 + tokens::space::sm, pill.y, pill.w - 40, pill.h}, pl,
               theme.text());
        r.symbol(paused ? Renderer::Symbol::Play : Renderer::Symbol::Pause,
                 Rect{pill.right() - 14, pill.cy() - 7, 14, 14}, theme.text_secondary(), 1.5f);
        // Settings row.
        settings_button_ = bottom.take_top(40);
        NavItem settings;
        settings.kind = NavItem::Kind::Page;
        settings.page = PageId::Settings;
        settings.label = L"Settings";
        settings.symbol = Renderer::Symbol::Gear;
        paint_nav_row(settings, settings_button_, s, h);
    }

    void paint_nav_row(const NavItem &item, const Rect &row, const Snapshot &s, const History &h) {
        Renderer &r = renderer_;
        const Theme &theme = theme_;
        const bool active = current_ && current_->id() == item.page;
        const bool hover = overlays_.empty() && row.contains(mouse_x_, mouse_y_);
        if (active) {
            r.fill_round(row, tokens::radius::md, with_alpha(theme.accent(), 0.16f));
            r.stroke_round(row, tokens::radius::md, with_alpha(theme.accent(), 0.28f));
        } else if (hover) {
            r.fill_round(row, tokens::radius::md, theme.track());
        }
        Rect inner = row.inset(tokens::space::sm, tokens::space::xs);
        const Rect icon = inner.take_left(22 + tokens::space::sm + 2);
        std::wstring detail;
        const Series *series = nullptr;
        const Series *secondary = nullptr;
        float max = 1;
        if (item.kind == NavItem::Kind::Metric) {
            r.metric_icon(Rect{icon.x, icon.cy() - 11, 22, 22}, item.metric);
            switch (item.metric) {
                case MetricKind::Cpu:
                    series = &h.cpu;
                    detail = fmt::percent(s.cpu_usage);
                    break;
                case MetricKind::Memory:
                    series = &h.memory_fraction;
                    detail = fmt::bytes(static_cast<int64_t>(s.memory_used)) + L" · " +
                             fmt::percent(s.memory_total ? static_cast<double>(s.memory_used) / s.memory_total : 0);
                    break;
                case MetricKind::Gpu:
                    series = h.gpu.empty() ? nullptr : &h.gpu[0];
                    detail = s.gpus.empty() ? std::wstring(fmt::unavailable) : fmt::percent(s.gpus[0].utilization);
                    break;
                case MetricKind::Disk:
                    series = &h.disk_read;
                    secondary = &h.disk_write;
                    max = nice_max(std::max(h.disk_read.max_recent(), h.disk_write.max_recent()));
                    detail = fmt::rate(s.disk_read_bps + s.disk_write_bps);
                    break;
                case MetricKind::Network:
                    series = &h.net_rx;
                    secondary = &h.net_tx;
                    max = nice_max(std::max(h.net_rx.max_recent(), h.net_tx.max_recent()));
                    detail = L"↓ " + fmt::rate(s.net_rx_bps) + L" ↑ " + fmt::rate(s.net_tx_bps);
                    break;
                default: break;
            }
        } else if (item.page == PageId::Battery) {
            r.glyph(MetricKind::Battery, Rect{icon.x + 4, icon.cy() - 7, 14, 14}, theme.accent());
            detail = page_detail(item.page);
        } else {
            r.symbol(item.symbol, Rect{icon.x + 4, icon.cy() - 7, 14, 14}, theme.accent(), 1.6f);
            detail = page_detail(item.page);
        }
        Rect text = inner;
        if (series) {
            const Rect spark = text.take_right(46);
            text.take_right(tokens::space::xs);
            if (series->size() > 1) {
                Renderer::SparklineOptions options;
                options.end_dot = false;
                options.halo = false;
                options.line_width = 1.25f;
                r.sparkline(Rect{spark.x, spark.cy() - 11, spark.w, 22}, series->data(), series->size(), kHistoryWindow,
                            max, item.metric, options);
                if (secondary && secondary->size() > 1) {
                    options.area = false;
                    Color end = rgba(metric_style(item.metric).end);
                    options.color_override = &end;
                    r.sparkline(Rect{spark.x, spark.cy() - 11, spark.w, 22}, secondary->data(), secondary->size(),
                                kHistoryWindow, max, item.metric, options);
                }
            }
        }
        TextStyle title;
        title.font = Font::Headline;
        title.valign = VAlign::Top;
        r.text(item.label, Rect{text.x, text.y, text.w, 18}, title, theme.text());
        TextStyle caption;
        caption.font = Font::Caption;
        caption.valign = VAlign::Top;
        caption.tabular = true;
        r.text(detail, Rect{text.x, text.y + 17, text.w, 14}, caption, theme.text_secondary());
    }

    // ---- input ----

    bool sidebar_click(const MouseEvent &e) {
        // Rows scrolled under the bottom controls are not there to click.
        const bool in_list = e.y < pause_button_.y - tokens::space::md * 2;
        for (const NavItem &item : nav_) {
            if (in_list && item.kind != NavItem::Kind::Section && item.rect.contains(e.x, e.y)) {
                navigate(item.page);
                return true;
            }
        }
        if (pause_button_.contains(e.x, e.y)) {
            set_paused(!store_.paused());
            return true;
        }
        if (settings_button_.contains(e.x, e.y)) {
            navigate(PageId::Settings);
            return true;
        }
        return sidebar_.contains(e.x, e.y);
    }

    void prune_overlays() {
        const bool had = !overlays_.empty();
        overlays_.erase(std::remove_if(overlays_.begin(), overlays_.end(), [](const auto &o) { return o->closed(); }),
                        overlays_.end());
        if (had) repaint();
    }

    void on_command(int id) {
        switch (id) {
            case IDM_PAGE_OVERVIEW: navigate(PageId::Overview); break;
            case IDM_PAGE_PROCESSES: navigate(PageId::Processes); break;
            case IDM_PAGE_CPU: navigate(PageId::Cpu); break;
            case IDM_PAGE_MEMORY: navigate(PageId::Memory); break;
            case IDM_PAGE_GPU: navigate(PageId::Gpu); break;
            case IDM_PAGE_DISK: navigate(PageId::Disk); break;
            case IDM_PAGE_NETWORK: navigate(PageId::Network); break;
            case IDM_PAGE_STARTUP: navigate(PageId::Startup); break;
            case IDM_PAGE_SERVICES: navigate(PageId::Services); break;
            case IDM_PAGE_HISTORY: navigate(PageId::History); break;
            case IDM_PAGE_INSPECT: navigate(PageId::Inspect); break;
            case IDM_PAGE_SYSTEM: navigate(PageId::System); break;
            case IDM_PAGE_BATTERY: navigate(PageId::Battery); break;
            case IDM_PAGE_SETTINGS: navigate(PageId::Settings); break;
            case IDM_PALETTE: open_palette(); break;
            case IDM_FIND:
                if (current_ && current_->id() != PageId::Processes && current_->id() != PageId::Services &&
                    current_->id() != PageId::Inspect)
                    navigate(PageId::Processes);
                if (current_) current_->focus_search();
                repaint();
                break;
            case IDM_INFO:
                if (current_) {
                    if (!current_->command(*this, IDM_INFO)) show_info(current_->selected_pid());
                }
                break;
            case IDM_END_TASK:
            case IDM_FORCE_QUIT:
            case IDM_END_TREE:
            case IDM_PIN:
                if (current_) current_->command(*this, id);
                break;
            case IDM_PAUSE: set_paused(!store_.paused()); break;
            case IDM_UNLOCK: relaunch_elevated(); break;
            case IDM_REFRESH: store_.refresh_now(); break;
            case IDM_OPEN: show_from_tray(); break;
            case IDM_QUIT:
                quit_ = true;
                DestroyWindow(hwnd_);
                break;
            default: break;
        }
    }

    void open_palette() {
        std::vector<PaletteItem> items;
        auto screen = [&](PageId id, const wchar_t *title, const wchar_t *shortcut) {
            items.push_back({PaletteItem::Kind::Screen, title, L"", shortcut, [this, id] { navigate(id); }});
        };
        screen(PageId::Overview, L"Overview", L"Ctrl+1");
        screen(PageId::Processes, L"Processes", L"Ctrl+2");
        screen(PageId::Cpu, L"CPU", L"Ctrl+3");
        screen(PageId::Memory, L"Memory", L"Ctrl+4");
        if (store_.has(PC_CAP_GPU)) screen(PageId::Gpu, L"GPU", L"Ctrl+5");
        screen(PageId::Disk, L"Disk", L"Ctrl+6");
        screen(PageId::Network, L"Network", L"Ctrl+7");
        screen(PageId::Startup, L"Startup", L"Ctrl+8");
        screen(PageId::Services, L"Services", L"Ctrl+9");
        screen(PageId::History, L"History", L"");
        screen(PageId::Inspect, L"Files & Ports", L"");
        if (store_.has(PC_CAP_BATTERY)) screen(PageId::Battery, L"Battery", L"");
        screen(PageId::System, L"System", L"");
        screen(PageId::Settings, L"Settings", L"Ctrl+,");
        items.push_back({PaletteItem::Kind::Command, store_.paused() ? L"Resume updates" : L"Pause updates", L"",
                         L"Ctrl+Shift+P", [this] { set_paused(!store_.paused()); }});
        if (!elevated())
            items.push_back({PaletteItem::Kind::Command, L"Unlock Full Access", L"Run Procyon as administrator", L"",
                             [this] { relaunch_elevated(); }});
        if (current_ && current_->selected_pid() > 0) {
            const int32_t pid = current_->selected_pid();
            const pc_process *p = store_.snapshot().find(pid);
            const std::wstring name = p ? process_display_name(*p) : L"PID " + std::to_wstring(pid);
            items.push_back(
                {PaletteItem::Kind::Command, L"End Task: " + name, L"", L"Del", [this] { on_command(IDM_END_TASK); }});
            items.push_back(
                {PaletteItem::Kind::Command, L"Get Info: " + name, L"", L"Ctrl+I", [this, pid] { show_info(pid); }});
        }
        items.push_back({PaletteItem::Kind::Command, L"Quit Procyon", L"", L"", [this] { on_command(IDM_QUIT); }});
        ViewQuery query;
        query.mode = PC_VIEW_GROUPED;
        query.sort_column = PC_COLUMN_CPU;
        query.descending = true;
        query.limit = 60;
        const Snapshot &s = store_.snapshot();
        for (const Row &row : store_.build_view(query)) {
            if (row.depth > 0) continue;
            const pc_process *p = row.process_index >= 0 && row.process_index < static_cast<int32_t>(s.processes.size())
                                      ? &s.processes[static_cast<size_t>(row.process_index)]
                                      : nullptr;
            const std::wstring name = row.is_group() ? row.group_name
                                      : p            ? fmt::from_utf8(p->app_name[0] ? p->app_name : p->name)
                                                     : L"";
            const int32_t pid = row.is_group() ? row.group_pid : p ? p->pid : 0;
            if (name.empty() || pid <= 0) continue;
            PaletteItem item{PaletteItem::Kind::App, name,
                             L"CPU " + fmt::cpu(row.cpu_percent) + L" · " + fmt::bytes(row.memory_bytes), L"",
                             [this, pid] { show_info(pid); }};
            item.icon_path = app_icon_path(row, p, s);
            item.icon_system = p && (p->flags & PC_PROC_SYSTEM) != 0;
            items.push_back(std::move(item));
        }
        overlays_.push_back(make_command_palette(std::move(items)));
        repaint();
    }

    // ---- tray ----

    void add_tray() {
        tray_ = {};
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = hwnd_;
        tray_.uID = kTrayId;
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        tray_.uCallbackMessage = WM_APP_TRAY;
        tray_.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_PROCYON), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                    LR_DEFAULTCOLOR));
        wcscpy_s(tray_.szTip, L"Procyon");
        tray_added_ = Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
    }

    void update_tray() {
        if (!tray_added_) return;
        const Snapshot &s = store_.snapshot();
        std::wstring tip = L"Procyon";
        if (store_.paused()) {
            tip += L" · paused";
        } else {
            // Every module switched on in Settings, like the menu bar modules on macOS.
            if (settings_.tray_modules & 1) tip += L" · CPU " + fmt::percent(s.cpu_usage);
            if (settings_.tray_modules & 2) tip += L" · Memory " + fmt::bytes(static_cast<int64_t>(s.memory_used));
            if (settings_.tray_modules & 4)
                tip += L" · ↓ " + fmt::rate(s.net_rx_bps) + L" ↑ " + fmt::rate(s.net_tx_bps);
            if ((settings_.tray_modules & 8) && !s.gpus.empty())
                tip += L" · GPU " + fmt::percent(s.gpus[0].utilization);
            if ((settings_.tray_modules & 16) && s.cpu_temperature >= 0)
                tip += L" · " + fmt::temperature(s.cpu_temperature, settings_.fahrenheit);
            if (settings_.tray_modules & 32)
                if (auto b = store_.battery(); b && b->present) tip += L" · Battery " + fmt::percent(b->level);
        }
        wcsncpy_s(tray_.szTip, tip.c_str(), _TRUNCATE);
        tray_.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &tray_);
    }

    void remove_tray() {
        if (tray_added_) Shell_NotifyIconW(NIM_DELETE, &tray_);
        tray_added_ = false;
    }

    // An alert as a Windows notification (a balloon of the tray icon, which Windows 10 and 11 show
    // as a toast and keep in the notification center), also while Procyon is in front.
    void notify(const AlertEvent &event) {
        if (!tray_added_) return;
        NOTIFYICONDATAW data = tray_;
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;
        wcsncpy_s(data.szInfoTitle, event.title.c_str(), _TRUNCATE);
        wcsncpy_s(data.szInfo, event.message.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
        repaint();
    }

    void hide_to_tray() {
        if (hidden_) return;
        hidden_ = true;
        ShowWindow(hwnd_, SW_HIDE);
        store_.set_process_sampling(false);
        // Nothing paints while hidden: let go of the GPU device, Direct2D, DirectWrite and the embedded
        // fonts (recreated by show_from_tray), then return the pages the heap no longer uses, so the
        // footprint in the tray is what the sampler touches, not what the window did.
        canvas_.shutdown();
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
    }

    void show_from_tray() {
        if (hidden_) canvas_.init(hwnd_);  // released by hide_to_tray; the dpi is kept
        hidden_ = false;
        store_.set_process_sampling(true);
        ShowWindow(hwnd_, SW_SHOW);
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        store_.refresh_now();
    }

    void tray_menu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Open Procyon");
        AppendMenuW(menu, MF_STRING, IDM_PAUSE, store_.paused() ? L"Resume updates" : L"Pause updates");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");
        SetMenuDefaultItem(menu, IDM_OPEN, FALSE);
        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        const int chosen =
            static_cast<int>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd_, nullptr));
        DestroyMenu(menu);
        if (chosen) on_command(chosen);
    }

    // ---- settings and placement (HKCU\Software\Procyon) ----

    void load_settings() {
        if (!persist_) return;  // --no-settings: defaults, nothing read
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
        auto dword = [&](const wchar_t *name, DWORD fallback) {
            DWORD value = fallback, size = sizeof(value);
            if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE *>(&value), &size) != ERROR_SUCCESS)
                return fallback;
            return value;
        };
        settings_.interval = std::clamp(dword(L"IntervalMs", 1000) / 1000.0, 0.5, 5.0);
        settings_.theme = static_cast<int>(dword(L"Theme", 0));
        settings_.fahrenheit = dword(L"Fahrenheit", 0) != 0;
        settings_.minimize_to_tray = dword(L"MinimizeToTray", 1) != 0;
        settings_.tray_modules = static_cast<int>(dword(L"TrayModules", 1 | 2));
        settings_.default_view = static_cast<int>(dword(L"DefaultView", PC_VIEW_GROUPED));
        if (settings_.default_view < PC_VIEW_FLAT || settings_.default_view > PC_VIEW_TREE)
            settings_.default_view = PC_VIEW_GROUPED;
        settings_.records_history = dword(L"RecordsHistory", 1) != 0;
        DWORD size = sizeof(placement_);
        if (RegQueryValueExW(key, L"Placement", nullptr, nullptr, reinterpret_cast<BYTE *>(&placement_), &size) ==
                ERROR_SUCCESS &&
            size == sizeof(placement_) && placement_.length == sizeof(placement_))
            has_placement_ = true;
        RegCloseKey(key);
    }

    void save_settings() {
        if (!persist_) return;  // --no-settings: nothing written back
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
            ERROR_SUCCESS)
            return;
        auto dword = [&](const wchar_t *name, DWORD value) {
            RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
        };
        dword(L"IntervalMs", static_cast<DWORD>(settings_.interval * 1000));
        dword(L"Theme", static_cast<DWORD>(settings_.theme));
        dword(L"Fahrenheit", settings_.fahrenheit ? 1 : 0);
        dword(L"MinimizeToTray", settings_.minimize_to_tray ? 1 : 0);
        dword(L"TrayModules", static_cast<DWORD>(settings_.tray_modules));
        dword(L"DefaultView", static_cast<DWORD>(settings_.default_view));
        dword(L"RecordsHistory", settings_.records_history ? 1 : 0);
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(hwnd_, &placement)) {
            if (placement.showCmd == SW_SHOWMINIMIZED) placement.showCmd = SW_SHOWNORMAL;
            RegSetValueExW(key, L"Placement", 0, REG_BINARY, reinterpret_cast<const BYTE *>(&placement),
                           sizeof(placement));
        }
        RegCloseKey(key);
    }

    void restore_placement(int show_command) {
        if (has_placement_) {
            placement_.showCmd = SW_SHOWNORMAL;
            SetWindowPlacement(hwnd_, &placement_);
        }
        ShowWindow(hwnd_, has_placement_ ? SW_SHOW : show_command);
        UpdateWindow(hwnd_);
    }

    HMENU build_menu(const std::vector<MenuItem> &items) {
        HMENU menu = CreatePopupMenu();
        for (const MenuItem &item : items) {
            if (item.separator) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                continue;
            }
            UINT flags = MF_STRING | (item.enabled ? MF_ENABLED : MF_GRAYED) | (item.checked ? MF_CHECKED : 0);
            if (!item.children.empty()) {
                HMENU sub = build_menu(item.children);
                AppendMenuW(menu, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(sub), item.label.c_str());
            } else {
                AppendMenuW(menu, flags, static_cast<UINT_PTR>(item.id), item.label.c_str());
            }
        }
        return menu;
    }

    // ---- window procedure ----

    LRESULT proc(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE:
                on_create();
                // Re-run WM_NCCALCSIZE now that the window exists, so the caption goes away.
                SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
                return 0;
            case WM_NCCALCSIZE: {
                // Keep the resize borders, drop the caption: the client area starts at the top of
                // the frame. Maximized, the frame hangs off the screen, so the top moves down by it.
                if (!wparam) break;
                auto params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lparam);
                const LONG top = params->rgrc[0].top;
                DefWindowProcW(hwnd_, message, wparam, lparam);
                params->rgrc[0].top = top;
                if (IsZoomed(hwnd_)) {
                    const UINT dpi = static_cast<UINT>(dpi_);
                    params->rgrc[0].top +=
                        GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                }
                return 0;
            }
            case WM_NCHITTEST: {
                const LRESULT hit = DefWindowProcW(hwnd_, message, wparam, lparam);
                if (hit != HTCLIENT && hit != HTCAPTION && hit != HTTOP && hit != HTTOPLEFT && hit != HTTOPRIGHT)
                    return hit;
                return caption_hit_test(lparam);
            }
            case WM_NCMOUSEMOVE: {
                POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(hwnd_, &pt);
                set_caption_hover(caption_button_at(to_dip(pt.x), to_dip(pt.y)));
                if (!nc_tracking_) {
                    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
                    TrackMouseEvent(&track);
                    nc_tracking_ = true;
                }
                break;
            }
            case WM_NCMOUSELEAVE:
                nc_tracking_ = false;
                set_caption_hover(-1);
                break;
            case WM_NCLBUTTONDOWN:
                if (wparam == HTMINBUTTON || wparam == HTMAXBUTTON || wparam == HTCLOSE) {
                    caption_pressed_ = wparam == HTMINBUTTON ? 0 : wparam == HTMAXBUTTON ? 1 : 2;
                    repaint();
                    return 0;
                }
                break;
            case WM_NCLBUTTONUP: {
                const int pressed = caption_pressed_;
                caption_pressed_ = -1;
                if (pressed >= 0) {
                    const int hit = wparam == HTMINBUTTON ? 0 : wparam == HTMAXBUTTON ? 1 : wparam == HTCLOSE ? 2 : -1;
                    if (hit == pressed) caption_button_action(hit);
                    repaint();
                    return 0;
                }
                break;
            }
            case WM_NCRBUTTONUP:
                if (wparam == HTCAPTION) {
                    // The system menu where the caption was clicked.
                    if (HMENU menu = GetSystemMenu(hwnd_, FALSE)) {
                        const int chosen =
                            static_cast<int>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, GET_X_LPARAM(lparam),
                                                            GET_Y_LPARAM(lparam), 0, hwnd_, nullptr));
                        if (chosen) PostMessageW(hwnd_, WM_SYSCOMMAND, static_cast<WPARAM>(chosen), lparam);
                    }
                    return 0;
                }
                break;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(hwnd_, &ps);
                paint();
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_SIZE:
                canvas_.resize(LOWORD(lparam), HIWORD(lparam));
                repaint();
                return 0;
            case WM_TIMER:
                if (wparam == kFrameTimer) {
                    KillTimer(hwnd_, kFrameTimer);
                    repaint();
                    return 0;
                }
                break;
            case WM_DPICHANGED: {
                dpi_ = static_cast<float>(HIWORD(wparam));
                canvas_.set_dpi(dpi_);
                const RECT *rc = reinterpret_cast<const RECT *>(lparam);
                SetWindowPos(hwnd_, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_GETMINMAXINFO: {
                auto info = reinterpret_cast<MINMAXINFO *>(lparam);
                info->ptMinTrackSize.x = to_px(900);
                info->ptMinTrackSize.y = to_px(600);
                return 0;
            }
            case WM_APP_SNAPSHOT: {
                store_.receive(reinterpret_cast<Snapshot *>(lparam));
                if (nav_.size() < 6) build_nav();
                if (current_) current_->tick(*this);
                for (auto &overlay : overlays_) overlay->tick(*this);
                update_tray();
                if (!hidden_) repaint();
                return 0;
            }
            case WM_APP_TRAY:
                switch (LOWORD(lparam)) {
                    case NIN_SELECT:
                    case NIN_KEYSELECT:
                    case WM_LBUTTONUP:
                    case WM_LBUTTONDBLCLK: show_from_tray(); break;
                    case WM_CONTEXTMENU:
                    case WM_RBUTTONUP: tray_menu(); break;
                    default: break;
                }
                return 0;
            case WM_MOUSEMOVE: {
                set_caption_hover(-1);
                if (!tracking_) {
                    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
                    TrackMouseEvent(&track);
                    tracking_ = true;
                }
                const MouseEvent e = mouse_event(lparam, wparam);
                mouse_x_ = e.x;
                mouse_y_ = e.y;
                if (!overlays_.empty())
                    overlays_.back()->mouse_move(*this, e);
                else if (current_)
                    current_->mouse_move(*this, e);
                repaint();
                return 0;
            }
            case WM_MOUSELEAVE:
                tracking_ = false;
                mouse_x_ = mouse_y_ = -1;
                if (current_) current_->mouse_leave(*this);
                repaint();
                return 0;
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN: {
                SetFocus(hwnd_);
                const MouseEvent e = mouse_event(lparam, wparam);
                const bool right = message == WM_RBUTTONDOWN;
                if (!overlays_.empty()) {
                    overlays_.back()->mouse_down(*this, e, right);
                    prune_overlays();
                } else if (!right && sidebar_click(e)) {
                    // handled
                } else if (current_) {
                    current_->mouse_down(*this, e, right);
                }
                repaint();
                return 0;
            }
            case WM_LBUTTONUP:
                if (overlays_.empty() && current_) current_->mouse_up(*this, mouse_event(lparam, wparam));
                return 0;
            case WM_LBUTTONDBLCLK:
                if (overlays_.empty() && current_) current_->double_click(*this, mouse_event(lparam, wparam));
                repaint();
                return 0;
            case WM_MOUSEWHEEL: {
                POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(hwnd_, &pt);
                const MouseEvent e =
                    mouse_event(MAKELPARAM(pt.x, pt.y), GET_KEYSTATE_WPARAM(wparam), GET_WHEEL_DELTA_WPARAM(wparam));
                if (!overlays_.empty())
                    overlays_.back()->wheel(*this, e);
                else if (sidebar_.contains(e.x, e.y))
                    sidebar_scroll_.wheel(e.wheel, 42 * 2);
                else if (current_)
                    current_->wheel(*this, e);
                repaint();
                return 0;
            }
            case WM_KEYDOWN: {
                const KeyEvent e = key_event(wparam);
                bool handled = false;
                if (!overlays_.empty()) {
                    handled = overlays_.back()->key(*this, e);
                    prune_overlays();
                } else if (current_) {
                    handled = current_->key(*this, e);
                    if (!handled && e.key == Key::Escape) handled = true;
                }
                repaint();
                return handled ? 0 : DefWindowProcW(hwnd_, message, wparam, lparam);
            }
            case WM_CHAR: {
                const wchar_t c = static_cast<wchar_t>(wparam);
                if (c < 0x20) return 0;
                if (!overlays_.empty())
                    overlays_.back()->character(*this, c);
                else if (current_)
                    current_->character(*this, c);
                repaint();
                return 0;
            }
            case WM_SETCURSOR:
                if (LOWORD(lparam) == HTCLIENT) {
                    Cursor cursor = Cursor::Arrow;
                    if (overlays_.empty() && current_ && content_.contains(mouse_x_, mouse_y_))
                        cursor = current_->cursor();
                    if (overlays_.empty() && sidebar_.contains(mouse_x_, mouse_y_)) {
                        for (const NavItem &item : nav_)
                            if (item.kind != NavItem::Kind::Section && item.rect.contains(mouse_x_, mouse_y_))
                                cursor = Cursor::Hand;
                        if (pause_button_.contains(mouse_x_, mouse_y_) || settings_button_.contains(mouse_x_, mouse_y_))
                            cursor = Cursor::Hand;
                    }
                    SetCursor(LoadCursorW(nullptr, cursor == Cursor::Hand    ? IDC_HAND
                                                   : cursor == Cursor::IBeam ? IDC_IBEAM
                                                                             : IDC_ARROW));
                    return TRUE;
                }
                break;
            case WM_COMMAND: on_command(LOWORD(wparam)); return 0;
            case WM_SETTINGCHANGE:
                if (lparam && wcscmp(reinterpret_cast<const wchar_t *>(lparam), L"ImmersiveColorSet") == 0)
                    apply_theme();
                return 0;
            case WM_THEMECHANGED: apply_theme(); return 0;
            case WM_SYSCOMMAND:
                if ((wparam & 0xFFF0) == SC_MINIMIZE && settings_.minimize_to_tray) {
                    hide_to_tray();
                    return 0;
                }
                break;
            case WM_CLOSE:
                if (settings_.minimize_to_tray && !quit_) {
                    hide_to_tray();
                    return 0;
                }
                DestroyWindow(hwnd_);
                return 0;
            case WM_DESTROY:
                save_settings();
                remove_tray();
                store_.stop();
                store_.flush_history();  // the minute in progress would otherwise go with the process
                canvas_.shutdown();
                PostQuitMessage(0);
                return 0;
            default: break;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HINSTANCE instance_;
    std::optional<PageId> initial_page_;
    bool persist_ = true;  // read and write HKCU\Software\Procyon
    HWND hwnd_ = nullptr;
    float dpi_ = 96;
    D2DCanvas canvas_;
    Renderer renderer_{canvas_};
    Store store_;
    Settings settings_;
    Theme theme_;
    std::vector<std::unique_ptr<Page>> pages_;
    Page *current_ = nullptr;
    std::vector<std::unique_ptr<Overlay>> overlays_;
    std::vector<NavItem> nav_;
    Rect sidebar_, content_, pause_button_, settings_button_;
    Rect caption_buttons_[3];
    int caption_hover_ = -1, caption_pressed_ = -1;
    ScrollState sidebar_scroll_;
    float mouse_x_ = -1, mouse_y_ = -1;
    bool tracking_ = false, nc_tracking_ = false;
    bool mica_ = false;  // the Mica backdrop is on, so the sidebar is translucent
    NOTIFYICONDATAW tray_{};
    bool tray_added_ = false;
    bool hidden_ = false;
    bool quit_ = false;
    WINDOWPLACEMENT placement_{};
    bool has_placement_ = false;
};

MainWindow *g_window = nullptr;

}  // namespace

int run_app(HINSTANCE instance, const std::wstring &args, int show_command) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::optional<PageId> initial;
    if (const size_t at = args.find(L"--page "); at != std::wstring::npos) {
        std::wstring name = args.substr(at + 7);
        if (const size_t space = name.find(L' '); space != std::wstring::npos) name.resize(space);
        initial = page_from_name(name);
    }

    // --no-settings: the defaults (1 s refresh, close to tray) and no registry writes, for measurements.
    MainWindow window(instance, initial, args.find(L"--no-settings") == std::wstring::npos);
    g_window = &window;
    if (!window.create(show_command)) return 1;
    HACCEL accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDR_ACCELERATORS));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        bool use_accelerators = true;
        if (msg.message == WM_KEYDOWN && window.text_input_active() &&
            (msg.wParam == VK_DELETE || msg.wParam == VK_BACK || msg.wParam == VK_RETURN) &&
            !(GetKeyState(VK_CONTROL) & 0x8000))
            use_accelerators = false;
        if (use_accelerators && accelerators && TranslateAcceleratorW(window.hwnd(), accelerators, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_window = nullptr;
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

}  // namespace procyon::ui
