// The game's peer-to-peer socket (libSceNet type 6 DGRAM_P2P, "udpp2p_socket") over net::PeerSocket. The game binds it to
// port 3658 with its vport, sends each packet to the address Matching2 signaling gave for a room member, and attributes a
// received datagram to a member by an exact match of that address and port. It polls with a new epoll every pass and
// sends only while the socket reports writable. Each datagram is one PeerSocket message, unchanged.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "net/upnp.h"
#include "runtime_online.h"

#define ALN_SYSV __attribute__((sysv_abi))

namespace aln::online {
namespace {

constexpr int32_t kEIntr = int32_t(0x80410104);
constexpr int32_t kEBadF = int32_t(0x80410109);
constexpr int32_t kEInval = int32_t(0x80410116);
constexpr int32_t kEAgain = int32_t(0x80410123);
constexpr int32_t kEMsgSize = int32_t(0x80410128);
constexpr int32_t kEAddrInUse = int32_t(0x80410130);
constexpr int32_t kENetUnreach = int32_t(0x80410133);
constexpr int32_t kENoBufs = int32_t(0x80410137);
constexpr int32_t kSockDgramP2P = 6;
constexpr int32_t kAfInet = 2;
constexpr int32_t kSolSocket = 0xffff, kSoNbio = 0x1200;
constexpr uint32_t kEpollIn = 0x1, kEpollOut = 0x2, kEpollErr = 0x8, kEpollHup = 0x10;
constexpr int32_t kEpollAdd = 1, kEpollMod = 2, kEpollDel = 3;
constexpr size_t kMaxDatagram = 0x10000;
constexpr uint64_t kRelinkMs = 2000;

struct Socket {
    bool bound = false, nonblocking = false, shut = false, aborted = false;
    uint16_t port = 0, vport = 0; // as the game asked, host order
};

struct Member {
    uint64_t account = 0;
    int link = -1;
    uint64_t linked_ms = 0;
};

struct Watch {
    uint32_t events = 0;
    uint64_t data = 0;
};

struct Epoll {
    std::map<int32_t, Watch> watches;
    bool aborted = false;
};

std::mutex g_mutex;
std::map<int32_t, Socket> g_sockets;
std::map<int32_t, Epoll> g_epolls;
int32_t g_next_id = 0x100;
int32_t g_bound = 0;
std::shared_ptr<net::PeerSocket> g_peer;
std::unique_ptr<net::upnp::PortMapping> g_upnp;
std::map<uint16_t, Member> g_members;
uint16_t g_next_reader = 0;
uint64_t g_dropped = 0;

uint16_t Swap16(uint16_t v) { return uint16_t(v >> 8 | v << 8); }

// SceNetSockaddrIn: len, family, port (network order) @2, address (network order) @4, vport (network order) @8.
void WriteAddress(uint8_t* out, const net::Endpoint& e, uint16_t vport) {
    std::memset(out, 0, 16);
    out[0] = 16;
    out[1] = kAfInet;
    const uint16_t port = Swap16(e.port), virtual_port = Swap16(vport);
    const uint32_t ip = __builtin_bswap32(e.ip);
    std::memcpy(out + 2, &port, 2);
    std::memcpy(out + 4, &ip, 4);
    std::memcpy(out + 8, &virtual_port, 2);
}

net::Endpoint ReadAddress(const uint8_t* in) {
    uint16_t port;
    uint32_t ip;
    std::memcpy(&port, in + 2, 2);
    std::memcpy(&ip, in + 4, 4);
    return {__builtin_bswap32(ip), Swap16(port)};
}

// The link to a member, started (again, once a dead link has rested) while the member belongs to the room. Caller holds
// g_mutex.
int Link(uint16_t id, Member& m) {
    if (!g_peer) return -1;
    const uint64_t now = net::now_ms();
    if (m.link >= 0 && g_peer->state(m.link) == net::LinkState::kClosed && now - m.linked_ms >= kRelinkMs) {
        g_peer->close(m.link);
        m.link = -1;
    }
    if (m.link < 0 && (!m.linked_ms || now - m.linked_ms >= kRelinkMs)) {
        m.link = g_peer->connect(m.account);
        m.linked_ms = now;
        if (m.link >= 0) std::printf("P2P: member 0x%x -> link %d\n", id, m.link);
    }
    return m.link;
}

void ReleaseMember(uint16_t id, Member& m, const char* why) {
    if (m.link >= 0 && g_peer) {
        const net::LinkStats s = g_peer->stats(m.link);
        std::printf("P2P: member 0x%x %s: in %llu datagrams / %llu B, out %llu datagrams / %llu B in %llu packets, %llu resends\n",
                    id, why, (unsigned long long)s.messages_in, (unsigned long long)s.bytes_in,
                    (unsigned long long)s.messages_out, (unsigned long long)s.bytes_out, (unsigned long long)s.packets_out,
                    (unsigned long long)s.resends);
        g_peer->close(m.link);
    }
    m.link = -1;
}

bool AnyReadable() {
    if (!g_peer) return false;
    for (auto& [id, m] : g_members)
        if (m.link >= 0 && g_peer->readable(m.link)) return true;
    return false;
}

uint32_t Ready(int32_t fd) {
    auto it = g_sockets.find(fd);
    if (it == g_sockets.end()) return kEpollHup | kEpollErr;
    const Socket& s = it->second;
    if (s.aborted) return kEpollHup;
    uint32_t ev = 0;
    if (s.bound && fd == g_bound && AnyReadable()) ev |= kEpollIn;
    if (s.bound && !s.shut) ev |= kEpollOut;
    return ev;
}

// One datagram from any member, round robin so one busy peer does not starve the others. Caller holds g_mutex.
bool TakeDatagram(std::vector<uint8_t>& data, uint16_t& from) {
    if (!g_peer || g_members.empty()) return false;
    auto start = g_members.lower_bound(g_next_reader);
    for (size_t n = 0; n < g_members.size(); ++n, ++start) {
        if (start == g_members.end()) start = g_members.begin();
        Member& m = start->second;
        if (m.link < 0 || !g_peer->receive(m.link, data)) continue;
        from = start->first;
        g_next_reader = uint16_t(from + 1);
        return true;
    }
    return false;
}

// ---- sockets ----
ALN_SYSV int32_t net_socket(const char* name, int32_t family, int32_t type, int32_t protocol) {
    (void)protocol;
    if (family != kAfInet || type != kSockDgramP2P) return kENetUnreach;
    std::lock_guard<std::mutex> lock(g_mutex);
    const int32_t id = g_next_id++;
    g_sockets[id] = Socket{};
    std::printf("Net: P2P socket %d (\"%s\")\n", id, name ? name : "");
    return id;
}

ALN_SYSV int32_t net_setsockopt(int32_t s, int32_t level, int32_t option, const void* value, uint32_t size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sockets.find(s);
    if (it == g_sockets.end()) return kEBadF;
    if (level == kSolSocket && option == kSoNbio && value && size >= 4) {
        int32_t on;
        std::memcpy(&on, value, 4);
        it->second.nonblocking = on != 0;
    }
    return 0;
}

ALN_SYSV int32_t net_getsockopt(int32_t s, int32_t level, int32_t option, void* value, uint32_t* size) {
    (void)level, (void)option;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_sockets.count(s)) return kEBadF;
    if (value && size && *size) std::memset(value, 0, *size);
    return 0;
}

ALN_SYSV int32_t net_bind(int32_t s, const uint8_t* address, uint32_t size) {
    if (!address || size < 16) return kEInval;
    uint16_t port, vport;
    std::memcpy(&port, address + 2, 2);
    std::memcpy(&vport, address + 8, 2);
    port = Swap16(port), vport = Swap16(vport);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sockets.find(s);
    if (it == g_sockets.end()) return kEBadF;
    if (g_peer) return kEAddrInUse;
    const Settings& cfg = settings();
    net::PeerSocket::Options options;
    options.port = cfg.config.udp_port ? cfg.config.udp_port : port;
    options.stun_servers = net::stun_servers(cfg.config.stun);
    options.bind_ip = cfg.config.bind_ip;
    std::string error;
    g_peer = net::PeerSocket::open(options, &error);
    if (!g_peer) {
        std::printf("Net: P2P socket %d cannot bind: %s\n", s, error.c_str());
        return kEAddrInUse;
    }
    it->second.bound = true;
    it->second.port = port;
    it->second.vport = vport;
    g_bound = s;
    std::printf("Net: P2P socket %d bound (game port %u, vport %u) to UDP %u\n", s, port, vport, g_peer->port());
    if (cfg.config.upnp) {
        g_upnp = std::make_unique<net::upnp::PortMapping>();
        g_upnp->start(g_peer->port());
    }
    for (auto& [id, m] : g_members) Link(id, m);
    return 0;
}

ALN_SYSV int32_t net_getsockname(int32_t s, uint8_t* address, uint32_t* size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sockets.find(s);
    if (it == g_sockets.end()) return kEBadF;
    if (!address || !size || *size < 16) return kEInval;
    WriteAddress(address, {0, it->second.port}, it->second.vport);
    *size = 16;
    return 0;
}

ALN_SYSV int64_t net_sendto(int32_t s, const void* data, uint64_t size, int32_t flags, const uint8_t* to, uint32_t to_size) {
    (void)flags;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sockets.find(s);
    if (it == g_sockets.end()) return kEBadF;
    if (it->second.aborted) return kEIntr;
    if (!it->second.bound || s != g_bound || !to || to_size < 16 || (!data && size)) return kEInval;
    if (size > kMaxDatagram) return kEMsgSize;
    // A datagram nobody can carry yet is lost, as UDP loses it; the game resends what matters. A connected link whose
    // queue is full says ENOBUFS, which the game takes as "stop sending this pass".
    const uint16_t id = peer_member(ReadAddress(to));
    auto m = g_members.find(id);
    const int link = m == g_members.end() ? -1 : Link(id, m->second);
    if (link >= 0 && g_peer->state(link) == net::LinkState::kConnected) {
        if (!g_peer->send(link, data, size_t(size))) return kENoBufs;
    } else if (g_dropped++ % 1000 == 0) {
        std::printf("P2P: datagram to %s dropped: member not connected (%llu so far)\n",
                    net::endpoint_text(ReadAddress(to)).c_str(), (unsigned long long)g_dropped);
    }
    return int64_t(size);
}

ALN_SYSV int64_t net_recvfrom(int32_t s, void* buffer, uint64_t size, int32_t flags, uint8_t* from, uint32_t* from_size) {
    (void)flags;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_sockets.find(s);
            if (it == g_sockets.end()) return kEBadF;
            if (it->second.aborted) return kEIntr;
            if (!it->second.bound || s != g_bound) return kEInval;
            std::vector<uint8_t> data;
            uint16_t member = 0;
            if (TakeDatagram(data, member)) {
                const size_t n = std::min<size_t>(data.size(), size_t(size));
                if (n && buffer) std::memcpy(buffer, data.data(), n);
                if (from && from_size && *from_size >= 16) {
                    WriteAddress(from, peer_endpoint(member), it->second.vport);
                    *from_size = 16;
                }
                return int64_t(n);
            }
            if (it->second.nonblocking) return kEAgain;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

ALN_SYSV int32_t net_shutdown(int32_t s, int32_t how) {
    (void)how;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sockets.find(s);
    if (it == g_sockets.end()) return kEBadF;
    it->second.shut = true;
    return 0;
}

ALN_SYSV int32_t net_socket_abort(int32_t s, int32_t flags) {
    (void)flags;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sockets.find(s);
    if (it == g_sockets.end()) return kEBadF;
    it->second.aborted = true;
    return 0;
}

ALN_SYSV int32_t net_socket_close(int32_t s) {
    std::unique_ptr<net::upnp::PortMapping> upnp;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_sockets.erase(s)) return kEBadF;
        if (s != g_bound) return 0;
        for (auto& [id, m] : g_members) ReleaseMember(id, m, "closed with the socket");
        g_peer.reset();
        g_bound = 0;
        upnp = std::move(g_upnp);
        std::printf("Net: P2P socket %d closed\n", s);
    }
    // Removing the router mapping can take seconds: not on the game's thread.
    if (upnp) std::thread([mapping = std::move(upnp)]() mutable { mapping.reset(); }).detach();
    return 0;
}

