/*
 * Anykernel OS — O(1) Preemptive Priority Scheduler
 *
 * Features (v0.4.5):
 *   - 64-level priority queues with O(1) bitmap scheduling (BSF)
 *   - task_sleep(ms) with sleep queue and PIT-driven wakeup
 *   - task_join(tid) with sleeping wait
 *   - CPU tick accounting per task
 *   - Stack canary verification on every schedule()
 *   - Idle task with dead-task reaper
 *   - Wait queue integration via sched_add_ready()
 *
 * TRAP HANDLING:
 *   1. EOI before schedule (in idt.c)
 *   2. sched_lock released in trampoline
 *   3. sti in trampoline
 *   4. Deferred stack free via reaper
 */

#include "sched.h"
#include "task.h"
#include "list.h"
#include "spinlock.h"
#include "pmm.h"
#include "memory.h"
#include "string.h"
#include "kprintf.h"
#include "log.h"
#include "pic.h"
#include "waitqueue.h"
#include "compiler.h"
#include "qos.h"
#include "vmm.h"
#include "percpu.h"
#include "errno.h"
#include "panic.h"

#include <stdint.h>
#include <stdbool.h>

/* ── External: context_switch(old, new) in ASM ────────────────── */
extern void context_switch(struct task *old, struct task *new_task);

/* ── Constants ────────────────────────────────────────────────── */

#define TASK_STACK_PAGES  4
#define TASK_STACK_SIZE   (TASK_STACK_PAGES * PAGE_SIZE)
#define MAX_TASKS         64

/* ── Scheduler state ─────────────────────────────────────────── */

static spinlock_t sched_lock __cacheline_aligned = SPINLOCK_INIT;

/* O(1) bitmap scheduler: 64 priority queues + bitmap */
static struct list_head prio_queues[TASK_PRIO_LEVELS];
static volatile uint64_t prio_bitmap __cacheline_aligned = 0;
static struct list_head sleep_queue = LIST_HEAD_INIT(sleep_queue);
static struct list_head deadline_queue = LIST_HEAD_INIT(deadline_queue);  /* EDF: sorted by deadline, O(1) dequeue */

/* O(1) helpers: BSF finds lowest set bit = highest priority.
 * IMPORTANTE: bsfq tiene resultado indefinido cuando la entrada es 0.
 * Retorna -1 si no hay bits activos para evitar comportamiento indefinido. */
static inline int bitmap_find_first(uint64_t bm) {
    if (bm == 0) return -1;
    uint64_t result;
    asm volatile("bsfq %1, %0" : "=r"(result) : "r"(bm));
    return (int)result;
}

/* Forward decl for EDF deadline queue (defined below) */
static void deadline_enqueue(struct task *t);

static inline void bitmap_enqueue(struct task *t) {
    if (t->deadline > 0) {
        deadline_enqueue(t);  /* EDF: separate sorted queue */
    } else {
        list_push_back(&prio_queues[t->priority], &t->run_node);
        prio_bitmap |= (1ULL << t->priority);
    }
}

static inline struct task *bitmap_dequeue(void) {
    /* Proteccion contra bitmap vacio (bsfq UB si input == 0) */
    int p = bitmap_find_first(prio_bitmap);
    if (p < 0) return 0;

    struct list_node *node = prio_queues[p].sentinel.next;
    struct task *t = container_of(node, struct task, run_node);
    list_remove_node(node);
    if (list_empty(&prio_queues[p]))
        prio_bitmap &= ~(1ULL << p);
    return t;
}
static struct list_head dead_queue  = LIST_HEAD_INIT(dead_queue);

static struct task  task_pool[MAX_TASKS];
static uint64_t     tcb_bitmap = ~0ULL;  /* 1 = free, 0 = used (BSF alloc) */
static uint32_t     next_tid = 0;

/* Per-task quantum from QoS class (macOS-inspired) */
static inline uint32_t task_quantum(struct task *t) {
    return task_qos_quantum(t);
}

/* EDF O(1): deadline tasks are kept in a separate queue sorted by deadline.
 * Insertion is O(N) worst case but N = deadline tasks only (typically 0-3).
 * Dequeue is O(1) — just take the head. */
