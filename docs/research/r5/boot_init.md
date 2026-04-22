# Boot Loaders and Early Init — Deep Dive for ALZE OS

Round: R5 cross-cutting. Date: 2026-04-22. Scope: everything between
power-on-reset and the first C instruction of the kernel, plus what
the kernel must still do itself after the bootloader hands off.
Cross-references R2 `review/boot_build.md` (concrete findings on the
current ALZE limine integration).

The prevailing hobby-OS mistake is to treat the bootloader as magic:
"Limine did everything, main() runs, done." It did not. Limine
populates a handful of responses and hands over a BSP in long mode
with paging on and a loader GDT/IDT. The kernel must still: parse
ACPI (RSDP → XSDT → MADT/FADT/MCFG/DSDT), boot APs via INIT-SIPI-SIPI,
install its own GDT/IDT, set EFER.NX, configure the LAPIC, wire x2APIC
if present, and bring up its own page tables with W^X. Every one of
those is a source of silent triple-faults if skipped. This doc maps
out the whole territory so ALZE's follow-on work doesn't re-learn
lessons the ecosystem already documented.

## 1. Limine (current ALZE choice — deep)

**Maintainer**: mintsuki (real name Mattias Svensson, Swedish).
Active since ~2020. Sole-maintainer project with a small contributor
set; the protocol is versioned and the stage-2 loader is C + nasm.
Upstream: <https://github.com/limine-bootloader/limine>. Protocol
spec: <https://github.com/limine-bootloader/limine/blob/v8.x/PROTOCOL.md>.

**Lineage**. Limine began as a stivale/stivale2 loader (stivale2 ≈
Multiboot2 but with tagged responses and a better long-mode handoff).
In 2022 mintsuki deprecated stivale2 in favour of a new **limine
protocol** (separate from stivale2) that uses *anchored request
structures* instead of tags: the kernel embeds `volatile struct
limine_*_request` objects in dedicated sections, the bootloader scans
the ELF for start/end markers and fills `.response` in place.

**Protocol revisions** (as of 2026):
- rev 0 (2022): initial.
- rev 1: clarified HHDM, added module string IDs.
- rev 2: framebuffer mode array, EDID response, better multi-FB.
- rev 3 (current, ALZE uses this — `main.c:59`): well-defined paging
  mode request (4-level vs 5-level), executable file request
  replaces the older kernel_file response, tightened memmap semantics
  (bootloader-reclaimable is explicit, kernel-and-modules is one
  type). ALZE's `LIMINE_BASE_REVISION(3)` KASSERT at `main.c:211`
  verifies acceptance — if the installed loader is older, the
  `limine_base_revision` third word is non-zero and the kernel
  panics cleanly on UART rather than faulting.
- rev 4 (pre-release 2026): adds SMP wake-up via NMI option, better
  kernel-address request, a first-class TLSF hint. Not yet stable —
  wait.

**What limine guarantees on entry to `_start`** (ALZE `main.c:284`):
- Long mode, CS flat 64-bit, DS/ES/SS flat data, IF=0.
- Paging on, 4-level (or 5-level if requested + supported).
  CR0.PG=PE=WP=1. CR4.PAE=1, CR4.PSE, OSFXSR *not* set (SSE off in
  ALZE). EFER.LME=LMA=1; **EFER.NX NOT guaranteed** — kernel must
  enable (R2 issue 15).
- 16 KiB stack, 16-byte aligned.
- Loader GDT active (throwaway), loader IDT active (every vector =
  halt stub — you MUST install your own before `sti`).
- HHDM: physical memory mapped at `hhdm_offset` (typically
  `0xffff800000000000` for 4-level) if HHDM request made. ALZE reads
  `hhdm_request.response->offset` for `PHYS2VIRT`/`VIRT2PHYS`.
- Kernel loaded at linked virtual address (ALZE: `0xffffffff80001000`,
  per `readelf -lW`).
- All granted request `.response` fields populated.

**What limine does NOT do**:
- No AP boot (rev ≤3). Kernel issues INIT-SIPI-SIPI itself (§10).
  Rev 4 draft adds opt-in AP-wake.
- No ACPI parsing (hands over RSDP pointer if you request it; XSDT /
  MADT / MCFG / FADT / DSDT walking is on you).
