# Linux especializado — distros con tesis de diseño

Cinco distribuciones Linux que no buscan reemplazar a Ubuntu en el escritorio. Cada una resuelve
un problema arquitectónico concreto, y cada una vale la pena destripar para ALZE OS.

## Overview

| Distro | Problema que resuelve | Usuario objetivo |
|---|---|---|
| **Kali Linux** | Toolkit de pentesting listo, bleeding-edge, con Debian como base estable | Pentesters, red teams, certificación OSCP |
| **Parrot OS** | Pentesting + privacidad/anonimato + menor huella de recursos | Pentesters, OSINT, usuarios privacy-conscious |
| **Qubes OS** | Aislamiento por compartimentación ("security by compartmentalization") | Periodistas, disidentes, analistas de alto riesgo |
| **NixOS** | Deriva de configuración y builds no reproducibles | SREs, investigadores, gente harta de `works-on-my-machine` |
| **Alpine Linux** | Tamaño, superficie de ataque, densidad de contenedores | Base de imágenes Docker, embebidos, routers |

## Arquitectura

- **Kali / Parrot**: Rolling release sobre Debian testing. Kali mantiene metapaquetes (`kali-linux-top10`,
  `kali-linux-web`, `kali-linux-wireless`) que agrupan herramientas por caso de uso. Parrot añade
  capa de anonimato (AnonSurf, Tor) y es menos demandante en recursos. Ambos comparten la misma
  genealogía: paquetes Debian + parches upstream.
- **Qubes OS**: Xen bare-metal hypervisor como base. `dom0` es el dominio administrativo con acceso
  a hardware; todo lo demás corre en VMs aisladas (qubes). TemplateVMs proveen un rootfs inmutable
  compartido por múltiples AppVMs que solo persisten `/home` y `/usr/local`. Comunicación inter-VM
  vía `qrexec` sobre Xen vchan, con política centralizada en dom0.
- **NixOS**: Todo el sistema (kernel, paquetes, `/etc`, servicios) se deriva de una expresión pura
  en el lenguaje Nix. Los paquetes viven en `/nix/store/<hash>-<name>` donde el hash cubre todas
  las entradas del build. Activar una nueva config crea una nueva "generación" atómica con
  rollback trivial.
- **Alpine Linux**: musl libc + BusyBox userland + OpenRC init + `apk` package manager. Kernel
  parcheado con grsecurity/PaX históricamente; hoy hardened flags por defecto (PIE, SSP). Una
  instalación a disco mínima cabe en ~130 MB; un contenedor base en ~5-8 MB.

## En qué es bueno

- **Kali**: catálogo completo de tools curado por OffSec; metapaquetes que eliminan búsqueda
  manual; docs excelentes; estándar de facto para certs.
- **Parrot**: privacy tools preinstaladas (AnonSurf, i2p, crypto); corre bien en 2 GB RAM;
  ediciones separadas (Security / Home / Architect) para no arrastrar 500 tools si no las necesitas.
- **Qubes**: aislamiento real a nivel hypervisor (no namespaces). Compromiso de una VM ≠ compromiso
  del host. DispVMs para abrir attachments sospechosos sin residuo. Copy-paste y transferencia
  de archivos inter-VM requieren gestos explícitos.
- **NixOS**: rollbacks atómicos que funcionan; `flake.lock` pinea revisiones exactas de todas las
  inputs; múltiples versiones del mismo paquete coexisten sin colisión; todo el sistema es
  reproducible bit-a-bit dado el mismo flake + lock.
- **Alpine**: imagen base Docker minúscula, superficie de ataque reducida, arranque rápido,
  perfect para contenedores stateless.

## En qué falla

- **Kali / Parrot**: no son distros para uso diario. Corren como root históricamente (Kali corrigió
  esto en 2020), muchos servicios expuestos, rolling release rompe cosas. Mezclar pentesting con
  banca online en la misma VM es mala idea.
- **Qubes**: soporte de hardware *muy* limitado — requiere VT-x + VT-d + TPM; muchos laptops
  modernos no arrancan. GPU passthrough es frágil. Consumo de RAM alto (4 GB mínimo, 16 GB cómodo).
  Curva de aprendizaje del modelo mental (¿en qué qube estoy?). Webcam/sleep/Bluetooth sufren.
