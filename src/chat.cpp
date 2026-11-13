#include "chat.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <algorithm>

namespace sms {

const char* const kPhotoText = "\x1b" "photo";   // can't be typed: CleanText drops control characters

const char* StText(St s) {
    switch (s) { case St::Waiting: return "Waiting"; case St::Delivered: return "Delivered"; case St::Read: return "Read"; default: return "Received"; }
}

void Engine::Logf(const char* fmt, ...) const {
    if (!io_.log) return;
    char b[512]; va_list v; va_start(v, fmt); vsnprintf(b, sizeof b, fmt, v); va_end(v);
    io_.log(b);
}

// One line of valid UTF-8, no control characters, at most kMaxText bytes (never cut inside a character).
std::string Engine::CleanText(const std::string& s) {
    std::string o;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > s.size()) { i++; continue; }   // stray byte
        bool ok = true;
        for (int k = 1; k < n; k++) if (((unsigned char)s[i + k] >> 6) != 2) ok = false;
        if (!ok) { i++; continue; }
        if (n == 1 && c < 32) { if (!o.empty() && o.back() != ' ') o += ' '; i++; continue; }   // newline, tab... -> space
        if (o.size() + n > kMaxText) break;
        o.append(s, i, n);
        i += n;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    size_t f = o.find_first_not_of(' ');
    return f == std::string::npos ? std::string() : o.substr(f);
}

Contact& Engine::Get(uint64_t peer) {
    Contact& c = contacts_[peer];
    c.id = peer;
    return c;
}

int Engine::Unread() const {
    int n = 0;
    for (auto& kv : contacts_) n += kv.second.unread;
    return n;
}

void Engine::SetPresence(uint64_t peer, const std::string& name, bool online, bool inGame) {
    if (!peer) return;
    bool isNew = !contacts_.count(peer);
    Contact& c = Get(peer);
    bool changed = isNew;
    if (!name.empty() && c.name != name) { c.name = name; changed = true; }
    bool wasOnline = c.online;
    c.online = online; c.inGame = online && inGame;
    if (online) {
        long long w = io_.wallclock ? io_.wallclock() : 0;
        if (w - c.lastSeenWall > 60) { c.lastSeenWall = w; changed = true; }   // saved at most once a minute
    }
    if (online && !wasOnline) {   // came online: send what's waiting straight away
        for (auto& m : c.msgs) if (m.mine && m.st == St::Waiting) m.lastSend = 0;
    }
    if (changed) Changed();
}

void Engine::Transmit(Contact& c, Msg& m) {
    char head[96];
    if (m.photo) {   // every piece; the receiver keeps what it already has
        std::string jpeg = io_.loadPhoto ? io_.loadPhoto(m.id) : std::string();
        if (jpeg.empty()) { Logf("sms %llx: the photo file is gone - it can't be sent", (unsigned long long)m.id); m.st = St::Delivered; Changed(); return; }
        for (size_t off = 0; off < jpeg.size(); off += kChunk) {
            snprintf(head, sizeof head, "C1|PCH|%llx|%llu|%zu|%zu|", (unsigned long long)m.id, (unsigned long long)m.key, off, jpeg.size());
            if (io_.send) io_.send(c.id, head + jpeg.substr(off, kChunk));
        }
    } else {
        snprintf(head, sizeof head, "C1|MSG|%llx|%llu|", (unsigned long long)m.id, (unsigned long long)m.key);
        if (io_.send) io_.send(c.id, head + m.text);
    }
    m.lastSend = io_.now ? io_.now() : 0;
    if (!m.lastSend) m.lastSend = 1;
}

void Engine::Trim(Contact& c) {
    if (c.msgs.size() <= kKeepPerContact) return;
    size_t drop = c.msgs.size() - kKeepPerContact;
    for (size_t i = 0; i < drop; i++) DropPhoto(c.msgs[i]);
    c.msgs.erase(c.msgs.begin(), c.msgs.begin() + drop);
}