- No syscall MSRs (IA32_LSTAR/STAR/FMASK, EFER.SCE) — kernel policy.
- No SSE/AVX enable; no NX enable; no GDT/IDT install; no serial.

**Limine features ALZE doesn't currently use but should consider**:
- `limine_rsdp_request` — unlocks ACPI (see §9). 10 LOC addition.
- `limine_kernel_address_request` — explicit phys/virt kernel base
  without inferring from linker symbols.
- `limine_module_request` — initrd-style modules (future
  `/sbin/init`).
- `limine_smbios_request` — board provenance in boot banner.
- `limine_paging_mode_request` — opt into 5-level (LA57) on Ice
  Lake+ servers. Currently ALZE assumes 4-level.
- `limine_efi_system_table_request` — UEFI runtime services for
  NVRAM / reset.

**Protocol shape**. Start/end markers at `main.c:52-56` are linker
anchors the loader scans the ELF for. Between them, each
`limine_*_request` struct has an 8-byte ID (two magic UUIDs per
request type); the loader writes `.response` in place. `volatile`
prevents compiler caching of pre-boot zero reads. Section naming
(`.limine_requests*`) is required by rev 3. The `/Anykernel OS v2.1`
comment in `limine.conf` is just an entry title — limine supports
multi-entry boot menus, not semantic.

## 2. GRUB 2

GNU GRUB 2, maintained by Daniel Kiper (Oracle). ~55k LOC core +
extensive fs/network drivers. Supports **Multiboot 1** (2004, 32-bit
only, obsolete), **Multiboot 2** (2010, tagged info — RSDP, SMBIOS,
EFI system table, memmap, framebuffer — but **hands off in 32-bit
protected mode, not long mode**; the kernel builds its own trampoline).
Also chainloads EFI binaries and implements the Linux boot protocol
natively.

Config is a quasi-Bourne scripting language; `grub.cfg` is generated
by `grub-mkconfig` from `/etc/default/grub` + `/etc/grub.d/*`. Value
is its 50+ filesystem drivers (ext4, btrfs, XFS, LVM, LUKS1/2, ZFS),
TCP/IP stack for PXE/HTTP boot, themes — all irrelevant for a hobby
kernel booting one ELF.

**Security**: GRUB itself is not signed; Fedora/Ubuntu chain through
Microsoft-signed `shim.efi` which verifies `grubx64.efi` via MOK. The
**BootHole CVE-2020-10713** (buffer overflow in font handling) led to
the SBAT (Secure Boot Advanced Targeting) revocation metadata model
now universal in the ecosystem.

**Overkill for ALZE.** Limine's entire loader is <30k LOC and vendorable.
Ref: Kiper, "GRUB2: Present and Future", OSSummit EU 2022.
<https://lwn.net/Articles/899286/>.
MB2 spec: <https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html>.

## 3. systemd-boot (sd-boot)

UEFI-only boot manager merged from gummiboot into systemd in 2015
(Poettering et al). ~8k LOC, in `systemd/src/boot/`. No scripting, no
filesystem drivers — firmware reads FAT32 ESP natively. Each entry is
a `.conf` file: `linux`, `initrd`, `options`.

Modern direction is **Unified Kernel Images (UKI)**: one PE/COFF
`.efi` embedding `vmlinuz` + initrd + cmdline + signature, integrates
with TPM2 PCR 11/12 + `systemd-cryptenroll` for measured boot. No
legacy BIOS, no network boot, Linux-specific vocabulary — not
applicable to ALZE unless ALZE ever ships as a UKI wrapper (heavier
than limine for a hobby OS).

Ref: systemd-boot(7). Poettering, "The Strange State of Authenticated
Boot...", 2021.
<http://0pointer.net/blog/authenticated-boot-and-disk-encryption-on-linux.html>.

## 4. rEFInd

UEFI boot manager by Rod Smith, derived from rEFIt. ~12k LOC. Unique
because it graphically scans every attached drive for EFI loaders and
presents icons — the dual/triple-boot champion (Windows Boot Manager +
macOS `boot.efi` + Linux stubs, auto-discovered). Supports UEFI
filesystem driver loading (ext4/btrfs/NTFS as UEFI drivers). Not
applicable to ALZE (single-kernel ISO boot).

