# ALZE OS — IRQ / HAL / Drivers Review

Reviewer: senior kernel reviewer (interrupts, HAL, drivers)
Date: 2026-04-21
Tree: `/root/repos/alze-os`

## Scope

| File | LOC | Role |
|---|---:|---|
| `kernel/pic.c` / `pic.h` | 235 / 118 | 8259A remap + PIT (tickless) + dynamic `irq_register` table |
| `kernel/lapic.c` / `lapic.h` | 388 / 182 | LAPIC MMIO, PIT-based calibration, periodic timer, IPI |
| `kernel/hal.c` | 56 | HAL glue: registers PIT+PIC as default backends |
| `kernel/hal_irq.h` / `hal_timer.h` | 56 / 53 | Polymorphic ops structs (`init/eoi/mask/unmask`, `init/get_ticks/tick_handler`) |
| `kernel/io.h` | 48 | `inb/outb/inw/outw/inl/outl` + `io_wait` |
| `kernel/interrupts.asm` | 263 | ISR stubs, `irq_common`, dedicated stubs 253/254 |
| `kernel/idt.c` | 221 | Gate install + `exception_handler_c` + `irq_handler_c` |
| `kernel/kb.c` / `kb.h` | 246 / 67 | PS/2 Set-1, ring buf 128 |
| `kernel/uart.c` / `uart.h` | 209 / 54 | COM1 16550, polled+IRQ, 256-byte pow2 ring |
| `kernel/pci.c` / `pci.h` | 196 / 176 | Mechanism-1 CF8/CFC scan, capability walk |
| `kernel/xhci.c` / `xhci.h` | 224 / 72 | Detection only: find, reset, PORTSC enum |
| `kernel/panic.c` / `panic.h` | 150 / 26 | klog dump + CR regs + RBP unwind |
| `kernel/watchdog.c` / `watchdog.h` | 55 / 37 | Soft/hard task stuck detector |
| `kernel/ssp.c` | 56 | `__stack_chk_guard` seeded from RDTSC |
| `kernel/ringbuf.h` | 98 | SPSC ring, pow2, `__sync_synchronize` |
| `kernel/kref.h` | 45 | atomic_t refcount + release cb |
| `kernel/list.h` | 83 | intrusive doubly-linked w/ sentinel |
| `kernel/kevent.h` | 80 | signaled+waitqueue, auto/manual reset |
| `kernel/msgqueue.c` / `msgqueue.h` | 137 / 72 | blocking MQ, send/recv waitqueues |

Total reviewed: ~3147 LOC.

## IRQ dispatch

Primary model today is **PIC-only**. PIT calibrates LAPIC at boot, but LAPIC timer is never armed from `init_devices` (no call to `lapic_timer_init`) — the tick source remains IRQ0 from the 8259A. LAPIC is only used for EOI on two non-PIC vectors (253 timer, 254 TLB IPI).

Remap follows the standard: master → 0x20, slave → 0x28 (`pic.h:22-23`), matching `IRQ_VECTOR(n) = 0x20+n`. Vectors 32 and 33 are installed; vector 34-47 are **not installed** in the IDT — a spurious IRQ7/IRQ15 would vector to an all-zero gate and triple-fault.

`irq_handler_c` (`idt.c:145-177`) has a hand-rolled switch for `IRQ_TIMER`/`IRQ_KEYBOARD` and delegates the rest to `irq_dispatch` (`pic.c:220-224`), which indexes `irq_handlers[16]` set by `irq_register`. No shared lines, no IRQ-threaded bottom half — everything runs top-half in the ISR with IF=0.

EOI ordering in the timer path is correct and explicitly commented: `pic_eoi` is sent **before** `schedule()` (`idt.c:149-166`) because a context switch never returns through this frame; otherwise the PIC would latch forever. Keyboard/default paths EOI at line 176.

Spurious IRQ handling: **absent on PIC side**. The canonical 8259 behavior — if IRQ7/IRQ15 is spurious, only the slave needs EOI for IRQ15 and nothing for IRQ7 — is not implemented; `pic_eoi(15)` unconditionally EOIs both (`pic.c:72-77`), which produces a phantom IRQ on the master. LAPIC has a SVR spurious vector 0xFF (`lapic.h:105`) but no ISR stub is installed at 255, so a spurious LAPIC IRQ would crash.

