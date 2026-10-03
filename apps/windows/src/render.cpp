// Direct2D/DirectWrite renderer: the component vocabulary of design/components.md.
#include <dwrite_3.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "resource.h"
#include "ui.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace procyon::ui {

// ---------------------------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------------------------

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

D2D1_COLOR_F rgba(uint32_t value) {
    return D2D1::ColorF(((value >> 24) & 0xFF) / 255.0f, ((value >> 16) & 0xFF) / 255.0f,
                        ((value >> 8) & 0xFF) / 255.0f, (value & 0xFF) / 255.0f);
}

D2D1_COLOR_F with_alpha(D2D1_COLOR_F color, float alpha) {
    color.a = alpha;
    return color;
}

D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

bool system_prefers_dark() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

// ---------------------------------------------------------------------------------------------
// Renderer: lifecycle
// ---------------------------------------------------------------------------------------------

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr const wchar_t *kEmbeddedFamily = L"Inter";

bool has_family(IDWriteFactory *factory, const wchar_t *name) {
    ComPtr<IDWriteFontCollection> fonts;
    if (FAILED(factory->GetSystemFontCollection(&fonts))) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
}

// The bundled UI face (InterVariable.ttf, an RCDATA resource) as a private collection in the
// typographic family model, so its weight and optical-size axes can be set per text format. Empty
// when the loader is missing (before Windows 10 1703) or the resource is absent.
ComPtr<IDWriteFontCollection2> load_embedded_fonts(IDWriteFactory6 *factory) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(IDR_FONT_INTER), RT_RCDATA);
    if (!found) return nullptr;
    HGLOBAL loaded = LoadResource(module, found);
    const void *bytes = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = SizeofResource(module, found);
    if (!bytes || size == 0) return nullptr;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    if (FAILED(factory->CreateInMemoryFontFileLoader(&loader)) || FAILED(factory->RegisterFontFileLoader(loader.Get())))
        return nullptr;
    ComPtr<IDWriteFontFile> file;
    // Resource memory stays mapped for the life of the module, so no owner object is needed.
    if (FAILED(loader->CreateInMemoryFontFileReference(factory, bytes, size, nullptr, &file))) return nullptr;
    ComPtr<IDWriteFontSetBuilder1> builder;
    ComPtr<IDWriteFontSet> set;
    if (FAILED(factory->CreateFontSetBuilder(&builder)) || FAILED(builder->AddFontFile(file.Get())) ||
        FAILED(builder->CreateFontSet(&set)))
        return nullptr;
    ComPtr<IDWriteFontCollection2> collection;
    if (FAILED(factory->CreateFontCollectionFromFontSet(set.Get(), DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC, &collection)))
        return nullptr;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(collection->FindFamilyName(kEmbeddedFamily, &index, &exists)) || !exists) return nullptr;
    return collection;
}

struct FontDescription {
    tokens::FontSpec spec;
    int weight_override;  // 0 keeps the token weight
};

FontDescription describe(Font font) {
    using namespace tokens::font;
    switch (font) {
        case Font::Display: return {display, 0};
        case Font::Title: return {title, 0};
        case Font::Headline: return {headline, 0};
        case Font::Body: return {body, 0};
        case Font::BodyMedium: return {body, 500};
        case Font::BodySemibold: return {body, 600};
        case Font::Label: return {label, 0};
        case Font::LabelSemibold: return {label, 600};
        case Font::Caption: return {caption, 0};
        case Font::Metric: return {metric, 0};
        case Font::Mono: return {mono, 0};
        case Font::Stat: return {{17, 600, tokens::FontDesign::Rounded}, 0};
        case Font::Brand: return {{15, 700, tokens::FontDesign::Rounded}, 0};
    }
    return {body, 0};
}

}  // namespace

bool Renderer::init(HWND window) {
    window_ = window;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(dwrite_.GetAddressOf()))))
        return false;
    ui_family_ = has_family(dwrite_.Get(), L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    display_family_ =
        has_family(dwrite_.Get(), L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    mono_family_ = has_family(dwrite_.Get(), L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    // The macOS app sets its screens in SF Pro; Inter is the closest face that may ship on Windows.
    if (SUCCEEDED(dwrite_.As(&dwrite6_)) && dwrite6_) embedded_fonts_ = load_embedded_fonts(dwrite6_.Get());
    const float dashes[] = {3, 4};
    factory_->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND,
                                    10, D2D1_DASH_STYLE_CUSTOM, 0),
        dashes, 2, &dashed_);
    return true;
}

void Renderer::shutdown() {
    formats_.clear();
    brush_.Reset();
    target_.Reset();
    dashed_.Reset();
    dwrite_.Reset();
    factory_.Reset();
}

void Renderer::set_dpi(float dpi) {
    dpi_ = dpi;
    if (target_) target_->SetDpi(dpi, dpi);
}

void Renderer::resize(UINT width, UINT height) {
    if (target_) target_->Resize(D2D1::SizeU(width, height));
}

bool Renderer::begin(const Theme &theme) {
    theme_ = theme;
    if (!target_) {
        RECT rc{};
        GetClientRect(window_, &rc);
        const D2D1_SIZE_U size =
            D2D1::SizeU(static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top));
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        props.dpiX = props.dpiY = dpi_;
        if (FAILED(factory_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(window_, size), &target_)))
            return false;
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brush_);
    }
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->Clear(theme.background());
    clips_ = 0;
    return true;
}

