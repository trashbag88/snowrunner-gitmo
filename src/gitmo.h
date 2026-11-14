// Gitmo - shared declarations.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include "transfer.h"
#include "chat.h"

#define GITMO_VERSION "0.9.12"

// ---- core (gitmo.cpp) ------------------------------------------------------
void Log(const char* fmt, ...);
void LogWatchdogReport(const std::string& report); // never waits to acquire the shared log lock
std::wstring ModuleDir();               // folder that holds version.dll (the game's Bin)
extern uintptr_t g_exeBase, g_exeEnd;   // SnowRunner.exe image range
void Watchdog_Start();                  // (watches that Gitmo's threads keep running)

struct Settings {
    bool enabled = true;
    bool log = true;
    bool research = true;   // stage one: log where the game keeps money
    int  menuKey = VK_DELETE;
    std::string menuKeyName = "Delete";
    bool dark = false;
    int notifyMode = 0;                 // notifications: 0 sound (default), 1 vibrate, 2 off
    bool sessionOnly = true;            // transfers only with players in the same game session
    int sessionResearch = 0;            // research: 1 = log the game's Hydra session status, 2 = also search memory for ids
    bool developer = false;             // Developer=1: show the developer section (money test buttons) in Settings
    bool escapeOnSend = true;           // EscapeOnSend=1: press Esc for the game when money leaves the balance
    int escapeVia = 0;                  // EscapeVia=both|directinput|window (0 / 1 / 2)
    int garageResearch = 0;             // research: 1 find the garage screens' copy of the balance, 2 update it (GarageResearch=)
    bool moneyResearch = false;         // research: log the game's call stacks when it changes the money (MoneyResearch=1)
    float phoneX = -1, phoneY = -1;     // phone's top-left as a share of the screen; -1 = default (right side)
    bool cracked = false;               // cracked-screen look
    std::string recentEmoji;            // SMS emoji panel: recently used, hex code points "1F600,1F44D"
    int wallpaper = 0;                  // home-screen wallpaper (0..4)
    int phoneSize = 1;                  // 0 small, 1 medium, 2 large (share of the screen height)
    float batteryMin = 15.0f;           // battery left, minutes (saved between sessions)
    int display = 0;                    // 0 auto (headset panel when VR is running), 1 screen, 2 headset
    float vrDistance = 0.6f;            // metres from the head to the phone panel in VR
    float vrPointerSpeed = 2.0f;        // VR: mouse pointer speed on the phone (1.0 = whole window across the phone)
    bool savePhotos = true;             // keep photos taken with the phone: Steam's screenshots, or Pictures\Gitmo without Steam
    // Found with the teaching marks on 2026-11-08 (game build 0x6ADB8627):
    int engineOffset = 0x768;           // Vehicle state byte: 3 = engine running, 0 = off
    int garageOffset = 0x2C;            // truck-control byte: 1 = in the garage, 0 = driving
};
extern Settings g_cfg;

// Each of these updates g_cfg and writes Gitmo.ini
void SaveThemeSetting(bool dark);
void SavePhonePos(float x, float y);
void SaveCracked(bool on);
void SaveRecentEmoji(const std::string& hexList);
void SaveWallpaper(int i);
void SavePhoneSize(int i);
void SaveDisplay(int mode);
void SaveVrDistance(float metres);
void SaveVrPointerSpeed(float speed);
void SaveNotifyMode(int mode);           // 0 sound, 1 vibrate, 2 off
void SaveBattery(float minutes);         // (writes Gitmo.ini only; the live value is in game.cpp)
void SaveMenuKey(int vk);                // sets g_cfg.menuKey/menuKeyName
std::string KeyName(int vk);            // "Delete", "F8", "K", or "0x2E" for anything unnamed
extern std::atomic<bool> g_captureKey;  // phone's "Change" button: next key pressed becomes the menu key

void Notify_Alert();                    // money received / new message: sound, vibrate or nothing, by the setting
void Notify_Vibrate();
void Notify_Play();                     // the phone's notification sound (Gitmo-notify.wav next to the DLL overrides the built-in one)

