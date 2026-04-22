# BSD Family — FreeBSD / NetBSD / OpenBSD / DragonFly deep dive

**Round:** R4 ALZE OS
**Scope:** BSD kernels como familia — lineage CSRG → forks → divergence. FreeBSD production UNIX, NetBSD portable/anykernel, OpenBSD security-first, DragonFly LWKT + HAMMER2. Énfasis en innovaciones operacionales y de seguridad que un kernel de hobby (ALZE) puede copiar: kqueue, pledge/unveil, capsicum, jails, rump kernels, W^X.
**Fuera de scope (ya cubierto):** HAMMER2 details y ZFS internals (ver R3 `modern_filesystems.md`); kqueue como primitiva de event notification vs epoll/io_uring (ver R3 `async_io_models.md`, Lemon 2001 ref). Aquí se recapitula sólo lo mínimo y se amplía lo BSD-específico.
**Referencia R1:** `otros.md` dedicó ~30 líneas a los cuatro BSDs combinados. Este R4 profundiza 10×.

---

## 1. Lineage: CSRG 1977-1995 y el pleito USL v. BSDi

### 1.1 CSRG y las releases 1BSD-4.4

La **Computer Systems Research Group** (CSRG) del Berkeley Dept. EECS operó 1977-1995. Bill Joy empaquetó 1977 sus patches a UNIX/32V + `ex`/`vi` + Pascal en "1BSD" (~30 tapes a $50). **4.1BSD** (1981) = nuevo sched + VM demand-paged. **4.2BSD** (1983) = Berkeley sockets + FFS (McKusick) + symlinks + TCP/IP (primera libre). **4.3BSD** (1986) = NFS + sendmail. **4.3BSD-Reno** (1990) y **Net/2** (1991) empezaron a remover código AT&T. **4.4BSD-Lite** (1994) y **Lite2** (1995) fueron las finales tras el settlement.

CSRG cerró 1995 con ~150 papers, ~20 PhDs (Joy, Leffler, McKusick, Karels, Bostic, Ousterhout, Stonebraker, Fabry) y con el UNIX moderno definido: sockets, FFS, VM paginada, POSIX signals, job control. La mayor parte del UNIX que usamos viene de Berkeley, no de Bell Labs.

### 1.2 USL v. BSDi (1992-1994)

Berkeley Software Design Inc (BSDi, spinoff ex-CSRG 1991) vendía **BSD/386** — UNIX funcional a $995 vs $100k+ del System V. USL (AT&T sub) denunció a BSDi y a la Universidad en 1992 alegando que Net/2 contenía código AT&T. UC contrademandó: AT&T distribuía System V sin copyright notices Berkeley.

Settlement 1994: de ~18,000 ficheros Net/2, tres removidos, setenta modificados, el resto legítimamente libre. Forks tuvieron que re-basar sobre **4.4BSD-Lite**. Durante el pleito, la incertidumbre frenó BSD dos años; en esa ventana **Linux** (1991, sin herencia AT&T) capturó el mindshare. Sin el pleito, la historia "Linux vs BSD" habría sido inversa. Torvalds lo ha admitido públicamente.

### 1.3 Post-1995 forks

- **NetBSD** — fork 1993 de 386BSD por Chris Demetriou, Theo de Raadt, Adam Glass, Charles Hannum. Objetivo: portabilidad. Primera release NetBSD 0.8 (1993).
- **FreeBSD** — fork 1993 de 386BSD por Nate Williams, Rod Grimes, Jordan Hubbard. Objetivo: x86/i386 production quality. 1.0 en 1993, pero tras USL v. BSDi hubo re-base 2.0 (1995) sobre 4.4BSD-Lite.
- **OpenBSD** — fork 1995 de NetBSD por **Theo de Raadt** tras conflicto personal con el core NetBSD team (fue expulsado). 2.0 en 1996. Objetivo: security + correctness.
- **DragonFly BSD** — fork 2003 de FreeBSD 4.x por **Matthew Dillon** tras disagreements sobre el rumbo SMP de FreeBSD 5.x (que Dillon consideró mal-diseñado). Objetivo: light-weight kernel threads (LWKT) + clustering + HAMMER2.

Hay derivados/productos encima: **pfSense**/**OPNsense** (FreeBSD + firewall), **TrueNAS** (FreeBSD + ZFS — ahora también Linux variant), **FreeNAS** (legacy), **GhostBSD**/**MidnightBSD** (desktop FreeBSD), **HardenedBSD** (FreeBSD + security patches tipo OpenBSD), **Dragonfly Mail Agent**, **OrbisOS** (PS4/PS5, FreeBSD 9 fork).

### 1.4 Timeline condensado

| Año | Evento |
|-----|--------|
| 1977 | 1BSD release |
| 1983 | 4.2BSD (sockets + FFS + TCP/IP) |
| 1991 | Net/2 (primer intento BSD sin código AT&T) |
| 1992 | USL v. BSDi filed |
| 1993 | 386BSD → NetBSD fork + FreeBSD fork |
| 1994 | 4.4BSD-Lite settlement |
| 1995 | OpenBSD fork de NetBSD; CSRG cierra |
| 1999 | FreeBSD jails (Poul-Henning Kamp) |
| 2000 | OpenBSD integra OpenSSH, primera release |
| 2001 | kqueue FreeBSD 4.1 (Lemon) |
| 2003 | ULE scheduler (Roberson); DragonFly fork; OpenBSD W^X |
| 2010 | capsicum FreeBSD 9 (Watson/Neumann) |
| 2014 | LibreSSL fork (OpenBSD) post-Heartbleed |
| 2015 | pledge() OpenBSD 5.9 |
| 2018 | unveil() OpenBSD 6.4 |
| 2021 | HAMMER2 mount-able multi-node cluster |
| 2024 | FreeBSD 14; OpenBSD 7.5; NetBSD 10.0 |
| 2026 | FreeBSD 15 (estimado); OpenBSD 7.8/7.9 |

