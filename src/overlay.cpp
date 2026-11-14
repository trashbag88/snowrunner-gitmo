// Gitmo - drawing the phone on top of the game (DirectX 11 + Dear ImGui).
//
// How it attaches without fighting other mods:
//  - It patches one entry in the DXGI factory's function table (CreateSwapChain)
//    to learn which swap chain the game creates, then patches Present /
//    Present1 / ResizeBuffers in that swap chain's table.
//  - Each patch remembers whatever was in the slot before (the system function,
//    or another mod's hook such as the VR mod's) and always calls it, so every
//    mod in the chain keeps running. Nothing is ever restored or overwritten later.
//  - All drawing is wrapped in an exception guard: if anything goes wrong the
//    overlay switches itself off and the game carries on.

#include "gitmo.h"
#include <cmath>
#include <algorithm>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

std::atomic<bool> g_menuOpen{false};
std::atomic<bool> g_captureKey{false};
std::atomic<bool> g_uiTyping{false};
bool g_uiVrPanel;
bool g_vrDragging;

using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwnd_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static CreateSwapChain_t o_CreateSwapChain;
static CreateSwapChainForHwnd_t o_CreateSwapChainForHwnd;
static Present_t o_Present;
static Present1_t o_Present1;
static ResizeBuffers_t o_ResizeBuffers;

static std::atomic<IDXGISwapChain*> g_sc{nullptr};
static HWND g_hwnd;
static WNDPROC g_origWndProc;
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static ID3D11RenderTargetView* g_rtv;
static bool g_imguiReady, g_broken, g_keyWasDown;
static UINT g_bbW, g_bbH;   // size of the frame the game actually draws
static std::mutex g_uiLock;

// Replace one function-table entry. What was there before is stored in *orig first,
// and only then is the entry switched (in one step), so a call racing the patch
// always finds a valid original.
template <class F>
static bool PatchSlot(void** vt, int idx, void* fn, F* orig) {
    void* prev = vt[idx];
    if (prev == fn) return false;
    *(void* volatile*)orig = prev;
    MemoryBarrier();
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(&vt[idx], fn);
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return true;
}

// ---- textures for the phone (Steam avatars) ------------------------------------
// Called from the phone's drawing (render thread, device known). The device is free-threaded
// for creation; the view is used as an ImGui texture id.
void* Overlay_MakeTexture(const uint8_t* rgba, int w, int h) {
    if (!g_dev || !rgba || w <= 0 || h <= 0) return nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ rgba, (UINT)w * 4, 0 };
    ID3D11Texture2D* t = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, &sd, &t))) return nullptr;
    ID3D11ShaderResourceView* v = nullptr;
    g_dev->CreateShaderResourceView(t, nullptr, &v);
    t->Release();
    return v;
}

void Overlay_FreeTexture(void* srv) { if (srv) ((ID3D11ShaderResourceView*)srv)->Release(); }

// ---- photo capture: the game's frame, before the phone is drawn onto it ------------------
static std::atomic<bool> g_captureReq{false};
void Overlay_RequestCapture() { g_captureReq = true; }