static void deadline_enqueue(struct task *t) {
    struct list_node *pos;
    list_for_each(pos, &deadline_queue) {
        struct task *q = container_of(pos, struct task, run_node);
        if (t->deadline < q->deadline) {
            /* Insert before q */
            t->run_node.next = pos;
            t->run_node.prev = pos->prev;
            pos->prev->next = &t->run_node;
            pos->prev = &t->run_node;
            return;
        }
    }
    list_push_back(&deadline_queue, &t->run_node);
}

static struct task *edf_pick(void) {
    if (list_empty(&deadline_queue)) return 0;
    struct task *t = container_of(deadline_queue.sentinel.next, struct task, run_node);
    list_remove_node(&t->run_node);
    return t;
}

/* ── Allocate a TCB — O(1) via BSF bitmap (macOS zone-style) ── */

static struct task *alloc_tcb(void) {
    if (tcb_bitmap == 0) return 0;  /* All 64 slots used */

    /* BSF: find first free bit in O(1) — like macOS zone allocator */
    int slot;
    asm volatile("bsfq %1, %q0" : "=r"(slot) : "rm"(tcb_bitmap));
    tcb_bitmap &= ~(1ULL << slot);  /* Mark used */

    struct task *t = &task_pool[slot];
    memset(t, 0, sizeof(*t));
    return t;
}

/* Free a TCB slot back to the bitmap */
static void free_tcb(struct task *t) {
    int slot = (int)(t - task_pool);
    if (slot >= 0 && slot < MAX_TASKS) {
        tcb_bitmap |= (1ULL << slot);  /* Mark free */
    }
}

/* ── Find task by TID ────────────────────────────────────────── */

static struct task *find_task(uint32_t tid) {
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        /* Only check allocated slots (bit clear in bitmap = in use) */
        if (!(tcb_bitmap & (1ULL << i)) && task_pool[i].tid == tid)
            return &task_pool[i];
    }
    return 0;
}

/* ── Stack canary ────────────────────────────────────────────── */

static void stack_canary_set(struct task *t) {
    if (t->stack_phys == 0) return;  /* boot stack */
    uint64_t *canary = (uint64_t *)PHYS2VIRT(t->stack_phys);
    *canary = TASK_STACK_CANARY;
}

static void stack_canary_check(struct task *t) {
    if (t->stack_phys == 0) return;  /* boot stack */
    uint64_t *canary = (uint64_t *)PHYS2VIRT(t->stack_phys);
    if (*canary != TASK_STACK_CANARY) {
        kprintf(ANSI_RED "\n!!! STACK OVERFLOW DETECTED !!!\n" ANSI_RESET);
        kprintf("  Task: '%s' (TID %u)\n", t->name, t->tid);
        kprintf("  Expected canary: 0x%016lx\n", (uint64_t)TASK_STACK_CANARY);
        kprintf("  Found:           0x%016lx\n", *canary);
        for (;;) asm volatile("cli; hlt");
    }
}

/* ── Trampoline ──────────────────────────────────────────────── */

static void task_entry_trampoline(void) {
    spin_unlock(&sched_lock);       /* Trap 2 */
    asm volatile("sti");            /* Trap 3 */

    task_entry_fn fn = get_current()->entry;
    fn();

    task_exit();
}

/* ── Idle task ───────────────────────────────────────────────── */

static void idle_fn(void) {
    for (;;) {
        sched_reap_dead();
        asm volatile("sti; hlt");
    }
}

/* ── Create a new task ───────────────────────────────────────── */

