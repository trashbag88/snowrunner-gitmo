// Gitmo - finding and tracking the player's money.
//
// Stage one method (no published offsets exist for this):
//  1. Read money, experience and rank from all four save slots, CompleteSave*.cfg
//     (Steam) or CompleteSave*.dat (Epic), each time a save is loaded.
//  2. Scan the game's writable memory for that money value with the same
//     experience value close by - the profile keeps them together, so the
//     pair is close to unique.
//  3. Track the value live for the phone's balance and activity feed.
//  4. Research=1: log the neighbourhood, what points to it and which game code
//     reads/writes it (a hardware breakpoint, logging only), so the next
//     version can locate it directly by code signature instead of scanning.
// Every memory read is exception-guarded; nothing is written unless the
// developer test buttons in the phone's Settings are pressed.

#include "gitmo.h"
#include <tlhelp32.h>
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include <set>
#include <map>
#include <cwctype>
#include <thread>
#include <climits>

static std::mutex g_mx;
static MoneyState g_st;                 // guarded by g_mx
static std::atomic<bool> g_rescan{false};

// ---- guarded memory access ------------------------------------------------------
static bool SafeCopy(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeRead32(uintptr_t a, int32_t& v) { return SafeCopy(&v, (const void*)a, 4); }
static bool SafeWrite32(uintptr_t a, int32_t v) {
    __try { *(volatile int32_t*)a = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- save file ------------------------------------------------------------------
static bool ParseNumberAfter(const std::string& s, size_t from, const char* key, long long& out) {
    size_t k = s.find(key, from);
    if (k == std::string::npos) return false;
    size_t p = k + strlen(key);
    while (p < s.size() && (s[p] == ' ' || s[p] == ':')) p++;
    char* end = nullptr;
    out = strtoll(s.c_str() + p, &end, 10);
    return end != s.c_str() + p;
}

// SnowRunner has four save slots. Steam keeps them as CompleteSave.cfg (slot 1),
// CompleteSave1.cfg (slot 2), CompleteSave2.cfg, CompleteSave3.cfg in the Steam
// cloud folder; Epic as CompleteSave*.dat under Documents. All four are read, and the
// balance is matched against each, so whichever save is loaded is found.
struct SlotSave { int slot = 0; SaveValues sv; FILETIME written{}; };

static int SlotOf(const wchar_t* name) {   // "CompleteSave2.cfg" -> 3, "CompleteSave.cfg" -> 1
    const wchar_t* p = name + wcslen(L"CompleteSave");
    return iswdigit(*p) ? (int)wcstol(p, nullptr, 10) + 1 : 1;
}

static void CollectSlots(const std::wstring& dir, const wchar_t* pattern, std::vector<SlotSave>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        int slot = SlotOf(fd.cFileName);
        if (slot < 1 || slot > 8) continue;
        // the same slot in several folders (more than one Steam user): keep the newest
        bool replaced = false;
        for (auto& s : out)
            if (s.slot == slot) {
                if (CompareFileTime(&fd.ftLastWriteTime, &s.written) > 0) { s.sv.file = dir + L"\\" + fd.cFileName; s.written = fd.ftLastWriteTime; }
                replaced = true;
            }
        if (!replaced) { SlotSave s; s.slot = slot; s.sv.file = dir + L"\\" + fd.cFileName; s.written = fd.ftLastWriteTime; out.push_back(s); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void ReadSaveValues(SlotSave& s) {
    SaveValues& sv = s.sv;
    sv.ok = false;
    HANDLE h = CreateFileW(sv.file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(h, nullptr), got = 0;
    std::string text(size, '\0');
    ReadFile(h, text.data(), size, &got, nullptr);
    CloseHandle(h);
    text.resize(got);
    size_t ppd = text.find("\"persistentProfileData\"");
    if (ppd == std::string::npos) return;
    long long m = 0, x = 0, r = 0;
    sv.ok = ParseNumberAfter(text, ppd, "\"money\"", m) && ParseNumberAfter(text, ppd, "\"experience\"", x);
    ParseNumberAfter(text, ppd, "\"rank\"", r);
    sv.money = (double)m; sv.xp = (double)x; sv.rank = (double)r;
    FILETIME created, exitT, kern, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exitT, &kern, &user))
        sv.savedThisSession = CompareFileTime(&s.written, &created) > 0;
}

static std::vector<SlotSave> ReadAllSlots(bool parse = true) {
    std::vector<SlotSave> out;
    wchar_t steam[MAX_PATH] = {}; DWORD cb = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &cb) == ERROR_SUCCESS) {
        std::wstring s(steam);
        std::replace(s.begin(), s.end(), L'/', L'\\');
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((s + L"\\userdata\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                    CollectSlots(s + L"\\userdata\\" + fd.cFileName + L"\\1465360\\remote", L"CompleteSave*.cfg", out);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    wchar_t prof[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH)) {
        std::wstring storage = std::wstring(prof) + L"\\Documents\\My Games\\SnowRunner\\base\\storage";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((storage + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                    CollectSlots(storage + L"\\" + fd.cFileName, L"CompleteSave*.dat", out);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (!parse) return out;   // file times only
    for (auto& s : out) ReadSaveValues(s);
    out.erase(std::remove_if(out.begin(), out.end(), [](const SlotSave& s) { return !s.sv.ok; }), out.end());
    std::sort(out.begin(), out.end(), [](const SlotSave& a, const SlotSave& b) { return a.slot < b.slot; });
    return out;
}

static std::string SlotList(const std::vector<SlotSave>& slots) {
    std::string s;
    char b[96];
    for (auto& x : slots) { sprintf_s(b, "%sslot %d: $%.0f xp %.0f rank %.0f", s.empty() ? "" : ", ", x.slot, x.sv.money, x.sv.xp, x.sv.rank); s += b; }
    return s.empty() ? "no readable saves" : s;
}

// ---- scanning -------------------------------------------------------------------
// Thread stacks hold short-lived copies (function arguments, locals); skip them.
static std::vector<uintptr_t> g_stackAllocs;

static void CollectStacks() {
    g_stackAllocs.clear();
    using NtQIT = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static auto q = (NtQIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    struct TBI { LONG ExitStatus; PVOID Teb; uintptr_t Cid[2]; uintptr_t Affinity; LONG Pri, BasePri; } tbi;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE || !q) return;
    THREADENTRY32 te{ sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (q(t, 0, &tbi, sizeof(tbi), nullptr) == 0 && tbi.Teb) {
            NT_TIB tib;
            MEMORY_BASIC_INFORMATION mbi;
            if (SafeCopy(&tib, tbi.Teb, sizeof(tib)) && VirtualQuery((uint8_t*)tib.StackBase - 1, &mbi, sizeof(mbi)))
                g_stackAllocs.push_back((uintptr_t)mbi.AllocationBase);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
}

static bool Scannable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    for (uintptr_t s : g_stackAllocs) if ((uintptr_t)mbi.AllocationBase == s) return false;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) return false;
    if (mbi.Type == MEM_IMAGE) {   // of the modules, only the game's own data
        uintptr_t b = (uintptr_t)mbi.BaseAddress;
        return b >= g_exeBase && b < g_exeEnd;
    }
    return mbi.Type == MEM_PRIVATE;
}

template <class F>
static void ForEachRegionWide(F f) {   // research only: everything writable except stacks and Gitmo itself
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t a = (uintptr_t)si.lpMinimumApplicationAddress, end = (uintptr_t)si.lpMaximumApplicationAddress;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&ForEachRegionWide<F>, &self);
    MEMORY_BASIC_INFORMATION mbi;
    while (a < end && VirtualQuery((void*)a, &mbi, sizeof(mbi))) {
        bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                  (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) && (HMODULE)mbi.AllocationBase != self;
        if (ok) f((uintptr_t)mbi.BaseAddress, mbi.RegionSize);   // (thread stacks too: the game's script engine may keep values there)
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
}

template <class F>
static void ForEachRegion(F f) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t a = (uintptr_t)si.lpMinimumApplicationAddress, end = (uintptr_t)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < end && VirtualQuery((void*)a, &mbi, sizeof(mbi))) {
        if (Scannable(mbi)) f((uintptr_t)mbi.BaseAddress, mbi.RegionSize);
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
}

// The game keeps the profile as a list of tagged 8-byte values (found 2026-11-08):
//   +0x00 money (int32)  +0x04 0x00010000 (integer tag)
//   +0x08 experience     +0x0C 0x00010000
//   +0x10 tagged pointer, top 16 bits 0x0020
//   +0x18 rank           +0x1C 0x00010000
// Old copies of the same record exist (from loading); the live one is the one the
// game changes. The money in the save may already be out of date, so records are
// matched on experience and rank, or on money when experience has moved on.
static const uint32_t kIntTag = 0x10000;

constexpr int32_t kMinMoney = -100000000;   // lowest balance still taken as a real one
static bool LooksLikeProfile(const uint8_t* p, int32_t xp, int32_t money, int32_t rank) {
    const int32_t* d = (const int32_t*)p;
    if ((uint32_t)d[1] != kIntTag || (uint32_t)d[3] != kIntTag || (uint32_t)d[7] != kIntTag) return false;
    if ((*(const uint64_t*)(p + 0x10) >> 48) != 0x20) return false;
    // money may be negative: the game can let a purchase take it below zero (2026-11-10 test: -102)
    if (d[0] < kMinMoney || d[6] != rank) return false;
    return d[2] == xp || (d[0] == money && d[2] > xp);
}

static bool StillProfile(uintptr_t a, int32_t minXp, int32_t& money) {
    uint8_t b[0x20];
    if (!SafeCopy(b, (void*)a, sizeof(b))) return false;
    const int32_t* d = (const int32_t*)b;
    if ((uint32_t)d[1] != kIntTag || (uint32_t)d[3] != kIntTag || (uint32_t)d[7] != kIntTag) return false;
    if ((*(const uint64_t*)(b + 0x10) >> 48) != 0x20 || d[0] < kMinMoney || d[2] < minXp) return false;
    money = d[0];
    return true;
}

static std::mutex g_scanMx;   // CollectStacks / g_stackAllocs are shared by the money scan and the probe

static std::vector<Candidate> ScanForMoney(const std::vector<SlotSave>& slots, size_t& rawHits, size_t& bytes) {
    std::vector<Candidate> out;
    rawHits = 0; bytes = 0;
    if (slots.empty()) return out;
    std::lock_guard<std::mutex> scanLock(g_scanMx);
    CollectStacks();
    const size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk + 0x20);
    ForEachRegion([&](uintptr_t base, size_t size) {
        for (size_t off = 0; off < size; off += kChunk) {
            size_t n = std::min(size - off, kChunk + 0x20);   // small overlap so a record on a chunk edge is seen
            if (!SafeCopy(buf.data(), (void*)(base + off), n)) continue;
            bytes += std::min(kChunk, size - off);
            size_t stop = std::min(n, kChunk);
            for (size_t i = 0; i + 0x20 <= n && i < stop; i += 8) {
                if (*(uint32_t*)&buf[i + 4] != kIntTag) continue;
                rawHits++;
                for (auto& sl : slots) {
                    const int32_t m = (int32_t)sl.sv.money, xp = (int32_t)sl.sv.xp, rk = (int32_t)sl.sv.rank;
                    if (!LooksLikeProfile(&buf[i], xp, m, rk)) continue;
                    Candidate c; c.addr = base + off + i; c.value = *(int32_t*)&buf[i]; c.xpOff = 8; c.rankOff = 0x18;
                    c.slot = sl.slot; c.minXp = xp;
                    if (out.size() < 32) out.push_back(c);
                    break;
                }
            }
        }
    });
    return out;
}

// ---- session research ---------------------------------------------------------------
// Co-op runs through Saber's own servers (Hydra), so Steam doesn't know who is in our game.
// The game itself must: look for the other players' ids (Steam id, Hydra id, name) in its
// memory every 30 s and log where they are, in the menu, together, and after leaving.
struct ProbeHit { uintptr_t addr, allocBase; size_t regionSize; };

static std::string HexAround(uintptr_t a, size_t before, size_t after) {
    std::vector<uint8_t> b(before + after);
    if (!SafeCopy(b.data(), (void*)(a - before), b.size())) return "(unreadable)";
    std::string s;
    char h[4];
    for (size_t i = 0; i < b.size(); i++) {
        if (i == before) s += "[";
        sprintf_s(h, "%02X", b[i]); s += h;
        if (i + 1 == before) s += "]"; else if (i + 1 < b.size()) s += " ";
    }
    std::string t;
    for (size_t i = 0; i < b.size(); i++) t += (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
    return s + "  |" + t + "|";
}

static void ProbeOnce(int round) {
    std::vector<ProbeTarget> t;
    Net_ProbeTargets(t);
    if (t.empty()) { if (round % 10 == 0) Log("probe: no other players to look for yet"); return; }
    uintptr_t sess = Game_SessionId();
    // first two bytes -> needles starting with them
    std::vector<std::vector<int>> first(65536);
    std::vector<uint8_t> any(65536, 0);
    size_t maxLen = 0;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i].bytes.size() < 4) continue;
        uint16_t k = (uint8_t)t[i].bytes[0] | ((uint8_t)t[i].bytes[1] << 8);
        first[k].push_back((int)i); any[k] = 1;
        maxLen = std::max(maxLen, t[i].bytes.size());
    }
    std::vector<size_t> count(t.size(), 0);
    std::vector<std::vector<ProbeHit>> hits(t.size());
    ULONGLONG t0 = GetTickCount64();
    size_t bytes = 0;
    {
        std::lock_guard<std::mutex> scanLock(g_scanMx);
        CollectStacks();
        const size_t kChunk = 1 << 20;
        std::vector<uint8_t> buf(kChunk + 256);
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t a = (uintptr_t)si.lpMinimumApplicationAddress, end = (uintptr_t)si.lpMaximumApplicationAddress;
        MEMORY_BASIC_INFORMATION mbi;
        while (a < end && VirtualQuery((void*)a, &mbi, sizeof(mbi))) {
            uintptr_t base = (uintptr_t)mbi.BaseAddress; size_t size = mbi.RegionSize;
            a = base + size;
            if (!Scannable(mbi)) continue;
            for (size_t off = 0; off < size; off += kChunk) {
                size_t n = std::min(size - off, kChunk + maxLen);
                if (!SafeCopy(buf.data(), (void*)(base + off), n)) continue;
                size_t stop = std::min(n, kChunk);
                bytes += stop;
                const uint8_t* p = buf.data();
                for (size_t i = 0; i + 1 < n && i < stop; i++) {
                    uint16_t k = p[i] | (p[i + 1] << 8);
                    if (!any[k]) continue;
                    for (int ni : first[k]) {
                        const std::string& nd = t[ni].bytes;
                        if (i + nd.size() > n || memcmp(p + i, nd.data(), nd.size()) != 0) continue;
                        uintptr_t at = base + off + i;
                        if (at == t[ni].steamCopy) continue;   // Steam's own copy (rich presence)
                        count[ni]++;
                        if (hits[ni].size() < 6) hits[ni].push_back({ at, (uintptr_t)mbi.AllocationBase, size });
                    }
                }
            }
        }
    }
    static std::map<std::string, size_t> last;
    static uintptr_t lastSess = ~(uintptr_t)0;
    bool changed = sess != lastSess;
    for (size_t i = 0; i < t.size(); i++) if (last[t[i].who + "|" + t[i].kind] != count[i]) changed = true;
    if (!changed) return;
    Log("probe: %s, scanned %zu MB in %llu ms", sess ? "a save is loaded" : "main menu", bytes >> 20, GetTickCount64() - t0);
    std::string who;
    std::string line;
    for (size_t i = 0; i <= t.size(); i++) {
        if (i == t.size() || t[i].who != who) {
            if (!line.empty()) Log("probe:   %s:%s", who.c_str(), line.c_str());
            if (i == t.size()) break;
            who = t[i].who; line.clear();
        }
        char b[64]; sprintf_s(b, " %s %zu", t[i].kind.c_str(), count[i]); line += b;
    }
    for (size_t i = 0; i < t.size(); i++) {
        size_t& l = last[t[i].who + "|" + t[i].kind];
        if (l != count[i] && t[i].kind != "name" && t[i].kind != "name-w") {   // where the ids are (names are too common)
            for (auto& h : hits[i])
                Log("probe:     %s %s at %p (block %p, %zu KB): %s", t[i].who.c_str(), t[i].kind.c_str(), (void*)h.addr, (void*)h.allocBase,
                    h.regionSize >> 10, HexAround(h.addr, 48, 64).c_str());
        }
        l = count[i];
    }
    lastSess = sess;
}

static DWORD WINAPI ProbeThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    Log("probe: session research on (SessionResearch=1): looking for other players' ids in game memory every 30 s");
    for (int round = 0;; round++) {
        Sleep(round == 0 ? 60000 : 30000);
        __try { ProbeOnce(round); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("probe: scan failed"); }
    }
    return 0;
}

void Probe_Start() {
    if (HANDLE t = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr)) CloseHandle(t);
}

// ---- research logging --------------------------------------------------------------
static std::string Where(uintptr_t a) {
    char b[64];
    if (a >= g_exeBase && a < g_exeEnd) sprintf_s(b, "exe+0x%llX", (unsigned long long)(a - g_exeBase));
    else sprintf_s(b, "%p", (void*)a);
    return b;
}

static void HexDump(uintptr_t a, size_t n) {
    std::vector<uint8_t> b(n);
    if (!SafeCopy(b.data(), (void*)a, n)) { Log("  (unreadable)"); return; }
    for (size_t i = 0; i < n; i += 16) {
        char line[128]; int p = sprintf_s(line, "  %+05X:", (int)i - (int)(n / 2));
        for (size_t j = 0; j < 16; j += 4) p += sprintf_s(line + p, sizeof(line) - p, " %08X", *(uint32_t*)&b[i + j]);
        uint64_t q0 = *(uint64_t*)&b[i], q1 = *(uint64_t*)&b[i + 8];
        std::string tag;
        if (q0 >= g_exeBase && q0 < g_exeEnd) tag += "  [+0]->" + Where(q0);
        if (q1 >= g_exeBase && q1 < g_exeEnd) tag += "  [+8]->" + Where(q1);
        Log("%s%s", line, tag.c_str());
    }
}

// Who points at the money's object? Level 1 anywhere, level 2 from the exe's globals.
static void PointerSearch(uintptr_t money) {
    const uintptr_t lo1 = money - 0x2000, hi1 = money;
    struct P { uintptr_t at, to; };
    std::vector<P> lvl1;
    std::vector<uint8_t> buf(1 << 20);
    auto scan = [&](uintptr_t lo, uintptr_t hi, bool exeOnly, std::vector<P>& out, size_t cap) {
        ForEachRegion([&](uintptr_t base, size_t size) {
            if (exeOnly && !(base >= g_exeBase && base < g_exeEnd)) return;
            for (size_t off = 0; off < size && out.size() < cap; off += buf.size()) {
                size_t n = std::min(buf.size(), size - off);
                if (!SafeCopy(buf.data(), (void*)(base + off), n)) continue;
                for (size_t i = 0; i + 8 <= n; i += 8) {
                    uint64_t v = *(uint64_t*)&buf[i];
                    if (v >= lo && v <= hi) { out.push_back({base + off + i, (uintptr_t)v}); if (out.size() >= cap) break; }
                }
            }
        });
    };
    scan(lo1, hi1, false, lvl1, 256);
    Log("research: %zu pointers into the 8 KB before the money:", lvl1.size());
    for (size_t i = 0; i < lvl1.size() && i < 40; i++)
        Log("  %s -> %p (money is +0x%llX from there)", Where(lvl1[i].at).c_str(), (void*)lvl1[i].to,
            (unsigned long long)(money - lvl1[i].to));
    int shown = 0;
    for (auto& p : lvl1) {
        if (p.at >= g_exeBase && p.at < g_exeEnd) continue;
        std::vector<P> lvl2;
        scan(p.at - 0x800, p.at, true, lvl2, 8);
        for (auto& q : lvl2) {
            Log("  GLOBAL %s -> %p, +0x%llX -> %p, +0x%llX = money", Where(q.at).c_str(), (void*)q.to,
                (unsigned long long)(p.at - q.to), (void*)p.to, (unsigned long long)(money - p.to));
            if (++shown >= 40) return;
        }
    }
    if (!shown) Log("  no 2-level path from the game's globals found");
}

static void DumpImage() {
    wchar_t name[64];
    auto nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)g_exeBase)->e_lfanew);
    swprintf_s(name, L"\\Gitmo-image-%08X.bin", nt->FileHeader.TimeDateStamp);
    std::wstring path = ModuleDir() + name;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) { Log("research: image dump already exists"); return; }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<uint8_t> page(0x1000);
    size_t ok = 0;
    for (uintptr_t a = g_exeBase; a < g_exeEnd; a += 0x1000) {
        if (!SafeCopy(page.data(), (void*)a, 0x1000)) std::fill(page.begin(), page.end(), 0); else ok++;
        DWORD w; WriteFile(h, page.data(), 0x1000, &w, nullptr);
    }
    CloseHandle(h);
    Log("research: wrote decrypted game image (%zu pages) to %ls", ok, name + 1);
}

