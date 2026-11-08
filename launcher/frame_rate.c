#include "aln_frame_rate.h"

#include <stdio.h>
#include <string.h>

int aln_apply_frame_rate(unsigned char *image, uint64_t image_size, const AlnFrameRatePatch *patch, char *error,
                         size_t error_size) {
    if ((uint64_t)patch->offset + sizeof(patch->original) > image_size ||
        memcmp(image + patch->offset, patch->original, sizeof(patch->original))) {
        snprintf(error, error_size, "frame rate patch: unexpected instruction at image offset 0x%x",
                 (unsigned)patch->offset);
        return 0;
    }
    memcpy(image + patch->offset, patch->replacement, sizeof(patch->replacement));
    return 1;
}