bool Renderer::end() {
    while (clips_ > 0) pop_clip();
    const HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        brush_.Reset();
        target_.Reset();
        return false;
    }
    return SUCCEEDED(hr);
}

ID2D1SolidColorBrush *Renderer::brush(D2D1_COLOR_F color) {
    brush_->SetColor(color);
    return brush_.Get();
}

// ---------------------------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------------------------

void Renderer::fill(const Rect &r, D2D1_COLOR_F color) {
    if (r.empty() || color.a <= 0) return;
    target_->FillRectangle(r.d2d(), brush(color));
}

void Renderer::fill_round(const Rect &r, float radius, D2D1_COLOR_F color) {
    if (r.empty() || color.a <= 0) return;
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    target_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), brush(color));
}

void Renderer::stroke_round(const Rect &r, float radius, D2D1_COLOR_F color, float width) {
    if (r.empty() || color.a <= 0) return;
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    const Rect inner = r.inset(width / 2, width / 2);
    target_->DrawRoundedRectangle(D2D1::RoundedRect(inner.d2d(), radius, radius), brush(color), width);
}

void Renderer::fill_gradient_round(const Rect &r, float radius, D2D1_COLOR_F start, D2D1_COLOR_F end) {
    if (r.empty()) return;
    D2D1_GRADIENT_STOP stops[2] = {{0, start}, {1, end}};
    ComPtr<ID2D1GradientStopCollection> collection;
    if (FAILED(target_->CreateGradientStopCollection(stops, 2, &collection))) return;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.right(), r.bottom())),
            collection.Get(), &gradient)))
        return;
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    target_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), gradient.Get());
}

void Renderer::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float width, bool dashed) {
    target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush(color), width,
                      dashed ? dashed_.Get() : nullptr);
}

void Renderer::fill_circle(float cx, float cy, float radius, D2D1_COLOR_F color) {
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush(color));
}

void Renderer::stroke_circle(float cx, float cy, float radius, D2D1_COLOR_F color, float width) {
    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush(color), width);
}

void Renderer::push_clip(const Rect &r) {
    target_->PushAxisAlignedClip(r.d2d(), D2D1_ANTIALIAS_MODE_ALIASED);
    ++clips_;
}

void Renderer::pop_clip() {
    if (clips_ <= 0) return;
    target_->PopAxisAlignedClip();
    --clips_;
}

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------

IDWriteTextFormat *Renderer::format(Font font) {
    const int key = static_cast<int>(font);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();
    const FontDescription d = describe(font);
    const int weight = d.weight_override ? d.weight_override : d.spec.weight;
    // Segoe UI Variable ships two optical sizes and no rounded design: "Text" is tuned for body sizes
    // and "Display" for 20 and up, so the family follows the size (the system type ramp sets Body
    // Large 18 in Text and Subtitle 20 in Display). Mixing them at nearby sizes reads as two faces.
    const wchar_t *family = d.spec.design == tokens::FontDesign::Monospaced ? mono_family_.c_str()
                            : d.spec.size >= 20                             ? display_family_.c_str()
                                                                            : ui_family_.c_str();
    ComPtr<IDWriteTextFormat> created;
    if (embedded_fonts_ && d.spec.design != tokens::FontDesign::Monospaced) {
        // Optical size follows the text size (Inter clamps it to its 14-32 range), as SF Pro does.
        const DWRITE_FONT_AXIS_VALUE axes[] = {{DWRITE_FONT_AXIS_TAG_WEIGHT, static_cast<float>(weight)},
                                               {DWRITE_FONT_AXIS_TAG_OPTICAL_SIZE, d.spec.size}};
        ComPtr<IDWriteTextFormat3> created3;
        if (SUCCEEDED(dwrite6_->CreateTextFormat(kEmbeddedFamily, embedded_fonts_.Get(), axes, 2, d.spec.size, L"en-us",
                                                 &created3)))
            created = created3;
    }
    if (!created && FAILED(dwrite_->CreateTextFormat(family, nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight),
                                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, d.spec.size,
                                                     L"en-us", &created)))
        return nullptr;
    created->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    formats_[key] = created;
    return created.Get();
}

ComPtr<IDWriteTextLayout> Renderer::layout(std::wstring_view value, const TextStyle &style, float width, float height) {
    IDWriteTextFormat *fmt_ = format(style.font);
    if (!fmt_) return nullptr;
    std::wstring text(value);
    if (style.uppercase)
        for (wchar_t &c : text) c = static_cast<wchar_t>(std::towupper(c));
    ComPtr<IDWriteTextLayout> out;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt_, std::max(width, 1.0f),
                                         std::max(height, 1.0f), &out)))
        return nullptr;
    out->SetTextAlignment(style.halign == HAlign::Left     ? DWRITE_TEXT_ALIGNMENT_LEADING
                          : style.halign == HAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                           : DWRITE_TEXT_ALIGNMENT_TRAILING);
    out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    out->SetWordWrapping(style.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (style.trim && !style.wrap) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(out.Get(), &ellipsis)))
            out->SetTrimming(&trimming, ellipsis.Get());
    }
    const DWRITE_TEXT_RANGE whole{0, static_cast<UINT32>(text.size())};
    if (style.tabular) {
        ComPtr<IDWriteTypography> typography;
        if (SUCCEEDED(dwrite_->CreateTypography(&typography))) {
            typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
            out->SetTypography(typography.Get(), whole);
        }
    }
    if (style.tracking != 0) {
        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(out.As(&layout1))) layout1->SetCharacterSpacing(0, style.tracking, 0, whole);
    }
    return out;
}

