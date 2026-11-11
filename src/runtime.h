#ifndef ALN_RUNTIME_H
#define ALN_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include "compat_win.h"
/* No signals on Windows: the exception handler resumes the thread in aln_longjmp
 * (gpu/shim/aln_toggles.h uses the same buffer type). */
typedef aln_jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) aln_setjmp(buffer)
#else
#include <setjmp.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
/* Ends the process without atexit handlers or C++ static destructors: those would tear down the video core under the
 * window, GPU, audio and guest threads, which keep running. */
static inline __attribute__((noreturn)) void runtime_exit(int status) {
    fprintf(stderr,"Runtime: process ends with status %d\n",status);
    fflush(NULL);
    _Exit(status);
}
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int32_t *runtime_errno(void);
int runtime_thread_cancel_pending(void);
void runtime_thread_test_cancel(const char *point);
int runtime_thread_wait_enter(void (*wake)(void *, void *), void *first, void *second);
void runtime_thread_wait_begin(const char *point, void (*wake)(void *, void *), void *first, void *second);
void runtime_thread_wait_end(void);
void runtime_thread_sleep_ns(uint64_t ns);
void runtime_thread_wake_condition(void *cond, void *mutex);
/* ALN_BOOT_DIAGNOSTIC: the calling thread's current blocking wait (NULL ends it); `point` must be a literal. */
void runtime_thread_track_waits(int enabled);
int runtime_thread_tracking_waits(void);
void runtime_thread_note_wait(const char *point, uintptr_t object);
void runtime_thread_diag_dump(void);
void runtime_sema_diag_dump(void);
uintptr_t runtime_sema_resolve(const char *name);
uintptr_t runtime_fiber_resolve(const char *name);
void runtime_fiber_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
/* AudioOut for the runtime's other audio libraries: the paced SDL3 path the game's own ports use, at 48 kHz. */
int32_t runtime_audio_out_init(void);
int32_t runtime_audio_out_open(int32_t type, uint32_t frames, uint32_t param);
int32_t runtime_audio_out_output(int32_t handle, const void *data);
int32_t runtime_audio_out_close(int32_t handle);
uintptr_t runtime_audio3d_resolve(const char *name);
void runtime_audio3d_report(void);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_json_resolve(const char *name);
void runtime_json_report(void);
uintptr_t runtime_aln_resolve(const char *name);
void runtime_aln_set_neo_mode(int on);
/* Direct memory on top of the retail pool, for the larger GPU heap of a larger render target; before the first query. */
void runtime_memory_set_direct_extra(uint64_t bytes);
/* The last guest memory maps and unmaps as "Runtime: memory op" lines; a range holding `focus` is marked. */
size_t runtime_memory_recent(char *out, size_t size, uintptr_t focus);
void runtime_set_language(int ps4_language);
void runtime_user_login(int player);
int runtime_user_player(int32_t user);
uintptr_t runtime_font_resolve(const char *name);
/* The accented letters composed into the font atlas, `cell` pixels square; the data must outlive the process's fonts. */
uintptr_t runtime_eventflag_resolve(const char *name);
/* The alienation.ini whose keyboard bindings the pad reads, once, at the first sample. */
void runtime_pad_set_settings(const char *settings_file);
/* 1 when the slot is Player 1 or has a live gamepad; 0 for a slot the game may drive with AI. */
int runtime_pad_slot_human(int slot);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
/* Online play for this process (runtime_online.cpp): reads the settings, the profile's online identity and the game
 * build ("CUSA00062-1.00"; NULL for an unknown build, which stays offline), then configures the NP layer. Before the
 * imports are bound. */
void runtime_online_configure(const char *settings_file, const char *user_dir, const char *build);
int runtime_online_active(void);
/* Matching2 and the game's P2P socket while online play is on; 0 otherwise or for another import. */
uintptr_t runtime_online_resolve(const char *name);
/* Lobby messages, the endpoint exchange and the queued Matching2 callbacks, on the thread calling sceNpCheckCallback. */
void runtime_online_pump(void);
/* The initial user signed in as `online_id` (16 characters at most), the network connected at `ip` (host byte
 * order). Off: runtime_np_resolve answers nothing and runtime_services.c keeps the offline behaviour. */
void runtime_np_configure(int enabled, uint64_t account, const char *online_id, uint32_t ip);
int runtime_np_enabled(void);
/* Called at the end of every sceNpCheckCallback while online play is on. */
void runtime_np_set_pump(void (*pump)(void));
uintptr_t runtime_np_resolve(const char *name);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
typedef struct {
    uint64_t offset, size;
    const void *data;
} RuntimeFilePatch;
/* The guest sees `guest` as `length` bytes: the host file, zeros past its end, and the patches on top. The patches'
 * data is borrowed and must outlive the process. Before the guest starts only. A path registered again with the same
 * length gets the new patches on top of the earlier ones. -1 with nothing registered when the table is full, the path
 * already has an overlay of another length or a patch falls outside `length`. */
int runtime_file_overlay(const char *guest, uint64_t length, const RuntimeFilePatch *patches, size_t count);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
/* Called after every successful read with the guest path, the file offset and the size (Discord presence). It runs on
 * the reading thread and must not block. */
void runtime_file_set_read_observer(void (*observe)(const char *guest, uint64_t offset, uint64_t size));
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
#ifndef _WIN32
/* Stacks below 1 TiB with a guard page at the bottom; released stacks are reused. */
void *runtime_stack_acquire(size_t size);
void runtime_stack_release(void *stack, size_t size);
#endif
/* Windows: reserves the PS4 user address range as a placeholder (no-op elsewhere). */
void runtime_memory_reserve(void);
/* Creates the guest memory pool and commits a first page; nonzero when that fails. */
int runtime_memory_prepare(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
/* OrbisSaveDataParam (1328 bytes) of a save directory of the current title. */
#define RUNTIME_SAVE_PARAM_SIZE 1328
int runtime_savedata_param(int32_t user, const char *dir, void *param);
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size);
void runtime_thread_attach_host(const char *name);
/* Name of a guest thread handle, or NULL when it is not a live thread. */
const char *runtime_thread_name(void *handle);
uint64_t runtime_memory_flexible_available(void);
#ifdef __cplusplus
}
#endif
#endif