static void CaptureFrame(IDXGISwapChain* sc) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) { Log("photo: couldn't read the game's frame"); Photo_Failed(); return; }
    D3D11_TEXTURE2D_DESC d{}; bb->GetDesc(&d);
    DXGI_FORMAT f = d.Format;
    bool bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM;
    bool rgba = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    bool ten = f == DXGI_FORMAT_R10G10B10A2_UNORM;
    if (!bgra && !rgba && !ten) { Log("photo: the game's frame format %d isn't supported", (int)f); bb->Release(); Photo_Failed(); return; }
    ID3D11Texture2D* src = bb; ID3D11Texture2D* resolved = nullptr;
    if (d.SampleDesc.Count > 1) {   // multisampled: resolve to one sample first
        D3D11_TEXTURE2D_DESC r = d; r.SampleDesc.Count = 1; r.SampleDesc.Quality = 0; r.BindFlags = 0; r.MiscFlags = 0; r.Usage = D3D11_USAGE_DEFAULT;
        if (SUCCEEDED(g_dev->CreateTexture2D(&r, nullptr, &resolved))) { g_ctx->ResolveSubresource(resolved, 0, bb, 0, f); src = resolved; }
    }
    D3D11_TEXTURE2D_DESC sd = d; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0; sd.BindFlags = 0; sd.MiscFlags = 0;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MipLevels = 1; sd.ArraySize = 1;
    ID3D11Texture2D* stg = nullptr;
    PhotoImg img;
    if (SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &stg))) {
        g_ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, src, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) {
            img.w = (int)d.Width; img.h = (int)d.Height; img.rgba.resize((size_t)img.w * img.h * 4);
            for (int y = 0; y < img.h; y++) {
                const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                uint8_t* o = img.rgba.data() + (size_t)y * img.w * 4;
                for (int x = 0; x < img.w; x++, o += 4) {
                    const uint8_t* p = row + x * 4;
                    if (rgba) { o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; }
                    else if (bgra) { o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; }
                    else { uint32_t v = *(const uint32_t*)p; o[0] = (uint8_t)((v & 1023) >> 2); o[1] = (uint8_t)(((v >> 10) & 1023) >> 2); o[2] = (uint8_t)(((v >> 20) & 1023) >> 2); }
                    o[3] = 255;
                }
            }
            g_ctx->Unmap(stg, 0);
        }
        stg->Release();
    }
    if (resolved) resolved->Release();
    bb->Release();
    if (img.w <= 0) { Log("photo: couldn't copy the game's frame"); Photo_Failed(); return; }
    Log("photo: took the game's view (%dx%d)", img.w, img.h);
    Photo_FromCapture(std::move(img));   // the worker makes the JPEG; the phone shows it when ready
}

// ---- window input ---------------------------------------------------------------
static bool IsInputMsg(UINT m) {
    return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || (m >= WM_KEYFIRST && m <= WM_KEYLAST) || m == WM_INPUT;
}

// VR: left-button presses and releases seen by the window since the last frame, in order,
// replayed at the phone's pointer (see OverlayFrame). 1 = down, 0 = up.
static int g_vrClicks[16], g_vrClickCount;

HWND Overlay_GameWindow() { return g_hwnd; }

static LRESULT CALLBACK HkWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
#ifdef GITMO_TEST
    if (Input_IsGitmoKey(m, w, l)) Log("test: Gitmo's Esc reached the game window: %s (phone %s)", m == WM_KEYDOWN ? "key down" : m == WM_KEYUP ? "key up" : "char", g_menuOpen ? "open" : "closed");
#endif
    if (Input_IsGitmoKey(m, w, l)) return CallWindowProcW(g_origWndProc, h, m, w, l);   // Gitmo's Esc: straight to the game
    if (g_menuOpen && g_imguiReady && IsInputMsg(m)) {
        {
            std::lock_guard<std::mutex> lk(g_uiLock);
            bool mouseMsg = m >= WM_MOUSEFIRST && m <= WM_MOUSELAST;
            if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) Input_NoteWindowKey();
            if (g_uiVrPanel && mouseMsg) {
                // In the headset these messages carry the position of the parked (hidden)
                // Windows cursor, not of the phone's pointer, so they must not reach the
                // phone: a click would land in the wrong place. Only the button is kept.
                if ((m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK || m == WM_LBUTTONUP) && g_vrClickCount < 16)
                    g_vrClicks[g_vrClickCount++] = m != WM_LBUTTONUP;
                // the wheel carries no position, so it is safe to pass on: it scrolls whatever
                // is under the phone's pointer (Settings, the contact list, a conversation)
                if (m == WM_MOUSEWHEEL || m == WM_MOUSEHWHEEL) ImGui_ImplWin32_WndProcHandler(h, m, w, l);
            } else {
                ImGui_ImplWin32_WndProcHandler(h, m, w, l);
            }
        }
        // While the phone is open, keep mouse and keyboard away from the game.
        // The menu key itself still has to reach nobody: it is read by polling.
        if (m == WM_INPUT) return DefWindowProcW(h, m, w, l);  // raw input must still be cleaned up
        return 0;
    }
    return CallWindowProcW(g_origWndProc, h, m, w, l);
}

