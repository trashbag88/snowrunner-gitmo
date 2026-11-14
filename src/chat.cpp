#include "chat.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <sstream>
#include <algorithm>
#include <string>
#include <vector>
#include <set>

namespace sms {

const char* const kPhotoText = "\x1b" "photo";   // can't be typed: CleanText drops control characters

namespace {
const size_t kMaxContacts = 500;     // unknown senders can't grow the contact list without limit
const size_t kMaxErased = 4096;      // remembered ids of messages the player erased / that were trimmed
const size_t kMaxPartials = 16;      // half-received photos, all peers together
const int kMaxPartialsPerPeer = 4;   // ...and per peer, so one peer can't starve the rest
inline unsigned long long LL(uint64_t v) { return (unsigned long long)v; }

// control message: C1|TYPE|idhex|0|
void SendCtl(const IO& io, uint64_t peer, const char* type, uint64_t id) {
    char b[64];
    snprintf(b, sizeof b, "C1|%s|%llx|0|", type, LL(id));
    if (io.send) io.send(peer, b);
}

template <class S>
void Remember(S& set, uint64_t id) {
    set.insert(id);
    if (set.size() > kMaxErased) set.erase(set.begin());
}

bool HaveTheirs(const Contact& c, uint64_t id) {
    for (const auto& m : c.msgs) if (!m.mine && m.id == id) return true;
    return false;
}

bool LooksLikeJpeg(const std::string& d) {
    return d.size() >= 4 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8 && (unsigned char)d[2] == 0xFF;
}
}   // namespace

const char* StText(St s) {
    switch (s) { case St::Waiting: return "Waiting"; case St::Delivered: return "Delivered"; case St::Read: return "Read"; case St::Failed: return "Failed"; default: return "Received"; }
}

void Engine::Logf(const char* fmt, ...) const {
    if (!io_.log) return;
    char b[512]; va_list v; va_start(v, fmt); vsnprintf(b, sizeof b, fmt, v); va_end(v);
    io_.log(b);
}

// One line of strictly valid UTF-8, at most kMaxText bytes (never cut inside a character).
// Dropped / replaced: stray and overlong bytes, surrogates, code points above U+10FFFF,
// control characters (C0, DEL, C1) -> space, and invisible bidi/zero-width controls that can
// make text lie about what it says (U+200B-200F, U+202A-202E, U+2066-2069, U+FEFF).
std::string Engine::CleanText(const std::string& s) {
    std::string o;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        int n; uint32_t cp;
        if (c < 0x80) { n = 1; cp = c; }
        else if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; }
        else { i++; continue; }                          // stray byte (80..C1, F5..FF)
        if (i + n > s.size()) { i++; continue; }
        bool ok = true;
        for (int k = 1; k < n; k++) {
            unsigned char b = (unsigned char)s[i + k];
            if ((b >> 6) != 2) { ok = false; break; }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (!ok) { i++; continue; }
        if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) { i++; continue; }   // overlong / surrogate / too big
        i += n;
        if (cp < 32 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029) {   // newline, tab, DEL, C1... -> space
            if (!o.empty() && o.back() != ' ') o += ' ';
            continue;
        }
        if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF) continue;
        if (o.size() + n > kMaxText) break;
        o.append(s, i - n, n);
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

void Engine::SetPresence(uint64_t peer, const std::string& rawName, bool online, bool inGame) {
    if (!peer) return;
    bool isNew = !contacts_.count(peer);
    if (isNew && contacts_.size() >= kMaxContacts) return;
    Contact& c = Get(peer);
    bool changed = isNew;
    std::string name = CleanText(rawName);   // names come from other players too
    if (!name.empty() && c.name != name) { c.name = name; changed = true; }
    bool wasOnline = c.online;
    c.online = online; c.inGame = online && inGame;
    if (online) {
        long long w = io_.wallclock ? io_.wallclock() : 0;
        if (w - c.lastSeenWall > 60) { c.lastSeenWall = w; changed = true; }   // saved at most once a minute
    }
    if (online && !wasOnline) {   // came online: send what's waiting straight away, and say again how far we've read
        for (auto& m : c.msgs) if (m.mine && m.st == St::Waiting) m.lastSend = 0;
        if (c.readId) c.readPending = true;   // a READ sent while they were away may have been lost
    }
    if (changed) Changed();
}

