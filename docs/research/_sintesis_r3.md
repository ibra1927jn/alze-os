# Síntesis round 3 alze_os — Subsistemas kernel modernos deep

**Fecha:** 2026-04-22
**Input:** 7 agentes paralelos, ~3,374 líneas. 0 fallos bloqueantes.

Complementa `_sintesis.md` (R1, paisaje OS families) y `review/_sintesis.md` (R2, code review de /root/repos/alze-os). **R3 es el primer deep-dive algorítmico** en subsistemas concretos: capabilities, async I/O, schedulers, sync, FS, virtualization, Rust-in-kernel.

- [`r3/capability_kernels.md`](r3/capability_kernels.md) — seL4 + Zircon + KeyKOS/EROS + Barrelfish + Mach (509 L)
- [`r3/async_io_models.md`](r3/async_io_models.md) — io_uring + IOCP + epoll/kqueue + zero-copy (568 L)
- [`r3/schedulers_modern.md`](r3/schedulers_modern.md) — CFS→EEVDF + sched_ext + ULE + EDF + work-stealing (479 L)
- [`r3/rcu_synchronization.md`](r3/rcu_synchronization.md) — RCU + hazard pointers + EBR + lock-free + LKMM (581 L)
- [`r3/modern_filesystems.md`](r3/modern_filesystems.md) — ZFS + APFS + Btrfs + bcachefs + WAFL + LFS (423 L)
- [`r3/virtualization_kvm.md`](r3/virtualization_kvm.md) — KVM + Firecracker + Xen + SEV-SNP/TDX + virtio (410 L)
- [`r3/rust_in_kernel.md`](r3/rust_in_kernel.md) — R4L + Redox + Asterinas + Hubris + verification state 2026 (404 L)

---

## Tabla cross-R3 (subsistema × state-of-art × ALZE feasibility)

| Subsistema | Mainstream 2026 | Idea copiable | LOC estimate solo-dev | ALZE v1 | ALZE v2 | ALZE v3 |
|---|---|---|---|---|---|---|
| **Capabilities** | seL4 (verified) + Zircon (prod) | Handle table + rights bitmap + endpoints | ~2-3k LOC | ✗ | Zircon-lite en C99 | seL4 patterns (CNode tree) aspiracional |
| **Async I/O** | io_uring (Linux) + IOCP (Windows) | IOCP-style (simpler, 30 años prod, menos CVE) | ~2-3k LOC 5 opcodes | blocking syscalls | IOCP-style 5 opcodes | ≤20 opcodes completo |
| **Scheduler** | EEVDF (Linux 6.6+) + sched_ext (6.12) | Per-CPU runqueues + priority + round-robin | ~1.5-2.5k LOC | O(1) bitmap actual | CFS-lite o EEVDF-lite | sched_ext requiere eBPF VM 20k LOC |
| **Sync** | RCU (Linux), hazptr (folly) | Spinlock + mutex + seqlock antes de RCU | ~500 LOC primitivos | spinlock + mutex | + rwlock + seqlock + qspinlock | RCU tree ~2k LOC |
| **Filesystems** | ZFS/APFS/Btrfs/bcachefs (CoW) | WAFL/LFS patterns (~5-10k LOC simplified) | ~5-10k LOC CoW-lite | ext2-lite estable | + journal + metadata CRC | CoW + snapshots |
| **Virtualization** | KVM+QEMU / Firecracker / SEV-SNP | virtio ring structure (same as io_uring) | 0 directo — virtio-lite patterns | ninguna | virtio-like device API | VMX host ~10-15k LOC |
| **Rust-in-kernel** | Linux R4L (experimental) + Redox alpha | Bilingual bridge (C + Rust per driver) | ~2k LOC FFI | C99 only | Rust para driver nuevo opcional | Full Rust rewrite = 18-24 meses |

---

## Top 14 ideas concretas R3 para ALZE

Ordenadas por **leverage para hobby kernel solo-dev**:

### 1. IOCP-style async I/O (NOT io_uring) — v2
Simpler, 30 años producción, ~1k LOC vs io_uring 3k+. 20+ CVEs en io_uring, casi 0 en IOCP. Pattern común: ring buffer con SQE/CQE completion. r3/async_io_models.md §4+§10.

