# PROGRESS.md — Estado del proyecto

## En curso
- [2026-03-30] | Heartbeat maintenance: VMM/IDT/console/PIC named constants | 100%

## Sesion 2026-03-30 (heartbeat pass 12)
- refactor(vmm): replace magic numbers with named page-offset and table-entry constants | b6aa714
- refactor(idt): use named constants for IRQ vectors and LAPIC/IPI gate entries | df3f145
- refactor(console): define named constants for glyph MSB and tab stop width | 043533c
- refactor(pic): replace magic IRQ count literals with named PIC constants | e9449db
- Build: clean (unchanged), Tests: 29/29 passing (unchanged)

## Sesion 2026-03-30 (heartbeat pass 11)
- refactor(spinlock): replace magic number with named RFLAGS_IF constant | 350ec8b
- refactor(kb): define named constants for PS/2 keyboard controller ports | 823f9c2
- refactor(lapic): derive CALIBRATION_PIT_TICKS from PIT_BASE_FREQ constant | cbdb382
- Build: clean (unchanged), Tests: 29/29 passing (unchanged)

## Sesion 2026-03-30 (heartbeat pass 10)
- refactor(kernel): define named constants for magic numbers in console, uart, pic | 6c94e09
- refactor(ktimer): move TIMER_TICK_MS to ktimer.h and use in watchdog | 85b0dec
- refactor(gdt): replace manual byte loop with memset for TSS zeroing | eab317e
- Build: clean (unchanged), Tests: 29/29 passing (unchanged)

## Sesion 2026-03-30 (heartbeat pass 9)
- refactor(tlb): define TLB_SHOOTDOWN_TIMEOUT constant for busy-wait limit | 2d367bc
- refactor(runtime_tests): define named constants for timer accuracy tolerance bounds | b41d834
- refactor(kmalloc): define SLAB_MAX_SIZE constant instead of magic 2048 | b3ad2bd
- Build: pre-existing failure (missing limine.h dependency), Tests: 29/29 passing (unchanged)

## Sesion 2026-03-29 (heartbeat pass 8)
- refactor(lapic): remove write-only lapic_timer_freq variable (dead code) | 6726f47
- refactor(kmalloc,xhci): remove unused SLAB_MAX_SIZE, POISON_PATTERN, 5 xHCI register defines | 6794e56
- refactor(cpuid): define named constants for CPUID feature bits and field masks | d3570a6
- refactor(pci): define PCI_MAX_BUS/DEVICE/FUNCTION constants for scan loops | ca672c2
- refactor(pic): define PIT_MAX_COUNT constant instead of magic 65535 | f4f0801
- refactor(ktimer): define TIMER_TICK_MS constant instead of magic 10 | 7154988
- refactor(idt): define named constants for page fault vector and error code bits | 94e205c
- refactor(panic): define RFLAGS bit constants for register dump decoding | b7532b2
- Build: clean (unchanged), Tests: 29/29 passing (unchanged)

## Sesion 2026-03-29 (heartbeat pass 7)
- refactor(pic): replace magic numbers with named constants for ICW3/masks/PIT commands | 0c0f4da
- refactor(idt): define IDT_GATE_KERNEL_INTR constant for 0x8E gate attribute | e931f1d
- refactor(lapic): replace magic numbers with named constants for MSR/ports/bit fields | 1253220
- refactor(vmm): define MSR_EFER and EFER_NXE_BIT constants for NX enable | 4a8d368
- refactor(xhci): replace magic numbers with named constants for BAR/PCI command bits | 7a8e8f7
- refactor(pci): define PCI_HEADER_MULTIFUNC constant for multi-function bit check | 3d00d49
- refactor(watchdog): remove write-only watchdog_warnings counter (dead code) | b9e42cf
- Build: clean (unchanged), Tests: 29/29 passing (unchanged)

## Sesion 2026-03-29 (heartbeat pass 6)
- fix(lapic): use 64-bit arithmetic to prevent overflow in timer calibration | 93283ff
- refactor(ext2): replace magic numbers 1024/128 with EXT2_BASE_BLOCK_SIZE/EXT2_REV0_INODE_SIZE | 2991263
- refactor(gdt): define named constants for temporary TSS stack sizes | 6985b63
- Build: clean (unchanged), Tests: 29/29 passing (unchanged)

## Sesion 2026-03-29 (heartbeat pass 5)
- refactor(console): move mid-file #include "font8x16.h" to top include block | fe25742
- refactor(kernel): remove unused #include headers in main.c and xhci.c | f693651
- fix(console): use exact COL_BG color when clearing scrolled line | e6d0e3b
- fix(kprintf): avoid signed integer overflow UB when printing INT64_MIN | 9cd6c68
- fix(ext2): validate s_log_block_size to prevent shift overflow | e4815af
- Build: clean (unchanged), Tests: 29/29 passing (unchanged)

## Sesion 2026-03-29 (heartbeat pass 4)
- refactor(kernel): move mid-file #includes to top in kprintf.c, main.c, tests.c | 284539d
- fix(ext2): remove redundant superblock size check in ext2_init | 26b49ea
- Build: clean (unchanged), Tests: 29/29 passing (unchanged)

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
