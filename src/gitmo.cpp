// Gitmo - a phone-style money app for SnowRunner.
// Stage one: version.dll proxy, in-game menu (F8), live balance, research logging.
//
// Loaded by SnowRunner as version.dll from its Bin folder. Every version.dll
// export is passed straight to C:\Windows\System32\version.dll (proxy.asm).
// Gitmo shares nothing with NoAutoReverse (dinput8.dll) or the VR mod (dxgi.dll).

#include "gitmo.h"
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>

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

std::wstring ModuleDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(g_self, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/"));
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    if (g_logBytes < 5u * 1024 * 1024) {
        SYSTEMTIME t; GetLocalTime(&t);
        int n = fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list a; va_start(a, fmt);
        n += vfprintf(g_log, fmt, a);
        va_end(a);
        n += fprintf(g_log, "\n");
        fflush(g_log);
        g_logBytes += n > 0 ? n : 0;
    }
    LeaveCriticalSection(&g_logLock);
}

// ---- settings -------------------------------------------------------------
static std::wstring IniPath() { return ModuleDir() + L"\\Gitmo.ini"; }

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
    for (auto& k : kKeys) if (_stricmp(k.name, s.c_str()) == 0) return k.vk;
    if ((s[0] == 'F' || s[0] == 'f') && s.size() > 1 && isdigit((unsigned char)s[1])) {
        int n = atoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (_strnicmp(s.c_str(), "Numpad", 6) == 0 && s.size() == 7 && isdigit((unsigned char)s[6])) return VK_NUMPAD0 + (s[6] - '0');
    if (s.size() == 1 && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);
    if (_strnicmp(s.c_str(), "0x", 2) == 0) { int v = (int)strtol(s.c_str() + 2, nullptr, 16); if (v > 7 && v < 0xFF) return v; }
    return 0;
}