// ---- epoll: SceNetEpollEvent {u32 events @0, u32 reserved @4, i32 ident @8, u64 data @0x10} ----
ALN_SYSV int32_t net_epoll_create(const char* name, int32_t flags) {
    (void)name, (void)flags;
    std::lock_guard<std::mutex> lock(g_mutex);
    const int32_t id = g_next_id++;
    g_epolls[id] = Epoll{};
    return id;
}

ALN_SYSV int32_t net_epoll_destroy(int32_t ep) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_epolls.erase(ep) ? 0 : kEBadF;
}

ALN_SYSV int32_t net_epoll_control(int32_t ep, int32_t op, int32_t fd, const uint8_t* event) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_epolls.find(ep);
    if (it == g_epolls.end()) return kEBadF;
    if (op == kEpollDel) return it->second.watches.erase(fd) ? 0 : kEInval;
    if ((op != kEpollAdd && op != kEpollMod) || !event) return kEInval;
    Watch w;
    std::memcpy(&w.events, event, 4);
    std::memcpy(&w.data, event + 0x10, 8);
    it->second.watches[fd] = w;
    return 0;
}

ALN_SYSV int32_t net_epoll_wait(int32_t ep, uint8_t* events, int32_t max, int32_t timeout_us) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(std::max(timeout_us, 0));
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_epolls.find(ep);
            if (it == g_epolls.end()) return kEBadF;
            if (it->second.aborted) return kEIntr;
            int32_t n = 0;
            for (const auto& [fd, w] : it->second.watches) {
                if (n >= max || !events) break;
                const uint32_t hit = Ready(fd) & (w.events | kEpollErr | kEpollHup);
                if (!hit) continue;
                uint8_t* e = events + size_t(n) * 0x18;
                std::memset(e, 0, 0x18);
                std::memcpy(e, &hit, 4);
                std::memcpy(e + 8, &fd, 4);
                std::memcpy(e + 0x10, &w.data, 8);
                ++n;
            }
            if (n || timeout_us == 0 || (timeout_us > 0 && std::chrono::steady_clock::now() >= deadline)) return n;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

