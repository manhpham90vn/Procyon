// Processes: every process as a flat list, grouped by app or as a tree, with search, sorting and
// the actions the core allows (End Task, Force Quit, End Process Tree, suspend, priority).
#include <algorithm>
#include <unordered_set>

#include "pages.hpp"
#include "resource.h"
#include "widgets.hpp"

namespace procyon::ui {
namespace {

enum MenuIds {
    MenuEnd = 1,
    MenuForce,
    MenuTree,
    MenuSuspend,
    MenuResume,
    MenuInfo,
    MenuOpenLocation,
    MenuCopyName,
    MenuCopyPath,
    MenuCopyPid,
    MenuPin,
    MenuPriorityBase = 100,  // + index into kPriorities
};

struct Priority {
    const wchar_t *label;
    int32_t nice;
};
const Priority kPriorities[] = {
    {L"High", -15}, {L"Above normal", -8}, {L"Normal", 0}, {L"Below normal", 8}, {L"Low", 15}};

// Column ids are pc_column values; the name column is PC_COLUMN_NAME.
class ProcessesPage : public Page {
public:
    ProcessesPage() {
        table_.row_count = [this] { return static_cast<int>(visible_.size()); };
        table_.cell = [this](int row, int column) { return cell(row, column); };
        table_.indent = [this](int row) { return this->row(row).depth; };
        table_.expander = [this](int row) {
            const Row &r = this->row(row);
            if (r.child_count <= 0) return 0;
            return collapsed(r) ? 1 : 2;
        };
        table_.on_toggle = [this](int row) { toggle(row); };
        table_.on_sort = [this](int column) {
            query_.sort_column = column;
            query_.descending = table_.sort_descending;
            rebuild();
        };
        table_.on_activate = [this](int row) {
            if (host_) host_->show_info(pid_of(this->row(row)));
        };
        table_.on_context = [this](int row, float x, float y) { context(row, x, y); };
        table_.on_select = [this](int row) { selected_key_ = row >= 0 ? key_of(this->row(row)) : ""; };
        table_.sort_column = PC_COLUMN_CPU;
        table_.sort_descending = true;
        table_.row_height = 24;
        query_.mode = PC_VIEW_GROUPED;
        search_.placeholder = L"Search by name, PID or user";
    }

    PageId id() const override { return PageId::Processes; }
    std::wstring title() const override { return L"Processes"; }

    void activate(Host &host) override {
        host_ = &host;
        if (!activated_) {
            activated_ = true;
            query_.mode = host.settings().default_view;  // the view Settings chooses for launch
        }
        build_columns(host);
        rebuild();
    }
    void tick(Host &host) override {
        host_ = &host;
        rebuild();
    }

