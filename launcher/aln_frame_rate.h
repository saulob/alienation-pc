/* The game's own frame rate target. */
#ifndef ALN_FRAME_RATE_H
#define ALN_FRAME_RATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Lua binding of engine_set_fps, verified for one executable. The engine starts at 59.94 FPS and a script that
 * calls the binding with 29.97 makes the display flip every second vblank; the replacement returns at once, so the
 * startup rate stays. */
typedef struct {
    uint32_t offset; /* image offset of the binding's entry */
    uint8_t original[4];
    uint8_t replacement[4];
} AlnFrameRatePatch;

/* Writes the replacement after checking the entry still holds the original code. Returns 0 and a message in `error`
 * when it does not; nothing is written then. */
int aln_apply_frame_rate(unsigned char *image, uint64_t image_size, const AlnFrameRatePatch *patch, char *error,
                         size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
