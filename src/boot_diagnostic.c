#include "boot_diagnostic.h"
#include "runtime.h"
#include "startup_phase.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_EVENTS 300
#define MAX_SNAPSHOTS 6
#define STALL_MS 5000

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int enabled;
static uint64_t started_ms;
static uint64_t counters[ALN_DIAG_COUNTERS];
static unsigned events;

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void read_setting(void) {
    const char *value = getenv("ALN_BOOT_DIAGNOSTIC");
    enabled = !value || atoi(value) > 0;
    started_ms = now_ms();
}

int aln_boot_diag_enabled(void) {
    pthread_once(&once, read_setting);
    return enabled;
}

void aln_boot_diag_count(int counter) {
    if (counter >= 0 && counter < ALN_DIAG_COUNTERS && aln_boot_diag_enabled())
        __atomic_add_fetch(&counters[counter], 1, __ATOMIC_RELAXED);
}

static double elapsed_s(void) { return (double)(now_ms() - started_ms) / 1000.0; }

void aln_boot_diag_event(const char *format, ...) {
    if (!aln_boot_diag_enabled()) return;
    const unsigned n = __atomic_add_fetch(&events, 1, __ATOMIC_RELAXED);
    if (n > MAX_EVENTS) return;
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    aln_checkpoint_note(text);
    printf("BootDiag: +%.3f s [thread %lu] %s%s\n", elapsed_s(), aln_thread_id(), text,
           n == MAX_EVENTS ? " (event limit reached)" : "");
}

static uint64_t counter(int which) { return __atomic_load_n(&counters[which], __ATOMIC_RELAXED); }

static void snapshot(unsigned number, uint64_t stalled_ms) {
    const uint64_t closes_begun = counter(ALN_DIAG_AVPLAYER_CLOSE_BEGIN), closes_done = counter(ALN_DIAG_AVPLAYER_CLOSE_END);
    printf("BootDiag: snapshot %u at +%.3f s: no guest flip for %llu ms (flips=%llu, presents=%llu), phase '%s'\n", number,
           elapsed_s(), (unsigned long long)stalled_ms, (unsigned long long)counter(ALN_DIAG_FLIP),
           (unsigned long long)counter(ALN_DIAG_PRESENT), aln_startup_phase_current());
    printf("BootDiag: AvPlayer %llu open, %llu closing\n",
           (unsigned long long)(counter(ALN_DIAG_AVPLAYER_OPEN) - closes_done),
           (unsigned long long)(closes_begun - closes_done));
    runtime_thread_diag_dump();
    runtime_sema_diag_dump();
    runtime_savedata_report();
    runtime_file_report();
    printf("BootDiag: end of snapshot %u\n", number);
}

static void *watchdog(void *unused) {
    (void)unused;
    uint64_t last_flips = counter(ALN_DIAG_FLIP), last_progress = now_ms();
    unsigned taken = 0;
    int armed = 1;
    while (taken < MAX_SNAPSHOTS) {
        const struct timespec second = {1, 0};
        nanosleep(&second, NULL);
        const uint64_t flips = counter(ALN_DIAG_FLIP), now = now_ms();
        if (flips != last_flips) {
            last_flips = flips;
            last_progress = now;
            armed = 1;
        } else if (armed && now - last_progress >= STALL_MS) {
            armed = 0;
            snapshot(++taken, now - last_progress);
        }
    }
    return NULL;
}

void aln_boot_diag_start(void) {
    if (!aln_boot_diag_enabled()) return;
    runtime_thread_track_waits(1);
    pthread_t thread;
    if (pthread_create(&thread, NULL, watchdog, NULL)) {
        puts("BootDiag: watchdog thread not started");
        return;
    }
    pthread_detach(thread);
    printf("BootDiag: enabled; a snapshot follows %d s without a guest flip (at most %d)\n", STALL_MS / 1000, MAX_SNAPSHOTS);
}
