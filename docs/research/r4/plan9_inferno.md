# Plan 9 + Inferno + 9front — deep dive for ALZE OS

**Round:** R4, agente 6 / 7
**Fecha:** 2026-04-22
**Scope:** Plan 9 from Bell Labs (1984-2015 Lucent/Bell Labs lineage), Inferno OS (Vita Nuova, 1996+), 9front (active fork, 2011+). R1 sólo rozó el tema dentro de `otros.md` (mención a "Redox extiende everything-is-a-file"); este doc cubre el canon completo.

---

## 1. Historia — del 10th Edition Unix a Lucent, vx32 y 9front

Plan 9 nace en el **Computer Science Research Center (CSRC) de Bell Labs** entre 1984 y 1989 como respuesta interna a la fragmentación de Unix. El equipo núcleo es exactamente el mismo que hizo Unix una década antes: **Ken Thompson, Rob Pike, Dennis Ritchie, Dave Presotto, Phil Winterbottom**, con **Howard Trickey** y **Tom Duff** en periferia. El nombre es broma de Ed Wood ("Plan 9 from Outer Space", 1959); la ambición interna era construir **el sucesor espiritual de Unix**, no un Unix compatible.

Motivaciones originales, tomadas de Pike-Presotto-Thompson-Trickey-Winterbottom 1990:

1. Unix cuando se diseñó (1970) asumía un PDP mini con varios usuarios en terminales seriales. En 1985 la realidad era estaciones de trabajo con red + servidor compartido. Unix nunca integró la red como **primera clase**; NFS, X11, rpc.* son parches ad-hoc sin semántica común.
2. Unix promete "everything is a file" pero lo rompe con sockets, ioctl, shmem, sysv msg queues, named pipes, devfs no uniforme, procfs ad-hoc, etc. Plan 9 intentaría **aplicar la promesa en serio**.
3. La multi-máquina debía ser la unidad de trabajo, no la excepción. `cpu(1)` en Plan 9 te transporta tu terminal a otra máquina — tu shell, tu `$home`, tu editor continúan, ahora ejecutando en el CPU server.

**Cuatro distribuciones oficiales de Bell Labs:**

| Edición | Año | Notas |
|---|---|---|
| 1st Edition | 1992 | Primera distribución comercial a universidades. Licencia restrictiva. |
| 2nd Edition | 1995 | Plan B de 9P (9P2000 aún no). Alef lang todavía vivo. |
| 3rd Edition | 2000 | Lucent publica bajo **Lucent Public License**. Software libre por primera vez. |
| 4th Edition | 2002 | 9P2000, Limbo reabsorbido, C Alef reemplazado por **thread(2)** en C. CD distribuído + snapshot continuous bootstrap. |

En **2014** el código oficial del 4th Edition pasa a **MIT License** (donado al equipo de *Plan 9 Foundation*), desbloqueando el fork legal. Bell Labs publica su último snapshot upstream aprox. **2015**; desde entonces, **9front** (fork comunitario, ver §11) es de facto el Plan 9 vivo.

**Inferno OS** (§10) es el spinoff comercial: Vita Nuova Holdings licencia Plan 9 a finales de 1996, empaqueta el subset orientado a aplicaciones embebidas/portables, y lanza Inferno en 1997. Re-licencia a GPL en 2005.

**Emuladores relevantes** para correr Plan 9 hoy en Linux/macOS/Windows:
- **drawterm** — cliente remoto que habla 9P a un CPU server Plan 9; tu terminal local en Linux hablando el protocolo hacia una instancia Plan 9 real o virtual.
- **vx32 / 9vx** (Russ Cox, 2008) — sandbox x86 user-mode que ejecuta binarios Plan 9 nativamente sobre Linux/BSD. Esencialmente un "WASM antes del WASM" para ISA x86-32.
- **p9p (Plan 9 from User Space)** — port del **userland** de Plan 9 (acme, rc, sam, mk, 9P library) a POSIX. `plan9port` de Russ Cox. Usado masivamente por hackers que sólo quieren `acme` + `sam` + `mk` en macOS/Linux.
- **QEMU/KVM** booteando 9front como guest — hoy es la ruta mainstream para desarrollo.

---

## 2. Filosofía central — "everything is a file server"

La línea Unix es "everything is a file". La línea Plan 9 es **"everything is a file *server*, and file servers talk 9P"**. Diferencia no trivial:

- En Unix, `open("/dev/null")` llega al VFS → driver char específico. Los drivers comparten tipo (file operations struct) pero no protocolo.
- En Plan 9, `open("/net/tcp/clone")` llega al VFS → el VFS envía un mensaje **Twalk/Topen 9P** sobre un canal. El canal puede ser:
  - local, hacia un driver kernel que implementa 9P (p.ej. `#l` la interfaz ethernet)
  - local, hacia un **proceso userland** que implementa 9P (p.ej. `upasfs` el mail fs)
  - remoto, a través de TCP a otra máquina (p.ej. un file server en otro edificio)

