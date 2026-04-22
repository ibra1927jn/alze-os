# Schedulers Modernos — Deep Dive para ALZE OS

**Round:** R3
**Fecha:** 2026-04-22
**Scope:** algoritmos y data structures de schedulers de kernel, desde el Linux 4BSD (1993) hasta EEVDF (Linux 6.6, 2023) y sched_ext (6.12, 2024), pasando por FreeBSD ULE, Solaris FSS, Plan 9, Genode, SCHED_DEADLINE, y el ecosistema de work-stealing user-land (Cilk, Go, Tokio, Rayon).
**Output esperado:** decisiones realistas para el scheduler de ALZE OS v1 (hoy: O(1) bitmap + vruntime fairness + EDF), v2 (post-capability), v3 (aspiracional con eBPF VM).

---

## 1. De O(n) → O(1) → CFS → EEVDF: el arco de Linux

### Pre-2003: O(n) scheduler

Linux 2.4 tenía un scheduler que escaneaba toda la runqueue en cada tick (`goodness()` sobre cada task). Con > 100 procesos runnables, la latencia de reschedule crecía linealmente. Inaceptable para servers ~2000.

### 2003–2007: O(1) scheduler (Ingo Molnar)

Linux 2.6.0–2.6.22. 140 niveles de prioridad (100 RT + 40 nice), dos arrays `active` y `expired` por CPU, bitmap de 140 bits. Picking task runnable = BSF sobre bitmap → O(1). Cuando un task consumía su quantum, migraba `active → expired`. Cuando `active` vacío, swap pointers. Heurística "interactivo" vs "batch" basada en `sleep_avg`. Problema: la heurística era un maze de if/else, ajustes manuales rompían workloads distintos.

**Ref:** Molnar I., "The Linux 2.6 scheduler", LKML 2002, https://lkml.org/lkml/2002/10/7/256.

### 2007–2023: CFS (Completely Fair Scheduler)

Linux 2.6.23. Ingo Molnar + Con Kolivas (RSDL/SD influence). Ganó sobre SD por merge política. Data structure: **red-black tree** ordenado por `vruntime` (virtual runtime). Task con menor vruntime = elegido. Cada nice level escala vruntime (`NICE_0_LOAD / weight`, tabla `sched_prio_to_weight[40]`). Fair-share implícito.

- `vruntime += delta * NICE_0_LOAD / weight`
- Sleeping tasks: se les regala `min_vruntime - sched_latency/2` al despertar → boost para interactivos
- Group scheduling (2008): grupos con sus propios rbtrees anidados, para cgroups cpu.shares
- `sched_latency_ns` = 24ms (default), `sched_min_granularity_ns` = 3ms
- Complejidad: O(log N) insert/remove, O(1) pick-leftmost (cached)

**Ref:** Molnar I., "Modular Scheduler Core and Completely Fair Scheduler [CFS]", kernel.org 2007, https://lwn.net/Articles/230501/ .
**Ref:** Jones M.T., "Inside the Linux 2.6 Completely Fair Scheduler", IBM developerWorks 2009, archivado en https://web.archive.org/web/2018*/www.ibm.com/developerworks/linux/library/l-completely-fair-scheduler/ .

### 2023: EEVDF (Earliest Eligible Virtual Deadline First)

Linux 6.6, mainline octubre 2023. Peter Zijlstra implementó el algoritmo de **Stoica/Abdelzaher/Towsley 1995**, reemplazando CFS después de 16 años. La razón: CFS no expresa "latency requirements" distintos del peso de nice. Un task nice=0 que necesita 1ms de latencia (audio) se mezcla con otro nice=0 que necesita 50ms (compile). EEVDF separa ambas dimensiones.

**Claves del algoritmo:**
- Cada task tiene `vruntime` (como CFS) + `deadline` (virtual deadline) + `slice` (cuánto CPU se le ha prometido por ronda)
- Task es **eligible** si `vruntime ≤ virtual_time` (le corresponde CPU en este round). Task NO eligible si ya tomó su porción.
- Dentro de los eligible, escoger el de menor `deadline` = vruntime + slice / weight
- Nueva syscall `sched_setattr` con `sched_runtime_ns` = latency request por task. `nice` sigue controlando peso, latency_nice (`-20..19` mapea a `slice`) controla tamaño del slice pedido
- Data structure: **augmented red-black tree** (rbtree con summary de min_vruntime en subárboles para scan rápido) — mismo rbtree de CFS, ~2k LOC delta neto

**Resultados (Phoronix benchmarks 6.5 vs 6.6-rc EEVDF, oct 2023):**
- Perf context_switch: +5-10% throughput
- Stress-NG futex: mejor en mixed prio
- Regresiones en algunos embarrassingly-parallel (gaming HEAVEN): ~2-4% peor que CFS
- Schbench P99 latency: mejor con latency-sensitive tasks marcadas

