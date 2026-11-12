/* The network status box: the states from the runtime's inputs, the answers to the script, the texts per language, the
 * hook sites, the authorization code that is never issued, and the script patch run on each prepared build's own
 * bytecode (game\prepared\<build>\stream.dat; a build that is not prepared is skipped): its colours, and the text's life
 * through the game's real tick and helper code while the game's own code clears, hides and repaints it and while the
 * dot comes and goes with the screens. */
#include "exit_game_script.h"
#include "online_status.h"
#include "online_status_script.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct {
    uint32_t key;
    uint8_t language;
    const char *text;
} OriginalText;

#include "online_status_text.inc"

static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAILED: %s (line %d)\n", #condition, __LINE__); ++failures; } } while (0)

enum { VAR_PROMPT_DELAY = 0x68687b7cu, VAR_STATUS_TAG = 0xcfa877dcu, VAR_ONLINE = 0xbfded186u, VAR_PLAYER = 0x5b5eb3f3u,
       VAR_STATUS_ICON = 0x437ff122u, VAR_STATUS_BOX = 0x3c7ca6a4u, VAR_RESPONSE_TIMEOUT = 0x5390fe42u,
       FIELD_TEXT = 0xfc6fa961u, FIELD_COLOUR = 0xfbaae205u };

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(table[i].name, nid)) return (uintptr_t)table[i].function;
    return 0;
}

static int classify(int enabled, int service, int np, int matching2, uint64_t connecting_ms) {
    const OnlineStatusInput in = {enabled, service, np, matching2, connecting_ms};
    return online_status_classify(&in);
}

static void test_states(void) {
    CHECK(classify(0, ONLINE_SERVICE_READY, 1, 1, 0) == ONLINE_STATUS_OFF);
    CHECK(classify(1, ONLINE_SERVICE_OFF, 0, 0, 0) == ONLINE_STATUS_CONNECTING);
    CHECK(classify(1, ONLINE_SERVICE_CONNECTING, 0, 0, 1000) == ONLINE_STATUS_CONNECTING);
    CHECK(classify(1, ONLINE_SERVICE_CONNECTING, 1, 1, ONLINE_STATUS_CONNECT_TIMEOUT_MS + 1) == ONLINE_STATUS_UNAVAILABLE);
    CHECK(classify(1, ONLINE_SERVICE_READY, 0, 1, 0) == ONLINE_STATUS_CONNECTING);
    CHECK(classify(1, ONLINE_SERVICE_READY, 1, 0, 0) == ONLINE_STATUS_CONNECTING);
    CHECK(classify(1, ONLINE_SERVICE_READY, 1, 1, 0) == ONLINE_STATUS_CONNECTED);
    CHECK(classify(1, ONLINE_SERVICE_FAILED, 1, 1, 0) == ONLINE_STATUS_UNAVAILABLE);
    CHECK(!strcmp(online_status_name(ONLINE_STATUS_UNAVAILABLE), "unavailable"));
}

static void test_answers(void) {
    for (int state = ONLINE_STATUS_OFF; state <= ONLINE_STATUS_UNAVAILABLE; ++state) {
        const int other = (state + 1) % (ONLINE_STATUS_UNAVAILABLE + 1);
        CHECK(online_status_answer(ONLINE_STATUS_STYLE_MAGIC, state) == state + 1);
        CHECK(online_status_answer(ONLINE_STATUS_PAINT_MAGIC, state) == state + 1);
        CHECK(online_status_answer(ONLINE_STATUS_CHANGED_MAGIC, state) == 0);
        CHECK(online_status_answer(ONLINE_STATUS_CHANGED_MAGIC, other) == 1);
        CHECK(online_status_answer(ONLINE_STATUS_PAINT_HIDDEN_MAGIC, state) == state + 1);
        CHECK(online_status_answer(ONLINE_STATUS_CHANGED_MAGIC, state) == 1);
    }
    CHECK(online_status_answer(0, ONLINE_STATUS_OFF) == -1 && online_status_answer(1, ONLINE_STATUS_OFF) == -1);
    CHECK(online_status_answer(0x414c4e45u, ONLINE_STATUS_OFF) == -1);
}

static void test_texts(void) {
    static const struct { uint32_t key; const char *original, *expected; } kCases[] = {
        {0x5fa8e43fu, "Connecting to UNX Global Matrix.", "Online Connecting"},
        {0x31bd96beu, "connecting to online services", "Online Connecting"},
        {0x6b03db9du, "Joined UNX Global Matrix.", "Online Connected"},
        {0x18f2e804u, "Cannot connect to UNX Global Matrix.", "Online Unavailable"},
        {0xdcb0d29bu, "no internet connection", "Online Off"},
        {0xa4f15affu, "you have not signed in to PSN\xe2\x84\xa0", "Online Off"},
        {0xdcb0d29bu, "Sem conex\xc3\xa3o com a internet.", "Online: desativado"},
        {0x6b03db9du, "Entrou na matriz global da UNX.", "Online: conectado"},
        {0x31bd96beu, "Verbindung mit Online-Diensten", "Online: Verbindung wird hergestellt"},
        {0x18f2e804u, "UNX\xe3\x82\xb0\xe3\x83\xad\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xab\xe3\x83\x9e\xe3\x83\x88\xe3\x83\xaa"
                      "\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xb9\xe3\x81\xab\xe6\x8e\xa5\xe7\xb6\x9a\xe3\x81\xa7\xe3\x81\x8d"
                      "\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x82\xaa\xe3\x83\xb3\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\xb3"
                      "\xef\xbc\x9a\xe5\x88\xa9\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf"},
        {0xdcb0d29bu, "no hay conexi\xc3\xb3n a internet", "online: desactivado"},
        {0x6b03db9du, "a text this table does not know", "Online Connected"},
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(*kCases); ++i) {
        const char *got = online_status_text(kCases[i].key, kCases[i].original);
        CHECK(got && !strcmp(got, kCases[i].expected));
        const uint32_t k = kCases[i].key, swapped = k >> 24 | (k >> 8 & 0xff00u) | (k << 8 & 0xff0000u) | k << 24;
        CHECK(online_status_text(swapped, kCases[i].original) == got);
        CHECK(online_status_text_kind(k) != ONLINE_TEXT_NONE);
    }
    const char *other = "Keine Internetverbindung.";
    CHECK(online_status_text(0x12345678u, other) == other);
    CHECK(online_status_text_kind(0x12345678u) == ONLINE_TEXT_NONE);
    CHECK(online_status_text(0x12345678u, NULL) == NULL);
    CHECK(!strcmp(online_status_text(0x5fa8e43fu, NULL), "Online Connecting"));
}

