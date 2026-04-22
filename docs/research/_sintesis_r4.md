# Síntesis round 4 alze_os — OSes que saltamos o solo rozamos

**Fecha:** 2026-04-22
**Input:** 7 agentes paralelos, ~3,507 líneas. 0 fallos bloqueantes.

Complementa `_sintesis.md` (R1) + `_sintesis_r3.md`. Donde R1 hizo overview de OS families y R3 deep-dive de subsistemas técnicos, R4 vuelve a **OSes concretos** — pero no 10-line summaries como R1 sino 400+ líneas per OS con detalles operacionales, política, deployments 2026.

- [`r4/fuchsia_zircon.md`](r4/fuchsia_zircon.md) — Google microkernel, Nest Hub 2021, Starnix Linux ABI, decline 2022-2024 (740 L)
- [`r4/sel4_verified.md`](r4/sel4_verified.md) — UNSW verified kernel, l4v, CAmkES, Microkit, aerospace deployment (602 L)
- [`r4/redox_os.md`](r4/redox_os.md) — Rust microkernel 2015+, schemes, 11 años alpha, ~1 FTE (361 L)
- [`r4/haiku_beos.md`](r4/haiku_beos.md) — BeOS descendant, BFS attribute queries, BMessage, 24 años volunteer (351 L)
- [`r4/bsd_family.md`](r4/bsd_family.md) — FreeBSD/NetBSD/OpenBSD/DragonFly + pledge/unveil/capsicum/jails (539 L)
- [`r4/plan9_inferno.md`](r4/plan9_inferno.md) — Bell Labs 1984, 9P everything-is-a-file-server, 9front active (439 L)
- [`r4/hobbyist_oses.md`](r4/hobbyist_oses.md) — SerenityOS + ToaruOS + HelenOS + Minix + TempleOS + lessons (475 L)

---

## Hallazgos meta — coincidencias + personas que se repiten

### Coincidencia #1: Travis Geiselbrecht escribió DOS kernels shipped a 20 años
- **NewOS** (2001+) → **Haiku kernel** (2004+, R1 beta 5 2024, shipping)
- **LK (Little Kernel)** (2008+) → **Zircon** (2015+, Nest Hub 2021, shipping)
Misma persona. Dos OSes microkernel-ish shipped a dos décadas de diferencia. **Lección**: kernel design es craft transferible, no un proyecto específico.

### Coincidencia #2: Dominic Giampaolo escribió DOS filesystems CoW
- **BFS** (Be File System, 1996-1998) → shipping en BeOS + Haiku (still shipping 2026)
- **APFS** (Apple File System, 2014-2017) → shipping en iOS/macOS (all Apple devices)
Misma persona. Dos CoW filesystems con 15+ años de intervalo. **Lección**: FS design es también craft transferible.

### Hallazgo crítico: ALZE tiene NO LICENSE
R4/bsd_family descubrió que `/root/repos/alze-os` no tiene archivo LICENSE. **Trivial fix**: 8 líneas ISC license, pero hasta arreglarlo legalmente nadie puede contribuir / forkear. Prioridad #1 no-código.

### Lección SerenityOS: proceso >>> código
Andreas Kling 2018+: **YouTube vlogs semanales** sobre debugging + culture "pretty and fast" + narrow scope (Unix clone). Resultado: Ladybird browser spinoff 2024 (foundation + funding). El diferenciador NO es tecnología sino engagement continuado. Hobbyist OSes que no ship videos = obscuridad. **Ship demo videos trimestral. Community > code.**

### Lección TempleOS: technical brilliance sin community = obscure after death
Terry Davis 100k+ LOC solo + HolyC + divine revelation. Brilliant. Muerto 2018. Nadie mantiene. Contrast con ToaruOS (K. Lange, 15 años solo, aarch64 port 2022, still alive). Longevidad requiere comunidad.

### Lección seL4: verification es institucional-only
~480k LOC Isabelle proof para 10k LOC C kernel. 2-3 engineer-years per kLOC. HENSOLDT + Boeing + Lockheed backing. Solo-dev NO puede. Puede copiar **patterns** (Untyped retype, CNode, endpoint fastpath) sin verification.

### Lección Redox: 11 años y sigue alpha
Jeremy Soller 2015 → 2026. ~1 FTE. ~20 KLOC kernel. Self-hosting rustc 2024. No production users. Enseña: **hobby OS timeline es 15-20 años mínimo sin backing institucional**.

### Lección Plan 9: 40 años después sigue siendo vanguard
9P + namespaces + "everything is a file server" = influencia sobre FUSE, Docker volumes, Unix sockets-as-APIs, Go (diseñado por Pike+Thompson), K8s service discovery. **ALZE adoptar 9P nativo ~1000 LOC = diferenciador real**. 9front tooling abre userland entero.

