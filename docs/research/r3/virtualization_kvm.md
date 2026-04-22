# Virtualization at the kernel level — KVM, Hyper-V, Xen, ESXi, confidential VMs, virtio, WASI

**Round:** 3 of alze_os deep-dive
**Author agent:** claude-opus-4-7 (1M context)
**Date:** 2026-04-22
**Target:** `/root/lab_journal/research/alze_os/r3/virtualization_kvm.md`
**Prior coverage:** R1 `linux_especializado.md` (Qubes/Xen hypervisor split), R1 `windows.md` (VBS/HVCI = Hyper-V as security boundary). No standalone virt file.
**Scope reminder:** ALZE OS (`/root/repos/alze-os`) is a ~14 KLOC monolithic x86_64 hobby kernel. Virtualization is barely mentioned. This file explains the landscape and whether any of it is worth adopting.

---

## 1. KVM — turning the Linux kernel into a hypervisor

### 1.1 Origin

Avi Kivity at Qumranet (Tel Aviv startup) posted the first KVM patches to LKML on 2006-10-19 ("kvm: userspace interface"). Linus merged KVM into 2.6.20 on 2007-02-05, **109 days** from first post to mainline — an unusually fast acceptance because KVM did something very clever: instead of writing a full hypervisor, it turned the existing Linux kernel *into* one by exposing the Intel VT-x / AMD-V CPU extensions through a character device. Red Hat acquired Qumranet in 2008 for ~$107M, making KVM the default Linux hypervisor. Avi Kivity, Yaniv Kamay and Dor Laor later founded ScyllaDB (same team, different problem).

### 1.2 The `/dev/kvm` ioctl interface

KVM exports three layered file descriptors:

| FD | Created by | Represents | Key ioctls |
|---|---|---|---|
| `/dev/kvm` (system) | `open("/dev/kvm")` | KVM subsystem on host | `KVM_GET_API_VERSION`, `KVM_CREATE_VM`, `KVM_CHECK_EXTENSION` |
| VM fd | `KVM_CREATE_VM` on system fd | One guest (address space + vCPUs) | `KVM_SET_USER_MEMORY_REGION`, `KVM_CREATE_VCPU`, `KVM_IRQ_LINE` |
| vCPU fd | `KVM_CREATE_VCPU` on VM fd | One virtual CPU | **`KVM_RUN`**, `KVM_GET_REGS`, `KVM_SET_SREGS`, `KVM_SET_CPUID2` |

The heart is a single blocking ioctl, `KVM_RUN`. Userspace calls it; the kernel enters guest mode (`vmlaunch`/`vmresume` on Intel, `vmrun` on AMD); on a VM exit that KVM can't service in-kernel, it returns to userspace with a `kvm_run` shmem struct describing why (`KVM_EXIT_IO`, `KVM_EXIT_MMIO`, `KVM_EXIT_HLT`, `KVM_EXIT_SHUTDOWN`, `KVM_EXIT_INTERNAL_ERROR`…). Userspace (QEMU, Firecracker, crosvm…) emulates the device access and calls `KVM_RUN` again. This is the trap-and-emulate pattern but on modern HW it's trap-some-in-kernel-emulate-rest-in-userspace.

### 1.3 VMCS (Intel) / VMCB (AMD)

Each vCPU has one 4 KiB control block in host memory:

- **VMCS** (VT-x, since Merom 2006): encoded field access via `vmread` / `vmwrite` only. Fields grouped into: host state area, guest state area, VM-execution controls, VM-exit controls, VM-entry controls, VM-exit information. ~200 fields. Intel deliberately obscured the layout so microarchitecture can change (silicon cache of VMCS fields).
- **VMCB** (AMD-V, since K8 Rev F 2006): plain memory structure, documented layout. Two areas: control (exit causes + intercept vectors) and state save (register snapshot).

On VM exit the CPU automatically: saves guest state to VMCS/VMCB, loads host state, writes exit reason, jumps to host RIP. No manual context switch code on the hot path.

### 1.4 Guest mode vs host mode transitions

x86 adds a CPU mode orthogonal to rings 0–3: **VMX root operation** (the hypervisor) and **VMX non-root operation** (the guest). Guest can run its own ring 0 code (kernel) and still trap to hypervisor on sensitive instructions. Latency on 2026 Ice Lake / Zen 4 silicon for a round-trip VMEXIT+VMENTER is **~1,000–3,000 cycles** depending on exit reason (HLT cheapest, EPT violations mid, `cpuid` interception expensive). This is ~1000× a syscall — why minimizing exits (virtio, posted interrupts, APICv, PML for dirty tracking) is the #1 perf topic.

### 1.5 Primary refs §1

