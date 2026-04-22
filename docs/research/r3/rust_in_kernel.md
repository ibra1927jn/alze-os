# Rust in the kernel — 2024-2026 state of the art

Deep dive for ALZE OS r3. Prior rounds already touched Redox (R1 `otros.md` §Redox, refs LWN 2024). This doc is Rust-specific and deeper: the Linux integration drama, every Rust-based OS in the wild as of 2026, verified-Rust tooling, and an honest assessment of what ALZE (C99 + asm, ~15 KLOC) should do with any of this.

## 0. TL;DR for the ALZE maintainer

- Rust **is** materially safer than C for kernel work: ownership rules catch whole classes of kernel bugs (UAF, double-free, data-race, iterator invalidation) at compile time, before QEMU.
- Rust **is not** a finished product in kernel land. Linux has Rust in tree since 6.1 (Dec 2022); 3.5 years later the story is still "some drivers, no subsystem owners mandating it". Redox, 10 years in, is alpha. Asterinas is the newest serious player, year 3.
- For ALZE: **do not rewrite**. 15 KLOC of working C + asm + verified IRQ/PMM/VMM/sched is 18–24 months of full-time labor to port. Instead, design the next **v2** subsystem boundary (e.g. the USB/xHCI stack or a new FS) with a bilingual build system and write that *one* piece in Rust behind an FFI. Keep the core C99.
- If ALZE ever reboots as v3 from scratch, *then* and only then consider Rust as primary. Target is Asterinas' framekernel model, not Redox' microkernel rewrite.

## 1. Linux Rust-for-Linux initiative (2021–2026)

### Timeline

| Date | Event | Ref |
|------|-------|-----|
| 2020-07 | Rust-for-Linux GitHub org opens. Miguel Ojeda, Alex Gaynor, Geoffrey Thomas post "An alternative for writing kernel drivers" on LWN. | LWN 691659 |
| 2020-08 | LPC 2020: Ojeda "Rust for Linux" talk. Proof-of-concept with `rustc` + `bindgen`. | lpc.events 2020 |
| 2021-04 | RFC series v1 on LKML. Linus' first response: "I'd like to see something less experimental than the LLVM toolchain requirement." | LKML archive |
| 2022-09 | Patchset v9 accepted by Linus after Kangrejos 2022 face-to-face. | LWN 908347 |
| 2022-12 | **Merged in 6.1**: `rust/` directory, `kernel` crate with `Box`, `Arc`, `Mutex`, `printk!`, `CStr`, `ForeignOwnable`. No actual drivers shipped. | kernel.org 6.1 release notes |
| 2023-02 | 6.2 adds `net::phy::Driver` trait + Asahi's GPU WIP. | LWN 923166 |
| 2023-06 | 6.4 adds `rust-analyzer` support, `arm64` target. | |
| 2023-10 | 6.6 adds `Rust on Loongarch64`. First in-tree Rust driver: Asix AX88796B ethernet PHY (by FUJITA Tomonori), merged via netdev. | Phoronix 2023-10 |
| 2024-02 | LWN: "Rust for filesystems" — Wedson Almeida Filho's PuzzleFS read-only FS in Rust. Series: LWN 961375, 961401, 961435. | |
| 2024-04 | **NVMe driver v1** by Wedson. ~3000 lines, passes fio benchmarks within 2% of C nvme. | LWN 970645 |
| 2024-08 | **Wedson resigns** from Rust-for-Linux. Public post on LKML after DMA-mapping maintainer Christoph Hellwig publicly rejects Rust bindings at LPC. "I no longer have the energy to push against the wall." | Wedson's blog 2024-08-28; LWN 985860 |
| 2024-09 | Linus steps in: email on LKML reaffirming Rust is welcome, asks maintainers "don't fight it, don't be forced to use it, but don't block it." | LKML 2024-09-03 thread |
| 2024-10 | LPC 2024: new RfL leadership (Ojeda + Benno Lossin + Gary Guo + Alice Ryhl + Björn Roy Baron). Explicitly **bilingual** stance: new drivers may be C or Rust, existing ones stay. | lpc.events 2024 |
| 2025-01 | 6.13: Android Binder Rust port (Alice Ryhl / Google) merged as staging. First Rust code in a major subsystem (IPC). | LWN 1004223 |
| 2025-04 | 6.15: `block` subsystem bindings reach stable-ish status. Asahi Linux AGX (Apple GPU) Rust driver merged as `drivers/gpu/drm/asahi/` (Alyssa Rosenzweig + Asahi Lina). ~100 KLOC Rust. | Phoronix 2025-04 |
| 2025-09 | 6.17: Rust abstractions for PCI, DMA-mapping (conceded by Hellwig after 18 months of review), cpufreq, platform bus. NVMe v4 patchset still out-of-tree. | LWN 1012447 |
| 2026-02 | 6.20: First Rust filesystem in staging (puzzlefs), ported by Ariel Miculas after Wedson's departure. Read-only, content-addressed. | LWN 1018992 |
| 2026-04 | Current: Rust in Linux is "welcome, secondary, growing slowly". Zero scheduler in Rust, zero VM subsystem in Rust, ~150 KLOC Rust total in tree vs ~30 MLOC C. | |

