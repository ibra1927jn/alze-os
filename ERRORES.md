# ERRORES.md — Lo que no volvemos a hacer

## Formato
[YYYY-MM-DD] | [archivo afectado] | [error] | [fix aplicado]

---

## Errores registrados
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