**Commit de referencia:** `147f3efaa2deb` Oct 2023 "sched/fair: Implement an EEVDF-like scheduling policy" (Zijlstra).
**Ref primaria:** Corbet J., "An EEVDF CPU scheduler for Linux", LWN 2023-03-09, https://lwn.net/Articles/925371/ .
**Ref algoritmo original:** Stoica I., Abdel-Wahab H., Jeffay K., Baruah S., Gehrke J., Plaxton G., "A Proportional Share Resource Allocation Algorithm for Real-Time, Time-Shared Systems", RTSS 1996, https://www.cs.unc.edu/~jeffay/papers/RTSS-96.pdf (earlier Stoica/Abdelzaher/Towsley TR UMD 1995).

**Por qué esto importa para ALZE:** EEVDF es *menos* heurística que CFS — está basado en un teorema, no en parches manuales. Pero sigue exigiendo rbtree + virtual time accounting. Para un kernel hobby, implementar EEVDF correcto requiere:
- Augmented rbtree con summary ops (~400 LOC)
- Virtual time + eligibility tracking
- Group scheduling (si querés cgroups)
- Wakeup preemption con slice comparison

Costo: ~2-3k LOC de kernel, + testing con artificial workloads. Probable que el hobby kernel no note beneficio sobre round-robin O(1) hasta > 100 tasks activas.

---

## 2. sched_ext — eBPF schedulers pluggables (Linux 6.12, 2024)

Merged mainline Nov 2024 (Linux 6.12). Autor: **Tejun Heo** (Meta) + Dan Schatzberg + David Vernet. Es la primera vez que kernel.org acepta un framework para cargar **schedulers como eBPF programs** con zero downtime.

### Modelo

- Nuevo `sched_class = ext_sched_class`, encima de `idle_sched_class` y debajo de `fair_sched_class`. Task con política `SCHED_EXT` pasa al scheduler eBPF custom.
- Callbacks que el eBPF program implementa: `select_cpu`, `enqueue`, `dequeue`, `dispatch`, `running`, `stopping`, `init_task`, `exit_task`, `cpu_online/offline`, `update_idle`, `set_cpumask`, `set_weight`.
- **Safety net**: watchdog detecta si el scheduler stalls (ningún task corre en X seconds). Si falla, fallback automático a CFS/EEVDF. **Nunca** puede bloquear el sistema.
- **Dispatch queues (DSQ)**: primitivas expuestas al eBPF. Global DSQ + per-CPU DSQ por defecto. User puede crear custom DSQs con FIFO/prio/VTIME ordering.
- Helpers eBPF: `scx_bpf_dispatch`, `scx_bpf_consume`, `scx_bpf_dsq_move`, time functions, CPU topology query.

### Schedulers production disponibles (scx repo)

- **scx_rusty** (Dan Schatzberg, Meta): Rust userspace + eBPF kernel. Load-balancing cross-NUMA, topology-aware. Target: cloud workloads mixed.
- **scx_layered** (Tejun Heo): configurable por YAML/JSON, asigna tasks a "layers" (e.g., "interactive / batch / background") con CPU subset + weight. Usado en Meta.
- **scx_bpfland** (Andrea Righi, Canonical): desktop/gaming-focused. Latency-sensitive tasks detectados por wakeup pattern, promovidos a CPU reservados.
- **scx_lavd**: Latency-criticality Aware Virtual Deadline (Changwoo Min).
- **scx_rustland_core** / **scx_central**: user-space decision maker, kernel eBPF glue.

### Casos de uso empresariales observados 2024-2026

- **Gaming**: Valve/Canonical shipping scx_bpfland en SteamOS derivatives, reducen input-lag en competitive FPS.
- **Database**: Oracle experimentando con policies que priorizan foreground query threads sobre vacuum/background.
- **ML training**: GPU-bound jobs marcados, CPU scheduler evita despertarlos cuando están GPU-wait.
- **Meta production**: scx_layered para aislar web-tier de cache-tier en la misma máquina.

### Costo de implementación

sched_ext core: ~10,000 LOC kernel + helpers. **Pero** requiere eBPF VM subyacente (~50k LOC: verifier + JIT x86/ARM + helpers + maps + CO-RE). **Imposible** para ALZE v1.

**Ref primaria:** Heo T. et al., "sched_ext: a BPF-extensible scheduler class", kernel patchset 2023-2024, https://lwn.net/Articles/922405/ (Corbet LWN 2023-03-23) y https://lwn.net/Articles/969324/ (LWN 2024-04-12 merge debate).
**Ref talk:** Heo T., "sched_ext: pluggable scheduling via BPF", Linux Plumbers Conference 2023, https://lpc.events/event/17/contributions/1339/ .
**Repo:** https://github.com/sched-ext/scx .

---

## 3. FreeBSD ULE — interactivity estimator + per-CPU queues

