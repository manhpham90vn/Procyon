// Cairo/Pango backend of the shared UI's Canvas.
#include "canvas_cairo.hpp"

#include <fontconfig/fontconfig.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

namespace procyon::ui {

// The fonts are linked into the executable (fonts.cpp), as the Windows app embeds them as
// resources: no install step, no lookup next to the binary.
extern "C" const unsigned char procyon_font_inter[], procyon_font_inter_end[];
extern "C" const unsigned char procyon_font_rounded[], procyon_font_rounded_end[];

namespace {

constexpr const char *kUiFamily = "Inter Variable";
constexpr const char *kRoundedFamily = "Nunito";
constexpr float kPi = 3.14159265358979f;

// fontconfig reads fonts from files: each embedded face goes into an anonymous memory file whose
// /proc/self/fd path stays valid for the life of the process.
void add_memory_font(const char *name, const unsigned char *begin, const unsigned char *end) {
    const int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) return;
    const size_t size = static_cast<size_t>(end - begin);
    size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, begin + written, size - written);
        if (n <= 0) {
            ::close(fd);
            return;
        }
        written += static_cast<size_t>(n);
    }
    const std::string path = "/proc/self/fd/" + std::to_string(fd);
    FcConfigAppFontAddFile(FcConfigGetCurrent(), reinterpret_cast<const FcChar8 *>(path.c_str()));
}

// Three box blurs approximate a Gaussian of standard deviation `sigma` (in pixels) on an A8 image.
void blur_alpha(unsigned char *data, int width, int height, int stride, float sigma) {
    if (sigma < 0.5f) return;
    // Box width for three passes (W3C filter effects): d = floor(sigma * 3 * sqrt(2 pi) / 4 + 0.5).
    const int d = std::max(1, static_cast<int>(std::floor(sigma * 3 * std::sqrt(2 * kPi) / 4 + 0.5f)));
    const int r = d / 2;
    std::vector<unsigned char> line(static_cast<size_t>(std::max(width, height)));
    auto pass = [&](unsigned char *start, int count, int step) {
        for (int i = 0; i < count; ++i) line[static_cast<size_t>(i)] = start[i * step];
        int sum = 0;
        for (int i = -r; i <= r; ++i) sum += (i >= 0 && i < count) ? line[static_cast<size_t>(i)] : 0;
        const int window = 2 * r + 1;
        for (int i = 0; i < count; ++i) {
            start[i * step] = static_cast<unsigned char>(sum / window);
            const int out = i - r, in = i + r + 1;
            if (out >= 0) sum -= line[static_cast<size_t>(out)];
            if (in < count) sum += line[static_cast<size_t>(in)];
        }
    };
    for (int round = 0; round < 3; ++round) {
        for (int y = 0; y < height; ++y) pass(data + y * stride, width, 1);
        for (int x = 0; x < width; ++x) pass(data + x, height, stride);
    }
}

}  // namespace

void register_embedded_fonts() {
    static bool done = false;
    if (done) return;
    done = true;
    add_memory_font("InterVariable.ttf", procyon_font_inter, procyon_font_inter_end);
    add_memory_font("NunitoVariable.ttf", procyon_font_rounded, procyon_font_rounded_end);
}

CairoCanvas::CairoCanvas() {
    register_embedded_fonts();
    // A font map of our own, created after the embedded fonts were added, so it sees them.
    font_map_ = pango_cairo_font_map_new();
    context_ = pango_font_map_create_context(font_map_);
    cairo_font_options_t *options = cairo_font_options_create();
    // Layouts are measured once and drawn at any scale: metrics must not snap to the pixel grid.
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    pango_cairo_context_set_font_options(context_, options);
    cairo_font_options_destroy(options);
    pango_context_set_round_glyph_positions(context_, FALSE);
    pango_cairo_context_set_resolution(context_, 96);
}

CairoCanvas::~CairoCanvas() {
    trim();
    for (auto &[key, description] : fonts_) pango_font_description_free(description);
    if (context_) g_object_unref(context_);
    if (font_map_) g_object_unref(font_map_);
}

void CairoCanvas::trim() {
    for (auto &[key, entry] : layouts_) g_object_unref(entry.layout);
    layouts_.clear();
    for (auto &[key, surface] : shadows_) cairo_surface_destroy(surface);
    shadows_.clear();
    for (auto &[id, surface] : images_) cairo_surface_destroy(surface);
    images_.clear();
}

