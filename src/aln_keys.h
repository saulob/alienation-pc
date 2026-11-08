/* Keyboard bindings: the actions, their defaults and the saved key names, shared by the launcher, the runtime pad
 * and the video core. Keys are SDL scancodes, saved as the shadPS4 names (space, lshift, f10, pgup...) or SDL's own
 * lowercase names for the rest. */
#ifndef ALN_KEYS_H
#define ALN_KEYS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ALN_KEY_LEFT_STICK_UP, ALN_KEY_LEFT_STICK_DOWN, ALN_KEY_LEFT_STICK_LEFT, ALN_KEY_LEFT_STICK_RIGHT,
    ALN_KEY_RIGHT_STICK_UP, ALN_KEY_RIGHT_STICK_DOWN, ALN_KEY_RIGHT_STICK_LEFT, ALN_KEY_RIGHT_STICK_RIGHT,
    ALN_KEY_CROSS, ALN_KEY_CIRCLE, ALN_KEY_SQUARE, ALN_KEY_TRIANGLE,
    ALN_KEY_L1, ALN_KEY_R1, ALN_KEY_L2, ALN_KEY_R2, ALN_KEY_L3, ALN_KEY_R3,
    ALN_KEY_DPAD_UP, ALN_KEY_DPAD_DOWN, ALN_KEY_DPAD_LEFT, ALN_KEY_DPAD_RIGHT,
    ALN_KEY_OPTIONS, ALN_KEY_TOUCHPAD_LEFT, ALN_KEY_TOUCHPAD_RIGHT,
    ALN_KEY_HOTKEY_WINDOW_MODE,
    ALN_KEY_COUNT
} AlnKeyAction;

#define ALN_KEY_FIRST_HOTKEY ALN_KEY_HOTKEY_WINDOW_MODE

typedef struct {
    const char *ini_key; /* the line in alienation.ini */
    const char *label;   /* the launcher's row */
    int default_scancode;
} AlnKeyInfo;

extern const AlnKeyInfo aln_key_actions[ALN_KEY_COUNT];

typedef struct { int scancode[ALN_KEY_COUNT]; } AlnKeyMap;

void aln_keys_defaults(AlnKeyMap *map);

/* The scancode a saved name stands for (case-insensitive), or 0 when it names no bindable key. */
int aln_key_from_name(const char *name);
/* False for no key and for Escape, which cancels a capture. */
int aln_key_bindable(int scancode);
/* The name saved for a scancode, written to `buffer`; empty when the key is not bindable. */
const char *aln_key_name(int scancode, char *buffer, size_t size);

/* Applies one saved line to the map; false when the key is not an action or the value names no bindable key. */
int aln_keys_apply(AlnKeyMap *map, const char *ini_key, const char *value);
/* Defaults, then every valid binding in the alienation.ini at `path`; a missing file or line keeps the default. */
void aln_keys_load(const char *path, AlnKeyMap *map);

#ifdef __cplusplus
}
#endif

#endif
