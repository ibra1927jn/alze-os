/*
 * Anykernel OS — Per-CPU Data (GS Segment)
 *
 * Each CPU gets a private `cpu_local` struct pointed to by the GS
 * segment register (MSR 0xC0000101 = GS_BASE).
 *
 * In SMP, each Application Processor (AP) will set its own GS_BASE
 * during AP startup, giving every CPU its own:
 *   - current running task
 *   - idle task
 *   - local tick counter
 *   - CPU ID
 *
 * Access is 1 cycle via `mov %gs:OFFSET, %reg` — zero-lock overhead.
 *
 * Inspired by:
 *   Linux  — current_task = per_cpu(current_task, cpu)
 *   macOS  — cpu_data_t via GS segment
 *   Windows — KPCR via GS:0
 */

#ifndef PERCPU_H
#define PERCPU_H

#include <stdint.h>

/* Forward declaration */
struct task;

/* ── Per-CPU data structure ──────────────────────────────────── */

/*
 * Layout is ABI-critical: offsets are hardcoded in inline asm.
 * Do NOT reorder fields without updating the asm accessors below.
 */
struct cpu_local {
    struct cpu_local *self;          /* offset  0: self-pointer (validation) */
    struct task      *current;       /* offset  8: running task on THIS cpu  */
    struct task      *idle;          /* offset 16: idle task for THIS cpu    */
    uint32_t          cpu_id;        /* offset 24: LAPIC ID                 */
    uint32_t          need_resched;  /* offset 28: reschedule flag           */
    uint64_t          ticks;         /* offset 32: local tick counter        */
};

/* MSR for GS base — x86_64 kernel mode uses MSR_GS_BASE (0xC0000101) */
#define MSR_GS_BASE 0xC0000101

/* ── MSR read/write ──────────────────────────────────────────── */

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    asm volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* ── GS-based per-CPU accessors (1 cycle, no lock) ───────────── */

static inline struct task *get_current(void) {
    struct task *t;
    asm volatile("mov %%gs:8, %0" : "=r"(t));
    return t;
}

static inline void set_current(struct task *t) {
    asm volatile("mov %0, %%gs:8" :: "r"(t) : "memory");
}

static inline struct task *get_idle(void) {
    struct task *t;
    asm volatile("mov %%gs:16, %0" : "=r"(t));
    return t;
}

static inline void set_idle(struct task *t) {
    asm volatile("mov %0, %%gs:16" :: "r"(t) : "memory");
}

static inline uint32_t get_cpu_id(void) {
    uint32_t id;
    asm volatile("mov %%gs:24, %0" : "=r"(id));
    return id;
}

static inline uint32_t get_need_resched(void) {
    uint32_t r;
    asm volatile("mov %%gs:28, %0" : "=r"(r));
    return r;
}

static inline void set_need_resched(uint32_t val) {
    asm volatile("mov %0, %%gs:28" :: "r"(val) : "memory");
}

/* ── Init: called once by BSP, once per AP in future ─────────── */

void percpu_init_bsp(void);

#endif /* PERCPU_H */
