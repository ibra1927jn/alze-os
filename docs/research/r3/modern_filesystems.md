# Modern Filesystems — ZFS, APFS, Btrfs, bcachefs, XFS, F2FS, Ceph, NILFS2, WAFL + LFS roots

**Fecha:** 2026-04-22
**Round:** R3 kernel subsystems modernos
**Input:** R1 síntesis top-idea #3 (CoW + E2E checksums), R2 review `fs_storage.md` (VFS sin locks, ext2-lite RO, fd-table global).
**Repo anchor:** `/root/repos/alze-os` — VFS+ext2 RO, ~1.750 LOC FS total.
**Objetivo:** Catálogo de los FS modernos: qué ideas se prestan, cuáles no escalan a un OS de 14k LOC, y un plan de migración v1→v2→v3 para ALZE.

---

## 0. Por qué este documento existe

R2 `fs_storage.md` identificó dos blockers sistemicos en ALZE:

1. **VFS sin read-lock en hot path** (`vfs_read`/`vfs_write`/`vfs_ioctl`/`vfs_seek` no toman `vfs_rwlock`) → UAF window con cualquier `close(fd)` concurrente.
2. **fd table global, no per-task** (`static struct file fd_table[16]` en `vfs.c:23`) → rompe aislamiento de procesos completamente.

Más abajo hay 19 issues más: div-by-zero en superblock, `s_inode_size` sin range-check, GDT bounds solo del primer bloque, feature-flags ignoradas, `name_len` vs `rec_len` sin validar, ramdisk + ext2 VFS adapters ignoran offset, sin `..` especial, sin symlinks, sin `strlcpy`, sin `le32_to_cpu`. Resumen: el FS de ALZE es **unsafe contra user data hoy mismo**.

R1 ya había marcado como top-idea #3 "Sistema declarativo atómico con CoW + E2E checksums" (combinando NixOS + APFS + ZFS). Este R3 expande esa idea: qué hay en el estado del arte, qué subconjunto puede entrar en ~5–10 kLOC, y qué conviene dejar fuera.

---

## 1. ZFS — el zettabyte filesystem

**Autores:** Jeff Bonwick, Matt Ahrens + equipo Sun (Bill Moore, Mark Shellenbaum, Mark Maybee). Diseño 2001, primer commit OpenSolaris 2005, integrado Solaris 10 update 2 (2006).
**Venue seminal:** Bonwick & Ahrens, *"The Zettabyte File System"* — presentación interna Sun 2003, luego publicada ~2006. Paper oficial más citado: Bonwick, Ahrens, Henson, Maybee, Shellenbaum, *"The Zettabyte File System"*, técnica report Sun Microsystems.
**URL / archive:** <https://www.cs.hmc.edu/~rhodes/courses/cs134/spring2010/paper/zfs_overview.pdf> · OpenZFS docs <https://openzfs.github.io/openzfs-docs/> · archive <https://web.archive.org/web/2024/https://openzfs.github.io/openzfs-docs/>.

### Arquitectura clave

- **Pooled storage** (`zpool`): la capa más baja no es "un disco con un FS encima"; es un "storage pool" multi-device con vdevs (mirror, raidz1/2/3, dRAID) y un espacio de bloques unificado. Un FS (`zfs create pool/home`) es un *dataset* que toma bloques del pool.
- **Copy-on-Write estricto**: todo bloque modificado se escribe a otra ubicación; los bloques antiguos quedan hasta que los libera un snapshot/GC. Esto elimina el "write hole" de RAID5/6 (no hay parity update parcial), y da atomicidad de todo el subárbol de un txg (transaction group) vía una única actualización del `uberblock`.
- **Merkle tree end-to-end**: cada `blkptr_t` (128 bytes) guarda un checksum (SHA-256 por defecto, Fletcher4 opcional para throughput) del bloque al que apunta; el árbol jerárquico de punteros termina en el uberblock, que es lo único que se actualiza atómicamente con un commit. Detecta **silent data corruption** (bitrot, cable flake, controller firmware bugs) que RAID hardware tradicional no ve.
- **RAID-Z**: variante de RAID5/6/Z3 con full-stripe writes obligatorios → no hay write-hole, pero las lecturas aleatorias necesitan leer toda la franja. `draid` (2020) añade distributed spare para rebuilds 10x más rápidos en pools grandes.
- **ZIL** (ZFS Intent Log): WAL síncrono opcional para `fsync()` rápido sobre SSD/NVMe SLOG device, mientras el pool principal sigue en HDD.
- **L2ARC** (Level-2 ARC): cache de lectura en SSD extendiendo el ARC (Adaptive Replacement Cache, patente IBM usada bajo licencia).
- **128-bit addressing**: máximo 2^128 bytes por pool. Bonwick famosa cita: "hirvjendo los océanos para almacenar 2^128 bits rompería leyes físicas" — el punto no es el tamaño real sino evitar *ever* tener que rediseñar el layout.
- **Datasets, clones, snapshots**: snapshots son gratis (cero bloques copiados); clones son snapshots escribibles; `zfs send/recv` es replicación incremental nativa.
- **Compresión built-in** (lz4 por defecto, zstd desde 2020), dedup opcional (memoria-hambriento).
- **Encryption** `zfs encrypt` desde 2019 (OpenZFS 0.8); por-dataset, AES-256-CCM/GCM.