void CairoCanvas::begin(cairo_t *cr, float scale) {
    cr_ = cr;
    scale_ = scale;
    depth_ = 0;
    ++frame_;
}

void CairoCanvas::end() {
    while (depth_ > 0) {
        cairo_restore(cr_);
        --depth_;
    }
    cr_ = nullptr;
    prune_layouts();
}

// ---------------------------------------------------------------------------------------------
// Sources, paths, strokes
// ---------------------------------------------------------------------------------------------

void CairoCanvas::set_color(Color c) { cairo_set_source_rgba(cr_, c.r, c.g, c.b, c.a); }

void CairoCanvas::set_gradient(const Gradient &g) {
    cairo_pattern_t *pattern = cairo_pattern_create_linear(g.start.x, g.start.y, g.end.x, g.end.y);
    cairo_pattern_add_color_stop_rgba(pattern, 0, g.from.r, g.from.g, g.from.b, g.from.a);
    cairo_pattern_add_color_stop_rgba(pattern, 1, g.to.r, g.to.g, g.to.b, g.to.a);
    cairo_set_source(cr_, pattern);
    cairo_pattern_destroy(pattern);
}

void CairoCanvas::round_rect_path(const Rect &r, float radius) {
    const float rad = std::clamp(radius, 0.0f, std::min(r.w, r.h) / 2);
    cairo_new_path(cr_);
    if (rad <= 0) {
        cairo_rectangle(cr_, r.x, r.y, r.w, r.h);
        return;
    }
    cairo_arc(cr_, r.right() - rad, r.y + rad, rad, -kPi / 2, 0);
    cairo_arc(cr_, r.right() - rad, r.bottom() - rad, rad, 0, kPi / 2);
    cairo_arc(cr_, r.x + rad, r.bottom() - rad, rad, kPi / 2, kPi);
    cairo_arc(cr_, r.x + rad, r.y + rad, rad, kPi, 3 * kPi / 2);
    cairo_close_path(cr_);
}

void CairoCanvas::build_path(const Path &path) {
    cairo_new_path(cr_);
    Point current{};
    bool open = false;
    for (const Path::Command &c : path.commands()) {
        switch (c.op) {
            case Path::Op::Move:
                cairo_move_to(cr_, c.p.x, c.p.y);
                current = c.p;
                open = true;
                break;
            case Path::Op::Line:
                if (!open) break;
                cairo_line_to(cr_, c.p.x, c.p.y);
                current = c.p;
                break;
            case Path::Op::Quad: {
                if (!open) break;
                // A quadratic Bézier is the cubic with control points 2/3 of the way to its one.
                const Point c1{current.x + 2.0f / 3 * (c.c1.x - current.x),
                               current.y + 2.0f / 3 * (c.c1.y - current.y)};
                const Point c2{c.p.x + 2.0f / 3 * (c.c1.x - c.p.x), c.p.y + 2.0f / 3 * (c.c1.y - c.p.y)};
                cairo_curve_to(cr_, c1.x, c1.y, c2.x, c2.y, c.p.x, c.p.y);
                current = c.p;
                break;
            }
            case Path::Op::Cubic:
                if (!open) break;
                cairo_curve_to(cr_, c.c1.x, c.c1.y, c.c2.x, c.c2.y, c.p.x, c.p.y);
                current = c.p;
                break;
            case Path::Op::Arc: {
                if (!open) break;
                // SVG endpoint arc → centre form (SVG 1.1 implementation notes, F.6.5).
                double rx = std::fabs(c.rx), ry = std::fabs(c.ry);
                if (rx == 0 || ry == 0 || (current.x == c.p.x && current.y == c.p.y)) {
                    cairo_line_to(cr_, c.p.x, c.p.y);
                    current = c.p;
                    break;
                }
                const double phi = c.rotation * kPi / 180, cos_phi = std::cos(phi), sin_phi = std::sin(phi);
                const double dx = (current.x - c.p.x) / 2, dy = (current.y - c.p.y) / 2;
                const double x1 = cos_phi * dx + sin_phi * dy, y1 = -sin_phi * dx + cos_phi * dy;
                const double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
                if (lambda > 1) {
                    rx *= std::sqrt(lambda);
                    ry *= std::sqrt(lambda);
                }
                const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
                const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
                double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
                if (c.large == c.sweep) coef = -coef;
                const double cx1 = coef * rx * y1 / ry, cy1 = -coef * ry * x1 / rx;
                const double cx = cos_phi * cx1 - sin_phi * cy1 + (current.x + c.p.x) / 2;
                const double cy = sin_phi * cx1 + cos_phi * cy1 + (current.y + c.p.y) / 2;
                const double start = std::atan2((y1 - cy1) / ry, (x1 - cx1) / rx);
                const double end = std::atan2((-y1 - cy1) / ry, (-x1 - cx1) / rx);
                double sweep = end - start;
                if (c.sweep && sweep < 0) sweep += 2 * kPi;
                if (!c.sweep && sweep > 0) sweep -= 2 * kPi;
                cairo_save(cr_);
                cairo_translate(cr_, cx, cy);
                cairo_rotate(cr_, phi);
                cairo_scale(cr_, rx, ry);
                if (c.sweep)
                    cairo_arc(cr_, 0, 0, 1, start, start + sweep);
                else
                    cairo_arc_negative(cr_, 0, 0, 1, start, start + sweep);
                cairo_restore(cr_);
                current = c.p;
                break;
            }
            case Path::Op::Close:
                if (!open) break;
                cairo_close_path(cr_);
                open = false;
                break;
        }
    }
}

