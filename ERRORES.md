# ERRORES.md — Mistakes we do not repeat

## Format
[YYYY-MM-DD] | [affected file] | [error] | [fix applied]

---

## Recorded errors
- [2026-03-28] | kernel/sched.c | bsfq tiene resultado indefinido cuando input == 0, bitmap_find_first podia devolver basura si prio_bitmap era 0 y bitmap_dequeue accedia a un indice invalido | Se agrego check bm == 0 → return -1 en bitmap_find_first, y guard p < 0 en bitmap_dequeue
- [2026-03-28] | kernel/pmm.h | El campo _reserved de struct page se usaba informalmente como ref counter para COW, sin API ni proteccion | Se reemplazo _reserved por ref_count (uint32_t) con API formal: pmm_ref_inc/pmm_ref_dec/pmm_ref_get, protegidas por spinlock
- [2026-03-28] | kernel/vmm.c | vmm_flush_tlb solo invalidaba TLB local, en SMP las demas CPUs mantendrian entradas obsoletas causando corrupcion de memoria | Se implemento infraestructura TLB shootdown via IPI (vector 0xFE), wired a vmm_flush_tlb. No-op seguro en single-core hasta que LAPIC este disponible
- [2026-03-28] | kernel/tlb_shootdown.c | El handler IPI no enviaba EOI al LAPIC, lo que bloquearia futuras interrupciones cuando LAPIC este activo | Se agrego escritura a LAPIC EOI register (0xFEE000B0) al final del handler. Seguro en single-core porque el handler nunca se invoca sin LAPIC
- [2026-03-28] | kernel/kmalloc.c | Integer overflow en calculo de order: el while loop podia salir con order = PMM_MAX_ORDER + 1, causando allocacion de tamano invalido | Se cambio condicion a order < PMM_MAX_ORDER y se agrego bounds check post-loop que retorna NULL si order excede el maximo
- [2026-03-28] | kernel/tlb_shootdown.c | Busy-wait infinito esperando IPI ack de otras CPUs: si una CPU no responde, el kernel se cuelga permanentemente | Se agrego timeout de ~1M iteraciones con break y warning via kprintf
- [2026-03-28] | kernel/lapic.c | Calibracion PIT sin timeout: si el PIT no responde, el kernel se cuelga en busy-wait infinito | Se agrego timeout (0xFFFFFF iteraciones) con fallback a valor estimado de ticks/sec
- [2026-03-28] | kernel/mutex.c | Race condition en priority inheritance: saved_priority se sobreescribia en el primer boost con la prioridad actual del owner (posiblemente ya boosteada por otro mutex), corrompiendo la restauracion | Se elimino la sobreescritura en el path de boost; saved_priority solo se guarda al adquirir el mutex
- [2026-03-28] | kernel/pmm.c | pmm_ref_get leia ref_count sin spinlock, a diferencia de ref_inc/ref_dec, causando posible lectura inconsistente en SMP | Se agrego spin_lock_irqsave/restore alrededor de la lectura, consistente con las demas operaciones de ref_count
- [2026-03-28] | kernel/ext2.c | memcpy del superblock sin bounds check sobre fs.disk_size: si el disco era mas pequeno que offset + sizeof(superblock), memcpy leia fuera de limites | Se agrego verificacion de tamano antes del memcpy, retornando -1 si el disco es insuficiente
- [2026-03-28] | kernel/xhci.c | Tres busy-wait loops con for(i<100000) sin feedback en timeout: si el hardware no responde, el loop termina silenciosamente y el codigo continua en estado invalido | Se reemplazo por while(--timeout>0) con check explicito; si timeout==0 se loguea error y retorna -1
- [2026-03-28] | kernel/kmalloc.c | Cast uint64_t→uint32_t sin validacion en kfree y kmalloc_usable_size: si el header estaba corrompido, order podia ser enorme causando memset/shift fuera de limites | Se agrego bounds check order > PMM_MAX_ORDER con log y return seguro en ambas funciones
- [2026-03-28] | kernel/sched.c | watchdog_ticks se incrementaba pero nunca se usaba para matar tareas colgadas: una tarea en loop infinito sin yield acaparaba la CPU indefinidamente | Se agrego enforcement en sched_tick: si una tarea (TID>1) excede 10000 ticks sin ceder, se marca TASK_DEAD y se encola para reap
- [2026-03-28] | kernel/sched.c | task_create no liberaba el TCB si pmm_alloc_pages_zero fallaba despues de alloc_tcb, causando leak permanente de slot en tcb_bitmap | Se agrego free_tcb(t) antes del return -ENOMEM en el path de fallo de stack
- [2026-03-28] | kernel/sched.c | sched_init buscaba idle task por &task_pool[1] (slot hardcodeado), lo cual se rompe si alloc_tcb cambia orden de asignacion | Se cambio a busqueda por TID retornado por task_create, con KASSERT si no se encuentra
- [2026-03-28] | kernel/sched.c | sched_init no verificaba retorno de alloc_tcb para init_task, un NULL dereference silencioso si 64 slots estan llenos | Se agrego KASSERT(init_task != 0)
- [2026-03-28] | kernel/sched.c | task_join leia target->finished y target->join_wq sin lock, race con task_exit y reaper que podia dejar target como dangling pointer | Se envolvio find_task + finished check + join_wq assignment dentro de spin_lock_irqsave(&sched_lock)
- [2026-03-28] | kernel/vmm.c | vmm_unmap_page no tomaba vmm_lock, race condition con vmm_map_page concurrente que podia corromper page tables | Se agrego spin_lock_irqsave/restore alrededor del walk y clear de PTE
- [2026-03-28] | kernel/vmm.c | vmm_virt_to_phys no tomaba vmm_lock, lectura inconsistente si map/unmap ocurrian concurrentemente | Se agrego locking con goto-based cleanup para todos los early returns
- [2026-03-28] | kernel/vmm.c | vmm_map_range_huge no tomaba vmm_lock, escrituras directas a PD entries sin sincronizacion | Se agrego spin_lock_irqsave/restore alrededor del loop completo
- [2026-03-28] | kernel/kmalloc.c | Quarantine flush en kfree modificaba free list y slab list de old_cls sin tomar classes[old_cls].lock cuando old_cls != cls_idx, data race en SMP | Se reestructuro quarantine: extraccion de evicted bajo quarantine_lock, devolucion a free list bajo el lock de la clase correcta
- [2026-03-28] | kernel/kmalloc.c | Double-free detection leia slab->obj_size y memoria del slot sin lock, posible lectura inconsistente bajo contention | Se movio el check dentro del spin_lock de la clase, con unlock antes del KASSERT(0)
- [2026-03-28] | kernel/string.c | memmove backward path usaba copia byte-a-byte, ~8x mas lento que forward path para buffers grandes | Se optimizo con rep movsq + STD flag para bulk backward, manteniendo byte-a-byte solo para residuos y buffers < 8 bytes
- [2026-03-28] | kernel/main.c | framebuffer pointer de Limine no se validaba contra NULL antes de usarlo en console_init | Se agregaron KASSERT(fb != NULL) y KASSERT(fb->address != NULL)
