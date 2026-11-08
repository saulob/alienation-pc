#include "aln_resolution.h"

#include <stdio.h>
#include <string.h>

const AlnResolution aln_resolutions[ALN_RESOLUTION_COUNT] = {
    {"native", "Native", 1920, 1080, 0}, {"480p", "480p", 854, 480, 0},
    {"720p", "720p", 1280, 720, 0},              {"1440p", "1440p", 2560, 1440, 1},
    {"1800p", "1800p", 3200, 1800, 1},           {"4K", "4K", 3840, 2160, 1},
};

int aln_resolution_index(const char *key) {
    for (int i = 0; key && i < ALN_RESOLUTION_COUNT; ++i)
        if (!strcmp(key, aln_resolutions[i].key)) return i;
    return 0;
}

const AlnAspectRatio aln_aspect_ratios[ALN_ASPECT_COUNT] = {
    {"16:9", "16:9 Original", 16, 9}, {"16:10", "16:10", 16, 10},       {"21:9", "21:9 Ultrawide", 21, 9},
    {"32:9", "32:9 Super Ultrawide", 32, 9}, {"4:3", "4:3", 4, 3},      {"5:4", "5:4", 5, 4},
};

int aln_aspect_index(const char *key) {
    for (int i = 0; key && i < ALN_ASPECT_COUNT; ++i)
        if (!strcmp(key, aln_aspect_ratios[i].key)) return i;
    return 0;
}

static int aligned_width(int height, const AlnAspectRatio *aspect) {
    const long width = ((long)height * aspect->width * 2 + aspect->height) / (2 * aspect->height);
    return (int)((width + 4) / 8 * 8);
}

AlnResolution aln_render_target(const AlnResolution *resolution, const AlnAspectRatio *aspect) {
    AlnResolution target = *resolution;
    const long budget = 3840L * 2160;
    int height = resolution->height;
    while (height > 8 && (long)aligned_width(height, aspect) * height > budget) height -= 8;
    target.width = aligned_width(height, aspect);
    target.height = height;
    if (target.width > 1920 || height > 1080 || (long)target.width * height > 1920L * 1080) target.pro_memory = 1;
    return target;
}

uint32_t aln_scaled_budget(uint32_t native_bytes, const AlnResolution *target) {
    const uint64_t native_pixels = 1920u * 1080u;
    const uint64_t step = 2u << 20;
    const uint64_t limit = 0x7fe00000u;
    const uint64_t pixels = (uint64_t)target->width * (uint64_t)target->height;
    uint64_t bytes = ((uint64_t)native_bytes * pixels + native_pixels - 1) / native_pixels;
    bytes = (bytes + step - 1) / step * step;
    if (bytes < native_bytes) bytes = native_bytes;
    return (uint32_t)(bytes < limit ? bytes : (limit > native_bytes ? limit : native_bytes));
}

uint64_t aln_budget_growth(const AlnResolutionSite *sites, size_t count, const AlnResolution *target) {
    uint64_t growth = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!sites[i].pixel_budget) continue;
        const uint64_t site_growth = aln_scaled_budget(sites[i].value, target) - sites[i].value;
        if (site_growth > growth) growth = site_growth;
    }
    return growth;
}

static int fail(char *error, size_t size, const char *what, uint32_t offset) {
    snprintf(error, size, "render resolution patch: %s at image offset 0x%x", what, (unsigned)offset);
    return 0;
}

int aln_apply_resolution(unsigned char *image, uint64_t image_size, const AlnResolutionSite *sites, size_t count,
                         const AlnResolution *resolution, char *error, size_t error_size) {
    for (size_t i = 0; i < count; ++i) {
        const AlnResolutionSite *site = &sites[i];
        const uint64_t end = (uint64_t)site->offset + site->opcode_length + 4;
        if (end > image_size) return fail(error, error_size, "site outside the image", site->offset);
        const unsigned char *at = image + site->offset;
        uint32_t current;
        memcpy(&current, at + site->opcode_length, 4);
        if (memcmp(at, site->opcode, site->opcode_length) || current != site->value)
            return fail(error, error_size, "unexpected instruction", site->offset);
    }
    for (size_t i = 0; i < count; ++i) {
        const uint32_t value = sites[i].pixel_budget ? aln_scaled_budget(sites[i].value, resolution)
                                                     : (uint32_t)(sites[i].height ? resolution->height : resolution->width);
        memcpy(image + sites[i].offset + sites[i].opcode_length, &value, 4);
    }
    return 1;
}
