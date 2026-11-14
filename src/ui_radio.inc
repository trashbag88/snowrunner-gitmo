// ---- Radio app (part of ui.cpp: uses its drawing helpers) ----------------------------------
// My stations (saved in Gitmo-radio.ini: add from a link, edit, delete), Browse (the
// radio-browser.info directory), Now playing (volume, save, share over SMS), and a mini
// player on the home screen. The sound itself is radio.cpp.
#include <unordered_map>

enum RadScr { RList, RNow, RForm };
static RadScr g_radScr = RList;
static int g_radTab = 0;                     // 0 My stations, 1 Browse
static bool g_radEdit = false;               // My stations in edit mode: tap a station to edit it
static std::vector<RadioStation> g_radList;  // this frame's copy of the saved stations
static unsigned g_radListVer = 0;
static int g_formIdx = -1;                   // the form: -1 add, else editing that station
static char g_fName[160], g_fUrl[1024], g_fGenre[64];
static bool g_formDelAsk, g_formFocus, g_formNameAuto, g_formTesting;
static std::string g_formErr;
static char g_query[128];
static bool g_browseLoaded;
static unsigned g_radSearchResultVersion;
static std::vector<RadioFound> g_radSearchFound;
static std::string g_radSearchErr, g_radSearchQuery;
static bool g_radShare;                      // the "Share to" sheet
static std::string g_radToast;
static ULONGLONG g_radToastUntil;
static std::unordered_map<std::string, int> g_radUrlIndex;

static bool RadioActive() { return g_rad.state >= 1 && g_rad.state <= 3; }
static void Toast(const std::string& t) { g_radToast = t; g_radToastUntil = GetTickCount64() + 2500; }
static bool SaveRadioStations(const std::vector<RadioStation>& list, const char* successToast) {
    if (!Radio_SetStations(list)) { Toast("Couldn't save stations"); return false; }
    Toast(successToast);
    return true;
}
static size_t Utf8PrefixLength(const std::string& value, size_t maxBytes) {
    size_t length = std::min(maxBytes, value.size());
    if (length < value.size())
        while (length > 0 && ((unsigned char)value[length] & 0xC0) == 0x80) length--;
    return length;
}
static void CopyUtf8(char* destination, size_t capacity, const std::string& value) {
    if (!capacity) return;
    std::string prefix = value.substr(0, Utf8PrefixLength(value, capacity - 1));
    strcpy_s(destination, capacity, prefix.c_str());
}
static void RebuildStationIndex() {
    g_radUrlIndex.clear();
    g_radUrlIndex.reserve(g_radList.size());
    for (size_t i = 0; i < g_radList.size(); i++)
        g_radUrlIndex.emplace(g_radList[i].url, (int)i);
}

#ifdef GITMO_TEST
static void RadioTestState(int phase) {   // made-up stations and playback for the screenshots
    g_radList = { { "Classic Rock 109", "http://test/rock", "Classic rock" }, { "Country Roads FM", "http://test/country", "Country" },
                  { "Lo-fi Trucking", "http://test/lofi", "Chill" }, { "BBC World Service", "http://test/bbc", "News" },
                  { "Dad's Garage Radio", "http://stream.example.net:8000/garage.mp3", "" } };
    RebuildStationIndex();
    RadioState r; r.state = 3; r.url = "http://test/rock"; r.name = "Classic Rock 109"; r.genre = "Classic rock";
    r.icyName = "CLASSIC ROCK 109"; r.title = "Fleetwood Mac - Dreams"; r.codec = "MP3"; r.kbps = 128; r.volume = Radio_Volume();
    if (phase == 38) { r.url = "http://stream.example.net:8000/garage.mp3"; r.name = ""; r.icyName = "Dad's Garage Radio"; r.title.clear(); }
    g_rad = r;
}
#endif

static void RadioRefresh() {
    unsigned v = Radio_StationsVersion();
    if (v != g_radListVer) { g_radList = Radio_Stations(); g_radListVer = v; RebuildStationIndex(); }
    g_rad = Radio_State();
#ifdef GITMO_TEST
    { extern int g_testFrame; int ph = g_testFrame / 300; if (ph >= 35 && ph <= 41 && ph != 40) RadioTestState(ph); }
#endif
}

