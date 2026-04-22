# Container primitives + isolation — deep dive

**Ronda:** R5 / `containers_primitives.md`
**Fecha:** 2026-04-22
**Scope:** Linux container primitives (namespaces, cgroups v2, seccomp, LSMs, overlayfs) + runtime stack (Docker/containerd/runc/crun/Kata/gVisor) + OCI spec + Podman rootless + eBPF networking + FreeBSD jails + Windows Silos. Complemento estructural de `r3/capability_kernels.md`: este archivo es la crítica honesta de por qué Linux necesitó *parchear* su ACL con namespaces mientras un kernel cap-puro obtiene contenedores por construcción. Ver §15.

**Reglas ALZE**: v1 no tiene multi-proceso serio → containers irrelevantes. v2 post-caps → "container semantics" caen por gravedad. v3 → opcionalmente "scopes" estilo jails (~3k LOC) si el mental-model lo pide.

---

## 1. Linux namespaces — los ocho tipos

### Génesis

Eric W. Biederman diseña el modelo 2002–2013. MNT es el más antiguo (2.4.19, 2002); UTS/IPC (2.6.19, 2006); PID (2.6.24, 2008); NET (2.6.29, 2008); USER (3.8, 2013) — el más complejo por uid mapping; CGROUP (4.6, 2016); TIME (5.6, 2020, Dmitry Safonov) — el último añadido.

