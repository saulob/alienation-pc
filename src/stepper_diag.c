#include "stepper_diag.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* The three builds share this code byte for byte except the displacements of globals (??). */
const char stepper_diag_frame_update_pattern[] =
    "554889e5415741564154534883ec304c8b25????????4889fb4c8d73404c89f7498b0424488945d8e8????????e8eee2feff488d"
    "7328488d7dd0488945d0e88de3feffc5fa1143204c89f7488b45d048894328e8????????488b7b08c5fa1043204c8b7b18c5fa10"
    "4f0c488b07c5fa1145c4c5fa114dbcc5f2590d????????c5fa114dc0ff5040c5fa1045c4c5fa104dbcc5fa105dc04c89ff89c6e8"
    "10faffff4c89f7c5fa1145c4e8????????488d05????????803d????????004c8b30498b064c8b7810740ac5fa1005????????eb"
    "19488b7b08c5fa104320488b07c5fa1145c0ff5038c5fa5945c04c89f741ffd7e8????????c5fa1045c4c5fa5905????????c5fa"
    "1145c4e874d9ffff";
const char stepper_diag_stepper_pattern[] =
    "554889e54157415641554154534883ec78488b05????????89b57cffffffc5fa115d90c5fa114d94c5fa11458c4889fb488b0048"
    "8945d0e834e8feffc5fa104d8cc5f857c0803d????????00488945c841b601b001c5fa5fc1c5f057c9c5fa114d8c7411803d????"
    "????00c605????????000f95c0c5fa104d9048895d80884320488d5b184531e44531ed4531ff48899d70ffffffc5fa5dc1c5fa11"
    "856cffffffc5f857c066662e0f1f84000000000041f6c701740a41ffc4e9a7000000669044897d88c5fa1145904589e7e89fe7fe"
    "ff4c8b6580488945c0c5fa104594498b7c2408488b07ff500841807c2420007415488d05????????c5fa104594488b38488b07ff"
    "5018488d05????????4589fc488b384885ff740b488b07c5fa104594ff5058e848e7feff488d7db8488d75c0488945b8e8e7e7fe"
    "ffc5f82e4594448b7d88488b9d70ffffff0f96c0443bad7cffffff0f9cc184c8750341b701c5fa10459041ffc541f6c6017455e8"
    "00e7feff488945b0e8f7e6feff488d7da84889de488945a8e897e7feffc5fa114590e8dde6feff488d7da0488d75b0488945a0e8"
    "7ce7feffc5fa104d8cc5fa58c9c5fa114d8ceb116666662e0f1f840000000000c5fa114590c5fa1045944889dfc5fa114594e869"
    "e7feffc5fa104590c5fa104d944531f6c5fa5cc1c5f82ec10f83befeffff4c8b7580c5fa10856cffffff498b3e488b07ff500841"
    "807e20007418488d05????????c5fa10856cffffff488b38488b07ff5010488d05????????c5fa10856cffffff488b384885ff74"
    "06488b07ff5060e828ddffff4889c74489eee8cddcffffe818ddffff4889c74489e6e8dddcffff";
const char stepper_diag_state_accessor_pattern[] =
    "554889e58a05????????84c07532488d3d????????e8????????85c07422488d3d????????48c705????????0000000048c705????"
    "????00000000e8????????488d05????????5dc3";

enum {
    STEPPER_CALL = 0x9b,          /* frame update: call stepper */
    STATE_CALL = 0x243,           /* stepper: call state accessor; mov rdi,rax; mov esi,r13d; call set steps */
    STEPS_SETTER_CALL = 0x24e,
    SKIPPED_SETTER_CALL = 0x25e,  /* after the accessor again; mov esi,r12d */
    STATE_LEA = 0x40,             /* accessor: lea rax,[rip+state] */
    MAX_STEPS = 64,
    MAX_SKIPPED = 1 << 20,
};
static const uint8_t kSetSteps[] = {0x89, 0x77, STEPPER_DIAG_STEPS, 0xc3};
static const uint8_t kSetSkipped[] = {0x89, 0x77, STEPPER_DIAG_SKIPPED, 0xc3};
static const float kMaxUpdateMs = 60000.0f;