El kernel **no sabe** qué hay al otro lado del canal: sólo sabe dirigir 9P. El resultado: **el mismo código de aplicación funciona sobre recursos locales y remotos**, sin distribución transparente mágica como NFS — es la **propia API la que es de red por construcción**.

Todo en Plan 9 es un file server que habla 9P:

- **Devices**: `#c` console, `#e` env, `#i` image, `#l` ether, `#m` mouse, `#s` pipe, `#t` serial, `#A` audio, `#S` sd (storage). El device driver es un componente del kernel que implementa 9P ops.
- **/proc**: cada proceso es un directorio con files `ctl`, `mem`, `status`, `note`, `regs`, `fd`, `wait`. Debugger (`acid`) no usa ptrace: abre `/proc/42/ctl` y escribe `waitstop`.
- **/net**: pila TCP/IP expuesta como filesystem. Conectar a una web: `open("/net/tcp/clone")` devuelve un dir `/net/tcp/N/` con `ctl`, `data`, `status`, `listen`, `local`, `remote`. Escribir `"connect 1.2.3.4!80"` a `ctl`; leer/escribir a `data` es la TCP stream.
- **/mnt/wsys** (Rio): ventanas. Abrir `/mnt/wsys/new/` crea una ventana nueva; tu proceso hereda esa ventana.
- **/env**: variables de entorno como files (`/env/PATH`).
- **/dev/draw**: subsistema de rendering (ver §5).

El **factorizado conceptual** es brutal: una primitiva (9P), una interfaz (file ops), y la distribución, el sandboxing, la virtualización y el debugging caen como casos particulares.

---

## 3. 9P protocol — RPC minimalista, stateful, message-based

**9P2000** es la revisión cuajada (4th Edition, 2002). Mensajes tipados, binarios, sobre cualquier transporte reliable (TCP, pipe, memoria). Cada mensaje lleva header fijo: `size[4] type[1] tag[2]` + payload específico. **Tag** es el ID de request para pipelining; el server responde con el mismo tag. **Fid** es un file-descriptor-handle server-side, negociado por el cliente.

**Mensajes (14 tipos, pares T/R)**:

| T-msg | Payload | Propósito |
|---|---|---|
| `Tversion` | `msize, version` | Handshake, negociar tamaño máximo de mensaje. |
| `Tauth` | `afid, uname, aname` | Handle de autenticación (delegado a factotum, §7). |
| `Tattach` | `fid, afid, uname, aname` | Abrir una "session" sobre el root del fs, ligando `fid` al root. |
| `Twalk` | `fid, newfid, wname[nwname]` | Navegar por path desde `fid`. Equivalente a `dirfd + openat`. |
| `Topen` | `fid, mode` | Abrir file para lectura/escritura. |
| `Tcreate` | `fid, name, perm, mode` | Crear file. |
| `Tread` | `fid, offset, count` | Leer bytes. |
| `Twrite` | `fid, offset, data` | Escribir bytes. |
| `Tclunk` | `fid` | Liberar fid (no es `close` — el file sigue si otros fids lo refieren). |
| `Tremove` | `fid` | Borrar + clunk. |
| `Tstat` | `fid` | Metadata (análogo a stat). |
| `Twstat` | `fid, stat` | Actualizar metadata (rename + chmod + chown + utime en uno). |
| `Tflush` | `oldtag` | Cancelar un mensaje pendiente. |
| `Terror` | — | **No existe** — el server responde `Rerror{ename}` ante cualquier T-msg. |

Total: **13 operaciones de cliente**, más versión y auth. La librería cliente de Plan 9 (`lib9p` en C, `styx` en Limbo) es **~1000 LOC**. Escribir un server nuevo (ej. un fs que expone tu GPS como `/mnt/gps/position`) son ~300 LOC en C usando `lib9p`.

**Propiedades no triviales:**

- **Stateful**: el servidor mantiene fids (similar a Windows handles, no a Unix fds puros). Permite cache eficiente y cierre correcto.
- **Connection-oriented**: una connection = un namespace abierto; perder la conexión = perder los fids.
- **Out-of-order pipelining**: múltiples T-msgs en vuelo distinguidos por tag. Latencia LAN no bloquea throughput.
- **Sin file types especiales**: file ordinario, directorio, **append-only**, **exclusive-open**, y **mount point**. Los dirs se leen con `Tread` — devuelven entradas stat concatenadas. No hay `readdir` separado.
- **Goroutine-friendly antes de Go**: la librería Plan 9 de servidores usa **1 thread por request** con `channels` (Alef/Limbo/thread(2)) para coordinación. Esto es exactamente el modelo que Pike luego plasmó en Go (§12).

Transportes oficiales:
- **TCP** puerto 564 — el default.
- **Unix pipes** — via kernel local path.
- **Serial/IL** — Plan 9 tenía un protocolo propio, *IL* (Internet Link), reliable datagram lite sobre IP, diseñado para 9P. Deprecado en favor de TCP tras 4th Edition.
- **VirtIO 9P / virtio-9p (`9pfs`)** — Linux + KVM usan 9P para share folders host↔guest. `-virtfs local,path=/foo,mount_tag=hostshare`. Demostración canónica de que 9P sobrevive fuera de Plan 9.

