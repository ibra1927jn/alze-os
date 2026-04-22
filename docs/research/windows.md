# Windows NT kernel family (NT 3.1 → Windows 11 / Server 2025)

Research notes for ALZE OS. Scope: what Cutler's team got engineering-right, what
37 years of backward compat did to it, and which ideas are worth stealing.

## Overview

Windows NT started as a clean-room OS in **November 1988**, when Dave Cutler
(ex-DEC, architect of VMS, RSX-11 and the cancelled MICA project) joined
Microsoft with a team of DEC West engineers. The initial brief was a portable,
multiprocessor OS presenting **multiple personalities** simultaneously — the
"environment subsystems" idea. NT 3.1 shipped in **July 1993** with three in
user mode: **Win32** (the future winner), **OS/2 1.x** text-mode (leftover from
the MS/IBM split), and **POSIX.1** (minimum spec for US federal procurement).
Later personalities came and went: Interix / SFU (2000s), then **WSL1** (2016,
syscall translation in a pico-process) and **WSL2** (2019, full Linux kernel
inside a lightweight Hyper-V VM).

Version arc: **NT 3.x → 4.0 (1996)** promoted the Win32 GUI (GDI/USER) from
CSRSS user-mode to kernel (`win32k.sys`) for performance. **NT 5.x = 2000/XP**
unified consumer and NT lines, added Active Directory, matured WDM.
**NT 6.x = Vista/7/8** brought UAC, WDDM, ALPC replacing LPC, PatchGuard,
the MinWin refactor. **NT 10.0 = Windows 10/11/Server 2016–2025** made
VBS/HVCI default, Hyper-V a security boundary *inside* the OS, and shipped
MSIX, DirectStorage, WSL2, ARM64 parity. The kernel version string has been
frozen at 10.0 since 2015 — a marketing decision, not a technical one.

## Arquitectura

Hybrid design, often mis-labeled "microkernel". In practice most of the
interesting code lives in one kernel-mode binary, `ntoskrnl.exe`, which contains
both the **Kernel** (KE: thread dispatcher, ISR/DPC, spinlocks, trap handling)
and the **Executive** (I/O, Object Manager, Memory Manager, Cache Manager,
Configuration Manager / Registry, Security Reference Monitor, ALPC, Process
Manager). Below it sits **HAL** (`hal.dll`), which abstracts interrupt
controllers, timers, SMP bus details.

