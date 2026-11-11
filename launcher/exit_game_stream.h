// Exit Game's bytecode patch in the guest's view of stream.dat (launcher/exit_game_script.h): the build's DATS block is
// read from the user's game, checked, patched, recompressed in place and served through the file overlay.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0 with the overlay registered; -1 with nothing registered, `detail` saying why.
int aln_exit_game_stream_install(const char *app0, const char *build_key, char *detail, size_t detail_size);

#ifdef __cplusplus
}
#endif