// ---- VR headset panel (vr.cpp) -------------------------------------------------
struct ID3D11Device; struct ID3D11RenderTargetView;
void VR_Start();                                   // before the VR mod starts OpenXR
bool VR_PanelWanted();                             // draw the phone into the headset panel this frame?
const char* VR_Status();
ID3D11RenderTargetView* VR_PhoneTarget(ID3D11Device* dev, int& w, int& h);
void VR_PhoneDrawn();                              // phone texture is up to date for this frame
void VR_Replace();                                 // put the phone in front of the head again
void VR_DragBy(float dYaw, float dPitch);          // swing the phone around the head (radians)
extern bool g_vrDragging;                          // the phone's border is being dragged in VR
extern bool g_uiVrPanel;                           // this frame the phone fills the headset panel texture

// ---- game state + battery (game.cpp) -----------------------------------------
void Game_Start();
void Game_Stop();                       // requests a cooperative stop after the current sleep or scan
uintptr_t Game_CurrentVehicle();
int Game_EngineRunning();               // 1 / 0 / -1 unknown
int Game_InGarage();                    // 1 / 0 / -1 unknown
uintptr_t Game_SessionId();             // 0 in the main menu; otherwise a value that changes with every loaded save
bool Game_InGame();                     // a save is loaded (single player or multiplayer)
void Game_Mark(int kind);               // research: 0 engine on, 1 engine off, 2 in garage, 3 driving
int Game_MarkCount(int kind);
uintptr_t Game_FindKeyTable();          // the game's own key-down table (0 = not found)
int Game_FindXInputSlots(uintptr_t* out, int max);   // globals holding the game's XInputGetState
float Battery_Minutes();
void Battery_Set(float minutes);
int Battery_State();                    // 0 paused/unknown, 1 charging, 2 on battery
extern const float kBatteryMinutes;

// ---- money (money.cpp) -----------------------------------------------------
struct SaveValues {
    bool ok = false;
    // kept as doubles on purpose: Gitmo's own copy must never look like the
    // game's integer money/experience pair to the memory scan - and NOT side by side:
    // the garage screens keep money, experience, rank as three doubles in a row, which
    // the test-only legacy scanner (money.cpp GarageCopiesUpdate) checks, so this copy must not match
    double money = 0;
    int32_t notAdjacent1 = 0x5A5A5A5A;
    double xp = 0;
    int32_t notAdjacent2 = 0x5A5A5A5A;
    double rank = 0;
    std::wstring file;
    bool savedThisSession = false;   // file written after the game started
};

struct Candidate {
    uintptr_t addr = 0;
    int xpOff = 0, rankOff = 0;   // where experience/rank sit relative to money (0 = not found)
    long long value = 0;
    int slot = 0;                 // save slot whose values it matched (1-4)
    int32_t minXp = 0;            // that save's experience (the live value only grows)
};

struct Activity {
    SYSTEMTIME when{};
    long long delta = 0;
    long long balance = 0;
    std::string label;             // e.g. "To Bob" for a transfer; empty = a change the game made
    std::string status;            // set when nothing moved, e.g. "Bob declined" (amount in .amount)
    long long amount = 0;
};

struct MoneyState {                // snapshot for the UI
    enum Phase { Waiting, Scanning, Found, NotFound } phase = Waiting;
    bool live = false;             // address found and readable
    bool sure = false;             // certain this copy is the one the game uses (transfers need it)
    bool group = false;            // sure only because all copies of the save hold the same amount:
                                   // transfers then change every copy (co-op guests: no fresh copy on joining)
    int slot = 0;                  // save slot being played (0 = not known yet)
    long long balance = 0;
    SaveValues save;
    std::vector<Candidate> cands;
    int selected = 0;
    std::vector<Activity> activity;   // newest first
    std::string status;
    int scans = 0;
};

