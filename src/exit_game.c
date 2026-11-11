#include "exit_game.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* The row's text before the swap: the bytecode's own string, never shown otherwise. */
#define MARKER "exit_game_application"
#define LANGUAGE_ENTRY 16
#define LANGUAGE_NAME_HOLDER 8
#define STRING_CHARS 0x10
#define EXIT_HOOK_SKIP 7
#define EXIT_HOOK_LENGTH 6
#define TEXT_COPY_LEAD 26
#define BOX_NEW_TEXT_LEAD 10
#define HOOK_COUNT 3
/* A layout box instance: its offset from the layout (x, y floats) and the flag that makes it recompute its rectangle. */
#define BOX_OFFSET_Y 0x60
#define BOX_DIRTY 0x48
/* Select Campaign's rows are fixed layout boxes; the Exit Game row is the Load box one row lower (Load top - New top). */
#define CAMPAIGN_ROW_STEP (-0.15625f)

static const ExitGameSite kSites[] = {
    {"CUSA00062-1.00", 0x239350u, 0x2cebe18u, 0x252943u, 0xd8930u, 0x25ac45u, 0xd87e0u, 0x25a486u, 0xcb1a0u,
     0x278eef0u, 0x278edb0u},
    {"CUSA00062-1.06", 0x23c880u, 0x2ce9180u, 0x256183u, 0xdb690u, 0x25e715u, 0xdb540u, 0x25df56u, 0xcdcf0u,
     0x278bd10u, 0x278bb50u},
    {"CUSA04430-1.00", 0x239f60u, 0x2ce01c8u, 0x253633u, 0xd94f0u, 0x25bb55u, 0xd93a0u, 0x25b396u, 0xcbbf0u,
     0x2783160u, 0x2782fa0u},
};

static const struct {
    const char *language;
    const char *text;
} kTexts[] = {
    {"en", "EXIT GAME"},
    {"fr", "QUITTER LE JEU"},
    {"de", "SPIEL BEENDEN"},
    {"it", "ESCI DAL GIOCO"},
    {"es", "SALIR DEL JUEGO"},
    {"las", "SALIR DEL JUEGO"},
    {"port", "SAIR DO JOGO"},
    {"bport", "SAIR DO JOGO"},
    {"nl", "SPEL AFSLUITEN"},
    {"da", "AFSLUT SPILLET"},
    {"fi", "LOPETA PELI"},
    {"nor", "AVSLUTT SPILLET"},
    {"sw", "AVSLUTA SPELET"},
    {"pol", "WYJDŹ Z GRY"},
    {"ru", "ВЫЙТИ ИЗ ИГРЫ"},
    {"jp", "ゲーム終了"},
    {"kr", "게임 종료"},
    {"cn", "結束遊戲"},
};

static const uint8_t kExitApp[] = {0x48, 0x8d, 0x05, 0, 0, 0, 0, 0x48, 0x8b, 0x38, 0x48, 0x8b, 0x07, 0xff, 0x60, 0x28};
/* The hud text's set-text helper copying the new text's bytes [rsi, rdx) before the call. */
static const uint8_t kTextCopy[] = {0x4c, 0x8d, 0x65, 0xc8, 0x48, 0x8d, 0x73, 0x20, 0x4d, 0x8d, 0xbe, 0xb0, 0x00, 0x00,
                                    0x00, 0x8b, 0x43, 0x10, 0x4c, 0x89, 0xe7, 0x48, 0x8d, 0x54, 0x03, 0x20, 0xe8};
/* A new layout box copying its text parameter, a C string in rsi; r15 holds the box. */
static const uint8_t kBoxNewText[] = {0x48, 0x8d, 0x9d, 0xd8, 0xfe, 0xff, 0xff, 0x48, 0x89, 0xdf, 0xe8};
/* Earlier in the same function: the box's offset written from r15 and its rectangle marked stale. */
static const uint8_t kBoxOffsetStore[] = {0x41, 0x89, 0x47, 0x5c, 0x41, 0x89, 0x4f, BOX_OFFSET_Y,
                                          0x41, 0xc6, 0x47, BOX_DIRTY, 0x01};
