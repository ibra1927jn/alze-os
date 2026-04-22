# Async I/O models — deep dive for ALZE OS

**Round:** 3 (kernel subsystems deep)
**Fecha:** 2026-04-22
**Autor:** agent (research-only)
**Longitud objetivo:** 300-500 líneas

R1 top idea #2 fue "ring buffer I/O + scheduler pluggable". Este documento
hace el deep-dive de la parte I/O. R1 (`linux_mainstream.md`,
`windows.md`, `macos.md`) cubrió io_uring / IOCP / kqueue a nivel
summary. Aquí entramos en el nivel de estructuras de datos, opcode
tables, historial de CVEs, y qué se puede robar para un kernel hobby
como ALZE. No se repiten fragmentos ya presentes en R1; se asume leído.

Estado ALZE (R2 review): **no hay async I/O.** Todo es blocking.
`read()`/`write()` en VFS son síncronos, la consola bloquea, el driver
ATA hace PIO. El scheduler tiene `waitqueue.h` pero no se usa desde
syscalls de I/O. Pregunta central: ¿vale la pena ring-ificar la ABI
ahora, o en v2, o nunca?

---

## 1. io_uring internals (Linux 5.1+, Jens Axboe, 2019)

El origen: Axboe, entonces mantainer de block layer Linux, publicó el
whitepaper "Efficient IO with io_uring" en enero 2019 (`kernel.dk`,
revisado 2020). Merge en Linux 5.1 (mayo 2019). Diseñado explícitamente
para resolver las tres heridas de Linux AIO: O_DIRECT-only, setup cost,
opcode zoo incoherente.

### 1.1 Arquitectura de rings

Dos ring buffers circular **compartidos** entre kernel y userspace via
`mmap` de un fd devuelto por `io_uring_setup(2)`:

- **SQ (Submission Queue)**: userspace escribe SQEs, kernel los
  consume. El array de SQEs es separado del ring de índices — el ring
  contiene índices `u32` dentro del array. Esto permite al userspace
  preparar SQEs en cualquier orden y hacer el commit batch via el
  índice ring. Tamaño `nr_entries` potencia de 2, típico 128-4096.
- **CQ (Completion Queue)**: kernel escribe CQEs, userspace los
  consume. No hay ring indirection — los CQEs se escriben directamente
  en el ring. CQE = 16 B (user_data 8 B + res 4 B + flags 4 B) o 32 B
  con `IORING_SETUP_CQE32`. Típicamente `2 * SQ size` para evitar
  overflow.

Los tres mapeos `mmap` (SQ ring, CQ ring, SQEs array) tienen offsets
definidos por `io_uring_params.sq_off/cq_off` devueltos en setup. Una
app bien escrita los `mmap`ea con `MAP_POPULATE` para evitar page
faults en steady state.

**Memory ordering:** productor hace `smp_store_release` en tail,
consumidor hace `smp_load_acquire`. En x86 el release/acquire son
no-op en el compilador (x86 TSO), en ARM son `stlr`/`ldar`. La cabeza
del ring es modificada solo por el consumidor — no hay CAS, son stores
simples más fences.

### 1.2 SQE / CQE format

**SQE** (64 B, 5.1; 128 B con `IORING_SETUP_SQE128`): `u8 opcode` (→256
opcodes max), `u8 flags` (IOSQE_*), `u16 ioprio`, `s32 fd`, `u64 off`,
`u64 addr` (buffer), `u32 len`, union de `rw_flags/msg_flags/timeout_flags/
accept_flags/...` según opcode, `u64 user_data` (opaco, eco en CQE),
`u16 buf_index/buf_group`, `u16 personality`, padding a 64 B.

**CQE** (16 B, 32 B con `CQE32`): `u64 user_data`, `s32 res` (bytes / fd /
-errno), `u32 flags` (`CQE_F_BUFFER`, `F_MORE` multishot, `F_SOCK_NONEMPTY`,
`F_NOTIF`).

