#include "exit_game_script.h"

#include <string.h>

/* Stated opcodes */
enum {
    OP_RESERVE = 0x00, OP_INT = 0x06, OP_STRING = 0x08, OP_PUSH_RETURN = 0x09, OP_PICK = 0x0b, OP_POP = 0x0f,
    OP_GET = 0x11, OP_NEW = 0x13, OP_SET = 0x14, OP_VAR = 0x17, OP_VAR_OR_NIL = 0x18, OP_JUMP = 0x1b,
    OP_JUMP_IF_FALSE = 0x1d, OP_NATIVE = 0x1f, OP_EQUAL = 0x2c, OP_LIST = 0x3f
};

/* Select Campaign's list when it is made: New and Load. Continue, when there is one, is inserted first later. */
#define CAMPAIGN_EXIT_INDEX 2

/* Field and state machine variable names, hashed as the bytecode refers to them. */
#define FIELD_TYPE 0x14958494u
#define FIELD_ID 0x39f744e4u
#define FIELD_TEXT 0xfc6fa961u
#define FIELD_BOX 0xe5e534ebu
#define FIELD_MESSAGE_ITEM 0x94ba2d0au
#define VAR_CHOSEN 0x4157579eu
#define VAR_SAVE_SLOTS 0x71f4ed37u

#define BYTECODE_MAGIC 0x214e0403u
#define STRING_TABLE 0x20u
#define IMPORT_TABLE 0x0cu
#define CODE_WORD 0x14u

/* 1.00 builds its Options list whole (three items, then Language inserted at 3): Credits lands at index 4. */
static const ExitGameScriptSite kSites[] = {
    {"CUSA00062-1.00", 0x475f6c00u, 0x28cb48u, 0x8b73c8u, 0xa32eu, 0x983ae236u, 0xf0602u, 0x6fa606u, 0x6fbbeau,
     0x6fe35du, 0x320du, 0x48fcu, 0x1du, 0x1c7u, 0x155u, 0x598dd2u, 0x5955b4u, 0x3357u, 0x2e07u, 5, 4, 0},
    {"CUSA00062-1.06", 0x493718c0u, 0x2abcdcu, 0x74486cu, 0xaaf5u, 0x95f05ed2u, 0xf87c7u, 0x575c36u, 0x5774c8u,
     0x57a574u, 0x343bu, 0x4bb9u, 0x1du, 0x1dbu, 0x15bu, 0x3fda66u, 0x3f91d1u, 0x358cu, 0x2ff9u, 2, EXIT_GAME_APPEND,
     0xaeu},
    {"CUSA04430-1.00", 0x489456c0u, 0x293956u, 0x8cbec0u, 0xa4e6u, 0xd8c6e3dfu, 0xf1b48u, 0x709fdcu, 0x70b5bfu,
     0x70de8au, 0x32acu, 0x4981u, 0x1du, 0x1d1u, 0x157u, 0x5a390cu, 0x59ff9eu, 0x33edu, 0x2e95u, 5, 4, 0},
};

/* The game turns its list_add calls into an intrinsic only inside the block's code range, so the appended code uses
 * list_insert and list_reverse, real natives. A list of unknown length gets its rows at the end by reversing it,
 * inserting at 0 and reversing it back: index 0 is always in range, and list_insert does not check its index. The
 * Select Campaign item uses the Load row's layout box; src/exit_game.c moves it down. */
enum { FIXED_ITEM_CODE = 62, APPEND_ITEM_CODE = 72, GUARD_CODE = 24, CAMPAIGN_ITEM_CODE = 56, CAMPAIGN_GUARD_CODE = 33 };

const ExitGameScriptSite *exit_game_script_site(const char *build) {
    for (size_t i = 0; build && i < sizeof(kSites) / sizeof(*kSites); ++i)
        if (!strcmp(build, kSites[i].build)) return &kSites[i];
    return NULL;
}

static size_t item_code_size(const ExitGameScriptSite *site) {
    return site->credits_index == EXIT_GAME_APPEND ? APPEND_ITEM_CODE : FIXED_ITEM_CODE;
}

