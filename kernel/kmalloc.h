/*
 * Anykernel OS v2.1 — Kernel Heap Allocator (kmalloc)
 *
 * Simple slab allocator built on top of the PMM.
 * Manages small objects (16B to 2048B) using per-size-class
 * free lists within PMM-provided pages.
 *
 * Size classes (power of 2):
 *   16, 32, 64, 128, 256, 512, 1024, 2048
 *
 * For allocations > 2048 bytes, falls back to full page allocation.
 *
 * Each slab page has a small header at the beginning, followed by
 * uniformly-sized slots. Free slots are linked via an intrusive
 * free list (the first 8 bytes of each free slot = next pointer).
 *
 * Thread-safe via spinlock.
 */

#ifndef KMALLOC_H
#define KMALLOC_H

#include <stddef.h>
#include <stdint.h>

/*
 * Allocate `size` bytes from the kernel heap.
 * Returns a virtual (HHDM-mapped) pointer, or NULL on failure.
 * Minimum allocation: 16 bytes. Actual allocation rounded up to
 * the next power-of-2 size class.
 */
void *kmalloc(uint64_t size);

/*
 * Free a previously allocated pointer.
 * The pointer MUST have been returned by kmalloc.
 * Passing NULL is a safe no-op.
 */
void kfree(void *ptr);

/*
 * Allocate `size` bytes, zeroed.
 * Equivalent to kmalloc + memset(0).
 */
void *kzmalloc(uint64_t size);

/*
 * Resize a previously allocated block.
 * - If ptr is NULL, equivalent to kmalloc(new_size).
 * - If new_size is 0, equivalent to kfree(ptr), returns NULL.
 * - Otherwise, allocates new block, copies min(old, new) bytes, frees old.
 */
void *krealloc(void *ptr, uint64_t new_size);

/*
 * Return the usable size of a kmalloc'd block.
 * This is the size class (e.g., 64 for a 50-byte request)
 * or the full page allocation size for large blocks.
 */
uint64_t kmalloc_usable_size(void *ptr);

/*
 * Diagnostic: print slab allocator statistics.
 */
void kmalloc_dump_stats(void);

#endif /* KMALLOC_H */
