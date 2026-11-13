// Gitmo - keeping clicks and keys away from the game while the phone is open.
//
// SnowRunner reads the mouse and keyboard through DirectInput (and checks some
// keys with GetAsyncKeyState / GetKeyState), not only through window messages.
// While the phone is open:
//  - DirectInput mouse and keyboard devices report "nothing pressed, no movement"
//    (their real data is still read and thrown away, so nothing piles up).
//    Wheels, pedals, shifters and gamepads are never touched.
//  - The game's own GetAsyncKeyState / GetKeyState calls see every key as up.
//    Gitmo's own key and mouse reads go through its own imports, so the phone
//    still works.
//  - The game's own key-down table (filled by its window handler, read by its hotkeys)
//    is cleared every frame, and whenever the game checks a key, so a key press that
//    reaches the game some other way still can't trigger a hotkey.
// Hooks chain to whatever was there before (the system, or NoAutoReverse's
// force-feedback recorder), and do nothing at all while the phone is closed.
// Each time the phone is put away, the log says which of these saw keys while it was open.

#include "gitmo.h"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <string>
#include <thread>
#include <algorithm>

// ---- what reached the game while the phone was open (for the log) --------------------
struct KeySet {   // 256 key codes
    std::atomic<uint32_t> w[8];
    void Clear() { for (auto& x : w) x = 0; }
    void Add(unsigned k) { if (k < 256) w[k >> 5].fetch_or(1u << (k & 31)); }
    std::string Text(bool dik) const {
        std::string s; int n = 0;
        for (unsigned k = 0; k < 256; k++) if (w[k >> 5] & (1u << (k & 31))) {
            if (++n > 12) { s += " ..."; break; }
            char b[16]; sprintf_s(b, dik ? " dik%02X" : " vk%02X", k); s += b;
        }
        return s.empty() ? " none" : s;
    }
};
static std::atomic<long> d_winKeys, d_diKbdReads, d_diKbdDown, d_diKbdEvents, d_diMouseReads;
static std::atomic<long> d_diKbdReadsEver;   // since the start, open or not: does the game read the keyboard this way at all?
static KeySet d_diKeys, d_asyncKeys, d_stateKeys, d_tableKeys;
static std::atomic<uint8_t*> g_keyTable{nullptr};   // the game's key-down table, once found

void Input_NoteWindowKey() { d_winKeys++; }

// ---- Esc for the game -----------------------------------------------------------------
// When money leaves the balance (a send accepted, a developer -$), an open game menu still
// shows - and checks prices against - the balance it read when it opened (the duplication
// found 2026-11-10). Backing out of a menu makes the game read it afresh, so Gitmo presses
// Esc for the game once: through DirectInput (the game's keyboard), as a window key message,
// or both (EscapeVia in Gitmo.ini). With no menu open, that opens the pause menu - accepted.
// Several quick changes give one Esc, 0.6 s after the last.
static std::atomic<ULONGLONG> g_escUntil{0};   // Esc held down for the game until this tick
static std::atomic<ULONGLONG> g_escWant{0};    // press it at this tick (0 = nothing waiting)
static std::atomic<bool> g_escWorker{false};
static bool EscActive() { return GetTickCount64() < g_escUntil; }
constexpr LPARAM kGitmoKeyMark = 1 << 25;       // (a reserved bit of the key-message lParam) "Gitmo sent this"

