#include "skip_intro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAILED: %s (line %d)\n", #condition, __LINE__); ++failures; } } while (0)

enum { IMAGE_SIZE = 0x2100000, STUB_BYTES = 1024, CROSS = 13 };

static const uint8_t was_pressed[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0x41, 0x89, 0xf6, 0x48, 0x8b, 0x03, 0xff, 0x50, 0x28,
    0x84, 0xc0, 0x74, 0x0f, 0x48, 0x89, 0xdf, 0x44, 0x89, 0xf6, 0xe8, 0, 0, 0, 0, 0x34, 0x01, 0xeb, 0x02,
    0x31, 0xc0, 0x5b, 0x41, 0x5e, 0x5d, 0xc3,
};
static const uint8_t update[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0x8b, 0x7b, 0x10, 0x85, 0xff, 0x78, 0x73, 0xc5, 0xfc,
    0x10, 0x43, 0x18, 0xc5, 0xfc, 0x10, 0x4b, 0x38, 0xc5, 0xfc, 0x10, 0x53, 0x58, 0xc5, 0xfc, 0x10, 0x5b, 0x70,
};
static const uint8_t dispatch[] = {0x89, 0xf0, 0x83, 0xfe, 0x0f, 0x76, 0x03, 0x31, 0xc0, 0xc3, 0x48, 0x8d, 0x0d};
static const uint8_t cross_now[] = {0x8a, 0x47, 0x19, 0x24, 0x40, 0xc0, 0xe8, 0x06, 0xc3};
static const uint8_t cross_before[] = {0x8a, 0x87, 0x91, 0x00, 0x00, 0x00, 0x24, 0x40, 0xc0, 0xe8, 0x06, 0xc3};

static void put_rel32(uint8_t *at, int64_t target, int64_t next) {
    const int32_t value = (int32_t)(target - next);
    memcpy(at, &value, sizeof(value));
}

static void put_state_test(uint8_t *image, uint32_t function, const uint8_t *cross, size_t cross_size) {
    const uint32_t table = function + 0x40, entry = function + 0x80;
    memcpy(image + function, dispatch, sizeof(dispatch));
    put_rel32(image + function + 13, table, function + 17);
    for (int i = 0; i < 16; ++i) put_rel32(image + table + i * 4, entry, table);
    memcpy(image + entry, cross, cross_size);
}

static uint8_t *make_image(const SkipIntroSite *site) {
    uint8_t *block = calloc(1, IMAGE_SIZE + STUB_BYTES);
    const uint32_t helper = site->was_pressed + 0x400;
    if (!block) exit(2);
    memcpy(block + site->was_pressed, was_pressed, sizeof(was_pressed));
    put_rel32(block + site->was_pressed + 30, helper, site->was_pressed + 34);
    put_state_test(block, helper, cross_before, sizeof(cross_before));
    put_state_test(block, site->is_down, cross_now, sizeof(cross_now));
    memcpy(block + site->update, update, sizeof(update));
    const uint64_t down = (uint64_t)(uintptr_t)block + site->is_down;
    const uint64_t pressed = (uint64_t)(uintptr_t)block + site->was_pressed;
    memcpy(block + site->is_down_slot, &down, 8);
    memcpy(block + site->is_down_slot + 0x10, &pressed, 8);
    return block;
}

