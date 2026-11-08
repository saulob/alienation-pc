#ifndef ALN_IMAGE_FORMAT_H
#define ALN_IMAGE_FORMAT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ALN_BOOT_MAGIC "ALNBOOT1"
#define ALN_CONTENT_MAGIC "ALNCONT1"
#define ALN_MAGIC_SIZE 8
#define ALN_CONTENT_VALUES 5
#define ALN_CONTENT_PROFILE_SIZE (ALN_MAGIC_SIZE + 4 * ALN_CONTENT_VALUES)
#define ALN_CONTENT_SKU_TRIAL 1u
#define ALN_CONTENT_SKU_FULL 3u

typedef enum { ALN_CONTENT_OK = 0, ALN_CONTENT_BAD_SIZE, ALN_CONTENT_BAD_SIGNATURE, ALN_CONTENT_BAD_SKU } AlnContentStatus;

/* Magic, then SKU and the four user-defined param.sfo values as little-endian uint32. */
static inline void aln_content_profile_encode(unsigned char out[ALN_CONTENT_PROFILE_SIZE],
                                              const uint32_t values[ALN_CONTENT_VALUES]) {
    memcpy(out, ALN_CONTENT_MAGIC, ALN_MAGIC_SIZE);
    for (unsigned i = 0; i < ALN_CONTENT_VALUES; ++i)
        for (unsigned b = 0; b < 4; ++b) out[ALN_MAGIC_SIZE + i * 4 + b] = (unsigned char)(values[i] >> (8 * b));
}

static inline AlnContentStatus aln_content_profile_parse(const unsigned char *data, size_t size,
                                                         uint32_t values[ALN_CONTENT_VALUES]) {
    if (size != ALN_CONTENT_PROFILE_SIZE) return ALN_CONTENT_BAD_SIZE;
    if (memcmp(data, ALN_CONTENT_MAGIC, ALN_MAGIC_SIZE)) return ALN_CONTENT_BAD_SIGNATURE;
    for (unsigned i = 0; i < ALN_CONTENT_VALUES; ++i) {
        const unsigned char *p = data + ALN_MAGIC_SIZE + i * 4;
        values[i] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }
    return values[0] == ALN_CONTENT_SKU_FULL || values[0] == ALN_CONTENT_SKU_TRIAL ? ALN_CONTENT_OK : ALN_CONTENT_BAD_SKU;
}
#endif
