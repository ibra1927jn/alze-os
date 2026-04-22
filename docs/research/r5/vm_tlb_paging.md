# R5 — Virtual Memory, TLB, Demand Paging & Modern Extensions

Date: 2026-04-22 · Round: R5 deep-dive · Target project: ALZE OS (`/root/repos/alze-os`)
Input: R2 review (`review/memory.md`). Host-side `test_pmm` 29/29 PASS; 3 274 LOC across PMM/VMM/kmalloc/VMA/TLB/mempressure. R2 flagged 14 issues — notably #2 (`vmm_map_range_huge` never invalidates TLB), #7 (`tlb_shootdown_broadcast(0)` ≠ full flush, it `invlpg 0`), #8 (no OOM path — `KASSERT` in `get_or_create_entry` panics on OOM), #9 (huge-page guard page unmap is a silent no-op), #11 (no SMEP/SMAP).

Why this file: paging is where hobby kernels die. The textbook "set CR3 and you're done" works for boot; then demand paging + COW + swap + TLB shootdowns add complexity exponentially. This doc maps the modern VM toolbox so ALZE can pick the minimum viable set and refuse the rest.

---

## 1. x86_64 paging — 4-level and 5-level

### Intel SDM Vol 3A ch. 4 (paging) and AMD64 APM Vol 2 ch. 5

Canonical refs:

- Intel — *Intel® 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A: System Programming Guide* — ch. 4 (Paging) — https://cdrdv2.intel.com/v1/dl/getContent/671200 (archive: https://web.archive.org/web/2026*/intel.com/sdm).
- AMD — *AMD64 Architecture Programmer's Manual, Vol. 2: System Programming* — ch. 5 (Page Translation and Protection) — rev. 3.42 (2024) — https://www.amd.com/en/search/documentation/hub.html .
- Intel — *5-Level Paging and 5-Level EPT White Paper* — 2017 — https://www.intel.com/content/dam/develop/external/us/en/documents/5-level-paging-white-paper-467220.pdf (archive: https://web.archive.org/web/2018*/intel.com/content/dam/develop/external/us/en/documents/5-level-paging-white-paper-467220.pdf). Shipped in Icelake-SP (2019) and Sapphire Rapids (2023).

### 4-level paging (since AMD K8, 2003; Intel Nehalem, 2008)

48-bit canonical virtual address, sign-extended to 64 bits. The address is split:

```
63........48 47......39 38......30 29......21 20......12 11......0
sign-extend  PML4 idx   PDPT idx   PD idx     PT idx     offset
(must match   9 bits     9 bits     9 bits     9 bits     12 bits
 bit 47)
```

Four levels of radix tree with 512 64-bit entries per 4 KB table. `CR3` physical address → PML4. Each step indexes with 9 bits. Each PTE holds PFN (40 bits) + flags (P, RW, US, PWT, PCD, A, D, PS, G, NX, protection-keys).

Intermediate entries can be **leaves** by setting `PS=1`:
- PDPT-level leaf → **1 GiB huge page** (since AMD Barcelona, 2007; Intel Westmere, 2010).
- PD-level leaf → **2 MiB huge page** (since original PAE, 1995).
- PT-level entry → always **4 KiB page**.

### 5-level paging (Icelake-SP, 2019)

57-bit canonical VA, 128 PiB user space. Adds PML5 on top. Gated by `CR4.LA57` + `IA32_EFER.LME` + long mode. Kernel must enable during boot if `CPUID.(EAX=7,ECX=0):ECX[bit 16]` (LA57) is set AND the firmware/bootloader did not already. Cost: one extra memory access per TLB miss (5 cache refs instead of 4); mitigated by PWC (page-walk cache) and LAPIC counters show <2 % throughput hit on typical workloads.

Linux support: `CONFIG_X86_5LEVEL` (v4.12, 2017) with run-time detection via `no5lvl` boot param to disable; Linux picks 4-level even on LA57 CPUs unless VA space > 56 bits is actually requested — keeps kernel addresses compatible with 4-level user processes.

### Page sizes in x86_64 — TLB + setup table

| Size | Where the leaf lives | TLB entries (Skylake iTLB/dTLB) | Typical use | Setup cost |
|------|----------------------|----------------------------------|-------------|------------|
| **4 KiB** | PT leaf | 64 iTLB + 64 dTLB (L1), 1536 STLB | User pages, fine-grained mprotect | 1 PT node = 4 KiB; full 4 KiB window = 1 PTE |
| **2 MiB** | PD leaf (`PS=1`) | 32 L1 dTLB slots (Skylake-SP), 1024 STLB | Kernel ID map, hugetlbfs, THP | Skip PT level — save 4 KiB + 8 PTE writes per 2 MiB |
| **1 GiB** | PDPT leaf (`PS=1`) | 4 L1 dTLB slots only — small pool | ID-mapping huge machines, DPDK hugepages | Skip PT + PD — save ~2 MiB of page-table memory per 1 GiB |

Skylake-SP has **dedicated** 2 MiB slots separate from 4 KiB ones, so covering both the kernel ID map (2 MiB) and user text (4 KiB) does not evict each other — explicitly designed to support mixed sizes.

Rule of thumb: **1 GiB leaves are gold for the kernel direct map**: one TLB entry covers 1 GiB, almost never evicted. This is the primary perf knob for kernel-heavy workloads (network packet processing, NVMe IO).

### Huge pages in kernel ID map — the "HHDM" pattern

Limine bootloader (used by ALZE — see `vmm.c:505`) provides the **Higher-Half Direct Map**: every physical byte is mapped into kernel VA `0xFFFF_8000_0000_0000 + phys`. Standard approach is to map HHDM with 2 MiB (or 1 GiB) huge pages; kernel pointer arithmetic `PHYS2VIRT(p) = p + 0xFFFF800000000000` gives O(1) phys→virt with zero TLB pressure.

