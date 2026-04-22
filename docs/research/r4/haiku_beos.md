# Haiku + BeOS — R4 deep-dive

> Ronda 4, foco único. Complementa lo dicho en [`otros.md`](../otros.md) (Haiku: kernel híbrido, BMessage, BFS live queries) y en [`r3/capability_kernels.md`](../r3/capability_kernels.md) (Geiselbrecht ex-NewOS → Zircon). Aquí entramos en el linaje completo, la historia, y qué se puede robar para ALZE OS.

## TL;DR

BeOS fue el canto del cisne de los "OS de autor" en los 90 — Jean-Louis Gassée, ex-Apple, fundó Be Inc. (1990) para construir un OS pensado desde cero para **multimedia** en hardware multiprocesador. Técnicamente brillante: **BFS** (64-bit journaling FS con atributos indexados de primera clase, diseñado por Dominic Giampaolo — **el mismo hombre que 15 años después diseñaría APFS en Apple**), kernel **NewOS** escrito por **Travis Geiselbrecht** (**el mismo hombre que dos décadas después escribiría LK y lideraría Zircon en Google**), threading pervasivo, latencias de ventana sub-10 ms cuando Windows NT 4 bloqueaba toda la UI con una impresora mal conectada. Comercialmente murió: Apple compró NeXT en 1996, Palm compró los restos de Be en 2001, y BeOS R5.0.3 es la última release oficial (2000).

**Haiku** es la resurrección de código abierto. OpenBeOS arranca en agosto 2001 (una semana después del anuncio de Palm), se renombra Haiku en 2004, y 24 años después sigue vivo con releases reales: R1/beta1 (2018), beta2 (2020), beta3 (2021), beta4 (2022), **beta5 (sep 2024)**. A 2026 es el caso de estudio textbook de **OS de nicho sobreviviendo por dedicación voluntaria y foco narrow**: un solo objetivo (preservar el paradigma BeOS R5, binary-compatible), una sola distribución canónica, ~20 core devs, ~100 contribuidores regulares, y un desktop que funciona de verdad para lectura/audio/escritura.

Para ALZE, dos lecciones:
1. **Un diferenciador define el proyecto** — BeOS fue "el OS responsive con BFS"; no intentó ser "otro UNIX pero mejor". Haiku sobrevive porque su propuesta es inconfundible.
2. **Travis Geiselbrecht es la prueba viviente** de que una persona con la intuición correcta puede escribir dos kernels-de-referencia (NewOS → Haiku, LK → Zircon) separados por 20 años. No se necesitan 500 ingenieros para arrancar un kernel — se necesita uno que entienda la primitiva central.

## 1. BeOS — historia (1990–2001)

### Fundación: Jean-Louis Gassée y Be Inc. (1990)

- **Jean-Louis Gassée**: ingeniero francés, ex-VP de Apple (1981–1990), responsable en Apple de Macintosh II y IIfx. Se va de Apple tras choque con Sculley. Funda **Be Inc.** en octubre 1990 en Menlo Park con Steve Sakoman (ex-Apple Newton) y capital semilla (~$1.6M de Gassée personalmente + inversores).
- **Tesis original**: los PCs de los 90 no están diseñados para multimedia. Windows y Mac son monotarea efectiva con colas cooperativas; UNIX es responsive pero feo y sin stack multimedia. Oportunidad: un OS desde cero multiprocesador, multithread, latency-first, con un filesystem que trate medios como ciudadanos de primera clase.
- **Hardware**: Be no empezó vendiendo software. Vendió la **BeBox** (1995–1997), workstation AT-style con dual PowerPC 603 (y luego 603e), GeekPort DIY (DAC/ADC de lab), DSP onboard. ~1800 unidades vendidas en total. Precio inicial US$1600.
- **Pivote 1997**: BeBox se cancela. Be reescribe BeOS para **PowerPC Mac clones** (Power Computing etc.) y luego para **x86** (1998). En 1997, Gassée rechaza ~$200M de Apple (que buscaba reemplazar Mac OS clásico); Apple compra NeXT en su lugar por $429M y se queda con NeXTSTEP → Mac OS X.
- **BeOS R5 (marzo 2000)**: última release comercial mayor. BeOS 5 Personal Edition (gratis, image 500 MB montable sobre Windows) + Pro Edition ($70). R5.0.3 (noviembre 2000) es el patch final.
- **Muerte (agosto 2001)**: Palm Inc. adquiere los activos de Be por ~$11M. Palm se queda con la IP para Palm OS Cobalt (que nunca embarca en hardware exitoso). BeOS como producto muere.

### Contexto cultural

