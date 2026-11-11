/* Exit Game rows in the Options and Select Campaign menus. Both are state machines in the game's Stated bytecode (the
 * stream.dat DATS block): a page builds a list of items and leaves through a guard when a confirmed item's id matches.
 * The patch adds an item whose id and text are the bytecode's own "exit_game_application" string to each list, and a
 * guard that calls the native exit_game_application for that id. src/exit_game.c shows the row's text and makes the
 * native exit the port. */
#ifndef ALN_EXIT_GAME_SCRIPT_H
#define ALN_EXIT_GAME_SCRIPT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *build;
    uint64_t block;          /* stream.dat offset of the DATS block */
    uint32_t zsize, dsize, units, crc; /* the original block; CRC-32 of its compressed payload */
    uint32_t code;           /* code base, header word at 0x14 */
    uint32_t item;           /* `call list_add; pop 5` adding the Credits item */
    uint32_t guard;          /* the jump-if-false leaving the Credits guard */
    uint32_t fail;           /* its target */
    uint16_t credits;        /* string index of "credits" */
    uint16_t marker;         /* string index of "exit_game_application" */
    uint16_t list_add;       /* import indexes */
    uint16_t list_insert;
    uint16_t exit_app;
    uint32_t campaign_list;  /* Select Campaign: `list 2` making the New and Load items a list */
    uint32_t campaign_guard; /* the Load guard comparing a menu_select message's item id */
    uint16_t load;           /* string indexes of "load" (the item id) and "LOAD" (its layout box) */
    uint16_t load_box;
    uint8_t item_pop;        /* the pop after the Credits list_add */
    uint8_t credits_index;   /* Credits' index in an Options list made whole by `list`; EXIT_GAME_APPEND otherwise */
    uint16_t list_reverse;   /* import index, used when the Options list grows one list_add at a time */
} ExitGameScriptSite;

#define EXIT_GAME_BLOCK_HEADER 0x28u
#define EXIT_GAME_MARKER "exit_game_application"
/* The Options rows are added one by one, some only under conditions, so their count is not known statically. */
#define EXIT_GAME_APPEND 0xffu

const ExitGameScriptSite *exit_game_script_site(const char *build);

/* Bytes the patch appends to the site's bytecode. */
size_t exit_game_script_extra(const ExitGameScriptSite *site);

/* Patches `sdat` (the decompressed block, `site->dsize` bytes) in place and appends the new code; `capacity` must hold
 * dsize + exit_game_script_extra(site). Returns the new size, or 0 with *why set and nothing written when a checked
 * byte, string or import differs. */
size_t exit_game_script_patch(uint8_t *sdat, size_t capacity, const ExitGameScriptSite *site, const char **why);

#ifdef __cplusplus
}
#endif

#endif
