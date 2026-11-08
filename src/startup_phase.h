/* The stage the process is in, kept for the crash report. */
#ifndef ALN_STARTUP_PHASE_H
#define ALN_STARTUP_PHASE_H

#ifdef __cplusplus
extern "C" {
#endif

/* `phase` must outlive the process. Nothing is logged. */
void aln_startup_phase(const char *phase);
const char *aln_startup_phase_current(void);

#ifdef __cplusplus
}
#endif

#endif