---

## 2. FreeBSD — performance + production UNIX

### 2.1 Production deployments en 2026

- **Netflix CDN (OpenConnect)**: toda la red edge corre FreeBSD. Drew Gallatin publica perf notes en BSDCan. 2015: 100 Gbps TLS por server; 2020: 400 Gbps multi-NIC; 2024: **800 Gbps** en AMD EPYC + Mellanox ConnectX-7 + kTLS + sendfile zero-copy. El sendfile(2) + in-kernel TLS + NUMA-aware VM es el stack que lo hace posible en FreeBSD y no (todavía) en Linux.
- **WhatsApp**: Erlang VM sobre FreeBSD, 2M conexiones concurrentes/server (2012). Rick Reed, High Scalability 2014.
- **Sony PlayStation**: OrbisOS (PS4 2013, PS5 2020) derivado de **FreeBSD 9**. Kernel modificado, userland Sony. ~200M dispositivos.
- **Juniper Networks**: Junos OS = FreeBSD kernel + routing suite. Backbone internet.
- **NetApp Data ONTAP legacy**, **Citrix NetScaler**, **Isilon/PowerScale OneFS** (FreeBSD+ZFS), **Yahoo histórico**, **Apple XNU userland** (sección 10).

La narrativa "BSDs son nicho" ignora que en infra-crítica (CDN, routing, NAS, consolas) FreeBSD *es* el UNIX de producción sin reconocimiento público.

### 2.2 Kernel features distintivas

- **kqueue** (Lemon 2001): sección 6.
- **ULE scheduler** (Roberson 2003): per-CPU runqueue + interactivity estimator + affinity. R3 `schedulers_modern.md`.
- **ZFS first-class**: portado desde illumos 2007, OpenZFS 2.0+ unificado. CDDL+BSD compatibles. `zfs-on-root` default en bsdinstall desde FreeBSD 10.
- **DTrace**: portado de Solaris. Cuarto OS con DTrace completo tras Solaris/macOS/NetBSD.
- **bhyve**: hipervisor type-2 desde cero (Neel Natu + Peter Grehan, FreeBSD 10). Más simple que KVM. Linux/Windows/FreeBSD guests.
- **jails**: sección 9.
- **Capsicum**: sección 8.
- **Netgraph** (Elischer): graph de nodos de red en kernel — módulos (Ethernet, PPPoE, tun, netflow) conectados por hooks. Único en UNIX world.
- **VIMAGE / VNET**: stack de red completamente virtualizado por jail. Más completo que Linux netns.
- **pf** (portado de OpenBSD, fork divergente).
- **GEOM** (PHK): framework de transformaciones stacked de almacenamiento. `gmirror`/`gstripe`/`geli`/`gpart` — pipeline I/O visible.

### 2.3 Governance

FreeBSD Foundation (501c3) desde 2000. Core Team elegido cada 2 años. ~300 committers. Proceso FRC para cambios grandes. No hay BDFL — consenso en mailing lists + core team como tiebreak. Modelo más horizontal que Linux (Linus BDFL) u OpenBSD (Theo BDFL).

---

## 3. NetBSD — portability + rump kernels

### 3.1 "Of course it runs NetBSD"

Lema oficial. NetBSD 2026 soporta **57 puertos activos**: amd64, i386, arm32, aarch64, sparc/sparc64, mips varios, alpha, vax, hp300, amiga, atari, sh3, m68k (sun3/mac68k/amiga), riscv, powerpc, x68k, luna68k, sun2/3/4/4u, dreamcast, landisk. NetBSD 10.0 (2024) mejoró aarch64 + riscv64 prod.

Clave: **MI/MD split riguroso**. `sys/arch/${arch}/` = machine-dependent (start.S, pmap, interrupt glue); `sys/kern/`, `sys/uvm/`, `sys/dev/`, `sys/net/` = MI. Nuevo port = implementar MD layer + device tree local. NetBSD *obliga* a factorizar MD estrictamente, diferencia cultural vs FreeBSD.

### 3.2 UVM — virtual memory

Charles Cranor 1998-99 diseñó **UVM** reemplazando el VM heredado Mach (lento y complejo):
- `vm_map` = `vm_map_entry`s en árbol rojo-negro.
- `vm_amap` = anonymous map (COW pages).
- `vm_aobj` = anonymous object (swap backing).
- Shadow chains más superficiales que Mach (menos indirección en COW fork).

FreeBSD se quedó con VM original (Alan Cox et al. lo optimizaron). OpenBSD adoptó UVM.

Cranor C.D., Parulkar G.M., "The UVM Virtual Memory System", USENIX ATC 1999. <https://www.usenix.org/legacy/events/usenix99/full_papers/cranor/cranor.pdf>.

### 3.3 Rump kernels — anykernel design

Antti Kantee, PhD thesis Aalto 2012: **"Flexible OS Internals: Design and Implementation of Anykernel and Rump Kernels"**.

Factoriza el kernel NetBSD de modo que **el mismo código** corra como:
1. in-kernel monolítico,
2. userland process sobre Linux/BSD host (testing),
3. librería linkable contra apps (unikernel-style),
4. rump server (driver en su propio proceso, accedido por IPC).

Mecanismo: drivers separados del "base" (schedulers/locks/threads). Drivers ven sólo un HAL re-implementable. `librumpkern.so` provee las primitivas en userland. Un TCP stack completo linkable: `rump_init()` en un proceso userland → TCP/IP privado independiente del host.

Aplicaciones 2026: Xen unikernels con rump (boot <100ms), testing driver como userland process, buildbots. Linux tiene intentos (UML, LKL) pero no sistemáticos — NetBSD es el único UNIX moderno con este design.