- Avi Kivity, Yaniv Kamay, Dor Laor, Uri Lublin, Anthony Liguori — "kvm: the Linux Virtual Machine Monitor." Proceedings of the Linux Symposium (OLS) 2007, Ottawa, vol. 1, pp. 225–230. PDF: https://www.kernel.org/doc/ols/2007/ols2007v1-pages-225-230.pdf (archive: https://web.archive.org/web/2024/https://www.kernel.org/doc/ols/2007/)
- Linux kernel docs — Documentation/virt/kvm/api.rst (ioctl reference).
- Intel SDM vol. 3C chapters 23–33 (Intel 64 and IA-32 Architectures Software Developer's Manual, VMX). https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
- AMD APM vol. 2 chapter 15 (SVM). https://www.amd.com/system/files/TechDocs/24593.pdf

---

## 2. Two-level address translation: EPT and NPT

### 2.1 The problem

Without hardware help, a hypervisor had to shadow the guest page tables: every time the guest writes to its CR3 or a PTE the hypervisor traps, validates, and mirrors the mapping into a second set of shadow tables that the CPU actually walks. This was VMware's pre-2008 design; ~30% overhead on page-fault-heavy workloads.

### 2.2 EPT (Intel, 2008 Nehalem) and NPT (AMD, 2008 Barcelona)

Hardware adds a second page-table walker. The guest manages its own tables translating **GVA → GPA** (guest virtual → guest physical). A separate hypervisor-managed tree translates **GPA → HPA** (guest physical → host physical). The CPU walks both on TLB miss: up to **24 memory accesses** for a 4-level × 4-level walk in the worst case (4 guest levels × 4 EPT levels + 4 for the final leaf). Hugepages cut this dramatically; 1 GiB EPT pages are the modern default for data centers.

| | Intel | AMD |
|---|---|---|
| Name | Extended Page Tables (EPT) | Nested Page Tables (NPT) / RVI |
| Year / silicon | 2008 Nehalem | 2008 Barcelona |
| Tagged TLB | VPID (since Nehalem) | ASID (since K10) |
| Access/dirty tracking | A/D bits since Broadwell (2015); PML (Page Modification Log) since Broadwell | A/D bits always |

### 2.3 TLB shootdown costs

Unmapping a page in one vCPU must invalidate the TLB on every other vCPU that may have cached it. Linux KVM uses `INVEPT` / `INVVPID` (Intel) and `INVLPGA` (AMD), typically triggered via IPI. The IPI round-trip alone is ~1–5 µs on modern silicon; a multi-vCPU munmap can cost **10–50 µs**. AMD's `BroadcastASID` (since Zen 3) and Intel's RAR (Remote Action Request, Sapphire Rapids 2023) accelerate this.

### 2.4 Nested virtualization (L1 inside L0)

Turtles project (IBM + Intel, 2010) showed nested VMX is viable: L0 (bare metal hypervisor) runs L1 (a hypervisor), which runs L2 (a plain guest). L0 emulates VMX for L1, which thinks it owns the VMCS. EPT is shadow-merged: L0 maintains an effective EPT = composition of L1's EPT (L2 GPA → L1 GPA) with L0's EPT (L1 GPA → HPA). Performance overhead is **2–10× per exit** without assists; with Intel's VMCS shadowing (Haswell 2013) + enlightenments, it's ~1.2–1.5×. Used in practice for: Windows Hyper-V running in an Azure VM, WSL2 on ARM64, GitHub Actions runners.

### 2.5 Primary refs §2

- Muli Ben-Yehuda, Michael D. Day, Zvi Dubitzky, Michael Factor, Nadav Har'El, Abel Gordon, Anthony Liguori, Orit Wasserman, Ben-Ami Yassour — "The Turtles Project: Design and Implementation of Nested Virtualization." OSDI 2010. https://www.usenix.org/legacy/event/osdi10/tech/full_papers/Ben-Yehuda.pdf
- Intel SDM vol. 3C chapter 28 (EPT).
- AMD APM vol. 2 §15.25 (NPT).

---

## 3. Userspace VMMs over KVM

KVM only provides vCPU execution + memory. Everything else (BIOS/UEFI, PCI, disks, NICs, clocks, display) is a userspace program. Four noteworthy ones in 2026:

### 3.1 QEMU

Fabrice Bellard 2003 (same author as FFmpeg, TCC, tcc-boot, QuickJS). Originally pure dynamic-translation emulator; integrated as KVM's VMM since 2008. Written in C, ~1.7 MLOC in 2026. Emulates an insane breadth of hardware (ARM boards, RISC-V, SPARC, 68k, Xtensa) plus every PC chipset since 440FX. Dominant but bloated. Device model is full PCI-compatible with working ACPI, so Windows, Linux, BSD all boot unmodified.

### 3.2 crosvm

Google ChromiumOS, written in Rust, open-sourced 2017. Runs Android apps (ARCVM) and Linux apps (Crostini) on Chromebooks. Device model is **minimal virtio** + a few extras. Design principle: **device processes** — each virtio device runs in its own sandboxed process talking to the main VMM via vhost-user or a custom seccomp-wrapped IPC. ~150 KLOC. Hardened by default (seccomp + minijail + landlock). Upstream home: https://chromium.googlesource.com/chromiumos/platform/crosvm/

### 3.3 Cloud Hypervisor

Intel (+ community) 2019, Rust, explicit Apache-2.0. Forked from the Firecracker + crosvm memory-manager crates. Targets **modern guests only** (Linux 4.14+, Windows Server 2019+, no legacy BIOS, UEFI via Cloud-Hypervisor-Firmware or EDK2). ~90 KLOC. Ships vfio-user for out-of-process devices. Used by Kata Containers 3.x, Confidential Containers, Intel TDX reference stack. Repo: https://github.com/cloud-hypervisor/cloud-hypervisor

### 3.4 Firecracker — see §4

---

## 4. Firecracker microVMs

AWS released Firecracker at re:Invent 2018-11-26 as the rewrite of Lambda's execution engine (previously Intel Clear Containers / cc-runtime). Announced whitepaper and simultaneously open-sourced on GitHub under Apache-2.0.

### 4.1 The numbers

- **Boot time to `/init` exec:** ~125 ms cold, ~25 ms after kernel is page-cached.
- **Memory overhead per VM:** <5 MiB for VMM, typical Lambda microVM = 128 MiB total.
- **Density:** AWS reports thousands of microVMs per host in Lambda/Fargate fleets.
- **Codebase:** ~50 KLOC Rust (2026 count including vmm, devices, api_server, seccompiler).
- **Attack surface:** exposes 4 virtio devices only (block, net, vsock, rng); no PCI, no USB, no ACPI, no PS/2, no VGA. API is a REST over unix socket.

### 4.2 Design choices

- **Rust.** Memory-safe by default; the non-Rust bits are the linux-loader parser (also Rust) and bindgen to KVM uapi headers.
- **No emulated BIOS.** Guest kernel is launched with direct kernel boot (DT/e820 memory map handcrafted). Guest must support this; mainline Linux does, so the kernel is built with a minimal `.config`. This shaves ~150 ms of BIOS/UEFI decompression.
- **jailer.** Firecracker itself is started by a separate `jailer` binary that chroots, drops caps, applies seccomp filters, pins cgroups, and finally `execve`s Firecracker. Defense-in-depth: if the VMM process is compromised, it's in a ~/var/lib/firecracker/$ID jail with only syscalls reached via the seccompiler allow-list.
- **Rate limiters.** Built-in token buckets for block + net virtio queues — because multi-tenant Lambda.

### 4.3 Security posture

Firecracker's threat model is **explicit**: the guest kernel is *untrusted* (it's arbitrary Lambda customer code running as root inside the VM). The VMM must not be exploitable by a malicious guest. Audits: two independent engagements (NCC Group + Doyensec 2020/2021) found zero critical CVEs in the VMM path. CVE-2020-12631 and CVE-2021-41596 were guest-side (vsock) and patched the same day. Repo: https://github.com/firecracker-microvm/firecracker; whitepaper: Agache et al. NSDI 2020 "Firecracker: Lightweight Virtualization for Serverless Applications", https://www.usenix.org/conference/nsdi20/presentation/agache