ALZE currently uses **2 MiB** HHDM only (vmm.c:520). Promoting HHDM to **1 GiB** on machines with `CPUID.AMD.ExtFeature.Page1GB=1` (all post-2010 CPUs) is one PD→PDPT flag flip and saves ~500 TLB entries.

---

## 2. TLB + INVLPG — the hardest part of paging

### Anatomy

Every CPU core has its **own** TLB (per-core; hyperthreads **share**, which mattered for L1TF/MDS — post-Spectre some TLB entries are flushed on HT context switch). Structure: L1 iTLB (code) + L1 dTLB (data) + unified L2 STLB. Also **page-walk cache** (PWC) caches PML4/PDPT/PD entries to skip levels on near-miss.

### `INVLPG <linear-addr>` (Intel 80486+)

Invalidates the TLB entry for one VA on the *local* CPU. Flushes all sizes mapping that VA (so `invlpg` at a 2 MiB boundary flushes the 2 MiB entry). Does **not** cross CPUs.

Full-flush primitives:

- `MOV CR3, <phys>` with unchanged value → flushes **all non-global** entries on local CPU.
- `CR4.PGE` toggle (clear then set) → flushes **all** entries including global.
- `INVPCID` (Haswell, 2013) — targeted flush by PCID; 4 types (individual addr / all PCIDs addr / all PCIDs all / all-but-global).

### Global pages — `CR4.PGE=1` + `PTE.G=1`

`CR4.PGE` (Pentium Pro, 1995) enables the global bit. PTE bit 8 (`G`) marks entries that survive `MOV CR3`. Intended for **kernel pages** — they don't change on context switch. Without PGE, every `MOV CR3` (process switch) would nuke kernel TLB entries → kernel cache-cold on every syscall → 10-30 % overhead.

Caveat: PTI (see below) mostly **defeats** PGE for user pages because PTI switches CR3 on every syscall.

### PCID (Process Context Identifier) — x86, Westmere 2010

12-bit tag in low bits of CR3 (`CR3[11:0] = PCID`) + `CR4.PCIDE=1`. Each TLB entry is tagged with PCID; `MOV CR3 (new-PCID)` does **not** flush — the new process's TLB entries coexist with the previous. Workloads with frequent context switches see 5-15 % wins.

- **4 096 possible PCIDs**, but Linux recycles only ~6 per-CPU (`TLB_NR_DYN_ASIDS=6`) — LRU eviction with explicit `INVPCID`.
- On PTI-enabled kernels, PCID is split: each process has **two** PCIDs — one for user mode (PTI table) and one for kernel mode (full table).

### ASID (Address Space Identifier) — Arm equivalent

ARMv8 TTBR0_EL1 / TTBR1_EL1 carry an ASID (8 or 16 bits). Arm has always had ASIDs (pre-dates Intel PCID by ~20 years; ARM920T 2001). Same idea — tag TLB entries.

### TLB shootdown IPI

When a kernel thread modifies a page table entry, **every CPU that might have cached the old translation** must invalidate. There is no hardware coherence for TLBs. Protocol:

1. Writer CPU holds the page-table lock, modifies PTE.
2. Writer computes CPU mask (who has this mm loaded — `mm->cpu_vm_mask_var`).
3. Writer sends IPI (inter-processor interrupt) to each target CPU.
4. Targets interrupt, execute `invlpg` (or full flush), ACK.
5. Writer waits for all ACKs, releases lock.

Cost: IPI round-trip is ~1-3 µs, so modifying 1 page on 64 cores costs ~100 µs of CPU × 64 = 6 ms aggregate. **Linux batches shootdowns** via `flush_tlb_batched_pending()` — coalesces many `invlpg` into one IPI that reloads CR3 (cheaper than many individual `invlpg` above some threshold, typically 33 pages on Intel per Kleen).

### ALZE issue (from R2 review memory.md #7)

`vmm_tlb_batch_flush` calls `tlb_shootdown_broadcast(0)` as its "full flush" — but receivers execute `invlpg 0`, which only flushes PFN 0's TLB entry. Not a full flush. **Fix**: extend the shootdown state with a `full_flush` flag → receivers reload CR3 instead of `invlpg`.

### PTI (Page Table Isolation) — Meltdown mitigation, Jan 2018

Papers:
- Lipp, Schwarz, Gruss et al. — *Meltdown: Reading Kernel Memory from User Space* — USENIX Security 2018 — https://meltdownattack.com/meltdown.pdf (archive https://web.archive.org/web/2018*/meltdown.pdf).
- Gruss et al. — *KAISER: Kernel Address Isolation to have Side-channels Efficiently Removed* — ESSoS 2017 (the pre-Meltdown version) — https://gruss.cc/files/kaiser.pdf.

PTI splits every process's page tables in two:
- **User page table**: user mappings + a tiny kernel stub (entry trampolines, IDT, per-CPU data for syscall dispatch).
- **Kernel page table**: full kernel mapping.

Every syscall/IRQ swaps CR3 twice — once on entry (load kernel CR3), once on exit (restore user CR3). Cost: ~200 cycles per transition + TLB pressure because user PCID is flushed on exit if PCID missing. Overhead: 0.5 % (compute-bound) to 30 % (syscall-heavy like Redis `set`).

Without PTI, Meltdown can read any kernel byte from userspace. Post-Meltdown CPUs (Cascade Lake 2019+, AMD from Zen) set `CPUID.ARCH_CAPABILITIES.RDCL_NO=1` → PTI disabled by default.

ALZE has no userspace yet → PTI does not apply. If/when ALZE v2 lands user mode, the model is: on Meltdown-affected CPUs, use two PML4s per process; otherwise one.

---

## 3. Demand paging

### Classical scheme

Page tables are populated **lazily**: a VA has a VMA (metadata: permissions, backing object, offset) but the PTE is not-present. First access → #PF → page-fault handler consults VMA → allocates page / reads from file / zeroes / CoW-copies → installs PTE → restarts instruction.