### Gobernanza

Sun → Oracle (adquisición 2010) cerró el source. OpenZFS (2013) unificó los forks de FreeBSD, illumos, Linux (ZFS-on-Linux de Brian Behlendorf). Desde 2020, "OpenZFS 2.0" es la única codebase, cross-platform. Linux via DKMS/kmod (licencia CDDL vs. GPL impide merge in-tree — Canonical envía ZFS en Ubuntu igualmente, cosa que Red Hat no hace).

### Tamaño real

OpenZFS Linux ~300 kLOC + userland. El kernel module `zfs.ko` por sí solo es ~250 kLOC. Comparar con ALZE total 14 kLOC: **22x más código que todo el kernel de ALZE solo para el FS**. Portar ZFS completo está fuera del alcance; las **ideas** (CoW txg, Merkle tree de bloques, uberblock atomic commit) sí caben en unos miles de líneas si se simplifica.

---

## 2. APFS — Apple Filesystem

**Autor principal:** Dominic Giampaolo (antes diseñador de BeFS), equipo Apple CoreOS Storage 2014–2017.
**Anuncio:** WWDC 2016, ship iOS 10.3 (Marzo 2017), macOS High Sierra (Sep 2017).
**Venue:** no hay paper académico. La referencia es el libro *Mac OS X and iOS Internals* / *MacOS and iOS Internals Volume I* de Jonathan Levin (NewOSXBook.com), y la APFS Reference oficial de Apple.
**URL / archive:** <https://developer.apple.com/support/apple-file-system/> · APFS Reference PDF <https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf> · archive <https://web.archive.org/web/2023/https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf>.

### Arquitectura

- **Container / volume model**: un *APFS container* ocupa una partición; contiene uno o más *volumes* que comparten el espacio libre dinámicamente. Parecido a ZFS dataset, pero sin vdev multi-disco (APFS es single-device).
- **CoW todo-a-todo**: metadata y datos; snapshots son operaciones O(1) metadata-only.
- **Snapshots por volumen** via `apfs_snap`: usadas por Time Machine (desde Big Sur), por `Update Brain` del OTA installer, y por SSV (Signed System Volume) desde macOS 11.
- **Crash protection por diseño**: no journal tradicional (no ext4-style JBD2); el mecanismo es una *checkpoint* del superblock: dos superblocks rotando + todas las escrituras son CoW → en crash, el último checkpoint válido se monta.
- **Encryption native**: FileVault usa APFS encryption per-file (key class como iOS data-protection). Per-file keys envueltos con key-class keys envueltos con key encryption key (KEK) atado al Secure Enclave.
- **No checksums de user data por decisión explícita**: Apple argumenta que el hardware (ECC RAM en Macs M-series, controladores NVMe con end-to-end data protection) ya los provee, y que duplicarlo añade write-amplification en SSD sin ganancia. Checksums **sí** para metadata (Fletcher64). Esta es probablemente la decisión de APFS más debatida — Ars Technica (Siracusa) y Howard Oakley han criticado repetidamente esta postura.
- **Space sharing y clones**: `cp -c src dst` hace un clone en O(1) copiando los extents pointers sin duplicar datos (write-on-divergence después). Útil para Time Machine y para desarrolladores.
- **Object-based on-disk layout**: cada nodo es un B-tree con object IDs; cambios van a objetos nuevos y se referencian desde el checkpoint.

### Tamaño

Apple no publica LOC. Estimado por reverse-engineering (Levin, apfs-fuse de Simon Gander): ~50–80 kLOC XNU kernel module + userland. Cerrado: solo `apfs_fuse` (read-only open source) y `apfs-rs` (subset) sirven como referencia externa.

### Notable

- Primer FS de consumo masivo diseñado específicamente para SSD/NVMe (ext4 y HFS+ son agnósticos).
- No soporta hard links en carpetas (simplificación deliberada).
- Migración online HFS+ → APFS in-place para 1.3 billion iOS devices fue un logro logístico enorme (iOS 10.3, sin pérdida de datos reportada masivamente).

