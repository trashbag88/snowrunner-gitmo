// Gitmo - reading game state (current truck, engine, garage) and the phone battery.
//
// Current truck: SnowRunner keeps a global "truck control" object whose +0x08 is
// the vehicle you drive (found the same way SMT does: a small function that
// returns the global's address, called right before "mov [rax], rbx").
//
// Engine running: Vehicle+0x768 (3 = running, 0 = off). In garage: truck-control +0x2C (1/0).
// Both found with the teaching marks (Settings > Developer), which stay available in case
// a game update moves them (then set EngineOffset / GarageOffset in Gitmo.ini).

#include "gitmo.h"
#include <cstdio>
#include <algorithm>
#include <map>

static bool SafeCopyG(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Addresses below 64 KB are never valid (a null pointer plus an offset): skip them instead of
// letting the read fault - a caught fault is harmless, but other mods' crash loggers report it.
template <class T> static bool Rd(uintptr_t a, T& v) { return a >= 0x10000 && SafeCopyG(&v, (void*)a, sizeof(T)); }

// ---- finding the truck-control global ----------------------------------------------------
static uintptr_t g_truckCtrlGlobal;   // address of the global pointer

static uintptr_t ScanRange(const uint8_t* b, size_t n, const int* pat, size_t pn, int& count) {
    uintptr_t found = 0;
    __try {
        for (size_t k = 0; k + pn <= n; k++) {
            size_t j = 0;
            while (j < pn && (pat[j] < 0 || b[k + j] == pat[j])) j++;
            if (j == pn) { found = (uintptr_t)(b + k); count++; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { count = 99; }
    return found;
}

static uintptr_t FindPattern(const char* sig) {
    std::vector<int> pat;
    for (const char* p = sig; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '?') { pat.push_back(-1); p++; continue; }
        pat.push_back((int)strtol(p, (char**)&p, 16));
    }
    auto nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)g_exeBase)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t found = 0; int count = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t f = ScanRange((const uint8_t*)(g_exeBase + sec->VirtualAddress), sec->Misc.VirtualSize, pat.data(), pat.size(), count);
        if (f) found = f;
    }
    return count == 1 ? found : 0;
}

static uintptr_t LocateTruckControl() {
    uintptr_t at = FindPattern("40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 33 C9 48 89 18");
    if (!at) return 0;
    int32_t rel = 0;
    if (!Rd(at + 10, rel)) return 0;
    uintptr_t callee = at + 9 + 5 + rel;
    // callee is "lea rax, [rip+disp32]; ret"
    uint8_t c[8];
    if (!SafeCopyG(c, (void*)callee, 8) || c[0] != 0x48 || c[1] != 0x8D || c[2] != 0x05 || c[7] != 0xC3) return 0;
    return callee + 7 + *(int32_t*)(c + 3);
}

// SnowRunner's own "key is down" table: 256 bytes indexed by Windows key code, filled by the
// game's window handler on WM_KEYDOWN / WM_KEYUP ("movzx eax, r13b; mov [rax+rdi+table], cl",
// rdi = the exe's base) and read by its hotkey checks. 0 if this game version differs.
uintptr_t Game_FindKeyTable() {
    uintptr_t at = FindPattern("41 0F B6 C5 88 8C 38 ? ? ? ? 8D 86 FF FD FF FF");
    int32_t rva = 0;
    if (!at || !Rd(at + 7, rva) || rva <= 0 || g_exeBase + (uint32_t)rva + 0x200 > g_exeEnd) return 0;
    return g_exeBase + (uint32_t)rva;
}