    void paint(Host &host, const Rect &bounds) override {
        host_ = &host;
        Renderer &r = host.renderer();
        const Theme &theme = r.theme();
        const Snapshot &s = host.store().snapshot();
        if (table_.columns.empty()) build_columns(host);
        const float mx = host.mouse_x(), my = host.mouse_y();
        Rect area = screen_area(bounds);
        area.h = bounds.bottom() - tokens::space::xl - area.y;
        Rect trailing;
        const std::wstring subtitle =
            host.store().has_snapshot()
                ? fmt::count(s.process_count) + L" processes · " + fmt::count(s.thread_count) + L" threads · CPU " +
                      fmt::percent(s.cpu_usage) + L" · Memory " + fmt::bytes(static_cast<int64_t>(s.memory_used))
                : L"Collecting…";
        area = page_header(r, area, L"Processes", subtitle, std::nullopt, &trailing);
        area.y -= kSectionGap - tokens::space::lg;  // the screens with tables use the tighter spacing
        area.h += kSectionGap - tokens::space::lg;

        // View mode segmented control in the header's trailing slot.
        const std::vector<std::wstring> modes = {L"All Processes", L"By App", L"Tree"};
        const int32_t mode_values[] = {PC_VIEW_FLAT, PC_VIEW_GROUPED, PC_VIEW_TREE};
        const float sw = r.segmented_width(modes);
        int selected_mode = 1;
        for (int i = 0; i < 3; ++i)
            if (query_.mode == mode_values[i]) selected_mode = i;
        mode_rects_ =
            r.segmented(Rect{trailing.right() - sw, trailing.y + 2, sw, kControlHeight}, modes, selected_mode);
        if (host.elevated()) {
            const float bw = r.badge(0, 0, L"Full access", Renderer::Tone::Success, true);
            r.badge(trailing.right() - sw - tokens::space::md - bw, trailing.y + 2 + 5, L"Full access",
                    Renderer::Tone::Success);
        }

        // Toolbar: search, match count, expand/collapse, Get Info, End Task.
        Rect toolbar = area.take_top(kControlHeight);
        search_.bounds = toolbar.take_left(std::min(340.0f, toolbar.w * 0.5f));
        search_.paint(r);
        if (!search_.text.empty()) {
            toolbar.take_left(tokens::space::md);
            const int matches = static_cast<int>(visible_.size());
            TextStyle style;
            style.font = Font::Label;
            r.text(matches == 1 ? L"1 match" : std::to_wstring(matches) + L" matches", toolbar.take_left(90), style,
                   theme.text_tertiary());
        }
        const Row *selected = table_.selected >= 0 && table_.selected < static_cast<int>(visible_.size())
                                  ? &row(table_.selected)
                                  : nullptr;
        const bool can_end = selected && !is_protected(*selected);
        end_button_ = end_task_button(r, toolbar.right(), toolbar.y, can_end, end_button_.contains(mx, my));
        float right = end_button_.x - tokens::space::md;
        info_button_ = Rect{right - 28, toolbar.y, 28, 28};
        r.icon_button(info_button_, Renderer::Symbol::Info, theme.text_secondary(), info_button_.contains(mx, my),
                      selected != nullptr);
        right = info_button_.x - tokens::space::xs;
        expand_button_ = {};
        if (query_.mode != PC_VIEW_FLAT) {
            expand_button_ = Rect{right - 28, toolbar.y, 28, 28};
            r.icon_button(expand_button_, all_collapsed_ ? Renderer::Symbol::Expand : Renderer::Symbol::Collapse,
                          theme.text_secondary(), expand_button_.contains(mx, my));
        }
        area.take_top(tokens::space::lg);

        banner_button_ = {};
        if (!host.elevated()) {
            const Banner b =
                action_banner(r, area.take_top(kActionBannerHeight), L"Processes of other users are locked",
                              L"Their command lines, open files and End Task need administrator access. Procyon "
                              L"restarts once, with a UAC prompt.",
                              L"Unlock Full Access", Renderer::Tone::Accent, banner_button_.contains(mx, my));
            banner_button_ = b.button;
            area.take_top(tokens::space::lg);
        }

        r.card(area);
        const Rect inner = area.inset(1, 1);
        table_.layout(inner);
        r.push_clip(inner);
        table_.paint(r);
        r.pop_clip();
        if (visible_.empty() && host.store().has_snapshot())
            empty_state(
                r, inner, search_.text.empty() ? L"No processes" : L"No matching processes",
                search_.text.empty() ? L"" : L"Nothing matches “" + search_.text + L"”. Try a name, PID or user.");
    }

