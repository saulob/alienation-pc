// The Online page without a window: its sections, the connection status built from the live link and the last
// Connection test, the stored test result, friends, invitations and public rooms as the lobby lists them.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "net/connection_test.h"
#include "net/json.h"
#include "online_view.h"

using namespace aln;

namespace {

int g_failures = 0;

#define CHECK(cond)                                             \
    do {                                                        \
        if (!(cond)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

using Mark = net::ConnectionReport::Mark;

net::ConnectionFacts ReadyFacts() {
    net::ConnectionFacts f;
    f.lobby_ok = true;
    f.relay_ok = true;
    f.lobby_latency_ms = 42;
    f.udp_port = 3658;
    f.udp_ok = true;
    f.second_answer = true;
    f.same_mapping = true;
    return f;
}

std::vector<std::string> Labels(const ConnectionStatus& status) {
    std::vector<std::string> labels;
    for (const ConnectionStatusRow& row : status.rows) labels.push_back(row.label);
    return labels;
}

void Sections() {
    CHECK(OnlinePageSections(false, OnlineTab::kSettings, 0, false) == std::vector<OnlineSection>{OnlineSection::kOnlinePlay});
    const auto settings = OnlinePageSections(true, OnlineTab::kSettings, 0, false);
    CHECK(settings == (std::vector<OnlineSection>{OnlineSection::kOnlinePlay, OnlineSection::kTabs, OnlineSection::kOnlineName,
                                                  OnlineSection::kFriendCode, OnlineSection::kConnectionTest}));
    const auto friends = OnlinePageSections(true, OnlineTab::kFriends, 2, false);
    CHECK(friends.size() >= 4 && friends[2] == OnlineSection::kStatusLine);
    CHECK(std::find(friends.begin(), friends.end(), OnlineSection::kFriendRequests) != friends.end());
    const auto lobby = OnlinePageSections(true, OnlineTab::kLobby, 0, true);
    CHECK(lobby[2] == OnlineSection::kStatusLine && std::find(lobby.begin(), lobby.end(), OnlineSection::kPublicRooms) != lobby.end());
    CHECK(std::find(lobby.begin(), lobby.end(), OnlineSection::kLobbyPlayers) != lobby.end());
}

void StatusOffline() {
    const ConnectionStatus off = ConnectionStatusOf(false, OnlineLink::kOff, nullptr, false);
    CHECK(off.state == ConnectionState::kOffline && off.rows.empty());
    CHECK(std::string(ConnectionStateText(off.state)) == "Offline");
}

void StatusWithoutTest() {
    const ConnectionStatus connecting = ConnectionStatusOf(true, OnlineLink::kConnecting, nullptr, false);
    CHECK(connecting.state == ConnectionState::kConnecting);
    CHECK(Labels(connecting) == (std::vector<std::string>{"Lobby", "Public", "Direct", "Router", "Relay"}));
    CHECK(std::string(connecting.rows[0].status) == "Connecting");
    for (size_t i = 1; i < connecting.rows.size(); ++i) CHECK(std::string(connecting.rows[i].status) == "Not tested");
    const ConnectionStatus up = ConnectionStatusOf(true, OnlineLink::kUp, nullptr, false);
    CHECK(up.state == ConnectionState::kConnected && !up.tested && std::string(up.rows[0].status) == "Ready");
    // Nothing but the live lobby link is ever shown Ready without a measurement.
    for (size_t i = 1; i < up.rows.size(); ++i) CHECK(up.rows[i].mark != Mark::kOk);
    const ConnectionStatus down = ConnectionStatusOf(true, OnlineLink::kUnreachable, nullptr, false);
    CHECK(down.state == ConnectionState::kNotReady && down.rows[0].mark == Mark::kProblem);
    CHECK(down.rows[0].text.find("Lobby unavailable") == 0);
}

void StatusFromTest() {
    const net::ConnectionFacts ready = ReadyFacts();
    const ConnectionStatus ok = ConnectionStatusOf(true, OnlineLink::kUp, &ready, false);
    CHECK(ok.state == ConnectionState::kConnected && ok.tested);
    CHECK(Labels(ok) == (std::vector<std::string>{"Lobby", "Public", "Direct", "Router", "Relay"}));
    CHECK(ok.rows[1].mark == Mark::kOk && ok.rows[2].mark == Mark::kOk && ok.rows[4].mark == Mark::kOk);
    CHECK(ok.rows[3].mark == Mark::kNote && ok.rows[3].text.find("not required") != std::string::npos);
    CHECK(ok.headline == "Connected. Ready for online play.");

    // A symmetric NAT: direct is limited, the relay keeps multiplayer working.
    net::ConnectionFacts symmetric = ready;
    symmetric.same_mapping = false;
    const ConnectionStatus relay = ConnectionStatusOf(true, OnlineLink::kUp, &symmetric, false);
    CHECK(relay.state == ConnectionState::kConnected && relay.rows[2].mark == Mark::kNote && relay.rows[4].mark == Mark::kOk);
    CHECK(relay.headline.find("relay") != std::string::npos);

    // No relay and no direct path: limited.
    net::ConnectionFacts closed = symmetric;
    closed.relay_ok = false;
    const ConnectionStatus limited = ConnectionStatusOf(true, OnlineLink::kUp, &closed, false);
    CHECK(limited.state == ConnectionState::kLimited && limited.rows[4].mark == Mark::kProblem);
    CHECK(limited.headline.rfind("Limited", 0) == 0);

    // The UDP port could not be opened.
    net::ConnectionFacts port = ready;
    port.port_open = false;
    const ConnectionStatus blocked = ConnectionStatusOf(true, OnlineLink::kUp, &port, false);
    CHECK(blocked.state == ConnectionState::kLimited && blocked.rows[1].mark == Mark::kProblem);

    // The live link wins over the test's sign-in: an unreachable lobby makes the relay unusable now.
    const ConnectionStatus lost = ConnectionStatusOf(true, OnlineLink::kUnreachable, &ready, false);
    CHECK(lost.state == ConnectionState::kNotReady && lost.rows[4].mark == Mark::kProblem);
    // While the link connects, the test's measurements stand.
    const ConnectionStatus reconnecting = ConnectionStatusOf(true, OnlineLink::kConnecting, &ready, false);
    CHECK(reconnecting.state == ConnectionState::kConnecting && reconnecting.rows[4].mark == Mark::kOk);
    CHECK(ConnectionStatusOf(true, OnlineLink::kUp, &ready, true).headline.find("Testing") != std::string::npos);
}

void FactsCodec() {
    net::ConnectionFacts f = ReadyFacts();
    f.router = net::ConnectionFacts::Router::kMappedBehindNat;
    f.upnp_off = true;
    const std::string text = net::encode_connection_facts(f, 1762900000);
    CHECK(text.find('.') == std::string::npos); // no address of any kind
    net::ConnectionFacts back;
    int64_t when = 0;
    CHECK(net::decode_connection_facts(text, back, when) && when == 1762900000);
    CHECK(back.lobby_ok && back.relay_ok && back.udp_ok && back.second_answer && back.same_mapping && back.upnp_off);
    CHECK(back.router == net::ConnectionFacts::Router::kMappedBehindNat && back.udp_port == 3658 && back.lobby_latency_ms == 42);
    CHECK(net::summarize_connection(back).verdict == net::summarize_connection(f).verdict);
    for (const char* bad : {"", "2:11111111101:3658:1:5", "1:1111111110:3658:1:5", "1:11111111131:3658:1:5",
                            "1:11111111101:70000:1:5", "1:11111111101:3658:1:0", "garbage"}) {
        CHECK(!net::decode_connection_facts(bad, back, when));
    }
    CHECK(TestedAgoText(5) == "Tested just now" && TestedAgoText(300) == "Tested 5 min ago");
    CHECK(TestedAgoText(7200) == "Tested 2 h ago" && TestedAgoText(3 * 86400) == "Tested 3 days ago");
}

net::Json Parse(const char* text) { return *net::Json::parse(text); }

void FriendsAndJoin() {
    const std::string eu = "CUSA00062-1.00";
    const auto friends = ReadOnlinePlayers(Parse(R"([
        {"account":"1","name":"Alpha","online":true,"room":"4096","build":"CUSA00062-1.00","joinable":true,
         "online_protocol":1,"session":"0123abcd-0000-0000-0000-000000000000"},
        {"account":"2","name":"Bravo","online":true,"room":"4097","build":"CUSA00062-1.06","joinable":false,"online_protocol":1},
        {"account":"3","name":"Charlie","online":true,"room":"4098","build":"CUSA00062-1.00","joinable":false,"online_protocol":1},
        {"account":"4","name":"Delta","online":false},
        {"account":"5","name":"Echo","online":true,"online_protocol":1,"build":"CUSA00062-1.00"}])"));
    CHECK(friends.size() == 5);
    CHECK(FriendStatusOf(friends[0], eu).text.rfind("Online, in a joinable room", 0) == 0 && CanJoinFriend(friends[0], eu));
    CHECK(FriendStatusOf(friends[1], eu).incompatible && !CanJoinFriend(friends[1], eu));
    CHECK(FriendStatusOf(friends[2], eu).text.rfind("Online, in a room", 0) == 0 && !CanJoinFriend(friends[2], eu));
    CHECK(FriendStatusOf(friends[3], eu).text == "Offline" && !CanJoinFriend(friends[3], eu));
    CHECK(FriendStatusOf(friends[4], eu).text.rfind("Online", 0) == 0 && !CanJoinFriend(friends[4], eu));
    CHECK(!CanJoinFriend(friends[0], "") && !CanJoinFriend(friends[0], "CUSA00062-1.06"));
    CHECK(!FriendStatusOf(friends[4], eu).incompatible && FriendStatusOf(friends[4], "CUSA04430-1.00").incompatible);
    CHECK(!FriendStatusOf(friends[3], "CUSA04430-1.00").incompatible);
    CHECK(FriendSectionTitle("Friends", 2) == "Friends (2)" && FriendSectionTitle("Outgoing requests", 0) == "Outgoing requests (0)");
    const auto requests = ReadOnlinePlayers(Parse(R"([{"account":"9","name":"Bravo","ms":1}])"));
    CHECK(requests.size() == 1 && requests[0].name == "Bravo" && !requests[0].online);

    const auto rooms = ReadPublicRooms(Parse(R"([
        {"host":"Alpha","players":2,"slots":4,"joinable":true,"minutes":3,"session":"s-1"},
        {"host":"Full","players":4,"slots":4,"joinable":false,"minutes":0},
        {"host":"Squad","players":1,"slots":4,"joinable":true,"minutes":12,"session":"s-2","version":"0.2.0",
         "ints":[[76,10],[77,1],[78,40],[79,3]]},
        {"host":"Easy","players":1,"slots":4,"joinable":true,"ints":[[77,0],[79,0]]},
        {"host":"","players":1,"slots":4}])"));
    CHECK(rooms.size() == 4 && rooms[0].session == "s-1" && rooms[1].session.empty());
    CHECK(PublicRoomStatus(rooms[0]) == "2/4 players \xC2\xB7 Joinable \xC2\xB7 3 min");
    CHECK(PublicRoomStatus(rooms[1]) == "4/4 players \xC2\xB7 Full \xC2\xB7 just opened");
    CHECK(rooms[2].game_mode == 1 && rooms[2].world_level == 3);
    CHECK(PublicRoomStatus(rooms[2]) ==
          "1/4 players \xC2\xB7 Joinable \xC2\xB7 Hardcore \xC2\xB7 World level 3 \xC2\xB7 v0.2.0 \xC2\xB7 12 min");
    CHECK(PublicRoomStatus(rooms[3]) == "1/4 players \xC2\xB7 Joinable \xC2\xB7 Normal \xC2\xB7 just opened");
}

void Invitations() {
    const std::string eu = "CUSA00062-1.00";
    const auto invitations = ReadSquadInvitations(Parse(R"([
        {"invitation":"aa","session":"s-1","name":"Alpha","build":"CUSA00062-1.00","players":2,"slots":4,"minutes":0,"joinable":true},
        {"invitation":"bb","session":"s-2","name":"Player","display_name":"Player-1A2B","build":"CUSA00062-1.00","players":4,
         "slots":4,"minutes":5,"joinable":false},
        {"invitation":"cc","session":"s-3","name":"Korea","build":"CUSA04430-1.00","players":1,"slots":4,"minutes":2,"joinable":false},
        {"invitation":"","session":"s-4","name":"Broken"}])"));
    CHECK(invitations.size() == 3);
    CHECK(invitations[1].name == "Player-1A2B");
    CHECK(CanAcceptInvitation(invitations[0], eu) && SquadInvitationText(invitations[0], eu) == "2/4 players \xC2\xB7 just now");
    CHECK(!CanAcceptInvitation(invitations[1], eu) && SquadInvitationText(invitations[1], eu) == "The squad is full or closed.");
    CHECK(!CanAcceptInvitation(invitations[2], eu) &&
          SquadInvitationText(invitations[2], eu) == "Another game version or region: CUSA04430-1.00.");
    CHECK(!CanAcceptInvitation(invitations[0], "") && SquadInvitationText(invitations[0], "") == "Install the game to join.");
    const auto sections = OnlinePageSections(true, OnlineTab::kFriends, 1, false, 2);
    CHECK(sections.size() >= 5 && sections[3] == OnlineSection::kInvitations && sections[4] == OnlineSection::kFriendRequests);
    CHECK(PendingJoinText("s-1", "aa", 1762900000) == "session=s-1\ninvitation=aa\ntime=1762900000\n");
    CHECK(PendingJoinText("s-1", "", 5) == "session=s-1\ninvitation=\ntime=5\n");
}

} // namespace

int main() {
    Sections();
    FriendsAndJoin();
    Invitations();
    StatusOffline();
    StatusWithoutTest();
    StatusFromTest();
    FactsCodec();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "online view tests OK", g_failures);
    return g_failures ? 1 : 0;
}
