# "The rest" — microkernels, alternative OSes, and the BSDs

Sistemas que no ganaron el mainstream y justo por eso valen oro: ideas no diluidas por compromiso comercial. Este doc es el más cargado de ideas copiables para ALZE OS.

## Overview

- **seL4** — Microkernel L4 de 3ª generación, ~10 KLOC C. Único kernel de propósito general con **verificación formal** end-to-end (refinamiento funcional, integridad, flujo de información). Vivo, en producción en aviónica/defensa/automoción. Existe porque la industria necesita un TCB matemáticamente demostrable.
- **Redox OS** — Microkernel en Rust, kernel ~16 KLOC. Filosofía "everything is a URL" (extiende "everything is a file" de Unix). Vivo, casi self-hosting. Existe para demostrar que un OS moderno en Rust puede ser realista.
- **Fuchsia / Zircon** — Kernel object-capability en C++ de Google. FIDL para IPC, Component Framework v2. Vivo en producción (Nest Hub). Existe para sustituir Linux+kernels ad-hoc de Google con un diseño coherente basado en capabilities.
- **Haiku** — Reimplementación open-source de BeOS (kernel híbrido modular, fork de NewOS). Vivo, en Beta 5 (2024). Existe para preservar el paradigma BeOS: multihilo pervasivo, BMessage, FS con metadata como DB.
- **ReactOS** — Reimplementación binary-compatible de Windows NT. Vivo pero lento: en 2026 sincronizó MSVCRT contra Wine 10.0 (−30 % fallos). Existe para ofrecer un Windows libre que ejecute binarios NT existentes.
- **FreeBSD** — UNIX monolítico, linaje BSD 4.4. Vivo, producción masiva (Netflix CDN, PlayStation). Existe como UNIX conservador con ZFS, jails, bhyve y pila de red de clase servidor.
- **OpenBSD** — Fork de NetBSD centrado en seguridad proactiva (1996–). Vivo. Existe para demostrar que la corrección y la simplicidad del código son la mejor defensa; inventa primitivas (pledge, unveil, W^X, arc4random, LibreSSL) que luego copia el mundo.
- **NetBSD** — UNIX centrado en portabilidad (~60 arquitecturas). Vivo. Existe para ser "el UNIX que corre en todo", y produjo el **rumpkernel / anykernel**: un diseño radical para reutilizar drivers en userland o bare-metal sin cambios.
- **DragonFly BSD** — Fork de FreeBSD 4.x (2003) de Matthew Dillon. Vivo aunque pequeño. Existe para explorar SMP con light-weight kernel threads (LWKT) y para desarrollar **HAMMER2**, un FS COW clusterable.

## Arquitectura

