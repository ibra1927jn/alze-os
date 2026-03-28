/*
 * Anykernel OS v2.1 — Early IDT (Interrupt Descriptor Table)
 *
 * Minimal IDT to catch critical CPU exceptions before Sprint 3.
 * Handles: #DE(0), #UD(6), #DF(8), #GP(13), #PF(14)
 *
 * Without this, any exception → triple fault → silent reboot.
 * With this, exceptions → PANIC with register dump → frozen CPU.
 */

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* Initialize and load the early IDT */
void idt_init(void);

#endif /* IDT_H */
