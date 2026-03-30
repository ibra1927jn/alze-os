/*
 * Anykernel OS — Virtual Memory Manager Implementation
 *
 * x86_64 4-level paging: PML4 → PDPT → PD → PT
 *
 * Bootstrap sequence in vmm_init():
 *   1. Allocate a zeroed PML4
 *   2. Map kernel sections with per-section permissions
 *   3. Map HHDM with 2MB huge pages
 *   4. Switch CR3 → kernel runs on OUR tables
 *
 * DANGER: If any mapping is wrong when CR3 is written,
 *         the CPU triple-faults and QEMU reboots instantly.
 */

#include "vmm.h"
#include "pmm.h"
#include "memory.h"
#include "kprintf.h"
#include "log.h"
#include "panic.h"
#include "spinlock.h"
#include "gdt.h"
#include "limine.h"
#include "tlb_shootdown.h"

/* ── Linker-provided section boundaries ──────────────────────── */

extern char __kernel_start, __kernel_end;
extern char __text_start, __text_end;
extern char __rodata_start, __rodata_end;
extern char __data_start, __data_end;
extern char __bss_start, __bss_end;

/* ── Kernel virtual base (from linker script) ─────────────────── */

#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000UL

/* ── Global PML4 (physical address) ──────────────────────────── */

static uint64_t pml4_phys = 0;
static spinlock_t vmm_lock = SPINLOCK_INIT;
static uint64_t vmm_tables_allocated = 0;  /* Count of page tables we've allocated */
static int vmm_wx_enforced = 0;  /* W^X enforcement: disabled during vmm_init boot-up */

/* ── Helpers ──────────────────────────────────────────────────── */

/* Read current CR3 */
static inline uint64_t read_cr3(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* Write CR3 (switch page tables — DANGEROUS) */
static inline void write_cr3(uint64_t cr3) {
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

/* Flush TLB for a single virtual address on ALL CPUs.
 * First invalidate the local entry, then send IPI to other CPUs.
 * On single-core (no LAPIC), the broadcast is a safe no-op. */
static inline void vmm_flush_tlb(uint64_t virt) {
    /* Flush local first — this CPU must not use the stale entry */
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
    /* Broadcast to all other CPUs via IPI (no-op if single-core) */
    tlb_shootdown_broadcast(virt);
}

/* Enable NX bit support via EFER MSR */
#define MSR_EFER      0xC0000080   /* Extended Feature Enable Register */
#define EFER_NXE_BIT  (1 << 11)    /* No-Execute Enable               */

static inline void enable_nx(void) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_EFER));
    lo |= EFER_NXE_BIT;
    asm volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(MSR_EFER));
}

/* ── Page Table Entry manipulation ────────────────────────────── */

/*
 * Get or create a page table entry at a given level.
 * table_phys: physical address of the current table
 * index: 0-511 entry index
 * allocate: if true, allocate a new table if entry is not present
 *
 * Returns pointer to the entry (via HHDM), or NULL if not present
 * and allocate=false.
 */
static uint64_t *get_or_create_entry(uint64_t table_phys, uint32_t index, bool allocate) {
    uint64_t *table = (uint64_t *)PHYS2VIRT(table_phys);
    uint64_t entry = table[index];

    if (entry & PTE_PRESENT) {
        return &table[index];
    }

    if (!allocate) return NULL;

    /* Allocate a new zeroed table */
    uint64_t new_table = pmm_alloc_pages_zero(0);
    KASSERT(new_table != 0);
    vmm_tables_allocated++;

    /* Point entry to new table with full permissions
     * (actual restrictions are at the lowest level) */
    table[index] = new_table | PTE_PRESENT | PTE_WRITE;
    return &table[index];
}

/*
 * Get the next-level table physical address from a PTE.
 */
static inline uint64_t pte_to_phys(uint64_t pte) {
    return pte & PTE_ADDR_MASK;
}

