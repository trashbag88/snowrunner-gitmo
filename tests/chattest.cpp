// SMS engine test: simulated players over a lossy network.
#include "chat.h"
#include <cstdio>
#include <memory>
#include <random>
#include <set>
#include <map>
#include <algorithm>
using namespace sms;

static uint64_t g_now = 1000000;
static std::mt19937_64 rng(1);
static int fails = 0;
static void Check(const char* what, bool ok) { printf("%-70s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }

struct Packet { uint64_t at, from, to; std::string msg; };
struct Net {
    double loss = 0, dup = 0; uint64_t maxDelay = 50;
    std::vector<Packet> q;
    std::set<uint64_t> offline;   // players whose game is closed: their packets vanish
    void Send(uint64_t from, uint64_t to, const std::string& m) {
        if (offline.count(from) || offline.count(to)) return;
        std::uniform_real_distribution<double> u(0, 1);
        if (u(rng) < loss) return;
        int n = u(rng) < dup ? 2 : 1;
        for (int k = 0; k < n; k++) q.push_back({ g_now + 1 + rng() % maxDelay, from, to, m });
    }
} net;

struct Player {
    uint64_t sid; std::string name; std::string disk; int rings = 0;
    std::map<uint64_t, std::string> photos;   // the "Gitmo-photos" folder
    std::unique_ptr<Engine> e;
    void Boot() {
        IO io;
        io.storePhoto = [this](uint64_t id, const std::string& j) { photos[id] = j; return true; };
        io.loadPhoto = [this](uint64_t id) { auto it = photos.find(id); return it == photos.end() ? std::string() : it->second; };
        io.deletePhoto = [this](uint64_t id) { photos.erase(id); };
        io.send = [this](uint64_t to, const std::string& m) { net.Send(sid, to, m); };
        io.now = [] { return g_now; };
        io.wallclock = [] { return (long long)(1700000000 + g_now / 1000); };
        io.newId = [] { static uint64_t c = 0x100; return ++c * 0x9E3779B97F4A7C15ull | 1; };
        io.persist = [this](const std::string& s) { disk = s; };
        io.ring = [this](uint64_t, const std::string&, const std::string&) { rings++; };
        e.reset(new Engine(io));
        e->Load(disk);
    }
};
std::vector<Player*> players;

static void Presence() {   // what net.cpp does every few seconds
    for (auto* p : players) for (auto* o : players) if (p != o && p->e)
        p->e->SetPresence(o->sid, o->name, !net.offline.count(o->sid) && !net.offline.count(p->sid), false);
}
static void Step(uint64_t ms) {
    uint64_t end = g_now + ms;
    while (g_now < end) {
        g_now += 10;
        std::stable_sort(net.q.begin(), net.q.end(), [](const Packet& a, const Packet& b) { return a.at < b.at; });
        std::vector<Packet> due;
        while (!net.q.empty() && net.q.front().at <= g_now) { due.push_back(net.q.front()); net.q.erase(net.q.begin()); }
        for (auto& pk : due) for (auto* p : players) if (p->sid == pk.to && p->e && !net.offline.count(p->sid)) p->e->OnMessage(pk.from, pk.msg);
        if (g_now % 5000 == 0) Presence();
        for (auto* p : players) if (p->e && !net.offline.count(p->sid)) p->e->Tick();
    }
}
static const Contact* C(Player& p, Player& o) { auto it = p.e->Contacts().find(o.sid); return it == p.e->Contacts().end() ? nullptr : &it->second; }
static std::vector<std::string> Texts(Player& p, Player& o, bool mine) {
    std::vector<std::string> v; if (auto* c = C(p, o)) for (auto& m : c->msgs) if (m.mine == mine) v.push_back(m.text); return v;
}

int main() {
    Player A{ 1001, "Ann" }, B{ 1002, "Bob" }, Cc{ 1003, "Cat" };
    players = { &A, &B, &Cc };
    for (auto* p : players) p->Boot();
    Presence(); Step(100);
    Check("1. everyone is a contact, online", C(A, B) && C(A, B)->online && C(B, A) && C(B, A)->online);

    A.e->Send(B.sid, "hey Bob");
    Step(300);
    Check("2a. Bob got it once, unread 1, rang", Texts(B, A, false) == std::vector<std::string>{ "hey Bob" } && C(B, A)->unread == 1 && B.rings == 1);
    Check("2b. Ann sees Delivered", C(A, B)->msgs.back().st == St::Delivered);
    B.e->SetViewing(A.sid); Step(300);
    Check("2c. Bob opened the chat: unread 0, Ann sees Read", C(B, A)->unread == 0 && C(A, B)->msgs.back().st == St::Read);
    B.e->Send(A.sid, "hi | pipes | and\ttabs\nnewline");
    Step(300);
    Check("2d. text with | and control chars arrives cleaned", Texts(A, B, false).size() == 1 && Texts(A, B, false)[0] == "hi | pipes | and tabs newline");
    Check("2e. Bob is viewing Ann: Bob's own reply doesn't count as unread anywhere", C(B, A)->unread == 0 && C(A, B)->unread == 1);
    B.e->SetViewing(0);

    // 3. offline: Bob's game is closed; Ann writes 3 messages, they wait
    net.offline.insert(B.sid); Presence();
    A.e->Send(B.sid, "one"); A.e->Send(B.sid, "two"); A.e->Send(B.sid, "three");
    Step(20000);
    int waiting = 0; for (auto& m : C(A, B)->msgs) if (m.mine && m.st == St::Waiting) waiting++;
    Check("3a. Bob offline: 3 messages waiting, contact shown offline", waiting == 3 && !C(A, B)->online);
    // Bob comes back after a restart (from his saved history)
    B.Boot(); net.offline.erase(B.sid);
    Step(12000);
    auto bt = Texts(B, A, false);
    Check("3b. after Bob is back: all 3 arrive, in order, once", bt.size() == 4 && bt[1] == "one" && bt[2] == "two" && bt[3] == "three");
    waiting = 0; for (auto& m : C(A, B)->msgs) if (m.mine && m.st == St::Waiting) waiting++;
    Check("3c. Ann's copies are Delivered", waiting == 0);
    Check("3d. Bob kept the earlier history over the restart", C(B, A) && C(B, A)->msgs.size() == 5);

    // 4. lossy, duplicating network, both directions, many messages
    net.loss = 0.3; net.dup = 0.3; net.maxDelay = 400;
    for (int i = 0; i < 40; i++) {
        char t[32]; sprintf(t, "a%d", i); A.e->Send(B.sid, t);
        sprintf(t, "b%d", i); B.e->Send(A.sid, t);
        Step(150);
    }
    net.loss = 0; net.dup = 0; net.maxDelay = 50;
    Step(30000);
    auto ab = Texts(B, A, false), ba = Texts(A, B, false);
    std::set<std::string> sab(ab.begin(), ab.end()), sba(ba.begin(), ba.end());
    int okA = 0, okB = 0; for (int i = 0; i < 40; i++) { okA += sab.count("a" + std::to_string(i)); okB += sba.count("b" + std::to_string(i)); }
    Check("4a. lossy: every message arrived", okA == 40 && okB == 40);
    Check("4b. lossy: none twice", sab.size() == ab.size() && sba.size() == ba.size());
    bool allDel = true; for (auto& m : C(A, B)->msgs) if (m.mine && m.st == St::Waiting) allDel = false;
    Check("4c. lossy: all of Ann's are acknowledged", allDel);

    // 5. saved state round-trips
    std::string d = A.disk; A.Boot();
    Check("5. history reloads identically", A.e->Save() == d);

    // 6. text cleaning
    Check("6a. empty / spaces only is refused", !A.e->Send(B.sid, "   ") && !A.e->Send(B.sid, ""));
    std::string longText(400, 'x'); longText += "\xC3\xA9";
    Check("6b. long text cut to 300 bytes", Engine::CleanText(longText).size() == 300);
    std::string u8 = std::string(299, 'y') + "\xC3\xA9";   // a 2-byte char straddling the limit
    Check("6c. never cut inside a UTF-8 character", Engine::CleanText(u8) == std::string(299, 'y'));
    Check("6d. stray bytes removed", Engine::CleanText("ok\xFF\xC3go") == "okgo");

    // 7. a third player: separate conversations
    Cc.e->Send(A.sid, "from cat");
    Step(500);
    Check("7. Cat's message lands in Cat's conversation only", Texts(A, Cc, false) == std::vector<std::string>{ "from cat" } && Texts(B, Cc, false).empty());
    Check("8. unread total", A.e->Unread() == C(A, Cc)->unread + C(A, B)->unread);

    // 10. photos
    auto Blob = [](size_t n, int seed) { std::string s(n, 0); std::mt19937 r(seed); for (auto& ch : s) ch = (char)(r() & 0xFF); return s; };
    auto PhotosOf = [](Player& p, Player& o) { std::vector<const Msg*> v; if (auto* c = C(p, o)) for (auto& m : c->msgs) if (m.photo) v.push_back(&m); return v; };
    {
        std::string pic = Blob(150000, 7);   // 4 pieces
        int ringsBefore = B.rings;
        Check("10a. a photo is accepted for sending", A.e->SendPhoto(B.sid, pic));
        Step(1000);
        auto bp = PhotosOf(B, A);
        Check("10b. Bob has it once, identical bytes, it rang", bp.size() == 1 && B.photos.count(bp[0]->id) && B.photos[bp[0]->id] == pic && B.rings == ringsBefore + 1);
        auto ap = PhotosOf(A, B);
        Check("10c. Ann sees Delivered", ap.size() == 1 && ap.back()->st == St::Delivered);
        Check("10d. too big or empty is refused", !A.e->SendPhoto(B.sid, std::string(kMaxPhoto + 1, 'x')) && !A.e->SendPhoto(B.sid, ""));

        // offline: waits, then goes
        net.offline.insert(B.sid); Presence();
        std::string pic2 = Blob(60000, 8);
        A.e->SendPhoto(B.sid, pic2);
        Step(40000);
        Check("10e. Bob offline: the photo waits", PhotosOf(A, B).back()->st == St::Waiting && PhotosOf(B, A).size() == 1);
        net.offline.erase(B.sid); Presence(); Step(8000);
        bp = PhotosOf(B, A);
        Check("10f. Bob back: it arrives", bp.size() == 2 && B.photos[bp[1]->id] == pic2 && PhotosOf(A, B).back()->st == St::Delivered);

        // lossy, duplicating network: three photos each way
        net.loss = 0.3; net.dup = 0.3; net.maxDelay = 400;
        std::vector<std::string> fromA, fromB;
        for (int i = 0; i < 3; i++) {
            fromA.push_back(Blob(100000 + i * 30000, 20 + i)); A.e->SendPhoto(B.sid, fromA.back());
            fromB.push_back(Blob(90000 + i * 20000, 40 + i)); B.e->SendPhoto(A.sid, fromB.back());
            Step(300);
        }
        Step(300000);   // ten resend rounds
        net.loss = 0; net.dup = 0; net.maxDelay = 50;
        Step(40000);
        bp = PhotosOf(B, A); ap = PhotosOf(A, B);
        int gotA = 0, gotB = 0;
        for (auto& s : fromA) for (auto* m : bp) if (!m->mine && B.photos[m->id] == s) gotA++;
        for (auto& s : fromB) for (auto* m : ap) if (!m->mine && A.photos[m->id] == s) gotB++;
        printf("     (gotA %d gotB %d, Bob's chat has %zu photos, Ann's %zu)\n", gotA, gotB, bp.size(), ap.size());
        // each side: its own 3 (Ann: 2 earlier + 3) plus the other's (Bob: 2 earlier + 3)
        Check("10g. lossy: all 6 photos arrived intact, once each", gotA == 3 && gotB == 3 && bp.size() == 2 + 3 + 3 && ap.size() == 2 + 3 + 3);
        bool allDel = true; for (auto* m : ap) if (m->mine && m->st == St::Waiting) allDel = false;
        Check("10h. lossy: all of Ann's photos acknowledged", allDel);

        // history and files
        std::string d = B.disk; B.Boot();
        Check("10i. reload keeps photos as photos", PhotosOf(B, A).size() == bp.size() && B.e->Save() == d);
        A.e->OnMessage(B.sid, "C1|PCH|abc|1|0|99999999|xx");
        Check("10j. a piece claiming a huge photo is ignored", PhotosOf(A, B).size() == ap.size());
        size_t filesBefore = A.photos.size();
        A.e->Forget(Cc.sid);
        Check("10k. photo files are kept until erased", A.photos.size() == filesBefore && filesBefore >= 8);
    }

    // 9. erase: Cat goes offline (a previous contact), Ann has a message waiting for her, then erases everything
    net.offline.insert(Cc.sid); Presence();
    A.e->Send(Cc.sid, "unsent to cat");
    Step(6000);
    size_t bobHad = C(B, A)->msgs.size();
    // a repeat of Bob's last message arrives late, after the erase
    uint64_t lastBobId = 0; for (auto& m : C(A, B)->msgs) if (!m.mine) lastBobId = m.id;
    A.e->EraseAll();
    Check("9a. erase: Bob (online) stays listed with no messages, unread 0", C(A, B) && C(A, B)->msgs.empty() && C(A, B)->unread == 0);
    Check("9b. erase: Cat (offline, previous contact) is gone, unsent message too", C(A, Cc) == nullptr && A.e->Unread() == 0);
    Check("9c. erase: saved to disk", A.disk.find("unsent to cat") == std::string::npos && A.disk.find("hey Bob") == std::string::npos);
    Check("9c2. erase: Ann's photo files are deleted too", A.photos.empty());
    char rep[64]; sprintf(rep, "C1|MSG|%llx|1700000000000|b39", (unsigned long long)lastBobId);
    A.e->OnMessage(B.sid, rep);
    Check("9d. erase: a late repeat of an erased message doesn't come back", C(A, B)->msgs.empty());
    Check("9e. erase: Bob's copy is untouched", C(B, A)->msgs.size() == bobHad);
    B.e->Send(A.sid, "after erase"); Step(500);
    Check("9f. erase: new messages still arrive and are kept", Texts(A, B, false) == std::vector<std::string>{ "after erase" });
    A.Boot();
    Check("9g. erase: survives a restart", Texts(A, B, false) == std::vector<std::string>{ "after erase" } && C(A, Cc) == nullptr);
    net.offline.erase(Cc.sid); Presence(); Step(6000);
    Check("9h. erase: Cat online again shows as a contact with nothing in it", C(A, Cc) && C(A, Cc)->online && C(A, Cc)->msgs.empty());

    printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL PASSED");
    return fails;
}
