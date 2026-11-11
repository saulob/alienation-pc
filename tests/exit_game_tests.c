#include "exit_game.h"
#include "exit_game_script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAILED: %s (line %d)\n", #condition, __LINE__); ++failures; } } while (0)

static const char *const kBuilds[] = {"CUSA00062-1.00", "CUSA00062-1.06", "CUSA04430-1.00"};

enum { FIELD_TYPE = 0x14958494u, FIELD_ID = 0x39f744e4u, FIELD_TEXT = 0xfc6fa961u, FIELD_BOX = 0xe5e534ebu,
       FIELD_MESSAGE_ITEM = 0x94ba2d0au, VAR_CHOSEN = 0x4157579eu, VAR_SAVE_SLOTS = 0x71f4ed37u };

static void put32(uint8_t *at, uint32_t value) {
    for (int i = 0; i < 4; ++i) at[i] = (uint8_t)(value >> (8 * i));
}

static void put16(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static uint32_t get32(const uint8_t *at) {
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static uint16_t get16(const uint8_t *at) {
    return (uint16_t)(at[0] | at[1] << 8);
}

/* A bytecode block with the site's strings, imports and the instructions the patch checks; everything else zero. */
enum { STRINGS = 0x1c4, ROW_CALLS = 0x70000, IMPORTS = 0x80000, NAMES = 0x90000 };

/* `string id; pick 1; set id; ...; push return` of a row whose localized text returns to the block of `list_op`. */
static void add_row_call(uint8_t *sdat, uint32_t at, const ExitGameScriptSite *site, uint16_t id, uint32_t list_op) {
    uint8_t *call = sdat + at;
    call[0] = 0x08;
    put16(call + 1, id);
    memcpy(call + 3, "\x0b\x01\x14", 3);
    put32(call + 6, FIELD_ID);
    call[20] = 0x09;
    put32(call + 21, list_op - 13 - site->code);
}
static uint8_t *make_bytecode(const ExitGameScriptSite *site, size_t capacity) {
    uint8_t *sdat = calloc(1, capacity);
    put32(sdat, 0x214e0403u);
    put32(sdat + 0x0c, IMPORTS);
    put32(sdat + 0x10, 0x200);
    put32(sdat + 0x14, site->code);
    put32(sdat + 0x20, STRINGS);
    put32(sdat + 0x24, 0x5000);
    const struct { uint16_t index; const char *text; } strings[] = {
        {site->credits, "credits"}, {site->marker, EXIT_GAME_MARKER}, {1, "list_add"}, {2, "list_insert"},
        {site->load, "load"}, {site->load_box, "LOAD"}, {3, "list_reverse"}};
    uint32_t name = NAMES;
    for (size_t i = 0; i < sizeof(strings) / sizeof(*strings); ++i) {
        put32(sdat + STRINGS + 4 + strings[i].index * 8u, name);
        strcpy((char *)sdat + name, strings[i].text);
        name += 32;
    }
    sdat[IMPORTS + site->list_add * 4u] = 1;
    sdat[IMPORTS + site->list_insert * 4u] = 2;
    if (site->credits_index == EXIT_GAME_APPEND) sdat[IMPORTS + site->list_reverse * 4u] = 3;
    put16(sdat + IMPORTS + site->exit_app * 4u, site->marker);

    uint8_t *item = sdat + site->item - 9;
    item[0] = 0x14;
    put32(item + 1, FIELD_TEXT);
    memcpy(item + 5, "\x0b\x01\x0b\x01\x1f", 5);
    put16(item + 10, site->list_add);
    item[12] = 0x0f;
    item[13] = site->item_pop;
    uint8_t *guard = sdat + site->guard - 9;
    guard[0] = 0x17;
    put32(guard + 1, VAR_CHOSEN);
    guard[5] = 0x08;
    put16(guard + 6, site->credits);
    guard[8] = 0x2c;
    guard[9] = 0x1d;
    put32(guard + 10, site->fail - site->code);

    uint8_t *load = sdat + site->campaign_list - 0x3f;
    load[0] = 0x13;
    load[1] = 0x08;
    put16(load + 2, site->load);
    memcpy(load + 4, "\x0b\x01\x14", 3);
    put32(load + 7, FIELD_ID);
    load[11] = 0x08;
    put16(load + 12, site->load_box);
    uint8_t *list = sdat + site->campaign_list - 4;
    memcpy(list, "\x0b\x01\x0b\x01\x3f\x02\x18", 7);
    put32(list + 7, VAR_SAVE_SLOTS);
    uint8_t *select = sdat + site->campaign_guard;
    memcpy(select, "\x00\x01\x11", 3);
    put32(select + 3, FIELD_MESSAGE_ITEM);
    select[7] = 0x08;
    put16(select + 8, site->load);
    select[10] = 0x2c;
    select[11] = 0x1d;
    put32(select + 12, 0x1234);
    add_row_call(sdat, ROW_CALLS, site, site->credits, site->item);
    add_row_call(sdat, ROW_CALLS + 0x40, site, site->load, site->campaign_list);
    return sdat;
}

/* Runs appended code the way the game would, for the instructions the patch writes. Values are small integers: tables
 * live in `tables`, the one list in `list`. It stops at a jump out of the appended code and returns its target. */
enum { LIST = 1000, TABLE = 2000, STRING = 3000, SAVE_SLOTS = 4000 };
typedef struct { int type, id, text, box, message_item; } Item;
typedef struct {
    int stack[32], depth;
    int list[8], count;
    Item tables[12];
    int tables_used, exit_calls;
} Machine;

static int *field(Item *item, uint32_t hash) {
    switch (hash) {
    case FIELD_TYPE: return &item->type;
    case FIELD_ID: return &item->id;
    case FIELD_TEXT: return &item->text;
    case FIELD_BOX: return &item->box;
    case FIELD_MESSAGE_ITEM: return &item->message_item;
    default: return NULL;
    }
}

static uint32_t run(Machine *m, const uint8_t *sdat, uint32_t start, const ExitGameScriptSite *site) {
    for (uint32_t at = start;;) {
        const uint8_t *op = sdat + at;
        if (m->depth < 0 || m->depth > 28) return 0;
        switch (*op) {
        case 0x00: at += 2; break;
        case 0x06: m->stack[m->depth++] = (int8_t)op[1]; at += 2; break;
        case 0x08: m->stack[m->depth++] = STRING + get16(op + 1); at += 3; break;
        case 0x0b: m->stack[m->depth] = m->stack[m->depth - 1 - op[1]]; ++m->depth; at += 2; break;
        case 0x0f: m->depth -= op[1]; at += 2; break;
        case 0x13: m->stack[m->depth++] = TABLE + m->tables_used++; at += 1; break;
        case 0x18: m->stack[m->depth++] = SAVE_SLOTS; at += 5; break;
        case 0x2c: --m->depth; m->stack[m->depth - 1] = m->stack[m->depth - 1] == m->stack[m->depth]; at += 1; break;
        case 0x3f: {
            const int n = op[1];
            m->count = n;
            memcpy(m->list, m->stack + m->depth - n, (size_t)n * sizeof(int));
            m->depth -= n;
            m->stack[m->depth++] = LIST;
            at += 2;
            break;
        }
        case 0x11: {
            int *value = field(&m->tables[m->stack[m->depth - 1] - TABLE], get32(op + 1));
            if (!value) return 0;
            m->stack[m->depth - 1] = *value;
            at += 5;
            break;
        }
        case 0x14: {
            int *value = field(&m->tables[m->stack[m->depth - 1] - TABLE], get32(op + 1));
            if (!value) return 0;
            *value = m->stack[m->depth - 2];
            m->depth -= 2;
            at += 5;
            break;
        }
        case 0x1f:
            if (get16(op + 1) == site->exit_app) {
                ++m->exit_calls;
            } else if (site->credits_index == EXIT_GAME_APPEND && get16(op + 1) == site->list_reverse &&
                       m->stack[m->depth - 1] == LIST) {
                for (int i = 0, j = m->count - 1; i < j; ++i, --j) {
                    const int swap = m->list[i];
                    m->list[i] = m->list[j];
                    m->list[j] = swap;
                }
                m->depth -= 1;
            } else if (get16(op + 1) == site->list_insert && m->stack[m->depth - 3] == LIST) {
                const int index = m->stack[m->depth - 2], value = m->stack[m->depth - 1];
                if (index < 0 || index > m->count) return 0;
                memmove(m->list + index + 1, m->list + index, (size_t)(m->count - index) * sizeof(int));
                m->list[index] = value;
                ++m->count;
                m->depth -= 3;
            } else {
                return 0; /* list_add, or anything else, from appended code */
            }
            at += 3;
            break;
        case 0x1d: {
            const uint32_t target = site->code + get32(op + 1);
            if (m->stack[--m->depth]) { at += 5; break; }
            if (target < site->dsize) return target;
            at = target;
            break;
        }
        case 0x1b: return site->code + get32(op + 1);
        default: return 0;
        }
    }
}

static int no_list_add(const uint8_t *code, size_t size, uint16_t list_add) {
    for (size_t i = 0; i + 2 < size; ++i)
        if (code[i] == 0x1f && get16(code + i + 1) == list_add) return 0;
    return 1;
}

/* Options: at the Credits list_add the stack holds the items list (four items), 3 and Language from the list_insert,
 * then the list and Credits twice. */
static void test_options(const uint8_t *sdat, const ExitGameScriptSite *site, uint32_t item_code) {
    Machine m = {{LIST, 3, TABLE + 3, LIST, TABLE + 4, LIST, TABLE + 4}, 7, {TABLE, TABLE + 1, TABLE + 2, TABLE + 3}, 4,
                 {{1, 10, 20, 0, 0}, {1, 11, 21, 0, 0}, {1, 12, 22, 0, 0}, {1, 13, 23, 0, 0},
                  {1, STRING + site->credits, 24, 0, 0}},
                 5, 0};
    CHECK(run(&m, sdat, item_code, site) == site->item + 5 && m.depth == 0);
    CHECK(m.count == 6);
    for (int i = 0; i < 5 && i < m.count; ++i) CHECK(m.list[i] == TABLE + i);
    const Item *exit_item = m.count == 6 ? &m.tables[m.list[5] - TABLE] : NULL;
    CHECK(exit_item && exit_item->type == 1 && exit_item->id == STRING + site->marker && exit_item->text == STRING + site->marker);
    CHECK(m.tables[4].id == STRING + site->credits && m.tables[4].text == 24);
}

/* Options built one list_add at a time (1.06): whatever the rows before it, Credits then Exit Game end the list, and the
 * stack holds what the original `pop` would have left. */
static void test_options_append(const uint8_t *sdat, const ExitGameScriptSite *site, uint32_t item_code) {
    static const int counts[] = {0, 1, 3, 5};
    for (size_t c = 0; c < sizeof(counts) / sizeof(*counts); ++c) {
        const int n = counts[c], below = 777;
        Machine m = {{below, LIST, TABLE + n, LIST, TABLE + n}, 5, {0}, n, {{0}}, n + 1, 0};
        for (int i = 0; i < n; ++i) {
            m.list[i] = TABLE + i;
            m.tables[i] = (Item){1, 10 + i, 20 + i, 0, 0};
        }
        m.tables[n] = (Item){1, STRING + site->credits, 24, 0, 0};
        CHECK(run(&m, sdat, item_code, site) == site->item + 5);
        CHECK(m.depth == 1 && m.stack[0] == below);
        CHECK(m.count == n + 2);
        for (int i = 0; i < n && i < m.count; ++i) CHECK(m.list[i] == TABLE + i);
        CHECK(m.list[n] == TABLE + n);
        const Item *exit_item = m.count == n + 2 ? &m.tables[m.list[n + 1] - TABLE] : NULL;
        CHECK(exit_item && exit_item->type == 1 && exit_item->id == STRING + site->marker &&
              exit_item->text == STRING + site->marker);
    }
}

static void test_options_guard(const uint8_t *sdat, const ExitGameScriptSite *site, uint32_t guard_code) {
    /* The guard runs after Credits' own compare failed: Exit Game's id exits once; another id leaves untouched. */
    const struct { int chosen, exits; } cases[] = {{STRING + site->marker, 1}, {STRING + site->load, 0}, {0, 0}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        /* Started after the reserve and the var read, with the chosen id already pushed. */
        Machine g = {{cases[i].chosen}, 1, {0}, 0, {{0}}, 0, 0};
        CHECK(sdat[guard_code + 2] == 0x17 && get32(sdat + guard_code + 3) == VAR_CHOSEN);
        CHECK(run(&g, sdat, guard_code + 2 + 5, site) == site->fail && g.exit_calls == cases[i].exits && g.depth == 0);
    }
}

/* Select Campaign: at `list 2` the stack holds New and Load and the same two again. */
static void test_campaign(const uint8_t *sdat, const ExitGameScriptSite *site, uint32_t item_code, uint32_t guard_code) {
    Machine m = {{TABLE, TABLE + 1, TABLE, TABLE + 1}, 4, {0}, 0,
                 {{0, STRING + 0x180, STRING + 0x181, STRING + 0x182, 0},
                  {0, STRING + site->load, STRING + 0x183, STRING + site->load_box, 0}},
                 2, 0};
    CHECK(run(&m, sdat, item_code, site) == site->campaign_list + 7);
    CHECK(m.depth == 4 && m.stack[2] == LIST && m.stack[3] == SAVE_SLOTS);
    CHECK(m.count == 3 && m.list[0] == TABLE && m.list[1] == TABLE + 1);
    const Item *exit_item = m.count == 3 ? &m.tables[m.list[2] - TABLE] : NULL;
    CHECK(exit_item && exit_item->id == STRING + site->marker && exit_item->text == STRING + site->marker &&
          exit_item->box == STRING + site->load_box);
    /* Continue, when there is one, is inserted first afterwards: Exit Game stays the last of four. */
    memmove(m.list + 1, m.list, 3 * sizeof(int));
    m.list[0] = TABLE + 9;
    CHECK(m.list[3] == exit_item - m.tables + TABLE);

    /* menu_select messages: Exit Game's id exits once, then the Load guard compares as before; any other id does not. */
    const int ids[] = {STRING + site->marker, STRING + site->load, STRING + 0x180};
    for (size_t i = 0; i < sizeof(ids) / sizeof(*ids); ++i) {
        Machine g = {{7, TABLE}, 2, {0}, 0, {{0, 0, 0, 0, ids[i]}}, 1, 0};
        CHECK(run(&g, sdat, guard_code, site) == site->campaign_guard + 7);
        CHECK(g.depth == 2 && g.stack[0] == 7 && g.stack[1] == ids[i] && g.exit_calls == (i == 0));
    }
}

static void test_script(const char *build) {
    const ExitGameScriptSite *site = exit_game_script_site(build);
    CHECK(site != NULL);
    if (!site) return;
    const int append = site->credits_index == EXIT_GAME_APPEND;
    const size_t capacity = site->dsize + exit_game_script_extra(site);
    CHECK(exit_game_script_extra(site) == (append ? 185u : 175u));
    uint8_t *sdat = make_bytecode(site, capacity);
    const char *why = NULL;
    CHECK(exit_game_script_patch(sdat, capacity - 1, site, &why) == 0 && why && !strcmp(why, "no room for the patch"));
    const size_t size = exit_game_script_patch(sdat, capacity, site, &why);
    CHECK(size == capacity);
    const uint32_t item_code = site->dsize, guard_code = item_code + (append ? 72 : 62), campaign_item = guard_code + 24,
                   campaign_guard = campaign_item + 56;
    CHECK(sdat[site->item] == 0x1b && site->code + get32(sdat + site->item + 1) == item_code);
    CHECK(sdat[site->guard] == 0x1d && site->code + get32(sdat + site->guard + 1) == guard_code);
    CHECK(sdat[site->campaign_list] == 0x1b && site->code + get32(sdat + site->campaign_list + 1) == campaign_item);
    CHECK(sdat[site->campaign_guard] == 0x1b && site->code + get32(sdat + site->campaign_guard + 1) == campaign_guard);
    CHECK(sdat[site->guard - 9] == 0x17 && get16(sdat + site->guard - 3) == site->credits);
    /* Outside the block's code range a list_add would not become the game's intrinsic. */
    CHECK(no_list_add(sdat + site->dsize, size - site->dsize, site->list_add));
    if (append)
        test_options_append(sdat, site, item_code);
    else
        test_options(sdat, site, item_code);
    test_options_guard(sdat, site, guard_code);
    test_campaign(sdat, site, campaign_item, campaign_guard);
    free(sdat);

    /* An identical list operation that no localize call returns to (1.06 has one) is never patched. */
    sdat = make_bytecode(site, capacity);
    put32(sdat + ROW_CALLS + 21, site->item - 13 - site->code + 0x40);
    CHECK(exit_game_script_patch(sdat, capacity, site, &why) == 0 && !strcmp(why, "the Credits row does not reach the item"));
    CHECK(sdat[site->item] == 0x1f);
    free(sdat);
    sdat = make_bytecode(site, capacity);
    put32(sdat + ROW_CALLS + 0x40 + 21, site->campaign_list - 13 - site->code + 0x40);
    CHECK(exit_game_script_patch(sdat, capacity, site, &why) == 0 && !strcmp(why, "the Load row does not reach the list"));
    free(sdat);

    if (append) {
        sdat = make_bytecode(site, capacity);
        sdat[NAMES + 6 * 32] = 'x';
        CHECK(exit_game_script_patch(sdat, capacity, site, &why) == 0 && !strcmp(why, "native imports differ"));
        CHECK(sdat[site->item] == 0x1f);
        free(sdat);
    }

    sdat = make_bytecode(site, capacity);
    sdat[site->guard - 1] = 0x2d;
    why = NULL;
    CHECK(exit_game_script_patch(sdat, capacity, site, &why) == 0 && why && !strcmp(why, "the Credits guard differs"));
    CHECK(sdat[site->item] == 0x1f);
    free(sdat);

    sdat = make_bytecode(site, capacity);
    sdat[site->campaign_guard + 10] = 0x2d;
    CHECK(exit_game_script_patch(sdat, capacity, site, &why) == 0 && !strcmp(why, "the Select Campaign guard differs"));
    CHECK(sdat[site->item] == 0x1f && sdat[site->campaign_list] == 0x3f);
    free(sdat);

    sdat = make_bytecode(site, capacity);
    sdat[site->campaign_list + 1] = 3;
    CHECK(exit_game_script_patch(sdat, capacity, site, &why) == 0 && !strcmp(why, "the Select Campaign list differs"));
    free(sdat);

    sdat = make_bytecode(site, capacity);
    strcpy((char *)sdat + NAMES + 32, "exit_game");
    CHECK(exit_game_script_patch(sdat, capacity, site, &why) == 0 && !strcmp(why, "string table differs"));
    free(sdat);
    CHECK(exit_game_script_patch(NULL, 0, NULL, &why) == 0 && !strcmp(why, "not a supported build"));
}

static void put_rel(uint8_t *image, uint32_t operand, uint32_t target) {
    put32(image + operand, target - (operand + 4));
}

enum { IMAGE_SIZE = 0x2d00000, STUB_BYTES = 4096 };

static uint8_t *make_image(const ExitGameSite *site) {
    uint8_t *image = calloc(1, IMAGE_SIZE + STUB_BYTES);
    memcpy(image + site->exit_app, "\x48\x8d\x05\0\0\0\0\x48\x8b\x38\x48\x8b\x07\xff\x60\x28", 16);
    put_rel(image, site->exit_app + 3, site->application);
    memcpy(image + site->text_copy - 26,
           "\x4c\x8d\x65\xc8\x48\x8d\x73\x20\x4d\x8d\xbe\xb0\x00\x00\x00\x8b\x43\x10\x4c\x89\xe7\x48\x8d\x54\x03\x20\xe8",
           27);
    put_rel(image, site->text_copy + 1, site->text_copy_target);
    memcpy(image + site->box_new_text - 10, "\x48\x8d\x9d\xd8\xfe\xff\xff\x48\x89\xdf\xe8", 11);
    put_rel(image, site->box_new_text + 1, site->box_new_text_copy);
    memcpy(image + site->box_offset_store, "\x41\x89\x47\x5c\x41\x89\x4f\x60\x41\xc6\x47\x48\x01", 13);
    memcpy(image + site->language_name,
           "\x48\x63\x05\0\0\0\0\x48\x8d\x0d\0\0\0\0\x48\xc1\xe0\x04\x48\x8d\x7c\x08\x08\xe9", 24);
    put_rel(image, site->language_name + 3, site->language_index);
    put_rel(image, site->language_name + 10, site->language_table);
    return image;
}

static int handler_calls;
static int count_handler(void) {
    return ++handler_calls == 1;
}

static void test_hooks(size_t build, int install) {
    const ExitGameSite *site = exit_game_site(kBuilds[build]);
    CHECK(site != NULL);
    if (!site) return;
    uint8_t *image = make_image(site);
    const char *why = NULL;
    CHECK(exit_game_verify(image, IMAGE_SIZE, site, &why));
    put_rel(image, site->text_copy + 1, site->text_copy_target + 16);
    CHECK(!exit_game_verify(image, IMAGE_SIZE, site, &why) && !strcmp(why, "the hud text copy differs"));
    put_rel(image, site->text_copy + 1, site->text_copy_target);
    image[site->exit_app + 13] = 0x90;
    CHECK(!exit_game_verify(image, IMAGE_SIZE, site, &why) && !strcmp(why, "exit_game_application differs"));
    image[site->exit_app + 13] = 0xff;
    image[site->box_offset_store + 7] = 0x64;
    CHECK(!exit_game_verify(image, IMAGE_SIZE, site, &why) && !strcmp(why, "the layout box text parameter differs"));
    image[site->box_offset_store + 7] = 0x60;
    image[site->box_new_text - 1] = 0xde;
    CHECK(!exit_game_verify(image, IMAGE_SIZE, site, &why) && !strcmp(why, "the layout box text parameter differs"));
    image[site->box_new_text - 1] = 0xdf;
    CHECK(!exit_game_verify(image, IMAGE_SIZE, NULL, &why) && !strcmp(why, "not a supported build"));
    if (!install) {
        free(image);
        return;
    }

    static const uint8_t block[] = {2, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 'f', 'r', 0};
    const uint8_t *holder = block;
    memcpy(image + site->language_table + 16 + 8, &holder, sizeof(holder));
    put32(image + site->language_index, 1);
    CHECK(exit_game_stub_bytes() == 3 * guest_hook_stub_size() && exit_game_stub_bytes() <= STUB_BYTES);
    CHECK(exit_game_install(image, IMAGE_SIZE, site, image + IMAGE_SIZE, STUB_BYTES));
    CHECK(image[site->exit_app + 7] == 0xe9 && image[site->text_copy] == 0xe9 && image[site->box_new_text] == 0xe9);

    /* Options: the hud text copy swaps exactly the marker. */
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    const char *marker = EXIT_GAME_MARKER, *other = "CREDITS";
    regs.rsi = (uintptr_t)other;
    regs.rdx = (uintptr_t)(other + strlen(other));
    exit_game_text_hook(&regs);
    CHECK(regs.rsi == (uintptr_t)other);
    regs.rsi = (uintptr_t)marker;
    regs.rdx = (uintptr_t)(marker + strlen(marker) - 1);
    exit_game_text_hook(&regs);
    CHECK(regs.rsi == (uintptr_t)marker);
    regs.rdx = (uintptr_t)(marker + strlen(marker));
    exit_game_text_hook(&regs);
    CHECK(regs.rdx - regs.rsi == strlen("QUITTER LE JEU") && !memcmp((const void *)(uintptr_t)regs.rsi, "QUITTER LE JEU", 14));

    /* Select Campaign: the new layout box gets the same text and moves one row below Load; other boxes are untouched. */
    uint8_t box[0x160] = {0};
    const float load_offset = 0.0f;
    memcpy(box + 0x60, &load_offset, sizeof(load_offset));
    memset(&regs, 0, sizeof(regs));
    regs.r15 = (uintptr_t)box;
    regs.rsi = (uintptr_t)"LOAD CAMPAIGN";
    exit_game_box_new_hook(&regs);
    CHECK(!strcmp((const char *)(uintptr_t)regs.rsi, "LOAD CAMPAIGN") && box[0x48] == 0);
    regs.rsi = 0;
    exit_game_box_new_hook(&regs);
    CHECK(regs.rsi == 0 && box[0x48] == 0);
    regs.rsi = (uintptr_t)marker;
    exit_game_box_new_hook(&regs);
    float offset;
    memcpy(&offset, box + 0x60, sizeof(offset));
    CHECK(!strcmp((const char *)(uintptr_t)regs.rsi, "QUITTER LE JEU") && offset == -0.15625f && box[0x48] == 1);
    put32(image + site->language_index, 100);
    regs.rsi = (uintptr_t)marker;
    exit_game_box_new_hook(&regs);
    CHECK(!strcmp((const char *)(uintptr_t)regs.rsi, "EXIT GAME"));

    /* Both menus reach the same native: one request to the host, however often the guard runs. */
    memset(&regs, 0, sizeof(regs));
    exit_game_exit_hook(&regs);
    CHECK(regs.skip == 0);
    exit_game_set_handler(count_handler);
    exit_game_exit_hook(&regs);
    CHECK(regs.skip == 1 && handler_calls == 1);
    regs.skip = 0;
    exit_game_exit_hook(&regs);
    CHECK(regs.skip == 1 && handler_calls == 1);
    exit_game_set_handler(NULL);
}

static char *read_source(const char *relative) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", ALN_SOURCE_DIR, relative);
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = calloc(1, (size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, file) != (size_t)size) text[0] = 0;
    fclose(file);
    return text;
}

/* The menu's request takes the window's own close path: the loop ends and the session is marked clean, finalized and
 * flushed before the process exits with 0. No second, harder exit exists. */
static void test_shutdown_path(void) {
    char *window = read_source("gpu/shim/window.cpp"), *gpu = read_source("gpu/shim/alngpu.cpp");
    char *window_h = read_source("gpu/shim/sdl_window.h"), *loader = read_source("src/loader.c");
    CHECK(window && gpu && window_h && loader);
    if (window && gpu && window_h && loader) {
        CHECK(!strstr(window, "hard exit") && strstr(window, "Exit Game: closing the window"));
        CHECK(strstr(window_h, "bool RequestClose() { return !close_requested.exchange(true); }"));
        const char *loop = strstr(gpu, "while (window->PollEvents())");
        const char *clean = loop ? strstr(loop, "runtime_log_mark_clean_exit();") : NULL;
        const char *finalize = clean ? strstr(clean, "runtime_log_finalize();") : NULL;
        const char *flush = finalize ? strstr(finalize, "runtime_log_flush();") : NULL;
        CHECK(loop && clean && finalize && flush && strstr(flush, "std::_Exit(0);"));
        CHECK(strstr(gpu, "if (!g_window || !g_window->RequestClose()) return 0;"));
        CHECK(strstr(loader, "exit_game_set_handler(alngpu_request_close);"));
        CHECK(!strstr(gpu, "TerminateProcess") && !strstr(window, "TerminateProcess") && !strstr(window, "abort("));
    }
    free(window);
    free(gpu);
    free(window_h);
    free(loader);
}

int main(void) {
    CHECK(!strcmp(exit_game_text("en"), "EXIT GAME"));
    CHECK(!strcmp(exit_game_text("bport"), "SAIR DO JOGO"));
    CHECK(!strcmp(exit_game_text("ru"), "ВЫЙТИ ИЗ ИГРЫ"));
    CHECK(!strcmp(exit_game_text("xx"), "EXIT GAME"));
    CHECK(!strcmp(exit_game_text(NULL), "EXIT GAME"));
    CHECK(exit_game_script_site("CUSA04181-1.00") == NULL && exit_game_site("CUSA04181-1.00") == NULL);
    CHECK(exit_game_site(NULL) == NULL);
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        test_script(kBuilds[i]);
        test_hooks(i, i == 0);
    }
    test_shutdown_path();
    printf("exit game tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
