# ERRORES.md — Lo que no volvemos a hacer

## Formato
[YYYY-MM-DD] | [archivo afectado] | [error] | [fix aplicado]

---

## Errores registrados
- [2026-03-28] | kernel/sched.c | bsfq tiene resultado indefinido cuando input == 0, bitmap_find_first podia devolver basura si prio_bitmap era 0 y bitmap_dequeue accedia a un indice invalido | Se agrego check bm == 0 → return -1 en bitmap_find_first, y guard p < 0 en bitmap_dequeue
- [2026-03-28] | kernel/pmm.h | El campo _reserved de struct page se usaba informalmente como ref counter para COW, sin API ni proteccion | Se reemplazo _reserved por ref_count (uint32_t) con API formal: pmm_ref_inc/pmm_ref_dec/pmm_ref_get, protegidas por spinlock
- [2026-03-28] | kernel/vmm.c | vmm_flush_tlb solo invalidaba TLB local, en SMP las demas CPUs mantendrian entradas obsoletas causando corrupcion de memoria | Se implemento infraestructura TLB shootdown via IPI (vector 0xFE), wired a vmm_flush_tlb. No-op seguro en single-core hasta que LAPIC este disponible
- [2026-03-28] | kernel/tlb_shootdown.c | El handler IPI no enviaba EOI al LAPIC, lo que bloquearia futuras interrupciones cuando LAPIC este activo | Se agrego escritura a LAPIC EOI register (0xFEE000B0) al final del handler. Seguro en single-core porque el handler nunca se invoca sin LAPIC
