// DirectInput hook-fight test for Gitmo 0.7.5: a "VR mod" hooks the device table's
// GetDeviceState first and keeps re-asserting it (saving whatever it finds as its original),
// then Gitmo loads and the "game" opens a mouse and a keyboard and reads them many times.
// 0.7.4 looped forever in this situation; 0.7.5 must not.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstdio>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
using GDS = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void*);
static void** g_vt;
static GDS volatile s_vrOrig;
static volatile long s_depth, s_maxDepth, s_vrCalls;
static HRESULT STDMETHODCALLTYPE VrGetDeviceState(void* self, DWORD cb, void* data) {
    long d = InterlockedIncrement(&s_depth); if (d > s_maxDepth) s_maxDepth = d;
    InterlockedIncrement(&s_vrCalls);
    HRESULT hr = (d < 60 && s_vrOrig) ? s_vrOrig(self, cb, data) : E_FAIL;   // (the real one has no limit)
    InterlockedDecrement(&s_depth);
    return hr;
}
static void VrAssert() {   // the VR mod re-hooking its entry
    if (g_vt[9] != (void*)VrGetDeviceState) {
        DWORD old; VirtualProtect(&g_vt[9], 8, PAGE_READWRITE, &old);
        s_vrOrig = (GDS)g_vt[9]; g_vt[9] = (void*)VrGetDeviceState;
        VirtualProtect(&g_vt[9], 8, old, &old);
    }
}
int main(int argc, char** argv) {
    IDirectInput8W* di = nullptr;
    DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&di, nullptr);
    IDirectInputDevice8W* probe = nullptr;
    di->CreateDevice(GUID_SysMouse, &probe, nullptr);
    g_vt = *(void***)probe;
    VrAssert();                                   // VR mod hooks first
    HMODULE h = LoadLibraryA(argv[1]);            // then Gitmo
    if (!h) { printf("load failed\n"); return 1; }
    Sleep(500);
    IDirectInputDevice8W *mouse = nullptr, *kbd = nullptr;
    di->CreateDevice(GUID_SysMouse, &mouse, nullptr);   // the "game" opens its devices
    di->CreateDevice(GUID_SysKeyboard, &kbd, nullptr);
    mouse->SetDataFormat(&c_dfDIMouse); kbd->SetDataFormat(&c_dfDIKeyboard);
    long ok = 0, calls = 0;
    ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 4000) {
        VrAssert();
        DIMOUSESTATE ms; BYTE keys[256];
        s_depth = 0;
        mouse->Acquire(); kbd->Acquire();
        if (SUCCEEDED(mouse->GetDeviceState(sizeof ms, &ms))) ok++;
        if (SUCCEEDED(kbd->GetDeviceState(sizeof keys, keys))) ok++;
        calls += 2;
        Sleep(5);
    }
    printf("reads %ld, succeeded %ld, VR hook calls %ld, deepest VR nesting %ld\n", calls, ok, s_vrCalls, s_maxDepth);
    printf("%s\n", s_maxDepth <= 2 ? "NO LOOP" : "LOOP");
    return 0;
}
