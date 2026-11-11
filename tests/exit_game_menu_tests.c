/* The real menus: each prepared build's Stated bytecode (game\prepared\<build>\stream.dat) is patched and its Options and
 * Select Campaign code is run from the builder's entry in a main-menu state, before and after the patch. The rows the
 * menu ends up with and the guards' calls are what is checked, not the patch bytes. A build that is not prepared is
 * skipped. */
#include "exit_game_script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int failures, checked;
#define CHECK(condition) do { if (!(condition)) { printf("FAILED: %s (line %d)\n", #condition, __LINE__); ++failures; } } while (0)

enum { FIELD_ID = 0x39f744e4u, FIELD_TEXT = 0xfc6fa961u, FIELD_BOX = 0xe5e534ebu, FIELD_MESSAGE_ITEM = 0x94ba2d0au,
       VAR_CHOSEN = 0x4157579eu, VAR_OPTIONS_LIST = 0x6998b048u, VAR_PLAYER = 0x5b5eb3f3u };

static uint32_t get32(const uint8_t *at) {
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static uint16_t get16(const uint8_t *at) {
    return (uint16_t)(at[0] | at[1] << 8);
}

typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t base;
} Block;

static const char *string_of(const Block *b, uint32_t index) {
    const uint32_t table = get32(b->data + 0x20) + 4u, count = get32(b->data + 0x24);
    if (index >= count) return "";
    return (const char *)b->data + get32(b->data + table + index * 8u);
}

static const char *native_of(const Block *b, uint32_t index) {
    const uint32_t table = get32(b->data + 0x0c), count = get32(b->data + 0x10);
    return index < count ? string_of(b, get16(b->data + table + index * 4u)) : "";
}

static int string_index(const Block *b, const char *text) {
    for (uint32_t i = 0, n = get32(b->data + 0x24); i < n; ++i)
        if (!strcmp(string_of(b, i), text)) return (int)i;
    return -1;
}

static int native_index(const Block *b, const char *name) {
    for (uint32_t i = 0, n = get32(b->data + 0x10); i < n; ++i)
        if (!strcmp(native_of(b, i), name)) return (int)i;
    return -1;
}

/* ---- a Stated machine for the instructions these menus use ---- */
enum { V_NIL, V_INT, V_STR, V_TABLE, V_LIST, V_RET, V_OBJ };
typedef struct { int kind, v; } Val;
typedef struct { uint32_t hash; Val value; } Slot;
enum { STACK = 96, TABLES = 48, SLOTS = 12, LISTS = 8, LIST_ITEMS = 24, VARS = 32 };
typedef struct {
    const Block *b;
    Val stack[STACK];
    int depth;
    Slot tables[TABLES][SLOTS];
    int fields[TABLES], tables_used;
    Val lists[LISTS][LIST_ITEMS];
    int list_len[LISTS], lists_used;
    Slot vars[VARS];
    int var_count, exit_calls;
    const char *error;
} Vm;

static void fail(Vm *m, const char *why) {
    if (!m->error) m->error = why;
}

static void push(Vm *m, Val v) {
    if (m->depth >= STACK) return fail(m, "stack overflow");
    m->stack[m->depth++] = v;
}

static Val pop(Vm *m) {
    if (m->depth <= 0) {
        fail(m, "stack underflow");
        return (Val){V_NIL, 0};
    }
    return m->stack[--m->depth];
}

static Val *slot(Slot *slots, int *count, int limit, uint32_t hash, int create) {
    for (int i = 0; i < *count; ++i)
        if (slots[i].hash == hash) return &slots[i].value;
    if (!create || *count >= limit) return NULL;
    slots[*count].hash = hash;
    slots[*count].value = (Val){V_NIL, 0};
    return &slots[(*count)++].value;
}

static Val *field(Vm *m, Val table, uint32_t hash, int create) {
    if (table.kind != V_TABLE) {
        fail(m, "field of a non-table");
        return NULL;
    }
    return slot(m->tables[table.v], &m->fields[table.v], SLOTS, hash, create);
}

