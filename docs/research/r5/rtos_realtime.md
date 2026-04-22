# RTOS + Real-Time — Deep Dive para ALZE OS

**Round:** R5
**Fecha:** 2026-04-22
**Scope:** sistemas operativos de tiempo real (RTOS) + Linux PREEMPT_RT + teoría de scheduling RT + primitives lock-free vs RT-safe. Objetivo: entender la **frontera entre un kernel general-purpose (lo que ALZE es) y un RTOS (lo que ALZE no es)** — qué se puede *aprender* sin pivotar de género, qué exige un rewrite, y qué no merece la pena tocar.
**Tesis de cierre (spoiler):** un RTOS no es "un kernel general-purpose con menos features". Es un **género distinto de OS**, con invariantes de diseño opuestos (determinismo > throughput, tiempo peor caso > tiempo promedio). ALZE puede aprender patrones localizados (IRQ threading, priority inheritance, per-CPU runqueues), pero **"convertir ALZE en RTOS" es scope-pivot, no feature**.

---

## 0. Qué significa "real-time"

Dos definiciones operacionales que conviene separar antes de continuar:

1. **Soft real-time:** el sistema intenta cumplir deadlines; fallar ocasionalmente degrada QoS pero no es catastrófico. Ejemplos: streaming de audio/video, juegos, dashboards. Linux vanilla con tuning llega aquí.
2. **Hard real-time:** **perder un deadline = fallo de sistema**. Hay un **WCET (Worst Case Execution Time)** probado o calculado, y el scheduler garantiza schedulability. Ejemplos: ABS de frenos, control de vuelo, marcapasos, reactor nuclear SCRAM. Aquí es donde viven QNX, VxWorks, RTEMS, seL4-RT y PREEMPT_RT Linux bien tuneado.
3. **Firm real-time:** deadline missed = resultado se descarta pero el sistema sigue. Telemetría periódica, some robotics.

Un RTOS **no es necesariamente rápido** — es **predecible**. Un RTOS típico procesa menos throughput que Linux en el mismo hardware. La métrica que gana es **jitter** (variación del tiempo de respuesta), no ops/sec. Latencia *peor caso* < 10 µs es típico en RTOS; Linux vanilla puede ir bajo 1 µs en promedio pero tiene outliers de milisegundos.

**Ref conceptual:** Buttazzo G., "Hard Real-Time Computing Systems: Predictable Scheduling Algorithms and Applications", Springer 3ª ed. 2011. ISBN 978-1-4614-0676-1.

---

## 1. Zephyr Project

**Quién/cuándo:** Linux Foundation, lanzado 2016 como fork/reescritura de Wind River Rocket + Viper. BSD-licensed → Apache 2.0 desde 2017. Governance: board con Intel, NXP, Nordic, Antmicro, Nordic Semi, Google.

**Arquitectura:**
- Microkernel-ish monolítico (kernel + threads + IPC compilado en un solo binario, sin userspace separado a menos que uses userspace mode con MPU).
- Multi-arch: x86, Arm Cortex-M (todos), Cortex-R, Cortex-A (con limitaciones), RISC-V (rv32/rv64), Xtensa (ESP32), ARC, Nios II, Tensilica, SPARC.
- Scheduler: **prioridades fijas** con preemption (100 niveles). Cooperative + preemptible threads mezclables. Tickless cuando idle.
- Footprint: mínimo ~8 KB flash / 2 KB RAM (nRF52 minimal), típico 30-100 KB para app con BLE + sensores, puede escalar a MB con TCP/IP + filesystem + USB + Matter.
- Build: **West** tool (en Python, multi-repo) + **Kconfig** (estilo Linux) + **Device Tree** (literalmente `.dts` como Linux).

**Features notables:**
- SMP desde Zephyr 2.x (2020): global spinlock inicial, luego per-CPU runqueues + IPI.
- Memory protection: **Userspace mode** con MPU (Cortex-M) o MMU (Cortex-A, x86) — threads en user mode, syscalls trampolin. Aislamiento opcional, no por defecto.
- **Logging subsystem** con niveles compile-time (no runtime overhead).
- **Settings** subsystem: key/value persistente (flash, NVS, FCB).
- **Networking:** IPv4/IPv6 nativo, TCP/UDP, sockets BSD API, MQTT, CoAP, LwM2M, DTLS vía mbedTLS.
- **Matter** reference: oficialmente soportado, nRF Connect SDK usa Zephyr como base.
- **Bluetooth:** stack BLE completo controller + host (no es BlueZ, es nativo).
- **Shell:** REPL con comandos custom, usable vía UART/RTT.

**Ship examples:**
- Nordic nRF Connect SDK (nRF52/nRF53/nRF91) — desde 2019.
- Espressif ESP32 en Zephyr (no ESP-IDF) desde 2020.
- Google Nest Wifi Pro internals (rumor no confirmado oficialmente, pero Matter Reference).
- PineTime (smartwatch) opcional InfiniTime usa FreeRTOS, pero Zephyr disponible.

**Ref:** Zephyr Project, "Zephyr Documentation", https://docs.zephyrproject.org/latest/ . Archive: https://web.archive.org/web/2025*/docs.zephyrproject.org/ .
**Ref:** Rostedt S., Molnar I., "Threaded Interrupts in Linux" (influyó en Zephyr), LWN 2007, https://lwn.net/Articles/244829/ .
**Ref:** Zephyr Project, "West (Zephyr's meta-tool)", https://docs.zephyrproject.org/latest/develop/west/ .

**LOC aproximado (2025):** núcleo `kernel/` ~50 KLOC C, `arch/` ~80 KLOC, `drivers/` >300 KLOC, total >1.5 MLOC (pero cada app solo compila la fracción que usa).

---

## 2. FreeRTOS

