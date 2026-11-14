// Gitmo - the phone as its own panel in the VR headset (OpenXR quad layer).
//
// The VR mod (dxgi.dll) runs the OpenXR session. Gitmo doesn't touch the VR mod at all:
// it sits between the OpenXR loader and the runtime (Virtual Desktop, SteamVR...), the
// same place an OpenXR "API layer" sits, but without installing anything system-wide.
//
//  1. At start-up, before the VR mod starts OpenXR, Gitmo loads the active runtime's DLL
//     and points that DLL's xrNegotiateLoaderRuntimeInterface export at Gitmo.
//  2. When the loader negotiates, Gitmo hands it its own xrGetInstanceProcAddr, which
//     returns the runtime's functions untouched except xrCreateSession, xrDestroySession
//     and xrEndFrame.
//  3. In xrEndFrame Gitmo copies the phone (drawn into its own texture) into its own
//     OpenXR swapchain and adds one more layer on top of the VR mod's layers: a quad
//     pinned in the room where the phone was opened, at the distance set on the phone.
//
// Every failure path just leaves the phone on the flat screen, and the VR mod's frames
// always go to the runtime unchanged.

#include "gitmo.h"
#include "shared.h"
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <vector>
#include <string>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <unknwn.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

// ---- runtime function pointers --------------------------------------------------------
static PFN_xrNegotiateLoaderRuntimeInterface r_Negotiate;
static PFN_xrGetInstanceProcAddr r_GIPA;
static PFN_xrCreateSession r_CreateSession;
static PFN_xrDestroySession r_DestroySession;
static PFN_xrEndFrame r_EndFrame;
static PFN_xrCreateReferenceSpace r_CreateReferenceSpace;
static PFN_xrLocateSpace r_LocateSpace;
static PFN_xrDestroySpace r_DestroySpace;
static PFN_xrEnumerateSwapchainFormats r_EnumFormats;
static PFN_xrCreateSwapchain r_CreateSwapchain;
static PFN_xrDestroySwapchain r_DestroySwapchain;
static PFN_xrEnumerateSwapchainImages r_EnumImages;
static PFN_xrAcquireSwapchainImage r_Acquire;
static PFN_xrWaitSwapchainImage r_Wait;
static PFN_xrReleaseSwapchainImage r_Release;

// ---- session state (all touched on the thread that calls xrEndFrame) --------------------
static XrInstance g_inst = XR_NULL_HANDLE;
static XrSession g_sess = XR_NULL_HANDLE;
static ID3D11Device* g_xrDev;                 // the device the VR mod gave OpenXR
static XrSpace g_local = XR_NULL_HANDLE, g_view = XR_NULL_HANDLE;
static XrSwapchain g_swap = XR_NULL_HANDLE;
static std::vector<ID3D11Texture2D*> g_images;
static uint32_t g_acquiredIndex;
static bool g_imageAcquired, g_imageWaited;
static std::atomic<bool> g_setupFailed{ false };
static std::atomic<int> g_state{0};           // 0 no VR seen, 1 hooked, 2 session running, 3 panel ready
static std::atomic<bool> g_frameSeen;

// The phone texture: drawn by the overlay on the game's thread, copied in xrEndFrame.
static ID3D11Device* g_texDev;
static ID3D11Texture2D* g_tex;
static ID3D11RenderTargetView* g_texRtv;
static std::atomic<bool> g_texFresh;          // drawn this frame -> submit the layer
const int kVrTexW = 588, kVrTexH = 1200;      // 0.49 : 1, same shape as the phone

// Where the phone hangs in the room: a direction (yaw / pitch) from where the head was
// when it was placed, at the set distance. It stays there - closing and reopening the
// phone doesn't move it - until it is dragged by its border or "Bring here" is tapped.
static bool g_pinValid;
static float g_pinYaw, g_pinPitch = -0.21f;   // radians; ~12 degrees below eye level
static XrVector3f g_pinPos{0, 0, 0};
static std::mutex g_pinMx;
static std::atomic<bool> g_wantPin{true};