void Input_PressEscape() {
    g_escWant = GetTickCount64() + 600;
    if (g_escWorker.exchange(true)) return;
    std::thread([] {
        for (;;) {
            ULONGLONG want = g_escWant, now = GetTickCount64();
            if (!want) break;
            if (now < want) { Sleep((DWORD)std::min<ULONGLONG>(want - now, 200)); continue; }
            if (!g_escWant.compare_exchange_strong(want, 0)) continue;   // a newer change came in: wait for it
            bool viaDi = g_cfg.escapeVia != 2, viaWin = g_cfg.escapeVia != 1;
            g_escUntil = GetTickCount64() + 150;   // DirectInput reads and key checks see Esc held for 150 ms
            HWND w = Overlay_GameWindow();
            if (viaWin && w) {
                PostMessageW(w, WM_KEYDOWN, VK_ESCAPE, 0x00010001 | kGitmoKeyMark);
                Sleep(80);
                PostMessageW(w, WM_KEYUP, VK_ESCAPE, (LPARAM)0xC0010001 | kGitmoKeyMark);
            }
            Log("input: pressed Esc for the game (%s) - an open menu rereads the balance", viaDi && viaWin ? "DirectInput + window" : viaDi ? "DirectInput" : "window");
        }
        g_escWorker = false;
        if (g_escWant) Input_PressEscape();
    }).detach();
}
bool Input_IsGitmoKey(UINT m, WPARAM w, LPARAM l) { return (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR) && (w == VK_ESCAPE || w == 0x1B) && (l & kGitmoKeyMark); }

// clear the game's key-down table; note any key that was set
static void ClearKeyTable() {
    uint8_t* t = g_keyTable;
    if (!t) return;
    bool esc = EscActive();
    __try {
        for (unsigned k = 0; k < 256; k++) if (t[k] && !(esc && k == VK_ESCAPE)) { d_tableKeys.Add(k); t[k] = 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_keyTable = nullptr; }
}

// ---- DirectInput device hooks ---------------------------------------------------
// GetDeviceState / GetDeviceData are patched in every device vtable the game uses. Gitmo
// patches the tables of its own test devices at start-up, and also catches the game's
// CreateDevice calls, so a device with any other table (a wrapper, another DirectInput
// version) is patched the moment the game opens it - in 0.7.3 the game's keyboard reads
// never reached Gitmo, so G still honked the horn while typing.
struct VtOrig { void** vt; void* fn; };
constexpr int kMaxVt = 8;
static VtOrig g_state[kMaxVt], g_data[kMaxVt], g_create[kMaxVt];
static SRWLOCK g_vtLock = SRWLOCK_INIT;

static void* OrigFor(VtOrig* t, void* self) {
    void** vt = *(void***)self;
    for (int i = 0; i < kMaxVt; i++) if (t[i].vt == vt) return t[i].fn;
    for (int i = 0; i < kMaxVt; i++) if (t[i].fn) return t[i].fn;
    return nullptr;
}

static std::string ModName(const void* p) {
    HMODULE m = nullptr; char path[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m);
    if (m) GetModuleFileNameA(m, path, MAX_PATH);
    const char* nm = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    char b[MAX_PATH + 32]; sprintf_s(b, "%s+0x%llX", nm, m ? (unsigned long long)((uintptr_t)p - (uintptr_t)m) : 0ull);
    return b;
}

// May Gitmo put its hook in front of this function? Only if it is DirectInput itself, the
// Steam overlay's device wrapper, or No Auto Reverse (a dinput8.dll proxy that hooks once).
// Anything else (the VR mod) may re-hook its own entries later, saving Gitmo's hook as its
// "original" - then each calls the other forever (0.7.4 froze at launch like that).
static bool SafeToWrap(const void* fn, std::string* owner = nullptr) {
    HMODULE m = nullptr; char path[MAX_PATH] = "";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fn, &m);
    if (m) GetModuleFileNameA(m, path, MAX_PATH);
    const char* nm = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    if (owner) *owner = *nm ? nm : "unknown memory";
    return !_stricmp(nm, "dinput8.dll") || !_stricmp(nm, "gameoverlayrenderer64.dll");
}

// Loop guard: the same device coming back into Gitmo's hook on the same thread means another
// mod's hook led back into ours - stop there. (A wrapper calling the real device inside one
// read is a different device object, which is fine.)
static thread_local void* t_diStack[8];
static thread_local int t_diDepth = 0;
struct DiEnter {
    bool loop = false;
    explicit DiEnter(void* self) {
        for (int i = 0; i < t_diDepth && i < 8; i++) if (t_diStack[i] == self) loop = true;
        if (!loop) { if (t_diDepth < 8) t_diStack[t_diDepth] = self; t_diDepth++; }
    }
    ~DiEnter() { if (!loop) t_diDepth--; }
};
static volatile LONG g_diLoopNoted = 0;
static void NoteDiLoop(const char* fn) {
    if (InterlockedExchange(&g_diLoopNoted, 1) == 0) Log("input: a %s hook loop with another mod was stopped (that read failed once)", fn);
}