- **Minor fault**: no I/O needed — page already in memory (e.g. CoW share, first touch of anonymous mem → zero page, mmap of already-cached file). Cost: ~1 µs.
- **Major fault**: disk I/O needed — read page from file or swap. Cost: ~10 ms HDD, ~100 µs NVMe. `/proc/<pid>/stat` field `majflt` counts these; Linux's `perf stat` reports them.

Bovet+Cesati — *Understanding the Linux Kernel, 3rd ed.* — O'Reilly 2005 — chapter 9 "Process Address Space" is still canonical even in 2026; the data structures (vm_area_struct, mm_struct) are unchanged.

### Copy-on-Write (CoW) fork()

When `fork()`:
1. Child inherits parent's mm with **shared** PTEs marked **read-only**.
2. Any write → #PF → handler sees the VMA is writable but PTE is RO → allocates new page, copies, installs RW PTE in child (and parent if refcount > 1).
3. Refcount on each physical page tracks how many mms point to it.

CoW is **~3000 LOC of kernel code** to get right (fork.c, memory.c, huge_memory.c). Pitfalls:
- Race on concurrent write (two threads fault same page) — solved with page lock + recheck.
- CoW on huge pages — split first, then CoW child 4 KB page, keep parent's huge page if possible.
- `vfork()` skip — shares address space, child must `exec` or `exit` promptly.
- `madvise(MADV_WIPEONFORK)` (Linux 4.14, 2017) — zero on CoW instead of copying, for security.

**ALZE decision** (from R4 Fuchsia/Zircon review): `process_create` takes an explicit `elf_blob + vmar` spec, no `fork()`. This cuts ~3 000 LOC and avoids the historical unix fork/exec correctness footguns.

### mmap() + page cache + file-backed pages

Every file page lives in the **page cache** — a global radix tree `inode → offset → struct page`. `mmap(MAP_SHARED, fd)` installs page-cache pages directly into the process's PTEs; writes go through the normal write-back mechanism; `msync()` flushes to disk. Cost model: one physical page backs ALL mmaps of that file+offset across all processes — huge memory savings for shared libs (libc.so.6 is mapped into every process from one set of PFNs).

`MAP_PRIVATE` adds a CoW layer on top: initial pages shared with page cache, first write CoWs.

### Swap — still relevant in 2026?

**Yes, but narrowly.** With 32-128 GB desktop RAM + NVMe, swap is no longer the latency disaster it was. Modern use cases:

- **zswap** / **zram** — compressed in-RAM swap, not backed by disk. Gives ~2-3× effective RAM at the cost of CPU. Chromebooks use zram heavily. Android uses zram as primary swap.
- **Hibernation** (`suspend-to-disk`) — writes entire RAM to a swap partition on power-off.
- **Cgroup memory limits** — a container hitting its memory cap swaps to its private swap tier before OOM.
- **Kernel same-page merging** (KSM, Linux 2.6.32, 2009) — deduplicates identical pages; found swap can un-dedupe.

Jens Axboe's io_uring work (Linux 5.1, 2019) meant swap IO finally uses async DMA paths — no more bouncing through block layer queue heads. But: for a 128 GB desktop running VMs, the practical truth is *swap off* — the OOM killer is preferable to swap-death grinds.

**ALZE decision**: no swap in v1/v2. Swap is a second 3-5 kLOC subsystem (storage integration, IO scheduler, reclaim heuristics). Reject until v4+.

---

## 4. Huge pages on Linux — hugetlbfs vs THP

### hugetlbfs (explicit) — since Linux 2.6.0 (2003)

Reserved pool at boot: `hugepages=N` kernel cmdline or `/proc/sys/vm/nr_hugepages` runtime. Allocated via `mmap(MAP_HUGETLB)` or from `hugetlbfs` mount. Pages are **locked** (never paged out, never split). Typical use: databases (Oracle SGA), DPDK packet pools, JVM `-XX:+UseLargePages`.

Problem: **fragmentation** — contiguous 2 MiB / 1 GiB physical regions must exist; on a long-running system the buddy allocator runs out.

### Transparent Huge Pages (THP) — Linux 2.6.38 (2011)

Andrea Arcangeli — *Transparent Huge Pages in Linux* — https://lwn.net/Articles/423584/ (archive https://web.archive.org/web/2011*/lwn.net/Articles/423584/).

Automatic: for any anonymous mmap region ≥ 2 MiB, the kernel tries to allocate **one** 2 MiB huge page. If it fails → falls back to 4 KiB pages + **khugepaged** kernel thread scans periodically and **promotes** contiguous 4 KiB runs into 2 MiB huge pages via memory compaction.

Modes via `/sys/kernel/mm/transparent_hugepage/enabled`:
- `always` — all anon mappings get THP. Maximum perf, maximum memory waste (1.9 MiB padding for a 1 KiB anon alloc).
- `madvise` (default post-2018) — only `MADV_HUGEPAGE` regions. Most DBs use this.
- `never` — THP off.

Perf: 5-15 % speedup on memory-intensive code (Redis, PostgreSQL seqscan, numerical kernels). Downside: **khugepaged stalls** under memory pressure because compaction holds mmap_sem; high-tail-latency workloads (Google Search, Facebook memcached) disable THP.

**ALZE applicability**: v2 should ship explicit huge-page support for kernel data structures (e.g. buddy metadata array). THP's auto-promotion machinery is 5+ kLOC and requires a working VMA + reclaim path first — skip until post-MVP.

---

## 5. memcg — cgroups memory controller v2

Tejun Heo — *Control Group v2* — Linux Documentation/admin-guide/cgroup-v2.rst — https://docs.kernel.org/admin-guide/cgroup-v2.html . Landed v4.5 (2016). v2 deprecates the v1 per-controller hierarchies.

