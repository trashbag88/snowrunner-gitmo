// Gitmo - player-to-player transfers (stage 3).
//
// There is no bank: every player keeps their own game money. A transfer moves
// money from the sender's account to the receiver's, and only after the
// receiver has accepted it. This file is the protocol only (no Steam, no game
// memory), so the test harness can run it over a simulated, lossy network.
//
//   sender                                   receiver
//   OFFER  ------------------------------->  shows "X wants to send you $N"
//          <-------------------------------  ACK            (prompt is up)
//          <-------------------------------  ACCEPT / DECLINE
//   takes the money from the sender
//   PAY    ------------------------------->  adds it, remembers the id
//          <-------------------------------  RECEIPT        (done)
//
// Every message is repeated until the answer arrives, and every repeat is
// harmless: the receiver credits a transfer id at most once, and only one it
// accepted. If anything goes wrong after the sender paid, the receiver answers
// PAY with VOID ("never credited, never will be") and the sender is refunded.
// A paid transfer the sender hasn't settled is saved to disk and resent next
// time the other player is online.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace tx {

enum class Out { Offered, Asked, Paid, RefundDue, Done, Declined, Expired, Canceled, Unreachable, Refunded, Failed };
enum class In  { Prompt, Accepted, Credited, Declined, Void };

struct Outgoing {
    uint64_t peer = 0, id = 0;
    long long amount = 0;
    std::string name;            // receiver's name
    Out st = Out::Offered;
    uint64_t created = 0, changed = 0, lastSend = 0;
    int cancelsLeft = 0;         // CANCEL repeats still to send
    std::string note;            // reason shown to the player (e.g. "Not in a map")
};

struct Incoming {
    uint64_t peer = 0, id = 0;
    long long amount = 0;
    std::string name;            // sender's name
    In st = In::Prompt;
    uint64_t created = 0, changed = 0, lastSend = 0;
    long long wall = 0;          // unix time, for keeping settled records a while
    bool seen = false;           // the player has seen how it ended
    std::string why;             // how it ended when no money moved ("Bob canceled", "No answer"...)
};

struct Peer {
    uint64_t id = 0;
    std::string name;
    uint64_t lastSeen = 0;       // last message from them (0 = never)
    bool sameGame = false;       // in the same multiplayer session as us (worked out by the Engine)
    bool hasGitmo = false;
    bool direct = false;         // our game session lists them as a member
    std::string session;         // THEIR game session id (their SESS message)
    uint64_t sessionAt = 0;
};

struct IO {
    std::function<void(uint64_t peer, const std::string& msg)> send;
    std::function<bool(long long delta, const std::string& label)> applyMoney;   // false = couldn't (not in a map)
    std::function<long long()> balance;            // < 0: not known right now
    std::function<bool()> mayPay;                  // false: taking money now could let it be spent twice (the sender is in a garage)
    std::function<void()> ring;                    // a new request came in
    std::function<void(const std::string&)> persist;
    std::function<uint64_t()> now;                 // milliseconds, monotonic
    std::function<long long()> wallclock;          // unix seconds
    std::function<uint64_t()> newId;
    std::function<void(const std::string&)> log;
    // a transfer ended without moving money (declined, no answer...): label, amount, why
    std::function<void(const std::string&, long long, const std::string&)> note;
};

// timings (milliseconds)
constexpr uint64_t kResend        = 2000;    // repeat an unanswered message
constexpr uint64_t kPaidResendMax = 10000;   // slower once a payment has been unanswered a while
constexpr uint64_t kReach         = 15000;   // no ACK at all: the receiver can't be reached
constexpr uint64_t kPromptTtl     = 60000;   // receiver: how long the Accept button stays up
constexpr uint64_t kOfferTtl      = 75000;   // sender: gives up waiting for an answer
constexpr uint64_t kPayWait       = 45000;   // receiver: accepted, but no payment came
constexpr uint64_t kOnline        = 16000;   // peer counts as online this long after a message
constexpr uint64_t kHelloEvery    = 5000;
constexpr long long kMaxAmount    = 100000000;   // $100 million per transfer
constexpr long long kMaxBalance   = 2147483647;  // the game keeps money in a 32-bit integer
constexpr long long kKeepSettledSec = 14 * 24 * 3600;

enum class SendResult { Ok, NotInMap, NotEnough, BadAmount, Busy, Offline, Self, NotInGame, InGarage };

class Engine {
public:
    explicit Engine(IO io) : io_(std::move(io)) {}

    void SetSelf(uint64_t me, const std::string& name) { me_ = me; myName_ = name; }
    void Load(const std::string& text);            // saved state from a previous session

    // what the player does
    SendResult Send(uint64_t peer, long long amount, uint64_t* idOut = nullptr);
    bool Cancel(uint64_t id);                      // only before the receiver accepted
    bool Accept(uint64_t peer, uint64_t id);
    bool Decline(uint64_t peer, uint64_t id);
    void MarkSeen(uint64_t peer, uint64_t id);

    // what the network does
    void SetPeer(uint64_t peer, const std::string& name, bool direct);     // a candidate from Steam; direct = in our game session
    void SetMySession(const std::string& id) { mySession_ = id; }         // our game session id ("" = none, e.g. main menu)
    void SetSessionOnly(bool on) { sessionOnly_ = on; }   // only players in our game session can send / be sent to
    bool InGame(uint64_t peer) const;
    void SayHello(uint64_t peer);
    bool Knows(uint64_t peer) const { return peers_.count(peer) != 0; }   // a friend in the game or in our lobby
    void OnMessage(uint64_t from, const std::string& msg);
    void Tick();

    // for the phone
    std::vector<Outgoing> Outs() const;
    std::vector<Incoming> Ins() const;
    std::vector<Peer> OnlinePeers() const;          // peers with Gitmo heard from recently (only our session's, if session-only)
    std::vector<Peer> OnlinePeersAll() const;       // the same, ignoring the session
    const Outgoing* FindOut(uint64_t id) const;
    std::string Save() const;

private:
    void SendMsg(uint64_t peer, const std::string& type, uint64_t id, long long amount, const std::string& extra = "");
    void SetOut(Outgoing& o, Out st, const std::string& note = "");
    void SetIn(Incoming& i, In st, const std::string& why = "");
    void Note(const std::string& label, long long amount, const std::string& why) { if (io_.note) io_.note(label, amount, why); }
    Incoming* FindIn(uint64_t peer, uint64_t id);
    Outgoing* FindOutMut(uint64_t peer, uint64_t id);
    std::string NameOf(uint64_t peer) const;
    void Persist();
    void Logf(const char* fmt, ...) const;

    IO io_;
    uint64_t me_ = 0;
    std::string myName_;
    std::vector<Outgoing> outs_;
    std::vector<Incoming> ins_;
    std::map<uint64_t, Peer> peers_;
    std::map<uint64_t, uint64_t> lastHello_;
    std::string mySession_;
    bool sessionOnly_ = false;
    bool InGame(const Peer& p) const;
};

const char* OutText(Out s);
bool OutFinal(Out s);
bool InFinal(In s);

}   // namespace tx
