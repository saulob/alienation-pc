// The classic libSceNpMatching2 (SceNpId based) over the lobby (docs/ONLINE.md). Offsets are the ones the game's
// Online_Matching_NP and Network_Room_Orbis write in requests and read in responses and events.
//   request callback (u16 ctx, u32 request, u16 event, i32 error, void* data, void* arg); opt = {callback @0, arg @8}
//   context callback (u16 ctx, u16 event, u8 cause, i32 error, void* arg): 0x6f01 start over, 0x6f02 started, 0x6f03 stopped
//   room callback (u16 ctx, u64 room, u16 event, void* data, void* arg)
//   signaling callback (u16 ctx, u64 room, u16 member, u16 event, i32 error, void* arg): 0x5101 dead, 0x5102 established
// The game holds its own lock while it calls Matching2 and takes it again in the callbacks, and marks a request pending
// only after the call returns, so callbacks are only ever made from pump() (sceNpCheckCallback, every frame) and never
// with our lock held.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <utility>
#include <vector>

#include "net/json.h"
#include "net/lobby_link.h"
#include "net/online_version.h"
#include "net/relay_frame.h"
#include "online_status.h"
#include "runtime_online.h"

#define ALN_SYSV __attribute__((sysv_abi))

namespace aln::online {
namespace {

using net::Json;

using RequestCallback = void(ALN_SYSV*)(uint16_t, uint32_t, uint16_t, int32_t, const void*, void*);
using ContextCallback = void(ALN_SYSV*)(uint16_t, uint16_t, uint8_t, int32_t, void*);
using RoomCallback = void(ALN_SYSV*)(uint16_t, uint64_t, uint16_t, const void*, void*);
using SignalingCallback = void(ALN_SYSV*)(uint16_t, uint64_t, uint16_t, uint16_t, int32_t, void*);

constexpr uint16_t kContextId = 1, kServerId = 1;
enum : uint16_t {
    kEvWorlds = 0x0002, kEvMembersExternal = 0x0003, kEvSetExternal = 0x0004, kEvRoomsExternal = 0x0005,
    kEvCreateJoin = 0x0101, kEvJoin = 0x0102, kEvLeave = 0x0103, kEvSearch = 0x0106, kEvSetInternal = 0x0109,
    kEvPingInfo = 0x0e01
};
enum : uint16_t { kCtxStartOver = 0x6f01, kCtxStarted = 0x6f02, kCtxStopped = 0x6f03 };
enum : uint16_t { kRoomMemberJoined = 0x1101, kRoomMemberLeft = 0x1102, kRoomKicked = 0x1103, kRoomDestroyed = 0x1104 };
enum : uint16_t { kSignalDead = 0x5101, kSignalEstablished = 0x5102 };
enum : int32_t { kSignalingPending = 1, kSignalingActive = 2 };
enum : uint8_t { kCauseKicked = 2, kCauseServer = 4, kCauseConnection = 7 };

// Server errors the game tells apart (its reasons: room gone, full, service unavailable, busy); the rest are ours.
constexpr int32_t kErrServiceDown = int32_t(0x80550d02), kErrBusy = int32_t(0x80550d03);
constexpr int32_t kErrNoRoom = int32_t(0x80550d13), kErrRoomGone = int32_t(0x80550d15), kErrFull = int32_t(0x80550d19);
constexpr int32_t kErrRefused = int32_t(0x80550ca2), kErrInvalid = int32_t(0x80550cb1), kErrNotStarted = int32_t(0x80550cb2);
constexpr int32_t kErrNoMember = int32_t(0x80550cb4);

constexpr uint64_t kEndpointWaitMs = 5000;  // STUN and UPnP get this long after the bind before we report anyway
constexpr uint64_t kEstablishWaitMs = 8000; // a peer is linked even if our own report is not confirmed
constexpr uint64_t kSettleMs = 500;         // a peer's endpoint must be stable this long
constexpr size_t kNpIdSize = 0x24, kOnlineIdChars = 16;
constexpr uint16_t kFirstSearchAttr = 0x4d, kLastSearchAttr = 0x53;

template <class T> T Rd(const void* p, size_t offset) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(v));
    return v;
}
template <class T> void Wr(void* p, size_t offset, T v) { std::memcpy(static_cast<uint8_t*>(p) + offset, &v, sizeof(v)); }

int32_t ErrorOf(const std::string& code) {
    if (code == "ok") return 0;
    if (code == "not_found" || code == "incompatible") return kErrNoRoom;
    if (code == "closed") return kErrRoomGone;
    if (code == "full") return kErrFull;
    if (code == "limit") return kErrBusy;
    if (code == net::kLinkLost || code == net::kUnreachable) return kErrServiceDown;
    return kErrRefused;
}

// Zeroed blocks the game reads during one callback.
struct Block {
    std::vector<std::unique_ptr<uint8_t[]>> parts;
    uint8_t* alloc(size_t size) {
        parts.emplace_back(new uint8_t[size + 16]());
        return parts.back().get();
    }
    uint8_t* copy(const std::vector<uint8_t>& bytes) {
        uint8_t* p = alloc(bytes.size());
        if (!bytes.empty()) std::memcpy(p, bytes.data(), bytes.size());
        return p;
    }
};

struct Member {
    uint16_t id = 0;
    uint64_t account = 0;
    std::string name;
    int64_t joined_ms = 0;
    Json bin;
};

struct Signal {
    uint64_t account = 0;
    net::Endpoint address;
    std::vector<net::Endpoint> lan;
    uint64_t changed_ms = 0;
    bool linked = false, established = false;
};

enum class Kind { kGame, kInternal, kResume, kUdp, kSearch, kCreateJoin, kWorlds, kRoomsExternal, kMembersExternal, kLeave };

struct Pending {
    Kind kind = Kind::kInternal;
    uint32_t request = 0;
    RequestCallback callback = nullptr;
    void* arg = nullptr;
    uint16_t event = 0;
    uint64_t room = 0;
    bool random = false;
};

// A WebApi request waiting for the lobby's answer on its own thread.
struct Waiter {
    std::mutex mutex;
    std::condition_variable done_cv;
    bool done = false;
    std::string code;
    Json body;
};

struct State {
    bool initialized = false, context = false, start_wanted = false;
    bool presence = false; // the link stays up for friends and invitations, not only while the context runs
    uint64_t retry_ms = 0;
    std::map<uint32_t, std::shared_ptr<Waiter>> waiters;
    ContextCallback context_cb = nullptr;
    void* context_arg = nullptr;
    RoomCallback room_cb = nullptr;
    void* room_arg = nullptr;
    SignalingCallback signal_cb = nullptr;
    void* signal_arg = nullptr;
    RequestCallback default_cb = nullptr;
    void* default_arg = nullptr;
    std::unique_ptr<net::LobbyLink> link;
    bool up = false, lost = false, down = false;
    uint32_t your_ip = 0, public_ip = 0;
    bool lobby_on_lan = false;
    uint32_t next_request = 1;
    std::map<uint32_t, Pending> pending;
    std::vector<std::function<void()>> calls;
    // the room we are in
    uint64_t room = 0;
    uint16_t me = 0, owner = 0, slots = 0;
    uint32_t world = 1;
    net::RoomKey key{};
    std::map<uint16_t, Member> members;
    std::map<uint16_t, Signal> signals;
    // our endpoint as reported to the lobby; the socket belongs to the P2P layer
    std::weak_ptr<net::PeerSocket> socket;
    bool relay_capable = false;
    uint64_t bound_ms = 0;
    uint16_t reported_port = 0;
    uint32_t reported_ip = 0;
    bool udp_inflight = false, udp_acked = false;
    uint64_t udp_retry_ms = 0;
    std::string last_external;
    std::string join_code; // ALN_JOIN_CODE: the game's room search finds only this room
};

