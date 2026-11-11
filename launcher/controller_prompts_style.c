#include "controller_prompts.h"

#include <string.h>

const char *AlnPromptStyleKey(int style) {
    return style == ALN_PROMPTS_XBOX ? ALN_PROMPTS_XBOX_KEY : ALN_PROMPTS_PLAYSTATION_KEY;
}

const char *AlnPromptStyleLabel(int style) {
    return style == ALN_PROMPTS_XBOX ? "Xbox" : "PlayStation (Original)";
}

int AlnParsePromptStyle(const char *key, int *style) {
    if (!key) {
        return 0;
    }
    if (!strcmp(key, ALN_PROMPTS_PLAYSTATION_KEY)) {
        if (style) {
            *style = ALN_PROMPTS_PLAYSTATION;
        }
        return 1;
    }
    if (!strcmp(key, ALN_PROMPTS_XBOX_KEY)) {
        if (style) {
            *style = ALN_PROMPTS_XBOX;
        }
        return 1;
    }
    return 0;
}

int AlnPromptPatchFits(const AlnControllerPromptPatch *patch, uint64_t stream_size) {
    if (!patch || !patch->data || patch->zsize < 2 || patch->dsize < 0x100) {
        return 0;
    }
    const uint64_t block_bytes = (uint64_t)patch->block_units * 64;
    if (patch->offset > stream_size || block_bytes < 40) {
        return 0;
    }
    if (patch->offset + block_bytes > stream_size) {
        return 0;
    }
    return (uint64_t)40 + patch->zsize <= block_bytes;
}

int AlnScenePatchFits(const AlnControllerScenePatch *patch, uint64_t stream_size) {
    if (!patch || !patch->data || patch->zsize < 2 || patch->original_dsize < 0x100 || patch->payload_offset < 0x30) {
        return 0;
    }
    const uint64_t block_bytes = (uint64_t)patch->block_units * 64;
    if (patch->offset > stream_size || block_bytes < patch->payload_offset) {
        return 0;
    }
    if (patch->offset + block_bytes > stream_size) {
        return 0;
    }
    return (uint64_t)patch->payload_offset + patch->zsize <= block_bytes;
}
