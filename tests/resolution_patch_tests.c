#include "aln_frame_rate.h"
#include "aln_resolution.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAILED: %s (line %d)\n", #condition, __LINE__); ++failures; } } while (0)

static const AlnResolutionSite sites[] = {
    {16, 3, {0xc7, 0x47, 0x34}, 1080, 1, 0},
    {32, 3, {0xc7, 0x47, 0x30}, 1920, 0, 0},
};

static const AlnResolutionSite heap_sites[] = {
    {0, 4, {0x48, 0xc7, 0x45, 0x88}, 0x40000000, 0, 1},
    {8, 4, {0x48, 0xc7, 0x45, 0x88}, 0x40000000, 0, 1},
};

static void build_image(unsigned char *image) {
    memset(image, 0x90, 64);
    memcpy(image + 16, "\xc7\x47\x34\x38\x04\x00\x00", 7);
    memcpy(image + 32, "\xc7\x47\x30\x80\x07\x00\x00", 7);
}

static uint32_t immediate(const unsigned char *image, uint32_t offset) {
    uint32_t value;
    memcpy(&value, image + offset + 3, 4);
    return value;
}

static void render_target(void) {
    const AlnAspectRatio *wide = &aln_aspect_ratios[aln_aspect_index("21:9")];
    AlnResolution native = aln_render_target(&aln_resolutions[0], &aln_aspect_ratios[0]);
    CHECK(native.width == 1920 && native.height == 1080 && !native.pro_memory);
    AlnResolution ultrawide = aln_render_target(&aln_resolutions[aln_resolution_index("1440p")], wide);
    CHECK(ultrawide.width == 3360 && ultrawide.height == 1440 && ultrawide.pro_memory);
    AlnResolution quad = aln_render_target(&aln_resolutions[aln_resolution_index("1440p")], &aln_aspect_ratios[0]);
    CHECK(quad.width == 2560 && quad.height == 1440);
    const AlnAspectRatio *super = &aln_aspect_ratios[aln_aspect_index("32:9")];
    AlnResolution capped = aln_render_target(&aln_resolutions[aln_resolution_index("4K")], super);
    CHECK((long)capped.width * capped.height <= 3840L * 2160 && capped.width % 8 == 0);
}

static void resolution_patch(void) {
    unsigned char image[64];
    char error[160];
    AlnResolution target = {"x", "x", 3360, 1440, 1};
    build_image(image);
    CHECK(aln_apply_resolution(image, sizeof(image), sites, 2, &target, error, sizeof(error)));
    CHECK(immediate(image, 16) == 1440 && immediate(image, 32) == 3360);

    AlnResolution native = aln_resolutions[0];
    build_image(image);
    CHECK(aln_apply_resolution(image, sizeof(image), sites, 2, &native, error, sizeof(error)));
    CHECK(immediate(image, 16) == 1080 && immediate(image, 32) == 1920);

    build_image(image);
    image[33] = 0x46;
    CHECK(!aln_apply_resolution(image, sizeof(image), sites, 2, &target, error, sizeof(error)));
    CHECK(immediate(image, 16) == 1080 && strstr(error, "0x20"));

    build_image(image);
    CHECK(!aln_apply_resolution(image, 36, sites, 2, &target, error, sizeof(error)));
}

static void heap_budget(void) {
    const uint32_t retail = 0x40000000;
    const AlnResolution native = aln_resolutions[0];
    const AlnResolution wide = {"x", "x", 3360, 1440, 1};
    const AlnResolution low = aln_resolutions[aln_resolution_index("720p")];
    const AlnResolution quad = {"x", "x", 2560, 1440, 1};
    const AlnResolution four_k = aln_resolutions[aln_resolution_index("4K")];
    CHECK(aln_scaled_budget(retail, &native) == retail);
    CHECK(aln_scaled_budget(retail, &low) == retail);
    CHECK(aln_scaled_budget(retail, &wide) == 0x7fe00000u);
    CHECK(aln_scaled_budget(retail, &four_k) == 0x7fe00000u);
    const uint32_t quad_budget = aln_scaled_budget(retail, &quad);
    CHECK(quad_budget > retail && quad_budget < 0x7fe00000u && quad_budget % (2u << 20) == 0);
    CHECK(quad_budget >= (uint64_t)retail * 2560 * 1440 / (1920 * 1080));
    CHECK(aln_scaled_budget(0, &wide) == 0);

    CHECK(aln_budget_growth(heap_sites, 2, &native) == 0);
    CHECK(aln_budget_growth(heap_sites, 2, &wide) == 0x7fe00000u - retail);
    CHECK(aln_budget_growth(sites, 2, &wide) == 0);
    CHECK(aln_budget_growth(heap_sites, 0, &wide) == 0);

    unsigned char image[32];
    char error[160];
    memset(image, 0x90, sizeof(image));
    memcpy(image, "\x48\xc7\x45\x88\x00\x00\x00\x40", 8);
    memcpy(image + 8, "\x48\xc7\x45\x88\x00\x00\x00\x40", 8);
    CHECK(aln_apply_resolution(image, sizeof(image), heap_sites, 2, &wide, error, sizeof(error)));
    uint32_t first, second;
    memcpy(&first, image + 4, 4);
    memcpy(&second, image + 12, 4);
    CHECK(first == 0x7fe00000u && second == 0x7fe00000u);
    CHECK(image[0] == 0x48 && image[3] == 0x88);

    memcpy(image + 8, "\x48\xc7\x45\x90\x00\x00\x00\x40", 8);
    memcpy(image + 4, "\x00\x00\x00\x40", 4);
    CHECK(!aln_apply_resolution(image, sizeof(image), heap_sites, 2, &wide, error, sizeof(error)));
    memcpy(&first, image + 4, 4);
    CHECK(first == 0x40000000u);
}

static void frame_rate_patch(void) {
    static const AlnFrameRatePatch patch = {8, {0x48, 0x8b, 0x47, 0x28}, {0x31, 0xc0, 0xc3, 0x90}};
    unsigned char image[16];
    char error[160];
    memset(image, 0xcc, sizeof(image));
    memcpy(image + 8, patch.original, 4);
    CHECK(aln_apply_frame_rate(image, sizeof(image), &patch, error, sizeof(error)));
    CHECK(!memcmp(image + 8, patch.replacement, 4) && image[7] == 0xcc && image[12] == 0xcc);
    CHECK(!aln_apply_frame_rate(image, sizeof(image), &patch, error, sizeof(error)));
    CHECK(!aln_apply_frame_rate(image, 10, &patch, error, sizeof(error)));
}

int main(void) {
    render_target();
    resolution_patch();
    heap_budget();
    frame_rate_patch();
    if (!failures) puts("resolution and frame rate patch tests passed");
    return failures ? 1 : 0;
}