float Renderer::text(std::wstring_view value, const Rect &r, const TextStyle &style, D2D1_COLOR_F color) {
    if (value.empty() || r.empty()) return 0;
    ComPtr<IDWriteTextLayout> l = layout(value, style, r.w, style.wrap ? r.h : 10000);
    if (!l) return 0;
    DWRITE_TEXT_METRICS metrics{};
    l->GetMetrics(&metrics);
    float y = r.y;
    if (style.valign == VAlign::Center)
        y = r.y + (r.h - metrics.height) / 2;
    else if (style.valign == VAlign::Bottom)
        y = r.bottom() - metrics.height;
    target_->DrawTextLayout(D2D1::Point2F(r.x, y), l.Get(), brush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    return metrics.widthIncludingTrailingWhitespace;
}

float Renderer::measure(std::wstring_view value, Font font, float tracking) {
    if (value.empty()) return 0;
    TextStyle style;
    style.font = font;
    style.trim = false;
    style.tracking = tracking;
    ComPtr<IDWriteTextLayout> l = layout(value, style, 10000, 10000);
    if (!l) return 0;
    DWRITE_TEXT_METRICS metrics{};
    l->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

float Renderer::line_height(Font font) {
    TextStyle style;
    style.font = font;
    ComPtr<IDWriteTextLayout> l = layout(L"Ag", style, 1000, 1000);
    if (!l) return describe(font).spec.size * 1.3f;
    DWRITE_TEXT_METRICS metrics{};
    l->GetMetrics(&metrics);
    return metrics.height;
}

// ---------------------------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------------------------

void Renderer::card(const Rect &r, std::optional<MetricKind> tint, float radius) {
    if (r.empty()) return;
    // The shadow sits on the static card background only (rule 4): a few stacked translucent
    // rounded rectangles stand in for a blur, which Direct2D's window target doesn't offer.
    const tokens::ShadowSpec &shadow = theme_.card_shadow();
    const D2D1_COLOR_F shadow_color = rgba(shadow.color);
    const int layers = 4;
    for (int i = layers; i >= 1; --i) {
        const float spread = shadow.radius * i / layers;
        const Rect s = r.offset(0, shadow.y).inset(-spread / 2, -spread / 2);
        fill_round(s, radius + spread / 2, with_alpha(shadow_color, shadow.opacity / layers));
    }
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
    // Soft colored shadow: static, so a faux blur is fine.
    for (int i = 3; i >= 1; --i) {
        const float spread = r.w * 0.12f * i;
        fill_round(r.offset(0, r.w * 0.06f).inset(-spread / 2, -spread / 2), radius + spread / 2,
                   with_alpha(rgba(style.start), 0.35f / 3));
    }
    fill_gradient_round(r, radius, rgba(style.start), rgba(style.end));
    stroke_round(r, radius, with_alpha(D2D1::ColorF(D2D1::ColorF::White), 0.25f), 0.5f);
    glyph(kind, r.inset(r.w * 0.25f, r.h * 0.25f), D2D1::ColorF(D2D1::ColorF::White));
}

void Renderer::glyph(MetricKind kind, const Rect &r, D2D1_COLOR_F color) {
    const float w = r.w, h = r.h, x = r.x, y = r.y;
    const float t = std::max(1.0f, w / 10);
    ID2D1SolidColorBrush *b = brush(color);
    switch (kind) {
        case MetricKind::Cpu: {
            const Rect die = r.inset(w * 0.2f, h * 0.2f);
            target_->DrawRoundedRectangle(D2D1::RoundedRect(die.d2d(), w * 0.08f, w * 0.08f), b, t);
            target_->FillRectangle(die.inset(w * 0.17f, h * 0.17f).d2d(), b);
            for (int i = 0; i < 3; ++i) {
                const float p = die.x + die.w * (0.25f + 0.25f * i);
                target_->DrawLine(D2D1::Point2F(p, y), D2D1::Point2F(p, die.y), b, t);
                target_->DrawLine(D2D1::Point2F(p, die.bottom()), D2D1::Point2F(p, y + h), b, t);
                const float q = die.y + die.h * (0.25f + 0.25f * i);
                target_->DrawLine(D2D1::Point2F(x, q), D2D1::Point2F(die.x, q), b, t);
                target_->DrawLine(D2D1::Point2F(die.right(), q), D2D1::Point2F(x + w, q), b, t);
            }
            break;
        }
        case MetricKind::Memory: {
            const Rect board{x, y + h * 0.2f, w, h * 0.5f};
            target_->DrawRoundedRectangle(D2D1::RoundedRect(board.d2d(), w * 0.06f, w * 0.06f), b, t);
            for (int i = 0; i < 3; ++i)
                target_->FillRectangle(
                    Rect{x + w * (0.12f + 0.3f * i), board.y + board.h * 0.25f, w * 0.18f, board.h * 0.5f}.d2d(), b);
            for (int i = 0; i < 6; ++i)
                target_->FillRectangle(
                    Rect{x + w * (0.08f + 0.15f * i), board.bottom() + h * 0.05f, w * 0.08f, h * 0.18f}.d2d(), b);
            break;
        }
        case MetricKind::Disk: {
            const Rect slab{x, y + h * 0.3f, w, h * 0.4f};
            target_->DrawRoundedRectangle(D2D1::RoundedRect(slab.d2d(), h * 0.1f, h * 0.1f), b, t);
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(slab.right() - w * 0.2f, slab.cy()), w * 0.07f, w * 0.07f),
                                 b);
            target_->DrawLine(D2D1::Point2F(x + w * 0.15f, slab.cy()), D2D1::Point2F(x + w * 0.55f, slab.cy()), b, t);
            break;
        }
        case MetricKind::Network: {
            const D2D1_POINT_2F hub = D2D1::Point2F(x + w / 2, y + h * 0.62f);
            const D2D1_POINT_2F nodes[3] = {D2D1::Point2F(x + w * 0.15f, y + h * 0.2f),
                                            D2D1::Point2F(x + w * 0.85f, y + h * 0.2f),
                                            D2D1::Point2F(x + w / 2, y + h * 0.95f)};
            for (const auto &n : nodes) {
                target_->DrawLine(hub, n, b, t);
                target_->FillEllipse(D2D1::Ellipse(n, w * 0.12f, w * 0.12f), b);
            }
            target_->FillEllipse(D2D1::Ellipse(hub, w * 0.15f, w * 0.15f), b);
            break;
        }
        case MetricKind::Gpu: {
            const Rect card{x, y + h * 0.15f, w, h * 0.6f};
            target_->DrawRoundedRectangle(D2D1::RoundedRect(card.d2d(), w * 0.06f, w * 0.06f), b, t);
            const D2D1_POINT_2F c = D2D1::Point2F(card.x + card.w * 0.62f, card.cy());
            target_->DrawEllipse(D2D1::Ellipse(c, card.h * 0.3f, card.h * 0.3f), b, t);
            target_->FillEllipse(D2D1::Ellipse(c, card.h * 0.08f, card.h * 0.08f), b);
            target_->FillRectangle(Rect{card.x + w * 0.12f, card.y + card.h * 0.3f, w * 0.22f, card.h * 0.4f}.d2d(), b);
            target_->FillRectangle(Rect{x + w * 0.1f, card.bottom(), w * 0.6f, h * 0.12f}.d2d(), b);
            break;
        }
        case MetricKind::Battery: {
            const Rect body{x, y + h * 0.25f, w * 0.85f, h * 0.5f};
            target_->DrawRoundedRectangle(D2D1::RoundedRect(body.d2d(), h * 0.08f, h * 0.08f), b, t);
            target_->FillRectangle(
                Rect{body.right() + w * 0.03f, body.y + body.h * 0.3f, w * 0.1f, body.h * 0.4f}.d2d(), b);
            target_->FillRoundedRectangle(D2D1::RoundedRect(body.inset(w * 0.1f, h * 0.1f).d2d(), h * 0.04f, h * 0.04f),
                                          b);
            break;
        }
        case MetricKind::Energy: {
            ComPtr<ID2D1PathGeometry> path;
            if (FAILED(factory_->CreatePathGeometry(&path))) return;
            ComPtr<ID2D1GeometrySink> sink;
            path->Open(&sink);
            sink->BeginFigure(D2D1::Point2F(x + w * 0.6f, y), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(x + w * 0.15f, y + h * 0.58f));
            sink->AddLine(D2D1::Point2F(x + w * 0.48f, y + h * 0.58f));
            sink->AddLine(D2D1::Point2F(x + w * 0.4f, y + h));
            sink->AddLine(D2D1::Point2F(x + w * 0.85f, y + h * 0.4f));
            sink->AddLine(D2D1::Point2F(x + w * 0.52f, y + h * 0.4f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            target_->FillGeometry(path.Get(), b);
            break;
        }
    }
}

ComPtr<ID2D1PathGeometry> Renderer::sparkline_path(const Rect &r, const float *values, size_t count, size_t window,
                                                   float max, bool closed, float baseline) {
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory_->CreatePathGeometry(&path)) || count == 0 || window < 2) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);
    const float step = r.w / static_cast<float>(window - 1);
    const size_t first_slot = count >= window ? 0 : window - count;
    const size_t first = count >= window ? count - window : 0;
    auto point = [&](size_t i) {
        const float v = std::clamp(max > 0 ? values[first + i] / max : 0.0f, 0.0f, 1.0f);
        return D2D1::Point2F(r.x + step * static_cast<float>(first_slot + i), r.bottom() - v * r.h);
    };
    const size_t n = std::min(count, window);
    const D2D1_POINT_2F start = point(0);
    sink->BeginFigure(closed ? D2D1::Point2F(start.x, baseline) : start,
                      closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    if (closed) sink->AddLine(start);
    for (size_t i = 1; i < n; ++i) sink->AddLine(point(i));
    if (closed) sink->AddLine(D2D1::Point2F(point(n - 1).x, baseline));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    return path;
}

