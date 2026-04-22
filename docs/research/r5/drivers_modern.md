# Modern driver frameworks + key devices — deep dive for ALZE OS

**Round:** 5 (cross-cutting kernel systems)
**Fecha:** 2026-04-22
**Autor:** agent (research-only)
**Longitud objetivo:** 300-500 líneas

R2 `review/irq_hal_drivers.md` disecciona el estado actual de ALZE:
`kb.c` (PS/2 Set-1), `uart.c` (16550 COM1), `pci.c` (mechanism-1 CF8/CFC
scan), `xhci.c` (detect-only, no command ring). HAL skeleton sin
routing (~30% de call sites lo usan). Total ~1200 LOC de drivers
cubriendo input teclado y console serial; ni NVMe, ni NIC, ni audio,
ni GPU. Este documento mapea cómo los kernels reales manejan hardware
moderno 2026 y qué de eso es copiable a ALZE. Lectura previa:
`review/irq_hal_drivers.md`, `linux_mainstream.md`, `windows.md`,
`r4/fuchsia_zircon.md` §DFv2. No se repite summary.

---

## 1. USB xHCI host controller

Cuatro generaciones de HC: UHCI (Intel USB 1.1), OHCI (Compaq USB 1.1),
EHCI (Intel USB 2.0, 2002), **xHCI** (Intel USB 3.0+, 2010). xHCI 1.2
(May 2019, 680 pp,
`intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf`,
archive `web.archive.org/web/2024*/intel.com/.../xhci`) añade USB 3.2
Gen 2×2 (20 Gbps) y USB4 (comparte PHY con TB3/4).

**Register layout** desde BAR0, cuatro bloques: Capability (RO —
`CAPLENGTH`, `HCIVERSION`, `HCSPARAMS1/2/3`, `HCCPARAMS1/2`),
Operational (`USBCMD`, `USBSTS`, `CRCR`, `DCBAAP`, `CONFIG`), Port
Register Set (4 dwords por puerto: `PORTSC`, `PORTPMSC`, `PORTLI`,
`PORTHLPMC`), Runtime (`MFINDEX` + Interrupter sets: `IMAN`, `IMOD`,
`ERSTSZ`, `ERSTBA`, `ERDP`), Doorbell Array (`DB[0..MaxSlots]`, slot 0
= commands, 1..N = endpoints). ALZE `xhci.c` hoy solo toca CAP + USBCMD
reset + PORTSC.

**Ring architecture** — todos los paths de datos usan TRBs (Transfer
Request Blocks, 16 B), cuatro rings: **Command Ring** (driver → HC),
**Transfer Ring** ×(slots × 32 endpoints, datos + setup/data/status
TRBs), **Event Ring** (HC → driver, con ERST para segmentos
discontiguos), y **DCBAA** (Device Context Base Array, indexado por
slot_id, apunta a Device Context 1 KB o 2 KB según `CSZ`). Mismo
patrón productor/consumidor shared-memory que io_uring/virtio/NVMe,
con **cycle bit** (TRB.C toggleado en cada wrap) en vez de head/tail
modulares.

**Enumeración mínima** para hablar con un device: (1) port reset
`PORTSC.PR=1`, wait `PRC`; (2) Enable Slot cmd → slot_id; (3) allocate
Device Context, escribir `DCBAA[slot_id]`; (4) init Input Context
(slot + ep0); (5) Address Device cmd (BSR=1 primero, luego BSR=0); (6)
GET_DESCRIPTOR(Device) vía Setup/Data/Status TRBs en ep0; (7)
SET_CONFIGURATION + Configure Endpoint cmd por cada endpoint.

**Hubs** son devices con status-change endpoint; hub class driver
poll, emite port-reset downstream, repite §enum con `route_string` en
slot context. **USB-C PD** lo gestiona un TCPM separado (Linux
`drivers/usb/typec/`) que negocia roles y Alt Modes (DP, TBT). **USB4 /
TB4** añade tunneling PCIe/DP/USB3 vía un Connection Manager (~30k LOC
en Linux `drivers/thunderbolt/`); al daisy-chainear una eGPU, la GPU
aparece como PCI device hotplug — el xHCI driver no la ve.

LOC: Linux `drivers/usb/host/xhci*.c` ~40k. Minimal funcional sin
isoch/LPM: ~3500-5000 LOC C.

