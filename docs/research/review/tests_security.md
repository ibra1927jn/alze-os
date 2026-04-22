# ALZE OS — Tests & Security Review

Date: 2026-04-21
Reviewer: senior audit pass
Repo: `/root/repos/alze-os` (git HEAD `6101b72`, 144 commits)
Scope boundary: read-only on repo, write only this file.

## Scope

| File | LOC | Purpose |
|------|-----|--------|
| `kernel/tests.c` | 1096 | main in-kernel selftest catalog |
| `kernel/runtime_tests.c` | 144 | post-boot integration/perf suite |
| `kernel/selftest.c` / `.h` | 41 / 40 | micro test runner (name+fn list) |
| `kernel/klog.c` / `.h` | 98 / 49 | 4 KB ring buffer + syslog levels |
| `kernel/log.h` | 50 | ANSI colored `LOG_*` macros |
| `kernel/kprintf.c` / `.h` | 332 / ~30 | freestanding printf + ksnprintf + rate limiter |
| `kernel/panic.c` / `.h` | 150 / 26 | register dump + RBP chain + klog flush |
| `kernel/ssp.c` | 56 | `__stack_chk_guard` + `__stack_chk_fail` |
| `tests/test_pmm.c` | 235 | userspace PMM harness (native gcc build) |
| Kernel total | 13 931 LOC |

Counts (exact, from grep):
- `kernel/tests.c`: 66 `static bool test_*` functions, 66 `selftest_register(...)` calls.
- `kernel/runtime_tests.c`: 5 runtime tests (stress, ctx-switch bench, IPC bench, timer accuracy, leak).
- `tests/test_pmm.c`: 6 test functions (`test_basic`, `_alignment`, `_exhaustion`, `_coalescing`, `_splitting`, `_stress`).
- `PROGRESS.md` claims "Tests: 29/29". `runtime_tests.c:142` prints "41 kernel + 5 runtime". Reality: 66 in-kernel + 5 runtime. **Documentation is stale and understated by more than 2×.**

## Test architecture

- **In-kernel selftests** (`kernel/tests.c`): real kernel code linked into `kernel.elf`, run on bare metal inside QEMU after `init_early_hw/memory/devices` but **before `sched_init`** (`kernel/main.c:290-296`). Each test is `static bool`, registered via `selftest_register(name, fn)`, dispatched by `selftest.c:24-33`. Pass/fail reported as colored `[OK]`/`[FAIL]` lines; aggregate count printed at end.
- **Post-boot runtime tests** (`kernel/runtime_tests.c`): invoked from `main.c:302` *after* `sched_init` so they can `task_create`, `sched_yield`, `task_sleep`, `task_join`, `mq_send/recv`. Produces five log lines; no machine-parseable pass/fail flag beyond `[ERROR]` text.
- **External standalone** (`tests/test_pmm.c`): compiled natively via `make test-userspace` (`Makefile:142-145`) with `-DPMM_USERSPACE_TEST`. It **directly links against the real `kernel/pmm.c`** (not a mock) — the userspace harness allocates a fake `struct page[]`, calls `pmm_init_test`, and hammers the actual buddy allocator. The 21 KB `test_pmm` binary at the repo root is a **stale checked-in build artifact** (not referenced by `Makefile`, not in `.gitignore` — should be removed or added to ignore list).
- **Invocation driver**: `make test` (`Makefile:122-139`) builds the ISO, runs QEMU with `-serial file:build/serial.log` for 8 seconds, then greps for `[FAIL]` or `0 failures`. Timing-based; flaky on slow machines.
- **SELFTEST_MAX=64** (`kernel/selftest.h:15`). `selftest_register` silently returns if full (`selftest.c:13`). With **66 registrations**, the **last 2 tests (Console size reasonable, KB no modifiers at boot) are silently dropped** at runtime. See Issues.

Subsystem coverage tally (by `selftest_register` line):

| Subsystem | # tests | Subsystem LOC | coverage density |
|-----------|---------|---------------|-------------------|
| PMM / buddy | 9 | 667 | strong |
| kmalloc / quarantine | 10 | 478 | strong |
| VMM | 6 | 716 | decent |
| VFS / devfs | 5 | 269+192 | moderate |
| string / mem ops | 6 | — | OK |
| klog | 2 | 98 | OK |
| scheduler / CFS | 2 (vruntime, nice) + 1 runtime stress | 688 | **thin** |
| mempressure | 2 | — | OK |
| primitives (rwlock, kref, ringbuf, bitmap, kevent) | 5 | — | OK |
| console / kb / UART / watchdog | 5 | — | smoke-only |
| drivers (xhci, pci, ramdisk, ext2, ahci) | **0** | 224+196+102+485 | **none** |
| mutex / semaphore / waitqueue / msgqueue | **0** | 114+69+111+137 | **none (only runtime IPC bench exercises mq)** |
| tlb shootdown / lapic / percpu | 1 (percpu only) | 2 ERRORES entries | weak |

