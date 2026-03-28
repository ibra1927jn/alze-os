/*
 * Anykernel OS — IPC Message Queue
 *
 * Fixed-size message passing between kernel threads.
 * Producer-consumer pattern with sleeping wait:
 *   - mq_send() blocks if queue is full
 *   - mq_recv() blocks if queue is empty
 *
 * Messages are small fixed-size buffers. For large data,
 * send a pointer in the message.
 */

#ifndef MSGQUEUE_H
#define MSGQUEUE_H

#include "waitqueue.h"
#include <stdint.h>
#include <stdbool.h>

#define MQ_MSG_SIZE     64      /* Max bytes per message */
#define MQ_CAPACITY     16      /* Max messages in queue */

struct mq_message {
    uint8_t  data[MQ_MSG_SIZE];
    uint32_t len;               /* Actual message length */
    uint32_t sender_tid;        /* Who sent it */
};

struct msg_queue {
    struct mq_message  buf[MQ_CAPACITY];
    uint32_t           head;        /* Read index */
    uint32_t           tail;        /* Write index */
    uint32_t           count;       /* Current messages */
    spinlock_t         lock;
    struct wait_queue  send_wq;     /* Waiters for space */
    struct wait_queue  recv_wq;     /* Waiters for data */
};

/* Initialize a message queue */
void mq_init(struct msg_queue *mq);

/*
 * Send a message. Blocks if queue is full.
 * data: pointer to message data
 * len: message length (max MQ_MSG_SIZE)
 * Returns 0 on success, -1 on error.
 */
int mq_send(struct msg_queue *mq, const void *data, uint32_t len);

/*
 * Receive a message. Blocks if queue is empty.
 * buf: buffer to receive into (must be >= MQ_MSG_SIZE)
 * out_len: receives actual length
 * out_sender: receives sender TID (optional, can be NULL)
 * Returns 0 on success.
 */
int mq_recv(struct msg_queue *mq, void *buf, uint32_t *out_len,
            uint32_t *out_sender);

/*
 * Try to send without blocking. Returns -1 if full.
 */
int mq_trysend(struct msg_queue *mq, const void *data, uint32_t len);

/*
 * Try to receive without blocking. Returns -1 if empty.
 */
int mq_tryrecv(struct msg_queue *mq, void *buf, uint32_t *out_len,
               uint32_t *out_sender);

#endif /* MSGQUEUE_H */