std::mutex g_mutex;
State g;
// The link as the network status box sees it; read without g_mutex from the game's script threads. A link that is
// resuming stays ready, as Matching2 also hides it from the game.
std::atomic<int> g_service{ONLINE_SERVICE_OFF};
std::atomic<uint64_t> g_connecting_since{0};
std::atomic<bool> g_context_started{false};
std::atomic<int> g_reported_status{-1};

uint64_t RtcTick(int64_t unix_ms) { return uint64_t(unix_ms) * 1000 + UINT64_C(62135596800000000); }

std::string SafeName(const std::string& name) { return net::online_name(name, "Player"); }

// SceNpId: online id (16 characters, terminator, 3 padding), opt[8] with the account, reserved[8]. Our own is the exact
// one the NP layer signed in with: the game finds its player among the members with sceNpCmpOnlineId.
uint8_t* NpId(Block& b, const std::string& name, uint64_t account) {
    uint8_t* p = b.alloc(kNpIdSize);
    std::memcpy(p, name.data(), std::min(name.size(), kOnlineIdChars));
    Wr<uint64_t>(p, 0x14, account);
    return p;
}

// ---- request bodies from the game's structs ----
// BinAttr {u16 id @0, data @8, size @0x10}.
Json BinAttrs(const uint8_t* array, uint64_t count) {
    Json out = Json::Array();
    for (uint64_t i = 0; array && i < count && i < 16; ++i) {
        const uint8_t* e = array + i * 0x18;
        const uint8_t* data = Rd<const uint8_t*>(e, 8);
        const uint64_t size = std::min<uint64_t>(Rd<uint64_t>(e, 0x10), 0x100);
        Json pair = Json::Array();
        pair.push(int64_t(Rd<uint16_t>(e, 0)));
        pair.push(data ? net::hex_encode(data, size_t(size)) : std::string());
        out.push(std::move(pair));
    }
    return out;
}

// IntAttr {u16 id @0, u32 value @4}.
Json IntAttrs(const uint8_t* array, uint64_t count) {
    Json out = Json::Array();
    for (uint64_t i = 0; array && i < count && i < 32; ++i) {
        Json pair = Json::Array();
        pair.push(int64_t(Rd<uint16_t>(array, size_t(i * 8))));
        pair.push(int64_t(Rd<uint32_t>(array, size_t(i * 8 + 4))));
        out.push(std::move(pair));
    }
    return out;
}

std::vector<uint8_t> Unhex(const Json& j) {
    std::vector<uint8_t> v;
    net::hex_decode(j.as_string(), v);
    return v;
}

// ---- response structs for the game ----
// RoomMemberDataInternal (0x58): next @0, join date @8, SceNpId @0x10, member id u16 @0x38, member bin attrs @0x48 with
// their count @0x50; an attribute (0x20) is {update date @0, BinAttr @8}.
uint8_t* MemberStruct(Block& b, const Member& m) {
    uint8_t* p = b.alloc(0x58);
    Wr<uint64_t>(p, 8, RtcTick(m.joined_ms));
    std::memcpy(p + 0x10, NpId(b, m.name, m.account), kNpIdSize);
    Wr<uint16_t>(p, 0x38, m.id);
    const size_t count = std::min<size_t>(m.bin.size(), 16);
    if (count) {
        uint8_t* attrs = b.alloc(0x20 * count);
        for (size_t i = 0; i < count; ++i) {
            const Json& pair = m.bin.items()[i];
            if (pair.items().size() != 2) continue;
            const std::vector<uint8_t> data = Unhex(pair.items()[1]);
            uint8_t* e = attrs + i * 0x20;
            Wr<uint64_t>(e, 0, RtcTick(m.joined_ms));
            Wr<uint16_t>(e, 8, uint16_t(pair.items()[0].as_int()));
            Wr<uint8_t*>(e, 0x10, b.copy(data));
            Wr<uint64_t>(e, 0x18, data.size());
        }
        Wr<uint8_t*>(p, 0x48, attrs);
        Wr<uint64_t>(p, 0x50, count);
    }
    return p;
}

Member MemberOf(const Json& j) {
    Member m;
    m.id = uint16_t(j["id"].as_int());
    m.account = j["account"].as_u64();
    m.name = SafeName(j["name"].as_string());
    m.joined_ms = j["joined"].as_int();
    m.bin = j["bin"];
    return m;
}

// Room bin attrs internal (0x28 each): update date @0, member @8, BinAttr {id @0x10, data @0x18, size @0x20}.
uint8_t* InternalAttrs(Block& b, const Json& attrs, uint64_t& count) {
    count = attrs.size();
    if (!count) return nullptr;
    uint8_t* array = b.alloc(0x28 * count);
    for (uint64_t i = 0; i < count; ++i) {
        const Json& pair = attrs.items()[i];
        const std::vector<uint8_t> data = Unhex(pair.items().size() > 1 ? pair.items()[1] : Json());
        uint8_t* e = array + i * 0x28;
        Wr<uint16_t>(e, 0x10, uint16_t(pair.items().empty() ? 0 : pair.items()[0].as_int()));
        Wr<uint8_t*>(e, 0x18, b.copy(data));
        Wr<uint64_t>(e, 0x20, data.size());
    }
    return array;
}

// BinAttr array {id @0, data @8, size @0x10}.
uint8_t* PlainAttrs(Block& b, const Json& attrs, uint64_t& count) {
    count = 0;
    std::vector<const Json*> list;
    for (const Json& pair : attrs.items())
        if (pair.items().size() == 2) list.push_back(&pair);
    if (list.empty()) return nullptr;
    uint8_t* array = b.alloc(0x18 * list.size());
    for (const Json* pair : list) {
        const std::vector<uint8_t> data = Unhex(pair->items()[1]);
        Wr<uint16_t>(array + count * 0x18, 0, uint16_t(pair->items()[0].as_int()));
        Wr<uint8_t*>(array + count * 0x18, 8, b.copy(data));
        Wr<uint64_t>(array + count * 0x18, 0x10, data.size());
        ++count;
    }
    return array;
}

// The searchable int attrs the game asks for (0x4d..0x53), in order, 0 for one the room never set.
uint8_t* SearchInts(Block& b, const Json& ints, uint64_t& count) {
    count = kLastSearchAttr - kFirstSearchAttr + 1;
    uint8_t* array = b.alloc(8 * count);
    for (uint64_t k = 0; k < count; ++k) Wr<uint16_t>(array, size_t(k * 8), uint16_t(kFirstSearchAttr + k));
    for (const Json& pair : ints.items()) {
        const int64_t id = pair.items().empty() ? 0 : pair.items()[0].as_int();
        if (id >= kFirstSearchAttr && id <= kLastSearchAttr && pair.items().size() == 2)
            Wr<uint32_t>(array, size_t((id - kFirstSearchAttr) * 8 + 4), uint32_t(pair.items()[1].as_int()));
    }
    return array;
}

// RoomDataExternal (0xa0): next @0, max slots u16 @8, members u16 @0xa, server u16 @0x10, world @0x14, room id @0x20,
// owner SceNpId* @0x40, searchable int attrs @0x58/@0x60, bin attrs external @0x78/@0x80.
uint8_t* ExternalRoom(Block& b, const Json& r) {
    uint8_t* room = b.alloc(0xa0);
    Wr<uint16_t>(room, 8, uint16_t(r["slots"].as_int(4)));
    Wr<uint16_t>(room, 0xa, uint16_t(r["count"].as_int()));
    Wr<uint16_t>(room, 0x10, kServerId);
    Wr<uint32_t>(room, 0x14, uint32_t(r["world"].as_int(1)));
    Wr<uint64_t>(room, 0x20, r["id"].as_u64());
    Wr<uint8_t*>(room, 0x40, NpId(b, SafeName(r["owner_name"].as_string()), r["owner_account"].as_u64()));
    uint64_t n = 0;
    Wr<uint8_t*>(room, 0x58, SearchInts(b, r["ints"], n));
    Wr<uint64_t>(room, 0x60, n);
    Wr<uint8_t*>(room, 0x78, PlainAttrs(b, r["bin_ext"], n));
    Wr<uint64_t>(room, 0x80, n);
    return room;
}