// ---- drawing -------------------------------------------------------------------
static bool SetupImGui(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev))) {
        Log("overlay: the game's swap chain is not DirectX 11 -> overlay off");
        return false;
    }
    g_dev->GetImmediateContext(&g_ctx);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // never write imgui.ini (the VR mod uses that name)
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_dev, g_ctx)) {
        Log("overlay: ImGui init failed -> overlay off");
        return false;
    }
    RECT rc; GetClientRect(g_hwnd, &rc);
    UI_Init((float)(rc.bottom - rc.top));
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HkWndProc);
    Log("overlay: ready on window %p (%ldx%ld), press %s in game", g_hwnd, rc.right - rc.left,
        rc.bottom - rc.top, g_cfg.menuKeyName.c_str());
    return true;
}

static bool MakeRtv(IDXGISwapChain* sc) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
    D3D11_TEXTURE2D_DESC td; bb->GetDesc(&td);
    g_bbW = td.Width; g_bbH = td.Height;
    HRESULT hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    if (FAILED(hr)) {   // typeless back buffer: pick a concrete format
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = td.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS ? DXGI_FORMAT_R10G10B10A2_UNORM
                  : td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS    ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                   : DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = g_dev->CreateRenderTargetView(bb, &rd, &g_rtv);
    }
    static bool logged;
    if (!logged) { logged = true; Log("overlay: back buffer %ux%u format %d -> %s", td.Width, td.Height, td.Format, SUCCEEDED(hr) ? "ok" : "FAILED"); }
    bb->Release();
    return SUCCEEDED(hr);
}

#ifdef GITMO_TEST
int g_testFrame = [] { const char* e = getenv("GITMO_TEST_PHASE"); return e ? atoi(e) * 300 : 0; }();   // start at a later test phase
ImVec2 g_testGotIt, g_testBezel, g_testPhonePos;
bool g_testSheet;
ImVec2 g_testEmojiKey, g_testEmojiCell;   // phase 23: the smiley key, and the first emoji in the panel
bool g_testEmojiOpen; char g_testDraftCopy[400];
ImVec2 g_testSettingsMid; float g_testSettingsScroll; int g_testWallpaperNow;   // phase 30: drag-to-scroll
ImVec2 g_testCameraKey, g_testTakePhoto;   // phase 33: tap "Take photo"
ImVec2 g_testSheetBtnA, g_testSheetBtnB; int g_testSheetClicks;   // phases 46, 48, 49: the transfer sheet's main button
float g_testBattery = -1; int g_testBattState;
static void TestCapture(IDXGISwapChain* sc, int n) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &st))) {
        g_ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\shot%d.bmp", ModuleDir().c_str(), n);
            FILE* f = _wfopen(path, L"wb");
            BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
            ih.biSize = sizeof(ih); ih.biWidth = d.Width; ih.biHeight = -(LONG)d.Height; ih.biPlanes = 1; ih.biBitCount = 32;
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
            fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
            std::vector<uint8_t> row(d.Width * 4);
            for (UINT y = 0; y < d.Height; y++) {
                uint8_t* src = (uint8_t*)m.pData + y * m.RowPitch;
                for (UINT x = 0; x < d.Width; x++) { row[x*4] = src[x*4+2]; row[x*4+1] = src[x*4+1]; row[x*4+2] = src[x*4]; row[x*4+3] = 255; }
                fwrite(row.data(), row.size(), 1, f);
            }
            fclose(f);
            g_ctx->Unmap(st, 0);
        }
        st->Release();
    }
    bb->Release();
}
#endif