---

## 2. NVMe

Reemplaza AHCI para SSDs PCIe. Spec "NVM Express Base Specification
2.0c" (NVMe Inc, Oct 2022, `nvmexpress.org/specifications/`, PDF
público gratis). NVMe 2.1 (2024) añade Computational Storage. PCIe Gen
5 x4 = 14 GB/s secuencial en drives consumer (990 Pro, SN850X).

**Queue model**: **Admin QP** (id=0, crea/destruye I/O queues,
firmware, logs) + **I/O QPs** (id=1..N, hasta 64k queues × 64k
entries). Linux NVMe driver crea 1 I/O queue per CPU con afinidad
IRQ MSI-X — elimina contention completamente.

**SQE = 64 B**: `opcode` (1), fuse/psdt (1), `cid` (2), `nsid` (4),
metadata ptr, PRP1/PRP2 o SGL1, `slba` (8), `nlb` (2), control flags.
**CQE = 16 B**: `cdw0` (4), `sq_head/id` (4), `cid` (2), `phase+status`
(2). Phase tag = cycle bit análogo. **PRP (Physical Region Page)** =
lista de phys addrs 4 KB-aligned, simple para ≤2 páginas; más grande ⇒
PRP list page via PRP2. **SGL (Scatter-Gather List)** = estructura más
rica (data/bit-bucket/segment/last), usada en Fabrics.

**Namespaces**: 1..N por controller, cada uno disco lógico con `nsid`,
LBA size (512/4096), PI. **ZNS (Zoned Namespaces, NVMe 2.0)** — namespace
dividido en zonas secuenciales (256 MB típico, write-sequential-only),
elimina SLC-cache/GC overhead. FS amigos: f2fs, zonefs, btrfs-zoned.
QEMU `nvme` emula ZNS para dev.

LOC: Linux `drivers/nvme/host/` ~20k. Minimal (admin + 1 I/O queue +
PRP + polled, sin MSI-X): ~1500 LOC C. Refs: HelenOS
`uspace/drv/block/nvme/`, Biscuit OS `biscuit/nvme.c` (~800 LOC).

---

## 3. PCIe + PCI Express config space

**Legacy mechanism-1** (ALZE `pci.c` actual): write `0x8000_0000 |
bus<<16 | dev<<11 | fn<<8 | reg` a 0xCF8, r/w dword desde 0xCFC. Scan
completo ~65k I/O × 4 = ~10 s en hardware real. **ECAM (PCIe 1.0,
2003)** — MMIO con base en tabla ACPI **MCFG**; addr = `ecam_base +
bus<<20 | dev<<15 | fn<<12 | reg`. 256 MB de address space para 256
buses × 32 dev × 8 fn × 4 KB config. Acceso = 1 MOV MMIO (µs vs ms).
PCIe expande config de 256 B a **4 KB** por función; regs 256-4095 son
**solo vía ECAM**. Spec "PCI Express Base Specification Revision
6.0.1" (PCI-SIG, Aug 2022, `pcisig.com/specifications/pciexpress/`,
members-only; copias en archive). Gen 6 = 64 GT/s PAM4, Gen 7 (2025) =
128 GT/s.

**BARs**: 6 × 32-bit (o 3 × 64-bit). Sizing algorithm: (1) write
`0xFFFFFFFF`; (2) read back; (3) mask low 4 bits MMIO o 2 bits I/O;
(4) `~masked + 1` = size; (5) restore original. ALZE no hace sizing
(R2 P1). Bit 0: 0=MMIO/1=I/O. Bit 2:1 (MMIO): 00=32-bit, 10=64-bit.
Bit 3: 1=prefetchable (WC safe).

**Capabilities** — config offset 0x34 = Capabilities Pointer (byte).
Linked-list walk: header `u8 cap_id, u8 next_ptr, ...`. IDs relevantes:
0x01=PM, 0x05=MSI, 0x10=PCIe, 0x11=MSI-X, 0x14=PCIe Advanced Features.
**Extended capabilities** (offset ≥256, ECAM-only): header 32-bit
`cap_id:16, version:4, next:12`. IDs: 0x0001=AER, 0x000D=ACS,
0x000F=ATS, 0x001E=L1 PM Substates, 0x0024=SR-IOV.