Drivers load into the same address space (KM). User mode holds **NTDLL** (the
syscall stub library and native NT API), the **environment subsystems** with
their runtime servers (`csrss.exe` for Win32), the **Win32 client DLLs**
(`kernel32`, `user32`, `gdi32`, `advapi32`), and — confusingly — **`win32k.sys`**
in kernel mode since NT 4.0, the largest single source of Windows CVEs for a
decade. Cross-cutting primitives: the **Object Manager** provides a typed
hierarchical namespace (`\Device\`, `\??\`, `\BaseNamedObjects\`,
`\KnownDlls\`...) behind every handle; the **Registry** is the global config DB
via Configuration Manager, persisted as hive files under
`%SystemRoot%\System32\Config\`; **ALPC/RPC** carry fast local messages (see IPC).

## En qué es bueno

- **Driver model stability + versioning.** A WDM / WDF driver written for
  Windows 7 will frequently still load on Windows 11. WDDM graphics drivers
  version across generations. Few other OS vendors keep this much binary
  contract with third parties.
- **IOCP.** I/O Completion Ports shipped in NT 3.5 (1994) — years before
  `epoll` (2002) or `kqueue` (2000). The model (kernel-managed thread pool,
  FIFO completion queue, LIFO thread wake, concurrency value) is still the
  reference design for scalable async I/O.
- **WinDbg + PDB symbols.** First-class kernel debugger over serial/1394/USB/net,
  full public symbol server. Live-kernel and post-mortem dump analysis on the
  same tool. Open-source OSes still do not match this DX.
- **ETW.** Structured, schema'd, low-overhead kernel tracing with in-kernel
  providers. DTrace-level power with minimal perf cost; manifest-based
  providers allow offline decoding.
- **NTFS journaling + USN journal.** Metadata consistency via `$LogFile`,
  plus a separate change journal (`$UsnJrnl`) that apps can tail.
- **ACL-based security.** SIDs + DACL/SACL + privileges + integrity levels is
  strictly more expressive than Unix mode bits; ACEs cascade via inheritance.
- **Hyper-V as a security boundary.** VBS / HVCI / Credential Guard run the
  kernel code integrity and LSA secrets inside a separate VTL, so a
  ring-0 exploit does not automatically win the box.

## En qué falla

- **Registry.** Global, binary, mutable hive files. A single write-heavy
  component (Windows Update's `COMPONENTS` hive is the famous example) can
  corrupt and wedge updates for the life of the install. There is no
  transactional story for cross-hive operations.
- **Win32 API accretion.** 40+ years of compat. `CreateFileW` has six legacy
  modes, `GetMessage` is still the Win16-era pump, thread-local storage has
  two incompatible implementations. Attack surface is enormous.
- **Drive letters.** A:–Z: namespace grafted over the Object Manager. Mount
  points and symlinks help, but no clean VFS tree. Removable media vs
  network vs virtual disks still feel like different worlds.
- **MAX_PATH.** `MAX_PATH = 260` baked into thousands of binaries. Long paths
  require the `\\?\` prefix, a manifest opt-in, a Group Policy flip **and**
  code that actually handles them. Explorer only partially does.
- **Metadata-op perf.** NTFS is ~10x slower than ext4/XFS for `git status`
  style traversal. Defender filter-driver stack makes it worse in practice.
- **Per-process file locks by default.** Opening a file without `FILE_SHARE_*`
  locks out every other reader, so `tail -f` style tooling is painful.
- **Update topology.** Windows Update + WSUS + SCCM + Intune + MDM + Autopatch
  is a patchwork. Enterprise update UX is still worse than `apt upgrade`.

## Cómo funciona por dentro

### Scheduling

Preemptive, priority-driven, per-CPU ready queues. **32 priority levels** split:

- **0**  — zero page thread only
- **1–15** — dynamic / variable range (most user code sits at ~8)
- **16–31** — real-time range; no dynamic boosts applied

Quanta are short (~10–15 ms on client, ~120 ms on server, tuned via
`Win32PrioritySeparation`). The scheduler applies **priority boosts** on I/O
completion, foreground activation, GUI input, and wait-release of scarce
resources; each boost decays by one per quantum until the thread falls back to
base. Affinity masks, CPU sets (Win10+) and **Processor Groups** extend to
>64 CPUs. **Fibers** (user-mode cooperative) exist but are legacy; modern
code uses thread pools + IOCP.

### Memory

Virtual memory via page tables + **PFN database** (one `MMPFN` per physical
page frame). Pages move between lists: **Active** (in some working set),
**Modified** (dirty, pending write-back), **Standby** (clean, cached,
reclaimable with a soft fault), **Free** (stale), **Zero** (zeroed, satisfies
demand-zero faults; filled by `MmZeroPageThread` at priority 0). The
**balance set manager** trims working sets under pressure. Files map in via
**section objects** (Windows' `mmap` equivalent) + the Cache Manager — the
filesystem cache *is* mapped sections, so `ReadFile` and `MapViewOfFile`
share pages.

### IPC

- **ALPC** (Asynchronous LPC, Vista+, replaces original NT LPC). Used by
  CSRSS, LSASS, SCM, RPC local transport (`ncalrpc`). Small messages copy
  through the port queue; large ones pass a shared section pointer.
- **Named pipes** — bidirectional, `\\.\pipe\name`, optionally message-mode,
  integrate with IOCP.
- **RPC** — `ncalrpc` on ALPC, `ncacn_np` on pipes, `ncacn_ip_tcp` on TCP.
  MIDL IDL with auth (Kerberos/NTLM/Negotiate). Most of COM and WMI ride on it.
- **Shared sections + events/mutexes/semaphores** — low-level synchronization
  (`KEVENT`, `KMUTANT`, `KSEMAPHORE`), HANDLE-wrapped in user mode.
- **Mailslots** — legacy broadcast IPC, nobody should use these.

### Syscalls

Evolution: `int 2e` (NT 3.1) → `sysenter`/`syscall` (since XP / x64). Client
path: app → `kernel32!CreateFileW` → `ntdll!NtCreateFile` syscall stub.

The stub loads the **syscall number** into `EAX`/`RAX`, arguments into
registers/stack, and executes the trap instruction. The number + calling
convention is *not* a stable ABI — Microsoft renumbers every Windows release
to deter direct syscalls.

**`KUSER_SHARED_DATA`** at `0x7FFE0000` (user) / `0xFFFFF78000000000` (kernel)
is a read-only page shared with every process, holding system time, tick
count, OS version, processor features, and — historically — a pointer to
the preferred syscall stub (`SystemCall` field), letting the OS pick
`int 2e` vs `sysenter` vs `syscall` without recompiling NTDLL. Modern
builds hardcode `syscall` on x64.

### Filesystem

**NTFS** (since NT 3.1). Everything is a file, including metadata. **`$MFT`**
is the Master File Table — every file has ≥1 record (1 KB) holding typed
attributes: `$STANDARD_INFORMATION`, `$FILE_NAME`, `$DATA`, `$INDEX_ROOT`,
`$SECURITY_DESCRIPTOR`. Small files store data inline ("resident").
**`$LogFile`** is the write-ahead log for metadata, replayed at mount (NTFS
journals *metadata*, not file data). **`$UsnJrnl`** is a tailable change-
journal for indexers and backup. **Alternate Data Streams** (multiple `$DATA`
attributes) power `Zone.Identifier` (Mark-of-the-Web) and get abused by
malware. **Reparse points** — tagged redirects in an MFT record — underpin
symlinks, junctions, mount points, dedup, OneDrive placeholders, WSL metadata.
Transparent compression and EFS encryption are per-file via special `$DATA`
handling.

**ReFS** (Server 2012+). Designed for data integrity on commodity storage.
Copy-on-write B+ trees, always-on metadata checksums, optional **integrity
streams** (CRC-32C over data) that pair with Storage Spaces mirror/parity to
auto-heal bitrot. Background scrubber runs every 4 weeks. Trade-offs: no
`$LogFile`-style metadata journal (CoW instead), no hard links in early
versions, no short names, no EFS.

## Qué podríamos copiar para ALZE OS

1. **IOCP-style async I/O with kernel-managed thread pools.** The handle-
   oriented completion queue, with LIFO thread wake and a tunable concurrency
   value, is the best-tested design for high-fanout async on SMP. Don't
   reinvent `io_uring` without reading `CreateIoCompletionPort` first.
2. **Object Manager typed namespace as the primary OS surface.** Unix's
   "everything is a file" is too loose — sockets, semaphores, threads, GPU
   queues all get crammed into a path + `ioctl`. NT's "everything is a typed
   object in a hierarchical namespace" is a better fit for modern security:
   each object has a type with its own access mask and methods, and ACLs
   attach uniformly.
3. **ALPC as the local RPC transport.** Adaptive message-copy vs. shared-
   section, built-in caller identity (SID/integrity level), port-based
   capabilities. Make it the only IPC primitive the OS blesses.
4. **ETW-style structured tracing baked into the kernel.** Manifest-defined
   schemas, lock-free per-CPU buffers, zero-cost when no consumer is
   listening. `printk` + `dtrace` + `perf` unified at boot.
5. **ACL-based security with SIDs, DACL, SACL and integrity levels.** Finer
   than rwx, auditable via SACL, supports inheritance. Worth copying even
   if the policy language is painful — the data model is right.
6. **MSIX-style app container format.** Signed zip + manifest + per-app
   virtualized file/registry namespace + capability declarations + clean
   uninstall guarantee. Equivalent of Flatpak/Snap but with a cleaner
   per-user layered view. Make this the *only* install format.
7. **VBS/HVCI as default.** Use the hypervisor as a security boundary inside
   the OS from day one: code-integrity and credential/secret stores in a
   higher VTL, so ring-0 compromise is not terminal.

Do **not** copy: the Registry as a single global mutable hive, drive letters,
kernel-mode GDI, or any API that takes a `LPOVERLAPPED`.

## Fuentes consultadas

- https://en.wikipedia.org/wiki/Architecture_of_Windows_NT
- https://en.wikipedia.org/wiki/Windows_NT
- https://en.wikipedia.org/wiki/Windows_NT_OS_kernel_executable
- https://en.wikipedia.org/wiki/Dave_Cutler
- https://www.itprotoday.com/compute-engines/windows-nt-and-vms-rest-story
- https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/windows-kernel-mode-object-manager
- https://en.wikipedia.org/wiki/Object_Manager
- https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities
- https://learn.microsoft.com/en-us/windows/win32/procthread/priority-boosts
- https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports
- https://en.wikipedia.org/wiki/Input/output_completion_port
- https://en.wikipedia.org/wiki/Local_Inter-Process_Communication
- https://csandker.io/2022/05/24/Offensive-Windows-IPC-3-ALPC.html
- https://en.wikipedia.org/wiki/NTFS
- https://learn.microsoft.com/en-us/windows-server/storage/refs/refs-overview
- https://learn.microsoft.com/en-us/windows-server/storage/refs/integrity-streams
- https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation
- https://learn.microsoft.com/en-us/archive/technet-wiki/15259.page-frame-number-pfn-database
- https://rayanfam.com/topics/inside-windows-page-frame-number-part1/
- https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/event-tracing-for-windows--etw-
- https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/device-guard-and-credential-guard
- https://learn.microsoft.com/en-us/windows/security/identity-protection/credential-guard/
- https://learn.microsoft.com/en-us/windows/wsl/compare-versions
- https://en.wikipedia.org/wiki/Windows_Subsystem_for_Linux
- https://learn.microsoft.com/en-us/windows/msix/overview
- https://learn.microsoft.com/en-us/windows/msix/msix-container
- https://github.com/microsoft/DirectStorage
- https://devblogs.microsoft.com/directx/directstorage-1-1-now-available/
- http://www.nynaeve.net/?p=48
- https://www.geoffchappell.com/studies/windows/km/cpu/sep.htm
- https://unit42.paloaltonetworks.com/win32k-analysis-part-1/
- https://en.wikipedia.org/wiki/Client/Server_Runtime_Server_Subsystem
