# macOS · Darwin · XNU

Technical notes on macOS as shipping product, Darwin as the open-source core, and XNU as the hybrid kernel (Mach microkernel + BSD personality + IOKit driver model). Compiled as reference for ALZE OS design decisions.

## Overview

macOS descends from **NeXTSTEP** (1989), itself built on CMU's Mach microkernel + a 4.3BSD userland. Apple acquired NeXT in 1996, shipped Rhapsody (1997), Mac OS X Server 1.0 (1999), and Mac OS X 10.0 "Cheetah" in 2001. In 2000 Apple open-sourced the core as **Darwin** under the Apple Public Source License — Steve Jobs picked the name for the evolution metaphor. Darwin = XNU kernel + BSD userland + core system libraries; everything above (Cocoa, Quartz, Metal, AppKit) stays proprietary.

Timeline highlights: Tiger (10.4) shipped launchd + Spotlight; Leopard (10.5) shipped Seatbelt sandbox + full Unix03 conformance; Snow Leopard (10.6) shipped GCD + OpenCL + Blocks; Mavericks (10.9) added compressed memory; High Sierra (10.13) shipped APFS; Catalina (10.15) deprecated kexts; Big Sur (11) shipped Apple Silicon (ARM64).

Today macOS powers Apple's desktops/laptops (same XNU ships on iOS/iPadOS/tvOS/watchOS/visionOS). Primary uses: creative pro (Final Cut, Logic, Adobe), software dev (the preferred Unix for many devs thanks to Homebrew + native BSD), enterprise mobility, AI/ML research on Apple Silicon (unified memory + Metal). Darwin itself still ships as a bootable OS but community attention has waned since public releases largely stopped at 10.15/11.

## Arquitectura

**XNU = Mach + BSD + IOKit in one address space.** Apple rejected pure microkernel performance penalty by collapsing everything into the kernel image, keeping Mach's abstractions (tasks, threads, ports, VM) while avoiding IPC across a trust boundary for every syscall.

- **Mach core** (`osfmk/`): derived from OSF MK 7.3. Owns `task_t` (container of resources), `thread_t` (schedulable unit), `ipc_port` (kernel-backed message queue), `vm_map` (VM address space). Messages flow via `mach_msg()` trap; ports are capabilities.
- **BSD layer** (`bsd/`): FreeBSD-synced. Provides POSIX syscalls, `proc` (wraps `task`), `vnode` (VFS node), networking stack, signals, pipes, sockets, kqueue, POSIX shm/sem, UIDs/GIDs, TrustedBSD MAC framework.
- **IOKit** (`iokit/`): C++ (Embedded C++ subset) driver framework. `IOService` base class, I/O Registry tree, matching-by-personality. Drivers run in kernel historically; now pushed to userland via DriverKit.
- **libkern/Platform Expert**: C++ runtime support for IOKit + arch-specific boot code.

Userland (Darwin):
- **launchd** (PID 1): replaces init/inetd/cron/watchdogd. Reads plist jobs from `/System/Library/LaunchDaemons` and `~/Library/LaunchAgents`. Supports on-demand socket/port activation, KeepAlive, throttling. Talks to `launchctl` over a Mach-specific IPC.
- **dyld** (dynamic linker): maps the **dyld shared cache** — a single prelinked blob at `/System/Library/dyld/` containing every system library with Objective-C runtime pre-initialized, mapped into every process's address space at a fixed base via a dedicated syscall. Slashes startup time and RSS.
- **Sandbox** (originally Seatbelt): policy module for TrustedBSD MAC framework. Scheme-language profiles in `/System/Library/Sandbox/Profiles/*.sb`. `Sandbox.kext` hooks syscalls; `sandboxd` user daemon bound to special Mach port 14 (`HOST_SEATBELT_PORT`).
- **Endpoint Security framework** (ES): ~100 event types (NOTIFY / AUTH), surfaced via `EndpointSecurity.kext`. Lets vendor EDRs ingest exec, open, mount, etc., entirely from userspace — Apple's answer to "stop writing kexts".
- **XPC**: typed IPC over Mach ports, managed by launchd. Anonymous services per-app, Mach service names (reverse DNS).

## En qué es bueno