- **NixOS**: curva de aprendizaje brutal. Errores de eval de Nix son crípticos. Cualquier script
  con `#!/usr/bin/env python` falla porque `/usr/bin/env` no existe igual. Docs fragmentadas entre
  manual, wiki, nixpkgs source. El lenguaje Nix en sí es peculiar (perezoso, sin tipos).
- **Alpine / musl**: incompatibilidades reales con software asumiendo glibc:
  - **DNS resolver**: musl no soporta `single-request`/`single-request-reopen` de
    `/etc/resolv.conf`; search/ndots funcionan distinto → `Unknown Host` intermitentes en
    Kubernetes reportados históricamente.
  - **dlopen**: `dlclose` es no-op en musl; destructores solo corren al `exit()` del proceso.
    Constructores solo la primera vez. Software que cuenta referencias de librerías rompe.
  - **Stack size**: thread stack default 128 KB en musl vs 2-10 MB en glibc → segfaults en
    apps Python/Java/C++ multihilo sin `pthread_attr_setstacksize` manual.
  - **NSS**: musl no implementa el framework NSS (`/etc/nsswitch.conf`), lo cual rompe LDAP/SSSD.
  - **Locales**: default `C.UTF-8` en musl vs `C` en glibc; software que hace asunciones sobre
    bytes 0x80-0xff falla.

## Cómo funciona por dentro

### Isolation model

- **Qubes**: isolation a nivel hypervisor Xen. Cada AppVM es un dominio HVM/PV completo con su
  propio kernel. El rootfs viene de un TemplateVM montado read-only vía `root.img` + `root-cow.img`
  (copy-on-write descartable al reboot). `NetVM` aisla la pila de red en su propia VM; `FirewallVM`
  media tráfico; `DispVM` se destruye al cerrar. Todo inter-VM pasa por `qrexec-daemon` en dom0
  que enforce policies (archivos en `/etc/qubes/policy.d/*.policy`).
- **Kali / Parrot / NixOS / Alpine**: confían en mecanismos del kernel Linux — `unshare(2)`,
  namespaces (pid/net/mount/user/uts/ipc/cgroup), seccomp-bpf, capabilities. Aislamiento de
  procesos, no de kernels. Un exploit de kernel rompe todo.

### Package management

- **apk (Alpine)**: binario estático escrito en C, index en texto plano indexado, firmas RSA por
  repositorio. `apk add`, `apk del`, `apk upgrade`. Rápido (instalar 100 paquetes en segundos)
  porque no hay triggers estilo dpkg.
- **Nix**: paquetes son *builds* de expresiones puras. `/nix/store/<hash>-<name>` donde el hash
  cubre source + deps + builder + env. Múltiples versiones coexisten. `nix-env`, `nix-shell`,
  `nix build` — o con flakes: `nix develop`, `nix run`. GC cuando no hay GC roots.
- **Debian + Kali metapackages**: `apt` estándar sobre repos Kali adicionales. Los metapaquetes
  son paquetes-vacío-con-depends (`kali-linux-everything`, `kali-tools-web`, `kali-tools-wireless`,
  `kali-linux-headless`). Un solo `apt install` trae una toolbox coherente.

### Reproducibility

- **NixOS flakes**: `flake.nix` declara inputs (nixpkgs@sha, otros flakes); `flake.lock` congela
  cada input a un commit/narHash exacto. Eval en "pure mode" bloquea acceso a `$HOME`, variables
  de entorno no declaradas, fecha actual. Dado el mismo flake+lock, el build plan es
  determinístico; con builds bit-reproducibles upstream, el output también.
- **Alpine / Debian / Kali**: reproducible-builds.org progress existe pero no es garantía.
  Rolling releases rompen repros con el tiempo.
- **Qubes**: TemplateVMs se pueden reconstruir pero no hay garantía de repro a nivel distro.

### libc choice

- **Alpine** es la única aquí que usa **musl**. Tradeoffs:
  - Pro: tamaño (~600 KB libc vs ~2 MB glibc), código más auditable, licencia MIT.
  - Con: ver "En qué falla" — DNS, dlopen, stack, NSS, locales. Históricamente también hubo gaps
    en `fts.h`, `iconv`, backtrace. Proyecto gcompat existe como shim.
- Qubes/Kali/Parrot/NixOS usan **glibc**. NixOS además permite pinear la versión exacta de glibc
  por paquete sin conflicto (cosa imposible en FHS tradicional).

### Boot / init

- **Alpine**: OpenRC (scripts shell + supervisión liviana, parallel con `rc_parallel="YES"`).
  Init PID 1 es BusyBox `init`. Sin systemd units, sin journald, sin sockets activation — logs a
  syslog o archivo plano.
