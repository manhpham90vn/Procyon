// The Cairo/Pango backend of the shared UI's Canvas: shapes, paths, gradients, real shadows (a
// blurred mask cached by size) and text in the bundled Inter and Nunito.
#pragma once

#include <cairo.h>
#include <pango/pangocairo.h>

#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "ui.hpp"

namespace procyon::ui {

class CairoCanvas : public Canvas {
public:
    CairoCanvas();
    ~CairoCanvas() override;
    CairoCanvas(const CairoCanvas &) = delete;
    CairoCanvas &operator=(const CairoCanvas &) = delete;

    // Draws a frame into `cr`, whose user space is in DIPs; `scale` is device pixels per DIP.
    void begin(cairo_t *cr, float scale);
    void end();
    // Lets go of cached layouts, shadows and images (the window is hidden).
    void trim();

    // ---- Canvas ----
    float dpi() const override { return 96 * scale_; }
    void fill_rect(const Rect &r, Color color) override;
    void fill_round_rect(const Rect &r, float radius, Color color) override;
    void fill_round_rect(const Rect &r, float radius, const Gradient &gradient) override;
    void stroke_round_rect(const Rect &r, float radius, Color color, float width) override;
    void fill_ellipse(Point center, float rx, float ry, Color color) override;
    void stroke_ellipse(Point center, float rx, float ry, Color color, float width) override;
    void line(Point a, Point b, Color color, float width, Dash dash) override;
    void fill_path(const Path &path, Color color) override;
    void fill_path(const Path &path, const Gradient &gradient) override;
    void stroke_path(const Path &path, Color color, const Stroke &stroke) override;
    void stroke_path(const Path &path, const Gradient &gradient, const Stroke &stroke) override;
    void shadow(const Rect &r, float radius, const Shadow &shadow) override;
    void draw_image(const Image &image, const Rect &r) override;
    void push_clip(const Rect &r) override;
    void pop_clip() override;
    void push_mask(const Path &path) override;
    void pop_mask() override;
    void push_scale(float scale, Point about) override;
    void pop_transform() override;
    float draw_text(std::wstring_view text, const Rect &r, const TextStyle &style, Color color) override;
    float measure_text(std::wstring_view text, Font font, float tracking, bool tabular = false) override;
    float line_height(Font font) override;
    float baseline(Font font) override;

private:
    struct ShadowKey {
        int w, h, radius, blur, scale;
        bool operator<(const ShadowKey &o) const {
            return std::tie(w, h, radius, blur, scale) < std::tie(o.w, o.h, o.radius, o.blur, o.scale);
        }
    };
    // A laid-out string, kept while frames keep drawing it: tables redraw hundreds of unchanged
    // cells a second, and shaping them again each time would be most of a frame's cost.
    struct LayoutKey {
        std::wstring text;
        float width, height, tracking;
        uint8_t font, halign;
        bool trim, tabular, wrap, uppercase;
        bool operator==(const LayoutKey &o) const {
            return width == o.width && height == o.height && tracking == o.tracking && font == o.font &&
                   halign == o.halign && trim == o.trim && tabular == o.tabular && wrap == o.wrap &&
                   uppercase == o.uppercase && text == o.text;
        }
    };
    struct LayoutKeyHash {
        size_t operator()(const LayoutKey &k) const;
    };
    struct CachedLayout {
        PangoLayout *layout = nullptr;
        float width = 0, height = 0;  // logical extents in DIPs
        bool aligned = false;         // the layout has a width and aligns itself
        uint32_t frame = 0;
    };

    void set_color(Color color);
    void set_gradient(const Gradient &gradient);
    void build_path(const Path &path);
    void round_rect_path(const Rect &r, float radius);
    void apply_stroke(const Stroke &stroke);
    PangoFontDescription *font(Font font);
    const CachedLayout *layout(std::wstring_view value, const TextStyle &style, float width, float height);
    void prune_layouts();

    cairo_t *cr_ = nullptr;
    float scale_ = 1;
    PangoFontMap *font_map_ = nullptr;
    PangoContext *context_ = nullptr;
    std::unordered_map<int, PangoFontDescription *> fonts_;
    std::unordered_map<LayoutKey, CachedLayout, LayoutKeyHash> layouts_;
    uint32_t frame_ = 0;
    std::map<ShadowKey, cairo_surface_t *> shadows_;
    std::unordered_map<uint64_t, cairo_surface_t *> images_;
    int depth_ = 0;  // cairo_save calls of push_clip/push_mask/push_scale still open
};

// Registers the fonts embedded in the executable with fontconfig. Call once before any text.
void register_embedded_fonts();

}  // namespace procyon::ui
