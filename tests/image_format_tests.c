#include "aln_image_format.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

static int contains(const char *file, const char *text) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", ALN_SOURCE_DIR, file);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char buffer[1 << 20];
    const size_t n = fread(buffer, 1, sizeof(buffer) - 1, f);
    fclose(f);
    buffer[n] = 0;
    return strstr(buffer, text) != NULL;
}

int main(void) {
    const uint32_t in[ALN_CONTENT_VALUES] = {3, 0x01020304u, 0, 0xffffffffu, 7};
    unsigned char data[ALN_CONTENT_PROFILE_SIZE];
    uint32_t out[ALN_CONTENT_VALUES];

    CHECK(ALN_CONTENT_PROFILE_SIZE == 28);
    aln_content_profile_encode(data, in);
    CHECK(!memcmp(data, "ALNCONT1", 8) && data[8] == 3 && data[12] == 4 && data[13] == 3 && data[14] == 2 && data[15] == 1);
    CHECK(aln_content_profile_parse(data, sizeof(data), out) == ALN_CONTENT_OK && !memcmp(in, out, sizeof(in)));
    CHECK(aln_content_profile_parse(data, sizeof(data) - 1, out) == ALN_CONTENT_BAD_SIZE);
    CHECK(aln_content_profile_parse(data, sizeof(data) + 1, out) == ALN_CONTENT_BAD_SIZE);
    data[0] = 'D';
    CHECK(aln_content_profile_parse(data, sizeof(data), out) == ALN_CONTENT_BAD_SIGNATURE);
    data[0] = 'A';
    for (uint32_t sku = 0; sku < 5; ++sku) {
        const uint32_t values[ALN_CONTENT_VALUES] = {sku, 0, 0, 0, 0};
        aln_content_profile_encode(data, values);
        const int valid = sku == ALN_CONTENT_SKU_FULL || sku == ALN_CONTENT_SKU_TRIAL;
        CHECK(aln_content_profile_parse(data, sizeof(data), out) == (valid ? ALN_CONTENT_OK : ALN_CONTENT_BAD_SKU));
    }
    static const unsigned char dcp_profile[ALN_CONTENT_PROFILE_SIZE] = {'D', 'C', 'P', 'C', 'O', 'N', 'T', '1', 3};
    CHECK(aln_content_profile_parse(dcp_profile, sizeof(dcp_profile), out) == ALN_CONTENT_BAD_SIGNATURE);

    CHECK(contains("scripts/prepare_image.py", "b'" ALN_CONTENT_MAGIC "'"));
    CHECK(contains("scripts/prepare_image.py", "b'" ALN_BOOT_MAGIC "'"));
    CHECK(contains("scripts/prepare_image.py", "'<8s5I'"));
    CHECK(contains("launcher/boot_image.cpp", "aln_content_profile_encode"));
    CHECK(contains("launcher/boot_image.cpp", "ALN_BOOT_MAGIC"));
    CHECK(contains("src/loader.c", "aln_content_profile_parse"));
    CHECK(contains("src/loader.c", "ALN_BOOT_MAGIC"));
    CHECK(contains("launcher/game_files.cpp", "aln_content_profile_parse"));
    puts("image format tests passed");
    return 0;
}
