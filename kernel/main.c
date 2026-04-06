/*
 * Anykernel OS v2.1 — Kernel Entry Point
 *
 * Limine hands control here in Long Mode (64-bit), Ring 0.
 * Initializes all subsystems and prints diagnostics.
 */

#include <stdint.h>
#include <stddef.h>

#include "uart.h"
#include "gdt.h"
#include "idt.h"
#include "panic.h"
#include "kprintf.h"
#include "log.h"
#include "memory.h"
#include "pmm.h"
#include "kmalloc.h"
#include "vmm.h"
#include "kb.h"
#include "console.h"
#include "cpuid.h"
#include "sched.h"
#include "vfs.h"
#include "vma.h"
#include "mempressure.h"
#include "watchdog.h"
#include "tlb_shootdown.h"
#include "lapic.h"
#include "ramdisk.h"
#include "pci.h"
#include "xhci.h"
#include "selftest.h"
#include "pic.h"
#include "percpu.h"
#include "cpuidle.h"

/* Limine protocol */
#include "limine.h"

/* SSP init (defined in ssp.c) */
extern void ssp_init(void);

/* ── Global: HHDM offset (used by PHYS2VIRT / VIRT2PHYS) ─────── */

uint64_t hhdm_offset = 0;

/* ── Limine requests ──────────────────────────────────────────── */

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3)

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

/* ── Helpers ──────────────────────────────────────────────────── */

static const char *memmap_type_str(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:                 return "Usable";
        case LIMINE_MEMMAP_RESERVED:               return "Reserved";
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "ACPI Reclaimable";
        case LIMINE_MEMMAP_ACPI_NVS:               return "ACPI NVS";
        case LIMINE_MEMMAP_BAD_MEMORY:             return "Bad Memory";
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "Bootloader Reclaim";
        case LIMINE_MEMMAP_KERNEL_AND_MODULES:     return "Kernel & Modules";
        case LIMINE_MEMMAP_FRAMEBUFFER:            return "Framebuffer";
        default:                                   return "Unknown";
    }
}

/* ── Print memory map and tally usable pages ──────────────────── */

static void print_memory_map(struct limine_memmap_response *resp) {
    uint64_t entry_count = resp->entry_count;
    LOG_INFO("Memory map: %lu entries", entry_count);

    uint64_t usable_bytes = 0;
    uint64_t usable_pages = 0;
    for (uint64_t i = 0; i < entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];

        uint64_t aligned_base = PAGE_ALIGN_UP(e->base);
        uint64_t aligned_end  = PAGE_ALIGN_DOWN(e->base + e->length);
        uint64_t pages = (aligned_end > aligned_base)
                         ? (aligned_end - aligned_base) / PAGE_SIZE : 0;

        kprintf("  [%2lu] %p-%p %8lu KB %-22s",
                i, (void *)e->base, (void *)(e->base + e->length),
                e->length / 1024, memmap_type_str(e->type));

        if (e->type == LIMINE_MEMMAP_USABLE) {
            kprintf(" (%lu pages)", pages);
            usable_bytes += e->length;
            usable_pages += pages;
        }
        kprintf("\n");
    }
    LOG_INFO("Total usable: %lu KB (%lu MB) = %lu pages",
             usable_bytes / 1024, usable_bytes / (1024 * 1024), usable_pages);
}

/* ── Print boot memory report ─────────────────────────────────── */

static void print_boot_memory_report(void) {
    extern char __kernel_start, __kernel_end;
    uint64_t kernel_size = (uint64_t)&__kernel_end - (uint64_t)&__kernel_start;
    uint64_t free_pg = pmm_free_count();
    uint64_t used_pg = pmm_used_count();
    uint64_t total = free_pg + used_pg;
    uint64_t pct = (free_pg * 100) / (total > 0 ? total : 1);

    kprintf("\n--- Boot Memory Report ---\n");
    kprintf("  Kernel:   %lu KB (0x%lx - 0x%lx)\n",
            kernel_size / 1024, (uint64_t)&__kernel_start, (uint64_t)&__kernel_end);
    kprintf("  RAM:      %lu pages (%lu MB)\n", total, total * 4 / 1024);
    kprintf("  Free:     %lu pages (%lu KB) [%lu%%]\n", free_pg, free_pg * 4, pct);
    kprintf("  Used:     %lu pages (%lu KB)\n", used_pg, used_pg * 4);
    kprintf("  Peak:     %lu pages (%lu KB)\n", pmm_peak_used(), pmm_peak_used() * 4);
}

/* Tests are in kernel/tests.c */
extern void register_selftests(void);

/* Saved for VMM to determine HHDM size from actual memory map */
struct limine_memmap_response *memmap_resp_saved = 0;

/* Runtime tests (in runtime_tests.c) */
extern void run_runtime_tests(void);

/* Subsystem externs (defined in their respective .c files) */
extern void hal_init(void);
extern void devfs_init(void);
extern void workqueue_process_system(void);

/* ── Print boot banner ────────────────────────────────────────── */

