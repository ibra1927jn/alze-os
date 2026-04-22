# ALZE OS — Memory Subsystem Review

Reviewer: senior kernel MM reviewer. Date: 2026-04-21. Test status: `test_pmm` 29/29 PASS on host.

## Scope

| File | LOC | Role |
|---|---|---|
| `kernel/pmm.c` | 667 | Buddy allocator, 11 orders (0..10 = 4 KB..4 MB) |
| `kernel/pmm.h` | 190 | PMM API, zones, `struct page` (32 B) |
| `kernel/vmm.c` | 716 | 4-level paging, W^X, TLB batching, CR3 switch |
| `kernel/vmm.h` | 143 | PTE flag defs, API, tlb_batch |
| `kernel/kmalloc.c` | 478 | Slab (8 classes 16..2048), large-page path, quarantine |
| `kernel/kmalloc.h` | 100 | kmalloc API, profiling hooks |
| `kernel/bitmap_alloc.h` | 73 | `id_pool` (64-bit BSF bitmap primitive) |
| `kernel/vma.c` | 154 | VMA list, static pool of 128 entries |
| `kernel/vma.h` | 99 | VMA types/perms, `vm_space` |
| `kernel/mempressure.c` | 109 | Pressure thresholds + callback fan-out |
| `kernel/mempressure.h` | 75 | Thresholds (75/25 % + watermarks 20/10/5 %) |
| `kernel/tlb_shootdown.c` | 101 | IPI vector 0xFE shootdown, ack spin with timeout |
| `kernel/tlb_shootdown.h` | 67 | Shootdown state struct |
| `kernel/memory.h` | 67 | `PAGE_SIZE`, `ALIGN_*`, `PHYS2VIRT` |
| `tests/test_pmm.c` | 235 | Host-side 6 scenarios, exercises split/coalesce/stress |
| **Total** | **3274** | |

## PMM

Design: binary **buddy allocator**, orders 0..10 (one PFN..1024 PFNs = 4 MiB). Single global `struct pmm_state` (pmm.c:58). `struct page` is 32 B (pmm.h:65–74) with fwd/back free-list links, `order`, `flags`, `ref_count`, `zone`, `color`.

Bootstrap (pmm.c:587–665):
1. Pass 1 scans Limine memmap for max **usable** address (pmm.c:597–602). Correctly ignores reserved MMIO at very high addresses that would otherwise blow up the page array.
2. Pass 2 finds the largest USABLE region and steals `total_pfns * 32 B` at its base for the page array (pmm.c:618–632). Asserts steal fits.
3. Passes 3 mark everything RESERVED, assign zone + color, then feed each USABLE region into the buddy (excluding the stolen metadata pages) via `pmm_feed_region`.

`pmm_feed_region` (pmm.c:558–585) coalesces PFNs into largest naturally-aligned buddy block per iteration. Clean.

Zones: DMA (<16 MB), NORMAL (<4 GB), HIGH (rest). `pmm_alloc_pages_zone` (pmm.c:277–341) linearly walks free lists checking `blk_end <= zone_limit`. No per-zone free lists (all orders share the global list), so zone alloc is **O(zone candidates)** not O(1). Tolerable but suboptimal; documentation at pmm.h:85 promises per-zone lists that `struct zone_info` defines but the code never uses — dead struct (`pmm.zones[]`).

Alignment: buddy is XOR-based `buddy_pfn = pfn ^ (1 << order)` (pmm.c:120). Blocks are naturally aligned because feed-region only frees aligned chunks (test confirms: `(big % 128KB) == 0`).

Contiguous alloc: yes via order. Max 4 MiB contiguous (order 10).

Lowmem reservation: only implicit — DMA pool depends on what Limine marks usable below 16 MB. No explicit "reserve first 1 MB" or "never return PFN 0". See **Issues #1**.

Stats: `free_pages`, `used_pages`, `reserved_pages`, `peak_used`, per-order `free_lists[i].count`, zone counts via scan. `pmm_dump_stats` (pmm.c:390).