- **Dot-com boom**: Be era la startup OS-darling; cover de Wired, press love. Cayó con la burbuja.
- **Competencia directa**: NeXTSTEP (ganó el premio con Apple), QNX (ganó el nicho embedded/real-time), Windows NT (ganó por momentum enterprise), Linux (ganó el server/libre). BeOS quedó sin nicho defendible.
- **La lección económica**: ningún OS nuevo de uso general ha cruzado el chasm desde Windows 95. Incluso OSes técnicamente superiores necesitan un **app moat** o una **distribución captiva** (OEM deals) que Be nunca tuvo.

## 2. Killer features de BeOS (por qué la gente lo recordaba)

| Feature | Qué era | Estado mainstream 1998 |
|---|---|---|
| **Threading pervasivo** | Cada window = thread; cada app = team (proceso) con N threads; scheduler preemptivo | Windows NT4 tenía threads pero UI single-thread; Mac OS clásico cooperativo; Linux 2.0 sin threads nativos (LinuxThreads vía clone) |
| **BFS (Be File System)** | 64-bit journaling FS con atributos indexados, queries tipo SQL sobre atributos | ext2 (no journal), FAT32, HFS (sin journal hasta HFS+ 1998) |
| **Media Kit** | APIs real-time para audio/video pipelines, nodos conectables, latencias <10 ms | DirectShow en Windows (pesado), QuickTime (cooperativo), Linux no tenía stack de audio decente |
| **Instant-on UX** | Boot de ~10 s en hardware de la época; UI nunca bloqueada — ventanas repintándose durante disk busy | Windows 98 boot 60–90 s; un floppy malo podía freezear la UI |
| **BeAPI (C++ nativo)** | API del sistema en C++ OOP, no POSIX. Herencia de BApplication/BWindow/BView | Win32 era C procedural; Cocoa aún no existía; Motif en UNIX |

### "The BeOS teapot demo"

Gassée famoso por demos en los 90: arrancaba BeOS, abría 20 videos MPEG-1 simultáneos en una BeBox dual-PPC 603 a 133 MHz, y la UI seguía respondiendo. En Windows NT 4 o Mac OS 8 con hardware equivalente, el sistema se arrastraba o colgaba. **Esa era la tesis de venta**: BeOS no era más rápido en benchmarks sintéticos, era responsive bajo carga.

## 3. NewOS kernel — Travis Geiselbrecht (1997–)

### El dato que importa

**Travis Geiselbrecht** escribió NewOS (el kernel que acabaría siendo Haiku) ~1997–2001 en Be Inc., contribuyó al kernel BeOS R5, y **20 años después** escribió **LK (Little Kernel)** → base de **Zircon** en Google Fuchsia. Una sola persona detrás de dos kernels productivos separados por 2 décadas. El paper mental a sacar: la primitiva correcta de kernel (team + thread + port/channel + VM object) se puede reescribir en una base de código nueva sin pérdida de insight — la dificultad está en la intuición de diseño, no en las líneas de C.

- **NewOS git historical**: https://github.com/travisg/newos — repo personal de Travis, commits que se paran alrededor de 2007 cuando el flujo migra a Haiku. El código es legible, C+C++ mixto, portable (x86, SH4, ARM, PPC stubs).
- **Relación NewOS ↔ Haiku**: Haiku **forkeó NewOS** tempranamente (~2002) y reescribió ~70 % del código para lograr ABI-compat con BeOS R5. El espíritu de la arquitectura es NewOS; los detalles de syscalls, semántica de `port` y semánticas de BFS vienen de R5.

### Arquitectura del kernel NewOS / Haiku

- **Tipo**: híbrido (no microkernel puro, no monolítico). Scheduler + VM + FS + net stack en kernel space. Drivers mayoritariamente en kernel pero con API estable (módulos cargables).
- **Lenguaje**: C con bolsillos de C++ (especialmente en subsistemas nuevos post-2010). Arranque y boot loader en ASM x86.
- **Primitivas kernel**:
  - **team** ≈ proceso. Unidad de address space + resource ownership.
  - **thread** ≈ POSIX thread pero de primera clase en el kernel, con prioridad real-time disponible (1–120, con urgencia RT ≥100).
  - **port** ≈ cola de mensajes del kernel, FIFO, typed. Primitiva IPC de bajo nivel sobre la que BMessage se construye en user space.
  - **semaphore** ≈ primitiva de sincronización, usada extensivamente (Be exponía sems al user como recurso público).
  - **area** ≈ región de memoria reservada, nombrable y compartible entre teams (precursor de `mmap` shared anon memory).