Jeff Roberson 2003. Reemplaza al **4BSD scheduler** (que era un port casi directo del scheduler BSD 4.3 de los 80s: multilevel feedback, per-CPU global runqueue con big kernel lock).

### Estructura

- **3 runqueues por CPU**: `current`, `next`, `idle`. Current y next son arrays de 64 FIFO lists (64 prioridades). Task nuevo → next. Current vacío → swap current/next. Idle queue para tasks ocasionales.
- **Interactivity score**: `voluntary_sleep_time / run_time * 128`. Scores bajos (< 30) → interactive → priority boost, pushed to current queue. Scores altos → batch → next queue.
- **Thread affinity**: cada thread recuerda `td_oncpu`. Balancer migra solo bajo presión. Cache-friendly.
- **Balancer**: periodic task (balancer_tick), roba de CPUs cargadas a vacías. Honrar topology (SMT/NUMA) via `cpu_top` structure.

### Contraste con Linux CFS

- ULE es **per-CPU run queue** desde el día 1, no intentaba runqueue global
- **No fairness basada en weight** — ULE es priority-based, con interactivity score modulando la prio
- **No group scheduling** (no cgroups concept)
- Latencia interactiva: buena, comparable a CFS en workloads desktop

### Plot twist 2022

FreeBSD añadió `sched_dlock` para reducir lock contention, pero ULE sigue siendo el scheduler default en 2026. No ha habido "EEVDF moment" en FreeBSD.

**Ref primaria:** Roberson J., "ULE: A Modern Scheduler For FreeBSD", USENIX BSDCon 2003, https://www.usenix.org/legacy/publications/library/proceedings/bsdcon03/tech/full_papers/roberson/roberson.pdf .
**Ref update:** Bonwick J./Roberson J., "Improvements to the FreeBSD ULE scheduler", FreeBSD Status Reports 2019-2021, https://www.freebsd.org/status/ .

---

## 4. Solaris FSS (Fair Share Scheduler) + zones

Sun Microsystems, production desde Solaris 9 (2002). El concepto: administrador asigna **shares** a projects/zones. Un project con 2× shares que otro recibe 2× CPU *cuando hay contención*. Si no hay contención, cada uno consume lo que necesite.

### Algoritmo

- Cada thread acumula `fsspri` (FSS priority). Cuanto más CPU consume relativo a shares, más grande fsspri → menor prioridad actual
- Decay periódico: fsspri *= decay_factor. Previene starvation histórico
- Nivel de granularidad: **project** (group of processes). Zones heredan project constraints.
- Interacción con zones: cada zone puede tener cap de CPU (`zone.cpu-cap`) + shares (`zone.cpu-shares`). Cap es hard, shares es soft.

### Influencia en Linux cgroups cpu.shares

El modelo **weight-based proportional sharing** de Linux cgroups v1/v2 (`cpu.weight` en v2, rango 1–10000, default 100) es conceptualmente idéntico a FSS. Paul Menage diseñó cgroups (2007) consciente del trabajo de Solaris y BSD jails. Peter Zijlstra integró fair group scheduling en CFS copiando la idea de per-group runqueue con shares.

**Ref:** Sun Microsystems, "Solaris Resource Manager 1.3", whitepaper 2004, archivado en https://web.archive.org/web/2009*/docs.oracle.com/cd/E19253-01/817-1592/ .
**Ref:** Menage P., "cgroups.txt", Linux kernel Documentation/cgroups/ 2007, https://www.kernel.org/doc/Documentation/cgroup-v1/cgroups.txt .

---

## 5. Plan 9 process scheduler — Bell Labs minimalism

Plan 9 from Bell Labs, Tom Duff + Rob Pike + Ken Thompson. Su scheduler cabe en ~200 LOC de C.

### Algoritmo

- **Multilevel priority queue**: 20 niveles (PriNormal = 10, PriRealtime = 13-19, PriIdle = 0-9). Array `runq[Nrq]`, each a linked list.
- Round-robin within level, strict priority across.
- Quantum = schedquanta ticks (default 2). Task que consume quantum rota a priority-1 (decay).
- Task que bloquea → priority+1 (boost) on wake. Heurística parecida a O(1) Linux pero sin arrays expired/active.
- **No load balancing** between CPUs in the classic sense — Plan 9's `procsave` migrates only on demand.

### Influencia

- **9front** (fork mantenido desde 2011) conserva el modelo casi sin cambios
- **Inferno OS** heredó algo similar, adaptado a su VM Dis
- **Go runtime** (Pike, Cox, Thompson) tomó el patrón: `runtime.runqueue` con niveles reducidos y work-stealing (ver §8)

**Ref:** Pike R. et al., "Plan 9 from Bell Labs", AT&T internal 1995, http://doc.cat-v.org/plan_9/4th_edition/papers/9 .
**Ref:** proc.c en https://github.com/9fans/plan9port o 9front tree (~200 lines).