### 3.4 pkgsrc + NPF

**pkgsrc**: package system cross-OS (NetBSD, FreeBSD, OpenBSD, Linux, macOS, Solaris, AIX, Windows/Cygwin), ~25,000 paquetes. Similar a FreeBSD ports pero portable.

**NPF** (2010+, Mindaugas Rasiukevicius): packet filter alternativa a pf/ipfilter. Rule-set compilado a bytecode BPF-like, evaluación lock-free per-CPU, JIT opcional. Más moderno que pf. Poco usado fuera de NetBSD.

---

## 4. OpenBSD — security + correctness

### 4.1 Filosofía

Theo de Raadt desde 1995. Axioma: **"secure by default"**. Desactivar servicios por defecto. Auditar línea a línea. Preferir simplicidad a features. Publicar patches proactivos. La release de OpenBSD cada 6 meses (mayo + noviembre) es implacable desde 1996: 60+ releases sin saltar.

"Only two remote holes in the default install, in a heck of a long time" — slogan oficial.

### 4.2 Proyectos inventados/popularizados

- **OpenSSH** (1999, Markus Friedl + Damien Miller + Niels Provos): fork de SSH 1.2.12 de Tatu Ylönen (última versión libre). Hoy es *el* SSH en ~todo el mundo. Release canónico en OpenBSD + "portable" port para resto.
- **OpenNTPd** (2003): 14× menos código que ntpd (Mills). Evita CVEs repetidos.
- **OpenBGPD** (2003): BGP desde cero. Usado en IXPs.
- **OpenIKED** (2010): IKEv2 IPsec daemon.
- **LibreSSL** (2014): fork OpenSSL post-Heartbleed. Primera semana removieron ~90k líneas (VMS legacy, platforms obsoletas). Menor superficie. Usado en OpenBSD + macOS (desde 2016).
- **doas** (2015): ~400 líneas vs ~12,000 de sudo. Default en OpenBSD 5.8.
- **httpd(8)**: web server base (reuben.cr), config estilo nginx mínimo, ~3000 líneas.
- **relayd**, **iked**, **tmux** (fork original Nicholas Marriott 2007), **rcctl**, **syspatch**.

OpenBSD es la usina de infrastructure software del mundo UNIX. Linux consume sin contribuir upstream (salvo casos aislados).

### 4.3 Mitigaciones inventadas o popularizadas

Orden cronológico de integración OpenBSD. Este set es la razón de "secure-by-design":

| Año | Mitigación | Notas |
|-----|-----------|-------|
| 1996 | Chroot hardening | Remove setuid+fix race |
| 2003 | **W^X** | No page W+X simultánea. OpenBSD primero mainstream. |
| 2003 | **ASLR** | Userspace randomized (PaX Linux antes pero sólo patch). |
| 2003 | **ProPolice/SSP** | Stack canaries en gcc todo OpenBSD (Etoh, IBM). |
| 2004 | `arc4random` | CSPRNG non-blocking auto-rekeyed. Reemplaza rand/random. |
| 2008 | `malloc` hardening | Guard pages + randomized + junk fills. |
| 2013 | KARL | Kernel re-linkado al boot con orden aleatorio de funciones. |
| 2015 | **pledge()** | Sección 7. |
| 2016 | `mprotect` no-escalate W→WX. |
| 2017 | KPTI-like pre-Meltdown | Separación user/kernel VA ya presente. |
| 2018 | **unveil()** | Sección 7. |
| 2018 | MAP_STACK | Syscalls solo desde stacks marcados → stops ROP-heap. |
| 2018 | SROP mitigations | Syscalls solo desde libc pages. |
| 2019 | IBT/Intel CET | Indirect Branch Tracking. |
| 2020 | RETGUARD | Return address obfuscation per-function. |
| 2021 | `execute-only` mmap | PaX MPROTECT equivalent. |
| 2023 | xonly ARM64 | Execute-only text segments. |

Linux alcanza la mayoría hoy (KSPP patches) pero OpenBSD llegó primero y de forma sistemática. W^X y ASLR tardaron años en llegar a Linux mainline. De Raadt T., "Exploit Mitigation Techniques", BSDCan 2005-2018, <https://www.openbsd.org/papers/>.

### 4.4 Auditoría

De Raadt maintuvo desde 1996 el hábito de **audit continuo**: el security team lee código línea a línea buscando patrones de uso inseguros (strcpy, gets, integer overflow chains, race conditions). En los 2000s auditaron decenas de miles de líneas. El estilo de código OpenBSD (C KNF style) es deliberadamente legible para facilitar auditoría.

### 4.5 OpenBSD no tiene (y es una decisión)

- **No ZFS**: sólo FFS2 con soft-updates. Sin checksums E2E. Decision: ZFS es demasiado complejo para auditar, CDDL vs ISC license tensions, memoria pesada. Trade-off: OpenBSD trades data integrity por simplicity.
- **No SMP scaling fuerte**: la kernel usa **single big lock** históricamente (removido incrementalmente desde 2014). Network stack paralelizado ~2018. VM todavía tiene contención. FreeBSD + Linux escalan mejor en 64+ cores.
- **No jails nativas** (tiene `chroot` + `vmm` hypervisor + firewall-based sandbox).
- **No loadable kernel modules** (desactivados por default — los hacen "pre-loaded static" o nada).
- **No Linux binary compatibility** (removido ~2015 — demasiado complejo para auditar).

Cada "no" es una elección consciente. OpenBSD prefiere 90% funcionalidad de Linux + 10× seguridad auditada.

---

## 5. DragonFly BSD — Dillon's vision

### 5.1 Origen

Matthew Dillon (ex-FreeBSD committer, autor DICE compiler Amiga 80s) forkeó FreeBSD 4.x en julio 2003. Motivo: FreeBSD 5.x adoptó "fine-grained locking" SMP que Dillon consideró mal diseñado — demasiadas locks, race-heavy. FreeBSD 5 fue inestable hasta 5.3+, validando la preocupación.