**MSI-X** — cap contiene table BAR+offset + PBA BAR+offset +
table_size (2048 max). Cada entry = 16 B: `addr_lo, addr_hi, data,
vector_ctrl`. x86: address encoding `FEE00000 | dest<<12`; data =
`trigger | delivery_mode | vector`. Driver programa cada entry con
vector IDT distinto → IRQ independent per queue (NVMe: 1 entry per
CPU, afinidad igual).

**ACS (Access Control Services)** previene peer-to-peer DMA entre
devices bajo el mismo bridge. Sin ACS, IOMMU no puede aislar devices
en grupos distintos → passthrough solo del grupo entero a una VM.
**IOMMU** (Intel VT-d, AMD-Vi, ARM SMMU) = page tables hardware para
DMA; protege de device malicioso o buggy que DMAs fuera del buffer.
Linux `iommu/` ~50k LOC. Microkernels (seL4 `camkes-vm`, Redox)
aprovechan IOMMU por diseño — driver user-space recibe capability
`IOMMU_DOMAIN` y DMAs solo a IOVAs remappeadas.

---

## 4. Network stack

Kernel separation: **driver NIC** (rings TX/RX, ISR, NAPI) ↔ **net
core** (sk_buff queue, netfilter hooks) ↔ **protocol layers** (ARP, IP,
TCP, UDP, ICMP) ↔ **socket API**.

**Drivers NIC clave**:
- Intel **e1000e** (desktop 1 GbE, ~15k LOC): 82574+.
- Intel **igc** (i225/i226, 2.5 GbE, ~12k LOC).
- Intel **ixgbe** (82599/X540/X550, 10 GbE, ~50k LOC). TSO + checksum
  offload + MSI-X.
- RealTek **r8169** (RTL8169/8168/8125, 1-2.5 GbE consumer, ~18k LOC).
  Chip-variant hell — tablas magic-init per PCI_ID.
- Mellanox **mlx5** (CX-5/6/7, 100-400 GbE, ~200k LOC). RDMA, SR-IOV,
  vxlan offload, hw timestamping. Control plane via admin queue FW
  (mismo patrón que NVMe).

**TCP/IP implementations**:
- **lwIP** (A. Dunkels, SICS 2001, `savannah.nongnu.org/projects/lwip/`,
  ~15k LOC, BSD). Standard embebido. Usado en ESP-IDF, FreeRTOS+TCP,
  Xen Mini-OS, QEMU user-mode net.
- **picoTCP** (Altran 2014, `picotcp.com/`, ~40k LOC). Más modular, TCP
  + SCTP + MQTT + CoAP.
- **Linux `net/`** ~1.2M LOC — su propio universo.
- **seL4 TCP**: camkes-uses lwIP en component con TCB capability-bound.
  Ref: Heiser et al. "Component-Based Construction of a TCP/IP Stack
  for seL4" (NICTA 2013, `trustworthy.systems/publications/`).

**io_uring para network** — desde 5.5 soporta `SEND/RECV/ACCEPT/CONNECT`;
5.11 multishot accept; 6.0+ `SEND_ZC` zero-copy con notificaciones.
ABI de elección 2026 para servidores HTTP high-throughput.

**DPDK** (Data Plane Development Kit, Intel 2010,
`dpdk.org/`, `doc.dpdk.org/guides/prog_guide/`) — driver NIC completo
en userspace vía UIO/VFIO, kernel out-of-path, polled mode (sin IRQ),
huge pages (sin TLB misses), lock-less rings. Throughput 200-400 Gbps
single socket; paquete ~50 ns en PMD. Users: VPP (Cisco), OvS-DPDK,
TRex, Seastar (ScyllaDB). Simétrico: **SPDK** (Intel 2015,
`spdk.io/doc/`) — user-space NVMe, 10M+ IOPS single core. Users:
Ceph BlueStore variants, Intel VROC.

**Wi-Fi 7 (802.11be, 2024)** — MLO multi-link, 320 MHz, 4K-QAM.
Complejidad: Intel AX411/BE200, Linux `drivers/net/wireless/intel/iwlwifi/`
~100k LOC + firmware blob MB. **Hobby OS: inviable sin firmware.**
**BT 5.4 (2023)** — PAwR. Stack Linux BlueZ daemon + hci driver. Hobby
OS: BR/EDR vía HCI-over-USB ~3k LOC es factible; LE + profiles = otro
universo.