---

## 4. Namespaces per process — `bind`, `mount`, `rfork`

En Unix, `/` es global: los procesos comparten el árbol. Plan 9 **no tiene `/` global**. Cada proceso tiene su propia tabla de mount (**namespace**) heredada del padre con CoW. Cambiar el propio namespace **no afecta al padre**.

**Syscalls clave**:

- `bind(old, new, flag)` — monta `old` *encima* de `new` en tu namespace. `flag` controla si es replace (**`MREPL`**), before (**`MBEFORE`**, union mount), after (**`MAFTER`**), o create-allowed (**`MCREATE`**).
- `mount(fd, afd, dst, flag, aname)` — monta el 9P server conectado al file-descriptor `fd` en el path `dst`.
- `rfork(flags)` — primitiva unificada que incluye namespace semantics. Flags: `RFNAMEG` (copy-on-write del namespace), `RFFDG` (copy fd table), `RFPROC` (new process), `RFMEM` (shared memory), `RFNOMNT` (no further mounts allowed).

**Union directories** via `bind` con `MBEFORE/MAFTER`:
```
bind -a /usr/local/bin /bin       # añade /usr/local/bin al final de /bin
bind -b /home/me/bin /bin         # añade /home/me/bin al principio de /bin
```
Los dirs se **unifican**: `ls /bin` muestra la unión. No hay `$PATH` — lo reemplaza el namespace.

**Ejemplo práctico**: sandbox de un proceso con solo lo que necesita:
```
rfork n                 # nuevo namespace privado
unmount /                # vacío
bind '#c' /dev          # solo consola
bind /bin /bin          # solo binarios
bind /tmp /tmp          # scratch
exec /bin/suspect
```
Esto es un **chroot + mount namespace + seccomp light** pre-construidos en el propio modelo del OS, sin syscalls añadidas.

**cpu(1)** (transporte de sesión a otra máquina) usa exactamente esto: el cliente monta su propio namespace (incluyendo su `/mnt/term` con su terminal local) dentro del contexto del proceso remoto. El "ssh de Plan 9" no copia archivos — proyecta tu namespace.

---

## 5. Rio — windowing como filesystem

**Rio** es el window system. Reemplaza a **8½** (el anterior) en 4th Edition. Expone el servicio ventanas como `/mnt/wsys/`:

- `/mnt/wsys/new/` — crea ventana nueva; lees/escribes a `cons` como stdin/stdout.
- `/mnt/wsys/N/cons` — consola (stdin/stdout de esa ventana).
- `/mnt/wsys/N/mouse` — stream de eventos de mouse (x y buttons ms).
- `/mnt/wsys/N/kbd` — eventos teclado raw.
- `/mnt/wsys/N/wctl` — ctl para resize/move/hide.
- `/mnt/wsys/N/snarf` — clipboard.
- `/mnt/wsys/N/label` — título.

**Rendering**: Rio delega en `/dev/draw`, un filesystem expuesto por el kernel (!) que implementa el modelo **Draw** (Pike-Newton): rectángulos, colors, un operador de compositing (la famosa `draw(5)` man page). No hay GPU ni OpenGL. Escribir "`r src dst mask rect`" a `/dev/draw/N/data` pinta pixels. Fonts `.subfont` separadas.

El resultado UX es minimalista: no decoraciones de ventana reales, sin iconos, sin drag-and-drop nativo al estilo moderno. Pero **cada ventana es componible vía files**: un script puede pintar/leer input de cualquier ventana sin librerías de widgets — abre files.

---

## 6. Acme editor — files as UI

**Acme** (Rob Pike, 1993-1994, paper "Acme: A User Interface for Programmers"). Editor de texto donde **cualquier texto es un comando ejecutable**. La convención: **botón izquierdo selecciona**, **botón medio ejecuta comando**, **botón derecho busca/navega**.

La interfaz de programación de acme es un **filesystem 9P** (accesible como `/mnt/acme/`):

- `/mnt/acme/N/body` — el texto del buffer.
- `/mnt/acme/N/tag` — la barra de título (que también es editable).
- `/mnt/acme/N/data` — stream leído/escrito en seek-position actual.
- `/mnt/acme/N/ctl` — comandos de control (dirty, mark, dump).
- `/mnt/acme/N/event` — stream de eventos (clicks, selects) para plugins.

Esto significa que **extender acme no requiere plugin system**: cualquier script que hable 9P a `/mnt/acme/` puede leer/escribir buffers, reaccionar a eventos, crear ventanas. `acme.el`, `vim`, `VSCode` inventan extension APIs; acme **no necesita una porque ya la tiene**: 9P.

Combinaciones clásicas: pipeline `cat file.go | gofmt | acme-replace-body`, refactor "rename symbol" como script que habla al fs de acme.

---

## 7. Factotum — un auth agent para todo