void Renderer::sparkline(const Rect &r, const float *values, size_t count, size_t window, float max, MetricKind kind,
                         const SparklineOptions &options) {
    if (r.empty()) return;
    const tokens::MetricStyle &style = metric_style(kind);
    const D2D1_COLOR_F start = options.color_override ? *options.color_override : rgba(style.start);
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
    push_clip(r.inset(-options.line_width * 4, -options.line_width * 4));
    ComPtr<ID2D1StrokeStyle> round;
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                            D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                nullptr, 0, &round);
    if (options.area) {
        ComPtr<ID2D1PathGeometry> area = sparkline_path(plot, values, count, window, max, true, r.bottom());
        D2D1_GRADIENT_STOP stops[2] = {{0, with_alpha(start, tokens::chart::fillOpacityTop)},
                                       {1, with_alpha(start, tokens::chart::fillOpacityBottom)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        if (area && SUCCEEDED(target_->CreateGradientStopCollection(stops, 2, &collection)) &&
            SUCCEEDED(target_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.bottom())),
                collection.Get(), &gradient)))
            target_->FillGeometry(area.Get(), gradient.Get());
    }
    ComPtr<ID2D1PathGeometry> stroke = sparkline_path(plot, values, count, window, max, false, 0);
    if (!stroke) {
        pop_clip();
        return;
    }
    if (options.halo) {
        // A glow is wider translucent strokes, never a blur (rule 4).
        target_->DrawGeometry(stroke.Get(), brush(with_alpha(start, 0.08f)),
                              options.line_width + tokens::chart::glowRadius * 1.6f, round.Get());
        target_->DrawGeometry(stroke.Get(), brush(with_alpha(start, 0.14f)),
                              options.line_width + tokens::chart::glowRadius * 0.8f, round.Get());
    }
    target_->DrawGeometry(stroke.Get(), brush(start), options.line_width, round.Get());
    if (options.end_dot) {
        // The newest sample always sits at the right edge.
        const float v = std::clamp(max > 0 ? values[count - 1] / max : 0.0f, 0.0f, 1.0f);
        fill_circle(plot.right(), plot.bottom() - v * plot.h, options.line_width * 1.4f, start);
    }
    pop_clip();
}