IOSQE flags clave: `FIXED_FILE`, `IO_DRAIN` (barrera), `IO_LINK` /
`IO_HARDLINK` (encadena siguiente), `ASYNC` (force io_wq), `BUFFER_SELECT`
(kernel escoge buf del pool), `CQE_SKIP_SUCCESS`.

### 1.4 Submission path

Flujo: (1) user llena `sqes[idx]`; (2) `sq_array[sq_tail & mask] = idx`;
(3) `smp_store_release(sq_tail, sq_tail+1)`; (4) opcional
`io_uring_enter(fd, to_submit, min_complete, flags)`.

**SQPOLL**: kernel thread `io_sq_thread` poll-loop sobre `sq_tail`, no
syscall en submit. Timeout idle 1s; `IORING_ENTER_SQ_WAKEUP` para
reactivar. Quema 1 core 100% pero submit ~50 ns vs ~400 ns del syscall.

**IOPOLL** (solo `O_DIRECT` + drivers con `iopoll`, NVMe el target):
kernel poll directo al HW queue, no interrupt. Latencia ~3 µs vs ~8 µs
interrupt+context-switch.

### 1.5 Kernel-side: `io_kiocb`, `io_wq`, task_work

- `struct io_kiocb` = request descriptor; embeds `struct kiocb` + async
  state (list ptrs, retry, deferred completion, refcount). Slab cache
  per-ring.
- `io_wq` = workqueue unbound per-ring. SQEs que no completan inline
  (block on disk/acquire) se offload a worker thread. Desde 5.11 los
  workers son kernel threads normales (`iou-wrk-<pid>` en `ps`).
- **task_work**: completions entregadas al thread dueño del ring via
  `task_work_add()`, procesadas al volver a userspace o bloquear.
  Cache locality + sin wakeups gratuitos.

### 1.6 Registered resources (hot path)

- `REGISTER_FILES` — tabla fds fija, SQE usa índice + `IOSQE_FIXED_FILE`.
  Evita `fdget()` + refcount. Accept multishot escribe directo a slot.
- `REGISTER_BUFFERS` — pin GUP una vez, ref por index+offset. Elimina
  `iov_iter_get_pages()` per-op.
- `REGISTER_RING_FDS` (5.18+) — cache del fd del ring mismo.
- `REGISTER_PBUF_RING` (5.19+) — provided buffer ring.
- `REGISTER_IOWQ_AFF` — pin io_wq a CPUs específicos.

---

## 2. io_uring opcode catalog (5.1 → 6.11, 2019-2026)

~60 opcodes en 2026. Lista agrupada por familia:

**File I/O:** `NOP`, `READV`, `WRITEV`, `READ`, `WRITE`, `READ_FIXED`,
`WRITE_FIXED`, `READ_MULTISHOT` (6.7), `FSYNC`, `FALLOCATE`, `FADVISE`,
`SYNC_FILE_RANGE`, `SPLICE`, `TEE`, `COPY_FILE_RANGE` (6.7),
`FTRUNCATE` (6.9).

**Filesystem:** `OPENAT`, `OPENAT2`, `CLOSE`, `STATX`, `LINKAT`, `UNLINKAT`,
`RENAMEAT`, `MKDIRAT`, `SYMLINKAT`, `FSETXATTR`/`SETXATTR`,
`FGETXATTR`/`GETXATTR`.

**Network:** `SENDMSG`, `RECVMSG`, `SEND`, `RECV`, `SEND_ZC` (6.0),
`SENDMSG_ZC` (6.1), `ACCEPT`, `CONNECT`, `SHUTDOWN`, `SOCKET` (5.19),
`BIND` (6.11), `LISTEN` (6.11), `ACCEPT` multishot (5.19),
`RECV` multishot (6.0), `RECVMSG` multishot (6.0).

**Polling/timers:** `POLL_ADD`, `POLL_REMOVE`, `POLL_UPDATE` (multishot
via `IORING_POLL_ADD_MULTI`, 5.13), `TIMEOUT`, `TIMEOUT_REMOVE`,
`LINK_TIMEOUT` (timeout attaché al SQE previous).

