/* Issue #5: Issue5Diag stepper sites, hook and accounting on synthetic images built from the production fingerprints.
 * tests/patch_site_tests.py checks the same sites against the real executables. */
#include "stepper_diag.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAILED: %s (line %d)\n", #condition, __LINE__); ++failures; } } while (0)

enum { IMAGE_SIZE = 0x2800000, STUB_BYTES = 1024 };

static const char *kBuilds[] = {"CUSA00062-1.00", "CUSA00062-1.06", "CUSA04430-1.00"};

static void put_pattern(uint8_t *at, const char *pattern) {
    for (size_t i = 0; pattern[2 * i]; ++i) {
        unsigned value = 0;
        if (pattern[2 * i] != '?') sscanf(pattern + 2 * i, "%2x", &value);
        at[i] = (uint8_t)value;
    }
}

static int64_t call_target(const uint8_t *image, uint64_t at) {
    int32_t rel;
    memcpy(&rel, image + at + 1, sizeof(rel));
    return (int64_t)at + 5 + rel;
}

static uint8_t *make_image(const StepperDiagSite *site) {
    uint8_t *image = calloc(1, IMAGE_SIZE + STUB_BYTES);
    if (!image) exit(2);
    put_pattern(image + site->frame_update, stepper_diag_frame_update_pattern);
    put_pattern(image + site->stepper, stepper_diag_stepper_pattern);
    const int64_t accessor = call_target(image, site->stepper + 0x243);
    put_pattern(image + accessor, stepper_diag_state_accessor_pattern);
    const int32_t lea = (int32_t)((int64_t)site->state - (accessor + 0x47));
    memcpy(image + accessor + 0x43, &lea, sizeof(lea));
    const uint8_t set_steps[] = {0x89, 0x77, 0x04, 0xc3}, set_skipped[] = {0x89, 0x77, 0x08, 0xc3};
    memcpy(image + call_target(image, site->stepper + 0x24e), set_steps, sizeof(set_steps));
    memcpy(image + call_target(image, site->stepper + 0x25e), set_skipped, sizeof(set_skipped));
    return image;
}

static int verifies(const uint8_t *image, const StepperDiagSite *site, const char **why) {
    *why = "";
    return stepper_diag_verify(image, IMAGE_SIZE, site, why);
}

/* Each build's site resolves to its stepper; another build's offsets and changed code are refused. */
static void sites(void) {
    for (size_t b = 0; b < 3; ++b) {
        const StepperDiagSite *site = stepper_diag_site(kBuilds[b]);
        CHECK(site != NULL);
        if (!site) continue;
        CHECK(site->frame_update - site->stepper == 0x550);
        uint8_t *image = make_image(site);
        const char *why;
        CHECK(verifies(image, site, &why));
        for (size_t o = 0; o < 3; ++o)
            if (o != b) CHECK(!verifies(image, stepper_diag_site(kBuilds[o]), &why));

        struct {
            uint64_t at;
            uint8_t value;
            const char *why;
        } const mutations[] = {
            {site->frame_update + 0x58, 0x90, "frame update differs"},
            {site->frame_update + STEPPER_DIAG_HOOK, 0xe9, "frame update differs"},
            {site->stepper + 0x139, 0x90, "stepper differs"},
            {site->stepper + 0xb6, 0x90, "stepper differs"},
            {(uint64_t)call_target(image, site->stepper + 0x243) + 0x43, 0x11, "stepper state differs"},
            {(uint64_t)call_target(image, site->stepper + 0x24e) + 2, 0x08, "stepper counters differ"},
            {(uint64_t)call_target(image, site->stepper + 0x25e) + 2, 0x0c, "stepper counters differ"},
        };
        for (size_t m = 0; m < sizeof(mutations) / sizeof(*mutations); ++m) {
            const uint8_t original = image[mutations[m].at];
            image[mutations[m].at] = mutations[m].value;
            CHECK(!verifies(image, site, &why) && !strcmp(why, mutations[m].why));
            image[mutations[m].at] = original;
        }
        CHECK(verifies(image, site, &why));
        free(image);
    }
}