net::RoomKey KeyOf(const std::string& hex) {
    net::RoomKey key{};
    std::vector<uint8_t> bytes;
    if (net::hex_decode(hex, bytes) && bytes.size() == key.size()) std::memcpy(key.data(), bytes.data(), key.size());
    return key;
}

std::vector<net::Endpoint> Candidates(const Signal& s) {
    std::vector<net::Endpoint> list;
    const bool same_address = s.address.ip && s.address.ip == g.public_ip;
    if (same_address) list = s.lan;
    if (s.address.valid()) list.push_back(s.address);
    if (!same_address) list.insert(list.end(), s.lan.begin(), s.lan.end());
    return list;
}

// The relay's two ends live on other threads (the socket's, the lobby link's) and must not take g_mutex.
std::mutex g_relay_mutex;
net::LobbyLink* g_relay_link = nullptr;
std::weak_ptr<net::PeerSocket> g_relay_socket;
// While the lobby link reconnects, the seat is not ours again until room.resume answers: the lobby would count frames
// sent before that as faults of a sender with no room. The link layer retransmits what is refused here.
bool g_relay_paused = false;

bool SendRelayFrame(std::vector<uint8_t>&& frame) {
    std::lock_guard<std::mutex> lock(g_relay_mutex);
    return g_relay_link && !g_relay_paused && g_relay_link->send_binary(std::move(frame));
}

void PauseRelay(bool paused) {
    std::lock_guard<std::mutex> lock(g_relay_mutex);
    g_relay_paused = paused;
}

void ReceiveRelayFrame(const uint8_t* data, size_t size) {
    std::shared_ptr<net::PeerSocket> socket;
    {
        std::lock_guard<std::mutex> lock(g_relay_mutex);
        socket = g_relay_socket.lock();
    }
    if (socket) socket->deliver_relay(data, size);
}

void SetRelayEnds(net::LobbyLink* link, const std::shared_ptr<net::PeerSocket>& socket) {
    std::lock_guard<std::mutex> lock(g_relay_mutex);
    g_relay_link = link;
    g_relay_socket = socket;
    g_relay_paused = false;
}

void ConfigureRelay() {
    const std::shared_ptr<net::PeerSocket> socket = g.socket.lock();
    if (!socket) return;
    if (g.relay_capable && g.room && g.link) {
        socket->enable_relay(net::relay_token(g.key), SendRelayFrame);
    } else {
        socket->disable_relay();
    }
}

void ApplyPeers() {
    const std::shared_ptr<net::PeerSocket> socket = g.socket.lock();
    if (!socket || !g.room) return;
    socket->set_room(g.key, settings().identity.account);
    for (auto& [member, s] : g.signals) {
        if (member == g.me || !s.account) continue;
        socket->set_peer(s.account, Candidates(s), member);
        if (s.linked) p2p_set_member(member, s.account);
    }
    ConfigureRelay();
}

void ClearRoom() {
    if (const std::shared_ptr<net::PeerSocket> socket = g.socket.lock()) {
        for (const auto& [member, s] : g.signals) socket->forget_peer(s.account);
        socket->disable_relay();
    }
    p2p_forget_members();
    g.room = 0;
    g.me = g.owner = g.slots = 0;
    g.members.clear();
    g.signals.clear();
    g.last_external.clear();
}

// The room from a create or join answer; also becomes our room. Caller holds g_mutex.
// Response {RoomDataInternal* @0, members @8, count @0x10, me @0x18, owner @0x20}; RoomDataInternal (0x80): max slots
// u16 @8, server u16 @0xa, world @0xc, room id @0x18, flags @0x40, bin attrs internal @0x48/@0x50.
uint8_t* JoinResponse(Block& b, const Json& body) {
    ClearRoom();
    const Json& room = body["room"];
    g.room = room["id"].as_u64();
    g.me = uint16_t(body["me"].as_int());
    g.owner = uint16_t(room["owner"].as_int());
    g.slots = uint16_t(room["slots"].as_int(4));
    g.world = uint32_t(room["world"].as_int(1));
    g.key = KeyOf(room["key"].as_string());
    uint8_t* internal = b.alloc(0x80);
    Wr<uint16_t>(internal, 8, g.slots);
    Wr<uint16_t>(internal, 0xa, kServerId);
    Wr<uint32_t>(internal, 0xc, g.world);
    Wr<uint64_t>(internal, 0x18, g.room);
    Wr<uint32_t>(internal, 0x40, uint32_t(room["flags"].as_int()));
    uint64_t attrs = 0;
    Wr<uint8_t*>(internal, 0x48, InternalAttrs(b, room["bin_int"], attrs));
    Wr<uint64_t>(internal, 0x50, attrs);
    uint8_t* response = b.alloc(0x30);
    Wr<uint8_t*>(response, 0, internal);
    uint8_t *head = nullptr, *previous = nullptr, *me = nullptr, *owner = nullptr;
    uint64_t count = 0;
    for (const Json& j : room["members"].items()) {
        Member m = MemberOf(j);
        if (m.id == g.me) m.name = settings().online_id;
        g.members[m.id] = m;
        uint8_t* p = MemberStruct(b, m);
        (previous ? Wr<uint8_t*>(previous, 0, p) : void(head = p));
        previous = p;
        if (m.id == g.me) me = p;
        if (m.id == g.owner) owner = p;
        ++count;
    }
    Wr<uint8_t*>(response, 8, head);
    Wr<uint64_t>(response, 0x10, count);
    Wr<uint8_t*>(response, 0x18, me);
    Wr<uint8_t*>(response, 0x20, owner);
    ApplyPeers();
    return response;
}

// Search results: range {start @0, total @4, count @8}, RoomDataExternal list @0x10. Rooms of another build or protocol
// never reach the game; the lobby leaves them out and counts them.
uint8_t* SearchResponse(Block& b, const Json& body, uint32_t from, bool random) {
    uint8_t* response = b.alloc(0x20);
    std::vector<const Json*> rooms;
    for (const Json& r : body["rooms"].items())
        if (r["count"].as_int() > 0) rooms.push_back(&r);
    if (random) {
        static std::mt19937 shuffle{std::random_device{}()};
        std::shuffle(rooms.begin(), rooms.end(), shuffle);
    }
    uint8_t *head = nullptr, *previous = nullptr;
    for (const Json* r : rooms) {
        uint8_t* room = ExternalRoom(b, *r);
        (previous ? Wr<uint8_t*>(previous, 0, room) : void(head = room));
        previous = room;
    }
    Wr<uint32_t>(response, 0, from);
    Wr<uint32_t>(response, 4, uint32_t(body["total"].as_int()));
    Wr<uint32_t>(response, 8, uint32_t(rooms.size()));
    Wr<uint8_t*>(response, 0x10, head);
    if (body["incompatible"].as_int())
        std::printf("Online: search left out %lld room(s) of another game build or online version\n",
                    (long long)body["incompatible"].as_int());
    return response;
}

// {RoomDataExternal list @0, count @8}.
uint8_t* RoomsExternalResponse(Block& b, const Json& body) {
    uint8_t* response = b.alloc(0x10);
    uint8_t *head = nullptr, *previous = nullptr;
    uint64_t n = 0;
    for (const Json& r : body["rooms"].items()) {
        uint8_t* room = ExternalRoom(b, r);
        (previous ? Wr<uint8_t*>(previous, 0, room) : void(head = room));
        previous = room;
        ++n;
    }
    Wr<uint8_t*>(response, 0, head);
    Wr<uint64_t>(response, 8, n);
    return response;
}

