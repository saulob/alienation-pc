// What the Online page shows, apart from drawing it: its tabs and sections, the Online play switch and the players the
// lobby lists.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/connection_test.h"
#include "net/json.h"

namespace aln {

// The launcher's own lobby link.
enum class OnlineLink { kOff, kConnecting, kUp, kUnreachable };

enum class OnlineSection {
    kOnlinePlay,
    kTabs,
    kStatusLine,
    kFriendRequests,
    kInvitations,
    kOnlineName,
    kFriendCode,
    kConnectionTest,
    kFriends,
    kPublicRooms,
    kLobbyVisibility,
    kLobbyPlayers
};

enum class OnlineTab { kSettings, kFriends, kLobby };
inline constexpr OnlineTab kOnlineTabs[] = {OnlineTab::kSettings, OnlineTab::kFriends, OnlineTab::kLobby};
const char* OnlineTabLabel(OnlineTab tab);

// With online play off only its switch is shown; on, the tabs and the sections of the selected one follow it.
std::vector<OnlineSection> OnlinePageSections(bool on, OnlineTab tab, size_t friend_requests, bool lobby_visible,
                                              size_t invitations = 0);

// `on` is the effective state: ALN_ONLINE, when set, wins over the saved switch and locks it.
struct OnlinePlaySwitch {
    bool on = false;
    bool locked = false;
};
OnlinePlaySwitch ResolveOnlinePlay(bool saved);

inline constexpr char kConnectionTestRequired[] = "Run the Connection test on the Online page before playing online.";

// What the Online page says about the connection: the Lobby row is the launcher's link as it is now; Public, Direct,
// Router and Relay come from the last finished Connection test, never assumed.
enum class ConnectionState { kOffline, kConnecting, kConnected, kLimited, kNotReady };
const char* ConnectionStateText(ConnectionState state);

struct ConnectionStatusRow {
    std::string label;
    net::ConnectionReport::Mark mark = net::ConnectionReport::Mark::kNote;
    const char* status = ""; // Ready, Limited, Not ready, Connecting or Not tested
    std::string text;
};

struct ConnectionStatus {
    ConnectionState state = ConnectionState::kOffline;
    std::string headline;
    std::vector<ConnectionStatusRow> rows;
    bool tested = false;
};

// `last_test` is null when no test has finished with Online play on.
ConnectionStatus ConnectionStatusOf(bool online_on, OnlineLink link, const net::ConnectionFacts* last_test, bool testing);
// "Tested just now", "Tested 5 min ago", "Tested 3 h ago", "Tested 2 days ago".
std::string TestedAgoText(int64_t seconds);
// Under the running Connection test on Windows, whose firewall may ask about the multiplayer port's first use; none
// elsewhere.
const char* ConnectionTestRunningNote(bool windows);

// How the Connection test section is drawn: while PLAY waits for the test it is a gold panel with the Run test button as
// the primary action; otherwise the plain row.
struct ConnectionTestLook {
    bool panel = false;
    bool primary_button = false;
    const char* required_text = nullptr;
};
ConnectionTestLook ConnectionTestLookOf(bool required);

// The panel's border and tint, 0 to 1, for the seconds since a blocked PLAY pointed at it: a few soft pulses that fade
// out; 0 before and after.
inline constexpr double kConnectionTestEmphasisSeconds = 2.5;
float ConnectionTestEmphasis(double seconds_since_blocked);

// The launcher asks whether PLAY may start every frame to grey its button, and once more when PLAY is pressed.
class PlayAttemptDetector {
public:
    bool Pressed(int frame) {
        const bool again = frame == last_frame_;
        last_frame_ = frame;
        return again;
    }

private:
    int last_frame_ = -1;
};

struct OnlinePlayer {
    std::string account; // names the player in lobby requests; never shown
    std::string name;
    bool online = false;
    bool in_room = false;
    // What the friend's own client advertised while online; empty or 0 when it did not say.
    std::string platform;
    std::string version;
    int online_protocol = 0;
    std::string build;     // the game build of the friend's room ("CUSA00062-1.00")
    bool joinable = false; // the room has a free seat, matches this PC's build and is open to this player
    std::string session;   // the room's NP session when this player may join it from the friend list
};

// The rows of friends.list "friends" or friends.requests "incoming"/"outgoing".
std::vector<OnlinePlayer> ReadOnlinePlayers(const net::Json& rows);
const char* FriendPresence(const OnlinePlayer& player);

// The Friends tab's headings with their counts: "Friends (2)", "Incoming requests (1)", "Outgoing requests (0)".
std::string FriendSectionTitle(const char* section, size_t count);
inline constexpr char kFriendsOnlyByRequest[] =
    "Players in your squad or in Lobby are not your friends until one sends a friend request and the other accepts it.";

// The line under a friend's name: "Online, in a room · Windows · v0.8.0", and whether to add "Incompatible" (online on
// another game build than `local_build`, or in a room of another online protocol).
struct FriendStatus {
    std::string text;
    bool incompatible = false;
};
FriendStatus FriendStatusOf(const OnlinePlayer& player, const std::string& local_build);
// A friend's squad this PC can enter through its session: Join on the Friends tab.
bool CanJoinFriend(const OnlinePlayer& player, const std::string& local_build);

// A public room of this PC's game build, as rooms.list shows it: never an address or a way to join from the launcher;
// the game's own Online menu finds and joins it.
struct PublicRoom {
    std::string host;
    int players = 0;
    int slots = 0;
    bool joinable = false;
    std::string version; // the host's port version; empty when it did not say
    int minutes = 0;     // since the room opened
    std::string session; // its NP session, for Join; empty when it cannot be joined
    // The game's searchable room attributes: 0x4d game_mode (bit 0 hardcore), 0x4f world_level; -1 when not set.
    int game_mode = -1, world_level = -1;
};
std::vector<PublicRoom> ReadPublicRooms(const net::Json& rows);
// "2/4 players · Joinable · Hardcore · World level 3 · v0.1.0 · 5 min"
std::string PublicRoomStatus(const PublicRoom& room);
std::string FriendRequestText(const OnlinePlayer& sender);

// A squad invitation from a friend, as invite.list and the invite.received event give it.
struct SquadInvitation {
    std::string invitation; // names it in invite.answer; never shown
    std::string session;
    std::string name;
    std::string build;
    int players = 0, slots = 0, minutes = 0;
    bool joinable = false;
};
std::vector<SquadInvitation> ReadSquadInvitations(const net::Json& rows);
// "2/4 players · 3 min ago", or why it cannot be accepted now.
std::string SquadInvitationText(const SquadInvitation& invitation, const std::string& local_build);
bool CanAcceptInvitation(const SquadInvitation& invitation, const std::string& local_build);

// The online_join.ini text the game reads at start.
std::string PendingJoinText(const std::string& session, const std::string& invitation, int64_t unix_seconds);

// What the lobby says this player is to the one reading the list.
enum class LobbyRelation { kNone, kPending, kFriend, kIncoming };

struct LobbyPlayer {
    std::string player; // the handle of this listing; never shown
    std::string name;
    std::string version; // empty from a client that did not say
    LobbyRelation relation = LobbyRelation::kNone;
};

// Requests to answer first, then pending, friends and everyone else, by name within each.
void SortLobbyPlayers(std::vector<LobbyPlayer>& players);
// The rows of lobby.list "players", sorted.
std::vector<LobbyPlayer> ReadLobbyPlayers(const net::Json& rows);
std::string LobbyVersionLabel(const LobbyPlayer& player);
std::string LobbyUpdatedText(int minutes);

struct LobbyAction {
    const char* text = nullptr;
    bool enabled = false;
    bool primary = false;
    bool decline = false; // Decline sits beside it
};
LobbyAction LobbyActionOf(LobbyRelation relation);

// When the Lobby list is read: rarely by itself, at once for a friend event or the button, one read at a time.
class LobbyRefresh {
public:
    static constexpr double kAutoSeconds = 300.0;
    // The lobby gives back one read every 5 s.
    static constexpr double kManualGapSeconds = 5.0;
    static constexpr double kRetrySeconds = 30.0;

    bool active() const { return active_; }
    bool Due(double now) const { return !active_ && now >= next_; }
    void Start(double now) {
        active_ = true;
        started_ = now;
        next_ = now + kAutoSeconds;
    }
    void Finish() { active_ = false; }
    void Fail(double now) {
        active_ = false;
        next_ = next_ < now + kRetrySeconds ? next_ : now + kRetrySeconds;
    }
    void Soon(double now) { next_ = now; }
    bool Manual(double now) {
        if (active_ || now - started_ < kManualGapSeconds) return false;
        next_ = now;
        return true;
    }
    void Reset() {
        active_ = false;
        next_ = 0;
    }

private:
    bool active_ = false;
    double next_ = 0;
    double started_ = -1e9;
};

} // namespace aln
