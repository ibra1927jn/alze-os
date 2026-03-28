/*
 * Anykernel OS — Ticket Spinlock
 *
 * Fair FIFO spinlock using tickets. Unlike test-and-set spinlocks,
 * ticket locks guarantee that waiters are served in arrival order.
 * This prevents starvation under high contention (critical for SMP).
 *
 * How it works:
 *   - Each locker takes a "ticket" (atomic increment of next_ticket)
 *   - Spins until serving_ticket matches their ticket
 *   - Unlock increments serving_ticket to serve next waiter
 *
 * Memory ordering:
 *   - Lock:   ACQUIRE (no loads/stores before this point)
 *   - Unlock: RELEASE (all stores visible before unlock)
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    volatile uint16_t next_ticket;
    volatile uint16_t serving_ticket;
} spinlock_t;

#define SPINLOCK_INIT { .next_ticket = 0, .serving_ticket = 0 }

/*
 * Acquire the lock — takes a ticket and waits for your turn.
 * FIFO fair: first to arrive = first to enter critical section.
 */
static inline void spin_lock(spinlock_t *lock) {
    uint16_t my_ticket = __atomic_fetch_add(&lock->next_ticket, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&lock->serving_ticket, __ATOMIC_ACQUIRE) != my_ticket) {
        asm volatile("pause");
    }
}

/*
 * Release the lock — serves the next waiter in line.
 */
static inline void spin_unlock(spinlock_t *lock) {
    __atomic_fetch_add(&lock->serving_ticket, 1, __ATOMIC_RELEASE);
}

/*
 * Try to acquire without spinning. Returns true if acquired.
 * Only succeeds if no one else is waiting.
 */
static inline bool spin_trylock(spinlock_t *lock) {
    uint16_t expected = __atomic_load_n(&lock->serving_ticket, __ATOMIC_RELAXED);
    return __atomic_compare_exchange_n(
        &lock->next_ticket, &expected, expected + 1,
        false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/*
 * Check if lock is held (for assertions only).
 */
static inline bool spin_is_locked(spinlock_t *lock) {
    return __atomic_load_n(&lock->next_ticket, __ATOMIC_RELAXED) !=
           __atomic_load_n(&lock->serving_ticket, __ATOMIC_RELAXED);
}

/*
 * IRQ-safe lock: save RFLAGS, disable interrupts, THEN acquire.
 *
 * WHY: If an IRQ fires while we hold a spinlock, and the IRQ handler
 * tries to acquire the SAME lock, we deadlock on a single core.
 * Solution: disable IRQs first, so no handler can preempt us.
 */
static inline void spin_lock_irqsave(spinlock_t *lock, uint64_t *flags) {
    uint64_t rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    asm volatile("cli");
    *flags = rflags;
    spin_lock(lock);
}

/*
 * IRQ-safe unlock: release lock, THEN restore RFLAGS.
 */
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    if (flags & (1UL << 9)) {   /* Bit 9 = IF (Interrupt Flag) */
        asm volatile("sti");
    }
}

#endif /* SPINLOCK_H */