// 0 = other (wheel, pedals, gamepad...), 1 = mouse, 2 = keyboard
static int DevKind(void* self) {
    // IDirectInputDevice8::GetCapabilities is slot 3 in both the A and W tables
    using F = HRESULT(STDMETHODCALLTYPE*)(void*, DIDEVCAPS*);
    DIDEVCAPS caps{}; caps.dwSize = sizeof(caps);
    if (FAILED(((F)(*(void***)self)[3])(self, &caps))) return 0;
    BYTE t = GET_DIDEVICE_TYPE(caps.dwDevType);
    return t == DI8DEVTYPE_MOUSE ? 1 : t == DI8DEVTYPE_KEYBOARD ? 2 : 0;
}

static HRESULT STDMETHODCALLTYPE Hk_GetDeviceState(void* self, DWORD cb, void* data) {
    using F = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void*);
    F o = (F)OrigFor(g_state, self);
    if (!o) return E_FAIL;
    HRESULT hr;
    { DiEnter g(self); if (g.loop) { NoteDiLoop("GetDeviceState"); return DIERR_INPUTLOST; } hr = o(self, cb, data); }
    // a keyboard's state is 256 bytes, so only those are worth checking while the phone is closed
    if (!g_menuOpen) {
        if (cb == 256 && DevKind(self) == 2) {
            d_diKbdReadsEver++;
            if (data && SUCCEEDED(hr) && EscActive() && g_cfg.escapeVia != 2) ((uint8_t*)data)[DIK_ESCAPE] |= 0x80;
        }
        return hr;
    }
    int kind = SUCCEEDED(hr) && data ? DevKind(self) : 0;
    if (kind == 2) {
        d_diKbdReads++; d_diKbdReadsEver++;
        bool any = false;
        if (cb == 256) for (unsigned k = 0; k < 256; k++) if (((uint8_t*)data)[k] & 0x80) { d_diKeys.Add(k); any = true; }
        if (any) d_diKbdDown++;
    }
    if (kind == 1) d_diMouseReads++;
    if (kind) memset(data, 0, cb);
    if (kind == 2 && cb == 256 && EscActive() && g_cfg.escapeVia != 2) ((uint8_t*)data)[DIK_ESCAPE] = 0x80;   // Gitmo's own Esc gets through
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_GetDeviceData(void* self, DWORD cbObj, DIDEVICEOBJECTDATA* rgdod, DWORD* inOut, DWORD flags) {
    using F = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, DIDEVICEOBJECTDATA*, DWORD*, DWORD);
    F o = (F)OrigFor(g_data, self);
    if (!o) return E_FAIL;
    HRESULT hr;
    { DiEnter g(self); if (g.loop) { NoteDiLoop("GetDeviceData"); return DIERR_INPUTLOST; } hr = o(self, cbObj, rgdod, inOut, flags); }
    if (g_menuOpen && SUCCEEDED(hr) && inOut) {
        int kind = DevKind(self);
        if (kind == 2) {
            d_diKbdEvents += (long)*inOut;
            if (rgdod && cbObj >= sizeof(DIDEVICEOBJECTDATA) && !(flags & DIGDD_PEEK))
                for (DWORD i = 0; i < *inOut; i++) {
                    auto* e = (DIDEVICEOBJECTDATA*)((uint8_t*)rgdod + i * cbObj);
                    if (e->dwData & 0x80) d_diKeys.Add(e->dwOfs);
                }
        }
        if (kind) *inOut = 0;   // events consumed, none reported
    }
    return hr;
}

