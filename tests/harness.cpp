// Test harness for Gitmo: loads version.dll, checks the proxy, then renders a
// hidden DX11 window so the overlay draws (test build captures screenshots).
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dxguid.lib")
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
extern "C" void* __imp_GetAsyncKeyState;

__declspec(noinline) void FreezeHere() { Sleep(9000); }   // simulated game freeze
int wmain(int argc, wchar_t** argv) {
    HMODULE h = LoadLibraryW(argv[1]);
    if (!h) { printf("load failed %lu\n", GetLastError()); return 1; }
    auto sizeW = (DWORD(WINAPI*)(LPCWSTR, LPDWORD))GetProcAddress(h, "GetFileVersionInfoSizeW");
    auto infoW = (BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID))GetProcAddress(h, "GetFileVersionInfoW");
    auto queryW = (BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT))GetProcAddress(h, "VerQueryValueW");
    DWORD dummy, n = sizeW(L"C:\\Windows\\System32\\kernel32.dll", &dummy);
    void* buf = malloc(n);
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (infoW(L"C:\\Windows\\System32\\kernel32.dll", 0, n, buf) && queryW(buf, L"\\", (void**)&fi, &len))
        printf("proxy ok: kernel32 version %u.%u.%u.%u (info size %lu)\n", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
               HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS), n);
    else printf("proxy FAILED\n");

    // a fake profile with the save's values
    volatile int32_t* prof = (int32_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    // the game's profile record layout, twice: a stale copy first, then the live one
    const int bases[2] = {100, 200};
    for (int base : bases) {
        prof[base] = _wtoi(argv[2]); prof[base + 1] = 0x10000; prof[base + 2] = _wtoi(argv[3]); prof[base + 3] = 0x10000;
        *(volatile uint64_t*)&prof[base + 4] = 0x0020012345678000ull;
        prof[base + 6] = _wtoi(argv[4]); prof[base + 7] = 0x10000;
    }
    prof += 100;   // prof[100] below = the live record (absolute index 200)

    Sleep(500);
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"GitmoTest";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(L"GitmoTest", L"Gitmo test", WS_OVERLAPPEDWINDOW, 0, 0, 1600 + 16, 900 + 39, nullptr, nullptr, wc.hInstance, nullptr);
    ID3D11Device* dev; ID3D11DeviceContext* ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { printf("no device\n"); return 1; }
    IDXGIFactory1* f; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f);
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferDesc.Width = argc > 6 ? _wtoi(argv[5]) : 1600; d.BufferDesc.Height = argc > 6 ? _wtoi(argv[6]) : 900; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2;
    d.OutputWindow = wnd; d.Windowed = TRUE; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* sc;
    HRESULT hr = f->CreateSwapChain(dev, &d, &sc);
    printf("swap chain hr=0x%08lX\n", hr);
    ID3D11Texture2D* bb; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    ID3D11RenderTargetView* rtv; dev->CreateRenderTargetView(bb, nullptr, &rtv); bb->Release();
    ULONGLONG t0 = GetTickCount64();
    int step = 0;
    long long sink = 0;
    while (GetTickCount64() - t0 < 25000) {
        ULONGLONG t = GetTickCount64() - t0;
        if (step == 0 && t > 3000) { prof[100] += 3200; step++; }
        if (step == 1 && t > 3600) { prof[100] -= 750; step++; }
        if (step == 2 && t > 4200) { prof[100] += 12000; step++; }
        if (step == 3 && t > 8000) { FreezeHere(); step = 10; }
        if (step == 3 && t > 20000) { prof[103] = 0; prof[3] = 0; step++; }   // both records freed/reused
        if (step < 4) sink += prof[100] + GetAsyncKeyState(VK_LBUTTON);   // the "HUD" reading the money every frame
        float col[4] = { 0.36f, 0.42f, 0.30f, 1 };   // muddy green, like a map
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, col);
        sc->Present(0, 0);
        Sleep(16);
        MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }
    // Which module now answers DirectInput mouse reads and the exe's key checks?
    auto modOf = [](void* p) {
        static char name[MAX_PATH]; HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m);
        GetModuleFileNameA(m, name, MAX_PATH); return strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name; };
    auto dic = (HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, LPUNKNOWN))GetProcAddress(LoadLibraryW(L"dinput8.dll"), "DirectInput8Create");
    IDirectInput8W* di = nullptr; IDirectInputDevice8W* mouse = nullptr;
    dic(GetModuleHandleW(nullptr), 0x0800, IID_IDirectInput8W, (void**)&di, nullptr);
    di->CreateDevice(GUID_SysMouse, &mouse, nullptr);
    void** vt = *(void***)mouse;
    printf("mouse GetDeviceState -> %s, GetDeviceData -> %s\n", modOf(vt[9]), modOf(vt[10]));
    printf("exe GetAsyncKeyState -> %s\n", modOf(__imp_GetAsyncKeyState));
    printf("done (%lld)\n", sink & 1);
    return 0;
}