static int SavedIndex(const std::string& url) {
    auto it = g_radUrlIndex.find(url);
    return it == g_radUrlIndex.end() ? -1 : it->second;
}
static ImU32 TileColor(const std::string& key) {
    static const unsigned pal[8] = { 0x5E5CE6, 0xFF9F0A, 0x32ADE6, 0xFF453A, 0x30B158, 0xBF5AF2, 0xFF375F, 0x0A84FF };
    unsigned h = 2166136261u;
    for (char c : key) h = (h ^ (uint8_t)c) * 16777619u;
    return Hex(pal[h % 8]);
}
static std::string StationLabel() {
    return !g_rad.name.empty() ? g_rad.name : !g_rad.icyName.empty() ? g_rad.icyName : std::string("Radio");
}
static void SplitTitle(const std::string& t, std::string& song, std::string& artist) {
    size_t d = t.find(" - ");
    if (d == std::string::npos) { song = t; artist.clear(); return; }
    artist = t.substr(0, d); song = t.substr(d + 3);
}
static std::string HostOf(const std::string& url) {
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    size_t at = authority.rfind('@');
    if (at != std::string::npos) authority.erase(0, at + 1);
    if (!authority.empty() && authority[0] == '[') {
        size_t close = authority.find(']');
        if (close != std::string::npos) return authority.substr(0, close + 1);
    }
    size_t colon = authority.find(':');
    if (colon != std::string::npos) authority.resize(colon);
    return authority;
}
static bool IsWebLink(const std::string& u) {
    std::string l = u.substr(0, 8);
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    return l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0;
}
// ---- icons ------------------------------------------------------------------------------------
static void IconStar(ImVec2 c, float r, ImU32 col, bool filled) {
    ImVec2 p[10];
    for (int i = 0; i < 10; i++) {
        float ang = -1.5707963f + i * 0.62831853f, rr = i % 2 ? r * 0.45f : r;
        p[i] = ImVec2(c.x + cosf(ang) * rr, c.y + sinf(ang) * rr);
    }
    if (filled) DL()->AddConcavePolyFilled(p, 10, col);
    else DL()->AddPolyline(p, 10, col, ImDrawFlags_Closed, std::max(1.2f, r * 0.13f));
}
static void IconStop(ImVec2 c, float r, ImU32 col) { DL()->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, r * 0.25f); }
static void IconPlay(ImVec2 c, float r, ImU32 col) {
    DL()->AddTriangleFilled(ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x - r * 0.7f, c.y + r), col);
}
static void IconShare(ImVec2 c, float r, ImU32 col) {
    float th = std::max(1.3f, r * 0.13f);
    ImVec2 box[4] = { ImVec2(c.x - r * 0.35f, c.y - r * 0.15f), ImVec2(c.x - r * 0.65f, c.y - r * 0.15f), ImVec2(c.x - r * 0.65f, c.y + r * 0.8f), ImVec2(c.x + r * 0.65f, c.y + r * 0.8f) };
    DL()->AddPolyline(box, 4, col, 0, th);
    ImVec2 box2[2] = { ImVec2(c.x + r * 0.65f, c.y + r * 0.8f), ImVec2(c.x + r * 0.65f, c.y - r * 0.15f) };
    DL()->AddPolyline(box2, 2, col, 0, th);
    DL()->AddLine(ImVec2(c.x + r * 0.65f, c.y - r * 0.15f), ImVec2(c.x + r * 0.35f, c.y - r * 0.15f), col, th);
    DL()->AddLine(ImVec2(c.x, c.y + r * 0.35f), ImVec2(c.x, c.y - r * 0.9f), col, th);
    ImVec2 tip[3] = { ImVec2(c.x - r * 0.35f, c.y - r * 0.55f), ImVec2(c.x, c.y - r * 0.9f), ImVec2(c.x + r * 0.35f, c.y - r * 0.55f) };
    DL()->AddPolyline(tip, 3, col, 0, th);
}
static void IconChevronRight(ImVec2 c, float r, ImU32 col) {
    ImVec2 p[3] = { ImVec2(c.x - r * 0.3f, c.y - r * 0.6f), ImVec2(c.x + r * 0.3f, c.y), ImVec2(c.x - r * 0.3f, c.y + r * 0.6f) };
    DL()->AddPolyline(p, 3, col, 0, std::max(1.5f, r * 0.2f));
}
static void IconPlus(ImVec2 c, float r, ImU32 col) {
    DL()->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, std::max(1.5f, r * 0.22f));
    DL()->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, std::max(1.5f, r * 0.22f));
}
static void IconSpeaker(ImVec2 c, float r, ImU32 col, int waves) {
    DL()->AddRectFilled(ImVec2(c.x - r * 0.75f, c.y - r * 0.3f), ImVec2(c.x - r * 0.35f, c.y + r * 0.3f), col, r * 0.08f);
    ImVec2 cone[4] = { ImVec2(c.x - r * 0.4f, c.y - r * 0.3f), ImVec2(c.x + r * 0.1f, c.y - r * 0.75f), ImVec2(c.x + r * 0.1f, c.y + r * 0.75f), ImVec2(c.x - r * 0.4f, c.y + r * 0.3f) };
    DL()->AddConvexPolyFilled(cone, 4, col);
    for (int i = 0; i < waves; i++) {
        DL()->PathArcTo(ImVec2(c.x + r * 0.1f, c.y), r * (0.45f + 0.35f * i), -0.85f, 0.85f, 10);
        DL()->PathStroke(col, 0, std::max(1.2f, r * 0.13f));
    }
}
static void EqBars(ImVec2 c, float r, ImU32 col) {   // the "playing" animation
    float t = (float)ImGui::GetTime();
    for (int i = 0; i < 3; i++) {
        float h = r * (0.45f + 0.55f * fabsf(sinf(t * (3.1f + i * 1.3f) + i * 1.7f)));
        float x = c.x + (i - 1) * r * 0.62f;
        DL()->AddRectFilled(ImVec2(x - r * 0.2f, c.y + r - h * 2), ImVec2(x + r * 0.2f, c.y + r), col, r * 0.1f);
    }
}
static void Tile(ImVec2 c, float size, const std::string& key) {
    DL()->AddRectFilled(ImVec2(c.x - size / 2, c.y - size / 2), ImVec2(c.x + size / 2, c.y + size / 2), TileColor(key), size * 0.24f);
    IconNote(c, size * 0.32f, IM_COL32(255, 255, 255, 255));
}

// ---- small widgets ------------------------------------------------------------------------------
// a text box like the SMS one; Enter returns true
static bool RadField(const char* id, char* buf, size_t n, ImVec2 a, ImVec2 b, const char* placeholder, bool& focus) {
    DL()->AddRectFilled(a, b, T.outBg, 10 * s);
    float fs = 14.5f * s;
    ImVec2 tp(a.x + 11 * s, (a.y + b.y) * 0.5f - fs * 0.5f - 1 * s);
    ImGui::SetCursorScreenPos(tp);
    ImGui::PushFont(F_Reg, fs);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, T.text);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, Fade(kBlue, 0.35f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::SetNextItemWidth(b.x - a.x - 22 * s);
    if (focus && !g_behindSheet) { ImGui::SetKeyboardFocusHere(); focus = false; }
    ImGui::BeginDisabled(g_behindSheet);
    bool enter = ImGui::InputText(id, buf, n, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::EndDisabled();
    bool active = ImGui::IsItemActive();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    ImGui::PopFont();
    if (!buf[0]) Text(F_Reg, fs, tp, T.sub, placeholder);
    if (active) DL()->AddRect(a, b, kBlue, 10 * s, 0, 1.6f * s);
    return enter;
}
static int Segmented(const char* idp, const char* const* names, int n, int cur, ImVec2 p, float w) {
    DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + 32 * s), T.outBg, 16 * s);
    float seg = (w - 6 * s) / n;
    int picked = -1;
    for (int i = 0; i < n; i++) {
        ImVec2 a0(p.x + 3 * s + i * seg, p.y + 3 * s), a1(a0.x + seg, p.y + 29 * s);
        char id[24]; sprintf_s(id, "%s%d", idp, i);
        if (Hit(id, a0, a1) && i != cur) picked = i;
        if (i == cur) DL()->AddRectFilled(a0, a1, T.card, 13 * s);
        TextCenter(F_Semi, 13 * s, ImVec2((a0.x + a1.x) * 0.5f, a0.y + 4 * s), i == cur ? T.text : T.sub, names[i]);
    }
    return picked;
}
static const char* RadioStateText() {
    switch (g_rad.state) { case 1: return "Connecting..."; case 2: return "Buffering..."; case 3: return "Playing"; case 4: return "Couldn't play"; }
    return "Stopped";
}