static void set_var(Vm *m, uint32_t hash, Val v) {
    Val *at = slot(m->vars, &m->var_count, VARS, hash, 1);
    if (at) *at = v;
}

static Val get_var(Vm *m, uint32_t hash) {
    const Val *at = slot(m->vars, &m->var_count, VARS, hash, 0);
    return at ? *at : (Val){V_NIL, 0};
}

static int truthy(Val v) {
    return v.kind != V_NIL && !(v.kind == V_INT && v.v == 0);
}

static int same(Val a, Val b) {
    return a.kind == b.kind && a.v == b.v;
}

static Val new_list(Vm *m) {
    if (m->lists_used >= LISTS) {
        fail(m, "too many lists");
        return (Val){V_NIL, 0};
    }
    m->list_len[m->lists_used] = 0;
    return (Val){V_LIST, m->lists_used++};
}

static void native(Vm *m, const char *name) {
    if (!strcmp(name, "list_new")) {
        push(m, new_list(m));
    } else if (!strcmp(name, "list_add")) {
        const Val v = pop(m), list = pop(m);
        if (list.kind != V_LIST || m->list_len[list.v] >= LIST_ITEMS) return fail(m, "list_add on a non-list");
        m->lists[list.v][m->list_len[list.v]++] = v;
    } else if (!strcmp(name, "list_insert")) {
        const Val v = pop(m), index = pop(m), list = pop(m);
        if (list.kind != V_LIST || index.kind != V_INT) return fail(m, "list_insert arguments");
        const int n = m->list_len[list.v];
        if (index.v < 0 || index.v > n || n >= LIST_ITEMS) return fail(m, "list_insert index out of range");
        memmove(&m->lists[list.v][index.v + 1], &m->lists[list.v][index.v], (size_t)(n - index.v) * sizeof(Val));
        m->lists[list.v][index.v] = v;
        ++m->list_len[list.v];
    } else if (!strcmp(name, "list_reverse")) {
        const Val list = pop(m);
        if (list.kind != V_LIST) return fail(m, "list_reverse on a non-list");
        for (int i = 0, j = m->list_len[list.v] - 1; i < j; ++i, --j) {
            const Val t = m->lists[list.v][i];
            m->lists[list.v][i] = m->lists[list.v][j];
            m->lists[list.v][j] = t;
        }
    } else if (!strcmp(name, EXIT_GAME_MARKER)) {
        ++m->exit_calls;
    } else {
        static char why[96];
        snprintf(why, sizeof(why), "unexpected native %s", name);
        fail(m, why);
    }
}