// Hardware breakpoint: which game instructions touch the money (logging only).
static uintptr_t g_bpAddr;
static int g_bpSlot = -1;
struct Hit { std::atomic<uint64_t> rip{0}; std::atomic<uint32_t> count{0}; };
static Hit g_hits[64];
static PVOID g_veh;

static LONG CALLBACK BpHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || g_bpSlot < 0) return EXCEPTION_CONTINUE_SEARCH;
    if (!(e->ContextRecord->Dr6 & (1ull << g_bpSlot))) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t rip = e->ContextRecord->Rip;
    for (auto& h : g_hits) {
        uint64_t cur = h.rip.load();
        if (cur == rip) { h.count++; break; }
        if (cur == 0) {
            uint64_t zero = 0;
            if (h.rip.compare_exchange_strong(zero, rip) || zero == rip) { h.count++; break; }
        }
    }
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static bool g_bpWriteOnly = false;
static int SetBreakpointAllThreads(bool on) {
    int done = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{ sizeof(te) };
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                int s = g_bpSlot;
                (&c.Dr0)[s] = on ? g_bpAddr : 0;
                c.Dr7 &= ~((3ull << (s * 2)) | (0xFull << (16 + s * 4)));
                if (on) c.Dr7 |= (1ull << (s * 2)) | ((g_bpWriteOnly ? 1ull : 3ull) << (16 + s * 4)) | (3ull << (18 + s * 4)); // local, (read/)write, 4 bytes
                if (SetThreadContext(t, &c)) done++;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return done;
}

