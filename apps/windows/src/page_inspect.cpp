// Files & Ports: listening ports, connections, and "Who Is Using…" a file, folder or drive, laid
// out like the macOS InspectView (segmented tabs in the header, search + options, table card).
#include <shobjidl.h>

#include <algorithm>

#include "pages.hpp"
#include "widgets.hpp"

namespace procyon::ui {
namespace {

enum Columns { ColProcess = 1, ColPort, ColProtocol, ColLocal, ColRemote, ColState, ColPath, ColKind };
enum Tabs { TabPorts = 0, TabConnections, TabFiles };
enum MenuIds { MenuInfo = 1, MenuCopy, MenuOpen };

const wchar_t *state_name(int32_t state) {
    switch (state) {
        case PC_TCP_LISTEN: return L"Listening";
        case PC_TCP_SYN_SENT: return L"Connecting";
        case PC_TCP_SYN_RECEIVED: return L"Accepting";
        case PC_TCP_ESTABLISHED: return L"Established";
        case PC_TCP_CLOSE_WAIT: return L"Close wait";
        case PC_TCP_CLOSING: return L"Closing";
        case PC_TCP_TIME_WAIT: return L"Time wait";
        case PC_TCP_CLOSED: return L"Closed";
        default: return L"";
    }
}

std::wstring lower(std::wstring s) {
    for (wchar_t &c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

bool loopback(const std::wstring &address) {
    return address == L"127.0.0.1" || address == L"::1" || address.rfind(L"127.", 0) == 0;
}

// A folder picker; empty when cancelled.
std::wstring pick_folder(HWND owner) {
    std::wstring result;
    IFileOpenDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return result;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Who is using this folder or drive?");
    if (SUCCEEDED(dialog->Show(owner))) {
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

class InspectPage : public Page {
public:
    InspectPage() {
        table_.row_count = [this] { return static_cast<int>(rows_.size()); };
        table_.cell = [this](int row, int column) { return cell(row, column); };
        table_.on_sort = [this](int) { sort(); };
        table_.on_context = [this](int row, float x, float y) { context(row, x, y); };
        table_.on_activate = [this](int row) {
            if (host_ && row >= 0 && row < static_cast<int>(rows_.size()))
                host_->show_info(rows_[static_cast<size_t>(row)].pid);
        };
        table_.row_height = 48;
        search_.keycap = L"";
        set_tab(TabPorts);
    }
    PageId id() const override { return PageId::Inspect; }
    std::wstring title() const override { return L"Files & Ports"; }

    void activate(Host &host) override {
        host_ = &host;
        reload();
    }
    void tick(Host &host) override {
        host_ = &host;
        if (tab_ != TabFiles && host.store().ticks() % 3 == 0) reload();
    }

    void paint(Host &host, const Rect &bounds) override {
        host_ = &host;
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        const float mx = host.mouse_x(), my = host.mouse_y();
        Rect area = screen_area(bounds);
        Rect trailing;
        area = page_header(r, area, L"Files & Ports", subtitle_, std::nullopt, &trailing);
        area.y -= kSectionGap - tokens::space::lg;
        area.h += kSectionGap - tokens::space::lg;
        const std::vector<std::wstring> tabs = {L"Listening Ports", L"Connections", L"Open Files"};
        const float sw = r.segmented_width(tabs);
        tab_rects_ = r.segmented(Rect{trailing.right() - sw, trailing.y + 2, sw, kControlHeight}, tabs, tab_);

        Rect toolbar = area.take_top(kControlHeight);
        search_.bounds = toolbar.take_left(std::min(340.0f, toolbar.w * 0.45f));
        search_.placeholder = tab_ == TabFiles ? L"Search by path or app" : L"Search by app, port or address";
        search_.paint(r);
        toolbar.take_left(tokens::space::md);
        refresh_button_ = Rect{toolbar.right() - 28, toolbar.y, 28, 28};
        r.icon_button(refresh_button_, Renderer::Symbol::Refresh, theme.text_secondary(),
                      refresh_button_.contains(mx, my));
        checkbox_ = {};
        browse_button_ = {};
        clear_button_ = {};
        if (tab_ != TabFiles) {
            // "Hide local-only" checkbox.
            checkbox_ =
                Rect{toolbar.x, toolbar.y, 14 + tokens::space::xs + 2 + r.measure(L"Hide local-only", Font::Body), 28};
            const Rect box{toolbar.x, toolbar.cy() - 7, 14, 14};
            r.fill_round(box, 3, hide_loopback_ ? theme.accent() : theme.surface_sunken());
            r.stroke_round(box, 3, hide_loopback_ ? theme.accent() : theme.border_strong());
            if (hide_loopback_)
                r.symbol(Renderer::Symbol::Check, box.inset(2, 2), D2D1::ColorF(D2D1::ColorF::White), 1.6f);
            TextStyle style;
            style.font = Font::Body;
            r.text(L"Hide local-only", Rect{box.right() + tokens::space::xs + 2, toolbar.y, 200, 28}, style,
                   theme.text());
        } else {
            browse_button_ =
                button(r, toolbar.x, toolbar.y, L"Who Is Using…", false, false, browse_button_.contains(mx, my));
            if (!target_.empty()) {
                float x = browse_button_.right() + tokens::space::md;
                std::wstring name = target_;
                while (!name.empty() && name.back() == L'\\') name.pop_back();
                if (const size_t slash = name.find_last_of(L'\\');
                    slash != std::wstring::npos && slash + 1 < name.size())
                    name = name.substr(slash + 1);
                x += r.badge(x, toolbar.cy() - 8.5f, name, Renderer::Tone::Accent) + tokens::space::xs;
                clear_button_ = Rect{x, toolbar.y, 28, 28};
                r.icon_button(clear_button_, Renderer::Symbol::Close, theme.text_tertiary(),
                              clear_button_.contains(mx, my));
            }
        }
        area.take_top(tokens::space::lg);
        banner_button_ = {};
        if (!complete_ && !host.elevated()) {
            const Banner b =
                action_banner(r, area.take_top(kActionBannerHeight), L"Processes of other users are hidden",
                              L"System services and other users' processes need administrator access to show their "
                              L"files and sockets.",
                              L"Unlock Full Access", Renderer::Tone::Accent, banner_button_.contains(mx, my));
            banner_button_ = b.button;
            area.take_top(tokens::space::lg);
        }
        area.h = bounds.bottom() - tokens::space::xl - area.y;
        r.card(area);
        const Rect inner = area.inset(1, 1);
        table_.layout(inner);
        r.push_clip(inner);
        table_.paint(r);
        r.pop_clip();
        if (rows_.empty()) {
            if (tab_ == TabFiles)
                empty_state(
                    r, inner, loaded_files_ ? L"No open files match" : L"Open files",
                    loaded_files_
                        ? L"Nothing has the file or folder open."
                        : L"Pick a folder or drive with Who Is Using…, or press Refresh to list every open file.");
            else
                empty_state(r, inner, L"Nothing to show", L"");
        }
    }

    void mouse_move(Host &, const MouseEvent &e) override { table_.mouse_move(e); }
    void mouse_leave(Host &) override { table_.mouse_leave(); }
    void mouse_down(Host &host, const MouseEvent &e, bool right) override {
        for (size_t i = 0; i < tab_rects_.size(); ++i) {
            if (tab_rects_[i].contains(e.x, e.y)) {
                set_tab(static_cast<int>(i));
                reload();
                return;
            }
        }
        bool cleared = false;
        if (search_.mouse_down(e.x, e.y, cleared)) {
            table_.focused = false;
            if (cleared) sort();
            return;
        }
        if (right) {
            table_.mouse_down(e, true);
            return;
        }
        if (!checkbox_.empty() && checkbox_.contains(e.x, e.y)) {
            hide_loopback_ = !hide_loopback_;
            sort();
            return;
        }
        if (!browse_button_.empty() && browse_button_.contains(e.x, e.y)) {
            const std::wstring picked = pick_folder(host.hwnd());
            if (!picked.empty()) {
                target_ = picked;
                load_files();
            }
            return;
        }
        if (!clear_button_.empty() && clear_button_.contains(e.x, e.y)) {
            target_.clear();
            sort();
            return;
        }
        if (refresh_button_.contains(e.x, e.y)) {
            if (tab_ == TabFiles)
                load_files();
            else
                reload();
            return;
        }
        if (!banner_button_.empty() && banner_button_.contains(e.x, e.y)) {
            host.relaunch_elevated();
            return;
        }
        table_.mouse_down(e, false);
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
        return table_.selected >= 0 && table_.selected < static_cast<int>(rows_.size())
                   ? rows_[static_cast<size_t>(table_.selected)].pid
                   : 0;
    }

private:
    struct RowData {
        int32_t pid;
        std::wstring process;
        std::wstring a, b, c, d;  // tab-specific columns
        int32_t state = 0;
        int sort_key = 0;
        bool local_only = false;
    };

    void set_tab(int tab) {
        tab_ = tab;
        rows_.clear();
        table_.selected = -1;
        table_.scroll.offset = 0;
        if (tab == TabFiles) {
            table_.columns = {{ColProcess, L"App", 220, 1, HAlign::Left, true, false},
                              {ColKind, L"Kind", 110, 0, HAlign::Left, true, false},
                              {ColPath, L"Path", 300, 3, HAlign::Left, true, false}};
            table_.sort_column = ColProcess;
            table_.sort_descending = false;
        } else if (tab == TabPorts) {
            table_.columns = {{ColProcess, L"App", 220, 1.5f, HAlign::Left, true, false},
                              {ColPort, L"Port", 90, 0, HAlign::Left},
                              {ColProtocol, L"Protocol", 120, 0, HAlign::Left, true, false},
                              {ColRemote, L"Address", 180, 1, HAlign::Left, true, false},
                              {ColState, L"Reachable From", 140, 0, HAlign::Left, true, false}};
            table_.sort_column = ColPort;
            table_.sort_descending = false;
        } else {
            table_.columns = {{ColProcess, L"App", 200, 1, HAlign::Left, true, false},
                              {ColRemote, L"Remote", 200, 1.5f, HAlign::Left, true, false},
                              {ColLocal, L"Local", 170, 1, HAlign::Left, true, false},
                              {ColProtocol, L"Protocol", 110, 0, HAlign::Left, true, false},
                              {ColState, L"State", 110, 0, HAlign::Left, true, false}};
            table_.sort_column = ColProcess;
            table_.sort_descending = false;
        }
        search_.clear();
    }

    std::wstring process_name(int32_t pid) const {
        if (!host_) return L"";
        const pc_process *p = host_->store().snapshot().find(pid);
        if (p) return process_display_name(*p);
        return pid == 4 ? L"System" : pid == 0 ? L"System Idle Process" : L"PID " + std::to_wstring(pid);
    }

    void reload() {
        if (!host_) return;
        if (tab_ == TabFiles) {
            if (loaded_files_) sort();
            return;
        }
        all_.clear();
        bool complete = true;
        const std::vector<pc_connection> connections = host_->store().connections(-1, complete);
        complete_ = complete;
        for (const pc_connection &c : connections) {
            const bool listen = c.protocol == PC_PROTOCOL_UDP || c.state == PC_TCP_LISTEN;
            if (tab_ == TabPorts && !listen) continue;
            if (tab_ == TabConnections && listen) continue;
            RowData row;
            row.pid = c.pid;
            row.process = process_name(c.pid);
            row.a = std::wstring(c.protocol == PC_PROTOCOL_TCP ? L"TCP" : L"UDP") +
                    (c.family == 6 ? L" · IPv6" : L" · IPv4");
            const std::wstring local = fmt::from_utf8(c.local_address);
            row.local_only = loopback(local);
            if (tab_ == TabPorts) {
                row.b = std::to_wstring(c.local_port);
                row.c = local == L"*" ? L"All addresses" : local;
                row.d = row.local_only ? L"This PC" : L"Network";
                row.state = row.local_only ? 0 : 1;
                row.sort_key = c.local_port;
            } else {
                row.b = local + L":" + std::to_wstring(c.local_port);
                row.c = c.remote_address[0] ? fmt::from_utf8(c.remote_address) + L":" + std::to_wstring(c.remote_port)
                                            : std::wstring(fmt::unavailable);
                row.d = state_name(c.state);
                row.state = c.state;
                row.sort_key = c.local_port;
            }
            all_.push_back(std::move(row));
        }
        sort();
    }

    void load_files() {
        if (!host_) return;
        bool complete = true;
        const std::vector<OpenFileCopy> files = host_->store().open_files(-1, complete);
        complete_ = complete;
        all_.clear();
        for (const OpenFileCopy &f : files) {
            RowData row;
            row.pid = f.pid;
            row.process = process_name(f.pid);
            row.a = f.kind == PC_FILE_CWD         ? L"Working directory"
                    : f.kind == PC_FILE_DIRECTORY ? L"Folder"
                    : f.kind == PC_FILE_OTHER     ? L"Device"
                                                  : L"File";
            row.b = f.path;
            all_.push_back(std::move(row));
        }
        loaded_files_ = true;
        sort();
    }

    void sort() {
        rows_.clear();
        const std::wstring needle = lower(search_.text);
        std::wstring prefix = lower(target_);
        while (!prefix.empty() && prefix.back() == L'\\') prefix.pop_back();
        int exposed = 0;
        for (const RowData &row : all_) {
            if (tab_ != TabFiles && hide_loopback_ && row.local_only) continue;
            if (tab_ == TabFiles && !prefix.empty()) {
                const std::wstring path = lower(row.b);
                if (path.rfind(prefix, 0) != 0) continue;
                if (path.size() > prefix.size() && path[prefix.size()] != L'\\') continue;
            }
            if (!needle.empty() && lower(row.process).find(needle) == std::wstring::npos &&
                lower(row.b).find(needle) == std::wstring::npos && lower(row.c).find(needle) == std::wstring::npos &&
                std::to_wstring(row.pid).find(needle) == std::wstring::npos)
                continue;
            if (tab_ == TabPorts && !row.local_only) ++exposed;
            rows_.push_back(row);
        }
        const int column = table_.sort_column;
        const bool desc = table_.sort_descending;
        std::stable_sort(rows_.begin(), rows_.end(), [&](const RowData &x, const RowData &y) {
            int order = 0;
            switch (column) {
                case ColProtocol:
                case ColKind: order = _wcsicmp(x.a.c_str(), y.a.c_str()); break;
                case ColPort: order = x.sort_key - y.sort_key; break;
                case ColLocal:
                case ColPath: order = _wcsicmp(x.b.c_str(), y.b.c_str()); break;
                case ColRemote: order = _wcsicmp(x.c.c_str(), y.c.c_str()); break;
                case ColState: order = x.state - y.state; break;
                default: order = _wcsicmp(x.process.c_str(), y.process.c_str()); break;
            }
            if (order == 0) order = x.pid - y.pid;
            return desc ? order > 0 : order < 0;
        });
        table_.select(std::min(table_.selected, static_cast<int>(rows_.size()) - 1));
        switch (tab_) {
            case TabPorts:
                subtitle_ = std::to_wstring(rows_.size()) + L" listening · " + std::to_wstring(exposed) +
                            L" reachable from the network";
                break;
            case TabConnections: subtitle_ = std::to_wstring(rows_.size()) + L" connections"; break;
            default:
                subtitle_ = loaded_files_ ? fmt::count(static_cast<int64_t>(rows_.size())) + L" open files"
                                          : L"Who holds a file or folder open";
                break;
        }
    }

    Cell cell(int row, int column) {
        const RowData &d = rows_[static_cast<size_t>(row)];
        const Theme &theme = host_->renderer().theme();
        Cell c;
        switch (column) {
            case ColProcess:
                c.text = d.process;
                c.bold = true;
                c.subtitle = L"PID " + std::to_wstring(d.pid);
                break;
            case ColPort:
                c.text = d.b;
                c.mono = true;
                break;
            case ColProtocol:
            case ColKind:
                c.text = d.a;
                c.color = theme.text_secondary();
                break;
            case ColLocal:
            case ColPath:
                c.text = d.b;
                c.mono = true;
                break;
            case ColRemote:
                c.text = d.c;
                c.mono = true;
                c.dim = d.c == fmt::unavailable;
                break;
            case ColState:
                c.text = d.d;
                if (tab_ == TabPorts)
                    c.badge = d.state ? Renderer::Tone::Warning : Renderer::Tone::Neutral;
                else
                    c.color = theme.text_secondary();
                break;
            default: break;
        }
        return c;
    }

    void context(int row, float x, float y) {
        if (!host_) return;
        const RowData d = rows_[static_cast<size_t>(row)];
        std::vector<MenuItem> items = {{MenuInfo, L"Get Info"},
                                       {MenuCopy, tab_ == TabFiles ? L"Copy Path" : L"Copy Address"}};
        if (tab_ == TabFiles) items.push_back({MenuOpen, L"Show in Explorer"});
        switch (host_->popup_menu(items, x, y)) {
            case MenuInfo: host_->show_info(d.pid); break;
            case MenuCopy:
                host_->copy_to_clipboard(tab_ == TabFiles ? d.b : tab_ == TabPorts ? d.c + L":" + d.b : d.c);
                break;
            case MenuOpen: host_->open_in_explorer(d.b); break;
            default: break;
        }
    }

    Host *host_ = nullptr;
    Table table_;
    TextField search_;
    std::vector<Rect> tab_rects_;
    Rect refresh_button_, checkbox_, browse_button_, clear_button_, banner_button_;
    int tab_ = TabPorts;
    bool complete_ = true;
    bool hide_loopback_ = false;
    bool loaded_files_ = false;
    std::wstring target_;
    std::wstring subtitle_ = L"What is listening, connected, and holding files open";
    std::vector<RowData> all_, rows_;
};

}  // namespace

std::unique_ptr<Page> make_inspect_page() { return std::make_unique<InspectPage>(); }

}  // namespace procyon::ui
