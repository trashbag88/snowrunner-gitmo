// Do DirectInput mouse and keyboard devices share one vtable?
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstdio>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
int main() {
    for (int w = 0; w < 2; w++) {
        IUnknown* di = nullptr;
        if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, w ? IID_IDirectInput8W : IID_IDirectInput8A, (void**)&di, nullptr))) continue;
        using CD = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, LPUNKNOWN);
        void *m = nullptr, *k = nullptr;
        ((CD)(*(void***)di)[3])(di, GUID_SysMouse, &m, nullptr);
        ((CD)(*(void***)di)[3])(di, GUID_SysKeyboard, &k, nullptr);
        void** vm = m ? *(void***)m : nullptr; void** vk = k ? *(void***)k : nullptr;
        printf("%s: mouse vtable %p, keyboard vtable %p -> %s; GetDeviceState mouse %p keyboard %p\n", w ? "W" : "A", vm, vk,
               vm == vk ? "SAME" : "DIFFERENT", vm ? vm[9] : nullptr, vk ? vk[9] : nullptr);
    }
}