/* Runs from `pc` until one of `stops` is reached; returns the stop, or 0 on an error or after too many steps. */
static uint32_t run(Vm *m, uint32_t pc, const uint32_t *stops, size_t stop_count) {
    const uint8_t *d = m->b->data;
    for (int steps = 0; steps < 4000 && !m->error; ++steps) {
        for (size_t i = 0; i < stop_count; ++i)
            if (pc == stops[i]) return pc;
        if (pc + 5 > m->b->size) return fail(m, "pc outside the block"), 0;
        const uint8_t *op = d + pc;
        const uint32_t target = m->b->base + get32(op + 1);
        switch (*op) {
        case 0x00: pc += 2; break;
        case 0x06: push(m, (Val){V_INT, (int8_t)op[1]}); pc += 2; break;
        case 0x08: push(m, (Val){V_STR, get16(op + 1)}); pc += 3; break;
        case 0x09: push(m, (Val){V_RET, (int)target}); pc += 5; break;
        case 0x0a: for (int i = 0; i < op[1]; ++i) pop(m); push(m, (Val){V_STR, -1}); pc += 2; break;
        case 0x0b:
            if (op[1] >= m->depth) return fail(m, "pick below the stack"), 0;
            push(m, m->stack[m->depth - 1 - op[1]]);
            pc += 2;
            break;
        case 0x0c: {
            const int drop = op[1], keep = op[2];
            if (drop + keep > m->depth) return fail(m, "drop below the stack"), 0;
            memmove(&m->stack[m->depth - drop - keep], &m->stack[m->depth - keep], (size_t)keep * sizeof(Val));
            m->depth -= drop;
            pc += 3;
            break;
        }
        case 0x0d: {
            if (op[1] >= m->depth) return fail(m, "swap below the stack"), 0;
            const Val t = m->stack[m->depth - 1];
            m->stack[m->depth - 1] = m->stack[m->depth - 1 - op[1]];
            m->stack[m->depth - 1 - op[1]] = t;
            pc += 2;
            break;
        }
        case 0x0f: m->depth -= op[1]; pc += 2; break;
        case 0x11: {
            const Val *v = field(m, pop(m), get32(op + 1), 0);
            push(m, v ? *v : (Val){V_NIL, 0});
            pc += 5;
            break;
        }
        case 0x13:
            if (m->tables_used >= TABLES) return fail(m, "too many tables"), 0;
            m->fields[m->tables_used] = 0;
            push(m, (Val){V_TABLE, m->tables_used++});
            pc += 1;
            break;
        case 0x14: {
            const Val target_table = pop(m), value = pop(m);
            Val *v = field(m, target_table, get32(op + 1), 1);
            if (v) *v = value;
            pc += 5;
            break;
        }
        case 0x16: push(m, get_var(m, get32(op + 1))); pc += 5; break;
        case 0x17:
        case 0x18: push(m, get_var(m, get32(op + 1))); pc += 5; break;
        case 0x19: set_var(m, get32(op + 1), pop(m)); pc += 5; break;
        case 0x1b:
            /* The localize call: `push return; swap 1; swap 2; swap 1; jump`: it leaves the text and returns. */
            if (pc >= 6 && !memcmp(op - 6, "\x0d\x01\x0d\x02\x0d\x01", 6)) {
                const Val back = pop(m), text = pop(m);
                pop(m);
                if (back.kind != V_RET) return fail(m, "localize without a return address"), 0;
                push(m, (Val){V_STR, text.v});
                pc = (uint32_t)back.v;
            } else {
                pc = target;
            }
            break;
        case 0x1c: pc = truthy(pop(m)) ? target : pc + 5; break;
        case 0x1d: pc = truthy(pop(m)) ? pc + 5 : target; break;
        case 0x1f: native(m, native_of(m->b, get16(op + 1))); pc += 3; break;
        case 0x2c: { const Val b = pop(m), a = pop(m); push(m, (Val){V_INT, same(a, b)}); pc += 1; break; }
        case 0x2d: { const Val b = pop(m), a = pop(m); push(m, (Val){V_INT, !same(a, b)}); pc += 1; break; }
        case 0x3f: {
            const Val list = new_list(m);
            const int n = op[1];
            if (list.kind != V_LIST || n > m->depth) return fail(m, "list literal"), 0;
            memcpy(m->lists[list.v], &m->stack[m->depth - n], (size_t)n * sizeof(Val));
            m->list_len[list.v] = n;
            m->depth -= n;
            push(m, list);
            pc += 2;
            break;
        }
        default: {
            static char why[64];
            snprintf(why, sizeof(why), "unhandled op %02x at %#x", *op, pc);
            return fail(m, why), 0;
        }
        }
    }
    return fail(m, "no stop reached"), 0;
}

static Vm *new_vm(const Block *b) {
    Vm *m = calloc(1, sizeof(Vm));
    m->b = b;
    push(m, (Val){V_OBJ, 0});
    push(m, (Val){V_OBJ, 1});
    return m;
}

static const char *id_of(Vm *m, Val item) {
    const Val *id = item.kind == V_TABLE ? field(m, item, FIELD_ID, 0) : NULL;
    return id && id->kind == V_STR ? string_of(m->b, (uint32_t)id->v) : "?";
}