### Lección BSDs: unsung winners de la era Linux
- PS4/PS5 = orbisOS (FreeBSD) = ~200M unidades
- Netflix CDN (FreeBSD, 800 Gbps per box)
- WhatsApp (FreeBSD + Erlang)
- Juniper routers (JUNOS = FreeBSD)
Más despliegue que mucho Linux. No-single-vendor governance + correctness focus + licencia BSD (no GPL) = sustentabilidad.

---

## Tabla cross-R4 (OS × state 2026 × relevance for ALZE)

| OS | State 2026 | Team | LOC | Lección unique | Copiable para ALZE |
|---|---|---|---|---|---|
| **Fuchsia + Zircon** | declining (Nest only) | Google core team + layoffs 2024 | Zircon ~150k | Capabilities PRO but no apps = no market | VMO + handle model + FIDL IDL = v2 aspiracional |
| **seL4** | active (UNSW + HENSOLDT) | 50-100 researchers + industry | Kernel 10k + proof 480k | Only fully-verified general-purpose kernel | Patterns (CNode, endpoints) v2, NO verification |
| **Redox** | alpha (11 años) | ~1 FTE (Jeremy Soller + donations) | ~20k kernel + ~100k total | Rust microkernel pattern completo | Scheme URL IPC = v2 pattern worth studying |
| **Haiku** | beta (5 shipped R1 betas) | ~20 volunteers 24 años | ~5M total | BeOS-compat, BFS attribute queries | Attribute queries FS model + BMessage IPC = ideas |
| **FreeBSD** | mature shipping everywhere | 200+ committers | ~20M | **ZFS first-class + jails + kqueue + pledge** | License (BSD), kqueue pattern, jails inspiration |
| **OpenBSD** | mature security-first | 150+ committers | ~3M | Security mitigations inventadas (W^X 2003, pledge 2015) | **pledge/unveil ~50 LOC** = v1 inmediato |
| **NetBSD** | niche portability | 100+ committers | ~8M | 57+ ports, rump kernels | Rump kernel pattern (kernel code en userspace) = v2 |
| **DragonFly** | niche | ~10 contributors | ~3M | HAMMER2 CoW FS, LWKT | HAMMER2 paper + LWKT concepts |
| **Plan 9** | mature (Bell Labs 2015 last update) | Bell Labs dissolved | ~500k | **9P + namespaces** = 40 años vanguard | **9P nativo ~1000 LOC = REAL differentiator ALZE** |
| **9front** | active fork (Moody + Bernstein + cinap) | ~5-10 committers | ~600k | Plan 9 + UEFI + Wi-Fi + NVMe 2026 | Same as Plan 9 plus modern drivers |
| **Inferno** | dormant (Vita Nuova minimal) | ~0-2 active | ~200k | Limbo → Go inheritance | Historical — study Go inspiration |
| **SerenityOS** | active (no 1.0 yet) | 100+ contributors post-Ladybird | ~2M | **Community + YouTube + narrow = winning hobby model** | **Process lessons** > code |
| **Ladybird** | alpha 2026 | Foundation + funded team | ~500k | SerenityOS browser spinoff → independent | Not copyable, case study in focus |
| **ToaruOS** | active | K. Lange solo 15 años | ~80k | Solo-dev longevity + aarch64 port 2022 | Demonstrates solo-dev feasibility |
| **HelenOS** | active academic | Charles U. Prague ~15 | ~1M | Academic microkernel Czech Republic | — |
| **Minix 3** | dead (2018 last) | VU Amsterdam dissolved | ~200k | Reliability via driver restart | Historical lesson only |
| **TempleOS** | dead (Terry 2018) | Solo 100k LOC | ~100k | Divine revelation pure | Cautionary tale |
| **KolibriOS** | active | Russian forum community | ~200 KB asm | Minimal x86 assembly | Historical curiosity |
| **Oberon** | academic (ETH) | Wirth dead 2024, Gutknecht | ~50k | TUI unique, Pascal-esque language | Historical education |

---

## Top 10 ideas R4 para ALZE

### 1. ADD LICENSE (ISC, 8 líneas) — v1 día 1
Trivial but blocker. Sin license file, nadie puede contribuir legalmente. R4/bsd_family.md finding.

### 2. Implement 9P protocol nativo (~1,000 LOC) — v1 opcional, diferenciador real
El único hobby kernel 2026 con 9P nativo = opens 9front/drawterm/p9p userland toolchain. "Everything is a file server" filosofía. Copiable en ~1k LOC C99. R4/plan9_inferno.md §15.

