/*
 * Anykernel OS — Task Control Block (TCB)
 *
 * Each kernel thread has a TCB that stores its saved state,
 * stack, identity, queue linkage, and scheduling metadata.
 */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "list.h"

/* ── Task states ─────────────────────────────────────────────── */

enum task_state {
    TASK_READY,       /* In the ready queue, waiting for CPU */
    TASK_RUNNING,     /* Currently executing on the CPU */
    TASK_SLEEPING,    /* Sleeping, will be woken at sleep_until tick */
    TASK_DEAD         /* Finished, waiting for reap */
};

/* ── Thread entry point signature ────────────────────────────── */

typedef void (*task_entry_fn)(void);

/* ── Priority levels (64 total, 0 = highest) ────────────────── */

#define TASK_PRIO_LEVELS  64

enum task_priority {
    TASK_PRIO_HIGH   = 0,    /* Kernel critical */
    TASK_PRIO_NORMAL = 32,   /* Default */
    TASK_PRIO_LOW    = 48,   /* Background */
    TASK_PRIO_IDLE   = 63    /* Idle only */
};

/* ── Stack canary ────────────────────────────────────────────── */

#define TASK_STACK_CANARY 0xDEADC0DEDEADC0DEULL

/* Forward decl for join wait queue */
struct wait_queue;

/* Forward decl for per-task fd table */
struct file;

/* ── Task Control Block ──────────────────────────────────────── */

struct task {
    uint64_t           rsp;           /* Saved stack pointer (MUST be first) */
    uint64_t           stack_phys;    /* Physical address of stack allocation */
    uint32_t           tid;           /* Unique thread ID */
    enum task_state    state;
    enum task_priority priority;      /* Scheduling priority */
    task_entry_fn      entry;         /* Entry function (for trampoline) */
    char               name[16];
    struct list_node   run_node;      /* Linkage in ready/dead/sleep queue */

    /* CPU accounting */
    uint64_t           ticks_used;    /* Total PIT ticks this task has run */

    /* Sleep support */
    uint64_t           sleep_until;   /* PIT tick when task should wake */

    /* Join support — wait queue that task_join sleeps on */
    volatile bool      finished;      /* Set when task exits */
    struct wait_queue  *join_wq;      /* Joiners sleep here, woken on exit */

    /* QoS class (macOS-inspired) */
    uint8_t            qos;           /* enum qos_class */

    /* Deadline scheduling (EDF) */
    uint64_t           deadline;      /* Absolute tick deadline (0 = none) */

    /* Watchdog: consecutive ticks without yielding */
    uint32_t           watchdog_ticks; /* Reset on schedule/yield */

    /* Per-task file descriptors */
    struct file        *fd_table;     /* Points to FD_TABLE_SIZE entries */
};

/* ── API ─────────────────────────────────────────────────────── */

/*
 * Create a new kernel thread.
 * Allocates a 16KB stack via PMM, sets up initial context + stack canary.
 * Returns TID, or -1 on failure.
 */
int task_create(const char *name, task_entry_fn entry);

/*
 * Exit the current thread. Marks as DEAD, calls schedule().
 * Stack is freed by the reaper (idle task), NOT here.
 */
__attribute__((noreturn))
void task_exit(void);

/*
 * Get the currently running task.
 */
struct task *task_current(void);

/*
 * Sleep the current thread for approximately `ms` milliseconds.
 * Thread is moved to sleep queue and woken by PIT handler.
 */
void task_sleep(uint64_t ms);

/*
 * Wait for thread `tid` to finish (blocking join).
 * Returns 0 on success, -1 if tid not found.
 */
int task_join(uint32_t tid);

/*
 * Set an absolute deadline (PIT tick) for EDF scheduling.
 * Tasks with deadlines are always preferred over non-deadline tasks.
 * Set to 0 to clear the deadline.
 */
void task_set_deadline(uint64_t deadline_tick);

#endif /* TASK_H */