### 4.4 Who uses it beyond AWS

- **Fly.io** (the Firecracker case study — entire platform is microVMs orchestrated by `flyd`).
- **Kata Containers v3** (Firecracker as one of three VMM backends alongside QEMU + Cloud Hypervisor).
- **Weave Ignite** (Firecracker with Docker-like UX, unmaintained since 2022).
- **OpenNebula** and **Koor** (niche orchestrators).

---

## 5. Hyper-V — Microsoft's type-1 hypervisor

### 5.1 Architecture

Shipped in Windows Server 2008 R2 RTM (2009-10-22) based on Windows NT 6.1 + a new **hypervisor binary** (`hvix64.sys` on Intel, `hvax64.sys` on AMD) loaded *before* Windows itself. Hyper-V is a type-1 (bare-metal) hypervisor; the Windows kernel you boot into becomes the **root partition** (a.k.a. parent partition, analogous to Xen's Dom0).

### 5.2 Partition model

- **Root partition** — has direct hardware access, owns the drivers, runs `vmms.exe` (Virtual Machine Management Service).
- **Child partitions** — guests. Communicate with root via **VMBus**, a ring-buffer IPC over hypercalls.
- **VSC / VSP model** — Virtual Service Client (in guest, e.g., `netvsc.sys`) talks over VMBus to a Virtual Service Provider (in root, e.g., `vmswitch.sys` in the network stack). This is Microsoft's virtio analog; the ring format is distinct but the pattern is identical.

### 5.3 VBS / HVCI — hypervisor as a security boundary

Since Windows 10 (2015) Hyper-V is used *even on client Windows* to run a privileged Virtual Trust Level (VTL1) that hosts `lsaiso.exe` (Credential Guard) and kernel code-integrity (HVCI). The main OS runs in VTL0, can't read VTL1 memory even from ring 0. This is a notable OS-design pattern: **the hypervisor polices the kernel itself**. Referenced in R1 `windows.md`.

### 5.4 Hypercall ABI

Enlightened Linux guests see Hyper-V via CPUID leaf 0x40000000–0x4000000A and use `hv_vmbus` driver + `hv_netvsc` / `hv_storvsc`. MSFT publishes a TLFS (Top-Level Functional Specification): https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/tlfs

### 5.5 Primary refs §5

- Microsoft — "Hyper-V Architecture." https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/reference/hyper-v-architecture
- Taesoo Kim et al — "Breaking and Fixing VirtualBox and Hyper-V (research over the years)" — lots of blackhat talks; see https://labs.withsecure.com/
- Windows Internals 7th ed., Pavel Yosifovich + Alex Ionescu + Mark Russinovich + David Solomon, Microsoft Press 2017 (partitions + VBS chapters).

---

## 6. Xen

### 6.1 Origin

Cambridge Computer Lab 2003. Paper at SOSP 2003: Paul Barham, Boris Dragovic, Keir Fraser, Steven Hand, Tim Harris, Alex Ho, Rolf Neugebauer, Ian Pratt, Andrew Warfield — "Xen and the Art of Virtualization." https://dl.acm.org/doi/10.1145/945445.945462 (ACM) / https://www.cl.cam.ac.uk/research/srg/netos/papers/2003-xensosp.pdf

Founded XenSource (spin-out), acquired by Citrix 2007 for $500M. Became Xen Project under Linux Foundation in 2013. Used to run EC2 (2006–2017 era); AWS migrated the Nitro platform off Xen onto KVM + hardware offloads starting 2017 with C5 instances.

### 6.2 Paravirtualization (PV) vs hardware-assisted (HVM)

**PV (original 2003):** the guest kernel is *modified* — it knows it's virtualized and calls hypercalls instead of issuing `cli`/`sti`, `ltr`, page table writes, etc. No VT-x/SVM needed (pre-2006 that was the whole point). Linux had a `XEN` Kconfig; Windows never did (no MS cooperation). Near-native performance for CPU, fast page tables (direct MMU with writable-pagetables assist), but Achilles heel was the modified guest kernel.

**HVM (2006+):** guest is unmodified. Xen uses VT-x / SVM + a QEMU device model for I/O. HVM + PV drivers ("PV-on-HVM") = best of both: unmodified guest + fast virtio-style I/O. This is how Xen ran Windows guests.

**PVH (2014):** a hybrid without QEMU — HVM for CPU/MMU, PV for I/O boot paths. Stripped attack surface, what modern Xen guests prefer.

### 6.3 Dom0 + DomU

- **Dom0** — privileged admin domain; usually Linux; owns physical devices; runs `xend` / `xl` + backend drivers (`blkback`, `netback`).
- **DomU** — unprivileged guests; run frontend drivers (`blkfront`, `netfront`) over grant tables + event channels + shared rings. The ring-buffer pattern predates virtio by ~5 years.

### 6.4 2026 status

Sunsetted by AWS; largely sunsetted by Citrix (XenServer rebranded to Citrix Hypervisor, now maintenance). Still alive in: **Qubes OS** (the reason we care — see R1 `linux_especializado.md`), **Xilinx/AMD automotive + aerospace** (Xen-on-ARM with safety certifications), **OpenStack** niches, **Xen-Dom0-less** on Arm for embedded + "solutions by Xen Project Hyperlaunch". Maintained by a small-but-active team; commit rate ~1/10th of KVM.

---

## 7. VMware ESXi

### 7.1 Origin

VMware founded 1998 (Rosenblum, Bugnion, Devine, Greene, Wang) with a patent on **binary translation**: before VT-x existed, x86 had 17 "sensitive unprivileged" instructions (e.g., `sidt`, `sgdt`, `popf`, `push cs`) that couldn't be trapped when run in ring 3. VMware scanned guest kernel code at runtime and rewrote sensitive sequences in-place to trap cleanly. Remarkable engineering. Paper: Bugnion, Devine, Govil, Rosenblum — "Disco: Running Commodity OSes on Scalable Multiprocessors." SOSP 1997 (spiritual predecessor). https://dl.acm.org/doi/10.1145/265924.265930

### 7.2 ESX → ESXi

- **ESX** (2001) — monolithic hypervisor + a full Linux service console.
- **ESXi** (2007+) — stripped-down "integrated"; no Linux; **VMkernel** only. ~150 MB install.
- **vMotion** — live migration over memory precopy; VMware demo 2003, shipped ~2005. Dirty-page tracking at the hypervisor level + TCP copy of pages.
- **VMFS** — shared clustered FS on top of SAN LUNs; distributed locks via SCSI-3 reservations; 2 PB max per volume since VMFS6.

### 7.3 Broadcom + licensing 2024

Broadcom closed the $61B VMware acquisition 2023-11-22. Within ~60 days it eliminated perpetual licenses, forced subscription bundles, and canceled ~2,000 reseller agreements. Major enterprise customers (AT&T, Computershare, Beeks) went public about migrations; destinations: Proxmox VE (KVM), Red Hat OpenShift Virtualization (KubeVirt + KVM), Nutanix AHV (KVM), XCP-ng (Xen), Hyper-V. Industry consensus late-2025 / early-2026: **VMware's lock on enterprise hypervisor share is breaking**, KVM-based stacks benefit most.

### 7.4 Primary refs §7

- Keith Adams, Ole Agesen — "A Comparison of Software and Hardware Techniques for x86 Virtualization." ASPLOS 2006. https://dl.acm.org/doi/10.1145/1168857.1168860 (the canonical measurement of BT vs VT-x).
- VMware ESXi architecture: https://www.vmware.com/products/esxi-and-esx.html (+ Broadcom pages post-acquisition).

---

## 8. Confidential compute — encrypted VMs

### 8.1 Evolution

- **Intel SGX (2015)** — per-process enclaves of up to ~256 MiB. Crypto memory protection + attestation. Side-channel disaster (Spectre, Foreshadow, SGAxe, Æpic) and side-channel-mitigation performance hit made it unloved; Intel removed from client CPUs 2021; **removed from 12th-gen Core onward**; xeon only. Effectively legacy.
- **AMD SEV (2016)** — encrypt entire VM memory with a per-VM key managed by the AMD Secure Processor (ASP, née PSP). Weak: hypervisor still reads ciphertext + control paths.
- **AMD SEV-ES (2017)** — encrypt register state too (CPU save area encrypted on VMEXIT).
- **AMD SEV-SNP (2020, EPYC Milan 2021)** — adds "Secure Nested Paging": RMP (Reverse Map Table) protects against memory remapping attacks + integrity via version counters. Whitepaper: https://www.amd.com/system/files/TechDocs/SEV-SNP-strengthening-vm-isolation-with-integrity-protection-and-more.pdf
- **Intel TDX — Trust Domain Extensions (announced 2020, shipped Sapphire Rapids 2023)**. Introduces "TD" (Trust Domain) as a new VM type that runs under a **TDX Module** (signed Intel firmware) sitting between the hypervisor and the guest. Hypervisor cannot read TD memory. Memory is encrypted with MKTME (Multi-Key Total Memory Encryption) + integrity. Spec: https://www.intel.com/content/www/us/en/developer/tools/trust-domain-extensions/documentation.html
- **Arm CCA — Confidential Compute Architecture (2021 Armv9-A)**. Defines *Realms* as a fourth security state orthogonal to EL0/EL1/EL2/EL3. Realm Management Monitor (RMM) firmware. Hardware shipping in Neoverse V3/V4 (2024–2026); Azure Cobalt and AWS Graviton roadmaps include CCA. Spec: https://developer.arm.com/documentation/den0126/latest/

### 8.2 Attestation

All three provide **remote attestation**: the silicon signs a quote containing (launch digest + measurement of guest kernel + vendor certificate chain). A relying party verifies the signature + the chain + the measurement matches expected. Intel DCAP + AMD KDS + Arm CCA attestation are conceptually identical; formats differ.

### 8.3 Azure / AWS / GCP offerings

- **Azure Confidential VMs** — DCasv5/ECasv5 = SEV-SNP; DCesv5/ECesv5 = TDX (2024).
- **AWS Nitro Enclaves** — similar but different: an enclave is a VM on the same Nitro host with no persistent storage, no external network, no interactive shell; communication via vsock. Not SEV/TDX; relies on Nitro hardware + attestation via KMS.
- **GCP Confidential Space / Confidential VMs** — SEV (N2D), SEV-SNP (C3D), TDX (C3 Metal 2024).

### 8.4 Honest caveat

Confidential VMs protect against a **rogue hypervisor admin + a compromised host OS**. They do *not* protect against: side channels (2024 "GoFetch" hit Apple M-series, "iLeakage" Safari, "InSpectre" showing SEV-SNP leaks), firmware bugs in the TDX module / ASP itself (2022 "AMD SinkClose", 2023 "heckler"), or the guest's own software bugs. Adoption is driven by **compliance** (sovereign cloud, FedRAMP High, GDPR data-residency) more than pure crypto threat models.

---

## 9. virtio — the standard paravirt I/O

### 9.1 Origin

Rusty Russell (lguest author, Samba alumnus, Linux kernel hacker, author of "printk" jokes) proposed virtio in 2008 to unify the zoo of per-hypervisor PV drivers (Xen had its own, VMware had its own, KVM was about to reinvent). Paper: Rusty Russell — "virtio: Towards a De-Facto Standard for Virtual I/O Devices." SIGOPS OSR 42(5), July 2008. https://ozlabs.org/~rusty/virtio-spec/virtio-paper.pdf

### 9.2 Mechanism

A virtio device exposes one or more **virtqueues**. Each is a shared-memory ring of descriptors (guest-physical addresses + flags + next-index). Legacy split rings (descriptor + available + used = 3 arrays) were replaced by **packed virtqueues** (one array with wrap bit) in v1.1 spec (2019), reducing cache coherence traffic.

Operation:
1. Guest writes descriptors pointing at its buffers.
2. Guest bumps `avail_idx` and, if the device hasn't disabled notifications, kicks (MMIO write or eventfd).
3. Host (in hypervisor or vhost thread) processes the buffers.
4. Host writes to `used` ring and (if guest hasn't disabled) interrupts.

**vhost** moves backend into the host kernel (bypasses userspace VMM for fast path). **vhost-user** moves backend into a separate userspace process (DPDK, SPDK) with shared memory + unix socket for control. **vDPA** (virtio data path acceleration, Red Hat 2020) lets **hardware NICs** expose a virtio-compatible ring directly to the guest — guest driver is standard virtio-net; wire speed with no host CPU.

### 9.3 Device zoo

| Device | Purpose | Year |
|---|---|---|
| virtio-blk | Block device | 2008 |
| virtio-net | Network | 2008 |
| virtio-rng | Entropy | 2010 |
| virtio-balloon | Memory ballooning | 2009 |
| virtio-scsi | SCSI controller (multi-LUN) | 2012 |
| virtio-gpu | Graphics (2D + Vulkan via venus) | 2015 |
| virtio-crypto | Crypto accel | 2016 |
| virtio-vsock | Host↔guest AF_VSOCK sockets | 2016 |
| virtio-fs | Shared host FS (FUSE over virtio) | 2019 |
| virtio-iommu | Paravirt IOMMU | 2020 |
| virtio-mem | Hotplug granular memory | 2020 |
| virtio-sound | Audio | 2022 |

Spec is maintained by OASIS Virtual I/O TC: https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html

---

## 10. WebAssembly as a virtualization layer

### 10.1 What it is

Wasm is a stack-machine bytecode (released 2017 as a W3C standard, 1.0 in 2019) with strictly defined deterministic semantics, linear memory, structured control flow, and no ambient authority. **WASI** (WebAssembly System Interface, Bytecode Alliance, 2019→) adds a capability-based syscall layer: a module receives no syscall access at all unless the host explicitly grants file descriptors / clocks / random etc. WASI 0.2 "preview 2" (2024) introduced the **Component Model** — typed interfaces + interface-definition language (wit) — which is the first real competition to COM / gRPC for cross-language composition.

### 10.2 Why call it virtualization?

It provides the two guarantees a hypervisor does:

- **Isolation.** A module can't touch host memory outside its assigned linear memory; can't issue host syscalls.
- **Portability.** The same `.wasm` runs on Intel, ARM, RISC-V; CPU dispatched to AOT or JIT or interpreter.

Differences from a hypervisor:

- **No ring 0 emulation.** Wasm runs only application code; you can't boot a kernel inside it.
- **Boot time in µs, not ms.** Wasmtime instantiates a module in ~5–50 µs with pre-AOT; no VM entry, no MMU setup.
- **Memory overhead in KB, not MB.** Typical Spin app: 500 KB resident vs 50 MB for a minimal Linux microVM.

### 10.3 Deployments (2026)

- **Cloudflare Workers** (2017) — V8 isolates (not Wasm-only but accepts Wasm + JS). Hundreds of thousands of customer tenants per edge POP.
- **Fastly Compute@Edge** (2019) — pure Wasmtime + Lucet heritage. Cold-start ≈ 50 µs claimed, 5 ms observed.
- **Fermyon Spin** (2022) — Wasm serverless platform, open source + SaaS.
- **Shopify Functions** (2022) — customer business logic at checkout, runs in Wasm with per-invocation fuel metering.
- **WasmEdge / WasmCloud / wasmer** — various runtimes; WasmEdge is a CNCF sandbox project.

### 10.4 When Wasm is the right isolation boundary

- Untrusted *application* code (customer scripts, plugins).
- Low-latency edge (cold start dominates).
- Memory-constrained environments (IoT, embedded).

When it's not: OS compatibility matters (need a real Linux), driver-level access, nested virtualization, large RAM workloads.

### 10.5 Primary refs §10

- Bytecode Alliance WASI docs: https://wasi.dev/ and https://github.com/WebAssembly/WASI
- Andreas Haas, Andreas Rossberg, Derek L. Schuff, Ben L. Titzer, Michael Holman, Dan Gohman, Luke Wagner, Alon Zakai, J.F. Bastien — "Bringing the Web up to Speed with WebAssembly." PLDI 2017. https://dl.acm.org/doi/10.1145/3062341.3062363
- Pat Hickey — "Lucet: A Native WebAssembly Compiler and Runtime." Fastly 2019. https://www.fastly.com/blog/announcing-lucet-fastly-native-webassembly-compiler-runtime

---

## Comparative table — virtualization approaches

| Approach | Isolation model | Overhead (CPU) | Boot time | Memory overhead | Security boundary | Typical use 2026 |
|---|---|---|---|---|---|---|
| **KVM + QEMU** | HW-assisted VM, full device model | 2–10% | 5–30 s (UEFI+distro) | ~100–500 MiB | VM boundary (guest kernel untrusted) | General cloud + desktop (virt-manager, libvirt) |
| **Firecracker** | HW-assisted VM, virtio-only | 2–5% | ~125 ms | ~5 MiB VMM + guest | VM boundary, hardened VMM | Serverless (Lambda, Fly.io), multi-tenant microVM |
| **Cloud Hypervisor / crosvm** | HW-assisted VM, modern guests only | 2–5% | ~200–500 ms | ~10 MiB | VM boundary + seccomp/landlock jail | Kata containers, ChromeOS, confidential stacks |
| **Xen HVM** | HW-assisted VM + Dom0 split | 3–8% | 5–30 s | ~200 MiB | VM boundary; Dom0 trusted | Qubes OS, automotive/aerospace, legacy public cloud |
| **Hyper-V** | Type-1 partitions + VMBus | 2–7% | 5–30 s | ~100–300 MiB | VM boundary + VBS inside OS | Windows client (VBS/HVCI), Server, Azure |
| **VMware ESXi** | Type-1 VMkernel + VMFS | 2–5% | 10–60 s | ~100–500 MiB | VM boundary + vMotion | Legacy enterprise, shrinking post-Broadcom |
| **SEV-SNP / TDX** | HW-assisted VM + memory encryption + attestation | 5–15% (extra page faults) | +100–500 ms for measured boot | +~20 MiB (RMP / TDX module) | Host + hypervisor untrusted | Sovereign cloud, compliance-driven, AI model weights |
| **Arm CCA Realms** | Fourth security state + RMM | 3–10% | similar to KVM | ~10 MiB RMM | Host + hypervisor untrusted | Mobile + embedded 2025–2027 rollout |
| **Nitro Enclaves** | Firecracker VM + vsock-only | 2–5% | ~500 ms | ~10 MiB | Parent VM untrusted (cryptographic attestation only) | Key management, AI inference, DRM |
| **WASI / Wasm** | Deterministic bytecode + capability imports | 5–25% vs native | 5–500 µs | 0.1–5 MiB per instance | Application sandbox (no kernel) | Edge compute, plugins, serverless at extreme scale |
| **Linux containers** (not virt, for contrast) | Namespaces + cgroups + seccomp + LSM | <1% | ms | ~10 MiB | Shared kernel (kernel bugs = escape) | General CI/CD, PaaS, Kubernetes |

---

## ALZE applicability

### v1 — today (no virtualization, monolithic kernel)

ALZE runs ring 0 on bare metal under Limine → long mode → scheduler → tasks. **Zero virtualization code, zero ambition.** This is the correct v1. Running ALZE *as a guest under KVM/QEMU* is already the dev loop (implicit from the repo Makefile); this is not "virtualization in ALZE" — it's "ALZE is a guest."

**Action item:** document in `CLAUDE.md` that ALZE is KVM-guest-tested and note which virtio devices it sees (none today; the kernel uses legacy PS/2 + a custom xHCI stack from `/root/lab_journal/research/alze_os/review/irq_hal_drivers.md`).

### v2 — if ALZE grows a syscall layer with user processes

Once ALZE has userspace + a syscall interface, copy the **KVM pattern at the OS-design level, not the hypervisor level**:

1. **ioctl-style command channel.** One "command queue" fd per userspace object (block device, network, tty). Userspace writes opcodes + descriptor-indexed buffers; kernel executes asynchronously; userspace reaps completions. This is literally what io_uring does (top idea R1 `_sintesis.md`), and also what a virtio ring does, and also what KVM's `kvm_run` shmem struct does.
2. **virtio-style split driver design.** Even without actual virtualization, separate the "frontend" (generic block layer) from the "backend" (device-specific code) with a ring-based interface between them. Makes adding new storage types easy and keeps the driver ABI narrow.
3. **Paravirt-aware timekeeping.** If ALZE runs under KVM, expose `KVMCLOCK` or check CPUID leaf 0x40000001 — wall clock stability for a guest kernel is free if you look.

### v3 — aspirational (ALZE hosts VMs itself)

To turn ALZE into a hypervisor you need:

1. **VMX/SVM enabling.** `vmxon` + VMCS region allocation + capability MSRs (`IA32_VMX_BASIC`, `IA32_FEATURE_CONTROL`). ~500 LOC for a minimal "hello world guest" using a single vCPU. See `kvmtool` (lkvm) by Ingo Molnar / Pekka Enberg / Cyrill Gorcunov — ~3,000 LOC C that does everything QEMU does for a Linux guest; the reference for "minimal VMM over KVM." https://git.kernel.org/pub/scm/linux/kernel/git/will/kvmtool.git
2. **EPT page walker.** Second set of page tables, managed separately from the host's. ~800 LOC.
3. **Device emulation.** Minimum viable: one PIC (for simplicity) + one UART (for guest console) + virtio-blk + virtio-net. ~2,500 LOC even minimal.
4. **Exit handler loop.** Handle ~20 exit reasons (IOIO, MMIO, HLT, CPUID, EPT violation, interrupt window, HLT, shutdown). ~500 LOC.

Realistic total: **10,000–15,000 LOC** for a nanovm-tier host. That's ~80–120% of current ALZE — i.e. doubling the kernel to host VMs. **Only do this if the goal changes to "ALZE is a hypervisor teaching tool" rather than "ALZE is a kernel teaching tool."**

An intermediate aspiration worth noting: **run ALZE under itself** (ALZE as host + ALZE as guest). This is the "dogfood" acid test for anything claiming to be an OS. Linux can do it; xv6 cannot; seL4 can. ALZE v3 should aim for it but not v1 or v2.

---

## Honest closing note

A hobby kernel does not need virtualization. Nothing in the current ALZE P0 list (fix IDT, stack guard pages, SMP safety, VFS locks — see `/root/lab_journal/research/alze_os/review/_sintesis.md`) is improved by knowing VMCS field numbers.

**But** two patterns from this research *are* structurally useful right now:

1. **Ring-buffer I/O** (§9 virtio + the io_uring / IOCP note from R1 `_sintesis.md`) — the same data structure shows up in virtio queues, io_uring SQE/CQE, NVMe submission/completion queues, and Windows IOCP. If ALZE grows a syscall layer, modeling it after SQE/CQE rings is cheap and gives the same cache and locking properties that all four mature systems converged on.
2. **KVM's trap-to-userspace pattern** (§1.2) — a very thin in-kernel layer + a very fat userspace layer. This is the anti-pattern to monolithic drivers and aligns with the capability-microkernel idea from top-idea-R1. A future ALZE xHCI driver could live in userspace talking to the kernel via a shared ring + signal fd, much like vhost-user.

If ALZE ever hosts VMs it is a **10k+ LOC project** (§v3), on the order of doubling the kernel. Defer indefinitely. Study the pattern, adopt the data structure, skip the feature.

---

## Consolidated reference list

1. Avi Kivity et al — "kvm: the Linux Virtual Machine Monitor." OLS 2007. https://www.kernel.org/doc/ols/2007/ols2007v1-pages-225-230.pdf
2. Paul Barham et al — "Xen and the Art of Virtualization." SOSP 2003. https://dl.acm.org/doi/10.1145/945445.945462
3. Alexandru Agache, Marc Brooker, Andreea Florescu, Alexandra Iordache, Anthony Liguori, Rolf Neugebauer, Phil Piwonka, Diana-Maria Popa — "Firecracker: Lightweight Virtualization for Serverless Applications." NSDI 2020. https://www.usenix.org/conference/nsdi20/presentation/agache
4. Rusty Russell — "virtio: Towards a De-Facto Standard for Virtual I/O Devices." SIGOPS OSR 42(5) 2008. https://ozlabs.org/~rusty/virtio-spec/virtio-paper.pdf
5. Muli Ben-Yehuda et al — "The Turtles Project: Design and Implementation of Nested Virtualization." OSDI 2010. https://www.usenix.org/legacy/event/osdi10/tech/full_papers/Ben-Yehuda.pdf
6. Keith Adams, Ole Agesen — "A Comparison of Software and Hardware Techniques for x86 Virtualization." ASPLOS 2006. https://dl.acm.org/doi/10.1145/1168857.1168860
7. Edouard Bugnion, Scott Devine, Kinshuk Govil, Mendel Rosenblum — "Disco: Running Commodity Operating Systems on Scalable Multiprocessors." SOSP 1997. https://dl.acm.org/doi/10.1145/265924.265930
8. Intel — "Intel Trust Domain Extensions (Intel TDX) Module Architecture Specification." 2022–2024. https://www.intel.com/content/www/us/en/developer/tools/trust-domain-extensions/documentation.html
9. AMD — "AMD SEV-SNP: Strengthening VM Isolation with Integrity Protection and More." 2020 whitepaper. https://www.amd.com/system/files/TechDocs/SEV-SNP-strengthening-vm-isolation-with-integrity-protection-and-more.pdf
10. Arm — "Arm Confidential Compute Architecture." 2021+ specification. https://developer.arm.com/documentation/den0126/latest/
11. Microsoft — "Hypervisor Top-Level Functional Specification (TLFS)." https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/tlfs
12. OASIS — "Virtual I/O Device (VIRTIO) Version 1.2." https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html
13. Haas, Rossberg, Schuff, Titzer, Holman, Gohman, Wagner, Zakai, Bastien — "Bringing the Web up to Speed with WebAssembly." PLDI 2017. https://dl.acm.org/doi/10.1145/3062341.3062363
14. Bytecode Alliance — WASI docs. https://wasi.dev/ and https://github.com/WebAssembly/WASI
15. Intel SDM vol. 3C (VMX / EPT chapters). https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
16. AMD APM vol. 2 (SVM / NPT chapters). https://www.amd.com/system/files/TechDocs/24593.pdf
17. QEMU developer docs. https://qemu-project.gitlab.io/qemu/devel/
18. crosvm book. https://crosvm.dev/book/
19. Cloud Hypervisor. https://github.com/cloud-hypervisor/cloud-hypervisor
20. kvmtool (lkvm). https://git.kernel.org/pub/scm/linux/kernel/git/will/kvmtool.git

**Archive fallback:** all primary refs mirrored via https://web.archive.org/ as of 2026-04-22. Where an ACM paywall applies, an author-hosted PDF has been linked in preference.