void CairoCanvas::apply_stroke(const Stroke &stroke) {
    cairo_set_line_width(cr_, stroke.width);
    cairo_set_line_cap(cr_, stroke.round ? CAIRO_LINE_CAP_ROUND : CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr_, CAIRO_LINE_JOIN_ROUND);
    // Direct2D's dash patterns, in multiples of the stroke width.
    if (stroke.dash == Dash::Dotted) {
        const double dashes[] = {3.0 * stroke.width, 4.0 * stroke.width};
        cairo_set_dash(cr_, dashes, 2, 0);
    } else if (stroke.dash == Dash::Dashed) {
        const double dashes[] = {2.0 * stroke.width, 2.0 * stroke.width};
        cairo_set_dash(cr_, dashes, 2, 0);
    } else {
        cairo_set_dash(cr_, nullptr, 0, 0);
    }
}

// ---------------------------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------------------------

void CairoCanvas::fill_rect(const Rect &r, Color color) {
    if (r.empty() || color.a <= 0) return;
    cairo_new_path(cr_);
    cairo_rectangle(cr_, r.x, r.y, r.w, r.h);
    set_color(color);
    cairo_fill(cr_);
}

void CairoCanvas::fill_round_rect(const Rect &r, float radius, Color color) {
    if (r.empty() || color.a <= 0) return;
    round_rect_path(r, radius);
    set_color(color);
    cairo_fill(cr_);
}

void CairoCanvas::fill_round_rect(const Rect &r, float radius, const Gradient &gradient) {
    if (r.empty()) return;
    round_rect_path(r, radius);
    set_gradient(gradient);
    cairo_fill(cr_);
}

void CairoCanvas::stroke_round_rect(const Rect &r, float radius, Color color, float width) {
    if (r.empty() || color.a <= 0) return;
    round_rect_path(r, radius);
    set_color(color);
    Stroke stroke;
    stroke.width = width;
    stroke.round = false;
    apply_stroke(stroke);
    cairo_stroke(cr_);
}

void CairoCanvas::fill_ellipse(Point center, float rx, float ry, Color color) {
    if (color.a <= 0 || rx <= 0 || ry <= 0) return;
    Path path;
    path.add_ellipse(center, rx, ry);
    fill_path(path, color);
}

void CairoCanvas::stroke_ellipse(Point center, float rx, float ry, Color color, float width) {
    if (color.a <= 0 || rx <= 0 || ry <= 0) return;
    Path path;
    path.add_ellipse(center, rx, ry);
    Stroke stroke;
    stroke.width = width;
    stroke_path(path, color, stroke);
}

