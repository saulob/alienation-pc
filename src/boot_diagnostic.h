/* On unless ALN_BOOT_DIAGNOSTIC=0: bounded lifecycle events, and a snapshot of thread waits when guest flips stop. */
#ifndef ALN_BOOT_DIAGNOSTIC_H
#define ALN_BOOT_DIAGNOSTIC_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ALN_DIAG_FLIP,
    ALN_DIAG_PRESENT,
    ALN_DIAG_AVPLAYER_OPEN,
    ALN_DIAG_AVPLAYER_CLOSE_BEGIN,
    ALN_DIAG_AVPLAYER_CLOSE_END,
    ALN_DIAG_COUNTERS
};

int aln_boot_diag_enabled(void);
/* Starts the stall watchdog; does nothing unless enabled. */
void aln_boot_diag_start(void);
void aln_boot_diag_count(int counter);
void aln_boot_diag_event(const char *format, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
