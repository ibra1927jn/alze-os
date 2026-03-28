/*
 * Anykernel OS — Reference Counting (Linux kref-inspired)
 *
 * Thread-safe reference counting for object lifecycle management.
 * When refcount drops to 0, a release callback is invoked.
 *
 * Usage:
 *   struct my_obj { struct kref ref; int data; };
 *   void my_release(struct kref *r) { kfree(container_of(r, struct my_obj, ref)); }
 *   kref_init(&obj->ref);            // refcount = 1
 *   kref_get(&obj->ref);             // refcount++
 *   kref_put(&obj->ref, my_release); // refcount--, calls release at 0
 */

#ifndef KREF_H
#define KREF_H

#include "atomics.h"

struct kref {
    atomic_t refcount;
};

static inline void kref_init(struct kref *r) {
    atomic_set(&r->refcount, 1);
}

static inline void kref_get(struct kref *r) {
    atomic_inc(&r->refcount);
}

/* Returns 1 if the object was released, 0 otherwise */
static inline int kref_put(struct kref *r, void (*release)(struct kref *)) {
    if (atomic_dec(&r->refcount) == 0) {
        release(r);
        return 1;
    }
    return 0;
}

static inline int kref_read(struct kref *r) {
    return atomic_read(&r->refcount);
}

#endif /* KREF_H */
