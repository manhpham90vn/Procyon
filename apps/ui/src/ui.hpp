// Shared UI foundation for the non-Apple Procyon apps (Windows today, Linux next): geometry,
// colors, the theme (design tokens resolved for light or dark), formatting, the vector path and
// Canvas interface each platform implements, and the Renderer that draws every component of
// design/components.md on top of a Canvas. Nothing in here includes a platform header.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Tokens.generated.h"

namespace procyon::ui {

// ---- geometry (DIPs) ----

struct Point {
    float x = 0, y = 0;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    float cx() const { return x + w / 2; }
    float cy() const { return y + h / 2; }
    Point center() const { return {cx(), cy()}; }
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect inset(float left, float top, float rightInset, float bottomInset) const {
        return {x + left, y + top, w - left - rightInset, h - top - bottomInset};
    }
    Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
    // Takes `height` from the top, leaving the rest in *this.
    Rect take_top(float height) {
        Rect r{x, y, w, height};
        y += height;
        h -= height;
        return r;
    }
    Rect take_bottom(float height) {
        Rect r{x, bottom() - height, w, height};
        h -= height;
        return r;
    }
    Rect take_left(float width) {
        Rect r{x, y, width, h};
        x += width;
        w -= width;
        return r;
    }
    Rect take_right(float width) {
        Rect r{right() - width, y, width, h};
        w -= width;
        return r;
    }
};

// ---- colors ----

// Straight (not premultiplied) RGBA, 0…1.
struct Color {
    float r = 0, g = 0, b = 0, a = 1;
};

namespace colors {
constexpr Color white{1, 1, 1, 1};
constexpr Color black{0, 0, 0, 1};
constexpr Color transparent{0, 0, 0, 0};
}  // namespace colors

Color rgba(uint32_t value);  // 0xRRGGBBAA
Color with_alpha(Color color, float alpha);
Color mix(Color a, Color b, float t);

// ---- theme ----

enum class MetricKind { Cpu, Memory, Disk, Network, Gpu, Battery, Energy };
const tokens::MetricStyle &metric_style(MetricKind kind);

struct Theme {
    bool dark = false;

    Color color(const tokens::ColorPair &pair) const { return rgba(dark ? pair.dark : pair.light); }
    Color background() const { return color(tokens::palette::background); }
    Color surface() const { return color(tokens::palette::surface); }
    Color surface_raised() const { return color(tokens::palette::surfaceRaised); }
    Color surface_sunken() const { return color(tokens::palette::surfaceSunken); }
    Color border() const { return color(tokens::palette::border); }
    Color border_strong() const { return color(tokens::palette::borderStrong); }
    Color text() const { return color(tokens::palette::textPrimary); }
    Color text_secondary() const { return color(tokens::palette::textSecondary); }
    Color text_tertiary() const { return color(tokens::palette::textTertiary); }
    Color accent() const { return color(tokens::palette::accent); }
    Color success() const { return color(tokens::palette::success); }
    Color warning() const { return color(tokens::palette::warning); }
    Color danger() const { return color(tokens::palette::danger); }
    Color chart_grid() const { return color(tokens::palette::chartGrid); }
    Color track() const { return color(tokens::palette::track); }
    const tokens::ShadowSpec &card_shadow() const {
        return dark ? tokens::shadow::card.dark : tokens::shadow::card.light;
    }
};

// ---- formatting (design/components.md: unknown values are an em dash, never 0) ----

namespace fmt {
extern const wchar_t *const unavailable;
std::wstring percent(double fraction, int digits = 0);                       // 0..1 -> "42%"
std::wstring cpu(double percent);                                            // 100 = one core; "12.5%"
std::wstring bytes(double value);                                            // base 1024
std::wstring bytes(int64_t value);                                           // -1 unknown
std::wstring rate(double bytes_per_second);                                  // "1.2 MB/s"
std::wstring count(int64_t value);                                           // "1,234"
std::wstring duration(double seconds);                                       // "2h 15m"
std::wstring temperature(double celsius, bool fahrenheit);                   // "61°C"
std::wstring watts(double value);                                            // "4.2 W"
std::wstring interval(double seconds);                                       // "1s", "0.5s"
std::wstring number(double value, int digits);                               // "12.5"
std::wstring time_of_day(int64_t unix_seconds);                              // "14:05"
std::wstring date_time(int64_t unix_seconds);                                // "Oct 3, 2026 14:05"
std::pair<std::wstring, std::wstring> split_unit(const std::wstring &text);  // "1.2 GB" -> ("1.2", "GB")
std::wstring from_utf8(std::string_view text);
std::string to_utf8(std::wstring_view text);
}  // namespace fmt

// ---- text ----

enum class Font {
    Display,
    Title,
    Headline,
    Body,
    BodyMedium,
    BodySemibold,
    Label,
    LabelSemibold,
    Caption,
    Metric,
    Mono,
    Stat,   // 17 semibold rounded: StatGrid values, hero gauge labels
    Brand,  // 15 bold rounded: the sidebar wordmark
};
// Size, weight and design of a Font, from the tokens.
tokens::FontSpec font_spec(Font font);

enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Center, Bottom };

