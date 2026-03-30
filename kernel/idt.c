/*
 * Anykernel OS v2.1 — IDT Setup + C Exception Handler
 *
 * The ISR stubs live in interrupts.asm (pure NASM).
 * This file handles:
 *   1. IDT table setup and LIDT
 *   2. C-level exception handler with register dump
 */

#include "idt.h"
#include "kprintf.h"
#include "panic.h"
#include "gdt.h"
#include "string.h"
#include "pic.h"
#include "log.h"
#include "kb.h"
#include "sched.h"
#include "ktimer.h"
#include "watchdog.h"
#include "lapic.h"
#include "tlb_shootdown.h"
#include <stdint.h>

/* x86 exception vector numbers */
#define EXCEPTION_PAGE_FAULT  14

/* Page fault error code bits */
#define PF_PRESENT       (1 << 0)   /* 0 = not present, 1 = protection violation */
#define PF_WRITE         (1 << 1)   /* 0 = read, 1 = write */
#define PF_USER          (1 << 2)   /* 0 = kernel, 1 = user mode */
#define PF_RESERVED_BIT  (1 << 3)   /* 1 = reserved bit set in PTE */
#define PF_INSTR_FETCH   (1 << 4)   /* 1 = instruction fetch (NX) */

/* ── IDT structures ───────────────────────────────────────────── */

