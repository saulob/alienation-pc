// The game's side of online play without the game: this test calls the classic Matching2, NpWebApi, Invitation dialog
// and libSceNet entry points the way Online_Matching_NP, Network_Room_Orbis, Online_Sessions_Orbis and
// Network_Connection_Manager do (request structs, callbacks pumped like sceNpCheckCallback, the DGRAM_P2P socket polled
// with a fresh epoll), while other players use the lobby protocol and connect over UDP. Needs
// ALN_TEST_LOBBY=ws://127.0.0.1:<port>/ pointing at a running development lobby (server/lobby/local/server.js); every
// socket is bound to loopback.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/identity.h"
#include "net/lobby_link.h"
#include "net/online_version.h"
#include "net/peer_socket.h"
#include "online_client.h"
#include "online_view.h"
#include "online_status.h"
#include "runtime_online.h"

#define SYSV __attribute__((sysv_abi))

namespace fs = std::filesystem;
using namespace aln;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

online::Settings g_settings;

template <class T> T Rd(const void* p, size_t offset) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(v));
    return v;
}
template <class T> void Wr(void* p, size_t offset, T v) { std::memcpy(static_cast<uint8_t*>(p) + offset, &v, sizeof(v)); }

template <class F> F Fn(const char* name) {
    uintptr_t f = online::matching2_export(name);
    if (!f) f = online::webapi_export(name);
    if (!f) f = online::p2p_export(name);
    if (!f) std::printf("missing export %s\n", name);
    return reinterpret_cast<F>(f);
}

constexpr char kEu[] = "CUSA00062-1.00";
constexpr char kKr[] = "CUSA04430-1.00";
constexpr uint32_t kPublic = 0x04000000, kHidden = 0x14000000;
constexpr int32_t kEAgain = int32_t(0x80410123);

struct Seen {
    std::vector<std::pair<uint16_t, int32_t>> context;
    struct Request {
        uint32_t id;
        uint16_t event;
        int32_t error;
        std::vector<uint8_t> copy;
        uint64_t room = 0, worlds = 0;
        uint32_t world = 0;
        uint16_t me = 0, slots = 0, server = 0;
        uint32_t count = 0, total = 0;
        std::string me_name, owner_name;
        std::vector<std::string> names;
    };
    std::vector<Request> requests;
    struct Room {
        uint16_t event;
        uint16_t member = 0;
        std::string name;
        uint32_t attr = 0;
        uint8_t cause = 0;
    };
    std::vector<Room> rooms;
    std::vector<std::pair<uint16_t, uint16_t>> signals; // member, event
} g_seen;

std::string NpName(const uint8_t* np_id) { return std::string(reinterpret_cast<const char*>(np_id), strnlen(reinterpret_cast<const char*>(np_id), 16)); }

SYSV void OnContext(uint16_t ctx, uint16_t event, uint8_t cause, int32_t error, void* arg) {
    (void)ctx, (void)cause, (void)arg;
    g_seen.context.push_back({event, error});
}

SYSV void OnRequest(uint16_t ctx, uint32_t id, uint16_t event, int32_t error, const void* data, void* arg) {
    (void)ctx, (void)arg;
    Seen::Request r{id, event, error, {}};
    if (data && !error && (event == 0x0101 || event == 0x0102)) {
        const uint8_t* internal = Rd<const uint8_t*>(data, 0);
        r.slots = Rd<uint16_t>(internal, 8);
        r.server = Rd<uint16_t>(internal, 0xa);
        r.world = Rd<uint32_t>(internal, 0xc);
        r.room = Rd<uint64_t>(internal, 0x18);
        const uint8_t* me = Rd<const uint8_t*>(data, 0x18);
        r.me = Rd<uint16_t>(me, 0x38);
        r.me_name = NpName(me + 0x10);
        for (const uint8_t* m = Rd<const uint8_t*>(data, 8); m; m = Rd<const uint8_t*>(m, 0)) r.names.push_back(NpName(m + 0x10));
        const uint8_t* attrs = Rd<const uint8_t*>(internal, 0x48);
        if (attrs && Rd<uint64_t>(internal, 0x50)) {
            const uint8_t* bytes = Rd<const uint8_t*>(attrs, 0x18);
            r.copy.assign(bytes, bytes + Rd<uint64_t>(attrs, 0x20));
        }
    }
    if (data && !error && event == 0x0002) {
        r.worlds = Rd<uint64_t>(data, 8);
        r.world = Rd<uint32_t>(Rd<const uint8_t*>(data, 0), 8);
    }
    if (data && !error && event == 0x0106) {
        r.total = Rd<uint32_t>(data, 4);
        r.count = Rd<uint32_t>(data, 8);
        const uint8_t* room = Rd<const uint8_t*>(data, 0x10);
        if (room) {
            r.room = Rd<uint64_t>(room, 0x20);
            r.slots = Rd<uint16_t>(room, 8);
            r.server = Rd<uint16_t>(room, 0x10);
            r.owner_name = NpName(Rd<const uint8_t*>(room, 0x40));
            const uint8_t* ints = Rd<const uint8_t*>(room, 0x58);
            r.copy.assign(ints, ints + 8 * Rd<uint64_t>(room, 0x60));
        }
    }
    if (data && !error && event == 0x0003)
        for (const uint8_t* m = Rd<const uint8_t*>(data, 0); m; m = Rd<const uint8_t*>(m, 0))
            r.names.push_back(NpName(m + 0x10) + (m[0x34] == 2 ? "*" : ""));
    if (data && !error && event == 0x0005) r.count = uint32_t(Rd<uint64_t>(data, 8));
    if (data && !error && event == 0x0103) r.room = Rd<uint64_t>(data, 0);
    g_seen.requests.push_back(std::move(r));
}

SYSV void OnRoom(uint16_t ctx, uint64_t room, uint16_t event, const void* data, void* arg) {
    (void)ctx, (void)room, (void)arg;
    Seen::Room r;
    r.event = event;
    if ((event == 0x1101 || event == 0x1102) && data) {
        const uint8_t* member = Rd<const uint8_t*>(data, 0);
        r.member = Rd<uint16_t>(member, 0x38);
        r.name = NpName(member + 0x10);
        const uint8_t* attrs = Rd<const uint8_t*>(member, 0x48);
        if (attrs && Rd<uint64_t>(member, 0x50) && Rd<uint64_t>(attrs, 0x18) >= 4)
            r.attr = Rd<uint32_t>(Rd<const uint8_t*>(attrs, 0x10), 0);
    }
    if ((event == 0x1103 || event == 0x1104) && data) r.cause = Rd<uint8_t>(data, 0);
    g_seen.rooms.push_back(r);
}

SYSV void OnSignal(uint16_t ctx, uint64_t room, uint16_t member, uint16_t event, int32_t error, void* arg) {
    (void)ctx, (void)room, (void)error, (void)arg;
    g_seen.signals.push_back({member, event});
}

bool PumpUntil(const std::function<bool()>& done, int timeout_ms = 8000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        online::pump();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return done();
}

const Seen::Request* RequestById(uint32_t id) {
    for (const auto& r : g_seen.requests)
        if (r.id == id) return &r;
    return nullptr;
}

const uint32_t kLoopback = net::parse_ip("127.0.0.1");

// A player of another PC on the same build, as a plain lobby session: creates or joins rooms by the lobby protocol.
struct Remote {
    net::LobbySession session;
    bool ok = false;
    Remote(const std::string& url, const std::filesystem::path& dir, const char* name, const char* build = "CUSA00062-1.00") {
        std::string error;
        ok = session.open(url, net::hello_body(net::load_identity(dir), name, net::kOnlineGame, true, true, false, build),
                          nullptr, &error);
        if (!ok) std::printf("remote %s: %s\n", name, error.c_str());
    }
    std::optional<net::Json> Call(const char* op, net::Json body, std::string* code) { return session.call(op, std::move(body), code); }
    uint64_t Create(int slots, uint32_t flags) {
        net::Json body = net::Json::Object();
        body.set("world", 1);
        body.set("slots", slots);
        body.set("flags", int64_t(flags));
        std::string code;
        const auto made = Call("room.create", body, &code);
        return made && code == "ok" ? (*made)["room"]["id"].as_u64() : 0;
    }
    std::string Join(uint64_t room, int* me = nullptr) {
        net::Json body = net::Json::Object();
        body.set("room", net::u64_text(room));
        std::string code;
        const auto joined = Call("room.join", body, &code);
        if (joined && me) *me = int((*joined)["me"].as_int());
        return code;
    }
    void Leave(uint64_t room) {
        net::Json body = net::Json::Object();
        body.set("room", net::u64_text(room));
        std::string code;
        Call("room.leave", body, &code);
    }
};

} // namespace

namespace aln::online {
const Settings& settings() { return g_settings; }
} // namespace aln::online

// The system event queue (runtime_services.c) and the overlay list (the video core), as the test sees them.
std::vector<std::vector<uint8_t>> g_system_events;
std::vector<std::pair<std::string, std::vector<std::string>>> g_choices;
int g_choice_answer = -1;
extern "C" int runtime_system_event_push(const void* event, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(event);
    g_system_events.emplace_back(bytes, bytes + size);
    return 1;
}
extern "C" int alngpu_choice_begin(const char* title, const char* const* items, const char* const* images, int count, int focus) {
    (void)images, (void)focus;
    g_choices.push_back({title, std::vector<std::string>(items, items + count)});
    return 1;
}
extern "C" int alngpu_choice_poll(void) { return g_choice_answer; }
extern "C" int runtime_online_status(void);
int g_np_signed_in = 0;
extern "C" int runtime_np_signed_in(void) { return g_np_signed_in; }
extern "C" int alngpu_overlay_captures_input(void) { return 0; }

