// Gitmo - transfer protocol. See transfer.h for the message flow.
#include "transfer.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <climits>

namespace tx {

const char* OutText(Out s) {
    switch (s) {
    case Out::Offered: return "Offered"; case Out::Asked: return "Asked"; case Out::Paid: return "Paid";
    case Out::RefundDue: return "RefundDue"; case Out::Done: return "Done"; case Out::Declined: return "Declined";
    case Out::Expired: return "Expired"; case Out::Canceled: return "Canceled"; case Out::Unreachable: return "Unreachable";
    case Out::Refunded: return "Refunded"; case Out::Failed: return "Failed";
    }
    return "?";
}
bool OutFinal(Out s) { return !(s == Out::Offered || s == Out::Asked || s == Out::Paid || s == Out::RefundDue); }
bool InFinal(In s) { return s == In::Credited || s == In::Declined || s == In::Void; }
static const char* InText(In s) {
    switch (s) { case In::Prompt: return "Prompt"; case In::Accepted: return "Accepted"; case In::Credited: return "Credited";
                 case In::Declined: return "Declined"; case In::Void: return "Void"; }
    return "?";
}

static std::string Clean(const std::string& s) {   // names travel inside '|'-separated messages
    std::string o;
    for (char c : s) if (c != '|' && c != '\n' && c != '\r' && c != '\t') o += c;
    if (o.size() > 48) {
        size_t end = 48;
        if (((unsigned char)o[end] & 0xC0) == 0x80) {
            while (end && ((unsigned char)o[end] & 0xC0) == 0x80) --end;
        }
        o.resize(end);
    }
    return o;
}

static bool ParseUnsigned(const std::string& s, int base, uint64_t& value) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        bool digit = c >= '0' && c <= '9';
        bool hex = (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!digit && !(base == 16 && hex)) return false;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = strtoull(s.c_str(), &end, base);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    value = (uint64_t)parsed;
    return true;
}

static bool ParseNonnegative(const std::string& s, long long& value) {
    uint64_t parsed = 0;
    if (!ParseUnsigned(s, 10, parsed) || parsed > (uint64_t)LLONG_MAX) return false;
    value = (long long)parsed;
    return true;
}

void Engine::Logf(const char* fmt, ...) const {
    if (!io_.log) return;
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    io_.log(b);
}

std::string Engine::NameOf(uint64_t peer) const {
    auto it = peers_.find(peer);
    if (it != peers_.end() && !it->second.name.empty()) return it->second.name;
    return "another player";
}

void Engine::SendMsg(uint64_t peer, const std::string& type, uint64_t id, long long amount, const std::string& extra) {
    char head[96];
    snprintf(head, sizeof head, "G1|%s|%llx|%lld|", type.c_str(), (unsigned long long)id, amount);
    if (io_.send) io_.send(peer, head + Clean(extra));
}

void Engine::SetOut(Outgoing& o, Out st, const std::string& note) {
    Logf("transfer %llx to %s: %s -> %s%s%s", (unsigned long long)o.id, o.name.c_str(), OutText(o.st), OutText(st),
         note.empty() ? "" : " (", note.empty() ? "" : (note + ")").c_str());
    o.st = st; o.changed = io_.now(); o.lastSend = 0; o.persistPending = false;
    if (!note.empty()) o.note = note;
    if (st == Out::Expired || st == Out::Canceled || st == Out::Unreachable || st == Out::Failed) o.cancelsLeft = 3;
    // shows in Activity: a transfer that ended without moving any money
    const char* why = nullptr;
    switch (st) {
    case Out::Declined: why = o.note.empty() ? "Declined" : o.note.c_str(); break;
    case Out::Expired: why = "No answer"; break;
    case Out::Canceled: why = "You canceled"; break;
    case Out::Unreachable: why = "Couldn't reach them"; break;
    case Out::Failed: why = o.note.empty() ? "Didn't go through" : o.note.c_str(); break;
    default: break;
    }
    if (why) Note("To " + o.name, o.amount, why);
}