// {RoomMemberDataExternal list @0, count @8}; an entry (0x40): next @0, SceNpId @0x10, role u8 @0x34 (2: owner).
uint8_t* MembersExternalResponse(Block& b, const Json& body) {
    uint8_t* response = b.alloc(0x10);
    uint8_t *head = nullptr, *previous = nullptr;
    uint64_t n = 0;
    for (const Json& m : body["members"].items()) {
        uint8_t* p = b.alloc(0x40);
        std::memcpy(p + 0x10, NpId(b, SafeName(m["name"].as_string()), 0), kNpIdSize);
        p[0x34] = m["owner"].as_bool() ? 2 : 1;
        (previous ? Wr<uint8_t*>(previous, 0, p) : void(head = p));
        previous = p;
        ++n;
    }
    Wr<uint8_t*>(response, 0, head);
    Wr<uint64_t>(response, 8, n);
    return response;
}

// World list: {World* @0, u64 count @8}; World (0x28 bytes): world id @8, rooms @0x18, members @0x1c.
uint8_t* WorldsResponse(Block& b, const Json& body) {
    const Json& worlds = body["worlds"];
    uint8_t* response = b.alloc(0x10);
    uint8_t* array = b.alloc(0x28 * std::max<size_t>(worlds.size(), 1));
    for (size_t i = 0; i < worlds.size(); ++i) {
        Wr<uint32_t>(array + i * 0x28, 8, uint32_t(worlds.items()[i]["world"].as_int(1)));
        Wr<uint32_t>(array + i * 0x28, 0x18, uint32_t(worlds.items()[i]["rooms"].as_int()));
        Wr<uint32_t>(array + i * 0x28, 0x1c, uint32_t(worlds.items()[i]["members"].as_int()));
    }
    Wr<uint8_t*>(response, 0, array);
    Wr<uint64_t>(response, 8, worlds.size());
    return response;
}

// ---- requests ----
bool LinkUsable() { return g.link && !g.down; }

void Complete(const Pending& p, int32_t error, std::shared_ptr<Block> block = nullptr, const void* data = nullptr) {
    if (!p.callback) return;
    const RequestCallback cb = p.callback;
    void* arg = p.arg;
    const uint32_t request = p.request;
    const uint16_t event = p.event;
    g.calls.push_back([=] {
        cb(kContextId, request, event, error, data, arg);
        (void)block;
    });
}

Pending GamePending(Kind kind, const void* opt, uint16_t event) {
    Pending p;
    p.kind = kind;
    p.event = event;
    p.callback = opt ? Rd<RequestCallback>(opt, 0) : g.default_cb;
    p.arg = opt ? Rd<void*>(opt, 8) : g.default_arg;
    p.request = g.next_request++;
    return p;
}

// Sends a request for the game; answers it with "service unavailable" when there is no link.
uint32_t Issue(Pending p, const std::string& op, Json body) {
    if (!LinkUsable()) {
        Complete(p, kErrServiceDown);
        return p.request;
    }
    const uint32_t seq = g.link->request(op, std::move(body));
    g.pending[seq] = p;
    return p.request;
}

uint32_t Issue(Kind kind, const std::string& op, Json body, const void* opt, uint16_t event) {
    return Issue(GamePending(kind, opt, event), op, std::move(body));
}

void PushRoomEvent(uint16_t event, std::shared_ptr<Block> block, const uint8_t* data) {
    if (!g.room_cb) return;
    const RoomCallback cb = g.room_cb;
    void* arg = g.room_arg;
    const uint64_t room = g.room;
    g.calls.push_back([=] {
        cb(kContextId, room, event, data, arg);
        (void)block;
    });
}

void PushContextEvent(uint16_t event, int32_t error) {
    if (!g.context_cb) return;
    const ContextCallback cb = g.context_cb;
    void* arg = g.context_arg;
    g.calls.push_back([=] { cb(kContextId, event, 0, error, arg); });
}

void PushSignal(uint16_t member, uint16_t event) {
    if (!g.signal_cb) return;
    const SignalingCallback cb = g.signal_cb;
    void* arg = g.signal_arg;
    const uint64_t room = g.room;
    g.calls.push_back([=] { cb(kContextId, room, member, event, 0, arg); });
}

// Kicked or destroyed: {event cause u8 @0}.
void RoomGone(uint16_t event, uint8_t cause) {
    if (!g.room) return;
    auto block = std::make_shared<Block>();
    uint8_t* data = block->alloc(0x10);
    data[0] = cause;
    PushRoomEvent(event, block, data);
    ClearRoom();
}

Json HelloBody() {
    const Settings& s = settings();
    return net::hello_body(s.identity, s.online_id, s.game, true, true, s.config.lobby_visible, s.build);
}

// ---- lobby messages ----
void Finish(Waiter& w, const std::string& code, const Json& body) {
    std::lock_guard<std::mutex> lock(w.mutex);
    w.done = true;
    w.code = code;
    w.body = body;
    w.done_cv.notify_all();
}

void FailWaiters(const std::string& code) {
    for (auto& [seq, w] : g.waiters) Finish(*w, code, Json::Object());
    g.waiters.clear();
}

void OnResponse(const net::LobbyMessage& m) {
    if (auto w = g.waiters.find(m.seq); w != g.waiters.end()) {
        Finish(*w->second, m.code, m.body);
        g.waiters.erase(w);
        return;
    }
    auto it = g.pending.find(m.seq);
    if (it == g.pending.end()) return;
    const Pending p = it->second;
    g.pending.erase(it);
    const int32_t error = ErrorOf(m.code);
    switch (p.kind) {
    case Kind::kInternal: return;
    case Kind::kUdp:
        g.udp_inflight = false;
        g.udp_acked = !error;
        std::printf("Matching2: endpoint %s (lobby has %s:%lld)\n", error ? "refused" : "reported",
                    net::lobby_endpoint_text(m.body["public_ip"].as_string(), m.body["ip"].as_string()).c_str(),
                    (long long)m.body["port"].as_int());
        if (error) {
            g.reported_port = 0;
            g.udp_retry_ms = net::now_ms() + 5000;
        }
        return;
    case Kind::kResume:
        if (!error) {
            std::printf("Matching2: seat resumed in room %llu as member 0x%x\n", (unsigned long long)g.room, g.me);
        } else {
            std::printf("Matching2: seat lost after the reconnect (%s)\n", m.code.c_str());
            RoomGone(kRoomDestroyed, kCauseConnection);
        }
        PauseRelay(false);
        g.lost = false;
        g.reported_port = 0;
        g.udp_acked = false;
        g.link->release_held();
        return;
    default: break;
    }
    auto block = std::make_shared<Block>();
    const void* data = nullptr;
    if (!error) {
        if (p.kind == Kind::kWorlds) data = WorldsResponse(*block, m.body);
        if (p.kind == Kind::kRoomsExternal) data = RoomsExternalResponse(*block, m.body);
        if (p.kind == Kind::kMembersExternal) data = MembersExternalResponse(*block, m.body);
        if (p.kind == Kind::kSearch) {
            data = SearchResponse(*block, m.body, uint32_t(m.body["from"].as_int(1)), p.random);
            std::printf("Matching2: search found %u room(s)\n", Rd<uint32_t>(data, 8));
        }
        if (p.kind == Kind::kLeave) {
            uint8_t* left = block->alloc(0x10);
            Wr<uint64_t>(left, 0, p.room);
            data = left;
        }
        if (p.kind == Kind::kCreateJoin) {
            data = JoinResponse(*block, m.body);
            std::printf("Matching2: %s room %llu as member 0x%x (owner 0x%x, %zu member(s), %u slots)\n",
                        p.event == kEvJoin ? "joined" : "created", (unsigned long long)g.room, g.me, g.owner, g.members.size(),
                        g.slots);
        }
    } else {
        std::printf("Matching2: request 0x%04x failed: %s\n", p.event, m.code.c_str());
        if (p.event == kEvSetExternal) g.last_external.clear();
        if (m.code == "incompatible")
            std::printf("Online: room of another game build or online version (room %s, protocol %lld; this PC %s, protocol %d)\n",
                        m.body["build"].as_string().c_str(), (long long)m.body["online_protocol"].as_int(),
                        settings().build.c_str(), net::kOnlineProtocolVersion);
    }
    Complete(p, error, block, data);
}