Per-cgroup counters:
- `memory.current` — bytes in use
- `memory.max` — hard limit; OOM above
- `memory.high` — soft limit; reclaim starts above, processes throttled via sleep
- `memory.low` — protected minimum; reclaim skips if cgroup is below

Accounting: every allocation (page cache, anon, slab since v4.15 with `memcg_kmem`) is charged to the current task's memcg. Percpu counters aggregated lazily; dirty memory tracked for writeback throttling.

### Pressure Stall Information (PSI) — Johannes Weiner, Linux 4.20 (2018)

`/proc/pressure/memory` — shows percent of wallclock time at least one task is stalled on memory. Three scopes: `some` (any task), `full` (all tasks). Windows: 10s, 60s, 300s.

Key innovation over "is memory full?": PSI measures **pressure** = time-spent-waiting. A system with 1 MB free is OK if no one is waiting; a system with 10 GB free is broken if everyone is swap-thrashing. PSI is the input signal for Android's LMK, Facebook's oomd, systemd-oomd.

Weiner — *Pressure Stall Information* — https://facebookmicrosites.github.io/psi/ (archive https://web.archive.org/web/2019*/facebookmicrosites.github.io/psi/).

### OOM reaper integration

Cgroup v2 has `memory.oom.group=1` — whole cgroup is OOM-killed as a unit (good for containers). Paired with OOM reaper (Michal Hocko, Linux 4.6, 2016): after kernel sends SIGKILL, a kthread `oom_reaper` asynchronously reaps the victim's address space without waiting for the victim to actually exit — bypasses exit deadlocks on mmap_sem.

---

## 6. Swap compression — zswap and zram

### zswap (Seth Jennings, Linux 3.11, 2013)

Compressed **cache** in front of regular swap. Allocation path on swap-out:
1. Page to be swapped → compress with LZO / LZ4 / ZSTD.
2. Store compressed blob in a small in-RAM region (default 20 % of RAM).
3. If the blob pool fills → evict oldest to disk-backed swap.

Typical compression ratio 2:1 → doubles effective swap capacity for ~5 % CPU overhead. Standard on Ubuntu, Fedora since ~2019.

### zram (Nitin Gupta, Linux 3.14, 2014)

Creates a **block device** (`/dev/zram0`) backed by compressed RAM. Used by ChromeOS, Android, Fedora 34+ **as the only swap device** (no disk swap). Typical Android 8 GB phone: 4 GB zram, ~6 GB effective via LZ4-RLE.

Difference from zswap: zswap is a *cache tier* before another swap device; zram is a *standalone* swap backing. Both use the same compression APIs (crypto API).

### Use cases 2026

- Embedded / mobile — always zram.
- Containerized cloud — zswap for general purpose; enables cgroup swap limits.
- Desktop/server — trend is disable swap entirely on 64 GB+ machines; tiny zram for hibernate-resume footprint.

### ALZE applicability

Zram/zswap share the swap page-out machinery — without swap, no zram/zswap. Reject v1/v2; aspirational v4+.

---

## 7. OOM killer — choosing the victim

Classic heuristic (`mm/oom_kill.c`):
```
oom_score = RSS_pages + swap_pages + 1/3 * page_tables
oom_score *= (1000 - oom_score_adj) / 1000
victim = task with max oom_score
```

`oom_score_adj` is a per-task knob `[-1000, +1000]` exposed at `/proc/<pid>/oom_score_adj`. Value `-1000` makes the task immune; `+1000` makes it the first victim. systemd sets `OOMScoreAdjust=-900` for PID 1 and core services.

Problems with in-kernel OOM:
- Triggers **too late** — by the time kernel detects "no reclaim progress for N iterations", the system is already thrashing for tens of seconds. User sees freeze.
- Picks wrong victim — biggest RSS isn't always the one causing pressure.

### Userspace OOM killers (PSI-driven)

- **Facebook oomd** — https://github.com/facebookincubator/oomd — reads `/proc/pressure/memory`, kills based on configured thresholds. Deployed on Facebook fleet since 2018.
- **systemd-oomd** — ships with systemd 247+ (2020). Default on Fedora 34+.
- **earlyoom** — simple, watches `MemAvailable` + swap, sends SIGTERM / SIGKILL before kernel OOM.

Rule: userspace OOM kills proactively at ~10 % pressure; kernel OOM is last resort at ~0 % free.

### FreeBSD — page daemon approach

FreeBSD has a dedicated `pageout` kthread (the "page daemon") that sweeps PTE access bits + two-handed clock. Kill decision uses `v_free_target` / `v_free_min` watermarks rather than per-process scoring; victim chosen by largest RSS among recently-active processes. McKusick + Neville-Neil — *The Design and Implementation of the FreeBSD Operating System, 2nd ed.* — Addison-Wesley 2014 — ch. 6.

Difference: FreeBSD integrates reclaim and OOM into one daemon, Linux splits them (kswapd + oom_kill.c).

### ALZE

R2 memory.md issue #8: no OOM policy, `KASSERT` panics on failed page-table alloc. **v1 fix**: add emergency reserve (16 pre-allocated 4 KiB pages for page-table nodes), propagate failure through `get_or_create_entry` return, unwind partial mappings. **v2 fix**: wire at least one `mempressure_register` callback (even a stub that logs stats). No full OOM killer until v3+.

---

## 8. Slab allocators — SLAB → SLUB → SLOB → SLUB

Kernel small-object allocators have had an identity crisis.

### SLAB (Jeff Bonwick, Solaris 2.4, 1994 → Linux 2.2, 1999)

- Bonwick — *The Slab Allocator: An Object-Caching Kernel Memory Allocator* — USENIX Summer 1994 — https://people.eecs.berkeley.edu/~kubitron/courses/cs194-24-S13/hand-outs/bonwick_slab.pdf .
- Bonwick + Adams — *Magazines and Vmem: Extending the Slab Allocator to Many CPUs and Arbitrary Resources* — USENIX 2001 — https://www.usenix.org/legacy/publications/library/proceedings/usenix01/full_papers/bonwick/bonwick.pdf .