// ---- small maths ----------------------------------------------------------------------
static XrQuaternionf QMul(XrQuaternionf a, XrQuaternionf b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static XrVector3f QRot(XrQuaternionf q, XrVector3f v) {
    XrQuaternionf p{v.x, v.y, v.z, 0}, c{-q.x, -q.y, -q.z, q.w};
    XrQuaternionf r = QMul(QMul(q, p), c);
    return { r.x, r.y, r.z };
}
static XrQuaternionf AxisAngle(float x, float y, float z, float a) {
    float s = sinf(a * 0.5f);
    return { x * s, y * s, z * s, cosf(a * 0.5f) };
}

// ---- the layer ------------------------------------------------------------------------
static void ReleaseSwapchain() {
    if (g_swap && g_imageAcquired) {
        if (!g_imageWaited) {
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            XrResult waited = r_Wait(g_swap, &wi);
            g_imageWaited = waited != XR_TIMEOUT_EXPIRED && XR_SUCCEEDED(waited);
        }
        if (g_imageWaited) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if (XR_FAILED(r_Release(g_swap, &ri))) Log("vr: could not release a panel image during cleanup");
        } else {
            Log("vr: could not wait for a panel image during cleanup");
        }
    }
    g_imageAcquired = g_imageWaited = false;
    g_acquiredIndex = 0;
    if (g_swap) {
        XrResult r = r_DestroySwapchain(g_swap);
        if (XR_FAILED(r)) Log("vr: could not destroy the phone panel swapchain (%d)", (int)r);
        g_swap = XR_NULL_HANDLE;
    }
    g_images.clear();
}