// the mini player: on the home screen (over the wallpaper) and under the station lists
static void MiniPlayer(ImVec2 a, ImVec2 b, const char* id, bool onWallpaper) {
    ImU32 fg = onWallpaper ? IM_COL32(255, 255, 255, 255) : T.text, sub = onWallpaper ? IM_COL32(255, 255, 255, 170) : T.sub;
    DL()->AddRectFilled(a, b, onWallpaper ? IM_COL32(0, 0, 0, 115) : T.card, 16 * s);
    if (!onWallpaper) DL()->AddRect(a, b, T.divider, 16 * s, 0, 1.0f);
    float my = (a.y + b.y) * 0.5f;
    ImVec2 stopC(b.x - 28 * s, my);
    bool open = Hit(id, a, ImVec2(stopC.x - 22 * s, b.y));
    if (!g_behindSheet && ImGui::IsItemHovered()) DL()->AddRectFilled(a, b, IM_COL32(255, 255, 255, onWallpaper ? 18 : 0), 16 * s);
    Tile(ImVec2(a.x + 30 * s, my), 38 * s, StationLabel());
    std::string song, artist; SplitTitle(g_rad.title, song, artist);
    std::string l1 = g_rad.title.empty() ? StationLabel() : artist.empty() ? song : song + " \xC2\xB7 " + artist;
    std::string l2 = g_rad.state == 3 ? (g_rad.title.empty() ? std::string("Live") : StationLabel()) : std::string(RadioStateText());
    float tw = stopC.x - 24 * s - (a.x + 58 * s);
    Text(F_Semi, 13.5f * s, ImVec2(a.x + 58 * s, my - 18 * s), fg, Fit(F_Semi, 13.5f * s, l1, tw).c_str());
    Text(F_Reg, 12 * s, ImVec2(a.x + 58 * s, my + 1 * s), g_rad.state == 4 ? kRedText() : sub, Fit(F_Reg, 12 * s, l2, tw).c_str());
    char sid[32]; sprintf_s(sid, "%s_stop", id);
    bool stop = Hit(sid, ImVec2(stopC.x - 20 * s, stopC.y - 20 * s), ImVec2(stopC.x + 20 * s, stopC.y + 20 * s));
    if (!g_behindSheet && ImGui::IsItemHovered()) DL()->AddCircleFilled(stopC, 18 * s, onWallpaper ? IM_COL32(255, 255, 255, 40) : Fade(T.text, 0.08f));
    IconStop(stopC, 7 * s, fg);
    if (stop) Radio_Stop();
    else if (open) { g_app = RadioApp; g_radScr = RNow; g_radShare = false; }
}

// ---- My stations ------------------------------------------------------------------------------------
static void OpenForm(int idx) {
    g_formIdx = idx; g_formDelAsk = false; g_formErr.clear(); g_formNameAuto = false; g_formTesting = false;
    if (idx >= 0 && idx < (int)g_radList.size()) {
        CopyUtf8(g_fName, sizeof g_fName, g_radList[idx].name);
        CopyUtf8(g_fUrl, sizeof g_fUrl, g_radList[idx].url);
        CopyUtf8(g_fGenre, sizeof g_fGenre, g_radList[idx].genre);
        g_formFocus = false;
    } else {
        g_fName[0] = g_fUrl[0] = g_fGenre[0] = 0;
        g_formFocus = true;   // straight to the link box
    }
    g_radScr = RForm;
}

static void StationRow(const char* id, ImVec2 p, float w, float rh, const std::string& name, const std::string& sub, ImU32 subCol,
                       bool playing, int right, bool divider, bool& click, bool* star) {
    float starW = star ? 48 * s : 0;
    click = Hit(id, p, ImVec2(p.x + w - starW, p.y + rh));
    if (!g_behindSheet && ImGui::IsItemHovered()) DL()->AddRectFilled(ImVec2(p.x + 4 * s, p.y + 2 * s), ImVec2(p.x + w - 4 * s, p.y + rh - 2 * s), Fade(T.text, 0.04f), 12 * s);
    Tile(ImVec2(p.x + 32 * s, p.y + rh * 0.5f), 38 * s, name);
    float tw = w - 64 * s - 44 * s;
    Text(F_Semi, 15 * s, ImVec2(p.x + 62 * s, p.y + 11 * s), T.text, Fit(F_Semi, 15 * s, name, tw).c_str());
    Text(F_Reg, 12.5f * s, ImVec2(p.x + 62 * s, p.y + 32 * s), subCol, Fit(F_Reg, 12.5f * s, sub, tw).c_str());
    ImVec2 rc(p.x + w - 24 * s, p.y + rh * 0.5f);
    if (right == 1) IconChevronRight(rc, 10 * s, T.sub);
    else if (playing) EqBars(rc, 8 * s, kGreen);
    if (star) {
        char sid[40]; sprintf_s(sid, "%s_star", id);
        *star = Hit(sid, ImVec2(p.x + w - starW, p.y), ImVec2(p.x + w, p.y + rh));
        bool on = right == 2;
        IconStar(playing ? ImVec2(rc.x - 26 * s, rc.y) : rc, 10 * s, on ? Hex(0xFFCC00) : T.sub, on);
        if (playing) EqBars(rc, 8 * s, kGreen);
    }
    if (divider) DL()->AddLine(ImVec2(p.x + 62 * s, p.y + rh), ImVec2(p.x + w - 16 * s, p.y + rh), T.divider, 1.0f);
}