// Sends (or resends) one message. Does not call Changed(): callers do, so nothing re-enters while
// they are iterating. A photo that can't be sent is marked Failed.
void Engine::Transmit(Contact& c, Msg& m) {
    char head[96];
    if (m.photo) {   // every piece; the receiver keeps what it already has
        std::string jpeg = io_.loadPhoto ? io_.loadPhoto(m.id) : std::string();
        if (jpeg.empty()) { Logf("sms %llx: the photo file is gone - it can't be sent", LL(m.id)); m.st = St::Failed; return; }
        for (size_t off = 0; off < jpeg.size(); off += kChunk) {
            int n = snprintf(head, sizeof head, "C1|PCH|%llx|%llu|%zu|%zu|", LL(m.id), LL(m.key), off, jpeg.size());
            std::string pkt;
            pkt.reserve((size_t)n + kChunk);
            pkt.append(head, (size_t)n);
            pkt.append(jpeg, off, kChunk);
            if (io_.send) io_.send(c.id, pkt);
        }
    } else {
        int n = snprintf(head, sizeof head, "C1|MSG|%llx|%llu|", LL(m.id), LL(m.key));
        std::string pkt(head, (size_t)n);
        pkt += m.text;
        if (io_.send) io_.send(c.id, pkt);
    }
    m.lastSend = io_.now ? io_.now() : 0;
    if (!m.lastSend) m.lastSend = 1;
}

// Keeps the newest kKeepPerContact messages. Never drops one of ours that is still Waiting
// (it hasn't been delivered yet); remembers the ids of theirs that go, so a late repeat isn't
// taken for a new message.
void Engine::Trim(Contact& c) {
    if (c.msgs.size() <= kKeepPerContact) return;
    size_t over = c.msgs.size() - kKeepPerContact;
    decltype(c.msgs) keep;
    for (auto& m : c.msgs) {
        bool pending = m.mine && m.st == St::Waiting;
        if (over && !pending) {
            over--;
            if (!m.mine) Remember(erased_, m.id);
            DropPhoto(m);
            continue;
        }
        keep.push_back(std::move(m));
    }
    c.msgs.swap(keep);
}

// Fills in a new outgoing message and queues it. The key (the order the other side shows
// them in) is wall*1000 but always one more than our last key for this contact, so it never goes
// backwards or repeats, even if the clock stalls or steps back.
Msg& Engine::QueueMine(Contact& c, Msg&& m) {
    m.mine = true;
    m.wall = io_.wallclock ? io_.wallclock() : 0;
    uint64_t key = (uint64_t)(m.wall < 0 ? 0 : m.wall) * 1000;
    for (const auto& x : c.msgs) if (x.mine && x.key >= key) key = x.key + 1;
    m.key = key;
    m.st = St::Waiting;
    uint64_t id = m.id;
    c.msgs.push_back(std::move(m));
    Trim(c);
    for (auto& x : c.msgs) if (x.mine && x.id == id) return x;
    return c.msgs.back();   // (can't happen: Waiting messages are never trimmed)
}

bool Engine::SendPhoto(uint64_t peer, const std::string& jpeg) {
    if (!peer || jpeg.empty() || jpeg.size() > kMaxPhoto || !LooksLikeJpeg(jpeg) ||
        (!contacts_.count(peer) && contacts_.size() >= kMaxContacts)) return false;
    Msg m;
    m.id = io_.newId ? io_.newId() : 1;
    if (!io_.storePhoto || !io_.storePhoto(m.id, jpeg)) { Logf("sms: couldn't keep the photo to send"); return false; }
    Contact& c = Get(peer);
    m.photo = true;
    m.text = kPhotoText;
    Msg& q = QueueMine(c, std::move(m));
    if (c.online) Transmit(c, q);
    Logf("sms %llx: photo to %s (%zu KB)%s", LL(q.id), c.name.c_str(), jpeg.size() / 1024, c.online ? "" : ", waiting until they're online");
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
    if (!HaveTheirs(c, m.id)) return;   // so old that it was trimmed straight away: nothing to show or ring for
    std::string name = c.name;          // (the ring callback may call Forget, so c can't be used after it)
    if (viewing_ == from) MarkRead(from);
    else {
        c.unread++;
        if (io_.ring) io_.ring(from, name, ringText);
    }
    Logf("sms %llx: %s from %s", LL(m.id), m.photo ? "photo" : "message", name.c_str());
    Changed();
}

