// Gitmo - colour emoji for Dear ImGui.
//
// A font "loader" for ImGui 1.92 that draws Windows' Segoe UI Emoji in colour with
// DirectWrite + Direct2D (its colour layers, via IDWriteFactory2::TranslateColorGlyphRun),
// into a small bitmap per emoji, which ImGui keeps in its font texture like any glyph.
// Merged after the text font, so letters and digits still come from Segoe UI and only the
// characters Segoe UI lacks (the emoji) come from here. Each emoji is scaled to fit the
// text line, so it is never cut off in a text field or a message bubble.
//
// Limits: every code point is drawn on its own, so joined sequences (family, skin tones, flags)
// show as their separate parts, and zero-width joiners / variation selectors draw nothing.
#include "gitmo.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <d2d1.h>
#include <dwrite_2.h>
#include <wincodec.h>
#include <cmath>
#include <cstdint>
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
float g_unit;    // 1 / design units per em
float g_faceH;   // emoji height (ascent + descent) per em
bool g_ready, g_tried, g_com;
int g_refs;      // font sources using this loader; everything is released with the last one

const ImWchar kFirstEmoji = 0x2000;   // below this: letters, digits, '#', '*'... they stay Segoe UI
const int kMaxBitmap = 1024;          // refuse absurd sizes (huge font size x density)

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

void Release() { Rel(g_face); Rel(g_wic); Rel(g_d2d); Rel(g_dw); g_ready = false; }

void UninitCom() {
    if (g_com) { CoUninitialize(); g_com = false; }
}

void Shutdown() {
    Release();
    g_tried = false;   // a later atlas may try again
    UninitCom();
}

bool Fail(const char* why) {
    if (why) Log(why);
    Release();
    UninitCom();
    return false;
}

bool Init() {
    if (g_tried) return g_ready;
    g_tried = true;
    g_com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));   // (already initialised in another mode is fine: not ours to undo)
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), (IUnknown**)&g_dw))) return Fail("emoji: DirectWrite 2 not available - emoji in one colour");
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return Fail("emoji: Direct2D not available - emoji in one colour");
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) return Fail("emoji: WIC not available - emoji in one colour");
    IDWriteFontCollection* sys = nullptr;
    if (FAILED(g_dw->GetSystemFontCollection(&sys))) return Fail("emoji: can't read the system fonts - emoji in one colour");
    UINT32 idx = 0; BOOL exists = FALSE;
    if (SUCCEEDED(sys->FindFamilyName(L"Segoe UI Emoji", &idx, &exists)) && exists) {
        IDWriteFontFamily* fam = nullptr; IDWriteFont* font = nullptr;
        if (SUCCEEDED(sys->GetFontFamily(idx, &fam)) &&
            SUCCEEDED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)))
            font->CreateFontFace(&g_face);
        Rel(font);
        Rel(fam);
    }
    Rel(sys);
    if (!g_face) return Fail("emoji: Segoe UI Emoji not installed - emoji in one colour");
    g_face->GetMetrics(&g_fm);
    if (g_fm.designUnitsPerEm == 0 || g_fm.ascent + g_fm.descent == 0) return Fail("emoji: Segoe UI Emoji has unusable metrics - emoji in one colour");
    g_unit = 1.0f / g_fm.designUnitsPerEm;
    g_faceH = (float)(g_fm.ascent + g_fm.descent) * g_unit;
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
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteColorGlyphRunEnumerator* layers = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) {
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);   // 1 unit = 1 pixel, whatever the screen's DPI
        if (SUCCEEDED(g_d2d->CreateWicBitmapRenderTarget(bmp, rp, &rt)) &&
            SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush))) {
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            DWRITE_GLYPH_RUN run{};
            run.fontFace = g_face; run.fontEmSize = em; run.glyphCount = 1; run.glyphIndices = &gi;
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
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
            } else {
                rt->DrawGlyphRun(D2D1::Point2F(baseX, baseY), &run, brush);   // no colour layers: white, tinted like text
            }
            ok = SUCCEEDED(rt->EndDraw());
            if (ok) {
                bgra.resize((size_t)w * h * 4);
                ok = SUCCEEDED(bmp->CopyPixels(nullptr, w * 4, (UINT)bgra.size(), bgra.data()));
            }
        }
    }
    Rel(layers); Rel(brush); Rel(rt); Rel(bmp);
    return ok;
}

// premultiplied BGRA -> straight RGBA (what ImGui's font texture holds), in place
void ToStraightRGBA(std::vector<uint8_t>& px) {
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        uint8_t b = px[i], g = px[i + 1], r = px[i + 2], a = px[i + 3];
        if (a && a < 255) {
            r = (uint8_t)std::min(255, (r * 255 + a / 2) / a);
            g = (uint8_t)std::min(255, (g * 255 + a / 2) / a);
            b = (uint8_t)std::min(255, (b * 255 + a / 2) / a);
        }
        px[i] = r; px[i + 1] = g; px[i + 2] = b;
    }
}

bool SrcInit(ImFontAtlas*, ImFontConfig*) {
    if (!Init()) return false;
    g_refs++;
    return true;
}
void SrcDestroy(ImFontAtlas*, ImFontConfig*) {
    if (g_refs > 0 && --g_refs == 0) Shutdown();
}
bool Contains(ImFontAtlas*, ImFontConfig*, ImWchar cp) {
    if (cp < kFirstEmoji) return false;
    return g_ready && GlyphOf(cp) != 0;
}
bool BakedInit(ImFontAtlas*, ImFontConfig*, ImFontBaked*, void*) { return true; }

bool LoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, ImWchar cp, ImFontGlyph* out, float* outAdvance) {
    if (!g_ready || cp < kFirstEmoji) return false;
    UINT16 gi = GlyphOf(cp);
    if (!gi) return false;
    // the line the emoji must fit in (set up by the text font this one is merged into)
    float lineH = baked->Ascent - baked->Descent;
    if (!std::isfinite(lineH) || lineH <= 0) lineH = baked->Size;
    if (!std::isfinite(lineH) || lineH <= 0) return false;
    float em = lineH / g_faceH * 0.98f;
    if (!std::isfinite(em) || em <= 0) return false;
    DWRITE_GLYPH_METRICS gm{};
    if (FAILED(g_face->GetDesignGlyphMetrics(&gi, 1, &gm, FALSE))) return false;
    float advance = (float)gm.advanceWidth * g_unit * em;
    if (!std::isfinite(advance) || advance < 0) return false;
    if (outAdvance) { *outAdvance = advance; return true; }

    out->Codepoint = cp;
    out->AdvanceX = advance;
    if (gm.advanceWidth == 0) { out->Visible = false; return true; }   // joiners, variation selectors: nothing to draw

    const float density = src->RasterizerDensity * baked->RasterizerDensity;
    if (!std::isfinite(density) || density <= 0) return false;
    double widthPx = ceil((double)advance * density);
    double heightPx = ceil((double)lineH * density);
    if (!std::isfinite(widthPx) || !std::isfinite(heightPx) ||
        widthPx > kMaxBitmap || heightPx > kMaxBitmap) return false;
    int w = std::max(1, (int)widthPx), h = std::max(1, (int)heightPx);
    float emPx = em * density;
    float ascPx = (float)g_fm.ascent * g_unit * emPx;
    float baseY = (h - g_faceH * emPx) * 0.5f + ascPx;   // centred in the line
    std::vector<uint8_t> px;
    bool colored = false;
    if (!Render(gi, emPx, 0.0f, baseY, w, h, px, colored)) return false;
    ToStraightRGBA(px);

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