- **SMP**: soportado desde el día 1 (BeBox era dual-CPU). Scheduler con runqueues per-CPU, affinity bits, load balancing cada ~10 ms.
- **64-bit**: BeOS R5 siempre fue 32-bit user space. Haiku introduce **x86_64 builds** como primary arch alrededor de 2018; 32-bit `x86_gcc2` se mantiene como variante para correr binarios BeOS R5 nativos (R5 fue compilado con GCC 2.95 — el ABI de C++ de GCC ≥3 es incompatible).
- **Driver API**: estable, compatible con drivers BeOS R5 (raros hoy) + modernos (USB, xHCI, Wi-Fi via FreeBSD compat layer `freebsd_wlan`).

## 4. BFS — el filesystem que definió BeOS

**Dominic Giampaolo** fue el arquitecto de BFS en Be Inc. (1996–1999). En 1999 publicó el libro canónico:

> Giampaolo, Dominic. 1999. *Practical File System Design with the Be File System*. Morgan Kaufmann. ISBN 1-55860-497-9. PDF libre en https://www.nobius.org/dbg/practical-file-system-design.pdf (autorizada por el autor).

El mismo Giampaolo **se fue a Apple en 2002** y, tras ~15 años trabajando en HFS+ y journalling, **diseñó APFS** (Apple File System, ship 2017 en iOS 10.3 / macOS 10.13). El linaje intelectual BFS → APFS es directo: ambos comparten la obsesión por metadata rica indexada y por copy-on-write incremental.

### BFS — detalles técnicos

- **Estructura**: B+tree sobre todos los directorios; cada directorio es un B+tree de (nombre → inode).
- **Journal**: journaling de metadata (no de datos) por defecto. Block-size default 2 KiB, configurable hasta 8 KiB.
- **Atributos**: cada file puede tener N atributos (name, type, value). Tipos soportados: `B_INT32_TYPE`, `B_STRING_TYPE`, `B_TIME_TYPE`, `B_DOUBLE_TYPE`, etc. — 20+ tipos built-in. Un atributo es un mini-file propio (puede tener size arbitrario).
- **Índices**: atributos pueden declararse **indexed**. El FS mantiene un B+tree por índice (`/boot/home/index:BEOS:TYPE` etc.). Consulta: live queries sobre índices.
- **Query**: sintaxis tipo SQL-lite. Ejemplo:
  ```
  (BEOS:TYPE == "audio/mpeg") && (Audio:Bitrate >= 128) && (last_modified > %last_week%)
  ```
  ejecutable desde el CLI (`query`), desde `Tracker` (file manager), o desde código. **Live**: la consulta puede mantenerse abierta y notificar cuando files entran/salen del match set.
- **Aplicación real**: el cliente de email de BeOS guardaba cada email como un file con atributos (`MAIL:from`, `MAIL:subject`, `MAIL:priority`). La "inbox" era un live query `MAIL:status == "new"`. No había base de datos de emails — el filesystem era la base de datos.
- **Max volume**: 2^63 bytes (teórico), probado hasta decenas de TB. Bloque size limita files individuales a 2^32 × block_size.

### Comparativa FS

| FS | Journal | Atributos | Live query | COW | Checksums E2E |
|---|---|---|---|---|---|
| **BFS** (1998) | metadata | first-class indexed | sí | no | no |
| ext4 (2008) | metadata | via xattr, no indexed | no | no | no |
| APFS (2017) | metadata | xattr indexed (Spotlight) | sí (Spotlight daemon) | sí | metadata only |
| ZFS (2005) | no (txg+uberblock) | xattr no indexed | no | sí | sí |
| btrfs (2009) | no (COW) | xattr no indexed | no | sí | sí |
| NTFS (1993) | metadata | ADS (streams), no indexed kernel-side | no (via Search Indexer daemon) | no | no |

## 5. Messaging API — BMessage, BLooper, BHandler

BeOS expuso dos capas de IPC:

- **Bajo nivel (kernel)**: `port_t`. FIFO byte-oriented, per-team. `write_port(port, code, buffer, size)` + `read_port(port, &code, buffer, size)`. Blocking y non-blocking variants. ~comparable con POSIX mq_send/mq_receive pero más lightweight.
- **Alto nivel (user space)**: **BMessage** sobre port. Un `BMessage` es un record con:
  - `what` (int32, "command code" tipo `B_SAVE_REQUESTED`, `B_QUIT_REQUESTED`, custom)
  - N campos nombrados con tipo: `message->AddString("filename", "/tmp/x")`, `message->FindInt32("count", &n)`
  - Serialize a bytes, send via port al `BLooper` de destino, deserialize del otro lado