static void print_boot_banner(int failures) {
    uint64_t uptime_ms = pit_get_ticks() * 10;  /* 100 Hz = 10ms per tick */
    kprintf("\n==============================\n");
    kprintf("  Anykernel OS v0.7.0\n");
    kprintf("  %d tests, %d failures\n", selftest_count(), failures);
    kprintf("  Boot time: %lu ms\n", uptime_ms);
    kprintf("  Idle: %s\n", cpuidle_has_mwait() ? "MWAIT (deep sleep)" : "HLT");
    kprintf("  Timer: tickless-ready\n");
    kprintf("==============================\n");

    kprintf("\n");
    if (failures > 0) {
        LOG_ERROR("SELF-TEST FAILURES: %d", failures);
    }
}

/* ── Idle loop — runs forever after boot ──────────────────────── */

static _Noreturn void idle_loop(void) {
    for (;;) {
        sched_reap_dead();
        workqueue_process_system();
        mempressure_check();

        if (pit_is_tickless() == 0) {
            pit_set_oneshot(PIT_BASE_FREQ / 100);
        }

        cpu_idle();

        char c;
        while ((c = kb_getchar()) != 0) {
            if (console_available()) {
                console_putchar(c);
            }
        }
    }
}

/* ── Kernel main ──────────────────────────────────────────────── */

void _start(void) {

    /* 1. UART — must be first */
    uart_init();
    LOG_OK("UART COM1 initialized at 115200 baud");

    /* 2. SSP — randomize stack canary */
    ssp_init();
    LOG_OK("Stack canary randomized (RDTSC)");

    /* 3. CPU identification */
    cpuid_detect();

    /* 4. Verify Limine protocol */
    KASSERT(LIMINE_BASE_REVISION_SUPPORTED);
    LOG_OK("Limine base revision OK");

    /* 5. GDT + TSS */
    gdt_init();
    LOG_OK("GDT loaded (7 entries, TSS with RSP0 + IST1)");

    /* 6. IDT — catch exceptions + handle IRQs */
    idt_init();
    LOG_OK("IDT loaded (#DE #UD #DF #GP #PF + IRQ0/1 + LAPIC timer/IPI)");

    /* 6b. Per-CPU data — MUST be before sti (sched_tick reads GS) */
    percpu_init_bsp();

    /* 7. PIC + PIT — hardware interrupt infrastructure */
    pic_init();
    pit_init(100);  /* 100 Hz = 10ms tick */
    pic_unmask(IRQ_TIMER);  /* Enable timer IRQ */
    asm volatile("sti");    /* ENABLE INTERRUPTS! */
    LOG_OK("PIC remapped, PIT at 100 Hz, interrupts ENABLED");

    /* 8. HHDM */
    KASSERT(hhdm_request.response != NULL);
    hhdm_offset = hhdm_request.response->offset;
    LOG_INFO("HHDM offset: 0x%016lx", hhdm_offset);

    /* 9. Memory map */
    KASSERT(memmap_request.response != NULL);
    memmap_resp_saved = memmap_request.response;
    print_memory_map(memmap_request.response);

    /* 10. PMM — Physical Memory Manager (Buddy Allocator) */
    pmm_init(memmap_request.response, hhdm_offset);
    pmm_dump_stats();

    /* 11. VMM — Virtual Memory Manager (Paging) */
    vmm_init();
    vmm_dump_tables();

    /* 11b. VMA — Virtual Memory Areas */
    vma_init_kernel();

    /* 11c. HAL — Hardware Abstraction Layer */
    hal_init();

    /* 11d. CPU Idle States */
    cpuidle_init();

    /* 11e. W^X Audit */
    vmm_audit_wx();

    /* 12. Framebuffer Console */
    if (fb_request.response && fb_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
        KASSERT(fb != NULL);
        KASSERT(fb->address != NULL);
        console_init(fb->address, fb->width, fb->height, fb->pitch, fb->bpp);
        LOG_OK("Framebuffer console: %lux%lu, %u bpp",
               fb->width, fb->height, fb->bpp);
    }

    /* 13. Keyboard */
    kb_init();

    /* 13b. VFS + devfs */
    vfs_init();
    devfs_init();

    /* 13c. Memory Pressure + Watchdog */
    mempressure_init();
    watchdog_init();

    /* 13d. LAPIC */
    lapic_init();

    /* 13e. TLB Shootdown */
    tlb_shootdown_init();

    /* 13f. Ramdisk + ext2 */
    ramdisk_init();

    /* 13g. PCI + xHCI */
    pci_enumerate();
    xhci_init();

    /* 14. Self-tests */
    register_selftests();
    int failures = run_all_selftests();

    /* 15. Boot memory report */
    print_boot_memory_report();

    kmalloc_dump_stats();

    /* Banner */
    print_boot_banner(failures);

    /* ── Step 16: Scheduler ────────────────────────── */
    sched_init();
    LOG_OK("Scheduler initialized (per-CPU via GS)");

    run_runtime_tests();

    LOG_INFO("System ready. Init task handling keyboard.");

    idle_loop();
}