**Factotum** (Cox-Grosse-Pike-Presotto-Quinlan 2002) es el agente de autenticación universal. Corre en user-land, expone `/mnt/factotum/` vía 9P. Cuando cualquier proceso necesita auth (p.ej. conectar a un file server remoto, autenticar SSH, TLS, Kerberos, POP3, SMTP), abre `/mnt/factotum/rpc` y ejecuta un protocolo RPC simple.

Claves: **los programas nunca ven las credentials**. Factotum tiene las keys; el programa sólo obtiene el resultado del handshake. Llega antes que el SSH-agent como concepto unificado; a diferencia del agent, **no es específico de SSH** — sirve para 9P auth, TLS client certs, PAP/CHAP, HTTP digest, Kerberos.

Paper canónico: Cox et al "Security in Plan 9", USENIX 2002. Una idea resumen: **secoff** (secrets officer) + **factotum** = un *capability broker* que media el acceso a credenciales, análogo moderno a HashiCorp Vault pero en 1990-something LOC integrado al OS.

---

## 8. Plan 9 from Bell Labs CD — la distribución real

La 4ª edición se distribuía como ISO booteable. Contenido típico:
- **kernel** — monolítico, ~300 KLOC C.
- **rc** — shell reemplazo de sh, con listas como tipo de datos (no strings).
- **sam + acme** — editores.
- **mk** — reemplazo de make con dependencias por regex y paralelismo inteligente.
- **venti + fossil** — backing store archival + filesystem activo. Venti: content-addressable immutable store (SHA1 chunks); Fossil: snapshotting FS on top, hace snapshots cada noche a Venti.
- **cpu, import, export** — comandos de transporte de namespace / mount remoto.
- **netaudit, netstat** — comandos que leen `/net` + formato.
- **factotum, secstore** — auth.
- **webfs** — HTTP como filesystem (`/mnt/web`).
- **upas** — mail server + mail fs.
- **rio + 8½** — window systems.

Tooling hoy: **`9` command** en plan9port wraps binarios para ejecutar en POSIX (`9 mk`, `9 acme`). Russ Cox mantiene esto activamente (último release p9p Feb 2025 aprox.).

---

## 9. Venti + Fossil — archival storage + snapshots

Merece sección propia porque es anticipo directo de ZFS/IPFS/restic:

