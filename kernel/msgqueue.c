/*
 * Anykernel OS — IPC Message Queue Implementation
 */

#include "msgqueue.h"
#include "task.h"
#include "sched.h"
#include "string.h"
#include "errno.h"

void mq_init(struct msg_queue *mq) {
    mq->head = 0;
    mq->tail = 0;
    mq->count = 0;
    mq->lock = (spinlock_t)SPINLOCK_INIT;
    wq_init(&mq->send_wq);
    wq_init(&mq->recv_wq);
}

/* ── Internal: enqueue/dequeue with lock held ────────────────── */

static void mq_enqueue(struct msg_queue *mq, const void *data,
                        uint32_t len, uint32_t sender) {
    struct mq_message *msg = &mq->buf[mq->tail];
    uint32_t copy_len = (len > MQ_MSG_SIZE) ? MQ_MSG_SIZE : len;
    memcpy(msg->data, data, copy_len);
    msg->len = copy_len;
    msg->sender_tid = sender;
    mq->tail = (mq->tail + 1) % MQ_CAPACITY;
    mq->count++;
}

static void mq_dequeue(struct msg_queue *mq, void *buf,
                        uint32_t *out_len, uint32_t *out_sender) {
    struct mq_message *msg = &mq->buf[mq->head];
    memcpy(buf, msg->data, msg->len);
    if (out_len) *out_len = msg->len;
    if (out_sender) *out_sender = msg->sender_tid;
    mq->head = (mq->head + 1) % MQ_CAPACITY;
    mq->count--;
}

/* ── Blocking send ───────────────────────────────────────────── */

int mq_send(struct msg_queue *mq, const void *data, uint32_t len) {
    if (!mq || !data || len > MQ_MSG_SIZE) return -EINVAL;

    while (1) {
        uint64_t irq_flags;
        spin_lock_irqsave(&mq->lock, &irq_flags);

        if (mq->count < MQ_CAPACITY) {
            mq_enqueue(mq, data, len, task_current()->tid);

            spin_unlock_irqrestore(&mq->lock, irq_flags);

            /* Wake a receiver */
            wq_wake_one(&mq->recv_wq);
            return 0;
        }

        /* Queue full — sleep */
        struct task *cur = task_current();
        cur->state = TASK_SLEEPING;
        list_push_back(&mq->send_wq.waiters, &cur->run_node);

        spin_unlock_irqrestore(&mq->lock, irq_flags);
        schedule();
    }
}

/* ── Blocking receive ────────────────────────────────────────── */

int mq_recv(struct msg_queue *mq, void *buf, uint32_t *out_len,
            uint32_t *out_sender) {
    if (!mq || !buf) return -EINVAL;
    while (1) {
        uint64_t irq_flags;
        spin_lock_irqsave(&mq->lock, &irq_flags);

        if (mq->count > 0) {
            mq_dequeue(mq, buf, out_len, out_sender);

            spin_unlock_irqrestore(&mq->lock, irq_flags);

            /* Wake a sender */
            wq_wake_one(&mq->send_wq);
            return 0;
        }

        /* Queue empty — sleep */
        struct task *cur = task_current();
        cur->state = TASK_SLEEPING;
        list_push_back(&mq->recv_wq.waiters, &cur->run_node);

        spin_unlock_irqrestore(&mq->lock, irq_flags);
        schedule();
    }
}

/* ── Non-blocking variants ───────────────────────────────────── */

int mq_trysend(struct msg_queue *mq, const void *data, uint32_t len) {
    if (!mq || !data || len > MQ_MSG_SIZE) return -EINVAL;

    uint64_t irq_flags;
    spin_lock_irqsave(&mq->lock, &irq_flags);

    if (mq->count >= MQ_CAPACITY) {
        spin_unlock_irqrestore(&mq->lock, irq_flags);
        return -EAGAIN;
    }

    mq_enqueue(mq, data, len, task_current()->tid);
    spin_unlock_irqrestore(&mq->lock, irq_flags);

    wq_wake_one(&mq->recv_wq);
    return 0;
}

int mq_tryrecv(struct msg_queue *mq, void *buf, uint32_t *out_len,
               uint32_t *out_sender) {
    if (!mq || !buf) return -EINVAL;
    uint64_t irq_flags;
    spin_lock_irqsave(&mq->lock, &irq_flags);

    if (mq->count == 0) {
        spin_unlock_irqrestore(&mq->lock, irq_flags);
        return -EAGAIN;
    }

    mq_dequeue(mq, buf, out_len, out_sender);
    spin_unlock_irqrestore(&mq->lock, irq_flags);

    wq_wake_one(&mq->send_wq);
    return 0;
}