- **seL4**: microkernel verificado formalmente. Todo objeto del kernel (threads, CNodes, endpoints, page tables, untyped memory) se referencia por **capabilities** almacenadas en **CSpaces** (árboles de CNodes). IPC síncrono por rendezvous en endpoints. Pruebas en Isabelle/HOL: refinamiento de la C implementation contra una Haskell spec ejecutable, contra una spec abstracta.
- **Redox OS**: microkernel Rust. IPC no inventa una primitiva nueva: reutiliza operaciones sobre URLs con esquemas (`file:`, `tcp:`, `bus:`, `log:`, `pipe:`). Cada scheme es un daemon en userspace; el kernel sólo enruta `open/read/write/close` al scheme handler. Consecuencia: kernel tiny (~16 KLOC) y superficie de ataque minúscula.
- **Fuchsia / Zircon**: microkernel object-capability en C++. Los syscalls operan sobre **handles** (enteros 32-bit) que referencian kernel objects y llevan *rights* por handle. Primitiva IPC: **channels** (bidireccional, datagram-like, transporta bytes + handles). Sobre channels se define **FIDL**, un IDL que genera stubs tipados. El Component Framework v2 declara qué capabilities ofrece/usa cada componente.
- **Haiku**: kernel híbrido modular (NewOS fork), C++ de arriba abajo. IPC de alto nivel por **BMessage** (mensajes estructurados con campos nombrados y tipados) enviados entre **BLoopers** (objetos con thread + cola). Arquitectura cliente/servidor: apps ↔ servers (app_server, registrar, etc.) ↔ kernel. **BFS** almacena extended attributes indexadas, permitiendo **live queries** (el filesystem como base de datos indexada).
- **ReactOS**: reimplementación de la pila NT. Kernel NT-style (Object Manager, HAL, IRP-based I/O), subsistema Win32 en userland, compatibilidad ABI binaria con drivers y apps NT. Userland comparte código extensivamente con Wine.
- **FreeBSD**: UNIX monolítico clásico, loadable kernel modules, VFS con ZFS y UFS2 first-class. **Jails** como isolation primitive nativa (extiende chroot con namespace de procesos/users/red). **bhyve** como hipervisor type-2 ligero. **DTrace** portado desde Solaris. **Capsicum** como capability framework.
- **OpenBSD**: UNIX con foco en corrección. Kernel conservador, auditado. **W^X** obligatorio en páginas. **pledge(2)** reduce el conjunto de syscalls disponibles por proceso tras `exec`. **unveil(2)** restringe el namespace de filesystem visible. **LibreSSL** como fork de OpenSSL. **arc4random** como CSPRNG estándar.
- **NetBSD**: UNIX portable (~60 ports). Drivers escritos contra una interfaz HAL-neutral. El **anykernel** permite que el *mismo código de driver* corra como: (1) driver in-kernel monolítico, (2) userland process vía **rumpkernel** (una librería del kernel NetBSD linkable contra POSIX / Xen / bare metal / KVM).
- **DragonFly BSD**: fork de FreeBSD 4.x. SMP no por big-lock sino por **LWKT** (light-weight kernel threads) con tokens/serializers en vez de mutexes pesados. **HAMMER2**: FS COW, snapshots baratos, dedup online + batch, dir-entry indexing, multiple mount roots, compresión, checksums E2E, replicación cluster con resolución de conflictos.

## En qué es bueno

- **seL4** — único microkernel con prueba formal de *refinamiento* + integridad + non-interference. IPC fastpath ~200 ciclos. TCB mínimo y auditado línea a línea.
- **Redox** — demuestra que un OS moderno en Rust es viable; el modelo de schemes da un namespace extensible homogéneo que Linux nunca tuvo limpio.
- **Fuchsia** — FIDL genera IPC tipado en múltiples lenguajes; Component Framework hace explícito el contrato de capabilities; upgrade de drivers sin reboot.
- **Haiku** — UX de escritorio extremadamente coherente, latencias bajas, `Tracker` + live queries ofrecen algo que ningún FS mainstream iguala.
- **ReactOS** — único proyecto que seriamente reimplementa drivers y kernel NT (no sólo userland como Wine).
- **FreeBSD** — networking stack de referencia, ZFS-on-root en producción, jails simples y testeados, bhyve + VNET bastan para montar multi-tenant sin Docker.
- **OpenBSD** — seguridad por diseño: pledge/unveil, W^X, SMAP/SMEP obligatorios, exploits reales mitigados *antes* de descubrirse. Código limpio y legible.
- **NetBSD** — portabilidad absurda. Rumpkernel permite correr drivers NetBSD en Linux userland, en unikernels, o en tests unitarios sub-segundo.
- **DragonFly** — HAMMER2 COW + cluster, LWKT model limpia para SMP masivo.

## En qué falla

- **seL4** — userland anémico: necesitas construir casi todo encima (no hay "distro" usable). Verificación no cubre drivers externos ni el compilador (aunque hay CompCert C). Curva de aprendizaje brutal.
- **Redox** — driver coverage mínimo (USB, GPU, Wi-Fi flojo). Ecosystem pequeño. Todavía no self-hosting completo.
- **Fuchsia** — Google-controlled, RFC process opaco, futuro producto incierto tras recortes. Código abierto pero gobernanza no. Portar apps Linux cuesta.
- **Haiku** — 32-bit todavía principal, x86_64 Beta. No hay navegador moderno estable (WebPositive rezagado). Comunidad pequeña.
- **ReactOS** — años atrás de Windows en compatibilidad. No WoW64 en x86_64. Drivers modernos (GPU) no funcionan; inestable para uso diario.
- **FreeBSD** — hardware gaming/laptop flojo (Nvidia sólo propietario, Wi-Fi 802.11ac parcial). Portes Linux necesarios para muchas apps desktop. Linuxulator parcial.
- **OpenBSD** — performance inferior en I/O multihilo (elección deliberada). SMP scaling limitado. Sin ZFS, FS (FFS) sin checksums. Desktop minimal.
- **NetBSD** — performance media, desktop experience casi inexistente, pocos usuarios fuera de embebidos.
- **DragonFly** — comunidad muy pequeña, HAMMER2 no production-hardened como ZFS, hardware support limitado.

