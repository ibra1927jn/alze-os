# ALZE OS — Scheduling & Synchronization Review

Reviewer: senior concurrency engineer
Date: 2026-04-21
Commit: 6101b72 (fix workqueue static init)

## Scope

| File | LOC |
|---|---|
| `kernel/sched.c` | 688 |
| `kernel/sched.h` | 63 |
| `kernel/context_switch.asm` | 45 |
| `kernel/task.h` | 145 |
| `kernel/spinlock.h` | 96 |
| `kernel/adaptive_mutex.h` | 89 |
| `kernel/mutex.c` | 114 |
| `kernel/mutex.h` | 69 |
| `kernel/rwlock.h` | 93 |
| `kernel/atomics.h` | 89 |
| `kernel/semaphore.c` | 69 |
| `kernel/semaphore.h` | 52 |
| `kernel/waitqueue.c` | 111 |
| `kernel/waitqueue.h` | 72 |
| `kernel/workqueue_def.c` | 57 |
| `kernel/workqueue_def.h` | 78 |
| `kernel/ktimer.c` | 146 |
| `kernel/ktimer.h` | 80 |
| `kernel/qos.h` | 78 |
| `kernel/percpu.c` | 30 |
| `kernel/percpu.h` | 104 |
| `kernel/cpuidle.c` | 89 |
| `kernel/cpuidle.h` | 52 |
| **Total** | **2509** |

## Scheduler

Hybrid O(1) bitmap + CFS-like + EDF, single global runqueue.

- **Structure**: 64 FIFO priority queues (`prio_queues[]`) + `uint64_t prio_bitmap` + separate sorted `deadline_queue` (EDF) + `sleep_queue` + `dead_queue` (`sched.c:78-132`). BSF instruction picks highest priority in 1 cycle (`sched.c:86-91`).
- **Fairness within a priority**: `bitmap_dequeue()` scans the FIFO list of the highest-priority bucket and picks the task with the lowest `vruntime` — CFS-style (`sched.c:117-123`). This degrades dequeue from O(1) to O(N) within a priority band (N = runnable at that prio).
- **Tick source**: PIT IRQ0 at 100 Hz (TIMER_TICK_MS=10) via `irq_handler_c` → `pit_tick`/`ktimer_tick`/`sched_tick`/`watchdog_check` → `pic_eoi` → conditional `schedule()` (`idt.c:149-166`). Order is correct: EOI before reschedule (so PIC is re-armed even if context switch never returns).
- **Preemption**: Only voluntary (`schedule()`, `sched_yield`, blocking primitives) and tick-driven via `need_resched` flag. No `preempt_disable/enable` nesting counter — preemption is implicitly disabled while `sched_lock` is held (since it does `cli`).
- **Quantum**: Per-QoS, 1..8 ticks (`qos.h:42-48`). `sched_tick` sets `need_resched` when `ticks_used % quantum == 0` (`sched.c:524`).
- **Voluntary yield**: `sched_yield` → `schedule()` (`sched.c:479`).
- **Idle**: single global idle task (stored in percpu `idle`) running `sti; hlt` loop, also reaps dead tasks (`sched.c:237-242`). Does NOT call `cpu_idle()` — MWAIT/deep C-states go unused.
- **SMP**: not load-balanced. One `sched_lock`, one `prio_bitmap`, one `task_pool`. Per-CPU struct exists but only BSP is initialized (`percpu.c:15-30`). The `get_current()`/`set_current()` accessors are per-CPU-aware though the scheduler state is not.
- **Watchdog**: tasks exceeding 10 000 ticks without reschedule get marked dead inline from `sched_tick` (`sched.c:510-521`).

## Context switch

`context_switch(old, new)` — System V AMD64 ABI, RDI=old, RSI=new.