static void verification(const SkipIntroSite *site) {
    SkipIntroHooks hooks;
    const char *why = "";
    uint8_t *image = make_image(site);
    CHECK(skip_intro_verify(image, IMAGE_SIZE, site, &hooks, &why));
    CHECK(hooks.is_down == site->is_down && hooks.update == site->update);

    image[site->was_pressed + 18] = 0x30;
    CHECK(!skip_intro_verify(image, IMAGE_SIZE, site, &hooks, &why) && hooks.is_down == 0);
    image[site->was_pressed + 18] = 0x28;

    image[site->is_down + 0x80 + 4] = 0x80;
    CHECK(!skip_intro_verify(image, IMAGE_SIZE, site, &hooks, &why));
    image[site->is_down + 0x80 + 4] = 0x40;

    image[site->was_pressed + 0x400 + 0x80 + 7] = 0x80;
    CHECK(!skip_intro_verify(image, IMAGE_SIZE, site, &hooks, &why));
    image[site->was_pressed + 0x400 + 0x80 + 7] = 0x40;

    image[site->is_down_slot + 0x10] ^= 1;
    CHECK(!skip_intro_verify(image, IMAGE_SIZE, site, &hooks, &why));
    image[site->is_down_slot + 0x10] ^= 1;

    image[site->update + 19] = 0x11;
    CHECK(!skip_intro_verify(image, IMAGE_SIZE, site, &hooks, &why));
    image[site->update + 19] = 0x10;

    memcpy(image + 0x1000, update, sizeof(update));
    CHECK(!skip_intro_verify(image, IMAGE_SIZE, site, &hooks, &why));
    memset(image + 0x1000, 0, sizeof(update));

    CHECK(skip_intro_verify(image, IMAGE_SIZE, site, &hooks, &why));
    CHECK(!skip_intro_verify(image, site->update + 8, site, &hooks, &why));
    CHECK(!skip_intro_verify(image, IMAGE_SIZE, NULL, &hooks, &why));
    free(image);
}

static void installation(const char *build) {
    const SkipIntroSite *site = skip_intro_site(build);
    uint8_t *image = make_image(site);
    uint8_t *stubs = image + IMAGE_SIZE;
    uint8_t *before = malloc(IMAGE_SIZE);
    if (!before) exit(2);
    memcpy(before, image, IMAGE_SIZE);

    CHECK(skip_intro_stub_bytes() <= STUB_BYTES);
    CHECK(!skip_intro_install(image, IMAGE_SIZE, build, stubs, 8));
    CHECK(!skip_intro_install(image, IMAGE_SIZE, "CUSA00000-1.00", stubs, STUB_BYTES));
    CHECK(!memcmp(image, before, IMAGE_SIZE));

    CHECK(skip_intro_install(image, IMAGE_SIZE, build, stubs, STUB_BYTES));
    CHECK(image[site->is_down] == 0xe9 && image[site->update] == 0xe9);
    size_t changed = 0;
    for (size_t i = 0; i < IMAGE_SIZE; ++i) changed += image[i] != before[i];
    CHECK(changed <= 15);
    CHECK(!skip_intro_install(image, IMAGE_SIZE, build, stubs, STUB_BYTES));
    free(before);
    free(image);
}

static void off_by_default(void) {
    int player = 0;
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rsi = CROSS;
    CHECK(skip_intro_video_added(&player, "/app0/video/splash.mp4") == 0);
    CHECK(skip_intro_video_frame(&player) == 0);
    CHECK(skip_intro_video_added(&player, "/app0/video/intro.mp4") == 0);
    CHECK(skip_intro_video_frame(&player) == 0);
    skip_intro_update_hook(&regs);
    skip_intro_is_down_hook(&regs);
    CHECK(regs.skip == 0 && regs.rax == 0);
}

typedef int (__attribute__((sysv_abi)) *IsDown)(void *controller, unsigned button);

/* The shape of isDown's entry: out-of-range buttons return 0, the rest 7. */
static const uint8_t tiny_is_down[] = {0x89, 0xf0, 0x83, 0xfe, 0x0f, 0x76, 0x03, 0x31, 0xc0, 0xc3, 0xb8, 7, 0, 0, 0, 0xc3};

