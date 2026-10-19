# Third-Party Open Source Licenses & Upstream Repository Pinning

irisOS is built on a strict **Two-Zone Licensing Architecture** to ensure that end-users and commercial developers can distribute 100% proprietary, closed-source applications and hardware drivers without legal contamination.

---

## Zone 1: Permissive SDK, Kernel & Shared Libraries
All code in `kernel/`, `sdk/`, and shared system libraries (`/System/Libraries`) must be licensed exclusively under permissive licenses (MIT, BSD, Apache 2.0, ISC, or FTL). **Strictly NO GPL or LGPL code is permitted in Zone 1.**

| Submodule / Component | Upstream Canonical URL | irisOS Mirror / Fork | License | Role in irisOS |
| :--- | :--- | :--- | :--- | :--- |
| **Zircon Microkernel** | `https://fuchsia.googlesource.com/zircon` | `https://github.com/stirbeiu/zircon-standalone` | MIT / BSD 3-Clause | 64-bit microkernel, vDSO (`libzircon.so`), and host `zbi` tool |
| **mlibc** | `https://github.com/managarm/mlibc` | `https://github.com/stirbeiu/mlibc` | MIT | C Standard Library & dynamic linker (`libc.so`, `ld-iris.so`, `libc.a`) |
| **frigg** | `https://github.com/managarm/frigg` | `https://github.com/stirbeiu/frigg` | BSD 2-Clause | C++ utility & data structure library required by mlibc |
| **UFS2 / FFS (libufs)** | `https://github.com/freebsd/freebsd-src` | `https://github.com/stirbeiu/libufs-standalone` | BSD 2-Clause | UFS2/FFS filesystem implementation for `/System` and decrypted `/Users` |
| **FatFs** | `http://elm-chan.org/fsw/ff/00index_e.html` | `https://github.com/stirbeiu/fatfs` | BSD-like (FatFs license) | FAT32 filesystem implementation for the UEFI `ESP` partition |
| **mbedtls** | `https://github.com/Mbed-TLS/mbedtls` | `https://github.com/stirbeiu/mbedtls` | Apache 2.0 | AES-256-XTS & Argon2 cryptographic engine for `iris_crypt` |
| **pixman** | `https://gitlab.freedesktop.org/pixman/pixman` | `https://github.com/stirbeiu/pixman` | MIT | 2D pixel manipulation library for `iris_compositor` & `libiris_gui` |
| **FreeType** | `https://gitlab.freedesktop.org/freetype/freetype` | `https://github.com/stirbeiu/freetype` | FreeType License (FTL) | TrueType font rendering engine for desktop GUI |
| **LLVM / compiler-rt** | `https://github.com/llvm/llvm-project` | `https://github.com/llvm/llvm-project` (Homebrew LLVM) | Apache 2.0 with LLVM Exception | Low-level runtime builtins (`libclang_rt.builtins-*.a`) |

### FreeType License Election & Required Attribution
Upstream FreeType is dual-licensed under two alternative licenses:
1. The **FreeType License (FTL)** (a permissive BSD-style license with credit attribution).
2. The **GNU General Public License (GPLv2)**.

To strictly uphold irisOS's **Zone 1 Permissive Licensing Policy** and guarantee zero GPL contamination in shared libraries (`/System/Libraries/libfreetype.so`), **irisOS strictly elects to license and redistribute FreeType under the FreeType License (FTL)**.

In accordance with Section 2 ("Redistribution") of the FreeType License, any redistribution of binary distributions or documentation includes the following required attribution:

> "Portions of this software are copyright © 1996-2026 The FreeType Project (https://freetype.org). All rights reserved."

---

## Zone 2: Standalone User-Space Programs & Utilities
Standalone executables that execute within their own isolated Ring 3 address spaces and interact exclusively over standard IPC channels or POSIX file descriptors may use permissive or copyleft licenses.

| Package / Binary | Canonical Upstream | irisOS Mirror / Fork | License | Role in irisOS |
| :--- | :--- | :--- | :--- | :--- |
| **Toybox** | `https://github.com/landley/toybox` | `https://github.com/stirbeiu/toybox` | 0BSD | Primary command-line shell (`sh`) and 200+ core POSIX utilities |

---

## Upstream Pinning & Mirroring Policy
1. **Canonical Upstream Tracking:** The canonical URLs above represent the official upstream development sources for all third-party software integrated into irisOS.
2. **Permanent Availability & Supply-Chain Hardening:** To prevent dependency rot, supply-chain breakage, deleted upstream repositories, or unexpected upstream API changes, all external dependencies are mirrored or forked under `https://github.com/stirbeiu/<repo>`.
3. **Primary / Fallback Fetch Architecture:** All fetch scripts in `scripts/fetch_*.sh` implement an automated failover strategy:
   - Primary: GitHub mirror under `stirbeiu/`
   - Fallback: Canonical upstream repository
   - Feature `--clean` flags and shallow (`--depth 1`) clones to keep the host repository lightweight and reproducible.
4. **Submodule Pinning:** Vendor dependencies are referenced via pinned Git commit SHAs and release tags rather than tracking floating branches (`main` or `master`).