---

## 3. Btrfs — B-tree filesystem

**Autor:** Chris Mason (Oracle 2007, luego Facebook/Meta 2012+), con contribuciones grandes de Josef Bacik, David Sterba, Qu Wenruo, Filipe Manana.
**Anuncio/paper:** Mason, *"Btrfs Design"*, wiki.kernel.org 2007+.
**Venue:** papers académicos Rodeh+Bacik+Mason, *"BTRFS: The Linux B-tree filesystem"*, ACM TOS 2013.
**URL / archive:** <https://btrfs.readthedocs.io/> · paper <https://dominoweb.draco.res.ibm.com/reports/rj10501.pdf> · archive <https://web.archive.org/web/2023/https://btrfs.readthedocs.io/>.

### Arquitectura

- **CoW B-trees everywhere**: toda metadata (y opcionalmente datos) vive en B+ trees; cada modificación propaga nuevos nodos hacia arriba hasta la raíz del *chunk tree* → escritura de la raíz del superblock = commit atómico.
- **Subvolumes** (`btrfs subvol create`): namespaces independientes dentro del mismo FS, cada uno con su propio root. Snapshots son subvolumes clonados en O(1).
- **Multi-device desde dentro**: no necesita LVM/mdadm; `btrfs device add/remove`, `btrfs balance`, profiles RAID0/1/10/5/6 seleccionables **por separado** para datos y para metadata (ej. `-d raid0 -m raid1` = datos rápidos, metadata redundante).
- **RAID5/6 inestable**: documentado oficialmente como "may encounter transient errors / write hole may be possible" en status page. Facebook usa RAID1 + RAID10; RAID56 sigue evitado en prod después de ~15 años.
- **Compresión transparente**: LZO / Zstd / ZLIB seleccionable por-mount, por-archivo (con `chattr +c`), o auto.
- **Checksums**: CRC32C (default), con opciones xxhash64, SHA-256, BLAKE2 (desde 2020). Para metadata siempre; para datos configurable.
- **Send/receive**: replicación incremental tipo ZFS (`btrfs send -p old new | btrfs receive`).
- **Deduplication**: offline (`duperemove`, `bees`); no online.

### Estado de producción

- **Facebook/Meta**: desplegado en ~2016 en flota compuesta (Jim Jagielski, Chris Mason charlas LWN), aparentemente con cientos de miles de nodos. RAID1 + snapshots para rollback rápido.
- **SUSE** usa Btrfs como root FS default de SLES desde 2014; snapper (integración con zypper) es la killer-app aquí.
- **Fedora** cambió a Btrfs default en F33 (Oct 2020), aún es default en 2026.
- **Synology** y otros NAS vendors lo usan extensamente.

### Red Hat drama

Red Hat marcó Btrfs "deprecated" en RHEL 7.4 (2017), lo removió del instalador en RHEL 8 (2019). Razones técnicas citadas (a través de Ric Wheeler, entonces RH storage lead): "we don't have the expertise in-house, and maintaining it upstream requires dedicated engineers". Red Hat apostó por XFS + Stratis + Ceph en su lugar. **No hay reinstalación conocida hasta 2026** — el status sigue "available in community repos via ELrepo, not supported". Meta, SUSE, Fedora siguen sin ellos.

### Tamaño

`fs/btrfs/` en Linux 6.7 ~150 kLOC (solo el kernel module), más `btrfs-progs` userland ~80 kLOC. Similar orden de magnitud a ZFS.

### Qué salió mal

- Tiempos de balance multi-TB son brutales (múltiples días en pools grandes).
- "No space left on device" con disco lleno de snapshots + fragmentación → requiere `btrfs balance start -dusage=50` periódico, o el pool se muere.
- Reports de corrupción silenciosa en power-loss con NVMe baratos que mienten sobre fsync (culpa del hardware, pero impacto visible en Btrfs).
- Lenta convergencia de `raid56`.

---

## 4. bcachefs

**Autor:** Kent Overstreet — ex-Google, mantenedor de bcache (Linux 3.10, 2013).
**Origen:** bcache (block-cache para acelerar HDD con SSD) → bcachefs "full FS" anunciado 2015, acepted Linux 6.7 (Enero 2024).
**Venue:** LWN cobertura continua; Overstreet docs en <https://bcachefs.org/>.
**URL / archive:** <https://bcachefs.org/> · LWN merge <https://lwn.net/Articles/934640/> · archive <https://web.archive.org/web/2024/https://bcachefs.org/>.

### Arquitectura

