# Hobbyist and solo-dev OSes — what ALZE can learn

**Ronda:** R4 / `hobbyist_oses.md`
**Fecha:** 2026-04-22
**Scope:** otros hobbyist / academic / solo-dev OSes y, sobre todo, sus **lecciones de proceso**. R1 los pasó por alto (`otros.md` mencionó Redox/Haiku/ReactOS y poco más); R4 hace foco en SerenityOS, ToaruOS, HelenOS, TempleOS, Minix 3, Oberon, Kolibri/Menuet, y en la comunidad osdev.org. ALZE OS *es* un hobbyist OS; este doc es el más cercano a "cómo te organizas para que no muera en 2027".

Referencias cross-link ya cubiertas: Redox (`otros.md` + `r4/redox_os.md`), Haiku (`otros.md` + `r4/haiku_beos.md`), ReactOS (`otros.md`), BSDs (`r4/bsd_family.md`). Aquí los trato como **comparables de proceso**, no de arquitectura.

---

## 1. Panorama 2026 — quién sigue vivo

| OS | Lenguaje | Scope | Equipo | Años activo | Achievement señal |
|---|---|---|---|---|---|
| **SerenityOS** | C++20 | Unix-like desktop from scratch | ~500 committers, 1 BDFL (Kling) hasta 2024 | 2018–presente | Userland + GUI + self-hosting compiler en 6 años |
| **Ladybird** | C++20 | Navegador web cross-platform | ~50 FT tras 2024 spin-off, foundation-funded | 2019–presente (spin-off 2024) | Passes Acid3, primer alpha público 2026 |
| **ToaruOS** | C | Unix-like + Yutani compositor | 1 persona (K. Lange) | 2011–presente (~15 años) | Self-hosting solo-dev; aarch64 port 2022 |
| **HelenOS** | C | Microkernel multiserver, portable | ~20 académicos CUNI + contributors | 2001–presente | 8 arquitecturas; multiserver filesystem |
| **Minix 3** | C | Multiserver reliable OS | ~10 Vrije Universiteit | 2005 relanzamiento–~2018 activo | Driver restart; fue default en Intel ME hasta ~2020 |
| **TempleOS** | HolyC (C variant) | Single-user ring-0 religious OS | 1 persona (Terry Davis, RIP 2018) | 2003–2018 | ~100 KLOC OS completo por 1 persona con esquizofrenia |
| **ReactOS** | C | Binary-compatible Windows NT | ~30 core + community | 1996–presente | Wine-compatible userland; MSVCRT sync 2026 |
| **Oberon System** | Oberon lang | Research workstation OS | Wirth + Gutknecht + ETH team | 1987–presente (A2 fork activo) | Compiler + OS + GUI en <10 KLOC; nuevo en 2013 |
| **KolibriOS** | FASM x86 asm | Desktop minimalista | ~15 (comunidad ex-URSS) | 2004–presente | OS + apps + GUI en **floppy de 1.44 MB** |
| **MenuetOS (64)** | FASM x86-64 asm | Desktop asm, closed-source | Ville Turjanmaa + ~5 | 2000–presente | Kernel SMP en <2 MB, closed-source |

Muertos / coma:
- **SkyOS** — Robert Szeleney ~2003–2009. Desktop con BFS clone. Archivado.
- **BlueBottle / A2** — Active Oberon system. Sobrevive en ETH pero sin tracción.
- **Syllable / AtheOS** — Fork BeOS-inspired; último commit ~2012.
- **HouseOS / House** — Haskell kernel experiment 2005. Github dormido.

---

## 2. SerenityOS — el caso canónico hobbyist 2018–2024

### Origen