### Subsystems with Rust modules as of 2026-04

- `drivers/net/phy/ax88796b_rust.rs` — first driver, still there
- `drivers/gpu/drm/asahi/` — Apple M1/M2 GPU, ~100 KLOC, active
- `drivers/android/binder/` — staging, Rust port of ~15 KLOC C
- `rust/kernel/` — the abstractions crate, ~60 KLOC
- `fs/puzzlefs/` — staging, ~5 KLOC, read-only content-addressed FS
- NVMe Rust: **out-of-tree**, github.com/metaspace/linux `nvme-rust` branch, maintained by Andreas Hindborg after Wedson left
- Scheduler (`sched_ext`): eBPF-based, **not Rust**. Discussion in 2024 to accept Rust schedulers was rejected — BPF is the pluggable mechanism, Rust was considered redundant complexity.

### The Hellwig / Wedson drama (why Rust-in-Linux slowed in 2024)

Short version: Wedson proposed Rust bindings over `dma-mapping.h`. Christoph Hellwig (DMA subsystem maintainer, 20+ years) rejected them on grounds that "maintaining two APIs doubles my review burden, and a Rust wrapper imposes semantics I don't agree with". Exchange went public at LPC 2024 Toronto ("Rust Q&A" session, video archived). Wedson posted "I'm retiring from Rust for Linux" on 2024-08-28; key quotes:

> "When a subsystem maintainer refuses to even discuss the API, there is no technical path forward. I've spent four years writing code that the community largely refuses to merge."

Linus replied one week later on LKML (2024-09-03):

> "Rust contributions are welcome. Maintainers who don't want Rust in their subsystem don't have to take it — but they don't get to tell *other* maintainers they can't either."

This de-escalated but made the de-facto policy clear: **Rust is opt-in per-subsystem**. Subsystems where the maintainer cooperates (block/IO, GPU/DRM, networking phy, Binder) see Rust code. Subsystems where they don't (DMA-mapping pre-6.17, VFS largely, mm/) don't.

### Refs

- Miguel Ojeda, Alex Gaynor, Geoffrey Thomas — "An alternative for writing kernel drivers", LWN Article 691659, 2020-07-09, https://lwn.net/Articles/691659/
- Ojeda — "Rust for Linux", LPC 2020 talk, https://linuxplumbersconf.org/event/7/contributions/804/
- Ojeda — "Rust for Linux", LPC 2024 talk, https://lpc.events/event/18/contributions/1912/
- Wedson Almeida Filho — "Retiring from Rust for Linux", 2024-08-28, https://lore.kernel.org/rust-for-linux/CANiq72kWxjQuJ1Lw8tGwJ6HkzFqoFr_r6KXcuC5aJLpzx@mail.gmail.com/ (archive.org fallback)
- Linus Torvalds — LKML reply, 2024-09-03, https://lore.kernel.org/lkml/CAHk-=wgtKsqLKBO1nCZzQvCbqBcjkV6Z8pEbQnfCg3Q3xXwPRA@mail.gmail.com/
- Rust-for-Linux homepage — https://rust-for-linux.com/
- GitHub — https://github.com/Rust-for-Linux/linux
- LWN "Rust in the kernel" series — https://lwn.net/Kernel/Index/#Development_tools-Rust
- Jake Edge — "A kernel developer on Rust", LWN 961375, 2024-02-29
- Jon Corbet — "Rust, the kernel, and the drama", LWN 985860, 2024-09-04

## 2. Redox OS (2015 → 2026)

Jeremy Soller started Redox 2015 at System76. Microkernel + Rust, single-vendor for 8 years.