// The game loads XInput itself and keeps the address of XInputGetState in globals:
//   lea rdx, ["XInputGetState"]; ...; call [GetProcAddress]; mov [global], rax
// Returns those globals (up to max), found from the name string, so a game update that
// moves the code still works.
int Game_FindXInputSlots(uintptr_t* out, int max) {
    auto nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)g_exeBase)->e_lfanew);
    static const char name[] = "XInputGetState";   // with its terminating 0
    int pn[sizeof name]; for (size_t i = 0; i < sizeof name; i++) pn[i] = (unsigned char)name[i];
    uintptr_t str = 0; int cnt = 0;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        uintptr_t f = ScanRange((const uint8_t*)(g_exeBase + sec->VirtualAddress), sec->Misc.VirtualSize, pn, sizeof name, cnt);
        if (f && !str) str = f;
    }
    if (!str) return 0;
    int n = 0;
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && n < max; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* b = (const uint8_t*)(g_exeBase + sec->VirtualAddress);
        size_t len = sec->Misc.VirtualSize;
        __try {
            for (size_t k = 0; k + 7 + 40 < len && n < max; k++) {
                if (b[k] != 0x48 || b[k + 1] != 0x8D || b[k + 2] != 0x15) continue;   // lea rdx, [rip+disp32]
                if ((uintptr_t)(b + k + 7) + *(const int32_t*)(b + k + 3) != str) continue;
                for (size_t j = k + 7; j < k + 7 + 32; j++)                                // the "mov [rip+disp32], rax" after the call
                    if (b[j] == 0x48 && b[j + 1] == 0x89 && b[j + 2] == 0x05) {
                        uintptr_t g = (uintptr_t)(b + j + 7) + *(const int32_t*)(b + j + 3);
                        if (g > g_exeBase && g + 8 <= g_exeEnd) out[n++] = g;
                        break;
                    }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return n;
}

static std::atomic<bool> g_tcSearchDone{false};

// The truck-control object exists only while a save is loaded: it is 0 in the main menu and
// a new object for every load (checked 2026-11-09 by loading two saves from the menu).
uintptr_t Game_SessionId() {
#ifdef GITMO_TEST
    return getenv("GITMO_TEST_MENU") ? 0 : 1;   // the test harness has no game: "a save is loaded" unless testing the main menu
#endif
    if (!g_truckCtrlGlobal) return g_tcSearchDone ? 1 : 0;   // not found (game update?): never block anything
    uintptr_t tc = 0;
    if (!Rd(g_truckCtrlGlobal, tc)) return 0;
    return tc;
}
bool Game_InGame() { return Game_SessionId() != 0; }

uintptr_t Game_CurrentVehicle() {
    uintptr_t tc = 0, veh = 0;
    if (!Rd(g_truckCtrlGlobal, tc) || !Rd(tc + 8, veh)) return 0;
    return veh;
}

// ---- engine / garage ----------------------------------------------------------------------
static int g_engineOff = -1, g_garageOff = -1;

int Game_EngineRunning() {   // 1 yes, 0 no, -1 unknown / no truck
    uintptr_t veh = Game_CurrentVehicle();
    uint8_t v = 0;
    if (!veh || g_engineOff < 0 || !Rd(veh + g_engineOff, v)) return -1;
    // The byte is a set of flags: the low two bits are the engine (3 = running, 0 = off); the
    // others are separate states (seen while driving: 0x83, 0x0F, 0x1F, 0x2F), so only those two count.
    static uint8_t seen[256];
    if (!seen[v]) { seen[v] = 1; Log("game: engine state byte 0x%02X -> engine %s", v, (v & 3) ? "running" : "off"); }
    return (v & 3) != 0 ? 1 : 0;
}

int Game_InGarage() {
    uintptr_t tc = 0;
    uint8_t v = 0;
    if (g_garageOff < 0 || !Rd(g_truckCtrlGlobal, tc) || !tc || !Rd(tc + g_garageOff, v)) return -1;
    if (v > 1) return -1;
    return v;
}

// ---- research marks -----------------------------------------------------------------------
struct Snap { int kind; std::vector<uint8_t> veh, ta, tc, data; uintptr_t vehAddr; };
static std::mutex g_snapLock;
static std::vector<Snap> g_snaps;
static uintptr_t g_dataStart, g_dataSize;   // the exe's writable section(s)

static void FindDataSection() {
    auto nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)g_exeBase)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if ((sec->Characteristics & IMAGE_SCN_MEM_WRITE) && sec->Misc.VirtualSize > g_dataSize) {
            g_dataStart = g_exeBase + sec->VirtualAddress; g_dataSize = sec->Misc.VirtualSize;
        }
    if (g_dataSize > (64u << 20)) g_dataSize = 64u << 20;
}

static std::vector<uint8_t> Grab(uintptr_t a, size_t n) {
    std::vector<uint8_t> v(n, 0xCD);
    if (a) for (size_t off = 0; off < n; off += 0x1000) SafeCopyG(&v[off], (void*)(a + off), std::min<size_t>(0x1000, n - off));
    return v;
}

static const char* kKindNames[] = { "ENGINE ON", "ENGINE OFF", "IN GARAGE", "DRIVING (not garage)" };

static void Analyse(int a, int b, const char* what, std::vector<uint8_t> Snap::*field, uintptr_t base, bool exeRel) {
    std::vector<const Snap*> A, B;
    for (auto& s : g_snaps) { if (s.kind == a) A.push_back(&s); if (s.kind == b) B.push_back(&s); }
    if (A.empty() || B.empty()) return;
    size_t n = (A[0]->*field).size();
    int shown = 0;
    for (size_t i = 0; i < n && shown < 40; i++) {
        uint8_t va = (A[0]->*field)[i], vb = (B[0]->*field)[i];
        if (va == vb) continue;
        bool ok = true;
        for (auto* s : A) if ((s->*field).size() <= i || (s->*field)[i] != va) ok = false;
        for (auto* s : B) if ((s->*field).size() <= i || (s->*field)[i] != vb) ok = false;
        if (!ok) continue;
        if (exeRel) Log("  %s: exe+0x%llX  %s=%u  %s=%u", what, (unsigned long long)(base - g_exeBase + i), kKindNames[a], va, kKindNames[b], vb);
        else Log("  %s: +0x%zX  %s=%u  %s=%u", what, i, kKindNames[a], va, kKindNames[b], vb);
        shown++;
    }
    if (!shown) Log("  %s: no byte matches every mark", what);
}

