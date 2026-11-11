/* The ASIA/KR Portuguese Voice-Over Language fix: where it applies, what it writes and when it refuses, on synthetic
 * text and on each prepared build's real stream.dat (skipped when a build is not prepared). */
#include "text_fix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAILED: %s (line %d)\n", #condition, __LINE__); ++failures; } } while (0)

static const char kBroken[] = " Ingl\xc3\xaas";
static const char kFixed[] = "Ingl\xc3\xaas ";

static void synthetic(void) {
    AlnTextFix fix = {"CUSA04430-1.00", 0, 0, 0, 0, 0, 16};
    uint8_t data[64];
    memset(data, 'x', sizeof(data));
    memcpy(data + 16, kBroken, 9);
    CHECK(aln_text_fix_apply(data, sizeof(data), &fix) == 1);
    CHECK(!memcmp(data + 16, kFixed, 9));
    CHECK(data[15] == 'x' && data[25] == 'x');
    CHECK(aln_text_fix_apply(data, sizeof(data), &fix) == 0);

    memset(data, 'x', sizeof(data));
    memcpy(data + 16, kBroken, 9);
    data[16] = 'I';
    CHECK(aln_text_fix_apply(data, sizeof(data), &fix) == 0);
    memcpy(data + 16, kBroken, 9);
    data[24] = '!';
    CHECK(aln_text_fix_apply(data, sizeof(data), &fix) == 0);
    memcpy(data + 16, kBroken, 9);
    CHECK(aln_text_fix_apply(data, 24, &fix) == 0);
    CHECK(aln_text_fix_apply(NULL, 0, &fix) == 0 && aln_text_fix_apply(data, sizeof(data), NULL) == 0);
    CHECK(!memcmp(data + 16, kBroken, 9));
}

static void scope(void) {
    size_t count = 9;
    CHECK(aln_text_fixes("CUSA04430-1.00", &count) != NULL && count == 2);
    for (const char *build = "CUSA00062-1.00"; build; build = !strcmp(build, "CUSA00062-1.00") ? "CUSA00062-1.06" : NULL) {
        CHECK(aln_text_fixes(build, &count) == NULL && count == 0);
    }
    CHECK(aln_text_fixes("CUSA04181-1.00", &count) == NULL && count == 0);
    CHECK(aln_text_fixes(NULL, &count) == NULL && count == 0);
}

static uint8_t *read_all(const char *build, size_t *size) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/game/prepared/%s/stream.dat", ALN_SOURCE_DIR, build);
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    *size = (size_t)ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = malloc(*size);
    if (!data || fread(data, 1, *size, file) != *size) { free(data); data = NULL; }
    fclose(file);
    return data;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int contains(const uint8_t *data, size_t size, const char *text) {
    const size_t length = strlen(text);
    for (size_t i = 0; i + length <= size; ++i)
        if (!memcmp(data + i, text, length)) return 1;
    return 0;
}

static void real(const char *build) {
    size_t size = 0;
    uint8_t *stream = read_all(build, &size);
    if (!stream) { printf("skipped %s: not prepared\n", build); return; }
    size_t count = 0;
    const AlnTextFix *fixes = aln_text_fixes(build, &count);
    int broken_blocks = 0;

    for (size_t block = 0; block + 24 <= size;) {
        const uint8_t *h = stream + block;
        const uint32_t zsize = le32(h + 4), dsize = le32(h + 8), units = le32(h + 12);
        if (le32(h) != block / 64 || le32(h + 16) != le32(h) || !units || block + (size_t)units * 64 > size) { block += 64; continue; }
        if (le32(h + 20) == 0x52415744u && zsize <= units * 64 - ALN_TEXT_FIX_PAYLOAD) {
            uint8_t *text = malloc(dsize);
            uLongf inflated = dsize;
            if (text && uncompress(text, &inflated, h + ALN_TEXT_FIX_PAYLOAD, zsize) == Z_OK && inflated == dsize) {
                for (size_t i = 0; i + 9 <= dsize; ++i)
                    if (!memcmp(text + i, kBroken, 9)) {
                        int listed = 0;
                        for (size_t f = 0; f < count; ++f) listed |= fixes[f].block == block && fixes[f].offset == i;
                        CHECK(listed);
                        ++broken_blocks;
                    }
            }
            free(text);
        }
        block += (size_t)units * 64;
    }
    /* Exactly the blocks that carry the broken text are fixed, so no other language is touched. */
    CHECK(broken_blocks == (int)count);

    for (size_t f = 0; f < count; ++f) {
        const AlnTextFix *fix = &fixes[f];
        const uint8_t *h = stream + fix->block;
        CHECK(fix->block + (size_t)fix->units * 64 <= size);
        CHECK(le32(h) == fix->block / 64 && le32(h + 20) == 0x52415744u && le32(h + 4) == fix->zsize &&
              le32(h + 8) == fix->dsize && le32(h + 12) == fix->units);
        CHECK(crc32(0L, h + ALN_TEXT_FIX_PAYLOAD, fix->zsize) == fix->crc);
        uint8_t *text = malloc(fix->dsize), *fixed = malloc(fix->dsize);
        uLongf inflated = fix->dsize;
        CHECK(uncompress(text, &inflated, h + ALN_TEXT_FIX_PAYLOAD, fix->zsize) == Z_OK && inflated == fix->dsize);
        memcpy(fixed, text, fix->dsize);
        CHECK(aln_text_fix_apply(fixed, fix->dsize, fix) == 1);
        size_t differing = 0;
        for (size_t i = 0; i < fix->dsize; ++i) differing += text[i] != fixed[i];
        CHECK(differing >= 1 && differing <= 8 && !memcmp(fixed + fix->offset, kFixed, 9));
        CHECK(fix->offset >= 2 && !fixed[fix->offset - 1] && !fixed[fix->offset - 2]);
        CHECK(contains(text, fix->dsize, f == 0 ? "Portugu\xc3\xaas (Brasil)" : "Portugu\xc3\xaas (Portugal)"));
        CHECK(contains(text, fix->dsize, f == 0 ? "Idioma da voz" : "Idioma do \xc3\xa1udio"));

        uLongf packed_size = compressBound(fix->dsize);
        uint8_t *packed = malloc(packed_size);
        CHECK(compress2(packed, &packed_size, fixed, fix->dsize, 9) == Z_OK);
        CHECK(packed_size <= (uLongf)fix->units * 64 - ALN_TEXT_FIX_PAYLOAD);
        uLongf back_size = fix->dsize;
        uint8_t *back = malloc(fix->dsize);
        CHECK(uncompress(back, &back_size, packed, packed_size) == Z_OK && !memcmp(back, fixed, fix->dsize));
        CHECK(memcmp(text, fixed, fix->dsize) != 0);
        printf("%s %#llx: \" Ingl\\xc3\\xaas\" -> \"Ingl\\xc3\\xaas \" (%u of %u bytes)\n", build,
               (unsigned long long)fix->block, (unsigned)packed_size, fix->units * 64 - ALN_TEXT_FIX_PAYLOAD);
        free(text); free(fixed); free(packed); free(back);
    }
    free(stream);
}

int main(void) {
    synthetic();
    scope();
    real("CUSA00062-1.00");
    real("CUSA00062-1.06");
    real("CUSA04430-1.00");
    printf("text fix tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