void Engine::SetIn(Incoming& i, In st, const std::string& why) {
    Logf("transfer %llx from %s: %s -> %s ($%lld)", (unsigned long long)i.id, i.name.c_str(), InText(i.st), InText(st), i.amount);
    i.st = st; i.changed = io_.now(); i.lastSend = 0; i.persistPending = false;
    if (InFinal(st)) i.wall = io_.wallclock ? io_.wallclock() : 0;
    if (st == In::Declined || st == In::Void) i.why = why;
    if (st == In::Declined || st == In::Void) Note("From " + i.name, i.amount, why.empty() ? (st == In::Declined ? "You declined" : "Called off") : why);
}

Incoming* Engine::FindIn(uint64_t peer, uint64_t id) {
    for (auto& i : ins_) if (i.peer == peer && i.id == id) return &i;
    return nullptr;
}
Outgoing* Engine::FindOutMut(uint64_t peer, uint64_t id) {
    for (auto& o : outs_) if (o.id == id && (peer == 0 || o.peer == peer)) return &o;
    return nullptr;
}
const Outgoing* Engine::FindOut(uint64_t id) const {
    for (auto& o : outs_) if (o.id == id) return &o;
    return nullptr;
}

// ---- saved state ----------------------------------------------------------------------
// Keep unsettled payments, completed refunds, and receiver credits so restarts
// cannot repeat a money change.
std::string Engine::Save() const {
    std::string s = "# Gitmo transfers - do not edit while the game is running\n";
    char line[256];
    for (auto& o : outs_) {
        if (o.st != Out::Paid && o.st != Out::RefundDue && o.st != Out::Refunded) continue;
        snprintf(line, sizeof line, "OUT %llu %llx %lld %c %s\n", (unsigned long long)o.peer, (unsigned long long)o.id, o.amount,
                 o.st == Out::Paid ? 'P' : o.st == Out::RefundDue ? 'R' : 'F', Clean(o.name).c_str());
        s += line;
    }
    for (auto& i : ins_) {
        if (i.st != In::Credited) continue;
        snprintf(line, sizeof line, "IN %llu %llx %lld %lld %s\n", (unsigned long long)i.peer, (unsigned long long)i.id, i.amount,
                 i.wall, Clean(i.name).c_str());
        s += line;
    }
    return s;
}

bool Engine::Persist() {
    if (!io_.persist) {
        Logf("transfers: can't persist transfer state (storage callback is unavailable)");
        return false;
    }
    if (!io_.persist(Save())) {
        Logf("transfers: couldn't persist transfer state");
        return false;
    }
    return true;
}

void Engine::TrimIncomingHistory() {
    constexpr size_t kMaxUnseenTerminal = 256;
    auto isUnseenTerminal = [](const Incoming& i) {
        return i.st != In::Credited && InFinal(i.st) && !i.seen;
    };
    size_t count = (size_t)std::count_if(ins_.begin(), ins_.end(), isUnseenTerminal);
    size_t removed = 0;
    while (count > kMaxUnseenTerminal) {
        auto oldest = std::find_if(ins_.begin(), ins_.end(), isUnseenTerminal);
        if (oldest == ins_.end()) break;
        ins_.erase(oldest);
        --count;
        ++removed;
    }
    if (removed) Logf("transfers: discarded %zu oldest unseen request result(s) to cap history", removed);
}

void Engine::TryRefund(Outgoing& o) {
    if (!io_.applyMoney(o.amount, "Refund (" + o.name + ")")) {
        Logf("transfer %llx: refund still cannot be applied", (unsigned long long)o.id);
        return;
    }
    SetOut(o, Out::Refunded);
    if (Persist()) return;

    if (io_.applyMoney(-o.amount, "Undo unrecorded refund (" + o.name + ")")) {
        SetOut(o, Out::RefundDue);
        o.lastSend = io_.now();
        return;
    }

    o.persistPending = true;
    o.lastSend = io_.now();
    Logf("transfer %llx: refund changed the balance but could not be saved or rolled back; retrying persistence",
         (unsigned long long)o.id);
}