---

## 5. Linux driver model

Ref: "Linux Device Drivers 3rd ed" (Corbet/Rubini/Kroah-Hartman,
O'Reilly 2005, free PDF `lwn.net/Kernel/LDD3/`; desactualizado post-2.6
pero intro canónica). Docs vivas `kernel.org/doc/html/latest/driver-api/`.

**Core structures**:
- **kobject** (`include/linux/kobject.h`) = refcount + parent + ktype +
  sysfs dir. Todo lo que la tiene aparece en `/sys/`.
- **struct device** (`include/linux/device.h`) = device concreto con
  parent, bus, driver, dma_mask, pm_info.
- **struct device_driver** = metadata + `probe/remove/shutdown/
  suspend/resume`.
- **struct bus_type** = match function (PCI vendor/device id, USB
  VID/PID/class, platform string o DT compatible), probe hooks, uevent.
  Instancias: pci_bus_type, usb_bus_type, platform_bus_type,
  i2c_bus_type, spi_bus_type.

**Binding**: `device_register` → itera drivers del bus → `match(dev,
drv)` → primer hit llama `probe(dev)` → 0 = bind. **Hotplug**: pciehp
ISR → `pci_bus_add_device` → `device_add` → match → probe; `remove()`
en unplug; refcount protege use-after-free.

**DT vs ACPI vs manual**: Device Tree (`.dts` compilado a `.dtb`,
bootloader lo pasa, `compatible = "vendor,chip"`) en ARM/PPC/RISC-V;
ACPI (DSDT con `_HID` y `_CID`, métodos AML bytecode Turing-completo
ejecutado por ACPICA) en x86 y AArch64 servers; manual probing (ports
fijos) para ISA legacy extinto.

**Class hierarchy + sysfs**: `/sys/class/{net,block,input,tty}/` agrupa
por función, independiente del bus físico. `/sys/devices/` = árbol
físico. `/sys/bus/pci/drivers/e1000e/{bind,unbind,new_id,remove_id}`
permite reconfig runtime.

LOC: `drivers/base/` ~30k. Minimal "bus + driver + device + probe/remove":
~800 LOC.

---

## 6. Windows Driver Framework (WDF)

**"Developing Drivers with the Windows Driver Foundation"** (Penny
Orwick + Guy Smith, Microsoft Press 2007, ISBN 978-0-7356-2374-3).
Nota: el plan cita "Tony Cook"; no hay libro oficial Cook — el
canónico es Orwick/Smith. Docs: `learn.microsoft.com/windows-hardware/
drivers/wdf/`.

**WDM era (pre-2005)**: IRP (I/O Request Packet) + `DriverEntry` +
`AddDevice` + dispatch table (`IRP_MJ_READ/WRITE/...`). Todo manual:
PnP, power, locks, completions. BSOD magnet.

**WDF = KMDF + UMDF.** Framework on top of WDM; driver compila a `.sys`
linkeando `wdf01000.sys` runtime. Handle-based API (`WDFDEVICE`,
`WDFREQUEST`, `WDFQUEUE`, `WDFIOTARGET`), event-driven callbacks en
vez de dispatch switch. Object hierarchy con auto-cleanup. **KMDF** =
kernel-mode (hardware directo). **UMDF v2** = user-mode, proceso
separado `WUDFHost.exe` (crash aislado). Callbacks PnP/power:
`EvtDevicePrepareHardware`, `EvtDeviceD0Entry/Exit`, `EvtDeviceReleaseHardware`.
Framework maneja state machine; driver implementa solo callbacks
relevantes. Race conditions WDM quedan ocultos.

**I/O queues** `WDFQUEUE` con parallel/sequential/manual dispatch;
framework serializa o paraleliza. `EvtIoRead/Write/DeviceControl`.

**Filter drivers**: PnP stack permite apilar upper-filter + function +
lower-filter + bus. Anti-malware, BitLocker FVE, Hyper-V vSMB usan
filters.

---

## 7. Fuchsia DFv2

Ref: `fuchsia.dev/fuchsia-src/development/drivers/concepts/`, Google
2022+. Reemplaza DFv1 (state-machine manual). Ver `r4/fuchsia_zircon.md`.

**Driver como componente** v2 (CML manifest), process aislado. Zircon
nunca carga drivers — los carga `driver_manager` user-space que
también hace binding.