void Renderer::ring_gauge(float cx, float cy, float radius, float fraction, MetricKind kind, float width) {
    const tokens::MetricStyle &style = metric_style(kind);
    stroke_circle(cx, cy, radius, theme_.track(), width);
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction <= 0) return;
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory_->CreatePathGeometry(&path))) return;
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);
    const float angle = std::max(0.001f, std::min(fraction, 0.9999f)) * 2 * kPi;
    const D2D1_POINT_2F start = D2D1::Point2F(cx, cy - radius);
    const D2D1_POINT_2F end = D2D1::Point2F(cx + radius * std::sin(angle), cy - radius * std::cos(angle));
    sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(radius, radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  angle > kPi ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    D2D1_GRADIENT_STOP stops[2] = {{0, rgba(style.start)}, {1, rgba(style.end)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(target_->CreateGradientStopCollection(stops, 2, &collection)) ||
        FAILED(target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx - radius, cy - radius),
                                                D2D1::Point2F(cx + radius, cy + radius)),
            collection.Get(), &gradient)))
        return;
    ComPtr<ID2D1StrokeStyle> round;
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0,
                                &round);
    // Halo: a wider translucent arc in the end color.
    target_->DrawGeometry(path.Get(), brush(with_alpha(rgba(style.end), 0.18f)), width * 1.9f, round.Get());
    target_->DrawGeometry(path.Get(), gradient.Get(), width, round.Get());
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

void Renderer::usage_bar(const Rect &r, float fraction, D2D1_COLOR_F color) {
    fill_round(r, tokens::radius::pill, theme_.track());
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction <= 0) return;
    Rect filled = r;
    filled.w = std::max(r.h, r.w * fraction);
    fill_round(filled, tokens::radius::pill, color);
}

void Renderer::stacked_bar(const Rect &r, const Segment *segments, size_t count) {
    fill_round(r, tokens::radius::sm, theme_.track());
    ComPtr<ID2D1RoundedRectangleGeometry> mask;
    const float radius = std::min(r.h / 2, tokens::radius::sm);
    if (FAILED(factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r.d2d(), radius, radius), &mask))) return;
    ComPtr<ID2D1Layer> layer;
    target_->CreateLayer(nullptr, &layer);
    target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), mask.Get()), layer.Get());
    float x = r.x;
    for (size_t i = 0; i < count; ++i) {
        const float w = r.w * std::clamp(segments[i].fraction, 0.0f, 1.0f);
        if (w <= 0) continue;
        fill_round(Rect{x, r.y, std::max(0.0f, w - tokens::space::xxs), r.h}, tokens::radius::xs, segments[i].color);
        x += w;
    }
    target_->PopLayer();
}

D2D1_COLOR_F Renderer::tone_color(const Theme &theme, Tone tone) {
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
    const D2D1_COLOR_F color = tone_color(theme_, tone);
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
    const D2D1_COLOR_F color = live ? theme_.success() : theme_.text_tertiary();
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
    D2D1_COLOR_F fill = on ? theme_.accent() : with_alpha(theme_.text(), theme_.dark ? 0.18f : 0.12f);
    if (!enabled) fill = with_alpha(fill, 0.45f);
    fill_round(track, tokens::radius::pill, fill);
    const float kx = on ? track.right() - 10 - 2 : track.x + 10 + 2;
    fill_circle(kx, track.cy(), 8, with_alpha(D2D1::ColorF(D2D1::ColorF::White), enabled ? 1.0f : 0.7f));
}

void Renderer::icon_button(const Rect &r, Symbol s, D2D1_COLOR_F color, bool hovered, bool enabled) {
    if (hovered && enabled) fill_round(r, tokens::radius::sm, with_alpha(theme_.text(), 0.06f));
    symbol(s, Rect{r.cx() - 8, r.cy() - 8, 16, 16}, enabled ? color : theme_.text_tertiary(), 1.5f);
}