/* ── Core: Map a 4KB page ─────────────────────────────────────── */

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    KASSERT(IS_PAGE_ALIGNED(virt));
    KASSERT(IS_PAGE_ALIGNED(phys));
    KASSERT(pml4_phys != 0);

    uint64_t irq_flags;
    spin_lock_irqsave(&vmm_lock, &irq_flags);

    /* Walk PML4 → PDPT → PD → PT, creating tables as needed */
    uint64_t *pml4e = get_or_create_entry(pml4_phys, PML4_INDEX(virt), true);
    KASSERT(pml4e != NULL);

    uint64_t pdpt_phys = pte_to_phys(*pml4e);
    uint64_t *pdpte = get_or_create_entry(pdpt_phys, PDPT_INDEX(virt), true);
    KASSERT(pdpte != NULL);

    uint64_t pd_phys = pte_to_phys(*pdpte);
    uint64_t *pde = get_or_create_entry(pd_phys, PD_INDEX(virt), true);
    KASSERT(pde != NULL);

    KASSERT(!(*pde & PTE_HUGE));

    uint64_t pt_phys = pte_to_phys(*pde);
    uint64_t *pt = (uint64_t *)PHYS2VIRT(pt_phys);

    /* Collision detection: warn if remapping to a DIFFERENT phys address.
     * Re-mapping the same phys with different flags is normal
     * (e.g. per-section permission changes). */
    uint64_t old = pt[PT_INDEX(virt)];
    if ((old & PTE_PRESENT) && (old & PTE_ADDR_MASK) != phys) {
        LOG_WARN("VMM: remap 0x%lx: old phys 0x%lx → new phys 0x%lx",
                 virt, old & PTE_ADDR_MASK, phys);
    }

    /*
     * W^X Enforcement (OpenBSD + macOS Hardened Runtime):
     * A page must NEVER be both Writable AND Executable.
     * Writable = PTE_WRITE set, Executable = PTE_NX NOT set.
     * Disabled during vmm_init boot sequence (initial broad mapping
     * needs W+X temporarily before per-section permissions refine it).
     */
    if (vmm_wx_enforced && (flags & PTE_WRITE) && !(flags & PTE_NX)) {
        LOG_WARN("VMM: W^X VIOLATION at 0x%lx (flags=0x%lx) — page is W+X!", virt, flags);
        /* Force NX bit on writable pages */
        flags |= PTE_NX;
    }

    pt[PT_INDEX(virt)] = phys | flags;

    spin_unlock_irqrestore(&vmm_lock, irq_flags);

    vmm_flush_tlb(virt);
}

/* ── Core: Unmap a 4KB page ───────────────────────────────────── */

void vmm_unmap_page(uint64_t virt) {
    KASSERT(IS_PAGE_ALIGNED(virt));
    KASSERT(pml4_phys != 0);

    uint64_t irq_flags;
    spin_lock_irqsave(&vmm_lock, &irq_flags);

    uint64_t *pml4 = (uint64_t *)PHYS2VIRT(pml4_phys);
    if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT)) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return;
    }

    uint64_t *pdpt = (uint64_t *)PHYS2VIRT(pte_to_phys(pml4[PML4_INDEX(virt)]));
    if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT)) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return;
    }

    uint64_t *pd = (uint64_t *)PHYS2VIRT(pte_to_phys(pdpt[PDPT_INDEX(virt)]));
    if (!(pd[PD_INDEX(virt)] & PTE_PRESENT)) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return;
    }
    if (pd[PD_INDEX(virt)] & PTE_HUGE) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return;  /* Cannot unmap part of a 2MB huge page */
    }

    uint64_t *pt = (uint64_t *)PHYS2VIRT(pte_to_phys(pd[PD_INDEX(virt)]));

    pt[PT_INDEX(virt)] = 0;

    spin_unlock_irqrestore(&vmm_lock, irq_flags);

    vmm_flush_tlb(virt);
}

/* ── Walk: Virtual → Physical ─────────────────────────────────── */