Concurrency: single `pmm_lock` spinlock guarding every path, including `pmm_ref_get` (fixed 2026-03-28). IRQ-save lock is correct pre- and post-SMP.

## VMM / page tables

4-level paging (PML4 → PDPT → PD → PT). No 5-level, no recursive mapping: uses the HHDM linear map to access tables (`PHYS2VIRT` everywhere). This is simpler and avoids recursive-slot headaches.

Bootstrap (vmm.c:481–554):
1. `enable_nx()` sets `EFER.NXE`.
2. Allocate zeroed PML4, find kernel phys base by walking **Limine's** CR3 tables (vmm.c:299–327) — correct since the kernel image is in higher half (not HHDM).
3. Map whole kernel W+X temporarily (vmm.c:505, flags `PRESENT|WRITE|GLOBAL` **without NX** — intentional, W^X enforcement disabled until step 6).
4. Map HHDM with 2 MiB huge pages up to `vmm_find_max_phys()` (highest usable+BOOTLOADER_RECLAIMABLE+KERNEL+FB, 2 MiB rounded). Flags = `PRESENT|WRITE|GLOBAL|NX`.
5. `write_cr3(pml4_phys)` — moment of truth.
6. `vmm_apply_section_permissions`: re-map `.text` RX, `.rodata` RO+NX, `.data`/`.bss` RW+NX; sweep all pages in `[__kernel_start,__kernel_end)` that are not in a named section and re-map RW+NX. Flips `vmm_wx_enforced=1`.
7. `vmm_setup_kernel_stacks`: 16 KB TSS RSP0 stack + guard, 8 KB IST1 stack + guard, boot-stack guard page.

Large pages: 2 MiB via `vmm_map_range_huge` — used for HHDM only. 1 GiB partially supported in `vmm_virt_to_phys` read-path (vmm.c:233) but no writer path exists.

W^X: enforced from step 6 onward. `vmm_map_page` and `vmm_map_page_batched` force `PTE_NX` when `PTE_WRITE|!PTE_NX` is passed (vmm.c:163–167, 585–588) and log a warning. `vmm_audit_wx` (vmm.c:703) walks all present leaves and counts violations. HHDM is W+NX — clean. Good.

NX: enabled. SMEP/SMAP: **not enabled**, no `CR4.SMEP`/`CR4.SMAP` writes anywhere (grep confirms). ASID/PCID: unused. `CR3_PCID` unset.

TLB: `vmm_flush_tlb` does local `invlpg` + `tlb_shootdown_broadcast`. `vmm_map_page` flushes after edit ✓. `vmm_unmap_page` flushes after edit ✓. `vmm_map_range_huge` writes PD entries directly **but does not flush any TLB entry per 2 MiB page** (vmm.c:267–295) — only relies on the global CR3-switch that happened earlier in `vmm_init` (where HHDM is first mapped). Post-boot callers of `vmm_map_range_huge` would see stale TLB. See **Issues #2**.

Cross-CPU correctness: since LAPIC is single-core currently (`active_cpus=1`), shootdown is a no-op. When AP startup lands, the infrastructure is in place. `tlb_shootdown_broadcast` holds `tlb_sd.lock` while spinning — serializes all shootdowns; acceptable.

## kmalloc / heap

**Slab**, 8 classes `16, 32, 64, 128, 256, 512, 1024, 2048`. Each slab = one 4 KiB page: 32 B header + N slots. Free slots intrusively chained via first 8 B.

Large alloc path: for `size > 2048`, allocate `order` buddy pages (`needed = size + 8`), store `order` in first 8 B, hand out `ptr+8`. `kfree_large` detects a large alloc by `(ptr-8) & 0xFFF == 0` (page-aligned header). Slab first-slot is at `page+32` so cannot collide. OK.

Fragmentation: `kmalloc_fragmentation` returns `partial_slabs*100/total_slabs`. No per-CPU magazines, no slab-reclaim except when `slab->used == 0` in which case the slab page is returned to PMM (`slab_return_slot`, kmalloc.c:240). No lazy reclamation for large allocs.