Key ideas:
- **Cache of identical objects** (e.g. `struct task_struct`). Each cache has per-size slabs of full pages carved into slots.
- **Constructor/destructor** — amortize `init`/`cleanup` across reuses (e.g. initialize a mutex once, reuse object).
- **Magazines** — per-CPU free lists; allocations never touch global lock in common case.
- **Coloring** — stagger object start offset per slab to spread cache line usage across L1 ways.

Linux SLAB grew baroque: per-NUMA-node lists, shared lists, complex refill paths.

### SLUB (Christoph Lameter, Linux 2.6.22, 2007)

Lameter — *The SLUB allocator* — LWN.net, 2007 — https://lwn.net/Articles/229984/ .

Goals: simplicity + better perf on many-core. Key differences:
- **No constructors/destructors** (never mattered in practice).
- **Per-CPU slab pointer** only (no per-node lists by default).
- **Merging** — caches with same size class + flags are merged (`dentry` and `inode_cache` share if sizes match) → fewer slabs, better density.
- **Object free → just CMPXCHG** on the slab's free-list head; allocations are 2-4 instructions on fast path.

SLUB won: since Linux 5.19 (2022) it is the only option; SLAB and SLOB removed in 6.5 (2023).

### SLOB (Matt Mackall, Linux 2.6.16, 2006)

Tiny allocator for embedded (< 32 MB RAM). First-fit free list, ~500 LOC. Removed 2023 — nobody was using it; embedded Linux moved to SLUB + SL_TINY.

### kmem_cache_create / kmem_cache_alloc (Linux API)

```c
kmem_cache_t *task_cache = kmem_cache_create(
    "task_struct", sizeof(struct task_struct),
    0,                        // align (0 = cache-line)
    SLAB_HWCACHE_ALIGN | SLAB_PANIC,
    NULL                      // ctor (nullable)
);

struct task_struct *t = kmem_cache_alloc(task_cache, GFP_KERNEL);
// ... use ...
kmem_cache_free(task_cache, t);
```

Per-CPU cache ("magazine") holds ~30 free objects — refill from global partial-slab list when empty. Alloc fast-path is lockless via `this_cpu_cmpxchg`.

### ALZE kmalloc

Current (kmalloc.c:478 LOC): 8 classes {16, 32, 64, 128, 256, 512, 1024, 2048}, slab = one 4 KiB page with 32 B header + packed slots, intrusive free-list, 32-entry quarantine ring for double-free detection, OpenBSD-style `0xDE` poison, large-alloc fallback to buddy allocator. **No per-CPU magazines** (pre-SMP single lock).

R2 review issues: #3 (no redzones between slots → driver overflow is invisible), #4 (lazy `initialized` flag racy under SMP), and 32 B SIMD-align not guaranteed for 16-byte class. These are correctness bugs that scale with LOC growth — fix in v1 before SMP lands. Per-CPU magazines are v2.

---

## 9. vmalloc / vzalloc — non-contiguous kernel memory

### The problem

`kmalloc` returns **physically contiguous** memory (necessary for DMA, page-table install) but size is bounded by buddy max order (4 MiB on ALZE, 8 MiB on Linux default). Large allocations (e.g. 32 MiB kernel buffer) cannot be satisfied without contiguous physical memory, which is rare on long-lived systems.

### vmalloc (since Linux 0.12, 1992)

Allocates **virtually contiguous, physically non-contiguous** pages. Internally:
1. Reserve VA range in vmalloc area (`0xFFFFC9000_0000_0000` on x86_64, 32 TiB).
2. Alloc N individual physical pages from buddy (order 0 each).
3. Map them into the VA range consecutively.
4. Flush TLB.

Cost: one TLB entry per 4 KiB (no huge pages historically → TLB hostile). Reading a 32 MiB vmalloc buffer sequentially → 8 192 TLB misses. Linux 5.15 (2021) added `vmalloc` with huge-page backing (`VM_ALLOW_HUGE_VMAP` flag) for non-user-accessible kernel memory.

`vzalloc` = `vmalloc` + memset-zero.

### Shadow pages for __GFP_ZERO

`vzalloc` / `__GFP_ZERO` memsets pages at alloc time. Some subsystems need the zeroing guarantee but want lazy. Solution: **shadow pages** — unmapped guard/zero pages that map to a single physical zero page; write faults trigger on-demand physical allocation. Used by KASAN, CoW zero pages.

### ZSTD-compressed vmalloc (experimental, ~2023)

Nick Terrell et al. at Meta proposed compressed vmalloc — large rarely-accessed kernel buffers stored compressed, decompressed on access. Prototyped for kernel ftrace buffers; not mainlined. Saves 30-60 % of kernel RAM on idle nodes.

### ALZE

No vmalloc equivalent currently. The buddy 4 MiB max is fine for kernel v1 needs (page cache is not yet a thing; no large kernel buffers). v2 should add `vmalloc`-like for the eventual page cache and DMA scatter-gather buffers.

---

## 10. Modern pointer extensions — LAM, MTE, PAC

### Intel LAM (Linear Address Masking) — 2023 (Sapphire Rapids+)

Intel — *Linear Address Masking Architecture Specification* — 2022 — https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/best-practices/linear-address-masking.html .

Idea: **ignore top bits of pointer** during translation — use them as software metadata. User VAs are 48-bit (4-level) or 57-bit (5-level); LAM lets user software stash a 7-bit tag (LAM48) or 6-bit tag (LAM57) in the unused top bits of a 64-bit pointer. Kernel sets `CR3.LAM_U48` / `CR3.LAM_U57` and untagging happens transparently.

Use cases:
- **HWASAN** (hardware-assisted AddressSanitizer) — the tag encodes the "expected color"; load/store checks color vs shadow memory.
- **Sandbox / JIT** — tag identifies trust domain of the pointer.
- **GC** — generational tag or forwarding bit in pointer.

