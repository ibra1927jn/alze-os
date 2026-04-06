/*
 * Anykernel OS — Virtual Memory Areas Implementation
 */

#include "vma.h"
#include "string.h"
#include "kprintf.h"
#include "log.h"
#include "kmalloc.h"
#include "errno.h"

/* ── Global kernel address space ─────────────────────────────── */

struct vm_space kernel_vm_space;

/* Static pool for VMAs (no kmalloc needed during early boot) */
static struct vma vma_pool[VMA_MAX_REGIONS * 4]; /* Space for kernel + tasks */
static uint32_t vma_pool_used = 0;

static struct vma *alloc_vma(void) {
    if (vma_pool_used < sizeof(vma_pool) / sizeof(vma_pool[0])) {
        struct vma *v = &vma_pool[vma_pool_used++];
        memset(v, 0, sizeof(*v));
        return v;
    }
    return 0;
}

/* ── Init ────────────────────────────────────────────────────── */

void vm_space_init(struct vm_space *vs) {
    list_head_init(&vs->regions);
    vs->count = 0;
    vs->heap_start = 0;
    vs->heap_end = 0;
}

/* ── Add VMA ─────────────────────────────────────────────────── */

int vma_add(struct vm_space *vs, uint64_t start, uint64_t end,
            enum vma_type type, uint32_t perms, const char *name) {
    struct vma *v = alloc_vma();
    if (!v) return -ENOMEM;

    v->start = start;
    v->end = end;
    v->type = type;
    v->perms = perms;
    strncpy(v->name, name, VMA_NAME_MAX - 1);
    v->name[VMA_NAME_MAX - 1] = '\0';

    /* Insert sorted by start address */
    struct list_node *pos;
    struct list_node *insert_after = &vs->regions.sentinel;

    list_for_each(pos, &vs->regions) {
        struct vma *existing = container_of(pos, struct vma, node);
        if (existing->start > start) break;
        insert_after = pos;
    }

    /* Insert after insert_after */
    v->node.next = insert_after->next;
    v->node.prev = insert_after;
    insert_after->next->prev = &v->node;
    insert_after->next = &v->node;

    vs->count++;
    return 0;
}

/* ── Remove VMA ──────────────────────────────────────────────── */

int vma_remove(struct vm_space *vs, uint64_t start) {
    struct list_node *pos;
    list_for_each(pos, &vs->regions) {
        struct vma *v = container_of(pos, struct vma, node);
        if (v->start == start) {
            list_remove_node(pos);
            vs->count--;
            return 0;
        }
    }
    return -ENOENT; /* Not found */
}

/* ── Find VMA containing address ─────────────────────────────── */

struct vma *vma_find(struct vm_space *vs, uint64_t addr) {
    struct list_node *pos;
    list_for_each(pos, &vs->regions) {
        struct vma *v = container_of(pos, struct vma, node);
        if (addr >= v->start && addr < v->end) return v;
        if (v->start > addr) break; /* Sorted: no point continuing */
    }
    return 0;
}

/* ── Check access permissions ────────────────────────────────── */

bool vma_check_access(struct vm_space *vs, uint64_t addr, uint32_t perms) {
    struct vma *v = vma_find(vs, addr);
    if (!v) return false;                    /* Unmapped = fault */
    if (v->type == VMA_GUARD) return false;  /* Guard page = fault */
    return (v->perms & perms) == perms;       /* Check all required perms */
}

/* ── Dump VMAs ───────────────────────────────────────────────── */

static const char *vma_type_str(enum vma_type t) {
    switch (t) {
        case VMA_TEXT:   return "TEXT";
        case VMA_DATA:   return "DATA";
        case VMA_BSS:    return "BSS";
        case VMA_HEAP:   return "HEAP";
        case VMA_STACK:  return "STACK";
        case VMA_GUARD:  return "GUARD";
        case VMA_MMIO:   return "MMIO";
        case VMA_SHARED: return "SHARED";
        default:         return "???";
    }
}

void vma_dump(struct vm_space *vs) {
    kprintf("  VMA regions (%u):\n", vs->count);
    struct list_node *pos;
    list_for_each(pos, &vs->regions) {
        struct vma *v = container_of(pos, struct vma, node);
        kprintf("    0x%016lx - 0x%016lx  [%c%c%c] %-6s %s\n",
                v->start, v->end,
                (v->perms & VMA_PERM_READ)  ? 'R' : '-',
                (v->perms & VMA_PERM_WRITE) ? 'W' : '-',
                (v->perms & VMA_PERM_EXEC)  ? 'X' : '-',
                vma_type_str(v->type),
                v->name);
    }
}

/* ── Initialize kernel VMA mappings ──────────────────────────── */

void vma_init_kernel(void) {
    vm_space_init(&kernel_vm_space);

    /* Kernel text/code — from linker symbols */
    extern char __kernel_start, __kernel_end;
    uint64_t k_start = (uint64_t)&__kernel_start;
    uint64_t k_end = (uint64_t)&__kernel_end;

    vma_add(&kernel_vm_space, k_start, k_end,
            VMA_TEXT, VMA_PERM_READ | VMA_PERM_EXEC, "kernel");

    LOG_OK("VMA: kernel address space initialized (%u regions)",
           kernel_vm_space.count);
    vma_dump(&kernel_vm_space);
}
