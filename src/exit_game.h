/* Exit Game in the Options and Select Campaign menus (the bytecode side is launcher/exit_game_script.h). Hooks on the
 * game: the text a row receives (a hud text in Options, a new layout box in Select Campaign) is swapped from the row's
 * marker to "Exit Game" in the game's language, and the native exit_game_application exits the port through the
 * registered handler instead of the console's application object. */
#ifndef ALN_EXIT_GAME_H
#define ALN_EXIT_GAME_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *build;
    uint32_t exit_app;         /* the exit_game_application native */
    uint32_t application;      /* the application object pointer it calls through */
    uint32_t text_copy;        /* the hud text set-text helper's call that copies the text bytes */
    uint32_t text_copy_target; /* that call's target */
    uint32_t box_new_text;     /* the call that copies a new layout box's text parameter */
    uint32_t box_new_text_copy;
    uint32_t box_offset_store; /* where the same function writes the box's offset and marks its rectangle stale */
    uint32_t language_name;    /* the current language's name getter */
    uint32_t language_index;   /* its globals: the current index and the 16-byte entry table */
    uint32_t language_table;
} ExitGameSite;

typedef int (*ExitGameHandler)(void);

const ExitGameSite *exit_game_site(const char *build);

/* "Exit Game" in the game's language name ("en", "fr", "bport", ...); English for any other name. */
const char *exit_game_text(const char *language);

void exit_game_set_handler(ExitGameHandler handler);
size_t exit_game_stub_bytes(void);

/* Checks every hooked byte. 0 with *why naming the failed check. */
int exit_game_verify(const uint8_t *image, uint64_t size, const ExitGameSite *site, const char **why);

/* Installs the hooks after exit_game_verify; 0, saying why, with nothing written otherwise. */
int exit_game_install(uint8_t *image, uint64_t size, const ExitGameSite *site, uint8_t *stubs, size_t stubs_size);

void GUEST_HOOK_ABI exit_game_text_hook(GuestRegs *regs);
void GUEST_HOOK_ABI exit_game_box_new_hook(GuestRegs *regs);
void GUEST_HOOK_ABI exit_game_exit_hook(GuestRegs *regs);

#ifdef __cplusplus
}
#endif

#endif