## Coverage vs 14K LOC

66 selftests + 5 runtime + 6 userspace PMM = **77 cases over ~14k LOC ≈ 1 test per 180 LOC**, heavily skewed toward MM (25/77 touch PMM/kmalloc/VMM).

Biggest gaps:
1. **0 tests for xhci.c (224 LOC)** — three hardware timeout loops historically bug-heavy (ERRORES 2026-03-28). Only compile-time existence is verified.
2. **0 tests for ext2.c (485 LOC)** — read-only path and write path both untested. `test_dev_zero/random` touches devfs only.
3. **0 tests for mutex.c / semaphore.c / waitqueue.c / msgqueue.c** as unit tests. IPC bench in runtime_tests.c exercises `mq_send/recv` but never asserts ordering, blocking, or wakeup correctness.
4. **0 scheduler unit tests** — `test_vruntime_increases` and `test_nice_default` are trivial property reads. No preemption, priority inheritance, `task_exit`/reap, watchdog-kill-stuck-task, or CFS fairness tests — despite sched.c being the most bug-prone file in ERRORES (6 entries).
5. **0 TLB shootdown tests** — hot area in ERRORES (infinite busy-wait, missing EOI) but zero assertions.
6. **0 PCI enumeration tests** — 196 LOC of bus scan untested.
7. **0 ramdisk / devfs lifecycle tests** — only `/dev/null|zero|random` smoke reads.
8. **0 panic-path tests** — SSP failure path cannot be exercised without crashing the kernel under QEMU; no dedicated fault-injection build target.

## Logging

Four layers exist, and they are almost coherent:
- `kprintf.c:152` — freestanding printf; every char flows to `uart_putc`, `console_putchar`, **and** `klog_putchar` (line 43). Cost: triple-fanout on every char, including the klog spinlock on each byte.
- `klog.c:20` — 4 KB power-of-2 ring, protected by `klog_lock`. On full buffer it drops one byte at a time (tail++) before writing — O(N) where it could O(1). Acceptable given the 4 KB size.
- `log.h` — the canonical frontend: `LOG_INFO/DEBUG/WARN/OK/FAIL/ERROR` macros wrapping `kprintf` with ANSI colors and implicit `\n`. **No severity integer is passed to klog**, so color tags end up in the ring but `klog_set_level` filtering applies only to the explicit `klog_write_level` path (`klog.c:55`), which **is never called from anywhere** in the tree (confirmed by grep). Severity filtering is effectively dead code.
- `ksnprintf` / `kvsnprintf` (`kprintf.c:247-312`) — buffer-sized correctly via `struct snprintf_ctx`, NUL-terminates. Safe.

Severity levels: defined `KLOG_EMERG..KLOG_DEBUG` (klog.h:16-23) but **no LOG_INFO macro emits them**, so `klog_get_level`/`set_level` is purely cosmetic today.

Lock contention: `klog_lock` is taken once per character (`klog.c:37-47`). A single `LOG_INFO("msg")` of 30 chars acquires the spinlock 30×. For a kernel with 50-ms PIT periods this is fine; for bursty logging during `vmm_init`+`pmm_init` this adds measurable overhead. Recommend buffering a full line and writing once.

Ring for late-boot: yes, `klog_dump()` is called from `kernel_panic` at `panic.c:93` before register dump — so the last 4 KB is emitted even if the panic came after serial/FB drowning.

## Panic path

`kernel/panic.c:75-150` `kernel_panic(msg, file, line)`:
1. `cli` first (`:77`).
2. Captures CR0/CR2/CR3/CR4 + RSP/RBP/RFLAGS inline.
3. Calls `klog_dump()` — flushes 4 KB ring to UART for post-mortem.
4. Prints ANSI red banner with msg/file/line.
5. Dumps control regs, RFLAGS with bit-decoded flags.
6. Walks RBP chain up to `PANIC_MAX_FRAMES=16`, bounding by `is_kernel_addr(0xFFFF800000000000..)`.
7. Prints 8 raw qwords from current RSP (`PANIC_STACK_DUMP_ENTRIES=8`).
8. Prints "(see klog dump above for context)" — **no actual current task name / TID / state is dumped**. This is a TODO disguised as a label.
9. Infinite `hlt` loop. **No reboot fallback**, consistent with `-no-reboot` QEMU flag.