**Control:** `ASYNC_CANCEL`, `FILES_UPDATE`, `MSG_RING` (5.18 — enviar
CQE a otro ring, útil para thread pools), `URING_CMD` (5.19 —
opcode genérico per-driver, e.g. NVMe passthrough), `FUTEX_WAIT`,
`FUTEX_WAKE`, `FUTEX_WAITV` (6.7).

**Provided buffers:** `PROVIDE_BUFFERS`, `REMOVE_BUFFERS`.

**Zero-copy hot path para networking** (2024-2026):
- `SEND_ZC`: notifica complete-with-kernel-done en res, luego
  `CQE_F_NOTIF` cuando page sale de tx queue → refcount drop. Doble
  CQE per SQE. Throughput +30% vs SEND normal en ConnectX-6.
- `SENDMSG_ZC`: igual pero con scatter/gather.
- `RECV` con `IORING_RECV_MULTISHOT` + `PROVIDE_BUFFERS`: un solo SQE
  produce N CQEs hasta EAGAIN. La app nunca hace `recv()` explícito,
  solo consume CQEs con índice de buffer.

**Multishot ops** (no retire el SQE después de 1 completion):
`ACCEPT_MULTI`, `RECV_MULTI`, `POLL_ADD_MULTI`, `READ_MULTISHOT`.
Libera la necesidad de rearm. Cuando el multishot termina (EOF,
buffer pool agotado) marca `!CQE_F_MORE`.

---

## 3. Security history — io_uring como attack surface

io_uring es **grande y complejo** (~25k LOC en `io_uring/` en 6.11).
Cada opcode es una mini-syscall, con sus propias validaciones, y el
modelo async + fixed resources + deferred completion crea race
opportunities que los syscalls síncronos no tienen.

### 3.1 CVE parade (selección)

| CVE | Año | Impacto | Resumen |
|---|---|---|---|
| CVE-2022-2153 | 2022 | NULL deref | `io_sqe_buffer_register` sin validar vmas |
| CVE-2022-1786 | 2022 | Use-after-free | `io_flush_timeouts` vs cancel |
| CVE-2023-0240 | 2023 | UAF | `io_poll_add` + cancel race |
| CVE-2023-2598 | 2023 | OOB write | `io_sqe_buffer_register` + fixed buffers |
| CVE-2023-21400 | 2023 | LPE | race en `io_rsrc_update` |
| CVE-2024-0582 | 2024 | UAF | `io_sq_thread` cleanup race |
| CVE-2024-26718 | 2024 | use-after-free | `__io_req_find_next` concurrent |
| CVE-2024-35888 | 2024 | UAF | `io_uring` cancel + IOPOLL |
| CVE-2024-42284 | 2024 | data race | SQPOLL + drain |
| CVE-2025-21727 | 2025 | UAF | multishot accept + close race |
| CVE-2025-38089 | 2025 | OOB | ring size validation |

Patrón dominante: **UAF y races entre completion path, cancel path, y
resource unregister**. El modelo async implica que un objeto puede
estar "in-flight" en N paths a la vez; cancel + timeout + close concurrent
es el caldo de cultivo.

### 3.2 Respuestas de vendors

- **Google ChromeOS / Android** (2023): desactivó io_uring en
  sandboxes seccomp para apps. Justificación pública: "ataque surface
  grande, beneficio limitado en mobile". Todavía deshabilitado en
  2026.
- **Docker Engine** 23+: deshabilita io_uring en seccomp profile por
  default.
- **Azure Linux / Flatcar**: io_uring disponible pero no recomendado
  para containers untrusted.
- **Kernel Hardened Linux / grsecurity**: io_uring off por default.

### 3.3 Lección ALZE

io_uring es el counter-example perfecto de "feature bello = superficie
de ataque gigante". Linux tiene ~500 devs mirando; ALZE 1-2. **Copiar
diseño, limitar opcode count, review explícito por opcode.**

---

## 4. Windows IOCP (I/O Completion Ports, 1994)

