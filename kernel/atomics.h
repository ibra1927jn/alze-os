/*
 * Anykernel OS — Atomic Operations
 *
 * Lock-free primitives for SMP-safe counters and flags.
 * Uses GCC/Clang __atomic builtins which compile to proper
 * x86_64 instructions (LOCK XADD, LOCK CMPXCHG, etc.).
 *
 * Inspired by: Linux atomic_t, macOS OSAtomic, C11 <stdatomic.h>.
 */

#ifndef ATOMICS_H
#define ATOMICS_H

#include <stdint.h>
#include <stdbool.h>

/* ── Atomic integer type ─────────────────────────────────────── */

typedef struct {
    volatile int64_t value;
} atomic_t;

#define ATOMIC_INIT(v) { .value = (v) }

/* ── Read / Write ────────────────────────────────────────────── */

static inline int64_t atomic_read(const atomic_t *a) {
    return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST);
}

static inline void atomic_set(atomic_t *a, int64_t val) {
    __atomic_store_n(&a->value, val, __ATOMIC_SEQ_CST);
}

/* ── Arithmetic (returns NEW value) ──────────────────────────── */

static inline int64_t atomic_add(atomic_t *a, int64_t val) {
    return __atomic_add_fetch(&a->value, val, __ATOMIC_SEQ_CST);
}

static inline int64_t atomic_sub(atomic_t *a, int64_t val) {
    return __atomic_sub_fetch(&a->value, val, __ATOMIC_SEQ_CST);
}

static inline int64_t atomic_inc(atomic_t *a) {
    return atomic_add(a, 1);
}

static inline int64_t atomic_dec(atomic_t *a) {
    return atomic_sub(a, 1);
}

/* ── Compare-And-Swap ────────────────────────────────────────── */

/*
 * If *a == expected, set *a = desired, return true.
 * Otherwise, set *expected = current value of *a, return false.
 */
static inline bool atomic_cas(atomic_t *a, int64_t *expected, int64_t desired) {
    return __atomic_compare_exchange_n(
        &a->value, expected, desired,
        false,  /* strong CAS */
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST
    );
}

/* ── Exchange (returns OLD value) ────────────────────────────── */

static inline int64_t atomic_xchg(atomic_t *a, int64_t val) {
    return __atomic_exchange_n(&a->value, val, __ATOMIC_SEQ_CST);
}

/* ── Test-and-set (flag operations) ──────────────────────────── */

static inline bool atomic_test_and_set(atomic_t *a) {
    return __atomic_test_and_set(&a->value, __ATOMIC_SEQ_CST);
}

static inline void atomic_clear(atomic_t *a) {
    __atomic_clear(&a->value, __ATOMIC_SEQ_CST);
}

/* ── Memory barriers ─────────────────────────────────────────── */

#define atomic_fence()       __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define atomic_fence_acquire() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define atomic_fence_release() __atomic_thread_fence(__ATOMIC_RELEASE)

#endif /* ATOMICS_H */