- CoW, checksums (xxhash, CRC32C, SHA-256), encryption (ChaCha20-Poly1305), compresión (LZ4, gzip, zstd).
- **Tiered storage nativo**: el diseño original de bcache escalado a FS completo — cada disco tiene una *target* (foreground, background, promote) y los datos migran automáticamente entre tiers según heurística de uso.
- **Erasure coding** (N+M), snapshots, send/recv, subvolumes. Promete "todo lo que ZFS hace pero in-tree Linux y GPL".
- **Locking design**: Overstreet defiende que su locking es fundamentalmente más simple que Btrfs (menos contention paths).

### 2025 drama Linus vs. Kent

Resumen fáctico verificado hasta 2026-04: bcachefs mergeado Linux 6.7. Durante 2024, tensiones repetidas Overstreet ↔ Torvalds por pull requests fuera-de-merge-window, tone en LKML, y fixes bcachefs que Kent quería meter out-of-cycle. A finales 2024 / inicios 2025 Linus acordó con Kent mantener bcachefs **out-of-tree** a partir de Linux 6.17 o similar (exacto release bajo disputa) por el proceso, no por el código. Kent sigue manteniéndolo como módulo DKMS.

**Estado 2026-04-22:** bcachefs es oficialmente "experimental" y aparentemente out-of-tree desde algún punto en 2025. El proyecto vive; reviews de filesystems 2026 lo siguen nombrando como "lo que podría reemplazar a Btrfs si sobrevive al drama". Los mirror sources están en <https://evilpiepirate.org/git/bcachefs.git>.

### Lecciones para ALZE

- Un FS moderno tarda **~10 años desde anuncio hasta mergeable**, incluso con autor dedicado full-time.
- Locking correcto sigue siendo el tema más difícil (exactamente el blocker R2 en ALZE).
- Kent es franco: "bcache → bcachefs doubled the code every 2 years".

### Tamaño

`fs/bcachefs/` ~100 kLOC en Linux 6.7 cuando fue merged. En 2026 probablemente ~120–150 kLOC.

---

## 5. XFS

**Autor original:** SGI (Silicon Graphics Inc.), 1993 para IRIX. Equipo: Adam Sweeney, Doug Doucette, Curtis Anderson, Mike Nishimoto, Geoff Peck.
**Paper:** Sweeney et al., *"Scalability in the XFS File System"*, USENIX 1996 — <https://www.usenix.org/legacy/publications/library/proceedings/sd96/sweeney.html>.
**Port Linux:** 2001 (Christoph Hellwig + SGI folks). Mantenedor 2010s+: Dave Chinner (Red Hat); Darrick Wong (Oracle, luego RH) lidera changes 2016+.
**URL / archive:** <https://xfs.org/> · <https://docs.kernel.org/filesystems/xfs/> · archive <https://web.archive.org/web/2024/https://xfs.org/>.

### Arquitectura

- **Journaling, NO CoW**: journal de metadata solamente (por defecto); datos no journaled → requiere apps que hagan `fsync()` para durability.
- **B+ trees en todos los sitios**: extent allocation, free space, directories, inodes — todo B+ tree. No bitmap-of-blocks como ext2/4.
- **Allocation groups**: el FS se divide en 2–64 AGs independientes, cada uno con sus propios B+ trees → paralelismo interno. Uno de los motivos por los que XFS escala mejor que ext4 en volúmenes grandes.
- **Extent-based**: un archivo es una lista de `(offset, length, disk_block)`. ext4 también adoptó esto (ext4 extents, 2008).
- **Delayed allocation**: igual que ZFS/Btrfs — se reserva espacio al escribir pero la ubicación final se decide al flush → menos fragmentación.
- **Metadata CRC32C**: añadido 2013 (kernel 3.10). No E2E sobre datos — usa mismo argumento que APFS: "hardware lo hace".
- **Reflinks** (2018, kernel 4.9): `cp --reflink=always` hace copy-on-write at file level. Con reflinks + `xfs_io` se pueden hacer snapshots por archivo.
- **Online defrag** (`xfs_fsr`), **online grow** (`xfs_growfs`); no shrink.
- **Volúmenes masivos**: en prod > 500 TB comunes (Isilon OneFS históricamente, varios sistemas HPC).

### Estado

Default de RHEL desde 7 (2014). RH apuesta oficial. 30+ años en producción. Muy pocos "here-be-dragons"; lo que más duele es que sin reflinks (muchas distros aún default-off) `cp -r` copia datos.

### Tamaño

`fs/xfs/` en Linux 6.7 ~90 kLOC. `xfsprogs` userland ~60 kLOC.

### Filosofía

XFS es el filesystem "journaling hecho bien" — no CoW, pero cada subsistema pensado para concurrencia y escalabilidad desde el día 1. Si ALZE quiere estabilidad antes que features, XFS es el modelo a imitar (no Btrfs).