bool Engine::SendPhoto(uint64_t peer, const std::string& jpeg) {
    if (!peer || jpeg.empty() || jpeg.size() > kMaxPhoto) return false;
    Msg m;
    m.id = io_.newId ? io_.newId() : 1;
    if (!io_.storePhoto || !io_.storePhoto(m.id, jpeg)) { Logf("sms: couldn't keep the photo to send"); return false; }
    Contact& c = Get(peer);
    m.mine = true;
    m.photo = true;
    m.text = kPhotoText;
    m.wall = io_.wallclock ? io_.wallclock() : 0;
    m.key = (uint64_t)m.wall * 1000 + (counter_++ % 1000);
    m.st = St::Waiting;
    c.msgs.push_back(m);
    Trim(c);
    if (c.online) Transmit(c, c.msgs.back());
    Logf("sms %llx: photo to %s (%zu KB)%s", (unsigned long long)m.id, c.name.c_str(), jpeg.size() / 1024, c.online ? "" : ", waiting until they're online");
    Changed();
    return true;
}

int Engine::Receiving(uint64_t peer) const {
    int n = 0;
    for (auto& kv : partial_) if (kv.second.from == peer) n++;
    return n;
}

// one of theirs: kept in the order they wrote them, even if messages arrive out of order
void Engine::AddTheirs(Contact& c, uint64_t from, const Msg& m, const std::string& ringText) {
    auto at = c.msgs.end();
    for (auto it = c.msgs.begin(); it != c.msgs.end(); ++it) if (!it->mine && it->key > m.key) { at = it; break; }
    c.msgs.insert(at, m);
    Trim(c);
    if (viewing_ == from) MarkRead(from);
    else {
        c.unread++;
        if (io_.ring) io_.ring(from, c.name, ringText);
    }
    Logf("sms %llx: %s from %s", (unsigned long long)m.id, m.photo ? "photo" : "message", c.name.c_str());
    Changed();
}

bool Engine::Send(uint64_t peer, const std::string& text) {
    std::string t = CleanText(text);
    if (t.empty() || !peer) return false;
    Contact& c = Get(peer);
    Msg m;
    m.id = io_.newId ? io_.newId() : 1;
    m.mine = true;
    m.wall = io_.wallclock ? io_.wallclock() : 0;
    m.key = (uint64_t)m.wall * 1000 + (counter_++ % 1000);
    m.text = t;
    m.st = St::Waiting;
    c.msgs.push_back(m);
    Trim(c);
    if (c.online) Transmit(c, c.msgs.back());
    Logf("sms %llx: to %s (%zu bytes)%s", (unsigned long long)m.id, c.name.c_str(), t.size(), c.online ? "" : ", waiting until they're online");
    Changed();
    return true;
}

void Engine::SendRead(Contact& c) {
    if (!c.readId || !c.online) return;
    char b[64];
    snprintf(b, sizeof b, "C1|READ|%llx|0|", (unsigned long long)c.readId);
    if (io_.send) io_.send(c.id, b);
    c.readPending = false;
}

void Engine::MarkRead(uint64_t peer) {
    auto it = contacts_.find(peer);
    if (it == contacts_.end()) return;
    Contact& c = it->second;
    uint64_t last = 0;
    for (auto& m : c.msgs) if (!m.mine) last = m.id;
    bool changed = c.unread != 0;
    c.unread = 0;
    if (last && last != c.readId) { c.readId = last; c.readPending = true; changed = true; SendRead(c); }
    if (changed) Changed();
}

void Engine::Forget(uint64_t peer) {
    auto it = contacts_.find(peer);
    if (it == contacts_.end()) return;
    for (auto& m : it->second.msgs) DropPhoto(m);
    contacts_.erase(it);
    Changed();
}

void Engine::EraseAll() {
    size_t n = 0;
    for (auto it = contacts_.begin(); it != contacts_.end();) {
        Contact& c = it->second;
        for (auto& m : c.msgs) { if (!m.mine) erased_.insert(m.id); DropPhoto(m); }
        n += c.msgs.size();
        if (!c.online) { it = contacts_.erase(it); continue; }   // previous contacts go; who's online now stays listed
        c.msgs.clear();
        c.unread = 0;
        c.readId = 0;
        c.readPending = false;
        ++it;
    }
    partial_.clear();
    Logf("sms: erased the message history (%zu messages)", n);
    Changed();
}

