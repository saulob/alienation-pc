/* Local online identity for the classic NP API: the initial user signed in as this profile, the network connected
 * and the account checks passing. Bound only with online play on. */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NP_USER_NOT_FOUND ((int32_t)0x80550004)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_AUTH_ISSUER 1
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NP_STATE_SIGNED_IN 2
#define NET_CTL_STATE_IPOBTAINED 3
#define NET_CTL_INFO_IP_ADDRESS 14
#define NP_POLL_RUNNING 1
#define LOCAL_USER 1
#define MAX_CALLBACKS 4
#define MAX_REQUESTS 32
#define NP_ID_SIZE 36
#define ONLINE_ID_CHARS 16

typedef void (ABI *NpStateCallback)(int32_t user, int32_t state, const void *np_id, void *arg);

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int enabled;
static uint64_t account;
static unsigned char np_id[NP_ID_SIZE]; /* SceNpId: online id (16 + terminator + 3), opt[8], reserved[8] */
static uint32_t local_ip; /* host byte order: 127.0.0.1 is 0x7f000001 */
static struct { int used, delivered; NpStateCallback callback; void *arg; } state_callbacks[MAX_CALLBACKS];
static int next_callback=1;
static struct { int id, started, auth; int32_t result; } requests[MAX_REQUESTS];
static int next_request=0x100;
static struct { void *callback, *arg; int used; } netctl_callbacks[MAX_CALLBACKS];
static int logged_availability, logged_parental, logged_plus;
static volatile int signed_in;
static void (*pump)(void);

void runtime_np_configure(int on, uint64_t account_id, const char *online_id, uint32_t ip) {
    pthread_mutex_lock(&lock);
    enabled=on && account_id && online_id && *online_id;
    account=account_id;
    local_ip=ip;
    memset(np_id,0,sizeof(np_id));
    if (enabled) {
        memcpy(np_id,online_id,strnlen(online_id,ONLINE_ID_CHARS));
        memcpy(np_id+20,&account,sizeof(account));
    }
    memset(state_callbacks,0,sizeof(state_callbacks));
    memset(requests,0,sizeof(requests));
    memset(netctl_callbacks,0,sizeof(netctl_callbacks));
    logged_availability=logged_parental=logged_plus=0;
    signed_in=0;
    pthread_mutex_unlock(&lock);
}
int runtime_np_enabled(void) { return enabled; }
int runtime_np_signed_in(void) { return signed_in; }
void runtime_np_set_pump(void (*fn)(void)) { pump=fn; }

static int local_online_id(const char *online_id) {
    return online_id && !strncmp(online_id,(const char *)np_id,ONLINE_ID_CHARS);
}