void OnPeer(const Json& b) {
    const uint16_t member = uint16_t(b["member"].as_int());
    if (member == g.me) return;
    Signal& s = g.signals[member];
    const net::Endpoint address{net::parse_ip(b["ip"].as_string()), uint16_t(b["port"].as_int())};
    std::vector<net::Endpoint> lan;
    for (const Json& c : b["lan"].items()) {
        const net::Endpoint e{net::parse_ip(c["ip"].as_string()), uint16_t(c["port"].as_int())};
        if (e.valid() && net::is_lan(e.ip) && lan.size() < 4) lan.push_back(e);
    }
    s.account = b["account"].as_u64();
    if (address != s.address || lan != s.lan || !s.changed_ms) {
        if (s.linked) std::printf("Matching2: member 0x%x moved to %s\n", member, net::endpoint_text(address).c_str());
        s.address = address;
        s.lan = lan;
        s.changed_ms = net::now_ms();
    }
    std::printf("Matching2: member 0x%x is at %s (%zu LAN candidate(s))\n", member,
                address.valid() ? net::endpoint_text(address).c_str() : "no direct address", lan.size());
    const std::shared_ptr<net::PeerSocket> socket = g.socket.lock();
    if (socket && s.account) {
        socket->set_peer(s.account, Candidates(s), member);
        for (const net::Endpoint& e : Candidates(s)) socket->punch(e);
    }
}

void MemberGone(uint16_t id) {
    auto it = g.signals.find(id);
    if (it == g.signals.end()) return;
    const bool established = it->second.established;
    if (const std::shared_ptr<net::PeerSocket> socket = g.socket.lock()) socket->forget_peer(it->second.account);
    g.signals.erase(it);
    p2p_forget_member(id);
    if (established) PushSignal(id, kSignalDead);
}

void OnEvent(const net::LobbyMessage& m) {
    const Json& b = m.body;
    if (m.event.rfind("invite.", 0) == 0 || m.event.rfind("friend.", 0) == 0) {
        sessions_event(m.event, b);
        return;
    }
    if (m.event == "chat") return;
    if (m.event == "relay.closed") {
        std::printf("Matching2: the lobby stopped relaying for this connection (%s)\n", b["reason"].as_string().c_str());
        if (const std::shared_ptr<net::PeerSocket> socket = g.socket.lock()) socket->disable_relay();
        return;
    }
    if (!g.room || b["room"].as_u64() != g.room) return;
    auto block = std::make_shared<Block>();
    if (m.event == "room.member_joined") {
        const Member joined = MemberOf(b["member"]);
        g.members[joined.id] = joined;
        std::printf("Matching2: member 0x%x joined\n", joined.id);
        uint8_t* data = block->alloc(0x10);
        Wr<uint8_t*>(data, 0, MemberStruct(*block, joined));
        PushRoomEvent(kRoomMemberJoined, block, data);
    } else if (m.event == "room.member_left") {
        const uint16_t id = uint16_t(b["member"].as_int());
        Member left;
        left.id = id;
        if (g.members.count(id)) left = g.members[id];
        g.members.erase(id);
        MemberGone(id);
        std::printf("Matching2: member 0x%x left (%s)\n", id, b["why"].as_string().c_str());
        uint8_t* data = block->alloc(0x10);
        Wr<uint8_t*>(data, 0, MemberStruct(*block, left));
        PushRoomEvent(kRoomMemberLeft, block, data);
    } else if (m.event == "room.owner") {
        g.owner = uint16_t(b["to"].as_int());
        std::printf("Matching2: room owner 0x%llx -> 0x%x\n", (unsigned long long)b["from"].as_int(), g.owner);
    } else if (m.event == "room.kicked" || m.event == "room.gone") {
        std::printf("Matching2: %s\n", m.event == "room.kicked" ? "kicked from the room" : "the room was closed");
        RoomGone(m.event == "room.kicked" ? kRoomKicked : kRoomDestroyed, m.event == "room.kicked" ? kCauseKicked : kCauseServer);
    } else if (m.event == "peer") {
        OnPeer(b);
    } else if (m.event == "peer.gone") {
        MemberGone(uint16_t(b["member"].as_int()));
    }
}

void NoteRelayCapability(const Json& hello_reply) {
    const net::RelayCapability capability = net::relay_capability(hello_reply);
    g.relay_capable = capability.usable();
    std::printf("Matching2: lobby relay %s\n", g.relay_capable ? "available as a fallback" : "not offered");
    ConfigureRelay();
}

void NoteLobbyAddress(const std::string& ip) {
    g.your_ip = g.public_ip = net::parse_ip(ip);
    g.lobby_on_lan = net::is_lan(net::classify_text(ip));
}

void OnLink(const net::LobbyMessage& m) {
    using K = net::LobbyMessage::Kind;
    if (m.kind == K::kUp) {
        NoteRelayCapability(m.body);
        g.up = true;
        g_service = ONLINE_SERVICE_READY;
        NoteLobbyAddress(m.body["ip"].as_string());
        std::printf("Matching2: signed in to the lobby as %s (build %s, lobby %s); it sees %s\n", settings().online_id.c_str(),
                    settings().build.c_str(), m.body["backend"].as_string().empty() ? "?" : m.body["backend"].as_string().c_str(),
                    m.body["ip"].as_string().c_str());
        if (g.start_wanted) PushContextEvent(kCtxStarted, 0);
    } else if (m.kind == K::kLost) {
        g.lost = true;
        g.udp_inflight = false;
        PauseRelay(true);
        std::printf("Matching2: lobby connection lost; the game is not told while it reconnects\n");
    } else if (m.kind == K::kBack) {
        NoteRelayCapability(m.body);
        NoteLobbyAddress(m.body["ip"].as_string());
        if (g.room) {
            Json body = Json::Object();
            body.set("room", net::u64_text(g.room));
            body.set("member", int64_t(g.me));
            const uint32_t seq = g.link->request("room.resume", std::move(body), true);
            g.pending[seq] = Pending{Kind::kResume};
        } else {
            PauseRelay(false);
            g.lost = false;
            g.reported_port = 0;
            g.link->release_held();
        }
    } else if (m.kind == K::kDown) {
        PauseRelay(false);
        g.relay_capable = false;
        ConfigureRelay();
        g.up = false;
        g.down = true;
        g.lost = false;
        g_service = ONLINE_SERVICE_FAILED;
        // A context started again opens a new session, which must hear our endpoint again.
        g.udp_inflight = g.udp_acked = false;
        g.reported_port = 0;
        g.reported_ip = 0;
        g.udp_retry_ms = 0;
        std::printf("Matching2: lobby unavailable (%s)%s\n", m.code.c_str(),
                    m.code == "wrong_game" || m.code == "proto" ? ": this lobby does not serve this version of the game" : "");
        for (auto& [seq, p] : g.pending) Complete(p, kErrServiceDown);
        g.pending.clear();
        FailWaiters(net::kUnreachable);
        g.retry_ms = net::now_ms() + 30000;
        RoomGone(kRoomDestroyed, kCauseConnection);
        // The game retries a context that starts over a few times, then reports the service unreachable.
        if (g.start_wanted) PushContextEvent(kCtxStartOver, kErrServiceDown);
    }
}

