# Síntesis — investigación OS para ALZE OS

**Fecha:** 2026-04-21
**Input:** 5 agentes paralelos, 827 líneas totales, 0 errores.

- [`linux_mainstream.md`](linux_mainstream.md) — Ubuntu/Debian/Fedora/Arch (107 L)
- [`linux_especializado.md`](linux_especializado.md) — Kali/Parrot/Qubes/NixOS/Alpine (172 L)
- [`macos.md`](macos.md) — Darwin/XNU (154 L)
- [`windows.md`](windows.md) — NT kernel family (242 L)
- [`otros.md`](otros.md) — seL4/Redox/Fuchsia/Haiku/ReactOS/BSDs (152 L)

---

## Tabla comparativa

| Dimensión | Linux mainstream | Linux especializado | macOS/XNU | Windows NT | Otros (micro + BSD) |
|---|---|---|---|---|---|
| **Kernel** | Monolítico modular | Idem (Qubes = Xen hypervisor + HVM) | Híbrido: Mach μK + BSD personality + IOKit | Híbrido micro-ish: Executive + KE + HAL | seL4/Redox/Fuchsia = micro puro; BSDs = monolítico |
| **IPC primario** | pipes, sockets, futex, D-Bus (ad-hoc zoo) | `+ qrexec` (Qubes) broker policy | Mach ports + XPC (typed, capability rights) | ALPC (adaptive message/shm), named pipes, RPC sobre ALPC | seL4 sync IPC + caps / Zircon channels+handles / Redox schemes / BSD sockets |
| **Concurrencia** | CFS → EEVDF, futex userspace fast-path | Idem | GCD queues + QoS + workq_kernreturn | 32-level priority + quanta + boosts + fibers | seL4 fastpath IPC en registros; DragonFly LWKT |
| **Seguridad** | DAC + SELinux/AppArmor + cgroups + seccomp + LSM + eBPF KRSI | Xen compartmentalization / NixOS reproducibility / Alpine minimal | TrustedBSD MAC + Sandbox (seatbelt) + SIP + Endpoint Security | ACL (SID+DACL+SACL) + integrity levels + VBS/HVCI (hypervisor boundary) | **seL4 caps + CSpace**, **OpenBSD pledge/unveil**, FreeBSD Capsicum, jails+VNET |
| **Filesystem** | ext4 / XFS / btrfs + overlayfs | Nix content-addressed store | APFS (CoW + snapshots + clones + sealed volume) | NTFS (MFT+$LogFile+ADS+reparse) + ReFS (CoW+integrity streams) | **ZFS** (txg+uberblock+E2E checksums+ARC) / HAMMER2 |
| **Async I/O killer** | **io_uring** (SQE/CQE rings) | Idem | `kqueue` + GCD + async XPC | **IOCP** (precedió epoll/kqueue ~10 años) | — |
| **Safe programmability** | **eBPF** (verifier + sandbox + ISA) | Idem | DTrace (dyld shared cache) | ETW (manifest-schema lock-free) | — |
| **Paquete/install** | apt / dnf / pacman | **Nix CAS** / apk | pkg installer + notarization | MSIX + Windows Store | pkg (FreeBSD) / pkgsrc (NetBSD) |
| **Driver model** | in-tree kernel modules (inestable fuera) | Idem | **IOKit** declarative matching + DriverKit (user) | WDF/WDM stable ABI + PDB symbols | seL4 drivers = userland processes; anykernel (NetBSD) |
| **Status** | Dominante | Nicho vivo | Mainstream propietario | Mainstream propietario | seL4/Fuchsia research; BSDs nicho sólido |
| **Idea más robable** | io_uring + eBPF + sched_ext + cgroups v2 | NixOS declarativo + Qubes compartmentalization | Mach ports + APFS snapshots + GCD QoS | IOCP + Object Manager + ALPC + ETW + ACL + MSIX + VBS | **seL4 caps + ZFS txg + pledge + FIDL stubs + jails + anykernel** |

---

## Top 3 ideas para ALZE OS (cross-system, priorizadas)

### 1. Capability-based kernel con handles tipados, revocables, y stubs IDL generados

**Fuentes combinadas:** seL4 (primitiva), Zircon (rights bitmask + syscall enforcement), Fuchsia FIDL (IDL → stubs Rust/C), Fuchsia CFv2 (component manifests).

