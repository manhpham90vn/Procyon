// A tray icon on the desktops that host one: the StatusNotifierItem protocol (KDE, XFCE, Cinnamon,
// Budgie, and GNOME with the AppIndicator extension Ubuntu ships), with its menu over DBusMenu.
#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>

namespace procyon::ui {

class Tray {
public:
    struct Callbacks {
        std::function<void()> activate;  // left click: open or hide the window
        std::function<void()> toggle_pause;
        std::function<void()> settings;  // Settings…: the window on its Settings page
        std::function<void()> quit;
    };

    explicit Tray(Callbacks callbacks);
    ~Tray();
    Tray(const Tray &) = delete;
    Tray &operator=(const Tray &) = delete;

    // Whether a tray host (StatusNotifierWatcher with a host registered) is running right now.
    bool available() const { return available_; }
    // Asks the session bus once, synchronously (startup only).
    static bool host_present();

    void set_tooltip(const std::string &text);
    // Text beside the icon (the Ayatana label, which Ubuntu's AppIndicator extension and other
    // Ayatana hosts draw in the panel font); `guide` is its widest form, so the panel keeps its width.
    void set_label(const std::string &text, const std::string &guide);
    void set_paused(bool paused);

    // GDBus vtable entries (public so the vtable at namespace scope can name them).
    static void on_method(GDBusConnection *, const char *, const char *, const char *interface, const char *method,
                          GVariant *parameters, GDBusMethodInvocation *invocation, gpointer self);
    static GVariant *on_property(GDBusConnection *, const char *, const char *, const char *interface,
                                 const char *property, GError **, gpointer self);

private:
    void clicked(int id);  // a menu item, by its MenuId
    void item_method(const char *method, GVariant *parameters, GDBusMethodInvocation *invocation);
    void menu_method(const char *method, GVariant *parameters, GDBusMethodInvocation *invocation);
    GVariant *item_property(const char *property);
    GVariant *menu_property(const char *property);
    GVariant *layout() const;
    GVariant *pixmaps() const;
    void register_with_watcher();
    void emit(const char *interface, const char *signal, GVariant *parameters);

    Callbacks callbacks_;
    GDBusConnection *bus_ = nullptr;
    GDBusNodeInfo *item_info_ = nullptr;
    GDBusNodeInfo *menu_info_ = nullptr;
    guint item_id_ = 0, menu_id_ = 0, name_id_ = 0, watch_id_ = 0;
    std::string bus_name_;
    std::string tooltip_;
    std::string label_, label_guide_;
    bool paused_ = false;
    bool available_ = false;
    guint revision_ = 1;
};

}  // namespace procyon::ui
