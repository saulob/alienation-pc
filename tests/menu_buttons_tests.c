/* Asian Menu Buttons per build, on synthetic images that hold each build's verified getters. The choice decides Confirm
 * and Cancel; the prompt style does not (the prompts follow the getters, so the shown button is the working one). */
#include "aln_menu_buttons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAILED: %s (line %d)\n", #condition, __LINE__); ++failures; } } while (0)

enum { IMAGE_SIZE = 0x1800000 };

static void lea(uint8_t *image, uint32_t at, uint32_t target) {
    const int32_t rel = (int32_t)((int64_t)target - ((int64_t)at + 7));
    memcpy(image + at, "\x48\x8d\x35", 3);
    memcpy(image + at + 3, &rel, sizeof(rel));
}

/* The build's own getters: `confirm_circle` is the CUSA04430 answer. */
static uint8_t *make_image(const AlnMenuButtonSite *site, int confirm_circle) {
    uint8_t *image = calloc(1, IMAGE_SIZE);
    memcpy(image + site->cross, "cross", 6);
    memcpy(image + site->circle, "circle", 7);
    lea(image, site->confirm, confirm_circle ? site->circle : site->cross);
    lea(image, site->cancel, confirm_circle ? site->cross : site->circle);
    return image;
}

static const char *apply(uint8_t *image, const char *build, int asian) {
    char message[160];
    const char *confirm = aln_menu_buttons_apply(image, IMAGE_SIZE, build, asian, message, sizeof(message));
    printf("%s %s: %s\n", build, asian ? "on" : "off", message);
    return confirm;
}

/* Only the two getters' operands may change. */
static int only_getters_changed(const uint8_t *image, const uint8_t *before, const AlnMenuButtonSite *site) {
    for (size_t i = 0; i < IMAGE_SIZE; ++i) {
        if (image[i] == before[i]) continue;
        if (!((i >= site->confirm + 3 && i < site->confirm + 7) || (i >= site->cancel + 3 && i < site->cancel + 7)))
            return 0;
    }
    return 1;
}

/* CUSA00062 1.00 has no getters: its scripts confirm with Cross, A with Xbox prompts, and On cannot change that. */
static void europe_100(void) {
    uint8_t *image = calloc(1, IMAGE_SIZE), *before = calloc(1, IMAGE_SIZE);
    CHECK(aln_menu_button_site("CUSA00062-1.00") == NULL);
    CHECK(!strcmp(apply(image, "CUSA00062-1.00", 0), "cross"));
    CHECK(!strcmp(apply(image, "CUSA00062-1.00", 1), "cross"));
    CHECK(!memcmp(image, before, IMAGE_SIZE));
    free(image);
    free(before);
}

/* Each build that asks the engine gets the chosen layout, from its own answer either way. */
static void chosen_layout(const char *build, int native_circle) {
    const AlnMenuButtonSite *site = aln_menu_button_site(build);
    CHECK(site != NULL);
    if (!site) return;
    for (int asian = 0; asian < 2; ++asian) {
        uint8_t *image = make_image(site, native_circle), *before = make_image(site, native_circle);
        const char *wanted = asian ? "circle" : "cross";
        CHECK(!strcmp(apply(image, build, asian), wanted));
        CHECK(!strcmp(aln_menu_buttons_confirm(image, IMAGE_SIZE, site), wanted));
        CHECK(only_getters_changed(image, before, site));
        CHECK((asian == native_circle) == !memcmp(image, before, IMAGE_SIZE));
        memcpy(before, image, IMAGE_SIZE);
        CHECK(!strcmp(apply(image, build, asian), wanted) && !memcmp(image, before, IMAGE_SIZE));
        free(image);
        free(before);
    }
}

/* Code that is not the verified getters is never written. */
static void unverified(void) {
    const AlnMenuButtonSite *site = aln_menu_button_site("CUSA04430-1.00");
    if (!site) return;
    uint8_t *image = make_image(site, 1);
    image[site->confirm + 1] = 0x8b;
    uint8_t *before = malloc(IMAGE_SIZE);
    memcpy(before, image, IMAGE_SIZE);
    CHECK(aln_menu_buttons_confirm(image, IMAGE_SIZE, site) == NULL);
    CHECK(apply(image, "CUSA04430-1.00", 0) == NULL && apply(image, "CUSA04430-1.00", 1) == NULL);
    CHECK(!memcmp(image, before, IMAGE_SIZE));
    free(image);
    image = make_image(site, 1);
    memcpy(image + site->circle, "circlE", 7);
    CHECK(aln_menu_buttons_confirm(image, IMAGE_SIZE, site) == NULL);
    lea(image, site->cancel, site->circle);
    memcpy(image + site->circle, "circle", 7);
    CHECK(aln_menu_buttons_confirm(image, IMAGE_SIZE, site) == NULL);
    CHECK(aln_menu_buttons_confirm(image, 0x1000, site) == NULL);
    free(image);
    free(before);
    CHECK(aln_menu_button_site("CUSA04181-1.00") == NULL && aln_menu_button_site(NULL) == NULL);
}

static void set_env(const char *value) {
#ifdef _WIN32
    char text[64];
    snprintf(text, sizeof(text), "ALN_ASIAN_MENU_BUTTONS=%s", value);
    _putenv(text);
#else
    if (*value) setenv("ALN_ASIAN_MENU_BUTTONS", value, 1);
    else unsetenv("ALN_ASIAN_MENU_BUTTONS");
#endif
}

static void environment(void) {
    set_env("");
    CHECK(aln_menu_buttons_wanted(0) == 0 && aln_menu_buttons_wanted(1) == 1);
    set_env("1");
    CHECK(aln_menu_buttons_wanted(0) == 1);
    set_env("off");
    CHECK(aln_menu_buttons_wanted(1) == 0);
    set_env("banana");
    CHECK(aln_menu_buttons_wanted(1) == 1 && aln_menu_buttons_wanted(0) == 0);
    set_env("");
}

/* The loader passes the saved choice, never the prompt style, and logs what it did. */
static void loader_wiring(void) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/src/loader.c", ALN_SOURCE_DIR);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) return;
    static char text[1 << 21];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    const char *call = strstr(text, "aln_menu_buttons_apply(");
    CHECK(call && strstr(call, "aln_menu_buttons_wanted(aln_asian_menu_buttons(settings_file))") &&
          strstr(call, "aln_menu_buttons_wanted(aln_asian_menu_buttons(settings_file))") - call < 120);
    CHECK(strstr(text, "Asian menu buttons: %s") != NULL);
}

int main(void) {
    europe_100();
    chosen_layout("CUSA00062-1.06", 0);
    chosen_layout("CUSA04430-1.00", 1);
    unverified();
    environment();
    loader_wiring();
    printf("menu buttons tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
