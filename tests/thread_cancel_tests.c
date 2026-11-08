/* Focused tests of scePthreadCancel: a cancelled guest thread leaves every kind of blocking wait and can be joined. */
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define CANCELED ((void *)1)
#define ERR_SRCH ((int32_t)0x80020003)
#define NORMAL_RESULT ((void *)42)

typedef void *(ABI *Entry)(void *);
typedef int32_t (ABI *CreateFn)(void **out, void *attr, Entry entry, void *arg, const char *name);
typedef int32_t (ABI *JoinFn)(void *thread, void **result);
typedef int32_t (ABI *CancelFn)(void *thread);
typedef int32_t (ABI *MutexInitFn)(void **mutex, void *attr, const char *name);
typedef int32_t (ABI *MutexFn)(void **mutex);
typedef int32_t (ABI *CondInitFn)(void **cond, void *attr, const char *name);
typedef int32_t (ABI *CondWaitFn)(void **cond, void **mutex);
typedef int32_t (ABI *SemaCreateFn)(uint32_t *out, const char *name, uint32_t attr, int32_t initial, int32_t maximum, const void *options);
typedef int32_t (ABI *SemaWaitFn)(uint32_t id, int32_t need, uint32_t *timeout);
typedef int32_t (ABI *SemaSignalFn)(uint32_t id, int32_t count);
typedef int32_t (ABI *UsleepFn)(uint32_t usec);
typedef int32_t (ABI *EventCreateFn)(void **out, const char *name, uint32_t attr, uint64_t init, const void *param);
typedef int32_t (ABI *EventWaitFn)(void *ef, uint64_t pattern, uint32_t mode, uint64_t *result, uint32_t *timeout);

static CreateFn create_thread;
static JoinFn join_thread;
static CancelFn cancel_thread;
static MutexInitFn mutex_init;
static MutexFn mutex_lock;
static CondInitFn cond_init;
static CondWaitFn cond_wait;
static SemaCreateFn sema_create;
static SemaWaitFn sema_wait;
static SemaSignalFn sema_signal;
static UsleepFn usleep_guest;
static EventCreateFn event_create;
static EventWaitFn event_wait;

