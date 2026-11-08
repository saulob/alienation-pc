/* The render resolutions and aspect ratios the launcher offers, and the verified game sites that select them. */
#ifndef ALN_RESOLUTION_H
#define ALN_RESOLUTION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;  /* render_resolution in alienation.ini */
    const char *name; /* launcher label */
    int width, height;
    int pro_memory;   /* above the 1080p pixel count: the game would need its PS4 Pro memory path */
} AlnResolution;

#define ALN_RESOLUTION_COUNT 6
extern const AlnResolution aln_resolutions[ALN_RESOLUTION_COUNT];

/* Index of the key, 0 (native) when it is not listed. */
int aln_resolution_index(const char *key);

typedef struct {
    const char *key;
    const char *name;
    int width, height;
} AlnAspectRatio;

#define ALN_ASPECT_COUNT 6
extern const AlnAspectRatio aln_aspect_ratios[ALN_ASPECT_COUNT];

/* Index of the key, 0 (16:9 Original) when it is not listed. */
int aln_aspect_index(const char *key);

/* The width is aligned to the 8-pixel tile; a target above the 4K pixel count keeps its ratio and loses height. */
AlnResolution aln_render_target(const AlnResolution *resolution, const AlnAspectRatio *aspect);

/* One `mov reg, imm32` in the game that holds a render width or height, verified for one executable. */
typedef struct {
    uint32_t offset; /* image offset of the opcode */
    uint8_t opcode_length;
    uint8_t opcode[2];
    uint32_t value;  /* the original immediate */
    int height;
} AlnResolutionSite;

/* Writes the resolution's width and height into every site after checking each one still holds the original
 * instruction. Returns 0 and a message in `error` when the image does not match; nothing is written then. */
int aln_apply_resolution(unsigned char *image, uint64_t image_size, const AlnResolutionSite *sites, size_t count,
                         const AlnResolution *resolution, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
