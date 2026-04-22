# Modern kernel security hardening — deep dive for ALZE OS

**Ronda:** R5 / `security_hardening.md`
**Fecha:** 2026-04-22
**Scope:** mitigaciones hardware-assisted + software-enforced del kernel moderno (2013–2026). Cómo Intel/AMD/Arm + LLVM/GCC + Linux han gastado ~20% de presupuesto de performance en "security tax" desde Haswell y Meltdown.
**Repo referencia:** `/root/repos/alze-os` (144 commits, HEAD `6101b72`, 13,931 LOC kernel). Estado actual por R2 `review/tests_security.md`: SSP `-fstack-protector-strong` + canary xorshift re-sembrado desde TSC + `-Werror -Wall -Wextra`. **SIN** SMAP/SMEP activos, **SIN** KASLR, **SIN** KPTI, **SIN** CFI, **SIN** seccomp (no hay userspace aún). Canary es global + TSC-only.

**No duplica:**
- `linux_mainstream.md` §seccomp-bpf (Drewry 2012) y eBPF-seccomp arXiv 2302.10366 — se referencian pero no se reexplican.
- `otros.md` §OpenBSD W^X + pledge/unveil — se citan como filosofía pero el deep-dive es el kernel-level hardening cross-platform.
- `r3/capability_kernels.md` §seL4 infoflow proof — aquí hablamos de defensas *runtime* no de *verificación*.

**Aporta nuevo:** CR4-bit-level playbook de SMAP/SMEP; KPTI page-table layout; KASLR entropy budget y bypasses (EntryBleed 2022); CET shadow stack + IBT opcode `0xF3 0x0F 0x1E`; Arm PAC keys (APIA/APIB/APDA/APDB/APGA) + BTI landing pads; microarchitectural side-channel matriz (Meltdown / Foreshadow-NG / MDS / Zombieload / CacheOut / CROSSTalk / Retbleed / Downfall 2023 / Inception 2023 / GoFetch 2024 Apple); ALZE v1/v2/v3 roadmap con estimate de LOC por feature.

---

## 1. Cronología de la "era del hardening"

| Año | Evento | Impacto |
|-----|--------|---------|
| 2003 | PaX/grsecurity publica KERNEXEC + UDEREF en Linux patched | precursor software de SMEP/SMAP |
| 2005 | Abadi et al CCS *"Control-Flow Integrity"* | paper seminal CFI |
| 2009 | DEP/NX universal en kernels (post-XP SP2) | W^X base |
| 2012 | seccomp-BPF (Will Drewry, Google) merged Linux 3.5 | primera mitigación userspace-triggered |
| 2013 | Intel Haswell envía SMEP | CR4.SMEP bit 20 |
| 2014 | Intel Broadwell envía SMAP | CR4.SMAP bit 21 |
| 2016 | Linux 4.8 KASLR default-on x86_64 | |
| 2017 Jan | Gruss et al *KAISER* (KPTI precursor) Black Hat EU | |
| 2018 Jan | Meltdown + Spectre v1/v2 disclosed | KPTI emergency merged; retpoline compiler patches |
| 2018 Aug | L1TF / Foreshadow | bare-metal + SGX + VMM |
| 2019 May | MDS (Zombieload, RIDL, Fallout) | microcode + MDS_CLEAR verw |
| 2020 Jan | Intel CET (Control-flow Enforcement) whitepaper + Tiger Lake ships | shadow stack + IBT |
| 2020 Jun | CrossTalk (SRBDS) | random-number leak across cores |
| 2021 Jun | Landlock (Mickaël Salaün) merged Linux 5.13 | unprivileged sandbox LSM |
| 2022 Jan | Linux 5.17 `CONFIG_SCHED_CORE` core-scheduling for SMT side-channel | Linux 5.17 |
| 2022 Feb | Linux 5.17 BPF LSM stable | |
| 2022 Jul | Retbleed (Wikner+Razavi) USENIX Security | retpolines are NOT enough on Skylake |
| 2022 Oct | Linux 6.1 Kernel-CFI (Clang) | |
| 2023 Aug | Downfall (Moghimi) + Inception (ETH Zurich) USENIX Security | gather-instruction + return-address leak |
| 2024 Mar | GoFetch (Chen et al) S&P | Apple M-series DMP constant-time break |
| 2024 Jul | Microsoft IPE (Integrity Policy Enforcement) Linux 6.11 | signed-binary-only policy LSM |
| 2025 Q4 | Linux 6.14 fine-grained KASLR (per-function) | |
| 2026 | Arm v9.5-A MTE4 + PACQARMA5; Intel Lunar Lake CET + Memory Protection Keys (MPK) kernel | |

---

## 2. SMAP / SMEP — x86_64 baseline, casi gratis

**SMEP — Supervisor Mode Execution Prevention**
- Intel Haswell, Q2 2013. AMD Zen (2017). CPUID.07.0.EBX bit 7.
- Activado via `CR4.SMEP` (bit 20) = 1.
- **Efecto:** cualquier `CS`=kernel que intente *fetch* de una página con `U/S`=1 (user page) dispara `#PF` con error code `PF_INSTR|PF_USER=0`. El kernel no puede ejecutar código en páginas de usuario.
- **Ataque que bloquea:** clásico *ret2usr* — overflow en kernel, pivot a shellcode en `mmap`-eado de user. Pre-2013 era devastador (Spender/PaX grsec UDEREF patch existía desde 2005 para mitigarlo en software).
- **Coste:** cero runtime, un bit CR4 en cold boot.

