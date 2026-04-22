# Linux Mainstream Distributions — Ubuntu, Debian, Fedora, Arch

## Overview

Linux is a Unix-like, free and open-source monolithic kernel originally released by Linus Torvalds in 1991, now the core of most server, cloud, embedded, and Android deployments on Earth. The kernel itself is shared across every "Linux distribution"; what differentiates a distro is the userland it ships (libc, init, package manager, desktop, defaults) and its release cadence.

- **Debian** (1993) is the ancestral community distro: ~70,000 packages, strict free-software policy, conservative stable branch that freezes for ~2 years. Target: servers, purists, downstreams that want a solid base.
- **Ubuntu** (2004, Canonical) is Debian-derived, adds a predictable 6-month cadence plus an LTS every 2 years with 5–10 years of security support, proprietary-friendly defaults, Snap packaging. Target: developers wanting "Debian that just works", enterprise servers, cloud images.
- **Fedora** (2003, Red Hat-sponsored) is the upstream for RHEL: 6-month cadence, ~13-month support per release, aggressive adoption of new tech (Wayland by default, btrfs by default since F33, PipeWire, systemd everywhere). Target: developers who want modern stacks without Arch-level breakage.
- **Arch Linux** (2002) is rolling-release, minimalist, builds the system from a near-empty base. `pacman` + AUR give near-zero lag from upstream tarballs to installable packages. Target: power users, tinkerers, and anyone building a custom workstation.

## Arquitectura

The Linux kernel is **monolithic-with-modules**: scheduler, MM, VFS, network stack, and drivers all run in a single address space in ring 0, but can be extended via loadable kernel modules (`.ko`) inserted through `insmod`/`modprobe` without reboot. This trades the isolation of a microkernel for in-process calls between subsystems (no message-passing overhead).

Boot flow is nearly identical across all four distros:

1. **Firmware** (UEFI, rarely BIOS) runs POST, loads the EFI stub or GRUB2 from the ESP.
2. **Bootloader** (GRUB2 on Debian/Ubuntu/Fedora, `systemd-boot` or GRUB on Arch) loads `vmlinuz` and `initramfs.img` into RAM and jumps to the kernel.
3. **Kernel** self-decompresses, brings up MM/scheduler/IRQ, mounts the initramfs as a tmpfs rootfs, and execs `/init` (PID 1 in early userspace).
4. **initramfs** loads storage/crypto/network modules needed to find the real root (LVM, LUKS, iSCSI, NFS), pivots to the real root FS, and execs the real PID 1.
5. **systemd** (PID 1 on all four distros since ~2015) reads `.service`/`.target` units, parallelizes startup via socket activation, and brings the system to `multi-user.target` or `graphical.target`.

Package managers diverge sharply:

- Debian/Ubuntu: `dpkg` (low-level) + `apt` (high-level), `.deb` format, signed repos via `Release`/`InRelease`. Ubuntu adds Snap (confined, self-updating).
- Fedora: `rpm` + `dnf5` (libdnf5-based, written in C++), `.rpm` format, delta-RPMs, modularity. Flatpak is first-class for GUI apps.
- Arch: `pacman` (C, tar.zst archives), PKGBUILD scripts in the AUR built locally with `makepkg`.

## En qué es bueno

Linux's real strengths are mechanical, not marketing. **Hardware coverage** is unmatched: the in-tree driver model means a single `git pull` of the kernel brings support for thousands of devices compiled against the current ABI — there is no stable in-kernel module ABI on purpose, forcing drivers upstream where they get refactored for free. **Network stack** (TCP, XDP, eBPF hooks, multi-path TCP, QUIC offload via UDP GSO) is widely regarded as the fastest production stack in existence; XDP can drop 20+ Mpps on commodity hardware before packets hit the socket layer. **Containers and namespaces**: `mount`, `pid`, `net`, `user`, `uts`, `ipc`, `cgroup` namespaces plus cgroups v2 let Docker/Podman/Kubernetes carve the kernel into isolated worlds without a hypervisor. **Observability**: eBPF + tracepoints + perf let you attach programs to nearly every kernel event live, with kernel-side verification; `bpftrace` gives DTrace-quality one-liners. **Scheduler/IO flexibility**: since 6.12, `sched_ext` lets userspace BPF programs implement full custom schedulers with the kernel falling back to CFS/EEVDF if the BPF scheduler misbehaves for 30s. Meta reported 1.25–3% RPS and 3–6% p99 latency gains on their FB frontend from bespoke BPF schedulers.