static void OverlayFrame(IDXGISwapChain* sc) {
#ifdef GITMO_TEST
    g_menuOpen = true;
    ++g_testFrame;
#endif
    // Rebinding ("Change" in the phone's Settings): the next key pressed becomes the menu key.
    static bool capturing, prev[256];
    if (g_captureKey && g_menuOpen) {
        if (!capturing) {   // remember what is already held so only a new press counts
            capturing = true;
            for (int vk = 0; vk < 256; vk++) prev[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        }
        for (int vk = 8; vk < 255; vk++) {
            if (vk >= VK_SHIFT && vk <= VK_MENU) continue;          // modifiers on their own
            if (vk >= VK_LSHIFT && vk <= VK_RMENU) continue;
            if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS) continue;
            bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (d && !prev[vk] && GetForegroundWindow() == g_hwnd) {
                if (vk != VK_ESCAPE) {
                    SaveMenuKey(vk);
                    Log("menu key changed to %s", g_cfg.menuKeyName.c_str());
                }
                g_captureKey = false;
                g_keyWasDown = vk == g_cfg.menuKey;   // don't let this same press close the phone
                break;
            }
            prev[vk] = d;
        }
    } else {
        capturing = false;
        g_captureKey = false;
    }

    // Menu key: polled, so it works however the game reads the keyboard.
    bool down = (GetAsyncKeyState(g_cfg.menuKey) & 0x8000) && GetForegroundWindow() == g_hwnd;
    if (g_captureKey) down = g_keyWasDown;   // while choosing a key, the old key doesn't toggle
    if (g_uiTyping && g_menuOpen) down = g_keyWasDown;   // typing a message: the key is text (e.g. Delete), not "close"
    if (down && !g_keyWasDown) {
        // The phone works in the main menu too; only money transfers need a loaded save
        // (no balance there, so Send and incoming transfers are refused by the transfer code).
        g_menuOpen = !g_menuOpen; Log("menu %s%s", g_menuOpen ? "opened" : "closed", Game_InGame() ? "" : " (main menu)");
    }
    g_keyWasDown = down;
    Input_Frame(g_menuOpen);
    static bool clipped;
    static bool wasOpen;
    if (!g_menuOpen) {
        g_uiTyping = false;
        if (wasOpen) { wasOpen = false; std::lock_guard<std::mutex> lk(g_uiLock); UI_PhoneClosed(); }
        if (clipped) { ClipCursor(nullptr); clipped = false; }
        return;
    }

    wasOpen = true;
    std::lock_guard<std::mutex> lk(g_uiLock);
    if (!g_imguiReady) {
        if (!SetupImGui(sc)) { g_broken = true; return; }
        g_imguiReady = true;
    }
    if (!g_rtv && !MakeRtv(sc)) return;
    // "Take photo": the frame as the game drew it - the phone hasn't been drawn onto it yet
    if (g_captureReq.exchange(false)) CaptureFrame(sc);

    // In VR the phone is drawn into its own texture, which Gitmo hands to the headset as a
    // panel hanging in the room (vr.cpp). On a flat screen it is drawn onto the game's frame.
    int vw = 0, vh = 0;
    ID3D11RenderTargetView* vrRtv = VR_PanelWanted() ? VR_PhoneTarget(g_dev, vw, vh) : nullptr;
    g_uiVrPanel = vrRtv != nullptr;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    // Feed the mouse directly too: the game may read it as raw input only.
    // Lay the phone out in the game's own frame, which can differ from the window's size
    // (fullscreen at another resolution, upscaling...). The mouse is in window pixels, so
    // it is scaled into frame pixels to line up with the buttons.
    RECT rc; GetClientRect(g_hwnd, &rc);
    float cw = (float)std::max<LONG>(rc.right - rc.left, 1), ch = (float)std::max<LONG>(rc.bottom - rc.top, 1);
    if (g_bbW && g_bbH) io.DisplaySize = ImVec2((float)g_bbW, (float)g_bbH);
    float sx = io.DisplaySize.x / cw, sy = io.DisplaySize.y / ch;
    static bool loggedScale;
    if (!loggedScale && (fabsf(sx - 1) > 0.01f || fabsf(sy - 1) > 0.01f)) {
        loggedScale = true;
        Log("overlay: game frame %ux%u, window %.0fx%.0f -> mouse scaled %.3f x %.3f", g_bbW, g_bbH, cw, ch, sx, sy);
    }
    POINT p; GetCursorPos(&p); ScreenToClient(g_hwnd, &p);
    float mx = p.x * sx, my = p.y * sy;
    if (g_uiVrPanel) {
        // In the headset the mouse drives Gitmo's own pointer on the phone, by how far it
        // moves (like a game cursor), at the speed set on the phone. The real cursor is
        // parked in the middle of the game window after each move - and kept inside the
        // window - so it never runs into an edge or clicks another program on the desktop.
        io.DisplaySize = ImVec2((float)vw, (float)vh);
        RECT sr = rc;
        MapWindowPoints(g_hwnd, nullptr, (POINT*)&sr, 2);
        ClipCursor(&sr);
        clipped = true;
        POINT centre{ (sr.left + sr.right) / 2, (sr.top + sr.bottom) / 2 };
        static ImVec2 ptr;
        static ULONGLONG lastVr;
        ULONGLONG nowT = GetTickCount64();
        POINT now; GetCursorPos(&now);
        if (nowT - lastVr > 250) {   // phone just opened in the headset: pointer starts mid-phone
            ptr = ImVec2(vw * 0.5f, vh * 0.55f);
            now = centre;
        }
        lastVr = nowT;
        float dx = (float)(now.x - centre.x), dy = (float)(now.y - centre.y);
        if (now.x != centre.x || now.y != centre.y) SetCursorPos(centre.x, centre.y);

        bool held = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (g_vrDragging && !held) g_vrDragging = false;
        // Both speeds are per mouse movement, not per window pixel, so they feel the same
        // whatever resolution the VR mod renders at. (Tuned on a 2768-pixel-wide window.)
        const float kRef = 2768.0f;
        if (g_vrDragging) {
            // Dragging the border swings the phone around you (2768 pixels of mouse
            // movement turn it about 90 degrees); the pointer stays on the grabbed spot.
            const float kTurn = 1.5708f;
            if (dx || dy) VR_DragBy(-dx / kRef * kTurn, -dy / kRef * kTurn * 0.75f);
        } else {
            // Speed 1.0 = the first version's feel; default 2.0.
            float k = (float)vw / kRef * g_cfg.vrPointerSpeed;
            ptr.x = std::min(std::max(ptr.x + dx * k, 0.0f), (float)vw - 1);
            ptr.y = std::min(std::max(ptr.y + dy * k, 0.0f), (float)vh - 1);
        }
        mx = ptr.x; my = ptr.y;
    } else {
        g_vrDragging = false;
        if (clipped) { ClipCursor(nullptr); clipped = false; }
    }
    bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#ifdef GITMO_TEST
    {   // scripted mouse: click "Got it" in phase 3, drag the border in phase 0
        int ph = g_testFrame / 300, k = g_testFrame % 300;
        lmb = false; mx = my = -100;
        if (ph == 4 && k >= 100 && k < 110) { mx = g_testGotIt.x; my = g_testGotIt.y; lmb = k >= 102 && k <= 104; }
        if (ph == 4 && k == 150) Log("test: sheet after clicking Got it: %s", g_testSheet ? "STILL OPEN" : "closed");
        // phase 23: tap the smiley, then an emoji, then the text field
        if (ph == 23 && k >= 60 && k < 66) { mx = g_testEmojiKey.x; my = g_testEmojiKey.y; lmb = k >= 61 && k <= 62; }
        if (ph == 23 && k == 100) Log("test: emoji panel 38 frames after tapping the smiley: %s", g_testEmojiOpen ? "open" : "CLOSED");
        if (ph == 23 && k >= 120 && k < 126) { mx = g_testEmojiCell.x; my = g_testEmojiCell.y; lmb = k >= 121 && k <= 122; }
        if (ph == 23 && k == 160) Log("test: after tapping an emoji: panel %s, draft \"%s\"", g_testEmojiOpen ? "open" : "CLOSED", g_testDraftCopy);
        if (ph == 23 && k >= 180 && k < 186) { mx = g_testEmojiKey.x + 90; my = g_testEmojiKey.y; lmb = k >= 181 && k <= 182; }
        if (ph == 23 && k == 220) Log("test: after tapping the text field: panel %s", g_testEmojiOpen ? "STILL OPEN" : "closed");
        // phase 30: press on a wallpaper thumbnail in Settings and drag up 320 px: it must scroll, not pick the wallpaper
        if (ph == 30 && k == 40) Log("test: settings scroll before the drag %.0f, wallpaper %d", g_testSettingsScroll, g_testWallpaperNow);
        if (ph == 30 && k >= 60 && k < 150) { mx = g_testSettingsMid.x; my = g_testSettingsMid.y - std::min(k - 61, 80) * 4.0f; lmb = k >= 61 && k <= 145; }
        if (ph == 33 && k >= 60 && k < 66) { mx = g_testTakePhoto.x; my = g_testTakePhoto.y; lmb = k >= 61 && k <= 62; }
        // phases 46/48/49: click the transfer sheet's button at 5 spots across it (left edge to
        // right edge, upper part) - every one must land, even where a list lies underneath
        if ((ph == 46 || ph == 48 || ph == 49) && k == 50) g_testSheetClicks = 0;
        if ((ph == 46 || ph == 48 || ph == 49) && k >= 60 && k < 110) {
            int spot = (k - 60) / 10, t = (k - 60) % 10;
            float fx = 0.08f + spot * 0.21f;
            mx = g_testSheetBtnA.x + (g_testSheetBtnB.x - g_testSheetBtnA.x) * fx;
            my = g_testSheetBtnA.y + (g_testSheetBtnB.y - g_testSheetBtnA.y) * 0.25f;
            lmb = t >= 2 && t <= 4;
        }
        if ((ph == 46 || ph == 48 || ph == 49) && k == 120) Log("test: sheet button clicks landed %d of 5", g_testSheetClicks);
        if (ph == 33 && k == 200) Log("test: 140 frames after tapping Take photo: photo state %d", Photo_State());
        if (ph == 30 && k == 200) Log("test: settings scroll after dragging up %.0f, wallpaper %d (should be unchanged)", g_testSettingsScroll, g_testWallpaperNow);
        if (ph == 0 && k == 199) Log("test: phone at %.0f,%.0f before drag", g_testPhonePos.x, g_testPhonePos.y);
        if (ph == 0 && k >= 200 && k < 232) {
            static ImVec2 start; if (k == 200) start = g_testBezel;
            int step = k < 202 ? 0 : std::min(k - 202, 20);
            mx = start.x - step * 15.0f; my = start.y + step * 5.0f; lmb = k >= 201 && k <= 226;
        }
        if (ph == 0 && k == 240) Log("test: phone at %.0f,%.0f after dragging -300,+100; saved %.4f,%.4f", g_testPhonePos.x, g_testPhonePos.y, g_cfg.phoneX, g_cfg.phoneY);
    }
#endif
    io.AddMousePosEvent(mx, my);
    if (g_uiVrPanel && g_vrClickCount > 0) {
        // Every press and release since the last frame, in order, at the pointer - so even
        // a click quicker than one frame is never lost or put somewhere else.
        for (int i = 0; i < g_vrClickCount; i++) io.AddMouseButtonEvent(0, g_vrClicks[i] != 0);
    } else {
        io.AddMouseButtonEvent(0, lmb);
    }
    g_vrClickCount = 0;
    io.MouseDrawCursor = true;
    ImGui::NewFrame();
    UI_Draw();
    ImGui::Render();

    ID3D11RenderTargetView* oldRtv = nullptr; ID3D11DepthStencilView* oldDsv = nullptr;
    g_ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    if (g_uiVrPanel) {
        const float clear[4] = { 0, 0, 0, 0 };   // transparent around the rounded corners
        g_ctx->ClearRenderTargetView(vrRtv, clear);
        g_ctx->OMSetRenderTargets(1, &vrRtv, nullptr);
    } else {
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    }
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
    if (oldRtv) oldRtv->Release();
    if (oldDsv) oldDsv->Release();
    if (g_uiVrPanel) VR_PhoneDrawn();
#ifdef GITMO_TEST
    if (g_testFrame % 300 == 250) TestCapture(sc, g_testFrame / 300);
    if (getenv("GITMO_TEST_ESC") && g_testFrame % 300 == 100) { Log("test: three money-left events in a row"); Input_PressEscape(); Input_PressEscape(); Input_PressEscape(); }
#endif
}