### 2. Zircon handle table + rights bitmap — v2
~2k LOC en C99. Typed handles + revocable. Pattern capability sin formal verification. Evita confused deputy problem inherente en Unix ACL. r3/capability_kernels.md §5.

### 3. Spinlock + mutex + seqlock primero, RCU después — v1+v2
R2 review identificó "SMP assumptions" como P0. Arreglar lock-ordering bugs + per-CPU runqueues ANTES de RCU. RCU es ~2k LOC grace period machinery — solo si profiling muestra necesidad. r3/rcu_synchronization.md §§7, 11.

### 4. WAFL paper (Hitz 1994) como referencia simple CoW FS — v3
100-500k LOC de ZFS es imposible copiar; WAFL paper es ~20 páginas, describe CoW + snapshots + consistency en forma canónica simple. ~5-10k LOC implementable solo-dev. r3/modern_filesystems.md §9.

### 5. C99 stay — Rust experimento por driver nuevo opcional — v2
R4L drama 2024 muestra que Rust-in-Linux es herido. Rewriting ALZE en Rust = 18-24 meses. Path pragmatico: mantener C99, escribir *nuevo* driver en Rust con FFI boundary. Si no gusta el experimento, eliminar sin pérdida. r3/rust_in_kernel.md §9.

### 6. Virtio ring pattern — diseño I/O abstraction v2
Virtio = io_uring = IOCP = xHCI TRB = NVMe SQ/CQ. Todos siguen ring buffer con producer/consumer. Diseñar **UN pattern** en ALZE v2 y usar para todo: disk, net, audio, console. r3/virtualization_kvm.md §9.

### 7. Per-CPU runqueues antes de CFS/EEVDF — v2
Más bang-per-buck que cambiar algorithm. SMP hoy con un runqueue global = TODO thread gana lock del mismo. Per-CPU + load balancer simple. ~1k LOC. r3/schedulers_modern.md v2 Option C.

### 8. Flat 1 GB huge-page ID map para kernel — v1
Elimina VMM complexity. Kernel map nunca cambia post-boot. 2 MB huge pages para user. Skippe demand paging + COW + swap. Saves ~3k LOC. r3 + r5/vm_tlb_paging §3.

### 9. NO CoW fork() nunca — v1 decisión arquitectónica
Process creation via explicit process_create (Zircon model), no fork. Saves demand paging complexity. ALZE no necesita fork semantics (no shell scripts Unix compat objetivo). r3/capability_kernels.md + r5/vm_tlb_paging §3.

### 10. NO swap/THP — v1 decisión
32+ GB RAM desktop 2026 → swap muerto excepto containerized workloads. Hobby kernel no tiene swap. Saves ~2-3k LOC + páginador thread. r5/vm_tlb_paging §6.

### 11. Determinismo opcional: fixed tick + input-as-commands — v1 arquitectura
Aunque no sea RT kernel, diseñar con fixed timestep + serializable state enables future rollback/deterministic replay. r5/rtos_realtime §11.

### 12. Adopt SSP + SMAP + SMEP + KASLR en v1 — security básica
Current ALZE tiene SSP. Agregar SMAP+SMEP (CR4 bits, ~50 LOC) + KASLR (via Limine, ~200 LOC) = baseline security sin perf tax significante. r5/security_hardening.md §§1-3.

### 13. uACPI library en lugar de ACPICA — v2
ACPICA es 100k+ LOC Intel legacy. uACPI (recent community rewrite, ~20k LOC) is tailored for hobby kernels. Parser para MADT/MCFG/DSDT. r5/boot_init §9.

### 14. Container primitives son bandaid en Linux ACL — v3 think
Si ALZE tiene capabilities (v2+), "container" = capability scope subset. No necesitas 8 namespaces + cgroups como Linux. Natural en cap-based kernel. r5/containers_primitives.md §15.

---

## Anti-patterns R3 (nuevos)

Continúan numeración (R1 anti-patterns = 1-12):