static void BreakpointResearch(uintptr_t addr, int seconds) {
    // pick a debug register nobody uses (checked on this thread's view of Dr7)
    CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(GetCurrentThread(), &c);
    for (int s = 0; s < 4; s++) if (!(c.Dr7 & (3ull << (s * 2)))) { g_bpSlot = s; break; }
    if (g_bpSlot < 0) { Log("research: all debug registers busy, skipping code trace"); return; }
    g_bpAddr = addr;
    g_veh = AddVectoredExceptionHandler(1, BpHandler);
    Log("research: tracing which game code touches the money for %d s (slot DR%d). Earning or spending money now helps.", seconds, g_bpSlot);
    for (int t = 0; t < seconds; t += 2) {   // re-apply so new threads are covered
        SetBreakpointAllThreads(true);
        Sleep(2000);
    }
    SetBreakpointAllThreads(false);
    Sleep(200);
    RemoveVectoredExceptionHandler(g_veh);
    int n = 0;
    for (auto& h : g_hits) {
        uint64_t rip = h.rip.load();
        if (!rip) continue;
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)rip, &mod);
        if (mod && mod != (HMODULE)g_exeBase) {
            wchar_t mn[MAX_PATH] = L"?"; GetModuleFileNameW(mod, mn, MAX_PATH);
            Log("  (other module %ls, hits %u - Gitmo's own reads or another mod)", wcsrchr(mn, L'\\') ? wcsrchr(mn, L'\\') + 1 : mn, h.count.load());
            continue;
        }
        n++;
        uint8_t code[64] = {};
        SafeCopy(code, (void*)(rip - 32), 64);
        char hex[200]; int p = 0;
        for (int i = 0; i < 64; i++) p += sprintf_s(hex + p, sizeof(hex) - p, i == 32 ? "|%02X" : "%02X", code[i]);
        Log("  code %s  hits %u  bytes[-32..+32] %s", Where(rip).c_str(), h.count.load(), hex);
    }
    Log("research: code trace done, %d places", n);
}

// ---- money research: who changes the money, with the whole call stack ----------------------
// MoneyResearch=1: a write-only hardware breakpoint on the live balance. Every time the game
// writes it, the handler unwinds the writing thread's stack (exe frames only) and counts each
// distinct stack. The point: find the game's own "change money" routine, the one that also
// tells the HUD / garage screens to redraw.
struct StackHit {
    std::atomic<uint64_t> key{ 0 };
    std::atomic<uint32_t> count{ 0 };
    std::atomic<int> ready{ 0 };
    uint64_t frames[24];
    int n;
    int32_t before, after;
};
static StackHit g_stacks[48];
static std::atomic<int32_t> g_watchLast{ 0 };