int task_create(const char *name, task_entry_fn entry) {
    uint64_t irq_flags;
    spin_lock_irqsave(&sched_lock, &irq_flags);

    struct task *t = alloc_tcb();
    if (!t) {
        spin_unlock_irqrestore(&sched_lock, irq_flags);
        return -ENOMEM;
    }

    uint64_t stack_phys = pmm_alloc_pages_zero(2);
    if (stack_phys == 0) {
        /* Devolver el TCB al bitmap — evitar leak de slot */
        free_tcb(t);
        spin_unlock_irqrestore(&sched_lock, irq_flags);
        return -ENOMEM;
    }

    t->stack_phys = stack_phys;
    t->tid = next_tid++;
    t->state = TASK_READY;
    t->priority = TASK_PRIO_NORMAL;
    t->qos = QOS_DEFAULT;
    t->deadline = 0;
    t->watchdog_ticks = 0;
    t->fd_table = 0;  /* Tasks inherit no fds by default */
    t->entry = entry;
    t->ticks_used = 0;
    t->sleep_until = 0;
    t->finished = false;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';

    /* Set stack canary at bottom of stack */
    stack_canary_set(t);

    /* Guard page: unmap bottom page of stack to catch overflow via #PF.
     * Linux, macOS, and Windows all use this technique. */
    uint64_t stack_virt = (uint64_t)PHYS2VIRT(stack_phys);
    vmm_unmap_page(stack_virt);  /* Unmap page 0 of 4 (the bottom) */

    /* Build initial context_switch frame */
    uint64_t stack_top = stack_virt + TASK_STACK_SIZE;

    uint64_t *sp = (uint64_t *)stack_top;
    *(--sp) = (uint64_t)task_entry_trampoline;  /* ret address */
    *(--sp) = 0;   /* rbp */
    *(--sp) = 0;   /* rbx */
    *(--sp) = 0;   /* r12 */
    *(--sp) = 0;   /* r13 */
    *(--sp) = 0;   /* r14 */
    *(--sp) = 0;   /* r15 */

    t->rsp = (uint64_t)sp;

    bitmap_enqueue(t);

    spin_unlock_irqrestore(&sched_lock, irq_flags);

    LOG_OK("Task '%s' created (TID %u, stack 0x%lx)", t->name, t->tid, stack_phys);
    return (int)t->tid;
}

/* ── task_exit ───────────────────────────────────────────────── */

__attribute__((noreturn))
void task_exit(void) {
    uint64_t exit_flags;
    spin_lock_irqsave(&sched_lock, &exit_flags);

    struct task *self = get_current();
    self->state = TASK_DEAD;
    self->finished = true;

    /* Wake ALL threads waiting to join us — instant, no polling */
    if (self->join_wq) {
        wq_wake_all(self->join_wq);
    }

    list_push_back(&dead_queue, &self->run_node);

    LOG_INFO("Task '%s' (TID %u) exiting, used %lu ticks",
             self->name, self->tid, self->ticks_used);

    spin_unlock_irqrestore(&sched_lock, exit_flags);
    schedule();

    for (;;) asm volatile("hlt");
}

/* ── task_current ────────────────────────────────────────────── */

struct task *task_current(void) {
    return get_current();
}

/* ── task_sleep ──────────────────────────────────────────────── */

void task_sleep(uint64_t ms) {
    uint64_t irq_flags;
    spin_lock_irqsave(&sched_lock, &irq_flags);

    uint64_t ticks = ms / 10;  /* 100 Hz PIT = 10ms/tick */
    if (ticks == 0) ticks = 1;

    struct task *self = get_current();
    self->sleep_until = pit_get_ticks() + ticks;
    self->state = TASK_SLEEPING;
    list_push_back(&sleep_queue, &self->run_node);

    spin_unlock_irqrestore(&sched_lock, irq_flags);
    schedule();
}

/* ── task_join ───────────────────────────────────────────────── */

int task_join(uint32_t tid) {
    uint64_t irq_flags;
    spin_lock_irqsave(&sched_lock, &irq_flags);

    struct task *target = find_task(tid);
    if (!target) {
        spin_unlock_irqrestore(&sched_lock, irq_flags);
        return -ESRCH;
    }

    /* Ya termino? Retornar inmediatamente */
    if (target->finished) {
        spin_unlock_irqrestore(&sched_lock, irq_flags);
        return 0;
    }

    /* Asignar wait queue on-demand para joiners.
     * La WQ vive en nuestro stack — seguro porque este frame
     * persiste mientras estemos bloqueados en wq_wait. */
    struct wait_queue join_wq;
    wq_init(&join_wq);

    if (!target->join_wq) {
        target->join_wq = &join_wq;
    }

    spin_unlock_irqrestore(&sched_lock, irq_flags);

    /* Dormir hasta que target llame task_exit -> wq_wake_all */
    while (!target->finished) {
        wq_wait(target->join_wq);
    }
    return 0;
}

