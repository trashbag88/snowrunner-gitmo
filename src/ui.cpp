// Gitmo - the phone. Everything is drawn with ImGui's draw list so it looks
// like a phone payments app rather than a debug window.

#include "gitmo.h"
#include "shared.h"
#include "imgui.h"
#include "imgui_internal.h"   // ClearActiveID: a drag that scrolls cancels the tap
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <functional>
#include <map>

static ImFont *F_Reg, *F_Semi, *F_Bold;

enum App { Drawer, GitmoApp, SettingsApp, SmsApp, RadioApp };
static App g_app = Drawer;      // what the phone shows: the app drawer or an open app
enum Screen { Home, ActivityScr, SendPick, SendAmount, SendStatus };
static Screen g_screen = Home;  // tab inside Gitmo
static TxView g_tx;             // transfers, read once per frame
static uint64_t g_sendPeer, g_sendId;
static std::string g_sendName, g_amount, g_sendErr;   // g_amount: digits typed on the keypad
static bool g_sheet;            // "transfers are coming" bottom sheet
static const char* g_sheetTitle = "";
static std::string g_name;
static RadioState g_rad;         // the radio, read once per frame (status bar, mini player, Radio app)
static float s = 1.0f;          // scale: 1.0 = an 800 px tall phone

struct Theme { ImU32 bezel, bg, card, text, sub, accent, accentDark, accentSoft, divider, outBg; };
static Theme T;

static ImU32 Hex(unsigned rgb, int a = 255) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a); }

static void ApplyTheme() {
    if (g_cfg.dark)
        T = { Hex(0x050608), Hex(0x0D1117), Hex(0x171C24), Hex(0xF0F3F6), Hex(0x8B949E),
              Hex(0x1DC983), Hex(0x10935D), Hex(0x10291E), Hex(0x262C35), Hex(0x222832) };
    else
        T = { Hex(0x101216), Hex(0xF3F5F7), Hex(0xFFFFFF), Hex(0x0E1726), Hex(0x6B7280),
              Hex(0x0FA968), Hex(0x0A8150), Hex(0xE1F5EA), Hex(0xE6E8EC), Hex(0xEEF0F3) };
}

// ---- Steam name -------------------------------------------------------------------
static void GuardedPersona(void* (__cdecl* acc)(), const char* (__cdecl* getName)(void*), char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    __try {
        void* f = acc();
        const char* name = f ? getName(f) : nullptr;
        if (name) strncpy_s(out, cap, name, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}

std::string Steam_PersonaName() {
    HMODULE sa = GetModuleHandleW(L"steam_api64.dll");
    if (!sa) return {};
    auto getName = (const char*(__cdecl*)(void*))GetProcAddress(sa, "SteamAPI_ISteamFriends_GetPersonaName");
    if (!getName) return {};
    for (int v = 20; v >= 14; v--) {
        char fn[64]; sprintf_s(fn, "SteamAPI_SteamFriends_v%03d", v);
        auto acc = (void*(__cdecl*)())GetProcAddress(sa, fn);
        if (!acc) continue;
        char name[256] = {};
        GuardedPersona(acc, getName, name, sizeof(name));
        if (name[0]) return name;
    }
    return {};
}

// ---- helpers ----------------------------------------------------------------------
static ImDrawList* DL() { return ImGui::GetWindowDrawList(); }

static ImVec2 Measure(ImFont* f, float size, const char* t) { return f->CalcTextSizeA(size, FLT_MAX, 0, t); }
static void Text(ImFont* f, float size, ImVec2 p, ImU32 c, const char* t) { DL()->AddText(f, size, p, c, t); }
static void TextRight(ImFont* f, float size, ImVec2 rightTop, ImU32 c, const char* t) {
    Text(f, size, ImVec2(rightTop.x - Measure(f, size, t).x, rightTop.y), c, t);
}
static void TextCenter(ImFont* f, float size, ImVec2 centerTop, ImU32 c, const char* t) {
    Text(f, size, ImVec2(centerTop.x - Measure(f, size, t).x * 0.5f, centerTop.y), c, t);
}

static std::string Money(long long v, bool sign = false) {
    unsigned long long magnitude = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    char digits[32]; sprintf_s(digits, "%llu", magnitude);
    std::string out;
    int n = (int)strlen(digits);
    for (int i = 0; i < n; i++) { out += digits[i]; if ((n - i - 1) % 3 == 0 && i != n - 1) out += ','; }
    const char* pre = v < 0 ? "\xE2\x88\x92$" : (sign ? "+$" : "$");   // U+2212 minus
    return pre + out;
}

static bool g_behindSheet;   // drawing the screen behind the sheet: its buttons don't exist

static bool Hit(const char* id, ImVec2 a, ImVec2 b) {
    if (g_behindSheet) return false;   // (a disabled ImGui button would still take the click first)
    ImGui::SetCursorScreenPos(a);
    return ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y));
}

// Drag to scroll, like a phone: press anywhere in a list and move up or down. Once the press
// has moved a few pixels it is a scroll, and the button it started on doesn't fire. (In VR
// this is the main way to scroll; the mouse wheel works too.) Call just before EndChild().
static void DragScroll() {
    static ImGuiID active; static float fromY, fromScroll; static bool moved;
    if (g_behindSheet) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGuiID id = ImGui::GetID("##dragscroll");
    if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        active = id; fromY = io.MousePos.y; fromScroll = ImGui::GetScrollY(); moved = false;
    }
    if (active != id) return;
    if (!io.MouseDown[0]) { active = 0; return; }
    float dy = io.MousePos.y - fromY;
    if (!moved && fabsf(dy) > 8 * s) { moved = true; ImGui::ClearActiveID(); }   // now a scroll: the tap is cancelled
    if (moved) ImGui::SetScrollY(std::max(0.0f, std::min(fromScroll - dy, ImGui::GetScrollMaxY())));
}

// ---- icons (drawn, so no icon font is needed) ----------------------------------------
static void IconArrow(ImVec2 c, float r, ImU32 col, bool out) {
    float k = r * 0.42f, th = r * 0.13f;
    ImVec2 from = out ? ImVec2(c.x - k, c.y + k) : ImVec2(c.x + k, c.y - k);
    ImVec2 tip  = out ? ImVec2(c.x + k, c.y - k) : ImVec2(c.x - k, c.y + k);
    DL()->AddLine(from, tip, col, th);
    float h = r * 0.5f;
    if (out) { DL()->AddLine(tip, ImVec2(tip.x - h, tip.y), col, th); DL()->AddLine(tip, ImVec2(tip.x, tip.y + h), col, th); }
    else     { DL()->AddLine(tip, ImVec2(tip.x + h, tip.y), col, th); DL()->AddLine(tip, ImVec2(tip.x, tip.y - h), col, th); }
}
static void IconBank(ImVec2 c, float r, ImU32 col) {
    float w = r * 0.62f;
    DL()->AddTriangleFilled(ImVec2(c.x - w * 1.1f, c.y - r * 0.25f), ImVec2(c.x + w * 1.1f, c.y - r * 0.25f), ImVec2(c.x, c.y - r * 0.68f), col);
    for (int i = -1; i <= 1; i++) DL()->AddRectFilled(ImVec2(c.x + i * w * 0.72f - r * 0.07f, c.y - r * 0.18f), ImVec2(c.x + i * w * 0.72f + r * 0.07f, c.y + r * 0.38f), col);
    DL()->AddRectFilled(ImVec2(c.x - w * 1.05f, c.y + r * 0.44f), ImVec2(c.x + w * 1.05f, c.y + r * 0.56f), col, r * 0.03f);
}
static void IconHome(ImVec2 c, float r, ImU32 col) {
    float th = r * 0.14f;
    ImVec2 roof[3] = { ImVec2(c.x - r * 0.75f, c.y - r * 0.05f), ImVec2(c.x, c.y - r * 0.7f), ImVec2(c.x + r * 0.75f, c.y - r * 0.05f) };
    DL()->AddPolyline(roof, 3, col, 0, th);
    DL()->AddRect(ImVec2(c.x - r * 0.5f, c.y - r * 0.2f), ImVec2(c.x + r * 0.5f, c.y + r * 0.62f), col, r * 0.08f, 0, th);
}
static void IconList(ImVec2 c, float r, ImU32 col) {
    for (int i = -1; i <= 1; i++) {
        float y = c.y + i * r * 0.42f;
        DL()->AddCircleFilled(ImVec2(c.x - r * 0.6f, y), r * 0.1f, col);
        DL()->AddLine(ImVec2(c.x - r * 0.32f, y), ImVec2(c.x + r * 0.72f, y), col, r * 0.14f);
    }
}
static void IconGear(ImVec2 c, float r, ImU32 col) {
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159265f / 4;
        DL()->AddCircleFilled(ImVec2(c.x + cosf(a) * r * 0.55f, c.y + sinf(a) * r * 0.55f), r * 0.16f, col);
    }
    DL()->AddCircle(c, r * 0.42f, col, 0, r * 0.2f);
}
static void IconNote(ImVec2 c, float r, ImU32 col) {   // two beamed quavers: the radio
    float hr = r * 0.27f, th = std::max(1.0f, r * 0.13f);
    ImVec2 h1(c.x - r * 0.42f, c.y + r * 0.55f), h2(c.x + r * 0.5f, c.y + r * 0.4f);
    DL()->AddCircleFilled(h1, hr, col);
    DL()->AddCircleFilled(h2, hr, col);
    float s1 = h1.x + hr - th * 0.5f, s2 = h2.x + hr - th * 0.5f, t1 = c.y - r * 0.6f, t2 = c.y - r * 0.75f;
    DL()->AddLine(ImVec2(s1, h1.y), ImVec2(s1, t1), col, th);
    DL()->AddLine(ImVec2(s2, h2.y), ImVec2(s2, t2), col, th);
    float bh = r * 0.24f;
    DL()->AddQuadFilled(ImVec2(s1 - th * 0.5f, t1), ImVec2(s2 + th * 0.5f, t2), ImVec2(s2 + th * 0.5f, t2 + bh), ImVec2(s1 - th * 0.5f, t1 + bh), col);
}

// ---- pieces -----------------------------------------------------------------------
static void IconCross(ImVec2 c, float r, ImU32 col);
// "Today", "Yesterday", or "Sat 8 Nov" for older entries.
static std::string DayLabel(const SYSTEMTIME& t) {
    SYSTEMTIME now; GetLocalTime(&now);
    auto days = [](const SYSTEMTIME& x) { FILETIME f; SYSTEMTIME d = x; d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0; d.wDayOfWeek = 0;
                                          SystemTimeToFileTime(&d, &f); return (long long)(((ULARGE_INTEGER{ { f.dwLowDateTime, f.dwHighDateTime } }).QuadPart) / 864000000000ULL); };
    long long diff = days(now) - days(t);
    if (diff == 0) return "Today";
    if (diff == 1) return "Yesterday";
    static const char* wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char* mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    SYSTEMTIME d = t; FILETIME f; SystemTimeToFileTime(&d, &f); FileTimeToSystemTime(&f, &d);   // fills in the weekday
    char b[32]; sprintf_s(b, "%s %d %s", wd[d.wDayOfWeek % 7], t.wDay, mo[(t.wMonth + 11) % 12]);
    return b;
}
static void ActivityRow(const Activity& a, ImVec2 p, float w, bool divider) {
    float h = 58 * s;
    ImVec2 ic(p.x + 32 * s, p.y + h * 0.5f);
    if (!a.status.empty()) {   // a transfer that ended without moving money (declined, no answer...)
        DL()->AddCircleFilled(ic, 19 * s, T.outBg);
        IconCross(ic, 26 * s, T.sub);
        Text(F_Semi, 15 * s, ImVec2(p.x + 62 * s, p.y + 11 * s), T.text, a.label.c_str());
        char sub[200]; sprintf_s(sub, "%s, %02d:%02d  \xC2\xB7  %s", DayLabel(a.when).c_str(), a.when.wHour, a.when.wMinute, a.status.c_str());
        DL()->PushClipRect(ImVec2(p.x + 62 * s, p.y), ImVec2(p.x + w - 86 * s, p.y + h), true);
        Text(F_Reg, 12.5f * s, ImVec2(p.x + 62 * s, p.y + 31 * s), T.sub, sub);
        DL()->PopClipRect();
        std::string amt = Money(a.amount);
        ImVec2 sz = Measure(F_Semi, 15 * s, amt.c_str());
        ImVec2 rt(p.x + w - 16 * s, p.y + 19 * s);
        TextRight(F_Semi, 15 * s, rt, T.sub, amt.c_str());
        DL()->AddLine(ImVec2(rt.x - sz.x - 1 * s, rt.y + sz.y * 0.55f), ImVec2(rt.x + 1 * s, rt.y + sz.y * 0.55f), T.sub, 1.4f * s);   // struck through
        if (divider) DL()->AddLine(ImVec2(p.x + 62 * s, p.y + h), ImVec2(p.x + w - 16 * s, p.y + h), T.divider, 1.0f);
        return;
    }
    bool in = a.delta > 0;
    DL()->AddCircleFilled(ic, 19 * s, in ? T.accentSoft : T.outBg);
    IconArrow(ic, 19 * s, in ? T.accent : T.text, !in);
    Text(F_Semi, 15 * s, ImVec2(p.x + 62 * s, p.y + 11 * s), T.text, !a.label.empty() ? a.label.c_str() : in ? "Money in" : "Money out");
    char when[96]; sprintf_s(when, "%s, %02d:%02d  \xC2\xB7  balance %s", DayLabel(a.when).c_str(), a.when.wHour, a.when.wMinute, Money(a.balance).c_str());
    Text(F_Reg, 12.5f * s, ImVec2(p.x + 62 * s, p.y + 31 * s), T.sub, when);
    TextRight(F_Semi, 15 * s, ImVec2(p.x + w - 16 * s, p.y + 19 * s), in ? T.accent : T.text, Money(a.delta, true).c_str());
    if (divider) DL()->AddLine(ImVec2(p.x + 62 * s, p.y + h), ImVec2(p.x + w - 16 * s, p.y + h), T.divider, 1.0f);
}

static void EmptyActivity(ImVec2 p, float w) {
    TextCenter(F_Semi, 15 * s, ImVec2(p.x + w * 0.5f, p.y + 20 * s), T.text, "No activity yet");
    TextCenter(F_Reg, 13 * s, ImVec2(p.x + w * 0.5f, p.y + 42 * s), T.sub, "Earn or spend money in game");
    TextCenter(F_Reg, 13 * s, ImVec2(p.x + w * 0.5f, p.y + 59 * s), T.sub, "and it shows up here.");
}

static void* AvatarTex(uint64_t id);   // SMS section: a player's Steam picture as a texture (null until Steam has it)

static void ScreenHome(const MoneyState& st, ImVec2 a, ImVec2 b) {
    float x = a.x + 20 * s, w = b.x - a.x - 40 * s, y = a.y + 6 * s;
    // header
    Text(F_Bold, 30 * s, ImVec2(x, y), T.accent, "gitmo");
    ImVec2 av(b.x - 38 * s, y + 18 * s);
    uint64_t me = g_tx.me;
#ifdef GITMO_TEST
    if (!me) me = 2;   // the stand-in picture
#endif
    if (void* tex = me ? AvatarTex(me) : nullptr) {   // your Steam picture, round, like SMS
        DL()->AddImageRounded(ImTextureRef((ImTextureID)tex), ImVec2(av.x - 18 * s, av.y - 18 * s), ImVec2(av.x + 18 * s, av.y + 18 * s),
                              ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255), 18 * s);
    } else {   // until Steam has it: your initial
        DL()->AddCircleFilled(av, 18 * s, T.accentSoft);
        char ini[2] = { g_name.empty() ? 'D' : (char)toupper((unsigned char)g_name[0]), 0 };
        TextCenter(F_Bold, 16 * s, ImVec2(av.x, av.y - 11 * s), T.accent, ini);
    }
    y += 44 * s;
    std::string hi = "Hi, " + (g_name.empty() ? std::string("driver") : g_name);
    Text(F_Reg, 15 * s, ImVec2(x, y), T.sub, hi.c_str());
    y += 30 * s;

    // balance card
    float ch = 156 * s, strip = 44 * s;
    DL()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + ch), T.accent, 22 * s);
    DL()->AddRectFilled(ImVec2(x, y + ch - strip), ImVec2(x + w, y + ch), T.accentDark, 22 * s, ImDrawFlags_RoundCornersBottom);
    ImU32 white = IM_COL32(255, 255, 255, 255), soft = IM_COL32(255, 255, 255, 200);
    Text(F_Semi, 14 * s, ImVec2(x + 20 * s, y + 16 * s), soft, "Gitmo balance");
    if (st.live && st.slot) Text(F_Bold, 42 * s, ImVec2(x + 18 * s, y + 38 * s), white, Money(st.balance).c_str());
    else         Text(F_Bold, 42 * s, ImVec2(x + 18 * s, y + 38 * s), soft, "$ \xE2\x80\x94");
    float sy = y + ch - strip + 13 * s;
    char wallet[48];
    if (st.live && st.slot) sprintf_s(wallet, "SnowRunner wallet \xC2\xB7 save %d", st.slot); else sprintf_s(wallet, "SnowRunner wallet");
    Text(F_Reg, 13 * s, ImVec2(x + 20 * s, sy), soft, wallet);
    const char* chip = !Game_InGame() ? "Main menu" : st.live ? (st.slot ? "Live" : "Checking") : (st.phase == MoneyState::Scanning ? "Searching" : "Waiting");
    float cw = Measure(F_Semi, 12.5f * s, chip).x;
    ImVec2 c0(x + w - cw - 44 * s, sy - 4 * s), c1(x + w - 16 * s, sy + 20 * s);
    DL()->AddRectFilled(c0, c1, IM_COL32(255, 255, 255, 38), 12 * s);
    float pulse = st.live ? 1.0f : 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 4);
    DL()->AddCircleFilled(ImVec2(c0.x + 12 * s, (c0.y + c1.y) * 0.5f), 4 * s, st.live ? IM_COL32(170, 255, 205, 255) : IM_COL32(255, 230, 150, (int)(255 * pulse)));
    Text(F_Semi, 12.5f * s, ImVec2(c0.x + 22 * s, c0.y + 4 * s), white, chip);
    y += ch + 22 * s;

    // quick actions
    const char* labels[3] = { "Send", "Request", "Bank" };
    for (int i = 0; i < 3; i++) {
        ImVec2 c(x + w * (i * 2 + 1) / 6.0f, y + 28 * s);
        ImVec2 h0(c.x - 34 * s, c.y - 28 * s), h1(c.x + 34 * s, c.y + 50 * s);
        bool clicked = Hit(labels[i], h0, h1);
        bool hov = ImGui::IsItemHovered();
        DL()->AddCircleFilled(c, (hov ? 29 : 27) * s, T.accentSoft);
        if (i == 0) IconArrow(c, 26 * s, T.accent, true);
        if (i == 1) IconArrow(c, 26 * s, T.accent, false);
        if (i == 2) IconBank(c, 26 * s, T.accent);
        TextCenter(F_Semi, 13.5f * s, ImVec2(c.x, c.y + 33 * s), T.text, labels[i]);
        if (clicked && i == 0) { g_screen = SendPick; g_sendErr.clear(); }
        else if (clicked) { g_sheet = true; g_sheetTitle = labels[i]; }
    }
    y += 92 * s;

    // recent activity
    Text(F_Bold, 18 * s, ImVec2(x, y), T.text, "Recent activity");
    const char* all = "See all";
    ImVec2 asz = Measure(F_Semi, 14 * s, all);
    if (Hit("seeall", ImVec2(x + w - asz.x, y), ImVec2(x + w, y + asz.y + 4 * s))) g_screen = ActivityScr;
    TextRight(F_Semi, 14 * s, ImVec2(x + w, y + 3 * s), T.accent, all);
    y += 34 * s;
    int rows = (int)std::min<size_t>(st.activity.size(), 4);
    float listH = rows ? rows * 58 * s : 84 * s;
    float maxH = b.y - y - 8 * s;
    if (listH > maxH) { rows = (int)(maxH / (58 * s)); listH = rows * 58 * s; }
    DL()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + listH), T.card, 18 * s);
    if (st.activity.empty()) EmptyActivity(ImVec2(x, y), w);
    for (int i = 0; i < rows; i++) ActivityRow(st.activity[i], ImVec2(x, y + i * 58 * s), w, i + 1 < rows);
}

static void ScreenActivity(const MoneyState& st, ImVec2 a, ImVec2 b) {
    float x = a.x + 20 * s, w = b.x - a.x - 40 * s, y = a.y + 6 * s;
    Text(F_Bold, 28 * s, ImVec2(x, y), T.text, "Activity");
    y += 46 * s;
    long long in = 0, out = 0;
    for (auto& e : st.activity) (e.delta > 0 ? in : out) += e.delta;
    float half = (w - 10 * s) * 0.5f;
    for (int i = 0; i < 2; i++) {
        float bx = x + i * (half + 10 * s);
        DL()->AddRectFilled(ImVec2(bx, y), ImVec2(bx + half, y + 62 * s), T.card, 16 * s);
        Text(F_Reg, 12.5f * s, ImVec2(bx + 14 * s, y + 10 * s), T.sub, i == 0 ? "Money in" : "Money out");
        Text(F_Bold, 19 * s, ImVec2(bx + 14 * s, y + 28 * s), i == 0 ? T.accent : T.text, Money(i == 0 ? in : out, i == 0).c_str());
    }
    y += 76 * s;
    Text(F_Semi, 13 * s, ImVec2(x, y), T.sub, "ALL ACTIVITY ON THIS SAVE");
    y += 24 * s;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginChild("##list", ImVec2(w, b.y - y - 8 * s), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float listH = st.activity.empty() ? 84 * s : st.activity.size() * 58 * s;
    DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + listH), T.card, 18 * s);
    if (st.activity.empty()) EmptyActivity(p, w);
    for (size_t i = 0; i < st.activity.size(); i++) ActivityRow(st.activity[i], ImVec2(p.x, p.y + i * 58 * s), w, i + 1 < st.activity.size());
    ImGui::Dummy(ImVec2(w, listH));
    DragScroll();
    ImGui::EndChild();
}