/* The Off text of each language, in the order of kTexts: a concise "online off / disabled", never "offline" or
 * "disconnected". */
static void test_off_wording(void) {
    enum { LANGUAGES = 18, ENGLISH_INDEX = 4 };
    static const char *const kOff[LANGUAGES] = {
        "Online: desativado", "線上：已關閉", "Online: slået fra", "Online: deaktiviert", "Online Off", "online: desactivado",
        "Verkko: pois päältä", "En ligne : désactivé", "online: disattivato", "オンライン：オフ", "온라인: 꺼짐",
        "online: desactivado", "Online: uitgeschakeld", "Online: slått av", "Online: wyłączone", "Online: desativado",
        "Онлайн: выключено", "Online: avstängt",
    };
    CHECK(!strcmp(kTexts[ENGLISH_INDEX][ONLINE_TEXT_OFFLINE], "Online Off"));
    for (int language = 0; language < LANGUAGES; ++language) {
        const char *off = kTexts[language][ONLINE_TEXT_OFFLINE];
        CHECK(!strcmp(off, kOff[language]));
        CHECK(!strstr(off, "ffline") && !strstr(off, "isconnect"));
        for (int kind = 0; kind < ONLINE_TEXT_OFFLINE; ++kind) CHECK(strcmp(off, kTexts[language][kind]));
    }
    int seen[LANGUAGES] = {0};
    for (size_t i = 0; i < sizeof(kOriginals) / sizeof(*kOriginals); ++i) {
        if (kOriginals[i].key != 0xdcb0d29bu && kOriginals[i].key != 0xa4f15affu) continue;
        const char *got = online_status_text(kOriginals[i].key, kOriginals[i].text);
        CHECK(kOriginals[i].language < LANGUAGES && got && !strcmp(got, kOff[kOriginals[i].language]));
        if (kOriginals[i].language < LANGUAGES) seen[kOriginals[i].language] = 1;
    }
    enum { SPANISH = 5, SPANISH_LATAM = 11 };
    CHECK(!strcmp(kOff[SPANISH], kOff[SPANISH_LATAM]));
    seen[SPANISH_LATAM] |= seen[SPANISH]; /* the same original texts, listed once */
    for (int language = 0; language < LANGUAGES; ++language) CHECK(seen[language]);
}

static void test_sites(void) {
    static const char *const kBuilds[] = {"CUSA00062-1.00", "CUSA00062-1.06", "CUSA04430-1.00"};
    for (size_t i = 0; i < 3; ++i) CHECK(online_status_site(kBuilds[i]) != NULL);
    CHECK(online_status_site("CUSA00001-1.00") == NULL && online_status_site(NULL) == NULL);
    static uint8_t blank[0x300000];
    const char *why = "";
    CHECK(!online_status_verify(blank, sizeof(blank), online_status_site(kBuilds[0]), &why) && why[0]);
    CHECK(online_status_stub_bytes() == 3 * guest_hook_stub_size());
}

/* The UNX Global Matrix login's authorization code is never issued: the status machine never dials the dead server. */
typedef int32_t (ABI *CreateFn)(const void *);
typedef int32_t (ABI *CodeFn)(int32_t, const void *, char *, int32_t *);
typedef int32_t (ABI *PollFn)(int32_t, int32_t *);
static void test_auth_code(void) {
    runtime_np_configure(1, 0x123456789ull, "Alpha", 0x7f000001u);
    const CreateFn create = (CreateFn)runtime_np_resolve("sceNpAuthCreateAsyncRequest");
    const CodeFn code = (CodeFn)runtime_np_resolve("sceNpAuthGetAuthorizationCode");
    const PollFn poll = (PollFn)runtime_np_resolve("sceNpAuthPollAsync");
    CHECK(create && code && poll);
    if (!create || !code || !poll) return;
    const int32_t id = create(NULL);
    char text[0x80] = "";
    int32_t issuer = 0, result = -1;
    CHECK(id > 0 && code(id, NULL, text, &issuer) == 0);
    for (int i = 0; i < 3; ++i) CHECK(poll(id, &result) == 1);
    CHECK(!runtime_np_signed_in());
    runtime_np_configure(0, 0, NULL, 0);
}

/* ---- the game's bytecode, run in a model of its VM and of the status box ---- */
typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t base;
} Block;

