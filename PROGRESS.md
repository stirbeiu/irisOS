# irisOS Implementation Progress Tracker

This document tracks progress, completed milestones, verification proofs, and immediate next steps for **irisOS** across all 6 roadmap phases.

---

## Current Status Overview
- **Active Phase:** Phase 4 (PCIe Drivers, Storage Encryption & Filesystems)
- **Current Milestone:** Phase 4 Step 4.2 (`iris_crypt` Block-Level AES-256-XTS Encryption Proxy) 100% COMPLETED & VERIFIED
- **Overall Status:** Phase 1, 2, 3, and Phase 4 Steps 4.1 & 4.2 Complete (100%). Ready for Phase 4 Step 4.3 (`iris_fshost`: GPT parser, FatFs, UFS2/libufs VFS router).

---

## Phase 1 & 2: Toolchain, Pinned Zircon Build & iris_init Handoff (MILESTONE COMPLETED)

### Step 1.1: Architectural Lock-In & Specification Hardening
- **Status:** COMPLETED
- **Details:**
  - Updated [`AGENTS.md`](AGENTS.md) with 9 locked-in architectural specifications:
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
    - [`build/toolchains/x86_64-iris.cmake`](build/toolchains/x86_64-iris.cmake) (`x86_64-unknown-elf`, `-fuse-ld=lld`)
    - [`build/toolchains/arm64-iris.cmake`](build/toolchains/arm64-iris.cmake) (`aarch64-unknown-elf`, `-fuse-ld=lld`)
  - Created Meson cross-compilation files for `mlibc`, `pixman`, and `freetype`:
    - [`build/toolchains/meson-x86_64.txt`](build/toolchains/meson-x86_64.txt)
    - [`build/toolchains/meson-arm64.txt`](build/toolchains/meson-arm64.txt)

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
- **Script:** [`scripts/fetch_zircon.sh`](scripts/fetch_zircon.sh)
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
  - [`build/CMakeLists.txt`](build/CMakeLists.txt): Master CMake build harness for `libzircon` (vDSO) and syscall generation.
  - [`build/Makefile`](build/Makefile): Top-level orchestrator calling CMake, Ninja, and host tool builds.
- **Artifacts Produced & Verified:**
  - `x86_64`:
    - `out/build-x86_64/lib/libzircon.a`: Static archive (93 KB)
    - `out/build-x86_64/lib/libzircon.so`: Dynamic shared object (71 KB ELF 64-bit LSB shared object)
  - `aarch64`:
    - `out/build-arm64/lib/libzircon.a`: Static archive (89 KB)
    - `out/build-arm64/lib/libzircon.so`: Dynamic shared object (70 KB ELF 64-bit LSB shared object)

### Step 1.8: Freestanding `iris_init` Bootstrap & Dynamic vDSO Resolver
- **Status:** COMPLETED
- **Source File:** [`servers/iris_init/src/main.c`](servers/iris_init/src/main.c)
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
- **Orchestration:** Integrated `kernel-x86_64` and `kernel-arm64` targets into [`build/Makefile`](build/Makefile).

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
- **Packaging Script:** [`build/mkimage.sh`](build/mkimage.sh)
- **Features:**
  - Uses native host `out/host/zbi` tool to combine `kernel.zbi`, commandline arguments (`userboot=bin/iris_init kernel.serial=legacy`), and `bin/iris_init` into a bootable `iris_a.zbi` image.
  - Generates a 64 MB FAT32 UEFI ESP disk image (`esp_x86_64.img` / `esp_arm64.img`) formatted with `mformat`, populated with `/EFI/BOOT/BOOTX64.EFI` (or `BOOTAA64.EFI`) and `/EFI/BOOT/iris_a.zbi`.

### Step 1.12: QEMU UEFI Virtual Machine Launcher
- **Status:** COMPLETED
- **Launcher Script:** [`scripts/run_qemu.sh`](scripts/run_qemu.sh)
- **Makefile Integration:** `make run-x86` and `make run-arm` in [`build/Makefile`](build/Makefile).
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
| 2026-10-19 | `libiris.a` & `libiris.so` (x86_64) | `ninja -C out/build-x86_64 iris_static iris_shared` | Verified static archive (54 KB) and ELF dynamic shared object (48 KB) with `SONAME [libiris.so]` and `DT_NEEDED [libzircon.so]` |
| 2026-10-19 | `libiris.a` & `libiris.so` (arm64) | `ninja -C out/build-arm64 iris_static iris_shared` | Verified static archive (55 KB) and ELF dynamic shared object (50 KB) with `SONAME [libiris.so]` and `DT_NEEDED [libzircon.so]` |
| 2026-10-19 | `test_libiris` (x86_64) | `ninja -C out/build-x86_64 test_libiris` | **VERIFIED**: Static-PIE ELF64 executable passes static assertions (24-byte header ABI, field offsets), IPC packing, spawn args, and symbol linkage |
| 2026-10-19 | `test_libiris` (arm64) | `ninja -C out/build-arm64 test_libiris` | **VERIFIED**: Static-PIE ELF64 executable passes static assertions (24-byte header ABI, field offsets), IPC packing, spawn args, and symbol linkage |