// ---- small widgets ------------------------------------------------------------------
static bool Pill(const char* id, const char* label, ImVec2 a, ImVec2 b, bool filled) {
    bool c = Hit(id, a, b);
    bool hov = ImGui::IsItemHovered();
    DL()->AddRectFilled(a, b, filled ? (hov ? T.accentDark : T.accent) : (hov ? T.divider : T.outBg), (b.y - a.y) * 0.5f);
    ImVec2 sz = Measure(F_Semi, 14 * s, label);
    Text(F_Semi, 14 * s, ImVec2((a.x + b.x - sz.x) * 0.5f, (a.y + b.y - sz.y) * 0.5f), filled ? IM_COL32(255, 255, 255, 255) : T.text, label);
    return c;
}

// a destructive button: red text, or filled red when it's the final "yes"
static ImU32 kRedText() { return g_cfg.dark ? IM_COL32(255, 69, 58, 255) : IM_COL32(255, 59, 48, 255); }
static bool DangerPill(const char* id, const char* label, ImVec2 a, ImVec2 b, bool filled) {
    bool c = Hit(id, a, b);
    bool hov = ImGui::IsItemHovered();
    ImU32 red = kRedText();
    DL()->AddRectFilled(a, b, filled ? (hov ? IM_COL32(215, 40, 32, 255) : red) : (hov ? T.divider : T.outBg), (b.y - a.y) * 0.5f);
    ImVec2 sz = Measure(F_Semi, 14 * s, label);
    Text(F_Semi, 14 * s, ImVec2((a.x + b.x - sz.x) * 0.5f, (a.y + b.y - sz.y) * 0.5f), filled ? IM_COL32(255, 255, 255, 255) : red, label);
    return c;
}
static bool g_eraseAsk = false;   // Settings > Messages: "Erase all messages?" showing
#ifdef GITMO_TEST
static float g_msgSectionY = 0;
#endif

// iOS-style switch; p = its right edge, vertically centred
static bool Toggle(const char* id, ImVec2 p, bool on) {
    float w = 48 * s, h = 28 * s;
    ImVec2 a(p.x - w, p.y - h / 2), b(p.x, p.y + h / 2);
    bool c = Hit(id, a, b);
    DL()->AddRectFilled(a, b, on ? T.accent : T.divider, h / 2);
    DL()->AddCircleFilled(ImVec2(on ? b.x - h / 2 : a.x + h / 2, p.y), h / 2 - 3 * s, IM_COL32(255, 255, 255, 255));
    return c;
}

static void SettingRow(const char* label, const char* value, ImVec2 p, float w, bool divider) {
    Text(F_Reg, 15 * s, ImVec2(p.x + 16 * s, p.y + 15 * s), T.text, label);
    if (value) TextRight(F_Reg, 15 * s, ImVec2(p.x + w - 16 * s, p.y + 15 * s), T.sub, value);
    if (divider) DL()->AddLine(ImVec2(p.x + 16 * s, p.y + 50 * s), ImVec2(p.x + w - 16 * s, p.y + 50 * s), T.divider, 1.0f);
}

static void SectionTitle(ImVec2& p, const char* t) {
    Text(F_Semi, 13 * s, p, T.sub, t);
    p.y += 22 * s;
}

// ---- battery --------------------------------------------------------------------------
static bool g_dead;   // battery ran out: stays off until it has charged a little
static float g_uiBatt = 15.0f;   // this frame's battery minutes / state (read once per frame)
static int g_uiBattState;

static void BatteryIcon(ImVec2 lt, float w, float h, float level, bool charging, ImU32 col) {
    ImU32 red = Hex(0xFF3B30), green = Hex(0x34C759);
    ImU32 fill = level <= 0.2f && !charging ? red : charging ? green : col;
    float r = h * 0.28f;
    DL()->AddRect(lt, ImVec2(lt.x + w, lt.y + h), col, r, 0, std::max(1.0f, h * 0.11f));
    DL()->AddRectFilled(ImVec2(lt.x + w + h * 0.08f, lt.y + h * 0.32f), ImVec2(lt.x + w + h * 0.22f, lt.y + h * 0.68f), col, h * 0.06f);
    float inset = h * 0.18f;
    float fw = (w - 2 * inset) * std::min(std::max(level, 0.0f), 1.0f);
    if (fw > 0.5f) DL()->AddRectFilled(ImVec2(lt.x + inset, lt.y + inset), ImVec2(lt.x + inset + fw, lt.y + h - inset), fill, r * 0.6f);
    if (charging) {   // lightning bolt
        ImVec2 c(lt.x + w * 0.5f, lt.y + h * 0.5f);
        float k = h * 0.42f;
        ImVec2 bolt[6] = { ImVec2(c.x + k * 0.15f, c.y - k), ImVec2(c.x - k * 0.55f, c.y + k * 0.15f), ImVec2(c.x - k * 0.02f, c.y + k * 0.15f),
                           ImVec2(c.x - k * 0.15f, c.y + k), ImVec2(c.x + k * 0.55f, c.y - k * 0.15f), ImVec2(c.x + k * 0.02f, c.y - k * 0.15f) };
        DL()->AddConcavePolyFilled(bolt, 6, IM_COL32(255, 255, 255, 255));
        DL()->AddPolyline(bolt, 6, IM_COL32(0, 0, 0, 90), ImDrawFlags_Closed, std::max(1.0f, h * 0.06f));
    }
}

static void StatusBar(ImVec2 a, ImVec2 b, ImU32 col) {
    SYSTEMTIME t; GetLocalTime(&t);
    char tm[8]; sprintf_s(tm, "%d:%02d", t.wHour, t.wMinute);
    Text(F_Semi, 15 * s, ImVec2(a.x + 30 * s, a.y + 13 * s), col, tm);
    if (g_rad.state >= 1 && g_rad.state <= 3)   // the radio is on: a note after the clock
        IconNote(ImVec2(a.x + 30 * s + Measure(F_Semi, 15 * s, tm).x + 14 * s, a.y + 23 * s), 7 * s, col);
    float cx = (a.x + b.x) * 0.5f;
    DL()->AddRectFilled(ImVec2(cx - 48 * s, a.y + 10 * s), ImVec2(cx + 48 * s, a.y + 36 * s), Hex(0x000000), 13 * s);
    float level = g_uiBatt / kBatteryMinutes;
    bool charging = g_uiBattState == 1;
    float rx = b.x - 28 * s, y = a.y + 30 * s;
    BatteryIcon(ImVec2(rx - 25 * s, y - 13 * s), 23 * s, 11 * s, level, charging, col);
    char pct[8]; sprintf_s(pct, "%d%%", (int)(level * 100 + 0.5f));
    TextRight(F_Semi, 12.5f * s, ImVec2(rx - 29 * s, y - 15.5f * s), level <= 0.2f && !charging ? Hex(0xFF3B30) : col, pct);
    float px = rx - 33 * s - Measure(F_Semi, 12.5f * s, pct).x;
    for (int i = 0; i < 4; i++)
        DL()->AddRectFilled(ImVec2(px - 20 * s + i * 5 * s, y - (4 + i * 3) * s), ImVec2(px - 17 * s + i * 5 * s, y - 1 * s), col, 1 * s);
    // notifications: speaker with sound waves (sound), a buzzing phone (vibrate), speaker with a cross (off)
    ImVec2 sp(px - 40 * s, y - 7 * s);
    if (g_cfg.notifyMode == 1) {
        ImVec2 c(sp.x + 3 * s, sp.y);
        DL()->AddRect(ImVec2(c.x - 3 * s, c.y - 5.5f * s), ImVec2(c.x + 3 * s, c.y + 5.5f * s), col, 1.5f * s, 0, 1.4f * s);
        for (int side = -1; side <= 1; side += 2) {   // the buzz: a zigzag either side
            float x0 = c.x + side * 6 * s;
            ImVec2 z[4] = { ImVec2(x0, c.y - 4 * s), ImVec2(x0 + side * 1.6f * s, c.y - 1.3f * s), ImVec2(x0, c.y + 1.3f * s), ImVec2(x0 + side * 1.6f * s, c.y + 4 * s) };
            DL()->AddPolyline(z, 4, col, 0, 1.3f * s);
        }
        return;
    }
    DL()->AddRectFilled(ImVec2(sp.x - 5 * s, sp.y - 2.5f * s), ImVec2(sp.x - 2 * s, sp.y + 2.5f * s), col, 0.8f * s);
    DL()->AddTriangleFilled(ImVec2(sp.x - 2.5f * s, sp.y - 2.5f * s), ImVec2(sp.x + 2.5f * s, sp.y - 6 * s), ImVec2(sp.x - 2.5f * s, sp.y + 2.5f * s), col);
    DL()->AddTriangleFilled(ImVec2(sp.x - 2.5f * s, sp.y + 2.5f * s), ImVec2(sp.x + 2.5f * s, sp.y - 6 * s), ImVec2(sp.x + 2.5f * s, sp.y + 6 * s), col);
    if (g_cfg.notifyMode == 0) {
        DL()->PathArcTo(ImVec2(sp.x + 2.5f * s, sp.y), 4.5f * s, -0.9f, 0.9f, 10); DL()->PathStroke(col, 0, 1.4f * s);
        DL()->PathArcTo(ImVec2(sp.x + 2.5f * s, sp.y), 7.5f * s, -0.9f, 0.9f, 12); DL()->PathStroke(col, 0, 1.4f * s);
    } else {
        DL()->AddLine(ImVec2(sp.x + 5 * s, sp.y - 3.5f * s), ImVec2(sp.x + 11 * s, sp.y + 3.5f * s), col, 1.5f * s);
        DL()->AddLine(ImVec2(sp.x + 5 * s, sp.y + 3.5f * s), ImVec2(sp.x + 11 * s, sp.y - 3.5f * s), col, 1.5f * s);
    }
}

static void ScreenDead(ImVec2 a, ImVec2 b, bool charging) {
    DL()->AddRectFilled(a, b, Hex(0x000000), 34 * s);
    float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.45f;
    float w = 120 * s, h = 56 * s;
    ImU32 frame = Hex(0x48484A);
    float level = g_uiBatt / kBatteryMinutes;
    BatteryIcon(ImVec2(cx - w / 2, cy - h / 2), w, h, charging ? std::max(level, 0.03f) : 0.04f, charging, frame);
    if (!charging) DL()->AddRectFilled(ImVec2(cx - w / 2 + h * 0.18f, cy - h / 2 + h * 0.18f), ImVec2(cx - w / 2 + h * 0.18f + 7 * s, cy + h / 2 - h * 0.18f), Hex(0xFF3B30), 3 * s);
    if (charging) {
        TextCenter(F_Semi, 17 * s, ImVec2(cx, cy + h + 10 * s), IM_COL32(255, 255, 255, 230), "Charging...");
        char pct[16]; sprintf_s(pct, "%d%%", (int)(level * 100 + 0.5f));
        TextCenter(F_Reg, 14 * s, ImVec2(cx, cy + h + 36 * s), Hex(0x8E8E93), pct);
    } else {
        TextCenter(F_Semi, 17 * s, ImVec2(cx, cy + h + 10 * s), IM_COL32(255, 255, 255, 230), "Connect to charger");
        TextCenter(F_Reg, 13 * s, ImVec2(cx, cy + h + 36 * s), Hex(0x8E8E93), "Start the engine or go to the garage");
    }
}

// ---- cracked screen ---------------------------------------------------------------------
struct CrackSeg { float x0, y0, x1, y1, w; };
static std::vector<CrackSeg> g_cracks;   // in screen-width units, origin = top-left of the screen

static void BuildCracks(float aspect) {   // aspect = height / width
    g_cracks.clear();
    uint32_t seed = 0x6A17u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
    const float cx = 0.70f, cy = 0.30f * aspect, pi2 = 6.2831853f;
    std::function<void(float, float, float, float, float, int)> crack = [&](float x, float y, float ang, float len, float w, int depth) {
        float left = len;
        while (left > 0) {
            float step = 0.035f + rnd() * 0.05f;
            ang += (rnd() - 0.5f) * 0.5f;
            float nx = x + cosf(ang) * step, ny = y + sinf(ang) * step;
            g_cracks.push_back({ x, y, nx, ny, w });
            if (depth < 2 && rnd() < 0.16f) crack(nx, ny, ang + (rnd() < 0.5f ? -1 : 1) * (0.5f + rnd() * 0.5f), left * (0.3f + rnd() * 0.4f), w * 0.75f, depth + 1);
            x = nx; y = ny; left -= step; w *= 0.97f;
        }
    };
    for (int i = 0; i < 6; i++) crack(cx, cy, i * pi2 / 6 + rnd() * 0.6f, 0.25f + rnd() * 0.9f, 1.4f, 0);   // 0.9.1: 6 radials (was 11)
    for (float r : { 0.06f }) {   // a ring of short splinters round the impact (0.9.1: one ring of 9, was two of 14)
        int n = 9;
        for (int i = 0; i < n; i++) {
            if (rnd() < 0.3f) continue;
            float a0 = i * pi2 / n + rnd() * 0.1f, a1 = a0 + pi2 / n * (0.6f + rnd() * 0.35f);
            float r0 = r * (0.85f + rnd() * 0.3f), r1 = r * (0.85f + rnd() * 0.3f);
            g_cracks.push_back({ cx + cosf(a0) * r0, cy + sinf(a0) * r0, cx + cosf(a1) * r1, cy + sinf(a1) * r1, 1.0f });
        }
    }
}

static void DrawCracks(ImVec2 a, ImVec2 b) {
    // The foreground list is drawn after every window, including the scrolling lists
    // inside the apps, so the glass is always on top of everything on the phone.
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    float W = b.x - a.x;
    static float builtFor = -1;
    float aspect = (b.y - a.y) / W;
    if (fabsf(builtFor - aspect) > 0.001f) { BuildCracks(aspect); builtFor = aspect; }
    fg->PushClipRect(a, b, false);
    for (auto& c : g_cracks) {   // dark edge, then the bright fracture line
        ImVec2 p0(a.x + c.x0 * W, a.y + c.y0 * W), p1(a.x + c.x1 * W, a.y + c.y1 * W);
        fg->AddLine(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x + 1, p1.y + 1), IM_COL32(0, 0, 0, 38), c.w * 1.6f * s);   // 0.9.1: fainter (was 70 / 175)
        fg->AddLine(p0, p1, IM_COL32(255, 255, 255, 100), c.w * s);
    }
    ImVec2 imp(a.x + 0.70f * W, a.y + 0.30f * aspect * W);
    fg->AddCircleFilled(imp, 6 * s, IM_COL32(255, 255, 255, 35));
    fg->AddCircleFilled(imp, 2.5f * s, IM_COL32(255, 255, 255, 90));
    fg->PopClipRect();
}

// ---- Gitmo app: bottom tabs -----------------------------------------------------------------
static void HomePill(ImVec2 a, ImVec2 b, ImU32 col) {
    float cx = (a.x + b.x) * 0.5f;
    if (Hit("homepill", ImVec2(cx - 80 * s, b.y - 22 * s), ImVec2(cx + 80 * s, b.y))) { g_app = Drawer; g_sheet = false; g_captureKey = false; }
    bool hov = ImGui::IsItemHovered();
    DL()->AddRectFilled(ImVec2(cx - 55 * s, b.y - (hov ? 12 : 11) * s), ImVec2(cx + 55 * s, b.y - 6 * s), col, 3 * s);
}

static void NavBar(ImVec2 a, ImVec2 b) {
    DL()->AddRectFilled(a, b, T.card, 34 * s, ImDrawFlags_RoundCornersBottom);
    DL()->AddLine(a, ImVec2(b.x, a.y), T.divider, 1.0f);
    const char* names[2] = { "Home", "Activity" };
    float w = (b.x - a.x) / 2;
    for (int i = 0; i < 2; i++) {
        ImVec2 c(a.x + w * (i + 0.5f), a.y + 22 * s);
        if (Hit(names[i], ImVec2(a.x + w * i, a.y), ImVec2(a.x + w * (i + 1), b.y - 24 * s))) g_screen = (Screen)i;
        bool on = g_screen == i || (i == 0 && g_screen >= SendPick);
        ImU32 col = on ? T.accent : T.sub;
        if (i == 0) IconHome(c, 12 * s, col);
        if (i == 1) IconList(c, 12 * s, col);
        TextCenter(F_Semi, 11.5f * s, ImVec2(c.x, c.y + 15 * s), col, names[i]);
    }
    HomePill(a, b, T.text);
}

static void Sheet(ImVec2 a, ImVec2 b) {
    DL()->AddRectFilled(a, b, IM_COL32(0, 0, 0, 110), 34 * s);
    float h = 262 * s;
    ImVec2 p0(a.x, b.y - h), p1 = b;
    if (Hit("dim", a, ImVec2(b.x, p0.y))) g_sheet = false;   // tapping above the sheet also closes it
    DL()->AddRectFilled(p0, p1, T.card, 30 * s);
    DL()->AddRectFilled(ImVec2((a.x + b.x) * 0.5f - 20 * s, p0.y + 10 * s), ImVec2((a.x + b.x) * 0.5f + 20 * s, p0.y + 14 * s), T.divider, 2 * s);
    float cx = (a.x + b.x) * 0.5f, y = p0.y + 34 * s;
    DL()->AddCircleFilled(ImVec2(cx, y + 24 * s), 26 * s, T.accentSoft);
    bool bank = !strcmp(g_sheetTitle, "Bank");
    if (bank) IconBank(ImVec2(cx, y + 24 * s), 26 * s, T.accent);
    else IconArrow(ImVec2(cx, y + 24 * s), 26 * s, T.accent, !strcmp(g_sheetTitle, "Send"));
    y += 62 * s;
    TextCenter(F_Bold, 19 * s, ImVec2(cx, y), T.text, bank ? "Savings are coming" : "Requests are coming");
    y += 30 * s;
    TextCenter(F_Reg, 14 * s, ImVec2(cx, y), T.sub, bank ? "A savings account for your money" : "Asking a player for money");
    TextCenter(F_Reg, 14 * s, ImVec2(cx, y + 19 * s), T.sub, "arrives in a later update.");
    TextCenter(F_Reg, 14 * s, ImVec2(cx, y + 38 * s), T.sub, bank ? "" : "Until then, they can use Send.");
#ifdef GITMO_TEST
    extern ImVec2 g_testGotIt; g_testGotIt = ImVec2(cx, b.y - 44 * s);
#endif
    if (Pill("gotit", "Got it", ImVec2(a.x + 24 * s, b.y - 66 * s), ImVec2(b.x - 24 * s, b.y - 22 * s), true)) g_sheet = false;
}


// ---- transfers ------------------------------------------------------------------------------
static void Avatar(ImVec2 c, float r, const std::string& name, bool soft = true) {
    DL()->AddCircleFilled(c, r, soft ? T.accentSoft : T.accent);
    char ini[2] = { name.empty() ? '?' : (char)toupper((unsigned char)name[0]), 0 };
    ImVec2 sz = Measure(F_Bold, r * 0.95f, ini);
    Text(F_Bold, r * 0.95f, ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f), soft ? T.accent : IM_COL32(255, 255, 255, 255), ini);
}
static void IconCheck(ImVec2 c, float r, ImU32 col) {
    ImVec2 pts[3] = { ImVec2(c.x - r * 0.42f, c.y + r * 0.02f), ImVec2(c.x - r * 0.12f, c.y + r * 0.32f), ImVec2(c.x + r * 0.45f, c.y - r * 0.3f) };
    DL()->AddPolyline(pts, 3, col, 0, r * 0.16f);
}
static void IconCross(ImVec2 c, float r, ImU32 col) {
    float k = r * 0.32f;
    DL()->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, r * 0.15f);
    DL()->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, r * 0.15f);
}
static void Spinner(ImVec2 c, float r, ImU32 col) {
    float t = (float)ImGui::GetTime() * 5;
    DL()->PathArcTo(c, r, t, t + 4.2f, 32);
    DL()->PathStroke(col, 0, r * 0.16f);
}
static bool BackButton(const char* id, float x, float y) {
    bool c = Hit(id, ImVec2(x - 6 * s, y - 4 * s), ImVec2(x + 70 * s, y + 30 * s));
    ImU32 col = ImGui::IsItemHovered() ? T.accentDark : T.accent;
    ImVec2 pts[3] = { ImVec2(x + 9 * s, y + 4 * s), ImVec2(x + 1 * s, y + 13 * s), ImVec2(x + 9 * s, y + 22 * s) };
    DL()->AddPolyline(pts, 3, col, 0, 2.6f * s);
    Text(F_Semi, 15 * s, ImVec2(x + 15 * s, y + 3 * s), col, "Back");
    return c;
}
static const tx::Outgoing* FindOut(uint64_t id) {
    for (auto& o : g_tx.outs) if (o.id == id) return &o;
    return nullptr;
}