**SMAP — Supervisor Mode Access Prevention**
- Intel Broadwell Q4 2014. AMD Zen 2017. CPUID.07.0.EBX bit 20.
- Activado via `CR4.SMAP` (bit 21) = 1.
- **Efecto:** load/store del kernel sobre páginas user triggera `#PF`. Excepciones explícitas:
  - `STAC` (Set AC) + `CLAC` (Clear AC) — prefijo alrededor de acceso controlado (`copy_from_user`, `copy_to_user`).
  - `RFLAGS.AC` (bit 18) gating: cuando AC=1, SMAP está desactivado para esa instrucción.
- Linux wraps cada user-access con `user_access_begin()` / `user_access_end()` (stac/clac) — en x86_64 UAPI.
- **Ataque que bloquea:** kernel dereferencing unvalidated user pointer (`*ptr` donde `ptr` viene de syscall). Pre-SMAP: NULL-deref en kernel con `mmap(NULL, ...)` atacante controlaba page 0. SMAP lo mata muerto incluso si `mmap_min_addr=0`.
- **Coste:** dos instrucciones (STAC+CLAC) por cada user access. ~2–5 ns bursteado. Linux paga ~1% en workloads heavy-syscall.

**Primary refs:**
- Intel SDM Vol 3A §4.6.1 (CR4 bits) + §4.10 (access-rights checks). [Intel SDM Volumes 1-4](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
- AMD Architecture Programmer's Manual Vol 2 §5.3.4 (SMAP/SMEP). [AMD APM](https://www.amd.com/en/support/tech-docs/amd64-architecture-programmer-s-manual-volumes-1-5).
- PaX UDEREF original patch (2005) archivado en [grsecurity.net](https://grsecurity.net/features) — precursor software.

**Cómo activar (código x86_64 aproximado):**
```c
/* tras cpuid detect soporta los bits */
uint64_t cr4;
__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
cr4 |= (1UL << 20);  /* SMEP */
cr4 |= (1UL << 21);  /* SMAP */
__asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

/* acceso controlado */
static inline void stac(void) { __asm__ volatile("stac" ::: "cc"); }
static inline void clac(void) { __asm__ volatile("clac" ::: "cc"); }
```

---

## 3. KPTI / Meltdown — el tax de 2018

**Meltdown (CVE-2017-5754)**
- Lipp, Schwarz, Gruss, Prescher, Haas, Fogh, Horn, Mangard, Kocher, Genkin, Yarom, Hamburg. *"Meltdown: Reading Kernel Memory from User Space"*, USENIX Security 2018. [PDF](https://www.usenix.org/system/files/conference/usenixsecurity18/sec18-lipp.pdf) / [meltdownattack.com](https://meltdownattack.com/).
- Fallo: Intel OoO engine retira exceptions *después* de que loads dependientes se ejecutan especulativamente. Un user load de kernel VA dispara `#PF` al retire, pero los datos ya entrenaron el cache. Flush+Reload decodifica los 256 bytes.
- Afecta Intel casi todo desde P6 (1995) hasta pre-Cascade Lake (2019). AMD no afectado (mejor enforcement de privilege en speculation). Arm Cortex-A75 afectado.

**KPTI (Kernel Page Table Isolation), née KAISER**
- Gruss, Lipp, Schwarz, Fellner, Maurice, Mangard. *"KASLR is Dead: Long Live KASLR"*, ESSoS 2017. [PDF](https://gruss.cc/files/kaiser.pdf). Propuesto originalmente para defender contra prefetch-side-channel sobre KASLR; *ya estaba parcialmente merged cuando Meltdown aterrizó*.
- Linux emergency merge: 4.15 (Jan 2018) — unprecedented out-of-cycle backports a 4.14 / 4.9 / 4.4.
- Diseño: **dos page tables por proceso**:
  - "user" PGD: mapea userspace normal + *sólo* las pages de entry trampoline + IDT/TSS/GDT (mínimo absolutamente necesario para un retorno a kernel).
  - "kernel" PGD: mapea todo.
- Transición: en `syscall`/IRQ entry, trampoline ASM escribe `CR3` con el kernel PGD (flushes TLB except PCID-tagged). Salida: CR3 vuelve a user PGD.
- **PCID mitigation:** Intel PCID (Process Context ID) tagea entradas TLB con 12-bit ID; KPTI asigna dos PCIDs por proceso (user=0x0, kernel=0x80) y usa `MOV CR3` con `NOINV` bit (bit 63) para no flushear. Bajó el coste de 30% → 5–15%.
- **Coste runtime:** 5–30% syscall-heavy (`dd`, database connect storms). Redis `GET` pipelined: ~12%. `make` build: ~3%. Pure compute: ~0%.
- **Bypass moderno:** *EntryBleed* (Luo et al 2022, CVE-2022-42862) — filtra el kernel entry point VA a través del TLB side-channel en CPUs con KPTI pero sin mitigación completa.

**Primary refs:**
- Linux `Documentation/x86/pti.rst` — [git.kernel.org](https://www.kernel.org/doc/html/latest/arch/x86/pti.html).
- Hertzbleed (Wang et al 2022, USENIX Security): clase de side-channel que KPTI no ataca, por si se cita.

---

## 4. KASLR — probabilistic defence, 30 bits and dropping

**Qué es:** cada boot, el kernel se carga a una dirección base pseudorandom dentro de un rango. En Linux x86_64 (`CONFIG_RANDOMIZE_BASE=y` default desde 4.8):
- Base kernel en rango `0xffffffff80000000 + (rand * 2 MiB)` dentro de 1 GiB → **9 bits efectivos** (512 slots × 2 MiB).
- `CONFIG_RANDOMIZE_MEMORY=y` también randomiza direct-map (`PAGE_OFFSET`), vmalloc base, vmemmap. ~30 bits total virtual-space.

**Problema:** 9 bits = 512 posibilidades. Un bug KASLR-leak (e.g. `dmesg` con un pointer, `/proc/kallsyms` con `kptr_restrict=0`, un prefetch-side-channel) defeat completo. KASLR es *defense in depth*, no barrera.

**Fine-grained KASLR (FG-KASLR)**
- Kees Cook + Alexander Lobakin + Kristen Accardi patch series 2020–2023. Merged Linux 6.5 (initial) / 6.14 (stable default).
- Per-function randomization: `objtool` coloca cada función en su propia sección, linker las permuta al boot dentro de `kallsyms` patched. ~24 bits efectivos por función.
- **Coste:** +~0.5% icache miss; perdura por re-ordering de hot functions. Gruss et al mostraron 2017 que *per-se* KASLR cae a prefetch-side-channel independientemente del grano; FG-KASLR sube la barra pero no la cierra.

**Primary refs:**
- Gruss, Maurice, Fogh, Lipp, Mangard. *"Prefetch Side-Channel Attacks: Bypassing SMAP and Kernel ASLR"*, CCS 2016. [PDF](https://gruss.cc/files/prefetch.pdf).
- Kees Cook FG-KASLR LWN article 2020 [lwn.net/Articles/824307](https://lwn.net/Articles/824307/).

---

## 5. Spectre family + retpoline + IBRS + STIBP + eIBRS

**Spectre v1 — Bounds Check Bypass (CVE-2017-5753)**
- Kocher, Horn, Fogh, Genkin, Gruss, Haas, Hamburg, Lipp, Mangard, Prescher, Schwarz, Yarom. *"Spectre Attacks: Exploiting Speculative Execution"*, IEEE S&P 2019. [PDF](https://spectreattack.com/spectre.pdf).
- Afecta casi todo CPU OoO (2005–presente). Mitigación: `array_index_nospec()` / LFENCE-masked bounds check, insertado en gadgets manualmente.

**Spectre v2 — Branch Target Injection (CVE-2017-5715)**
- Atacante entrena BTB con mispredicted indirect branches → kernel salta al gadget atacante-controlado en speculation → leaks via cache.
- **Retpoline** (Paul Turner, Google, 2018): reemplaza `jmp *%rax` / `call *%rax` por una trampa:
  ```
  call  1f
  2: lfence; jmp 2b
  1: mov  %rax, (%rsp)
     ret
  ```
  RSB (return stack buffer) siempre predice correctamente la primera vez, y el loop `lfence; jmp` atrapa la speculation. [LLVM retpoline RFC](https://support.google.com/faqs/answer/7625886).
- **IBRS/IBPB/STIBP** (Indirect Branch Restricted Speculation / Predictor Barrier / Single-Thread Indirect Branch Predictor): microcode MSR bits. `wrmsr IA32_SPEC_CTRL (0x48), 1` on kernel entry, clear on exit.
- **eIBRS** (enhanced IBRS, Skylake+): always-on modo de BTB isolation — mucho más barato que write/clear per-entry. Intel lo recomienda sobre retpoline desde Cascade Lake.
- **Retbleed (Wikner+Razavi, USENIX Sec 2022):** en Skylake (pre-Alder Lake Intel + pre-Zen3 AMD), el `ret` ALSO mispredicts via BTB cuando el RSB underflows. Mitigación: IBRS obligatorio ("retbleed=ibrs") o `rethunk` (Call-depth tracking GRSEC-style). Coste adicional: +7–14% kernel-heavy.

**Arm Spectre v2 equivalente:**
- CSV2/CSV3 architectural features (ARMv8.5-A) garantizan BTB isolation across exception levels.
- Pre-v8.5: firmware-provided `SMCCC_ARCH_WORKAROUND_1` (BPIALL on entry via SMC call).

**Coste:**
- Retpoline puro: +5–15% en syscall-heavy workloads.
- eIBRS only: +1–3%.
- IBRS (retbleed mode) Skylake: +14–30%. Fedora 36 desactivó por defecto en cargas no-server.

---

## 6. seccomp — userspace syscall filter (cubierto en R1, aquí el why)

Ya cubierto detalle en `linux_mainstream.md:61,98`. Resumen de lo que ALZE necesitaría si/cuando tenga userspace:

- BPF program instalado via `prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)`.
- Program evalúa `struct seccomp_data` (syscall nr + args + arch + IP), devuelve `SECCOMP_RET_{ALLOW,ERRNO,KILL,TRAP,LOG,USER_NOTIF,TRACE}`.
- Filters se *anidan* (hijo hereda, y además puede añadir, nunca quitar) — propiedad monotónica por diseño.
- Drewry 2012 Google: *"Dynamic seccomp policies"* USENIX Security Symposium WIP. Merged Linux 3.5.
- **eBPF-seccomp** (arXiv 2302.10366 Ramakrishna et al 2023): stateful filters, verifier complexity blocker.

Para ALZE: deferido hasta que haya userspace (v3+). ~500 LOC (BPF interpreter subset + verifier stub).

---

## 7. LSM stack — SELinux, AppArmor, Smack, Landlock, IPE, BPF-LSM

**LSM framework (Linux Security Modules)**
- Wright, Cowan, Smalley, Morris, Kroah-Hartman. *"Linux Security Modules: General Security Support for the Linux Kernel"*, USENIX Security 2002. [PDF](https://www.usenix.org/legacy/publications/library/proceedings/sec02/wright/wright_html/). Hooks en syscall boundary + VFS + socket + IPC.
- **Stacking** (Casey Schaufler 2018): múltiples LSMs coexisten. Linux 4.2+ permite AppArmor+Yama+Landlock+BPF juntos.

| LSM | Modelo | Año | Default en |
|-----|--------|-----|------------|
| SELinux | Type Enforcement + MLS + RBAC | 2003 Linux 2.6 | RHEL/Fedora/Android |
| AppArmor | path-based profiles | 2007 Linux 2.6.36 (mainline) | Ubuntu/SUSE |
| Smack | simpler label-based | 2008 Linux 2.6.25 | Tizen/automotive |
| Yama | ptrace restrictions | 2010 Linux 3.4 | Ubuntu/Debian |
| Landlock | unprivileged sandbox | 2021 Linux 5.13 | systemd-nspawn, sandboxer |
| BPF-LSM | eBPF programs as hooks | 2020 Linux 5.7 stable 5.17 | Cilium/Tetragon |
| IPE | Integrity Policy Enforcement (Microsoft) | 2024 Linux 6.11 | locked-down Azure hosts |
| LoadPin | only-trusted-fs load | 2016 Linux 4.7 | ChromeOS |

**SELinux deep:** el concepto clave es **type enforcement (TE)**: cada subject (process) tiene un *domain*, cada object (file/socket/...) tiene un *type*. Matrix `allow DOMAIN TYPE:CLASS PERMS`. Android SELinux policy tiene ~50k rules.

**Landlock:** Mickaël Salaün ANSSI 2017–2021. Syscalls `landlock_create_ruleset`, `landlock_add_rule`, `landlock_restrict_self`. El proceso se encadena a sí mismo (no requiere root), análogo a OpenBSD `pledge`+`unveil`. Merged Linux 5.13 (2021-06). V2 filesystem paths only; V3 (5.19) adds network connect/bind. [landlock.io](https://landlock.io/).

**IPE:** Microsoft Azure Boot 2022–2024. Merged Linux 6.11 (2024-09). Policy se firma y el kernel rechaza ejecución de binarios no-firmados por el trust-root. Competes con IMA-appraisal.

**BPF-LSM (aka KRSI — Kernel Runtime Security Instrumentation):**
- KP Singh, Google 2019. *"Kernel Runtime Security Instrumentation"*, LSS-EU 2019. [Slides](https://static.sched.com/hosted_files/lssna19/9b/Kernel%20Runtime%20Security%20Instrumentation.pdf). Merged Linux 5.7 stable 5.17.
- `bpf_lsm_*` hooks == LSM hooks. Attach BPF program vía `BPF_PROG_TYPE_LSM` + `bpf_lsm_attach`.
- Cilium Tetragon, Falco 0.35+ use para policy + observability.

Primary refs:
- LSM docs in `Documentation/security/lsm.rst` [kernel.org](https://www.kernel.org/doc/html/latest/security/lsm.html).
- SELinux Project [github.com/SELinuxProject](https://github.com/SELinuxProject).

---

## 8. CFI — Clang Kernel CFI, Linux 6.1+

**Abadi, Budiu, Erlingsson, Ligatti.** *"Control-Flow Integrity"*, CCS 2005. [DOI 10.1145/1102120.1102165](https://dl.acm.org/doi/10.1145/1102120.1102165). Seminal paper: para cada indirect call/jmp, en compile time asignar un *label* al callsite y al target; en runtime antes del call, verify label match. Prevents arbitrary ROP/JOP.

**Clang CFI** (Tice et al 2014, Google, Enforcing Forward-Edge CFI LLVM) — [Clang CFI docs](https://clang.llvm.org/docs/ControlFlowIntegrity.html). Variants:
- `-fsanitize=cfi-icall` — indirect function calls: type-signature check.
- `-fsanitize=cfi-vcall` (C++) — vtable dispatch, más complejo.
- `-fsanitize=cfi-mfcall` — member function pointer.

**Kernel CFI (kCFI)**
- Sami Tolvanen (Google) 2018 initial for Android, 2021 push for mainline. Merged Linux 6.1 (Dec 2022, `CONFIG_CFI_CLANG`).
- Forward-edge only (indirect calls). Per-function 4-byte type-hash prefix; callsite loads `hash = *(uint32_t*)(target - 4)` and compares to expected before `call`.
- Mismatch → `__cfi_slowpath` → `panic("CFI failure")`.
- **Backward-edge:** deferido a Intel CET shadow-stack / Arm PAC.
- Coste: +1.5% text size (type hashes), +0.3–1% runtime.
- Requires Clang; GCC no soporta (tiene su propio `-fvtable-verify` pero no forward-edge completo).

Refs:
- Tolvanen, "Control Flow Integrity in the Linux Kernel", LSS 2022 [video](https://www.youtube.com/watch?v=pc1LzwWVTrc).
- `Documentation/kbuild/llvm.rst` + `Documentation/security/hardening.rst` kernel docs.

---

## 9. Intel CET — Shadow Stack + IBT

**Whitepaper:** Intel *"Control-flow Enforcement Technology Specification"*, Rev 3.0, May 2019. [Intel CET spec PDF](https://www.intel.com/content/dam/develop/external/us/en/documents/catc17-introduction-intel-cet-844137.pdf). Shipping: Tiger Lake (mobile 2020), Sapphire Rapids (server 2023), Alder Lake P-cores.

**Shadow Stack (SS)**
- Second stack per thread, write-protected at HW level (`SSP` register, new page attribute `R|SS`).
- Every `call` pushes return address both to regular stack AND shadow stack. Every `ret` pops both and compares; mismatch → `#CP` control-protection fault.
- Prevents ROP completely — attacker cannot modify shadow stack without triggering #CP.
- User-mode: enabled by `IA32_U_CET.SHSTK_EN` MSR bit + `CR4.CET`. Kernel-mode: `IA32_S_CET.SHSTK_EN`.
- Linux 6.6 (Oct 2023): user-mode shadow stack. Kernel-mode: WIP 2025.

**IBT — Indirect Branch Tracking**
- Every *legitimate* indirect jump/call target must begin with `ENDBR64` (opcode `0xF3 0x0F 0x1E 0xFA`) or `ENDBR32`. Otherwise `#CP`.
- Compiler inserts `ENDBR64` at function entry + at any basic block that may be indirect-branch target.
- Coupled with retpoline replacement: IBT lets you remove retpoline in eIBRS+IBT mode.

**Coste runtime:** Shadow stack: +0.5–2% (extra memory writes on call/ret). IBT: essentially 0 (ENDBR64 is 4-byte NOP on non-IBT CPUs). Memory: 8 bytes per call frame extra for shadow stack.

Primary refs: Intel SDM Vol 1 §17 + Vol 3 §18. [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

---

## 10. Arm PAC + BTI — ARMv8.3-A / v8.5-A

**PAC — Pointer Authentication Codes**
- ARMv8.3-A (2017), shipping Apple A12 (2018), Cortex-A710 (2021), Neoverse N2/V2 (2021).
- Five keys in system registers: APIA/APIB (instruction), APDA/APDB (data), APGA (generic). Each 128-bit.
- Instrumenta *pointer signing*: `PACIASP` firma `LR` con key A + SP como tweak, into top unused bits (usually bits 55-63 — VA space).
- Verify con `AUTIASP`; inválido produce pointer canónicamente corrupto → `#DA` on deref.
- Compiler: `gcc -mbranch-protection=pac-ret` + `-msign-return-address=non-leaf`.
- **Attacks/bypasses:** Ravichandran et al *"PACMAN"*, ISCA 2022 — speculatively leak PAC bits on Apple M1 via prefetcher. Mitigation via architectural fixes in M2+ + compiler.

**BTI — Branch Target Identification**
- ARMv8.5-A (2020), Cortex-A77+, Neoverse N2+.
- Analog to Intel IBT. Indirect-branch target must be `BTI` instruction (`HINT #20–23` families: `bti`, `bti c`, `bti j`, `bti jc`) or `#BP` exception.
- `-mbranch-protection=standard` in gcc/clang = PAC + BTI.

**Coste:** PAC: +0.5–1% (extra cycles per call/ret). BTI: 0 (hint NOPs).

Primary refs: Arm ARM DDI 0487 §D5. [Arm Architecture Reference Manual](https://developer.arm.com/documentation/ddi0487/latest/).

---

## 11. Side-channel mitigation matrix

| Attack | CVE | Year | Fix | Perf cost |
|--------|-----|------|-----|-----------|
| Meltdown | 2017-5754 | 2018 | KPTI | 5–30% syscall-heavy |
| Spectre v1 (BCB) | 2017-5753 | 2018 | `array_index_nospec`, LFENCE masks | 0.5% spot gadgets |
| Spectre v2 (BTI) | 2017-5715 | 2018 | retpoline, IBRS, eIBRS | 1–15% depending |
| Spectre v4 (SSB) | 2018-3639 | 2018 | SSBD MSR, PR_SPEC_STORE_BYPASS | 2–8% |
| L1TF / Foreshadow | 2018-3615/3620/3646 | 2018 | L1D flush on VMENTER, PTE inversion | 3–15% VMM |
| MDS (Zombieload/RIDL/Fallout) | 2018-12126/12127/12130 | 2019 | `VERW` (MDS_CLEAR) on kernel exit; HT disable | 3–9% |
| SWAPGS | 2019-1125 | 2019 | LFENCE after `swapgs` | <1% |
| TAA (TSX-async abort) | 2019-11135 | 2019 | disable TSX or VERW | <3% |
| CacheOut / VRS | 2020-0549 | 2020 | VERW | <2% |
| SRBDS / CrossTalk | 2020-0543 | 2020 | microcode lock on RNG reads | 1–5% |
| Retbleed | 2022-29900/29901 | 2022 | IBRS-always / rethunk / call-depth-track | 7–30% Skylake |
| Downfall (GDS gather) | 2022-40982 | 2023 | microcode GDS_MITG + disable gather | 9–50% gather-heavy |
| Inception / SRSO | 2023-20569 | 2023 | IBPB on VMENTER, untrained return predictor | 4–12% AMD Zen |
| GoFetch (Apple M1/M2) | — | 2024 | DIT bit (Data Independent Timing) for crypto | varies by workload |
| BHI (Branch History Injection) | 2022-0001 | 2022 | BHB clear on entry | <1% |
| MMIO stale data (MMSD) | 2022-21123/4/5/6 | 2022 | VERW | <2% |
| iTLB multihit | 2018-12207 | 2019 | huge-page split on ITLB | <5% |
| Collide+Power | 2023 arXiv | 2023 | (no kernel fix) | — research |

Compound worst-case cost in 2019–2022 era Linux: **18–30%** syscall-heavy workloads on mitigated Skylake Xeon. Intel Ice Lake + eIBRS dropped that to 5–8%. AMD Zen 2+ historically ~2–5% since AMD was less affected by Meltdown family.

Primary refs:
- Lipp et al 2018 (Meltdown), Kocher et al 2019 (Spectre) — above.
- Canella et al *"A Systematic Evaluation of Transient Execution Attacks and Defenses"*, USENIX Sec 2019. [PDF](https://www.usenix.org/system/files/sec19-canella.pdf). Survey de primera ronda completa.
- Wikner & Razavi *"Retbleed"*, USENIX Sec 2022. [PDF](https://comsec.ethz.ch/wp-content/files/retbleed_sec22.pdf).
- Moghimi *"Downfall: Exploiting Speculative Data Gathering"*, USENIX Sec 2023. [PDF](https://downfall.page/).
- Trujillo, Grass et al *"Inception: Exposing New Attack Surfaces with Training in Transient Execution"*, USENIX Sec 2023. [PDF](https://comsec.ethz.ch/wp-content/files/inception_sec23.pdf).
- Chen et al *"GoFetch: Breaking Constant-Time Cryptographic Implementations Using Data Memory-Dependent Prefetchers"*, IEEE S&P 2024 (Apple M1/M2/M3 DMP break). [gofetch.fail](https://gofetch.fail/).

---

## 12. Stack protection variants

GCC/Clang options:
- `-fstack-protector` — canary only on functions with arrays ≥8 chars. Weak.
- `-fstack-protector-strong` — + any fn with local address-taken, local struct of char arrays, `alloca`. Kees Cook's default since Linux 3.14 (2014). **ALZE usa esto** (Makefile:25 per R2).
- `-fstack-protector-all` — every function. High overhead (~8%), rarely used.
- `-fstack-clash-protection` — probes alloca/VLA pages to prevent skipping guard pages. GCC 8+ Linux.
- `-fcf-protection=full` (Intel) — emits ENDBR64 + shadow-stack hints (complements CET).
- `-fno-stack-protector` — never use for kernel (ALZE no hace; good).

**Canary design:**
- 64-bit random word written at start of epilogue area, checked at return.
- Low byte should be NUL (`0x00`) to prevent string-family overflows copying past it.
- Linux per-task canary: each task has `task_struct.stack_canary`, context switch swaps via `gs:0x28` segment offset.
- ALZE hoy tiene **global canary único** (R2 §SSP). Riesgo: leak en una task → todas comprometidas. Fix: per-task canary + swap on ctx-switch.

Primary refs:
- StackGuard original: Cowan, Pu, Maier, Walpole, Bakke, Beattie, Grier, Wagle, Zhang. *"StackGuard: Automatic Adaptive Detection and Prevention of Buffer-Overflow Attacks"*, USENIX Security 1998. [PDF](https://www.usenix.org/legacy/publications/library/proceedings/sec98/full_papers/cowan/cowan.pdf).
- Etoh Hiroaki IBM ProPolice 2001 — GCC integration.

---

## 13. Tabla resumen — kernel hardening features

| Feature | Vendor/ISA | Year | Perf cost | Security guarantee |
|---------|------------|------|-----------|--------------------|
| SMEP | Intel Haswell / AMD Zen | 2013/2017 | ~0% | No kernel-mode exec of user pages |
| SMAP | Intel Broadwell / AMD Zen | 2014/2017 | ~1% | No kernel-mode access of user pages (except STAC-wrapped) |
| KPTI | software (all x86_64 Intel) | 2018 | 5–30% syscall-heavy | Blocks Meltdown read of kernel from userspace |
| KASLR | software, kernel base random | ~2015 | ~0% | 9-bit entropy, probabilistic ASLR |
| FG-KASLR | software, per-function | 2023–2025 | 0.5% | 24-bit entropy, reshuffles hot code |
| Retpoline | software trampolines | 2018 | 5–15% | Defeats BTI training on indirect branch |
| IBRS / eIBRS | Intel microcode | 2018 / SKL+ | 1–14% (v1) / 1–3% (eIBRS) | Isolates BTB per privilege domain |
| STIBP | Intel microcode | 2018 | 2–5% | Isolates BTB between SMT siblings |
| CET shadow stack | Intel Tiger Lake+ | 2020 | 0.5–2% | ROP impossible (HW-enforced return address) |
| CET IBT | Intel Tiger Lake+ | 2020 | ~0% | Requires ENDBR64 at indirect branch targets |
| Clang kCFI | software, Linux 6.1 | 2022 | 0.3–1% | Forward-edge type-hash check |
| Arm PAC | ARMv8.3-A | 2017 | 0.5–1% | HMAC-style pointer signing, defeats LR/data ptr overwrite |
| Arm BTI | ARMv8.5-A | 2020 | ~0% | ARM analog of IBT |
| Arm MTE | ARMv8.5-A | 2020 | 2–5% (async) / 10–20% (sync) | Memory tagging, catches UAF/OOB |
| seccomp-BPF | software, kernel 3.5 | 2012 | <1% | Whitelist syscalls per process |
| Landlock | software, kernel 5.13 | 2021 | <0.5% | Unprivileged FS+net sandbox |
| BPF-LSM | software, kernel 5.7 | 2020 | varies | Security hook via eBPF programs |
| SELinux/AppArmor/Smack | software, kernel 2.6+ | 2003–2008 | 1–3% | MAC policy enforcement |
| IPE (Microsoft) | software, kernel 6.11 | 2024 | <1% | Signed-binary-only execution |
| `-fstack-protector-strong` | GCC/Clang | 2014 | 0.5–1% | Stack canary, detects stack overflow |

---

## 14. ALZE applicability — three waves

### v1 — baseline hobby kernel (ship before anything else)

**Already have (per R2):**
- SSP `-fstack-protector-strong` + TSC-seeded xorshift canary (ssp.c:36-45).
- W^X at page-table level (vmm.c mapping flags — R2 confirms basic page tables present).

**Add — mandatory v1 (~500 LOC total):**

| Feature | LOC estimate | Location | Notes |
|---------|-------------|----------|-------|
| SMEP enable | ~30 | `kernel/boot.c` post-cpuid | CR4 bit 20 + CPUID.07.0.EBX bit 7 check |
| SMAP enable | ~40 | same | CR4 bit 21 + STAC/CLAC helpers |
| `stac()`/`clac()` wrappers | ~20 | `kernel/uaccess.h` (new) | inline asm, called from copy_from/to_user |
| `copy_from_user` / `copy_to_user` | ~80 | `kernel/uaccess.c` (new) | page-fault-safe, returns bytes-not-copied |
| KASLR limine API | ~100 | `kernel/boot.c` | Request Limine's `kernel-address` feature + `random-seed` for canary |
| Per-task canary + swap | ~60 | `kernel/sched.c` + `context_switch.asm` | TCB field + `mov %gs:canary_offset` load-before-ret |
| Entropy mixing (TSC + LAPIC + RTC + limine seed) | ~50 | `kernel/ssp.c` | closes R2 issue #5 |
| Panic-in-panic guard | ~15 | `kernel/panic.c` | R2 issue #1 |
| `is_user_ptr()` validator | ~30 | `kernel/uaccess.c` | 0x00000000_00000000..0x00007FFF_FFFFFFFF range check |
| Smoke-tests (SMAP fault, SMEP fault, canary fail) | ~80 | `kernel/tests.c` | fault-injection tests with `in_panic` reset for pass |

**Total v1:** ~500 LOC. Yields Haswell-baseline. All testable on QEMU `-cpu host,+smap,+smep`.

**Explicit skips v1:**
- KPTI — hobby kernel no tiene usuarios hostiles. Meltdown asume attacker-controlled userspace; ALZE no tiene userspace aún. Defer indefinitely.
- Retpoline/IBRS — same argument. If ever running attacker code, revisit.
- MDS/L1TF — same.
- CFI — needs Clang build path, ALZE currently GCC-only per Makefile (R2 §Makefile:25 sugiere). Defer to v2.
- seccomp — no userspace. Defer to post-userspace.

### v2 — capability-era kernel (after userspace lands)

**After:**
- CFI via Clang: `make CC=clang` build path + `-fsanitize=kcfi` (~150 LOC adapting asm call-sites, linker scripts).
- seccomp-BPF minimal: classic-BPF interpreter in ~400 LOC + 5 syscalls `{read, write, exit, brk, rt_sigreturn}` baseline.
- AppArmor-lite path-based ACL for `open()` — ~300 LOC using existing VFS path walker.
- BHI clear on entry (software Spectre v2 for retpoline-free older CPUs) — ~40 LOC entry.asm.
- KASLR full: base + stack canary + VMAP + kmalloc slab base randomization.
- `clang -fsanitize=cfi-icall` compile path. Match forward-edge type hashes.
- eBPF interpreter *read-only* for observability hooks (not enforcement yet) — ~1000 LOC.

**Total v2:** ~1800 LOC over v1.

### v3 — aspirational hardware-era

**Requires hardware test environment:**
- Intel CET shadow-stack + IBT (needs Tiger Lake + `ENDBR64` emission throughout asm). Compiler: `-fcf-protection=full`. ~200 LOC asm changes.
- Arm PAC + BTI for AArch64 port (if ALZE ever adds Arm support). Compiler: `-mbranch-protection=standard`. ~300 LOC.
- BPF-LSM style hooks — observability-only, for external security agent (Falco/Tetragon-lite).
- IPE-like signed-binary enforcement with verified-boot chain (Limine + kernel sig + userspace sig).
- Per-process KASLR (Kata Containers-style)
- Memory Tagging Extension (MTE) for heap — async mode, ~8 tag bits in top of pointer, hardware-enforced.

**Total v3:** ~2500 LOC more, but largely conditional — only build on supported HW.

---

## 15. Honest appraisal — the security tax

Hardening desde Meltdown ha agregado conservadoramente:
- **~8% promedio** en cargas syscall-heavy (compound KPTI + retpoline + MDS + SSBD + MDS_CLEAR + Retbleed).
- **~2% promedio** en cargas compute-bound (CFI + shadow stack writes).
- **~15%** pico en Skylake con Retbleed IBRS-always mode.
- **~20%** en VMMs con L1D flush on VMENTER.

Para un hobby kernel **el ratio cost/benefit es muy diferente del de Linux production**:
- No hay multi-tenant → Meltdown no es threat model → KPTI es puro overhead.
- No hay untrusted userspace → Spectre v2 retpoline es overhead sin payoff.
- No hay attacker con physical access → MDS/L1TF sólo comen ciclos.
- El "security tax" en ALZE debería ser <2% neto gastado en SMAP STAC/CLAC churn + canary check + future KASLR boot-time randomization.

**Recomendación concreta para ALZE v1**:
1. Habilitar SMAP+SMEP en boot (~70 LOC, 0% runtime cost).
2. Reforzar SSP existente: per-task canary + Limine entropy (~110 LOC, 0.3% runtime).
3. KASLR vía request Limine's `kernel-address-feature` + `random-seed` (~100 LOC, 0% runtime).
4. Panic-in-panic guard (~15 LOC, fixes R2 issue #1 at zero cost).

**NO hacer en v1:**
- NO KPTI — no hay threat model.
- NO retpoline — no hay threat model y GCC retpoline impone syscall overhead real.
- NO CFI hasta que haya build Clang en Makefile.
- NO seccomp hasta que haya userspace.

**Threat model honesto de ALZE hoy:**
- Boot-time kernel integrity: limine verified-boot (futuro), SSP canary vs stack corruption bug.
- Runtime: bugs propios (buffer overflow en string formatting, NULL deref, race conditions en locking — see ERRORES.md). El "attacker" es el propio código + QEMU.
- No hay untrusted userspace ejecutando cosas.
- Los 5-20% de tax de mitigations modernos son *defense against attackers ALZE doesn't have yet*. Agregarlos es over-engineering.

La regla: **cada defense debe mapear a un threat identificable**. SMAP/SMEP mapea a "kernel bugs that deref userspace" — real en cualquier kernel con syscalls. SSP mapea a "stack overflow bug" — real. KASLR mapea a "exploit reliability reduction" — valor marginal pero gratis si Limine ya lo ofrece. KPTI/retpoline/MDS mapean a "hostile code executing in another context" — no existe en ALZE.

Cuando (si) ALZE gana userspace: reabrir esta nota y re-evaluar v2.

---

## 16. Referencias primarias canónicas (resumen)

- Intel SDM Vol 3A §4, §17, §18 — [intel.com/sdm](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
- AMD64 APM Vol 2 §5 — [amd.com/apm](https://www.amd.com/en/support/tech-docs/amd64-architecture-programmer-s-manual-volumes-1-5).
- Arm ARM DDI 0487 — [developer.arm.com](https://developer.arm.com/documentation/ddi0487/latest/).
- Lipp et al 2018 Meltdown USENIX Sec — [meltdownattack.com](https://meltdownattack.com/).
- Kocher et al 2019 Spectre IEEE S&P — [spectreattack.com](https://spectreattack.com/).
- Gruss et al 2017 KAISER ESSoS — [PDF](https://gruss.cc/files/kaiser.pdf).
- Gruss et al 2016 Prefetch CCS — [PDF](https://gruss.cc/files/prefetch.pdf).
- Drewry 2012 seccomp USENIX Security WIP — [kernel.org/doc seccomp](https://www.kernel.org/doc/html/latest/userspace-api/seccomp_filter.html).
- Wright et al 2002 LSM USENIX Sec — [PDF](https://www.usenix.org/legacy/publications/library/proceedings/sec02/wright/wright_html/).
- Cowan et al 1998 StackGuard USENIX Sec — [PDF](https://www.usenix.org/legacy/publications/library/proceedings/sec98/full_papers/cowan/cowan.pdf).
- Abadi et al 2005 CFI CCS — [DOI](https://dl.acm.org/doi/10.1145/1102120.1102165).
- Intel CET Spec 2019/2020 — [PDF](https://www.intel.com/content/dam/develop/external/us/en/documents/catc17-introduction-intel-cet-844137.pdf).
- Canella et al 2019 Transient-Exec Survey USENIX Sec — [PDF](https://www.usenix.org/system/files/sec19-canella.pdf).
- Wikner & Razavi 2022 Retbleed USENIX Sec — [PDF](https://comsec.ethz.ch/wp-content/files/retbleed_sec22.pdf).
- Moghimi 2023 Downfall USENIX Sec — [downfall.page](https://downfall.page/).
- Trujillo et al 2023 Inception USENIX Sec — [PDF](https://comsec.ethz.ch/wp-content/files/inception_sec23.pdf).
- Chen et al 2024 GoFetch IEEE S&P — [gofetch.fail](https://gofetch.fail/).
- Ravichandran et al 2022 PACMAN ISCA — [pacmanattack.com](https://pacmanattack.com/).
- Kernel Self-Protection Project — [kernsec.org/wiki/index.php/Kernel_Self_Protection_Project](https://kernsec.org/wiki/index.php/Kernel_Self_Protection_Project).
- Landlock — [landlock.io](https://landlock.io/).
- Kees Cook security-hardening wiki — [Documentation/security/self-protection.rst](https://www.kernel.org/doc/html/latest/security/self-protection.html).
- KP Singh 2019 BPF-LSM/KRSI LSS-EU slides — [sched.co](https://static.sched.com/hosted_files/lssna19/9b/Kernel%20Runtime%20Security%20Instrumentation.pdf).
- Tolvanen 2022 kCFI LSS — [YouTube](https://www.youtube.com/watch?v=pc1LzwWVTrc).

Fin del deep-dive.