/* ── Wake sleeping tasks ─────────────────────────────────────── */

static void wake_sleepers(void) {
    /* Called with sched_lock held */
    uint64_t now = pit_get_ticks();
    struct list_node *pos, *tmp;

    list_for_each_safe(pos, tmp, &sleep_queue) {
        struct task *t = container_of(pos, struct task, run_node);
        if (now >= t->sleep_until) {
            list_remove_node(pos);
            t->state = TASK_READY;
            bitmap_enqueue(t);
        }
    }
}

/* ── Schedule ────────────────────────────────────────────────── */

__hot void schedule(void) {
    uint64_t irq_flags;
    spin_lock_irqsave(&sched_lock, &irq_flags);

    set_need_resched(0);

    /* Wake any sleeping tasks whose time has come */
    wake_sleepers();

    /* Check stack canary of current task */
    stack_canary_check(get_current());

    /* O(1) pick: BSF on bitmap finds highest priority in 1 cycle */
    /* But EDF tasks with deadlines get absolute priority */
    struct task *next;
    struct task *edf = edf_pick();
    if (edf) {
        next = edf;  /* Already removed from deadline_queue by edf_pick */
    } else if (likely(prio_bitmap != 0)) {
        next = bitmap_dequeue();
        /* Fallback si bitmap_dequeue fallo (no deberia pasar) */
        if (!next) next = get_idle();
    } else {
        next = get_idle();
    }

    if (likely(next == get_current())) {
        spin_unlock_irqrestore(&sched_lock, irq_flags);
        return;
    }

    struct task *old = get_current();
    if (old->state == TASK_RUNNING) {
        old->state = TASK_READY;
        bitmap_enqueue(old);
    }

    next->state = TASK_RUNNING;
    set_current(next);
    next->watchdog_ticks = 0;  /* Reset watchdog on schedule */

    /* Prefetch new task's stack into L1 cache before switching */
    if (next->stack_phys) {
        prefetch((void *)PHYS2VIRT(next->stack_phys + TASK_STACK_SIZE - 64));
    }

    context_switch(old, next);

    /* We resume here when switched back to this thread */
    spin_unlock_irqrestore(&sched_lock, irq_flags);
}

/* ── sched_yield ─────────────────────────────────────────────── */

void sched_yield(void) {
    schedule();
}

/* ── sched_tick — called from PIT handler ────────────────────── */

__hot void sched_tick(void) {
    /* Guard: PIT fires before sched_init sets current via GS */
    struct task *cur = get_current();
    if (unlikely(!cur)) return;

    /* CPU accounting */
    cur->ticks_used++;
    cur->watchdog_ticks++;  /* Watchdog: cuenta ticks sin ceder CPU */

    /* Watchdog enforcement: matar tareas colgadas (~100s a 100Hz).
     * No matar idle ni init (TID 0/1) — solo tareas de usuario. */
    #define WATCHDOG_LIMIT 10000
    if (cur->watchdog_ticks >= WATCHDOG_LIMIT && cur->tid > 1) {
        kprintf(ANSI_RED "[WATCHDOG] Task '%s' (TID %u) exceeded %u ticks without yielding — killing\n" ANSI_RESET,
                cur->name, cur->tid, WATCHDOG_LIMIT);
        cur->state = TASK_DEAD;
        cur->finished = true;
        if (cur->join_wq) {
            wq_wake_all(cur->join_wq);
        }
        list_push_back(&dead_queue, &cur->run_node);
        set_need_resched(1);
        return;
    }

    /* Per-QoS quantum: only reschedule when quantum expires */
    if (cur->ticks_used % task_quantum(cur) == 0)
        set_need_resched(1);
}

