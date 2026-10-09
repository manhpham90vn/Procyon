// The main window of the Linux app: a GTK 4 window whose one drawing area shows the shared UI
// (sidebar, pages, overlays) through the Cairo canvas. Like the Windows app it draws its own caption:
// the title bar is gone, a GtkWindowHandle over the top strip drags the window, and the desktop's
// own window buttons (GtkWindowControls) sit at its right end.
#include "window.hpp"

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <malloc.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

#include "canvas_cairo.hpp"
#include "commands.hpp"
#include "overlays.hpp"
#include "pages.hpp"
#include "platform.hpp"
#include "platform_linux.hpp"
#include "tray_linux.hpp"
#include "version.h"

namespace procyon::ui {
namespace {

constexpr const char *kAppId = "dev.procyon.Procyon";
constexpr float kCaptionHeight = 32;
constexpr float kSidebarWidth = 240;
// The sidebar's brand block, above its first row: a drag handle like the macOS sidebar's top.
constexpr float kSidebarHandleHeight = 52;

struct NavItem {
    enum class Kind { Page, Metric, Section };
    Kind kind = Kind::Page;
    PageId page = PageId::Overview;
    std::wstring label;
    Renderer::Symbol symbol = Renderer::Symbol::Grid;
    MetricKind metric = MetricKind::Cpu;
    Rect rect;
};

const char *page_name(PageId id) {
    switch (id) {
        case PageId::Overview: return "overview";
        case PageId::Processes: return "processes";
        case PageId::Cpu: return "cpu";
        case PageId::Memory: return "memory";
        case PageId::Disk: return "disk";
        case PageId::Network: return "network";
        case PageId::Gpu: return "gpu";
        case PageId::Battery: return "battery";
        case PageId::Startup: return "startup";
        case PageId::Services: return "services";
        case PageId::History: return "history";
        case PageId::Inspect: return "inspect";
        case PageId::System: return "system";
        case PageId::Settings: return "settings";
    }
    return "overview";
}

std::optional<PageId> page_from_name(const std::string &name) {
    for (PageId id : {PageId::Overview, PageId::Processes, PageId::Cpu, PageId::Memory, PageId::Disk, PageId::Network,
                      PageId::Gpu, PageId::Battery, PageId::Startup, PageId::Services, PageId::History, PageId::Inspect,
                      PageId::System, PageId::Settings})
        if (name == page_name(id)) return id;
    return std::nullopt;
}

// Runs a nested main loop until `done` is set: GTK 4 dialogs and menus are asynchronous, the Host
// interface the pages use is not.
void wait_for(const bool &done) {
    GMainContext *context = g_main_context_default();
    while (!done) g_main_context_iteration(context, TRUE);
}

class MainWindow : public Host {
public:
    // Offscreen rendering for screenshots (--screenshot): the window is never shown; after a few
    // samples the page is drawn into a PNG and the app quits.
    struct Screenshot {
        std::string path;
        int theme = 0;  // Settings::theme: 0 system, 1 light, 2 dark
        float scale = 2;
        int width = 1280, height = 800;
    };

    MainWindow(GtkApplication *app, std::optional<PageId> initial, bool persist, std::optional<Screenshot> screenshot,
               bool background = false)
        : app_(app),
          initial_page_(initial),
          persist_(persist),
          screenshot_(std::move(screenshot)),
          start_in_background_(background) {}

    ~MainWindow() override { *alive_ = false; }

