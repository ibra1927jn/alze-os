# ERRORES.md — Mistakes we do not repeat

## Format
[YYYY-MM-DD] | [affected file] | [error] | [fix applied]

---

## Recorded errors
- [2026-03-28] | kernel/sched.c | bsfq has undefined result when input == 0; bitmap_find_first could return garbage if prio_bitmap was 0 and bitmap_dequeue would access an invalid index | Added check bm == 0 → return -1 in bitmap_find_first, and guard p < 0 in bitmap_dequeue
- [2026-03-28] | kernel/pmm.h | The _reserved field of struct page was used informally as a ref counter for COW, without API or protection | Replaced _reserved with ref_count (uint32_t) with formal API: pmm_ref_inc/pmm_ref_dec/pmm_ref_get, protected by spinlock
- [2026-03-28] | kernel/vmm.c | vmm_flush_tlb only invalidated the local TLB; on SMP other CPUs would keep stale entries causing memory corruption | Implemented TLB shootdown infrastructure via IPI (vector 0xFE), wired to vmm_flush_tlb. Safe no-op on single-core until LAPIC is available
- [2026-03-28] | kernel/tlb_shootdown.c | The IPI handler did not send EOI to LAPIC, which would block future interrupts when LAPIC is active | Added write to LAPIC EOI register (0xFEE000B0) at end of handler. Safe on single-core because handler is never invoked without LAPIC
- [2026-03-28] | kernel/kmalloc.c | Integer overflow in order calculation: the while loop could exit with order = PMM_MAX_ORDER + 1, causing invalid size allocation | Changed condition to order < PMM_MAX_ORDER and added post-loop bounds check that returns NULL if order exceeds maximum
- [2026-03-28] | kernel/tlb_shootdown.c | Infinite busy-wait waiting for IPI ack from other CPUs: if a CPU does not respond, the kernel hangs permanently | Added timeout of ~1M iterations with break and warning via kprintf
- [2026-03-28] | kernel/lapic.c | PIT calibration without timeout: if the PIT does not respond, the kernel hangs in infinite busy-wait | Added timeout (0xFFFFFF iterations) with fallback to estimated ticks/sec value
- [2026-03-28] | kernel/mutex.c | Race condition in priority inheritance: saved_priority was overwritten on first boost with the owner's current priority (possibly already boosted by another mutex), corrupting the restoration | Removed the overwrite in the boost path; saved_priority is only stored when acquiring the mutex
- [2026-03-28] | kernel/pmm.c | pmm_ref_get read ref_count without spinlock, unlike ref_inc/ref_dec, causing possible inconsistent read on SMP | Added spin_lock_irqsave/restore around the read, consistent with other ref_count operations
- [2026-03-28] | kernel/ext2.c | memcpy of superblock without bounds check on fs.disk_size: if the disk was smaller than offset + sizeof(superblock), memcpy would read out of bounds | Added size validation before memcpy, returning -1 if the disk is too small
- [2026-03-28] | kernel/xhci.c | Three busy-wait loops with for(i<100000) without feedback on timeout: if hardware does not respond, the loop ends silently and code continues in invalid state | Replaced with while(--timeout>0) with explicit check; if timeout==0, logs error and returns -1
- [2026-03-28] | kernel/kmalloc.c | Cast uint64_t→uint32_t without validation in kfree and kmalloc_usable_size: if the header was corrupted, order could be huge causing memset/shift out of bounds | Added bounds check order > PMM_MAX_ORDER with log and safe return in both functions
- [2026-03-28] | kernel/sched.c | watchdog_ticks was incremented but never used to kill stuck tasks: a task in an infinite loop without yield would monopolize the CPU indefinitely | Added enforcement in sched_tick: if a task (TID>1) exceeds 10000 ticks without yielding, it is marked TASK_DEAD and queued for reaping
- [2026-03-28] | kernel/sched.c | task_create did not free the TCB if pmm_alloc_pages_zero failed after alloc_tcb, causing permanent slot leak in tcb_bitmap | Added free_tcb(t) before return -ENOMEM in the stack allocation failure path
- [2026-03-28] | kernel/sched.c | sched_init searched for idle task via &task_pool[1] (hardcoded slot), which breaks if alloc_tcb changes allocation order | Changed to search by TID returned by task_create, with KASSERT if not found
- [2026-03-28] | kernel/sched.c | sched_init did not verify alloc_tcb return for init_task, a silent NULL dereference if all 64 slots are full | Added KASSERT(init_task != 0)
- [2026-03-28] | kernel/sched.c | task_join read target->finished and target->join_wq without lock, race with task_exit and reaper that could leave target as dangling pointer | Wrapped find_task + finished check + join_wq assignment inside spin_lock_irqsave(&sched_lock)
- [2026-03-28] | kernel/vmm.c | vmm_unmap_page did not take vmm_lock, race condition with concurrent vmm_map_page that could corrupt page tables | Added spin_lock_irqsave/restore around walk and PTE clear
- [2026-03-28] | kernel/vmm.c | vmm_virt_to_phys did not take vmm_lock, inconsistent read if map/unmap occurred concurrently | Added locking with goto-based cleanup for all early returns
- [2026-03-28] | kernel/vmm.c | vmm_map_range_huge did not take vmm_lock, direct writes to PD entries without synchronization | Added spin_lock_irqsave/restore around the entire loop
- [2026-03-28] | kernel/kmalloc.c | Quarantine flush in kfree modified free list and slab list of old_cls without taking classes[old_cls].lock when old_cls != cls_idx, data race on SMP | Restructured quarantine: extraction of evicted under quarantine_lock, return to free list under the correct class lock
- [2026-03-28] | kernel/kmalloc.c | Double-free detection read slab->obj_size and slot memory without lock, possible inconsistent read under contention | Moved the check inside the class spin_lock, with unlock before KASSERT(0)
- [2026-03-28] | kernel/string.c | memmove backward path used byte-by-byte copy, ~8x slower than forward path for large buffers | Optimized with rep movsq + STD flag for bulk backward copy, keeping byte-by-byte only for residuals and buffers < 8 bytes
- [2026-03-28] | kernel/main.c | Limine framebuffer pointer was not validated against NULL before use in console_init | Added KASSERT(fb != NULL) and KASSERT(fb->address != NULL)