struct idt_entry {
    uint16_t offset_low;      /* offset bits 0..15 */
    uint16_t selector;        /* code segment selector */
    uint8_t  ist;             /* IST offset (0 = none) */
    uint8_t  type_attr;       /* type and attributes */
    uint16_t offset_mid;      /* offset bits 16..31 */
    uint32_t offset_high;     /* offset bits 32..63 */
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Interrupt frame — matches the push order in interrupts.asm */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;          /* pushed by stub */
    uint64_t error_code;      /* pushed by CPU or dummy 0 */
    uint64_t rip;             /* pushed by CPU */
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

/* ── IDT table ────────────────────────────────────────────────── */

static struct idt_entry idt[256];
static struct idt_pointer idtr;

/* ── External: ISR stubs defined in interrupts.asm ────────────── */

extern void isr_stub_0(void);
extern void isr_stub_6(void);
extern void isr_stub_7(void);
extern void isr_stub_8(void);
extern void isr_stub_13(void);
extern void isr_stub_14(void);
extern void isr_stub_16(void);
extern void isr_stub_32(void);  /* IRQ0: Timer */
extern void isr_stub_33(void);  /* IRQ1: Keyboard */
extern void isr_stub_253(void); /* LAPIC Timer (vector 0xFD) */
extern void isr_stub_254(void); /* IPI: TLB Shootdown (vector 0xFE) */

/* ── Exception names ──────────────────────────────────────────── */

static const char *exception_name(uint64_t vector) {
    switch (vector) {
        case 0:  return "Divide Error (#DE)";
        case 6:  return "Invalid Opcode (#UD)";
        case 7:  return "Device Not Available (#NM) — FPU/SSE used without CR0.TS clear";
        case 8:  return "Double Fault (#DF)";
        case 13: return "General Protection (#GP)";
        case 14: return "Page Fault (#PF)";
        case 16: return "x87 Floating-Point Error (#MF)";
        default: return "Unknown Exception";
    }
}

/* ── C exception handler (called from interrupts.asm) ─────────── */

void exception_handler_c(struct interrupt_frame *frame) {
    uint64_t cr2;
    asm volatile("movq %%cr2, %0" : "=r"(cr2));

    kprintf("\n\n");
    kprintf(ANSI_RED "╔══════════════════════════════════════╗" ANSI_RESET "\n");
    kprintf(ANSI_RED "║        !!! CPU EXCEPTION !!!         ║" ANSI_RESET "\n");
    kprintf(ANSI_RED "╚══════════════════════════════════════╝" ANSI_RESET "\n");
    kprintf(ANSI_RED "  Vector:     " ANSI_RESET "%lu — %s\n", frame->vector, exception_name(frame->vector));
    kprintf(ANSI_RED "  Error Code: " ANSI_RESET "0x%lx\n", frame->error_code);
    kprintf(ANSI_YELLOW "  RIP:        " ANSI_RESET "0x%016lx\n", frame->rip);
    kprintf(ANSI_YELLOW "  CS:         " ANSI_RESET "0x%lx\n", frame->cs);
    kprintf(ANSI_YELLOW "  RFLAGS:     " ANSI_RESET "0x%016lx\n", frame->rflags);
    kprintf(ANSI_YELLOW "  RSP:        " ANSI_RESET "0x%016lx\n", frame->rsp);
    kprintf(ANSI_YELLOW "  CR2:        " ANSI_RESET "0x%016lx\n", cr2);
    kprintf("\n" ANSI_CYAN "  Registers:" ANSI_RESET "\n");
    kprintf("    RAX=%016lx  RBX=%016lx\n", frame->rax, frame->rbx);
    kprintf("    RCX=%016lx  RDX=%016lx\n", frame->rcx, frame->rdx);
    kprintf("    RSI=%016lx  RDI=%016lx\n", frame->rsi, frame->rdi);
    kprintf("    RBP=%016lx  R8 =%016lx\n", frame->rbp, frame->r8);
    kprintf("    R9 =%016lx  R10=%016lx\n", frame->r9, frame->r10);
    kprintf("    R11=%016lx  R12=%016lx\n", frame->r11, frame->r12);
    kprintf("    R13=%016lx  R14=%016lx\n", frame->r13, frame->r14);
    kprintf("    R15=%016lx\n", frame->r15);

    if (frame->vector == EXCEPTION_PAGE_FAULT) {
        kprintf("\n" ANSI_RED "  Page Fault details:" ANSI_RESET "\n");
        kprintf("    %s\n", (frame->error_code & PF_PRESENT) ? "Protection violation" : "Page not present");
        kprintf("    %s access\n", (frame->error_code & PF_WRITE) ? "Write" : "Read");
        kprintf("    %s mode\n", (frame->error_code & PF_USER) ? "User" : "Kernel");
        if (frame->error_code & PF_RESERVED_BIT) kprintf("    Reserved bit set\n");
        if (frame->error_code & PF_INSTR_FETCH)  kprintf("    Instruction fetch\n");
    }

    asm volatile("cli");
    for (;;) {
        asm volatile("hlt");
    }
}

/* ── IRQ handler (called from irq_common in interrupts.asm) ──── */

/* Forward decl from pic.c */
extern void irq_dispatch(uint8_t irq);

void irq_handler_c(struct interrupt_frame *frame) {
    uint8_t irq = (uint8_t)(frame->vector - PIC_IRQ_BASE_MASTER);

    switch (irq) {
        case IRQ_TIMER:
            pit_tick();
            ktimer_tick();  /* Process timer wheel (O(1) per tick) */
            sched_tick();   /* Set need_resched flag */
            watchdog_check(); /* Detect hung tasks */

            /*
             * TRAP 1: EOI MUST be sent BEFORE schedule().
             * If context_switch jumps to another thread, the
             * pic_eoi(irq) at the bottom never executes, and
             * the PIC stops delivering timer interrupts forever.
             */
            pic_eoi(irq);

            if (sched_need_resched()) {
                schedule();
            }
            return;  /* EOI already sent */

        case IRQ_KEYBOARD:
            kb_irq_handler();
            break;
        default:
            irq_dispatch(irq);
            break;
    }

    pic_eoi(irq);
}

/* ── IDT setup ────────────────────────────────────────────────── */

/* Type/attr byte: Present=1, DPL=0, Type=0xE (64-bit Interrupt Gate) */
#define IDT_GATE_KERNEL_INTR  0x8E

static void idt_set_gate(int vector, void (*handler)(void), uint8_t ist) {
    uint64_t addr = (uint64_t)handler;

    idt[vector].offset_low  = addr & 0xFFFF;
    idt[vector].selector    = GDT_KERNEL_CODE;
    idt[vector].ist         = ist;
    idt[vector].type_attr   = IDT_GATE_KERNEL_INTR;
    idt[vector].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].reserved    = 0;
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));

    /* Install exception handlers (stubs from interrupts.asm) */
    idt_set_gate(0,  isr_stub_0,  0);  /* #DE */
    idt_set_gate(6,  isr_stub_6,  0);  /* #UD */
    idt_set_gate(7,  isr_stub_7,  0);  /* #NM — FPU/SSE */
    idt_set_gate(8,  isr_stub_8,  1);  /* #DF — uses IST1 */
    idt_set_gate(13, isr_stub_13, 0);  /* #GP */
    idt_set_gate(14, isr_stub_14, 0);  /* #PF */
    idt_set_gate(16, isr_stub_16, 0);  /* #MF — x87 FP error */

    /* Hardware IRQs (remapped by PIC to 0x20+) */
    idt_set_gate(IRQ_VECTOR(IRQ_TIMER), isr_stub_32, 0);     /* IRQ0: Timer */
    idt_set_gate(IRQ_VECTOR(IRQ_KEYBOARD), isr_stub_33, 0);  /* IRQ1: Keyboard */

    /* LAPIC vectors (fuera del rango PIC) */
    idt_set_gate(LAPIC_TIMER_VECTOR, isr_stub_253, 0);    /* LAPIC Timer */
    idt_set_gate(IPI_TLB_SHOOTDOWN, isr_stub_254, 0);     /* TLB Shootdown IPI */

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;

    asm volatile("lidt %0" : : "m"(idtr));
}