static uint32_t get32(const uint8_t *at) {
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static const char *string_of(const Block *b, uint32_t index) {
    const uint32_t table = get32(b->data + 0x20) + 4u, count = get32(b->data + 0x24);
    return index < count ? (const char *)b->data + get32(b->data + table + index * 8u) : "";
}

static const char *native_of(const Block *b, uint32_t index) {
    const uint32_t table = get32(b->data + 0x0c), count = get32(b->data + 0x10);
    return index < count ? string_of(b, (uint16_t)(b->data[table + index * 4u] | b->data[table + index * 4u + 1] << 8)) : "";
}

static int native_index_of(const Block *b, const char *name) {
    for (uint32_t i = 0, count = get32(b->data + 0x10); i < count; ++i)
        if (!strcmp(native_of(b, i), name)) return (int)i;
    return -1;
}

static int string_index_of(const Block *b, const char *text) {
    for (uint32_t i = 0, n = get32(b->data + 0x24); i < n; ++i)
        if (!strcmp(string_of(b, i), text)) return (int)i;
    return -1;
}

/* In the original code: the places the tests start from or stand in for. */
typedef struct {
    uint32_t helper, localize, hud;  /* the status helper, its tag localization and its menu hud lookup */
    uint32_t tick;                   /* the per-tick handler right after its dot update: `pop 1; status box set?` */
    uint32_t connected, connected_return; /* the CONNECTED state's entry paint */
    uint32_t lb_clear, fade_clear;   /* the LB state's entry and the CONNECTED fade: clear the text box and its tag */
    uint32_t hides[2];               /* the server error and disconnect paths: layout_box_set_visible(text box, false) */
} Places;

enum { V_NIL, V_BOOL, V_INT, V_STR, V_RET, V_TABLE, V_FLOAT, V_COLOUR, V_LOC, V_BOX, V_HUD, V_OBJECT, V_EMPTY };
typedef struct { int kind; int64_t v; } Val;
typedef struct { int alive, text, text_visible, visible; float colour[3]; } TextBox;
enum { MAX_STACK = 64, MAX_BOXES = 48, MAX_TABLES = 16, MAX_COLOURS = 32, MAX_EVENTS = 32 };
enum { EV_NEW = 1, EV_TEXT, EV_COLOUR, EV_SHOW, EV_HIDE };

typedef struct {
    const Block *b;
    const Places *at;
    Val stack[MAX_STACK];
    int depth;
    int state, player, icon, box, tag, prompt_delay, hud_ready, online;
    TextBox boxes[MAX_BOXES];
    int boxes_used, tables[MAX_TABLES][2], tables_used, colours_used;
    float colours[MAX_COLOURS][3];
    int helper_calls, text_sets, event_count;
    int events[MAX_EVENTS]; /* what the code did to the text box, in order */
    const char *error;
} Vm;

static void push(Vm *m, int kind, int64_t v) {
    if (m->depth >= MAX_STACK) m->error = "stack overflow";
    else m->stack[m->depth++] = (Val){kind, v};
}

static Val pop(Vm *m) {
    if (!m->depth) {
        m->error = "stack underflow";
        return (Val){V_NIL, 0};
    }
    return m->stack[--m->depth];
}

static int truth(Val v) {
    return v.kind == V_BOOL ? (int)v.v : v.kind != V_NIL;
}

static Val var_of(const Vm *m, uint32_t name) {
    switch (name) {
    case VAR_STATUS_BOX: return m->box ? (Val){V_BOX, m->box - 1} : (Val){V_NIL, 0};
    case VAR_STATUS_TAG: return m->tag >= 0 ? (Val){V_STR, m->tag} : (Val){V_NIL, 0};
    case VAR_STATUS_ICON: return m->icon ? (Val){V_OBJECT, 0} : (Val){V_NIL, 0};
    case VAR_PROMPT_DELAY: return m->prompt_delay ? (Val){V_INT, 7} : (Val){V_NIL, 0};
    case VAR_PLAYER: return (Val){V_INT, m->player};
    case VAR_ONLINE: return (Val){V_BOOL, m->online};
    default: return (Val){V_OBJECT, 0};
    }
}

/* A layout box goes when the machine drops its only reference to it. */
static void set_var(Vm *m, uint32_t name, Val v) {
    if (name == VAR_STATUS_BOX) {
        if (m->box && (v.kind != V_BOX || v.v != m->box - 1)) m->boxes[m->box - 1].alive = 0;
        m->box = v.kind == V_BOX ? (int)v.v + 1 : 0;
    } else if (name == VAR_STATUS_TAG) {
        m->tag = v.kind == V_STR ? (int)v.v : -1;
    } else if (name == VAR_PROMPT_DELAY) {
        m->prompt_delay = v.kind != V_NIL;
    } else if (name == VAR_STATUS_ICON) {
        m->icon = v.kind != V_NIL;
    }
}

static void event(Vm *m, int kind) {
    if (m->event_count < MAX_EVENTS) m->events[m->event_count++] = kind;
}

static TextBox *box_arg(Vm *m, Val box) {
    return box.kind == V_BOX && box.v < m->boxes_used && m->boxes[box.v].alive ? &m->boxes[box.v] : NULL;
}

/* Natives whose registered body is a bare `ret` (CUSA00062 1.06, the same in the other builds): the game turns calls to
 * them into intrinsics only inside the block's code range. Called from elsewhere they consume their arguments and leave
 * an empty result, which the natives that take it reject, stopping the handler. */
static const char *const kStubNatives[] = {"vec4", "vec3", "vec2", "list_add", "list_get", "list_set", "list_new",
                                           "list_size", "hide_type_from_compiler", "is_reference", "q_single_v2",
                                           "q_single_v3", "delta_t", "time_in_state", "not", "abs"};

static int stub_native(const char *name) {
    for (size_t i = 0; i < sizeof(kStubNatives) / sizeof(*kStubNatives); ++i)
        if (!strcmp(name, kStubNatives[i])) return 1;
    return 0;
}

static uint32_t code_end(const Block *b) {
    return b->base + get32(b->data + 0x18);
}

static void native(Vm *m, uint32_t index, uint32_t pc) {
    const char *name = native_of(m->b, index);
    if (pc >= code_end(m->b) && stub_native(name)) {
        const uint32_t table = get32(m->b->data + 0x0c);
        for (int i = 0; i < m->b->data[table + index * 4u + 2]; ++i) pop(m);
        for (int i = 0; i < m->b->data[table + index * 4u + 3]; ++i) push(m, V_EMPTY, 0);
        return;
    }
    const uint32_t imports = get32(m->b->data + 0x0c);
    for (int i = 0; i < m->depth && i < m->b->data[imports + index * 4u + 2]; ++i)
        if (m->stack[m->depth - 1 - i].kind == V_EMPTY) {
            m->error = "a native was given the empty result of a stub native";
            return;
        }
    if (!strcmp(name, "q_single_v1")) {
        pop(m), pop(m);
        push(m, V_OBJECT, 0);
    } else if (!strcmp(name, "q_single_v2")) {
        for (int i = 0; i < 4; ++i) pop(m);
        push(m, V_NIL, 0);
    } else if (!strcmp(name, "is_reference")) {
        const int kind = pop(m).kind;
        push(m, V_BOOL, kind == V_OBJECT || kind == V_TABLE || kind == V_BOX);
    } else if (!strcmp(name, "is_string")) {
        push(m, V_BOOL, pop(m).kind == V_STR);
    } else if (!strcmp(name, "hide_type_from_compiler")) {
        const Val v = pop(m);
        push(m, v.kind, v.v);
    } else if (!strcmp(name, "is_custom_typed")) {
        push(m, V_BOOL, pop(m).kind == V_HUD);
    } else if (!strcmp(name, "vec4")) {
        const Val a = pop(m), b = pop(m), g = pop(m), r = pop(m), parts[3] = {r, g, b};
        if (a.kind != V_INT || a.v != 1) m->error = "alpha";
        if (m->colours_used == MAX_COLOURS) m->error = "colours";
        if (m->error) return;
        for (int i = 0; i < 3; ++i) {
            const uint32_t bits = (uint32_t)parts[i].v;
            if (parts[i].kind == V_FLOAT) memcpy(&m->colours[m->colours_used][i], &bits, 4);
            else m->colours[m->colours_used][i] = (float)parts[i].v;
        }
        push(m, V_COLOUR, m->colours_used++);
    } else if (!strcmp(name, "layout_box_set_text")) {
        const Val text = pop(m);
        TextBox *box = box_arg(m, pop(m));
        if (box) box->text = text.kind == V_LOC ? (int)text.v : -2;
        if (box) event(m, EV_TEXT);
        ++m->text_sets;
    } else if (!strcmp(name, "layout_box_set_text_color")) {
        const Val colour = pop(m);
        TextBox *box = box_arg(m, pop(m));
        if (box && colour.kind == V_COLOUR) memcpy(box->colour, m->colours[colour.v], sizeof(box->colour));
        if (box) event(m, EV_COLOUR);
    } else if (!strcmp(name, "layout_box_set_text_visible") || !strcmp(name, "layout_box_set_visible")) {
        const int shown = truth(pop(m));
        TextBox *box = box_arg(m, pop(m));
        if (box && name[15] == 't') box->text_visible = shown, event(m, shown ? EV_SHOW : EV_HIDE);
        else if (box) box->visible = shown;
    } else if (!strcmp(name, "layout_box_new")) {
        const Val table = pop(m), box = pop(m), layout = pop(m);
        if (table.kind != V_TABLE || box.kind != V_STR || layout.kind != V_STR || m->boxes_used == MAX_BOXES ||
            strcmp(string_of(m->b, (uint32_t)box.v), "Network_Status") || strcmp(string_of(m->b, (uint32_t)layout.v), "Menu_Main")) {
            m->error = "layout_box_new";
            return;
        }
        if (m->tables[table.v][1] == -2) {
            m->error = "layout_box_new was given the empty colour of a stub vec4";
            return;
        }
        TextBox *made = &m->boxes[m->boxes_used];
        made->alive = made->text_visible = made->visible = 1;
        made->text = m->tables[table.v][0];
        if (m->tables[table.v][1] >= 0) memcpy(made->colour, m->colours[m->tables[table.v][1]], sizeof(made->colour));
        event(m, EV_NEW);
        push(m, V_BOX, m->boxes_used++);
    } else if (!strcmp(name, "server_request_auth_code")) {
        const Val arg = pop(m);
        const int answer = arg.kind == V_INT ? online_status_answer((uint32_t)arg.v, m->state) : -1;
        push(m, V_INT, answer < 0 ? 0 : answer);
    } else {
        m->error = name;
    }
}

/* Runs the code at pc until one of `stops` or a `return`, and returns where it stopped. The localization and the menu hud
 * lookup are played: a tag localizes to itself, the hud is there while hud_ready. */
static uint32_t run(Vm *m, uint32_t pc, const uint32_t *stops, size_t stop_count) {
    const uint8_t *c = m->b->data;
    m->colours_used = m->tables_used = 0;
    for (int steps = 0; !m->error && steps < 4000; ++steps) {
        for (size_t i = 0; i < stop_count; ++i)
            if (pc == stops[i]) return pc;
        if (m->at && pc == m->at->localize) {
            const Val ret = pop(m), tag = pop(m);
            if (ret.kind != V_RET || tag.kind != V_STR) m->error = "localize";
            push(m, V_LOC, tag.v);
            pc = (uint32_t)ret.v;
            continue;
        }
        if (m->at && pc == m->at->hud) {
            const Val ret = pop(m);
            if (ret.kind != V_RET) m->error = "hud";
            push(m, m->hud_ready ? V_HUD : V_NIL, 0);
            pc = (uint32_t)ret.v;
            continue;
        }
        if (m->at && pc == m->at->helper) ++m->helper_calls;
        const uint8_t op = c[pc];
        const uint32_t u32 = get32(c + pc + 1);
        switch (op) {
        case 0x00: pc += 2; break;
        case 0x01: push(m, V_NIL, 0); pc += 1; break;
        case 0x03: push(m, V_BOOL, 0); pc += 1; break;
        case 0x04: push(m, V_BOOL, 1); pc += 1; break;
        case 0x05: push(m, V_FLOAT, (int64_t)u32); pc += 5; break;
        case 0x06: push(m, V_INT, c[pc + 1]); pc += 2; break;
        case 0x07: push(m, V_INT, (int64_t)u32); pc += 5; break;
        case 0x08: push(m, V_STR, c[pc + 1] | c[pc + 2] << 8); pc += 3; break;
        case 0x09: push(m, V_RET, m->b->base + u32); pc += 5; break;
        case 0x0b:
            if (c[pc + 1] < m->depth) push(m, m->stack[m->depth - 1 - c[pc + 1]].kind, m->stack[m->depth - 1 - c[pc + 1]].v);
            else m->error = "pick";
            pc += 2;
            break;
        case 0x0c: {
            const int below = c[pc + 1], kept = c[pc + 2];
            if (m->depth < below + kept) m->error = "return values";
            else {
                memmove(m->stack + m->depth - below - kept, m->stack + m->depth - kept, (size_t)kept * sizeof(Val));
                m->depth -= below;
            }
            pc += 3;
            break;
        }
        case 0x0f: for (int i = 0; i < c[pc + 1]; ++i) pop(m); pc += 2; break;
        case 0x13:
            if (m->tables_used == MAX_TABLES) m->error = "tables";
            else {
                m->tables[m->tables_used][0] = m->tables[m->tables_used][1] = -1;
                push(m, V_TABLE, m->tables_used++);
            }
            pc += 1;
            break;
        case 0x14: {
            const Val table = pop(m), v = pop(m);
            if (table.kind != V_TABLE) m->error = "set field";
            else if (u32 == FIELD_TEXT && v.kind == V_LOC) m->tables[table.v][0] = (int)v.v;
            else if (u32 == FIELD_COLOUR && v.kind == V_COLOUR) m->tables[table.v][1] = (int)v.v;
            else if (u32 == FIELD_COLOUR && v.kind == V_EMPTY) m->tables[table.v][1] = -2;
            pc += 5;
            break;
        }
        case 0x16: push(m, V_BOOL, var_of(m, u32).kind != V_NIL && (u32 != VAR_ONLINE || m->online)); pc += 5; break;
        case 0x17: case 0x18: { const Val v = var_of(m, u32); push(m, v.kind, v.v); pc += 5; break; }
        case 0x19: set_var(m, u32, pop(m)); pc += 5; break;
        case 0x1a: set_var(m, u32, (Val){V_NIL, 0}); pc += 5; break;
        case 0x1b: pc = m->b->base + u32; break;
        case 0x1c: case 0x1d: pc = truth(pop(m)) == (op == 0x1c) ? m->b->base + u32 : pc + 5; break;
        case 0x1e: {
            const Val ret = pop(m);
            if (ret.kind != V_RET) m->error = "ret";
            pc = (uint32_t)ret.v;
            break;
        }
        case 0x1f: native(m, (uint32_t)(c[pc + 1] | c[pc + 2] << 8), pc); pc += 3; break;
        case 0x2c: { const Val a = pop(m), b = pop(m); push(m, V_BOOL, a.kind == b.kind && a.v == b.v); pc += 1; break; }
        case 0x38: return pc;
        default:
            printf("model: op %#x at %#x\n", op, pc);
            m->error = "an instruction the model does not know";
        }
    }
    if (!m->error) m->error = "the code never stopped";
    return 0;
}

static int load_block(const ExitGameScriptSite *site, const char *build, Block *b, size_t extra) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/game/prepared/%s/stream.dat", ALN_SOURCE_DIR, build);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t *z = malloc(site->zsize);
    b->size = site->dsize;
    b->data = calloc(1, site->dsize + extra);
    uLongf size = site->dsize;
    const int ok = z && b->data && !fseek(f, (long)site->block + 0x28, SEEK_SET) &&
                   fread(z, 1, site->zsize, f) == site->zsize &&
                   uncompress(b->data, &size, z, site->zsize) == Z_OK && size == site->dsize;
    fclose(f);
    free(z);
    b->base = get32(b->data + 0x14);
    return ok;
}

