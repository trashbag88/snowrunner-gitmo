// Protocol test for Gitmo transfers: simulated players over a lossy network.
#include "transfer.h"
#include "shared.h"
#include <cstdio>
#include <memory>
#include <set>
#include <random>
#include <deque>
#include <algorithm>

using namespace tx;

static uint64_t g_now = 1000000;
static std::mt19937_64 rng(1);
static int fails = 0;
static bool quiet = true;

struct Packet { uint64_t at, from, to; std::string msg; };
struct Net {
    double loss = 0, dup = 0; uint64_t maxDelay = 50;
    std::string drop;   // drop every message of this type (e.g. "PAY")
    std::vector<Packet> q;
    void Send(uint64_t from, uint64_t to, const std::string& m) {
        if (!drop.empty() && m.find("|" + drop + "|") != std::string::npos) return;
        std::uniform_real_distribution<double> u(0, 1);
        if (u(rng) < loss) return;
        int n = u(rng) < dup ? 2 : 1;
        for (int k = 0; k < n; k++) q.push_back({ g_now + 1 + rng() % maxDelay, from, to, m });
    }
} net;

struct Player {
    uint64_t sid; std::string name;
    long long money; bool inMap = true; bool failWrites = false; bool failPersistNext = false;
    int failPersistAfter = -1; bool inGarage = false;
    std::string disk;
    std::unique_ptr<Engine> e;
    std::set<uint64_t> acceptedIds, creditedIds;   // harness bookkeeping (truth)
    void Boot();
};
std::vector<Player*> players;
static uint64_t g_idc = 0x1000;

void Player::Boot() {
    IO io;
    io.send = [this](uint64_t to, const std::string& m) { net.Send(sid, to, m); };
    io.applyMoney = [this](long long d, const std::string& label) {
        if (!inMap || failWrites || money + d < 0) return false;
        money += d;
        if (!quiet) printf("    [%s] %+lld (%s) -> %lld\n", name.c_str(), d, label.c_str(), money);
        return true;
    };
    io.balance = [this]() { return inMap ? money : -1LL; };
    io.mayPay = [this]() { return !inGarage; };
    io.ring = [] {};
    io.persist = [this](const std::string& s) {
        if (failPersistNext) { failPersistNext = false; return false; }
        if (failPersistAfter == 0) { failPersistAfter = -1; return false; }
        if (failPersistAfter > 0) --failPersistAfter;
        disk = s;
        return true;
    };
    io.now = [] { return g_now; };
    io.wallclock = [] { return (long long)(1700000000 + g_now / 1000); };
    io.newId = [] { return ++g_idc * 0x9E3779B97F4A7C15ull | 1; };
    io.log = [this](const std::string& s) { if (!quiet) printf("  %s: %s\n", name.c_str(), s.c_str()); };
    e.reset(new Engine(io));
    e->SetSelf(sid, name);
    e->Load(disk);
    for (auto* p : players) if (p != this) e->SetPeer(p->sid, p->name, true);
}

static void Step(uint64_t ms) {   // advance time in 10 ms steps, delivering packets
    uint64_t end = g_now + ms;
    while (g_now < end) {
        g_now += 10;
        std::stable_sort(net.q.begin(), net.q.end(), [](const Packet& a, const Packet& b) { return a.at < b.at; });
        std::vector<Packet> due;
        while (!net.q.empty() && net.q.front().at <= g_now) { due.push_back(net.q.front()); net.q.erase(net.q.begin()); }
        for (auto& pk : due) for (auto* p : players) if (p->sid == pk.to && p->e) p->e->OnMessage(pk.from, pk.msg);
        for (auto* p : players) {
            if (!p->e) continue;
            if (g_now % kHelloEvery == 0) for (auto* o : players) if (o != p) p->e->SayHello(o->sid);
            p->e->Tick();
        }
    }
}