Refs: Biederman, *"Multiple Instances of the Global Linux Namespaces"*, OLS 2006 [landley.net/kdocs/ols/2006](https://landley.net/kdocs/ols/2006/ols2006v1-pages-101-112.pdf). Kerrisk, *"Namespaces in operation"*, LWN 7-part series 2013 [lwn.net/Articles/531114](https://lwn.net/Articles/531114/) — tutorial canónico. `namespaces(7)`.

### Syscalls

- **`clone(CLONE_NEW*, …)`** — hijo nace en nuevos namespaces. Flags: `CLONE_NEWPID/NET/NS/UTS/IPC/USER/CGROUP/TIME`. `clone3()` (5.3+) struct-based.
- **`unshare(flags)`** — proceso actual se desvincula sin forkear.
- **`setns(fd, nstype)`** — entra en ns existente (fd de `/proc/<pid>/ns/<type>`). Base de `nsenter(1)`, `docker exec`.

### Semántica por tipo

| Namespace | Aísla | Peligro |
|---|---|---|
| **MNT** (`CLONE_NEWNS`) | Árbol de mounts; `/` propio | Mount propagation (shared/slave/private) sutil; default "shared" fuga |
| **PID** | PID space; PID 1 dentro reaper | Si PID 1 muere, SIGKILL a todos |
| **NET** | interfaces, IPs, rutas, iptables | Extra indirection en packet path |
| **IPC** | SysV IPC + POSIX mq | POSIX shm needs MNT coordination |
| **UTS** | hostname, domainname | Trivial; el primero históricamente |
| **USER** | uid/gid mapping | El único que permite al non-root crear los demás |
| **CGROUP** | view del cgroup tree | Previene info leak sobre host layout |
| **TIME** | `CLOCK_MONOTONIC`/`BOOTTIME` offset | `CLOCK_REALTIME` sigue global |

### Notas operacionales

- `/proc/<pid>/ns/` = 8 symlinks `nsfs`; comparar inode = same ns.
- USER ns **anidable** hasta 32 niveles (`MAX_USER_NS_LEVEL`).
- `unshare -U -r` crea USER ns con "root inside = you outside" — base rootless.
- Overhead creación ns: pocos µs. Destrucción NET ns lenta (RCU + connection flush). Container startup ~100 ms dominado por cgroups + overlayfs, no ns.
- Debilidades: `/proc` y `/sys` requieren remount o virtualización (virtiofs, lxcfs). Los 8 ns indirections viven en `struct nsproxy` embebida en `task_struct`.

---

## 2. cgroups v2 — unified hierarchy

### Historia

cgroups v1 (Paul Menage + Rohit Seth, Google, 2007): jerarquías independientes por controller, userspace complexity. cgroups v2 (Tejun Heo, Facebook): Linux 4.5 merge (Mar 2016), **default universal 2023** (Fedora 31 en 2019, Debian 11 en 2021, RHEL 9 en 2022).

Refs: Heo, *"Control Group v2"* kernel doc [kernel.org](https://www.kernel.org/doc/html/latest/admin-guide/cgroup-v2.html). Corbet LWN 2014 [lwn.net/Articles/604609](https://lwn.net/Articles/604609/).

### Reglas estructurales v2

1. **Una sola jerarquía** — `/sys/fs/cgroup/` cgroup2 fs.
2. **No thread-granularity** por defecto (salvo "threaded" leaves).
3. **No internal processes** — un cgroup con hijos no tiene procs directos.
4. **Top-down control** — `cgroup.controllers` muestra disponibles, `cgroup.subtree_control` activa para hijos.

### Controllers principales

| Controller | Archivos clave |
|---|---|
| **cpu** | `cpu.weight`, `cpu.max` (CFS/EEVDF backed) |
| **memory** | `memory.max`, `memory.swap.max`, `memory.oom.group` |
| **io** | `io.weight`, `io.max` (BFQ compat) |
| **pids** | `pids.max` (anti-forkbomb) |
| **cpuset** | `cpuset.cpus`, `cpuset.mems` (NUMA pinning) |
| **hugetlb** | `hugetlb.<size>.max` |
| **rdma** | RDMA resources (IB, RoCE) |
| **misc** | scalar arbitrary (GPU, FPGA) |
| **perf_event** | profiling limits |
| **freezer** (v2 2018+) | `cgroup.freeze` — Docker pause, CRIU |

Files per-cgroup: `cgroup.procs`, `cgroup.threads`, `cgroup.events` (populated/frozen). **PSI** (Biederman+Weiner 4.20, 2018): `memory.pressure`, `io.pressure` — fracción stalled. Base de `systemd-oomd` + Facebook `oomd`.

Números: crear cgroup + move proc ~50 µs. Facebook post-2020: todos servers v2. K8s 1.25 (2022) v2 default.

---

## 3. OCI — Open Container Initiative

### Génesis

2015-06: Docker dona format + runtime a Linux Foundation. Se funda OCI con Red Hat, CoreOS, Microsoft, IBM, Google. Tres specs: **Runtime** (bundle = `config.json` + `rootfs/`, v1.2.0 en 2024), **Image** (manifest + config JSON + layers tar.gz), **Distribution** (registry HTTP API, v1.1 en 2024).

### config.json snapshot

```json
{
  "ociVersion": "1.2.0",
  "process": { "args": ["/bin/sh"], "capabilities": {"bounding": [...]}, "noNewPrivileges": true },
  "root": {"path": "rootfs"},
  "linux": {
    "namespaces": [{"type":"pid"},{"type":"network"},...],
    "resources": {"memory":{"limit":536870912}, "cpu":{"shares":1024}},
    "seccomp": {"defaultAction":"SCMP_ACT_ERRNO", "syscalls":[...]},
    "maskedPaths": ["/proc/kcore",...], "readonlyPaths": ["/proc/bus",...]
  }
}
```

El container entero = ese JSON + rootfs.

### Impls

- **runc** — reference, Go ~30k LOC [github.com/opencontainers/runc](https://github.com/opencontainers/runc). Usa libcontainer engine + `nsexec` trampoline (C) para setup pre-Go runtime.
- **crun** — C99 ~15k LOC [github.com/containers/crun](https://github.com/containers/crun) (Scrivano, Red Hat 2018). 50× más rápido que runc en containers pequeños (no Go runtime). Default Podman 2021+. Modelo para ALZE si alguna vez OCI compat.
- **youki** — Rust clone 2021+.
- **kata-runtime** (§6), **runsc** (gVisor §7).

---

## 4. Docker → containerd → runc stack

### 2013–15: monolith

Docker 2013: un binario = daemon + CLI + builder + runtime + registry + volumes. Monorepo Go.

### 2015–: decomposition

1. **runc** extraído 2015 — core exec.
2. **containerd** extraído 2015–16 — lifecycle daemon [github.com/containerd](https://github.com/containerd/containerd), delega exec a runc.
3. **Docker Engine** → UX layer sobre containerd.

### 2020–22: CRI era

Kubernetes **CRI** (Container Runtime Interface, gRPC) es la interfaz estándar. **dockershim** deprecated K8s 1.20 (2020), removed 1.24 (2022). **containerd** default de K8s (GKE/EKS/AKS). **CRI-O** (Red Hat 2017–) = alternativa más fina.

### Stack actual

```
Kubelet → containerd (o CRI-O) → runc (o crun, kata-runtime, runsc, youki) → kernel
            gRPC CRI              OCI runtime spec       namespaces+cgroups+seccomp+LSM
```

Refs: [github.com/containerd/containerd/blob/main/docs/architecture.md]. Moby = upstream Docker Engine post-split [mobyproject.org](https://mobyproject.org/).

---

## 5. Podman + Buildah + Skopeo — Red Hat rootless

- **Podman** (Dan Walsh, Red Hat, 2018–) [podman.io](https://podman.io/). **No daemon**: fork-and-exec per comando. Sin `/var/run/docker.sock` escalation risk. Compat CLI con Docker.
- **Buildah** — construye OCI images sin Dockerfile required.
- **Skopeo** — `copy docker://a.io/img docker://b.io/img` sin pull local.
- **Rootless default**: `newuidmap`/`newgidmap` (setuid helpers) setup USER ns con `/etc/subuid` (~65536 uids). Dentro cree root; fuera es el user. Podman rootless-first; Docker lo retrofit (20.10, Dec 2020).
- **Pods nativos** pre-K8s: `podman pod create`, `podman generate kube` emite YAML k8s.

---

## 6. Kata Containers — microVM isolation

Fusion 2017 de Intel Clear Containers + HyperHQ runV. [katacontainers.io](https://katacontainers.io/).

**Thesis**: namespaces+seccomp comparten kernel host con container → kernel bug escapa (ej. CVE-2019-5736 runc escape). Kata corre cada pod en **microVM** (Firecracker o QEMU-Lite) con kernel propio. Aislación hardware-enforced (VT-x/SVM).

**Stack**: `kubelet → containerd → kata-runtime → kata-shim → kata-agent (in VM) → Firecracker/QEMU → Kata kernel → workload`. virtio-fs para rootfs sharing; vsock para agent comm.

**Trade-offs**: + hardware isolation; + OCI-compat (K8s RuntimeClass). − RAM ~50 MB base/pod; − startup 150–300 ms vs 50 ms runc; − net tap-bridge overhead.

**Deployments**: Alibaba, AWS Fargate (Firecracker directo), Baidu.

Ref: Agache+Brooker+Florescu et al, *"Firecracker: Lightweight Virtualization for Serverless"*, NSDI 2020 [usenix.org/conference/nsdi20/presentation/agache](https://www.usenix.org/conference/nsdi20/presentation/agache). Cross-ref: `r3/virtualization_kvm.md`.

---

## 7. gVisor — user-space kernel

Google, público 2018. [github.com/google/gvisor](https://github.com/google/gvisor), Go ~200k LOC.

**Thesis**: entre "share host kernel" (runc) y "full VM" (Kata) hay punto medio = **kernel en user-space** implementando Linux syscall ABI. Workload hace syscalls al kernel falso ("Sentry"), no al real.

**Arquitectura**: (1) **Sentry** = user-space kernel. Maneja ~240 syscalls, FS virtual, netstack puro-Go. (2) **Gofer** = proceso para FS I/O real (9P protocol). (3) modos de intercepción: **ptrace mode** (kernel host intercepta → Sentry; ~10× overhead) y **KVM mode** (Sentry usa KVM para trap; más rápido, requiere `/dev/kvm`).

**Trade-offs**: + escape requiere break Sentry (Go, memory-safe) + host kernel vía narrow syscall surface. − sólo ~240/400 syscalls (apps raras fallan en `ioctl`, `perf_event_open`). − I/O penalty por Gofer 9P.

**Deploy**: Google Cloud Run, App Engine Standard Gen2. Compite con Kata en "untrusted multi-tenant".

Ref: Young+Hao+Veeraraghavan et al, *"The True Cost of Containing: A gVisor Case Study"*, HotCloud 2019 [usenix.org/conference/hotcloud19/presentation/young](https://www.usenix.org/conference/hotcloud19/presentation/young).

---

## 8. overlayfs — layering filesystem

Miklos Szeredi (FUSE author). Merged Linux 3.18 (Dec 2014). Doc [kernel.org/filesystems/overlayfs](https://www.kernel.org/doc/html/latest/filesystems/overlayfs.html).

**Modelo** union mount: `lowerdir` (RO layers, `:`-separated multi-layer) + `upperdir` (RW) + `workdir` (scratch para atomicity) → `merged` mount point.

```
mount -t overlay overlay -o lowerdir=/l1:/l2,upperdir=/up,workdir=/wk /merged
```

**Semantics**: lookup upper→lowers (first-hit); write triggers **copy-up** (copia lower→upper, luego muta; first-write paga copia full → "no DB files en overlay"); delete = **whiteout** (char device `c 0 0`); opaque dirs via xattr `trusted.overlay.opaque=y`.

**Por qué containers**: Docker image = N RO layers (lowerdirs) + un RW upper per instance. Layers compartidos cross-containers (deduplicate). Crear container = crear upper+work vacíos + mount = O(ms).

**Competidores**: AUFS (Okajima, 2006, nunca mergeó upstream; Docker pre-2014); DeviceMapper+thinpool; btrfs subvolumes / zfs clones; **Composefs** (Larsson+Scrivano, 6.6 merge 2023) optimizado OCI read-mostly via ostree dedup.

Overhead: ~20% metadata ops vs ext4; large-file copy-up lineal.

---

## 9. seccomp-bpf — syscall filtering

seccomp "strict" (Arcangeli 2005): sólo `read/write/exit/sigreturn`. Inusable. **seccomp-bpf** (Will Drewry + Kees Cook, Linux 3.5, 2012): BPF program filtra syscalls por número + arg values. Acciones: `ALLOW`, `KILL`, `ERRNO`, `TRAP`, `LOG`, `USER_NOTIF` (5.0+). LWN [lwn.net/Articles/475043](https://lwn.net/Articles/475043/).

Proceso: `seccomp(SECCOMP_SET_MODE_FILTER, ...)` carga BPF. Hijo hereda; filter additive (no relaja).

**Docker default profile** [github.com/moby/moby/blob/master/profiles/seccomp/default.json](https://github.com/moby/moby/blob/master/profiles/seccomp/default.json): bloquea ~44 syscalls (`add_key`, `keyctl`, `*_module`, `perf_event_open`, `reboot`, `swapon`, `iopl`, etc.), allowlist ~300 common. Mitigó pre-emptivamente al menos dos CVEs (keyctl, user_ns race). K8s 1.27+ seccomp `RuntimeDefault` GA.

---

## 10. LSMs — SELinux + AppArmor

Linux Security Modules (Crispin Cowan 2001, upstream 2.6 en 2003). Hooks (`security_inode_permission` etc.) implementables por LSMs.

- **SELinux** (NSA 1998, upstream 2.6). MAC con **contexts** `user:role:type:level`. Containers: Docker asigna `container_t` + unique **MCS label** (`s0:c123,c456`) → incluso con namespace/DAC escape, MLS bloquea. Loscocco+Smalley OLS 2001 [nsa.gov/selinux](https://www.nsa.gov/research/_files/selinux/papers.shtml).
- **AppArmor** (Canonical/Ubuntu, 2007 upstream). Path-based. Profiles declarativos `/etc/apparmor.d/docker-default`. Menos expresivo, más simple.
- **LSM stacking** (5.1, 2019) — múltiples simultáneos, pero SELinux/AppArmor siguen mutually exclusive (major LSMs).
- **Landlock** (5.13, 2021) — unprivileged sandbox, proceso se auto-restringe, path-based.
- **BPF LSM** (5.7, 2020) — attach BPF a hooks LSM (Cilium Tetragon, Falco).

K8s `securityContext`: `seLinuxOptions`, `appArmorProfile`, `capabilities.drop/add`, `runAsNonRoot`, `readOnlyRootFilesystem` — todo declarativo.

---

## 11. Cilium + eBPF networking

Thomas Graf + Isovalent, Cilium 1.0 (2018). Reemplaza `kube-proxy` (iptables, cientos-miles reglas/Service, linear lookup, 10k pods → 100k reglas → overhead) con **eBPF at TC (traffic control) + XDP (NIC driver level)**.

**Arquitectura**: daemon per-node genera eBPF programs per-policy/per-service. Hooks: **XDP** at NIC (DDoS drop antes de `skb` alloc), **TC ingress/egress** (routing/NAT/LB), **cgroup/connect4** (socket-level NAT sin conntrack). BPF maps = policy state, conntrack, LB backends.

**Beneficios**: sub-ms en packet path; no conntrack overhead east-west; L7 policies (HTTP/Kafka/gRPC) via envoy sidecar; **Hubble** observability via eBPF flow logs. `kubeProxyReplacement: strict` elimina kube-proxy entero. GKE Dataplane V2 = Cilium under the hood.

Ref: Graf KubeCon 2017 [youtube](https://www.youtube.com/watch?v=ilKlmTDdFgk). Docs [docs.cilium.io](https://docs.cilium.io/). [ebpf.io](https://ebpf.io/) umbrella.

---

## 12. Rootless containers

**Motivación**: Docker daemon root + `/var/run/docker.sock` → grupo `docker` = root-equiv → agujero en shared nodes (HPC, CI).

**Modelo**: USER ns + uid-map amplio (65536 via `/etc/subuid`); **slirp4netns** o **pasta** para user-space TCP/UDP; overlayfs rootless via **fuse-overlayfs** (userland, más lento) o **kernel rootless overlayfs** (5.11, 2021 — default moderno).

Podman rootless es diseño original; Docker 20.10 (Dec 2020) GA (retrofit). En 2026 ambos maduros; Podman más limpio.

Ref [rootlesscontaine.rs](https://rootlesscontaine.rs/).

---

## 13. FreeBSD jails — el ancestro

Kamp + Watson, *"Jails: Confining the omnipotent root"*, SANE 2000 [papers.freebsd.org/2000/phk-jails](https://papers.freebsd.org/2000/phk-jails.files/sane2000-jail.pdf). **16 años antes** que Linux namespaces completos.

**Modelo**: `jail(2)` FreeBSD 4.0 (2000). Jail = FS partition + IP(s) + jailname + sysctl restrictions. `jail_attach(jid)` mueve proceso. Descendientes heredan. Monolítico: dentro o fuera, no per-resource.

**Evolución**: **VIMAGE / vnet jails** (7–9) — network stack virtualizado (= NET ns). **Hierarchical jails** (8+). **jail.conf** (2012). Herramientas: iocage, bastille.

### Jails vs Linux ns

| Eje | jail | Linux ns |
|---|---|---|
| Granularidad | monolítico | 8 tipos independientes |
| FS root | `path=/jails/x` obligatorio | MNT ns + pivot_root |
| Network | VIMAGE opt-in | NET ns (veth pair) |
| Rootless | no (jail requires root) | USER ns |
| Resource limits | RACL ortogonal | cgroups v2 |
| Filosofía | "un container simple bien hecho" | "mecanismo compositional" |

**Capsicum** (Watson+Anderson+Laurie+Kennaway, USENIX Security 2010 [papers.freebsd.org/2010/rwatson-capsicum](https://papers.freebsd.org/2010/rwatson-capsicum.files/rwatson-capsicum-paper.pdf)): añade **capabilities** a FreeBSD (cap mode + `cap_rights_limit` on fd). Coexiste con jails; es el retrofit cap-model, análogo a OpenBSD pledge/unveil. Cross-ref `r4/bsd_family.md`.

---

## 14. Windows Server Containers + Hyper-V Containers

Windows Server 2016 (2016). Dos flavors:

### Windows Server Containers (process-isolated)

Shared kernel, isolated user-space. Primitivas:
- **Job Objects** (NT desde 2000) — agrupar procesos con limits (~cgroups).
- **Silos** (Windows 10, 2015–) — namespace equivalente. Cada silo: object namespace (`\Device`, `\BaseNamedObjects`), registry view, session. **Server Silos** = full container (hostname+net+reg+objects); **Application Silos** = más pequeño (AppContainer/UWP sandboxing).
- **Host Compute Service (HCS)** — API para crear containers, usada por Docker Desktop Windows y containerd Windows (hcsshim).

### Hyper-V Containers (hypervisor-isolated)

Mismo container image, pero en Hyper-V VM dedicada. Nano Server base (40 MB). Análogo a Kata. Default cuando `--isolation=hyperv` o versión host ≠ container base.

K8s Windows nodes 1.14 (2019) GA. `kube-proxy` Windows usa HNS (Host Network Service), no iptables.

Refs [learn.microsoft.com/virtualization/windowscontainers](https://learn.microsoft.com/en-us/virtualization/windowscontainers/). Silos [learn.microsoft.com/windows/win32/procthread/server-silos](https://learn.microsoft.com/en-us/windows/win32/procthread/server-silos). Cross-ref R1 `windows.md` (Object Manager, Job Objects).

---

## Tabla — container isolation primitives

| Primitiva | Kernel | Isolation dim | Overhead | Madurez | Escape CVEs |
|---|---|---|---|---|---|
| Linux namespaces (8) | Linux | PID/net/mnt/uts/ipc/user/cgroup/time | O(µs) setup, zero steady | Mature 2002–2020 | CVE-2019-5736 runc, user_ns race, CVE-2024-21626 |
| cgroups v2 | Linux | resource (cpu/mem/io/pids) — no security | O(ns) per-syscall | Mature 2016, default 2023 | CVE-2022-0492 release_agent |
| seccomp-bpf | Linux | syscall-level | 1–5% syscall-heavy | Mature 2012 | bypass en non-filtered; filter bugs |
| SELinux (MCS) | Linux | MAC labels | 1–3% | Mature 2003 | policy holes; enforcing required |
| AppArmor | Linux | path-based MAC | ~1% | Mature 2007 | profile gaps |
| FreeBSD jails | FreeBSD | all-in-one (fs/pid/net w/ VIMAGE) | O(µs) setup | Mature 2000 | raros, ~ cada 5 años |
| Windows Silos | NT | object ns + registry + session | comparable Linux | Mature 2015 | raros públicos |
| eBPF LSM | Linux 5.7+ | custom hooks | ~5% | Moderate 2020 | verifier bugs |
| Landlock | Linux 5.13+ | unprivileged path sandbox | <1% | Young 2021 | nuevo |
| Capsicum | FreeBSD | fd-level caps | near-zero | Mature 2010 | minimal surface |
| pledge/unveil | OpenBSD | promise syscall + path | near-zero | Mature 2015 | muy bajo |

---

## Tabla — container runtimes

| Runtime | Isolation | Startup | Overhead | Security | Use case |
|---|---|---|---|---|---|
| Docker (containerd+runc) | ns+cg+seccomp+(LSM) | ~50 ms | near-zero | medium | general, dev |
| containerd | idem Docker-less | ~50 ms | near-zero | medium | K8s default |
| runc | direct OCI exec | ~30 ms | near-zero | medium | reference impl |
| crun | same, C | ~10 ms | near-zero | medium | Podman default |
| youki | same, Rust | 10–15 ms | near-zero | medium | experimental |
| Kata | microVM (FC/QEMU) | 150–300 ms | ~50 MB/pod | high (HW) | multi-tenant untrusted |
| gVisor (runsc) | user-space kernel | 50–100 ms | 2–5× syscall | high (narrow kernel) | untrusted web |
| Firecracker | microVM standalone | <125 ms | ~5 MB/vm | high | AWS Lambda, Fargate |
| Podman rootless | idem sin daemon | ~50 ms | near-zero | medium+ | desktop, CI |
| Win Server Container | Silo + Job | ~100 ms | low | medium | Windows dev |
| Hyper-V Container | full hypervisor | 1–3 s | higher | high | Windows untrusted |

---

## ALZE applicability

### v1 (hoy, `/root/repos/alze-os`)

**Containers irrelevantes.** R2 review: no multi-proceso robusto, IDT incompleta, SMP unverified, FS sin locks. Hablar de containers cuando el kernel no sobrevive SMP boot es absurdo. Cerrar P0 blockers primero. Cualquier diseño container-aware antes es premature.

### v2 (post-capabilities, ~2026-Q4)

Siguiendo `r3/capability_kernels.md` §ALZE v2 (Zircon-lite en C99), si ALZE adopta **handles + rights** a nivel kernel, los "containers" caen naturales:

- **Process isolation** — cada proceso ya tiene address space propio; si su handle table contiene solo caps que el creator le dio, no accede nada más → **ese** es un container mínimo.
- **"PID ns"** innecesario — no hay cap a "list all processes" por defecto; sin ambient authority = sin cross-process leak.
- **"NET ns"** innecesario — cap a un `endpoint` de red específico, no al stack.
- **"MNT ns"** innecesario — cap a directorios específicos (tipo `openat` + Capsicum `cap_rights`), no al root FS.

"Container semantics" gratis. El namespace privado de caps por proceso (CSpace seL4 / handle table Zircon) **es** el namespace.

**NO hacer**: implementar Linux-style namespaces en C99. Trabajo muerto, duplica lo que cap model ya da.

### v3 (aspirational — "scopes" estilo jails)

Si ALZE quiere abstracción user-visible de contenedor, el modelo es **FreeBSD jails**, no Linux ns:

- **scope** = grupo de procesos + hostname + caps iniciales + quota (mem, cpu).
- `scope_create(parent, config) → scope_cap` — creator decide qué caps otorga.
- Entry: hijos del creator con la scope cap entran.
- Quotas: "meter" caps estilo KeyKOS space bank / seL4 SchedContext.
- **~3k LOC** si ALZE ya tiene caps + resource quota abstraction.

Refs útiles: Kamp+Watson 2000 jails paper (10k-line patch, 6 mo work, 95% del valor con minimal complexity); Shapiro KeyKOS space bank; seL4 SchedContext.

**NO hacer**: OCI runtime compliance. Overhead de `config.json` Linux-specific (seccomp, AppArmor labels, cgroup paths) no justifica para hobbyist OS.

### v4 (nunca o muy tarde)

OCI image format + registry protocol → sólo si ALZE quisiera correr legacy Linux workloads vía gVisor-like Sentry. Proyecto de años. K8s CRI / orchestration → fuera de scope.

---

## 15. Honest note — containers son un parche a Linux

Linux es kernel **monolítico** con **ACL security** y **ambient authority** (root, UID, paths globales). Nada es capability. El modelo Unix 1970 era: FS global + UID check per-syscall. Funcionó para timesharing VAX.

En cloud era (multi-tenant, untrusted, microservices), ese modelo colapsa. Opciones:
1. Rehacer cap-based (seL4, Zircon) — correcto pero caro, throw-away 30 años.
2. Parchear con namespaces + cgroups + seccomp + LSMs + USER ns + rootless — lo que Linux hizo.

Resultado: el stack de este documento. 8 namespaces, unified cgroups, BPF syscall filters, 2 major LSMs + stacking, overlayfs, OCI spec, Podman, Kata, gVisor, Cilium. **Todos existen porque Linux no es capability-based.**

### Equivalencias estructurales

| "Container primitive" Linux | Natural en kernel cap-based |
|---|---|
| MNT ns | no cap al root FS; sólo caps a dirs específicos |
| PID ns | no cap a "ver todos los procesos"; PID = implementation detail |
| NET ns | caps a endpoints específicos (no stack) |
| USER ns | no existe UID; no hay root |
| IPC ns | no "global IPC"; IPC requiere cap a endpoint |
| UTS ns | hostname = servicio; cap al servicio si permitido |
| CGROUP ns | no "global cgroup tree"; quota es una cap |
| TIME ns | clock es una cap; otros tienen la suya |
| cgroups v2 limits | resource caps con quota attr (KeyKOS meter / seL4 SchedContext) |
| seccomp-bpf | sin caps = syscalls sobre objetos inexistentes fallan |
| SELinux/AppArmor | policy = distribución de caps al arrancar (CapDL) |
| overlayfs | CoW FS nativo (ZFS/APFS/btrfs); no "layers" externos |
| OCI config.json | constructor describe caps iniciales; no hay "container" first-class |
| rootless containers | nunca hubo root; always rootless |

### La lección para ALZE

Si ALZE adopta caps en v2 (recomendación `r3/capability_kernels.md`), el **80% del software stack de containers no necesita reimplementación**. Porque la contención es inherente al cap model. Un proceso con sólo cap-al-endpoint-X no toca nada más. No hay escape de namespace porque no hay namespace — hay CSpace privado.

Esto no es perfección teórica. Shapiro+Weber 2000 (EROS confinement) **probaron formalmente** que un constructor bien diseñado preserva confinement estructuralmente. seL4 extendió a info-flow. Zircon lo hace informalmente pero en producción (Nest). Linux containers no son estructuralmente seguros — mitigan mucho, pero escapes publicados cada 1–2 años lo confirman.

**Containers son un bandaid sobre un modelo más débil.** Necesarios porque Linux no va a rehacerse cap-based. Innecesarios para un OS que empiece con caps desde día uno.

**ALZE debe perseguir caps, no containers.** Gastar LOC/mes en namespaces Linux-style = reinventar un parche. Gastar LOC en handles + rights + endpoints = resolver la misma clase de problema (isolation, quotas, revocable authority) de raíz.

El único aspecto donde containers aportan valor independiente del modelo de seguridad es **distribución de artefactos** (OCI image format + layers + registries). Empaquetar software portable + deltas eficientes es problema real ortogonal a security. Si ALZE v4 alguna vez quisiera portabilidad de workloads, un formato tipo OCI image (JSON manifest + layered tarballs) es compromiso razonable. Pero **no confundir con container runtime** — son cosas separadas.

---

## Referencias primarias

**Namespaces** — Biederman, OLS 2006 [landley.net/kdocs/ols/2006](https://landley.net/kdocs/ols/2006/ols2006v1-pages-101-112.pdf). Kerrisk LWN 2013 [lwn.net/Articles/531114](https://lwn.net/Articles/531114/). man `namespaces(7)`, `user_namespaces(7)`, `pid_namespaces(7)`, `time_namespaces(7)`.

**cgroups v2** — Heo kernel doc [kernel.org/cgroup-v2](https://www.kernel.org/doc/html/latest/admin-guide/cgroup-v2.html). Corbet LWN 2014 [lwn.net/Articles/604609](https://lwn.net/Articles/604609/). PSI doc [kernel.org/psi](https://www.kernel.org/doc/html/latest/accounting/psi.html).

**OCI** — Runtime Spec [github.com/opencontainers/runtime-spec](https://github.com/opencontainers/runtime-spec). Image Spec [github.com/opencontainers/image-spec](https://github.com/opencontainers/image-spec). Distribution Spec [github.com/opencontainers/distribution-spec](https://github.com/opencontainers/distribution-spec).

**Runtimes** — runc [github.com/opencontainers/runc](https://github.com/opencontainers/runc). containerd arch [github.com/containerd/containerd/docs/architecture.md](https://github.com/containerd/containerd/blob/main/docs/architecture.md). crun [github.com/containers/crun](https://github.com/containers/crun). youki [github.com/containers/youki](https://github.com/containers/youki). K8s CRI [kubernetes.io/docs/concepts/architecture/cri](https://kubernetes.io/docs/concepts/architecture/cri/).

**Podman** — [podman.io](https://podman.io/). Buildah [buildah.io](https://buildah.io/). Skopeo [github.com/containers/skopeo](https://github.com/containers/skopeo).

**Kata + Firecracker** — Kata docs [katacontainers.io/docs](https://katacontainers.io/docs/). Agache et al NSDI 2020 [usenix.org/nsdi20/agache](https://www.usenix.org/conference/nsdi20/presentation/agache).

**gVisor** — Young et al HotCloud 2019 [usenix.org/hotcloud19/young](https://www.usenix.org/conference/hotcloud19/presentation/young). Repo [github.com/google/gvisor](https://github.com/google/gvisor).

**overlayfs** — Szeredi kernel doc [kernel.org/overlayfs](https://www.kernel.org/doc/html/latest/filesystems/overlayfs.html). Composefs [github.com/containers/composefs](https://github.com/containers/composefs).

**seccomp + LSM** — Drewry+Cook LWN 2012 [lwn.net/Articles/475043](https://lwn.net/Articles/475043/). Loscocco+Smalley SELinux OLS 2001 [nsa.gov/selinux](https://www.nsa.gov/research/_files/selinux/papers.shtml). Landlock Salaün LWN 2021 [lwn.net/Articles/859908](https://lwn.net/Articles/859908/).

**Cilium + eBPF** — Graf KubeCon 2017 [youtube](https://www.youtube.com/watch?v=ilKlmTDdFgk). [docs.cilium.io](https://docs.cilium.io/). [ebpf.io](https://ebpf.io/).

**Rootless** — [rootlesscontaine.rs](https://rootlesscontaine.rs/). Podman rootless [docs.podman.io](https://docs.podman.io/en/latest/markdown/podman.1.html#rootless-mode).

**BSD jails + Capsicum** — Kamp+Watson SANE 2000 [papers.freebsd.org/phk-jails](https://papers.freebsd.org/2000/phk-jails.files/sane2000-jail.pdf). Watson+Anderson+Laurie+Kennaway USENIX Security 2010 [papers.freebsd.org/capsicum](https://papers.freebsd.org/2010/rwatson-capsicum.files/rwatson-capsicum-paper.pdf).

**Windows** — [learn.microsoft.com/virtualization/windowscontainers](https://learn.microsoft.com/en-us/virtualization/windowscontainers/). Silos [learn.microsoft.com/windows/win32/procthread/server-silos](https://learn.microsoft.com/en-us/windows/win32/procthread/server-silos).

**Meta** — Shapiro+Weber, *"Verifying the EROS Confinement Mechanism"*, IEEE S&P 2000 [cis.upenn.edu/~shap/EROS/confinement.pdf](https://www.cis.upenn.edu/~shap/EROS/confinement.pdf). Miller+Yee+Shapiro, *"Capability Myths Demolished"* 2003 [srl.cs.jhu.edu](http://srl.cs.jhu.edu/pubs/SRL2003-02.pdf). Ver `/root/lab_journal/research/alze_os/r3/capability_kernels.md` para argumento cap-vs-ACL estructural.
