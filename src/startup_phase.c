#include "startup_phase.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#define MAX_CHECKPOINT_LINES 96

static const char *current_phase = "process start";
static int display_width, display_height;
static char display_resolution[16], display_aspect[16], video_path[160];
static char last_checkpoint[2][160];
static unsigned checkpoint_count, published_checkpoint;

static void copy_text(char *to, size_t size, const char *from) {
    size_t length = from ? strlen(from) : 0;
    if (length >= size) length = size - 1;
    memcpy(to, from, length);
    to[length] = 0;
}

static double uptime_seconds(void);

void aln_startup_phase(const char *phase) {
    uptime_seconds();
    __atomic_store_n(&current_phase, phase, __ATOMIC_RELEASE);
}

const char *aln_startup_phase_current(void) {
    return __atomic_load_n(&current_phase, __ATOMIC_ACQUIRE);
}

void aln_crash_context_set_display(int width, int height, const char *resolution, const char *aspect_ratio) {
    display_width = width;
    display_height = height;
    copy_text(display_resolution, sizeof(display_resolution), resolution);
    copy_text(display_aspect, sizeof(display_aspect), aspect_ratio);
}

void aln_crash_context_set_video(const char *path) {
    copy_text(video_path, sizeof(video_path), path);
}

#define THREAD_LABELS 128
static struct { unsigned long thread; char name[32]; } thread_labels[THREAD_LABELS];
static unsigned thread_label_count;

static unsigned long current_thread(void);

unsigned long aln_thread_id(void) { return current_thread(); }

void aln_thread_label_set(const char *name) {
    const unsigned long self = current_thread();
    for (unsigned i = 0; i < THREAD_LABELS && i < __atomic_load_n(&thread_label_count, __ATOMIC_ACQUIRE); ++i)
        if (thread_labels[i].thread == self) {
            copy_text(thread_labels[i].name, sizeof(thread_labels[i].name), name);
            return;
        }
    const unsigned slot = __atomic_fetch_add(&thread_label_count, 1, __ATOMIC_ACQ_REL);
    if (slot >= THREAD_LABELS) return;
    copy_text(thread_labels[slot].name, sizeof(thread_labels[slot].name), name);
    __atomic_store_n(&thread_labels[slot].thread, self, __ATOMIC_RELEASE);
}

const char *aln_thread_label(unsigned long thread) {
    const unsigned count = __atomic_load_n(&thread_label_count, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < THREAD_LABELS && i < count; ++i)
        if (__atomic_load_n(&thread_labels[i].thread, __ATOMIC_ACQUIRE) == thread) return thread_labels[i].name;
    return "";
}

static unsigned long current_thread(void) {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return (unsigned long)(uintptr_t)pthread_self();
#endif
}

static double uptime_seconds(void) {
    static double start = -1;
#ifdef _WIN32
    const double now = (double)GetTickCount64() / 1000.0;
#else
    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec);
    const double now = (double)spec.tv_sec + (double)spec.tv_nsec / 1e9;
#endif
    if (start < 0) start = now;
    return now - start;
}

void aln_checkpoint_note(const char *text) {
    const unsigned slot = (__atomic_load_n(&published_checkpoint, __ATOMIC_ACQUIRE) + 1) & 1;
    char note[160];
    snprintf(note, sizeof(note), "[thread %lu] %s", current_thread(), text ? text : "");
    copy_text(last_checkpoint[slot], sizeof(last_checkpoint[slot]), note);
    __atomic_store_n(&published_checkpoint, slot, __ATOMIC_RELEASE);
}

void aln_checkpoint(const char *format, ...) {
    char text[140];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    aln_checkpoint_note(text);
    if (__atomic_add_fetch(&checkpoint_count, 1, __ATOMIC_RELAXED) <= MAX_CHECKPOINT_LINES)
        printf("Checkpoint: +%.3f s thread %lu: %s\n", uptime_seconds(), current_thread(), text);
}

static size_t append(char *out, size_t size, size_t at, const char *text) {
    while (*text && at + 1 < size) out[at++] = *text++;
    out[at < size ? at : size - 1] = 0;
    return at;
}

static size_t append_number(char *out, size_t size, size_t at, int value) {
    char digits[12];
    int count = 0;
    unsigned remaining = value < 0 ? 0u : (unsigned)value;
    do digits[count++] = (char)('0' + remaining % 10); while ((remaining /= 10) && count < 11);
    while (count) {
        char one[2] = {digits[--count], 0};
        at = append(out, size, at, one);
    }
    return at;
}

size_t aln_crash_context_describe(char *out, size_t size) {
    if (!size) return 0;
    out[0] = 0;
    size_t at = append(out, size, 0, "phase=");
    at = append(out, size, at, aln_startup_phase_current());
    at = append(out, size, at, ", render ");
    at = append_number(out, size, at, display_width);
    at = append(out, size, at, "x");
    at = append_number(out, size, at, display_height);
    at = append(out, size, at, " (");
    at = append(out, size, at, display_resolution);
    at = append(out, size, at, ") ");
    at = append(out, size, at, display_aspect);
    at = append(out, size, at, ", video=");
    at = append(out, size, at, video_path[0] ? video_path : "none");
    at = append(out, size, at, ", last checkpoint=");
    const char *note = last_checkpoint[__atomic_load_n(&published_checkpoint, __ATOMIC_ACQUIRE)];
    at = append(out, size, at, note[0] ? note : "none");
    return at;
}
