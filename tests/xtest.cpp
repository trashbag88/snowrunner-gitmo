// Gamepad-slot test for Gitmo 0.7.3: a "game" that keeps XInputGetState in a global and
// reads it with "mov rax, [slot]", plus a "VR mod" that keeps re-wrapping that global the
// way the real one does (saving whatever is there as its original). 0.7.2 looped forever
// here; 0.7.3 must not, and every read must pass through Gitmo.
#include <windows.h>
#include <cstdio>
#include <cstdint>
struct XS { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
using F = DWORD(WINAPI*)(DWORD, XS*);
F volatile g_slot;                      // the game's global
static F volatile s_vrOrig;
static volatile long s_vrCalls, s_depth, s_maxDepth;
static DWORD WINAPI VrHook(DWORD u, XS* st) {   // the "VR mod": calls its saved original, then adds a button
    long d = InterlockedIncrement(&s_depth); if (d > s_maxDepth) s_maxDepth = d;
    InterlockedIncrement(&s_vrCalls);
    DWORD r = s_vrOrig ? s_vrOrig(u, st) : ERROR_DEVICE_NOT_CONNECTED;
    if (d > 50) { InterlockedDecrement(&s_depth); return r; }   // (the real one has no such limit)
    st->buttons |= 0x1000;   // "A" from the VR controller
    InterlockedDecrement(&s_depth);
    return ERROR_SUCCESS;
}
__declspec(noinline) void GameInit() {
    HMODULE m = LoadLibraryA("xinput9_1_0.dll");
    g_slot = (F)GetProcAddress(m, "XInputGetState");
}
__declspec(noinline) DWORD GameRead(XS* st) {
    F f = g_slot;
    if (!f) return 1;
    return f(0, st);
}
int main(int argc, char** argv) {
    GameInit();
    HMODULE h = LoadLibraryA(argv[1]);
    if (!h) { printf("load failed\n"); return 1; }
    ULONGLONG t0 = GetTickCount64();
    long reads = 0, withA = 0;
    while (GetTickCount64() - t0 < 16000) {
        if (GetTickCount64() - t0 > 3000 && g_slot != (F)VrHook) { s_vrOrig = g_slot; g_slot = VrHook; }   // VR mod re-asserting its hook
        XS st{}; s_depth = 0;
        DWORD r = GameRead(&st);
        reads++; if (r == 0 && (st.buttons & 0x1000)) withA++;
        Sleep(10);
    }
    printf("reads %ld, with VR button %ld, VR hook calls %ld, deepest VR nesting %ld\n", reads, withA, s_vrCalls, s_maxDepth);
    printf("%s\n", s_maxDepth <= 1 ? "NO LOOP" : "LOOP");
    return 0;
}