static uint8_t *executable_block(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

/* Runs after the installation tests, which leave the tracking waiting for a video. */
static void press_sequence(void) {
    uint8_t *block = executable_block(16384);
    if (!block) {
        puts("skipped the stub execution test: no executable memory");
        return;
    }
    memcpy(block + 0x100, tiny_is_down, sizeof(tiny_is_down));
    GuestHook hook = {"isDown", 0x100, 5, {0}, skip_intro_is_down_hook};
    memcpy(hook.original, block + 0x100, 5);
    size_t bad;
    CHECK(guest_hooks_install(block, 8192, 0, NULL, &hook, 1, block + 8192, 8192, &bad) == GUEST_HOOK_OK);
    IsDown is_down = (IsDown)(block + 0x100);

    int32_t pad[8] = {0}, other[8] = {0};
    pad[4] = 3;
    other[4] = -1;
    int player = 0, other_player = 0;
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)pad;

    CHECK(is_down(pad, CROSS) == 7);
    CHECK(skip_intro_video_added(&player, "/app0/video/splash.mp4") == 1);
    CHECK(skip_intro_video_frame(&other_player) == 0);
    CHECK(skip_intro_video_frame(&player) == 1);
    CHECK(skip_intro_video_frame(&player) == 0);
    skip_intro_update_hook(&regs);
    CHECK(is_down(pad, CROSS) == 7);
    CHECK(skip_intro_video_added(&player, "/app0/video/splash_SIE.mp4") == 1);
    skip_intro_video_closed(&player);
    CHECK(skip_intro_video_frame(&player) == 0);
    CHECK(skip_intro_video_added(&player, "/app0/video/splash_SIE.mp4") == 1);
    CHECK(skip_intro_video_frame(&player) == 1);
    skip_intro_video_closed(&player);
    skip_intro_video_added(&player, "/app0/video/intro.mp4");
    skip_intro_update_hook(&regs);
    CHECK(is_down(pad, CROSS) == 7);
    skip_intro_video_frame(&other_player);
    skip_intro_update_hook(&regs);
    CHECK(is_down(pad, CROSS) == 7);

    skip_intro_video_frame(&player);
    regs.rdi = (uint64_t)(uintptr_t)other;
    skip_intro_update_hook(&regs);
    CHECK(is_down(pad, CROSS) == 7);
    regs.rdi = (uint64_t)(uintptr_t)pad;
    skip_intro_update_hook(&regs);
    CHECK(is_down(pad, CROSS) == 1);
    CHECK(is_down(pad, CROSS - 1) == 7);
    CHECK(is_down(pad, 99) == 0);
    CHECK(is_down(other, CROSS) == 7);
    regs.rdi = (uint64_t)(uintptr_t)other;
    skip_intro_update_hook(&regs);
    CHECK(is_down(pad, CROSS) == 1);
    regs.rdi = (uint64_t)(uintptr_t)pad;
    skip_intro_update_hook(&regs);
    CHECK(is_down(pad, CROSS) == 1);
    skip_intro_update_hook(&regs);
    CHECK(is_down(pad, CROSS) == 7);
    skip_intro_update_hook(&regs);
    CHECK(is_down(pad, CROSS) == 7);

    skip_intro_video_closed(&player);
    skip_intro_video_added(&player, "/app0/video/title.mp4");
    CHECK(skip_intro_video_added(&player, "/app0/video/splash.mp4") == 0);
    CHECK(skip_intro_video_frame(&player) == 0);
    skip_intro_video_added(&player, "/app0/video/intro.mp4");
    skip_intro_video_frame(&player);
    skip_intro_update_hook(&regs);
    CHECK(is_down(pad, CROSS) == 7);
}

int main(void) {
    off_by_default();
    verification(skip_intro_site("CUSA00062-1.00"));
    verification(skip_intro_site("CUSA00062-1.06"));
    verification(skip_intro_site("CUSA04430-1.00"));
    CHECK(skip_intro_site("CUSA04181-1.00") == NULL && skip_intro_site(NULL) == NULL);
    CHECK(skip_intro_site("CUSA00062-1.06")->is_down != skip_intro_site("CUSA00062-1.00")->is_down);
    installation("CUSA00062-1.00");
    installation("CUSA00062-1.06");
    installation("CUSA04430-1.00");
    press_sequence();
    if (!failures) puts("skip intro tests passed");
    return failures ? 1 : 0;
}