Ref: <https://www.rodsbooks.com/refind/>. Smith's Secure Boot + custom
keys writeups are the authoritative hobby-level resource.

## 5. UEFI + Secure Boot

**Spec**: UEFI 2.10 (2024), 3.0 draft (2026-2027). ~2500 pages.
<https://uefi.org/specifications>.

Key concepts:
- **Boot services** (AllocatePages, LocateProtocol, OpenProtocol):
  available until kernel calls `ExitBootServices()`. **Runtime
  services** (GetVariable, SetVariable, ResetSystem, GetTime): survive,
  but require `SetVirtualAddressMap()` to relocate.
- **GPT** replaces MBR: 128 partitions default, 2 TB+ disks,
  CRC-protected, partition GUIDs. Hybrid MBR is a legacy-interop wart.
- **ESP (EFI System Partition)**: FAT32, GUID
  `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`; every UEFI loader lives here
  as PE/COFF.
- **Protocols** (COM-style, by GUID):
  - `EFI_GRAPHICS_OUTPUT_PROTOCOL` (GOP) — framebuffer; Limine uses
    GOP to populate `limine_framebuffer_response`.
  - `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` — read ESP.
  - `EFI_SIMPLE_NETWORK_PROTOCOL`, `EFI_PXE_BASE_CODE_PROTOCOL` —
    network boot.
  - `EFI_BLOCK_IO_PROTOCOL`, `EFI_DEVICE_PATH_PROTOCOL`,
    `EFI_LOADED_IMAGE_PROTOCOL`.
- **EDK2 / TianoCore** (Intel): open-source reference,
  <https://github.com/tianocore/edk2> (~2.5M LOC). OVMF is EDK2 for
  QEMU — `qemu-system-x86_64 -bios OVMF.fd`.

**Secure Boot chain**:
- **PK** (Platform Key): OEM or user-owned, signs KEK.
- **KEK**: signs db/dbx updates.
- **db**: allowed-signer list (Microsoft UEFI CA, etc).
- **dbx**: revocation list (BootHole, LogoFAIL appended here).
- **MOK**: shim's sidechannel for user-enrolled keys without touching
  OEM db.

