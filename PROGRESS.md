# PROGRESS.md — Estado del proyecto

## En curso
- [2026-03-29] | Heartbeat maintenance: 7 fixes + 3 cleanup items across pmm, msgqueue, ramdisk, ext2, vfs, pic, idt, repo | 100%

## Sesion 2026-03-29 (heartbeat pass 3)
- fix(sched): add missing panic.h include — KASSERT was undeclared, breaking build | db3bc1c
- refactor(kernel): normalize limine.h include paths in pmm.c, ramdisk.c, main.c | ce1d9eb
- Build: clean (was 1 error), Tests: 29/29 passing (unchanged)

## Sesion 2026-03-29 (heartbeat pass 2)
- fix(pmm): move irq_flags inside #ifndef guard to fix -Wunused-variable | 2670721
- fix(test): guard PMM_USERSPACE_TEST define to fix -Wmacro-redefined | 2670721
- fix(ramdisk): prevent integer overflow in bounds check (offset+count wrap) | 2580b15
- fix(ext2): prevent integer overflow in read bounds check | 2580b15
- fix(msgqueue): add NULL validation to mq_send, mq_recv, mq_trysend, mq_tryrecv | b05e8d9
- fix(ramdisk): add NULL buf check in ramdisk_vfs_read | 42b11f7
- fix(vfs): add NULL name check in vfs_register_device | 42b11f7
- Tests: 29/29 passing (was 29/29), 0 warnings (was 2)

## Sesion 2026-03-29 (heartbeat pass 2)
- security(repo): add .gitignore for build artifacts, editor files, and secrets | 113fe45
- refactor(pic): remove unused mask1/mask2 variables in pic_init (dead code) | 9d21178
- refactor(idt): move mid-file #includes to standard include block at top | 7f7bd8a
- Tests: 29/29 passing (unchanged)

## Completado (anterior)
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
- [2026-03-28] | 6 HIGH bugs fixed (kmalloc overflow, TLB timeout, LAPIC timeout, mutex PI, PMM ref lock, ext2 bounds) | Commit 857c7ce
- [2026-03-28] | 3 MEDIUM bugs fixed (xHCI timeouts, kmalloc cast validation, watchdog enforcement) | Commit 592ed75
- [2026-03-28] | 12 quality fixes (TCB leak, task_join TOCTOU, VMM locks x3, quarantine race, poison lock, memmove SIMD, FB assert) | Commit fec0ee6

## Pendiente
- SMP AP startup (update active_cpus for TLB shootdown) | Prioridad: media
- ext2 indirect block support (single/double/triple indirect) | Prioridad: baja
- xHCI full initialization (device context, command/event rings, transfers) | Prioridad: baja
- USB HID protocol (keyboard/mouse over USB) | Prioridad: baja
- AHCI driver for real disk ext2 | Prioridad: baja
