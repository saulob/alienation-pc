#include "online_settings.h"

#include <cstdlib>
#include <string>

#include "net/connection_test.h"
#include "settings.h"

namespace aln {

bool IsOnlineSetting(std::string_view key) {
    for (const std::string_view online :
         {kOnlineEnabledKey, kOnlineNameKey, kOnlineUdpPortKey, kOnlineStunKey, kConnectionTestPortKey,
          kConnectionTestResultKey}) {
        if (key == online) {
            return true;
        }
    }
    return false;
}

bool IsValidOnlineSetting(std::string_view key, std::string_view value) {
    if (key == kOnlineUdpPortKey || key == kConnectionTestPortKey) {
        if (value.empty() || value.size() > 5 || value.find_first_not_of("0123456789") != std::string_view::npos) {
            return false;
        }
        return std::atoi(std::string(value).c_str()) <= 65535;
    }
    if (key == kOnlineNameKey) {
        return value.size() <= 64;
    }
    if (key == kConnectionTestResultKey) {
        net::ConnectionFacts facts;
        int64_t when = 0;
        return value.empty() || net::decode_connection_facts(std::string(value), facts, when);
    }
    return true;
}

net::OnlineConfig ReadOnlineConfig(const std::filesystem::path& settings_file) {
    if (settings_file.empty()) {
        return {};
    }
    return OnlineConfigOf(LauncherSettings(settings_file));
}

net::OnlineConfig OnlineConfigOf(const trg::SettingsStore& settings) {
    net::OnlineConfig config;
    config.enabled = settings.GetBool(kOnlineEnabledKey, false);
    config.name = settings.Get(kOnlineNameKey, net::kDefaultOnlineName);
    config.udp_port = static_cast<uint16_t>(std::atoi(settings.Get(kOnlineUdpPortKey, "0").c_str()));
    config.stun = settings.Get(kOnlineStunKey, net::kDefaultStunServers);
    config.lobby_visible = settings.GetBool(kLobbyVisibleKey, false);
    return config;
}

bool ConnectionTestNeeded(const trg::SettingsStore& settings, bool online_on, bool testing, uint16_t port) {
    if (!online_on) {
        return false;
    }
    return testing || std::atoi(settings.Get(kConnectionTestPortKey, "0").c_str()) != port;
}

bool RecordConnectionTest(LauncherSettings& settings, uint16_t port, const std::string& result) {
    return settings.SaveNow({{kConnectionTestPortKey, std::to_string(port)}, {kConnectionTestResultKey, result}});
}

void ForgetConnectionTestWhenOff(trg::SettingsStore& settings, bool online_on) {
    if (online_on) return;
    if (settings.Get(kConnectionTestPortKey, "0") != "0") settings.Set(kConnectionTestPortKey, "0");
    if (!settings.Get(kConnectionTestResultKey, "").empty()) settings.Set(kConnectionTestResultKey, "");
}

} // namespace aln