namespace {

using Init = int32_t SYSV (*)(const void*);
using CreateContext = int32_t SYSV (*)(const void*, uint16_t*);
using RegisterCtx = int32_t SYSV (*)(void*, void*);
using Register = int32_t SYSV (*)(uint16_t, void*, void*);
using Start = int32_t SYSV (*)(uint16_t, uint64_t);
using Stop = int32_t SYSV (*)(uint16_t);
using ServerId = int32_t SYSV (*)(uint16_t, uint16_t*);
using Request = int32_t SYSV (*)(uint16_t, const void*, const void*, uint32_t*);
using Status = int32_t SYSV (*)(uint16_t, uint64_t, uint16_t, int32_t*, uint32_t*, uint16_t*);
using Socket = int32_t SYSV (*)(const char*, int32_t, int32_t, int32_t);
using SetOpt = int32_t SYSV (*)(int32_t, int32_t, int32_t, const void*, uint32_t);
using Bind = int32_t SYSV (*)(int32_t, const uint8_t*, uint32_t);
using SendTo = int64_t SYSV (*)(int32_t, const void*, uint64_t, int32_t, const uint8_t*, uint32_t);
using RecvFrom = int64_t SYSV (*)(int32_t, void*, uint64_t, int32_t, uint8_t*, uint32_t*);
using Close = int32_t SYSV (*)(int32_t);
using Abort = int32_t SYSV (*)(int32_t, int32_t);
using EpollCreate = int32_t SYSV (*)(const char*, int32_t);
using EpollControl = int32_t SYSV (*)(int32_t, int32_t, int32_t, const uint8_t*);
using EpollWait = int32_t SYSV (*)(int32_t, uint8_t*, int32_t, int32_t);

// Network_Connection_Manager's select: a new epoll per pass, IN|OUT on the socket, timeout 0.
uint32_t Select(int32_t sock) {
    const int32_t ep = Fn<EpollCreate>("sceNetEpollCreate")("select", 0);
    uint8_t ev[0x18] = {};
    Wr<uint32_t>(ev, 0, 3);
    Wr<uint32_t>(ev, 0x10, uint32_t(sock));
    CHECK(Fn<EpollControl>("sceNetEpollControl")(ep, 1, sock, ev) == 0);
    uint8_t out[0x18] = {};
    const int32_t n = Fn<EpollWait>("sceNetEpollWait")(ep, out, 1, 0);
    CHECK(Fn<Close>("sceNetEpollDestroy")(ep) == 0);
    if (n == 1) CHECK(Rd<int32_t>(out, 8) == sock && Rd<uint32_t>(out, 0x10) == uint32_t(sock));
    return n == 1 ? Rd<uint32_t>(out, 0) : 0;
}

void Address(uint8_t* out, uint32_t ip_be, uint16_t port_be, uint16_t vport) {
    std::memset(out, 0, 16);
    out[0] = 16;
    out[1] = 2;
    Wr<uint16_t>(out, 2, port_be);
    Wr<uint32_t>(out, 4, ip_be);
    Wr<uint16_t>(out, 8, uint16_t(vport >> 8 | vport << 8));
}

const Seen::Request* JoinAsGame(uint16_t ctx, void* const* opt, uint64_t room) {
    uint8_t request[0x28] = {};
    Wr<uint64_t>(request, 0, room);
    uint32_t id = 0;
    CHECK(Fn<Request>("sceNpMatching2JoinRoom")(ctx, request, opt, &id) == 0);
    CHECK(PumpUntil([&] { return RequestById(id); }));
    return RequestById(id);
}

void LeaveAsGame(uint16_t ctx, void* const* opt, uint64_t room) {
    uint8_t request[16] = {};
    Wr<uint64_t>(request, 0, room);
    uint32_t id = 0;
    CHECK(Fn<Request>("sceNpMatching2LeaveRoom")(ctx, request, opt, &id) == 0);
    CHECK(PumpUntil([&] { return RequestById(id); }));
    CHECK(RequestById(id) && RequestById(id)->error == 0);
}

bool SawRoomEvent(size_t from, uint16_t event, uint16_t member, const char* name = nullptr) {
    for (size_t i = from; i < g_seen.rooms.size(); ++i) {
        const Seen::Room& r = g_seen.rooms[i];
        if (r.event == event && r.member == member && (!name || r.name == name)) return true;
    }
    return false;
}

// Rooms of every size the game can meet, with the game joining through its own JoinRoom: the four-seat squad, a member
// leaving and taking the same seat again, two and three seats, a full room and a room that is gone.
void Squads(const char* url, const fs::path& base, uint16_t ctx, void* const* opt) {
    Remote host(url, base / "squad-host", "SquadHost"), second(url, base / "squad-2", "SquadTwo"),
        third(url, base / "squad-3", "SquadThree"), fifth(url, base / "squad-5", "SquadFive");
    CHECK(host.ok && second.ok && third.ok && fifth.ok);
    const uint64_t squad = host.Create(4, kPublic);
    int seat = 0;
    CHECK(squad != 0 && second.Join(squad, &seat) == "ok" && seat == 0x20);
    CHECK(third.Join(squad, &seat) == "ok" && seat == 0x30);
    const Seen::Request* four = JoinAsGame(ctx, opt, squad);
    CHECK(four && four->error == 0 && four->me == 0x40 && four->slots == 4);
    CHECK(four && four->names == std::vector<std::string>({"SquadHost", "SquadTwo", "SquadThree", "Alpha"}));
    CHECK(fifth.Join(squad) == "full");
    const size_t before = g_seen.rooms.size();
    third.Leave(squad);
    CHECK(PumpUntil([&] { return SawRoomEvent(before, 0x1102, 0x30); }));
    CHECK(third.Join(squad, &seat) == "ok" && seat == 0x30);
    CHECK(PumpUntil([&] { return SawRoomEvent(before, 0x1101, 0x30, "SquadThree"); }));
    CHECK(fifth.Join(squad) == "full");
    LeaveAsGame(ctx, opt, squad);
    CHECK(fifth.Join(squad, &seat) == "ok" && seat == 0x40);

    Remote duo_host(url, base / "duo-host", "DuoHost"), duo_late(url, base / "duo-late", "DuoLate");
    const uint64_t duo = duo_host.Create(2, kPublic);
    const Seen::Request* two = JoinAsGame(ctx, opt, duo);
    CHECK(two && two->error == 0 && two->me == 0x20 && two->slots == 2);
    CHECK(duo_late.Join(duo) == "full");
    LeaveAsGame(ctx, opt, duo);

    Remote trio_host(url, base / "trio-host", "TrioHost"), trio_guest(url, base / "trio-guest", "TrioGuest");
    const uint64_t trio = trio_host.Create(3, kPublic);
    CHECK(trio_guest.Join(trio) == "ok");
    const Seen::Request* three = JoinAsGame(ctx, opt, trio);
    CHECK(three && three->error == 0 && three->me == 0x30 && three->slots == 3);
    CHECK(duo_late.Join(trio) == "full");
    LeaveAsGame(ctx, opt, trio);

    Remote full_host(url, base / "full-host", "FullHost");
    const uint64_t taken = full_host.Create(2, kPublic);
    CHECK(duo_late.Join(taken) == "ok");
    const Seen::Request* full = JoinAsGame(ctx, opt, taken);
    CHECK(full && full->error == int32_t(0x80550d19));

    Remote gone_host(url, base / "gone-host", "GoneHost");
    const uint64_t gone = gone_host.Create(4, kPublic);
    gone_host.Leave(gone);
    const Seen::Request* vanished = JoinAsGame(ctx, opt, gone);
    CHECK(vanished && vanished->error == int32_t(0x80550d13));
}

// A guest that no direct path reaches (it reports a dead UDP port and dials no direct candidate): the link comes up
// through the lobby's relay, the game's signaling is established and datagrams still pass unchanged.
void RelayFallback(const char* url, const fs::path& base, uint16_t ctx, void* const* opt, int32_t sock) {
    uint8_t create[0xd0] = {};
    Wr<uint16_t>(create, 0, 2);
    Wr<uint32_t>(create, 8, kPublic);
    Wr<uint32_t>(create, 0xc, 1);
    uint32_t created = 0;
    CHECK(Fn<Request>("sceNpMatching2CreateJoinRoom")(ctx, create, opt, &created) == 0);
    CHECK(PumpUntil([&] { return RequestById(created); }));
    const Seen::Request* room = RequestById(created);
    CHECK(room && room->error == 0);
    if (!room || room->error) return;
    const uint64_t room_id = room->room;

    net::PeerSocket::Options options;
    options.bind_ip = kLoopback;
    options.relay_after_ms = 300;
    std::string error;
    std::shared_ptr<net::PeerSocket> socket = net::PeerSocket::open(options, &error);
    CHECK(socket != nullptr);
    if (!socket) return;
    const net::Identity id = net::load_identity(base / "relay-guest");
    net::LobbyLink::Options link_options;
    link_options.url = url;
    link_options.hello = [&] { return net::hello_body(id, "RelayGuest", net::kOnlineGame, true, true, false, kEu); };
    net::LobbyLink link(link_options);
    link.set_binary_sink([socket](const uint8_t* data, size_t size) { socket->deliver_relay(data, size); });
    link.start();
    std::vector<net::LobbyMessage> inbox;
    auto next = [&](const std::function<bool(const net::LobbyMessage&)>& match) -> std::optional<net::LobbyMessage> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            online::pump();
            net::LobbyMessage m;
            while (link.poll(m)) inbox.push_back(std::move(m));
            for (size_t i = 0; i < inbox.size(); ++i) {
                if (!match(inbox[i])) continue;
                net::LobbyMessage found = std::move(inbox[i]);
                inbox.erase(inbox.begin() + ptrdiff_t(i));
                return found;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::nullopt;
    };
    CHECK(next([](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kUp; }));
    net::Json udp = net::Json::Object();
    udp.set("port", int64_t(9));
    link.request("udp", udp);
    net::Json join = net::Json::Object();
    join.set("room", net::u64_text(room_id));
    const uint32_t join_seq = link.request("room.join", join);
    auto joined = next([&](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kResponse && m.seq == join_seq; });
    CHECK(joined && joined->code == "ok");
    if (!joined) return;
    net::RoomKey key{};
    std::vector<uint8_t> bytes;
    net::hex_decode(joined->body["room"]["key"].as_string(), bytes);
    if (bytes.size() == key.size()) std::memcpy(key.data(), bytes.data(), key.size());
    socket->set_room(key, id.account);
    socket->enable_relay(net::relay_token(key), [&link](std::vector<uint8_t>&& frame) { return link.send_binary(std::move(frame)); });
    socket->set_peer(g_settings.identity.account, {}, 0x10);
    const int guest_link = socket->connect(g_settings.identity.account);
    const size_t signals = g_seen.signals.size();
    CHECK(PumpUntil([&] {
        for (size_t i = signals; i < g_seen.signals.size(); ++i)
            if (g_seen.signals[i] == std::make_pair(uint16_t(0x20), uint16_t(0x5102))) return true;
        return false;
    }, 20000));
    CHECK(PumpUntil([&] { return socket->state(guest_link) == net::LinkState::kConnected; }));
    CHECK(socket->path(guest_link) == net::LinkPath::kRelay);
    int32_t status = 0;
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    CHECK(Fn<Status>("sceNpMatching2SignalingGetConnectionStatus")(ctx, room_id, 0x20, &status, &peer_ip, &peer_port) == 0);
    CHECK(status == 2);
    uint8_t to[16];
    Address(to, peer_ip, peer_port, 1872);
    const std::string payload(900, 'r');
    CHECK(Fn<SendTo>("sceNetSendto")(sock, payload.data(), payload.size(), 0, to, 16) == int64_t(payload.size()));
    std::vector<uint8_t> got;
    CHECK(PumpUntil([&] { return socket->receive(guest_link, got); }));
    CHECK(std::string(got.begin(), got.end()) == payload);
    const std::string back(300, 'b');
    CHECK(socket->send(guest_link, back.data(), back.size()));
    uint8_t buffer[0x1800], from[16];
    uint32_t from_size = 16;
    int64_t read = -1;
    CHECK(PumpUntil([&] {
        from_size = 16;
        read = Fn<RecvFrom>("sceNetRecvfrom")(sock, buffer, sizeof(buffer), 0, from, &from_size);
        return read > 0;
    }));
    CHECK(read == int64_t(back.size()) && Rd<uint32_t>(from, 4) == peer_ip && Rd<uint16_t>(from, 2) == peer_port);
    LeaveAsGame(ctx, opt, room_id);
    socket.reset();
    link.stop();
}

// A player of another PC with events: a lobby link, its answers and the events it hears.
struct Player {
    net::Identity id;
    net::LobbyLink link;
    std::vector<net::LobbyMessage> inbox;
    std::string code;
    static net::LobbyLink::Options Options(const std::string& url, const net::Identity& id, const char* name, const char* build) {
        net::LobbyLink::Options o;
        o.url = url;
        o.hello = [id, name = std::string(name), build = std::string(build)] {
            return net::hello_body(id, name, net::kOnlineGame, true, true, false, build);
        };
        return o;
    }
    Player(const std::string& url, const fs::path& dir, const char* name, const char* build = "CUSA00062-1.00")
        : id(net::load_identity(dir)), link(Options(url, id, name, build)) {
        link.start();
        if (auto up = Next([](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kUp; }))
            code = up->body["friend_code"].as_string();
    }
    ~Player() { link.stop(); }
    std::optional<net::LobbyMessage> Next(const std::function<bool(const net::LobbyMessage&)>& match, int seconds = 10) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            online::pump();
            net::LobbyMessage m;
            while (link.poll(m)) inbox.push_back(std::move(m));
            for (size_t i = 0; i < inbox.size(); ++i) {
                if (!match(inbox[i])) continue;
                net::LobbyMessage found = std::move(inbox[i]);
                inbox.erase(inbox.begin() + ptrdiff_t(i));
                return found;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::nullopt;
    }
    net::Json Call(const char* op, net::Json body, std::string* code_out = nullptr) {
        const uint32_t seq = link.request(op, std::move(body));
        auto m = Next([&](const net::LobbyMessage& r) { return r.kind == net::LobbyMessage::Kind::kResponse && r.seq == seq; });
        if (code_out) *code_out = m ? m->code : "timeout";
        return m ? m->body : net::Json::Object();
    }
    std::optional<net::LobbyMessage> Event(const char* name) {
        return Next([&](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kEvent && m.event == name; });
    }
};

using WebInit = int32_t SYSV (*)(int32_t, uint64_t);
using WebContext = int32_t SYSV (*)(int32_t, const void*);
using WebCreate = int32_t SYSV (*)(int32_t, const char*, const char*, int32_t, const void*, int64_t*);
using WebSend = int32_t SYSV (*)(int64_t, const void*, uint64_t, void*);
using WebRead = int32_t SYSV (*)(int64_t, void*, uint64_t);
using WebDelete = int32_t SYSV (*)(int64_t);
using ParseNpId = int32_t SYSV (*)(const char*, uint8_t*);
using DialogCall = int32_t SYSV (*)();
using DialogOpen = int32_t SYSV (*)(const uint8_t*);
using DialogResult = int32_t SYSV (*)(uint8_t*);

struct WebReply {
    int32_t ret = -1, status = 0;
    std::string body;
};

// One request as Web_Api.cpp makes it: CreateRequest, SendRequest2 (only httpStatus read), ReadData until 0, delete.
WebReply Web(int32_t user_ctx, const char* group, const char* path, int method, const std::string& body = "",
             const char* type = nullptr) {
    WebReply reply;
    uint8_t content[32] = {};
    Wr<uint64_t>(content, 0, body.size());
    Wr<const char*>(content, 8, type ? type : "application/json; charset=utf-8");
    int64_t id = 0;
    reply.ret = Fn<WebCreate>("sceNpWebApiCreateRequest")(user_ctx, group, path, method, body.empty() ? nullptr : content, &id);
    if (reply.ret < 0 || id <= 0) return reply;
    uint8_t info[32] = {};
    reply.ret = Fn<WebSend>("sceNpWebApiSendRequest2")(id, body.data(), body.size(), info);
    reply.status = Rd<int32_t>(info, 0);
    char chunk[0x2000];
    for (int32_t n; (n = Fn<WebRead>("sceNpWebApiReadData")(id, chunk, sizeof(chunk))) > 0;) reply.body.append(chunk, size_t(n));
    CHECK(Fn<WebDelete>("sceNpWebApiDeleteRequest")(id) == 0);
    return reply;
}

std::string SessionDataBytes(uint64_t room, uint8_t seats) {
    std::string data(16, '\x5a');
    std::memcpy(data.data(), &room, 8);
    const uint32_t world = 1;
    const uint16_t server = 1;
    std::memcpy(data.data() + 8, &world, 4);
    std::memcpy(data.data() + 12, &server, 2);
    data[14] = char(seats);
    return data;
}

// POST /v1/sessions exactly as Online_Sessions_Orbis writes it (two Content-Type lines in the first part, an image).
std::string SessionPost(const char* privacy, uint8_t seats, const std::string& data) {
    const std::string boundary = "--xyz123xyz123xyz123";
    const std::string json = std::string("{\"sessionName\":\"Squad\",\"sessionType\":\"owner-bind\",\"sessionPrivacy\":\"") + privacy +
                             "\",\"sessionMaxUser\":" + std::to_string(seats) +
                             ",\"sessionStatus\":\"Mission\",\"availablePlatforms\":[\"PS4\"],\"localizedSessionNames\":[],"
                             "\"localizedSessionStatus\":[]}\n\n";
    const std::string jpeg("\xff\xd8\xff\xe0 not really a picture \n\n--x \xff\xd9", 32);
    return boundary + "\nContent-Type:application/json;charset=utf-8\nContent-Description:session-request\nContent-Type:image/jpg\n" +
           "Content-Length:" + std::to_string(json.size()) + "\n\n" + json + boundary +
           "\nContent-Type:image/jpeg\nContent-Description:session-image\nContent-Disposition:attachment\nContent-Length:" +
           std::to_string(jpeg.size()) + "\n\n" + jpeg + "\n" + boundary +
           "\nContent-Type:application/octet-stream\nContent-Description:session-data\nContent-Disposition:attachment\n"
           "Content-Length:16\n\n" + data + "\n" + boundary + "--\n";
}

std::string Field(const std::string& json, const char* key) {
    const std::optional<net::Json> j = net::Json::parse(json);
    return j ? (*j)[key].as_string() : std::string();
}

const Seen::Request* CreateAsGame(uint16_t ctx, void* const* opt, uint16_t slots) {
    uint8_t create[0xd0] = {};
    Wr<uint16_t>(create, 0, slots);
    Wr<uint32_t>(create, 8, kHidden);
    Wr<uint32_t>(create, 0xc, 1);
    uint32_t id = 0;
    CHECK(Fn<Request>("sceNpMatching2CreateJoinRoom")(ctx, create, opt, &id) == 0);
    CHECK(PumpUntil([&] { return RequestById(id); }));
    return RequestById(id);
}

// The Invitation dialog in send mode for `session`, as the game opens it: the overlay's title and rows, then cancelled.
std::pair<std::string, std::vector<std::string>> InviteDialog(const std::string& session) {
    std::vector<char> sid(0x30, 0);
    std::memcpy(sid.data(), session.data(), std::min(session.size(), sid.size() - 1));
    uint8_t data[0x20] = {};
    Wr<const char*>(data, 0, "Join my squad");
    Wr<const char*>(data, 8, sid.data());
    Wr<uint32_t>(data, 0x18, 3);
    uint8_t param[0x90] = {};
    Wr<uint32_t>(param, 0x34, 1);
    Wr<const uint8_t*>(param, 0x48, data);
    g_choices.clear();
    g_choice_answer = -1;
    CHECK(Fn<DialogCall>("sceInvitationDialogInitialize")() == 0);
    CHECK(Fn<DialogOpen>("sceInvitationDialogOpen")(param) == 0);
    g_choice_answer = -2;
    CHECK(Fn<DialogCall>("sceInvitationDialogUpdateStatus")() == 3);
    CHECK(Fn<DialogCall>("sceInvitationDialogTerminate")() == 0);
    g_choice_answer = -1;
    return g_choices.empty() ? std::pair<std::string, std::vector<std::string>>{} : g_choices.back();
}

bool InviteRowsBecome(const std::string& session, const std::vector<std::string>& rows) {
    return PumpUntil([&] { return InviteDialog(session).second == rows; }, 12000);
}

// The A/B log: Alpha creates a 4-seat squad (hidden, its public session, then public) and Bravo's search finds it.
void PublicSquad(const char* url, const fs::path& base, uint16_t ctx, void* const* opt) {
    const int32_t user = Fn<WebContext>("sceNpWebApiCreateContext")(Fn<WebInit>("sceNpWebApiInitialize")(1, 0x20000),
                                                                     g_settings.online_id.c_str());
    const Seen::Request* hosted = CreateAsGame(ctx, opt, 4);
    CHECK(hosted && hosted->error == 0 && hosted->slots == 4);
    if (!hosted || hosted->error) return;
    const uint64_t room = hosted->room;
    const WebReply made = Web(user, "sessionInvitation", "/v1/sessions", 1, SessionPost("public", 4, SessionDataBytes(room, 4)),
                              "multipart/mixed; boundary=xyz123xyz123xyz123");
    CHECK(made.status == 200 && Field(made.body, "sessionId").size() == 36);
    uint8_t make_public[0x20] = {};
    Wr<uint64_t>(make_public, 0, room);
    Wr<uint32_t>(make_public, 8, kHidden);
    Wr<uint32_t>(make_public, 0xc, kPublic);
    uint32_t published = 0;
    CHECK(Fn<Request>("sceNpMatching2SetRoomDataInternal")(ctx, make_public, opt, &published) == 0);
    CHECK(PumpUntil([&] { return RequestById(published); }));
    CHECK(RequestById(published) && RequestById(published)->error == 0);
    Player bravo(url, base / "public-bravo", "Bravo");
    net::Json search = net::Json::Object();
    search.set("world", 1);
    search.set("flag_mask", int64_t(0x60000000));
    std::string code;
    const net::Json found = bravo.Call("room.search", search, &code);
    size_t matches = 0;
    for (const net::Json& r : found["rooms"].items()) matches += r["id"].as_u64() == room && r["slots"].as_int() == 4;
    CHECK(code == "ok" && found["total"].as_int() >= 1 && matches == 1);
    LeaveAsGame(ctx, opt, room);
    CHECK(bravo.Call("room.search", search, &code)["rooms"].size() + 1 == found["rooms"].size());
}

// NP sessions as the game uses them for every room, the Invitation dialog, invitations received while playing, the
// userProfile friend list and the launcher's Join.
void Sessions(const char* url, const fs::path& base, uint16_t ctx, void* const* opt) {
    const int32_t lib = Fn<WebInit>("sceNpWebApiInitialize")(1, 0x20000);
    CHECK(lib > 0);
    const int32_t user = Fn<WebContext>("sceNpWebApiCreateContext")(lib, g_settings.online_id.c_str());
    CHECK(user > 0);

    // Hosting: CreateJoinRoom, then POST /v1/sessions; a player of another PC reads the session and joins the room.
    const Seen::Request* hosted = CreateAsGame(ctx, opt, 4);
    CHECK(hosted && hosted->error == 0);
    if (!hosted || hosted->error) return;
    const uint64_t room = hosted->room;
    const WebReply made = Web(user, "sessionInvitation", "/v1/sessions", 1, SessionPost("public", 4, SessionDataBytes(room, 4)),
                              "multipart/mixed; boundary=xyz123xyz123xyz123");
    const std::string session = Field(made.body, "sessionId");
    CHECK(made.ret == 0 && made.status == 200 && session.size() == 36 && session.size() <= 45);
    Player bravo(url, base / "session-bravo", "Bravo");
    net::Json ask = net::Json::Object();
    ask.set("session", session);
    std::string code;
    bravo.Call("session.join", ask, &code);
    CHECK(code == "ok");
    const net::Json data = bravo.Call("session.data", ask, &code);
    std::vector<uint8_t> bytes;
    net::hex_decode(data["data"].as_string(), bytes);
    uint64_t named = 0;
    if (bytes.size() == 16) std::memcpy(&named, bytes.data(), 8);
    CHECK(code == "ok" && bytes.size() == 16 && named == room && bytes[14] == 4);

    // The squad's invite-only setting changes the session's privacy; a name/status update is accepted as is.
    const std::string session_path = "/v1/sessions/" + session;
    CHECK(Web(user, "sessionInvitation", session_path.c_str(), 2, "{\"sessionPrivacy\":\"private\"}\n").status == 204);
    Player stranger(url, base / "session-stranger", "Stranger");
    stranger.Call("session.join", ask, &code);
    CHECK(code == "forbidden");
    CHECK(Web(user, "sessionInvitation", session_path.c_str(), 2, "{\"sessionPrivacy\":\"public\"}\n").status == 204);
    CHECK(Web(user, "sessionInvitation", session_path.c_str(), 2, "{\"sessionName\":\"Squad\",\"sessionStatus\":\"x\"}\n").status == 204);

    // Before any friendship the Invitation dialog says why nobody is listed: none added, a request waiting, a friend on
    // another game build, a friend offline. Being in the same rooms or lobby makes nobody a friend.
    CHECK(InviteRowsBecome(session, {"No friends added: add one by friend code in the launcher's Friends tab"}));
    CHECK(InviteDialog(session).first == "Invite a friend to your squad\nno friends");
    net::Json add = net::Json::Object();
    add.set("code", bravo.code);
    CHECK(online::lobby_call("friends.add", add, nullptr, &code) && code == "ok");
    CHECK(InviteRowsBecome(session, {"No friends yet: a friend request is waiting in the launcher's Friends tab"}));
    {
        Player charlie(url, base / "session-charlie", "Charlie", kKr);
        net::Json add_charlie = net::Json::Object();
        add_charlie.set("code", charlie.code);
        CHECK(online::lobby_call("friends.add", add_charlie, nullptr, &code) && code == "ok");
        net::Json accept_host = net::Json::Object();
        accept_host.set("account", net::u64_text(g_settings.identity.account));
        charlie.Call("friends.accept", accept_host, &code);
        CHECK(code == "ok");
        CHECK(InviteRowsBecome(session, {"Friends are online on another game version"}));
        CHECK(InviteDialog(session).first ==
              "Invite a friend to your squad\n1 friend: 0 online with your game version, 1 on another version, 0 offline");
        net::Json remove = net::Json::Object();
        remove.set("account", net::u64_text(charlie.id.account));
        charlie.link.stop();
        CHECK(InviteRowsBecome(session, {"No friend is online right now"}));
        CHECK(online::lobby_call("friends.remove", remove, nullptr, &code) && code == "ok");
    }

    // Friends through the normal request and accept, then the friend list as userProfile answers it; its npId parses to
    // the friend's online id.
    CHECK(online::lobby_call("friends.add", add, nullptr, &code) && code == "ok");
    net::Json accept = net::Json::Object();
    accept.set("account", net::u64_text(g_settings.identity.account));
    bravo.Call("friends.accept", accept, &code);
    CHECK(code == "ok");
    for (int i = 0; i < 20; ++i) online::pump(), std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const std::string list_path = "/v1/users/" + g_settings.online_id +
                                  "/friendList?friendStatus=friend&fields=npId,region,personalDetail&presenceType=incontext&offset=0&limit=250";
    const WebReply list = Web(user, "userProfile", list_path.c_str(), 0);
    const std::optional<net::Json> friends = net::Json::parse(list.body);
    CHECK(list.status == 200 && friends && (*friends)["totalResults"].as_int() == 1);
    if (friends && (*friends)["friendList"].size() == 1) {
        const net::Json& row = (*friends)["friendList"].items()[0];
        CHECK(row["presence"]["onlineStatus"].as_string() == "online");
        uint8_t np_id[0x24];
        CHECK(Fn<ParseNpId>("sceNpWebApiUtilityParseNpId")(row["npId"].as_string().c_str(), np_id) == 0);
        CHECK(NpName(np_id) == "Bravo");
    }
    const WebReply presence = Web(user, "userProfile", "/v1/users/Bravo/presence?type=incontext", 0);
    CHECK(presence.status == 200 && presence.body.find("\"online\"") != std::string::npos);
    CHECK(Web(user, "userProfile", "/v1/users/Alpha/blockList?fields=npId&offset=0", 0).body.find("\"totalResults\":0") !=
          std::string::npos);
    CHECK(Web(user, "titleSmallStorage", "/v1/whatever", 0).status == 404);

    // The Invitation dialog (send): the game's param, the friend picked in the overlay list, the friend hears it.
    std::vector<char> sid_buffer(0x30, 0);
    std::memcpy(sid_buffer.data(), session.data(), session.size());
    const char* message = "Join my squad";
    uint8_t dialog_data[0x20] = {};
    Wr<const char*>(dialog_data, 0, message);
    Wr<const char*>(dialog_data, 8, sid_buffer.data());
    Wr<uint32_t>(dialog_data, 0x10, 2);
    Wr<uint32_t>(dialog_data, 0x18, 3);
    uint8_t param[0x90] = {};
    Wr<uint64_t>(param, 0, 0x30);
    Wr<uint32_t>(param, 0x30, 0x90);
    Wr<uint32_t>(param, 0x34, 1);
    Wr<int32_t>(param, 0x38, 1);
    Wr<const uint8_t*>(param, 0x48, dialog_data);
    CHECK(Fn<DialogCall>("sceInvitationDialogInitialize")() == 0);
    g_choices.clear();
    g_choice_answer = -1;
    CHECK(Fn<DialogOpen>("sceInvitationDialogOpen")(param) == 0);
    CHECK(Fn<DialogCall>("sceInvitationDialogUpdateStatus")() == 2);
    CHECK(g_choices.size() == 1 && g_choices[0].second == std::vector<std::string>{"Bravo \xC2\xB7 online"});
    CHECK(g_choices.size() == 1 &&
          g_choices[0].first == "Invite a friend to your squad\n1 friend: 1 online with your game version, 0 on another version, 0 offline");
    g_choice_answer = 0;
    CHECK(Fn<DialogCall>("sceInvitationDialogUpdateStatus")() == 3);
    uint32_t sent = 0;
    uint8_t result[0x38] = {};
    Wr<uint32_t*>(result, 0x10, &sent);
    CHECK(Fn<DialogResult>("sceInvitationDialogGetResult")(result) == 0);
    CHECK(Rd<uint32_t>(result, 8) == 0 && Rd<uint32_t>(result, 0xc) == 0 && sent == 1);
    CHECK(Fn<DialogCall>("sceInvitationDialogTerminate")() == 0);
    const auto invited = bravo.Event("invite.received");
    CHECK(invited && invited->body["session"].as_string() == session && invited->body["name"].as_string() == "Alpha" &&
          invited->body["message"].as_string() == message);
    // Bravo accepts and joins the room the session names, like the game would.
    if (invited) {
        net::Json answer = net::Json::Object();
        answer.set("invitation", invited->body["invitation"].as_string());
        answer.set("accept", true);
        const net::Json accepted = bravo.Call("invite.answer", answer, &code);
        CHECK(code == "ok" && accepted["session"].as_string() == session);
    }
    net::Json join = net::Json::Object();
    join.set("room", net::u64_text(room));
    bravo.Call("room.join", join, &code);
    CHECK(code == "ok");
    CHECK(PumpUntil([&] { return SawRoomEvent(0, 0x1101, 0x20, "Bravo"); }));
    CHECK(Web(user, "sessionInvitation", session_path.c_str(), 3).status == 204);
    LeaveAsGame(ctx, opt, room);
    bravo.Call("room.leave", join, &code);

    // Joining: Bravo hosts a private squad and invites the game; the invitation is prompted in game, accepted, and reaches
    // the game as SESSION_INVITATION with both ids; the game's join then goes POST members, GET sessionData, JoinRoom.
    net::Json squad = net::Json::Object();
    squad.set("world", 1);
    squad.set("slots", 4);
    squad.set("flags", int64_t(kHidden));
    const net::Json squad_room = bravo.Call("room.create", squad, &code);
    const uint64_t bravo_room = squad_room["room"]["id"].as_u64();
    CHECK(code == "ok" && bravo_room != 0);
    net::Json bravo_session = net::Json::Object();
    bravo_session.set("private", true);
    bravo_session.set("max", 4);
    const std::string bravo_data = SessionDataBytes(bravo_room, 4);
    bravo_session.set("data", net::hex_encode(bravo_data.data(), bravo_data.size()));
    const std::string private_session = bravo.Call("session.create", bravo_session, &code)["session"].as_string();
    CHECK(code == "ok" && !private_session.empty());
    net::Json invite = net::Json::Object();
    invite.set("session", private_session);
    net::Json to = net::Json::Array();
    to.push(net::u64_text(g_settings.identity.account));
    invite.set("to", to);
    g_choices.clear();
    g_choice_answer = -1;
    g_system_events.clear();
    bravo.Call("invite.send", invite, &code);
    CHECK(code == "ok");
    CHECK(PumpUntil([] { return !g_choices.empty(); }));
    CHECK(!g_choices.empty() && g_choices[0].first == "Bravo invites you to their squad" &&
          g_choices[0].second == std::vector<std::string>({"Join squad", "Decline"}));
    g_choice_answer = 0;
    CHECK(PumpUntil([] { return !g_system_events.empty(); }));
    std::string invitation;
    if (!g_system_events.empty()) {
        const std::vector<uint8_t>& event = g_system_events[0];
        CHECK(event.size() >= 0x94 && Rd<uint32_t>(event.data(), 0) == 0x10000002);
        CHECK(std::string(reinterpret_cast<const char*>(event.data() + 4)) == private_session);
        invitation = reinterpret_cast<const char*>(event.data() + 0x34);
        CHECK(invitation.size() == 32 && Rd<uint32_t>(event.data(), 0x74) == 1 && Rd<int32_t>(event.data(), 0x90) == 1);
    }
    const auto answered = bravo.Event("invite.answered");
    CHECK(answered && answered->body["accept"].as_bool());
    const std::string members_path = "/v1/sessions/" + private_session + "/members";
    CHECK(Web(user, "sessionInvitation", members_path.c_str(), 1).status == 204);
    const std::string data_path = "/v1/sessions/" + private_session + "/sessionData";
    const WebReply session_data = Web(user, "sessionInvitation", data_path.c_str(), 0);
    CHECK(session_data.status == 200 && session_data.body == bravo_data);
    uint64_t target = 0;
    if (session_data.body.size() >= 8) std::memcpy(&target, session_data.body.data(), 8);
    const Seen::Request* joined = JoinAsGame(ctx, opt, target);
    CHECK(joined && joined->error == 0 && joined->me == 0x20 && target == bravo_room);
    const std::string used_path = "/v1/users/" + g_settings.online_id + "/invitations/" + invitation;
    CHECK(Web(user, "sessionInvitation", used_path.c_str(), 2, "{\"usedFlag\":true}").status == 204);
    LeaveAsGame(ctx, opt, bravo_room);

    // A declined invitation answers the inviter and closes nothing else.
    g_choices.clear();
    g_choice_answer = -1;
    bravo.Call("invite.send", invite, &code);
    CHECK(PumpUntil([] { return !g_choices.empty(); }));
    g_choice_answer = 1;
    const auto declined = bravo.Event("invite.answered");
    CHECK(declined && !declined->body["accept"].as_bool());
    g_choice_answer = -1;

    // The launcher's Join: online_join.ini in the profile becomes SESSION_INVITATION without an invitation id; an old
    // one is dropped.
    const fs::path profile = base / "join-profile";
    fs::create_directories(profile);
    {
        std::ofstream file(profile / "online_join.ini");
        file << "session=" << private_session << "\ninvitation=\ntime=" << std::time(nullptr) << "\n";
    }
    g_system_events.clear();
    online::load_pending_join(profile.string());
    CHECK(!fs::exists(profile / "online_join.ini"));
    CHECK(PumpUntil([] { return !g_system_events.empty(); }));
    if (!g_system_events.empty()) {
        CHECK(std::string(reinterpret_cast<const char*>(g_system_events[0].data() + 4)) == private_session);
        CHECK(Rd<uint32_t>(g_system_events[0].data(), 0x74) == 0 && g_system_events[0][0x34] == 0);
    }
    {
        std::ofstream file(profile / "online_join.ini");
        file << "session=" << private_session << "\ntime=" << (std::time(nullptr) - 3600) << "\n";
    }
    g_system_events.clear();
    online::load_pending_join(profile.string());
    for (int i = 0; i < 5; ++i) online::pump();
    CHECK(g_system_events.empty());

    // The launcher's Accept and play: the accepted invitation waits in online_join.ini while the game starts, reaches
    // the game as an accepted SESSION_INVITATION, and the session leads to Bravo's room.
    {
        std::ofstream file(profile / "online_join.ini");
        file << "session=" << private_session << "\ninvitation=" << invitation << "\ntime=" << std::time(nullptr) << "\n";
    }
    g_system_events.clear();
    online::load_pending_join(profile.string());
    CHECK(!fs::exists(profile / "online_join.ini"));
    CHECK(PumpUntil([] { return !g_system_events.empty(); }));
    if (!g_system_events.empty()) {
        const std::vector<uint8_t>& event = g_system_events[0];
        CHECK(std::string(reinterpret_cast<const char*>(event.data() + 4)) == private_session);
        CHECK(std::string(reinterpret_cast<const char*>(event.data() + 0x34)) == invitation && Rd<uint32_t>(event.data(), 0x74) == 1);
    }
    CHECK(Web(user, "sessionInvitation", members_path.c_str(), 1).status == 204);
    const WebReply launched = Web(user, "sessionInvitation", data_path.c_str(), 0);
    uint64_t launched_room = 0;
    if (launched.body.size() >= 8) std::memcpy(&launched_room, launched.body.data(), 8);
    const Seen::Request* rejoined = JoinAsGame(ctx, opt, launched_room);
    CHECK(rejoined && rejoined->error == 0 && launched_room == bravo_room);
    LeaveAsGame(ctx, opt, bravo_room);
    bravo.Call("room.leave", net::Json::Object(), &code);
}

// The game hosts and invites three friends at once: each invitation is its own, a decline or a later invitation touches
// no other, the room fills at four seats, and a seat a guest leaves is taken again.
void Invites(const char* url, const fs::path& base, uint16_t ctx, void* const* opt) {
    Player bravo(url, base / "invite-bravo", "Bravo"), charlie(url, base / "invite-charlie", "Charlie"),
        delta(url, base / "invite-delta", "Delta"), echo(url, base / "invite-echo", "Echo");
    Player* guests[] = {&bravo, &charlie, &delta};
    std::string code;
    for (Player* guest : guests) {
        net::Json add = net::Json::Object();
        add.set("code", guest->code);
        CHECK(online::lobby_call("friends.add", add, nullptr, &code) && code == "ok");
        net::Json accept = net::Json::Object();
        accept.set("account", net::u64_text(g_settings.identity.account));
        guest->Call("friends.accept", accept, &code);
        CHECK(code == "ok");
    }
    const Seen::Request* hosted = CreateAsGame(ctx, opt, 4);
    CHECK(hosted && hosted->error == 0);
    if (!hosted || hosted->error) return;
    const uint64_t room = hosted->room;
    net::Json made = net::Json::Object();
    made.set("private", true);
    made.set("max", 4);
    const std::string data = SessionDataBytes(room, 4);
    made.set("data", net::hex_encode(data.data(), data.size()));
    net::Json created;
    CHECK(online::lobby_call("session.create", made, &created, &code) && code == "ok");
    const std::string session = created["session"].as_string();
    const auto invite = [&](std::initializer_list<Player*> who) {
        net::Json body = net::Json::Object();
        body.set("session", session);
        net::Json to = net::Json::Array();
        for (Player* p : who) to.push(net::u64_text(p->id.account));
        body.set("to", to);
        return online::lobby_call("invite.send", body, nullptr, &code) && code == "ok";
    };
    const auto received = [&](Player& p) {
        const auto event = p.Event("invite.received");
        return event && event->body["session"].as_string() == session ? event->body["invitation"].as_string() : std::string();
    };
    const auto answer = [&](Player& p, const std::string& invitation, bool accept) {
        net::Json body = net::Json::Object();
        body.set("invitation", invitation);
        body.set("accept", accept);
        p.Call("invite.answer", body, &code);
        return code;
    };
    const auto join = [&](Player& p) {
        net::Json ask = net::Json::Object();
        ask.set("session", session);
        p.Call("session.join", ask, &code);
        if (code != "ok") return 0;
        net::Json bytes_hex = p.Call("session.data", ask, &code);
        std::vector<uint8_t> bytes;
        net::hex_decode(bytes_hex["data"].as_string(), bytes);
        uint64_t named = 0;
        if (bytes.size() == 16) std::memcpy(&named, bytes.data(), 8);
        if (named != room) return 0;
        net::Json body = net::Json::Object();
        body.set("room", net::u64_text(room));
        const net::Json joined = p.Call("room.join", body, &code);
        return code == "ok" ? int(joined["me"].as_int()) : 0;
    };

    CHECK(invite({&bravo, &charlie, &delta}));
    const std::string first[3] = {received(bravo), received(charlie), received(delta)};
    CHECK(!first[0].empty() && !first[1].empty() && !first[2].empty());
    CHECK(first[0] != first[1] && first[1] != first[2] && first[0] != first[2]);
    const size_t before = g_seen.rooms.size();
    CHECK(answer(charlie, first[1], false) == "ok");
    CHECK(answer(delta, first[2], true) == "ok" && join(delta) == 0x20);
    CHECK(answer(bravo, first[0], true) == "ok" && join(bravo) == 0x30);
    CHECK(PumpUntil([&] { return SawRoomEvent(before, 0x1101, 0x20, "Delta") && SawRoomEvent(before, 0x1101, 0x30, "Bravo"); }));

    CHECK(invite({&charlie}));
    const std::string again = received(charlie);
    CHECK(!again.empty() && again != first[1]);
    CHECK(!bravo.Next([](const net::LobbyMessage& m) { return m.event == "invite.received"; }, 1));
    CHECK(answer(charlie, again, true) == "ok" && join(charlie) == 0x40);
    CHECK(PumpUntil([&] { return SawRoomEvent(before, 0x1101, 0x40, "Charlie"); }));

    net::Json full = net::Json::Object();
    full.set("room", net::u64_text(room));
    echo.Call("room.join", full, &code);
    CHECK(code == "full");
    const size_t left = g_seen.rooms.size();
    bravo.Call("room.leave", full, &code);
    CHECK(PumpUntil([&] { return SawRoomEvent(left, 0x1102, 0x30); }));
    const net::Json taken = echo.Call("room.join", full, &code);
    CHECK(code == "ok" && taken["me"].as_int() == 0x30);
    CHECK(PumpUntil([&] { return SawRoomEvent(left, 0x1101, 0x30, "Echo"); }));
    CHECK(join(bravo) == 0);
    echo.Call("room.leave", full, &code);
    CHECK(PumpUntil([&] { return SawRoomEvent(left, 0x1102, 0x30, "Echo"); }));
    const size_t rejoined = g_seen.rooms.size();
    CHECK(join(bravo) == 0x30);
    CHECK(PumpUntil([&] { return SawRoomEvent(rejoined, 0x1101, 0x30, "Bravo"); }));
    echo.Call("room.join", full, &code);
    CHECK(code == "full");
    for (Player* guest : guests) guest->Call("room.leave", full, &code);
    std::printf("online-hle: four-player invitations: three pending, decline and re-invite, full room, seat reuse\n");
    LeaveAsGame(ctx, opt, room);
}

// A player of another PC with its own peer socket: it dials every member the lobby names, as the game does, so the
// players of a room end up linked pairwise.
struct MeshNode {
    Player player;
    std::shared_ptr<net::PeerSocket> socket;
    int seat = 0;
    std::map<int, int> links;
    std::map<int, uint64_t> accounts;

