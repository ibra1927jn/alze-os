/*
 * Anykernel OS — Work Queue (Deferred Work in Process Context)
 *
 * Like Linux workqueues or macOS dispatch queues. Allows scheduling
 * work to run later in process context (not IRQ context).
 *
 * Why: IRQ handlers must be fast and can't sleep. If an IRQ needs
 * to do complex work (allocate memory, take a mutex), it queues
 * deferred work instead.
 *
 * Usage:
 *   struct work my_work;
 *   work_init(&my_work, my_function, my_arg);
 *   workqueue_schedule(&my_work);  // Runs later in worker thread
 */

#ifndef WORKQUEUE_H_DEFERRED
#define WORKQUEUE_H_DEFERRED

#include <stdint.h>
#include <stdbool.h>
#include "list.h"
#include "spinlock.h"

/* Work callback */
typedef void (*work_fn)(void *arg);

/* Work item */
struct work {
    struct list_node node;      /* Linkage in work queue */
    work_fn          func;      /* Function to execute */
    void            *arg;       /* Argument */
    volatile bool    pending;   /* Queued but not yet executed */
};

/* Work queue */
struct workqueue {
    struct list_head items;     /* Pending work items */
    spinlock_t       lock;
    const char      *name;
    uint64_t         processed; /* Total items processed */
};

/* ── API ─────────────────────────────────────────────────────── */

/* Initialize a work item */
static inline void work_init(struct work *w, work_fn fn, void *arg) {
    w->func = fn;
    w->arg = arg;
    w->pending = false;
}

/* Initialize a work queue */
static inline void workqueue_init(struct workqueue *wq, const char *name) {
    list_head_init(&wq->items);
    wq->lock = (spinlock_t)SPINLOCK_INIT;
    wq->name = name;
    wq->processed = 0;
}

/* Schedule work on a queue (safe from IRQ context) */
void workqueue_schedule(struct workqueue *wq, struct work *w);

/* Process all pending work (call from process context) */
void workqueue_process(struct workqueue *wq);

/* Check if queue has pending work */
static inline bool workqueue_pending(struct workqueue *wq) {
    return !list_empty(&wq->items);
}

/* Global system work queue (one worker handles all) */
extern struct workqueue system_wq;

/* Initialize the system work queue */
void workqueue_init_system(void);

/* Process system work queue (called from idle task) */
void workqueue_process_system(void);

#endif /* WORKQUEUE_H_DEFERRED */
