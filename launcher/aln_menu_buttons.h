/* Asian Menu Buttons: the menus' Confirm and Cancel in builds whose scripts ask the engine for them
 * (engine_get_confirm_button and engine_get_cancel_button: menu input and the CTRL_ACCEPT / CTRL_CANCEL prompt glyphs
 * both follow them). Their answer is a name compiled into the executable: "circle" confirms in CUSA04430, "cross" in
 * CUSA00062 1.06. The setting picks the answer in every such build; the prompt style never changes it. */
#ifndef ALN_MENU_BUTTONS_H
#define ALN_MENU_BUTTONS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *build;
    uint32_t confirm; /* engine_get_confirm_button's `lea rsi, [rip+name]` */
    uint32_t cancel;  /* engine_get_cancel_button's */
    uint32_t cross;   /* the "cross" and "circle" names the two choose from */
    uint32_t circle;
} AlnMenuButtonSite;

/* The verified getters of a build; NULL for a build whose scripts fix Cross as Confirm themselves. */
const AlnMenuButtonSite *aln_menu_button_site(const char *build);

/* "cross" or "circle": the Confirm the getters answer now. NULL when they are not the verified code. */
const char *aln_menu_buttons_confirm(const uint8_t *image, uint64_t size, const AlnMenuButtonSite *site);

/* The saved choice (0/1) with ALN_ASIAN_MENU_BUTTONS (0/1, off/on) taking precedence. */
int aln_menu_buttons_wanted(int saved);

/* Makes Circle confirm and Cross go back (`circle_confirms`, as in the Asian release) or the reverse. Returns the menus'
 * Confirm afterwards ("cross" or "circle"), NULL when the getters are not the verified code (nothing written);
 * `message` says what was done, for the log. */
const char *aln_menu_buttons_apply(uint8_t *image, uint64_t size, const char *build, int circle_confirms, char *message,
                                   size_t message_size);

#ifdef __cplusplus
}
#endif

#endif
