# alze_os — Plan de profundización rounds 3/4/5

**Fecha:** 2026-04-22
**Motivación:** mismo patrón que alze_engine. R1+R2 cubrieron OS families y code review de /root/repos/alze-os; ahora profundizar.

**Estado previo:**
- R1 (5 agentes, 939 L) — linux_mainstream, linux_especializado, macos, windows, otros (seL4/Redox/Fuchsia/Haiku/ReactOS/BSDs). `_sintesis.md`.
- R2 (6 agentes, 1,438 L) — code review de /root/repos/alze-os por subsistema. `review/*.md` + `review/_sintesis.md`.

**Top 3 ideas R1 sintetizadas (para referencia):**
1. Capability-based kernel con handles tipados + IDL stubs generados (seL4 + Zircon + FIDL)
2. Ring buffer I/O + scheduler pluggable (io_uring + IOCP + sched_ext)
3. Sistema declarativo atómico con CoW + E2E checksums (NixOS + APFS + ZFS)

**Review P0 blockers (12 items Sprint 1-3):** IDT incompleta, asunciones SMP, FS sin locks.

## Round 3 — Subsistemas kernel modernos deep (7 agentes)
Target: `/root/lab_journal/research/alze_os/r3/`

1. `capability_kernels.md` — seL4 (CAmkES + IDL + proof system) + Zircon/Fuchsia + KeyKOS/EROS heritage + Mach ports
2. `async_io_models.md` — io_uring internals + Windows IOCP + Linux AIO legacy + epoll/kqueue/libxev modern
3. `schedulers_modern.md` — EEVDF (Linux 6.6+) + sched_ext (eBPF) + FreeBSD ULE + Genode + pluggable policy
4. `rcu_synchronization.md` — McKenney RCU + hazard pointers + epoch-based reclamation + lock-free primitives kernel
5. `modern_filesystems.md` — ZFS + APFS + Btrfs + bcachefs — CoW + transactions + checksums + snapshots + encryption
6. `virtualization_kvm.md` — KVM internals + Hyper-V + EPT/NPT + SEV/TDX confidential compute + microVM (Firecracker)
7. `rust_in_kernel.md` — Linux Rust 2024-2026 + Redox + Asterinas + Verus verified + RustyHermit

## Round 4 — OSes que saltamos o solo rozamos (7 agentes)
Target: `/root/lab_journal/research/alze_os/r4/`

1. `fuchsia_zircon.md` — Google microkernel, 2026 status, Nest/IoT deployment
2. `sel4_verified.md` — formally verified microkernel deep, CAmkES, camkes-vm, seL4 Foundation
3. `redox_os.md` — Rust microkernel, 2026 status, relationship to other Rust OSes
4. `haiku_beos.md` — BeOS descendant, messaging IPC, ported apps 2026
5. `bsd_family.md` — FreeBSD/NetBSD/OpenBSD differences, kqueue, pledge, capsicum, jails, rump kernels
6. `plan9_inferno.md` — Bell Labs research, 9front active, Go language inheritance
7. `hobbyist_oses.md` — SerenityOS, ToaruOS, HelenOS, TempleOS lessons — what solo-dev OSes achieve

## Round 5 — Cross-cutting kernel systems (7 agentes)
Target: `/root/lab_journal/research/alze_os/r5/`

1. `boot_init.md` — limine (ALZE usa) + GRUB + systemd-boot + UEFI + Stivale2 + device trees
2. `drivers_modern.md` — USB xHCI + NVMe + net stack + Linux kobj/dts + Windows WDF/KMDF
3. `vm_tlb_paging.md` — demand paging + huge pages + transparent hugepage + memcg + swappiness
4. `power_management.md` — ACPI + cpufreq + cpuidle + runtime PM + schedutil + modern-standby
5. `security_hardening.md` — SMAP/SMEP + KPTI + KASLR + seccomp + eBPF LSM + CFI + IBT
6. `rtos_realtime.md` — Zephyr + FreeRTOS + QNX + VxWorks + RTEMS + PREEMPT_RT Linux
7. `containers_primitives.md` — cgroups v2 + namespaces + SELinux/AppArmor + overlayfs + OCI runtime

## Política
- 300-500 L por archivo, primary refs con autor año venue URL.
- Foco algoritmos + data structures + números concretos.
- ALZE applicability: copiable en C99+asm hoy, o v2 después de capability model, o v3 aspiracional.
- Un `_sintesis_rN.md` por round al cierre + master final.

**Total estimado**: 21 agentes + 3 síntesis + 1 master = ~8,000-10,000 líneas de research nueva, ~12,000 L total alze_os.
