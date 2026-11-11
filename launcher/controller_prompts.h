// Controller prompt style and the generated Xbox asset patch table.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ALN_PROMPTS_PLAYSTATION = 0,
    ALN_PROMPTS_XBOX = 1,
};

#define ALN_PROMPTS_KEY "controller_prompts"
#define ALN_PROMPTS_PLAYSTATION_KEY "playstation"
#define ALN_PROMPTS_XBOX_KEY "xbox"
#define ALN_PROMPTS_DEFAULT_KEY ALN_PROMPTS_PLAYSTATION_KEY

const char *AlnPromptStyleKey(int style);
const char *AlnPromptStyleLabel(int style);
int AlnParsePromptStyle(const char *key, int *style);

typedef struct {
    const char *build;
    const char *name;
    uint64_t offset;
    uint32_t original_zsize;
    uint32_t original_dsize;
    uint32_t original_crc32;
    uint32_t block_units;
    uint32_t zsize;
    uint32_t dsize;
    const unsigned char *data;
} AlnControllerPromptPatch;

const AlnControllerPromptPatch *AlnControllerPromptPatches(size_t *count);

typedef struct {
    const char *build;
    const char *name;
    uint64_t offset;
    uint32_t original_zsize;
    uint32_t original_dsize;
    uint32_t original_crc32;
    uint32_t block_units;
    uint32_t zsize;
    uint32_t payload_offset;
    const unsigned char *data;
} AlnControllerScenePatch;

const AlnControllerScenePatch *AlnControllerScenePatches(size_t *count);
const char *const *AlnXboxGlyphNames(size_t *count);

typedef struct {
    uint16_t x, y, width, height;
    uint32_t zsize, dsize;
    const unsigned char *data;
} AlnPromptGlyphMask;

typedef struct {
    const char *build;
    const char *name;
    uint64_t offset;
    uint32_t original_zsize;
    uint32_t original_dsize;
    uint32_t original_crc32;
    uint32_t block_units;
    const AlnPromptGlyphMask *masks;
    size_t mask_count;
} AlnPromptGlyphAtlas;

const AlnPromptGlyphAtlas *AlnPromptGlyphAtlases(size_t *count);

int AlnPromptPatchFits(const AlnControllerPromptPatch *patch, uint64_t stream_size);
int AlnScenePatchFits(const AlnControllerScenePatch *patch, uint64_t stream_size);

// The style saved in alienation.ini; PlayStation when the file has no usable value.
int aln_controller_prompts_style(const char *settings_file);
// Serves the Xbox resources over /app0/stream.dat when the saved style is Xbox. The archive is only read.
int aln_controller_prompts_install(const char *settings_file, const char *app0, const char *build_key, char *detail,
                                   size_t detail_size);

#ifdef __cplusplus
}
#endif
