// The game's side of online play without the game: this test calls the classic Matching2 and libSceNet entry points the
// way Online_Matching_NP, Network_Room_Orbis and Network_Connection_Manager do (request structs, callbacks pumped like
// sceNpCheckCallback, the DGRAM_P2P socket polled with a fresh epoll), while a second player joins through the lobby
// protocol and connects over UDP. Needs ALN_TEST_LOBBY=ws://127.0.0.1:<port>/ pointing at a running development lobby
// (server/lobby/local/server.js); every socket is bound to loopback.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/identity.h"
#include "net/lobby_link.h"
#include "net/online_version.h"
#include "net/peer_socket.h"
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

} // namespace

namespace aln::online {
const Settings& settings() { return g_settings; }
} // namespace aln::online

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
    g_settings.online_id = "HostA";
    g_settings.game = net::kOnlineGame;
    g_settings.build = kEu;

    // An unreachable service: the context starts over with "service unavailable", which the game retries and then shows
    // as "Cannot connect to UNX Global Matrix". Nothing pretends to be online.
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

    // The real service: started, a server id, one world (what makes the game's "UNX Global Matrix" ready).
    g_seen.context.clear();
    g_settings.config.lobby = url;
    CHECK(Fn<Start>("sceNpMatching2ContextStart")(ctx, 10000000) == 0);
    CHECK(PumpUntil([] { return !g_seen.context.empty(); }));
    CHECK(!g_seen.context.empty() && g_seen.context[0] == std::make_pair(uint16_t(0x6f02), int32_t(0)));
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
    CHECK(room && room->me == 0x10 && room->me_name == "HostA" && room->copy == std::vector<uint8_t>{compat});
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
    CHECK(RequestById(members) && RequestById(members)->names == std::vector<std::string>({"HostA*", "GuestB"}));
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

    // Shutdown in the game's order: abort, shutdown, close; then the context stops.
    CHECK(Fn<Abort>("sceNetSocketAbort")(sock, 0) == 0);
    CHECK(Select(sock) & 0x10);
    CHECK(Fn<Abort>("sceNetShutdown")(sock, 2) == 0);
    CHECK(Fn<Close>("sceNetSocketClose")(sock) == 0);
    CHECK(online::peer_socket() == nullptr);
    CHECK(Fn<Stop>("sceNpMatching2ContextStop")(ctx) == 0);
    CHECK(PumpUntil([] { return g_seen.context.size() >= 2; }));
    CHECK(g_seen.context.size() >= 2 && g_seen.context.back().first == 0x6f03);
    guest.stop();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