size_t exit_game_script_extra(const ExitGameScriptSite *site) {
    return site ? item_code_size(site) + GUARD_CODE + CAMPAIGN_ITEM_CODE + CAMPAIGN_GUARD_CODE : 0;
}

static uint32_t read_u32(const uint8_t *at) {
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static uint16_t read_u16(const uint8_t *at) {
    return (uint16_t)(at[0] | at[1] << 8);
}

static uint8_t *put8(uint8_t *at, uint8_t value) {
    *at = value;
    return at + 1;
}

static uint8_t *put16(uint8_t *at, uint8_t op, uint16_t value) {
    at[0] = op;
    at[1] = (uint8_t)value;
    at[2] = (uint8_t)(value >> 8);
    return at + 3;
}

static uint8_t *put32(uint8_t *at, uint8_t op, uint32_t value) {
    at[0] = op;
    for (int i = 0; i < 4; ++i) at[1 + i] = (uint8_t)(value >> (8 * i));
    return at + 5;
}

static uint8_t *put_pick(uint8_t *at, uint8_t depth) {
    return put8(put8(at, OP_PICK), depth);
}

static const char *string_at(const uint8_t *sdat, size_t size, uint32_t index) {
    const uint32_t table = read_u32(sdat + STRING_TABLE) + 4u, count = read_u32(sdat + STRING_TABLE + 4);
    if (index >= count || (uint64_t)table + (uint64_t)count * 8u > size) return NULL;
    const uint32_t at = read_u32(sdat + table + index * 8u);
    if (at >= size || !memchr(sdat + at, 0, size - at)) return NULL;
    return (const char *)sdat + at;
}

static int import_is(const uint8_t *sdat, size_t size, uint16_t index, const char *name) {
    const uint32_t table = read_u32(sdat + IMPORT_TABLE), count = read_u32(sdat + IMPORT_TABLE + 4);
    if (index >= count || (uint64_t)table + (uint64_t)count * 4u > size) return 0;
    const char *found = string_at(sdat, size, read_u16(sdat + table + index * 4u));
    return found && !strcmp(found, name);
}

static int bytes_are(const uint8_t *sdat, size_t size, uint32_t at, const uint8_t *bytes, size_t count) {
    return at <= size && size - at >= count && !memcmp(sdat + at, bytes, count);
}

/* A row's text comes from a localize call whose pushed return address is the block that finishes the row: the patched
 * list operation must be in that block, not in an identical copy no path reaches. The row is the item whose id was set
 * to `id` shortly before the call. */
enum { ROW_BLOCK_LEAD = 13, ROW_ID_WINDOW = 40 };
static int row_returns_to(const uint8_t *sdat, size_t size, const ExitGameScriptSite *site, uint32_t list_op, uint16_t id) {
    if (list_op < ROW_BLOCK_LEAD) return 0;
    uint8_t pushret[5], id_set[8], *at = put32(pushret, OP_PUSH_RETURN, list_op - ROW_BLOCK_LEAD - site->code);
    at = put16(id_set, OP_STRING, id);
    put32(put_pick(at, 1), OP_SET, FIELD_ID);
    for (const uint8_t *found = sdat; (found = memchr(found, OP_PUSH_RETURN, size - (size_t)(found - sdat))) != NULL;
         ++found) {
        const size_t offset = (size_t)(found - sdat);
        if (offset + sizeof(pushret) > size || memcmp(found, pushret, sizeof(pushret))) continue;
        const size_t window = offset < ROW_ID_WINDOW ? offset : ROW_ID_WINDOW;
        for (size_t back = sizeof(id_set); back <= window; ++back)
            if (!memcmp(found - back, id_set, sizeof(id_set))) return 1;
    }
    return 0;
}

/* The Load item {id "load", layout box "LOAD"} shortly before `pick 1; pick 1; list 2; var-or-nil save slots`, and the
 * Load guard `reserve 1; get message item; string "load"; equal; jump-if-false`. */
enum { CAMPAIGN_ITEM_BACK = 0x3f, CAMPAIGN_LIST_LEAD = 4 };
static const char *verify_campaign(const uint8_t *sdat, size_t size, const ExitGameScriptSite *site) {
    const char *load = string_at(sdat, size, site->load), *load_box = string_at(sdat, size, site->load_box);
    if (!load || strcmp(load, "load") || !load_box || strcmp(load_box, "LOAD")) return "Select Campaign strings differ";
    uint8_t item[14], *at = put8(item, OP_NEW);
    at = put32(put_pick(put16(at, OP_STRING, site->load), 1), OP_SET, FIELD_ID);
    put16(at, OP_STRING, site->load_box);
    uint8_t list[11];
    at = put_pick(put_pick(list, 1), 1);
    put32(put8(put8(at, OP_LIST), 2), OP_VAR_OR_NIL, VAR_SAVE_SLOTS);
    if (site->campaign_list < CAMPAIGN_ITEM_BACK ||
        !bytes_are(sdat, size, site->campaign_list - CAMPAIGN_ITEM_BACK, item, sizeof(item)) ||
        !bytes_are(sdat, size, site->campaign_list - CAMPAIGN_LIST_LEAD, list, sizeof(list)))
        return "the Select Campaign list differs";
    if (!row_returns_to(sdat, size, site, site->campaign_list, site->load)) return "the Load row does not reach the list";
    uint8_t guard[12];
    at = put32(put8(put8(guard, OP_RESERVE), 1), OP_GET, FIELD_MESSAGE_ITEM);
    put8(put8(put16(at, OP_STRING, site->load), OP_EQUAL), OP_JUMP_IF_FALSE);
    if (!bytes_are(sdat, size, site->campaign_guard, guard, sizeof(guard))) return "the Select Campaign guard differs";
    return NULL;
}

static const char *verify(const uint8_t *sdat, size_t capacity, const ExitGameScriptSite *site) {
    const size_t size = site->dsize;
    if (capacity < size + exit_game_script_extra(site) || size < 0x30) return "no room for the patch";
    if (read_u32(sdat) != BYTECODE_MAGIC || read_u32(sdat + CODE_WORD) != site->code) return "not the bytecode block";
    const char *credits = string_at(sdat, size, site->credits), *marker = string_at(sdat, size, site->marker);
    if (!credits || strcmp(credits, "credits") || !marker || strcmp(marker, EXIT_GAME_MARKER))
        return "string table differs";
    if (!import_is(sdat, size, site->list_add, "list_add") || !import_is(sdat, size, site->list_insert, "list_insert") ||
        !import_is(sdat, size, site->exit_app, EXIT_GAME_MARKER) ||
        (site->credits_index == EXIT_GAME_APPEND && !import_is(sdat, size, site->list_reverse, "list_reverse")))
        return "native imports differ";
    uint8_t item[14], *at = put32(item, OP_SET, FIELD_TEXT);
    at = put16(put_pick(put_pick(at, 1), 1), OP_NATIVE, site->list_add);
    put8(put8(at, OP_POP), site->item_pop);
    if (!bytes_are(sdat, size, site->item - 9, item, sizeof(item))) return "the Credits item differs";
    if (!row_returns_to(sdat, size, site, site->item, site->credits)) return "the Credits row does not reach the item";
    uint8_t guard[14];
    at = put16(put32(guard, OP_VAR, VAR_CHOSEN), OP_STRING, site->credits);
    at = put8(at, OP_EQUAL);
    put32(at, OP_JUMP_IF_FALSE, site->fail - site->code);
    if (!bytes_are(sdat, size, site->guard - 9, guard, sizeof(guard))) return "the Credits guard differs";
    return verify_campaign(sdat, size, site);
}

size_t exit_game_script_patch(uint8_t *sdat, size_t capacity, const ExitGameScriptSite *site, const char **why) {
    const char *reason = site ? verify(sdat, capacity, site) : "not a supported build";
    if (reason) {
        if (why) *why = reason;
        return 0;
    }
    const int append = site->credits_index == EXIT_GAME_APPEND;
    const uint8_t first = append ? 0 : site->credits_index, second = append ? 0 : site->credits_index + 1;
    const uint32_t item_code = site->dsize, guard_code = item_code + (uint32_t)item_code_size(site);
    /* At the Credits list_add the stack ends with the list and Credits, then both again. */
    uint8_t *at = sdat + item_code;
    at = put8(put8(at, OP_POP), 1);
    if (append) at = put_pick(put16(at, OP_NATIVE, site->list_reverse), 1);
    at = put8(put8(at, OP_INT), first);
    at = put16(put_pick(at, 2), OP_NATIVE, site->list_insert);
    at = put8(put8(at, OP_RESERVE), 4);
    at = put8(at, OP_NEW);
    at = put32(put_pick(at, 1), OP_GET, FIELD_TYPE);
    at = put32(put_pick(at, 1), OP_SET, FIELD_TYPE);
    at = put32(put_pick(put16(at, OP_STRING, site->marker), 1), OP_SET, FIELD_ID);
    at = put32(put_pick(put16(at, OP_STRING, site->marker), 1), OP_SET, FIELD_TEXT);
    at = put_pick(at, 2);
    at = put8(put8(at, OP_INT), second);
    at = put16(put_pick(at, 2), OP_NATIVE, site->list_insert);
    if (append) at = put16(put_pick(at, 2), OP_NATIVE, site->list_reverse);
    at = put8(put8(at, OP_POP), (uint8_t)(site->item_pop + 1));
    at = put32(at, OP_JUMP, site->item + 5 - site->code);

    at = put8(put8(at, OP_RESERVE), 2);
    at = put16(put32(at, OP_VAR, VAR_CHOSEN), OP_STRING, site->marker);
    at = put8(at, OP_EQUAL);
    at = put32(at, OP_JUMP_IF_FALSE, site->fail - site->code);
    at = put16(at, OP_NATIVE, site->exit_app);
    at = put32(at, OP_JUMP, site->fail - site->code);

    const uint32_t campaign_item_code = (uint32_t)(at - sdat);
    at = put8(put8(at, OP_LIST), 2);
    at = put8(put8(at, OP_RESERVE), 4);
    at = put8(at, OP_NEW);
    at = put32(put_pick(put16(at, OP_STRING, site->marker), 1), OP_SET, FIELD_ID);
    at = put32(put_pick(put16(at, OP_STRING, site->load_box), 1), OP_SET, FIELD_BOX);
    at = put32(put_pick(put16(at, OP_STRING, site->marker), 1), OP_SET, FIELD_TEXT);
    at = put_pick(at, 1);
    at = put8(put8(at, OP_INT), CAMPAIGN_EXIT_INDEX);
    at = put16(put_pick(at, 2), OP_NATIVE, site->list_insert);
    at = put8(put8(at, OP_POP), 1);
    at = put32(at, OP_VAR_OR_NIL, VAR_SAVE_SLOTS);
    at = put32(at, OP_JUMP, site->campaign_list + 7 - site->code);

    const uint32_t campaign_guard_code = (uint32_t)(at - sdat);
    at = put8(put8(at, OP_RESERVE), 2);
    at = put32(put_pick(at, 0), OP_GET, FIELD_MESSAGE_ITEM);
    at = put8(put16(at, OP_STRING, site->marker), OP_EQUAL);
    uint8_t *skip = at;
    at += 5;
    at = put16(at, OP_NATIVE, site->exit_app);
    put32(skip, OP_JUMP_IF_FALSE, (uint32_t)(at - sdat) - site->code);
    at = put32(put8(put8(at, OP_RESERVE), 1), OP_GET, FIELD_MESSAGE_ITEM);
    at = put32(at, OP_JUMP, site->campaign_guard + 7 - site->code);

    put32(sdat + site->item, OP_JUMP, item_code - site->code);
    put32(sdat + site->guard, OP_JUMP_IF_FALSE, guard_code - site->code);
    put32(sdat + site->campaign_list, OP_JUMP, campaign_item_code - site->code);
    put32(sdat + site->campaign_guard, OP_JUMP, campaign_guard_code - site->code);
    return (size_t)(at - sdat);
}