bool Engine::Send(uint64_t peer, const std::string& text) {
    std::string t = CleanText(text);
    if (t.empty() || !peer || (!contacts_.count(peer) && contacts_.size() >= kMaxContacts)) return false;
    Contact& c = Get(peer);
    Msg m;
    m.id = io_.newId ? io_.newId() : 1;
    m.text = std::move(t);
    Msg& q = QueueMine(c, std::move(m));
    if (c.online) Transmit(c, q);
    Logf("sms %llx: to %s (%zu bytes)%s", LL(q.id), c.name.c_str(), q.text.size(), c.online ? "" : ", waiting until they're online");
    Changed();
    return true;
}

void Engine::SendRead(Contact& c) {
    if (!c.readId || !c.online) return;
    SendCtl(io_, c.id, "READ", c.readId);
    c.readPending = false;   // (SetPresence sets it again when they come back online)
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

// Forgets a contact. Anything they are still repeating (messages, half-received photos) is
// remembered as erased, so it can't bring the conversation back.
void Engine::Forget(uint64_t peer) {
    auto it = contacts_.find(peer);
    if (it == contacts_.end()) return;
    for (auto& m : it->second.msgs) { if (!m.mine) Remember(erased_, m.id); DropPhoto(m); }
    for (auto p = partial_.begin(); p != partial_.end();) {
        if (p->second.from == peer) { Remember(erased_, p->first); p = partial_.erase(p); } else ++p;
    }
    contacts_.erase(it);
    Changed();
}

void Engine::EraseAll() {
    size_t n = 0;
    for (auto it = contacts_.begin(); it != contacts_.end();) {
        Contact& c = it->second;
        for (auto& m : c.msgs) { if (!m.mine) Remember(erased_, m.id); DropPhoto(m); }
        n += c.msgs.size();
        if (!c.online) { it = contacts_.erase(it); continue; }   // previous contacts go; who's online now stays listed
        c.msgs.clear();
        c.unread = 0;
        c.readId = 0;
        c.readPending = false;
        ++it;
    }
    for (auto& kv : partial_) Remember(erased_, kv.first);   // a photo still arriving must not complete later
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
    long long num = strtoll(msg.substr(p3 + 1, p4 - p3 - 1).c_str(), nullptr, 10);
    if (num < 0) num = 0;
    if (!id) return;

    // The contact is only created (or marked online) once the message is known to be real, and
    // never past kMaxContacts: junk and strangers can't fill the list.
    auto touch = [&]() -> Contact* {
        if (!contacts_.count(from) && contacts_.size() >= kMaxContacts) return nullptr;
        Contact& c = Get(from);
        if (!c.online) SetPresence(from, std::string(), true, false);   // same "came online" handling as everywhere else
        return &c;
    };

    if (type == "MSG") {
        if (erased_.count(id)) { SendCtl(io_, from, "ACK", id); return; }     // a repeat of one the player erased
        std::string text = CleanText(msg.substr(p4 + 1));
        if (text.empty()) { SendCtl(io_, from, "ACK", id); return; }          // nothing to show, but stop the repeats
        Contact* cp = touch();
        if (!cp) return;
        Contact& c = *cp;
        SendCtl(io_, from, "ACK", id);   // always: they repeat until they hear it
        if (HaveTheirs(c, id)) return;   // a repeat
        Msg m; m.id = id; m.mine = false; m.key = (uint64_t)num; m.wall = num / 1000; m.text = std::move(text); m.st = St::Received;
        AddTheirs(c, from, m, m.text);
        return;
    }
    if (type == "PCH") {   // a piece of a photo: <offset>|<total>|<bytes>
        if (erased_.count(id)) { SendCtl(io_, from, "ACK", id); return; }
        size_t q1 = msg.find('|', p4 + 1), q2 = q1 == std::string::npos ? q1 : msg.find('|', q1 + 1);
        if (q2 == std::string::npos) return;
        size_t off = (size_t)strtoull(msg.substr(p4 + 1, q1 - p4 - 1).c_str(), nullptr, 10);
        size_t total = (size_t)strtoull(msg.substr(q1 + 1, q2 - q1 - 1).c_str(), nullptr, 10);
        size_t len = msg.size() - (q2 + 1);
        if (!total || total > kMaxPhoto || off % kChunk || off >= total || len != std::min(kChunk, total - off)) { Logf("sms: ignored a bad photo piece"); return; }
        auto contact = contacts_.find(from);
        if (contact != contacts_.end() && HaveTheirs(contact->second, id)) { SendCtl(io_, from, "ACK", id); return; }   // already have it all
        if (contact == contacts_.end() && contacts_.size() >= kMaxContacts) return;
        auto pit = partial_.find(id);
        if (pit == partial_.end()) {
            // a new photo: limit how many half-received ones there can be, in total and per peer
            if (partial_.size() >= kMaxPartials || Receiving(from) >= kMaxPartialsPerPeer) return;
            Partial& np = partial_[id];
            np.from = from; np.key = (uint64_t)num; np.data.assign(total, '\0'); np.got.assign((total + kChunk - 1) / kChunk, false);
            pit = partial_.find(id);
        }
        Partial& pa = pit->second;
        if (pa.from != from || pa.data.size() != total) return;
        size_t idx = off / kChunk;
        if (!pa.got[idx]) { memcpy(&pa.data[off], msg.data() + q2 + 1, len); pa.got[idx] = true; pa.have++; }
        pa.last = io_.now ? io_.now() : 1;
        if (pa.have < pa.got.size()) return;
        std::string jpeg; jpeg.swap(pa.data);
        uint64_t key = pa.key;
        partial_.erase(pit);
        if (!LooksLikeJpeg(jpeg)) {   // not a photo: don't keep it, and stop them repeating it
            Logf("sms %llx: a photo that came in isn't a JPEG - dropped", LL(id));
            Remember(erased_, id);
            Changed();
            SendCtl(io_, from, "ACK", id);
            return;
        }
        Contact* cp = touch();
        if (!cp) return;
        Contact& c = *cp;
        if (!io_.storePhoto || !io_.storePhoto(id, jpeg)) { Logf("sms %llx: couldn't keep a photo that came in", LL(id)); return; }
        SendCtl(io_, from, "ACK", id);
        Msg m; m.id = id; m.mine = false; m.key = key; m.wall = (long long)(key / 1000); m.text = kPhotoText; m.photo = true; m.st = St::Received;
        AddTheirs(c, from, m, "\xF0\x9F\x93\xB7 Photo");
        return;
    }
    if (type == "ACK") {
        if (!contacts_.count(from)) return;   // acks never create a contact
        Contact* cp = touch();
        if (!cp) return;
        bool changed = false;
        for (auto& m : cp->msgs)
            if (m.mine && m.id == id && m.st == St::Waiting) { m.st = St::Delivered; changed = true; }
        if (changed) Changed();
        return;
    }
    if (type == "READ") {
        if (!contacts_.count(from)) return;
        Contact* cp = touch();
        if (!cp) return;
        Contact& c = *cp;
        int upto = -1;
        for (int i = 0; i < (int)c.msgs.size(); i++) if (c.msgs[i].mine && c.msgs[i].id == id) upto = i;
        if (upto < 0) return;
        bool changed = false;
        for (int i = 0; i <= upto; i++)
            if (c.msgs[i].mine && c.msgs[i].st != St::Read && c.msgs[i].st != St::Failed) { c.msgs[i].st = St::Read; changed = true; }
        if (changed) Changed();
        return;
    }
    // any other type: ignored (and no contact is created for it)
}

void Engine::Tick() {
    uint64_t now = io_.now ? io_.now() : 0;
    for (auto it = partial_.begin(); it != partial_.end();)   // half-received photos that stopped coming
        if (now - it->second.last > kPartialExpire) it = partial_.erase(it); else ++it;
    bool changed = false;
    for (auto& kv : contacts_) {
        Contact& c = kv.second;
        if (!c.online) continue;
        int sent = 0, photos = 0;
        for (auto& m : c.msgs) {
            if (!m.mine || m.st != St::Waiting || sent >= 10) continue;
            uint64_t every = m.photo ? kPhotoResend : kResend;
            if (m.lastSend != 0 && now - m.lastSend < every) continue;
            if (m.photo && photos++ >= 1) continue;   // one photo per contact per tick
            St before = m.st;
            Transmit(c, m); sent++;
            if (m.st != before) changed = true;
        }
        if (c.readPending) SendRead(c);
    }
    if (changed) Changed();   // after the loops: a listener may add or remove contacts
}

// ---- saved state: Gitmo-sms.txt ------------------------------------------------------------
//   C <tab> id <tab> lastSeen <tab> unread <tab> readId(hex) <tab> name
//   M <tab> peer <tab> id(hex) <tab> mine <tab> wall <tab> state <tab> key <tab> text
//   E <tab> id(hex)                      (a message that was erased: its repeats are ignored)
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
        snprintf(b, sizeof b, "C\t%llu\t%lld\t%d\t%llx\t", LL(c.id), c.lastSeenWall, c.unread, LL(c.readId));
        o << b << Esc(c.name) << "\n";
        for (auto& m : c.msgs) {
            snprintf(b, sizeof b, "M\t%llu\t%llx\t%d\t%lld\t%d\t%llu\t", LL(c.id), LL(m.id), m.mine ? 1 : 0, m.wall, (int)m.st, LL(m.key));
            o << b << Esc(m.text) << "\n";
        }
    }
    for (uint64_t id : erased_) { snprintf(b, sizeof b, "E\t%llx\n", LL(id)); o << b; }
    return o.str();
}