uint64_t vmm_virt_to_phys(uint64_t virt) {
    if (pml4_phys == 0) return 0;

    uint64_t irq_flags;
    spin_lock_irqsave(&vmm_lock, &irq_flags);

    uint64_t result = 0;

    uint64_t *pml4 = (uint64_t *)PHYS2VIRT(pml4_phys);
    if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT)) goto out;

    uint64_t *pdpt = (uint64_t *)PHYS2VIRT(pte_to_phys(pml4[PML4_INDEX(virt)]));
    if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT)) goto out;

    /* 1GB huge page (unlikely but possible) */
    if (pdpt[PDPT_INDEX(virt)] & PTE_HUGE) {
        result = (pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK) | (virt & PAGE_OFFSET_1GB);
        goto out;
    }

    uint64_t *pd = (uint64_t *)PHYS2VIRT(pte_to_phys(pdpt[PDPT_INDEX(virt)]));
    if (!(pd[PD_INDEX(virt)] & PTE_PRESENT)) goto out;

    /* 2MB huge page */
    if (pd[PD_INDEX(virt)] & PTE_HUGE) {
        result = (pd[PD_INDEX(virt)] & PTE_ADDR_MASK) | (virt & PAGE_OFFSET_2MB);
        goto out;
    }

    uint64_t *pt = (uint64_t *)PHYS2VIRT(pte_to_phys(pd[PD_INDEX(virt)]));
    if (!(pt[PT_INDEX(virt)] & PTE_PRESENT)) goto out;

    result = (pt[PT_INDEX(virt)] & PTE_ADDR_MASK) | (virt & PAGE_OFFSET_4KB);

out:
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return result;
}

/* ── Bulk: Map a range of 4KB pages ───────────────────────────── */

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        vmm_map_page(virt + off, phys + off, flags);
    }
}

/* ── Bulk: Map a range using 2MB huge pages ───────────────────── */

void vmm_map_range_huge(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    KASSERT((virt & PAGE_OFFSET_2MB) == 0);  /* Must be 2MB-aligned */
    KASSERT((phys & PAGE_OFFSET_2MB) == 0);
    KASSERT((size & PAGE_OFFSET_2MB) == 0);

    uint64_t irq_flags;
    spin_lock_irqsave(&vmm_lock, &irq_flags);

    for (uint64_t off = 0; off < size; off += MB(2)) {
        uint64_t v = virt + off;
        uint64_t p = phys + off;

        /* Walk PML4 -> PDPT -> PD, creating tables as needed */
        uint64_t *pml4e = get_or_create_entry(pml4_phys, PML4_INDEX(v), true);
        KASSERT(pml4e != NULL);

        uint64_t pdpt_phys = pte_to_phys(*pml4e);
        uint64_t *pdpte = get_or_create_entry(pdpt_phys, PDPT_INDEX(v), true);
        KASSERT(pdpte != NULL);

        uint64_t pd_phys = pte_to_phys(*pdpte);
        uint64_t *pd = (uint64_t *)PHYS2VIRT(pd_phys);

        /* Write 2MB huge page entry directly into PD */
        pd[PD_INDEX(v)] = p | flags | PTE_HUGE;
    }

    spin_unlock_irqrestore(&vmm_lock, irq_flags);
}

/* ── vmm_init: Build our tables and switch CR3 ────────────────── */

