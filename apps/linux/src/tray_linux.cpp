// StatusNotifierItem (org.kde.StatusNotifierItem) and its menu (com.canonical.dbusmenu), exported
// with GDBus. No libappindicator: that library is GTK 3 only.
#include "tray_linux.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "canvas_cairo.hpp"
#include "platform_linux.hpp"
#include "ui.hpp"

namespace procyon::ui {
namespace {

constexpr const char *kItemPath = "/StatusNotifierItem";
constexpr const char *kMenuPath = "/MenuBar";
constexpr const char *kWatcher = "org.kde.StatusNotifierWatcher";

constexpr const char *kItemXml = R"xml(<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="XAyatanaLabel" type="s" access="read"/>
    <property name="XAyatanaLabelGuide" type="s" access="read"/>
    <property name="XAyatanaOrderingIndex" type="u" access="read"/>
    <method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg name="status" type="s"/></signal>
    <signal name="XAyatanaNewLabel"><arg name="label" type="s"/><arg name="guide" type="s"/></signal>
  </interface>
</node>)xml";

constexpr const char *kMenuXml = R"xml(<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/>
      <arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/>
      <arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg name="id" type="i" direction="in"/>
      <arg name="name" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/>
      <arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg name="events" type="a(isvu)" direction="in"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="updatesNeeded" type="ai" direction="out"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <signal name="LayoutUpdated"><arg name="revision" type="u"/><arg name="parent" type="i"/></signal>
    <signal name="ItemsPropertiesUpdated">
      <arg name="updatedProps" type="a(ia{sv})"/>
      <arg name="removedProps" type="a(ias)"/>
    </signal>
  </interface>
</node>)xml";

enum MenuId { MenuRoot = 0, MenuOpen = 1, MenuPause = 2, MenuSeparator = 3, MenuQuit = 4 };

const GDBusInterfaceVTable kVTable = {&Tray::on_method, &Tray::on_property, nullptr, {}};

// The sidebar's brand mark (gradient disc with a sparkle) at `size` pixels, as the ARGB32 rows in
// network byte order and without premultiplication that StatusNotifierItem asks for.
GVariant *brand_pixmap(int size) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);
    {
        CairoCanvas canvas;
        canvas.begin(cr, 1);
        Renderer r(canvas);
        const float s = static_cast<float>(size);
        r.fill_gradient_round(Rect{0, 0, s, s}, s / 2, rgba(tokens::metric::cpu.start),
                              rgba(tokens::metric::memory.start));
        const float glyph = s / 2;
        r.symbol(Renderer::Symbol::Sparkle, Rect{(s - glyph) / 2, (s - glyph) / 2, glyph, glyph}, colors::white,
                 std::max(1.0f, s / 16));
        canvas.end();
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    const unsigned char *data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    std::vector<unsigned char> bytes(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t pixel;
            std::memcpy(&pixel, data + y * stride + x * 4, 4);
            const uint32_t a = pixel >> 24;
            auto channel = [&](int shift) {
                const uint32_t c = (pixel >> shift) & 0xFF;
                return static_cast<unsigned char>(a ? std::min<uint32_t>(255, c * 255 / a) : 0);
            };
            unsigned char *out = &bytes[(static_cast<size_t>(y) * size + x) * 4];
            out[0] = static_cast<unsigned char>(a);
            out[1] = channel(16);
            out[2] = channel(8);
            out[3] = channel(0);
        }
    }
    cairo_surface_destroy(surface);
    GVariant *pixels = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, bytes.data(), bytes.size(), 1);
    return g_variant_new("(ii@ay)", size, size, pixels);
}

GVariant *menu_item(int id, std::initializer_list<std::pair<const char *, GVariant *>> properties) {
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    for (const auto &[key, value] : properties) g_variant_builder_add(&props, "{sv}", key, value);
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    return g_variant_new("(ia{sv}av)", id, &props, &children);
}

}  // namespace

bool Tray::host_present() {
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return false;
    GVariant *reply =
        g_dbus_connection_call_sync(bus, kWatcher, "/StatusNotifierWatcher", "org.freedesktop.DBus.Properties", "Get",
                                    g_variant_new("(ss)", kWatcher, "IsStatusNotifierHostRegistered"),
                                    G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr, nullptr);
    bool present = false;
    if (reply) {
        GVariant *value = nullptr;
        g_variant_get(reply, "(v)", &value);
        present = g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value);
        g_variant_unref(value);
        g_variant_unref(reply);
    }
    g_object_unref(bus);
    return present;
}