    void mouse_move(Host &, const MouseEvent &e) override { table_.mouse_move(e); }
    void mouse_leave(Host &) override { table_.mouse_leave(); }
    void mouse_down(Host &host, const MouseEvent &e, bool right) override {
        bool cleared = false;
        if (search_.mouse_down(e.x, e.y, cleared)) {
            table_.focused = false;
            if (cleared) {
                query_.filter.clear();
                rebuild();
            }
            return;
        }
        if (right) {
            table_.mouse_down(e, true);
            return;
        }
        for (size_t i = 0; i < mode_rects_.size(); ++i) {
            if (mode_rects_[i].contains(e.x, e.y)) {
                const int32_t values[] = {PC_VIEW_FLAT, PC_VIEW_GROUPED, PC_VIEW_TREE};
                query_.mode = values[i];
                collapsed_.clear();
                all_collapsed_ = false;
                rebuild();
                return;
            }
        }
        if (end_button_.contains(e.x, e.y)) {
            command(host, IDM_END_TASK);
            return;
        }
        if (info_button_.contains(e.x, e.y)) {
            command(host, IDM_INFO);
            return;
        }
        if (!expand_button_.empty() && expand_button_.contains(e.x, e.y)) {
            collapsed_.clear();
            all_collapsed_ = !all_collapsed_;
            rebuild();
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

    bool key(Host &host, const KeyEvent &e) override {
        bool consumed = false;
        if (search_.key(e, consumed)) {
            query_.filter = fmt::to_utf8(search_.text);
            rebuild();
        }
        if (consumed) return true;
        if (search_.focused && e.vk == VK_RETURN) {
            search_.focused = false;
            table_.focused = true;
            if (table_.selected < 0 && !visible_.empty()) table_.select(0);
            return true;
        }
        if (search_.focused && (e.vk == VK_DOWN || e.vk == VK_UP)) {
            search_.focused = false;
            table_.focused = true;
        }
        if (table_.key(e)) return true;
        if (e.vk == VK_SPACE && table_.selected >= 0 && !search_.focused) {
            toggle(table_.selected);
            return true;
        }
        (void)host;
        return false;
    }
    bool character(Host &, wchar_t c) override {
        if (search_.character(c)) {
            query_.filter = fmt::to_utf8(search_.text);
            rebuild();
            return true;
        }
        return false;
    }
    void focus_search() override {
        search_.focused = true;
        table_.focused = false;
    }

    // From a "top apps" row elsewhere: the by-app view (the saved default stays), no search, the
    // app pinned at the top and selected with its processes shown, like the macOS showInProcesses.
    void reveal(const std::string &app_id, int32_t pid) override {
        query_.mode = PC_VIEW_GROUPED;
        search_.clear();
        query_.filter.clear();
        all_collapsed_ = false;
        collapsed_.clear();
        if (!app_id.empty()) pinned_app_ = app_id;
        selected_key_.clear();
        rebuild();
        int target = -1;
        for (size_t i = 0; i < visible_.size() && target < 0; ++i) {
            const Row &r = row(static_cast<int>(i));
            if (r.is_group() ? r.group_id == app_id : (pid_of(r) == pid && r.depth == 0)) target = static_cast<int>(i);
        }
        if (target >= 0) {
            table_.select(target);
            selected_key_ = key_of(row(target));
        }
        search_.focused = false;
        table_.focused = true;
    }
    bool search_focused() const override { return search_.focused; }
    int32_t selected_pid() const override {
        return table_.selected >= 0 && table_.selected < static_cast<int>(visible_.size())
                   ? pid_of(row(table_.selected))
                   : 0;
    }
    LPCWSTR cursor() const override { return IDC_ARROW; }

    bool command(Host &host, int id) override {
        host_ = &host;
        const Row *selected = table_.selected >= 0 && table_.selected < static_cast<int>(visible_.size())
                                  ? &row(table_.selected)
                                  : nullptr;
        switch (id) {
            case IDM_END_TASK:
                if (selected) end(*selected, false, false);
                return true;
            case IDM_FORCE_QUIT:
                if (selected) end(*selected, true, false);
                return true;
            case IDM_END_TREE:
                if (selected) end(*selected, true, true);
                return true;
            case IDM_INFO:
                if (selected) host.show_info(pid_of(*selected));
                return true;
            case IDM_PIN:
                if (selected) toggle_pin(*selected);
                return true;
            default: return false;
        }
    }

private:
    const Row &row(int visible_index) const { return rows_[visible_[static_cast<size_t>(visible_index)]]; }

    const pc_process *process_of(const Row &r) const {
        if (!host_ || r.process_index < 0) return nullptr;
        const Snapshot &s = host_->store().snapshot();
        return r.process_index < static_cast<int32_t>(s.processes.size())
                   ? &s.processes[static_cast<size_t>(r.process_index)]
                   : nullptr;
    }

    int32_t pid_of(const Row &r) const {
        if (r.is_group()) return r.group_pid;
        const pc_process *p = process_of(r);
        return p ? p->pid : 0;
    }

    std::string key_of(const Row &r) const {
        if (r.is_group()) return "g:" + r.group_id;
        return "p:" + std::to_string(pid_of(r));
    }

    // The app a row belongs to: the grouping key of the core.
    std::string app_of(const Row &r) const {
        if (r.is_group()) return r.group_id;
        const pc_process *p = process_of(r);
        return p ? p->app_id : std::string();
    }

    // Whether the row carries the pin: a top-level row (group, process or subtree root) of the
    // pinned app, like the macOS table.
    bool pinned(const Row &r) const { return !pinned_app_.empty() && r.depth == 0 && app_of(r) == pinned_app_; }

    void toggle_pin(const Row &r) {
        const std::string app = app_of(r);
        pinned_app_ = pinned_app_ == app ? std::string() : app;
        rebuild();
    }

    // Moves the pinned app's top-level rows, each with its subtree, above the sorted rows; the
    // parent links are re-indexed to the new order.
    void pin_rows() {
        if (pinned_app_.empty()) return;
        std::vector<size_t> first, rest;
        for (size_t i = 0; i < rows_.size();) {
            size_t end = i + 1;
            while (end < rows_.size() && rows_[end].depth > rows_[i].depth) ++end;
            std::vector<size_t> &target = app_of(rows_[i]) == pinned_app_ ? first : rest;
            for (size_t j = i; j < end; ++j) target.push_back(j);
            i = end;
        }
        if (first.empty()) return;  // the app isn't running (or is filtered out): nothing moves
        first.insert(first.end(), rest.begin(), rest.end());
        std::vector<int32_t> new_index(rows_.size(), -1);
        for (size_t n = 0; n < first.size(); ++n) new_index[first[n]] = static_cast<int32_t>(n);
        std::vector<Row> reordered;
        reordered.reserve(rows_.size());
        for (size_t old : first) {
            Row row = rows_[old];
            if (row.parent_row >= 0) row.parent_row = new_index[static_cast<size_t>(row.parent_row)];
            reordered.push_back(std::move(row));
        }
        rows_ = std::move(reordered);
    }

    bool is_protected(const Row &r) const {
        if (r.is_group()) {
            for (const pc_process &p : host_->store().snapshot().processes)
                if (p.app_id == r.group_id && !(p.flags & PC_PROC_PROTECTED)) return false;
            return true;
        }
        const pc_process *p = process_of(r);
        return !p || (p->flags & PC_PROC_PROTECTED);
    }

    bool collapsed(const Row &r) const {
        const std::string key = key_of(r);
        if (all_collapsed_) return !collapsed_.count(key);  // the set lists the expanded ones
        return collapsed_.count(key) != 0;
    }

    void toggle(int visible_index) {
        const std::string key = key_of(row(visible_index));
        if (collapsed_.count(key))
            collapsed_.erase(key);
        else
            collapsed_.insert(key);
        rebuild();
    }

    void build_columns(Host &host) {
        Store &store = host.store();
        table_.columns = {{PC_COLUMN_NAME, L"Name", 250, 1, HAlign::Left, true, false},
                          {PC_COLUMN_PID, L"PID", 64, 0, HAlign::Left, true, false},
                          {PC_COLUMN_USER, L"User", 72, 0, HAlign::Left, true, false},
                          {PC_COLUMN_CPU, L"CPU", 70, 0, HAlign::Right},
                          {PC_COLUMN_MEMORY, L"Memory", 82, 0, HAlign::Right}};
        if (store.has(PC_CAP_PROCESS_GPU)) table_.columns.push_back({PC_COLUMN_GPU, L"GPU", 64, 0, HAlign::Right});
        if (store.has(PC_CAP_PROCESS_ENERGY))
            table_.columns.push_back({PC_COLUMN_POWER, L"Power", 70, 0, HAlign::Right});
        table_.columns.push_back({PC_COLUMN_DISK_READ, L"Disk Read", 78, 0, HAlign::Right});
        table_.columns.push_back({PC_COLUMN_DISK_WRITE, L"Disk Write", 78, 0, HAlign::Right});
        if (store.has(PC_CAP_PROCESS_NETWORK)) {
            table_.columns.push_back({PC_COLUMN_NET_RX, L"Net ↓", 78, 0, HAlign::Right});
            table_.columns.push_back({PC_COLUMN_NET_TX, L"Net ↑", 78, 0, HAlign::Right});
        }
        table_.columns.push_back({PC_COLUMN_THREADS, L"Threads", 64, 0, HAlign::Right});
    }

    void rebuild() {
        if (!host_) return;
        query_.sort_column = table_.sort_column;
        query_.descending = table_.sort_descending;
        rows_ = host_->store().build_view(query_);
        pin_rows();
        visible_.clear();
        std::vector<char> hidden(rows_.size(), 0);
        for (size_t i = 0; i < rows_.size(); ++i) {
            const Row &r = rows_[i];
            if (r.parent_row >= 0 &&
                (hidden[static_cast<size_t>(r.parent_row)] || collapsed(rows_[static_cast<size_t>(r.parent_row)]))) {
                hidden[i] = 1;
                continue;
            }
            visible_.push_back(i);
        }
        int selected = -1;
        if (!selected_key_.empty())
            for (size_t i = 0; i < visible_.size(); ++i)
                if (key_of(row(static_cast<int>(i))) == selected_key_) selected = static_cast<int>(i);
        table_.selected = selected;
    }

    Cell cell(int visible_index, int column) {
        const Row &r = row(visible_index);
        const pc_process *p = process_of(r);
        const Theme &theme = host_->renderer().theme();
        const Snapshot &s = host_->store().snapshot();
        Cell c;
        const bool restricted = p && (p->flags & PC_PROC_RESTRICTED);
        switch (column) {
            case PC_COLUMN_NAME:
                if (r.is_group()) {
                    c.text = r.group_name;
                    c.bold = true;
                    c.count = r.process_count;
                } else if (p) {
                    c.text = process_display_name(*p);
                    c.lock = restricted;
                    c.paused = p->state == PC_STATE_STOPPED;
                    if (p->flags & PC_PROC_PROTECTED) c.color = theme.text_secondary();
                }
                c.pinned = pinned(r);
                break;
            case PC_COLUMN_PID:
                c.text = r.is_group() ? std::to_wstring(r.group_pid) : p ? std::to_wstring(p->pid) : L"";
                c.mono = true;
                c.color = theme.text_secondary();
                break;
            case PC_COLUMN_USER:
                if (p) {
                    c.text = p->user[0] ? fmt::from_utf8(p->user) : std::wstring(fmt::unavailable);
                    c.dim = !p->user[0];
                }
                c.color = theme.text_secondary();
                break;
            case PC_COLUMN_CPU:
                c.text = fmt::cpu(r.cpu_percent);
                c.dim = r.cpu_percent < 0;
                c.heat = r.cpu_percent > 0 ? static_cast<float>(r.cpu_percent / 100.0) : 0;
                c.heat_kind = MetricKind::Cpu;
                break;
            case PC_COLUMN_MEMORY:
                c.text = fmt::bytes(r.memory_bytes);
                c.dim = r.memory_bytes < 0;
                c.heat =
                    r.memory_bytes > 0 && s.memory_total ? static_cast<float>(r.memory_bytes) / s.memory_total * 6 : 0;
                c.heat_kind = MetricKind::Memory;
                break;
            case PC_COLUMN_DISK_READ:
            case PC_COLUMN_DISK_WRITE: {
                const double v = column == PC_COLUMN_DISK_READ ? r.disk_read_bps : r.disk_write_bps;
                c.text = fmt::rate(v);
                c.dim = v < 0;
                c.heat = v > 0 ? static_cast<float>(v / 20'000'000.0) : 0;
                c.heat_kind = MetricKind::Disk;
                break;
            }
            case PC_COLUMN_NET_RX:
            case PC_COLUMN_NET_TX: {
                const double v = column == PC_COLUMN_NET_RX ? r.net_rx_bps : r.net_tx_bps;
                c.text = fmt::rate(v);
                c.dim = v < 0;
                c.heat = v > 0 ? static_cast<float>(v / 10'000'000.0) : 0;
                c.heat_kind = MetricKind::Network;
                break;
            }
            case PC_COLUMN_THREADS:
                c.text = fmt::count(r.threads);
                c.dim = r.threads < 0;
                c.color = theme.text_secondary();
                break;
            case PC_COLUMN_GPU:
                c.text = fmt::cpu(r.gpu_percent);
                c.dim = r.gpu_percent < 0;
                c.heat = r.gpu_percent > 0 ? static_cast<float>(r.gpu_percent / 100.0) : 0;
                c.heat_kind = MetricKind::Gpu;
                break;
            case PC_COLUMN_POWER:
                c.text = fmt::watts(r.power_watts);
                c.dim = r.power_watts < 0;
                c.heat = r.power_watts > 0 ? static_cast<float>(r.power_watts / 5) : 0;
                c.heat_kind = MetricKind::Energy;
                break;
            default: break;
        }
        return c;
    }

    std::wstring explain(const pc_process &p) const {
        std::wstring text;
        if (p.flags & PC_PROC_SYSTEM)
            text += L"This is a Windows system process. Ending it may make Windows unstable or sign you out.";
        if (p.path[0]) text += (text.empty() ? L"" : L"\n\n") + std::wstring(L"Path: ") + fmt::from_utf8(p.path);
        return text;
    }

    void end(const Row &r, bool force, bool tree) {
        if (!host_) return;
        Store &store = host_->store();
        const Snapshot &s = store.snapshot();
        if (is_protected(r)) {
            host_->alert(L"Can't end this process", L"It is critical to Windows (or it is Procyon itself).");
            return;
        }
        std::vector<int32_t> pids;
        std::wstring name;
        bool system = false;
        if (r.is_group()) {
            name = r.group_name;
            for (const pc_process &p : s.processes)
                if (p.app_id == r.group_id && !(p.flags & PC_PROC_PROTECTED)) {
                    pids.push_back(p.pid);
                    system |= (p.flags & PC_PROC_SYSTEM) != 0;
                }
        } else if (const pc_process *p = process_of(r)) {
            name = process_display_name(*p);
            pids.push_back(p->pid);
            system = (p->flags & PC_PROC_SYSTEM) != 0;
        }
        if (pids.empty()) return;
        const bool ask = force || tree || r.is_group() || system;
        if (ask) {
            std::wstring title = (tree ? L"End the process tree of " : force ? L"Force quit " : L"End ") + name + L"?";
            std::wstring message =
                tree    ? L"The process and every process it started will be ended immediately. Unsaved work is lost."
                : force ? L"The process will be ended immediately without a chance to save."
                : r.is_group()
                    ? L"All " + std::to_wstring(pids.size()) + L" processes of this app will be asked to close."
                    : L"";
            if (!r.is_group()) {
                const pc_process *p = process_of(r);
                if (p) {
                    const std::wstring why = explain(*p);
                    if (!why.empty()) message += (message.empty() ? L"" : L"\n\n") + why;
                }
            }
            if (!host_->confirm(title, message, tree ? L"End Tree" : force ? L"Force Quit" : L"End Task", true)) return;
        }
        pc_result worst = PC_OK;
        for (int32_t pid : pids) {
            const pc_result result = tree ? store.end_tree(pid) : store.end_process(pid, force);
            if (result != PC_OK && (worst == PC_OK || result == PC_ERR_PERMISSION)) worst = result;
        }
        if (worst != PC_OK) host_->report(worst, L"end " + name);
        store.refresh_now();
    }

    void context(int visible_index, float x, float y) {
        if (!host_) return;
        const Row r = row(visible_index);
        const pc_process *p = process_of(r);
        const bool prot = is_protected(r);
        std::vector<MenuItem> items;
        items.push_back({MenuEnd, L"End Task", !prot, false, false, true});
        items.push_back({MenuForce, L"Force Quit", !prot, false, false, true});
        items.push_back({MenuTree, L"End Process Tree", !prot && !r.is_group(), false, false, true});
        items.push_back({0, L"", true, false, true});
        if (host_->store().has(PC_CAP_SUSPEND) && !r.is_group()) {
            const bool suspended = p && p->state == PC_STATE_STOPPED;
            items.push_back({suspended ? MenuResume : MenuSuspend, suspended ? L"Resume" : L"Suspend", !prot});
        }
        if (host_->store().has(PC_CAP_PRIORITY) && !r.is_group()) {
            MenuItem priority{0, L"Priority", !prot};
            for (size_t i = 0; i < std::size(kPriorities); ++i)
                priority.children.push_back({static_cast<int>(MenuPriorityBase + i), kPriorities[i].label, true,
                                             p && p->nice == kPriorities[i].nice});
            items.push_back(priority);
        }
        items.push_back({0, L"", true, false, true});
        items.push_back({MenuPin, app_of(r) == pinned_app_ ? L"Unpin" : L"Pin to Top", !app_of(r).empty()});
        items.push_back({MenuInfo, L"Get Info"});
        items.push_back({MenuOpenLocation, L"Open File Location", p && p->path[0] != 0});
        MenuItem copy{0, L"Copy"};
        copy.children = {{MenuCopyName, L"Name"}, {MenuCopyPath, L"Path", p && p->path[0] != 0}, {MenuCopyPid, L"PID"}};
        items.push_back(copy);
        const int chosen = host_->popup_menu(items, x, y);
        const int32_t pid = pid_of(r);
        switch (chosen) {
            case MenuEnd: end(r, false, false); break;
            case MenuForce: end(r, true, false); break;
            case MenuTree: end(r, true, true); break;
            case MenuSuspend:
                if (host_->confirm(L"Suspend " + (p ? process_display_name(*p) : L"process") + L"?",
                                   L"The process stops running until you resume it. Apps may become unresponsive.",
                                   L"Suspend", false)) {
                    const pc_result result = host_->store().suspend(pid);
                    if (result != PC_OK) host_->report(result, L"suspend the process");
                    host_->store().refresh_now();
                }
                break;
            case MenuResume: {
                const pc_result result = host_->store().resume(pid);
                if (result != PC_OK) host_->report(result, L"resume the process");
                host_->store().refresh_now();
                break;
            }
            case MenuInfo: host_->show_info(pid); break;
            case MenuPin: toggle_pin(r); break;
            case MenuOpenLocation:
                if (p) host_->open_in_explorer(fmt::from_utf8(p->path));
                break;
            case MenuCopyName:
                host_->copy_to_clipboard(r.is_group() ? r.group_name : p ? process_display_name(*p) : L"");
                break;
            case MenuCopyPath:
                if (p) host_->copy_to_clipboard(fmt::from_utf8(p->path));
                break;
            case MenuCopyPid: host_->copy_to_clipboard(std::to_wstring(pid)); break;
            default:
                if (chosen >= MenuPriorityBase &&
                    chosen < MenuPriorityBase + static_cast<int>(std::size(kPriorities))) {
                    const pc_result result =
                        host_->store().set_priority(pid, kPriorities[chosen - MenuPriorityBase].nice);
                    if (result != PC_OK) host_->report(result, L"change the priority");
                    host_->store().refresh_now();
                }
                break;
        }
    }

    Host *host_ = nullptr;
    Table table_;
    TextField search_;
    ViewQuery query_;
    std::vector<Row> rows_;
    std::vector<size_t> visible_;
    std::unordered_set<std::string> collapsed_;
    bool all_collapsed_ = false;
    std::string selected_key_;
    std::string pinned_app_;  // app id kept at the top of every view, empty for none
    bool activated_ = false;
    std::vector<Rect> mode_rects_;
    Rect end_button_, info_button_, expand_button_, banner_button_;
};

}  // namespace

std::unique_ptr<Page> make_processes_page() { return std::make_unique<ProcessesPage>(); }

}  // namespace procyon::ui