void Engine::Load(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    uint64_t now = io_.now();
    long long wall = io_.wallclock ? io_.wallclock() : 0;
    int nOut = 0, nIn = 0, skipped = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.size() > 512) { ++skipped; continue; }
        std::istringstream ls(line);
        std::string kind, p, i, a, f5;
        if (!(ls >> kind >> p >> i >> a >> f5)) { ++skipped; continue; }
        uint64_t peer = 0, id = 0;
        long long amount = 0;
        if (!ParseUnsigned(p, 10, peer) || !ParseUnsigned(i, 16, id) ||
            !ParseNonnegative(a, amount) || !peer || !id || amount <= 0 || amount > kMaxAmount) {
            ++skipped;
            continue;
        }
        std::string name; std::getline(ls, name);
        if (!name.empty() && name[0] == ' ') name.erase(0, 1);
        name = Clean(name);
        if (kind == "OUT" && (f5 == "P" || f5 == "R" || f5 == "F")) {
            if (FindOutMut(peer, id)) { ++skipped; continue; }
            Outgoing o; o.peer = peer; o.id = id; o.amount = amount; o.name = name;
            o.st = f5 == "P" ? Out::Paid : f5 == "R" ? Out::RefundDue : Out::Refunded;
            o.created = o.changed = now;
            outs_.push_back(o); nOut++;
        } else if (kind == "IN") {
            long long w = 0;
            if (FindIn(peer, id) || !ParseNonnegative(f5, w)) { ++skipped; continue; }
            if (wall && w && wall - w > kKeepSettledSec) continue;   // old enough to forget
            Incoming x; x.peer = peer; x.id = id; x.amount = amount; x.name = name; x.st = In::Credited;
            x.wall = w; x.seen = true; x.created = x.changed = now;
            ins_.push_back(x); nIn++;
        } else ++skipped;
    }
    TrimIncomingHistory();
    Logf("transfers: loaded %d outgoing and %d received transfer record(s)", nOut, nIn);
    if (skipped) Logf("transfers: ignored %d malformed or duplicate saved record(s)", skipped);
}

// ---- the player ---------------------------------------------------------------------------
SendResult Engine::Send(uint64_t peer, long long amount, uint64_t* idOut) {
    if (peer == me_) return SendResult::Self;
    if (amount <= 0 || amount > kMaxAmount) return SendResult::BadAmount;
    long long bal = io_.balance();
    if (bal < 0) return SendResult::NotInMap;
    if (bal < amount) return SendResult::NotEnough;
    if (io_.mayPay && !io_.mayPay()) return SendResult::InGarage;
    auto it = peers_.find(peer);
    uint64_t now = io_.now();
    if (it == peers_.end() || !it->second.hasGitmo || now - it->second.lastSeen > kOnline) return SendResult::Offline;
    if (sessionOnly_ && !InGame(it->second)) return SendResult::NotInGame;
    for (auto& o : outs_) if (o.st == Out::Offered || o.st == Out::Asked) return SendResult::Busy;
    Outgoing o;
    o.peer = peer; o.id = io_.newId(); o.amount = amount; o.name = NameOf(peer);
    o.st = Out::Offered; o.created = o.changed = now;
    outs_.push_back(o);
    Logf("transfer %llx: offering $%lld to %s", (unsigned long long)o.id, amount, o.name.c_str());
    SendMsg(peer, "OFFER", o.id, amount, myName_);
    outs_.back().lastSend = now;
    if (idOut) *idOut = o.id;
    return SendResult::Ok;
}

bool Engine::Cancel(uint64_t id) {
    Outgoing* o = FindOutMut(0, id);
    if (!o || (o->st != Out::Offered && o->st != Out::Asked)) return false;
    SetOut(*o, Out::Canceled, "You canceled");
    SendMsg(o->peer, "CANCEL", o->id, o->amount);
    o->lastSend = io_.now(); o->cancelsLeft--;
    return true;
}

bool Engine::Accept(uint64_t peer, uint64_t id) {
    Incoming* i = FindIn(peer, id);
    if (!i || i->st != In::Prompt) return false;
    long long bal = io_.balance();
    if (bal < 0 || bal + i->amount > kMaxBalance) {   // left the map since the request came in
        SetIn(*i, In::Declined, bal < 0 ? "Declined: you weren't in a map" : "Declined: your wallet is full");
        SendMsg(peer, "DECLINE", id, i->amount, bal < 0 ? "notinmap" : "full");
        return false;
    }
    SetIn(*i, In::Accepted);
    SendMsg(peer, "ACCEPT", id, i->amount);
    i->lastSend = io_.now();
    return true;
}

bool Engine::Decline(uint64_t peer, uint64_t id) {
    Incoming* i = FindIn(peer, id);
    if (!i || i->st != In::Prompt) return false;
    SetIn(*i, In::Declined);
    i->seen = true;
    SendMsg(peer, "DECLINE", id, i->amount, "declined");
    return true;
}

