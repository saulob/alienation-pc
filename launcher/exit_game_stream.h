// The game's Stated bytecode patches in the guest's view of stream.dat: the build's DATS block is read from the user's
// game, checked, patched (Exit Game rows, launcher/exit_game_script.h; the network status box, online_status_script.h),
// recompressed in place and served through the file overlay.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ALN_SCRIPT_EXIT_GAME = 1, ALN_SCRIPT_ONLINE_STATUS = 2 };

typedef struct {
    int applied; // the ALN_SCRIPT_* patches the registered overlay holds; 0: nothing registered
    char exit_game[160];
    char online_status[160];
} AlnScriptPatches;

// Registers one overlay with every `wanted` patch that applies; each text says what happened to its patch.
void aln_script_stream_install(const char *app0, const char *build_key, int wanted, AlnScriptPatches *out);

#ifdef __cplusplus
}
#endif
