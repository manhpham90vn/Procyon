// Shared UI foundation for the Win32 app: geometry, theme (design tokens resolved for light or
// dark), formatting, and the Direct2D renderer that draws every component of design/components.md.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Tokens.generated.h"

namespace procyon::ui {

using Microsoft::WRL::ComPtr;

// ---- geometry (DIPs) ----

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    float cx() const { return x + w / 2; }
    float cy() const { return y + h / 2; }
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
    D2D1_RECT_F d2d() const { return D2D1::RectF(x, y, x + w, y + h); }
};

// ---- theme ----

enum class MetricKind { Cpu, Memory, Disk, Network, Gpu, Battery, Energy };
const tokens::MetricStyle &metric_style(MetricKind kind);

D2D1_COLOR_F rgba(uint32_t value);  // 0xRRGGBBAA
D2D1_COLOR_F with_alpha(D2D1_COLOR_F color, float alpha);
D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t);

struct Theme {
    bool dark = false;

    D2D1_COLOR_F color(const tokens::ColorPair &pair) const { return rgba(dark ? pair.dark : pair.light); }
    D2D1_COLOR_F background() const { return color(tokens::palette::background); }
    D2D1_COLOR_F surface() const { return color(tokens::palette::surface); }
    D2D1_COLOR_F surface_raised() const { return color(tokens::palette::surfaceRaised); }
    D2D1_COLOR_F surface_sunken() const { return color(tokens::palette::surfaceSunken); }
    D2D1_COLOR_F border() const { return color(tokens::palette::border); }
    D2D1_COLOR_F border_strong() const { return color(tokens::palette::borderStrong); }
    D2D1_COLOR_F text() const { return color(tokens::palette::textPrimary); }
    D2D1_COLOR_F text_secondary() const { return color(tokens::palette::textSecondary); }
    D2D1_COLOR_F text_tertiary() const { return color(tokens::palette::textTertiary); }
    D2D1_COLOR_F accent() const { return color(tokens::palette::accent); }
    D2D1_COLOR_F success() const { return color(tokens::palette::success); }
    D2D1_COLOR_F warning() const { return color(tokens::palette::warning); }
    D2D1_COLOR_F danger() const { return color(tokens::palette::danger); }
    D2D1_COLOR_F chart_grid() const { return color(tokens::palette::chartGrid); }
    D2D1_COLOR_F track() const { return color(tokens::palette::track); }
    const tokens::ShadowSpec &card_shadow() const {
        return dark ? tokens::shadow::card.dark : tokens::shadow::card.light;
    }
};

// Whether Windows' "Choose your mode" is dark for apps.
bool system_prefers_dark();

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

// ---- renderer ----

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

class Renderer {
public:
    bool init(HWND window);
    void shutdown();
    void resize(UINT width, UINT height);
    // Begins a frame; false when the device is lost (the window then repaints on the next frame).
    bool begin(const Theme &theme);
    bool end();
    void set_dpi(float dpi);
    float dpi() const { return dpi_; }
    ID2D1RenderTarget *target() { return target_.Get(); }
    const Theme &theme() const { return theme_; }
    IDWriteFactory *dwrite() { return dwrite_.Get(); }

    ID2D1SolidColorBrush *brush(D2D1_COLOR_F color);

