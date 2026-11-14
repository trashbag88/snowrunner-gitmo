// Gitmo - the phone's internet radio.
//
//   network (WinHTTP, or a plain socket for old "ICY 200 OK" servers)
//     -> ICY metadata taken out (the song title)
//     -> MP3 / AAC (ADTS) frames found in the byte stream
//     -> Windows' own decoder (Media Foundation MFT) -> 16-bit PCM
//     -> waveOut on the default output device (so in VR it reaches the headset too)
//
// Each Play starts a fresh session with its own reader and player threads. Stop or a
// new Play cancels the old request and signals the player without joining either thread.
// The radio's volume is applied to the samples, so the game's own volume is untouched.
// Stations are kept in Gitmo-radio.ini next to the DLL.

#include "gitmo.h"
#include "shared.h"
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <deque>
#include <memory>
#include <thread>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>
#include <exception>
#include <system_error>
#include <cerrno>
#include <climits>
#include <cstdlib>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

static const wchar_t* kAgent = L"Gitmo/" _CRT_WIDE(GITMO_VERSION) L" (SnowRunner phone mod)";

// ---- small helpers -------------------------------------------------------------------
static std::wstring Wide(const std::string& s) {
    if (s.empty() || s.size() > INT_MAX) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, 0);
    int written = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    if (written <= 0) return {};
    w.resize((size_t)written);
    return w;
}
static std::string Utf8(const std::wstring& w) {
    if (w.empty() || w.size() > INT_MAX) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, 0);
    int written = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    if (written <= 0) return {};
    s.resize((size_t)written);
    return s;
}
static std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static int ParseInt(const std::string& s, int fallback = 0) {
    if (s.empty()) return fallback;
    errno = 0;
    char* end = nullptr;
    long value = strtol(s.c_str(), &end, 10);
    if (errno == ERANGE || end == s.c_str() || *end || value < INT_MIN || value > INT_MAX) return fallback;
    return (int)value;
}
// Latin-1 text (old servers' headers and titles) to UTF-8; valid UTF-8 is left alone
static std::string FixText(const std::string& s) {
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0 || s.empty()) return s;
    std::string o;
    for (unsigned char c : s) { if (c < 0x80) o += (char)c; else { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); } }
    return o;
}

// ---- HTTP: WinHTTP, with a socket fallback for Shoutcast v1 ("ICY 200 OK") --------------
class Http {
public:
    ~Http() { Close(); }
    // Opens url (following redirects); false + err on failure
    bool Open(const std::string& url, bool icy, std::string& err) {
        if (url.empty() || url.size() > 8192 ||
            std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; })) {
            err = "That web link is invalid or too long";
            return false;
        }
        std::string u = url;
        for (int hop = 0; hop < 6; hop++) {
            Close();
            int r = OpenWinHttp(u, icy, err);
            if (r == 2) r = OpenSocket(u, icy, err);   // the server answered in a way WinHTTP won't accept
            if (r == 3) {
                u = ResolveRedirect(u, location_);
                if (u.empty()) { err = "The server sent an invalid redirect"; return false; }
                continue;
            }
            return r == 1;
        }
        err = "Too many redirects";
        return false;
    }
    void Cancel() {
        cancelled_ = true;
        HINTERNET req = req_.exchange(nullptr);
        if (req) WinHttpCloseHandle(req);
        SOCKET sock = sock_.load();
        if (sock != INVALID_SOCKET) shutdown(sock, SD_BOTH);
    }
    int Read(uint8_t* buf, int n) {   // >0 bytes, 0 end of stream, -1 error
        if (cancelled_) return -1;
        if (pre_.size()) { int k = std::min<int>(n, (int)pre_.size()); memcpy(buf, pre_.data(), k); pre_.erase(0, k); return k; }
        HINTERNET req = req_.load();
        if (req) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) return -1;
            if (avail == 0) return 0;
            DWORD got = 0;
            if (!WinHttpReadData(req, buf, std::min<DWORD>(avail, (DWORD)n), &got)) return -1;
            return (int)got;
        }
        SOCKET sock = sock_.load();
        if (sock != INVALID_SOCKET) { int k = recv(sock, (char*)buf, n, 0); return k < 0 ? -1 : k; }
        return -1;
    }
    std::string Header(const std::string& name) const { auto it = hdr_.find(Lower(name)); return it == hdr_.end() ? std::string() : it->second; }
    bool ReadAll(std::string& out, size_t max) {
        uint8_t b[16384];
        for (;;) {
            int k = Read(b, sizeof b);
            if (k < 0) return false;
            if (k == 0) return true;
            if ((size_t)k > max - std::min(max, out.size())) { out.clear(); return false; }
            out.append((char*)b, (size_t)k);
        }
    }
    void Close() {
        HINTERNET req = req_.exchange(nullptr);
        if (req) WinHttpCloseHandle(req);
        if (con_) WinHttpCloseHandle(con_);
        if (ses_) WinHttpCloseHandle(ses_);
        con_ = ses_ = nullptr;
        SOCKET sock = sock_.exchange(INVALID_SOCKET);
        if (sock != INVALID_SOCKET) { shutdown(sock, SD_BOTH); closesocket(sock); }
        hdr_.clear(); pre_.clear();
    }