### Arm MTE (Memory Tagging Extension) — ARMv8.5-A, 2019; Pixel 8 2023

Arm — *Armv8.5-A Memory Tagging Extension, White Paper* — 2019 — https://developer.arm.com/documentation/102925/ .

Stronger than LAM — hardware **checks** the tag. Every 16-byte memory block has a 4-bit hardware tag stored in separate memory (tag memory). Every allocation is tagged (4 bits in pointer top, 4 bits in memory metadata). Load/store hardware compares; mismatch → SIGILL (sync mode) or log (async mode).

Use cases:
- **glibc MTE** — `malloc` tags allocations, `free` re-tags with new value → UAF is a hardware fault.
- **Kernel MTE** (since Linux 5.10, 2020) — `kasan=on + kasan.mode=hw_tags` — slab objects tagged; overflows + UAF caught at hardware speed (≈2-5 % overhead vs ~3× for software KASAN).

Requirements: ARMv8.5+ (Cortex-X2, Apple M3, Pixel 8). x86 has no equivalent — LAM is the closest but is *tag* not *check*.

### Arm PAC (Pointer Authentication) — ARMv8.3-A, 2017; Apple A12 2018

Arm — *Pointer Authentication on ARMv8.3* — 2017 — https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/pointer-auth-v7.pdf .

ROP/JOP defense. Pointers stored in memory are signed with a 64-bit-QARMA PAC embedded in unused top bits. `PACIA` instruction signs; `AUTIA` verifies. Key stored in `SCTLR_EL1` (kernel-visible). When a pointer is loaded for use (return from call, indirect branch), hardware verifies; mismatch → bit tampering detected.

Saves ROP gadgets: the attacker overwrites a return address in memory; on `ret`, `AUTIASP` fails because PAC is wrong; fault.

Used by:
- iOS/macOS — all indirect branches + return addresses signed since Apple A12 (iPhone XS, 2018).
- Linux aarch64 kernel — `CONFIG_ARM64_PTR_AUTH` (v5.0, 2019).
- Android 12+.

### Summary table — VM features modern

| Feature | Vendor | ISA | Introduced | Purpose |
|---------|--------|-----|------------|---------|
| PCID | Intel | x86_64 | Westmere, 2010 (CPUID) | TLB ASID — avoid flush on CR3 load |
| ASID | Arm | ARMv6+ | ~2001 | TLB ASID — avoid flush on TTBR switch |
| PGE (global) | Intel | i586 | Pentium Pro, 1995 | Kernel TLB entries survive CR3 reload |
| LAM | Intel | x86_64 | Sapphire Rapids, 2023 | Pointer tagging (software-only use) |
| MTE | Arm | ARMv8.5-A | 2019 (Pixel 8, 2023) | Hardware-checked memory tags → UAF/overflow caught |
| PAC | Arm | ARMv8.3-A | 2017 (Apple A12, 2018) | Signed pointers → ROP/JOP mitigation |
| 5-level paging | Intel + AMD | x86_64 | Icelake-SP, 2019 | 57-bit VA (128 PiB user) |
| SMEP | Intel | x86_64 | Ivy Bridge, 2012 | Ring-0 cannot execute user pages |
| SMAP | Intel | x86_64 | Broadwell, 2014 | Ring-0 accidental user-memory access traps |
| PTI | software | any | Jan 2018 (Meltdown) | Split user/kernel page tables |

---

## 11. Memory ordering + page attribute tables (PAT)

### PAT — Intel PAT, 1998 (P6)

Page-Attribute Table: 8 entries, each a 3-bit memory type (UC, UC-, WC, WT, WP, WB, reserved, reserved). PTE bits `PWT`, `PCD`, `PAT` index into PAT to pick the type for a page.

Types:
- **WB** (write-back) — normal cacheable memory, coherent. Default for regular RAM.
- **WC** (write-combining) — write bursts combined in write-combining buffer before hitting bus; no caching on reads. **Framebuffers** → 5-10× framebuffer throughput on VGA/graphics MMIO vs UC.
- **UC** (uncacheable) — every access goes to bus. Strict ordering. **MMIO registers** (PCI BAR for device control).
- **UC-** (weak UC) — same as UC but overridable by MTRR to WC/WT.
- **WT** (write-through) — writes go to cache AND memory. Rare; used for ROM-shadow.
- **WP** (write-protect) — reads cache, writes uncached. Also rare.

### MMIO pages need UC

Device register at `0xFED0_0000` (HPET): the CPU **must not** cache loads/stores, and writes **must** go through in program order. PTE for that page → `PAT_UC`. Accessing as WB would let the CPU reorder a "start DMA" write past a "configure DMA source" write → silent corruption.

### WC for framebuffers

Linear framebuffer at `0xE000_0000` (Intel integrated GPU BAR): software writes many bytes consecutively (drawing a row of pixels), reads rarely. WC coalesces 64-byte cache-line bursts into one bus write → ~8× throughput vs UC.

### Memory barriers around page-table updates

After writing a PTE, software **must** ensure the write is globally visible before issuing `invlpg` or returning from the page-fault handler. On x86 (TSO), **normal store → store ordering is preserved**, but you still need `invlpg` itself to flush the TLB; the HW will re-walk after invlpg. On Arm (weak ordering), you need `DSB ISH` between PTE store and `TLBI` instruction, then `DSB ISH` again between `TLBI` and the next instruction that might use the translation, then `ISB` if that instruction is on the same core.

Linux has `flush_tlb_fix_spurious_fault` for the pattern: after a "make writable" page-fault, the **other CPU** might still see the old read-only PTE in its TLB → spurious re-faults. Solution: check if the PTE is correct; if so, just re-execute (the eventual TLB refresh will fix it).

---

## 12. Page-table self-referencing — hobby kernel trick

### The trick

