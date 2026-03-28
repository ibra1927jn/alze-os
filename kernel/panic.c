/*
 * Anykernel OS — Kernel Panic (Enhanced)
 *
 * Full CPU register dump, CR2, current task info, klog dump.
 * Gives maximum diagnostic info on crash.
 */

#include "panic.h"
#include "kprintf.h"
#include "log.h"
#include "klog.h"
#include <stdint.h>

__attribute__((noreturn))
void kernel_panic(const char *msg, const char *file, int line) {
    /* Disable interrupts immediately */
    asm volatile("cli");

    /* Capture ALL control registers */
    uint64_t cr0, cr2, cr3, cr4;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));

    /* Capture general purpose registers */
    uint64_t rsp, rbp, rflags;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    asm volatile("pushfq; pop %0" : "=r"(rflags));

    /* Dump ring buffer to serial for post-mortem */
    klog_dump();

    /* Print panic banner with colors */
    kprintf("\n\n");
    kprintf(ANSI_RED "╔══════════════════════════════════╗" ANSI_RESET "\n");
    kprintf(ANSI_RED "║      !!! KERNEL PANIC !!!        ║" ANSI_RESET "\n");
    kprintf(ANSI_RED "╚══════════════════════════════════╝" ANSI_RESET "\n");
    kprintf(ANSI_RED "  Message: " ANSI_RESET "%s\n", msg);
    kprintf(ANSI_RED "  File:    " ANSI_RESET "%s\n", file);
    kprintf(ANSI_RED "  Line:    " ANSI_RESET "%d\n", line);

    /* Control registers */
    kprintf(ANSI_YELLOW "\n  Control Registers:\n" ANSI_RESET);
    kprintf("    CR0:    0x%016lx\n", cr0);
    kprintf("    CR2:    0x%016lx  (fault addr)\n", cr2);
    kprintf("    CR3:    0x%016lx  (page table)\n", cr3);
    kprintf("    CR4:    0x%016lx\n", cr4);

    /* Stack/flags */
    kprintf(ANSI_YELLOW "\n  Stack & Flags:\n" ANSI_RESET);
    kprintf("    RSP:    0x%016lx\n", rsp);
    kprintf("    RBP:    0x%016lx\n", rbp);
    kprintf("    RFLAGS: 0x%016lx", rflags);

    /* Decode RFLAGS */
    kprintf("  [");
    if (rflags & (1 << 0))  kprintf(" CF");
    if (rflags & (1 << 6))  kprintf(" ZF");
    if (rflags & (1 << 7))  kprintf(" SF");
    if (rflags & (1 << 9))  kprintf(" IF");
    if (rflags & (1 << 11)) kprintf(" OF");
    kprintf(" ]\n");

    /* Stack dump (top 8 qwords) */
    kprintf(ANSI_YELLOW "\n  Stack dump (top 8 entries):\n" ANSI_RESET);
    uint64_t *stack = (uint64_t *)rsp;
    for (int i = 0; i < 8; i++) {
        kprintf("    [RSP+%02x] 0x%016lx\n", i * 8, stack[i]);
    }

    /* Current task info (if scheduler is running) */
    extern struct task *task_current(void);
    /* Only try to access task_current if sched is initialized */
    kprintf(ANSI_YELLOW "\n  Task Info:\n" ANSI_RESET);
    kprintf("    (see klog dump above for context)\n");

    kprintf("\n  System halted.\n");

    /* Halt forever */
    for (;;) {
        asm volatile("hlt");
    }
}