void vmm_init(void) {
    LOG_INFO("VMM: Building kernel page tables...");

    /* Enable NX bit */
    enable_nx();

    /* 1. Allocate PML4 */
    pml4_phys = pmm_alloc_pages_zero(0);
    KASSERT(pml4_phys != 0);

    /* 2. Determine kernel physical base.
     *    Limine maps kernel at KERNEL_VIRT_BASE.
     *    We can find the physical base by reading Limine's current CR3
     *    and walking to our _start symbol, or by using the HHDM:
     *    kernel_phys = VIRT2PHYS(kernel_virt) only works if HHDM covers it.
     *
     *    Actually, the kernel is in the higher half (0xFFFFFFFF80000000),
     *    NOT in the HHDM (0xFFFF800000000000). So VIRT2PHYS won't work.
     *    We need to walk Limine's page tables to find the physical base. */
    uint64_t limine_cr3 = read_cr3();
    uint64_t kernel_virt = (uint64_t)&__kernel_start;

    /* Walk Limine's tables to find kernel physical base */
    uint64_t *l_pml4 = (uint64_t *)PHYS2VIRT(limine_cr3 & PTE_ADDR_MASK);
    uint64_t l_pml4e = l_pml4[PML4_INDEX(kernel_virt)];
    KASSERT(l_pml4e & PTE_PRESENT);

    uint64_t *l_pdpt = (uint64_t *)PHYS2VIRT(pte_to_phys(l_pml4e));
    uint64_t l_pdpte = l_pdpt[PDPT_INDEX(kernel_virt)];
    KASSERT(l_pdpte & PTE_PRESENT);

    uint64_t kernel_phys_base;

    if (l_pdpte & PTE_HUGE) {
        /* 1GB mapping — extract base */
        kernel_phys_base = pte_to_phys(l_pdpte);
    } else {
        uint64_t *l_pd = (uint64_t *)PHYS2VIRT(pte_to_phys(l_pdpte));
        uint64_t l_pde = l_pd[PD_INDEX(kernel_virt)];
        KASSERT(l_pde & PTE_PRESENT);

        if (l_pde & PTE_HUGE) {
            /* 2MB mapping */
            kernel_phys_base = pte_to_phys(l_pde);
        } else {
            /* 4KB mapping — walk to PT */
            uint64_t *l_pt = (uint64_t *)PHYS2VIRT(pte_to_phys(l_pde));
            uint64_t l_pte = l_pt[PT_INDEX(kernel_virt)];
            KASSERT(l_pte & PTE_PRESENT);
            kernel_phys_base = pte_to_phys(l_pte);
        }
    }

    LOG_INFO("VMM: Kernel phys base: 0x%lx", kernel_phys_base);

    uint64_t kernel_size = (uint64_t)&__kernel_end - (uint64_t)&__kernel_start;
    LOG_INFO("VMM: Kernel size: %lu KB (%lu pages)", kernel_size / 1024, kernel_size / PAGE_SIZE);

    /* 3. Map kernel: entire range with RW+X initially.
     *    The CPU must execute code from .text immediately after CR3 switch,
     *    so we need X permission. Per-section permissions refine after:
     *    .text → RX, .rodata → RO+NX, .data/.bss → RW+NX.
     *    W^X enforcement is deferred until per-section is complete. */
    vmm_map_range(
        (uint64_t)&__kernel_start,
        kernel_phys_base,
        kernel_size,
        PTE_PRESENT | PTE_WRITE | PTE_GLOBAL
    );

    LOG_OK("VMM: Kernel mapped (%lu pages)", kernel_size / PAGE_SIZE);

    /* 4. Map HHDM: all physical memory using 2MB huge pages.
     *    We use the memory map to find the actual highest physical address
     *    instead of guessing from PMM page counts. */
    extern uint64_t hhdm_offset;

    /* Find the highest physical address from usable/kernel/FB regions only.
     * Skip reserved MMIO ranges (some go to 1TB!) to save page tables. */
    extern struct limine_memmap_response *memmap_resp_saved;
    uint64_t max_phys = 0;
    if (memmap_resp_saved) {
        for (uint64_t i = 0; i < memmap_resp_saved->entry_count; i++) {
            uint64_t type = memmap_resp_saved->entries[i]->type;
            /* Only count regions that contain actual data we need to access */
            if (type == LIMINE_MEMMAP_USABLE ||
                type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
                type == LIMINE_MEMMAP_KERNEL_AND_MODULES ||
                type == LIMINE_MEMMAP_FRAMEBUFFER) {
                uint64_t top = memmap_resp_saved->entries[i]->base
                             + memmap_resp_saved->entries[i]->length;
                if (top > max_phys) max_phys = top;
            }
        }
    }
    /* Fallback */
    if (max_phys == 0) {
        max_phys = (pmm_free_count() + pmm_used_count() + 256) * PAGE_SIZE;
    }
    /* Round up to 2MB and add some headroom */
    max_phys = ALIGN_UP(max_phys, MB(2));

    vmm_map_range_huge(
        hhdm_offset,     /* Virtual base of HHDM */
        0,               /* Physical base: 0 */
        max_phys,        /* Size to map */
        VMM_FLAGS_HHDM
    );

    LOG_OK("VMM: HHDM mapped (%lu MB via 2MB huge pages)", max_phys / MB(1));

    /* 5. Switch CR3 — THE MOMENT OF TRUTH */
    LOG_INFO("VMM: Switching CR3 to 0x%lx ...", pml4_phys);
    write_cr3(pml4_phys);
    LOG_OK("VMM: CR3 switched! Running on kernel page tables!");

    /* 6. Per-section permissions (now that we're on our tables).
     *    Re-map each section with correct permissions.
     *    We do this AFTER the switch because changing permissions
     *    on Limine's tables would be rude. */
    {
        uint64_t text_virt  = (uint64_t)&__text_start;
        uint64_t text_phys  = kernel_phys_base + (text_virt - (uint64_t)&__kernel_start);
        uint64_t text_size  = (uint64_t)&__text_end - text_virt;

        uint64_t ro_virt    = (uint64_t)&__rodata_start;
        uint64_t ro_phys    = kernel_phys_base + (ro_virt - (uint64_t)&__kernel_start);
        uint64_t ro_size    = (uint64_t)&__rodata_end - ro_virt;

        uint64_t data_virt  = (uint64_t)&__data_start;
        uint64_t data_phys  = kernel_phys_base + (data_virt - (uint64_t)&__kernel_start);
        uint64_t data_size  = (uint64_t)&__data_end - data_virt;

        uint64_t bss_virt   = (uint64_t)&__bss_start;
        uint64_t bss_phys   = kernel_phys_base + (bss_virt - (uint64_t)&__kernel_start);
        uint64_t bss_size   = (uint64_t)&__bss_end - bss_virt;

        /* .text → Read+Execute (no write, no NX) */
        if (text_size > 0)
            vmm_map_range(text_virt, text_phys, text_size, VMM_FLAGS_KERNEL_RX);

        /* .rodata → Read-only (no write, NX) */
        if (ro_size > 0)
            vmm_map_range(ro_virt, ro_phys, ro_size, VMM_FLAGS_KERNEL_RO);

        /* .data → Read+Write (NX) */
        if (data_size > 0)
            vmm_map_range(data_virt, data_phys, data_size, VMM_FLAGS_KERNEL_RW);

        /* .bss → Read+Write (NX) */
        if (bss_size > 0)
            vmm_map_range(bss_virt, bss_phys, bss_size, VMM_FLAGS_KERNEL_RW);

        /* Gap-page sweep: any kernel page not in a named section
         * still has the initial W+X flags. Set NX on those gaps.
         * (Alignment padding between .text/.rodata/.data/.bss) */
        {
            uint64_t kstart = (uint64_t)&__kernel_start;
            uint64_t kend   = (uint64_t)&__kernel_end;
            uint32_t gaps_fixed = 0;

            for (uint64_t va = kstart; va < kend; va += PAGE_SIZE) {
                /* Skip pages inside known sections */
                if (va >= text_virt  && va < text_virt  + text_size) continue;
                if (va >= ro_virt    && va < ro_virt    + ro_size)   continue;
                if (va >= data_virt  && va < data_virt  + data_size) continue;
                if (va >= bss_virt   && va < bss_virt   + bss_size)  continue;

                /* This is a gap page — set NX to close W^X violation */
                uint64_t gap_phys = kernel_phys_base + (va - kstart);
                vmm_map_page(va, gap_phys, VMM_FLAGS_KERNEL_RW);
                gaps_fixed++;
            }
            if (gaps_fixed > 0) {
                LOG_INFO("VMM: %u gap pages fixed (W+X → RW+NX)", gaps_fixed);
            }
        }

        LOG_OK("VMM: Per-section permissions applied (W^X clean)");
        LOG_INFO("  .text:   %lu KB (RX)", text_size / 1024);
        LOG_INFO("  .rodata: %lu KB (RO)", ro_size / 1024);
        LOG_INFO("  .data:   %lu KB (RW+NX)", data_size / 1024);
        LOG_INFO("  .bss:    %lu KB (RW+NX)", bss_size / 1024);

        /* W^X now enforced — all future map_page calls are audited.
         * OpenBSD does the same: permissive during boot, strict after. */
        vmm_wx_enforced = 1;
        LOG_OK("VMM: W^X enforcement ENABLED (OpenBSD mode)");
    }

    /* 7. Kernel stack improvements:
     *    a) Allocate a proper 16KB stack for TSS RSP0 (used during
     *       privilege transitions when we add user-mode in Sprint 4+).
     *    b) Add guard page on Limine's current boot stack.
     *
     *    NOTE: We do NOT switch RSP here because doing so via inline
     *    asm would destroy the current C stack frame. The actual boot
     *    stack switch will happen in Sprint 4 via a dedicated asm
     *    trampoline during scheduler init. */
    {
        #define KERNEL_STACK_PAGES 4   /* 16 KB */
        #define KERNEL_STACK_VIRT  0xFFFFFF0000100000UL

        /* Allocate IST/RSP0 stack pages */
        for (int i = 0; i < KERNEL_STACK_PAGES; i++) {
            uint64_t phys_page = pmm_alloc_pages_zero(0);
            KASSERT(phys_page != 0);
            vmm_map_page(
                KERNEL_STACK_VIRT + (i + 1) * PAGE_SIZE,
                phys_page,
                VMM_FLAGS_KERNEL_RW
            );
        }
        /* Page at KERNEL_STACK_VIRT is NOT mapped = guard page */

        uint64_t rsp0_top = KERNEL_STACK_VIRT + (KERNEL_STACK_PAGES + 1) * PAGE_SIZE;
        tss_set_rsp0(rsp0_top);

        LOG_OK("VMM: TSS RSP0 stack at 0x%lx (16 KB, guard at 0x%lx)",
               KERNEL_STACK_VIRT + PAGE_SIZE, KERNEL_STACK_VIRT);

        /* Guard page on Limine's boot stack */
        uint64_t rsp;
        asm volatile("mov %%rsp, %0" : "=r"(rsp));
        uint64_t guard_page = PAGE_ALIGN_DOWN(rsp) - PAGE_SIZE;
        vmm_unmap_page(guard_page);
        LOG_OK("VMM: Boot stack guard page at 0x%lx", guard_page);

        /* IST1 stack for Double Fault (8KB = 2 pages + guard) */
        #define IST1_STACK_PAGES 2
        #define IST1_STACK_VIRT  0xFFFFFF0000200000UL

        for (int i = 0; i < IST1_STACK_PAGES; i++) {
            uint64_t phys_page = pmm_alloc_pages_zero(0);
            KASSERT(phys_page != 0);
            vmm_map_page(
                IST1_STACK_VIRT + (i + 1) * PAGE_SIZE,
                phys_page,
                VMM_FLAGS_KERNEL_RW
            );
        }
        uint64_t ist1_top = IST1_STACK_VIRT + (IST1_STACK_PAGES + 1) * PAGE_SIZE;
        tss_set_ist1(ist1_top);
        LOG_OK("VMM: IST1 (#DF) stack at 0x%lx (8 KB, guard at 0x%lx)",
               IST1_STACK_VIRT + PAGE_SIZE, IST1_STACK_VIRT);
    }

    LOG_INFO("VMM: %lu page tables allocated (%lu KB overhead)",
             vmm_tables_allocated, vmm_tables_allocated * 4);
}