**Mecanismo concreto a implementar:**
- Cada kernel object (thread, page, endpoint, IRQ, FS node, TCP socket) se accede **sólo** por capability. Sin ambient authority. Sin concepto de "root" como autoridad global.
- Cada capability lleva `rights_bitmask` — subset de operaciones. `cap_duplicate(c, rights & mask)` deriva con menos derechos. Enforcement en syscall entry, no en cada handler.
- Los procesos publican/consumen protocolos via un `.cm` manifest. Un `component_manager` hace el routing — el programa recibe handles a lo que pidió, nunca abre namespaces globales.
- `IDL compiler` (estilo FIDL) genera stubs typed en tiempo de build. Wire format determinista + versionable. Mata el zoo de `ioctl` + socket + named pipe.

**Por qué gana:** sandbox y compartmentalization dejan de ser features añadidas — son el default del día 1. Pentesters, containers, micro-VMs todas caen del mismo modelo.

**Riesgo de copia literal:** seL4 de verdad verificado formalmente requirió 200K líneas de prueba Isabelle/HOL para 10K de C. ALZE puede empezar con el modelo de caps sin exigir verificación total, pero debe verificar al menos el IPC fastpath + el scheduler (ver idea 3).

### 2. Ring buffer I/O + scheduler pluggable por safe ISA

**Fuentes combinadas:** Linux io_uring (SQE/CQE dual ring), Windows IOCP (completion queue model maduro), Linux sched_ext (userspace BPF scheduler) + eBPF verifier.

**Mecanismo concreto:**
- **Syscall ABI primario = ring.** Par de rings `mmap`-eados entre kernel y userspace con entradas de 64 B. Syscall típico = `WRITE` + memory fence en userspace, kernel consume por poll o doorbell. Meltdown/Spectre mitigation cost = 0 (no syscall → no `swapgs`). Batching gratis.
- **Scheduler (CPU, I/O, memory, network) es BPF-like.** Verificador en kernel acepta bytecode de safe-ISA (Rust compilado a Wasm o equivalente). Callbacks `enqueue/dispatch/balance`. Deadman timer → si la policy stall, reverts a default conservador. Permite políticas custom por workload (gaming, batch, realtime).
- **Combina con cgroups v2-style unified hierarchy** para accounting — cada proceso pertenece a exactamente un grupo, todos los recursos (CPU, mem, IO, net, PID, custom) en el mismo árbol, PSI expuesto como ring.

**Por qué gana:** órdenes de magnitud en performance I/O y flexibilidad sin retrofit. Linux lleva 5 años retrofitando io_uring y sched_ext; ALZE puede empezar ahí.

**Riesgo:** complejidad del verificador. Es la componente más sutil del kernel. Empezar con un conjunto de primitivas muy restringido, expandir con casos de uso.

### 3. Sistema declarativo atómico con snapshots CoW y checksums E2E

**Fuentes combinadas:** NixOS (config declarativa + generations + CAS store), APFS (boot snapshots + sealed volume + atomic pointer swap), ZFS (txg + uberblock + Merkle checksum desde la raíz + ARC).

**Mecanismo concreto:**
- **`alze-system.conf`** (TOML/KDL) describe el sistema completo — paquetes, services, users, network, kernel cmdline. `alze rebuild switch` crea una nueva generación atómica. `--rollback` revierte al generation anterior en el siguiente boot. Nunca dejas un sistema medio-actualizado.
- **Store content-addressed** (`/alze/store/<blake3>-<name>`). Múltiples versiones coexisten. GC preciso sobre alcanzabilidad desde generations activas.
- **Filesystem nativo = ZFS-style txg + uberblock atómica.** Writes agrupados en transacción; commit = rotación del uberblock. Cada bloque lleva checksum **del hijo** → árbol Merkle verificable desde la raíz. Self-healing con mirrors/parity. Boot snapshot = apuntar a uberblock viejo. Update del sistema = crear snapshot, aplicar generation, atomic swap, reboot.
- **ARC (adaptive replacement) sobre LRU.** MRU+MFU con ghost lists, tamaño variable, se reduce bajo presión antes de swap.

**Por qué gana:** la triple "instalar rompe el sistema / update corrupto / rollback imposible" desaparece por construcción. Data integrity verificada E2E, no asumida. Container layers + OS updates comparten el mismo mecanismo (snapshot + overlay).