    void create() {
        load_settings();
        window_ = gtk_application_window_new(app_);
        gtk_window_set_title(GTK_WINDOW(window_), "Procyon");
        gtk_window_set_icon_name(GTK_WINDOW(window_), kAppId);
        gtk_window_set_default_size(GTK_WINDOW(window_), width_, height_);
        if (maximized_) gtk_window_maximize(GTK_WINDOW(window_));
        // No title bar: an empty widget in its place keeps the client-side frame (shadow, resize
        // edges, rounded corners) without the header.
        GtkWidget *no_titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_visible(no_titlebar, FALSE);
        gtk_window_set_titlebar(GTK_WINDOW(window_), no_titlebar);

        area_ = gtk_drawing_area_new();
        gtk_widget_set_size_request(area_, 900, 600);
        gtk_widget_set_focusable(area_, TRUE);
        gtk_drawing_area_set_draw_func(
            GTK_DRAWING_AREA(area_),
            [](GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer self) {
                static_cast<MainWindow *>(self)->paint(cr, w, h);
            },
            this, nullptr);

        GtkWidget *overlay = gtk_overlay_new();
        gtk_overlay_set_child(GTK_OVERLAY(overlay), area_);
        // Drag handles: the strip above the page (with the window buttons) and the sidebar's top.
        GtkWidget *caption = gtk_window_handle_new();
        gtk_widget_set_valign(caption, GTK_ALIGN_START);
        gtk_widget_set_halign(caption, GTK_ALIGN_FILL);
        gtk_widget_set_margin_start(caption, static_cast<int>(kSidebarWidth));
        gtk_widget_set_size_request(caption, -1, static_cast<int>(kCaptionHeight));
        GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(strip), spacer);
        GtkWidget *controls = gtk_window_controls_new(GTK_PACK_END);
        gtk_widget_set_valign(controls, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_end(controls, 6);
        gtk_box_append(GTK_BOX(strip), controls);
        gtk_window_handle_set_child(GTK_WINDOW_HANDLE(caption), strip);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), caption);
        GtkWidget *brand = gtk_window_handle_new();
        gtk_widget_set_valign(brand, GTK_ALIGN_START);
        gtk_widget_set_halign(brand, GTK_ALIGN_START);
        gtk_widget_set_size_request(brand, static_cast<int>(kSidebarWidth), static_cast<int>(kSidebarHandleHeight));
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), brand);
        gtk_window_set_child(GTK_WINDOW(window_), overlay);

        connect_input();
        g_signal_connect(window_, "close-request", G_CALLBACK(+[](GtkWindow *, gpointer self) -> gboolean {
                             return static_cast<MainWindow *>(self)->on_close_request();
                         }),
                         this);

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
        // The sampler thread hands each snapshot to the main loop.
        auto alive = alive_;
        store_.start([this, alive](Snapshot *snapshot) {
            struct Delivery {
                MainWindow *window;
                std::shared_ptr<bool> alive;
                Snapshot *snapshot;
            };
            g_main_context_invoke_full(
                nullptr, G_PRIORITY_DEFAULT,
                [](gpointer data) -> gboolean {
                    auto *d = static_cast<Delivery *>(data);
                    if (*d->alive)
                        d->window->on_snapshot(d->snapshot);
                    else
                        delete d->snapshot;
                    return G_SOURCE_REMOVE;
                },
                new Delivery{this, alive, snapshot}, [](gpointer data) { delete static_cast<Delivery *>(data); });
            return true;
        });
        navigate(initial_page_.value_or(PageId::Overview));
        if (!screenshot_) make_tray();
        if (!screenshot_) listen_for_handover();
        // A "System" appearance follows the desktop at once, also while paused and idle.
        if (!screenshot_) platform::watch_color_scheme([this] { repaint(); });
        if (screenshot_) {
            settings_.theme = screenshot_->theme;
            g_application_hold(G_APPLICATION(app_));
            held_ = true;
            return;
        }
        // --background: running without a window from the start, as when it was closed (the macOS app's
        // -launchInMenuBar); measured by scripts/bench-linux.py.
        if (start_in_background_) {
            hide_window();
            return;
        }
        gtk_window_present(GTK_WINDOW(window_));
        gtk_widget_grab_focus(area_);
    }

    void make_tray() {
        tray_ = std::make_unique<Tray>(
            Tray::Callbacks{[this] { hidden_ ? present() : gtk_window_present(GTK_WINDOW(window_)); },
                            [this] { set_paused(!store_.paused()); },
                            [this] {
                                present();
                                navigate(PageId::Settings);
                            },
                            [this] { quit(); }});
        if (store_.paused()) tray_->set_paused(true);
    }

    bool failed() const { return failed_; }

    // A second launch while Procyon runs in the background brings the window back. While the root
    // copy runs in this one's place, that copy's window is the one to use: this one stays hidden and
    // asks the root copy to show its window instead.
    void present() {
        if (handed_over_) {
            if (handover_pipe_ >= 0 && ::write(handover_pipe_, "present\n", 8) < 0) close_handover_pipe();
            return;
        }
        if (hidden_) show_again();
        gtk_window_present(GTK_WINDOW(window_));
    }

    void shutdown() {
        if (!window_) return;
        // After the root copy took over, its settings and history are the current ones: this copy
        // writes neither.
        if (!handed_over_) save_settings();
        store_.stop();
        if (!handed_over_) store_.flush_history();  // the minute in progress would otherwise go with the process
        close_handover_pipe();
        if (handover_watch_) g_source_remove(handover_watch_);
        handover_watch_ = 0;
        if (held_) g_application_release(G_APPLICATION(app_));
        held_ = false;
        tray_.reset();
        GtkWidget *window = window_;
        window_ = nullptr;
        area_ = nullptr;
        gtk_window_destroy(GTK_WINDOW(window));
    }

    // ---- Host ----
    Store &store() override { return store_; }
    Renderer &renderer() override { return renderer_; }
    Settings &settings() override { return settings_; }
    void repaint() override {
        if (area_ && !hidden_) gtk_widget_queue_draw(area_);
    }
    void request_frame() override {
        if (frame_timer_) return;
        frame_timer_ = g_timeout_add(
            16,
            [](gpointer self) -> gboolean {
                auto *w = static_cast<MainWindow *>(self);
                w->frame_timer_ = 0;
                w->repaint();
                return G_SOURCE_REMOVE;
            },
            this);
    }
    double now() override {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
    float mouse_x() override { return overlays_.empty() ? mouse_x_ : -1; }
    float mouse_y() override { return overlays_.empty() ? mouse_y_ : -1; }

    void navigate(PageId id) override {
        // A screen this machine doesn't have (a shortcut, --page): stay, or start on Overview.
        if (!page_available(store_, id)) {
            if (current_) return;
            id = PageId::Overview;
        }
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
        GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", fmt::to_utf8(title).c_str());
        if (!message.empty()) gtk_alert_dialog_set_detail(dialog, fmt::to_utf8(message).c_str());
        const std::string action_label = fmt::to_utf8(action);
        const char *buttons[] = {"Cancel", action_label.c_str(), nullptr};
        gtk_alert_dialog_set_buttons(dialog, buttons);
        gtk_alert_dialog_set_cancel_button(dialog, 0);
        gtk_alert_dialog_set_default_button(dialog, destructive ? 0 : 1);
        const int chosen = choose(dialog);
        g_object_unref(dialog);
        return chosen == 1;
    }

    void alert(const std::wstring &title, const std::wstring &message) override {
        GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", fmt::to_utf8(title).c_str());
        if (!message.empty()) gtk_alert_dialog_set_detail(dialog, fmt::to_utf8(message).c_str());
        choose(dialog);
        g_object_unref(dialog);
    }

    void report(pc_result result, const std::wstring &action) override {
        if (result == PC_OK) return;
        const std::wstring title = L"Couldn't " + action;
        const std::wstring message = fmt::from_utf8(pc_result_message(result));
        if (result == PC_ERR_PERMISSION && !elevated() && can_elevate()) {
            if (confirm(title, message + L"\n\nRun Procyon as root to act on every process.", L"Run as root", false))
                relaunch_elevated();
            return;
        }
        alert(title, message);
    }

    int popup_menu(const std::vector<MenuItem> &items, float x, float y) override {
        GSimpleActionGroup *group = g_simple_action_group_new();
        struct State {
            int chosen = 0;
            bool done = false;
        } state;
        GMenu *menu = build_menu(items, group, &state);
        GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
        gtk_widget_insert_action_group(area_, "pm", G_ACTION_GROUP(group));
        gtk_widget_set_parent(popover, area_);
        gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
        gtk_widget_set_halign(popover, GTK_ALIGN_START);
        const GdkRectangle at{static_cast<int>(x), static_cast<int>(y), 1, 1};
        gtk_popover_set_pointing_to(GTK_POPOVER(popover), &at);
        gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
        // An item's action runs before or after "closed" depending on how it was chosen: finish
        // on the next idle so both orders end with the choice known.
        g_signal_connect(popover, "closed", G_CALLBACK(+[](GtkPopover *, gpointer data) {
                             g_idle_add(
                                 [](gpointer d) -> gboolean {
                                     static_cast<State *>(d)->done = true;
                                     return G_SOURCE_REMOVE;
                                 },
                                 data);
                         }),
                         &state);
        gtk_popover_popup(GTK_POPOVER(popover));
        wait_for(state.done);
        gtk_widget_unparent(popover);
        gtk_widget_insert_action_group(area_, "pm", nullptr);
        g_object_unref(menu);
        g_object_unref(group);
        return state.chosen;
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

    bool elevated() override { return ::geteuid() == 0; }

    // Full access: the same program as root through polkit's pkexec, on the same display. The
    // window hides meanwhile and comes back when the elevated copy ends or authorization fails.
    void relaunch_elevated() override {
        if (elevated() || !can_elevate()) return;
        char exe[4096];
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) return;
        exe[n] = '\0';
        std::vector<std::string> args{"pkexec", "env"};
        // The display, and the user's own config and data folders: the root copy reads the same
        // settings and keeps the same history (pkexec resets HOME; PKEXEC_UID names the user).
        for (const char *name : {"DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "XDG_CURRENT_DESKTOP",
                                 "XDG_CONFIG_HOME", "XDG_DATA_HOME"})
            if (const char *value = g_getenv(name)) args.push_back(std::string(name) + "=" + value);
        // The session bus, for the tray icon and notifications. Spelled out: GLib takes $XDG_RUNTIME_DIR/bus
        // only when the socket is its own user's, which for root it isn't.
        if (const char *bus = g_getenv("DBUS_SESSION_BUS_ADDRESS"))
            args.push_back(std::string("DBUS_SESSION_BUS_ADDRESS=") + bus);
        else if (const char *runtime = g_getenv("XDG_RUNTIME_DIR"))
            args.push_back(std::string("DBUS_SESSION_BUS_ADDRESS=unix:path=") + runtime + "/bus");
        // The desktop's light or dark preference as this copy reads it. The root copy can't: dconf finds
        // the user's database through HOME, which pkexec sets to root's, so it would read no dark
        // preference and turn a "System" appearance light.
        args.push_back(std::string("PROCYON_DESKTOP_DARK=") + (platform::system_prefers_dark() ? "1" : "0"));
        // The root copy reads its stdin for requests from this one (see listen_for_handover).
        args.emplace_back("PROCYON_HANDOVER=1");
        args.emplace_back(exe);
        args.emplace_back("--page");
        args.emplace_back(current_ ? page_name(current_->id()) : "overview");
        std::vector<char *> argv;
        for (auto &a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        save_settings();
        store_.flush_history();
        GPid child = 0;
        GError *error = nullptr;
        // The root copy's stdin is a pipe from this copy: the session's single instance, the one a second
        // launch reaches, passes "show the window" on through it (pkexec keeps stdin, closes other fds).
        int pipe = -1;
        if (!g_spawn_async_with_pipes(nullptr, argv.data(), nullptr,
                                      static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD),
                                      nullptr, nullptr, &child, &pipe, nullptr, nullptr, &error)) {
            alert(L"Couldn't run Procyon as root", fmt::from_utf8(error ? error->message : ""));
            if (error) g_error_free(error);
            return;
        }
        // Hand over to the root copy: one tray icon, one sampler, one writer of the settings and the
        // history. This copy waits hidden, without updates, until the root copy ends.
        handed_over_ = true;
        handover_pipe_ = pipe;
        g_unix_set_fd_nonblocking(handover_pipe_, TRUE, nullptr);
        ::signal(SIGPIPE, SIG_IGN);  // a write after the root copy ended fails with EPIPE instead
        paused_before_handover_ = store_.paused();
        store_.set_paused(true);
        store_.set_records_history(false);
        tray_.reset();
        hide_window();
        handover_started_ = g_get_monotonic_time();
        g_child_watch_add(
            child,
            [](GPid pid, gint status, gpointer self) {
                g_spawn_close_pid(pid);
                auto *w = static_cast<MainWindow *>(self);
                // 126: the authorization dialog was dismissed; 127: not authorized. A root copy that
                // fails within seconds of starting didn't get to show a window either.
                const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                const bool early = g_get_monotonic_time() - w->handover_started_ < 5 * G_USEC_PER_SEC;
                if (code == 126 || code == 127 || (code != 0 && early)) {
                    w->take_back();
                    if (code != 126 && code != 127)
                        w->alert(L"Couldn't run Procyon as root", L"The root copy stopped as it started (exit status " +
                                                                      std::to_wstring(code) +
                                                                      L"). Procyon keeps running as your user.");
                } else {
                    w->quit();
                }
            },
            this);
    }

    void copy_to_clipboard(const std::wstring &text) override {
        gdk_clipboard_set_text(gtk_widget_get_clipboard(area_), fmt::to_utf8(text).c_str());
    }

    void open_in_explorer(const std::wstring &path) override {
        if (path.empty()) return;
        GFile *file = g_file_new_for_path(fmt::to_utf8(path).c_str());
        GtkFileLauncher *launcher = gtk_file_launcher_new(file);
        gtk_file_launcher_open_containing_folder(launcher, GTK_WINDOW(window_), nullptr, nullptr, nullptr);
        g_object_unref(launcher);
        g_object_unref(file);
    }

    void open_url(const std::wstring &url) override {
        if (url.empty()) return;
        GtkUriLauncher *launcher = gtk_uri_launcher_new(fmt::to_utf8(url).c_str());
        gtk_uri_launcher_launch(launcher, GTK_WINDOW(window_), nullptr, nullptr, nullptr);
        g_object_unref(launcher);
    }

    std::wstring pick_folder(const std::wstring &title) override {
        GtkFileDialog *dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, fmt::to_utf8(title).c_str());
        struct Pick {
            bool done = false;
            std::string path;
        } pick;
        gtk_file_dialog_select_folder(
            dialog, GTK_WINDOW(window_), nullptr,
            [](GObject *source, GAsyncResult *result, gpointer data) {
                auto *p = static_cast<Pick *>(data);
                if (GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, nullptr)) {
                    if (char *path = g_file_get_path(file)) {
                        p->path = path;
                        g_free(path);
                    }
                    g_object_unref(file);
                }
                p->done = true;
            },
            &pick);
        wait_for(pick.done);
        g_object_unref(dialog);
        return fmt::from_utf8(pick.path);
    }

    void set_paused(bool paused) override {
        store_.set_paused(paused);
        if (tray_) tray_->set_paused(paused);
        update_tray();
        repaint();
    }
    bool paused() override { return store_.paused(); }