- **Integrated HW/SW**: Apple Silicon + XNU + metal stack delivers top-tier perf/watt; kernel knows about P/E cores + unified memory + AMX/ANE.
- **World-class dev tools**: **Instruments** (GUI over DTrace + kdebug + Signposts), **DTrace** (shipped since 10.5), **sample/spindump**, **Xcode + lldb**.
- **Darwin IPC model**: Mach ports + XPC give typed, capability-passing IPC that's actually adopted. You can pass file descriptors, rights, audit tokens through a single `mach_msg()`.
- **APFS**: CoW, snapshots, clones, space-sharing volumes, native encryption.
- **Stability**: protected memory since 10.0, mature VM, graceful handling of bad apps/drivers (especially with DriverKit).
- **Grand Central Dispatch**: first-class primitive, not a third-party lib. `dispatch_async` is the idiomatic way to do work; 15-instruction enqueue cost.
- **Sandbox profiles** + **code signing** + **notarization**: layered defense; every AppStore app runs under a non-trivial MAC profile.
- **POSIX + Mach together**: portability of code + power of Mach for platform features.

## En qué falla

- **Locked-down platform**: SIP (System Integrity Protection) blocks root from touching system files; notarization forces every binary through Apple's signing service; Gatekeeper blocks unsigned code by default. Great for grandma, painful for research OS work.
- **Kernel extension deprecation without full parity**: kexts are on life support since Catalina. DriverKit covers USB/HID/audio/network/PCI but has gaps (no full FS driver parity, no arbitrary kernel hooking). Security tools had to rewrite for ES framework.
- **IOKit C++ quirks**: Embedded C++ (no exceptions, no multiple inheritance, no templates in classical form, no RTTI beyond OSMetaClass), learning curve, verbose matching plists.
- **OSS Darwin neglected**: tarball cadence slowed dramatically post-10.15; PureDarwin never achieved self-hosting. Darwin-as-an-OS effectively a byproduct, not a goal.
- **Rosetta 2 overhead**: AOT x86-64 → ARM64 translation is excellent but not free; some workloads lose 20-40%; no 32-bit x86 support.
- **Vendor lock-in**: APFS format semi-documented, closed bootloader chain, Secure Enclave attestation gates features. Hackintosh is effectively dead post-Apple Silicon.
- **Hybrid kernel ≠ microkernel safety**: a bad driver still panics the box (pre-DriverKit). The Mach layer's isolation benefits are architectural, not enforced.

## Cómo funciona por dentro

### Scheduling

Mach threads are the unit of scheduling; `task_t` is a resource container (address space, port namespace, IOKit handles). XNU uses **128 priority levels** (0-127) split across bands (normal, highpri, kernel, realtime). Since 2020 the **Clutch scheduler** (and **Edge scheduler** on Apple Silicon) replaces the classic timesharing model: threads are grouped into **clutch buckets** keyed by `(task, QoS class)`, and an Earliest-Deadline-First algorithm picks root buckets. Goal: low latency for high-QoS work + starvation avoidance for background buckets.

**QoS classes** (userspace API, propagated via Mach messages/pthread attrs): `USER_INTERACTIVE`, `USER_INITIATED`, `DEFAULT`, `UTILITY`, `BACKGROUND`. QoS drives CPU affinity (P vs E core), I/O throttle, timer coalescing, cache QoS tagging in hardware, Low Power Mode behavior. Background work literally pauses in iOS LPM.

**Grand Central Dispatch** sits atop this: `dispatch_queue_t` is user-space work container; libdispatch manages a pool of worker threads obtained from the kernel via `workq_open`/`workq_kernreturn` (syscalls 367/368). Each queue targets a root queue at some QoS; submitted blocks run on whatever worker the kernel hands out. **thread_call** is the in-kernel equivalent (delayed callouts on kernel threads).

### Memory

Mach VM is the memory subsystem. Core concepts: **vm_map** (address space), **vm_object** (backing-store-less memory region), **pager** (source of pages: default/swap, vnode, device). Pages are 4 KiB (x86) / 16 KiB (ARM64). Physical RAM is modeled as a cache over virtual memory — every page has a `vm_page_t` and lives on a queue (active, inactive, speculative, free, compressed).