void SaveMenuKey(int vk) {
    g_cfg.menuKey = vk;
    g_cfg.menuKeyName = KeyName(vk);
    std::wstring w(g_cfg.menuKeyName.begin(), g_cfg.menuKeyName.end());
    WritePrivateProfileStringW(L"Gitmo", L"MenuKey", w.c_str(), IniPath().c_str());
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
    std::wstring ini = IniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w; WriteFile(h, kDefaultIni, (DWORD)strlen(kDefaultIni), &w, nullptr);
            CloseHandle(h);
        }
    }
    const wchar_t* sec = L"Gitmo";
    g_cfg.enabled  = GetPrivateProfileIntW(sec, L"Enabled", 1, ini.c_str()) != 0;
    g_cfg.log      = GetPrivateProfileIntW(sec, L"Log", 1, ini.c_str()) != 0;
    g_cfg.research = GetPrivateProfileIntW(sec, L"Research", 1, ini.c_str()) != 0;
    wchar_t buf[32];
    GetPrivateProfileStringW(sec, L"MenuKey", L"Delete", buf, 32, ini.c_str());
    char key[32]; WideCharToMultiByte(CP_UTF8, 0, buf, -1, key, 32, nullptr, nullptr);
    if (int vk = key[0] ? ParseKey(key) : 0) { g_cfg.menuKey = vk; g_cfg.menuKeyName = KeyName(vk); }
    GetPrivateProfileStringW(sec, L"Theme", L"light", buf, 32, ini.c_str());
    g_cfg.dark = _wcsicmp(buf, L"dark") == 0;
    GetPrivateProfileStringW(sec, L"PhoneX", L"-1", buf, 32, ini.c_str()); g_cfg.phoneX = (float)_wtof(buf);
    GetPrivateProfileStringW(sec, L"PhoneY", L"-1", buf, 32, ini.c_str()); g_cfg.phoneY = (float)_wtof(buf);
    g_cfg.cracked = GetPrivateProfileIntW(sec, L"CrackedScreen", 0, ini.c_str()) != 0;
    {   // recently used emoji, as hex code points ("1F600,1F44D")
        wchar_t rb[256] = {};
        GetPrivateProfileStringW(sec, L"RecentEmoji", L"", rb, 256, ini.c_str());
        for (wchar_t* q = rb; *q; q++) if (*q < 128) g_cfg.recentEmoji += (char)*q;
    }
    // notifications: NotifyMode=sound|vibrate|off (older Gitmo: NotifySound=1/0)
    GetPrivateProfileStringW(sec, L"NotifyMode", L"", buf, 32, ini.c_str());
    if (!_wcsicmp(buf, L"vibrate")) g_cfg.notifyMode = 1;
    else if (!_wcsicmp(buf, L"off")) g_cfg.notifyMode = 2;
    else if (!_wcsicmp(buf, L"sound")) g_cfg.notifyMode = 0;
    else g_cfg.notifyMode = GetPrivateProfileIntW(sec, L"NotifySound", 1, ini.c_str()) != 0 ? 0 : 2;
    g_cfg.sessionOnly = GetPrivateProfileIntW(sec, L"SessionOnly", 1, ini.c_str()) != 0;
    g_cfg.sessionResearch = GetPrivateProfileIntW(sec, L"SessionResearch", 0, ini.c_str());
    g_cfg.moneyResearch = GetPrivateProfileIntW(sec, L"MoneyResearch", 0, ini.c_str()) != 0;
    g_cfg.garageResearch = (int)GetPrivateProfileIntW(sec, L"GarageResearch", 0, ini.c_str());
    g_cfg.escapeOnSend = GetPrivateProfileIntW(sec, L"EscapeOnSend", 1, ini.c_str()) != 0;
    GetPrivateProfileStringW(sec, L"EscapeVia", L"both", buf, 32, ini.c_str());
    g_cfg.escapeVia = !_wcsicmp(buf, L"directinput") ? 1 : !_wcsicmp(buf, L"window") ? 2 : 0;
    g_cfg.developer = GetPrivateProfileIntW(sec, L"Developer", 0, ini.c_str()) != 0;
    GetPrivateProfileStringW(sec, L"PhoneSize", L"medium", buf, 32, ini.c_str());
    g_cfg.phoneSize = _wcsicmp(buf, L"small") == 0 ? 0 : _wcsicmp(buf, L"large") == 0 ? 2 : 1;
    g_cfg.wallpaper = std::min(std::max((int)GetPrivateProfileIntW(sec, L"Wallpaper", 0, ini.c_str()), 0), 4);
    GetPrivateProfileStringW(sec, L"BatteryMinutes", L"15", buf, 32, ini.c_str()); g_cfg.batteryMin = (float)_wtof(buf);
    GetPrivateProfileStringW(sec, L"Display", L"auto", buf, 32, ini.c_str());
    g_cfg.display = _wcsicmp(buf, L"screen") == 0 ? 1 : _wcsicmp(buf, L"headset") == 0 ? 2 : 0;
    GetPrivateProfileStringW(sec, L"VrDistance", L"0.60", buf, 32, ini.c_str());
    g_cfg.vrDistance = std::min(std::max((float)_wtof(buf), 0.25f), 3.0f);
    GetPrivateProfileStringW(sec, L"VrPointerSpeed", L"2.0", buf, 32, ini.c_str());
    g_cfg.vrPointerSpeed = std::min(std::max((float)_wtof(buf), 0.5f), 6.0f);
    g_cfg.savePhotos = GetPrivateProfileIntW(sec, L"SavePhotos", GetPrivateProfileIntW(sec, L"SavePhotosToSteam", 1, ini.c_str()), ini.c_str()) != 0;
    // optional overrides, only needed if a game update moves these fields (hex like 0x768 accepted)
    GetPrivateProfileStringW(sec, L"EngineOffset", L"", buf, 32, ini.c_str()); if (buf[0]) g_cfg.engineOffset = (int)wcstol(buf, nullptr, 0);
    GetPrivateProfileStringW(sec, L"GarageOffset", L"", buf, 32, ini.c_str()); if (buf[0]) g_cfg.garageOffset = (int)wcstol(buf, nullptr, 0);
}

void SavePhonePos(float x, float y) {
    g_cfg.phoneX = x; g_cfg.phoneY = y;
    wchar_t b[32];
    swprintf_s(b, L"%.4f", x); WritePrivateProfileStringW(L"Gitmo", L"PhoneX", b, IniPath().c_str());
    swprintf_s(b, L"%.4f", y); WritePrivateProfileStringW(L"Gitmo", L"PhoneY", b, IniPath().c_str());
}

void SaveCracked(bool on) {
    g_cfg.cracked = on;
    WritePrivateProfileStringW(L"Gitmo", L"CrackedScreen", on ? L"1" : L"0", IniPath().c_str());
}