static void *resolve(const char *nid) {
    void *function = (void *)runtime_resolve(nid, 0);
    CHECK(function != NULL);
    return function;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void pause_ms(int ms) {
    struct timespec t = {0, (long)ms * 1000000L};
    nanosleep(&t, NULL);
}

static atomic_int started;
static void *cond_handle, *mutex_handle, *event_handle, *long_thread;
static uint32_t sema_id;

static void *ABI sleeper(void *arg) { (void)arg; atomic_fetch_add(&started, 1); usleep_guest(5000000); return NORMAL_RESULT; }
static void *ABI cond_waiter(void *arg) {
    (void)arg;
    mutex_lock(&mutex_handle);
    atomic_fetch_add(&started, 1);
    for (;;) cond_wait(&cond_handle, &mutex_handle);
}
static void *ABI sema_waiter(void *arg) { (void)arg; atomic_fetch_add(&started, 1); sema_wait(sema_id, 1, NULL); return NORMAL_RESULT; }
static void *ABI event_waiter(void *arg) {
    (void)arg;
    uint64_t bits = 0;
    atomic_fetch_add(&started, 1);
    event_wait(event_handle, 1, 0x21, &bits, NULL);
    return NORMAL_RESULT;
}
static void *ABI joiner(void *arg) { void *result = NULL; atomic_fetch_add(&started, 1); join_thread(arg, &result); return NORMAL_RESULT; }
static void *ABI quick(void *arg) { return arg; }

static void *start(Entry entry, void *arg, const char *name) {
    void *thread = NULL;
    CHECK(create_thread(&thread, NULL, entry, arg, name) == 0 && thread != NULL);
    return thread;
}

static void wait_started(int count) {
    for (int i = 0; i < 200 && atomic_load(&started) < count; ++i) pause_ms(10);
    pause_ms(50);
}

static void expect_cancelled(void *thread, const char *what) {
    void *result = NORMAL_RESULT;
    const double begin = now();
    CHECK(cancel_thread(thread) == 0);
    CHECK(join_thread(thread, &result) == 0);
    if (result != CANCELED || now() - begin > 2.0) printf("  %s: result %p after %.2fs\n", what, result, now() - begin);
    CHECK(result == CANCELED);
    CHECK(now() - begin < 2.0);
}

int main(void) {
    runtime_start(1);
    create_thread = (CreateFn)resolve("6UgtwV+0zb4#libkernel");
    join_thread = (JoinFn)resolve("onNY9Byn-W8#libkernel");
    cancel_thread = (CancelFn)resolve("qBDmpCyGssE#libkernel");
    mutex_init = (MutexInitFn)resolve("cmo1RIYva9o#libkernel");
    mutex_lock = (MutexFn)resolve("9UK1vLZQft4#libkernel");
    cond_init = (CondInitFn)resolve("2Tb92quprl0#libkernel");
    cond_wait = (CondWaitFn)resolve("WKAXJ4XBPQ4#libkernel");
    sema_create = (SemaCreateFn)resolve("188x57JYp0g#libkernel");
    sema_wait = (SemaWaitFn)resolve("Zxa0VhQVTsk#libkernel");
    sema_signal = (SemaSignalFn)resolve("4czppHBiriw#libkernel");
    usleep_guest = (UsleepFn)resolve("1jfXLRVzisc#libkernel");
    event_create = (EventCreateFn)resolve("BpFoboUJoZU#libkernel");
    event_wait = (EventWaitFn)resolve("JTvBflhYazQ#libkernel");
    if (failures) return 1;

    CHECK(mutex_init(&mutex_handle, NULL, "m") == 0);
    CHECK(cond_init(&cond_handle, NULL, "c") == 0);
    CHECK(sema_create(&sema_id, "s", 1, 0, 10, NULL) == 0);
    CHECK(event_create(&event_handle, "e", 0x21, 0, NULL) == 0);

    void *quick_thread = start(quick, NORMAL_RESULT, "quick");
    void *quick_result = NULL;
    CHECK(join_thread(quick_thread, &quick_result) == 0 && quick_result == NORMAL_RESULT);
    CHECK(cancel_thread(quick_thread) == ERR_SRCH);
    CHECK(cancel_thread((void *)0x1234) == ERR_SRCH);

    atomic_store(&started, 0);
    void *thread = start(sleeper, NULL, "sleeper");
    wait_started(1);
    CHECK(cancel_thread(thread) == 0);
    CHECK(cancel_thread(thread) == 0);
    void *result = NULL;
    CHECK(join_thread(thread, &result) == 0 && result == CANCELED);

    atomic_store(&started, 0);
    thread = start(cond_waiter, NULL, "cond");
    wait_started(1);
    expect_cancelled(thread, "condition wait");

    atomic_store(&started, 0);
    thread = start(sema_waiter, NULL, "sema");
    wait_started(1);
    expect_cancelled(thread, "semaphore wait");

    /* The cancelled waiter left the queue: the next signal is kept for the next waiter. */
    CHECK(sema_signal(sema_id, 1) == 0);
    uint32_t timeout = 100000;
    CHECK(sema_wait(sema_id, 1, &timeout) == 0);

    atomic_store(&started, 0);
    thread = start(event_waiter, NULL, "event");
    wait_started(1);
    expect_cancelled(thread, "event flag wait");

    atomic_store(&started, 0);
    long_thread = start(sleeper, NULL, "target");
    wait_started(1);
    void *join_waiter = start(joiner, long_thread, "joiner");
    wait_started(2);
    expect_cancelled(join_waiter, "join");
    expect_cancelled(long_thread, "joined thread");

    atomic_store(&started, 0);
    thread = start(sleeper, NULL, "uncancelled");
    wait_started(1);
    CHECK(cancel_thread(thread) == 0);
    CHECK(join_thread(thread, &result) == 0 && result == CANCELED);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
