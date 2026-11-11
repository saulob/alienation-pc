#include "skip_intro.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define ANY (-1)
#define CROSS 13
#define PRESS_UPDATES 2
#define HOOK_IS_DOWN_LENGTH 5
#define HOOK_UPDATE_LENGTH 10
#define HANDLE_OFFSET 0x10

static const short kWasPressed[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0x41, 0x89, 0xf6, 0x48, 0x8b, 0x03, 0xff, 0x50, 0x28,
    0x84, 0xc0, 0x74, 0x0f, 0x48, 0x89, 0xdf, 0x44, 0x89, 0xf6, 0xe8, ANY, ANY, ANY, ANY, 0x34, 0x01, 0xeb, 0x02,
    0x31, 0xc0, 0x5b, 0x41, 0x5e, 0x5d, 0xc3,
};
enum { kWasPressedCount = sizeof(kWasPressed) / sizeof(*kWasPressed), kPreviousDownCall = 29 };

static const short kUpdate[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0x8b, 0x7b, 0x10, 0x85, 0xff, 0x78, ANY, 0xc5, 0xfc,
    0x10, 0x43, 0x18, 0xc5, 0xfc, 0x10, 0x4b, 0x38, 0xc5, 0xfc, 0x10, 0x53, 0x58, 0xc5, 0xfc, 0x10, 0x5b, 0x70,
};
enum { kUpdateCount = sizeof(kUpdate) / sizeof(*kUpdate) };

/* Both state tests start with this dispatch on the button index; Cross is entry 13 of its jump table. */
static const short kButtonDispatch[] = {
    0x89, 0xf0, 0x83, 0xfe, 0x0f, 0x76, 0x03, 0x31, 0xc0, 0xc3, 0x48, 0x8d, 0x0d, ANY, ANY, ANY, ANY,
};
enum { kButtonDispatchCount = sizeof(kButtonDispatch) / sizeof(*kButtonDispatch), kTableLea = 10, kTableEntries = 16 };

static const uint8_t kCrossNow[] = {0x8a, 0x47, 0x19, 0x24, 0x40, 0xc0, 0xe8, 0x06, 0xc3};
static const uint8_t kCrossBefore[] = {0x8a, 0x87, 0x91, 0x00, 0x00, 0x00, 0x24, 0x40, 0xc0, 0xe8, 0x06, 0xc3};

static const SkipIntroSite kSites[] = {
    {"CUSA00062-1.00", 0x129f960, 0x129fb40, 0x129fd20, 0x1f5dc78},
    {"CUSA00062-1.06", 0x12a4ae0, 0x12a4cc0, 0x12a4ea0, 0x1f59d08},
    {"CUSA04430-1.00", 0x12a0be0, 0x12a0dc0, 0x12a0fa0, 0x1f51c58},
};

