# PROGRESS.md — Estado del proyecto

## En curso
- [2026-03-28] | Quality/hardening pass: sched.c, vmm.c, kmalloc.c, string.c, main.c (12 fixes) | 100%

## Completado
- [2026-03-28] | TLB shootdown IPI infrastructure (vector 0xFE, tlb_shootdown.c/.h, wired to vmm_flush_tlb) | Pending commit
- [2026-03-28] | Fix bitmap_find_first UB (bsfq with input 0 in sched.c) | Pending commit
- [2026-03-28] | Fix PMM ref counting (_reserved → ref_count with proper API) | Pending commit
- [2026-03-28] | TLB shootdown ISR stub (isr_stub_254 in interrupts.asm, registered in IDT, LAPIC EOI in handler) | Pending commit
- [2026-03-28] | ext2 read-only filesystem (superblock, GDT, inode read, directory listing, file read direct blocks) | Pending commit
- [2026-03-28] | Ramdisk driver (Limine boot module backed, auto-mounts ext2 if valid) | Pending commit
- [2026-03-28] | PCI enumeration (mechanism 1, full bus scan, find by class/sub/progif) | Pending commit
- [2026-03-28] | xHCI USB 3.x minimal detection (PCI probe, BAR0 MMIO, reset, port enumeration) | Pending commit
- [2026-03-28] | LAPIC driver (init, EOI, IPI, timer calibration via PIT ch2, MMIO r/w, refactored TLB shootdown) | Pending commit

## Pendiente
- SMP AP startup (update active_cpus for TLB shootdown) | Prioridad: media
- ext2 indirect block support (single/double/triple indirect) | Prioridad: baja
- xHCI full initialization (device context, command/event rings, transfers) | Prioridad: baja
- USB HID protocol (keyboard/mouse over USB) | Prioridad: baja
- AHCI driver for real disk ext2 | Prioridad: baja
