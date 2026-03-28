/*
 * Anykernel OS — Lock-free Ring Buffer (Linux kfifo-inspired)
 *
 * Single-producer, single-consumer (SPSC) lock-free circular buffer.
 * Linux uses kfifo for high-performance kernel FIFOs.
 *
 * Power-of-2 sized for branchless modulo via bitmask.
 * No locks needed for SPSC — uses memory barriers only.
 *
 * Usage:
 *   RING_BUF_DEFINE(my_ring, 256);  // 256-byte ring
 *   ring_write(&my_ring, data, len);
 *   ring_read(&my_ring, buf, len);
 */

#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdint.h>
#include "compiler.h"

struct ring_buf {
    uint8_t    *data;
    uint32_t    size;     /* Must be power of 2 */
    uint32_t    mask;     /* size - 1, for branchless modulo */
    volatile uint32_t head;  /* Write position (producer) */
    volatile uint32_t tail;  /* Read position (consumer) */
};

/* Define a static ring buffer with backing storage */
#define RING_BUF_DEFINE(name, sz) \
    static uint8_t name##_storage[sz]; \
    static struct ring_buf name = { \
        .data = name##_storage, .size = (sz), \
        .mask = (sz) - 1, .head = 0, .tail = 0 \
    }

static inline void ring_init(struct ring_buf *r, uint8_t *buf, uint32_t size) {
    r->data = buf;
    r->size = size;
    r->mask = size - 1;
    r->head = 0;
    r->tail = 0;
}

static inline uint32_t ring_used(struct ring_buf *r) {
    return r->head - r->tail;
}

static inline uint32_t ring_free(struct ring_buf *r) {
    return r->size - ring_used(r);
}

static inline int ring_empty(struct ring_buf *r) {
    return r->head == r->tail;
}

static inline int ring_full(struct ring_buf *r) {
    return ring_used(r) == r->size;
}

/* Write up to count bytes. Returns bytes actually written. */
static inline uint32_t ring_write(struct ring_buf *r,
                                   const void *buf, uint32_t count) {
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t avail = ring_free(r);
    if (count > avail) count = avail;

    for (uint32_t i = 0; i < count; i++) {
        r->data[(r->head + i) & r->mask] = src[i];
    }
    __sync_synchronize();  /* Memory barrier: ensure data visible before head */
    r->head += count;
    return count;
}

/* Read up to count bytes. Returns bytes actually read. */
static inline uint32_t ring_read(struct ring_buf *r,
                                  void *buf, uint32_t count) {
    uint8_t *dst = (uint8_t *)buf;
    uint32_t avail = ring_used(r);
    if (count > avail) count = avail;

    for (uint32_t i = 0; i < count; i++) {
        dst[i] = r->data[(r->tail + i) & r->mask];
    }
    __sync_synchronize();  /* Memory barrier: ensure reads complete before tail */
    r->tail += count;
    return count;
}

/* Peek at the next byte without consuming it */
static inline int ring_peek(struct ring_buf *r) {
    if (ring_empty(r)) return -1;
    return r->data[r->tail & r->mask];
}

#endif /* RINGBUF_H */