private:
    HINTERNET ses_ = nullptr, con_ = nullptr;
    std::atomic<HINTERNET> req_{nullptr};
    std::atomic<SOCKET> sock_{INVALID_SOCKET};
    std::atomic<bool> cancelled_{false};
    std::map<std::string, std::string> hdr_;
    std::string pre_, location_;

    static std::string ResolveRedirect(const std::string& base, const std::string& location) {
        if (location.empty()) return {};
        std::string locationPrefix = Lower(location.substr(0, std::min<size_t>(8, location.size())));
        if (locationPrefix.compare(0, 7, "http://") == 0 || locationPrefix.compare(0, 8, "https://") == 0) return location;
        size_t scheme = base.find("://");
        if (scheme == std::string::npos) return {};
        std::string origin = base.substr(0, scheme + 3);
        size_t pathStart = base.find_first_of("/?#", scheme + 3);
        if (pathStart == std::string::npos) return location[0] == '/' ? origin + location : origin + "/" + location;
        origin = base.substr(0, pathStart);
        if (location.compare(0, 2, "//") == 0) return base.substr(0, scheme + 1) + location;
        if (location[0] == '/') return origin + location;
        std::string path = base.substr(pathStart);
        size_t suffix = path.find_first_of("?#");
        if (suffix != std::string::npos) path.resize(suffix);
        if (location[0] == '?') return origin + (path.empty() ? "/" : path) + location;
        size_t slash = path.find_last_of('/');
        return origin + (slash == std::string::npos ? "/" : path.substr(0, slash + 1)) + location;
    }

    // 1 ok, 0 failed, 2 try the socket
    int OpenWinHttp(const std::string& url, bool icy, std::string& err) {
        if (cancelled_) { err = "Connection cancelled"; return 0; }
        std::wstring w = Wide(url);
        URL_COMPONENTS uc{}; uc.dwStructSize = sizeof uc;
        wchar_t host[256], path[2048], extra[2048];
        uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
        if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc) || (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)) { err = "That isn't a web link (http:// or https://)"; return 0; }
        ses_ = WinHttpOpen(kAgent, 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses_) ses_ = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses_) { err = "Couldn't start the connection"; return 0; }
        WinHttpSetTimeouts(ses_, 10000, 10000, 10000, 15000);
        con_ = WinHttpConnect(ses_, host, uc.nPort, 0);
        std::wstring pq = std::wstring(path) + extra;
        if (pq.empty()) pq = L"/";
        HINTERNET req = con_ ? WinHttpOpenRequest(con_, L"GET", pq.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
        req_ = req;
        if (!req) { err = "Couldn't reach the server"; return 0; }
        if (cancelled_) { Cancel(); err = "Connection cancelled"; return 0; }
        const wchar_t* hdrs = icy ? L"Icy-MetaData: 1\r\n" : L"Accept: application/json\r\n";
        if (!WinHttpSendRequest(req, hdrs, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req, nullptr)) {
            DWORD e = GetLastError();
            if (e == ERROR_WINHTTP_INVALID_SERVER_RESPONSE && uc.nScheme == INTERNET_SCHEME_HTTP) return 2;
            err = e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "Server not found" : e == ERROR_WINHTTP_TIMEOUT ? "The server didn't answer" :
                  e == ERROR_WINHTTP_CANNOT_CONNECT ? "Couldn't connect to the server" : e == ERROR_WINHTTP_SECURE_FAILURE ? "The server's security certificate isn't valid" :
                  "Couldn't connect (error " + std::to_string(e) + ")";
            return 0;
        }
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        // all headers, raw, into the map
        sz = 0;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &sz, WINHTTP_NO_HEADER_INDEX);
        if (sz) {
            std::wstring raw(sz / 2 + 1, 0);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &sz, WINHTTP_NO_HEADER_INDEX)) ParseHeaders(Utf8(raw.c_str()));
        }
        if (code != 200) { err = code == 404 ? "Station not found at that link (404)" : "The server said " + std::to_string(code); return 0; }
        return 1;
    }

    void ParseHeaders(const std::string& raw) {
        size_t p = 0;
        while (p < raw.size()) {
            size_t e = raw.find("\r\n", p); if (e == std::string::npos) e = raw.size();
            std::string line = raw.substr(p, e - p);
            size_t c = line.find(':');
            if (c != std::string::npos) hdr_[Lower(gitmo_shared::TrimAsciiWhitespace(line.substr(0, c)))] = FixText(gitmo_shared::TrimAsciiWhitespace(line.substr(c + 1)));
            p = e + 2;
        }
    }

    // 1 ok, 0 failed, 3 redirect to location_
    int OpenSocket(const std::string& url, bool icy, std::string& err) {
        Close();
        static bool wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
        if (!wsa) { err = "Couldn't start networking"; return 0; }
        if (url.compare(0, 7, "http://") != 0) { err = "The socket fallback only supports http links"; return 0; }
        std::string rest = url.substr(7);   // after http://
        size_t sl = rest.find_first_of("/?#");
        std::string hostport = rest.substr(0, sl);
        std::string path = sl == std::string::npos ? "/" : rest.substr(sl);
        if (path[0] == '?') path.insert(path.begin(), '/');
        size_t fragment = path.find('#');
        if (fragment != std::string::npos) path.resize(fragment);
        if (path.empty()) path = "/";
        if (hostport.empty()) { err = "That isn't a web link"; return 0; }
        std::string host, port = "80";
        if (hostport[0] == '[') {
            size_t close = hostport.find(']');
            if (close == std::string::npos) { err = "That isn't a web link"; return 0; }
            host = hostport.substr(1, close - 1);
            if (close + 1 < hostport.size()) {
                if (hostport[close + 1] != ':') { err = "That isn't a web link"; return 0; }
                port = hostport.substr(close + 2);
            }
        } else {
            size_t col = hostport.rfind(':');
            if (col != std::string::npos) { host = hostport.substr(0, col); port = hostport.substr(col + 1); }
            else host = hostport;
        }
        int portNumber = ParseInt(port, -1);
        if (host.empty() || portNumber < 1 || portNumber > 65535) { err = "That isn't a web link"; return 0; }
        addrinfo hints{}, *res = nullptr; hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) { err = "Server not found"; return 0; }
        for (addrinfo* a = res; a; a = a->ai_next) {
            SOCKET sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (sock == INVALID_SOCKET) continue;
            sock_ = sock;
            DWORD to = 15000; setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof to);
            if (!cancelled_ && connect(sock, a->ai_addr, (int)a->ai_addrlen) == 0) break;
            SOCKET owned = sock_.exchange(INVALID_SOCKET);
            if (owned != INVALID_SOCKET) closesocket(owned);
        }
        freeaddrinfo(res);
        SOCKET sock = sock_.load();
        if (sock == INVALID_SOCKET || cancelled_) { err = "Couldn't connect to the server"; return 0; }
        std::string rq = "GET " + path + " HTTP/1.0\r\nHost: " + hostport + "\r\nUser-Agent: " + Utf8(kAgent) + "\r\n" + (icy ? "Icy-MetaData: 1\r\n" : "") + "Connection: close\r\n\r\n";
        size_t sent = 0;
        while (sent < rq.size()) {
            int n = send(sock, rq.data() + sent, (int)std::min<size_t>(rq.size() - sent, INT_MAX), 0);
            if (n <= 0) { err = "Couldn't send the station request"; return 0; }
            sent += (size_t)n;
        }
        std::string head;
        char b[4096];
        while (head.find("\r\n\r\n") == std::string::npos && head.find("\n\n") == std::string::npos) {
            int k = recv(sock, b, sizeof b, 0);
            if (k <= 0) { err = "The server closed the connection"; return 0; }
            head.append(b, k);
            if (head.size() > 65536) { err = "The server's answer wasn't understood"; return 0; }
        }
        size_t end = head.find("\r\n\r\n"); size_t skip = 4;
        if (end == std::string::npos) { end = head.find("\n\n"); skip = 2; }
        pre_ = head.substr(end + skip);
        std::string hs = head.substr(0, end);
        size_t le = hs.find('\n');
        std::string status = hs.substr(0, le);
        ParseHeaders(le == std::string::npos ? "" : hs.substr(le + 1));
        int code = 0;
        size_t sp = status.find(' ');
        size_t codeEnd = sp == std::string::npos ? std::string::npos : status.find(' ', sp + 1);
        if (sp != std::string::npos)
            code = ParseInt(status.substr(sp + 1, codeEnd == std::string::npos ? std::string::npos : codeEnd - sp - 1));
        if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && !Header("location").empty()) { location_ = Header("location"); return 3; }
        if (code != 200) { err = "The server said " + std::to_string(code); return 0; }
        return 1;
    }
};

// ---- stations file ---------------------------------------------------------------------
static std::mutex g_stMx;
static std::vector<RadioStation> g_stations;
static std::atomic<int> g_volume{60};
static std::atomic<unsigned> g_stVersion{1};

static std::wstring StationsPath() { return ModuleDir() + L"\\Gitmo-radio.ini"; }

static bool SaveStations() {   // with g_stMx held
    std::string out = "; Gitmo radio - your stations, in the order the phone shows them.\r\n"
                      "; One per line:  Name | Stream link | Genre\r\n"
                      "; (Edited on the phone; you can also edit this file while the game is closed.)\r\n"
                      "[Radio]\r\nVolume=" + std::to_string(g_volume.load()) + "\r\n\r\n[Stations]\r\n";
    for (auto& st : g_stations) {
        auto clean = [](std::string v) { for (auto& c : v) if (c == '|' || c == '\r' || c == '\n') c = ' '; return gitmo_shared::TrimAsciiWhitespace(v); };
        out += clean(st.name) + " | " + clean(st.url) + " | " + clean(st.genre) + "\r\n";
    }
    if (out.size() > MAXDWORD) { Log("radio: station list is too large to save"); return false; }
    const std::wstring path = StationsPath();
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log("radio: couldn't write Gitmo-radio.ini (error %lu)", GetLastError()); return false; }
    size_t off = 0;
    bool ok = true;
    DWORD error = ERROR_SUCCESS;
    while (off < out.size()) {
        DWORD written = 0;
        DWORD chunk = (DWORD)std::min<size_t>(out.size() - off, MAXDWORD);
        if (!WriteFile(h, out.data() + off, chunk, &written, nullptr) || !written) {
            ok = false; error = GetLastError();
            if (!error) error = ERROR_WRITE_FAULT;
            break;
        }
        off += written;
    }
    if (ok && !FlushFileBuffers(h)) { ok = false; error = GetLastError(); }
    if (!CloseHandle(h) && ok) { ok = false; error = GetLastError(); }
    if (!ok) {
        DeleteFileW(tmp.c_str());
        Log("radio: couldn't finish writing Gitmo-radio.ini (error %lu)", error);
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        DeleteFileW(tmp.c_str());
        Log("radio: couldn't replace Gitmo-radio.ini (error %lu)", error);
        return false;
    }
    return true;
}

