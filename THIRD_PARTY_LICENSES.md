# Third-Party Open Source Licenses & Upstream Pinning

`irisOS` enforces a **Two-Zone Licensing Architecture** (Section 2 of [`AGENTS.md`](AGENTS.md)) so developers can distribute 100% proprietary, closed-source applications and drivers without legal obligation to disclose source code.

## Zone 1: Permissive SDK, Kernel & Shared Libraries
All code in `kernel/`, `sdk/`, and `/System/Libraries` uses permissive licenses (`MIT`, `BSD`, `Apache 2.0`, `ISC`, or `FTL`). **Zero GPL or LGPL code is permitted in Zone 1.**

| Component | Canonical Upstream | irisOS Mirror | License | Role |
| :--- | :--- | :--- | :--- | :--- |
| **Zircon** | `https://fuchsia.googlesource.com/zircon` | `https://github.com/stirbeiu/zircon-standalone` | MIT / BSD 3-Clause | Microkernel, `libzircon.so` vDSO, host `zbi` |
| **mlibc** | `https://github.com/managarm/mlibc` | `https://github.com/stirbeiu/mlibc` | MIT | C library & dynamic linker (`libc.so`, `ld-iris.so`) |
| **frigg** | `https://github.com/managarm/frigg` | `https://github.com/stirbeiu/frigg` | BSD 2-Clause | Freestanding C++ containers for `mlibc` |
| **libufs** | `https://github.com/freebsd/freebsd-src` | `https://github.com/stirbeiu/libufs-standalone` | BSD 2-Clause | FreeBSD UFS2/FFS library for `/System` & `/Users` |
| **FatFs** | `http://elm-chan.org/fsw/ff/00index_e.html` | `https://github.com/stirbeiu/fatfs` | BSD-style (FatFs) | FAT32 library for the UEFI `ESP` partition |
| **Mbed-TLS** | `https://github.com/Mbed-TLS/mbedtls` | `https://github.com/stirbeiu/mbedtls` | Apache 2.0 | AES-256-XTS & Argon2id crypto for `iris_crypt` |
| **Pixman** | `https://gitlab.freedesktop.org/pixman/pixman` | `https://github.com/stirbeiu/pixman` | MIT | 2D SIMD compositing for `iris_compositor` & `libiris_gui` |
| **FreeType** | `https://gitlab.freedesktop.org/freetype/freetype` | `https://github.com/stirbeiu/freetype` | FreeType License (FTL) | Font engine (`libfreetype`) for GUI typography |
| **compiler-rt** | `https://github.com/llvm/llvm-project` | Homebrew LLVM | Apache 2.0 w/ LLVM Exc. | Freestanding compiler builtins (`libclang_rt.builtins`) |

### FreeType License (FTL) Election & Attribution
FreeType is dual-licensed under the **FreeType License (FTL)** and GPLv2. To uphold Zone 1 rules, **`irisOS` strictly elects the permissive FreeType License (FTL)**:
> "Portions of this software are copyright © 1996-2026 The FreeType Project (https://freetype.org). All rights reserved."

## Zone 2: Standalone User-Space Executables
Standalone programs running in isolated address spaces and communicating only via IPC or standard streams:

| Component | Canonical Upstream | irisOS Mirror | License | Role |
| :--- | :--- | :--- | :--- | :--- |
| **Toybox** | `https://github.com/landley/toybox` | `https://github.com/stirbeiu/toybox` | 0BSD | System shell (`sh`) and 200+ POSIX CLI utilities |

## Supply-Chain Pinning Policy
All `scripts/fetch_*.sh` scripts fetch pinned commit SHAs / release tags from the primary `https://github.com/stirbeiu/<repo>` mirror with automatic failover to the canonical upstream URL.