No NMI path. Bit 7 of port 0x70 is never cleared; NMIs are routed to exception vector 2 which is not in the IDT.

## LAPIC

Calibration (`lapic.c:144-210`) uses **PIT channel 2** via the speaker gate (port 0x61) in mode 0 for ~10 ms. Strategy: arm LAPIC timer with `0xFFFFFFFF` initial count, wait for PIT ch2 OUT bit, read CCR, scale. Fallback on PIT timeout uses `LAPIC_FALLBACK_TICKS = 100000` (`lapic.c:45`), which is a **silent lie** — calibration reports success but the scheduler would then tick at a wrong frequency. This should `lapic_enabled = 0` and refuse to arm the timer.

TSC-deadline is defined (`LAPIC_TIMER_TSC_DEADLINE` at `lapic.h:88`) but unused; only periodic mode is implemented. No CPUID probe for `0x01:ECX.bit24` before selecting it.

IPI infrastructure: `lapic_send_ipi` (point-to-point) and `lapic_send_ipi_all` (all-excluding-self) at `lapic.c:291-323`. `lapic_wait_icr_idle` uses `pause` in a non-bounded loop — if a write-back machine-check leaves the ICR pending, the sender hangs. Delivery mode is hardcoded to Fixed+Physical+Edge+Assert. No shootdown/reschedule/wakeup/halt IPI vectors are plumbed here — only vector 0xFE (TLB shootdown) is wired in `idt.c:214`. No INIT-SIPI-SIPI helper for AP bring-up.

Only **xAPIC MMIO** is supported. `LAPIC_BASE_PHYS = 0xFEE00000` is hardcoded (`lapic.h:24`); if BIOS relocated the base, `lapic_check_msr` warns and ignores it (`lapic.c:122-126`), using the hardcoded HHDM mapping anyway — that is a bug on machines where the MSR returns a different base. x2APIC mode is absent.

## HAL abstraction

Two very thin ops vtables (`hal_irq_ops`, `hal_timer_ops`) with `name/init/eoi/mask/unmask` and `name/frequency_hz/init/get_ticks/tick_handler`. Registration replaces the pointer globally (`hal.c:40-48`). Good idea; execution is **leaky**:

- `idt.c:145-177` calls `pic_eoi` directly, not `hal_irq_eoi`. `kb_init` (`kb.c:158`) calls `pic_unmask` directly. When LAPIC/IOAPIC replaces `hal_irq`, none of these sites will honor it.
- `hal_timer->init` is `uint32_t freq_hz`, but the struct stores `.frequency_hz = 100` as a literal — if someone calls `hal_timer_init(1000)`, the field still reads 100. `pit_init` actually gets called via `main.c:224` directly, bypassing the HAL entirely.
- Timer-tick wiring from `irq_handler_c` hardcodes `pit_tick()` (`idt.c:150`), not `hal_timer_tick()`.

Net: the HAL exists as a table but only ~30% of call sites route through it. The boundary should be all driver-agnostic consumers calling `hal_*`, with PIC/PIT as the sole direct callers — that is not the case today.

## Port I/O + MMIO

`io.h` wrappers use correct `"Nd"` constraint and `asm volatile`, good. `io_wait` writes to port 0x80 (standard ~1 μs post-write delay).

MMIO discipline is **inconsistent**:

- `lapic_read/write` (`lapic.c:74-84`) cast to `volatile uint32_t*` ✓
- `xhci_op_read32/write32` (`xhci.c:26-37`) also `volatile` ✓
- But `xhci.c:199` casts the whole capability regs block to `volatile struct xhci_cap_regs*` and then reads multi-byte fields directly. Since the struct uses `packed`, the compiler may emit byte-sized loads for unaligned fields; per the xHCI spec, capability registers must be read as aligned 32-bit words. `caps->caplength` is fine (byte, offset 0), `caps->hci_version` is fine (word, offset 2), but `caps->hcsparams1..hccparams2` should be 32-bit MMIO reads — they will be today because they’re aligned, but there is no `READ_ONCE`, and the compiler is free to fold multiple reads.

No memory barriers anywhere around MMIO (`mfence`/`lfence`/`sfence`). LAPIC EOI, PIC outb, xHCI reset all rely on strongly-ordered x86 semantics for port I/O and the compiler’s `volatile`, which is acceptable for UP x86 but fragile across re-orderings in LTO.

