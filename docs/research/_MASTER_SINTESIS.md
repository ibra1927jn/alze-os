# Master Síntesis — alze_os research completa (R1+R2+R3+R4+R5)

**Fecha:** 2026-04-22
**Input total:** 32 agentes paralelos en 5 rondas, ~12,870 líneas research, ~40 archivos.

| Ronda | Foco | Agentes | Líneas | Archivos |
|---|---|---|---|---|
| R1 | Paisaje OS families | 5 | 939 | 5 |
| R2 | Code review ALZE kernel | 6 | 1,438 | 6 |
| R3 | Subsistemas kernel modernos | 7 | 3,374 | 7 |
| R4 | OSes missed deep | 7 | 3,507 | 7 |
| R5 | Cross-cutting systems | 7 | 3,612 | 7 |
| **Total** | **21 OSes + 7 subsistemas + 7 cross-cutting** | **32** | **12,870** | **32 research + 5 sintesis + 1 master** |

Este doc **reemplaza** la pregunta "¿qué debe ser ALZE OS?" con respuesta concreta.

---

## Target reiterado

`/root/repos/alze-os` — x86_64 C99 + asm, limine bootloader, Makefile + linker scripts, ~15k LOC actual per R3/rust_in_kernel check. Review R2 encontró 60+ issues concretos, 12 P0 en Sprints 1-3. Ninguna área merge-ready.

**Acciones de cumplimiento fuera de código (urgentes)**:
1. **ADD LICENSE file** (ISC, 8 líneas) — descubierto por R4/bsd_family. Sin license nadie puede contribuir legalmente.
2. **Scope decision** (1 página): ¿hobbyist learning OS vs ship apps ambition? Esta decisión afecta los próximos 15-20 años.

---

## 7 Decisiones críticas arquitectónicas (commit antes de más código)

### Decisión #1 — Scope & ambition level
ALZE es:
- (A) **Hobbyist learning project**, 5-10 años de iteración por fun. Narrow scope (shell + basic editor + network ping). Meta: enjoyment + blog posts + YouTube vlogs (SerenityOS model).
- (B) **Ship real OS** con production users. Timeline 15-20 años sin backing (Redox track record). Meta: community of 20-100 users.
- (C) **Research vehicle** para patterns concretos (9P native, Zircon-lite capabilities, WAFL-inspired CoW). Meta: artículos / papers.

**Recomendación**: (A) + (C) combinación. Ship YouTube vlogs + narrow scope + 1-2 diferenciadores técnicos genuinos. Evitar (B) por timeline unrealistic.

### Decisión #2 — Kernel model
- ACL monolítico (Linux-like) actual
- → **Capability-inspired** v2 (Zircon-lite handle + rights, no verification) ← **RECOMENDADO**
- → seL4-verified aspiracional (institucional only, skip)

### Decisión #3 — Primary IPC mechanism
- POSIX pthread + UNIX domain sockets — default, soso
- → **9P protocol nativo** (~1000 LOC) ← **REAL diferenciador para hobby OS 2026**
- → Zircon channels + FIDL — requiere IDL toolchain

### Decisión #4 — Memory model
- Demand paging + fork + swap + THP (Linux emulation) ← NO. Saves ~5k LOC
- → **Flat 1 GB huge-page kernel ID map + 2 MB user pages, no fork, no swap** ← **v1 decision firme**
- → 5-level paging v3 si ever

### Decisión #5 — Driver ecosystem v1
- Real HW (NVMe + xHCI + Wi-Fi) ← NO. Specs 1000+ páginas cada. Años de battle.
- → **virtio-only policy**: virtio-blk + virtio-net + virtio-console ← **v1 concrete**
- → Real HW v3 aspiracional

### Decisión #6 — Language
- **C99 mantener** ← **v1+v2**. Current ~15k LOC.
- → Rust bilingual per NEW driver opcional ← v2 experimento
- → Full Rust rewrite ← NO (18-24 meses perdidos per Redox)

### Decisión #7 — Engine evolution strategy
- FromSoftware model (de alze_engine research) aplica aquí también: **evolve one kernel 15+ años, no rewrite**.
- Archive format estable, syscall ABI estable, driver ABI estable.
- Don't refactor for purity. Incremental commits.