void CairoCanvas::line(Point a, Point b, Color color, float width, Dash dash) {
    if (color.a <= 0) return;
    cairo_new_path(cr_);
    cairo_move_to(cr_, a.x, a.y);
    cairo_line_to(cr_, b.x, b.y);
    Stroke stroke;
    stroke.width = width;
    stroke.round = false;
    stroke.dash = dash;
    apply_stroke(stroke);
    set_color(color);
    cairo_stroke(cr_);
}

void CairoCanvas::fill_path(const Path &path, Color color) {
    if (color.a <= 0 || path.empty()) return;
    build_path(path);
    set_color(color);
    cairo_fill(cr_);
}

void CairoCanvas::fill_path(const Path &path, const Gradient &gradient) {
    if (path.empty()) return;
    build_path(path);
    set_gradient(gradient);
    cairo_fill(cr_);
}

void CairoCanvas::stroke_path(const Path &path, Color color, const Stroke &stroke) {
    if (color.a <= 0 || path.empty()) return;
    build_path(path);
    apply_stroke(stroke);
    set_color(color);
    cairo_stroke(cr_);
}

void CairoCanvas::stroke_path(const Path &path, const Gradient &gradient, const Stroke &stroke) {
    if (path.empty()) return;
    build_path(path);
    apply_stroke(stroke);
    set_gradient(gradient);
    cairo_stroke(cr_);
}

// ---------------------------------------------------------------------------------------------
// Shadows and images
// ---------------------------------------------------------------------------------------------

void CairoCanvas::shadow(const Rect &r, float radius, const Shadow &shadow) {
    if (r.empty() || shadow.color.a <= 0) return;
    const float pad = std::ceil(shadow.blur * 2);
    const ShadowKey key{static_cast<int>(std::lround(r.w * 2)), static_cast<int>(std::lround(r.h * 2)),
                        static_cast<int>(std::lround(radius * 4)), static_cast<int>(std::lround(shadow.blur * 4)),
                        static_cast<int>(std::lround(scale_ * 100))};
    auto it = shadows_.find(key);
    if (it == shadows_.end()) {
        if (shadows_.size() > 96) {  // a resize produced many one-off sizes
            for (auto &[k, surface] : shadows_) cairo_surface_destroy(surface);
            shadows_.clear();
        }
        // The alpha mask of the rounded rectangle, padded for the blur, in device pixels.
        const int width = static_cast<int>(std::ceil((r.w + pad * 2) * scale_));
        const int height = static_cast<int>(std::ceil((r.h + pad * 2) * scale_));
        cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, std::max(width, 1), std::max(height, 1));
        cairo_t *m = cairo_create(mask);
        cairo_scale(m, scale_, scale_);
        cairo_t *saved = cr_;
        cr_ = m;
        round_rect_path(Rect{pad, pad, r.w, r.h}, radius);
        cairo_set_source_rgba(m, 0, 0, 0, 1);
        cairo_fill(m);
        cr_ = saved;
        cairo_destroy(m);
        cairo_surface_flush(mask);
        // SwiftUI's shadow(radius:) is the blur radius; the Gaussian's standard deviation is half of it.
        blur_alpha(cairo_image_surface_get_data(mask), width, height, cairo_image_surface_get_stride(mask),
                   shadow.blur / 2 * scale_);
        cairo_surface_mark_dirty(mask);
        cairo_surface_set_device_scale(mask, scale_, scale_);
        it = shadows_.emplace(key, mask).first;
    }
    set_color(shadow.color);
    cairo_mask_surface(cr_, it->second, r.x - pad, r.y - pad + shadow.dy);
}

void CairoCanvas::draw_image(const Image &image, const Rect &r) {
    if (r.empty() || image.width <= 0 || image.height <= 0) return;
    auto it = images_.find(image.id);
    if (it == images_.end()) {
        if (images_.size() > 1024) {
            for (auto &[id, surface] : images_) cairo_surface_destroy(surface);
            images_.clear();
        }
        // Premultiplied BGRA rows are CAIRO_FORMAT_ARGB32 on little-endian machines.
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, image.width, image.height);
        const int stride = cairo_image_surface_get_stride(surface);
        unsigned char *data = cairo_image_surface_get_data(surface);
        for (int y = 0; y < image.height; ++y)
            std::memcpy(data + y * stride, image.pixels.data() + static_cast<size_t>(y) * image.width,
                        static_cast<size_t>(image.width) * 4);
        cairo_surface_mark_dirty(surface);
        it = images_.emplace(image.id, surface).first;
    }
    cairo_save(cr_);
    cairo_translate(cr_, r.x, r.y);
    cairo_scale(cr_, r.w / image.width, r.h / image.height);
    cairo_set_source_surface(cr_, it->second, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_GOOD);
    cairo_paint(cr_);
    cairo_restore(cr_);
}