static void ScreenSendPick(ImVec2 a, ImVec2 b) {
    float x = a.x + 20 * s, w = b.x - a.x - 40 * s, y = a.y + 4 * s;
    if (BackButton("back_pick", x, y)) g_screen = Home;
    y += 36 * s;
    Text(F_Bold, 28 * s, ImVec2(x, y), T.text, "Send money");
    y += 40 * s;
    Text(F_Reg, 13.5f * s, ImVec2(x, y), T.sub, g_tx.ready ? g_tx.status.c_str() : "Waiting for Steam...");
    y += 30 * s;
    const auto& peers = g_tx.peers;
    if (peers.empty()) {
        DL()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 120 * s), T.card, 18 * s);
        TextCenter(F_Semi, 15 * s, ImVec2(x + w * 0.5f, y + 22 * s), T.text, "No one to send to yet");
        TextCenter(F_Reg, 13 * s, ImVec2(x + w * 0.5f, y + 48 * s), T.sub, g_cfg.sessionOnly ? "Players in your game session" : "Steam friends playing SnowRunner");
        TextCenter(F_Reg, 13 * s, ImVec2(x + w * 0.5f, y + 66 * s), T.sub, "show up here when they have");
        TextCenter(F_Reg, 13 * s, ImVec2(x + w * 0.5f, y + 84 * s), T.sub, "Gitmo too.");
        y += 134 * s;
    } else {
        float rh = 62 * s;
        int rows = (int)std::min<size_t>(peers.size(), (size_t)std::max(1.0f, (b.y - y - 70 * s) / rh));
        DL()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + rows * rh), T.card, 18 * s);
        for (int i = 0; i < rows; i++) {
            const tx::Peer& p = peers[i];
            ImVec2 r0(x, y + i * rh), r1(x + w, y + (i + 1) * rh);
            char id[32]; sprintf_s(id, "peer%d", i);
            if (Hit(id, r0, r1)) { g_sendPeer = p.id; g_sendName = p.name; g_amount.clear(); g_sendErr.clear(); g_screen = SendAmount; }
            if (ImGui::IsItemHovered()) DL()->AddRectFilled(r0, r1, T.outBg, 18 * s);
            Avatar(ImVec2(x + 34 * s, r0.y + rh * 0.5f), 20 * s, p.name);
            Text(F_Semi, 15.5f * s, ImVec2(x + 66 * s, r0.y + 12 * s), T.text, p.name.empty() ? "Player" : p.name.c_str());
            Text(F_Reg, 12.5f * s, ImVec2(x + 66 * s, r0.y + 33 * s), p.sameGame ? T.accent : T.sub, p.sameGame ? "In your game" : "Playing SnowRunner");
            ImVec2 ch(x + w - 22 * s, r0.y + rh * 0.5f);   // chevron
            ImVec2 cv[3] = { ImVec2(ch.x - 4 * s, ch.y - 7 * s), ImVec2(ch.x + 3 * s, ch.y), ImVec2(ch.x - 4 * s, ch.y + 7 * s) };
            DL()->AddPolyline(cv, 3, T.sub, 0, 2.0f * s);
            if (i + 1 < rows) DL()->AddLine(ImVec2(x + 66 * s, r1.y), ImVec2(x + w - 16 * s, r1.y), T.divider, 1.0f);
        }
        y += rows * rh + 14 * s;
    }
    // payments still being settled (e.g. the other player's game closed mid-transfer)
    int open = 0;
    for (auto& o : g_tx.outs) if (o.st == tx::Out::Paid || o.st == tx::Out::RefundDue) open++;
    if (open) {
        char t[96]; sprintf_s(t, "%d payment%s still settling. It finishes when", open, open == 1 ? "" : "s");
        Text(F_Reg, 12.5f * s, ImVec2(x, y), T.sub, t);
        Text(F_Reg, 12.5f * s, ImVec2(x, y + 17 * s), T.sub, "they're online again, or comes back to you.");
    }
}

static void ScreenSendAmount(const MoneyState& st, ImVec2 a, ImVec2 b) {
    float x = a.x + 20 * s, w = b.x - a.x - 40 * s, y = a.y + 4 * s;
    if (BackButton("back_amt", x, y)) { g_screen = SendPick; return; }
    float cx = (a.x + b.x) * 0.5f;
    y += 30 * s;
    Avatar(ImVec2(cx, y + 22 * s), 22 * s, g_sendName);
    y += 52 * s;
    std::string to = "To " + (g_sendName.empty() ? std::string("player") : g_sendName);
    TextCenter(F_Semi, 15 * s, ImVec2(cx, y), T.text, to.c_str());
    y += 30 * s;
    long long amt = g_amount.empty() ? 0 : _atoi64(g_amount.c_str());
    long long bal = Money_Balance();
    TextCenter(F_Bold, 46 * s, ImVec2(cx, y), amt ? T.text : T.sub, Money(amt).c_str());
    y += 60 * s;
    std::string info;
    ImU32 ic = T.sub;
    if (!Game_InGame()) info = "Load a save to send money";
    else if (bal < 0) info = st.live ? "Earn or spend any money first, then send" : "Load into a map to send money";
    else if (amt > bal) { info = "More than your balance (" + Money(bal) + ")"; ic = Hex(0xE5484D); }
    else if (amt > tx::kMaxAmount) { info = "Up to " + Money(tx::kMaxAmount) + " per transfer"; ic = Hex(0xE5484D); }
    else info = "Balance " + Money(bal);
    if (!g_sendErr.empty()) { info = g_sendErr; ic = Hex(0xE5484D); }
    TextCenter(F_Reg, 13 * s, ImVec2(cx, y), ic, info.c_str());
    y += 30 * s;

    // keypad
    static const char* keys[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "00", "0", "<" };
    float kw = w / 3, kh = std::min(52 * s, (b.y - y - 72 * s) / 4);
    for (int i = 0; i < 12; i++) {
        ImVec2 k0(x + (i % 3) * kw, y + (i / 3) * kh), k1(k0.x + kw, k0.y + kh);
        char id[16]; sprintf_s(id, "key%d", i);
        bool c = Hit(id, k0, k1);
        if (ImGui::IsItemHovered()) DL()->AddRectFilled(ImVec2(k0.x + 6 * s, k0.y + 3 * s), ImVec2(k1.x - 6 * s, k1.y - 3 * s), T.outBg, 12 * s);
        ImVec2 kc((k0.x + k1.x) * 0.5f, (k0.y + k1.y) * 0.5f);
        if (i == 11) {   // backspace
            ImVec2 pts[5] = { ImVec2(kc.x - 13 * s, kc.y), ImVec2(kc.x - 5 * s, kc.y - 8 * s), ImVec2(kc.x + 12 * s, kc.y - 8 * s),
                              ImVec2(kc.x + 12 * s, kc.y + 8 * s), ImVec2(kc.x - 5 * s, kc.y + 8 * s) };
            DL()->AddPolyline(pts, 5, T.text, ImDrawFlags_Closed, 2.0f * s);
            IconCross(ImVec2(kc.x + 3 * s, kc.y), 14 * s, T.text);
        } else {
            ImVec2 sz = Measure(F_Semi, 24 * s, keys[i]);
            Text(F_Semi, 24 * s, ImVec2(kc.x - sz.x * 0.5f, kc.y - sz.y * 0.5f), T.text, keys[i]);
        }
        if (c) {
            g_sendErr.clear();
            if (i == 11) { if (!g_amount.empty()) g_amount.pop_back(); }
            else if (g_amount.empty() && keys[i][0] == '0') {}   // no leading zeros
            else if (g_amount.size() + strlen(keys[i]) <= 9) g_amount += keys[i];
        }
    }
    y += 4 * kh + 10 * s;
    bool ok = Game_InGame() && amt > 0 && bal >= 0 && amt <= bal && amt <= tx::kMaxAmount;
    std::string label = amt ? "Send " + Money(amt) : std::string("Send");
    ImVec2 p0(x, std::min(y, b.y - 54 * s)), p1(x + w, p0.y + 46 * s);
    if (ok) {
        if (Pill("send", label.c_str(), p0, p1, true)) {
            uint64_t id = 0;
            switch (Tx_Send(g_sendPeer, amt, &id)) {
            case tx::SendResult::Ok: g_sendId = id; g_screen = SendStatus; break;
            case tx::SendResult::Offline: g_sendErr = (g_sendName.empty() ? std::string("They") : g_sendName) + " just went offline"; break;
            case tx::SendResult::Busy: g_sendErr = "Wait until your last transfer is answered"; break;
            case tx::SendResult::NotEnough: g_sendErr = "More than your balance"; break;
            case tx::SendResult::NotInMap: g_sendErr = Game_InGame() ? "Load into a map to send money" : "Load a save to send money"; break;
            case tx::SendResult::NotInGame: g_sendErr = (g_sendName.empty() ? std::string("They") : g_sendName) + " left your game"; break;
            case tx::SendResult::InGarage: g_sendErr = "Leave the garage to send money"; break;
            default: g_sendErr = "That amount can't be sent"; break;
            }
        }
    } else {
        DL()->AddRectFilled(p0, p1, T.outBg, (p1.y - p0.y) * 0.5f);
        ImVec2 sz = Measure(F_Semi, 14 * s, label.c_str());
        Text(F_Semi, 14 * s, ImVec2((p0.x + p1.x - sz.x) * 0.5f, (p0.y + p1.y - sz.y) * 0.5f), T.sub, label.c_str());
    }
}

// One big icon, a title, the amount and a line or two of explanation.
static void StatusCard(ImVec2 a, ImVec2 b, int icon /*0 spinner, 1 check, 2 cross, 3 clock*/, const std::string& title,
                       long long amount, const std::string& l1, const std::string& l2) {
    float cx = (a.x + b.x) * 0.5f, y = a.y + 70 * s;
    ImVec2 c(cx, y + 40 * s);
    bool bad = icon == 2;
    DL()->AddCircleFilled(c, 40 * s, bad ? T.outBg : T.accentSoft);
    if (icon == 0) Spinner(c, 22 * s, T.accent);
    if (icon == 1) IconCheck(c, 44 * s, T.accent);
    if (icon == 2) IconCross(c, 44 * s, T.text);
    if (icon == 3) {
        DL()->AddCircle(c, 20 * s, T.accent, 0, 3 * s);
        DL()->AddLine(c, ImVec2(c.x, c.y - 12 * s), T.accent, 3 * s);
        DL()->AddLine(c, ImVec2(c.x + 9 * s, c.y + 5 * s), T.accent, 3 * s);
    }
    y += 104 * s;
    TextCenter(F_Bold, 21 * s, ImVec2(cx, y), T.text, title.c_str());
    y += 34 * s;
    if (amount) { TextCenter(F_Bold, 38 * s, ImVec2(cx, y), bad ? T.sub : T.text, Money(amount).c_str()); y += 54 * s; }
    if (!l1.empty()) TextCenter(F_Reg, 13.5f * s, ImVec2(cx, y), T.sub, l1.c_str());
    if (!l2.empty()) TextCenter(F_Reg, 13.5f * s, ImVec2(cx, y + 19 * s), T.sub, l2.c_str());
}

static void ScreenSendStatus(ImVec2 a, ImVec2 b) {
    float x = a.x + 20 * s, w = b.x - a.x - 40 * s;
    const tx::Outgoing* o = FindOut(g_sendId);
    ImVec2 p0(x, b.y - 58 * s), p1(x + w, b.y - 12 * s);
    if (!o) {   // long finished and forgotten
        StatusCard(a, b, 1, "Finished", 0, "See Activity for what moved.", "");
        if (Pill("done0", "Done", p0, p1, true)) g_screen = Home;
        return;
    }
    std::string n = o->name.empty() ? "the other player" : o->name;
    uint64_t now = GetTickCount64();
    bool cancellable = false;
    switch (o->st) {
    case tx::Out::Offered:
        StatusCard(a, b, 0, "Contacting " + n, o->amount, "Nothing has left your balance.", "");
        cancellable = true; break;
    case tx::Out::Asked: {
        int left = (int)(((long long)tx::kPromptTtl - (long long)(now - o->created)) / 1000);
        StatusCard(a, b, 3, "Waiting for " + n, o->amount, "They need to accept it first.",
                   left > 0 ? "They have " + std::to_string(left) + " s to answer." : "Any moment now...");
        cancellable = true; break;
    }
    case tx::Out::Paid:
        StatusCard(a, b, 0, "Sending...", o->amount, n + " accepted. The money has left", "your balance and is on its way.");
        break;
    case tx::Out::RefundDue:
        StatusCard(a, b, 2, "Didn't go through", o->amount, "Your money comes back as soon", "as you're in a map.");
        break;
    case tx::Out::Done:
        StatusCard(a, b, 1, "Sent to " + n, o->amount, "It's in their balance now.", "");
        break;
    case tx::Out::Refunded:
        StatusCard(a, b, 2, "Didn't go through", o->amount, "The money came back to you.", "");
        break;
    default: {
        const char* t = o->st == tx::Out::Declined ? "Declined" : o->st == tx::Out::Expired ? "No answer" :
                        o->st == tx::Out::Canceled ? "Canceled" : o->st == tx::Out::Unreachable ? "Couldn't reach them" : "Didn't go through";
        std::string why = o->note;
        if (o->st == tx::Out::Canceled || o->st == tx::Out::Expired || o->st == tx::Out::Unreachable) why.clear();
        StatusCard(a, b, 2, t, o->amount, why.empty() ? "Nothing left your balance." : why, why.empty() ? "" : "Nothing left your balance.");
        break;
    }
    }
    if (cancellable) {
        if (Pill("cancelsend", "Cancel", p0, p1, false)) Tx_Cancel(o->id);
    } else if (o->st != tx::Out::Paid) {
        if (Pill("donesend", "Done", p0, p1, true)) g_screen = Home;
    } else {
        TextCenter(F_Reg, 12.5f * s, ImVec2((a.x + b.x) * 0.5f, p0.y + 14 * s), T.sub, "You can close the phone; it finishes by itself.");
    }
}

// The incoming request (or how one ended) sits on top of whatever the phone shows.
static const tx::Incoming* IncomingToShow() {
    for (auto& i : g_tx.ins) if (i.st == tx::In::Prompt) return &i;
    for (auto& i : g_tx.ins) if (i.st == tx::In::Accepted) return &i;
    for (auto& i : g_tx.ins) if (!i.seen && (i.st == tx::In::Credited || i.st == tx::In::Void)) return &i;
    return nullptr;
}

static void IncomingSheet(const tx::Incoming& in, ImVec2 a, ImVec2 b) {
    DL()->AddRectFilled(a, b, IM_COL32(0, 0, 0, 120), 34 * s);
    float h = 360 * s;
    ImVec2 p0(a.x, b.y - h), p1 = b;
    DL()->AddRectFilled(p0, p1, T.card, 30 * s);
    DL()->AddRectFilled(ImVec2((a.x + b.x) * 0.5f - 20 * s, p0.y + 10 * s), ImVec2((a.x + b.x) * 0.5f + 20 * s, p0.y + 14 * s), T.divider, 2 * s);
    float cx = (a.x + b.x) * 0.5f, y = p0.y + 34 * s;
    std::string n = in.name.empty() ? "A player" : in.name;
    ImVec2 bl(a.x + 24 * s, b.y - 66 * s), br(b.x - 24 * s, b.y - 22 * s);
    if (in.st == tx::In::Prompt) {
        Avatar(ImVec2(cx, y + 26 * s), 26 * s, n, false);
        y += 66 * s;
        TextCenter(F_Semi, 16 * s, ImVec2(cx, y), T.text, (n + " wants to send you").c_str());
        y += 28 * s;
        TextCenter(F_Bold, 42 * s, ImVec2(cx, y), T.accent, Money(in.amount).c_str());
        y += 60 * s;
        int left = (int)(((long long)tx::kPromptTtl - (long long)(GetTickCount64() - in.created)) / 1000);
        TextCenter(F_Reg, 13 * s, ImVec2(cx, y), T.sub, "Nothing moves until you accept.");
        char t[64]; sprintf_s(t, "Answer within %d s", std::max(left, 0));
        TextCenter(F_Reg, 13 * s, ImVec2(cx, y + 19 * s), T.sub, t);
        float mid = (bl.x + br.x) * 0.5f;
        if (Pill("decline", "Decline", bl, ImVec2(mid - 6 * s, br.y), false)) Tx_Decline(in.peer, in.id);
        if (Pill("accept", "Accept", ImVec2(mid + 6 * s, bl.y), br, true)) Tx_Accept(in.peer, in.id);
        return;
    }
    if (in.st == tx::In::Accepted) {
        DL()->AddCircleFilled(ImVec2(cx, y + 26 * s), 26 * s, T.accentSoft);
        Spinner(ImVec2(cx, y + 26 * s), 15 * s, T.accent);
        y += 66 * s;
        TextCenter(F_Semi, 16 * s, ImVec2(cx, y), T.text, "Receiving...");
        y += 28 * s;
        TextCenter(F_Bold, 42 * s, ImVec2(cx, y), T.accent, Money(in.amount).c_str());
        y += 60 * s;
        TextCenter(F_Reg, 13 * s, ImVec2(cx, y), T.sub, ("from " + n).c_str());
        return;
    }
    bool got = in.st == tx::In::Credited;
    DL()->AddCircleFilled(ImVec2(cx, y + 26 * s), 26 * s, got ? T.accentSoft : T.outBg);
    if (got) IconCheck(ImVec2(cx, y + 26 * s), 30 * s, T.accent); else IconCross(ImVec2(cx, y + 26 * s), 30 * s, T.text);
    y += 66 * s;
    bool canceled = !got && in.why.size() >= 9 && in.why.compare(in.why.size() - 9, 9, " canceled") == 0;
    bool timedOut = !got && in.why == "No answer";
    TextCenter(F_Semi, 16 * s, ImVec2(cx, y), T.text, got ? "Money received" : canceled ? "Transfer canceled" : "Transfer called off");
    y += 28 * s;
    TextCenter(F_Bold, 42 * s, ImVec2(cx, y), got ? T.accent : T.sub, Money(in.amount, got).c_str());
    y += 60 * s;
    std::string l1 = got ? "from " + n : canceled ? n + " canceled it before it was sent." : timedOut ? "It ran out of time." : n + " canceled it, or it ran out of time.";
    TextCenter(F_Reg, 13 * s, ImVec2(cx, y), T.sub, l1.c_str());
    if (!got) TextCenter(F_Reg, 13 * s, ImVec2(cx, y + 19 * s), T.sub, "Nothing changed.");
    if (Pill("okin", "OK", bl, br, true)) Tx_MarkSeen(in.peer, in.id);
}

#ifdef GITMO_TEST
// Fake transfers for screenshot phases 9-14 and the receive-overlay regression at 46.
static TxView DemoTx(int phase) {
    TxView v; v.ready = true;
    uint64_t now = GetTickCount64();
    tx::Peer bob; bob.id = 2; bob.name = "Bob"; bob.lastSeen = now; bob.sameGame = true; bob.hasGitmo = true;
    tx::Peer cat; cat.id = 3; cat.name = "Cat Driver"; cat.lastSeen = now; cat.hasGitmo = true;
    v.peers = { bob, cat };
    v.status = "2 players with Gitmo online";
    tx::Outgoing o; o.peer = 2; o.id = 0x11; o.amount = 2500; o.name = "Bob"; o.created = now - 12000;
    if (phase == 11) { o.st = tx::Out::Asked; v.outs.push_back(o); }
    if (phase == 13) { o.st = tx::Out::Done; v.outs.push_back(o); }
    tx::Incoming i; i.peer = 2; i.id = 0x22; i.amount = 4000; i.name = "Bob"; i.created = now - 9000;
    if (phase == 12) { i.st = tx::In::Prompt; v.ins.push_back(i); }
    if (phase == 14 || phase == 46) { i.st = tx::In::Credited; i.seen = false; v.ins.push_back(i); }
    if (phase == 44) { i.st = tx::In::Void; i.seen = false; i.why = "Bob canceled"; v.ins.push_back(i); }
    return v;
}
#endif


// ---- SMS --------------------------------------------------------------------------------------
// Contacts: everyone with Gitmo who is online now (green dot) plus everyone written with before
// (washed out while offline). Conversations look like a phone's messages app.
static SmsView g_smsView;
static uint64_t g_chat;                    // open conversation (0 = contact list)
static uint64_t g_viewSent;                // conversation the SMS engine has been told is open
static char g_draft[sms::kMaxText + 1];
static std::string g_smsSendErr;
static size_t g_chatShown = (size_t)-1;    // messages drawn last frame: scroll to the end when it changes
static bool g_focusDraft;
static const ImU32 kBlue = IM_COL32(10, 132, 255, 255), kGreen = IM_COL32(52, 199, 89, 255);

static ImU32 Fade(ImU32 c, float a) {
    int al = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)al << IM_COL32_A_SHIFT);
}
static SYSTEMTIME LocalFromUnix(long long t) {
    ULARGE_INTEGER u; u.QuadPart = (ULONGLONG)(t * 10000000LL + 116444736000000000LL);
    FILETIME ft{ u.LowPart, u.HighPart }, lf; SYSTEMTIME st{};
    FileTimeToLocalFileTime(&ft, &lf); FileTimeToSystemTime(&lf, &st);
    return st;
}
static std::string ListTime(long long t) {
    if (t <= 0) return "";
    SYSTEMTIME st = LocalFromUnix(t);
    std::string d = DayLabel(st);
    if (d != "Today") return d;
    char b[16]; sprintf_s(b, "%02d:%02d", st.wHour, st.wMinute);
    return b;
}
static std::string SepTime(long long t) {
    SYSTEMTIME st = LocalFromUnix(t);
    char b[64]; sprintf_s(b, "%s %02d:%02d", DayLabel(st).c_str(), st.wHour, st.wMinute);
    return b;
}
// shortened with an ellipsis to fit maxW (never splitting a UTF-8 character)
static std::string Fit(ImFont* f, float size, const std::string& t, float maxW) {
    if (Measure(f, size, t.c_str()).x <= maxW) return t;
    std::string s = t;
    const char* ell = "\xE2\x80\xA6";
    while (!s.empty()) {
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
        if (!s.empty()) s.pop_back();
        if (Measure(f, size, (s + ell).c_str()).x <= maxW) break;
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s + ell;
}

// Steam avatars become textures once Steam has them; until then, the initial.
struct AvTex { void* srv = nullptr; };
static std::map<uint64_t, AvTex> g_avTex;
static void* AvatarTex(uint64_t id) {
    AvTex& t = g_avTex[id];
    if (t.srv) return t.srv;
#ifdef GITMO_TEST
    if (id == 2) {   // a stand-in picture for the screenshots
        static uint8_t px[64 * 64 * 4];
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            uint8_t* q = px + (y * 64 + x) * 4;
            bool head = (x - 32) * (x - 32) + (y - 26) * (y - 26) < 150, body = (x - 32) * (x - 32) + (y - 66) * (y - 66) < 700;
            q[0] = head || body ? 250 : (uint8_t)(40 + x * 2); q[1] = head || body ? 220 : (uint8_t)(90 + y); q[2] = head || body ? 190 : 200; q[3] = 255;
        }
        t.srv = Overlay_MakeTexture(px, 64, 64);
        return t.srv;
    }
#endif
    AvatarImg img;
    if (Avatar_Get(id, img) && !img.rgba.empty()) t.srv = Overlay_MakeTexture(img.rgba.data(), img.w, img.h);
    return t.srv;
}
static void ContactAvatar(ImVec2 c, float r, uint64_t id, const std::string& name, float alpha) {
    if (void* tex = AvatarTex(id)) {
        DL()->AddImageRounded(ImTextureRef((ImTextureID)tex), ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(0, 0), ImVec2(1, 1),
                              IM_COL32(255, 255, 255, (int)(255 * alpha)), r);
        return;
    }
    DL()->AddCircleFilled(c, r, Fade(g_cfg.dark ? Hex(0x3A3F4A) : Hex(0xC7CBD3), alpha));
    char ini[2] = { name.empty() ? '?' : (char)toupper((unsigned char)name[0]), 0 };
    ImVec2 sz = Measure(F_Semi, r * 0.95f, ini);
    Text(F_Semi, r * 0.95f, ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f), IM_COL32(255, 255, 255, (int)(255 * alpha)), ini);
}
static void OnlineDot(ImVec2 c, float r, ImU32 ring) {
    DL()->AddCircleFilled(c, r * 1.4f, ring);
    DL()->AddCircleFilled(c, r, kGreen);
}
static std::string PresenceText(const sms::Contact& c) {
    if (c.online) return c.inGame ? "In your game" : "Online";
    if (c.lastSeenWall) return "Last seen " + SepTime(c.lastSeenWall);
    return "Offline";
}
static const sms::Contact* FindContact(uint64_t id) {
    for (auto& c : g_smsView.contacts) if (c.id == id) return &c;
    return nullptr;
}
static void OpenChat(uint64_t id) {
    g_chat = id; g_chatShown = (size_t)-1; g_draft[0] = 0; g_focusDraft = true; g_smsSendErr.clear();
}