- **Kali / Parrot / NixOS / Qubes dom0**: systemd. NixOS lo integra de forma particular: las unit
  files son generadas desde la expresión Nix de `configuration.nix`, no editadas manualmente.

## Qué podríamos copiar para ALZE OS

1. **Config declarativa estilo NixOS**: un `alze-system.conf` (o TOML/KDL) que describa el sistema
   entero. `alze rebuild switch` crea una nueva generación atómica, `--rollback` revierte al boot
   previo. Store de paquetes content-addressed (`/alze/store/<blake3>-<name>`) para permitir
   múltiples versiones y GC preciso.
2. **Compartimentación estilo Qubes**: micro-VMs por dominio de confianza (trabajo / personal /
   banca / untrusted) usando Firecracker o Cloud Hypervisor (KVM más ligero que Xen). GUI proxy
   con un compositor tipo `gui-agent` enviando frames por vsock al compositor del host. DispVMs
   para abrir PDFs/URLs desconocidos. Pool preemptivo de VMs tibias para latencia baja.
3. **Init mínimo estilo Alpine**: opción de bootear con `init=/bin/busybox-init` + OpenRC para
   perfiles edge/contenedor, y systemd solo en el perfil desktop. Default hardening flags (PIE,
   SSP, RELRO, `_FORTIFY_SOURCE=3`) en el toolchain de sistema.
4. **Metapaquetes por workflow estilo Kali**: `alze-profile-dev`, `alze-profile-pentest`,
   `alze-profile-journalist`, `alze-profile-kiosk`. Un comando, un perfil coherente, sin que el
   usuario curate paquetes uno a uno.
5. **Flakes + lockfile para el sistema entero**: `alze.lock` con hash de cada input. Reproducible
   builds como garantía de primera clase, no como aspiración.
6. **qrexec-like policy engine**: IPC inter-dominio sobre un broker con política central en texto
   plano (`allow work -> personal: clipboard-paste`), auditable en git. Diseñar la API mínima
   (pipes + RPC nombrado) y dejar que userland componga.
7. **Rechazar musl como default**: adoptarla como perfil opcional (contenedores, embebidos), no
   como base. Los problemas de DNS/dlopen/stack queman tiempo del usuario final.
8. **TemplateVM + CoW rootfs**: rootfs compartido read-only entre qubes del mismo template;
   cambios persisten en overlay por-qube. Actualizar el template = actualizar N qubes
   derivadas en un acto.

## Fuentes consultadas

- Qubes OS Glossary: https://doc.qubes-os.org/en/latest/user/reference/glossary.html
- Qubes OS Template Implementation: https://doc.qubes-os.org/en/latest/developer/system/template-implementation.html
- Qubes OS qrexec: https://doc.qubes-os.org/en/latest/developer/services/qrexec.html
- Qubes OS GUI Domain: https://www.qubes-os.org/news/2020/03/18/gui-domain/
- Qubes OS Wikipedia: https://en.wikipedia.org/wiki/Qubes_OS
- Xen in Qubes OS Security Architecture: https://wiki.xenproject.org/wiki/Xen_in_Qubes_OS_Security_Architecture
- NixOS paper (Dolstra, Löh, Pierron 2010): https://edolstra.github.io/pubs/nixos-icfp2008-final.pdf
- NixOS Wikipedia: https://en.wikipedia.org/wiki/NixOS
- NixOS Flakes wiki: https://wiki.nixos.org/wiki/Flakes
- Nix flakes concepts (nix.dev): https://nix.dev/concepts/flakes.html
- musl functional differences from glibc: https://wiki.musl-libc.org/functional-differences-from-glibc.html
- Comparison of C/POSIX standard library implementations: http://www.etalabs.net/compare_libcs.html
- Alpine Linux about: https://alpinelinux.org/about/
- Alpine Package Keeper wiki: https://wiki.alpinelinux.org/wiki/Alpine_Package_Keeper
- Alpine Hardened Linux: https://wiki.alpinelinux.org/wiki/Hardened_linux
- Kali Linux features: https://www.kali.org/features/
- Kali Tools index: https://www.kali.org/tools/
- Kali Linux Wikipedia: https://en.wikipedia.org/wiki/Kali_Linux
- Parrot Security: https://parrotsec.org/
- StationX Kali vs Parrot: https://www.stationx.net/kali-linux-vs-parrot-os/