static int GuardedUnwind(CONTEXT* ctx, uint64_t* frames, int max) {
    int n = 0;
    __try {
        for (int i = 0; i < 40 && n < max; i++) {
            uint64_t rip = ctx->Rip;
            if (rip < 0x10000) break;
            if (rip >= g_exeBase && rip < g_exeEnd) frames[n++] = rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(rip, &imageBase, nullptr);
            if (f) {
                PVOID hd; DWORD64 ef;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, rip, f, ctx, &hd, &ef, nullptr);
            } else {
                ctx->Rip = *(DWORD64*)ctx->Rsp;
                ctx->Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

static LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || g_bpSlot < 0) return EXCEPTION_CONTINUE_SEARCH;
    if (!(e->ContextRecord->Dr6 & (1ull << g_bpSlot))) return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Dr6 = 0;
    uint64_t rip = e->ContextRecord->Rip;
    if (rip < g_exeBase || rip >= g_exeEnd) return EXCEPTION_CONTINUE_EXECUTION;   // Gitmo's own writes
    int32_t now = 0;
    SafeCopy(&now, (void*)g_bpAddr, 4);
    int32_t before = g_watchLast.exchange(now);
    CONTEXT c = *e->ContextRecord;
    uint64_t fr[24];
    int n = GuardedUnwind(&c, fr, 24);
    uint64_t key = 1469598103934665603ull;
    for (int i = 0; i < n; i++) key = (key ^ (fr[i] - g_exeBase)) * 1099511628211ull;
    if (!key) key = 1;
    for (auto& h : g_stacks) {
        uint64_t cur = h.key.load();
        if (cur == key) { h.count++; h.before = before; h.after = now; break; }
        if (cur == 0) {
            uint64_t zero = 0;
            if (h.key.compare_exchange_strong(zero, key)) {
                memcpy(h.frames, fr, n * sizeof(uint64_t)); h.n = n; h.before = before; h.after = now;
                h.count++; h.ready = 1;
                break;
            }
            if (zero == key) { h.count++; break; }
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI MoneyWatchThread(LPVOID) {
    CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(GetCurrentThread(), &c);
    for (int s = 0; s < 4; s++) if (!(c.Dr7 & (3ull << (s * 2)))) { g_bpSlot = s; break; }
    if (g_bpSlot < 0) { Log("money research: all debug registers busy -> off"); return 0; }
    g_bpWriteOnly = true;
    AddVectoredExceptionHandler(1, WatchHandler);
    Log("money research on (MoneyResearch=1): once the balance is found, every change the game makes is traced (DR%d)", g_bpSlot);
    uint32_t logged[48] = {};
    for (;;) {
        Sleep(3000);
        uintptr_t want = 0;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (g_st.live && g_st.sure && g_st.selected < (int)g_st.cands.size()) want = g_st.cands[g_st.selected].addr;
        }
        if (want != g_bpAddr) {
            if (g_bpAddr) SetBreakpointAllThreads(false);
            g_bpAddr = want;
            if (want) { int32_t v = 0; SafeCopy(&v, (void*)want, 4); g_watchLast = v; Log("money research: watching %s", Where(want).c_str()); }
        }
        if (g_bpAddr) SetBreakpointAllThreads(true);   // again each time: new threads
        for (int i = 0; i < 48; i++) {
            StackHit& h = g_stacks[i];
            if (!h.ready.load()) continue;
            uint32_t cnt = h.count.load();
            if (cnt == logged[i]) continue;
            if (logged[i]) { Log("money research: stack #%d again (%u times now; last %d -> %d)", i, cnt, h.before, h.after); logged[i] = cnt; continue; }
            logged[i] = cnt;
            std::string s;
            char b[48];
            for (int k = 0; k < h.n; k++) { sprintf_s(b, "%s0x%llX", k ? " < " : "", (unsigned long long)(h.frames[k] - g_exeBase)); s += b; }
            Log("money research: NEW stack #%d (money %d -> %d): %s", i, h.before, h.after, s.c_str());
        }
    }
}

void MoneyWatch_Start() {
    if (HANDLE t = CreateThread(nullptr, 0, MoneyWatchThread, nullptr, 0, nullptr)) CloseHandle(t);
}

// ---- main loop ------------------------------------------------------------------
static void SetStatus(MoneyState::Phase ph, const char* fmt, ...) {
    char b[256]; va_list a; va_start(a, fmt); vsprintf_s(b, fmt, a); va_end(a);
    std::lock_guard<std::mutex> lk(g_mx);
    g_st.phase = ph; g_st.status = b;
}


// ---- activity history -----------------------------------------------------------------
// Kept per save slot (switching saves shows that save's own list) and saved to
// Gitmo-activity.txt next to the DLL, so it survives restarts: newest first, 300 per slot.
static std::map<int, std::vector<Activity>> g_actBySlot;   // guarded by g_mx
static int g_actSlot = 0;
static std::atomic<bool> g_actDirty{false};
static const size_t kActivityKeep = 300;

static void AddActivity(const Activity& a) {   // call with g_mx held
    g_st.activity.insert(g_st.activity.begin(), a);
    if (g_st.activity.size() > kActivityKeep) g_st.activity.pop_back();
    g_actDirty = true;
}

static std::wstring ActivityPath() { return ModuleDir() + L"\\Gitmo-activity.txt"; }
static std::string Field(std::string v) {
    for (char& c : v) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return v;
}

static void SaveActivity() {
    if (!g_actDirty.exchange(false)) return;
    std::map<int, std::vector<Activity>> all;
    { std::lock_guard<std::mutex> lk(g_mx); all = g_actBySlot; all[g_actSlot] = g_st.activity; }
    std::string out = "# Gitmo activity history: slot, date, time, change, balance, amount, label, status\n";
    char line[512];
    for (auto& kv : all)
        for (auto& a : kv.second) {
            sprintf_s(line, "%d\t%04u-%02u-%02u\t%02u:%02u:%02u\t%lld\t%lld\t%lld\t%s\t%s\n", kv.first, a.when.wYear, a.when.wMonth, a.when.wDay,
                      a.when.wHour, a.when.wMinute, a.when.wSecond, a.delta, a.balance, a.amount, Field(a.label).c_str(), Field(a.status).c_str());
            out += line;
        }
    std::wstring path = ActivityPath(), tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { g_actDirty = true; return; }
    DWORD w = 0;
    BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) g_actDirty = true;   // try again later
}

static void LoadActivity() {
    HANDLE h = CreateFileW(ActivityPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(h, nullptr), got = 0;
    std::string text(size, '\0');
    ReadFile(h, text.data(), size, &got, nullptr);
    CloseHandle(h);
    text.resize(got);
    size_t pos = 0, n = 0;
    std::lock_guard<std::mutex> lk(g_mx);
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        std::string ln = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() : e + 1;
        if (ln.empty() || ln[0] == '#') continue;
        std::vector<std::string> f;
        size_t a = 0;
        for (;;) { size_t t = ln.find('\t', a); f.push_back(ln.substr(a, t == std::string::npos ? std::string::npos : t - a)); if (t == std::string::npos) break; a = t + 1; }
        if (f.size() < 8) continue;
        Activity x;
        unsigned Y = 0, M = 0, D = 0, hh = 0, mm = 0, ss = 0;
        if (sscanf_s(f[1].c_str(), "%u-%u-%u", &Y, &M, &D) != 3 || sscanf_s(f[2].c_str(), "%u:%u:%u", &hh, &mm, &ss) != 3) continue;
        x.when.wYear = (WORD)Y; x.when.wMonth = (WORD)M; x.when.wDay = (WORD)D; x.when.wHour = (WORD)hh; x.when.wMinute = (WORD)mm; x.when.wSecond = (WORD)ss;
        x.delta = _atoi64(f[3].c_str()); x.balance = _atoi64(f[4].c_str()); x.amount = _atoi64(f[5].c_str());
        x.label = f[6]; x.status = f[7];
        auto& v = g_actBySlot[atoi(f[0].c_str())];
        if (v.size() < kActivityKeep) { v.push_back(x); n++; }
    }
    g_st.activity = g_actBySlot[g_actSlot];
    Log("money: activity history loaded (%zu entries)", n);
}

static void UseSlotActivity(int slot) {   // call with g_mx held
    if (slot == g_actSlot) return;
    g_actDirty = true;
    g_actBySlot[g_actSlot] = g_st.activity;
    g_st.activity = g_actBySlot[slot];
    g_actSlot = slot;
}

static DWORD WINAPI MoneyThread(LPVOID) {
    // Which game is loaded: the truck-control object exists only while a save is loaded
    // (0 in the main menu) and is new for every load, so its address marks one game session.
    // On every load all four save slots are matched, preferring records that weren't there
    // before the load (the main menu keeps the last slot's record around).
    std::set<uintptr_t> oldAddrs;          // profile records that existed before this game was loaded
    uintptr_t session = (uintptr_t)-1;
    ULONGLONG nextScan = 0, nextFileCheck = 0;
    int tries = 0, savedHint = 0;          // savedHint: a slot the game saved during this game
    bool menuScanDone = false;
    bool lostLive = false;                 // the record Gitmo followed vanished: leftover copies are old ones
    std::map<int, FILETIME> fileTimes;     // slot -> save file time when this game was loaded
    uintptr_t confirmed = 0;
    // Driving through a tunnel to another map (or region) unloads the game for 2-3 s, which
    // looks like a trip to the main menu. The money record survives it, so when the game is
    // back within kMapChangeMs Gitmo keeps following the record it was sure of, instead of
    // searching again (that search could only find "old" copies and guessed the wrong save).
    const ULONGLONG kMapChangeMs = 8000;
    Candidate mapRec;                      // the record followed when the game was left (addr 0 = none)
    int mapSlot = 0;
    SaveValues mapSave;
    ULONGLONG leftAt = 0;
    auto startResearch = [](uintptr_t a) {
        if (!g_cfg.research) return;
        static uintptr_t target;
        static bool done;
        if (done) return;
        done = true;
        target = a;
        HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            Log("research: live profile record at %s:", Where(target).c_str());
            HexDump(target - 0x100, 0x200);
            PointerSearch(target);
            DumpImage();
#ifdef GITMO_TEST
            BreakpointResearch(target, 8);
#else
            BreakpointResearch(target, 90);
#endif
            return 0;
        }, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    };
    for (;;) {
        Sleep(250);
        SaveActivity();
        ULONGLONG now = GetTickCount64();
        uintptr_t s = Game_SessionId();
        if (s != session) {
            bool toMenu = s == 0;
            std::lock_guard<std::mutex> lk(g_mx);
            if (toMenu && session && session != (uintptr_t)-1) {   // leaving a game: remember what was followed
                mapRec = Candidate();
                if (g_st.live && g_st.sure && g_st.slot && g_st.selected < (int)g_st.cands.size()) {
                    mapRec = g_st.cands[g_st.selected]; mapSlot = g_st.slot; mapSave = g_st.save;
                }
                leftAt = now;
            }
            if (!toMenu && session == 0 && mapRec.addr && now - leftAt < kMapChangeMs) {
                int32_t v;
                if (StillProfile(mapRec.addr, mapRec.minXp, v)) {   // a map change: same save, same record
                    Log("money: back in a map after %llu ms (map change) -> still following %s (balance %d, slot %d)",
                        now - leftAt, Where(mapRec.addr).c_str(), v, mapSlot);
                    for (auto& c : g_st.cands) oldAddrs.insert(c.addr);
                    mapRec.value = v;
                    g_st.cands.assign(1, mapRec); g_st.selected = 0;
                    g_st.live = true; g_st.sure = true; g_st.group = false; g_st.slot = mapSlot; g_st.save = mapSave;
                    g_st.phase = MoneyState::Found; g_st.balance = v;
                    g_st.status = "Balance found (save slot " + std::to_string(mapSlot) + ")";
                    UseSlotActivity(mapSlot);
                    lostLive = false;
                    session = s;
                    mapRec = Candidate();
                    continue;   // fileTimes and savedHint carry on from before the map change
                }
                Log("money: back in a map after %llu ms, but the record followed before is gone -> searching", now - leftAt);
            }
            if (!toMenu) mapRec = Candidate();
            if (session != (uintptr_t)-1) Log(toMenu ? "money: back in the main menu" : "money: a save was loaded -> looking for its balance");
            for (auto& c : g_st.cands) oldAddrs.insert(c.addr);   // the last game's records are old now
            lostLive = false;
            g_st.live = false; g_st.sure = false; g_st.group = false; g_st.cands.clear(); g_st.selected = 0; g_st.slot = 0;
            g_st.phase = MoneyState::Waiting;
            g_st.status = toMenu ? "Load a save to see your balance" : "Looking for your balance";
            session = s;
            nextScan = now + (toMenu ? 4000 : 1500);
            tries = 0; savedHint = 0; menuScanDone = false; fileTimes.clear();
            continue;
        }
        if (session == 0) {
            // Main menu: note which records already exist - they don't belong to the next game.
            // Repeated while the menu is up: right after start the game hasn't loaded the saves yet.
            if (now >= nextScan) {
                nextScan = now + 15000;
                std::vector<SlotSave> slots = ReadAllSlots();
                size_t raw, bytes;
                auto found = ScanForMoney(slots, raw, bytes);
                size_t before = oldAddrs.size();
                for (auto& c : found) oldAddrs.insert(c.addr);
                if (!menuScanDone || oldAddrs.size() != before)
                    Log("money: main menu: %zu existing profile record(s) noted; saves: %s", found.size(), SlotList(slots).c_str());
                menuScanDone = true;
            }
            continue;
        }
        bool live;
        { std::lock_guard<std::mutex> lk(g_mx); live = g_st.live && !g_st.cands.empty(); }
        if (live) {   // live tracking of every matching record
            {
                std::lock_guard<std::mutex> lk(g_mx);
                std::vector<Candidate> keep;
                int sel = -1, other = -1;
                long long selPrev = 0, otherPrev = 0;
                bool selMoved = false;
                for (size_t i = 0; i < g_st.cands.size(); i++) {
                    Candidate c = g_st.cands[i];
                    int32_t v;
                    // Still a profile record? If the game freed or reused it, it is dropped instead of showing junk.
                    if (!StillProfile(c.addr, c.minXp, v)) {
                        Log("money: record %s is gone (unloaded or reused)", Where(c.addr).c_str());
                        if ((int)i == g_st.selected) lostLive = true;
                        continue;
                    }
                    long long prev = c.value;
                    c.value = v;
                    keep.push_back(c);
                    int k = (int)keep.size() - 1;
                    if ((int)i == g_st.selected) { sel = k; if (v != prev) { selMoved = true; selPrev = prev; } }
                    else if (v != prev && other < 0) { other = k; otherPrev = prev; }
                }
                g_st.cands = keep;
                if (keep.empty()) {
                    g_st.live = false; g_st.sure = false; g_st.group = false; g_st.phase = MoneyState::Waiting; g_st.status = "Lost track of the balance, searching again";
                    Log("money: no profile records left -> searching again");
                    nextScan = now + 3000; tries = 0;
                    continue;
                }
                long long prevBal = g_st.balance;
                bool moved = selMoved;
                if (!selMoved && other >= 0) {
                    // The game changed a different copy: that one is the live balance.
                    Log("money: the game updates %s (save slot %d), not %s -> following it", Where(keep[other].addr).c_str(), keep[other].slot,
                        sel >= 0 ? Where(keep[sel].addr).c_str() : "(gone)");
                    sel = other; prevBal = otherPrev; moved = true;
                    g_st.status = "Balance found";
                    g_st.sure = true;   // the game itself wrote this copy
                    g_st.group = false;
                } else if (sel < 0) {
                    sel = 0; prevBal = keep[0].value;
                    // the live record is gone: what's left are older copies, which a transfer
                    // wouldn't really change - not certain until the game changes one of them
                    if (g_st.sure) Log("money: the followed record is gone -> balance not certain until the game changes a copy (no transfers meanwhile)");
                    g_st.sure = false; g_st.group = false;
                    g_st.status = "Checking your balance";
                    Log("money: following %s", Where(keep[0].addr).c_str());
                } else if (selMoved) {
                    prevBal = selPrev;
                    g_st.sure = true;
                    if (g_st.group) Log("money: the game updates %s -> following only that copy", Where(keep[sel].addr).c_str());
                    g_st.group = false;
                }
                if (g_st.sure && keep[sel].slot != g_st.slot) {   // now certain which save is loaded
                    int slot = keep[sel].slot;
                    uintptr_t a = keep[sel].addr;
                    keep.erase(std::remove_if(keep.begin(), keep.end(), [&](const Candidate& c) { return c.slot != slot; }), keep.end());
                    for (size_t i = 0; i < keep.size(); i++) if (keep[i].addr == a) sel = (int)i;
                    g_st.cands = keep;
                    g_st.slot = slot;
                    UseSlotActivity(slot);
                    Log("money: playing save slot %d", slot);
                }
                g_st.selected = sel;
                long long v = keep[sel].value;
                if (moved && v != prevBal) {
                    Activity a; GetLocalTime(&a.when); a.delta = v - prevBal; a.balance = v;
                    AddActivity(a);
                    Log("money: %lld -> %lld (%+lld) at %s", prevBal, v, a.delta, Where(keep[sel].addr).c_str());
                    if (!confirmed) { confirmed = keep[sel].addr; startResearch(confirmed); }
                }
                g_st.balance = v;
            }
            // The game saving a slot proves that slot is the one being played.
            if (now >= nextFileCheck) {
                nextFileCheck = now + 5000;
                int curSlot;
                { std::lock_guard<std::mutex> lk(g_mx); curSlot = g_st.slot; }
                for (auto& x : ReadAllSlots(false)) {
                    auto it = fileTimes.find(x.slot);
                    if (it == fileTimes.end() || CompareFileTime(&x.written, &it->second) == 0) continue;
                    it->second = x.written;
                    savedHint = x.slot;
                    if (x.slot != curSlot) {
                        Log("money: the game saved slot %d while Gitmo %s -> searching again", x.slot, curSlot ? ("followed slot " + std::to_string(curSlot)).c_str() : "wasn't sure which save is loaded");
                        g_rescan = true;
                    }
                }
            }
            if (!g_rescan) continue;
        }
        if (!g_rescan && now < nextScan) continue;
        g_rescan = false;
        std::vector<SlotSave> slots = ReadAllSlots();
        if (slots.empty()) {
            SetStatus(MoneyState::NotFound, "Couldn't read your save files");
            Log("money: no readable CompleteSave found");
            nextScan = now + 30000;
            continue;
        }
        if (fileTimes.empty()) for (auto& x : slots) fileTimes[x.slot] = x.written;
        SetStatus(MoneyState::Scanning, "Looking for your balance");
        ULONGLONG t0 = GetTickCount64();
        size_t raw, bytes;
        auto all = ScanForMoney(slots, raw, bytes);
        std::vector<Candidate> fresh;
        for (auto& c : all) if (!oldAddrs.count(c.addr)) fresh.push_back(c);
        Log("money: saves: %s; scanned %zu MB in %llu ms: %zu tagged values, %zu profile record(s), %zu new since the load",
            SlotList(slots).c_str(), bytes >> 20, GetTickCount64() - t0, raw, all.size(), fresh.size());
        for (size_t i = 0; i < all.size() && i < 16; i++)
            Log("  #%zu %s  slot %d  money %lld%s", i, Where(all[i].addr).c_str(), all[i].slot, all[i].value, oldAddrs.count(all[i].addr) ? "  (from before the load)" : "");
        std::vector<Candidate>& pool = fresh.empty() ? all : fresh;
        std::set<int> inPool;
        for (auto& c : pool) inPool.insert(c.slot);
        int slot = inPool.size() == 1 ? *inPool.begin() : (savedHint && inPool.count(savedHint)) ? savedHint : 0;
        std::vector<Candidate> cands;
        for (auto& c : pool) if (!slot || c.slot == slot) cands.push_back(c);
        std::lock_guard<std::mutex> lk(g_mx);
        g_st.scans++;
        if (cands.empty()) {
            tries++;
            g_st.cands.clear(); g_st.selected = 0; g_st.live = false; g_st.sure = false; g_st.group = false;
            g_st.phase = MoneyState::NotFound;
            g_st.status = "Balance not found yet";
            nextScan = now + (tries < 6 ? 5000 : 30000);
            continue;
        }
        tries = 0;
        const SlotSave* ss = nullptr;
        for (auto& x : slots) if (x.slot == slot) ss = &x;
        int pick = 0;
        bool sure = false;
        if (slot && cands.size() == 1) sure = true;
        else if (slot && ss) {
            // Several copies of the same save's record: if the game saved since it started, the
            // save was written from the live copy (the one that matches); if not, the old copies
            // still hold the saved amount, so the live one is the one that differs.
            int match = 0, differ = 0, firstMatch = -1, firstDiffer = -1;
            for (size_t i = 0; i < cands.size(); i++) {
                if (cands[i].value == (long long)ss->sv.money) { if (!match++) firstMatch = (int)i; }
                else if (!differ++) firstDiffer = (int)i;
            }
            if (ss->sv.savedThisSession && match == 1) { pick = firstMatch; sure = true; }
            else if (!ss->sv.savedThisSession && differ == 1) { pick = firstDiffer; sure = true; }
        }
        // All copies of the loaded save hold the same amount (a co-op guest: joining doesn't
        // create a fresh copy). The balance is certain; a transfer changes every copy.
        bool group = false;
        bool onlyOld = lostLive && fresh.empty();   // the live record was lost and nothing new turned up
        if (onlyOld) {
            sure = false;
            Log("money: only copies from before the load were found after the live record was lost -> not certain (no transfers until the game changes one)");
        }
        if (!fresh.empty()) lostLive = false;
        if (!sure && !onlyOld && slot && cands.size() > 1) {
            bool same = true;
            for (auto& c : cands) if (c.value != cands[0].value) same = false;
            if (same) { sure = true; group = true; pick = 0; }
        }
        g_st.cands = cands;
        g_st.selected = pick;
        g_st.live = true; g_st.phase = MoneyState::Found; g_st.balance = cands[pick].value;
        g_st.sure = sure;
        g_st.group = group;
        g_st.slot = slot;   // 0 = several saves still possible
        if (ss) g_st.save = ss->sv;
        if (slot) UseSlotActivity(slot);
        g_st.status = sure ? "Balance found (save slot " + std::to_string(slot) + ")"
                           : "Balance found (checking which copy is live)";
        Log("money: following #%d %s (balance %lld, slot %d, %s)", pick, Where(cands[pick].addr).c_str(), cands[pick].value, cands[pick].slot,
            group ? ("sure: " + std::to_string(cands.size()) + " identical copies, a transfer changes all of them").c_str() : sure ? "sure" : "until one changes");
    }
}

static void GarageResearch(long long oldV, long long newV, std::vector<uintptr_t> skip);   // below
static int GarageCopiesUpdate(long long bal, double xp, double rank);   // below
static int GarageBindingsUpdate(uintptr_t record, long long bal);      // below
void Money_Start() {
    LoadActivity();
#ifdef GITMO_TEST
    if (getenv("GITMO_TEST_GRES")) std::thread([] { Sleep(3000); GarageResearch(54321, 55321, {}); }).detach();   // the scan, on the harness's fake balance
    if (getenv("GITMO_TEST_GBIND")) std::thread([] {   // run-6 copy: planted record + copy pointing at it, and a decoy
        int32_t* rec = (int32_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 64);
        uint8_t* copy = (uint8_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 64);
        uint8_t* decoy = (uint8_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 64);
        uint64_t r = (uint64_t)rec, other = r + 0x1000; uint32_t eight = 8, x18 = 0x18; int32_t stale = 5500;
        memcpy(copy, &r, 8); memcpy(copy + 8, &eight, 4); memcpy(copy + 12, &x18, 4); memcpy(copy + 16, &stale, 4);
        memcpy(decoy, &other, 8); memcpy(decoy + 8, &eight, 4); memcpy(decoy + 12, &x18, 4); memcpy(decoy + 16, &stale, 4);
        Sleep(3000);
        int n = GarageBindingsUpdate((uintptr_t)rec, 500);
        int32_t c, d; memcpy(&c, copy + 16, 4); memcpy(&d, decoy + 16, 4);
        Log("test: run-6 copies found %d; planted copy now %d (want 500), decoy %d (want 5500)", n, c, d);
    }).detach();
    if (getenv("GITMO_TEST_GCOPY")) std::thread([] {   // step 2 on a planted triple (and a decoy with another rank)
        volatile double* copy = (volatile double*)HeapAlloc(GetProcessHeap(), 0, 64);   // ordinary heap memory, like the game's
        volatile double* decoy = (volatile double*)HeapAlloc(GetProcessHeap(), 0, 64);
        copy[0] = 5500; copy[1] = 17360; copy[2] = 12; decoy[0] = 5500; decoy[1] = 17360; decoy[2] = 11;
        Sleep(3000);
        int n = GarageCopiesUpdate(4500, 17360, 12);
        Log("test: garage copies pass returned %d; planted copy now %.0f (want 4500), decoy %.0f (want 5500)", n, copy[0], decoy[0]);
    }).detach();
#endif
    if (HANDLE t = CreateThread(nullptr, 0, MoneyThread, nullptr, 0, nullptr)) CloseHandle(t);
}

MoneyState Money_Snapshot() { std::lock_guard<std::mutex> lk(g_mx); return g_st; }
void Money_Rescan() { g_rescan = true; }
void Money_Select(int i) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (i >= 0 && i < (int)g_st.cands.size()) { g_st.selected = i; int32_t v; if (SafeRead32(g_st.cands[i].addr, v)) g_st.balance = v; }
}
bool Money_TestAdjust(long long delta) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_st.live || g_st.selected >= (int)g_st.cands.size()) return false;
    uintptr_t a = g_st.cands[g_st.selected].addr;
    int32_t v;
    if (!SafeRead32(a, v) || v + delta < 0) return false;
    bool ok = SafeWrite32(a, (int32_t)(v + delta));
    Log("developer test: wrote %lld to #%d %s -> %s", v + delta, g_st.selected, Where(a).c_str(), ok ? "ok" : "FAILED");
    return ok;
}

