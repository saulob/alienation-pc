#include "aln_menu_buttons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const AlnMenuButtonSite kSites[] = {
    {"CUSA00062-1.06", 0xb1efbu, 0xb1f6bu, 0x17a1e7cu, 0x17a1f0cu},
    {"CUSA04430-1.00", 0xb18fbu, 0xb196bu, 0x179c71au, 0x179c689u},
};

static const uint8_t kLeaRsi[] = {0x48, 0x8d, 0x35};
enum { LEA_LENGTH = 7 };

const AlnMenuButtonSite *aln_menu_button_site(const char *build) {
    for (size_t i = 0; build && i < sizeof(kSites) / sizeof(*kSites); ++i)
        if (!strcmp(build, kSites[i].build)) return &kSites[i];
    return NULL;
}

int aln_menu_buttons_wanted(int saved) {
    const char *value = getenv("ALN_ASIAN_MENU_BUTTONS");
    if (value && (!strcmp(value, "1") || !strcmp(value, "on"))) return 1;
    if (value && (!strcmp(value, "0") || !strcmp(value, "off"))) return 0;
    return saved != 0;
}

static int holds(const uint8_t *image, uint64_t size, uint32_t at, const char *text) {
    const size_t length = strlen(text) + 1;
    return (uint64_t)at + length <= size && !memcmp(image + at, text, length);
}

/* The lea's target, or 0 when the bytes are no `lea rsi, [rip+rel32]`. */
static uint32_t lea_target(const uint8_t *image, uint64_t size, uint32_t at) {
    if ((uint64_t)at + LEA_LENGTH > size || memcmp(image + at, kLeaRsi, sizeof(kLeaRsi))) return 0;
    int32_t rel;
    memcpy(&rel, image + at + 3, sizeof(rel));
    return (uint32_t)((int64_t)at + LEA_LENGTH + rel);
}

const char *aln_menu_buttons_confirm(const uint8_t *image, uint64_t size, const AlnMenuButtonSite *site) {
    if (!site || !holds(image, size, site->cross, "cross") || !holds(image, size, site->circle, "circle")) return NULL;
    const uint32_t confirm = lea_target(image, size, site->confirm), cancel = lea_target(image, size, site->cancel);
    if (confirm == site->circle && cancel == site->cross) return "circle";
    if (confirm == site->cross && cancel == site->circle) return "cross";
    return NULL;
}

static void point_at(uint8_t *image, uint32_t at, uint32_t target) {
    const int32_t rel = (int32_t)((int64_t)target - ((int64_t)at + LEA_LENGTH));
    memcpy(image + at + 3, &rel, sizeof(rel));
}

const char *aln_menu_buttons_apply(uint8_t *image, uint64_t size, const char *build, int circle_confirms, char *message,
                                   size_t message_size) {
    const char *state = circle_confirms ? "on" : "off";
    const AlnMenuButtonSite *site = aln_menu_button_site(build);
    if (!site) {
        snprintf(message, message_size, "%s%s, Cross confirms and Circle cancels (fixed by this build's scripts)", state,
                 circle_confirms ? " requested but unavailable" : "");
        return "cross";
    }
    const char *own = aln_menu_buttons_confirm(image, size, site);
    if (!own) {
        snprintf(message, message_size, "%s requested but unavailable: the confirm and cancel getters differ", state);
        return NULL;
    }
    const char *wanted = circle_confirms ? "circle" : "cross";
    const char *buttons = circle_confirms ? "Circle confirms and Cross cancels" : "Cross confirms and Circle cancels";
    if (!strcmp(own, wanted)) {
        snprintf(message, message_size, "%s, %s (this build's own)", state, buttons);
        return wanted;
    }
    point_at(image, site->confirm, circle_confirms ? site->circle : site->cross);
    point_at(image, site->cancel, circle_confirms ? site->cross : site->circle);
    snprintf(message, message_size, "%s, %s (set for this build)", state, buttons);
    return wanted;
}
