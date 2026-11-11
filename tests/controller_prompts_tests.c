#include "controller_prompts.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                                                              \
    do {                                                                                                              \
        if (!(condition)) {                                                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                     \
            ++failures;                                                                                               \
        }                                                                                                             \
    } while (0)

static int SameText(const char *a, const char *b) {
    return a && b && !strcmp(a, b);
}

static int Contains(const char *value, const char *needle) {
    return value && needle && strstr(value, needle) != NULL;
}

int main(void) {
    int style = -1;
    CHECK(AlnParsePromptStyle("playstation", &style) && style == ALN_PROMPTS_PLAYSTATION);
    CHECK(AlnParsePromptStyle("xbox", &style) && style == ALN_PROMPTS_XBOX);
    CHECK(!AlnParsePromptStyle("", &style));
    CHECK(!AlnParsePromptStyle("PlayStation", &style));
    CHECK(SameText(AlnPromptStyleKey(ALN_PROMPTS_PLAYSTATION), "playstation"));
    CHECK(SameText(AlnPromptStyleKey(ALN_PROMPTS_XBOX), "xbox"));
    CHECK(Contains(AlnPromptStyleLabel(ALN_PROMPTS_PLAYSTATION), "PlayStation"));
    CHECK(Contains(AlnPromptStyleLabel(ALN_PROMPTS_XBOX), "Xbox"));

    size_t count = 0;
    const AlnControllerPromptPatch *patches = AlnControllerPromptPatches(&count);
    CHECK(patches != NULL);
    CHECK(count == 21);

    static const char *const builds[] = {"CUSA00062-1.00", "CUSA00062-1.06", "CUSA04430-1.00"};
    enum { BUILDS = sizeof(builds) / sizeof(builds[0]) };
    for (size_t b = 0; b < BUILDS; ++b) {
        size_t found = 0;
        uint64_t starts[8], ends[8];
        for (size_t i = 0; i < count; ++i) {
            const AlnControllerPromptPatch *patch = &patches[i];
            if (!SameText(patch->build, builds[b])) {
                continue;
            }
            CHECK(found < 8);
            starts[found] = patch->offset;
            ends[found] = patch->offset + (uint64_t)patch->block_units * 64;
            ++found;
            CHECK(patch->offset != 0);
            CHECK(patch->original_zsize > 0);
            CHECK(patch->original_dsize >= 0x100);
            CHECK(patch->original_crc32 != 0);
            CHECK(patch->zsize > 1);
            CHECK(patch->data != NULL);
            CHECK(patch->data[0] == 0x78);
            CHECK(patch->data[1] == 0xda);
            CHECK(AlnPromptPatchFits(patch, (uint64_t)1 << 40));
            CHECK(!AlnPromptPatchFits(patch, 0));
            CHECK((uint64_t)40 + patch->zsize <= (uint64_t)patch->block_units * 64);
        }
        CHECK(found == 7);
        for (size_t i = 0; i < found; ++i) {
            for (size_t j = i + 1; j < found; ++j) {
                CHECK(ends[i] <= starts[j] || ends[j] <= starts[i]);
            }
        }
    }

    for (size_t i = 0; i < count; ++i) {
        const AlnControllerPromptPatch *patch = &patches[i];
        CHECK(SameText(patch->build, builds[0]) || SameText(patch->build, builds[1]) ||
              SameText(patch->build, builds[2]));
        CHECK(patch->name != NULL && patch->name[0] != 0);
    }

    size_t scene_count = 0;
    const AlnControllerScenePatch *scenes = AlnControllerScenePatches(&scene_count);
    CHECK(scenes != NULL);
    CHECK(scene_count == 6);
    for (size_t i = 0; i < scene_count; ++i) {
        const AlnControllerScenePatch *scene = &scenes[i];
        CHECK(SameText(scene->build, builds[0]) || SameText(scene->build, builds[1]) ||
              SameText(scene->build, builds[2]));
        CHECK(SameText(scene->name, "controller_template") || SameText(scene->name, "menu_layout"));
        CHECK(scene->offset != 0);
        CHECK(scene->original_zsize > 0);
        CHECK(scene->original_dsize >= 0x100);
        CHECK(scene->original_crc32 != 0);
        CHECK(scene->zsize > 1);
        CHECK(scene->payload_offset == 0x94 || scene->payload_offset == 0x30);
        CHECK(scene->data != NULL);
        CHECK(scene->data[0] == 0x78);
        CHECK(scene->data[1] == 0xda);
        CHECK(AlnScenePatchFits(scene, (uint64_t)1 << 40));
        CHECK(!AlnScenePatchFits(scene, 0));
        CHECK((uint64_t)scene->payload_offset + scene->zsize <= (uint64_t)scene->block_units * 64);
    }
    for (size_t b = 0; b < BUILDS; ++b) {
        int template_rows = 0;
        int layout_rows = 0;
        for (size_t i = 0; i < scene_count; ++i) {
            if (!SameText(scenes[i].build, builds[b])) {
                continue;
            }
            if (SameText(scenes[i].name, "controller_template")) {
                ++template_rows;
                CHECK(scenes[i].payload_offset == 0x94);
            } else {
                ++layout_rows;
                CHECK(scenes[i].payload_offset == 0x30);
            }
        }
        CHECK(template_rows == 1);
        CHECK(layout_rows == 1);
    }

    size_t glyph_count = 0;
    const char *const *glyphs = AlnXboxGlyphNames(&glyph_count);
    CHECK(glyphs != NULL);
    CHECK(glyph_count == 12);
    static const char *const required[] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "View", "Menu",
                                           "Left Stick / L3", "Right Stick / R3"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        int found = 0;
        for (size_t g = 0; g < glyph_count; ++g) {
            found |= SameText(glyphs[g], required[i]);
        }
        CHECK(found);
    }
    for (size_t g = 0; g < glyph_count; ++g) {
        CHECK(!Contains(glyphs[g], "riangle"));
        CHECK(!Contains(glyphs[g], "ircle"));
        CHECK(!Contains(glyphs[g], "quare"));
        CHECK(!Contains(glyphs[g], "hare"));
        CHECK(!Contains(glyphs[g], "ptions"));
        CHECK(!Contains(glyphs[g], "ouchpad"));
    }

    if (failures) {
        fprintf(stderr, "%d controller prompt check(s) failed\n", failures);
        return 1;
    }
    puts("controller prompt checks passed");
    return 0;
}