### 5.2 LWKT — Light-Weight Kernel Threads

Diseño SMP alternativo:
- Cada CPU tiene su **LWKT scheduler privado** (no runqueue global).
- Thread pertenece a una CPU; migrar es explícito (IPI + "steal").
- **Tokens**: sincronización sin mutex. Un token es "owned" por una CPU; pasarlo = IPI. Más simple de razonar que mutex+PI.
- **Serializers**: tokens scope-local a subsistema.
- **No preempt por default**: un LWKT corre hasta yield explícito. Predecible.

Trade-off: menos locks + más determinismo, a costa de escalabilidad en workloads que ignoran affinity.

Dillon M., "Design elements of the FreeBSD VM system", BSDCan 2004. <https://www.dragonflybsd.org/presentations/>.

### 5.3 HAMMER y HAMMER2, clustering intent

**HAMMER** (2008, DFly 2.0): CoW con snapshots instantáneos, historial online, mirroring nativo. Single-node, no E2E checksums.

**HAMMER2** (2013+, estable ~2018): rediseño. Detalles en R3 `modern_filesystems.md`. Resumen BSD-específico: radix tree dinámico, dedup online+batch, snapshots O(1), múltiples mountable roots, **clustered multi-node** con resolución eventual, E2E checksums, compresión opcional. Único en el mundo BSD. Producción limitada (DFly es niche).

Dillon siempre posicionó DFly como "SSI cluster OS" — múltiples máquinas → un sistema. En 2026 sigue en progreso (k8s resolvió SSI en userspace) pero el HAMMER2 clustering primitive sobrevive.

---

## 6. kqueue — event notification unificada

### 6.1 Paper Lemon 2001

Jonathan Lemon (FreeBSD committer) presentó **"Kqueue: A generic and scalable event notification facility"** en USENIX ATC 2001 (freenix) + BSDCan 2001. Intro FreeBSD 4.1 (2000). Motivación: select/poll eran O(n); `/dev/poll` Solaris era stateful pero sólo fds. La tesis: el event API debe ser **unificado sobre todos los tipos de eventos**, no sólo fds.

### 6.2 API

```c
int kq = kqueue();
struct kevent changes[N];
struct kevent events[N];

EV_SET(&changes[0], sockfd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, udata);
EV_SET(&changes[1], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, udata);
EV_SET(&changes[2], pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, udata);
EV_SET(&changes[3], 0, EVFILT_TIMER, EV_ADD, NOTE_SECONDS, 5, udata);

int n = kevent(kq, changes, 4, events, N, NULL);
```

Un solo `kqueue()` maneja:
- **EVFILT_READ / EVFILT_WRITE**: fd ready
- **EVFILT_AIO**: async I/O completion
- **EVFILT_VNODE**: file modification (NOTE_DELETE, NOTE_WRITE, NOTE_EXTEND, NOTE_ATTRIB, NOTE_LINK, NOTE_RENAME, NOTE_REVOKE)
- **EVFILT_PROC**: proc events (NOTE_EXIT, NOTE_FORK, NOTE_EXEC, NOTE_TRACK)
- **EVFILT_SIGNAL**: signal
- **EVFILT_TIMER**: one-shot o periodic
- **EVFILT_USER**: user-triggered (útil para wakeup entre threads)
- **EVFILT_FS**: filesystem mount/unmount
- **EVFILT_EXCEPT**: out-of-band data

En Linux, cada tipo requiere API separado: `epoll` (fds), `signalfd` (signals), `timerfd` (timers), `inotify` (files), `pidfd + epoll` (proc), `eventfd` (user). Agregar uno nuevo necesita nuevo syscall y nuevo fd-type.

### 6.3 Diseño interno, cross-platform

`struct knote` por evento registered. Hash `(kqueue, ident, filter)` → knote. Evento dispara: source llama `knote()` callback → knote en ready-queue → thread en `kevent()` copia a userspace. Edge-triggered (`EV_CLEAR`), level (default), oneshot (`EV_ONESHOT`).

vs epoll/io_uring (ver R3): kqueue = readiness unificada + API estable + implementación simple (~2000 LOC de C). Punto dulce para ALZE. io_uring exige ring infra + memory-shared que complica hardening.

Presente en FreeBSD (origin), NetBSD, OpenBSD, DragonFly, macOS/iOS/Darwin. **No está en Linux** — Molnar+Linus rechazaron "otro API" cuando epoll existía. Libraries cross-platform que lo usan: libevent, libev, libuv (Node.js), tokio (Rust vía mio).

---

## 7. pledge + unveil — minimal hardening

### 7.1 pledge(2)

OpenBSD 5.9 (mar 2016). Autores: de Raadt, Beck, Holland, Unangst. `int pledge(const char *promises, const char *execpromises)`. `promises` = strings separados por espacios, ~30 subsets: `stdio`, `rpath`, `wpath`, `cpath`, `dpath`, `inet`, `unix`, `dns`, `tty`, `exec`, `proc`, `id`, `audio`, `bpf`, `pf`, `route`, `mcast`, `ps`, `settime`, `sendfd`, `recvfd`, etc.

**Irreversible reduction**: cada llamada sólo puede reducir. Violación → SIGABRT (coredump) por default, o ENOSYS con promise `"error"`. Patrón típico: load config / open files / connect peers; luego `pledge("stdio inet", NULL)`; loop sólo hace network I/O.

### 7.2 unveil(2)

OpenBSD 6.4 (oct 2018). Reduce la visibilidad del filesystem por proceso.

```c
int unveil(const char *path, const char *permissions);
```

`permissions` = subset de `r` (read), `w` (write), `x` (execute), `c` (create).