Per distro: Debian's QA is brutal and the resulting stable branch is bug-for-bug reproducible across tens of thousands of packages. Ubuntu's LTS kernel livepatching (Canonical Livepatch) plus 10-year ESM is the reason it dominates cloud AMIs. Fedora's aggressive upstream-first policy means RHEL-bound bugs are filed years earlier. Arch's documentation (ArchWiki) is the de facto reference manual for the Linux userland — most searches for a kernel feature land on ArchWiki first.

## En qué falla

The weaknesses are equally concrete. **Fragmentation tax**: four package formats, two init systems (systemd vs. a long tail of OpenRC/runit/s6 holdouts), three printing stacks, two audio servers (PulseAudio legacy and PipeWire current), and at least GNOME/KDE/Xfce desktop stacks — upstream application developers pay this tax on every release. **Desktop audio history** was a decade of pain: ALSA (kernel, per-card mixing only), then PulseAudio (userspace mixer, brittle, famous for first-month problems), now PipeWire (2017–2021) which finally unifies PulseAudio/JACK/video capture on a single daemon. **Display stack**: X11/Xorg carried 35 years of cruft (indirect rendering, network transparency nobody uses, security model that lets any client keylog any other); Wayland has been "the future" since 2008 and only stabilized for daily use around 2022–2024, and even in 2026 there is no single "Wayland server" — each DE ships its own compositor, so bugs multiply. **Init system politics**: systemd's scope expansion (journald, resolved, networkd, timesyncd, homed, oomd) triggered community forks (Devuan) and keeps alienating sysadmins who want a PID 1 that only reaps zombies. **GUI UX**: HiDPI fractional scaling, color management, and gaming input latency were unsolved for years; NVIDIA proprietary drivers still tangle with Wayland compositors. **ABI instability inside the kernel**: a feature for refactoring, a curse for out-of-tree drivers (NVIDIA, ZFS). **Btrfs** is still the cautionary tale: ~15 years in-tree, still considered fragile for RAID5/6, and filesystem-at-scale tests have shown it failing where XFS completed.

## Cómo funciona por dentro

### Scheduling