static void unsupported_build(void) {
    const char *why = "";
    CHECK(stepper_diag_site("CUSA00062-1.05") == NULL && stepper_diag_site(NULL) == NULL);
    CHECK(!stepper_diag_verify(NULL, 0, NULL, &why) && !strcmp(why, "not a supported build"));
    uint8_t *image = make_image(stepper_diag_site("CUSA00062-1.06"));
    uint8_t *copy = malloc(IMAGE_SIZE + STUB_BYTES);
    if (!copy) exit(2);
    memcpy(copy, image, IMAGE_SIZE + STUB_BYTES);
    CHECK(!stepper_diag_install(image, IMAGE_SIZE, "CUSA00062-1.05", NULL, image + IMAGE_SIZE, STUB_BYTES));
    CHECK(!stepper_diag_install(image, IMAGE_SIZE, "CUSA00062-1.00", "CUSA00062 1.00", image + IMAGE_SIZE, STUB_BYTES));
    CHECK(!memcmp(copy, image, IMAGE_SIZE + STUB_BYTES));
    stepper_diag_hook(&(GuestRegs){0});
    CHECK(stepper_diag_take().frames == 0);
    free(copy);
    free(image);
}

static int same_window(StepperWindow a, StepperWindow b) {
    return a.frames == b.frames && a.steps == b.steps && a.skipped == b.skipped &&
           a.skipping_frames == b.skipping_frames && a.rejected == b.rejected;
}

/* The hook reads this frame's steps and skips from the state object, the stepper's time from xmm0 and the step from
 * the frame update's configuration; the game's state is only read. */
static void hook(void) {
    const StepperDiagSite *site = stepper_diag_site("CUSA00062-1.06");
    uint8_t *image = make_image(site);
    CHECK(stepper_diag_stub_bytes() <= STUB_BYTES);
    CHECK(stepper_diag_install(image, IMAGE_SIZE, site->build, "CUSA00062 1.06", image + IMAGE_SIZE, STUB_BYTES));
    CHECK(image[site->frame_update + STEPPER_DIAG_HOOK] == 0xe9);

    float config[4] = {0.0f, 0.0f, 0.0f, 1.0f / 59.94f};
    uint64_t frame_update_object[2] = {0, (uint64_t)(uintptr_t)config};
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rbx = (uint64_t)(uintptr_t)frame_update_object;
    const struct {
        int32_t steps, skipped;
        float ms;
    } frames[] = {{1, 0, 9.5f}, {1, 1, 29.3f}, {1, 2, 41.8f}, {2, 0, 12.0f}};
    for (size_t i = 0; i < sizeof(frames) / sizeof(*frames); ++i) {
        memcpy(image + site->state + STEPPER_DIAG_STEPS, &frames[i].steps, 4);
        memcpy(image + site->state + STEPPER_DIAG_SKIPPED, &frames[i].skipped, 4);
        memcpy(regs.xmm[0], &frames[i].ms, sizeof(float));
        uint8_t state[12];
        memcpy(state, image + site->state, sizeof(state));
        GuestRegs after = regs;
        stepper_diag_hook(&after);
        CHECK(!memcmp(&after, &regs, sizeof(regs)) && !memcmp(state, image + site->state, sizeof(state)));
    }
    const StepperWindow window = stepper_diag_take();
    CHECK(window.frames == 4 && window.steps == 5 && window.skipped == 3 && window.skipping_frames == 2);
    CHECK(fabs(window.update_ms_max - 41.8) < 1e-4 && fabs(window.update_ms_sum - 92.6) < 1e-3);
    CHECK(fabsf(window.step_seconds - 1.0f / 59.94f) < 1e-9f);
    CHECK(fabs(stepper_window_game_speed(&window) - 62.5) < 1e-9);
    free(image);
}