// Our endpoint goes to the lobby once the game's socket is bound and STUN / UPnP had their chance.
void ReportEndpoint(uint64_t now) {
    std::shared_ptr<net::PeerSocket> socket = peer_socket();
    if (socket != g.socket.lock()) {
        g.socket = socket;
        SetRelayEnds(g.link.get(), socket);
        g.bound_ms = now;
        g.reported_port = 0;
        g.udp_acked = false;
        g.udp_inflight = false;
        ApplyPeers();
    }
    if (!socket || !g.up || g.lost || g.udp_inflight || now < g.udp_retry_ms) return;
    net::Endpoint mapped;
    const net::StunState stun = socket->stun(&mapped);
    const UpnpView upnp = upnp_view();
    const bool pending = stun == net::StunState::kRunning || upnp.working;
    if (!g.lobby_on_lan && pending && now - g.bound_ms < kEndpointWaitMs) return;
    const net::EndpointChoice choice = net::choose_endpoint(g.your_ip, g.lobby_on_lan, socket->port(), stun, mapped,
                                                            upnp.mapped ? upnp.external : net::Endpoint{});
    g.public_ip = choice.ip;
    if (choice.port == g.reported_port && choice.ip == g.reported_ip) return;
    Json body = Json::Object();
    body.set("port", int64_t(choice.port));
    if (choice.ip != g.your_ip) body.set("public_ip", net::ip_text(choice.ip));
    Json lan = Json::Array();
    std::vector<uint32_t> addresses = net::local_ipv4_addresses();
    addresses.insert(addresses.begin(), net::parse_ip("127.0.0.1"));
    for (const uint32_t ip : addresses) {
        if (!net::is_lan(ip) || lan.size() >= 4) continue;
        Json c = Json::Object();
        c.set("ip", net::ip_text(ip));
        c.set("port", int64_t(socket->port()));
        lan.push(std::move(c));
    }
    body.set("lan", std::move(lan));
    std::printf("Matching2: %s (local %u)\n", net::endpoint_choice_text(choice, g.relay_capable).c_str(), socket->port());
    g.reported_port = choice.port;
    g.reported_ip = choice.ip;
    g.udp_inflight = true;
    const uint32_t seq = g.link->request("udp", std::move(body));
    g.pending[seq] = Pending{Kind::kUdp};
}

// A member's link starts once its endpoint is known and settled and ours has reached the lobby (or waited long enough);
// signaling becomes "established" for the game when that link is connected.
void LinkMembers(uint64_t now) {
    if (!g.room || g.socket.expired()) return;
    const bool ready = g.udp_acked || now - g.bound_ms > kEstablishWaitMs;
    for (auto& [member, s] : g.signals) {
        if (member == g.me || !s.account || !g.members.count(member)) continue;
        if (!s.linked && ready && now - s.changed_ms >= kSettleMs) {
            s.linked = true;
            std::printf("Matching2: member 0x%x linking%s\n", member,
                        Candidates(s).empty() ? " through the relay (no direct endpoint)" : "");
            p2p_set_member(member, s.account);
        }
        if (s.linked && !s.established && p2p_member_connected(member)) {
            s.established = true;
            std::printf("Matching2: member 0x%x established at %s\n", member, net::endpoint_text(peer_endpoint(member)).c_str());
            PushSignal(member, kSignalEstablished);
        }
    }
}

// The lobby link (again after it went down). Caller holds g_mutex.
bool StartLink() {
    const Settings& s = settings();
    if (s.config.lobby.empty()) {
        g_service = ONLINE_SERVICE_FAILED;
        return false;
    }
    g_connecting_since = net::now_ms();
    g_service = ONLINE_SERVICE_CONNECTING;
    FailWaiters(net::kUnreachable);
    SetRelayEnds(nullptr, g.socket.lock());
    g.link.reset();
    g.down = g.up = g.lost = false;
    g.udp_inflight = g.udp_acked = false;
    g.reported_port = 0;
    g.reported_ip = 0;
    g.relay_capable = false;
    net::LobbyLink::Options options;
    options.url = s.config.lobby;
    options.hello = HelloBody;
    g.link = std::make_unique<net::LobbyLink>(std::move(options));
    g.link->set_binary_sink(ReceiveRelayFrame);
    SetRelayEnds(g.link.get(), g.socket.lock());
    g.link->start();
    std::printf("Online: connecting to %s\n", s.config.lobby.c_str());
    return true;
}

void HandleLobbyMessages() {
    if (!g.link) return;
    net::LobbyMessage m;
    while (g.link->poll(m)) {
        if (m.kind == net::LobbyMessage::Kind::kResponse) OnResponse(m);
        else if (m.kind == net::LobbyMessage::Kind::kEvent) OnEvent(m);
        else OnLink(m);
    }
}

std::mutex g_pumping;
thread_local bool t_pumping = false;

// Lobby answers for a thread that waits on one while no pump runs: game callbacks it queues still run only in pump().
void Drain() {
    std::unique_lock<std::mutex> serial(g_pumping, std::defer_lock);
    if (!t_pumping && !serial.try_lock()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    HandleLobbyMessages();
}

// ---- entry points ----
ALN_SYSV int32_t m2_initialize(const void* param) {
    (void)param;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.initialized = true;
    return 0;
}

ALN_SYSV int32_t m2_terminate() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g.initialized = false;
    return 0;
}

// CreateContext {own SceNpId* @0, communication id @8, passphrase @0x10, service label @0x18}.
ALN_SYSV int32_t m2_create_context(const void* param, uint16_t* context) {
    (void)param;
    if (!context) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    *context = kContextId;
    g.context = true;
    return 0;
}

ALN_SYSV int32_t m2_register_context_callback(void* callback, void* arg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g.context_cb = reinterpret_cast<ContextCallback>(callback);
    g.context_arg = arg;
    return 0;
}

ALN_SYSV int32_t m2_context_start(uint16_t context, uint64_t timeout) {
    (void)context, (void)timeout;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g.context) return kErrNotStarted;
    g.start_wanted = true;
    g_context_started = true;
    if (g.link && !g.down) {
        if (g.up) PushContextEvent(kCtxStarted, 0);
        return 0;
    }
    if (!StartLink()) {
        std::printf("Matching2: no lobby server is set; online play is unavailable\n");
        PushContextEvent(kCtxStartOver, kErrServiceDown);
        return 0;
    }
    std::printf("Matching2: going online\n");
    return 0;
}

ALN_SYSV int32_t m2_context_stop(uint16_t context) {
    (void)context;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g.room && LinkUsable()) {
        Json body = Json::Object();
        body.set("room", net::u64_text(g.room));
        Issue(Pending{Kind::kInternal}, "room.leave", std::move(body));
    }
    ClearRoom();
    g.start_wanted = false;
    g_context_started = false;
    g.reported_port = 0;
    g.udp_acked = false;
    PushContextEvent(kCtxStopped, 0);
    std::printf("Matching2: offline\n");
    return 0;
}

ALN_SYSV int32_t m2_destroy_context(uint16_t context) {
    (void)context;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.context = false;
    return 0;
}

ALN_SYSV int32_t m2_get_server_id(uint16_t context, uint16_t* server) {
    (void)context;
    if (!server) return kErrInvalid;
    *server = kServerId;
    return 0;
}

ALN_SYSV int32_t m2_set_default_request_opt(uint16_t context, const void* opt) {
    (void)context;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.default_cb = opt ? Rd<RequestCallback>(opt, 0) : nullptr;
    g.default_arg = opt ? Rd<void*>(opt, 8) : nullptr;
    return 0;
}

// GetWorldInfoList {server id u16 @0}: the lobby answers, so a world list proves the service is there.
ALN_SYSV int32_t m2_get_world_info_list(uint16_t context, const void* request, const void* opt, uint32_t* id) {
    (void)context, (void)request;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t r = Issue(Kind::kWorlds, "worlds", Json::Object(), opt, kEvWorlds);
    if (id) *id = r;
    return 0;
}