The Completely Fair Scheduler (CFS), introduced in 2.6.23 (2007), kept per-task `vruntime` in a red-black tree and picked the leftmost node. After 15 years of accumulated heuristics ("accumulated hacks" in Peter Zijlstra's words), it was replaced in **6.6 (Nov 2023) by EEVDF** — Earliest Eligible Virtual Deadline First — a 1995 paper that assigns each task a virtual deadline based on weight and slice, selecting the task with the earliest eligible deadline. EEVDF keeps CFS's red-black tree structure but replaces the heuristic `sched_latency`/`min_granularity` knobs with a cleaner `slice` per-task attribute.

On top of this, **`sched_ext`** (merged 6.12) exposes a BPF-based pluggable scheduling class. A userspace BPF program implements `enqueue`/`dequeue`/`dispatch` callbacks; the BPF verifier guarantees termination and memory safety, and if the BPF scheduler fails to run a runnable task for ~30s the kernel reaps it and reverts to EEVDF. This turns scheduling into a hot-swappable userspace concern. Real schedulers already shipped: `scx_rusty` (Rust, topology-aware), `scx_lavd` (gaming latency), `scx_layered` (cgroup-aware).

### Memory

The MM subsystem is organized around the page allocator (buddy system), slab/slub for kernel objects, per-CPU free lists, and a page cache that unifies file I/O and anonymous memory. **Transparent Huge Pages (THP)** promotes 4 KB pages into 2 MB or 1 GB pages opportunistically for anonymous/tmpfs mappings to reduce TLB pressure, and will break them back down on swap — unlike `hugetlbfs` which is static. Recent work adds **multi-size THP (mTHP)** so the kernel can fold into intermediate sizes (16/32/64 KB on ARM) instead of the hard 4 KB → 2 MB cliff. **memcg** (memory cgroup) tracks per-cgroup RSS, pagecache, swap and kernel objects with v2 unified hierarchy; OOM-kill decisions can be delegated to `systemd-oomd` reading PSI (pressure stall information) counters. **zRAM** creates compressed RAM-backed block devices used as swap or `/tmp`; Fedora enables zRAM swap by default and ChromeOS has used it for years. **Folios** (5.16+) replaced many `struct page` references with `struct folio`, a typed head-of-compound-page, to make multi-page handling less error-prone.

### IPC

Linux offers a large IPC toolbox. **Pipes** and **FIFOs** (named pipes) are the Unix classic — half-duplex, byte-stream, backed by a kernel circular buffer. **UNIX domain sockets** (`AF_UNIX`) are full-duplex, support SCM_RIGHTS fd passing and SCM_CREDENTIALS peer credentials; they are the transport for Wayland, PipeWire, and most local daemons. **POSIX and SysV shared memory** (`shm_open`, `shmget`) map the same physical pages into multiple processes; synchronization is left to the programmer — typically via **futexes** (`futex(2)`), a fast-path uncontended lock implemented entirely in userspace with a kernel-side wait queue only on contention. The C library `pthread_mutex` is built on futex. **D-Bus** is a higher-level RPC/pubsub bus layered on Unix sockets, used for desktop session buses, systemd control, NetworkManager, Bluetooth. **kdbus** and **bus1** attempts to move D-Bus into the kernel were both rejected. **Netlink** sockets are the modern way userspace talks to kernel subsystems (routing, netfilter, audit).

### Syscalls

The syscall surface is huge (~450 on x86_64). Three modernizations stand out:

- **io_uring** (5.1, 2019): two shared ring buffers between userspace and kernel — the Submission Queue (SQEs) and Completion Queue (CQEs) — mapped via `mmap` so submission and completion require zero syscalls in steady state. The app fills an SQE, bumps the SQ tail; the kernel consumes it, fills a CQE, bumps the CQ tail. With `IORING_SETUP_SQPOLL` a kernel thread polls the SQ and the app never calls `io_uring_enter`. Fixed buffers and fixed files eliminate repeated pinning and fd lookups. It subsumes async read/write, accept/connect, send/recv, splice, openat, fsync, timeouts.
- **seccomp-bpf**: a classic-BPF filter installed per-thread that inspects syscall number + args and returns ALLOW/ERRNO/TRAP/KILL; used by Docker, Chrome, systemd sandboxing. Stateless by design. Research prototypes (Ramakrishna et al., 2023) propose eBPF-seccomp for stateful filters, but adoption is blocked on verifier complexity.
- **eBPF**: a small in-kernel VM with a verifier that proves termination and memory safety. Programs attach to kprobes, tracepoints, perf events, XDP, tc, cgroups, and LSM hooks. Used for observability (bpftrace, bcc), networking (Cilium), security (Falco, Tetragon), and now scheduling (sched_ext).

### Filesystem

- **ext4**: the safe default on Debian, Ubuntu, and Arch. Journaled metadata, extents, ~1 EB max, boring and fast. Best raw throughput for most workloads.
- **XFS**: Fedora server and RHEL default. B+ trees everywhere, designed for 1990s SGI supercomputers, excels at parallel writes, huge files, and maintains performance at petabyte scale. Metadata journaling only; no data checksums.
- **btrfs**: copy-on-write, snapshots, subvolumes, built-in RAID0/1/10, online scrub with checksums, compression (zstd/lzo/zlib), send/receive. Default on Fedora Workstation since F33 (2020) and openSUSE for longer. CoW cost shows up as higher write latency on databases, and parity RAID modes remain risky.
- **overlayfs**: a union filesystem that stacks a writable "upper" dir on a read-only "lower" dir, producing a merged view. This is the mechanism behind Docker/Podman layered images and live USB systems. Writes hit only `upper`; reads fall through.

## Qué podríamos copiar para ALZE OS

Ranked by implementation leverage, these are the mechanisms an ALZE OS architect should steal verbatim.

1. **io_uring's SQE/CQE dual ring design as the primary syscall ABI**, not as an afterthought. Every syscall should go through a `mmap`-ed pair of rings with 64-byte entries. This collapses syscall overhead to a `WRITE` + memory fence in the fast path, kills meltdown/spectre mitigation cost (no syscall → no `swapgs`), and makes batching the default. Keep `IORING_SETUP_SQPOLL`-equivalent kernel-thread polling for latency-critical workloads and fall back to doorbell syscalls otherwise.
2. **sched_ext's userspace-BPF pluggable scheduler pattern, generalized to all policy**. Expose enqueue/dispatch callbacks for CPU scheduling, but also for I/O priority, memory reclaim order, and network QoS. Require the safe-language verifier (Rust crate or Wasm) and enforce a deadman timer that reverts to a conservative default if the custom policy stalls.
3. **EEVDF's virtual-deadline model over CFS's vruntime**. Each runnable task carries a `(weight, slice)` tuple; the scheduler picks the earliest eligible deadline in an augmented red-black tree. This gives O(log n) selection, per-task latency targets ("I need to run within 2 ms"), and avoids the dozens of tunables CFS accumulated.
4. **Futex-style uncontended fast path for all synchronization primitives**. Userspace cmpxchg with zero syscalls in the common case, kernel wait queue only on contention. This should be the only primitive in the kernel; all higher-level locks (mutex, rwlock, condvar, semaphore) build on it. Copy `FUTEX_WAIT_BITSET` priority-inheritance and `FUTEX2` 64-bit wait words.
5. **cgroups v2 unified hierarchy as the accounting primitive, not an afterthought**. Every process belongs to exactly one cgroup, and cgroups nest arbitrarily; CPU, memory, I/O, PIDs, network, and custom resources all attach to the same tree. Combine with PSI (pressure stall info) exposed via a ring so userspace oom-killers like systemd-oomd work natively.
6. **eBPF-style in-kernel safe programmability** — a verifier-validated bytecode that can attach to tracepoints, scheduler decisions, LSM hooks, and network paths. This is the single biggest Linux advantage of the last decade: most new features (XDP, sched_ext, KRSI) are really new BPF attachment points, not new kernel code. ALZE should make the safe-ISA and verifier first-class from day one, not retrofitted.
7. **Folios over raw pages**. Kernel MM code that handles "a range of contiguous pages" should be typed as `folio`, not repeated `struct page` loops. Linux is retrofitting this at great pain; ALZE can start there and save a decade of bug churn.
8. **initramfs pivot-root pattern**: ship a minimal bootstrap FS in the boot image with just enough drivers and logic to find and mount the real root (over encrypted/networked/LVM storage), then swap roots and exec PID 1 again. Keeps the kernel image bootloader-friendly-small while supporting arbitrary root stacks.
9. **overlayfs as the primitive for package/container layering**, not as an extra driver. If every system directory is an overlay of (base image) + (package layer) + (user writable), you get transactional package installs, rollbacks, and container images for free.
10. **ArchWiki as a cultural artifact**: the distribution's user-facing docs should be the best reference implementation of the system. Invest in it from day one; it pays for itself in support load.

Things to explicitly **not** copy: the 450-syscall flat surface, the X11-era split of display and input, the out-of-tree driver cliff (enforce a stable module ABI), the PulseAudio → PipeWire decade of pain (pick one audio/video graph from the start), and systemd's expansionist scope (keep PID 1 small, offload services).

## Fuentes consultadas

- [Linux kernel — Wikipedia](https://en.wikipedia.org/wiki/Linux_kernel) — overall history, architecture overview.
- [Monolithic kernel — Wikipedia](https://en.wikipedia.org/wiki/Monolithic_kernel) — monolithic-with-modules design rationale.
- [CFS Scheduler — kernel.org docs](https://docs.kernel.org/scheduler/sched-design-CFS.html) — primary source on the pre-EEVDF scheduler.
- [sched_ext: a BPF-extensible scheduler class (Igalia blog)](https://blogs.igalia.com/changwoo/sched-ext-a-bpf-extensible-scheduler-class-part-1/) — mechanism and Meta's production gains.
- [Efficient IO with io_uring (Jens Axboe, kernel.dk PDF)](https://kernel.dk/io_uring.pdf) — original design paper.
- [io_uring(7) man page](https://www.man7.org/linux/man-pages/man7/io_uring.7.html) — SQE/CQE semantics.
- [Transparent Hugepage Support — kernel.org docs](https://docs.kernel.org/admin-guide/mm/transhuge.html) — THP and mTHP reference.
- [zram — kernel.org docs](https://docs.kernel.org/admin-guide/blockdev/zram.html) — compressed RAM block device.
- [Seccomp BPF — kernel.org docs](https://www.kernel.org/doc/html/v4.19/userspace-api/seccomp_filter.html) — syscall filtering primitive.
- [Programmable System Call Security with eBPF (arXiv 2302.10366)](https://arxiv.org/abs/2302.10366) — eBPF-seccomp proposal.
- [Arch boot process — ArchWiki](https://wiki.archlinux.org/title/Arch_boot_process) — bootloader → initramfs → systemd flow.
- [Booting process of Linux — Wikipedia](https://en.wikipedia.org/wiki/Booting_process_of_Linux) — cross-distro boot summary.
- [Arch compared to other distributions — ArchWiki](https://wiki.archlinux.org/title/Arch_compared_to_other_distributions) — primary source on distro-vs-distro philosophy.
- [pacman — ArchWiki](https://wiki.archlinux.org/title/Pacman) — rolling-release package manager.
- [PipeWire — Wikipedia](https://en.wikipedia.org/wiki/PipeWire) — audio/video framework history, PulseAudio/JACK unification.
- [Ubuntu vs Debian vs Fedora (Serverspace, 2026)](https://serverspace.io/about/blog/ubuntu-vs-debian-vs-fedora-which-linux-distro-to-choose-in-2026/) — release-cycle comparison.
- [Linux File System Comparison (linuxteck, 2026)](https://www.linuxteck.com/linux-file-system-comparison-ext4-xfs-btrfs/) — ext4 vs XFS vs btrfs production notes.
- [Inter-process communication in Linux (opensource.com)](https://opensource.com/article/19/4/interprocess-communication-linux-channels) — pipes/sockets/shm mechanisms.