static void RadioMine(float x, float y, float w, float bottom) {
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginChild("##radmine", ImVec2(w, bottom - y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float y0 = p.y, rh = 60 * s;
    size_t n = g_radList.size();
    if (n == 0) {
        DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + 92 * s), T.card, 16 * s);
        TextCenter(F_Semi, 15 * s, ImVec2(p.x + w * 0.5f, p.y + 16 * s), T.text, "No stations yet");
        TextCenter(F_Reg, 13 * s, ImVec2(p.x + w * 0.5f, p.y + 40 * s), T.sub, "Add one from its stream link,");
        TextCenter(F_Reg, 13 * s, ImVec2(p.x + w * 0.5f, p.y + 58 * s), T.sub, "or find one in Browse.");
        p.y += 104 * s;
    } else {
        DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + n * rh), T.card, 16 * s);
        for (size_t i = 0; i < n; i++) {
            const RadioStation& st = g_radList[i];
            bool cur = g_rad.url == st.url && (RadioActive() || g_rad.state == 4);
            std::string sub = st.genre.empty() ? HostOf(st.url) : st.genre;
            ImU32 subCol = T.sub;
            if (cur) { sub = g_rad.state == 4 ? g_rad.err : RadioStateText(); subCol = g_rad.state == 4 ? kRedText() : kGreen; }
            char id[24]; sprintf_s(id, "radst%zu", i);
            bool click;
            StationRow(id, ImVec2(p.x, p.y + i * rh), w, rh, st.name, sub, subCol, cur && g_rad.state != 4, g_radEdit ? 1 : 0, i + 1 < n, click, nullptr);
            if (click) {
                if (g_radEdit) OpenForm((int)i);
                else if (cur && g_rad.state != 4) g_radScr = RNow;
                else Radio_Play(st);
            }
        }
        p.y += n * rh + 12 * s;
    }
    // add a station from its link
    DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + 54 * s), T.card, 16 * s);
    bool add = Hit("radadd", p, ImVec2(p.x + w, p.y + 54 * s));
    if (!g_behindSheet && ImGui::IsItemHovered()) DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + 54 * s), Fade(T.text, 0.04f), 16 * s);
    ImVec2 pc(p.x + 32 * s, p.y + 27 * s);
    DL()->AddRectFilled(ImVec2(pc.x - 19 * s, pc.y - 19 * s), ImVec2(pc.x + 19 * s, pc.y + 19 * s), kBlue, 9 * s);
    IconPlus(pc, 8 * s, IM_COL32(255, 255, 255, 255));
    Text(F_Semi, 15 * s, ImVec2(p.x + 62 * s, p.y + 16 * s), kBlue, "Add station");
    if (add) OpenForm(-1);
    p.y += 66 * s;
    if (n && !g_radEdit) { TextCenter(F_Reg, 12 * s, ImVec2(p.x + w * 0.5f, p.y), T.sub, "Tap Edit to rename, change or delete a station"); p.y += 24 * s; }
    ImGui::SetCursorScreenPos(ImVec2(x, y0));
    ImGui::Dummy(ImVec2(w, p.y - y0));
    DragScroll();
    ImGui::EndChild();
}

// ---- Browse ---------------------------------------------------------------------------------------------
static void RadioBrowse(float x, float y, float w, float bottom) {
    float bw = 78 * s;
    bool noFocus = false;
    bool go = RadField("##radq", g_query, sizeof g_query, ImVec2(x, y), ImVec2(x + w - bw - 8 * s, y + 38 * s), "Name or genre", noFocus);
    if (Pill("radsearch", "Search", ImVec2(x + w - bw, y + 1 * s), ImVec2(x + w, y + 37 * s), true)) go = true;
    if (go) { Radio_Search(g_query); g_browseLoaded = true; }
    y += 48 * s;
    int st = Radio_SearchResult(g_radSearchResultVersion, g_radSearchFound, g_radSearchErr, g_radSearchQuery);
    std::string cap = st == 1 ? "SEARCHING..." : g_radSearchQuery.empty() ? "MOST POPULAR" : "RESULTS FOR \xE2\x80\x9C" + g_radSearchQuery + "\xE2\x80\x9D";
    for (auto& c : cap) c = (char)toupper((unsigned char)c);
    Text(F_Semi, 12 * s, ImVec2(x + 4 * s, y), T.sub, Fit(F_Semi, 12 * s, cap, w - 40 * s).c_str());
    if (st == 1) Spinner(ImVec2(x + w - 10 * s, y + 8 * s), 7 * s, T.sub);
    y += 22 * s;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginChild("##radbrowse", ImVec2(w, bottom - y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float y0 = p.y, rh = 60 * s;
    if (st == 3) {
        DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + 78 * s), T.card, 16 * s);
        TextCenter(F_Semi, 14 * s, ImVec2(p.x + w * 0.5f, p.y + 16 * s), T.text, "Couldn't reach the station directory");
        TextCenter(F_Reg, 12.5f * s, ImVec2(p.x + w * 0.5f, p.y + 40 * s), T.sub, Fit(F_Reg, 12.5f * s, g_radSearchErr, w - 24 * s).c_str());
        p.y += 90 * s;
    } else if (st == 2 && g_radSearchFound.empty()) {
        DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + 78 * s), T.card, 16 * s);
        TextCenter(F_Semi, 14 * s, ImVec2(p.x + w * 0.5f, p.y + 16 * s), T.text, "No stations found");
        TextCenter(F_Reg, 12.5f * s, ImVec2(p.x + w * 0.5f, p.y + 40 * s), T.sub, "Try a shorter name, or a genre like jazz");
        p.y += 90 * s;
    } else if (!g_radSearchFound.empty()) {
        DL()->AddRectFilled(p, ImVec2(p.x + w, p.y + g_radSearchFound.size() * rh), T.card, 16 * s);
        for (size_t i = 0; i < g_radSearchFound.size(); i++) {
            const RadioFound& f = g_radSearchFound[i];
            std::string sub = f.codec;
            if (f.kbps) sub += " \xC2\xB7 " + std::to_string(f.kbps) + "k";
            if (!f.country.empty()) sub += " \xC2\xB7 " + f.country;
            if (!f.genre.empty()) sub += " \xC2\xB7 " + f.genre;
            bool cur = g_rad.url == f.url && RadioActive();
            int saved = SavedIndex(f.url);
            char id[24]; sprintf_s(id, "radf%zu", i);
            bool click, star;
            StationRow(id, ImVec2(p.x, p.y + i * rh), w, rh, f.name, cur ? std::string(RadioStateText()) : sub, cur ? kGreen : T.sub, cur, saved >= 0 ? 2 : 0, i + 1 < g_radSearchFound.size(), click, &star);
            if (star) {
                std::vector<RadioStation> list = g_radList;
                const char* toast = saved >= 0 ? "Removed from My stations" : "Added to My stations";
                if (saved >= 0) list.erase(list.begin() + saved);
                else list.push_back({ f.name, f.url, f.genre });
                SaveRadioStations(list, toast);
            } else if (click) {
                if (cur) g_radScr = RNow;
                else { Radio_Play({ f.name, f.url, f.genre }); Radio_CountClick(f.uuid); }
            }
        }
        p.y += g_radSearchFound.size() * rh + 10 * s;
        TextCenter(F_Reg, 11.5f * s, ImVec2(p.x + w * 0.5f, p.y), T.sub, "Stations from radio-browser.info");
        p.y += 26 * s;
    }
    ImGui::SetCursorScreenPos(ImVec2(x, y0));
    ImGui::Dummy(ImVec2(w, std::max(1.0f, p.y - y0)));
    DragScroll();
    ImGui::EndChild();
}