void Game_Mark(int kind) {   // 0 engine on, 1 engine off, 2 in garage, 3 driving
    uintptr_t tc = 0, veh = 0, ta = 0;
    Rd(g_truckCtrlGlobal, tc); Rd(tc + 8, veh); Rd(veh + 0x68, ta);
    Snap s; s.kind = kind; s.vehAddr = veh;
    s.veh = Grab(veh, 0x2000); s.ta = Grab(ta, 0x800); s.tc = Grab(tc, 0x800);
    if (kind >= 2) s.data = Grab(g_dataStart, g_dataSize);
    std::lock_guard<std::mutex> lk(g_snapLock);
    g_snaps.push_back(std::move(s));
    int counts[4] = {};
    for (auto& x : g_snaps) counts[x.kind]++;
    Log("mark: %s (truck %p) - marks so far: on %d, off %d, garage %d, driving %d", kKindNames[kind], (void*)veh,
        counts[0], counts[1], counts[2], counts[3]);
    if (kind <= 1 && counts[0] && counts[1]) {
        // only compare snapshots of the same truck
        Log("research: bytes that follow ENGINE ON / OFF (same truck only):");
        std::vector<Snap> keep = g_snaps;
        g_snaps.erase(std::remove_if(g_snaps.begin(), g_snaps.end(), [&](const Snap& x) { return x.kind <= 1 && x.vehAddr != veh; }), g_snaps.end());
        Analyse(0, 1, "vehicle", &Snap::veh, 0, false);
        Analyse(0, 1, "truckaction", &Snap::ta, 0, false);
        Analyse(0, 1, "truckcontrol", &Snap::tc, 0, false);
        g_snaps = keep;
    }
    if (kind >= 2 && counts[2] && counts[3]) {
        Log("research: bytes that follow IN GARAGE / DRIVING:");
        Analyse(2, 3, "truckcontrol", &Snap::tc, 0, false);
        Analyse(2, 3, "global", &Snap::data, g_dataStart, true);
    }
}

int Game_MarkCount(int kind) {
    std::lock_guard<std::mutex> lk(g_snapLock);
    int n = 0; for (auto& s : g_snaps) if (s.kind == kind) n++;
    return n;
}

// ---- phone battery ----------------------------------------------------------------------
// 15 minutes of use. Draining whenever the engine is off; charging (2 minutes of
// battery per minute) while the engine runs or you're in the garage. Paused
// while no truck is loaded (main menu, loading) or the state is unknown.
static std::atomic<float> g_battMin{15.0f};
static std::atomic<int> g_battState{0};   // 0 unknown/paused, 1 charging, 2 on battery
const float kBatteryMinutes = 15.0f;

float Battery_Minutes() { return g_battMin; }
void Battery_Set(float minutes) {   // developer test buttons
    minutes = std::min(std::max(minutes, 0.0f), kBatteryMinutes);
    g_battMin = minutes;
    SaveBattery(minutes);
    Log("battery: set to %.2f min by the developer buttons", minutes);
}
int Battery_State() { return g_battState; }

static DWORD WINAPI GameThread(LPVOID) {
    FindDataSection();
    for (int i = 0; i < 120 && !g_truckCtrlGlobal; i++) {   // the code is decrypted shortly after start
        Sleep(1000);
        g_truckCtrlGlobal = LocateTruckControl();
    }
    g_tcSearchDone = true;
    if (g_truckCtrlGlobal) Log("game: truck control global at exe+0x%llX", (unsigned long long)(g_truckCtrlGlobal - g_exeBase));
    else Log("game: truck control global NOT found -> battery stays as it is; the phone opens anywhere");
    ULONGLONG last = GetTickCount64(), lastSave = last;
    int lastState = -1;
    for (;;) {
        Sleep(500);
        ULONGLONG now = GetTickCount64();
        float dtMin = (now - last) / 60000.0f;
        last = now;
        int eng = Game_EngineRunning(), gar = Game_InGarage();
        int st = 0;
        if (Game_CurrentVehicle() || gar == 1) {
            if (eng == 1 || gar == 1) st = 1;
            else if (eng == 0) st = 2;
        }
        float m = g_battMin;
        if (st == 1) m = std::min(kBatteryMinutes, m + 2 * dtMin);
        if (st == 2) m = std::max(0.0f, m - dtMin);
        g_battMin = m;
        g_battState = st;
        if (st != lastState) { Log("battery: %s (%.1f min left)", st == 1 ? "charging" : st == 2 ? "on battery" : "paused (no truck / state unknown)", m); lastState = st; }
        if (now - lastSave > 30000) { lastSave = now; SaveBattery(m); }
    }
}

void Game_Start() {
    g_battMin = std::min(std::max(g_cfg.batteryMin, 0.0f), kBatteryMinutes);
    g_engineOff = g_cfg.engineOffset;
    g_garageOff = g_cfg.garageOffset;
    if (HANDLE t = CreateThread(nullptr, 0, GameThread, nullptr, 0, nullptr)) CloseHandle(t);
}