**For ALZE**: Secure Boot support = getting limine shim-signed
(mintsuki's SBAT is pending upstream). Ecosystem problem, not kernel
problem. Don't design around it now.

Refs: UEFI Forum, *UEFI Specification v2.10*, 2024. Zimmer / Rothman /
Marisetty, *Beyond BIOS*, 3rd ed., Intel Press, 2017.

## 6. Legacy BIOS

Phase chain: reset vector `0xFFFFFFF0` → firmware POST → sector 0
(MBR, 512 B) loaded at `0x7C00` in **16-bit real mode**, IF=0 →
stage-1 code reads VBR → stage 1.5/2 brings in the real bootloader →
real mode → **32-bit protected mode** (CR0.PE, GDT, long jump) → reads
kernel → **64-bit long mode** (PAE, EFER.LME, paging, long jump).

MBR layout: 446 B code + 64 B partition table + `0x55 AA` signature.
Four primary partitions + extended hack; 32-bit LBA ⇒ 2 TB. GPT was
standardized precisely to kill these.

2026 reality: Intel dropped CSM from Tiger Lake (2020) client silicon
onwards; servers followed 2022-2023. QEMU/SeaBIOS still works.
BIOS-boot is retrocomputing + VM-test only. Limine's hybrid ISO covers
both, which is why ALZE's `make iso` target still produces a BIOS boot
sector — useful for QEMU test coverage.

Refs: OSDev Wiki, "BIOS" <https://wiki.osdev.org/BIOS>; "MBR"
<https://wiki.osdev.org/MBR>; "Real Mode"
<https://wiki.osdev.org/Real_Mode>.

## 7. Stivale2 (historical)

Limine's predecessor protocol (deprecated 2023). Tagged-struct handoff:
header tags (kernel → loader) request features, info tags (loader →
kernel) chain via `next` pointer. Already did long-mode entry, HHDM,
clean FPU state, memmap with `bootloader_reclaimable`, framebuffer +
EDID, stable stack — everything the limine protocol kept. Changes in
the new protocol: anchored request structs instead of a walking list,
responses written in-place, `LIMINE_BASE_REVISION(n)` acceptance
sentinel, 32-byte UUIDs instead of numeric tag IDs.

| Feature              | Multiboot2    | Stivale2         | Limine proto |
|----------------------|---------------|------------------|--------------|
| Long-mode entry      | no            | yes              | yes          |
| Info passing         | tagged list   | tagged list      | in-struct    |
| HHDM                 | no            | optional         | optional     |
| Framebuffer          | yes           | yes              | yes          |
| ACPI RSDP            | tag           | tag              | request      |
| SMP wake             | no            | optional         | rev 4 draft  |
| Multi-kernel loader  | GRUB          | limine/tomatboot | limine       |

Ref: Mintsuki, stivale2 spec (deprecated),
<https://github.com/stivale/stivale/blob/master/STIVALE2.md>.

## 8. Device Tree Blob (DTB) / FDT

Used on Arm, RISC-V, PowerPC, MIPS, SPARC. **Not x86_64** (which uses
ACPI). Arm SystemReady-IR uses DTB; SystemReady-SR (server) uses ACPI.

`.dts` source compiled by `dtc` → `.dtb` flat binary. Spec: DeviceTree
Specification v0.4, 2022. <https://www.devicetree.org/specifications/>.

Bootloader (U-Boot, EDK2, coreboot) passes DTB pointer in a known
register (Arm64 uses x0, per Linux boot protocol). Kernel parses FDT,
walks nodes, matches `compatible` strings. Nodes describe CPUs,
memory, interrupt controller, clocks, regulators, pin-mux, every SoC
peripheral. `.dtbo` overlays apply at runtime (hotplug / board
variant). Minimal libfdt ~2k LOC is enough to bootstrap a from-scratch
kernel.

**Relevance to ALZE**: none near-term (x86_64 only). If ALZE ever
ports to Arm or RISC-V, DTB parsing is mandatory on most SoCs (no
ACPI fallback). Plan late, not first.

Ref: Antoniou, "An Introduction to the Device Tree", Linux Plumbers
2014. <https://docs.kernel.org/devicetree/>.

## 9. ACPI (x86_64)

Spec: ACPI 6.5 (2024), UEFI Forum. <https://uefi.org/specifications>.

Table hierarchy:
- **RSDP** (16-byte signature `"RSD PTR "`): in low BIOS area
  (legacy) or handed over by UEFI. Points to XSDT (rev 2+) or RSDT
  (rev 1). Limine's `limine_rsdp_request` surfaces this.
- **XSDT**: array of 64-bit pointers to subordinate tables.
- **FADT**: SCI interrupt, PM1a/1b event blocks, reset register,
  FACS + DSDT pointers.
- **MADT** (aka APIC): LAPICs (one per logical CPU), IOAPICs,
  Interrupt Source Overrides, NMI sources. **CPU count + LAPIC IDs
  live here** — prerequisite to INIT-SIPI-SIPI.
- **MCFG**: PCIe ECAM base addresses per segment.
- **HPET**: High-Precision Event Timer base.
- **DSDT / SSDT**: AML bytecode describing thermal zones, power
  states, EC, `_CRS` for every ISA device. Requires an AML
  interpreter.

Parsing options:
- **ACPICA** (Intel, Apache 2.0, ~100k LOC): reference AML
  interpreter, used by Linux/FreeBSD/Haiku/Illumos. Overkill for ALZE
  now; unavoidable once S3/S4 sleep, thermal, laptop EC are needed.
  <https://acpica.org/>.
- **uACPI** (Tatianin, 2023+, MIT, ~30k LOC): clean-room rewrite
  designed for hobby OSes. <https://github.com/uACPI/uACPI>. Modern
  default.
- **Hand-rolled static-table parsing**: RSDP → XSDT → MADT gives
  CPU count + LAPIC IDs + IOAPIC base. MCFG gives PCIe. FADT gives
  reset register. **No AML needed for this.** ALZE can stay at this
  level for a long time.

Recommended ALZE staging: add `limine_rsdp_request` → walk XSDT →
capture MADT/MCFG/FADT/HPET → parse MADT for AP IDs (feeds §10) →
parse MCFG for PCIe ECAM → defer DSDT/SSDT/AML to uACPI when sleep /
thermal / laptop EC actually matter.

## 10. SMP AP boot

Classic sequence (Intel SDM Vol 3A §9.4):
1. BSP discovers all LAPIC IDs from MADT.
2. Allocate a page of low memory (< 1 MiB, page-aligned) — the SIPI
   vector is an 8-bit page number.
3. Copy AP trampoline into that page. Trampoline starts 16-bit real
   mode → loads GDT → protected mode → enables PAE + paging + EFER.LME
   → long mode → jumps to C `ap_main()`.
4. BSP writes LAPIC ICR:
   - **INIT** IPI (delivery 0b101, level-assert). Wait ~10 ms (SDM's
     200 µs is stale; 10 ms is the robust modern wait).
   - **SIPI**, vector = `trampoline_page >> 12`. Wait ~200 µs.
   - **Second SIPI**, same vector. Legacy Pentium workaround still
     mandated by SDM.
5. AP increments a shared atomic; BSP waits for count.
6. BSP writes per-AP stack pointer somewhere trampoline reads (or AP
   indexes by LAPIC ID into a per-CPU array).

**x2APIC vs xAPIC**:
- **xAPIC** (legacy): MMIO at 0xFEE00000, 8-bit IDs (≤255 CPUs).
- **x2APIC**: MSR 0x800-0x83F, 32-bit IDs. Enabled via IA32_APIC_BASE
  MSR bit 10. Required above 255 threads. "Cluster x2APIC" mode
  changes IPI mask interpretation.

**How bootloaders handle AP boot**:
- **Limine rev ≤3** (ALZE today): no. Kernel does INIT-SIPI-SIPI.
- **Limine rev 4 (draft)**: opt-in, loader wakes APs in long mode,
  hands over ready stack pointers.
- **GRUB / MB2, iPXE**: never.
- **Stivale2**: optional SMP tag.
- **Linux**: does it itself in arch code.

**Common traps**:
- Trampoline writing outside its own page → silent AP crash.
- Missing IF=0 in long-mode jump tail → AP takes interrupt on unset
  stack.
- Forgetting to set per-CPU GS_BASE on AP before `%gs:cpu_self`.
  R2 notes ALZE handles this for BSP at `main.c:221`; same ordering
  applies per-AP.
- SIPI before trampoline cache line is visible. Rare on x86
  (strongly-ordered) but happens.

**ALZE state**: LAPIC initialized, TLB-shootdown IPI wired
(`main.c:273-274`), no APs launched. Console is unlocked (R2 issue 5):
that is exactly the SMP gate — the moment AP0 prints, state corrupts.

Refs: Intel SDM Vol 3A ch 9. OSDev Wiki, "SMP"
<https://wiki.osdev.org/Symmetric_Multiprocessing>.

## 11. Network boot

Chain: PXE ROM in NIC firmware → DHCP (options 66/67 = TFTP server +
boot filename) → TFTP pulls bootstrap → bootstrap (pxelinux or iPXE)
pulls kernel + initrd.

- **PXE** (Intel, 1998): 16-bit real-mode, TFTP only. Dead on UEFI
  except as compat mode.
- **UEFI PXE**: `EFI_PXE_BASE_CODE_PROTOCOL`. Same DHCP + TFTP flow
  callable from UEFI apps.
- **UEFI HTTP Boot** (UEFI 2.5+): DHCP option 67 = URL, firmware pulls
  HTTP(S). Modern data-center bare-metal provisioning: no TFTP, signed
  images, proxy-friendly.
- **iPXE** (<https://ipxe.org>, GPLv2, ~100k LOC): script-driven
  replacement. Supports HTTP/HTTPS, iSCSI, FCoE, AoE. Runs as PXE
  payload or UEFI app. Two-line iPXE script = netbooted kernel in
  QEMU.

**Relevance to ALZE**: none near-term. Long-term, netbooting for CI
is cheaper than ISO-rebuild per iteration. Wait until the kernel is
bigger. Ref: iPXE docs <https://ipxe.org/docs>; UEFI 2.10 §24.

## 12. Bootloader → kernel handoff (what limine gives ALZE)

Concrete register-and-state contract when `_start` is entered on
ALZE today:

| Element    | Value at `_start`                                    |
|------------|------------------------------------------------------|
| CPL        | 0                                                    |
| CS         | 0x28 (loader kernel code, 64-bit)                    |
| DS/ES/SS   | 0x30 (loader kernel data)                            |
| FS/GS base | loader-defined (ALZE overwrites both via GDT + MSR)  |
| RIP        | `&_start` (`0xffffffff80001000` per `readelf -lW`)   |
| RSP        | 16-byte aligned, inside 16 KiB loader stack          |
| RFLAGS.IF  | 0                                                    |
| CR0        | PE=1, PG=1, WP=1, EM=0, MP=1, NE=1                   |
| CR3        | loader's PML4 (HHDM + kernel mapped)                 |
| CR4        | PAE=1, PSE, PGE, SMEP if CPU supports                |
| EFER       | LME=1, LMA=1; **NX = undefined, kernel must set**    |
| GDT        | loader's 3-entry GDT                                 |
| IDT        | loader's halt-stub IDT                               |
| TSS        | none / loader's                                      |
| FPU/SSE    | disabled (limine does not touch CR4.OSFXSR)          |
| Arguments  | none in regs; kernel reads `*_request.response`      |

**ALZE's three phases** (summarized from R2; full trace in
`review/boot_build.md`):

- `init_early_hw`: uart → ssp → cpuid → revision-check → gdt → idt →
  percpu (GS_BASE) → pic → pit → `sti`. Silent-fault windows: panic
  before `uart_init`, and the non-present IDT gates (R2 issues 2-4).
- `init_memory`: reads HHDM offset, walks memmap, PMM bitmap, VMM
  builds own PML4 (W^X, guard-page stacks), switches CR3, enables NX
  (missing CPUID guard — R2 issue 15), `vmm_audit_wx()`.
- `init_devices`: framebuffer console, kb, VFS skeleton, LAPIC +
  TLB-shootdown IPI (SMP scaffold, no APs launched), ramdisk, PCI,
  xHCI.

**Mental model**: limine gives ALZE a CPU in long mode with paging on,
a stack, and pointers to memmap / HHDM / framebuffer / RSDP (if
requested). Everything else — GDT, IDT, TSS, NX, FPU policy, LAPIC,
ACPI, SMP, timers — is the kernel's job.

## Boot-loader comparison table

| Loader         | License   | LOC (core) | MB1 | MB2 | Stivale2 | Limine proto | UEFI | BIOS | SMP boot | Config    | Best for                          |
|----------------|-----------|------------|-----|-----|----------|--------------|------|------|----------|-----------|-----------------------------------|
| Limine         | BSD-2     | ~30k       | yes | yes | legacy   | yes          | yes  | yes  | rev4     | text conf | hobby x86_64 OSes (ALZE)          |
| GRUB 2         | GPLv3     | ~55k       | yes | yes | no       | no           | yes  | yes  | no       | Bourne    | Linux distros, multi-fs disks     |
| systemd-boot   | LGPL-2.1  | ~8k        | no  | no  | no       | no           | yes  | no   | no       | .conf     | UEFI Linux with UKIs              |
| rEFInd         | GPLv3     | ~12k       | no  | no  | no       | no           | yes  | no   | no       | text conf | macOS/Win/Linux graphical triple  |
| iPXE           | GPLv2     | ~100k      | via MB | via MB | no | no           | yes  | yes  | no       | script    | network-booted bare metal         |

Notes: "LOC (core)" excludes bundled filesystem / network / theme
drivers for GRUB (would be +500k), which is the honest reason GRUB
is massive. "SMP boot" here means "does the loader itself wake APs";
every loader can load a kernel that wakes APs.

## ALZE applicability

**v1 — validate current limine integration (do now)**:
- Protocol rev 3 with HHDM + memmap + framebuffer requests
  (`main.c:59`) is correct for 2026 hobby x86_64. Keep.
- R2 issue 12: vendor limine binaries (`limine-bios.sys`,
  `limine-bios-cd.bin`, `limine-uefi-cd.bin`) or add pinned
  `limine:` sub-target. Rev 3 semantics depend on a specific loader
  version — pinning is essential.
- R2 issue 10: emit build-id (`-Wl,--build-id=sha1` +
  `KEEP(*(.note.gnu.build-id))`) and print in boot banner for
  crash-log ↔ binary matching.
- Add `limine_rsdp_request` (≈10 LOC) — unlocks ACPI/MADT parsing,
  prerequisite for AP boot.
- Add `limine_kernel_address_request` for explicit phys/virt base
  (currently inferred from linker symbols).
- Before EFER.NX write: `KASSERT(cpuid_has_nx())` (R2 issue 15).
- Before `sti`: IDT gates for vectors 1-31, 0x27, 0x2F, 0xFF (R2
  issues 2-4). Non-present gate = triple fault = silent reboot.
  Single highest-leverage follow-up.

**v2 — optional multi-bootloader fallback** (only if concrete
reason): GRUB2 support via Multiboot2 header, ~200 LOC trampoline
(32-bit → 64-bit mode switch + populate limine-equivalent views from
MB2 tags). Not worth it while ALZE is dev-test only.

**v3 — Arm / RISC-V port (long horizon)**: DTB parsing mandatory
on most SoCs, ACPI optional. Plan libfdt-equivalent (~2k LOC), keep
ACPI for SystemReady-SR servers. Years out; document not architect.

## Honest close

Limine is the right choice for ALZE: actively maintained, tight
handoff semantics, small LOC, clean revision-acceptance check via
`LIMINE_BASE_REVISION()`. Don't change bootloaders — no concrete
reason.

The pitfall is treating the loader as magic. Limine gives you long
mode, paging, a stack, and four or five response pointers. The kernel
still owes: own GDT + TSS; full IDT (missing vectors = silent triple
fault — R2 issues 2-4); EFER.NX with CPUID guard (R2 issue 15); ACPI
parsing (RSDP → XSDT → MADT); SMP AP boot via INIT-SIPI-SIPI; LAPIC +
IOAPIC (LAPIC done, IOAPIC pending); FPU/SSE policy (ALZE: disabled,
deferred to userspace); syscall MSRs when userspace arrives; console
locking the moment a second CPU prints (R2 issue 5).

R2 caught every concrete gap. This R5 doc is the *why*: all 15 gaps
are "we forgot", none are "bootloader doesn't do that".

## References (primary)

- mintsuki, *Limine Boot Protocol v8.x*, 2025-2026.
  <https://github.com/limine-bootloader/limine/blob/v8.x/PROTOCOL.md>.
- mintsuki, *Stivale 2 Protocol* (deprecated 2023).
  <https://github.com/stivale/stivale/blob/master/STIVALE2.md>.
- GNU GRUB Project, *Multiboot2 Specification v2.0*, 2010.
  <https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html>.
- UEFI Forum, *UEFI Specification v2.10*, 2024;
  *ACPI Specification v6.5*, 2024. <https://uefi.org/specifications>.
- Intel, *SDM Vol 3A*, 2025 — ch 4 (paging), 9 (MP management),
  10 (APIC).
- Zimmer / Rothman / Marisetty, *Beyond BIOS: Developing with UEFI*,
  3rd ed., Intel Press, 2017.
- DeviceTree Org, *DeviceTree Specification v0.4*, 2022.
  <https://www.devicetree.org/specifications/>.
- Poettering, *systemd-boot(7)*; *The Strange State of Authenticated
  Boot...*, 2021.
  <http://0pointer.net/blog/authenticated-boot-and-disk-encryption-on-linux.html>.
- Smith, Rod, *rEFInd Documentation*. <https://www.rodsbooks.com/refind/>.
- Kiper, *GRUB2: Present and Future*, OSSummit EU 2022.
  <https://lwn.net/Articles/899286/>.
- iPXE Project docs, <https://ipxe.org/docs>.
- Tatianin, Daniil, *uACPI*, 2023-2026.
  <https://github.com/uACPI/uACPI>. ACPICA, <https://acpica.org/>.
- OSDev Wiki: *Limine*, *Multiboot*, *BIOS*, *Real Mode*, *SMP*,
  *APIC*. <https://wiki.osdev.org/>.
- **R2 internal**: `review/boot_build.md` (2026-04-21) — 15 concrete
  issues in ALZE's current integration; this R5 doc explains which
  are "bootloader doesn't do that" vs "we forgot". All 15 are the
  second kind.