- **BLooper** = object con su propio thread y message queue. Heredar de BLooper y override `MessageReceived(BMessage*)` para handle.
- **BHandler** = object no-thread que se "attach" a un BLooper para handle mensajes. Muchos BHandlers → un BLooper.
- **BApplication** = un BLooper global que representa la aplicación. Recibe `B_QUIT_REQUESTED`, `B_ABOUT_REQUESTED`, etc.
- **BWindow** = BLooper que además tiene un viewport gráfico. **Cada ventana es su propio thread**. Eso es la clave de la responsiveness: una ventana bloqueada no afecta otras.

### Influencia histórica

- POSIX message queues (POSIX.1b, 1993) son más primitivas (bytes + priority, no typed fields).
- **Fuchsia FIDL + channels** toma mucho de BMessage: channels son port-like, FIDL añade typing, pero la filosofía "mensaje estructurado con campos tipados" viene de BeOS.
- Android Handler/Looper (SDK desde API 1) son **literalmente BLooper/BHandler con otro nombre**. Andy Rubin había trabajado con varios ex-Be ingenieros y Danger Hiptop (su startup pre-Android) tenía diseño similar.
- Cocoa `NSNotificationCenter` + NSRunLoop es cercano pero menos tipado.

## 6. BeAPI — C++ framework original

- Root namespace: todas las clases empiezan por `B` (BView, BWindow, BApplication, BString, BList, BButton, BTextView...).
- Todo en C++: no hay C API principal. Wrappers C existen para Fortran/Pascal legacy pero no recomendados.
- Patrón típico de "Hello World":
  ```cpp
  class HelloApp : public BApplication {
  public:
      HelloApp() : BApplication("application/x-vnd.hello") {
          BRect frame(100, 100, 300, 200);
          BWindow *w = new BWindow(frame, "Hello", B_TITLED_WINDOW, 0);
          w->Show();
      }
  };
  int main() { HelloApp app; app.Run(); return 0; }
  ```
- **ABI estability**: el BeAPI de R5 (2000) sigue binary-compatible con Haiku x86_gcc2 (2024). Un binario compilado para BeOS R5 en 2000 corre en Haiku R1/beta5 sin recompilar. **24 años de ABI stability voluntaria** — algo que ni Apple ni Microsoft logran con ese ceiling.
- El precio: el ABI de C++ está pinned a GCC 2.95 para la variante "x86_gcc2". Los símbolos name-mangled no son compatibles con GCC moderno. Haiku mantiene **dos variantes paralelas** de sus libs: `x86_gcc2` (compat R5) y `x86` (GCC ≥11, "hybrid"). En x86_64 no hay compat R5 (nunca existió binario R5 de 64-bit), sólo GCC moderno.

## 7. Package management — HaikuDepot + hpkg

Haiku introdujo **hpkg** (Haiku package format) alrededor de 2013. Innovación clave: los paquetes **no se instalan extrayendo**, se **montan**.

