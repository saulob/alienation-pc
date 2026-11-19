/* Issue5Diag: the game's own fixed-step accounting, observed once a frame where the frame update stores the stepper's
 * time. The stepper leaves the steps it ran (+4) and the steps of lag it discarded (+8) in its state object; nothing in
 * the game is changed. */
#ifndef ALN_STEPPER_DIAG_H
#define ALN_STEPPER_DIAG_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *build;     /* "CUSA00062-1.06" */
    uint32_t frame_update; /* calls the stepper, then stores its time in milliseconds */
    uint32_t stepper;
    uint32_t state;        /* int32 +4: steps run this frame, +8: steps skipped this frame */
} StepperDiagSite;

enum {
    STEPPER_DIAG_HOOK = 0x107, /* frame update: call to the timeline accessor, xmm0 = the stepper's milliseconds */
    STEPPER_DIAG_HOOK_LENGTH = 5,
    STEPPER_DIAG_STEPS = 4,
    STEPPER_DIAG_SKIPPED = 8,
};

/* Hex with ?? for the displacements of globals: the same bytes in every supported build. */
extern const char stepper_diag_frame_update_pattern[];
extern const char stepper_diag_stepper_pattern[];
extern const char stepper_diag_state_accessor_pattern[];

const StepperDiagSite *stepper_diag_site(const char *build);

/* Checks the frame update, the stepper, its state accessor and setters against the build's fingerprints. 0 with *why
 * naming the failed check otherwise. */
int stepper_diag_verify(const uint8_t *image, uint64_t size, const StepperDiagSite *site, const char **why);

size_t stepper_diag_stub_bytes(void);

/* Hooks the frame update and prints the source build once; prints why and returns 0, writing nothing, otherwise. */
int stepper_diag_install(uint8_t *image, uint64_t size, const char *build, const char *label, uint8_t *stubs,
                         size_t stubs_size);

typedef struct {
    uint64_t frames, steps, skipped, skipping_frames, rejected;
    double update_ms_sum, update_ms_max;
    float step_seconds;
} StepperWindow;

/* One frame of the stepper; a frame whose values cannot come from the stepper is only counted as rejected. */
void stepper_window_add(StepperWindow *window, int32_t steps, int32_t skipped, float update_ms, float step_seconds);

/* Steps run of all the steps the engine's clock advanced by, in percent; 0 without frames. */
double stepper_window_game_speed(const StepperWindow *window);

/* The Issue5Diag line for `seconds` of wall time; 0 and nothing written without frames. */
int stepper_window_format(const StepperWindow *window, double seconds, char *line, size_t size);

/* The live window: filled by the hook on the game's thread, taken (and emptied) by the reporter. */
void stepper_diag_record(int32_t steps, int32_t skipped, float update_ms, float step_seconds);
StepperWindow stepper_diag_take(void);
int stepper_diag_report(double seconds, char *line, size_t size);

void GUEST_HOOK_ABI stepper_diag_hook(GuestRegs *regs);

#ifdef __cplusplus
}
#endif

#endif
