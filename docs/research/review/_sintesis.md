# Review síntesis — ALZE OS kernel

**Fecha:** 2026-04-22
**Input:** 6 agentes paralelos sobre `/root/repos/alze-os` (14K LOC, 96 archivos kernel). 0 fallos.

- [`boot_build.md`](boot_build.md) — Makefile, linker, limine, main, GDT/IDT, early console (300 L)
- [`memory.md`](memory.md) — PMM, VMM, kmalloc, bitmap_alloc, VMA, mempressure, TLB (188 L)
- [`scheduling_sync.md`](scheduling_sync.md) — sched, context_switch, locks, atomics, waitq/workq/ktimer, cpuidle (197 L)
- [`irq_hal_drivers.md`](irq_hal_drivers.md) — PIC/LAPIC, HAL, kb, xhci, pci, uart, panic, SSP, primitivas (203 L)
- [`fs_storage.md`](fs_storage.md) — VFS, ext2, devfs, ramdisk, string (156 L)
- [`tests_security.md`](tests_security.md) — tests.c, runtime_tests, selftest, klog, SSP policy, PROGRESS/ERRORES (193 L)

**Total:** 1237 líneas de review. ~60 issues concretos con file:line.

---

## Tabla de verdicts por área

| Área | Verdict | Issues críticos | Fortaleza principal | Bloqueador #1 |
|---|---|---|---|---|
| **boot_build** | ⚠️ Ship-blocker | 2 | limine protocol + `vmm_audit_wx` como CI gate + `#DF` en IST1 | IDT con solo 4 vectores instalados; los demás = triple-fault silencioso |
| **memory** | ⚠️ Ship-blocker | 3 | ERRORES.md fixes todos in-place, `test_pmm` 29/29 green | Stack guard-pages **no-op** (HHDM 2 MiB huge) + kmalloc **sin redzone** |
| **scheduling_sync** | 🚨 Peligroso para SMP | 4 | Ticket spinlocks correctos, CFS+EDF, priority inheritance | `task_exit` auto-deadlock por nested lock, `wq_wait_timeout` **roto** |
| **irq_hal_drivers** | ⚠️ Ship-blocker | 4 P0 | kref correcto, ringbuf SPSC-correct, HAL como vtable | IDT vectors 2-15/17-21/0xFF sin stub → triple fault |
| **fs_storage** | 🚨 Unsafe contra user data | 2 sistemicos | memmove backward correcto, `u64` promotion en offsets | VFS sin locks + fd table global + adapters ignoran offset |
| **tests_security** | ⚠️ Proceso degradando | 3 proceso | gitleaks 0 findings, `test_pmm` harness real (no mock) | `SELFTEST_MAX=64` con 66 registrados → 2 tests silenciados; panic no re-entrant |

Nadie está merge-ready sin trabajo. Todo el kernel está ship-blocked.

---

## P0 — blockers que matan el kernel en uso real

Estos fallan en bare metal o con carga sintética; deben arreglarse antes de cualquier "usable":