    MeshNode(const std::string& url, const fs::path& dir, const char* name) : player(url, dir, name) {
        net::PeerSocket::Options options;
        options.bind_ip = kLoopback;
        std::string error;
        socket = net::PeerSocket::open(options, &error);
        if (!socket) return;
        net::Json udp = net::Json::Object();
        udp.set("port", int64_t(socket->port()));
        player.Call("udp", udp);
    }
    bool ok() const { return socket && !player.code.empty(); }

    std::string Join(uint64_t room) {
        net::Json body = net::Json::Object();
        body.set("room", net::u64_text(room));
        std::string code;
        const net::Json joined = player.Call("room.join", body, &code);
        if (code != "ok") return code;
        seat = int(joined["me"].as_int());
        net::RoomKey key{};
        std::vector<uint8_t> bytes;
        net::hex_decode(joined["room"]["key"].as_string(), bytes);
        if (bytes.size() == key.size()) std::memcpy(key.data(), bytes.data(), key.size());
        socket->set_room(key, player.id.account);
        return code;
    }

    void Leave(uint64_t room) {
        net::Json body = net::Json::Object();
        body.set("room", net::u64_text(room));
        player.Call("room.leave", body);
        for (const auto& [member, link] : links) {
            socket->close(link);
            socket->forget_peer(accounts[member]);
        }
        links.clear();
        accounts.clear();
        seat = 0;
    }

