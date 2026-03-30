/*
 * Anykernel OS — Runtime Test Suite
 *
 * Post-boot integration tests: stress, benchmarks, timer, leaks.
 * Separated from main.c for modularity.
 */

#include <stdint.h>
#include "sched.h"
#include "task.h"
#include "spinlock.h"
#include "msgqueue.h"
#include "pic.h"
#include "pmm.h"
#include "kprintf.h"
#include "log.h"

/* ── Stress test ─────────────────────────────────────────────── */

/* Timer accuracy: expected 200ms sleep, tolerance window in ms */
#define TIMER_EXPECTED_MS   200
#define TIMER_TOLERANCE_LO  190
#define TIMER_TOLERANCE_HI  250

#define STRESS_THREADS   10
#define STRESS_ITERS     10000
#define STRESS_EXPECTED  (STRESS_THREADS * STRESS_ITERS)
static volatile uint64_t shared_counter = 0;
static spinlock_t counter_lock = SPINLOCK_INIT;

static void stress_thread_fn(void) {
    for (int i = 0; i < STRESS_ITERS; i++) {
        uint64_t flags;
        spin_lock_irqsave(&counter_lock, &flags);
        shared_counter++;
        spin_unlock_irqrestore(&counter_lock, flags);
        if (i % 1000 == 0) sched_yield();
    }
}

/* ── RDTSC ───────────────────────────────────────────────────── */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ── Context switch benchmark ────────────────────────────────── */

#define BENCH_SWITCHES 1000
static void bench_ping_fn(void) {
    for (int i = 0; i < BENCH_SWITCHES; i++) sched_yield();
}

/* ── IPC benchmark ───────────────────────────────────────────── */

static struct msg_queue bench_mq;
#define IPC_MSGS 500
static void ipc_sender_fn(void) {
    uint32_t msg = 0xCAFE;
    for (int i = 0; i < IPC_MSGS; i++) mq_send(&bench_mq, &msg, sizeof(msg));
}
static void ipc_receiver_fn(void) {
    uint32_t buf, len;
    for (int i = 0; i < IPC_MSGS; i++) mq_recv(&bench_mq, &buf, &len, 0);
}

/* ── Timer accuracy ──────────────────────────────────────────── */

static volatile uint64_t sleep_start_tick = 0;
static volatile uint64_t sleep_end_tick = 0;
static void timer_test_fn(void) {
    sleep_start_tick = pit_get_ticks();
    task_sleep(200);
    sleep_end_tick = pit_get_ticks();
}

/* ── Run all runtime tests ───────────────────────────────────── */

void run_runtime_tests(void) {
    uint64_t pmm_free_before = pmm_free_count();

    /* ══ TEST 1: Stress ═════════════════════════════════════════ */
    kprintf("\n--- [TEST 1] Stress: %d threads x %d ---\n",
            STRESS_THREADS, STRESS_ITERS);
    shared_counter = 0;
    int stress_tids[STRESS_THREADS];
    for (int i = 0; i < STRESS_THREADS; i++) {
        char name[16];
        ksnprintf(name, sizeof(name), "stress-%d", i);
        stress_tids[i] = task_create(name, stress_thread_fn);
    }
    for (int i = 0; i < STRESS_THREADS; i++)
        task_join((uint32_t)stress_tids[i]);

    if (shared_counter == STRESS_EXPECTED)
        LOG_OK("Stress PASSED: %lu/%d", shared_counter, STRESS_EXPECTED);
    else
        LOG_ERROR("Stress FAILED: %lu/%d", shared_counter, STRESS_EXPECTED);

    /* ══ TEST 2: Context switch benchmark ═══════════════════════ */
    kprintf("\n--- [TEST 2] Context switch benchmark ---\n");
    int bench_tid = task_create("bench-ping", bench_ping_fn);
    uint64_t ts_start = rdtsc();
    for (int i = 0; i < BENCH_SWITCHES; i++) sched_yield();
    uint64_t ts_end = rdtsc();
    task_join((uint32_t)bench_tid);
    uint64_t per_switch = (ts_end - ts_start) / (BENCH_SWITCHES * 2);
    LOG_OK("Context switch: %lu cycles/switch (%d pairs)", per_switch, BENCH_SWITCHES);

    /* ══ TEST 3: IPC benchmark ══════════════════════════════════ */
    kprintf("\n--- [TEST 3] IPC benchmark ---\n");
    mq_init(&bench_mq);
    uint64_t ipc_start = rdtsc();
    int ipc_s = task_create("ipc-send", ipc_sender_fn);
    int ipc_r = task_create("ipc-recv", ipc_receiver_fn);
    task_join((uint32_t)ipc_s);
    task_join((uint32_t)ipc_r);
    uint64_t per_msg = (rdtsc() - ipc_start) / IPC_MSGS;
    LOG_OK("IPC: %lu cycles/msg (%d messages)", per_msg, IPC_MSGS);

    /* ══ TEST 4: Timer accuracy ═════════════════════════════════ */
    kprintf("\n--- [TEST 4] Timer accuracy ---\n");
    int tt = task_create("timer-test", timer_test_fn);
    task_join((uint32_t)tt);
    uint64_t sleep_ms = (sleep_end_tick - sleep_start_tick) * 10;
    if (sleep_ms >= TIMER_TOLERANCE_LO && sleep_ms <= TIMER_TOLERANCE_HI)
        LOG_OK("Timer PASSED: sleep(%dms) = %lu ms", TIMER_EXPECTED_MS, sleep_ms);
    else
        LOG_ERROR("Timer FAILED: sleep(%dms) = %lu ms", TIMER_EXPECTED_MS, sleep_ms);

    /* ══ TEST 5: Memory leak check ══════════════════════════════ */
    kprintf("\n--- [TEST 5] Memory leak check ---\n");
    for (int i = 0; i < 5; i++) task_sleep(20);
    uint64_t pmm_free_after = pmm_free_count();
    int64_t leak = (int64_t)pmm_free_before - (int64_t)pmm_free_after;
    if (leak <= 0)
        LOG_OK("Leak check PASSED: %lu → %lu pages (no leak)", pmm_free_before, pmm_free_after);
    else
        LOG_ERROR("LEAK: %ld pages (%ld KB) not freed", leak, leak * 4);

    /* ══ Final Report ═══════════════════════════════════════════ */
    kprintf("\n══════════════════════════════════════\n");
    kprintf("  Phase 4 COMPLETE  (v0.6.2)\n");
    kprintf("  41 kernel + 5 runtime tests\n");
    kprintf("══════════════════════════════════════\n\n");
}