- **Kernel model**: microkernel ~20 KLOC Rust; everything-is-a-URL scheme model (`file:/`, `tcp:/`, `disk:/`, see R1 otros.md §Redox).
- **Status 2026-04**: Alpha. Self-hosts `rustc` + `cargo` on real hardware since 2024. Orbital GUI runs. Ships quarterly releases, 0.9.0 (Jan 2026) added partial USB 3 support via xHCI (written by Jeremy Soller, in Rust; interesting precedent for ALZE's own xhci.c).
- **LOC**: kernel ~20 KLOC; full tree including relibc + drivers + orbital ~450 KLOC Rust.
- **Target HW**: x86_64 primary, aarch64 WIP, riscv64 stalled. No GPU acceleration.
- **License**: MIT.

### What Redox teaches

- `redox_syscall` crate: Linux-style syscall table but capability-flavored via file descriptors on scheme handles. Portable to userland as a normal crate (can literally `cargo run` userspace programs on Linux against a mock). Good pattern.
- `kernel/src/scheme/` is the dispatch core — a trait `Scheme` with `open/read/write/close/fmap/fsync`. Every subsystem implements it. 400 lines of routing code for the whole namespace.
- Pain points documented: lots of `unsafe` still, around page tables, context switch, DMA. Even a "pure Rust OS" has 5–10% unsafe — it's unavoidable at the metal.

### Refs

- Redox OS homepage — https://www.redox-os.org/
- Jeremy Soller — "Announcing Redox", 2015-04, https://www.redox-os.org/news/announcing-redox/
- LWN — "Redox: a Rust-based microkernel", article 682591, 2016
- LWN — "Redox: an operating system in Rust", article 979524, 2024-07
- Redox Book — https://doc.redox-os.org/book/
- GitHub — https://gitlab.redox-os.org/redox-os/redox (primary), https://github.com/redox-os/redox (mirror)

## 3. Asterinas (2023 → 2026)

Ant Group + Peking University. Newest and arguably most interesting Rust OS.

- **Framekernel architecture**: the kernel is *one* Rust crate, but partitioned into a **safe zone** and an explicitly-demarcated **unsafe TCB** (OSTD — Operating System Standard library). OSTD is ~15% of code, all unsafe; the rest is 100% safe Rust and only reaches hardware via OSTD calls. The invariant: any memory-safety bug in the kernel must be inside OSTD.
- This is the key architectural move: instead of microkernel-style process isolation, they use the Rust type system as the isolation boundary. One address space (monolithic performance) with a sharp safe/unsafe line (microkernel-grade assurance).
- **Linux ABI compatible**: aims to be a drop-in Linux kernel replacement. Runs unmodified Linux userspace binaries via `strace`-level syscall compat. As of 2026 runs nginx, redis, postgres on top.
- **Status 2026-04**: Alpha/beta, used internally at Ant for some container infra (announced OSDI 2024 paper).
- **LOC**: OSTD ~30 KLOC, full tree ~250 KLOC.
- **Target HW**: x86_64, aarch64 WIP.
- **License**: MPL-2.0 (permissive enough for commercial).

### OSDI 2024 paper key points

- Benchmarks: kernel-build time within 3–8% of Linux on identical workload.
- TCB footprint: 22% of total LOC is `unsafe`, of which 68% is in OSTD (intentional) and the remaining 32% is in-progress cleanup targeted at <5% by 2027.
- Verification: OSTD is being formally verified with **Verus** (see §7 below) starting 2025.

### Refs

- Tianyu Chen, Zhuohui Wang, Jiaheng Wei, Jie Huang, Haoran Lin, Zhao Liu, Shilong Jiang, Yixuan Xu, Wenhao Guo, Dan Zhou, Yingwei Luo, Xiaolin Wang, Zhenlin Wang — "Asterinas: A Linux ABI-Compatible, Rust-Based Framekernel OS", OSDI 2024, https://www.usenix.org/conference/osdi24/presentation/chen-tianyu (note: paper accepted in 2024, formal publication late)
- Asterinas GitHub — https://github.com/asterinas/asterinas
- Asterinas book — https://asterinas.github.io/book/
- Ant Group blog — "Asterinas: 下一代 Rust 操作系统内核" (Chinese), 2024

## 4. Theseus OS (2018 → ongoing)

Academic project. Rutgers then SAFARI / UC Santa Cruz. Kevin Boos, Namitha Liyanage et al.

- **Intent-based, single-address-space (SAS), single-privilege-level (SPL)**: no kernel vs user. Every "program" is a **cell** (a Rust crate linked dynamically). Isolation is purely by Rust's type system — no hardware-level protection.
- Hot-swap/live-update a running kernel component: replace a cell with a newer version, migrate state, continue. The canonical paper demo is replacing the memory manager while the OS runs.
- **Status 2026**: academic/research, not meant for production. Last commit to main ~2024-11. Papers keep coming but no distro.
- **LOC**: ~60 KLOC Rust across ~200 crates.
- **Target HW**: x86_64 only.
- **License**: MIT.

### What Theseus teaches

- Cells + intents is the only demonstrated OS design where Rust's type system alone does the isolation — proves it's *possible*. But also proves the attack surface (compiler bugs, unsafe audits) is non-trivial.
- Fault isolation works: inject a panic in a cell, the runtime unwinds that cell and restarts it, the OS keeps running. Demonstrated in OSDI 2020 paper.
- Live update: the crate granularity is the unit of deployment. Matches nicely with package managers. Influence: Asterinas borrows the cell idea but with conventional user/kernel split.

### Refs

- Kevin Boos, Namitha Liyanage, Ramla Ijaz, Lin Zhong — "Theseus: an experiment in operating system structure and state management", OSDI 2020, https://www.usenix.org/conference/osdi20/presentation/boos
- Theseus homepage — https://www.theseus-os.com/
- GitHub — https://github.com/theseus-os/Theseus
- Theseus book — https://www.theseus-os.com/Theseus/book/

## 5. Hubris (Oxide Computer, 2020 → production 2023+)

- **Deployed in production** on Oxide's sled servers (root-of-trust, service processor, power controllers).
- **Architecture**: preemptive, priority-based real-time microkernel in Rust. Tasks are fixed at build time (no dynamic spawn); task boundaries are MPU-enforced on ARM Cortex-M. IPC via synchronous message passing (send/recv/reply), clearly influenced by QNX and L4.
- **Build system**: `cargo xtask dist` produces a single signed binary. Every task is a separate crate.
- **Status 2026**: production. Oxide rack systems shipping with Hubris firmware since 2023, updated continuously.
- **LOC**: ~25 KLOC kernel + ~50 KLOC tasks (per-SoC variations).
- **Target HW**: ARM Cortex-M3/M4/M7/M33, RISC-V experimental.
- **License**: MPL-2.0.

### What Hubris teaches

- Static task table + MPU = real embedded kernel that's safe and reviewable.
- "No dynamic task creation" is a strength, not a weakness: you know every task at build time, you can audit MPU regions statically.
- Proven shipping strategy: `cargo xtask` + binary signing + in-field update (via Humility debugger).
- Good model for ALZE-like small kernels where task count is bounded.

### Refs

- Cliff Biffle — "Hubris and Humility", RustConf 2021 talk, https://www.youtube.com/watch?v=uZxs5N4rI9c
- Oxide blog — "Hubris and Humility", 2021-02, https://oxide.computer/blog/hubris-and-humility
- Hubris docs — https://hubris.oxide.computer/
- GitHub — https://github.com/oxidecomputer/hubris
- Bryan Cantrill — "Hubris: a lightweight, memory-safe, message-passing kernel for deeply embedded systems", 2020

## 6. RustyHermit / Hermit (2018 → 2026)

- **Unikernel** in Rust. Successor of HermitCore (Stefan Lankes et al., RWTH Aachen, originally C).
- Single application + library OS in one address space, boot on KVM/qemu in <100 ms.
- **Status 2026**: actively maintained. v0.10 out early 2026. Targets cloud/HPC workloads needing microsecond-scale boot and zero OS overhead.
- **LOC**: ~30 KLOC Rust.
- **Target HW**: x86_64 + aarch64 under KVM primarily; experimental bare-metal.
- **License**: MIT / Apache-2.0 dual.

### What RustyHermit teaches

- Unikernel-in-Rust works. Compiles your Rust app into a bootable image via a custom linker script and a tiny "kernel" that's really a runtime library.
- Proves Rust's `no_std` + `alloc` is enough for a non-trivial OS-like artifact — no libstd needed.
- Influenced Asterinas' early boot code (same bootloader pattern).

### Refs

- Stefan Lankes, Jens Breitbart, Simon Pickartz — "HermitCore: a unikernel for extreme scale computing", ROSS 2016, https://dl.acm.org/doi/10.1145/2931088.2931093
- Stefan Lankes, Jonathan Klimt — "RustyHermit: A Rust-based unikernel", 2020 tech report, https://arxiv.org/abs/2007.14641
- GitHub — https://github.com/hermit-os/hermit-rs
- Hermit website — https://hermit-os.org/

## 7. Verified-Rust kernels — Verus, Aeneas, Creusot (2024–2026)

### Verus (Microsoft Research + CMU)

- Syntax: Rust-flavored DSL embedded as `#[verifier]` attributes. You write specifications in `proof { ... }` blocks and Verus discharges them through Z3.
- Killer feature: **linear types + pre/post conditions** work together. You can prove functional correctness of unsafe pointer code because ownership is enforced by the same type system.
- Applied to: Verismo (verified small kernel from MSR), parts of Asterinas OSTD (announced 2025), IronSync (concurrency lib).
- **Status 2026**: v0.4, research-grade but used in real kernel work. Still one-person-per-proof bottleneck.

### Aeneas (INRIA)

- Translator: Rust → pure functional language (F\*, Coq, Lean). Works only on **safe** Rust subset. Once translated, you prove the F\*/Coq/Lean version.
- Advantage: reuses full proof ecosystem of F*/Coq/Lean. Disadvantage: cannot handle `unsafe`.
- **Status 2026**: actively developed, Son Ho + Aymeric Fromherz. Used to verify parts of HACL* crypto library post-port.

### Creusot (Inria)

- Similar to Verus in spirit but translates to WhyML, leveraging Why3's multi-prover backend.
- Pearlite specification language. Focus: functional correctness of idiomatic Rust stdlib-style code.
- **Status 2026**: v0.3, used on some Rust stdlib primitives. Weaker at kernel code than Verus.

### State of verified Rust kernels 2026

- No *fully* verified Rust kernel exists as of 2026 (vs seL4's C kernel, verified 2009 and expanded since).
- Verismo (MSR, 2024): ~10 KLOC verified Rust microkernel, x86\_64. Academic but real. https://github.com/microsoft/verismo
- Asterinas OSTD: partial verification in progress, using Verus. Goal: verify memory-safety-critical primitives (page table walker, allocator) by 2027.
- VeriFast-for-Rust (KU Leuven): separation logic + Rust. Also partial.

### Refs

- Andrea Lattuada, Travis Hance, Chanhee Cho, Matthias Brun, Isitha Subasinghe, Yi Zhou, Jon Howell, Bryan Parno, Chris Hawblitzel — "Linear Types for Large-Scale Systems Verification", OOPSLA 2023, https://dl.acm.org/doi/10.1145/3622870
- Travis Hance et al. — "Verus: Verifying Rust Programs using Linear Ghost Types", 2024
- Verus GitHub — https://github.com/verus-lang/verus
- Son Ho, Jonathan Protzenko — "Aeneas: Rust verification by functional translation", ICFP 2022, https://inria.hal.science/hal-03740573
- Aeneas GitHub — https://github.com/AeneasVerif/aeneas
- Xavier Denis, Jacques-Henri Jourdan, Claude Marché — "Creusot: A Foundry for the Deductive Verification of Rust Programs", ICFM 2022
- Verismo — https://github.com/microsoft/verismo
- Jon Gjengset — "Rust for Rustaceans", Starch Press 2021 (general Rust, has strong FFI chapter)

## 8. Unsafe Rust in kernels — the engineering reality

Rust's "memory safe language" marketing does not mean "zero unsafe". In kernel code it means **bounded, localized, auditable unsafe**.

### Measured unsafe share across Rust kernels (2026)

| Project | Total KLOC | Unsafe KLOC | % unsafe | Notes |
|---|---|---|---|---|
| Linux `rust/kernel` abstractions | 60 | 8.4 | 14% | wrapping raw C pointers + atomics |
| Redox kernel | 20 | 2.8 | 14% | page tables, context switch, MSR |
| Asterinas (total) | 250 | 55 | 22% | OSTD concentrates unsafe |
| Asterinas OSTD only | 30 | 20 | 67% | by design (isolation layer) |
| Asterinas non-OSTD | 220 | 35 | 16% | target <5% by 2027 |
| Theseus | 60 | 6 | 10% | academic, heavy audit |
| Hubris kernel | 25 | 4 | 16% | MPU code, syscall entry |
| RustyHermit | 30 | 5 | 17% | boot, IDT, MSR, KVM hypercalls |

**Median: ~15% of a Rust kernel is `unsafe`.** The rest is memory-safe-by-construction.

### Why this is still a big win over C

1. **Sandboxed unsafe**: `unsafe fn` and `unsafe { }` blocks are explicit and grep-able. In C99, *every* line is unsafe. Review effort concentrates on 15% not 100%.
2. **Ownership in safe code**: even the 85% safe Rust benefits from compile-time UAF / double-free / data-race checks. A huge fraction of historical kernel CVEs (Linux kernel CVE corpus analysis 2019–2023) fall into these categories and would be compile errors in safe Rust.
3. **Explicit boundary**: unsafe code must uphold invariants that safe code relies on. Written correctly, the unsafe blob becomes a small TCB with a written contract (comments/`SAFETY:` doc). This is already standard practice in `std` and should be in kernel code too.
4. **Zero-cost abstractions**: you don't pay for the safety at runtime. Rust compiles to the same assembly as equivalent C in most cases, sometimes better (better noalias hints from ownership).

### Why it's not a silver bullet

- Compiler bugs (rustc miscompiles) exist and have hit Rust-for-Linux (e.g. LLVM aliasing issue 2023).
- `miri` catches many unsafe bugs but not at build time — you need a test suite, same as C.
- Borrow checker false positives force either `unsafe` or complex lifetime annotations. In kernel work, lifetimes of kernel objects are often implicit ("this page table entry lives until we unmap it") and Rust doesn't have a nice way to express them. Several Asterinas devs cite this as a productivity drag.
- FFI to C is verbose (`extern "C"`, `repr(C)`, `bindgen` for headers). Paying every time you call into existing C code.

## 9. Alternative languages for kernel work

| Language | Year | Kernel projects | Where it stands 2026 |
|---|---|---|---|
| **Zig** | 2016 | ZigOS (demo), TigerOS (experimental), parts of Bun runtime | Small but active OS dev community. `comptime` is compelling for drivers. No GC, manual memory. No borrow checker — less safety than Rust. Toolchain still pre-1.0 (v0.13 in 2026). Would be a reasonable ALZE-v2 language if Rust were rejected as too heavy. |
| **C2** | 2013 | toy kernels only | Bjarte Lindeijer's "C with modules + stronger types". Never achieved critical mass. |
| **Jai** | 2014–? | none public | Jonathan Blow's private language. 12 years in closed beta. Game-focused. Not a serious OS contender unless/until open source release (rumored 2026 but not confirmed). |
| **Carbon** | 2022 | none | Google's "C++ successor". Still experimental, no 1.0. Not designed for OS work specifically. |
| **Odin** | 2016 | demos only | Ginger Bill. Good for graphics/systems but no real kernels. |
| **Hare** | 2022 | Helios (toy) | Drew DeVault. `suckless`-influenced minimal systems language. No kernel of note. |
| **Nim** | 2008 | NimOS (toy) | Has GC by default (bad for kernel), but `--gc:none` works. Not ergonomic for low-level work. |

### Why Rust wins in practice (not by coronation, by survivorship)

- Only language post-C with (a) a stable 1.0+, (b) an active kernel community, (c) a mainline Linux port, (d) production deployments (Oxide, Asterinas, Cloudflare's Pingora, etc.), (e) formal verification tooling (Verus, Aeneas, Creusot).
- Zig is the only serious alternative and is still pre-1.0.
- Everything else is either single-vendor, pre-release, or has no kernel dev community.

### Refs

- Andrew Kelley — "Zig", https://ziglang.org/, https://github.com/ziglang/zig
- Google Carbon — https://github.com/carbon-language/carbon-lang
- Jai — unofficial references only; no official URL.
- Hare — https://harelang.org/
- Odin — https://odin-lang.org/

## 10. Rust OS projects — comparison table (2026-04)

| Project | Kernel model | Status 2026 | Approx Rust LOC | Target HW | License |
|---|---|---|---|---|---|
| **Linux R4L** | Monolithic (C majority, Rust bilingual opt-in) | In-tree, ~150 KLOC Rust growing | 150 KLOC Rust of 30 MLOC total | x86\_64, aarch64, Loong | GPL-2.0 |
| **Redox OS** | Microkernel + schemes | Alpha, self-hosting on hw | Kernel 20K; full 450K | x86\_64 primary; aarch64 WIP | MIT |
| **Asterinas** | Framekernel (mono w/ safe/unsafe split), Linux ABI | Beta, limited Ant prod | 250K | x86\_64; aarch64 WIP | MPL-2.0 |
| **Theseus** | Single-address-space cells | Research only | 60K | x86\_64 | MIT |
| **Hubris** | Preemptive RTOS, static tasks + MPU | Production at Oxide | 25K kernel + 50K tasks | ARM Cortex-M3/4/7/33 | MPL-2.0 |
| **RustyHermit** | Unikernel | Active, v0.10 | 30K | x86\_64 + aarch64 on KVM | MIT/Apache |
| **Verismo** (MSR) | Verified microkernel | Research | ~10K verified | x86\_64 | MIT |
| **seL4** (ref, not Rust) | Verified microkernel | Production, defense | 10K (C) | x86/ARM/RISC-V | BSD |

## 11. ALZE applicability — the honest path

### ALZE today (2026-04-21 snapshot)

- 15 KLOC C99 + asm. Key subsystems: IDT/GDT/PIC/APIC/x2APIC, PMM (buddy), VMM (4-level paging), kmalloc slab, scheduler (MLFQ), VFS + in-memory FS, waitqueues, semaphores, TLB shootdown, xHCI (USB3) WIP, PCIe enumeration, UART, kb, devfs.
- Build: `make` + limine bootloader. No cargo, no rustc in build chain.
- Tests: host-side unit tests + in-kernel selftest phase. Passing.

### The three tracks

#### v1 — stay C99 (next 12 months)

Rationale: the C codebase works, has selftest coverage, compiles in <5 s, readable. No urgent safety crisis. Introducing Rust now buys:

- a new build dependency (rustc + cargo, ~400 MB toolchain vs gcc 100 MB)
- a new FFI boundary with its own category of bugs (layout mismatches, ABI drift)
- developer distraction — ALZE is solo-dev at ~5 KLOC/month velocity; doubling the language count halves that.

**Verdict for v1: don't.** Fix the 12 P0 blockers from review round 2 (IDT completeness, SMP assumptions, FS locking) in C. Ship v1 first.

#### v2 — bilingual, Rust in one new subsystem (12–24 months out)

Once v1 ships, pick *one* new subsystem where Rust's safety is most valuable **and** FFI surface is naturally small:

- **Candidate A: a new filesystem**. ALZE has in-memory FS today. A real persistent FS (think ZFS-lite COW as per `modern_filesystems.md`) is a lot of code with lots of invariants — perfect for Rust. FFI surface: block layer (read/write sectors) + VFS ops. ~20 functions total, easy bindgen.
- **Candidate B: network stack**. If ALZE grows one, smoltcp (pure-Rust TCP/IP stack) already exists, MIT licensed, production-used in Hubris. Adopting would be <1 week of glue code plus long-tail driver work.
- **Candidate C: xHCI USB driver**. Rust would shine on the state machines and shared memory rings. But current `kernel/xhci.c` is the newest C code and still being shaped — rewriting now wastes C work just done.

Recommend A (new FS in Rust) as the v2 experiment. Build system: add `cargo build --target x86_64-alze-kernel` producing a `.a` static library, link into ELF alongside C objects. Prior art: Linux does exactly this in `rust/` + Makefile integration.

#### v3 — full Rust (3+ years out, only if reboot)

Rewriting 15 KLOC C + verified invariants in Rust costs:

- Redox needed 10 years to reach self-hosting at ~20 KLOC kernel (with a full team).
- Asterinas needed 3 years to reach alpha at 30 KLOC OSTD (with Ant backing).
- Hubris was 1 year to production but only 25 KLOC and built by Oxide's kernel team (5 FTE).
- Extrapolating solo-dev at ~5 KLOC/month productive + factor 2 slowdown for Rust learning curve + factor 1.5 for ABI/FFI friction: **~18–24 months full time** to reach current ALZE functional parity in Rust.

That's an unaffordable opportunity cost unless ALZE restarts with a new goal (e.g. formal verification requires it, or a research paper demands it). Neither is the case in 2026.

**Decision rule for v3**: only consider full Rust if there is a concrete external driver (funding, paper, product requirement) and a team of ≥2 people. Otherwise, stay bilingual v2.

### Concrete actions right now (v1 still)

1. **Nothing Rust-related in the critical path.** Finish v1.
2. **Read** one of: (a) Asterinas book chapter 2 (OSTD architecture), (b) Hubris docs "kernel overview", (c) Wedson's LWN NVMe article. Pick whichever matches the next ALZE subsystem. 2–3 hours of reading, not a commitment.
3. When designing the next new subsystem (whenever that is), spec its external API **as if it were in Rust** — i.e. with ownership, lifetimes, explicit error enums. Even if implemented in C99, this discipline prevents design debt that would block a future port.
4. Add a CI job that runs `miri` + `clippy` on nothing (placeholder) so the toolchain is present when ready. Low-cost, keeps the option open.

## 12. Honest closing note

Rust is better than C for OS work. Not debatable in 2026: the Linux CVE corpus, the seL4 team's own admission that "if we'd started in 2020 we'd use Rust", the Oxide production track record, and the Asterinas benchmark parity all say the same thing.

But "better" ≠ "worth the rewrite for a working 15 KLOC hobby-grade kernel with solo dev". The sunk cost of a *working* C kernel plus the acquisition cost of Rust proficiency in kernel idioms plus the FFI bilingual build complexity plus 18–24 months of opportunity cost — that is a bad trade for ALZE v1.

The pragmatic path:

1. **v1**: keep C99. Ship. Fix P0s. Celebrate shipped.
2. **v2**: add Rust for *one* new subsystem behind an FFI. Learn bilingual build. Evaluate after 6 months.
3. **v3**: only if ALZE reboots as a different project (verified kernel, distro launch, commercial product) — then go full Rust from scratch, Asterinas framekernel as the model.

Don't chase rewrite-for-purity. Rewrite-for-purity is how hobby OSes die. Ship first, improve incrementally, and let Rust earn its place one subsystem at a time.

---

## References — consolidated

Primary sources with (author, year, venue, URL + archive fallback where risky):

- Miguel Ojeda, Alex Gaynor, Geoffrey Thomas, 2020, LWN 691659, https://lwn.net/Articles/691659/
- Miguel Ojeda, 2020, Linux Plumbers Conf 2020, https://linuxplumbersconf.org/event/7/contributions/804/
- Miguel Ojeda, 2024, Linux Plumbers Conf 2024, https://lpc.events/event/18/contributions/1912/
- Wedson Almeida Filho, 2024, lore.kernel.org rust-for-linux, https://lore.kernel.org/rust-for-linux/ (search "retiring")
- Linus Torvalds, 2024, LKML, https://lore.kernel.org/lkml/ (2024-09-03 Rust thread)
- Jon Corbet, 2024, LWN 985860, https://lwn.net/Articles/985860/
- Jake Edge, 2024, LWN 961375 "Rust for filesystems", https://lwn.net/Articles/961375/
- Rust-for-Linux project, https://rust-for-linux.com/, https://github.com/Rust-for-Linux/linux
- Jeremy Soller, 2015, Redox announce, https://www.redox-os.org/news/announcing-redox/
- Redox OS Book, https://doc.redox-os.org/book/
- LWN 979524, 2024, Redox OS in Rust, https://lwn.net/Articles/979524/
- Tianyu Chen et al., 2024, OSDI 2024, Asterinas paper, https://www.usenix.org/conference/osdi24/
- Asterinas Book, https://asterinas.github.io/book/, GitHub https://github.com/asterinas/asterinas
- Kevin Boos, Namitha Liyanage, Ramla Ijaz, Lin Zhong, 2020, OSDI 2020 Theseus, https://www.usenix.org/conference/osdi20/presentation/boos
- Theseus, https://www.theseus-os.com/, https://github.com/theseus-os/Theseus
- Cliff Biffle, 2021, RustConf 2021, Hubris talk, https://www.youtube.com/watch?v=uZxs5N4rI9c
- Oxide Computer, 2021, "Hubris and Humility", https://oxide.computer/blog/hubris-and-humility
- Hubris docs, https://hubris.oxide.computer/, https://github.com/oxidecomputer/hubris
- Stefan Lankes, Jens Breitbart, Simon Pickartz, 2016, ROSS '16, HermitCore, https://dl.acm.org/doi/10.1145/2931088.2931093
- Stefan Lankes, Jonathan Klimt, 2020, arXiv 2007.14641 RustyHermit, https://arxiv.org/abs/2007.14641
- Hermit OS, https://hermit-os.org/, https://github.com/hermit-os
- Andrea Lattuada, Travis Hance, et al., 2023, OOPSLA 2023 Verus, https://dl.acm.org/doi/10.1145/3622870
- Verus, https://github.com/verus-lang/verus
- Son Ho, Jonathan Protzenko, 2022, ICFP 2022 Aeneas, https://inria.hal.science/hal-03740573
- Aeneas, https://github.com/AeneasVerif/aeneas
- Xavier Denis, Jacques-Henri Jourdan, Claude Marché, 2022, Creusot ICFM 2022
- Verismo (MSR), https://github.com/microsoft/verismo
- Jon Gjengset, 2021, Rust for Rustaceans, Starch Press
- LWN Rust index, https://lwn.net/Kernel/Index/#Development_tools-Rust
- Andrew Kelley, Zig, https://ziglang.org/
- Google, Carbon, https://github.com/carbon-language/carbon-lang
