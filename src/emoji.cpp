// Gitmo - colour emoji for Dear ImGui.
//
// A font "loader" for ImGui 1.92 that draws Windows' Segoe UI Emoji in colour with
// DirectWrite + Direct2D (its colour layers, via IDWriteFactory2::TranslateColorGlyphRun),
// into a small bitmap per emoji, which ImGui keeps in its font texture like any glyph.
// Merged after the text font, so letters and digits still come from Segoe UI and only the
// characters Segoe UI lacks (the emoji) come from here. Each emoji is scaled to fit the
// text line, so it is never cut off in a text field or a message bubble.
#include "gitmo.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <d2d1.h>
#include <dwrite_2.h>
#include <wincodec.h>
#include <cmath>
#include <algorithm>
#include <vector>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {
IDWriteFactory2* g_dw;
ID2D1Factory* g_d2d;
IWICImagingFactory* g_wic;
IDWriteFontFace* g_face;
DWRITE_FONT_METRICS g_fm;
bool g_ready, g_tried;

bool Init() {
    if (g_tried) return g_ready;
    g_tried = true;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // (already initialised in another mode is fine)
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), (IUnknown**)&g_dw))) { Log("emoji: DirectWrite 2 not available - emoji in one colour"); return false; }
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) return false;
    IDWriteFontCollection* sys = nullptr;
    if (FAILED(g_dw->GetSystemFontCollection(&sys))) return false;
    UINT32 idx = 0; BOOL exists = FALSE;
    sys->FindFamilyName(L"Segoe UI Emoji", &idx, &exists);
    if (exists) {
        IDWriteFontFamily* fam = nullptr; IDWriteFont* font = nullptr;
        if (SUCCEEDED(sys->GetFontFamily(idx, &fam)) &&
            SUCCEEDED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)))
            font->CreateFontFace(&g_face);
        if (font) font->Release();
        if (fam) fam->Release();
    }
    sys->Release();
    if (!g_face) { Log("emoji: Segoe UI Emoji not installed - emoji in one colour"); return false; }
    g_face->GetMetrics(&g_fm);
    g_ready = true;
    Log("emoji: colour emoji ready (Segoe UI Emoji via DirectWrite)");
    return true;
}

UINT16 GlyphOf(ImWchar cp) {
    UINT32 c = (UINT32)cp; UINT16 gi = 0;
    if (g_face) g_face->GetGlyphIndices(&c, 1, &gi);
    return gi;
}

// Draw one glyph run (all its colour layers) into a w x h premultiplied BGRA bitmap.
bool Render(UINT16 gi, float em, float baseX, float baseY, int w, int h, std::vector<uint8_t>& bgra, bool& colored) {
    IWICBitmap* bmp = nullptr;
    if (FAILED(g_wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return false;
    ID2D1RenderTarget* rt = nullptr;
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(g_d2d->CreateWicBitmapRenderTarget(bmp, rp, &rt))) { bmp->Release(); return false; }
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ID2D1SolidColorBrush* brush = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);
    DWRITE_GLYPH_RUN run{};
    run.fontFace = g_face; run.fontEmSize = em; run.glyphCount = 1; run.glyphIndices = &gi;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    IDWriteColorGlyphRunEnumerator* layers = nullptr;
    HRESULT hr = g_dw->TranslateColorGlyphRun(baseX, baseY, &run, nullptr, DWRITE_MEASURING_MODE_NATURAL, nullptr, 0, &layers);
    colored = SUCCEEDED(hr) && layers;
    if (colored) {
        BOOL more = FALSE;
        while (SUCCEEDED(layers->MoveNext(&more)) && more) {
            const DWRITE_COLOR_GLYPH_RUN* lr = nullptr;
            if (FAILED(layers->GetCurrentRun(&lr)) || !lr) break;
            // palette index 0xFFFF = "use the text colour": black, the usual default
            D2D1_COLOR_F c = lr->paletteIndex == 0xFFFF ? D2D1::ColorF(0, 0, 0, 1)
                                                        : D2D1::ColorF(lr->runColor.r, lr->runColor.g, lr->runColor.b, lr->runColor.a);
            brush->SetColor(c);
            rt->DrawGlyphRun(D2D1::Point2F(lr->baselineOriginX, lr->baselineOriginY), &lr->glyphRun, brush);
        }
        layers->Release();
    } else {
        rt->DrawGlyphRun(D2D1::Point2F(baseX, baseY), &run, brush);   // no colour layers: white, tinted like text
    }
    bool ok = SUCCEEDED(rt->EndDraw());
    brush->Release(); rt->Release();
    if (ok) {
        bgra.resize((size_t)w * h * 4);
        ok = SUCCEEDED(bmp->CopyPixels(nullptr, w * 4, (UINT)bgra.size(), bgra.data()));
    }
    bmp->Release();
    return ok;
}