Tray::Tray(Callbacks callbacks) : callbacks_(std::move(callbacks)) {
    bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus_) return;
    item_info_ = g_dbus_node_info_new_for_xml(kItemXml, nullptr);
    menu_info_ = g_dbus_node_info_new_for_xml(kMenuXml, nullptr);
    item_id_ =
        g_dbus_connection_register_object(bus_, kItemPath, item_info_->interfaces[0], &kVTable, this, nullptr, nullptr);
    menu_id_ =
        g_dbus_connection_register_object(bus_, kMenuPath, menu_info_->interfaces[0], &kVTable, this, nullptr, nullptr);
    bus_name_ = "org.kde.StatusNotifierItem-" + std::to_string(::getpid()) + "-1";
    name_id_ = g_bus_own_name_on_connection(bus_, bus_name_.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr,
                                            nullptr, nullptr);
    // (Re)register whenever a watcher appears: the panel may start after Procyon, or restart.
    watch_id_ = g_bus_watch_name_on_connection(
        bus_, kWatcher, G_BUS_NAME_WATCHER_FLAGS_NONE,
        [](GDBusConnection *, const char *, const char *, gpointer self) {
            static_cast<Tray *>(self)->register_with_watcher();
        },
        [](GDBusConnection *, const char *, gpointer self) {
            auto *tray = static_cast<Tray *>(self);
            tray->available_ = false;
            platform::set_tray_available(false);
        },
        this, nullptr);
}

Tray::~Tray() {
    platform::set_tray_available(false);
    if (watch_id_) g_bus_unwatch_name(watch_id_);
    if (name_id_) g_bus_unown_name(name_id_);
    if (bus_) {
        if (item_id_) g_dbus_connection_unregister_object(bus_, item_id_);
        if (menu_id_) g_dbus_connection_unregister_object(bus_, menu_id_);
        g_object_unref(bus_);
    }
    if (item_info_) g_dbus_node_info_unref(item_info_);
    if (menu_info_) g_dbus_node_info_unref(menu_info_);
}

void Tray::register_with_watcher() {
    g_dbus_connection_call(
        bus_, kWatcher, "/StatusNotifierWatcher", kWatcher, "RegisterStatusNotifierItem",
        g_variant_new("(s)", bus_name_.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
        [](GObject *source, GAsyncResult *result, gpointer self) {
            GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, nullptr);
            auto *tray = static_cast<Tray *>(self);
            tray->available_ = reply != nullptr;
            platform::set_tray_available(tray->available_);
            if (reply) g_variant_unref(reply);
        },
        this);
}

void Tray::emit(const char *interface, const char *signal, GVariant *parameters) {
    if (!bus_) return;
    const char *path = g_str_equal(interface, "com.canonical.dbusmenu") ? kMenuPath : kItemPath;
    g_dbus_connection_emit_signal(bus_, nullptr, path, interface, signal, parameters, nullptr);
}

void Tray::set_tooltip(const std::string &text) {
    if (text == tooltip_) return;
    tooltip_ = text;
    emit("org.kde.StatusNotifierItem", "NewToolTip", nullptr);
}

void Tray::set_label(const std::string &text, const std::string &guide) {
    if (text == label_ && guide == label_guide_) return;
    label_ = text;
    label_guide_ = guide;
    emit("org.kde.StatusNotifierItem", "XAyatanaNewLabel", g_variant_new("(ss)", label_.c_str(), label_guide_.c_str()));
}

void Tray::set_paused(bool paused) {
    if (paused == paused_) return;
    paused_ = paused;
    ++revision_;
    emit("com.canonical.dbusmenu", "LayoutUpdated", g_variant_new("(ui)", revision_, 0));
}

GVariant *Tray::pixmaps() const {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    for (int size : {22, 32, 48}) g_variant_builder_add_value(&builder, brand_pixmap(size));
    return g_variant_builder_end(&builder);
}

GVariant *Tray::layout() const {
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    g_variant_builder_add(&children, "v", menu_item(MenuOpen, {{"label", g_variant_new_string("Open Procyon")}}));
    g_variant_builder_add(
        &children, "v",
        menu_item(MenuPause, {{"label", g_variant_new_string(paused_ ? "Resume updates" : "Pause updates")}}));
    g_variant_builder_add(&children, "v", menu_item(MenuSeparator, {{"type", g_variant_new_string("separator")}}));
    g_variant_builder_add(&children, "v", menu_item(MenuQuit, {{"label", g_variant_new_string("Quit")}}));
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "children-display", g_variant_new_string("submenu"));
    return g_variant_new("(ia{sv}av)", MenuRoot, &props, &children);
}

void Tray::on_method(GDBusConnection *, const char *, const char *, const char *interface, const char *method,
                     GVariant *parameters, GDBusMethodInvocation *invocation, gpointer self) {
    auto *tray = static_cast<Tray *>(self);
    if (g_str_equal(interface, "com.canonical.dbusmenu"))
        tray->menu_method(method, parameters, invocation);
    else
        tray->item_method(method, parameters, invocation);
}

GVariant *Tray::on_property(GDBusConnection *, const char *, const char *, const char *interface, const char *property,
                            GError **, gpointer self) {
    auto *tray = static_cast<Tray *>(self);
    return g_str_equal(interface, "com.canonical.dbusmenu") ? tray->menu_property(property)
                                                            : tray->item_property(property);
}