void Engine::OnMessage(uint64_t from, const std::string& msg) {
    if (!from) return;
    // C1|TYPE|idhex|number|text
    size_t p1 = msg.find('|'), p2 = p1 == std::string::npos ? p1 : msg.find('|', p1 + 1);
    size_t p3 = p2 == std::string::npos ? p2 : msg.find('|', p2 + 1);
    size_t p4 = p3 == std::string::npos ? p3 : msg.find('|', p3 + 1);
    if (p4 == std::string::npos || msg.compare(0, p1, "C1") != 0) { Logf("sms: ignored a malformed message"); return; }
    std::string type = msg.substr(p1 + 1, p2 - p1 - 1);
    uint64_t id = strtoull(msg.substr(p2 + 1, p3 - p2 - 1).c_str(), nullptr, 16);
    long long num = _atoi64(msg.substr(p3 + 1, p4 - p3 - 1).c_str());
    if (!id) return;
    Contact& c = Get(from);
    c.online = true;   // we just heard from them

    if (type == "MSG") {
        char ack[64];
        snprintf(ack, sizeof ack, "C1|ACK|%llx|0|", (unsigned long long)id);
        if (io_.send) io_.send(from, ack);   // always: they repeat until they hear it
        for (auto& m : c.msgs) if (!m.mine && m.id == id) return;   // a repeat
        if (erased_.count(id)) return;                                // a repeat of one the player erased
        std::string text = CleanText(msg.substr(p4 + 1));
        if (text.empty()) return;
        Msg m; m.id = id; m.mine = false; m.key = (uint64_t)num; m.wall = num / 1000; m.text = text; m.st = St::Received;
        AddTheirs(c, from, m, text);
        return;
    }
    if (type == "PCH") {   // a piece of a photo: <offset>|<total>|<bytes>
        char ack[64];
        snprintf(ack, sizeof ack, "C1|ACK|%llx|0|", (unsigned long long)id);
        for (auto& m : c.msgs) if (!m.mine && m.id == id) { if (io_.send) io_.send(from, ack); return; }   // already have it all
        if (erased_.count(id)) { if (io_.send) io_.send(from, ack); return; }
        size_t q1 = msg.find('|', p4 + 1), q2 = q1 == std::string::npos ? q1 : msg.find('|', q1 + 1);
        if (q2 == std::string::npos) return;
        size_t off = (size_t)_strtoui64(msg.substr(p4 + 1, q1 - p4 - 1).c_str(), nullptr, 10);
        size_t total = (size_t)_strtoui64(msg.substr(q1 + 1, q2 - q1 - 1).c_str(), nullptr, 10);
        size_t len = msg.size() - (q2 + 1);
        if (!total || total > kMaxPhoto || off % kChunk || off >= total || len != std::min(kChunk, total - off)) { Logf("sms: ignored a bad photo piece"); return; }
        Partial& pa = partial_[id];
        if (pa.data.empty()) {
            if (partial_.size() > 16) { partial_.erase(id); return; }   // (something is flooding us)
            pa.from = from; pa.key = (uint64_t)num; pa.data.assign(total, '\0'); pa.got.assign((total + kChunk - 1) / kChunk, false);
        }
        if (pa.from != from || pa.data.size() != total) return;
        size_t idx = off / kChunk;
        if (!pa.got[idx]) { memcpy(&pa.data[off], msg.data() + q2 + 1, len); pa.got[idx] = true; pa.have++; }
        pa.last = io_.now ? io_.now() : 1;
        if (pa.have < pa.got.size()) return;
        std::string jpeg; jpeg.swap(pa.data);
        uint64_t key = pa.key;
        partial_.erase(id);
        if (!io_.storePhoto || !io_.storePhoto(id, jpeg)) { Logf("sms %llx: couldn't keep a photo that came in", (unsigned long long)id); return; }
        if (io_.send) io_.send(from, ack);
        Msg m; m.id = id; m.mine = false; m.key = key; m.wall = (long long)(key / 1000); m.text = kPhotoText; m.photo = true; m.st = St::Received;
        AddTheirs(c, from, m, "\xF0\x9F\x93\xB7 Photo");
        return;
    }
    if (type == "ACK") {
        for (auto& m : c.msgs)
            if (m.mine && m.id == id && m.st == St::Waiting) { m.st = St::Delivered; Changed(); }
        return;
    }
    if (type == "READ") {
        int upto = -1;
        for (int i = 0; i < (int)c.msgs.size(); i++) if (c.msgs[i].mine && c.msgs[i].id == id) upto = i;
        if (upto < 0) return;
        bool changed = false;
        for (int i = 0; i <= upto; i++)
            if (c.msgs[i].mine && c.msgs[i].st != St::Read) { c.msgs[i].st = St::Read; changed = true; }
        if (changed) Changed();
        return;
    }
}