static void Check(const char* what, bool ok) {
    printf("%-70s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static long long Total() { long long t = 0; for (auto* p : players) t += p->money; return t; }
static bool Settled() {
    for (auto* p : players) for (auto& o : p->e->Outs()) if (!OutFinal(o.st)) return false;
    for (auto* p : players) for (auto& i : p->e->Ins()) if (i.st == In::Prompt || i.st == In::Accepted) return false;
    return true;
}
static const Incoming* PromptOf(Player& p) {
    static Incoming c;
    for (auto& i : p.e->Ins()) if (i.st == In::Prompt) { c = i; return &c; }
    return nullptr;
}
static Out StateOf(Player& p, uint64_t id) { const Outgoing* o = p.e->FindOut(id); return o ? o->st : Out::Failed; }

int main(int argc, char**) {
    quiet = argc < 2;
    std::string seed;
    if (gitmo_shared::GetEnvironmentVariable("SEED", seed)) rng.seed(strtoull(seed.c_str(), nullptr, 10));
    Player A{ 76561198000000001ull, "Ann", 10000 }, B{ 76561198000000002ull, "Bob", 500 }, C{ 76561198000000003ull, "Cat", 0 };
    players = { &A, &B, &C };
    for (auto* p : players) p->Boot();
    Step(6000);   // hellos
    Check("1. everyone sees the other two online", A.e->OnlinePeers().size() == 2 && B.e->OnlinePeers().size() == 2);

    uint64_t id = 0;
    // --- 2. happy path
    Check("2a. send $1,000 to Bob", A.e->Send(B.sid, 1000, &id) == SendResult::Ok);
    Step(300);
    Check("2b. Bob sees the request, nothing moved yet",
          PromptOf(B) && PromptOf(B)->amount == 1000 && B.e->PromptCount() == 1 && A.money == 10000 && B.money == 500);
    Check("2c. Ann's phone says waiting (Asked)", StateOf(A, id) == Out::Asked);
    B.e->Accept(A.sid, PromptOf(B)->id);
    Step(500);
    Check("2d. accepted: Ann 9,000, Bob 1,500, done", A.money == 9000 && B.money == 1500 && StateOf(A, id) == Out::Done);

    // --- 3. decline
    A.e->Send(B.sid, 200, &id); Step(300);
    B.e->Decline(A.sid, PromptOf(B)->id); Step(300);
    Check("3. declined: nothing moved, sender sees Declined", A.money == 9000 && B.money == 1500 && StateOf(A, id) == Out::Declined);

    // --- 4. no answer
    A.e->Send(B.sid, 200, &id); Step(80000);
    Check("4. ignored for 80 s: expired, nothing moved", A.money == 9000 && B.money == 1500 && StateOf(A, id) == Out::Expired && !PromptOf(B));

    // --- 5. sender cancels before the answer; a late Accept does nothing
    A.e->Send(B.sid, 300, &id); Step(300);
    uint64_t bid = PromptOf(B)->id;
    A.e->Cancel(id); Step(300);
    Check("5a. cancel removes the request on Bob's phone", !PromptOf(B) && StateOf(A, id) == Out::Canceled);
    Check("5b. Bob can't accept it any more", !B.e->Accept(A.sid, bid));
    Step(3000);
    Check("5c. nothing moved", A.money == 9000 && B.money == 1500);

    // --- 6. receiver not in a map: turned down automatically
    B.inMap = false;
    A.e->Send(B.sid, 100, &id); Step(500);
    Check("6. receiver not in a map: auto-declined, nothing moved", StateOf(A, id) == Out::Declined && A.money == 9000 && !PromptOf(B));
    B.inMap = true;

    // --- 7. not enough money
    Check("7. can't send more than the balance", A.e->Send(B.sid, 9001) == SendResult::NotEnough);

    // --- 8. sender spends the money while the request is open
    A.e->Send(B.sid, 5000, &id); Step(300);
    A.money = 100;   // bought an upgrade meanwhile
    B.e->Accept(A.sid, PromptOf(B)->id); Step(1000);
    Check("8. sender can't pay any more: fails, nothing moved, Bob's side void", StateOf(A, id) == Out::Failed && A.money == 100 && B.money == 1500);
    A.money = 9000;

    // --- 9. receiver leaves the map between Accept and the payment -> refund
    A.e->Send(B.sid, 700, &id); Step(300);
    bid = PromptOf(B)->id;
    net.loss = 1.0;                       // the ACCEPT gets out, then the line goes quiet
    B.e->Accept(A.sid, bid);
    net.loss = 0; net.q.clear();
    B.inMap = false;                      // the accept is resent; Ann pays; Bob can't take it
    Step(3000);
    Check("9. receiver couldn't take it: Ann refunded, Bob unchanged", StateOf(A, id) == Out::Refunded && A.money == 9000 && B.money == 1500);
    B.inMap = true;

    // --- 10. receiver's game restarts after accepting (memory lost) -> refund, never credited later
    A.e->Send(B.sid, 400, &id); Step(300);
    B.e->Accept(A.sid, PromptOf(B)->id);
    net.loss = 1.0; Step(2500); net.loss = 0; net.q.clear();   // Ann got nothing yet... or did he? both cases:
    B.Boot();                                                 // Bob's game restarts
    Step(6000);
    Check("10. Bob restarted before paying: settled with no money moved", Settled() && A.money + B.money == 10500);

    // --- 11. sender restarts while a payment is unanswered: still settles
    A.e->Send(B.sid, 250, &id); Step(300);
    B.e->Accept(A.sid, PromptOf(B)->id);
    net.drop = "PAY"; Step(500);   // ACCEPT arrives, Ann pays (and saves it); every PAY is lost ...
    net.drop.clear(); net.q.clear();
    bool paidBeforeRestart = A.money == 10500 - B.money - 250;   // the $250 left Ann but hasn't reached Bob
    A.Boot(); Step(8000);
    printf("    [11] paid before restart: %s; after: Ann %lld, Bob %lld, transfer %s\n", paidBeforeRestart ? "yes" : "no",
           A.money, B.money, OutText(StateOf(A, id)));
    Check("11a. the payment really was in flight when Ann restarted", paidBeforeRestart);
    Check("11b. Ann restarted mid-payment: settled once, total kept", Settled() && A.money + B.money == 10500);

    // --- 12. Bob's wallet would overflow
    B.money = 2147483000;
    A.e->Send(B.sid, 1000, &id); Step(500);
    Check("12. receiver's balance would pass the game's limit: declined", StateOf(A, id) == Out::Declined);
    B.money = 10500 - A.money;

    // --- 13. three players, heavy loss + duplicates + reordering, random actions and restarts
    std::uniform_int_distribution<int> pick(0, 2);
    net.loss = 0.35; net.dup = 0.3; net.maxDelay = 1500;
    long long total0 = Total();
    std::map<std::pair<uint64_t, uint64_t>, int> creditCount;   // (receiver, id) -> times credited
    int sends = 0, accepts = 0, restarts = 0, credits = 0;
    for (int round = 0; round < 3000; round++) {
        Player& s = *players[pick(rng)];
        Player& r = *players[pick(rng)];
        int act = (int)(rng() % 10);
        if (act < 3 && &s != &r) { if (s.e->Send(r.sid, 1 + (long long)(rng() % 2000)) == SendResult::Ok) sends++; }
        else if (act < 6) { if (auto* p = PromptOf(r)) { if (rng() % 4) { if (r.e->Accept(p->peer, p->id)) accepts++; } else r.e->Decline(p->peer, p->id); } }
        else if (act == 6 && rng() % 20 == 0) { r.Boot(); restarts++; }
        else if (act == 7 && rng() % 10 == 0) { r.inMap = !r.inMap; }
        else if (act == 8) { for (auto& o : s.e->Outs()) if (o.st == Out::Offered && rng() % 5 == 0) s.e->Cancel(o.id); }
        Step(200 + rng() % 800);
        // count credits from each player's view of its own Credited records (by id) -- use money moves instead below
    }
    for (auto* p : players) p->inMap = true;
    net.loss = 0; net.dup = 0; net.maxDelay = 50;
    Step(200000);
    long long paidOut = 0;
    for (auto* p : players) for (auto& o : p->e->Outs()) if (o.st == Out::Paid || o.st == Out::RefundDue) paidOut += o.amount;
    for (auto* p : players) for (auto& i : p->e->Ins()) if (i.st == In::Credited) credits++;
    printf("    (%d sends, %d accepts, %d restarts, %d credited; balances %lld / %lld / %lld)\n", sends, accepts, restarts, credits, A.money, B.money, C.money);
    Check("13a. everything settled once the network is quiet", Settled() && paidOut == 0);
    Check("13b. no money created or lost across 3,000 random rounds", Total() == total0);
    Check("13c. nobody went negative", A.money >= 0 && B.money >= 0 && C.money >= 0);
    Check("13d. some transfers really happened", credits >= 3);

    // --- 14. saved state round-trips
    {
        Player& p = A;
        std::string d = p.e->Save();
        p.Boot();
        Check("14. saved transfer state reloads identically", p.e->Save() == d);
    }
    // --- 15. only players in the same game session (Ann hosts session S1; Bob and Cat join)
    {
        // each Gitmo knows its own session id; "direct" = our session's member list names them
        auto setSess = [](Player& p, const char* sid, std::vector<Player*> direct) {
            for (auto* o : players) if (o != &p)
                p.e->SetPeer(o->sid, o->name, std::find(direct.begin(), direct.end(), o) != direct.end());
            p.e->SetMySession(sid);
            p.e->SetSessionOnly(true);
        };
        auto sees = [](Player& p, Player& o) { for (auto& x : p.e->OnlinePeers()) if (x.id == o.sid) return x.sameGame; return false; };
        // all in the main menu: nobody is in anyone's game
        setSess(A, "", {}); setSess(B, "", {}); setSess(C, "", {});
        Step(6000);
        Check("15a. in the main menu nobody is listed", A.e->OnlinePeers().empty() && B.e->OnlinePeers().empty() && C.e->OnlinePeers().empty());
        // together: only the session id is shared (no member lists), so this tests SESS
        setSess(A, "S1", {}); setSess(B, "S1", {}); setSess(C, "S1", {});
        Step(6000);
        Check("15b. same session id: everyone sees everyone", sees(A, B) && sees(A, C) && sees(B, C) && sees(C, A));
        long long a0 = A.money, b0 = B.money, c0 = C.money;
        A.money = 5000; B.money = 5000; C.money = 5000;
        uint64_t sid = 0;
        Check("15c. guest can send to the other guest", B.e->Send(C.sid, 100, &sid) == SendResult::Ok);
        Step(300); if (auto* pr = PromptOf(C)) C.e->Accept(B.sid, pr->id); Step(500);
        Check("15d. ... and it arrives", B.money == 4900 && C.money == 5100 && StateOf(B, sid) == Out::Done);
        // Cat leaves for her own game (a session of her own)
        setSess(C, "S2", {});
        Step(6000);
        Check("15e. after Cat leaves, nobody lists her and she lists nobody", !sees(A, C) && !sees(B, C) && C.e->OnlinePeers().empty()
              && A.e->OnlinePeers().size() == 1 && B.e->OnlinePeers().size() == 1);
        Check("15f. sending to her is refused (not in your game)", B.e->Send(C.sid, 100) == SendResult::NotInGame);
        C.e->SetSessionOnly(false);
        Check("15g. Cat (session check off) can still offer", C.e->Send(B.sid, 100, &sid) == SendResult::Ok);
        Step(1000);
        Check("15h. Bob's Gitmo declines it automatically, nothing moved", StateOf(C, sid) == Out::Declined && !PromptOf(B)
              && B.money == 4900 && C.money == 5100);
        // Cat joins again, but her Gitmo doesn't share its session (older version): Ann's member list still finds her
        setSess(C, "", {}); setSess(A, "S1", { &C });
        Step(6000);
        Check("15i. the host's member list alone is enough", sees(A, C) && A.e->Send(C.sid, 1) == SendResult::Ok);
        Step(80000);
        Check("15j. all settled", Settled());
        A.money = a0; B.money = b0; C.money = c0;
    }
    {   // 16. the sender is in a garage when the receiver accepts: nothing may leave (its screens
        //     still show the old balance, so the money could be spent twice)
        A.money = 5000; B.money = 5000;
        uint64_t sid = 0;
        A.inGarage = true;
        Check("16. sending while already in a garage is refused", A.e->Send(B.sid, 1000) == SendResult::InGarage);
        A.inGarage = false;
        Check("16a. Ann sends outside the garage", A.e->Send(B.sid, 1000, &sid) == SendResult::Ok);
        Step(300);
        A.inGarage = true;   // ...then drives into a garage before Bob answers
        if (auto* pr = PromptOf(B)) B.e->Accept(A.sid, pr->id);
        Step(3000);
        Check("16b. accepted while Ann is in a garage: failed, nothing moved", StateOf(A, sid) == Out::Failed && A.money == 5000 && B.money == 5000);
        A.inGarage = false;
        Check("16c. outside again, a new send works", A.e->Send(B.sid, 1000, &sid) == SendResult::Ok);
        Step(300); if (auto* pr = PromptOf(B)) B.e->Accept(A.sid, pr->id); Step(1000);
        Check("16d. ... and arrives", A.money == 4000 && B.money == 6000 && StateOf(A, sid) == Out::Done);
        Step(80000);
        Check("16e. all settled", Settled());
    }
    // --- 17. a failed transfer-state write must not leave an untracked money change
    {
        long long oldA = A.money, oldB = B.money;
        A.money = B.money = 5000;
        uint64_t sid = 0;
        Check("17a. start a transfer whose sender state write will fail", A.e->Send(B.sid, 600, &sid) == SendResult::Ok);
        Step(300);
        A.failPersistNext = true;
        if (auto* pr = PromptOf(B)) B.e->Accept(A.sid, pr->id);
        Step(1000);
        Check("17b. failed sender persistence rolls the debit back", StateOf(A, sid) == Out::Failed && A.money == 5000 && B.money == 5000);

        Check("17c. start a transfer whose receiver state write will fail once", A.e->Send(B.sid, 700, &sid) == SendResult::Ok);
        Step(300);
        B.failPersistNext = true;
        if (auto* pr = PromptOf(B)) B.e->Accept(A.sid, pr->id);
        Step(6000);
        Check("17d. receiver retries after rolling back an unsaved credit", StateOf(A, sid) == Out::Done
              && A.money == 4300 && B.money == 5700);
        A.money = oldA; B.money = oldB;
    }
    // --- 18. a failed refund-state write cannot leave a refund applied but untracked
    {
        long long oldA = A.money, oldB = B.money;
        A.money = B.money = 5000;
        uint64_t sid = 0;
        Check("18a. start a transfer whose receiver cannot credit", A.e->Send(B.sid, 500, &sid) == SendResult::Ok);
        Step(300);
        net.drop = "VOID";
        B.failWrites = true;
        if (auto* pr = PromptOf(B)) B.e->Accept(A.sid, pr->id);
        Step(500);
        net.drop.clear(); net.q.clear();
        B.failWrites = false;
        char idText[32];
        sprintf_s(idText, "%llx", (unsigned long long)sid);
        A.failPersistAfter = 1;
        A.e->OnMessage(B.sid, std::string("G1|VOID|") + idText + "|500|");
        Check("18b. unsaved refund is rolled back and remains due", StateOf(A, sid) == Out::RefundDue
              && A.money == 4500 && B.money == 5000);
        Step(5000);
        Check("18c. refund retries and settles exactly once", StateOf(A, sid) == Out::Refunded
              && A.money == 5000 && B.money == 5000);
        A.money = oldA; B.money = oldB;
    }
    // --- 19. malformed fields cannot be partially parsed into protocol state
    {
        size_t inBefore = A.e->Ins().size(), outBefore = A.e->Outs().size();
        A.e->OnMessage(B.sid, "G1|OFFER|12zz|10|Bob");
        A.e->OnMessage(B.sid, "G1|OFFER|1234|10junk|Bob");
        A.e->OnMessage(B.sid, "G1|UNKNOWN|1234|10|Bob");
        A.e->Load("OUT 2 12zz 10 P Bob\nOUT 2 1234 999999999 P Bob\nIN 2 1234 10 nope Bob\n");
        Check("19. malformed network and saved-state fields are ignored",
              A.e->Ins().size() == inBefore && A.e->Outs().size() == outBefore);
    }
    // --- 20. unread canceled requests stay bounded
    {
        for (uint64_t n = 0; n < 300; ++n) {
            char idText[32];
            sprintf_s(idText, "%llx", (unsigned long long)(0x500000 + n));
            std::string offer = std::string("G1|OFFER|") + idText + "|1|Bob";
            std::string cancel = std::string("G1|CANCEL|") + idText + "|1|";
            A.e->OnMessage(B.sid, offer);
            A.e->OnMessage(B.sid, cancel);
            A.e->Tick();
        }
        size_t unseenFinal = 0;
        for (const auto& i : A.e->Ins())
            if (i.st != In::Credited && InFinal(i.st) && !i.seen) ++unseenFinal;
        Check("20. unread terminal request history is capped", unseenFinal <= 256);
    }
    printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL PASSED");
    return fails;
}