Halt vs reboot: halt only. On real hardware a triple-fault escape hatch is absent; if the panic itself faults (e.g. `klog_dump` touches dead state), the CPU simply loops in the same broken `hlt`.

**Panic re-entrancy: NOT guarded.** There is no `in_panic` flag. A fault inside `klog_dump`, `kprintf`, or `panic_stack_trace` would re-enter `kernel_panic` via `KASSERT` or the page-fault handler, leading to infinite recursion and exhausted IST1 stack. This is the single biggest defect on the panic path.

## Stack protection (SSP)

- Makefile uses `-fstack-protector-strong` (`Makefile:25`). Good.
- Canary `__stack_chk_guard` **is defined with a non-zero literal `0x595E9FBD94FDA766`** (`ssp.c:33`). If `ssp_init()` is never reached — e.g. early crash in `uart_init`/`cpuid_detect` — the hardcoded constant protects the kernel from day-zero attackers reading the ELF.
- `ssp_init()` (`ssp.c:36-45`) reseeds the canary from `rdtsc()` with a 3-stage xorshift (17/13/7) and clears the low byte to force at least one NUL (catches strcpy-style overflows). Called from `main.c:206`, **immediately after `uart_init` and before anything else**. Entropy source is TSC alone — weak (TSC at early boot is predictable within ±64 cycles for identical hardware). No mixing with LAPIC, CMOS RTC, or Limine's random boot token. **No per-task canary rotation**: every task shares the same global guard, so a leaked canary from one task compromises all.
- `__stack_chk_fail` (`ssp.c:53-56`) calls `PANIC("Stack Smashing Detected...")`. Noreturn. Reasonable — but see panic re-entrancy note.

## SECURITY_RULES adherence

`SECURITY_RULES.md` (8 rules). Cross-check:
- Rule 1 (no .env / tokens): `gitleaks detect --source .` ran clean ("no leaks found, 144 commits scanned"). `gitleaks protect` also clean. Pass.
- Rule 2 (no invented seed data): N/A for kernel.
- Rule 3 (no prod deploy): N/A.
- Rule 4 (no hardcoded creds): grep of the tree shows 1 match on "password" — it's the regex pattern in SECURITY_RULES.md itself. Pass.
- Rule 5 (no push without clean build): Makefile has `-Werror` (`Makefile:32`). Good. `make test` is available but **`make test` is not a prerequisite of any push hook** and no CI config (`.github/`, `.gitlab-ci.yml`) exists.
- Rule 6 (.env in .gitignore before first commit): `.gitignore:19-20` covers `.env` and `.env.*`. Pass.
- Rule 7 (exposed key detection): no runtime check; relies on manual gitleaks.
- Rule 8 (test data marked TEST): `tests/test_pmm.c` uses `PMM_USERSPACE_TEST` define + `TEST_BASE_PHYS` names. Pass.

`.gitignore` missing patterns: `*.log`, `build/serial.log` (generated by `make test`), `test_pmm` (the 21 KB binary sitting in repo root), `.DS_Store` is covered, `id_rsa*`/`*.pem`/`*.key`/`credentials.json`/`secrets.*` are not listed. Low risk for a pure-kernel repo but worth tightening.

## PROGRESS.md + ERRORES.md discipline

- **PROGRESS.md**: last update 2026-04-06 (`heartbeat pass 23`), 190 lines. 23 heartbeat sessions recorded over ~10 days. Sessions are dense and useful — each lists commits, short commit-message-style descriptions, and `Build: clean` + `Tests: 29/29 passing` footers. **But the "29/29" number has not been updated since the early sessions even though `kernel/tests.c` now has 66 registered tests and the runtime suite prints "41 kernel + 5 runtime".** The maintained discipline is high-frequency but the content has drifted out of sync with reality.
- **ERRORES.md**: last update 2026-03-30 (translation commit). 24 real entries — all dated 2026-03-28 in one burst. Since then: zero new entries despite 20+ heartbeat commits, 3 post-audit bug fixes including `workqueue_def.c` NULL-deref (commit `6101b72`, a textbook ERRORES-worthy bug). **The log has not been written to in 22 days and is no longer a live signal.**
- Error-distribution patterns: `kernel/sched.c` (6 entries, most bug-prone), `vmm.c` and `kmalloc.c` (4 each, locking/bounds), `tlb_shootdown.c` (2, timeout/EOI). Theme: "we keep breaking locking around shared state" (9/24 entries are race/lock fixes) and "we keep forgetting timeouts on busy-waits" (3/24).