Double-free: OpenBSD-style quarantine of 32 entries + poison byte `0xDE`. `kfree` checks poison on `ptr[8..12]` (kmalloc.c:300–308) before accepting — raises `KASSERT(0)` on detected double-free. `QUARANTINE_SIZE=32` objects delay the return.

UAF debug: poison the slot on free (kmalloc.c:313); poison the entire page on slab reclaim (kmalloc.c:248); poison entire large alloc on free (kmalloc.c:273). No redzone before/after the slot (i.e., no canary bytes between adjacent slots). See **Issues #3**.

Alignment guarantees: slab returns `data_start + i*obj_size`. `data_start = slab+sizeof(header) = slab+32`. For `obj_size>=16`, slots are 16 B-aligned. For 16-byte class, slots start at +32, +48, +64… only 16 B aligned. **Not 32 B-aligned** — a SIMD caller using AVX-256 (32 B-aligned mov) would fault. Finding.

Large alloc: `order` integer-overflow was fixed (ERRORES 2026-03-28). Current bound check `order > PMM_MAX_ORDER → NULL` is at kmalloc.c:191–194 plus the corruption bounds check at kmalloc.c:268/385.

Init: lazy (`initialized=0` flag, kmalloc.c:155–168). Not a race pre-SMP but race-prone if concurrent first-callers exist under SMP. See **Issues #4**.

## VMA

Address-space layout: `struct vm_space` with a sorted linked list of VMAs (by start address). No red-black tree, so `vma_find` is O(n) — fine for small N.

Mapping/unmapping API: `vma_add` / `vma_remove` / `vma_find` / `vma_check_access`. **No overlap detection** in `vma_add` (vma.c:39–69): adding `[100,200)` then `[150,250)` silently leaves both in the list; `vma_find(160)` returns the first (lower-start). Permission leaks possible.

Refcount on backings: none. No shared backing objects; each VMA is a raw range descriptor, no physical page ownership linkage.

COW: not implemented. PMM has `ref_count` + inc/dec/get API but no VMA-level COW flag or `handle_cow_fault` wiring.

Error unwind on failure: `vma_add` returns `-ENOMEM` on exhaustion (128-entry static pool). Caller must handle. Pool is **never recycled** — `vma_remove` unlinks the node but does not return the slot to the pool (`vma_pool_used` only ever grows). See **Issues #5**.

Thread-safety: `vm_space` has no lock; `vma_pool_used` is unsynchronized global; `alloc_vma` increments it racefully. See **Issues #6**.

## TLB shootdown

IPI vector **0xFE** (outside PIC + exceptions range). Single global `tlb_sd` struct protects by spinlock. Protocol: write `target_addr`, set `pending_cpus = active_cpus-1`, send IPI to all-others via `lapic_send_ipi_all`, spin on `ack_count` with 1 M-iteration timeout (post-2026-03-28 fix). EOI sent by handler via `lapic_eoi` (post-2026-03-28 fix).

Single target address only — not batched at the shootdown level. The higher-level `vmm_tlb_batch_flush` either does individual `invlpg+broadcast` loops (vmm.c:613–616, one IPI per address!) or a full-flush via CR3 reload + `tlb_shootdown_broadcast(0)` — but target=0 in the broadcast means receivers execute `invlpg 0`, which flushes the page at address 0 only, **not** a full TLB flush. See **Issues #7**.

Fairness: global `tlb_sd.lock` held throughout the spin-wait — all other shootdowns serialize behind it. Acceptable pre-SMP; a bottleneck on many-core.

Safe on current CPU: broadcast is no-op when `active_cpus <= 1`. Handler does `invlpg (target)` and EOIs. Not NMI-safe (regular IPI OK for intended use).

## Memory pressure

Thresholds: NORMAL (>75 % free), WARNING (25–75 %), CRITICAL (<25 %). Watermarks 20/10/5 %.

Trigger: `mempressure_check()` called from idle loop only (main.c:183). No call from allocation hot paths — an allocation burst inside a short ISR/task never triggers reclaim until idle runs.