static void GuardedFrame(IDXGISwapChain* sc) {
    __try { OverlayFrame(sc); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_broken = true;
        g_menuOpen = false;
        Log("overlay: error 0x%08X while drawing -> overlay switched off for this session", GetExceptionCode());
    }
}

// ---- hooks ---------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (sc == g_sc && !g_broken && !(flags & DXGI_PRESENT_TEST)) GuardedFrame(sc);
    return o_Present(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE Hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    if (sc == g_sc && !g_broken && !(flags & DXGI_PRESENT_TEST)) GuardedFrame(sc);
    return o_Present1(sc, sync, flags, pp);
}
static HRESULT STDMETHODCALLTYPE Hk_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (sc == g_sc) {
        std::lock_guard<std::mutex> lk(g_uiLock);
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    }
    return o_ResizeBuffers(sc, n, w, h, f, fl);
}

HWND Overlay_Window() { return g_sc ? g_hwnd : nullptr; }

static void AttachSwapChain(IDXGISwapChain* sc, HWND hwnd, UINT w, UINT h) {
    Log("swap chain created: %p window %p %ux%u%s", sc, hwnd, w, h, g_sc ? " (already attached to another, ignored)" : "");
    if (g_sc) return;
    void** vt = *(void***)sc;
    PatchSlot(vt, 8, (void*)Hk_Present, &o_Present);
    PatchSlot(vt, 13, (void*)Hk_ResizeBuffers, &o_ResizeBuffers);
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1))) {
        PatchSlot(*(void***)sc1, 22, (void*)Hk_Present1, &o_Present1);
        sc1->Release();
    }
    if (!o_Present || !o_ResizeBuffers) { Log("overlay: could not hook Present -> overlay off"); return; }
    g_hwnd = hwnd;
    g_sc = sc;
    Log("overlay: hooked the game's swap chain");
}