## Cómo funciona por dentro

### Microkernel IPC

- **seL4**: una operación IPC típica (`Call`) es un rendezvous síncrono sobre un endpoint. Sin buffers en kernel — sólo colas de threads esperando. El **fastpath** está en `src/fastpath/fastpath.c`: si (a) el mensaje cabe en registros (`seL4_FastMessageRegisters`, típicamente 4), (b) hay un receptor esperando, (c) prioridades/VSpace compatibles, entonces el kernel salta la ruta completa, copia 4 registros, cambia VSpace, retorna. Medido en ~200 ciclos en ARM. `Call` envía un mensaje y además genera automáticamente un **reply capability** de un solo uso que el receptor usa con `ReplyWait`.
- **Fuchsia / Zircon**: la primitiva es **channel**, un par de endpoints con handles de send/receive. `zx_channel_write` acepta un blob de bytes y un array de handles — los handles se *transfieren* (no se copian), invalidándose en el origen. FIDL genera stubs que serializan structs a bytes + handles siguiendo el wire format FIDL (layout determinista, pointers ofsets, out-of-line tables).
- **Redox**: no hay primitiva IPC dedicada. `open("tcp:1.2.3.4:80")` llega al scheme handler `tcp:`, que es un proceso userland registrado en el kernel. El kernel sólo hace dispatch; read/write/fsync/close fluyen al daemon. Consecuencia: IPC = VFS ops, lo que simplifica el kernel pero mete un extra hop por cada operación.

### Capability-based security

- **seL4 CSpace**: cada thread tiene una **CSpace root** (un CNode). Un CNode es una tabla de slots; cada slot contiene una capability (puntero a kernel object + derechos). Las capabilities se direccionan por `(CPtr, depth)` — una secuencia de bits navegando el árbol de CNodes. El kernel nunca acepta un puntero raw, sólo CPtrs. Operaciones: `Mint` (derivar con menos derechos), `Revoke` (invalidar descendientes), `Copy`, `Move`. Todo objeto del kernel (incluso página física) es accesible sólo vía capability.
- **Zircon handles**: cada proceso tiene una **handle table**. Syscalls aceptan `zx_handle_t` + rights check. Rights comunes: `ZX_RIGHT_READ/WRITE/EXECUTE/MAP/DUPLICATE/TRANSFER/WAIT`. `zx_handle_duplicate` con rights reducidos es el equivalente a seL4 Mint.
- **Capsicum (FreeBSD)**: retrofita capabilities sobre UNIX. Un proceso entra en **capability mode** con `cap_enter()`: a partir de ahí no puede usar namespaces globales (`open` absoluto, `connect`, `bind` a puerto nuevo), sólo derivar file descriptors de los que ya tiene. Cada fd puede limitarse con `cap_rights_limit()` a un subset de `CAP_READ/WRITE/FSTAT/MMAP/...`. Usado en `tcpdump`, `hastd`, `dhclient`, Chromium sandbox en FreeBSD.

### Jails / containers native

- **FreeBSD jails**: `jail(8)` crea un entorno con (a) chroot, (b) namespace de procesos (jail sólo ve sus PIDs), (c) namespace de users (root dentro ≠ root fuera), (d) opcionalmente **VNET** — pila de red virtualizada completa (IFs, routing table, firewall, sockets propios). VNET se introdujo en 8.0, default-on en GENERIC desde 12.0. `epair(4)` es un "cable ethernet virtual": un extremo al host, otro al jail, y con `bridge`/`pf` compones topologías. Más ligero que VM (~MB RAM, arranque instantáneo), más fuerte que chroot. Límites por jail: `cpuset`, `rctl` (memoria, fds, procesos).

### Filesystems que copiaríamos