// ---- transfers ----------------------------------------------------------------------
// Only the copy the game is known to use may be changed: writing a stale copy would
// make money appear or vanish without the game noticing.
long long Money_Balance() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_st.live || !g_st.sure || g_st.selected >= (int)g_st.cands.size()) return -1;
    int32_t v;
    if (!StillProfile(g_st.cands[g_st.selected].addr, 0, v)) return -1;
    return v;
}

void Money_Note(const std::string& label, long long amount, const std::string& status) {
    std::lock_guard<std::mutex> lk(g_mx);
    Activity a; GetLocalTime(&a.when); a.delta = 0; a.balance = g_st.balance; a.label = label; a.status = status; a.amount = amount;
    AddActivity(a);
}


// ---- research: where a garage screen keeps its own copy of the balance (GarageResearch=1) ----
// The garage's screens read the balance once, when they open, and check prices against that
// copy - so money sent away there could still be spent (the 2026-11-10 duplication test).
// To find that copy: right after Gitmo changes the balance in a garage, every place in memory
// still holding the OLD amount (as a tagged game value, a plain number, a double, a float, or
// on-screen text) is noted and then watched for four minutes. Backing out of the screen and
// opening it again makes the game read the balance afresh: the places that then jump to the
// new amount are the garage's copy. Read-only: nothing is written.
namespace {
struct GHit { uintptr_t addr; int kind; long long num; std::string text; };
const char* kGKind[] = { "tagged", "int32", "double", "float", "text", "utf16" };
std::atomic<bool> g_gresBusy{false};
}   // namespace
std::atomic<int> g_garageResearchState{0};   // for the phone: 0 idle, 1 scanning, 2 watching
namespace {

std::string GText(const uint8_t* p, bool wide) {
    std::string s;
    for (int i = 0; i < 24; i++) {
        unsigned c = wide ? (unsigned)(p[i * 2] | (p[i * 2 + 1] << 8)) : p[i];
        if (!c) break;
        s += (c >= 32 && c < 127) ? (char)c : '.';
    }
    return s;
}
}   // namespace