**Binding declarativo** en `.bind` DSL → bytecode:
```
using fuchsia.pci;
fuchsia.BIND_PROTOCOL == fuchsia.pci.BIND_PROTOCOL.DEVICE;
fuchsia.BIND_PCI_VID == 0x8086;
fuchsia.BIND_PCI_DID == 0x1533;  // i210
```
`driver_manager` evalúa contra device properties; match → bind. Rules
inspeccionables sin leer código.

**FIDL**: drivers comunican vía FIDL protocols (IDL con stubs
Rust/C++/Dart). Un block driver expone `fuchsia.hardware.block/Block`;
un FS consume sin saber si el implementor es NVMe/USB MSC/ramdisk.
Decoupling perfecto.

**Restart + isolation**: driver crash → component manager restart;
`driver_host` reintenta bind. Driver comprometido no tiene acceso al
kernel, solo a sus FIDL handles. Anti-rootkit natural.

---

## 8. User-space drivers

**FUSE** (Szeredi 2001, `github.com/libfuse/libfuse`) — kernel
`fuse.ko` presenta VFS mount; I/O se serializa a chardev y daemon
userspace procesa. Overhead ~3-10 µs por syscall (2 context switches).
Users: sshfs, s3fs, gocryptfs. Adecuado para metadata-heavy.

**UIO** (Linux, H.J. Koch 2007) — kernel shim ~100 LOC per device
expone `/dev/uioN`; userspace mmap'a BAR, lee IRQ via `read()` (shim
→ eventfd). DPDK legacy, FPGA drivers.

**VFIO** (A. Williamson 2012) — reemplazo UIO IOMMU-aware para
virtualization. Userspace: container, attach groups, programa IOMMU
mappings vía `VFIO_IOMMU_MAP_DMA`. Isolation real. Users: KVM PCIe
passthrough (GPU/NIC), DPDK moderno.

**SPDK/DPDK** = user-space NVMe/NIC polled, HugeTLB, lock-less;
§2 y §4.

**Regla**: drivers legacy + simple + universally-needed → kernel;
drivers high-throughput específicos (network functions, storage
engines dedicados) → user-space.

---

## 9. Interrupt handling

**Evolution**: PIC 8259A (ALZE actual, 15 líneas, cascaded, ports
0x20/0xA0) → IOAPIC (Intel 1996, MMIO, 24 lines, redirige a cualquier
LAPIC; necesario para ≥2 CPUs) → MSI (PCI 2.2, 1999, device escribe
dword a `FEE00000` magic, 1 vector per device) → MSI-X (PCIe 2006,
≤2048 vectors per function, tabla indexada, per-queue afinidad) →
x2APIC (MSR-based vs MMIO, 2^32 CPUs vs 256, default Linux 3.x+).

**Linux top/bottom half**:
- **Top half (hardirq)**: ISR directo. Rápido, corto, ACK hardware,
  schedule bottom half. IF=0 durante ejecución.
- **Bottom half**: (1) **softirq** 10 vectors globales priorizados
  (NET_RX/TX, BLOCK, TIMER, ...); (2) **tasklet** sobre TASKLET_SOFTIRQ,
  deprecated — se reemplaza por workqueue; (3) **workqueue** kernel
  threads con callbacks schedulables que pueden bloquear, default
  moderno. **`request_threaded_irq`** corre el handler en kthread RT;
  PREEMPT_RT lo hace default para todas las IRQs → latencia
  determinística.

**Windows DPC** (Deferred Procedure Call) ≈ softirq. ISR completa
rápido + `KeInsertQueueDpc`; DPCs corren en IRQL=DISPATCH_LEVEL antes
del return a passive level. Per-CPU queue. Vista+ añade **Threaded DPCs**
≈ threaded IRQ Linux.

**Interrupt moderation** — sin moderación, NIC 100 Gbps @ 64 B = 150
Mpps = 150 M IRQ/s = CPU pegged. Solución: hardware coalesce (wait N
paquetes OR M µs). Intel IAA: adaptive moderation dinámica.

---

## 10. Modern hardware complexity 2026

Lo que enfrentas apuntando a real HW:
- **M.2 NVMe SSD**: NVMe + PCIe Gen 4/5 hotplug + thermal throttling +
  PS4 state + SMART + TCG Opal. 1500+ pp specs.