Primera llamada: el proceso deja de ver nada del FS excepto `path` (con los perms indicados). Llamadas subsequentes añaden paths. `unveil(NULL, NULL)` cierra el set — no más unveil permitido.

Accesos a paths fuera → `ENOENT`. Combina con pledge ("veil syscalls, unveil paths"). Pattern típico:
```c
unveil("/etc/myapp", "r");
unveil("/var/log/myapp", "rwc");
unveil("/tmp", "rwc");
unveil(NULL, NULL);  // seal
pledge("stdio rpath wpath cpath", NULL);
```

### 7.3 Por qué funcionan

- **API trivial**: dos syscalls, strings human-readable.
- **Coarse-grained**: no intentan MAC completo tipo SELinux (que nadie usa bien). El programa declara lo que necesita; no hay política central.
- **Developer-driven**: el autor de la app sabe mejor qué syscalls usa. Centralmente (administradores) es impossible maintain.
- **Incremental**: se añade a código existente sin rediseño. ~100-line diff por programa típico.

En 2026, ~90% de base OpenBSD usa pledge. ~400 programas unveil. El autor de la app comenta `# pledge("stdio inet dns", ...);` y el programa está sandboxed. Más fácil que seccomp-bpf (que requiere BPF programs).

### 7.4 Adopción fuera de OpenBSD

- **FreeBSD**: tiene capsicum (ver sección 8), no adoptó pledge/unveil. Algunos defienden portarlos; rechazado históricamente porque capsicum es "mejor" en teoría.
- **Linux**: hay intentos (seccomp wrapper libs: libpledge, pledge.com de Justine Tunney 2022 que funciona como trampoline sobre seccomp). No mainstream.
- **Go standard library**: `src/syscall/unix/pledge.go` sólo OpenBSD.
- **Rust crates**: `openbsd` crate expone pledge/unveil.

De Raadt T., Beck B., "pledge() and unveil() in OpenBSD", BSDCan 2018. <https://www.openbsd.org/papers/BeckPledgeUnveilBSDCan2018.pdf>.

---

## 8. capsicum — FreeBSD capability mode

### 8.1 Paper Watson+Neumann 2010

Robert N.M. Watson, Jonathan Anderson, Ben Laurie, Kris Kennaway. **"Capsicum: practical capabilities for UNIX"**, USENIX Security 2010. <https://www.cl.cam.ac.uk/~rnw24/papers/201008-usenixsec2010-capsicum-website.pdf>. Implementado en FreeBSD 9.0 (2012).

Motivación: retrofit capabilities sobre UNIX sin romper apps. Inspirado en KeyKOS/EROS, pero pragmático: aceptas el overhead de dual POSIX+capability para permitir compat.

### 8.2 Primitives

- **capability mode**: `cap_enter()` — proceso transitions. Una vez dentro, no puede:
  - abrir paths absolutos
  - hacer `bind`/`connect` a new addrs (salvo que el socket ya estuviera en ese estado)
  - usar `kill`, `ptrace`, `fork` que viola jail semantics
  - lookup en global namespaces (`/dev`, `/proc`)
- **cap_rights_limit(fd, rights)**: restringe un fd a subset de ops. ~60 rights: `CAP_READ`, `CAP_WRITE`, `CAP_MMAP_R`, `CAP_MMAP_W`, `CAP_MMAP_X`, `CAP_FCHMOD`, `CAP_FSTAT`, `CAP_IOCTL`, `CAP_ACCEPT`, `CAP_BIND`, `CAP_CONNECT`, etc. Un fd con `CAP_READ` sólo puede ser `read()`-able; syscall distinto → `ENOTCAPABLE`.
- **cap_ioctls_limit**: si tienes CAP_IOCTL, puedes limitar a lista específica de ioctls.
- **cap_fcntls_limit**: similar para fcntls.

Derivación: `openat(dirfd, "subfile", ...)` desde un dirfd en capability mode opera relativo — no absoluto — por lo que no puedes escapar del subtree. `casper` daemons son helpers que corren fuera de capability mode y proveen servicios (DNS, syslog, etc.) a procesos capsicum via socket/pipe.

### 8.3 Uso en producción

- **tcpdump** (FreeBSD): re-arquitecturado con capsicum. Parent opens BPF socket, child drops en capability mode con solo ese fd. Exploit en parser de protocolos → no puede escapar.
- **sshd** (en FreeBSD builds).
- **Chromium** sandbox en FreeBSD.
- **hastd** (HAST storage daemon).
- **dhclient**.

### 8.4 Capsicum vs pledge

|  | Capsicum | pledge |
|---|-----------|--------|
| Unidad | por fd + capability mode global | por proceso |
| Granularidad | rights bitmask per-fd | syscall subsets |
| API | `cap_enter` + `cap_rights_limit` | `pledge(promises)` |
| LOC para adoptar | ~200-500 por programa | ~10 por programa |
| Model mental | capability + derivación | promesa declarada |
| Adoption FreeBSD | ~30 programas | N/A |
| Adoption OpenBSD | N/A | ~400 programas |
| Ergonomía | media (requiere rearquitecturar) | alta (drop-in) |
| Expresividad | mayor (per-fd rights) | menor (coarse) |

Ambos válidos, distintos trade-offs. pledge ganó en adoption por ergonomía. Capsicum ganó en pureza académica (más cercano a capabilities "reales").

Watson R.N.M., Neumann P.G., "Capsicum: practical capabilities for UNIX", USENIX Security 2010. <https://www.cl.cam.ac.uk/research/security/capsicum/>.

---

## 9. jails — container precursor

### 9.1 PHK 2000 + primitives

