# R3 — RCU & Modern Kernel Synchronization

Date: 2026-04-22 · Round: R3 deep-dive · Target project: ALZE OS (`/root/repos/alze-os`)
Input: R2 review (`review/scheduling_sync.md`, `review/memory.md`, `review/fs_storage.md`).
Why this file: R2 flagged three P0 classes — (a) nested-lock wakeup ordering in the scheduler (Issues 3/4/5 of scheduling_sync), (b) **no per-task fd table + VFS hot paths take no lock** (Issues 1/2 of fs_storage), (c) global locks everywhere (`pmm_lock`, `vmm_lock`, `vfs_rwlock`) that will melt the first moment AP startup lands. This document surveys the modern synchronization toolbox so we can pick the right primitives for ALZE v1/v2/v3.

---

## 1. RCU (Read-Copy-Update)

**Paul McKenney — IBM / Linux. First shipped in DYNIX/ptx (Sequent), landed in Linux 2.5.43 (October 2002).** The canonical early paper is *Read-Copy Update* (OLS 2002).

### Core idea

Readers **never block, never write, never wait**. A reader traverses a shared data structure under an *RCU read-side critical section* (`rcu_read_lock()` / `rcu_read_unlock()` — on non-preemptible kernels these are *literally* empty macros that only disable preemption, cost ≈ 0). Writers **publish a new version** of the object atomically (one pointer store with release semantics via `rcu_assign_pointer`) and **defer reclamation** of the old version until all pre-existing readers have finished.