/* ── Diagnostics: dump VMM state ──────────────────────────────────── */

void vmm_dump_tables(void) {
    if (pml4_phys == 0) {
        LOG_WARN("VMM: No page tables (not initialized)");
        return;
    }

    LOG_INFO("VMM Page Table Summary:");
    kprintf("  PML4: 0x%lx, %lu tables allocated (%lu KB)\n",
            pml4_phys, vmm_tables_allocated, vmm_tables_allocated * 4);

    uint64_t *pml4 = (uint64_t *)PHYS2VIRT(pml4_phys);
    uint32_t mapped_4k = 0, mapped_2m = 0;

    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)PHYS2VIRT(pte_to_phys(pml4[i]));

        for (int j = 0; j < PAGE_TABLE_ENTRIES; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            if (pdpt[j] & PTE_HUGE) continue;  /* 1GB page */

            uint64_t *pd = (uint64_t *)PHYS2VIRT(pte_to_phys(pdpt[j]));
            for (int k = 0; k < PAGE_TABLE_ENTRIES; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                if (pd[k] & PTE_HUGE) {
                    mapped_2m++;
                } else {
                    uint64_t *pt = (uint64_t *)PHYS2VIRT(pte_to_phys(pd[k]));
                    for (int l = 0; l < PAGE_TABLE_ENTRIES; l++) {
                        if (pt[l] & PTE_PRESENT) mapped_4k++;
                    }
                }
            }
        }
    }

    kprintf("  Mapped: %u x 4KB pages + %u x 2MB huge pages\n", mapped_4k, mapped_2m);
    kprintf("  Total mapped: %lu KB + %lu MB = %lu MB\n",
            (uint64_t)mapped_4k * 4, (uint64_t)mapped_2m * 2,
            ((uint64_t)mapped_4k * 4) / 1024 + (uint64_t)mapped_2m * 2);
}