NT 3.5 (1994), ~7 años antes que epoll y ~5 antes que kqueue. Dave
Cutler / NT team. IOCP es **el modelo completion-based original**.

### 4.1 Arquitectura

- **HANDLE** vía `CreateIoCompletionPort(handle, port, key, threads)`.
  Primera llamada crea el puerto; llamadas subsiguientes asocian
  HANDLEs (file, socket, named pipe, mailslot) al puerto.
- **Thread pool asociado al puerto**: el kernel mantiene un count
  configurable (default = NumberOfProcessors). Si hay >N threads
  ready, solo N ejecutan; el resto bloquea en
  `GetQueuedCompletionStatus`.
- **Overlapped I/O**: `ReadFile`/`WriteFile`/`WSARecv`/`WSASend` con
  `OVERLAPPED*` no bloquean; return `FALSE` + `GetLastError() ==
  ERROR_IO_PENDING`. Cuando completa, el kernel enqueue una
  "completion packet" (size, key, overlapped*) en el IOCP.
- `GetQueuedCompletionStatus(port, &size, &key, &ov, timeout)` —
  blocks worker. Retorna el próximo packet. API essentially idéntica
  al loop io_uring-style: "dame el próximo completion."
- `PostQueuedCompletionStatus` — inyectar user packets (útil para
  shutdown signals).

### 4.2 Scaling features

- **Thread pool autoscaling**: kernel wakes exactly 1 thread por
  completion; si bloquea en handler, wake un extra (el "magic number"
  regulator).
- **LIFO wake order**: last-blocked thread wakes first (cache warmth);
  cold threads retiran.
- **GetQueuedCompletionStatusEx** (Vista+) — batch dequeue hasta N
  packets, cierra gap con io_uring CQ batch.
- **Thread Pool API** (`CreateThreadpoolIo`, Vista+) — abstrae IOCP;
  Rust/Go/Node en Windows usan esto internamente.

### 4.3 Comparación con io_uring

| | io_uring | IOCP |
|---|---|---|
| Edad | 2019 | 1994 |
| Modelo | completion | completion |
| Submission | shared ring (no syscall en SQPOLL) | syscall per op (`ReadFile`, `WSASend`) |
| Completion | shared ring | syscall (`GetQueuedCompletionStatus[Ex]`) |
| Batch submit | nativo | no (GQCSEx batcheet solo dequeue) |
| Zero-copy | fixed buffers + SEND_ZC | `TransmitFile`, `WSASendMsg` + WSABUF |
| Fixed FDs | `IORING_REGISTER_FILES` | HANDLE ya es kernel object, no hay equivalente |
| Thread pool integration | via io_wq kernel | built-in, kernel-managed |
| Cross-fd polling | POLL_ADD | no nativo (usa `WaitForMultipleObjects`) |
| Multishot | sí (5.13+) | no explícito, pero infinite recv funciona natural |
| Complejidad kernel LOC | ~25k | ~10k (estimado, sources cerradas) |
| CVE history | 20+ en 5 años | handful en 30 años |

**Veredicto:** IOCP es más simple, más maduro, menos superficie. io_uring
es más rápido en el límite (batch submit + SQPOLL) pero a costo de
complejidad enorme. Para un kernel hobby **IOCP es mejor referencia**.

### 4.4 Ref Jeff Richter

"Windows via C/C++" 5ª ed (2007), cap 10 "Synchronous and Asynchronous
Device I/O" — es el texto canónico del lado userspace. Mark
Russinovich "Windows Internals" parts 1-2 (edición 7, 2017-2021) tiene
el lado kernel: `IoCompleteRequest`, `KeInsertQueue`, KQUEUE object.

---

## 5. Linux AIO (libaio) — el intento fallido

Linux 2.5 (2002), Benjamin LaHaise (Red Hat). API:
`io_setup/io_submit/io_getevents/io_cancel` + `struct iocb` array
copiado per submit.

**Por qué io_uring lo reemplazó:**
1. **Solo O_DIRECT funciona async.** Buffered I/O cae a `wait_on_page_locked`
   sync. Async sólo para disk direct — <5% de workloads reales.
