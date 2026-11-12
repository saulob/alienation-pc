#include "online_status.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define ANY (-1)
#define NATIVE_ARGS 0x28
#define NATIVE_RETURN 0x30
#define VALUE_INT 0x0600000000000000ull
#define PROLOGUE_LENGTH 8
#define CALL_LENGTH 5
#define LOOKUP_LEAD 11
#define LOOKUP_DEFAULT_CALL 15
#define HOOKS 3
#define ENGLISH 4

int online_status_classify(const OnlineStatusInput *in) {
    if (!in->enabled) return ONLINE_STATUS_OFF;
    if (in->service == ONLINE_SERVICE_FAILED) return ONLINE_STATUS_UNAVAILABLE;
    if (in->service == ONLINE_SERVICE_READY && in->np_signed_in && in->matching2_started) return ONLINE_STATUS_CONNECTED;
    if (in->service == ONLINE_SERVICE_CONNECTING && in->connecting_ms > ONLINE_STATUS_CONNECT_TIMEOUT_MS)
        return ONLINE_STATUS_UNAVAILABLE;
    return ONLINE_STATUS_CONNECTING;
}

const char *online_status_name(int status) {
    static const char *const kNames[] = {"off", "connecting", "connected", "unavailable"};
    return status >= 0 && status <= ONLINE_STATUS_UNAVAILABLE ? kNames[status] : "?";
}

static atomic_int g_painted = -1;

int online_status_answer(uint32_t magic, int state) {
    switch (magic) {
    case ONLINE_STATUS_STYLE_MAGIC: return state + 1;
    case ONLINE_STATUS_PAINT_MAGIC: atomic_store(&g_painted, state); return state + 1;
    case ONLINE_STATUS_PAINT_HIDDEN_MAGIC: atomic_store(&g_painted, -1); return state + 1;
    case ONLINE_STATUS_CHANGED_MAGIC: return atomic_load(&g_painted) != state;
    default: return -1;
    }
}

/* ---- texts ---- */
typedef struct {
    uint32_t key;
    uint8_t language;
    const char *text;
} OriginalText;

#include "online_status_text.inc"

/* The box's keys, as the script's string table hashes them; the localization file stores them byte-swapped. */
static const struct {
    uint32_t key;
    int kind;
} kKeys[] = {
    {0x31bd96beu, ONLINE_TEXT_CONNECTING},  /* menu__network__no_matching: WAITING_PSN */
    {0x5fa8e43fu, ONLINE_TEXT_CONNECTING},  /* menu__network__login_wait_prompt: LOGIN */
    {0x6b03db9du, ONLINE_TEXT_CONNECTED},   /* menu__network__connected */
    {0x18f2e804u, ONLINE_TEXT_UNAVAILABLE}, /* menu__network__error_general */
    {0xdcb0d29bu, ONLINE_TEXT_OFFLINE},     /* menu__network__platform_offline: NETWORK_OFFLINE */
    {0xa4f15affu, ONLINE_TEXT_OFFLINE},     /* menu__network__user_offline: USER_OFFLINE */
};

static uint32_t swap32(uint32_t v) {
    return v >> 24 | (v >> 8 & 0xff00u) | (v << 8 & 0xff0000u) | v << 24;
}

static int key_index(uint32_t key) {
    for (size_t i = 0; i < sizeof(kKeys) / sizeof(*kKeys); ++i)
        if (kKeys[i].key == key || kKeys[i].key == swap32(key)) return (int)i;
    return -1;
}

int online_status_text_kind(uint32_t key) {
    const int i = key_index(key);
    return i < 0 ? ONLINE_TEXT_NONE : kKeys[i].kind;
}

const char *online_status_text(uint32_t key, const char *original) {
    const int i = key_index(key);
    if (i < 0) return original;
    int language = ENGLISH;
    for (size_t j = 0; original && j < sizeof(kOriginals) / sizeof(*kOriginals); ++j)
        if (kOriginals[j].key == kKeys[i].key && !strcmp(kOriginals[j].text, original)) {
            language = kOriginals[j].language;
            break;
        }
    return kTexts[language][kKeys[i].kind];
}

/* ---- hooks ---- */
static const OnlineStatusSite kSites[] = {
    {"CUSA00062-1.00", 0x5a00, 0x29014c, 0x29015b},
    {"CUSA00062-1.06", 0x5a00, 0x294b4c, 0x294b5b},
    {"CUSA04430-1.00", 0x5a00, 0x2914dc, 0x2914eb},
};

/* server_request_auth_code: its user index (an int or a float) is the only argument. */
static const short kAuthCode[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x10, 0x4c, 0x8b, 0x25, ANY,
    ANY,  ANY,  ANY,  0x49, 0x89, 0xfe, 0x48, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x8b, 0x04,
    0x24, 0x48, 0x89, 0x45, 0xd8, 0x49, 0x8b, 0x46, 0x28, 0x4c, 0x8b, 0x38,
};
/* The localize helper: lookup(language, key, 1) or lookup(key, 1); a found text goes to the string constructor. */
static const short kLookup[] = {
    0xba, 0x01, 0x00, 0x00, 0x00, 0x44, 0x89, 0xff, 0x44, 0x89, 0xc6, 0xe8, ANY,  ANY,  ANY,  ANY,  0xeb, 0x0d, 0xbe,
    0x01, 0x00, 0x00, 0x00, 0x44, 0x89, 0xc7, 0xe8, ANY,  ANY,  ANY,  ANY,  0x31, 0xc9, 0x48, 0x85, 0xc0, 0x87, 0x0d,
};