uint64_t vmm_tables_count(void) {
    return vmm_tables_allocated;
}

/* ── W^X Audit (OpenBSD W^X + macOS Hardened Runtime) ──────────── */

uint32_t vmm_audit_wx(void) {
    if (pml4_phys == 0) return 0;

    uint32_t violations = 0;
    uint64_t *pml4 = (uint64_t *)PHYS2VIRT(pml4_phys);

    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)PHYS2VIRT(pte_to_phys(pml4[i]));

        for (int j = 0; j < PAGE_TABLE_ENTRIES; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            if (pdpt[j] & PTE_HUGE) continue;

            uint64_t *pd = (uint64_t *)PHYS2VIRT(pte_to_phys(pdpt[j]));
            for (int k = 0; k < PAGE_TABLE_ENTRIES; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                if (pd[k] & PTE_HUGE) {
                    /* 2MB page: check W^X */
                    if ((pd[k] & PTE_WRITE) && !(pd[k] & PTE_NX)) {
                        violations++;
                    }
                    continue;
                }

                uint64_t *pt = (uint64_t *)PHYS2VIRT(pte_to_phys(pd[k]));
                for (int l = 0; l < PAGE_TABLE_ENTRIES; l++) {
                    if (!(pt[l] & PTE_PRESENT)) continue;
                    /* W^X: both Writable AND Executable = violation */
                    if ((pt[l] & PTE_WRITE) && !(pt[l] & PTE_NX)) {
                        violations++;
                    }
                }
            }
        }
    }

    if (violations == 0) {
        LOG_OK("VMM: W^X audit PASSED — 0 violations");
    } else {
        LOG_ERROR("VMM: W^X audit FAILED — %u violations!", violations);
    }

    return violations;
}