13. **Implementar io_uring completo** — 60+ opcodes + 20+ CVEs. IOCP-style 5 opcodes suficiente.
14. **RCU antes de arreglar lock-ordering** — RCU no arregla bugs básicos. Fix SMP lock order primero.
15. **seL4-style formal verification solo-dev** — 2-3 eng-years / kLOC. Institucional only.
16. **ZFS/Btrfs clone** — 100-500k LOC. WAFL/LFS paper simpler reference.
17. **Rust rewrite mid-project** — 18-24 meses perdidos. Bilingual C+Rust per driver mejor.
18. **CoW fork() en hobby kernel** — demand paging + COW + swap = 3k+ LOC complexity. process_create elimina.
19. **Swap/THP en 2026** — 32 GB RAM desktop no necesita. Saves 2k LOC.
20. **sched_ext en hobby kernel** — requiere eBPF VM ~20k LOC. Per-CPU runqueues mejor ROI.
21. **Demand paging + fork COW — hobby kernel trampa**: elimina la complexity en v1 via explicit creation model.
22. **virtio-NOT — "real hardware prestige"**: xHCI + NVMe + Wi-Fi 7 specs = 1000+ páginas cada uno. Virtio-only v1 = ship en QEMU, real HW = v3.

---

## Update al stack ALZE-OS (delta R1+R2)

### v1 (C99+asm actual) — enfocar en arreglar P0 R2 review + adicionales R3

**Do ahora** (semanas 1-8):
- Fix 12 P0 R2 review (IDT incompleta, SMP lock order, FS sin locks)
- **Add LICENSE** (ISC, 8 líneas) — R4/bsd_family descubrió que ALZE no tiene
- **Add SMAP + SMEP** (~50 LOC, CR4 bits)
- **Add KASLR** via limine feature (~200 LOC kernel-side)
- **Stabilize flat 1GB ID map** — remove any demand paging attempts

**Decidir NOW** (escribir doc 1 página):
- Scope: hobby kernel study project vs ambition de ship apps
- NO fork(), NO swap, NO THP (decisión firmada)
- C99 canonical, Rust opcional post-v1

### v2 (2026-2027) — capability model

- **Zircon handle table + rights bitmap + endpoints** (~2-3k LOC)
- **Per-CPU runqueues + load balancer simple** (~1k LOC)
- **virtio-ring I/O pattern** unificado (disk, net, audio, console) — replace xHCI ambition
- **WAFL-inspired CoW FS-lite** (~5k LOC) sobre block device
- **IOCP-style async I/O** 5 opcodes (~2k LOC)
- **uACPI library integration** (~20k LOC dep, ~500 LOC glue)
- **pledge/unveil-style sandbox** on syscall path (~200 LOC)

### v3 (aspiracional, 2028+)
- 9P protocol nativo (~1k LOC) — R4 found genuinely distinct differentiator
- CoW FS full (snapshots, checksums Merkle)
- Rust experiment por driver nuevo
- VMX host / KVM-lite (~10-15k LOC)

---

## Ranking de densidad R3 (leverage solo-dev)

1. **capability_kernels.md** — Zircon lite es v2 concreto. Pattern cambia 10 años de arquitectura.
2. **rcu_synchronization.md** — directamente conecta con P0 R2 review. Lock ordering primero.
3. **async_io_models.md** — IOCP simpler que io_uring es contra-intuitive pero correcto.
4. **modern_filesystems.md** — WAFL paper como lectura obligatoria. ZFS es aspiracional only.
5. **schedulers_modern.md** — per-CPU runqueues ROI alto. sched_ext aspiracional.
6. **virtualization_kvm.md** — virtio ring pattern más que VMX host.
7. **rust_in_kernel.md** — confirma que bilingual C+Rust es el camino, no rewrite.

---

## Conclusión R3

R1 mapeó OS families. R2 encontró 60 bugs concretos. R3 **conecta teoría moderna con bugs específicos**: RCU/sync → R2 P0 SMP; VM/paging → R2 memory.md; capabilities → v2 goal arquitectónico.

Los top 3 ideas de R1 (caps + ring I/O + CoW FS declarativo) se concretizan en R3 con LOC estimates + scheduler de adopción. **R4 siguiente** cubrirá OSes missed (Fuchsia/Zircon, seL4, Redox, Haiku, BSDs, Plan 9, hobbyist). **R5** cubrirá cross-cutting (boot, drivers, VM, power, security, RTOS, containers).
