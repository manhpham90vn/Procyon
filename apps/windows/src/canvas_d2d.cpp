// Direct2D/DirectWrite backend of the shared UI's Canvas: shapes, paths, gradients, real shadows
// (the Shadow effect of a device context), clipping and text in the bundled Inter and Nunito.
#include "canvas_d2d.hpp"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <tuple>

#include "resource.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace procyon::ui {

namespace {

constexpr const wchar_t *kUiFamily = L"Inter";
constexpr const wchar_t *kRoundedFamily = L"Nunito";

D2D1_COLOR_F d2d(Color c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }
D2D1_POINT_2F d2d(Point p) { return D2D1::Point2F(p.x, p.y); }
D2D1_RECT_F d2d(const Rect &r) { return D2D1::RectF(r.x, r.y, r.right(), r.bottom()); }

bool has_family(IDWriteFactory *factory, const wchar_t *name) {
    ComPtr<IDWriteFontCollection> fonts;
    if (FAILED(factory->GetSystemFontCollection(&fonts))) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
}

bool family_in(IDWriteFontCollection2 *collection, const wchar_t *name) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists;
}

// The bundled faces (RCDATA resources) as one private collection in the typographic family
// model, so weight and optical-size axes can be set per text format. Null when the loader is
// missing (before Windows 10 1703) or no resource is present.
ComPtr<IDWriteFontCollection2> load_embedded_fonts(IDWriteFactory6 *factory) {
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    if (FAILED(factory->CreateInMemoryFontFileLoader(&loader)) || FAILED(factory->RegisterFontFileLoader(loader.Get())))
        return nullptr;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(factory->CreateFontSetBuilder(&builder))) return nullptr;
    HMODULE module = GetModuleHandleW(nullptr);
    int added = 0;
    for (int id : {IDR_FONT_INTER, IDR_FONT_ROUNDED}) {
        HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!found) continue;
        HGLOBAL loaded = LoadResource(module, found);
        const void *bytes = loaded ? LockResource(loaded) : nullptr;
        const DWORD size = SizeofResource(module, found);
        if (!bytes || size == 0) continue;
        ComPtr<IDWriteFontFile> file;
        // Resource memory stays mapped for the life of the module, so no owner object is needed.
        if (FAILED(loader->CreateInMemoryFontFileReference(factory, bytes, size, nullptr, &file))) continue;
        if (SUCCEEDED(builder->AddFontFile(file.Get()))) ++added;
    }
    if (added == 0) return nullptr;
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection2> collection;
    if (FAILED(builder->CreateFontSet(&set)) ||
        FAILED(factory->CreateFontCollectionFromFontSet(set.Get(), DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC, &collection)))
        return nullptr;
    if (!family_in(collection.Get(), kUiFamily)) return nullptr;
    return collection;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

bool D2DCanvas::init(HWND window) {
    window_ = window;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                 reinterpret_cast<void **>(factory_.GetAddressOf()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(dwrite_.GetAddressOf()))))
        return false;
    load_fonts();
    const float dotted[] = {3, 4};
    const auto props = [](D2D1_CAP_STYLE cap, D2D1_DASH_STYLE dash) {
        return D2D1::StrokeStyleProperties(cap, cap, cap, D2D1_LINE_JOIN_ROUND, 10, dash, 0);
    };
    factory_->CreateStrokeStyle(props(D2D1_CAP_STYLE_FLAT, D2D1_DASH_STYLE_CUSTOM), dotted, 2, &dotted_);
    factory_->CreateStrokeStyle(props(D2D1_CAP_STYLE_FLAT, D2D1_DASH_STYLE_DASH), nullptr, 0, &dashed_);
    factory_->CreateStrokeStyle(props(D2D1_CAP_STYLE_ROUND, D2D1_DASH_STYLE_SOLID), nullptr, 0, &round_);
    factory_->CreateStrokeStyle(props(D2D1_CAP_STYLE_ROUND, D2D1_DASH_STYLE_CUSTOM), dotted, 2, &round_dotted_);
    factory_->CreateStrokeStyle(props(D2D1_CAP_STYLE_ROUND, D2D1_DASH_STYLE_DASH), nullptr, 0, &round_dashed_);
    return true;
}

