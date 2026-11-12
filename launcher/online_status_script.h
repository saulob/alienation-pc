/* The network status box (AN.Menu.Network) shows Alienation PC's online state. The machine paints its text only from some
 * of its states and clears or hides it on several paths, while its dot is updated on every tick: the patch makes that
 * tick own the text. While the dot exists it repaints through the machine's own helper whenever the text box or its
 * tag is gone or the state changed (the state asked from server_request_auth_code with a magic argument); without the
 * dot the text is hidden. The helper paints the current state's tag, in the colour of the dot, and the box is never
 * hidden by the machine's error and disconnect paths. src/online_status.c answers the calls. The sites are found by
 * their code, the same in every supported build. */
#ifndef ALN_ONLINE_STATUS_SCRIPT_H
#define ALN_ONLINE_STATUS_SCRIPT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes the patch appends. */
size_t online_status_script_extra(void);

/* Patches the decompressed block (`size` bytes, already patched by others) and appends the new code; `capacity` must hold
 * size + online_status_script_extra(). Returns the new size, or 0 with *why set and nothing written. */
size_t online_status_script_patch(uint8_t *sdat, size_t size, size_t capacity, const char **why);

#ifdef __cplusplus
}
#endif

#endif