### 1. IDT mayormente vacía — `idt.c:208-214`
Solo instalados vectors 0 (#DE), 1 (#DB? o realmente IRQ0), IRQ1 (kb), 253 (TLB shootdown), 254 (resched). **IRQ2-15, vectors 2-5, 9-12, 15, 17-21 (NMI, #MC, #XM, #XF, #VE, #CP...), y 0xFF (spurious LAPIC)** no tienen gate. Cualquiera que dispare → #GP → #DF → triple fault → reboot silencioso. En prod, un #MC por bitflip ECC mata el host. **Agentes 1 y 4 coincidieron.**

### 2. PT_LOAD flags invertidas en `linker.ld:6-9`
ELF define `PF_X=1, PF_W=2, PF_R=4`. La linker script las trata como bit-0=R. `readelf` confirma `.rodata` PHDR = `E` (solo X), `.data` PHDR = `WE` (W+X). Ahora mismo está disimulado porque el VMM rebuild crea sus propios mappings, pero **cualquiera que lea la ELF y confíe en PHDR flags (depuradores, loaders, auditors) ve W+X .data**. Si mañana se quita el rebuild del VMM, W^X se rompe sin ruido.

### 3. Cadena de crashes potenciales en scheduler
- `sched.c:323-339` `task_exit` sostiene `sched_lock` y llama `wq_wake_all` → éste toma spinlock del wq + intenta `sched_add_ready` (que requiere `sched_lock`) = **nested ticket-spinlock = deadlock del CPU**.
- `waitqueue.c:78-111` `wq_wait_timeout` **está roto**: setea `sleep_until` pero nunca coloca la task en `sleep_queue`. Solo despierta si alguien más la despierta por accidente. Efectivamente un timeout que no existe.
- `sched.c:510-521` `sched_tick` watchdog pone la task en DEAD inline durante la IRQ; el reaper puede liberar el stack **mientras el IRQ return aún lo usa** = UAF.
- `sched.c:389-402` `task_join` guarda `join_wq` en el stack del primer joiner — joiners subsiguientes lo usan → UAF window si el primero retorna temprano.

### 4. xHCI BAR0 sin uncached bits (`xhci.c:187-196`)
Mapeado por HHDM sin `PTE_PCD|PTE_PWT`. El TODO está en el código. En HW real → wedge. QEMU lo disimula.

### 5. ext2 parse hostil (4 HIGH en `fs_storage.md`)
- `ext2.c:77` div-by-zero con `s_blocks_per_group=0`. `ext2.c:114` idem con `s_inodes_per_group=0`.
- `s_inode_size` acepta cualquier u16 no-cero (`ext2.c:69-73`).
- GDT bounds-check solo cubre primer bloque (`ext2.c:89`).
- `s_feature_incompat` **ignorado** — extensiones desconocidas montan silenciosamente.
- Directory `name_len` no validado contra `rec_len` (`ext2.c:173`) → leak de bytes adyacentes vía `print_dir_entry`.

Un kernel que puede trip por imagen hostil en `/boot/ramdisk` es un ship-blocker.

### 6. VFS sin locks + fd table global (`vfs.c:21`)
`vfs_read/write/ioctl/seek/tell` no llaman `rwlock_read_lock` nunca (grep confirmado). fd table es global, no per-task. TODO autoreconocido. Ambos rompen multitasking + VFS.

---

## Temas cross-cutting (mismos síntomas en múltiples áreas)

### A. SMP no está listo — ni remotamente
Agentes 1, 2, 3, 4, 5 detectaron el mismo vicio por separado:
- Console sin lock (agent 1)
- kmalloc `initialized` flag racy (agent 2)
- VMA `vm_space` + `vma_pool_used` sin locks (agent 2)
- `sched_tick` muta estado sin locks "porque IRQs disabled" (agent 3)
- VFS sin locks (agent 5)
- HAL llama directo a PIC en `idt.c:208-214` + `kb.c:158` — bypasses vtable (agent 4)

El scheduler se presenta SMP-aware pero **todo lo accesorio asume UP**. Encender el segundo CPU hoy es carreras en cascada.

### B. Dead code que nunca se wired
- `cpuidle` existe pero idle task llama `sti; hlt` directo — MWAIT detection nunca usada (agent 3)
- `workqueue_process_system()` sin callers en el tree (agent 3)
- `uart_enable_irq` setea IER pero sin `irq_register` ni `pic_unmask(4)` (agent 4)
- `klog_write_level` sin callsites — `LOG_*` macros bypassan severity; `klog_set_level` no filtra nada (agent 6)
- `kprintf_ratelimit` sin callsites (agent 6)
- `lapic_timer_init` existe pero **nunca se llama** — tick sigue siendo PIT/IRQ0 (agent 4)

Patrón: infraestructura construida + olvidada de conectar. Indica mucha heartbeat refactoring y poca feature integration.

### C. Margen de memoria crítico
- Stack guard pages son no-op (HHDM 2MiB, vmm_unmap bail) — **todas las tasks sin guard realmente efectivo** (agent 2)
- kmalloc sin redzone → overflow corrompe free-list (agent 2)
- VMA acepta overlaps silenciosamente (agent 2)
- SMEP/SMAP off — CR4 nunca escrito fuera del panic dump (agent 2)
- PFN 0 no reservado → return 0 ambiguo entre OOM y alloc exitosa (agent 2)
- SSP global, seed TSC-only, hardcoded fallback `0x595E9FBD94FDA766` (agents 4 + 6)
- `mempressure` con 0 callbacks registrados, polled desde idle — dormant (agent 2)

### D. Panic es frágil
- No re-entrant: sin `in_panic` flag; fault dentro de `klog_dump` recursa infinito (agent 6)
- Toma `klog` lock que puede estar held (agent 4)
- No dumpea current task info — placeholder "(see klog dump above)" (agent 6)

### E. Proceso de testing degradando
- `SELFTEST_MAX=64` pero 66 registrados → 2 tests descartados silenciosamente (agent 6)
- PROGRESS dice "29/29", runtime dice "41+5", realidad otra. 3 números discrepan (agent 6)
- Cobertura cero en **xhci (224 LOC), ext2 (485 LOC), pci (196 LOC), mutex/sema/waitqueue, TLB shootdown** — los subsistemas con más entradas en ERRORES.md (agent 6)
- `make test` con `sleep 8 && kill` + grep `[FAIL]` pero runtime suite logea `[ERROR]` → regresión pasa CI (agent 6)
- ERRORES.md stale desde 2026-03-30 pese a 12+ commits después (agent 6)
- PROGRESS.md stale desde 2026-04-06 (agent 6)

---

## Lo que está genuinamente bien

No todo es rojo — las cosas que merecen elogio:

- **PMM**: todos los fixes históricos de ERRORES.md verificados in place. `test_pmm` es real (no mock), linkea contra `pmm.c` actual, 29/29 green.
- **ERRORES.md as discipline**: mientras estuvo vivo, capturó bugs reales repetidos (memcpy bounds, list sentinel, EFER.NXE, IPI timeout+EOI). La cultura funciona.
- **vmm_audit_wx**: gate automático que pasa en test — previene regresiones W^X a pesar del linker.ld roto.
- **Ticket spinlocks** correctos con acquire/release semántica apropiada.
- **Priority inheritance** en mutex y adaptive_mutex.
- **Stack guard pages + canaries + watchdog**: design intent sólido, solo la implementación tiene los bugs arriba.
- **`#DF` usa IST1**: separación correcta de stack para el caso más crítico.
- **`LIMINE_BASE_REVISION(3)` + KASSERT**: correcto handling del protocolo limine.
- **Gitleaks 0 findings** en 144 commits.
- **kref correcto**, list macros safe contra double-remove (self-relink), ringbuf SPSC-correct.
- **memmove backward** optimizado correctamente (ERRORES.md:31 aprendido de incident).

---

## Roadmap de fixes priorizado

Orden sugerido — cada nivel desbloquea el siguiente:

### Sprint 1: boot reliability (P0)
1. Fill IDT gates: vectors 2-5, 9-12, 15, 17-21, 0x27, 0x2F, 0xFF. Stubs genéricos que panic con vector info.
2. Fix `linker.ld` PT_LOAD flags (`PF_R=4, PF_W=2, PF_X=1`).
3. xHCI BAR0 con `PTE_PCD|PTE_PWT`.
4. LAPIC calibration: eliminar silent fallback — panic si PIT timeout.

### Sprint 2: scheduler correctness (P0)
5. `task_exit`: release `sched_lock` antes de `wq_wake_all`; o rediseñar wake path para no requerir sched_lock en el caller.
6. `wq_wait_timeout`: insertar en `sleep_queue` correctamente.
7. `sched_tick` watchdog: diferir el "mark DEAD" a un tasklet/workqueue, no hacerlo inline.
8. `task_join`: WQ en TCB del target, no en stack del joiner.

### Sprint 3: FS safety (P0)
9. Añadir rwlock en VFS ops.
10. fd table per-task (cerrar TODO `vfs.c:21`).
11. ext2: pre-validar superblock (divisores != 0, feature_incompat), validar `name_len <= rec_len`, strict inode_size.
12. Adapters: respetar `offset` del fd.

### Sprint 4: SMP prep (P1)
13. Console lock. VMA lock. kmalloc init atomic.
14. Wire `lapic_timer_init` como tick source.
15. Auditar todas las rutas `current`/`per_cpu` para pin-to-CPU.
16. Wire `cpuidle` real.

### Sprint 5: memory hardening (P1)
17. Stack guard pages reales (split HHDM 4KiB en rangos que requieran guard, o usar arena alternativa).
18. kmalloc redzones + poison on free.
19. SMEP/SMAP enable en `cr4_init`.
20. PFN 0 reservada explícitamente.
21. tlb_shootdown: canal separado para "flush all" vs "flush page N".

### Sprint 6: proceso (P2)
22. `SELFTEST_MAX`: bump o reemplazar por lista dinámica.
23. Unificar conteo de tests (una única fuente de verdad).
24. `make test` debe fallar en `[ERROR]` además de `[FAIL]`.
25. Panic `in_panic` flag + trylock en todos los locks del panic path + current task dump.
26. SSP: per-task canary, re-seed post-main en schedule, quitar hardcoded fallback.
27. Reactivar PROGRESS.md + ERRORES.md como parte del commit workflow.

### Sprint 7: test coverage (P2)
28. Smoke tests para xhci (command ring, event ring).
29. ext2 parse contra fixtures malformadas.
30. Concurrency tests para mutex/semaphore/waitqueue (deterministic scheduling o fuzzing).

---

## Cosas que NO tocar (están correctas)

No gastar tiempo reescribiendo:
- PMM (está completo y testeado)
- Ticket spinlocks
- kref, list macros, ringbuf SPSC
- Priority inheritance
- `#DF` → IST1
- `vmm_audit_wx` gate
- Mutex/adaptive_mutex spin-then-sleep

---

## Mensaje para Ibrahim

**Lo bueno:** el diseño tiene buenas ideas (guard pages, canaries, audit, PMM cuidadoso, priority inheritance). ERRORES.md funcionó mientras se usó. Tests actuales pasan.

**Lo preocupante:** el kernel está a 1-2 sprints de "realmente usable", **no a 1-2 commits**. Hay 3 clases de bug sistémicas (IDT incompleta, SMP assumptions, FS sin locks) que no se arreglan con heartbeat passes. Necesitan un feature sprint con objetivo claro.

**Si tuvieras que cortar:** los Sprints 1-3 (12 items) son estrictamente P0 para bare metal. Sin eso, ALZE OS sigue siendo demo educativa, no sistema. Post-Sprint-3, sí es defendible como "pre-alpha".

**Si volvieras a activar ERRORES.md como disciplina**, los 3 últimos commits "workqueue static init", "ramdisk accessor removal", "pic helper extract" del histórico deberían haber dejado entries — y no los hay. Sugiero: fail-fast pre-commit hook que pida ENTRADA a ERRORES.md o PROGRESS.md en cada commit no-trivial.