    void fill(const Rect &r, D2D1_COLOR_F color);
    void fill_round(const Rect &r, float radius, D2D1_COLOR_F color);
    void stroke_round(const Rect &r, float radius, D2D1_COLOR_F color, float width = 1);
    void fill_gradient_round(const Rect &r, float radius, D2D1_COLOR_F start, D2D1_COLOR_F end);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float width = 1, bool dashed = false);
    void fill_circle(float cx, float cy, float radius, D2D1_COLOR_F color);
    void stroke_circle(float cx, float cy, float radius, D2D1_COLOR_F color, float width = 1);
    void push_clip(const Rect &r);
    void pop_clip();

    // Text. Returns the laid-out width.
    float text(std::wstring_view value, const Rect &r, const TextStyle &style, D2D1_COLOR_F color);
    float measure(std::wstring_view value, Font font, float tracking = 0);
    float line_height(Font font);

    // ---- components (design/components.md) ----
    void card(const Rect &r, std::optional<MetricKind> tint = std::nullopt, float radius = tokens::radius::lg);
    // Uppercase tracked caption; `symbol` draws a small glyph in front of it.
    enum class Symbol;
    void panel_caption(const Rect &r, std::wstring_view caption, std::optional<Symbol> symbol = std::nullopt);
    void metric_icon(const Rect &r, MetricKind kind);
    void glyph(MetricKind kind, const Rect &r, D2D1_COLOR_F color);  // the symbol alone
    // Time series, newest on the right; `max` is the scale, values beyond it are clipped.
    struct SparklineOptions {
        bool grid = false;  // 4 dashed lines and a solid baseline
        bool end_dot = true;
        bool halo = true;  // the glow: wider translucent strokes
        bool area = true;
        float line_width = tokens::chart::lineWidth;
        D2D1_COLOR_F *color_override = nullptr;
    };
    // Segmented control: `labels` in a sunken pill, the selected one raised. Returns each
    // segment's rect for hit testing.
    std::vector<Rect> segmented(const Rect &r, const std::vector<std::wstring> &labels, int selected);
    float segmented_width(const std::vector<std::wstring> &labels);
    // Toggle switch, 36x20.
    void toggle_switch(float x, float y, bool on, bool enabled);
    // Light/dark aware icon-only control background on hover.
    void icon_button(const Rect &r, Symbol symbol, D2D1_COLOR_F color, bool hovered, bool enabled = true);
    void sparkline(const Rect &r, const float *values, size_t count, size_t window, float max, MetricKind kind,
                   const SparklineOptions &options = {});
    void ring_gauge(float cx, float cy, float radius, float fraction, MetricKind kind,
                    float width = tokens::chart::gaugeLineWidth);
    void usage_bar(const Rect &r, float fraction, MetricKind kind);
    void usage_bar(const Rect &r, float fraction, D2D1_COLOR_F color);
    struct Segment {
        float fraction;
        D2D1_COLOR_F color;
    };
    void stacked_bar(const Rect &r, const Segment *segments, size_t count);
    enum class Tone { Neutral, Accent, Success, Warning, Danger };
    float badge(float x, float y, std::wstring_view label, Tone tone, bool measure_only = false);
    void live_indicator(float cx, float cy, bool live);
    // 1..5 chevrons and small UI symbols drawn as vectors.
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
        // Page and panel glyphs (stand-ins for the SF Symbols the macOS app uses).
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
        Pin,   // pinned app (Processes)
        Bell,  // alerts
        Peak,  // the busiest minutes (History)
    };
    void symbol(Symbol s, const Rect &r, D2D1_COLOR_F color, float width = 1.5f);

    static D2D1_COLOR_F tone_color(const Theme &theme, Tone tone);

private:
    IDWriteTextFormat *format(Font font);
    ComPtr<IDWriteTextLayout> layout(std::wstring_view value, const TextStyle &style, float width, float height);
    ComPtr<ID2D1PathGeometry> sparkline_path(const Rect &r, const float *values, size_t count, size_t window, float max,
                                             bool closed, float baseline);

    HWND window_ = nullptr;
    float dpi_ = 96;
    Theme theme_;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> dashed_;
    std::unordered_map<int, ComPtr<IDWriteTextFormat>> formats_;
    std::wstring ui_family_, display_family_, mono_family_;
    ComPtr<IDWriteFactory6> dwrite6_;                // variable-font text formats (Windows 10 1809+)
    ComPtr<IDWriteFontCollection2> embedded_fonts_;  // the bundled Inter, empty when it could not load
    int clips_ = 0;
};

// ---- input ----

struct MouseEvent {
    float x = 0, y = 0;
    bool ctrl = false, shift = false;
    int wheel = 0;  // WHEEL_DELTA units, vertical
};

struct KeyEvent {
    UINT vk = 0;
    bool ctrl = false, shift = false, alt = false;
};

// Smallest hit target that still feels right with a mouse.
constexpr float kRowHeight = 28;
constexpr float kControlHeight = 28;

}  // namespace procyon::ui