static bool SetupPanel() {
    if (g_setupFailed) return false;
    if (g_swap) return true;
    auto fail = [](const char* why, XrResult r) { Log("vr: %s (%d) -> phone stays on the flat screen", why, (int)r); g_setupFailed = true; return false; };
    XrResult r;
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!g_local && XR_FAILED(r = r_CreateReferenceSpace(g_sess, &rs, &g_local))) return fail("no room space", r);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!g_view && XR_FAILED(r = r_CreateReferenceSpace(g_sess, &rs, &g_view))) return fail("no head space", r);

    // A format the phone texture can be copied into directly: the R8G8B8A8 family.
    uint32_t n = 0;
    if (XR_FAILED(r = r_EnumFormats(g_sess, 0, &n, nullptr))) return fail("could not enumerate panel formats", r);
    std::vector<int64_t> fmts(n);
    if (n && XR_FAILED(r = r_EnumFormats(g_sess, n, &n, fmts.data()))) return fail("could not read panel formats", r);
    int64_t fmt = 0;
    for (int64_t want : { (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM })
        for (int64_t f : fmts) if (f == want && !fmt) fmt = f;
    if (!fmt) {
        Log("vr: the headset offers no RGBA8 panel format -> phone stays on the flat screen");
        g_setupFailed = true;
        return false;
    }

    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = fmt; sc.sampleCount = 1; sc.width = kVrTexW; sc.height = kVrTexH;
    sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
    if (XR_FAILED(r = r_CreateSwapchain(g_sess, &sc, &g_swap))) return fail("could not create the phone panel", r);
    uint32_t ni = 0;
    if (XR_FAILED(r = r_EnumImages(g_swap, 0, &ni, nullptr))) {
        ReleaseSwapchain();
        return fail("could not enumerate panel images", r);
    }
    if (!ni) {
        ReleaseSwapchain();
        Log("vr: the panel swapchain has no images -> phone stays on the flat screen");
        g_setupFailed = true;
        return false;
    }
    std::vector<XrSwapchainImageD3D11KHR> imgs(ni, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    if (XR_FAILED(r = r_EnumImages(g_swap, ni, &ni, (XrSwapchainImageBaseHeader*)imgs.data()))) {
        ReleaseSwapchain();
        return fail("no panel images", r);
    }
    for (auto& i : imgs) g_images.push_back(i.texture);
    g_state = 3;
    Log("vr: phone panel ready (%dx%d, format %lld, %u images)", kVrTexW, kVrTexH, (long long)fmt, ni);
    return true;
}

// Pin the phone in the room: straight ahead of where the head looks (level, so it
// doesn't tilt with a nod), a little below eye height, facing the player.
static void Pin(XrTime t) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(r_LocateSpace(g_view, g_local, t, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return;
    XrVector3f fwd = QRot(loc.pose.orientation, { 0, 0, -1 });
    {
        std::lock_guard<std::mutex> lock(g_pinMx);
        g_pinYaw = atan2f(-fwd.x, -fwd.z);
        g_pinPitch = -0.21f;   // ~12 degrees down
        g_pinPos = loc.pose.position;
        g_pinValid = true;
    }
    Log("vr: phone placed %.2f m in front of you", g_cfg.vrDistance);
}

// Dragging by the border: swing the phone around the head, keeping its distance.
void VR_DragBy(float dYaw, float dPitch) {
    std::lock_guard<std::mutex> lock(g_pinMx);
    g_pinYaw += dYaw;
    g_pinPitch = std::min(std::max(g_pinPitch + dPitch, -1.3f), 1.3f);
}

static bool BuildQuad(XrCompositionLayerQuad& q, XrTime t) {
    bool pin = g_wantPin.exchange(false);
    {
        std::lock_guard<std::mutex> lock(g_pinMx);
        pin = pin || !g_pinValid;
    }
    if (pin) Pin(t);
    float yaw, pitch;
    XrVector3f pinPos;
    {
        std::lock_guard<std::mutex> lock(g_pinMx);
        if (!g_pinValid) return false;
        yaw = g_pinYaw; pitch = g_pinPitch; pinPos = g_pinPos;
    }
    // A real-sized object: further away looks smaller. S / M / L are its height in metres
    // (larger than a real phone, so it stays readable in the headset).
    static const float kHeight[3] = { 0.30f, 0.40f, 0.52f };
    float d = std::min(std::max(g_cfg.vrDistance, 0.25f), 3.0f);
    float h = kHeight[std::min(std::max(g_cfg.phoneSize, 0), 2)];
    XrQuaternionf pinRot = QMul(AxisAngle(0, 1, 0, yaw), AxisAngle(1, 0, 0, pitch));
    XrVector3f off = QRot(pinRot, { 0, 0, -d });
    q = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // ImGui output is premultiplied
    q.space = g_local;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g_swap;
    q.subImage.imageRect = { { 0, 0 }, { kVrTexW, kVrTexH } };
    q.pose.orientation = pinRot;
    q.pose.position = { pinPos.x + off.x, pinPos.y + off.y, pinPos.z + off.z };
    q.size = { h * kVrTexW / kVrTexH, h };
    return true;
}

static bool CopyIntoPanel() {
    if (!g_xrDev || !g_tex) return false;
    ID3D11DeviceContext* ctx = nullptr;
    g_xrDev->GetImmediateContext(&ctx);
    if (!ctx) {
        g_setupFailed = true;
        Log("vr: could not get the headset graphics context -> phone stays on the flat screen");
        return false;
    }
    if (!g_imageAcquired) {
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult acquired = r_Acquire(g_swap, &ai, &g_acquiredIndex);
        if (XR_FAILED(acquired)) {
            g_setupFailed = true;
            Log("vr: could not acquire a panel image (%d) -> phone stays on the flat screen", (int)acquired);
            ctx->Release();
            return false;
        }
        g_imageAcquired = true;
        g_imageWaited = false;
    }

    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 5000000;   // 5 ms: skip the panel rather than stall a headset frame
    XrResult waited = r_Wait(g_swap, &wi);
    if (waited == XR_TIMEOUT_EXPIRED) {
        ctx->Release();
        return false;
    }
    if (XR_FAILED(waited)) {
        g_setupFailed = true;
        Log("vr: could not wait for a panel image (%d) -> phone stays on the flat screen", (int)waited);
        ctx->Release();
        return false;
    }
    g_imageWaited = true;

    bool ok = false;
    if (g_acquiredIndex < g_images.size()) {
        ctx->CopyResource(g_images[g_acquiredIndex], g_tex);
        ok = true;
    } else {
        g_setupFailed = true;
        Log("vr: the runtime returned an invalid panel image index -> phone stays on the flat screen");
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult released = r_Release(g_swap, &ri);
    if (XR_SUCCEEDED(released)) g_imageAcquired = g_imageWaited = false;
    else {
        g_setupFailed = true;
        Log("vr: could not release a panel image (%d) -> phone stays on the flat screen", (int)released);
        ok = false;
    }
    ctx->Release();
    return ok;
}

// ---- the three wrapped runtime functions -----------------------------------------------
static XrResult XRAPI_CALL W_EndFrame(XrSession s, const XrFrameEndInfo* info) {
    g_frameSeen = true;
    bool show = g_texFresh.exchange(false);
    if (!show || s != g_sess || !info || info->layerCount >= 15 || !g_tex || g_texDev != g_xrDev) return r_EndFrame(s, info);
    XrCompositionLayerQuad quad;
    const XrCompositionLayerBaseHeader* layers[16];
    __try {
        if (!SetupPanel() || !BuildQuad(quad, info->displayTime) || !CopyIntoPanel()) return r_EndFrame(s, info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_setupFailed = true;
        Log("vr: error 0x%08X in the phone panel -> phone stays on the flat screen", GetExceptionCode());
        return r_EndFrame(s, info);
    }
    for (uint32_t i = 0; i < info->layerCount; i++) layers[i] = info->layers[i];
    layers[info->layerCount] = (const XrCompositionLayerBaseHeader*)&quad;   // on top of everything
    XrFrameEndInfo fi = *info;
    fi.layers = layers;
    fi.layerCount = info->layerCount + 1;
    XrResult r = r_EndFrame(s, &fi);
    if (XR_FAILED(r)) {   // the runtime refused our layer: never cost the VR mod a frame twice
        Log("vr: the headset refused the phone panel (%d) -> phone stays on the flat screen", (int)r);
        g_setupFailed = true;
        g_state = 2;
    }
    return r;
}

static XrResult XRAPI_CALL W_CreateSession(XrInstance inst, const XrSessionCreateInfo* ci, XrSession* out) {
    XrResult r = r_CreateSession(inst, ci, out);
    if (XR_FAILED(r) || !out) return r;
    ID3D11Device* dev = nullptr;
    for (auto* h = (const XrBaseInStructure*)ci->next; h; h = h->next)
        if (h->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) dev = ((const XrGraphicsBindingD3D11KHR*)h)->device;
    if (g_sess) return r;   // a second session: leave it alone
    g_inst = inst; g_sess = *out; g_xrDev = dev; g_setupFailed = !dev;
    auto get = [&](const char* n, auto& fn) {
        fn = nullptr;
        return XR_SUCCEEDED(r_GIPA(inst, n, (PFN_xrVoidFunction*)&fn)) && fn != nullptr;
    };
    bool ok = get("xrCreateReferenceSpace", r_CreateReferenceSpace) & get("xrLocateSpace", r_LocateSpace) &
              get("xrDestroySpace", r_DestroySpace) & get("xrEnumerateSwapchainFormats", r_EnumFormats) &
              get("xrCreateSwapchain", r_CreateSwapchain) & get("xrDestroySwapchain", r_DestroySwapchain) &
              get("xrEnumerateSwapchainImages", r_EnumImages) & get("xrAcquireSwapchainImage", r_Acquire) &
              get("xrWaitSwapchainImage", r_Wait) & get("xrReleaseSwapchainImage", r_Release);
    if (!ok) g_setupFailed = true;
    g_state = 2;
    Log("vr: the VR mod started a headset session%s", g_setupFailed ? " (panel not possible)" : " - the phone can show in the headset");
    return r;
}

static XrResult XRAPI_CALL W_DestroySession(XrSession s) {
    if (s == g_sess) {
        ReleaseSwapchain();
        if (g_local) { r_DestroySpace(g_local); g_local = XR_NULL_HANDLE; }
        if (g_view) { r_DestroySpace(g_view); g_view = XR_NULL_HANDLE; }
        g_sess = XR_NULL_HANDLE; g_xrDev = nullptr;
        { std::lock_guard<std::mutex> lock(g_pinMx); g_pinValid = false; }
        g_state = 1;
        Log("vr: headset session ended");
    }
    return r_DestroySession(s);
}

static XrResult XRAPI_CALL W_GIPA(XrInstance inst, const char* name, PFN_xrVoidFunction* fn) {
    XrResult r = r_GIPA(inst, name, fn);
    if (XR_FAILED(r) || !fn || !*fn || !name) return r;
    if (!strcmp(name, "xrCreateSession"))  { r_CreateSession = (PFN_xrCreateSession)*fn;   *fn = (PFN_xrVoidFunction)W_CreateSession; }
    if (!strcmp(name, "xrDestroySession")) { r_DestroySession = (PFN_xrDestroySession)*fn; *fn = (PFN_xrVoidFunction)W_DestroySession; }
    if (!strcmp(name, "xrEndFrame"))       { r_EndFrame = (PFN_xrEndFrame)*fn;             *fn = (PFN_xrVoidFunction)W_EndFrame; }
    if (!strcmp(name, "xrGetInstanceProcAddr")) *fn = (PFN_xrVoidFunction)W_GIPA;
    return r;
}

static XrResult XRAPI_CALL W_Negotiate(const XrNegotiateLoaderInfo* li, XrNegotiateRuntimeRequest* rr) {
    XrResult r = r_Negotiate(li, rr);
    if (XR_SUCCEEDED(r) && rr && rr->getInstanceProcAddr && rr->getInstanceProcAddr != W_GIPA) {
        r_GIPA = rr->getInstanceProcAddr;
        rr->getInstanceProcAddr = W_GIPA;
        g_state = 1;
        Log("vr: OpenXR is starting - Gitmo is ready to add the phone panel");
    }
    return r;
}

// ---- start-up: find the runtime and take its negotiate export --------------------------
static std::wstring ActiveRuntimeJson() {
    DWORD envSize = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", nullptr, 0);
    if (envSize) {
        std::vector<wchar_t> env(envSize);
        DWORD written = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", env.data(), envSize);
        if (written && written < envSize) return std::wstring(env.data(), written);
    }

    DWORD bytes = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
                     RRF_RT_REG_SZ, nullptr, nullptr, &bytes) == ERROR_SUCCESS &&
        bytes >= sizeof(wchar_t)) {
        std::vector<wchar_t> value((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t));
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
                         RRF_RT_REG_SZ, nullptr, value.data(), &bytes) == ERROR_SUCCESS) {
            auto end = std::find(value.begin(), value.end(), L'\0');
            if (end != value.end()) return std::wstring(value.begin(), end);
        }
    }
    return L"";
}

