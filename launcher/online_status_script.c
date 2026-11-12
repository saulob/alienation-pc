#include "online_status_script.h"

#include <string.h>

#include "online_status.h"

enum {
    OP_RESERVE = 0x00, OP_NIL = 0x01, OP_FALSE = 0x03, OP_TRUE = 0x04, OP_FLOAT = 0x05, OP_INT = 0x06, OP_INT32 = 0x07,
    OP_STRING = 0x08, OP_PUSH_RETURN = 0x09, OP_PICK = 0x0b, OP_POP = 0x0f, OP_VAR_TEST = 0x16, OP_VAR = 0x17,
    OP_VAR_OR_NIL = 0x18, OP_SET_VAR = 0x19, OP_CLEAR_VAR = 0x1a, OP_JUMP = 0x1b, OP_JUMP_IF_TRUE = 0x1c,
    OP_JUMP_IF_FALSE = 0x1d, OP_NATIVE = 0x1f, OP_EQUAL = 0x2c
};

#define BYTECODE_MAGIC 0x214e0403u
#define IMPORT_TABLE 0x0cu
#define CODE_WORD 0x14u
#define STRING_TABLE 0x20u
#define VAR_PROMPT_DELAY 0x68687b7cu
#define VAR_STATUS_BOX 0x3c7ca6a4u
#define VAR_STATUS_TAG 0xcfa877dcu
#define VAR_STATUS_ICON 0x437ff122u
#define VAR_ONLINE 0xbfded186u
#define VAR_PATCH_NOTIFIED 0x438a026eu
#define TEXT_LENGTH 9
#define TAG_STORE_LENGTH 9
#define OWNER_LENGTH 5
#define GADGET_WINDOW 32
#define DOT_LENGTH 12
#define HELPER_TAG_WINDOW 0x60
#define HELPER_GATE_WINDOW 0x60
#define HELPER_VISIBLE_WINDOW 0x1c0
#define MAX_VISIBLE 4
#define HIDES 2
#define HIDE_WINDOW 0x4000
#define VISIBLE_LENGTH 5
#define EXTRA 1024

enum { STYLE_GREEN, STYLE_AMBER, STYLE_RED, STYLES };
enum { TAG_CONNECTED, TAG_ERROR, TAG_LOGIN, TAG_WAITING, TAG_NETWORK_OFFLINE, TAG_USER_OFFLINE, TAGS };
static const char *const kTags[TAGS] = {"CONNECTED", "GENERIC_ERROR", "LOGIN", "WAITING_PSN", "NETWORK_OFFLINE", "USER_OFFLINE"};
static const uint8_t kTagStyle[TAGS] = {STYLE_GREEN, STYLE_RED, STYLE_AMBER, STYLE_AMBER, STYLE_AMBER, STYLE_AMBER};

enum { DRAWN_NONE, DRAWN, DRAWN_NEW };

typedef struct {
    uint32_t base, helper, helper_hide; /* helper_hide: its `jump-if-true` into the patch_notified hide */
    uint32_t tag_store, localize, localized; /* the helper's tag store, its string path's localize call */
    uint32_t owner;                     /* the tick's `status box set?` after its dot update */
    uint32_t hides[HIDES];              /* `layout_box_set_visible(status box, false)` on a server error or disconnect */
    uint32_t text, text_green;                /* the helper's text colour choice */
    uint32_t gadget[STYLES]; /* in the dot's colour blocks: `<r g b a> vec4; set_text_color(top, it)`, then return */
    uint32_t dot, dot_style[STYLES];          /* the status dot's colour choice and its three colour blocks */
    uint16_t tags[TAGS], auth_code, vec4, set_visible;
    uint32_t visible[MAX_VISIBLE]; /* the helper's `status_icon set?` argument of layout_box_set_text_visible */
    uint8_t drawn[MAX_VISIBLE];    /* what the helper did to the text box before that call */
    size_t visible_count;
} Sites;

