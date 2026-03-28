/*
 * Anykernel OS v2.1 — Stack Smashing Protector (SSP)
 *
 * Provides the canary value and failure handler required by
 * -fstack-protector-strong. The canary is initialized with
 * RDTSC to avoid a predictable constant in the binary.
 */

#include <stdint.h>
#include "panic.h"

/* ── Stack canary ─────────────────────────────────────────────── */

/*
 * Initial canary value — overwritten by ssp_init() with RDTSC.
 * The compiler inserts this value at function entry and checks it
 * at function exit. If it changed, a stack buffer overflow occurred.
 */
uintptr_t __stack_chk_guard = 0x595E9FBD94FDA766;

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
    tsc ^= (tsc >> 17);
    tsc ^= (tsc << 13);
    tsc ^= (tsc >> 7);
    /* Ensure at least one null byte to catch string overflows */
    tsc &= ~(uint64_t)0xFF;
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