private:
    // ---- dialogs and menus ----

    int choose(GtkAlertDialog *dialog) {
        struct Choice {
            bool done = false;
            int button = -1;
        } choice;
        gtk_alert_dialog_choose(
            dialog, GTK_WINDOW(window_), nullptr,
            [](GObject *source, GAsyncResult *result, gpointer data) {
                auto *c = static_cast<Choice *>(data);
                c->button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, nullptr);
                c->done = true;
            },
            &choice);
        wait_for(choice.done);
        return choice.button;
    }

    template <typename State>
    GMenu *build_menu(const std::vector<MenuItem> &items, GSimpleActionGroup *group, State *state) {
        GMenu *menu = g_menu_new();
        GMenu *section = g_menu_new();
        auto flush = [&] {
            if (g_menu_model_get_n_items(G_MENU_MODEL(section)) > 0)
                g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
            g_object_unref(section);
            section = g_menu_new();
        };
        for (const MenuItem &item : items) {
            if (item.separator) {
                flush();
                continue;
            }
            const std::string label = fmt::to_utf8(item.label);
            if (!item.children.empty()) {
                GMenu *sub = build_menu(item.children, group, state);
                g_menu_append_submenu(section, label.c_str(), G_MENU_MODEL(sub));
                g_object_unref(sub);
                continue;
            }
            const std::string name = "i" + std::to_string(item.id) + "_" + std::to_string(next_action_++);
            // A checked item is a boolean-state action: GTK draws the check mark itself.
            GSimpleAction *action =
                item.checked ? g_simple_action_new_stateful(name.c_str(), nullptr, g_variant_new_boolean(TRUE))
                             : g_simple_action_new(name.c_str(), nullptr);
            g_simple_action_set_enabled(action, item.enabled);
            g_object_set_data(G_OBJECT(action), "procyon-id", GINT_TO_POINTER(item.id));
            g_signal_connect(action, "activate", G_CALLBACK(+[](GSimpleAction *a, GVariant *, gpointer data) {
                                 static_cast<State *>(data)->chosen =
                                     GPOINTER_TO_INT(g_object_get_data(G_OBJECT(a), "procyon-id"));
                             }),
                             state);
            g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
            g_object_unref(action);
            g_menu_append(section, label.c_str(), ("pm." + name).c_str());
        }
        flush();
        g_object_unref(section);
        return menu;
    }

    // ---- input ----

    void connect_input() {
        GtkEventController *motion = gtk_event_controller_motion_new();
        g_signal_connect(motion, "motion",
                         G_CALLBACK(+[](GtkEventControllerMotion *c, double x, double y, gpointer self) {
                             static_cast<MainWindow *>(self)->on_motion(GTK_EVENT_CONTROLLER(c), x, y);
                         }),
                         this);
        g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion *, gpointer self) {
                             static_cast<MainWindow *>(self)->on_leave();
                         }),
                         this);
        gtk_widget_add_controller(area_, motion);

        GtkGesture *click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
        g_signal_connect(click, "pressed",
                         G_CALLBACK(+[](GtkGestureClick *g, int n, double x, double y, gpointer self) {
                             static_cast<MainWindow *>(self)->on_pressed(GTK_GESTURE(g), n, x, y);
                         }),
                         this);
        g_signal_connect(click, "released", G_CALLBACK(+[](GtkGestureClick *g, int, double x, double y, gpointer self) {
                             static_cast<MainWindow *>(self)->on_released(GTK_GESTURE(g), x, y);
                         }),
                         this);
        gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(click));

        GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
        g_signal_connect(scroll, "scroll",
                         G_CALLBACK(+[](GtkEventControllerScroll *c, double dx, double dy, gpointer self) -> gboolean {
                             return static_cast<MainWindow *>(self)->on_scroll(c, dx, dy);
                         }),
                         this);
        gtk_widget_add_controller(area_, scroll);

        GtkEventController *keys = gtk_event_controller_key_new();
        g_signal_connect(keys, "key-pressed",
                         G_CALLBACK(+[](GtkEventControllerKey *, guint keyval, guint keycode, GdkModifierType state,
                                        gpointer self) -> gboolean {
                             return static_cast<MainWindow *>(self)->on_key(keyval, keycode, state);
                         }),
                         this);
        gtk_widget_add_controller(area_, keys);
    }

    MouseEvent mouse_event(GtkEventController *controller, double x, double y, int wheel = 0) const {
        MouseEvent e;
        e.x = static_cast<float>(x);
        e.y = static_cast<float>(y);
        const GdkModifierType state = gtk_event_controller_get_current_event_state(controller);
        e.ctrl = (state & GDK_CONTROL_MASK) != 0;
        e.shift = (state & GDK_SHIFT_MASK) != 0;
        e.wheel = wheel;
        return e;
    }

    void on_motion(GtkEventController *controller, double x, double y) {
        const MouseEvent e = mouse_event(controller, x, y);
        if (e.x == mouse_x_ && e.y == mouse_y_) return;
        mouse_x_ = e.x;
        mouse_y_ = e.y;
        if (!overlays_.empty())
            overlays_.back()->mouse_move(*this, e);
        else if (current_)
            current_->mouse_move(*this, e);
        update_cursor();
        repaint();
    }

    void on_leave() {
        mouse_x_ = mouse_y_ = -1;
        if (current_) current_->mouse_leave(*this);
        repaint();
    }

    void on_pressed(GtkGesture *gesture, int n_press, double x, double y) {
        gtk_widget_grab_focus(area_);
        const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
        const MouseEvent e = mouse_event(GTK_EVENT_CONTROLLER(gesture), x, y);
        mouse_x_ = e.x;
        mouse_y_ = e.y;
        if (button != GDK_BUTTON_PRIMARY && button != GDK_BUTTON_SECONDARY) return;
        const bool right = button == GDK_BUTTON_SECONDARY;
        if (n_press == 2 && !right) {
            if (overlays_.empty() && current_) current_->double_click(*this, e);
            repaint();
            return;
        }
        if (!overlays_.empty()) {
            overlays_.back()->mouse_down(*this, e, right);
            prune_overlays();
        } else if (!right && sidebar_click(e)) {
            // handled
        } else if (current_) {
            current_->mouse_down(*this, e, right);
        }
        repaint();
    }

    void on_released(GtkGesture *gesture, double x, double y) {
        if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_PRIMARY) return;
        if (overlays_.empty() && current_) current_->mouse_up(*this, mouse_event(GTK_EVENT_CONTROLLER(gesture), x, y));
        repaint();
    }

    gboolean on_scroll(GtkEventControllerScroll *controller, double, double dy) {
        // Wheel notches come as whole steps, touchpads as pixels: both become 120 per notch, with
        // the remainder carried so slow two-finger scrolls still move.
        const bool pixels = gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_SURFACE;
        scroll_remainder_ += -dy * (pixels ? kWheelNotch / 42.0 : kWheelNotch);
        const int wheel = static_cast<int>(scroll_remainder_);
        if (wheel == 0) return TRUE;
        scroll_remainder_ -= wheel;
        const MouseEvent e = mouse_event(GTK_EVENT_CONTROLLER(controller), mouse_x_, mouse_y_, wheel);
        if (!overlays_.empty())
            overlays_.back()->wheel(*this, e);
        else if (sidebar_.contains(e.x, e.y))
            sidebar_scroll_.wheel(e.wheel, 42 * 2);
        else if (current_)
            current_->wheel(*this, e);
        repaint();
        return TRUE;
    }

    static Key translate_key(guint keyval) {
        switch (keyval) {
            case GDK_KEY_Escape: return Key::Escape;
            case GDK_KEY_BackSpace: return Key::Back;
            case GDK_KEY_Delete:
            case GDK_KEY_KP_Delete: return Key::Delete;
            case GDK_KEY_Return:
            case GDK_KEY_KP_Enter: return Key::Return;
            case GDK_KEY_space: return Key::Space;
            case GDK_KEY_Tab:
            case GDK_KEY_ISO_Left_Tab: return Key::Tab;
            case GDK_KEY_Left: return Key::Left;
            case GDK_KEY_Right: return Key::Right;
            case GDK_KEY_Up: return Key::Up;
            case GDK_KEY_Down: return Key::Down;
            case GDK_KEY_Home: return Key::Home;
            case GDK_KEY_End: return Key::End;
            case GDK_KEY_Page_Up: return Key::PageUp;
            case GDK_KEY_Page_Down: return Key::PageDown;
            case GDK_KEY_Menu: return Key::Menu;
            default: return Key::None;
        }
    }

    // The accelerator table of the Windows app (res/Procyon.rc), plus Ctrl+Q.
    int shortcut(guint keyval, guint keycode, bool ctrl, bool shift, bool alt) const {
        const guint key = gdk_keyval_to_lower(keyval);
        if (ctrl && !alt && !shift) {
            // Ctrl+1…9 by the key's place on the number row, like the Windows app's virtual keys: on
            // layouts whose digits need Shift (AZERTY) the unshifted keyval is "&", "é", ….
            // XKB keycodes 10-18 are the row's 1-9 keys whatever the layout.
            guint digit = key >= GDK_KEY_1 && key <= GDK_KEY_9 ? key - GDK_KEY_1 : 9;
            if (digit == 9 && keycode >= 10 && keycode <= 18) digit = keycode - 10;
            if (digit < 9) {
                static const int pages[] = {IDM_PAGE_OVERVIEW, IDM_PAGE_PROCESSES, IDM_PAGE_CPU,
                                            IDM_PAGE_MEMORY,   IDM_PAGE_GPU,       IDM_PAGE_DISK,
                                            IDM_PAGE_NETWORK,  IDM_PAGE_STARTUP,   IDM_PAGE_SERVICES};
                return pages[digit];
            }
            switch (key) {
                case GDK_KEY_k: return IDM_PALETTE;
                case GDK_KEY_f: return IDM_FIND;
                case GDK_KEY_i: return IDM_INFO;
                case GDK_KEY_comma: return IDM_PAGE_SETTINGS;
                case GDK_KEY_q: return IDM_QUIT;
                case GDK_KEY_BackSpace: return IDM_END_TASK;
                default: break;
            }
        }
        if (ctrl && shift && !alt && key == GDK_KEY_p) return IDM_PAUSE;
        if (ctrl && alt && key == GDK_KEY_BackSpace) return shift ? IDM_END_TREE : IDM_FORCE_QUIT;
        if (!ctrl && !alt && (key == GDK_KEY_Delete || key == GDK_KEY_KP_Delete))
            return shift ? IDM_FORCE_QUIT : IDM_END_TASK;
        if (!ctrl && !alt && !shift && key == GDK_KEY_F5) return IDM_REFRESH;
        return 0;
    }

    gboolean on_key(guint keyval, guint keycode, GdkModifierType state) {
        const bool ctrl = (state & GDK_CONTROL_MASK) != 0, shift = (state & GDK_SHIFT_MASK) != 0,
                   alt = (state & GDK_ALT_MASK) != 0;
        KeyEvent e;
        e.key = translate_key(keyval);
        const guint upper = gdk_keyval_to_upper(keyval);
        if ((upper >= GDK_KEY_A && upper <= GDK_KEY_Z) || (upper >= GDK_KEY_0 && upper <= GDK_KEY_9))
            e.ch = static_cast<wchar_t>(upper);
        e.ctrl = ctrl;
        e.shift = shift;
        e.alt = alt;

        // A search field keeps Delete, Backspace and Return for itself, also with Ctrl (Ctrl+Backspace
        // deletes a word there), as the macOS app turns End Task off while its search field has focus.
        const bool text_input = !overlays_.empty() || (current_ && current_->search_focused());
        const bool editing_key = e.key == Key::Delete || e.key == Key::Back || e.key == Key::Return;
        if (!(text_input && editing_key)) {
            if (const int command = shortcut(keyval, keycode, ctrl, shift, alt)) {
                on_command(command);
                repaint();
                return TRUE;
            }
        }
        bool handled = false;
        if (!overlays_.empty()) {
            handled = overlays_.back()->key(*this, e);
            prune_overlays();
        } else if (current_) {
            handled = current_->key(*this, e);
            if (!handled && e.key == Key::Escape) handled = true;
        }
        if (!handled && !ctrl && !alt) {
            const gunichar c = gdk_keyval_to_unicode(keyval);
            if (c >= 0x20 && c != 0x7F) {
                if (!overlays_.empty())
                    handled = overlays_.back()->character(*this, static_cast<wchar_t>(c));
                else if (current_)
                    handled = current_->character(*this, static_cast<wchar_t>(c));
            }
        }
        repaint();
        return handled ? TRUE : FALSE;
    }

    void update_cursor() {
        Cursor cursor = Cursor::Arrow;
        if (overlays_.empty() && current_ && content_.contains(mouse_x_, mouse_y_)) cursor = current_->cursor();
        if (overlays_.empty() && sidebar_.contains(mouse_x_, mouse_y_)) {
            for (const NavItem &item : nav_)
                if (item.kind != NavItem::Kind::Section && item.rect.contains(mouse_x_, mouse_y_))
                    cursor = Cursor::Hand;
            if (pause_button_.contains(mouse_x_, mouse_y_) || settings_button_.contains(mouse_x_, mouse_y_))
                cursor = Cursor::Hand;
        }
        if (cursor == cursor_) return;
        cursor_ = cursor;
        gtk_widget_set_cursor_from_name(area_, cursor == Cursor::Hand    ? "pointer"
                                               : cursor == Cursor::IBeam ? "text"
                                                                         : "default");
    }

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
                if (current_ && !current_->command(*this, IDM_INFO)) show_info(current_->selected_pid());
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
            case IDM_OPEN: present(); break;
            case IDM_QUIT: quit(); break;
            default: break;
        }
    }

    void open_palette() {
        // Ctrl+K again closes it, as ⌘K toggles the macOS palette.
        bool was_open = false;
        for (auto &overlay : overlays_)
            if (overlay->is_palette() && !overlay->closed()) {
                overlay->close();
                was_open = true;
            }
        if (was_open) {
            overlays_.erase(
                std::remove_if(overlays_.begin(), overlays_.end(), [](const auto &o) { return o->closed(); }),
                overlays_.end());
            repaint();
            return;
        }
        std::vector<PaletteItem> items;
        auto screen = [&](PageId id, const wchar_t *title, const wchar_t *shortcut) {
            if (!page_available(store_, id)) return;
            items.push_back({PaletteItem::Kind::Screen, title, L"", shortcut, [this, id] { navigate(id); }});
        };
        screen(PageId::Overview, L"Overview", L"Ctrl+1");
        screen(PageId::Processes, L"Processes", L"Ctrl+2");
        screen(PageId::Cpu, L"CPU", L"Ctrl+3");
        screen(PageId::Memory, L"Memory", L"Ctrl+4");
        screen(PageId::Gpu, L"GPU", L"Ctrl+5");
        screen(PageId::Disk, L"Disk", L"Ctrl+6");
        screen(PageId::Network, L"Network", L"Ctrl+7");
        screen(PageId::Startup, L"Startup", L"Ctrl+8");
        screen(PageId::Services, L"Services", L"Ctrl+9");
        screen(PageId::History, L"History", L"");
        screen(PageId::Inspect, L"Files & Ports", L"");
        screen(PageId::Battery, L"Battery", L"");
        screen(PageId::System, L"System", L"");
        screen(PageId::Settings, L"Settings", L"Ctrl+,");
        items.push_back({PaletteItem::Kind::Command, store_.paused() ? L"Resume updates" : L"Pause updates", L"",
                         L"Ctrl+Shift+P", [this] { set_paused(!store_.paused()); }});
        if (!elevated() && can_elevate())
            items.push_back({PaletteItem::Kind::Command, L"Unlock Full Access", L"Run Procyon as root", L"",
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
        items.push_back(
            {PaletteItem::Kind::Command, L"Quit Procyon", L"", L"Ctrl+Q", [this] { on_command(IDM_QUIT); }});
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
            // Opens the app's actions, the macOS palette's second level.
            PaletteItem item{PaletteItem::Kind::App, name,
                             L"CPU " + fmt::cpu(row.cpu_percent) + L" · " + fmt::bytes(row.memory_bytes), L"", nullptr};
            item.children = app_palette_actions(*this, row, p, name, [this](int id) { on_command(id); });
            item.icon_path = app_icon_path(row, p, s);
            item.icon_system = p && (p->flags & PC_PROC_SYSTEM) != 0;
            items.push_back(std::move(item));
        }
        overlays_.push_back(make_command_palette(std::move(items)));
        repaint();
    }

    // ---- sidebar and painting ----

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
        if (page_available(store_, PageId::Gpu)) metric(PageId::Gpu, L"GPU", MetricKind::Gpu);
        metric(PageId::Disk, L"Disk", MetricKind::Disk);
        metric(PageId::Network, L"Network", MetricKind::Network);
        if (page_available(store_, PageId::Startup) || page_available(store_, PageId::Services)) {
            section(L"Manage");
            if (page_available(store_, PageId::Startup)) page(PageId::Startup, L"Startup", Renderer::Symbol::Power);
            if (page_available(store_, PageId::Services)) page(PageId::Services, L"Services", Renderer::Symbol::Gears);
        }
        section(L"Analyze");
        page(PageId::History, L"History", Renderer::Symbol::Clock);
        if (page_available(store_, PageId::Inspect)) page(PageId::Inspect, L"Files & Ports", Renderer::Symbol::Ports);
        section(L"Machine");
        if (page_available(store_, PageId::Battery)) page(PageId::Battery, L"Battery", Renderer::Symbol::Gauge);
        page(PageId::System, L"System", Renderer::Symbol::Info);
    }

    std::wstring page_detail(PageId id) {
        const Snapshot &s = store_.snapshot();
        const pc_system_info &info = store_.system_info();
        switch (id) {
            case PageId::Overview: return display_model(info);
            case PageId::Processes: return store_.has_snapshot() ? fmt::count(s.process_count) + L" running" : L" ";
            case PageId::Startup: return L"Apps that start at login";
            case PageId::Services: return L"Background services";
            case PageId::History: return store_.records_history() ? L"The last 24 hours" : L"Off";
            case PageId::Inspect: return L"Who uses a port or a file";
            case PageId::Battery: {
                if (auto b = store_.battery()) return fmt::percent(b->level) + L" · " + battery_state(*b);
                return L"";
            }
            case PageId::System: return fmt::from_utf8(info.os_name) + L" " + fmt::from_utf8(info.os_version);
            case PageId::Settings:
                return platform::tray_available() ? L"Updates, tray, alerts, appearance"
                                                  : L"Updates, alerts, appearance";
            default: return L"";
        }
    }

    void paint(cairo_t *cr, int width, int height, float scale = 0) {
        const bool dark = settings_.theme == 2 || (settings_.theme == 0 && platform::system_prefers_dark());
        theme_.dark = dark;
        if (!screenshot_) platform::set_gtk_dark(dark);
        // PROCYON_BENCH: tell the benchmark when the first frame is drawn (its startup measurement).
        if (!first_frame_reported_ && !screenshot_) {
            first_frame_reported_ = true;
            if (g_getenv("PROCYON_BENCH")) {
                std::fputs("procyon: first frame\n", stderr);
                std::fflush(stderr);
            }
        }
        renderer_.set_theme(theme_);
        if (scale <= 0) {
            scale = 1;
            if (GtkNative *native = gtk_widget_get_native(area_))
                if (GdkSurface *surface = gtk_native_get_surface(native))
                    scale = static_cast<float>(gdk_surface_get_scale(surface));
        }
        canvas_.begin(cr, scale);
        const Rect window{0, 0, static_cast<float>(width), static_cast<float>(height)};
        sidebar_ = Rect{0, 0, kSidebarWidth, window.h};
        content_ = Rect{kSidebarWidth, kCaptionHeight, window.w - kSidebarWidth, window.h - kCaptionHeight};
        renderer_.fill(window, theme_.background());
        paint_sidebar();
        if (current_) {
            renderer_.push_clip(content_);
            current_->paint(*this, content_);
            renderer_.pop_clip();
        }
        for (auto &overlay : overlays_) overlay->paint(*this, window);
        canvas_.end();
    }

    void paint_sidebar() {
        Renderer &r = renderer_;
        const Theme &theme = theme_;
        const Snapshot &s = store_.snapshot();
        const History &h = store_.history();
        r.fill(sidebar_, theme.surface_raised());
        r.line(sidebar_.right() - 0.5f, 0, sidebar_.right() - 0.5f, sidebar_.h, theme.border());
        const float pad = tokens::space::sm + 2;
        Rect area = sidebar_.inset(pad, 0);
        area.take_top(tokens::space::lg);

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

        Rect bottom = area.take_bottom(tokens::space::md + 40 + tokens::space::sm + 32 + tokens::space::sm);
        area.take_bottom(tokens::space::md);
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

        r.line(bottom.x + tokens::space::xs, bottom.y + 0.5f, bottom.right() - tokens::space::xs, bottom.y + 0.5f,
               theme.border());
        bottom.take_top(tokens::space::md);
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
                r.sparkline(Rect{spark.x, spark.cy() - 11, spark.w, 22}, series->data(), series->size(),
                            history_window(), max, item.metric, options);
                if (secondary && secondary->size() > 1) {
                    options.area = false;
                    Color end = rgba(metric_style(item.metric).end);
                    options.color_override = &end;
                    r.sparkline(Rect{spark.x, spark.cy() - 11, spark.w, 22}, secondary->data(), secondary->size(),
                                history_window(), max, item.metric, options);
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

    // ---- snapshots, background, notifications ----

    void on_snapshot(Snapshot *snapshot) {
        store_.receive(snapshot);
        if (nav_.size() < 6) build_nav();
        if (current_) current_->tick(*this);
        for (auto &overlay : overlays_) overlay->tick(*this);
        if (screenshot_ && ++screenshot_samples_ == 6) take_screenshot();
        // In the background, return what the samples freed: the first ones read every desktop entry,
        // GPU and sensor once, and glibc keeps freed pages of the sampler's arena otherwise. Then once
        // a minute, which costs a few microseconds.
        if (hidden_ && (++hidden_samples_ == 3 || hidden_samples_ % 60 == 0)) malloc_trim(0);
        update_tray();
        repaint();
    }

    void take_screenshot() {
        const Screenshot &shot = *screenshot_;
        cairo_surface_t *surface = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, static_cast<int>(shot.width * shot.scale), static_cast<int>(shot.height * shot.scale));
        cairo_t *cr = cairo_create(surface);
        cairo_scale(cr, shot.scale, shot.scale);
        paint(cr, shot.width, shot.height, shot.scale);
        cairo_destroy(cr);
        const cairo_status_t status = cairo_surface_write_to_png(surface, shot.path.c_str());
        cairo_surface_destroy(surface);
        if (status != CAIRO_STATUS_SUCCESS) {
            g_printerr("procyon: couldn't write %s\n", shot.path.c_str());
            failed_ = true;  // a non-zero exit status, so CI notices
        }
        quit();
    }

    // "1.2M", "640K": rates short enough for the panel, never more than three digits (the macOS menu
    // bar's compact form).
    static std::wstring compact_rate(double bytes_per_second) {
        const double kib = bytes_per_second / 1024, mib = kib / 1024, gib = mib / 1024;
        wchar_t text[16];
        if (bytes_per_second < 1024) return L"0K";
        if (kib < 999.5)
            std::swprintf(text, 16, L"%.0fK", kib);
        else if (mib < 9.95)
            std::swprintf(text, 16, L"%.1fM", std::max(mib, 1.0));
        else if (mib < 999.5)
            std::swprintf(text, 16, L"%.0fM", mib);
        else if (gib < 9.95)
            std::swprintf(text, 16, L"%.1fG", std::max(gib, 1.0));
        else
            std::swprintf(text, 16, L"%.0fG", gib);
        return text;
    }

    // The modules switched on in Settings, as the macOS menu bar shows them: beside the icon (the
    // panel label, one line in the panel's font) and, spelled out, in its tooltip.
    void update_tray() {
        if (!tray_) return;
        const Snapshot &s = store_.snapshot();
        std::wstring label, guide, tip;
        auto add = [&](const std::wstring &short_form, const std::wstring &widest, const std::wstring &long_form) {
            label += (label.empty() ? L"" : L"  ") + short_form;
            guide += (guide.empty() ? L"" : L"  ") + widest;
            tip += (tip.empty() ? L"" : L" · ") + long_form;
        };
        const int modules = settings_.tray_modules;
        // Paused, the last figures stay (as the macOS menu bar keeps them) and the tooltip says so.
        if (store_.has_snapshot()) {
            if (modules & 1) add(L"CPU " + fmt::percent(s.cpu_usage), L"CPU 100%", L"CPU " + fmt::percent(s.cpu_usage));
            if (modules & 2) {
                const double used = s.memory_total ? static_cast<double>(s.memory_used) / s.memory_total : 0;
                add(L"MEM " + fmt::percent(used), L"MEM 100%", L"Memory " + fmt::percent(used));
            }
            if (modules & 4)
                add(L"↓" + compact_rate(s.net_rx_bps) + L" ↑" + compact_rate(s.net_tx_bps), L"↓888M ↑888M",
                    L"↓ " + fmt::rate(s.net_rx_bps) + L" ↑ " + fmt::rate(s.net_tx_bps));
            if ((modules & 8) && !s.gpus.empty() && s.gpus[0].utilization >= 0)
                add(L"GPU " + fmt::percent(s.gpus[0].utilization), L"GPU 100%",
                    L"GPU " + fmt::percent(s.gpus[0].utilization));
            if ((modules & 16) && s.cpu_temperature >= 0) {
                const std::wstring t = fmt::temperature(s.cpu_temperature, settings_.fahrenheit);
                add(L"TEMP " + t, L"TEMP 188°F", L"CPU " + t);
            }
            if (modules & 32)
                if (auto b = store_.battery(); b && b->present)
                    add(L"BAT " + fmt::percent(b->level), L"BAT 100%", L"Battery " + fmt::percent(b->level));
        }
        if (store_.paused()) tip = tip.empty() ? L"Paused" : L"Paused · " + tip;
        tray_->set_label(fmt::to_utf8(label), fmt::to_utf8(guide));
        tray_->set_tooltip(fmt::to_utf8(tip));
    }

    // An alert as a desktop notification (GNotification: the portal or org.freedesktop.Notifications).
    void notify(const AlertEvent &event) {
        GNotification *notification = g_notification_new(fmt::to_utf8(event.title).c_str());
        g_notification_set_body(notification, fmt::to_utf8(event.message).c_str());
        g_notification_set_priority(notification, G_NOTIFICATION_PRIORITY_HIGH);
        // One notification per rule (and app): different alerts stand side by side, as on macOS and
        // Windows, while a rule that fires again after its cooldown replaces its own older one.
        const std::string id = "alert-" + std::to_string(static_cast<int>(event.kind)) +
                               (event.app_id.empty() ? std::string() : "-" + event.app_id);
        g_application_send_notification(G_APPLICATION(app_), id.c_str(), notification);
        g_object_unref(notification);
        repaint();
    }

    // With "keep running" on, closing the window keeps Procyon sampling (for alerts and History): in
    // the tray where the desktop hosts one, else in the background until it is opened again from the
    // app grid. The root copy runs beside the session's instance, so the app grid reaches it only through
    // the copy that started it: without that link and without a tray icon it would keep running with no
    // way back, so it quits.
    gboolean on_close_request() {
        const bool reachable = platform::tray_available() || !elevated() || handover_watch_ != 0;
        if (settings_.minimize_to_tray && !quit_ && reachable) {
            hide_window();
            return TRUE;
        }
        shutdown();
        return TRUE;
    }

    void hide_window() {
        if (hidden_) return;
        hidden_ = true;
        if (!held_) {
            g_application_hold(G_APPLICATION(app_));
            held_ = true;
        }
        gtk_widget_set_visible(window_, FALSE);
        store_.set_process_sampling(false);
        // Nothing paints while hidden: drop the cached text, shadows and icons, and return the
        // freed pages, so the background footprint is the sampler's, not the window's.
        canvas_.trim();
        malloc_trim(0);
        hidden_samples_ = 0;
    }

    void show_again() {
        hidden_ = false;
        store_.set_process_sampling(true);
        gtk_widget_set_visible(window_, TRUE);
        store_.refresh_now();
    }

    void quit() {
        quit_ = true;
        shutdown();
    }

    void close_handover_pipe() {
        if (handover_pipe_ < 0) return;
        ::close(handover_pipe_);
        handover_pipe_ = -1;
    }

    // The root copy: requests from the copy that started it arrive on stdin, one line each. "present"
    // comes from a second launch, which reaches that copy (the session's single instance), not this one.
    void listen_for_handover() {
        const char *handover = g_getenv("PROCYON_HANDOVER");
        if (!handover || std::string_view(handover) != "1") return;
        g_unix_set_fd_nonblocking(STDIN_FILENO, TRUE, nullptr);
        handover_watch_ = g_unix_fd_add(
            STDIN_FILENO, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
            [](gint fd, GIOCondition, gpointer self) -> gboolean {
                auto *w = static_cast<MainWindow *>(self);
                char buffer[256];
                bool asked = false;
                for (;;) {
                    const ssize_t n = ::read(fd, buffer, sizeof(buffer));
                    if (n > 0) {
                        asked = asked || std::string_view(buffer, static_cast<size_t>(n)).find("present") !=
                                             std::string_view::npos;
                        continue;
                    }
                    if (n < 0 && errno == EINTR) continue;
                    if (n < 0 && errno == EAGAIN) break;
                    // The other end closed: the copy that started this one is gone.
                    w->handover_watch_ = 0;
                    if (asked) w->present();
                    return G_SOURCE_REMOVE;
                }
                if (asked) w->present();
                return G_SOURCE_CONTINUE;
            },
            this);
    }

    // The root copy didn't take over (authorization dismissed, or it failed to start): this copy
    // resumes as it was.
    void take_back() {
        close_handover_pipe();
        handed_over_ = false;
        store_.set_records_history(settings_.records_history);
        store_.set_paused(paused_before_handover_);
        if (!screenshot_) make_tray();
        show_again();
        gtk_window_present(GTK_WINDOW(window_));
    }

    static bool can_elevate() {
        static const bool found = [] {
            char *path = g_find_program_in_path("pkexec");
            g_free(path);
            return path != nullptr;
        }();
        return found;
    }

    // ---- settings ($XDG_CONFIG_HOME/procyon/settings.ini) ----

    void load_settings() {
        // Keep running after the window closes only where there is a tray icon to come back from.
        const bool tray = !screenshot_ && Tray::host_present();
        platform::set_tray_available(tray);
        settings_.minimize_to_tray = tray;
        if (!persist_) return;  // --no-settings: defaults, nothing read
        auto value = [](const wchar_t *name, int64_t fallback) {
            return platform::read_setting(L"", name).value_or(fallback);
        };
        settings_.interval = std::clamp(value(L"IntervalMs", 1000) / 1000.0, 0.5, 5.0);
        settings_.theme = static_cast<int>(value(L"Theme", 0));
        settings_.fahrenheit = value(L"Fahrenheit", 0) != 0;
        settings_.minimize_to_tray = value(L"MinimizeToTray", tray ? 1 : 0) != 0;
        settings_.tray_modules = static_cast<int>(value(L"TrayModules", 1 | 2));
        settings_.default_view = static_cast<int>(value(L"DefaultView", PC_VIEW_GROUPED));
        if (settings_.default_view < PC_VIEW_FLAT || settings_.default_view > PC_VIEW_TREE)
            settings_.default_view = PC_VIEW_GROUPED;
        settings_.records_history = value(L"RecordsHistory", 1) != 0;
        width_ = static_cast<int>(std::clamp<int64_t>(value(L"Width", 1280), 900, 8000));
        height_ = static_cast<int>(std::clamp<int64_t>(value(L"Height", 800), 600, 8000));
        maximized_ = value(L"Maximized", 0) != 0;
    }

    void save_settings() {
        if (!persist_) return;  // --no-settings: nothing written back
        platform::write_setting(L"", L"IntervalMs", static_cast<int64_t>(settings_.interval * 1000));
        platform::write_setting(L"", L"Theme", settings_.theme);
        platform::write_setting(L"", L"Fahrenheit", settings_.fahrenheit ? 1 : 0);
        platform::write_setting(L"", L"MinimizeToTray", settings_.minimize_to_tray ? 1 : 0);
        platform::write_setting(L"", L"TrayModules", settings_.tray_modules);
        platform::write_setting(L"", L"DefaultView", settings_.default_view);
        platform::write_setting(L"", L"RecordsHistory", settings_.records_history ? 1 : 0);
        if (window_) {
            const bool maximized = gtk_window_is_maximized(GTK_WINDOW(window_));
            platform::write_setting(L"", L"Maximized", maximized ? 1 : 0);
            if (!maximized) {
                int w = 0, h = 0;
                gtk_window_get_default_size(GTK_WINDOW(window_), &w, &h);
                if (w > 0 && h > 0) {
                    platform::write_setting(L"", L"Width", w);
                    platform::write_setting(L"", L"Height", h);
                }
            }
        }
    }

    GtkApplication *app_;
    std::optional<PageId> initial_page_;
    bool persist_ = true;
    std::optional<Screenshot> screenshot_;
    int screenshot_samples_ = 0;
    bool failed_ = false;
    bool start_in_background_ = false;
    int hidden_samples_ = 0;  // samples since the window was hidden
    bool first_frame_reported_ = false;
    std::unique_ptr<Tray> tray_;
    bool handed_over_ = false;  // a root copy runs in this one's place
    bool paused_before_handover_ = false;
    gint64 handover_started_ = 0;
    int handover_pipe_ = -1;    // the root copy's stdin, while it runs in this one's place
    guint handover_watch_ = 0;  // in the root copy: the watch on stdin for that pipe
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    GtkWidget *window_ = nullptr;
    GtkWidget *area_ = nullptr;
    CairoCanvas canvas_;
    Renderer renderer_{canvas_};
    Store store_;
    Settings settings_;
    Theme theme_;
    std::vector<std::unique_ptr<Page>> pages_;
    Page *current_ = nullptr;
    std::vector<std::unique_ptr<Overlay>> overlays_;
    std::vector<NavItem> nav_;
    Rect sidebar_, content_, pause_button_, settings_button_;
    ScrollState sidebar_scroll_;
    float mouse_x_ = -1, mouse_y_ = -1;
    double scroll_remainder_ = 0;
    Cursor cursor_ = Cursor::Arrow;
    guint frame_timer_ = 0;
    int next_action_ = 0;
    int width_ = 1280, height_ = 800;
    bool maximized_ = false;
    bool hidden_ = false;
    bool held_ = false;
    bool quit_ = false;
};