---

## 6. F2FS — Flash-Friendly File System

**Autor:** Changman Lee (Samsung) + equipo SS&LAB Seoul, 2012. Merged Linux 3.8 (2013).
**Paper:** Lee et al., *"F2FS: A New File System for Flash Storage"*, USENIX FAST 2015 — <https://www.usenix.org/conference/fast15/technical-sessions/presentation/lee> · archive <https://web.archive.org/web/2023/https://www.usenix.org/conference/fast15/technical-sessions/presentation/lee>.

### Arquitectura

- **Log-structured filesystem** (LFS-based, heredero de Sprite LFS de Rosenblum/Ousterhout 1992): todas las escrituras son append-only a un segmento activo; cleaner/GC recicla segmentos viejos.
- **NAT (Node Address Table)**: redirección de inode numbers a locations on-disk → evita el "wandering tree problem" de LFS clásico (actualizar un archivo profundo rewrite-cascadea hasta raíz).
- **Six logs** (hot/warm/cold × node/data): separa metadata caliente, datos fríos, etc. → GC barato porque segmentos son homogéneos en age.
- **Multi-head logging**: paraleliza writes para NAND flash multi-plane.
- **Online fsck** (`fsck.f2fs`) con consistent points.

### Uso

- Android usa F2FS como `/data` default en muchos dispositivos Samsung, OnePlus, Xiaomi desde ~2016. ChromeOS lo adopta en 2020.
- Buen fit para eMMC/UFS: SSD/flash tienen FTL que hace wear-leveling, pero el FS encima sigue beneficiándose de sequential writes y segment alignment.

### No usa

- Checksums E2E de datos (solo CRC en metadata).
- No snapshots nativos (hay patches `f2fs-tools` parciales).
- No encryption propia (usa dm-crypt / ext4 encryption).

### Tamaño

`fs/f2fs/` ~30 kLOC Linux 6.7 — **significativamente más pequeño que Btrfs/XFS/bcachefs**, lo que lo hace un buen caso estudio para "FS moderno mínimo".

---

## 7. Ceph — distributed object storage

**Autor:** Sage Weil, tesis PhD UCSC 2007 (supervisores Scott Brandt, Carlos Maltzahn).
**Paper seminal:** Weil et al., *"Ceph: A Scalable, High-Performance Distributed File System"*, OSDI 2006 — <https://www.usenix.org/legacy/event/osdi06/tech/weil.html> · archive <https://web.archive.org/web/2024/https://www.usenix.org/legacy/event/osdi06/tech/weil.html>.

No es un FS local; es un **object store distribuido** que se expone via:
- **RADOS** (core object store)
- **CephFS** (POSIX FS encima de RADOS)
- **RBD** (block device)
- **RGW** (S3-compatible gateway)

### Arquitectura local

- **BlueStore** (2016, reemplazo de FileStore): los OSDs (Object Storage Daemons) escriben **directo a bloque**, saltándose el FS local. RocksDB para metadata, userspace.
- **CRUSH**: algoritmo pseudo-random determinístico para colocar objetos en OSDs — no hay metadata centralizada de "dónde está cada objeto", solo la `CRUSH map` compartida + object name + hash.
- Replicación 3x default, o erasure coding.

### Relevancia para ALZE

Cero directa — ALZE es uniprocessor single-node. Pero **la idea BlueStore** (FS = userspace object store sobre raw device + DB para metadata) es interesante como arquitectura: el kernel expone raw block + un DB de espacio libre → userland implementa el resto. Si ALZE persigue microkernel, BlueStore-style FS en userland es la referencia moderna.

---

## 8. NILFS2 — log-structured con continuous snapshots

**Autor:** Ryusuke Konishi + equipo NTT Labs Japan, 2005–2009. Merged Linux 2.6.30 (2009).
**Paper:** Konishi et al., *"The Linux Implementation of a Log-structured File System"*, SIGOPS OSR 2006.
**URL:** <https://nilfs.sourceforge.io/> · archive <https://web.archive.org/web/2023/https://nilfs.sourceforge.io/>.

### Qué hace

- LFS puro con **snapshot continuo**: cada segmento sellado es un checkpoint → cualquier momento del pasado es recuperable hasta que el cleaner lo recoja.
- `lscp` lista checkpoints, `chcp` los convierte en snapshots permanentes.
- Diseñado para "time-travel": si escribes cualquier cosa mal, `mount -t nilfs2 -o cp=N` y lees el estado histórico.

### Producción

Nicho. Algunas cámaras IP, algunos embedded. Nunca quitó cuota a ext4/XFS/Btrfs. Pero **conceptualmente es el FS más elegante para "versionado inherente"** — los snapshots no son una feature, son el comportamiento por defecto.