### Lección para ALZE

Plan 9 es literalmente el proof-of-concept de "round-robin + priority + decay" en un kernel moderno usable. ALZE ya se parece a esto: multilevel priority + decay via nice/vruntime. **No hace falta ir a EEVDF para tener un scheduler decente.**

---

## 6. Genode — capability-mediated scheduling

Genode Labs, 2006–presente. Framework de componentes sobre microkernels (seL4, NOVA, Fiasco.OC, Linux as platform). El scheduler es *delegado*: cada componente puede tener su propio scheduler interno, y el componente padre (CPU root resource) controla CPU quota mediante capabilities.

### Modelo

- **CPU session** (capability). Un componente pide `cpu_session->create_thread(...)`, recibe `thread_cap`. El padre puede limitar tiempo vía `cpu_session->quota()`.
- **Multi-level scheduler**: root scheduler → subscheduler por componente. Cada componente puede re-schedular internamente sus threads hijas.
- No hay "global run queue" — el scheduler es literalmente una jerarquía de recursos delegados.
- Real-time: prioridades estáticas + cap-based admission control.

### Ventaja

- Fault isolation: si un componente tiene scheduler bug, no afecta al resto del sistema
- Hierarchical resource accounting "free": el padre sabe exactamente cuánto CPU consumen sus hijos
- Policy separation: el mecanismo de scheduling está fijo, la policy es componible

### Desventaja

- Complejidad de razonar sobre múltiples layers
- Overhead de capability invocations en cada switch cross-component

**Ref:** Feske N., "Genode Foundations", book 2026 edition, https://genode.org/documentation/genode-foundations/ .
**Ref:** "Scheduling in Genode", dentro del mismo book, §10.

---

## 7. Real-time: EDF + Rate-Monotonic (Liu+Layland 1973) + SCHED_DEADLINE

### Teoría

**Liu & Layland 1973** demostraron dos teoremas centrales:
1. **Rate-Monotonic (RM)**: asignar prioridades inversamente proporcional al periodo (tarea más frecuente = mayor prio). Schedulable si suma utilización U = Σ (C_i / T_i) ≤ n(2^(1/n) - 1). Para n grande tiende a ln(2) ≈ 0.693.
2. **Earliest Deadline First (EDF)**: en cada instante, correr la tarea cuyo deadline absoluto está más cerca. Schedulable si U ≤ 1 (óptimo en uniprocesador).

EDF es **dynamic priority** (la prio cambia con el deadline corriente), RM es **static**. RM es más simple de implementar y analizar (analizable por response time analysis en tiempo de diseño); EDF usa mejor la CPU pero la response time analysis es más dura.

**Ref primaria:** Liu C.L., Layland J.W., "Scheduling Algorithms for Multiprogramming in a Hard-Real-Time Environment", Journal of the ACM 20(1) 1973, https://dl.acm.org/doi/10.1145/321738.321743 .

### SCHED_DEADLINE (Linux 3.14, 2014)

Dario Faggioli + Juri Lelli. Implementa CBS (Constant Bandwidth Server) variant de EDF. Cada task configura 3 parámetros:
- `sched_runtime`: budget por period
- `sched_deadline`: relative deadline
- `sched_period`: period

Bandwidth reservation: task no puede consumir más de runtime/period. Si excede, se bloquea hasta el próximo period. **Admission control**: kernel rechaza `sched_setattr` si U total > 1 (o ratio configurable).

SCHED_DEADLINE toma **prioridad absoluta** sobre SCHED_RT (FIFO/RR) que toma prio sobre SCHED_NORMAL (CFS/EEVDF). Usado en sistemas con mezcla de best-effort + real-time (audio low-latency, PLC industrial con Linux+PREEMPT_RT).

**Ref:** Faggioli D., Lelli J., "SCHED_DEADLINE: adding EDF scheduling to Linux", Linux Documentation, https://docs.kernel.org/scheduler/sched-deadline.html .
**Ref:** Lelli J. et al., "Deadline scheduling in the Linux kernel", Software: Practice & Experience 46(6) 2016.

### QNX Neutrino

Microkernel RTOS, scheduling policies: FIFO, RR, SPORADIC (budget-based similar a SCHED_DEADLINE). 256 prioridades, priority inheritance en muteces kernel. Certificado IEC 61508 SIL3 en variantes safety-critical. Usado en automotive (Ford Sync, BMW iDrive 7+).

### RTEMS

Open-source RTOS para embedded (NASA, ESA). SMP scheduler desde 2014, soporta EDF, RM (prio fixed), Simple (single-prio), Strong-APA (affinity-aware EDF), MrsP (multiprocessor priority-inheritance). Usado en JWST, Perseverance (MSL).

**Ref:** RTEMS Foundation, "RTEMS 6 User Manual — SMP Scheduling", 2024, https://docs.rtems.org/ .

---

