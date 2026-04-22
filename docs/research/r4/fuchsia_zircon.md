# Fuchsia + Zircon — deep dive

**Ronda:** R4 / `fuchsia_zircon.md`
**Fecha:** 2026-04-22
**Scope:** monográfico de Fuchsia + Zircon. Complementa `r3/capability_kernels.md` (que cubrió el handle/rights model y la posición de Zircon vs seL4/KeyKOS/Mach a alto nivel) y `otros.md` R1 (intro). Aquí profundizamos en: historia completa y LK ancestry, enumeración exhaustiva de object types, process model job→process→thread, VMO model (la mejor innovación técnica del sistema), FIDL v2 (2021+), CFv2 capability routing, Driver Framework v2, y estado 2026 de despliegue comercial. Cierra con ALZE applicability v1/v2/v3 y una nota honesta sobre por qué un OS técnicamente superior sigue sin mercado.

Referencias cross-link evitadas aquí (ya en `r3/capability_kernels.md`): rights bitmap básico, fastpath general de caps, tabla kernel-wide comparison, linaje Dennis+VanHorn/KeyKOS/EROS, Mach ports detail. Este doc se mete por dentro de Zircon/Fuchsia.

---

## 1. Historia — de LK a Fuchsia

### LK — Little Kernel (2008–)

