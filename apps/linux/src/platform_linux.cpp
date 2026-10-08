// platform.hpp for Linux: app icons from the desktop entries and the icon theme, the GNOME/GTK
// colour scheme, XDG folders for data and settings, the GDK clipboard.
#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "platform.hpp"
#include "platform_linux.hpp"

namespace procyon::ui::platform {
namespace {

std::string xdg_dir(const char *variable, const char *fallback) {
    const char *value = std::getenv(variable);
    if (value && *value == '/') return value;
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/" + fallback;
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
            add(lower(base));
            g_free(base);
            if (const char *wm = g_desktop_app_info_get_startup_wm_class(G_DESKTOP_APP_INFO(info))) add(lower(wm));
            if (char *found = g_find_program_in_path(program.c_str())) {
                if (char *resolved = realpath(found, nullptr)) {
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
    char *base = g_path_get_basename(file.c_str());
    auto it = icons.find(lower(base));
    g_free(base);
    if (it == icons.end()) {
        char *dir = g_path_get_dirname(file.c_str());
        it = icons.find(std::string("dir:") + dir);
        g_free(dir);
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

bool system_prefers_dark() {
    // GNOME (and the desktops that follow its setting): org.gnome.desktop.interface color-scheme.
    static GSettings *interface = []() -> GSettings * {
        GSettingsSchemaSource *source = g_settings_schema_source_get_default();
        GSettingsSchema *schema =
            source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : nullptr;
        if (!schema) return nullptr;
        const bool has_key = g_settings_schema_has_key(schema, "color-scheme");
        g_settings_schema_unref(schema);
        return has_key ? g_settings_new("org.gnome.desktop.interface") : nullptr;
    }();
    if (interface) {
        gchar *scheme = g_settings_get_string(interface, "color-scheme");
        const bool dark = scheme && g_strcmp0(scheme, "prefer-dark") == 0;
        const bool light = scheme && g_strcmp0(scheme, "prefer-light") == 0;
        g_free(scheme);
        if (dark || light) return dark;
    }
    // Elsewhere: GTK's own preference, or a theme named "…-dark".
    if (GtkSettings *settings = gtk_settings_get_default()) {
        gboolean prefer_dark = FALSE;
        gchar *theme = nullptr;
        g_object_get(settings, "gtk-application-prefer-dark-theme", &prefer_dark, "gtk-theme-name", &theme, nullptr);
        const bool dark = prefer_dark || (theme && g_str_has_suffix(theme, "-dark"));
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
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
}

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
    g_key_file_save_to_file(file, path.c_str(), nullptr);
}

}  // namespace procyon::ui::platform
