# Power management — deep dive

**Ronda:** R5 / `power_management.md`
**Fecha:** 2026-04-22
**Scope:** ALZE OS R2 review no mencionó power management. `cpuidle.c` existe (MWAIT C1) pero la infra termina ahí: no hay ACPI tables, no cpufreq, no runtime PM, no thermal, no battery. Este doc profundiza el área entera para decidir qué copiar v1/v2 y qué dejar fuera. Thesis del final: power mgmt "real" (S3 resume correcto) es la tumba de los hobby OSes — ALZE debe targetear "halts cleanly" para v1 y, como mucho, S5 + HLT para v2.

Referencias cruzadas evitadas: detalles de MWAIT C1 básico ya están en `review/scheduling_sync.md:46` + `cpuidle.c:60-75`. Aquí el contexto amplio.

---

## 1. El problema: por qué power management es distinto

Un kernel "normal" (scheduler + VMM + FS + drivers) tiene un modelo fácil de razonar: todo está activo, todo mantiene estado, todo responde a IRQs. Power management rompe ese modelo por el eje del tiempo:

- **Componentes se apagan** (CPU en C6, disco en D3, GPU power-gated) y tienen que **re-inicializarse** en tiempos no-deterministas.
- **El "estado del sistema" deja de ser una variable única** — hay tasks dormidas, drivers suspendidos, hardware en estados transitorios, wake-up sources armados.
- **Race conditions son mucho más sutiles** — un device que vuelve de D3 mientras el kernel cree que aún está en D0 causa crashes que aparecen horas después del `resume`.
- **Los tests son caros** — suspend/resume no se prueba en QEMU fácilmente. La validación real requiere hardware + múltiples ciclos.

Por eso Linux `drivers/base/power/` + `kernel/power/` + `drivers/cpufreq/` + `drivers/cpuidle/` + `drivers/thermal/` suman ~60 000 LOC, más ~200 000 LOC distribuidos en callbacks `.suspend`/`.resume` por todos los drivers. Windows no publica números pero PowerMgmt.sys + PoFx + ACPI.sys es una fracción comparable.

---

## 2. ACPI — la madre de todo power management moderno

### 2.1 Historia y versiones