### Live Boot Verification Output (arm64 QEMU + UEFI):
```text
======================================================================
 Launching irisOS (arm64) in QEMU with UEFI...
 Bootloader: Gigaboot (UEFI)
 Image:      out/images/esp_arm64.img
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
 Image:      out/images/esp_x86_64.img
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

## Phase 3: The Safe SDK (`libiris`) & `mlibc` Integration (IN PROGRESS)

### Step 3.1: Safe SDK (`sdk/libiris`) & IPC Wire Protocol
- **Status:** COMPLETED
- **Licensing:** STRICTLY MIT (Zone 1 SDK).
- **Components Implemented:**
  1. **IPC Wire Protocol ([`sdk/libiris/include/iris/ipc.h`](sdk/libiris/include/iris/ipc.h)):**
     - Defined standard 24-byte packed C-ABI header `iris_msg_hdr_t` (`IRIS_IPC_MAGIC = 0x49524953`).
     - Enforced 24-byte layout via compile-time `_Static_assert`.
     - Standard protocol spaces: `IRIS_PROTO_SYSTEM` (`0x0001`), `IRIS_PROTO_FS` (`0x0002`), `IRIS_PROTO_CRYPT` (`0x0003`), `IRIS_PROTO_COMPOSITOR` (`0x0004`), `IRIS_PROTO_PTY` (`0x0005`), `IRIS_PROTO_DRIVER` (`0x0006`).
     - Standard opcodes and status mappings matching Zircon status codes.
     - Inline helpers: `iris_msg_hdr_init()`, `iris_msg_hdr_init_reply()`, `iris_msg_hdr_is_valid()`, `iris_msg_hdr_is_reply()`.
  2. **Channel Primitives ([`sdk/libiris/include/iris/channel.h`](sdk/libiris/include/iris/channel.h) & [`src/channel.c`](sdk/libiris/src/channel.c)):**
     - `iris_channel_create()`: Bidirectional channel endpoint creation.
     - `iris_channel_send()` & `iris_channel_recv()`: Ergonomic message transmission with capability handle transfer. Features an 8 KB stack optimization with heap-free temporary VMO mapping fallback for messages up to 64 KB.
     - `iris_channel_call()`: Synchronous request/response transaction wrapping `zx_channel_call()`.
     - `iris_channel_reply()` & `iris_channel_send_simple()`: Reply helpers.
  3. **Virtual Memory Objects ([`sdk/libiris/include/iris/vmo.h`](sdk/libiris/include/iris/vmo.h) & [`src/vmo.c`](sdk/libiris/src/vmo.c)):**
     - `iris_vmo_create()`: Anonymous VMO allocation.
     - `iris_vmo_map()` & `iris_vmo_map_offset()`: Mapping into root process VMAR or target VMAR.
     - `iris_vmo_unmap()` & `iris_vmo_unmap_from()`: Virtual memory release.
     - `iris_vmo_duplicate()`: Handle duplication with restricted rights (e.g. read-only permissions for window sharing).
     - `iris_vmo_get_size()`, `iris_vmo_set_size()`, `iris_vmo_read()`, `iris_vmo_write()`.
  4. **Process Spawning Primitives ([`sdk/libiris/include/iris/spawn.h`](sdk/libiris/include/iris/spawn.h) & [`src/spawn.c`](sdk/libiris/src/spawn.c)):**
     - Full `posix_spawn()` foundation over `zx_process_create()`, `zx_thread_create()`, and `zx_process_start()`.
     - Serializes `zx_proc_args_t` bootstrap channel message with `argv`, `envp`, core capabilities (`PA_PROC_SELF`, `PA_THREAD_SELF`, `PA_VMAR_ROOT`, `PA_VMO_STACK`, `PA_VMO_VDSO`, `PA_JOB_DEFAULT`), and file descriptor inheritance via `PA_HND(PA_FD, target_fd)`.
  5. **Runtime Capabilities ([`sdk/libiris/include/iris/init.h`](sdk/libiris/include/iris/init.h) & [`src/init.c`](sdk/libiris/src/init.c)):**
     - `iris_init_runtime()` and weak fallback symbol definitions for `zx_process_self()`, `zx_vmar_root_self()`, `zx_job_default()`.
  6. **Master Umbrella Header ([`sdk/libiris/include/iris/iris.h`](sdk/libiris/include/iris/iris.h)) & Subsystem Documentation ([`docs/ipc_protocol.md`](docs/ipc_protocol.md)).**
  7. **Standalone Test Suite ([`sdk/libiris/tests/test_libiris.c`](sdk/libiris/tests/test_libiris.c)):**
     - Standalone Static-PIE ELF64 executable validating header field offsets, packing logic, spawn arg structures, and clean linkage.
- **Build Targets Produced & Verified:**
  - `x86_64`:
    - `out/build-x86_64/lib/libiris.a`: Static library archive (54 KB)
    - `out/build-x86_64/lib/libiris.so`: Dynamic shared library (48 KB ELF 64-bit LSB shared object)
    - `out/build-x86_64/bin/test_libiris`: Static-PIE ELF64 verification binary (48 KB)
  - `aarch64`:
    - `out/build-arm64/lib/libiris.a`: Static library archive (55 KB)
    - `out/build-arm64/lib/libiris.so`: Dynamic shared library (50 KB ELF 64-bit LSB shared object)
    - `out/build-arm64/bin/test_libiris`: Static-PIE ELF64 verification binary (50 KB)

### Step 3.2: `mlibc` Vendor Integration & irisOS Sysdeps
- **Status:** COMPLETED
- **Licensing:** STRICTLY MIT (Permissive Zone 1 System Libraries).
- **Components Implemented:**
  1. **Vendor Tree Isolation & Fetch Scripts:**
     - [`scripts/fetch_mlibc.sh`](scripts/fetch_mlibc.sh): Fetches pinned `mlibc` tag `v6.3.1` (`2444b0997c91c327b9989f25e4b8bd2c053d3194`) and pre-clones dependency `frigg` at pinned commit `98220ab8f3ac6b1f146c360598a334f7f1fc06e1`. Injects irisOS dispatch into `vendor/mlibc/meson.build` and symlinks `vendor/mlibc/sysdeps/iris -> sdk/mlibc-sysdeps/iris`.
     - [`scripts/fetch_compiler_rt.sh`](scripts/fetch_compiler_rt.sh): Sparse-checkouts LLVM `compiler-rt/lib/builtins` (pinned tag `llvmorg-21.1.0`) into `vendor/compiler-rt/lib/builtins/` to provide soft-float quadruple precision (128-bit `long double` for ARM64) and complex arithmetic.
     - [`.gitignore`](.gitignore): Excludes `vendor/mlibc/*` and `vendor/compiler-rt/*` to keep the main Git repository lightweight while preserving `.gitkeep`.
  2. **irisOS Sysdeps Layer ([`sdk/mlibc-sysdeps/iris/`](sdk/mlibc-sysdeps/iris/)):**
     - **Memory Management (`generic/generic.cpp`):** `sys_anon_allocate`, `sys_anon_free`, `sys_vm_map`, `sys_vm_unmap` using Zircon VMOs (`zx_vmo_create`) and VMAR mappings (`zx_vmar_map`, `zx_vmar_unmap`).
     - **Debug Console Output:** `sys_libc_log` and `sys_write` routing directly to `zx_debuglog_write()` via startup logging handles, enabling instant `printf()` output over serial console before VFS initialization.
     - **Process Termination:** `sys_exit` mapping directly to `zx_process_exit()`.
     - **Thread Synchronization:** `sys_futex_wait` and `sys_futex_wake` hooking into Zircon native futex syscalls with nanosecond deadline conversion.
     - **Thread Control Block (TCB) Setup:** Architecture-specific `sys_tcb_set()` implementing `ZX_PROP_REGISTER_FS` on x86_64 and EL0 user-space `msr tpidr_el0` on AArch64.
     - **Process Lifecycle & Startup Handles:** `zx_process_self()`, `zx_thread_self()`, `zx_vmar_root_self()`, `zx_job_default()`, and `zx_take_startup_handle()` implemented to parse bootstrap handles.
     - **ABI Bit Definitions:** 53 standard POSIX ABI headers populated in `include/abi-bits/`.
  3. **C Runtime Entry & Trampolines:**
     - Freestanding startup entry points: `crt-x86_64/crt1.S`, `crt-x86_64/crti.S`, `crt-x86_64/crtn.S`, and `crt-aarch64/crt1.S`, `crt-aarch64/crti.S`, `crt-aarch64/crtn.S`.
     - `generic/entry.cpp`: `__mlibc_entry()` calling `__dlapi_enter()` and executing `main(argc, argv, environ)`.
     - `generic/thread.cpp`: `__mlibc_enter_thread()` and `sys_prepare_stack()`.
     - Register unpacking assembly: `generic/thread_entry_x86_64.S` and `generic/thread_entry_aarch64.S`.
  4. **Build System Orchestration:**
     - Cross-compilation toolchain files: [`build/toolchains/meson-x86_64.txt`](build/toolchains/meson-x86_64.txt) and [`build/toolchains/meson-arm64.txt`](build/toolchains/meson-arm64.txt) configured with Homebrew LLVM Clang, `-nostdlib`, `-nostdinc++`, `-ffreestanding`, and `-D_GNU_SOURCE`.
     - Integrated `builtins-x86_64`, `builtins-arm64`, `mlibc-x86_64`, `mlibc-arm64`, and `mlibc` targets into [`build/Makefile`](build/Makefile).
     - Subsystem documentation in [`docs/mlibc_sysdeps.md`](docs/mlibc_sysdeps.md).
- **Artifacts Produced & Verified:**
  - `x86_64`:
    - `out/build-x86_64/mlibc/libc.a`: Static C library archive (11 MB)
    - `out/build-x86_64/mlibc/libc.so`: Dynamic shared C library (3.3 MB ELF 64-bit AMD x86-64)
    - `out/build-x86_64/mlibc/ld-iris.so`: Dynamic linker / RTLD (829 KB ELF 64-bit AMD x86-64)
    - `out/build-x86_64/mlibc/ld-iris.a`: Static dynamic linker archive (97 KB)
    - `out/build-x86_64/mlibc/sysdeps/iris/crt1.o`, `crti.o`, `crtn.o`: Freestanding C runtime initialization objects
    - `out/build-x86_64/lib/libclang_rt.builtins-x86_64.a`: Compiler builtins archive (263 KB)
  - `aarch64`:
    - `out/build-arm64/mlibc/libc.a`: Static C library archive (11 MB)
    - `out/build-arm64/mlibc/libc.so`: Dynamic shared C library (3.5 MB ELF 64-bit AArch64)
    - `out/build-arm64/mlibc/ld-iris.so`: Dynamic linker / RTLD (853 KB ELF 64-bit AArch64)
    - `out/build-arm64/mlibc/ld-iris.a`: Static dynamic linker archive (97 KB)
    - `out/build-arm64/mlibc/sysdeps/iris/crt1.o`, `crti.o`, `crtn.o`: Freestanding C runtime initialization objects
    - `out/build-arm64/lib/libclang_rt.builtins-aarch64.a`: Compiler builtins archive (247 KB)

### Step 3.3: First User-Space C Program (`apps/tests/hello_c`) in QEMU
- **Status:** COMPLETED
- **Source Files:**
  - [`apps/tests/hello_c/main.c`](apps/tests/hello_c/main.c): Standard POSIX C application using `#include <stdio.h>`, `<stdlib.h>`, and `<string.h>`, allocating memory via `malloc()`, copying via `strcpy()`, printing formatted text via `printf()`, and releasing via `free()`.
  - [`sdk/mlibc-sysdeps/iris/generic/entry.cpp`](sdk/mlibc-sysdeps/iris/generic/entry.cpp): Implemented static-PIE self-relocation (`self_relocate()`) processing `R_X86_64_RELATIVE` and `R_AARCH64_RELATIVE` relocations from `.dynamic` (`DT_RELA`), executing global initializers (`run_init_array()`), initializing stdio files (`stdin`, `stdout`, `stderr`), and cleanly exiting via `fflush(nullptr); mlibc::sys_exit(result);`.
  - [`servers/iris_init/src/main.c`](servers/iris_init/src/main.c): Freestanding BOOTFS ELF loader resolving segments, creating child process/thread, mapping the child vDSO in accordance with Zircon microkernel policies (`VDso::valid_code_mapping`), and passing `g_debuglog_handle` as `PA_FDIO_LOGGER` so `printf()` streams directly to the UART console.
  - [`sdk/libiris/src/spawn.c`](sdk/libiris/src/spawn.c): Enhanced `iris_elf_load_segments()` with 64-bit page masks and vDSO segment support.
- **Verification Commands & Results:**
  - Build commands:
    ```bash
    make -C build mlibc-x86_64 hello-c-x86_64 image-x86_64
    make -C build mlibc-arm64 hello-c-arm64 image-arm64
    ```
  - Execution output in QEMU (`x86_64`):
    ```text
    [00006.399] 01051.01054> [irisOS] Welcome to irisOS (x86_64) - iris_init bootstrap successful!
    [00006.399] 01051.01054> [iris_init] Root Resource: OK
    [00006.428] 01051.01054> [iris_init] BOOTFS VMO: OK
    [00006.428] 01051.01054> [iris_init] Root Job: OK
    [00006.476] 01051.01054> [iris_init] vDSO VMO: OK
    [00006.498] 01051.01054> [iris_init] Root VMAR: OK
    [00006.516] 01051.01054> [iris_init] Located bin/hello_c in BOOTFS! Spawning process...
    [00006.538] 01051.01054> [iris_init] Starting child process bin/hello_c...
    [00006.565] 01064.01066> [hello_c] Hello from standard C on irisOS! malloc/free/printf working.
    [00006.581] 01051.01054> [iris_init] hello_c process execution finished.
    ```
  - Execution output in QEMU (`arm64`):
    ```text
    [00008.767] 01044.01047> [irisOS] Welcome to irisOS (arm64) - iris_init bootstrap successful!
    [00008.767] 01044.01047> [iris_init] Root Resource: OK
    [00008.767] 01044.01047> [iris_init] BOOTFS VMO: OK
    [00008.767] 01044.01047> [iris_init] Root Job: OK
    [00008.767] 01044.01047> [iris_init] vDSO VMO: OK
    [00008.767] 01044.01047> [iris_init] Root VMAR: OK
    [00008.768] 01044.01047> [iris_init] Located bin/hello_c in BOOTFS! Spawning process...
    [00008.770] 01044.01047> [iris_init] Starting child process bin/hello_c...
    [00008.781] 01057.01059> [hello_c] Hello from standard C on irisOS! malloc/free/printf working.
    [00008.787] 01044.01047> [iris_init] hello_c process execution finished.
    ```

---

### Step 3.4: 100% Permissive Policy Hardening: UFS2 Migration & GitHub Mirror Infrastructure
- **Status:** COMPLETED
- **Permissive License Hardening (100% MIT/BSD):**
  - Removed `lwext4` to eliminate GPL contamination risks in shared libraries and Ring 3 daemons.
  - Migrated primary root and user filesystems to **FreeBSD UFS2 / FFS (`libufs`, BSD 2-Clause)**.
  - Created standalone public repository [`stirbeiu/libufs-standalone`](https://github.com/stirbeiu/libufs-standalone) containing userland `libufs` and kernel UFS2/FFS structures extracted from FreeBSD 14.2 (`releng/14.2`).
  - Implemented [`scripts/fetch_ufs.sh`](scripts/fetch_ufs.sh) featuring primary mirror fetching with automatic fallback to FreeBSD upstream and `--clean` support.
- **Mirrors Created & Populated under `stirbeiu`:**
  - `stirbeiu/zircon-standalone`: Standalone Zircon microkernel mirror populated directly from verified `milestone-1011` tree with irisOS build patches.
  - `stirbeiu/libufs-standalone`: Standalone BSD 2-Clause UFS2/FFS filesystem and `libufs` library mirror.
  - `stirbeiu/mlibc`: Fork of `managarm/mlibc` (pinned `v6.3.1`).
  - `stirbeiu/frigg`: Fork of `managarm/frigg` (pinned `98220ab`).
  - `stirbeiu/mbedtls`: Fork of `Mbed-TLS/mbedtls`.
  - `stirbeiu/toybox`: Fork of `landley/toybox`.
  - `stirbeiu/pixman`: Mirrored from `gitlab.freedesktop.org/pixman/pixman.git`.
  - `stirbeiu/freetype`: Fork of `freetype/freetype`.
- **Primary + Fallback Pattern across Fetch Scripts:**
  - [`scripts/fetch_zircon.sh`](scripts/fetch_zircon.sh): Primary `stirbeiu/zircon-standalone.git` -> Fallback `fuchsia.googlesource.com/zircon` with `--clean` and shallow clone.
  - [`scripts/fetch_mlibc.sh`](scripts/fetch_mlibc.sh): Primary `stirbeiu/mlibc.git` and `stirbeiu/frigg.git` -> Fallback `managarm/mlibc.git` and `managarm/frigg.git` with `--clean`.
  - [`scripts/fetch_ufs.sh`](scripts/fetch_ufs.sh): Primary `stirbeiu/libufs-standalone.git` -> Fallback `freebsd/freebsd-src.git` (`releng/14.2`) with `--clean`.
- **Repository Isolation & Tracking:**
  - Updated [`.gitignore`](.gitignore) to exclude all vendor trees (`vendor/mlibc/*`, `vendor/libufs/*`, `vendor/fatfs/*`, `vendor/mbedtls/*`, `vendor/toybox/*`, `vendor/pixman/*`, `vendor/freetype/*`, `vendor/compiler-rt/*`) while preserving `.gitkeep` markers.
  - Updated [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) and [`AGENTS.md`](AGENTS.md) confirming 100% permissive licensing across the kernel, SDK, and Ring 3 servers.

---

### Step 3.5: Client-Side File Descriptor Table, `posix_spawn` & FD Inheritance (100% VERIFIED)
- **Status:** COMPLETED & VERIFIED
- **Client-Side File Descriptor Table (`sdk/mlibc-sysdeps/iris/`):**
  - Implemented per-process file descriptor table in [`include/iris/fd_table.h`](sdk/mlibc-sysdeps/iris/include/iris/fd_table.h) and [`generic/generic.cpp`](sdk/mlibc-sysdeps/iris/generic/generic.cpp) supporting up to 256 file descriptors (`IRIS_MAX_FD`).
  - Pre-allocated stdio descriptors (FDs 0, 1, 2) routed to the debuglog handle until explicit channels are bound.
  - Implemented `sys_open`, `sys_read`, `sys_write`, `sys_close`, `sys_seek`, `sys_isatty`, and `sys_fstat` using the FD table.
  - Added support for `/dev/console`, `/dev/debuglog`, `/dev/null`, and `/dev/zero` virtual character devices.
- **Process Spawning Primitives & FD Inheritance (`sys_spawn`):**
  - Implemented `mlibc::sys_spawn` in `generic.cpp` (wrapped with `#if !MLIBC_BUILDING_RTLD` to keep `ld-iris.so` freestanding).
  - Iterates over active descriptors in `g_fd_table` and constructs `iris_spawn_action_t` arrays with `IRIS_SPAWN_ACTION_ADD_FD` / `PA_HND(PA_FD, fd)`.
  - Injected `mlibc::sys_spawn` declaration and hook into `vendor/mlibc` via [`scripts/fetch_mlibc.sh`](scripts/fetch_mlibc.sh).
  - Enhanced child process startup in [`entry.cpp`](sdk/mlibc-sysdeps/iris/generic/entry.cpp) to extract all `PA_HND(PA_FD, fd)` descriptors into the child's table upon launch.
- **vDSO Dynamic System Call Dispatch (Zero Direct `syscall`s):**
  - Exported complete C wrappers for all Zircon system calls (`zx_channel_*`, `zx_process_*`, `zx_thread_*`, `zx_vmo_*`, `zx_vmar_*`, `zx_clock_*`) dynamically dispatched through `libzircon.so` vDSO symbol lookup.
  - Removed static `libzircon.a` from user application linking to eliminate illegal direct `syscall` instructions outside vDSO.
- **Documentation:**
  - Authored [`docs/build_system.md`](docs/build_system.md) documenting the multi-toolchain orchestrator, Homebrew LLVM setup, target triples, and dynamic linker architecture (`ld-iris.so` + `libc.so`).
- **Dual-Architecture QEMU Verification Proofs:**
  - x86_64 QEMU output:
    ```text
    [00005.815] 01051.01054> [irisOS] Welcome to irisOS (x86_64) - iris_init bootstrap successful!
    [00005.815] 01051.01054> [iris_init] Root Resource: OK
    [00005.840] 01051.01054> [iris_init] BOOTFS VMO: OK
    [00005.840] 01051.01054> [iris_init] Root Job: OK
    [00005.872] 01051.01054> [iris_init] vDSO VMO: OK
    [00005.885] 01051.01054> [iris_init] Root VMAR: OK
    [00005.899] 01051.01054> [iris_init] Located bin/hello_c in BOOTFS! Spawning process...
    [00005.923] 01051.01054> [iris_init] Starting child process bin/hello_c...
    [00005.935] 01064.01066> [hello_c] Hello from standard C on irisOS! malloc/free/printf working.
    [00005.935] 01064.01066> [hello_c] FD Table: direct write(1) to stdout successful.
    [00005.936] 01064.01066> [hello_c] FD Table: isatty(1)=1 isatty(2)=1 (console tty verified).
    [00005.938] 01064.01066> [hello_c] FD Table: open("/dev/console") allocated fd 3, write OK.
    [00005.938] 01064.01066> [hello_c] FD Table: close(fd 3) successful.
    [00005.938] 01064.01066> [hello_c] FD Table: invalid fd rejection (EBADF) verified.
    [00005.938] 01064.01066> [hello_c] POSIX Spawn: file actions init/adddup2/destroy working.
    [00005.938] 01064.01066> [hello_c] All Phase 3 C runtime, FD table, and spawn verifications PASSED!
    ```
  - arm64 QEMU output:
    ```text
    [00008.847] 01044.01047> [irisOS] Welcome to irisOS (arm64) - iris_init bootstrap successful!
    [00008.847] 01044.01047> [iris_init] Root Resource: OK
    [00008.847] 01044.01047> [iris_init] BOOTFS VMO: OK
    [00008.847] 01044.01047> [iris_init] Root Job: OK
    [00008.847] 01044.01047> [iris_init] vDSO VMO: OK
    [00008.847] 01044.01047> [iris_init] Root VMAR: OK
    [00008.848] 01044.01047> [iris_init] Located bin/hello_c in BOOTFS! Spawning process...
    [00008.850] 01044.01047> [iris_init] Starting child process bin/hello_c...
    [00008.863] 01057.01059> [hello_c] Hello from standard C on irisOS! malloc/free/printf working.
    [00008.866] 01057.01059> [hello_c] FD Table: direct write(1) to stdout successful.
    [00008.867] 01057.01059> [hello_c] FD Table: isatty(1)=1 isatty(2)=1 (console tty verified).
    [00008.869] 01057.01059> [hello_c] FD Table: open("/dev/console") allocated fd 3, write OK.
    [00008.869] 01057.01059> [hello_c] FD Table: close(fd 3) successful.
    [00008.869] 01057.01059> [hello_c] FD Table: invalid fd rejection (EBADF) verified.
    [00008.870] 01057.01059> [hello_c] POSIX Spawn: file actions init/adddup2/destroy working.
    [00008.870] 01057.01059> [hello_c] All Phase 3 C runtime, FD table, and spawn verifications PASSED!
    ```

---

## Phase 4: Storage, Encryption & Filesystems (Active)

### Step 4.1: Unified PCIe (ECAM) Discovery & Ring 3 VirtIO Block Driver (`virtio-blk-pci`)
- **Status:** COMPLETED & VERIFIED (100% Dual-Architecture)
- **PCIe (ECAM) Configuration Space Scanner (`sdk/libiris/include/iris/pci.h`, `servers/drivers/pci/pci.c`, `sdk/libiris/src/pci.c`):**
  - Unified PCIe bus scanner compatible across `x86_64` (QEMU `-M q35`, physical ECAM at `0xE0000000ULL`) and `arm64` (QEMU `-M virt`, physical ECAM at `0x4010000000ULL`).
  - Maps physical ECAM MMIO window into process address space using Root Resource capability via `zx_vmo_create_physical()` and `zx_vmar_map()`.
  - Scans buses 0..15, detects devices, parses 32-bit and 64-bit BARs, and extracts VirtIO 1.0+ PCI vendor-specific capabilities:
    - Common configuration (`VIRTIO_PCI_CAP_COMMON_CFG`, `virtio_pci_common_cfg_t`)
    - Notification / Doorbell registers (`VIRTIO_PCI_CAP_NOTIFY_CFG`)
    - ISR status registers (`VIRTIO_PCI_CAP_ISR_CFG`)
    - Device-specific configuration (`VIRTIO_PCI_CAP_DEVICE_CFG`)
  - Enables PCI memory space decoding and bus master DMA (`pci_enable_device()`).
- **Standalone Ring 3 VirtIO Block Driver Daemon (`servers/drivers/virtio_blk/src/main.c` -> `bin/virtio_blk`):**
  - Unprivileged user-space server running in Ring 3 (x86_64) / EL0 (arm64).
  - Receives driver control channel and Root Resource handle via process startup arguments.
  - Negotiates VirtIO 1.0 specification features (`VIRTIO_F_VERSION_1`).
  - Safe user-space DMA buffer management via Zircon BTI:
    - Creates dummy IOMMU via `zx_iommu_create(root_resource, ZX_IOMMU_TYPE_DUMMY, ...)`
    - Creates BTI handle via `zx_bti_create(iommu, 0, 0, &bti)`
    - Allocates physically contiguous VMOs via `zx_vmo_create_contiguous(bti, ...)`
    - Pins DMA buffers and retrieves physical addresses via `zx_bti_pin(bti, ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE | ZX_BTI_CONTIGUOUS, ...)`
  - Implements split Virtqueue 0 (descriptors, available ring, used ring) and synchronous submission with completion timeout polling.
  - Implements the 24-byte `iris_msg_hdr_t` wire protocol (`IRIS_PROTO_DRIVER`):
    - `IRIS_BLK_OP_GET_INFO` (`0x0050`): Returns total sector count (e.g. 131,072 sectors for 64 MB disk) and 512-byte sector size.
    - `IRIS_BLK_OP_READ` (`0x0051`): Reads $N$ sectors into response message payload.
    - `IRIS_BLK_OP_WRITE` (`0x0052`): Writes $N$ sectors to disk from request payload.
    - `IRIS_BLK_OP_FLUSH` (`0x0053`): Flushes disk write cache.
- **QEMU Storage Test Harness & End-to-End Verification (`iris_init`):**
  - Updated `build/mkimage.sh` to generate 64 MB test drive (`out/images/disk_<arch>.raw`) and pack `bin/virtio_blk` into bootable ZBI.
  - Updated `scripts/run_qemu.sh` to attach the virtual drive via `-drive file=out/images/disk_<arch>.raw,if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0`.
  - In `iris_init`: discovers VirtIO block controller, spawns `bin/virtio_blk`, issues `BLK_GET_INFO`, writes 512-byte verification pattern to Sector 100, reads it back over IPC, and verifies 512/512 bytes match.
- **Documentation:**
  - Authored [`docs/virtio_drivers.md`](docs/virtio_drivers.md) detailing unified PCIe ECAM architecture, VirtIO 1.0 compliance, BTI DMA lifecycle, and block IPC protocol.
- **Dual-Architecture QEMU Verification Proofs:**
  - **x86_64 QEMU Console Output (`make run-x86`):**
    ```text
    [00006.576] 01051.01054> [irisOS] Welcome to irisOS (x86_64) - iris_init bootstrap successful!
    [00006.576] 01051.01054> [iris_init] Root Resource: OK
    [00006.576] 01051.01054> [iris_init] BOOTFS VMO: OK
    [00006.576] 01051.01054> [iris_init] Root Job: OK
    [00006.633] 01051.01054> [iris_init] vDSO VMO: OK
    [00006.633] 01051.01054> [iris_init] Root VMAR: OK
    [00006.666] 01051.01054> [iris_init] Located bin/hello_c in BOOTFS! Spawning process...
    [00006.720] 01051.01054> [iris_init] Starting child process: hello_c...
    [00006.778] 01064.01066> [hello_c] Hello from standard C on irisOS! malloc/free/printf working.
    [00006.795] 01064.01066> [hello_c] FD Table: direct write(1) to stdout successful.
    [00006.816] 01064.01066> [hello_c] FD Table: isatty(1)=1 isatty(2)=1 (console tty verified).
    [00006.832] 01064.01066> [hello_c] FD Table: open("/dev/console") allocated fd 3, write OK.
    [00006.848] 01064.01066> [hello_c] FD Table: close(fd 3) successful.
    [00006.864] 01064.01066> [hello_c] FD Table: invalid fd rejection (EBADF) verified.
    [00006.881] 01064.01066> [hello_c] POSIX Spawn: file actions init/adddup2/destroy working.
    [00006.881] 01064.01066> [hello_c] All Phase 3 C runtime, FD table, and spawn verifications PASSED!
    [00006.911] 01051.01054> [iris_init] hello_c process execution finished.
    [00006.925] 01051.01054> ========================================================
    [00006.925] 01051.01054> [iris_init] Phase 4 Step 4.1: Unified PCIe Discovery & Block Driver
    [00006.925] 01051.01054> ========================================================
    [00006.985] 01051.01054> [iris_init] Mapping ECAM at phys 0x00000000e0000000...
    [00007.045] 01051.01054> [iris_init] Discovered VirtIO Block Device at PCI 0x00:0x03 (Vendor: 0x1af4, Device: 0x1001)
    [00007.099] 01051.01054> [iris_init] Spawning bin/virtio_blk driver daemon...
    [00007.099] 01051.01054> [iris_init] Located bin/virtio_blk in BOOTFS! Spawning process...
    [00007.243] 01051.01054> [iris_init] Starting child process: virtio_blk...
    [00007.259] 01083.01085> [virtio_blk] irisOS VirtIO Block Driver Daemon starting...
    [00007.259] 01083.01085> [virtio_blk] Mapping PCIe ECAM configuration space...
    [00007.266] 01083.01085> [virtio_blk] Scanning PCIe buses for VirtIO block device...
    [00007.268] 01083.01085> [virtio_blk] Discovered VirtIO Block device at PCI 00:03.0 (Vendor: 0x1af4, Device: 0x1001)
    [00007.270] 01083.01085> [virtio_blk] Mapped BAR 4 (phys: 0xc000000000) to vaddr: 0x178a1539b000
    [00007.271] 01083.01085> [virtio_blk] Configuring Virtqueue 0 (size: 256 descriptors)...
    [00007.281] 01083.01085> [virtio_blk] VirtIO block device active! Capacity: 131072 sectors (64 MB)
    [00007.281] 01083.01085> [virtio_blk] Entering IPC server loop on channel...
    [00007.414] 01051.01054> [iris_init] Sending BLK_GET_INFO query over IPC channel...
    [00007.429] 01051.01054> [iris_init] VirtIO Block query successful! Capacity: 0x0000000000020000 sectors, Sector Size: 0x0000000000000200 bytes
    [00007.475] 01051.01054> [iris_init] Preparing 512-byte verification pattern for Sector 100...
    [00007.517] 01051.01054> [iris_init] Writing Sector 100 via IPC request...
    [00007.534] 01051.01054> [iris_init] Sector 100 write confirmed OK. Reading back over IPC...
    [00007.551] 01051.01054> [iris_init] Sector 100 read/write data verification: MATCHED! (512/512 bytes OK)
    [00007.551] 01051.01054> [iris_init] VirtIO Block read/write test on Sector 100: PASSED!
    [00007.581] 01051.01054> [iris_init] Phase 4 Step 4.1 verification COMPLETE!
    ```
  - **arm64 QEMU Console Output (`make run-arm`):**
    ```text
    [00008.822] 01044.01047> [irisOS] Welcome to irisOS (arm64) - iris_init bootstrap successful!
    [00008.822] 01044.01047> [iris_init] Root Resource: OK
    [00008.822] 01044.01047> [iris_init] BOOTFS VMO: OK
    [00008.822] 01044.01047> [iris_init] Root Job: OK
    [00008.822] 01044.01047> [iris_init] vDSO VMO: OK
    [00008.822] 01044.01047> [iris_init] Root VMAR: OK
    [00008.823] 01044.01047> [iris_init] Located bin/hello_c in BOOTFS! Spawning process...
    [00008.825] 01044.01047> [iris_init] Starting child process: hello_c...
    [00008.837] 01057.01059> [hello_c] Hello from standard C on irisOS! malloc/free/printf working.
    [00008.837] 01057.01059> [hello_c] FD Table: direct write(1) to stdout successful.
    [00008.839] 01057.01059> [hello_c] FD Table: isatty(1)=1 isatty(2)=1 (console tty verified).
    [00008.841] 01057.01059> [hello_c] FD Table: open("/dev/console") allocated fd 3, write OK.
    [00008.841] 01057.01059> [hello_c] FD Table: close(fd 3) successful.
    [00008.842] 01057.01059> [hello_c] FD Table: invalid fd rejection (EBADF) verified.
    [00008.842] 01057.01059> [hello_c] POSIX Spawn: file actions init/adddup2/destroy working.
    [00008.842] 01057.01059> [hello_c] All Phase 3 C runtime, FD table, and spawn verifications PASSED!
    [00008.846] 01044.01047> [iris_init] hello_c process execution finished.
    [00008.846] 01044.01047> ========================================================
    [00008.846] 01044.01047> [iris_init] Phase 4 Step 4.1: Unified PCIe Discovery & Block Driver
    [00008.846] 01044.01047> ========================================================
    [00008.846] 01044.01047> [iris_init] Mapping ECAM at phys 0x0000004010000000...
    [00008.848] 01044.01047> [iris_init] Discovered VirtIO Block Device at PCI 0x00:0x02 (Vendor: 0x1af4, Device: 0x1001)
    [00008.849] 01044.01047> [iris_init] Spawning bin/virtio_blk driver daemon...
    [00008.849] 01044.01047> [iris_init] Located bin/virtio_blk in BOOTFS! Spawning process...
    [00008.862] 01044.01047> [iris_init] Starting child process: virtio_blk...
    [00008.871] 01076.01078> [virtio_blk] irisOS VirtIO Block Driver Daemon starting...
    [00008.871] 01076.01078> [virtio_blk] Mapping PCIe ECAM configuration space...
    [00008.872] 01076.01078> [virtio_blk] Scanning PCIe buses for VirtIO block device...
    [00008.875] 01076.01078> [virtio_blk] Discovered VirtIO Block device at PCI 00:02.0 (Vendor: 0x1af4, Device: 0x1001)
    [00008.876] 01076.01078> [virtio_blk] Mapped BAR 4 (phys: 0x8000004000) to vaddr: 0x51eafeb4b000
    [00008.877] 01076.01078> [virtio_blk] Configuring Virtqueue 0 (size: 256 descriptors)...
    [00008.881] 01076.01078> [virtio_blk] VirtIO block device active! Capacity: 131072 sectors (64 MB)
    [00008.881] 01076.01078> [virtio_blk] Entering IPC server loop on channel...
    [00008.883] 01044.01047> [iris_init] Sending BLK_GET_INFO query over IPC channel...
    [00008.884] 01044.01047> [iris_init] VirtIO Block query successful! Capacity: 0x0000000000020000 sectors, Sector Size: 0x0000000000000200 bytes
    [00008.884] 01044.01047> [iris_init] Preparing 512-byte verification pattern for Sector 100...
    [00008.884] 01044.01047> [iris_init] Writing Sector 100 via IPC request...
    [00008.885] 01044.01047> [iris_init] Sector 100 write confirmed OK. Reading back over IPC...
    [00008.886] 01044.01047> [iris_init] Sector 100 read/write data verification: MATCHED! (512/512 bytes OK)
    [00008.886] 01044.01047> [iris_init] VirtIO Block read/write test on Sector 100: PASSED!
    [00008.886] 01044.01047> [iris_init] Phase 4 Step 4.1 verification COMPLETE!
    ```

---

### Developer Ergonomics & Build Orchestrator: Unified `build` & `run` Commands
- **Status:** COMPLETED & VERIFIED
- **Features Implemented:**
  - Added root [`Makefile`](Makefile) and [`scripts/iris.sh`](scripts/iris.sh) (`./iris` CLI) providing 3 build targets:
    - `make build all` (or `./iris build all`): Builds full images for both x86_64 and aarch64.
    - `make build x86_64` (or `./iris build x86_64`): Builds only for x86_64.
    - `make build aarch64` (or `./iris build aarch64`): Builds only for aarch64.
  - Implemented unified QEMU run commands with default graphical Cocoa window + simultaneous serial console in stdio:
    - `make run x86_64`: Boots the x86_64 VM directly in a QEMU Cocoa window with serial console in terminal, bypassing rebuilds if the image is already present.
    - `make run aarch64`: Boots the aarch64 VM directly with ramfb Cocoa window + serial console in terminal without rebuilding.
    - `make run all`: Prompts the user with a warning about launching 2 concurrent VMs across both architectures, and upon `y` confirmation, concurrently launches both VMs with Cocoa windows and tagged `[x86_64]` / `[aarch64]` multiplexed serial consoles.
- **Verification Commands:**
  ```bash
  make build x86_64
  make build aarch64
  echo "n" | make run all
  ./iris help
  ```

---

### System Polish, Visual Branding & Diagnostic Cleanup Pass
- **Status:** COMPLETED & VERIFIED (100% Dual-Architecture)
- **Features & Fixes Implemented:**
  1. **Kernel Command Line & QEMU Hardware Entropy (`build/mkimage.sh`, `scripts/run_qemu.sh`):**
     - Added `zircon.nodename=irisOS-dev` to the ZBI boot command line and Gigaboot default nodename.
     - Added `-device virtio-rng-pci` and `-cpu max` to QEMU invocations for both `x86_64` and `arm64`.
     - Eliminated x86_64 hypervisor microarchitecture false-positive warning (`Microarch: QEMU Virtual CPU`) and enabled ARMv8.5 `FEAT_RNG` (`RNDR`) hardware entropy collection in Zircon's `HwRngCollector`, eliminating early-boot randomness warnings across both architectures.
  2. **ASCII Boot Banner & Single-Buffer Systemd Logging (`servers/iris_init/src/main.c`):**
     - Implemented freestanding `vsnprintf` / `snprintf` in `iris_init` so every log message is formatted into a single 256-byte line buffer prior to calling `zx_debuglog_write()`, eliminating split lines and fragmented timestamp prefixes.
     - Added the official 6-line `irisOS` ASCII boot banner followed immediately by `System Node: irisOS-dev (<arch>)`.
     - Standardized all bootstrap, process, and driver status logs using `[  OK  ]` tags.
  3. **Interactive Test Suite Rename (`apps/tests/its_irisOS`):**
     - Renamed `apps/tests/hello_c` to `apps/tests/its_irisOS` (`bin/its_irisOS`).
     - Updated [`build/Makefile`](build/Makefile), [`build/mkimage.sh`](build/mkimage.sh), and [`servers/iris_init/src/main.c`](servers/iris_init/src/main.c) to build, package, and execute `bin/its_irisOS`.
     - Fixed 16-byte stack alignment (`and $~0xF, %rsp`) before `call __iris_start` in [`sdk/mlibc-sysdeps/iris/crt-x86_64/crt1.S`](sdk/mlibc-sysdeps/iris/crt-x86_64/crt1.S) for strict System V AMD64 ABI SSE/AVX compliance under `-cpu max`.
  4. **VirtIO BTI Teardown Cleanliness (`servers/drivers/virtio_blk/src/main.c`):**
     - Stored Pinned Memory Token handles (`g_vq_pmt` and `g_dma_pmt`) returned by `zx_bti_pin()`.
     - Added `cleanup_virtio_block_device()` to reset the VirtIO device and explicitly call `zx_pmt_unpin()` on both tokens upon driver exit, ensuring zero leaked pages are flagged by the microkernel BTI.
- **Dual-Architecture Verified Console Output:**
  - **x86_64 Verified Boot Output (`./scripts/run_qemu.sh -a x86_64 --headless`):**
    ```text
    [00006.898] 01051.01054>   _      _      ____   _____ 
    [00006.898] 01051.01054>  (_)    (_)    / __ \ / ____|
    [00006.898] 01051.01054>   _ _ __ _ ___| |  | | (___  
    [00006.898] 01051.01054>  | | '__| / __| |  | |\___ \ 
    [00006.898] 01051.01054>  | | |  | \__ \ |__| |____) |
    [00006.898] 01051.01054>  |_|_|  |_|___/\____/|_____/ 
    [00006.899] 01051.01054> System Node: irisOS-dev (x86_64)
    [00006.899] 01051.01054> [  OK  ] Extracted Root Resource capability.
    [00006.899] 01051.01054> [  OK  ] Extracted BOOTFS VMO capability.
    [00006.899] 01051.01054> [  OK  ] Extracted Root Job capability.
    [00006.899] 01051.01054> [  OK  ] Extracted vDSO VMO capability.
    [00006.899] 01051.01054> [  OK  ] Extracted Root VMAR capability.
    [00006.899] 01051.01054> [  OK  ] Located bin/its_irisOS in BOOTFS (2266696 bytes).
    [00006.903] 01051.01054> [  OK  ] Started process its_irisOS (bin/its_irisOS).
    [00006.968] 01064.01066> [  OK  ] its_irisOS: malloc/free & snprintf (standard C runtime heap & stdio verified).
    [00006.973] 01064.01066> [  OK  ] its_irisOS: FD Table direct write(1) to stdout.
    [00006.978] 01064.01066> [  OK  ] its_irisOS: FD Table isatty(1)=1, isatty(2)=1 (console TTY verified).
    [00006.983] 01064.01066> [  OK  ] its_irisOS: Console device open("/dev/console") -> fd 3, write & close.
    [00006.988] 01064.01066> [  OK  ] its_irisOS: FD Table invalid descriptor rejection (EBADF).
    [00006.992] 01064.01066> [  OK  ] its_irisOS: POSIX spawn file actions (init/adddup2/destroy).
    [00006.997] 01064.01066> [  OK  ] its_irisOS: Interactive Test Suite all checks passed.
    [00007.004] 01051.01054> [  OK  ] Process its_irisOS finished execution.
    [00007.014] 01051.01054> [  OK  ] Mapped PCIe ECAM configuration space at phys 0x00000000e0000000.
    [00007.020] 01051.01054> [  OK  ] Discovered VirtIO Block Device at PCI 00:03.0 (Vendor: 0x1af4, Device: 0x1001).
    [00007.025] 01051.01054> [  OK  ] Located bin/virtio_blk in BOOTFS (2240208 bytes).
    [00007.043] 01051.01054> [  OK  ] Started process virtio_blk (bin/virtio_blk).
    [00007.061] 01081.01083> [  OK  ] virtio_blk: Bound to PCI 00:03.0 (Vendor: 0x1af4, Device: 0x1001).
    [00007.067] 01081.01083> [  OK  ] virtio_blk: Mapped BAR 4 (phys 0xc000000000 -> vaddr 0x1f35a230d000).
    [00007.077] 01081.01083> [  OK  ] virtio_blk: Virtqueue 0 (256 descriptors) & BTI DMA initialized (131072 sectors, 64 MB).
    [00007.077] 01081.01083> [  OK  ] virtio_blk: IPC server listening on control channel.
    [00007.086] 01051.01054> [  OK  ] VirtIO Block IPC query (BLK_GET_INFO): 131072 sectors (512 bytes/sector).
    [00007.092] 01051.01054> [  OK  ] VirtIO Block IPC write to Sector 100 confirmed (512 bytes).
    [00007.098] 01051.01054> [  OK  ] VirtIO Block IPC read from Sector 100 verified (512/512 bytes matched).
    [00007.099] 01081.01083> [  OK  ] virtio_blk: Unpinned BTI DMA tokens (zx_pmt_unpin) and exited cleanly.
    [00007.107] 01051.01054> [  OK  ] Phase 4 Step 4.1 PCIe & VirtIO Block verification complete.
    ```
  - **arm64 Verified Boot Output (`./scripts/run_qemu.sh -a arm64 --headless`):**
    ```text
    [00009.124] 01044.01047>   _      _      ____   _____ 
    [00009.124] 01044.01047>  (_)    (_)    / __ \ / ____|
    [00009.124] 01044.01047>   _ _ __ _ ___| |  | | (___  
    [00009.124] 01044.01047>  | | '__| / __| |  | |\___ \ 
    [00009.124] 01044.01047>  | | |  | \__ \ |__| |____) |
    [00009.124] 01044.01047>  |_|_|  |_|___/\____/|_____/ 
    [00009.124] 01044.01047> System Node: irisOS-dev (arm64)
    [00009.124] 01044.01047> [  OK  ] Extracted Root Resource capability.
    [00009.124] 01044.01047> [  OK  ] Extracted BOOTFS VMO capability.
    [00009.124] 01044.01047> [  OK  ] Extracted Root Job capability.
    [00009.124] 01044.01047> [  OK  ] Extracted vDSO VMO capability.
    [00009.124] 01044.01047> [  OK  ] Extracted Root VMAR capability.
    [00009.124] 01044.01047> [  OK  ] Located bin/its_irisOS in BOOTFS (2385960 bytes).
    [00009.129] 01044.01047> [  OK  ] Started process its_irisOS (bin/its_irisOS).
    [00009.138] 01057.01059> [  OK  ] its_irisOS: malloc/free & snprintf (standard C runtime heap & stdio verified).
    [00009.138] 01057.01059> [  OK  ] its_irisOS: FD Table direct write(1) to stdout.
    [00009.138] 01057.01059> [  OK  ] its_irisOS: FD Table isatty(1)=1, isatty(2)=1 (console TTY verified).
    [00009.139] 01057.01059> [  OK  ] its_irisOS: Console device open("/dev/console") -> fd 3, write & close.
    [00009.139] 01057.01059> [  OK  ] its_irisOS: FD Table invalid descriptor rejection (EBADF).
    [00009.139] 01057.01059> [  OK  ] its_irisOS: POSIX spawn file actions (init/adddup2/destroy).
    [00009.139] 01057.01059> [  OK  ] its_irisOS: Interactive Test Suite all checks passed.
    [00009.144] 01044.01047> [  OK  ] Process its_irisOS finished execution.
    [00009.146] 01044.01047> [  OK  ] Mapped PCIe ECAM configuration space at phys 0x0000004010000000.
    [00009.146] 01044.01047> [  OK  ] Discovered VirtIO Block Device at PCI 00:02.0 (Vendor: 0x1af4, Device: 0x1001).
    [00009.147] 01044.01047> [  OK  ] Located bin/virtio_blk in BOOTFS (2357520 bytes).
    [00009.150] 01044.01047> [  OK  ] Started process virtio_blk (bin/virtio_blk).
    [00009.169] 01074.01076> [  OK  ] virtio_blk: Bound to PCI 00:02.0 (Vendor: 0x1af4, Device: 0x1001).
    [00009.170] 01074.01076> [  OK  ] virtio_blk: Mapped BAR 4 (phys 0x8000004000 -> vaddr 0x66545f962000).
    [00009.174] 01074.01076> [  OK  ] virtio_blk: Virtqueue 0 (256 descriptors) & BTI DMA initialized (131072 sectors, 64 MB).
    [00009.174] 01074.01076> [  OK  ] virtio_blk: IPC server listening on control channel.
    [00009.176] 01044.01047> [  OK  ] VirtIO Block IPC query (BLK_GET_INFO): 131072 sectors (512 bytes/sector).
    [00009.178] 01044.01047> [  OK  ] VirtIO Block IPC write to Sector 100 confirmed (512 bytes).
    [00009.179] 01044.01047> [  OK  ] VirtIO Block IPC read from Sector 100 verified (512/512 bytes matched).
    [00009.180] 01074.01076> [  OK  ] virtio_blk: Unpinned BTI DMA tokens (zx_pmt_unpin) and exited cleanly.
    [00009.185] 01044.01047> [  OK  ] Phase 4 Step 4.1 PCIe & VirtIO Block verification complete.
    ```

---

### Step 4.2: Block-Level Encryption Proxy (`iris_crypt`) with Mbed-TLS (AES-256-XTS + Argon2id)
- **Status:** COMPLETED & VERIFIED (100% Dual-Architecture)
- **Licensing:** STRICTLY Apache 2.0 / MIT (Zone 1 Permissive).
- **Components Implemented:**
  1. **Mbed-TLS 3.6 Fetch & Freestanding Build (`scripts/fetch_mbedtls.sh`, `build/mbedtls_iris_config.h`, `build/CMakeLists.txt`, `build/Makefile`):**
     - Created [`scripts/fetch_mbedtls.sh`](scripts/fetch_mbedtls.sh) fetching pinned Mbed-TLS `v3.6.2` from primary mirror `https://github.com/stirbeiu/mbedtls.git` with automatic fallback to `https://github.com/Mbed-TLS/mbedtls.git`, shallow clone (`--depth 1`), and `--clean` support.
     - Configured minimal freestanding [`build/mbedtls_iris_config.h`](build/mbedtls_iris_config.h) enabling AES (`MBEDTLS_AES_C`), XTS mode (`MBEDTLS_CIPHER_MODE_XTS`), SHA-256 (`MBEDTLS_SHA256_C`), SHA-512 (`MBEDTLS_SHA512_C`), PKCS5, HKDF, and platform zeroization.
     - Implemented freestanding memory-hard Argon2id key derivation (`iris_argon2id_derive`) in [`servers/iris_crypt/src/argon2.c`](servers/iris_crypt/src/argon2.c) using 1 KiB BlaMka G-round blocks and Mbed-TLS SHA-512 variable-length hashing ($H'$).
     - Built `out/build-x86_64/lib/libmbedcrypto.a` and `out/build-arm64/lib/libmbedcrypto.a` via Homebrew LLVM.
  2. **Sector 0 Metadata Header & Ring 3 Block Proxy (`servers/iris_crypt/include/iris_crypt.h`, `servers/iris_crypt/src/main.c` -> `bin/iris_crypt`):**
     - Defined 4096-byte (8-sector) on-disk `iris_crypt_header_t` with magic `0x5450595243534952ULL` (`"IRISCRYPT"`), format version `1`, 32-byte Argon2 salt, `argon2_t_cost`/`argon2_m_cost`/`argon2_lanes` parameters, 64-byte AES-256-XTS wrapped master key, and SHA-256 verification/integrity digests.
     - Implemented Phase 4 deterministic development key fallback (`IRIS_CRYPT_DEV_PASSPHRASE`) so automated QEMU boots initialize/unlock Sector 0 and mount without blocking on interactive input.
     - Implemented `IRIS_PROTO_DRIVER` block proxy service translating logical sector $L$ to physical sector $P = L + 8$ on downstream `virtio_blk`:
       * `IRIS_BLK_OP_GET_INFO`: Adjusts reported capacity by subtracting the 8 metadata sectors (`131,072` physical sectors $\rightarrow$ `131,064` usable logical sectors).
       * `IRIS_BLK_OP_WRITE`: Encrypts each 512-byte sector using Mbed-TLS `mbedtls_aes_crypt_xts(MBEDTLS_AES_ENCRYPT)` with 64-bit little-endian logical sector LBA as the 16-byte XTS `data_unit` tweak, then writes ciphertext to physical Sector $L + 8$ on `virtio_blk`.
       * `IRIS_BLK_OP_READ`: Reads ciphertext from physical Sector $L + 8$ on `virtio_blk`, decrypts via `mbedtls_aes_crypt_xts(MBEDTLS_AES_DECRYPT)`, and returns plaintext to the caller.
       * `IRIS_BLK_OP_FLUSH`: Forwards cache flush transparently to `virtio_blk`.
     - Enhanced [`servers/drivers/virtio_blk/src/main.c`](servers/drivers/virtio_blk/src/main.c) to support multiple concurrent client channels transferred via Zircon capability handle passing.
  3. **End-to-End Verification & Subsystem Documentation (`servers/iris_init/src/main.c`, `docs/storage_and_vfs.md`):**
     - Updated `iris_init` to spawn `virtio_blk` and `iris_crypt`, write 512-byte plaintext to logical Sector 0 via `iris_crypt`, read physical Sector 8 directly from `virtio_blk` to prove on-disk data is AES-256-XTS ciphertext (`plain[0..3]=49524953 != cipher[0..3]=5ba520ff`), and read back logical Sector 0 via `iris_crypt` to verify 512/512 bytes match original plaintext.
     - Authored [`docs/storage_and_vfs.md`](docs/storage_and_vfs.md) documenting the Ring 3 storage pipeline, 4 KB Sector 0 metadata header, Argon2id + AES-256-XTS key hierarchy, GPT layout, and VFS routing.
- **Dual-Architecture Verified Console Output:**
  - **x86_64 Verified Boot Output (`./scripts/run_qemu.sh -a x86_64 --headless`):**
    ```text
    [00007.314] 01051.01054>   _      _      ____   _____ 
    [00007.314] 01051.01054>  (_)    (_)    / __ \ / ____|
    [00007.314] 01051.01054>   _ _ __ _ ___| |  | | (___  
    [00007.314] 01051.01054>  | | '__| / __| |  | |\___ \ 
    [00007.314] 01051.01054>  | | |  | \__ \ |__| |____) |
    [00007.314] 01051.01054>  |_|_|  |_|___/\____/|_____/ 
    [00007.314] 01051.01054> System Node: irisOS-dev (x86_64)
    [00007.314] 01051.01054> [  OK  ] Extracted Root Resource capability.
    [00007.314] 01051.01054> [  OK  ] Extracted BOOTFS VMO capability.
    [00007.314] 01051.01054> [  OK  ] Extracted Root Job capability.
    [00007.314] 01051.01054> [  OK  ] Extracted vDSO VMO capability.
    [00007.314] 01051.01054> [  OK  ] Extracted Root VMAR capability.
    [00007.314] 01051.01054> [  OK  ] Located bin/its_irisOS in BOOTFS (2266696 bytes).
    [00007.318] 01051.01054> [  OK  ] Started process its_irisOS (bin/its_irisOS).
    [00007.383] 01064.01066> [  OK  ] its_irisOS: malloc/free & snprintf (standard C runtime heap & stdio verified).
    [00007.388] 01064.01066> [  OK  ] its_irisOS: FD Table direct write(1) to stdout.
    [00007.393] 01064.01066> [  OK  ] its_irisOS: FD Table isatty(1)=1, isatty(2)=1 (console TTY verified).
    [00007.398] 01064.01066> [  OK  ] its_irisOS: Console device open("/dev/console") -> fd 3, write & close.
    [00007.402] 01064.01066> [  OK  ] its_irisOS: FD Table invalid descriptor rejection (EBADF).
    [00007.406] 01064.01066> [  OK  ] its_irisOS: POSIX spawn file actions (init/adddup2/destroy).
    [00007.406] 01064.01066> [  OK  ] its_irisOS: Interactive Test Suite all checks passed.
    [00007.420] 01051.01054> [  OK  ] Process its_irisOS finished execution.
    [00007.432] 01051.01054> [  OK  ] Mapped PCIe ECAM configuration space at phys 0x00000000e0000000.
    [00007.437] 01051.01054> [  OK  ] Discovered VirtIO Block Device at PCI 00:03.0 (Vendor: 0x1af4, Device: 0x1001).
    [00007.445] 01051.01054> [  OK  ] Located bin/virtio_blk in BOOTFS (2240656 bytes).
    [00007.456] 01051.01054> [  OK  ] Started process virtio_blk (bin/virtio_blk).
    [00007.486] 01081.01083> [  OK  ] virtio_blk: Bound to PCI 00:03.0 (Vendor: 0x1af4, Device: 0x1001).
    [00007.498] 01081.01083> [  OK  ] virtio_blk: Mapped BAR 4 (phys 0xc000000000 -> vaddr 0x27ac6507000).
    [00007.508] 01081.01083> [  OK  ] virtio_blk: Virtqueue 0 (256 descriptors) & BTI DMA initialized (131072 sectors, 64 MB).
    [00007.508] 01081.01083> [  OK  ] virtio_blk: IPC server listening on control channel.
    [00007.517] 01051.01054> [  OK  ] VirtIO Block IPC query (BLK_GET_INFO): 131072 sectors (512 bytes/sector).
    [00007.523] 01051.01054> [  OK  ] VirtIO Block IPC write to Sector 100 confirmed (512 bytes).
    [00007.529] 01051.01054> [  OK  ] VirtIO Block IPC read from Sector 100 verified (512/512 bytes matched).
    [00007.529] 01051.01054> [  OK  ] Phase 4 Step 4.1 PCIe & VirtIO Block verification complete.
    [00007.537] 01051.01054> [  OK  ] Located bin/iris_crypt in BOOTFS (2268848 bytes).
    [00007.560] 01051.01054> [  OK  ] Started process iris_crypt (bin/iris_crypt).
    [00007.582] 01107.01109> [  OK  ] iris_crypt: Initialized 4 KB Sector 0 header (magic 0x5450595243534952, Argon2id + AES-256-XTS).
    [00007.587] 01107.01109> [  OK  ] iris_crypt: Unlocked AES-256-XTS master key via Argon2id dev key fallback.
    [00007.593] 01107.01109> [  OK  ] iris_crypt: Block proxy listening (logical Sector 0 -> physical Sector 8, 131064 sectors).
    [00007.597] 01051.01054> [  OK  ] iris_crypt IPC query (BLK_GET_INFO): 131064 usable sectors (8 sectors / 4 KB reserved for Sector 0 header).
    [00007.603] 01051.01054> [  OK  ] Wrote 512-byte plaintext to logical Sector 0 via iris_crypt.
    [00007.608] 01051.01054> [  OK  ] Verified physical Sector 8 on virtio_blk is AES-256-XTS ciphertext (plain[0..3]=49524953 != cipher[0..3]=5ba520ff).
    [00007.617] 01051.01054> [  OK  ] Verified logical Sector 0 read via iris_crypt matches original plaintext (512/512 bytes matched).
    [00007.621] 01107.01109> [  OK  ] iris_crypt: Zeroized AES-256-XTS keys and exited cleanly.
    [00007.628] 01081.01083> [  OK  ] virtio_blk: Unpinned BTI DMA tokens (zx_pmt_unpin) and exited cleanly.
    [00007.633] 01051.01054> [  OK  ] Phase 4 Step 4.2 iris_crypt AES-256-XTS proxy verification complete.
    ```
  - **arm64 Verified Boot Output (`./scripts/run_qemu.sh -a arm64 --headless`):**
    ```text
    [00009.176] 01044.01047>   _      _      ____   _____ 
    [00009.176] 01044.01047>  (_)    (_)    / __ \ / ____|
    [00009.176] 01044.01047>   _ _ __ _ ___| |  | | (___  
    [00009.176] 01044.01047>  | | '__| / __| |  | |\___ \ 
    [00009.176] 01044.01047>  | | |  | \__ \ |__| |____) |
    [00009.176] 01044.01047>  |_|_|  |_|___/\____/|_____/ 
    [00009.176] 01044.01047> System Node: irisOS-dev (arm64)
    [00009.176] 01044.01047> [  OK  ] Extracted Root Resource capability.
    [00009.176] 01044.01047> [  OK  ] Extracted BOOTFS VMO capability.
    [00009.176] 01044.01047> [  OK  ] Extracted Root Job capability.
    [00009.176] 01044.01047> [  OK  ] Extracted vDSO VMO capability.
    [00009.176] 01044.01047> [  OK  ] Extracted Root VMAR capability.
    [00009.176] 01044.01047> [  OK  ] Located bin/its_irisOS in BOOTFS (2385960 bytes).
    [00009.179] 01044.01047> [  OK  ] Started process its_irisOS (bin/its_irisOS).
    [00009.188] 01057.01059> [  OK  ] its_irisOS: malloc/free & snprintf (standard C runtime heap & stdio verified).
    [00009.188] 01057.01059> [  OK  ] its_irisOS: FD Table direct write(1) to stdout.
    [00009.189] 01057.01059> [  OK  ] its_irisOS: FD Table isatty(1)=1, isatty(2)=1 (console TTY verified).
    [00009.189] 01057.01059> [  OK  ] its_irisOS: Console device open("/dev/console") -> fd 3, write & close.
    [00009.189] 01057.01059> [  OK  ] its_irisOS: FD Table invalid descriptor rejection (EBADF).
    [00009.190] 01057.01059> [  OK  ] its_irisOS: POSIX spawn file actions (init/adddup2/destroy).
    [00009.190] 01057.01059> [  OK  ] its_irisOS: Interactive Test Suite all checks passed.
    [00009.194] 01044.01047> [  OK  ] Process its_irisOS finished execution.
    [00009.196] 01044.01047> [  OK  ] Mapped PCIe ECAM configuration space at phys 0x0000004010000000.
    [00009.197] 01044.01047> [  OK  ] Discovered VirtIO Block Device at PCI 00:02.0 (Vendor: 0x1af4, Device: 0x1001).
    [00009.197] 01044.01047> [  OK  ] Located bin/virtio_blk in BOOTFS (2357912 bytes).
    [00009.204] 01044.01047> [  OK  ] Started process virtio_blk (bin/virtio_blk).
    [00009.219] 01074.01076> [  OK  ] virtio_blk: Bound to PCI 00:02.0 (Vendor: 0x1af4, Device: 0x1001).
    [00009.220] 01074.01076> [  OK  ] virtio_blk: Mapped BAR 4 (phys 0x8000004000 -> vaddr 0x851880892000).
    [00009.225] 01074.01076> [  OK  ] virtio_blk: Virtqueue 0 (256 descriptors) & BTI DMA initialized (131072 sectors, 64 MB).
    [00009.225] 01074.01076> [  OK  ] virtio_blk: IPC server listening on control channel.
    [00009.226] 01044.01047> [  OK  ] VirtIO Block IPC query (BLK_GET_INFO): 131072 sectors (512 bytes/sector).
    [00009.229] 01044.01047> [  OK  ] VirtIO Block IPC write to Sector 100 confirmed (512 bytes).
    [00009.230] 01044.01047> [  OK  ] VirtIO Block IPC read from Sector 100 verified (512/512 bytes matched).
    [00009.230] 01044.01047> [  OK  ] Phase 4 Step 4.1 PCIe & VirtIO Block verification complete.
    [00009.231] 01044.01047> [  OK  ] Located bin/iris_crypt in BOOTFS (2383520 bytes).
    [00009.232] 01044.01047> [  OK  ] Started process iris_crypt (bin/iris_crypt).
    [00009.247] 01100.01102> [  OK  ] iris_crypt: Initialized 4 KB Sector 0 header (magic 0x5450595243534952, Argon2id + AES-256-XTS).
    [00009.248] 01100.01102> [  OK  ] iris_crypt: Unlocked AES-256-XTS master key via Argon2id dev key fallback.
    [00009.248] 01100.01102> [  OK  ] iris_crypt: Block proxy listening (logical Sector 0 -> physical Sector 8, 131064 sectors).
    [00009.249] 01044.01047> [  OK  ] iris_crypt IPC query (BLK_GET_INFO): 131064 usable sectors (8 sectors / 4 KB reserved for Sector 0 header).
    [00009.251] 01044.01047> [  OK  ] Wrote 512-byte plaintext to logical Sector 0 via iris_crypt.
    [00009.251] 01044.01047> [  OK  ] Verified physical Sector 8 on virtio_blk is AES-256-XTS ciphertext (plain[0..3]=49524953 != cipher[0..3]=5ba520ff).
    [00009.252] 01044.01047> [  OK  ] Verified logical Sector 0 read via iris_crypt matches original plaintext (512/512 bytes matched).
    [00009.253] 01100.01102> [  OK  ] iris_crypt: Zeroized AES-256-XTS keys and exited cleanly.
    [00009.256] 01074.01076> [  OK  ] virtio_blk: Unpinned BTI DMA tokens (zx_pmt_unpin) and exited cleanly.
    [00009.264] 01044.01047> [  OK  ] Phase 4 Step 4.2 iris_crypt AES-256-XTS proxy verification complete.
    ```

---

## Phase 4 Active Next Step
- **Immediate Next Step:** Phase 4, Step 4.3: Monolithic VFS Router & Filesystem Server (`iris_fshost`) with GPT partition parser, FatFs (`ESP`), and `libufs` UFS2 mounting (`/System` read-only from `System_A` and `/Users` read-write from decrypted `Data`).