**Quién/cuándo:** Richard Barry, 2003, empresa Real Time Engineers Ltd. Amazon adquirió (y a Barry) en Nov 2017 para impulsar AWS IoT. Licencia: MIT desde 2017 (antes un GPL-modificado).

**Arquitectura:**
- **Minimalismo:** kernel propio = ~9 000 líneas de C. Literalmente el RTOS más pequeño ampliamente desplegado.
- Single-file-per-arch port: `port.c` + `portmacro.h` (~1 KLOC).
- Sin MMU por defecto. Thread = task, cada task su stack.
- Scheduler: **prioridades fijas preemptivas** (0 = idle, hasta `configMAX_PRIORITIES-1`), opcional round-robin entre prioridad igual con time-slicing.
- **Tickless idle** opcional (`configUSE_TICKLESS_IDLE`).
- **SMP port oficial:** disponible desde 2023, GA 2024 en `v11.0.0` (Amazon fork). Antes, varios forks (Espressif ESP-IDF llevaba SMP desde 2016). Limitaciones: big kernel lock por defecto, fine-grained opcional.

**Primitivas IPC:**
- Queues (copy-by-value, FIFO o prioridad).
- Semaphores (binary, counting, mutex).
- **Mutex con priority inheritance** (ver sección 11).
- Event groups (24 bits de flags por grupo).
- Stream buffers + message buffers (single producer / single consumer, lock-free path optimizado).
- Direct-to-task notifications (más rápidas que semáforos — baipasean la cola).

**Compliance:**
- **MISRA-C 2012** compliant (verificado con PC-lint, documentado). Importante para cert automotive/medical.
- Kernel verificado con **CBMC** (bounded model checker) para properties críticas (ticks, queue invariants). Amazon publicó proofs en `CBMC-proofs/` del repo FreeRTOS.
- Se usa como base en certifiable derivatives: **SafeRTOS** (Wittenstein/HighIntegritySystems, IEC 61508 SIL 3, IEC 62304, FDA).

**Ship examples:**
- ESP-IDF (Espressif): FreeRTOS dual-core en todos los ESP32/S/C/H/P.
- Arduino-ESP32 (por extensión).
- NXP MCUXpresso SDK.
- Microchip Harmony.
- Millones de dispositivos IoT AWS Greengrass + AWS IoT Device SDK.

**Ref primaria:** Barry R., "Mastering the FreeRTOS Real Time Kernel: a Hands-On Tutorial Guide", Real Time Engineers 2016 PDF libre, https://www.freertos.org/Documentation/RTOS_book.html .
**Ref:** FreeRTOS, "FreeRTOS Kernel Documentation", https://www.freertos.org/features.html .
**Ref SMP:** AWS / FreeRTOS Kernel v11, https://www.freertos.org/2024/03/freertos-kernel-v11.html .

**LOC:** `tasks.c` ~4.4 KLOC, `queue.c` ~2.5 KLOC, `timers.c` + `event_groups.c` + `stream_buffer.c` ~2 KLOC cada, total kernel ~9-10 KLOC.

---

## 3. QNX Neutrino

**Quién/cuándo:** Gordon Bell + Dan Dodge, 1982, "QUNIX" → QNX 2 en 1984. QNX Software Systems (Ottawa). Adquirida por BlackBerry en 2010. Commercial, no open source, aunque hay **QNX Software Development Platform (SDP) 8.0** (2024) con non-commercial license para hobby.

**Arquitectura:**
- **Microkernel puro, ~60-100 KLOC** (Neutrino Core Kernel). Uno de los microkernels más pequeños ampliamente desplegados en producción.
- Todo lo demás (filesystems, red, drivers) corre como procesos en userspace comunicándose por **message passing síncrono** con priority inheritance (similares a seL4 Notifications pero más POSIX-y).
- **POSIX-certificado** — implementa PSE52 Realtime Controller profile y PSE54 Multi-Purpose Realtime.
- Multi-arch: x86_64, ARMv7/v8 (AArch64), antes PowerPC, MIPS, SH-4 (algunos retirados).
- Scheduler: FIFO + round-robin + **sporadic server** (capacity + replenishment) + **adaptive partition** (cada partición recibe % CPU garantizado, con stealing si hay holgura).
- **Resource manager framework:** cualquier proceso puede publicarse como "/dev/foo" y servir `open/read/write/ioctl` por messages — así se implementan drivers, filesystems, redes.

**Usos en producción (notables):**
- **BMW iDrive / Mercedes MBUX / Audi MMI / Ford Sync** — toda la infotainment tier-1 europea y muchas americanas corren QNX o Android-on-QNX hypervisor.
- **Cisco routers** (algunos) y ventanilla de red IOS.
- Sistemas de control de **reactores nucleares** (Westinghouse AP1000 algunos subsistemas).
- Dispositivos médicos: **Philips** scanners, **Stryker** quirúrgicos.
- Control de tráfico aéreo en algunos aeropuertos canadienses.
- Control industrial: **ABB**, **Emerson** DCS.

**Hypervisor:** QNX Hypervisor 2.x corre Android/Linux guests en partition, para seguir certificado el control mientras corre "entertainment" en best-effort.

**Ref:** BlackBerry QNX, "QNX Neutrino RTOS Documentation", https://www.qnx.com/developers/docs/ .
**Ref:** Hildebrand D., "An Architectural Overview of QNX", USENIX Workshop on Micro-kernels 1992, https://www.usenix.org/legacy/publications/library/proceedings/micro92/full_papers/hildebrand.pdf .
**Ref:** BlackBerry QNX SDP 8.0 release 2024, https://blackberry.qnx.com/en/products/foundation-software/qnx-sdp (archive fallback).

---

## 4. VxWorks

