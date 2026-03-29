# Anykernel OS v2.1

Custom x86_64 higher-half kernel. Boots via Limine protocol on QEMU.

## Stack

- **Language**: C (gnu11) + x86_64 Assembly (NASM)
- **Toolchain**: Clang + ld.lld + NASM (cross-compile to x86_64-unknown-none)
- **Bootloader**: Limine (BIOS + UEFI)
- **Build**: GNU Make (MSYS2 mingw64 shell)
- **Emulator**: QEMU (qemu-system-x86_64, 128M RAM, serial stdio)
- **ISO**: xorriso

## Build Commands

```bash
make              # build kernel.elf
make iso          # build bootable ISO
make run          # launch QEMU with serial output
make test         # build + run self-tests in QEMU (headless, checks serial log)
make test-userspace  # userspace PMM unit tests (gcc)
make clean        # remove build artifacts
```

## Architecture

- **kernel/**: all kernel sources (~45 .c files, ~90 .h/.c/.asm files total)
- **limine/**: bootloader binaries (limine-bios.sys, BOOTX64.EFI)
- **tests/**: userspace test harnesses (test_pmm.c)
- **linker.ld**: higher-half layout at 0xffffffff80000000
- **limine.conf**: boot configuration

### Kernel Modules

Memory: pmm (buddy allocator + ref counting), vmm (paging + TLB shootdown), kmalloc (slab allocator + quarantine), vma, mempressure
Scheduling: sched (priority bitmap, watchdog), waitqueue, mutex (priority inheritance), semaphore, msgqueue, ktimer, workqueue_def
Filesystem: vfs, devfs, ext2 (read-only), ramdisk
Hardware: gdt, idt, pic, lapic (timer + IPI), uart, kb, console (framebuffer), cpuid, cpuidle, percpu
Bus/Device: pci (mechanism 1 enumeration), xhci (USB 3.x detection)
Core: panic, kprintf, klog, string, ssp (stack protector), hal
Testing: selftest, tests, runtime_tests

## Project Rules

- Code in English, comments in Spanish
- Commits in English: `tipo(scope): descripcion breve`
- Read ERRORES.md before starting any task
- Update PROGRESS.md when a task is completed
- Never mark done without functional verification (tests green)
- Improve existing code before adding new features
- All busy-waits must have timeouts (learned from xHCI/LAPIC bugs)
- All lock-protected data must use consistent locking (learned from VMM/PMM bugs)
- Compiler flags include -Wall -Wextra -Werror — zero warnings policy

## Current State

Kernel boots, runs self-tests, has working memory management (PMM+VMM+slab), preemptive scheduler, basic filesystem (ext2 read-only over ramdisk), PCI enumeration, and xHCI detection. Single-core with SMP infrastructure (TLB shootdown, LAPIC IPI) ready for AP startup. 8 commits on main.