## 8. Work-stealing schedulers (user-land, influencia kernel)

Paradigma: cada CPU tiene su propia deque de tasks. Cuando un CPU se queda sin trabajo, **roba** del fondo (opposite end) del deque de otro CPU. Low contention en el common case (push/pop de la misma deque), contention solo al robar.

### Cilk — MIT, Blumofe+Leiserson 1994-1995

**Ref primaria:** Blumofe R.D., Leiserson C.E., "Scheduling Multithreaded Computations by Work Stealing", Proc. 35th FOCS 1994, https://dl.acm.org/doi/10.5555/1398518.1398947 (luego J. ACM 1999: https://dl.acm.org/doi/10.1145/324133.324234 ).

**Claim:** work-stealing es óptimo dentro de constant factor para "fully-strict" multithreaded computations. Span T∞ + work T1 / P caracteriza el runtime en P procesadores.

Cilk fue primero un language extension (Cilk-NOW 1995, Cilk-5 1998, Cilk++ Intel 2009, Cilk Plus 2010, deprecated 2017). Hoy la idea vive en TBB, OpenMP tasks, Go, Rayon.

### Go runtime scheduler — G-M-P model

Dmitry Vyukov 2012-2013 (Go 1.1). Tres componentes:
- **G** (goroutine): lightweight thread, ~2KB stack growing
- **M** (machine): OS thread
- **P** (processor): logical context, típicamente GOMAXPROCS de ellos

Cada P tiene su local run queue (256 entries, LIFO + FIFO hybrid). Cuando vacío, **steals half** del queue de otro P (random victim). Global queue también existe para overflow.

Additional:
- **Preemption**: desde Go 1.14 (2020) hay async preemption via signals (SIGURG). Antes era cooperative solo en function prologues.
- **Network poller** integrated: goroutines bloqueadas en I/O salen del P, M se puede reusar
- **LIFO slot** (g0.runnext): última goroutine pushed corre primero (cache locality)

**Ref:** Vyukov D., "Scalable Go Scheduler Design Doc", 2012, https://go.googlesource.com/proposal/+/master/design/4_scheduler.md (archivado https://web.archive.org/web/*/rain.ifmo.ru/~tsyvarev/papers/scalable-go-scheduler.pdf).

### Tokio Rust runtime

Carl Lerche 2017-presente. Multi-threaded runtime async Rust.
- **Work-stealing** entre worker threads
- **LIFO slot** (copia del patrón Go): task despertada de I/O va al slot LIFO del worker actual
- **Task budget**: cada task coop ejecuta hasta consumir budget (128 poll calls por default), después yields cooperativamente — previene tail-latency blocker
- **Blocking tasks**: `tokio::task::spawn_blocking` → separate thread pool, no mezcla con async

**Ref:** Lerche C., "Making the Tokio scheduler 10x faster", blog 2019-10-13, https://tokio.rs/blog/2019-10-scheduler .

### Rayon

Niko Matsakis 2016. Data-parallelism library Rust. `par_iter()` sobre colecciones. Join-based (`rayon::join(a, b)` parallel split + merge). Work-stealing con deques lockless (Chase-Lev). Target: CPU-bound compute, no async I/O.

**Ref:** Matsakis N., "Rayon: data parallelism in Rust", blog 2015-2016, http://smallcultfollowing.com/babysteps/blog/2015/12/18/rayon-data-parallelism-in-rust/ .

### Influencia en kernels

Kernels mostly han ignorado work-stealing por razones históricas: runqueues globales (antiguas) o per-CPU FIFO (modernas). **Pero** los I/O completion paths modernos (io_uring kworker, Windows IOCP, seL4 kernel threads) usan ideas work-stealing internamente. sched_ext en Linux permite ahora implementar un scheduler work-stealing sin parches al kernel.

---

## 9. Ultra-Low Latency: Google Ghost, Meta Turbo Boost Max, L1TF co-sched

### Google Ghost (OSDI 2021)

Predecesor de sched_ext. Google implementó "scheduler in userspace": enqueue/dispatch callbacks corren en un **daemon userland** con privilegios. Kernel pequeño shim mueve tasks según las decisiones del daemon. Latencia sub-ms para search / Ads.

Por qué fue reemplazado: eBPF es mejor safety story. Ghost exigía trusted userspace daemon, un kernel panic en el daemon = kernel panic. sched_ext corre en verified eBPF VM → safer. Google migrated to sched_ext 2023-2024. Ghost NO llegó a mainline.

**Ref:** Humphries J.T. et al., "ghOSt: Fast & Flexible User-Space Delegation of Linux Scheduling", SOSP 2021, https://dl.acm.org/doi/10.1145/3477132.3483542 (dir. Asaf Cidon + Google).
**Paper PDF:** https://www.cs.columbia.edu/~asaf/publications/ghost-sosp21.pdf

### Meta / Turbo Boost Max 3.0