static void RadioList(ImVec2 a, ImVec2 b) {
    float x = a.x + 20 * s, w = b.x - a.x - 40 * s, y = a.y + 6 * s;
    Text(F_Bold, 28 * s, ImVec2(x, y), T.text, "Radio");
    if (g_radTab == 0 && !g_radList.empty()) {
        const char* lbl = g_radEdit ? "Done" : "Edit";
        ImVec2 sz = Measure(F_Semi, 15.5f * s, lbl);
        if (Hit("radedit", ImVec2(x + w - sz.x - 10 * s, y + 2 * s), ImVec2(x + w + 6 * s, y + 36 * s))) g_radEdit = !g_radEdit;
        TextRight(F_Semi, 15.5f * s, ImVec2(x + w, y + 10 * s), kBlue, lbl);
    }
    y += 48 * s;
    static const char* tabs[2] = { "My stations", "Browse" };
    int pick = Segmented("radtab", tabs, 2, g_radTab, ImVec2(x, y), w);
    if (pick >= 0) {
        g_radTab = pick; g_radEdit = false;
        if (pick == 1 && !g_browseLoaded) { Radio_Search(""); g_browseLoaded = true; }
    }
    y += 44 * s;
    bool mini = RadioActive();
    float miniH = 60 * s;
    float bottom = b.y - 26 * s - (mini ? miniH + 8 * s : 0);
    if (g_radTab == 0) RadioMine(x, y, w, bottom);
    else RadioBrowse(x, y, w, bottom);
    if (mini) MiniPlayer(ImVec2(a.x + 12 * s, bottom + 4 * s), ImVec2(b.x - 12 * s, bottom + 4 * s + miniH), "radmini", false);
}

// ---- Now playing --------------------------------------------------------------------------------------------
static void VolumeSlider(ImVec2 a, ImVec2 b) {
    static bool dragging;
    Hit("radvol", ImVec2(a.x - 10 * s, a.y - 14 * s), ImVec2(b.x + 10 * s, b.y + 14 * s));
    if (!g_behindSheet && ImGui::IsItemActive()) {
        float t = (ImGui::GetIO().MousePos.x - a.x) / (b.x - a.x);
        int v = (int)roundf(std::min(1.0f, std::max(0.0f, t)) * 100);
        if (v != Radio_Volume()) Radio_SetVolume(v, false);
        dragging = true;
    } else if (dragging) { dragging = false; Radio_SetVolume(Radio_Volume(), true); }   // saved when let go
    float t = Radio_Volume() / 100.0f, h = b.y - a.y, kx = a.x + (b.x - a.x) * t;
    DL()->AddRectFilled(a, b, T.divider, h * 0.5f);
    if (t > 0) DL()->AddRectFilled(a, ImVec2(std::max(kx, a.x + h), b.y), T.text, h * 0.5f);
    ImVec2 kc(kx, (a.y + b.y) * 0.5f);
    DL()->AddCircleFilled(kc, 10 * s, IM_COL32(255, 255, 255, 255));
    DL()->AddCircle(kc, 10 * s, T.divider, 0, 1.0f);
}