Kamp P.H., Watson R.N.M., "Jails: Confining the omnipotent root", SANE 2000. <https://papers.freebsd.org/2000/phk-jails.files/sane2000-jail.pdf>. Intro FreeBSD 4.0 (mar 2000). Motivación: multi-admin hosting — chroot aislaba FS pero el root chroot-eado podía montar/reconfig/matar-fuera; jails cierra esas vías.

`jail(2)` syscall con struct: root path (chroot implícito), hostname, IPv4/IPv6 assignable set, process namespace (solo ve sus PIDs), user namespace (root interno ≠ root externo), VNET opcional. Anidables desde FreeBSD 8. `jail_attach(2)` = docker exec equivalent. Más fuerte que chroot, más barata que VM, más simple que Linux netns+cgroups.

### 9.2 VNET + rctl + cpuset

**VNET** (FreeBSD 8.0, 2009, default en GENERIC desde 12.0): por jail → routing propio, firewall propio (pf/ipfw), interfaces propios (típico `epair(4)` — cable virtual host↔jail), sockets propios, sysctls red propios. Con `bridge(4)` + pf construyes topologías arbitrarias. En Linux el equivalente son 5 primitivas ortogonales (veth+bridge+netns+iptables+netfilter); VNET es una sola.

`rctl(4)` = per-jail/process/user limits (RAM, fds, procs, pcpu%, bw). `cpuset(1)` asigna cores. Con rctl+cpuset+VNET+jail, FreeBSD ofrece containers con una década de ventaja sobre LXC (2008) y más de una década sobre Docker (2013).

### 9.3 Por qué no ganó mindshare

La oleada "containers = cloud abstraction" (2013-2017) fue Linux-centric: Docker marketing superior, Docker Hub/compose/swarm/k8s todo Linux-first, k8s asumió namespaces Linux. FreeBSD handicap: sin image layering canónico (ZFS clones ayudan pero no es el mismo flow). Hoy jails dominan nichos (TrueNAS, ISP multi-tenant, appliances) pero no ganaron la "cloud container" war.

### 9.4 Reimaginando para ALZE

Jails demuestran que **una sola primitiva kernel** puede reemplazar namespaces×7 + cgroups. ALZE cap-based: una "jail cap" derivable con attenuation daría containers *más seguros que Linux namespaces y más simples que jails POSIX*.

---

## 10. macOS / XNU relationship

### 10.1 XNU = Mach + BSD + IOKit

**XNU** = kernel macOS/iOS/iPadOS/tvOS/watchOS/visionOS. Híbrido:
- **Mach 3.0 core** (CMU 1990): task/thread/port/VM/scheduler base.
- **BSD personality**: file systems, sockets, POSIX syscalls, VFS — derivado FreeBSD.
- **IOKit**: framework drivers C++ (embedded runtime, no RTTI completo, no exceptions).

NeXTSTEP (Tevanian + Jobs 1989+) construyó sobre Mach 2.5 + 4.3BSD. Apple adquirió NeXT 1996; Mac OS X (2001) heredó. El "BSD part" re-sincroniza con FreeBSD: 10.0 (2001)=FBSD 3.x; 10.2=4.4; 10.5=5; 10.10+ (2014+) continua selectiva.

### 10.2 Código FreeBSD en macOS

- **Network stack** (`bsd/net/`, `bsd/netinet/`, `bsd/netinet6/`): TCP/IP mayormente FreeBSD.
- **libc userland**: herencia FreeBSD + Darwin extensions.
- **VFS layer**: derivado FreeBSD (HFS+ obsoleto, APFS propietario usa vnode-like interface).
- **dtrace**: via FreeBSD (que lo portó de Solaris).
- **kqueue**: XNU implementa y expone userland.
- **Utilities** (`ls find grep awk make sed`): BSD versions, no GNU. Diferencias notables (BSD `sed -i` requiere `""`).

Apple contribuye *poco* back a FreeBSD upstream — XNU es open source (APSL) pero sync es one-way. Modificaciones suelen ser iOS/Darwin-specific (IOKit, XPC, sandbox.framework). Contribuciones bidireccionales: lldb, clang+LLVM (Apple-born, sirve a todos los BSDs). launchd open-sourced 2006 pero FreeBSD no lo adoptó.

### 10.3 Para ALZE

macOS demuestra que un **hybrid kernel (microkernel base + BSD personality)** puede ser production-grade décadas. No hay que elegir monolítico vs micro; se puede tener task/thread/port Mach-style + file/socket BSD-style encima.

---

## 11. Tabla — BSD family comparison 2026

| | FreeBSD | NetBSD | OpenBSD | DragonFly |
|---|---------|--------|---------|-----------|
| **Fundación** | 1993 | 1993 | 1995 | 2003 |
| **License** | BSD-2 + BSD-3 | BSD-2 + BSD-3 | BSD + ISC | BSD-3 |
| **Primary strength** | Performance + scale production | Portability (57+ archs) | Security + correctness | LWKT SMP + HAMMER2 |
| **Signature feature** | ZFS + jails + capsicum + kqueue | rump kernels + anykernel | pledge/unveil + W^X + arc4random + LibreSSL | LWKT + HAMMER2 clustered |
| **Scheduler** | ULE | M2 (custom) | BSD 4.3-derived | LWKT |
| **Default FS** | UFS2 (soft-updates) / ZFS | FFSv2 / wapbl | FFS2 | HAMMER2 |
| **Package system** | pkg + ports | pkgsrc | ports (no binaries central) | dports (fork FreeBSD ports) |
| **SMP scaling** | Excellent (Netflix 800 Gbps) | Good | Moderate (historical big lock) | Good via LWKT |
| **Kernel LOC (approx, amd64)** | ~2.5 MLOC kernel + drivers | ~2.0 MLOC | ~600 KLOC (smallest) | ~1.5 MLOC |
| **Production users 2026** | Netflix CDN, PS4/PS5, Juniper, Sony, Isilon, NetScaler, TrueNAS | embedded, routers, academic | OpenBSD.amsterdam hosting, firewalls, OpenSSH bastions, security-sensitive sites | DragonFly-native niche (~hundreds) |
| **Release cadence** | ~2 years major, quarterly patch | irregular (~3-5 years major) | 6 months strict | ~12-18 months |
| **Governance** | Core Team (~9 elected) + Foundation | core@ (small team) | Theo de Raadt BDFL | Dillon-led |
| **Verified code** | no | no | extensive audit, no formal | no |
| **Linux compat** | partial (Linuxulator) | partial (compat_linux) | removed 2015 | partial |
| **Containers** | jails + VNET + capsicum | rump server style | chroot + vmm hypervisor | jails (inherited from FreeBSD 4.x) |
| **Hypervisor native** | bhyve | no | vmm | no |
| **Community size 2026** | large (~300 committers) | small (~80 committers) | small (~100 committers) | tiny (~20 active) |