Reclaim path: `mempressure_check` invokes registered callbacks on transitions. **Zero callbacks registered anywhere in the tree** (grep `mempressure_register\(` returns only the definition). So the whole infrastructure is dormant.

OOM path: none. `pmm_alloc_pages` returns 0 → caller gets NULL → either `KASSERT` (e.g., `vmm.c:104 KASSERT(new_table != 0)`) or silently bubbles up `NULL` to the user. No OOM killer, no OOM reserves, no "emergency pool" below min watermark. See **Issues #8**.

## Highlights

- Buddy allocator is textbook-clean; XOR buddy math; coalescing climbs orders correctly; test_pmm 29/29 PASS on host including 100 K stress cycles.
- W^X applied per-section with explicit enforcement flag toggle AND a gap-sweep that catches alignment padding between sections — nice defense-in-depth.
- Post-mortem fixes from ERRORES.md are all present and correct (`ref_get` lock, `vmm_unmap_page` lock, `vmm_virt_to_phys` lock, huge-map lock, quarantine lock reorder, double-free detection under lock, order-bounds on large free, IPI timeout + EOI, EFER.NXE enabled).
- Quarantine ring (OpenBSD-style) + 0xDE poison for UAF detection on slabs and large allocs.
- TSS RSP0 and IST1 (#DF) stacks with explicit guard pages.
- `struct page` held to 32 B with a `_Static_assert` equivalent via padding — indexing math is branch-free.

## Issues found

1. **PFN 0 not reserved; `0` means OOM ambiguously.** `pmm.c:134 pmm_alloc_pages` returns `0` on OOM (pmm.c:156) and also returns `pfn_to_phys(pfn=0) = 0` if PFN 0 is ever freed into the pool. If Limine ever marks page 0 USABLE (rare but BIOS-dependent), a successful alloc of phys 0 is indistinguishable from failure. **Fix:** mark PFN 0 `PAGE_RESERVED` unconditionally at pmm_init tail, or return `(uint64_t)-1` on OOM.

2. **`vmm_map_range_huge` never invalidates TLB** (vmm.c:267–295). It writes 2 MiB PD entries inside `vmm_lock` and returns without any `invlpg` or shootdown. `vmm_init` can get away with it because CR3 is re-written afterwards (full flush), but any post-boot caller that maps a fresh HHDM huge page would see stale TLB on the local CPU and all others. **Fix:** after each `pd[PD_INDEX(v)] = ...`, add `vmm_flush_tlb(v)` (or batch and `tlb_shootdown_broadcast` at end).

3. **Slab has no redzone between slots.** kmalloc.c:137–141: slots are packed back-to-back. An in-kernel driver buffer overflow into the adjacent slot is undetectable until the next free-time poison check (and even then only if the adjacent slot was free). Caller-controlled overflow from driver to adjacent kernel struct is possible. **Fix:** add guard bytes (e.g., 8 B pre/post each slot with known pattern, checked on free).

4. **`kmalloc_init` is lazy + racy.** kmalloc.c:155–175. First concurrent kmalloc callers on SMP can both see `initialized==0`, both run init, both reset `classes[].slabs = NULL` — clobbering in-flight allocations. **Fix:** call `kmalloc_init()` explicitly from `main.c` before scheduler starts, or use `__atomic_compare_exchange` on `initialized`.

5. **VMA pool never recycles.** `vma.c:19 alloc_vma` only grows `vma_pool_used`; `vma.c:73 vma_remove` unlinks but does not free. The pool (`VMA_MAX_REGIONS*4 = 128` entries) bleeds over the lifetime of the kernel — eventual `-ENOMEM`. **Fix:** use a bitmap allocator (`bitmap_alloc.h` already exists!) to manage pool slots.

6. **VMA list + pool have no locking.** `vma.c:13 kernel_vm_space`, `vma.c:17 vma_pool_used`, `alloc_vma`, `vma_add`, `vma_remove` — none take a lock. Kernel boot is single-threaded so current safe, but the moment a second task mutates a shared `vm_space` (e.g. module-load adding MMIO VMA concurrently with idle), list corruption. **Fix:** per-`vm_space` spinlock + global lock on pool allocator.

7. **`vmm_tlb_batch_flush` full-flush signal is wrong.** `vmm.c:610`: `tlb_shootdown_broadcast(0)` tells other CPUs to `invlpg 0` (only flushes PFN 0 on those CPUs), not a full-TLB flush. The local CPU's CR3 reload does flush locally. **Fix:** extend shootdown protocol with a `full_flush` flag, or send a second vector that receivers treat as "reload CR3".

8. **No OOM policy, no reserves.** `pmm_alloc_pages(0)` returning 0 directly blows up `KASSERT(new_table != 0)` inside `get_or_create_entry` (vmm.c:104). A transient OOM in the middle of a `vmm_map_page` will *panic the kernel*, not return an error. **Fix:** propagate failure up from `get_or_create_entry`, unwind partial allocations, and keep a reserve pool for page-table nodes.

9. **Guard-page unmap on HHDM is a silent no-op.** `sched.c:293` calls `vmm_unmap_page(PHYS2VIRT(stack_phys))` to install a guard at the bottom of a task stack. HHDM is mapped with 2 MiB huge pages (vmm.c:520), and `vmm_unmap_page` explicitly bails out on `PTE_HUGE` (vmm.c:202–205) without splitting the huge page. **Result: task stacks have no HHDM-side guard page at all.** Stack overflow silently corrupts adjacent stacks. **Fix:** split the 2 MiB entry into 512 × 4 KiB entries on-demand before unmapping, or allocate task stacks in a dedicated non-HHDM virtual range (like TSS/IST1 are at `0xFFFFFF0000…`).

10. **Partial-overlap on re-map only logs a warning.** `vmm.c:151–154`: remap of a virt with a **different** phys triggers `LOG_WARN` but still overwrites the PTE. A driver bug that double-maps its MMIO gets a warning and silent corruption. **Fix:** promote to `KASSERT` unless an explicit `VMM_REMAP_ALLOW` flag is passed.

11. **No SMEP/SMAP.** grep finds no `CR4` writes. SMEP would prevent Ring-0 from executing user-mapped pages; SMAP would prevent stray accesses. Both are toggled in `CR4` and cheap. Given userland is planned, enabling them *now* (even if no user pages exist yet) hardens against future regressions.

12. **`vma_add` accepts overlapping ranges silently.** `vma.c:52–68` inserts sorted by start but never checks `new.start < prev.end`. A buggy caller adds `[0x1000,0x3000)` RW then `[0x2000,0x4000)` RX; `vma_check_access(0x2500, EXEC)` returns false because it finds the first VMA (no EXEC). Permission bypass in the other direction: `vma_check_access(0x2500, READ)` returns true, but the RX VMA should be consulted for any future cleanup logic that assumes no overlap. **Fix:** reject overlap in `vma_add`.

13. **`kfree` large-alloc detection collides with a malformed small pointer.** `kmalloc.c:260`: `((ptr-8) & 0xFFF) == 0` is the large-alloc sentinel. If an attacker-controlled (via driver bug) corrupted pointer happens to be `page+8`, `kfree_large` interprets `*(uint64_t*)page` as `order` and `pmm_free_pages(virt_to_phys(page), order)`. There IS a bounds check `order > PMM_MAX_ORDER → LOG_ERROR; return true` (kmalloc.c:268) — mitigates the crash case but still leaks a large alloc detection on stray pointers.

14. **`mempressure_check` called only from idle.** Burst allocations between idle ticks never notice they crossed a watermark. Combined with zero registered callbacks, the whole pressure system currently does nothing observable.

## Recommendations

1. Enable SMEP + SMAP in `vmm_init` before CR3 switch (`mov %%cr4, …; or $0x100000 | 0x200000, %%cr4`). Cheap, large attack-surface reduction.
2. Split huge pages on first `vmm_unmap_page` / `vmm_map_page` that targets an address inside a 2 MiB PD entry — or move all guard-page users to a non-HHDM dedicated range. See #9.
3. Reserve PFN 0 unconditionally in `pmm_init` tail loop; optionally reserve PFN 1..255 (1 MiB) as well for BIOS legacy safety.
4. Add per-slot redzones (8 B pattern pre/post) and verify on free. Critical for catching driver overflows before they land in adjacent slots.
5. Add a page-table emergency reserve (e.g., 16 pre-allocated 4 KiB pages) so `vmm_map_page` can succeed during OOM; plumb error return instead of `KASSERT`.
6. Bitmap-recycle the VMA pool; add `vm_space->lock` spinlock.
7. Switch `vmm_tlb_batch_flush` full-flush to a dedicated shootdown opcode (e.g., write a second field `tlb_sd.full_flush` and have receivers reload CR3).
8. Wire at least one `mempressure_register` callback — even a stub that calls `kmalloc_dump_stats` on CRITICAL would be informative.
9. Explicitly `kmalloc_init()` from `main.c` pre-SMP, remove the `initialized` flag.
10. Use per-zone free lists (the `zone_info.free_lists[]` struct already exists in `pmm.h:87` — just populate them) to make zone allocation O(1) and avoid the linear walk in `pmm_alloc_pages_zone`.

## Risk zones

**Low memory.** Worst-case path: `kmalloc(size)` → `pmm_alloc_pages_zero(0)` returns 0 → `slab_new` returns NULL → caller sees NULL. Inside VMM: `get_or_create_entry(…,allocate=true)` hits `KASSERT(new_table != 0)` which **panics the kernel**. Any page-fault handler that needs to install a fresh page-table under OOM will panic. Mempressure never triggers (no callbacks). Net: ALZE has **no graceful low-memory path**. Risk: high.

**Concurrent page-fault + mmu change.** Current `vmm_lock` is held across the whole `vmm_map_page` / `vmm_unmap_page` call *including* the `vmm_flush_tlb` (→ `tlb_shootdown_broadcast`). Broadcast spins on ACKs up to 1 M iterations; a CPU mid-page-fault trying to take `vmm_lock` will spin behind the broadcast. No deadlock because broadcast never reacquires `vmm_lock`. Risk: moderate — acceptable correctness; poor tail latency under high MM churn.

**Lock-order.** Forward order: `vmm_lock` → (no inner MM lock) → `tlb_sd.lock`. PMM path: `pmm_lock` — never nested with `vmm_lock`. kmalloc path: `classes[].lock` → `pmm_lock` (via `slab_new` and `slab_return_slot`). kmalloc quarantine: `quarantine_lock` → optionally `classes[old_cls].lock`. Fixed 2026-03-28 so the class lock is released before taking the other class's lock — avoids AB-BA deadlock. No inversion spotted.

**Can a driver buffer overflow corrupt the heap?** Yes. Slots are packed with no redzone; an overflow of N bytes from one slab slot into the next is undetectable until the next-slot free time, and only if the next slot is in the "poison tail". No redzone means first 8 B of the next slot (the intrusive free-list pointer) is corruptible — a targeted overflow can redirect the free list to an attacker-chosen address and return it as a future kmalloc pointer. **Exploitable primitive.** Mitigations present: poison on free, quarantine (delays reuse ~32 frees), double-free detection. Missing: redzone, `write-rare` headers. Risk: **high**.

**Can the kernel heap be corrupted from within itself?** A wild write into a freed large-alloc base (first 8 B = `order`) is bounds-checked (`order > PMM_MAX_ORDER`) so corruption there just aborts the free — does not misuse PMM. Good.

**Huge-page W^X cleanliness.** HHDM is mapped W+NX. `vmm_audit_wx` confirms 0 violations. A future `vmm_map_range_huge` caller that omits `PTE_NX` would *not* trigger the enforcement check (that path lives in `vmm_map_page` / `vmm_map_page_batched` only, not in `vmm_map_range_huge` — vmm.c:267–295 lacks the W^X gate). If HHDM were remapped without NX by mistake, the whole physical space becomes executable. **Add the W^X check to `vmm_map_range_huge` too.**