static int load_block(const char *build, const ExitGameScriptSite *site, Block *b) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/game/prepared/%s/stream.dat", ALN_SOURCE_DIR, build);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t header[0x28];
    uint8_t *z = malloc(site->zsize);
    int ok = !fseek(f, (long)site->block, SEEK_SET) && fread(header, 1, sizeof(header), f) == sizeof(header) &&
             get32(header + 4) == site->zsize && fread(z, 1, site->zsize, f) == site->zsize;
    fclose(f);
    b->size = site->dsize + exit_game_script_extra(site);
    b->data = calloc(1, b->size);
    uLongf out = site->dsize;
    ok = ok && uncompress(b->data, &out, z, site->zsize) == Z_OK && out == site->dsize;
    free(z);
    b->base = get32(b->data + 0x14);
    return ok;
}

/* The Options builder: after `string "options_menu"; native q_single_v3; pick 0; native is_reference; jump-if-true`. */
static int options_entries(const Block *b, uint32_t *entries, int max) {
    const int name = string_index(b, "options_menu"), query = native_index(b, "q_single_v3"),
              reference = native_index(b, "is_reference");
    uint8_t pattern[12] = {0x08, (uint8_t)name, (uint8_t)(name >> 8), 0x1f, (uint8_t)query, (uint8_t)(query >> 8),
                           0x0b, 0x00, 0x1f, (uint8_t)reference, (uint8_t)(reference >> 8), 0x1c};
    int found = 0;
    for (size_t at = 0; at + sizeof(pattern) + 4 < b->size && found < max; ++at)
        if (b->data[at] == 0x08 && !memcmp(b->data + at, pattern, sizeof(pattern))) entries[found++] = (uint32_t)at + 16;
    return found;
}

typedef struct {
    int rows;
    const char *ids[LIST_ITEMS];
} Rows;

/* Runs every candidate builder entry; the one that reaches the Credits continuation is the Options page. */
static int options_rows(const Block *b, const ExitGameScriptSite *site, Rows *rows, const char **error) {
    uint32_t entries[16];
    const int count = options_entries(b, entries, 16);
    const uint32_t stop = site->item + 5;
    for (int i = 0; i < count; ++i) {
        Vm *m = new_vm(b);
        set_var(m, VAR_PLAYER, (Val){V_INT, 0});
        if (run(m, entries[i], &stop, 1) == stop) {
            const Val list = get_var(m, VAR_OPTIONS_LIST);
            rows->rows = list.kind == V_LIST ? m->list_len[list.v] : -1;
            for (int r = 0; r < rows->rows; ++r) rows->ids[r] = id_of(m, m->lists[list.v][r]);
            free(m);
            return 1;
        }
        if (m->error) *error = m->error;
        free(m);
    }
    return 0;
}

static void test_options(const char *build, const Block *original, const Block *patched, const ExitGameScriptSite *site) {
    Rows before = {0}, after = {0};
    const char *error = "no builder entry";
    CHECK(options_rows(original, site, &before, &error));
    CHECK(options_rows(patched, site, &after, &error));
    if (!before.rows || !after.rows) {
        printf("%s Options: %s\n", build, error);
        return;
    }
    printf("%s Options:", build);
    for (int r = 0; r < after.rows; ++r) printf(" %s", after.ids[r]);
    printf("\n");
    CHECK(before.rows == 5 && after.rows == 6);
    for (int r = 0; r < before.rows && r < after.rows; ++r) CHECK(!strcmp(before.ids[r], after.ids[r]));
    CHECK(!strcmp(before.ids[before.rows - 1], "credits"));
    CHECK(after.rows == 6 && !strcmp(after.ids[5], EXIT_GAME_MARKER));
}

static void test_options_guard(const Block *patched, const ExitGameScriptSite *site) {
    const struct { const char *chosen; int exits; uint32_t stop; } cases[] = {
        {EXIT_GAME_MARKER, 1, site->fail}, {"credits", 0, site->guard + 5}, {"audio", 0, site->fail}};
    const uint32_t stops[] = {site->fail, site->guard + 5};
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        Vm *m = new_vm(patched);
        set_var(m, VAR_CHOSEN, (Val){V_STR, string_index(patched, cases[i].chosen)});
        CHECK(patched->data[site->guard - 11] == 0x00);
        CHECK(run(m, site->guard - 11, stops, 2) == cases[i].stop && m->exit_calls == cases[i].exits);
        if (m->error) printf("  guard %s: %s\n", cases[i].chosen, m->error);
        free(m);
    }
}

