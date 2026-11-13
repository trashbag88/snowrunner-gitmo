// Gitmo - SMS: text messages and photos between players who have Gitmo.
//
// No server: a message goes straight to the other player's Gitmo over the same Steam
// channel as transfers. If they're offline it waits on the sender's PC and goes out as
// soon as they're online again. Every message has an id; the receiver keeps each id once
// and always answers ACK (so the sender can stop repeating it), and READ when the player
// has seen the conversation.
//
//   C1|MSG|<idhex>|<order key>|<text>                    a message (key = sender's unix time x1000 + counter; text may contain '|')
//   C1|PCH|<idhex>|<order key>|<offset>|<total>|<bytes>   one piece (up to kChunk bytes) of a photo (a JPEG of <total> bytes)
//   C1|ACK|<idhex>|0|                                    received (for a photo: every piece)
//   C1|READ|<idhex>|0|                                   seen, up to and including this message
//
// A photo is sent as all its pieces at once, and again every kPhotoResend while it isn't
// acknowledged; the receiver keeps pieces from earlier tries, so each try only has to fill
// the gaps. Photos are kept as files by the IO callbacks (named by message id).
//
// Pure logic (no Steam, no UI), so the test harness can run it over a lossy network.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>

namespace sms {

enum class St { Waiting, Delivered, Read, Received };   // Received = theirs

struct Msg {
    uint64_t id = 0;
    bool mine = false;
    long long wall = 0;          // unix seconds (sender's clock)
    uint64_t key = 0;            // sender's order: unix ms-ish stamp, rising with every message it sends
    std::string text;            // (a photo: kPhotoText)
    bool photo = false;          // a picture: its JPEG is kept by IO::storePhoto / loadPhoto under this id
    St st = St::Waiting;
    uint64_t lastSend = 0;       // ms, for repeats (not saved)
};

struct Contact {
    uint64_t id = 0;
    std::string name;
    long long lastSeenWall = 0;  // last time they were online (unix seconds)
    bool online = false;         // has Gitmo and is reachable right now (not saved)
    bool inGame = false;         // in our game session (not saved)
    int unread = 0;
    uint64_t readId = 0;         // their latest message we've told them we read
    bool readPending = false;    // READ still to send
    std::vector<Msg> msgs;       // oldest first
};

struct IO {
    std::function<void(uint64_t peer, const std::string& msg)> send;
    std::function<uint64_t()> now;                 // milliseconds, monotonic
    std::function<long long()> wallclock;          // unix seconds
    std::function<uint64_t()> newId;
    std::function<void(const std::string&)> persist;
    std::function<void(const std::string&)> log;
    std::function<void(uint64_t peer, const std::string& name, const std::string& text)> ring;   // a new message came in
    std::function<bool(uint64_t id, const std::string& jpeg)> storePhoto;   // keep a photo (false: couldn't)
    std::function<std::string(uint64_t id)> loadPhoto;                     // "" if it's gone
    std::function<void(uint64_t id)> deletePhoto;
};

constexpr size_t kMaxText = 300;               // bytes of UTF-8 per message
constexpr size_t kKeepPerContact = 500;        // history kept per contact
constexpr uint64_t kResend = 5000;             // repeat an unacknowledged message while they're online
constexpr size_t kChunk = 48 * 1024;           // bytes of photo per network message
constexpr size_t kMaxPhoto = 1536 * 1024;      // largest photo accepted
constexpr uint64_t kPhotoResend = 30000;       // repeat an unacknowledged photo (all its pieces)
constexpr uint64_t kPartialExpire = 180000;    // drop a half-received photo after this long without a new piece
extern const char* const kPhotoText;           // a photo message's text in the history file

class Engine {
public:
    explicit Engine(IO io) : io_(std::move(io)) {}
    void Load(const std::string& text);
    std::string Save() const;

    // presence, from the transfer engine's view of who has Gitmo and is online
    void SetPresence(uint64_t peer, const std::string& name, bool online, bool inGame);

    // the player
    bool Send(uint64_t peer, const std::string& text);        // false: empty
    bool SendPhoto(uint64_t peer, const std::string& jpeg);   // false: empty, too big or couldn't be kept
    void SetViewing(uint64_t peer) { viewing_ = peer; if (peer) MarkRead(peer); }   // 0 = no chat open
    void MarkRead(uint64_t peer);
    void Forget(uint64_t peer);                           // delete a conversation and the contact
    void EraseAll();                                      // delete every conversation on this PC (theirs keep their copy)

    // the network
    void OnMessage(uint64_t from, const std::string& msg);
    void Tick();

    const std::map<uint64_t, Contact>& Contacts() const { return contacts_; }
    int Unread() const;
    int Receiving(uint64_t peer) const;   // photos from them coming in now (pieces still missing)

    static std::string CleanText(const std::string& s);   // one line, valid UTF-8, at most kMaxText bytes

private:
    struct Partial { uint64_t from = 0, key = 0, last = 0; std::string data; std::vector<bool> got; size_t have = 0; };
    Contact& Get(uint64_t peer);
    void Transmit(Contact& c, Msg& m);
    void SendRead(Contact& c);
    void AddTheirs(Contact& c, uint64_t from, const Msg& m, const std::string& ringText);
    void Trim(Contact& c);                  // keep kKeepPerContact, deleting the photos of the oldest
    void DropPhoto(const Msg& m) { if (m.photo && io_.deletePhoto) io_.deletePhoto(m.id); }
    void Changed() { if (io_.persist) io_.persist(Save()); }
    void Logf(const char* fmt, ...) const;

    IO io_;
    std::map<uint64_t, Contact> contacts_;
    std::map<uint64_t, Partial> partial_;   // photos being received, by message id
    uint64_t viewing_ = 0;
    uint64_t counter_ = 0;
    std::set<uint64_t> erased_;  // ids of erased messages from them, so a late repeat doesn't bring one back (this run only)
};

const char* StText(St s);

}   // namespace sms