- **Advanced Configuration and Power Interface**. Spec original 1996 (Intel+Microsoft+Toshiba), reemplaza APM (Advanced Power Management, BIOS-driven, pre-1995).
- Hoy mantenida por UEFI Forum. Spec vigente: **ACPI 6.5** (2022-09). [uefi.org/acpi](https://uefi.org/specs/ACPI/6.5/) / [archive](https://web.archive.org/web/2025*/https://uefi.org/specs/ACPI/6.5/).
- Core idea: firmware describe tablas (ACPI tables) en memoria, el OS las parsea y ejecuta bytecode (AML — ACPI Machine Language) con un intérprete. AML = dialecto bytecode de ASL (ACPI Source Language, similar a C/Pascal mix).

### 2.2 Tablas principales

| Tabla | Sig | Contenido |
|---|---|---|
| RSDP | `RSD PTR ` | Root System Description Pointer — encontrado en BIOS ROM o UEFI system table. Punto de entrada. |
| RSDT/XSDT | `RSDT`/`XSDT` | Root System Description Table — array de punteros al resto. XSDT es 64-bit. |
| FADT | `FACP` | Fixed ACPI Description Table — registros de power mgmt fijos, puntero a FACS+DSDT. |
| FACS | `FACS` | Firmware ACPI Control Structure — wakeup vector + hardware signature. |
| DSDT | `DSDT` | Differentiated System Description Table — AML bytecode del namespace principal. |
| SSDT | `SSDT` | Secondary System Description Table — AML adicional, carga dinámica. |
| MADT | `APIC` | Multiple APIC Description Table — LAPIC/IOAPIC/IRQ routing. |
| HPET | `HPET` | High Precision Event Timer. |
| MCFG | `MCFG` | PCI Express memory-mapped config space base. |
| SRAT | `SRAT` | System Resource Affinity Table — NUMA topology. |
| SLIT | `SLIT` | System Locality Information Table — NUMA distances. |
| DMAR | `DMAR` | DMA Remapping — IOMMU (VT-d). |

### 2.3 ACPI Sleep states (S-states)

| Estado | Descripción | Exit time | Data preservation |
|---|---|---|---|
| **S0** | Working — CPU activo | — | Todo |
| **S0ix** | Modern Standby (platform idle) — variante S0 sub-estados | 10-50 ms | Todo (CPU en C10, devices en D3) |
| **S1** | Power-on standby — CPU stops execution pero caches + memoria vivos | <1 s | CPU state + DRAM + devices |
| **S2** | Raro/opcional — CPU off, caches se pierden | <1 s | DRAM + devices |
| **S3** | Suspend-to-RAM (ACPI "sleep") — solo DRAM en self-refresh | 1-3 s | DRAM solamente; CPU y devices pierden estado |
| **S4** | Suspend-to-disk (hibernate) — DRAM se dump a disco | 5-30 s | Disco (hibernate image) |
| **S5** | Soft-off — shutdown, igual a poweroff pero el PSU sigue dando 5V standby para wake | — | Nada en RAM; solo settings en CMOS/NVRAM |
| **G3** | Mechanical off — PSU desconectado físicamente | — | Nada |

Nota: S1 está prácticamente muerto en hardware post-2010 (ahorra poca energía vs S3). La industria moved desde S3 hacia **S0ix/Modern Standby** post-2018 porque S3 tarda >1s para wake y S0ix puede wake en <100 ms manteniendo "always connected".

### 2.4 ACPI Device states (D-states)

Por cada device en el namespace:

| Estado | Descripción | Power | Context |
|---|---|---|---|
| **D0** | Fully on | Max | Operacional |
| **D0 active/idle** | D0 pero sub-estados (algunos drivers) | ~max | Operacional, auto-idle interno |
| **D1** | Opcional, ahorro ligero | <D0 | Parcial (device-specific) |
| **D2** | Opcional, ahorro medio | <D1 | Parcial |
| **D3hot** | Off pero aún enumerable en el bus (PCI config space live) | Min on-bus | Perdido |
| **D3cold** | Off + power removed del slot/link | ~0 | Perdido; requiere re-enumerate |

PCI Express Power Management Capability define estados `PCIPM D0/D1/D2/D3hot/D3cold` + `L-states` del link (L0 on, L0s/L1 low-power, L2/L3 off).

### 2.5 Global states (G-states)

- **G0** = working (includes S0, S0ix).
- **G1** = sleeping (includes S1-S4).
- **G2** = soft-off (= S5).
- **G3** = mechanical off.

### 2.6 ACPI Processor states (C-states y P-states) — desde ACPI

ACPI define los frameworks; el detalle x86 va en la sección 3.

- **C-states** (`_CST` method): idle states. C0 running, C1+ idle progresivamente más profundos.
- **P-states** (`_PSS` method): performance states — pares (frecuencia, voltaje) dentro de C0.
- **T-states** (`_TSS`): throttling states — CPU duty-cycles para thermal mgmt (muy crudo; rara vez usado post-2010, P-states + Turbo lo reemplazan).

---

## 3. x86 C-states — dormir de verdad

### 3.1 Tabla de C-states modernos (Intel Skylake → Raptor Lake)

| C-state | Descripción | Power (typ) | Exit latency | Qué se preserva |
|---|---|---|---|---|
| **C0** | Active execution | 100% TDP | — | Todo |
| **C1** | `HLT` — CPU halted, caches coherentes | ~30% TDP | <10 ns | L1/L2/L3, TLB, reg state |
| **C1E** | Enhanced C1 — baja voltaje + frecuencia bus | ~20% TDP | ~10 ns | Igual que C1 |
| **C3** | Sleep — L1/L2 flushed, L3 kept | ~10% TDP | ~80 ns | L3, reg state, architectural state |
| **C6** | Deep Sleep — core power gated, arch state saved en SRAM de uncore | ~2% TDP | 100-300 μs | Arch state en SRAM; L1/L2/L3 perdidos de ese core |
| **C7** | Deeper Sleep — L3 también flushed/power-gated (LLC) | ~1% TDP | 300 μs-1 ms | Solo RAM |
| **C8** | C7 + más uncore off | <1% | 1 ms | RAM |
| **C9** | C8 + VR (voltage regulator) throttled | <1% | 1-3 ms | RAM |
| **C10** | Deepest — package idle, VR off, PLL off, "Deep Package C-state" | <0.5 W package | 3-10 ms | RAM |

AMD Zen usa numeración similar (C0/C1/C2 con `HLT`/clock gate/package clock gate); los nombres marketing cambian.

### 3.2 `HLT` vs `MWAIT`

- **`HLT`** (1 byte, desde 8086): para el CPU hasta el próximo interrupt. Entra en C1 (o similar — el hardware decide). Semánticamente simple, pero el kernel **no puede indicar** cuánto tiempo piensa estar idle ni qué profundidad acepta.
- **`MONITOR/MWAIT`** (SSE3, 2004): `MONITOR addr` arma una línea de cache como tripwire; `MWAIT eax, ecx` entra en el C-state codificado en `eax` hasta que (a) la cacheline monitored se invalida, (b) llega un IRQ (si `ecx & 1`), o (c) un timer expira. Permite al kernel elegir el C-state y evita race-condition cacheline de `HLT`-based idle loops.
- Hint nibble de MWAIT `eax`: `0x00`=C1, `0x10`=C2, `0x20`=C3, `0x30`=C4, `0x40`=C5, `0x50`=C6, `0x60`=C7. Sub-state en bits bajos.
- Intel CPUID leaf 0x05 (MONITOR/MWAIT) expone `MONITOR line size` + `supported C-states per sub-state`. Leaf 0x06 (Thermal/Power) bit 0 = Digital Thermal Sensor.

ALZE usa `MWAIT 0x00` (C1) ya — ver `cpuidle.c:67`. Llegar a C6/C10 requiere (a) validar soporte en leaf 0x05, (b) medir latencia, (c) decidir cuándo es seguro según "próximo timer/IRQ predicted". Es exactamente lo que Linux `menu`/`teo` governors hacen.

### 3.3 cpuidle governor (Linux)

`drivers/cpuidle/governors/`:

- **`menu`** (Venki Pallipadi, 2007; Arjan van de Ven refinements 2009): predice idle duration usando histórico + próximo timer deadline. Para cada C-state, compara `predicted_idle >= target_residency[state]` y elige el más profundo que cumple. Heuristic-heavy.
- **`teo`** (Rafael Wysocki, 2018 — Timer-Events Oriented): más agresivo en deep C-states, pero solo si próximo timer está lejos. Reduce "wakeup storms" en which CPU va a C6 y sale por un timer a 50 μs después (costoso vs ganancia).
- **`ladder`** (legacy, pre-2010): simple FSM, go one state deeper on each successful wake; downgrade if woken too early. Casi deprecated.
- **`haltpoll`** (2019): para virtualización — spin antes de HLT para evitar VM exit cuando hay mucha carga. Arquitecturalmente interesante para microkernels: la idle task polls un flag antes de "dormir de verdad".

Métricas por estado: `target_residency` (μs) = break-even entre ahorro de energía y costo de entrada/salida. `exit_latency` (μs) = tiempo para volver a C0.

### 3.4 Tickless (NOHZ)

Pre-tickless: timer interrupt a HZ (100/250/1000) fijo; CPU nunca duerme >1 tick.

**NOHZ_IDLE** (Thomas Gleixner + Frederic Weisbecker, Linux 2.6.21, 2007): timer se reprograma al próximo evento conocido (timer expiration, sched deadline). CPU puede dormir segundos.

**NOHZ_FULL** (2013): tickless incluso cuando hay exactly 1 task running — elimina el tick periodico en CPUs dedicadas a real-time o HPC. Requiere que el scheduler no dependa del tick para fairness (aportado con EEVDF).

ALZE hoy: PIT a 100 Hz, **no-tickless**. El kernel despierta cada 10 ms aunque no haya nada que hacer. MWAIT-C1 ahorra mucho dentro del tick, pero los 100 wake/s queman C6/C10 opportunities.

---

## 4. cpufreq — bajar la frecuencia

### 4.1 Drivers

| Driver | Hardware | Mecanismo |
|---|---|---|
| **acpi-cpufreq** | x86 generic | MSR writes vía ACPI `_PSS`. Pre-HWP. Requiere OS para elegir P-state. |
| **intel_pstate** | Intel 2013+ (Sandy Bridge) | OS escribe hint; HW puede overrule (HWP). |
| **amd-pstate** | AMD Zen+ (2022) | CPPC-like, HW-managed P-states. |
| **cppc_cpufreq** | ARM/generic | Collaborative Processor Performance Control (ACPI 5.0+). |
| **armhw-cpufreq**/**scpi**/**scmi** | ARM | Firmware-assisted DVFS. |

### 4.2 Governors clásicos

| Governor | Política |
|---|---|
| **performance** | Siempre en max P-state. Default en servers. |
| **powersave** | Siempre en min P-state. Default en laptops a veces. |
| **userspace** | Daemon de userland elige (ej. `cpufreq-selector`). |
| **ondemand** | Escala al max cuando util > up_threshold (típ 95%), baja gradual si cae. Histórico pre-2015. |
| **conservative** | Como ondemand pero sube gradual también — menos spikes. |
| **schedutil** | Integrado con scheduler (ver 4.4). Default desde 4.14. |

### 4.3 Intel P-states detail

- **Legacy P-states (pre-Haswell)**: MSR `IA32_PERF_CTL`, OS escribe ratio. Voltage set por platform via VR.
- **Turbo Boost** (Nehalem 2008): HW sube sobre max P-state documentado si thermal + power budget lo permiten. MSR `IA32_MISC_ENABLE` bit 38 = turbo disable.
- **Turbo Boost Max 3.0** (Broadwell-E 2016): algunos cores tienen "favored core" ratings (mejor silicon); el OS debe schedulear workloads latency-sensitive en esos cores. Requiere `ITMT` (Intel Turbo Max Technology) driver + scheduler hints. En Linux: `CONFIG_X86_INTEL_ITMT` + `arch_asym_cpu_priority`.
- **Speed Shift / HWP** (Skylake 2015, ACPI 6.1 CPPC ratification): hardware-managed P-states. OS escribe "desired performance + min + max + energy/perf preference" (MSR `IA32_HWP_REQUEST`), HW decide realtime (μs scale) la frecuencia/voltage. OS-driven escalation (ms scale) ya no alcanza la fidelidad del HW — por eso HWP domina.
- **Intel Thread Director** (Alder Lake 2021): hint al OS sobre qué core (P-core vs E-core) conviene por clase de workload. Classifies running threads into 4 classes vía HW telemetry. MSR `IA32_HW_FEEDBACK_*`. Linux >= 5.18 implementa mapping a `sched/fair.c` via `hfi` driver.

Refs:
- Rafael J. Wysocki, *"intel_pstate"*, [kernel.org/doc admin-guide/pm/intel_pstate](https://www.kernel.org/doc/html/latest/admin-guide/pm/intel_pstate.html) / [archive](https://web.archive.org/web/2025*/https://www.kernel.org/doc/html/latest/admin-guide/pm/intel_pstate.html).
- Intel SDM Vol 3B ch 14 "Power and Thermal Management" — MSRs, HWP, RAPL. [intel.com sdm](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

### 4.4 schedutil — integración scheduler ↔ cpufreq

Vincent Guittot + Peter Zijlstra, Linux 4.7 (2016), default 4.14. [kernel.org/doc scheduler/sched-capacity](https://www.kernel.org/doc/html/latest/scheduler/sched-capacity.html).

Idea: en lugar de que cpufreq muestree cada 10 ms (ondemand), el **scheduler** que ya conoce la util por-CFS entity via **PELT (Per-Entity Load Tracking)** le dice directo al cpufreq: "este CPU necesita X% de capacidad, escala la freq acorde".

- PELT: geometric decay con tau=32 ms (`y^32 = 0.5`). Cada sched tick actualiza `util_avg` por task y por rq.
- schedutil: `target_freq = 1.25 × util_avg × max_freq`. El 1.25 es headroom.
- **UCLAMP** (Patrick Bellasi 2019, Linux 5.3): permite a userland/cgroup acotar (min/max util), útil para UI frame rate budgets (Android) + servicios batch con cap.

Resultado: respuesta sub-tick a bursts, sin overhead de sample timer separado. En benchmarks Pixel-4 reduced p99 frame-drop 30% vs ondemand (Vincent Guittot LPC 2018).

### 4.5 Energy Model + EAS (Arm heterogeneous)

- **Arm big.LITTLE** (2011): cluster of high-perf cores (Cortex-A15) + cluster of efficient (A7). Issue: scheduler debía decidir "place here or there". Hacks iniciales (IKS, HMP) fallaban.
- **Energy Aware Scheduling (EAS)** — Morten Rasmussen (Arm), 2015-2018. *"Energy Aware Scheduling for the Linux Kernel"*. [Connect 2018 slides](https://static.linaro.org/connect/hkg18/presentations/hkg18-218.pdf) / [archive](https://web.archive.org/web/2023*/https://static.linaro.org/connect/hkg18/presentations/hkg18-218.pdf). Mainline Linux 5.0 (Q1 2019).
- **Energy Model** (`kernel/power/energy_model.c`): registra por CPU un array `(capacity_i, power_i)` pairs (del DT `operating-points-v2` + `em-pd-millijoules`). EAS consulta: "si muevo esta task con `util_avg` a este CPU, ¿cuál de los candidatos minimiza energía manteniendo latencia aceptable?".
- **DynamIQ** (Arm 2017): sucesor big.LITTLE — hasta 8 cores por cluster, mezclables. A76+A55, A78+A55, X1+A78+A55.
- **Intel Hybrid (Alder Lake 2021)**: misma idea, P-cores (Golden Cove) + E-cores (Gracemont). Thread Director (arriba) es el equivalente al EAS pero con HW classifier en lugar de DT-declared energy model.

### 4.6 Frequency scaling en server/desktop

Desktops/servers high-end usan governors **performance** casi siempre — el ahorro es marginal vs el hit en latency. Workstations crypto/HPC a veces desactivan turbo+HT para estabilidad de timing. Ningún kernel serio de HPC corre ondemand.

---

## 5. Runtime PM (RPM) — devices duermen mientras el system está up

### 5.1 Concepto

Rafael Wysocki + Alan Stern, Linux 2.6.32 (2009). [kernel.org/doc power/runtime_pm](https://www.kernel.org/doc/html/latest/power/runtime_pm.html) / [archive](https://web.archive.org/web/2025*/https://www.kernel.org/doc/html/latest/power/runtime_pm.html).

- Cada device en el bus tree tiene un `struct dev_pm_info`:
  - `usage_count` (atomic): cuántos "activos" requests hay.
  - `runtime_status`: `RPM_ACTIVE` / `RPM_SUSPENDED` / `RPM_SUSPENDING` / `RPM_RESUMING`.
  - `autosuspend_delay_ms`.
- API principal:
  - `pm_runtime_get_sync(dev)` — bump usage; si estaba suspended, resume ahora (sleeping).
  - `pm_runtime_get(dev)` — async version.
  - `pm_runtime_put(dev)` — dec usage; si llega a 0, disparar autosuspend timer.
  - `pm_runtime_put_autosuspend(dev)` — similar pero respeta delay.
  - `pm_runtime_suspend_at_idle()` — suspender ahora si usage==0.
- Driver implementa callbacks en `pm_ops`:
  - `.runtime_suspend(dev)` — guardar state, cortar clocks, bajar power.
  - `.runtime_resume(dev)` — restaurar.
  - `.runtime_idle(dev)` — hint que el device podría estar idle; default llama suspend.

### 5.2 Parent-child constraint

Bus tree matters: `usb_device` suspended requiere que el `usb_host_controller` esté activo suficiente para propagate el wake. PM core hace auto-chain: suspending child mientras parent está suspended no pasa.

### 5.3 Autosuspend delay

El delay default típico: 2000 ms para USB devices (evita flap-y), 10 ms para devices con resume muy rápido. Demasiado corto = thrashing; demasiado largo = miss ahorro. Ajustable vía sysfs `autosuspend_delay_ms`.

### 5.4 Cross-cutting con system suspend

En suspend-to-RAM, system PM core llama a `.suspend_late()` → hace `pm_runtime_force_suspend()` si no está suspended. En resume, `pm_runtime_force_resume()`. Esto permite que devices que ya estaban runtime-suspended skippeen re-suspension innecesaria.

---

## 6. Device suspend/resume — el campo de minas

### 6.1 Callback order

`drivers/base/power/main.c` define **9 phases**:

1. `.prepare` — return error para cancelar suspend; opcional.
2. `.suspend` — con IRQs on, processes frozen. Driver guarda state mutable.
3. `.suspend_late` — IRQs on, but drivers can't talk to hardware (PM genérico corre aquí).
4. `.suspend_noirq` — IRQs **off**, HW register access seguro, no-one else can race.
5. (→ Low-power state, DRAM en self-refresh)
6. `.resume_noirq` — IRQs off, restore HW registers.
7. `.resume_early` — IRQs on.
8. `.resume` — drivers talk to hardware, wake wait queues.
9. `.complete` — final cleanup.

Cada phase respeta el device tree (children suspend antes que parents en suspend; inverso en resume).

### 6.2 Firma de problemas comunes

- **Device no re-enumera post-resume**: BAR0 reset, no se re-programa. Causa ~30% de "my laptop doesn't resume" reports.
- **USB hub pierde estado**: children removed+re-added con nuevos device numbers.
- **GPU hang post-resume**: VRAM state lost, display pipe no re-armado. Nouveau/amdgpu/i915 dedican >5k LOC cada uno solo a suspend/resume.
- **Lockdep explosión**: paths de suspend toman locks en orden distinto a runtime; hay infra (`lockdep_assert_held_suspend`) para detectarlo.
- **RTC wake time**: si el RTC wake event se arma a T pero el resume tarda más que T-now, se perdió el wake; system queda "stuck sleeping".

### 6.3 PCIe detail

Standard PCI PM: device va D0 → D3hot (config space alive, BAR access undefined). D3cold requiere ACPI `_PS3` method del root port para quitar power físicamente. En resume, `_PS0` restaura y el device re-hace link training.

### 6.4 USB suspend

USB spec define selective suspend (per-device) + bus suspend (todo el host controller). 3 ms sin tráfico → device signal "suspend"; RESUME signaled por 20 ms para wake. Device descriptors cached; no re-enumerate si descriptor matches.

---

## 7. Modern Standby / s2idle

### 7.1 Windows Modern Standby

Microsoft, Windows 8+ (2012) como "Connected Standby", renombrado **Modern Standby** (Windows 10+). [Microsoft Docs — Modern Standby](https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/modern-standby) / [archive](https://web.archive.org/web/2025*/https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/modern-standby).

- El system nunca entra a S3. Está siempre en S0, pero con sub-estados de platform idle (S0i1, S0i2, S0i3) que son casi-S3 en consumo (CPU en C10, devices en D3, display off).
- Wake event wake en <100 ms (vs 1-3 s S3).
- "Always connected" — network stack puede wake el system por push notifications, manteniendo DRIPS (Deepest Runtime Idle Platform State) la mayor parte del tiempo.
- **DRIPS** = platform state cuando el SoC está en power state más profundo con RAM vivo. Literal: package C10 + DRAM self-refresh + PCH en slumber.

Problema bien documentado (2021-2024): Modern Standby en laptops "drena batería" si devices no cooperan con DRIPS. Microsoft introduce `modernstandby /diagnose` + `powercfg /sleepstudy` como herramientas post-mortem.

### 7.2 Linux s2idle

Rafael Wysocki + Len Brown, Linux 4.12 (2017). `/sys/power/mem_sleep` valores: `s2idle` (freeze), `shallow` (standby → ACPI S1), `deep` (suspend → ACPI S3).

- `s2idle`: el equivalente Linux de Modern Standby. Freeze userspace, quiesce devices, el cpuidle goes a C10 por todos los cores, espera wake IRQ.
- NO escribe los registros ACPI `PM1a_CNT.SLP_EN` que triggean la transición firmware-driven a S3. Kernel nunca se duerme "real".
- Plus: s2idle funciona en laptops donde firmware rompe S3 (muchos post-2018 — Intel pushed OEMs a s2idle-only platforms).
- Minus: ahorro menor que S3 true (DRAM no sale de auto-refresh porque ya estaba, pero los voltaje rails de PCH/uncore pueden no bajar tanto como en S3).

Matthew Garrett + Rafael Wysocki han escrito extensivamente:
- Matthew Garrett, *"s2idle is a mess"*, 2019 blog. [mjg59.dreamwidth.org](https://mjg59.dreamwidth.org/) / [archive](https://web.archive.org/web/2023*/https://mjg59.dreamwidth.org/).
- LWN articles curado de 2017-2024: [lwn.net/Kernel/Index/#Power_management](https://lwn.net/Kernel/Index/) / [archive](https://web.archive.org/web/2025*/https://lwn.net/Kernel/Index/).

---

## 8. Thermal management

### 8.1 Thermal framework Linux

`drivers/thermal/`. [kernel.org/doc driver-api/thermal](https://www.kernel.org/doc/html/latest/driver-api/thermal/index.html).

Conceptos:
- **Thermal zone**: un subsistema térmico (CPU package, GPU, battery, SSD, VRM...). Cada uno con temperature sensor + trip points.
- **Trip point**: umbral de temperatura (passive, active, hot, critical). Al cruzarse, se activan cooling devices.
- **Cooling device**: algo que reduce temperatura — CPU freq throttle, fan RPM up, GPU freq cap.
- **Governor**:
  - `step_wise`: per-trip incremento simple.
  - `fair_share`: reparte cooling proporcional a peso.
  - `bang_bang`: ON/OFF (fan).
  - `user_space`: notifica userland daemon (ej. `thermald`).
  - `power_allocator`: PID controller con power budgets (Arm IPA — Intelligent Power Allocation).

### 8.2 Intel DTT

**Dynamic Tuning Technology** (antes Dynamic Platform and Thermal Framework, DPTF). Driver firmware-assisted, expone policies más granulares que ACPI thermal zones. Utilizado en laptops Intel-powered para "cuando está en lap vs en desk, diferentes thermal limits".

### 8.3 hwmon

Sensors exposed via `/sys/class/hwmon/*/temp[1..N]_input`. RAPL, coretemp, nct6775 (SuperIO), k10temp (AMD), acpi_thermal, iio (generic), drm_thermal.

### 8.4 Critical shutdown

Todos los frameworks mantienen una trip point "critical" (CPU ~100-105°C). Cross = kernel llama `orderly_poweroff()` → `shutdown(8)` o, si falla, `emergency_restart()`. Garantía: si se quita thermal control, system self-shutdown antes de daño.

---

## 9. Power metrics — medir consumo

### 9.1 RAPL (Intel)

**Running Average Power Limit**. Intel Sandy Bridge+ (2011), MSR `MSR_RAPL_POWER_UNIT` + dominios:
- `MSR_PKG_ENERGY_STATUS`: joules consumidos por package.
- `MSR_PP0_ENERGY_STATUS`: cores.
- `MSR_PP1_ENERGY_STATUS`: "other" (iGPU típicamente).
- `MSR_DRAM_ENERGY_STATUS`: DRAM controller.

Contadores de 32 bits, wrap ~60 s. Linux `powercap` subsystem (`drivers/powercap/`) los expone bajo `/sys/class/powercap/intel-rapl/`.

Uses:
- Per-process power profiling (`perf stat -a -e power/energy-pkg/`).
- Power-aware scheduling (experimental).
- Cloud billing (AWS Graviton usa similar Arm Energy Probe).

Refs:
- Hähnel et al., *"Measuring Energy Consumption for Short Code Paths Using RAPL"*, ACM SIGMETRICS 2012. [acm.org](https://dl.acm.org/doi/10.1145/2425248.2425252) / [archive](https://web.archive.org/web/2023*/https://dl.acm.org/doi/10.1145/2425248.2425252).
- Khan et al., *"RAPL in Action: Experiences in Using RAPL for Power Measurements"*, ACM TOMPECS 2018 — shows mediciones RAPL con error <5% vs external power meter en steady-state; peor en transientes.

### 9.2 Arm Energy Probe

Hardware externo (USB probe) o on-die via Arm IMON. No hay MSR equivalent mainstream. Performance governors asumen energy model del DT, no measured.

### 9.3 Per-process energy

- Ideal: charge `energy_j` por task cuando run. Requires taking RAPL snapshot each context switch (overhead).
- Actual en Linux: `perf stat --per-thread` aproxima via (CPU time × package power / total CPU time). Inexacto con heterogeneous cores.
- Android `BatteryStats`: más sofisticado — registra por UID via cpufreq-time + kernel wakelock contribution.

---

## 10. Battery management

### 10.1 UPower

Freedesktop project. Daemon userland que agrega `/sys/class/power_supply/*` en un DBus interface (`org.freedesktop.UPower`). [upower.freedesktop.org](https://upower.freedesktop.org/) / [archive](https://web.archive.org/web/2025*/https://upower.freedesktop.org/).

- Expone: battery level, charge/discharge rate, AC presence, time-to-empty, time-to-full.
- Consumed por desktop envs (GNOME/KDE) para notifications + low-battery action (suspend at 5%).

### 10.2 `/sys/class/power_supply/BAT0/`

Kernel driver (ACPI battery `/proc/acpi/battery/` legacy, o sbs-battery / cros-ec-sbs / generic-adc-battery) expone:
- `energy_now` (μWh) / `charge_now` (μAh).
- `energy_full`, `energy_full_design`. Ratio = battery health.
- `current_now`, `voltage_now`.
- `status` (`Charging`, `Discharging`, `Full`, `Not charging`).
- `cycle_count` (cuando el fuel-gauge lo soporta — Li-ion degrada a ~80% capacity en ~500-1000 ciclos).

### 10.3 Charge limits (battery longevity)

Hardware-dependent: algunos laptops (ThinkPad TLP, Dell, ASUS) tienen EC-managed charge limit ("stop charging at 80%" = prolonga vida Li-ion 2-3×). Expuesto vía:
- `/sys/class/power_supply/BAT0/charge_start_threshold`
- `/sys/class/power_supply/BAT0/charge_stop_threshold`

No standard en ACPI; cada OEM implementa.

### 10.4 Battery-aware scheduling

Experimental research. Android: "battery saver mode" desde 6.0 — CFS throttling + restricciones de background services. No está en mainline Linux como sched class.

---

## 11. Wakeup sources

### 11.1 Concepto

En sleep/suspend, el kernel arma un subset de IRQs como "wake-capable" antes de dormir. Al llegar IRQ de una fuente wake-enabled, sale del sleep.

### 11.2 PCI PME (Power Management Event)

Bit en PCI config register `PMCSR` (offset 0x04 de PM cap). Device que detecta condition (link up, magic packet WoL, MSI requesting wake) asserta PME#. Root port propaga al ACPI, que despierta al sistema.

### 11.3 USB wake

USB 2.0+: device signal resume con K state por 1-15 ms. Host controller IRQ el kernel. Típicos: keyboard, mouse, HID.

### 11.4 ACPI wake events

GPE (General Purpose Events) + fixed events. Cada uno tiene `_WAK` method (AML) que el firmware ejecuta para setup (e.g., restore LCD brightness). Kernel parsea `_PRW` (Power Resources for Wake) per-device para saber qué wake sources están disponibles.

### 11.5 Unexpected wakeups

Problema recurrente (año tras año, LWN reports): un device aserta wake por bug o interrupt storm, system despierta cada 30 s = battery drain. Debug:
- `cat /sys/kernel/debug/wakeup_sources` — muestra qué activity keeps system awake.
- `dmesg | grep -i wake` — logs kernel de wake events.
- `powertop` — GUI tool para "top wake offenders".

Matthew Garrett docs: *"Why does my laptop wake up?"* — guía tradicional de Linux on laptops.

### 11.6 Wakelock (Android)

Android kernel fork 2009 (pre-mainline) introdujo wakelock primitive — "this task needs system awake". Rechazo mainline inicial por semantic overlap con runtime PM; eventual merge 2013 como `wakeup source` generic. Usado por Android + embedded.

---

## 12. ALZE applicability — qué copiar en cada versión

### v1 (hoy): halt cleanly

**Estado actual** del repo (`/root/repos/alze-os`):
- `cpuidle.c` detecta MWAIT, usa C1. Idle task llama `sti; hlt` (no MWAIT) — ver review `scheduling_sync.md:46` "`sti;hlt` directo — MWAIT detection nunca usada".
- **No hay ACPI parser**.
- No hay shutdown handler. `shutdown()` syscall no existe.
- Panic halts el CPU con `cli; hlt` loop (ver `panic.c`).
- No hay wake, suspend, resume, thermal, battery nada.

**Target v1**:
1. Wire `cpu_idle()` como idle task real — trivial: cambiar `sched.c:237-242` para llamar `cpu_idle()` en lugar de `sti; hlt`. Este es literalmente un 2-line fix y ya aporta ahorro energético medible.
2. **Shutdown clean** via ACPI S5:
   - Parsear RSDP → FADT → obtener `PM1a_CNT_BLK` address.
   - Parsear DSDT mínimo para extraer `\_S5_` package (4 bytes: `SLP_TYPa`, `SLP_TYPb`, `Rsvd`, `Rsvd`).
   - Escribir `(SLP_TYPa << 10) | SLP_EN` a `PM1a_CNT_BLK`.
   - 100-200 LOC total (parsear tabla simple + un AML term). Muchos hobby OSes lo tienen; copiable de OSDev wiki + BOOTBOOT + Serenity.
3. **Reset clean**: ACPI FADT `RESET_REG` + `RESET_VALUE` — 20 LOC.
4. **Que panic loop sea `cli; hlt`** (ya lo es).

Total v1: < 300 LOC añadidos. Aporta "power off de verdad" + "reset por software".

### v2 (post-Sprint-3 del review): S3/S5 simples + runtime PM mínimo

**Si y solo si ALZE targetea laptops**:
1. **ACPI parser completo** (RSDP/XSDT/FADT/FACS/DSDT) + intérprete AML mínimo. ACPICA (Intel's reference) es GPL2/dual; no se puede embed pero sirve como ref. Alternativa: subset AML custom (solo `Method`, `Name`, `Store`, `Return`, basic objects) — ~2-3 KLOC. Genode y seL4 usan ACPICA licensed.
2. **S3 suspend**: mucho más complicado que S5 — requiere:
   - Guardar state de CPU (GPRs, control regs, MSRs).
   - Quiesce todos los devices (llamar suspend callbacks — requiere que los drivers los implementen).
   - Flush DRAM a self-refresh.
   - Escribir FACS `FirmwareWakingVector` con dirección de resume code.
   - Write PM1a_CNT con SLP_TYPa/SLP_EN para S3.
   - Resume code (16-bit → 32-bit → 64-bit → restore state → return from suspend call).
   - Re-init todos los devices en resume.
   - Budget realista: 5-10 KLOC + bugs sutiles por meses.
3. **Runtime PM simplificado**: usage_count atomic + simple suspend-on-zero. Sin autosuspend delay (ese es refinamiento). Sin parent-child chain (cada device independiente). ~300 LOC core + callbacks per driver.
4. **Battery read-only** via ACPI `_BIF`/`_BST` methods. 200 LOC.

Total v2: 8-12 KLOC. Pesa más que la mitad del kernel actual.

### v3 (aspiracional, años)

Si ALZE alguna vez targetea "real laptop experience":
- cpufreq infra completa + schedutil-style governor.
- Thermal framework con trip points + cooling devices.
- s2idle support (quiesce all, deep C-state).
- Modern Standby equivalent.
- RAPL per-process accounting.
- Wake source debug infra (wakelocks equivalent).

Total: 30-50 KLOC. Requiere que todo el rest del kernel sea power-aware.

---

## 13. Tabla resumen: complejidad vs valor por feature

| Feature | LOC estimado | v1? | v2? | Valor |
|---|---|---|---|---|
| Wire `cpu_idle()` como idle real | 5 | ✅ | | Alto — ya existe el código, solo wire |
| ACPI S5 (shutdown) | 200 | ✅ | | Alto — no más "reboot para apagar" |
| ACPI reset | 20 | ✅ | | Medio — `hlt` infinito funciona |
| C-state selection (C2/C6) | 500 | | ✅ | Medio — depends de MWAIT hints |
| Tickless (NOHZ) | 800 | | ✅ | Alto en battery; nulo en server |
| ACPI parser completo + AML | 3000 | | ✅ | Prerequisito de todo lo demás |
| S3 suspend/resume | 8000 | | ⚠️ | Alto si laptop; nulo si desktop |
| Runtime PM simple | 500 | | ✅ | Medio — requiere drivers colaboradores |
| Battery read-only | 200 | | ✅ | Medio |
| cpufreq + governor | 2000 | | | Bajo en v2 sin hardware muy variable |
| Thermal framework | 1500 | | | Bajo hasta que se sobrecaliente |
| RAPL accounting | 300 | | | Bajo (obs tool, no funcional) |
| Modern Standby / s2idle | 5000+ | | | Bajo — requiere resto power-aware |
| Wakeup source debug | 500 | | | Bajo en hobby OS |

---

## 14. Referencias primarias (autor año venue URL)

- UEFI Forum. *"ACPI Specification 6.5"*. 2022-09. [uefi.org](https://uefi.org/specs/ACPI/6.5/) / [archive](https://web.archive.org/web/2025*/https://uefi.org/specs/ACPI/6.5/).
- Intel. *"Software Developer's Manual Vol 3B"* chapter 14 "Power and Thermal Management", MSRs, HWP, RAPL. Rev 2024. [intel.com](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) / [archive](https://web.archive.org/web/2025*/https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
- Rafael J. Wysocki (Intel). *"Linux kernel power management documentation"*. Ongoing. [kernel.org/doc admin-guide/pm](https://www.kernel.org/doc/html/latest/admin-guide/pm/index.html) / [archive](https://web.archive.org/web/2025*/https://www.kernel.org/doc/html/latest/admin-guide/pm/index.html).
- Rafael J. Wysocki. *"intel_pstate CPU performance scaling driver"*. kernel docs. [kernel.org](https://www.kernel.org/doc/html/latest/admin-guide/pm/intel_pstate.html).
- Rafael J. Wysocki + Alan Stern. *"Runtime Power Management Framework for I/O Devices"*. Linux kernel docs since 2.6.32 (2009). [kernel.org/doc power/runtime_pm](https://www.kernel.org/doc/html/latest/power/runtime_pm.html).
- Peter Zijlstra + Vincent Guittot. *"schedutil CPU frequency governor"*. Linux 4.7+ (2016). [kernel.org/doc admin-guide/pm/cpufreq](https://www.kernel.org/doc/html/latest/admin-guide/pm/cpufreq.html).
- Vincent Guittot. *"PELT and schedutil"*. LPC 2018. [linuxplumbersconf.org](https://linuxplumbersconf.org/event/2/contributions/113/) / [archive](https://web.archive.org/web/2023*/https://linuxplumbersconf.org/event/2/contributions/113/).
- Morten Rasmussen (Arm). *"Energy Aware Scheduling for the Linux Kernel"*. Linaro Connect HKG18, 2018. [linaro.org slides](https://static.linaro.org/connect/hkg18/presentations/hkg18-218.pdf) / [archive](https://web.archive.org/web/2023*/https://static.linaro.org/connect/hkg18/presentations/hkg18-218.pdf).
- Arm Ltd. *"Arm DynamIQ Shared Unit Technical Reference Manual"*. 2017-2024 editions. [developer.arm.com](https://developer.arm.com/documentation/100453/latest/) / [archive](https://web.archive.org/web/2023*/https://developer.arm.com/).
- Linux kernel thermal framework. *"Generic Thermal Sysfs driver"*. [kernel.org/doc driver-api/thermal](https://www.kernel.org/doc/html/latest/driver-api/thermal/index.html).
- Hähnel, Döbel, Völp, Härtig. *"Measuring Energy Consumption for Short Code Paths Using RAPL"*. ACM SIGMETRICS 2012. [acm.org](https://dl.acm.org/doi/10.1145/2425248.2425252) / [archive](https://web.archive.org/web/2023*/https://dl.acm.org/doi/10.1145/2425248.2425252).
- Khan, Hirki, Niemi, Ohlsson, Nurminen. *"RAPL in Action: Experiences in Using RAPL for Power Measurements"*. ACM TOMPECS 2018. [acm.org](https://dl.acm.org/doi/10.1145/3177754) / [archive](https://web.archive.org/web/2023*/https://dl.acm.org/doi/10.1145/3177754).
- Microsoft Corp. *"Modern Standby"* documentation. [learn.microsoft.com](https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/modern-standby) / [archive](https://web.archive.org/web/2025*/https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/modern-standby).
- UPower project. *"UPower power management daemon docs"*. Freedesktop. [upower.freedesktop.org](https://upower.freedesktop.org/) / [archive](https://web.archive.org/web/2025*/https://upower.freedesktop.org/).
- LWN editors. *"Linux Power Management articles"* index. 2008-2025. [lwn.net Kernel/Index](https://lwn.net/Kernel/Index/#Power_management).
- Matthew Garrett. *"s2idle is a mess"*. Personal blog 2019. [mjg59.dreamwidth.org](https://mjg59.dreamwidth.org/).
- Venki Pallipadi + Arjan van de Ven. *"cpuidle — Do nothing, efficiently"*. Ottawa Linux Symposium 2007. [kernel.org doc](https://www.kernel.org/doc/ols/2007/ols2007v2-pages-119-126.pdf) / [archive](https://web.archive.org/web/2023*/https://www.kernel.org/doc/ols/2007/ols2007v2-pages-119-126.pdf).
- Frederic Weisbecker. *"NOHZ_FULL: full dynticks"*. LWN 2013-09. [lwn.net/Articles/549580](https://lwn.net/Articles/549580/) / [archive](https://web.archive.org/web/2023*/https://lwn.net/Articles/549580/).
- Patrick Bellasi. *"UCLAMP: Utilization Clamping for CPU Scheduler"*. Linux 5.3, LPC 2019. [kernel.org doc](https://www.kernel.org/doc/html/latest/scheduler/sched-capacity.html).

---

## 15. Nota honesta — donde la ambición hobby muere

Power management es el área donde más kernels hobby se despeñan porque:

1. **El "happy path" es engañoso**. Hacer que un kernel ejecute `hlt` en idle es trivial (ALZE lo tiene). Pero power mgmt "real" = suspend-to-RAM en un laptop post-2018 con GPU discrete + nvme + wifi + bluetooth + fingerprint + webcam + audio codec — cada uno con su `.suspend`/`.resume` que hay que haberlo escrito correctamente, testeado en hardware real, y que no rompa al segundo/tercer ciclo de suspend.

2. **Es irreversible**: un bug en `.resume` de un device crashea el kernel POST-resume, cuando el usuario abre la laptop. Debug es terrible (no hay stack, no hay logs salvo los que persistieron en disco antes del suspend). Los bugs aparecen semanas después de merge.

3. **La superficie de API es enorme**. Linux tiene 9 phases de callback per device, runtime PM, system PM, freezer, wakeup sources, OPP, thermal, cpufreq, cpuidle. Cada uno con su lifecycle.

4. **Los OEMs hostilizan**: ACPI DSDTs tienen bugs propietarios por laptop. "Windows-only" methods en AML. `_OSI("Linux")` vs `_OSI("Windows 2020")` retornando resultados distintos. Es battleground.

5. **El testing es caro**: hay que tener hardware físico, cicla decenas de veces, mira qué cuelga. Los CI farms de Linux (Intel's 0day, Linaro) tienen clusters dedicados a suspend/resume testing.

**Números del mundo real**:
- Linux `drivers/base/power/` + `kernel/power/`: ~15 KLOC core.
- Linux `drivers/cpufreq/` + `drivers/cpuidle/`: ~25 KLOC.
- Linux `drivers/thermal/`: ~20 KLOC.
- Suspend/resume callbacks across all drivers: ~200 KLOC.
- ACPI implementation (ACPICA): ~100 KLOC (pero reused from Intel reference).
- **Total: >350 KLOC** dedicados a power management en Linux mainline.

Esto es **25× el tamaño actual de ALZE completo** (14 KLOC).

**Recomendación concreta para ALZE**:

- **v1 (ahora)**: Wire `cpu_idle()` como idle task. Implementar ACPI S5 shutdown (200 LOC). Document explicitly: "ALZE is desktop-first, always-on". No suspend. No runtime PM. No thermal beyond watchdog panic at critical temp (if ever).

- **v2 (post-Sprint-3 del review)**: IF y solo IF ALZE targetea laptops, evaluar si S3 vale el costo. Probablemente **no**: para un kernel educativo, la perdida de energía de un laptop que nunca duerme es tolerable; el costo en LOC + correctness de S3 no lo es.

- **v3 (aspiracional)**: cpufreq + schedutil + runtime PM. Años de trabajo. Reserved para cuando ALZE tenga tracción de usuarios reales demandando "battery life".

**Alternativa pragmática**: ALZE podría asumir "siempre AC-powered" como design choice declarado, análogo a ReactOS (que no suspend en laptop mucho tiempo sin bugs) o la mayoría de hobby OSes (Serenity, ToaruOS). Esto es honesto y libera ~10 KLOC de scope. El día que ALZE se use en un laptop como daily driver será el día que importe — hasta entonces, desktop-only + "halts cleanly" es un perfecto target.

**El error a evitar**: abrir "Sprint 8: power management" sin más ambición declarada que "hay que tener algo". Sin target claro (laptop vs server vs embedded), power mgmt engulle tiempo sin producir valor. Si el target es servers/dev-VMs → power mgmt es literalmente 0 prioridad (gcloud/AWS no suspenden VMs, corren siempre). Si el target es embedded → el stack es distinto (no ACPI, sí DT + clkctrl + regulator framework). Si el target es desktop/laptop → es donde el 90% del costo vive. Elegir target **antes** de escribir una línea de ACPI.