/* Select Campaign: from the New item to the `list 2` continuation, then its menu_select guard. */
static void test_campaign(const char *build, const Block *original, const Block *patched,
                          const ExitGameScriptSite *site) {
    const int new_id = string_index(original, "new");
    uint8_t start[4] = {0x13, 0x08, (uint8_t)new_id, (uint8_t)(new_id >> 8)};
    uint32_t entry = 0;
    for (uint32_t at = site->campaign_list - 0x3f - 0x60; at < site->campaign_list - 0x3f; ++at)
        if (!memcmp(original->data + at, start, sizeof(start))) entry = at;
    CHECK(entry != 0);
    if (!entry) return;
    const uint32_t stop = site->campaign_list + 7;
    for (int pass = 0; pass < 2; ++pass) {
        const Block *b = pass ? patched : original;
        Vm *m = new_vm(b);
        CHECK(run(m, entry, &stop, 1) == stop);
        const Val list = m->depth >= 2 ? m->stack[m->depth - 2] : (Val){V_NIL, 0};
        const int rows = list.kind == V_LIST ? m->list_len[list.v] : -1;
        CHECK(rows == 2 + pass);
        if (rows == 2 + pass) {
            CHECK(!strcmp(id_of(m, m->lists[list.v][0]), "new") && !strcmp(id_of(m, m->lists[list.v][1]), "load"));
            if (pass) {
                const Val exit_row = m->lists[list.v][2];
                const Val *box = field(m, exit_row, FIELD_BOX, 0);
                CHECK(!strcmp(id_of(m, exit_row), EXIT_GAME_MARKER) && box && box->kind == V_STR &&
                      !strcmp(string_of(b, (uint32_t)box->v), "LOAD"));
                printf("%s Select Campaign: new load %s\n", build, id_of(m, exit_row));
            }
        }
        if (m->error) printf("%s Select Campaign: %s\n", build, m->error);
        free(m);
    }
    const char *items[] = {EXIT_GAME_MARKER, "load", "new"};
    const uint32_t back = site->campaign_guard + 7;
    for (size_t i = 0; i < 3; ++i) {
        Vm *m = new_vm(patched);
        push(m, (Val){V_TABLE, 0});
        m->tables_used = 1;
        Val *item = field(m, (Val){V_TABLE, 0}, FIELD_MESSAGE_ITEM, 1);
        *item = (Val){V_STR, string_index(patched, items[i])};
        CHECK(run(m, site->campaign_guard, &back, 1) == back && m->exit_calls == (i == 0));
        free(m);
    }
}

static void test_build(const char *build) {
    const ExitGameScriptSite *site = exit_game_script_site(build);
    CHECK(site != NULL);
    if (!site) return;
    Block original = {0}, patched = {0};
    if (!load_block(build, site, &original) || !load_block(build, site, &patched)) {
        printf("skipped %s: its prepared stream.dat is missing or not the build's\n", build);
        free(original.data);
        free(patched.data);
        return;
    }
    const char *why = "";
    CHECK(exit_game_script_patch(patched.data, patched.size, site, &why) != 0);
    if (*why) printf("%s: %s\n", build, why);
    test_options(build, &original, &patched, site);
    test_options_guard(&patched, site);
    test_campaign(build, &original, &patched, site);
    ++checked;
    free(original.data);
    free(patched.data);
}

int main(void) {
    static const char *const builds[] = {"CUSA00062-1.00", "CUSA00062-1.06", "CUSA04430-1.00"};
    for (size_t i = 0; i < sizeof(builds) / sizeof(*builds); ++i) test_build(builds[i]);
    printf("exit game menu tests: %d builds run, %d failures\n", checked, failures);
    return failures ? 1 : 0;
}
