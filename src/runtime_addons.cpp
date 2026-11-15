#include "addon_install.h"
#include "runtime.h"
#include "utf8.h"
#include <fstream>
#include <iterator>
extern "C" void runtime_content_load_addons(const char *app0) {
    try {
        const auto path = aln::PathFromUtf8(app0);
        std::ifstream in(path / "sce_sys" / "param.sfo", std::ios::binary);
        const std::string sfo{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        const auto title = aln::SfoString(sfo, "TITLE_ID");
        const auto identity = aln::CheckSfoIdentity(sfo);
        if (identity.state != aln::GameFileState::kValid || !identity.build ||
            (title == "CUSA00062" && std::string_view(identity.build->version) != "1.06")) {
            puts("Runtime: local DLC requires European game update 1.06; add-ons skipped");
            return;
        }
        std::vector<std::string> diagnostics;
        const auto addons = aln::InstalledAddons(aln::AddonRootsForApp0(path), title, &diagnostics);
        for (const auto &line : diagnostics)
            printf("Runtime: %s\n", line.c_str());
        for (const auto &addon : addons) {
            if (runtime_content_addon(addon.label.c_str(), aln::Utf8(addon.directory).c_str())) {
                fprintf(stderr, "Runtime: skipping unregistrable DLC %s at %s\n", addon.label.c_str(),
                        aln::Utf8(addon.directory).c_str());
            }
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "Runtime: cannot load DLC: %s\n", e.what());
        // Optional DLC scan failures must not prevent the base game from starting.
    }
}