Andreas Kling (ex-Apple WebKit, ex-Nokia). Tras rehab por adicción (documentado en su blog + vlogs), busca proyecto terapéutico. 10 octubre 2018 primer commit SerenityOS. [github.com/SerenityOS/serenity](https://github.com/SerenityOS/serenity) / [fundación](https://serenityos.org/). Kling decide **Unix-clone con look-and-feel 90s** (evocación Windows 2000 + BeOS). No innova en arquitectura — copia System V deliberadamente. Premisa: "the OS I want to use, nothing more, nothing less".

### Arquitectura (deliberadamente aburrida)

- Kernel monolítico x86_64 (x86_32 descontinuado ~2022), multicore.
- ext2 filesystem propio + custom VFS.
- ELF binaries, POSIX-ish syscalls, bash-compatible shell propia.
- GUI: `WindowServer` (display server propio) + `LibGUI` + `LibGfx`. Tema "Chocolate" (beige). Icons pixel-art.
- Self-hosting C++ desde 2021 con GCC port. Clang tardó más.
- Red: stack TCP/IP básico desde ~2021, pero en 2026 **sigue sin Wi-Fi driver estable** — ethernet virtio + e1000 funcionan; hardware real escaso. Sólo DHCP + TCP, sin IPv6 production-ready.

Todo en C++ moderno: `AK::` (la libc++ propia: `String`, `Vector`, `HashMap`, `WeakPtr`, `RefPtr`, `NonnullOwnPtr`). Kling impone style guide **estricto**: `AK::` replaces std, no exceptions, dos-espacios, `snake_case` para funciones, `PascalCase` clases. Guía escrita: [CONTRIBUTING.md](https://github.com/SerenityOS/serenity/blob/master/CONTRIBUTING.md).

### El ingrediente no-técnico: YouTube

- Canal Andreas Kling (2019–): ~300 videos, ~1 M views acumulados por 2024. Formato: "Let's build X in SerenityOS" 30–90 min, screen-recorded pair-coding con música lofi, sin edición pesada. Ejemplo: "OS hacking: writing a new assembly listing tool" (30 min).
- **Efecto multiplicador**: cada video convierte ~1 % viewers en comentadores + ~0.1 % en committers de primer PR. Kling estima (blog 2022) que el 60 % de contribuciones externas llegaron por un video específico.
- Weekly videos consistentes 2019–2023. Comunidad Discord ~5,000 users 2023.
- **Lección clave**: el YouTube *no es marketing*, es **recruitment engine**. La fuente de trabajo gratis de primer nivel.

### Ladybird browser — spin-off 2024

[ladybird.org](https://ladybird.org). Kling anuncia julio 2024 que deja el papel de BDFL en Serenity y funda la **Ladybird Browser Initiative** (501c3 US). Financiación inicial: $1 M de Chris Wanstrath (ex-GitHub CEO), luego Nat Friedman, Shopify. Objetivo: navegador web *desde cero*, cross-platform (Serenity + Linux + macOS), sin código de Chromium/WebKit/Gecko.

- **Estado 2026**: pasa Acid3. ~60 % Web Platform Tests. Alpha público Q2 2026 targeting enthusiasts. Still pre-1.0.
- Escrito en C++, compartiendo `LibWeb` + `LibJS` + `LibWasm` con Serenity (monorepo → submódulo desacoplado). LibJS pasa ~95 % test262.
- Equipo: ~12 FT pagados + 50 part-time contributors.
- Kling abandonó Serenity formalmente como BDFL en 2024; Serenity sigue (PRs activos, ~20/semana 2026) pero sin líder fuerte → riesgo de deriva.

### Métricas crudas (2026-04)

- LOC: ~2.5 M (kernel+libs+apps+ports).
- Commits: ~85,000.
- Contributors all-time: ~1,400.
- Ports: bash, vim, gcc, git, doom, quake, SDL, Python, Node, ffmpeg. Ports dir tiene ~400 software paquetes.
- **Sigue sin**: Wi-Fi estable, audio mixing profesional, USB mass storage 100 %, navegador *dentro del OS* (Ladybird ahora targetea Linux/Mac primero).

### Lección dura: sin BDFL el proyecto se dispersa

Tras el spin-off, Serenity pierde dirección unificada. Issues se acumulan (2,000+ abiertos en 2026 vs ~800 en 2023), PRs bikeshed, tareas grandes (network stack, real Wi-Fi, SMP refinement) se estancan. **El BDFL era el filtro de calidad**. Una comunidad sin BDFL necesita reemplazarlo por un comité editorial explícito.

Primary refs:
- Andreas Kling, *SerenityOS — a graphical Unix-like operating system* (2018-). [serenityos.org](https://serenityos.org/) / [archive](https://web.archive.org/web/2026*/https://serenityos.org/).
- Andreas Kling, YouTube channel [@awesomekling](https://www.youtube.com/@awesomekling) 2019-2024.
- Kling, *"Ladybird — a new browser for the web"*, blog post, 2024-07-01. [awesomekling.github.io](https://awesomekling.github.io/Ladybird-browser-initiative/).
- Chris Wanstrath, *"Why I'm funding Ladybird"*, 2024. [chriswanstrath.com](https://chriswanstrath.com/2024/07/ladybird).
- SerenityOS Handbook (community wiki). [wiki.serenityos.org](https://wiki.serenityos.org/).

---

## 3. ToaruOS — 15 años de un solo dev

### Origen

Kevin Lange (entonces alumno UIUC). Primer commit 2011 como proyecto de clase. Desde entonces, **single-person project** salvo PRs puntuales. [github.com/klange/toaruos](https://github.com/klange/toaruos) / [toaruos.org](https://toaruos.org/).

### Arquitectura

- Kernel híbrido C, x86_64 (2021+) y **aarch64 (2022+)** — rara avis: un hobbyist OS portado a ARM por la misma persona.
- VFS + ext2/iso9660 + tmpfs + devfs.
- Userland POSIX parcial; shell `esh` (Emmanuel Shell).
- Compositor gráfico **Yutani** — server/client sobre Unix domain sockets, doble buffer, compositing real con transparencia. `yctx_poll` event loop style.
- Window manager propio (Panel + menu + file browser) estilo early GNOME.
- Puertos: Python 3.6, GCC, vim, doom, quake, Mesa off-screen.

### Estilo de desarrollo

- **1 commit/día promedio** durante 15 años.
- Blog escaso pero consistente: ~10 posts/año. Estilo técnico seco.
- **Sin Discord ni YouTube** — presencia mínima en redes. Pequeño Twitter.
- Lange trabaja fulltime en otra cosa (Red Hat Bluestreak luego Meta); ToaruOS es estrictamente hobby.

### Lección ToaruOS

- Un solo dev *puede* mantener un OS hobbyist **15 años** sin morir. Requisitos: (a) scope fijo (no ampliar a cosas imposibles), (b) hábito diario/semanal, (c) no depender de community (no drama cuando no hay PRs), (d) placer intrínseco en el código. **Opuesto a SerenityOS**: sin marketing, sin multiplicador — pero sobrevive.
- **Scope discipline**: Lange nunca prometió Wi-Fi, ni navegador, ni SMP. Hace lo que puede hacer él. ALZE: lista corta de features que *yo* (o el equipo) puede hacer, no lo que sería "chulo".

Primary refs:
- K. Lange, *ToaruOS — A hobby UNIX-like operating system* (2011-). [toaruos.org](https://toaruos.org/) / [github](https://github.com/klange/toaruos).
- K. Lange, blog: [blog.toaruos.org](https://blog.toaruos.org/) / [archive](https://web.archive.org/web/2026*/https://blog.toaruos.org/).
- K. Lange, *"ToaruOS at 10 — a retrospective"* 2021. Self-published.

---

## 4. HelenOS — academia como OS project

### Origen

Charles University Prague (Univerzita Karlova). Faculty of Mathematics and Physics. Iniciado 2001 como SPARTAN research kernel; renombrado HelenOS ~2004. Supervisor: Petr Tůma + estudiantes. [helenos.org](http://helenos.org/) / [github](https://github.com/HelenOS/helenos).

### Arquitectura

- **Microkernel multiserver** (filosofía seL4-light).
- Kernel ~30 KLOC C. 8 arquitecturas: ia32, amd64, arm32, arm64, ia64, mips32, ppc32, sparc64.
- **IPC async + sync**: llamadas de mensaje con argumentos en registros + optional buffer. Interfaz similar a Mach ports pero más limpia.
- **VFS multiserver**: un proceso `vfs` en userland, filesystem drivers como servers separados (`ext4fs`, `fat`, `mfs`). Crash de un FS driver → VFS restarts; kernel intacto.
- GUI **HelenOS GUI** (HGUI) — window server userland, widgets propios. Feo pero funcional.
- HelenOS Coastline → ports Unix: bash, gcc (legacy 4.x), Python 2.7 histórico.

### Estado 2026

Activo pero tranquilo. ~20 contribs core. Release `0.15.0` 2024. Dev list activa, ~5 commits/semana. Usado como **objeto de estudio** en cursos sistemas operativos en 3 universidades europeas.

### Lección HelenOS

- **Un OS con vida académica sobrevive décadas** sin necesitar ser popular. Cada tesis doctoral produce un subsistema + paper + código mergeado. El OS actúa como substrato de research reusable.
- **Multiserver discipline**: la separación filesystem driver ↔ VFS ↔ kernel demuestra en la práctica que el restart-driver-on-crash (pitch Minix 3) **funciona** sin penalty prohibitiva.
- ALZE podría imitar el patrón: aliarse con una universidad (o un curso online) → cada semestre 5-10 estudiantes contribuyen un subsistema cada uno, bajo supervisión. **Mucho más barato que pagar FTs**.

Primary refs:
- J. Jermář, *"HelenOS — a self-contained microkernel multiserver operating system"*, Charles University thesis + conference reports, 2006-.
- [HelenOS book](http://www.helenos.org/book/) — tutorial en profundidad.
- HelenOS wiki. [helenos.org/wiki](http://www.helenos.org/wiki/).

---

## 5. Minix 3 — Tanenbaum y el "driver restart"

### Origen

Andrew S. Tanenbaum (Vrije Universiteit Amsterdam). Minix 1 (1987) como teaching OS acompañando el libro *Operating Systems: Design and Implementation*. Famosamente, **Linus Torvalds lo usó para bootstrap Linux en 1991**, desencadenando el [Tanenbaum–Torvalds debate](https://www.oreilly.com/openbook/opensources/book/appa.html) en comp.os.minix 1992. Tanenbaum: "Linux is obsolete" (monolíticos son el pasado). Linus: "microkernels are an academic diversion". Hoy el debate se lee como quién tenía razón en qué: Tanenbaum en reliability, Linus en pragmatismo y hardware support.

**Minix 3** (2005) fue un relanzamiento con foco en reliability — "self-healing OS".

### Arquitectura Minix 3

- **Microkernel ~5 KLOC** + IPC síncrono + scheduler. Todo lo demás en userland como servers.
- **Reincarnation Server (RS)**: daemon que monitorea servers de sistema (FS, net, drivers). Un crash → RS lee stats heartbeat + respawn con estado limpio. Pocos segundos downtime. Demostrado en tests: inyectan fallos en driver USB, sistema sobrevive.
- [Herder+Bos+Gras+Homburg+Tanenbaum, *"MINIX 3: A Highly Reliable, Self-Repairing Operating System"*, ACM SIGOPS Operating Systems Review, 2006-07, DOI 10.1145/1151374.1151379](https://dl.acm.org/doi/10.1145/1151374.1151379).
- NetBSD userland port para pragmatismo (2010s).

### Curiosidad: Intel ME

Intel Management Engine firmó Minix 3 como OS base desde ~2015. Probablemente "la instalación de OS más grande del planeta" sin que nadie lo supiera. Intel nunca lo anunció; Tanenbaum lo descubrió via reverse-engineering community 2017 y [publicó carta abierta](http://www.cs.vu.nl/~ast/intel/) — estaba divertido pero molesto por no ser citado. Hacia 2022+, nuevas generaciones Intel migraron ME a otro OS (no publicado, posiblemente ThreadX).

### Estado 2026

Parado. Último release **3.4.0 (2018)**. Tanenbaum retired. VU grupo se disolvió. Community wiki congelado. Wikipedia entry desactualizada. En 2026, Minix 3 es **proyecto de museo** — artefacto educativo estable, sin futuro activo.

### Lección Minix 3

- **"Reliable self-healing" como pitch funcionó académicamente + Intel** pero no capturó comunidad open-source. Lección: un pitch bueno sin community-building = adoption silenciosa (Intel) pero no ecosistema.
- **Teaching OS que mutó a production OS** es difícil: el público target es académico, no el usuario. Minix 1 cerraba el ciclo en clase; Minix 3 quiso ser más y no supo atraer.
- ALZE: no apuntar a "sustituir Linux". Apuntar a "ser un vehículo de aprendizaje cuyas ideas se roban" — OpenBSD es el modelo: nunca será popular pero sus ideas (pledge, unveil, LibreSSL) están en todas partes.

Primary refs:
- A.S. Tanenbaum + A. Woodhull, *Operating Systems: Design and Implementation*, 3rd ed, Pearson 2006 — libro canónico.
- J.N. Herder, H. Bos, B. Gras, P. Homburg, A.S. Tanenbaum, *"MINIX 3: A Highly Reliable, Self-Repairing Operating System"*, SIGOPS 2006. [DOI](https://dl.acm.org/doi/10.1145/1151374.1151379) / [ACM author copy](https://www.cs.vu.nl/~ast/Publications/Papers/MINIX_3.pdf).
- [Minix 3 website](http://www.minix3.org/) (frozen).
- Tanenbaum–Torvalds debate archive. [oreilly.com](https://www.oreilly.com/openbook/opensources/book/appa.html).

---

## 6. TempleOS — el lado oscuro de la genialidad aislada

### Origen

Terry A. Davis (1969–2018). Bipolar / schizophrenia severa (publicly documented). Cree haber recibido revelación divina; Dios le ordena construir "God's official temple". Trabaja ~10 años (2003–2013) produciendo **~100,000 LOC en solitario**: kernel + compilador + editor + apps + sample programs.

- TempleOS site (original ya no operativo): archivo en [templeos.org wayback](https://web.archive.org/web/2018*/https://www.templeos.org/).
- [Terry A. Davis archive](https://templeos.holyc.xyz/) — mirror comunitario post-mortem.
- Davis murió 2018 atropellado por tren tras años homeless. La community retiene su obra.

### Arquitectura

- **Single-user, single-process-mode filosófico** pero técnicamente multitasking cooperativo.
- **Ring-0 everywhere**: no hay user vs kernel mode. Cualquier programa puede `cli` / `hlt` / leer physical memory. Justificación "bíblica": "como un templo, no debe haber barreras". Técnica: simplifica kernel, elimina syscall cost. Obviamente inaceptable para multi-user.
- **Resolución 640×480, 16 colores**. Fija. Justificación: "Dios dijo que el cielo tiene 16 colores".
- **HolyC** — variante C con: (a) REPL + JIT integrado, (b) `$$` para embedded graphics en código, (c) semántica de strings más simples.
- **No network stack**. Sin Internet — "Dios lo prohibió".
- FAT filesystem propio, ~1 MB kernel binary.
- Build system: el OS se compila a sí mismo en ~30 segundos desde dentro.

### El engagement engine fallido

- Davis streameaba en YouTube frecuentemente 2013–2018. Contenido: él programando TempleOS mientras monologa religioso/agresivo (a menudo racista/paranoide por la enfermedad).
- Comunidad pequeña lo siguió como outsider-art; plataformas lo baneaban por harassment. Nunca hubo PRs externos significativos — **el código es enteramente suyo**.
- Post-mortem (2018+): forks comunitarios (Shrine, ZealOS) con limpieza + bugfixes + expansión resolución. Ninguno gana tracción real — el proyecto sin su autor carece de "alma".

### Lecciones TempleOS (la más amarga)

1. **Genialidad técnica sin community = muerte del proyecto con el autor**. Davis probablemente era el mejor hobbyist single-handed OS dev de la historia: kernel + compilador + IDE + graphics + FS + editor en 10 años él solo. **Pero sin comunidad el proyecto murió con él**.
2. **Purity / religiosidad / "mi visión única"** atrae curiosidad pero no contributors. La gente no quiere vivir en tu cosmología.
3. **Restrictions as identity**: 640×480, 16 colores, sin red, ring-0 — eran parte del atractivo cultural. Irónicamente Serenity también tiene restrictions (look-and-feel 90s, no JS en WindowServer, AK::) pero **enmarcadas como estilo, no como doctrina**.
4. **Mental health + hobby OS**: el proceso *puede* ser terapéutico (Kling también empezó Serenity post-rehab). Davis demuestra que sin red de apoyo profesional puede ser destructivo. ALZE no debe convertirse en refugio de aislamiento.

Primary refs:
- Terry A. Davis, *TempleOS documentation* (en-OS, accesible vía wayback).
- [TempleOS Holy documentation archive](https://templeos.holyc.xyz/).
- Jamie Zawinski, *"The strangest operating system I know"*, jwz.org, 2018. [jwz.org](https://www.jwz.org/blog/2018/08/terry-davis-rip/).
- VICE documentary, *"God's Lonely Programmer"*, 2014 — primer medio major que cubre Davis.

---

## 7. Oberon System — el OS más elegante jamás escrito

### Origen

Niklaus Wirth (Pascal, Modula) + Jürg Gutknecht. ETH Zurich 1987. Proyecto docente: ¿qué OS puede un equipo de 2 personas construir en 1 año?

- Wirth + Gutknecht, *The Oberon System*, **book**, 1992. Springer-Verlag.
- Revised: Wirth + Gutknecht, *Project Oberon: The Design of an Operating System, a Compiler, and a Computer*, Revised 2013 edition. PDF: [projectoberon.com](http://www.projectoberon.com/) / [inf.ethz.ch](https://people.inf.ethz.ch/wirth/ProjectOberon/). **Gratis en PDF**.

### Arquitectura

- Single-user workstation OS para estación Ceres (hardware ETH propio) y luego FPGA re-implementation (Project Oberon 2013 usa RISC custom en FPGA).
- **Todo el sistema en ~10 KLOC** de Oberon language. Kernel + FS + compilador + GUI + apps.
- **Texto como interfaz**: la pantalla es un *text viewer*. No "ventanas con botones" sino texto estructurado donde cualquier *word* puede ser activa (click → ejecuta comando). Ejemplo: documento con texto `Hello.World.Greet` → click derecho ejecuta `Hello.World.Greet`. El sistema no separa código y documento: ambos son texto viewable-editable.
- **Cooperative single-task** (single-threaded kernel); cada "comando" es una función que el viewer invoca.
- **Garbage-collected Oberon language** — Wirth baja el GC a nivel kernel para que todo sea memoria manejada. Sin manual `malloc/free` en el OS entero.
- FS propio jerárquico, sin paths absolutos complicados.

### Lecciones Oberon

1. **10 KLOC para un sistema completo** con GUI + compilador + apps: demuestra que la línea superior de "complejidad mínima viable" de un OS-with-GUI es ~10× menor que Unix. Se paga con: single-user, no protection, no network, no multi-window in the modern sense.
2. **Language-OS co-design**: al controlar *ambos*, Wirth elimina mucha ceremonia (sin ELF loader sofisticado; sin libc ceremonia; GC en lugar de malloc; type safety end-to-end). Lisp Machines / Smalltalk-80 / Oberon son los 3 ejemplos paradigmáticos.
3. **Texto como UI activa**: anticipa hyperlinks + REPL + notebooks (Jupyter) + Emacs Org mode. Modelo menospreciado. ALZE podría implementar un "text-as-UI" shell como experimento en userland sin comprometer el kernel.
4. **Docencia intensiva**: Oberon sigue enseñándose en ETH y en Checa/Estonia. Los libros PDF gratis son el asset más valioso del proyecto — las ideas se replicán solas porque son *legibles*.

Primary refs:
- N. Wirth + J. Gutknecht, *Project Oberon: The Design of an Operating System, a Compiler, and a Computer*, 1992 + revised 2013. [PDF libre](http://people.inf.ethz.ch/wirth/ProjectOberon/index.html).
- N. Wirth, *"Good Ideas, Through the Looking Glass"*, IEEE Computer, 2006. [DOI 10.1109/MC.2006.20](https://ieeexplore.ieee.org/document/1580377).
- [Oberon Community Platform / A2 Bluebottle](http://www.ocp.inf.ethz.ch/).

---

## 8. KolibriOS + MenuetOS — el culto al tamaño pequeño

### KolibriOS

- Fork open-source de MenuetOS 2004 (tras Ville Turjanmaa cerrar código MenuetOS). [kolibrios.org](http://kolibrios.org/en/).
- **100 % FASM assembly**. Sin una línea de C.
- Cabe en un floppy 1.44 MB: kernel + shell + ~20 apps (text editor, calc, game, paint).
- Multitasking, GUI con window manager propio, DOOM port, navegador web básico (HTTP sin HTTPS moderna), ~300 apps en el repo.
- **Activo en 2026**: commits ~3/semana, foro ruso activo, release `0.7.7.0.+svn` 2025. Pequeña pero persistente comunidad ex-URSS.

### MenuetOS (64)

- Ville Turjanmaa, Finlandia. [menuetos.net](http://www.menuetos.net/).
- MenuetOS (1) liberado open-source, MenuetOS64 closed-source.
- Todo en FASM x86_64. SMP-aware. GUI, navegador, SIMD showcase.
- **Closed-source** desde ~2005 — decisión polémica que fracturó la comunidad (resultado: Kolibri).
- Activo sporadic: releases 2022, 2024. Solo.

### Lecciones

1. **"Cabe en un floppy"** es un pitch con marketing intrínseco — cuantificable, romántico, antagonista directo de Windows/Linux bloat. ALZE podría adoptar algo análogo: "ALZE kernel fits in 1 MB binary", medido continuamente.
2. **Assembly-only** es autoimpuesto: limita a x86 (KolibriOS x86_64 port abandonado), slow porting, pero extremo control + placer estético. Identidad nicho-dura. No adoptable por ALZE (C base), pero el *principio*: tener una restricción identitaria cuantificable ayuda a comunicar.
3. **Closed-source (Menuet) vs open (Kolibri)**: el fork abierto ganó en community/longevidad; el cerrado sobrevive solo gracias a su autor. **Confirma regla**: hobbyist OS cerrado es casi siempre proyecto personal sin futuro tras el autor.

Primary refs:
- [KolibriOS project](http://kolibrios.org/en/).
- [MenuetOS project](http://www.menuetos.net/).
- A. Chetverikov et al., *"KolibriOS — a small operating system"*, various conferences. Community docs only.

---

## 9. osdev.org community — el commons hobbyist

[osdev.org](https://osdev.org/) + [wiki.osdev.org](https://wiki.osdev.org/) + [forum.osdev.org](https://forum.osdev.org/).

Desde ~2000, el **wiki + forum de facto** para hobbyist OS dev. Gestionado por voluntarios. Contenido:

- **Bare Bones tutorial**: de "hello world" kernel a protected mode en ~500 líneas. [wiki.osdev.org/Bare_Bones](https://wiki.osdev.org/Bare_Bones). Millones de lectores — el "getting started" canónico.
- **Meaty Skeleton**: next-level, multiboot + higher-half kernel + linker scripts.
- **Cientos de artículos**: IDT, GDT, paging, PMM, VMM, ELF, ACPI, APIC, keyboards, USB, FAT, ext2, etc.
- **Common pitfalls** page: inline assembly errors, build system errors, emulator-vs-real-hardware mismatches.
- Foro: moderadores experimentados (Brendan, Candy, Solar, Combuster históricos) responden preguntas; muchos hobbyist OSes pasaron por allí (Serenity en sus primeros meses, Redox temprano, ToaruOS).

### Lección osdev.org

- **El commons público** (wiki + foro) vale más que cualquier libro. No tiene dueño, no puede morir, ha acumulado 20 años de *rastrillo* — cada pitfall que encontró alguien está documentado. ALZE **debe** publicar sus discoveries en osdev wiki (cada bug raro, cada descubrimiento del hardware) → karma + descubribilidad + ayuda al próximo.
- Hobbyist OS que **no referencia osdev wiki** suele estar reinventando errores ya documentados.

### Pitfalls comunes documentados (extracto)

- Inline asm: olvidar `"memory"` clobber → compilador cachea valores obsoletos tras la instrucción.
- GDT load sin reload de segment registers con `ljmp` → tripfault en jump siguiente.
- IDT sin `set_gate` para #PF early → page fault durante boot dispara #DF luego #TF = reboot misterioso.
- Paging: olvidar identity-map las primeras páginas antes de `mov cr0, cr0 | PG` → instrucción siguiente ya no existe en virtual → halt.
- Emulators (qemu, bochs) perdonan errores que hardware real no (unaligned access, uninitialized segments). Testear en real hardware temprano.

Primary refs:
- [osdev.org wiki](https://wiki.osdev.org/Main_Page).
- [osdev.org forum](https://forum.osdev.org/).
- J. Bos (BrightCode), [*"Writing an OS from Scratch"* tutorial series](https://cfenollosa.com/blog/writing-a-simple-operating-system-from-scratch.html) — referencia complementaria 2017-.

---

## 10. ReactOS, SkyOS y otros — brief

### ReactOS

Ya tratado en `otros.md` (R1). Brief recap:
- **Binary-compatible Windows NT clone**. Comparte código userland con Wine.
- ~30 años activo (1996–). 2026: "sync with Wine 10.0" reportado por Phoronix como **major step toward NT6** (−30 % fallos tests). [phoronix.com/ReactOS-Starts-2026](https://www.phoronix.com/news/ReactOS-Starts-2026).
- Vivo pero lento. Comparable a WINE en el sentido de que perseguir una superficie API cerrada es trabajo de Sísifo.

### SkyOS (muerto)

- Robert Szeleney, ~2003–2009. Desktop OS propietario con **SkyFS** (fork BeOS BFS).
- GUI pulido, Mozilla port. Community grande brevemente.
- Abandonado por Szeleney sin transición open-source. Código perdido.
- **Lección**: OS cerrado de un solo autor sin plan de sucesión **muere con el cansancio del autor**. Replica de TempleOS sin el drama.

### Syllable / AtheOS

- Kurt Skauen AtheOS 1996–. Multithreaded desktop OS, inspirado AmigaOS.
- Fork Syllable 2002 por Kristian Van Der Vliet. Desktop (Syllable Desktop) + server (Syllable Server).
- Último commit útil ~2012. Website todavía up, proyecto no.

### Plan 9 / 9front

(Tratado en `r4/plan9_inferno.md`, no repito). Mencionar: 9front sigue activo (release cada ~6 meses), comunidad pequeña pero constante. Caso análogo a HelenOS: substrato de research.

### House (Haskell)

- Galois Inc. ~2005. Kernel escrito en Haskell.
- Demostró que sí se puede; nadie lo continuó.
- **Lección**: lenguaje exótico atrae una vez y no retiene contribuidores que no son expertos del lenguaje.

---

## 11. Lessons for ALZE OS — consolidado

### Lecciones de proceso (las más importantes)

#### (a) Narrow scope, explícito y publicado

- **SerenityOS**: "Unix desktop from scratch, 90s aesthetic". NO "reinventar nada". La restricción *es* el valor.
- **ToaruOS**: "hobbyist Unix + Yutani compositor". Lange nunca promete más.
- **TempleOS**: 640×480 × 16 colors × ring-0 — una pitch restrictiva memorable.
- **KolibriOS**: "cabe en floppy".

**ALZE acción**: escribir en README.md una línea de 1-frase que diga exactamente qué *es* y qué *no será jamás*. Ej: "ALZE OS: capability-based educational kernel in C, targeting x86_64 and riscv64, with a textual-UI shell. Not a Linux replacement. Not a production OS."

#### (b) Community engagement > code quality (doloroso pero real)

- Kling + YouTube + Discord = 1,400 contributors.
- Lange sin presencia pública = 15 años solo.
- Davis en ring-0 + broadcasting = proyecto muerto tras autor.
- Tanenbaum académico + paper = adoption silenciosa (Intel ME) pero sin comunidad.

**ALZE acción**: un canal de comunicación consistente. Recomendado: **blog post mensual + demo video trimestral**. Formato Kling: screen-recorded hack sesión, music lofi, 30 min, subida y olvida. 12 videos/año × 2 años = 24 entry points para que alguien descubra el proyecto. Cost: ~40 h/año de grabación; ROI: probablemente más PRs que cualquier optimización de código.

#### (c) Single-person longevity es posible pero sólo con scope fijo

- **ToaruOS — 15 años solo** pero scope congelado.
- **TempleOS — 15 años solo** pero murió con el autor.
- **Solo-devs que murieron jóvenes**: decenas de hobby OSes en github con ~2 años de commits y luego silencio.

**ALZE acción**: si ALZE va a ser solo-dev, escribir NOW un doc "scope hard-lock": lo que nunca se hará (Wi-Fi, GPU nativa, Bluetooth, navegador, etc.). Quitar la presión de "ser completo".

#### (d) Don't target "beat Linux" — target fun + learning

- **HelenOS**: "we're an educational research OS, period" → sobrevive 25 años.
- **Minix 3**: "we'll beat Linux in reliability" → muerto en 2018.
- **ReactOS**: "we'll replace Windows" → 30 años tras Windows, nunca llegará.
- **SerenityOS**: "we'll be the OS Kling wants to use" → 1,400 contributors.

**ALZE acción**: el pitch debe ser **interno** (qué aprendo / quién se une) no **externo** (a qué OS sustituyo). Copia la frase Kling: "the OS I want to use".

#### (e) Community > code after author dies

- TempleOS + SkyOS + AtheOS: code brillante + autor ausente = muerto.
- SerenityOS + HelenOS: code mediocre + comunidad = viva.
- **Preventivo**: ALZE debe tener al menos 3 committers activos antes de año 2. Si llega al año 2 con un solo committer, declarar proyecto personal + no esperar longevity.

#### (f) Ship demo videos trimestrales — regla de oro Kling

- 2019: 12 videos → 30 committers.
- 2022: 100+ videos → 500 committers acumulados.
- El video es **el mejor on-ramp posible** para un hobbyist OS: resuelve el "¿por dónde empiezo?" de un curioso.
- Contrast: ToaruOS sin videos → 15 años solo, 0 fanbase amplia.

### Lecciones técnicas (menores comparadas con las de proceso)

1. **Text-as-UI shell** (Oberon): un experimento barato en userland que puede dar identidad. ALZE userland puede tener su propio "active text viewer" — ~2 semanas de trabajo.
2. **Driver restart** (Minix 3 + HelenOS): no requiere microkernel puro — cualquier kernel con **userland servers + supervisor** puede hacerlo. Si ALZE adopta capability model, es natural.
3. **Port de DOOM como KPI** (Serenity + Toaru + Kolibri): cuando tu OS corre DOOM es *el* hito reconocible para el exterior. Usar como milestone de marketing.
4. **Compilador self-host** (Serenity 2021, Oberon from day 1): hito narrativo enorme. Duplica el LOC del proyecto pero multiplica la gravitas. ALZE: autoidentificar cuándo gcc/clang port sería realista.
5. **osdev wiki contributions**: cada descubrimiento raro de ALZE debe volver al wiki. Barato, alto karma.

---

## 12. ALZE applicability — tabla cruda

| Lección | Origen | Acción ALZE concreta | Esfuerzo | ROI |
|---|---|---|---|---|
| Narrow scope pitch en README | Serenity + Toaru + Kolibri | 1 frase "qué es / qué no será" en README.md | 1 h | Alto (claridad) |
| Blog mensual | Serenity + osdev community | Set up blog estático (zola/hugo), 1 post/mes | 2 h/mes | Medio (SEO + recruit) |
| Demo video trimestral | Kling | 30 min screencast, subir a YouTube | 4 h × 4 = 16 h/año | Alto (recruit multiplicador) |
| Scope hard-lock doc | ToaruOS lesson | `SCOPE.md`: features que NUNCA se harán | 2 h | Alto (sanity-check) |
| osdev wiki edits | osdev commons | Un edit/mes con algo que encontraste | 30 min/mes | Medio (karma + SEO) |
| Port DOOM como KPI | Serenity + Toaru + Kolibri | Tarea en roadmap: year-2 milestone | N/A (es goal) | Alto (hito narrativo) |
| 3 committers año 2 | SerenityOS community-building | Target explícito: si < 3 committers activos año 2, revalorar scope | 0 h | Alto (early-warning) |
| Driver-restart supervisor userland | Minix 3 + HelenOS | Incluir supervisor en la lista de subsystems v2 | N/A (design-time) | Alto (reliability feature) |
| Text-as-UI shell experimento | Oberon | Userland app ~2 semanas | 40 h | Medio (identity) |
| gcc/clang self-host | Serenity 2021 + Oberon | Milestone year-3 (solo tras IPC estable) | 100+ h | Alto (gravitas) |
| Discord + forum | Serenity | Abrir Discord cuando haya primer contributor externo | 1 h | Alto (community hub) |
| Kling-style YT screencast | Serenity | Primer video cuando kernel boot + print "hello" | 4 h | Alto (first-move signal) |

---

## 13. Honest note — the SerenityOS + TempleOS lesson

> SerenityOS es el caso canónico de hobbyist-OS-done-right 2020s. Andreas Kling construyó un Unix-clone sin innovación arquitectónica que atrajo 1,400 contributors en 6 años. Tres ingredientes: **(1) narrow focus** (Unix clone + 90s aesthetic, nada más); **(2) community engagement** (~1,000 horas de YouTube + Discord activo + blog); **(3) culture of craft** ("pretty + fast" como mantra, code review exigente, AK:: style guide no-negociable). Técnicamente Serenity **no innova nada** — ext2, monolithic kernel, POSIX, WindowServer similar a early X11/Quartz. La innovación es **proceso**, no código.

> En el otro extremo, TempleOS prueba la versión oscura: técnicamente más brillante que muchos proyectos "exitosos" (kernel + compilador + graphics + IDE en 100 KLOC solo), pero sin comunidad = muerto con su autor. Terry Davis era probablemente el mejor solo-OS-dev de la historia. Su OS sobrevive como museo, no como ecosistema.

> **ALZE podría elegir UN ingrediente de Kling (narrow scope / YouTube / craft culture) y superar a gemios técnicos solos**. El tramo más barato y con mayor ROI: **demo video trimestral**. Un video de 30 min cada 3 meses = 2 horas/mes de esfuerzo, 4 videos/año, 8 videos en 2 años. Ese es el ancho de banda mínimo a partir del cual Kling empezó a capturar committers.

> **Ship demo videos. Community > code.** Si ALZE nunca publica un video ni un blog post, será ToaruOS en el mejor caso (15 años solo) o Syllable en el peor (5 años y abandono). Si publica videos consistentes, tiene la chance realista de ser el próximo Serenity.

> Complementario, la advertencia HelenOS: si no hay ambición de YouTube/community, alinearse con una institución educativa da 20+ años de vida silenciosa. La muerte hobbyist es el punto medio: demasiado ambicioso para ser solo, demasiado solo para ser institución.

---

## Fuentes consultadas (master list)

- A. Kling, *SerenityOS* project (2018-). [serenityos.org](https://serenityos.org/) / [github](https://github.com/SerenityOS/serenity).
- A. Kling, YouTube [@awesomekling](https://www.youtube.com/@awesomekling), 2019-.
- Ladybird Browser Initiative, 2024-. [ladybird.org](https://ladybird.org/).
- C. Wanstrath, *"Why I'm funding Ladybird"*, 2024. [chriswanstrath.com](https://chriswanstrath.com/2024/07/ladybird).
- K. Lange, *ToaruOS* (2011-). [toaruos.org](https://toaruos.org/) / [github](https://github.com/klange/toaruos).
- HelenOS project. [helenos.org](http://www.helenos.org/) / [book](http://www.helenos.org/book/).
- J.N. Herder, H. Bos, B. Gras, P. Homburg, A.S. Tanenbaum, *"MINIX 3: A Highly Reliable, Self-Repairing Operating System"*, SIGOPS 2006. [DOI](https://dl.acm.org/doi/10.1145/1151374.1151379).
- A.S. Tanenbaum + A. Woodhull, *Operating Systems: Design and Implementation*, 3rd ed, Pearson 2006.
- Tanenbaum–Torvalds debate archive. [oreilly.com](https://www.oreilly.com/openbook/opensources/book/appa.html).
- T.A. Davis, *TempleOS documentation*. Archive: [templeos.holyc.xyz](https://templeos.holyc.xyz/) + [wayback](https://web.archive.org/web/2018*/https://www.templeos.org/).
- J. Zawinski, *"The strangest operating system I know"*, 2018. [jwz.org](https://www.jwz.org/blog/2018/08/terry-davis-rip/).
- N. Wirth + J. Gutknecht, *Project Oberon*, 1992 + Revised 2013. [PDF free](http://people.inf.ethz.ch/wirth/ProjectOberon/index.html).
- N. Wirth, *"Good Ideas, Through the Looking Glass"*, IEEE Computer 2006. [DOI 10.1109/MC.2006.20](https://ieeexplore.ieee.org/document/1580377).
- [KolibriOS project](http://kolibrios.org/en/).
- [MenuetOS project](http://www.menuetos.net/).
- [osdev.org wiki](https://wiki.osdev.org/Main_Page) / [forum](https://forum.osdev.org/).
- ReactOS project. [reactos.org](https://reactos.org/). [Phoronix 2026 status](https://www.phoronix.com/news/ReactOS-Starts-2026).
- C. Fenollosa, *"Writing a simple operating system from scratch"*, tutorial 2017-. [cfenollosa.com](https://cfenollosa.com/blog/writing-a-simple-operating-system-from-scratch.html).