The hard problem reduces to: *"when is it safe to free the old copy?"* Answer: after a **grace period**. A grace period ends once every CPU that was inside a read-side critical section at the moment of the publish has left it at least once. Trick: on a non-preemptible kernel you do NOT need to track individual readers; a **context switch on every CPU** is a sufficient witness that no pre-existing reader is still holding the old pointer (because the reader would have disabled preemption, and a context switch can't happen under disabled preemption). So `synchronize_rcu()` boils down to "wait until every CPU has scheduled at least once."

### Reader API

```c
rcu_read_lock();               // preempt_disable on non-preempt kernels
p = rcu_dereference(gp);       // READ_ONCE + dependent-read barrier (no-op on x86 TSO, matters on Alpha/ARM)
if (p) do_something(p->field);
rcu_read_unlock();             // preempt_enable
```

### Writer API

```c
spin_lock(&update_lock);        // serialise writers among themselves
new = kmalloc(sizeof *new);
*new = *old;                    // copy
new->field = NEW_VAL;           // modify
rcu_assign_pointer(gp, new);    // publish — smp_store_release
spin_unlock(&update_lock);
synchronize_rcu();              // blocks until grace period ends
kfree(old);                     // or: call_rcu(old, kfree) — async
```

### Linux usage (load-bearing)

- **dentry cache** (`fs/dcache.c`): path lookup walks the hash buckets under `rcu_read_lock` — this is *the* reason `open("/usr/lib/libc.so.6")` scales.
- **routing table / FIB trie** (`net/ipv4/fib_trie.c`): every packet traverses the trie; writers (route updates) are rare.
- **VFS mount tree**, **SELinux policy**, **IDR**, **task list**, **module list**, **networking sockets** — the list is long.
- **epoll, notifiers, tracepoints**, everywhere a "read-mostly observer list" exists.

**Rule of thumb**: RCU wins when reader-to-writer ratio is ≥ 10:1 and readers are latency-sensitive.

### Costs

- **Reader cost**: zero on non-preempt kernels (an empty macro). On preempt-RCU kernels, ~1 per-CPU counter increment.
- **Writer cost**: the update itself is a normal locked store. `synchronize_rcu()` is *expensive* — can take 10s of milliseconds. Use `call_rcu()` for async reclamation when latency matters.
- **Memory overhead**: old versions are alive until the grace period ends — bursty updates can inflate memory use. Linux bounds this with `rcu_barrier()` and OOM-aware callback scheduling.

---

## 2. RCU variants

| Variant | Read-side | Grace period trigger | When to use | Linux CONFIG |
|---|---|---|---|---|
| **Classic RCU** (Tree-RCU) | preempt_disable | every CPU voluntarily context-switches | general kernel code | `CONFIG_TREE_RCU` (default) |
| **Preemptible RCU** | per-CPU counter | rcu_read_unlock after counter reaches 0 | PREEMPT_RT, low-latency | `CONFIG_PREEMPT_RCU` |
| **SRCU** (Sleepable RCU) | per-srcu-struct counter | `synchronize_srcu` on that struct | reader can block (I/O, mutex) | `CONFIG_SRCU` always on |
| **Tasks RCU** | voluntary context switch | every task has run | trampoline/ftrace patching (needs user-space to exit kernel) | `CONFIG_TASKS_RCU` |
| **Tasks Trace RCU** | explicit enter/exit | trace hook | BPF trampolines | `CONFIG_TASKS_TRACE_RCU` |
| **SRCU-P** (prepare for sleep) | polled by `start_poll_synchronize_srcu` | async | latency-tolerant | 5.10+ |

**Tree-RCU structure**: a two- or three-level tree of per-CPU "rcu_node" structs; each CPU reports quiescent state up the tree. Scales to thousands of CPUs because CPUs contend only on leaf nodes, not a single global counter.

**Tradeoff summary**:
- **Latency**: Preempt-RCU gives bounded read latency (interrupts and preemption allowed inside `rcu_read_lock`). Classic RCU gives unbounded if a CPU never context-switches (e.g., tight loop with preemption disabled).
- **Throughput**: Classic RCU's empty read-side macros deliver *infinite* reader throughput in the non-preempt case.
- **Reclamation latency**: SRCU has longer grace periods because it must wait for readers that may sleep.

---

## 3. Hazard pointers (Maged Michael, 2002 → 2004)

**Maged Michael — "Safe Memory Reclamation for Dynamic Lock-Free Objects Using Hazard Pointers" IEEE TPDS 15(6), June 2004.** (PODC 2002 short version.) US patent 6,826,757 (expired / no longer enforced).

### Core idea

Each thread maintains `K` (typically 2–8) *hazard pointer* slots. Before dereferencing a shared pointer, the reader **publishes the pointer into one of its hazard slots** (with release semantics), then re-reads the shared variable to confirm it still points at the same object (similar to CAS retry loop). A thread that wants to free an object **scans every other thread's hazard slots**; if none is equal to its own pointer, free is safe.

### Pseudocode (reader)

```c
do {
    p = gp_load_acquire(&gp);
    my_hazards[0] = p;
    atomic_thread_fence(memory_order_seq_cst);  // or StoreLoad fence
} while (gp_load_acquire(&gp) != p);
use(p);
my_hazards[0] = NULL;
```

### Reclamation

Retired pointers accumulate in a per-thread retire-list. When the list grows past a threshold (e.g., 2× number of threads × K), the thread scans every hazard slot across the system, builds a set of currently-protected pointers, and frees anything in its retire-list not in that set.

### Comparison with RCU

| Aspect | RCU | Hazard pointers |
|---|---|---|
| Reader write cost | 0 (empty macro) | 1 store + StoreLoad fence |
| Per-object overhead | 0 | O(K) slots per thread |
| Grace-period visibility | coarse (system-wide) | fine (per-pointer) |
| Bounded memory | no (bursty writers inflate) | yes (proportional to K·threads) |
| Protection scope | all pointers in read section | only explicitly published ones |
| Linux kernel use | yes (pervasive) | **no** (Linux chose RCU) |
| Folly / other user-land | yes (folly::hazptr 2018, C++26 `std::hazard_pointer`) | yes |

### Why Linux didn't adopt hazard pointers widely

- Reader StoreLoad fence (on x86 an MFENCE or locked op) costs ≈ 30 cycles — RCU's empty macro wins any read-heavy benchmark.
- Hazard pointers excel for **bounded memory** on unbounded-writer systems — Linux had a budget for worst-case memory use and chose grace-period throttling instead.
- Incremental adoption would have forced two reclamation disciplines to coexist per structure.

### Where hazard pointers shine

- User-land (no kernel context-switch signal to piggyback on).
- Embedded systems with strict memory bounds.
- Lock-free data structures with deep pointer chasing where you don't want a whole read-section marked.
- C++26 standardization (the STL adopted hazard pointers, not RCU, because RCU is deeply tied to the kernel's scheduler concept).

---

## 4. Epoch-based reclamation (EBR)

**Keir Fraser — *Practical Lock-Freedom*, PhD thesis, University of Cambridge, February 2004.** Referenced below as "Fraser 2004". Earlier ideas trace to Harris 2001 / Herlihy.

### Core idea

Maintain a global epoch counter `e ∈ {0, 1, 2}` and a per-thread "local epoch" + "active" flag. A thread entering a critical section snapshots the global epoch into its local slot and sets active. On exit, clear active. A writer that wants to reclaim:
1. Scans all threads; if all active threads are at epoch `e`, advance global epoch to `e+1`.
2. Once the epoch has advanced two steps past the retirement epoch, the retired object is unreachable (no thread can have referenced it during the retirement window).

Three epochs suffice because an active thread can be at most one epoch behind — advancing twice guarantees no active thread is in the retirement epoch's window.

### Pseudocode

```c
// reader
my_epoch = global_epoch;
my_active = true;
atomic_thread_fence(memory_order_seq_cst);
// ... critical section (reads shared structure) ...
my_active = false;

// writer
retire(ptr, current_epoch);
if (all_active_threads_at(current_epoch))
    advance_global_epoch();
// objects retired at epoch e-2 are safe to free
```

### Comparison with hazard pointers

| Aspect | EBR | Hazard pointers |
|---|---|---|
| Read-side cost | 1 plain store (often amortized) + 1 fence | 1 store + 1 fence per pointer |
| Memory bound | **no** — a stuck reader blocks reclamation system-wide | bounded per K slots |
| Per-pointer granularity | no (coarse section) | yes |
| Writer scan cost | O(threads) | O(threads × K) |
| Ease of use | very easy — drop-in "enter/exit" | invasive — must retrofit every pointer load |

### Adoption

- **crossbeam / crossbeam-epoch** (Rust, Aaron Turon): the canonical EBR implementation in a language standard library. All Rust lock-free data structures (Tokio's scheduler, concurrent hashmaps, crossbeam::channel) ride on it.
- **Boost.Intrusive / libcds** (user-land C++): EBR and hazard pointers both available.
- **Linux kernel**: no EBR; RCU subsumes its use case and piggybacks on the scheduler.

### Hybrid: QSBR (Quiescent-State-Based Reclamation)

Userspace-RCU library (Mathieu Desnoyers 2009, `liburcu`) implements QSBR, which is essentially EBR with explicit "I am now quiescent" callbacks — closer to Linux RCU than to raw EBR.

---

## 5. Lock-free data structures

### Treiber stack (R. Kent Treiber, IBM RJ 5118, 1986)

Simplest lock-free structure. Push = CAS the head pointer; pop = CAS head with head→next.

**ABA hazard**: between reading head and CAS-ing, the head can be popped, pushed back with same address but different next. Classic mitigations: tagged pointers (upper bits as version counter), hazard pointers, or RCU.

### Michael-Scott queue (PODC 1996)

**Maged Michael + Michael Scott — "Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms" PODC 1996.**

Two-lock blocking queue (head lock + tail lock — independent) *and* a fully lock-free variant using CAS on head/tail pointers. The lock-free variant handles the tail-lagging-behind case by having enqueuers help advance tail before retrying.

**Pitfalls**:
- ABA on head/tail (solve with tagged pointers or HP).
- Linked-list nodes need safe memory reclamation.
- Dequeue-then-read of next pointer must be ordered (acquire-release).

Used in Java's `ConcurrentLinkedQueue`, .NET, many OS work-stealing runtimes.

### Harris lock-free linked list (DISC 2001)

**Tim Harris — "A Pragmatic Implementation of Non-Blocking Linked-Lists" DISC 2001.**

Sorted singly-linked list with **logical deletion via a mark bit stolen from the next pointer LSB**. Three steps:

1. Find predecessor `p` and successor `s` of the node `n` to delete.
2. CAS `n→next` from `s` to `s | MARKED` — logical delete.
3. CAS `p→next` from `n` to `s` — physical unlink.

Other threads traversing the list that encounter a marked next-pointer help complete step 3 (compete-and-help pattern).

This pattern became the basis for **Harris-Michael list** (Michael 2002 added hazard-pointer safety) and split-ordered hash tables.

### Split-ordered hash table (Shalev + Shavit, 2006)

**Ori Shalev + Nir Shavit — "Split-Ordered Lists: Lock-Free Extensible Hash Tables" JACM 53(3), 2006.**

Hash table that **grows without rehashing**: one underlying sorted lock-free list (Harris), where keys are ordered by their bit-reversed hash. Buckets are "views" into sections of this list; growing the table adds new bucket pointers without moving data.

Key insight: by using bit-reversed ordering, splitting a bucket means putting a sentinel node in the middle — no data movement, only pointer insertion.

Used in Java's `ConcurrentHashMap` ideas (early versions), libcds, folly.

### MPMC queues (Dmitry Vyukov)

**Dmitry Vyukov — 1024cores.net blog series, ~2011–2014. Intel/Google.**

- **Bounded MPMC queue** based on circular array + per-slot sequence number; each slot has a seq counter that readers CAS — extremely low contention, 2 cache lines per slot.
- **Unbounded MPMC**: linked list of bounded segments.

Core trick: the *sequence* field turns the queue into per-slot readers/writers that do not CAS the same location unless they are actually both trying to use that exact slot. Used in the Rust `crossbeam::channel` bounded variant, in Go's internal runqueues (work-stealing), and in LMAX Disruptor (semantically).

### Use cases: kernel vs user-land

| Structure | Kernel use | User-land use |
|---|---|---|
| Treiber stack | slab freelists, per-CPU free stacks | lock-free pools |
| MS queue | network rx rings (partial), kfifo (ring variant) | Java `ConcurrentLinkedQueue` |
| Harris list | sparse — Linux prefers RCU list with spinlock updates | libcds, folly |
| Split-ord hash | not in kernel — kernel uses RCU hash table (`hlist` + `rcu_head`) | folly F14, ConcurrentHashMap |
| Vyukov MPMC | Linux `kfifo` is a simpler cousin | crossbeam, Go runqueue, Disruptor |
| **RCU-protected hash** (`hlist_bl`, `dcache`) | **pervasive in kernel** | uRCU, liburcu |

The kernel pattern: **use a spinlock for writers, RCU for readers**. Pure lock-free is rare in Linux because RCU-protected structures give equivalent reader scalability with dramatically simpler code.

---

## 6. Memory barriers / fences

### x86_64 TSO (Total Store Order)

- Loads are **not** reordered with other loads.
- Stores are **not** reordered with other stores.
- Loads **may** be reordered with earlier stores to **different** locations (StoreLoad reordering — the only relaxation).
- Locked RMW instructions are full barriers.

Consequence: on x86_64, `smp_rmb()` and `smp_wmb()` are **compiler barriers only** (`asm volatile("" ::: "memory")`). `smp_mb()` requires an `mfence` or a locked op.

### ARM / weak ordering

ARMv8 is weakly ordered: loads/loads, loads/stores, stores/loads, stores/stores can all be reordered. `DMB ISH` is the full barrier. Acquire/release semantics are provided natively by `LDAR`/`STLR` instructions in ARMv8.1+.

### Linux smp_* helpers

| Macro | Meaning | x86 cost | ARM cost |
|---|---|---|---|
| `smp_rmb()` | LoadLoad barrier | compiler barrier | `dmb ishld` |
| `smp_wmb()` | StoreStore barrier | compiler barrier | `dmb ishst` |
| `smp_mb()` | full barrier | `lock addl $0, 0(%%rsp)` | `dmb ish` |
| `smp_load_acquire(p)` | load + consume-later ordering | plain MOV | `LDAR` |
| `smp_store_release(p, v)` | prior-writes + store ordering | plain MOV | `STLR` |
| `smp_mb__after_atomic()` | orders after atomic RMW | no-op | no-op (atomics imply) |
| `smp_mb__before_atomic()` | orders before atomic RMW | no-op | no-op |

### C11 / C++11 memory model

- `memory_order_relaxed` — atomicity only, no ordering.
- `memory_order_acquire` / `memory_order_release` — one-way barriers.
- `memory_order_acq_rel` — both, for RMW.
- `memory_order_seq_cst` — total order, default if unspecified, expensive.
- `memory_order_consume` — data-dependency-only order. **Discredited**: compilers can't express the DEP annotation; in practice all compilers promote it to `acquire`. RCU's `rcu_dereference` is the real-world `consume` and is implemented with compiler magic (`READ_ONCE` + barriers on weak archs).

`atomic_thread_fence(order)` issues a standalone fence without an atomic op.

ALZE uses GCC `__atomic_*` built-ins defaulting to `SEQ_CST` (from review/scheduling_sync.md §Atomics). That is **safe but slow**; on x86 SEQ_CST stores compile to locked ops (~25 cycles) when `RELEASE` would compile to plain MOV (~1 cycle).

---

## 7. Spinlocks, mutexes, rwlocks

### Spinlocks

- **Test-and-set (TAS)**: `while (xchg(&lock, 1)) pause;`. Simple, no fairness, cache-line bounces under contention.
- **Test-and-test-and-set (TTAS)**: same but read first, CAS only when observed free. Reduces cache coherency traffic.
- **Ticket spinlock**: two counters, `next` and `serving`; fetch-and-add `next` to reserve a ticket, spin until `serving == ticket`. **FIFO fair**. ALZE uses this (review/scheduling_sync.md §Spinlocks). Downside: cache-line bouncing across all waiters on every unlock.
- **MCS spinlock (Mellor-Crummey + Scott, 1991)**: each waiter spins on its own local node, avoiding coherency storm. Enqueue into a linked list; the current holder's unlock writes directly into the next node's field. Complex API (waiter must pass a node).
- **qspinlock (Linux 4.2+, Peter Zijlstra + Waiman Long, 2014)**: Linux's current default. Fast path is a single CAS when uncontended. Contended path uses an MCS-like per-CPU queue node. Fits in **4 bytes** despite queueing (uses per-CPU MCS nodes indexed by context: task / softirq / hardirq / nmi). Scales to 100+ cores.
- **Paravirt-aware spinlock**: virtual-machine kernels need to handle "preempted holder" (host scheduled the vCPU holding the lock out). Linux uses `__pv_queued_spin_unlock` with a vCPU halt + kick-unhalt.

**ALZE's ticket spinlock is already in the top tier for a single-socket small system**; qspinlock buys you 4-byte footprint + scalability on many-core, at the cost of ~500 LOC.

### Mutexes

- **Sleeping mutex**: acquire → try atomic, if fail put self on wait queue, sleep. Wakeup on unlock.
- **Adaptive mutex (spin-then-sleep)**: spin for N iterations; if still contended and holder is running, keep spinning; else sleep. Solaris "adaptive mutex" pioneered this; Linux `mutex` has been adaptive since 2.6.23. ALZE has `adaptive_mutex` with 100-iter spin (review/scheduling_sync §adaptive_mutex) — correct direction, tuning is TBD.
- **Priority inheritance mutex (PI-mutex)**: when a low-priority holder blocks a high-priority waiter, temporarily boost holder to waiter's priority. Linux's `rt_mutex` implements this with a priority-waiter tree. ALZE has PI in both `mutex` and `adaptive_mutex` — good.
- **Robust mutex (pthread)**: if the holder dies while holding, next waiter gets `EOWNERDEAD` and can recover or abandon. Implemented via robust-futex list in kernel.

### Reader-writer locks

- **Classic rwlock**: reader count + writer flag. Fast for reads, bad under writer pressure (reader starvation without care).
- **Writer-preference rwlock**: ALZE's approach (`w_waiting` counter blocks new readers).
- **Seqlock** (see §8) — preferred for read-mostly when writes are rare.
- **Percpu-rwsem (Linux)**: optimized for reader-mostly; reader takes a per-CPU counter (no cache bouncing); writer synchronizes all per-CPU counters with RCU. Used for `freeze_super`, `cgroup_threadgroup_rwsem`.
- **Big-reader lock (brlock, historical)**: pre-percpu-rwsem version. Readers cheap, writer takes all per-CPU locks.

ALZE's rwlock uses busy-wait spinning on both sides (review/scheduling_sync §rwlock) — that is pessimistic; writers should sleep via wait queue. Under any real contention it will waste CPU. Recommended: upgrade to **spin-on-uncontended, sleep-on-contention** using existing `waitqueue` machinery.

---

## 8. Seqlocks

**Historically attributed to Christoph Hellwig's kernel work; shipped in Linux 2.5, replaced the earlier jiffies-protection scheme.**

Readers and writers coordinate via an **even/odd sequence counter**:

```c
// writer
write_seqlock(&lk);     // seq++; seq is now odd
update_shared_data();
write_sequnlock(&lk);   // seq++; seq is now even

// reader
do {
    s = read_seqbegin(&lk);  // load seq; if odd, wait
    data = shared_data;       // may read torn data
    smp_rmb();
} while (read_seqretry(&lk, s));  // retry if seq changed
```

- Readers **never block writers** and **never wait** (beyond the retry loop). Zero cache-line ownership transfer on the read path.
- Writers are serialized by an internal spinlock.
- Tradeoff: readers may see **torn** data mid-update, which is why the retry is required. The read must be idempotent — side-effecting reads are forbidden.

**Linux usage**:
- `jiffies_64` (wall-clock on 32-bit)
- `struct timekeeper` — `__ktime_get_ts64` uses a seqlock (`tk_core.seq`).
- `d_path` in dcache.
- `task_cputime_adjusted`.

**Not suitable when**:
- The read is expensive (retry is costly).
- The read has side effects.
- You need readers to actually block writers.

For ALZE's time-of-day / `pit_get_ticks` → seqlock is a natural upgrade once SMP is on (review/scheduling_sync Issue #14 notes 64-bit read is atomic on x86 today, so this is a v2 item).

---

## 9. Futexes (Linux)

**Ulrich Drepper — "Futexes Are Tricky" 2005 (Red Hat tech report).** Original Linux futex by Hubertus Franke + Rusty Russell + Matthew Kirkwood (2002).

### Core idea

User-space fast path, kernel slow path. A futex is just a `uint32_t` in shared memory. Uncontended lock/unlock never enters the kernel:

```c
// lock: user-land CAS 0→1
if (atomic_cmpxchg(&m, 0, 1) == 0) return;    // no kernel call
// contended path: atomic_cmpxchg(&m, 1, 2); syscall(SYS_futex, &m, FUTEX_WAIT, 2, ...)

// unlock: if (atomic_store(&m, 0)) no kernel call
// if was 2: syscall(SYS_futex, &m, FUTEX_WAKE, 1)
```

### Why "tricky"

Drepper's paper enumerates the traps:
- **Lost wakeup** — must re-check value inside kernel before sleeping (the FUTEX_WAIT compares to expected value atomically with the sleep).
- **PI futex** — kernel-side priority inheritance, walks the blocking chain to boost priorities transitively.
- **Robust futex** — user-space registers a list of held futexes with the kernel. On thread death, kernel walks list and marks each "owner died".
- **Requeue** (`FUTEX_CMP_REQUEUE`) — move waiters from one futex to another without waking them; essential for `pthread_cond_broadcast` without thundering herd.

### Kernel data structures

- `futex_hash_buckets` indexed by `(mm, uaddr)` — a global hash table of per-futex waiter queues.
- A waiter on a futex pins a reference to the `struct page` of its uaddr (to avoid unmap races).
- Priority-inheritance futex uses `rt_mutex` internally.

### Relevance to ALZE

Futexes are a **user-space-first** primitive. ALZE is kernel-only today (no user mode — review/scheduling_sync §Task model: "kernel-only threads"). Futex support becomes a P1 item the moment user mode lands; plan for it but do not build it in v1/v2.

---

## 10. Formal memory models

### LKMM — Linux Kernel Memory Model

**Alan Stern + Andrea Parri + Paul McKenney + Luc Maranget + Jade Alglave — "Frightening Small Children and Disconcerting Grown-ups: Concurrency in the Linux Kernel" ASPLOS 2018.** Earlier version as "A Formal Kernel Memory-Ordering Model" LWN 2017 (parts 1/2).

LKMM formalizes what the Linux source *means* by its barriers:
- `READ_ONCE`, `WRITE_ONCE`
- `smp_rmb`, `smp_wmb`, `smp_mb`
- `smp_load_acquire`, `smp_store_release`
- `rcu_dereference`, `rcu_assign_pointer`
- locked RMWs and their fence implications.

The model is expressed in `.cat` files interpreted by **herd7** (from the Alglave–Maranget diy/herdtools suite). You can write a litmus test (e.g., message-passing pattern), ask herd7 "can this outcome happen on LKMM?" and get yes/no.

LKMM is **weaker** than SEQ_CST but stronger than pure C11 release-acquire on some patterns — it guarantees ordering rules for "control dependencies" and RCU that C11 does not.

### C11 / C++11 memory model

**Hans Boehm + Sarita Adve — "Foundations of the C++ Concurrency Memory Model" PLDI 2008.** Boehm's formalization is the foundation of the C11/C++11 atomics — *data-race-free* programs behave as if SC.

Tools:
- **cppmem** (Mark Batty, Cambridge 2011) — interactive C11 model explorer.
- **herd7** (Alglave + Maranget) — handles both LKMM and C11, plus hardware (x86-TSO, ARMv8, Power, RISC-V).

### Differences that bite

- LKMM's "address dependency ordering" (the `DEP` relation from `rcu_dereference`) is **not** in C11. `memory_order_consume` tried to express it but no compiler implements it faithfully.
- C11 allows "out-of-thin-air" values in pathological relaxed scenarios that LKMM forbids by fiat.
- C11 requires `atomic_*` types for all shared access; LKMM allows plain-aligned access with `READ_ONCE`/`WRITE_ONCE` annotations.

### Relevance to ALZE

You probably don't need a formal model. But:
- Document **which model** your atomics satisfy. ALZE uses `__atomic_*` with SEQ_CST default → you are implicitly in C11 SEQ_CST land. That is unambiguous but slow.
- If you later introduce RCU-like patterns, understand that `__atomic_load_n(..., __ATOMIC_CONSUME)` in GCC silently becomes `__ATOMIC_ACQUIRE` — the `consume` order is not implementable in current compilers.
- For the current code size (~2500 LOC of sync), a **herd7 litmus suite** of 10–20 tests would catch the nested-lock-ordering bugs from R2 before they hit hardware. Overkill for v1; reasonable for v2.

---

## Synchronization primitives — comparison table

| Primitive | Reader cost | Writer cost | Fairness | Scalability | Memory | Typical kernel use |
|---|---|---|---|---|---|---|
| **TAS spinlock** | n/a | CAS + spin | none | poor (cache bounce) | 4 B | rare — legacy |
| **Ticket spinlock** | n/a | fetch-add + spin | FIFO | OK small N | 8 B | ALZE current, legacy Linux |
| **MCS spinlock** | n/a | CAS + local spin | FIFO | great | per-waiter node | basis for qspinlock |
| **qspinlock** | n/a | CAS fast path, MCS slow | FIFO | great (100+ CPU) | 4 B | Linux ≥4.2 default |
| **Sleeping mutex** | n/a | CAS + WQ sleep | FIFO | great (no spin) | ~64 B | VFS inode, process tree |
| **Adaptive mutex** | n/a | spin N then sleep | FIFO | great | ~64 B | Linux `mutex`, Solaris |
| **PI mutex** | n/a | CAS + PI walk | FIFO | good | ~128 B | rt_mutex, POSIX pthread |
| **rwlock (sleep)** | atomic inc | CAS + WQ | configurable | OK reads | ~48 B | legacy `rwsem` |
| **percpu-rwsem** | per-CPU inc | RCU sync + all-CPU | writer-starves | excellent reads | N×CPU B | `cgroup_rwsem`, `freeze_super` |
| **seqlock** | 2 loads + retry | CAS + seq++ | readers never block | excellent reads | 8 B | `jiffies`, `timekeeper` |
| **RCU (classic)** | 0 (empty macro) | pub + grace period | readers never block | excellent | per-obj old copy | dcache, FIB, task list |
| **SRCU** | per-srcu load | longer grace period | readers may sleep | good | per-srcu struct | tracepoint, notifier |
| **Hazard pointer** | store + fence | scan all HPs | per-pointer | good | K×thread slots | folly, C++26 std |
| **Epoch (EBR)** | store + fence | scan all epochs | per-section | good | bounded-if-no-straggler | crossbeam, liburcu-QSBR |
| **Futex** | atomic CAS (uncontended) | syscall on contention | configurable | excellent (user-space) | 4 B + kernel hash | glibc pthread, all user sync |

---

## ALZE applicability — v1 / v2 / v3

### v1 — today (single CPU, kernel-only threads, Big Kernel Lock ethos)

**Already in tree (review/scheduling_sync §Scope, 2509 LOC of sync):**
- Ticket spinlock with correct acquire/release (`spinlock.h`).
- Sleeping mutex with priority inheritance (`mutex.c`).
- Adaptive mutex spin-then-sleep (`adaptive_mutex.h`).
- Writer-preference rwlock (`rwlock.h`) — busy-wait, needs v2 fix.
- Wait-queue + counting semaphore (`waitqueue.c`, `semaphore.c`).
- Work-queue (single global, FIFO — `workqueue_def.c`, not driven).
- 64-entry ticket timer wheel (`ktimer.c`).
- GCC `__atomic_*` SEQ_CST defaults (`atomics.h`).

**v1 fixes required before v1 is "done" (from R2 review):**
1. **Unify lock ordering** — never call `sched_add_ready` while holding another lock (fixes Issues 3, 4 of scheduling_sync). Either stage wakeups to a post-unlock list, or drop outer lock before `sched_add_ready`.
2. **Fix `wq_wait_timeout`** (Issue 2) — either register a ktimer that calls `wq_wake_one` at the deadline, or link into `sleep_queue` in addition to the wq.
3. **Fix `task_exit` lock discipline** (Issue 3) — wake joiners before setting TASK_DEAD, drop `sched_lock` before `schedule()`.
4. **Fix `task_join`** (Issue 1) — lock-protected list of joiners on target, not single `join_wq` pointer.
5. **Fix `sched_tick` watchdog-kill** (Issue 5) — set `need_resched` + `should_kill` flag; let `schedule()` do the transition.
6. **VFS read-lock** (fs_storage Issue 1) — `vfs_read/write/ioctl/seek/tell` must `rwlock_read_lock` around the fd lookup.
7. **Per-task fd table** (fs_storage Issue 2) — move `fd_table[16]` into `struct task`. Until this is done, VFS + multitasking is unsound.

**v1 stance**: **Big-Kernel-Lock-ish** per subsystem. One spinlock per subsystem (pmm_lock, vmm_lock, sched_lock, vfs_rwlock). Document the global lock order in one header:
```
sched_lock > mutex.waiters.lock > wq.lock > timer_lock > kmalloc class lock > pmm_lock
vmm_lock is INDEPENDENT of the above chain (and always outer-most within MM)
vfs_rwlock is INDEPENDENT and always outermost within FS
```
Any cross-subsystem call must release all locks first.

### v2 — SMP-safe + modest scalability (AP startup, 2–8 cores)

Goals: turn on SMP without deadlocks in the first minute. Primitives to add or upgrade:

- **Keep ticket spinlock for now**. It is FIFO fair and correct; upgrade to qspinlock only once profiling shows the ticket's cache-bounce is the bottleneck (realistically not at ≤8 cores).
- **Per-CPU runqueues + per-CPU rq->lock**. Current `sched_lock` is global (review/scheduling_sync §SMP: "not load-balanced. One sched_lock, one prio_bitmap"). v2 moves to one runqueue per CPU, a per-CPU `rq->lock`, and a work-stealing load-balance tick. This alone eliminates the sched_lock as a scalability ceiling.
- **Per-CPU slab magazines** (kmalloc). `kmalloc_init` race (memory Issue 4) must be resolved pre-SMP. Add per-CPU free list of recent frees; drain to global slab under class lock on magazine full.
- **Per-zone free lists** (pmm). The struct exists (memory.md §PMM "zone_info"); populate. Makes `pmm_alloc_pages_zone` O(1).
- **rwlock → sleeping rwsem**. Busy-wait rwlock is a bug. Replace with rwsem backed by wait-queue, or use per-cpu-rwsem for read-mostly cases (VFS, module list).
- **seqlock for timekeeping**. Replace `pit_get_ticks`'s volatile read with a proper seqlock when moving beyond single-counter time.
- **Lock debugging ("lockdep-lite")**. Record `const char *name` + held-stack in each spinlock in DEBUG builds; enforce a total order at `spin_lock` time. Critical for catching lock-order regressions as the kernel grows.
- **Add `preempt_disable/enable` counters** per CPU; assert `preempt_count == 0` at entry to `schedule()` from voluntary paths; detect "sleep with spinlock held" bugs statically in tests.
- **TLB shootdown opcode split** (memory Issue 7) — full-flush vs single-addr, not overload target=0.
- **Atomics relaxation**. Audit hot paths and downgrade SEQ_CST to ACQUIRE/RELEASE where safe. On x86 SEQ_CST stores compile to locked ops; RELEASE stores compile to plain MOV. Potential 10–30× perf on uncontended paths.

**v2 is where ALZE starts to look like a real kernel.** Budget: ~1500–2000 LOC of new code + audit of existing.

### v3 — aspirational RCU + lock-free reader paths

Goal: scale to 16+ cores, become a research-grade kernel.

- **Add classic (Tree-)RCU infrastructure**:
  - Per-CPU quiescent-state counter.
  - Callback queue (`call_rcu(head, func)`).
  - `synchronize_rcu()` via scheduler hook — every CPU must context-switch.
  - Grace-period kernel thread (`rcu_gp_kthread`) that advances the grace-period epoch.
  - `rcu_read_lock()` / `rcu_read_unlock()` as `preempt_disable`/`preempt_enable` macros.
  - `rcu_assign_pointer` / `rcu_dereference` with appropriate barriers.
  - Estimated cost: **~2000 LOC of new code** + subtle ordering rules. Linux's implementation is ~8000 LOC of rcutree.c alone plus another 5000 of supporting code, but a minimal Tree-RCU for a 2-level hierarchy on ≤16 CPUs fits in ~2000.

- **RCU-protect read-heavy structures**:
  - **VFS device table** — currently under `vfs_rwlock` (fs_storage §VFS design). `vfs_lookup` is a hot path on every open. RCU-protect the list + refcount on vnode.
  - **Task list** — currently under `sched_lock`. Make task list RCU; readers (ps-style introspection) never block the scheduler.
  - **Network routing table** (when networking lands) — canonical RCU use case.
  - **Module list** (when modules land).

- **SRCU for paths that can sleep** — any RCU-protected traversal that might fault in a page (ext2 block read) must be SRCU, not classic RCU.

- **Seqlock everywhere read-mostly and write-rare**: time-of-day, load-average snapshots, statistics exports.

- **Consider qspinlock**: by v3 you have enough cores that ticket spinlock's cache-bounce matters.

- **Consider lock-free MPMC queue (Vyukov)** for the workqueue: the current single-global FIFO + spinlock is a choke point at 16+ producers. A bounded MPMC per-CPU WQ + global spill-over gives you Linux-style scalability.

- **Formal verification (herd7 litmus suite)**: 20–50 tests covering every publish/subscribe and wakeup pattern. Catches the next decade of memory-ordering regressions.

---

## Primary references

| Author(s) | Year | Title | Venue | URL |
|---|---|---|---|---|
| Paul McKenney | 2002 | Read-Copy Update | OLS 2002 | https://www.kernel.org/pub/linux/kernel/people/paulmck/RCU/rcu.2002.07.08.pdf |
| Paul McKenney | 2015+ | Is Parallel Programming Hard, And, If So, What Can You Do About It? | book, living doc | https://mirrors.edge.kernel.org/pub/linux/kernel/people/paulmck/perfbook/perfbook.html |
| Maged Michael | 2004 | Safe Memory Reclamation for Dynamic Lock-Free Objects Using Hazard Pointers | IEEE TPDS 15(6) | https://www.cs.otago.ac.nz/cosc440/readings/hazard-pointers.pdf |
| Keir Fraser | 2004 | Practical Lock-Freedom | Cambridge PhD thesis | https://www.cl.cam.ac.uk/techreports/UCAM-CL-TR-579.pdf |
| Maged Michael + Michael Scott | 1996 | Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms | PODC 1996 | https://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf |
| Tim Harris | 2001 | A Pragmatic Implementation of Non-Blocking Linked-Lists | DISC 2001 | https://timharris.uk/papers/2001-disc.pdf |
| Shalev + Shavit | 2006 | Split-Ordered Lists: Lock-Free Extensible Hash Tables | JACM 53(3) | https://people.csail.mit.edu/shanir/publications/Split-Ordered_Lists.pdf |
| Stern + Parri + McKenney | 2017 | A Formal Kernel Memory-Ordering Model (part 1, part 2) | LWN | https://lwn.net/Articles/718628/ , https://lwn.net/Articles/720550/ |
| Alglave + Maranget + McKenney + Parri + Stern | 2018 | Frightening Small Children and Disconcerting Grown-ups: Concurrency in the Linux Kernel | ASPLOS 2018 | https://dl.acm.org/doi/10.1145/3173162.3177156 |
| Ulrich Drepper | 2005 | Futexes Are Tricky | Red Hat tech report | https://akkadia.org/drepper/futex.pdf |
| Dmitry Vyukov | 2011+ | Bounded MPMC queue / 1024cores | blog | https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue (archive: https://web.archive.org/web/*/1024cores.net) |
| Hans Boehm + Sarita Adve | 2008 | Foundations of the C++ Concurrency Memory Model | PLDI 2008 | https://www.hpl.hp.com/techreports/2008/HPL-2008-56.pdf |
| Mellor-Crummey + Scott | 1991 | Algorithms for Scalable Synchronization on Shared-Memory Multiprocessors | ACM TOCS 9(1) | https://www.cs.rochester.edu/~scott/papers/1991_TOCS_synch.pdf |
| Zijlstra + Long | 2014 | qspinlock (Linux commit series) | LKML | https://lwn.net/Articles/590243/ |
| Franke + Russell + Kirkwood | 2002 | Fuss, Futexes and Furwocks: Fast Userlevel Locking in Linux | Ottawa Linux Symposium | https://www.kernel.org/doc/ols/2002/ols2002-pages-479-495.pdf |
| Mathieu Desnoyers | 2009 | Low-Impact Operating System Tracing / Userspace RCU | Polytechnique PhD | https://liburcu.org/ |
| Treiber | 1986 | Systems Programming: Coping with Parallelism | IBM RJ 5118 | (paywalled; summary: https://en.wikipedia.org/wiki/Treiber_stack) |

---

## Honest closing note — what ALZE should actually do

RCU is brilliant, but it is not a starter primitive. **A hobby kernel's first synchronization layer should be exactly what ALZE already has**: ticket spinlocks + sleeping mutex with PI + wait-queues + semaphores + a simple rwlock. ALZE got this right. The gap is not in *which primitives* exist, but in *lock ordering, SMP correctness, and FS integration*:

1. **Finish v1 first**. The five scheduler fixes (Issues 1/2/3/4/5 from review/scheduling_sync) and the two VFS fixes (fs_storage Issues 1/2) are **strictly higher priority** than any new primitive. None of them require new concurrency machinery — they require discipline and a documented global lock order. ALZE at single-CPU is already dangerous without these fixes.

2. **v2 unlocks SMP**. Per-CPU runqueues, per-CPU slab magazines, lock debugging, rwsem-with-sleep, seqlock for time. Budget this as ~2 months of focused work + a suite of concurrency stress tests (`test_pmm`-style for every subsystem). Do not turn on AP startup until v2 lands — "Two CPUs, opposite lock order" (review/scheduling_sync §Risk zones) will deadlock within seconds.

3. **RCU is a v3 aspiration, not a v1/v2 requirement**. Adding RCU means:
   - Building grace-period infrastructure (~2000 LOC).
   - Reasoning about two reclamation disciplines coexisting (spinlock-protected `kfree` vs `call_rcu(head, free)`) without footguns.
   - Memorizing the subtle rules: "no sleeping inside `rcu_read_lock`", "no `synchronize_rcu` while holding a mutex that a reader may try to take", "address-dependency ordering on Alpha doesn't matter here but `rcu_dereference` must be used anyway for future-proofing".
   - Accepting that a bug in the grace-period thread silently corrupts every RCU-protected structure.

   The payoff — zero-cost reader paths on VFS/task-list/route-table — is enormous for a 16+ core system. For a 2–4 core hobby kernel it is **not worth the complexity**. Profile first. Add RCU only when you can point at a benchmark showing the current reader scalability is the actual bottleneck.

4. **Don't skip the formal models, but don't over-invest**. A 20-test herd7 litmus suite covering your publish/subscribe patterns (wait-queue wakeup, mutex release, lock-free stats counters if any) takes an afternoon to write and catches memory-ordering regressions that hardware testing never will. Linux does this. crossbeam does this. ALZE can do this at any scale.

**Bottom line**: ALZE v1 is a solid single-CPU kernel with a good primitives inventory and a disciplined errors culture. The work for the next six months is fixing the lock-order bugs, making the scheduler SMP-safe, and fixing the VFS fd table. **Resist the temptation to add RCU as an engineering trophy.** It belongs after you've profiled and hit the ceiling of per-CPU runqueues + percpu-rwsem for your actual workload. Start with BKL-per-subsystem, move to fine-grained locks when measurements demand it, and treat RCU as the last 10× of reader scalability you pay for only when the previous 10× ran out.