2. **`io_submit` bloquea en metadata.** Inode miss / extent alloc → block.
3. **Opcode zoo limitado:** sólo `PREAD/PWRITE/FSYNC/FDSYNC/PREADV/
   PWRITEV/POLL`. No accept, no openat, no stat.
4. **Copy-in cost.** `iocb` array copiado userspace→kernel en cada
   submit. Sin shared ring.
5. Bugs crónicos en `io_destroy` teardown.

Axboe (2019): "AIO has been a disappointment since day one. It's limited,
it's complicated, and it doesn't scale." **Lección ALZE:** no mezclar
"async" con "solo direct I/O". Si async, async siempre.

---

## 6. Readiness models: epoll / kqueue / event ports

**Categorización:**
- **Readiness:** "tell me when fd is readable/writable", then I still
  do the read() myself. Completion lives in userspace code.
- **Completion:** "do this op, tell me when done". Kernel did the read.

| | epoll | kqueue | event ports | IOCP | io_uring |
|---|---|---|---|---|---|
| OS | Linux 2.5.44 (2002) | FreeBSD 4.1 (2000), macOS | Solaris 10 (2004) | NT 3.5 (1994) | Linux 5.1 (2019) |
| Modelo | readiness | readiness | readiness | completion | completion |
| LT/ET | both (EPOLLET) | ET-like (EV_CLEAR) | LT | n/a | n/a |
| Edge-trigger default | no | no (explicit) | no | n/a | n/a |
| Exotic events | no (fd only) | **yes** (signals, proc, vnode, timer, user) | signals, timers, aio | limited | timers, poll, futex, msg_ring |
| Thundering herd fix | `EPOLLEXCLUSIVE` (4.5) | built-in | built-in | built-in | N/A (pull model) |
| Dup fd behavior | linked (bug hist.) | clean | clean | n/a | registered files |

### 6.1 LT vs ET, thundering herd

**LT** (level-triggered): "fd readable ahora", re-aparece mientras no
drene. Default epoll. **ET** (edge): "cambio not-ready→ready", 1 vez.
Más rápido, más peligroso (drenar hasta EAGAIN o cuelga). nginx usa ET.

**Thundering herd:** N workers `epoll_wait`-ing mismo listen socket,
accept → todos wake, 1 gana, N-1 vuelven. Fix: `EPOLLEXCLUSIVE` (4.5,
2016) o `SO_REUSEPORT` + epoll fd por worker.

### 6.2 kqueue event types únicos

kqueue nativo (epoll necesita fd-por-tipo separado): `EVFILT_SIGNAL`
(reemplaza signalfd), `EVFILT_PROC` (fork/exec/exit — Linux usa pidfd
5.3+), `EVFILT_VNODE` (inotify separado en Linux), `EVFILT_TIMER`,
`EVFILT_USER` (cross-thread wakeups).

Lemon BSDCan 2001 es el paper canónico. Argumenta kqueue > epoll por
event type unification. 25 años después kqueue sigue más limpia; Linux
parcheó con zoo (signalfd/pidfd/timerfd/inotifyfd/eventfd).

---

## 7. User-space libraries

### 7.1 liburing (Axboe, ~2019+)

El wrapper oficial. `io_uring_queue_init`, `io_uring_get_sqe`,
`io_uring_prep_*(sqe, ...)` por opcode, `io_uring_submit`,
`io_uring_wait_cqe`, `io_uring_cqe_seen`. Low-overhead — inline
functions, no malloc. El fork/extensions están en github.com/axboe/
liburing, releases cada 2-4 semanas.

### 7.2 libxev (Mitchell Hashimoto, Zig, 2022+)

Cross-platform event loop en Zig. Backends: io_uring (Linux), kqueue
(macOS/BSD), IOCP (Windows), epoll (Linux fallback). API uniforme. Muy
compacto (~10k LOC total). Referencia para "cómo abstraer los 4 modelos
modernos sin perder el zero-cost."