static void ScreenSmsList(ImVec2 a, ImVec2 b) {
    float x = a.x + 20 * s, y = a.y + 4 * s;
    Text(F_Bold, 30 * s, ImVec2(x, y), T.text, "SMS");
    y += 42 * s;
    std::vector<const sms::Contact*> v;
    for (auto& c : g_smsView.contacts) v.push_back(&c);
    auto last = [](const sms::Contact* c) { return c->msgs.empty() ? 0ULL : c->msgs.back().key ? c->msgs.back().key : (unsigned long long)c->msgs.back().wall * 1000; };
    std::sort(v.begin(), v.end(), [&](const sms::Contact* p, const sms::Contact* q) {
        if (p->online != q->online) return p->online;
        if (last(p) != last(q)) return last(p) > last(q);
        return _stricmp(p->name.c_str(), q->name.c_str()) < 0;
    });
    int on = 0;
    for (auto* c : v) if (c->online) on++;
    char sub[64];
    if (!g_smsView.ready) sprintf_s(sub, "Waiting for Steam...");
    else if (on) sprintf_s(sub, "%d online", on);
    else sprintf_s(sub, "No one online right now");
    Text(F_Reg, 13.5f * s, ImVec2(x, y), T.sub, sub);
    y += 28 * s;
    float w = b.x - a.x - 40 * s;
    if (v.empty()) {
        DL()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 104 * s), T.card, 18 * s);
        TextCenter(F_Semi, 15 * s, ImVec2(x + w * 0.5f, y + 22 * s), T.text, "No contacts yet");
        TextCenter(F_Reg, 13 * s, ImVec2(x + w * 0.5f, y + 48 * s), T.sub, "Players with Gitmo show up here");
        TextCenter(F_Reg, 13 * s, ImVec2(x + w * 0.5f, y + 66 * s), T.sub, "while they're online.");
        return;
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, y));
    ImGui::BeginChild("##smslist", ImVec2(b.x - a.x, b.y - y - 28 * s), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float rh = 70 * s;
    for (size_t i = 0; i < v.size(); i++) {
        const sms::Contact& c = *v[i];
        float al = c.online ? 1.0f : 0.45f;   // offline contacts are washed out
        ImVec2 r0(a.x + 8 * s, p.y + i * rh), r1(b.x - 8 * s, p.y + (i + 1) * rh);
        char id[32]; sprintf_s(id, "ct%llu", (unsigned long long)c.id);
        if (Hit(id, r0, r1)) OpenChat(c.id);
        if (ImGui::IsItemHovered()) DL()->AddRectFilled(r0, r1, T.outBg, 14 * s);
        ImVec2 ac(r0.x + 37 * s, r0.y + rh * 0.5f);
        ContactAvatar(ac, 25 * s, c.id, c.name, al);
        if (c.online) OnlineDot(ImVec2(ac.x + 18 * s, ac.y + 18 * s), 5.5f * s, T.bg);
        float tx = ac.x + 39 * s, right = r1.x - 10 * s;
        std::string when = c.msgs.empty() ? "" : ListTime(c.msgs.back().wall);
        float tw = when.empty() ? 0 : Measure(F_Reg, 12.5f * s, when.c_str()).x + 10 * s;
        if (!when.empty()) TextRight(F_Reg, 12.5f * s, ImVec2(right, r0.y + 15 * s), Fade(T.sub, al), when.c_str());
        Text(F_Semi, 16 * s, ImVec2(tx, r0.y + 12 * s), Fade(T.text, al), Fit(F_Semi, 16 * s, c.name.empty() ? "Player" : c.name, right - tw - tx).c_str());
        std::string prev = c.msgs.empty() ? PresenceText(c)
                         : std::string(c.msgs.back().mine ? "You: " : "") + (c.msgs.back().photo ? "\xF0\x9F\x93\xB7 Photo" : c.msgs.back().text);
        float bw = c.unread ? 30 * s : 0;
        ImU32 pc = c.unread ? T.text : T.sub;
        Text(c.unread ? F_Semi : F_Reg, 13.5f * s, ImVec2(tx, r0.y + 37 * s), Fade(pc, al), Fit(F_Reg, 13.5f * s, prev, right - bw - tx).c_str());
        if (c.unread) {
            ImVec2 bc(right - 10 * s, r0.y + 46 * s);
            char nb[8]; sprintf_s(nb, "%d", std::min(c.unread, 99));
            float bwid = std::max(20 * s, Measure(F_Bold, 12 * s, nb).x + 12 * s);
            DL()->AddRectFilled(ImVec2(bc.x - bwid * 0.5f, bc.y - 10 * s), ImVec2(bc.x + bwid * 0.5f, bc.y + 10 * s), kBlue, 10 * s);
            ImVec2 sz = Measure(F_Bold, 12 * s, nb);
            Text(F_Bold, 12 * s, ImVec2(bc.x - sz.x * 0.5f, bc.y - sz.y * 0.5f), IM_COL32(255, 255, 255, 255), nb);
        }
        if (i + 1 < v.size()) DL()->AddLine(ImVec2(tx, r1.y), ImVec2(right, r1.y), T.divider, 1.0f);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(b.x - a.x, v.size() * rh + 10 * s));
    DragScroll();
    ImGui::EndChild();
}

static void ArrowUp(ImVec2 c, float r, ImU32 col) {
    DL()->AddLine(ImVec2(c.x, c.y + r * 0.55f), ImVec2(c.x, c.y - r * 0.55f), col, r * 0.2f);
    ImVec2 pts[3] = { ImVec2(c.x - r * 0.45f, c.y - r * 0.1f), ImVec2(c.x, c.y - r * 0.58f), ImVec2(c.x + r * 0.45f, c.y - r * 0.1f) };
    DL()->AddPolyline(pts, 3, col, 0, r * 0.2f);
}

// ---- emoji panel (the smiley button in the typing bar) ------------------------------------
// Like a phone's emoji keyboard: sections you scroll through, tabs along the bottom to jump.
// Single code points only - no skin tones or joined sequences, since the font draws one glyph
// per character (in one colour). Any the installed Windows emoji font lacks are left out.
struct EmojiCat { const char* name; uint32_t icon; std::vector<uint32_t> list; };
static std::vector<EmojiCat> g_emoji;
static bool g_emojiOpen;
static int g_emojiJump = -1;               // tab tapped: scroll to that section next frame
static int g_draftCursor, g_wantCursor = -1;   // the text field's cursor (bytes), and where to put it back

static int Utf8(uint32_t cp, char* o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}
static std::string U8(uint32_t cp) { char b[5]; int n = Utf8(cp, b); return std::string(b, n); }

static std::vector<uint32_t> RecentEmoji() {
    std::vector<uint32_t> v;
    for (const char* p = g_cfg.recentEmoji.c_str(); *p;) {
        char* e; unsigned long cp = strtoul(p, &e, 16);
        if (e == p) break;
        if (cp) v.push_back((uint32_t)cp);
        p = *e == ',' ? e + 1 : e;
    }
    return v;
}
static void UseEmoji(uint32_t cp) {   // recently used: newest first, 24 kept (three rows)
    auto v = RecentEmoji();
    v.erase(std::remove(v.begin(), v.end(), cp), v.end());
    v.insert(v.begin(), cp);
    if (v.size() > 24) v.resize(24);
    std::string s;
    for (auto x : v) { char b[12]; sprintf_s(b, "%s%X", s.empty() ? "" : ",", x); s += b; }
    SaveRecentEmoji(s);
}

static void EmojiInit() {
    if (!g_emoji.empty()) return;
    g_emoji = {
        { "RECENTLY USED", 0x1F558, {} },
        { "SMILEYS", 0x1F600, { 0x1F600, 0x1F603, 0x1F604, 0x1F601, 0x1F606, 0x1F605, 0x1F602, 0x1F923, 0x1F60A, 0x1F607, 0x1F642, 0x1F643,
            0x1F609, 0x1F60C, 0x1F60D, 0x1F970, 0x1F618, 0x1F60B, 0x1F61B, 0x1F61C, 0x1F92A, 0x1F61D, 0x1F911, 0x1F917, 0x1F92D, 0x1F92B,
            0x1F914, 0x1F910, 0x1F928, 0x1F610, 0x1F611, 0x1F636, 0x1F60F, 0x1F612, 0x1F644, 0x1F62C, 0x1F925, 0x1F634, 0x1F924, 0x1F62A,
            0x1F637, 0x1F912, 0x1F915, 0x1F922, 0x1F92E, 0x1F927, 0x1F975, 0x1F976, 0x1F974, 0x1F635, 0x1F92F, 0x1F920, 0x1F973, 0x1F60E,
            0x1F913, 0x1F9D0, 0x1F615, 0x1F61F, 0x1F641, 0x1F62E, 0x1F62F, 0x1F632, 0x1F633, 0x1F97A, 0x1F626, 0x1F627, 0x1F628, 0x1F630,
            0x1F625, 0x1F622, 0x1F62D, 0x1F631, 0x1F616, 0x1F623, 0x1F61E, 0x1F613, 0x1F629, 0x1F62B, 0x1F971, 0x1F624, 0x1F621, 0x1F620,
            0x1F92C, 0x1F608, 0x1F47F, 0x1F480, 0x1F4A9, 0x1F921, 0x1F47B, 0x1F47D, 0x1F916 } },
        { "PEOPLE", 0x1F44B, { 0x1F44B, 0x1F91A, 0x270B, 0x1F596, 0x1F44C, 0x1F90F, 0x270C, 0x1F91E, 0x1F91F, 0x1F918, 0x1F919, 0x1F448,
            0x1F449, 0x1F446, 0x1F447, 0x261D, 0x1F44D, 0x1F44E, 0x270A, 0x1F44A, 0x1F91B, 0x1F91C, 0x1F44F, 0x1F64C, 0x1F450, 0x1F932,
            0x1F91D, 0x1F64F, 0x1F4AA, 0x1F9BE, 0x1F440, 0x1F445, 0x1F444, 0x1F9E0, 0x1F937, 0x1F926, 0x1F64B, 0x1F646, 0x1F645, 0x1F483,
            0x1F57A, 0x1F46A } },
        { "HEARTS & SYMBOLS", 0x2764, { 0x2764, 0x1F9E1, 0x1F49B, 0x1F49A, 0x1F499, 0x1F49C, 0x1F5A4, 0x1F90D, 0x1F90E, 0x1F494, 0x2763,
            0x1F495, 0x1F49E, 0x1F493, 0x1F497, 0x1F496, 0x1F498, 0x1F49D, 0x1F4AF, 0x1F4A2, 0x1F4A5, 0x1F4AB, 0x1F4A6, 0x1F4A8, 0x1F525,
            0x2728, 0x2B50, 0x1F31F, 0x26A1, 0x2705, 0x274C, 0x2757, 0x2753, 0x26A0, 0x1F6D1, 0x26D4, 0x1F534, 0x1F7E2, 0x1F7E1, 0x1F535,
            0x2B06, 0x2B07, 0x27A1, 0x2B05, 0x1F504, 0x1F197, 0x1F198, 0x1F192 } },
        { "TRUCKS & TRAVEL", 0x1F69A, { 0x1F69A, 0x1F69B, 0x1F69C, 0x1F6FB, 0x1F697, 0x1F699, 0x1F690, 0x1F3CE, 0x1F691, 0x1F692, 0x1F693,
            0x1F68C, 0x1F68D, 0x1F3CD, 0x1F6F5, 0x1F6B2, 0x26FD, 0x1F6A7, 0x1F6DE, 0x2693, 0x1F681, 0x2708, 0x1F680, 0x1F6F6, 0x26F5,
            0x1F6A2, 0x1F682, 0x1F686, 0x1F5FA, 0x1F9ED, 0x26F0, 0x1F3D4, 0x1F3D5, 0x1F309, 0x1F3ED, 0x1F3E0, 0x1F3D7, 0x1F6A6, 0x1F6A5,
            0x1F3C1, 0x1F6A9 } },
        { "NATURE & WEATHER", 0x1F332, { 0x2600, 0x1F324, 0x26C5, 0x1F327, 0x26C8, 0x1F329, 0x1F328, 0x2744, 0x2603, 0x26C4, 0x1F32A, 0x1F32B,
            0x1F308, 0x2614, 0x1F4A7, 0x1F30A, 0x1F30D, 0x1F319, 0x1F332, 0x1F333, 0x1F334, 0x1F335, 0x1F33F, 0x1F340, 0x1F341, 0x1F342,
            0x1F343, 0x1F344, 0x1F33E, 0x1F43B, 0x1F98C, 0x1F43A, 0x1F98A, 0x1F417, 0x1F404, 0x1F40E, 0x1F415, 0x1F408, 0x1F41F, 0x1F985,
            0x1F426, 0x1F40D, 0x1F98E, 0x1F422, 0x1F41D } },
        { "FOOD & DRINK", 0x1F354, { 0x2615, 0x1F37A, 0x1F37B, 0x1F943, 0x1F377, 0x1F964, 0x1F354, 0x1F355, 0x1F32D, 0x1F32E, 0x1F35F, 0x1F96A,
            0x1F969, 0x1F357, 0x1F953, 0x1F373, 0x1F95E, 0x1F369, 0x1F36A, 0x1F382, 0x1F370, 0x1F36B, 0x1F37F, 0x1F34E, 0x1F34C, 0x1F349,
            0x1F347, 0x1F353, 0x1F352, 0x1F951, 0x1F955, 0x1F33D, 0x1F954 } },
        { "OBJECTS", 0x1F4A1, { 0x1F4B0, 0x1F4B5, 0x1F4B8, 0x1F4B3, 0x1FA99, 0x1F3E6, 0x1F4F1, 0x1F4DE, 0x260E, 0x1F527, 0x1F528, 0x26CF,
            0x1F6E0, 0x1F529, 0x2699, 0x1F9F0, 0x1FA9B, 0x1F50B, 0x1F50C, 0x1F4A1, 0x1F526, 0x1F5DD, 0x1F511, 0x1F3AE, 0x1F3A7, 0x1F4F7,
            0x23F0, 0x231B, 0x1F389, 0x1F38A, 0x1F381, 0x1F3C6, 0x1F947, 0x1F3AF, 0x1F3B2, 0x1F4E6, 0x1F4CD, 0x1F4CC, 0x1F4DD, 0x1F4E3,
            0x1F514, 0x1F4A4, 0x26FA } },
    };
    int kept = 0, dropped = 0;
    for (auto& c : g_emoji) {
        std::vector<uint32_t> ok;
        for (uint32_t cp : c.list) { if (F_Reg->IsGlyphInFont((ImWchar)cp)) { ok.push_back(cp); kept++; } else dropped++; }
        c.list.swap(ok);
        if (!F_Reg->IsGlyphInFont((ImWchar)c.icon)) c.icon = c.list.empty() ? '?' : c.list[0];
    }
    Log("sms: emoji panel has %d emoji (%d not in this PC's emoji font)", kept, dropped);
}

// put an emoji into the draft where the cursor was
static void InsertEmoji(uint32_t cp) {
    char u[5]; int n = Utf8(cp, u);
    size_t len = strlen(g_draft), at = std::min((size_t)std::max(g_draftCursor, 0), len);
    while (at > 0 && at < len && ((unsigned char)g_draft[at] & 0xC0) == 0x80) at--;   // never inside a character
    if (len + n > sms::kMaxText) return;
    memmove(g_draft + at + n, g_draft + at, len - at + 1);
    memcpy(g_draft + at, u, n);
    g_draftCursor = (int)(at + n);
    g_wantCursor = g_draftCursor;
    g_focusDraft = true;   // back into the text field, so typing carries on after it
    UseEmoji(cp);
}

static int DraftCallback(ImGuiInputTextCallbackData* d) {
    if (g_wantCursor >= 0) { d->CursorPos = d->SelectionStart = d->SelectionEnd = std::min(g_wantCursor, d->BufTextLen); g_wantCursor = -1; }
    g_draftCursor = d->CursorPos;
    return 0;
}

// smiley (panel closed) or keyboard (panel open), like a phone's emoji key
static void EmojiKeyIcon(ImVec2 c, float r, ImU32 col, bool keyboard) {
    float th = 1.6f * s;
    if (!keyboard) {
        DL()->AddCircle(c, r, col, 0, th);
        DL()->AddCircleFilled(ImVec2(c.x - r * 0.36f, c.y - r * 0.28f), r * 0.13f, col);
        DL()->AddCircleFilled(ImVec2(c.x + r * 0.36f, c.y - r * 0.28f), r * 0.13f, col);
        DL()->PathArcTo(ImVec2(c.x, c.y + r * 0.02f), r * 0.55f, 0.18f * 3.14159265f, 0.82f * 3.14159265f, 12);
        DL()->PathStroke(col, 0, th);
        return;
    }
    ImVec2 k0(c.x - r * 1.05f, c.y - r * 0.7f), k1(c.x + r * 1.05f, c.y + r * 0.7f);
    DL()->AddRect(k0, k1, col, r * 0.22f, 0, th);
    for (int row = 0; row < 2; row++)
        for (int i = 0; i < 4; i++)
            DL()->AddRectFilled(ImVec2(k0.x + r * (0.3f + i * 0.42f), k0.y + r * (0.3f + row * 0.36f)),
                                ImVec2(k0.x + r * (0.5f + i * 0.42f), k0.y + r * (0.48f + row * 0.36f)), col, 1.0f * s);
    DL()->AddRectFilled(ImVec2(c.x - r * 0.5f, k1.y - r * 0.4f), ImVec2(c.x + r * 0.5f, k1.y - r * 0.24f), col, 1.0f * s);
}

// The emoji / photo panel's background. The panels end 14*s above the screen's bottom edge
// (b), but their colour runs on to that edge with the screen's own rounded corners, so nothing
// is drawn past the curve (a square bottom used to stick out over the phone's frame).
static void PanelBg(ImVec2 a, ImVec2 b) {
    DL()->AddRectFilled(a, ImVec2(b.x, b.y + 14 * s), g_cfg.dark ? Hex(0x1C1C1E) : Hex(0xE3E5EA), 34 * s, ImDrawFlags_RoundCornersBottom);
    DL()->AddLine(a, ImVec2(b.x, a.y), T.divider, 1.0f);
}

static void EmojiPanel(ImVec2 a, ImVec2 b) {   // a..b = the panel's area
    EmojiInit();
    g_emoji[0].list = RecentEmoji();
    float W = b.x - a.x;
    PanelBg(a, b);
    float tabH = 36 * s, gy1 = b.y - tabH;
    int cols = 8;
    float pad = 8 * s, cell = (W - 2 * pad) / cols, es = 24 * s;

    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + 1));
    ImGui::BeginChild("##emoji", ImVec2(W, gy1 - a.y - 1), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float y = p0.y + 6 * s;
    float secY[16] = {};
    static bool dragging; static float dragFromY, dragFromScroll;
    ImGuiIO& io = ImGui::GetIO();
    for (int k = 0; k < (int)g_emoji.size() && k < 16; k++) {
        const EmojiCat& cat = g_emoji[k];
        secY[k] = y - p0.y;
        if (cat.list.empty()) continue;
        Text(F_Semi, 11 * s, ImVec2(a.x + pad + 4 * s, y), T.sub, cat.name);
        y += 20 * s;
        int rows = ((int)cat.list.size() + cols - 1) / cols;
        ImVec2 g0(a.x + pad, y), g1(a.x + pad + cols * cell, y + rows * cell);
#ifdef GITMO_TEST
        { extern ImVec2 g_testEmojiCell; extern int g_testFrame; static int setFor = -1; if (setFor != g_testFrame) { setFor = g_testFrame; g_testEmojiCell = ImVec2(g0.x + cell * 0.5f, g0.y + cell * 0.5f); } }   // first section's first emoji
#endif
        // one button for the whole grid: a tap picks the cell under it, a drag scrolls
        char id[16]; sprintf_s(id, "emo%d", k);
        bool live = !g_behindSheet;
        if (live) Hit(id, g0, g1);
        bool hov = live && ImGui::IsItemHovered(), act = live && ImGui::IsItemActive();
        if (live && ImGui::IsItemActivated()) { dragging = false; dragFromY = io.MousePos.y; dragFromScroll = ImGui::GetScrollY(); }
        if (act && fabsf(io.MousePos.y - dragFromY) > 6 * s) dragging = true;
        if (act && dragging) ImGui::SetScrollY(dragFromScroll - (io.MousePos.y - dragFromY));
        int under = -1;
        if (hov || act) {
            int cx = (int)((io.MousePos.x - g0.x) / cell), cy = (int)((io.MousePos.y - g0.y) / cell);
            if (cx >= 0 && cx < cols && cy >= 0 && cy < rows && cy * cols + cx < (int)cat.list.size()) under = cy * cols + cx;
        }
        if (live && ImGui::IsItemDeactivated() && !dragging && hov && under >= 0) InsertEmoji(cat.list[under]);
        for (int i = 0; i < (int)cat.list.size(); i++) {
            ImVec2 c0(g0.x + (i % cols) * cell, g0.y + (i / cols) * cell);
            if (c0.y + cell < a.y || c0.y > gy1) continue;   // off screen
            if (i == under && !dragging) DL()->AddRectFilled(ImVec2(c0.x + 2 * s, c0.y + 2 * s), ImVec2(c0.x + cell - 2 * s, c0.y + cell - 2 * s), Fade(T.text, act ? 0.16f : 0.08f), 8 * s);
            std::string e = U8(cat.list[i]);
            ImVec2 sz = Measure(F_Reg, es, e.c_str());
            Text(F_Reg, es, ImVec2(c0.x + (cell - sz.x) * 0.5f, c0.y + (cell - sz.y) * 0.5f), T.text, e.c_str());
        }
        y = g1.y + 10 * s;
    }
    ImGui::SetCursorScreenPos(p0);   // (no recent ones yet: that section and its tab stay empty)
    ImGui::Dummy(ImVec2(W, y - p0.y));
    float scroll = ImGui::GetScrollY();
    if (g_emojiJump >= 0 && g_emojiJump < (int)g_emoji.size()) { ImGui::SetScrollY(secY[g_emojiJump]); scroll = secY[g_emojiJump]; g_emojiJump = -1; }
    ImGui::EndChild();

    // tabs: one per section; the one being shown is highlighted
    int cur = 0;
    for (int k = 0; k < (int)g_emoji.size() && k < 16; k++) if (!g_emoji[k].list.empty() && secY[k] <= scroll + 8 * s) cur = k;
    int n = (int)g_emoji.size();
    float tw = (W - 2 * pad) / n;
    for (int k = 0; k < n; k++) {
        ImVec2 t0(a.x + pad + k * tw, gy1 + 3 * s), t1(t0.x + tw, b.y - 3 * s);
        bool empty = g_emoji[k].list.empty();
        char id[16]; sprintf_s(id, "emotab%d", k);
        if (Hit(id, t0, t1) && !empty) g_emojiJump = k;
        if (k == cur) DL()->AddRectFilled(ImVec2(t0.x + 3 * s, t0.y), ImVec2(t1.x - 3 * s, t1.y), Fade(T.text, 0.10f), 8 * s);
        std::string e = U8(g_emoji[k].icon);
        ImVec2 sz = Measure(F_Reg, 17 * s, e.c_str());
        Text(F_Reg, 17 * s, ImVec2((t0.x + t1.x - sz.x) * 0.5f, (t0.y + t1.y - sz.y) * 0.5f), Fade(k == cur ? T.text : T.sub, empty ? 0.35f : 1.0f), e.c_str());
    }
}