static void GarageResearch(long long oldV, long long newV, std::vector<uintptr_t> skip) {
    Log("garage research: balance %lld -> %lld in a garage; looking for places still holding %lld", oldV, newV, oldV);
    // the amount as the game might write it on screen
    std::vector<std::string> ascii;
    std::vector<std::wstring> wide;
    {
        char d[32]; sprintf_s(d, "%lld", oldV < 0 ? -oldV : oldV);
        std::string digits = d, grouped[4];
        const char* seps[4] = { ",", " ", ".", "" };
        for (int k = 0; k < 4; k++) {
            std::string g;
            for (size_t i = 0; i < digits.size(); i++) { if (i && (digits.size() - i) % 3 == 0) g += seps[k]; g += digits[i]; }
            grouped[k] = g;
        }
        for (auto& g : grouped) ascii.push_back(g);
        for (auto& g : grouped) wide.push_back(std::wstring(g.begin(), g.end()));
        for (wchar_t sp : { (wchar_t)0x00A0, (wchar_t)0x202F, (wchar_t)0x2009 }) {
            std::wstring g;
            for (size_t i = 0; i < digits.size(); i++) { if (i && (digits.size() - i) % 3 == 0) g += sp; g += (wchar_t)digits[i]; }
            wide.push_back(g);
        }
        std::sort(ascii.begin(), ascii.end()); ascii.erase(std::unique(ascii.begin(), ascii.end()), ascii.end());
        std::sort(wide.begin(), wide.end()); wide.erase(std::unique(wide.begin(), wide.end()), wide.end());
    }
    auto skipped = [&](uintptr_t a) { for (uintptr_t s : skip) if (a >= s && a < s + 4) return true; return false; };   // just the balance Gitmo writes
    std::vector<GHit> hits;
    size_t counts[6] = {};
    const size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk + 64);
    ULONGLONG t0 = GetTickCount64();
    const int32_t v32 = (int32_t)oldV; const double vd = (double)oldV; const float vf = (float)oldV;
    std::unique_lock<std::mutex> scanLock(g_scanMx);   // CollectStacks / g_stackAllocs are shared with the balance search
    CollectStacks();
    ForEachRegionWide([&](uintptr_t base, size_t size) {
        for (size_t off = 0; off < size; off += kChunk) {
            size_t n = std::min(size - off, kChunk + 64);
            if (!SafeCopy(buf.data(), (void*)(base + off), n)) continue;
            size_t stop = std::min(n, kChunk);
            const uint8_t* b = buf.data();
            auto add = [&](size_t i, int kind, long long num, std::string text) {
                uintptr_t a = base + off + i;
                if (skipped(a)) return;
                counts[kind]++;
                if (hits.size() < 80000) hits.push_back({ a, kind, num, std::move(text) });
            };
            for (size_t i = 0; i + 8 <= stop; i += 4) {
                int32_t x; memcpy(&x, b + i, 4);
                if (x == v32) add(i, ((i & 7) == 0 && *(const uint32_t*)(b + i + 4) == kIntTag) ? 0 : 1, oldV, "");
                float f; memcpy(&f, b + i, 4);
                if (f == vf && x != v32) add(i, 3, oldV, "");
                if ((i & 7) == 0) { double dd; memcpy(&dd, b + i, 8); if (dd == vd) add(i, 2, oldV, ""); }
            }
            for (auto& t : ascii) {
                for (const uint8_t* p = b; (p = std::search(p, b + stop, t.begin(), t.end())) < b + stop; p += t.size()) {
                    if (p > b && p[-1] >= '0' && p[-1] <= '9') continue;          // part of a longer number
                    if (p + t.size() < b + n && p[t.size()] >= '0' && p[t.size()] <= '9') continue;
                    if (p - b < 6) continue;
                    add(p - b, 4, 0, GText(p - 6, false));   // with 6 characters before it
                }
            }
            for (auto& t : wide) {
                const uint8_t* tb = (const uint8_t*)t.data(); size_t tl = t.size() * 2;
                for (const uint8_t* p = b; (p = std::search(p, b + stop, tb, tb + tl)) < b + stop; p += 2) {
                    if ((p - b) & 1) continue;
                    if (p - b < 12) continue;
                    add(p - b, 5, 0, GText(p - 12, true));   // with 6 characters before it
                }
            }
        }
    });
    scanLock.unlock();
    Log("garage research: scanned in %llu ms - tagged %zu, int32 %zu, double %zu, float %zu, text %zu, utf16 %zu (watching %zu for 4 minutes)",
        GetTickCount64() - t0, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], hits.size());
    for (size_t i = 0; i < hits.size(); i++) {   // the strong candidates, with their surroundings (for a fix later)
        if (hits[i].kind == 0 || hits[i].kind == 2) Log("  #%zu %s %s  %s", i, kGKind[hits[i].kind], Where(hits[i].addr).c_str(), HexAround(hits[i].addr, 0x20, 0x28).c_str());
        else if (hits[i].kind >= 4 && i < 600) Log("  #%zu %s %s \"%s\"", i, kGKind[hits[i].kind], Where(hits[i].addr).c_str(), hits[i].text.c_str());
    }
    auto read = [&](const GHit& h, long long& nv, std::string& t) -> bool {
        uint8_t cur[64] = {};
        if (!SafeCopy(cur, (void*)(h.kind == 4 ? h.addr - 6 : h.kind == 5 ? h.addr - 12 : h.addr), 64)) return false;   // same context as noted
        if (h.kind >= 4) { t = GText(cur, h.kind == 5); return true; }
        if (h.kind <= 1) { int32_t x; memcpy(&x, cur, 4); nv = x; }
        else if (h.kind == 2) { double dd; memcpy(&dd, cur, 8); nv = (dd > -1e12 && dd < 1e12) ? (long long)dd : LLONG_MIN; }
        else { float f; memcpy(&f, cur, 4); nv = (f > -1e9f && f < 1e9f) ? (long long)f : LLONG_MIN; }
        return true;
    };
    // Settle: for 3 s, anything that changes on its own (counters, timers, scratch memory) isn't
    // a cached balance - it's dropped, so the changes that matter aren't drowned out.
    std::vector<char> alive(hits.size(), 1);
    size_t dropped = 0;
    for (int k = 0; k < 10; k++) {
        Sleep(300);
        for (size_t i = 0; i < hits.size(); i++) {
            if (!alive[i]) continue;
            long long nv = 0; std::string t;
            if (!read(hits[i], nv, t) || (hits[i].kind >= 4 ? t != hits[i].text : nv != hits[i].num)) { alive[i] = 0; dropped++; }
        }
    }
    Log("garage research: %zu stay still, %zu changed on their own and are ignored", hits.size() - dropped, dropped);
    Log("garage research: now buy something on this same screen, back out of the screen, then tap +1,000 (the marker)");
    g_garageResearchState = 2;
    // Watch: a spot that jumps to the NEW balance (the screen read it afresh) and that drops by
    // the same amount as the real balance when something is bought is the garage's copy.
    struct Ev { ULONGLONG t; long long d; };
    std::vector<Ev> real;                       // the game changing the real balance (purchases)
    std::vector<std::vector<Ev>> moves(hits.size());
    std::vector<char> toNew(hits.size(), 0);
    long long lastBal;
    { std::lock_guard<std::mutex> lk(g_mx); lastBal = g_st.balance; }
    int logged = 0;
    ULONGLONG end = GetTickCount64() + 240000;
    while (GetTickCount64() < end) {
        Sleep(300);
        ULONGLONG now = GetTickCount64();
        long long bal;
        { std::lock_guard<std::mutex> lk(g_mx); bal = g_st.balance; }
        if (bal != lastBal) { real.push_back({ now, bal - lastBal }); Log("garage research: your real balance %lld -> %lld (%+lld)", lastBal, bal, bal - lastBal); lastBal = bal; }
        for (size_t i = 0; i < hits.size(); i++) {
            if (!alive[i]) continue;
            GHit& h = hits[i];
            long long nv = 0; std::string t;
            if (!read(h, nv, t)) continue;
            if (h.kind >= 4) {
                if (t == h.text) continue;
                if (logged++ < 300) Log("garage research: #%zu %s %s: \"%s\" -> \"%s\"", i, kGKind[h.kind], Where(h.addr).c_str(), h.text.c_str(), t.c_str());
                h.text = t;
                continue;
            }
            if (nv == h.num) continue;
            moves[i].push_back({ now, nv - h.num });
            if (nv == newV) toNew[i] = 1;
            bool strong = h.kind == 0 || h.kind == 2;
            if ((strong || nv == newV) && logged++ < 300)
                Log("garage research: #%zu %s %s: %lld -> %lld%s", i, kGKind[h.kind], Where(h.addr).c_str(), h.num, nv, nv == newV ? "   <== now the NEW balance" : "");
            h.num = nv;
        }
    }
    // Summary: which spots did both
    int likely = 0;
    for (size_t i = 0; i < hits.size(); i++) {
        if (!alive[i] || moves[i].empty()) continue;
        bool withBuy = false;
        for (auto& m : moves[i]) for (auto& r : real)
            if (r.d < 0 && m.d == r.d && (m.t > r.t ? m.t - r.t : r.t - m.t) < 3000) withBuy = true;
        if (!toNew[i] && !withBuy) continue;
        Log("garage research: %s #%zu %s %s (now %lld)%s%s  %s", withBuy ? "LIKELY the garage's copy:" : "partly:", i, kGKind[hits[i].kind],
            Where(hits[i].addr).c_str(), hits[i].num, toNew[i] ? " [took the new balance]" : "", withBuy ? " [moved with a purchase]" : "",
            HexAround(hits[i].addr, 0x20, 0x28).c_str());
        if (withBuy) likely++;
    }
    for (auto& r : real) {
        if (r.d >= 0) continue;
        int listed = 0;
        Log("garage research: around the purchase of %lld at +%llu ms: everything that held the old balance and changed within 3 s:", -r.d, r.t - (end - 240000));
        for (size_t i = 0; i < hits.size() && listed < 120; i++) {
            if (!alive[i]) continue;
            for (auto& m : moves[i]) {
                if ((m.t > r.t ? m.t - r.t : r.t - m.t) > 3000) continue;
                Log("  #%zu %s %s: moved %+lld, now %lld   %s", i, kGKind[hits[i].kind], Where(hits[i].addr).c_str(), m.d, hits[i].num, HexAround(hits[i].addr, 0x10, 0x18).c_str());
                listed++;
                break;
            }
        }
        if (!listed) Log("  (nothing)");
    }
    {
        ULONGLONG buyT = 0, markT = 0;
        for (auto& r : real) { if (r.d < 0 && !buyT) buyT = r.t; else if (r.d > 0 && buyT && !markT) markT = r.t; }
        if (buyT && markT) {
            Log("garage research: what held the old balance, stayed put through the purchase, and changed before the marker (%llu s later) - the screen closing:", (markT - buyT) / 1000);
            int listed = 0, skippedN = 0;
            for (int pass = 0; pass < 2; pass++)   // tagged / double / float / text first, then plain numbers
                for (size_t i = 0; i < hits.size(); i++) {
                    if (!alive[i] || moves[i].empty()) continue;
                    bool strong = hits[i].kind != 1;
                    if ((pass == 0) != strong) continue;
                    const Ev& first = moves[i][0];
                    if (first.t <= buyT + 3000 || first.t > markT + 1000) continue;
                    if (listed >= 250) { skippedN++; continue; }
                    long long was = hits[i].num; for (auto& m : moves[i]) was -= m.d;
                    Log("  #%zu %s %s: %lld -> %+lld at +%llu ms after the purchase, now %lld   %s", i, kGKind[hits[i].kind], Where(hits[i].addr).c_str(),
                        was, first.d, first.t - buyT, hits[i].num, HexAround(hits[i].addr, 0x18, 0x20).c_str());
                    listed++;
                }
            Log("garage research: %d listed%s", listed, skippedN ? (", " + std::to_string(skippedN) + " more not listed").c_str() : "");
        } else Log("garage research: no purchase followed by a +1,000 marker - nothing to compare");
    }
    Log("garage research: finished - %d likely spot(s), %zu purchase(s) seen", likely, real.size());
    g_garageResearchState = 0;
    g_gresBusy = false;
}