const OnlineStatusSite *online_status_site(const char *build) {
    for (size_t i = 0; build && i < sizeof(kSites) / sizeof(*kSites); ++i)
        if (!strcmp(build, kSites[i].build)) return &kSites[i];
    return NULL;
}

static int matches(const uint8_t *image, uint64_t size, uint64_t at, const short *pattern, size_t count) {
    if (at > size || size - at < count) return 0;
    for (size_t i = 0; i < count; ++i)
        if (pattern[i] != ANY && image[at + i] != (uint8_t)pattern[i]) return 0;
    return 1;
}

static uint64_t call_target(const uint8_t *image, uint32_t at) {
    int32_t rel;
    memcpy(&rel, image + at + 1, sizeof(rel));
    return (uint64_t)((int64_t)at + CALL_LENGTH + rel);
}

int online_status_verify(const uint8_t *image, uint64_t size, const OnlineStatusSite *site, const char **why) {
    if (!site) *why = "not a supported build";
    else if (!matches(image, size, site->auth_code, kAuthCode, sizeof(kAuthCode) / sizeof(*kAuthCode)))
        *why = "server_request_auth_code differs";
    else if (site->lookup_language < LOOKUP_LEAD || site->lookup_default != site->lookup_language + LOOKUP_DEFAULT_CALL ||
             !matches(image, size, site->lookup_language - LOOKUP_LEAD, kLookup, sizeof(kLookup) / sizeof(*kLookup)))
        *why = "the localization lookup differs";
    else if (call_target(image, site->lookup_language) >= size || call_target(image, site->lookup_default) >= size)
        *why = "the localization lookup calls leave the image";
    else return 1;
    return 0;
}

size_t online_status_stub_bytes(void) {
    return HOOKS * guest_hook_stub_size();
}

typedef const char *(GUEST_HOOK_ABI *LookupLanguage)(int32_t language, uint32_t key, int32_t flag);
typedef const char *(GUEST_HOOK_ABI *LookupDefault)(uint32_t key, int32_t flag);

static LookupLanguage g_lookup_language;
static LookupDefault g_lookup_default;
static int (*g_state)(void);

static void GUEST_HOOK_ABI lookup_language_hook(GuestRegs *regs) {
    const uint32_t key = (uint32_t)regs->rsi;
    const char *text = online_status_text(key, g_lookup_language((int32_t)regs->rdi, key, (int32_t)regs->rdx));
    regs->rax = (uint64_t)(uintptr_t)text;
    regs->skip = 1;
}

static void GUEST_HOOK_ABI lookup_default_hook(GuestRegs *regs) {
    const uint32_t key = (uint32_t)regs->rdi;
    const char *text = online_status_text(key, g_lookup_default(key, (int32_t)regs->rsi));
    regs->rax = (uint64_t)(uintptr_t)text;
    regs->skip = 1;
}

static void GUEST_HOOK_ABI auth_code_hook(GuestRegs *regs) {
    const uint8_t *context = (const uint8_t *)(uintptr_t)regs->rdi;
    const uint64_t *args;
    uint64_t *result;
    memcpy(&args, context + NATIVE_ARGS, sizeof(args));
    if ((args[0] & ~0xffffffffull) != VALUE_INT || (uint32_t)args[0] >> 8 != ONLINE_STATUS_STYLE_MAGIC >> 8) return;
    const int state = g_state ? g_state() : ONLINE_STATUS_OFF;
    const int answer = online_status_answer((uint32_t)args[0], state);
    if (answer < 0) return;
    memcpy(&result, context + NATIVE_RETURN, sizeof(result));
    *result = VALUE_INT | (uint32_t)answer;
    regs->skip = 1;
}

int online_status_install(uint8_t *image, uint64_t size, const char *build, int (*state)(void), uint8_t *stubs,
                          size_t stubs_size) {
    const char *why = "";
    const OnlineStatusSite *site = online_status_site(build);
    if (!online_status_verify(image, size, site, &why)) {
        printf("Online status: unavailable for %s: %s\n", build ? build : "an unknown game build", why);
        return 0;
    }
    g_lookup_language = (LookupLanguage)(void *)(image + call_target(image, site->lookup_language));
    g_lookup_default = (LookupDefault)(void *)(image + call_target(image, site->lookup_default));
    g_state = state;
    GuestHook hooks[HOOKS] = {
        {"localization lookup", site->lookup_language, CALL_LENGTH, {0}, lookup_language_hook},
        {"localization lookup", site->lookup_default, CALL_LENGTH, {0}, lookup_default_hook},
        {"server_request_auth_code", site->auth_code, PROLOGUE_LENGTH, {0}, auth_code_hook},
    };
    for (size_t i = 0; i < HOOKS; ++i) memcpy(hooks[i].original, image + hooks[i].offset, hooks[i].length);
    size_t bad;
    const GuestHookStatus status = guest_hooks_install(image, size, 0, NULL, hooks, HOOKS, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Online status: unavailable for %s: %s\n", build, guest_hook_status_text(status));
        return 0;
    }
    return 1;
}
