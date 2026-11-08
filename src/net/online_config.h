// Online settings as the game and the launcher use them: alienation.ini values (launcher/online_settings.h) with the ALN_*
// environment overrides of docs/ONLINE.md applied on top.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aln::net {

// The UDP port the game binds for its peer-to-peer socket; when it is taken (a second instance on this PC), the system
// picks a free one.
inline constexpr uint16_t kGameUdpPort = 3658;
// Online play is not implemented for this game yet: no lobby, STUN server or UPnP mapping is used unless a developer
// override (ALN_LOBBY, ALN_STUN, ALN_UPNP) names one.
inline constexpr char kDefaultStunServers[] = "";
inline constexpr char kDefaultLobbyUrl[] = "";
inline constexpr char kDefaultOnlineName[] = "Player";

struct OnlineConfig {
    bool enabled = false;
    std::string lobby = kDefaultLobbyUrl;
    std::string name = kDefaultOnlineName; // as typed; net::online_name makes it the in-game id
    bool upnp = false;
    uint16_t udp_port = 0;       // 0: the game's port, or any free one
    std::string stun = kDefaultStunServers; // "" or "off": no STUN
    uint32_t bind_ip = 0; // never configurable: a test binds loopback so it needs no firewall rule
    bool lobby_visible = false; // the player chose to appear in the public Lobby
};

// ALN_ONLINE, ALN_LOBBY, ALN_ONLINE_NAME, ALN_UPNP, ALN_UDP_PORT and ALN_STUN, when set, replace the saved values.
void apply_environment(OnlineConfig& config);

// "host:port" entries of a comma list; the port defaults to 3478.
std::vector<std::pair<std::string, uint16_t>> stun_servers(const std::string& list);

bool parse_bool(const std::string& value, bool fallback);

} // namespace aln::net