static size_t deflated_size(const uint8_t *data, size_t size) {
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (deflateInit2(&s, 9, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return (size_t)-1;
    uLong bound = deflateBound(&s, (uLong)size);
    uint8_t *out = malloc(bound);
    s.next_in = (Bytef *)data, s.avail_in = (uInt)size, s.next_out = out, s.avail_out = (uInt)bound;
    const int r = deflate(&s, Z_FINISH);
    const size_t written = r == Z_STREAM_END ? s.total_out : (size_t)-1;
    deflateEnd(&s);
    free(out);
    return written;
}

/* Every offset of `pattern` (-1: any byte) from the code on. */
static size_t find_all(const Block *b, const int *pattern, size_t count, uint32_t *out, size_t max) {
    size_t found = 0;
    for (uint32_t at = b->base; at + count <= b->size; ++at) {
        size_t i = 0;
        while (i < count && (pattern[i] < 0 || b->data[at + i] == pattern[i])) ++i;
        if (i == count && found < max) out[found] = at;
        if (i == count) ++found;
    }
    return found;
}

static uint32_t find_unique(const Block *b, const int *pattern, size_t count) {
    uint32_t at = 0;
    return find_all(b, pattern, count, &at, 1) == 1 ? at : 0;
}

#define WORD(v) (int)((v) & 0xff), (int)((v) >> 8 & 0xff), (int)((v) >> 16 & 0xff), (int)((uint32_t)(v) >> 24)
#define COUNT(a) (sizeof(a) / sizeof(*(a)))

static int find_places(const Block *b, Places *p) {
    memset(p, 0, sizeof(*p));
    const int connected = string_index_of(b, "CONNECTED"), set_visible = native_index_of(b, "layout_box_set_visible");
    const int call[] = {0x00, 0x03, 0x04, 0x08, connected & 0xff, connected >> 8, 0x09, -1, -1, -1, -1, 0x1b};
    const int tick[] = {0x00, 0x01, 0x01, 0x0f, 0x01, 0x16, WORD(VAR_STATUS_BOX), 0x1c};
    const int lb[] = {0x06, 0x01, 0x19, WORD(VAR_RESPONSE_TIMEOUT), 0x1a, WORD(VAR_STATUS_BOX), 0x1a, WORD(VAR_STATUS_TAG)};
    const int fade[] = {0x1a, WORD(VAR_STATUS_BOX), 0x1a, WORD(VAR_STATUS_TAG), 0x01, 0x0e, 0x01};
    const int hide[] = {0x17, WORD(VAR_STATUS_BOX), 0x03, 0x1f, set_visible & 0xff, set_visible >> 8};
    const int store[] = {0x00, 0x02, 0x0b, -1, 0x19, WORD(VAR_STATUS_TAG), 0x0b, -1, 0x1f, -1, -1, 0x1c};
    p->connected = find_unique(b, call, COUNT(call));
    if (!p->connected || connected < 0 || set_visible < 0) return 0;
    p->connected_return = b->base + get32(b->data + p->connected + 7);
    p->helper = b->base + get32(b->data + p->connected + 12);
    p->tick = find_unique(b, tick, COUNT(tick)) + 3;
    p->lb_clear = find_unique(b, lb, COUNT(lb)) + 7;
    p->fade_clear = find_unique(b, fade, COUNT(fade));
    if (find_all(b, hide, COUNT(hide), p->hides, 2) != 2) return 0;
    uint32_t tag_store = 0;
    for (uint32_t at = p->helper; at < p->helper + 0x60 && !tag_store; ++at) {
        size_t i = 0;
        while (i < COUNT(store) && (store[i] < 0 || b->data[at + i] == store[i])) ++i;
        if (i == COUNT(store)) tag_store = at;
    }
    if (!tag_store || p->tick == 3 || p->lb_clear == 7 || !p->fade_clear) return 0;
    const uint32_t path = b->base + get32(b->data + tag_store + 15);
    const uint32_t localized = b->base + get32(b->data + path + 8);
    p->localize = b->base + get32(b->data + path + 13);
    if (b->data[localized] != 0x00 || b->data[localized + 2] != 0x09 || b->data[localized + 7] != 0x1b) return 0;
    p->hud = b->base + get32(b->data + localized + 8);
    return 1;
}

/* ---- colours ---- */
typedef struct { uint32_t text, text_green, text_continue, dot, dot_original, dot_colour[3]; } Colours;
enum { GREEN, AMBER, RED };

static int find_colours(const Block *b, Colours *c) {
    static const int kText[] = {0x00, 0x01, 0x0b, -1, 0x1c, -1, -1, -1, -1, 0x00, 0x05, 0x05, 0x33, 0x33, 0x73, 0x3f};
    static const int kDot[] = {0x00, 0x01, 0x17, WORD(VAR_ONLINE), 0x1c, -1, -1, -1, -1, 0x00, 0x01, 0x0b, 0x00, 0x1c};
    c->text = find_unique(b, kText, COUNT(kText));
    c->dot = find_unique(b, kDot, COUNT(kDot));
    if (!c->text || !c->dot) return 0;
    c->text_green = b->base + get32(b->data + c->text + 5);
    c->text_continue = b->base + get32(b->data + c->text_green + 20);
    const uint32_t a = b->base + get32(b->data + c->dot + 8), then = b->base + get32(b->data + a + 8);
    c->dot_original = a;
    c->dot_colour[RED] = b->base + get32(b->data + c->dot + 17);
    c->dot_colour[AMBER] = a + 12;
    c->dot_colour[GREEN] = b->base + get32(b->data + then + 5);
    return 1;
}

/* The dot's own colours, which the text now takes too. */
static int rgb_is(const float *rgb, int style) {
    static const float kRgb[3][3] = {{0.5f, 1.0f, 0.5f}, {1.0f, 1.0f, 0.5f}, {1.1f, 0.3f, 0.3f}};
    return style >= 0 && rgb[0] == kRgb[style][0] && rgb[1] == kRgb[style][1] && rgb[2] == kRgb[style][2];
}

static void vm_init(Vm *m, const Block *b, const Places *at) {
    memset(m, 0, sizeof(*m));
    m->b = b;
    m->at = at;
    m->tag = -1;
    m->hud_ready = 1;
}

/* A status text takes the helper's own in-range colour until its box exists (any other text keeps the helper's choice);
 * the dot is coloured by the state the text names, or by the current one once the text is gone. */
static void test_colours(const char *build, const Block *b, const Colours *c) {
    static const struct { const char *tag; int style; } kTags[] = {
        {"CONNECTED", GREEN}, {"GENERIC_ERROR", RED}, {"LOGIN", AMBER}, {"WAITING_PSN", AMBER},
        {"NETWORK_OFFLINE", AMBER}, {"USER_OFFLINE", AMBER}, {"SERVICES_PROHIBITED", -1},
    };
    for (size_t i = 0; i < COUNT(kTags); ++i) {
        Vm text, dot;
        vm_init(&text, b, NULL), vm_init(&dot, b, NULL);
        text.tag = dot.tag = string_index_of(b, kTags[i].tag);
        CHECK(text.tag >= 0);
        for (int k = 0; k < 6; ++k) push(&text, V_BOOL, 0);
        const uint32_t text_stops[] = {c->text_continue, c->text_green, c->text + 9};
        const uint32_t reached = run(&text, c->text, text_stops, 3);
        const uint32_t dot_stops[] = {c->dot_colour[GREEN], c->dot_colour[AMBER], c->dot_colour[RED], c->dot_original, c->dot + 12};
        push(&dot, V_BOOL, 0);
        const uint32_t dot_reached = run(&dot, c->dot, dot_stops, 5);
        if (text.error || dot.error) printf("FAILED: %s %s: %s\n", build, kTags[i].tag, text.error ? text.error : dot.error), ++failures;
        if (kTags[i].style < 0) {
            CHECK(reached == c->text + 9 && text.depth == 6);
            continue;
        }
        CHECK(reached == c->text_green && text.depth == 6);
        CHECK(dot_reached == c->dot_colour[kTags[i].style] && dot.depth == 1);
    }
    static const struct { int state, style; } kStates[] = {
        {ONLINE_STATUS_CONNECTED, GREEN}, {ONLINE_STATUS_UNAVAILABLE, RED}, {ONLINE_STATUS_CONNECTING, AMBER}, {ONLINE_STATUS_OFF, AMBER},
    };
    for (size_t i = 0; i < COUNT(kStates); ++i) {
        Vm dot;
        vm_init(&dot, b, NULL);
        dot.state = kStates[i].state;
        push(&dot, V_BOOL, 0);
        const uint32_t stops[] = {c->dot_colour[GREEN], c->dot_colour[AMBER], c->dot_colour[RED], c->dot + 12};
        const uint32_t reached = run(&dot, c->dot, stops, 4);
        if (dot.error) printf("FAILED: %s state %d: %s\n", build, kStates[i].state, dot.error), ++failures;
        CHECK(reached == stops[kStates[i].style] && dot.depth == 1);
    }
}

/* ---- the text's life ---- */
static const char *const kStateTag[] = {"NETWORK_OFFLINE", "LOGIN", "CONNECTED", "GENERIC_ERROR"};
static const int kStateStyle[] = {AMBER, AMBER, GREEN, RED};

typedef struct {
    const char *build;
    int lost; /* failures */
} Life;

static void life_check(Life *life, int ok, const char *state, const char *step) {
    if (ok) return;
    printf("FAILED: %s %s: %s\n", life->build, state, step);
    ++life->lost;
}

/* One tick of the machine (player 0, after its dot update) to its return. */
static void tick(Vm *m) {
    m->helper_calls = m->text_sets = m->event_count = 0;
    push(m, V_NIL, 0);
    const uint32_t end = run(m, m->at->tick, NULL, 0);
    if (!m->error && (m->b->data[end] != 0x38 || m->depth)) m->error = "the tick did not return cleanly";
}

static void run_span(Vm *m, uint32_t from, uint32_t to) {
    run(m, from, &to, 1);
}

/* The dot is up and the text box shows the state's own text, in its colour, visible. */
static int shows(const Vm *m, int state) {
    if (m->error || !m->icon || !m->box) return 0;
    const TextBox *box = &m->boxes[m->box - 1];
    return box->alive && box->visible && box->text_visible && box->text == string_index_of(m->b, kStateTag[state]) &&
           m->tag == box->text && rgb_is(box->colour, kStateStyle[state]);
}

/* No dot: the text box is still there but hidden, and its tag is cleared so that the dot's return paints it again. */
static int hidden(const Vm *m) {
    if (m->error || m->icon || !m->box || m->tag >= 0) return 0;
    const TextBox *box = &m->boxes[m->box - 1];
    return box->alive && !box->text_visible;
}

static int alive_boxes(const Vm *m) {
    int alive = 0;
    for (int i = 0; i < m->boxes_used; ++i) alive += m->boxes[i].alive;
    return alive;
}

static int did(const Vm *m, int kind) {
    for (int i = 0; i < m->event_count; ++i)
        if (m->events[i] == kind) return 1;
    return 0;
}

static int quiet(const Vm *m) {
    return !m->error && !m->event_count && !m->helper_calls && !m->text_sets;
}

/* The game paints its text from a few of its states and clears or hides it on several paths, while its dot is updated on
 * every tick: each step below is the game's own code (or a state it reaches), and the text must come back on the next
 * tick with the dot still up, then stay without being painted again. */
static int test_life(const char *build, const Block *b, const Places *at) {
    static const char *const kNames[] = {"Offline", "Connecting", "Connected", "Unavailable"};
    Life life = {build, 0};
    for (int state = ONLINE_STATUS_OFF; state <= ONLINE_STATUS_UNAVAILABLE; ++state) {
        const char *name = kNames[state];
        Vm m;
        vm_init(&m, b, at);
        m.state = state;

        /* The game paints before the dot exists (the LB state's first paint): the text is made but not shown. */
        run_span(&m, at->connected, at->connected_return);
        pop(&m);
        life_check(&life, !m.error && m.box && !m.boxes[m.box - 1].text_visible && !did(&m, EV_SHOW), name,
                   "a paint before the dot exists is not shown");

        /* The dot appears in a tick: the text that was made without it is painted again and shown. */
        m.icon = 1;
        tick(&m);
        life_check(&life, shows(&m, state) && did(&m, EV_SHOW) && !did(&m, EV_NEW) && alive_boxes(&m) == 1, name,
                   "the dot appears: the text is shown on the same box");
        tick(&m);
        life_check(&life, shows(&m, state) && quiet(&m), name, "steady: not painted again");

        /* Select Campaign -> Options -> Select Campaign, and Options -> Select Mission, again and again. */
        for (int round = 0; round < 3; ++round) {
            m.icon = 0;
            tick(&m);
            life_check(&life, hidden(&m) && did(&m, EV_HIDE) && !did(&m, EV_NEW), name,
                       "no dot (Options): the text is hidden and its tag cleared");
            tick(&m);
            life_check(&life, hidden(&m) && quiet(&m), name, "no dot: nothing repeats");
            m.icon = 1;
            tick(&m);
            life_check(&life, shows(&m, state) && !did(&m, EV_NEW) && alive_boxes(&m) == 1 && m.boxes_used == 1, name,
                       "the dot returns: the same text box is shown again, no duplicate");
        }

        /* The game paints while there is no dot (a screen that has none): hidden, restored with the dot. */
        m.icon = 0;
        tick(&m);
        run_span(&m, at->connected, at->connected_return);
        pop(&m);
        life_check(&life, !m.error && !m.boxes[m.box - 1].text_visible, name, "no dot: the game's own paint stays hidden");
        tick(&m);
        life_check(&life, hidden(&m), name, "no dot: the game's own paint is hidden again by the owner");
        m.icon = 1;
        tick(&m);
        life_check(&life, shows(&m, state), name, "the dot returns after the game's own paint: shown");

        run_span(&m, at->lb_clear, at->lb_clear + 10);
        life_check(&life, !m.box && m.tag < 0 && m.icon, name, "the LB entry clears the text, the dot stays");
        tick(&m);
        life_check(&life, shows(&m, state) && alive_boxes(&m) == 1, name, "after the LB entry: restored, no duplicate");

        m.prompt_delay = 1;
        tick(&m);
        life_check(&life, shows(&m, state) && !m.prompt_delay && !m.helper_calls, name, "the CONNECTED fade never starts");
        run_span(&m, at->fade_clear, at->fade_clear + 10);
        tick(&m);
        life_check(&life, shows(&m, state) && alive_boxes(&m) == 1, name, "after the fade's clear: restored");

        for (int i = 0; i < 2; ++i) {
            run_span(&m, at->hides[i], at->hides[i] + 9);
            tick(&m);
            life_check(&life, shows(&m, state), name, "a server error or disconnect does not hide the text");
        }

        run_span(&m, at->connected, at->connected_return);
        pop(&m);
        tick(&m);
        life_check(&life, shows(&m, state), name, "the game's own paint (CONNECTED) shows the current state");

        m.hud_ready = 0;
        run_span(&m, at->fade_clear, at->fade_clear + 10);
        tick(&m);
        life_check(&life, !m.error && !m.box && m.icon && !did(&m, EV_NEW), name, "no menu hud: no text box");
        m.hud_ready = 1;
        tick(&m);
        life_check(&life, shows(&m, state) && alive_boxes(&m) == 1, name, "the menu hud is back: restored");

        const int next = (state + 1) % (ONLINE_STATUS_UNAVAILABLE + 1);
        m.state = next;
        tick(&m);
        life_check(&life, shows(&m, next) && m.text_sets == 1 && alive_boxes(&m) == 1, name,
                   "a new state repaints the same text box");
        m.state = state;
        tick(&m);
        life_check(&life, shows(&m, state), name, "and back");
        if (m.error) printf("FAILED: %s %s: %s\n", build, name, m.error), ++life.lost;
    }

    /* Off -> Connecting -> Connected -> Unavailable -> Connecting -> Connected -> Off. */
    static const int kWalk[] = {ONLINE_STATUS_OFF, ONLINE_STATUS_CONNECTING, ONLINE_STATUS_CONNECTED, ONLINE_STATUS_UNAVAILABLE,
                                ONLINE_STATUS_CONNECTING, ONLINE_STATUS_CONNECTED, ONLINE_STATUS_OFF};
    Vm m;
    vm_init(&m, b, at);
    m.icon = 1;
    for (size_t i = 0; i < COUNT(kWalk); ++i) {
        m.state = kWalk[i];
        tick(&m);
        life_check(&life, shows(&m, kWalk[i]) && alive_boxes(&m) == 1 && (i == 0 || m.text_sets == 1), "transition",
                   "the state changes: text and colour follow, one text box");
        tick(&m);
        life_check(&life, quiet(&m), "transition", "the state is kept: nothing is painted");
    }
    return life.lost;
}

/* Where the jump at `at` lands, or 0 when `at` is not a jump, jump-if or push-return. */
static uint32_t branch_target(const Block *b, uint32_t at) {
    const uint8_t op = b->data[at];
    return op == 0x1b || op == 0x1c || op == 0x1d || op == 0x09 ? b->base + get32(b->data + at + 1) : 0;
}

/* The owner sits on the path that keeps the dot: the dot's update function (the only maker of Menu_Main/Network_Icon,
 * where its colour is chosen) is only ever called by the tick, and every return from it goes through the owner
 * trampoline, which comes back to the tick's own code with the stack as it was, with or without a dot. */
static void test_hook(const char *build, const Block *o, const Block *b, const Places *at) {
    const uint8_t update_call[] = {0x00, 0x01, 0x16, WORD(VAR_STATUS_ICON), 0x1c};
    const uint32_t icon_test = at->tick - 0xf;
    CHECK(!memcmp(o->data + icon_test, update_call, sizeof(update_call)));
    if (memcmp(o->data + icon_test, update_call, sizeof(update_call))) return;
    const uint32_t call = branch_target(o, icon_test + 7);
    CHECK(o->data[call] == 0x00 && o->data[call + 2] == 0x09 && o->data[call + 7] == 0x1b);
    const uint32_t update = branch_target(o, call + 7);
    const int icon_name = string_index_of(o, "Network_Icon");
    const int maker[] = {0x08, icon_name & 0xff, icon_name >> 8};
    uint32_t makers[2];
    CHECK(icon_name >= 0 && find_all(o, maker, COUNT(maker), makers, 2) == 1 && makers[0] > update && makers[0] < update + 0x200);
    Colours colours;
    CHECK(find_colours(o, &colours) && colours.dot > update && colours.dot < update + 0x200);

    uint32_t returns[8];
    size_t calls = 0;
    for (uint32_t i = o->base; i + 5 <= o->size; ++i) {
        if (branch_target(o, i) != update) continue;
        const int called = o->data[i] == 0x1b && i >= o->base + 5 && o->data[i - 5] == 0x09;
        if (!called || calls == COUNT(returns)) printf("FAILED: %s: the dot update is reached at %#x not as a call\n", build, i), ++failures;
        else returns[calls++] = branch_target(o, i - 5);
    }
    CHECK(calls >= 2);
    const uint32_t site = at->tick + 2, owner = branch_target(b, site);
    CHECK(b->data[site] == 0x1b && owner >= o->size);
    for (size_t i = 0; i < calls; ++i)
        for (int dot = 0; dot < 2; ++dot) {
            Vm m;
            vm_init(&m, b, at);
            m.icon = dot;
            m.box = 0;
            push(&m, V_NIL, 0), push(&m, V_NIL, 0);
            const uint32_t end = run(&m, returns[i], NULL, 0);
            const int outcome = dot ? shows(&m, ONLINE_STATUS_OFF) : !m.boxes_used && !m.event_count;
            if (m.error || !outcome || b->data[end] != 0x38 || m.depth != 1) {
                printf("FAILED: %s: return %#x (dot %d) does not pass the owner cleanly: %s\n", build, returns[i], dot,
                       m.error ? m.error : "wrong result or stack");
                ++failures;
            }
        }
}

static void test_script(const char *build, int with_exit_game) {
    const ExitGameScriptSite *site = exit_game_script_site(build);
    Block b;
    const size_t extra = exit_game_script_extra(site) + online_status_script_extra();
    if (!site || !load_block(site, build, &b, extra)) {
        printf("skipped %s: not prepared\n", build);
        return;
    }
    const char *why = "";
    Colours colours;
    Places places;
    CHECK(find_colours(&b, &colours));
    CHECK(find_places(&b, &places));
    Block original = b;
    original.data = malloc(b.size);
    if (!original.data) return free(b.data);
    memcpy(original.data, b.data, b.size);
    size_t size = b.size;
    if (with_exit_game) size = exit_game_script_patch(b.data, b.size + extra, site, &why);
    CHECK(size);
    uint8_t *after_exit = malloc(b.size);
    if (!after_exit) return free(b.data), free(original.data);
    memcpy(after_exit, b.data, b.size);
    const size_t patched = online_status_script_patch(b.data, size, b.size + extra, &why);
    if (!patched) printf("FAILED: %s: %s\n", build, why), ++failures;
    if (!patched) return free(b.data), free(original.data), free(after_exit);
    CHECK(patched - size <= online_status_script_extra());
    int overlap = 0;
    for (size_t i = 0; i < original.size; ++i) overlap += original.data[i] != after_exit[i] && after_exit[i] != b.data[i];
    CHECK(!overlap);
    free(after_exit);
    b.size = patched;
    test_hook(build, &original, &b, &places);
    test_colours(build, &b, &colours);
    const int lost = test_life(build, &b, &places);
    failures += lost;
    const size_t units = site->units * 64u - 0x28u, compressed = deflated_size(b.data, patched);
    CHECK(compressed <= units);
    printf("%s%s: status patch %zu bytes, text life %s, block %zu of %zu bytes\n", build, with_exit_game ? " with Exit Game" : "",
           patched - size, lost ? "FAILED" : "kept", compressed, units);
    free(b.data);
    free(original.data);
}

int main(void) {
    test_states();
    test_answers();
    test_texts();
    test_off_wording();
    test_sites();
    test_auth_code();
    static const char *const builds[] = {"CUSA00062-1.00", "CUSA00062-1.06", "CUSA04430-1.00"};
    for (size_t i = 0; i < 3; ++i) {
        test_script(builds[i], 1);
        test_script(builds[i], 0);
    }
    printf("%s (%d failures)\n", failures ? "FAILED" : "online status tests passed", failures);
    return failures ? 1 : 0;
}