void D2DCanvas::load_fonts() {
    ui_family_ = has_family(dwrite_.Get(), L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    display_family_ =
        has_family(dwrite_.Get(), L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    mono_family_ = has_family(dwrite_.Get(), L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    // The macOS app sets its screens in SF Pro and its big figures in SF Rounded; Inter and Nunito
    // are the closest faces that may ship outside Apple platforms.
    if (SUCCEEDED(dwrite_.As(&dwrite6_)) && dwrite6_) embedded_fonts_ = load_embedded_fonts(dwrite6_.Get());
    has_rounded_ = embedded_fonts_ && family_in(embedded_fonts_.Get(), kRoundedFamily);
}

void D2DCanvas::shutdown() {
    release_device();
    layouts_.clear();
    ellipses_.clear();
    tabular_.Reset();
    formats_.clear();
    dotted_.Reset();
    dashed_.Reset();
    round_.Reset();
    round_dotted_.Reset();
    round_dashed_.Reset();
    embedded_fonts_.Reset();
    dwrite6_.Reset();
    dwrite_.Reset();
    factory_.Reset();
}

// Everything that belongs to the GPU device: recreated after a device loss.
void D2DCanvas::release_device() {
    shadows_.clear();
    images_.clear();
    layers_.clear();
    brush_.Reset();
    if (target_) target_->SetTarget(nullptr);
    back_buffer_.Reset();
    target_.Reset();
    device_.Reset();
    visual_.Reset();
    composition_target_.Reset();
    composition_.Reset();
    swap_chain_.Reset();
    d3d_.Reset();
}

bool D2DCanvas::create_device() {
    // A hardware D3D11 device (WARP when there is none, as over Remote Desktop), shared with Direct2D.
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                   &d3d_, nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d_,
                               nullptr, nullptr);
    if (FAILED(hr)) return false;
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d3d_.As(&dxgi))) return false;
    if (FAILED(factory_->CreateDevice(dxgi.Get(), &device_))) return false;
    if (FAILED(device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &target_))) return false;
    target_->SetDpi(dpi_, dpi_);
    // Grayscale antialiasing reads like macOS text; ClearType's colour fringes stand out on dark
    // surfaces, in screenshots, and cannot be drawn over a transparent backdrop at all.
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brush_);
    return true;
}

bool D2DCanvas::create_swap_chain() {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> dxgi_factory;
    if (FAILED(d3d_.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgi_factory))))
        return false;
    RECT rc{};
    GetClientRect(window_, &rc);
    width_ = std::max(1L, rc.right - rc.left);
    height_ = std::max(1L, rc.bottom - rc.top);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;  // transparent pixels show the Mica backdrop
    if (FAILED(dxgi_factory->CreateSwapChainForComposition(d3d_.Get(), &desc, nullptr, &swap_chain_))) return false;
    // The swap chain reaches the window through a DirectComposition visual.
    if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&composition_)))) return false;
    if (FAILED(composition_->CreateTargetForHwnd(window_, TRUE, &composition_target_))) return false;
    if (FAILED(composition_->CreateVisual(&visual_))) return false;
    visual_->SetContent(swap_chain_.Get());
    composition_target_->SetRoot(visual_.Get());
    return SUCCEEDED(composition_->Commit());
}

bool D2DCanvas::bind_back_buffer() {
    ComPtr<IDXGISurface> surface;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_, dpi_);
    if (FAILED(target_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &back_buffer_))) return false;
    target_->SetTarget(back_buffer_.Get());
    return true;
}

void D2DCanvas::set_dpi(float dpi) {
    if (dpi == dpi_) return;
    dpi_ = dpi;
    shadows_.clear();  // rasterised at the old scale
    if (target_) {
        target_->SetDpi(dpi, dpi);
        if (swap_chain_) bind_back_buffer();  // the bitmap carries the dpi too
    }
}

void D2DCanvas::resize(UINT width, UINT height) {
    width_ = std::max(1u, width);
    height_ = std::max(1u, height);
    if (!swap_chain_) return;
    target_->SetTarget(nullptr);
    back_buffer_.Reset();
    if (FAILED(swap_chain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0)) || !bind_back_buffer())
        release_device();  // recreated on the next frame
}