Write-combining on framebuffer is not visible in the reviewed files (that lives in `console.c`, out of scope) — but the TODO at `xhci.c:196` explicitly flags that BAR0 is mapped via HHDM without `PTE_PCD | PTE_PWT`. On real hardware this can cache MMIO reads → reset handshake hangs.

## Drivers

### kb (PS/2 — `kb.c`)
- Init order: `kb_init` flushes OBF, resets modifier state, unmasks IRQ1 directly via `pic_unmask` (leaky HAL).
- Interrupt registration: **none** — IRQ1 is hardcoded in `idt.c:168-170` as a switch case. `kb_irq_handler` receives nothing, reads port 0x60 blind.
- Ring buf: 128 bytes, indexed by `uint8_t`, so modulo is implicit at 256 — **mismatch with `KB_BUF_SIZE=128`**, modulo is done via `% KB_BUF_SIZE` (`kb.c:104, 236`), so that’s correct but the index type should be `uint32_t` for clarity.
- No debouncing/locking between IRQ producer and `kb_getchar` consumer; on SMP this is racey (no atomics, no barriers).
- No error: `kb_init` ignores controller self-test, no self-test command 0xAA.

### uart (COM1 — `uart.c`)
- Detects hardware with scratch-register ping (`uart.c:108-112`) — good.
- RX buffer is 256 bytes, power-of-2, indices are `volatile uint32_t` with mask-wrap — correct SPSC idiom, but no memory barriers.
- `uart_enable_irq` flips `irq_mode = 1` and sets IER, but **never registers an IRQ handler via `irq_register`** and **never unmasks IRQ4**. The IRQ handler exists (`uart_irq_handler`) but is never wired — unreachable code today.
- TX `uart_putc` has a timeout (`uart.c:149-152`) ✓, but `uart_puts` calls it in a loop with no error propagation; on missing THR, output is silently truncated.
- `uart_put_hex` reimplements nibble printing — fine.

### pci (`pci.c`)
- Scan covers bus 0..255, dev 0..31, fn 0..7 (~65k probes @ 4 I/O each). On real hw with no `MCFG`/ECAM fast path this can be 10+ seconds during boot.
- `PCI_VENDOR_INVALID = 0xFFFF` is checked (`pci.c:76`) ✓ — handles "no device" case cleanly. However the callback skips **without reading header type first**, so if a single-function device happens to be at fn=0 and later functions return 0xFFFF, that’s correct; but when fn=0 vendor is 0xFFFF the loop `break` still assumes single-function — correct.
- Capability walk is bounded (`PCI_CAP_MAX_WALK=48`) — good.
- `pci_read16`/`pci_read8` do byte/word-in-dword alignment correctly.
- BAR sizing (write 0xFFFFFFFF to BAR, read back for size mask) is **not implemented** — `xhci_init` just reads `pci_dev.bar[0]` as-is, so it will miss prefetchable/size info. For a minimal detection driver this is acceptable.

### xhci (`xhci.c`)
- Initialization: finds class 0C/03/30, maps BAR0 via HHDM, does the USBCMD RUN=0 → HCH wait → HCRST → CNR wait sequence. Timeouts are explicit (`XHCI_TIMEOUT_HALT/RESET/READY` at `xhci.c:49-51`) — lesson from ERRORES.md already applied.
- **No command ring, no event ring, no DCBAAP, no slot context, no doorbell** — this is strictly port enumeration. That is fine as documented, but three concerns:
  1. `xhci.c:199-212` reads cap regs BEFORE `xhci_reset()`. The xHCI spec says cap regs are readable any time (RO), so that is OK. But `xhci_cap_length` is stored in a global and then used to offset operational regs — if `xhci_init` is called twice, the second pass uses the already-cached value. No re-entry guard.
  2. `xhci_enumerate_ports` reads PORTSC right after reset. A just-reset controller has PORTSC with CSC/PEC bits set but USB3 ports default to RxDetect; the driver may see "no device" on a connected SuperSpeed port if PLS hasn’t settled. No port-reset sequence.
  3. Bus master + memory space are set (`xhci.c:180-185`) but the reviewer would want a prior READ of `PCI_COMMAND` to also clear INTx disable when MSI is eventually used.
- No IRQ hookup (MSI cap offset is captured in `pci_device` but unused).
- The TODO at `xhci.c:196` flagging missing PCD|PWT on the MMIO mapping is the biggest latent risk — works in QEMU TCG, will wedge on KVM/real hw once caches get involved.