- **USB4/TB4**: tunnel PCIe/DP/USB3 sobre mismo cable, 40 Gbps, daisy-
  chain hotplug. CM ~30k LOC en Linux.
- **Wi-Fi 7** (802.11be): MLO, 320 MHz, firmware blob MB.
- **BT 5.4**: BR/EDR + LE + mesh + LE audio (LC3).
- **DisplayPort 2.1**: UHBR20 (80 Gbps), DSC, Alt Mode sobre USB-C.
- **GPU**: PCIe + DMA-BUF + dma-fence + VRAM/GTT domains + GuC
  firmware + MIG partitioning. i915 ~500k LOC.
- **ARM PMU + ETM**: perf counters + embedded trace. CoreSight
  framework en `drivers/hwtracing/coresight/`.
- **Audio**: Intel HDA (ACPI `PNP0000`), SoundWire, USB audio class 3.

Cada uno = semanas-meses de experto fulltime.

---

## 11. Tables

### 11.1 Driver frameworks comparison

| Framework | Kernel/User | Binding | Hotplug | Crash restart | IDL | Core LOC |
|---|---|---|---|---|---|---|
| Linux kobj + driver_model | kernel | PCI/USB/platform/DT compatible | yes (pciehp, usb hub) | no (oops → panic) | no | ~30k |
| Windows WDF (KMDF/UMDF) | kernel or user | PnP manager + INF | yes | UMDF yes; KMDF no | no | ~60k |
| Fuchsia DFv2 | user (component) | `.bind` bytecode | yes | yes | FIDL | ~40k |
| FreeBSD newbus | kernel | identify + probe + attach | yes | no | no | ~15k |
| seL4 CAmkES drivers | user | component composition (.camkes) | offline typical | yes (camkes-restart) | CAmkES IDL | ~5k/driver |
| Redox schemes | user | scheme URL (`pci:`, `disk:`) | limited | yes (scheme daemon) | no (byte streams) | ~3k |

### 11.2 Key device interfaces

| Iface | Command model | Queue count | DMA pattern | Spec pp |
|---|---|---|---|---|
| USB xHCI | TRBs in rings (cmd + xfer + event) | 1 cmd + 1024 slots × 32 ep | driver buffer pointers | 680 |
| NVMe | SQE+CQE per QP | ≤64k QP (1 per CPU typical) | PRP lists or SGLs | 510 |
| PCIe ECAM | config dword r/w | n/a | n/a (config only) | 1200 |
| virtio 1.2 | descriptor ring (split or packed) | 1 per virtqueue (2-16 typical) | indirect desc optional | 180 |
| AHCI SATA | command list + FIS receive | 32 cmd × 32 ports | PRDT scatter list | 140 |

### 11.3 ALZE applicability

| Feature | v1 (next) | v2 (after capability model) | v3 (aspirational) |
|---|---|---|---|
| Fix R2 P0 drivers | IDT stubs, spurious IRQ7/15, xHCI PCD\|PWT, LAPIC fallback | — | — |
| Route all via HAL | `hal_irq_eoi` + `hal_timer_tick` everywhere | — | — |
| PCI ECAM fast-path | try MCFG first, CF8/CFC fallback | — | — |
| virtio-blk | ~400 LOC, replaces ramdisk | — | — |
| virtio-net | ~500 LOC, lwIP port | — | — |
| virtio-console | ~200 LOC, preferred serial | — | — |
| MSI-X | — | PCI cap walk + table programming + per-vector IDT | — |
| IOMMU (VT-d) | — | DMAR parse, domain per device | — |
| xHCI command + event ring | — | HID kbd+mouse over USB | USB MSC |
| NVMe | — | — | minimal (admin + 1 I/O queue) |
| e1000e/igc real | — | — | no TSO/checksum offload |
| User-space drivers (DFv2-style) | — | — | driver hosts + FIDL-like IPC |
| ACPI interpreter | — | ACPICA (~200k LOC) or AML subset | — |
| Wi-Fi / Bluetooth | — | — | **not realistic for solo hobby** |
| GPU (modeset only) | — | simple DRM with limine | VirtIO GPU 2D; full accel unrealistic |

---

## 12. Primary references

