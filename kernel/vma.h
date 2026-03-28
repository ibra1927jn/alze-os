/*
 * Anykernel OS — Virtual Memory Areas (VMA)
 *
 * Tracks structured memory regions per task:
 *   TEXT    — Code (read-only, executable)
 *   DATA    — Initialized data (read-write)
 *   HEAP    — Dynamic allocations (grows up)
 *   STACK   — Task stack (grows down)
 *   GUARD   — Unmapped guard page (catches overflows)
 *   MMIO    — Memory-mapped I/O (uncacheable)
 *
 * Each task has a list of VMAs describing its address space.
 * Guard pages between regions detect stack overflow and heap corruption.
 *
 * Inspired by: Linux vm_area_struct, macOS vm_map_entry.
 */

#ifndef VMA_H
#define VMA_H

#include <stdint.h>
#include <stdbool.h>
#include "list.h"

/* ── VMA types ───────────────────────────────────────────────── */

enum vma_type {
    VMA_NONE  = 0,
    VMA_TEXT,        /* Code: RX (read + execute) */
    VMA_DATA,        /* Initialized data: RW */
    VMA_BSS,         /* Zero-initialized data: RW */
    VMA_HEAP,        /* Dynamic allocs: RW (grows up) */
    VMA_STACK,       /* Task stack: RW (grows down) */
    VMA_GUARD,       /* Unmapped guard page */
    VMA_MMIO,        /* Memory-mapped I/O */
    VMA_SHARED,      /* Shared memory region */
};

/* ── VMA permissions (bitmask) ───────────────────────────────── */

#define VMA_PERM_READ    0x01
#define VMA_PERM_WRITE   0x02
#define VMA_PERM_EXEC    0x04
#define VMA_PERM_USER    0x08   /* Accessible from Ring 3 (future) */

/* ── VMA descriptor ──────────────────────────────────────────── */

#define VMA_NAME_MAX 16

struct vma {
    struct list_node  node;         /* Linkage in task's VMA list */
    uint64_t          start;        /* Start virtual address (page-aligned) */
    uint64_t          end;          /* End virtual address (exclusive) */
    enum vma_type     type;
    uint32_t          perms;        /* VMA_PERM_* bitmask */
    char              name[VMA_NAME_MAX]; /* Debug name */
};

/* ── Per-task address space ──────────────────────────────────── */

#define VMA_MAX_REGIONS 32

struct vm_space {
    struct list_head   regions;     /* List of VMAs */
    uint32_t           count;       /* Number of VMAs */
    uint64_t           heap_start;  /* Current heap start */
    uint64_t           heap_end;    /* Current heap end (brk) */
};

/* ── API ─────────────────────────────────────────────────────── */

/* Initialize a vm_space */
void vm_space_init(struct vm_space *vs);

/* Add a VMA region */
int vma_add(struct vm_space *vs, uint64_t start, uint64_t end,
            enum vma_type type, uint32_t perms, const char *name);

/* Remove a VMA by address */
int vma_remove(struct vm_space *vs, uint64_t start);

/* Find the VMA containing a given address */
struct vma *vma_find(struct vm_space *vs, uint64_t addr);

/* Check if an access (at addr with perms) is allowed */
bool vma_check_access(struct vm_space *vs, uint64_t addr, uint32_t perms);

/* Dump all VMAs for debugging */
void vma_dump(struct vm_space *vs);

/* ── Kernel address space ────────────────────────────────────── */

/* Global kernel vm_space (shared by all tasks in Ring 0) */
extern struct vm_space kernel_vm_space;

/* Initialize kernel VMA mappings from boot info */
void vma_init_kernel(void);

#endif /* VMA_H */
