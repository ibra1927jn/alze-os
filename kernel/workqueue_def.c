/*
 * Anykernel OS — Work Queue Implementation
 */

#include "workqueue_def.h"
#include "kprintf.h"
#include "log.h"
#include "compiler.h"

/* ── Global system work queue ────────────────────────────────── */

struct workqueue system_wq;

void workqueue_init_system(void) {
    workqueue_init(&system_wq, "system-wq");
    LOG_OK("System work queue initialized");
}

/* ── Schedule work ───────────────────────────────────────────── */

void workqueue_schedule(struct workqueue *wq, struct work *w) {
    uint64_t irq_flags;
    spin_lock_irqsave(&wq->lock, &irq_flags);

    if (!w->pending) {
        w->pending = true;
        list_push_back(&wq->items, &w->node);
    }

    spin_unlock_irqrestore(&wq->lock, irq_flags);
}

/* ── Process pending work ────────────────────────────────────── */

void workqueue_process(struct workqueue *wq) {
    while (1) {
        uint64_t irq_flags;
        spin_lock_irqsave(&wq->lock, &irq_flags);

        if (list_empty(&wq->items)) {
            spin_unlock_irqrestore(&wq->lock, irq_flags);
            return;
        }

        struct list_node *node = wq->items.sentinel.next;
        list_remove_node(node);
        struct work *w = container_of(node, struct work, node);
        w->pending = false;
        wq->processed++;

        spin_unlock_irqrestore(&wq->lock, irq_flags);

        /* Execute in process context (can sleep, allocate, etc.) */
        w->func(w->arg);
    }
}

void workqueue_process_system(void) {
    workqueue_process(&system_wq);
}
