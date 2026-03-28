/*
 * Anykernel OS — CPU Idle States
 *
 * Inspired by:
 *   Linux cpuidle framework  — dynamic C-state selection
 *   macOS App Nap / Power Nap — aggressive sleep when idle
 *   Windows NT idle loop — MWAIT + hint-based C-state entry
 *
 * Detects MONITOR/MWAIT support via CPUID and selects the
 * deepest safe idle state based on pending work.
 */

#ifndef CPUIDLE_H
#define CPUIDLE_H

#include <stdint.h>

/* Idle states (Intel C-states mapped to our abstraction) */
#define IDLE_POLL   0   /* Spin (debugging only) */
#define IDLE_HLT    1   /* HLT — wakes on any interrupt */
#define IDLE_MWAIT  2   /* MWAIT C1 — lower power HLT equivalent */

/*
 * Initialize idle subsystem. Detects MWAIT support.
 */
void cpuidle_init(void);

/*
 * Enter the deepest available idle state.
 * Returns when an interrupt wakes the CPU.
 *
 * Before calling: disable preemption / ensure nothing pending.
 * If tickless mode is active, this enters deep sleep.
 */
void cpu_idle(void);

/*
 * Returns the deepest idle state available on this CPU.
 */
int cpuidle_deepest_state(void);

/*
 * Returns 1 if MWAIT is supported, 0 otherwise.
 */
int cpuidle_has_mwait(void);

/*
 * Returns total time spent in idle (in PIT ticks).
 */
uint64_t cpuidle_total_idle(void);

#endif /* CPUIDLE_H */
