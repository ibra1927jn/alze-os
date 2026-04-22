# ALZE OS — Boot / Build / Early-Init Review

Reviewer: senior kernel/systems. Date: 2026-04-21. Repo:
`/root/repos/alze-os` (29/29 tests passing, heartbeat pass 23).

## Scope

Full read: `Makefile` (145), `linker.ld` (62), `limine.conf` (5),
`.gitignore` (20), `kernel/main.c` (306), `gdt.c/.h` (198+41),
`idt.c/.h` + `interrupts.asm` (220+19+263), `cpuid.c/.h` + `cpu.h`
(124+14+27), `console.c/.h` + `font8x16.h` (386+39+113), `compiler.h`
(65), `errno.h` (30). Spot-reads: `panic.h/.c`, `ssp.c`, `percpu.c`,
`pic.c`, `lapic.c`, `uart.c`, `vmm.c` (init/audit_wx/find_max_phys),
`memory.h`, `limine/limine.h`, `ERRORES.md`, `PROGRESS.md`. **~2050
LOC in-depth.** Binary checks: `readelf -l/-S/-W`, `nm`, `objdump -d`
on `build/kernel.elf`.

## Build system

clang + ld.lld + nasm, target `x86_64-unknown-none`. Produces
`build/kernel.elf` and a BIOS+UEFI hybrid ISO via xorriso.

CFLAGS (`Makefile:23-37`): `-ffreestanding -fstack-protector-strong
-fno-pic -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mcmodel=kernel
-Wall -Wextra -Werror -std=gnu11 -O2 -g`. All correct: red-zone off
(IST), SSE/MMX off (no FPU save, no 16-byte alignment trap).

LDFLAGS (`Makefile:39-44`): `-nostdlib -static -T linker.ld -z
max-page-size=0x1000`. No `--build-id` (and `/DISCARD/ *(.note*)` at
`linker.ld:59` drops it anyway — intentional but costs post-mortem
identification). No `-MMD -MP` dep tracking.

`linker.ld`: ENTRY `_start`, base `0xffffffff80000000`, 4 PHDRs
(requests, text, rodata, data), every section `ALIGN(4096)`. Exports
`__kernel_{start,end}` and `__{text,rodata,data,bss}_{start,end}` for
VMM (`vmm.c:30-33, 538`). `readelf -lW` confirms entry
`0xffffffff80001000` and 4 4-KiB-aligned PT_LOADs.

Reproducibility: `-g` embeds paths, no `SOURCE_DATE_EPOCH`, no
`LC_ALL=C`. Not reproducible (normal for in-dev).

Targets: `all`, `iso`, `run`, `test` (QEMU + grep `[FAIL]`),
`test-userspace` (builds `test_pmm` host ELF via gcc). **`make iso`
fails on fresh clone**: copies three Limine binaries not vendored
(only `limine.h` is) — see issue 12.

## Boot flow

`limine.conf`: `protocol: limine`, `kernel_path:
boot():/boot/kernel.elf`. `main.c:59` requests
`LIMINE_BASE_REVISION(3)`; `init_early_hw` KASSERTs acceptance
(`main.c:211`).

Requests are `static volatile` in `.limine_requests*` (`main.c:52-77`):
start/end markers, HHDM, memmap, framebuffer. The marker macros
(`limine/limine.h:55-64`) expand to full `uint64_t arr[]={...};`
definitions — each `static volatile LIMINE_*` line is itself a
complete declaration.

`_start` (`main.c:284`) is plain C; `objdump -d` shows only a
compiler prologue. **No manual stack pivot, no CR3 touch, no GDT/IDT
change before C runs.** Relies on Limine rev-3 guarantees (long mode,
IF=0, 16 KiB stack, responses populated, paging on, loader GDT/IDT).
Valid.

Init order:
1. `init_early_hw`: uart → ssp → cpuid → revision-check → gdt → idt →
   `percpu_init_bsp` (GS_BASE) → pic → pit → unmask IRQ0 → `sti`
2. `init_memory`: HHDM offset → memmap dump → pmm → vmm (builds own
   PML4, switches CR3, per-section W^X, guard-page stacks) → vma →
   hal → cpuidle → `vmm_audit_wx()`
3. `init_devices`: framebuffer console → kb → vfs → devfs →
   mempressure → watchdog → **lapic → tlb_shootdown** → ramdisk →
   pci → xhci