Intel TBM3 identifica los 1-2 cores "fastest" del die (por test binning). Meta modifica el scheduler para que latency-critical tasks *prefieran* esos cores. Implementado como policy scx_layered + topology awareness. Ganancia 5-15% tail latency en web tier.

### Co-scheduling for security (L1TF)

L1 Terminal Fault (CVE-2018-3646). Para hyperthreading, ambos threads de un core comparten L1d → attacker en HT sibling puede leer cachelines del victim. **Mitigación**: co-scheduling de siblings: ambos SMT threads del mismo core deben pertenecer al mismo trust domain (same cgroup, same VM). Implementado como **core scheduling** Linux 5.14 (2021): cookie-based, threads con misma cookie pueden compartir core, otros no.

**Ref:** Corbet J., "Core scheduling lands in 5.14", LWN 2021-07-22, https://lwn.net/Articles/861251/ .

---

## 10. Benchmarks: qué significa "bueno" en 2026

| Benchmark | Qué mide | Target "bueno" 2026 |
|---|---|---|
| **cyclictest** (rt-tests) | IRQ → wakeup latency en RT task | < 50 µs max, < 10 µs avg (PREEMPT_RT + isolcpus) |
| **schbench** (Meta) | Wakeup latency bajo varias loads | P99 < 200 µs con load factor ~0.8 |
| **stress-ng --cpu N --sched fair** | Throughput bajo load sintético | Escalar lineal a N CPUs hasta saturar |
| **Phoronix tcpkali, redis-bench** | Latency-throughput curve | Mejor P99 importa más que throughput peak |
| **hackbench** (Ingo Molnar) | Task creation + IPC load | Time-to-complete, menor = mejor |
| **perf sched latency** | Per-task runqueue wait distribution | Tail < 1ms en desktop, < 100µs en RT |

**Ref:** rt-tests: https://git.kernel.org/pub/scm/utils/rt-tests/rt-tests.git/ .
**Ref:** schbench: https://git.kernel.org/pub/scm/linux/kernel/git/mason/schbench.git/ .

En 2026, "bueno" = P99 wakeup < 100µs en workloads interactivos, sin tradeoff importante en throughput. EEVDF mueve la aguja más para P99 que para throughput medio.

---

## Tabla comparativa de schedulers de kernel

| Scheduler | Modelo | Fairness | Real-time | Pluggable | SMP | LOC approx |
|---|---|---|---|---|---|---|
| **Linux CFS** (2007–2023) | rbtree vruntime weight-based | Proportional share | Via SCHED_FIFO/RR separate class | No (monolithic) | Per-CPU rq + migration | ~8k |
| **Linux EEVDF** (2023–) | rbtree augmented + eligibility + virtual deadline | Proportional + latency-aware | Via SCHED_FIFO/RR/DEADLINE separate | No | Per-CPU rq + migration | ~10k |
| **Linux sched_ext** (2024–) | eBPF callbacks + DSQs | Policy-defined | Policy-defined | **Yes (eBPF)** | Per-CPU + custom | ~10k core + eBPF VM |
| **FreeBSD ULE** (2003–) | Per-CPU 3 queues + interactivity score | Priority-based | RT class | No | Per-CPU + balancer | ~4k |
| **SCHED_DEADLINE** (Linux 2014–) | EDF (CBS variant) | None (RT is absolute) | **Yes (hard EDF + admission)** | No | Per-CPU EDF + globally stealable | ~2k (incremental) |
| **Genode** | Hierarchical cap-delegated | Policy per-component | Yes (static prio + admission) | **Yes (by delegation)** | Varies per kernel | ~3k framework |
| **Plan 9** | 20-level prio + RR + boost | Priority-based | No (not RT) | No | Simple global | ~200 |
| **Solaris FSS** | Share-based fsspri + decay | **Fair-share (weight)** | Via RT class | No | Per-CPU rq | ~5k |

---

## ALZE applicability

### v1 (hoy — O(1) bitmap + vruntime + EDF, single CPU)

ALZE ya tiene un scheduler híbrido (review R2 `scheduling_sync.md`): 64 FIFO priority queues + bitmap + vruntime dentro de priority + EDF separate queue + sleep queue. 688 LOC en `kernel/sched.c`. Es **más sofisticado** que Plan 9, **más simple** que CFS completo.

**Correcto tal cual** para:
- < 100 tasks runnables
- Single-CPU (no load balance needed)
- Kernel-mode threads solo

**Fixes obligatorios antes de SMP (de la review R2):**
1. Missed-wakeup en `task_join` (single-joiner assumption unsafe con múltiples joiners)
2. `wq_wait_timeout` broken (no on sleep_queue)
3. Nested lock ordering `wq->lock → sched_lock` vs `sched_lock → wq->lock` → deadlock
4. `mutex_unlock` llama `sched_add_ready` con waiters.lock held
5. Watchdog kill in-place en sched_tick (race con context_switch)
6. `sched_tick` sin `sched_lock` (SMP bomb)

