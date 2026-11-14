// Gitmo - a phone-style money app for SnowRunner.
// Stage one: version.dll proxy, in-game menu (F8), live balance, research logging.
//
// Loaded by SnowRunner as version.dll from its Bin folder. Every version.dll
// export is passed straight to C:\Windows\System32\version.dll (proxy.asm).
// Gitmo shares nothing with NoAutoReverse (dinput8.dll) or the VR mod (dxgi.dll).

#include "gitmo.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <string>
#include <vector>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#pragma comment(lib, "winmm.lib")

void Watchdog_Start();

extern "C" void* g_real[17] = {};
static const char* kExports[17] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW", "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW", "GetFileVersionInfoW",
    "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
    "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW" };

static HMODULE g_self;
uintptr_t g_exeBase, g_exeEnd;
Settings g_cfg;

// ---- log ------------------------------------------------------------------
static CRITICAL_SECTION g_logLock;
static FILE* g_log;
static size_t g_logBytes;
static bool g_logFull;
static const size_t kLogLimit = 5u * 1024 * 1024;

static bool GetModuleFilePath(HMODULE module, std::wstring& path) {
    path.assign(MAX_PATH, L'\0');
    for (;;) {
        DWORD cap = (DWORD)path.size();
        DWORD n = GetModuleFileNameW(module, &path[0], cap);
        if (!n) { path.clear(); return false; }
        if (n < cap) { path.resize(n); return true; }
        if (path.size() > (size_t)MAXDWORD / 2) {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            path.clear();
            return false;
        }
        path.resize(path.size() * 2);
    }
}

static bool GetSystemDirectoryPath(std::wstring& path) {
    path.assign(MAX_PATH, L'\0');
    for (;;) {
        if (path.size() > MAXDWORD) {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            path.clear();
            return false;
        }
        UINT cap = (UINT)path.size();
        UINT n = GetSystemDirectoryW(&path[0], cap);
        if (!n) { path.clear(); return false; }
        if (n < cap) { path.resize(n); return true; }
        if (n == MAXDWORD) {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            path.clear();
            return false;
        }
        path.resize((size_t)n + 1);
    }
}

// The folder this DLL is in (worked out once; paths longer than MAX_PATH are fine).
std::wstring ModuleDir() {
    static const std::wstring dir = [] {
        std::wstring p;
        if (!GetModuleFilePath(g_self, p)) {
            OutputDebugStringW(L"Gitmo: unable to get the DLL path.\n");
            return std::wstring();
        }
        size_t slash = p.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            OutputDebugStringW(L"Gitmo: DLL path has no directory separator.\n");
            return std::wstring();
        }
        p.resize(slash);
        return p;
    }();
    return dir;
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    if (g_logBytes < kLogLimit) {
        SYSTEMTIME t; GetLocalTime(&t);
        int n = fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list a; va_start(a, fmt);
        n += vfprintf(g_log, fmt, a);
        va_end(a);
        n += fprintf(g_log, "\n");
        fflush(g_log);
        g_logBytes += n > 0 ? n : 0;
    } else if (!g_logFull) {   // say so once, instead of going quiet without a word
        g_logFull = true;
        fprintf(g_log, "[log limit of 5 MB reached - nothing more is written this session]\n");
        fflush(g_log);
    }
    LeaveCriticalSection(&g_logLock);
}

