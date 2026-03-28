/*
 * Anykernel OS — Quality of Service Classes (macOS-inspired)
 *
 * macOS maps user-facing "QoS classes" to internal scheduling parameters.
 * Instead of forcing users to pick numerical priorities, they declare
 * the *nature* of their work and the kernel chooses the right priority.
 *
 * QoS classes (highest to lowest):
 *   USER_INTERACTIVE — UI thread, must respond within 16ms (60fps)
 *   USER_INITIATED   — User clicked something, expecting a result
 *   UTILITY          — Long-running task with progress bar
 *   BACKGROUND       — Invisible work (backups, indexing)
 *
 * Each maps to a scheduler priority and quantum multiplier.
 */

#ifndef QOS_H
#define QOS_H

#include "task.h"

/* ── QoS class definitions ───────────────────────────────────── */

enum qos_class {
    QOS_USER_INTERACTIVE = 0,  /* Highest: UI response */
    QOS_USER_INITIATED   = 1,  /* User-triggered work */
    QOS_DEFAULT          = 2,  /* Normal kernel threads */
    QOS_UTILITY          = 3,  /* Background with progress */
    QOS_BACKGROUND       = 4,  /* Invisible background work */
    QOS_COUNT            = 5
};

/* ── QoS-to-scheduler mapping ────────────────────────────────── */

/* Each QoS class maps to a priority range and quantum */
struct qos_params {
    enum task_priority priority;    /* Base scheduler priority */
    uint32_t           quantum;     /* Ticks before preemption */
    const char        *name;        /* Human-readable name */
};

static const struct qos_params qos_table[QOS_COUNT] = {
    [QOS_USER_INTERACTIVE] = { .priority = 4,  .quantum = 1,  .name = "interactive" },
    [QOS_USER_INITIATED]   = { .priority = 16, .quantum = 2,  .name = "user-init"   },
    [QOS_DEFAULT]          = { .priority = 32, .quantum = 2,  .name = "default"     },
    [QOS_UTILITY]          = { .priority = 48, .quantum = 4,  .name = "utility"     },
    [QOS_BACKGROUND]       = { .priority = 56, .quantum = 8,  .name = "background"  },
};

/* ── API ─────────────────────────────────────────────────────── */

/*
 * Apply a QoS class to a task. Sets both priority and quantum.
 * Can be called after task_create to change the task's class.
 */
static inline void task_set_qos(struct task *t, enum qos_class qos) {
    if (qos >= QOS_COUNT) qos = QOS_DEFAULT;
    t->priority = qos_table[qos].priority;
    t->qos = qos;
}

/*
 * Get the QoS-based quantum for a task.
 */
static inline uint32_t task_qos_quantum(struct task *t) {
    if (t->qos >= QOS_COUNT) return 2;
    return qos_table[t->qos].quantum;
}

/*
 * Get human-readable QoS name.
 */
static inline const char *qos_name(enum qos_class qos) {
    if (qos >= QOS_COUNT) return "unknown";
    return qos_table[qos].name;
}

#endif /* QOS_H */
