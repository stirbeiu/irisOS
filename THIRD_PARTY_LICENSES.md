# Third-Party Upstream Software & Licenses

irisOS incorporates select, battle-tested open-source components in accordance with the **Two-Zone Licensing Policy** defined in [`AGENTS.md`](file:///Users/stefan/Dev/irisos/AGENTS.md). 

Under this policy, all shared libraries, SDK headers, drivers, and the microkernel are strictly permissive (MIT / BSD / ISC / Apache 2.0 / FTL), ensuring 100% proprietary and closed-source software can be compiled, linked, and distributed on irisOS without license viral contamination.

---

## Zone 1: Microkernel, System Libraries & SDK (Permissive Only)

| Component | Upstream Project | License | Purpose in irisOS |
|---|---|---|---|
| **Zircon Microkernel** | [Google Zircon](https://fuchsia.googlesource.com/zircon) | BSD-style | Standalone microkernel, vDSO (`libzircon.so`), and Gigaboot UEFI bootloader |
| **mlibc** | [managarm/mlibc](https://github.com/managarm/mlibc) | MIT | C standard library and dynamic linker (`ld-iris.so`) |
| **lwext4** | [gabrbed/lwext4](https://github.com/gabrbed/lwext4) | BSD-2-Clause | Lightweight Ext4 filesystem library for `System_A`/`B` and `Data` |
| **FatFs** | [ChaN FatFs](http://elm-chan.org/fsw/ff/00index_e.html) | BSD-style / Permissive | FAT32 filesystem driver for the EFI System Partition (`ESP`) |
| **mbedTLS** | [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) | Apache 2.0 | Cryptographic primitives (AES-256-XTS, Argon2) for disk encryption |
| **Pixman** | [freedesktop/pixman](https://gitlab.freedesktop.org/pixman/pixman) | MIT | 2D pixel manipulation and SIMD compositing backend |
| **FreeType** | [FreeType Project](https://freetype.org) | FTL / BSD-style | High-quality font rasterization and glyph rendering |

---

## Zone 2: Standalone User-Space Programs & Utilities

| Component | Upstream Project | License | Purpose in irisOS |
|---|---|---|---|
| **Toybox** | [landley/toybox](https://github.com/landley/toybox) | 0BSD | Primary CLI command suite, POSIX utilities, and shell (`sh`) |

---

## Summary of Permissive Guarantees
- **No GPL or LGPL in Zone 1:** No application running on irisOS is required to open-source its code merely by linking against irisOS SDKs, system libraries, or invoking system calls.
- **Isolated Utilities in Zone 2:** Any future utilities licensed under copyleft licenses (e.g. GPL userland tools) run strictly in independent Ring 3 processes, communicating only via IPC channels or standard I/O pipes.