static void RadioNow(ImVec2 a, ImVec2 b) {
    float W = b.x - a.x, cx = (a.x + b.x) * 0.5f, x = a.x + 16 * s;
    // back, like SMS: "< Radio"
    bool back = Hit("radback", ImVec2(x - 6 * s, a.y), ImVec2(x + 80 * s, a.y + 40 * s));
    ImU32 bc = !g_behindSheet && ImGui::IsItemHovered() ? Fade(kBlue, 0.7f) : kBlue;
    ImVec2 ch[3] = { ImVec2(x + 9 * s, a.y + 8 * s), ImVec2(x + 1 * s, a.y + 17 * s), ImVec2(x + 9 * s, a.y + 26 * s) };
    DL()->AddPolyline(ch, 3, bc, 0, 2.6f * s);
    Text(F_Reg, 15.5f * s, ImVec2(x + 15 * s, a.y + 7 * s), bc, "Radio");
    if (back) { g_radScr = RList; return; }
    if (g_rad.url.empty()) {
        TextCenter(F_Semi, 16 * s, ImVec2(cx, a.y + 160 * s), T.text, "Nothing playing");
        TextCenter(F_Reg, 13 * s, ImVec2(cx, a.y + 186 * s), T.sub, "Pick a station in My stations or Browse.");
        return;
    }
    std::string station = StationLabel();
    float ts = 168 * s, ty = a.y + 50 * s;
    DL()->AddRectFilled(ImVec2(cx - ts / 2, ty), ImVec2(cx + ts / 2, ty + ts), TileColor(station), 26 * s);
    IconNote(ImVec2(cx, ty + ts / 2), ts * 0.3f, IM_COL32(255, 255, 255, 255));
    float y = ty + ts + 22 * s;
    std::string song, artist; SplitTitle(g_rad.title, song, artist);
    if (g_rad.title.empty()) { song = station; artist = g_rad.genre; }
    TextCenter(F_Bold, 20 * s, ImVec2(cx, y), T.text, Fit(F_Bold, 20 * s, song, W - 40 * s).c_str());
    if (!artist.empty()) TextCenter(F_Reg, 15 * s, ImVec2(cx, y + 28 * s), T.sub, Fit(F_Reg, 15 * s, artist, W - 40 * s).c_str());
    y += 60 * s;
    // LIVE . station . 128k MP3   (or what it's doing / what went wrong)
    if (g_rad.state == 4) {
        float ew = W - 48 * s;
        DL()->AddText(F_Reg, 12.5f * s, ImVec2(a.x + 24 * s, y), kRedText(), g_rad.err.c_str(), nullptr, ew);
    } else {
        std::string line = station;
        if (g_rad.kbps) line += " \xC2\xB7 " + std::to_string(g_rad.kbps) + "k";
        if (!g_rad.codec.empty()) line += (g_rad.kbps ? " " : " \xC2\xB7 ") + g_rad.codec;
        bool live = g_rad.state == 3;
        const char* badge = live ? "LIVE" : g_rad.state == 0 ? "STOPPED" : g_rad.state == 1 ? "CONNECTING" : "BUFFERING";
        float bwid = Measure(F_Bold, 11 * s, badge).x + 12 * s;
        line = Fit(F_Reg, 12.5f * s, line, W - 60 * s - bwid);
        float lw = Measure(F_Reg, 12.5f * s, line.c_str()).x, total = bwid + 7 * s + lw, bx = cx - total / 2;
        DL()->AddRectFilled(ImVec2(bx, y), ImVec2(bx + bwid, y + 18 * s), live ? Hex(0xFF375F) : T.outBg, 5 * s);
        TextCenter(F_Bold, 11 * s, ImVec2(bx + bwid / 2, y + 2 * s), live ? IM_COL32(255, 255, 255, 255) : T.sub, badge);
        Text(F_Reg, 12.5f * s, ImVec2(bx + bwid + 7 * s, y + 1 * s), T.sub, line.c_str());
    }
    y += 46 * s;
    // star  [ stop / play ]  share
    bool active = RadioActive();
    ImVec2 pc(cx, y + 34 * s);
    bool pp = Hit("radpp", ImVec2(pc.x - 36 * s, pc.y - 36 * s), ImVec2(pc.x + 36 * s, pc.y + 36 * s));
    bool hov = !g_behindSheet && ImGui::IsItemHovered();
    DL()->AddCircleFilled(pc, (hov ? 35 : 34) * s, T.text);
    if (active) IconStop(pc, 11 * s, T.bg); else IconPlay(ImVec2(pc.x + 3 * s, pc.y), 14 * s, T.bg);
    if (pp) { if (active) Radio_Stop(); else Radio_Play({ g_rad.name, g_rad.url, g_rad.genre }); }
    int saved = SavedIndex(g_rad.url);
    ImVec2 sc(cx - 96 * s, pc.y);
    if (Hit("radstar", ImVec2(sc.x - 24 * s, sc.y - 24 * s), ImVec2(sc.x + 24 * s, sc.y + 24 * s))) {
        std::vector<RadioStation> list = g_radList;
        const char* toast = saved >= 0 ? "Removed from My stations" : "Added to My stations";
        if (saved >= 0) list.erase(list.begin() + saved);
        else list.push_back({ station, g_rad.url, g_rad.genre });
        SaveRadioStations(list, toast);
    }
    IconStar(sc, 13 * s, saved >= 0 ? Hex(0xFFCC00) : T.sub, saved >= 0);
    ImVec2 shc(cx + 96 * s, pc.y);
    if (Hit("radshare", ImVec2(shc.x - 24 * s, shc.y - 24 * s), ImVec2(shc.x + 24 * s, shc.y + 24 * s))) g_radShare = true;
    IconShare(shc, 13 * s, T.sub);
    y += 98 * s;
    // volume
    IconSpeaker(ImVec2(a.x + 30 * s, y), 9 * s, T.sub, 0);
    VolumeSlider(ImVec2(a.x + 50 * s, y - 3 * s), ImVec2(b.x - 50 * s, y + 3 * s));
    IconSpeaker(ImVec2(b.x - 32 * s, y), 9 * s, T.sub, 2);
    char vl[64]; sprintf_s(vl, "Radio volume %d%%  \xC2\xB7  the game's volume is separate", Radio_Volume());
    TextCenter(F_Reg, 11.5f * s, ImVec2(cx, y + 18 * s), T.sub, vl);
}