---

## Stack ALZE-OS definitivo v1 (~17k LOC target, current ~15k)

Por capa, con LOC estimate + cross-ref a R1-R5:

### Boot + early init (~800 LOC ALZE ya tiene, mantener)
- **Limine rev 4** (current). R5/boot_init §1.
- Kernel `_start` → GDT/IDT/LAPIC init → mm_init → irq_init → scheduler_init → kmain.
- **ADD: populate IDT completamente** (R2 blocker #1-4).
- **ADD: SMAP/SMEP CR4 bits** en boot (R5/security_hardening).
- **ADD: KASLR offset randomization** via Limine (~200 LOC).

### Memory management (~3k LOC, current ~2.5k)
- **Flat 1 GB huge-page kernel ID map** (via limine HHDM + own overrides).
- **PMM (Physical Memory Manager)**: simple bitmap or buddy. Current ALZE has one, fix R2 findings (PFN 0 reserve, TLB shootdown).
- **VMM for user process**: 4 KB pages, 2 MB huge pages opt-in. No demand paging. No fork COW.
- **kmalloc slab-like**: ~300 LOC. Per-CPU magazines v2.
- **TLB shootdown via IPI** (fix R2 memory.md finding).
- NO swap. NO THP. NO memcg v1.

### Interrupts + drivers (~2.5k LOC)
- **LAPIC + IOAPIC + MSI-X** v2 (basic LAPIC current).
- **virtio-blk** driver (~500 LOC, r5/drivers_modern).
- **virtio-net** driver (~600 LOC).
- **virtio-console** driver (~200 LOC).
- **PS/2 keyboard + PCI legacy UART** (current, keep for bare-metal fallback).
- **xHCI stub** (current, simplify o remove — virtio-console suficiente).

### Scheduler (~1.5k LOC, current ~1k)
- **Priority-based round-robin** current. Keep v1.
- **Fix R2 scheduling_sync 5 lock-order bugs**.
- **Per-CPU runqueues + load balancer** v2 (~1k LOC add).
- No CFS/EEVDF v1. No sched_ext ever (requires eBPF 20k LOC VM).

### Synchronization (~500 LOC)
- **Spinlock + mutex + seqlock** primitives (current mostly).
- **Fix R2 lock-ordering bugs** primero.
- **rwlock + qspinlock** v2.
- NO RCU v1 (r3/rcu_synchronization — grace period machinery = 2k LOC).

### Filesystem (~2k LOC, current ~1.5k)
- **ext2-lite estable** (fix R2 fs_storage 7 bugs: locks, fd-table, div-by-zero, GDT bounds, name_len/rec_len).
- NO CoW v1. NO journal v1. Stability first.
- v2: + journal + metadata CRC (~2k LOC add).
- v3: WAFL-inspired CoW-lite + snapshots (~5-8k LOC).

### VFS + syscalls (~2k LOC)
- **Syscall dispatch table** (~300 LOC current).
- **VFS layer thin**: open/read/write/close/lseek/stat/unlink/mkdir/rmdir/dup2/pipe/fstat.
- **ADD: pledge-style syscall allow-mask per process** (~200 LOC).

### Security (~500 LOC)
- SSP current (keep).
- **ADD SMAP + SMEP** (~50 LOC CR4 + fault handler updates).
- **ADD KASLR** (~200 LOC).
- **ADD SSP per-task random canary** (R2 finding: current global canary weak).
- NO KPTI, NO CFI, NO CET, NO PAC v1.

### Testing + klog (~1.5k LOC, current ~1k)
- **kunit-lite test framework** (current).
- **klog ring buffer** (current, keep).
- **ADD: SMP concurrency torture tests** (R2 finding).

### Userspace bootstrap (~1.5k LOC)
- **init process** minimal.
- **Shell (tiny, no POSIX full compat)** — reference reshitto or similar.
- **coreutils-lite**: ls, cat, echo, mkdir, rm (10-15 tools).

### Optional diferenciador (~1k LOC)
- **9P protocol server** nativo (R4/plan9_inferno §15).
- Opens 9front + plan9port + drawterm userland.
- **This is the ALZE "unique angle" vs other hobby OSes.**

**TOTAL v1 estimate**: ~17k LOC. Current ~15k. **Remaining ~2k LOC + massive refactor + bug fixes**.

---

## LOC budget v1 → v2 → v3

| Layer | v1 LOC | v2 LOC | v3 LOC | Total |
|---|---|---|---|---|
| Boot + init | 800 | +200 (uACPI integration glue) | — | 1,000 |
| Memory management | 3,000 | +1,000 (per-CPU allocator + huge pages user) | +2,000 (streaming / large pages) | 6,000 |
| Interrupts + drivers | 2,500 | +3,000 (MSI-X + IOMMU + more virtio) | +10,000 (real HW NVMe, xHCI, Wi-Fi stub) | 15,500 |
| Scheduler | 1,500 | +1,000 (per-CPU + balancer + threaded IRQ) | +500 (SCHED_DEADLINE-lite) | 3,000 |
| Synchronization | 500 | +500 (rwlock + qspinlock) | +2,000 (RCU tree) | 3,000 |
| Filesystem | 2,000 | +2,000 (journal + CRC) | +5,000 (CoW-lite + snapshots) | 9,000 |
| VFS + syscalls | 2,000 | +1,000 (more syscalls + epoll-like) | +500 (io_uring-style 5 opcodes) | 3,500 |
| Security | 500 | +500 (seccomp-lite + CFI) | +1,000 (KPTI if hostile userspace) | 2,000 |
| Capability model | 0 | +2,500 (Zircon-lite handles + endpoints) | +2,000 (CNode-lite) | 4,500 |
| Networking | 0 | +3,000 (virtio-net + lwIP port or picoTCP) | +2,000 (sockets full POSIX) | 5,000 |
| 9P protocol | 1,000 | +500 (server + client) | — | 1,500 |
| Userspace bootstrap | 1,500 | +1,500 (more coreutils + init) | — | 3,000 |
| Testing + klog | 1,500 | +500 (SMP torture + fuzzer) | — | 2,000 |
| Editor/tools (outside kernel) | 0 | — | +optional — | — |
| **Total kernel** | **~17k** | **+17k (+100%)** | **+25k (+73%)** | **~59k LOC** |

**Reality check solo-dev**: escribir 17k LOC new + fix 60 bugs R2 = 6-9 meses full-time. v2 (+17k) = 1 año full-time. v3 (+25k) = 1.5-2 años. Total kernel v1+v2+v3 = **3-4 años solo-dev full-time**.

Redox comparison: 11 años, ~20k kernel + ~100k total, still alpha. ALZE on 20k kernel trajectory to "beta" = 10-15 años realista.

---

## Top 40 ideas destiladas (R1+R2+R3+R4+R5)

Agrupadas por leverage inmediato.

### Inmediato v1 (décidir + implementar en semanas)
1. **ADD LICENSE** (ISC 8 líneas) — R4/bsd_family
2. **Fix R2 12 P0 blockers** (IDT, SMP locks, FS locks)
3. **SMAP + SMEP CR4 bits** (~50 LOC) — R5/security_hardening
4. **KASLR via limine offset** (~200 LOC) — R5/security_hardening
5. **Flat 1GB huge-page kernel ID map policy** — R5/vm_tlb_paging
6. **NO fork / NO swap / NO THP policy doc** — R5/vm_tlb_paging
7. **virtio-only driver policy v1** — R5/drivers_modern
8. **MWAIT idle + C-states detection** (~100 LOC) — R3/schedulers + R5/power
9. **Limine rev 4 upgrade verify** — R5/boot_init
10. **Per-task SSP canary random** (fix R2 global canary weakness)

### v1 extensions
11. **pledge-style syscall allow-mask** (~50-200 LOC) — R4/bsd_family
12. **ext2-lite fixed + 7 R2 bugs** — R5/vm + R3/modern_filesystems
13. **9P protocol server native** (~1k LOC) — R4/plan9_inferno ← **REAL diferenciador**
14. **virtio-blk + virtio-net + virtio-console** (~1.3k LOC) — R5/drivers_modern
15. **SMP torture tests** — R2 finding propagated

### v2 — capability migration + modern patterns
16. **Zircon handle table + rights bitmap** (~2-3k LOC) — R3/capability_kernels + R4/fuchsia_zircon
17. **Per-CPU runqueues + load balancer** (~1k LOC) — R3/schedulers
18. **rwlock + qspinlock** (~500 LOC) — R3/rcu_synchronization
19. **IOCP-style async I/O 5 opcodes** (~2k LOC) — R3/async_io_models
20. **uACPI library integration** (~500 LOC glue) — R5/boot_init
21. **Journal + metadata CRC en FS** (~2k LOC) — R3/modern_filesystems
22. **Endpoint IPC fast path (Zircon/seL4 pattern)** (~500 LOC) — R4/sel4_verified
23. **virtio-ring como unified I/O abstraction** — R5/drivers + R3/virtualization
24. **Threaded interrupts** (~500 LOC) — R5/rtos_realtime
25. **seccomp-lite syscall filter BPF-like** (~500 LOC) — R5/security_hardening

### v3 — aspiracional
26. **WAFL-inspired CoW-lite FS + snapshots** (~5-8k LOC) — R3/modern_filesystems
27. **BFS attribute queries** (opt) — R4/haiku_beos
28. **kqueue-style event notification** (~2k LOC) — R4/bsd_family
29. **Rump kernels pattern** (test infra) — R4/bsd_family
30. **KPTI + CFI if hostile userspace** — R5/security_hardening
31. **PAC/BTI on Arm port** — R5/security_hardening
32. **Rust bilingual per NEW driver opcional** — R3/rust_in_kernel
33. **Landlock-style sandboxing** — R5/containers_primitives
34. **Virtualization VMX host** (~10-15k LOC) — R3/virtualization
35. **Memory Tagging Extension (MTE) Arm** — R5/vm_tlb

### Non-code (crítico)
36. **YouTube vlogs trimestrales** (SerenityOS model) — R4/hobbyist_oses
37. **Scope decision 1-página doc** — firma NO fork, NO swap, virtio-only
38. **Engine evolution strategy doc** — FromSoftware model (iterative, no rewrite) — applies to kernels too
39. **Community engagement plan** — Matrix/Discord + Mastodon + GitHub issues
40. **Quarterly demo video target** — ship running ALZE in QEMU with a narrative

---

## Anti-patterns consolidados (43 total)

**R1 (1-12, no repetir)**: ACL-only, monolithic-only, big-bang-rewrites, etc.

**R3 (13-22)**:
13. io_uring completo (60+ opcodes + 20 CVEs)
14. RCU antes de fix lock-ordering
15. seL4 verification solo-dev
16. ZFS/Btrfs clone
17. Rust rewrite mid-project
18. CoW fork() en hobby kernel
19. Swap/THP 2026 desktop
20. sched_ext sin eBPF VM
21. Demand paging + fork antes de fixes core
22. NOT virtio (real HW prestige trap)

**R4 (23-32)**:
23. OS sin LICENSE
24. Formal verification solo-dev
25. Rust rewrite kernel existente
26. Container namespaces en cap kernel (redundante)
27. Plan 9 namespaces sin 9P underneath
28. TempleOS-style technical purity sin community
29. Minix 3-style reliability pitch sin users
30. GCC 2.95 legacy binary compat trap
31. Annual cadence hobby OS (Redox 11 años alpha realidad)
32. Solo-dev ambitious scope "everything Linux"

**R5 (33-43)**:
33. Real HW driver ambition v1
34. Power management hobby kernel (10k+ LOC no payoff)
35. KPTI sin threat model
36. CFI/CET/PAC sin HW target
37. PREEMPT_RT en general-purpose kernel
38. Namespaces + cgroups sin caps (bandaid)
39. Demand paging antes de fix core R2 bugs
40. Swap/zswap/zram desktop 2026
41. Fine-grained KASLR solo-dev
42. ACPICA full scratch (use uACPI)
43. xHCI complete + USB-C + TB4

---

## OSes por lección dominante

| OS | Lección única para ALZE |
|---|---|
| **Linux mainstream** | Feature-complete era terminó 1999. No replicar todo. |
| **seL4** | Verification = institutional. Patterns copyable sin proof. |
| **Zircon/Fuchsia** | Capabilities + VMOs son superior but OS sin apps = sin market. |
| **Redox** | 11 años alpha con 1 FTE. Hobby timeline 15-20 años mínimo. |
| **Haiku** | 24 años volunteer + narrow focus = shippable. Process > code. |
| **FreeBSD** | Unsung production winner. License BSD + correctness. |
| **OpenBSD** | pledge/unveil minimal security. Kernel mitigations inventadas años antes que Linux. |
| **Plan 9** | 9P como primary IPC = ALZE realistic differentiator. ~1k LOC. |
| **SerenityOS** | Process > code. YouTube = multiplier. |
| **TempleOS** | Cautionary: genius sin community = obscure death. |
| **Minix 3** | Reliability pitch sin users = muerto 2018. |
| **ToaruOS** | Solo-dev 15 años longevity posible (con disciplina). |
| **Windows NT** | Capability-like object manager + WDF = good driver abstraction. |
| **macOS/XNU** | Mach hybrid + BSD userland. Lesson: híbridos trade-offs. |
| **NetBSD** | Portability + rump kernel pattern para testing. |

---

## Qué hacer HOY mismo (actionable)

Si el objetivo es avanzar ALZE OS hoy, los primeros commits concretos:

1. **Add LICENSE** file (ISC, 8 líneas) — 5 minutos. Fix legal bloqueo.
2. **Write scope decision doc** (1 página): A/B/C decision + NO fork + NO swap + virtio-only.
3. **Fix R2 P0 blockers Sprint 1** (3 items, ~2 semanas solo-dev): IDT incompleta, SMP lock order, FS locks.
4. **Add SMAP + SMEP** (~50 LOC, ~1 día).
5. **Add KASLR via Limine offset** (~200 LOC, ~2 días).
6. **Document virtio-only policy** en ERRORES.md / DECISIONS.md.

Esa lista es ~3-4 semanas de trabajo concentrado. Al final, ALZE está "v1-ready" — todos los bugs P0 cerrados + security baseline + decisiones arquitectónicas commiteadas.

---

## Qué NO hacer

1. **NO ship más research** — 12,870 líneas es suficiente. Pasar a código.
2. **NO fork() / swap / THP / demand paging** — firmar decision, stick with it.
3. **NO real HW drivers v1** — virtio-only.
4. **NO capabilities v1** — fix monolithic first, capabilities es v2 migration deliberada.
5. **NO Rust rewrite** — C99 stay. Bilingual experiment v2 opcional.
6. **NO feature parity con Linux** — narrow scope.
7. **NO PREEMPT_RT patterns v1** — separate genre.
8. **NO io_uring full** — IOCP-style simpler.
9. **NO power management** — halt cleanly only.
10. **NO engine rewrite mid-project** — FromSoftware evolution model.

---

## Cierre — la lección maestra

La research está completa. **32 agentes + 12,870 líneas**. Ninguna research adicional agregará valor marginal significativo.

**La lección meta a través de 21 OSes estudiados**: la diferencia entre OSes que shippean (FreeBSD, Haiku, SerenityOS) y OSes que mueren (Minix 3, TempleOS, Luminous-equivalent) no es la tecnología — es la **disciplina sobre scope + cadence + community engagement**.

ALZE tiene más probabilidad de shippear si adopta:
- FromSoftware-style iterative evolution (alze_engine research lesson aplicable aquí)
- SerenityOS-style community engagement (YouTube + Matrix + quarterly demos)
- BSD-style license + governance (no single-vendor lock)
- Plan 9-style architectural differentiator (9P native = unique angle)

...que si adopta la última técnica kernel buzzword (sched_ext, io_uring, Rust, capabilities verified).

**Stack decisions son trivialmente reversibles. Scope + community discipline no.**

Próximo paso natural: **ejecutar las 6 acciones "HOY mismo"** arriba. Si ALZE está "v1-ready" en 4 semanas, seguir con v1 remaining ~2k LOC + userspace + 9P integration. Si no, pivotear la pregunta: ¿es ALZE un learning project o un ship-goal project?
