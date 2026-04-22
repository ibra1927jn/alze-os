# Capability-based kernels — deep dive

**Ronda:** R3 / `capability_kernels.md`
**Fecha:** 2026-04-22
**Scope:** profundizar en el concepto de capability que R1/`otros.md` introdujo a vista de pájaro. Aquí: origen 1966, linaje completo Hydra→KeyKOS→EROS→CapROS, seL4 internals + verification pipeline, Zircon handle/rights model al detalle, Barrelfish multikernel (no tratado en R1), Mach ports heritage hacia XNU, problemas abiertos (confinement, revocation, leak), IDL+RPC generation, comparación estructural vs ACL/Unix, y la aplicación moderna (WASI, OCap, Pony, Rust-typed-caps).

Referencias cross-link evitadas aquí (ya en `otros.md`): fastpath seL4 en registros, CSpace-as-tree descripción básica, Zircon handle overview, Component Framework v2 overview. Este doc profundiza por debajo de eso.

---

## 1. El concepto de capability — origen y semántica

### Definición

Una **capability** es una referencia unforgeable (no falsificable) a un objeto, que *simultáneamente* es prueba de autoridad para ejecutar operaciones sobre ese objeto. Dos propiedades indivisibles:

1. **Designation**: identifica qué objeto. Sustituye "nombres" (paths, UIDs, integer IDs con ACL lookup).
2. **Authority**: autoriza qué operaciones. Sustituye permission checks separados (`stat` + mode bits + uid compare).

No hay "lookup by name + permission check". Tener la cap **es** la autoridad. Kerning de Alan Perlis: "a capability is a ticket to ride" — quien tiene el ticket sube al tren, sin mostrar DNI.

### Origen: Dennis + Van Horn 1966