    void Serve() {
        net::LobbyMessage m;
        while (player.link.poll(m)) player.inbox.push_back(std::move(m));
        for (size_t i = 0; i < player.inbox.size();) {
            const net::LobbyMessage& e = player.inbox[i];
            if (e.kind != net::LobbyMessage::Kind::kEvent || (e.event != "peer" && e.event != "peer.gone")) {
                ++i;
                continue;
            }
            const int member = int(e.body["member"].as_int());
            if (e.event == "peer") {
                std::vector<net::Endpoint> candidates = {{net::parse_ip(e.body["ip"].as_string()), uint16_t(e.body["port"].as_int())}};
                for (const net::Json& c : e.body["lan"].items())
                    candidates.push_back({net::parse_ip(c["ip"].as_string()), uint16_t(c["port"].as_int())});
                const uint64_t account = e.body["account"].as_u64();
                socket->set_peer(account, candidates, uint16_t(member));
                accounts[member] = account;
                links[member] = socket->connect(account);
            } else if (links.count(member)) {
                socket->close(links[member]);
                socket->forget_peer(accounts[member]);
                links.erase(member);
                accounts.erase(member);
            }
            player.inbox.erase(player.inbox.begin() + ptrdiff_t(i));
        }
    }

    bool Connected(int peer_seat) {
        const auto it = links.find(peer_seat);
        return it != links.end() && socket->state(it->second) == net::LinkState::kConnected;
    }
};

using Mesh = std::vector<std::unique_ptr<MeshNode>>;

bool Pump(Mesh& nodes, const std::function<bool()>& done, int timeout_ms = 15000) {
    return PumpUntil([&] {
        for (auto& node : nodes) node->Serve();
        return done();
    }, timeout_ms);
}

// Every pair of the room's players has a working link: the game with each guest through its DGRAM_P2P socket (signaling
// established, a datagram each way attributed to the right member) and each two guests directly. Returns the pairs checked.
size_t VerifyMesh(Mesh& nodes, uint16_t ctx, uint64_t room, int32_t sock) {
    size_t pairs = 0;
    for (auto& node : nodes) {
        int32_t status = 0;
        uint32_t ip = 0;
        uint16_t port = 0;
        const auto linked = [&] {
            return Fn<Status>("sceNpMatching2SignalingGetConnectionStatus")(ctx, room, uint16_t(node->seat), &status, &ip, &port) == 0 &&
                   status == 2 && node->Connected(0x10);
        };
        CHECK(Pump(nodes, linked));
        uint8_t to[16];
        Address(to, ip, port, 1872);
        const std::string down = "game to seat " + std::to_string(node->seat);
        CHECK(Fn<SendTo>("sceNetSendto")(sock, down.data(), down.size(), 0, to, 16) == int64_t(down.size()));
        std::vector<uint8_t> got;
        CHECK(Pump(nodes, [&] { return node->socket->receive(node->links[0x10], got); }));
        CHECK(std::string(got.begin(), got.end()) == down);
        const std::string up = "seat " + std::to_string(node->seat) + " to game";
        CHECK(node->socket->send(node->links[0x10], up.data(), up.size()));
        uint8_t buffer[0x1800], from[16];
        uint32_t from_size = 16;
        int64_t read = -1;
        CHECK(Pump(nodes, [&] {
            from_size = 16;
            read = Fn<RecvFrom>("sceNetRecvfrom")(sock, buffer, sizeof(buffer), 0, from, &from_size);
            return read > 0;
        }));
        CHECK(read == int64_t(up.size()) && std::memcmp(buffer, up.data(), up.size()) == 0);
        CHECK(Rd<uint32_t>(from, 4) == ip && Rd<uint16_t>(from, 2) == port);
        ++pairs;
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        for (size_t j = i + 1; j < nodes.size(); ++j) {
            MeshNode& a = *nodes[i];
            MeshNode& b = *nodes[j];
            CHECK(Pump(nodes, [&] { return a.Connected(b.seat) && b.Connected(a.seat); }));
            const std::string text = "seat " + std::to_string(a.seat) + " to seat " + std::to_string(b.seat);
            std::vector<uint8_t> got;
            CHECK(a.socket->send(a.links[b.seat], text.data(), text.size()));
            CHECK(Pump(nodes, [&] { return b.socket->receive(b.links[a.seat], got); }));
            CHECK(std::string(got.begin(), got.end()) == text);
            const std::string back = "seat " + std::to_string(b.seat) + " to seat " + std::to_string(a.seat);
            CHECK(b.socket->send(b.links[a.seat], back.data(), back.size()));
            got.clear();
            CHECK(Pump(nodes, [&] { return a.socket->receive(a.links[b.seat], got); }));
            CHECK(std::string(got.begin(), got.end()) == back);
            ++pairs;
        }
    }
    return pairs;
}

// A room of `players` players hosted by the game, from creation to the last leave: its session, a search that finds it,
// the seats the guests take, the member events the game hears, the full mesh of links between the players, a full room
// that refuses the next player, and a guest that leaves and comes back to the mesh.
void RoomOfSize(int players, const char* url, const fs::path& base, uint16_t ctx, void* const* opt, int32_t sock, int32_t web) {
    const std::string tag = "mesh" + std::to_string(players);
    const size_t guests = size_t(players - 1);
    const Seen::Request* hosted = CreateAsGame(ctx, opt, uint16_t(players));
    CHECK(hosted && hosted->error == 0 && hosted->slots == players && hosted->me == 0x10 && hosted->me_name == g_settings.online_id);
    CHECK(hosted && hosted->names == std::vector<std::string>({g_settings.online_id}));
    if (!hosted || hosted->error) return;
    const uint64_t room = hosted->room;
    const WebReply made = Web(web, "sessionInvitation", "/v1/sessions", 1,
                              SessionPost("public", uint8_t(players), SessionDataBytes(room, uint8_t(players))),
                              "multipart/mixed; boundary=xyz123xyz123xyz123");
    CHECK(made.status == 200 && Field(made.body, "sessionId").size() == 36);
    uint8_t make_public[0x20] = {};
    Wr<uint64_t>(make_public, 0, room);
    Wr<uint32_t>(make_public, 8, kHidden);
    Wr<uint32_t>(make_public, 0xc, kPublic);
    uint32_t published = 0;
    CHECK(Fn<Request>("sceNpMatching2SetRoomDataInternal")(ctx, make_public, opt, &published) == 0);
    CHECK(PumpUntil([&] { return RequestById(published); }));

    Mesh nodes;
    std::vector<std::string> names = {g_settings.online_id + "*"};
    for (size_t i = 0; i < guests; ++i) {
        const std::string name = "Mesh" + std::to_string(players) + char('B' + i);
        nodes.push_back(std::make_unique<MeshNode>(url, base / (tag + "-" + name), name.c_str()));
        CHECK(nodes.back()->ok());
        names.push_back(name);
    }
    MeshNode late(url, base / (tag + "-late"), "MeshLate");
    CHECK(late.ok());
    if (!late.ok()) return;

    net::Json search = net::Json::Object();
    search.set("world", 1);
    search.set("flag_mask", int64_t(0x60000000));
    std::string code;
    const net::Json found = nodes[0]->player.Call("room.search", search, &code);
    size_t matches = 0;
    for (const net::Json& r : found["rooms"].items()) matches += r["id"].as_u64() == room && r["slots"].as_int() == players;
    CHECK(code == "ok" && matches == 1);

    const size_t before = g_seen.rooms.size();
    for (size_t i = 0; i < guests; ++i) {
        CHECK(nodes[i]->Join(room) == "ok" && nodes[i]->seat == int(0x20 + 0x10 * i));
        CHECK(PumpUntil([&] { return SawRoomEvent(before, 0x1101, uint16_t(nodes[i]->seat), names[i + 1].c_str()); }));
    }
    CHECK(late.Join(room) == "full");
    CHECK(nodes[0]->player.Call("room.search", search, &code)["rooms"].size() + 1 == found["rooms"].size());

    uint8_t members_request[8] = {};
    Wr<uint64_t>(members_request, 0, room);
    uint32_t members = 0;
    CHECK(Fn<Request>("sceNpMatching2GetRoomMemberDataExternalList")(ctx, members_request, opt, &members) == 0);
    CHECK(PumpUntil([&] { return RequestById(members); }));
    CHECK(RequestById(members) && RequestById(members)->names == names);

    const size_t expected = size_t(players) * size_t(players - 1) / 2;
    CHECK(VerifyMesh(nodes, ctx, room, sock) == expected);

    MeshNode& leaver = *nodes[0];
    const int seat = leaver.seat;
    const size_t signals = g_seen.signals.size();
    const size_t left = g_seen.rooms.size();
    leaver.Leave(room);
    CHECK(PumpUntil([&] { return SawRoomEvent(left, 0x1102, uint16_t(seat), names[1].c_str()); }));
    CHECK(Pump(nodes, [&] {
        for (size_t i = signals; i < g_seen.signals.size(); ++i)
            if (g_seen.signals[i] == std::make_pair(uint16_t(seat), uint16_t(0x5101))) return true;
        return false;
    }));
    int32_t status = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    CHECK(Fn<Status>("sceNpMatching2SignalingGetConnectionStatus")(ctx, room, uint16_t(seat), &status, &ip, &port) != 0);
    CHECK(Pump(nodes, [&] {
        for (size_t i = 1; i < nodes.size(); ++i)
            if (nodes[i]->links.count(seat)) return false;
        return true;
    }));
    CHECK(late.Join(room) == "ok" && late.seat == seat);
    late.Leave(room);
    const size_t again = g_seen.rooms.size();
    CHECK(leaver.Join(room) == "ok" && leaver.seat == seat);
    CHECK(PumpUntil([&] { return SawRoomEvent(again, 0x1101, uint16_t(seat), names[1].c_str()); }));
    const size_t linked = VerifyMesh(nodes, ctx, room, sock);
    CHECK(linked == expected);
    std::printf("online-hle: %d-player room: session, search, %zu seats, %zu of %zu peer pairs linked, leave and rejoin\n", players,
                guests + 1, linked, expected);

    for (auto& node : nodes) node->Leave(room);
    LeaveAsGame(ctx, opt, room);
}

// The launcher while no game runs: the squad invitation arrives at the launcher's lobby link, Accept and play leaves the
// pending join the game reads at start, and the game enters the inviter's room through the session.
void LauncherAcceptAndPlay(const char* url, const fs::path& base, uint16_t ctx, void* const* opt) {
    const int32_t lib = Fn<WebInit>("sceNpWebApiInitialize")(1, 0x20000);
    const int32_t user = Fn<WebContext>("sceNpWebApiCreateContext")(lib, g_settings.online_id.c_str());
    Player inviter(url, base / "launcher-inviter", "Bravo");
    std::string code;
    net::Json add = net::Json::Object();
    add.set("code", inviter.code);
    CHECK(online::lobby_call("friends.add", add, nullptr, &code) && code == "ok");
    net::Json accept = net::Json::Object();
    accept.set("account", net::u64_text(g_settings.identity.account));
    inviter.Call("friends.accept", accept, &code);
    CHECK(code == "ok");

    net::Json squad = net::Json::Object();
    squad.set("world", 1);
    squad.set("slots", 4);
    squad.set("flags", int64_t(kHidden));
    const uint64_t room = inviter.Call("room.create", squad, &code)["room"]["id"].as_u64();
    CHECK(code == "ok" && room != 0);
    const std::string data = SessionDataBytes(room, 4);
    net::Json made = net::Json::Object();
    made.set("private", true);
    made.set("max", 4);
    made.set("data", net::hex_encode(data.data(), data.size()));
    const std::string session = inviter.Call("session.create", made, &code)["session"].as_string();
    CHECK(code == "ok" && !session.empty());

    OnlineClient launcher(base / "host");
    net::OnlineConfig config;
    config.enabled = true;
    config.lobby = url;
    config.name = g_settings.online_id;
    config.upnp = false;
    config.stun = "off";
    double clock = 0;
    const auto tick = [&] {
        clock += 0.05;
        launcher.Update(true, config, net::kOnlineGame, kEu, clock);
        online::pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    };
    CHECK(PumpUntil([&] { tick(); return launcher.state().link == OnlineClient::Link::kUp && launcher.state().fetched; }));
    CHECK(launcher.state().invitations.empty());

    net::Json invite = net::Json::Object();
    invite.set("session", session);
    net::Json to = net::Json::Array();
    to.push(net::u64_text(g_settings.identity.account));
    invite.set("to", to);
    inviter.Call("invite.send", invite, &code);
    CHECK(code == "ok");
    CHECK(PumpUntil([&] { tick(); return !launcher.state().invitations.empty(); }));
    if (launcher.state().invitations.empty()) return;
    const SquadInvitation shown = launcher.state().invitations[0];
    CHECK(shown.name == "Bravo" && shown.session == session && CanAcceptInvitation(shown, kEu));
    CHECK(!CanAcceptInvitation(shown, kKr));
    launcher.AcceptInvitation(shown.invitation);
    CHECK(launcher.state().invitations.empty());
    std::optional<OnlineClient::PendingJoin> join;
    CHECK(PumpUntil([&] { tick(); join = launcher.TakeJoin(); return join.has_value(); }));
    CHECK(join && join->session == session && join->invitation == shown.invitation && join->who == "Bravo");
    CHECK(inviter.Event("invite.answered") != std::nullopt);

    const fs::path profile = base / "launcher-profile";
    fs::create_directories(profile);
    if (!join) return;
    {
        std::ofstream file(profile / net::kPendingJoinFile, std::ios::binary | std::ios::trunc);
        file << PendingJoinText(join->session, join->invitation, int64_t(std::time(nullptr)));
    }
    CHECK(fs::exists(profile / net::kPendingJoinFile));
    g_system_events.clear();
    online::load_pending_join(profile.string());
    CHECK(!fs::exists(profile / net::kPendingJoinFile));
    CHECK(PumpUntil([] { return !g_system_events.empty(); }));
    if (g_system_events.empty()) return;
    const std::vector<uint8_t>& event = g_system_events[0];
    CHECK(Rd<uint32_t>(event.data(), 0) == 0x10000002 && Rd<uint32_t>(event.data(), 0x74) == 1);
    CHECK(std::string(reinterpret_cast<const char*>(event.data() + 4)) == session);
    CHECK(std::string(reinterpret_cast<const char*>(event.data() + 0x34)) == shown.invitation);
    const std::string data_path = "/v1/sessions/" + session + "/sessionData";
    const WebReply reply = Web(user, "sessionInvitation", data_path.c_str(), 0);
    uint64_t target = 0;
    if (reply.body.size() >= 8) std::memcpy(&target, reply.body.data(), 8);
    CHECK(reply.status == 200 && target == room);
    const Seen::Request* joined = JoinAsGame(ctx, opt, target);
    CHECK(joined && joined->error == 0 && joined->me == 0x20 && joined->names.size() == 2);
    std::printf("online-hle: launcher Accept and play: invitation, pending join file, game join\n");
    LeaveAsGame(ctx, opt, room);
    inviter.Call("room.leave", net::Json::Object(), &code);
    g_choice_answer = 1;
    for (int i = 0; i < 20; ++i) online::pump();
    g_choice_answer = -1;
}

// The game ends (Exit Game, window closed, guest exit): the squad and the friends see the player go at once, not after the
// lobby's 30 s grace for a dropped socket. Last in the run: the lobby link is closed.
void CleanExit(const char* url, const fs::path& base, uint16_t ctx, void* const* opt) {
    Player watcher(url, base / "exit-watcher", "Watcher");
    std::string code;
    net::Json add = net::Json::Object();
    add.set("code", watcher.code);
    CHECK(online::lobby_call("friends.add", add, nullptr, &code) && code == "ok");
    net::Json accept = net::Json::Object();
    accept.set("account", net::u64_text(g_settings.identity.account));
    watcher.Call("friends.accept", accept, &code);
    CHECK(code == "ok");
    net::Json squad = net::Json::Object();
    squad.set("world", 1);
    squad.set("slots", 4);
    squad.set("flags", int64_t(kPublic));
    const uint64_t room = watcher.Call("room.create", squad, &code)["room"]["id"].as_u64();
    CHECK(code == "ok" && room != 0);
    const Seen::Request* joined = JoinAsGame(ctx, opt, room);
    CHECK(joined && joined->error == 0 && joined->me == 0x20);
    CHECK(watcher.Event("room.member_joined") != std::nullopt);

    const auto start = std::chrono::steady_clock::now();
    online::leave_for_exit();
    const auto left = watcher.Next([](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kEvent && m.event == "room.member_left"; }, 5);
    CHECK(left && left->body["member"].as_int() == 0x20 && left->body["why"].as_string() == "leave");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK(watcher.Event("friend.offline") != std::nullopt);
    std::printf("online-hle: clean exit: member left with reason leave and friend offline without the grace period\n");
}

void Identities() {
    CHECK(net::online_id("Player", 0x123456789ull) == "Player-456789");
    CHECK(net::online_id("", 0xabcdefull) == "Player-abcdef");
    CHECK(net::online_id("Saulo", 7) == "Saulo");
    CHECK(net::online_id("bad name!", 7) == "bad_name");
    CHECK(online::peer_member(online::peer_endpoint(0x20)) == 0x20 && online::peer_member({kLoopback, 0x20}) == 0);
}

} // namespace

