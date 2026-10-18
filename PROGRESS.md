# irisOS Implementation Progress Tracker

This document tracks progress, completed milestones, verification proofs, and immediate next steps for **irisOS** across all 6 roadmap phases.

---

## Current Status Overview
- **Active Phase:** Phase 1 (Toolchain, Pinned Zircon Build & iris_init Handoff)
- **Current Milestone:** Host Toolchain, Repository Initialization & Architecture Lock-In
- **Overall Status:** In Progress (Toolchains configured, host packages installing)

---

## Phase 1 & 2: Toolchain, Pinned Zircon Build & iris_init Handoff

### Step 1.1: Architectural Lock-In & Specification Hardening
- **Status:** COMPLETED
- **Details:**
  - Updated [`AGENTS.md`](file:///Users/stefan/Dev/irisos/AGENTS.md) with 9 locked-in architectural specifications:
    1. Host Environment (macOS Apple Silicon with Homebrew LLVM).
    2. Pinned Zircon source tree build with standalone CMake/Ninja wrapper (no `jiri` or Fuchsia `GN`).
    3. `userboot` to `iris_init` bootstrap channel ABI using `<zircon/processargs.h>`.
    4. Custom 24-byte C-ABI IPC wire protocol (`iris_msg_hdr_t` in `<iris/ipc.h>`).
    5. VFS router (`iris_fshost`), client-side FD table in `mlibc-sysdeps`, and `PA_HND(PA_FD, fd)` inheritance.
    6. `iris_crypt` block channel proxy with 4 KB Sector 0 metadata header on `Data`.
    7. Gigaboot UEFI bootloader with Dual-ZBI layout on ESP (`iris_a.zbi`, `iris_b.zbi`).
    8. Unified PCIe (ECAM) device discovery for VirtIO devices on both `x86_64` and `arm64`.
    9. Multi-build system orchestration for CMake, Meson, and Make subprojects.
  - Added Section 8: Strict Documentation and Progress Tracking policy.

### Step 1.2: Repository & Directory Structure Setup
- **Status:** COMPLETED
- **Details:**
  - Initialized Git repository (`git init`).
  - Configured `.gitignore` to exclude build artifacts (`out/`, `build/out/`) and system files.
  - Created standardized directory layout per Section 5 (`build/toolchains`, `docs`, `scripts`, `kernel/zircon`, `sdk`, `servers`, `apps`, `vendor`, `out`).
  - Added `.gitkeep` markers for empty directories to track repository structure.

### Step 1.3: Cross-Compilation Toolchain Configurations
- **Status:** COMPLETED
- **Details:**
  - Created CMake cross-compilation files pointing to Homebrew LLVM binaries:
    - [`build/toolchains/x86_64-iris.cmake`](file:///Users/stefan/Dev/irisos/build/toolchains/x86_64-iris.cmake) (`x86_64-unknown-elf`, `-fuse-ld=lld`)
    - [`build/toolchains/arm64-iris.cmake`](file:///Users/stefan/Dev/irisos/build/toolchains/arm64-iris.cmake) (`aarch64-unknown-elf`, `-fuse-ld=lld`)
  - Created Meson cross-compilation files for `mlibc`, `pixman`, and `freetype`:
    - [`build/toolchains/meson-x86_64.txt`](file:///Users/stefan/Dev/irisos/build/toolchains/meson-x86_64.txt)
    - [`build/toolchains/meson-arm64.txt`](file:///Users/stefan/Dev/irisos/build/toolchains/meson-arm64.txt)

### Step 1.4: Host Development Dependencies
- **Status:** COMPLETED
- **Required Packages:** `llvm lld cmake ninja meson qemu xorriso mtools`
- **Installation Command:**
  ```bash
  brew install llvm lld cmake ninja meson qemu xorriso mtools
  ```
- **Verification Results:**
  - `clang`: Homebrew clang version 23.1.3 (`/opt/homebrew/opt/llvm/bin/clang`)
  - `ld.lld`: LLD 23.1.3 (`/opt/homebrew/bin/ld.lld`)
  - `cmake`: 4.4.4
  - `ninja`: 1.13.2
  - `meson`: 1.12.1
  - `qemu-system-x86_64`: 11.1.2
  - `qemu-system-aarch64`: 11.1.2
  - `xorriso`: 1.5.8.pl02
  - `mtools`: 4.0.49

### Step 1.5: Pinned Standalone Zircon Fetch Script
- **Status:** COMPLETED
- **Script:** [`scripts/fetch_zircon.sh`](file:///Users/stefan/Dev/irisos/scripts/fetch_zircon.sh)
- **Upstream:** `https://fuchsia.googlesource.com/zircon`
- **Pinned Commit:** `86b09d1d34dbd44390cee58e95f795f89169ea79` (branch `milestone-1011`)
- **Fetched Components in `kernel/zircon/`:**
  - `kernel/`: Full 64-bit microkernel source (x86_64 and arm64).
  - `bootloader/`: Gigaboot UEFI bootloader (`BOOTX64.EFI` / `BOOTAA64.EFI`).
  - `system/ulib/zircon/`: `libzircon.so` (vDSO) sources and wrappers.
  - `system/public/zircon/`: System calls (`syscalls.abigen`), types, headers (`processargs.h`).
  - `system/host/zbi/`: Source code for the host `zbi` image packaging utility.
- **Repository Impact:** `kernel/zircon/*` ignored in `.gitignore` to prevent bloat in the main repository (~56 MB isolated locally).

### Step 1.6: Host Tools Compilation (`zbi` & `abigen`)
- **Status:** COMPLETED
- **Binaries:**
  - `out/host/zbi`: Native macOS Apple Silicon (`Mach-O 64-bit executable arm64`) tool for packing and inspecting ZBI images.
  - `out/host/abigen`: Native syscall header and stub generator (`Mach-O 64-bit executable arm64`).
- **Modernization Fixes:**
  - Modernized `kernel/zircon/system/host/zbi/zbi.cpp` container types from deprecated `std::list<const iovec>` to `std::list<iovec>` for compatibility with modern C++17/20 in LLVM 23.
  - Added `#include <cassert>` in `kernel/zircon/system/host/abigen/kernel_wrapper_generator.cpp`.

### Step 1.7: Standalone CMake/Ninja Build Harness & vDSO
- **Status:** COMPLETED
- **Files Created:**
  - [`build/CMakeLists.txt`](file:///Users/stefan/Dev/irisos/build/CMakeLists.txt): Master CMake build harness for `libzircon` (vDSO) and syscall generation.
  - [`build/Makefile`](file:///Users/stefan/Dev/irisos/build/Makefile): Top-level orchestrator calling CMake, Ninja, and host tool builds.
- **Artifacts Produced & Verified:**
  - `x86_64`:
    - `out/build-x86_64/lib/libzircon.a`: Static archive (93 KB) for freestanding `iris_init`
    - `out/build-x86_64/lib/libzircon.so`: Dynamic shared object (71 KB ELF 64-bit LSB shared object)
  - `aarch64`:
    - `out/build-arm64/lib/libzircon.a`: Static archive (89 KB) for freestanding `iris_init`
    - `out/build-arm64/lib/libzircon.so`: Dynamic shared object (70 KB ELF 64-bit LSB shared object)

---

## Verification & Testing Log
| Test Date | Component | Test Command | Result |
|---|---|---|---|
| 2026-10-18 | Git Repo & Filesystem | `git status` | Clean working tree with tracking markers |
| 2026-10-18 | LLVM Toolchains | `clang --target=x86_64-unknown-elf -fuse-ld=/opt/homebrew/bin/ld.lld` | Successfully built & linked 64-bit ELF executable for x86-64 |
| 2026-10-18 | LLVM Toolchains | `clang --target=aarch64-unknown-elf -fuse-ld=/opt/homebrew/bin/ld.lld` | Successfully built & linked 64-bit ELF executable for ARM aarch64 |
| 2026-10-18 | Host Utilities | `cmake`, `ninja`, `meson`, `qemu`, `xorriso`, `mtools` | All binaries installed and responding to `--version` |
| 2026-10-18 | Zircon Fetch | `./scripts/fetch_zircon.sh` | Pinned commit `86b09d1` fetched; all 5 core directories verified |
| 2026-10-19 | Host Tool (`zbi`) | `./out/host/zbi --help` | Successfully executes; prints usage and switches |
| 2026-10-19 | Host Tool (`abigen`) | `./out/host/abigen ...` | Successfully parses `syscalls.abigen` and generates 11 output headers/stubs |
| 2026-10-19 | Build Harness (x86_64) | `make -C build build-x86_64` | Configures and links `libzircon.a` and `libzircon.so` (ELF 64-bit x86-64) |
| 2026-10-19 | Build Harness (arm64) | `make -C build build-arm64` | Configures and links `libzircon.a` and `libzircon.so` (ELF 64-bit ARM aarch64) |

---

## Immediate Next Steps
1. Commit build harness and documentation changes to Git.
2. Compile Gigaboot UEFI bootloader (`BOOTX64.EFI` and `BOOTAA64.EFI`).
3. Implement freestanding `iris_init` bootstrap binary linked against `libzircon.a`.
4. Pack initial `iris_a.zbi` using `out/host/zbi` and boot in QEMU via Gigaboot.