**Quién/cuándo:** Wind River Systems, 1987 (derivado de VRTX en los '80s). Desde 2009 subsidiaria de Intel; escindida 2018 (TPG Capital) y re-vendida 2023 a Aptiv PLC. Commercial, no open source.

**Arquitectura:**
- Monolítico tradicional en orígenes; modernizado a microkernel con user-mode processes desde VxWorks 6 (2003+) con **Real-Time Processes (RTP)**.
- Scheduler: prioridades fijas 0-255, preemptive, round-robin opcional, **POSIX scheduling policies** (SCHED_FIFO, SCHED_RR, SCHED_SPORADIC, SCHED_OTHER).
- Multi-arch: x86_64, ARMv7/v8, PowerPC (aún soportado para legacy aero/space), RISC-V (desde VxWorks 7 SR0680, ~2022).
- **WIND Kernel** + Board Support Packages (BSPs) personalizadas por OEM.

**Usos icónicos:**
- **Mars Pathfinder** (1997) — famoso por el bug de priority inversion (ver sección 11), Glenn Reeves + Mike Jones.
- **Mars rovers Spirit + Opportunity + Curiosity + Perseverance** — VxWorks todos. Ingenuity helicóptero también.
- **James Webb Space Telescope** — control de instrumentos.
- **Boeing 787** (algunos buses).
- **BMW iDrive gen 1-3** (luego migró a QNX).
- **Cisco CRS-1** (algunos componentes de routing fabric).

**Long life cycle:** contratos aerospace/defense exigen soporte a 20-30 años. Wind River mantiene BSPs para CPUs descontinuadas (e.g. PowerPC 7xx/7xxx) mucho más allá de EOL del silicio.

**Ref:** Wind River, "VxWorks Documentation Portal", https://www.windriver.com/products/vxworks (archive: https://web.archive.org/web/2025*/windriver.com/products/vxworks).
**Ref histórica:** Reeves G.E., "What really happened on Mars Rover Pathfinder", RISKS Digest 1997, http://catless.ncl.ac.uk/Risks/19.49.html#subj1 .

---

## 5. RTEMS — Real-Time Executive for Multiprocessor Systems

**Quién/cuándo:** U.S. Army Missile Command + OAR Corp, iniciado 1988 (entonces "Real-Time Executive for Missile Systems" → rebautizado). Open source (modified GPL con kernel exception, permite binding estático a código propietario). Mantenido por comunidad + OAR + ESA + NASA.

**Arquitectura:**
- Single-address-space, multi-threaded. Sin MMU por defecto (pero soportada en algunos BSPs).
- Multi-arch: ARM, PowerPC, SPARC (LEON3/LEON4 específicamente — el CPU de ESA), x86, RISC-V, MIPS, Nios II, MicroBlaze, m68k, SH.
- Scheduler: priority-based con deterministic scheduler **O(1)**, opcional EDF y CBS (Constant Bandwidth Server) desde RTEMS 4.11.
- **SMP** desde RTEMS 5 (2020) — escalas hasta 32 CPUs.
- POSIX: parcial (pthread, semaphores, message queues, timers). No certified pero "compatible".
- **Classic API** propietaria además de POSIX — legado de VRTX-like, con `rtems_task_create`, `rtems_semaphore_obtain`, etc.

**Usos icónicos:**
- **ESA** (European Space Agency): casi todas las misiones desde 2000s usan RTEMS + LEON SPARC.
- **Mars Reconnaissance Orbiter** (NASA, algunos subsistemas).
- **MAVEN**, **InSight**, **Lunar Reconnaissance Orbiter**.
- **CERN** — control de algunos experimentos.
- **Korean Aerospace** satélites.

**Porqué en academia:** open source + predecible + multi-arch + certifiable (ECSS-E-ST-40C-compatible path con Coverity + LDRA tooling).

**Ref:** RTEMS Project, "RTEMS Documentation", https://docs.rtems.org/ .
**Ref histórica:** OAR Corp, "A Historical Perspective on RTEMS", https://devel.rtems.org/wiki/Developer .
**Ref aerospace:** ESA, "Using RTEMS on LEON processors", https://www.esa.int/Enabling_Support/Space_Engineering_Technology/Onboard_Data_Processing/Microprocessors .

---

## 6. PREEMPT_RT Linux — la larga marcha

**Historia comprimida:**
- 2004: Ingo Molnar publica el primer patch "Real-Time Preemption" para Linux 2.6.
- 2005-2023: serie de patches out-of-tree mantenidos por Molnar + Thomas Gleixner + Steven Rostedt + Sebastian Andrzej Siewior. Apoyo financiero de Linutronix (OSADL, Intel, IBM, Red Hat). LTS paralelos a los LTS vanilla.
- 2024: **PREEMPT_RT fusionado en mainline** — la última porción crítica (printk-rt, RCU boost completo) llega en Linux 6.12 (noviembre 2024). Después de 20 años.

**Qué hace PREEMPT_RT (técnicamente):**
1. **Hard-IRQ handlers convertidos a threads** (`threaded IRQs`). Por defecto ejecutan como tareas kthread con prioridad FIFO. Solo bottom-halves muy cortas quedan en hard-IRQ context. → un IRQ ya no bloquea al scheduler arbitrariamente.
2. **spinlocks → `rt_mutex`**: casi todos los `spin_lock()` del kernel se transforman en locks que soportan **priority inheritance** y son *preemptible*. Un thread esperando un spinlock puede ser preempted por otro RT task. Los auténticos spinlocks que siguen siendo no-preemptible se llaman `raw_spinlock_t` y se usan solo en paths muy cortos (scheduler guts, IRQ entry).
3. **Preemptible RCU** (McKenney): las read-side critical sections pueden ser preempted sin violar RCU grace periods. Ya existía como opción desde mucho antes, pero es default en RT.
4. **Priority inheritance** a través de rt_mutex, futex PI, pthread mutexes con `PRIO_INHERIT`.
5. **High-resolution timers** (hrtimers, McKenney + Gleixner 2007) — deadlines en nanosegundos.
6. **Deadline scheduler (SCHED_DEADLINE)**: implementa EDF + CBS desde Linux 3.14 (2014). Fuera de PREEMPT_RT pero sinérgico.
7. **NO_HZ_FULL** + tickless isolated CPUs: dedicar CPUs específicas a tasks RT sin interrupciones de timer en modo "nohz_full".
8. **CPU isolation** (`isolcpus=`, cpusets), IRQ affinity pinning, `irqbalance` desactivado.

**Latencias alcanzables (documentadas por OSADL):**
- **Vanilla Linux no-PREEMPT:** peor caso 1-10 ms en carga moderada, outliers de cientos de ms bajo IO pesado.
- **PREEMPT (voluntary):** peor caso 500 µs - 2 ms.
- **PREEMPT_RT sin tuning:** peor caso 100-300 µs típico en x86 modernos.
- **PREEMPT_RT + tuning (isolcpus, nohz_full, SMT off, SpeedStep off, C-states off, IRQ pinning, no SMM):** peor caso **< 30 µs** en Intel Xeon clase industrial, **< 15 µs** en algunos ARM Cortex-A72 con caches lockeadas.
- **Lowest documentado (OSADL monitor):** < 10 µs worst case sostenido en Beckhoff x86-64 industrial PCs con cycletest corriendo semanas.

**Trade-off honesto:** PREEMPT_RT reduce throughput 5-15% en benchmarks CPU-bound y I/O pesado. No es gratis. Las `rt_mutex` son más pesadas que spinlocks, los threaded IRQs hacen 2 context switches donde antes había 0.

**Refs primarias:**
- Molnar I., "Real-Time Preemption, -RT patchset", LKML Oct 2004, https://lwn.net/Articles/107269/ .
- Corbet J., "Realtime preemption, part 1", LWN 2005, https://lwn.net/Articles/107269/ .
- Corbet J., "The realtime preemption merge", LWN 2023 (al cerrar el hito), https://lwn.net/Articles/948333/ .
- Corbet J., "PREEMPT_RT is here", LWN Nov 2024, https://lwn.net/Articles/999355/ .
- McKenney P.E., "Is Parallel Programming Hard, and, If So, What Can You Do About It?", https://mirrors.edge.kernel.org/pub/linux/kernel/people/paulmck/perfbook/perfbook.html (cap sobre preemptible RCU).
- Gleixner T., Molnar I., "Realtime preemption and general purpose operating systems", OLS 2005, http://halobates.de/download/ols2005.pdf .
- OSADL, "Realtime Linux Latency Plots", https://www.osadl.org/Realtime-QA-Farms.osadl-qa-farm-realtime.0.html .

---

## 7. NuttX

**Quién/cuándo:** Gregory Nutt, 2007. Inicialmente BSD license. **Donado a Apache Foundation en 2020**, ahora Apache 2.0, top-level project como **Apache NuttX**.

**Arquitectura:**
- RTOS tipo Unix en miniatura: **POSIX + ANSI C** como primitives principales (a diferencia de FreeRTOS que tiene su propia API). `fork` real (en arch con MMU), `pthread`, `fopen`, `sockets` BSD.
- Multi-arch: ARM Cortex-M/R/A, RISC-V, Xtensa (ESP32), x86, AVR, z80, MIPS, SH, Zilog, Renesas RX.
- Flat vs protected vs kernel builds:
  - **Flat:** todo en un address space (como FreeRTOS).
  - **Protected:** kernel + user con MPU.
  - **Kernel:** MMU completa, user processes aislados.
- Scheduler: priority-based (256 niveles), RR, FIFO, sporadic. SMP desde 2019 con una master-CPU + spin_lock global por defecto, o fine-grained.
- **BINFS / ROMFS** filesystems, **SMARTFS** + **LittleFS** + **SPIFFS** para flash.
- Soporta **NSH** (NuttShell) — shell sh-like con mini-coreutils.

**Uso icónico:**
- **PX4 Autopilot** (drones, ArduPilot alternativa) — corre en Pixhawk hardware con STM32F7/H7. Vuelo controlado en real time.
- **Xiaomi** smart speakers (usa NuttX en Sonos-compatible devices según reports 2022).
- **Sony** Spresense (CXD5602 multi-core Cortex-M4F).
- **Samsung** varios (donated contributions significativas).

**Ref:** Apache NuttX Project, "NuttX Documentation", https://nuttx.apache.org/docs/latest/ .
**Ref:** Nutt G., "NuttX README", https://github.com/apache/nuttx/blob/master/README.md .
**Ref PX4:** PX4 Autopilot, "NuttX on PX4", https://docs.px4.io/main/en/concept/architecture.html .

---

## 8. Contiki-NG

**Quién/cuándo:** Adam Dunkels, **SICS** (Swedish Institute of Computer Science), 2002. Contiki original. **Contiki-NG** fork 2017, mantenido activamente por RISE SICS + académicos (InriaI, INESC TEC).

**Arquitectura:**
- Super-ligero: **< 10 KB ROM + 2 KB RAM** en micros de 8/16-bit.
- **Protothreads** (Dunkels 2005) — "threads" cooperativos que caben en stack único, usan un hack de `switch/case` con `__LINE__` para salvar PC entre yields. Zero stack por protothread. Limitación: variables locales no persisten entre yields (deben ser estáticas).
- **Event-driven kernel**: no threads reales, todo son callbacks y timers.
- **uIP** TCP/IP stack (Dunkels 2003) — probably smallest compliant TCP/IP ever (<5 KB).
- **Rime** + **6LoWPAN** + **IEEE 802.15.4** + **CoAP** nativos.
- Targets: wireless sensor networks (WSN), IoT edge. CPUs clásicos: MSP430, AVR, ARM Cortex-M.

**Estado 2026:** Contiki-NG sigue activo en niches específicos — investigación académica de WSN, algunos deployments industriales de smart meters y ambient sensors. No compite con Zephyr/FreeRTOS en mass-market IoT.

**Ref:** Dunkels A., Grönvall B., Voigt T., "Contiki - a Lightweight and Flexible Operating System for Tiny Networked Sensors", IEEE LCN 2004, https://dunkels.com/adam/dunkels04contiki.pdf .
**Ref protothreads:** Dunkels A., Schmidt O., Voigt T., Ali M., "Protothreads: Simplifying Event-Driven Programming of Memory-Constrained Embedded Systems", SenSys 2006, https://dunkels.com/adam/dunkels06protothreads.pdf .
**Ref:** Contiki-NG, "Contiki-NG Documentation", https://docs.contiki-ng.org/ .

---

## 9. Hobbyist / language-native RTOS ecosystems

### TinyGo

Go compiler fork para micros (LLVM backend). No es un RTOS sino un **runtime Go que corre en bare-metal** sobre targets como Cortex-M, AVR, WebAssembly, RISC-V. Incluye goroutines implementadas como protothreads (cooperative) + canales. Útil para hobby boards (Pine64 RISC-V, BBC micro:bit). **No es hard-RT** — el GC y el scheduler cooperativo lo descartan para crítico.
**Ref:** TinyGo Project, https://tinygo.org/docs/ .

### Embedded Rust (`embedded-hal`)

No es un RTOS; es una **abstracción de HAL** (Hardware Abstraction Layer) que estandariza traits para GPIO, SPI, I2C, UART, Timer, Delay. Crates ecosystem:
- **RTIC** (Real-Time Interrupt-driven Concurrency, antes "Real-Time For the Masses") — framework que usa prioridades de interrupt del NVIC de Cortex-M como scheduler, con análisis estático de data dependencies para garantizar lock-freedom. Per Johan Eriksson et al. Pragmatic para Cortex-M, **hard-RT apto** en su dominio.
- **Embassy** — async executor para micros, usa `async/await` de Rust con un executor custom. No garantiza hard-RT pero sí buena latency.
- **Drone OS** — alternativa con IRQ-threading model.
- **Tock OS** — MIT + Stanford, 2015+, Rust-native con **grants** (capability-like para apps) y tres-layer architecture (kernel Rust + capsules + apps). Orientado a IoT seguro.

**Ref RTIC:** Eriksson J., Lindgren P. et al., "Real-Time For the Masses", LCTES 2013, https://rtic.rs/2/book/en/ .
**Ref Tock:** Levy A. et al., "Multiprogramming a 64 kB Computer Safely and Efficiently", SOSP 2017, https://www.tockos.org/assets/papers/tock-sosp2017.pdf .
**Ref Embassy:** Embassy Project, https://embassy.dev/ .

### TinyCore — nota de desambiguación

"Tiny Core Linux" es una **distro Linux ligera** (11 MB), NO un RTOS. Sin relación con los RTOS arriba. Mencionada en la lista del prompt probablemente por confusión nomenclatural. Se omite aquí.

---

## 10. Real-Time Linux vs RTOS dedicado — cuándo cada uno gana

| Dimensión | RTOS (Zephyr/FreeRTOS/QNX/etc) | PREEMPT_RT Linux |
|---|---|---|
| **Latencia peor caso** | < 10 µs típico, < 2 µs en Cortex-M bare | < 30 µs tuneado, < 100 µs sin tuning |
| **Footprint mínimo** | 10 KB - 1 MB | ~50 MB kernel + rootfs |
| **Complejidad** | Orden mágnitudes menor | Kernel 35 MLOC + userspace |
| **POSIX compat** | Parcial (o ninguna) | Full glibc |
| **Networking** | Limitado, a veces sin TCP/IP full | Stack completo (todo) |
| **File systems** | FATfs, LittleFS, básicos | ext4, xfs, btrfs, ZFS, todo |
| **Tooling/IDE** | Vendor-specific, fragmentado | GCC/Clang + GDB + perf + bpftrace |
| **Ecosystem apps** | Escaso | Infinito |
| **Cert (aero/auto/med)** | Con effort, algunos pre-cert (SafeRTOS SIL3, VxWorks DO-178C) | Difícil — Linux no está pre-cert, Red Hat In-Vehicle trabaja en ello |
| **Update OTA** | Se diseña | Plug-and-play |
| **Seguridad** | Minimal attack surface | Gran superficie + hardening |

**Regla operacional:**
- ¿Tu deadline es **< 10 µs worst case** y el sistema es **resource-constrained** (< 1 MB RAM)? → RTOS.
- ¿Necesitás **TCP/IP completo + sistema de archivos + apps Linux + GUI** en 100 ms worst case? → PREEMPT_RT.
- ¿Estás entre ambos? Considera QNX (RTOS con POSIX y red completa) o Zephyr con networking (RTOS mid-size).

**Determinismo — la fuente de la divergencia:** un RTOS es determinista porque decide *diseñar fuera* las fuentes de jitter:
- Sin demand paging (todo locked en RAM).
- Sin disk caches.
- Allocator bounded (`O(1)` malloc o pools estáticos).
- Sin TLB shootdowns sorpresa (memoria física fija).
- Sin SMI/SMM (en aero/auto se apagan explícitamente).
- Preemption puntos deterministas.

Linux hereda décadas de optimización para throughput promedio y tiene todas esas fuentes de jitter "por defecto". PREEMPT_RT + tuning las mitiga pero no las elimina. Un RTOS nunca las tuvo.

---

## 11. Scheduling theory — RMA, EDF, priority inheritance

### Liu & Layland 1973 — el paper fundacional

**Liu C.L., Layland J.W., "Scheduling Algorithms for Multiprogramming in a Hard-Real-Time Environment", JACM Vol. 20 No. 1, Jan 1973, https://dl.acm.org/doi/10.1145/321738.321743** (archive: https://www.di.unipi.it/~acg/papers/Liu-Layland.pdf ).

Resultados clave:

1. **Rate Monotonic (RM) priority assignment** — asignar prioridad inversamente proporcional al período. **Teorema:** bajo RM estático con tasks periódicos independientes, el schedulability test es:
   $$ \sum_{i=1}^{n} \frac{C_i}{T_i} \leq n(2^{1/n} - 1) $$
   Donde C_i = WCET, T_i = período. Límite → ln(2) ≈ 0.693 cuando n → ∞. Si la utilización total ≤ ln(2), RM garantiza schedulability.

2. **Earliest Deadline First (EDF) dinámico** — escoger siempre la tarea con deadline más cercano. **Teorema:** EDF es óptimo en uniprocessor para tasks periódicos (alcanza utilización 100%). Bound: U ≤ 1.

Corolario: **EDF domina RMA en utilización**, pero es más costoso de implementar (hay que recalcular ordering dinámicamente) y tiene peor overload behavior (si te pasás del 100%, *todas* las tasks empiezan a fallar, no solo la menos prioritaria).

### Deadline Monotonic (DM) — Leung + Whitehead 1982

**Leung J., Whitehead J., "On the complexity of fixed-priority scheduling of periodic, real-time tasks", Performance Evaluation 2(4), 1982, https://doi.org/10.1016/0166-5316(82)90004-1** .

Generalización de RM para el caso deadline ≠ período: asignar prioridad por deadline relativo (no por período). DM domina RM cuando deadlines < periods.

### Priority inversion + priority inheritance

**Sha L., Rajkumar R., Lehoczky J.P., "Priority Inheritance Protocols: An Approach to Real-Time Synchronization", IEEE TC Vol. 39 No. 9, Sep 1990, https://doi.org/10.1109/12.57058** (archive: https://www.cs.cmu.edu/afs/cs/project/able/ftp/pip/pip.pdf ).

**Problema:** task H (alta prio) espera un mutex tomado por L (baja prio). Llega M (media prio) y preempta a L. H queda bloqueado indirectamente por M — **priority inversion unbounded**.

**Solución PIP (priority inheritance protocol):** cuando H se bloquea en un mutex tomado por L, L **hereda** la prioridad de H hasta que libere el mutex. Entonces M no puede preemptar a L. PIP resuelve pero no elimina chains (si L espera otro mutex, la herencia se propaga).

**Solución PCP (priority ceiling protocol):** cada mutex tiene una "ceiling priority" = max de las prios de tasks que pueden usarlo. Task solo adquiere mutex si su prioridad > ceilings de todos los mutexes ya tomados. Garantiza: no deadlocks, bloqueo limitado a 1 critical section, latencia acotada.

**Caso Mars Pathfinder 1997:** VxWorks tenía PIP disponible pero *no activado por defecto* en el mutex compartido entre el bus manager (alto) y un meteorological task (bajo). Un task medio bloqueaba al bajo, el alto timeoutea → watchdog → reset. Reeves + Jones lo diagnosticaron remotamente por JPL y activaron PIP vía uplink de comando. Lección: **default matters**.

**Ref:** Reeves G., "What really happened on Mars Rover Pathfinder", RISKS 19.49, 1997, http://catless.ncl.ac.uk/Risks/19.49.html#subj1 .

### SCHED_DEADLINE en Linux

Implementación de **CBS (Constant Bandwidth Server)** + **EDF** en Linux kernel 3.14 (2014). Primarios: Juri Lelli, Dario Faggioli, Luca Abeni.

Cada task: `runtime` (CPU budget por período) + `deadline` + `period`. EDF entre tasks SCHED_DEADLINE, estos preemptan a SCHED_FIFO/RR/NORMAL. Admission control: no deja crear tasks que excedan schedulability.

**Ref:** Lelli J., Scordino C., Abeni L., Faggioli D., "Deadline scheduling in the Linux kernel", Software: Practice and Experience 46(6), 2016, https://doi.org/10.1002/spe.2335 .
**Ref kernel:** Documentation/scheduler/sched-deadline.rst.

---

## 12. Lock-free ≠ real-time: la falacia frecuente

Un error común de hobbyist: "voy a usar Michael-Scott lock-free queue para mi kernel RT, no hay locks, será determinista". **Falso.**

### La jerarquía de progress guarantees (Herlihy + Shavit)

**Herlihy M., Shavit N., "The Art of Multiprocessor Programming", Morgan Kaufmann 2008/2020 (rev 2nd ed 2020). ISBN 978-0124159501.**

Progress guarantees, de más fuerte a más débil:

1. **Wait-free:** cada thread completa su operación en un número acotado de sus propios pasos, independientemente de la contención. Más fuerte posible. **RT-safe.**
2. **Lock-free:** **algún** thread progresa en cualquier momento, pero un thread individual puede sufrir livelock indefinido si otros compiten. Throughput bueno agregado; latencia individual **NO acotada**. **NO RT-safe** en el sentido hard.
3. **Obstruction-free:** un thread progresa si corre solo. Si hay contención puede livelock.
4. **Starvation-free locks:** eventualmente cada thread adquiere el lock, pero no hay cota. No es "lock-free" técnicamente.
5. **Deadlock-free locks:** algún thread progresa. Puede haber starvation individual.

**Ejemplo concreto — Michael-Scott queue:**
```
enqueue(x):
  node = new(x, null)
  loop:
    tail = Q.tail.load()
    next = tail.next.load()
    if tail == Q.tail.load():
      if next == null:
        if CAS(tail.next, null, node): break
      else:
        CAS(Q.tail, tail, next)  // help
    // retry unbounded
  CAS(Q.tail, tail, node)
```

En teoría lock-free — progreso global siempre. En la práctica un thread puede hacer **N retries** con N = número de threads contendiendo, y en el peor caso el CAS siempre lo pierde. Sin cota superior. Si tu task RT entra aquí con un deadline de 50 µs, ningún teorema te dice que saldrás a tiempo.

**Wait-free queue (Kogan-Petrank 2011):** real-time-safe pero mucho más complejo, añade un "helping mechanism" con anuncios. Overhead constante pero elevado.

### Conclusión operacional

Para RT:
- **Preferir locks con priority inheritance** bien entendidos (PIP/PCP) sobre lock-free "porque es rápido".
- **Lock-free es aceptable** si hay un bound de retries probado para el workload (e.g. SPSC ring buffer con 1 productor + 1 consumidor: retry-bound = 0).
- **Wait-free** donde realmente se necesita y se puede costear el overhead.
- **RCU** es un excelente middle ground en kernels: readers son wait-free en el fast path, writers sincronizan.

**Ref:** Herlihy M., "Wait-free synchronization", TOPLAS 13(1), 1991, https://dl.acm.org/doi/10.1145/114005.102808 .
**Ref:** Kogan A., Petrank E., "Wait-free queues with multiple enqueuers and dequeuers", PPoPP 2011, https://dl.acm.org/doi/10.1145/1941553.1941585 .

---

## 13. Tabla comparativa — RTOS resumen

| OS | License | Kernel LOC | POSIX | Dominio típico | SMP | Cert |
|---|---|---|---|---|---|---|
| **Zephyr** | Apache 2.0 | ~50 K (kernel/) | parcial | IoT, Matter, wearables | sí (2020+) | en progreso (IEC 61508) |
| **FreeRTOS** | MIT | ~9 K | no (API propia) | IoT mass-market, MCU | sí (2024+) | SafeRTOS deriv → SIL3, FDA |
| **QNX Neutrino** | commercial | ~60-100 K | PSE52/PSE54 | auto infotainment, medical, nuclear | sí (siempre) | ISO 26262 ASIL D, IEC 61508 SIL 3, IEC 62304 |
| **VxWorks** | commercial | ~500 K+ | sí | aero/defense, Mars rovers, industrial | sí | DO-178C DAL A, IEC 62304, ISO 26262 |
| **RTEMS** | GPL+exception | ~200 K | parcial (pthread etc) | space (ESA LEON), academia, defense | sí (2020+) | ECSS-compatible path |
| **NuttX** | Apache 2.0 | ~150 K | sí (amplio) | drones (PX4), IoT | sí (2019+) | no formal |
| **Contiki-NG** | BSD | ~30 K | no | WSN, 6LoWPAN | no (single-CPU micros) | no |
| **Linux PREEMPT_RT** | GPLv2 | ~35 MLOC total | sí (glibc) | industrial, audio pro, broadcast | sí | Red Hat In-Vehicle trabajando, no ready |

Notas:
- LOC kernel-only, excluye drivers + libs.
- "POSIX parcial" = subset (pthread, signals básicos, pero no full PSE).
- Cert column: las más citadas/alcanzadas, no exhaustiva.

---

## 14. ALZE applicability

### v1 — hoy (ALZE es general-purpose hobbyist)

**NO** intentar RT. El kernel en `/root/repos/alze-os` tiene problemas básicos antes (IDT incompleta, asunciones SMP, FS sin locks — ver `review/_sintesis.md`). Pretender hard-RT sobre esta base sería autoengaño. **Regla:** no llamar "real-time" a nada del kernel en docs/README; decir "hobbyist general-purpose".

**Qué sí copiar de RTOS ya mismo, sin cambiar género:**
1. **Per-CPU runqueues** — así no necesitás un big kernel lock para scheduler. Patrón trivial de seguir, ya presente en Linux 2.6.x pre-CFS.
2. **Tick-based simple round-robin** con prioridades fijas en el top-level, antes de meter EEVDF o CFS-like. Literalmente `current_task = pick_highest_priority_runnable()`. Igual que FreeRTOS.
3. **Mutex con priority inheritance básico** cuando tengas mutexes. Código de PIP simple cabe en ~50 líneas. No inventar locks "lock-free because fast".
4. **IRQ latency metric** — medir y reportar. Aunque no sea RT, querés saber el peor caso. Añadir un benchmark `cyclictest`-like mínimo al CI: bucle midiendo delta entre 2 IRQs de timer esperadas, print worst case cada N segundos.
5. **Spinlocks cortos documentados** — dejar claro qué holds son "long" y se convierten en waits en futuro, vs cuáles son invariantes cortos (estilo raw_spinlock_t).

### v2 — post-capability (si ALZE madura)

Si ALZE sobrevive y adopta capability model (R3 `capability_kernels.md`):
1. **IRQ threading opcional** — mover handlers a bottom-halves que corran como tareas schedulables. Requiere scheduler robusto ya. Patrón PREEMPT_RT simplificado.
2. **Preemptible spinlocks** donde se justifique latencia — convertir locks largos (vfs, inodo locks) en sleepable. Mantener `raw_spinlock` para hot-path scheduler/IRQ entry.
3. **Bounded allocator** para paths críticos (slab con freelists de tamaño fijo, sin splits/merges arbitrarios). Aun sin ser RT, reduce jitter significativo.
4. **Big kernel lock: nunca**. No reintroducir lo que Linux pasó una década matando.

### v3 — aspirational (scope pivot, honestidad)

**Real-time hard-RT es un género distinto.** Si alguna vez ALZE quisiera ser RTOS:
- **Rewrite**, no refactor, del scheduler.
- **Allocator fully bounded** (sin general-purpose malloc en critical paths). Patrón: pools fijos + slab simple.
- **NO page faults en critical path** → pinning de memoria para tasks RT, o no usar MMU en perfil RT.
- **Preemptible RCU** diseñado desde el principio — readers mark preemption, writers wait per-CPU flags. Complejidad McKenney-nivel.
- **Deterministic IPC** — message-passing con buffers preallocados y cost acotado.
- **WCET analysis** — hay que poder calcular o medir peor caso. Herramientas como aiT, Bound-T. Caro.
- **Cert path** si esto es producto: DO-178C, ISO 26262, IEC 62304. Años y millones de dólares.

**Alternativa realista:** si un día se quiere un "perfil RT" en ALZE, que sea un **kernel distinto** — sibling project tipo "alze-rt" derivado pero separado, igual que Linux tuvo PREEMPT_RT como patch out-of-tree 20 años antes de merge. Mantener ALZE general-purpose y no diluir sus invariantes.

---

## 15. Cierre honesto

**Claim central de este archivo:** los RTOS son una **categoría de OS**, no una feature de los OS generales. Sus invariantes de diseño —determinismo > throughput, WCET > promedio, bounded allocator, no page faults, preemption deterministic— son **opuestos** a los invariantes de un general-purpose kernel (throughput promedio, virtualización de memoria por demand, allocator optimizado para ops/sec).

Linux logró acercarse con PREEMPT_RT después de **20 años** de trabajo coordinado de Molnar, Gleixner, Rostedt, Siewior, McKenney y decenas más, con el respaldo de Linutronix, OSADL, Intel, IBM, Red Hat. El merge en Linux 6.12 (nov 2024) es un evento histórico en la disciplina — no un target razonable para un solo-dev hobbyist.

**Qué ALZE puede hacer bien sin pivotar:**
- **Baja latencia, no real-time.** Scheduler simple, per-CPU runqueues, locks con PI, no long-held spinlocks. Objetivo: worst-case IRQ-to-task wakeup medible y razonable (< 500 µs en hardware modest). Sin garantías pero con métricas.
- **Medir peor caso, siempre**, no solo promedio. Un `cyclictest` pobre vale oro diagnóstico.
- **Resistir la tentación de llamar "real-time" a algo que no lo es**, tanto por honestidad como porque atrae expectativas imposibles.

**Qué NO hacer:**
- No meter rt_mutex + threaded IRQs + SCHED_DEADLINE en un kernel que aún no tiene una IDT completa.
- No llamar "RTOS" ni "real-time kernel" al README si el scheduler es round-robin cooperativo.
- No confiar en "lock-free = fast = real-time". Falso ternario.
- No leer este archivo como TODO list. Es contexto para decidir qué **no** hacer.

**Siguiente lectura sugerida:** para el perfil bajo-latencia-no-RT, ver `r3/schedulers_modern.md` (EEVDF + per-CPU) y `r3/rcu_synchronization.md` (readers sin locks). Para hard-RT, este archivo + Buttazzo 2011 + Liu-Layland 1973. Para verification-grade, `r4/sel4_verified.md`.

---

**Referencias primarias consolidadas:**

- Barry R., "Mastering the FreeRTOS Real Time Kernel", 2016. https://www.freertos.org/Documentation/RTOS_book.html
- Buttazzo G., "Hard Real-Time Computing Systems", Springer 3ª ed. 2011. ISBN 978-1-4614-0676-1.
- Corbet J., "PREEMPT_RT is here", LWN 2024-11. https://lwn.net/Articles/999355/
- Corbet J., "The realtime preemption merge", LWN 2023. https://lwn.net/Articles/948333/
- Dunkels A. et al., "Contiki", IEEE LCN 2004. https://dunkels.com/adam/dunkels04contiki.pdf
- Dunkels A. et al., "Protothreads", SenSys 2006. https://dunkels.com/adam/dunkels06protothreads.pdf
- Gleixner T., Molnar I., "Realtime preemption and GPOS", OLS 2005. http://halobates.de/download/ols2005.pdf
- Hildebrand D., "An Architectural Overview of QNX", USENIX 1992. https://www.usenix.org/legacy/publications/library/proceedings/micro92/full_papers/hildebrand.pdf
- Herlihy M., "Wait-free synchronization", TOPLAS 13(1), 1991. https://dl.acm.org/doi/10.1145/114005.102808
- Herlihy M., Shavit N., "The Art of Multiprocessor Programming", 2008/2020. ISBN 978-0124159501.
- Kogan A., Petrank E., "Wait-free queues", PPoPP 2011. https://dl.acm.org/doi/10.1145/1941553.1941585
- Lelli J. et al., "Deadline scheduling in the Linux kernel", SPE 46(6), 2016. https://doi.org/10.1002/spe.2335
- Leung J., Whitehead J., "On the complexity of fixed-priority scheduling", PerfEval 1982. https://doi.org/10.1016/0166-5316(82)90004-1
- Levy A. et al., "Multiprogramming a 64 kB Computer Safely and Efficiently" (Tock OS), SOSP 2017. https://www.tockos.org/assets/papers/tock-sosp2017.pdf
- Liu C.L., Layland J.W., "Scheduling Algorithms for Multiprogramming in a Hard-Real-Time Environment", JACM 20(1), 1973. https://dl.acm.org/doi/10.1145/321738.321743
- Molnar I., "Real-Time Preemption", LKML 2004. https://lwn.net/Articles/107269/
- OSADL, "Realtime Linux Latency Plots". https://www.osadl.org/Realtime-QA-Farms.osadl-qa-farm-realtime.0.html
- Reeves G., "What really happened on Mars Rover Pathfinder", RISKS 19.49, 1997. http://catless.ncl.ac.uk/Risks/19.49.html#subj1
- Sha L., Rajkumar R., Lehoczky J.P., "Priority Inheritance Protocols", IEEE TC 39(9), 1990. https://doi.org/10.1109/12.57058
- RTEMS Project. https://docs.rtems.org/
- NuttX (Apache). https://nuttx.apache.org/docs/latest/
- Zephyr Project. https://docs.zephyrproject.org/latest/
- BlackBerry QNX. https://www.qnx.com/developers/docs/
- Wind River VxWorks. https://www.windriver.com/products/vxworks
- Contiki-NG. https://docs.contiki-ng.org/
- Embassy. https://embassy.dev/
- RTIC. https://rtic.rs/