static const StepperDiagSite kSites[] = {
    {"CUSA00062-1.00", 0xf20e0, 0xf1b90, 0x27943c0},
    {"CUSA00062-1.06", 0xf4e40, 0xf48f0, 0x27911e0},
    {"CUSA04430-1.00", 0xf2ca0, 0xf2750, 0x2788630},
};

const StepperDiagSite *stepper_diag_site(const char *build) {
    for (size_t i = 0; build && i < sizeof(kSites) / sizeof(*kSites); ++i)
        if (!strcmp(build, kSites[i].build)) return &kSites[i];
    return NULL;
}

static int hex_value(char c) {
    return c <= '9' ? c - '0' : c - 'a' + 10;
}

static int matches(const uint8_t *image, uint64_t size, uint64_t at, const char *pattern) {
    const size_t count = strlen(pattern) / 2;
    if (at > size || size - at < count) return 0;
    for (size_t i = 0; i < count; ++i) {
        const char *pair = pattern + 2 * i;
        if (pair[0] != '?' && image[at + i] != (uint8_t)(hex_value(pair[0]) << 4 | hex_value(pair[1]))) return 0;
    }
    return 1;
}

static int64_t rel32_target(const uint8_t *image, uint64_t at, unsigned length) {
    int32_t value;
    memcpy(&value, image + at + length - 4, sizeof(value));
    return (int64_t)at + length + value;
}

static int bytes_at(const uint8_t *image, uint64_t size, int64_t at, const uint8_t *bytes, size_t count) {
    return at >= 0 && (uint64_t)at <= size && size - (uint64_t)at >= count && !memcmp(image + at, bytes, count);
}

static int64_t state_of(const uint8_t *image, uint64_t size, int64_t accessor) {
    if (accessor < 0 || !matches(image, size, (uint64_t)accessor, stepper_diag_state_accessor_pattern)) return -1;
    return rel32_target(image, (uint64_t)accessor + STATE_LEA, 7);
}

int stepper_diag_verify(const uint8_t *image, uint64_t size, const StepperDiagSite *site, const char **why) {
    if (!site) {
        *why = "not a supported build";
        return 0;
    }
    if (!matches(image, size, site->frame_update, stepper_diag_frame_update_pattern)) {
        *why = "frame update differs";
        return 0;
    }
    if (rel32_target(image, site->frame_update + STEPPER_CALL, 5) != site->stepper ||
        !matches(image, size, site->stepper, stepper_diag_stepper_pattern)) {
        *why = "stepper differs";
        return 0;
    }
    const int64_t accessor = rel32_target(image, site->stepper + STATE_CALL, 5);
    if (state_of(image, size, accessor) != site->state ||
        rel32_target(image, site->stepper + SKIPPED_SETTER_CALL - 11, 5) != accessor) {
        *why = "stepper state differs";
        return 0;
    }
    const int64_t state_end = (int64_t)site->state + STEPPER_DIAG_SKIPPED + 4;
    if (!bytes_at(image, size, rel32_target(image, site->stepper + STEPS_SETTER_CALL, 5), kSetSteps, sizeof(kSetSteps)) ||
        !bytes_at(image, size, rel32_target(image, site->stepper + SKIPPED_SETTER_CALL, 5), kSetSkipped,
                  sizeof(kSetSkipped)) ||
        (uint64_t)state_end > size) {
        *why = "stepper counters differ";
        return 0;
    }
    return 1;
}

size_t stepper_diag_stub_bytes(void) {
    return guest_hook_stub_size();
}

void stepper_window_add(StepperWindow *window, int32_t steps, int32_t skipped, float update_ms, float step_seconds) {
    if (steps < 1 || steps > MAX_STEPS || skipped < 0 || skipped > MAX_SKIPPED || !isfinite(update_ms) ||
        update_ms < 0.0f || update_ms > kMaxUpdateMs) {
        ++window->rejected;
        return;
    }
    ++window->frames;
    window->steps += (uint64_t)steps;
    window->skipped += (uint64_t)skipped;
    window->skipping_frames += skipped > 0;
    window->update_ms_sum += update_ms;
    if (update_ms > window->update_ms_max) window->update_ms_max = update_ms;
    if (isfinite(step_seconds) && step_seconds > 0.0f && step_seconds < 1.0f) window->step_seconds = step_seconds;
}