static void LoadStations() {
    std::lock_guard<std::mutex> lk(g_stMx);
    g_stations.clear();
    std::string text = Photo_ReadFile(StationsPath());
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF) text = text.substr(3);   // BOM
    std::string sec;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p); if (e == std::string::npos) e = text.size();
        std::string line = gitmo_shared::TrimAsciiWhitespace(text.substr(p, e - p));
        p = e + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { sec = Lower(line); continue; }
        if (sec == "[radio]") {
            if (Lower(line).rfind("volume=", 0) == 0) g_volume = std::min(100, std::max(0, ParseInt(line.substr(7))));
            continue;
        }
        if (!sec.empty() && sec != "[stations]") continue;
        RadioStation st;
        size_t a = line.find('|');
        if (a == std::string::npos) { st.url = line; }
        else {
            st.name = gitmo_shared::TrimAsciiWhitespace(line.substr(0, a));
            size_t b = line.find('|', a + 1);
            st.url = gitmo_shared::TrimAsciiWhitespace(line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1));
            if (b != std::string::npos) st.genre = gitmo_shared::TrimAsciiWhitespace(line.substr(b + 1));
        }
        if (st.url.empty()) continue;
        if (st.name.empty()) st.name = st.url;
        g_stations.push_back(st);
    }
    g_stVersion++;
    Log("radio: %zu station%s in Gitmo-radio.ini, volume %d", g_stations.size(), g_stations.size() == 1 ? "" : "s", g_volume.load());
}

std::vector<RadioStation> Radio_Stations() { std::lock_guard<std::mutex> lk(g_stMx); return g_stations; }
unsigned Radio_StationsVersion() { return g_stVersion; }
bool Radio_SetStations(const std::vector<RadioStation>& list) {
    std::lock_guard<std::mutex> lk(g_stMx);
    std::vector<RadioStation> previous = g_stations;
    g_stations = list;
    bool saved = SaveStations();
    if (!saved) g_stations.swap(previous);
    g_stVersion++;
    return saved;
}
int Radio_Volume() { return g_volume; }
void Radio_SetVolume(int v, bool save) {
    g_volume = std::min(100, std::max(0, v));
    if (save) { std::lock_guard<std::mutex> lk(g_stMx); SaveStations(); }
}

// ---- playback state ---------------------------------------------------------------------
struct Session {
    unsigned gen = 0;
    RadioStation st;
    std::atomic<bool> stop{false};
    std::mutex httpMx;
    Http* activeHttp = nullptr;
    std::mutex mx;
    struct Chunk { int rate, ch; std::vector<int16_t> s; size_t pos = 0; };
    std::deque<Chunk> q;          // decoded audio waiting for the speakers
    uint64_t qDuration = 0;       // buffered audio duration in 100-ns units
    std::atomic<bool> readerDone{false};

    void CancelNetwork() {
        std::lock_guard<std::mutex> lk(httpMx);
        if (activeHttp) activeHttp->Cancel();
    }
};

struct HttpBinding {
    Session& session;
    HttpBinding(Session& s, Http& h) : session(s) {
        std::lock_guard<std::mutex> lk(session.httpMx);
        session.activeHttp = &h;
        if (session.stop) h.Cancel();
    }
    ~HttpBinding() {
        std::lock_guard<std::mutex> lk(session.httpMx);
        session.activeHttp = nullptr;
    }
};

struct MfContext {
    bool uninitialize = false;
    bool started = false;
    ~MfContext() {
        if (started) MFShutdown();
        if (uninitialize) CoUninitialize();
    }
    bool Start(std::string& err) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) uninitialize = true;
        else if (hr != RPC_E_CHANGED_MODE) {
            err = "Couldn't initialize the audio thread";
            Log("radio: CoInitializeEx failed (0x%08lX)", hr);
            return false;
        }
        hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            err = "Couldn't start Windows audio support";
            Log("radio: MFStartup failed (0x%08lX)", hr);
            return false;
        }
        started = true;
        return true;
    }
};

struct ReaderDone {
    std::shared_ptr<Session> session;
    ~ReaderDone() { session->readerDone = true; }
};

static std::mutex g_mx;            // guards everything below
static RadioState g_state;
static std::shared_ptr<Session> g_cur;
static unsigned g_gen = 0;

static void Set(const std::shared_ptr<Session>& ss, int state, const std::string& err = {}) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (ss->gen != g_gen) return;
    if (g_state.state != state || !err.empty()) Log("radio: %s%s%s", state == 1 ? "connecting" : state == 2 ? "buffering" : state == 3 ? "playing" : state == 4 ? "error: " : "stopped", err.c_str(), "");
    g_state.state = state;
    if (state == 4) g_state.err = err;
}

RadioState Radio_State() { std::lock_guard<std::mutex> lk(g_mx); RadioState s = g_state; s.volume = g_volume; return s; }

// ---- frames in the byte stream ------------------------------------------------------------
enum Codec { CNone, CMp3, CAac };
struct FrameInfo { int len = 0, rate = 0, ch = 0, kbps = 0, samples = 0; };