## Panic + watchdog + SSP

Panic (`panic.c`) disables IF, captures CR0/CR2/CR3/CR4 + RSP/RBP/RFLAGS, dumps klog first (so if kprintf later faults, the klog is already flushed), then prints banner + regs + RBP-chain (`panic_stack_trace`) with 16-frame cap, then 8 qwords of raw stack, then `hlt`. Good.

Risks in the panic path:
- `klog_dump()` is called first (`panic.c:93`). If klog uses a spinlock and the panic happened while that spinlock was held by the current CPU, this **self-deadlocks**. `klog_dump` ought to be lock-skipping in panic context.
- `kprintf` likely takes the console lock. Same class of problem: a panic from a code path that already holds the console lock will spin forever.
- `panic_stack_trace` validates frame pointers only by address range (`is_kernel_addr`, `0xFFFF800000000000`..`-1`). A corrupt frame with any canonical kernel address could still deref into unmapped memory and page-fault inside the panic — at which point `exception_handler_c` prints another banner and `cli;hlt`s, so we don’t triple-fault, but the original panic reason is obscured.
- Control registers are captured AFTER `cli` but BEFORE any output — order is fine. However RFLAGS is read after `cli`, so IF will always print 0 in the panic output (not the pre-panic value) — misleading.

Watchdog (`watchdog.c`) is **not** a hardware watchdog (no port 0xCF9 or TCO). It reads `task_current()->watchdog_ticks` in `watchdog_check` called from `irq_handler_c` at timer tick. Soft (500 ticks=5s) warns, hard (1000=10s) resets the counter and expects `sched_tick` to do the kill. The soft warning condition `>= SOFT && < SOFT+1` is a 1-tick window — OK as a "fire once at crossing" but fragile; a single missed tick silently skips the warning.

SSP (`ssp.c`): `__stack_chk_guard` is a **single global** seeded once at boot from RDTSC xorshift + forced zero LSB. This catches string overflows in Ring 0 (the guaranteed null byte) but:
- **Canary is not re-seeded per task.** All tasks share the same guard; once an attacker (or bug) learns it from any task, every other task is predictable.
- No TLS/GS-based canary, so the compiler uses the linker symbol, which means `-mstack-protector-guard=global`. On SMP this is fine because it’s read-only after `ssp_init`, but the entropy is ≤64 bits tied to boot-time TSC.
- `__stack_chk_fail` → PANIC, which is correct.

## kref / list / ringbuf / kevent / msgqueue

- **kref** (`kref.h`): `atomic_t` with `__atomic_*(SEQ_CST)`. `kref_init` sets 1, `kref_get` increments, `kref_put` decrements and calls release at 0. Semantics are correct. Missing: `kref_get_unless_zero` — without it, there is no race-safe upgrade of a weak reference found via a list, which is a common kernel pattern.
- **list** (`list.h`): Sentinel-based, symmetric `prev/next`. `list_remove_node` both unlinks and **self-links** (`node->next = node; node->prev = node`) — so double-remove is safe (it becomes a no-op), matching Linux `list_del_init` semantics. No `list_empty_not_head` idiom though. Macros `list_for_each_safe` are correct.
- **ringbuf** (`ringbuf.h`): SPSC, power-of-2, index type `uint32_t` with mask-wrap. `__sync_synchronize()` as a full barrier after producer writes and before consumer advances tail — safe but **over-strong on x86** (`sfence`/`lfence` or just `__atomic_store(release)` would be enough). NOT MPMC-safe — producer and consumer must each be pinned to one writer/reader.
- **kevent** (`kevent.h`): Spinlock-protected `signaled` + waitqueue. Manual vs auto reset is correct. `kevent_wait` drops the lock before `wq_wait` — but between unlock and `wq_wait` the event can be signaled and lost for auto-reset unless `wq_wait` re-checks; not visible here (depends on `waitqueue.c`, out of scope). **Likely lost-wakeup race** on the drop-lock/block gap if `wq_wait` doesn’t use a prepare-to-wait idiom.
- **msgqueue** (`msgqueue.c`): Blocking send/recv, non-blocking trysend/tryrecv. The sleep path (`mq_send:63-69`, `mq_recv:91-97`) sets `TASK_SLEEPING`, pushes onto the wq list, drops the lock, and calls `schedule()`. Same lost-wakeup class as kevent: between the state store and `schedule()`, a producer can signal `recv_wq`, wake "no-one" (because the task is not yet on the list when lock is dropped — wait, it IS pushed before drop, so that’s OK). But if `schedule()` executes before the wakeup reaches the wq, fine. The actual bug: `TASK_SLEEPING` is written under `mq->lock` but `sched_lock` is not held — a concurrent `wq_wake_one` running on another CPU could observe the task on the wq before the state is visible, racing with scheduler internal state. Needs barrier or unified lock.