double stepper_window_game_speed(const StepperWindow *window) {
    const uint64_t elapsed = window->steps + window->skipped;
    return elapsed ? 100.0 * (double)window->steps / (double)elapsed : 0.0;
}

int stepper_window_format(const StepperWindow *window, double seconds, char *line, size_t size) {
    if (!window->frames || seconds <= 0.0 || !size) return 0;
    const double target = window->step_seconds > 0.0f ? 1.0 / window->step_seconds : 0.0;
    snprintf(line, size,
             "Issue5Diag: game steps %.1f/s, target %.2f/s, game speed %.1f%%, dropped %llu steps in %llu of %llu "
             "frames, %.2f steps a frame, stepper avg %.1f ms, max %.1f ms a frame",
             (double)window->steps / seconds, target, stepper_window_game_speed(window),
             (unsigned long long)window->skipped, (unsigned long long)window->skipping_frames,
             (unsigned long long)window->frames, (double)window->steps / (double)window->frames,
             window->update_ms_sum / (double)window->frames, window->update_ms_max);
    return 1;
}

/* The hook runs on the game's thread, the reporter on the vblank thread. */
static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static StepperWindow g_window;
static const uint8_t *g_state;

static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {
    }
}

static void unlock(void) {
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}

void stepper_diag_record(int32_t steps, int32_t skipped, float update_ms, float step_seconds) {
    lock();
    stepper_window_add(&g_window, steps, skipped, update_ms, step_seconds);
    unlock();
}

StepperWindow stepper_diag_take(void) {
    lock();
    const StepperWindow taken = g_window;
    const float step_seconds = g_window.step_seconds;
    memset(&g_window, 0, sizeof(g_window));
    g_window.step_seconds = step_seconds;
    unlock();
    return taken;
}

int stepper_diag_report(double seconds, char *line, size_t size) {
    const StepperWindow window = stepper_diag_take();
    return stepper_window_format(&window, seconds, line, size);
}

/* At the frame update's call to the timeline accessor: the stepper has just returned, xmm0 holds its time in
 * milliseconds and rbx is the frame update's object, whose +8 holds the step configuration (+0xc: 1 / fps). */
void GUEST_HOOK_ABI stepper_diag_hook(GuestRegs *regs) {
    if (!g_state) return;
    int32_t steps, skipped;
    float update_ms, step_seconds = 0.0f;
    uint64_t config;
    memcpy(&steps, g_state + STEPPER_DIAG_STEPS, sizeof(steps));
    memcpy(&skipped, g_state + STEPPER_DIAG_SKIPPED, sizeof(skipped));
    memcpy(&update_ms, regs->xmm[0], sizeof(update_ms));
    memcpy(&config, (const void *)(uintptr_t)(regs->rbx + 8), sizeof(config));
    if (config) memcpy(&step_seconds, (const void *)(uintptr_t)(config + 0xc), sizeof(step_seconds));
    stepper_diag_record(steps, skipped, update_ms, step_seconds);
}

int stepper_diag_install(uint8_t *image, uint64_t size, const char *build, const char *label, uint8_t *stubs,
                         size_t stubs_size) {
    const char *why = "";
    const StepperDiagSite *site = stepper_diag_site(build);
    const char *name = label ? label : build ? build : "an unknown game build";
    if (!stepper_diag_verify(image, size, site, &why)) {
        printf("Issue5Diag: stepper unavailable for %s: %s\n", name, why);
        return 0;
    }
    GuestHook hook = {"frame update: stepper time", site->frame_update + STEPPER_DIAG_HOOK, STEPPER_DIAG_HOOK_LENGTH,
                      {0}, stepper_diag_hook};
    memcpy(hook.original, image + hook.offset, hook.length);
    size_t bad;
    const GuestHookStatus status = guest_hooks_install(image, size, 0, NULL, &hook, 1, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Issue5Diag: stepper unavailable for %s: %s\n", name, guest_hook_status_text(status));
        return 0;
    }
    g_state = image + site->state;
    printf("Issue5Diag: stepper source %s\n", name);
    return 1;
}