static const uint8_t kLanguageName[] = {0x48, 0x63, 0x05, 0, 0, 0, 0, 0x48, 0x8d, 0x0d, 0, 0, 0, 0,
                                        0x48, 0xc1, 0xe0, 0x04, 0x48, 0x8d, 0x7c, 0x08, 0x08, 0xe9};

static ExitGameHandler handler;
static const uint8_t *game_image;
static const ExitGameSite *game_site;
static atomic_int exit_requested;
static atomic_int options_logged, campaign_logged;

const ExitGameSite *exit_game_site(const char *build) {
    for (size_t i = 0; build && i < sizeof(kSites) / sizeof(*kSites); ++i)
        if (!strcmp(build, kSites[i].build)) return &kSites[i];
    return NULL;
}

const char *exit_game_text(const char *language) {
    for (size_t i = 0; language && i < sizeof(kTexts) / sizeof(*kTexts); ++i)
        if (!strcmp(language, kTexts[i].language)) return kTexts[i].text;
    return kTexts[0].text;
}

void exit_game_set_handler(ExitGameHandler value) {
    handler = value;
}

size_t exit_game_stub_bytes(void) {
    return HOOK_COUNT * guest_hook_stub_size();
}

static int32_t read_i32(const uint8_t *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

/* `pattern` at `at`, its zero bytes standing for the rel32 operands. */
static int code_matches(const uint8_t *image, uint64_t size, uint64_t at, const uint8_t *pattern, size_t count) {
    if (at > size || size - at < count) return 0;
    for (size_t i = 0; i < count; ++i)
        if (pattern[i] && image[at + i] != pattern[i]) return 0;
    return 1;
}

static uint64_t rip_target(const uint8_t *image, uint64_t operand) {
    return operand + 4 + (int64_t)read_i32(image + operand);
}

int exit_game_verify(const uint8_t *image, uint64_t size, const ExitGameSite *site, const char **why) {
    const char *reason = NULL;
    if (!site)
        reason = "not a supported build";
    else if (!code_matches(image, size, site->exit_app, kExitApp, sizeof(kExitApp)) ||
             rip_target(image, site->exit_app + 3) != site->application)
        reason = "exit_game_application differs";
    else if (site->text_copy < TEXT_COPY_LEAD ||
             !code_matches(image, size, site->text_copy - TEXT_COPY_LEAD, kTextCopy, sizeof(kTextCopy)) ||
             rip_target(image, site->text_copy + 1) != site->text_copy_target)
        reason = "the hud text copy differs";
    else if (site->box_new_text < BOX_NEW_TEXT_LEAD || site->box_offset_store >= site->box_new_text ||
             !code_matches(image, size, site->box_new_text - BOX_NEW_TEXT_LEAD, kBoxNewText, sizeof(kBoxNewText)) ||
             rip_target(image, site->box_new_text + 1) != site->box_new_text_copy ||
             !code_matches(image, size, site->box_offset_store, kBoxOffsetStore, sizeof(kBoxOffsetStore)))
        reason = "the layout box text parameter differs";
    else if (!code_matches(image, size, site->language_name, kLanguageName, sizeof(kLanguageName)) ||
             rip_target(image, site->language_name + 3) != site->language_index ||
             rip_target(image, site->language_name + 10) != site->language_table ||
             site->language_index <= site->language_table || site->language_index + 4ull > size)
        reason = "the language getter differs";
    if (reason && why) *why = reason;
    return !reason;
}

/* The game's current language name, from the table its language getter reads; "" when unset. */
static void current_language(char *out, size_t size) {
    out[0] = 0;
    const int32_t index = read_i32(game_image + game_site->language_index);
    const uint32_t entries = (game_site->language_index - game_site->language_table) / LANGUAGE_ENTRY;
    if (index < 0 || (uint32_t)index >= entries) return;
    const uint8_t *entry = game_image + game_site->language_table + (uint32_t)index * LANGUAGE_ENTRY;
    const uint8_t *block;
    memcpy(&block, entry + LANGUAGE_NAME_HOLDER, sizeof(block));
    if (!block) return;
    uint64_t length;
    memcpy(&length, block, sizeof(length));
    if (!length || length >= size) return;
    memcpy(out, block + STRING_CHARS, length);
    out[length] = 0;
}

static const char *row_text(atomic_int *logged, const char *menu) {
    char language[16];
    current_language(language, sizeof(language));
    if (!atomic_exchange(logged, 1))
        printf("Exit Game: %s row shown (language %s)\n", menu, language[0] ? language : "unknown");
    return exit_game_text(language);
}

/* Before a hud text copies its new text's bytes [rsi, rdx). The Options rows are hud texts. */
void GUEST_HOOK_ABI exit_game_text_hook(GuestRegs *regs) {
    const size_t length = (size_t)(regs->rdx - regs->rsi);
    if (length != sizeof(MARKER) - 1 || memcmp((const void *)(uintptr_t)regs->rsi, MARKER, length)) return;
    const char *text = row_text(&options_logged, "Options");
    regs->rsi = (uintptr_t)text;
    regs->rdx = (uintptr_t)(text + strlen(text));
}

/* Before a new layout box copies its text parameter. The Select Campaign rows are layout boxes; only its Exit Game
 * item carries the marker, on the Load box, which moves one row down. */
void GUEST_HOOK_ABI exit_game_box_new_hook(GuestRegs *regs) {
    if (!regs->rsi || strcmp((const char *)(uintptr_t)regs->rsi, MARKER)) return;
    regs->rsi = (uintptr_t)row_text(&campaign_logged, "Select Campaign");
    uint8_t *box = (uint8_t *)(uintptr_t)regs->r15;
    float offset;
    memcpy(&offset, box + BOX_OFFSET_Y, sizeof(offset));
    offset += CAMPAIGN_ROW_STEP;
    memcpy(box + BOX_OFFSET_Y, &offset, sizeof(offset));
    box[BOX_DIRTY] = 1;
}

/* At exit_game_application, before it reads the application object: the port's own exit instead. */
void GUEST_HOOK_ABI exit_game_exit_hook(GuestRegs *regs) {
    if (!handler) return;
    regs->skip = 1;
    if (atomic_exchange(&exit_requested, 1)) return;
    puts("Exit Game: exit_game_application reached, closing the game");
    if (!handler()) puts("Exit Game: no window took the request");
}

int exit_game_install(uint8_t *image, uint64_t size, const ExitGameSite *site, uint8_t *stubs, size_t stubs_size) {
    const char *why = "";
    if (!exit_game_verify(image, size, site, &why)) {
        printf("Exit Game: unavailable: %s\n", why);
        return 0;
    }
    GuestHook hooks[HOOK_COUNT] = {
        {"exit_game_application", site->exit_app + EXIT_HOOK_SKIP, EXIT_HOOK_LENGTH, {0}, exit_game_exit_hook},
        {"hud text copy", site->text_copy, 5, {0}, exit_game_text_hook},
        {"layout box text parameter", site->box_new_text, 5, {0}, exit_game_box_new_hook},
    };
    for (size_t i = 0; i < HOOK_COUNT; ++i) memcpy(hooks[i].original, image + hooks[i].offset, hooks[i].length);
    size_t bad = 0;
    const GuestHookStatus status =
        guest_hooks_install(image, size, 0, NULL, hooks, HOOK_COUNT, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Exit Game: unavailable: %s (%s)\n", guest_hook_status_text(status), hooks[bad].name);
        return 0;
    }
    game_image = image;
    game_site = site;
    return 1;
}
