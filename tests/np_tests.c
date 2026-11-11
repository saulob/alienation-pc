/* The local NP identity: nothing is bound with online play off; with it on, the initial user is signed in through
 * the registered state callback, the account checks the game runs succeed for its own online id only, and the
 * network reports an obtained IP address. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(table[i].name, nid)) return (uintptr_t)table[i].function;
    return 0;
}

#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NP_USER_NOT_FOUND ((int32_t)0x80550004)

typedef int32_t (ABI *RegisterFn)(void *, void *);
typedef int32_t (ABI *IdFn)(int32_t);
typedef int32_t (ABI *VoidFn)(void);
typedef int32_t (ABI *CreateFn)(const void *);
typedef int32_t (ABI *PollFn)(int32_t, int32_t *);
typedef int32_t (ABI *AvailabilityFn)(int32_t, const char *, void *);
typedef int32_t (ABI *ParentalFn)(int32_t, const char *, int8_t *, uint8_t *);
typedef int32_t (ABI *PlusFn)(int32_t, const void *, uint8_t *);
typedef int32_t (ABI *AccountFn)(const char *, uint64_t *);
typedef int32_t (ABI *StateFn)(int32_t *);
typedef int32_t (ABI *InfoFn)(int32_t, char *);
typedef int32_t (ABI *NetCtlRegisterFn)(void *, void *, int32_t *);
typedef int32_t (ABI *NatFn)(uint32_t *);

static struct { int calls; int32_t user, state; unsigned char np_id[36]; void *arg; } seen;
static ABI void state_callback(int32_t user, int32_t state, const void *np_id, void *arg) {
    ++seen.calls;
    seen.user = user;
    seen.state = state;
    memcpy(seen.np_id, np_id, sizeof(seen.np_id));
    seen.arg = arg;
}
static ABI void netctl_callback(int32_t event, void *arg) { (void)event; (void)arg; }

#define BIND(type, name) type name = (type)runtime_np_resolve(#name)

int main(void) {
    runtime_np_configure(0, 0, NULL, 0);
    CHECK(!runtime_np_enabled() && runtime_np_resolve("sceNpCheckCallback") == 0 && runtime_np_resolve("sceNetCtlGetState") == 0);
    runtime_np_configure(1, 0, "Player-a", 0x7f000001);
    CHECK(!runtime_np_enabled());               /* no account: stays offline */

    const uint64_t account = 0x123456789abcdefull;
    runtime_np_configure(1, account, "Player-abcdef", 0xc0a80105);
    CHECK(runtime_np_enabled());
    CHECK(runtime_np_resolve("sceNpMatching2CreateContext") == 0); /* matching keeps its offline answer */

    BIND(RegisterFn, sceNpRegisterStateCallback);
    BIND(IdFn, sceNpUnregisterStateCallback);
    BIND(VoidFn, sceNpCheckCallback);
    BIND(CreateFn, sceNpCreateAsyncRequest);
    BIND(IdFn, sceNpDeleteRequest);
    BIND(PollFn, sceNpPollAsync);
    BIND(AvailabilityFn, sceNpCheckNpAvailability);
    BIND(ParentalFn, sceNpGetParentalControlInfo);
    BIND(PlusFn, sceNpCheckPlus);
    BIND(AccountFn, sceNpGetAccountId);
    BIND(StateFn, sceNetCtlGetState);
    BIND(InfoFn, sceNetCtlGetInfo);
    BIND(NetCtlRegisterFn, sceNetCtlRegisterCallback);
    BIND(NatFn, sceNetCtlGetNatInfo);
    CHECK(sceNpRegisterStateCallback && sceNpCheckCallback && sceNpPollAsync && sceNetCtlGetNatInfo);
    if (failures) return 1;

    /* Signed in once per registration, from CheckCallback, with the SceNpId of this profile. */
    int marker;
    CHECK(sceNpCheckCallback() == 0 && seen.calls == 0);
    const int32_t callback = sceNpRegisterStateCallback((void *)state_callback, &marker);
    CHECK(callback > 0);
    CHECK(seen.calls == 0);
    CHECK(sceNpCheckCallback() == 0 && seen.calls == 1);
    CHECK(seen.user == 1 && seen.state == 2 && seen.arg == &marker);
    CHECK(!strcmp((const char *)seen.np_id, "Player-abcdef") && seen.np_id[16] == 0);
    uint64_t opt; memcpy(&opt, seen.np_id + 20, 8);
    CHECK(opt == account);
    CHECK(sceNpCheckCallback() == 0 && seen.calls == 1);
    CHECK(sceNpUnregisterStateCallback(callback) == 0 && sceNpUnregisterStateCallback(callback) == NP_INVALID_ARGUMENT);
    CHECK(sceNpRegisterStateCallback(NULL, NULL) == NP_INVALID_ARGUMENT);

    /* The user's account checks: NP availability, parental control, PS Plus. */
    char online_id[20] = "Player-abcdef", other[20] = "Someone";
    int32_t result = -1;
    const int32_t availability = sceNpCreateAsyncRequest(NULL);
    CHECK(availability > 0);
    CHECK(sceNpPollAsync(availability, &result) == 1);  /* nothing started yet */
    CHECK(sceNpCheckNpAvailability(availability, online_id, NULL) == 0);
    CHECK(sceNpPollAsync(availability, &result) == 0 && result == 0);
    CHECK(sceNpDeleteRequest(availability) == 0 && sceNpPollAsync(availability, &result) == NP_INVALID_ARGUMENT);

    const int32_t foreign = sceNpCreateAsyncRequest(NULL);
    CHECK(sceNpCheckNpAvailability(foreign, other, NULL) == 0);
    CHECK(sceNpPollAsync(foreign, &result) == 0 && result == NP_USER_NOT_FOUND);
    sceNpDeleteRequest(foreign);

    const int32_t parental = sceNpCreateAsyncRequest(NULL);
    int8_t age = -1; uint8_t restrictions[3] = {1, 1, 1};
    CHECK(sceNpGetParentalControlInfo(parental, online_id, &age, restrictions) == 0);
    CHECK(sceNpPollAsync(parental, &result) == 0 && result == 0 && age >= 18);
    CHECK(!restrictions[0] && !restrictions[1] && !restrictions[2]);
    sceNpDeleteRequest(parental);

    const int32_t plus = sceNpCreateAsyncRequest(NULL);
    const uint64_t plus_param[3] = {0x38, 1, 1};
    uint8_t plus_result[33] = {0};
    CHECK(sceNpCheckPlus(plus, plus_param, plus_result) == 0 && plus_result[0] == 1);
    CHECK(sceNpPollAsync(plus, &result) == 0 && result == 0);
    sceNpDeleteRequest(plus);
    CHECK(sceNpCheckNpAvailability(0x7fffffff, online_id, NULL) == NP_INVALID_ARGUMENT);

    uint64_t id = 0;
    CHECK(sceNpGetAccountId(online_id, &id) == 0 && id == account);
    CHECK(sceNpGetAccountId(other, &id) == NP_USER_NOT_FOUND);

    /* NetCtl: IP obtained, the address as text, NAT type 2 with the mapped address in network order. */
    int32_t state = 0;
    CHECK(sceNetCtlGetState(&state) == 0 && state == 3);
    char ip[16];
    CHECK(sceNetCtlGetInfo(14, ip) == 0 && !strcmp(ip, "192.168.1.5"));
    CHECK(sceNetCtlGetInfo(2, ip) < 0);
    int32_t cid = 0;
    CHECK(sceNetCtlRegisterCallback((void *)netctl_callback, NULL, &cid) == 0 && cid > 0);
    uint32_t nat[4] = {16, 0, 0, 0};
    CHECK(sceNetCtlGetNatInfo(nat) == 0 && nat[2] == 2);
    const unsigned char *mapped = (const unsigned char *)&nat[3];
    CHECK(mapped[0] == 192 && mapped[1] == 168 && mapped[2] == 1 && mapped[3] == 5);

    /* Two profiles never share an online id or account. */
    runtime_np_configure(1, account + 1, "Player-abcdf0", 0x7f000001);
    sceNpRegisterStateCallback((void *)state_callback, NULL);
    sceNpCheckCallback();
    CHECK(seen.calls == 2 && !strcmp((const char *)seen.np_id, "Player-abcdf0"));
    CHECK(sceNpGetAccountId(online_id, &id) == NP_USER_NOT_FOUND);

    printf("np tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
