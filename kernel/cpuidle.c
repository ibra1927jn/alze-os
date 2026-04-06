/*
 * Anykernel OS — CPU Idle States Implementation
 *
 * Linux:   cpuidle governor selects C-state based on predicted idle time.
 * macOS:   Timer coalescing + App Nap let CPU reach deep C-states.
 * Windows: Idle loop uses MWAIT with C-state hints.
 *
 * We detect MWAIT via CPUID and use it when available.
 * Combined with tickless timer (pit_set_oneshot/pit_stop),
 * the CPU truly sleeps between events.
 */

#include "cpuidle.h"
#include "pic.h"
#include "log.h"
#include <stdint.h>

/* ── Constants ───────────────────────────────────────────────── */

#define CPUID_MWAIT_BIT   (1 << 3)  /* CPUID leaf 1, ECX bit 3: MONITOR/MWAIT */
#define MWAIT_C1_HINT     0x00      /* C1 state hint for MWAIT instruction     */

/* ── State ────────────────────────────────────────────────────── */

static int has_mwait = 0;
static int deepest_state = IDLE_HLT;  /* Default: HLT */
static volatile uint64_t total_idle_ticks = 0;

/* ── CPUID Detection ─────────────────────────────────────────── */

static inline void cpuid_leaf(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                              uint32_t *ecx, uint32_t *edx) {
    asm volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

void cpuidle_init(void) {
    uint32_t eax, ebx, ecx, edx;

    /* Check CPUID leaf 1, ECX bit 3 = MONITOR/MWAIT */
    cpuid_leaf(1, &eax, &ebx, &ecx, &edx);

    if (ecx & CPUID_MWAIT_BIT) {
        has_mwait = 1;
        deepest_state = IDLE_MWAIT;
        LOG_OK("cpuidle: MWAIT supported — deep idle enabled");
    } else {
        has_mwait = 0;
        deepest_state = IDLE_HLT;
        LOG_OK("cpuidle: MWAIT not available — using HLT");
    }
}

/* ── Idle Entry ───────────────────────────────────────────────── */

/*
 * MWAIT usage:
 *   1. MONITOR sets up an address range to watch.
 *   2. MWAIT enters an implementation-dependent optimized state.
 *   3. CPU wakes when the monitored range is written or an IRQ fires.
 *
 * We use a dummy variable as the monitor target.
 * Hint = 0x00 → C1 (lightest MWAIT state, universal support).
 */
static volatile uint64_t mwait_target __attribute__((aligned(64))) = 0;

void cpu_idle(void) {
    uint64_t start = pit_get_ticks();

    if (has_mwait) {
        /* MONITOR: watch mwait_target for writes */
        asm volatile("monitor" :: "a"(&mwait_target), "c"(0), "d"(0));
        /* MWAIT: enter C1 state */
        asm volatile("mwait" :: "a"(MWAIT_C1_HINT), "c"(0));
    } else {
        /* Classic HLT — wakes on any interrupt */
        asm volatile("hlt");
    }

    uint64_t end = pit_get_ticks();
    total_idle_ticks += (end - start);
}

/* ── Queries ──────────────────────────────────────────────────── */

int cpuidle_deepest_state(void) {
    return deepest_state;
}

int cpuidle_has_mwait(void) {
    return has_mwait;
}

uint64_t cpuidle_total_idle(void) {
    return total_idle_ticks;
}