### 7.3 mio + tokio (Rust) y async/await

- **mio**: low-level event loop 1-thread cross-platform (usado por tokio).
- **tokio**: runtime async/await. Multi-threaded work-stealing,
  IOCP/epoll/kqueue backends; io_uring sólo opcional rascal, no
  production en 2026.

Tokio no usa io_uring por default: (1) tokio es readiness-based
(`Future::poll(cx)` → Ready/Pending + `Waker`), io_uring es completion,
modelo mismatch; (2) API estable tokio no puede exponer completion sin
breaking changes.

**glommio** (Datadog, Glauber Costa, 2020): runtime Rust completion-based
nativo io_uring, thread-per-core, no work-stealing. Storage workloads.

### 7.4 Go runtime

Go `netpoll` = epoll/kqueue/IOCP según OS. No usa io_uring en 2026
(Keith Randall, GopherCon 2024: "io_uring performance difference is
small for typical Go workloads, not worth the complexity"). Disk I/O =
goroutines bloqueantes + M:N scheduler.

---

## 8. Zero-copy paths

Zero-copy significa "los bytes no se copian dentro del kernel entre
buffers de page cache y buffers de socket." Siempre hay un copy al NIC
(DMA) por definición — zero-copy se refiere al `copy_from_user` y
`skb_copy` internos.

### 8.1 Linux primitives

- **`sendfile`** (2.2, 1999) — file → socket sin userspace. Pages del
  page cache adjuntadas a skb, DMA al NIC.
- **`splice`** — bytes entre 2 fds via kernel pipe buffer, más general
  que sendfile. `SPLICE_F_MOVE` intenta page-steal.
- **`vmsplice`** — pages userspace → pipe; splice-out → zero-copy
  desde process memory.
- **`MSG_ZEROCOPY`** (4.14, 2017) — send flag, pin pages, notif via
  errqueue (`MSG_ERRQUEUE` + `SO_EE_ORIGIN_ZEROCOPY`). User no debe
  tocar buffer hasta notif. +20-40% throughput en sends >10 KB.
- **`TCP_ZEROCOPY_RECEIVE`** (4.18, 2018) — getsockopt API. Kernel mapea
  pages NIC → userspace. Requiere buffers aligned 4 KB + NIC con
  header/payload split. Netflix, YouTube.

**Costs:** page pinning (GUP, compaction interrupted, memcg pressure),
completion callback (flow control más sutil), alineación 4 KB. Para
<4 KB zero-copy pierde. Regla Netflix: zero-copy >16 KB.

**io_uring + zero-copy:** `SEND_ZC` (6.0) integra `MSG_ZEROCOPY` en ring
— CQE inicial "kernel done", CQE `F_NOTIF` "NIC done, reuse buffer".
2 CQEs por SQE. `RECV` + provided buffers = equivalente
`TCP_ZEROCOPY_RECEIVE` via ring.

---

## 9. io_uring + networking (2024-2026)

Pasos clave:
- **5.19**: opcode `SOCKET`, `ACCEPT_MULTI` (multishot accept).
  Servidor = 1 SQE, miles de CQEs con nuevos fds.
- **6.0**: `SEND_ZC`, `RECV_MULTI`. Throughput iperf3 single-core en
  ConnectX-7 subió de ~80 Gbps a ~150 Gbps.
- **6.1**: `SENDMSG_ZC`.
- **6.11**: `BIND`, `LISTEN`. Servidor completo sin syscalls
  fuera del ring.

Axboe Netdev 2023 keynote "io_uring and networking": benchmark
200 Gbps en single thread con ConnectX-7 + fixed buffers + multishot
recv + SEND_ZC. Comparación: epoll + sendmsg plateaued ~80 Gbps por
core.

**Ring + tx queue coherente:** el diseño clave es que el ring de SQEs
se mapea casi 1:1 con la hardware tx queue del NIC. Doorbell de ring
= doorbell de NIC (con xdp sockets / AF_XDP, no io_uring directo, pero
la arquitectura mental es gemela).

### 9.1 AF_XDP y io_uring

AF_XDP (2018) es otro ring-based I/O path, pero kernel-bypass (userspace
habla directo con NIC ring). No son competidores — AF_XDP es para
packet-level (switches, IDS), io_uring es para socket-level (HTTP
servers). Ideal combo: AF_XDP para dispatching, io_uring para apps.

---

## 10. ALZE applicability

Estado ALZE R2:
- `kernel/vfs.c` tiene `read`/`write` síncrono.
- `kernel/blkdev.c` bloquea en PIO para ATA.
- `kernel/waitqueue.h` existe pero no está conectado a I/O syscalls.
- `kernel/sched.c` tiene priorities pero no I/O-wait detection.
- **No hay epoll, no hay kqueue, no hay io_uring.** Todo blocking.

### 10.1 v1 (0-6 meses) — keep blocking

**NO implementar async I/O.** Razones: (1) async requiere scheduler
robusto (I/O wait accounting, yield correcto, deferred work) — ALZE
scheduler todavía básico; (2) async requiere driver model "start +
callback" — ALZE drivers son sync; (3) security surface enorme para
solo-dev.

Qué sí: `O_NONBLOCK` + `EAGAIN`; un `poll()`-style simple (readiness LT
array fds, ~200 LOC); thread-per-request default — funciona, maintainable.

### 10.2 v2 (6-18 meses) — mini ring ABI, 5 opcodes

Si v1 estable + scheduler listo:
- 2 rings mmap SQ+CQ. SQE 16 B, CQE 16 B (no 64 — ALZE no tiene 50
  opcodes).
- **5 opcodes:** `NOP`, `READ`, `WRITE`, `ACCEPT`, `RECV`. Cada uno
  review seguridad explícito antes de merge.
- **No SQPOLL**, no fixed files, no fixed buffers, no multishot.
  Syscall clásica `alze_ring_enter(fd, n_submit, min_complete)`.
- Workqueue kernel interno para ops que bloquean (reusar
  `workqueue_def.h` ya presente en R2 review).

LOC estimado: ring 2-3k + scheduler integration ~500 + tests ~1k. Doable
en 3-4 semanas focused.

### 10.3 v3 (18+ meses) — expansión cuidadosa

Solo si v2 corre en producción real: `OPENAT/CLOSE/FSYNC/STATX`, fixed
buffers, `SEND` zero-copy (requiere NIC driver ZC), `POLL_ADD` multishot,
linked SQEs. **Tope deliberado: 20 opcodes.** Más que eso es Linux.

### 10.4 IOCP-style vs io_uring-style para ALZE

Si goal = **learning + simplicidad**, IOCP-style gana: 1 syscall submit
+ 1 dequeue, no ring mapping, no memory ordering, ~1k LOC vs ~3k. Menor
surface porque no expone memory compartida userspace↔kernel.

Si goal = **performance state-of-art**, io_uring-style. Pero con 5 op,
no 60.

**Recomendación:** ALZE v2 IOCP-style. v3 puede migrar a ring cuando
VFS/scheduler/drivers estén listos. Windows siguió esa trayectoria 30
años — IOCP desde 1994, nunca migraron a rings, corre workloads
web-scale.

---

## Tabla comparativa async I/O final

| | io_uring | IOCP | Linux AIO | epoll | kqueue |
|---|---|---|---|---|---|
| Modelo | completion | completion | completion | readiness | readiness |
| Año | 2019 | 1994 | 2002 | 2002 | 2000 |
| Ring shared | sí (SQ+CQ) | no (syscalls) | no | no | no |
| Zero-copy nativo | SEND_ZC, fixed bufs | TransmitFile, WSABUF | no | n/a | n/a |
| Buffered file I/O | sí | sí | **no** (O_DIRECT) | n/a | n/a |
| Batching | sí | solo dequeue | parcial | solo dequeue | solo dequeue |
| Cross-fd polling | POLL_ADD | Wait*Objects (ext.) | sí (opc. POLL) | core feature | core feature |
| Signals/procs/vnode | limited | no | no | no | **sí** |
| Security history | 20+ CVE / 5 años | pocos / 30 años | pocos (poco uso) | moderate | pocos |
| HW baseline | kernel 5.1+, x86_64 estándar | NT 3.5+ | kernel 2.6+ | kernel 2.5.44+ | FreeBSD/macOS |
| LOC kernel aprox | 25k | 10k (estimado) | 3k | 2k | 4k |
| Complejidad hobby OS | alta | media-baja | no imitar | baja | media |
| Recomendación ALZE v2 | demasiado ambicioso | **sí** | descartar | reference para poll() v1 | reference para event types |

---

## Referencias primarias

- Jens Axboe — "Efficient IO with io_uring" — kernel.dk, 2019-2020 —
  https://kernel.dk/io_uring.pdf
- Jens Axboe — io_uring blog posts — https://axboe.dk/
- liburing repo + docs — https://github.com/axboe/liburing
- io_uring(7) man page — https://www.man7.org/linux/man-pages/man7/io_uring.7.html
- Jonathan Corbet — "Ringing in a new asynchronous I/O API" — LWN,
  2019 — https://lwn.net/Articles/776703/
- Axboe — "io_uring and networking" — Netdev 0x17, 2023 —
  https://netdevconf.info/0x17/
- Stefan Seyfried — "io_uring reference guide" — https://unixism.net/loti/
- CVE database search io_uring — https://nvd.nist.gov/vuln/search —
  query "io_uring".
- Microsoft Docs — IOCP — https://learn.microsoft.com/en-us/windows/
  win32/fileio/i-o-completion-ports
- Jeff Richter — "Windows via C/C++" 5th ed (Microsoft Press, 2007),
  cap 10 "Async Device I/O".
- Mark Russinovich, David Solomon, Alex Ionescu — "Windows Internals
  Part 1" 7ª ed (2017), cap 6 I/O system.
- Jonathan Lemon — "Scalable event multiplexing: epoll vs kqueue" —
  BSDCan 2001 — https://people.freebsd.org/~jlemon/papers/kqueue.pdf
- Linux AIO — Benjamin LaHaise — fs/aio.c (2.5.32, 2002) — kernel
  source + LWN 2003 "Asynchronous I/O support in Linux" —
  https://lwn.net/Articles/24366/
- MSG_ZEROCOPY — Willem de Bruijn — Linux 4.14, 2017 —
  Documentation/networking/msg_zerocopy.rst
- Rust async/await RFC 2394 — https://rust-lang.github.io/rfcs/2394-async_await.html
- Mitchell Hashimoto — libxev — https://github.com/mitchellh/libxev
- Glauber Costa — glommio — https://github.com/DataDog/glommio
- Tokio runtime — https://tokio.rs/blog/
- Chromium sandbox io_uring disable — https://bugs.chromium.org/p/chromium/issues/detail?id=1447758

---

## Honest note (cierre)

**io_uring es elegante pero tóxico para kernel hobby.** 20+ CVEs en 5
años con cientos de devs mirándolo; un solo-dev no sostiene esa
superficie. Decisión correcta ALZE:

1. **v1: no async I/O.** Blocking + threads. Go hace esto con M:N y
   escala.
2. **v2: si async, IOCP-style.** Más simple, menos surface. 1k LOC vs
   3k. La decisión Microsoft 1994 sigue defendible en 2026.
3. **5 opcodes max v2, 20 max v3.** No matchear Linux — guerra perdida.
4. **Cada opcode con security review explícito pre-merge.** Quién puede
   llamar, qué valida, side effects, races cancel/timeout/close.
5. **Si objetivo = aprendizaje, epoll-style readiness primero.** 200
   LOC, 80% de casos, enseña conceptos sin superficie io_uring.
   Solaris event ports y kqueue son referencias limpias.

Lo peor para ALZE no es quedarse con blocking I/O — es implementar un
io_uring mal con 20 UAFs que el autor nunca encuentra.
**Disciplina > ambición.**