// ---------------------------------------------------------------------------------------------
// Clipping and transforms (one cairo_save each, so they nest in any order)
// ---------------------------------------------------------------------------------------------

void CairoCanvas::push_clip(const Rect &r) {
    cairo_save(cr_);
    ++depth_;
    cairo_new_path(cr_);
    cairo_rectangle(cr_, r.x, r.y, std::max(r.w, 0.0f), std::max(r.h, 0.0f));
    cairo_clip(cr_);
}

void CairoCanvas::pop_clip() {
    if (depth_ <= 0) return;
    cairo_restore(cr_);
    --depth_;
}

void CairoCanvas::push_mask(const Path &path) {
    cairo_save(cr_);
    ++depth_;
    build_path(path);
    cairo_clip(cr_);
}

void CairoCanvas::pop_mask() { pop_clip(); }

void CairoCanvas::push_scale(float scale, Point about) {
    cairo_save(cr_);
    ++depth_;
    cairo_translate(cr_, about.x, about.y);
    cairo_scale(cr_, scale, scale);
    cairo_translate(cr_, -about.x, -about.y);
}

void CairoCanvas::pop_transform() { pop_clip(); }

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------

PangoFontDescription *CairoCanvas::font(Font f) {
    const int key = static_cast<int>(f);
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second;
    const tokens::FontSpec spec = font_spec(f);
    PangoFontDescription *description = pango_font_description_new();
    if (spec.design == tokens::FontDesign::Monospaced) {
        pango_font_description_set_family(description, "monospace");
    } else {
        // The macOS app sets its screens in SF Pro and its big figures in SF Rounded; Inter and
        // Nunito are the closest faces that may ship outside Apple platforms.
        pango_font_description_set_family(description,
                                          spec.design == tokens::FontDesign::Rounded ? kRoundedFamily : kUiFamily);
        // Optical size follows the text size (Inter clamps it to its 14-32 range), as SF Pro does.
        const std::string variations = "opsz=" + std::to_string(static_cast<int>(spec.size));
        pango_font_description_set_variations(description, variations.c_str());
    }
    pango_font_description_set_weight(description, static_cast<PangoWeight>(spec.weight));
    pango_font_description_set_absolute_size(description, spec.size * PANGO_SCALE);
    fonts_[key] = description;
    return description;
}

size_t CairoCanvas::LayoutKeyHash::operator()(const LayoutKey &k) const {
    auto bits = [](float f) {
        uint32_t u = 0;
        std::memcpy(&u, &f, sizeof u);
        return static_cast<size_t>(u);
    };
    size_t h = std::hash<std::wstring>{}(k.text);
    auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
    mix(bits(k.width));
    mix(bits(k.height));
    mix(bits(k.tracking));
    mix(static_cast<size_t>(k.font) | static_cast<size_t>(k.halign) << 8 | static_cast<size_t>(k.trim) << 16 |
        static_cast<size_t>(k.tabular) << 17 | static_cast<size_t>(k.wrap) << 18 |
        static_cast<size_t>(k.uppercase) << 19);
    return h;
}