static void SkipJsonSpace(const std::string& json, size_t& pos) {
    while (pos < json.size() && isspace((unsigned char)json[pos])) pos++;
}

static bool ReadJsonString(const std::string& json, size_t& pos, std::string& out) {
    return gitmo_shared::ReadJsonString(json, pos, out);
}

static bool JsonStringMember(const std::string& json, const char* key, std::string& value) {
    for (size_t pos = 0; pos < json.size();) {
        if (json[pos] != '"') { pos++; continue; }
        std::string token;
        if (!ReadJsonString(json, pos, token)) return false;
        size_t next = pos;
        SkipJsonSpace(json, next);
        if (token == key && next < json.size() && json[next] == ':') {
            next++;
            SkipJsonSpace(json, next);
            return ReadJsonString(json, next, value);
        }
    }
    return false;
}

static std::wstring RuntimeLibrary(const std::wstring& json) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, json.c_str(), L"rb") != 0 || !f) return L"";
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return L""; }
    __int64 size = _ftelli64(f);
    if (size <= 0 || size > 1024 * 1024 || _fseeki64(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return L"";
    }
    std::string s((size_t)size, '\0');
    bool read = fread(&s[0], 1, s.size(), f) == s.size();
    fclose(f);
    std::string lib;
    if (!read || !JsonStringMember(s, "library_path", lib) || lib.empty() || lib.find('\0') != std::string::npos) return L"";
    int wideSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, lib.data(), (int)lib.size(), nullptr, 0);
    if (wideSize <= 0) return L"";
    std::wstring w(wideSize, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, lib.data(), (int)lib.size(), &w[0], wideSize) != wideSize) return L"";
    bool absolute = w.size() > 2 && (w[1] == L':' || (w[0] == L'\\' && w[1] == L'\\'));
    if (!absolute) {
        size_t slash = json.find_last_of(L"\\/");
        if (slash != std::wstring::npos) w = json.substr(0, slash + 1) + w;
    }
    DWORD capacity = MAX_PATH * 2;
    std::vector<wchar_t> full(capacity);
    for (;;) {
        DWORD written = GetFullPathNameW(w.c_str(), capacity, full.data(), nullptr);
        if (!written) return w;
        if (written < capacity) return std::wstring(full.data(), written);
        if (written >= MAXDWORD) return w;
        capacity = written + 1;
        full.resize(capacity);
    }
}