const SkipIntroSite *skip_intro_site(const char *build) {
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

static int bytes_at(const uint8_t *image, uint64_t size, int64_t at, const uint8_t *bytes, size_t count) {
    return at >= 0 && (uint64_t)at <= size && size - (uint64_t)at >= count && !memcmp(image + at, bytes, count);
}

static int32_t rel32_at(const uint8_t *image, uint64_t at) {
    int32_t value;
    memcpy(&value, image + at, sizeof(value));
    return value;
}

static int64_t cross_entry(const uint8_t *image, uint64_t size, int64_t function) {
    if (function < 0 || !matches(image, size, (uint64_t)function, kButtonDispatch, kButtonDispatchCount)) return -1;
    const int64_t table = function + kTableLea + 7 + rel32_at(image, (uint64_t)function + kTableLea + 3);
    if (table < 0 || (uint64_t)table > size || size - (uint64_t)table < kTableEntries * 4) return -1;
    return table + rel32_at(image, (uint64_t)table + CROSS * 4);
}

static int64_t previous_down(const uint8_t *image, uint64_t was_pressed) {
    return (int64_t)was_pressed + kPreviousDownCall + 5 + rel32_at(image, was_pressed + kPreviousDownCall + 1);
}

static int occurs_once(const uint8_t *image, uint64_t size, const short *pattern, size_t count, uint64_t expected) {
    int found = 0;
    for (uint64_t at = 0; at + count <= size; ++at) {
        if (image[at] != (uint8_t)pattern[0] || !matches(image, size, at, pattern, count)) continue;
        if (at != expected || found++) return 0;
    }
    return found == 1;
}

static int slot_holds(const uint8_t *image, uint64_t size, uint64_t slot, uint32_t function) {
    uint64_t value;
    if (slot > size || size - slot < sizeof(value)) return 0;
    memcpy(&value, image + slot, sizeof(value));
    return value == (uint64_t)(uintptr_t)image + function;
}

int skip_intro_verify(const uint8_t *image, uint64_t size, const SkipIntroSite *site, SkipIntroHooks *hooks,
                      const char **why) {
    const char *reason;
    hooks->is_down = hooks->update = 0;
    if (!site) {
        *why = "not a supported build";
        return 0;
    }
    if (!matches(image, size, site->was_pressed, kWasPressed, kWasPressedCount))
        reason = "wasPressed differs";
    else if (!bytes_at(image, size, cross_entry(image, size, previous_down(image, site->was_pressed)), kCrossBefore,
                       sizeof(kCrossBefore)))
        reason = "previous-state test differs";
    else if (!bytes_at(image, size, cross_entry(image, size, site->is_down), kCrossNow, sizeof(kCrossNow)))
        reason = "isDown differs";
    else if (!slot_holds(image, size, site->is_down_slot, site->is_down) ||
             !slot_holds(image, size, site->is_down_slot + 0x10, site->was_pressed))
        reason = "isDown is not the controller class's slot";
    else if (!matches(image, size, site->update, kUpdate, kUpdateCount))
        reason = "update differs";
    else if (!occurs_once(image, size, kWasPressed, kWasPressedCount, site->was_pressed) ||
             !occurs_once(image, size, kUpdate, kUpdateCount, site->update))
        reason = "controller class is not unique";
    else {
        hooks->is_down = site->is_down;
        hooks->update = site->update;
        return 1;
    }
    *why = reason;
    return 0;
}

size_t skip_intro_stub_bytes(void) {
    return 2 * guest_hook_stub_size();
}

enum { OFF, WAITING, LOADING, ARMED, PRESSING, SPENT };

static atomic_int g_state = OFF;
static atomic_uintptr_t g_player;
static atomic_uintptr_t g_splash;
static atomic_uintptr_t g_controller;
static atomic_int g_press_updates;

static const char *file_name(const char *path) {
    const char *name = path;
    for (const char *c = path; *c; ++c)
        if (*c == '/' || *c == '\\') name = c + 1;
    return name;
}

int skip_intro_video_added(const void *player, const char *path) {
    if (atomic_load(&g_state) == OFF || !path) return 0;
    const char *name = file_name(path);
    if (!strcmp(name, "intro.mp4")) {
        atomic_store(&g_player, (uintptr_t)player);
        atomic_store(&g_state, LOADING);
    } else if (!strncmp(name, "splash", 6)) {
        if (atomic_load(&g_state) != WAITING) return 0;
        atomic_store(&g_splash, (uintptr_t)player);
        return 1;
    } else {
        atomic_store(&g_splash, 0);
        atomic_store(&g_state, OFF);
        puts("Skip Intro: title reached, skip finished");
    }
    return 0;
}

int skip_intro_video_frame(const void *player) {
    uintptr_t splash = (uintptr_t)player;
    if (atomic_load(&g_splash) == splash && atomic_compare_exchange_strong(&g_splash, &splash, 0)) {
        puts("Skip Intro: splash hidden, ending it at the first frame");
        return 1;
    }
    int expected = LOADING;
    if (atomic_load(&g_state) == LOADING && atomic_load(&g_player) == (uintptr_t)player &&
        atomic_compare_exchange_strong(&g_state, &expected, ARMED))
        puts("Skip Intro: intro playing, waiting for the next controller update");
    return 0;
}

void skip_intro_video_closed(const void *player) {
    uintptr_t splash = (uintptr_t)player;
    atomic_compare_exchange_strong(&g_splash, &splash, 0);
    const int state = atomic_load(&g_state);
    if (state < LOADING || atomic_load(&g_player) != (uintptr_t)player) return;
    if (state == ARMED) puts("Skip Intro: intro ended before the skip was sent");
    atomic_store(&g_player, 0);
    atomic_store(&g_state, WAITING);
}

void GUEST_HOOK_ABI skip_intro_update_hook(GuestRegs *regs) {
    const int state = atomic_load(&g_state);
    if (state != ARMED && state != PRESSING) return;
    const uintptr_t controller = (uintptr_t)regs->rdi;
    if (state == ARMED) {
        int32_t handle;
        memcpy(&handle, (const void *)(controller + HANDLE_OFFSET), sizeof(handle));
        int expected = ARMED;
        if (handle < 0) return;
        atomic_store(&g_controller, controller);
        atomic_store(&g_press_updates, 0);
        if (atomic_compare_exchange_strong(&g_state, &expected, PRESSING)) puts("Skip Intro: Cross tap sent to the intro");
    } else if (controller == atomic_load(&g_controller) && atomic_fetch_add(&g_press_updates, 1) + 1 >= PRESS_UPDATES) {
        int expected = PRESSING;
        atomic_compare_exchange_strong(&g_state, &expected, SPENT);
    }
}

void GUEST_HOOK_ABI skip_intro_is_down_hook(GuestRegs *regs) {
    if (atomic_load_explicit(&g_state, memory_order_relaxed) != PRESSING || (uint32_t)regs->rsi != CROSS ||
        (uintptr_t)regs->rdi != atomic_load(&g_controller))
        return;
    regs->rax = 1;
    regs->skip = 1;
}

static int unavailable(const char *build, const char *reason) {
    printf("Skip Intro: requested but unavailable for %s: %s\n", build ? build : "an unknown game build", reason);
    return 0;
}

int skip_intro_install(uint8_t *image, uint64_t size, const char *build, uint8_t *stubs, size_t stubs_size) {
    const char *why = "";
    SkipIntroHooks sites;
    if (!skip_intro_verify(image, size, skip_intro_site(build), &sites, &why)) return unavailable(build, why);
    GuestHook hooks[2] = {
        {"Controller_Orbis::isDown", sites.is_down, HOOK_IS_DOWN_LENGTH, {0}, skip_intro_is_down_hook},
        {"Controller_Orbis::update", sites.update, HOOK_UPDATE_LENGTH, {0}, skip_intro_update_hook},
    };
    for (size_t i = 0; i < 2; ++i) memcpy(hooks[i].original, image + hooks[i].offset, hooks[i].length);
    size_t bad;
    const GuestHookStatus status = guest_hooks_install(image, size, 0, NULL, hooks, 2, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) return unavailable(build, guest_hook_status_text(status));
    atomic_store(&g_state, WAITING);
    puts("Skip Intro: enabled, intro video will be skipped");
    return 1;
}
