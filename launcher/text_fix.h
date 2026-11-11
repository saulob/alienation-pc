/* ASIA/KR's Portuguese Voice-Over Language value is " Inglês" with a leading space and the game shows "Inglê": the
 * space moves the text and its last letter is cut. The same-size fix keeps the space after the word instead. */
#ifndef ALN_TEXT_FIX_H
#define ALN_TEXT_FIX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AlnTextFix {
    const char *build;
    uint64_t block;      /* localization block in stream.dat */
    uint32_t zsize;      /* its original compressed size, dsize and 64-byte units */
    uint32_t dsize;
    uint32_t units;
    uint32_t crc;        /* CRC-32 of the original compressed payload */
    uint32_t offset;     /* of the text in the inflated block */
} AlnTextFix;

#define ALN_TEXT_FIX_PAYLOAD 0x30u
#define ALN_TEXT_FIX_LENGTH 8u

const AlnTextFix *aln_text_fixes(const char *build, size_t *count);

/* Replaces the text at fix->offset in the inflated block; 0 (nothing written) unless the original bytes are there. */
int aln_text_fix_apply(uint8_t *data, size_t size, const AlnTextFix *fix);

/* Registers the fixes of `build` in the guest's stream.dat; the count applied, 0 for a build without any (nothing
 * registered), -1 on failure with `detail` saying why. */
int aln_text_fix_install(const char *app0, const char *build_key, char *detail, size_t detail_size);

#ifdef __cplusplus
}
#endif

#endif