Kernel LOC: contando `sys/` completo (drivers incluidos). OpenBSD es el más pequeño porque elimina agresivamente código: no ZFS, no Linux compat, menos drivers. FreeBSD es el más grande por base de drivers exhaustiva + ZFS + bhyve + capsicum + módulos.

---

## 12. ALZE applicability

### v1 — lo que se puede (y debe) copiar hoy en C99+asm

1. **Elegir licencia ISC (o BSD-2-Clause).** `/root/repos/alze-os/` no tiene LICENSE. ISC (8 líneas, usada en OpenBSD) es funcionalmente equivalente a BSD-2 pero más corta. Permite fork libre sin virality GPL, absorbable por Linux/macOS/Windows/Fuchsia/seL4. Alineado con filosofía ALZE (pequeño, educativo). **Acción**: agregar `LICENSE` con texto ISC + copyright `© 2024-2026 Ibrahim Boutereguila`.

2. **Implementar kqueue.** ALZE no tiene event-notification (blocking-only, ver R3 async_io). kqueue: 2 syscalls, cubre fds+signals+timers+proc+fs+user events, <2 KLOC implementable (hash `(kq,ident,filter)`→knote + ready-queue), extensible vía EVFILT_*.

3. **pledge(2)-style syscall restriction.** `alze_pledge(promises_bitmask)` reduce syscalls disponibles por proceso. Promises ALZE: `STDIO/RPATH/WPATH/INET/EXEC/PROC`. ~50 líneas en syscall dispatcher. Per-task bitmask, trap→kill o ENOSYS en violación.

4. **W^X obligatorio en VMM.** Revisar `/root/repos/alze-os/kernel/*/vm*.c`: ninguna page WRITE+EXECUTE simultánea. PTE check al mapping rechaza. Hacer explícit enforcement, no sólo convention.

5. **arc4random CSPRNG.** Reemplazar rand()-style con ChaCha20-based. Seedable de hw entropy (RDRAND) + timers. API: `arc4random/arc4random_buf/arc4random_uniform`. ~200 líneas. Ref: OpenBSD `sys/dev/rnd.c` + `lib/libc/crypt/arc4random.c`.

6. **Stack canaries.** Compilar kernel con `-fstack-protector-strong` + proveer `__stack_chk_guard` + `__stack_chk_fail`. ~30 líneas adicionales, ganancia de seguridad clara.

7. **ASLR en userland.** Cuando haya userland (v2), loader randomiza base text/data/stack/heap. 20-28 bits de entropía típicos sobre VA 48-bit.

### v2 — post capability model

8. **unveil(2) sobre capabilities.** `alze_unveil(fs_cap, subpath, rights)` deriva sub-cap con prefix + rights. Pledge corta syscalls, unveil corta paths.

9. **jail-style primitive.** UNA cap componiendo: FS subtree + PID filter + user namespace + opt net namespace. `isolation_context` cap derivable con attenuation. Más seguro que Linux namespaces, más simple que jails POSIX.

10. **VNET-style virtual net stack.** Múltiples instancias paralelas (por jail) en vez de global. Cada instancia = `net_context` con rt table + sockets + iface + fw rules. Más simple que Linux netns+iptables+conntrack.

11. **rump-style driver factorization.** Drivers ALZE compilables como (a) in-kernel, (b) user-test binary con HAL stub. Unit tests sub-segundo de USB/NVMe sin boot. ~15% overhead code structure, 10× dev velocity.

### v3 — aspiracional

12. **capsicum-style mode para POSIX compat.** Si ALZE añade POSIX personality, `cap_enter()` quita namespaces globales y confina a handles ya abiertos. Permite portar software UNIX sin reescribir.

13. **HAMMER2-style CoW FS.** Post-v2, FS persistent con radix tree dinámico + dedup online + snapshots O(1). ~5 KLOC versión simple.

14. **Auditoría estilo OpenBSD.** Hábito de review línea a línea. /root/repos/alze-os ~15 KLOC, auditable por 1 persona en 40-80h. Repetir cada sprint.

15. **pledge+unveil sobre VFS cap-based.** unveil opera sobre una cap (no path ACL global). El kernel no mantiene ACL — la cap ES el permiso, attenuation = policy.

16. **Syscalls por verb string.** OpenBSD usa números; pledge toma strings. ALZE v3: expose syscalls por verb → hash → dispatcher. Facilita pledge+logging+debugging.

---

## 13. Fuentes primarias