// ---- photos in SMS -------------------------------------------------------------------------
// The camera key (left of the emoji key) opens a panel: "Take photo" (the game's view as it is
// right now, without the phone) and the player's recent Steam screenshots. Either becomes a
// preview to send or cancel. Photos show in the chat as pictures; tap one to see it big.
enum { PhNone, PhPanel, PhWorking, PhPreview, PhViewer };
static int g_photoUi = PhNone;
static ULONGLONG g_photoErrUntil, g_workSince;
static ULONGLONG g_photoSendErrUntil;
struct PicTex { void* srv = nullptr; int w = 0, h = 0; ULONGLONG used = 0, retryAt = 0; bool failed = false, asked = false; };
static std::map<std::string, PicTex> g_pics;   // "m<id>": a photo message; "t<path>": a screenshot thumbnail
static std::vector<std::wstring> g_shots;
static std::string g_prevJpeg;
static PicTex g_prevTex;
static std::string g_viewKey;                  // the photo shown big

static std::string Utf8Of(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// once a frame: finished decodes become textures; old ones are let go
static void PicsPump() {
    std::string key; bool ok = false; PhotoImg img;
    while (Photo_DecodedNext(key, ok, img)) {
        PicTex& t = g_pics[key];
        if (ok && img.w > 0) {
            if (t.srv) Overlay_FreeTexture(t.srv);
            t.srv = Overlay_MakeTexture(img.rgba.data(), img.w, img.h);
            t.w = img.w; t.h = img.h;
        }
        t.failed = !ok || !t.srv;
    }
    while (g_pics.size() > 60) {
        auto old = g_pics.begin();
        for (auto it = g_pics.begin(); it != g_pics.end(); ++it) if (it->second.used < old->second.used) old = it;
        if (old->second.srv) Overlay_FreeTexture(old->second.srv);
        g_pics.erase(old);
    }
}
static PicTex* Pic(const std::string& key, const std::wstring& path, int maxSide) {
    PicTex& t = g_pics[key];
    t.used = GetTickCount64();
    ULONGLONG now = t.used;
    if (!t.srv && !t.failed && (!t.asked || now >= t.retryAt)) {
        t.asked = Photo_DecodeAsync(key, path, maxSide);
        t.retryAt = now + (t.asked ? 3000 : 1000);
    }
    return &t;
}
static void ClosePreview() {
    if (g_prevTex.srv) Overlay_FreeTexture(g_prevTex.srv);
    g_prevTex = PicTex{};
    g_prevJpeg.clear();
    g_photoSendErrUntil = 0;
}
static void ClosePhotos() {   // leaving the chat, closing the phone
    if (g_photoUi == PhWorking) Photo_Cancel();
    ClosePreview();
    g_photoUi = PhNone;
}

static void CameraIcon(ImVec2 c, float r, ImU32 col) {
    float th = 1.6f * s;
    ImVec2 a0(c.x - r * 1.05f, c.y - r * 0.55f), a1(c.x + r * 1.05f, c.y + r * 0.78f);
    DL()->AddRect(a0, a1, col, r * 0.25f, 0, th);
    DL()->AddRectFilled(ImVec2(c.x - r * 0.42f, a0.y - r * 0.32f), ImVec2(c.x + r * 0.42f, a0.y + th * 0.5f), col, r * 0.12f);
    DL()->AddCircle(ImVec2(c.x, c.y + r * 0.12f), r * 0.38f, col, 0, th);
}

// a picture in a box: cover = fill it (cropping), otherwise fit it (whole picture, centred)
static void DrawPic(const PicTex& t, ImVec2 a, ImVec2 b, float round, bool cover, int alpha = 255) {
    if (!t.srv || t.w <= 0 || t.h <= 0) return;
    float bw = b.x - a.x, bh = b.y - a.y, ia = (float)t.w / t.h, ba = bw / bh;
    ImVec2 uv0(0, 0), uv1(1, 1), p0 = a, p1 = b;
    if (cover) {
        if (ia > ba) { float k = ba / ia; uv0.x = (1 - k) * 0.5f; uv1.x = 1 - uv0.x; }
        else { float k = ia / ba; uv0.y = (1 - k) * 0.5f; uv1.y = 1 - uv0.y; }
    } else {
        float k = std::min(bw / t.w, bh / t.h), w = t.w * k, h = t.h * k;
        p0 = ImVec2(a.x + (bw - w) * 0.5f, a.y + (bh - h) * 0.5f); p1 = ImVec2(p0.x + w, p0.y + h);
    }
    DL()->AddImageRounded(ImTextureRef((ImTextureID)t.srv), p0, p1, uv0, uv1, IM_COL32(255, 255, 255, alpha), round);
}

static void PhotoPanel(ImVec2 a, ImVec2 b) {
    float W = b.x - a.x;
    PanelBg(a, b);
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + 1));
    ImGui::BeginChild("##photos", ImVec2(W, b.y - a.y - 1), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float pad = 8 * s, gap = 4 * s, y = p0.y + 8 * s;
    if (GetTickCount64() < g_photoErrUntil) {
        TextCenter(F_Semi, 12 * s, ImVec2((a.x + b.x) * 0.5f, y), kRedText(), "Couldn't make the photo - try again");
        y += 20 * s;
    }
    int cols = 3;
    float cell = (W - 2 * pad - (cols - 1) * gap) / cols;
    int n = 1 + (int)g_shots.size();
    for (int i = 0; i < n; i++) {
        ImVec2 c0(a.x + pad + (i % cols) * (cell + gap), y + (i / cols) * (cell + gap)), c1(c0.x + cell, c0.y + cell);
        char id[24]; sprintf_s(id, "photo%d", i);
        bool click = Hit(id, c0, c1);
        bool hov = !g_behindSheet && ImGui::IsItemHovered();
        if (i == 0) {   // "Take photo": the game's view right now, without the phone
            DL()->AddRectFilled(c0, c1, hov ? Fade(kBlue, 0.85f) : kBlue, 8 * s);
            CameraIcon(ImVec2((c0.x + c1.x) * 0.5f, c0.y + cell * 0.40f), cell * 0.17f, IM_COL32(255, 255, 255, 255));
            TextCenter(F_Semi, 12.5f * s, ImVec2((c0.x + c1.x) * 0.5f, c0.y + cell * 0.66f), IM_COL32(255, 255, 255, 255), "Take photo");
#ifdef GITMO_TEST
            { extern ImVec2 g_testTakePhoto; g_testTakePhoto = ImVec2((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f); }
#endif
            if (click) { Overlay_RequestCapture(); g_photoUi = PhWorking; g_workSince = GetTickCount64(); }
            continue;
        }
        const std::wstring& shot = g_shots[i - 1];
        bool visible = c1.y >= a.y && c0.y <= b.y;
        PicTex* t = visible ? Pic("t" + Utf8Of(shot), Photo_SteamThumb(shot), 240) : nullptr;
        if (t && t->srv) DrawPic(*t, c0, c1, 8 * s, true, hov ? 210 : 255);
        else DL()->AddRectFilled(c0, c1, Fade(T.text, 0.08f), 8 * s);
        if (click) { Photo_FromFile(shot); g_photoUi = PhWorking; g_workSince = GetTickCount64(); }
    }
    y += ((n + cols - 1) / cols) * (cell + gap) + 6 * s;
    bool steam = Steam_Available();
    if (g_shots.empty()) {
        TextCenter(F_Reg, 11.5f * s, ImVec2((a.x + b.x) * 0.5f, y), T.sub, steam ? "Screenshots you take in SnowRunner" : "Photos you take with the phone");
        TextCenter(F_Reg, 11.5f * s, ImVec2((a.x + b.x) * 0.5f, y + 16 * s), T.sub, steam ? "with Steam (F12) show up here." : "show up here (Pictures\\Gitmo).");
        y += 36 * s;
    } else {
        TextCenter(F_Reg, 11 * s, ImVec2((a.x + b.x) * 0.5f, y), T.sub, steam ? "Your Steam screenshots" : "Your photos (Pictures\\Gitmo)");
        y += 22 * s;
    }
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(W, y - p0.y));
    DragScroll();
    ImGui::EndChild();
}

// the whole chat area while a photo is being made, previewed, or looked at
static void PhotoScreen(ImVec2 a, ImVec2 b, const sms::Contact* c) {
    float cx = (a.x + b.x) * 0.5f;
    // black to the screen's edges (its bottom corners are rounded like the phone's)
    DL()->AddRectFilled(ImVec2(a.x - 2, a.y), ImVec2(b.x + 2, b.y), IM_COL32(0, 0, 0, 255), 34 * s, ImDrawFlags_RoundCornersBottom);
    ImU32 white = IM_COL32(255, 255, 255, 255), grey = IM_COL32(170, 170, 176, 255);
    if (g_photoUi == PhWorking) {
        int st = Photo_State();
        if (st == 2) {
            std::string j; PhotoImg prev;
            if (Photo_Take(j, prev)) {
                ClosePreview();
                g_prevJpeg.swap(j);
                g_prevTex.srv = Overlay_MakeTexture(prev.rgba.data(), prev.w, prev.h);
                g_prevTex.w = prev.w; g_prevTex.h = prev.h;
                g_photoUi = g_prevTex.srv ? PhPreview : PhPanel;
            }
            return;
        }
        if (st == 3 || GetTickCount64() - g_workSince > 15000) { Photo_Cancel(); g_photoUi = PhPanel; g_photoErrUntil = GetTickCount64() + 4000; return; }
        float t = (float)(GetTickCount64() % 1000) / 1000.0f * 6.2831853f, cy = (a.y + b.y) * 0.45f;
        DL()->PathArcTo(ImVec2(cx, cy), 16 * s, t, t + 4.4f, 24);
        DL()->PathStroke(white, 0, 3 * s);
        TextCenter(F_Semi, 14 * s, ImVec2(cx, cy + 30 * s), white, "Getting the photo ready...");
        if (Pill("photocancel1", "Cancel", ImVec2(cx - 50 * s, b.y - 70 * s), ImVec2(cx + 50 * s, b.y - 36 * s), false)) { Photo_Cancel(); g_photoUi = PhPanel; }
        return;
    }
    if (g_photoUi == PhPreview) {
        std::string who = c->name.empty() ? "Player" : c->name;
        std::string title = "Send to " + Fit(F_Semi, 14 * s, who, (b.x - a.x) * 0.6f) + "?";
        TextCenter(F_Semi, 14 * s, ImVec2(cx, a.y + 10 * s), white, title.c_str());
        DrawPic(g_prevTex, ImVec2(a.x + 8 * s, a.y + 36 * s), ImVec2(b.x - 8 * s, b.y - 96 * s), 10 * s, false);
        char kb[32]; sprintf_s(kb, "%zu KB", g_prevJpeg.size() / 1024);
        bool sendFailed = GetTickCount64() < g_photoSendErrUntil;
        TextCenter(F_Reg, 11 * s, ImVec2(cx, b.y - 90 * s), sendFailed ? kRedText() : grey,
                   sendFailed ? "Couldn't send - try again" : kb);
        float bw = (b.x - a.x - 48 * s) / 2, by = b.y - 70 * s;
        if (Pill("photocancel2", "Cancel", ImVec2(a.x + 16 * s, by), ImVec2(a.x + 16 * s + bw, by + 36 * s), false)) { ClosePreview(); g_photoUi = PhPanel; }
        ImVec2 s0(b.x - 16 * s - bw, by), s1(b.x - 16 * s, by + 36 * s);
        bool send = Hit("photosend", s0, s1);
        DL()->AddRectFilled(s0, s1, !g_behindSheet && ImGui::IsItemHovered() ? Fade(kBlue, 0.85f) : kBlue, 18 * s);
        TextCenter(F_Semi, 14 * s, ImVec2((s0.x + s1.x) * 0.5f, s0.y + 9 * s), white, "Send");
        if (send) {
            if (Sms_SendPhoto(c->id, g_prevJpeg)) {
                ClosePreview(); g_photoSendErrUntil = 0; g_photoUi = PhNone; g_chatShown = (size_t)-1;
            } else {
                g_photoSendErrUntil = GetTickCount64() + 4000;
            }
        }
        return;
    }
    // PhViewer: the photo big; a tap anywhere closes it
    auto it = g_pics.find(g_viewKey);
    if (it != g_pics.end()) { it->second.used = GetTickCount64(); DrawPic(it->second, ImVec2(a.x, a.y + 4 * s), ImVec2(b.x, b.y - 40 * s), 0, false); }
    TextCenter(F_Reg, 11.5f * s, ImVec2(cx, b.y - 32 * s), grey, "Tap to close");
    if (Hit("photoviewclose", a, b)) g_photoUi = PhNone;
}

static void RadioOpenShared(const std::string& name, const std::string& url);   // ui_radio.inc

// A station shared from the Radio app: "📻 Listening to <song> on <station> <link>".
// The sender writes a line break before the link, but SMS sends one line, so it arrives as a
// space: the link is the last word. Gives the text before the link, the station's name (as far
// as it can tell) and the link.
static bool ParseRadioShare(const std::string& t, std::string& text, std::string& name, std::string& url) {
    static const char kRadio[] = "\xF0\x9F\x93\xBB";
    if (t.compare(0, 4, kRadio) != 0) return false;
    std::string s = t;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t sp = s.find_last_of(" \n");
    if (sp == std::string::npos || sp < 4) return false;
    url = s.substr(sp + 1);
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) return false;
    if (url.size() < 12) return false;
    text = s.substr(0, sp);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) text.pop_back();
    std::string what = text.substr(4);
    if (what.compare(0, 14, " Listening to ") == 0) what = what.substr(14);
    size_t on = what.rfind(" on ");
    name = on == std::string::npos ? what : what.substr(on + 4);
    return true;
}

static void ScreenSmsChat(ImVec2 a, ImVec2 b) {
    const sms::Contact* c = FindContact(g_chat);
    if (!c) { g_chat = 0; g_emojiOpen = false; ClosePhotos(); return; }
    if (g_viewSent != g_chat) { Sms_Viewing(g_chat); g_viewSent = g_chat; }
    if (g_photoUi >= PhWorking) { PhotoScreen(a, b, c); return; }
    float W = b.x - a.x, cx = (a.x + b.x) * 0.5f, x = a.x + 16 * s;
    std::string name = c->name.empty() ? "Player" : c->name;
    // header: back, picture, name, presence
    bool back = Hit("smsback", ImVec2(x - 6 * s, a.y), ImVec2(x + 70 * s, a.y + 40 * s));
    ImU32 bc = ImGui::IsItemHovered() ? Fade(kBlue, 0.7f) : kBlue;
    ImVec2 ch[3] = { ImVec2(x + 9 * s, a.y + 8 * s), ImVec2(x + 1 * s, a.y + 17 * s), ImVec2(x + 9 * s, a.y + 26 * s) };
    DL()->AddPolyline(ch, 3, bc, 0, 2.6f * s);
    Text(F_Reg, 15.5f * s, ImVec2(x + 15 * s, a.y + 7 * s), bc, "SMS");
    ContactAvatar(ImVec2(cx, a.y + 20 * s), 19 * s, c->id, name, 1.0f);
    if (c->online) OnlineDot(ImVec2(cx + 14 * s, a.y + 34 * s), 4.5f * s, T.bg);
    TextCenter(F_Semi, 13 * s, ImVec2(cx, a.y + 42 * s), T.text, Fit(F_Semi, 13 * s, name, W * 0.5f).c_str());
    TextCenter(F_Reg, 11.5f * s, ImVec2(cx, a.y + 60 * s), c->online ? kGreen : T.sub, PresenceText(*c).c_str());
    float top = a.y + 80 * s;
    DL()->AddLine(ImVec2(a.x, top), ImVec2(b.x, top), T.divider, 1.0f);
    if (back) { g_chat = 0; g_emojiOpen = false; ClosePhotos(); return; }

    // messages, then the typing bar, then (when open) the emoji panel below it
    bool panel = g_emojiOpen || g_photoUi == PhPanel;
    float barH = 56 * s, panelH = panel ? 250 * s : 0;
    float by0 = panel ? b.y - 14 * s - panelH - barH : b.y - 26 * s - barH;
    float sendErrH = g_smsSendErr.empty() ? 0.0f : 18 * s;
    float messagesBottom = by0 - sendErrH;
    ImGui::SetCursorScreenPos(ImVec2(a.x, top + 1));
    ImGui::BeginChild("##smsmsgs", ImVec2(W, messagesBottom - top - 1), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float y = p0.y + 10 * s, fs = 15 * s, padX = 12 * s, padY = 7 * s, maxBub = W * 0.72f;
    const auto& msgs = c->msgs;
    int lastMine = -1;
    for (int i = 0; i < (int)msgs.size(); i++) if (msgs[i].mine) lastMine = i;
    long long prevWall = 0; int prevMine = -1;
    ImU32 theirBg = g_cfg.dark ? Hex(0x26262A) : Hex(0xE9E9EB);
    for (int i = 0; i < (int)msgs.size(); i++) {
        const sms::Msg& m = msgs[i];
        if (i == 0 || m.wall - prevWall > 15 * 60) {   // a time line after a pause
            y += i ? 10 * s : 0;
            TextCenter(F_Semi, 11.5f * s, ImVec2(cx, y), T.sub, SepTime(m.wall).c_str());
            y += 22 * s; prevMine = -1;
        } else y += prevMine == (int)m.mine ? 3 * s : 10 * s;
        bool queued = m.mine && m.st == sms::St::Waiting && !c->online;
        if (m.photo) {   // the picture itself, rounded; tap to see it big
            std::string key = "m" + std::to_string(m.id);
            PicTex* t = Pic(key, Sms_PhotoPath(m.id), 640);
            float mw = W * 0.62f, mh = 230 * s, pw = mw, ph = mw * 0.62f;
            if (t->srv && t->w > 0) { float k = std::min(mw / t->w, mh / t->h); pw = t->w * k; ph = t->h * k; }
            float bx = m.mine ? b.x - 12 * s - pw : a.x + 12 * s;
            ImVec2 q0(bx, y), q1(bx + pw, y + ph);
            if (t->srv) DrawPic(*t, q0, q1, 14 * s, true, queued ? 140 : 255);
            else {
                DL()->AddRectFilled(q0, q1, theirBg, 14 * s);
                CameraIcon(ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f - 6 * s), 12 * s, T.sub);
                TextCenter(F_Reg, 11 * s, ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f + 12 * s), T.sub, t->failed ? "Photo not found" : "Photo");
            }
            char pid[32]; sprintf_s(pid, "pic%llx", (unsigned long long)m.id);
            if (Hit(pid, q0, q1) && t->srv) { g_viewKey = key; g_photoUi = PhViewer; }
            y += ph;
        } else if (std::string shText, shName, shUrl; ParseRadioShare(m.text, shText, shName, shUrl)) {
            // a shared station: the message, then its link (underlined); tap to play it in Radio
            float inner = maxBub - 2 * padX, lfs = 13.5f * s;
            ImVec2 ts = F_Reg->CalcTextSizeA(fs, FLT_MAX, inner, shText.c_str());
            std::string link = Fit(F_Reg, lfs, shUrl, inner);
            ImVec2 ls = Measure(F_Reg, lfs, link.c_str());
            float bw = std::max(ts.x, ls.x) + 2 * padX, bh = ts.y + 5 * s + ls.y + 2 * padY;
            float bx = m.mine ? b.x - 12 * s - bw : a.x + 12 * s;
            char lid[32]; sprintf_s(lid, "share%llx", (unsigned long long)m.id);
            bool open = Hit(lid, ImVec2(bx, y), ImVec2(bx + bw, y + bh));
            bool hov = !g_behindSheet && ImGui::IsItemHovered();
            ImU32 bg = m.mine ? (queued ? Fade(kBlue, 0.5f) : kBlue) : theirBg;
            if (hov) bg = m.mine ? Fade(kBlue, 0.85f) : (g_cfg.dark ? Hex(0x323238) : Hex(0xDEDEE1));
            DL()->AddRectFilled(ImVec2(bx, y), ImVec2(bx + bw, y + bh), bg, std::min(17 * s, bh * 0.5f));
            ImU32 tc = m.mine ? IM_COL32(255, 255, 255, 255) : T.text, lc = m.mine ? IM_COL32(255, 255, 255, 235) : kBlue;
            DL()->AddText(F_Reg, fs, ImVec2(bx + padX, y + padY), tc, shText.c_str(), nullptr, inner);
            float ly = y + padY + ts.y + 5 * s;
            Text(F_Reg, lfs, ImVec2(bx + padX, ly), lc, link.c_str());
            DL()->AddLine(ImVec2(bx + padX, ly + ls.y), ImVec2(bx + padX + ls.x, ly + ls.y), lc, 1.0f * s);
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (open) RadioOpenShared(shName, shUrl);
            y += bh;
        } else {
            ImVec2 ts = F_Reg->CalcTextSizeA(fs, FLT_MAX, maxBub - 2 * padX, m.text.c_str());
            float bw = ts.x + 2 * padX, bh = ts.y + 2 * padY;
            float bx = m.mine ? b.x - 12 * s - bw : a.x + 12 * s;
            ImU32 bg = m.mine ? (queued ? Fade(kBlue, 0.5f) : kBlue) : theirBg;
            DL()->AddRectFilled(ImVec2(bx, y), ImVec2(bx + bw, y + bh), bg, std::min(17 * s, bh * 0.5f));
            DL()->AddText(F_Reg, fs, ImVec2(bx + padX, y + padY), m.mine ? IM_COL32(255, 255, 255, 255) : T.text, m.text.c_str(), nullptr, maxBub - 2 * padX);
            y += bh;
        }
        if (i == lastMine) {
            const char* st = m.st == sms::St::Read ? "Read" : m.st == sms::St::Delivered ? "Delivered"
                           : c->online ? "Sending..." : "Waiting - sends when they're online";
            TextRight(F_Reg, 11 * s, ImVec2(b.x - 14 * s, y + 3 * s), T.sub, st);
            y += 19 * s;
        }
        prevWall = m.wall; prevMine = m.mine;
    }
    if (g_smsView.receiving.count(c->id)) {   // a photo from them on its way in
        y += 10 * s;
        const char* t = "\xF0\x9F\x93\xB7 Receiving a photo...";
        ImVec2 ts = Measure(F_Reg, 13 * s, t);
        DL()->AddRectFilled(ImVec2(a.x + 12 * s, y), ImVec2(a.x + 12 * s + ts.x + 2 * padX, y + ts.y + 2 * padY), theirBg, 16 * s);
        Text(F_Reg, 13 * s, ImVec2(a.x + 12 * s + padX, y + padY), T.sub, t);
        y += ts.y + 2 * padY;
    }
    if (msgs.empty()) {
        TextCenter(F_Reg, 13.5f * s, ImVec2(cx, y + 30 * s), T.sub, c->online ? "No messages yet. Say hi!" : "No messages yet.");
        y += 60 * s;
    }
    y += 12 * s;
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(W, y - p0.y));
    if (g_chatShown != msgs.size()) { ImGui::SetScrollHereY(1.0f); g_chatShown = msgs.size(); }
    DragScroll();
    ImGui::EndChild();
    if (!g_smsSendErr.empty())
        TextCenter(F_Reg, 11.5f * s, ImVec2(cx, messagesBottom + 2 * s), kRedText(), g_smsSendErr.c_str());

    // typing bar: emoji key, text field, send arrow
    float fy0 = by0 + 9 * s, fy1 = by0 + barH - 9 * s, er = (fy1 - fy0) * 0.5f;
    {   // camera: Take photo / Steam screenshots
        ImVec2 pc(a.x + 8 * s + er, (fy0 + fy1) * 0.5f);
        bool camKey = Hit("smscamera", ImVec2(pc.x - er, fy0), ImVec2(pc.x + er, fy1));
        bool chov = !g_behindSheet && ImGui::IsItemHovered();
        if (chov) DL()->AddCircleFilled(pc, er, Fade(T.text, 0.08f));
        CameraIcon(pc, er * 0.5f, g_photoUi == PhPanel ? kBlue : (chov ? T.text : T.sub));
        if (camKey) {
            g_emojiOpen = false;
            g_photoUi = g_photoUi == PhPanel ? PhNone : PhPanel;
            if (g_photoUi == PhPanel) g_shots = Photo_SteamScreenshots(g_tx.me, 60);
        }
#ifdef GITMO_TEST
        { extern ImVec2 g_testCameraKey; g_testCameraKey = pc; }
#endif
    }
    ImVec2 ec(a.x + 8 * s + er * 3 + 2 * s, (fy0 + fy1) * 0.5f);
    bool emojiKey = Hit("smsemoji", ImVec2(ec.x - er, fy0), ImVec2(ec.x + er, fy1));
    bool ekHov = !g_behindSheet && ImGui::IsItemHovered();
    if (ekHov) DL()->AddCircleFilled(ec, er, Fade(T.text, 0.08f));
    EmojiKeyIcon(ec, er * 0.62f, g_emojiOpen ? kBlue : (ekHov ? T.text : T.sub), g_emojiOpen);
    if (emojiKey) { g_emojiOpen = !g_emojiOpen; if (g_photoUi == PhPanel) g_photoUi = PhNone; g_focusDraft = true; g_wantCursor = g_draftCursor; }
