// Gitmo - transfers over Steam.
//
// Uses the game's own steam_api64.dll (flat API) once the game has started Steam:
//   - who: Steam friends currently playing SnowRunner, plus everyone in the same
//     Steam lobby as us. Each gets a HELLO every few seconds; the ones that answer
//     have Gitmo and show up on the phone.
//   - how: ISteamNetworkingMessages on Gitmo's own channel. The game itself uses
//     the older ISteamNetworking, so nothing here touches its traffic.
//   - same session: SnowRunner runs co-op through Saber's Hydra servers, not Steam.
//     Gitmo asks the game's own Hydra SDK (pros.sdk.x64.dll) for the current game
//     session: a player is "in your game" when the session's member list names them,
//     or when their Gitmo reports the same session id (SESS message).
// The transfer rules themselves live in transfer.cpp.
#include "gitmo.h"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <random>
#include <algorithm>
#include <map>
#include <set>
#include <regex>

std::atomic<int> g_txWaiting{0};
std::atomic<int> g_smsUnread{0};

namespace {

const int kChannel = 47710;               // Gitmo's message channel (any number the game doesn't use)
const int kSendReliable = 8 | 32;         // k_nSteamNetworkingSend_Reliable | AutoRestartBrokenSession
const int kCbSessionRequest = 1251;       // SteamNetworkingMessagesSessionRequest_t
const uint32_t kSnowRunnerApp = 1465360;

struct Identity { uint8_t raw[136]; };    // SteamNetworkingIdentity

// flat API
using Acc_t        = void* (__cdecl*)();
using GetSteamID_t = uint64_t (__cdecl*)(void*);
using FriendCnt_t  = int (__cdecl*)(void*, int);
using FriendIdx_t  = uint64_t (__cdecl*)(void*, int, int);
using FriendGame_t = bool (__cdecl*)(void*, uint64_t, void*);
using FriendName_t = const char* (__cdecl*)(void*, uint64_t);
using MyName_t     = const char* (__cdecl*)(void*);
using LobbyCnt_t   = int (__cdecl*)(void*, uint64_t);
using LobbyMem_t   = uint64_t (__cdecl*)(void*, uint64_t, int);
using AppID_t      = uint32_t (__cdecl*)(void*);
using IdClear_t    = void (__cdecl*)(Identity*);
using IdSet_t      = void (__cdecl*)(Identity*, uint64_t);
using IdGet_t      = uint64_t (__cdecl*)(const Identity*);
using SendTo_t     = int (__cdecl*)(void*, const Identity*, const void*, uint32_t, int, int);
using Receive_t    = int (__cdecl*)(void*, int, void**, int);
using AcceptSes_t  = bool (__cdecl*)(void*, const Identity*);
using Release_t    = void (__cdecl*)(void*);
using RegCb_t      = void (__cdecl*)(void*, int);
using P2PState_t   = bool (__cdecl*)(void*, uint64_t, void*);
using RpCount_t    = int (__cdecl*)(void*, uint64_t);
using RpKey_t      = const char* (__cdecl*)(void*, uint64_t, int);
using Rp_t         = const char* (__cdecl*)(void*, uint64_t, const char*);
using CoplayCnt_t  = int (__cdecl*)(void*);
using CoplayIdx_t  = uint64_t (__cdecl*)(void*, int);
using LobbyOwner_t = uint64_t (__cdecl*)(void*, uint64_t);
using Avatar_t     = int (__cdecl*)(void*, uint64_t);
using ImgSize_t    = bool (__cdecl*)(void*, int, uint32_t*, uint32_t*);
using ImgRGBA_t    = bool (__cdecl*)(void*, int, uint8_t*, int);
using ReqUser_t    = bool (__cdecl*)(void*, uint64_t, bool);
using HasFriend_t  = bool (__cdecl*)(void*, uint64_t, int);
using LobbyDataN_t = int (__cdecl*)(void*, uint64_t);
using LobbyDataI_t = bool (__cdecl*)(void*, uint64_t, int, char*, int, char*, int);
using LobbyReq_t   = bool (__cdecl*)(void*, uint64_t);
using CoplayTime_t = int (__cdecl*)(void*, uint64_t);
using CoplayGame_t = uint32_t (__cdecl*)(void*, uint64_t);
using WriteShot_t  = uint32_t (__cdecl*)(void*, void*, uint32_t, int, int);   // ISteamScreenshots::WriteScreenshot (24-bit RGB)

struct Api {
    void *user = nullptr, *friends = nullptr, *mm = nullptr, *nm = nullptr, *utils = nullptr, *net = nullptr;
    GetSteamID_t getSteamID; FriendCnt_t friendCount; FriendIdx_t friendByIndex; FriendGame_t friendGame;
    FriendName_t friendName; MyName_t myName; LobbyCnt_t lobbyCount; LobbyMem_t lobbyMember; AppID_t appID;
    IdClear_t idClear; IdSet_t idSet; IdGet_t idGet; SendTo_t sendTo; Receive_t receive; AcceptSes_t acceptSession;
    Release_t release; RegCb_t registerCallback;
    // optional (session check and its diagnostics)
    P2PState_t p2pState; RpCount_t rpCount; RpKey_t rpKey; Rp_t rp; CoplayCnt_t coplayCount; CoplayIdx_t coplay; LobbyOwner_t lobbyOwner;
    HasFriend_t hasFriend; LobbyDataN_t lobbyDataCount; LobbyDataI_t lobbyDataByIndex; LobbyReq_t lobbyRequest;
    CoplayTime_t coplayTime; CoplayGame_t coplayGame;
    Avatar_t mediumAvatar; ImgSize_t imgSize; ImgRGBA_t imgRGBA; ReqUser_t requestUser;
    void* screenshots = nullptr; WriteShot_t writeShot;
} S;

std::mutex g_mx;                 // guards g_eng and g_sms
tx::Engine* g_eng = nullptr;
sms::Engine* g_sms = nullptr;
std::atomic<unsigned> g_smsVersion{1};
std::atomic<bool> g_ready{false};
std::string g_status = "Starting";
uint64_t g_me = 0;
uint32_t g_app = kSnowRunnerApp;

uint64_t NowMs() { return GetTickCount64(); }

template <class F> F Get(HMODULE m, const char* n) { return (F)GetProcAddress(m, n); }

bool Resolve(HMODULE sa) {
    S.getSteamID     = Get<GetSteamID_t>(sa, "SteamAPI_ISteamUser_GetSteamID");
    S.friendCount    = Get<FriendCnt_t>(sa, "SteamAPI_ISteamFriends_GetFriendCount");
    S.friendByIndex  = Get<FriendIdx_t>(sa, "SteamAPI_ISteamFriends_GetFriendByIndex");
    S.friendGame     = Get<FriendGame_t>(sa, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    S.friendName     = Get<FriendName_t>(sa, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    S.myName         = Get<MyName_t>(sa, "SteamAPI_ISteamFriends_GetPersonaName");
    S.lobbyCount     = Get<LobbyCnt_t>(sa, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    S.lobbyMember    = Get<LobbyMem_t>(sa, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    S.appID          = Get<AppID_t>(sa, "SteamAPI_ISteamUtils_GetAppID");
    S.idClear        = Get<IdClear_t>(sa, "SteamAPI_SteamNetworkingIdentity_Clear");
    S.idSet          = Get<IdSet_t>(sa, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    S.idGet          = Get<IdGet_t>(sa, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    S.sendTo         = Get<SendTo_t>(sa, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    S.receive        = Get<Receive_t>(sa, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    S.acceptSession  = Get<AcceptSes_t>(sa, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    S.release        = Get<Release_t>(sa, "SteamAPI_SteamNetworkingMessage_t_Release");
    S.registerCallback = Get<RegCb_t>(sa, "SteamAPI_RegisterCallback");
    S.p2pState       = Get<P2PState_t>(sa, "SteamAPI_ISteamNetworking_GetP2PSessionState");
    S.rpCount        = Get<RpCount_t>(sa, "SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount");
    S.rpKey          = Get<RpKey_t>(sa, "SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex");
    S.rp             = Get<Rp_t>(sa, "SteamAPI_ISteamFriends_GetFriendRichPresence");
    S.coplayCount    = Get<CoplayCnt_t>(sa, "SteamAPI_ISteamFriends_GetCoplayFriendCount");
    S.coplay         = Get<CoplayIdx_t>(sa, "SteamAPI_ISteamFriends_GetCoplayFriend");
    S.lobbyOwner     = Get<LobbyOwner_t>(sa, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    S.hasFriend      = Get<HasFriend_t>(sa, "SteamAPI_ISteamFriends_HasFriend");
    S.lobbyDataCount = Get<LobbyDataN_t>(sa, "SteamAPI_ISteamMatchmaking_GetLobbyDataCount");
    S.lobbyDataByIndex = Get<LobbyDataI_t>(sa, "SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex");
    S.lobbyRequest   = Get<LobbyReq_t>(sa, "SteamAPI_ISteamMatchmaking_RequestLobbyData");
    S.coplayTime     = Get<CoplayTime_t>(sa, "SteamAPI_ISteamFriends_GetFriendCoplayTime");
    S.coplayGame     = Get<CoplayGame_t>(sa, "SteamAPI_ISteamFriends_GetFriendCoplayGame");
    S.mediumAvatar   = Get<Avatar_t>(sa, "SteamAPI_ISteamFriends_GetMediumFriendAvatar");
    S.imgSize        = Get<ImgSize_t>(sa, "SteamAPI_ISteamUtils_GetImageSize");
    S.imgRGBA        = Get<ImgRGBA_t>(sa, "SteamAPI_ISteamUtils_GetImageRGBA");
    S.requestUser    = Get<ReqUser_t>(sa, "SteamAPI_ISteamFriends_RequestUserInformation");
    S.writeShot      = Get<WriteShot_t>(sa, "SteamAPI_ISteamScreenshots_WriteScreenshot");
    return S.getSteamID && S.friendCount && S.friendByIndex && S.friendGame && S.friendName && S.myName &&
           S.idClear && S.idSet && S.idGet && S.sendTo && S.receive && S.acceptSession && S.release && S.registerCallback;
}

// The interface accessors return null until the game has started Steam.
bool GuardedInterfaces(HMODULE sa) {
    __try {
        auto user  = Get<Acc_t>(sa, "SteamAPI_SteamUser_v021");
        auto fr    = Get<Acc_t>(sa, "SteamAPI_SteamFriends_v017");
        auto mm    = Get<Acc_t>(sa, "SteamAPI_SteamMatchmaking_v009");
        auto nm    = Get<Acc_t>(sa, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
        auto ut    = Get<Acc_t>(sa, "SteamAPI_SteamUtils_v010");
        auto net   = Get<Acc_t>(sa, "SteamAPI_SteamNetworking_v006");
        auto shots = Get<Acc_t>(sa, "SteamAPI_SteamScreenshots_v003");
        if (!user || !fr || !nm) return false;
        S.user = user(); S.friends = fr(); S.nm = nm();
        S.mm = mm ? mm() : nullptr; S.utils = ut ? ut() : nullptr; S.net = net ? net() : nullptr;
        S.screenshots = shots ? shots() : nullptr;
        if (!S.user || !S.friends || !S.nm) return false;
        g_me = S.getSteamID(S.user);
        if (S.utils && S.appID) { uint32_t a = S.appID(S.utils); if (a) g_app = a; }
        return g_me != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void MakeId(Identity& id, uint64_t sid) { S.idClear(&id); S.idSet(&id, sid); }

int GuardedSend(uint64_t to, const std::string& m) {
    __try {
        Identity id; MakeId(id, to);
        return S.sendTo(S.nm, &id, m.data(), (uint32_t)m.size(), kSendReliable, kChannel);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

struct RawMsg { uint64_t from; std::string data; };
// SteamNetworkingMessage_t: m_pData +0, m_cbSize +8, m_identityPeer +16
int GuardedReceive(RawMsg* out, int max) {
    void* msgs[32];
    int n = 0;
    __try {
        n = S.receive(S.nm, kChannel, msgs, max < 32 ? max : 32);
        for (int i = 0; i < n; i++) {
            uint8_t* m = (uint8_t*)msgs[i];
            const char* d = *(const char**)m;
            int sz = *(int*)(m + 8);
            out[i].from = S.idGet((const Identity*)(m + 16));
            // text messages are small; a photo piece is up to 48 KB (sms::kChunk) plus its header
            if (d && sz > 0 && sz <= 64 * 1024) out[i].data.assign(d, sz); else out[i].data.clear();
            S.release(m);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return n;
}

struct FriendInfo { uint64_t id; std::string name; uint64_t lobby; };
struct FriendRaw { uint64_t id; const char* name; uint64_t lobby; };
// Friends playing the same game, with their lobby (FriendGameInfo_t: gameID +0, lobby +16).
int GuardedFriendsRaw(FriendRaw* out, int max) {
    int k = 0;
    __try {
        int n = S.friendCount(S.friends, 4 /*k_EFriendFlagImmediate*/);
        for (int i = 0; i < n && i < 2000 && k < max; i++) {
            uint64_t f = S.friendByIndex(S.friends, i, 4);
            alignas(8) uint8_t info[32] = {};
            if (!f || !S.friendGame(S.friends, f, info)) continue;
            if ((uint32_t)(*(uint64_t*)info & 0xFFFFFF) != g_app) continue;
            out[k].id = f; out[k].name = S.friendName(S.friends, f); out[k].lobby = *(uint64_t*)(info + 16);
            k++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return k; }
    return k;
}
void GuardedFriends(std::vector<FriendInfo>& out) {
    static FriendRaw raw[64];
    int n = GuardedFriendsRaw(raw, 64);
    for (int i = 0; i < n; i++) out.push_back({ raw[i].id, raw[i].name ? raw[i].name : "", raw[i].lobby });
}

// Lobby members are only visible to members, so a lobby we can count is our own.
bool GuardedLobby(uint64_t lobby, std::vector<uint64_t>& members) {
    if (!S.mm || !S.lobbyCount || !S.lobbyMember || !lobby) return false;
    __try {
        int n = S.lobbyCount(S.mm, lobby);
        for (int i = 0; i < n && i < 16; i++) { uint64_t m = S.lobbyMember(S.mm, lobby, i); if (m) members.push_back(m); }
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const char* GuardedNameRaw(uint64_t id) {
    __try { return S.friendName(S.friends, id); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
std::string GuardedName(uint64_t id) { const char* n = GuardedNameRaw(id); return n ? n : ""; }

// ---- is the game connected to this player? --------------------------------------------------
// P2PSessionState_t: m_bConnectionActive +0, m_bConnecting +1, m_eP2PSessionError +2, m_bUsingRelay +3
struct P2P { bool known; uint8_t active, connecting, error, relay; };
P2P GuardedP2P(uint64_t id) {
    P2P r = {};
    if (!S.net || !S.p2pState) return r;
    __try {
        alignas(8) uint8_t b[32] = {};
        r.known = S.p2pState(S.net, id, b);
        r.active = b[0]; r.connecting = b[1]; r.error = b[2]; r.relay = b[3];
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = {}; }
    return r;
}

// Rich presence as "key=value; key=value" (diagnostics: does the game publish a session id?)
int GuardedRichRaw(uint64_t id, char* out, int cap) {
    out[0] = 0;
    if (!S.rpCount || !S.rpKey || !S.rp) return -1;
    __try {
        int n = S.rpCount(S.friends, id), pos = 0;
        for (int i = 0; i < n && i < 16; i++) {
            const char* k = S.rpKey(S.friends, id, i);
            const char* v = k ? S.rp(S.friends, id, k) : nullptr;
            int w = _snprintf_s(out + pos, cap - pos, _TRUNCATE, "%s%s=%.60s", pos ? "; " : "", k ? k : "?", v ? v : "");
            if (w < 0) break;
            pos += w;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
std::string RichPresence(uint64_t id) {
    char b[512];
    int n = GuardedRichRaw(id, b, sizeof b);
    return n < 0 ? "n/a" : n == 0 ? "none" : b;
}

int GuardedCoplayCount() {
    if (!S.coplayCount) return -1;
    __try { return S.coplayCount(S.friends); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
bool GuardedHasFriend(uint64_t id) {
    if (!S.hasFriend || !id) return false;
    __try { return S.hasFriend(S.friends, id, 4 /*k_EFriendFlagImmediate*/); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Lobby metadata as "key=value; ..." (research: does the game put its Hydra session there?)
int GuardedLobbyDataRaw(uint64_t lobby, char* out, int cap) {
    out[0] = 0;
    if (!S.mm || !S.lobbyDataCount || !S.lobbyDataByIndex || !lobby) return -1;
    __try {
        int n = S.lobbyDataCount(S.mm, lobby), pos = 0;
        for (int i = 0; i < n && i < 24; i++) {
            char k[256] = {}, v[512] = {};
            if (!S.lobbyDataByIndex(S.mm, lobby, i, k, sizeof k, v, sizeof v)) continue;
            int w = _snprintf_s(out + pos, cap - pos, _TRUNCATE, "%s%s=%.120s", pos ? "; " : "", k, v);
            if (w < 0) break;
            pos += w;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
std::string LobbyData(uint64_t lobby) {
    static char b[4096];
    int n = GuardedLobbyDataRaw(lobby, b, sizeof b);
    return n < 0 ? "n/a" : n == 0 ? "none" : b;
}
void GuardedLobbyRequest(uint64_t lobby) {
    if (!S.mm || !S.lobbyRequest || !lobby) return;
    __try { S.lobbyRequest(S.mm, lobby); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
struct Coplay { int time; uint32_t app; };
Coplay GuardedCoplay(uint64_t id) {
    Coplay c = {};
    if (!S.coplayTime || !S.coplayGame) return c;
    __try { c.time = S.coplayTime(S.friends, id); c.app = S.coplayGame(S.friends, id); } __except (EXCEPTION_EXECUTE_HANDLER) { c = {}; }
    return c;
}
const char* GuardedRpRaw(uint64_t id, const char* key) {
    if (!S.rp) return nullptr;
    __try { return S.rp(S.friends, id, key); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

uint64_t GuardedLobbyOwner(uint64_t lobby) {
    if (!S.mm || !S.lobbyOwner || !lobby) return 0;
    __try { return S.lobbyOwner(S.mm, lobby); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ---- the game's own session (Saber's Hydra SDK, pros.sdk.x64.dll) -----------------------
// Co-op runs through Hydra, not Steam. The SDK hands out a copy of the matchmaking session
// status: status* hydra5_matchmake_session_get_status(user*), freed with
// hydra5_release_matchmake_session_status(status*). Layout (from the release function):
//   +0x00.. session info (+0 char*, +8 char*, +0x20 char** / +0x28 count, ...)
//   +0x60 char*, +0x70 char*, +0x78 array of {char*, char**, count} / +0x80 count, +0x98 char*
// Research build: log the whole thing whenever it changes.
using ProsCount_t   = size_t (__cdecl*)();
using ProsUser_t    = void* (__cdecl*)(int);
using HyStatus_t    = void* (__cdecl*)(void*);
using HyRelease_t   = void (__cdecl*)(void*);
struct Hydra { bool tried = false; ProsCount_t count = nullptr; ProsUser_t user = nullptr; HyStatus_t status = nullptr; HyRelease_t release = nullptr; } H;

bool SafeQ(uintptr_t a, uint64_t& v) {
    if (a < 0x10000) return false;
    __try { v = *(const uint64_t*)a; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// a printable C string at a (>= 1 char), else -1
int SafeStr(uintptr_t a, char* out, int cap) {
    if (a < 0x10000) return -1;
    __try {
        const char* p = (const char*)a;
        int n = 0;
        for (; n < cap - 1; n++) {
            char c = p[n];
            if (!c) break;
            if (((unsigned char)c < 32 && c != '\n' && c != '\r' && c != '\t') || (unsigned char)c > 126) return -1;
            out[n] = c;
        }
        out[n] = 0;
        return n >= 1 ? n : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
void* GuardedHyStatus(void* user) {
    __try { return H.status(user); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void GuardedHyRelease(void* st) {
    __try { H.release(st); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
size_t GuardedProsCount() {
    __try { return H.count(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void* GuardedProsUser(int i) {
    __try { return H.user(i); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

std::string StrAt(uintptr_t a) {
    char b[160];
    return SafeStr(a, b, sizeof b) >= 1 ? std::string(b) : std::string();
}
std::string LongStrAt(uintptr_t a) {
    static char b[4096];
    return SafeStr(a, b, sizeof b) >= 1 ? std::string(b) : std::string();
}
// A (char**, count) list as "a, b, c", or "" if it doesn't look like one
std::string StrList(uint64_t p, uint64_t n) {
    if (p < 0x10000 || n < 1 || n > 32) return "";
    std::string out;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t e; if (!SafeQ(p + i * 8, e)) return "";
        std::string s = StrAt(e);
        if (s.empty()) return "";
        out += (i ? ", " : "") + s;
    }
    return out;
}

std::string DumpHydraStatus(uintptr_t st) {
    std::string s;
    char b[64];
    uint64_t q[0xB0 / 8] = {};
    for (int i = 0; i < 0xB0 / 8; i++) SafeQ(st + i * 8, q[i]);
    for (int i = 0; i < 0xB0 / 8; i++) {
        std::string str = q[i] > 0x10000 ? StrAt(q[i]) : "";
        if (!str.empty()) { sprintf_s(b, " +%02X=", i * 8); s += b; s += "\"" + str + "\""; }
        else if (q[i]) { sprintf_s(b, " +%02X=%llx", i * 8, (unsigned long long)q[i]); s += b; }
    }
    // lists: (char**, count) and arrays of {char*, char**, count}
    for (int i = 0; i + 1 < 0xB0 / 8; i++) {
        uint64_t p = q[i], n = q[i + 1];
        if (p < 0x10000 || n < 1 || n > 32) continue;
        std::string l = StrList(p, n);
        if (!l.empty()) { sprintf_s(b, " | +%02X list[%llu]: ", i * 8, (unsigned long long)n); s += b; s += l; continue; }
        std::string arr;
        for (uint64_t k = 0; k < n; k++) {
            uint64_t e0 = 0, e1 = 0, e2 = 0;
            if (!SafeQ(p + k * 0x18, e0) || !SafeQ(p + k * 0x18 + 8, e1) || !SafeQ(p + k * 0x18 + 16, e2)) { arr.clear(); break; }
            std::string name = StrAt(e0), members = StrList(e1, e2);
            arr += (k ? "; " : "") + (name.empty() ? std::string("?") : name) + " [" + members + "]";
        }
        if (!arr.empty()) { sprintf_s(b, " | +%02X groups[%llu]: ", i * 8, (unsigned long long)n); s += b; s += arr; }
    }
    return s;
}

void HydraResearch() {
    if (!H.tried) {
        HMODULE m = GetModuleHandleW(L"pros.sdk.x64.dll");
        if (!m) return;   // not loaded yet
        H.tried = true;
        H.count = (ProsCount_t)GetProcAddress(m, "prosdk_authorization_user_get_count");
        H.user = (ProsUser_t)GetProcAddress(m, "prosdk_authorization_user_get_by_index");
        H.status = (HyStatus_t)GetProcAddress(m, "hydra5_matchmake_session_get_status");
        H.release = (HyRelease_t)GetProcAddress(m, "hydra5_release_matchmake_session_status");
        Log("hydra: SDK found, session status %s", H.count && H.user && H.status && H.release ? "readable" : "NOT readable (functions missing)");
    }
    if (!H.count || !H.user || !H.status || !H.release) return;
    static std::string last;
    std::string all;
    size_t n = GuardedProsCount();
    for (size_t i = 0; i < n && i < 4; i++) {
        void* u = GuardedProsUser((int)i);
        if (!u) continue;
        void* st = GuardedHyStatus(u);
        all += "user " + std::to_string(i) + ": ";
        if (!st) { all += "no session status. "; continue; }
        all += DumpHydraStatus((uintptr_t)st) + ". ";
        GuardedHyRelease(st);
    }
    if (all.empty()) all = "no signed-in Hydra users";
    if (all != last) { Log("hydra: session status: %s", all.c_str()); last = all; }
}

// "key":"value" in a JSON text that may itself be escaped inside another string
std::string JsonField(const std::string& json, const char* key) {
    std::string t;
    for (char c : json) if (c != '\\') t += c;
    std::string k = std::string("\"") + key + "\":";
    size_t p = t.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    while (p < t.size() && t[p] == ' ') p++;
    if (p < t.size() && t[p] == '"') { size_t e = t.find('"', p + 1); return e == std::string::npos ? "" : t.substr(p + 1, e - p - 1); }
    size_t e = t.find_first_of(",}", p);
    return t.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

// The game session as the game's Hydra SDK sees it. Status layout (pros.sdk.x64.dll, 2026-11-09):
//   +0x00 char* session id ("" when not in a session, e.g. main menu)
//   +0x40 member array, +0x48 count; a member is 0x98 bytes: +0 char* Hydra user id, other
//   char* fields hold JSON (member data; static data with the Steam id and nickname)
struct HyMember { std::string hydra; uint64_t steam = 0; std::string nick; };
struct HySession { bool ok = false; std::string id; std::vector<HyMember> members; };

HySession ReadHydra() {
    HySession r;
    if (!H.tried) {
        HMODULE m = GetModuleHandleW(L"pros.sdk.x64.dll");
        if (!m) return r;   // not loaded yet
        H.tried = true;
        H.count = (ProsCount_t)GetProcAddress(m, "prosdk_authorization_user_get_count");
        H.user = (ProsUser_t)GetProcAddress(m, "prosdk_authorization_user_get_by_index");
        H.status = (HyStatus_t)GetProcAddress(m, "hydra5_matchmake_session_get_status");
        H.release = (HyRelease_t)GetProcAddress(m, "hydra5_release_matchmake_session_status");
        Log("session: the game's online SDK %s", H.count && H.user && H.status && H.release ? "found" : "found, but without the session functions Gitmo needs");
    }
    if (!H.count || !H.user || !H.status || !H.release) return r;
    r.ok = true;
    void* u = nullptr;
    size_t n = GuardedProsCount();
    for (size_t i = 0; i < n && i < 4 && !u; i++) u = GuardedProsUser((int)i);
    if (!u) return r;   // not signed in to the game's servers: no session
    void* st = GuardedHyStatus(u);
    if (!st) return r;
    uintptr_t b = (uintptr_t)st;
    uint64_t q = 0, arr = 0, cnt = 0;
    if (SafeQ(b, q)) r.id = StrAt(q);
    SafeQ(b + 0x40, arr); SafeQ(b + 0x48, cnt);
    if (arr > 0x10000 && cnt <= 16) {
        for (uint64_t k = 0; k < cnt; k++) {
            uintptr_t e = arr + k * 0x98;
            HyMember m;
            if (SafeQ(e, q)) m.hydra = StrAt(q);
            for (int off = 8; off < 0x98; off += 8) {
                if (!SafeQ(e + off, q) || q < 0x10000) continue;
                std::string j = LongStrAt(q);
                if (j.find("userId") == std::string::npos) continue;
                std::string id = JsonField(j, "userId");
                if (id.size() == 17 && id.compare(0, 4, "7656") == 0) {   // a Steam id
                    m.steam = _strtoui64(id.c_str(), nullptr, 10);
                    m.nick = JsonField(j, "nickname");
                }
            }
            if (!m.hydra.empty()) r.members.push_back(m);
        }
    }
    if (g_cfg.sessionResearch >= 1) {   // research: the whole status, when its text changes
        static std::string last;
        std::string d = DumpHydraStatus(b);
        for (uint64_t k = 0; arr > 0x10000 && k < cnt && k < 16; k++)
            for (int off = 0; off < 0x98; off += 8) {
                if (!SafeQ(arr + k * 0x98 + off, q) || q < 0x10000) continue;
                std::string t = LongStrAt(q);
                if (!t.empty()) d += " | member " + std::to_string(k) + " +" + std::to_string(off) + ": " + t.substr(0, 300);
            }
        std::string key = std::regex_replace(d, std::regex("=[0-9a-f]{9,}"), "=ptr");
        if (key != last) { Log("hydra: session status: %s", d.c_str()); last = key; }
    }
    GuardedHyRelease(st);
    return r;
}

// ---- incoming session requests -----------------------------------------------------------
// Steam asks before delivering messages from someone we haven't messaged yet. Accept only
// players we know: friends in SnowRunner and our lobby. Laid out like the SDK's CCallbackBase.
class CCallbackBase {
public:
    virtual void Run(void* p) = 0;
    virtual void Run(void* p, bool ioFailure, uint64_t call) = 0;
    virtual int GetCallbackSizeBytes() = 0;
    uint8_t m_nCallbackFlags = 0;
    int m_iCallback = 0;
};

bool GuardedAccept(const Identity* id) {
    __try { return S.acceptSession(S.nm, id); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint64_t GuardedIdGet(const void* p) {
    __try { return S.idGet((const Identity*)p); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

class SessionRequest : public CCallbackBase {
public:
    void Run(void* p) override { Handle(p); }
    void Run(void* p, bool, uint64_t) override { Handle(p); }
    int GetCallbackSizeBytes() override { return (int)sizeof(Identity); }
private:
    static void Handle(void* p) {
        if (!p) return;
        uint64_t from = GuardedIdGet(p);
        bool known;
        { std::lock_guard<std::mutex> lk(g_mx); known = g_eng && g_eng->Knows(from); }
        if (!known) known = GuardedHasFriend(from);   // any Steam friend
        bool ok = known && GuardedAccept((const Identity*)p);
        Log("transfers: connection request from %llu -> %s", (unsigned long long)from, ok ? "accepted" : known ? "accept FAILED" : "ignored (not a Steam friend or in our lobby)");
    }
} g_sessionCb;

// The game's own session events, watched (never answered) to learn who is in our game:
// a lobby we enter or create, and players whose game opens a P2P connection to ours.
std::mutex g_cbMx;
uint64_t g_cbLobby = 0;                  // lobby the game entered / created (Steam callbacks)
std::vector<uint64_t> g_cbPlayers;       // players whose game asked ours for a P2P connection
std::vector<std::pair<uint64_t, std::string>> g_probePlayers;   // research: whose ids to look for

struct CbData { uint64_t a, b, c; uint32_t d; char text[96]; bool ok; };
CbData GuardedCbRead(int id, const uint8_t* p) {
    CbData r = {};
    __try {
        switch (id) {
        case 504:  r.a = *(const uint64_t*)p; r.d = *(const uint32_t*)(p + 16); break;           // LobbyEnter_t: lobby, response
        case 513:  r.d = *(const uint32_t*)p; r.a = *(const uint64_t*)(p + 8); break;            // LobbyCreated_t: result, lobby
        case 506:  r.a = *(const uint64_t*)p; r.b = *(const uint64_t*)(p + 8); r.c = *(const uint64_t*)(p + 16);
                   r.d = *(const uint32_t*)(p + 24); break;                                       // LobbyChatUpdate_t
        case 1202: r.a = *(const uint64_t*)p; break;                                              // P2PSessionRequest_t
        case 1203: r.a = *(const uint64_t*)p; r.d = p[8]; break;                                  // P2PSessionConnectFail_t
        case 333:  r.a = *(const uint64_t*)p; r.b = *(const uint64_t*)(p + 8); break;             // GameLobbyJoinRequested_t
        case 337:  r.a = *(const uint64_t*)p; strncpy_s(r.text, (const char*)(p + 8), _TRUNCATE); break;   // GameRichPresenceJoinRequested_t
        }
        r.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { r.ok = false; }
    return r;
}

class Watch : public CCallbackBase {
public:
    Watch(int id, int size) : size_(size) { m_iCallback = id; }
    void Run(void* p) override { Handle(m_iCallback, p); }
    void Run(void* p, bool, uint64_t) override { Handle(m_iCallback, p); }
    int GetCallbackSizeBytes() override { return size_; }
private:
    int size_;
    static void Handle(int id, void* p) {
        if (!p) return;
        CbData d = GuardedCbRead(id, (const uint8_t*)p);
        if (!d.ok) return;
        auto u = [](uint64_t v) { return (unsigned long long)v; };
        switch (id) {
        case 504:
            Log("session check: the game entered Steam lobby %llu (response %u)", u(d.a), d.d);
            if (d.d == 1) { std::lock_guard<std::mutex> lk(g_cbMx); g_cbLobby = d.a; }
            break;
        case 513:
            Log("session check: the game created Steam lobby %llu (result %u)", u(d.a), d.d);
            if (d.d == 1) { std::lock_guard<std::mutex> lk(g_cbMx); g_cbLobby = d.a; }
            break;
        case 506:
            Log("session check: lobby %llu: player %llu changed (state %u)", u(d.a), u(d.b), d.d);
            break;
        case 1202: {
            Log("session check: the game was asked for a P2P connection by %llu (%s)", u(d.a), GuardedName(d.a).c_str());
            std::lock_guard<std::mutex> lk(g_cbMx);
            if (std::find(g_cbPlayers.begin(), g_cbPlayers.end(), d.a) == g_cbPlayers.end() && g_cbPlayers.size() < 64) g_cbPlayers.push_back(d.a);
            break;
        }
        case 1203:
            Log("session check: the game's P2P connection to %llu failed (error %u)", u(d.a), d.d);
            break;
        case 333:
            Log("session check: Steam invite accepted: lobby %llu from %llu", u(d.a), u(d.b));
            break;
        case 337:
            Log("session check: Steam join request from %llu, connect \"%s\"", u(d.a), d.text);
            break;
        }
    }
};
Watch g_watch[] = { Watch(504, 24), Watch(513, 16), Watch(506, 32), Watch(1202, 8), Watch(1203, 16), Watch(333, 16), Watch(337, 264) };

bool GuardedRegister() {
    __try {
        g_sessionCb.m_iCallback = kCbSessionRequest; S.registerCallback(&g_sessionCb, kCbSessionRequest);
        for (auto& w : g_watch) S.registerCallback(&w, w.m_iCallback);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- Steam avatars ---------------------------------------------------------------------------
// The phone asks (Avatar_Get); the Steam thread fetches the 64x64 picture when Steam has it.
std::mutex g_avMx;
std::map<uint64_t, AvatarImg> g_avatars;     // fetched
std::map<uint64_t, ULONGLONG> g_avWanted;    // asked for -> next try (ms)

int GuardedAvatarHandle(uint64_t id) {
    if (!S.mediumAvatar || !S.friends) return 0;
    __try { return S.mediumAvatar(S.friends, id); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool GuardedImage(int h, uint32_t* w, uint32_t* ht, uint8_t* buf, int cap) {
    if (!S.imgSize || !S.imgRGBA || !S.utils) return false;
    __try {
        if (!S.imgSize(S.utils, h, w, ht) || !*w || !*ht || (int)(*w * *ht * 4) > cap) return false;
        return buf ? S.imgRGBA(S.utils, h, buf, (int)(*w * *ht * 4)) : true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void GuardedRequestUser(uint64_t id) {
    if (!S.requestUser || !S.friends) return;
    __try { S.requestUser(S.friends, id, false); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void FetchAvatars() {
    std::vector<uint64_t> todo;
    ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(g_avMx);
        for (auto& kv : g_avWanted) if (!g_avatars.count(kv.first) && now >= kv.second) todo.push_back(kv.first);
    }
    for (uint64_t id : todo) {
        int h = GuardedAvatarHandle(id);   // 0 = none (yet), -1 = Steam is still loading it
        uint32_t w = 0, ht = 0;
        AvatarImg img;
        if (h > 0 && GuardedImage(h, &w, &ht, nullptr, 256 * 256 * 4)) {
            img.w = (int)w; img.h = (int)ht; img.rgba.resize((size_t)w * ht * 4);
            if (!GuardedImage(h, &w, &ht, img.rgba.data(), (int)img.rgba.size())) img.rgba.clear();
        }
        std::lock_guard<std::mutex> lk(g_avMx);
        if (!img.rgba.empty()) g_avatars[id] = std::move(img);
        else {
            if (h == 0) GuardedRequestUser(id);   // not a friend / not loaded: ask Steam for it
            g_avWanted[id] = now + (h == -1 ? 1000 : 15000);
        }
    }
}

// ---- saved state ---------------------------------------------------------------------------
std::wstring StatePath() { return ModuleDir() + L"\\Gitmo-transfers.txt"; }
std::wstring SmsPath() { return ModuleDir() + L"\\Gitmo-sms.txt"; }
// photos sent and received over SMS: Gitmo-photos\<message id>.jpg next to the game
std::wstring PhotoDir() { return ModuleDir() + L"\\Gitmo-photos"; }
std::wstring PhotoPath(uint64_t id) { wchar_t b[40]; swprintf_s(b, L"\\%016llx.jpg", (unsigned long long)id); return PhotoDir() + b; }

void SaveFile(const std::wstring& p, const std::string& text);
void SaveState(const std::string& text) { SaveFile(StatePath(), text); }
void SaveFile(const std::wstring& p, const std::string& text) {
    std::wstring tmp = p + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log("transfers: couldn't save state (error %lu)", GetLastError()); return; }
    DWORD w = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        Log("transfers: couldn't save state (error %lu)", GetLastError());
}

std::string LoadFile(const std::wstring& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
std::string LoadState() {
    std::ifstream f(StatePath(), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

uint64_t NewId() {
    static std::random_device rd;
    uint64_t v = ((uint64_t)rd() << 32) | rd();
    return v ? v : 1;
}

void Ring() {
    // An answer is needed: the phone's notification sound (also heard in the headset), unless
    // muted in Settings. The phone shows the request as soon as it is opened.
    Notify_Alert();
}

// ---- the thread ----------------------------------------------------------------------------
DWORD WINAPI NetThread(LPVOID) {
    // wait for the game to start Steam
    HMODULE sa = nullptr;
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        sa = GetModuleHandleW(L"steam_api64.dll");
        if (sa) break;
        if (GetTickCount64() - t0 > 180000) { g_status = "Steam not found"; Log("transfers: steam_api64.dll never loaded -> transfers off"); return 0; }
        Sleep(1000);
    }
    if (!Resolve(sa)) { g_status = "This Steam version isn't supported"; Log("transfers: steam_api64.dll lacks the functions Gitmo needs -> transfers off"); return 0; }
    while (!GuardedInterfaces(sa)) {
        if (GetTickCount64() - t0 > 300000) { g_status = "Steam isn't running"; Log("transfers: Steam never came up -> transfers off"); return 0; }
        Sleep(2000);
    }
    std::string myName = Steam_PersonaName();
    Log("transfers: Steam ready after %llu s (app %u)%s", (GetTickCount64() - t0) / 1000, g_app,
        S.mm ? "" : ", no lobby access");

    tx::IO io;
    io.send = [](uint64_t to, const std::string& m) {
        int r = GuardedSend(to, m);
        if (r != 1) Log("transfers: send to %llu failed (result %d)", (unsigned long long)to, r);
    };
    io.applyMoney = [](long long d, const std::string& label) { return Money_Apply(d, label); };
    io.balance = [] { return Game_InGame() ? Money_Balance() : -1; };   // no save loaded (main menu): no transfers either way
    io.ring = [] { Ring(); };
    io.persist = [](const std::string& s) { SaveState(s); };
    io.now = [] { return NowMs(); };
    io.wallclock = [] { return (long long)_time64(nullptr); };
    io.newId = [] { return NewId(); };
    io.log = [](const std::string& s) { Log("%s", s.c_str()); };
    io.note = [](const std::string& label, long long amount, const std::string& why) { Money_Note(label, amount, why); };
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_eng = new tx::Engine(io);
        g_eng->SetSelf(g_me, myName);
        g_eng->SetSessionOnly(g_cfg.sessionOnly);
        g_eng->Load(LoadState());
        sms::IO so;
        so.send = [](uint64_t to, const std::string& m) {
            int r = GuardedSend(to, m);
            if (r != 1) Log("sms: send to %llu failed (result %d)", (unsigned long long)to, r);
        };
        so.now = [] { return NowMs(); };
        so.wallclock = [] { return (long long)_time64(nullptr); };
        so.newId = [] { return NewId(); };
        so.persist = [](const std::string& s) { SaveFile(SmsPath(), s); g_smsVersion++; };
        so.log = [](const std::string& s) { Log("%s", s.c_str()); };
        so.ring = [](uint64_t, const std::string&, const std::string&) { Notify_Alert(); };
        so.storePhoto = [](uint64_t id, const std::string& jpeg) {
            CreateDirectoryW(PhotoDir().c_str(), nullptr);
            SaveFile(PhotoPath(id), jpeg);
            return GetFileAttributesW(PhotoPath(id).c_str()) != INVALID_FILE_ATTRIBUTES;
        };
        so.loadPhoto = [](uint64_t id) { return LoadFile(PhotoPath(id)); };
        so.deletePhoto = [](uint64_t id) { DeleteFileW(PhotoPath(id).c_str()); };
        g_sms = new sms::Engine(so);
        g_sms->Load(LoadFile(SmsPath()));
    }
    Log("transfers: %s", g_cfg.sessionOnly ? "only players in your game session (SessionOnly=1)" : "all friends playing SnowRunner (SessionOnly=0)");
    Log("transfers: session requests %s", GuardedRegister() ? "handled" : "NOT handled (register failed)");
    g_status = "Looking for players";
    g_ready = true;

    ULONGLONG nextFind = 0;
    int lastFriends = -1, lastLobby = -1, lastCoplay = -2;
    std::string lastSession = "-";
    uint64_t lastOurLobby = ~0ull;
    std::string lastMyRich;
    std::map<uint64_t, std::string> lastDiag;   // per player: last logged session-check line
    std::map<uint64_t, std::string> lastRich;
    std::map<uint64_t, ULONGLONG> richNext;
    std::vector<uint64_t> hello;
    RawMsg in[32];
    for (;;) {
        Sleep(50);
        int n = GuardedReceive(in, 32);
        ULONGLONG now = GetTickCount64();
        static ULONGLONG nextAvatars;
        if (now >= nextAvatars) { nextAvatars = now + 1000; FetchAvatars(); }   // Steam pictures for SMS contacts
        if (now >= nextFind) {   // who could we send to?
            nextFind = now + tx::kHelloEvery;
            // 1. our game session, from the game's own online SDK
            HySession hs = ReadHydra();
            {
                std::string sig = hs.ok ? hs.id : "?";
                for (auto& m : hs.members) sig += "," + m.hydra;
                if (sig != lastSession) {
                    if (!hs.ok) Log("session: can't read the game session yet");
                    else if (hs.id.empty()) Log("session: not in a game session (main menu or offline)");
                    else {
                        std::string who;
                        for (auto& m : hs.members)
                            who += (who.empty() ? "" : ", ") + (m.steam == g_me ? std::string("you") :
                                   !m.nick.empty() ? m.nick : m.steam ? std::to_string(m.steam) : m.hydra);
                        Log("session: game session %s, %zu player(s): %s", hs.id.c_str(), hs.members.size(), who.c_str());
                    }
                    lastSession = sig;
                }
            }
            auto inSession = [&](const std::string& hydra, uint64_t steam) {
                for (auto& m : hs.members) if ((!hydra.empty() && m.hydra == hydra) || (steam && m.steam == steam)) return true;
                return false;
            };

            // 2. who could we send to: friends in SnowRunner, Steam players in our session, Gitmo players we hear from
            std::vector<FriendInfo> fr;
            GuardedFriends(fr);
            if ((int)fr.size() != lastFriends) { Log("transfers: %zu friend(s) playing SnowRunner", fr.size()); lastFriends = (int)fr.size(); }
            struct Cand { uint64_t id; std::string name; bool isFriend; };
            std::vector<Cand> cands;
            auto add = [&](uint64_t id, const std::string& name, bool isFriend) {
                if (!id || id == g_me) return;
                for (auto& c : cands) if (c.id == id) return;
                cands.push_back({ id, name, isFriend });
            };
            for (auto& f : fr) add(f.id, f.name, true);
            for (auto& m : hs.members) if (m.steam && m.steam != g_me) add(m.steam, m.nick.empty() ? GuardedName(m.steam) : m.nick, GuardedHasFriend(m.steam));
            {   // (a friend can be missing from Steam's "playing" list, e.g. when shown as offline)
                std::vector<tx::Peer> gp;
                { std::lock_guard<std::mutex> lk(g_mx); gp = g_eng->OnlinePeersAll(); }
                for (auto& p : gp) add(p.id, p.name, GuardedHasFriend(p.id));
            }
            if (g_cfg.sessionResearch >= 2) {
                std::lock_guard<std::mutex> lk(g_cbMx);
                g_probePlayers.clear();
                for (auto& c : cands) g_probePlayers.push_back({ c.id, c.name });
            }
            struct Seen { uint64_t id; std::string name; bool direct; };
            std::vector<Seen> seen;
            for (auto& c : cands) {
                const char* h = c.isFriend ? GuardedRpRaw(c.id, "hydraUID") : nullptr;   // friends publish their Hydra id
                bool direct = inSession(h ? std::string(h) : std::string(), c.id);
                if (c.isFriend || direct) seen.push_back({ c.id, c.name, direct });
            }

            std::lock_guard<std::mutex> lk(g_mx);
            // Can't read the session (the game changed)? Then fall back to every friend in SnowRunner.
            g_eng->SetSessionOnly(g_cfg.sessionOnly && hs.ok);
            g_eng->SetMySession(hs.id);
            hello.clear();
            for (auto& x : seen) { g_eng->SetPeer(x.id, x.name, x.direct); hello.push_back(x.id); }
            for (uint64_t p : hello) g_eng->SayHello(p);
            // SMS contacts: everyone with Gitmo who is online now, plus everyone written with before
            std::vector<uint64_t> avatars;
            {
                std::set<uint64_t> on;
                for (auto& p : g_eng->OnlinePeersAll()) {
                    on.insert(p.id);
                    g_sms->SetPresence(p.id, p.name, true, g_eng->InGame(p.id));
                }
                for (auto& kv : g_sms->Contacts()) {
                    if (!on.count(kv.first)) g_sms->SetPresence(kv.first, "", false, false);
                    avatars.push_back(kv.first);
                }
                g_smsVersion++;
            }
            {
                std::lock_guard<std::mutex> al(g_avMx);
                for (uint64_t id : avatars) if (!g_avWanted.count(id)) g_avWanted[id] = 0;
            }
        }
        std::lock_guard<std::mutex> lk(g_mx);
        for (int i = 0; i < n; i++) {
            if (in[i].data.empty()) continue;
            if (in[i].data.compare(0, 3, "C1|") == 0) g_sms->OnMessage(in[i].from, in[i].data);
            else g_eng->OnMessage(in[i].from, in[i].data);
        }
        g_eng->Tick();
        g_sms->Tick();
        g_smsUnread = g_sms->Unread();
        int waiting = 0;
        for (auto& x : g_eng->Ins()) if (x.st == tx::In::Prompt) waiting++;
        g_txWaiting = waiting;
        size_t online = g_eng->OnlinePeers().size();
        if (g_cfg.sessionOnly)
            g_status = online ? std::to_string(online) + (online == 1 ? " player in your game has Gitmo" : " players in your game have Gitmo") : "No one in your game has Gitmo";
        else
            g_status = online ? std::to_string(online) + (online == 1 ? " player with Gitmo online" : " players with Gitmo online") : "No one with Gitmo online";
    }
}

}   // namespace

static void AddText(std::vector<ProbeTarget>& out, const std::string& who, const std::string& kind, const std::string& s, uintptr_t steamCopy = 0) {
    if (s.size() < 4) return;
    ProbeTarget t; t.who = who; t.kind = kind; t.bytes = s; t.steamCopy = steamCopy;
    out.push_back(t);
    std::string w;   // UTF-16 (ASCII part only; fine for ids)
    for (char c : s) { w += c; w += '\0'; }
    t.kind = kind + "-w"; t.bytes = w; t.steamCopy = 0;
    out.push_back(t);
}
static int HexVal(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void AddPlayer(std::vector<ProbeTarget>& out, uint64_t id, const std::string& who, const std::string& name) {
    ProbeTarget t; t.who = who; t.kind = "id"; t.bytes.assign((const char*)&id, 8); out.push_back(t);
    AddText(out, who, "id-text", std::to_string(id));
    if (name.size() >= 4) AddText(out, who, "name", name);
    const char* h = GuardedRpRaw(id, "hydraUID");
    std::string hs = h ? h : "";
    if (hs.size() >= 32) {
        AddText(out, who, "hydra", hs, (uintptr_t)h);
        std::string upper = hs; for (auto& c : upper) c = (char)toupper((unsigned char)c);
        if (upper != hs) AddText(out, who, "hydra-up", upper);
        std::string bin;   // 16 bytes in text order, and as a Windows GUID (first three groups little-endian)
        for (size_t i = 0; i + 1 < hs.size(); ) {
            if (hs[i] == '-') { i++; continue; }
            int a = HexVal(hs[i]), b = HexVal(hs[i + 1]);
            if (a < 0 || b < 0) { bin.clear(); break; }
            bin += (char)(a * 16 + b); i += 2;
        }
        if (bin.size() == 16) {
            ProbeTarget b1; b1.who = who; b1.kind = "hydra-bin"; b1.bytes = bin; out.push_back(b1);
            std::string g = bin;
            std::swap(g[0], g[3]); std::swap(g[1], g[2]); std::swap(g[4], g[5]); std::swap(g[6], g[7]);
            ProbeTarget b2; b2.who = who; b2.kind = "hydra-guid"; b2.bytes = g; out.push_back(b2);
        }
    }
}

void Net_ProbeTargets(std::vector<ProbeTarget>& out) {
    if (!g_ready) return;
    std::vector<std::pair<uint64_t, std::string>> players;
    { std::lock_guard<std::mutex> lk(g_cbMx); players = g_probePlayers; }
    if (players.empty()) return;
    for (auto& p : players) AddPlayer(out, p.first, p.second.empty() ? std::to_string(p.first) : p.second, p.second);
    AddPlayer(out, g_me, "you", "");
}

void Tx_Start() {
    if (HANDLE t = CreateThread(nullptr, 0, NetThread, nullptr, 0, nullptr)) CloseHandle(t);
}

TxView Tx_Snapshot() {
    TxView v;
    v.ready = g_ready;
    std::lock_guard<std::mutex> lk(g_mx);
    v.status = g_status;
    v.me = g_me;
    if (!g_eng) return v;
    v.peers = g_eng->OnlinePeers();
    v.outs = g_eng->Outs();
    v.ins = g_eng->Ins();
    return v;
}

// ---- SMS for the phone ----
bool Sms_Snapshot(unsigned haveVersion, SmsView& out) {
    unsigned v = g_smsVersion.load();
    if (v == haveVersion && out.ready) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    out.ready = g_sms != nullptr;
    out.version = v;
    out.contacts.clear();
    out.receiving.clear();
    if (g_sms) for (auto& kv : g_sms->Contacts()) { out.contacts.push_back(kv.second); if (int r = g_sms->Receiving(kv.first)) out.receiving[kv.first] = r; }
    return true;
}
// A photo taken with the phone goes into the player's Steam screenshots for SnowRunner, like
// one taken with F12: Steam saves it at full size, makes the thumbnail and lists it in its
// screenshot manager (and so in the phone's own photo panel).
static uint32_t GuardedWriteShot(void* rgb, uint32_t size, int w, int h) {
    __try { return S.writeShot(S.screenshots, rgb, size, w, h); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool Steam_Available() { return g_ready && S.screenshots && S.writeShot; }
bool Steam_SaveScreenshot(const PhotoImg& img) {
    if (!Steam_Available()) return false;
    if (img.w <= 0 || img.h <= 0 || img.rgba.size() < (size_t)img.w * img.h * 4) return false;
    std::vector<uint8_t> rgb((size_t)img.w * img.h * 3);
    for (size_t i = 0, o = 0, n = (size_t)img.w * img.h; i < n; i++, o += 3) {
        rgb[o] = img.rgba[i * 4]; rgb[o + 1] = img.rgba[i * 4 + 1]; rgb[o + 2] = img.rgba[i * 4 + 2];
    }
    uint32_t h = GuardedWriteShot(rgb.data(), (uint32_t)rgb.size(), img.w, img.h);
    if (h) Log("photo: saved to Steam screenshots (%dx%d)", img.w, img.h);
    else Log("photo: Steam didn't take the screenshot");
    return h != 0;
}

bool Sms_SendPhoto(uint64_t peer, const std::string& jpeg) {
    std::lock_guard<std::mutex> lk(g_mx);
    bool ok = g_sms && g_sms->SendPhoto(peer, jpeg);
    g_smsVersion++;
    return ok;
}
std::wstring Sms_PhotoPath(uint64_t id) { return PhotoPath(id); }
bool Sms_Send(uint64_t peer, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mx);
    bool ok = g_sms && g_sms->Send(peer, text);
    g_smsVersion++;
    return ok;
}
void Sms_EraseAll() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_sms) return;
    g_sms->EraseAll();
    g_smsUnread = 0;
    g_smsVersion++;
}
void Sms_Viewing(uint64_t peer) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_sms) return;
    g_sms->SetViewing(peer);
    g_smsUnread = g_sms->Unread();
    g_smsVersion++;
}
bool Avatar_Get(uint64_t id, AvatarImg& out) {
    std::lock_guard<std::mutex> lk(g_avMx);
    auto it = g_avatars.find(id);
    if (it != g_avatars.end()) { out = it->second; return true; }
    if (!g_avWanted.count(id)) g_avWanted[id] = 0;
    return false;
}

tx::SendResult Tx_Send(uint64_t peer, long long amount, uint64_t* id) {
    // Not from a garage: its screens keep the balance they read when they opened, so money
    // sent away there could still be spent on upgrades (tested 2026-11-10: $498 bought a $600
    // item and the balance went to -$102). Receiving there is fine.
    std::lock_guard<std::mutex> lk(g_mx);
    return g_eng ? g_eng->Send(peer, amount, id) : tx::SendResult::Offline;
}
bool Tx_Cancel(uint64_t id) { std::lock_guard<std::mutex> lk(g_mx); return g_eng && g_eng->Cancel(id); }
bool Tx_Accept(uint64_t peer, uint64_t id) { std::lock_guard<std::mutex> lk(g_mx); return g_eng && g_eng->Accept(peer, id); }
bool Tx_Decline(uint64_t peer, uint64_t id) { std::lock_guard<std::mutex> lk(g_mx); return g_eng && g_eng->Decline(peer, id); }
void Tx_MarkSeen(uint64_t peer, uint64_t id) { std::lock_guard<std::mutex> lk(g_mx); if (g_eng) g_eng->MarkSeen(peer, id); }