- Saves the six callee-saved regs (`rbp,rbx,r12,r13,r14,r15`) on old stack, writes RSP to `old->rsp` (offset 0, hard-coded), loads `new->rsp`, pops callee-saved, `ret` (`context_switch.asm:21-45`). ABI-correct (caller-saved rax/rcx/rdx/rsi/rdi/r8-r11 are scratch per call convention — the compiler has already spilled anything live).
- **RFLAGS**: *not saved/restored*. `schedule()` holds `sched_lock` which did `cli` via `spin_lock_irqsave`. Every path out of `schedule()` eventually hits `spin_unlock_irqrestore` with the IF that the *current invocation* saved, so the switched-in task sees IF restored from its own earlier save when it reaches line 474. Fresh tasks run through `task_entry_trampoline` which unlocks and explicitly re-enables IF (`sched.c:225-233`). Correct, but fragile — see Issues.
- **FPU/SSE/AVX**: *no XSAVE/FXSAVE anywhere in the tree*. No lazy-FPU trap (#NM stub prints a string and halts per `idt.c:89`). Kernel code must avoid `float`/SSE; any compiler auto-vectorization would silently corrupt state across switches. No `-mno-sse/-mgeneral-regs-only` check documented.
- **Segment regs / TSS**: no CS/SS/DS reload — flat kernel, so unchanged across switches. TSS.RSP0 is *not* updated per-task (fine while everything runs in ring 0; breaks when user mode lands).
- **TLB**: kernel-only, shared CR3 → no flush needed on switch. Guard page (bottom stack page unmapped via `vmm_unmap_page`) catches overflow (`sched.c:291-294`).
- **Cache**: L1 prefetch of new task's stack top before switch (`sched.c:467-469`).

## Task model

`struct task` (`task.h:51-92`) — 144 bytes approx. `rsp` MUST be offset 0 (asserted implicitly by `context_switch.asm:30`).

- **Fields**: rsp, stack_phys, tid, state, priority, entry, name[16], run_node, ticks_used, sleep_until, finished, join_wq, qos, deadline, watchdog_ticks, vruntime, nice, fd_table.
- **Lifecycle**: READY (queued) → RUNNING (on CPU) → {SLEEPING (sleep/wait), DEAD}. No BLOCKED vs SLEEPING split. No ZOMBIE intermediate — reaper frees stack+TCB from `dead_queue` (`sched.c:573-603`).
- **Wait reasons**: implicit (via wait-queue pointer). No diagnostics on which WQ a task is blocked on.
- **Kernel vs user**: kernel-only threads. No user-mode yet (no CR3 swap, no user stack, no TSS RSP0 update per task).
- **Stack**: 4 pages = 16 KiB; bottom page unmapped as guard (`sched.c:294`); 8-byte canary at the next page boundary, checked on every `schedule()` (`sched.c:205-221, 435`).
- **TCB pool**: static 64-entry `task_pool[]` + BSF-bitmap allocator (`sched.c:134-190`), O(1) alloc/free.

## Spinlocks

Ticket spinlock, FIFO fair, single-holder (`spinlock.h:27-50`).

- Uses `__atomic_fetch_add(... RELAXED)` for the ticket and `__atomic_load_n(... ACQUIRE)` for the serving poll — **correct release/acquire pairing** with unlock (`RELEASE`).
- `spin_trylock` uses a strong CAS on `next_ticket` with expected = current serving — only succeeds when lock is idle (`spinlock.h:56-61`).
- `spin_lock_irqsave` / `spin_unlock_irqrestore` save RFLAGS with `pushfq; pop`, `cli`, then lock. Unlock: release ticket, *then* `sti` if IF was set (`spinlock.h:78-94`). Correct ordering (release before sti — avoid delivering an IRQ that tries to take the same lock we just freed only after HW fetch).
- **No debug infrastructure**: no owner tracking, no caller `__builtin_return_address` record, no held-across-sleep detection, no lockdep-style cycle checking. Under contention you will have zero visibility.

## Adaptive mutex / mutex / rwlock

**adaptive_mutex** (`adaptive_mutex.h`): spin-then-sleep (100 iters ≈ microsecond, fine on single CPU but small on SMP). Uses `__sync_lock_test_and_set` → full barrier, acceptable. Priority inheritance in sleep phase.
- `ADAPTIVE_SPIN_LIMIT=100` (`adaptive_mutex.h:29`) — tuneable but not documented.

**mutex** (`mutex.c`, `mutex.h`): sleeping mutex with priority inheritance. `mutex_set_owner` records `saved_priority` on acquisition (`mutex.c:24-31`). Fix from ERRORES (2026-03-28) correctly avoids re-saving inside the boost path.
- Fairness: `wq_wake_one` takes the head of the list → FIFO wake order.
- Recursion policy: **none** — same task taking a held mutex deadlocks itself (no owner check for recursion).
- Lock-ordering doc: absent. Nothing comments `m->waiters.lock` (a spinlock) vs outer holders.

**rwlock** (`rwlock.h`): writer-preference via `w_waiting` counter (`rwlock.h:67-84`). Busy-wait with `pause` — not sleeping, so rwlock contention wastes CPU. No integration with wait queues. Re-entrancy of readers not supported (a reader who takes the lock twice from same task is fine but unobservable).

**Lock ordering**: no documented global order. Grep shows nesting: in adaptive_mutex, `m->guard` → `waiters` (same struct); in mutex, `waiters.lock` held across `list_push_back(&m->waiters.waiters, ...)`. The scheduler takes `sched_lock` → then `sched_add_ready` (again `sched_lock`, recursive via wq_wake → sched_add_ready). See Issues.

## Atomics

GCC `__atomic_*` built-ins, defaulting to `__ATOMIC_SEQ_CST` (`atomics.h:27-81`).
- `atomic_read/set`, `add/sub/inc/dec`, `cas`, `xchg`, `test_and_set`, `clear`, and `fence` variants (acq/rel/seq_cst).
- Everything is SEQ_CST — over-strong but safe. No acq/rel splits at the atomic API level — relaxed accelerations must be done ad hoc (and are: spinlock uses relaxed/acquire/release correctly).
- Architecture assumption: x86_64 TSO. Stores are implicitly released, loads implicitly acquired at the hardware level; all `__atomic_*` operations on x86 compile to `lock`-prefixed ops or plain movs with natural TSO.

## Waitqueue / semaphore

**waitqueue** (`waitqueue.c`): protected by per-WQ spinlock. `wq_wait` sets state=SLEEPING, enqueues self, releases spinlock, calls `schedule()`. **Missed-wakeup safety**: the pattern is NOT `wait_event` / `wait_event_interruptible` style — it lacks a cond-check-under-lock. Callers must handle spurious wake and re-check their own condition; all current callers (mutex, semaphore, mq) do (`semaphore.c:13-33`, `mutex.c:35-68`, `msgqueue.c:50-99`).
- `wq_wake_one` / `wq_wake_all`: pull from head, flip state to READY, call `sched_add_ready` (which takes `sched_lock`) **while still holding the wq spinlock** (`waitqueue.c:32-74`). See Issues.
- `wq_wait_timeout` uses `cur->sleep_until` — reuses the scheduler's sleep-queue timeout path, but task is NOT on `sleep_queue`, so `wake_sleepers` will never remove it. The timeout branch therefore relies on "caller polls `pit_get_ticks()` after waking", which only works if someone else wakes the task (spurious). See Issues.

**semaphore** (`semaphore.c`): standard counting sem with re-check loop. Good pattern.

## Workqueue

Single global `system_wq`, FIFO, process-context execution (`workqueue_def.c`).
- Static initialization post-fix (`workqueue_def.c:9-14`) — the recent commit 6101b72 was because `LIST_HEAD_INIT(system_wq.items)` depended on `system_wq` being defined *before* its initializer referenced it; as a plain C static initializer this is legal only because the sentinel is accessed by address. The fix ensures sentinel->next/prev point to sentinel at link time.
- No delayed/scheduled work primitive (no `workqueue_schedule_delayed`). No cancellation API (`work_cancel`). `w->pending` is the only re-enqueue guard.
- No worker thread — `workqueue_process_system()` must be called manually. Currently nothing calls it (grep shows no callers outside the def itself). **Dead code path in production** unless the idle thread or a kworker is added.
- Per-CPU: none.

## ktimer

Single-wheel (256 slots, 10ms/slot → 2.56 s/revolution), O(1) insert, O(slot_size) per-tick scan (`ktimer.c`).
- Slot = `expires & 255`. Timers scheduled >2.56 s in the future wrap onto a wheel slot that will fire *too early*. There is no cascading wheel (Linux has 5-level hierarchical wheel); `list_for_each_safe` in `ktimer_tick` handles this by checking `now >= t->expires` before firing, so a wrap-around timer stays in the slot but gets re-examined every 2.56 s — effectively the wheel loses precision for long timers (each re-visit could be up to `TIMER_WHEEL_SIZE` ticks late). Acceptable given the intended short-timer use.
- Callback runs with `timer_lock` released (`ktimer.c:127-129`) — good, avoids reentrancy with `ktimer_start/cancel` from inside a callback.
- Callback runs *in IRQ context* (called from PIT handler). Must not sleep, must not take sleepable locks. Not documented.
- Drift: `t->expires = now + t->interval` (re-arm, `ktimer.c:133`) — drifts (based on actual fire time, not scheduled time). Fine for coarse timers.

## QoS + cpuidle + percpu

**QoS** (`qos.h`): 5 classes mapping to (priority, quantum). Pure function of `t->qos` — no dynamic boost/decay. Used in `sched_tick` via `task_qos_quantum(cur)` (`sched.c:524`). `task_set_qos` changes both priority and class atomically by writing two fields without holding `sched_lock` — tiny race window.

**cpuidle** (`cpuidle.c`): MONITOR/MWAIT detection + C1 entry. Implemented but **never called**: the idle task uses inline `sti; hlt` instead of `cpu_idle()`. Dead code for now. No tickless (pit_set_oneshot not invoked on idle entry).

**percpu** (`percpu.c`, `percpu.h`): GS-based, 6-field `struct cpu_local` with ABI-hardcoded offsets (0/8/16/24/28/32). `get_current`/`set_current` are single-instruction `mov %gs:8`. Only BSP is initialized. There is no `preempt_disable` around access — a preempting IRQ between two accesses on different CPUs would be a disaster on SMP, but single-CPU makes it fine. The `ticks` field is unused.

## Highlights

- Ticket spinlocks with correct acquire/release ordering — better than typical hobby-OS test-and-set.
- CFS-style `vruntime` fairness inside priority bands is an unusual but elegant combination with O(1) bitmap + EDF override.
- Stack guard pages + canaries + watchdog kill form solid defense-in-depth against stack overflow / runaway tasks.
- Priority inheritance in both `mutex` and `adaptive_mutex`.
- ERRORES.md shows a disciplined post-mortem culture — several subtle races in this subsystem were already caught (bsfq UB, PI double-save, join TOCTOU, TCB leak on failure).

## Issues found

1. **Missed-wakeup via unlocked join_wq assignment in `task_join`** — `sched.c:393-402`. After releasing `sched_lock`, the code loops `while (!target->finished) wq_wait(target->join_wq)`. Between the store `target->join_wq = &join_wq` and the wait, the target may exit, observe `self->join_wq` as the just-assigned pointer, call `wq_wake_all`. Fine. But if *another* joiner arrives with a *different* local wq, it overwrites `target->join_wq` (only first joiner's WQ wins due to `if (!target->join_wq)`), so a second joiner sees `target->join_wq` pointing to the **first joiner's stack frame** — if the first joiner woke and returned, that frame is dead. Use-after-free. Comment at `sched.c:389` claims safety only for single joiner.

2. **`wq_wait_timeout` is broken** — `waitqueue.c:78-111`. Task sets `sleep_until` but is *not* put on `sleep_queue`, so `wake_sleepers` never dequeues it. If nobody else wakes it, it sleeps forever; the caller's post-schedule() check at line 98 only runs when *some other* waker returns control. Fix: either add self to `sleep_queue` as well, or wire up a ktimer that calls `wq_wake_one`.

3. **`wq_wake_one/all` holds wq spinlock across `sched_add_ready`** — `waitqueue.c:41-51`, `waitqueue.c:56-74`. `sched_add_ready` acquires `sched_lock`. Lock order here: *wq->lock* → *sched_lock*. In `schedule()`, order is *sched_lock* only — no nesting. But in `task_exit` (`sched.c:323-339`), `sched_lock` is taken, then `wq_wake_all(self->join_wq)` is called **while still holding `sched_lock`**. So the ordering is *sched_lock → wq->lock → sched_lock (recursive)*. On a single CPU with `cli`, recursion is detected by `spin_lock_irqsave` spinning forever on its own ticket — **self-deadlock** in `task_exit` when a joiner is waiting. (Single-core saves the kernel only because `cli` masks the clock — but the task never exits.)

4. **`mutex_unlock` calls `sched_add_ready` while holding `m->waiters.lock`** — `mutex.c:93-97`. Same ordering issue as #3: *mutex.waiters.lock* → *sched_lock*. If `schedule()` tries to acquire both in the other direction, deadlock.

5. **`sched_tick` watchdog-kill manipulates state without rescheduling cleanly** — `sched.c:510-521`. It flips `cur->state = TASK_DEAD`, `finished = true`, `wq_wake_all` (nested lock — same issue #3), enqueues on `dead_queue`, sets `need_resched`, returns. But the task is still RUNNING on this CPU. When `schedule()` runs next, it sees `old->state != TASK_RUNNING` → does NOT re-enqueue (correct), but `old` is now on `dead_queue`. If `context_switch(old, next)` saves old's RSP onto a dead stack, then reaper calls `pmm_free_pages(stack_phys)` **while** a context might still be mid-switch on that stack → use-after-free. The PIT handler runs on the victim's own kernel stack (no IST), so everything up the call chain writes into that stack.

6. **`sched_tick` runs entirely without `sched_lock`** — `sched.c:485-526`. Reads/writes `cur->ticks_used`, `watchdog_ticks`, `vruntime`, and in the kill path mutates the dead_queue list without any lock. On SMP this would race with `sched_reap_dead` and any concurrent `schedule()`. Single-core works because IRQs are disabled in IRQ handlers, but this is an SMP time bomb.

7. **`task_entry_trampoline` unlocks `sched_lock` but never restores IRQ flags** — `sched.c:225-233`. `spin_unlock(&sched_lock)` is the plain non-IRQ variant, then `sti`. The original `spin_lock_irqsave` saved flags into a local on a caller's stack (long gone). If a nested caller ever took `sched_lock` with `spin_lock` (not irqsave), releasing with the plain unlock is fine; here the original locker was `spin_lock_irqsave` in `schedule()` so its flags record is leaked. Ends up OK because we know we entered with IF=0 and explicitly `sti`, but this path is NOT equivalent to `spin_unlock_irqrestore` and only works because of that assumption. Fragile.

8. **`sched_runnable_count` can loop on stale bitmap** — `sched.c:607-632`. Takes `sched_lock`, reads `prio_bitmap` into local `bm`, then iterates. If `bm` has a bit set for a priority whose list is actually empty at the time of the read (shouldn't happen but only because nothing else clears the bit outside the lock), it still works. Defensive, but `bitmap_find_first` guards against `bm==0` only as the outer exit condition — the inner `list_for_each` on an empty list is a no-op. Fine.

9. **`find_task` scans without `sched_lock`** — `sched.c:194-201`. Called from `task_join` (which does hold the lock, good). No other callers, but the function is non-static in feel. Low risk.

10. **Writer starvation not actually prevented in `rwlock`** — `rwlock.h:41-56`. The `w_waiting` guard prevents *new* readers from entering only when a writer is waiting, but a reader holding the lock when a writer arrives can release and immediately re-acquire via `rwlock_read_lock` before the writer wins the CAS race, because both are busy-spinning with `pause`. Probability is low but bounded by CPU-timing. Not catastrophic.

11. **`vruntime` can underflow / skew when a new task inherits `cur->vruntime`** — `sched.c:279-282`. If the current task has run for a long time, new tasks start with a very high vruntime and will be starved by freshly-created low-vruntime tasks from another current. In practice single-scheduler bounded. Mirrors Linux's min_vruntime policy but without a global min.

12. **`task_set_nice` under lock protects only the current task** — `sched.c:535-544`. Fine. But `task_set_deadline` similarly only mutates `cur->deadline` while the task may already be enqueued on `prio_queues[]` (not `deadline_queue`). Setting deadline on a currently-ready task does NOT re-enqueue onto the EDF queue until the next `schedule()` reschedules it (and even then, only if the task re-enters `bitmap_enqueue`). Subtle semantics bug: `task_set_deadline` from *another task* on a ready task has no effect.

13. **`adaptive_lock` reads `m->owner` without barriers in spin phase** — `adaptive_mutex.h:44-50`. Spin phase is pure TAS on `locked`; owner is only read in the sleep phase under `guard`. OK on x86 TSO. Not portable.

14. **`pit_get_ticks` read outside lock in `wq_wait_timeout`** — `waitqueue.c:79,98`. Ticks counter is `volatile uint64_t` updated by PIT IRQ. 64-bit read on x86-64 is atomic. Fine.

15. **No FPU state in TCB at all** — any kernel code compiled with SSE enabled will corrupt xmm0 across tasks. Currently the kernel builds with `-mno-sse`? Not visible from source; depends on Makefile (out of scope). If not, latent corruption.

## Recommendations

- **Unify lock ordering**: always call `sched_add_ready` with *no* other spinlock held, OR pull wake-ups out to a post-unlock list (staged wakeup). Fixes #3, #4.
- **Fix `wq_wait_timeout`** by either registering a ktimer that calls `wq_wake_one` at the deadline, or linking into `sleep_queue` in addition to the wq.
- **`task_exit`**: release `sched_lock` before `wq_wake_all(self->join_wq)`, or defer the wake until after `schedule()` returns (it never does — so move the wake to before setting `TASK_DEAD`, enqueue on dead, then drop lock, then `schedule()`).
- **`task_join`**: use a lock-protected list of joiners on the target, not a single `join_wq` pointer that can clobber previous joiners' stack WQs. One `struct wait_queue target->join_wq` owned by the target lifetime, not the first joiner's stack.
- **`sched_tick` watchdog kill**: do not flip state inline. Just set `need_resched` + a `should_kill` flag; let `schedule()` do the transition.
- **SMP-proof `sched_tick`**: take a per-CPU runqueue lock before mutating `cur`'s runtime fields. Today's single-CPU works by coincidence.
- **Lockdep-lite**: record a `const char *name` and a held-stack in each spinlock during debug builds; enforce a total order at `spin_lock` time. Shipping production kernels without deadlock detection is how you earn 3am phone calls.
- **Wire cpuidle**: call `cpu_idle()` from the idle task instead of raw `sti;hlt`.
- **Wire workqueue worker**: spawn a kthread that loops `workqueue_process_system(); wq_wait(&system_wq.wq)` or hook it into idle.
- **Add `preempt_disable/enable` counters** per CPU; assert `preempt_count == 0` at entry to `schedule()` from voluntary paths; detect "sleep with spinlock held" bugs.
- **FPU**: either compile-time ban SSE (`-mno-sse -mno-mmx -mgeneral-regs-only`) or implement lazy FXSAVE on #NM.
- **Document lock order** in a single header: `sched_lock > mutex.waiters.lock > wq.lock > timer_lock > kmalloc class locks > percpu`.

## Risk zones

- **High interrupt load**: ktimer callbacks run in IRQ context with `timer_lock` briefly dropped per-callback. A callback that takes a sleepable lock (mutex) would deadlock, and one that takes another spinlock could invert ordering with some arbitrary subsystem. PIT at 100 Hz + dense wheel slot = extended IRQ latency (up to N callbacks × O(callback)).
- **Two CPUs, opposite lock order**: today impossible (single CPU), but #3/#4 mean that the minute SMP is turned on, `task_exit` calling `wq_wake_all` while holding `sched_lock`, crossed with any CPU running `schedule()` at the moment a nested wq wake tries `sched_add_ready`, will deadlock within seconds.
- **Timer fires during scheduler tick**: the PIT itself is the tick. The IRQ handler dispatches `pit_tick → ktimer_tick → sched_tick → watchdog_check → pic_eoi → maybe schedule()`. If `ktimer_tick` callback triggers a path that flips the current task's state (e.g., a wake that modifies `cur`), `sched_tick` then reads stale state. Today all callbacks are benign. Future hazard.
- **Nested IRQs**: IRQs re-enabled only after `iretq` (from outer handler) or `sti` (in trampoline). Nested IRQs are effectively off. A kernel that ever calls `sti` early inside an IRQ handler (none currently do) would recurse into `sched_tick` and corrupt `ticks_used`.
- **TCB leak under contention**: `alloc_tcb` + `free_tcb` operate on `tcb_bitmap` under `sched_lock`. Good. But `watchdog_check` reads `t->watchdog_ticks` without a lock (`watchdog.c:26-40`) — fine on single CPU, torn on SMP for 32-bit reads on misaligned addresses (here it's aligned `uint32_t`, so atomic).

Overall: solid single-CPU kernel with careful attention to x86 memory ordering, priority inheritance, and O(1) scheduling. The recorded ERRORES culture shows maturing engineering. The top three production-risk items are the nested-lock wakeup ordering (Issues 3/4), the broken `wq_wait_timeout` (Issue 2), and the inline watchdog-kill state flip (Issue 5). Fix those before turning on SMP.