Reserve one slot (e.g., PML4[511]) and point it **at the PML4 itself**. Then VAs constructed from that slot index walk the page tables themselves as if they were data.

```c
pml4[511] = pml4_phys | PRESENT | WRITE;
// now VA 0xFFFF_FF80_0000_0000 + x maps as if PML4 were a PT
```

Effect: any page table at any level is accessible via a crafted VA without ever allocating a temporary mapping.

- VA `0xFFFF_FF80_0000_0000` → PML4 itself (as a leaf "page", 512 entries of 8 B = 4 KiB).
- VA `0xFFFF_FFFF_C000_0000 + pml4_idx * 8` → PDPT corresponding to `pml4_idx`.
- VA `0xFFFF_FFFF_FFFF_E000 + pml4_idx * 64 + pdpt_idx * 8` → PD corresponding to those indices.

Used by:
- **Linux x86_32** historically (pre-x86_64, ~2004) for `vmalloc` region walking.
- **Many hobby kernels** (toaruOS, MINIX, SerenityOS in early versions).
- **xv6-x86_64** — uses a simpler fixed-mapping approach instead.

### Why ALZE doesn't use it

HHDM (Higher-Half Direct Map via Limine) gives `phys → virt` via `PHYS2VIRT(p) = p + 0xFFFF800000000000` for ALL physical memory. Walking page tables is just `pml4_virt = PHYS2VIRT(cr3_phys); entry = pml4_virt[PML4_INDEX(va)];` — straightforward, no recursive-slot magic. R2 review memory.md confirms: *"No 5-level, no recursive mapping: uses the HHDM linear map to access tables (`PHYS2VIRT` everywhere). This is simpler and avoids recursive-slot headaches."*

Keep it that way. Recursive mapping becomes painful when:
- You need to read the PML4 of **another** address space.
- You need to unmap the recursive slot itself.
- You 5-level-page — now you need to recurse one deeper.

HHDM is strictly better when the bootloader provides it.

---

## 13. Canonical references