- Jack B. Dennis + Earl C. Van Horn, *"Programming Semantics for Multiprogrammed Computations"*, Communications of the ACM, vol. 9 no. 3, March 1966. DOI 10.1145/365230.365252. [ACM](https://dl.acm.org/doi/10.1145/365230.365252) / [archive](https://web.archive.org/web/2023*/https://dl.acm.org/doi/10.1145/365230.365252).
- Introducen el término "capability" en el sentido OS. Contexto: MIT, compartición controlada entre procesos en sistemas multi-programados.
- Modelo: el kernel mantiene una tabla por proceso (C-list, capability list); cada entrada = (object-ptr, permission-bits). Syscalls operan sobre índices C-list, nunca sobre punteros raw.
- Ya aparecen primitivas `copy`, `restrict`, `revoke`. El paper mismo dice "seal/unseal" para extensibilidad.

### Implementaciones hardware tempranas

- **Cambridge CAP** (Wilkes+Needham, 1970–77): caps en hardware dedicado + microcódigo. Académico; no escaló comercialmente.
- **Plessey System 250** (1972, UK defense): primer comercial hardware-caps.
- **IBM System/38 → AS/400 → IBM i**: caps como "authority" en single-level store. Sigue facturando en 2026. Probablemente el único sistema cap-based con revenue masivo histórico.
- **Intel iAPX 432** (1981): fracaso — 10× más lento que 8086. Lección: caps a nivel hardware por cada instrucción no funciona. Consenso moderno: caps en software sobre MMU estándar.

### Hydra — CMU, 1974

Primer OS cap-based completo en software. Introduce: (1) **objects typed** — cada objeto tiene un type manager; (2) **rights por operación** — bitmap por cap; (3) **amplification** — un type manager puede añadir rights privados al mismo type. Ancestro directo de "drivers as userland processes" en seL4/Fuchsia/Redox.

### Contraste estructural vs ACL (Unix)

| Eje | Capability-based | ACL-based (Unix, Windows NTFS) |
|---|---|---|
| **Autoridad** | Inherente a la referencia (tenerla = poder) | Almacenada en el objeto (consulta en cada acceso) |
| **Namespace** | Privado por proceso (C-list/CSpace/handle table) | Global (paths, object names) |
| **Delegación** | Natural: dar la cap (con o sin atenuación) | Forzada: compartir credencial o escribir ACL |
| **Confused deputy** | Estructuralmente evitado (designation = authority) | Clásico — el servicio usa SUS permisos, no los del caller |
| **Revocación** | Problema diseñado (seL4 CNode recursion; KeyKOS indirection) | Trivial (mutar el ACL) |
| **Ambient authority** | No existe por construcción | Ubicua (`root`, `Administrator`, current working dir) |
| **Complejidad conceptual** | Alta al principio (hay que diseñar el namespace) | Baja (todo mundo entiende mode bits) |

El "confused deputy" de Norm Hardy ([1988 paper](https://capsec.org/archives/Hardy-confused-deputy.pdf)) es el ejemplo canónico: un compilador SUID que escribe billing log en `/sys/billing.log` puede ser engañado por un cliente que pida output en `/sys/billing.log` — el compilador escribe con sus propios permisos. En un sistema de caps, el cliente tendría que *dar* una cap de escritura; no puede "engañar" al compilador para usar la de billing porque el compilador sólo tiene la cap concreta al billing log, no autoridad ambiente "puedo escribir en /sys".

---

## 2. KeyKOS → EROS → CapROS — el linaje más puro

### KeyKOS — Tymshare, 1980s (Ann Hardy + Norm Hardy + Charles Landau)

Construido para commercial timesharing. Terminología: capability = "key". Rasgos distintivos:
- **Persistence ortogonal**. Snapshot completo (registers, page tables, caps) cada ~30s a disco. Crash = rewind al último snapshot; la app no ve el crash. Un cliente de Tymshare corrió 15 años sin reboot lógico.
- **Single-level store**. No hay FS vs memoria; todo son "segments" accedidos por caps. Cache de disco invisible al dev.
- **Meter / space bank** — CPU time y memoria son objetos cap-accesibles; agotar meter suspende el proceso.

### EROS — University of Pennsylvania, 1996–2005 (Jonathan Shapiro)

Re-engineering moderno de KeyKOS en C. IPC ~1.5 µs en P-II 300 MHz (1999); fastpath en asm. **Confinement proof** (Shapiro+Weber 2000): primera demostración formal de que el constructor EROS garantiza que un módulo instanciado no puede comunicarse fuera de sus caps iniciales. Resuelve constructivamente el confinement problem de Lampson 1973.

### CapROS — 2005–~2012 (Charles Landau)

Fork EROS enfocado a embedded / reliable systems. Recupera la persistence que EROS había dejado WIP. Proyecto dormido pero código disponible. Cita obligatoria si se explora persistence ortogonal.

### Lecciones para ALZE del linaje

1. **Persistence ortogonal** es tentadora pero cara — requiere que *todo* el estado del kernel sea snapshot-eable. No para v1.
2. **Single-level store** (KeyKOS/AS400) es un experimento caro pero coherente; Linux/BSD/Windows lo rechazaron por razones prácticas (hardware mmap reusable). ALZE: **no** en v1. FS + memoria separados es estándar y suficiente.
3. **Confinement como teorema estructural** — si ALZE adopta caps, debe diseñar los constructores de procesos de forma que el confinement se preserve (no "abrir una backdoor" via default caps al init). Ver §6.

---

## 3. seL4 — profundización real

### Historia

L4 original: Jochen Liedtke, GMD, 1993 (*"Improving IPC by Kernel Design"*, SOSP 1993). Segunda generación L4Ka::Pistachio, Karlsruhe 2001. NICTA/UNSW (Sydney) forma seL4 (security-enhanced L4) 2004 — Heiser + Elphinstone lideran. Verificación publicada SOSP 2009 (Klein et al). Open source: Junio 2014. seL4 Foundation 2020 (governance neutral). Miembros 2026: HENSOLDT Cyber, UNSW, Proofcraft, GD Mission Systems.

### Estructura del kernel

~10k líneas C (aarch64 port: ~12k). El kernel es single-threaded en su modelo de ejecución (big-kernel-lock implícito en uniprocessor; MCS scheduler opcional para multicore).

**Objetos kernel** (todos accedidos vía capability):

- **TCB** (Thread Control Block) — un thread.
- **CNode** — array de `2^N` slots; cada slot contiene una capability. Los slots forman el CSpace por navegación (radix-tree).
- **Endpoint** — sync IPC rendezvous point. Estado: idle, send-queue, recv-queue.
- **Notification** — async signal (equivalente a semaphore/condvar). Bitmask badge.
- **VSpace** — raíz de page tables (PGD en aarch64, PML4 en x86_64).
- **Frame** — página física (4 KiB, 2 MiB huge, 1 GiB huge).
- **Untyped** — memoria no tipada, puede ser `Retype`-ada en objetos concretos.
- **IRQHandler / IRQControl** — IRQ delivery + registration.
- **IOPort** (x86) / **IOSpace** (SMMU/IOMMU) — DMA-safe I/O access.
- **Reply** cap (MCS variant) — reply-once capability.
- **SchedContext** (MCS) — budget + period, attach-able a threads.

### Untyped memory + Retype

El modelo de memoria seL4 es inusual y elegante:

- Al boot, el kernel descubre toda la RAM libre y entrega caps `Untyped` al primer proceso (root server).
- `seL4_Untyped_Retype(untyped_cap, type, size, dest_cnode, slot, ...)` consume una porción de untyped y crea un objeto del tipo pedido, colocando la cap resultante en un slot específico.
- Untyped es jerárquico — retyping de una sub-region deja otra sub-region disponible. Es un bump allocator con revocation: `Revoke(untyped)` destruye todos los objetos derivados.
- **Consecuencia**: no hay `kmalloc` en el kernel. El kernel no decide cuánta memoria dedicar a threads vs page tables vs endpoints — el userland lo decide al retype-ar. Esto hace la verificación factible (sin allocator dinámico complejo en el kernel).

### CNode tree + capability addressing

- **CPtr** (Capability Pointer) = índice de bits + `depth` (cuántos bits usar).
- El CSpace root de un thread es un CNode. Si un slot contiene otra CNode cap, se entra recursivamente (guard bits).
- Lookup: `cap_lookup(cspace, cptr, depth)` navega el árbol. O(depth/radix).
- Guarda + radix bits por CNode permiten comprimir árboles "skipping" niveles.

### IPC — detalle por encima de fastpath

Fastpath ya está en `otros.md`. Slowpath y otros modos:

- **seL4_Send(cap, msg)** — block hasta que reciba; sin reply.
- **seL4_Call(cap, msg)** — send + implicit reply cap + recv; el kernel genera una Reply cap de uso único.
- **seL4_NBSend / seL4_NBRecv** — non-blocking variantes.
- **seL4_Wait(cap)** — recv de endpoint o notification.
- **Notifications**: async, bitmask-OR en "badge" del endpoint; no transportan datos más allá del badge. Equivalente a edge-triggered interrupt.
- **Badges**: al mintear una cap de endpoint, se le puede asignar un badge (integer). En el receive-side el receptor ve el badge — permite que un servidor distinga múltiples clientes sin una cap separada por cliente.
- **Message Registers**: primeros N words del mensaje en registros de la CPU (N=4 en ARM, 12 en x86_64 con msgregs). Más allá van al IPC buffer page (una Frame mappeada en ambos procesos o copiada).

### CAmkES — component framework

- CAmkES = seL4 Component Architecture and Middleware. [docs.sel4.systems/projects/camkes](https://docs.sel4.systems/projects/camkes/).
- Hecho sobre seL4 por Data61 (CSIRO). Genera al build time:
  - **CapDL** spec (capability distribution language) — quién tiene qué caps al arrancar.
  - **Glue code C** — marshalling de llamadas RPC entre componentes.
  - **Boot image** con las caps ya distribuidas.
- IDL: ADL (Architecture Description Language) y IDL4 para interfaces. Describes componentes, connection types (RPC, event, dataport), y system composition.
- **CAmkES VM**: variante para ejecutar Linux como guest sobre seL4 (usado en automoción — infotainment + safety-critical cluster en el mismo SoC).

### Verification pipeline

Tres niveles encadenados por **refinamiento** (cada nivel simula al abstracto):

1. **Abstract spec** — Isabelle/HOL, ~4k LOC. Modelo matemático.
2. **Executable spec** — Haskell, ~5k LOC. Ejecutable (oracle para testing); Isabelle/HOL la importa.
3. **C impl** — ~10k LOC. Parser C→Isabelle prueba que cada función C refina la Haskell.
4. **Binary translation validation** — SMT verifica que el ejecutable GCC implementa la semántica C asumida.

**Propiedades probadas**: functional correctness (refinamiento), integrity (sin write-cap → objeto intacto), authority confinement (info-flow bajo policies), bound delay.

**Costo**: ~200k LOC Isabelle/HOL para 10k C. ~20:1 prueba:código. ~25 py iniciales ARM (2009), +5 py cada port (x86_64 2013, RISC-V 2020). Heiser 2022: **2–3 engineer-years por kLOC** para verification nueva. Exclusivamente manual.

**NO cubre**: hardware bugs (Meltdown/Spectre/MDS), bootloader, compilador (parcialmente), drivers externos, timing side-channels (parcial), DMA sin IOMMU configurado.

---

## 4. Zircon / Fuchsia — capabilities de producción

### Historia y status 2026

Travis Geiselbrecht (ex-NewOS/Haiku, ex-iOS kernel) inicia Zircon ~2016, public ~2019. Deployment productivo: Nest Hub y Nest Hub Max shipping desde 2021. 2022–23: layoffs grandes en el equipo Fuchsia; 2024–26 sigue committing pero slower. Google narrativiza "Fuchsia embedded".

### Handle + rights model

- **Handle** = `zx_handle_t`, integer 32-bit. Tabla por proceso.
- Cada handle apunta a un **kernel object** (KOID) + lleva un **rights** bitmap.
- Rights comunes (definidos en `//zircon/system/public/zircon/rights.h`):
  - `ZX_RIGHT_DUPLICATE` — `zx_handle_duplicate` permitido.
  - `ZX_RIGHT_TRANSFER` — write a través de channel permitido.
  - `ZX_RIGHT_READ` / `ZX_RIGHT_WRITE` — data ops.
  - `ZX_RIGHT_MAP` — mmapable VMO.
  - `ZX_RIGHT_EXECUTE` — página ejecutable (W^X: no co-habita con WRITE).
  - `ZX_RIGHT_WAIT` — wait/signal ops.
  - `ZX_RIGHT_INSPECT` — introspection (obtener info del kernel object).
  - Tipadas: `ZX_RIGHT_MANAGE_JOB`, `ZX_RIGHT_MANAGE_PROCESS`, etc.
- `zx_handle_duplicate(h, rights_subset, out)` — crea un nuevo handle al mismo objeto con rights ≤ originales.
- `zx_handle_replace(h, rights_subset, out)` — consume el original, devuelve uno atenuado (atomic).
- Syscall entry verifica: (a) handle existe en la tabla del caller, (b) los rights requeridos por el syscall están presentes. Falla → `ZX_ERR_ACCESS_DENIED`.

### Primitivas IPC

- **Channel** — par bidireccional. `zx_channel_create()` devuelve dos handles. `zx_channel_write(h, bytes, handles)` envía un blob + array de handles. Los handles se *transfieren* (invalidados en origen, materializados en destino con la misma KOID pero handle number nuevo).
- **Socket** — stream o datagram, bytes only (no handles). Para bulk data.
- **FIFO** — cola bidireccional de elementos fixed-size. Para sync rápido.
- **Event** / **EventPair** — signal objects.
- **Port** — equivalente a kqueue: multiplexing de waits sobre múltiples objetos. Edge/level config.
- **VMO** (Virtual Memory Object) — equivalente a page-cache / shmem. Se mappea en address spaces vía `zx_vmar_map`.

### FIDL — Fuchsia Interface Definition Language

- Spec: [fuchsia.dev/fuchsia-src/reference/fidl](https://fuchsia.dev/fuchsia-src/reference/fidl/language/language) / [archive](https://web.archive.org/web/2024*/https://fuchsia.dev/fuchsia-src/reference/fidl/language/language).
- IDL tipado. Tipos built-in: primitives, strings, vectors, arrays, tables (versionable), unions, handles (con rights subset).
- `fidlc` compilador → genera bindings para Rust, C++, Dart, Go, Java.
- **Wire format** determinista: offsets de 8-byte, out-of-line secondary objects, handles separados del data stream (pasan por channel's handle array).
- **Evolution model**: `table` con campos ordinal-indexados permite añadir campos sin romper wire compat. `strict union` vs `flexible union`. `resource struct` = contiene handles.
- **Protocols**: una colección de methods one-way y two-way (request/response). Un `protocol` FIDL se compila a un objeto cliente (envía) y un trait/interfaz servidor (implementa y maneja requests).

### Component Framework v2 (CFv2)

- Cada componente declara un `.cml` manifest (Component Manifest Language, CUE-like / JSON5).
- Declara: `use`, `offer`, `expose`, `capabilities`, `children`.
- El Component Manager es el único con "ambient authority" y distribuye caps según los manifests.
- Runtime types: ELF binary, Dart VM, Web runner, etc.
- Desde 2022, CFv1 deprecated. CFv2 es el único modelo.

### Números de código

- Zircon kernel: ~150k LOC C++ (mucho más que seL4). Sin verificación formal.
- Escrito en C++ moderno (`kmalloc` estilo `ktl::unique_ptr`, RAII, some templates). No hay `std::`; hay `ktl::` (kernel template library).
- Soporta x86_64 + aarch64 + RISC-V (WIP).

---

## 5. Barrelfish — multikernel

### Qué es

Baumann+Barham+Dagand+Harris+Isaacs+Peter+Roscoe+Schüpbach+Singhania, *"The Multikernel: A New OS Architecture for Scalable Multicore Systems"*, SOSP 2009. ETH Zürich + Microsoft Research Cambridge.

### Thesis

Las arquitecturas manycore (≥16 cores, NUMA, heterogeneous ISA, chiplets) se parecen más a **redes distribuidas** que a un "single computer con muchos cores". Shared-memory synchronization primitives (mutex, atomic CAS) ya no escalan más allá de 32–64 cores en algunos workloads.

Barrelfish propone: tratar cada core como si fuera una máquina separada. Un kernel por core (**CPU driver** en jerga Barrelfish) + **monitor** userland que sincroniza estado compartido por **message passing explícito**. No hay shared mutable state en el kernel.

### Estructura

- **CPU driver**: ~10k LOC C por core. Maneja trap handling, scheduling local, page tables locales, local caps. Independientes entre sí (pueden tener ABIs distintas si los cores son heterogéneos — x86 + Xeon Phi + GPU).
- **Monitor**: proceso userland por core. Se comunica con monitors de otros cores via channels (shared memory cross-core, o inter-socket links).
- **System Knowledge Base (SKB)**: Prolog-like engine que mantiene el estado global (qué procesos corren dónde, topología NUMA, cache coherence domains). Queries Prolog deciden placement.
- **Capabilities**: Barrelfish también es cap-based. Distribuye caps entre cores via messages.

### Experimentos + status

48-core AMD Opteron sin problemas en TLB shootdown (vs Linux saturaba). Intel Xeon Phi como "otro core" con ABI distinta. Heterogeneous cores (ARM big.LITTLE, Apple E/P). Proyecto research pure, código público pero poco mantenido tras 2020 (Roscoe → Oracle, Baumann → Azure). Influencia real: el multikernel model se cita constantemente en papers modernos sobre kernels para heterogeneous systems (DPU offload, Smart NICs, CXL memory).

### Para ALZE

- **No** un modelo copy-paste (requiere skill manycore + heterogeneous desde día 1).
- **Sí** un recordatorio arquitectónico: si ALZE quiere soportar manycore bien en v2, la pregunta no es "¿cómo añadimos locks finer-grained?" sino "¿cómo partimos el estado por core desde el principio?". Al menos: per-CPU caches, per-CPU run queues, message-based rebalancing — ya es consenso en Linux post-2015 (ver sched_ext doc R3 `schedulers_modern.md`).

---

## 6. Mach ports — ancestro directo

### Historia

Mach = microkernel de CMU (Rashid + Tevanian, 1985–94), intended replacement de BSD 4.x con compat BSD encima. NeXTSTEP (1989–) adoptó Mach 2.5. NeXT → Apple, 1996. macOS X (2001) → XNU = Mach 3.0 parcial + BSD personality + IOKit.

### Ports = capabilities (aunque no lo llaman así siempre)

- Un **Mach port** es una IPC endpoint identificada por un integer. Nombrespace privado por process (!). Dos procesos pueden tener el mismo port referenciado con integers distintos.
- **Port rights** — cada port en una process table tiene rights:
  - `MACH_PORT_RIGHT_RECEIVE` — el único receive-right por port (1:1). Quien lo tiene es "el servidor".
  - `MACH_PORT_RIGHT_SEND` — can send. Varios procesos pueden tener send-right al mismo port.
  - `MACH_PORT_RIGHT_SEND_ONCE` — single-use. Se consume al enviar. Usado para reply channels.
  - `MACH_PORT_RIGHT_DEAD_NAME` — marcador de port muerto (el receiver desapareció).
  - `MACH_PORT_RIGHT_PORT_SET` — agrupación para multiplexing.
- **`mach_msg`** — envía un mensaje a un port. El mensaje es una estructura con header + body; el body puede contener **port rights** (embedded), **out-of-line memory** (VM copy-on-write para blobs grandes).
- Port rights se *transfieren* via mensaje — reaparecen con un integer nuevo en el destinatario.

### Correspondencia con caps

- Port + send-right ≈ cap con rights={send}.
- Port + send-once ≈ reply-capability seL4.
- Port + receive ≈ ownership del objeto.
- Mach no tiene un sistema general de "rights bitmap" más rico; es binario por operación.

### XNU hoy (macOS 26, iOS 26)

- XNU = Mach core + BSD personality (POSIX syscalls) + IOKit + Sandbox + TrustedBSD MAC.
- **Mach ports** siguen siendo el IPC subyacente. Todo servicio privilegiado (launchd, WindowServer, opendirectoryd, etc) se comunica via Mach ports.
- **XPC** (introducido 10.7 Lion) = framework encima de Mach ports con serialización tipada (`xpc_dictionary_t`, XPC bplist). XPC Services = sandboxed helpers. Parecido filosóficamente a FIDL pero menos formal.
- **launchd bootstrap server** = el "component manager" de facto: registra name→port mappings para servicios del sistema. `bootstrap_look_up(name) → port`. Análogo al CF Component Manager de Fuchsia.

### Lecciones

- Mach demostró que un port-as-cap system puede ser la base de un OS comercial. El hecho de que XNU sea en gran medida "compatibility BSD + IPC Mach" valida la idea.
- Debilidades: mensajes con VM OOL tienen un historial de vulnerabilidades (CVE-2021-30869 etc) — la complejidad del marshalling Mach es una superficie grande. FIDL deliberadamente simplifica.

---

## 7. Capability leak, revocation, confinement

### El problema de revocation

Tres soluciones históricas:

1. **CNode recursion (seL4)**. Caps derivadas forman un "derivation tree". `Revoke(source)` recorre árbol y destruye descendientes. O(n). Fast con pocas derivadas; lento en fan-out masivo. Pro: preciso. Con: requiere kernel almacenar ptrs al derivar.
2. **Indirection + epoch (KeyKOS, Mach dead names)**. Caps apuntan a un proxy revocable. Revoke = invalidar proxy. Cost: indirection extra por IPC. Pro: revocación O(1). Con: overhead permanente.
3. **Sealed caps + unseal key (Hydra, E language)**. Cap opaca; manager con unsealer la abre. Revoke = rotar la key. Pro: cap serializable. Con: gestión de claves.

### Capability leak

Una cap se "leaks" cuando llega a un principal no intended por el creador. Modelos:

- **Transitive leakage** — si A da cap a B, B puede darla a C con `Copy` (si la cap tiene right `DUPLICATE` o `TRANSFER`). Mitigación: no dar ese right (seL4 no tiene `DUPLICATE` como default, requiere `Mint` del owner; Zircon handle debe tener `ZX_RIGHT_DUPLICATE`).
- **Covert channels** — no leak de la cap per se, sino de información sobre la cap via side channels (timing, termination, resource exhaustion). seL4 *information flow* proof mitiga bajo policies específicas.
- **Storage channels** — el propio estado del kernel (contadores, bitmaps) visible indirectamente. seL4 tuvo que formalizar `kernel heap isolation` para probar no-interference.

### Confinement problem

- Butler Lampson, *"A note on the confinement problem"*, CACM 1973. [bwlampson.site](https://www.bwlampson.site/11-Confinement/11-ConfinementAbstract.html).
- Definición: ¿puedes ejecutar un servicio untrusted con datos sensibles y garantizar que no los filtra?
- Lampson lista canales: storage (file, var globales), legitimate (return values), covert (CPU usage, resource contention).
- **Constructor pattern (EROS/KeyKOS)** es la solución práctica en caps: el constructor del proceso controla exactamente las caps iniciales; si el proceso no tiene cap a "network" ni a "disk outside sandbox", entonces los únicos canales de leak son covert. Shapiro probó formalmente que el EROS constructor garantiza esto.
- **seL4 domain-based info flow** — extensión moderna. Un "domain" es un conjunto de objetos; cap-based policies + sched config garantizan no-interference entre dominios.

### Distributed revocation (KeyKOS-style)

- Caps con "segment number" + epoch. Al revocar, se bump epoch del segment. Futuras operaciones se chequean contra epoch; stale → fail.
- Funciona across crashes (persistence) y across hosts distribuidos (con reloj lógico).
- seL4 no hace esto; dentro de un kernel es overkill. Pero sí útil si ALZE quisiera federar caps across machines (v4).

---

## 8. IDL + RPC stub generation — panorama

### Por qué IDL

Caps bajas solo dan "un objeto, unas ops". Los sistemas reales tienen interfaces con métodos tipados, structs, versionado. IDL cubre el gap: type safety cross-language, stubs auto-generados (dev escribe interfaz una vez, compilador emite cliente+servidor), wire format determinista, zero-copy posible.

### Comparativa

| IDL | Sistema | Wire format | Lenguajes target | Nota |
|---|---|---|---|---|
| **CAmkES ADL + IDL4** | seL4 | custom C structs | C, C++ | integra con CapDL |
| **FIDL** | Fuchsia | bytes + OOL + handles, determinista | Rust, C++, Dart, Go, Java | version-compat via tables |
| **Cap'n Proto** | — (general) | infoset mmapable, zero-copy lectura | C++, Rust, Go, many | Kenton Varda (post-protobuf). [capnproto.org](https://capnproto.org/) |
| **COM IDL / MIDL** | Windows | NDR (Network Data Representation) | C/C++, .NET | ancestro de DCOM |
| **protobuf + gRPC** | (userland dist systems) | varint + tags | muchos | no specific OS cap support, pero widely used |
| **AIDL** | Android Binder | parcel-based | Java, C++, Rust (recent) | Android Binder IPC (cap-ish) |
| **D-Bus XML introspection** | Linux (ad-hoc) | signature strings | C, Python, etc | weak typing |

### FIDL en detalle

Tables (extensible), unions (strict/flexible), bits (flag enums), resource types (portan handles), handle types con rights mínimas requeridas. `protocol` = colección de methods: one-way, two-way (request/response), event (servidor→cliente unsolicited). Tools: `fidlc` compile, `fidlgen_rust/cpp/dart/go`. ABI-safe changes documentados (añadir campo a table, strict→flexible enum).

### Para ALZE

v1: nada de IDL. v2: **Cap'n Proto** — maduro, zero-copy read, múltiples lenguajes, no atado a ningún OS. FIDL: atado a Fuchsia, caro extraer. Custom Rust-proc-macro: tentador pero trabajo. Empezar con Cap'n Proto.

---

## 9. ACL vs capabilities — el debate estructural

### El "confused deputy" reloaded

Ejemplo moderno: un navegador descarga un archivo y se lo pide a una extensión para procesarlo. La extensión tiene permiso de escribir en `~/Documents` (otorgado por el usuario en install). El archivo descargado *pide* ser guardado en `~/Documents/.bashrc`. ACL: sí, la extensión tiene permiso. Cap: la extensión sólo tiene cap al directorio específico `~/Documents/downloads/<this-file>`; no puede escribir `.bashrc`.

El patrón **POLA** (Principle of Least Authority), Lampson 1974, Saltzer+Schroeder 1975, es formalizable en caps y sólo heurístico en ACL.

### Ambient authority

- **ACL/Unix**: `open("/etc/passwd")` es legal; el kernel hace lookup en el ACL de `/etc/passwd`, ve si el caller tiene perms. El caller *no necesita demostrar designation explícita*; el namespace es global y la autoridad es "ambient" (heredada del UID).
- **Capability**: no hay path global. El caller sólo puede `open` en una cap que ya tiene. Si no la tiene, no puede nombrar el archivo. Designation y authority son la misma cosa: la cap.

### Delegación

- **ACL**: delegar "leer este archivo a Bob" exige mutar el ACL del archivo. Si el archivo no tiene ACL per-user (ej. Unix mode bits), no puedes. Delegación temporal o condicional es aún peor.
- **Capability**: `Mint(cap, rights=READ_ONLY, badge=bob_id) → bob_cap`. Se la envías a Bob. Bob puede ejercerla y, si le das derecho a copiar, delegar a sí mismo, a otros. Revoke es un problema aparte (ver §7).

### Por qué Unix sobrevive

- Herencia masiva de software. Todo POSIX asume ambient authority.
- Simplicidad mental para usuarios finales (mode bits en `ls -l`).
- Capsicum (FreeBSD) + OpenBSD pledge/unveil son "retrofit caps" — no rompen Unix, sólo añaden atenuaciones locales.

### Lección para ALZE

Un sistema híbrido donde el **kernel es cap-puro** pero hay una **userland POSIX personality** encima (como XNU tiene BSD encima de Mach) es la ruta conservadora. ALZE v3 podría ofrecer ambos: nativo cap-based, y un pTHREAD layer opcional para portes.

---

## 10. Aplicación moderna — caps fuera del kernel

### WebAssembly + WASI preview 2 (2024–)

WASI preview 2 define un **capability-based API surface** para WebAssembly components. No hay "abrir por path" globalmente; un component recibe "preopens" (caps a directorios, sockets) por el host al instanciarlo. Interface Types (Component Model) ≈ FIDL: typed interfaces, resources, handles. Runtimes (wasmtime de Bytecode Alliance) lo implementan. Probablemente el deploy más masivo de caps en la industria hoy (serverless, edge compute).

### Object-capability (OCap) — JavaScript + browsers

Mark S. Miller, *"Robust Composition"*, PhD 2006. Agoric (ex-Google) construye **Hardened JavaScript** (SES) y promise-pipelining (E language) con caps como referencias a objetos. Pattern browser: iframes `sandbox`, postMessage con MessagePort (cap-ish), Service Workers con fetch handler scoped.

### Pony language

Sylvan Clebsch, PhD Imperial College 2017. Lenguaje actor-based con **reference capabilities** a nivel tipo: `iso` (unique), `val` (immutable), `ref` (mut single-thread), `box` (readonly shared), `trn` (transition), `tag` (identity). Compilador asegura data-race freedom. Paralelo OS-caps: "qué puede hacer esta referencia" está en el tipo, no en runtime check.

### Rust types as capabilities

Pattern: tipo con constructor restringido actúa como cap. `&mut T` = escritura exclusiva; `Arc<T>` = cap compartida; `Token` privados como "prueba". Compile-time caps — sin runtime overhead, pero no revocables dinámicamente. Pattern "newtype + private constructor" omnipresente. Para ALZE: kernel en Rust podría usar types-as-caps a nivel interno (compile-time) y `Capability` struct runtime para userland.

### Android Binder

Handles (integer) con reference counting, transferibles entre procesos via Binder driver. No OCAP puro (tiene UID checks), pero handles transferibles son cap-like. Probablemente el segundo deploy masivo cap-ish después de AS/400.

---

## Tabla comparativa — capability kernels

| Kernel | Verification | IPC model | Memory model | Status 2026 | Deployment signature |
|---|---|---|---|---|---|
| **seL4** | Formal end-to-end (Isabelle/HOL, refinement + integrity + info-flow) | Sync rendezvous endpoints; fastpath via registers; notifications async; reply caps | Untyped-retype, no kernel allocator, user-driven | Active (seL4 Foundation, 2020–) | Avionics (Rockwell Collins), automotive (Cog Systems), defense; HENSOLDT Cyber products |
| **Zircon (Fuchsia)** | None formal (C++ ~150k LOC) | Channels (bytes + handle transfer), sockets, FIFOs, ports (wait multiplex) | Kernel `kmalloc`-like; VMO user-visible | Active but slower (Google layoffs 2022–23) | Nest Hub, Nest Hub Max consumer devices |
| **KeyKOS / CapROS** | None (pre-dates formal methods in this field) | Sync capability invocation; gates as IPC point | Single-level store, ortho persistence, snapshots every ~30s | KeyKOS extinct (commercial); CapROS dormant ~2012 | Tymshare timesharing (1980s), some military systems |
| **EROS** | Confinement proof only (Shapiro+Weber 2000) | seL4-like; fastpath assembler | Space bank (explicit resource allocator obj) | Dead since ~2005; research ended | — (research only) |
| **Barrelfish** | None | Message passing between CPU drivers via monitors | Per-core address spaces; distributed CapDL | Dormant since ~2020 | — (research prototype, ETH/MSR) |
| **Mach 3.0 pure** | None | Ports with send/receive/send-once rights | Vanilla VM + OOL VM regions | Extinct as standalone | — |
| **XNU (macOS/iOS)** | None | Mach ports underneath; XPC tipado encima; BSD syscalls paralelos | Mach VM + UBC (unified buffer cache) | Active, huge deploy | All Apple computers + devices; billions of units |
| **HelenOS** | None | IPC over async calls, capability-ish | kmalloc-like | Active hobby (~50 contributors) | — (research/education) |
| **Genode** | Partial (depends on underlying kernel, has proof effort for some components) | Sessions + RPC via session caps | Resource quotas hierarchical | Active, commercial (Genode Labs) | Niche (secure workstations, Sculpt OS) |

---

## ALZE applicability — niveles concretos

### Estado actual (v1, hoy)

ALZE (`/root/repos/alze-os`, review R2) tiene:
- IDT incompleta (P0 blocker Sprint 1).
- SMP asunciones no verificadas.
- FS sin locks.
- Kernel C99+asm monolítico, limine bootloader.
- No hay concepto de capability; autorización implícita tipo UID=0.

**Recomendación v1**: **no** intentar capability model aún. Primero cerrar P0 blockers (IDT full, locking coherente, SMP boot correcto). Todo intento de rewrite cap-based antes de tener un kernel no-panic en SMP es premature optimization de arquitectura sobre corrección.

### v2 — migración intencionada (post-P0, target ~2026-Q4)

Adoptar **patrones** seL4/Zircon sin rewrite completo:

1. **Handle table por proceso**. Sustituir "integer fd globales" o "punteros a kernel object" en syscalls por `handle_t` (32-bit). Cada proceso tiene array de slots. Syscall entry resuelve handle → kernel object.
2. **Rights bitmask por handle**. Cada handle lleva 16–32 bits de rights. Syscall chequea bits requeridos. `handle_duplicate(h, rights_sub, out)` con enforcement.
3. **Unforgeable**. Handle integers del proceso no son validable desde otro proceso. Transferir handles via message (no "pasar el integer"). Kernel hace el re-binding.
4. **Typed kernel objects**. En C: struct `kobj` con tag enum; `kobj_thread`, `kobj_page`, `kobj_endpoint`. Handle apunta a `kobj*` con tag check en cada op.
5. **IPC sincrono sobre endpoints** (CNode-lite). No todavía el árbol completo CSpace; basta un handle table flat por proceso. Rendezvous: sender bloquea hasta receiver; fastpath si ambos listos y mensaje cabe en registros (N=6 en x86_64 SYSV ABI).
6. **Cap<Mint/Copy/Move/Revoke>-lite**. Sin árbol de derivación completo; al menos `duplicate_with_less_rights`, `close(handle)` (local revoke), `send(handle, bytes)` transfiere.

**No** v2:
- Untyped + Retype (caro, complica allocator).
- Single-level store / persistence.
- Componente manifest + Component Manager (overkill pre-userland serio).
- Formal verification.

Reference: Zircon handles son exactamente este nivel (handle + rights, sin árbol de CNodes). ALZE v2 = "Zircon-lite in C99".

### v3 — aspirational (long-term, years out)

Si ALZE prospera y tiene más de un dev:
- **CNode tree** completo estilo seL4.
- **Untyped memory model**: el kernel deja de hacer allocator, el userland decide. Verificación-friendly.
- **IDL minimalista**: elegir Cap'n Proto o definir `alze-idl` procmacro en Rust (asumiendo rewrite parcial a Rust).
- **Component manifest** — cada servicio declara offer/use; un init minimalista distribuye caps.
- **Formal verification** de al menos el IPC fastpath + scheduler. Lean o Isabelle/HOL.

### v4 — nunca

Verificación formal del kernel entero estilo seL4. **No para un hobbyist OS solo-dev**. Ver nota final.

### Plan concreto de migración v1→v2

1. Definir `handle_t`, `kobj_t`, `rights_t`. Sustituir `fd` actuales en syscalls.
2. Handle table + `handle_duplicate`, `handle_close`.
3. Primer `kobj` — `kobj_page`. Reemplaza "page allocator returns ptr" por "returns handle to page kobj".
4. `endpoint` kobj + `endpoint_call/recv`. Reemplazar un syscall simple (ej. `write`).
5. Propagar progresivamente. Feature flag `-DALZE_CAPS` para revertir por paso.
6. Tests: unit de handle table + integration "transferir handle via IPC invalida el original".

Esperable: ~2k LOC netas, ~1 mes dev dedicado.

---

## Nota honesta final

**Formal verification (seL4 estilo) = 2–3 engineer-years por kLOC de kernel C.**

Heiser 2022: ~25 py iniciales ARM para 10k C + 1.5k Haskell spec + 4k abstract spec; +5 py por port adicional. Un nuevo kernel con esa metodología: 20–30 py para 10k LOC. Expertos Isabelle/HOL: decenas globalmente en 2026.

**Conclusión para ALZE**:

- Un solo-dev con 1–2 h/día *no puede* verificar un kernel formalmente. Es trabajo full-time de equipo de ~5 personas durante años. Cost/benefit negativo para hobbyist OS.
- Un solo-dev *sí puede* adoptar **patrones capability**: handles unforgeable, rights bitmask, IPC endpoints, mental model "authority-from-having-the-cap". Esto da el valor estructural (ambient authority desaparece, confused deputy prevenido, delegación/revocación diseñadas) **sin** la prueba.
- Zircon, Android Binder y XNU demuestran que cap-based sin verificación formal sigue siendo mejor que ACL-Unix en security, composability, testability.
- La verificación formal es un lujo. Los patrones de caps son una decisión de diseño. ALZE debe perseguir los patrones; no aspire a la verificación total.

**Path recomendado**: v1 arregla blockers R2. v2 Zircon-lite en C99. v3 seL4-lite si llega multi-dev + rewrite Rust. v4 verification: nunca, excepto proyecto académico paralelo.

---

## Referencias primarias

**Fundamentos** — Dennis + Van Horn, *"Programming Semantics for Multiprogrammed Computations"*, CACM 9(3), March 1966 [ACM](https://dl.acm.org/doi/10.1145/365230.365252). Lampson, *"A note on the confinement problem"*, CACM 16(10), Oct 1973 [bwlampson.site](https://www.bwlampson.site/11-Confinement/11-ConfinementAbstract.html). Wulf+Levin+Harbison, *"HYDRA: The Kernel of a Multiprocessor Operating System"*, CACM 17(6), June 1974 [ACM](https://dl.acm.org/doi/10.1145/355616.364017). Saltzer+Schroeder, *"The Protection of Information in Computer Systems"*, Proc. IEEE 63(9), Sept 1975 [cs.virginia.edu](https://www.cs.virginia.edu/~evans/cs551/saltzer/). Hardy, *"The Confused Deputy"*, ACM OSR 22(4), Oct 1988 [capsec.org](https://capsec.org/archives/Hardy-confused-deputy.pdf).

**KeyKOS/EROS/CapROS** — Hardy, *"KeyKOS Architecture"*, ACM OSR 19(4), Oct 1985 [cap-lore.com](http://www.cap-lore.com/CapTheory/KK/). Shapiro+Smith+Farber, *"EROS: A Fast Capability System"*, SOSP 1999 [sigops](https://sigops.org/s/conferences/sosp/1999/papers/p170-shapiro.pdf). Shapiro+Weber, *"Verifying the EROS Confinement Mechanism"*, IEEE S&P 2000 [cis.upenn.edu](https://www.cis.upenn.edu/~shap/EROS/confinement.pdf). CapROS [capros.org](http://www.capros.org/).

**seL4** — Klein+Elphinstone+Heiser et al, *"seL4: Formal Verification of an OS Kernel"*, SOSP 2009 [sigops](https://sigops.org/s/conferences/sosp/2009/papers/klein-sosp09.pdf). Klein et al, *"Comprehensive Formal Verification of an OS Microkernel"*, TOCS 32(1), Feb 2014 [sel4.systems](https://sel4.systems/Research/pdfs/comprehensive-formal-verification-os-microkernel.pdf). Heiser, seL4 whitepaper 2020 [sel4.systems](https://sel4.systems/About/seL4-whitepaper.pdf). seL4 Manual [pdf](https://sel4.systems/Info/Docs/seL4-manual-latest.pdf). CAmkES [docs](https://docs.sel4.systems/projects/camkes/). Source [github.com/seL4/seL4](https://github.com/seL4/seL4).

**Zircon/Fuchsia** — Zircon concepts [fuchsia.dev](https://fuchsia.dev/fuchsia-src/concepts/kernel). Handles [fuchsia.dev/handles](https://fuchsia.dev/fuchsia-src/concepts/kernel/handles). FIDL spec [fuchsia.dev/fidl](https://fuchsia.dev/fuchsia-src/reference/fidl/language/language). Components v2 [fuchsia.dev/components](https://fuchsia.dev/fuchsia-src/concepts/components/v2/introduction).

**Barrelfish** — Baumann+Barham+Dagand+Harris+Isaacs+Peter+Roscoe+Schüpbach+Singhania, *"The Multikernel: A New OS Architecture for Scalable Multicore Systems"*, SOSP 2009 [sigops](https://sigops.org/s/conferences/sosp/2009/papers/baumann-sosp09.pdf). Project [barrelfish.org](http://www.barrelfish.org/).

**Mach/XNU** — Rashid+Tevanian+Young et al, ASPLOS 1987 [dl.acm.org](https://dl.acm.org/doi/10.1145/36206.36181). XNU source [github.com/apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu).

**Modern** — Miller, *"Robust Composition"*, PhD 2006 [erights.org](http://www.erights.org/talks/thesis/). WASI p2 [wasi.dev](https://wasi.dev/). Pony [ponylang.io](https://www.ponylang.io/). Cap'n Proto [capnproto.org](https://capnproto.org/). Watson+Anderson+Laurie+Kennaway, *"Capsicum"*, USENIX Security 2010 [papers.freebsd.org](https://papers.freebsd.org/2010/rwatson-capsicum.files/rwatson-capsicum-paper.pdf).

**Meta** — Levy, *"Capability-Based Computer Systems"*, Digital Press 1984 [homes.cs.washington.edu](https://homes.cs.washington.edu/~levy/capabook/). Miller+Yee+Shapiro, *"Capability Myths Demolished"* 2003 [srl.cs.jhu.edu](http://srl.cs.jhu.edu/pubs/SRL2003-02.pdf).
