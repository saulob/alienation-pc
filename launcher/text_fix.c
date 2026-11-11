#include "text_fix.h"

#include <string.h>

static const AlnTextFix kFixes[] = {
    {"CUSA04430-1.00", 0x3c053c00u, 76133u, 182452u, 1191u, 0x53905c3cu, 84199u},  /* Português (Brasil) */
    {"CUSA04430-1.00", 0x3c16d200u, 76341u, 183663u, 1194u, 0xd4bd343fu, 83975u},  /* Português (Portugal) */
};

static const uint8_t kOriginal[ALN_TEXT_FIX_LENGTH + 1] = " Ingl\xc3\xaas";
static const uint8_t kFixed[ALN_TEXT_FIX_LENGTH + 1] = "Ingl\xc3\xaas ";

const AlnTextFix *aln_text_fixes(const char *build, size_t *count) {
    *count = 0;
    if (!build) return NULL;
    const AlnTextFix *first = NULL;
    for (size_t i = 0; i < sizeof(kFixes) / sizeof(*kFixes); ++i) {
        if (strcmp(kFixes[i].build, build)) continue;
        if (!first) first = &kFixes[i];
        ++*count;
    }
    return first;
}

int aln_text_fix_apply(uint8_t *data, size_t size, const AlnTextFix *fix) {
    if (!data || !fix || fix->offset + ALN_TEXT_FIX_LENGTH + 1ull > size) return 0;
    uint8_t *text = data + fix->offset;
    if (memcmp(text, kOriginal, ALN_TEXT_FIX_LENGTH) || text[ALN_TEXT_FIX_LENGTH] != 0) return 0;
    memcpy(text, kFixed, ALN_TEXT_FIX_LENGTH);
    return 1;
}