template<class T>
class MfOwner {
public:
    MfOwner() = default;
    MfOwner(const MfOwner&) = delete;
    MfOwner& operator=(const MfOwner&) = delete;
    ~MfOwner() { Reset(); }
    T** Put() { Reset(); return &p_; }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    void Attach(T* p) { Reset(); p_ = p; }
    T* Detach() { T* p = p_; p_ = nullptr; return p; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
private:
    T* p_ = nullptr;
};

static bool Mp3Header(const uint8_t* p, FrameInfo& f) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    int ver = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3, br = p[2] >> 4, sr = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if (ver == 1 || layer != 1 || br == 0 || br == 15 || sr == 3) return false;   // Layer III only
    static const int brV1[15] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
    static const int brV2[15] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
    static const int srT[3][3] = { { 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 } };
    f.kbps = ver == 3 ? brV1[br] : brV2[br];
    f.rate = ver == 3 ? (sr == 0 ? 44100 : sr == 1 ? 48000 : 32000) : srT[ver][sr];
    f.ch = (p[3] >> 6) == 3 ? 1 : 2;
    f.len = (ver == 3 ? 144000 : 72000) * f.kbps / f.rate + pad;
    f.samples = ver == 3 ? 1152 : 576;
    return f.len > 4;
}
static bool AdtsHeader(const uint8_t* p, FrameInfo& f) {
    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return false;
    static const int srT[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    int sr = (p[2] >> 2) & 0xF;
    if (sr > 12) return false;
    f.rate = srT[sr];
    f.ch = ((p[2] & 1) << 2) | (p[3] >> 6);
    if (f.ch == 0) f.ch = 2;
    f.len = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
    f.samples = 1024;
    f.kbps = 0;
    return f.len > 7;
}

// ---- decoder: Windows' MP3 / AAC decoder MFT ----------------------------------------------
class Decoder {
public:
    ~Decoder() { Close(); }
    bool Open(Codec c, const FrameInfo& f, std::string& err) {
        Close();
        codec_ = c;
        MFT_REGISTER_TYPE_INFO in = { MFMediaType_Audio, c == CMp3 ? MFAudioFormat_MP3 : MFAudioFormat_AAC };
        IMFActivate** acts = nullptr; UINT32 n = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr, &acts, &n);
        if (FAILED(hr) || (!acts && n)) {
            if (acts) for (UINT32 i = 0; i < n; i++) if (acts[i]) acts[i]->Release();
            CoTaskMemFree(acts);
            err = "Windows couldn't find an audio decoder";
            return false;
        }
        for (UINT32 i = 0; i < n; i++) {
            if (acts[i]) {
                if (!mft_) acts[i]->ActivateObject(IID_PPV_ARGS(&mft_));
                acts[i]->Release();
            }
        }
        CoTaskMemFree(acts);
        if (!mft_) { err = "Windows has no decoder for this station's format"; return false; }
        MfOwner<IMFMediaType> t;
        hr = MFCreateMediaType(t.Put());
        if (FAILED(hr) || !t.Get()) { err = "Couldn't prepare the audio decoder"; Close(); return false; }
        if (c == CMp3) {
            MPEGLAYER3WAVEFORMAT wf{};
            wf.wfx.wFormatTag = WAVE_FORMAT_MPEGLAYER3; wf.wfx.nChannels = (WORD)f.ch; wf.wfx.nSamplesPerSec = f.rate;
            wf.wfx.nAvgBytesPerSec = f.kbps * 125; wf.wfx.nBlockAlign = 1; wf.wfx.cbSize = MPEGLAYER3_WFX_EXTRA_BYTES;
            wf.wID = MPEGLAYER3_ID_MPEG; wf.fdwFlags = MPEGLAYER3_FLAG_PADDING_ISO; wf.nBlockSize = (WORD)f.len; wf.nFramesPerBlock = 1; wf.nCodecDelay = 1393;
            hr = MFInitMediaTypeFromWaveFormatEx(t.Get(), &wf.wfx, sizeof wf);
        } else {
            hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, f.ch);
            if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, f.rate);
            if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1);   // ADTS
            if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0xFE);
            const uint8_t ud[12] = { 1, 0, 0xFE, 0, 0, 0, 0, 0, 0, 0, 0, 0 };   // HEAACWAVEINFO after the WAVEFORMATEX: payload 1 (ADTS)
            if (SUCCEEDED(hr)) hr = t->SetBlob(MF_MT_USER_DATA, ud, sizeof ud);
        }
        if (SUCCEEDED(hr)) hr = mft_->SetInputType(0, t.Get(), 0);
        if (FAILED(hr)) { err = "The decoder didn't accept this stream"; Log("radio: decoder input type 0x%08lX (%s %d Hz %d ch)", hr, c == CMp3 ? "MP3" : "AAC", f.rate, f.ch); Close(); return false; }
        if (!PickOutput()) { err = "The decoder has no output format we can play"; Close(); return false; }
        hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (SUCCEEDED(hr)) hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) { err = "The audio decoder couldn't start"; Close(); return false; }
        return true;
    }
    void Close() { if (mft_) { mft_->Release(); mft_ = nullptr; } time_ = 0; }
    bool IsOpen() const { return mft_ != nullptr; }
    // one compressed frame in; any decoded audio appended to out (16-bit interleaved, at rate()/ch())
    bool Feed(const uint8_t* p, size_t n, int frameSamples, int frameRate, std::vector<int16_t>& out) {
        if (!p || !n || n > MAXDWORD) return false;
        MfOwner<IMFSample> smp;
        MfOwner<IMFMediaBuffer> buf;
        HRESULT hr = MFCreateSample(smp.Put());
        if (FAILED(hr) || !smp.Get()) return false;
        hr = MFCreateMemoryBuffer((DWORD)n, buf.Put());
        if (FAILED(hr) || !buf.Get()) return false;
        BYTE* d = nullptr;
        hr = buf->Lock(&d, nullptr, nullptr);
        if (FAILED(hr) || !d) return false;
        memcpy(d, p, n);
        HRESULT unlockHr = buf->Unlock();
        if (FAILED(unlockHr)) return false;
        hr = buf->SetCurrentLength((DWORD)n);
        if (SUCCEEDED(hr)) hr = smp->AddBuffer(buf.Get());
        if (FAILED(hr)) return false;
        LONGLONG dur = (LONGLONG)frameSamples * 10000000 / std::max(frameRate, 1);
        hr = smp->SetSampleTime(time_);
        if (SUCCEEDED(hr)) hr = smp->SetSampleDuration(dur);
        if (FAILED(hr)) return false;
        hr = mft_->ProcessInput(0, smp.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            if (!Drain(out)) return false;
            hr = mft_->ProcessInput(0, smp.Get(), 0);
        }
        if (FAILED(hr)) return false;
        time_ += dur;
        return Drain(out);
    }
    int rate() const { return rate_; }
    int ch() const { return ch_; }
private:
    IMFTransform* mft_ = nullptr;
    Codec codec_ = CNone;
    int rate_ = 44100, ch_ = 2, bits_ = 16; bool float_ = false;
    LONGLONG time_ = 0;

    bool PickOutput() {
        MfOwner<IMFMediaType> best; int bestScore = -1;
        for (DWORD i = 0;; i++) {
            MfOwner<IMFMediaType> t;
            if (FAILED(mft_->GetOutputAvailableType(0, i, t.Put())) || !t.Get()) break;
            GUID sub{};
            if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub))) continue;
            UINT32 bits = 0;
            if (FAILED(t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits))) continue;
            int score = sub == MFAudioFormat_PCM && bits == 16 ? 2 : sub == MFAudioFormat_Float && bits == 32 ? 1 : -1;
            if (score > bestScore) { best.Attach(t.Detach()); bestScore = score; }
        }
        if (!best.Get()) return false;
        HRESULT hr = mft_->SetOutputType(0, best.Get(), 0);
        GUID sub{};
        UINT32 r = 0, c = 0, b = 0;
        if (FAILED(hr) || FAILED(best->GetGUID(MF_MT_SUBTYPE, &sub)) ||
            FAILED(best->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r)) ||
            FAILED(best->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c)) ||
            FAILED(best->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &b)) || !r || !c) return false;
        rate_ = (int)r; ch_ = (int)c; bits_ = (int)b; float_ = sub == MFAudioFormat_Float;
        return true;
    }
    bool Drain(std::vector<int16_t>& out) {
        for (int guard = 0; guard < 64; guard++) {
            MFT_OUTPUT_STREAM_INFO si{};
            if (FAILED(mft_->GetOutputStreamInfo(0, &si))) return false;
            MfOwner<IMFSample> provided;
            bool provides = (si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            if (!provides) {
                MfOwner<IMFMediaBuffer> ob;
                if (FAILED(MFCreateSample(provided.Put())) || !provided.Get() ||
                    FAILED(MFCreateMemoryBuffer(std::max<DWORD>(si.cbSize, 1 << 16), ob.Put())) || !ob.Get() ||
                    FAILED(provided->AddBuffer(ob.Get()))) return false;
            }
            MFT_OUTPUT_DATA_BUFFER db{}; db.pSample = provided.Get(); DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &db, &status);
            if (db.pEvents) db.pEvents->Release();
            IMFSample* got = db.pSample;
            MfOwner<IMFSample> returned;
            if (got && got != provided.Get()) returned.Attach(got);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { if (!PickOutput()) return false; continue; }
            if (FAILED(hr)) return false;
            if (!got) continue;
            MfOwner<IMFMediaBuffer> cb;
            if (FAILED(got->ConvertToContiguousBuffer(cb.Put())) || !cb.Get()) return false;
            BYTE* d = nullptr; DWORD len = 0;
            hr = cb->Lock(&d, nullptr, &len);
            if (FAILED(hr) || (!d && len)) return false;
            if (d) {
                if (float_) {
                    const float* fl = (const float*)d; size_t k = len / 4;
                    size_t start = out.size();
                    out.resize(start + k);
                    for (size_t i = 0; i < k; i++) {
                        float v = std::isfinite(fl[i]) ? fl[i] * 32767.0f : 0.0f;
                        out[start + i] = (int16_t)std::min(32767.0f, std::max(-32768.0f, v));
                    }
                } else if (bits_ == 16) {
                    const int16_t* s = (const int16_t*)d;
                    out.insert(out.end(), s, s + len / 2);
                }
            }
            HRESULT unlockHr = cb->Unlock();
            if (FAILED(unlockHr)) return false;
            if (got == provided.Get()) {
                provided.Reset();
            }
        }
        return true;
    }
};