### Tamaño

`fs/nilfs2/` ~20 kLOC Linux 6.7. **El más pequeño de los CoW-ish relevantes.** Candidato fuerte como referencia para ALZE v3.

---

## 9. WAFL — NetApp Write Anywhere File Layout

**Autores:** Dave Hitz, James Lau, Michael Malcolm (NetApp co-founders).
**Paper:** Hitz et al., *"File System Design for an NFS File Server Appliance"*, USENIX Winter 1994 — <https://www.usenix.org/legacy/publications/library/proceedings/sf94/full_papers/hitz.a> · archive <https://web.archive.org/web/2023/https://www.usenix.org/publications/library/proceedings/sf94/full_papers/hitz.a>.

### Por qué importa

**Primer FS comercial con CoW + snapshots.** 1992, cinco años antes de ZFS. Las ideas ZFS/APFS/Btrfs/bcachefs derivan conceptualmente de WAFL:

- Metadata + data son CoW al mismo tiempo.
- Snapshots = guardar un puntero al árbol viejo.
- "Write anywhere": no hay ubicación fija para nada, se escribe donde convenga para performance.
- `.snapshot/` directorios exponen snapshots como subdirs normales — invento NetApp que después heredó ZFS.
- NVRAM battery-backed log para evitar fsync-latency (equivalente conceptual a ZIL).

### Por qué es la referencia ideal para ALZE

El paper de 1994 es **12 páginas** y describe un FS completo con CoW, snapshots, consistency points, y el root-inode-double-update trick. Es el ancestro intelectual de todos los FS modernos y es **el más simple documentado en la literatura**. Si ALZE lee UN solo paper de FS, este es el candidato.

### Cerrado

WAFL es propietario NetApp, cerrado. Solo el paper de 1994 + posts del blog Dave Hitz están disponibles. No hay clones open source directos (ZFS ≈ WAFL rediseñado).

---

## 10. Raíces históricas

### Sprite LFS — el paper fundacional de log-structured

**Autores:** Mendel Rosenblum + John Ousterhout, UC Berkeley.
**Paper:** Rosenblum & Ousterhout, *"The Design and Implementation of a Log-Structured File System"*, ACM TOCS Feb 1992 — <https://people.eecs.berkeley.edu/~brewer/cs262/LFS.pdf> · archive <https://web.archive.org/web/2024/https://people.eecs.berkeley.edu/~brewer/cs262/LFS.pdf>.

Inventó:
- Escribir todo sequentially → amortiza seeks.
- Segmentos + cleaner → el problema del GC de FS.
- Inode map (predecesor del NAT de F2FS).

**Rosenblum luego co-fundó VMware.** Ousterhout es el autor de Tcl/Tk, luego Raft, y profesor en Stanford. Ambos siguen activos 2026.

### ReiserFS — la lección de "performance first, stability later"

**Autor:** Hans Reiser + Namesys, 1996–2006. Merged Linux 2.4 (2001).

- Inventó tail-packing (múltiples archivos pequeños en un bloque) y B+ tree metadata años antes que XFS lo hiciera mainstream en Linux.
- **Reiser4** iba a ser rediseño total con CoW + plugins + transactions → nunca mergeado por disputas de design review con Al Viro et al.
- Reiser fue condenado por asesinar a su esposa (2008); Namesys colapsó. ReiserFS marcado obsolete Linux 5.18 (2022), removed target 2025 — en 2026 está **removed** de mainline.

**Lección:** un FS depende de la salud del equipo mantenedor tanto como del código. Single-maintainer = single-point-of-failure.

### Tux3

**Autor:** Daniel Phillips, 2008+. Nunca mergeado.
- Diseño CoW elegante, atomic commit via "unify" phase, forward logging.
- Stuck en out-of-tree ~15 años. Proof de que Linux mainline tiene umbral muy alto para merging un FS nuevo (confirmed por Btrfs y bcachefs también).

### DEMOS, VMS FS, AdvFS (Tru64), JFS (IBM AIX → Linux)

Conceptualmente importantes pero out-of-scope. JFS en Linux fue mergeado 2002, never got traction, marcado obsolete ~2020.

---

## 11. Tabla comparativa