void Renderer::symbol(Symbol s, const Rect &r, D2D1_COLOR_F color, float width) {
    ID2D1SolidColorBrush *b = brush(color);
    const float cx = r.cx(), cy = r.cy(), k = std::min(r.w, r.h) / 2;
    ComPtr<ID2D1StrokeStyle> round;
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                            D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                nullptr, 0, &round);
    auto seg = [&](float x1, float y1, float x2, float y2) {
        target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, width, round.Get());
    };
    auto filled_path = [&](std::initializer_list<D2D1_POINT_2F> points) {
        ComPtr<ID2D1PathGeometry> path;
        if (FAILED(factory_->CreatePathGeometry(&path))) return;
        ComPtr<ID2D1GeometrySink> sink;
        path->Open(&sink);
        bool first = true;
        for (const auto &p : points) {
            if (first)
                sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
            else
                sink->AddLine(p);
            first = false;
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        target_->FillGeometry(path.Get(), b);
    };
    auto arc = [&](D2D1_POINT_2F from, D2D1_POINT_2F to, float radius, bool clockwise, bool large) {
        ComPtr<ID2D1PathGeometry> path;
        if (FAILED(factory_->CreatePathGeometry(&path))) return;
        ComPtr<ID2D1GeometrySink> sink;
        path->Open(&sink);
        sink->BeginFigure(from, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(
            D2D1::ArcSegment(to, D2D1::SizeF(radius, radius), 0,
                             clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                             large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        target_->DrawGeometry(path.Get(), b, width, round.Get());
    };
    switch (s) {
        case Symbol::ChevronRight:
            seg(cx - k * 0.3f, cy - k * 0.6f, cx + k * 0.3f, cy);
            seg(cx + k * 0.3f, cy, cx - k * 0.3f, cy + k * 0.6f);
            break;
        case Symbol::ChevronDown:
            seg(cx - k * 0.6f, cy - k * 0.3f, cx, cy + k * 0.3f);
            seg(cx, cy + k * 0.3f, cx + k * 0.6f, cy - k * 0.3f);
            break;
        case Symbol::Search:
            target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx - k * 0.15f, cy - k * 0.15f), k * 0.55f, k * 0.55f), b,
                                 width);
            seg(cx + k * 0.3f, cy + k * 0.3f, cx + k * 0.8f, cy + k * 0.8f);
            break;
        case Symbol::Close:
            seg(cx - k * 0.5f, cy - k * 0.5f, cx + k * 0.5f, cy + k * 0.5f);
            seg(cx + k * 0.5f, cy - k * 0.5f, cx - k * 0.5f, cy + k * 0.5f);
            break;
        case Symbol::Lock: {
            const Rect body{cx - k * 0.6f, cy - k * 0.05f, k * 1.2f, k * 0.9f};
            target_->FillRoundedRectangle(D2D1::RoundedRect(body.d2d(), k * 0.15f, k * 0.15f), b);
            arc(D2D1::Point2F(cx - k * 0.35f, cy - k * 0.1f), D2D1::Point2F(cx + k * 0.35f, cy - k * 0.1f), k * 0.35f,
                true, false);
            break;
        }
        case Symbol::Check:
            seg(cx - k * 0.6f, cy, cx - k * 0.15f, cy + k * 0.45f);
            seg(cx - k * 0.15f, cy + k * 0.45f, cx + k * 0.65f, cy - k * 0.45f);
            break;
        case Symbol::Pause:
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(Rect{cx - k * 0.55f, cy - k * 0.6f, k * 0.35f, k * 1.2f}.d2d(), 1, 1), b);
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(Rect{cx + k * 0.2f, cy - k * 0.6f, k * 0.35f, k * 1.2f}.d2d(), 1, 1), b);
            break;
        case Symbol::Play:
            filled_path({D2D1::Point2F(cx - k * 0.45f, cy - k * 0.6f), D2D1::Point2F(cx + k * 0.6f, cy),
                         D2D1::Point2F(cx - k * 0.45f, cy + k * 0.6f)});
            break;
        case Symbol::Gear: {
            target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), k * 0.3f, k * 0.3f), b, width);
            for (int i = 0; i < 8; ++i) {
                const float a = i * kPi / 4;
                seg(cx + std::cos(a) * k * 0.55f, cy + std::sin(a) * k * 0.55f, cx + std::cos(a) * k * 0.85f,
                    cy + std::sin(a) * k * 0.85f);
            }
            break;
        }
        case Symbol::Gears: {
            auto gear = [&](float gx, float gy, float gr) {
                target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(gx, gy), gr * 0.4f, gr * 0.4f), b, width);
                for (int i = 0; i < 6; ++i) {
                    const float a = i * kPi / 3;
                    seg(gx + std::cos(a) * gr * 0.6f, gy + std::sin(a) * gr * 0.6f, gx + std::cos(a) * gr,
                        gy + std::sin(a) * gr);
                }
            };
            gear(cx - k * 0.35f, cy - k * 0.3f, k * 0.6f);
            gear(cx + k * 0.4f, cy + k * 0.4f, k * 0.5f);
            break;
        }
        case Symbol::Info:
            target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), k * 0.8f, k * 0.8f), b, width);
            seg(cx, cy - k * 0.1f, cx, cy + k * 0.45f);
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy - k * 0.4f), width * 0.8f, width * 0.8f), b);
            break;
        case Symbol::Warning: {
            ComPtr<ID2D1PathGeometry> path;
            if (SUCCEEDED(factory_->CreatePathGeometry(&path))) {
                ComPtr<ID2D1GeometrySink> sink;
                path->Open(&sink);
                sink->BeginFigure(D2D1::Point2F(cx, cy - k * 0.8f), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(cx + k * 0.85f, cy + k * 0.7f));
                sink->AddLine(D2D1::Point2F(cx - k * 0.85f, cy + k * 0.7f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                target_->DrawGeometry(path.Get(), b, width, round.Get());
            }
            seg(cx, cy - k * 0.25f, cx, cy + k * 0.2f);
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + k * 0.45f), width * 0.8f, width * 0.8f), b);
            break;
        }
        case Symbol::Plus:
            seg(cx - k * 0.6f, cy, cx + k * 0.6f, cy);
            seg(cx, cy - k * 0.6f, cx, cy + k * 0.6f);
            break;
        case Symbol::Minus: seg(cx - k * 0.6f, cy, cx + k * 0.6f, cy); break;
        case Symbol::Dot: target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), k * 0.35f, k * 0.35f), b); break;
        case Symbol::Grid:
            for (int i = 0; i < 4; ++i) {
                const float gx = cx + (i % 2 ? k * 0.1f : -k * 0.9f), gy = cy + (i / 2 ? k * 0.1f : -k * 0.9f);
                target_->FillRoundedRectangle(D2D1::RoundedRect(Rect{gx, gy, k * 0.8f, k * 0.8f}.d2d(), 2, 2), b);
            }
            break;
        case Symbol::List:
            target_->DrawRoundedRectangle(
                D2D1::RoundedRect(Rect{cx - k * 0.9f, cy - k * 0.7f, k * 1.8f, k * 1.4f}.d2d(), 2, 2), b, width);
            for (int i = 0; i < 3; ++i) {
                const float ly = cy - k * 0.35f + i * k * 0.35f;
                seg(cx - k * 0.5f, ly, cx + k * 0.5f, ly);
            }
            break;
        case Symbol::Power: {
            const float a = kPi * 0.25f;
            arc(D2D1::Point2F(cx - std::sin(a) * k * 0.8f, cy - std::cos(a) * k * 0.8f),
                D2D1::Point2F(cx + std::sin(a) * k * 0.8f, cy - std::cos(a) * k * 0.8f), k * 0.8f, false, true);
            seg(cx, cy - k * 0.95f, cx, cy - k * 0.1f);
            break;
        }
        case Symbol::Ports:
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - k * 0.6f, cy - k * 0.5f), k * 0.25f, k * 0.25f), b);
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + k * 0.6f, cy - k * 0.5f), k * 0.25f, k * 0.25f), b);
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + k * 0.6f), k * 0.25f, k * 0.25f), b);
            seg(cx - k * 0.6f, cy - k * 0.5f, cx, cy + k * 0.1f);
            seg(cx + k * 0.6f, cy - k * 0.5f, cx, cy + k * 0.1f);
            seg(cx, cy + k * 0.1f, cx, cy + k * 0.6f);
            break;
        case Symbol::Desktop:
            target_->DrawRoundedRectangle(
                D2D1::RoundedRect(Rect{cx - k * 0.9f, cy - k * 0.7f, k * 1.8f, k * 1.2f}.d2d(), 2, 2), b, width);
            seg(cx - k * 0.4f, cy + k * 0.85f, cx + k * 0.4f, cy + k * 0.85f);
            seg(cx, cy + k * 0.5f, cx, cy + k * 0.85f);
            break;
        case Symbol::Clock:
            target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), k * 0.8f, k * 0.8f), b, width);
            seg(cx, cy - k * 0.45f, cx, cy);
            seg(cx, cy, cx + k * 0.35f, cy + k * 0.2f);
            break;
        case Symbol::Sparkle:
            filled_path({D2D1::Point2F(cx, cy - k), D2D1::Point2F(cx + k * 0.25f, cy - k * 0.25f),
                         D2D1::Point2F(cx + k, cy), D2D1::Point2F(cx + k * 0.25f, cy + k * 0.25f),
                         D2D1::Point2F(cx, cy + k), D2D1::Point2F(cx - k * 0.25f, cy + k * 0.25f),
                         D2D1::Point2F(cx - k, cy), D2D1::Point2F(cx - k * 0.25f, cy - k * 0.25f)});
            break;
        case Symbol::Waveform:
            seg(cx - k * 0.9f, cy, cx - k * 0.5f, cy);
            seg(cx - k * 0.5f, cy, cx - k * 0.3f, cy - k * 0.6f);
            seg(cx - k * 0.3f, cy - k * 0.6f, cx, cy + k * 0.6f);
            seg(cx, cy + k * 0.6f, cx + k * 0.3f, cy - k * 0.3f);
            seg(cx + k * 0.3f, cy - k * 0.3f, cx + k * 0.5f, cy);
            seg(cx + k * 0.5f, cy, cx + k * 0.9f, cy);
            break;
        case Symbol::Flame:
            filled_path({D2D1::Point2F(cx, cy - k), D2D1::Point2F(cx + k * 0.55f, cy - k * 0.2f),
                         D2D1::Point2F(cx + k * 0.65f, cy + k * 0.45f), D2D1::Point2F(cx + k * 0.25f, cy + k),
                         D2D1::Point2F(cx - k * 0.25f, cy + k), D2D1::Point2F(cx - k * 0.65f, cy + k * 0.45f),
                         D2D1::Point2F(cx - k * 0.45f, cy - k * 0.25f), D2D1::Point2F(cx - k * 0.1f, cy - k * 0.1f)});
            break;
        case Symbol::Bullets:
            for (int i = 0; i < 3; ++i) {
                const float ly = cy - k * 0.55f + i * k * 0.55f;
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - k * 0.7f, ly), width * 0.7f, width * 0.7f), b);
                seg(cx - k * 0.3f, ly, cx + k * 0.9f, ly);
            }
            break;
        case Symbol::Drive:
            target_->DrawRoundedRectangle(
                D2D1::RoundedRect(Rect{cx - k * 0.9f, cy - k * 0.45f, k * 1.8f, k * 0.9f}.d2d(), 2, 2), b, width);
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + k * 0.5f, cy), width * 0.7f, width * 0.7f), b);
            break;
        case Symbol::Thermo:
            target_->DrawRoundedRectangle(
                D2D1::RoundedRect(Rect{cx - k * 0.25f, cy - k * 0.95f, k * 0.5f, k * 1.2f}.d2d(), k * 0.25f, k * 0.25f),
                b, width);
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + k * 0.55f), k * 0.42f, k * 0.42f), b);
            seg(cx, cy - k * 0.3f, cx, cy + k * 0.5f);
            break;
        case Symbol::Refresh: {
            // arrow.clockwise: a circle open at the top right, swept clockwise from the right-hand
            // end to the top end, where the arrowhead points on along the circle.
            const float radius = k * 0.72f;
            const float start = -10 * kPi / 180, end = -60 * kPi / 180;  // screen angles, y down
            const D2D1_POINT_2F from(cx + std::cos(start) * radius, cy + std::sin(start) * radius);
            const D2D1_POINT_2F to(cx + std::cos(end) * radius, cy + std::sin(end) * radius);
            arc(from, to, radius, true, true);
            // Head: back along the tangent (clockwise direction is (-sin, cos)), out along the normal.
            const float tx = -std::sin(end), ty = std::cos(end);
            const float nx = std::cos(end), ny = std::sin(end);
            const float h = k * 0.45f, w = k * 0.42f;
            seg(to.x, to.y, to.x - tx * h + nx * w, to.y - ty * h + ny * w);
            seg(to.x, to.y, to.x - tx * h - nx * w, to.y - ty * h - ny * w);
            break;
        }
        case Symbol::Stack:
            for (int i = 0; i < 3; ++i) {
                const float ly = cy - k * 0.5f + i * k * 0.5f;
                seg(cx - k * 0.8f, ly, cx, ly + k * 0.35f);
                seg(cx, ly + k * 0.35f, cx + k * 0.8f, ly);
            }
            break;
        case Symbol::Gauge:
            arc(D2D1::Point2F(cx - k * 0.8f, cy + k * 0.4f), D2D1::Point2F(cx + k * 0.8f, cy + k * 0.4f), k * 0.85f,
                true, true);
            seg(cx, cy + k * 0.3f, cx + k * 0.35f, cy - k * 0.3f);
            break;
        case Symbol::Expand:
            seg(cx - k * 0.5f, cy - k * 0.2f, cx, cy - k * 0.7f);
            seg(cx, cy - k * 0.7f, cx + k * 0.5f, cy - k * 0.2f);
            seg(cx - k * 0.5f, cy + k * 0.2f, cx, cy + k * 0.7f);
            seg(cx, cy + k * 0.7f, cx + k * 0.5f, cy + k * 0.2f);
            break;
        case Symbol::Collapse:
            seg(cx - k * 0.5f, cy - k * 0.8f, cx, cy - k * 0.3f);
            seg(cx, cy - k * 0.3f, cx + k * 0.5f, cy - k * 0.8f);
            seg(cx - k * 0.5f, cy + k * 0.8f, cx, cy + k * 0.3f);
            seg(cx, cy + k * 0.3f, cx + k * 0.5f, cy + k * 0.8f);
            break;
        case Symbol::Pin:
            // A push pin: head, a wider collar, the needle.
            filled_path({D2D1::Point2F(cx - k * 0.35f, cy - k * 0.9f), D2D1::Point2F(cx + k * 0.35f, cy - k * 0.9f),
                         D2D1::Point2F(cx + k * 0.3f, cy - k * 0.1f), D2D1::Point2F(cx + k * 0.7f, cy + k * 0.25f),
                         D2D1::Point2F(cx - k * 0.7f, cy + k * 0.25f), D2D1::Point2F(cx - k * 0.3f, cy - k * 0.1f)});
            seg(cx, cy + k * 0.25f, cx, cy + k * 0.95f);
            break;
        case Symbol::Bell:
            arc(D2D1::Point2F(cx - k * 0.6f, cy + k * 0.3f), D2D1::Point2F(cx + k * 0.6f, cy + k * 0.3f), k * 0.6f,
                true, true);
            seg(cx - k * 0.85f, cy + k * 0.45f, cx + k * 0.85f, cy + k * 0.45f);
            seg(cx - k * 0.6f, cy + k * 0.3f, cx - k * 0.85f, cy + k * 0.45f);
            seg(cx + k * 0.6f, cy + k * 0.3f, cx + k * 0.85f, cy + k * 0.45f);
            seg(cx - k * 0.2f, cy + k * 0.75f, cx + k * 0.2f, cy + k * 0.75f);
            break;
        case Symbol::Peak:
            seg(cx - k * 0.8f, cy - k * 0.8f, cx + k * 0.8f, cy - k * 0.8f);
            seg(cx, cy - k * 0.5f, cx, cy + k * 0.85f);
            seg(cx - k * 0.5f, cy, cx, cy - k * 0.5f);
            seg(cx, cy - k * 0.5f, cx + k * 0.5f, cy);
            break;
    }
}

}  // namespace procyon::ui