// ---- the garage screens' copy of the balance (research step 2: GarageResearch=2) ------------
// Research runs 1-3 (2026-11-10) found the garage screens hold the balance as three doubles
// side by side - money, experience, rank (what the scripts' SetMoneyAndRankData passes) - read
// when the screen opens, and checked against when you buy. After Gitmo changes the balance in a
// garage, every such triple with this profile's experience and rank gets the current balance.
// One worker, so quick changes can't leave an in-between amount behind: it always writes the
// latest balance.
static bool SafeWriteBytes(uintptr_t a, const void* src, size_t n) {
    __try { memcpy((void*)a, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::atomic<int> g_garageFixCount{-1};   // phone: copies updated by the last pass (-1 none yet)
namespace { std::atomic<bool> g_gcPending{false}, g_gcRunning{false}; }

static int GarageCopiesUpdate(long long bal, double xp, double rank) {
    std::vector<uintptr_t> found;
    const size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk + 32);
    ULONGLONG t0 = GetTickCount64();
    ForEachRegion([&](uintptr_t base, size_t size) {
        for (size_t off = 0; off < size; off += kChunk) {
            size_t n = std::min(size - off, kChunk + 32);
            if (!SafeCopy(buf.data(), (void*)(base + off), n)) continue;
            size_t stop = std::min(n, kChunk);
            for (size_t i = 8; i + 16 <= n && i < stop + 8; i += 8) {   // i = the experience; money just before, rank just after
                double x, r, m;
                memcpy(&x, buf.data() + i, 8);
                if (x != xp) continue;
                memcpy(&r, buf.data() + i + 8, 8); memcpy(&m, buf.data() + i - 8, 8);
                if (r != rank || !(m > -1e8 && m < 2147483648.0) || m != (double)(long long)m) continue;
                if (found.size() < 64) found.push_back(base + off + i - 8);
            }
        }
    });
    int written = 0, already = 0;
    double nv = (double)bal;
    for (uintptr_t a : found) {
        double cur[3];
        if (!SafeCopy(cur, (void*)a, 24) || cur[1] != xp || cur[2] != rank) continue;
        if (cur[0] == nv) { already++; continue; }
        if (SafeWriteBytes(a, &nv, 8)) { written++; Log("garage copies: %s %.0f -> %lld", Where(a).c_str(), cur[0], bal); }
    }
    Log("garage copies: %zu money/xp/rank group(s) for xp %.0f rank %.0f: %d updated to %lld, %d already right (%llu ms)",
        found.size(), xp, rank, written, bal, already, GetTickCount64() - t0);
    return written + already;
}


// Research run 6 (2026-11-10) found the copy the garage screens check prices against: a small
// record that points at the live balance and keeps its own copy of it, refreshed only when a
// screen opens:
//   -0x10  pointer to the live profile record (the balance Gitmo changes)
//   -0x08  8, 0x18 (two int32: where experience and rank sit in that record)
//   +0x00  the balance as the screen last read it (int32)
// Writing the current balance there keeps the screen honest. Found by the pointer, so only
// records that belong to this exact balance are touched.
static int GarageBindingsUpdate(uintptr_t record, long long bal) {
    std::vector<uintptr_t> found;
    const size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk + 32);
    ULONGLONG t0 = GetTickCount64();
    ForEachRegion([&](uintptr_t base, size_t size) {
        for (size_t off = 0; off < size; off += kChunk) {
            size_t n = std::min(size - off, kChunk + 32);
            if (!SafeCopy(buf.data(), (void*)(base + off), n)) continue;
            size_t stop = std::min(n, kChunk);
            for (size_t i = 0; i + 24 <= n && i < stop; i += 8) {
                uint64_t ptr; memcpy(&ptr, buf.data() + i, 8);
                if (ptr != record) continue;
                uint32_t a, b; memcpy(&a, buf.data() + i + 8, 4); memcpy(&b, buf.data() + i + 12, 4);
                if (a != 8 || b != 0x18) continue;
                uintptr_t at = base + off + i + 16;
                if (at == record) continue;
                if (found.size() < 64) found.push_back(at);
            }
        }
    });
    int written = 0, already = 0;
    for (uintptr_t a : found) {
        int32_t cur;
        if (!SafeRead32(a, cur)) continue;
        if (cur == (int32_t)bal) { already++; continue; }
        if (SafeWrite32(a, (int32_t)bal)) { written++; Log("garage copies: %s %d -> %lld   %s", Where(a).c_str(), cur, bal, HexAround(a, 0x18, 0x18).c_str()); }
    }
    Log("garage copies: %zu screen cop%s of the balance (pointing at %s): %d updated to %lld, %d already right (%llu ms)",
        found.size(), found.size() == 1 ? "y" : "ies", Where(record).c_str(), written, bal, already, GetTickCount64() - t0);
    return (int)found.size();
}

static void GarageCopiesSoon() {   // called with g_mx held, after a change in a garage
    g_gcPending = true;
    if (g_gcRunning.exchange(true)) return;
    std::thread([] {
        while (g_gcPending.exchange(false)) {
            long long bal; double xp = -1, rank = -1; uintptr_t record = 0;
            {
                std::lock_guard<std::mutex> lk(g_mx);
                if (!g_st.live || g_st.selected >= (int)g_st.cands.size()) break;
                const Candidate& c = g_st.cands[g_st.selected];
                int32_t x, r;
                if (!SafeRead32(c.addr + c.xpOff, x) || !SafeRead32(c.addr + c.rankOff, r)) break;
                bal = g_st.balance; xp = x; rank = r; record = c.addr;
            }
            g_garageFixCount = GarageBindingsUpdate(record, bal);   // the copy the screens check (run 6)
        }
        g_gcRunning = false;
        if (g_gcPending) { std::lock_guard<std::mutex> lk(g_mx); GarageCopiesSoon(); }   // a change came in as it finished
    }).detach();
}

static void MaybeGarageResearch(long long oldV, long long newV) {   // called with g_mx held
    // (research step 2 - writing the balance into screen copies - is switched off for good: it didn't
    //  stop the duplication, and once wrote into memory the game had reused. The Esc does the job.)
    if (g_cfg.garageResearch >= 2) return;
    if (!g_cfg.garageResearch || Game_InGarage() != 1 || g_gresBusy.exchange(true)) return;
    std::vector<uintptr_t> skip;
    for (auto& c : g_st.cands) skip.push_back(c.addr);
    g_garageResearchState = 1;
    std::thread([oldV, newV, skip] { GarageResearch(oldV, newV, skip); }).detach();
}

bool Money_Apply(long long delta, const std::string& label) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_st.live || !g_st.sure || g_st.selected >= (int)g_st.cands.size()) { Log("transfer: balance not known for certain -> not changed"); return false; }
    Candidate& c = g_st.cands[g_st.selected];
    int32_t v;
    if (!StillProfile(c.addr, 0, v)) { Log("transfer: profile record gone -> not changed"); return false; }
    long long nv = (long long)v + delta;
    if ((delta < 0 && nv < 0) || nv < kMinMoney || nv > 2147483647LL) { Log("transfer: %lld %+lld is out of range -> not changed", (long long)v, delta); return false; }
    // Group mode: every copy must still hold the same amount, then all of them change together.
    std::vector<Candidate*> targets{ &c };
    if (g_st.group) {
        for (auto& o : g_st.cands) {
            if (&o == &c) continue;
            int32_t ov;
            if (!StillProfile(o.addr, 0, ov)) continue;   // gone: nothing to keep in step
            if (ov != v) { Log("transfer: the copies of your balance no longer agree -> not changed"); g_st.sure = false; g_st.group = false; return false; }
            targets.push_back(&o);
        }
    }
    for (Candidate* t : targets)
        if (!SafeWrite32(t->addr, (int32_t)nv)) { Log("transfer: write failed at %s", Where(t->addr).c_str()); return false; }
    // Tell the tracker this change is ours, so it shows up once, with its label.
    for (Candidate* t : targets) t->value = nv;
    if (targets.size() > 1) Log("transfer: changed all %zu copies of the balance", targets.size());
    g_st.balance = nv;
    Activity a; GetLocalTime(&a.when); a.delta = delta; a.balance = nv; a.label = label;
    AddActivity(a);
    Log("transfer: balance %d -> %lld (%+lld, %s)", v, nv, delta, label.c_str());
    // money left: an open game menu would still offer things at the old balance - Esc closes it
    if (delta < 0 && g_cfg.escapeOnSend) Input_PressEscape();
    MaybeGarageResearch(v, nv);
    return true;
}