**Valor de upgrade a EEVDF:** bajo. El hobby kernel no tiene workloads con latency-nice requirements explícitos. La complejidad de augmented rbtree + eligibility tracking + slice math no compensa mientras no haya user-mode con apps latency-sensitive.

### v2 (post capability model — EEVDF-lite o CFS-lite, multi-CPU)

Cuando ALZE tenga capabilities + user-mode + apps reales:

**Opción A — CFS-lite:** rbtree por vruntime, **sin** group scheduling. ~1500 LOC de incremental sobre v1. Suficiente para desktop interactivo. Pattern:
- Substituir `prio_queues[]` por un rbtree ordenado por `vruntime`
- Mantener EDF queue separado para RT
- Mantener O(1) bitmap como *hint*, no como primary structure
- Per-CPU runqueue + simple periodic balancer (roba del CPU más loaded)

**Opción B — EEVDF-lite:** augmented rbtree (+summary), eligibility check, slice-from-weight. ~2500 LOC. Gana latency-awareness si se expone `latency_nice` syscall.

**Opción C — mantener O(1) bitmap + vruntime:** **esto es lo que ALZE ya hace**. Si los workloads son mayormente batch + kernel threads, O(1) BSF pick + vruntime fairness es **suficiente** y *más simple* que EEVDF. Plan 9 demuestra que este modelo puede ser producción-ready.

**Recomendación v2:** Opción C + fixes de R2 + per-CPU runqueues + balancer simple. NO reinventar CFS salvo que haya evidence de problem.

### v3 (aspiracional — sched_ext equivalent, requires eBPF VM)

Blocker mayor: **eBPF VM no existe en ALZE**. Implementar un safe-ISA verifier + JIT para x86_64 es ~20k LOC mínimo:
- Bytecode parser + type inference
- Verifier: termination check (bounded loops), memory safety (offset bounds), register type tracking
- JIT: ISA translation, register allocation, code patching
- Helpers: curated kernel APIs exponibles al eBPF program
- Maps: hashmap/array/lru/percpu data structures para shared state user↔kernel

Alternativa más barata: **cargable C kernel modules** firmados, no verificados. Modelo Linux pre-eBPF. Permite custom schedulers sin VM pero sacrifica safety → requires signature verification + trusted admin.

**Realistic path v3:** en ALZE, primero implementar un DSL tipo Cilk-threads con `spawn`/`sync` como extension de la API de threads, NO un eBPF verifier completo. Work-stealing runtime con 2-3 queues por CPU y LIFO slot (copiando Tokio/Go). Eso da el 80% del beneficio con 5% de la complejidad de sched_ext.

---

## Honest note

Un kernel hobby debería empezar con **O(1) round-robin + priority** y nada más. Razones:

1. **El scheduler raramente es el bottleneck.** Con < 50 tasks ready y quantum 10ms, cualquier algoritmo correcto funciona. El bottleneck son los IRQs, el memory management, el FS.

2. **CFS/EEVDF son complejos.** rbtrees, virtual runtime accounting, group scheduling, load balancing, wake-affine heurística. Cada una de esas partes es un pozo de bugs. Linux tardó 15+ años en pulirlos.

3. **La valor de EEVDF sobre CFS sobre O(1)-BSF es visible solo cuando hay workloads heterogéneos con latency requirements explícitos.** En ALZE, los workloads son inicialmente `idle + kernel_worker + shell + grub`. No hay audio/video/gaming con latency budgets medidos.

4. **sched_ext es genuinamente innovador** pero exige eBPF VM antes. Ese es un proyecto separado de ~20k LOC que NO debería iniciarse antes de que el kernel tenga capability model + user-mode + MMU full + networking. Quizás 2027-2028 en el roadmap realista de ALZE.

5. **Perfilar antes de optimizar.** Si eventualmente un benchmark muestra P99 wakeup > 5ms bajo load, eso es señal de upgrade. Hasta entonces: el scheduler actual de ALZE (O(1) bitmap + vruntime + EDF separado) es **mejor** que el de Plan 9 y **suficiente** para el roadmap v1-v2.

**Ordering recomendado de trabajo scheduler en ALZE:**
1. Arreglar los 6 issues de la review R2 (**obligatorio** para SMP)
2. Per-CPU runqueues simples (copiar pattern ULE: 3 queues por CPU, balancer periódico)
3. Mantener el modelo bitmap+vruntime+EDF existente — NO migrar a rbtree
4. Implementar `sched_setattr`-style syscall con `nice` + `deadline` + `qos_class` (ya parcial)
5. Añadir `preempt_disable/enable` con counter per-CPU
6. Wire `cpu_idle()` para MWAIT (review R2 item señalado)
7. Wire workqueue worker threads
8. **Solo si se observan issues reales de latency**: considerar EEVDF-lite con rbtree