int sched_need_resched(void) {
    if (!get_current()) return 0;
    return get_need_resched();
}

/* ── sched_add_ready — used by wait queues to re-enqueue ─────── */

void sched_add_ready(struct task *t) {
    uint64_t irq_flags;
    spin_lock_irqsave(&sched_lock, &irq_flags);
    bitmap_enqueue(t);
    spin_unlock_irqrestore(&sched_lock, irq_flags);
}

/* ── task_set_deadline — EDF API ─────────────────────────────── */

void task_set_deadline(uint64_t deadline_tick) {
    uint64_t irq_flags;
    spin_lock_irqsave(&sched_lock, &irq_flags);
    struct task *cur = get_current();
    if (cur)
        cur->deadline = deadline_tick;
    spin_unlock_irqrestore(&sched_lock, irq_flags);
}

/* ── Reap dead tasks ─────────────────────────────────────────── */

void sched_reap_dead(void) {
    while (!list_empty(&dead_queue)) {
        uint64_t irq_flags;
        spin_lock_irqsave(&sched_lock, &irq_flags);

        if (list_empty(&dead_queue)) {
            spin_unlock_irqrestore(&sched_lock, irq_flags);
            break;
        }

        struct list_node *node = dead_queue.sentinel.next;
        list_remove_node(node);

        struct task *dead = container_of(node, struct task, run_node);
        uint64_t stack_phys = dead->stack_phys;
        char dead_name[16];
        strncpy(dead_name, dead->name, sizeof(dead_name));
        uint32_t dead_tid = dead->tid;

        /* Free TCB bitmap slot — O(1) reclaim (macOS zone-style) */
        dead->stack_phys = 0;
        free_tcb(dead);

        spin_unlock_irqrestore(&sched_lock, irq_flags);

        if (stack_phys) {
            pmm_free_pages(stack_phys, 2);
        }
        LOG_INFO("Reaped task '%s' (TID %u)", dead_name, dead_tid);
    }
}

/* ── sched_init ──────────────────────────────────────────────── */

void sched_init(void) {
    /* Initialize all 64 priority queues */
    for (int i = 0; i < TASK_PRIO_LEVELS; i++)
        list_head_init(&prio_queues[i]);
    prio_bitmap = 0;

    struct task *init_task = alloc_tcb();
    KASSERT(init_task != 0);  /* Sin TCB para init = sistema inutilizable */
    init_task->tid = next_tid++;
    init_task->state = TASK_RUNNING;
    init_task->stack_phys = 0;
    init_task->priority = TASK_PRIO_HIGH;
    init_task->entry = 0;
    init_task->ticks_used = 0;
    init_task->finished = false;
    init_task->deadline = 0;
    init_task->watchdog_ticks = 0;
    init_task->fd_table = 0;
    init_task->qos = QOS_DEFAULT;
    strncpy(init_task->name, "init", sizeof(init_task->name));
    set_current(init_task);

    int idle_tid = task_create("idle", idle_fn);
    KASSERT(idle_tid >= 0);

    uint64_t irq_flags;
    spin_lock_irqsave(&sched_lock, &irq_flags);

    /* Buscar idle task por TID, no por slot fijo en task_pool.
     * El slot puede variar si alloc_tcb cambia de orden. */
    struct list_node *pos;
    int idle_found = 0;
    list_for_each(pos, &prio_queues[TASK_PRIO_NORMAL]) {
        struct task *t = container_of(pos, struct task, run_node);
        if (t->tid == (uint32_t)idle_tid) {
            set_idle(t);
            list_remove_node(pos);
            if (list_empty(&prio_queues[TASK_PRIO_NORMAL]))
                prio_bitmap &= ~(1ULL << TASK_PRIO_NORMAL);
            t->state = TASK_READY;
            idle_found = 1;
            break;
        }
    }
    KASSERT(idle_found);

    spin_unlock_irqrestore(&sched_lock, irq_flags);

    LOG_OK("Scheduler initialized (init TID=%u, idle TID=%u)",
           init_task->tid, get_idle()->tid);
}