- **ZFS**: tres ideas grandes que ningún FS mainstream Linux tiene limpias simultáneamente:
  1. **Transaction groups (txg)**: cada escritura se agrupa en un txg numerado. Cada ~5s (o cuando `dirty_data` supera un umbral) el txg se cierra y se hace `sync` atómico. Power loss → txg parcial se descarta completo; nunca hay estado intermedio en disco.
  2. **COW + Merkle**: todo bloque escrito es nuevo; la `uberblock` se actualiza atómicamente al final del txg. Cada bloque lleva checksum (sha256/fletcher4) *del bloque hijo*, no del propio — árbol de Merkle desde la uberblock. Self-healing: si lees un bloque con checksum malo y tienes mirror/RAIDZ, ZFS repara on-read.
  3. **ARC (Adaptive Replacement Cache)**: cache en RAM, DVA-keyed, block-size variable (512B–16MB). Mantiene **MRU + MFU** simultáneamente con "ghost lists" para detectar qué algoritmo funcionaría mejor y ajusta `arc_c` dinámicamente. Supera LRU y LFU en workloads mixtos. Buffers con refs activas son no-evictables; throttle limita allocs nuevos bajo presión.
- **HAMMER2**: COW también, pero con **radix tree dinámico** donde cada entrada controla cuántos bits consume. Snapshots O(1). Dedup online y batch. Mountable snapshots como FS independientes. Clustering nativo multi-nodo con resolución eventual (no en ZFS).

### pledge / unveil

- **pledge(2)**: un proceso declara un conjunto de *promises* (strings), p.ej. `"stdio rpath inet"`. Cada promise habilita un subset de syscalls — hay ~30 subsets. Llamadas fuera del subset → `SIGABRT` (o `ENOSYS` con `"error"`). Irreversible: sólo puedes **reducir** el set con llamadas subsiguientes. Apto para el patrón de init → pledge restringido → loop de trabajo. Diseño: Theo de Raadt + Bob Beck, BSDCan 2018.
- **unveil(2)**: primera llamada "drop a veil" sobre todo el filesystem — el proceso deja de ver nada. Luego, cada `unveil(path, perms)` hace visible un subárbol con permisos `r/w/x/c`. `unveil(NULL, NULL)` cierra la lista (no más unveils permitidos). Accesos fuera → `ENOENT`. Combina con pledge: pledge corta syscalls, unveil corta paths. Patchea ~400 programas de OpenBSD base.

### Formal verification

- **seL4** cubre con pruebas en Isabelle/HOL:
  - **Refinamiento funcional**: la impl en C satisface la spec abstracta (toda ejecución del binario corresponde a una ejecución permitida por la spec).
  - **Integridad**: un thread sin capability de escritura sobre un objeto no puede modificarlo.
  - **Confidencialidad / non-interference** (información flow): bajo ciertas policies, dos configuraciones indistinguibles desde el punto de vista de un sujeto permanecen indistinguibles tras cualquier ejecución.
  - **Binary correctness**: el binario (compilado con GCC) implementa correctamente la semántica C asumida — vía un translation validation paso.
  - No cubre: hardware bugs (Meltdown/Spectre), drivers externos, bootloader, compilador (parcialmente mitigado). No cubre timing side-channels en todos los modos.

## Qué podríamos copiar para ALZE OS

Lista agresiva de primitivas con origen identificado. Cada una independiente; se pueden adoptar selectivamente.