// ---- Add / edit a station ---------------------------------------------------------------------------------
static void RadioForm(ImVec2 a, ImVec2 b) {
    float x = a.x + 20 * s, w = b.x - a.x - 40 * s, y = a.y + 6 * s, cx = (a.x + b.x) * 0.5f;
    bool editing = g_formIdx >= 0 && g_formIdx < (int)g_radList.size();
    std::string url = gitmo_shared::TrimAsciiWhitespace(g_fUrl);
    bool testingThis = !url.empty() && g_rad.url == url;
    // header: Cancel   Add station   Save
    bool cancel = Hit("radfcancel", ImVec2(x - 6 * s, y), ImVec2(x + 70 * s, y + 32 * s));
    Text(F_Reg, 15.5f * s, ImVec2(x, y + 6 * s), kBlue, "Cancel");
    TextCenter(F_Semi, 16 * s, ImVec2(cx, y + 6 * s), T.text, editing ? "Edit station" : "Add station");
    ImVec2 ssz = Measure(F_Semi, 15.5f * s, "Save");
    bool save = Hit("radfsave", ImVec2(x + w - ssz.x - 10 * s, y), ImVec2(x + w + 6 * s, y + 32 * s));
    TextRight(F_Semi, 15.5f * s, ImVec2(x + w, y + 6 * s), kBlue, "Save");
    y += 46 * s;

    Text(F_Semi, 12 * s, ImVec2(x + 4 * s, y), T.sub, "STREAM LINK");
    y += 20 * s;
    RadField("##radfurl", g_fUrl, sizeof g_fUrl, ImVec2(x, y), ImVec2(x + w, y + 40 * s), "http://...  (Ctrl+V to paste)", g_formFocus);
    y += 46 * s;
    // what Test play found
    if (testingThis && g_formTesting) {
        if (g_rad.state == 1 || g_rad.state == 2) { Spinner(ImVec2(x + 10 * s, y + 8 * s), 6 * s, T.sub); Text(F_Reg, 12.5f * s, ImVec2(x + 22 * s, y), T.sub, "Checking..."); }
        else if (g_rad.state == 3) {
            std::string ok = "Works";
            if (!g_rad.codec.empty()) ok += " \xC2\xB7 " + g_rad.codec;
            if (g_rad.kbps) ok += " " + std::to_string(g_rad.kbps) + "k";
            if (!g_rad.icyName.empty()) ok += " \xC2\xB7 \xE2\x80\x9C" + g_rad.icyName + "\xE2\x80\x9D";
            Text(F_Semi, 12.5f * s, ImVec2(x + 4 * s, y), kGreen, Fit(F_Semi, 12.5f * s, ok, w - 8 * s).c_str());
        } else if (g_rad.state == 4) DL()->AddText(F_Reg, 12.5f * s, ImVec2(x + 4 * s, y), kRedText(), g_rad.err.c_str(), nullptr, w - 8 * s);
    } else Text(F_Reg, 12 * s, ImVec2(x + 4 * s, y), T.sub, "The station's MP3 or AAC stream, or a .pls / .m3u link");
    y += 34 * s;
    // the name fills in from the stream once a test finds it
    if (g_formNameAuto && testingThis && !g_rad.icyName.empty()) {
        if (!g_fName[0]) CopyUtf8(g_fName, sizeof g_fName, g_rad.icyName);
        g_formNameAuto = false;
    }
    bool noFocus = false;
    Text(F_Semi, 12 * s, ImVec2(x + 4 * s, y), T.sub, "NAME");
    y += 20 * s;
    RadField("##radfname", g_fName, sizeof g_fName, ImVec2(x, y), ImVec2(x + w, y + 40 * s), "Filled in by Test play, or type one", noFocus);
    y += 54 * s;
    Text(F_Semi, 12 * s, ImVec2(x + 4 * s, y), T.sub, "GENRE (OPTIONAL)");
    y += 20 * s;
    RadField("##radfgenre", g_fGenre, sizeof g_fGenre, ImVec2(x, y), ImVec2(x + w, y + 40 * s), "Rock, News, Country...", noFocus);
    y += 56 * s;
    // test play / stop test
    bool playingThis = testingThis && RadioActive();
    if (Pill("radftest", playingThis ? "Stop" : "Test play", ImVec2(x, y), ImVec2(x + w, y + 40 * s), false)) {
        if (playingThis) Radio_Stop();
        else if (!IsWebLink(url)) g_formErr = "Enter a stream link starting with http:// or https://";
        else { g_formErr.clear(); g_formTesting = true; g_formNameAuto = !g_fName[0]; Radio_Play({ gitmo_shared::TrimAsciiWhitespace(g_fName), url, gitmo_shared::TrimAsciiWhitespace(g_fGenre) }); }
    }
    y += 52 * s;
    if (!g_formErr.empty()) { DL()->AddText(F_Reg, 12.5f * s, ImVec2(x + 4 * s, y), kRedText(), g_formErr.c_str(), nullptr, w - 8 * s); y += 36 * s; }
    // delete (editing only), asks first
    if (editing) {
        if (!g_formDelAsk) {
            if (DangerPill("radfdel", "Delete station", ImVec2(x, y), ImVec2(x + w, y + 40 * s), false)) g_formDelAsk = true;
        } else {
            DL()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 112 * s), T.card, 16 * s);
            std::string q = "Delete \xE2\x80\x9C" + g_radList[g_formIdx].name + "\xE2\x80\x9D?";
            Text(F_Semi, 15 * s, ImVec2(x + 16 * s, y + 12 * s), T.text, Fit(F_Semi, 15 * s, q, w - 32 * s).c_str());
            Text(F_Reg, 12 * s, ImVec2(x + 16 * s, y + 34 * s), T.sub, "This can't be undone.");
            float bw2 = (w - 32 * s - 8 * s) / 2, by = y + 62 * s;
            if (Pill("radfdelno", "Cancel", ImVec2(x + 16 * s, by), ImVec2(x + 16 * s + bw2, by + 36 * s), false)) g_formDelAsk = false;
            if (DangerPill("radfdelyes", "Delete", ImVec2(x + 24 * s + bw2, by), ImVec2(x + 24 * s + 2 * bw2, by + 36 * s), true)) {
                std::vector<RadioStation> list = g_radList;
                list.erase(list.begin() + g_formIdx);
                if (!SaveRadioStations(list, "Station deleted")) return;
                g_formDelAsk = false; g_radScr = RList;
                if (list.empty()) g_radEdit = false;
                return;
            }
        }
    }
    if (save) {
        if (!IsWebLink(url)) { g_formErr = "Enter a stream link starting with http:// or https://"; return; }
        std::string name = gitmo_shared::TrimAsciiWhitespace(g_fName);
        if (name.empty()) name = testingThis && !g_rad.icyName.empty() ? g_rad.icyName : HostOf(url);
        std::vector<RadioStation> list = g_radList;
        RadioStation st{ name, url, gitmo_shared::TrimAsciiWhitespace(g_fGenre) };
        if (editing) list[g_formIdx] = st; else list.push_back(st);
        if (!SaveRadioStations(list, editing ? "Station saved" : "Station added")) return;
        g_radScr = RList; g_radTab = 0;
        return;
    }
    if (cancel) {
        // a test of a link that isn't saved stops with the form
        if (g_formTesting && testingThis && SavedIndex(url) < 0) Radio_Stop();
        g_radScr = RList;
    }
}