- Un `.hpkg` es un archivo custom con manifest + contenido comprimido (Zstandard o ZLIB).
- El kernel incluye un **packagefs**: monta paquetes activos a read-only en `/boot/system/packages/` y los hace visibles como vistas unificadas en `/boot/system/{bin,lib,data,...}`.
- **Activar / desactivar un paquete = mover el archivo al directorio de activados**. No copy, no rollback complicado.
- Upgrades atómicos: remplazar el `.hpkg` → packagefs refleja la nueva versión en la próxima boot o via `pkgman` refresh.
- **HaikuDepot** es el client GUI; `pkgman` es el CLI. Repos oficiales: `HaikuPorts` (https://github.com/haikuports/haikuports) mantiene recipes al estilo Gentoo portage.
- Estado 2026: ~5 k paquetes mantenidos para x86_gcc2/x86/x86_64 (no todos en los 3 arcos). Ports activos: LibreOffice, LLVM/Clang, Python 3.11, Node 20, Rust toolchain, Qt 5/6, GTK 3 (parcial), WebKit.

## 8. Software portado 2026

- **Browsers**: **WebPositive** (browser nativo Haiku basado en WebKit) funciona para sitios simples pero se queda atrás en JS moderno. **Falkon** (QupZilla, Qt+WebEngine) está portado y es el "browser usable" de facto. Firefox/Chromium oficiales no portados — demasiado acoplados a Linux build system, aunque hay intentos experimentales con el fork de WebKit2GTK.
- **Oficina**: **LibreOffice 7.x** portado a Haiku x86_64 en 2023 (beta4), estable en beta5. **Calligra** también disponible.
- **Dev tools**: **GCC 13** default en beta5, Clang 16 vía paquete, **Rust 1.75** (via rustup community port, 2025), **Python 3.11**, **Go 1.21**, Node 20 LTS.
- **Editores**: **Pe** (editor nativo BeOS legendario, OSS desde Be), **StyledEdit** (TextEdit-like), Kate, VS Code (sólo Code - OSS fork "VSCodium" build experimental, electron port parcial), Vim/Neovim/Emacs nativos.
- **Multimedia**: **MediaPlayer** (nativo, usando Media Kit), **ffmpeg** port completo, **MPV** portado.
- **Games**: ScummVM nativo, DOSBox, Quake 1/2/3 ports históricos, **Wine experimental** (x86_64 sólo, rotura).
- **Development workflow típico**: `haikuporter` (build tool) + recipe file → compila paquete en chroot aislado (análogo a `pbuilder` Debian o `sbuild`). `pkgman install <pkg>`.
- **QEMU guests**: Haiku corre decentemente en QEMU/KVM con virtio. Ideal para desarrollo no destructivo. Builds CI oficiales ejecutan tests en QEMU.
- **Hardware real 2026**: Wi-Fi via FreeBSD wrappers (muchos chipsets Intel/Atheros funcionan), USB 3 (xHCI) estable, GPU solo framebuffer o Radeon KMS parcial (no Nvidia, no hardware video accel). Laptop daily driver: técnicamente posible en ThinkPads ~2015 con Wi-Fi Intel AC-7260/7265, no recomendable como primary OS.

## 9. Estado del proyecto Haiku a 2026

### Release cadence

| Release | Fecha | Highlights |
|---|---|---|
| R1/alpha1 | sep 2009 | Primera alpha pública tras 8 años de dev |
| R1/alpha4 | nov 2012 | Última alpha, se congela el release engineering |
| R1/beta1 | sep 2018 | Package management hpkg; primera beta real. **6 años entre alpha4 y beta1** |
| R1/beta2 | jun 2020 | x86_64 first-class; muchos ports |
| R1/beta3 | jul 2021 | WebPositive mejorado, xHCI |
| R1/beta4 | dic 2022 | Boot loader EFI robusto, nuevos drivers Wi-Fi |
| R1/beta5 | sep 2024 | **Estado 2026**. KDL (Kernel Debugging Land) mejorado, Wayland experimental (no default), hardware compat 2020–2023 era |

R1 stable no ha ship en 2026. La broma interna: "R1 será cuando esté listo" — la meta sigue siendo **estabilidad BeOS R5 compat first**, no timeline.

### Gobernanza y financiación

- **Haiku Inc.**: nonprofit 501(c)(3), registrada en EEUU. Acepta donaciones, patrocina Google Summer of Code cada año (~4–8 estudiantes/año), contrata 1–2 devs part-time cuando hay budget.
- **Core team**: ~20 committers activos. **Axel Dörfler** (lead histórico, ex-BeOS tester) sigue activo en 2026. François Revol, Adrien Destugues, Stephan Aßmus, Oliver Tappe, Ryan Leavengood — nombres recurrentes desde ~2005.
- **Financiación 2025**: ~$60k/año en donaciones. Parte se usa para GSoC matching funds y para hosting infra. El resto del trabajo es volunteer puro.
- **Comunidad**: foros en https://discuss.haiku-os.org, IRC #haiku en Libera.Chat (~60 users regulares), IRC #haiku-commits con feed de commits.

### Comparativa longevidad

Proyectos OS de 90s-2000s que siguen vivos en 2026:
- **Linux** (1991–): escala industrial, ~25 k commits/release.
- **FreeBSD** (1993–): ~2 k committers totales, ~500 activos.
- **OpenBSD** (1996–): ~250 committers totales, ~50 activos.
- **Haiku** (2001–): ~100 contributors totales, ~20 activos.
- **ReactOS** (1996–): ~50 activos.
- **AROS** (1995–): ~10 activos (reimpl Amiga).
- **SkyOS / MenuetOS / Syllable**: muertos o zombi.

**24 años × 20 devs volunteer = ~100 person-years de dev**. Comparable a un proyecto comercial de 3 años con 30 ingenieros. Haiku es lo que ese esfuerzo compra si se concentra en un objetivo.

## 10. Tabla maestra — BeOS/Haiku key features vs estado mainstream 2026

| Innovación | Año BeOS/Haiku | Mainstream catching up | ¿Sigue siendo exclusiva? |
|---|---|---|---|
| **BFS: atributos indexados + live queries** | 1998 | Spotlight (macOS 2005) es daemon externo, no FS. Windows Search similar. NTFS ADS sin índice. Linux xattr sin índice. | **Sí**: ningún FS mainstream tiene queries como primitiva del FS + live notification en 2026. |
| **Media Kit: audio/video pipeline kernel-aware** | 1998 | PipeWire (Linux, 2021) se acerca. CoreAudio (macOS) bueno pero no video. WASAPI (Windows) sólo audio. | **Parcialmente**: PipeWire iguala audio pero no tiene el modelo de nodos gráficos de video que BeOS tenía. |
| **BMessage: typed structured IPC** | 1998 | FIDL (Fuchsia, 2019) converge. Android Handler/Looper copia directa. D-Bus (Linux, 2003) cercano. | **No**: la idea está mainstream, via D-Bus / FIDL / Android. |
| **Threading pervasivo + window-per-thread** | 1998 | Windows/macOS/Linux tienen threads; pero window-per-thread sigue inusual (toolkits modernos usan event loop single-thread). | **Sí**: el modelo "una window = un thread" es exclusivo. |
| **BeAPI C++ binary-compat 24 años** | 2000 (R5) | Win32 tiene binary compat similar. macOS rompió 3 veces (Carbon → Cocoa → Cocoa64 → ARM). | **Comparable a Win32**: ambos mantienen ABI décadas. |
| **hpkg packagefs (mount, no install)** | 2013 | NixOS (2003) es conceptualmente cercano. Snap/Flatpak usan squashfs mount pero con containerization. | **Parcial**: NixOS y snap usan mounted images, pero sin la integración unified view tan limpia. |
| **Boot ~10s en hardware de su época** | 1998 | Fast boot con SSD + UEFI logra lo mismo desde ~2015 en Win/Mac. Pero la responsiveness bajo carga sigue sin match. | **No (boot), Sí (responsiveness)**. |

## 11. ALZE applicability — qué robar para ALZE OS

### Tier 1 — claramente worth stealing

**v1. Atributos indexados + live queries como primitiva del VFS.**
ALZE OS actualmente tiene un VFS básico con ext2 readonly (ver `/root/repos/alze-os/kernel/vfs.c`). Cuando ALZE necesite un FS nativo propio, BFS es el modelo más interesante *para un OS no-Unix*:
- Cada inode lleva una lista de atributos (name, type, value) además del contenido.
- El FS mantiene B+trees por atributo para los marcados indexables.
- Syscall `alze_query(expression) → cursor` que itera matches + `alze_query_watch(cursor) → notification fd` para live updates.
- Esto encaja con filosofía capability-first de ALZE: la query es una cap sobre un índice, no un path absoluto.
- Costo: implementar BFS-like desde cero es ~5–10 KLOC sólido. Mirror del libro de Giampaolo como guía.

**v2. BMessage-style structured messages sobre la primitiva IPC raw.**
ALZE ya tiene mensajes raw planificados (ver síntesis de `otros.md` punto #2). BMessage añade una capa de alto nivel:
- Cada mensaje = header (`what: u32`) + lista de campos tipados (`name: str, type: enum, value: bytes`).
- Kernel mueve los bytes, userland libs hacen parse/build.
- Ventaja sobre FIDL: no necesitas un IDL compiler para empezar — el tipado es runtime-checked, util para desarrollo iterativo.
- Costo: ~500 LOC en una lib userland; 0 impacto en kernel.

**v3. Window-per-thread si ALZE llega a tener un window server.**
Lejos del roadmap actual (ALZE es pre-desktop), pero si se llega: una window = un thread desde el día 1. Permite programación directa sin event loops complejos. Copia directa de BWindow.

### Tier 2 — inspiración, no copia directa

**v4. Hybrid kernel aspirational, no microkernel puro.**
Haiku demuestra que un kernel híbrido bien diseñado puede dar la responsiveness que la gente asocia a microkernels, sin el overhead de IPC constante. ALZE actualmente es monolítico (kernel/ path tiene scheduler + vmm + pmm + vfs + xhci en espacio común). La decisión microkernel-vs-hibrido es open; Haiku es evidencia de que híbrido funciona si drivers críticos (FS, net) están en kernel y el resto (app_server, registrar, UI) en userland.

**v5. ABI stability como compromiso explícito.**
Haiku mantuvo ABI BeOS R5 durante 24 años **voluntariamente**, dejándose atar a GCC 2.95 en una variante. No es gratis; es una decisión de producto. Para ALZE, aprender de Windows Win32 y Haiku BeAPI: si alguna vez ALZE tiene users, romper ABI es la forma más rápida de perderlos.

**v6. Volunteer longevity pattern.**
24 años de Haiku con ~100 contribuidores totales. Patrón:
- **Un objetivo claro y único** (preservar BeOS R5 paradigma) — no mission creep.
- **Una distribución canónica** — no "Haiku Ubuntu vs Haiku Fedora".
- **Un lead dev respetado** (Axel Dörfler) con bus factor >1 (Stephan, François, Adrien como secundarios).
- **GSoC participation yearly** — pipeline de devs junior que a veces se quedan.
- **Releases cuando estén listos** — no calendar-driven.

ALZE como hobby kernel personal de 1 dev (ver PROGRESS.md del repo) no tiene la escala de Haiku, pero la filosofía "narrow focus + be patient" aplica idéntica.

### Tier 3 — no copiar

**v7. GCC 2.95 legacy.**
Haiku paga un precio técnico altísimo por la compat BeOS R5 x86_gcc2. ALZE, sin legacy userland, **no debe heredar ningún ABI histórico**. Empezar limpio con Itanium ABI / System V AMD64 moderno o un ABI propio.

**v8. Hybrid C+C++ mezcla.**
El codebase Haiku mezcla C kernel + C++ subsistemas + C++ userland. Funciona pero hace el build complejo. ALZE actualmente es C puro (ver Makefile en /root/repos/alze-os) — mantener esa simplicidad mientras el kernel sea pequeño.

## 12. Fuentes

### Oficiales y directas

- **Haiku Project site** — https://www.haiku-os.org/ (official, 2001–). Blog: https://www.haiku-os.org/blog/
- **Haiku R1/beta5 release notes** (2024-09-15) — https://www.haiku-os.org/get-haiku/release-notes/r1beta5/
- **Haiku R1/beta4 release notes** (2022-12-23) — https://www.haiku-os.org/get-haiku/release-notes/r1beta4/
- **Haiku R1/beta3 release notes** (2021-07-26) — https://www.haiku-os.org/get-haiku/release-notes/r1beta3/
- **Haiku R1/beta2 release notes** (2020-06-09) — https://www.haiku-os.org/get-haiku/release-notes/r1beta2/
- **Haiku R1/beta1 release notes** (2018-09-28) — https://www.haiku-os.org/get-haiku/release-notes/r1beta1/
- **Haiku source tree** — https://git.haiku-os.org/haiku (primary) / mirror https://github.com/haiku/haiku
- **HaikuPorts recipes** — https://github.com/haikuports/haikuports

### BeOS histórico (archive fallback cuando hace falta)

- **Be Inc. archived site** — https://web.archive.org/web/2000*/http://www.be.com/
- **BeBox technical specs** — https://www.nobius.org/dbg/beboxspecs.html
- **Be Newsletter archive** (1995–2001) — https://birdhouse.org/beos/byte/ y https://www.haiku-os.org/legacy-docs/benewsletter/
- **Jean-Louis Gassée blog "Monday Note"** — https://mondaynote.com/ (post-Be, frecuentes retrospective posts)
- Gassée, Jean-Louis. 1998. Interview en *Wired*: "The Be Thing". https://www.wired.com/1998/11/gasse/

### Travis Geiselbrecht / NewOS

- **NewOS GitHub (historical)** — https://github.com/travisg/newos (mirror del CVS original). Commits 1997–2007.
- **Travis's GitHub profile** — https://github.com/travisg (LK, NewOS, diversos side projects)
- **LK (Little Kernel) → Zircon** — https://github.com/littlekernel/lk (origen del kernel que Google adoptaría como Zircon).
- Geiselbrecht, Travis. Talks en ELCE y Linaro Connect sobre LK (2015–2018), YouTube.

### Axel Dörfler y talks Haiku

- **Dörfler, Axel**. "Haiku — an Open Source Operating System". FOSDEM 2010. https://archive.fosdem.org/2010/schedule/events/haiku
- **Dörfler, Axel**. "Haiku's package management". FOSDEM 2014. https://archive.fosdem.org/2014/schedule/event/haiku_pm/
- **Revol, François**. "Haiku, a free software operating system inspired by BeOS". FOSDEM 2016. https://archive.fosdem.org/2016/schedule/event/haiku/
- **Destugues, Adrien**. "Haiku hardware abstraction layer and drivers". FOSDEM 2019. https://archive.fosdem.org/2019/schedule/event/haiku/

### BFS y Dominic Giampaolo

- **Giampaolo, Dominic. 1999. *Practical File System Design with the Be File System***. Morgan Kaufmann. ISBN 1-55860-497-9. PDF autorizado: https://www.nobius.org/dbg/practical-file-system-design.pdf
- **BFS Wikipedia** — https://en.wikipedia.org/wiki/Be_File_System
- Giampaolo's WWDC talks sobre APFS (2016, 2017) — https://developer.apple.com/videos/play/wwdc2017/715/ (APFS talk). Relevante por el linaje BFS→APFS.
- **Haiku BFS implementation** — https://git.haiku-os.org/haiku/tree/src/add-ons/kernel/file_systems/bfs

### Package management

- **Haiku Package Management design doc** — https://www.haiku-os.org/docs/develop/packages/BuildingPackages.html
- **hpkg format spec** — https://www.haiku-os.org/docs/develop/packages/FileFormat.html
- Tappe, Oliver. "Haiku package management internals". Haiku user meetup, 2013 — blog post https://blog.haiku-os.org/2013/04/haiku-package-management/

### BMessage / BeAPI

- **Be Book (BeOS R5 API reference, mirror)** — https://www.haiku-os.org/legacy-docs/bebook/
- **Haiku API docs** — https://api.haiku-os.org/
- **Negus, Chris; Schneider, Chris. 2000. *Using BeOS***. Que. ISBN 0-7897-1788-0 (dated but still useful).

### Retrospective / postmortem

- Gassée, Jean-Louis. 2005. "BeOS and Apple". Monday Note retrospective. https://mondaynote.com/beos-and-apple-the-untold-story
- Pavlov, Scot Hacker. 1999. *BeOS: Porting UNIX Applications*. O'Reilly. ISBN 1-56592-673-X. (Archive copy: https://www.birdhouse.org/beos/bible/).
- **OSNews "A Programmer's Introduction to the Haiku OS"** (2011) — https://www.osnews.com/story/24945/a-programmers-introduction-to-the-haiku-os/

## 13. Nota final honesta

Haiku es el caso de estudio textbook de un **OS de nicho sobreviviendo por dedicación y foco narrow**. Veinticuatro años in, tiene:

- Un desktop parcialmente usable (lectura, oficina básica, media, dev).
- Una comunidad pequeña pero estable.
- Release cadence real (5 betas en 6 años, ritmo humano).
- ABI compat R5 como promesa cumplida — un binario de BeOS 5.0.3 del año 2000 corre en 2026.

Pero también:
- Nunca será daily driver para la mayoría de usuarios técnicos en 2026. Browser moderno es el talón de Aquiles persistente (WebKit fork siempre atrasado).
- Hardware coverage es del 2015, no del 2025. Wi-Fi 6E, Thunderbolt 4, GPUs modernas no funcionan.
- El cliente de email con live query BFS — la killer demo de BeOS — sigue siendo genuinamente único en 2026 sobre Haiku nativo, pero nadie fuera del círculo Haiku lo usa.

**Para ALZE, la lección es doble**:

1. **Focus on ONE differentiator.** BeOS tenía BFS + responsiveness. Haiku tiene preservación del paradigma BeOS. QNX tenía real-time. seL4 tiene verification. Redox tiene Rust + URLs. **Si ALZE no puede terminar la frase "ALZE es el único OS que X" sin dudar, el proyecto no tiene diferenciador**. El kernel tecnológicamente mejor sin diferenciador pierde contra el peor con uno.

2. **Volunteer longevity = narrow mandate + patient cadence + one canonical distribution.** No hay atajos. Haiku es el ejemplo. ALZE puede ser un proyecto personal indefinidamente sano si el mandato sigue siendo "learn OS dev deeply" y no muta a "compete with Linux".

---

**Fun fact for the record**: **Travis Geiselbrecht** escribió:
- **NewOS** (1997–, → kernel base de Haiku, vivo en 2026 con ~100 contributors)
- **LK / Little Kernel** (2008–, → Zircon / Fuchsia, embarcando en Nest Hubs desde 2021)

Dos kernels-de-referencia separados por **una década**, del mismo cerebro, ambos **object-capability-ish**, ambos **message-passing first**. No es coincidencia ni suerte: es la prueba empírica de que el conocimiento de kernel design acumulado por una persona a lo largo de una carrera **se transfiere a código nuevo sin pérdida**. Cuando ALZE se atasque con dudas de diseño, la heurística "qué habría hecho Travis" es sorprendentemente productiva — ambos sus kernels comparten el sesgo hacia primitivas ortogonales pequeñas (team, thread, port/channel, VM object, handle) y evitan los mega-diseños.

El otro fun fact: **Dominic Giampaolo** escribió BFS (1996–1999 en Be, filesystem con atributos indexados y live queries) y **luego APFS** (2014–2017 en Apple, filesystem COW con snapshots). Dos filesystems de referencia separados por 20 años, el segundo shipping en **mil millones** de devices iOS/macOS en 2017. Lección paralela a Travis: el diseño de filesystems es **conocimiento personal no-transferible a equipos**; un arquitecto con intuición correcta vale más que 30 ingenieros medios.

BeOS no sobrevivió. Pero sus dos arquitectos clave siguen escribiendo el futuro del OS design 25 años después, en proyectos que sí son producción global. **Esa es la métrica real de una escuela de diseño**: no si el producto original vive, sino si las ideas se reencarnan.