void Money_Start();                     // starts the background finder thread
// Session research: byte strings to look for in game memory (another player's ids)
struct ProbeTarget { std::string who, kind, bytes; uintptr_t steamCopy = 0; };
void Net_ProbeTargets(std::vector<ProbeTarget>& out);   // net.cpp
void Probe_Start();                                      // money.cpp
void MoneyWatch_Start();                                 // money.cpp: MoneyResearch=1
bool Money_Snapshot(uint64_t& version, MoneyState& out);  // false: unchanged since the previous snapshot
void Money_Rescan();
void Money_Select(int i);
bool Money_TestAdjust(long long delta); // developer test: write balance +/- delta
long long Money_Balance();              // live balance, or -1 when not known for certain (not in a map)
bool Money_Apply(long long delta, const std::string& label);   // transfers: change the balance, logged with a label
void Money_Note(const std::string& label, long long amount, const std::string& status);   // a transfer that moved nothing

// ---- transfers between players (net.cpp, transfer.cpp) ---------------------------
struct TxView {
    bool ready = false;                 // Steam found and transfers running
    std::string status;                 // e.g. "Looking for players"
    uint64_t me = 0;
    std::vector<tx::Peer> peers;        // online players with Gitmo
    std::vector<tx::Outgoing> outs;
    std::vector<tx::Incoming> ins;
};
void Tx_Start();
bool Tx_Snapshot(uint64_t& version, TxView& out);  // false: unchanged since the previous snapshot
tx::SendResult Tx_Send(uint64_t peer, long long amount, uint64_t* id);
bool Tx_Cancel(uint64_t id);
bool Tx_Accept(uint64_t peer, uint64_t id);
bool Tx_Decline(uint64_t peer, uint64_t id);
void Tx_MarkSeen(uint64_t peer, uint64_t id);
extern std::atomic<int> g_txWaiting;    // incoming requests waiting for an answer (for the closed phone)

// ---- SMS: text messages between Gitmo players (chat.cpp, net.cpp) -----------------
struct SmsView {
    bool ready = false;
    unsigned version = 0;               // changes whenever anything in it changes
    std::vector<sms::Contact> contacts;
    std::map<uint64_t, int> receiving;   // contact -> photos coming in from them right now
};
bool Sms_Snapshot(unsigned haveVersion, SmsView& out);   // false: nothing new since haveVersion
bool Sms_Send(uint64_t peer, const std::string& text);
bool Sms_SendPhoto(uint64_t peer, const std::string& jpeg);   // a JPEG (Photo_* below makes one)
std::wstring Sms_PhotoPath(uint64_t messageId);               // where a photo message's JPEG is kept
void Sms_Viewing(uint64_t peer);        // the conversation open on the phone (0 = none)
void Sms_EraseAll();                    // Settings > Messages > Erase: every conversation on this PC
extern std::atomic<int> g_smsUnread;

// ---- Steam names and avatars (net.cpp) ----------------------------------------------
struct AvatarImg { int w = 0, h = 0; std::vector<uint8_t> rgba; };
bool Avatar_Get(uint64_t steamId, AvatarImg& out);   // true once Steam has the picture (asks for it otherwise)
std::string Steam_PersonaName();

