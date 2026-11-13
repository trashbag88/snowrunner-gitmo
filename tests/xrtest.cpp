// Checks Gitmo's OpenXR hand-off without starting a VR session:
// load Gitmo (which patches the runtime's negotiate export), then negotiate the way the
// OpenXR loader does and make sure the runtime's function lookup still works through Gitmo.
#include <windows.h>
#include <cstdio>
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { printf("usage: xrtest <gitmo.dll> <runtime.dll>\n"); return 2; }
    HMODULE g = LoadLibraryW(argv[1]);
    printf("gitmo loaded: %s\n", g ? "yes" : "NO");
    Sleep(1500);   // Gitmo's start-up thread
    HMODULE rt = GetModuleHandleW(wcsrchr(argv[2], L'\\') + 1);
    printf("runtime already loaded by gitmo: %s\n", rt ? "yes" : "NO");
    if (!rt) rt = LoadLibraryW(argv[2]);
    auto neg = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(rt, "xrNegotiateLoaderRuntimeInterface");
    MEMORY_BASIC_INFORMATION mbi{}; VirtualQuery((void*)neg, &mbi, sizeof(mbi));
    printf("negotiate export points %s the runtime image\n", (HMODULE)mbi.AllocationBase == rt ? "INSIDE (not patched)" : "outside (Gitmo stub)");
    XrNegotiateLoaderInfo li{ XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(li),
                              1, XR_CURRENT_LOADER_RUNTIME_VERSION, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0x3ff, 0xfff) };
    XrNegotiateRuntimeRequest rr{ XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(rr) };
    XrResult r = neg(&li, &rr);
    printf("negotiate: %d, gipa=%p\n", (int)r, (void*)rr.getInstanceProcAddr);
    VirtualQuery((void*)rr.getInstanceProcAddr, &mbi, sizeof(mbi));
    printf("gipa belongs to %s\n", (HMODULE)mbi.AllocationBase == g ? "Gitmo (good)" : (HMODULE)mbi.AllocationBase == rt ? "the runtime (NOT wrapped)" : "?");
    PFN_xrEnumerateInstanceExtensionProperties en = nullptr;
    r = rr.getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&en);
    uint32_t n = 0;
    XrResult r2 = en ? en(nullptr, 0, &n, nullptr) : XR_ERROR_FUNCTION_UNSUPPORTED;
    printf("lookup through gitmo: %d, extensions from the runtime: %d (%u)\n", (int)r, (int)r2, n);
    return 0;
}