static HRESULT STDMETHODCALLTYPE Hk_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    HRESULT hr = o_CreateSwapChain(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out && d) AttachSwapChain(*out, d->OutputWindow, d->BufferDesc.Width, d->BufferDesc.Height);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = o_CreateSwapChainForHwnd(f, dev, hwnd, d, fs, o, out);
    if (SUCCEEDED(hr) && out && *out) AttachSwapChain(*out, hwnd, d ? d->Width : 0, d ? d->Height : 0);
    return hr;
}

// True once the game itself has created a window (its window procedure lives in
// SnowRunner.exe). That only happens after Steam's DRM wrapper has unpacked the
// game's code, so anything that runs after it sees the real code.
static BOOL CALLBACK FindGameWindow(HWND h, LPARAM found) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && (uintptr_t)GetWindowLongPtrW(h, GWLP_HINSTANCE) == g_exeBase) {
        *(bool*)found = true;
        return FALSE;
    }
    return TRUE;
}

static void HookFactory();

// Wait for the game's own window before touching DXGI.
// Creating a DXGI factory any earlier wakes the VR mod (dxgi.dll) too early: on the
// first factory it scans the game's code for the addresses it needs, and before the
// DRM has unpacked that code it finds none (0 of 24) - its camera fixes then stay off,
// which leaves the headset view zoomed in and turning the wrong way.
void Overlay_Start() {
    if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            ULONGLONG start = GetTickCount64();
            bool found = false;
            while (!found && GetTickCount64() - start < 120000) {
                EnumWindows(FindGameWindow, (LPARAM)&found);
                if (!found) Sleep(50);
            }
            Log("overlay: %s after %.1f s", found ? "game window is up" : "no game window yet, going ahead",
                (GetTickCount64() - start) / 1000.0);
            HookFactory();
            return 0;
        }, nullptr, 0, nullptr)) CloseHandle(t);
}

static void HookFactory() {
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? (HRESULT(WINAPI*)(REFIID, void**))GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
    IDXGIFactory1* f = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), (void**)&f))) {
        Log("overlay: no DXGI factory -> overlay off");
        return;
    }
    void** vt = *(void***)f;
    PatchSlot(vt, 10, (void*)Hk_CreateSwapChain, &o_CreateSwapChain);
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(f->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2))) {
        void** vt2 = *(void***)f2;
        if (vt2 != vt) Log("overlay: note: factory has a separate IDXGIFactory2 table");
        PatchSlot(vt2, 15, (void*)Hk_CreateSwapChainForHwnd, &o_CreateSwapChainForHwnd);
        f2->Release();
    }
    f->Release();
    Log("overlay: waiting for the game's swap chain (CreateSwapChain %s, ForHwnd %s)",
        o_CreateSwapChain ? "hooked" : "no", o_CreateSwapChainForHwnd ? "hooked" : "no");
}
