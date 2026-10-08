// The component vocabulary of design/components.md, drawn with Canvas primitives. Platform
// independent: the Direct2D (Windows) and Cairo (Linux) backends only implement Canvas.
#include <algorithm>
#include <cmath>

#include "icons.hpp"
#include "platform.hpp"
#include "ui.hpp"

namespace procyon::ui {

namespace {
constexpr float kPi = 3.14159265358979f;
// Corner smoothing of SwiftUI's .continuous corners (Figma's 60% matches iOS).
constexpr float kContinuousSmoothing = 0.6f;
// Rounded rectangles this round or rounder get continuous corners; smaller radii look the same.
constexpr float kContinuousMinRadius = 8;
}  // namespace

// ---------------------------------------------------------------------------------------------
// Colors and tokens
// ---------------------------------------------------------------------------------------------

Color rgba(uint32_t value) {
    return {((value >> 24) & 0xFF) / 255.0f, ((value >> 16) & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f,
            (value & 0xFF) / 255.0f};
}

Color with_alpha(Color color, float alpha) {
    color.a = alpha;
    return color;
}

Color mix(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

const tokens::MetricStyle &metric_style(MetricKind kind) {
    switch (kind) {
        case MetricKind::Cpu: return tokens::metric::cpu;
        case MetricKind::Memory: return tokens::metric::memory;
        case MetricKind::Disk: return tokens::metric::disk;
        case MetricKind::Network: return tokens::metric::network;
        case MetricKind::Gpu: return tokens::metric::gpu;
        case MetricKind::Battery: return tokens::metric::battery;
        case MetricKind::Energy: return tokens::metric::energy;
    }
    return tokens::metric::cpu;
}

tokens::FontSpec font_spec(Font font) {
    using namespace tokens::font;
    switch (font) {
        case Font::Display: return display;
        case Font::Title: return title;
        case Font::Headline: return headline;
        case Font::Body: return body;
        case Font::BodyMedium: return {body.size, 500, body.design};
        case Font::BodySemibold: return {body.size, 600, body.design};
        case Font::Label: return label;
        case Font::LabelSemibold: return {label.size, 600, label.design};
        case Font::Caption: return caption;
        case Font::Metric: return metric;
        case Font::Mono: return mono;
        case Font::Stat: return {17, 600, tokens::FontDesign::Rounded};
        case Font::Brand: return {15, 700, tokens::FontDesign::Rounded};
    }
    return body;
}

// ---------------------------------------------------------------------------------------------
// Path
// ---------------------------------------------------------------------------------------------

void Path::add_rect(const Rect &r) {
    move_to({r.x, r.y});
    line_to({r.right(), r.y});
    line_to({r.right(), r.bottom()});
    line_to({r.x, r.bottom()});
    close();
}

void Path::add_round_rect(const Rect &r, float radius, bool continuous) {
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    if (radius <= 0) {
        add_rect(r);
        return;
    }
    if (!continuous || radius >= std::min(r.w, r.h) / 2 - 0.01f) {
        // Circular corners: four quarter arcs, clockwise from the top edge.
        move_to({r.x + radius, r.y});
        line_to({r.right() - radius, r.y});
        arc_to({r.right(), r.y + radius}, radius, radius, 0, false, true);
        line_to({r.right(), r.bottom() - radius});
        arc_to({r.right() - radius, r.bottom()}, radius, radius, 0, false, true);
        line_to({r.x + radius, r.bottom()});
        arc_to({r.x, r.bottom() - radius}, radius, radius, 0, false, true);
        line_to({r.x, r.y + radius});
        arc_to({r.x + radius, r.y}, radius, radius, 0, false, true);
        close();
        return;
    }
    // Continuous corners (the iOS squircle), after Figma's corner smoothing: each corner spans
    // p = (1 + smoothing) * radius along both edges and is a cubic, a short circular arc and a
    // mirrored cubic, so the curvature eases in instead of jumping as a circle's does.
    float smoothing = kContinuousSmoothing;
    const float max_span = std::min(r.w, r.h) / 2;
    if ((1 + smoothing) * radius > max_span) smoothing = std::max(0.0f, max_span / radius - 1);
    const float p = (1 + smoothing) * radius;
    const auto rad = [](float degrees) { return degrees * kPi / 180; };
    const float arc_measure = 90 * (1 - smoothing);
    const float arc_section = std::sin(rad(arc_measure / 2)) * radius * std::sqrt(2.0f);
    const float angle_alpha = (90 - arc_measure) / 2;
    const float p3_to_p4 = radius * std::tan(rad(angle_alpha / 2));
    const float angle_beta = 45 * smoothing;
    const float c = p3_to_p4 * std::cos(rad(angle_beta));
    const float d = c * std::tan(rad(angle_beta));
    const float b = (p - arc_section - c - d) / 3;
    const float a = 2 * b;
    // One corner in a local frame: the path arrives along -u (u points back along the incoming
    // edge from the corner) and leaves along v. `start` is the corner point plus u * p.
    const auto corner = [&](Point corner_point, Point u, Point v) {
        const Point start{corner_point.x + u.x * p, corner_point.y + u.y * p};
        const auto at = [&](float lx, float ly) {
            return Point{start.x - u.x * lx + v.x * ly, start.y - u.y * lx + v.y * ly};
        };
        line_to(start);
        cubic_to(at(a, 0), at(a + b, 0), at(a + b + c, d));
        const Point arc_end = at(a + b + c + arc_section, d + arc_section);
        arc_to(arc_end, radius, radius, 0, false, true);
        const float ex = a + b + c + arc_section, ey = d + arc_section;
        cubic_to(at(ex + d, ey + c), at(ex + d, ey + b + c), at(ex + d, ey + a + b + c));
    };
    move_to({r.x + p, r.y});
    corner({r.right(), r.y}, {-1, 0}, {0, 1});
    corner({r.right(), r.bottom()}, {0, -1}, {-1, 0});
    corner({r.x, r.bottom()}, {1, 0}, {0, -1});
    corner({r.x, r.y}, {0, 1}, {1, 0});
    close();
}

void Path::add_ellipse(Point center, float rx, float ry) {
    move_to({center.x + rx, center.y});
    arc_to({center.x - rx, center.y}, rx, ry, 0, false, true);
    arc_to({center.x + rx, center.y}, rx, ry, 0, false, true);
    close();
}

void Path::add_arc(Point center, float radius, float start_angle, float sweep) {
    const Point from{center.x + radius * std::cos(start_angle), center.y + radius * std::sin(start_angle)};
    const float end_angle = start_angle + sweep;
    const Point to{center.x + radius * std::cos(end_angle), center.y + radius * std::sin(end_angle)};
    move_to(from);
    arc_to(to, radius, radius, 0, std::fabs(sweep) > kPi, sweep > 0);
}

Path Path::smooth(const std::vector<Point> &points, bool closed, float baseline) {
    Path path;
    if (points.empty()) return path;
    if (closed) {
        path.move_to({points.front().x, baseline});
        path.line_to(points.front());
    } else {
        path.move_to(points.front());
    }
    // Quadratic curves through the midpoints, with the sample as the control point (Sparkline.swift).
    for (size_t i = 1; i < points.size(); ++i) {
        const Point previous = points[i - 1], current = points[i];
        const Point mid{(previous.x + current.x) / 2, (previous.y + current.y) / 2};
        path.quad_to(previous, mid);
        if (i == points.size() - 1) path.line_to(current);
    }
    if (closed) {
        path.line_to({points.back().x, baseline});
        path.close();
    }
    return path;
}

Path Path::transformed(float scale, Point offset) const {
    Path out;
    out.commands_.reserve(commands_.size());
    const auto map = [&](Point p) { return Point{p.x * scale + offset.x, p.y * scale + offset.y}; };
    for (Command c : commands_) {
        c.p = map(c.p);
        c.c1 = map(c.c1);
        c.c2 = map(c.c2);
        c.rx *= scale;
        c.ry *= scale;
        out.commands_.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Renderer: primitives
// ---------------------------------------------------------------------------------------------

namespace {

bool continuous_corners(const Rect &r, float radius) {
    return radius >= kContinuousMinRadius && radius < std::min(r.w, r.h) / 2 - 0.5f;
}

}  // namespace

void Renderer::fill(const Rect &r, Color color) {
    if (r.empty() || color.a <= 0) return;
    canvas_.fill_rect(r, color);
}

void Renderer::fill_round(const Rect &r, float radius, Color color) {
    if (r.empty() || color.a <= 0) return;
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    if (continuous_corners(r, radius)) {
        Path path;
        path.add_round_rect(r, radius, true);
        canvas_.fill_path(path, color);
        return;
    }
    canvas_.fill_round_rect(r, radius, color);
}

void Renderer::stroke_round(const Rect &r, float radius, Color color, float width) {
    if (r.empty() || color.a <= 0) return;
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    const Rect inner = r.inset(width / 2, width / 2);
    if (continuous_corners(r, radius)) {
        Path path;
        path.add_round_rect(inner, std::max(0.0f, radius - width / 2), true);
        Stroke stroke;
        stroke.width = width;
        canvas_.stroke_path(path, color, stroke);
        return;
    }
    canvas_.stroke_round_rect(inner, radius, color, width);
}

void Renderer::fill_gradient_round(const Rect &r, float radius, Color start, Color end) {
    if (r.empty()) return;
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    const Gradient gradient{{r.x, r.y}, {r.right(), r.bottom()}, start, end};
    if (continuous_corners(r, radius)) {
        Path path;
        path.add_round_rect(r, radius, true);
        canvas_.fill_path(path, gradient);
        return;
    }
    canvas_.fill_round_rect(r, radius, gradient);
}

void Renderer::line(float x1, float y1, float x2, float y2, Color color, float width, bool dashed) {
    canvas_.line({x1, y1}, {x2, y2}, color, width, dashed ? Dash::Dotted : Dash::None);
}

void Renderer::fill_circle(float cx, float cy, float radius, Color color) {
    canvas_.fill_ellipse({cx, cy}, radius, radius, color);
}

void Renderer::stroke_circle(float cx, float cy, float radius, Color color, float width) {
    canvas_.stroke_ellipse({cx, cy}, radius, radius, color, width);
}

void Renderer::push_clip(const Rect &r) {
    canvas_.push_clip(r);
    ++clips_;
}

void Renderer::pop_clip() {
    if (clips_ <= 0) return;
    canvas_.pop_clip();
    --clips_;
}

float Renderer::text(std::wstring_view value, const Rect &r, const TextStyle &style, Color color) {
    if (value.empty() || r.empty()) return 0;
    return canvas_.draw_text(value, r, style, color);
}

float Renderer::measure(std::wstring_view value, Font font, float tracking) {
    if (value.empty()) return 0;
    return canvas_.measure_text(value, font, tracking);
}

float Renderer::line_height(Font font) { return canvas_.line_height(font); }

// ---------------------------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------------------------

void Renderer::card(const Rect &r, std::optional<MetricKind> tint, float radius) {
    if (r.empty()) return;
    // The shadow sits on the static card background only (rule 4), never on live content.
    const tokens::ShadowSpec &spec = theme_.card_shadow();
    canvas_.shadow(r, radius, Shadow{with_alpha(rgba(spec.color), spec.opacity), spec.radius, spec.y});
    fill_round(r, radius, theme_.surface());
    if (tint) {
        const tokens::MetricStyle &style = metric_style(*tint);
        const float strength = theme_.dark ? 0.14f : 0.08f;
        fill_gradient_round(r, radius, with_alpha(rgba(style.start), strength), with_alpha(rgba(style.end), 0));
    }
    stroke_round(r, radius, theme_.border(), 1);
}

void Renderer::panel_caption(const Rect &r, std::wstring_view caption, std::optional<Symbol> symbol_) {
    Rect row = r;
    if (symbol_) {
        const Rect g = row.take_left(12 + tokens::space::xs + 2);
        symbol(*symbol_, Rect{g.x, g.cy() - 6, 12, 12}, theme_.text_tertiary(), 1.3f);
    }
    TextStyle style;
    style.font = Font::Caption;
    style.uppercase = true;
    style.tracking = 0.8f;
    text(caption, row, style, theme_.text_tertiary());
}

void Renderer::metric_icon(const Rect &r, MetricKind kind) {
    const tokens::MetricStyle &style = metric_style(kind);
    const float radius = r.w * 0.28f;
    // Soft colored shadow (MetricIcon: start color at 35%, radius 18% of the size, 6% down).
    canvas_.shadow(r, radius, Shadow{with_alpha(rgba(style.start), 0.35f), r.w * 0.18f, r.w * 0.06f});
    fill_gradient_round(r, radius, rgba(style.start), rgba(style.end));
    stroke_round(r, radius, with_alpha(colors::white, 0.25f), 0.5f);
    const Rect glyph_rect = r.inset(r.w * 0.22f, r.h * 0.22f);
    // The symbol's hairline shadow, then the symbol.
    glyph(kind, glyph_rect.offset(0, r.w * 0.02f), with_alpha(colors::black, 0.2f));
    glyph(kind, glyph_rect, colors::white);
}

namespace {

const char *metric_icon_name(MetricKind kind) {
    switch (kind) {
        case MetricKind::Cpu: return "cpu";
        case MetricKind::Memory: return "memory-stick";
        case MetricKind::Disk: return "hard-drive";
        case MetricKind::Network: return "network";
        case MetricKind::Gpu: return "box";
        case MetricKind::Battery: return "battery-full";
        case MetricKind::Energy: return "zap";
    }
    return "cpu";
}

struct SymbolSpec {
    const char *icon;
    bool filled;  // the closed figures are filled too (the .fill variants of SF Symbols)
};

SymbolSpec symbol_spec(Renderer::Symbol s) {
    using S = Renderer::Symbol;
    switch (s) {
        case S::ChevronRight: return {"chevron-right", false};
        case S::ChevronDown: return {"chevron-down", false};
        case S::Search: return {"search", false};
        case S::Close: return {"x", false};
        case S::Lock: return {"lock", false};
        case S::Check: return {"check", false};
        case S::Pause: return {"pause", true};
        case S::Play: return {"play", true};
        case S::Gear: return {"settings", false};
        case S::Info: return {"info", false};
        case S::Warning: return {"triangle-alert", false};
        case S::Plus: return {"plus", false};
        case S::Minus: return {"minus", false};
        case S::Dot: return {nullptr, true};
        case S::Grid: return {"layout-grid", true};
        case S::List: return {"list", false};
        case S::Power: return {"power", false};
        case S::Gears: return {"cog", false};
        case S::Ports: return {"ethernet-port", false};
        case S::Desktop: return {"monitor", false};
        case S::Clock: return {"clock", false};
        case S::Sparkle: return {"sparkle", true};
        case S::Waveform: return {"activity", false};
        case S::Flame: return {"flame", true};
        case S::Bullets: return {"text-align-start", false};
        case S::Drive: return {"hard-drive", false};
        case S::Thermo: return {"thermometer", false};
        case S::Refresh: return {"rotate-cw", false};
        case S::Stack: return {"layers", false};
        case S::Gauge: return {"gauge", false};
        case S::Expand: return {"chevrons-up-down", false};
        case S::Collapse: return {"chevrons-down-up", false};
        case S::Pin: return {"pin", true};
        case S::Bell: return {"bell", false};
        case S::Peak: return {"trending-up", false};
        case S::Terminal: return {"terminal", false};
        case S::WindowMinimize: return {"minus", false};
        case S::WindowMaximize: return {"square", false};
        case S::WindowRestore: return {"copy", false};
    }
    return {"info", false};
}

// Fits the 24-grid icon into `r`, centred.
void draw_icon(Canvas &canvas, const icons::Icon &icon, const Rect &r, Color color, float width, bool filled) {
    const float scale = std::min(r.w, r.h) / 24;
    const Point offset{r.cx() - 12 * scale, r.cy() - 12 * scale};
    if (filled && !icon.closed.empty()) canvas.fill_path(icon.closed.transformed(scale, offset), color);
    Stroke stroke;
    stroke.width = width;
    canvas.stroke_path(icon.outline.transformed(scale, offset), color, stroke);
}

}  // namespace

void Renderer::glyph(MetricKind kind, const Rect &r, Color color) {
    const icons::Icon *icon = icons::lucide(metric_icon_name(kind));
    if (!icon) return;
    // Lucide's 2-of-24 stroke, a touch heavier for the semibold SF Symbols the tiles carry.
    draw_icon(canvas_, *icon, r, color, std::max(1.0f, r.w / 24 * 2.3f), kind == MetricKind::Energy);
}

void Renderer::symbol(Symbol s, const Rect &r, Color color, float width) {
    const SymbolSpec spec = symbol_spec(s);
    if (!spec.icon) {  // Dot
        fill_circle(r.cx(), r.cy(), std::min(r.w, r.h) * 0.175f, color);
        return;
    }
    const icons::Icon *icon = icons::lucide(spec.icon);
    if (!icon) return;
    draw_icon(canvas_, *icon, r, color, width, spec.filled);
}

void Renderer::app_icon(std::wstring_view path, const Rect &r, bool system) {
    if (r.empty()) return;
    const float size = std::min(r.w, r.h);
    const Rect box{r.cx() - size / 2, r.cy() - size / 2, size, size};
    if (!path.empty()) {
        const int pixels = static_cast<int>(std::lround(size * canvas_.dpi() / 96));
        std::wstring key(path);
        key += L'#';
        key += std::to_wstring(pixels);
        auto it = app_icons_.find(key);
        if (it == app_icons_.end()) {
            if (app_icons_.size() > 2048) app_icons_.clear();
            it = app_icons_.emplace(std::move(key), platform::app_icon(path, pixels)).first;
        }
        if (it->second) {
            canvas_.draw_image(*it->second, box);
            return;
        }
    }
    // ProcessIcon's stand-in: a sunken continuous tile with a terminal (or cog) glyph.
    const float radius = size * 0.25f;
    fill_round(box, radius, theme_.surface_sunken());
    stroke_round(box, radius, theme_.border(), 0.5f);
    const float glyph_size = size * 0.6f;
    symbol(system ? Symbol::Gears : Symbol::Terminal,
           Rect{box.cx() - glyph_size / 2, box.cy() - glyph_size / 2, glyph_size, glyph_size}, theme_.text_tertiary(),
           std::max(1.0f, size / 12));
}

void Renderer::sparkline_points(const Rect &r, const float *values, size_t count, size_t window, float max,
                                std::vector<Point> &out) const {
    out.clear();
    if (count == 0 || window < 2) return;
    const float step = r.w / static_cast<float>(window - 1);
    const size_t first_slot = count >= window ? 0 : window - count;
    const size_t first = count >= window ? count - window : 0;
    const size_t n = std::min(count, window);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const float v = std::clamp(max > 0 ? values[first + i] / max : 0.0f, 0.0f, 1.0f);
        out.push_back({r.x + step * static_cast<float>(first_slot + i), r.bottom() - v * r.h});
    }
}

void Renderer::sparkline(const Rect &r, const float *values, size_t count, size_t window, float max, MetricKind kind,
                         const SparklineOptions &options) {
    if (r.empty()) return;
    const tokens::MetricStyle &style = metric_style(kind);
    const Color start = options.color_override ? *options.color_override : rgba(style.start);
    if (options.grid) {
        // Dashed inner lines and a solid baseline.
        for (int i = 0; i < 4; ++i) {
            const float gy = r.y + (r.h - options.line_width) * i / 4 + options.line_width / 2;
            line(r.x, gy, r.right(), gy, theme_.chart_grid(), 1, true);
        }
        line(r.x, r.bottom() - options.line_width / 2, r.right(), r.bottom() - options.line_width / 2,
             theme_.chart_grid(), 1);
    }
    if (count == 0) return;
    const Rect plot = r.inset(0, options.line_width / 2);
    std::vector<Point> points;
    sparkline_points(plot, values, count, window, max, points);
    if (points.empty()) return;
    push_clip(r.inset(-options.line_width * 4, -options.line_width * 4));
    if (options.area) {
        const Path area = Path::smooth(points, true, r.bottom());
        canvas_.fill_path(area, Gradient{{r.x, r.y},
                                         {r.x, r.bottom()},
                                         with_alpha(start, tokens::chart::fillOpacityTop),
                                         with_alpha(start, tokens::chart::fillOpacityBottom)});
    }
    const Path stroke = Path::smooth(points);
    Stroke pen;
    if (options.halo) {
        // A glow is wider translucent strokes, never a blur (rule 4).
        pen.width = options.line_width + tokens::chart::glowRadius * 1.6f;
        canvas_.stroke_path(stroke, with_alpha(start, 0.08f), pen);
        pen.width = options.line_width + tokens::chart::glowRadius * 0.8f;
        canvas_.stroke_path(stroke, with_alpha(start, 0.14f), pen);
    }
    pen.width = options.line_width;
    canvas_.stroke_path(stroke, start, pen);
    if (options.end_dot) {
        // The newest sample always sits at the right edge.
        fill_circle(points.back().x, points.back().y, options.line_width * 1.4f, start);
    }
    pop_clip();
}

void Renderer::ring_gauge(float cx, float cy, float radius, float fraction, MetricKind kind, float width) {
    const tokens::MetricStyle &style = metric_style(kind);
    stroke_circle(cx, cy, radius, theme_.track(), width);
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction <= 0) return;
    const float sweep = std::max(0.001f, std::min(fraction, 0.9999f)) * 2 * kPi;
    Path arc;
    arc.add_arc({cx, cy}, radius, -kPi / 2, sweep);  // from 12 o'clock, clockwise
    const Gradient gradient{{cx - radius, cy - radius}, {cx + radius, cy + radius}, rgba(style.start), rgba(style.end)};
    Stroke pen;
    // Halo: a wider translucent arc in the end color.
    pen.width = width * 1.9f;
    canvas_.stroke_path(arc, with_alpha(rgba(style.end), 0.18f), pen);
    pen.width = width;
    canvas_.stroke_path(arc, gradient, pen);
}

void Renderer::usage_bar(const Rect &r, float fraction, MetricKind kind) {
    const tokens::MetricStyle &style = metric_style(kind);
    fill_round(r, tokens::radius::pill, theme_.track());
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction <= 0) return;
    Rect filled = r;
    filled.w = std::max(r.h, r.w * fraction);
    fill_gradient_round(filled, tokens::radius::pill, rgba(style.start), rgba(style.end));
}

void Renderer::usage_bar(const Rect &r, float fraction, Color color) {
    fill_round(r, tokens::radius::pill, theme_.track());
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction <= 0) return;
    Rect filled = r;
    filled.w = std::max(r.h, r.w * fraction);
    fill_round(filled, tokens::radius::pill, color);
}

void Renderer::stacked_bar(const Rect &r, const Segment *segments, size_t count) {
    fill_round(r, tokens::radius::sm, theme_.track());
    Path mask;
    mask.add_round_rect(r, std::min(r.h / 2, tokens::radius::sm));
    canvas_.push_mask(mask);
    float x = r.x;
    for (size_t i = 0; i < count; ++i) {
        const float w = r.w * std::clamp(segments[i].fraction, 0.0f, 1.0f);
        if (w <= 0) continue;
        fill_round(Rect{x, r.y, std::max(0.0f, w - tokens::space::xxs), r.h}, tokens::radius::xs, segments[i].color);
        x += w;
    }
    canvas_.pop_mask();
}

Color Renderer::tone_color(const Theme &theme, Tone tone) {
    switch (tone) {
        case Tone::Accent: return theme.accent();
        case Tone::Success: return theme.success();
        case Tone::Warning: return theme.warning();
        case Tone::Danger: return theme.danger();
        default: return theme.text_secondary();
    }
}

float Renderer::badge(float x, float y, std::wstring_view label, Tone tone, bool measure_only) {
    const float padding = 6;
    const float width = measure(label, Font::Caption) + padding * 2;
    const float height = 17;
    if (measure_only) return width;
    const Color color = tone_color(theme_, tone);
    const Rect r{x, y, width, height};
    fill_round(r, tokens::radius::pill, with_alpha(color, 0.12f));
    stroke_round(r, tokens::radius::pill, with_alpha(color, 0.2f), 0.5f);
    TextStyle style;
    style.font = Font::Caption;
    style.halign = HAlign::Center;
    text(label, r, style, color);
    return width;
}

void Renderer::live_indicator(float cx, float cy, bool live) {
    const Color color = live ? theme_.success() : theme_.text_tertiary();
    if (live) fill_circle(cx, cy, 7, with_alpha(color, 0.18f));
    fill_circle(cx, cy, 3.5f, color);
}

float Renderer::segmented_width(const std::vector<std::wstring> &labels) {
    float width = 4;
    for (const std::wstring &label : labels) width += measure(label, Font::BodyMedium) + tokens::space::lg * 2;
    return width;
}

std::vector<Rect> Renderer::segmented(const Rect &r, const std::vector<std::wstring> &labels, int selected) {
    std::vector<Rect> out;
    fill_round(r, tokens::radius::sm + 1, theme_.surface_sunken());
    stroke_round(r, tokens::radius::sm + 1, theme_.border(), 1);
    float x = r.x + 2;
    for (size_t i = 0; i < labels.size(); ++i) {
        const float w = measure(labels[i], Font::BodyMedium) + tokens::space::lg * 2;
        const Rect segment{x, r.y + 2, w, r.h - 4};
        if (static_cast<int>(i) == selected) {
            fill_round(segment, tokens::radius::sm, theme_.dark ? with_alpha(theme_.text(), 0.16f) : theme_.surface());
            if (!theme_.dark) stroke_round(segment, tokens::radius::sm, theme_.border_strong(), 1);
        }
        TextStyle style;
        style.font = static_cast<int>(i) == selected ? Font::BodyMedium : Font::Body;
        style.halign = HAlign::Center;
        text(labels[i], segment, style, static_cast<int>(i) == selected ? theme_.text() : theme_.text_secondary());
        out.push_back(segment);
        x += w;
    }
    return out;
}

void Renderer::toggle_switch(float x, float y, bool on, bool enabled) {
    const Rect track{x, y, 36, 20};
    Color fill = on ? theme_.accent() : with_alpha(theme_.text(), theme_.dark ? 0.18f : 0.12f);
    if (!enabled) fill = with_alpha(fill, 0.45f);
    fill_round(track, tokens::radius::pill, fill);
    const float kx = on ? track.right() - 10 - 2 : track.x + 10 + 2;
    fill_circle(kx, track.cy(), 8, with_alpha(colors::white, enabled ? 1.0f : 0.7f));
}

void Renderer::icon_button(const Rect &r, Symbol s, Color color, bool hovered, bool enabled) {
    if (hovered && enabled) fill_round(r, tokens::radius::sm, with_alpha(theme_.text(), 0.06f));
    symbol(s, Rect{r.cx() - 8, r.cy() - 8, 16, 16}, enabled ? color : theme_.text_tertiary(), 1.5f);
}

}  // namespace procyon::ui