1. **Capabilities tipo seL4 + CSpace como primitiva única de autorización del kernel.** Todo kernel object (thread, page, endpoint, I/O port, IRQ handler) se accede sólo por capability. Ops `Mint/Copy/Move/Revoke` + attenuation por derechos. Elimina el concepto de "root" del kernel — la autoridad viene de tener la cap.
2. **IPC síncrono por rendezvous + fastpath por registros (seL4).** Sin buffers en kernel. Fastpath cuando mensaje cabe en N registros + receptor esperando. Target: <500 ciclos round-trip.
3. **Reply capabilities de un solo uso (seL4 Call).** Patrón RPC seguro: el cliente no necesita una cap permanente al servidor, sólo la obtiene por la duración de la llamada.
4. **Handles tipados con rights bitmask (Zircon).** Sobre la primitiva de cap, cada handle lleva un bitmask de operaciones permitidas. `handle_duplicate(h, rights & mask)` para derivar con menos derechos. Enforcement en el syscall entry.
5. **IDL tipado con stubs generados (Fuchsia FIDL).** Compilador FIDL → stubs en Rust/C/Zig para ambos extremos, wire-format determinista, versionable. Componentes declaran en manifiesto qué protocolos exponen y consumen.
6. **Component manifests con capabilities declaradas (Fuchsia CFv2).** Cada programa publica un `.cm` que lista "offer" y "use". El component manager hace el routing — el programa no abre namespaces globales, sólo recibe lo que su manifest pide.
7. **pledge(2) estilo OpenBSD: reducción irreversible post-exec del set de syscalls.** Implementar como `alze_pledge("stdio rpath inet")`. ALZE OS al ser cap-based ya parte más restrictivo, pero pledge permite al propio programa autorrestringirse por fase (init vs loop).
8. **unveil(2): vista de filesystem reducida por proceso.** `alze_unveil(path, "rwx")` compone bien con capabilities: unveil opera sobre la cap al FS scheme. Patrón: pledge reduce syscalls, unveil reduce paths.
9. **Schemes URL como namespace unificado (Redox).** En vez de múltiples API (sockets, files, pipes, devfs, procfs, sysfs), todo es `scheme:/path`. `tcp:1.2.3.4:80`, `file:/etc/hosts`, `proc:42/status`, `ipc:/svc/name`. Cada scheme es un servicio userland registrable. Simplifica kernel y extensible sin recompilar.
10. **Jails-style isolation primitive (FreeBSD).** Una sola syscall/cap que crea un entorno con: chroot + PID namespace + user namespace + opcional VNET. Mucho más simple que Linux namespaces (7 tipos distintos) + cgroups. Con VNET: pila de red completa aislada, `epair`-cables virtuales para topologías.
11. **Capsicum capability mode para userland legacy.** Para programas escritos en estilo POSIX, un `cap_enter()` que les quita namespaces globales y los confina a los fds que ya tienen. Puerta de entrada para portar software UNIX sin reescribir.
12. **ZFS txg + uberblock atómica + checksums E2E en el FS nativo.** Writes agrupados en txg. Commit atómico rotando uberblock. Cada bloque carga checksum *del hijo* → árbol Merkle desde la raíz. Self-healing con mirrors. `txg_time` parametrizable por pool.
13. **ARC en lugar de LRU de páginas.** MRU + MFU con ghost lists, tamaño variable, bajo presión de memoria se reduce antes de swap. Métricas `arc_hits/misses/evictions` visibles.
14. **COW radix tree de HAMMER2 para snapshots O(1) y dedup online.** Cada entrada del radix elige dinámicamente cuántos bits consume — mejor uso de espacio que B-tree fijo.
15. **Anykernel design (NetBSD).** Kernel code factorizado tal que un driver/subsistema compila como: (a) in-kernel, (b) userland process, (c) librería enlazable contra un host (tests, simuladores). Permite unit-testing de drivers y recovery de fallos (driver crash ≠ kernel panic).
16. **BMessage-style typed structured messages (Haiku) como capa sobre IPC raw.** Mensajes con campos nombrados `(name, type, value)` además de bytes planos. Útil para desktop/app layer donde FIDL es overkill.
17. **Live queries sobre extended attributes indexadas (BFS).** El FS mantiene índices sobre xattrs; una query abierta notifica cuando entran/salen matches. Aplicaciones reactivas sin daemons custom.
18. **W^X obligatorio + ASLR + SMAP/SMEP (OpenBSD).** Línea base: ninguna página escribible y ejecutable simultáneamente; kernel no ejecuta ni deferencia memoria user sin marcadores explícitos.
19. **arc4random como CSPRNG del sistema, sin `/dev/random` blocking.** API simple, seeded desde entropía kernel, rekeyed automáticamente. Obsolete `rand()` y `getrandom()` ceremonia.
20. **Verificación formal al menos del fastpath IPC y del scheduler.** Incluso si no se verifica el kernel entero, Isabelle/HOL o Lean sobre las rutinas críticas + property-based tests sobre el resto. Copiado: spec ejecutable (Haskell/OCaml) que sirva tanto como oracle de tests como target de refinamiento futuro.