// CreateJoinRoom: max slots u16 @0, flags @8, world @0xc, internal attrs @0x60/@0x68, searchable int attrs @0x70/@0x78,
// external attrs @0x90/@0x98, member attrs @0xa0/@0xa8.
ALN_SYSV int32_t m2_create_join_room(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint16_t wanted = Rd<uint16_t>(request, 0);
    if (!wanted) return kErrInvalid;
    Json body = Json::Object();
    body.set("world", int64_t(std::max<uint32_t>(1, Rd<uint32_t>(request, 0xc))));
    body.set("slots", int64_t(std::clamp<uint16_t>(wanted, 1, 4)));
    body.set("flags", int64_t(Rd<uint32_t>(request, 8)));
    body.set("ints", IntAttrs(Rd<const uint8_t*>(request, 0x70), Rd<uint64_t>(request, 0x78)));
    body.set("bin_int", BinAttrs(Rd<const uint8_t*>(request, 0x60), Rd<uint64_t>(request, 0x68)));
    body.set("bin_ext", BinAttrs(Rd<const uint8_t*>(request, 0x90), Rd<uint64_t>(request, 0x98)));
    body.set("member_bin", BinAttrs(Rd<const uint8_t*>(request, 0xa0), Rd<uint64_t>(request, 0xa8)));
    std::printf("Matching2: create room (slots %u%s, flags 0x%08llx, search attributes %s)\n", wanted,
                wanted > 4 ? ", the lobby gives 4" : "", (unsigned long long)body["flags"].as_int(), body["ints"].dump().c_str());
    const uint32_t r = Issue(Kind::kCreateJoin, "room.create", std::move(body), opt, kEvCreateJoin);
    if (id) *id = r;
    return 0;
}

// JoinRoom: room id u64 @0, member attrs @0x18/@0x20.
ALN_SYSV int32_t m2_join_room(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    Json body = Json::Object();
    body.set("room", net::u64_text(Rd<uint64_t>(request, 0)));
    body.set("member_bin", BinAttrs(Rd<const uint8_t*>(request, 0x18), Rd<uint64_t>(request, 0x20)));
    std::printf("Matching2: join room %llu\n", (unsigned long long)Rd<uint64_t>(request, 0));
    const uint32_t r = Issue(Kind::kCreateJoin, "room.join", std::move(body), opt, kEvJoin);
    if (id) *id = r;
    return 0;
}

ALN_SYSV int32_t m2_leave_room(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t room = Rd<uint64_t>(request, 0);
    Json body = Json::Object();
    body.set("room", net::u64_text(room));
    std::printf("Matching2: leave room %llu\n", (unsigned long long)room);
    Pending p = GamePending(Kind::kLeave, opt, kEvLeave);
    p.room = room;
    const uint32_t r = Issue(p, "room.leave", std::move(body));
    if (id) *id = r;
    if (room == g.room) ClearRoom();
    return 0;
}

// SearchRoom: option u8 @0 (0x10 random order), world @4, start @0x10, max @0x14, flag filter @0x18, flag attr @0x1c,
// int filters @0x20/@0x28 (16 bytes: operator u8 @0, attr u16 @8, value u32 @0xc).
ALN_SYSV int32_t m2_search_room(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t start = std::max<uint32_t>(1, Rd<uint32_t>(request, 0x10));
    Json body = Json::Object();
    body.set("from", int64_t(start));
    body.set("count", int64_t(20));
    if (!g.join_code.empty()) {
        // A join code names one room: the game's own conditions do not apply.
        body.set("code", g.join_code);
        std::printf("Matching2: search for the room with code %s\n", g.join_code.c_str());
    } else {
        body.set("world", int64_t(Rd<uint32_t>(request, 4)));
        body.set("flag_mask", int64_t(Rd<uint32_t>(request, 0x18)));
        body.set("flag_value", int64_t(Rd<uint32_t>(request, 0x1c)));
        Json filters = Json::Array();
        const uint8_t* list = Rd<const uint8_t*>(request, 0x20);
        const uint64_t n = Rd<uint64_t>(request, 0x28);
        for (uint64_t i = 0; list && i < n && i < 16; ++i) {
            const uint8_t op = list[i * 16];
            if (op < 1 || op > 6) continue;
            Json f = Json::Array();
            f.push(int64_t(Rd<uint16_t>(list, size_t(i * 16 + 8))));
            f.push(int64_t(op));
            f.push(int64_t(Rd<uint32_t>(list, size_t(i * 16 + 0xc))));
            filters.push(std::move(f));
        }
        std::printf("Matching2: search %s (flags 0x%08llx under 0x%08llx)\n", filters.dump().c_str(),
                    (unsigned long long)body["flag_value"].as_int(), (unsigned long long)body["flag_mask"].as_int());
        body.set("filters", std::move(filters));
    }
    Pending p = GamePending(Kind::kSearch, opt, kEvSearch);
    p.random = (request[0] & 0x10) != 0;
    const uint32_t r = Issue(p, "room.search", std::move(body));
    if (id) *id = r;
    return 0;
}

// SetRoomDataInternal: room u64 @0, flag filter @8, flag attr @0xc, attrs @0x10/@0x18. The game makes a new room public
// here (or keeps it hidden for invitations).
ALN_SYSV int32_t m2_set_room_internal(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    Json body = Json::Object();
    body.set("room", net::u64_text(Rd<uint64_t>(request, 0)));
    body.set("flag_mask", int64_t(Rd<uint32_t>(request, 8)));
    body.set("flag_value", int64_t(Rd<uint32_t>(request, 0xc)));
    body.set("bin_int", BinAttrs(Rd<const uint8_t*>(request, 0x10), Rd<uint64_t>(request, 0x18)));
    std::printf("Matching2: room flags 0x%08llx under mask 0x%08llx\n", (unsigned long long)body["flag_value"].as_int(),
                (unsigned long long)body["flag_mask"].as_int());
    const uint32_t r = Issue(Kind::kGame, "room.set_internal", std::move(body), opt, kEvSetInternal);
    if (id) *id = r;
    return 0;
}

// SetRoomDataExternal: room u64 @0, int attrs @8/@0x10, external attrs @0x28/@0x30. Unchanged repeats are answered here.
ALN_SYSV int32_t m2_set_room_external(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    Json body = Json::Object();
    body.set("room", net::u64_text(Rd<uint64_t>(request, 0)));
    body.set("ints", IntAttrs(Rd<const uint8_t*>(request, 8), Rd<uint64_t>(request, 0x10)));
    body.set("bin_ext", BinAttrs(Rd<const uint8_t*>(request, 0x28), Rd<uint64_t>(request, 0x30)));
    const std::string text = body.dump();
    uint32_t r;
    if (text == g.last_external) {
        const Pending p = GamePending(Kind::kGame, opt, kEvSetExternal);
        Complete(p, 0);
        r = p.request;
    } else {
        g.last_external = text;
        r = Issue(Kind::kGame, "room.set_external", std::move(body), opt, kEvSetExternal);
    }
    if (id) *id = r;
    return 0;
}

// GetRoomDataExternalList: room ids @0 with their count @8.
ALN_SYSV int32_t m2_get_rooms_external(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    const uint64_t* ids = Rd<const uint64_t*>(request, 0);
    const uint64_t n = Rd<uint64_t>(request, 8);
    if (!ids || !n || n > 20) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    Json rooms = Json::Array();
    for (uint64_t i = 0; i < n; ++i) rooms.push(net::u64_text(ids[i]));
    Json body = Json::Object();
    body.set("rooms", std::move(rooms));
    const uint32_t r = Issue(Kind::kRoomsExternal, "room.external", std::move(body), opt, kEvRoomsExternal);
    if (id) *id = r;
    return 0;
}

// GetRoomMemberDataExternalList: room id @0.
ALN_SYSV int32_t m2_get_members_external(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    Json body = Json::Object();
    body.set("room", net::u64_text(Rd<uint64_t>(request, 0)));
    const uint32_t r = Issue(Kind::kMembersExternal, "room.members", std::move(body), opt, kEvMembersExternal);
    if (id) *id = r;
    return 0;
}

// SignalingGetPingInfo {room @0}: {server u16 @0, world @4, room @8, rtt u32 @0x10 (microseconds)}, from the links of
// the room's members: the worst measured round trip.
ALN_SYSV int32_t m2_ping_info(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto block = std::make_shared<Block>();
    uint8_t* data = block->alloc(0x20);
    int worst = -1;
    for (const auto& [member, m] : g.members)
        if (member != g.me) worst = std::max(worst, p2p_rtt_ms(member));
    Wr<uint16_t>(data, 0, kServerId);
    Wr<uint32_t>(data, 4, g.world);
    Wr<uint64_t>(data, 8, Rd<uint64_t>(request, 0));
    Wr<uint32_t>(data, 0x10, uint32_t(std::max(worst, 0)) * 1000);
    const Pending p = GamePending(Kind::kGame, opt, kEvPingInfo);
    Complete(p, 0, block, data);
    if (id) *id = p.request;
    return 0;
}