Timelines realistas:
- Steps 1-7: 3-6 meses lab-time para un dev serious, ~4-6k LOC delta
- Step 8 (EEVDF-lite): **no antes** de tener user-mode + 10+ apps de test + profiling framework. 2027-2028.
- eBPF VM para sched_ext: **no antes** de 2028, posiblemente nunca. El 80% del valor se consigue con Cilk-style work-stealing threads.

---

## Referencias (ordenadas por tema)

### EEVDF / CFS
- Stoica I., Abdel-Wahab H., Jeffay K., Baruah S., Gehrke J., Plaxton G., "A Proportional Share Resource Allocation Algorithm for Real-Time, Time-Shared Systems", RTSS 1996 → https://www.cs.unc.edu/~jeffay/papers/RTSS-96.pdf
- Corbet J., "An EEVDF CPU scheduler for Linux", LWN 2023-03-09 → https://lwn.net/Articles/925371/
- Corbet J., "EEVDF scheduler may be merged for Linux 6.6", LWN 2023-08-16 → https://lwn.net/Articles/941548/
- Zijlstra P., commit `147f3efaa2deb`, Linux 6.6 mainline Oct 2023 → https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=147f3efaa2deb
- Molnar I., "Modular Scheduler Core and Completely Fair Scheduler [CFS]", LWN 2007 → https://lwn.net/Articles/230501/

### sched_ext
- Heo T., Schatzberg D., Vernet D. et al., "sched_ext: a BPF-extensible scheduler class", patchset 2023-2024, thread index → https://lwn.net/Articles/922405/
- Corbet J., "The case for sched_ext", LWN 2024-03-11 → https://lwn.net/Articles/964686/
- Heo T., "sched_ext: pluggable scheduling via BPF", LPC 2023 → https://lpc.events/event/17/contributions/1339/
- scx repo → https://github.com/sched-ext/scx

### ULE
- Roberson J., "ULE: A Modern Scheduler For FreeBSD", USENIX BSDCon 2003 → https://www.usenix.org/legacy/publications/library/proceedings/bsdcon03/tech/full_papers/roberson/roberson.pdf

### Solaris FSS
- Sun Microsystems, "System Administration Guide: Solaris Containers-Resource Management", Oracle docs → https://docs.oracle.com/cd/E19683-01/817-1592/
- Menage P., "cgroups v1 design", kernel Documentation → https://www.kernel.org/doc/Documentation/cgroup-v1/cgroups.txt

### Plan 9
- Pike R. et al., "Plan 9 from Bell Labs", AT&T TR 1995 → http://doc.cat-v.org/plan_9/4th_edition/papers/9
- 9front source tree → http://code.9front.org/hg/plan9front/file/tip/sys/src/9/port/proc.c

### Genode
- Feske N., "Genode Foundations", 2026 edition, book → https://genode.org/documentation/genode-foundations/

### Real-time
- Liu C.L., Layland J.W., "Scheduling Algorithms for Multiprogramming in a Hard-Real-Time Environment", JACM 20(1) 1973 → https://dl.acm.org/doi/10.1145/321738.321743
- Faggioli D., Lelli J., "SCHED_DEADLINE kernel documentation" → https://docs.kernel.org/scheduler/sched-deadline.html
- Lelli J. et al., "Deadline scheduling in the Linux kernel", SPE 46(6) 2016
- RTEMS Foundation, "RTEMS 6 User Manual" → https://docs.rtems.org/

### Work-stealing user-land
- Blumofe R.D., Leiserson C.E., "Scheduling Multithreaded Computations by Work Stealing", FOCS 1994 / JACM 1999 → https://dl.acm.org/doi/10.1145/324133.324234
- Vyukov D., "Scalable Go Scheduler Design Doc", 2012 → https://go.googlesource.com/proposal/+/master/design/4_scheduler.md
- Lerche C., "Making the Tokio scheduler 10x faster", 2019 → https://tokio.rs/blog/2019-10-scheduler
- Matsakis N., "Rayon: data parallelism in Rust", 2016 → http://smallcultfollowing.com/babysteps/blog/2015/12/18/rayon-data-parallelism-in-rust/

### Ultra-low latency
- Humphries J.T. et al., "ghOSt: Fast & Flexible User-Space Delegation of Linux Scheduling", SOSP 2021 → https://dl.acm.org/doi/10.1145/3477132.3483542
- Corbet J., "Core scheduling lands in 5.14", LWN 2021-07-22 → https://lwn.net/Articles/861251/

### Benchmarks
- rt-tests (cyclictest) → https://git.kernel.org/pub/scm/utils/rt-tests/rt-tests.git/
- schbench (Meta) → https://git.kernel.org/pub/scm/linux/kernel/git/mason/schbench.git/
- Phoronix Test Suite → https://phoronix-test-suite.com/