bool D2DCanvas::begin(Color background) {
    if (!target_ || !swap_chain_ || !back_buffer_) {
        release_device();
        if (!create_device() || !create_swap_chain() || !bind_back_buffer()) {
            release_device();
            return false;
        }
    }
    ++frame_;
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    transforms_.clear();
    target_->Clear(d2d(background));
    clips_ = 0;
    return true;
}

// A layout no recent frame drew is a value that has changed (a CPU figure, a byte count) or a
// screen that was left: dropping it right away keeps the cache at about one screen's worth of
// text, a few hundred entries. Three frames of grace cover a measure_text() before begin().
void D2DCanvas::prune_layouts() {
    constexpr uint32_t kGraceFrames = 3;
    for (auto it = layouts_.begin(); it != layouts_.end();) {
        if (frame_ - it->second.frame > kGraceFrames)
            it = layouts_.erase(it);
        else
            ++it;
    }
}

bool D2DCanvas::end() {
    while (clips_ > 0) pop_clip();
    while (!layers_.empty()) pop_mask();
    while (!transforms_.empty()) pop_transform();
    prune_layouts();
    HRESULT hr = target_->EndDraw();
    // Before Present: a flip-model swap chain's buffer 0 is the frame just drawn only until then.
    if (SUCCEEDED(hr) && !capture_path_.empty()) {
        captured_ = save_back_buffer(capture_path_);
        capture_path_.clear();
    }
    if (SUCCEEDED(hr)) hr = swap_chain_->Present(1, 0);
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        release_device();
        return false;
    }
    return SUCCEEDED(hr);
}

