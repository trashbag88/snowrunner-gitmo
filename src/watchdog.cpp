// Gitmo - freeze recorder.
//
// If the game window stops answering Windows for 6 seconds ("Not responding"),
// write every thread's call stack to Gitmo.log as module+offset, so a freeze can
// be traced to the module it is stuck in (the game, a driver, DirectInput, a mod).
//
// Careful about deadlocks: a thread is only suspended long enough to read its
// registers (no locks, no allocation while it is stopped); its stack is walked
// after it has been resumed. A truly frozen thread doesn't move, so its stack
// reads correctly; a running one may give a few junk frames, which is harmless.
// At most 3 reports per session, and only once the window answers again in between.

#include "gitmo.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>

HWND Overlay_Window();

static const int kMaxThreads = 256, kMaxFrames = 28;
struct ThreadStack { DWORD tid; int n; uint64_t pc[kMaxFrames]; };
static ThreadStack g_stacks[kMaxThreads];

static int Unwind(CONTEXT ctx, uint64_t* out, int max) {
    int n = 0;
    __try {
        while (n < max && ctx.Rip) {
            out[n++] = ctx.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (fe) {
                void* handlerData = nullptr; DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
            } else {   // leaf function: return address is at the top of the stack
                ctx.Rip = *(DWORD64*)ctx.Rsp;
                ctx.Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

static int CaptureAll() {
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{ sizeof(te) };
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok && count < kMaxThreads; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        bool got = false;
        if (SuspendThread(t) != (DWORD)-1) {
            got = GetThreadContext(t, &ctx) != 0;   // registers only while stopped
            ResumeThread(t);
        }
        CloseHandle(t);
        if (!got) continue;
        ThreadStack& s = g_stacks[count++];
        s.tid = te.th32ThreadID;
        s.n = Unwind(ctx, s.pc, kMaxFrames);
    }
    CloseHandle(snap);
    return count;
}

// Module name without the loader lock (a frozen thread may be holding it):
// VirtualQuery gives the module's base, GetMappedFileName asks the kernel for its file.
static void Where(uint64_t pc, char* out, size_t cap) {
    MEMORY_BASIC_INFORMATION mbi;
    wchar_t path[MAX_PATH];
    if (VirtualQuery((void*)pc, &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE &&
        K32GetMappedFileNameW(GetCurrentProcess(), (void*)pc, path, MAX_PATH)) {
        const wchar_t* name = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
        sprintf_s(out, cap, "%ls+0x%llX", name, (unsigned long long)(pc - (uint64_t)mbi.AllocationBase));
    } else {
        sprintf_s(out, cap, "%p", (void*)pc);
    }
}

static void Report(HWND hwnd, int seconds) {
    DWORD uiTid = GetWindowThreadProcessId(hwnd, nullptr);
    int n = CaptureAll();
    Log("FREEZE: the game window has not responded for %d s - stacks of all %d threads follow (main window thread first)", seconds, n);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            const ThreadStack& s = g_stacks[i];
            if ((pass == 0) != (s.tid == uiTid)) continue;
            Log(" thread %lu%s:", s.tid, s.tid == uiTid ? " (MAIN window thread)" : "");
            for (int f = 0; f < s.n; f++) {
                char w[300]; Where(s.pc[f], w, sizeof(w));
                Log("   %2d %s", f, w);
            }
        }
    }
    Log("FREEZE: end of report");
}

static DWORD WINAPI WatchdogThread(LPVOID) {
    HWND hwnd = nullptr;
    while (!(hwnd = Overlay_Window())) Sleep(500);
    int reports = 0;
    bool reportedThisFreeze = false;
    ULONGLONG hungSince = 0;   // 0 = responding
    for (;;) {
        Sleep(1000);
        if (!IsWindow(hwnd)) { HWND h = Overlay_Window(); if (h) hwnd = h; else continue; }
        DWORD_PTR r;
        bool alive = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &r) != 0;
        ULONGLONG now = GetTickCount64();
        if (alive) {
            if (reportedThisFreeze) Log("FREEZE: the game window is responding again after %llu s", (now - hungSince) / 1000);
            hungSince = 0; reportedThisFreeze = false;
            continue;
        }
        if (!hungSince) hungSince = now - 1000;   // it already failed to answer within the 1 s wait
        if (now - hungSince >= 6000 && !reportedThisFreeze && reports < 3) {
            reportedThisFreeze = true; reports++;
            Report(hwnd, (int)((now - hungSince) / 1000));
        }
    }
}

void Watchdog_Start() {
    if (HANDLE t = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr)) CloseHandle(t);
}