// ---- the reader: network -> frames -> PCM ------------------------------------------------
static bool IsPlaylistType(const std::string& ct, const std::string& url) {
    std::string u = Lower(url);
    size_t q = u.find('?'); if (q != std::string::npos) u = u.substr(0, q);
    auto ends = [&](const char* e) { size_t n = strlen(e); return u.size() >= n && u.compare(u.size() - n, n, e) == 0; };
    return ct.find("mpegurl") != std::string::npos || ct.find("scpls") != std::string::npos || ct.find("x-pls") != std::string::npos ||
           ends(".pls") || ends(".m3u") || ends(".m3u8");
}
// the first stream link in a .pls / .m3u playlist; "" if none ("hls" set for HLS playlists)
static std::string FromPlaylist(const std::string& body, bool& hls) {
    hls = body.find("#EXT-X-") != std::string::npos;
    if (hls) return {};
    size_t p = 0;
    while (p < body.size()) {
        size_t e = body.find('\n', p); if (e == std::string::npos) e = body.size();
        std::string line = gitmo_shared::TrimAsciiWhitespace(body.substr(p, e - p));
        p = e + 1;
        std::string l = Lower(line);
        size_t eq = l.find('=');
        if (l.rfind("file", 0) == 0 && eq != std::string::npos) line = gitmo_shared::TrimAsciiWhitespace(line.substr(eq + 1)), l = Lower(line);
        if (l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0) return line;
    }
    return {};
}

static void ReaderThreadBody(std::shared_ptr<Session> ss) {
    ReaderDone done{ ss };
    MfContext mf;
    std::string initErr;
    if (!mf.Start(initErr)) { Set(ss, 4, initErr); return; }
    Http http;
    HttpBinding binding(*ss, http);
    std::string err, url = ss->st.url;
    bool opened = false;
    for (int depth = 0; depth < 3 && !ss->stop; depth++) {
        if (!http.Open(url, true, err)) break;
        std::string ct = Lower(http.Header("content-type"));
        if (IsPlaylistType(ct, url) && ct.find("audio/mpeg") == std::string::npos) {
            std::string body;
            if (!http.ReadAll(body, 1 << 18)) { err = "The playlist was too large or couldn't be read"; break; }
            bool hls = false;
            std::string next = FromPlaylist(body, hls);
            if (hls) { err = "This station uses HLS streaming, which the phone can't play"; break; }
            if (next.empty()) { err = "The playlist has no stream in it"; break; }
            Log("radio: playlist -> %s", next.c_str());
            url = next;
            continue;
        }
        opened = true;
        break;
    }
    if (!opened) { if (!ss->stop) Set(ss, 4, err.empty() ? "Couldn't open the station" : err); return; }

    std::string ct = Lower(http.Header("content-type"));
    int metaint = std::max(0, ParseInt(http.Header("icy-metaint")));
    Codec codec = ct.find("aac") != std::string::npos ? CAac : (ct.find("mpeg") != std::string::npos || ct.find("mp3") != std::string::npos) ? CMp3 : CNone;
    if (ct.find("ogg") != std::string::npos || ct.find("opus") != std::string::npos || ct.find("flac") != std::string::npos || ct.find("mp4") != std::string::npos) {
        Set(ss, 4, "This station's format (" + ct + ") isn't supported; try its MP3 or AAC stream");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (ss->gen == g_gen) {
            g_state.icyName = http.Header("icy-name");
            if (g_state.icyName.empty()) g_state.icyName = http.Header("ice-name");
            g_state.kbps = std::max(0, ParseInt(http.Header("icy-br")));
            g_state.codec = codec == CAac ? "AAC" : codec == CMp3 ? "MP3" : "";
        }
    }
    Log("radio: connected: %s, metaint %d, name \"%s\"", ct.empty() ? "(no content type)" : ct.c_str(), metaint, http.Header("icy-name").c_str());
    Set(ss, 2);

    Decoder dec;
    std::string pend;           // audio bytes not yet cut into frames
    std::string meta; int metaLeft = -1;   // -1: not in a metadata block
    int untilMeta = metaint;
    bool synced = false; FrameInfo cur{};
    std::vector<int16_t> pcm;
    uint8_t buf[16384];
    size_t bad = 0;
    unsigned consecutiveDecodeFailures = 0;
    bool decodeFailureLogged = false;
    while (!ss->stop) {
        {   // don't race ahead of the speakers: keep about 4 s decoded
            std::unique_lock<std::mutex> lk(ss->mx);
            if (ss->qDuration > 40000000ull) { lk.unlock(); Sleep(30); continue; }
        }
        int k = http.Read(buf, sizeof buf);
        if (k <= 0) { if (!ss->stop) Set(ss, 4, k == 0 ? "The station stopped sending" : "Lost the connection to the station"); break; }
        // take the ICY metadata out
        int i = 0;
        while (i < k) {
            if (metaint <= 0) { pend.append((char*)buf + i, k - i); break; }
            if (metaLeft < 0 && untilMeta > 0) {
                int n = std::min(untilMeta, k - i);
                pend.append((char*)buf + i, n); i += n; untilMeta -= n;
                continue;
            }
            if (metaLeft < 0) { metaLeft = buf[i++] * 16; meta.clear(); if (metaLeft == 0) { metaLeft = -1; untilMeta = metaint; } continue; }
            int n = std::min(metaLeft, k - i);
            meta.append((char*)buf + i, n); i += n; metaLeft -= n;
            if (metaLeft == 0) {
                metaLeft = -1; untilMeta = metaint;
                size_t a = meta.find("StreamTitle='");
                if (a != std::string::npos) {
                    a += 13;
                    size_t b = meta.find("';", a);
                    std::string title = FixText(gitmo_shared::TrimAsciiWhitespace(meta.substr(a, b == std::string::npos ? std::string::npos : b - a)));
                    size_t t1 = title.find('~');   // some send "Title~Artist~~year~..."
                    if (t1 != std::string::npos && title.find(" - ") == std::string::npos) {
                        size_t t2 = title.find('~', t1 + 1);
                        std::string song = gitmo_shared::TrimAsciiWhitespace(title.substr(0, t1)), artist = gitmo_shared::TrimAsciiWhitespace(title.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1));
                        title = artist.empty() ? song : artist + " - " + song;
                    }
                    std::lock_guard<std::mutex> lk(g_mx);
                    if (ss->gen == g_gen && title != g_state.title) { g_state.title = title; Log("radio: now playing \"%s\"", title.c_str()); }
                }
            }
        }
        // decide the codec from the bytes if the server didn't say
        if (codec == CNone && pend.size() >= 4096) {
            for (size_t j = 0; j + 8 < pend.size() && codec == CNone; j++) {
                FrameInfo f, g; const uint8_t* p = (const uint8_t*)pend.data() + j;
                if (AdtsHeader(p, f) && j + f.len + 8 < pend.size() && AdtsHeader(p + f.len, g)) codec = CAac;
                else if (Mp3Header(p, f) && j + f.len + 8 < pend.size() && Mp3Header(p + f.len, g)) codec = CMp3;
            }
            if (codec == CNone) { Set(ss, 4, "This station's format isn't supported (MP3 and AAC streams work)"); break; }
            std::lock_guard<std::mutex> lk(g_mx);
            if (ss->gen == g_gen) g_state.codec = codec == CAac ? "AAC" : "MP3";
        }
        if (codec == CNone) continue;
        // cut frames and decode them
        size_t p = 0;
        auto header = [&](const uint8_t* q, FrameInfo& f) { return codec == CMp3 ? Mp3Header(q, f) : AdtsHeader(q, f); };
        while (p + 10 <= pend.size()) {
            const uint8_t* q = (const uint8_t*)pend.data() + p;
            FrameInfo f;
            if (!synced) {
                if (q[0] == 'I' && q[1] == 'D' && q[2] == '3' && p + 10 <= pend.size()) {   // an ID3 tag: skip it
                    size_t tl = 10 + ((q[6] & 0x7F) << 21 | (q[7] & 0x7F) << 14 | (q[8] & 0x7F) << 7 | (q[9] & 0x7F));
                    if (p + tl > pend.size()) break;
                    p += tl; continue;
                }
                FrameInfo g;
                if (!header(q, f)) { p++; bad++; continue; }
                if (p + f.len + 10 > pend.size()) break;   // need the next header to be sure
                if (!header(q + f.len, g) || g.rate != f.rate) { p++; bad++; continue; }
                synced = true;
                bad = 0;
            } else if (!header(q, f)) { synced = false; continue; }
            if (p + f.len > pend.size()) break;
            if (!dec.IsOpen() || f.rate != cur.rate || f.ch != cur.ch) {
                std::string derr;
                if (!dec.Open(codec, f, derr)) { Set(ss, 4, derr); ss->stop = true; break; }
                cur = f;
                Log("radio: decoding %s %d Hz, %d channel%s%s", codec == CMp3 ? "MP3" : "AAC", f.rate, f.ch, f.ch == 1 ? "" : "s",
                    f.kbps ? (", " + std::to_string(f.kbps) + " kbit/s").c_str() : "");
                std::lock_guard<std::mutex> lk(g_mx);
                if (ss->gen == g_gen && !g_state.kbps && f.kbps) g_state.kbps = f.kbps;
            }
            pcm.clear();
            if (!dec.Feed(q, f.len, f.samples, f.rate, pcm)) {
                if (!decodeFailureLogged) {
                    Log("radio: skipping audio frames rejected by the decoder");
                    decodeFailureLogged = true;
                }
                if (++consecutiveDecodeFailures >= 64) {
                    Set(ss, 4, "The audio decoder stopped accepting this station's stream");
                    ss->stop = true;
                    break;
                }
            } else {
                consecutiveDecodeFailures = 0;
            }
            p += f.len;
            if (!pcm.empty()) {
                std::lock_guard<std::mutex> lk(ss->mx);
                Session::Chunk c; c.rate = dec.rate(); c.ch = dec.ch(); c.s.swap(pcm);
                ss->qDuration += (uint64_t)(c.s.size() / c.ch) * 10000000ull / c.rate;
                ss->q.push_back(std::move(c));
            }
        }
        if (ss->stop) break;
        pend.erase(0, p);
        if (pend.size() > (1 << 20)) pend.clear();   // garbage with no frames in it
        if (!synced && bad > (1 << 20)) { Set(ss, 4, "Couldn't find any audio in the stream"); break; }
    }
    dec.Close();
}