#ifdef GITMO_TEST
    { extern ImVec2 g_testEmojiKey; g_testEmojiKey = ec; }
#endif
    float fx0 = ec.x + er + 8 * s, fx1 = b.x - 12 * s;
    DL()->AddRectFilled(ImVec2(fx0, fy0), ImVec2(fx1, fy1), T.card, (fy1 - fy0) * 0.5f);
    DL()->AddRect(ImVec2(fx0, fy0), ImVec2(fx1, fy1), T.divider, (fy1 - fy0) * 0.5f, 0, 1.0f);
    float sr = (fy1 - fy0) * 0.5f - 4 * s;
    ImVec2 sc(fx1 - sr - 4 * s, (fy0 + fy1) * 0.5f);
    float ix = fx0 + 14 * s, iw = sc.x - sr - 8 * s - ix;
    ImGui::SetCursorScreenPos(ImVec2(ix, (fy0 + fy1) * 0.5f - fs * 0.5f - 1 * s));
    ImGui::PushFont(F_Reg, fs);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, T.text);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, Fade(kBlue, 0.35f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::SetNextItemWidth(iw);
    bool refocus = g_focusDraft && !g_behindSheet;
    if (refocus) { ImGui::SetKeyboardFocusHere(); g_focusDraft = false; }
    ImGui::BeginDisabled(g_behindSheet);
    bool enter = ImGui::InputText("##draft", g_draft, sizeof g_draft,
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways, DraftCallback);
    ImGui::EndDisabled();
    // tapping the text field: back to typing, like a phone. Only a real tap counts: Gitmo
    // putting the cursor back after an emoji activates the field a frame later, with no click.
    if (!g_behindSheet && ImGui::IsMouseClicked(0) && ImGui::IsItemHovered()) { g_emojiOpen = false; if (g_photoUi == PhPanel) g_photoUi = PhNone; }
    (void)refocus;
    if (!g_behindSheet && !ImGui::IsItemActive() && g_draftCursor > (int)strlen(g_draft)) g_draftCursor = (int)strlen(g_draft);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    ImGui::PopFont();
    if (!g_draft[0]) Text(F_Reg, fs, ImVec2(ix, (fy0 + fy1) * 0.5f - fs * 0.5f - 1 * s), T.sub, c->online ? "Text message" : "Text message (sends when they're online)");
    bool has = false;
    for (const char* q = g_draft; *q; q++) if (*q != ' ') has = true;
    bool click = Hit("smssend", ImVec2(sc.x - sr - 2 * s, sc.y - sr - 2 * s), ImVec2(sc.x + sr + 2 * s, sc.y + sr + 2 * s));
    DL()->AddCircleFilled(sc, sr, has ? kBlue : T.divider);
    ArrowUp(sc, sr, IM_COL32(255, 255, 255, 255));
    if ((enter || click) && has) {
        if (Sms_Send(c->id, g_draft)) {
            g_draft[0] = 0;
            g_draftCursor = 0;
            g_smsSendErr.clear();
        } else {
            g_smsSendErr = "Couldn't send. Try again.";
        }
        g_focusDraft = true;   // keep typing
    } else if (enter) g_focusDraft = true;
    if (g_emojiOpen) EmojiPanel(ImVec2(a.x, by0 + barH), ImVec2(b.x, b.y - 14 * s));
    if (g_photoUi == PhPanel) PhotoPanel(ImVec2(a.x, by0 + barH), ImVec2(b.x, b.y - 14 * s));
#ifdef GITMO_TEST
    { extern bool g_testEmojiOpen; extern char g_testDraftCopy[400]; g_testEmojiOpen = g_emojiOpen; strcpy_s(g_testDraftCopy, g_draft); }
#endif
}

static void ScreenSms(ImVec2 a, ImVec2 b) {
    if (g_chat) ScreenSmsChat(a, b);
    else ScreenSmsList(a, b);
}

#ifdef GITMO_TEST
// a made-up landscape (sky, hills, mud road, a truck), for the photo test phases
static PhotoImg TestPicture(int w, int h, int kind) {
    PhotoImg im; im.w = w; im.h = h; im.rgba.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float fx = (float)x / w, fy = (float)y / h;
        float hill = 0.55f + 0.08f * sinf(fx * 6.0f + kind) + 0.04f * sinf(fx * 17.0f);
        int r, g, b;
        if (fy < hill) { r = (int)(90 + 110 * fy); g = (int)(140 + 80 * fy); b = (int)(200 + 40 * fy); if (kind == 2) { r /= 3; g /= 3; b = b / 2; } }
        else if (fabsf(fx - 0.5f - (fy - hill) * 0.6f) < 0.04f + (fy - hill) * 0.5f) { r = 110; g = 85; b = 60; }   // the road
        else { r = (int)(50 + 30 * fy); g = (int)(95 + 40 * fy); b = 45; }
        float dx = fx - 0.78f, dy = fy - 0.18f;
        if (dx * dx + dy * dy < 0.004f && kind != 2) { r = 255; g = 230; b = 150; }   // the sun
        if (fx > 0.42f + kind * 0.05f && fx < 0.58f + kind * 0.05f && fy > hill + 0.05f && fy < hill + 0.13f) { r = 200; g = 60; b = 40; }   // a truck
        uint8_t* p = &im.rgba[((size_t)y * w + x) * 4];
        p[0] = (uint8_t)std::min(r, 255); p[1] = (uint8_t)std::min(g, 255); p[2] = (uint8_t)std::min(b, 255); p[3] = 255;
    }
    return im;
}
static void TestPhotos() {   // message photos + some "Steam screenshots" with thumbnails
    static bool done; if (done) return; done = true;
    CreateDirectoryW((ModuleDir() + L"\\Gitmo-photos").c_str(), nullptr);
    std::string j;
    if (Photo_EncodeJpeg(TestPicture(1280, 720, 0), 1280, j)) { HANDLE h = CreateFileW(Sms_PhotoPath(0x701).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); DWORD w; WriteFile(h, j.data(), (DWORD)j.size(), &w, nullptr); CloseHandle(h); }
    if (Photo_EncodeJpeg(TestPicture(900, 1200, 1), 1280, j)) { HANDLE h = CreateFileW(Sms_PhotoPath(0x702).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); DWORD w; WriteFile(h, j.data(), (DWORD)j.size(), &w, nullptr); CloseHandle(h); }
    std::wstring dir = ModuleDir() + L"\\testshots";
    CreateDirectoryW(dir.c_str(), nullptr); CreateDirectoryW((dir + L"\\thumbnails").c_str(), nullptr);
    for (int i = 0; i < 5; i++) {
        PhotoImg im = TestPicture(1600, 900, i % 3);
        wchar_t n[64]; swprintf_s(n, L"\\2026111%d_1200%02d_1.jpg", i, i);
        for (int big = 0; big < 2; big++) {
            if (!Photo_EncodeJpeg(im, big ? 1600 : 200, j)) continue;
            std::wstring path = big ? dir + n : dir + L"\\thumbnails" + n;
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); DWORD w; WriteFile(h, j.data(), (DWORD)j.size(), &w, nullptr); CloseHandle(h);
        }
        Sleep(20);   // distinct file times: newest first
    }
}
static SmsView DemoSms(int phase) {
    SmsView v; v.ready = true;
    long long now = (long long)_time64(nullptr);
    auto msg = [](uint64_t id, bool mine, long long wall, const char* t, sms::St st) {
        sms::Msg m; m.id = id; m.mine = mine; m.wall = wall; m.key = (uint64_t)wall * 1000 + id; m.text = t; m.st = st; return m; };
    sms::Contact bob; bob.id = 2; bob.name = "Bob"; bob.online = true; bob.inGame = true; bob.unread = phase == 15 || phase == 18 ? 2 : 0;
    bob.msgs = { msg(1, false, now - 26 * 3600, "you still stuck in the swamp?", sms::St::Received),
                 msg(2, true, now - 26 * 3600 + 40, "yeah the azov is buried to the axles lol", sms::St::Read),
                 msg(3, true, now - 26 * 3600 + 55, "bring the winch truck when you can", sms::St::Read),
                 msg(4, false, now - 3600, "on my way. sending you 2k for fuel", sms::St::Received),
                 msg(5, false, now - 3590, "check gitmo", sms::St::Received),
                 msg(6, true, now - 3500, "got it, thanks! \xF0\x9F\x99\x8F", phase == 16 ? sms::St::Read : sms::St::Delivered) };
    if (phase >= 31) {   // photos: one from Bob, one back (their JPEGs are made by TestPhotos)
        sms::Msg p1 = msg(0x701, false, now - 1200, "", sms::St::Received); p1.photo = true; p1.text = sms::kPhotoText;
        sms::Msg p2 = msg(0x702, true, now - 1100, "", sms::St::Read); p2.photo = true; p2.text = sms::kPhotoText;
        bob.msgs.push_back(p1);
        bob.msgs.push_back(msg(11, false, now - 1180, "look where I parked \xF0\x9F\x98\x82", sms::St::Received));
        bob.msgs.push_back(p2);
        bob.msgs.push_back(msg(12, true, now - 1090, "that's my view right now", sms::St::Read));
    }
    if (phase >= 43) {   // shared radio stations: one from Bob, one back
        bob.msgs.push_back(msg(13, false, now - 300, "\xF0\x9F\x93\xBB Listening to Just Between You And Me by Charley Pride on Truckers Radio USA http://streamer.radio.co/s81d30349a/low", sms::St::Received));
        bob.msgs.push_back(msg(14, false, now - 290, "this one's good for night driving", sms::St::Received));
        bob.msgs.push_back(msg(15, true, now - 200, "\xF0\x9F\x93\xBB Listening to Absolute Radio 90s\nhttps://stream-ar.hellorayo.co.uk/absolute90shigh.aac", sms::St::Read));
    }
    sms::Contact cat; cat.id = 3; cat.name = "Cat Driver"; cat.online = true;
    cat.msgs = { msg(7, false, now - 5 * 86400, "gg on that contract", sms::St::Received) };
    sms::Contact ein; ein.id = 4; ein.name = "Dave Diesel"; ein.online = false; ein.lastSeenWall = now - 86400 - 3000;
    ein.msgs = { msg(8, false, now - 2 * 86400, "same time tomorrow?", sms::St::Received),
                 msg(9, true, now - 2 * 86400 + 120, "yep, 8pm", sms::St::Read),
                 msg(10, true, now - 600, "you around? want to do the michigan run", sms::St::Waiting) };
    sms::Contact dave; dave.id = 5; dave.name = "Dave from the forums with a very long name"; dave.online = false; dave.lastSeenWall = now - 9 * 86400;
    v.contacts = { bob, cat, ein, dave };
    return v;
}
#endif

#include "ui_radio.inc"

// ---- app drawer -------------------------------------------------------------------------------
static void AppIcon(const char* id, const char* label, ImVec2 c, float size, ImU32 bg, int kind) {
    ImVec2 a(c.x - size / 2, c.y - size / 2), b(c.x + size / 2, c.y + size / 2);
    bool clicked = Hit(id, ImVec2(a.x - 8 * s, a.y - 6 * s), ImVec2(b.x + 8 * s, b.y + 26 * s));
    bool hov = ImGui::IsItemHovered();
    DL()->AddRectFilled(ImVec2(a.x, a.y + 3 * s), ImVec2(b.x, b.y + 3 * s), IM_COL32(0, 0, 0, 50), size * 0.26f);
    DL()->AddRectFilled(a, b, bg, size * 0.26f);
    if (hov) DL()->AddRectFilled(a, b, IM_COL32(255, 255, 255, 30), size * 0.26f);
    if (kind == 0) {   // Gitmo: a bold "g" with a coin ring
        DL()->AddCircle(c, size * 0.31f, IM_COL32(255, 255, 255, 90), 0, size * 0.045f);
        ImVec2 sz = Measure(F_Bold, size * 0.62f, "g");
        Text(F_Bold, size * 0.62f, ImVec2(c.x - sz.x / 2, c.y - sz.y * 0.58f), IM_COL32(255, 255, 255, 255), "g");
    } else if (kind == 2) {   // SMS: a speech bubble
        ImVec2 q0(c.x - size * 0.30f, c.y - size * 0.22f), q1(c.x + size * 0.30f, c.y + size * 0.18f);
        DL()->AddRectFilled(q0, q1, IM_COL32(255, 255, 255, 255), size * 0.2f);
        DL()->AddTriangleFilled(ImVec2(c.x - size * 0.18f, q1.y - size * 0.04f), ImVec2(c.x - size * 0.27f, q1.y + size * 0.12f),
                                ImVec2(c.x - size * 0.02f, q1.y - size * 0.02f), IM_COL32(255, 255, 255, 255));
    } else if (kind == 3) {   // Radio: a little transistor radio
        ImU32 wc = IM_COL32(255, 255, 255, 255);
        DL()->AddLine(ImVec2(c.x - size * 0.16f, c.y - size * 0.13f), ImVec2(c.x + size * 0.2f, c.y - size * 0.33f), wc, size * 0.045f);
        DL()->AddRectFilled(ImVec2(c.x - size * 0.32f, c.y - size * 0.14f), ImVec2(c.x + size * 0.32f, c.y + size * 0.25f), wc, size * 0.07f);
        DL()->AddCircleFilled(ImVec2(c.x - size * 0.13f, c.y + size * 0.055f), size * 0.11f, bg);
        for (int i = 0; i < 3; i++)
            DL()->AddRectFilled(ImVec2(c.x + size * 0.05f, c.y - size * 0.05f + i * size * 0.08f), ImVec2(c.x + size * 0.24f, c.y - size * 0.02f + i * size * 0.08f), bg, size * 0.01f);
    } else {
        IconGear(c, size * 0.36f, IM_COL32(255, 255, 255, 240));
    }
    TextCenter(F_Semi, 12.5f * s, ImVec2(c.x, b.y + 7 * s), IM_COL32(255, 255, 255, 235), label);
    int waiting = 0;
    if (kind == 0) for (auto& i : g_tx.ins) if (i.st == tx::In::Prompt || (!i.seen && (i.st == tx::In::Credited || i.st == tx::In::Void))) waiting++;
    if (kind == 2) waiting = g_smsUnread;
    if (waiting) {   // red badge while a request waits for an answer
        ImVec2 bc(b.x - 2 * s, a.y + 2 * s);
        DL()->AddCircleFilled(bc, 11 * s, Hex(0xFF3B30));
        char nb[8]; sprintf_s(nb, "%d", waiting);
        ImVec2 sz = Measure(F_Bold, 13 * s, nb);
        Text(F_Bold, 13 * s, ImVec2(bc.x - sz.x * 0.5f, bc.y - sz.y * 0.5f), IM_COL32(255, 255, 255, 255), nb);
    }
    if (clicked) { g_app = kind == 0 ? GitmoApp : kind == 2 ? SmsApp : kind == 3 ? RadioApp : SettingsApp; g_screen = Home; }
}

// ---- wallpapers: painted from soft overlapping lights, so they scale to any size -------------
struct Blob { float x, y, r; ImU32 col; };
struct Wallpaper { const char* name; unsigned base; Blob blobs[4]; bool stars; };
static const Wallpaper kWallpapers[5] = {
    { "Forest",  0x0E3B2C, { {0.15f,0.62f,0.55f,IM_COL32(29,201,131,38)}, {0.95f,0.38f,0.45f,IM_COL32(80,170,255,26)}, {0.55f,0.95f,0.40f,IM_COL32(255,255,255,14)}, {} }, false },
    { "Dusk",    0x2B1A3F, { {0.20f,0.88f,0.70f,IM_COL32(255,140,80,70)}, {0.92f,0.30f,0.50f,IM_COL32(230,90,160,50)}, {0.50f,0.08f,0.40f,IM_COL32(255,210,120,26)}, {0.10f,0.30f,0.30f,IM_COL32(120,90,220,40)} }, false },
    { "Mud",     0x33241A, { {0.10f,0.90f,0.65f,IM_COL32(150,100,55,80)}, {0.88f,0.55f,0.45f,IM_COL32(110,130,60,50)}, {0.50f,0.12f,0.38f,IM_COL32(255,220,170,22)}, {} }, false },
    { "Glacier", 0x123A5A, { {0.20f,0.28f,0.55f,IM_COL32(170,220,255,55)}, {0.90f,0.82f,0.60f,IM_COL32(255,255,255,38)}, {0.60f,0.55f,0.30f,IM_COL32(90,200,230,40)}, {} }, false },
    { "Night",   0x080D1C, { {0.82f,0.14f,0.32f,IM_COL32(120,140,255,38)}, {0.18f,0.96f,0.60f,IM_COL32(40,60,150,70)}, {}, {} }, true },
};

// The rounded screen as a polygon (clockwise on screen), for clipping the lights to it.
static void RoundedRectPoly(ImVec2 a, ImVec2 b, float r, std::vector<ImVec2>& out) {
    out.clear();
    r = std::min(r, std::min(b.x - a.x, b.y - a.y) * 0.5f);
    const ImVec2 c[4] = { ImVec2(b.x - r, a.y + r), ImVec2(b.x - r, b.y - r), ImVec2(a.x + r, b.y - r), ImVec2(a.x + r, a.y + r) };
    const int seg = 10;
    for (int k = 0; k < 4; k++)   // corners top-right, bottom-right, bottom-left, top-left
        for (int i = 0; i <= seg; i++) {
            float t = (-0.5f + k * 0.5f + 0.5f * i / seg) * 3.14159265f;
            out.push_back(ImVec2(c[k].x + cosf(t) * r, c[k].y + sinf(t) * r));
        }
}
// Sutherland-Hodgman: the part of convex polygon `poly` inside convex polygon `clip`
static void ClipConvex(const std::vector<ImVec2>& poly, const std::vector<ImVec2>& clip, std::vector<ImVec2>& out,
                       std::vector<ImVec2>& cur, std::vector<ImVec2>& next) {
    cur.assign(poly.begin(), poly.end());
    next.clear();
    for (size_t e = 0; e < clip.size() && !cur.empty(); e++) {
        ImVec2 p = clip[e], q = clip[(e + 1) % clip.size()];
        auto side = [&](ImVec2 v) { return (q.x - p.x) * (v.y - p.y) - (q.y - p.y) * (v.x - p.x); };   // >= 0: inside (clockwise on screen)
        next.clear();
        for (size_t i = 0; i < cur.size(); i++) {
            ImVec2 u = cur[i], v = cur[(i + 1) % cur.size()];
            float su = side(u), sv = side(v);
            if (su >= 0) next.push_back(u);
            if ((su >= 0) != (sv >= 0)) { float t = su / (su - sv); next.push_back(ImVec2(u.x + (v.x - u.x) * t, u.y + (v.y - u.y) * t)); }
        }
        cur.swap(next);
    }
    out.assign(cur.begin(), cur.end());
}
static bool InRoundedRect(ImVec2 v, ImVec2 a, ImVec2 b, float r) {
    if (v.x < a.x || v.x > b.x || v.y < a.y || v.y > b.y) return false;
    float cx = std::min(std::max(v.x, a.x + r), b.x - r), cy = std::min(std::max(v.y, a.y + r), b.y - r);
    return (v.x - cx) * (v.x - cx) + (v.y - cy) * (v.y - cy) <= r * r;
}

