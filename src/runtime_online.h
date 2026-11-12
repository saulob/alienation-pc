// Online play inside the game process (docs/ONLINE.md): the settings, the classic NP Matching2 rooms over the lobby and the
// game's peer-to-peer socket over net::PeerSocket. The C entry points are in runtime.h.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/addr.h"
#include "net/identity.h"
#include "net/json.h"
#include "net/online_config.h"
#include "net/peer_socket.h"

namespace aln::online {

struct Settings {
    bool enabled = false;
    net::OnlineConfig config;
    net::Identity identity;
    std::string online_id; // the SceNpId online id the game and other players see
    std::string game;
    std::string build; // "CUSA00062-1.00": only rooms of the same build ever meet
};

const Settings& settings();

// The game addresses room member m as kPeerAddress:m. The pair is the same on every PC of the room, never a real endpoint,
// so the game's exact address match on received datagrams holds whatever path (LAN, public, relay) a link takes.
inline constexpr uint32_t kPeerAddress = 0x7F414C4E; // 127.65.76.78
inline net::Endpoint peer_endpoint(uint16_t member) { return {kPeerAddress, member}; }
inline uint16_t peer_member(const net::Endpoint& e) { return e.ip == kPeerAddress ? e.port : 0; }

// Per-frame work on the thread that calls sceNpCheckCallback: lobby messages, the endpoint exchange and every Matching2
// callback.
void pump();
// Reads the developer overrides of the rooms (ALN_JOIN_CODE).
void matching2_configure();
// Keeps the lobby link up from now on (friends see the player online, invitations arrive), started again when it drops.
void start_presence();
// One lobby request answered on the calling thread (a WebApi request), up to `timeout_ms`; false with `code` otherwise.
// Game callbacks still run only from pump().
bool lobby_call(const std::string& op, net::Json body, net::Json* reply, std::string* code, int timeout_ms = 8000);
// A request whose answer nobody waits for.
void lobby_send(const std::string& op, net::Json body);
// The game is ending: leaves the room and closes the lobby link so the seat and the presence go at once; waits for the
// lobby's answer for at most a second.
void leave_for_exit();
std::string own_build();

// NP sessions, invitations and the Invitation dialog (runtime_webapi.cpp).
uintptr_t webapi_export(const std::string& symbol);
// A lobby "invite.*" or "friend.*" event, on the pump thread.
void sessions_event(const std::string& event, const net::Json& body);
// Per pump: invitation prompts and the launcher's pending join.
void sessions_pump();
// A Join or an accepted invitation the launcher left for this game process (online_join.ini in the profile).
void load_pending_join(const std::string& user_dir);
uintptr_t matching2_export(const std::string& symbol);
uintptr_t p2p_export(const std::string& symbol);

// The game's P2P socket once it is bound.
std::shared_ptr<net::PeerSocket> peer_socket();
struct UpnpView {
    bool working = false;
    bool mapped = false;
    net::Endpoint external;
};
UpnpView upnp_view();

// The room members the P2P socket may reach: Matching2 adds one when it tells the game the member is connected and
// removes it when the member leaves. The link to the member's account starts at once.
void p2p_set_member(uint16_t member, uint64_t account);
void p2p_forget_member(uint16_t member);
void p2p_forget_members();
bool p2p_member_connected(uint16_t member);
// The member's smoothed round trip on its link, -1 while unknown.
int p2p_rtt_ms(uint16_t member);

} // namespace aln::online