static uint32_t read_u32(const uint8_t *at) {
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static uint16_t read_u16(const uint8_t *at) {
    return (uint16_t)(at[0] | at[1] << 8);
}

static const char *string_at(const uint8_t *sdat, size_t size, uint32_t index) {
    const uint32_t table = read_u32(sdat + STRING_TABLE) + 4u, count = read_u32(sdat + STRING_TABLE + 4);
    if (index >= count || (uint64_t)table + (uint64_t)count * 8u > size) return NULL;
    const uint32_t at = read_u32(sdat + table + index * 8u);
    if (at >= size || !memchr(sdat + at, 0, size - at)) return NULL;
    return (const char *)sdat + at;
}

static int string_index(const uint8_t *sdat, size_t size, const char *text, uint16_t *out) {
    const uint32_t count = read_u32(sdat + STRING_TABLE + 4);
    for (uint32_t i = 0; i < count && i <= 0xffffu; ++i) {
        const char *found = string_at(sdat, size, i);
        if (!found) return 0;
        if (!strcmp(found, text)) {
            *out = (uint16_t)i;
            return 1;
        }
    }
    return 0;
}

static int import_index(const uint8_t *sdat, size_t size, const char *name, uint16_t *out) {
    const uint32_t table = read_u32(sdat + IMPORT_TABLE), count = read_u32(sdat + IMPORT_TABLE + 4);
    if ((uint64_t)table + (uint64_t)count * 4u > size) return 0;
    for (uint32_t i = 0; i < count && i <= 0xffffu; ++i) {
        const char *found = string_at(sdat, size, read_u16(sdat + table + i * 4u));
        if (found && !strcmp(found, name)) {
            *out = (uint16_t)i;
            return 1;
        }
    }
    return 0;
}

/* The only offset of `bytes` (0xff00 | n: any byte n) from `from` on, or 0. */
static uint32_t unique(const uint8_t *sdat, size_t size, uint32_t from, const uint16_t *pattern, size_t count) {
    uint32_t found = 0;
    for (size_t at = from; at + count <= size; ++at) {
        size_t i = 0;
        while (i < count && (pattern[i] > 0xff || sdat[at + i] == pattern[i])) ++i;
        if (i < count) continue;
        if (found) return 0;
        found = (uint32_t)at;
    }
    return found;
}

/* The first offset of the pattern in [from,to), or 0. */
static uint32_t first(const uint8_t *sdat, size_t size, uint32_t from, uint32_t to, const uint16_t *pattern, size_t count) {
    for (size_t at = from; at < to && at + count <= size; ++at) {
        size_t i = 0;
        while (i < count && (pattern[i] > 0xff || sdat[at + i] == pattern[i])) ++i;
        if (i == count) return (uint32_t)at;
    }
    return 0;
}

static int contains(const uint8_t *data, size_t size, const uint8_t *bytes, size_t count) {
    for (size_t at = 0; at + count <= size; ++at)
        if (data[at] == bytes[0] && !memcmp(data + at, bytes, count)) return 1;
    return 0;
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

#define ANY 0xffffu
#define WORD(v) (uint16_t)((v) & 0xff), (uint16_t)((v) >> 8 & 0xff), (uint16_t)((v) >> 16 & 0xff), (uint16_t)((v) >> 24)
#define COUNT(a) (sizeof(a) / sizeof(*(a)))

static uint32_t target(const Sites *s, const uint8_t *sdat, uint32_t operand_at) {
    return s->base + read_u32(sdat + operand_at);
}

/* The helper's `pick flag; jump-if-true green` before its red text colour, and the green branch's vec4 (0.3, 1, 0.3). */
static const char *find_text(const uint8_t *sdat, size_t size, Sites *s) {
    static const uint16_t kChoice[] = {0x00, 0x01, 0x0b, ANY, 0x1c, ANY, ANY, ANY, ANY, 0x00, 0x05, 0x05, 0x33, 0x33, 0x73, 0x3f};
    static const uint16_t kGreen[] = {0x00, 0x05, 0x05, 0x9a, 0x99, 0x99, 0x3e, 0x06, 0x01, 0x05, 0x9a, 0x99, 0x99, 0x3e,
                                      0x06, 0x01, 0x1f, ANY, ANY, 0x1b};
    s->text = unique(sdat, size, s->helper, kChoice, COUNT(kChoice));
    if (!s->text || s->text > s->helper + 0x100) return "the status text colour choice differs";
    s->text_green = target(s, sdat, s->text + 5);
    if (s->text_green + 24 > size) return "the green status text differs";
    for (size_t i = 0; i < COUNT(kGreen); ++i)
        if (kGreen[i] <= 0xff && sdat[s->text_green + i] != kGreen[i]) return "the green status text differs";
    if (read_u16(sdat + s->text_green + 17) != s->vec4) return "the green status text differs";
    return NULL;
}

/* `if online ... jump-if-true A; pick nm; jump-if-true red`; A: `if online jump-if-true B`, then yellow; B: `pick nm;
 * jump-if-false green`. Every colour block starts by getting the dot's box. */
static const char *find_dot(const uint8_t *sdat, size_t size, Sites *s) {
    static const uint16_t kChoice[] = {0x00, 0x01, 0x17, WORD(VAR_ONLINE), 0x1c, ANY, ANY, ANY, ANY, 0x00, 0x01, 0x0b, 0x00, 0x1c};
    s->dot = unique(sdat, size, s->base, kChoice, COUNT(kChoice));
    if (!s->dot) return "the status dot colour choice is not unique";
    const uint32_t a = target(s, sdat, s->dot + 8);
    s->dot_style[STYLE_RED] = target(s, sdat, s->dot + 17);
    s->dot_style[STYLE_AMBER] = a + 12;
    if (a + 15 > size || sdat[a] != 0x00 || sdat[a + 2] != OP_VAR || read_u32(sdat + a + 3) != VAR_ONLINE ||
        sdat[a + 7] != OP_JUMP_IF_TRUE)
        return "the status dot colour choice differs";
    const uint32_t b = target(s, sdat, a + 8);
    if (b + 10 > size || sdat[b + 4] != OP_JUMP_IF_FALSE) return "the status dot colour choice differs";
    s->dot_style[STYLE_GREEN] = target(s, sdat, b + 5);
    for (int i = 0; i < STYLES; ++i) {
        const uint32_t at = s->dot_style[i];
        if (at + 7 > size || sdat[at] != OP_RESERVE || sdat[at + 2] != OP_VAR || read_u32(sdat + at + 3) != VAR_STATUS_ICON)
            return "the status dot colours differ";
    }
    return NULL;
}

/* What the helper did to the text box before `layout_box_set_text_visible(status box, status_icon set?)` at `at`. */
static int drawn_before(const uint8_t *sdat, uint32_t at, uint16_t set_color) {
    const uint32_t box = at - 5;
    if (sdat[box] != OP_VAR || read_u32(sdat + box + 1) != VAR_STATUS_BOX) return -1;
    if (sdat[box - 5] == OP_SET_VAR && read_u32(sdat + box - 4) == VAR_STATUS_BOX) return DRAWN_NEW;
    if (sdat[box - 5] == OP_CLEAR_VAR && read_u32(sdat + box - 4) == VAR_STATUS_BOX) return DRAWN_NONE;
    if (sdat[box - 3] == OP_NATIVE && read_u16(sdat + box - 2) == set_color) return DRAWN;
    return -1;
}

static int jumped_into(const uint8_t *sdat, size_t size, uint32_t base, uint32_t from, uint32_t to) {
    for (size_t at = base; at + 5 <= size; ++at) {
        const uint8_t op = sdat[at];
        if (op != OP_JUMP && op != OP_JUMP_IF_TRUE && op != OP_JUMP_IF_FALSE && op != OP_PUSH_RETURN) continue;
        const uint32_t to_at = base + read_u32(sdat + at + 1);
        if (to_at > from && to_at < to) return 1;
    }
    return 0;
}

/* `reserve 2; pick n; set status_tag; pick n; is_string; jump-if-true S`, S: `reserve 2; pick n; hide_type_from_compiler;
 * push-return R; jump L`: L localizes the tag and returns to R. */
static const char *find_tag_store(const uint8_t *sdat, size_t size, Sites *s, uint16_t is_string, uint16_t hide_type) {
    const uint16_t store[] = {OP_RESERVE, 2, OP_PICK, ANY, OP_SET_VAR, WORD(VAR_STATUS_TAG), OP_PICK, ANY, OP_NATIVE,
                              is_string & 0xff, is_string >> 8, OP_JUMP_IF_TRUE};
    s->tag_store = first(sdat, size, s->helper, s->helper + HELPER_TAG_WINDOW, store, COUNT(store));
    if (!s->tag_store || sdat[s->tag_store + 3] != sdat[s->tag_store + 10]) return "the status helper's tag differs";
    const uint32_t path = target(s, sdat, s->tag_store + 15);
    const uint8_t pick = sdat[s->tag_store + 3];
    if (path + 17 > size || sdat[path] != OP_RESERVE || sdat[path + 2] != OP_PICK || sdat[path + 3] != pick ||
        sdat[path + 4] != OP_NATIVE || read_u16(sdat + path + 5) != hide_type || sdat[path + 7] != OP_PUSH_RETURN ||
        sdat[path + 12] != OP_JUMP)
        return "the status helper's localization differs";
    s->localized = target(s, sdat, path + 8);
    s->localize = target(s, sdat, path + 13);
    if (jumped_into(sdat, size, s->base, s->tag_store, s->tag_store + OWNER_LENGTH))
        return "the status helper's tag store is a jump target";
    return NULL;
}

/* vec4 is a `ret` stub outside the block's code range (the game makes it an intrinsic only inside it), so appended code
 * gets its colours from these: each sets the text colour of the box under it and returns to the address under that. */
static const char *find_gadgets(const uint8_t *sdat, size_t size, Sites *s, uint16_t set_color) {
    const uint16_t tail[] = {OP_NATIVE, s->vec4 & 0xff, s->vec4 >> 8, OP_NATIVE, set_color & 0xff, set_color >> 8,
                             OP_NIL, OP_PICK, 3, 0x0c, 3, 2, 0x1e};
    static const uint8_t kFirst[STYLES][5] = {{OP_FLOAT, 0x00, 0x00, 0x00, 0x3f}, {OP_INT, 1}, {OP_FLOAT, 0xcd, 0xcc, 0x8c, 0x3f}};
    static const uint8_t kFirstLength[STYLES] = {5, 2, 5};
    uint8_t icon[5];
    put32(icon, OP_VAR, VAR_STATUS_ICON);
    int found = 0;
    for (uint32_t at = first(sdat, size, s->base, (uint32_t)size, tail, COUNT(tail)); at;
         at = first(sdat, size, at + 1, (uint32_t)size, tail, COUNT(tail))) {
        uint32_t entry = 0;
        for (uint32_t back = at - 5; back > at - GADGET_WINDOW && !entry; --back)
            if (!memcmp(sdat + back, icon, sizeof(icon))) entry = back + 5;
        int style = -1;
        for (int i = 0; entry && i < STYLES && style < 0; ++i)
            if (!memcmp(sdat + entry, kFirst[i], kFirstLength[i])) style = i;
        if (style < 0 || found & 1 << style) return "the status dot colours differ";
        found |= 1 << style;
        s->gadget[style] = entry;
    }
    return found == 7 ? NULL : "the status dot colours differ";
}

static const char *find_sites(const uint8_t *sdat, size_t size, Sites *s) {
    if (size < 0x30 || read_u32(sdat) != BYTECODE_MAGIC) return "not the game's bytecode";
    s->base = read_u32(sdat + CODE_WORD);
    if (s->base >= size) return "the code base is outside the block";
    for (int i = 0; i < TAGS; ++i)
        if (!string_index(sdat, size, kTags[i], &s->tags[i])) return "a status name is missing";
    uint16_t is_reference, set_visible, set_color, is_string, hide_type;
    if (!import_index(sdat, size, "layout_box_set_text_visible", &set_visible) ||
        !import_index(sdat, size, "layout_box_set_text_color", &set_color) ||
        !import_index(sdat, size, "server_request_auth_code", &s->auth_code) ||
        !import_index(sdat, size, "is_reference", &is_reference) || !import_index(sdat, size, "vec4", &s->vec4) ||
        !import_index(sdat, size, "is_string", &is_string) || !import_index(sdat, size, "hide_type_from_compiler", &hide_type))
        return "a native is missing";
    s->set_visible = set_visible;

    /* The tick, after it updated the dot: `status box set?`. */
    const uint16_t owner[] = {OP_RESERVE, 1, OP_NIL, OP_POP, 1, OP_VAR_TEST, WORD(VAR_STATUS_BOX), OP_JUMP_IF_TRUE};
    s->owner = unique(sdat, size, s->base, owner, COUNT(owner));
    if (!s->owner) return "the status tick is not unique";
    s->owner += 5;
    uint16_t set_box_visible;
    if (!import_index(sdat, size, "layout_box_set_visible", &set_box_visible)) return "a native is missing";
    const uint16_t hide_box[] = {OP_VAR, WORD(VAR_STATUS_BOX), OP_FALSE, OP_NATIVE, set_box_visible & 0xff, set_box_visible >> 8};
    size_t hides = 0;
    for (uint32_t at = first(sdat, size, s->base, (uint32_t)size, hide_box, COUNT(hide_box)); at;
         at = first(sdat, size, at + 1, (uint32_t)size, hide_box, COUNT(hide_box))) {
        if (hides == HIDES || at < s->owner || at > s->owner + HIDE_WINDOW) return "the status box hides differ";
        s->hides[hides++] = at + 5;
    }
    if (hides != HIDES) return "the status box hides differ";

    const uint16_t call[] = {OP_RESERVE, 3, OP_TRUE, OP_STRING, s->tags[TAG_CONNECTED] & 0xff, s->tags[TAG_CONNECTED] >> 8,
                             OP_PUSH_RETURN, ANY, ANY, ANY, ANY, OP_JUMP};
    const uint32_t connected = unique(sdat, size, s->base, call, COUNT(call));
    if (!connected || connected + 16 > size) return "the CONNECTED status call is not unique";
    s->helper = target(s, sdat, connected + 12);
    uint8_t tag[5];
    put32(tag, OP_SET_VAR, VAR_STATUS_TAG);
    if (s->helper >= size - HELPER_TAG_WINDOW || !contains(sdat + s->helper, HELPER_TAG_WINDOW, tag, sizeof(tag)))
        return "the status helper differs";

    /* With Network_Manager up, the helper hides the text for good once that manager has shown an error dialog. */
    const uint16_t gate[] = {OP_PICK, 0, OP_NATIVE, is_reference & 0xff, is_reference >> 8, OP_JUMP_IF_TRUE, ANY, ANY, ANY, ANY};
    s->helper_hide = first(sdat, size, s->helper, s->helper + HELPER_GATE_WINDOW, gate, COUNT(gate));
    if (!s->helper_hide) return "the status helper's manager check differs";
    s->helper_hide += 5;
    const uint32_t hide = target(s, sdat, s->helper_hide + 1);
    if (hide + 9 > size || sdat[hide + 2] != OP_PICK || sdat[hide + 4] != 0x10 || read_u32(sdat + hide + 5) != VAR_PATCH_NOTIFIED)
        return "the status helper's hide differs";
    const uint16_t visible[] = {OP_VAR_TEST, WORD(VAR_STATUS_ICON), OP_NATIVE, set_visible & 0xff, set_visible >> 8};
    int seen = 0;
    for (uint32_t at = s->helper + 10; at < s->helper + HELPER_VISIBLE_WINDOW && at + VISIBLE_LENGTH + 3 <= size; ++at)
        if (first(sdat, size, at, at + 1, visible, COUNT(visible))) {
            const int drawn = drawn_before(sdat, at, set_color);
            if (s->visible_count == MAX_VISIBLE || drawn < 0) return "the status text visibility differs";
            seen |= 1 << drawn;
            s->drawn[s->visible_count] = (uint8_t)drawn;
            s->visible[s->visible_count++] = at;
        }
    if (seen != 7) return "the status text visibility differs";
    const char *why = find_tag_store(sdat, size, s, is_string, hide_type);
    if (!why) why = find_text(sdat, size, s);
    if (!why) why = find_gadgets(sdat, size, s, set_color);
    return why ? why : find_dot(sdat, size, s);
}

size_t online_status_script_extra(void) {
    return EXTRA;
}

typedef struct {
    uint8_t *start, *at;
    uint32_t origin, base;
} Code;

static uint32_t here(const Code *c) {
    return c->origin + (uint32_t)(c->at - c->start);
}

/* A jump or pushed return address whose target is set later. */
static uint8_t *forward(Code *c, uint8_t op) {
    uint8_t *operand = c->at + 1;
    c->at = put32(c->at, op, 0);
    return operand;
}

static void land(const Code *c, uint8_t *operand) {
    const uint32_t target = here(c) - c->base;
    for (int i = 0; i < 4; ++i) operand[i] = (uint8_t)(target >> (8 * i));
}

static void jump(Code *c, uint32_t to) {
    c->at = put32(c->at, OP_JUMP, to - c->base);
}

static void pop1(Code *c) {
    c->at = put8(put8(c->at, OP_POP), 1);
}

/* Copies the bytes the trampoline at `site` replaced, then goes on after them. */
static void replaced(Code *c, const uint8_t *sdat, uint32_t site, size_t length) {
    memcpy(c->at, sdat + site, length);
    c->at += length;
    jump(c, site + (uint32_t)length);
}

static void ask(Code *c, const Sites *s, uint32_t magic) {
    c->at = put32(c->at, OP_INT32, magic);
    c->at = put16(c->at, OP_NATIVE, s->auth_code);
}

/* While the dot exists the text is the current state's: repainted through the helper when the text box or its tag is
 * gone or the state changed since the last paint, and never faded. Without the dot the text is hidden and its tag
 * cleared, so the first tick with the dot paints it again on the same box. */
static void emit_owner(Code *c, const uint8_t *sdat, const Sites *s) {
    uint8_t *tail[3], *absent[2], *no_dot, *no_box, *ret;
    c->at = put8(put8(c->at, OP_RESERVE), 3);
    c->at = put32(c->at, OP_VAR_TEST, VAR_STATUS_ICON);
    no_dot = forward(c, OP_JUMP_IF_FALSE);
    c->at = put32(c->at, OP_CLEAR_VAR, VAR_PROMPT_DELAY);
    c->at = put32(c->at, OP_VAR_TEST, VAR_STATUS_BOX);
    absent[0] = forward(c, OP_JUMP_IF_FALSE);
    c->at = put32(c->at, OP_VAR_TEST, VAR_STATUS_TAG);
    absent[1] = forward(c, OP_JUMP_IF_FALSE);
    ask(c, s, ONLINE_STATUS_CHANGED_MAGIC);
    c->at = put8(put8(put8(c->at, OP_INT), 1), OP_EQUAL);
    tail[0] = forward(c, OP_JUMP_IF_FALSE);
    land(c, absent[0]);
    land(c, absent[1]);
    c->at = put8(put8(put8(c->at, OP_RESERVE), 3), OP_TRUE);
    c->at = put16(c->at, OP_STRING, s->tags[TAG_CONNECTED]);
    ret = forward(c, OP_PUSH_RETURN);
    jump(c, s->helper);
    land(c, ret);
    pop1(c);
    tail[1] = forward(c, OP_JUMP);
    land(c, no_dot);
    c->at = put32(c->at, OP_VAR_TEST, VAR_STATUS_TAG);
    tail[2] = forward(c, OP_JUMP_IF_FALSE);
    c->at = put32(c->at, OP_CLEAR_VAR, VAR_STATUS_TAG);
    c->at = put32(c->at, OP_VAR_TEST, VAR_STATUS_BOX);
    no_box = forward(c, OP_JUMP_IF_FALSE);
    c->at = put32(c->at, OP_VAR, VAR_STATUS_BOX);
    c->at = put8(c->at, OP_FALSE);
    c->at = put16(c->at, OP_NATIVE, s->set_visible);
    land(c, no_box);
    for (int i = 0; i < 3; ++i) land(c, tail[i]);
    replaced(c, sdat, s->owner, OWNER_LENGTH);
}

/* The helper paints the current state's tag whatever its caller passed (the caller's own tag if the state is unknown). */
static void emit_tag(Code *c, const uint8_t *sdat, const Sites *s) {
    enum { PAINTS = 4 };
    static const uint8_t kTagOf[PAINTS] = {TAG_NETWORK_OFFLINE, TAG_LOGIN, TAG_CONNECTED, TAG_ERROR}; /* ONLINE_STATUS_* */
    uint8_t *branch[PAINTS], *set[PAINTS], *hidden, *asked;
    c->at = put8(put8(c->at, OP_RESERVE), 3);
    c->at = put32(c->at, OP_VAR_TEST, VAR_STATUS_ICON);
    hidden = forward(c, OP_JUMP_IF_FALSE);
    ask(c, s, ONLINE_STATUS_PAINT_MAGIC);
    asked = forward(c, OP_JUMP);
    land(c, hidden);
    ask(c, s, ONLINE_STATUS_PAINT_HIDDEN_MAGIC);
    land(c, asked);
    for (int i = 0; i < PAINTS; ++i) {
        c->at = put8(put8(c->at, OP_PICK), 0);
        c->at = put8(put8(c->at, OP_INT), (uint8_t)(ONLINE_STATUS_OFF + i + 1));
        c->at = put8(c->at, OP_EQUAL);
        branch[i] = forward(c, OP_JUMP_IF_TRUE);
    }
    pop1(c);
    replaced(c, sdat, s->tag_store, TAG_STORE_LENGTH);
    for (int i = 0; i < PAINTS; ++i) {
        land(c, branch[i]);
        pop1(c);
        c->at = put16(c->at, OP_STRING, s->tags[kTagOf[i]]);
        set[i] = forward(c, OP_JUMP);
    }
    for (int i = 0; i < PAINTS; ++i) land(c, set[i]);
    c->at = put32(c->at, OP_SET_VAR, VAR_STATUS_TAG);
    c->at = put32(c->at, OP_VAR, VAR_STATUS_TAG);
    c->at = put32(c->at, OP_PUSH_RETURN, s->localized - c->base);
    jump(c, s->localize);
}

static void branch_on_tag(Code *c, const Sites *s, uint8_t **style);

/* Sets the text colour of the status box from the status tag (none for any other text). */
static void emit_tag_colour(Code *c, const Sites *s) {
    uint8_t *by_tag[TAGS], *done, *ret[STYLES];
    branch_on_tag(c, s, by_tag);
    pop1(c);
    done = forward(c, OP_JUMP);
    for (int style = 0; style < STYLES; ++style) {
        for (int i = 0; i < TAGS; ++i)
            if (kTagStyle[i] == style) land(c, by_tag[i]);
        pop1(c);
        c->at = put8(put8(c->at, OP_RESERVE), 6);
        ret[style] = forward(c, OP_PUSH_RETURN);
        c->at = put8(put8(c->at, OP_NIL), OP_NIL);
        c->at = put32(c->at, OP_VAR, VAR_STATUS_BOX);
        jump(c, s->gadget[style]);
    }
    for (int style = 0; style < STYLES; ++style) land(c, ret[style]);
    pop1(c);
    land(c, done);
}

/* The helper's visibility calls that follow its text box being made or updated (the box on top of the stack): the text
 * takes its state's colour, then the helper's own visibility (the text shows while the dot exists) goes on. */
static void emit_drawn(Code *c, const uint8_t *sdat, const Sites *s, size_t i) {
    uint8_t *made = NULL;
    if (s->drawn[i] == DRAWN_NEW) {
        c->at = put32(c->at, OP_VAR_TEST, VAR_STATUS_BOX);
        made = forward(c, OP_JUMP_IF_TRUE);
        replaced(c, sdat, s->visible[i], VISIBLE_LENGTH);
        land(c, made);
    }
    emit_tag_colour(c, s);
    replaced(c, sdat, s->visible[i], VISIBLE_LENGTH);
}

/* Pushes status_tag and jumps to style[i] (with the tag still pushed) for the first known tag of that style. */
static void branch_on_tag(Code *c, const Sites *s, uint8_t **style) {
    c->at = put8(put8(c->at, OP_RESERVE), 3);
    c->at = put32(c->at, OP_VAR_OR_NIL, VAR_STATUS_TAG);
    for (int i = 0; i < TAGS; ++i) {
        c->at = put16(c->at, OP_STRING, s->tags[i]);
        c->at = put8(put8(put8(c->at, OP_PICK), 1), OP_EQUAL);
        style[i] = forward(c, OP_JUMP_IF_TRUE);
    }
}

/* The helper builds its text colour in its own code (vec4 only works there): a status tag takes its green block, any
 * other text keeps the helper's choice. The state's colour is set once the box exists (emit_tag_colour). */
static void emit_text(Code *c, const uint8_t *sdat, const Sites *s) {
    uint8_t *by_tag[TAGS];
    branch_on_tag(c, s, by_tag);
    pop1(c);
    replaced(c, sdat, s->text, TEXT_LENGTH);
    for (int i = 0; i < TAGS; ++i) land(c, by_tag[i]);
    pop1(c);
    jump(c, s->text_green);
}

/* The dot agrees with the status text while there is one, and shows the current state when there is none. */
static void emit_dot(Code *c, const uint8_t *sdat, const Sites *s) {
    uint8_t *by_tag[TAGS], *by_state[3];
    branch_on_tag(c, s, by_tag);
    pop1(c);
    c->at = put32(c->at, OP_INT32, ONLINE_STATUS_STYLE_MAGIC);
    c->at = put16(c->at, OP_NATIVE, s->auth_code);
    static const uint8_t kAnswers[] = {ONLINE_STATUS_CONNECTED + 1, ONLINE_STATUS_UNAVAILABLE + 1, 0};
    for (int i = 0; i < 3; ++i) {
        c->at = put8(put8(c->at, OP_PICK), 0);
        c->at = put8(put8(c->at, OP_INT), kAnswers[i]);
        c->at = put8(c->at, OP_EQUAL);
        by_state[i] = forward(c, OP_JUMP_IF_TRUE);
    }
    pop1(c);
    jump(c, s->dot_style[STYLE_AMBER]);
    land(c, by_state[2]);
    pop1(c);
    replaced(c, sdat, s->dot, DOT_LENGTH);
    for (int style = 0; style < STYLES; ++style) {
        for (int i = 0; i < TAGS; ++i)
            if (kTagStyle[i] == style) land(c, by_tag[i]);
        if (style == STYLE_GREEN) land(c, by_state[0]);
        if (style == STYLE_RED) land(c, by_state[1]);
        pop1(c);
        jump(c, s->dot_style[style]);
    }
}

size_t online_status_script_patch(uint8_t *sdat, size_t size, size_t capacity, const char **why) {
    Sites s = {0};
    const char *problem = find_sites(sdat, size, &s);
    if (!problem && (capacity < size || capacity - size < EXTRA)) problem = "no room for the patch";
    if (problem) {
        *why = problem;
        return 0;
    }
    Code c = {sdat + size, sdat + size, (uint32_t)size, s.base};
    const uint32_t owner = here(&c);
    emit_owner(&c, sdat, &s);
    const uint32_t tag = here(&c);
    emit_tag(&c, sdat, &s);
    const uint32_t text = here(&c);
    emit_text(&c, sdat, &s);
    const uint32_t dot = here(&c);
    emit_dot(&c, sdat, &s);
    uint32_t drawn[MAX_VISIBLE] = {0};
    for (size_t i = 0; i < s.visible_count; ++i) {
        if (s.drawn[i] == DRAWN_NONE) continue;
        drawn[i] = here(&c);
        emit_drawn(&c, sdat, &s, i);
    }
    put32(sdat + s.owner, OP_JUMP, owner - s.base);
    put32(sdat + s.tag_store, OP_JUMP, tag - s.base);
    put32(sdat + s.text, OP_JUMP, text - s.base);
    put32(sdat + s.dot, OP_JUMP, dot - s.base);
    put32(sdat + s.helper_hide, OP_JUMP_IF_TRUE, s.helper_hide + 5 - s.base);
    for (size_t i = 0; i < s.visible_count; ++i)
        if (drawn[i]) put32(sdat + s.visible[i], OP_JUMP, drawn[i] - s.base);
    for (size_t i = 0; i < HIDES; ++i) sdat[s.hides[i]] = OP_TRUE;
    return size + (size_t)(c.at - c.start);
}