/* ---- NpManager ---- */
static ABI int32_t np_ok(void) { return 0; }
static ABI int32_t np_register_state(void *callback, void *arg) {
    if (!callback) return NP_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int32_t id=NP_INVALID_ARGUMENT;
    for (int i=0;i<MAX_CALLBACKS;++i) if (!state_callbacks[i].used) {
        state_callbacks[i].used=next_callback++;
        state_callbacks[i].delivered=0;
        state_callbacks[i].callback=(NpStateCallback)callback;
        state_callbacks[i].arg=arg;
        id=state_callbacks[i].used;
        break;
    }
    pthread_mutex_unlock(&lock);
    return id;
}
static ABI int32_t np_unregister_state(int32_t id) {
    pthread_mutex_lock(&lock);
    int32_t r=NP_INVALID_ARGUMENT;
    for (int i=0;i<MAX_CALLBACKS;++i) if (state_callbacks[i].used==id) { state_callbacks[i].used=0; r=0; }
    pthread_mutex_unlock(&lock);
    return r;
}
/* Each registration gets the current state once, on the thread calling CheckCallback. */
static ABI int32_t np_check_callback(void) {
    struct { NpStateCallback callback; void *arg; } due[MAX_CALLBACKS];
    int count=0;
    unsigned char id[NP_ID_SIZE];
    pthread_mutex_lock(&lock);
    memcpy(id,np_id,sizeof(id));
    for (int i=0;i<MAX_CALLBACKS;++i) if (state_callbacks[i].used && !state_callbacks[i].delivered) {
        state_callbacks[i].delivered=1;
        due[count].callback=state_callbacks[i].callback;
        due[count++].arg=state_callbacks[i].arg;
    }
    pthread_mutex_unlock(&lock);
    for (int i=0;i<count;++i) {
        printf("Online: NP signed in as %.16s (user %d)\n",(const char *)id,LOCAL_USER);
        due[i].callback(LOCAL_USER,NP_STATE_SIGNED_IN,id,due[i].arg);
        signed_in=1;
    }
    if (pump) pump();
    return 0;
}
static ABI int32_t np_create_request(const void *param) {
    (void)param;
    pthread_mutex_lock(&lock);
    int32_t id=NP_INVALID_ARGUMENT;
    for (int i=0;i<MAX_REQUESTS;++i) if (!requests[i].id) {
        requests[i].id=next_request++;
        requests[i].started=requests[i].auth=0;
        requests[i].result=0;
        id=requests[i].id;
        break;
    }
    pthread_mutex_unlock(&lock);
    return id;
}
static int request_slot(int32_t id) {
    for (int i=0;i<MAX_REQUESTS;++i) if (id && requests[i].id==id) return i;
    return -1;
}
static int32_t finish_request(int32_t id, int32_t result) {
    pthread_mutex_lock(&lock);
    const int slot=request_slot(id);
    if (slot>=0) { requests[slot].started=1; requests[slot].result=result; }
    pthread_mutex_unlock(&lock);
    return slot>=0 ? 0 : NP_INVALID_ARGUMENT;
}
static ABI int32_t np_delete_request(int32_t id) {
    pthread_mutex_lock(&lock);
    const int slot=request_slot(id);
    if (slot>=0) requests[slot].id=0;
    pthread_mutex_unlock(&lock);
    return slot>=0 ? 0 : NP_INVALID_ARGUMENT;
}
static ABI int32_t np_abort_request(int32_t id) { return finish_request(id,NP_INVALID_ARGUMENT); }
static ABI int32_t np_poll(int32_t id, int32_t *result) {
    pthread_mutex_lock(&lock);
    const int slot=request_slot(id);
    int32_t r=NP_INVALID_ARGUMENT;
    if (slot>=0 && !requests[slot].started) r=NP_POLL_RUNNING;
    else if (slot>=0) { if (result) *result=requests[slot].result; r=0; }
    pthread_mutex_unlock(&lock);
    return r;
}

/* ---- NpAuth: the authorization code of the game's UNX Global Matrix login, never issued (online_status.h) ---- */
/* sceNpAuthGetAuthorizationCode(request, param, SceNpAuthorizationCode *, int *issuer) */
static ABI int32_t auth_get_code(int32_t id, const void *param, char *code, int32_t *issuer) {
    (void)param;
    pthread_mutex_lock(&lock);
    const int slot=request_slot(id);
    if (slot>=0) requests[slot].auth=1;
    pthread_mutex_unlock(&lock);
    if (slot<0) return NP_INVALID_ARGUMENT;
    if (code) strcpy(code,"ALIENATIONPC");
    if (issuer) *issuer=NP_AUTH_ISSUER;
    return 0;
}
/* The game reads only the return value: 1 running, 0 done, anything else failed. */
static ABI int32_t auth_poll(int32_t id, int32_t *result) {
    pthread_mutex_lock(&lock);
    const int slot=request_slot(id), auth=slot>=0 && requests[slot].auth;
    pthread_mutex_unlock(&lock);
    return auth ? NP_POLL_RUNNING : np_poll(id,result);
}
static void log_once(int *flag, const char *what) {
    pthread_mutex_lock(&lock);
    const int first=!*flag;
    *flag=1;
    pthread_mutex_unlock(&lock);
    if (first) printf("Online: %s\n",what);
}
/* sceNpCheckNpAvailability(request, const SceNpOnlineId *, reserved) */
static ABI int32_t np_check_availability(int32_t id, const char *online_id, void *reserved) {
    (void)reserved;
    const int32_t result=local_online_id(online_id) ? 0 : NP_USER_NOT_FOUND;
    if (!result) log_once(&logged_availability,"NP availability ok");
    return finish_request(id,result);
}
/* sceNpGetParentalControlInfo(request, const SceNpOnlineId *, int8_t *age, SceNpParentalControlInfo *):
 * an adult account with no content, chat or user-generated content restriction. */