## Highlights

- HAL skeleton exists with clear intent (`hal.c`, `hal_*.h`) and explicit design notes citing Linux `irq_chip` / macOS `IOInterruptController`.
- LAPIC calibration uses PIT ch2 (not ch0), so it does not disturb the running scheduler tick — clever, and with a bounded timeout (per the ERRORES.md fix).
- Timer EOI-before-schedule trap is called out explicitly in `idt.c:156-161` — good comment, prevents the classic "PIT dies after first context switch".
- xHCI reset uses bounded spin-waits with error logs (ERRORES.md fix applied).
- `pci_find_capability` has an infinite-loop guard (`PCI_CAP_MAX_WALK=48`).
- list_remove_node is self-re-linking → double-remove safe.
- Panic flushes klog BEFORE printing (post-mortem survives further faults).
- SSP canary masked to include a null byte (catches strcpy-style overflows).

## Issues found

P0:

- `idt.c:208-214` — **IDT has only vectors 32, 33, 253, 254 installed among IRQs.** Any IRQ2-IRQ15 that fires (cascade, spurious IRQ7/15, LPT, RTC) hits a zero gate → triple fault. Fix: install `isr_stub_N` for all 16 IRQs or at minimum add a common spurious stub at 39/47 and 255.
- `lapic.h:105` + `lapic.c:242` — **LAPIC spurious vector 0xFF has no ISR stub.** If the SVR fires (and it can when TPR masking races with an in-flight interrupt), CPU triple-faults.
- `pic.c:72-77` — **Spurious IRQ7/IRQ15 not distinguished.** For IRQ15 you must EOI only the slave; for IRQ7 you must NOT EOI at all. Current code EOIs both for any `irq>=8` and always EOIs master — creates phantom master IRQs.
- `xhci.c:187-196` — **BAR0 MMIO mapped via HHDM without `PTE_PCD|PTE_PWT`.** TODO already in source. On KVM/real hw the reset handshake races against cached reads.
- `lapic.c:190-194` — **Calibration fallback on PIT timeout silently continues** with `LAPIC_FALLBACK_TICKS = 100000`. Scheduler would tick at a wrong freq with no warning to downstream consumers. Fix: set `lapic_enabled = 0` and refuse `lapic_timer_init`.

P1:

- `uart.c:130-135` — **`uart_enable_irq` sets IER but never installs an IRQ4 handler and never calls `pic_unmask(4)`.** The IRQ-mode receive path is dead code. Either wire it via `irq_register(4, uart_irq_thunk)` + `pic_unmask(4)` or remove the code and the `irq_mode` branches.
- `idt.c:145-177` — **Timer and keyboard are hardcoded in `irq_handler_c` instead of going through `irq_register`.** Inconsistent with `irq_dispatch` fallback. Pick one model.
- `hal.c:45-48` + callers — **HAL leakage:** `kb.c:158` calls `pic_unmask` directly; `idt.c:150,161,169,176` call `pit_tick`/`pic_eoi` directly. When IOAPIC replaces PIC, these break.
- `lapic.c:93-97` — **`lapic_wait_icr_idle` has no timeout.** Mirrors exactly the class of bug already recorded in ERRORES.md for PIT calibration and TLB shootdown wait.
- `xhci.c:181-185` — `pci_read16(PCI_COMMAND)` returns the raw reg; code ORs in bits but **does not clear PCI_CMD_INTX_DISABLE**. Fine while using legacy INTx that is unused, but when MSI is wired, INTx disable must be asserted.
- `pci.c:75-79` — `pci_read16(PCI_VENDOR_ID)` of a non-existent device returns 0xFFFF — checked. But `pci_read32` for BARs of a device that **does** exist but has no BAR returns all-ones; `xhci.c:157-176` only checks `mmio_phys == 0`, not `== 0xFFFFFFF0`. A 32-bit all-ones BAR would pass `!= 0` and fail at the next MMIO read.
- `panic.c:93` — **`klog_dump()` called from panic may deadlock** if the faulting CPU holds the klog spinlock. Add a `panic_in_progress` flag that all klog/console paths skip locks on.
- `ssp.c:33` — **Single global `__stack_chk_guard`.** Not re-seeded per task. Consider `-mstack-protector-guard=tls` with a per-task seed at `task_create`.