1. Intel — "eXtensible Host Controller Interface for Universal Serial
   Bus (xHCI) Requirements Specification, Revision 1.2", May 2019.
   `intel.com/.../extensible-host-controler-interface-usb-xhci.pdf`.
   Archive: `web.archive.org/web/2024*/intel.com/.../xhci.pdf`.
2. NVM Express Inc. — "NVM Express Base Specification 2.0c", Oct 2022.
   `nvmexpress.org/specifications/`. Free PDF.
3. PCI-SIG — "PCI Express Base Specification Revision 6.0.1", Aug
   2022. `pcisig.com/specifications/pciexpress/` (members). Archive
   via unofficial copies.
4. Corbet, Rubini, Kroah-Hartman — "Linux Device Drivers, 3rd ed",
   O'Reilly 2005. Free `lwn.net/Kernel/LDD3/`.
5. Linux Kernel Driver API docs, kernel.org 2026.
   `kernel.org/doc/html/latest/driver-api/`.
6. Orwick + Smith — "Developing Drivers with the Windows Driver
   Foundation", Microsoft Press 2007, ISBN 978-0-7356-2374-3.
7. Microsoft — Windows Driver Frameworks docs, 2026.
   `learn.microsoft.com/windows-hardware/drivers/wdf/`.
8. Google — "Fuchsia Driver Framework v2 Concepts", 2022-2026.
   `fuchsia.dev/fuchsia-src/development/drivers/concepts/`.
9. SPDK docs, Intel 2015-2026. `spdk.io/doc/`.
10. DPDK Programmer's Guide, 2010-2026. `doc.dpdk.org/guides/prog_guide/`.
11. Szeredi, M. — "FUSE: Filesystem in Userspace", 2001+.
    `github.com/libfuse/libfuse`; doc
    `kernel.org/doc/Documentation/filesystems/fuse.txt`.
12. Axboe, J. — "Efficient IO with io_uring", kernel.dk 2019-2020.
    `kernel.dk/io_uring.pdf`.
13. virtio 1.2 Specification, OASIS 2022.
    `docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.pdf`.
14. Heiser, G. et al. — "Component-Based Construction of a TCP/IP
    Stack for seL4", NICTA 2013. `trustworthy.systems/publications/`.
15. Dunkels, A. — "Design and Implementation of the lwIP TCP/IP
    Stack", SICS 2001. `savannah.nongnu.org/projects/lwip/`.

---

## 13. Honest note — hostile hardware

Modern hardware es hostil a hobby kernels. Cada spec:
- NVMe base: ~510 pp
- xHCI 1.2: 680 pp
- PCIe 6.0: ~1200 pp
- USB-C PD 3.1: ~650 pp
- Wi-Fi 802.11be: ~5000 pp
- Intel SDM Vol 3A-3D: ~3000 pp solo system programming
- Per-vendor firmware blobs: undocumented, SHA-locked

Un NIC real = semanas-meses de experto fulltime. Una GPU accelerated =
years. Linux ~6000 drivers; no una persona.

**Política realista ALZE: virtio-only v1.** QEMU/KVM exponen virtio-*
(blk, net, console, gpu, input, rng, balloon, scsi, fs) con spec
pública abierta (OASIS virtio 1.2, 180 pp). Cada driver virtio:
~300-600 LOC. **Todo el stack virtio en ALZE cabría en ~3000 LOC.**
Funciona en QEMU, bhyve, crosvm, Firecracker, cloud-hypervisor — todos
los hypervisores que importan en 2026.

Ship funcional en QEMU antes de real HW. Real HW drivers = v3
aspirational; para la mayoría de hobby OSes "v3" = "never". SerenityOS
(3+ años, 5+ devs rotación) tiene e1000/e1000e/RTL8139/RTL8168 —
conseguido a coste enorme. ToaruOS (1 dev) sigue en virtio-net + VBE
emulado. Ese es el techo realista.

Lección operativa: la **ABI** del driver layer (HAL + `hal_irq` +
capability model v2) merece diseñarse bien desde ahora — el coste de
cambiarla tarde es alto. Los **drivers concretos** importan menos: se
portan de Serenity/Redox/ToaruOS en days si la ABI es buena. **Invertir
en ABI, no en drivers reales.**

Próximo paso táctico: **virtio-blk** en ALZE (~500 LOC, spec bien
documentada, test inmediato en QEMU con `-drive if=virtio`) antes de
nada más.