void LogWatchdogReport(const std::string& report) {
    if (report.empty()) return;
    if (!g_log || !TryEnterCriticalSection(&g_logLock)) {
        OutputDebugStringA("Gitmo: watchdog report could not acquire the log; report follows:\n");
        OutputDebugStringA(report.c_str());
        return;
    }

    bool logged = false;
    if (g_logBytes < kLogLimit) {
        char prefix[40];
        SYSTEMTIME t; GetLocalTime(&t);
        int prefixSize = sprintf_s(prefix, "[%02d:%02d:%02d.%03d] ",
                                   t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        size_t required = prefixSize > 0 ? (size_t)prefixSize + report.size() + 1 : kLogLimit;
        if (prefixSize > 0 && required <= kLogLimit - g_logBytes) {
            size_t written = fwrite(prefix, 1, (size_t)prefixSize, g_log);
            if (written == (size_t)prefixSize) written += fwrite(report.data(), 1, report.size(), g_log);
            if (written == (size_t)prefixSize + report.size()) written += fwrite("\n", 1, 1, g_log);
            g_logBytes += written;
            logged = written == required && fflush(g_log) == 0;
        }
    }
    LeaveCriticalSection(&g_logLock);

    if (!logged) {
        OutputDebugStringA("Gitmo: watchdog report was not fully written to Gitmo.log; report follows:\n");
        OutputDebugStringA(report.c_str());
    }
}

// ---- settings -------------------------------------------------------------
static const std::wstring& IniPath() {
    static const std::wstring p = ModuleDir() + L"\\Gitmo.ini";
    return p;
}

// Reading and writing one value of the [Gitmo] section.
static int IniInt(const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(L"Gitmo", key, def, IniPath().c_str()); }
static std::wstring IniStr(const wchar_t* key, const wchar_t* def, DWORD cap = 64) {
    std::wstring s(cap, L'\0');
    DWORD n = GetPrivateProfileStringW(L"Gitmo", key, def, &s[0], cap, IniPath().c_str());
    s.resize(n);
    return s;
}
static float IniFloat(const wchar_t* key, const wchar_t* def) { return (float)_wtof(IniStr(key, def).c_str()); }
static bool Is(const std::wstring& s, const wchar_t* v) { return _wcsicmp(s.c_str(), v) == 0; }
static void IniSet(const wchar_t* key, const wchar_t* value) { WritePrivateProfileStringW(L"Gitmo", key, value, IniPath().c_str()); }
static void IniSetF(const wchar_t* key, const wchar_t* fmt, float v) { wchar_t b[32]; swprintf_s(b, fmt, v); IniSet(key, b); }

static const struct { const char* name; int vk; } kKeys[] = {
    {"Delete", VK_DELETE}, {"Insert", VK_INSERT}, {"Home", VK_HOME}, {"End", VK_END},
    {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"Pause", VK_PAUSE}, {"ScrollLock", VK_SCROLL},
    {"Tab", VK_TAB}, {"Backspace", VK_BACK}, {"Enter", VK_RETURN}, {"Space", VK_SPACE},
    {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
    {"NumLock", VK_NUMLOCK}, {"CapsLock", VK_CAPITAL}, {"Numpad*", VK_MULTIPLY}, {"Numpad+", VK_ADD},
    {"Numpad-", VK_SUBTRACT}, {"Numpad.", VK_DECIMAL}, {"Numpad/", VK_DIVIDE}, {"`", VK_OEM_3},
    {"-", VK_OEM_MINUS}, {"=", VK_OEM_PLUS}, {"[", VK_OEM_4}, {"]", VK_OEM_6}, {"\\", VK_OEM_5},
    {";", VK_OEM_1}, {"'", VK_OEM_7}, {",", VK_OEM_COMMA}, {".", VK_OEM_PERIOD}, {"/", VK_OEM_2},
};

std::string KeyName(int vk) {
    for (auto& k : kKeys) if (k.vk == vk) return k.name;
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Numpad" + std::to_string(vk - VK_NUMPAD0);
    char b[8]; sprintf_s(b, "0x%02X", vk); return b;
}

static int ParseKey(const std::string& s) {
    if (s.empty()) return 0;
    for (auto& k : kKeys) if (_stricmp(k.name, s.c_str()) == 0) return k.vk;
    if ((s[0] == 'F' || s[0] == 'f') && s.size() > 1 && isdigit((unsigned char)s[1])) {
        int n = 0;
        for (size_t i = 1; i < s.size(); ++i) {
            if (!isdigit((unsigned char)s[i])) return 0;
            n = n * 10 + (s[i] - '0');
            if (n > 24) return 0;
        }
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (_strnicmp(s.c_str(), "Numpad", 6) == 0 && s.size() == 7 && isdigit((unsigned char)s[6])) return VK_NUMPAD0 + (s[6] - '0');
    if (s.size() == 1 && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);
    if (_strnicmp(s.c_str(), "0x", 2) == 0 && s.size() > 2) {
        for (size_t i = 2; i < s.size(); ++i) if (!isxdigit((unsigned char)s[i])) return 0;
        char* end = nullptr;
        long v = strtol(s.c_str() + 2, &end, 16);
        if (end && !*end && v > 7 && v < 0xFF) return (int)v;
    }
    return 0;
}

void SaveMenuKey(int vk) {
    g_cfg.menuKey = vk;
    g_cfg.menuKeyName = KeyName(vk);
    std::wstring w(g_cfg.menuKeyName.begin(), g_cfg.menuKeyName.end());
    IniSet(L"MenuKey", w.c_str());
}

static const char* kDefaultIni =
    "[Gitmo]\r\n"
    "; 1 = Gitmo is active, 0 = it loads but does nothing.\r\n"
    "Enabled=1\r\n"
    "; Key that opens and closes the Gitmo phone, e.g. Delete, Insert, Home, End,\r\n"
    "; PageUp, PageDown, F1 - F12, a letter or digit. Easiest to change on the\r\n"
    "; phone itself: Settings > Menu key > Change.\r\n"
    "MenuKey=Delete\r\n"
    "; Phone colours: light or dark. Also switchable in the phone's Settings.\r\n"
    "Theme=light\r\n"
    "; 1 = write Gitmo.log next to this file.\r\n"
    "Log=1\r\n"
    "; Stage one only: log where the game keeps your money so the next version\r\n"
    "; can find it reliably. Read-only, changes nothing in the game.\r\n"
    "Research=1\r\n"
    "; 1 = send and receive money with other players who have Gitmo (over Steam).\r\n"
    "Transfers=1\r\n"
    "; Notifications (money received, new messages): sound, vibrate or off.\r\n"
    "NotifyMode=sound\r\n"
    "; 1 = only players in your game session can send you money or be sent money.\r\n"
    "; 0 = any Steam friend playing SnowRunner with Gitmo.\r\n"
    "SessionOnly=1\r\n"
    "; 1 = when money leaves your balance (a transfer you sent), press Esc for the game once,\r\n"
    "; so a shop or garage menu that's open backs out and shows the new balance when\r\n"
    "; reopened. With no menu open this opens the pause menu. 0 = don't.\r\n"
    "EscapeOnSend=1\r\n"
    "; How that Esc reaches the game: both, directinput or window.\r\n"
    "EscapeVia=both\r\n";

static void LoadSettings() {
    const std::wstring& ini = IniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w; WriteFile(h, kDefaultIni, (DWORD)strlen(kDefaultIni), &w, nullptr);
            CloseHandle(h);
        }
    }
    g_cfg.enabled  = IniInt(L"Enabled", 1) != 0;
    g_cfg.log      = IniInt(L"Log", 1) != 0;
    g_cfg.research = IniInt(L"Research", 1) != 0;
    {   // the key is plain ASCII ("Delete", "F8", "0x77")
        std::string key;
        for (wchar_t c : IniStr(L"MenuKey", L"Delete")) if (c < 128) key += (char)c;
        if (int vk = ParseKey(key)) { g_cfg.menuKey = vk; g_cfg.menuKeyName = KeyName(vk); }
    }
    g_cfg.dark = Is(IniStr(L"Theme", L"light"), L"dark");
    g_cfg.phoneX = IniFloat(L"PhoneX", L"-1");
    g_cfg.phoneY = IniFloat(L"PhoneY", L"-1");
    g_cfg.cracked = IniInt(L"CrackedScreen", 0) != 0;
    {   // recently used emoji, as hex code points ("1F600,1F44D")
        g_cfg.recentEmoji.clear();
        for (wchar_t c : IniStr(L"RecentEmoji", L"", 1024)) if (c < 128) g_cfg.recentEmoji += (char)c;
    }
    // notifications: NotifyMode=sound|vibrate|off (older Gitmo: NotifySound=1/0)
    std::wstring nm = IniStr(L"NotifyMode", L"");
    if (Is(nm, L"vibrate")) g_cfg.notifyMode = 1;
    else if (Is(nm, L"off")) g_cfg.notifyMode = 2;
    else if (Is(nm, L"sound")) g_cfg.notifyMode = 0;
    else g_cfg.notifyMode = IniInt(L"NotifySound", 1) != 0 ? 0 : 2;
    g_cfg.sessionOnly = IniInt(L"SessionOnly", 1) != 0;
    g_cfg.sessionResearch = IniInt(L"SessionResearch", 0);
    g_cfg.moneyResearch = IniInt(L"MoneyResearch", 0) != 0;
    g_cfg.garageResearch = IniInt(L"GarageResearch", 0);
    g_cfg.escapeOnSend = IniInt(L"EscapeOnSend", 1) != 0;
    std::wstring via = IniStr(L"EscapeVia", L"both");
    g_cfg.escapeVia = Is(via, L"directinput") ? 1 : Is(via, L"window") ? 2 : 0;
    g_cfg.developer = IniInt(L"Developer", 0) != 0;
    std::wstring size = IniStr(L"PhoneSize", L"medium");
    g_cfg.phoneSize = Is(size, L"small") ? 0 : Is(size, L"large") ? 2 : 1;
    g_cfg.wallpaper = std::min(std::max(IniInt(L"Wallpaper", 0), 0), 4);
    g_cfg.batteryMin = IniFloat(L"BatteryMinutes", L"15");
    std::wstring disp = IniStr(L"Display", L"auto");
    g_cfg.display = Is(disp, L"screen") ? 1 : Is(disp, L"headset") ? 2 : 0;
    g_cfg.vrDistance = std::min(std::max(IniFloat(L"VrDistance", L"0.60"), 0.25f), 3.0f);
    g_cfg.vrPointerSpeed = std::min(std::max(IniFloat(L"VrPointerSpeed", L"2.0"), 0.5f), 6.0f);
    g_cfg.savePhotos = IniInt(L"SavePhotos", IniInt(L"SavePhotosToSteam", 1)) != 0;
    // optional overrides, only needed if a game update moves these fields (hex like 0x768 accepted)
    std::wstring s = IniStr(L"EngineOffset", L"");
    if (!s.empty()) g_cfg.engineOffset = (int)wcstol(s.c_str(), nullptr, 0);
    s = IniStr(L"GarageOffset", L"");
    if (!s.empty()) g_cfg.garageOffset = (int)wcstol(s.c_str(), nullptr, 0);
}

void SavePhonePos(float x, float y) {
    g_cfg.phoneX = x; g_cfg.phoneY = y;
    IniSetF(L"PhoneX", L"%.4f", x);
    IniSetF(L"PhoneY", L"%.4f", y);
}

void SaveCracked(bool on) {
    g_cfg.cracked = on;
    IniSet(L"CrackedScreen", on ? L"1" : L"0");
}

void SaveRecentEmoji(const std::string& hexList) {
    g_cfg.recentEmoji = hexList;
    std::wstring w(hexList.begin(), hexList.end());
    IniSet(L"RecentEmoji", w.c_str());
}

void SaveWallpaper(int i) {
    g_cfg.wallpaper = i;
    IniSet(L"Wallpaper", std::to_wstring(i).c_str());
}

void SavePhoneSize(int i) {
    g_cfg.phoneSize = i;
    IniSet(L"PhoneSize", i == 0 ? L"small" : i == 2 ? L"large" : L"medium");
}

void SaveDisplay(int mode) {
    g_cfg.display = mode;
    IniSet(L"Display", mode == 1 ? L"screen" : mode == 2 ? L"headset" : L"auto");
}

void SaveVrDistance(float m) {
    g_cfg.vrDistance = m;
    IniSetF(L"VrDistance", L"%.2f", m);
}

void SaveVrPointerSpeed(float v) {
    g_cfg.vrPointerSpeed = v;
    IniSetF(L"VrPointerSpeed", L"%.1f", v);
}

void SaveNotifyMode(int mode) {
    g_cfg.notifyMode = mode;
    IniSet(L"NotifyMode", mode == 1 ? L"vibrate" : mode == 2 ? L"off" : L"sound");
}

void SaveBattery(float minutes) { IniSetF(L"BatteryMinutes", L"%.2f", minutes); }

void SaveThemeSetting(bool dark) { IniSet(L"Theme", dark ? L"dark" : L"light"); }

// "Vibrate": a phone's buzz-buzz on a table, made here as a WAV (two short rattles).
static const std::vector<uint8_t>& VibrateWav() {
    static std::vector<uint8_t> w = [] {
        const int rate = 22050;
        const float buzz = 0.32f, gap = 0.16f;
        int n = (int)(rate * (2 * buzz + gap));
        std::vector<int16_t> pcm(n);
        for (int i = 0; i < n; i++) {
            float t = (float)i / rate;
            float local = t < buzz ? t : t - buzz - gap;
            if (local < 0 || local > buzz) continue;
            float env = std::min(1.0f, local / 0.02f) * std::min(1.0f, (buzz - local) / 0.04f);
            // the motor: ~150 Hz, clipped so it sounds like a rattle, with a slow wobble
            float m = sinf(6.2831853f * 150.0f * t) + 0.5f * sinf(6.2831853f * 300.0f * t + 0.7f) + 0.25f * sinf(6.2831853f * 450.0f * t);
            m = std::max(-0.9f, std::min(0.9f, m * 1.6f));
            float wob = 0.75f + 0.25f * sinf(6.2831853f * 22.0f * t);
            pcm[i] = (int16_t)(m * env * wob * 0.105f * 32767);   // a quarter of the 0.8.x level (0.42 -> 0.21 in 0.9.0 -> 0.105 in 0.9.1)
        }
        std::vector<uint8_t> f(44 + n * 2);
        auto u32 = [&](size_t at, uint32_t v) { memcpy(&f[at], &v, 4); };
        auto u16 = [&](size_t at, uint16_t v) { memcpy(&f[at], &v, 2); };
        memcpy(&f[0], "RIFF", 4); u32(4, (uint32_t)(36 + n * 2)); memcpy(&f[8], "WAVEfmt ", 8);
        u32(16, 16); u16(20, 1); u16(22, 1); u32(24, rate); u32(28, rate * 2); u16(32, 2); u16(34, 16);
        memcpy(&f[36], "data", 4); u32(40, (uint32_t)(n * 2));
        memcpy(&f[44], pcm.data(), n * 2);
        return f;
    }();
    return w;
}
void Notify_Vibrate() { PlaySoundW((LPCWSTR)VibrateWav().data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT); }

// The notification sound is built into the DLL (resource "NOTIFY", type WAVE). A file called
// Gitmo-notify.wav next to the DLL replaces it, for anyone who wants a different sound.
void Notify_Play() {
    static const std::wstring custom = ModuleDir() + L"\\Gitmo-notify.wav";
    if (GetFileAttributesW(custom.c_str()) != INVALID_FILE_ATTRIBUTES &&
        PlaySoundW(custom.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) return;
    static const void* wav = [] {   // looked up once; stays mapped as long as the DLL is loaded
        HRSRC r = FindResourceW(g_self, L"NOTIFY", L"WAVE");
        HGLOBAL h = r ? LoadResource(g_self, r) : nullptr;
        return h ? LockResource(h) : nullptr;
    }();
    if (!wav || !PlaySoundW((LPCWSTR)wav, nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT)) MessageBeep(MB_ICONASTERISK);
}

// what an incoming money transfer or message does, by the Notifications setting
void Notify_Alert() {
    if (g_cfg.notifyMode == 0) Notify_Play();
    else if (g_cfg.notifyMode == 1) Notify_Vibrate();
}

// ---- startup ----------------------------------------------------------------
static DWORD WINAPI InitThread(LPVOID) {
    if (ModuleDir().empty()) return ERROR_PATH_NOT_FOUND;
    LoadSettings();
    if (g_cfg.log) {
        // keep the previous session's log, so a launch that froze or crashed can still be read
        std::wstring cur = ModuleDir() + L"\\Gitmo.log", prev = ModuleDir() + L"\\Gitmo.previous.log";
        MoveFileExW(cur.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
        g_log = _wfsopen(cur.c_str(), L"w", _SH_DENYWR);
    }
    HMODULE exe = GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    g_exeBase = (uintptr_t)exe;
    g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
    Log("Gitmo %s loaded (game build 0x%08X, image size 0x%X)", GITMO_VERSION,
        nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    int missing = 0;
    for (void* p : g_real) if (!p) missing++;
    if (missing) Log("WARNING: %d version.dll functions not found in the system copy", missing);
    if (!g_cfg.enabled) { Log("Enabled=0 in Gitmo.ini -> doing nothing"); return 0; }
    Log("menu key %s, theme %s, research %s", g_cfg.menuKeyName.c_str(),
        g_cfg.dark ? "dark" : "light", g_cfg.research ? "on" : "off");
    // The headset panel is only prepared when the VR mod (a dxgi.dll next to the game) is
    // installed, so flat-screen players get nothing extra. Display=screen in Gitmo.ini (or
    // on the phone) keeps the phone on the flat screen even then.
    {
        bool vrMod = false;
        bool vrPathKnown = false;
        std::wstring exePath;
        if (GetModuleFilePath(nullptr, exePath)) {
            size_t slash = exePath.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                exePath.resize(slash + 1);
                exePath += L"dxgi.dll";
                vrMod = GetFileAttributesW(exePath.c_str()) != INVALID_FILE_ATTRIBUTES;
                vrPathKnown = true;
            }
        }
        bool allowed = IniInt(L"VrPanel", 1) != 0;
        if (!vrPathKnown) Log("vr: unable to resolve the game path -> headset panel off");
        else if (!vrMod) Log("vr: no VR mod next to the game -> flat-screen phone");
        else if (!allowed) Log("vr: VrPanel=0 in Gitmo.ini -> headset panel off");
        else VR_Start();
    }
    Overlay_Start();
    Input_Start();
    Money_Start();
    Radio_Start();
    if (g_cfg.sessionResearch >= 2) Probe_Start();
    if (g_cfg.moneyResearch) MoneyWatch_Start();
    Game_Start();
    // Transfers between players over Steam (Transfers=0 in Gitmo.ini turns them off).
    if (IniInt(L"Transfers", 1)) Tx_Start();
    else Log("transfers: Transfers=0 in Gitmo.ini -> off");
    Watchdog_Start();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        std::wstring sys;
        if (!GetSystemDirectoryPath(sys)) {
            OutputDebugStringW(L"Gitmo: unable to get the Windows system directory.\n");
            return FALSE;
        }
        sys += L"\\version.dll";
        HMODULE real = LoadLibraryW(sys.c_str());
        if (!real) {
            OutputDebugStringW(L"Gitmo: unable to load the system version.dll.\n");
            return FALSE;
        }
        InitializeCriticalSection(&g_logLock);
        for (int i = 0; i < 17; i++) g_real[i] = (void*)GetProcAddress(real, kExports[i]);
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        Game_Stop();
    }
    return TRUE;
}