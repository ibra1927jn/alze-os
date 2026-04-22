# Síntesis round 5 alze_os — Cross-cutting kernel systems

**Fecha:** 2026-04-22
**Input:** 7 agentes paralelos, ~3,612 líneas. 0 fallos bloqueantes.

Complementa R1-R4. **R5 cubre sistemas cross-cutting** — los 7 subsistemas que juntos completan la superficie operacional de un kernel real: boot/init, drivers, VM/paging, power, security, RTOS, containers.

- [`r5/boot_init.md`](r5/boot_init.md) — limine protocol deep + GRUB + UEFI + SMP AP + ACPI (511 L)
- [`r5/drivers_modern.md`](r5/drivers_modern.md) — USB xHCI + NVMe + PCIe + Linux kobj + virtio (488 L)
- [`r5/vm_tlb_paging.md`](r5/vm_tlb_paging.md) — x86_64 paging + TLB + demand paging + huge pages + LAM/MTE (595 L)
- [`r5/power_management.md`](r5/power_management.md) — ACPI + cpufreq + cpuidle + thermal + RAPL (570 L)
- [`r5/security_hardening.md`](r5/security_hardening.md) — SMAP/SMEP + KPTI + KASLR + CFI + CET + PAC/BTI + Spectre (482 L)
- [`r5/rtos_realtime.md`](r5/rtos_realtime.md) — Zephyr + FreeRTOS + QNX + VxWorks + PREEMPT_RT (522 L)
- [`r5/containers_primitives.md`](r5/containers_primitives.md) — namespaces + cgroups + OCI + Kata + gVisor + jails (444 L)

---

## Decision-stack R5 para ALZE v1 (hobby x86_64 solo-dev)

Cada subsistema con prescripción concreta + LOC estimate + descartes explícitos:

| Subsistema | v1 recomendado | LOC | v1 descartado | Razón |
|---|---|---|---|---|
| **Boot** | **limine stay** (current choice correct) + verify protocol rev 4 + unused features | 0 extra | GRUB2, rEFInd, custom | Limine is right; don't change bootloaders |
| **Drivers** | **virtio-only policy**: virtio-blk + virtio-net + virtio-console | ~500 LOC × 3 = 1.5k | Real HW NVMe/xHCI/Wi-Fi | Real HW specs = 1000+ páginas cada una; virtio ships en QEMU |
| **VM/paging** | **Flat 1 GB huge-page ID map kernel + 2 MB user** + fix R2 memory bugs | ~800 LOC (net, despues fixes) | Demand paging, fork COW, swap, THP | Saves ~3k LOC; hobby kernel no necesita |
| **Power** | **Nada**. halt cleanly via HLT. No suspend/resume. | 0 extra | cpufreq, cpuidle, S3 suspend, thermal | PM = 350k LOC Linux. 10k+ LOC hobby minimum. Skip. |
| **Security** | **SMAP + SMEP + SSP + KASLR** v1 baseline | ~500 LOC | KPTI, CFI, CET, PAC, Spectre mitigations | Hobby kernel no enfrenta usuarios hostiles. Skip tax. |
| **RTOS features** | **Nada**. General-purpose kernel. | 0 extra | PREEMPT_RT patterns | RTOS es genero separado. No pivotear. |
| **Containers** | **Nada**. Single-user single-process casi. | 0 extra | Namespaces, cgroups, overlayfs | No tiene multi-process yet. v3 aspiracional via capabilities. |

**TOTAL R5 v1 LOC añadido ALZE**: ~2.8k LOC across 7 subsistemas. Mayoría es descartar, no agregar. Política: **"lo mínimo que hace el kernel útil"**.

---

## Top 12 ideas R5 para ALZE

### 1. **FIX R2 P0 blockers antes de todo** — v1 semana 1
R2 review identificó IDT incompleta, SMP lock order, FS sin locks. R5/vm_tlb_paging mapea 14 memory issues específicas a fixes. R5/drivers_modern mapea 5 driver issues. **Pre-condición para todo lo demás**.

### 2. **virtio-only policy v1** — driver decisión arquitectónica
1 archivo decisión: "ALZE v1 solo soporta HW virtio." Real NVMe/xHCI/Wi-Fi = v3. Ship en QEMU primero. Saves meses de battle con 1000-page specs. r5/drivers_modern §13.

### 3. **Flat 1 GB huge-page kernel ID map** — v1 VM simplification
Elimina demand paging completamente del kernel. Page table setup once en boot. 4 KB para user pages only. ~800 LOC neto. r5/vm_tlb_paging §3.

### 4. **NO fork(), NO swap, NO THP** — v1 decisiones firmes
Firma 1-página doc: "ALZE no implementará fork(). process_create explícito. No swap (32 GB RAM baseline). No THP (complejidad sin valor demostrado)." Saves ~3-5k LOC. r5/vm_tlb_paging §§3, 6.

### 5. **SMAP + SMEP + SSP + KASLR baseline** — v1 security
~500 LOC total. SMAP/SMEP = CR4 bits ~50 LOC. SSP ya tiene (R2). KASLR via Limine offset (~200 LOC). Skip KPTI, CFI, CET, PAC, retpoline. r5/security_hardening §§1-3.

### 6. **Pledge-style syscall sandbox** — v1 simple
~50-200 LOC. Proceso declara "solo hago net + stdio" → kernel enforce. OpenBSD invention. r4/bsd_family + r5/security_hardening.

### 7. **uACPI library integration** — v2
ACPICA (Intel) es 100k+ LOC legacy. uACPI (community rewrite) ~20k LOC hobby-friendly. Integration ~500 LOC glue. r5/boot_init §9.