struct TextStyle {
    Font font = Font::Body;
    HAlign halign = HAlign::Left;
    VAlign valign = VAlign::Center;
    bool trim = true;      // ellipsis when the text doesn't fit
    bool tabular = false;  // tabular figures for columns of numbers
    bool wrap = false;
    bool uppercase = false;
    float tracking = 0;  // extra letter spacing in DIPs (captions)
};

// ---- vector paths ----

// A path in DIPs, built the way SVG and every 2D API build one. Arcs use the SVG endpoint form,
// which Direct2D takes as is and Cairo/Skia convert to a centre form.
class Path {
public:
    enum class Op : uint8_t { Move, Line, Quad, Cubic, Arc, Close };
    struct Command {
        Op op;
        Point p;                             // end point (Move, Line, Quad, Cubic, Arc)
        Point c1, c2;                        // control points (Quad uses c1; Cubic uses both)
        float rx = 0, ry = 0, rotation = 0;  // Arc: radii and x-axis rotation in degrees
        bool large = false, sweep = false;   // Arc: SVG large-arc and sweep (clockwise on screen) flags
    };

    void move_to(Point p) { commands_.push_back({Op::Move, p, {}, {}}); }
    void line_to(Point p) { commands_.push_back({Op::Line, p, {}, {}}); }
    void quad_to(Point control, Point p) { commands_.push_back({Op::Quad, p, control, {}}); }
    void cubic_to(Point c1, Point c2, Point p) { commands_.push_back({Op::Cubic, p, c1, c2}); }
    void arc_to(Point p, float rx, float ry, float rotation, bool large, bool sweep) {
        commands_.push_back({Op::Arc, p, {}, {}, rx, ry, rotation, large, sweep});
    }
    void close() { commands_.push_back({Op::Close, {}, {}, {}}); }

    // Closed figures.
    void add_rect(const Rect &r);
    // `continuous` corners are the squircle of SwiftUI's RoundedRectangle(style: .continuous); the
    // default is circular.
    void add_round_rect(const Rect &r, float radius, bool continuous = false);
    void add_ellipse(Point center, float rx, float ry);
    void add_circle(Point center, float radius) { add_ellipse(center, radius, radius); }
    // Open arc of a circle from `start_angle` sweeping `sweep` (radians, clockwise on screen, 0 at
    // 3 o'clock).
    void add_arc(Point center, float radius, float start_angle, float sweep);

    // The smooth time-series line through `points` (quadratic curves through the midpoints, as
    // Sparkline.swift draws it). `closed` adds the baseline edges for an area fill.
    static Path smooth(const std::vector<Point> &points, bool closed = false, float baseline = 0);

    bool empty() const { return commands_.empty(); }
    const std::vector<Command> &commands() const { return commands_; }
    // Translates and scales every point: for icons drawn from a fixed grid.
    Path transformed(float scale, Point offset) const;

private:
    std::vector<Command> commands_;
};

// Two-stop linear gradient between two points.
struct Gradient {
    Point start, end;
    Color from, to;
};

enum class Dash { None, Dotted, Dashed };

struct Stroke {
    float width = 1;
    bool round = true;  // round caps and joins (the default everywhere the macOS app strokes)
    Dash dash = Dash::None;
};

// A raster image (an app icon): premultiplied BGRA rows, top-down. `id` is unique for the life
// of the process, so backends can cache their copy of the pixels by it.
struct Image {
    uint64_t id = 0;
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;
};

// Blurred, offset shadow under a rounded rectangle (tokens.shadow).
struct Shadow {
    Color color;  // with the opacity in `a`
    float blur;   // radius, as SwiftUI's shadow(radius:)
    float dy;     // vertical offset
};

// ---- canvas: what a platform backend implements (Direct2D, Cairo, Skia …) ----

class Canvas {
public:
    virtual ~Canvas() = default;