const CairoCanvas::CachedLayout *CairoCanvas::layout(std::wstring_view value, const TextStyle &style, float width,
                                                     float height) {
    LayoutKey key{std::wstring(value),
                  std::max(width, 1.0f),
                  std::max(height, 1.0f),
                  style.tracking,
                  static_cast<uint8_t>(style.font),
                  static_cast<uint8_t>(style.halign),
                  style.trim,
                  style.tabular,
                  style.wrap,
                  style.uppercase};
    auto it = layouts_.find(key);
    if (it == layouts_.end()) {
        std::wstring text = key.text;
        if (key.uppercase)
            for (wchar_t &c : text) c = static_cast<wchar_t>(std::towupper(c));
        const std::string utf8 = fmt::to_utf8(text);
        PangoLayout *l = pango_layout_new(context_);
        pango_layout_set_font_description(l, font(static_cast<Font>(key.font)));
        pango_layout_set_text(l, utf8.c_str(), static_cast<int>(utf8.size()));
        PangoAttrList *attributes = pango_attr_list_new();
        if (key.tabular) pango_attr_list_insert(attributes, pango_attr_font_features_new("tnum=1"));
        if (key.tracking != 0)
            pango_attr_list_insert(attributes,
                                   pango_attr_letter_spacing_new(static_cast<int>(key.tracking * PANGO_SCALE)));
        pango_layout_set_attributes(l, attributes);
        pango_attr_list_unref(attributes);
        const auto halign = static_cast<HAlign>(key.halign);
        CachedLayout entry;
        entry.layout = l;
        if (key.wrap || key.trim) {
            // A box width: Pango wraps or ellipsizes inside it and aligns the lines itself.
            pango_layout_set_width(l, static_cast<int>(key.width * PANGO_SCALE));
            pango_layout_set_alignment(l, halign == HAlign::Left     ? PANGO_ALIGN_LEFT
                                          : halign == HAlign::Center ? PANGO_ALIGN_CENTER
                                                                     : PANGO_ALIGN_RIGHT);
            if (key.wrap) {
                pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
                pango_layout_set_height(l, static_cast<int>(key.height * PANGO_SCALE));
                if (key.trim) pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
            } else {
                pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
            }
            entry.aligned = true;
        }
        PangoRectangle logical{};
        pango_layout_get_extents(l, nullptr, &logical);
        entry.width = static_cast<float>(logical.width) / PANGO_SCALE;
        entry.height = static_cast<float>(logical.height) / PANGO_SCALE;
        if (entry.aligned && !key.wrap) {
            // The ink may be narrower than the box: report the text's own width.
            int w = 0;
            pango_layout_get_size(l, &w, nullptr);
            PangoRectangle line_logical{};
            pango_layout_line_get_extents(pango_layout_get_line_readonly(l, 0), nullptr, &line_logical);
            entry.width = static_cast<float>(line_logical.width) / PANGO_SCALE;
        }
        it = layouts_.emplace(std::move(key), entry).first;
    }
    it->second.frame = frame_;
    return &it->second;
}

void CairoCanvas::prune_layouts() {
    // Strings not drawn for a few seconds (a scrolled-away row, an old value) are dropped.
    if (layouts_.size() < 4000 && frame_ % 64 != 0) return;
    for (auto it = layouts_.begin(); it != layouts_.end();) {
        if (frame_ - it->second.frame > 120) {
            g_object_unref(it->second.layout);
            it = layouts_.erase(it);
        } else {
            ++it;
        }
    }
}

float CairoCanvas::draw_text(std::wstring_view value, const Rect &r, const TextStyle &style, Color color) {
    if (value.empty() || r.empty() || !cr_) return 0;
    const CachedLayout *l = layout(value, style, r.w, style.wrap ? r.h : 10000);
    if (!l) return 0;
    float y = r.y;
    if (style.valign == VAlign::Center)
        y = r.y + (r.h - l->height) / 2;
    else if (style.valign == VAlign::Bottom)
        y = r.bottom() - l->height;
    float x = r.x;
    if (!l->aligned) {
        if (style.halign == HAlign::Center) x = r.x + (r.w - l->width) / 2;
        if (style.halign == HAlign::Right) x = r.right() - l->width;
    }
    set_color(color);
    cairo_move_to(cr_, x, y);
    pango_cairo_show_layout(cr_, l->layout);
    return l->width;
}

float CairoCanvas::measure_text(std::wstring_view value, Font f, float tracking) {
    if (value.empty()) return 0;
    TextStyle style;
    style.font = f;
    style.trim = false;
    style.tracking = tracking;
    const CachedLayout *l = layout(value, style, 10000, 10000);
    return l ? l->width : 0;
}

float CairoCanvas::line_height(Font f) {
    TextStyle style;
    style.font = f;
    style.trim = false;
    const CachedLayout *l = layout(L"Ag", style, 1000, 1000);
    return l ? l->height : font_spec(f).size * 1.3f;
}

}  // namespace procyon::ui