4. selftests → boot report → `sched_init` → runtime tests → `idle_loop`

`percpu_init_bsp()` correctly precedes `sti` (`main.c:221`):
`sched_tick` reads `%gs:` so GS_BASE must be set first.

Silent-fault windows:
- `kprintf → uart_putc` is gated by `uart_available`
  (`uart.c:106-115`). Any panic before `uart_init` returns produces
  zero output. Window = `_start` prologue + `uart_init` body.
- After `idt_init`, only vectors 0, 6, 7, 8, 13, 14, 16, 32, 33, 253,
  254 are present gates. **Everything else = non-present → #GP →
  #DF → triple fault with no diagnostic** (issues 2-4).

## GDT / IDT / CPU

**GDT** (`gdt.c`, 7 entries, `gdt.h:11-17`):
| idx | sel  | role           | access | flags |
|-----|------|----------------|--------|-------|
| 0   | 0x00 | null           | 0      | 0     |
| 1   | 0x08 | kernel code 64 | 0x9A   | L=1   |
| 2   | 0x10 | kernel data    | 0x92   | 0     |
| 3   | 0x18 | user code 64   | 0xFA   | L=1   |
| 4   | 0x20 | user data      | 0xF2   | 0     |
| 5/6 | 0x28 | TSS (16-byte)  | 0x89   | —     |

CS reloaded via `pushq/lretq` (`gdt.c:170-185`); DS/ES/SS loaded with
data selector; FS/GS zeroed. TSS loaded via `ltr` (`gdt.c:189`).
`tss.iopb_offset = sizeof(tss)` disables the I/O bitmap.

**TSS stacks** (`gdt.c:45-48`): `temp_rsp0_stack[4096]`,
`temp_ist1_stack[2048]`, 16-byte aligned. Replaced by guard-page
stacks in `vmm_setup_kernel_stacks()` after CR3 switch
(`tss_set_rsp0` / `tss_set_ist1`, `gdt.c:192-198`). Only IST1
populated; IST2-7 = 0.

**IDT** (`idt.c`, 256 entries). `idt_set_gate` always writes
`type_attr = 0x8E` → 64-bit **interrupt gate**, P=1, DPL=0. No trap
gates, no DPL=3 (no syscall yet; Sprint-4 roadmap per `vmm.c:547`).
Installed vectors: 0/6/7/8(IST1)/13/14/16, IRQ0→0x20, IRQ1→0x21,
LAPIC timer 0xFD, TLB IPI 0xFE. **Nothing at 0xFF (LAPIC spurious),
0x27/0x2F (PIC spurious), NMI/#DB/#BP/#OF/#BR/#TS/#NP/#SS/#AC/#MC/
#XM/#VE/#CP** — see issues 2-4.

**Stubs** (`interrupts.asm`): macros for error/no-error entry, funnel
into `isr_common` (halts) or `irq_common` (iretq). Frame layout
(`interrupts.asm:14-30`) matches `struct interrupt_frame`
(`idt.c:52-62`). **IRQ0 handler** (`idt.c:145-177`) sends PIC EOI
*before* calling `schedule()` — comment at `idt.c:155-161` documents
this as a previously-learned trap (otherwise the switched-away stack
never reaches the trailing `pic_eoi`). Correct.

No `SYSCALL`/`SYSRET` MSR setup — expected.

## Early console

Discovery: `init_devices` (`main.c:256-263`) reads
`fb_request.response->framebuffers[0]`, asserts `fb != NULL` and
`fb->address != NULL`, calls `console_init(addr, w, h, pitch, bpp)`.

Pixel format: `console.c:271` hard-rejects `bpp != 32`. Writes
`uint32_t` pixels as ARGB α=0; `fb->red_mask_shift` etc. never
inspected → BGR panel = inverted colours. `ppitch = fb_pitch / 4`
(`console.c:84, 98, 123, 288`) assumes pitch divisible by 4, not
asserted.

Drawing: `draw_glyph` iterates 8×16 per glyph bit. `scroll_up`
`memmove`s `fb_pitch * (fb_height - FONT_H)` bytes (optimized
`rep movsq` forward path). Cursor wraps at `cols`, scrolls at `rows`;
`clear_region` clamps correctly. ANSI SGR + cursor-motion parser
(`console.c:132-265`), multi-param CSI (max 8), file-static FSM.

