/*
 * Anykernel OS — Preemptive Round-Robin Scheduler
 *
 * Manages kernel threads via a circular ready queue.
 * PIT IRQ0 triggers schedule() every quantum (10ms).
 */

#ifndef SCHED_H
#define SCHED_H

#include "task.h"

/*
 * Initialize the scheduler.
 * Wraps the current execution context as task 0 ("init").
 * Creates the idle task.
 */
void sched_init(void);

/*
 * Pick the next READY task and switch to it.
 * Called from PIT handler (preemptive) or sched_yield (voluntary).
 *
 * IMPORTANT: Caller must have already sent EOI to PIC if called
 * from an IRQ handler.
 */
void schedule(void);

/*
 * Voluntary yield — gives up CPU to the next ready task.
 */
void sched_yield(void);

/*
 * Reap dead tasks — free their stacks and TCBs.
 * Called from idle task.
 */
void sched_reap_dead(void);

/*
 * Notify scheduler that a tick has occurred.
 * Sets need_resched flag (called from PIT handler).
 */
void sched_tick(void);

/*
 * Check if rescheduling is needed.
 */
int sched_need_resched(void);

/*
 * Add a task to the appropriate priority ready queue.
 * Used by wait queues and mutex to re-enqueue woken tasks.
 */
void sched_add_ready(struct task *t);

#endif /* SCHED_H */