### 8. **Per-CPU runqueues** — v2 scheduler
Antes de CFS/EEVDF algorithm change. Single global runqueue = todos threads contend por mismo lock. Per-CPU + load balancer simple = ~1k LOC. r5 (implicito) + r3/schedulers.

### 9. **Threaded interrupts** — v2 low-latency
PREEMPT_RT pattern. IRQ handler corre en thread priority. Puede preempt interrupt de baja prioridad. No hard-RT pero "low-latency" válido para ALZE. ~500 LOC. r5/rtos_realtime §6.

### 10. **virtio ring como I/O abstraction unificada** — v2
Mismo pattern que io_uring, IOCP, xHCI TRB, NVMe SQ/CQ. ALZE v2 adopta **un ring buffer pattern** reused across disk/net/audio/console. Saves divergence complexity. r5/drivers_modern + r3/async_io.

### 11. **MWAIT idle + C-states optional** — v2
Current ALZE uses `sti;hlt` directo (R2 finding). MWAIT enables C1E, C3, deeper states = less power. ~100 LOC + CPUID detection. r5/power_management + r3/schedulers review.

### 12. **Containers = capabilities (not namespaces)** — v3 think
Si ALZE adopta capabilities en v2, "container" es capability scope subset. No necesita 8 namespaces + cgroups bandaid. r5/containers_primitives §15.

---

## Anti-patterns R5

Continúan numeración (R1 = 1-12, R3 = 13-22, R4 = 23-32):

33. **Real HW driver ambition v1** — NVMe/xHCI/Wi-Fi/BT = 1000+ páginas spec cada uno. virtio-only v1.
34. **Power management en hobby kernel** — 10k+ LOC minimum, no payoff sin usuarios. Skip.
35. **KPTI sin threat model** — 5-30% syscall overhead. Hobby OS no enfrenta hostile userspace.
36. **CFI/CET/PAC sin HW target** — requires specific CPU + compiler. Defer until real HW test.
37. **PREEMPT_RT-style en general-purpose kernel** — separate genre. Don't pivot.
38. **Namespaces + cgroups sin capability model** — bandaid over Linux ACL, complexity no vale.
39. **Demand paging antes de core bugs** — R2 found 14 VM issues. Fix those primero.
40. **Swap/zswap/zram desktop 2026** — 32 GB RAM = swap muerto. Container/embedded only.
41. **Fine-grained KASLR solo-dev** — ROI cuestionable, basic KASLR suficiente.
42. **Real ACPI implementation scratch** — ACPICA/uACPI existen, no reinventar.
43. **xHCI complete implementation** — USB 3.2 spec es 100+ páginas, USB-C + TB4 añade 200+. virtio-console v1.

---

## Cross-system integration rules R5

### Boot → early kernel handoff
- Limine provides: long mode, paging 4-level, GDT basic, framebuffer, memmap, HHDM, ACPI tables ptr
- Kernel must still: populate IDT (R2 blocker), initialize LAPIC, parse ACPI MADT, start APs
- Path: limine → `_start` → `arch_init` → `mm_init` → `irq_init` → `scheduler_init` → `kmain`

### Memory + drivers
- Virtio drivers DMA via Contiguous VMO (Zircon term) or raw phys_alloc (ALZE current)
- IOMMU integration v3 aspiracional (virtio doesn't require IOMMU if trusted host)
- PCIe MSI-X vectors allocated from IRQ domain (v2)

### Security + paging
- SMAP requires fault path to handle STAC/CLAC around copy_from_user / copy_to_user
- SMEP = no fix required post-enable, CPU enforces
- KASLR requires limine kernel_address randomization flag + relocations + symbol-table fixup in kernel

### Scheduler + drivers (IRQ threading)
- Threaded IRQs require scheduler with real-time priorities (v2+)
- irq_handler runs in schedulable context, can block, sleeping in fast path forbidden

### Boot + power (no power v1)
- Limine doesn't do ACPI sleep states
- Kernel `halt` = cli + hlt forever. `reboot` = ACPI reset or keyboard controller 0xFE

---

## Ranking de densidad R5

1. **vm_tlb_paging.md** — mapea R2 14 memory issues directamente a fixes concretos. Inmediato.
2. **drivers_modern.md** — R2 5 driver issues + virtio-only policy decision. Inmediato.
3. **boot_init.md** — limine protocol deep + integration hooks. Referencia continua.
4. **security_hardening.md** — ~500 LOC v1 baseline SMAP+SMEP+KASLR. Pragmático.
5. **containers_primitives.md** — para entender lo que ALZE NO implementa y por qué. Útil conceptualmente.
6. **rtos_realtime.md** — aspiracional only. Pero el threaded-IRQ pattern es útil.
7. **power_management.md** — mayormente "skip." Pero RAPL idea interesante v3.

---

## Conclusión R5

R5 demuestra que **el 80% del trabajo de un hobby kernel es DECIDIR QUÉ NO HACER**. Solo 2.8k LOC netos v1 ALZE, mientras descartamos ~20-30k LOC de features Linux-equivalent.

**Lessons cross-cutting**:
- Boot: don't change bootloader. Limine correct.
- Drivers: virtio-only. Real HW is v3 battle.
- VM: flat ID map + no demand paging + no fork.
- Power: none. Kernel always-on.
- Security: 4 cheap features (SMAP+SMEP+SSP+KASLR) + skip expensive ones.
- RTOS: no pivoteo. Separate genre.
- Containers: capabilities > namespaces (v2+).

**R5 prepara al master synthesis** a consolidar todo: R1 paisaje + R2 bugs + R3 subsistemas + R4 OSes + R5 cross-cutting = stack ALZE-OS definitivo con LOC budget + roadmap v1/v2/v3.