// true: this table is now (or already was) patched
static bool PatchDeviceTable(void* dev) {
    void** vt = *(void***)dev;
    AcquireSRWLockExclusive(&g_vtLock);
    int freeIdx = -1;
    for (int i = 0; i < kMaxVt; i++) {
        if (g_state[i].vt == vt) { ReleaseSRWLockExclusive(&g_vtLock); return true; }
        if (!g_state[i].vt && freeIdx < 0) freeIdx = i;
    }
    bool ok = false;
    DWORD old;
    std::string o9, o10;
    if (vt[9] != (void*)Hk_GetDeviceState && (!SafeToWrap(vt[9], &o9) || !SafeToWrap(vt[10], &o10))) {
        ReleaseSRWLockExclusive(&g_vtLock);
        static volatile LONG n = 0;
        if (InterlockedIncrement(&n) <= 8)
            Log("input: DirectInput table %s left alone - already hooked by %s (Gitmo won't hook over another mod)", ModName(vt).c_str(),
                (!SafeToWrap(vt[9]) ? o9 : o10).c_str());
        return false;
    }
    if (freeIdx >= 0 && vt[9] != (void*)Hk_GetDeviceState && VirtualProtect(&vt[9], 2 * sizeof(void*), PAGE_READWRITE, &old)) {
        g_state[freeIdx] = { vt, vt[9] };    // GetDeviceState (the original first, so a call never finds it empty)
        g_data[freeIdx]  = { vt, vt[10] };   // GetDeviceData
        InterlockedExchangePointer(&vt[9], (void*)Hk_GetDeviceState);
        InterlockedExchangePointer(&vt[10], (void*)Hk_GetDeviceData);
        VirtualProtect(&vt[9], 2 * sizeof(void*), old, &old);
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_vtLock);
    return ok;
}

// IDirectInput8::CreateDevice (slot 3): patch the table of every device the game opens
static HRESULT STDMETHODCALLTYPE Hk_CreateDevice(void* self, REFGUID guid, void** out, LPUNKNOWN outer) {
    using F = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, LPUNKNOWN);
    F o = (F)OrigFor(g_create, self);
    if (!o) return E_FAIL;
    HRESULT hr;
    { DiEnter g(self); if (g.loop) { NoteDiLoop("CreateDevice"); return DIERR_GENERIC; } hr = o(self, guid, out, outer); }
    if (SUCCEEDED(hr) && out && *out) {
        void* dev = *out;
        void** vt = *(void***)dev;
        bool known = false;
        for (int i = 0; i < kMaxVt; i++) if (g_state[i].vt == vt) known = true;
        const char* what = IsEqualGUID(guid, GUID_SysKeyboard) ? "keyboard" : IsEqualGUID(guid, GUID_SysMouse) ? "mouse" : "controller";
        // only keyboards and mice: Gitmo never blocks wheels, pedals or gamepads, so their
        // tables (which the VR mod may hook too) are left exactly as they are
        bool kbm = IsEqualGUID(guid, GUID_SysKeyboard) || IsEqualGUID(guid, GUID_SysMouse);
        bool ok = kbm ? PatchDeviceTable(dev) : false;
        if (!kbm) known = true;   // (nothing to report as "new")
        static volatile LONG n = 0;
        if (InterlockedIncrement(&n) <= 24)
            Log("input: game opened a DirectInput %s (table %s, GetDeviceState %s)%s", what, ModName(vt).c_str(), ModName(vt[9]).c_str(),
                known ? "" : ok ? " - new table, now patched" : " - new table, not patched");
    }
    return hr;
}

static void PatchDirectInputTable(void* dinput) {
    void** vt = *(void***)dinput;
    AcquireSRWLockExclusive(&g_vtLock);
    bool have = false;
    for (int i = 0; i < kMaxVt; i++) if (g_create[i].vt == vt) have = true;
    DWORD old;
    if (!have)
        for (int i = 0; i < kMaxVt; i++) if (!g_create[i].vt) {
            std::string o3;
            if (vt[3] != (void*)Hk_CreateDevice && !SafeToWrap(vt[3], &o3)) { Log("input: DirectInput CreateDevice left alone - already hooked by %s", o3.c_str()); break; }
            if (vt[3] != (void*)Hk_CreateDevice && VirtualProtect(&vt[3], sizeof(void*), PAGE_READWRITE, &old)) {
                g_create[i] = { vt, vt[3] };
                InterlockedExchangePointer(&vt[3], (void*)Hk_CreateDevice);
                VirtualProtect(&vt[3], sizeof(void*), old, &old);
            }
            break;
        }
    ReleaseSRWLockExclusive(&g_vtLock);
}