/* Dropped steps are the stepper's own skips: a frame without skipped steps never adds any. */
static void accounting(void) {
    StepperWindow w = {0};
    stepper_window_add(&w, 1, 0, 10.0f, 1.0f / 59.94f);
    stepper_window_add(&w, 2, 0, 12.0f, 1.0f / 59.94f);
    CHECK(w.skipped == 0 && w.skipping_frames == 0 && stepper_window_game_speed(&w) == 100.0);
    stepper_window_add(&w, 1, 3, 30.0f, 1.0f / 59.94f);
    CHECK(w.skipped == 3 && w.skipping_frames == 1 && w.steps == 4 && w.frames == 3);

    StepperWindow bad = {0};
    stepper_window_add(&bad, 0, 0, 1.0f, 0.0f);
    stepper_window_add(&bad, 65, 0, 1.0f, 0.0f);
    stepper_window_add(&bad, 1, -1, 1.0f, 0.0f);
    stepper_window_add(&bad, 1, 0x7fffffff, 1.0f, 0.0f);
    stepper_window_add(&bad, 1, 0, NAN, 0.0f);
    stepper_window_add(&bad, 1, 0, -1.0f, 0.0f);
    stepper_window_add(&bad, 1, 0, 1e9f, 0.0f);
    CHECK(bad.frames == 0 && bad.steps == 0 && bad.rejected == 7);
    stepper_window_add(&bad, 1, 0, 5.0f, NAN);
    stepper_window_add(&bad, 1, 0, 5.0f, 2.0f);
    CHECK(bad.frames == 2 && bad.step_seconds == 0.0f);

    char line[320];
    CHECK(!stepper_window_format(&bad, 0.0, line, sizeof(line)));
    StepperWindow empty = {0};
    CHECK(!stepper_window_format(&empty, 60.0, line, sizeof(line)));
    StepperWindow slow = {0};
    for (int i = 0; i < 2040; ++i) stepper_window_add(&slow, 1, i % 34 < 26 ? 1 : 0, i == 7 ? 41.8f : 29.3f, 1.0f / 59.94f);
    CHECK(stepper_window_format(&slow, 60.0, line, sizeof(line)));
    CHECK(!strcmp(line, "Issue5Diag: game steps 34.0/s, target 59.94/s, game speed 56.7%, dropped 1560 steps in 1560 of "
                        "2040 frames, 1.00 steps a frame, stepper avg 29.3 ms, max 41.8 ms a frame"));
    printf("  %s\n", line);
}

/* Each report empties the window, so a restart or a stretch without frames (loading, the title) starts afresh; the
 * step stays known. */
static void windows(void) {
    stepper_diag_take();
    char line[320];
    for (int i = 0; i < 100; ++i) stepper_diag_record(1, 1, 30.0f, 1.0f / 59.94f);
    CHECK(stepper_diag_report(60.0, line, sizeof(line)));
    CHECK(!stepper_diag_report(60.0, line, sizeof(line)));
    stepper_diag_record(1, 0, 8.0f, 0.0f);
    const StepperWindow next = stepper_diag_take();
    CHECK(next.frames == 1 && next.skipped == 0 && fabsf(next.step_seconds - 1.0f / 59.94f) < 1e-9f);
    StepperWindow a = {0}, b = {0};
    for (int i = 0; i < 1000; ++i) {
        stepper_window_add(&a, 1 + i % 3, i % 5, 10.0f, 1.0f / 59.94f);
        stepper_diag_record(1 + i % 3, i % 5, 10.0f, 1.0f / 59.94f);
    }
    b = stepper_diag_take();
    CHECK(same_window(a, b));
}

int main(void) {
    sites();
    unsupported_build();
    hook();
    accounting();
    windows();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("stepper diagnostic tests passed\n");
    return 0;
}
