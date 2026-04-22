# seL4 — the verified microkernel, in depth

**Ronda:** R4 / `sel4_verified.md`
**Fecha:** 2026-04-22
**Scope:** deep-dive exclusivo en seL4 + su stack de verificación + CAmkES + despliegue embedded. Complementa (no duplica) `otros.md` y `r3/capability_kernels.md`.

**No duplica**:
- CNode-as-tree básico, Untyped/Retype concept, fastpath <200 cycles narrativa, CAmkES overview de 3 líneas → todo eso ya está en R3.
- Comparación cap vs ACL, linaje KeyKOS/EROS, Mach ports, Zircon → R3 cubre.

**Aporta nuevo**:
- Pipeline de verificación paso a paso (abstract → Haskell → C → binary) con detalle de herramientas concretas.
- Historia de las *cinco* propiedades probadas (functional correctness 2009 → binary 2013 → infoflow 2013 → timing 2017 → RISC-V full 2024).
- MCS scheduler extension a detalle (Anna Lyons PhD 2019).
- CAmkES templates + ADL + CapDL + global-systems-initializer como pipeline completo.
- Despliegue real (HENSOLDT, Boeing, Lockheed, automoción MBUX-like hypervisors).
- Limitaciones concretas de la verificación (drivers, compilador, hardware side-channels, DMA).
- Números 2026: seL4 Foundation members, funding, production deployments.

---

## 1. Historia precisa

### Pre-seL4: linaje L4