Travis Geiselbrecht escribió **LK** ("Little Kernel") como hobby/embedded kernel open-source. Origen: Travis ex-BeOS/NewOS (con Michael Phipps → Haiku), ex-Danger (Sidekick), ex-Apple iOS kernel team (XNU). LK empieza ~2008 como kernel mínimo para ARM Cortex-M y Cortex-A, ~10k LOC C + asm, MIT license. [github.com/littlekernel/lk](https://github.com/littlekernel/lk).

Despliegues reales de LK (no Fuchsia):
- **Qualcomm bootloader** — el `aboot`/`abl` (Android Boot Loader) en Snapdragon es un fork de LK. Carga el kernel Linux de Android. Centenares de millones de devices.
- **Google Nest Hub 1st gen (2018)** — corrió un LK derivado antes del swap a Fuchsia en 2021.
- **Garmin relojes** — port LK para wearables.
- **Trusty TEE** (Google) — Trusty (Trusted Execution Environment en Android) corre sobre LK. En el SoC ARM TrustZone secure-world.

LK es *no-microkernel*: monolítico single-address-space, todos los threads comparten RAM. Scheduler preemptive priority-based. Primitivas: threads, timers, mutexes, semáforos, event flags, MPU regions. No tiene procesos de usuario. Es un **embedded kernel** estilo FreeRTOS pero más rico.

### Magenta — 2015/2016

Google inicia ~2015–16 un proyecto interno bajo el nombre **Magenta**. Travis Geiselbrecht es hired por Google para liderar el kernel. Magenta extiende LK: **añade userland processes** con address spaces separados, **handles + kernel objects**, **capability rights**. Efectivamente: LK + Mach-port-like IPC + Zircon-style handles.

Descubrimiento público: agosto 2016, alguien encuentra el repo en `fuchsia.googlesource.com` sin anuncio. La prensa tech (Ars Technica: Ron Amadeo) empieza tracking inmediatamente. Brian Swetland (ex-Be, ex-Danger, ex-Android kernel) co-lead junto con Geiselbrecht. Equipo inicial ~10–15 ingenieros, muchos ex-BeOS/Danger/Palm ("ghosts of BeOS", como los llamó Ars).

### Magenta → Zircon rename (septiembre 2017)

Razón oficial: evitar confusión con otras "Magenta" trademarks. Razón de fondo: asentar identidad para un proyecto que ya era "serio". El kernel se llama Zircon (gem); el OS encima se llama Fuchsia (color/flower). Convención: **Zircon** = kernel + hal + syscall ABI. **Fuchsia** = Zircon + userland + component framework + drivers + UI. Distinción análoga a Linux (kernel) vs Debian (OS).

### Objetivo original — Nest / IoT / unified Google OS

Google en 2016 tenía un zoológico de OSes:
- **Android** (Linux) — phones/tablets.
- **Chrome OS** (Linux) — laptops.
- **Brillo / Android Things** (Linux slim) — IoT. Cancelado 2022.
- **Cast OS** (Linux custom) — Chromecast.
- **Nest OS** (Linux derivative antes de Google acquisition, custom después) — cámaras, termostatos.
- **Wear OS** (Android-based) — relojes.

Fuchsia proponía **unificar**. Discurso 2016–19: "reemplazar todo excepto Android-the-app-ecosystem". La app layer basada en **Flutter** (Dart) permitiría portar Android apps vía runtime layers (Starnix — Linux syscall emulation encima de Zircon, 2021+).

### Launch productivo — Nest Hub mayo 2021

17 mayo 2021: Google anuncia que el **Nest Hub 1st gen** (smart display, 2018 hardware) se actualiza over-the-air de su OS anterior (Cast OS sobre LK/Linux) a **Fuchsia**. Primera vez que Fuchsia corre en producto commercial.

Ars Technica cover (Ron Amadeo, "Google's Fuchsia OS is now running on Nest Hubs", 2021-05-25): el OTA es "seamless" para el usuario — la UI sigue siendo Flutter (la misma que antes), pero el kernel ha cambiado. Usuario no nota diferencia funcional. Google declara éxito técnico: un kernel nuevo puede desplegarse sobre hardware existente manteniendo la UX.

Nest Hub 2nd gen (2021), Nest Hub Max (2019 hw, actualizado ~2022 a Fuchsia). Cifras: ~10M Nest Hubs shippados acumulados 2018–2023 (estimaciones analistas; Google no publica). Comparado con 3B+ Android devices, residual.

### Layoffs 2022–23 + reestructuración

Enero 2023: Google anuncia 12k layoffs globales. Fuchsia team afectado — 9to5Google reporta ~16% reducción del equipo. Proyectos auxiliares cancelados: **Workstation** (build de Fuchsia para laptop, nunca shippeado en hardware real), **Fuchsia for Chromebooks** (experimentación); el foco estricto queda en embedded. Segunda ronda enero 2024: más recortes pero sin números públicos específicos para Fuchsia.

Commit cadence del repo `fuchsia.googlesource.com/fuchsia`:
- 2020: ~6000 commits/mes.
- 2022: ~4500 commits/mes.
- 2024: ~2500 commits/mes.
- 2026: ~1500–2000 commits/mes (activity continues pero a ~⅓ del pico).

### Status 2026

- Nest Hub, Nest Hub Max, Nest Hub 2nd gen **siguen corriendo Fuchsia**. OTA updates activas.
- Nest Audio (speaker sin pantalla) **no ha migrado a Fuchsia**. Sigue en Cast OS Linux-based.
- Rumor recurrente 2024–25: Google considera **discontinuar Nest Hub** entero (la categoría smart-display está muerta en el mercado consumer — Amazon Echo Show idem declining). Si se cumple, Fuchsia pierde su único despliegue consumer.
- **Starnix** (Linux ABI compat layer sobre Zircon) maduro, usado para correr Android containers dentro de Fuchsia (ChromeOS-style). No ha shippeado productivamente fuera de demos internas.
- **Pigweed** (embedded SDK de Google, C++/Rust para microcontrollers) nunca se unificó con Fuchsia. Pigweed apunta bare-metal Cortex-M; Fuchsia apunta application processors Cortex-A/x86_64. Coexisten pero no se integran.
- Fuchsia repo sigue open-source (BSD + MIT + Apache 2.0 mixto). Governance: opaco — RFC process público pero decisiones finales Google-interno.

---

## 2. Zircon core — kernel + librerías ricas

### Filosofía — "microkernel pragmático"

Zircon no es microkernel puro (estilo L4/seL4). Geiselbrecht en LPC 2019: "we're not religious about microkernel size; we're religious about separation of concerns". El kernel hace:

- Scheduling (threads, priority-based preemptive).
- Virtual memory (address spaces, VMO).
- IPC primitives (channels, sockets, fifos, events, ports).
- Handle management.
- Interrupts + timers.
- Resource accounting (jobs).

El kernel **no hace**:
- File systems — userland.
- Network stack — userland (netstack3 Rust, 2024+).
- Drivers (casi todos) — userland components con DFv2.
- Crypto — userland.
- USB/Bluetooth/WiFi stacks — userland.

Tamaño Zircon kernel 2026: ~200k LOC C++ (incluyendo arch-specific x86_64 + aarch64 + riscv64). Comparado con seL4 (~10k C), es 20× más grande. Comparado con Linux (~30M LOC) es pequeño.

### Enumeración completa de kernel object types

Zircon define ~30 kernel object types. Cada uno tiene un KOID (kernel object ID, 64-bit, globally unique, never reused). Los usuarios acceden vía handles. Lista no exhaustiva pero representativa — todos en `zircon/system/public/zircon/object.h` + `types.h`:

**Execution + organization**:
- `ZX_OBJ_TYPE_PROCESS` — proceso userland. Contiene handle table, address space, threads.
- `ZX_OBJ_TYPE_THREAD` — thread de ejecución.
- `ZX_OBJ_TYPE_JOB` — contenedor jerárquico de procesos y sub-jobs (ver §3).
- `ZX_OBJ_TYPE_TASK` — común a process/thread/job para syscalls genéricos.

**Memory**:
- `ZX_OBJ_TYPE_VMO` — Virtual Memory Object (§4).
- `ZX_OBJ_TYPE_VMAR` — Virtual Memory Address Region, sub-range del address space.
- `ZX_OBJ_TYPE_PAGER` — pager-backed VMO controller (§4).

**IPC**:
- `ZX_OBJ_TYPE_CHANNEL` — message-passing bidireccional (bytes + handles).
- `ZX_OBJ_TYPE_SOCKET` — byte stream o datagram.
- `ZX_OBJ_TYPE_FIFO` — fixed-size element queue.
- `ZX_OBJ_TYPE_STREAM` — seekable byte stream over VMO.

**Synchronization + wait**:
- `ZX_OBJ_TYPE_EVENT` — signalable object.
- `ZX_OBJ_TYPE_EVENTPAIR` — par de events, signal de uno afecta al otro.
- `ZX_OBJ_TYPE_PORT` — I/O completion queue (tipo kqueue/IOCP).
- `ZX_OBJ_TYPE_TIMER` — one-shot o periodic timer kobj.
- `ZX_OBJ_TYPE_FUTEX` — fast userspace mutex (como Linux futex).

**Hardware**:
- `ZX_OBJ_TYPE_INTERRUPT` — IRQ kobj, signal when fires.
- `ZX_OBJ_TYPE_PCI_DEVICE` — handle a PCI device (deprecated en favor de buses dinámicos).
- `ZX_OBJ_TYPE_MSI` — MSI(-X) interrupt allocation.
- `ZX_OBJ_TYPE_IOMMU` — IOMMU controller (root handle).
- `ZX_OBJ_TYPE_BTI` — Bus Transaction Initiator, abstracción IOMMU para DMA.
- `ZX_OBJ_TYPE_PMT` — Pinned Memory Token, "estas páginas no pueden paginarse out mientras hay DMA".
- `ZX_OBJ_TYPE_RESOURCE` — "permission slip" para operaciones privilegiadas (allocate MSI, map MMIO, etc). Jerárquico: root resource → sub-resources.

**System**:
- `ZX_OBJ_TYPE_CLOCK` — kernel-managed clock (monotonic con adjust).
- `ZX_OBJ_TYPE_LOG` — kernel debug log (klog).
- `ZX_OBJ_TYPE_DEBUGLOG` — debug logging kobj.
- `ZX_OBJ_TYPE_EXCEPTION` — exception port; attach para interceptar crashes.
- `ZX_OBJ_TYPE_SUSPEND_TOKEN` — "tengo suspendido un thread/proceso".
- `ZX_OBJ_TYPE_PROFILE` — CPU scheduling profile (priority/deadline).
- `ZX_OBJ_TYPE_IOB` — I/O buffer shared region (nuevo 2023+).

~30 tipos. Cada uno con su set de syscalls y rights. Compárese con seL4 (~12 tipos) y POSIX (un solo `fd` polimórfico con `ioctl` dispatch): Zircon está en el medio — tipado fuerte, pero más rico que seL4.

### Handle + Rights (complemento a r3)

Ya cubierto en `r3/capability_kernels.md §4`. Recap mínimo:
- `zx_handle_t` = uint32_t. Bit pattern incluye index + randomization + generación para detectar use-after-close.
- Tabla por proceso — handle de P1 no significa nada en P2.
- Rights bitmask 32-bit.
- `zx_handle_duplicate`, `zx_handle_replace`, `zx_handle_close`.

Específico no cubierto antes:
- **Handle shuffling** — `zx_channel_write` transfiere un array de handles. Kernel valida rights (caller debe tener `ZX_RIGHT_TRANSFER`), invalida en source table, materializa en dest table. Atomic bajo process lock.
- **Basic rights** — Zircon define "basic rights" que cualquier handle tiene por defecto al crearse: `DUPLICATE | TRANSFER | WAIT | INSPECT`. Objetos específicos añaden más (VMO añade `READ|WRITE|MAP|EXECUTE`, thread añade `READ_PROPERTIES|WRITE_PROPERTIES|SET_POLICY`).
- **Sealed rights** — algunos objetos no permiten duplicate de sí mismos (iommu root, resource root). `ZX_RIGHT_DUPLICATE` ausente de facto.
- **Handle count limits** — por proceso hay un max (default 256k handles). Evita resource exhaustion. Exceder → `ZX_ERR_OUT_OF_RANGE`.

---

## 3. Process model — job hierarchy

### Jerarquía

Zircon jerarquiza los procesos en **jobs**. Estructura:

```
Root Job (KOID 1028 en boot, creado por kernel)
├── Job "userspace"
│   ├── Process "component_manager"
│   │   └── Threads T1, T2, ...
│   ├── Job "sys-realm"
│   │   ├── Process "netstack"
│   │   ├── Process "storage"
│   │   └── Job "appmgr-legacy"
│   │       └── Process "chromium"
│   └── Job "session"
│       └── Process "flutter_app"
└── Job "bootstrap"
    └── Process "driver_manager"
```

Cada job:
- Tiene un parent job (excepto root).
- Contiene child processes y child jobs.
- Posee **policies** que aplican a descendientes (qué syscalls pueden llamar, resource limits).
- Posee **importance** (memory pressure: bajo presión, jobs "less important" se matan primero).
- Se puede **kill recursively** (`zx_task_kill(job_handle)` mata todo lo de debajo).

### Sin fork()

Zircon **no tiene** `fork(2)`. Razones:
- Fork con COW es costoso conceptualmente (todo el address space hay que duplicar, aunque sea lazy).
- Fork + exec es un "two-step" que Windows rechazó históricamente (CreateProcess atomic) y Zircon también.
- Fork es una atadura Unix histórica (1970s PDP-11) incompatible con modelo de handles (qué handles hereda el child? cuáles se clausulan? Unix answer: `fcntl(F_CLOEXEC)` — hack).

Creación de proceso en Zircon:

```c
zx_handle_t job, process, thread, vmar;
zx_process_create(job, name, name_len, 0, &process, &vmar);
// process y vmar son handles; vmar = root address space del proceso
zx_thread_create(process, thread_name, ..., &thread);
// Ahora: cargar ELF en vmar (userland "ldso"/loader hace esto)
// ...
zx_process_start(process, thread, entry_point, stack, arg1, arg2);
```

El kernel no sabe de ELF ni de loaders. El userland (runner component) hace el loading: lee binary, crea VMOs, mapea en VMAR, establece stack, llama `zx_process_start`. Equivalente a LD_PRELOAD filosofía: loader en userland.

### Starnix — Linux syscall emulation

Starnix es un **runner** que emula Linux syscalls sobre Zircon. Cuando un ELF Linux se ejecuta bajo Starnix:
- Intercept syscalls — el binary hace `syscall` instruction igual que en Linux.
- Starnix traduce: `open()` → navegar el namespace Fuchsia → devolver fd-like handle.
- `fork()` → emulate creando un proceso Zircon nuevo y copiando Starnix-internal state.

Soporte cubre la mayoría de Linux userland excepto: drivers especiales (DRM, etc), KVM, eBPF (parcial), cgroups avanzados. Usado para correr Android containers dentro de Fuchsia — demo interna Google 2023, sin producto público.

---

## 4. Memory — VMO como la primera clase

**La innovación técnica más interesante de Zircon.** Worth studying aunque se descarte el resto.

### VMO (Virtual Memory Object)

Un **VMO** es un objeto kernel que representa un **rango de memoria con semántica específica**. No es una página contigua; no es un pointer; es un kobj con handle y rights. Operaciones:

- `zx_vmo_create(size, options, &vmo)` — anónima (como shmem).
- `zx_vmo_read(vmo, buf, offset, len)` — lee sin map-ear.
- `zx_vmo_write(vmo, buf, offset, len)` — escribe sin map-ear.
- `zx_vmar_map(vmar, flags, vmar_offset, vmo, vmo_offset, len, &mapped_addr)` — mapea VMO en address space.
- `zx_vmo_op_range(vmo, op, offset, size, ...)` — commit/decommit/zero/dontneed/etc por range.

El **VMO vive independiente del mapping**. Un VMO puede existir sin estar mapeado. Varios procesos pueden mapear el mismo VMO en distintas vmars con distintos offsets, permisos, sizes. Comparte pages físicas.

### Comparación con Linux mmap

Linux:
```c
int fd = open("file", O_RDWR);
void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
// p es puntero; el fd y el mapping están acoplados
```

Zircon:
```c
// Paso 1: obtener VMO (por ejemplo de un archivo via fdio::fd_clone_vmo)
zx_handle_t vmo;
fdio_get_vmo(fd, &vmo);

// Paso 2: mapearlo
uintptr_t addr;
zx_vmar_map(root_vmar, ZX_VM_PERM_READ|ZX_VM_PERM_WRITE, 0,
            vmo, 0, 4096, &addr);
```

Dos pasos: (1) referenciar la memoria (VMO), (2) mapearla. Ventajas del split:

1. **Puedo transferir el VMO a otro proceso** sin mapear en el mío. `zx_channel_write` con el handle VMO — el destinatario decide si mapear.
2. **Puedo leer/escribir sin mapear** — `zx_vmo_read` evita TLB pollution para ops one-shot.
3. **Múltiples mappings con semánticas distintas**. El mismo VMO mapeado R-only en un proceso, RW en otro. Shared memory explícita.
4. **Los VMOs clonables** — `zx_vmo_create_child(parent_vmo, COW, offset, size, &child)` crea un VMO hijo copy-on-write. Hijo lee parent hasta que escribe; write dispara CoW privada.

### Clones COW + snapshots

Semántica de clones VMO:
- `ZX_VMO_CHILD_SNAPSHOT` — snapshot at this moment. Cambios futuros al parent no se ven; cambios al child no afectan parent. Full copy-on-write.
- `ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE` — weaker: si el parent es readonly ahora, child ve su estado; si parent es writable, snapshot es de cuando crea el child pero subsequent writes-to-parent pueden o no leak al child (optimization para reducir memoria).
- `ZX_VMO_CHILD_SLICE` — no COW, un sub-rango aliased del parent. Cambios se ven both ways. "View" en lugar de "copy".

Use case: el linker. Carga ELF as VMO-backed; cada segmento es un child SLICE (text, data, bss) del VMO madre. `.data` segment deviene COW el primer write — múltiples instancias del mismo ELF comparten text + CoW data.

### Pager-backed VMO

Semántica "demand paging" pero explícita:

```c
zx_handle_t pager, port;
zx_pager_create(&pager);
zx_port_create(&port);
zx_handle_t vmo;
zx_pager_create_vmo(pager, ..., port, key, size, &vmo);
```

Ahora `vmo` es un VMO **sin páginas commited**. Cuando alguien lee/escribe una página no-resident, el kernel envía un **mensaje al port** describiendo qué rango se quiere. El **pager process** (filesystem, blobfs, etc) lee del backing store, llama `zx_pager_supply_pages(pager, vmo, range, aux_vmo)` para poblar, y el fault se resume.

Comparado con Linux `fault()` en `struct vm_operations_struct`, pero **explícito y en userland**. El pager es un componente; puede ser bloquearse, crashear (el VMO se marca broken) sin llevarse el kernel. Fault handling → userland.

Esto es lo que permite que el filesystem sea un componente normal, no kernel-embedded: cuando abres un archivo, el resultado es un pager-backed VMO; el pager es el filesystem process.

### Contiguous VMOs — DMA

Para DMA drivers necesitan páginas físicamente contiguas:

```c
zx_handle_t bti; // Bus Transaction Initiator, otorgado por platform bus driver
zx_vmo_create_contiguous(bti, size, alignment_log2, &vmo);
```

Alloca físicamente contiguo. Y luego:

```c
zx_handle_t pmt; // Pinned Memory Token
zx_bti_pin(bti, ZX_BTI_PERM_READ, vmo, 0, size, addrs, num_addrs, &pmt);
```

`pmt` garantiza que las páginas no se paginan out. `addrs[]` son las bus-physical (IOMMU-translated si hay) addresses para el DMA descriptor. El driver programa el device con `addrs`; device DMAs; driver desuspende pages con `zx_pmt_unpin`.

Todo esto vive en userland. El driver **no tiene access** al physical memory en general — sólo lo que el BTI le autoriza. El IOMMU se programa por el kernel para reflejar esto. **Protección anti-DMA-rogue** estructural.

---

## 5. FIDL — Fuchsia Interface Definition Language

### FIDL v2 (2021+)

FIDL v1 (hasta 2020) tenía wire format con features legacy. **FIDL v2** (2021 flag day) simplifica: eliminates "envelope inline optimization" confusion, nueva codificación para tables, strict/flexible unions unificados. Toda Fuchsia 2022+ en FIDL v2.

### Layer cake

1. **`.fidl` source**: sintaxis humana.
2. **`fidlc`** compiler: parse → JSON IR canónico.
3. **`fidlgen_<lang>`**: JSON IR → bindings en C, C++, Rust, Dart, Go. Bindings incluyen encode/decode, proxy cliente, dispatch servidor.
4. **Wire format**: bytes on the wire, versioned.

### Ejemplo

```fidl
// fuchsia.example.calculator.fidl
library fuchsia.example.calculator;

@discoverable
closed protocol Calculator {
    strict Add(struct { a int32; b int32; }) -> (struct { sum int32; });
    strict Divide(struct { a int32; b int32; }) -> (struct { quotient int32; remainder int32; })
        error DivideError;
    flexible Log(struct { level LogLevel; msg string:100; });  // one-way
};

type DivideError = strict enum : int32 {
    DIVIDE_BY_ZERO = 1;
    OVERFLOW = 2;
};

type LogLevel = strict enum : int32 { DEBUG = 0; INFO = 1; WARN = 2; ERROR = 3; };
```

Observa:
- `strict` vs `flexible`: strict falla si el peer añade un variant no conocido; flexible lo ignora/guarda. Evolutivo.
- `error` en two-way methods: result es `union { response, error }` on wire.
- `string:100` = bounded string con max 100 bytes on wire. Bound es parte del tipo.
- `@discoverable` = protocol está disponible para routing por CFv2.

### Wire format

- **Data bytes** separados de **handles array**.
- Data: headers 16-byte (txn_id, magic, flags, ordinal), luego payload 8-byte aligned.
- Out-of-line secondary objects: strings/vectors/opcional fields, pointer en primary = `UINT64_MAX` (presente, data a continuación) o `0` (absent).
- **Tables**: encoded as "envelope vector" — cada campo es un envelope (size + handle_count + inline_value_or_pointer). Añadir campo = append envelope. Lector ignora envelopes con ordinal > max conocido.
- **Unions**: selector (ordinal) + envelope para la variante activa. Flexible unions permiten ordinal unknown → bytes preservados.
- **Handles**: en el data stream aparece un "handle presence indicator" (0 o UINT32_MAX); los handles reales viajan aparte en el channel handle array.

Compat rules documentadas: [fuchsia.dev/fidl/abi-compat](https://fuchsia.dev/fuchsia-src/concepts/fidl/abi-api-compat). Ejemplos: añadir campo a table = compat; cambiar tipo de campo = break; strict→flexible = compat one-way (viejos clientes fallan en respuestas nuevas).

### Bindings comparison

- **Rust** (`fidl_rust`): async futures, natively typed. Proxy → `FooProxy` con async fns returning futures. Server → trait `FooRequestStream` + handler. Uso en netstack3, scenic, etc.
- **C++** (`fidl_cpp`): HLCPP (high-level, RAII, exceptions-free) y LLCPP (low-level, wire-format direct, zero-copy). LLCPP para perf-critical (drivers).
- **Dart** (`fidl_dart`): Flutter UI. Uso en system UI (sessionmgr, ermine).
- **Go** (`fidl_go`): netstack original en Go (deprecated en favor de netstack3 Rust, 2024+).
- **C**: C bindings minimales, casi sin uso fuera de legacy.

### Tabla — FIDL evolution rules (compat/break)

| Cambio | Wire-compat | Source-compat | Nota |
|---|---|---|---|
| Añadir campo a table | Sí | Depende lang | Flutter: cambio de constructor puede romper llamadores; Rust: `..` pattern |
| Cambiar tipo de campo | **No** | No | Break seguro |
| Strict → flexible (enum/union) | Parcial | No | Viejos peers con valores nuevos fallan |
| Flexible → strict | **No** | No | Break |
| Añadir method strict a protocol | **No** | Sí | Viejos servidores no responden al method nuevo → error |
| Añadir method flexible a protocol | Sí | Sí | Unknown method ignorado |
| Cambiar `string:N` a `string:M` con M>N | Sí | Sí | Clientes viejos no envían >N; servidores nuevos aceptan más |
| Renombrar field (mantener ordinal) | Sí wire | No source | Wire format es ordinal-based |

---

## 6. Component Framework v2 (CFv2)

CFv2 launched ~2021 como rewrite de CFv1 (aka "appmgr"). CFv1 tenía un modelo hierarchical ad-hoc; CFv2 formaliza. Since 2023 CFv1 fully deprecated.

### Componente = unidad de deployment

Un **componente** es:
- Un **manifiesto** `.cml` (Component Manifest Language — JSON5 sintaxis, compilado a `.cm` binario).
- Cero, uno o varios **binarios** (ELF, Dart bytecode, etc).
- Zero o más **capabilities** declaradas como "this component offers X".
- Zero o más **capabilities** consumidas: "I need Y routed to me".

### Capabilities types — tabla

| Capability type | Qué transporta | Ejemplo | Consumer uses via |
|---|---|---|---|
| **Protocol** | un FIDL protocol (e.g. `fuchsia.net.http.Loader`) | HTTP client API, logger, persistence | `use { protocol: "fuchsia.net.http.Loader" }`, accessible en `/svc/fuchsia.net.http.Loader` en el namespace del componente |
| **Directory** | un directorio de archivos (navegable via `fuchsia.io/Directory`) | `/config`, `/data`, certificados, fonts | `use { directory: "fonts", rights: [ "r*" ] }` |
| **Service** | bundle de protocols (instance-based, enumerable) | device class (all "audio outputs"), plural interfaces | Discovery vía listing instances |
| **Storage** | isolated persistent storage (per-component namespace en disk) | app "preferences", cache | `use { storage: "data", path: "/data" }` |
| **Runner** | un ambiente de ejecución (Dart VM, ELF runner, Web engine) | `elf_runner`, `dart_jit_runner` | Declared en manifest `program.runner` |
| **Resolver** | resuelve component URL → package → manifest | `fuchsia-pkg://...` scheme | Part of environment config |
| **Event Stream** | subscribe a lifecycle events (started, stopped, debug_started) | testing/monitoring harnesses | `use { event_stream: ["started", "stopped"] }` |
| **Dictionary** (2024+) | agregación de capabilities bajo un nombre | bundle de "diagnostics tools" | Route as single unit |
| **Config** (structured config, 2023+) | valores typed configurables per-instance | feature flags, endpoints | Compile-time schema en CML |

### Routing

Un componente no tiene ambient namespace. Su `/svc` (services dir) contiene **exactamente** las capabilities que su manifest declaró `use`, y que fueron `offered`/`exposed` por ancestros en el component tree.

Ejemplo CML:

```json5
{
  include: [ "syslog/client.shard.cml" ],
  program: {
    runner: "elf",
    binary: "bin/my_app",
  },
  use: [
    { protocol: [ "fuchsia.net.http.Loader" ] },
    { directory: "config-data", rights: [ "r*" ], path: "/config" },
  ],
  capabilities: [
    { protocol: "fuchsia.example.MyService" },
  ],
  expose: [
    { protocol: "fuchsia.example.MyService", from: "self" },
  ],
}
```

Un **parent** de este componente declara:

```json5
{
  children: [
    { name: "my_app", url: "fuchsia-pkg://fuchsia.com/my_app#meta/my_app.cm" },
  ],
  offer: [
    { protocol: "fuchsia.net.http.Loader", from: "parent", to: "#my_app" },
    { directory: "config-data", from: "parent", to: "#my_app" },
  ],
  expose: [
    { protocol: "fuchsia.example.MyService", from: "#my_app" },
  ],
}
```

El **Component Manager** (proceso privilegiado, casi el único con "ambient authority") parsea el árbol de manifests al boot, valida que todo `use` tiene un `offer` matching, y establece los handles al crear cada componente. Si un `use` no tiene su oferta, **el componente falla al arrancar** con error descriptivo.

### Realms

Un realm = un subárbol del component tree. Cada realm tiene su **environment** (resolvers + runners + policies). Se usan para testing (realm builder spawn testing subtree), para aislar subsistemas (sys realm vs session realm), para A/B testing.

### Component lifecycle

- **Not started** — componente declarado pero no running.
- **Resolved** — manifest cargado, paquete disponible.
- **Started** — procesos creados, running.
- **Stopping** — signal enviado, grace period.
- **Stopped** — procesos terminados.
- **Destroyed** — component instance removido.

Transitions observables via event streams. Usados por debugger, diagnostic, crash reporting.

---

## 7. Dart + Flutter — UI layer (y su declive)

### 2016–2021: Flutter como system UI

Fuchsia UX original = Flutter (escrito en Dart). System shell = un Flutter app (project names: Armadillo, luego Ermine, luego sessionmgr). Todo el primary UI corría en Dart VM. Dart JIT en desarrollo, AOT en producción.

**Topaz** (2016–2020) — el repo de userspace Dart/Flutter stuff. Había módulos: `sessionmgr`, `ledger` (sync distribuido experimental, cancelado), `maxwell` (ML-driven suggestions, cancelado), `mondrian` (layout). Mucho de esto era "Fuchsia Workstation" experimentation que nunca llegó a consumer.

### 2022+ pivot a C++/Rust nativo

Con focus en embedded (Nest Hub) y layoffs, Google:
- Deprecated Workstation experiments.
- El Nest Hub UI **sigue siendo Flutter** (se preserva del Nest Hub pre-Fuchsia), pero eso es un app, no el system shell.
- System components críticos escriben en **Rust** (netstack3, wlan, muchos drivers DFv2) o C++ (kernel, legacy drivers).
- **Dart en el system** reduced a apps individuales.

### 2026 status

Flutter sigue siendo cross-platform dominant (iOS/Android/web/desktop) — Google mantiene Flutter. Pero Flutter ≠ Fuchsia; Flutter funciona en cualquier OS. El hecho de que Fuchsia fuera "el OS hecho para Flutter" ya no es verdad. Fuchsia hoy es un kernel + framework embedded-focused, y Flutter es un UI toolkit cross-platform desarrollado independientemente. La narrativa unificada se rompió.

---

## 8. Driver Framework v2 (DFv2)

### Drivers como componentes

Todos los drivers en Fuchsia 2023+ son **components CFv2**. Cada driver:
- Manifest CML.
- Binary ELF (C++ o Rust).
- Declara capabilities que consume (bus access, BTI) y expone (device protocols).

DFv2 replaces DFv1 ("banjo"-based, static driver binding). Razones: DFv1 era in-process (driver_host con múltiples drivers en mismo address space); DFv2 isola drivers en componentes separados (driver process cada uno o por grupos), más crash isolation.

### Driver host + driver manager

- **Driver Manager** — componente que orquesta discovery y binding. Lee device tree (ACPI en x86, DT en ARM), encuentra devices, resuelve qué driver bind.
- **Driver Host** — host process que ejecuta un driver (o cluster de drivers). Crash del host = drivers restart; no afecta kernel ni otros drivers.

### Binding

Un driver declara **binding rules** declarativos (Bazel-like `.bind` source):

```
using fuchsia.pci;
using fuchsia.hardware.usb;

fuchsia.BIND_PROTOCOL == fuchsia.pci.BIND_PROTOCOL.DEVICE;
fuchsia.pci.BIND_VID == 0x8086;
fuchsia.pci.BIND_DID == 0x15fa;
```

Driver manager evalúa reglas contra devices descubiertos. Match → instantiate driver component → bind.

### Hot-plug + replacement

Driver component se puede restart sin reboot. Update via OTA: reemplazar el package, component manager resuelve nuevo manifest, restart driver. State transient; persistent state en storage caps.

### vs Linux kernel drivers

| Axis | Linux | Fuchsia DFv2 |
|---|---|---|
| Address space | Kernel space | Userland process |
| Crash blast radius | Kernel panic | Driver component restart |
| Binding | Module autoload + udev rules | Manifest + bind rules |
| IPC to userland | sysfs/netlink/ioctl | FIDL protocols |
| Debug iteration | rebuild kernel/module, reboot | rebuild component, restart |
| Access to hw | Directo | Via BTI, MMIO handle, interrupt handle |
| IOMMU protection | Opcional (dma-api) | Obligatorio (BTI) |

---

## 9. Deployment reality 2026

### Productos Fuchsia activos

- **Google Nest Hub 1st gen** (2018 hw, Fuchsia desde 2021) — activo.
- **Google Nest Hub 2nd gen** (2021) — Fuchsia desde launch.
- **Google Nest Hub Max** (2019 hw, Fuchsia desde ~2022).

### Productos Fuchsia hipotéticos / cancelados

- **Fuchsia Workstation** — build laptop. Nunca hardware consumer. Ars Technica 2024 lo declara "desaparecido del repo como target".
- **Nest Audio / Nest Mini** (speakers sin pantalla) — siguen Cast OS Linux.
- **Chromecast** — Linux-based Cast OS. No Fuchsia.
- **Pixel phones** — Android (Linux). No Fuchsia.
- **Wear OS watches** — Android variant. No Fuchsia.

### Futuro consumer

Nest Hub categoría smart-display en declive market-wide:
- Amazon Echo Show: revenue down 2023–25 según analistas Canalys/IDC.
- Google no lanza Nest Hub 3 en 2025; rumores CES 2026 apuntan a cancelación de línea.
- Si Nest Hub se EOL, Fuchsia pierde su único despliegue en producto consumer.

### Automotive / embedded enterprise

- Rumor 2024: Google licensing Fuchsia para automotive OEMs. Sin anuncio público.
- No hay confirmación de design wins en automotive production ships.

### Pigweed + Fuchsia

**Pigweed** (pigweed.dev) — colección de C++ modules para embedded, parcialmente Google-internal + open-source. Targets: Cortex-M bare-metal, RTOSes (Zephyr, FreeRTOS). **No es un OS**; es librerías + tooling + "Pigweed SDK".

Pigweed no corre encima de Zircon (Zircon es application-class, Cortex-A). Pigweed y Fuchsia son complementarios por abstracción (microcontroller vs application processor) pero no integrados en un stack. Un hypothetical Pigweed-on-Zircon no existe.

### Código y comunidad

- Repo: `fuchsia.googlesource.com/fuchsia` — abierto read. Push requiere Google CLA + review + `OWNERS` files.
- Gerrit code review público.
- RFCs en `docs/contribute/rfcs/` — proceso público pero approval Google-interno.
- Externa: ~ docenas de "friends of Fuchsia" sin Googlers empujando commits. Comunidad externa comparativamente pequeña vs Linux o incluso seL4 Foundation.

---

## 10. Tabla — Zircon vs seL4 vs KeyKOS vs Mach (enfocada a diferencias)

Complementa la tabla wider en `r3/capability_kernels.md §tabla final`. Esta enfoca *diferencias estructurales* entre los cuatro cap-kernels más referenciados.

| Eje | Zircon | seL4 | KeyKOS/CapROS | Mach 3.0 |
|---|---|---|---|---|
| **Handle/cap model** | Integer handle (32-bit) + rights bitmap, per-process table | CPtr navegando CNode tree (radix), rights per-slot | "Key" (segment:offset), ortho-persistent | Port name (integer) per-process + right kind (send/recv/send-once) |
| **Unforgeable** | Sí (handle integer solo válido en su process table, randomizado) | Sí (CPtr resuelto in-kernel via CSpace) | Sí (keys verificadas por kernel) | Sí (port names per-process) |
| **Rights granularity** | 32-bit bitmask (read/write/exec/duplicate/transfer/map/wait/inspect + type-specific) | Per-type rights (Read, Write, Execute, Grant, GrantReply, en variantes) | Weak/strong keys, poca granularidad | Binary per right-kind |
| **Verification** | Ninguna formal | End-to-end formal proof (Isabelle/HOL, refinement + integrity + info-flow) | Ninguna (pre-formal-methods era) | Ninguna |
| **IDL approach** | FIDL (typed IDL, multi-lang bindings, wire format evolvable via tables) | CAmkES IDL4 + ADL (C/C++, ties into CapDL) | Ad hoc (keys per type) | MIG (Mach Interface Generator) + XPC typed dict |
| **IPC primitive** | Channel (bytes + handles), socket, fifo, event, port | Endpoint (sync rendezvous) + Notification (async) + Reply caps | Key invocation (like RPC), Gates | Port send/receive, OOL VM |
| **Memory model** | VMO first-class, clonable COW, pager-backed, BTI for DMA | Untyped memory retyping, user-driven allocator | Single-level store + ortho persistence | VM objects + OOL via VM copy |
| **Deployment scale** | ~10M Nest Hubs | Millions: avionics (Boeing 787+), automotive (Cog Systems), defense | Tymshare (1980s, extinct commercial) | Billions: all Apple devices (XNU ≠ pure Mach) |
| **License** | BSD+MIT+Apache 2.0 mixed | GPLv2 (kernel), BSD (user libs) | Various (KeyKOS extinct proprietary; CapROS BSD-like) | BSD (Mach original) + Apple CDDL+APSL (XNU) |
| **LOC kernel** | ~200k C++ | ~10k C | ~20k C (KeyKOS) | ~50k C (Mach 3 alone) |
| **Active devel 2026** | Yes, ~2000 commits/mes, ~⅓ pico 2020 | Yes, moderate, Foundation-governed | No (CapROS dormant since ~2012) | No standalone; XNU active but Mach portion stable |
| **Process model** | Job → Process → Thread hierarchy, no fork() | root_task + manual via Untyped retype; no fork() | Domains (processes) spawned via constructors | Tasks + threads, fork() emulated in BSD layer |
| **Driver model** | DFv2 (userland components, IOMMU-protected DMA) | CAmkES VM or custom user drivers | Custom | IOKit (XNU, partially kernel-space C++) |

---

## 11. ALZE applicability — niveles

### v1 (hoy) — ninguno

ALZE `/root/repos/alze-os` es monolítico C99+asm, limine boot, kernel-land allocator, SMP WIP, IDT incompleta (P0 per review R2). Introducir handle + rights + VMO + FIDL requiere **rewrite estructural** del ABI syscall. No procede en v1.

**v1 acción**: estudiar. Leer `fuchsia.dev/fuchsia-src/concepts/kernel/` y `concepts/process/`. Tomar notas. No tocar código.

### v2 (post-P0, ~2026-Q4) — VMO + handle model lightweight

Adopción parcial de patrones Zircon sin rewrite total:

1. **Handle + rights**. Ya cubierto en `r3/capability_kernels.md §11 v2` como "Zircon-lite in C99". Sigue siendo el paso 1.

2. **Introducir VMO-like object**. En lugar de que el allocator kernel retorne `void*`, retornar un `vmo_t*` (typed struct with refcount, size, pages array, ops vtable). Syscalls de memoria: `vmo_create(size) → handle`, `vmo_map(vmo_handle, vaddr, perms, len)`, `vmo_read/write(vmo_handle, off, buf, len)`. Ventaja inmediata: el mapping es explícito, el proceso puede tener un VMO sin mapearlo (útil para IPC transfer).

3. **Channel syscall**. `channel_create() → (h1, h2)`, `channel_write(h, bytes, len, handles[], n_handles)`, `channel_read(h, ...)`. Es la primitiva IPC básica. No necesita IDL — bytes + handle array es suficiente en v2.

4. **Job para resource accounting**. Incluso si no se jerarquiza completo: un "job" = contenedor de procesos con límites (max memory, max handles, max threads). Syscall `job_create(parent_job) → handle`, `job_set_policy(job, policy)`. Si falta tiempo, skip job hierarchy, pero el concepto es útil.

Esfuerzo estimado: ~3k LOC netas sobre la v1 Zircon-lite, ~3–6 semanas dev dedicado. Complejidad: VMO + pager semantics es lo caro (pager-backed puede omitirse en v2; sólo anonymous + contiguous).

### v3 (aspiracional) — CFv2-like component framework

Si ALZE tiene multi-dev y rewrite parcial a Rust:

1. **Manifest format** — CML-like en JSON5 o TOML. Decisión simple: copiar CML semántica pero adaptarla.
2. **Component manager** — proceso userland privilegiado que lee manifests y distribuye caps al startup.
3. **Runners** — ELF runner inicial; más tarde web runner, etc.
4. **IDL** — **NO FIDL** (muy ligado a Fuchsia tooling). Usar **Cap'n Proto** o definir un `alze-idl` proc-macro Rust.
5. **Capability routing** — services, directories, protocols. Semántica clave: un componente no accede a namespace global; sólo a lo routed por manifest.
6. **Driver framework** — drivers as components post-v3. Requiere IOMMU obligatoria y BTI-like abstraction. Muy costoso — cada driver Linux habría que reescribir.

Este es el ambicious endgame. Realísticamente: **no antes de 2028** con solo-dev. Si llega multi-dev + empresa sponsor, podría bajar.

### Lo que ALZE NO debería copiar

- **Dart/Flutter system UI** — atadura de toolkit no apropiada para un hobbyist OS. Si ALZE tiene UI, iniciar framebuffer + rendering propio o portar algo existente (no Flutter).
- **Fuchsia build system (GN/Ninja + SDK manifests)** — demasiado complejo. Use `make`/`ninja`/`cmake` ordinarios.
- **Starnix** (Linux ABI emu) — útil long-term para portar apps Linux, pero es un proyecto en sí mismo (~50k LOC). Out of scope hobbyist.
- **FIDL toolchain** — atado a `fidlc`, `fidlgen_*`, manifest system. Usar Cap'n Proto o ad-hoc.

---

## 12. Nota honesta final — el lado comercial

**Zircon representa la inversión más grande de Google en microkernel en la historia.** Se estima ~500+ ingenieros-año acumulados (2016–2026). Equipo de ~100 ingenieros en pico (2020). Infraestructura build/CI ingente. Documentación pulida. Sintaxis bonita.

**Y aun así, en 2026, el deployment es ~10M Nest Hubs y la categoría está muriendo.**

### Por qué

1. **Linux es good enough**. El argumento técnico para un cap-kernel es fuerte en teoría (security, composability, driver crash isolation). En práctica: Linux tiene fuzzing exhaustivo, seccomp, SELinux/AppArmor, namespaces, eBPF. La ventaja marginal de Zircon no justifica el porting cost.
2. **Apps matter more than kernels**. Un teléfono Pixel corre Android porque hay 3M apps en Play Store. Un Nest Hub con Fuchsia muestra YouTube/Photos/smart home controls — ahí no hay "app ecosystem". Es un dispositivo appliance. Fuera del appliance, Fuchsia no tiene apps.
3. **Developer ergonomics**. Portar una app Linux (con su dependencias glibc, systemd, /proc, etc) a Fuchsia es semanas de trabajo. Nadie lo hace sin pagarle.
4. **OS without market niche**. Android ocupó "phone". Chrome OS "cheap laptop". Linux "server". Windows "desktop". macOS "creative pro". Fuchsia propuso "unified Google OS" — un target que requería migrar ecosistemas existentes, con beneficio marginal para usuario final.
5. **Starnix llegó tarde**. Starnix (Linux ABI compat) podría haber bridged el gap (corre apps Linux mientras la nueva ecosystem se construye). Fue priorizado después del momentum initial loss.

### Lección para ALZE

> **Un kernel es una decisión técnica. Un OS es una decisión de mercado.**

ALZE no tiene ambiciones de mercado. Es un kernel hobbyist + didáctico. Por tanto la moraleja no se aplica exacto, pero se traduce:

- **Si ALZE quiere ser *usado*** (por alguien más que su autor, para hacer algo real): apunta a **Linux ABI compat** primero. POSIX/ELF//proc subset. Permite correr binarios Linux. No reinventes el world. Ejemplos: Haiku ganó tracción parcial con su POSIX layer; Illumos sobrevive por Solaris compat; WSL1 corrió binaries ELF Linux en Windows. **Sin compat = sin usuarios**.

- **Si ALZE quiere ser *estudiado*** (por autor + lectores potenciales, como artefacto de aprendizaje): **copia patrones Zircon**. Son gold standard — VMO as first-class, handle+rights, channel IPC, no-fork process creation, driver framework CFv2-like. Aquí el propósito es entender y documentar; no hay presión comercial. Escribe sobre ello, documenta las decisiones, publica blog posts. Fuchsia ha hecho el diseño; ALZE lo puede copiar con mano más ligera.

- **Si ALZE intenta ambos**: falla ambos. Mismo error que Fuchsia — "kernel superior + OS nuevo", nadie compra el combo. Mejor elige.

### El veredicto sobre Fuchsia

Fuchsia es un **triumph técnico + tragedia comercial**. Zircon es el mejor microkernel-shipped-in-production históricamente (seL4 tiene mejor verificación pero deployment más nicho). FIDL es el mejor IDL cross-lang (Cap'n Proto compite pero sin OS integration). CFv2 es el mejor component framework (sistemd + Android Binder combinados, pero más coherente).

Ninguna de esas victorias técnicas se traduce a usuarios. Google lleva la carga porque Google tiene capital para sostener un OS como proyecto R&D extendido. Un hobbyist no lo tiene.

Por tanto: **ALZE copia lo que puede, no pretende competir, y se recuerda que el kernel no es el producto.**

---

## Referencias primarias

**Historia + equipo**:
- Geiselbrecht, *"LK: The Little Kernel"*, embedded kernel docs, 2008– [github.com/littlekernel/lk](https://github.com/littlekernel/lk).
- Amadeo (Ars Technica), *"Google's Fuchsia OS is now running on Nest Hubs"*, 2021-05-25 [arstechnica.com](https://arstechnica.com/gadgets/2021/05/googles-fuchsia-os-is-now-running-on-nest-hubs/) / [archive](https://web.archive.org/web/2023*/https://arstechnica.com/gadgets/2021/05/).
- Amadeo (Ars Technica), *"Google's Fuchsia OS layoffs"*, 2023-01 [arstechnica.com](https://arstechnica.com/gadgets/2023/01/google-lays-off-16-of-the-fuchsia-os-team/).
- 9to5Google Fuchsia tag coverage [9to5google.com/guides/fuchsia/](https://9to5google.com/guides/fuchsia/).
- Geiselbrecht, *"Zircon: Experiences building a Kernel"*, LPC 2019 talk [lpc.events](https://linuxplumbersconf.org/event/4/contributions/444/).

**Zircon concepts (fuchsia.dev)**:
- Zircon overview [fuchsia.dev/fuchsia-src/concepts/kernel](https://fuchsia.dev/fuchsia-src/concepts/kernel).
- Handles [fuchsia.dev/fuchsia-src/concepts/kernel/handles](https://fuchsia.dev/fuchsia-src/concepts/kernel/handles).
- Rights [fuchsia.dev/fuchsia-src/concepts/kernel/rights](https://fuchsia.dev/fuchsia-src/concepts/kernel/rights).
- Jobs [fuchsia.dev/fuchsia-src/concepts/kernel/jobs](https://fuchsia.dev/fuchsia-src/concepts/kernel/jobs).
- VMOs [fuchsia.dev/fuchsia-src/reference/kernel_objects/vm_object](https://fuchsia.dev/fuchsia-src/reference/kernel_objects/vm_object).
- Channels [fuchsia.dev/fuchsia-src/reference/kernel_objects/channel](https://fuchsia.dev/fuchsia-src/reference/kernel_objects/channel).

**FIDL**:
- FIDL language spec [fuchsia.dev/fuchsia-src/reference/fidl/language/language](https://fuchsia.dev/fuchsia-src/reference/fidl/language/language).
- FIDL wire format [fuchsia.dev/fuchsia-src/reference/fidl/language/wire-format](https://fuchsia.dev/fuchsia-src/reference/fidl/language/wire-format).
- ABI/API compat rules [fuchsia.dev/fuchsia-src/concepts/fidl/abi-api-compat](https://fuchsia.dev/fuchsia-src/concepts/fidl/abi-api-compat).
- FIDL v2 RFC [fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0114_fidl_envelope_inlining](https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0114_fidl_envelope_inlining).

**Components**:
- Component framework overview [fuchsia.dev/fuchsia-src/concepts/components](https://fuchsia.dev/fuchsia-src/concepts/components).
- CFv2 introduction [fuchsia.dev/fuchsia-src/concepts/components/v2/introduction](https://fuchsia.dev/fuchsia-src/concepts/components/v2/introduction).
- Capabilities reference [fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities](https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities).
- CML reference [fuchsia.dev/fuchsia-src/reference/components/cml](https://fuchsia.dev/fuchsia-src/reference/components/cml).

**Driver Framework**:
- DFv2 overview [fuchsia.dev/fuchsia-src/concepts/drivers](https://fuchsia.dev/fuchsia-src/concepts/drivers).
- DFv2 migration [fuchsia.dev/fuchsia-src/development/drivers/migration](https://fuchsia.dev/fuchsia-src/development/drivers/migration).

**Starnix**:
- Starnix RFC [fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0082_starnix](https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0082_starnix).

**Source code**:
- Fuchsia source [fuchsia.googlesource.com/fuchsia](https://fuchsia.googlesource.com/fuchsia).
- Mirror GitHub [github.com/rust-embedded/Fuchsia-mirror](https://github.com/) (unofficial mirrors vary).

**Pigweed**:
- Pigweed docs [pigweed.dev](https://pigweed.dev).

**LK / Trusty**:
- LK upstream [github.com/littlekernel/lk](https://github.com/littlekernel/lk).
- Trusty TEE [source.android.com/docs/security/features/trusty](https://source.android.com/docs/security/features/trusty).