## Highlights

- Self-test registration pattern is clean and idiomatic; dispatcher is 20 lines (`selftest.c:19-37`).
- Panic dumps CR0/2/3/4 + RFLAGS decoded + RBP chain — rare on hobby kernels.
- `ksnprintf` bounds-checks the output buffer correctly (`kprintf.c:206-211`).
- Userspace PMM harness avoids mocking: the same `pmm.c` runs under both QEMU and native gcc, via `PMM_USERSPACE_TEST`.
- `-fstack-protector-strong` + `-Wall -Wextra -Werror` at build time. SSP init happens 2nd thing after UART.
- gitleaks history scan (144 commits) clean.

## Issues found

Priority: H=high, M=medium, L=low.

1. **[H] Panic-in-panic unguarded.** `kernel/panic.c:75` has no `static int in_panic` flag; `klog_dump`, `panic_stack_trace`, or a KASSERT inside kprintf would recurse infinitely. Fix: at entry, `if (__atomic_test_and_set(&in_panic)) { asm("cli; hlt; jmp ."); }`.
2. **[H] `SELFTEST_MAX=64` silently drops the last 2 tests.** `kernel/selftest.h:15` + `selftest.c:13`. With 66 registrations, `test_kb_modifiers` and `test_console_size` (last two in `register_selftests`) never execute, but the runner reports the first 64 as "64 passed 0 failed". Users believe 66/66 passed. Fix: raise to 128 or `PANIC` on overflow.
3. **[H] `klog_write_level()` never called.** `kernel/klog.c:55` + `kernel/log.h:32`. The `LOG_*` macros go straight to `kprintf` → `klog_putchar`, bypassing severity. `klog_set_level`/`klog_get_level` exist but only filter a dead path. Two tests (`test_klog_level` at `tests.c:936`, `test_klog_message_count` at `:929`) mock the API but do not prove filtering works end-to-end. Fix: teach `LOG_*` to thread severity through, or delete the dead API.
4. **[H] No current-task info in panic dump.** `kernel/panic.c:141-142` prints "(see klog dump above)" — placeholder. Every other kernel panic dump includes TID, task name, state, kernel stack base. Fix: `struct task *t = task_current(); if (t) kprintf("  TID=%u name=%s state=%d", t->tid, t->name, t->state);`.
5. **[M] Weak SSP entropy + global canary.** `kernel/ssp.c:37` TSC-only seed, called once at boot; `__stack_chk_guard` is a single uintptr_t shared by all tasks. No per-task rotation. Fix: mix TSC with Limine's entropy request (if present) + CMOS seconds; on task_create, reseed guard into the task's TCB and swap on context switch.
6. **[M] Runtime tests have no machine-parseable pass/fail.** `make test` greps for `[FAIL]`, but `runtime_tests.c` uses `LOG_ERROR` for failures (`runtime_tests.c:96,127,137`), which renders `[ERROR]`, not `[FAIL]`. A runtime regression would be scored as a warning, not a failure. Fix: emit `[FAIL]` or a sentinel line; also grep for "LEAK:" and "Timer FAILED".
7. **[M] `make test` timing is fragile.** `Makefile:128` uses `sleep 8 && kill`. On a busy host, boot+66-tests+5-runtime can exceed 8 s → partial log → "Could not determine test result" → false negative. Fix: use QEMU isa-debug-exit + `-device isa-debug-exit,iobase=0xf4,iosize=0x04` and kernel-side `outw(0xf4, 0x10)` at end of runtime tests.
8. **[M] 0 tests for scheduler invariants, mutex PI, TLB shootdown, xhci, ext2, pci.** See Coverage section. `kernel/sched.c` has 6 ERRORES entries but only 2 trivial tests (vruntime>0, nice==0). Specifically `test_vruntime_increases` at `tests.c:741` asserts `cur->vruntime > 0` — exerts nothing the scheduler could get wrong.
9. **[M] `test_pmm` binary checked into repo.** `/root/repos/alze-os/test_pmm` is a 21 KB ELF committed at the root. `.gitignore` does not cover it. Not a security risk but violates hygiene.
10. **[M] klog ring grabs spinlock per character.** `klog.c:36-48`. 30-char LOG_INFO = 30 spin_lock/unlock cycles. Fix: `klog_write_line()` that locks once per line.
11. **[L] PROGRESS.md claims "29/29 tests" for 12+ sessions in a row.** Stale count. `runtime_tests.c:142` says "41 kernel + 5 runtime". Reality: 64 effective (66 registered, cap=64) + 5 runtime.
12. **[L] ERRORES.md last updated 2026-03-30.** Subsequent bug fixes (commit `6101b72` workqueue NULL-deref, commits on LOG_INFO migration) were not logged. The discipline has lapsed.
13. **[L] `.gitignore` missing `*.log`, `test_pmm`, `build/serial.log`, `*.pem`, `*.key`.** Low risk for a kernel project but easy win.
14. **[L] kprintf rate limiter path exists (`kprintf.c:316-332`) but `struct kprintf_ratelimit` has no call sites.** Dead infrastructure.
15. **[L] Panic handler path prints "Task Info: (see klog dump above for context)" — cosmetic dishonesty.** `panic.c:141-142`.