static void DrawWallpaper(int idx, ImVec2 a, ImVec2 b, float rounding) {
    const Wallpaper& wp = kWallpapers[std::min(std::max(idx, 0), 4)];
    DL()->AddRectFilled(a, b, Hex(wp.base), rounding);
    float W = b.x - a.x, Hh = b.y - a.y;
    // the lights are cut to the rounded screen exactly, so none spill past its corners
    static std::vector<ImVec2> screen, circle, part, cur, next;
    screen.reserve(44); circle.reserve(64); part.reserve(128); cur.reserve(128); next.reserve(128);
    RoundedRectPoly(a, b, rounding, screen);
    for (const Blob& bl : wp.blobs) {
        if (bl.r <= 0) continue;
        ImVec2 c(a.x + W * bl.x, a.y + Hh * bl.y);
        float r = W * bl.r;
        circle.clear();
        for (int i = 0; i < 64; i++) { float t = i * 6.2831853f / 64; circle.push_back(ImVec2(c.x + cosf(t) * r, c.y + sinf(t) * r)); }
        ClipConvex(circle, screen, part, cur, next);
        if (part.size() >= 3) DL()->AddConvexPolyFilled(part.data(), (int)part.size(), bl.col);
    }
    if (wp.stars) {
        uint32_t seed = 0x5EED;
        for (int i = 0; i < 60; i++) {
            seed = seed * 1664525u + 1013904223u; float x = (seed >> 8) / 16777216.0f;
            seed = seed * 1664525u + 1013904223u; float y = (seed >> 8) / 16777216.0f;
            seed = seed * 1664525u + 1013904223u; float z = (seed >> 8) / 16777216.0f;
            float sr = std::max(0.6f, W * 0.004f * (0.5f + z));
            ImVec2 p(a.x + W * x, a.y + Hh * y * 0.7f);
            if (InRoundedRect(p, ImVec2(a.x + sr, a.y + sr), ImVec2(b.x - sr, b.y - sr), std::max(0.0f, rounding - sr)))
                DL()->AddCircleFilled(p, sr, IM_COL32(255, 255, 255, (int)(90 + 140 * z)));
        }
    }
}

static void ScreenDrawer(ImVec2 a, ImVec2 b) {
    DrawWallpaper(g_cfg.wallpaper, a, b, 34 * s);
    float W = b.x - a.x;
    StatusBar(a, ImVec2(b.x, a.y + 48 * s), IM_COL32(255, 255, 255, 240));
    // lock-screen style clock
    SYSTEMTIME t; GetLocalTime(&t);
    char tm[8]; sprintf_s(tm, "%d:%02d", t.wHour, t.wMinute);
    float cx = (a.x + b.x) * 0.5f;
    TextCenter(F_Bold, 72 * s, ImVec2(cx, a.y + 70 * s), IM_COL32(255, 255, 255, 245), tm);
    static const char* days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char* months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    char date[48]; sprintf_s(date, "%s, %d %s", days[t.wDayOfWeek % 7], t.wDay, months[(t.wMonth + 11) % 12]);
    TextCenter(F_Semi, 16 * s, ImVec2(cx, a.y + 156 * s), IM_COL32(255, 255, 255, 210), date);
    // apps
    float size = 62 * s, colW = W / 4;
    float y = a.y + 250 * s;
    AppIcon("app_sms", "SMS", ImVec2(a.x + colW * 0.5f, y), size, kGreen, 2);
    AppIcon("app_gitmo", "Gitmo", ImVec2(a.x + colW * 1.5f, y), size, T.accent, 0);
    AppIcon("app_radio", "Radio", ImVec2(a.x + colW * 2.5f, y), size, Hex(0xFF375F), 3);
    AppIcon("app_settings", "Settings", ImVec2(a.x + colW * 3.5f, y), size, Hex(0x6E6E73), 1);
    if (RadioActive()) MiniPlayer(ImVec2(a.x + 14 * s, b.y - 102 * s), ImVec2(b.x - 14 * s, b.y - 38 * s), "drawermini", true);
    // home indicator (no-op on the home screen)
    DL()->AddRectFilled(ImVec2(cx - 55 * s, b.y - 11 * s), ImVec2(cx + 55 * s, b.y - 6 * s), IM_COL32(255, 255, 255, 200), 3 * s);
}

// ---- Settings app ----------------------------------------------------------------------------
static void ScreenSettingsApp(const MoneyState& st, ImVec2 a, ImVec2 b) {
    float x = a.x + 20 * s, w = b.x - a.x - 40 * s, y = a.y + 6 * s;
    Text(F_Bold, 28 * s, ImVec2(x, y), T.text, "Settings");
    y += 50 * s;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginChild("##settings", ImVec2(w, b.y - y - 26 * s), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float y0 = p.y;

    // display
    SectionTitle(p, "DISPLAY");
    DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + 164 * s), T.card, 16 * s);
    {   // phone size: segmented Small / Medium / Large, in the bottom row of this card
        float ry = p.y + 112 * s;
        DL()->AddLine(ImVec2(p.x + 16 * s, ry), ImVec2(p.x + w - 16 * s, ry), T.divider, 1.0f);
        Text(F_Reg, 15 * s, ImVec2(p.x + 16 * s, ry + 15 * s), T.text, "Phone size");
        const char* names[3] = { "S", "M", "L" };
        float sw = 126 * s, sx = p.x + w - sw - 10 * s, sy = ry + 10 * s;
        DL()->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + 32 * s), T.outBg, 16 * s);
        float seg = (sw - 6 * s) / 3;
        for (int i = 0; i < 3; i++) {
            ImVec2 a0(sx + 3 * s + i * seg, sy + 3 * s), a1(a0.x + seg, sy + 29 * s);
            bool sel = g_cfg.phoneSize == i;
            char id[16]; sprintf_s(id, "size%d", i);
            if (Hit(id, a0, a1) && !sel) SavePhoneSize(i);
            if (sel) DL()->AddRectFilled(a0, a1, T.card, 13 * s);
            TextCenter(F_Semi, 13.5f * s, ImVec2((a0.x + a1.x) * 0.5f, a0.y + 4 * s), sel ? T.text : T.sub, names[i]);
        }
    }
    Text(F_Reg, 15 * s, ImVec2(p.x + 16 * s, p.y + 15 * s), T.text, "Dark mode");
    if (Toggle("dark", ImVec2(p.x + w - 14 * s, p.y + 26 * s), g_cfg.dark)) { g_cfg.dark = !g_cfg.dark; ApplyTheme(); SaveThemeSetting(g_cfg.dark); }
    DL()->AddLine(ImVec2(p.x + 16 * s, p.y + 52 * s), ImVec2(p.x + w - 16 * s, p.y + 52 * s), T.divider, 1.0f);
    Text(F_Reg, 15 * s, ImVec2(p.x + 16 * s, p.y + 62 * s), T.text, "Cracked screen");
    Text(F_Reg, 12 * s, ImVec2(p.x + 16 * s, p.y + 82 * s), T.sub, "It's been a rough season");
    if (Toggle("cracked", ImVec2(p.x + w - 14 * s, p.y + 82 * s), g_cfg.cracked)) SaveCracked(!g_cfg.cracked);
    p.y += 178 * s;

    // VR: where the phone shows, and how far away it hangs in the headset
    SectionTitle(p, "VR");
    DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + 228 * s), T.card, 16 * s);
    {
        auto segmented = [&](const char* idp, const char* const* names, int n, int cur, float sw, float sy) {
            float sx = p.x + w - sw - 10 * s;
            DL()->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + 32 * s), T.outBg, 16 * s);
            float seg = (sw - 6 * s) / n;
            int picked = -1;
            for (int i = 0; i < n; i++) {
                ImVec2 a0(sx + 3 * s + i * seg, sy + 3 * s), a1(a0.x + seg, sy + 29 * s);
                char id[24]; sprintf_s(id, "%s%d", idp, i);
                if (Hit(id, a0, a1) && i != cur) picked = i;
                if (i == cur) DL()->AddRectFilled(a0, a1, T.card, 13 * s);
                TextCenter(F_Semi, 13 * s, ImVec2((a0.x + a1.x) * 0.5f, a0.y + 4 * s), i == cur ? T.text : T.sub, names[i]);
            }
            return picked;
        };
        // where: Auto / Screen / Headset
        Text(F_Reg, 15 * s, ImVec2(p.x + 16 * s, p.y + 15 * s), T.text, "Show on");
        static const char* where[3] = { "Auto", "Screen", "Headset" };
        int pick = segmented("disp", where, 3, g_cfg.display, 200 * s, p.y + 10 * s);
        if (pick >= 0) SaveDisplay(pick);
        Text(F_Reg, 12 * s, ImVec2(p.x + 16 * s, p.y + 48 * s), T.sub, VR_Status());
        DL()->AddLine(ImVec2(p.x + 16 * s, p.y + 70 * s), ImVec2(p.x + w - 16 * s, p.y + 70 * s), T.divider, 1.0f);

        // distance: - 0.60 m +
        Text(F_Reg, 15 * s, ImVec2(p.x + 16 * s, p.y + 84 * s), T.text, "Distance");
        float bx = p.x + w - 150 * s, by = p.y + 78 * s;
        if (Pill("vrnear", "\xE2\x88\x92", ImVec2(bx, by), ImVec2(bx + 40 * s, by + 32 * s), false))
            SaveVrDistance(std::max(0.25f, roundf((g_cfg.vrDistance - 0.05f) * 20) / 20));
        char dl[16]; sprintf_s(dl, "%.2f m", g_cfg.vrDistance);
        TextCenter(F_Semi, 14 * s, ImVec2(bx + 70 * s, by + 7 * s), T.text, dl);
        if (Pill("vrfar", "+", ImVec2(bx + 100 * s, by), ImVec2(bx + 140 * s, by + 32 * s), false))
            SaveVrDistance(std::min(3.0f, roundf((g_cfg.vrDistance + 0.05f) * 20) / 20));
        DL()->AddLine(ImVec2(p.x + 16 * s, p.y + 122 * s), ImVec2(p.x + w - 16 * s, p.y + 122 * s), T.divider, 1.0f);

        // pointer speed: - 2.0x +
        Text(F_Reg, 15 * s, ImVec2(p.x + 16 * s, p.y + 136 * s), T.text, "Pointer speed");
        by = p.y + 130 * s;
        if (Pill("vrslow", "\xE2\x88\x92", ImVec2(bx, by), ImVec2(bx + 40 * s, by + 32 * s), false))
            SaveVrPointerSpeed(std::max(0.5f, roundf((g_cfg.vrPointerSpeed - 0.5f) * 2) / 2));
        sprintf_s(dl, "%.1f\xC3\x97", g_cfg.vrPointerSpeed);
        TextCenter(F_Semi, 14 * s, ImVec2(bx + 70 * s, by + 7 * s), T.text, dl);
        if (Pill("vrfast", "+", ImVec2(bx + 100 * s, by), ImVec2(bx + 140 * s, by + 32 * s), false))
            SaveVrPointerSpeed(std::min(6.0f, roundf((g_cfg.vrPointerSpeed + 0.5f) * 2) / 2));
        DL()->AddLine(ImVec2(p.x + 16 * s, p.y + 174 * s), ImVec2(p.x + w - 16 * s, p.y + 174 * s), T.divider, 1.0f);

        // bring it back in front of you (it otherwise stays where it was left)
        Text(F_Reg, 12 * s, ImVec2(p.x + 16 * s, p.y + 186 * s), T.sub, "Drag the border to move it;");
        Text(F_Reg, 12 * s, ImVec2(p.x + 16 * s, p.y + 202 * s), T.sub, "it stays where you leave it.");
        if (Pill("vrhere", "Bring here", ImVec2(p.x + w - 122 * s, p.y + 186 * s), ImVec2(p.x + w - 12 * s, p.y + 218 * s), false))
            VR_Replace();
    }
    p.y += 242 * s;

    // wallpaper: five thumbnails, tap one to use it
    SectionTitle(p, "WALLPAPER");
    {
        float ch = 150 * s;
        DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + ch), T.card, 16 * s);
        float gap = 8 * s, tw = (w - 24 * s - 4 * gap) / 5, th = tw * 1.75f;
        for (int i = 0; i < 5; i++) {
            ImVec2 t0(p.x + 12 * s + i * (tw + gap), p.y + 12 * s), t1(t0.x + tw, t0.y + th);
            char id[16]; sprintf_s(id, "wp%d", i);
            if (Hit(id, t0, ImVec2(t1.x, t1.y + 20 * s)) && g_cfg.wallpaper != i) SaveWallpaper(i);
            DrawWallpaper(i, t0, t1, 8 * s);
            bool sel = g_cfg.wallpaper == i;
            if (sel) DL()->AddRect(ImVec2(t0.x - 3 * s, t0.y - 3 * s), ImVec2(t1.x + 3 * s, t1.y + 3 * s), T.accent, 10 * s, 0, 2.5f * s);
            TextCenter(sel ? F_Semi : F_Reg, 11.5f * s, ImVec2((t0.x + t1.x) * 0.5f, t1.y + 5 * s), sel ? T.accent : T.sub, kWallpapers[i].name);
        }
        p.y += ch + 14 * s;
    }

    // notifications: the sound when someone sends you money
    SectionTitle(p, "NOTIFICATIONS");
    DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + 104 * s), T.card, 16 * s);
    Text(F_Reg, 15 * s, ImVec2(p.x + 16 * s, p.y + 10 * s), T.text, "Alerts");
    Text(F_Reg, 12 * s, ImVec2(p.x + 16 * s, p.y + 32 * s), T.sub, "Money received and new messages");
    {   // Sound / Vibrate / Off, and a button to hear the current one
        static const char* modes[3] = { "Sound", "Vibrate", "Off" };
        float tw = 62 * s, sx = p.x + 12 * s, sw = w - 24 * s - tw - 8 * s, sy = p.y + 58 * s;
        DL()->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + 32 * s), T.outBg, 16 * s);
        float seg = (sw - 6 * s) / 3;
        for (int i = 0; i < 3; i++) {
            ImVec2 a0(sx + 3 * s + i * seg, sy + 3 * s), a1(a0.x + seg, sy + 29 * s);
            bool sel = g_cfg.notifyMode == i;
            char id[16]; sprintf_s(id, "notify%d", i);
            if (Hit(id, a0, a1) && !sel) { SaveNotifyMode(i); Notify_Alert(); }   // let them hear (or feel) the choice
            if (sel) DL()->AddRectFilled(a0, a1, T.card, 13 * s);
            TextCenter(F_Semi, 13 * s, ImVec2((a0.x + a1.x) * 0.5f, a0.y + 4 * s), sel ? T.text : T.sub, modes[i]);
        }
        if (Pill("notifytest", "Test", ImVec2(p.x + w - 12 * s - tw, sy), ImVec2(p.x + w - 12 * s, sy + 32 * s), false))
            g_cfg.notifyMode == 1 ? Notify_Vibrate() : Notify_Play();   // (Off: still lets you hear the tone)
    }
    p.y += 118 * s;

    // messages: erase the SMS history kept on this PC (asks first)
    SectionTitle(p, "MESSAGES");
#ifdef GITMO_TEST
    g_msgSectionY = p.y - y0;
#endif
    {
        size_t msgs = 0, convs = 0, waiting = 0;
        for (auto& c : g_smsView.contacts) {
            if (!c.msgs.empty()) convs++;
            msgs += c.msgs.size();
            for (auto& m : c.msgs) if (m.mine && m.st == sms::St::Waiting) waiting++;
        }
        if (msgs == 0) g_eraseAsk = false;
        float mh = g_eraseAsk ? 116 * s : 64 * s;
        DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + mh), T.card, 16 * s);
        if (!g_eraseAsk) {
            char sub[96];
            if (msgs == 0) sprintf_s(sub, "No messages kept");
            else sprintf_s(sub, "%zu message%s with %zu %s", msgs, msgs == 1 ? "" : "s", convs, convs == 1 ? "person" : "people");
            Text(F_Reg, 15 * s, ImVec2(p.x + 16 * s, p.y + 10 * s), T.text, "Message history");
            Text(F_Reg, 12 * s, ImVec2(p.x + 16 * s, p.y + 32 * s), T.sub, sub);
            if (msgs && DangerPill("erase", "Erase", ImVec2(p.x + w - 92 * s, p.y + 16 * s), ImVec2(p.x + w - 12 * s, p.y + 48 * s), false))
                g_eraseAsk = true;
        } else {
            Text(F_Semi, 15 * s, ImVec2(p.x + 16 * s, p.y + 12 * s), T.text, "Erase all messages?");
            Text(F_Reg, 12 * s, ImVec2(p.x + 16 * s, p.y + 34 * s), T.sub, "Only on this PC; the others keep theirs.");
            Text(F_Reg, 12 * s, ImVec2(p.x + 16 * s, p.y + 50 * s), waiting ? kRedText() : T.sub,
                 waiting ? "Unsent messages are deleted too." : "This can't be undone.");
            float bw2 = (w - 32 * s - 8 * s) / 2, by = p.y + 72 * s;
            if (Pill("erasecancel", "Cancel", ImVec2(p.x + 16 * s, by), ImVec2(p.x + 16 * s + bw2, by + 34 * s), false)) g_eraseAsk = false;
            if (DangerPill("eraseyes", "Erase", ImVec2(p.x + 24 * s + bw2, by), ImVec2(p.x + 24 * s + 2 * bw2, by + 34 * s), true)) {
#ifndef GITMO_TEST
                Sms_EraseAll();
                g_chat = 0;
#endif
                g_eraseAsk = false;
            }
        }
        p.y += mh + 14 * s;
    }

    // controls: the key that opens the phone
    SectionTitle(p, "CONTROLS");
    bool capturing = g_captureKey;
    bool vrClash = g_cfg.menuKey == VK_INSERT;
    float kh = (capturing || vrClash) ? 74 * s : 56 * s;
    DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + kh), T.card, 16 * s);
    Text(F_Reg, 15 * s, ImVec2(p.x + 16 * s, p.y + 9 * s), T.text, "Open / close phone");
    if (capturing) Text(F_Semi, 13 * s, ImVec2(p.x + 16 * s, p.y + 30 * s), T.accent, "Press a key...  (Esc cancels)");
    else           Text(F_Bold, 13.5f * s, ImVec2(p.x + 16 * s, p.y + 30 * s), T.sub, g_cfg.menuKeyName.c_str());
    if (vrClash && !capturing) Text(F_Reg, 12 * s, ImVec2(p.x + 16 * s, p.y + 50 * s), T.sub, "Insert also opens the VR mod's menu.");
    if (capturing) Text(F_Reg, 12 * s, ImVec2(p.x + 16 * s, p.y + 50 * s), T.sub, "Any key except Shift, Ctrl, Alt and Windows.");
    float pw = 92 * s;
    if (Pill("rebind", capturing ? "Cancel" : "Change", ImVec2(p.x + w - pw - 12 * s, p.y + 12 * s), ImVec2(p.x + w - 12 * s, p.y + 44 * s), !capturing))
        g_captureKey = !capturing;
    p.y += kh + 14 * s;

    // battery
    SectionTitle(p, "BATTERY");
    DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + 60 * s), T.card, 16 * s);
    {
        float lvl = g_uiBatt / kBatteryMinutes;
        int stt = g_uiBattState;
        BatteryIcon(ImVec2(p.x + 16 * s, p.y + 18 * s), 46 * s, 22 * s, lvl, stt == 1, T.text);
        char l1[64]; sprintf_s(l1, "%d%%  \xC2\xB7  %.0f min left", (int)(lvl * 100 + 0.5f), ceilf(g_uiBatt));
        Text(F_Semi, 15 * s, ImVec2(p.x + 78 * s, p.y + 12 * s), T.text, l1);
        const char* l2 = stt == 1 ? "Charging (engine running or in the garage)" : stt == 2 ? "On battery (engine off)" : "Not draining (no truck loaded yet)";
        Text(F_Reg, 12.5f * s, ImVec2(p.x + 78 * s, p.y + 32 * s), stt == 1 ? T.accent : T.sub, l2);
    }
    p.y += 74 * s;

    // about
    SectionTitle(p, "ABOUT");
    DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + 52 * s), T.card, 16 * s);
    SettingRow("Gitmo", "version " GITMO_VERSION, p, w, false);
    p.y += 66 * s;

    // developer: money finder + test buttons (only with Developer=1 in Gitmo.ini: the buttons change real money)
    if (g_cfg.developer) {
    SectionTitle(p, "DEVELOPER");
    float dh = 204 * s + std::min<size_t>(st.cands.size(), 4) * 24 * s + (g_cfg.garageResearch ? 18 * s : 0);   // (two rows of buttons)
    DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + dh), T.card, 16 * s);
    float ty = p.y + 14 * s;
    Text(F_Semi, 14 * s, ImVec2(p.x + 16 * s, ty), T.text, st.status.c_str());
    ty += 24 * s;
    char line[160];
    if (st.save.ok) sprintf_s(line, "Save: %s  \xC2\xB7  XP %.0f  \xC2\xB7  rank %.0f", Money((long long)st.save.money).c_str(), st.save.xp, st.save.rank);
    else sprintf_s(line, "Save: not read yet");
    Text(F_Reg, 13 * s, ImVec2(p.x + 16 * s, ty), T.sub, line);
    ty += 20 * s;
    sprintf_s(line, "Scans: %d  \xC2\xB7  matches: %zu", st.scans, st.cands.size());
    Text(F_Reg, 13 * s, ImVec2(p.x + 16 * s, ty), T.sub, line);
    ty += 26 * s;
    for (size_t i = 0; i < st.cands.size() && i < 4; i++) {
        const Candidate& c = st.cands[i];
        bool sel = (int)i == st.selected;
        sprintf_s(line, "%s #%zu  %s", sel ? "\xE2\x97\x8F" : "\xE2\x97\x8B", i, Money(c.value).c_str());
        char id[16]; sprintf_s(id, "cand%zu", i);
        if (Hit(id, ImVec2(p.x + 10 * s, ty - 2 * s), ImVec2(p.x + w - 10 * s, ty + 20 * s))) Money_Select((int)i);
        Text(sel ? F_Semi : F_Reg, 13.5f * s, ImVec2(p.x + 16 * s, ty), sel ? T.accent : T.text, line);
        ty += 24 * s;
    }
    ty += 6 * s;
    float bw = (w - 32 * s - 16 * s) / 3;
    if (Pill("rescan", "Rescan", ImVec2(p.x + 16 * s, ty), ImVec2(p.x + 16 * s + bw, ty + 36 * s), false)) Money_Rescan();
    // same path as a transfer (Money_Apply), so a test behaves exactly like sending / receiving
    if (Pill("plus", "+$1,000", ImVec2(p.x + 24 * s + bw, ty), ImVec2(p.x + 24 * s + 2 * bw, ty + 36 * s), false)) Money_Apply(1000, "Developer test");
    if (Pill("minus", "\xE2\x88\x92$1,000", ImVec2(p.x + 32 * s + 2 * bw, ty), ImVec2(p.x + 32 * s + 3 * bw, ty + 36 * s), false)) Money_Apply(-1000, "Developer test");
    ty += 44 * s;
    // finer steps, for setting the balance just below a price
    if (Pill("plus100", "+$100", ImVec2(p.x + 24 * s + bw, ty), ImVec2(p.x + 24 * s + 2 * bw, ty + 36 * s), false)) Money_Apply(100, "Developer test");
    if (Pill("minus100", "\xE2\x88\x92$100", ImVec2(p.x + 32 * s + 2 * bw, ty), ImVec2(p.x + 32 * s + 3 * bw, ty + 36 * s), false)) Money_Apply(-100, "Developer test");
    ty += 46 * s;
    Text(F_Reg, 12 * s, ImVec2(p.x + 16 * s, ty), T.sub, "These change your real balance, like a transfer.");
    if (g_cfg.garageResearch) {   // GarageResearch=1: where to look in the steps
        extern std::atomic<int> g_garageResearchState;
        int gs = g_garageResearchState;
        Text(F_Semi, 12 * s, ImVec2(p.x + 16 * s, ty + 16 * s), gs == 1 ? kRedText() : T.accent,
             gs == 1 ? "Research: scanning - wait..." : gs == 2 ? "Research: buy here, back out, tap +1,000" : "Research on: tap -1,000 in a garage screen");
    }
    p.y += dh + 10 * s;

    // battery test: jump to full or nearly empty
    DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + 108 * s), T.card, 16 * s);
    Text(F_Semi, 14 * s, ImVec2(p.x + 16 * s, p.y + 12 * s), T.text, "Battery test");
    Text(F_Reg, 12 * s, ImVec2(p.x + 16 * s, p.y + 32 * s), T.sub, "7% is about one minute with the engine off.");
    {
        float bw2 = (w - 32 * s - 8 * s) / 2, by = p.y + 56 * s;
        if (Pill("batt100", "Set 100%", ImVec2(p.x + 16 * s, by), ImVec2(p.x + 16 * s + bw2, by + 36 * s), false)) Battery_Set(kBatteryMinutes);
        if (Pill("batt7", "Set 7%", ImVec2(p.x + 24 * s + bw2, by), ImVec2(p.x + 24 * s + 2 * bw2, by + 36 * s), false)) Battery_Set(kBatteryMinutes * 0.07f);
    }
    p.y += 118 * s;
    }   // developer

    // The buttons above move ImGui's cursor around; reset it so the scroll range is
    // exactly the content height (otherwise you could scroll on past the last item).
    ImGui::SetCursorScreenPos(ImVec2(x, y0));
    ImGui::Dummy(ImVec2(w, p.y - y0));
