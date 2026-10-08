// platform.hpp for Linux: app icons from the desktop entries and the icon theme, the GNOME/GTK
// colour scheme, XDG folders for data and settings, the GDK clipboard.
#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform.hpp"
#include "platform_linux.hpp"

namespace procyon::ui::platform {
namespace {

// The user this copy works for: the one who unlocked full access when pkexec started it as root
// (PKEXEC_UID), so the root copy keeps their settings and history; else the real user.
const passwd *pkexec_caller() {
    static passwd entry{};
    static char buffer[4096];
    static const passwd *caller = []() -> const passwd * {
        if (::geteuid() != 0) return nullptr;
        const char *value = std::getenv("PKEXEC_UID");
        if (!value || !*value) return nullptr;
        char *end = nullptr;
        const unsigned long uid = std::strtoul(value, &end, 10);
        if (!end || *end != '\0' || uid == 0) return nullptr;
        passwd *result = nullptr;
        return ::getpwuid_r(static_cast<uid_t>(uid), &entry, buffer, sizeof(buffer), &result) == 0 ? result : nullptr;
    }();
    return caller;
}

std::string home_folder() {
    if (const passwd *caller = pkexec_caller(); caller && caller->pw_dir) return caller->pw_dir;
    const char *home = std::getenv("HOME");
    return home ? home : "/tmp";
}

std::string xdg_dir(const char *variable, const char *fallback) {
    // pkexec passes the caller's XDG_CONFIG_HOME/XDG_DATA_HOME through when they set them.
    const char *value = std::getenv(variable);
    if (value && *value == '/') return value;
    return home_folder() + "/" + fallback;
}

void give_back(const std::string &path) {
    // Best effort: a file the user can't own stays root's and is rewritten next time as a new file.
    const passwd *caller = pkexec_caller();
    if (caller && ::lchown(path.c_str(), caller->pw_uid, caller->pw_gid) != 0) return;
}

std::string settings_path() { return xdg_dir("XDG_CONFIG_HOME", ".config") + "/procyon/settings.ini"; }

GKeyFile *settings_file() {
    static GKeyFile *file = [] {
        GKeyFile *f = g_key_file_new();
        g_key_file_load_from_file(f, settings_path().c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
        return f;
    }();
    return file;
}

std::string group_name(std::wstring_view group) { return group.empty() ? "Procyon" : fmt::to_utf8(group); }

// Installed applications by the program they run, as the core groups processes: the executable's
// name, its StartupWMClass, and the directory a launcher points into (/opt/google/chrome).
struct AppIcons {
    std::unordered_map<std::string, GIcon *> by_key;
};

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(g_ascii_tolower(c));
    return s;
}

// Programs that run many different apps' code: their name alone says nothing about which app a
// process is, so it never picks an icon (a desktop entry running "python3 tool.py" would otherwise
// give every Python process its icon).
bool interpreter(const std::string &name) {
    if (name.rfind("python", 0) == 0) return true;
    for (const char *i : {"sh", "bash", "dash", "zsh", "env", "perl", "ruby", "node", "java", "electron", "flatpak",
                          "snap", "gjs", "wine", "mono", "dotnet"})
        if (name == i) return true;
    return false;
}

const AppIcons &app_icons() {
    static const AppIcons icons = [] {
        AppIcons result;
        GList *apps = g_app_info_get_all();
        for (GList *l = apps; l; l = l->next) {
            GAppInfo *info = G_APP_INFO(l->data);
            GIcon *icon = g_app_info_get_icon(info);
            if (!icon || !G_IS_DESKTOP_APP_INFO(info)) continue;
            auto add = [&](const std::string &key) {
                if (!key.empty() && !result.by_key.count(key)) result.by_key[key] = G_ICON(g_object_ref(icon));
            };
            const char *exe = g_app_info_get_executable(info);
            std::string program = exe ? exe : "";
            // "flatpak run --command=x id" and "env VAR=1 x" run something else than their first word.
            if (const char *line = g_app_info_get_commandline(info)) {
                gchar **argv = nullptr;
                if (g_shell_parse_argv(line, nullptr, &argv, nullptr)) {
                    for (gchar **a = argv; *a; ++a) {
                        const std::string arg = *a;
                        if (arg.rfind("--command=", 0) == 0) program = arg.substr(10);
                    }
                    g_strfreev(argv);
                }
            }
            char *base = g_path_get_basename(program.c_str());
            if (!interpreter(lower(base))) add(lower(base));
            g_free(base);
            if (const char *wm = g_desktop_app_info_get_startup_wm_class(G_DESKTOP_APP_INFO(info))) add(lower(wm));
            if (char *found = g_find_program_in_path(program.c_str())) {
                if (char *resolved = realpath(found, nullptr)) {
                    if (!interpreter(lower(program.substr(program.find_last_of('/') + 1))))
                        add(std::string("path:") + resolved);
                    char *dir = g_path_get_dirname(resolved);
                    const std::string d = dir;
                    if (d != "/usr/bin" && d != "/bin" && d != "/usr/local/bin" && d != "/usr/sbin" && d != "/snap/bin")
                        add("dir:" + d);
                    g_free(dir);
                    free(resolved);
                }
                g_free(found);
            }
        }
        g_list_free_full(apps, g_object_unref);
        return result;
    }();
    return icons;
}

std::shared_ptr<const Image> image_from_pixbuf(GdkPixbuf *pixbuf, int pixels) {
    if (!pixbuf) return nullptr;
    const int width = gdk_pixbuf_get_width(pixbuf), height = gdk_pixbuf_get_height(pixbuf);
    const int stride = gdk_pixbuf_get_rowstride(pixbuf), channels = gdk_pixbuf_get_n_channels(pixbuf);
    const guchar *data = gdk_pixbuf_read_pixels(pixbuf);
    static std::atomic<uint64_t> next_id{1};
    auto image = std::make_shared<Image>();
    image->id = next_id++;
    // Centred in a square of `pixels`, the way the macOS and Windows icons are drawn.
    image->width = image->height = pixels;
    image->pixels.assign(static_cast<size_t>(pixels) * pixels, 0);
    const int ox = (pixels - width) / 2, oy = (pixels - height) / 2;
    for (int y = 0; y < height; ++y) {
        if (y + oy < 0 || y + oy >= pixels) continue;
        for (int x = 0; x < width; ++x) {
            if (x + ox < 0 || x + ox >= pixels) continue;
            const guchar *p = data + y * stride + x * channels;
            const uint32_t a = channels == 4 ? p[3] : 255;
            const uint32_t r = p[0] * a / 255, g = p[1] * a / 255, b = p[2] * a / 255;
            image->pixels[static_cast<size_t>(y + oy) * pixels + (x + ox)] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    return image;
}

}  // namespace

std::shared_ptr<const Image> app_icon(std::wstring_view path, int pixels) {
    if (path.empty() || pixels <= 0) return nullptr;
    const std::string file = fmt::to_utf8(path);
    const auto &icons = app_icons().by_key;
    // The executable itself first, then the folder an app is installed in, then its name.
    auto it = icons.find("path:" + file);
    if (it == icons.end()) {
        char *dir = g_path_get_dirname(file.c_str());
        it = icons.find(std::string("dir:") + dir);
        g_free(dir);
    }
    if (it == icons.end()) {
        char *base = g_path_get_basename(file.c_str());
        const std::string name = lower(base);
        g_free(base);
        if (!interpreter(name)) it = icons.find(name);
    }
    if (it == icons.end()) return nullptr;
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return nullptr;
    GtkIconPaintable *paintable =
        gtk_icon_theme_lookup_by_gicon(gtk_icon_theme_get_for_display(display), it->second, pixels, 1, GTK_TEXT_DIR_LTR,
                                       static_cast<GtkIconLookupFlags>(0));
    if (!paintable) return nullptr;
    std::shared_ptr<const Image> image;
    if (GFile *icon_file = gtk_icon_paintable_get_file(paintable)) {
        if (char *icon_path = g_file_get_path(icon_file)) {
            // SVGs render at the size asked for; bitmaps are scaled down to it.
            if (GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(icon_path, pixels, pixels, TRUE, nullptr)) {
                image = image_from_pixbuf(pixbuf, pixels);
                g_object_unref(pixbuf);
            }
            g_free(icon_path);
        }
        g_object_unref(icon_file);
    }
    g_object_unref(paintable);
    return image;
}

namespace {

// org.gnome.desktop.interface, where it has the color-scheme key (GNOME and the desktops that follow it).
GSettings *interface_settings() {
    static GSettings *interface = []() -> GSettings * {
        GSettingsSchemaSource *source = g_settings_schema_source_get_default();
        GSettingsSchema *schema =
            source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : nullptr;
        if (!schema) return nullptr;
        const bool has_key = g_settings_schema_has_key(schema, "color-scheme");
        g_settings_schema_unref(schema);
        return has_key ? g_settings_new("org.gnome.desktop.interface") : nullptr;
    }();
    return interface;
}

// GTK's dark preference as the desktop set it, before set_gtk_dark changed it for Procyon's windows.
std::optional<bool> desktop_prefers_dark_gtk;

bool gtk_prefers_dark(GtkSettings *settings) {
    if (desktop_prefers_dark_gtk) return *desktop_prefers_dark_gtk;
    gboolean prefer_dark = FALSE;
    g_object_get(settings, "gtk-application-prefer-dark-theme", &prefer_dark, nullptr);
    return prefer_dark;
}

}  // namespace

void set_gtk_dark(bool dark) {
    GtkSettings *settings = gtk_settings_get_default();
    if (!settings) return;
    if (!desktop_prefers_dark_gtk) desktop_prefers_dark_gtk = gtk_prefers_dark(settings);
    gboolean current = FALSE;
    g_object_get(settings, "gtk-application-prefer-dark-theme", &current, nullptr);
    if (static_cast<bool>(current) != dark)
        g_object_set(settings, "gtk-application-prefer-dark-theme", dark ? TRUE : FALSE, nullptr);
}

void watch_color_scheme(std::function<void()> changed) {
    static std::function<void()> callback;
    callback = std::move(changed);
    const auto notify = +[](gpointer, gpointer, gpointer) {
        if (callback) callback();
    };
    if (GSettings *interface = interface_settings())
        g_signal_connect(interface, "changed::color-scheme", G_CALLBACK(notify), nullptr);
    if (GtkSettings *settings = gtk_settings_get_default())
        g_signal_connect(settings, "notify::gtk-theme-name", G_CALLBACK(notify), nullptr);
}

bool system_prefers_dark() {
    // The root copy (pkexec) is told what the desktop prefers: it can't read the user's dconf database.
    if (pkexec_caller())
        if (const char *dark = std::getenv("PROCYON_DESKTOP_DARK"); dark && *dark) return dark[0] == '1';
    if (GSettings *interface = interface_settings()) {
        gchar *scheme = g_settings_get_string(interface, "color-scheme");
        const bool dark = scheme && g_strcmp0(scheme, "prefer-dark") == 0;
        const bool light = scheme && g_strcmp0(scheme, "prefer-light") == 0;
        g_free(scheme);
        if (dark || light) return dark;
    }
    // Elsewhere: GTK's own preference, or a theme named "…-dark".
    if (GtkSettings *settings = gtk_settings_get_default()) {
        gchar *theme = nullptr;
        g_object_get(settings, "gtk-theme-name", &theme, nullptr);
        const bool dark = gtk_prefers_dark(settings) || (theme && g_str_has_suffix(theme, "-dark"));
        g_free(theme);
        return dark;
    }
    return false;
}

std::wstring data_file(std::wstring_view name) {
    return fmt::from_utf8(xdg_dir("XDG_DATA_HOME", ".local/share") + "/procyon/") + std::wstring(name);
}

void create_parent_directories(std::wstring_view path) {
    char *dir = g_path_get_dirname(fmt::to_utf8(path).c_str());
    // The folders this call creates, outermost last, to hand back to the pkexec caller.
    std::vector<std::string> created;
    for (std::string d = dir; !d.empty() && d != "/" && !g_file_test(d.c_str(), G_FILE_TEST_EXISTS);) {
        created.push_back(d);
        char *parent = g_path_get_dirname(d.c_str());
        d = parent;
        g_free(parent);
    }
    g_mkdir_with_parents(dir, 0700);
    for (const std::string &d : created) give_back(d);
    g_free(dir);
}

void file_written(std::wstring_view path) { give_back(fmt::to_utf8(path)); }

std::wstring clipboard_text() {
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return {};
    // GDK reads the clipboard asynchronously: wait for it in a nested loop (a paste is a user action,
    // and the reply comes within milliseconds).
    struct Read {
        GMainLoop *loop;
        std::string text;
    } read{g_main_loop_new(nullptr, FALSE), {}};
    gdk_clipboard_read_text_async(
        gdk_display_get_clipboard(display), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            auto *r = static_cast<Read *>(data);
            if (char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, nullptr)) {
                r->text = text;
                g_free(text);
            }
            g_main_loop_quit(r->loop);
        },
        &read);
    g_main_loop_run(read.loop);
    g_main_loop_unref(read.loop);
    return fmt::from_utf8(read.text);
}

namespace {
bool tray_present = false;
}  // namespace

bool tray_available() { return tray_present; }
void set_tray_available(bool available) { tray_present = available; }

void sampler_thread_begin() {}
void sampler_thread_end() {}

std::optional<int64_t> read_setting(std::wstring_view group, std::wstring_view name) {
    GError *error = nullptr;
    const gint64 value =
        g_key_file_get_int64(settings_file(), group_name(group).c_str(), fmt::to_utf8(name).c_str(), &error);
    if (error) {
        g_error_free(error);
        return std::nullopt;
    }
    return value;
}

void write_setting(std::wstring_view group, std::wstring_view name, int64_t value) {
    GKeyFile *file = settings_file();
    const std::string g = group_name(group), key = fmt::to_utf8(name);
    GError *error = nullptr;
    const gint64 current = g_key_file_get_int64(file, g.c_str(), key.c_str(), &error);
    if (!error && current == value) return;
    if (error) g_error_free(error);
    g_key_file_set_int64(file, g.c_str(), key.c_str(), value);
    const std::string path = settings_path();
    create_parent_directories(fmt::from_utf8(path));
    if (g_key_file_save_to_file(file, path.c_str(), nullptr)) give_back(path);
}

}  // namespace procyon::ui::platform
