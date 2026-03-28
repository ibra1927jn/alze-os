/*
 * Anykernel OS — Compiler & CPU Optimization Macros
 *
 * Portable performance hints that help the compiler and CPU
 * generate faster code without changing program semantics.
 */

#ifndef COMPILER_H
#define COMPILER_H

/* ── Branch prediction hints ─────────────────────────────────── */

/* Tell the compiler which branch is more likely, enabling the CPU
 * branch predictor to speculate correctly on hot paths. */
#define likely(x)    __builtin_expect(!!(x), 1)
#define unlikely(x)  __builtin_expect(!!(x), 0)

/* ── Cache hints ─────────────────────────────────────────────── */

/* L1 cache line size on x86_64 (64 bytes). Used for alignment. */
#define CACHE_LINE_SIZE  64

/* Align a struct or variable to a cache line boundary.
 * Prevents false sharing between CPU cores. */
#define __cacheline_aligned  __attribute__((aligned(CACHE_LINE_SIZE)))

/* Prefetch data into L1 cache before it's needed.
 * Locality 3 = keep in all cache levels (temporal).
 * Locality 0 = don't pollute cache (non-temporal). */
#define prefetch(addr)       __builtin_prefetch((addr), 0, 3)
#define prefetch_w(addr)     __builtin_prefetch((addr), 1, 3)
#define prefetch_nt(addr)    __builtin_prefetch((addr), 0, 0)

/* ── Function attributes ─────────────────────────────────────── */

/* Mark functions that are called very frequently.
 * Compiler optimizes them more aggressively (inlining, alignment). */
#define __hot      __attribute__((hot))

/* Mark functions that are rarely called (error paths, init). */
#define __cold     __attribute__((cold))

/* Prevent compiler from inlining (for code size or debugging). */
#define __noinline __attribute__((noinline))

/* Force inlining even at -O0. */
#define __always_inline inline __attribute__((always_inline))

/* Mark function as pure (no side effects, result depends only on args). */
#define __pure     __attribute__((pure))

/* Mark function as const (like pure, but doesn't read global memory). */
#define __const    __attribute__((const))

/* ── Memory barriers ─────────────────────────────────────────── */

/* Full compiler memory barrier — prevents compiler reordering. */
#define barrier()  asm volatile("" ::: "memory")

/* CPU memory fence — prevents CPU reordering (for SMP). */
#define mb()       asm volatile("mfence" ::: "memory")
#define rmb()      asm volatile("lfence" ::: "memory")
#define wmb()      asm volatile("sfence" ::: "memory")

#endif /* COMPILER_H */