// Copies the back buffer into a CPU-readable texture and encodes it as a PNG with WIC. The buffer is
// premultiplied BGRA; the PNG gets straight alpha, which is opaque wherever the page drew.
bool D2DCanvas::save_back_buffer(const std::wstring &path) {
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    buffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(d3d_->CreateTexture2D(&desc, nullptr, &staging))) return false;
    ComPtr<ID3D11DeviceContext> context;
    d3d_->GetImmediateContext(&context);
    context->CopyResource(staging.Get(), buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const UINT stride = desc.Width * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
        const BYTE *from = static_cast<const BYTE *>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        BYTE *to = pixels.data() + static_cast<size_t>(y) * stride;
        for (UINT x = 0; x < desc.Width; ++x, from += 4, to += 4) {
            const BYTE a = from[3];
            for (int c = 0; c < 3; ++c) to[c] = a ? static_cast<BYTE>(std::min(255, from[c] * 255 / a)) : 0;
            to[3] = a;
        }
    }
    context->Unmap(staging.Get(), 0);

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(desc.Width, desc.Height)))
        return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA) return false;
    return SUCCEEDED(frame->WritePixels(desc.Height, stride, static_cast<UINT>(pixels.size()), pixels.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// ---------------------------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------------------------

void D2DCanvas::draw_image(const Image &image, const Rect &r) {
    if (image.width <= 0 || image.height <= 0 || r.empty()) return;
    auto it = images_.find(image.id);
    if (it == images_.end()) {
        if (images_.size() > 1024) images_.clear();
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> bitmap;
        if (FAILED(target_->CreateBitmap(D2D1::SizeU(static_cast<UINT>(image.width), static_cast<UINT>(image.height)),
                                         image.pixels.data(), static_cast<UINT>(image.width) * 4, &props, &bitmap)))
            return;
        it = images_.emplace(image.id, bitmap).first;
    }
    const D2D1_RECT_F dest = d2d(r);
    target_->DrawBitmap(it->second.Get(), &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
}

// ---------------------------------------------------------------------------------------------
// Brushes, geometry, strokes
// ---------------------------------------------------------------------------------------------

ID2D1SolidColorBrush *D2DCanvas::brush(Color color) {
    brush_->SetColor(d2d(color));
    return brush_.Get();
}

ComPtr<ID2D1LinearGradientBrush> D2DCanvas::gradient_brush(const Gradient &gradient) {
    D2D1_GRADIENT_STOP stops[2] = {{0, d2d(gradient.from)}, {1, d2d(gradient.to)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> out;
    if (FAILED(target_->CreateGradientStopCollection(stops, 2, &collection))) return nullptr;
    target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(d2d(gradient.start), d2d(gradient.end)),
                                       collection.Get(), &out);
    return out;
}

ComPtr<ID2D1PathGeometry> D2DCanvas::geometry(const Path &path) {
    ComPtr<ID2D1PathGeometry> out;
    if (path.empty() || FAILED(factory_->CreatePathGeometry(&out))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(out->Open(&sink))) return nullptr;
    bool open = false;
    // A figure is filled when it ends with Close; D2D needs to know up front, so look ahead.
    const auto &commands = path.commands();
    const auto figure_closed = [&](size_t from) {
        for (size_t i = from + 1; i < commands.size(); ++i) {
            if (commands[i].op == Path::Op::Move) return false;
            if (commands[i].op == Path::Op::Close) return true;
        }
        return false;
    };
    for (size_t i = 0; i < commands.size(); ++i) {
        const Path::Command &c = commands[i];
        switch (c.op) {
            case Path::Op::Move:
                if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->BeginFigure(d2d(c.p), figure_closed(i) ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                open = true;
                break;
            case Path::Op::Line:
                if (!open) break;
                sink->AddLine(d2d(c.p));
                break;
            case Path::Op::Quad:
                if (!open) break;
                sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(d2d(c.c1), d2d(c.p)));
                break;
            case Path::Op::Cubic:
                if (!open) break;
                sink->AddBezier(D2D1::BezierSegment(d2d(c.c1), d2d(c.c2), d2d(c.p)));
                break;
            case Path::Op::Arc:
                if (!open) break;
                sink->AddArc(
                    D2D1::ArcSegment(d2d(c.p), D2D1::SizeF(c.rx, c.ry), c.rotation,
                                     c.sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                     c.large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                break;
            case Path::Op::Close:
                if (!open) break;
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                open = false;
                break;
        }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return nullptr;
    return out;
}

ID2D1StrokeStyle *D2DCanvas::stroke_style(const Stroke &stroke) {
    switch (stroke.dash) {
        case Dash::Dotted: return stroke.round ? round_dotted_.Get() : dotted_.Get();
        case Dash::Dashed: return stroke.round ? round_dashed_.Get() : dashed_.Get();
        default: return stroke.round ? round_.Get() : nullptr;
    }
}

// ---------------------------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------------------------

void D2DCanvas::fill_rect(const Rect &r, Color color) {
    if (r.empty() || color.a <= 0) return;
    target_->FillRectangle(d2d(r), brush(color));
}

void D2DCanvas::fill_round_rect(const Rect &r, float radius, Color color) {
    if (r.empty() || color.a <= 0) return;
    target_->FillRoundedRectangle(D2D1::RoundedRect(d2d(r), radius, radius), brush(color));
}

void D2DCanvas::fill_round_rect(const Rect &r, float radius, const Gradient &gradient) {
    if (r.empty()) return;
    if (ComPtr<ID2D1LinearGradientBrush> b = gradient_brush(gradient))
        target_->FillRoundedRectangle(D2D1::RoundedRect(d2d(r), radius, radius), b.Get());
}

void D2DCanvas::stroke_round_rect(const Rect &r, float radius, Color color, float width) {
    if (r.empty() || color.a <= 0) return;
    target_->DrawRoundedRectangle(D2D1::RoundedRect(d2d(r), radius, radius), brush(color), width);
}

void D2DCanvas::fill_ellipse(Point center, float rx, float ry, Color color) {
    if (color.a <= 0) return;
    target_->FillEllipse(D2D1::Ellipse(d2d(center), rx, ry), brush(color));
}

void D2DCanvas::stroke_ellipse(Point center, float rx, float ry, Color color, float width) {
    if (color.a <= 0) return;
    target_->DrawEllipse(D2D1::Ellipse(d2d(center), rx, ry), brush(color), width);
}

void D2DCanvas::line(Point a, Point b, Color color, float width, Dash dash) {
    if (color.a <= 0) return;
    Stroke stroke;
    stroke.round = false;
    stroke.dash = dash;
    target_->DrawLine(d2d(a), d2d(b), brush(color), width, stroke_style(stroke));
}

void D2DCanvas::fill_path(const Path &path, Color color) {
    if (color.a <= 0) return;
    if (ComPtr<ID2D1PathGeometry> g = geometry(path)) target_->FillGeometry(g.Get(), brush(color));
}

void D2DCanvas::fill_path(const Path &path, const Gradient &gradient) {
    ComPtr<ID2D1PathGeometry> g = geometry(path);
    ComPtr<ID2D1LinearGradientBrush> b = gradient_brush(gradient);
    if (g && b) target_->FillGeometry(g.Get(), b.Get());
}

void D2DCanvas::stroke_path(const Path &path, Color color, const Stroke &stroke) {
    if (color.a <= 0) return;
    if (ComPtr<ID2D1PathGeometry> g = geometry(path))
        target_->DrawGeometry(g.Get(), brush(color), stroke.width, stroke_style(stroke));
}

void D2DCanvas::stroke_path(const Path &path, const Gradient &gradient, const Stroke &stroke) {
    ComPtr<ID2D1PathGeometry> g = geometry(path);
    ComPtr<ID2D1LinearGradientBrush> b = gradient_brush(gradient);
    if (g && b) target_->DrawGeometry(g.Get(), b.Get(), stroke.width, stroke_style(stroke));
}

// ---------------------------------------------------------------------------------------------
// Shadows
// ---------------------------------------------------------------------------------------------

ID2D1Effect *D2DCanvas::shadow_effect(const ShadowKey &key, const Rect &r, float radius, const Shadow &shadow) {
    auto it = shadows_.find(key);
    if (it != shadows_.end()) return it->second.Get();
    if (shadows_.size() > 96) shadows_.clear();  // a resize produced many one-off sizes
    // The alpha mask: the rounded rectangle in an offscreen bitmap, padded for the blur. Its own
    // BeginDraw/EndDraw nests inside the window's frame.
    const float pad = std::ceil(shadow.blur * 2);
    const D2D1_SIZE_F size = D2D1::SizeF(r.w + pad * 2, r.h + pad * 2);
    ComPtr<ID2D1BitmapRenderTarget> offscreen;
    if (FAILED(target_->CreateCompatibleRenderTarget(size, &offscreen))) return nullptr;
    ComPtr<ID2D1SolidColorBrush> black;
    offscreen->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &black);
    offscreen->BeginDraw();
    offscreen->Clear(D2D1::ColorF(0, 0, 0, 0));
    offscreen->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(pad, pad, pad + r.w, pad + r.h), radius, radius),
                                    black.Get());
    if (FAILED(offscreen->EndDraw())) return nullptr;
    ComPtr<ID2D1Bitmap> mask;
    if (FAILED(offscreen->GetBitmap(&mask))) return nullptr;
    ComPtr<ID2D1Effect> effect;
    if (FAILED(target_->CreateEffect(CLSID_D2D1Shadow, &effect))) return nullptr;
    effect->SetInput(0, mask.Get());
    // SwiftUI's shadow(radius:) is the blur radius; the Gaussian's standard deviation is half of it.
    effect->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, std::max(0.5f, shadow.blur / 2));
    // The colour's alpha is the shadow's opacity.
    effect->SetValue(D2D1_SHADOW_PROP_COLOR,
                     D2D1::Vector4F(shadow.color.r, shadow.color.g, shadow.color.b, shadow.color.a));
    effect->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, D2D1_SHADOW_OPTIMIZATION_QUALITY);
    effect->SetValue(D2D1_PROPERTY_CACHED, TRUE);  // rasterised once, reused every frame
    return shadows_.emplace(key, effect).first->second.Get();
}

void D2DCanvas::shadow(const Rect &r, float radius, const Shadow &shadow) {
    if (r.empty() || shadow.color.a <= 0) return;
    const auto channel = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    const uint32_t packed = channel(shadow.color.r) << 24 | channel(shadow.color.g) << 16 |
                            channel(shadow.color.b) << 8 | channel(shadow.color.a);
    const ShadowKey key{static_cast<int>(std::lround(r.w * 2)), static_cast<int>(std::lround(r.h * 2)),
                        static_cast<int>(std::lround(radius * 4)), static_cast<int>(std::lround(shadow.blur * 4)),
                        packed};
    ID2D1Effect *effect = shadow_effect(key, r, radius, shadow);
    if (!effect) return;
    const float pad = std::ceil(shadow.blur * 2);
    const D2D1_POINT_2F offset = D2D1::Point2F(r.x - pad, r.y - pad + shadow.dy);
    target_->DrawImage(effect, &offset, nullptr, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
}

// ---------------------------------------------------------------------------------------------
// Clipping and transforms
// ---------------------------------------------------------------------------------------------

void D2DCanvas::push_clip(const Rect &r) {
    target_->PushAxisAlignedClip(d2d(r), D2D1_ANTIALIAS_MODE_ALIASED);
    ++clips_;
}

void D2DCanvas::pop_clip() {
    if (clips_ <= 0) return;
    target_->PopAxisAlignedClip();
    --clips_;
}

void D2DCanvas::push_mask(const Path &path) {
    ComPtr<ID2D1PathGeometry> g = geometry(path);
    ComPtr<ID2D1Layer> layer;
    if (!g || FAILED(target_->CreateLayer(nullptr, &layer))) {
        layers_.push_back(nullptr);
        return;
    }
    target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.Get()), layer.Get());
    layers_.push_back(layer);
}

void D2DCanvas::pop_mask() {
    if (layers_.empty()) return;
    if (layers_.back()) target_->PopLayer();
    layers_.pop_back();
}

void D2DCanvas::push_scale(float scale, Point about) {
    D2D1_MATRIX_3X2_F current;
    target_->GetTransform(&current);
    transforms_.push_back(current);
    target_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, d2d(about)) *
                          *D2D1::Matrix3x2F::ReinterpretBaseType(&current));
}

