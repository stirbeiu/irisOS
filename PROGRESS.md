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

---

## Verification & Testing Log
| Test Date | Component | Test Command | Result |
|---|---|---|---|
| 2026-10-18 | Git Repo & Filesystem | `git status` | Clean working tree with tracking markers |
| 2026-10-18 | LLVM Toolchains | `clang --target=x86_64-unknown-elf -fuse-ld=/opt/homebrew/bin/ld.lld` | Successfully built & linked 64-bit ELF executable for x86-64 |
| 2026-10-18 | LLVM Toolchains | `clang --target=aarch64-unknown-elf -fuse-ld=/opt/homebrew/bin/ld.lld` | Successfully built & linked 64-bit ELF executable for ARM aarch64 |
| 2026-10-18 | Host Utilities | `cmake`, `ninja`, `meson`, `qemu`, `xorriso`, `mtools` | All binaries installed and responding to `--version` |

---

## Immediate Next Steps
1. Review and confirm repository setup with the user.
2. Commit initial repository structure and toolchain configuration.
3. Implement `scripts/fetch_zircon.sh` to fetch pinned standalone Zircon microkernel source and build the host `zbi` packaging tool.