**Locking: none.** Safe today under UP + interrupt-gate IDT (IF=0 on
entry, no same-CPU ISR re-entry). The moment a second CPU is online
(`lapic_init` / `tlb_shootdown_init` compiled in at `main.c:273-274`),
concurrent access to `cursor_x/_y`, `ansi_state`, `ansi_params[]`,
and the raw framebuffer will race. SMP-gate.

## Highlights

- Correct freestanding toolchain flags; SSP guard RDTSC-seeded before
  any SSP-protected function unwinds (`ssp.c`, `main.c:206`).
- `_start` delegates to three clearly named phase functions (heartbeat
  pass 21 refactor, per PROGRESS.md). Readable.
- `LIMINE_BASE_REVISION(3)` is KASSERT-verified, not assumed
  (`main.c:211`).
- `#DF` uses IST1 (`idt.c:203`): stack overflow in a normal thread
  lands on a safe stack.
- IRQ0 EOI-before-schedule ordering captured in-comment as a known
  trap (`idt.c:155-161`) — institutional knowledge preserved in code.
- ERRORES.md: one-line file/error/fix columns. Visible descendants:
  fb non-NULL asserts, UART scratch probe, ref_count spinlock, TLB
  IPI EOI, PIT calibration timeout.
- Linker-exported section symbols feed `vmm_apply_section_permissions`
  (`vmm.c:534-538`); `vmm_audit_wx()` runs at end of `init_memory`
  (`main.c:250`) as an automated regression gate.
- PIC masks all IRQs before callers selectively unmask
  (`pic.c:68-69`).

## Issues found

1. **`linker.ld:6-9` PT_LOAD flag bits inverted.** ELF p_flags are
   PF_X=1, PF_W=2, PF_R=4. The script treats bit 0 as "readable":
   `rodata (1<<0)` = 1 = **X only** (comment says "R--");
   `data (1<<0)|(1<<1)` = 3 = **W|X** (comment says "RW-"); text/
   requests happen to come out right by coincidence. `readelf -lW`
   confirms: rodata PHDR `E`, data PHDR `WE`. Runtime masked because
   VMM rebuilds mappings with correct flags and `vmm_audit_wx` passes,
   but ELF tools trusting PHDR flags see a W+X .data. Fix: define
   `PF_X=1 PF_W=2 PF_R=4` at top of script and use names.

2. **No IDT gate for LAPIC spurious (0xFF)** even though
   `lapic.c:242` programs SVR with vector 0xFF. On real HW a spurious
   → non-present gate → #GP → #DF → triple fault. Add trivial
   `isr_stub_255: iretq` and `idt_set_gate(255, isr_stub_255, 0)`.