void D2DCanvas::pop_transform() {
    if (transforms_.empty()) return;
    target_->SetTransform(transforms_.back());
    transforms_.pop_back();
}

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------

IDWriteTextFormat *D2DCanvas::format(Font font) {
    const int key = static_cast<int>(font);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();
    const tokens::FontSpec spec = font_spec(font);
    const bool mono = spec.design == tokens::FontDesign::Monospaced;
    const bool rounded = spec.design == tokens::FontDesign::Rounded;
    ComPtr<IDWriteTextFormat> created;
    if (embedded_fonts_ && !mono) {
        // Optical size follows the text size (Inter clamps it to its 14-32 range), as SF Pro does.
        const DWRITE_FONT_AXIS_VALUE axes[] = {{DWRITE_FONT_AXIS_TAG_WEIGHT, static_cast<float>(spec.weight)},
                                               {DWRITE_FONT_AXIS_TAG_OPTICAL_SIZE, spec.size}};
        ComPtr<IDWriteTextFormat3> created3;
        const wchar_t *family = rounded && has_rounded_ ? kRoundedFamily : kUiFamily;
        if (SUCCEEDED(
                dwrite6_->CreateTextFormat(family, embedded_fonts_.Get(), axes, 2, spec.size, L"en-us", &created3)))
            created = created3;
    }
    if (!created) {
        // Segoe UI Variable ships two optical sizes and no rounded design: "Text" is tuned for body
        // sizes and "Display" for 20 and up, so the family follows the size.
        const wchar_t *family = mono              ? mono_family_.c_str()
                                : spec.size >= 20 ? display_family_.c_str()
                                                  : ui_family_.c_str();
        if (FAILED(dwrite_->CreateTextFormat(family, nullptr, static_cast<DWRITE_FONT_WEIGHT>(spec.weight),
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, spec.size, L"en-us",
                                             &created)))
            return nullptr;
    }
    created->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    formats_[key] = created;
    return created.Get();
}

