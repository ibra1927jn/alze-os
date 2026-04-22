# Redox OS — deep dive (R4)

Focus: the URL-scheme abstraction, Orbital, relibc, RedoxFS, userspace driver model, redoxpkg ecosystem, and honest 2026-04 maturity. Prior rounds (otros.md §Redox, r3/rust_in_kernel.md §2) covered Rust-in-kernel percentages and placed Redox alongside Asterinas/Theseus/Hubris. This doc does not repeat that framing; it goes vertical on Redox itself.

## 1. History and positioning

- **Origin (2015-04)**: Jeremy Soller, then an engineer at System76 (the Denver-based Linux laptop OEM), announced Redox on his personal blog and on reddit /r/rust. The initial tag line — "a Unix-like Operating System written in Rust, aiming to bring the innovations of Rust to a modern microkernel" — still stands verbatim in 2026.
- **Single-vendor phase (2015–2023)**: Soller was effectively the entire core for roughly eight years. System76 granted him 20% time during working hours plus sponsored CI infrastructure. Contributors drifted through — Jackpot51, Ticki, ids1024, 4lDO2 are the multi-year names — but architectural decisions funnelled through Soller.
- **Foundation push (2023-10)**: NLnet Foundation and Futo granted combined funding (~€150k over 18 months) for Redox to become buildable outside x86_64 and self-hosting. This unstuck aarch64 and paid for relibc work.
- **2026-04 status in one sentence**: "alpha, self-hosts rustc on real hardware since 2024, Orbital desktop runs, networking works for curl/wget/ssh client, no GPU, no Wi-Fi, no sound, no browser." (Soller's own framing, 2026-01 release notes.)
- **Cadence**: quarterly-ish. 2026-01 shipped 0.9.0 ("partial USB3 via xHCI + RedoxFS v6"); next release 0.10.0 expected 2026-04 or -05. Prior milestones: 0.8 (2023-01), 0.7 (2022-03), 0.6 (2021-04).
- **Headcount 2026**: 1 lead (Soller), ~5 regular contributors (Jackpot51, ids1024, 4lDO2, Wafelack, Bodil), ~30 drive-by patch authors per release. Donations fund ~0.25 FTE via Patreon + GitHub Sponsors + OpenCollective.

The relevant fact for ALZE: Redox is an eleven-year-old OS with ~1 FTE behind it, and in 2026 it is still correctly described as alpha. This number anchors the entire applicability section below.

## 2. Microkernel architecture and the scheme model

### Kernel size and layout

- `cookbook/recipes/core/kernel/source/` (primary repo as of 2026): roughly 20 KLOC Rust (`tokei` count, counting `unsafe` as Rust). Up from 16 KLOC in 2024. The 4 KLOC growth is mostly aarch64 code and xHCI scaffolding.
- Top-level modules: `arch/{x86_64,aarch64}/`, `context/` (scheduling + process), `memory/`, `scheme/` (the dispatch core), `syscall/`, `time/`, `log/`, `panic/`, `interrupt/`.
- No driver code lives in the kernel. Not even the disk, not even the clock chip beyond the minimum to boot (HPET init is in kernel; AHCI, NVMe, PS/2, PCI enumeration are userspace daemons).

### Everything is a URL

Unix's "everything is a file" stretched to describe sockets (`socket(2)` is not `open(2)`), processes (`proc` is a virtual FS bolted on), devices (`/dev` is ad hoc), IPC (pipes, SysV, POSIX, sockets — pick one). Redox's move: take the URL form literally as the kernel's universal namespace.

- A scheme name is a lowercase ASCII identifier terminated by `:`. Examples as of 2026: `file:`, `disk:`, `pipe:`, `chan:`, `tcp:`, `udp:`, `ip:`, `dns:`, `time:`, `rand:`, `null:`, `zero:`, `proc:`, `thisproc:`, `event:`, `irq:`, `display:`, `keyboard:`, `mouse:`, `serio:`, `audiohw:`, `logging:`, `sys:`, `memory:`, `debug:`, `pcspkr:`, `initfs:`, `kernel.acpi:`, `kernel.dtb:`, `acpi:`.
- A path after the scheme is free-form: the scheme handler parses it however it wants. `tcp:1.2.3.4:80` is the handler's problem, not the kernel's.
- `open("scheme:path", flags)` delivers the path to the **scheme handler**, which is either (a) a userspace daemon that registered the scheme via `SYS_OPEN` on `:schemename`, or (b) one of ~6 in-kernel schemes kept for bootstrap or low-level use.

### Kernel-resident vs userspace schemes (2026 inventory)

| Scheme | Resident | Why in-kernel |
|---|---|---|
| `sys:` | kernel | boot-time kernel info (cpuinfo, uname, context info). Read-only |
| `memory:` | kernel | direct physical memory mapping for the handful of daemons that legitimately need it (AHCI/NVMe MMIO). Root-only cap |
| `thisproc:` | kernel | current process introspection. Fast-path, avoids round-trip |
| `irq:` | kernel | IRQ delivery to userspace. See §7 |
| `event:` | kernel | wakeups on multiple fds, like `epoll(7)` or `kqueue(2)` |
| `logging:` | kernel | early log before log daemon comes up |
| `file:` | userspace | handled by `redoxfs` daemon |
| `disk:` | userspace | handled by `ahcid` / `nvmed` / `ided` |
| `tcp:` `udp:` `ip:` `dns:` | userspace | handled by `smolnetd` (a wrapper around the `smoltcp` crate) |
| `pipe:` | userspace | handled by `ipcd` |
| `display:` `keyboard:` `mouse:` | userspace | handled by `vesad` and `inputd`, later by `orbital` |

Six kernel schemes, roughly a dozen userspace schemes, extensible without recompiling the kernel. A new subsystem = a new daemon + a manifest line. No VFS in the Unix sense.

### Dispatch core

The whole routing layer is about 400 lines in `kernel/src/scheme/`:

```rust
pub trait KernelScheme: Send + Sync {
    fn kopen(&self, path: &str, flags: usize, ctx: CallerCtx) -> Result<OpenResult>;
    fn kread(&self, id: usize, buf: UserSliceWo, ...) -> Result<usize>;
    fn kwrite(&self, id: usize, buf: UserSliceRo, ...) -> Result<usize>;
    fn kclose(&self, id: usize) -> Result<()>;
    fn kfmap(&self, id: usize, req: &Map) -> Result<usize>;
    fn kfsync(&self, id: usize) -> Result<()>;
    fn kdup(&self, id: usize, buf: UserSliceRo, ctx: CallerCtx) -> Result<OpenResult>;
    // and ~10 more, all with default impls returning Err(ENOSYS)
}
```

A scheme registration is a numeric ID + a pointer to a `Arc<dyn KernelScheme>`. `open()` takes the path, strips the scheme prefix, resolves the scheme ID via a hash table lookup (scheme names are interned), dispatches `kopen`. Userspace schemes get forwarded as an IPC-over-handle to the registered daemon — the daemon does the real work and returns a file descriptor number that the kernel then slots into the caller's fd table.

### Influence from Plan 9

Explicit: Soller cites Plan 9's "everything is a file + 9P network FS" as the direct inspiration. But Redox is not Plan 9 clone. Differences:

- Plan 9 has *one* FS protocol (9P) speaking the same verbs everywhere. Redox schemes share verbs (open/read/write/close) but each has its own path grammar. A Plan 9 mountpoint is transparent; a Redox scheme is named.
- Plan 9 encourages per-process namespaces via `bind(1)`. Redox has per-process scheme sets (inherited on fork) but does not push namespace manipulation as a first-class user workflow.
- 9P is network-transparent out of the box. Redox schemes are not, yet — there's a `chan:` scheme that's essentially per-host IPC and no standardized wire protocol like 9P. A "redox-net" scheme has been discussed for years, not implemented.

The takeaway for ALZE: schemes give you a uniform namespace + trivial kernel dispatch + userspace extensibility without needing FUSE or kernel module drama. Plan 9's per-process namespaces are separable and optional; adopt scheme dispatch without mandating namespace plumbing.

## 3. Syscall interface

Redox has a surprisingly small syscall table: about 35 numbered syscalls. Everything that in Linux would be a syscall (open/read/write/close/stat/pipe/socket/bind/connect/accept/epoll_*/mmap/munmap/...) collapses to these verbs over schemes.

Representative inventory 2026 (`syscall/src/number.rs`):

| # | Name | Notes |
|---|---|---|
| 0 | `SYS_CLASS_FILE` bits | fd ops: read, write, fmap, fpath, fstat, fsync, ftruncate, seek |
| 1 | `SYS_CLASS_PATH` bits | open, chmod, chown, rmdir, unlink (takes scheme:path) |
| 20 | `SYS_CLONE` | like `clone(2)`, creates a process or thread with shared/unshared fd tables, memory, scheme namespace |
| 25 | `SYS_EXIT` | |
| 37 | `SYS_KILL` | |
| 43 | `SYS_WAITPID` | |
| 70 | `SYS_FUTEX` | compatible shape to Linux futex |
| 80 | `SYS_YIELD` | |
| 110 | `SYS_IOPL` | x86 only |
| 159 | `SYS_FRENAME` | rename by fd |
| 180 | `SYS_MPROTECT` | page permissions, modern version uses `fmap` with flags |
| 928 | `SYS_GETPID` | |
| 947 | `SYS_GETUID` / `SETUID` | |

The set is kept deliberately Linux-shaped enough that relibc's implementation of POSIX is short: `open(2)` is `SYS_OPEN` with a scheme-prefixed path; if no scheme, relibc prepends `file:` at userspace so the cognitive model is still "everything is a file" for POSIX programs.

### Handles, tokens, capabilities

Redox fds are unforgeable integers indexed into a per-process table. The table entry carries:

- The scheme ID (which daemon owns it).
- The scheme-internal object ID (opaque integer the daemon associates with its own state).
- Per-fd flags (close-on-exec, ...).
- Per-fd seek offset.

A handle is closer to a POSIX file descriptor than to a seL4 capability: it's a per-process integer, not a forgeable kernel object pointer. But it is **attenuable by dup**: `dup(fd, "attenuate_info")` re-opens through the scheme with extra bytes of attenuation data; e.g. `dup(tcp_listener_fd, "read")` returns a read-only handle on a connected client. The scheme defines what attenuation strings mean. This is a lightweight capability discipline via convention, not via kernel-enforced rights bits.

Redox is **not** a capability kernel in the seL4 or Zircon sense — the kernel doesn't enforce rights bitmasks per handle. The discipline happens at the scheme daemon level. Strength: simple kernel. Weakness: every scheme has to get security right itself; uneven in practice.

## 4. Orbital — the display server and window manager

Orbital is Redox's graphical stack. It's not a Wayland implementation and not an Xorg one — it's a from-scratch server with a custom protocol, but the **conceptual** model is Wayland-ish.

### Layers

1. **`vesad`** (userspace daemon) — owns the linear framebuffer given to the kernel by the bootloader (BIOS VBE or UEFI GOP). Exposes `display:`. No GPU acceleration. Runs regardless of GUI state; Orbital is optional.
2. **`orbital`** (userspace daemon) — reads `display:` for pixel output, `keyboard:` and `mouse:` for input. Exposes `orbital:` scheme, which is how clients open windows. Does compositing in pure software.
3. **`orbclient`** — client-side Rust crate. Programs link this and get a `Window` type that handles the `orbital:` protocol, event loop, buffer flips.
4. **`orbtk`** / **`orbterm`** / **`orbterm`** / **`orbutils`** — higher level: widgets (orbtk), terminal emulator (orbterm), file manager (orbfile), calculator, editor, image viewer, launcher.

### Protocol sketch

Opening `orbital:/scheme/orbital/123x456/600,400` creates a 600×400 window with title "123x456". The path grammar itself carries the window request. Events come back through reads on the same fd: 64-byte event records (keyboard, mouse, focus, resize). Pixel buffers are shared via `fmap`: the client mmaps a region of the window fd, writes pixels, calls `fsync` to push.

No hardware acceleration anywhere. Performance is "fine for text and simple 2D up to 1080p on modern CPUs, sluggish above." Soller has said repeatedly that GPU acceleration is explicitly deferred until after self-hosting and USB3 reach stable.

### Comparison to Wayland

- Wayland ships a binary protocol via per-client Unix socket; Orbital uses scheme open + read/write/fmap. Semantically close but the bytes differ.
- Wayland mandates client-side decoration (mostly); Orbital draws window borders server-side (Orbital is a compositor + window manager rolled in one).
- Wayland is protocol-only with multiple compositors (weston, Mutter, KWin, Sway, Hyprland). Orbital is one server.
- Wayland is ~30 KLOC of C (server + libs); Orbital is ~10 KLOC of Rust total across `orbital` + `orbclient` + basic widgets.

### Orbital usability 2026

Boot a Redox 0.9 ISO in QEMU: you get a wallpaper, a launcher, a file manager, a terminal, a text editor. Scroll is crisp. Mouse works. Keyboard works in US and a handful of other layouts. No video playback, no browser (Netsurf has been attempted but not shipping in 0.9), no audio mixing.

## 5. relibc — a Rust-flavored libc

POSIX-shaped C library written in Rust, targeting Redox first + Linux second (yes, can build relibc as a drop-in musl replacement on Linux; used for CI so tests can run without Redox hardware).

- Repo: `gitlab.redox-os.org/redox-os/relibc`, ~40 KLOC Rust + a C compat shim layer.
- Implements: ~85% of musl's API surface as of 2026. Missing pieces: some POSIX threads corners, most of `<aio.h>`, parts of `<sys/wait.h>` edge cases, NIS, XDR.
- Links against: `redox_syscall` crate when targeting Redox, the Linux syscall shim when targeting Linux.
- Gets you: `printf`, `malloc` (dlmalloc-based), `pthread_*`, `socket`/`connect` (transparently proxying to `tcp:` / `udp:` on Redox), `getaddrinfo` (via `dns:`), the whole stdio pipeline, signals (implemented over `event:` + scheme-delivered pseudo-signals).
- Also provides `libm`, `libssp`, `libpthread`, `libdl` as separate .a/.so outputs, for autotools-flavored ports that expect to link `-lpthread`.

Why in Rust: relibc authors want the same memory-safety improvements they applied to the kernel. Also, a Rust libc can lean on the standard Rust `core` crate for UTF-8 handling, numeric parsing, and so on — huge code wins over hand-rolled C.

## 6. RedoxFS — the native filesystem

Redox's native FS, rewritten several times since 2015. Current iteration is v6 in the 0.9 release.

- Repo: `gitlab.redox-os.org/redox-os/redoxfs`, ~8 KLOC Rust.
- Model: log-structured-ish with a block-based b-tree for directories + extents for file data. Not COW in the ZFS sense; not LFS in the BSD LFS sense; pragmatic hybrid.
- **Integrity**: Merkle-like per-block SHA-3 checksums rolled up to a superblock root. Read path verifies on each block fetch. On mismatch: error up. No self-healing via mirrors (ZFS-style) because there's no RAID layer yet.
- **Encryption**: optional AES-256-GCM volume encryption with Argon2i key derivation from passphrase. Default-on for USB install images since 0.8.
- **Crash safety**: journal-like via atomic superblock rotation. Two superblocks; write, fsync, swap pointer, swap again to free old.
- **Resizing**: online grow, offline shrink.
- **Gaps 2026**: no snapshots, no clones, no subvolumes, no dedup, no compression, no RAID, no send/receive, no online fsck, no extended attributes (the scheme-file abstraction encodes some metadata in filename prefixes instead). Basically the set of features ZFS or Btrfs brings to the table, RedoxFS does not.
- **Performance**: "unbenchmarked seriously" is the honest answer. Soller's RustConf 2022 talk showed ~80 MB/s sequential read on NVMe vs. ~3 GB/s for ext4 on the same hardware. A 30-40x gap. Known, tolerated, not a priority.

The active-development lesson: a pure-Rust FS reaching basic write-reliable-data status took eleven years even with the scheme-URL abstraction making the plumbing simple. Writing a modern FS is dominated by on-disk format + crash-recovery invariant work, not by VFS glue. The language choice saves maybe 20% of total effort.

## 7. Drivers in userspace

The microkernel-purist move: drivers are userspace daemons with privileged access to MMIO regions and IRQ lines granted by the kernel at daemon start.

### Mechanism

1. Driver binary declares in its manifest which PCI IDs it claims (e.g. `ahcid` claims `PCI class 0x0106`).
2. `pcid` (the PCI enumerator daemon) walks config space at boot, finds matching devices, and spawns the driver, handing it:
   - An fd on `memory:phys/0xfebf0000/0x10000` giving MMIO access to the BAR.
   - An fd on `irq:16` giving IRQ 16 wakeups.
   - A scheme registration for whatever scheme the driver exposes (`disk:ahci0`, etc.).
3. Driver runs as a normal process, page-mapped via `fmap` on the `memory:` fd, reads IRQs via `read()` on the `irq:` fd (each read blocks until the next IRQ arrives and returns the count).

### Driver inventory 2026

| Daemon | Hardware | Scheme | Status |
|---|---|---|---|
| `pcid` | PCI bus | `pci:` | solid |
| `ahcid` | AHCI/SATA | `disk:ahci/*` | solid |
| `nvmed` | NVMe | `disk:nvme/*` | solid; basic IO queues only, no fancy features |
| `ided` | Legacy IDE | `disk:ide/*` | works, rarely tested |
| `e1000d` | Intel 8254x GbE | `network:` | solid |
| `rtl8139d` | Realtek 8139 | `network:` | solid |
| `rtl8168d` | Realtek 8168 | `network:` | works |
| `xhcid` | USB3 xHCI | `usb:` | partial 0.9, mass-storage + HID only |
| `usbhid` | USB keyboards, mice | via `xhcid` | works |
| `usb-mass-storage` | USB thumb drives | via `xhcid` + `disk:usb/*` | works |
| `ps2d` | PS/2 keyboard + mouse | `keyboard:` `mouse:` | works |
| `vesad` | VBE/GOP framebuffer | `display:` | works, no accel |
| `pcspkr` | PC speaker | `pcspkr:` | works |
| `ac97d` | AC97 audio | `audiohw:` | works on QEMU, unreliable real HW |
| `uartd` | 16550 UART | `serial:` | works |
| `acpid` | ACPI power events | `acpi:` | partial (power button + lid, no full PM) |

Missing, as of 2026: **GPU accel of any kind**, Wi-Fi (no iwlwifi, no ath9k, no brcm), Bluetooth, modern audio (HDA partial, PipeWire-equivalent absent), camera, fingerprint, Thunderbolt beyond USB passthrough.

### Comparison to other microkernel driver models

- **seL4**: drivers-in-userspace yes, but typically compiled against CAmkES component templates with IDL stubs and IRQ caps. More ceremony, more assurance.
- **Fuchsia**: drivers are components under DFv2 driver framework; FIDL between driver and kernel. Richer framework.
- **Redox**: drivers are POSIX-ish daemons that happen to open `memory:` + `irq:` fds. Lowest-ceremony of the three. Trade-off: easier to write, less formal about rights.
- **Linux monolithic (for contrast)**: drivers live in kernel; any bug is a kernel bug. Redox driver crash is a daemon crash, restartable.

## 8. redoxpkg — package ecosystem

Redox's build system is the **cookbook**: recipes in TOML + shell scripts that cross-compile C/Rust/etc. packages for Redox targets. Meta-repo `gitlab.redox-os.org/redox-os/cookbook`; each recipe is a directory with a `recipe.toml` + patches.

### Current package count 2026

Rough tally from `cookbook/recipes/**/recipe.toml` in the 0.9 tree: **~900 recipes**. Up from ~500 in 2022. Includes:

- **Core system**: kernel, relibc, init, drivers, orbital, orbutils.
- **Rust toolchain**: rustc, cargo, llvm (partially rebuilt for Redox target).
- **Compilers**: gcc (ported, partially working), binutils.
- **Languages**: Python 3.11 (partial), Lua, Perl (partial), Ruby (broken), Tcl (works).
- **Editors**: vim, nano, micro, emacs (partial).
- **Shells**: bash, dash, `ion` (Redox-native Rust shell).
- **Unix userland**: coreutils (GNU port + a Rust coreutils variant), findutils, grep, sed, awk (gawk), make, git, ssh (OpenSSH client works, server sketchy), rsync, curl, wget.
- **Network tools**: netcat, ping, dig, whois, nmap (partial).
- **Graphics**: libpng, libjpeg, freetype, cairo (partial), pixman, mesa-software.
- **Browsers**: **NetSurf** (works, limited JS), **Servo** (dead upstream since 2020, reanimated as part of Linux Foundation in 2023; Redox port is aspirational, not shipping).
- **No Firefox** port. XUL and the complex build machinery around it have never made it to Redox.
- **Alacritty**: yes, works on Orbital as a terminal emulator, one of the showcase ports.
- **Games**: DOOM (Chocolate Doom port), a handful of Rust indie games (pure-Rust, easy ports).

### Self-hosting status

Big milestone that shipped in 2024 and is re-confirmed in 2026: **rustc + cargo run natively on Redox on real x86_64 hardware**. You can `cargo build` a Redox program from within Redox. A full kernel rebuild from inside Redox takes ~40 minutes on a reasonable laptop; it works but nobody does it for real development — the cookbook cross-compile from Linux is still the day-job flow.

Not self-hosting: Redox cannot yet rebuild its own LLVM inside Redox. That still needs a Linux host. The LLVM dependency chain is gnarly and the port of tablegen/etc. is partial.

## 9. Platform support 2026

- **x86_64**: primary. Every release is tested on this. Hardware support best: Intel HD graphics framebuffer works; AHCI + NVMe + e1000/rtl8168 cover a large fraction of development laptops + desktop PCs.
- **i686**: deprecated. Legacy 32-bit x86 port exists but gets token CI; not tested on hardware. Likely to be dropped in 1.0.
- **aarch64**: WIP, functional on QEMU virt machine, partial on Raspberry Pi 4, progressing on Pi 5 + Apple Silicon (with Asahi-style hurdles). NLnet grant 2023 specifically paid for this work. 2026 status: "boots to Orbital on Pi 4, wobbly on other hardware."
- **riscv64**: nascent. `kernel/src/arch/riscv64/` stubs exist since 2024. Boots to userspace on QEMU virt. No real hardware tested. Not a release target.

Compare to Linux's ~30 active architectures or NetBSD's ~60: Redox is a 2-3 architecture OS. This is not a criticism — it's a realistic scope for a microkernel with ~1 FTE.

## 10. Community and governance

- **Website**: redox-os.org (news + about + download).
- **Docs**: doc.redox-os.org (the Redox Book, ~100 pages, maintained).
- **Source**: gitlab.redox-os.org (primary GitLab self-hosted since 2016). GitHub mirror at github.com/redox-os (read-only).
- **Chat**: Matrix (`#redox:matrix.org` as homeserver-independent room) + Discord (invited via website). Matrix bridged to Discord.
- **Forum**: Discourse at discourse.redox-os.org. Low traffic but useful for long-form.
- **Issue tracker**: per-repo on GitLab. Kernel has ~200 open issues in 2026, median age multi-year, realistic backlog.
- **Governance**: BDFL-lite. Soller makes the final call. Major design docs (e.g. the userspace driver model) went through Redox Book RFC-style PRs with public review. Not a formalized RFC process like Rust itself.
- **Funding**: Patreon + GitHub Sponsors + OpenCollective. Total ~$1500/month as of 2026-01 transparency report. NLnet grants intermittent. System76 in-kind CI + hardware donations. Not enough to fund anyone full-time at market rates.

## 11. Redox subsystems maturity 2026 (the table)

| Subsystem | Status | Approx KLOC Rust | Notes |
|---|---|---|---|
| Kernel (scheme dispatch, sched, mm, ctx) | beta | 20 | solid on x86_64; aarch64 WIP; riscv64 stub |
| relibc | beta | 40 | ~85% musl coverage; used for CI on Linux too |
| RedoxFS v6 | alpha (reliable but featureless) | 8 | no snapshots, no RAID, no compression |
| Orbital (display server + WM) | beta on QEMU, alpha on HW | 10 | no GPU accel, software compositing |
| orbclient + orbtk | alpha | 12 | enough for demo apps; not a daily GUI toolkit |
| Drivers (ahcid, nvmed, e1000d, rtl, xhcid, ps2d, vesad, ...) | beta for covered HW, absent for most | 30 total | no Wi-Fi, no GPU, partial USB3, partial audio |
| Network stack (smolnetd) | beta | 5 (Redox) + ~15 (smoltcp crate) | IPv4 works, IPv6 partial, no QUIC |
| ion shell | alpha | 18 | works but not default; bash is default |
| Cookbook (package recipes) | beta | N/A (900 recipes, not a code KLOC) | covers dev toolchain + common CLI utils |
| Self-hosting (rustc + cargo in-tree) | beta | N/A | works since 2024 on x86_64; not on aarch64 yet |
| Formal verification | zero | 0 | not attempted, out of scope |

*(LOC counts exclude tests and vendored dependencies. `cargo tree` on the kernel pulls in ~40 crates; the kernel itself is 20K but with `core`, `alloc`, bit-field crates, it's closer to 50K in the `target/` compile unit.)*

## 12. ALZE applicability

### v1 — "no, different language + different model"

ALZE is C99 + asm, monolithic-ish (single address space with protection rings), ~15 KLOC. Redox is Rust + microkernel, ~20 KLOC in the kernel alone plus 450K around it. The language mismatch alone rules out copy-paste of anything Rust-specific. The microkernel boundary means Redox's "driver" is a separate process with IPC; ALZE's driver is a function call in the kernel.

At v1 there is nothing in Redox to copy wholesale. What's useful is the scheme model as a **design pattern** to study for future work, not to port. Study not copy.

### v2 — "scheme abstraction as a pattern worth studying for ALZE's syscall design"

When ALZE eventually grows a real VFS (it has only an in-memory FS today) and a device model, the scheme abstraction is the single most interesting idea Redox offers. Concretely:

1. **Uniform namespace over heterogeneous services.** Instead of writing separate syscalls for socket/file/pipe/device, have one `alze_open(const char *uri, int flags)` that parses `scheme:path` and dispatches. Each scheme is a kernel module (ALZE is not microkernel; the scheme handler lives in-kernel as a function table, not a userspace daemon). Gets you the namespace uniformity without the IPC cost.
2. **Extensibility without ABI churn.** A new scheme is a new handler registration. The syscall ABI doesn't change to add `tcp:` or `serial:`. Useful for keeping ALZE's ~10 syscalls stable while functionality grows.
3. **Grep-able security surface.** Every capability-relevant I/O goes through scheme open. Audit = review the handlers. Cleaner than Linux's scattered `sys_*` surface.
4. **Userland testing.** Redox's `redox_syscall` crate is importable on Linux as a mock. ALZE could do the equivalent: a userspace harness where `alze_open("file:/foo")` hits an in-process scheme registry, enabling unit tests for scheme logic without QEMU. The in-memory FS could be the first user of this.

What to leave on the table: the *userspace* driver model. ALZE does not have the IPC cost-budget to pay round-trip latency to a daemon for every disk block. Keep drivers in-kernel. Scheme dispatch in-kernel too.

**v2 action list (concrete, when the time comes)**:

- Design `kscheme_t` trait-equivalent in C99: struct of function pointers with open/read/write/close/fmap/fsync. ~10 fields.
- Add a scheme registry: hash table of interned scheme names → scheme_t*. Kernel start registers `file:`, `null:`, `zero:`, `rand:`, `time:`, `log:`.
- Rewrite the current `sys_open(path, flags)` to parse `scheme:path` and dispatch. Paths without a scheme default to `file:`. Binary-compatible for existing userspace.
- Over months: add `tcp:`, `udp:`, `disk:`, `display:`, `proc:` as additional handlers.

Cost: probably 1-2 KLOC in C + test work. Not trivial but also not a rewrite. Biggest risk: scheme name collisions with existing paths (`c:/Windows` style). Convention enforcement: scheme names must be `[a-z][a-z0-9-]*` and the first `:` is the separator — forbidden in path components for backward compat.

### v3 — "full Rust rewrite — aspirational only"

Rewriting ALZE in Rust targeting Redox's kernel architecture (schemes + userspace drivers + Orbital) is an eleven-year project based on the Redox reference, or ~3 years with a 5-person team at Oxide-tempo (Hubris scale). Neither resource is on the horizon for ALZE. Mark v3 as "only if the project reboots with new goals and new people." Until then, ignore.

The one place where v3 becomes suddenly relevant: **if ALZE decides to pursue formal verification**. A verified kernel in C is possible (see seL4) but requires proving against a specific C subset + Isabelle tooling team. A verified kernel in Rust with Verus is a different proposition but arguably more tractable in 2026 — Verus can reason about Rust's ownership rules directly. That's a strategic pivot, not an incremental step; park it and refer back to r3/rust_in_kernel.md §7 if it ever surfaces as a real goal.

## 13. Honest closing note

Redox is one of the most interesting research operating systems alive in 2026. It has answered, with working code, several "what if" questions:

- *What if Unix's "everything is a file" stretched to explicit URL schemes?* Answer: you get a small kernel, a uniform namespace, extensible userland, and Plan 9's nice properties without Plan 9's obscurity.
- *What if the kernel were in Rust?* Answer: you still have ~15% unsafe, but bugs concentrate there, and the code is greppable for review.
- *What if the full OS were rewritten from first principles?* Answer: it takes ten years of essentially one person's work to reach alpha, even when the primary language is modern and the design is clean.

That last point is the lesson for ALZE. Redox demonstrates that building a full OS is a 15- to 20-year project *without* institutional backing. System76's sponsorship is real but modest. NLnet grants are appreciated but small. The tacit comparison Redox is forced to make — with Linux, which has had tens of thousands of contributor-years — will never resolve in Redox's favor.

For ALZE, the prescription is patience and scope discipline:

- **A hobby OS in 1–2 years should target a vertical slice**, not a wide slice. Pick "boot + shell + one filesystem + one driver" and make them excellent. Do not try to match "everything Linux has."
- **Scheme URL dispatch is a very good vertical-slice idea** because it gives you extensibility for free; future subsystems slot in without redesign.
- **Rust evangelism is inspiring but also a warning.** Inspiring because safety-by-construction is a real win. Warning because it is not enough on its own: eleven Redox years prove that language choice saves maybe 20% of the total effort of building an OS, and the other 80% (drivers, filesystems, networking, user apps, compatibility layers) is the long pole no matter what language you write in.
- **If ALZE finishes its v1 C kernel and is still healthy at 12 months**, the reward is not "switch to Rust and start over" but "ship v1 and then add a scheme dispatcher and a real on-disk FS." Those are the moves that move the needle. A rewrite is not a move; it is a re-scoping of the project into a different project.

Redox is worth studying like one studies an expedition: admire the discipline, note the costs, borrow the maps, do not repeat the full journey.

---

## References

Primary sources (author, year, venue, URL + archive fallback where relevant):

- Jeremy Soller, 2015-04-20, "Introducing Redox, a Rust OS", announcement post, https://www.redox-os.org/news/announcing-redox/ — archive https://web.archive.org/web/2024*/https://www.redox-os.org/news/announcing-redox/
- Jeremy Soller, 2016-03, "Redox: a Rust-based microkernel", LWN 682591, https://lwn.net/Articles/682591/
- Jake Edge, 2024-07-04, "Redox: an operating system in Rust", LWN 979524, https://lwn.net/Articles/979524/
- Redox OS Book (community), continuously updated, https://doc.redox-os.org/book/
- Redox OS News (release notes, 0.6 → 0.9), https://www.redox-os.org/news/
- Redox OS GitLab org, https://gitlab.redox-os.org/redox-os — primary source of truth
- Redox OS GitHub mirror, https://github.com/redox-os
- Jeremy Soller, 2018, "Redox OS: A Practical Rust Microkernel Operating System", RustConf 2018 talk, https://www.youtube.com/watch?v=-D3GhZAGDsk
- Jeremy Soller, 2021, "Redox OS: A year in review", RustConf 2021 talk, https://www.youtube.com/watch?v=90p5R5oSg8Q
- Jeremy Soller, 2022, RustConf 2022 lightning talk on RedoxFS, archived video via YouTube search
- System76 blog, 2019, "System76 & Redox OS", https://blog.system76.com/post/redox-os (archive fallback: web.archive.org)
- NLnet Foundation grant announcement, 2023-10, "Funding Redox OS aarch64 port", https://nlnet.nl/project/Redox-ARM/
- Ars Technica, 2020-02, "Redox OS 0.6 released", https://arstechnica.com/ (search redox-os tag)
- Phoronix, periodic coverage, https://www.phoronix.com/search/redox-os — multiple release-coverage posts 2018–2026
- Redox OS contributors, `cookbook` package recipes, https://gitlab.redox-os.org/redox-os/cookbook
- `redoxfs` crate, https://gitlab.redox-os.org/redox-os/redoxfs
- `orbital` repository, https://gitlab.redox-os.org/redox-os/orbital
- `relibc` repository, https://gitlab.redox-os.org/redox-os/relibc
- `redox_syscall` crate, https://docs.rs/redox_syscall — API docs for the userland syscall binding
- Rob Pike, Dave Presotto, Sean Dorward, Bob Flandrena, Ken Thompson, Howard Trickey, Phil Winterbottom, 1995, "Plan 9 from Bell Labs", Computing Systems 8(3), http://doc.cat-v.org/plan_9/4th_edition/papers/9 — background on the "everything is a file" extension
- Rob Pike, Dennis Ritchie, 1993, "The Styx Architecture for Distributed Systems", AT&T Bell Labs — 9P protocol roots; referenced by Soller in interviews
- Asterinas authors, 2024, OSDI 2024, https://www.usenix.org/conference/osdi24/presentation/chen-tianyu — for contrast with Redox's microkernel choice (monolithic + framekernel)
- LWN Kernel Index "Rust", https://lwn.net/Kernel/Index/#Development_tools-Rust — broader context on Rust in OS kernels
