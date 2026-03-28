# ==============================================================================
# Anykernel OS v2.1 — Build System
#
# Cross-compiles the kernel using clang targeting x86_64-elf (freestanding).
# Creates a bootable ISO with Limine via xorriso.
#
# Usage (from MSYS2 mingw64 shell):
#   make          — build kernel.elf
#   make iso      — build bootable ISO
#   make run      — launch QEMU with serial output
#   make clean    — remove build artifacts
# ==============================================================================

# ── Toolchain ────────────────────────────────────────────────────
CC      := clang
LD      := ld.lld
NASM    := nasm

# Target triple for freestanding x86_64
TARGET  := x86_64-unknown-none

# ── Compiler flags ───────────────────────────────────────────────
CFLAGS  := --target=$(TARGET)       \
           -ffreestanding           \
           -fstack-protector-strong \
           -fno-pic                 \
           -mno-red-zone            \
           -mno-sse                 \
           -mno-sse2                \
           -mno-mmx                 \
           -mcmodel=kernel          \
           -Wall -Wextra -Werror    \
           -std=gnu11               \
           -O2                      \
           -g                       \
           -Ikernel                 \
           -Ilimine

LDFLAGS := -flavor gnu                 \
           -m elf_x86_64               \
           -nostdlib                    \
           -static                     \
           -T linker.ld                \
           -z max-page-size=0x1000

# ── NASM flags ───────────────────────────────────────────────────
NASMFLAGS := -f elf64 -g -F dwarf

# ── Sources and objects ──────────────────────────────────────────
KERNEL_SRC := kernel/main.c kernel/uart.c kernel/gdt.c kernel/panic.c kernel/kprintf.c kernel/ssp.c kernel/string.c kernel/idt.c kernel/pmm.c kernel/kmalloc.c kernel/selftest.c kernel/vmm.c kernel/pic.c kernel/tests.c kernel/kb.c kernel/console.c kernel/cpuid.c kernel/klog.c kernel/sched.c kernel/waitqueue.c kernel/mutex.c kernel/msgqueue.c kernel/semaphore.c kernel/ktimer.c kernel/workqueue_def.c kernel/vfs.c kernel/devfs.c kernel/vma.c kernel/hal.c kernel/mempressure.c kernel/watchdog.c kernel/runtime_tests.c kernel/percpu.c kernel/cpuidle.c kernel/tlb_shootdown.c kernel/lapic.c kernel/ext2.c kernel/ramdisk.c kernel/pci.c kernel/xhci.c
KERNEL_OBJ := $(patsubst kernel/%.c,build/%.o,$(KERNEL_SRC))

ASM_SRC    := kernel/interrupts.asm kernel/context_switch.asm
ASM_OBJ    := $(patsubst kernel/%.asm,build/%.o,$(ASM_SRC))

ALL_OBJ    := $(KERNEL_OBJ) $(ASM_OBJ)
KERNEL_ELF := build/kernel.elf

# ── ISO configuration ───────────────────────────────────────────
ISO_DIR    := build/iso_root
ISO_FILE   := build/os.iso
LIMINE_DIR := limine

# ── QEMU ─────────────────────────────────────────────────────────
QEMU       := qemu-system-x86_64
QEMU_FLAGS := -serial stdio          \
              -no-reboot             \
              -no-shutdown           \
              -m 128M                \
              -cdrom $(ISO_FILE)

# ==============================================================================
# Targets
# ==============================================================================

.PHONY: all iso run clean test test-userspace

all: $(KERNEL_ELF)

# ── Compile C sources ────────────────────────────────────────────
build/%.o: kernel/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

# ── Assemble NASM sources ────────────────────────────────────────
build/%.o: kernel/%.asm | build
	$(NASM) $(NASMFLAGS) $< -o $@

# ── Link kernel ELF ──────────────────────────────────────────────
$(KERNEL_ELF): $(ALL_OBJ) linker.ld | build
	$(LD) $(LDFLAGS) $(ALL_OBJ) -o $@

# ── Build bootable ISO ───────────────────────────────────────────
iso: $(KERNEL_ELF)
	@mkdir -p $(ISO_DIR)/boot/limine $(ISO_DIR)/EFI/BOOT
	cp $(KERNEL_ELF)                     $(ISO_DIR)/boot/kernel.elf
	cp limine.conf                       $(ISO_DIR)/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys     $(ISO_DIR)/boot/limine/limine-bios.sys
	cp $(LIMINE_DIR)/limine-bios-cd.bin  $(ISO_DIR)/boot/limine/limine-bios-cd.bin
	cp $(LIMINE_DIR)/BOOTX64.EFI        $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI
	xorriso -as mkisofs                                                  \
	    -b boot/limine/limine-bios-cd.bin                                \
	    -no-emul-boot -boot-load-size 4 -boot-info-table                 \
	    --efi-boot EFI/BOOT/BOOTX64.EFI                                  \
	    -efi-boot-part --efi-boot-image                                  \
	    --protective-msdos-label                                         \
	    $(ISO_DIR) -o $(ISO_FILE)
	@echo ""
	@echo "=== ISO created: $(ISO_FILE) ==="

# ── Run in QEMU ──────────────────────────────────────────────────
run: iso
	$(QEMU) $(QEMU_FLAGS)

# ── Build directory ──────────────────────────────────────────────
build:
	@mkdir -p build

# ── Clean ────────────────────────────────────────────────────────
clean:
	rm -rf build

# ── Automated test: build, run QEMU, check for failures ─────────
test: iso
	@echo "=== Running kernel self-tests in QEMU ==="
	@rm -f build/serial.log
	$(QEMU) -serial file:build/serial.log -display none \
	        -cdrom $(ISO_FILE) -no-reboot -m 128M &
	@sleep 8 && kill %1 2>/dev/null || true
	@echo ""
	@echo "=== Serial output ==="
	@cat build/serial.log
	@echo ""
	@if grep -q '\[FAIL\]' build/serial.log; then \
	    echo "❌ TESTS FAILED"; exit 1; \
	elif grep -q '0 failures' build/serial.log; then \
	    echo "✅ ALL TESTS PASSED"; \
	else \
	    echo "⚠️  Could not determine test result"; exit 1; \
	fi

# ── Userspace PMM tests ──────────────────────────────────────────
test-userspace: | build
	gcc -DPMM_USERSPACE_TEST -O2 -Wall -Wno-format \
	    -o build/test_pmm tests/test_pmm.c kernel/pmm.c
	./build/test_pmm