// The sign depends on the font alone, so one per Font serves every trimmed layout in it.
IDWriteInlineObject *D2DCanvas::ellipsis(Font font, IDWriteTextFormat *format) {
    const int key = static_cast<int>(font);
    auto it = ellipses_.find(key);
    if (it != ellipses_.end()) return it->second.Get();
    ComPtr<IDWriteInlineObject> created;
    if (FAILED(dwrite_->CreateEllipsisTrimmingSign(format, &created))) return nullptr;
    ellipses_[key] = created;
    return created.Get();
}

size_t D2DCanvas::LayoutKeyHash::operator()(const LayoutKey &k) const {
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

const D2DCanvas::CachedLayout *D2DCanvas::layout(std::wstring_view value, const TextStyle &style, float width,
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
        ComPtr<IDWriteTextLayout> created = create_layout(key);
        if (!created) return nullptr;
        CachedLayout entry;
        entry.layout = created;
        created->GetMetrics(&entry.metrics);
        it = layouts_.emplace(std::move(key), std::move(entry)).first;
    }
    it->second.frame = frame_;
    return &it->second;
}

ComPtr<IDWriteTextLayout> D2DCanvas::create_layout(const LayoutKey &key) {
    IDWriteTextFormat *fmt_ = format(static_cast<Font>(key.font));
    if (!fmt_) return nullptr;
    std::wstring text = key.text;
    if (key.uppercase)
        for (wchar_t &c : text) c = static_cast<wchar_t>(std::towupper(c));
    ComPtr<IDWriteTextLayout> out;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt_, key.width, key.height,
                                         &out)))
        return nullptr;
    const auto halign = static_cast<HAlign>(key.halign);
    out->SetTextAlignment(halign == HAlign::Left     ? DWRITE_TEXT_ALIGNMENT_LEADING
                          : halign == HAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                     : DWRITE_TEXT_ALIGNMENT_TRAILING);
    out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    out->SetWordWrapping(key.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (key.trim && !key.wrap) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        if (IDWriteInlineObject *sign = ellipsis(static_cast<Font>(key.font), fmt_)) out->SetTrimming(&trimming, sign);
    }
    const DWRITE_TEXT_RANGE whole{0, static_cast<UINT32>(text.size())};
    if (key.tabular) {
        if (!tabular_ && SUCCEEDED(dwrite_->CreateTypography(&tabular_)))
            tabular_->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
        if (tabular_) out->SetTypography(tabular_.Get(), whole);
    }
    if (key.tracking != 0) {
        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(out.As(&layout1))) layout1->SetCharacterSpacing(0, key.tracking, 0, whole);
    }
    return out;
}

