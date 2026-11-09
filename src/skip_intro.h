/* Skip Intro: the startup splash ends at its first frame; the intro video is skipped with Cross. With the setting on, the game's controller object reports Cross as
 * held for two controller updates once the intro is playing, as a real tap would; the game takes its own skip. */
#ifndef ALN_SKIP_INTRO_H
#define ALN_SKIP_INTRO_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *build;       /* "CUSA00062-1.00" */
    uint32_t is_down;        /* Controller_Orbis::isDown(button) */
    uint32_t was_pressed;    /* Controller_Orbis::wasPressed(button) */
    uint32_t update;         /* Controller_Orbis::update() */
    uint32_t is_down_slot;   /* image offset of isDown's vtable slot; wasPressed's follows two slots later */
} SkipIntroSite;

typedef struct {
    uint32_t is_down;
    uint32_t update;
} SkipIntroHooks;

/* Checks the controller class against the build's fingerprints. 0 with *why naming the failed check otherwise. */
int skip_intro_verify(const uint8_t *image, uint64_t size, const SkipIntroSite *site, SkipIntroHooks *hooks,
                      const char **why);

/* The verified site of a build key; NULL for any other build. */
const SkipIntroSite *skip_intro_site(const char *build);

size_t skip_intro_stub_bytes(void);

/* Hooks the controller class and enables the video tracking. Writes nothing and returns 0, saying why, when the build
 * or its code is not the verified one or the stub area does not fit. */
int skip_intro_install(uint8_t *image, uint64_t size, const char *build, uint8_t *stubs, size_t stubs_size);

/* The AvPlayer's view of the startup videos: a splash video (added returns 1) must reach the game with black frames
 * only, and is ended at its first frame (frame returns 1 once; the player then finishes as if the file ended). Only the
 * intro arms the Cross skip, and any other video ends the feature. */
int skip_intro_video_added(const void *player, const char *path);
int skip_intro_video_frame(const void *player);
void skip_intro_video_closed(const void *player);

void GUEST_HOOK_ABI skip_intro_is_down_hook(GuestRegs *regs);
void GUEST_HOOK_ABI skip_intro_update_hook(GuestRegs *regs);

#ifdef __cplusplus
}
#endif

#endif