ALN_SYSV int32_t m2_register_room_callback(uint16_t context, void* callback, void* arg) {
    (void)context;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.room_cb = reinterpret_cast<RoomCallback>(callback);
    g.room_arg = arg;
    return 0;
}

ALN_SYSV int32_t m2_register_signaling_callback(uint16_t context, void* callback, void* arg) {
    (void)context;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.signal_cb = reinterpret_cast<SignalingCallback>(callback);
    g.signal_arg = arg;
    return 0;
}

// Room messages are never sent: the game imports no SendRoomMessage.
ALN_SYSV int32_t m2_register_message_callback(uint16_t context, void* callback, void* arg) {
    (void)context, (void)callback, (void)arg;
    return 0;
}

// (ctx, room, member, status*, address* and port* in network order). A member of the room is pending until its link is
// connected, then active at its room-wide virtual endpoint, which is what the P2P socket reports as its datagrams' source.
ALN_SYSV int32_t m2_connection_status(uint16_t context, uint64_t room, uint16_t member, int32_t* status, uint32_t* address,
                                      uint16_t* port) {
    (void)context;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g.room || room != g.room || member == g.me || !g.members.count(member)) return kErrNoMember;
    auto it = g.signals.find(member);
    const bool active = it != g.signals.end() && it->second.established;
    const net::Endpoint at = peer_endpoint(member);
    if (status) *status = active ? kSignalingActive : kSignalingPending;
    if (address) *address = active ? __builtin_bswap32(at.ip) : 0;
    if (port) *port = active ? uint16_t(at.port >> 8 | at.port << 8) : 0;
    return 0;
}

struct Export {
    const char* name;
    void* function;
};

#define M2(name, fn) {name, reinterpret_cast<void*>(&fn)}
const Export kExports[] = {
    M2("sceNpMatching2Initialize", m2_initialize),
    M2("sceNpMatching2Terminate", m2_terminate),
    M2("sceNpMatching2CreateContext", m2_create_context),
    M2("sceNpMatching2RegisterContextCallback", m2_register_context_callback),
    M2("sceNpMatching2ContextStart", m2_context_start),
    M2("sceNpMatching2ContextStop", m2_context_stop),
    M2("sceNpMatching2DestroyContext", m2_destroy_context),
    M2("sceNpMatching2GetServerId", m2_get_server_id),
    M2("sceNpMatching2SetDefaultRequestOptParam", m2_set_default_request_opt),
    M2("sceNpMatching2GetWorldInfoList", m2_get_world_info_list),
    M2("sceNpMatching2CreateJoinRoom", m2_create_join_room),
    M2("sceNpMatching2JoinRoom", m2_join_room),
    M2("sceNpMatching2LeaveRoom", m2_leave_room),
    M2("sceNpMatching2SearchRoom", m2_search_room),
    M2("sceNpMatching2SetRoomDataInternal", m2_set_room_internal),
    M2("sceNpMatching2SetRoomDataExternal", m2_set_room_external),
    M2("sceNpMatching2GetRoomDataExternalList", m2_get_rooms_external),
    M2("sceNpMatching2GetRoomMemberDataExternalList", m2_get_members_external),
    M2("sceNpMatching2SignalingGetPingInfo", m2_ping_info),
    M2("sceNpMatching2SignalingGetConnectionStatus", m2_connection_status),
    M2("sceNpMatching2RegisterRoomEventCallback", m2_register_room_callback),
    M2("sceNpMatching2RegisterSignalingCallback", m2_register_signaling_callback),
    M2("sceNpMatching2RegisterRoomMessageCallback", m2_register_message_callback),
};
#undef M2

} // namespace

void matching2_configure() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g.join_code.clear();
    if (const char* code = std::getenv("ALN_JOIN_CODE"); code && *code) {
        for (const char* p = code; *p && g.join_code.size() < 8; ++p)
            if (std::isalnum(static_cast<unsigned char>(*p))) g.join_code += char(std::toupper(static_cast<unsigned char>(*p)));
    }
}

void leave_for_exit() {
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !g.link) return;
    if (g.room && LinkUsable()) {
        Json body = Json::Object();
        body.set("room", net::u64_text(g.room));
        g.link->request("room.leave", std::move(body), true);
        ClearRoom();
    }
    if (g.link->drain(1000)) g.link->stop();
    std::printf("Matching2: left the lobby for exit\n");
}

void pump() {
    std::unique_lock<std::mutex> serial(g_pumping, std::try_to_lock);
    if (!serial.owns_lock()) return;
    t_pumping = true;
    std::vector<std::function<void()>> calls;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        HandleLobbyMessages();
        const uint64_t now = net::now_ms();
        if (g.presence && (!g.link || g.down) && now >= g.retry_ms) {
            g.retry_ms = now + 30000;
            StartLink();
        }
        ReportEndpoint(now);
        LinkMembers(now);
        calls.swap(g.calls);
    }
    for (auto& call : calls) call();
    sessions_pump();
    t_pumping = false;
}

void start_presence() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g.presence = true;
    if (!g.link || g.down) StartLink();
}

bool lobby_call(const std::string& op, Json body, Json* reply, std::string* code, int timeout_ms) {
    auto waiter = std::make_shared<Waiter>();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if ((!g.link || g.down) && !StartLink()) {
            if (code) *code = net::kUnreachable;
            return false;
        }
        g.waiters[g.link->request(op, std::move(body))] = waiter;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(waiter->mutex);
            if (waiter->done_cv.wait_for(lock, std::chrono::milliseconds(20), [&] { return waiter->done; })) break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto it = g.waiters.begin(); it != g.waiters.end(); ++it) {
                if (it->second == waiter) {
                    g.waiters.erase(it);
                    break;
                }
            }
            if (code) *code = "timeout";
            return false;
        }
        Drain();
    }
    if (code) *code = waiter->code;
    if (reply) *reply = waiter->body;
    return waiter->code == "ok";
}

void lobby_send(const std::string& op, Json body) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (LinkUsable()) g.link->request(op, std::move(body));
}

std::string own_build() { return settings().build; }

uintptr_t matching2_export(const std::string& symbol) {
    for (const Export& e : kExports)
        if (symbol == e.name) return reinterpret_cast<uintptr_t>(e.function);
    return 0;
}

} // namespace aln::online

extern "C" int runtime_np_signed_in(void);

extern "C" int runtime_online_status(void) {
    using namespace aln;
    using namespace aln::online;
    OnlineStatusInput in{};
    in.enabled = settings().enabled;
    in.service = g_service.load();
    in.np_signed_in = runtime_np_signed_in();
    in.matching2_started = g_context_started.load();
    const uint64_t since = g_connecting_since.load(), now = net::now_ms();
    in.connecting_ms = since && now > since ? now - since : 0;
    const int status = online_status_classify(&in);
    if (g_reported_status.exchange(status) != status) {
        const char* why = status == ONLINE_STATUS_CONNECTED ? "lobby signed in, NP signed in, Matching2 started"
                        : status == ONLINE_STATUS_OFF ? "Online Play is off"
                        : in.service == ONLINE_SERVICE_FAILED ? "the lobby is unavailable; it is tried again"
                        : status == ONLINE_STATUS_UNAVAILABLE ? "the lobby did not answer in time; still trying"
                        : in.service != ONLINE_SERVICE_READY ? "the lobby link is connecting"
                        : !in.np_signed_in ? "waiting for the NP sign-in" : "waiting for the game's Matching2 context";
        std::printf("Online status: %s (%s)\n", online_status_name(status), why);
    }
    return status;
}