static void HookDirectInput() {
    HMODULE di = GetModuleHandleW(L"dinput8.dll");
    if (!di) di = LoadLibraryW(L"dinput8.dll");
    auto create = di ? (HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, LPUNKNOWN))GetProcAddress(di, "DirectInput8Create") : nullptr;
    if (!create) { Log("input: DirectInput not available"); return; }
    const IID* iids[2] = { &IID_IDirectInput8W, &IID_IDirectInput8A };
    for (int i = 0; i < 2; i++) {
        IUnknown* dinput = nullptr;
        if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, *iids[i], (void**)&dinput, nullptr))) continue;
        using CD = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, LPUNKNOWN);
        // both test devices: in case keyboards and mice ever use different tables
        for (const GUID* g : { &GUID_SysMouse, &GUID_SysKeyboard }) {
            void* dev = nullptr;
            if (SUCCEEDED(((CD)(*(void***)dinput)[3])(dinput, *g, &dev, nullptr)) && dev) {
                PatchDeviceTable(dev);
                ((IUnknown*)dev)->Release();
            }
        }
        PatchDirectInputTable(dinput);   // after Gitmo's own CreateDevice calls: from now on, the game's
        dinput->Release();
    }
    int tables = 0; for (auto& t : g_state) if (t.vt) tables++;
    Log("input: DirectInput mouse/keyboard blocking ready (%d device table(s); the game's own devices are checked as it opens them)", tables);
}

// Each time the phone closes: is every patched table still pointing at Gitmo?
static void CheckDeviceTables() {
    for (int i = 0; i < kMaxVt; i++) {
        void** vt = g_state[i].vt;
        if (!vt) continue;
        if (vt[9] != (void*)Hk_GetDeviceState || vt[10] != (void*)Hk_GetDeviceData)
            Log("  DirectInput table %s: GetDeviceState now %s, GetDeviceData now %s (another mod hooked over Gitmo)",
                ModName(vt).c_str(), ModName(vt[9]).c_str(), ModName(vt[10]).c_str());
    }
}

// ---- the game's own key checks ------------------------------------------------------
using GetKeyFn = SHORT(WINAPI*)(int);
// Start out pointing at the real functions, so the hooks can never call an empty pointer.
static volatile GetKeyFn o_GetAsyncKeyState = &GetAsyncKeyState, o_GetKeyState = &GetKeyState;
// Re-entry guard: if another mod's hook ever leads back into ours (a hook cycle),
// the second visit goes straight to Windows instead of looping forever.
static thread_local int t_inKeyHook = 0;
static SHORT WINAPI Hk_GetAsyncKeyState(int vk) {
    if (t_inKeyHook) return GetAsyncKeyState(vk);
    t_inKeyHook++;
    SHORT r = o_GetAsyncKeyState(vk);
    if (g_menuOpen) {
        if (r & 0x8000) d_asyncKeys.Add((unsigned)vk & 0xFF);
        r = 0;
        ClearKeyTable();   // the game is checking keys right now: make sure its table is empty too
    }
    if (vk == VK_ESCAPE && EscActive() && g_cfg.escapeVia != 2) r = (SHORT)0x8000;
    t_inKeyHook--;
    return r;
}
static SHORT WINAPI Hk_GetKeyState(int vk) {
    if (t_inKeyHook) return GetKeyState(vk);
    t_inKeyHook++;
    SHORT r = o_GetKeyState(vk);
    if (g_menuOpen) {
        if (r & 0x8000) d_stateKeys.Add((unsigned)vk & 0xFF);
        r = (SHORT)(r & 1);   // keep toggle state (Caps Lock etc.), report "not pressed"
        ClearKeyTable();
    }
    t_inKeyHook--;
    return r;
}