- **Venti** (Quinlan-Dorward 2002, "Venti: a new approach to archival storage", FAST '02): content-addressable block store. Cada bloque ~8KB se identifica por **SHA-1(content)**. Write-once: escrito una vez, inmutable para siempre. Dedup automático (si ya existe ese hash, no se reescribe). El Venti server expone un protocolo simple sobre TCP (`Tread {score}` → block).
- **Fossil** — FS activo COW sobre disco local. Snapshots automáticos diarios. Cada snapshot serializa el árbol en bloques Venti y conserva solo el score raíz. Restaurar snapshot = montar `/n/dump/2024/0101/`.

Esto es **git-content-addressable** + **snapshotting CoW FS** + **dedup** antes de 2005. ZFS dedup (2010) y BorgBackup/restic (2015+) son reinvención. IPFS (2015) toma el patrón Venti literalmente y lo globaliza con DHT.

---

## 10. Inferno OS — portable, Limbo, Dis VM

**Inferno** (Vita Nuova 1996+, fork de Plan 9 con objetivo comercial embedded/appliance). Diferencias cardinales respecto a Plan 9:

1. **Kernel portátil** — corre **hosted** (como proceso en Linux/Windows/macOS/Plan 9) o **native** (bare-metal en x86, ARM, MIPS, PowerPC, SPARC). Un único codebase, abstracción HAL ligera. Más portable que Plan 9 oficial.
2. **Lenguaje Limbo** (Dorward-Pike-Winterbottom 1997). Tipado estático, garbage-collected, modular, con **channels** y **spawn** (concurrencia goroutine-style). Referenced types + value types. Limbo **es el ancestro directo de Go** — Pike y Thompson reciclaron casi entera la sintaxis + `chan` + `:=` + short declaration + packages.
3. **Dis VM** — Limbo compila a **Dis**, VM register-based (no stack-based como JVM). Instruction set pequeño (~80 opcodes). Bytecode portátil; la misma app Limbo corre en cualquier host.
4. **Styx** — el protocolo equivalente a 9P en Inferno, rebautizado. Convergió con 9P2000 en 2005 — hoy son básicamente el mismo wire format.
5. **Namespace per module** — extensión natural del namespace per process: cada módulo Limbo tiene su propia vista del filesystem.

**Uso comercial**: set-top boxes Philips, routers Lucent, appliances. Aqua (fabricante brasileño) corrió Inferno en dispositivos embebidos hasta bien entrados los 2010s. **Estado 2026**: Vita Nuova sigue existiendo pero el repo activo está en `github.com/inferno-os/inferno-os` (mirror GPL'd). Actividad baja pero presente: últimas commits ~2024 (bugfixes comunitarios). Es más "preservación" que desarrollo activo.

**Relación con 9front**: comunidades separadas pero amigas. 9front es Plan 9 nativo sobre hardware moderno; Inferno es VM sobre cualquier host.

---

## 11. 9front — el Plan 9 activo (2011+)

**9front** (pronunciado "nine-front") es el fork comunitario activo, iniciado ~2011 tras el estancamiento de Bell Labs. Dominios: `9front.org`, git en `git.9front.org` (también mirror `github.com/9front/9front`). Comunidad alrededor de IRC (`#cat-v` en Libera.Chat) y un canal Matrix. Humor negro característico: releases con nombres como "DO NOT INSTALL" y arte ascii gatuno.

**Qué trae 9front vs Plan 9 oficial**:

- **UEFI boot** — Plan 9 oficial solo BIOS. 9front boota UEFI con `9boot`.
- **WiFi modernos** — drivers iwl (Intel), rt*, etc. Plan 9 oficial no tenía WiFi decente.
- **x86_64** — 9front tiene port amd64 estable (Plan 9 oficial se quedó en 386 + amd64 experimental).
- **NVMe, AHCI, GPT** — drivers modernos.
- **USB3 (xHCI)**.
- **Audio drivers** HDA, USB audio.
- **CWFS + HJFS** — reemplaza Fossil+Venti (que tenían bugs no resueltos). CWFS = "Cached Worm FS", más simple, más robusto. HJFS = alternative para instalaciones single-disk.
- **rc + mothra + gefs** — nuevas tools. **gefs** (2023) es un FS B-tree moderno escrito para 9front, intentando suceder a cwfs.
- **git9** — cliente git nativo Plan 9, sin depender de software POSIX.

Actividad en 2026: releases bimestrales aproximadamente. Dev activo: **Jacob Moody (moody), Ori Bernstein (ori), cinap_lenrek** entre otros. Papers ocasionales en `man.cat-v.org`. Política de release explícita: "works on my thinkpad" — no hay CI-as-gate, sólo merge manual.

**9legacy** — otro fork más conservador, mantiene compatibilidad estricta con Bell Labs 4e. Menor actividad.

---

## 12. Influencia de Plan 9 en Go

**Go** (Griesemer-Pike-Thompson, 2007-2009, public release 2009) es literalmente un nieto de Plan 9. Mapping directo:

| Plan 9 / Limbo construct | Go equivalent |
|---|---|
| Channels (Limbo `chan of T`) | `chan T` |
| `spawn fn(args)` en Limbo | `go fn(args)` |
| `alt { ... }` en Limbo | `select { ... }` |
| Plan 9 `thread(2)` con `sendp/recvp` | `ch <- v` / `v := <-ch` |
| 9P "1 thread per request" servers | goroutine per connection |
| Plan 9 module system (implicit linking) | Go packages |
| `:=` short var declaration | `:=` (copiado literal de Limbo) |
| Plan 9 `errstr` por syscall | `err` as second return value |
| `rfork(RFMEM)` — light threads | goroutines scheduled on M:N |
| Plan 9 C compilers (`8c`, `6c`, `5c`) | Go toolchain (`go build` con backends derivados) |

Los primeros backends de Go (`6g`, `8g`) son **derivados directos** de los compiladores Plan 9 C de Ken Thompson (ese es el origen de la curiosa AT&T asm syntax modificada que Go usa internamente). El linker `6l` es Plan 9 `ld` reescrito. Sólo en Go 1.5 (2015) el runtime + compilador se auto-hostearon en Go, pero la estructura de directorios y el estilo de build siguen siendo muy Plan 9.

**net/http** — interfaz universal `Handler interface { ServeHTTP(w, r) }` es **exactamente el espíritu 9P**: un único tipo de contrato sobre el cual componer servicios. `http.FileServer`, `http.StripPrefix`, `http.HandlerFunc` son el equivalente userland de componentes 9P.

**io.Reader / io.Writer** — la primitiva "todo es un stream de bytes" es la versión Go del "todo habla 9P read/write".

**context.Context** — cancelación propagada = Plan 9 `Tflush` generalizado.

Pike lo dice él mismo en la charla *Go at Google: Language Design in the Service of Software Engineering* (SPLASH 2012): "Go descends from Plan 9. Almost every decision has Plan 9 roots, filtered through Limbo".

---

## 13. Influencia de Plan 9 fuera de Go

- **FUSE (Linux, Szeredi 2003-2005)** — exactly "9P in a different dress". FUSE kernel module recibe VFS ops, las reenvía a un proceso userland vía device `/dev/fuse`. Diferencia de 9P: FUSE es *local-only* por diseño, no tiene wire format de red estandarizado. Ganó porque Linux nunca fue a aceptar 9P en mainline en 2002, pero el kernel aceptó un wrapper local.
- **Docker volumes / namespaces / overlayfs** — Linux mount namespaces (`CLONE_NEWNS`, 2002) son **namespace-per-process a la Plan 9** simplificados y menos integrados. Docker 2013 construye sobre eso. OverlayFS es union mount (cf. `bind -a`).
- **Kubernetes service discovery vía DNS + `/etc/hosts` kube-proxy** — el patrón "todo servicio accesible por un nombre en un namespace" es Plan 9 `/net/` generalizado.
- **`/proc/<pid>/*` en Linux** — copiado literal del `/proc` de Plan 9 (Killian, 1984 es anterior, pero Plan 9 lo extiende con todos los dirs). Linux adoptó `/proc` en 1992; el `/proc/<pid>/{maps, status, cmdline}` es Plan 9 template.
- **systemd-nspawn** — contenedor ligero con namespace propio, está más cerca del modelo rfork de Plan 9 que Docker.
- **Wayland** — protocolo de display como mensajes tipados. No es 9P pero filosóficamente análogo: expone interfaces como objetos con operaciones (aunque con XML IDL en lugar de `Twalk/Topen`).
- **WireGuard + Tailscale** — "tu sesión se proyecta a donde tú vayas" es cpu(1) por VPN.
- **virtio-9p / 9pfs en KVM** — Linux consume 9P directamente para host-guest folder sharing, única deuda técnica explícita.
- **IPFS** — Venti content-addressed resurge globalmente con DHT encima.
- **seL4 capability model** — no viene de Plan 9 (viene de KeyKOS/EROS/Mach), pero converge en "primitiva única uniforme".

---

## 14. Tabla — Plan 9 concepts → modern equivalents

| Plan 9 concept | Linux / moderno equivalente | Fidelidad |
|---|---|---|
| **9P protocol** | FUSE (local only) + virtio-9p (Linux client) + v9fs | **Lossy**: FUSE no es remoto, no tiene tags/pipelining estándar. virtio-9p es 9P verdadero pero sólo host↔guest. |
| **Namespace per process** | Linux mount namespaces (`unshare --mount`) | **1-to-1 en concepto**, pero en Linux es opt-in y no define `bind` union-mount-before/after; OverlayFS es el substituto para unions. |
| **`bind -a` / `bind -b` (union)** | OverlayFS, AUFS | **Lossy**: overlayfs fijo a lowerdir+upperdir, no recursivo ni de-montable por path como Plan 9. |
| **cpu(1) / remote namespace projection** | SSH + `sshfs` + env propagation | **Lossy**: SSH no proyecta namespace coherente. Mejor aproximación: `ssh + tmux + shared vol`. WireGuard + NFS se acerca más en network. |
| **Factotum** | ssh-agent + GPG agent + pass + HashiCorp Vault | **Lossy**: cada uno cubre un slice. No hay un factotum unified en Linux desktop. |
| **Rio (windowing as 9P)** | Wayland (protocolo tipado) + X11 sockets | **Lossy**: Wayland es typed protocol pero no es filesystem; X11 es protocolo binario TCP. `/proc/N/fd/` es lo más cercano a "la ventana como file". |
| **Acme (files as UI)** | Emacs server + VSCode remote + neovim msgpack-rpc | **Lossy**: todos los editores modernos tienen APIs RPC, pero no universalmente consumibles sin librería. Acme + 9P es uniform-sin-SDK. |
| **`/net` como filesystem** | `/sys/class/net`, `ss`, `ip`, `socket()` | **Lossy**: Linux expone metadata en sysfs pero sockets siguen siendo syscall set separado. SocketCAN expone como device, parcial. |
| **`/proc` per-process** | Linux `/proc/<pid>/` | **1-to-1**: Linux copia directo. |
| **`/dev/draw` (rendering FS)** | DRM + /dev/dri/card0 + Mesa + Wayland | **Lossy**: DRM es ioctl-heavy + mmap, no 9P file ops. |
| **Venti (CA store)** | IPFS, git objects, restic, BorgBackup, ZFS dedup | **Lossy per tool**, pero conjuntamente cubren. git es el más cercano conceptualmente. |
| **Fossil (snapshotting CoW)** | ZFS snapshots, Btrfs snapshots, APFS snapshots | **1-to-1**: ZFS de hecho es más avanzado (ARC, RAIDZ). |
| **rfork namespace flags** | Linux `clone3()` CLONE_NEW* flags | **1-to-1 aproximado**, Linux desagregado en namespaces separados (mnt/pid/net/user/uts/ipc). |
| **Inferno Dis VM + Limbo** | Java JVM, .NET CLR, WASM | **Parcial**: WASM es el heredero filosófico más cercano (bytecode portable, hostable). |
| **9P over TCP port 564** | NFSv4 + SMB | **Lossy**: NFS es stateless-heavier, SMB es 100× más grande. 9P es drásticamente más simple. |

---

## 15. ALZE applicability — tres niveles

ALZE OS a día de hoy (`/root/repos/alze-os`, kernel x86_64 monolítico-microkernel-hybrid, ~30 KLOC C, VFS parcial sin locks, 29 tests runtime). Capacidad realista de absorber ideas Plan 9 en tres tiers:

### v1 — implementable en el repo **hoy** (1-3 sprints)

**9P como IPC universal del kernel**. Esta es la recomendación fuerte.

- Implementar `lib9p` en C99 en el kernel: ~1000 LOC para encode/decode de los 14 mensajes + fid table + tag table. Probado, pequeño, clean.
- Reemplazar el VFS operations struct actual por "dispatcher a 9P handler". Cada driver/filesystem implementa los ops 9P en lugar de una tabla propia.
- **Kernel `#c/cons` cons device**, **`#e/env`**, **`#p/<pid>/*`** (proc fs), **`#m/mouse`** cuando exista: todo 9P nativo.
- Exponer el **mismo protocolo** sobre **pipe**, **unix socket**, y **TCP/564** — no escribir drivers de network especiales para poder hacer mount remoto el día que haya red.
- **Beneficio competitivo real**: ALZE kernel pasa a ser "**Plan 9 compatible**" lo cual automáticamente abre el porte de:
  - drawterm — terminal remoto que habla 9P contra ALZE.
  - plan9port userland (acme, sam, mk, rc) — correría sobre ALZE con un shim POSIX finito.
  - 9front utilities seleccionadas — `stats`, `ps`, `mk`, `git9`.

  Sin este compatibility layer, ALZE sería yet-another-hobby-OS. Con 9P, hereda décadas de userland testado.

- Costo: ~2 semanas de kernel work para `lib9p` + refactor VFS. Menor que implementar un VFS "moderno" propio.

### v2 — requiere capability model (3-6 meses tras v1)

**Namespaces per process**, al estilo Plan 9 `rfork(RFNAMEG)` + `bind` + `mount`.

- Cada `task_struct` mantiene una **mount table privada** con CoW entre padre/hijo.
- Nueva syscall `alze_bind(old, new, flags)` con `MREPL/MBEFORE/MAFTER/MCREATE`.
- Syscall `alze_mount(fd, dst, flags)` donde `fd` es un canal 9P abierto.
- Combinar con cap-based model: una cap puede ser "cap para extender un namespace específico". Un proceso sin esa cap no puede hacer `bind`.
- Sandboxing inmediato: `alze_rfork(NEW_NAMESPACE | NOMOUNT)` + reconstruir minimal namespace = jail ligero sin código extra.
- Prioridad implementación: **alta**, porque es barata una vez hay capabilities y resuelve el tema "contenedores" sin implementar cgroups + 7 namespaces Linux-style por separado.

### v3 — aspiracional (1-2 años)

**Réplica completa Plan 9** — rio + acme + factotum + venti + fossil + cpu(1).

- Implica portar todo el userland de Plan 9. Viable porque p9p lo demuestra en POSIX; en un OS cap+9P nativo es más directo.
- Valor real: ALZE como **Plan 9 vivo en 2026 con pedigree moderno** (Rust-adjacent, capabilities, UEFI, 9P nativo). Atrae la comunidad 9front+cat-v+Go veterans.
- Riesgo: scope creep. No prometer antes de v1+v2.

---

## 16. Honest note — why this is ALZE's realistic differentiator

Plan 9 tiene **40 años**. Prácticamente todas las "innovaciones de OS" de 2005-2025 son **Plan 9 diluido**:

- **FUSE** — 9P sin la parte de red y sin nomenclatura consistente.
- **Docker + Kubernetes** — namespace-per-process + service discovery, fragmentado en 7 syscalls de Linux en lugar de una primitiva `rfork`.
- **Unix domain sockets usados como API** (D-Bus, systemd sockets, varlink) — 9P sin uniformidad, cada daemon inventa su wire format.
- **Containers en general** — el concepto "un proceso con su vista propia del mundo" es Plan 9 namespace por diseño original.
- **net/http handlers** — 9P pattern aplicado a web.
- **virtio-9p en KVM** — Linux admite 9P porque funciona.

Para ALZE, intentar **"ser mejor que Linux"** en cobertura de drivers, schedulers o filesystems es perder por KO técnico — Linux tiene 2000 contribuidores/mes, 30 millones de LOC, y 30 años de battle-testing.

En cambio, **ser Plan 9 compatible en 2026 con implementación moderna** es una posición defendible:

1. **Tamaño manejable**: 9P son ~1000 LOC C. Un kernel cap+9P fits en 30-50 KLOC. Una persona puede entender y mantener eso.
2. **Diferenciación clara**: ningún OS mainstream (Linux, BSDs, Windows, macOS) expone 9P nativo. ALZE sería el único kernel *desde el día uno* de 2026 con 9P como IPC primary.
3. **Herencia de userland**: drawterm, plan9port, p9p, 9front tools — décadas de software probado, portable con esfuerzo finito.
4. **Narrativa cultural**: "nieto legítimo de Plan 9" atrae a la tribu de Rob Pike / Russ Cox / cat-v, que son muchos de los mismos que escriben Go, Rust-kernel, Zig y OS experimental. Valioso en reclutamiento de contribuidores.
5. **Technically honest**: un hobby OS no va a reimplementar XDP ni sched_ext. Pero un hobby OS **puede** implementar 9P bien, porque el diseño está terminado, documentado, con test vectors, y con clientes de referencia que validan bit a bit.

Recomendación de acción para R4→R5 plan: en el siguiente sprint de ALZE, abrir tema "**9P as primary IPC**" como decisión arquitectónica. Spike de ~500 LOC de `lib9p` en el kernel; smoke test montando `#c/cons` vía 9P; demostrar que un proceso user puede abrir el fid root, walk, read. Si ese spike cuaja, el resto del VFS pasa a ser 9P dispatchers y ALZE gana su identidad.

---

## Fuentes consultadas

### Papers canónicos

- Pike, Presotto, Thompson, Trickey, Winterbottom — *Plan 9 from Bell Labs*, UKUUG Summer 1990 + revised 1995 proceedings — https://9p.io/sys/doc/9.html (archive https://web.archive.org/web/2024/https://9p.io/sys/doc/9.html).
- Pike, Presotto, Thompson, Trickey — *The Use of Name Spaces in Plan 9*, SIGOPS European Workshop 1992, pub. Operating Systems Review 27(2) 1993 — https://9p.io/sys/doc/names.html.
- Pike — *Acme: A User Interface for Programmers*, USENIX Winter 1994 — https://9p.io/sys/doc/acme.html.
- Pike — *Rio: design of a concurrent window system*, Bell Labs TR, c. 2000 — https://9p.io/sys/doc/rio-design.pdf.
- Cox, Grosse, Pike, Presotto, Quinlan — *Security in Plan 9*, USENIX Security Symposium 2002 — https://9p.io/sys/doc/auth.html.
- Quinlan, Dorward — *Venti: a new approach to archival storage*, FAST '02 — https://9p.io/sys/doc/venti/venti.html.
- Dorward, Pike, Winterbottom — *The Limbo Programming Language*, Vita Nuova TR 1997, revised — https://www.vitanuova.com/inferno/papers/limbo.html.
- Pike, Winterbottom, Dorward, Flandrena, Thompson, Trickey, Quinlan — *The Inferno Operating System*, Bell Labs Technical Journal Winter 1997 — https://www.vitanuova.com/inferno/papers/bltj.html.
- Ritchie — *The Evolution of the Unix Time-sharing System*, AT&T Bell Labs TJ 1984 (linaje) — https://www.bell-labs.com/usr/dmr/www/hist.html.

### Specs y manuales

- *Plan 9 Manual, Volume 1 — User Commands* (Fourth Edition) — https://9p.io/sys/man/1/INDEX.html.
- *intro(5) — introduction to the Plan 9 File Protocol, 9P* — https://9p.io/magic/man2html/5/intro.
- *draw(3)* — kernel device `/dev/draw` — https://9p.io/magic/man2html/3/draw.
- 9P2000 reference implementation — https://github.com/9fans/plan9port/tree/master/src/lib9p.
- *v9fs* (Linux 9P client) — https://www.kernel.org/doc/html/latest/filesystems/9p.html.
- *virtio-9p (9pfs)* (QEMU) — https://wiki.qemu.org/Documentation/9psetup.

### 9front + proyectos activos

- 9front project — https://9front.org/ (wiki, docs, FQA, releases).
- 9front git — http://git.9front.org/plan9front/plan9front/HEAD/info.html (mirror https://github.com/9front/9front).
- *9front FQA* (The 9front Frequently Questioned Answers) — http://fqa.9front.org/.
- cat-v.org Plan 9 essays — http://doc.cat-v.org/plan_9/ (compendio de papers y polémicas).
- p9p (plan9port) — https://github.com/9fans/plan9port (Russ Cox, Plan 9 userland en POSIX).

### Inferno

- Inferno OS project — https://www.vitanuova.com/inferno/.
- Inferno OS repo — https://github.com/inferno-os/inferno-os.
- *Dis Virtual Machine Specification* — https://www.vitanuova.com/inferno/papers/dis.html.

### Go lineage

- Pike — *Go at Google: Language Design in the Service of Software Engineering*, SPLASH 2012 — https://go.dev/talks/2012/splash.article.
- Pike — *Concurrency Is Not Parallelism* (Heroku Waza 2012) — https://go.dev/blog/waza-talk (aplica modelo Plan 9/Limbo a Go).
- Cox — *From Research to Production: The Plan 9 Roots of Go* (charla, transcripta en https://research.swtch.com/gotour).
- Griesemer, Pike, Thompson — *The Go Programming Language*, initial release notes 2009 — https://go.dev/doc/faq#history.

### Comentario y difusión

- Raymond — *The Art of Unix Programming* §20 "Plan 9: The Way the Future Was", 2003 — http://catb.org/~esr/writings/taoup/html/plan9.html.
- LWN — *Plan 9 from User Space* (2006) — https://lwn.net/Articles/196467/.
- LWN — *Plan 9 comes to the Linux kernel* (2005, v9fs merge) — https://lwn.net/Articles/116158/.
- Hacker News threads recurrentes sobre 9front, buscable en https://hn.algolia.com/?q=9front.
- Phoronix — cobertura periódica 9front releases (e.g. https://www.phoronix.com/search/9front).

### Archive fallbacks

Para 9p.io (Bell Labs) cuando esté caído, usar https://web.archive.org/web/2024/https://9p.io/sys/doc/ — hay snapshots 2019-2025 del árbol completo. cat-v.org también rehospeda los papers canónicos en http://doc.cat-v.org/plan_9/.