#ifdef GITMO_TEST
    extern int g_testFrame;
    if (g_testFrame / 300 == 6) ImGui::SetScrollY(1e6f);   // scroll as far as it goes: must stop at the last item
    if (g_testFrame / 300 == 3) ImGui::SetScrollY(560 * s);   // show Notifications + Controls
    if (g_testFrame / 300 == 19 || g_testFrame / 300 == 20) ImGui::SetScrollY(g_msgSectionY - 120 * s);   // show Messages
    if (g_testFrame / 300 == 29) ImGui::SetScrollY(g_msgSectionY - 250 * s);   // show Notifications
#endif
#ifdef GITMO_TEST
    { extern ImVec2 g_testSettingsMid; extern float g_testSettingsScroll; extern int g_testWallpaperNow;
      ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
      g_testSettingsMid = ImVec2(wp.x + ws.x * 0.3f, wp.y + ws.y * 0.62f); g_testSettingsScroll = ImGui::GetScrollY(); g_testWallpaperNow = g_cfg.wallpaper; }
#endif
    DragScroll();
    ImGui::EndChild();
}

// ---- entry points ----------------------------------------------------------------------
void UI_Init(float) {
    ImGuiIO& io = ImGui::GetIO();
    // Each weight gets colour emoji merged in (emoji.cpp: Segoe UI Emoji drawn by DirectWrite,
    // sized to the text line), then Segoe UI Symbol for any other symbol (one colour).
    auto withEmoji = [&](const char* path) {
        ImFont* f = io.Fonts->AddFontFromFileTTF(path, 16.0f);
        if (!f) return f;
        { ImFontConfig fc; fc.MergeMode = true; fc.SizePixels = 16.0f; fc.FontLoader = Emoji_FontLoader(); io.Fonts->AddFont(&fc); }
        const char* fb = "C:\\Windows\\Fonts\\seguisym.ttf";
        if (GetFileAttributesA(fb) != INVALID_FILE_ATTRIBUTES) { ImFontConfig fc; fc.MergeMode = true; io.Fonts->AddFontFromFileTTF(fb, 16.0f, &fc); }
        return f;
    };
    F_Reg  = withEmoji("C:\\Windows\\Fonts\\segoeui.ttf");
    F_Semi = withEmoji("C:\\Windows\\Fonts\\seguisb.ttf");
    F_Bold = withEmoji("C:\\Windows\\Fonts\\segoeuib.ttf");
    if (!F_Reg) F_Reg = io.Fonts->AddFontDefault();
    if (!F_Semi) F_Semi = F_Reg;
    if (!F_Bold) F_Bold = F_Semi;
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(0, 0);
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.ScrollbarSize = 0;
    ApplyTheme();
    g_name = Steam_PersonaName();
    Log("ui: fonts %s, Steam name %s", F_Reg && F_Bold ? "Segoe UI" : "default", g_name.empty() ? "(none)" : "found");
}

void UI_Draw() {
#ifdef GITMO_TEST
    extern int g_testFrame;
    extern float g_testBattery; extern int g_testBattState;
    static int lastPhase = -1;
    int phase = g_testFrame / 300;   // 0 drawer, 1 gitmo, 2 activity, 3 settings, 4 sheet, 5 dark drawer cracked, 6 dark settings cracked, 7 dead, 8 dead charging
                                     // 9 send: pick, 10 amount, 11 waiting, 12 incoming request (dark), 13 sent, 14 received over the drawer
                                     // 46 received-transfer sheet over an open SMS chat
    if (phase != lastPhase && phase >= 35) {
        lastPhase = phase;
        g_sheet = false; g_captureKey = false; g_cfg.cracked = false; g_chat = 0; g_emojiOpen = false; g_photoUi = PhNone;
        g_app = phase == 35 ? Drawer : phase == 43 || phase == 46 ? SmsApp : phase == 44 ? GitmoApp : RadioApp;
        if (phase == 43 || phase == 46) g_chat = 2;
        if (phase == 44) g_screen = Home;
        g_radShare = phase == 41;
        g_radTab = phase == 40 ? 1 : 0; g_radEdit = phase == 36 ? false : g_radEdit;
        g_radScr = phase == 37 || phase == 41 || phase == 42 ? RNow : phase == 38 || phase == 39 ? RForm : RList;
        RadioRefresh();
        if (phase == 38) { OpenForm(-1); strcpy_s(g_fUrl, "http://stream.example.net:8000/garage.mp3"); g_fName[0] = 0; strcpy_s(g_fGenre, "Rock"); g_formTesting = true; g_formNameAuto = true; g_formFocus = false; }
        if (phase == 39) { OpenForm(4); g_formDelAsk = true; }
        if (phase == 40) { strcpy_s(g_query, "classic rock"); Radio_Search("classic rock"); g_browseLoaded = true; }
        if (phase == 42) Radio_Play({ "Groove Salad", "https://ice6.somafm.com/groovesalad-128-mp3", "Ambient" });
        if (phase == 45) {   // what tapping Bob's link does
            std::string t, n, u;
            if (ParseRadioShare("\xF0\x9F\x93\xBB Listening to Just Between You And Me by Charley Pride on Truckers Radio USA http://streamer.radio.co/s81d30349a/low", t, n, u))
                RadioOpenShared(n, u);
        }
        bool dark = phase == 37 || phase == 39;
        if (g_cfg.dark != dark) { g_cfg.dark = dark; ApplyTheme(); }
        g_cfg.wallpaper = 1;
        g_testBattery = -1.0f; g_testBattState = 1;
    } else if (phase != lastPhase && phase >= 15) {   // 15 SMS list, 16 chat, 17 chat with someone offline (dark), 18 list (dark, cracked)
        lastPhase = phase;
        g_app = phase == 19 || phase == 20 || phase == 29 || phase == 30 ? SettingsApp : phase >= 24 && phase <= 28 ? Drawer : SmsApp;
        g_sheet = false; g_captureKey = false; g_cfg.cracked = phase == 18;
        g_cfg.notifyMode = phase == 29 ? 1 : 0;   // 29: Settings > Notifications on Vibrate (and the status bar icon)
        // 31 chat with photos, 32 the photo panel, 33 tap "Take photo" -> preview, 34 a photo big
        if (phase >= 31) { TestPhotos(); g_chat = 2; g_emojiOpen = false; g_draft[0] = 0; }
        if (phase == 32 || phase == 33) { g_photoUi = PhPanel; g_shots = Photo_SteamScreenshots(0, 60); }
        if (phase == 34) { g_photoUi = PhViewer; g_viewKey = "m" + std::to_string(0x701); Pic(g_viewKey, Sms_PhotoPath(0x701), 640); }
        if (phase >= 24) g_cfg.wallpaper = (phase - 24) % 5;   // 24-28: the home screen with each wallpaper
        g_eraseAsk = phase == 20;   // 19 Settings > Messages, 20 its "Erase all messages?" (dark)
        g_chat = phase == 16 || phase >= 21 ? 2 : phase == 17 ? 4 : 0; g_chatShown = (size_t)-1;
        if (phase == 16) strcpy_s(g_draft, "see you at the garage");
        // 21 chat with the emoji panel (recently used on top), 22 the same in dark, jumped to Trucks
        g_emojiOpen = phase == 21 || phase == 22;   // 23: closed; the mouse script taps the smiley, an emoji, the text field
        if (phase == 23) { strcpy_s(g_draft, "hi "); g_draftCursor = 3; g_cfg.recentEmoji.clear(); }
        if (phase == 21) { strcpy_s(g_draft, "on my way \xF0\x9F\x9A\x9A"); g_draftCursor = (int)strlen(g_draft); g_cfg.recentEmoji = "1F44D,1F602,1F69A,2764,1F525,1F64F,1F4B0,1F62C,1F60E"; }
        if (phase == 22) { g_draft[0] = 0; g_emojiJump = 4; }
        bool dark = (phase >= 17 && phase <= 18) || phase == 20 || phase == 22;
        if (g_cfg.dark != dark) { g_cfg.dark = dark; ApplyTheme(); }
        g_testBattery = -1.0f; g_testBattState = 1;
    } else if (phase != lastPhase && phase >= 9) {
        lastPhase = phase;
        static const Screen scr[6] = { SendPick, SendAmount, SendStatus, Home, SendStatus, Home };
        g_app = phase == 14 ? Drawer : GitmoApp;
        g_screen = scr[std::min(phase - 9, 5)];
        g_sheet = false; g_captureKey = false; g_cfg.cracked = false;
        g_sendName = "Bob"; g_sendPeer = 2; g_amount = "2500"; g_sendId = 0x11;
        if (g_cfg.dark != (phase == 12)) { g_cfg.dark = phase == 12; ApplyTheme(); }
        g_testBattery = -1.0f; g_testBattState = 1;
    } else if (phase != lastPhase) {
        lastPhase = phase;
        g_app = phase == 0 || phase == 5 ? Drawer : phase == 3 || phase == 6 ? SettingsApp : GitmoApp;
        g_screen = phase == 2 ? ActivityScr : Home;
        g_sheet = phase == 4; g_sheetTitle = "Send";
        g_captureKey = phase == 3;
        g_cfg.cracked = phase >= 5 && phase <= 6;
        g_cfg.wallpaper = phase == 0 ? 1 : phase == 5 ? 4 : 2;
        if (g_cfg.dark != (phase >= 5)) { g_cfg.dark = phase >= 5; ApplyTheme(); }
        g_testBattery = phase == 7 ? 0.0f : phase == 8 ? 0.1f : phase == 0 ? 2.4f : -1.0f;
        g_testBattState = phase == 8 || phase == 1 ? 1 : 2;
    }
#endif
    static MoneyState st;
    static uint64_t moneyVersion = ~uint64_t(0);
    Money_Snapshot(moneyVersion, st);
    static uint64_t txVersion = ~uint64_t(0);
    Tx_Snapshot(txVersion, g_tx);
    Sms_Snapshot(g_smsView.version, g_smsView);
    PicsPump();
    RadioRefresh();
#ifdef GITMO_TEST
    if (g_testFrame / 300 >= 15) g_smsView = DemoSms(g_testFrame / 300);
#endif
#ifdef GITMO_TEST
    if (g_testFrame / 300 >= 9) g_tx = DemoTx(g_testFrame / 300);
    if (g_testFrame / 300 == 2) {   // Activity: transfers that moved no money
        Activity d; GetLocalTime(&d.when); d.label = "To Bob"; d.status = "Bob declined"; d.amount = 2500;
        Activity e = d; e.label = "From Cat Driver"; e.status = "No answer"; e.amount = 400;
        Activity f; GetLocalTime(&f.when); f.label = "From Bob"; f.delta = 4000; f.balance = st.balance + 4000;
        st.activity.insert(st.activity.begin(), { f, d, e });
    }
#endif
    ImGuiIO& io = ImGui::GetIO();
    // Always the same share of the screen, whatever the resolution (Settings > Phone size).
    // Only a floor for tiny windows, and never taller than the screen itself.
    static const float kShare[3] = { 0.43f, 0.55f, 0.64f };   // ~30% smaller than the first S/M/L
    float H = io.DisplaySize.y * kShare[std::min(std::max(g_cfg.phoneSize, 0), 2)];
    H = std::min(std::max(H, 420.0f), io.DisplaySize.y * 0.98f);
    if (g_uiVrPanel) H = io.DisplaySize.y;   // headset: the phone fills its own panel texture
    float W = H * 0.49f;
    s = H / 800.0f;
    ImVec2 pos(io.DisplaySize.x - W - io.DisplaySize.x * 0.05f, (io.DisplaySize.y - H) * 0.5f);
    if (g_cfg.phoneX >= 0 && g_cfg.phoneY >= 0) pos = ImVec2(g_cfg.phoneX * io.DisplaySize.x, g_cfg.phoneY * io.DisplaySize.y);
    if (g_uiVrPanel) pos = ImVec2(0, 0);

    // Drag by the border: press on the black bezel (not the screen) and move.
    const float bezel = 14 * s;
    static bool dragging;
    ImVec2 m = io.MousePos;
    auto inside = [&](ImVec2 a, ImVec2 b) { return m.x >= a.x && m.y >= a.y && m.x < b.x && m.y < b.y; };
    // In VR the border is a little wider to grab (the pointer is harder to aim there).
    const float grab = g_uiVrPanel ? 22 * s : bezel;
    bool onBezel = inside(pos, ImVec2(pos.x + W, pos.y + H)) &&
                   !inside(ImVec2(pos.x + grab, pos.y + grab), ImVec2(pos.x + W - grab, pos.y + H - grab));
    if (g_uiVrPanel) {
        // VR: the overlay swings the panel around the head while the button is held.
        dragging = false;
        if (onBezel && ImGui::IsMouseClicked(0)) g_vrDragging = true;
        onBezel = onBezel || g_vrDragging;
    } else if (onBezel && ImGui::IsMouseClicked(0)) dragging = true;
    if (dragging) {
        if (ImGui::IsMouseDown(0)) {
            pos.x += io.MouseDelta.x; pos.y += io.MouseDelta.y;
            g_cfg.phoneX = pos.x / io.DisplaySize.x; g_cfg.phoneY = pos.y / io.DisplaySize.y;
        } else {
            dragging = false;
            SavePhonePos(g_cfg.phoneX, g_cfg.phoneY);   // remembered for next time
        }
    }
    if (!g_uiVrPanel) {
        pos.x = std::min(std::max(pos.x, -W * 0.5f), io.DisplaySize.x - W * 0.5f);
        pos.y = std::min(std::max(pos.y, 0.0f), io.DisplaySize.y - H * 0.25f);
    }
    if (onBezel || dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
#ifdef GITMO_TEST
    extern ImVec2 g_testBezel, g_testPhonePos; extern bool g_testSheet;
    g_testBezel = ImVec2(pos.x + bezel * 0.5f, pos.y + H * 0.5f); g_testPhonePos = pos; g_testSheet = g_sheet;
#endif
    float margin = 40 * s;   // room for the shadow outside the phone
    ImGui::SetNextWindowPos(ImVec2(pos.x - margin, pos.y - margin));
    ImGui::SetNextWindowSize(ImVec2(W + 2 * margin, H + 2 * margin));
    ImGui::Begin("##gitmo", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 o = pos, e(pos.x + W, pos.y + H);
    for (int i = g_uiVrPanel ? 0 : 6; i >= 1; i--) {   // soft shadow (flat screen only)
        float g = i * 3.0f * s;
        DL()->AddRectFilled(ImVec2(o.x - g, o.y - g + 8 * s), ImVec2(e.x + g, e.y + g + 8 * s), IM_COL32(0, 0, 0, 14), 48 * s + g);
    }
    DL()->AddRectFilled(o, e, T.bezel, 48 * s);
    if (onBezel || dragging)
        DL()->AddRect(ImVec2(o.x + 1, o.y + 1), ImVec2(e.x - 1, e.y - 1), IM_COL32(255, 255, 255, dragging || g_vrDragging ? 90 : 55), 48 * s, 0, 2.0f * s);
    ImVec2 a(o.x + bezel, o.y + bezel), b(e.x - bezel, e.y - bezel);

    // battery: dead below 0, wakes up again once it has a little charge (2%)
    float batt = Battery_Minutes();
    int battState = Battery_State();
#ifdef GITMO_TEST
    if (g_testBattery >= 0) { batt = g_testBattery; battState = g_testBattState; }
#endif
    g_uiBatt = batt; g_uiBattState = battState;
    if (batt <= 0.0f) g_dead = true;
    if (g_dead && batt >= 0.3f) g_dead = false;

    const tx::Incoming* incoming = g_dead ? nullptr : IncomingToShow();
    if (g_dead) {
        g_sheet = false;
        ScreenDead(a, b, battState == 1);
    } else if (g_app == Drawer) {
        g_behindSheet = incoming != nullptr;
        ScreenDrawer(a, b);
        g_behindSheet = false;
    } else {
        DL()->AddRectFilled(a, b, T.bg, 34 * s);
        StatusBar(a, ImVec2(b.x, a.y + 48 * s), T.text);
        bool modal = g_sheet || incoming;
        g_behindSheet = modal;   // the sheet takes all clicks while it is up
        if (g_app == GitmoApp) {
            float navH = 70 * s;
            ImVec2 ca(a.x, a.y + 52 * s), cb(b.x, b.y - navH);
            DL()->PushClipRect(ca, cb, true);
            if (g_screen == Home) ScreenHome(st, ca, cb);
            else if (g_screen == ActivityScr) ScreenActivity(st, ca, cb);
            else if (g_screen == SendPick) ScreenSendPick(ca, cb);
            else if (g_screen == SendAmount) ScreenSendAmount(st, ca, cb);
            else ScreenSendStatus(ca, cb);
            DL()->PopClipRect();
            NavBar(ImVec2(a.x, b.y - navH), b);
        } else if (g_app == SmsApp) {
            ScreenSms(ImVec2(a.x, a.y + 52 * s), b);
            HomePill(a, b, T.text);
        } else if (g_app == RadioApp) {
            ScreenRadio(ImVec2(a.x, a.y + 52 * s), b);
            HomePill(a, b, T.text);
        } else {
            ScreenSettingsApp(st, ImVec2(a.x, a.y + 52 * s), b);
            HomePill(a, b, T.text);
        }
        g_behindSheet = false;
        if (g_sheet && !incoming) Sheet(a, b);
    }
    if (incoming) IncomingSheet(*incoming, a, b);
    if (g_cfg.cracked) DrawCracks(a, b);
    ImGui::End();
    // the SMS engine marks messages read only while their conversation is really on screen
    if (g_viewSent && (g_dead || g_app != SmsApp || g_chat != g_viewSent)) { Sms_Viewing(0); g_viewSent = 0; }
    g_uiTyping = ImGui::GetIO().WantTextInput;
}

void UI_PhoneClosed() {   // overlay: the phone was put away
    if (g_viewSent) { Sms_Viewing(0); g_viewSent = 0; }
    g_eraseAsk = false;
    g_emojiOpen = false;
    ClosePhotos();
    g_radShare = false;
}