// ---- gamepad (XInput) -----------------------------------------------------------------
// The game reads gamepads through XInputGetState pointers it keeps in globals ("slots").
// In VR the VR mod puts its own function in one of them (re-writing it every frame), and
// hands the VR controllers to the game that way - so presses made while using the phone
// opened menus and the map.
// Gitmo never writes those slots (0.7.2's first try did; the VR mod then wrapped Gitmo and
// Gitmo wrapped the VR mod, an endless loop that froze the game at launch). Instead the few
// game instructions that READ a slot ("mov rax, [slot]") are pointed at a cell of Gitmo's
// near the game, holding Gitmo's function; that function calls whatever is in the game's
// slot right now (the VR mod's, or XInput's) and, while the phone is open, reports an idle
// pad. Whoever writes the slot, the order stays game -> Gitmo -> that function.
struct XPad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XState { DWORD packet; XPad pad; };
using XGetState = DWORD(WINAPI*)(DWORD, XState*);
constexpr int kXSlots = 4;
static uintptr_t g_xSlot[kXSlots];
static std::atomic<int> g_xSlots{0};
static std::atomic<long> d_xReads, d_xActive;
static std::atomic<uint32_t> d_xButtons;
static thread_local int t_inXHook = 0;
static void** volatile g_xCells;   // Gitmo's cells near the game (what the redirected reads now read)

template <int N> static DWORD WINAPI Hk_XInputGetState(DWORD user, XState* st);
static void* const kXHooks[kXSlots] = { (void*)&Hk_XInputGetState<0>, (void*)&Hk_XInputGetState<1>, (void*)&Hk_XInputGetState<2>, (void*)&Hk_XInputGetState<3> };