// Point one export of a loaded DLL at our function. Export entries are 32-bit offsets
// from the DLL's base, so the jump goes through a small stub placed just above the DLL.
static bool PatchExport(HMODULE mod, const char* name, void* target, void** orig) {
    auto base = (uint8_t*)mod;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return false;
    auto ex = (IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
    auto names = (DWORD*)(base + ex->AddressOfNames);
    auto ords = (WORD*)(base + ex->AddressOfNameOrdinals);
    auto funcs = (DWORD*)(base + ex->AddressOfFunctions);
    for (DWORD i = 0; i < ex->NumberOfNames; i++) {
        if (strcmp((char*)(base + names[i]), name) != 0) continue;
        DWORD* slot = &funcs[ords[i]];
        uint8_t* stub = nullptr;
        for (uintptr_t a = ((uintptr_t)base + nt->OptionalHeader.SizeOfImage + 0xFFFF) & ~(uintptr_t)0xFFFF;
             a < (uintptr_t)base + 0x7FFF0000 && !stub; a += 0x10000)
            stub = (uint8_t*)VirtualAlloc((void*)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!stub) return false;
        stub[0] = 0xFF; stub[1] = 0x25; *(DWORD*)(stub + 2) = 0;   // jmp [rip+0]
        *(void**)(stub + 6) = target;
        DWORD old;
        if (!VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old) ||
            !FlushInstructionCache(GetCurrentProcess(), stub, 16)) {
            VirtualFree(stub, 0, MEM_RELEASE);
            return false;
        }
        DWORD originalRva = *slot;
        if (!VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &old)) {
            VirtualFree(stub, 0, MEM_RELEASE);
            return false;
        }
        DWORD originalProtect = old;
        InterlockedExchange((LONG*)slot, (LONG)(stub - base));
        DWORD ignored;
        if (!VirtualProtect(slot, sizeof(DWORD), originalProtect, &ignored)) {
            InterlockedExchange((LONG*)slot, (LONG)originalRva);
            VirtualProtect(slot, sizeof(DWORD), originalProtect, &ignored);
            VirtualFree(stub, 0, MEM_RELEASE);
            return false;
        }
        *orig = base + originalRva;
        return true;
    }
    return false;
}