P2:

- `pic.c:210` — `irq_handlers[]` has no locking; registration during boot only is implicit, not enforced.
- `kb.c:97-108` — ring buf head/tail are `uint8_t` not `volatile`; SMP visibility with IRQ producer + task consumer is not guaranteed.
- `ringbuf.h:72,87` — `__sync_synchronize` is a full `mfence` on x86; a release/acquire pair would be cheaper.
- `panic.c:89-90` — RFLAGS captured post-`cli`; IF bit will always read 0, misleading for post-mortem.
- `lapic.c:122-126` — Non-standard LAPIC base ignored (uses hardcoded HHDM mapping). Should remap via `vmm_map_range` at the reported base.
- `xhci.c:22,200` — `xhci_base` and `xhci_cap_length` globals, no re-entry guard.
- `msgqueue.c:64-68, 92-96` — state store + wq enqueue + schedule may race on SMP without memory barrier between them.
- `kevent.h:61-66` — lost-wakeup window between `spin_unlock` and `wq_wait` (depends on wq API, not visible here).

## Recommendations

1. **Install all 16 IRQ stubs** + a common `isr_spurious` at 39, 47, and 255; always log + EOI-skip.
2. **Route every IRQ through `hal_irq_*`** and every timer read through `hal_timer_*`. Move `pit_tick`+`pic_eoi` out of `idt.c` into a registered handler; then LAPIC/IOAPIC swap is literally one `hal_irq_register(&ioapic_ops)` call.
3. **Replace all unbounded spin loops** with the pattern already in ERRORES.md: bounded counter + log + return -1. Apply to `lapic_wait_icr_idle` (`lapic.c:93`).
4. **Harden calibration fallback**: on PIT timeout, mark `lapic_enabled = 0`; force callers to keep using the PIT path.
5. **Fix MMIO mapping flags** for xHCI BAR0 (PCD|PWT). Either extend `vmm_map_range_mmio` or map explicitly in `xhci_init`.
6. **Panic lock-skip flag**: set `atomic_store(panic_in_progress, 1)` at top of `kernel_panic`; all spinlock acquire paths short-circuit.
7. **Per-task SSP**: seed a canary at `task_create` and use TLS-based guard (GS-relative).
8. **Wire UART IRQ4** via `irq_register` and `pic_unmask(4)` to activate the buffered-receive code that is currently dead.
9. **Spurious IRQ7/15 handling**: read PIC ISR (OCW3=0x0B) before EOI to distinguish real vs spurious.
10. **xHCI next step**: even before command/event rings, a correct port-reset sequence (write PR=1, wait PRC, clear change bits) will dramatically improve device enumeration accuracy.

## Risk zones

- **Spurious IRQ storm**: today any IRQ ≥2 (RTC, CMOS, ATA legacy) fires → triple fault. QEMU masking hides it. Will bite on first real hardware boot.
- **Device that NACKs forever**: `lapic_wait_icr_idle` — unbounded. Any xAPIC quirk (Westmere bug, delivery stuck) hangs the kernel mid-IPI with no telemetry.
- **Hot-unplug**: PCI scan is one-shot at boot; no rescan, no hotplug interrupt, no detach. A PCIe hot-remove leaves stale `xhci_base` pointing at decommissioned MMIO → MCE on next access.
- **IRQ during panic**: `kernel_panic` does `cli` first so local IRQs are safe, but **NMIs and MCEs can still preempt**. There is no NMI handler installed — a #MC during panic → undefined.
- **Calibration during live tick**: `lapic_calibrate_timer` reprograms port 0x61 (speaker gate). If something else (future audio driver) uses ch2, they fight.
- **Keyboard producer / kb_getchar consumer on SMP**: no memory barrier, no atomics. Scancode tearing is possible.
- **klog self-deadlock in panic** if panic happens under klog or console spinlock.
- **Global stack canary**: one leak anywhere (e.g. uninitialized stack read via `kprintf("%s", ...)`) compromises all tasks.