static ABI int32_t np_parental_control(int32_t id, const char *online_id, int8_t *age, uint8_t *info) {
    if (!local_online_id(online_id)) return finish_request(id,NP_USER_NOT_FOUND);
    if (age) *age=20;
    if (info) memset(info,0,3);
    log_once(&logged_parental,"NP parental control: no restrictions");
    return finish_request(id,0);
}
/* sceNpCheckPlus(request, const SceNpCheckPlusParameter *, SceNpCheckPlusResult *): authorized. */
static ABI int32_t np_check_plus(int32_t id, const void *param, uint8_t *result) {
    if (!param || !result) return NP_INVALID_ARGUMENT;
    const int32_t r=finish_request(id,0);
    if (r) return r;
    result[0]=1;
    log_once(&logged_plus,"NP online multiplayer membership granted");
    return 0;
}
/* sceNpGetAccountId(const SceNpOnlineId *, uint64_t *) */
static ABI int32_t np_account_id(const char *online_id, uint64_t *out) {
    if (!online_id || !out) return NP_INVALID_ARGUMENT;
    if (!local_online_id(online_id)) return NP_USER_NOT_FOUND;
    *out=account;
    return 0;
}

/* ---- NetCtl: connected, IP obtained; the state never changes, so no event is sent ---- */
static ABI int32_t netctl_state(int32_t *state) {
    if (!state) return NET_CTL_INVALID_ADDR;
    *state=NET_CTL_STATE_IPOBTAINED;
    return 0;
}
static ABI int32_t netctl_info(int32_t code, char *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    if (code!=NET_CTL_INFO_IP_ADDRESS) return NET_CTL_NOT_CONNECTED;
    memset(info,0,16);
    snprintf(info,16,"%u.%u.%u.%u",local_ip>>24,(local_ip>>16)&255,(local_ip>>8)&255,local_ip&255);
    return 0;
}
static ABI int32_t netctl_register(void *callback, void *arg, int32_t *id) {
    if (!id || !callback) return NET_CTL_INVALID_ADDR;
    pthread_mutex_lock(&lock);
    int32_t r=NET_CTL_INVALID_ADDR;
    for (int i=0;i<MAX_CALLBACKS;++i) if (!netctl_callbacks[i].used) {
        netctl_callbacks[i].used=1; netctl_callbacks[i].callback=callback; netctl_callbacks[i].arg=arg;
        *id=i+1; r=0;
        break;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t netctl_unregister(int32_t id) {
    if (id<1 || id>MAX_CALLBACKS) return NET_CTL_INVALID_ADDR;
    pthread_mutex_lock(&lock);
    netctl_callbacks[id-1].used=0;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* SceNetCtlNatInfo: size, STUN status @4 (1: checked), NAT type @8 (2), mapped address @12. */
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1]=1; info[2]=2; info[3]=__builtin_bswap32(local_ip);
    return 0;
}

static const RuntimeExport exports[]={
    {"sceNpRegisterStateCallback",np_register_state}, {"sceNpUnregisterStateCallback",np_unregister_state},
    {"sceNpCheckCallback",np_check_callback},
    {"sceNpCreateAsyncRequest",np_create_request}, {"sceNpDeleteRequest",np_delete_request},
    {"sceNpAbortRequest",np_abort_request}, {"sceNpPollAsync",np_poll},
    {"sceNpCheckNpAvailability",np_check_availability}, {"sceNpGetParentalControlInfo",np_parental_control},
    {"sceNpCheckPlus",np_check_plus}, {"sceNpGetAccountId",np_account_id},
    {"sceNpAuthCreateAsyncRequest",np_create_request}, {"sceNpAuthDeleteRequest",np_delete_request},
    {"sceNpAuthAbortRequest",np_abort_request}, {"sceNpAuthGetAuthorizationCode",auth_get_code},
    {"sceNpAuthPollAsync",auth_poll},
    {"sceNpRegisterPlusEventCallback",np_ok}, {"sceNpUnregisterPlusEventCallback",np_ok},
    {"sceNpNotifyPlusFeature",np_ok}, {"sceNpSetNpTitleId",np_ok}, {"sceNpSetContentRestriction",np_ok},
    {"sceNetCtlGetState",netctl_state}, {"sceNetCtlGetInfo",netctl_info},
    {"sceNetCtlRegisterCallback",netctl_register}, {"sceNetCtlUnregisterCallback",netctl_unregister},
    {"sceNetCtlCheckCallback",np_ok}, {"sceNetCtlGetNatInfo",netctl_nat},
};
uintptr_t runtime_np_resolve(const char *name) { return enabled ? RUNTIME_LOOKUP(exports,name) : 0; }