void Engine::MarkSeen(uint64_t peer, uint64_t id) { if (Incoming* i = FindIn(peer, id)) i->seen = true; }

// ---- peers ----------------------------------------------------------------------------------
void Engine::SetPeer(uint64_t peer, const std::string& name, bool direct) {
    if (!peer || peer == me_) return;
    Peer& p = peers_[peer];
    p.id = peer;
    if (!name.empty()) p.name = name;
    p.direct = direct;
}

// Same session: our game lists them as a member, or both games report the same session id.
bool Engine::InGame(const Peer& p) const {
    if (p.direct) return true;
    if (mySession_.empty() || !p.sessionAt || io_.now() - p.sessionAt > kOnline) return false;
    return p.session == mySession_;
}
bool Engine::InGame(uint64_t peer) const {
    auto it = peers_.find(peer);
    return it != peers_.end() && InGame(it->second);
}

void Engine::SayHello(uint64_t peer) {
    if (!peer || peer == me_) return;
    SendMsg(peer, "HELLO", 0, 0, myName_);
    // our game session (older Gitmo versions ignore this message)
    SendMsg(peer, "SESS", 0, 0, "h:" + mySession_);
    lastHello_[peer] = io_.now();
}

std::vector<Peer> Engine::OnlinePeers() const {
    std::vector<Peer> v;
    uint64_t now = io_.now();
    for (auto& kv : peers_)
        if (kv.second.hasGitmo && kv.second.lastSeen && now - kv.second.lastSeen <= kOnline) {
            Peer p = kv.second;
            p.sameGame = InGame(p);
            if (sessionOnly_ && !p.sameGame) continue;
            v.push_back(p);
        }
    std::sort(v.begin(), v.end(), [](const Peer& a, const Peer& b) {
        if (a.sameGame != b.sameGame) return a.sameGame;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return v;
}

std::vector<Peer> Engine::OnlinePeersAll() const {
    std::vector<Peer> v;
    uint64_t now = io_.now();
    for (auto& kv : peers_)
        if (kv.second.hasGitmo && kv.second.lastSeen && now - kv.second.lastSeen <= kOnline) v.push_back(kv.second);
    return v;
}

// ---- messages -------------------------------------------------------------------------------
void Engine::OnMessage(uint64_t from, const std::string& msg) {
    if (!from || from == me_) return;
    // G1|TYPE|id|amount|extra
    if (msg.size() > 1024) { Logf("transfers: ignored an oversized message"); return; }
    size_t p1 = msg.find('|'), p2 = p1 == std::string::npos ? p1 : msg.find('|', p1 + 1);
    size_t p3 = p2 == std::string::npos ? p2 : msg.find('|', p2 + 1);
    size_t p4 = p3 == std::string::npos ? p3 : msg.find('|', p3 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos ||
        p4 == std::string::npos || p2 == p1 + 1 || p3 == p2 + 1 || p4 == p3 + 1 ||
        msg.compare(0, p1, "G1") != 0) {
        Logf("transfers: ignored a malformed message");
        return;
    }
    std::string type = msg.substr(p1 + 1, p2 - p1 - 1);
    bool handshake = type == "HELLO" || type == "SESS";
    bool transaction = type == "OFFER" || type == "CANCEL" || type == "PAY" ||
                       type == "ACK" || type == "ACCEPT" || type == "DECLINE" ||
                       type == "RECEIPT" || type == "VOID";
    uint64_t id = 0;
    long long amount = 0;
    if ((!handshake && !transaction) ||
        !ParseUnsigned(msg.substr(p2 + 1, p3 - p2 - 1), 16, id) ||
        !ParseNonnegative(msg.substr(p3 + 1, p4 - p3 - 1), amount) ||
        (handshake && (id != 0 || amount != 0)) ||
        (transaction && (!id || amount <= 0 || amount > kMaxAmount))) {
        Logf("transfers: ignored a malformed message");
        return;
    }
    std::string extra = Clean(msg.substr(p4 + 1));
    uint64_t now = io_.now();

    Peer& peer = peers_[from];
    peer.id = from;
    bool wasOnline = peer.hasGitmo && peer.lastSeen && now - peer.lastSeen <= kOnline;
    peer.hasGitmo = true;
    peer.lastSeen = now;

    if (type == "HELLO") {
        if (peer.name.empty() && !extra.empty()) peer.name = extra;
        if (!wasOnline) Logf("transfers: %s has Gitmo and is online", NameOf(from).c_str());
        auto h = lastHello_.find(from);
        if (h == lastHello_.end() || now - h->second > 3000) SayHello(from);   // answer, so they see us at once
        return;
    }
    if (type == "SESS") {
        bool before = InGame(peer);
        if (extra.compare(0, 2, "h:") == 0) { peer.session = extra.substr(2); peer.sessionAt = now; }
        else { peer.session.clear(); peer.sessionAt = 0; }   // Gitmo 0.5.3 sent something else here
        if (InGame(peer) != before) Logf("transfers: %s is %s", NameOf(from).c_str(), before ? "no longer in your game" : "in your game");
        return;
    }
    // ---- we are the receiver ----
    if (type == "OFFER") {
        if (Incoming* i = FindIn(from, id)) {   // a repeat: answer the same again
            if (i->amount != amount) return;
            if (i->st == In::Prompt) SendMsg(from, "ACK", id, i->amount);
            else if (i->st == In::Accepted) SendMsg(from, "ACCEPT", id, i->amount);
            else if (i->st == In::Declined || i->st == In::Void) SendMsg(from, "DECLINE", id, i->amount, "declined");
            return;
        }
        const char* why = nullptr;
        long long bal = io_.balance();
        int prompts = 0;
        for (auto& i : ins_) if (i.st == In::Prompt) prompts++;
        if (amount <= 0 || amount > kMaxAmount) why = "bad";
        else if (sessionOnly_ && !InGame(peer)) why = "notingame";
        else if (bal < 0) why = "notinmap";
        else if (bal + amount > kMaxBalance) why = "full";
        else if (prompts >= 5) why = "busy";
        if (why) {
            Logf("transfer %llx from %s: $%lld turned down automatically (%s)", (unsigned long long)id, NameOf(from).c_str(), amount, why);
            if (strcmp(why, "bad") != 0)
                Note("From " + NameOf(from), amount, !strcmp(why, "notingame") ? "Declined: not in your game" :
                     !strcmp(why, "notinmap") ? "Declined: you weren't in a map" :
                     !strcmp(why, "full") ? "Declined: your wallet is full" : "Declined: too many requests open");
            SendMsg(from, "DECLINE", id, amount, why);
            return;
        }
        if (peer.name.empty() && !extra.empty()) peer.name = extra;
        Incoming i;
        i.peer = from; i.id = id; i.amount = amount; i.name = NameOf(from);
        i.st = In::Prompt; i.created = i.changed = now;
        ins_.push_back(i);
        Logf("transfer %llx: %s wants to send you $%lld", (unsigned long long)id, i.name.c_str(), amount);
        SendMsg(from, "ACK", id, amount);
        if (io_.ring) io_.ring();
        return;
    }
    if (type == "CANCEL") {
        if (Incoming* i = FindIn(from, id)) if (i->amount == amount && (i->st == In::Prompt || i->st == In::Accepted)) {
            SetIn(*i, In::Void, i->name + " canceled"); i->seen = false;
            if (io_.ring) io_.ring();   // the receiver hears it, like the request itself
        }
        TrimIncomingHistory();
        return;
    }
    if (type == "PAY") {
        Incoming* i = FindIn(from, id);
        if (i && i->st == In::Credited && amount == i->amount) {
            if (i->persistPending) {
                if (!Persist()) return;
                i->persistPending = false;
            }
            SendMsg(from, "RECEIPT", id, i->amount);
            return;
        }
        if (i && i->st == In::Accepted && amount == i->amount) {
            if (io_.applyMoney(amount, "From " + i->name)) {
                SetIn(*i, In::Credited);
                i->seen = false;
                if (!Persist()) {
                    if (io_.applyMoney(-amount, "Undo unrecorded transfer from " + i->name)) {
                        SetIn(*i, In::Accepted);
                        Logf("transfer %llx: couldn't save the credit; rolled it back and will retry",
                             (unsigned long long)id);
                    } else {
                        i->persistPending = true;
                        i->lastSend = now;
                        Logf("transfer %llx: credit changed the balance but couldn't be saved or rolled back; withholding receipt",
                             (unsigned long long)id);
                    }
                    return;
                }
                SendMsg(from, "RECEIPT", id, amount);
                return;
            }
            Logf("transfer %llx: couldn't add the money (not in a map?) -> cancelled, the sender is refunded", (unsigned long long)id);
        }
        // Not accepted, already cancelled, wrong amount or unknown: it will never be credited.
        if (i && !InFinal(i->st)) { SetIn(*i, In::Void, "Didn't arrive"); i->seen = false; }
        TrimIncomingHistory();
        SendMsg(from, "VOID", id, amount);
        return;
    }

    // ---- we are the sender ----
    Outgoing* o = FindOutMut(from, id);
    if (type == "ACK") {
        if (o && o->amount == amount && o->st == Out::Offered) SetOut(*o, Out::Asked);
        return;
    }
    if (type == "ACCEPT") {
        if (o && o->amount != amount) return;
        if (!o || (OutFinal(o->st) && o->st != Out::Done)) { SendMsg(from, "CANCEL", id, amount); return; }
        if (o->st == Out::Paid) {
            if (o->persistPending) {
                if (!Persist()) return;
                o->persistPending = false;
            }
            SendMsg(from, "PAY", id, o->amount); o->lastSend = now; return;
        }
        if (o->st != Out::Offered && o->st != Out::Asked) return;
        long long bal = io_.balance();
        const char* fail = bal < 0 ? "You're not in a map" : bal < o->amount ? "Not enough money any more" : nullptr;
        // the money leaves only now, when they accept: not while the sender is in a garage, whose
        // screens would still show (and let them spend) the old balance
        if (!fail && io_.mayPay && !io_.mayPay()) fail = "You were in a garage - send it again outside";
        if (!fail && !io_.applyMoney(-o->amount, "To " + o->name)) fail = "Couldn't change your balance";
        if (fail) {
            SetOut(*o, Out::Failed, fail);
            SendMsg(from, "CANCEL", id, o->amount);
            o->lastSend = now; o->cancelsLeft--;
            return;
        }
        SetOut(*o, Out::Paid);
        if (!Persist()) {
            if (io_.applyMoney(o->amount, "Undo unsaved transfer to " + o->name)) {
                SetOut(*o, Out::Failed, "Couldn't save the transfer");
                SendMsg(from, "CANCEL", id, o->amount);
                o->lastSend = now; o->cancelsLeft--;
                Logf("transfer %llx: couldn't save the payment; rolled it back and canceled",
                     (unsigned long long)id);
            } else {
                o->persistPending = true;
                o->lastSend = now;
                Logf("transfer %llx: debit changed the balance but couldn't be saved or rolled back; withholding payment",
                     (unsigned long long)id);
            }
            return;
        }
        SendMsg(from, "PAY", id, o->amount);
        o->lastSend = now;
        return;
    }
    if (type == "DECLINE") {
        if (!o || o->amount != amount || (o->st != Out::Offered && o->st != Out::Asked)) return;
        std::string n = o->name;
        if (extra == "expired") SetOut(*o, Out::Expired, "No answer");
        else if (extra == "notinmap") SetOut(*o, Out::Declined, n + " isn't in a map right now");
        else if (extra == "notingame") SetOut(*o, Out::Declined, n + " isn't in your game");
        else if (extra == "full") SetOut(*o, Out::Declined, n + "'s wallet is full");
        else if (extra == "busy") SetOut(*o, Out::Declined, n + " has too many requests open");
        else if (extra == "bad") SetOut(*o, Out::Declined, "That amount isn't allowed");
        else SetOut(*o, Out::Declined, n + " declined");
        o->cancelsLeft = 0;
        return;
    }
    if (type == "RECEIPT") {
        if (o && o->amount == amount && o->st == Out::Paid) { SetOut(*o, Out::Done); Persist(); }
        return;
    }
    if (type == "VOID") {
        if (!o || o->amount != amount) return;
        if (o->st == Out::Paid) {
            SetOut(*o, Out::RefundDue, "It didn't go through, so the money came back");
            if (Persist()) {
                o->cancelsLeft = 0;
                TryRefund(*o);
            } else {
                SetOut(*o, Out::Paid);
                Logf("transfer %llx: couldn't save refund state; will retry without refunding yet",
                     (unsigned long long)id);
            }
        } else if (o->st == Out::Offered || o->st == Out::Asked) {
            SetOut(*o, Out::Expired, "It didn't go through");
            o->cancelsLeft = 0;
        }
        return;
    }
}

// ---- timers ---------------------------------------------------------------------------------
void Engine::Tick() {
    uint64_t now = io_.now();
    for (auto& o : outs_) {
        auto online = [&]() { auto it = peers_.find(o.peer); return it != peers_.end() && it->second.lastSeen && now - it->second.lastSeen <= kOnline; };
        switch (o.st) {
        case Out::Offered:
            if (now - o.created > kReach) SetOut(o, Out::Unreachable, "Couldn't reach " + o.name);
            else if (now - o.lastSend >= kResend) { SendMsg(o.peer, "OFFER", o.id, o.amount, myName_); o.lastSend = now; }
            break;
        case Out::Asked:
            if (now - o.created > kOfferTtl) SetOut(o, Out::Expired, "No answer");
            break;
        case Out::Paid: {
            if (o.persistPending) {
                if (now - o.lastSend >= kResend) {
                    o.lastSend = now;
                    if (Persist()) {
                        o.persistPending = false;
                        SendMsg(o.peer, "PAY", o.id, o.amount);
                        o.lastSend = now;
                    }
                }
                break;
            }
            uint64_t every = !online() ? 30000 : now - o.changed > 60000 ? kPaidResendMax : kResend;
            if (now - o.lastSend >= every) { SendMsg(o.peer, "PAY", o.id, o.amount); o.lastSend = now; }
            break;
        }
        case Out::RefundDue:
            if (now - o.lastSend >= kResend) { o.lastSend = now; TryRefund(o); }
            break;
        case Out::Refunded:
            if (o.persistPending && now - o.lastSend >= kResend) {
                o.lastSend = now;
                if (Persist()) o.persistPending = false;
            }
            break;
        default:
            if (o.cancelsLeft > 0 && now - o.lastSend >= kResend) {
                SendMsg(o.peer, "CANCEL", o.id, o.amount);
                o.lastSend = now; o.cancelsLeft--;
            }
            break;
        }
    }
    for (auto& i : ins_) {
        if (i.st == In::Prompt && now - i.created > kPromptTtl) {
            SetIn(i, In::Void, "No answer"); i.seen = false;
            SendMsg(i.peer, "DECLINE", i.id, i.amount, "expired");
        } else if (i.st == In::Accepted) {
            if (now - i.changed > kPayWait) {   // no payment came: call it off for good
                SetIn(i, In::Void, "Didn't arrive"); i.seen = false;
                SendMsg(i.peer, "VOID", i.id, i.amount);
            } else if (now - i.lastSend >= kResend) { SendMsg(i.peer, "ACCEPT", i.id, i.amount); i.lastSend = now; }
        } else if (i.st == In::Credited && i.persistPending && now - i.lastSend >= kResend) {
            i.lastSend = now;
            if (Persist()) {
                i.persistPending = false;
                SendMsg(i.peer, "RECEIPT", i.id, i.amount);
            }
        }
    }
    // forget finished transfers after a while (credited ones stay on disk, see Save)
    size_t outsBefore = outs_.size();
    outs_.erase(std::remove_if(outs_.begin(), outs_.end(), [&](const Outgoing& o) {
        return !o.persistPending && OutFinal(o.st) && o.cancelsLeft <= 0 && now - o.changed > 10 * 60000; }), outs_.end());
    bool save = outs_.size() != outsBefore;
    long long wall = io_.wallclock ? io_.wallclock() : 0;
    size_t before = ins_.size();
    ins_.erase(std::remove_if(ins_.begin(), ins_.end(), [&](const Incoming& i) {
        if (i.st == In::Credited) return wall && i.wall && wall - i.wall > kKeepSettledSec;
        return InFinal(i.st) && i.seen && now - i.changed > 10 * 60000; }), ins_.end());
    TrimIncomingHistory();
    if (save || ins_.size() != before) Persist();
}

std::vector<Outgoing> Engine::Outs() const { return outs_; }
std::vector<Incoming> Engine::Ins() const { return ins_; }
int Engine::PromptCount() const {
    return (int)std::count_if(ins_.begin(), ins_.end(), [](const Incoming& i) { return i.st == In::Prompt; });
}

}   // namespace tx