**Riesgo:** ZFS CDDL licensing. Implementar desde cero en Rust (proyecto `zfs-rs` tipo) o licenciar OpenZFS.

---

## Qué evitar (cross-system lessons)

Patrones vistos romper sistemas grandes — no replicar:

1. **Registry como hive mutable global (Windows).** Single point of corruption, write amplification, backup pesadilla. Si necesitas config central, usa el alze-system.conf declarativo (idea 3) — content addressable, versionado.
2. **`ioctl` + socket + named pipe zoo (Linux).** 450+ syscalls flat + cientos de `ioctl` codes + quirks per driver. ALZE: **sólo caps tipados + IDL stubs** (idea 1).
3. **Ambient authority (UNIX "root", Windows "Administrator").** Romper el modelo: la autoridad nace de tener la cap, no del UID. `sudo` deja de tener sentido.
4. **Out-of-tree driver cliff (Linux).** Sin ABI estable, cada driver third-party se pudre en 2 releases. ALZE: **driver model estable + drivers por default en userland** (IOKit/DriverKit-style) con un anykernel (NetBSD) opcional para in-kernel.
5. **X11-era split display/input + decada PulseAudio→PipeWire.** Decidir una arquitectura gráfica/audio **de día 1** (Wayland-plus o compositor propio con compositores guest via vsock estilo Qubes) y no reinventar en v2.
6. **musl como default (Alpine).** DNS sin `single-request`, `dlclose` que no libera, stack 128 KB, NSS ausente. Quema tiempo de usuarios. **glibc/uclibc o libc propia completa** como base; musl queda como perfil containers/embebidos.
7. **Drive letters + `MAX_PATH` + backslashes (Windows).** Solo VFS con paths completos tipo `<scheme>:/path` desde día 1.
8. **systemd expansionism (PID 1 hace demasiado).** Mantener PID 1 minúsculo. Un supervisor separado; sistema de services compuesto por programas independientes, no por un binario unificado.
9. **IOKit C++ dialect (Embedded-C++ con restricciones raras) + Seatbelt Scheme policy.** Elegir un **Rust safe-subset + una policy language declarativa tipo Cedar/Rego**, no reinventar Scheme sandboxed.
10. **Binary-compat con Win32 o Linux personality (ReactOS, WSL1).** Costoso, quedas atrapado en las decisiones del otro. Mejor: **userland compat opcional via syscall translation layer** (como lo hace WSL2 = VM dedicada) que compat binaria nativa.
11. **Formal verification del kernel ENTERO (seL4).** Cost/benefit pobre para uso general. Verificar solo IPC fastpath + scheduler + crypto primitives; el resto con property-based testing + fuzzing continuo.
12. **Kext / kernel modules con firma proprietary-only.** Si ALZE tiene un equivalente, el driver model **debe** permitir drivers fuera del vendor del OS desde día 1, con firma pero no gatekeeping.

---

## Ranking subjetivo de valor por fuente

De mayor a menor densidad de ideas implementables:

1. **otros.md** — el microkernel + BSD world tiene 20 ideas copy-able directas, casi ninguna presente en el mainstream. Este es el archivo más importante para arrancar.
2. **windows.md** — NT está subestimado. Object Manager + ALPC + IOCP + ETW + ACL son diseños que Linux sigue alcanzando.
3. **macos.md** — Mach ports + APFS + GCD son ganadores claros; IOKit es discutible.
4. **linux_mainstream.md** — io_uring, eBPF, sched_ext, EEVDF. Mucho valor pero ALZE ya asumirá algo así.
5. **linux_especializado.md** — NixOS es el aporte clave; Qubes es un modelo interesante pero quizás subsumido por caps-nativas.

---

**Próximos pasos si ALZE entra en design real:**

- Sintetizar las 3 ideas top en un **design document** con API boceto (syscall table = ~20 syscalls caps-oriented, IDL schema, FS on-disk format).
- Prototipo mínimo: microkernel Rust con capabilities + 1 scheme (fs) + ring-based syscalls = "hola mundo" pre-kernel hack.
- Buscar si alguna combinación específica ya existe en research (ej: Genode OS parece interesante — microkernel-agnostic + component model + caps).
