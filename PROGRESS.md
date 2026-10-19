# irisOS Implementation Progress Tracker

This document tracks progress, completed milestones, verification proofs, and immediate next steps for **irisOS** across all 6 roadmap phases.

---

## Current Status Overview
- **Active Phase:** Phase 3 (The Safe SDK & mlibc Integration)
- **Current Milestone:** Phase 1 & 2 Milestone COMPLETED (UEFI Gigaboot, vDSO Dynamic Resolver, Bootable Dual-ZBI, and Live QEMU Boot Verified)
- **Overall Status:** Phase 1 & 2 Complete (100%). Proceeding to Phase 3.

---

## Phase 1 & 2: Toolchain, Pinned Zircon Build & iris_init Handoff (MILESTONE COMPLETED)

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
    - `out/build-x86_64/lib/libzircon.a`: Static archive (93 KB)
    - `out/build-x86_64/lib/libzircon.so`: Dynamic shared object (71 KB ELF 64-bit LSB shared object)
  - `aarch64`:
    - `out/build-arm64/lib/libzircon.a`: Static archive (89 KB)
    - `out/build-arm64/lib/libzircon.so`: Dynamic shared object (70 KB ELF 64-bit LSB shared object)

### Step 1.8: Freestanding `iris_init` Bootstrap & Dynamic vDSO Resolver
- **Status:** COMPLETED
- **Source File:** [`servers/iris_init/src/main.c`](file:///Users/stefan/Dev/irisos/servers/iris_init/src/main.c)
- **Features Implemented:**
  - Freestanding C entry point `_start(void* start_arg, void* vdso_base)` conforming to Section 4.4 of AGENTS.md (no standard `int main` runtime).
  - Built-in ELF64 dynamic symbol resolver that parses the mapped vDSO's `PT_DYNAMIC`, `DT_SYMTAB`, and `DT_STRTAB` directly from `vdso_base`. This strictly complies with Zircon microkernel policy requiring all `syscall` instructions to execute from within the mapped vDSO code pages (`valid_pc(pc - vdso_code_address)`), completely eliminating `ZX_EXCP_POLICY_ERROR` faults.
  - Reads `zx_proc_args_t` message from the bootstrap channel and extracts essential root capabilities:
    - `PA_HND(PA_RESOURCE, 0)`: Root Resource handle (for device driver MMIO/IRQs/ports).
    - `PA_HND(PA_VMO_BOOTFS, 0)`: BOOTFS VMO handle (for spawning early servers).
    - `PA_HND(PA_JOB_DEFAULT, 0)`: Root Job handle (for child job & process isolation).
    - `PA_HND(PA_VMO_VDSO, 0)`: vDSO handle (`libzircon.so`).
  - Creates early debuglog via `zx_debuglog_create(root_resource, 0, &log)` and writes bootstrap banner to kernel serial console:
    `"[irisOS] Welcome to irisOS (x86_64 / arm64) - iris_init bootstrap successful!\n"`.
  - Compiled with `-ffreestanding -fno-stack-protector -fPIE -Wl,-pie -nostdlib -Wl,-e,_start`.
- **Artifacts Produced & Verified:**
  - `out/build-x86_64/bin/iris_init`: ELF 64-bit LSB `DYN` (PIE), AMD x86-64 (53 KB).
  - `out/build-arm64/bin/iris_init`: ELF 64-bit LSB `DYN` (PIE), AArch64 (51 KB).

### Step 1.9: Standalone Zircon Microkernel Compilation (x86_64 & arm64)
- **Status:** COMPLETED
- **Kernel Targets:**
  - `x86_64`: `out/build-x86_64/kernel/zircon.elf` (14 MB) & `out/build-x86_64/kernel/kernel.zbi` (1.3 MB)
  - `arm64`: `out/build-arm64/kernel/zircon.elf` (12 MB) & `out/build-arm64/kernel/kernel.zbi` (1.1 MB)
- **Orchestration:** Integrated `kernel-x86_64` and `kernel-arm64` targets into [`build/Makefile`](file:///Users/stefan/Dev/irisos/build/Makefile).

### Step 1.10: UEFI Bootloader (Gigaboot) & ARM64 UEFI Support
- **Status:** COMPLETED
- **Source:** `kernel/zircon/bootloader`
- **Artifacts:**
  - `x86_64`: `BOOTX64.EFI` (compiled for UEFI x86_64)
  - `arm64`: `BOOTAA64.EFI` (compiled for UEFI AArch64 with `-mgeneral-regs-only` and dynamic memory allocation via `AllocateAnyPages`)
- **Key Fixes:**
  - Added support for `ZBI_TYPE_KERNEL_ARM64` in Gigaboot image parser.
  - Added AArch64 direct kernel entry execution stub.
  - Passed `-mgeneral-regs-only` on AArch64 EFI build to prevent illegal NEON vector register usage in UEFI EL1.

### Step 1.11: Dual-ZBI Packaging & FAT32 ESP Disk Creation
- **Status:** COMPLETED
- **Packaging Script:** [`build/mkimage.sh`](file:///Users/stefan/Dev/irisos/build/mkimage.sh)
- **Features:**
  - Uses native host `out/host/zbi` tool to combine `kernel.zbi`, commandline arguments (`userboot=bin/iris_init kernel.serial=legacy`), and `bin/iris_init` into a bootable `iris_a.zbi` image.
  - Generates a 64 MB FAT32 UEFI ESP disk image (`esp_x86_64.img` / `esp_arm64.img`) formatted with `mformat`, populated with `/EFI/BOOT/BOOTX64.EFI` (or `BOOTAA64.EFI`) and `/EFI/BOOT/iris_a.zbi`.

### Step 1.12: QEMU UEFI Virtual Machine Launcher
- **Status:** COMPLETED
- **Launcher Script:** [`scripts/run_qemu.sh`](file:///Users/stefan/Dev/irisos/scripts/run_qemu.sh)
- **Makefile Integration:** `make run-x86` and `make run-arm` in [`build/Makefile`](file:///Users/stefan/Dev/irisos/build/Makefile).
- **Execution:** Boots QEMU with EDK2 OVMF/AAVMF UEFI firmware, VirtIO storage, and stdio serial console redirection.

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
| 2026-10-19 | `iris_init` (x86_64) | `llvm-readelf -h out/build-x86_64/bin/iris_init` | Verified ELF64 DYN (PIE), entry 0x2350, AMD x86-64 |
| 2026-10-19 | `iris_init` (arm64) | `llvm-readelf -h out/build-arm64/bin/iris_init` | Verified ELF64 DYN (PIE), entry 0x11000, AArch64 |
| 2026-10-19 | Microkernel (x86_64) | `llvm-readelf -h out/build-x86_64/kernel/zircon.elf` | Verified ELF64 EXEC, entry 0xffffffff80100218, AMD x86-64 (14 MB) |
| 2026-10-19 | Microkernel (arm64) | `llvm-readelf -h out/build-arm64/kernel/zircon.elf` | Verified ELF64 EXEC, entry 0xffffffff0000a1ac, AArch64 (12 MB) |
| 2026-10-19 | Disk Packaging | `./build/mkimage.sh x86_64` | Created `iris_a.zbi` (1.3 MB) and FAT32 UEFI `esp_x86_64.img` (64 MB) |
| 2026-10-19 | Disk Packaging | `./build/mkimage.sh arm64` | Created `iris_a.zbi` (1.2 MB) and FAT32 UEFI `esp_arm64.img` (64 MB) |
| 2026-10-19 | **UEFI Full Boot (x86_64)** | `scripts/run_qemu.sh -a x86_64` | **BOOT SUCCESSFUL**: Gigaboot loaded `iris_a.zbi`, launched `userboot`, executed `bin/iris_init`, and printed full banner + capability verification |
| 2026-10-19 | **UEFI Full Boot (arm64)** | `scripts/run_qemu.sh -a arm64` | **BOOT SUCCESSFUL**: Gigaboot loaded `iris_a.zbi`, booted kernel at EL1, initialized GICv2, PL011, timers, executed `bin/iris_init`, and printed full banner + capability verification |
| 2026-10-19 | **Dual Output GUI & Serial** | `scripts/run_qemu.sh -a [arch] --gui` | **VERIFIED**: Cocoa GUI window displays graphical framebuffer console with mirrored serial output streamed simultaneously to terminal `stdio` for both x86_64 and arm64 |

### Live Boot Verification Output (arm64 QEMU + UEFI):
```text
======================================================================
 Launching irisOS (arm64) in QEMU with UEFI...
 Bootloader: Gigaboot (UEFI)
 Image:      /Users/stefan/Dev/irisos/out/images/esp_arm64.img
 Firmware:   /opt/homebrew/share/qemu/edk2-aarch64-code.fd
 Display:    Headless Serial Console Only (-display none)
 (Press Ctrl+A then X in terminal to terminate QEMU, or close the GUI window)
======================================================================
BdsDxe: loading Boot0001 "UEFI Non-Block Boot Device" from PciRoot(0x0)/Pci(0x2,0x0)
BdsDxe: starting Boot0001 "UEFI Non-Block Boot Device" from PciRoot(0x0)/Pci(0x2,0x0)
KALLOC DONE: kernel_zone_base @ 0x5bf48000
boot image is a kernel+ramdisk combo image
ramdisk @ 0x5be29000
[00000.000] 00000.00000> PMM: boot reserve add [0x5bf48000, 0x5c0b1fff]
[00000.000] 00000.00000> mem_arena.base 0x40000000 size 0x20000000
[00000.000] 00000.00000> detected GICv2
[00000.000] 00000.00000> GICv2m 0: base spi 80 count 64
[00000.000] 00000.00000> arm generic timer freq 62500000 Hz
[00008.560] 00000.00000> reserving ramdisk phys range [0x5be21000, 0x5be29fff]
[00008.597] 00000.00000> welcome to Zircon
[00008.597] 00000.00000> KASLR: .text section at 0xffffffff10000000
[00008.611] 00000.00000> initializing heap
[00008.612] 00000.00000> initializing vm
[00008.615] 00000.00000> initializing kernel
[00008.615] 00000.00000> initializing mp
[00008.615] 00000.00000> initializing timers
[00008.617] 00000.00000> creating bootstrap completion thread
[00008.632] 00000.00000> top of bootstrap2()
[00008.640] 00000.00000> UART: started IRQ driven TX
[00008.640] 00000.00000> GICv2 MSI init
[00008.668] 00000.00000> userboot: ramdisk          0x9000 @ 0xffff00005be21000
[00008.696] 01030.01037> userboot: searching bootfs for 'bin/iris_init'
[00008.698] 01030.01037> userboot: userboot: loaded bin/iris_init at 0xaf66c6d24000, entry point 0xaf66c6d344f4
[00008.698] 01030.01037> userboot: userboot: loaded vDSO at 0x9f9fdf09c000, entry point 0x0
[00008.703] 01044.01047> [irisOS] Welcome to irisOS (arm64) - iris_init bootstrap successful!
[00008.703] 01044.01047> [iris_init] Root Resource: OK
[00008.703] 01044.01047> [iris_init] BOOTFS VMO: OK
[00008.703] 01044.01047> [iris_init] Root Job: OK
[00008.703] 01044.01047> [iris_init] vDSO VMO: OK
[00008.705] 01030.01037> userboot: process bin/iris_init started.
[00008.705] 01030.01037> userboot: finished!
```

### Live Boot Verification Output (x86_64 QEMU + UEFI):
```text
======================================================================
 Launching irisOS (x86_64) in QEMU with UEFI...
 Bootloader: Gigaboot (UEFI)
 Image:      /Users/stefan/Dev/irisos/out/images/esp_x86_64.img
 Firmware:   /opt/homebrew/share/qemu/edk2-x86_64-code.fd
 (Press Ctrl+A then X to terminate QEMU)
======================================================================
BdsDxe: loading Boot0001 "UEFI Non-Block Boot Device" from PciRoot(0x0)/Pci(0x3,0x0)
BdsDxe: starting Boot0001 "UEFI Non-Block Boot Device" from PciRoot(0x0)/Pci(0x3,0x0)
Framebuffer base is at 80000000
boot image is a kernel+ramdisk combo image
ramdisk @ 0x1dd46000
welcome to Zircon
...
[00005.671] 00000.00000> INIT: cpu 1, calling hook 0xffffffff001520f0 (userboot) at level 0xc0000, flags 0x1
[00005.687] 01037.01044> userboot: option "userboot=bin/iris_init"
[00005.687] 01037.01044> userboot: option "kernel.serial=legacy"
[00005.694] 01037.01044> userboot: searching bootfs for 'bin/iris_init'
[00005.697] 01037.01044> userboot: userboot: loaded bin/iris_init at 0x6f183c624000, entry point 0x6f183c625560
[00005.697] 01037.01044> userboot: userboot: loaded vDSO at 0x4ac8c3fa0000, entry point 0x0
[00005.699] 01037.01044> userboot: process bin/iris_init started.
[00005.700] 01037.01044> userboot: finished!
[00005.701] 01051.01054> [irisOS] Welcome to irisOS (x86_64) - iris_init bootstrap successful!
[00005.701] 01051.01054> [iris_init] Root Resource: OK
[00005.701] 01051.01054> [iris_init] BOOTFS VMO: OK
[00005.701] 01051.01054> [iris_init] Root Job: OK
[00005.701] 01051.01054> [iris_init] vDSO VMO: OK
```

---

## Phase 3: The Safe SDK (`libiris`) & `mlibc` Integration (NEXT MILESTONE)
1. **SDK Zone (`sdk/libiris` - STRICTLY MIT):**
   - Provide clean, type-safe C wrappers for Zircon handles, channels, VMOs, and ports.
   - Implement the 24-byte `iris_msg_hdr_t` IPC wire protocol (`<iris/ipc.h>`).
   - Implement process spawning primitives (`posix_spawn` over `zx_process_create`, `zx_process_start`, transferring `PA_HND(PA_FD, fd)`).
2. **`mlibc` Sysdeps (`sdk/mlibc-sysdeps/iris/`):**
   - Implement core irisOS sysdeps (`sys_anon_allocate`, `sys_vm_map`, `sys_write`, `sys_exit`, `sys_futex`, `sys_open`, `sys_read`).
   - Implement client-side FD table mapping file descriptors to Zircon channels.
3. **Cross-Compile `vendor/mlibc`:**
   - Fetch pinned `vendor/mlibc` submodule.
   - Cross-compile with Meson and Homebrew LLVM producing `libc.so` and `ld-iris.so`.
4. **Milestone Verification:**
   - A standard C program using `#include <stdio.h>`, `malloc()`, and `printf()` compiles with Clang and runs on irisOS.