void SaveRecentEmoji(const std::string& hexList) {
    g_cfg.recentEmoji = hexList;
    std::wstring w(hexList.begin(), hexList.end());
    WritePrivateProfileStringW(L"Gitmo", L"RecentEmoji", w.c_str(), IniPath().c_str());
}

void SaveWallpaper(int i) {
    g_cfg.wallpaper = i;
    WritePrivateProfileStringW(L"Gitmo", L"Wallpaper", std::to_wstring(i).c_str(), IniPath().c_str());
}

void SavePhoneSize(int i) {
    g_cfg.phoneSize = i;
    WritePrivateProfileStringW(L"Gitmo", L"PhoneSize", i == 0 ? L"small" : i == 2 ? L"large" : L"medium", IniPath().c_str());
}

void SaveDisplay(int mode) {
    g_cfg.display = mode;
    WritePrivateProfileStringW(L"Gitmo", L"Display", mode == 1 ? L"screen" : mode == 2 ? L"headset" : L"auto", IniPath().c_str());
}

void SaveVrDistance(float m) {
    g_cfg.vrDistance = m;
    wchar_t b[32]; swprintf_s(b, L"%.2f", m);
    WritePrivateProfileStringW(L"Gitmo", L"VrDistance", b, IniPath().c_str());
}

void SaveVrPointerSpeed(float v) {
    g_cfg.vrPointerSpeed = v;
    wchar_t b[32]; swprintf_s(b, L"%.1f", v);
    WritePrivateProfileStringW(L"Gitmo", L"VrPointerSpeed", b, IniPath().c_str());
}

void SaveNotifyMode(int mode) {
    g_cfg.notifyMode = mode;
    WritePrivateProfileStringW(L"Gitmo", L"NotifyMode", mode == 1 ? L"vibrate" : mode == 2 ? L"off" : L"sound", IniPath().c_str());
}

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

// what an incoming money transfer or message does, by the Notifications setting
void Notify_Alert() {
    if (g_cfg.notifyMode == 0) Notify_Play();
    else if (g_cfg.notifyMode == 1) Notify_Vibrate();
}

// The notification sound is built into the DLL (resource "NOTIFY", type WAVE). A file called
// Gitmo-notify.wav next to the DLL replaces it, for anyone who wants a different sound.
void Notify_Play() {
    std::wstring custom = ModuleDir() + L"\\Gitmo-notify.wav";
    if (GetFileAttributesW(custom.c_str()) != INVALID_FILE_ATTRIBUTES &&
        PlaySoundW(custom.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) return;
    HRSRC r = FindResourceW(g_self, L"NOTIFY", L"WAVE");
    HGLOBAL h = r ? LoadResource(g_self, r) : nullptr;
    const void* wav = h ? LockResource(h) : nullptr;   // stays mapped as long as the DLL is loaded
    if (!wav || !PlaySoundW((LPCWSTR)wav, nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT)) MessageBeep(MB_ICONASTERISK);
}

void SaveBattery(float minutes) {
    wchar_t b[32]; swprintf_s(b, L"%.2f", minutes);
    WritePrivateProfileStringW(L"Gitmo", L"BatteryMinutes", b, IniPath().c_str());
}

void SaveThemeSetting(bool dark) {
    WritePrivateProfileStringW(L"Gitmo", L"Theme", dark ? L"dark" : L"light", IniPath().c_str());
}

// ---- startup ----------------------------------------------------------------
static DWORD WINAPI InitThread(LPVOID) {
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
        wchar_t exeDir[MAX_PATH]; GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
        *(wcsrchr(exeDir, L'\\') + 1) = 0;
        bool vrMod = GetFileAttributesW((std::wstring(exeDir) + L"dxgi.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
        bool allowed = GetPrivateProfileIntW(L"Gitmo", L"VrPanel", 1, IniPath().c_str()) != 0;
        if (!vrMod) Log("vr: no VR mod next to the game -> flat-screen phone");
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
    if (GetPrivateProfileIntW(L"Gitmo", L"Transfers", 1, IniPath().c_str())) Tx_Start();
    else Log("transfers: Transfers=0 in Gitmo.ini -> off");
    void Watchdog_Start(); Watchdog_Start();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logLock);
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        wcscat_s(sys, L"\\version.dll");
        HMODULE real = LoadLibraryW(sys);
        if (!real) return FALSE;
        for (int i = 0; i < 17; i++) g_real[i] = (void*)GetProcAddress(real, kExports[i]);
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