struct App {
    std::optional<PageId> initial;
    bool persist = true;
    bool background = false;
    std::optional<MainWindow::Screenshot> screenshot;
    std::unique_ptr<MainWindow> window;
};

}  // namespace

int run_app(int argc, char **argv) {
    App app;
    // Our own options; GTK sees only the program name.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--page" && i + 1 < argc)
            app.initial = page_from_name(argv[++i]);
        else if (arg == "--no-settings")
            app.persist = false;
        else if (arg == "--background")
            app.background = true;
        else if (arg == "--screenshot" && i + 1 < argc) {
            if (!app.screenshot) app.screenshot.emplace();
            app.screenshot->path = argv[++i];
            app.persist = false;
        } else if (arg == "--dark" || arg == "--light") {
            if (!app.screenshot) app.screenshot.emplace();
            app.screenshot->theme = arg == "--dark" ? 2 : 1;
        } else if (arg == "--version") {
            g_print("Procyon %s\n", PROCYON_VERSION_STRING);
            return 0;
        }
    }
    g_set_prgname(kAppId);
    g_set_application_name("Procyon");
    // One instance per user session; the root copy (Full access), screenshots and measurements
    // (--no-settings) run beside it.
    const bool separate = ::geteuid() == 0 || (app.screenshot && !app.screenshot->path.empty()) || !app.persist;
    GtkApplication *gtk_app =
        gtk_application_new(kAppId, separate ? G_APPLICATION_NON_UNIQUE : G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(+[](GtkApplication *a, gpointer data) {
                         auto *state = static_cast<App *>(data);
                         if (state->window) {
                             state->window->present();
                             return;
                         }
                         state->window = std::make_unique<MainWindow>(a, state->initial, state->persist,
                                                                      state->screenshot, state->background);
                         state->window->create();
                     }),
                     &app);
    char *gtk_argv[] = {argv[0], nullptr};
    int status = g_application_run(G_APPLICATION(gtk_app), 1, gtk_argv);
    if (app.window && app.window->failed() && status == 0) status = 1;
    if (app.window) app.window->shutdown();
    app.window.reset();
    g_object_unref(gtk_app);
    return status;
}

}  // namespace procyon::ui