static void ReaderThread(std::shared_ptr<Session> ss) {
    try {
        ReaderThreadBody(ss);
    } catch (const std::exception& e) {
        Log("radio: stream worker failed: %s", e.what());
        Set(ss, 4, "The stream couldn't be processed");
    }
}

// ---- the player: PCM -> waveOut ---------------------------------------------------------------
struct HandleGuard {
    HANDLE h = nullptr;
    ~HandleGuard() { if (h) CloseHandle(h); }
};

struct WaveOutGuard {
    HWAVEOUT& device;
    WAVEHDR* headers;
    int count;
    bool Close() {
        if (!device) return true;
        MMRESULT r = waveOutReset(device);
        if (r != MMSYSERR_NOERROR) {
            Log("radio: waveOutReset failed (%u)", r);
            return false;
        }
        for (int i = 0; i < count; i++) {
            if (headers[i].dwFlags & WHDR_PREPARED) {
                r = waveOutUnprepareHeader(device, &headers[i], sizeof(WAVEHDR));
                if (r != MMSYSERR_NOERROR) {
                    Log("radio: waveOutUnprepareHeader failed (%u)", r);
                    return false;
                }
            }
        }
        r = waveOutClose(device);
        if (r != MMSYSERR_NOERROR) {
            Log("radio: waveOutClose failed (%u)", r);
            return false;
        }
        device = nullptr;
        memset(headers, 0, count * sizeof(WAVEHDR));
        return true;
    }
    ~WaveOutGuard() { Close(); }
};