    virtual float dpi() const = 0;

    virtual void fill_rect(const Rect &r, Color color) = 0;
    virtual void fill_round_rect(const Rect &r, float radius, Color color) = 0;
    virtual void fill_round_rect(const Rect &r, float radius, const Gradient &gradient) = 0;
    virtual void stroke_round_rect(const Rect &r, float radius, Color color, float width) = 0;
    virtual void fill_ellipse(Point center, float rx, float ry, Color color) = 0;
    virtual void stroke_ellipse(Point center, float rx, float ry, Color color, float width) = 0;
    virtual void line(Point a, Point b, Color color, float width, Dash dash) = 0;
    virtual void fill_path(const Path &path, Color color) = 0;
    virtual void fill_path(const Path &path, const Gradient &gradient) = 0;
    virtual void stroke_path(const Path &path, Color color, const Stroke &stroke) = 0;
    virtual void stroke_path(const Path &path, const Gradient &gradient, const Stroke &stroke) = 0;
    // A real Gaussian shadow under a rounded rectangle. Static content only (rule 4 of
    // components.md): backends cache it by size.
    virtual void shadow(const Rect &r, float radius, const Shadow &shadow) = 0;
    // Draws `image` scaled into `r`.
    virtual void draw_image(const Image &image, const Rect &r) = 0;

    virtual void push_clip(const Rect &r) = 0;
    virtual void pop_clip() = 0;
    // Everything until pop_mask is clipped to `path`.
    virtual void push_mask(const Path &path) = 0;
    virtual void pop_mask() = 0;
    // Scales everything until pop_transform around `about`.
    virtual void push_scale(float scale, Point about) = 0;
    virtual void pop_transform() = 0;

    // Draws `text` in `r`; returns the laid-out width.
    virtual float draw_text(std::wstring_view text, const Rect &r, const TextStyle &style, Color color) = 0;
    virtual float measure_text(std::wstring_view text, Font font, float tracking) = 0;
    virtual float line_height(Font font) = 0;
};

// ---- renderer: the component vocabulary of design/components.md over a Canvas ----

class Renderer {
public:
    explicit Renderer(Canvas &canvas) : canvas_(canvas) {}

    Canvas &canvas() { return canvas_; }
    void set_theme(const Theme &theme) { theme_ = theme; }
    const Theme &theme() const { return theme_; }
    float dpi() const { return canvas_.dpi(); }

    // ---- primitives ----
    void fill(const Rect &r, Color color);
    // Rounded rectangles with radius >= 8 that aren't capsules get continuous (squircle) corners,
    // as every card, tile and sidebar row on macOS has.
    void fill_round(const Rect &r, float radius, Color color);
    void stroke_round(const Rect &r, float radius, Color color, float width = 1);
    void fill_gradient_round(const Rect &r, float radius, Color start, Color end);
    void line(float x1, float y1, float x2, float y2, Color color, float width = 1, bool dashed = false);
    void fill_circle(float cx, float cy, float radius, Color color);
    void stroke_circle(float cx, float cy, float radius, Color color, float width = 1);
    void fill_path(const Path &path, Color color) { canvas_.fill_path(path, color); }
    void fill_path(const Path &path, const Gradient &gradient) { canvas_.fill_path(path, gradient); }
    void stroke_path(const Path &path, Color color, const Stroke &stroke) { canvas_.stroke_path(path, color, stroke); }
    void push_clip(const Rect &r);
    void pop_clip();

    // Text. Returns the laid-out width.
    float text(std::wstring_view value, const Rect &r, const TextStyle &style, Color color);
    float measure(std::wstring_view value, Font font, float tracking = 0);
    float line_height(Font font);