void Engine::Tick() {
    uint64_t now = io_.now ? io_.now() : 0;
    for (auto it = partial_.begin(); it != partial_.end();)   // half-received photos that stopped coming
        if (now - it->second.last > kPartialExpire) it = partial_.erase(it); else ++it;
    for (auto& kv : contacts_) {
        Contact& c = kv.second;
        if (!c.online) continue;
        int sent = 0, photos = 0;
        for (auto& m : c.msgs) {
            if (!m.mine || m.st != St::Waiting || sent >= 10) continue;
            uint64_t every = m.photo ? kPhotoResend : kResend;
            if (m.lastSend != 0 && now - m.lastSend < every) continue;
            if (m.photo && photos++ >= 1) continue;   // one photo per contact per tick
            Transmit(c, m); sent++;
        }
        if (c.readPending) SendRead(c);
    }
}

// ---- saved state: Gitmo-sms.txt ------------------------------------------------------------
//   C <tab> id <tab> lastSeen <tab> unread <tab> readId(hex) <tab> name
//   M <tab> peer <tab> id(hex) <tab> mine <tab> wall <tab> state <tab> key <tab> text
static std::string Esc(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '\\') o += "\\\\"; else if (ch == '\t') o += "\\t"; else if (ch == '\n') o += "\\n"; else if (ch == '\r') o += "\\r"; else o += ch;
    }
    return o;
}
static std::string Unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            o += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else o += s[i];
    }
    return o;
}

std::string Engine::Save() const {
    std::ostringstream o;
    o << "# Gitmo SMS history\n";
    char b[160];
    for (auto& kv : contacts_) {
        const Contact& c = kv.second;
        snprintf(b, sizeof b, "C\t%llu\t%lld\t%d\t%llx\t", (unsigned long long)c.id, c.lastSeenWall, c.unread, (unsigned long long)c.readId);
        o << b << Esc(c.name) << "\n";
        for (auto& m : c.msgs) {
            snprintf(b, sizeof b, "M\t%llu\t%llx\t%d\t%lld\t%d\t%llu\t", (unsigned long long)c.id, (unsigned long long)m.id, m.mine ? 1 : 0, m.wall, (int)m.st, (unsigned long long)m.key);
            o << b << Esc(m.text) << "\n";
        }
    }
    return o.str();
}

void Engine::Load(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    int nc = 0, nm = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 3 || line[1] != '\t') continue;
        std::vector<std::string> f;
        size_t a = 0;
        for (;;) {
            size_t b = line.find('\t', a);
            f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (f[0] == "C" && f.size() >= 6) {
            Contact& c = Get(_strtoui64(f[1].c_str(), nullptr, 10));
            c.lastSeenWall = _atoi64(f[2].c_str());
            c.unread = atoi(f[3].c_str());
            c.readId = _strtoui64(f[4].c_str(), nullptr, 16);
            c.name = Unesc(f[5]);
            nc++;
        } else if (f[0] == "M" && f.size() >= 8) {
            uint64_t peer = _strtoui64(f[1].c_str(), nullptr, 10);
            if (!peer) continue;
            Msg m;
            m.id = _strtoui64(f[2].c_str(), nullptr, 16);
            m.mine = f[3] == "1";
            m.wall = _atoi64(f[4].c_str());
            int st = atoi(f[5].c_str());
            m.st = st >= 0 && st <= 3 ? (St)st : (m.mine ? St::Waiting : St::Received);
            m.key = _strtoui64(f[6].c_str(), nullptr, 10);
            m.text = Unesc(f[7]);
            m.photo = m.text == kPhotoText;
            if (m.id) { Get(peer).msgs.push_back(m); nm++; }
        }
    }
    Logf("sms: loaded %d contact(s), %d message(s)", nc, nm);
}

}   // namespace sms