ALN_SYSV int32_t net_epoll_abort(int32_t ep, int32_t flags) {
    (void)flags;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_epolls.find(ep);
    if (it == g_epolls.end()) return kEBadF;
    it->second.aborted = true;
    return 0;
}

// Every other socket the game opens (debug consoles, HTTP) stays unreachable, as offline.
ALN_SYSV int32_t net_unreachable() { return kENetUnreach; }

struct Export {
    const char* name;
    void* function;
};

#define FN(name, fn) {name, reinterpret_cast<void*>(&fn)}
const Export kExports[] = {
    FN("sceNetSocket", net_socket),
    FN("sceNetSetsockopt", net_setsockopt),
    FN("sceNetGetsockopt", net_getsockopt),
    FN("sceNetBind", net_bind),
    FN("sceNetGetsockname", net_getsockname),
    FN("sceNetSendto", net_sendto),
    FN("sceNetRecvfrom", net_recvfrom),
    FN("sceNetShutdown", net_shutdown),
    FN("sceNetSocketAbort", net_socket_abort),
    FN("sceNetSocketClose", net_socket_close),
    FN("sceNetEpollCreate", net_epoll_create),
    FN("sceNetEpollDestroy", net_epoll_destroy),
    FN("sceNetEpollControl", net_epoll_control),
    FN("sceNetEpollWait", net_epoll_wait),
    FN("sceNetEpollAbort", net_epoll_abort),
    FN("sceNetConnect", net_unreachable),
    FN("sceNetListen", net_unreachable),
    FN("sceNetAccept", net_unreachable),
    FN("sceNetSend", net_unreachable),
    FN("sceNetRecv", net_unreachable),
};
#undef FN

} // namespace