| Feature | **ZFS** | **APFS** | **Btrfs** | **bcachefs** | **XFS** | **F2FS** | **ext4** |
|---|---|---|---|---|---|---|---|
| CoW | Sí (completo) | Sí (completo) | Sí (completo) | Sí (completo) | **No** (journal) | Sí (LFS) | No (journal) |
| Checksums datos | SHA-256/Fletcher4 | **No** (solo metadata) | CRC32C/SHA256 (opt) | xxhash/SHA256 | **No** (solo metadata) | **No** (solo metadata) | **No** |
| Checksums metadata | Sí | Fletcher64 | CRC32C | Sí | CRC32C (2013+) | CRC | CRC32C (opt) |
| Snapshots | O(1) dataset | O(1) volume | O(1) subvol | O(1) | Via reflinks (por-archivo) | No nativo | No |
| Encryption built-in | Sí (AES-CCM/GCM) | Sí (FileVault) | **No** (usar dm-crypt) | Sí (ChaCha20) | **No** | **No** | Sí (ext4 encrypt) |
| Compresión | lz4/zstd/gzip | LZFSE | lzo/zstd/zlib | lz4/zstd/gzip | **No** | **No** | **No** |
| RAID built-in | Sí (RAID-Z1/2/3, draid) | **No** (single-device) | Sí (0/1/10; 5/6 inestable) | Sí (+ erasure) | **No** (usar mdadm) | **No** | **No** |
| Max size | 2^128 bytes | 8 EiB | 16 EiB | ~16 EiB | 8 EiB | 3.94 TB por archivo / 16 TB FS | 1 EiB |
| Platforms | FreeBSD, Linux, illumos, macOS | macOS, iOS, iPadOS, tvOS, watchOS | Linux | Linux (out-of-tree 2025+) | Linux, IRIX (legacy) | Linux, ChromeOS, Android | Linux |
| LOC kernel | ~250 k (ZFS.ko) | ~60 k (est.) | ~150 k | ~100–150 k | ~90 k | ~30 k | ~50 k |
| Edad prod | 2005 | 2017 | 2013+ (unstable 2009) | 2024 (6.7) | 1993 IRIX / 2001 Linux | 2013 | 2008 |

---

## 12. Aplicabilidad a ALZE — plan v1 / v2 / v3

ALZE tiene hoy ~1750 LOC de FS (VFS + ext2 RO + ramdisk + devfs). El R2 review marcó unsafe: sin locks en hot path, fd-table global. Plan de migración en tres fases, acumulativas:

### v1 — estabilizar ext2-lite (Sprint 1, ~500 LOC añadidos)

**Objetivo:** seguir con el ext2 RO actual, pero *safe*. No añadir features. Cerrar los 21 issues R2.

- Read-lock en `vfs_read/write/ioctl/seek/tell` — copiar `vnode*` a stack dentro del lock, dispatch fuera.
- fd table per-task: mover `fd_table[16]` a `task_struct`. Dup → copiar refs. `fork` → duplicar tabla.
- `ext2_sb_validate()`: rechazar `inodes_per_group==0`, `blocks_per_group==0`, `inode_size ∉ [128, block_size]`, `feature_incompat != 0`, `first_data_block` inconsistente.
- Validar `name_len + 8 <= rec_len` en directory iteration.
- GDT bounds completo: verificar `ceil(groups * sizeof(gdt)/bs) * bs ≤ disk_size`.
- `ramdisk_vfs_read` y `ext2_vfs_read` → `pread` con offset.
- `strlcpy`, `le32_to_cpu` helpers, endianness static-assert.
- Self-test de imágenes ext2 hostiles (div-by-zero, name_len overflow, feature-incompat unknown) — no deben crashear.

Esto cierra R2. **Sin CoW, sin checksums, sin snapshots. Solo unsafe → safe.** Cero features añadidas.

### v2 — añadir journal + CRC metadata (Sprint 2, ~1.5–2 kLOC añadidos)

**Objetivo:** permitir escrituras sin que un crash deje el FS inconsistente.

- Implementar escrituras ext2 básicas (block/inode allocation, dir entries).
- Journal de metadata simple estilo ext3 (JBD clásico): ring buffer de transactions, commit record con CRC, replay al mount.
- CRC32C sobre cada block de metadata (superblock + GDT + inode table + indirect blocks). Misma filosofía que XFS/APFS: "metadata siempre, datos delegados al hardware".
- Reference: paper de Stephen Tweedie 1998 sobre JBD — ~8 páginas, implementación Linux ~3 kLOC.

Esto sale de "ext2 RO" a "ext3 RW mínimo". Sigue sin CoW, sin snapshots. Safe contra crash mid-write.

### v3 — CoW + snapshots vía metadata-ref (Sprint 3–4, ~5–8 kLOC añadidos)

**Objetivo:** top-idea R1 #3 realizada en forma simple.

