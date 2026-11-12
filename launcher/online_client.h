#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "net/identity.h"
#include "net/lobby_link.h"
#include "net/online_config.h"
#include "online_view.h"

namespace aln {

// The launcher's lobby link while online play is on. It shows the player online to friends and keeps the friend code,
// the friends, the requests and the squad invitations current from the lobby's events. With OnlineConfig::lobby_visible
// it also lists the player in the public Lobby and, while the Lobby tab is on screen, reads that list. Accepting an
// invitation or a Join leaves a pending join for the game the launcher starts next. Used from one thread.
class OnlineClient {
public:
    using Link = OnlineLink;
    struct State {
        Link link = Link::kOff;
        std::string friend_code;
        std::vector<OnlinePlayer> friends, incoming, outgoing;
        std::vector<SquadInvitation> invitations;
        bool fetched = false;
        std::string notice;
        std::vector<LobbyPlayer> lobby;
        size_t lobby_total = 0; // the lobby caps its answer: this counts those left out too
        bool lobby_fetched = false;
        double lobby_updated_at = 0; // on the clock passed to Update, when lobby_fetched
        std::string lobby_problem;
        std::vector<PublicRoom> rooms;
        size_t rooms_total = 0, rooms_incompatible = 0;
        bool rooms_fetched = false;
        std::string rooms_problem;
    };

    static constexpr double kNameSettleSeconds = 1.5;
    static constexpr double kRetrySeconds = 20.0;
    // Declines and renames by others send no event.
    static constexpr double kRefreshSeconds = 30.0;
    static constexpr double kLobbyWatchSeconds = 1.0;
    // The public rooms are read while the Lobby tab is on screen; the lobby allows one read every 5 s.
    static constexpr double kRoomsSeconds = 15.0;

    explicit OnlineClient(std::filesystem::path user_dir) : user_dir_(std::move(user_dir)) {}
    ~OnlineClient() {
        if (link_) link_->stop();
    }

    // Every frame. Off stops the link; on keeps one, started again for another lobby, game build or online name.
    // `build` ("CUSA00062-1.00", empty when no game is installed) selects the public rooms the Lobby tab shows.
    void Update(bool on, const net::OnlineConfig& config, const std::string& game, const std::string& build, double now);

    // The lists change at once; the lobby's answer and a fresh read follow.
    void AddFriend(const std::string& code);
    void Accept(const std::string& account);
    void Decline(const std::string& account); // also withdraws an outgoing request
    void Remove(const std::string& account);
    // A squad invitation: Accept asks the lobby for the session, then leaves the pending join; Decline only answers.
    void AcceptInvitation(const std::string& invitation);
    void DeclineInvitation(const std::string& invitation);
    // A friend's squad or a public room, through its session.
    void Join(const std::string& session, const std::string& who);
    struct PendingJoin {
        std::string session, invitation, who;
    };
    std::optional<PendingJoin> TakeJoin() { return std::exchange(join_, std::nullopt); }
    // Sends a friend request to a listed player, or accepts the one they sent.
    void RequestFromLobby(const std::string& player);
    void DeclineFromLobby(const std::string& player);
    // The Refresh lobby button; a read already running, or one that just ran, answers for it.
    void RefreshLobby(double now);
    void RefreshRooms(double now) { rooms_at_ = std::min(rooms_at_, now); }
    bool LobbyRefreshing() const { return lobby_refresh_.active(); }
    // Every frame the list is on screen: nothing reads it otherwise.
    void WatchLobby(double now) { lobby_watched_at_ = now; }

    const State& state() const { return state_; }

private:
    enum class Purpose {
        kFriends, kRequests, kAdd, kChange, kLobbyPublish, kLobbyList, kLobbyAdd, kLobbyChange, kRooms, kInvites,
        kInviteAccept, kInviteDecline
    };

    void Start(const net::OnlineConfig& config, const std::string& game, const std::string& name, double now);
    void SyncRooms(double now);
    void Stop();
    void Fetch(double now);
    void SyncLobby(bool visible, double now);
    void ForgetLobby();
    void LobbyAnswer(Purpose purpose, const net::LobbyMessage& message, double now);
    uint32_t Send(const std::string& op, net::Json body, Purpose purpose);
    void Handle(const net::LobbyMessage& message, double now);
    void Answer(Purpose purpose, const net::LobbyMessage& message, double now);

    std::filesystem::path user_dir_;
    std::unique_ptr<net::LobbyLink> link_;
    std::string url_, game_, name_, build_;
    net::Identity identity_;
    bool rooms_busy_ = false;
    double rooms_at_ = 0;
    std::string rooms_rev_;
    std::string wanted_name_;
    double name_since_ = 0;
    double retry_at_ = 0;
    double next_fetch_ = 0;
    bool fetching_ = false, fetch_again_ = false;
    bool lobby_told_ = false; // what the lobby was last asked; every new socket starts unlisted
    bool lobby_listed_ = false;
    LobbyRefresh lobby_refresh_;
    std::string lobby_rev_;
    double lobby_retry_at_ = 0;
    double lobby_watched_at_ = -1e9;
    std::map<uint32_t, Purpose> pending_;
    std::map<uint32_t, SquadInvitation> accepting_;
    std::optional<PendingJoin> join_;
    State state_;
};

} // namespace aln