static void PlayerThreadBody(std::shared_ptr<Session> ss) {
    bool nullOut = gitmo_shared::IsEnvironmentVariableSet("GITMO_RADIO_NULLOUT");   // tests: decode, don't make a sound
    const int kBufs = 8;
    HWAVEOUT wo = nullptr;
    HandleGuard event{ nullOut ? nullptr : CreateEventW(nullptr, FALSE, FALSE, nullptr) };
    WAVEHDR hdr[kBufs]{};
    std::vector<int16_t> mem[kBufs];
    WaveOutGuard output{ wo, hdr, kBufs };
    if (!nullOut && !event.h) {
        Set(ss, 4, "Couldn't create the sound notification event");
        ss->stop = true;
        ss->CancelNetwork();
        return;
    }
    int rate = 0, ch = 0;
    bool started = false;
    size_t played = 0; double peak = 0;
    ULONGLONG lastLog = GetTickCount64(), lastBatt = 0;
    while (!ss->stop) {
        if (GetTickCount64() - lastBatt > 1000) {   // a flat phone can't play the radio
            lastBatt = GetTickCount64();
            if (Battery_Minutes() <= 0.0f) { Log("radio: the phone's battery is flat - radio off"); Radio_Stop(); break; }
            // The radio keeps playing in the main menu and through map changes: the phone works there too.
        }
        if (nullOut) Sleep(25);
        else if (WaitForSingleObject(event.h, 25) == WAIT_FAILED) {
            Set(ss, 4, "Couldn't wait for the sound device");
            ss->stop = true;
            ss->CancelNetwork();
            break;
        }
        std::unique_lock<std::mutex> lk(ss->mx);
        if (!started) {   // build up 1.5 s first, so short network hiccups aren't heard
            if (ss->qDuration < 15000000ull) {
                bool done = ss->readerDone;
                lk.unlock();
                if (done) break;
                continue;
            }
            started = true;
            lk.unlock();
            Set(ss, 3);
            continue;
        }
        if (ss->q.empty()) {   // ran dry
            bool busy = false;
            for (int i = 0; i < kBufs; i++) if (wo && (hdr[i].dwFlags & WHDR_PREPARED) && !(hdr[i].dwFlags & WHDR_DONE)) busy = true;
            bool done = ss->readerDone;
            lk.unlock();
            if (!busy && !nullOut) { if (done) break; started = false; Set(ss, 2); }
            if (nullOut && done) break;
            continue;
        }
        Session::Chunk& front = ss->q.front();
        if (nullOut) {   // tests: consume in real time without a device
            rate = front.rate; ch = front.ch;
            size_t want = (size_t)rate / 40 * ch;   // 25 ms
            size_t n = std::min(want, front.s.size() - front.pos);
            for (size_t i = 0; i < n; i++) peak = std::max(peak, (double)std::abs((int)front.s[front.pos + i]));
            front.pos += n; played += n / ch;
            uint64_t consumed = (uint64_t)(n / ch) * 10000000ull / rate;
            ss->qDuration -= std::min(ss->qDuration, consumed);
            if (front.pos >= front.s.size()) ss->q.pop_front();
            if (ss->q.empty()) ss->qDuration = 0;
            lk.unlock();
            if (GetTickCount64() - lastLog > 5000) { lastLog = GetTickCount64(); Log("radio test: %.1f s played (%d Hz %d ch), peak level %.0f", played / (double)rate, rate, ch, peak); peak = 0; }
            continue;
        }
        if (!wo || front.rate != rate || front.ch != ch) {
            bool busy = false;
            for (int i = 0; i < kBufs; i++) if (wo && (hdr[i].dwFlags & WHDR_PREPARED) && !(hdr[i].dwFlags & WHDR_DONE)) busy = true;
            if (busy) continue;   // let the old format finish first
            lk.unlock();
            if (!output.Close()) { Set(ss, 4, "Couldn't close the previous sound format"); ss->stop = true; ss->CancelNetwork(); break; }
            rate = front.rate; ch = front.ch;
            WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = (WORD)ch; wf.nSamplesPerSec = rate; wf.wBitsPerSample = 16;
            wf.nBlockAlign = (WORD)(ch * 2); wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
            MMRESULT r = waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)event.h, 0, CALLBACK_EVENT);
            if (r != MMSYSERR_NOERROR) {
                wo = nullptr;
                Set(ss, 4, "Couldn't open the sound device (" + std::to_string(r) + ")");
                ss->stop = true;
                ss->CancelNetwork();
                break;
            }
            Log("radio: sound out %d Hz, %d channel%s", rate, ch, ch == 1 ? "" : "s");
            continue;
        }
        // fill every free buffer (50 ms each), volume applied on the way
        float v = g_volume / 100.0f, gain = v * v;
        size_t per = (size_t)rate / 20 * ch;
        for (int i = 0; i < kBufs && !ss->q.empty(); i++) {
            if ((hdr[i].dwFlags & WHDR_PREPARED) && !(hdr[i].dwFlags & WHDR_DONE)) continue;
            if (hdr[i].dwFlags & WHDR_PREPARED) {
                MMRESULT r = waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
                if (r != MMSYSERR_NOERROR) { Set(ss, 4, "Couldn't recycle a sound buffer"); ss->stop = true; ss->CancelNetwork(); break; }
            }
            mem[i].clear();
            if (mem[i].capacity() < per) mem[i].reserve(per);
            while (mem[i].size() < per && !ss->q.empty()) {
                Session::Chunk& c = ss->q.front();
                if (c.rate != rate || c.ch != ch) break;
                size_t n = std::min(per - mem[i].size(), c.s.size() - c.pos);
                size_t start = mem[i].size();
                mem[i].resize(start + n);
                for (size_t j = 0; j < n; j++) mem[i][start + j] = (int16_t)(c.s[c.pos + j] * gain);
                c.pos += n;
                uint64_t consumed = (uint64_t)(n / ch) * 10000000ull / rate;
                ss->qDuration -= std::min(ss->qDuration, consumed);
                if (c.pos >= c.s.size()) ss->q.pop_front();
                if (ss->q.empty()) ss->qDuration = 0;
            }
            if (mem[i].empty()) break;
            memset(&hdr[i], 0, sizeof(WAVEHDR));
            hdr[i].lpData = (LPSTR)mem[i].data(); hdr[i].dwBufferLength = (DWORD)(mem[i].size() * 2);
            MMRESULT r = waveOutPrepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
            if (r != MMSYSERR_NOERROR) { Set(ss, 4, "Couldn't prepare a sound buffer"); ss->stop = true; ss->CancelNetwork(); break; }
            r = waveOutWrite(wo, &hdr[i], sizeof(WAVEHDR));
            if (r != MMSYSERR_NOERROR) { Set(ss, 4, "Couldn't play a sound buffer"); ss->stop = true; ss->CancelNetwork(); break; }
        }
    }
    output.Close();
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (ss->gen == g_gen && g_state.state != 4) g_state.state = 0;
    }
}

static void PlayerThread(std::shared_ptr<Session> ss) {
    try {
        PlayerThreadBody(ss);
    } catch (const std::exception& e) {
        Log("radio: player worker failed: %s", e.what());
        ss->stop = true;
        ss->CancelNetwork();
        Set(ss, 4, "The sound couldn't be played");
    }
}

// ---- public -------------------------------------------------------------------------------------
void Radio_Start() { LoadStations(); }

void Radio_Play(const RadioStation& st) {
    std::shared_ptr<Session> ss;
    try {
        ss = std::make_shared<Session>();
        ss->st = st;
    } catch (const std::exception& e) {
        Log("radio: couldn't prepare playback: %s", e.what());
        return;
    }
    std::shared_ptr<Session> previous;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        previous = g_cur;
        if (previous) previous->stop = true;
        ss->gen = ++g_gen;
        g_cur = ss;
        g_state = RadioState();
        g_state.state = 1;
        g_state.url = st.url; g_state.name = st.name; g_state.genre = st.genre;
    }
    if (previous) previous->CancelNetwork();
    Log("radio: play \"%s\" %s", st.name.c_str(), st.url.c_str());
    std::thread reader;
    try {
        reader = std::thread(ReaderThread, ss);
        reader.detach();
    } catch (const std::exception& e) {
        if (reader.joinable()) reader.detach();
        ss->stop = true;
        ss->readerDone = true;
        Log("radio: couldn't start the stream worker: %s", e.what());
        Set(ss, 4, "Couldn't start the radio stream");
        return;
    }
    std::thread player;
    try {
        player = std::thread(PlayerThread, ss);
        player.detach();
    } catch (const std::exception& e) {
        if (player.joinable()) player.detach();
        ss->stop = true;
        ss->CancelNetwork();
        Log("radio: couldn't start the sound worker: %s", e.what());
        Set(ss, 4, "Couldn't start the sound player");
    }
}

void Radio_Stop() {
    std::shared_ptr<Session> current;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        current = g_cur;
        if (current) { current->stop = true; g_cur.reset(); }
        g_gen++;
        std::string url = g_state.url, name = g_state.name, genre = g_state.genre;
        g_state = RadioState();
        g_state.url = url; g_state.name = name; g_state.genre = genre;   // (Now Playing keeps showing what it was)
    }
    if (current) { current->CancelNetwork(); Log("radio: stop"); }
}

