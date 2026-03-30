/*
 * Anykernel OS v2.1 — Stack Smashing Protector (SSP)
 *
 * Provides the canary value and failure handler required by
 * -fstack-protector-strong. The canary is initialized with
 * RDTSC to avoid a predictable constant in the binary.
 */

#include <stdint.h>
#include "panic.h"

/* ── Constants ───────────────────────────────────────────────── */

/* Default canary — replaced by RDTSC in ssp_init() */
#define SSP_INITIAL_CANARY  0x595E9FBD94FDA766ULL

/* xorshift mixing constants for RDTSC entropy distribution */
#define SSP_MIX_SHIFT_R1    17
#define SSP_MIX_SHIFT_L     13
#define SSP_MIX_SHIFT_R2    7

/* Mask to ensure at least one null byte (catches string overflows) */
#define SSP_NULL_BYTE_MASK  0xFF

/* ── Stack canary ─────────────────────────────────────────────── */

/*
 * Initial canary value — overwritten by ssp_init() with RDTSC.
 * The compiler inserts this value at function entry and checks it
 * at function exit. If it changed, a stack buffer overflow occurred.
 */
uintptr_t __stack_chk_guard = SSP_INITIAL_CANARY;

/* Read Time Stamp Counter for randomization */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Initialize the canary with a non-deterministic value */
void ssp_init(void) {
    uint64_t tsc = rdtsc();
    /* Mix bits for better entropy distribution */
    tsc ^= (tsc >> SSP_MIX_SHIFT_R1);
    tsc ^= (tsc << SSP_MIX_SHIFT_L);
    tsc ^= (tsc >> SSP_MIX_SHIFT_R2);
    /* Ensure at least one null byte to catch string overflows */
    tsc &= ~(uint64_t)SSP_NULL_BYTE_MASK;
    __stack_chk_guard = tsc;
}

/* ── SSP failure handler ──────────────────────────────────────── */

/*
 * Called by compiler-inserted code when the stack canary is corrupted.
 * This means a buffer overflow in Ring 0 — absolutely critical.
 */
__attribute__((noreturn))
void __stack_chk_fail(void) {
    PANIC("Stack Smashing Detected (Buffer Overflow in Ring 0)");
}