// ---- share what's playing over SMS ---------------------------------------------------------------------------
// A station someone shared over SMS (its link tapped): play it and show Now playing, where
// the star saves it. A station already in My stations keeps its own name and genre.
static void RadioOpenShared(const std::string& name, const std::string& url) {
    std::string nm = name, genre;
    for (auto& st : Radio_Stations()) if (st.url == url) { nm = st.name; genre = st.genre; break; }
    if (nm.empty()) nm = HostOf(url);
    if (!(g_rad.url == url && RadioActive())) Radio_Play({ nm, url, genre });
    Log("radio: opened a shared station from SMS: \"%s\" %s", nm.c_str(), url.c_str());
    g_app = RadioApp; g_radScr = RNow; g_radShare = false; g_radEdit = false;
}

static void RadioShareSheet(ImVec2 a, ImVec2 b) {
    DL()->AddRectFilled(a, b, IM_COL32(0, 0, 0, 110), 34 * s, ImDrawFlags_RoundCornersBottom);
    const auto& cs = g_smsView.contacts;
    size_t n = std::min<size_t>(cs.size(), 5);
    float rh = 54 * s, h = 132 * s + (n ? n * rh : 50 * s);
    ImVec2 p0(a.x, b.y - h);
    if (Hit("radsharedim", a, ImVec2(b.x, p0.y))) { g_radShare = false; return; }
    DL()->AddRectFilled(p0, b, T.card, 30 * s);
    float cx = (a.x + b.x) * 0.5f, x = a.x + 20 * s, w = b.x - a.x - 40 * s, y = p0.y + 18 * s;
    TextCenter(F_Bold, 17 * s, ImVec2(cx, y), T.text, "Share to");
    std::string song, artist; SplitTitle(g_rad.title, song, artist);
    std::string what = g_rad.title.empty() ? StationLabel() : (artist.empty() ? song : song + " by " + artist) + " on " + StationLabel();
    TextCenter(F_Reg, 12.5f * s, ImVec2(cx, y + 24 * s), T.sub, Fit(F_Reg, 12.5f * s, what, w).c_str());
    y += 52 * s;
    if (!n) {
        TextCenter(F_Reg, 13 * s, ImVec2(cx, y + 6 * s), T.sub, "No one to share with yet: players with");
        TextCenter(F_Reg, 13 * s, ImVec2(cx, y + 24 * s), T.sub, "Gitmo show up here once you've met in a game.");
    }
    for (size_t i = 0; i < n; i++) {
        const sms::Contact& c = cs[i];
        ImVec2 r0(x, y + i * rh), r1(x + w, y + (i + 1) * rh);
        char id[24]; sprintf_s(id, "radshare%zu", i);
        bool click = Hit(id, r0, r1);
        if (ImGui::IsItemHovered()) DL()->AddRectFilled(r0, r1, Fade(T.text, 0.05f), 12 * s);
        std::string name = c.name.empty() ? "Player" : c.name;
        ContactAvatar(ImVec2(r0.x + 24 * s, (r0.y + r1.y) * 0.5f), 18 * s, c.id, name, 1.0f);
        Text(F_Semi, 15 * s, ImVec2(r0.x + 54 * s, r0.y + 9 * s), T.text, Fit(F_Semi, 15 * s, name, w - 70 * s).c_str());
        Text(F_Reg, 12 * s, ImVec2(r0.x + 54 * s, r0.y + 29 * s), c.online ? kGreen : T.sub, c.online ? "Online" : "Sends when they're online");
        if (click) {
            const std::string prefix = "\xF0\x9F\x93\xBB Listening to ";
            size_t fixed = prefix.size() + 1 + g_rad.url.size();
            if (fixed > sms::kMaxText) {
                Toast("Link is too long to share");
            } else {
                size_t titleRoom = sms::kMaxText - fixed;
                if (what.size() > titleRoom) {
                    const std::string ellipsis = "\xE2\x80\xA6";
                    what = titleRoom >= ellipsis.size()
                        ? what.substr(0, Utf8PrefixLength(what, titleRoom - ellipsis.size())) + ellipsis
                        : std::string();
                }
                std::string msg = prefix + what + "\n" + g_rad.url;
                if (Sms_Send(c.id, msg)) Toast("Sent to " + name); else Toast("Couldn't send");
            }
            g_radShare = false;
        }
    }
    if (Pill("radsharecancel", "Cancel", ImVec2(a.x + 24 * s, b.y - 64 * s), ImVec2(b.x - 24 * s, b.y - 24 * s), false)) g_radShare = false;
}

static void ScreenRadio(ImVec2 a, ImVec2 b) {
    bool before = g_behindSheet;
    g_behindSheet = before || g_radShare;   // the share sheet takes all clicks while it is up
    if (g_radScr == RNow) RadioNow(a, b);
    else if (g_radScr == RForm) RadioForm(a, b);
    else RadioList(a, b);
    g_behindSheet = before;
    if (g_radShare && !before) RadioShareSheet(a, b);
    if (GetTickCount64() < g_radToastUntil && !g_radToast.empty()) {   // a short note: "Station added"
        float cx = (a.x + b.x) * 0.5f, tw = Measure(F_Semi, 13 * s, g_radToast.c_str()).x + 32 * s, ty = b.y - 110 * s;
        DL()->AddRectFilled(ImVec2(cx - tw / 2, ty), ImVec2(cx + tw / 2, ty + 34 * s), Fade(T.text, 0.88f), 17 * s);
        TextCenter(F_Semi, 13 * s, ImVec2(cx, ty + 8 * s), T.bg, g_radToast.c_str());
    }
}
