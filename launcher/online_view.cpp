#include "online_view.h"

#include "online_settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <tuple>

#include "net/online_config.h"
#include "net/online_version.h"

namespace aln {

const char* OnlineTabLabel(OnlineTab tab) {
    switch (tab) {
    case OnlineTab::kSettings: return "Settings";
    case OnlineTab::kFriends: return "Friends";
    case OnlineTab::kLobby: return "Lobby";
    }
    return "";
}

std::vector<OnlineSection> OnlinePageSections(bool on, OnlineTab tab, size_t friend_requests, bool lobby_visible,
                                              size_t invitations) {
    if (!on) {
        return {OnlineSection::kOnlinePlay};
    }
    std::vector<OnlineSection> sections{OnlineSection::kOnlinePlay, OnlineSection::kTabs};
    if (tab != OnlineTab::kSettings) sections.push_back(OnlineSection::kStatusLine);
    switch (tab) {
    case OnlineTab::kSettings:
        for (const OnlineSection section : {OnlineSection::kOnlineName, OnlineSection::kFriendCode,
                                            OnlineSection::kConnectionTest}) {
            sections.push_back(section);
        }
        break;
    case OnlineTab::kFriends:
        if (invitations > 0) {
            sections.push_back(OnlineSection::kInvitations);
        }
        if (friend_requests > 0) {
            sections.push_back(OnlineSection::kFriendRequests);
        }
        sections.push_back(OnlineSection::kFriends);
        break;
    case OnlineTab::kLobby:
        sections.push_back(OnlineSection::kPublicRooms);
        sections.push_back(OnlineSection::kLobbyVisibility);
        if (lobby_visible) {
            sections.push_back(OnlineSection::kLobbyPlayers);
        }
        break;
    }
    return sections;
}

OnlinePlaySwitch ResolveOnlinePlay(bool saved) {
    const char* forced = std::getenv("ALN_ONLINE");
    if (!forced || !*forced) {
        return {saved, false};
    }
    return {net::parse_bool(forced, saved), true};
}

const char* ConnectionTestRunningNote(bool windows) {
    return windows ? "If Windows asks whether to allow network access, choose Allow access so other players can connect "
                     "to you."
                   : nullptr;
}

const char* ConnectionStateText(ConnectionState state) {
    switch (state) {
    case ConnectionState::kOffline: return "Offline";
    case ConnectionState::kConnecting: return "Connecting";
    case ConnectionState::kConnected: return "Connected";
    case ConnectionState::kLimited: return "Limited";
    case ConnectionState::kNotReady: return "Not ready";
    }
    return "";
}

ConnectionStatus ConnectionStatusOf(bool online_on, OnlineLink link, const net::ConnectionFacts* last_test, bool testing) {
    using Mark = net::ConnectionReport::Mark;
    ConnectionStatus status;
    if (!online_on) {
        status.headline = "Online play is off: the game makes no online connections.";
        return status;
    }
    const bool up = link == OnlineLink::kUp;
    const bool unreachable = link == OnlineLink::kUnreachable;
    if (up) {
        status.rows.push_back({"Lobby", Mark::kOk, "Ready", "Connected to the online service."});
    } else if (unreachable) {
        status.rows.push_back({"Lobby", Mark::kProblem, "Not ready", "Lobby unavailable: the online service cannot be reached."});
    } else {
        status.rows.push_back({"Lobby", Mark::kNote, "Connecting", "Connecting to the online service..."});
    }
    net::ConnectionReport report;
    if (last_test) {
        net::ConnectionFacts facts = *last_test;
        // While the link connects, the test's own sign-in stands; once it is up or unreachable, the link decides.
        if (up || unreachable) facts.lobby_ok = up;
        report = net::summarize_connection(facts);
        status.tested = true;
        for (size_t i = 1; i < report.lines.size(); ++i) {
            const net::ConnectionReport::Line& line = report.lines[i];
            status.rows.push_back({line.label, line.mark, net::ConnectionStatusText(line.mark), line.text});
        }
    } else {
        for (const char* label : {"Public", "Direct", "Router", "Relay"}) {
            status.rows.push_back({label, Mark::kNote, "Not tested", "Run the Connection test."});
        }
    }
    if (unreachable) {
        status.state = ConnectionState::kNotReady;
        status.headline = "Not ready: the online service cannot be reached right now.";
    } else if (!up) {
        status.state = ConnectionState::kConnecting;
        status.headline = "Connecting to the online service...";
    } else if (testing) {
        status.state = ConnectionState::kConnected;
        status.headline = "Connected. Testing how other players reach you...";
    } else if (!status.tested) {
        status.state = ConnectionState::kConnected;
        status.headline = "Connected. Run the Connection test to check how other players reach you.";
    } else if (report.ready) {
        const bool direct = report.lines.size() > 2 && report.lines[2].mark == Mark::kOk;
        status.state = ConnectionState::kConnected;
        status.headline = direct ? "Connected. Ready for online play."
                                 : "Connected. Ready for online play: players who cannot connect to you directly play "
                                   "through the relay.";
    } else {
        status.state = ConnectionState::kLimited;
        status.headline = report.verdict + ".";
    }
    return status;
}

std::string TestedAgoText(int64_t seconds) {
    if (seconds < 60) return "Tested just now";
    if (seconds < 3600) return "Tested " + std::to_string(seconds / 60) + " min ago";
    if (seconds < 2 * 86400) return "Tested " + std::to_string(seconds / 3600) + " h ago";
    return "Tested " + std::to_string(seconds / 86400) + " days ago";
}

ConnectionTestLook ConnectionTestLookOf(bool required) {
    return required ? ConnectionTestLook{true, true, "Connection test required"} : ConnectionTestLook{};
}

float ConnectionTestEmphasis(double seconds_since_blocked) {
    if (seconds_since_blocked < 0.0 || seconds_since_blocked >= kConnectionTestEmphasisSeconds) return 0.0f;
    const double progress = seconds_since_blocked / kConnectionTestEmphasisSeconds;
    const double pulse = 0.5 * (1.0 - std::cos(2.0 * 3.14159265358979 * 3.0 * progress));
    return static_cast<float>(pulse * (1.0 - 0.5 * progress));
}

std::vector<OnlinePlayer> ReadOnlinePlayers(const net::Json& rows) {
    std::vector<OnlinePlayer> players;
    for (const net::Json& row : rows.items()) {
        const std::string& shown = row["display_name"].as_string();
        OnlinePlayer player{row["account"].as_string(), shown.empty() ? row["name"].as_string() : shown, row["online"].as_bool(),
                            row["room"].as_u64() != 0, row["platform"].as_string(), row["version"].as_string(),
                            int(row["online_protocol"].as_int()), row["build"].as_string(), row["joinable"].as_bool(),
                            row["session"].as_string()};
        if (player.name.empty()) {
            player.name = net::kDefaultOnlineName;
        }
        players.push_back(std::move(player));
    }
    return players;
}

const char* FriendPresence(const OnlinePlayer& player) {
    if (player.in_room) return player.joinable ? "Online, in a joinable room" : "Online, in a room";
    return player.online ? "Online" : "Offline";
}

FriendStatus FriendStatusOf(const OnlinePlayer& player, const std::string& local_build) {
    FriendStatus status{FriendPresence(player)};
    if (!player.online) {
        return status;
    }
    if (!player.platform.empty()) {
        status.text += " \xC2\xB7 " + player.platform;
    }
    if (!player.version.empty()) {
        status.text += " \xC2\xB7 v" + player.version;
    }
    const bool other_build = !player.build.empty() && !local_build.empty() && player.build != local_build;
    status.incompatible = other_build || (player.in_room && !net::online_compatible(player.online_protocol));
    return status;
}

std::string FriendSectionTitle(const char* section, size_t count) {
    return std::string(section) + " (" + std::to_string(count) + ")";
}

bool CanJoinFriend(const OnlinePlayer& player, const std::string& local_build) {
    return player.online && player.joinable && !player.session.empty() && !local_build.empty() && player.build == local_build &&
           net::online_compatible(player.online_protocol);
}

std::vector<PublicRoom> ReadPublicRooms(const net::Json& rows) {
    std::vector<PublicRoom> rooms;
    for (const net::Json& row : rows.items()) {
        PublicRoom room{row["host"].as_string(), int(row["players"].as_int()), int(row["slots"].as_int()),
                        row["joinable"].as_bool(), row["version"].as_string(), int(row["minutes"].as_int()),
                        row["session"].as_string()};
        for (const net::Json& attr : row["ints"].items()) {
            if (attr.items().size() != 2) continue;
            const int64_t id = attr.items()[0].as_int(), value = attr.items()[1].as_int();
            if (id == 0x4d) room.game_mode = int(value);
            if (id == 0x4f) room.world_level = int(value);
        }
        if (room.host.empty() || room.players < 1 || room.slots < room.players) continue;
        rooms.push_back(std::move(room));
    }
    return rooms;
}

std::string PublicRoomStatus(const PublicRoom& room) {
    std::string text = std::to_string(room.players) + "/" + std::to_string(room.slots) + " players \xC2\xB7 ";
    text += room.joinable ? "Joinable" : "Full";
    if (room.game_mode >= 0) text += room.game_mode & 1 ? " \xC2\xB7 Hardcore" : " \xC2\xB7 Normal";
    if (room.world_level > 0) text += " \xC2\xB7 World level " + std::to_string(room.world_level);
    if (!room.version.empty()) text += " \xC2\xB7 v" + room.version;
    text += " \xC2\xB7 " + (room.minutes < 1 ? std::string("just opened") : std::to_string(room.minutes) + " min");
    return text;
}

std::string FriendRequestText(const OnlinePlayer& sender) {
    return sender.name + " wants to be your friend";
}

std::vector<SquadInvitation> ReadSquadInvitations(const net::Json& rows) {
    std::vector<SquadInvitation> invitations;
    for (const net::Json& row : rows.items()) {
        const std::string& shown = row["display_name"].as_string();
        SquadInvitation i{row["invitation"].as_string(), row["session"].as_string(), shown.empty() ? row["name"].as_string() : shown,
                          row["build"].as_string(), int(row["players"].as_int()), int(row["slots"].as_int()),
                          int(row["minutes"].as_int()), row["joinable"].as_bool()};
        if (i.invitation.empty() || i.session.empty()) continue;
        if (i.name.empty()) i.name = net::kDefaultOnlineName;
        invitations.push_back(std::move(i));
    }
    return invitations;
}

bool CanAcceptInvitation(const SquadInvitation& invitation, const std::string& local_build) {
    return invitation.joinable && !local_build.empty() && invitation.build == local_build;
}

std::string SquadInvitationText(const SquadInvitation& invitation, const std::string& local_build) {
    if (local_build.empty()) return "Install the game to join.";
    if (invitation.build != local_build) return "Another game version or region: " + invitation.build + ".";
    if (!invitation.joinable) return "The squad is full or closed.";
    return std::to_string(invitation.players) + "/" + std::to_string(invitation.slots) + " players \xC2\xB7 " +
           (invitation.minutes < 1 ? std::string("just now") : std::to_string(invitation.minutes) + " min ago");
}

std::string PendingJoinText(const std::string& session, const std::string& invitation, int64_t unix_seconds) {
    return "session=" + session + "\ninvitation=" + invitation + "\ntime=" + std::to_string(unix_seconds) + "\n";
}

void SortLobbyPlayers(std::vector<LobbyPlayer>& players) {
    const auto rank = [](LobbyRelation relation) {
        switch (relation) {
        case LobbyRelation::kIncoming: return 0;
        case LobbyRelation::kPending: return 1;
        case LobbyRelation::kFriend: return 2;
        case LobbyRelation::kNone: break;
        }
        return 3;
    };
    const auto folded = [](std::string text) {
        for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    std::stable_sort(players.begin(), players.end(), [&](const LobbyPlayer& a, const LobbyPlayer& b) {
        return std::tuple(rank(a.relation), folded(a.name), a.player) < std::tuple(rank(b.relation), folded(b.name), b.player);
    });
}

std::vector<LobbyPlayer> ReadLobbyPlayers(const net::Json& rows) {
    std::vector<LobbyPlayer> players;
    for (const net::Json& row : rows.items()) {
        const std::string state = row["state"].as_string();
        LobbyPlayer player{row["player"].as_string(), row["name"].as_string(), row["version"].as_string()};
        if (player.player.empty() || player.name.empty()) {
            continue;
        }
        player.relation = state == "friend"     ? LobbyRelation::kFriend
                          : state == "pending"  ? LobbyRelation::kPending
                          : state == "incoming" ? LobbyRelation::kIncoming
                                                : LobbyRelation::kNone;
        players.push_back(std::move(player));
    }
    SortLobbyPlayers(players);
    return players;
}

std::string LobbyVersionLabel(const LobbyPlayer& player) {
    return player.version.empty() ? "Version unknown" : "v" + player.version;
}

std::string LobbyUpdatedText(int minutes) {
    return minutes < 1 ? "Updated just now" : "Updated " + std::to_string(minutes) + " min ago";
}

LobbyAction LobbyActionOf(LobbyRelation relation) {
    switch (relation) {
    case LobbyRelation::kNone: return {"Send friend request", true, false};
    case LobbyRelation::kPending: return {"Pending", false, false};
    case LobbyRelation::kFriend: return {"Friend", false, false};
    case LobbyRelation::kIncoming: return {"Accept", true, true, true};
    }
    return {};
}

} // namespace aln