**Compressed memory** (10.9 Mavericks): instead of swapping inactive anonymous pages to disk, compress them (WKdm algorithm) into a dedicated compressor pool. Typical 2:1 ratio. Swap to `/private/var/vm/swapfile*` only happens when compressor is full. Massively reduces SSD wear and enables Apple's 8 GB RAM configs to feel like 16.

**Purgeable memory**: app marks regions as "discardable under pressure"; kernel reclaims without swap overhead. Used heavily by image caches (NSPurgeableData, Metal textures).

**Unified memory on Apple Silicon**: CPU and GPU share the same physical pages via IOMMU, no PCIe copy. Huge for ML workloads; the VM subsystem tags pages with GPU-visible attributes.

### IPC

**Mach port** = kernel-maintained message queue. Access gated by **port rights** held in a per-task name table:

- **Receive right**: exactly one per port (MPSC); owner dequeues with `mach_msg()`.
- **Send right**: multiple holders; allowed to enqueue.
- **Send-once right**: one-shot, auto-destroyed after use. Used for reply ports.
- **Port set**: kqueue-like aggregate, receive from many.
- **Dead name**: placeholder when peer dies.

Messages contain inline data + out-of-line data (copied via VM remap) + typed descriptors transferring port rights. A task **cannot fabricate rights** — you must create the port or be handed the right by someone else → capability system. `bootstrap_server` (launchd) mediates name lookup: services register `com.apple.whatever`, clients `bootstrap_look_up` to get a send right.

**XPC** layers typed IPC on top: dictionaries + typed values + connection lifecycle. Backed by a Mach port pair. Services declared in `Info.plist`, auto-spawned by launchd on first message. Rights to pass FDs, audit tokens, more ports.

**notify_server** / Darwin notifications: broadcast-style pub/sub over Mach ports, used by system to gossip state changes.

### Syscalls

XNU has **two parallel syscall planes**:

- **BSD syscalls** (positive numbers in `syscalls.master`): classic Unix — `open`, `read`, `execve`, `kevent`, `ptrace`. Trap via `svc`/`syscall` instruction.
- **Mach traps** (negative numbers): `mach_msg_trap`, `task_self_trap`, `thread_switch`, `semaphore_wait`. Small fixed set — most "Mach operations" are actually RPCs to the kernel's own Mach ports (e.g. `task_port` for task manipulation) via `mach_msg`.

The **commpage** is a read-only memory page mapped into every process at a fixed address containing kernel-populated data: timestamps, CPU feature bits, `gettimeofday` fast path, atomic primitives. Avoids syscalls for hot reads.

The **dyld shared cache** intercepts library loads: `dyld` (in userspace) recognizes a dylib is in cache and maps the pre-linked slice instead of the on-disk `.dylib`. All processes share the same physical pages.

**Sandbox / Seatbelt** intercepts via TrustedBSD MAC hooks on almost every syscall: `mpo_vnode_check_open`, `mpo_proc_check_signal`, hundreds more. Profiles compiled from Scheme DSL to bytecode, evaluated per-call against the process's profile.

### Filesystem

**APFS** (2017, High Sierra) replaced HFS+:

- **Copy-on-Write for metadata**: new records written, pointers updated, old records released. No journaling needed. Crash consistency by design.
- **Clones**: `clonefile(2)` → new inode, shared data blocks, delta extents for divergence. `cp -c` on CLI, Finder default.
- **Snapshots**: CoW point-in-time volume view. Used by Time Machine (local snapshots) and **boot snapshots** — the system volume is a sealed snapshot mounted read-only at boot. Updates apply to a new snapshot + atomic switch on reboot.
- **Space sharing containers**: one container, many volumes, shared free space. Replaces rigid partitioning.
- **Sparse files**: native sparse extents.
- **Native encryption**: three modes — none, single-key, per-file + separate metadata key. File-level keys wrapped by volume key wrapped by Secure Enclave on T2/Apple Silicon.
- **Optimized for SSD**: assumes random access, coalesces writes, TRIM-aware. Degrades badly on rotational media due to metadata fragmentation.

Limits vs ZFS/Btrfs: **no user-data checksums** (relies on SSD ECC), no snapshot-of-snapshot cloning, no send/receive, no RAID-Z equivalent. HFS+ is still supported read-only and for Time Machine backups on old disks.

