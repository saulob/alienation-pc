// The player's online identity: an account id and a secret made on this PC, kept in the user folder (docs/ONLINE.md).
// The lobby registers the account on first use and from then on accepts only the same secret.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace aln::net {

inline constexpr char kIdentityFileName[] = "online_identity.ini";

struct Identity {
    uint64_t account = 0;
    std::string secret; // 64 lower-case hex characters; never logged
    std::filesystem::path file;
    std::string error;  // why there is none
    bool ok() const { return account != 0 && secret.size() == 64; }
};

// Reads <user_dir>/online_identity.ini, creating it on first use. An existing file is never overwritten, so two
// processes starting together end up with the same identity.
Identity load_identity(const std::filesystem::path& user_dir, bool create = true);

// The name other players see: up to 16 letters, digits, '-' or '_' (the console's online id rules); other characters
// become '_'. `fallback` is used when nothing is left.
std::string online_name(const std::string& wanted, const std::string& fallback = "Player");

// The in-game online id (SceNpId) and the name the lobby shows: the Online name, or for the default one "Player-" and six
// hex digits of the account. The game tells room members apart by this id, so two profiles must never share it.
std::string online_id(const std::string& wanted, uint64_t account);

} // namespace aln::net