static XGetState SystemXInput() {   // for a slot the game hasn't filled yet
    static XGetState f = [] {
        HMODULE m = GetModuleHandleW(L"xinput9_1_0.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        return m ? (XGetState)GetProcAddress(m, "XInputGetState") : nullptr;
    }();
    return f;
}

template <int N> static DWORD WINAPI Hk_XInputGetState(DWORD user, XState* st) {
    XGetState o = *(XGetState volatile*)g_xSlot[N];   // what the game (or the VR mod) has put there now
    if (!o) o = SystemXInput();
    for (void* h : kXHooks) if ((void*)o == h) o = SystemXInput();   // never ourselves
    if (!o || t_inXHook) return ERROR_DEVICE_NOT_CONNECTED;         // (a loop through another mod: stop it)
    t_inXHook++;
    DWORD r = o(user, st);
    t_inXHook--;
    if (g_menuOpen && r == ERROR_SUCCESS && st) {
        d_xReads++;
        const XPad& p = st->pad;
        bool active = p.buttons || p.lt > 30 || p.rt > 30 || abs(p.lx) > 8000 || abs(p.ly) > 8000 || abs(p.rx) > 8000 || abs(p.ry) > 8000;
        if (active) { d_xActive++; d_xButtons.fetch_or(p.buttons | (p.lt > 30 ? 0x10000u : 0) | (p.rt > 30 ? 0x20000u : 0)); }
        st->pad = XPad{};   // the game sees an idle pad (same packet number: "nothing changed")
    }
    return r;
}

// A page within 2 GB of the game, so a game instruction's 32-bit offset can reach it.
static void** AllocNearExe() {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t p = (g_exeEnd + gran) & ~(gran - 1); p < g_exeEnd + 0x60000000ull; p += gran)
        if (void* r = VirtualAlloc((void*)p, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return (void**)r;
    for (uintptr_t p = (g_exeBase - 0x10000) & ~(gran - 1); p > 0x10000 && p > g_exeBase - 0x60000000ull; p -= gran)
        if (void* r = VirtualAlloc((void*)p, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return (void**)r;
    return nullptr;
}

// Point every "mov r64, [slot]" / "call [slot]" in the game's code at Gitmo's cell instead.
static int RedirectSlotReads(uintptr_t slot, void** cell) {
    auto nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)g_exeBase)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    int done = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* b = (uint8_t*)(g_exeBase + sec->VirtualAddress);
        size_t len = sec->Misc.VirtualSize;
        __try {
            for (size_t k = 0; k + 8 < len; k++) {
                size_t dispAt, insLen;
                if ((b[k] == 0x48 || b[k] == 0x4C) && b[k + 1] == 0x8B && (b[k + 2] & 0xC7) == 0x05) { dispAt = k + 3; insLen = 7; }   // mov r64, [rip+disp32]
                else if (b[k] == 0xFF && b[k + 1] == 0x15) { dispAt = k + 2; insLen = 6; }                                         // call [rip+disp32]
                else continue;
                uintptr_t end = (uintptr_t)(b + k + insLen);
                if (end + *(int32_t*)(b + dispAt) != slot) continue;
                long long nd = (long long)((uintptr_t)cell - end);
                if (nd != (int32_t)nd) continue;
                if (((uintptr_t)(b + dispAt) & 63) > 60) continue;   // the 4 bytes would straddle a cache line: not safe to change live
                DWORD old;
                if (!VirtualProtect(b + dispAt, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
                InterlockedExchange((volatile LONG*)(b + dispAt), (LONG)(int32_t)nd);   // (x64: a locked write needs no alignment)
                VirtualProtect(b + dispAt, 4, old, &old);
                FlushInstructionCache(GetCurrentProcess(), b + k, insLen);
                Log("input: gamepad read at exe+0x%llX now goes through Gitmo", (unsigned long long)((uintptr_t)(b + k) - g_exeBase));
                done++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return done;
}

static void SetUpGamepadBlock() {
    uintptr_t xs[kXSlots];
    int n = Game_FindXInputSlots(xs, kXSlots);
    if (!n) { Log("input: the game's gamepad reads not found - controllers are not blocked while the phone is open"); return; }
    void** cells = AllocNearExe();
    if (!cells) { Log("input: no memory near the game for the gamepad block - controllers are not blocked"); return; }
    int total = 0;
    for (int j = 0; j < n; j++) {
        g_xSlot[j] = xs[j];
        cells[j] = kXHooks[j];
        total += RedirectSlotReads(xs[j], &cells[j]);
    }
    g_xCells = cells;
    g_xSlots = n;
    Log("input: %d gamepad slot(s), %d read(s) redirected - controllers are hidden from the game while the phone is open", n, total);
}

// ---- every frame (overlay) ------------------------------------------------------------
void Input_Frame(bool open) {
    static bool was;
    static ULONGLONG since;
    if (void** cells = g_xCells) {   // another mod writing over Gitmo's cell: note it once (the loop guard keeps it safe)
        static bool noted;
        for (int j = 0; j < g_xSlots && !noted; j++)
            if (cells[j] != kXHooks[j]) { noted = true; Log("input: another mod replaced Gitmo's gamepad cell %d - leaving it", j); }
    }
    if (open && !was) {
        d_winKeys = 0; d_diKbdReads = 0; d_diKbdDown = 0; d_diKbdEvents = 0; d_diMouseReads = 0;
        d_diKeys.Clear(); d_asyncKeys.Clear(); d_stateKeys.Clear(); d_tableKeys.Clear();
        d_xReads = 0; d_xActive = 0; d_xButtons = 0;
        since = GetTickCount64();
    }
    if (open) ClearKeyTable();
    if (!open && was) {
        Log("input while the phone was open (%.0f s): window key presses %ld (kept from the game)", (GetTickCount64() - since) / 1000.0, d_winKeys.load());
        Log("  DirectInput keyboard: %ld reads, %ld with keys down, %ld buffered events, keys%s (all hidden from the game); %ld since start",
            d_diKbdReads.load(), d_diKbdDown.load(), d_diKbdEvents.load(), d_diKeys.Text(true).c_str(), d_diKbdReadsEver.load());
        Log("  DirectInput mouse reads %ld; game key checks GetAsyncKeyState%s / GetKeyState%s (answered 'up')",
            d_diMouseReads.load(), d_asyncKeys.Text(false).c_str(), d_stateKeys.Text(false).c_str());
        if (!g_keyTable) Log("  game key table: not found in this game version");
        else Log("  game key table: keys that reached the game's window handler anyway%s (cleared)", d_tableKeys.Text(false).c_str());
        if (!g_xSlots) Log("  gamepad: the game's XInput reads not found");
        else Log("  gamepad (XInput): %ld reads, %ld with buttons or sticks in use, buttons 0x%05X (all hidden from the game)",
                 d_xReads.load(), d_xActive.load(), d_xButtons.load());
        CheckDeviceTables();
    }
    was = open;
}

// True if p is inside the real Windows DLL (user32), i.e. no other mod has hooked this entry.
static bool PointsIntoSystem(const void* p, const char* dll) {
    HMODULE owner = nullptr, sys = GetModuleHandleA(dll);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &owner);
    return sys && owner == sys;
}

// Replace one entry in SnowRunner.exe's import table (only the game's own calls go through it).
// The original is stored in *orig BEFORE the entry is switched, so a game thread that
// calls through the new entry at that very moment never finds an empty pointer.
// Returns 1 = patched now, 2 = was already ours, 3 = another mod hooked it after us
// (left alone), 0 = not imported.
//
// onlyOverSystem: when re-checking later, only re-hook if the entry points back into the
// Windows DLL (the import table was rebuilt). If another mod (e.g. the VR mod) has put
// its own hook there, it has saved OUR hook as its "original", so the chain is already
// game -> other mod -> Gitmo -> Windows. Hooking over it again would make
// Gitmo -> other mod -> Gitmo -> ... an endless loop - the cause of the launch freezes.
static int PatchImport(const char* dll, const char* fn, void* hook, void* volatile* orig, bool onlyOverSystem) {
    auto base = (uint8_t*)g_exeBase;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, fn) != 0) continue;
            void* prev = (void*)iat->u1.Function;
            if (prev == hook) return 2;
            if (onlyOverSystem && !PointsIntoSystem(prev, dll)) return 3;
            *orig = prev;                        // 1) remember the original
            MemoryBarrier();
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return 0;
            InterlockedExchangePointer((void**)&iat->u1.Function, hook);   // 2) then switch, in one step
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            return 1;
        }
    }
    return 0;
}

static void PatchKeyImports(bool first) {
    // Steam's DRM wrapper may rebuild the import table after start-up, so this is
    // checked again later; a slot already pointing at our hook is left alone.
    static bool noted[2];
    struct { const char* fn; void* hook; void* volatile* orig; } k[2] = {
        { "GetAsyncKeyState", (void*)Hk_GetAsyncKeyState, (void* volatile*)&o_GetAsyncKeyState },
        { "GetKeyState",      (void*)Hk_GetKeyState,      (void* volatile*)&o_GetKeyState } };
    for (int i = 0; i < 2; i++) {
        int r = PatchImport("USER32.dll", k[i].fn, k[i].hook, k[i].orig, !first);
        if (r == 1 && !first) Log("input: %s hook re-applied (import table was reset)", k[i].fn);
        if (r == 3 && !noted[i]) { noted[i] = true; Log("input: %s is now hooked by another mod too - leaving its hook in front of ours", k[i].fn); }
    }
}

void Input_Start() {
    HookDirectInput();
    PatchKeyImports(true);
    Log("input: game key checks %s / %s", o_GetAsyncKeyState ? "GetAsyncKeyState hooked" : "GetAsyncKeyState not imported",
        o_GetKeyState ? "GetKeyState hooked" : "GetKeyState not imported");
    if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            for (int i = 0; i < 12; i++) {
                Sleep(5000);
                PatchKeyImports(false);
                if (i == 1) {   // by now the game's code is unpacked in memory
                    uintptr_t t = Game_FindKeyTable();
                    g_keyTable = (uint8_t*)t;
                    if (t) Log("input: the game's key table found at exe+0x%llX (cleared while the phone is open)", (unsigned long long)(t - g_exeBase));
                    else Log("input: the game's key table not found (another game version?) - window keys are still blocked");
                    SetUpGamepadBlock();
                }
            }
            return 0;
        }, nullptr, 0, nullptr)) CloseHandle(t);
}