## Qué podríamos copiar para ALZE OS

- **Mach ports + XPC as primary IPC**: typed, bidirectional, capability-passing (send rights = unforgeable references). Ship an ipc_port equivalent + a typed serialization layer in the core ABI; every service (FS, net, GUI) exposes XPC-style endpoints. Avoids the Linux-style `ioctl` + socket zoo.
- **Capability-based rights transfer**: only way to get a right is to create the port or be handed it. Eliminates ambient authority, makes sandbox straightforward.
- **IOKit matching/personality model**: declarative driver→device binding with scoring + probe. Let drivers live out-of-kernel (DriverKit-style) by default. Driver writes Info plist, kernel does three-phase matching (class → passive AND-of-properties → active probe with score).
- **APFS-style CoW + snapshots for atomic system updates**: sealed system volume + boot snapshot switch. Updates become "create snapshot, install into it, reboot, atomic pointer swap". Rollback is trivial. Clones via `clonefile` give cheap container images.
- **Grand Central Dispatch as primary concurrency primitive**: queues, not raw threads. User declares QoS intent; runtime + kernel cooperate on threadpool sizing, P/E affinity, I/O throttle. Kernel-assisted workqueue syscall (workq_open/kernreturn pattern) keeps enqueue cost ~15 instructions.
- **QoS propagation through IPC**: when you send a Mach message to a service, your QoS class travels with it so the server thread inherits priority. Avoids priority inversion by construction.
- **Compressed memory tier before swap**: RAM→compressor pool→disk. Huge win for SSD life and responsiveness.
- **Commpage**: kernel-exported read-only page for hot data (time, feature bits) avoids millions of syscalls.
- **dyld shared cache**: prelinked system library blob mapped into every process. Enormous startup + RSS win.
- **TrustedBSD MAC-style hook framework**: typed hooks at every security-relevant syscall so sandbox/EDR/policy modules compose cleanly without each writing their own kext.
- **Endpoint Security-style event stream**: one well-documented userspace API for security tools instead of N ad-hoc kernel hooks. Segregate NOTIFY and AUTH channels.

Things to explicitly *not* copy: IOKit's Embedded-C++ dialect (pick modern Rust/safe-subset); Seatbelt's Scheme profile language (too clever, hard to audit — prefer Cedar/Rego-style declarative policy); the kext → DriverKit transition pain (start with userspace drivers from day 1).

## Fuentes consultadas

- https://en.wikipedia.org/wiki/XNU
- https://en.wikipedia.org/wiki/Darwin_(operating_system)
- https://en.wikipedia.org/wiki/Apple_File_System
- https://en.wikipedia.org/wiki/Grand_Central_Dispatch
- https://en.wikipedia.org/wiki/Launchd
- https://github.com/apple-oss-distributions/xnu
- https://github.com/apple-oss-distributions/xnu/blob/main/doc/scheduler/sched_clutch_edge.md
- https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/Matching/Matching.html
- https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KernelProgramming/scheduler/scheduler.html
- https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KernelProgramming/vm/vm.html
- https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html
- https://developer.apple.com/system-extensions/
- https://docs.darlinghq.org/internals/macos-specifics/mach-ports.html
- https://newosxbook.com/articles/GCD.html
- https://tansanrao.com/blog/2025/04/xnu-kernel-and-darwin-evolution-and-architecture/
- https://karol-mazurek.medium.com/mach-ipc-security-on-macos-63ee350cb59b
- https://eclecticlight.co/2023/01/02/inside-apfs-from-containers-to-clones/
- https://www.ise.io/wp-content/uploads/2017/07/apple-sandbox.pdf
- https://www.chromium.org/developers/design-documents/sandbox/osx-sandboxing-design/
- https://research.meekolab.com/introduction-to-the-apple-endpoint-security-framework
- https://book.hacktricks.wiki/en/macos-hardening/macos-security-and-privilege-escalation/macos-proces-abuse/macos-ipc-inter-process-communication
- Levin, J. "\*OS Internals" (reference, 3 vols) — newosxbook.com
- Singh, A. "Mac OS X Internals: A Systems Approach" (reference)
- Siracusa, J. — Ars Technica Mac OS X reviews (10.0–10.10, classic)