// ---- Browse: the radio-browser.info directory ---------------------------------------------------
// A tiny JSON reader, enough for radio-browser's flat array of station objects.
namespace {
struct J {
    const char* p; const char* e;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool str(std::string& out) {
        ws();
        return gitmo_shared::ReadJsonString(p, e, out);
    }
    // any value, as text (strings unquoted; objects/arrays skipped and returned empty)
    bool val(std::string& out) {
        ws(); if (p >= e) return false;
        if (*p == '"') return str(out);
        if (*p == '{' || *p == '[') {
            char closes[64];
            int depth = 0;
            closes[depth++] = *p++ == '{' ? '}' : ']';
            while (p < e && depth > 0) {
                if (*p == '"') {
                    std::string ignored;
                    if (!str(ignored)) return false;
                } else if (*p == '{' || *p == '[') {
                    if (depth == (int)sizeof(closes)) return false;
                    closes[depth++] = *p++ == '{' ? '}' : ']';
                } else if (*p == '}' || *p == ']') {
                    if (*p++ != closes[--depth]) return false;
                } else {
                    p++;
                }
            }
            if (depth) return false;
            out.clear(); return true;
        }
        const char* s = p;
        while (p < e && *p != ',' && *p != '}' && *p != ']' &&
               *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
        if (p == s) return false;
        out.assign(s, p);
        return true;
    }
};
}

static bool ParseStations(const std::string& body, std::vector<RadioFound>& out) {
    std::vector<RadioFound> parsed;
    J j{ body.data(), body.data() + body.size() };
    j.ws(); if (j.p >= j.e || *j.p != '[') return false;
    j.p++;
    j.ws();
    if (j.p < j.e && *j.p == ']') {
        j.p++; j.ws();
        if (j.p != j.e) return false;
        out.clear();
        return true;
    }
    size_t entries = 0;
    for (;;) {
        j.ws(); if (j.p >= j.e || *j.p != '{') return false;
        j.p++;
        std::map<std::string, std::string> o;
        for (;;) {
            j.ws(); if (j.p >= j.e) return false;
            if (*j.p == '}') { j.p++; break; }
            std::string k, v;
            if (!j.str(k)) return false;
            j.ws(); if (j.p >= j.e || *j.p != ':') return false;
            j.p++;
            if (!j.val(v)) return false;
            o[k] = v;
            j.ws();
            if (j.p >= j.e) return false;
            if (*j.p == '}') { j.p++; break; }
            if (*j.p != ',') return false;
            j.p++;
        }
        RadioFound f;
        f.name = gitmo_shared::TrimAsciiWhitespace(o["name"]);
        f.url = gitmo_shared::TrimAsciiWhitespace(o["url_resolved"]);
        if (f.url.empty()) f.url = gitmo_shared::TrimAsciiWhitespace(o["url"]);
        f.uuid = o["stationuuid"]; f.country = o["countrycode"]; f.codec = o["codec"];
        int bitrate = ParseInt(o["bitrate"], -1);
        f.kbps = bitrate >= 0 && bitrate <= 10000 ? bitrate : 0;
        std::string tags = o["tags"];
        size_t c = tags.find(',');
        f.genre = gitmo_shared::TrimAsciiWhitespace(tags.substr(0, c));
        if (!f.genre.empty()) f.genre[0] = (char)toupper((unsigned char)f.genre[0]);
        std::string cd = Lower(f.codec);
        std::string hls = Lower(o["hls"]);
        bool playable = (cd.find("mp3") != std::string::npos || cd.find("aac") != std::string::npos) && hls != "1" && hls != "true";
        if (!f.name.empty() && !f.url.empty() && playable) parsed.push_back(std::move(f));
        if (++entries > 1000) return false;
        j.ws();
        if (j.p >= j.e) return false;
        if (*j.p == ']') { j.p++; break; }
        if (*j.p != ',') return false;
        j.p++;
    }
    j.ws();
    if (j.p != j.e) return false;
    out.swap(parsed);
    return true;
}

static std::string UrlEncode(const std::string& s) {
    std::string o; char b[4];
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { sprintf_s(b, "%%%02X", c); o += b; }
    }
    return o;
}

static const char* kServers[] = { "de1.api.radio-browser.info", "de2.api.radio-browser.info", "fi1.api.radio-browser.info" };
static std::atomic<int> g_server{0};   // the one that answered last

static bool ApiGet(const std::string& path, std::string& body, std::string& err) {
    for (int i = 0; i < 3; i++) {
        int sv = (g_server + i) % 3;
        Http h;
        body.clear();
        err.clear();
        if (h.Open(std::string("https://") + kServers[sv] + path, false, err)) {
            if (h.ReadAll(body, 4 << 20)) { g_server = sv; return true; }
            err = "The station directory response was too large or incomplete";
        }
        Log("radio: %s didn't answer (%s)", kServers[sv], err.c_str());
    }
    if (err.empty()) err = "The station directory didn't answer";
    return false;
}

static std::mutex g_srMx;
static int g_srState = 0;              // 0 nothing yet, 1 searching, 2 done, 3 failed
static unsigned g_srGen = 0;
static unsigned g_srVersion = 1;
static std::string g_srErr, g_srQuery;
static std::vector<RadioFound> g_srFound;

static void SearchThread(unsigned gen, std::string query) {
    try {
        std::vector<RadioFound> found; std::string err, body;
        std::string q = gitmo_shared::TrimAsciiWhitespace(query);
        bool ok;
        const std::string tail = "&limit=60&hidebroken=true&order=clickcount&reverse=true";
        if (q.empty()) ok = ApiGet("/json/stations/topclick/80?hidebroken=true", body, err) && ParseStations(body, found);
        else {
            ok = ApiGet("/json/stations/search?name=" + UrlEncode(q) + tail, body, err) && ParseStations(body, found);
            if (ok && found.size() < 15) {   // few by name: add stations with it as a genre tag
                std::vector<RadioFound> more;
                std::set<std::string> urls;
                for (const auto& f : found) urls.insert(f.url);
                if (ApiGet("/json/stations/search?tag=" + UrlEncode(Lower(q)) + tail, body, err) && ParseStations(body, more))
                    for (auto& m : more) {
                        if (found.size() >= 60) break;
                        if (urls.insert(m.url).second) found.push_back(std::move(m));
                    }
            }
        }
        if (!ok && err.empty()) err = "The station directory's answer wasn't understood";
        std::lock_guard<std::mutex> lk(g_srMx);
        if (gen != g_srGen) return;
        g_srState = ok ? 2 : 3; g_srErr = err; g_srFound.swap(found);
        ++g_srVersion;
        Log("radio: search \"%s\": %s, %zu playable", q.c_str(), ok ? "ok" : err.c_str(), g_srFound.size());
    } catch (const std::exception& e) {
        Log("radio: station search failed: %s", e.what());
        std::lock_guard<std::mutex> lk(g_srMx);
        if (gen == g_srGen) {
            g_srState = 3;
            g_srErr = "The station search couldn't be completed";
            g_srFound.clear();
            ++g_srVersion;
        }
    }
}

void Radio_Search(const std::string& query) {
    unsigned gen;
    {
        std::lock_guard<std::mutex> lk(g_srMx);
        gen = ++g_srGen; g_srState = 1; g_srErr.clear(); g_srQuery = query; g_srFound.clear();
        ++g_srVersion;
    }
    std::thread worker;
    try {
        worker = std::thread(SearchThread, gen, query);
        worker.detach();
    } catch (const std::exception& e) {
        if (worker.joinable()) worker.detach();
        Log("radio: couldn't start station search: %s", e.what());
        std::lock_guard<std::mutex> lk(g_srMx);
        if (gen == g_srGen) {
            g_srState = 3;
            g_srErr = "Couldn't start the station search";
            ++g_srVersion;
        }
    }
}

int Radio_SearchResult(unsigned& version, std::vector<RadioFound>& out, std::string& err, std::string& query) {
    std::lock_guard<std::mutex> lk(g_srMx);
    if (version != g_srVersion) {
        out = g_srFound; err = g_srErr; query = g_srQuery;
        version = g_srVersion;
    }
    return g_srState;
}

void Radio_CountClick(const std::string& uuid) {   // radio-browser asks apps to report plays (it ranks stations by them)
    if (uuid.empty()) return;
    try {
        std::thread worker([uuid] {
            try {
                std::string body, err;
                ApiGet("/json/url/" + UrlEncode(uuid), body, err);
            } catch (const std::exception& e) {
                Log("radio: couldn't report station play: %s", e.what());
            }
        });
        worker.detach();
    } catch (const std::exception& e) {
        Log("radio: couldn't start station play report: %s", e.what());
    }
}