No kprintf-format-string unbounded buffer issue (ksnprintf is correct). No gitleaks findings. No hardcoded credentials.

## Recommendations

Prioritized, biggest safety-per-effort first:

1. **Fix panic re-entrancy guard + dump current task** (issue #1, #4). One `static int in_panic` flag and a 3-line task print would turn a worst-case infinite-loop into a 2nd-chance HLT. ~20 LOC.
2. **Raise `SELFTEST_MAX` and `PANIC` on overflow** (issue #2). Trivial fix with outsized return: uncovers silent test loss and forces future additions to either grow the cap or fail loudly.
3. **Add unit tests for three most under-covered, highest-bug-count subsystems**:
   - (a) `kernel/sched.c` — priority inheritance round trip, `task_exit` + reaper, watchdog kills stuck task, CFS nice weighting produces expected pick order.
   - (b) `kernel/mutex.c` + `kernel/waitqueue.c` — 2-thread contended acquire, timeout, PI boost + restore.
   - (c) `kernel/ext2.c` — read of a known superblock, directory traversal, path resolution — against a fixed fixture ramdisk image committed under `tests/fixtures/`.
4. **Per-task stack canary + entropy mixing** (issue #5). Mix TSC with CMOS + Limine random + PIT count; store per-task guard in TCB, swap on context switch via GS base. Requires ~40 LOC in `sched.c` and `context_switch.asm` but closes the "one leak pwns all" class.
5. **Replace `sleep 8; kill` harness with isa-debug-exit** (issue #7). Deterministic CI signal.
6. **Reactivate ERRORES.md discipline** (issue #12). Add a git `commit-msg` hook that rejects commits touching `kernel/*.c` if neither PROGRESS.md nor ERRORES.md was updated in the same commit.
7. **Fuzzing that would pay off** — biggest ROI targets, in order:
   - `kvsnprintf` format-string fuzz (libfuzzer harness, trivial to write).
   - ext2 superblock/inode parser on mutated 4 KB blocks.
   - `kprintf` against random format strings (catch `%` at end of string, `%lll`, `%-`, width overflows).
   - PCI config-space reader against random 256-byte header blobs.
   - PMM buddy allocator already has the userspace harness — extend it with AFL-driven alloc/free sequences.

## Risk zones

A typical CI (github actions) would catch, but current process does not:

- **Stale test counts.** No assertion that "`register_selftests` calls <= SELFTEST_MAX". CI could grep and fail.
- **Dropped tests beyond SELFTEST_MAX.** No check at runtime that `selftest_count()` equals the number of registrations attempted.
- **`[ERROR]` vs `[FAIL]` mismatch in runtime_tests.** CI could grep both.
- **Boot time > 8 s.** No timeout detection, just silent failure.
- **Memory leak on warm boot path.** Only the final delta is checked; intermediate peaks are invisible.
- **Panic during panic.** Recursion would be caught by a simple "panic printed exactly once in log" assertion.
- **Drift between PROGRESS.md and reality.** Could be enforced by a script that extracts "Tests: X/X" and compares to `grep -c selftest_register`.
- **Unused dead code.** `klog_write_level`, `kprintf_ratelimit` struct — `clang --analyze` or `-Wunused-function` with `-Werror` would find them (already enabled, but they're marked `extern`/`void` so they escape).
- **SSP canary observability.** No test that `__stack_chk_guard != SSP_INITIAL_CANARY` after `ssp_init()`. A regression where `ssp_init()` silently became a no-op would not be detected.
- **Missing CI config entirely.** No `.github/workflows/`, no `.gitlab-ci.yml`. The discipline relies wholly on the operator running `make test` locally.

End of review.