bool SrcInit(ImFontAtlas*, ImFontConfig*) { return Init(); }
void SrcDestroy(ImFontAtlas*, ImFontConfig*) {}
bool Contains(ImFontAtlas*, ImFontConfig*, ImWchar cp) {
    if (cp < 0x2000) return false;   // never letters, digits, '#' or '*': those stay Segoe UI
    return g_ready && GlyphOf(cp) != 0;
}
bool BakedInit(ImFontAtlas*, ImFontConfig*, ImFontBaked*, void*) { return true; }

bool LoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, ImWchar cp, ImFontGlyph* out, float* outAdvance) {
    if (!g_ready || cp < 0x2000) return false;
    UINT16 gi = GlyphOf(cp);
    if (!gi) return false;
    // the line the emoji must fit in (set up by the text font this one is merged into)
    float lineH = baked->Ascent - baked->Descent;
    if (lineH <= 0) lineH = baked->Size;
    float faceH = (float)(g_fm.ascent + g_fm.descent) / g_fm.designUnitsPerEm;   // emoji height per em
    float em = lineH / faceH * 0.98f;
    DWRITE_GLYPH_METRICS gm{};
    g_face->GetDesignGlyphMetrics(&gi, 1, &gm, FALSE);
    float advance = (float)gm.advanceWidth / g_fm.designUnitsPerEm * em;
    if (outAdvance) { *outAdvance = advance; return true; }

    const float density = src->RasterizerDensity * baked->RasterizerDensity;
    int w = std::max(1, (int)ceilf(advance * density)), h = std::max(1, (int)ceilf(lineH * density));
    float emPx = em * density;
    float ascPx = (float)g_fm.ascent / g_fm.designUnitsPerEm * emPx;
    float baseY = (h - faceH * emPx) * 0.5f + ascPx;   // centred in the line
    std::vector<uint8_t> px;
    bool colored = false;
    if (!Render(gi, emPx, 0.0f, baseY, w, h, px, colored)) return false;

    // premultiplied BGRA -> straight RGBA (what ImGui's font texture holds)
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        uint8_t b = px[i], g = px[i + 1], r = px[i + 2], a = px[i + 3];
        if (a && a < 255) { r = (uint8_t)std::min(255, r * 255 / a); g = (uint8_t)std::min(255, g * 255 / a); b = (uint8_t)std::min(255, b * 255 / a); }
        px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a;
    }
    out->Codepoint = cp;
    out->AdvanceX = advance;
    ImFontAtlasRectId pack = ImFontAtlasPackAddRect(atlas, w, h);
    if (pack == ImFontAtlasRectId_Invalid) return false;
    ImTextureRect* r = ImFontAtlasPackGetRect(atlas, pack);
    out->X0 = 0; out->Y0 = 0; out->X1 = w / density; out->Y1 = h / density;
    out->Visible = true;
    out->Colored = colored;
    out->PackId = pack;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, r, px.data(), ImTextureFormat_RGBA32, w * 4);
    return true;
}
}   // namespace

const ImFontLoader* Emoji_FontLoader() {
    static ImFontLoader l;
    l.Name = "Gitmo colour emoji";
    l.FontSrcInit = SrcInit;
    l.FontSrcDestroy = SrcDestroy;
    l.FontSrcContainsGlyph = Contains;
    l.FontBakedInit = BakedInit;
    l.FontBakedLoadGlyph = LoadGlyph;
    return &l;
}