1. McKusick M.K., Neville-Neil G.V., Watson R.N.M., "The Design and Implementation of the FreeBSD Operating System", 2nd ed, Addison-Wesley 2015. ISBN 978-0321968975.
2. McKusick M.K., Karels M.J., Bostic K., Quarterman J.S., "The Design and Implementation of the 4.4BSD Operating System", Addison-Wesley 1996. ISBN 0-201-54979-4. <https://www.freebsd.org/doc/en_US.ISO8859-1/books/design-44bsd/>.
3. Lemon J., "Kqueue: A generic and scalable event notification facility", USENIX ATC 2001 (FREENIX track) + BSDCan 2001. <https://people.freebsd.org/~jlemon/papers/kqueue.pdf>. Archive: <https://web.archive.org/web/2020*/https://people.freebsd.org/~jlemon/papers/kqueue.pdf>.
4. de Raadt T., Beck B., "pledge() and unveil() in OpenBSD", BSDCan 2018. <https://www.openbsd.org/papers/BeckPledgeUnveilBSDCan2018.pdf>.
5. de Raadt T., "Exploit Mitigation Techniques — An Update After 10 Years", Hackfest 2015. <https://www.openbsd.org/papers/hackfest2015-mitigations/>.
6. Watson R.N.M., Anderson J., Laurie B., Kennaway K., "Capsicum: practical capabilities for UNIX", USENIX Security 2010. <https://www.usenix.org/legacy/event/sec10/tech/full_papers/Watson.pdf>. Website: <https://www.cl.cam.ac.uk/research/security/capsicum/>.
7. Kamp P.H., Watson R.N.M., "Jails: Confining the omnipotent root", SANE 2000. <https://papers.freebsd.org/2000/phk-jails.files/sane2000-jail.pdf>.
8. Kantee A., "Flexible Operating System Internals: The Design and Implementation of the Anykernel and Rump Kernels", PhD thesis Aalto University 2012. <https://aaltodoc.aalto.fi/handle/123456789/6318>.
9. Kantee A., Cormack J., "Rump Kernels: No OS? No Problem!", USENIX ;login: 39(5), 2014. <https://www.usenix.org/system/files/login/articles/login_1410_03_kantee.pdf>.
10. Dillon M., "DragonFly BSD design principles", DragonFly BSD web. <https://www.dragonflybsd.org/features/>.
11. Dillon M., "HAMMER2 Filesystem Design", source tree `sys/vfs/hammer2/DESIGN`. <http://bxr.su/DragonFly/sys/vfs/hammer2/DESIGN>.
12. Cranor C.D., Parulkar G.M., "The UVM Virtual Memory System", USENIX ATC 1999. <https://www.usenix.org/legacy/events/usenix99/full_papers/cranor/cranor.pdf>.
13. Roberson J., "ULE: A Modern Scheduler For FreeBSD", USENIX BSDCon 2003. <https://www.usenix.org/legacy/publications/library/proceedings/bsdcon03/tech/full_papers/roberson/roberson.pdf>.
14. Rasiukevicius M., "NPF: progress and perspective", AsiaBSDCon 2014. <https://www.netbsd.org/~rmind/pub/npf_asiabsdcon2014.pdf>.
15. FreeBSD Handbook — current release. <https://docs.freebsd.org/en/books/handbook/>.
16. OpenBSD Handbook / FAQ. <https://www.openbsd.org/faq/>.
17. NetBSD Guide. <https://www.netbsd.org/docs/guide/en/>.
18. DragonFly BSD Handbook. <https://www.dragonflybsd.org/docs/handbook/>.
19. Netflix Open Connect tech talks (Gallatin D.): <https://netflixtechblog.com/> and BSDCan proceedings 2015, 2017, 2019, 2021.
20. Salus P., "A Quarter Century of UNIX", Addison-Wesley 1994. (USL v. BSDi context.)
21. McKusick M.K., "Twenty Years of Berkeley Unix", in "Open Sources: Voices from the Open Source Revolution", O'Reilly 1999. <https://www.oreilly.com/openbook/opensources/book/kirkmck.html>.

---

## 14. Nota honesta final

Los BSDs son los **unsung winners** de la "Linux vs alternatives" war. En 2026 ship en más infraestructura crítica de lo que la cultura Linux-centric reconoce:
- PS4/PS5 = FreeBSD modificado (~200M dispositivos).
- Junos routers/switches = FreeBSD. Backbone internet.
- Netflix CDN = FreeBSD (~15% del tráfico internet en pico).
- OpenSSH en cada SSH session del mundo = dev en OpenBSD.
- macOS/iOS/iPadOS/tvOS/visionOS heredan chunks masivos de FreeBSD.
- TrueNAS/FreeNAS storage.
- Miles de firewalls pfSense/OPNsense/OpenBSD.

Licencia BSD + governance descentralizada + foco en corrección técnica sobre mindshare ha sostenido infraestructura durante **50 años** sin crisis existencial. Compare con churn de alternativas (Solaris muerto abierto, AIX/HP-UX legacy, Plan 9 research). Los BSDs no compiten por mindshare; simplemente funcionan.

Para ALZE:

1. **Pick BSD license (ISC).** Modelo que sobrevive décadas sin governance issues.
2. **pledge/unveil como template de "seguridad por declaración del desarrollador".** No MAC, no capabilities, no seccomp — más simple que todos, más efectivo en práctica. ALZE cap-based puede tener capabilities puras + pledge como sugar encima.
3. **Leer FreeBSD `sys/kern/sched_ule.c` y `sys/vm/`.** Más limpio y comentado que Linux equivalent. Referencia educativa superior.
4. **No reinventar event notification.** Copiar kqueue verbatim — compat con libevent/libuv/tokio.
5. **Auditar continuamente.** OpenBSD 30 años de code review línea a línea. ALZE 15 KLOC = auditable por 1 persona en 2 semanas. Hacerlo antes de escalar a 50 KLOC.

El hype dice "innovation = new". Los BSDs dicen "sustainability = correctness + simplicity + patience". Para un proyecto solo de largo plazo, el segundo modelo escala; el primero no.

---

**Líneas:** ~510.
**Referencias:** 21 primarias.
**Next:** R4 `plan9_inferno.md`, R4 `hobbyist_oses.md`.
