// Local add-ons installed from the player's own packages. Never changes base files or saves.
#pragma once
#include "game_install.h"

namespace aln {
struct Addon {
    std::string content_id, label, title;
    std::filesystem::path directory;
};
// game/addcont/<title>/<16-character entitlement label>
std::filesystem::path AddonRoot(const GameRoots &roots, std::string_view title_id);
bool AddonIdentity(const std::string &sfo, std::string_view title_id, Addon &addon);
std::vector<Addon> InstalledAddons(const GameRoots &roots, std::string_view title_id,
                                   std::vector<std::string> *diagnostics = nullptr);
// Normalizes --app0, including trailing separators, into the same roots used by the launcher.
GameRoots AddonRootsForApp0(const std::filesystem::path &app0);
InstallMessage DescribeAddonInstall(const InstallResult &result);
// Removes an exclusively owned .install-* staging directory; returns an actionable pending-cleanup message.
std::string CleanupAddonStaging(const std::filesystem::path &directory);
InstallResult InstallAddon(const std::filesystem::path &source, const GameRoots &roots, std::string_view title_id,
                           const InstallHooks &hooks = {});
} // namespace aln