float D2DCanvas::draw_text(std::wstring_view value, const Rect &r, const TextStyle &style, Color color) {
    if (value.empty() || r.empty()) return 0;
    const CachedLayout *l = layout(value, style, r.w, style.wrap ? r.h : 10000);
    if (!l) return 0;
    const DWRITE_TEXT_METRICS &metrics = l->metrics;
    float y = r.y;
    if (style.valign == VAlign::Center)
        y = r.y + (r.h - metrics.height) / 2;
    else if (style.valign == VAlign::Bottom)
        y = r.bottom() - metrics.height;
    target_->DrawTextLayout(D2D1::Point2F(r.x, y), l->layout.Get(), brush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    return metrics.widthIncludingTrailingWhitespace;
}

float D2DCanvas::measure_text(std::wstring_view value, Font font, float tracking, bool tabular) {
    if (value.empty()) return 0;
    TextStyle style;
    style.font = font;
    style.trim = false;
    style.tracking = tracking;
    style.tabular = tabular;
    const CachedLayout *l = layout(value, style, 10000, 10000);
    return l ? l->metrics.widthIncludingTrailingWhitespace : 0;
}

float D2DCanvas::baseline(Font font) {
    TextStyle style;
    style.font = font;
    const CachedLayout *l = layout(L"Ag", style, 1000, 1000);
    DWRITE_LINE_METRICS line{};
    UINT32 lines = 0;
    if (!l || FAILED(l->layout->GetLineMetrics(&line, 1, &lines)) || lines == 0) return font_spec(font).size;
    return line.baseline;
}

float D2DCanvas::line_height(Font font) {
    TextStyle style;
    style.font = font;
    const CachedLayout *l = layout(L"Ag", style, 1000, 1000);
    return l ? l->metrics.height : font_spec(font).size * 1.3f;
}

}  // namespace procyon::ui