void Engine::Load(const std::string& text) {
    contacts_.clear();   // loading replaces what's there; calling it twice must not double everything
    partial_.clear();
    erased_.clear();
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
            uint64_t peer = strtoull(f[1].c_str(), nullptr, 10);
            if (!peer) continue;
            if (!contacts_.count(peer) && contacts_.size() >= kMaxContacts) continue;
            Contact& c = Get(peer);
            c.lastSeenWall = strtoll(f[2].c_str(), nullptr, 10);
            c.unread = atoi(f[3].c_str());
            c.readId = strtoull(f[4].c_str(), nullptr, 16);
            c.name = CleanText(Unesc(f[5]));
            nc++;
        } else if (f[0] == "M" && f.size() >= 8) {
            uint64_t peer = strtoull(f[1].c_str(), nullptr, 10);
            if (!peer) continue;
            if (!contacts_.count(peer) && contacts_.size() >= kMaxContacts) continue;
            Msg m;
            m.id = strtoull(f[2].c_str(), nullptr, 16);
            m.mine = f[3] == "1";
            m.wall = strtoll(f[4].c_str(), nullptr, 10);
            int st = atoi(f[5].c_str());
            m.st = st >= 0 && st <= (int)St::Failed ? (St)st : (m.mine ? St::Waiting : St::Received);
            m.key = strtoull(f[6].c_str(), nullptr, 10);
            m.text = Unesc(f[7]);
            m.photo = m.text == kPhotoText;
            if (!m.photo) m.text = CleanText(m.text);   // same rules as live messages (also caps the length)
            if (m.id && (m.photo || !m.text.empty())) { Get(peer).msgs.push_back(std::move(m)); nm++; }
        } else if (f[0] == "E" && f.size() >= 2) {
            uint64_t id = strtoull(f[1].c_str(), nullptr, 16);
            if (id) Remember(erased_, id);
        }
    }
    for (auto& kv : contacts_) Trim(kv.second);   // a hand-edited or old file can't exceed the per-contact limit
    Logf("sms: loaded %d contact(s), %d message(s)", nc, nm);
}

}   // namespace sms