- **L4 v2 (Liedtke)** — Jochen Liedtke, GMD Bonn, 1993. *"Improving IPC by Kernel Design"*, SOSP 1993. DOI [10.1145/168619.168633](https://dl.acm.org/doi/10.1145/168619.168633). Escrito en assembler 486-específico, ~12 KLOC. Thesis central: "a concept is tolerated inside the microkernel only if moving it outside, i.e. permitting competing implementations, would prevent the implementation of the system's required functionality" — el *minimality principle*. IPC en ~250 ciclos en un 486/50, una revolución contra Mach (~5000 ciclos).
- **L4Ka::Hazelnut / Pistachio** — Karlsruhe, Uwe Dannowski + Volkmar Uhlig, 2001–2003. Primer L4 portable (C++, x86/IA-64/PowerPC/ARM/MIPS/Alpha/Sparc). Introduce el abstracto "IPC message registers" + "map/grant/flush" para VM.
- **L4::Fiasco** — TU Dresden, Michael Hohmuth, 1998–. Escrito en C++, preemptable internamente, soporte real-time. L4Re (L4 Runtime Environment) crece sobre Fiasco.

### Fundación del proyecto seL4

- **2004**: NICTA (National ICT Australia, Sydney) + UNSW forman el **Embedded Real-Time and OS** group liderado por **Gernot Heiser** (alemán/australiano, profesor UNSW desde 1991) y **Kevin Elphinstone** (PhD UNSW 1999 en L4). Harvey Tuch hace early work en un memory-model para verificación de C kernel code (TPHOLs 2005).
- **Propósito explícito**: un L4 re-escrito desde cero *diseñado* para ser verificable — no verificar el L4 existente. Decisión clave: lenguaje C subset estático, sin allocator dinámico en kernel, sin concurrencia intra-kernel (big lock), sin punteros aritméticos opacos.
- **2006–2009**: Data61 (grupo de verificación, Gerwin Klein + David Cock + Michael Norrish) desarrolla la metodología de refinamiento en Isabelle/HOL. El kernel mismo lo escribe Philip Derrin + Kevin Elphinstone a mano, en paralelo y coordinado con la spec Haskell de Dhammika Elkaduwe.
- **SOSP 2009**: Klein, Elphinstone, Heiser, Andronick, Cock, Derrin, Elkaduwe, Engelhardt, Kolanski, Norrish, Sewell, Tuch, Winwood, *"seL4: Formal Verification of an OS Kernel"*, SOSP 2009. [DOI 10.1145/1629575.1629596](https://dl.acm.org/doi/10.1145/1629575.1629596) / [PDF](https://sigops.org/s/conferences/sosp/2009/papers/klein-sosp09.pdf). Primer kernel general-purpose con **functional correctness proof** end-to-end (C ↔ abstract spec).
- **CACM 2010**: versión reprint con audiencia amplia — [CACM 53(6), June 2010](https://cacm.acm.org/magazines/2010/6/92498-sel4-formal-verification-of-an-operating-system-kernel/).
- **2011**: Secure Systems Group + Galois colaboran en **SMACCM** (Secure Mathematically-Assured Composition of Control Models), DARPA HACMS. Prueba on-air: un **Boeing Little Bird** (helicóptero autónomo modificado) corriendo seL4 resiste red-team attacks (2015).
- **2014-07-29**: **open source release** bajo GPLv2 (kernel) + BSD-2 (libseL4 userland). GitHub [seL4/seL4](https://github.com/seL4/seL4).
- **2020-04-07**: constitución de la **seL4 Foundation** (Linux Foundation-style, nonprofit neutral). Governance pasa de NICTA/Data61/CSIRO a foundation independiente. [sel4.systems/Foundation](https://sel4.systems/Foundation/).

### Personas clave

- **Gernot Heiser** — Scientia Professor UNSW, co-founder OK Labs (comercial L4 predecessor, adquirido por General Dynamics 2012). Jefe intelectual del proyecto.
- **Gerwin Klein** — Proofcraft CEO (spin-off 2021). Lead architect de la verificación. Primer autor del SOSP 2009.
- **Kevin Elphinstone** — Associate Professor UNSW, co-autor del kernel mismo.
- **June Andronick** — Proofcraft CEO-co-founder. Lead de la verificación de timing (2017) + automación de proofs.
- **Thomas Sewell** — binary-level verification. Ahora en UNSW + Proofcraft.
- **Anna Lyons** — PhD 2019 UNSW, *"Scheduling Contexts and the MCS Extension"*. Autora del MCS kernel branch, mergeado al mainline 2020.
- **Dhammika Elkaduwe** — autor de la spec Haskell ejecutable original.
- **David Cock** — proofs de integrity + information flow (con Murray et al 2013).

---

## 2. El stack de verificación — capa por capa

La verificación seL4 es un *pipeline de refinamientos* entre modelos formales, con la propiedad de que cada capa "simula" a la de arriba. La composición de refinamientos da la prueba end-to-end "el binario ARM es correcto respecto a la spec abstracta".

### Capa 1 — Abstract specification (Isabelle/HOL)

- ~4,900 líneas de Isabelle/HOL (como teoría `Abstract` / `Arch`).
- Define *qué* hace cada syscall a nivel matemático: funciones puras sobre un `state` record que tiene `cdt :: cslot_ptr ⇒ cslot_ptr option` (capability derivation tree), `cur_thread`, `kheap :: obj_ref ⇒ kernel_object option`, etc.
- Todas las ops se expresan en la mónada `(det_ext) s_monad`, que permite estado + no-determinismo + excepciones + fallo.
- Nota clave: el abstract no es ejecutable — hace cosas como "pick any thread in the ready queue" sin fijar política. La elección concreta vive en la capa Haskell.
- Referencia: [Abstract/Syscall_A.thy](https://github.com/seL4/l4v/blob/master/spec/abstract/Syscall_A.thy) en el proyecto **l4v** (L4 verification, repo separado del kernel).

### Capa 2 — Executable specification (Haskell)

- ~5,700 líneas Haskell.
- Misma estructura de datos, mismo estado, pero ejecutable — se compila y ejecuta como *kernel simulator*. Data61 la usó en los primeros años como oracle para property-based tests del C.
- Escrita en un subset de Haskell purificado: no `IO`, sólo la mónada `Kernel` (state + error). Record-wildcards para copia de estado.
- Importada a Isabelle vía herramienta custom **h2ipp** (Haskell-to-Isabelle preprocessor) + **Haskabelle** (later replaced): parse Haskell → genera teoría Isabelle con definiciones *equivalentes*.
- Referencia: [haskell/src/SEL4](https://github.com/seL4/seL4/tree/master/haskell) — sigue en el repo pero la versión autoritativa está en l4v.

### Capa 3 — C implementation

- ~10,500 líneas C (ARM) / ~12,000 (aarch64) / ~13,000 (x86_64).
- Kernel C normal pero con restricciones estrictas para ser parseable:
  - No `goto` fuera del *unified-return pattern* aceptado por la herramienta.
  - No `sizeof` dentro de expressions complejas.
  - No cast entre puntero y entero excepto en sites enumerados.
  - Sin `volatile` fuera de MMIO.
  - Sin unión types salvo los *tagged unions* generados por la herramienta `kernel_all.bf` (bitfield generator).
- **Parser**: la herramienta **c-parser** de NICTA (Michael Norrish) convierte C → una AST en Isabelle/HOL, usando la semántica de Simpl (Norbert Schirmer 2006).
- Referencia: [src/](https://github.com/seL4/seL4/tree/master/src) con [src/fastpath/fastpath.c](https://github.com/seL4/seL4/blob/master/src/fastpath/fastpath.c) como ejemplo canónico.

### Capa 4 — Binary validation

- **Sewell, Myreen, Klein — "Translation Validation for a Verified OS Kernel"**, PLDI 2013. [PDF](https://trustworthy.systems/publications/nicta_full_text/6449.pdf).
- Problema: el refinamiento C→Haskell→Abstract asume que *gcc compila correctamente la semántica C que asumimos*. Pero gcc no está verificado (CompCert sí, pero se eligió gcc por compat + perf).
- Solución: después del compile, una herramienta hace **translation validation** — toma el binario ARM generado y la semántica Isabelle del C, y usa un SMT solver (Z3 + SONOLAR) para probar que *este* binario concreto implementa la semántica C esperada.
- No es una prueba "el compilador es correcto en general" — es "este commit, con este gcc, genera un binario correcto". Cada release de seL4 re-corre la binary validation.
- Stack: Isabelle exporta la spec C a una forma intermedia graph-based (*graph language*), el binario ARM se decompila a la misma forma, SMT solver prueba equivalencia block-by-block.
- Referencia: [proof/asmrefine](https://github.com/seL4/l4v/tree/master/proof/asmrefine) en l4v.

### Capa 5 — Refinement glue (la parte cara)

- Lo que ata las capas: pruebas de que *cada función* de la capa N refina la función correspondiente de la capa N-1.
- Técnica: **forward simulation** — dado un estado concreto `s` y abstracto `S` relacionados por `R`, toda ejecución `s → s'` tiene una ejecución abstracta `S → S'` con `R s' S'`.
- Escalabilidad: Klein + Cock + Sewell — **"Secure Microkernels, State Monads and Scalable Refinement"**, TPHOLs 2008 / ITP 2008. [PDF](https://trustworthy.systems/publications/nicta_pubs/6223.pdf). Descompone proofs por *funciones locales* — cada `seL4_op` se verifica aislada, Hoare-triple style, dentro de la mónada de estado.
- Volumen: ~480,000 líneas de proof Isabelle/HOL para cubrir los 10k de C. Ratio ~20:1 (proof:code), número citado universalmente en el campo.
- Costo personas: **25 person-years** para functional correctness inicial ARMv6/v7 (2009). Cada port añade ~5-10 py (ver sección 5).

### Tabla — seL4 verification levels

| Propiedad | Año publicación | Referencia | LOC proof | Comentario |
|---|---|---|---|---|
| **Functional correctness** (refinement C↔Abstract) | 2009 | Klein et al SOSP 2009 | ~200k Isabelle | ARM only; kernel ~8.7k C |
| **Integrity** (no write without cap) | 2011 | Sewell et al ITP 2011 / [TOCS 2014](https://trustworthy.systems/publications/papers/Klein_AEMSKH_14.pdf) | +40k | composable con functional |
| **Authority confinement / Information flow (non-interference)** | 2013 | Murray, Matichuk, Brassil, Gammie, Bourke, Seefried, Lewis, Gao, Klein — **"seL4: from General Purpose to a Proof of Information Flow Enforcement"**, IEEE S&P 2013 | +100k | requiere MILS-style config estática |
| **Binary correctness** (gcc output ↔ C semantics) | 2013 (PLDI) | Sewell+Myreen+Klein PLDI 2013 | automatizado (SMT) | ARM only; manual bridge en funciones "hard" |
| **Timing: worst-case execution time (WCET) bound** | 2017 | Blackham et al **"Timing analysis of a protected operating system kernel"**, RTSS 2011 + **Sewell et al 2017 "High-Assurance Timing Analysis for a High-Assurance Real-Time Operating System"** RTSS 2017. [PDF 2017](https://trustworthy.systems/publications/nicta_full_text/10131.pdf) | sound WCET via refinement | ARM only; MCS kernel requiere re-hacer |
| **MCS (Mixed Criticality Scheduling) functional correctness** | 2024 | Kovalev et al **"Verifying the MCS Extensions of seL4"**, CPP 2024 | MCS branch verified | base kernel; scheduling policies |
| **RISC-V full verification (rv64)** | 2024 | seL4 Foundation + Proofcraft, announced July 2024 — [sel4.systems blog 2024-07](https://sel4.systems/news/2024-07/) | port complete | Hex-Five Inclusive Verification program |

El **kernel verificado 2026** es: ARMv7 + ARMv8 (aarch64) + x86_64 + RISC-V64 *functional correctness*; ARMv7 + aarch64 *binary*; ARMv7 *timing* + *info flow*. El x86_64 no tiene binary ni timing proof todavía (2026) — limitación admitida del roadmap.

---

## 3. Objetos del kernel — operaciones y uso

R3 ya lista los objetos (§3 de capability_kernels.md). Aquí las **operaciones completas** y el **uso típico** de cada uno.

### Tabla — seL4 kernel objects

| Objeto | Tamaño típico | Operaciones vía syscall | Uso típico |
|---|---|---|---|
| **CNode** | `16 * 2^N` bytes (N=radix bits) | Retype-from-Untyped; `CNode_Copy`, `CNode_Mint`, `CNode_Move`, `CNode_Delete`, `CNode_Revoke`, `CNode_Rotate`, `CNode_SaveCaller` | backbone del CSpace — todo thread root CNode es la raíz de su espacio de caps |
| **Endpoint** | 16 bytes | Retype; `Send`, `Recv`, `Call`, `NBSend`, `NBRecv`, `Reply` (non-MCS), `ReplyRecv` | sync RPC entre threads; badge identifica emisor |
| **Notification** | 16 bytes | Retype; `Signal`, `Wait`, `NBWait`, `Poll` | async signal (bitmask badge OR'd); equivalente a sem/condvar |
| **TCB** (Thread Control Block) | ~700 bytes (depende arch) | Retype; `TCB_Configure`, `TCB_SetPriority`, `TCB_SetSchedParams`, `TCB_Suspend`, `TCB_Resume`, `TCB_ReadRegisters`, `TCB_WriteRegisters`, `TCB_CopyRegisters`, `TCB_SetIPCBuffer`, `TCB_BindNotification`, `TCB_UnbindNotification`, `TCB_SetTLSBase` | un thread; posee CSpace root + VSpace + IPC buffer + opcional SchedContext (MCS) |
| **VSpace** (= PageGlobalDirectory / PML4) | 4 KiB | Retype; `VSpace_Map`, `VSpace_Unmap`; y por cada nivel inferior (PD, PT, Page) ops análogas | raíz de las page tables; cada address space |
| **Frame** (1 página física) | 4 KiB / 2 MiB / 1 GiB | Retype; `Frame_Map`, `Frame_Unmap`, `Frame_GetAddress` | página mapeable en cualquier VSpace con cap a la frame |
| **Untyped** | heredado | `Untyped_Retype(type, size_bits, dest_cnode, slot, ...)`, `Untyped_Revoke` | pool de memoria no tipada; userland decide qué crear |
| **IRQControl** | singleton | `IRQControl_Get(irq, dest_slot)` → crea IRQHandler | único creador de IRQHandler caps |
| **IRQHandler** | 16 bytes | `IRQHandler_Ack`, `IRQHandler_SetNotification`, `IRQHandler_Clear` | enlaza IRQ → notification (despachador userland) |
| **ASIDPool** / **ASIDControl** | kernel-internal | `ASIDPool_Assign(vspace)` | gestión de address space IDs para TLB tagging |
| **Reply** (MCS) | 16 bytes | auto-generado por `Call`; consume al reply | capability de reply de un solo uso (en MCS es first-class) |
| **SchedContext** (MCS) | 32 bytes | Retype; `SchedContext_Configure(budget, period)`, `SchedContext_Bind(tcb)`, `SchedContext_Unbind`, `SchedContext_YieldTo` | presupuesto de tiempo + periodo de refill, *separado* del TCB |
| **SchedControl** (MCS) | singleton por core | `SchedControl_Configure` | unico creador de SchedContexts válidos |

### Retype — el contrato

`seL4_Untyped_Retype(untyped, type, size_bits, root, node_index, node_depth, node_offset, num_objects)`:

- Consume `num_objects * 2^size_bits` bytes del untyped.
- Crea `num_objects` objetos del tipo pedido, caps colocadas en slots consecutivos desde `(root, node_index, node_offset)`.
- Si ya hay objetos derivados del untyped, los nuevos van *detrás* en memoria — no se puede fragmentar.
- `Untyped_Revoke(untyped)` destruye todos los descendientes y libera el untyped entero.
- Invariante del kernel (verificado): ninguna región de memoria está referenciada por dos objetos típicos distintos simultáneamente. Esta es la pieza central del memory-safety proof: el kernel nunca confunde memoria.

---

## 4. IPC design — más allá del fastpath

R3 cubrió el fastpath y las variantes básicas. Aquí los detalles que faltan.

### El rendezvous model

- **Endpoint** tiene tres estados: `idle`, `send-queue` (threads esperando enviar), `recv-queue` (threads esperando recibir). No ambos a la vez — son estados mutuamente excluyentes.
- `Send(ep, msg)` desde thread `T`:
  - Si ep en `recv-queue` con receptor `R`: transfer directo `T → R` (registros + badge + opcional handles), ambos continúan. Si no en MCS, `T` sigue; en MCS, `T` puede ceder sched context al receptor (`schedcontext donation`).
  - Si ep en `idle` o `send-queue`: `T` se mete en `send-queue` y bloquea.
- `Recv(ep)` simétrico.
- `Call(ep, msg)`: idéntico a Send, pero el kernel además:
  - Crea implícitamente una cap `Reply` single-use, almacenada en un slot especial del TCB receptor (non-MCS) o en un slot designado (MCS, donde Reply es first-class object).
  - Bloquea al caller esperando sobre esa Reply cap.
  - El receptor usa `Reply(msg)` o `ReplyRecv(ep, msg)` para enviar la respuesta y opcionalmente esperar el siguiente call.

### Fastpath anatomy (x86_64)

Código en `src/fastpath/fastpath.c`. Condiciones de activación (*all must hold*):
1. Syscall es `seL4_Call` o `seL4_ReplyRecv`.
2. Mensaje usa `≤ n_msgRegisters` words (4 en ARM, 4 en x86_64 con restricciones).
3. No hay extra caps en el mensaje (no handle transfer).
4. El endpoint está en recv-queue con exactamente un waiter.
5. El receptor tiene prioridad ≥ sender.
6. El receptor y sender comparten dominio (domain scheduling).
7. La VSpace está activa y cacheada.

Si todas se cumplen:
- Copia `n_msgRegisters` de `r0..r3` (ARM) o regs equiv.
- Cambia VSpace (carga `ttbr0` / `cr3`).
- Restaura contexto del receptor en sus regs.
- `eret` / `sysret`.

Si alguna falla: `enter_slowpath()` — misma semántica, pero por el camino normal C.

Medidas publicadas (Heiser + Klein, 2020 whitepaper): 
- ARM Cortex-A53 @ 1.4 GHz: 374 ns round-trip Call/Reply.
- x86_64 Xeon E5-2667v3 @ 3.2 GHz: 268 ns.
- RISC-V SiFive U74 @ 1.2 GHz: ~600 ns.
- Equivalente en ciclos: ~150-200 por lado (~400 round-trip).

### Badges

- Al `Mint` una cap de Endpoint/Notification, se le asigna un `badge` de 28-64 bits (dep arch).
- En el `Recv`, el kernel entrega el badge como parte del *message info*.
- Patrón típico: servidor tiene un único endpoint; múltiples clientes reciben caps minted con badges distintos (uno por cliente, o uno por tipo de cliente). El servidor `switch(badge)` para dispatchear.
- No hay broadcast — es punto a punto. Para "fanout" usa Notifications (badge OR'd es el canal idiomático).

### Notifications — el canal async

- Primitiva nativa post-2013 (antes eran *async endpoints*, renombradas por claridad).
- Una `Notification` tiene un *word* (bitmask). `Signal(ntfn)` hace OR del badge de la cap signal-side sobre ese word. `Wait(ntfn)` consume el word entero (devuelve el valor, reset a 0).
- Non-blocking `Poll(ntfn)` no bloquea si word=0.
- Se puede **bind** una Notification a un TCB (`TCB_BindNotification`): el TCB bloqueado en `Recv(ep)` se desbloquea si llega una signal pendiente — permite mezclar sync IPC y async signals en el mismo waiter.
- Usado para IRQ handlers (IRQHandler signals a Notification, driver thread waits).

### MCS Reply caps — first-class

En el **MCS kernel** (mainlined ~2020) la reply capability deja de ser un slot mágico en el TCB y se vuelve un *kernel object* real:

- `Reply` es un tipo Retype-able (16 bytes, slot en CNode normal).
- `Call` requiere pasar `Reply` cap explícitamente como argumento.
- La Reply puede transferirse entre threads — habilita patrones "operador-worker" donde un supervisor recibe Calls, delega a workers y el worker hace Reply directamente al caller original sin round-trip al supervisor.
- **Reply + SchedContext donation**: si un thread `A` hace Call a `B`, `A` puede donar su sched context a `B`; `B` ejecuta *con el budget de A*; al Reply, el budget vuelve a `A`. Esto resuelve el problema clásico de inversión de prioridad en microkernels sin necesidad de priority-inheritance ad-hoc — el *tiempo* es una capability que viaja con la Reply.

---

## 5. CAmkES — component framework a detalle

### Qué es, exactamente

- **CAmkES** = Component Architecture for microkernel-based Embedded Systems.
- Autores: **Ihor Kuz** + **Dave Cock** + **Gerwin Klein**, NICTA/Data61, ~2007 inicial, open source ~2014.
- Paper canónico: Kuz+Liu+Gorton+Heiser, *"CAmkES: A component model for secure microkernel-based embedded systems"*, Journal of Systems and Software 80(5), 2007. [DOI](https://doi.org/10.1016/j.jss.2006.08.039).
- Docs oficiales: [docs.sel4.systems/projects/camkes](https://docs.sel4.systems/projects/camkes/).

### Qué resuelve

Un sistema seL4 "desnudo" requiere un *root server* que (1) reciba todas las caps de untyped al boot, (2) decida cómo crear procesos, (3) retype memory en los objetos necesarios, (4) construya manualmente las CSpace de cada proceso child, (5) distribuya caps entre ellos para IPC. Escribir esto a mano para un sistema con 20 componentes = ~5000 LOC de boilerplate C por sistema.

CAmkES genera ese boilerplate desde una descripción declarativa.

### Los tres lenguajes

1. **ADL** (Architecture Description Language) — declara componentes + conexiones a nivel sistema:
   ```camkes
   component Producer {
       control;
       uses DataInterface d;
   }
   component Consumer {
       provides DataInterface d;
   }
   assembly {
       composition {
           component Producer p;
           component Consumer c;
           connection seL4RPCCall conn(from p.d, to c.d);
       }
   }
   ```
2. **IDL4** (Interface Definition Language) — declara interfaces tipados:
   ```idl4
   procedure DataInterface {
       int send(in string data);
       void flush();
   };
   ```
3. **Templates** (Jinja2-style) — expanden cada componente + conexión en glue C. Hay templates built-in para `seL4RPCCall`, `seL4Notification`, `seL4SharedData`, `seL4RPCDataport`, etc. Puedes escribir templates custom.

### Pipeline de build

```
   *.camkes (ADL)  + *.idl4
         │
         ▼
   camkes parser (Python)
         │
         ▼
   AST in memory  ─────────────────► CapDL spec
         │                             (text)
         ▼
   Template engine (Jinja)
         │
         ▼
   generated C glue    per component + per connection
         │
         ▼
   component C code + generated → linked to libsel4 + root server
         │
         ▼
   capdl-loader (root server) reads CapDL + loads all components
         │
         ▼
   seL4 kernel + userland image ready to boot
```

### CapDL — Capability Distribution Language

- [docs.sel4.systems/projects/capdl](https://docs.sel4.systems/projects/capdl/).
- **CapDL** es un formato textual que describe *el estado inicial del sistema de caps*: qué CNodes existen, qué slots contienen qué caps, qué TCBs están bindeados a qué VSpace, etc.
- El `capdl-loader` es un programa userland (escrito en C, ~5 KLOC) que corre al boot y traduce el CapDL a una secuencia de syscalls `Untyped_Retype` + `CNode_Copy/Mint/Move` que construyen el estado.
- CapDL es verificable — la intended capability distribution es una prueba *matemática* de la spec, y el loader puede en principio ser verificado contra ella (Andronick et al, *"Large-Scale Formal Verification in Practice: A Process Perspective"*, ICSE 2012, describe el effort).

### Connection types típicos

| Connection | Semántica | Template genera |
|---|---|---|
| `seL4RPCCall` | sync RPC, client blocks for reply | endpoint shared, badge per client, marshalling C functions |
| `seL4Notification` | async signal | notification shared, signal side + wait side |
| `seL4SharedData` | shared memory region | frame shared, mapped into both VSpaces |
| `seL4RPCDataport` | RPC + shared dataport (big payloads) | endpoint + frame; IDL marshalls pointers to the dataport |
| `seL4GlobalAsynch` | multi-producer async | notification + dataport + atomic counters |
| `seL4TimeServer` | timer subscription | notification + timer server component |

### CAmkES VMM — Virtual Machine Monitor

- Variante: ejecutar Linux como guest dentro de un componente CAmkES.
- Usado en **automoción** (cluster + infotainment en el mismo SoC), **aviónica** (cockpit display + flight-critical apps), **hypervisors militares**.
- Código: [github.com/seL4/camkes-vm](https://github.com/seL4/camkes-vm). Linux corre en EL1 (ARM) o ring 1 (x86_64) bajo seL4; VMM en el componente CAmkES hace passthrough de devices vía VM IRQ injection + shadow page tables.
- HENSOLDT Cyber lo usa en su OS **TRENTOS** para despliegues militares.

### Las tensiones honestas

- CAmkES es **static** — la composición se fija en build time. Añadir/quitar componentes en runtime requiere rebuild image.
- Para dinámica, existe el sucesor **Microkit** (antes llamado seL4-CoreOS o seL4 Core Platform) — sel4.systems 2023+. Más simple (menos generated code), más cercano a "metal" — target: sistemas embedded que no quieren el overhead CAmkES. [docs.sel4.systems/projects/microkit](https://docs.sel4.systems/projects/microkit/).
- **CapDL + Microkit** juntos son el futuro "moderno" post-2024 — CAmkES sobrevive por inercia en proyectos viejos.

---

## 6. MCS — Mixed Criticality Scheduling

### El problema

En real-time systems se ejecutan tareas de múltiples *criticality levels*: una tarea flight-critical (DO-178C Level A) convive con una tarea UX-nivel (Level E). En el kernel original seL4 el scheduler era round-robin con prioridades — pero si una tarea de baja criticality *monopolizaba* la CPU (loop infinito o bug), no había mecanismo kernel-level para *presupuestar* su tiempo. Usar priority inversion evita problemas parcialmente, pero no es suficiente para prueba DO-178C.

### La solución — SchedContext as capability

- Anna Lyons, PhD UNSW 2019: *"Scheduling Contexts and the MCS Extension"*. [thesis PDF](https://trustworthy.systems/publications/papers/Lyons_19.pdf).
- Idea: **separar** "qué ejecutar" (TCB) de "cuánto tiempo puedes ejecutar" (SchedContext). Un TCB es un thread; un SchedContext es un *presupuesto* (budget + periodo de refill + prioridad + core target).
- Un TCB ejecuta sólo mientras tiene un SchedContext *bindeado*. Sin SchedContext → no corre. Con presupuesto agotado → se suspende hasta el próximo refill.

### Semántica

- **Budget** `b` nanosegundos, **period** `p` nanosegundos. Invariante: el thread no ejecuta más de `b` ns dentro de cualquier ventana de `p` ns.
- **Refill queue**: al no-preemptible-boundary el kernel gestiona una lista de *refills* — entradas `(time, amount)` que indican cuándo el SchedContext recibe cuánto budget. Modelo *sporadic server* con refills dispersos — contrasta con *deferrable server* más simple.
- Cuando el budget se agota → el kernel emite un `timeout fault` al TCB. El TCB puede tener registrado un *timeout fault handler* (un endpoint cap); o si no, se suspende simplemente.

### Donation

- Cuando `A` hace `Call(B)` con SchedContext donation habilitada, el SchedContext de `A` se presta a `B` durante la ejecución del Call. `B` ejecuta con el budget/prioridad de `A` — patrón *helping*.
- Al `Reply`, el SchedContext vuelve a `A`.
- Efecto: servicios "pasivos" no necesitan su propio budget — corren sólo cuando un cliente los *activa*.
- Resuelve priority inversion *estructuralmente*: el servicio hereda la prioridad del cliente por construcción.

### Verificación

- **Kovalev+Klein+Sewell et al** 2024 — *"Verifying the MCS Extensions of seL4"*, CPP 2024. Functional correctness proof del MCS branch. 
- Aún no hay info-flow ni timing proof para MCS 2026 — cost es re-hacerlos sobre el kernel modificado. En roadmap Foundation.

### Para ALZE

MCS es el patrón interesante — separar "quién" de "cuánto" en el scheduler. En ALZE hobby, no aplica v1-v2 (no hay real-time). Pero si en v3 ALZE quisiera rodar algo con hard-real-time guarantees, copiar el *pattern* de SchedContext-as-capability sería el move correcto — mucho más limpio que cgroups+CFS+cpu.cfs_quota.

---

## 7. Plataformas target 2026

### ARM — primary

- **ARMv7-A** (Cortex-A7/A9/A15): primer full-verified port 2011.
- **ARMv8-A / aarch64**: full-verified 2019 (functional correctness + integrity + info-flow; timing parcial).
- **ARMv8-R** (real-time profile, no MMU sino MPU): port "in progress" 2026.
- Boards soportados: Raspberry Pi 3/4/5, TX1/TX2, Odroid, i.MX6/7/8, HiKey, Sabre Lite, ZCU102 (Xilinx Zynq UltraScale+ para FPGA+SoC), Qualcomm DragonBoard.

### RISC-V — full-verified 2024

- **rv64imac** — Hex-Five + Proofcraft + seL4 Foundation, verificación completa funcional anunciada Julio 2024. Anuncio: [sel4.systems/news/2024-07](https://sel4.systems/news/2024-07/).
- Boards: SiFive HiFive Unleashed + Unmatched, Polarfire SoC, Allwinner D1 (single-core).
- Info-flow + binary proofs en roadmap 2025-26.

### x86_64 — less complete

- Functional correctness: ✅ desde 2018 (Paolino+Klein *"Porting seL4 to x86_64"*, 2018).
- Integrity: ✅.
- Info-flow: ❌ (el paper original assumes ARM-like memory model; x86 TSO requiere re-argumentar).
- Binary validation: ❌ (la infra Sewell 2013 es ARM-only; re-portarla a x86 ISA es trabajo serio no empezado).
- Timing: ❌ (pipeline x86 es too variable — Intel ya dijo que no suministra WCET bounds for modern Xeons).
- Uso real x86_64: VMMs desktop/server (CAmkES VM), research. No se despliega en avión/coche sobre x86.

### Qualcomm Snapdragon

- Android pilot 2017 con Qualcomm — seL4 como TrustZone-lite guest. No entró en producto comercial.
- **Motorola Edge 2026**: rumor (no confirmado por Motorola) de que usan un seL4-derived microkernel en el secure element. Sin fuente oficial, no confirmable.

### Xilinx Zynq / PolarFire (SoC+FPGA)

- Despliegue avionica real — Rockwell Collins + Boeing prototipos. El Zynq tiene un ARM Cortex-A9 o A53 cluster acoplado a FPGA; seL4 corre en el ARM, FPGA hace aceleración + I/O.

### HENSOLDT Cyber — sovereign CPU

- [hensoldt-cyber.com](https://www.hensoldt-cyber.com/).
- Compañía alemana de defensa (HENSOLDT = ex-Airbus Defence Electronics division spin-off 2017). Construye:
  - **MiG-V** — RISC-V64 rv64imac diseñado in-house, *verifiable hardware* (parcialmente formalizado con Kyber/Isabelle).
  - **TRENTOS** — OS sobre seL4 para despliegues militares europeos. Primera target: radares y sistemas de comunicación táctica.
- Miembro fundador seL4 Foundation. Principal financiador de verification en 2024-26.

### Boeing + Lockheed — aerospace

- Boeing: **UH-60 Black Hawk** autonomy kit (piloto Optionally Piloted Black Hawk, DARPA ALIAS/MATRIX program) usa seL4 + CAmkES para el autopilot bus. Demostración red-team 2018.
- Lockheed Martin: AH-64 Apache retrofit research (no confirmado en producción). [Publications](https://sel4.systems/About/).
- **Gulfstream** (unconfirmed) — reportes de aislamiento seL4 para cabin electronics.

### Automoción

- **Volvo Cars** / **Volvo Trucks** — piloto 2022 de gateway ECU en seL4 (no production known 2026).
- **Audi/VW** — proyectos de investigación con TU München + Fraunhofer usando seL4.
- **Daimler Trucks** — similar al anterior.
- La narrativa general: cluster + ADAS + infotainment + gateway consolidation en un SoC multi-core, con seL4 como hypervisor y CAmkES separando los dominios. Linux corre en un componente para infotainment; un RTOS o directamente CAmkES components para safety-critical.

---

## 8. seL4 Foundation 2026 — governance + members

### Estructura

- Nonprofit, Delaware-registered, con membership tiers:
  - **Platinum** (~$300k/yr): board seat, vote on major decisions.
  - **Gold** (~$100k/yr): technical steering committee seat.
  - **Silver** (~$20k/yr): observer + voting on minor proposals.
  - **Associate / Academic**: reduced fee for universities + research orgs.
- [sel4.systems/Foundation/members](https://sel4.systems/Foundation/members/).

### Miembros conocidos 2026 (no lista oficial completa — la web los lista)

- **HENSOLDT Cyber** — Platinum. Principal financiador técnico post-2022.
- **Ghost Locomotion** (ex, desaparecida 2023) — fue Platinum, sponsored RISC-V port.
- **UNSW Sydney** + **Proofcraft** — académico/research, primeros commiters.
- **Boeing** — Gold.
- **Lockheed Martin / GD Mission Systems** — Gold.
- **Cog Systems** (Sydney, ex-OK Labs linaje) — Platinum. Despliega seL4 comercial en ruggedized phones + tablets para militar/first-responder.
- **Raytheon BBN** — Gold.
- **NIO** (Chinese EV maker) — Silver?, reportado en 2022 kickoff, unclear status.
- **DornerWorks** — embedded integrator, Grand Rapids MI. Builds seL4 + FreeRTOS + Linux hypervisors para customers.
- **Galois Inc** — Portland OR. Associate. DARPA HACMS partner.
- **Data61 / CSIRO** — founding, still technical contributor.
- **Colias Group**, **kry10** (NZ): smaller commercial users.

### Summits

- Anual, desde 2020. Ubicaciones: Sydney (2020 virtual), Munich (2022, HENSOLDT), Sydney (2023), Minneapolis (2024, invitado por DoD research). 2026: propuesto Berlin.
- Talks disponibles en [YouTube seL4 Foundation channel](https://www.youtube.com/@seL4).

### Funding

- No público-exacto pero Heiser 2024 summit keynote: "foundation runs a 7-figure annual budget, 100% from member dues + grants; no commercial/licensing income". Grants notable: AFRL, DARPA (históricamente HACMS), NSF (académico), ERC (European).

---

## 9. Limitaciones honestas

### Lo que la prueba NO cubre

1. **Hardware bugs**. Spectre, Meltdown, L1TF, MDS, Zenbleed — ningún proof Isabelle sobre C/binario puede cubrir un microarchitectural side-channel. Heiser 2018: "seL4 proofs are conditional on the hardware behaving as its ISA documents; Spectre breaks the assumption". Mitigación: mode switches con full flushing (coste en ciclos), disable SMT, use newer CPUs con hardware mitigations. En plataformas HENSOLDT MiG-V el riesgo se elimina diseñando el core desde cero con *constant-time guarantees* — ahí la prueba es más robusta.

2. **Bootloader + firmware**. U-Boot, UEFI, PSCI, device tree parser, early-boot C code — todo outside de la prueba. Si bootloader loads un binario modificado, el kernel "verificado" ya no es el kernel cargado. Mitigación: Secure Boot + measured boot (TPM-attested).

3. **Compilador**. GCC no verificado en general (CompCert sí). Binary validation (PLDI 2013) cubre el *output* específico del compilador para *este commit* — no el compilador. Cada nueva versión gcc requiere re-run del validator. Intento de usar CompCert oficialmente: descartado por performance (~30% slower) y por no tener todos los atributos GCC que seL4 usa.

4. **Drivers externos**. Drivers de userland (PCI bus, USB, Ethernet, video, audio) son componentes userland normales — sin verificación. Un driver bug no rompe el kernel, pero sí rompe al sistema completo. CAmkES + IOMMU limita el daño (driver no puede DMA fuera de su dominio asignado).

5. **Timing side-channels (full)**. El WCET bound 2017 cubre *tiempo máximo* — pero no necesariamente *non-interference temporal* (que el tiempo que tarda A ejecutando no revele info a B sobre el estado de A). Info-flow proof 2013 cubre storage channels pero no timing en todas las configuraciones. Paper Ge+Yarom+Heiser, *"Time Protection: the missing OS abstraction"*, EuroSys 2019: propone explicit *time partitioning* — sharing-aware cache coloring, flush caches on context switch, separate cores por dominio de seguridad. Implementado parcialmente en mainline 2021+.

6. **DMA sin IOMMU**. Un device con DMA unrestricted lee/escribe RAM arbitrariamente. Solo mitigable con IOMMU (SMMU en ARM, VT-d en x86, IOMMU en RISC-V). Caps `IOSpace` en seL4 configuran el IOMMU, pero la prueba asume que el hardware IOMMU funciona como documentado.

7. **Glitch attacks / fault injection**. Voltage glitching, clock glitching, laser fault injection en silicio — outside software proofs. Mitigación: hardware tamper-resistance, redundancia criptográfica.

8. **Multicore y memory model**. La verificación asume un *sequentially consistent* (SC) view del kernel heap, que en ARMv8 + x86_64 + RISC-V64 *no* es SC natively. Mitigación: el kernel es *single-threaded* internamente (big kernel lock), y los memory barriers explícitos en entry/exit aseguran visibility. Pero esto limita scaling: seL4 no hace fine-grained locking dentro del kernel. MCS permite scheduler decisions per-core con sched contexts separados, pero cross-core IPC sigue siendo costoso.

### Lo que la prueba SÍ cubre

- Todas las *funcional* traces del kernel son ejecuciones permitidas por la spec abstracta.
- Sin write-cap → objeto intacto (integrity).
- Bajo policies correctamente configuradas → no-interference entre dominios.
- WCET acotado por una constante computada analíticamente (ARM).

### Qué significa "verified" honestamente

En palabras de Heiser 2022 en summit: "*formal verification is not 'bug-free'. It's 'the set of behaviors matches the spec'. If the spec is wrong, the proof is wrong. If the assumptions fail, the proof is void. What it buys you is: zero implementation bugs in kernel C code relative to the spec, which is the vast majority of what can go wrong*".

---

## 10. ALZE applicability — 3 niveles

### v1 — Read the manual, don't code

- [seL4 Reference Manual](https://sel4.systems/Info/Docs/seL4-manual-latest.pdf) — ~120 páginas. Legible en 1-2 tardes.
- Leer [Heiser whitepaper 2020](https://sel4.systems/About/seL4-whitepaper.pdf) — ~20 páginas, orientado a non-experts.
- Leer [Trustworthy Systems publications page](https://trustworthy.systems/publications/) — navegar papers por año.
- Clone [github.com/seL4/seL4-tutorials](https://github.com/seL4/seL4-tutorials), hacer tutoriales 1-5 (ipc, untyped, capabilities, mapping, threads). Tiempo estimado: 8-12 h.
- *Output:* comprensión profunda del modelo sin escribir nada. Permite hablar con autoridad y tomar decisiones de diseño informadas para ALZE.

### v2 — Copy CNode + endpoint IPC, ~2-3k LOC unverified

Pattern copy-paste estructural, C99, en ALZE:

- **`kobj.h`**: tagged-union kernel objects — `enum kobj_type { KOBJ_CNODE, KOBJ_ENDPOINT, KOBJ_NOTIFICATION, KOBJ_TCB, KOBJ_VSPACE, KOBJ_UNTYPED }`. Struct base + switch por tag.
- **`cnode.c`**: ~300 LOC. Array of slots; `cnode_lookup(root, cptr, depth) → slot*` con guard + radix. `cnode_copy`, `cnode_mint`, `cnode_move`, `cnode_delete`. *No* `revoke` recursive todavía (es lo caro) — hazlo simple: delete slot = invalidate slot; caps derived rastreadas en una lista enlazada, deleted parent → mark descendants orphaned pero no los destruyas. v2.5 añade revoke completo.
- **`endpoint.c`**: ~400 LOC. `struct endpoint { state, thread_queue }`. `ep_send`, `ep_recv`, `ep_call`, `ep_reply`. Badge en la cap (8-byte badge field en slot).
- **`notification.c`**: ~200 LOC. Bitmask 64 bits.
- **`ipc_fastpath.S`**: ~100 LOC asm x86_64. Condiciones simples (4 registers, rx waiting, prio match). Slowpath en C.
- **`untyped.c`**: ~250 LOC. `ut_retype(ut, type, size_bits, dest, ...)` — bump allocator dentro del untyped. Tracked como linked list de child regions.
- **Syscall entry**: unifier que valida handle → cap en CSpace del thread, valida rights, despacha a op.

Total estimado **~2000-3000 LOC**. 1-2 meses solo-dev part-time. Unverified pero *estructuralmente correcto*.

Beneficios inmediatos ALZE:
- Todo el kernel deja de usar "UID=0 implícito". Autoridad = tener la cap.
- IPC clean, testeable, fastpath medible.
- Drivers userland posibles (driver = thread con caps a MMIO frames + IRQHandler).
- Revocation parcial desde v2; completa v2.5.

### v3 — Aspirational partial verification

*Solo si ALZE tiene tiempo sobrado y el dev se interesa en formal methods por aprender, no por deliverable*.

- **No intentar Isabelle/HOL refinement proof completo**. Costo 10+ py, inalcanzable.
- **Sí considerar**:
  - **Astrée** (AbsInt, Francia) — static analysis abstracto. Prueba ausencia de undefined behavior (div by zero, array OOB, overflow, null deref) en C embedded code. Coste: licencia comercial (~decenas de k€ / año) o académica. Outputs: warnings categorizados, false positives normales pero aceptables. Usado en Airbus A380 flight control SW. [absint.com/astree](https://www.absint.com/astree/).
  - **Frama-C** con WP plugin — deductive verification con contracts ACSL. Open source, LGPL. Menos automatizado que Astrée pero gratis + extensible. Prueba específicas funciones según contratos que tú escribes. Buen fit para una **función crítica aislada** — ej. `ipc_fastpath`, `cnode_lookup`. Tiempo: días-semanas por función. [frama-c.com](https://frama-c.com/).
  - **CBMC** (Bounded Model Checker, Diffblue / Oxford) — unwinds loops N veces y chequea invariantes con SAT/SMT. No completo pero encuentra bugs reales rápido. Open source. [cprover.org/cbmc](https://www.cprover.org/cbmc/).
  - **MIRAI** (Facebook/Meta) — abstract interpreter para Rust. Si algún día ALZE migra kernel a Rust, este es el primer paso.
- **Objetivo realista v3**: Frama-C + WP sobre 2-3 funciones críticas (ipc fastpath, cnode lookup, untyped retype). Demuestra ausencia de UB + conforme a contract. No es refinamiento pero es *mucho mejor* que fuzz testing.
- **Costo estimado**: 3-6 meses part-time para familiarizarse con Frama-C + verificar 3 funciones.

### v4 — nunca

Full Isabelle/HOL refinement proof de un kernel ALZE completo. No para solo-dev, no para hobby, no para 2026.

---

## 11. Nota final — honesta

Heiser 2022 summit: **"2 to 3 engineer-years per kLOC of kernel C"** para verification nueva (desde cero). Cita literal traducida: "si mañana escribes un kernel de 15 kLOC y quieres verificarlo al nivel seL4, presupuesta 30-45 persona-años. Esto es la versión actualizada del cálculo 2009, que fue 25 py para 8.7k. Hemos mejorado herramientas pero también el estándar ha subido".

**Conclusión para ALZE (solo-dev, 1-2 h/día, 2026)**:

- **Verificación formal al nivel seL4 es inalcanzable**. No es cuestión de inteligencia o esfuerzo — es cuestión de person-years absolutos. Un solo dev part-time entregaría ~0.2 py/año. Verificar ALZE requiriría 100+ años calendario.
- **Los patrones de seL4 son copiables** en C99 en ~3k LOC sin verificación. Los beneficios estructurales (no ambient authority, IPC clean, fastpath medible, drivers userland aislados) se obtienen con el *diseño*, no con la prueba.
- **La prueba es el lujo institucional**. HENSOLDT + Boeing + Foundation financian ~1M€/año para mantenerla. ALZE no.
- **Estructura > prueba** para hobby OS. Copiar CNode, Untyped, endpoints, badges, rights bitmask es copiar las *decisiones arquitectónicas correctas*. La prueba formal solo certifica que esas decisiones se implementaron sin bugs relativos a la spec — necesario para aviónica, irrelevante para research/hobby.

**Path ALZE confirmado** (consistente con R3 concl):
1. **v1 (hoy)**: cerrar P0 blockers (IDT, SMP, locks). Leer seL4 manual, hacer tutoriales.
2. **v2 (2026-Q3/Q4)**: copiar patrón Zircon-lite / seL4-lite (handles + rights + endpoints + fastpath) en C99. ~3k LOC nuevos.
3. **v3 (2027+, opcional)**: Frama-C + WP sobre 2-3 funciones críticas. Research-level.
4. **v4 (nunca)**: full refinement proof.

La verificación seL4 es **inspiradora estructuralmente, pero no replicable operacionalmente** por un proyecto solo-dev. Lo que sí es replicable: el *diseño* que la hace verificable (no allocator dinámico, no concurrencia intra-kernel, subset C restringido, objetos fixed-size, Untyped/Retype en userland). Ese diseño por sí solo hace ALZE más testeable, más mantenible, y más cerca de "verificable-si-alguna-vez-hubiera-equipo".

---

## Referencias primarias (autor year venue URL)

**Kernel + verification (papers)**:
- Klein, Elphinstone, Heiser, Andronick, Cock, Derrin, Elkaduwe, Engelhardt, Kolanski, Norrish, Sewell, Tuch, Winwood — *"seL4: Formal Verification of an OS Kernel"*, SOSP 2009. [PDF](https://sigops.org/s/conferences/sosp/2009/papers/klein-sosp09.pdf) / [DOI](https://dl.acm.org/doi/10.1145/1629575.1629596).
- Klein et al — *"Comprehensive Formal Verification of an OS Microkernel"*, TOCS 32(1), Feb 2014. [PDF](https://sel4.systems/Research/pdfs/comprehensive-formal-verification-os-microkernel.pdf).
- Cock, Klein, Sewell — *"Secure Microkernels, State Monads and Scalable Refinement"*, TPHOLs / ITP 2008. [PDF](https://trustworthy.systems/publications/nicta_pubs/6223.pdf).
- Sewell, Myreen, Klein — *"Translation Validation for a Verified OS Kernel"*, PLDI 2013. [PDF](https://trustworthy.systems/publications/nicta_full_text/6449.pdf).
- Murray, Matichuk, Brassil, Gammie, Bourke, Seefried, Lewis, Gao, Klein — *"seL4: from General Purpose to a Proof of Information Flow Enforcement"*, IEEE S&P 2013. [PDF](https://trustworthy.systems/publications/nicta_full_text/6464.pdf).
- Sewell, Kam, Heiser — *"High-Assurance Timing Analysis for a High-Assurance Real-Time Operating System"*, RTSS 2017. [PDF](https://trustworthy.systems/publications/nicta_full_text/10131.pdf).
- Blackham, Shi, Chattopadhyay, Roychoudhury, Heiser — *"Timing analysis of a protected operating system kernel"*, RTSS 2011. [PDF](https://trustworthy.systems/publications/nicta_full_text/4901.pdf).
- Ge, Yarom, Cock, Heiser — *"Time Protection: the missing OS abstraction"*, EuroSys 2019. [PDF](https://trustworthy.systems/publications/csiro_full_text/Ge_YCH_19.pdf).
- Lyons — *"Scheduling Contexts and the MCS Extension"*, PhD thesis UNSW 2019. [PDF](https://trustworthy.systems/publications/papers/Lyons_19.pdf).
- Kovalev, Klein, Sewell et al — *"Verifying the MCS Extensions of seL4"*, CPP 2024.
- Klein, Elphinstone, Heiser — *"Mixing and matching: Heterogeneous systems with verified components"*, HotOS 2013 (extrapolation to multi-kernel). [archive](https://trustworthy.systems/publications/papers/Klein_EH_13.pdf).

**CAmkES + Microkit**:
- Kuz, Liu, Gorton, Heiser — *"CAmkES: A component model for secure microkernel-based embedded systems"*, J. Systems & Software 80(5), 2007. [DOI](https://doi.org/10.1016/j.jss.2006.08.039).
- seL4 Microkit docs — [docs.sel4.systems/projects/microkit](https://docs.sel4.systems/projects/microkit/).
- CAmkES docs — [docs.sel4.systems/projects/camkes](https://docs.sel4.systems/projects/camkes/).
- CapDL docs — [docs.sel4.systems/projects/capdl](https://docs.sel4.systems/projects/capdl/).

**Manuales oficiales**:
- seL4 Reference Manual (current) — [PDF latest](https://sel4.systems/Info/Docs/seL4-manual-latest.pdf).
- Heiser — *"The seL4 Microkernel: An Introduction"*, whitepaper 2020. [PDF](https://sel4.systems/About/seL4-whitepaper.pdf).

**L4 linage**:
- Liedtke — *"Improving IPC by Kernel Design"*, SOSP 1993. [DOI](https://dl.acm.org/doi/10.1145/168619.168633).
- Liedtke — *"On μ-Kernel Construction"*, SOSP 1995. [archive](https://os.inf.tu-dresden.de/papers_ps/l4-95.pdf).
- Heiser, Elphinstone — *"L4 Microkernels: The Lessons from 20 Years of Research and Deployment"*, TOCS 34(1), 2016. [PDF](https://trustworthy.systems/publications/csiro_full_text/Heiser_Elphinstone_16.pdf).

**Industrial deployment**:
- HENSOLDT Cyber / TRENTOS — [hensoldt-cyber.com](https://www.hensoldt-cyber.com/).
- DARPA HACMS / Boeing Little Bird — [DARPA page archive](https://web.archive.org/web/2022/https://www.darpa.mil/program/high-assurance-cyber-military-systems).
- seL4 Foundation — [sel4.systems/Foundation](https://sel4.systems/Foundation/).
- seL4 Foundation member list — [sel4.systems/Foundation/members](https://sel4.systems/Foundation/members/).
- seL4 Summit 2024 recordings — [YouTube seL4 channel](https://www.youtube.com/@seL4).
- seL4 2024-07 RISC-V announcement — [sel4.systems/news/2024-07](https://sel4.systems/news/2024-07/).

**Proofcraft + tools**:
- Proofcraft — [proofcraft.systems](https://proofcraft.systems/).
- l4v verification repository — [github.com/seL4/l4v](https://github.com/seL4/l4v).
- Isabelle/HOL — [isabelle.in.tum.de](https://isabelle.in.tum.de/).
- Frama-C (alt for v3) — [frama-c.com](https://frama-c.com/).
- Astrée (alt for v3) — [absint.com/astree](https://www.absint.com/astree/).
- CBMC (alt for v3) — [cprover.org/cbmc](https://www.cprover.org/cbmc/).
- CompCert (not used by seL4 but referenced) — [compcert.org](https://compcert.org/).

**Archives (fallback)** — todos los links de sel4.systems y trustworthy.systems están cacheados en web.archive.org con wildcard `web.archive.org/web/2024*/...`. github.com es estable.
