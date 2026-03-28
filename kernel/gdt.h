/*
 * Anykernel OS v2.1 — GDT & TSS
 * Global Descriptor Table + Task State Segment for x86_64 Long Mode
 */

#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* GDT selectors */
#define GDT_NULL        0x00
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

/* Task State Segment (x86_64) */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;          /* Ring 0 stack pointer — used on privilege change */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];        /* Interrupt Stack Table entries 1-7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;   /* I/O Permission Bitmap offset     */
} __attribute__((packed));

/* Initialize GDT with kernel/user segments + TSS, load with lgdt + ltr */
void gdt_init(void);

/* Update RSP0 in the TSS (called during context switch) */
void tss_set_rsp0(uint64_t rsp0);

/* Update IST1 in the TSS (Double Fault stack) */
void tss_set_ist1(uint64_t ist1);

#endif /* GDT_H */