### 3. pledge-style sandbox (~50-200 LOC) — v1
OpenBSD pledge() declaran "I only do net + stdio" → kernel enforce. API tiny. Security bang-per-buck altísimo. R4/bsd_family.md §7.

### 4. kqueue-style event notification (~2k LOC) — v2
Better designed than epoll. Events sobre files + sockets + timers + signals + processes uniformemente. FreeBSD heredero. R4/bsd_family §6.

### 5. Zircon VMO + handle model adopt — v2
First-class virtual memory objects + typed handles + rights bitmap = capability-like sin formal verification. ~2-3k LOC. R4/fuchsia_zircon.md §4-5.

### 6. BMessage-style IPC (Haiku pattern) — v2
Object-oriented messaging pervasive. Influence on Android Handler/Looper + FIDL. Considerar sobre POSIX pthread signal/mutex soup. R4/haiku_beos §6.

### 7. BFS attribute queries (Haiku) — v3 FS idea
Files = database records. SQL-like query over file attributes. 2026 nadie tiene esto en Linux/macOS. Differentiator si ALZE tiene FS. R4/haiku_beos §5.

### 8. Rump kernels pattern (NetBSD) — v3
Kernel code corre en userspace testing. Validation + testing infrastructure. Relevant para solo-dev debugging. R4/bsd_family §3.

### 9. FreeBSD VNET as one primitive — v3
VNET = network stack clone per jail. Single primitive vs Linux 5 orthogonal (netns+veth+bridge+iptables+netfilter). Cleaner model. R4/bsd_family §9.

### 10. SerenityOS engagement model — v1 non-code
YouTube vlogs + narrow scope + community. Process > code. ALZE debería shipping demo videos trimestralmente. R4/hobbyist_oses §11.

---

## Anti-patterns R4 (nuevos)

Continúan numeración (R1 = 1-12, R3 = 13-22):

23. **OS without LICENSE** — legalmente bloqueante. Decide GPL vs MIT vs ISC vs BSD y commit.
24. **Formal verification solo-dev** — seL4 enseña que es institucional only. Don't attempt.
25. **Rust rewrite de kernel existente** — Redox 11 años alpha prueba timeline. Bilingual mejor.
26. **Container namespaces implementation en capability kernel** — redundante. Capabilities SON namespaces.
27. **Plan 9 namespace system port a non-9P kernel** — trivialmente roto sin 9P underneath. All or nothing.
28. **TempleOS-style pure technical purity** — no community = obscure post-author. Ship videos.
29. **Minix 3-style reliability pitch sin users** — dead 2018. Pitch isn't enough, users required.
30. **GCC 2.95 legacy binary compat** (BeOS/Haiku trampa) — technical debt per-decade-of-legacy.
31. **Annual cadence in hobby OS** — Redox shows 2026-04 still alpha. 2-3 year "milestone" more realistic than "release."
32. **Solo-dev ambitious scope (everything Linux has)** — SerenityOS ganó con narrow Unix subset. Don't try to match Linux feature list.

---

## Engines por lesson dominante

| OS | Lección única para ALZE |
|---|---|
| **Fuchsia/Zircon** | Capabilities + VMOs técnicamente superior a Unix, pero OS sin apps = sin market. Zircon patterns = gold standard copy. |
| **seL4** | Verification = institutional. Patterns copyable sin proof. Read the manual. |
| **Redox** | 11 años y sigue alpha. Hobby OS timeline = 15-20 años mínimo. Target vertical slice, no feature parity. |
| **Haiku** | 24 años de dedicación volunteer. Narrow focus + community > code quality. |
| **FreeBSD** | Unsung production winner. License BSD + no-single-vendor governance = sustentabilidad. |
| **OpenBSD** | Security innovations que Linux copia años después. pledge/unveil bang-per-buck altísimo. |
| **Plan 9** | 40 años vanguard. 9P como primary IPC es ALZE's realistic differentiator. |
| **SerenityOS** | Process > code. YouTube engagement = multiplier. Narrow focus wins hobbyist space. |
| **TempleOS** | Cautionary tale. Genius sin community = obscure death. |

---

## Cierre R4

R1 mapeó paisaje. R2 encontró bugs. R3 conectó técnicas modernas con bugs. **R4 añade lecciones humanas**: timelines realistas (Redox 11 años alpha), engagement (SerenityOS YouTube), coincidencias de personas (Geiselbrecht + Giampaolo), hallazgos legales (LICENSE missing), y **un differentiator real y concreto** (9P protocol nativo en ~1,000 LOC) que pone a ALZE en posición defendible vs "otro hobby Linux-clone."

R5 siguiente cubre cross-cutting systems (boot, drivers, VM, power, security, RTOS, containers) que completan el mapa operacional.