void Tray::item_method(const char *method, GVariant *, GDBusMethodInvocation *invocation) {
    // Activate is a left click; ContextMenu only reaches us where the host doesn't show Menu itself.
    if (g_str_equal(method, "Activate") || g_str_equal(method, "SecondaryActivate")) {
        if (callbacks_.activate) callbacks_.activate();
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

GVariant *Tray::item_property(const char *property) {
    if (g_str_equal(property, "Category")) return g_variant_new_string("SystemServices");
    if (g_str_equal(property, "Id")) return g_variant_new_string("procyon");
    if (g_str_equal(property, "Title")) return g_variant_new_string("Procyon");
    if (g_str_equal(property, "Status")) return g_variant_new_string("Active");
    if (g_str_equal(property, "WindowId")) return g_variant_new_int32(0);
    // Pixmaps rather than an icon name: the icon is the brand mark whether or not Procyon is installed.
    if (g_str_equal(property, "IconName") || g_str_equal(property, "OverlayIconName") ||
        g_str_equal(property, "AttentionIconName"))
        return g_variant_new_string("");
    if (g_str_equal(property, "IconPixmap")) return pixmaps();
    if (g_str_equal(property, "ToolTip")) {
        GVariantBuilder empty;
        g_variant_builder_init(&empty, G_VARIANT_TYPE("a(iiay)"));
        return g_variant_new("(sa(iiay)ss)", "", &empty, "Procyon", tooltip_.c_str());
    }
    if (g_str_equal(property, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    if (g_str_equal(property, "Menu")) return g_variant_new_object_path(kMenuPath);
    if (g_str_equal(property, "XAyatanaLabel")) return g_variant_new_string(label_.c_str());
    if (g_str_equal(property, "XAyatanaLabelGuide")) return g_variant_new_string(label_guide_.c_str());
    if (g_str_equal(property, "XAyatanaOrderingIndex")) return g_variant_new_uint32(0);
    return nullptr;
}

GVariant *Tray::menu_property(const char *property) {
    if (g_str_equal(property, "Version")) return g_variant_new_uint32(3);
    if (g_str_equal(property, "TextDirection")) return g_variant_new_string("ltr");
    if (g_str_equal(property, "Status")) return g_variant_new_string("normal");
    if (g_str_equal(property, "IconThemePath")) return g_variant_new_strv(nullptr, 0);
    return nullptr;
}

void Tray::menu_method(const char *method, GVariant *parameters, GDBusMethodInvocation *invocation) {
    if (g_str_equal(method, "GetLayout")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", revision_, layout()));
    } else if (g_str_equal(method, "Event")) {
        gint32 id = 0;
        const char *event = nullptr;
        g_variant_get(parameters, "(i&svu)", &id, &event, nullptr, nullptr);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        if (g_str_equal(event, "clicked")) {
            // After replying: Quit tears the object down.
            if (id == MenuOpen && callbacks_.activate) callbacks_.activate();
            if (id == MenuPause && callbacks_.toggle_pause) callbacks_.toggle_pause();
            if (id == MenuQuit && callbacks_.quit) callbacks_.quit();
        }
    } else if (g_str_equal(method, "EventGroup")) {
        GVariantIter *events = nullptr;
        g_variant_get(parameters, "(a(isvu))", &events);
        gint32 id = 0;
        const char *event = nullptr;
        std::vector<int> clicked;
        while (g_variant_iter_next(events, "(i&svu)", &id, &event, nullptr, nullptr))
            if (g_str_equal(event, "clicked")) clicked.push_back(id);
        g_variant_iter_free(events);
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
        for (int c : clicked) {
            if (c == MenuOpen && callbacks_.activate) callbacks_.activate();
            if (c == MenuPause && callbacks_.toggle_pause) callbacks_.toggle_pause();
            if (c == MenuQuit && callbacks_.quit) callbacks_.quit();
        }
    } else if (g_str_equal(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
    } else if (g_str_equal(method, "AboutToShowGroup")) {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@ai@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0),
                                      g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
    } else if (g_str_equal(method, "GetGroupProperties")) {
        // Every item with its properties, from the layout (the host asks for the ones it shows).
        GVariant *root = layout();
        GVariantBuilder result;
        g_variant_builder_init(&result, G_VARIANT_TYPE("a(ia{sv})"));
        GVariant *children = g_variant_get_child_value(root, 2);
        for (gsize i = 0; i < g_variant_n_children(children); ++i) {
            GVariant *boxed = g_variant_get_child_value(children, i);
            GVariant *item = g_variant_get_variant(boxed);
            GVariant *props = g_variant_get_child_value(item, 1);
            GVariant *id = g_variant_get_child_value(item, 0);
            g_variant_builder_add(&result, "(i@a{sv})", g_variant_get_int32(id), props);
            g_variant_unref(id);
            g_variant_unref(props);
            g_variant_unref(item);
            g_variant_unref(boxed);
        }
        g_variant_unref(children);
        g_variant_unref(g_variant_ref_sink(root));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", &result));
    } else if (g_str_equal(method, "GetProperty")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", g_variant_new_string("")));
    } else {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
    }
}

}  // namespace procyon::ui