int main() {
    Identities();
    const char* url = std::getenv("ALN_TEST_LOBBY");
    if (!url || !*url) {
        std::printf("online-hle: identity checks only; set ALN_TEST_LOBBY to a running development lobby\n");
        std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
        return g_failures ? 1 : 0;
    }
    const fs::path base = fs::temp_directory_path() / "aln-online-hle-tests";
    fs::remove_all(base);
    g_settings.enabled = true;
    g_settings.config.lobby = "ws://127.0.0.1:1/";
    g_settings.config.upnp = false;
    g_settings.config.stun = "off";
    g_settings.config.bind_ip = kLoopback;
    g_settings.identity = net::load_identity(base / "host");
    g_settings.online_id = "Alpha";
    g_settings.game = net::kOnlineGame;
    g_settings.build = kEu;

    // The network status box: Online Play off, then on and connecting before anything started.
    g_settings.enabled = false;
    CHECK(runtime_online_status() == ONLINE_STATUS_OFF);
    g_settings.enabled = true;
    CHECK(runtime_online_status() == ONLINE_STATUS_CONNECTING);

    // An unreachable service: the context starts over with "service unavailable", which the game retries. Nothing
    // pretends to be online.
    CHECK(Fn<Init>("sceNpMatching2Initialize")(nullptr) == 0);
    uint16_t ctx = 0;
    CHECK(Fn<CreateContext>("sceNpMatching2CreateContext")(nullptr, &ctx) == 0 && ctx == 1);
    Fn<RegisterCtx>("sceNpMatching2RegisterContextCallback")(reinterpret_cast<void*>(&OnContext), nullptr);
    CHECK(Fn<Start>("sceNpMatching2ContextStart")(ctx, 10000000) == 0);
    CHECK(PumpUntil([] { return !g_seen.context.empty(); }));
    CHECK(!g_seen.context.empty() && g_seen.context[0] == std::make_pair(uint16_t(0x6f01), int32_t(0x80550d02)));
    void* opt[4] = {reinterpret_cast<void*>(&OnRequest), nullptr, nullptr, nullptr};
    uint32_t worlds = 0;
    uint8_t world_request[16] = {};
    CHECK(Fn<Request>("sceNpMatching2GetWorldInfoList")(ctx, world_request, opt, &worlds) == 0);
    CHECK(PumpUntil([&] { return RequestById(worlds); }));
    CHECK(RequestById(worlds) && RequestById(worlds)->error == int32_t(0x80550d02));
    CHECK(runtime_online_status() == ONLINE_STATUS_UNAVAILABLE);

    // The real service: started, a server id, one world.
    g_seen.context.clear();
    g_settings.config.lobby = url;
    CHECK(Fn<Start>("sceNpMatching2ContextStart")(ctx, 10000000) == 0);
    CHECK(PumpUntil([] { return !g_seen.context.empty(); }));
    CHECK(!g_seen.context.empty() && g_seen.context[0] == std::make_pair(uint16_t(0x6f02), int32_t(0)));
    // Back after the lobby was unavailable: connected once NP has signed the user in too.
    CHECK(runtime_online_status() == ONLINE_STATUS_CONNECTING);
    g_np_signed_in = 1;
    CHECK(runtime_online_status() == ONLINE_STATUS_CONNECTED);
    uint16_t server = 0;
    CHECK(Fn<ServerId>("sceNpMatching2GetServerId")(ctx, &server) == 0 && server != 0);
    CHECK(Fn<Request>("sceNpMatching2GetWorldInfoList")(ctx, world_request, opt, &worlds) == 0);
    CHECK(PumpUntil([&] { return RequestById(worlds); }));
    CHECK(RequestById(worlds) && RequestById(worlds)->error == 0 && RequestById(worlds)->worlds == 1 &&
          RequestById(worlds)->world == 1);
    Fn<Register>("sceNpMatching2RegisterRoomEventCallback")(ctx, reinterpret_cast<void*>(&OnRoom), nullptr);
    Fn<Register>("sceNpMatching2RegisterSignalingCallback")(ctx, reinterpret_cast<void*>(&OnSignal), nullptr);

    // CreateJoinRoom as Online_Matching_NP builds it: hidden at first, compatibility byte 0x57, 0x70-byte room info,
    // searchable 0x4d..0x53, member attribute 0x59.
    std::vector<uint8_t> info(0x70);
    for (size_t i = 0; i < info.size(); ++i) info[i] = uint8_t(i);
    const uint8_t compat = 1;
    const uint32_t member_attr = 0x01020304;
    uint8_t bin_internal[0x18] = {}, bin_external[0x18] = {}, bin_member[0x18] = {}, ints[7 * 8] = {};
    Wr<uint16_t>(bin_internal, 0, 0x57);
    Wr<const uint8_t*>(bin_internal, 8, &compat);
    Wr<uint64_t>(bin_internal, 0x10, 1);
    Wr<uint16_t>(bin_external, 0, 0x55);
    Wr<const uint8_t*>(bin_external, 8, info.data());
    Wr<uint64_t>(bin_external, 0x10, info.size());
    Wr<uint16_t>(bin_member, 0, 0x59);
    Wr<const uint32_t*>(bin_member, 8, &member_attr);
    Wr<uint64_t>(bin_member, 0x10, 4);
    for (int i = 0; i < 7; ++i) Wr<uint16_t>(ints, size_t(i * 8), uint16_t(0x4d + i)), Wr<uint32_t>(ints, size_t(i * 8 + 4), uint32_t(10 + i));
    uint8_t create[0xd0] = {};
    Wr<uint16_t>(create, 0, 4);
    Wr<uint32_t>(create, 8, kHidden);
    Wr<uint32_t>(create, 0xc, 1);
    Wr<uint8_t*>(create, 0x60, bin_internal);
    Wr<uint64_t>(create, 0x68, 1);
    Wr<uint8_t*>(create, 0x70, ints);
    Wr<uint64_t>(create, 0x78, 7);
    Wr<uint8_t*>(create, 0x90, bin_external);
    Wr<uint64_t>(create, 0x98, 1);
    Wr<uint8_t*>(create, 0xa0, bin_member);
    Wr<uint64_t>(create, 0xa8, 1);
    uint32_t created = 0;
    CHECK(Fn<Request>("sceNpMatching2CreateJoinRoom")(ctx, create, opt, &created) == 0);
    CHECK(PumpUntil([&] { return RequestById(created); }));
    const Seen::Request* room = RequestById(created);
    CHECK(room && room->error == 0 && room->room >= 4096 && room->slots == 4 && room->server == server && room->world == 1);
    CHECK(room && room->me == 0x10 && room->me_name == "Alpha" && room->copy == std::vector<uint8_t>{compat});
    if (!room || room->error) {
        std::printf("FAILED (%d failures)\n", g_failures);
        return 1;
    }
    const uint64_t room_id = room->room;
    uint8_t make_public[0x20] = {};
    Wr<uint64_t>(make_public, 0, room_id);
    Wr<uint32_t>(make_public, 8, kHidden);
    Wr<uint32_t>(make_public, 0xc, kPublic);
    uint32_t published = 0;
    CHECK(Fn<Request>("sceNpMatching2SetRoomDataInternal")(ctx, make_public, opt, &published) == 0);
    CHECK(PumpUntil([&] { return RequestById(published); }));
    CHECK(RequestById(published) && RequestById(published)->error == 0 && RequestById(published)->event == 0x0109);

    // The P2P socket: "udpp2p_socket", AF_INET, DGRAM_P2P, non-blocking, bound with the game's vport.
    const int32_t sock = Fn<Socket>("sceNetSocket")("udpp2p_socket", 2, 6, 0);
    CHECK(sock > 0);
    const int32_t one = 1;
    CHECK(Fn<SetOpt>("sceNetSetsockopt")(sock, 0xffff, 0x20000, &one, 4) == 0);
    CHECK(Fn<SetOpt>("sceNetSetsockopt")(sock, 0xffff, 0x1200, &one, 4) == 0);
    uint8_t local[16];
    Address(local, 0, 0, 1872);
    CHECK(Fn<Bind>("sceNetBind")(sock, local, 16) == 0);
    CHECK(Fn<Socket>("sceNetSocket")("debug", 2, 1, 0) < 0);
    CHECK(Select(sock) == 2);
    uint8_t buffer[0x1800];
    uint8_t from[16];
    uint32_t from_size = 16;
    CHECK(Fn<RecvFrom>("sceNetRecvfrom")(sock, buffer, sizeof(buffer), 0, from, &from_size) == kEAgain);

    // The second player, on the same build: the lobby protocol and its own peer socket.
    net::PeerSocket::Options options;
    options.bind_ip = kLoopback;
    std::string error;
    auto guest_socket = net::PeerSocket::open(options, &error);
    CHECK(guest_socket != nullptr);
    if (!guest_socket) return 1;
    const net::Identity guest_id = net::load_identity(base / "guest");
    net::LobbyLink::Options link_options;
    link_options.url = url;
    link_options.hello = [&] { return net::hello_body(guest_id, "GuestB", net::kOnlineGame, true, true, false, kEu); };
    net::LobbyLink guest(link_options);
    guest.start();
    std::vector<net::LobbyMessage> guest_inbox;
    auto guest_next = [&](const std::function<bool(const net::LobbyMessage&)>& match) -> std::optional<net::LobbyMessage> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            online::pump();
            net::LobbyMessage m;
            while (guest.poll(m)) guest_inbox.push_back(std::move(m));
            for (size_t i = 0; i < guest_inbox.size(); ++i) {
                if (match(guest_inbox[i])) {
                    net::LobbyMessage found = std::move(guest_inbox[i]);
                    guest_inbox.erase(guest_inbox.begin() + ptrdiff_t(i));
                    return found;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::nullopt;
    };
    CHECK(guest_next([](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kUp; }));
    net::Json udp = net::Json::Object();
    udp.set("port", int64_t(guest_socket->port()));
    guest.request("udp", udp);
    net::Json search = net::Json::Object();
    search.set("world", 1);
    search.set("flag_mask", int64_t(0x60000000));
    const uint32_t search_seq = guest.request("room.search", search);
    auto found = guest_next([&](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kResponse && m.seq == search_seq; });
    CHECK(found && found->body["total"].as_int() == 1 && found->body["rooms"].items()[0]["id"].as_u64() == room_id);
    const uint32_t member_bin = 0x0a0b0c0d;
    net::Json join = net::Json::Object();
    join.set("room", net::u64_text(room_id));
    net::Json member_attrs = net::Json::Array();
    net::Json attr = net::Json::Array();
    attr.push(int64_t(0x59));
    attr.push(net::hex_encode(reinterpret_cast<const uint8_t*>(&member_bin), 4));
    member_attrs.push(attr);
    join.set("member_bin", member_attrs);
    const uint32_t join_seq = guest.request("room.join", join);
    auto joined = guest_next([&](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kResponse && m.seq == join_seq; });
    CHECK(joined && joined->code == "ok" && joined->body["me"].as_int() == 0x20);
    if (!joined) return 1;

    // The host's game hears the member join with its SceNpId and attribute 0x59.
    CHECK(PumpUntil([] { return !g_seen.rooms.empty(); }));
    CHECK(!g_seen.rooms.empty() && g_seen.rooms[0].event == 0x1101 && g_seen.rooms[0].member == 0x20 &&
          g_seen.rooms[0].name == "GuestB" && g_seen.rooms[0].attr == member_bin);
    int32_t status = -1;
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    CHECK(Fn<Status>("sceNpMatching2SignalingGetConnectionStatus")(ctx, room_id, 0x20, &status, &peer_ip, &peer_port) == 0);
    CHECK(status == 1);
    CHECK(Fn<Status>("sceNpMatching2SignalingGetConnectionStatus")(ctx, room_id, 0x30, &status, &peer_ip, &peer_port) != 0);

    // Endpoints are exchanged; the guest dials the host and the link comes up; signaling is then established.
    auto peer = guest_next([](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kEvent && m.event == "peer"; });
    CHECK(peer && peer->body["member"].as_int() == 0x10 && peer->body["port"].as_int() > 0);
    if (!peer) return 1;
    net::RoomKey key{};
    std::vector<uint8_t> key_bytes;
    net::hex_decode(joined->body["room"]["key"].as_string(), key_bytes);
    CHECK(key_bytes.size() == key.size());
    std::memcpy(key.data(), key_bytes.data(), std::min(key.size(), key_bytes.size()));
    guest_socket->set_room(key, guest_id.account);
    std::vector<net::Endpoint> host_candidates = {{net::parse_ip(peer->body["ip"].as_string()), uint16_t(peer->body["port"].as_int())}};
    for (const net::Json& c : peer->body["lan"].items())
        host_candidates.push_back({net::parse_ip(c["ip"].as_string()), uint16_t(c["port"].as_int())});
    guest_socket->set_peer(g_settings.identity.account, host_candidates);
    const int guest_link = guest_socket->connect(g_settings.identity.account);
    CHECK(PumpUntil([] { return !g_seen.signals.empty(); }, 15000));
    CHECK(!g_seen.signals.empty() && g_seen.signals[0] == std::make_pair(uint16_t(0x20), uint16_t(0x5102)));
    CHECK(Fn<Status>("sceNpMatching2SignalingGetConnectionStatus")(ctx, room_id, 0x20, &status, &peer_ip, &peer_port) == 0);
    CHECK(status == 2 && __builtin_bswap32(peer_ip) == online::kPeerAddress && uint16_t(peer_port >> 8 | peer_port << 8) == 0x20);
    CHECK(PumpUntil([&] { return guest_socket->state(guest_link) == net::LinkState::kConnected; }));

    // Datagrams both ways, unchanged: sendto the signaled address with the sender's vport; recvfrom reports exactly that
    // address and port, which is how the game attributes a datagram to its member.
    uint8_t to[16];
    Address(to, peer_ip, peer_port, 1872);
    const std::string hello(173, 'h');
    CHECK(Fn<SendTo>("sceNetSendto")(sock, hello.data(), hello.size(), 0, to, 16) == int64_t(hello.size()));
    std::vector<uint8_t> got;
    CHECK(PumpUntil([&] { return guest_socket->receive(guest_link, got); }));
    CHECK(std::string(got.begin(), got.end()) == hello);
    std::vector<uint8_t> big(6000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 7);
    CHECK(guest_socket->send(guest_link, big.data(), big.size()));
    int64_t read = -1;
    CHECK(PumpUntil([&] {
        from_size = 16;
        read = Fn<RecvFrom>("sceNetRecvfrom")(sock, buffer, sizeof(buffer), 0, from, &from_size);
        return read > 0;
    }));
    CHECK(read == int64_t(big.size()) && std::memcmp(buffer, big.data(), big.size()) == 0);
    CHECK(from_size == 16 && Rd<uint32_t>(from, 4) == peer_ip && Rd<uint16_t>(from, 2) == peer_port);
    CHECK(Fn<RecvFrom>("sceNetRecvfrom")(sock, buffer, sizeof(buffer), 0, from, &from_size) == kEAgain);
    uint8_t nobody[16];
    Address(nobody, __builtin_bswap32(online::kPeerAddress), uint16_t(0x4000), 1872);
    CHECK(Fn<SendTo>("sceNetSendto")(sock, hello.data(), hello.size(), 0, nobody, 16) == int64_t(hello.size()));
    CHECK(Select(sock) == 2);

    // Members of a findable room, and its external data, as the game reads them.
    uint8_t members_request[8] = {};
    Wr<uint64_t>(members_request, 0, room_id);
    uint32_t members = 0;
    CHECK(Fn<Request>("sceNpMatching2GetRoomMemberDataExternalList")(ctx, members_request, opt, &members) == 0);
    CHECK(PumpUntil([&] { return RequestById(members); }));
    CHECK(RequestById(members) && RequestById(members)->names == std::vector<std::string>({"Alpha*", "GuestB"}));
    uint64_t ids[1] = {room_id};
    uint8_t external_request[0x20] = {};
    Wr<const uint64_t*>(external_request, 0, ids);
    Wr<uint64_t>(external_request, 8, 1);
    uint32_t external = 0;
    CHECK(Fn<Request>("sceNpMatching2GetRoomDataExternalList")(ctx, external_request, opt, &external) == 0);
    CHECK(PumpUntil([&] { return RequestById(external); }));
    CHECK(RequestById(external) && RequestById(external)->error == 0 && RequestById(external)->count == 1);

    // A room of the KR build is never found or joined from the EU build.
    net::LobbySession korea;
    const net::Identity korea_id = net::load_identity(base / "korea");
    CHECK(korea.open(url, net::hello_body(korea_id, "KoreaC", net::kOnlineGame, true, false, false, kKr), nullptr, &error));
    std::string code;
    net::Json korea_room = net::Json::Object();
    korea_room.set("world", 1);
    korea_room.set("slots", 4);
    korea_room.set("flags", int64_t(kPublic));
    auto made = korea.call("room.create", korea_room, &code);
    CHECK(made && code == "ok");
    const uint64_t korea_room_id = made ? (*made)["room"]["id"].as_u64() : 0;

    // The guest leaves: the host's game hears it (member left, signaling dead) and the member's address goes quiet.
    net::Json leave = net::Json::Object();
    leave.set("room", net::u64_text(room_id));
    guest.request("room.leave", leave);
    CHECK(PumpUntil([] {
        for (const auto& r : g_seen.rooms)
            if (r.event == 0x1102 && r.member == 0x20 && r.name == "GuestB") return true;
        return false;
    }));
    CHECK(std::find(g_seen.signals.begin(), g_seen.signals.end(), std::make_pair(uint16_t(0x20), uint16_t(0x5101))) != g_seen.signals.end());
    CHECK(Fn<Status>("sceNpMatching2SignalingGetConnectionStatus")(ctx, room_id, 0x20, &status, &peer_ip, &peer_port) != 0);

    uint8_t leave_request[16] = {};
    Wr<uint64_t>(leave_request, 0, room_id);
    uint32_t left = 0;
    CHECK(Fn<Request>("sceNpMatching2LeaveRoom")(ctx, leave_request, opt, &left) == 0);
    CHECK(PumpUntil([&] { return RequestById(left); }));
    CHECK(RequestById(left) && RequestById(left)->error == 0 && RequestById(left)->room == room_id);
    uint8_t search_request[0x50] = {};
    search_request[0] = 0x08;
    Wr<uint32_t>(search_request, 4, 1);
    Wr<uint32_t>(search_request, 0x10, 1);
    Wr<uint32_t>(search_request, 0x14, 20);
    Wr<uint32_t>(search_request, 0x18, 0x60000000);
    uint32_t searched = 0;
    CHECK(Fn<Request>("sceNpMatching2SearchRoom")(ctx, search_request, opt, &searched) == 0);
    CHECK(PumpUntil([&] { return RequestById(searched); }));
    CHECK(RequestById(searched) && RequestById(searched)->error == 0 && RequestById(searched)->count == 0);
    uint8_t join_request[0x28] = {};
    Wr<uint64_t>(join_request, 0, korea_room_id);
    uint32_t refused = 0;
    CHECK(Fn<Request>("sceNpMatching2JoinRoom")(ctx, join_request, opt, &refused) == 0);
    CHECK(PumpUntil([&] { return RequestById(refused); }));
    CHECK(RequestById(refused) && RequestById(refused)->error == int32_t(0x80550d13));
    korea.close();

    Squads(url, base, ctx, opt);
    RelayFallback(url, base, ctx, opt, sock);
    PublicSquad(url, base, ctx, opt);
    Sessions(url, base, ctx, opt);
    LauncherAcceptAndPlay(url, base, ctx, opt);
    Invites(url, base, ctx, opt);
    const int32_t web = Fn<WebContext>("sceNpWebApiCreateContext")(Fn<WebInit>("sceNpWebApiInitialize")(1, 0x20000), g_settings.online_id.c_str());
    for (int players = 2; players <= 4; ++players) RoomOfSize(players, url, base, ctx, opt, sock, web);

    // Shutdown in the game's order: abort, shutdown, close; then the context stops.
    CHECK(Fn<Abort>("sceNetSocketAbort")(sock, 0) == 0);
    CHECK(Select(sock) & 0x10);
    CHECK(Fn<Abort>("sceNetShutdown")(sock, 2) == 0);
    CHECK(Fn<Close>("sceNetSocketClose")(sock) == 0);
    CHECK(online::peer_socket() == nullptr);
    CHECK(Fn<Stop>("sceNpMatching2ContextStop")(ctx) == 0);
    CHECK(PumpUntil([] { return g_seen.context.size() >= 2; }));
    CHECK(g_seen.context.size() >= 2 && g_seen.context.back().first == 0x6f03);
    // Without a Matching2 context the stack is not ready, whatever the lobby link does.
    CHECK(runtime_online_status() == ONLINE_STATUS_CONNECTING);
    CleanExit(url, base, ctx, opt);
    guest.stop();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