// Gitmo never loads the runtime itself (loading it at game start-up, before the game
// has set up its controllers, coincided with the game resetting the wheel controls).
// Instead Windows tells Gitmo the moment the VR mod loads it - before the OpenXR loader
// looks up the negotiate function - and the export is redirected right then.
static wchar_t g_rtFile[MAX_PATH];
static bool g_rtPatched;

struct LdrStr { USHORT Length, MaximumLength; PWSTR Buffer; };
struct LdrLoaded { ULONG Flags; const LdrStr* FullDllName; const LdrStr* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
using LdrCallback = VOID(CALLBACK*)(ULONG reason, const LdrLoaded* data, PVOID ctx);
using LdrRegister_t = LONG(NTAPI*)(ULONG, LdrCallback, PVOID, PVOID*);

static void PatchRuntime(HMODULE m) {
    if (g_rtPatched) return;
    g_rtPatched = true;
    if (PatchExport(m, "xrNegotiateLoaderRuntimeInterface", (void*)W_Negotiate, (void**)&r_Negotiate))
        Log("vr: the VR mod loaded the OpenXR runtime - Gitmo is in place for the phone panel");
    else
        Log("vr: the OpenXR runtime has an unexpected layout -> flat screen only");
}

static VOID CALLBACK OnDllLoaded(ULONG reason, const LdrLoaded* d, PVOID) {
    if (reason != 1 || !d || !d->BaseDllName || !d->BaseDllName->Buffer) return;   // 1 = loaded
    size_t n = d->BaseDllName->Length / sizeof(wchar_t);
    if (n == wcslen(g_rtFile) && _wcsnicmp(d->BaseDllName->Buffer, g_rtFile, n) == 0)
        __try { PatchRuntime((HMODULE)d->DllBase); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void VR_Start() {
    std::wstring json = ActiveRuntimeJson();
    std::wstring lib = json.empty() ? L"" : RuntimeLibrary(json);
    if (lib.empty()) { Log("vr: no OpenXR runtime installed -> phone on the flat screen only"); return; }
    const wchar_t* file = wcsrchr(lib.c_str(), L'\\') ? wcsrchr(lib.c_str(), L'\\') + 1 : lib.c_str();
    wcsncpy_s(g_rtFile, file, _TRUNCATE);
    char n[MAX_PATH]; WideCharToMultiByte(CP_UTF8, 0, file, -1, n, MAX_PATH, nullptr, nullptr);
    if (GetModuleHandleW(file)) { Log("vr: OpenXR (%s) was already running before Gitmo started -> no headset panel this session", n); return; }
    auto reg = (LdrRegister_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    PVOID cookie = nullptr;
    if (!reg || reg(0, OnDllLoaded, nullptr, &cookie) != 0) { Log("vr: cannot watch for the OpenXR runtime -> flat screen only"); return; }
    Log("vr: waiting for the VR mod to load the OpenXR runtime (%s)", n);
}

// ---- for the overlay -------------------------------------------------------------------
// True when the phone should be drawn into the headset panel this frame.
bool VR_PanelWanted() {
    if (g_cfg.display == 1) return false;                                  // Screen
    return g_state >= 2 && !g_setupFailed && g_frameSeen;                  // Auto / Headset
}

const char* VR_Status() {
    if (g_setupFailed && g_state >= 2) return "Headset panel not available";
    switch (g_state.load()) {
    case 3: return "Showing in your headset";
    case 2: return "Headset connected";
    case 1: return "Waiting for the headset";
    default: return "No headset";
    }
}

ID3D11RenderTargetView* VR_PhoneTarget(ID3D11Device* dev, int& w, int& h) {
    w = kVrTexW; h = kVrTexH;
    if (!dev) {
        g_setupFailed = true;
        Log("vr: no game graphics device for the phone texture -> flat screen only");
        return nullptr;
    }
    if (g_tex && g_texDev != dev) {
        g_setupFailed = true;
        Log("vr: the game graphics device changed -> phone stays on the flat screen");
        return nullptr;
    }
    if (!g_tex) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kVrTexW; d.Height = kVrTexH; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_tex)) || FAILED(dev->CreateRenderTargetView(g_tex, nullptr, &g_texRtv))) {
            if (g_tex) { g_tex->Release(); g_tex = nullptr; }
            g_setupFailed = true;
            Log("vr: could not create the phone texture -> flat screen only");
            return nullptr;
        }
        g_texDev = dev;
        if (g_xrDev && g_xrDev != dev) Log("vr: note: the headset uses a different graphics device than the game -> flat screen only");
    }
    return g_texRtv;
}

void VR_PhoneDrawn() { g_texFresh = true; }
void VR_Replace() { g_wantPin = true; }
