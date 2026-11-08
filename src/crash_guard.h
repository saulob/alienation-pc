/* Windows: what happens to an exception nothing recovers. The crashing thread writes a one-line report from static
 * memory first; the rest of the report and a minidump follow under a watchdog, so a lock held elsewhere cannot hide
 * the crash. The process ends with the exception code. */
#ifndef ALN_CRASH_GUARD_H
#define ALN_CRASH_GUARD_H

#include <stddef.h>
#include <stdint.h>
#include <windows.h>

typedef enum { ALN_FAULT_IGNORE, ALN_FAULT_CRASH } AlnFaultAction;

typedef struct {
    /* The detailed report, on the crashing thread, after the one-line report. Optional. */
    void (*detail)(EXCEPTION_POINTERS *info);
    /* "module+0x..." for an address, without taking locks; returns 0 when it has nothing to say. Optional. */
    int (*describe)(char *out, size_t size, uintptr_t address);
    /* Recent guest memory operations, "newest last", with the one holding `address` marked. Optional. */
    size_t (*memory_context)(char *out, size_t size, uintptr_t address);
} AlnCrashHooks;

/* A hardware exception code that always ends the process, one that ends it when the faulting code is the game's or the
 * port's, or one that is left to whoever raised it. `ours` is 0 for code in another module. */
AlnFaultAction aln_crash_classify(uint32_t code, int in_guest, int ours);
const char *aln_crash_code_name(uint32_t code);

/* Installs the unhandled-exception filter and starts the crash thread. The log must exist. */
void aln_crash_guard_install(const AlnCrashHooks *hooks);
/* The game image: addresses in it are the game's, whatever module lookups say. */
void aln_crash_guard_set_guest(uintptr_t image, uint64_t size);

/* The fatal part of the vectored handler: returns EXCEPTION_CONTINUE_SEARCH for an exception left to its owner, and
 * does not return for one it reports. */
LONG aln_crash_guard_vectored(EXCEPTION_POINTERS *info);
/* Ends the process after reporting `info`, from any thread. */
__attribute__((noreturn)) void aln_crash_guard_report(EXCEPTION_POINTERS *info, const char *note);

/* The same for a failure that is not a hardware exception (abort(), a failed C runtime check): `code` names it in the
 * report and the process ends with `exit_code`. */
__attribute__((noreturn)) void aln_crash_guard_report_here(const char *what, uint32_t code, uint32_t exit_code);

/* Puts the filter back when a library replaced it, and says which; 1 when it had been replaced. */
int aln_crash_guard_verify(void);

/* The minidump path: the log's path with .dmp, "_N" before it from attempt 2; 0 when it does not fit. */
int aln_crash_dump_path(char *out, size_t size, const char *log_path, int attempt);

#endif
