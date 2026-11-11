// Online play for this process: the launcher's Online play setting (alienation.ini, ALN_* overrides), the profile's online
// identity and the game build. With it on, runtime_np.c signs the initial user in, and this layer answers Matching2 and
// the P2P socket; with it off every import keeps its offline behaviour (runtime_services.c).
#include "runtime.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "net/addr.h"
#include "net/identity.h"
#include "net/online_config.h"
#include "net/online_version.h"
#include "online_settings.h"
#include "runtime_online.h"

namespace aln::online {
namespace {
Settings g_settings;
} // namespace

const Settings& settings() { return g_settings; }

} // namespace aln::online

using namespace aln;

extern "C" void runtime_online_configure(const char* settings_file, const char* user_dir, const char* build) {
    online::Settings& s = online::g_settings;
    s = online::Settings{};
    s.config = ReadOnlineConfig(settings_file ? settings_file : "");
    net::apply_environment(s.config);
    s.game = net::kOnlineGame;
    runtime_np_configure(0, 0, nullptr, 0);
    if (!s.config.enabled) {
        std::printf("Online: off\n");
        return;
    }
    if (!build || !*build) {
        std::printf("Online: not available for an unknown game build; playing offline\n");
        return;
    }
    s.identity = net::load_identity(user_dir ? user_dir : "user");
    if (!s.identity.ok()) {
        std::printf("Online: no online identity (%s); playing offline\n", s.identity.error.c_str());
        return;
    }
    s.build = build;
    s.online_id = net::online_id(s.config.name, s.identity.account);
    s.enabled = true;
    uint32_t ip = net::default_route_ip();
    if (!ip) ip = 0x7f000001;
    runtime_np_configure(1, s.identity.account, s.online_id.c_str(), ip);
    runtime_np_set_pump(runtime_online_pump);
    online::matching2_configure();
    std::printf("Online: on as %s, build %s, protocol %d, lobby %s, UDP port %s, UPnP %s\n", s.online_id.c_str(), build,
                net::kOnlineProtocolVersion, s.config.lobby.empty() ? "not set" : s.config.lobby.c_str(),
                s.config.udp_port ? std::to_string(s.config.udp_port).c_str() : "3658 or any free one",
                s.config.upnp ? "on" : "off");
}

extern "C" int runtime_online_active(void) { return online::g_settings.enabled ? 1 : 0; }

extern "C" void runtime_online_pump(void) {
    if (online::g_settings.enabled) online::pump();
}

extern "C" uintptr_t runtime_online_resolve(const char* name) {
    if (!online::g_settings.enabled || !name) return 0;
    const char* symbol = runtime_symbol(name);
    if (!symbol) return 0;
    const std::string s(symbol);
    if (const uintptr_t f = online::matching2_export(s)) return f;
    return online::p2p_export(s);
}