// ---- photos for SMS (photo.cpp) ----------------------------------------------------
struct PhotoImg { int w = 0, h = 0; std::vector<uint8_t> rgba; };
std::string Photo_ReadFile(const std::wstring& path);
bool Photo_Decode(const std::string& bytes, int maxSide, PhotoImg& out);   // JPEG/PNG -> RGBA, at most maxSide
bool Photo_EncodeJpeg(const PhotoImg& img, int maxSide, std::string& jpeg);
// a worker thread makes the JPEG to send (and a preview) from a captured frame or a file:
void Photo_FromCapture(PhotoImg&& frame);
void Photo_FromFile(const std::wstring& path);
void Photo_Failed();                       // (capture couldn't be read)
int Photo_State();                         // 0 nothing, 1 working, 2 ready, 3 failed
bool Photo_Take(std::string& jpeg, PhotoImg& preview);   // the ready result (then State() is 0)
void Photo_Cancel();
std::vector<std::wstring> Photo_SteamScreenshots(uint64_t steamId, int max);   // SnowRunner's, newest first
bool Steam_Available();                          // the game runs on Steam and its screenshot library can be used (net.cpp)
bool Steam_SaveScreenshot(const PhotoImg& img);   // into the player's Steam screenshots for SnowRunner (net.cpp)
std::wstring Photo_OwnFolder();                  // where photos go without Steam (Epic etc.): Pictures\Gitmo
bool Photo_SaveOwn(const PhotoImg& img);         // a full-size JPEG + thumbnail there
std::wstring Photo_SteamThumb(const std::wstring& shot);                       // Steam's small copy, if any
bool Photo_DecodeAsync(const std::string& key, const std::wstring& path, int maxSide);   // queued for an off-thread decode
bool Photo_DecodedNext(std::string& key, bool& ok, PhotoImg& img);                     // one finished decode, if any

// ---- internet radio (radio.cpp) ------------------------------------------------------
struct RadioStation { std::string name, url, genre; };
struct RadioState {
    int state = 0;                      // 0 off, 1 connecting, 2 buffering, 3 playing, 4 error
    std::string err;                    // state 4: what went wrong, in plain words
    std::string url, name, genre;       // the station (kept after Stop, for Now Playing)
    std::string icyName;                // the name the stream gives itself
    std::string title;                  // now playing, "Artist - Title" as the station sends it
    std::string codec;                  // "MP3" / "AAC"
    int kbps = 0;
    int volume = 60;                    // 0..100, the radio's own (the game's volume is separate)
};
struct RadioFound { std::string name, url, genre, codec, country, uuid; int kbps = 0; };   // Browse result
void Radio_Start();                     // loads Gitmo-radio.ini
std::vector<RadioStation> Radio_Stations();
unsigned Radio_StationsVersion();       // changes whenever the list changes
bool Radio_SetStations(const std::vector<RadioStation>& list);   // saves Gitmo-radio.ini; false leaves the saved list unchanged
void Radio_Play(const RadioStation& st);
void Radio_Stop();
RadioState Radio_State();
int Radio_Volume();
void Radio_SetVolume(int v, bool save);
void Radio_Search(const std::string& query);   // radio-browser.info; "" = most popular
int Radio_SearchResult(unsigned& version, std::vector<RadioFound>& out, std::string& err, std::string& query);   // copies results only when version changes
void Radio_CountClick(const std::string& uuid);

// ---- overlay and input (overlay.cpp, input.cpp) ----------------------------------------
void Overlay_Start();                   // hooks DXGI so the menu can draw
HWND Overlay_GameWindow();              // the game's main window
void Overlay_RequestCapture();          // grab the next game frame (before the phone is drawn)
void Overlay_FreeTexture(void* srv);
void* Overlay_MakeTexture(const uint8_t* rgba, int w, int h);   // an ID3D11ShaderResourceView* for ImGui
void Input_Start();                     // keeps clicks/keys from the game while the phone is open
void Input_Frame(bool phoneOpen);       // every frame (overlay): clears the game's key table while open, logs what got through
void Input_NoteWindowKey();             // a key message reached Gitmo's window hook while the phone was open
void Input_PressEscape();               // Esc for the game once (soon), so an open menu rereads the balance
bool Input_IsGitmoKey(UINT m, WPARAM w, LPARAM l);
extern std::atomic<bool> g_menuOpen;
extern std::atomic<bool> g_uiTyping;    // a text field on the phone has the keyboard: the menu key types instead

// ---- UI (ui.cpp, emoji.cpp) ------------------------------------------------------
void UI_Init(float screenH);            // fonts + style, called once after ImGui is set up
void UI_Draw();                         // draws the phone; called every frame while open
void UI_PhoneClosed();                  // the phone was put away
struct ImFontLoader; const ImFontLoader* Emoji_FontLoader();   // emoji.cpp: colour emoji for the phone's fonts