std::shared_ptr<net::PeerSocket> peer_socket() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_peer;
}

UpnpView upnp_view() {
    std::lock_guard<std::mutex> lock(g_mutex);
    UpnpView view;
    if (!g_upnp) return view;
    const net::upnp::Status s = g_upnp->status();
    view.working = s.state == net::upnp::State::kWorking;
    view.mapped = s.state == net::upnp::State::kMapped;
    view.external = s.external;
    return view;
}

void p2p_set_member(uint16_t member, uint64_t account) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Member& m = g_members[member];
    if (m.account != account) {
        ReleaseMember(member, m, "replaced");
        m = Member{account};
    } else if (m.link < 0) {
        m.linked_ms = 0; // the peer was just introduced: no need to wait for the retry
    }
    Link(member, m);
}

void p2p_forget_member(uint16_t member) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_members.find(member);
    if (it == g_members.end()) return;
    ReleaseMember(member, it->second, "left the room");
    g_members.erase(it);
}

void p2p_forget_members() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [id, m] : g_members) ReleaseMember(id, m, "left the room");
    g_members.clear();
}

bool p2p_member_connected(uint16_t member) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_members.find(member);
    if (it == g_members.end() || !g_peer) return false;
    const int link = Link(member, it->second);
    return link >= 0 && g_peer->state(link) == net::LinkState::kConnected;
}

int p2p_rtt_ms(uint16_t member) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_members.find(member);
    if (it == g_members.end() || it->second.link < 0 || !g_peer) return -1;
    const net::LinkStats s = g_peer->stats(it->second.link);
    return s.rtt_sampled ? int(s.rtt_ms) : -1;
}

uintptr_t p2p_export(const std::string& symbol) {
    for (const Export& e : kExports)
        if (symbol == e.name) return reinterpret_cast<uintptr_t>(e.function);
    return 0;
}

} // namespace aln::online
