/* The menus' network status box (bottom right) shows Alienation PC's online state instead of Housemarque's UNX Global
 * Matrix login, a server that no longer exists and that squads never needed. The game's own status machine still paints
 * the box (its authorization code never completes, so it never dials the server); its script patch
 * (launcher/online_status_script.c) asks the auth native for the state to paint. The box texts come from the game's
 * localization lookup, answered here for the status keys in every language. */
#ifndef ALN_ONLINE_STATUS_H
#define ALN_ONLINE_STATUS_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The lobby link. */
enum { ONLINE_SERVICE_OFF, ONLINE_SERVICE_CONNECTING, ONLINE_SERVICE_READY, ONLINE_SERVICE_FAILED };

/* What the box shows. */
enum { ONLINE_STATUS_OFF, ONLINE_STATUS_CONNECTING, ONLINE_STATUS_CONNECTED, ONLINE_STATUS_UNAVAILABLE };

typedef struct {
    int enabled;           /* Online Play on, a known build and an online identity */
    int service;           /* ONLINE_SERVICE_* */
    int np_signed_in;      /* the game was told the user signed in */
    int matching2_started; /* the game started its Matching2 context */
    uint64_t connecting_ms; /* how long the lobby link has been connecting */
} OnlineStatusInput;

#define ONLINE_STATUS_CONNECT_TIMEOUT_MS 45000u

int online_status_classify(const OnlineStatusInput *in);
const char *online_status_name(int status);

/* The ints the script passes server_request_auth_code: STYLE (the dot), PAINT (the helper, about to paint) and
 * PAINT_HIDDEN (the same while the dot does not exist, so the text is not shown) are answered ONLINE_STATUS_* + 1;
 * CHANGED is answered 1 when the state differs from the one last painted in view, else 0. */
#define ONLINE_STATUS_STYLE_MAGIC 0x414c4e43u
#define ONLINE_STATUS_PAINT_MAGIC 0x414c4e54u
#define ONLINE_STATUS_PAINT_HIDDEN_MAGIC 0x414c4e48u
#define ONLINE_STATUS_CHANGED_MAGIC 0x414c4e4fu

/* The answer to one of the magic ints for the current `state`, or -1 for any other argument. */
int online_status_answer(uint32_t magic, int state);

/* The localization keys of the box texts (as the game's lookup passes them). */
enum { ONLINE_TEXT_CONNECTING, ONLINE_TEXT_CONNECTED, ONLINE_TEXT_UNAVAILABLE, ONLINE_TEXT_OFFLINE, ONLINE_TEXT_NONE };
int online_status_text_kind(uint32_t key);
/* The text the game shows for `key`: ours for a status key in the language `original` is in, `original` otherwise. */
const char *online_status_text(uint32_t key, const char *original);

typedef struct {
    const char *build;
    uint32_t auth_code;       /* server_request_auth_code */
    uint32_t lookup_language; /* the localize helper's two lookup calls */
    uint32_t lookup_default;
} OnlineStatusSite;

const OnlineStatusSite *online_status_site(const char *build);
int online_status_verify(const uint8_t *image, uint64_t size, const OnlineStatusSite *site, const char **why);
size_t online_status_stub_bytes(void);
/* The text hooks and the auth native's answers to the script; `state` is the current ONLINE_STATUS_*. */
int online_status_install(uint8_t *image, uint64_t size, const char *build, int (*state)(void), uint8_t *stubs,
                          size_t stubs_size);

#ifdef __cplusplus
}
#endif

#endif
