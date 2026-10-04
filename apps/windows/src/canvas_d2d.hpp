// The Direct2D/DirectWrite backend of the shared UI's Canvas, drawing into a DirectComposition
// swap chain so transparent pixels show the window's Mica backdrop.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite_3.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "ui.hpp"

namespace procyon::ui {

using Microsoft::WRL::ComPtr;

class D2DCanvas : public Canvas {
public:
    // `window` must have WS_EX_NOREDIRECTIONBITMAP: the composition visual is all it shows.
    bool init(HWND window);
    void shutdown();
    void resize(UINT width, UINT height);
    void set_dpi(float dpi);
    // Begins a frame cleared to `background` (transparent where the backdrop should show); false
    // when the device is lost (the window then repaints on the next frame).
    bool begin(Color background);
    bool end();

    // ---- Canvas ----
    float dpi() const override { return dpi_; }
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
    float measure_text(std::wstring_view text, Font font, float tracking) override;
    float line_height(Font font) override;

private:
    struct ShadowKey {
        int w, h, radius, blur;
        uint32_t color;
        bool operator<(const ShadowKey &o) const {
            return std::tie(w, h, radius, blur, color) < std::tie(o.w, o.h, o.radius, o.blur, o.color);
        }
    };

    bool create_device();
    bool create_swap_chain();
    bool bind_back_buffer();
    void release_device();
    ID2D1SolidColorBrush *brush(Color color);
    ComPtr<ID2D1LinearGradientBrush> gradient_brush(const Gradient &gradient);
    ComPtr<ID2D1PathGeometry> geometry(const Path &path);
    ID2D1StrokeStyle *stroke_style(const Stroke &stroke);
    IDWriteTextFormat *format(Font font);
    ComPtr<IDWriteTextLayout> layout(std::wstring_view value, const TextStyle &style, float width, float height);
    void load_fonts();
    ID2D1Effect *shadow_effect(const ShadowKey &key, const Rect &r, float radius, const Shadow &shadow);

    HWND window_ = nullptr;
    float dpi_ = 96;
    UINT width_ = 1, height_ = 1;  // the swap chain, in pixels
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<IDCompositionDevice> composition_;
    ComPtr<IDCompositionTarget> composition_target_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<ID2D1Device> device_;
    ComPtr<ID2D1DeviceContext> target_;  // the device context everything draws into
    ComPtr<ID2D1Bitmap1> back_buffer_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> dotted_, dashed_, round_, round_dotted_, round_dashed_;
    std::unordered_map<int, ComPtr<IDWriteTextFormat>> formats_;
    std::wstring ui_family_, display_family_, mono_family_;
    ComPtr<IDWriteFactory6> dwrite6_;                // variable-font text formats (Windows 10 1809+)
    ComPtr<IDWriteFontCollection2> embedded_fonts_;  // the bundled Inter and Nunito; null when they could not load
    bool has_rounded_ = false;
    std::map<ShadowKey, ComPtr<ID2D1Effect>> shadows_;
    std::unordered_map<uint64_t, ComPtr<ID2D1Bitmap1>> images_;  // Image::id -> the GPU copy
    std::vector<ComPtr<ID2D1Layer>> layers_;
    std::vector<D2D1_MATRIX_3X2_F> transforms_;
    int clips_ = 0;
};

}  // namespace procyon::ui