- **Intel SDM Vol 3A**, ch. 4 — https://cdrdv2.intel.com/v1/dl/getContent/671200 (archive https://web.archive.org/web/2026*/intel.com/sdm).
- **AMD64 APM Vol 2**, ch. 5 — rev 3.42 (2024) — https://www.amd.com/en/search/documentation/hub.html .
- **Bovet + Cesati** — *Understanding the Linux Kernel, 3rd ed.* — O'Reilly, 2005. ch. 8 (Memory Management), ch. 9 (Process Address Space). ISBN 978-0596005658. Still canonical for the core data structures.
- **Mel Gorman** — *Understanding the Linux Virtual Memory Manager* — Prentice Hall, 2004 — https://www.kernel.org/doc/gorman/ . Dated (pre-THP, pre-memcg) but foundational for buddy / slab / rmap.
- **Andi Kleen** — *Scaling Linux to 1000 cores* — various LWN + talks; canonical for TLB shootdown batching — https://lwn.net/Articles/162369/ (archive https://web.archive.org/web/2006*/lwn.net/Articles/162369/).
- **Jens Axboe** — *Efficient IO with io_uring* — kernel recipes 2019 — https://kernel.dk/io_uring.pdf — covers swap/IO reintegration.
- **Lipp, Schwarz, Gruss et al.** — *Meltdown: Reading Kernel Memory from User Space* — USENIX Security 2018 — https://meltdownattack.com/meltdown.pdf .
- **Intel LAM whitepaper** — 2022 — https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/best-practices/linear-address-masking.html .
- **Arm MTE** — *Armv8.5-A Memory Tagging Extension* — 2019 — https://developer.arm.com/documentation/102925/ .
- **Linux Documentation/mm/** — https://www.kernel.org/doc/html/latest/mm/ — authoritative source for current VM internals (hugetlbpage, transhuge, memcg, vmemmap).
- **Jeff Bonwick** — *The Slab Allocator* — USENIX 1994 — https://people.eecs.berkeley.edu/~kubitron/courses/cs194-24-S13/hand-outs/bonwick_slab.pdf .
- **Johannes Weiner** — *Pressure Stall Information* — Facebook, 2018 — https://facebookmicrosites.github.io/psi/ .
- **Christoph Lameter** — *The SLUB allocator* — LWN, 2007 — https://lwn.net/Articles/229984/ .
- **Andrea Arcangeli** — *Transparent Huge Pages* — LWN, 2011 — https://lwn.net/Articles/423584/ .

---

## 14. ALZE applicability — v1 / v2 / v3

### v1 — stabilize 4-level paging + fix R2 memory.md findings (immediate)

**Goal**: ALZE v1 is a single-node, single-user, no-fork kernel. Scope = fix the 14 R2 issues, nothing else.

1. **Reserve PFN 0 unconditionally** (R2 #1) — mark `PAGE_RESERVED` at `pmm_init` tail.
2. **Flush TLB in `vmm_map_range_huge`** (R2 #2) — add `vmm_flush_tlb(v)` per 2 MiB PTE.
3. **Add slab redzones** (R2 #3) — 8 B `0xCC` pattern pre/post each slot, validate on free.
4. **Call `kmalloc_init()` explicitly from main.c pre-SMP** (R2 #4) — remove lazy init.
5. **Bitmap-recycle VMA pool** (R2 #5) — use existing `bitmap_alloc.h` primitive.
6. **Add `vm_space->lock`** (R2 #6) — spinlock around all VMA mutation.
7. **Fix `vmm_tlb_batch_flush` full-flush** (R2 #7) — extend shootdown protocol with `full_flush` flag; receivers reload CR3.
8. **Emergency reserve + propagate OOM** (R2 #8) — 16 pre-allocated page-table pages; plumb failure return through `get_or_create_entry`; replace `KASSERT` panic with graceful error.
9. **Fix huge-page guard-page silent-noop** (R2 #9) — allocate task stacks in dedicated non-HHDM VA range (`0xFFFFFF0000000000+`), mapped 4 KiB so `vmm_unmap_page` guard works.
10. **Promote partial-remap warning to KASSERT** (R2 #10) — or gate on explicit `VMM_REMAP_ALLOW` flag.
11. **Enable SMEP + SMAP** (R2 #11) — `or $0x100000 | 0x200000, %cr4` after CR3 switch.
12. **Reject overlapping VMA adds** (R2 #12) — validate `new.start >= prev.end` in `vma_add`.
13. **Wire at least one mempressure callback** (R2 #14) — even just `kmalloc_dump_stats` on CRITICAL.

Adopt architectural shape:
- **Flat 1 GiB huge-page ID map** for the kernel HHDM (if `CPUID.AMD.ExtFeature.Page1GB`) — saves ~500 TLB entries vs current 2 MiB.
- **4 KiB pages for user regions** (when userspace lands in v2).
- **No CoW fork()** — `process_create(elf, vmar_spec)` Zircon-style. Saves ~3 000 LOC.
- **No swap** — OOM killer + emergency reserve handles pressure.

### v2 — PCID, huge pages, real OOM (second milestone)

1. **Enable PCID** (`CR4.PCIDE=1`) + per-mm-struct PCID assignment. Recycle 6 PCIDs per CPU LRU-style. Requires SMP first.
2. **Use 2 MiB huge pages for anon user regions ≥ 2 MiB** — explicit (`MAP_HUGETLB` analog), not THP (which requires khugepaged + compaction).
3. **Per-CPU kmalloc magazines** — 30-entry free list per CPU per size class, refill from global partial-slab list.
4. **Per-zone free lists in PMM** — populate the unused `zone_info.free_lists[]` (R2 recommendation 10) → O(1) zone alloc vs current O(n) walk.
5. **vmalloc-equivalent** for kernel buffers > 4 MiB.
6. **PSI-style pressure tracking** — track wallclock spent in reclaim/OOM per cgroup (future); wire into memory-pressure callbacks.
7. **Proper `page_fault` handler** — user pages, anon zero-page CoW-lite (one shared zero page, RO mapped, CoW on write for anon mmap).

### v3 — 5-level paging, MTE/LAM, compressed swap (aspirational)

1. **5-level paging** (57-bit VA) — gated on `CPUID.(EAX=7).ECX.LA57` at boot. Most ALZE workloads will never need it; implement for completeness on Icelake-SP+ hosts.
2. **Intel LAM / Arm MTE / Arm PAC** — pointer tagging for kernel HWASAN-style UAF detection. Requires v2 per-CPU magazines + slab metadata rework to carry tags.
3. **Swap + zram** — IF ALZE ever becomes a real multi-user system with memory pressure workloads. Probably **never** — ALZE is a unikernel-ish embedded/desktop kernel, not a server hosting 1 000 tenants.
4. **Arm port** — port all the above to aarch64. PCID → ASID, `invlpg` → `TLBI`, PAT → MAIR_EL1 attributes, LAM → MTE.

### What ALZE explicitly refuses

- **COW fork()** — ~3 000 LOC, correctness minefield (huge-page CoW, vfork races, `MADV_WIPEONFORK`). Zircon-style `process_create` is safer and enough.
- **Swap / page-out** — ~3 000 LOC (storage integration, IO scheduler hooks, reclaim heuristics, swap tables). Use OOM + zram-like compression at most.
- **Transparent Huge Pages auto-promotion** (khugepaged-equivalent) — requires working compaction + defrag + background kthread. Explicit huge-page mmap is enough.
- **Kernel Same-Page Merging (KSM)** — only useful in hosting tenants' duplicate VMs. Out of scope.

---

## 15. Honest note — paging is where hobby kernels die

The textbook narrative — "allocate a PML4, map the kernel identity, load CR3, done" — is literally ~50 lines of code. Every hobby kernel reaches that milestone in a week. Then:

- **TLB shootdown** adds ~500 LOC + weeks of correctness debugging when the second CPU lands. ALZE already has this infrastructure partially wrong (R2 #2, #7).
- **Huge-page / 4 KiB coexistence** adds ~800 LOC (splitting, merging, guard-page handling, PAT on huge leaves). ALZE has bug #9 (guard-page silent-noop on 2 MiB HHDM) exactly here.
- **Demand paging / #PF handler** adds ~1 500 LOC (major/minor split, page-cache integration, swap-in, zero-page, anon CoW). ALZE hasn't started.
- **CoW fork()** adds ~3 000 LOC and is the source of half of Linux's historical VM CVEs (Dirty COW 2016, Dirty Pipe 2022). **Skip entirely.**
- **Swap** adds another ~3 000 LOC including storage integration. **Skip until v4.**
- **memcg / cgroups** adds ~5 000 LOC, only useful for multi-tenant. **Skip.**
- **NUMA aware allocators** adds ~2 000 LOC, only useful on >1-socket hardware. **Skip.**
- **Huge-page compaction / defrag** (khugepaged) adds ~1 500 LOC, requires working reclaim. **Skip.**

Total skippable: ~18 000 LOC. ALZE's entire memory subsystem is currently 3 274 LOC (per R2 review header). A "full Linux-style VM" is 10× that. The v1/v2 ambition keeps ALZE at ~6 000 LOC — still small, still auditable.

**Prescription**:
1. Fix the 14 R2 issues — two weeks of work, no new features.
2. Promote HHDM to 1 GiB pages — one flag flip + TLB shootdown.
3. Keep `process_create`-only (no fork), keep swap off, keep THP off.
4. Revisit in 6-12 months when SMP is real and v2 can add PCID + per-CPU magazines.

The lesson from Bovet/Cesati ch. 9 and from every hobby-kernel post-mortem: **don't chase Linux feature parity in VM — chase correctness + one solid model**. ALZE's lane is "Zircon-like, no fork, no swap, small surface." Stay in it.
