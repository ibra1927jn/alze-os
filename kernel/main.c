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
#include "ktimer.h"
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
    uint64_t uptime_ms = pit_get_ticks() * TIMER_TICK_MS;
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
            pit_set_oneshot(PIT_BASE_FREQ / (1000 / TIMER_TICK_MS));
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

/* ── Phase: early hardware (UART, SSP, CPU, GDT, IDT, PIC/PIT) ── */

static void init_early_hw(void) {
    uart_init();
    LOG_OK("UART COM1 initialized at 115200 baud");

    ssp_init();
    LOG_OK("Stack canary randomized (RDTSC)");

    cpuid_detect();

    KASSERT(LIMINE_BASE_REVISION_SUPPORTED);
    LOG_OK("Limine base revision OK");

    gdt_init();
    LOG_OK("GDT loaded (7 entries, TSS with RSP0 + IST1)");

    idt_init();
    LOG_OK("IDT loaded (#DE #UD #DF #GP #PF + IRQ0/1 + LAPIC timer/IPI)");

    /* Per-CPU data — MUST be before sti (sched_tick reads GS) */
    percpu_init_bsp();

    pic_init();
    pit_init(1000 / TIMER_TICK_MS);
    pic_unmask(IRQ_TIMER);
    asm volatile("sti");
    LOG_OK("PIC remapped, PIT at %u Hz, interrupts ENABLED", 1000 / TIMER_TICK_MS);
}

/* ── Phase: memory subsystems (HHDM, PMM, VMM, VMA, HAL) ──────── */

static void init_memory(void) {
    KASSERT(hhdm_request.response != NULL);
    hhdm_offset = hhdm_request.response->offset;
    LOG_INFO("HHDM offset: 0x%016lx", hhdm_offset);

    KASSERT(memmap_request.response != NULL);
    memmap_resp_saved = memmap_request.response;
    print_memory_map(memmap_request.response);

    pmm_init(memmap_request.response, hhdm_offset);
    pmm_dump_stats();

    vmm_init();
    vmm_dump_tables();

    vma_init_kernel();
    hal_init();
    cpuidle_init();
    vmm_audit_wx();
}

/* ── Phase: devices and filesystems ──────────────────────────────── */

static void init_devices(void) {
    if (fb_request.response && fb_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
        KASSERT(fb != NULL);
        KASSERT(fb->address != NULL);
        console_init(fb->address, fb->width, fb->height, fb->pitch, fb->bpp);
        LOG_OK("Framebuffer console: %lux%lu, %u bpp",
               fb->width, fb->height, fb->bpp);
    }

    kb_init();

    vfs_init();
    devfs_init();

    mempressure_init();
    watchdog_init();

    lapic_init();
    tlb_shootdown_init();

    ramdisk_init();

    pci_enumerate();
    xhci_init();
}

/* ── Kernel main ──────────────────────────────────────────────── */

void _start(void) {
    init_early_hw();
    init_memory();
    init_devices();

    /* Self-tests */
    register_selftests();
    int failures = run_all_selftests();

    /* Boot report */
    print_boot_memory_report();
    kmalloc_dump_stats();
    print_boot_banner(failures);

    /* Scheduler */
    sched_init();
    LOG_OK("Scheduler initialized (per-CPU via GS)");

    run_runtime_tests();

    LOG_INFO("System ready. Init task handling keyboard.");
    idle_loop();
}