3. **No IDT gates for vectors 1-5, 9-12, 15, 17-21** (includes NMI=2,
   #MC=18 machine check, #XM=19 SSE fault, #VE=20, #CP=21). Any of
   these firing = triple fault with zero diagnostic. Highest
   real-world risk on bare metal: **#MC** from ECC / bus errors →
   silent reboot that looks like "bad kernel". Fix: install a generic
   stub table for 0-31 that funnels through `exception_handler_c` so
   at least the vector number is printed.

4. **No PIC spurious gates (0x27 master, 0x2F slave).** Master-side
   spurious needs to NOT send EOI to master; slave-side spurious
   needs EOI only to master. Add stubs accordingly. Not exploitable
   today (IRQ7/IRQ15 are masked in `pic_init`) but real HW can still
   emit a latched spurious IRQ7 from bus glitches.

5. **Console has zero locking** (entire `console.c`). SMP-unsafe on
   `cursor_x/_y`, `ansi_state`, `ansi_params[]`, and raw framebuffer
   writes. Given LAPIC and TLB-IPI are already wired, the moment a
   second CPU logs, state corrupts. Add `spin_lock_irqsave` around
   `console_putchar` + `scroll_up`, or a per-CPU ring buffer drained
   by a single writer.

6. **`console_init` does not assert `(fb_pitch & 3) == 0`**
   (`console.c:269-284`). All row arithmetic uses `fb_pitch / 4`; a
   non-multiple-of-4 pitch skews every scanline silently.

7. **Pixel format hard-coded XRGB32**. `console.c:271` only checks
   `bpp == 32`. Add a check that `red_mask_shift == 16 &&
   blue_mask_shift == 0` (or similar) — otherwise BGR panels show
   inverted colours with no warning.

8. **`temp_ist1_stack` is 2 KiB** (`gdt.c:47`). A #DF before
   `vmm_setup_kernel_stacks()` runs (i.e. during `init_early_hw` or
   early `init_memory`) executes on only 2 KiB. `exception_handler_c`
   → `kprintf` (format buffer) → framebuffer path can plausibly push
   past that. Bump to 4 KiB to match RSP0.

9. **Panic before `uart_init` is silent.** `uart_available=0` →
   `uart_putc` (`uart.c:145-155`) returns immediately; `console_putchar`
   also a no-op (`fb_ready=0`). A KASSERT inside `uart_init` produces
   zero output. Document in `panic.h` or add a raw-port emergency path.

10. **No `--build-id`**, `.note*` discarded (`linker.ld:59`). Crash
    logs cannot be matched to a specific binary. Fix: `-Wl,--build-id
    =sha1` + `KEEP(*(.note.gnu.build-id))` and print ID in boot banner.

11. **`print_memory_map` accounting mismatch** (`main.c:106-120`):
    `usable_bytes += e->length` adds unaligned bytes while `usable_pages`
    counts only page-aligned interior. Printed totals can disagree.
    Cosmetic.

12. **`make iso` fails on fresh clone.** `Makefile:96-99` copies three
    Limine binaries that are not vendored (only `limine.h` is). Vendor
    them or add a `limine:` sub-target.

13. **`kernel_panic` uses `kprintf` heavily** (`panic.c:96-123`). A
    fault inside kprintf (format-string bug, bad va_list) re-enters
    kprintf from panic → re-fault. Add a panic-raw path.

14. **`-MMD -MP` not enabled.** Header changes do not rebuild
    dependent `.o`; incremental builds can ship stale objects.

15. **No NX-support KASSERT at boot.** `vmm.c:enable_nx()` writes
    EFER.NX without first checking CPUID.80000001h:EDX bit 20. A CPU
    without NX would #GP on the `wrmsr`. Add `KASSERT(CPUID_EXT_EDX_NX)`
    in `cpuid_detect` or before `enable_nx`.

## Recommendations

**P1 — correctness / silent-death:**
- Fix `linker.ld` flag-bit naming (issue 1), one-line change.
- Install generic catch-all stubs for vectors 1-31 not already handled
  (issue 3) → converts "silent triple fault" to "PANIC w/ vector".
- Install stubs for 0xFF, 0x27, 0x2F (issues 2, 4).
- KASSERT NX support before writing EFER (issue 15).

**P2 — SMP readiness** (before second CPU boots):
- Add irqsave spinlock to `console_putchar` / `scroll_up` / ANSI FSM.
- Audit other file-static state in the boot path (pit/ktimer/kb) for
  SMP safety — out of scope here, flag for follow-up.

**P3 — hardening:**
- Bump `temp_ist1_stack` to 4 KiB (issue 8).
- Assert `(fb_pitch & 3) == 0` + pixel-format masks (issues 6-7).
- Emit and print build-id (issue 10).
- Panic-raw path bypassing kprintf formatting (issue 13).

**P4 — polish:** `-MMD -MP` (issue 14); vendor/auto-build Limine
binaries (issue 12); doc "panic-before-UART is invisible" in panic.h
(issue 9).

## Risk zones

- **#MC (18)**: ECC / bus error → non-present gate → triple fault.
  Looks identical to "bad CR3" from outside. Highest bare-metal risk.
- **NMI (2)**: IPMI / HW watchdog NMI on a server → same fate.
- **LAPIC spurious (0xFF)**: rare in QEMU, common on real HW when EOI
  timing is tight. Tests pass, HW hangs.
- **PIC spurious IRQ7/15**: rare but real on legacy chipsets.
- **Panic inside `uart_init`**: no UART ready, no framebuffer yet →
  zero output, QEMU just hangs.
- **Headless boot + UART probe fail**: kernel stays alive but runs
  completely blind.
- **First SMP IPI delivery**: unprotected `console_*` / ANSI state
  races → corrupted diagnostics (not a crash, but unreadable).
- **#DF before `vmm_setup_kernel_stacks`** on the 2 KiB IST1 →
  handler stack overflow → triple fault.
- **`base + length` overflow in memmap parsing** (`main.c:107`): not
  exploitable today, no guard.
- **Base rev 3 unsupported by bundled bootloader**: KASSERT at
  `main.c:211` fires, panic prints cleanly via UART, halts. The
  *good* failure mode — contrast with all the silent ones above.