    // ---- components (design/components.md) ----
    void card(const Rect &r, std::optional<MetricKind> tint = std::nullopt, float radius = tokens::radius::lg);
    // Uppercase tracked caption; `symbol` draws a small glyph in front of it.
    enum class Symbol;
    void panel_caption(const Rect &r, std::wstring_view caption, std::optional<Symbol> symbol = std::nullopt);
    void metric_icon(const Rect &r, MetricKind kind);
    void glyph(MetricKind kind, const Rect &r, Color color);  // the symbol alone
    // An app's icon (ProcessIcon / AppIconView): the icon of the executable or bundle at `path`,
    // else a sunken tile with a terminal glyph, or a cog for `system` processes.
    void app_icon(std::wstring_view path, const Rect &r, bool system = false);
    // Time series, newest on the right; `max` is the scale, values beyond it are clipped.
    struct SparklineOptions {
        bool grid = false;  // 4 dashed lines and a solid baseline
        bool end_dot = true;
        bool halo = true;  // the glow: wider translucent strokes
        bool area = true;
        float line_width = tokens::chart::lineWidth;
        Color *color_override = nullptr;
    };
    // Segmented control: `labels` in a sunken pill, the selected one raised. Returns each
    // segment's rect for hit testing.
    std::vector<Rect> segmented(const Rect &r, const std::vector<std::wstring> &labels, int selected);
    float segmented_width(const std::vector<std::wstring> &labels);
    // Toggle switch, 36x20.
    void toggle_switch(float x, float y, bool on, bool enabled);
    // Light/dark aware icon-only control background on hover.
    void icon_button(const Rect &r, Symbol symbol, Color color, bool hovered, bool enabled = true);
    void sparkline(const Rect &r, const float *values, size_t count, size_t window, float max, MetricKind kind,
                   const SparklineOptions &options);
    // An overload rather than `options = {}`: GCC rejects a default argument built from a nested
    // struct with default member initializers while the enclosing class is incomplete.
    void sparkline(const Rect &r, const float *values, size_t count, size_t window, float max, MetricKind kind) {
        sparkline(r, values, count, window, max, kind, SparklineOptions{});
    }
    void ring_gauge(float cx, float cy, float radius, float fraction, MetricKind kind,
                    float width = tokens::chart::gaugeLineWidth);
    void usage_bar(const Rect &r, float fraction, MetricKind kind);
    void usage_bar(const Rect &r, float fraction, Color color);
    struct Segment {
        float fraction;
        Color color;
    };
    void stacked_bar(const Rect &r, const Segment *segments, size_t count);
    enum class Tone { Neutral, Accent, Success, Warning, Danger };
    float badge(float x, float y, std::wstring_view label, Tone tone, bool measure_only = false);
    void live_indicator(float cx, float cy, bool live);
    // Small UI symbols (Lucide icons, the stand-ins for the SF Symbols the macOS app uses).
    enum class Symbol {
        ChevronRight,
        ChevronDown,
        Search,
        Close,
        Lock,
        Check,
        Pause,
        Play,
        Gear,
        Info,
        Warning,
        Plus,
        Minus,
        Dot,
        // Page and panel glyphs.
        Grid,      // Overview
        List,      // Processes
        Power,     // Startup
        Gears,     // Services
        Ports,     // Files & Ports
        Desktop,   // System
        Clock,     // History / uptime
        Sparkle,   // brand
        Waveform,  // utilization panels
        Flame,     // top CPU
        Bullets,   // details
        Drive,     // volumes
        Thermo,    // temperature
        Refresh,
        Stack,  // processes count badge
        Gauge,  // memory pressure badge
        Expand,
        Collapse,
        Pin,       // pinned app (Processes)
        Bell,      // alerts
        Peak,      // the busiest minutes (History)
        Terminal,  // a process without an app icon
        // Window caption buttons.
        WindowMinimize,
        WindowMaximize,
        WindowRestore,
    };
    void symbol(Symbol s, const Rect &r, Color color, float width = 1.5f);

    static Color tone_color(const Theme &theme, Tone tone);

private:
    void sparkline_points(const Rect &r, const float *values, size_t count, size_t window, float max,
                          std::vector<Point> &out) const;

    Canvas &canvas_;
    Theme theme_;
    int clips_ = 0;
    // App icons by "path#pixels"; a null entry remembers that there is none.
    std::unordered_map<std::wstring, std::shared_ptr<const Image>> app_icons_;
};

// ---- input ----

struct MouseEvent {
    float x = 0, y = 0;
    bool ctrl = false, shift = false;
    int wheel = 0;  // 120 per notch, vertical, positive away from the user
};
constexpr int kWheelNotch = 120;

enum class Key {
    None,
    Escape,
    Back,
    Delete,
    Return,
    Space,
    Tab,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    Menu,  // the context menu key
};

struct KeyEvent {
    Key key = Key::None;
    wchar_t ch = 0;  // the key's upper-case letter or digit for shortcuts (Ctrl+V), 0 otherwise
    bool ctrl = false, shift = false, alt = false;
};

enum class Cursor { Arrow, Hand, IBeam };

// Smallest hit target that still feels right with a mouse.
constexpr float kRowHeight = 28;
constexpr float kControlHeight = 28;

}  // namespace procyon::ui