- **Modelo WAFL-simplificado**: superblock → root inode → B+ tree de inodes → B+ tree de block maps por inode → bloques de datos. Cada edit → escribir nuevo nodo → propagar upward hasta raíz → atomic commit del superblock (rotación dual, como APFS checkpoint).
- **Merkle checksums**: cada puntero en el árbol lleva CRC32C del nodo apuntado. Root del árbol está en el superblock → un único hash a verificar al mount.
- **Snapshots**: `snap_create(name)` = copiar el root pointer del superblock a una tabla de snapshots. O(1). Lectura por snapshot = montar con `root = snapshot_table[name]`. No hay "freeze", solo "no GC this tree".
- **GC simple**: refcount por nodo, decrementar al quitar snapshot, free cuando refcount=0. O bien mark-and-sweep periódico (más lento, más simple).

**No añadir:** RAID-Z, erasure coding, encryption, compression, dedup, pools multi-device. Todo eso es escala de 20+ kLOC adicionales cada uno. **Sí añadir** interfaces vía VFS ops (`vfs_snap_create`, `vfs_snap_list`, `vfs_snap_rollback`) porque son baratos y habilitan tooling userspace.

**Referencia canonical:** leer en orden —
1. Hitz et al. 1994 (WAFL, 12 páginas) — el modelo mental.
2. Rosenblum & Ousterhout 1992 (LFS, ~30 páginas) — el GC y segment cleaner si se quiere LFS puro.
3. Rodeh, Bacik, Mason 2013 (Btrfs TOS) — implementación real con B+ trees CoW.
4. Konishi et al. 2006 (NILFS2) — snapshot continuo elegante, y el FS moderno CoW más pequeño (~20 kLOC).

Si ALZE implementa el subset correcto de WAFL+NILFS2 en ~8 kLOC, estará al nivel funcional del FS de un Mac de 2017 en términos de atomic commits, minus encryption y minus performance. **Eso es suficiente** para un OS de investigación/hobby y cubre la top-idea R1.

---

## 13. Nota honesta — límites

Modern filesystems son **100k–500k LOC cada uno**. ZFS OpenZFS ~300k. Btrfs ~150k. Bcachefs ~150k. XFS ~90k. F2FS ~30k. NILFS2 ~20k. El mínimo viable de "FS serio production-grade" histórico ronda los 20–30 kLOC, y eso ya te obliga a equipo dedicado y ~5 años.

**ALZE no puede portar ZFS.** No solo por LOC — por licencias (CDDL), por dependencias (SPL, userland zpool tools, zed, systemd integration), y porque el modelo "un FS = subsistema aislable" no aplica: ZFS tiene su propio allocator, su propia memory pressure handling, su propio ARC cache — reemplaza cachos enteros de MM que ALZE no tiene.

**Las *ideas* son portables.** CoW transactions, checksummed block pointers formando un Merkle tree, snapshot via freeze-root-pointer, journal de metadata, extent-based allocation, B+ tree dir indexing, reflinks — todas son técnicas de 1–3 kLOC cada una en su forma mínima. WAFL 1994 demostró que un FS CoW **funcional** cabe en un paper de 12 páginas descriptivas; NILFS2 lo demuestra en ~20 kLOC actuales.

El consejo práctico: **leer WAFL + Sprite LFS antes de escribir una sola línea de código nuevo en el FS de ALZE**. Son los dos papers más relevantes para el espacio de diseño al que ALZE aspira. El resto (ZFS/APFS/Btrfs/bcachefs) son elaboraciones — útiles para entender tradeoffs, pero no los documentos de los que copiar la arquitectura inicial.

Y reiterar lo obvio: v1 (estabilizar + cerrar R2) viene **antes** que v2 o v3. Un FS "con snapshots CoW" pero con el fd-table global y sin locks en hot path es peor que el ext2-RO de hoy, porque añade superficie a bugs concurrentes sin resolver los existentes.

---

## Referencias — checklist

- [x] Bonwick & Ahrens — ZFS, Sun 2003/paper ~2006 — OpenZFS docs.
- [x] Giampaolo — APFS — Apple APFS Reference + Levin *macOS Internals*.
- [x] Mason + Rodeh + Bacik — Btrfs — wiki.kernel.org + ACM TOS 2013.
- [x] Overstreet — bcachefs — bcachefs.org + LWN merge coverage 2024.
- [x] Sweeney et al. — XFS — USENIX 1996.
- [x] Lee et al. — F2FS — USENIX FAST 2015.
- [x] Weil et al. — Ceph — OSDI 2006.
- [x] Konishi et al. — NILFS2 — SIGOPS OSR 2006.
- [x] Hitz et al. — WAFL — USENIX 1994.
- [x] Rosenblum & Ousterhout — Sprite LFS — ACM TOCS 1992.
- [x] Historical: Reiser, Tux3 — LWN coverage + wiki.

Fin.