## Fuentes consultadas

- [seL4: Formal Verification of an OS Kernel (SOSP 2009)](https://www.sigops.org/s/conferences/sosp/2009/papers/klein-sosp09.pdf)
- [Comprehensive Formal Verification of an OS Microkernel (TOCS)](https://sel4.systems/Research/pdfs/comprehensive-formal-verification-os-microkernel.pdf)
- [seL4 Whitepaper](https://sel4.systems/About/seL4-whitepaper.pdf)
- [seL4 IPC tutorial](https://docs.sel4.systems/Tutorials/ipc.html)
- [seL4 fastpath.c source](https://github.com/seL4/seL4/blob/master/src/fastpath/fastpath.c)
- [Zircon fundamentals — fuchsia.dev](https://fuchsia.dev/fuchsia-src/get-started/learn/intro/zircon)
- [Fuchsia FIDL intro](https://fuchsia.dev/fuchsia-src/development/languages/fidl/intro)
- [Fuchsia Components v2 introduction](https://fuchsia.dev/fuchsia-src/concepts/components/v2/introduction)
- [Redox OS — home](https://www.redox-os.org/)
- [Redox: a Rust-based microkernel (LWN)](https://lwn.net/Articles/682591/)
- [Redox: An operating system in Rust (LWN 2024)](https://lwn.net/Articles/979524/)
- [Haiku — Wikipedia](https://en.wikipedia.org/wiki/Haiku_(operating_system))
- [Haiku General FAQ](https://www.haiku-os.org/about/faq/)
- [A Programmer's Introduction to the Haiku OS (OSnews)](https://www.osnews.com/story/24945/a-programmers-introduction-to-the-haiku-os/)
- [ReactOS — Wikipedia](https://en.wikipedia.org/wiki/ReactOS)
- [ReactOS Starts 2026 With Another Major Step Toward NT6 — Phoronix](https://www.phoronix.com/news/ReactOS-Starts-2026)
- [FreeBSD Handbook — Jails and Containers](https://docs.freebsd.org/en/books/handbook/jails/)
- [Jail vnet by Examples (FreeBSD Foundation)](https://freebsdfoundation.org/wp-content/uploads/2020/03/Jail-vnet-by-Examples.pdf)
- [Capsicum: practical capabilities for UNIX (paper)](https://papers.freebsd.org/2010/rwatson-capsicum.files/rwatson-capsicum-paper.pdf)
- [Capsicum — Cambridge Computer Laboratory](https://www.cl.cam.ac.uk/research/security/capsicum/)
- [Pledge, and Unveil, in OpenBSD — Bob Beck, BSDCan 2018](https://www.openbsd.org/papers/BeckPledgeUnveilBSDCan2018.pdf)
- [pledge(2) manual](https://man.openbsd.org/pledge.2)
- [unveil(2) manual](https://man.openbsd.org/unveil.2)
- [OpenBSD's unveil() — LWN](https://lwn.net/Articles/767137/)
- [NetBSD rumpkernel wiki](https://wiki.netbsd.org/rumpkernel/)
- [Rump Kernels — USENIX ;login: 2014](https://www.usenix.org/system/files/login/articles/login_1410_03_kantee.pdf)
- [HAMMER filesystem — DragonFly BSD](https://www.dragonflybsd.org/hammer/)
- [HAMMER2 — Wikipedia](https://en.wikipedia.org/wiki/HAMMER2)
- [HAMMER2 DESIGN doc — DragonFly source](http://bxr.su/DragonFly/sys/vfs/hammer2/DESIGN)
- [ZFS ARC — OpenZFS DeepWiki](https://deepwiki.com/openzfs/zfs/3.1-arc-(adaptive-replacement-cache))
- [Activity of the ZFS ARC — Brendan Gregg](https://www.brendangregg.com/blog/2012-01-09/activity-of-the-zfs-arc.html)
- [The Dynamics of ZFS — Oracle Blog](https://blogs.oracle.com/solaris/the-dynamics-of-zfs)
- [ZFS Caching — 45Drives](https://www.45drives.com/community/articles/zfs-caching/)
