# Project Specification: irisOS

## 1. Role & Objective
You are assisting in the architecture and development of **irisOS**, a modern, 64-bit, microkernel-based operating system. 
Always adhere strictly to the architectural rules, licensing boundaries, directory structure, and 6-phase roadmap defined below. Do not introduce legacy 32-bit/BIOS code, GPL contamination in shared libraries, or upper-layer Google Fuchsia frameworks.

---

## 2. Core Constraints & Philosophy
* **OS Name:** `irisOS` (one word).
* **Target Architectures:** `x86_64` and `AArch64` (`aarch64`) ONLY.
* **Zero Legacy Bloat:** UEFI boot only. No 16-bit/32-bit real/protected mode code, no legacy BIOS.
* **Microkernel Architecture:** Based on a stripped-down **Zircon** microkernel (`bringup` target only). All hardware drivers, filesystems, encryption layers, and window compositors run as isolated user-space processes in **Ring 3 (x86_64) / EL0 (AArch64)**.
* **No Google UI/Runtime Frameworks:** Use ONLY the Zircon microkernel, its C vDSO (`libzircon.so`), and the `zbi` host packaging tool. Strictly exclude Dart, Flutter, FIDL, Scenic, and Fuchsia's component framework.
* **Reuse Proven FOSS:** Avoid reinventing wheels. Use battle-tested open-source libraries in `vendor/`.

---

## 3. Two-Zone Licensing Policy (CRITICAL)
End-users and businesses must be able to develop and distribute **100% proprietary, closed-source applications and hardware drivers** on `irisOS` without legal obligation to disclose their source code.

1. **Zone 1 — SDK & Shared Libraries (STRICTLY MIT / BSD / ISC / Apache 2.0 / FTL ONLY):**
   * Any library linked by user-space applications or drivers MUST use a permissive license.
   * **Strictly NO GPL or LGPL** in `sdk/` or core system libraries (`libiris`, `libiris_gui`, `mlibc`, `pixman`, `freetype`, `libzircon.so`). FreeType is explicitly designated as `FTL (Permissive BSD-style)`.
2. **Zone 2 — Standalone Programs & Utilities (MIT / BSD / GPL Allowed):**
   * Standalone executables that run in their own address space and communicate only via IPC or standard streams (`toybox`, and optional user-installable packages like `bash`, `coreutils`, or `nano`) may use any FOSS license.

---

## 4. Locked-In Technical Stack

### 4.1 Host Environment & Cross-Toolchain (macOS Apple Silicon)
* **Host Platform:** macOS (`Darwin arm64`, Apple Silicon).
* **Required Host Tools:**
  ```bash
  brew install llvm cmake ninja meson qemu xorriso mtools
  ```
* **Toolchain Binaries:** Apple's native `/usr/bin/clang` targets Mach-O only and lacks `ld.lld`. All cross-compilation toolchains in `build/toolchains/` (`x86_64-iris.cmake`, `arm64-iris.cmake`, `meson-x86_64.txt`, `meson-arm64.txt`) MUST explicitly use Homebrew LLVM binaries:
  * Compiler: `/opt/homebrew/opt/llvm/bin/clang` and `/opt/homebrew/opt/llvm/bin/clang++`
  * Linker: `/opt/homebrew/opt/llvm/bin/ld.lld`
  * Archiver & Utilities: `/opt/homebrew/opt/llvm/bin/llvm-ar`, `/opt/homebrew/opt/llvm/bin/llvm-objcopy`
  * Target Triples: `x86_64-unknown-elf` and `aarch64-unknown-elf`

### 4.2 Zircon Kernel Source, Version Pinning & Build System
* **No `jiri` or Full Fuchsia `GN`:** Avoid Google's multi-gigabyte Fuchsia monorepo build infrastructure.
* **Source Fetching:** `scripts/fetch_zircon.sh` fetches a pinned commit SHA of the standalone Zircon kernel source tree into `kernel/zircon/`.
* **Build Wrapper:** Compile the Zircon microkernel, `libzircon.so` (vDSO), and the host `zbi` tool using irisOS's standalone CMake/Ninja build wrapper configured with Homebrew LLVM (falling back to a lightweight Linux container only if strictly required for a specific legacy assembly step).

### 4.3 Bootloader & Dual-ZBI A/B Image Format (Gigaboot)
* **Bootloader:** **Gigaboot** (Zircon's native UEFI bootloader stub) compiled for UEFI x86_64 (`BOOTX64.EFI`) and AArch64 (`BOOTAA64.EFI`). Limine is excluded.
* **Dual-ZBI Layout on ESP:** To ensure kernel and early BOOTFS updates have full A/B rollback protection alongside `/System`, the FAT32 `ESP` stores two boot images:
  * `/EFI/BOOT/iris_a.zbi` (Kernel + Slot A BOOTFS)
  * `/EFI/BOOT/iris_b.zbi` (Kernel + Slot B BOOTFS)
* **Slot Selection:** Gigaboot reads GPT partition attributes (Priority, Tries Remaining, Successful) to select the active slot (`A` or `B`) and passes the active slot command line to `iris_init`.

### 4.4 Kernel-to-Userspace Bootstrap ABI (`userboot` -> `iris_init`)
* **No `argc`/`argv` Entry:** `iris_init` does not use standard C runtime entry (`int main`). It is launched directly by Zircon's `userboot` and receives its initial bootstrap channel via `<zircon/processargs.h>` (`zx_proc_args_t`).
* **Bootstrap Handle Extraction:** `iris_init` extracts the following essential capabilities from the bootstrap message:
  1. `PA_HND(PA_RESOURCE, 0)`: **Root Resource handle**, required to derive MMIO, IRQ, and I/O port capabilities for Ring 3 device drivers.
  2. `PA_HND(PA_VMO_BOOTFS, 0)`: **BOOTFS VMO handle**, containing early user-space binaries and drivers.
  3. `PA_HND(PA_JOB_DEFAULT, 0)`: **Root Job handle**, used to spawn isolated child jobs and server processes.
  4. `PA_HND(PA_VMO_VDSO, 0)`: **vDSO VMO handle** (`libzircon.so`), required to map system calls into child address spaces.
  5. **Early Serial Logging:** Created immediately via `zx_debuglog_create(root_resource, 0, &log_handle)` to stream console output to UART before `iris_pty` is initialized.

### 4.5 Inter-Process Communication (IPC Wire Protocol)
* **No FIDL:** Communication across Ring 3 processes uses raw Zircon channels (`zx_channel_*`) structured by a standard 24-byte C-ABI header defined in `sdk/libiris/include/iris/ipc.h`:
  ```c
  #include <stdint.h>
  #include <zircon/types.h>

  #define IRIS_IPC_MAGIC 0x49524953 // 'IRIS'

  #pragma pack(push, 4)
  typedef struct iris_msg_hdr {
      uint32_t magic;       // Must be IRIS_IPC_MAGIC
      uint32_t protocol;    // Subsystem ID (e.g. FS, CRYPT, COMPOSITOR, PTY)
      uint32_t opcode;      // Subsystem command/event opcode
      uint32_t txid;        // Transaction ID for request/response pairing
      int32_t  status;      // zx_status_t (ZX_OK, ZX_ERR_*, etc.)
      uint32_t payload_len; // Length of payload following this header in bytes
  } iris_msg_hdr_t;
  #pragma pack(pop)
  ```
* All Ring 3 servers (`iris_fshost`, `iris_crypt`, `iris_compositor`, `iris_pty`) and client libraries (`libiris`, `libiris_gui`, `mlibc-sysdeps`) communicate using this common wire protocol.

### 4.6 Capability Model, VFS & POSIX Emulation (`mlibc`)
* **Process Model (No `fork()`):** Zircon does not support copy-on-write `fork()`. Process creation is implemented via **`posix_spawn()`** over `zx_process_create()` and `zx_process_start()`.
* **C Library & Dynamic Linker:** **`mlibc`** (MIT) with irisOS sysdeps in `sdk/mlibc-sysdeps/iris/` producing `libc.so` and `ld-iris.so`.
* **VFS Router:** `iris_fshost` is a monolithic VFS mount manager and path router handling `/System`, `/Users`, `/Applications`, and the compatibility symlinks (`/bin`, `/lib`).
* **Client-Side File Descriptor Table:** `sdk/mlibc-sysdeps/iris/` maintains an internal table mapping `int fd -> zx_handle_t channel`. File operations (`sys_open`, `sys_read`, `sys_write`, `sys_seek`, `sys_close`) issue IPC messages to `iris_fshost`.
* **FD Inheritance:** `posix_spawn()` transfers open file descriptors to child processes using Zircon's standard `PA_HND(PA_FD, fd)` handle info tags in `zx_proc_args_t`.

### 4.7 Storage, Encryption & A/B Updates
* **Unified PCIe Device Discovery:** Standardize on **PCIe (ECAM)** for VirtIO devices (`virtio-blk-pci`, `virtio-gpu-pci`, `virtio-input-pci`) on BOTH `x86_64` (QEMU `-M q35`) and `AArch64` (QEMU `-M virt`). This allows `iris_init` and Ring 3 drivers to share a single PCIe enumeration codebase.
* **Block Driver:** User-space `virtio-blk-pci` using Zircon **BTI (`zx_bti_pin`)** for safe DMA and `zx_interrupt_wait()` for PCIe MSI/INTx interrupts.
* **Block-Level Encryption (`iris_crypt`):**
  * Sits as a block filter between `libufs` and `virtio-blk-pci`: `[libufs] <==> [iris_crypt] <==> [virtio-blk-pci]`.
  * Encrypts the `Data` partition with **AES-256-XTS** + **Argon2** using `mbedtls`.
  * **Metadata Header:** Sector 0 (first 4 KB) of the `Data` partition is reserved for the `iris_crypt` metadata header (Argon2 salt, cost parameters, and encrypted master key).
  * **Phase 4 Test Vector:** For automated testing prior to Phase 5's interactive passphrase prompt, `iris_crypt` supports a deterministic development test key fallback.
* **Partition Table (GPT with 4 Partitions):**
  1. `ESP` (FAT32 via **FatFs** — Gigaboot, `iris_a.zbi`, `iris_b.zbi`).
  2. `System_A` (Active immutable read-only **UFS2** via **libufs**).
  3. `System_B` (Inactive slot for seamless A/B updates with rollback watchdog).
  4. `Data` (Read-Write **UFS2** via **libufs**, block-encrypted by `iris_crypt`).
* **Developer Mode Override:** Optional writable RAM `OverlayFS` over `/System` when unlocked by the user.

### 4.8 Graphics & Windowing
* **Drivers:** UEFI GOP Framebuffer fallback + `virtio-gpu-pci` and `virtio-input-pci`.
* **Compositor (`iris_compositor`):** Zero-copy shared memory windowing using Zircon **VMOs** + IPC damage notifications.
* **Rendering Libraries:** **Pixman** (MIT) for 2D SIMD compositing + **FreeType** (FTL (Permissive BSD-style)) for font rendering.
* **System Shell & CLI Utilities:** **`Toybox`** (0BSD) compiled as a single binary providing `sh` and 200+ POSIX utilities without relying on `fork()`.

### 4.9 Multi-Build System Orchestrator (`build/`)
* A master top-level `Makefile` and helper orchestrator scripts inside `build/` coordinate the heterogeneous build systems across submodules:
  * CMake: `libufs`, `mbedtls`, `kernel/zircon`
  * Meson: `mlibc`, `pixman`, `freetype`
  * Makefile/kconfig: `toybox`
* The orchestrator automatically propagates Homebrew LLVM paths, cross-compilation target triples (`--target=x86_64-unknown-elf`, `--target=aarch64-unknown-elf`), and sysroot header/library search paths to all subprojects.

---

## 5. Git Repository Structure
Maintain this exact directory layout:

```text
irisOS/
├── build/                    # Master build scripts & multi-toolchain orchestrator
│   ├── Makefile              # Top-level orchestrator calling CMake, Meson, and Make subprojects
│   ├── toolchains/           # Homebrew LLVM configs (x86_64-iris.cmake, arm64-iris.cmake, meson-x86_64.txt, etc.)
│   └── mkimage.sh            # Packs dual ZBIs (.zbi), System_A/B images, and GPT disk.img
├── docs/                     # Subsystem documentation (architecture, IPC, VFS, build guides)
├── scripts/
│   ├── fetch_zircon.sh       # Fetches pinned commit of Zircon kernel + vDSO + zbi tool
│   └── run_qemu.sh           # Launches QEMU (x86_64 q35 or aarch64 virt) with UEFI & PCIe VirtIO
├── kernel/                   # [Kernel Zone - MIT]
│   └── zircon/               # Pinned Zircon microkernel source tree
├── sdk/                      # [SDK Zone - STRICTLY MIT / BSD ONLY]
│   ├── libiris/              # Native C wrapper for Zircon primitives & 24-byte IPC wire protocol
│   │   └── include/iris/ipc.h# iris_msg_hdr_t IPC definition
│   ├── libiris_gui/          # Client windowing library (wraps Pixman + FreeType + VMO surfaces)
│   └── mlibc-sysdeps/        # irisOS sysdeps (sysdeps/iris/) injected into vendor/mlibc
├── servers/                  # [Server Zone - MIT / BSD] (Ring 3 User-Space Daemons)
│   ├── iris_init/            # Bootstrap process: extracts userboot handles, scans PCIe, launches servers
│   ├── iris_fshost/          # Monolithic VFS router, GPT parser, A/B slot selector, UFS2/libufs server
│   ├── iris_crypt/           # Block-level AES-256-XTS encryption proxy for the Data partition
│   ├── iris_compositor/      # Zero-copy VMO window server & input event router
│   ├── iris_pty/             # Pseudo-terminal server for line buffering and signals (Ctrl+C)
│   └── drivers/              # Ring 3 PCIe hardware drivers (virtio-blk-pci, virtio-gpu-pci, virtio-input-pci, uart)
├── apps/                     # [App Zone - MIT / BSD / GPL Allowed]
│   ├── terminal/             # Graphical terminal emulator app
│   └── sysutils/             # Passphrase prompt, A/B update client, system tools
├── vendor/                   # [Third-Party FOSS Submodules - Untouched Upstream]
│   ├── mlibc/                # (MIT) Standard C library + dynamic linker [Meson]
│   ├── toybox/               # (0BSD) Shell and 200+ CLI utilities [Makefile]
│   ├── libufs/               # (BSD 2-Clause) FreeBSD UFS2/FFS filesystem library [CMake]
│   ├── fatfs/                # (BSD) FAT32 library for ESP partition [CMake]
│   ├── mbedtls/              # (Apache 2.0) AES-256-XTS & Argon2 crypto [CMake]
│   ├── pixman/               # (MIT) 2D pixel manipulation library [Meson]
│   └── freetype/             # (FTL (Permissive BSD-style)) Font engine [Meson/CMake]
├── PROGRESS.md               # Live step-by-step milestone and progress tracker
└── out/                      # Git-ignored build output & QEMU disk images
```

---

## 6. Runtime Filesystem Hierarchy (macOS-Style)

`iris_fshost` exposes this exact directory tree at runtime **(do NOT use traditional Linux `/usr` clutter or Fuchsia namespaces)**:

```text
/
├── System/                   # Mounted Read-Only from active GPT slot (System_A or System_B)
│   ├── Bin/                  # toybox binary and command symlinks (sh, ls, cp, cat, etc.)
│   ├── Libraries/            # ld-iris.so, libc.so, libzircon.so, libiris.so, libpixman.so
│   ├── Servers/              # iris_compositor, non-boot drivers, background daemons
│   └── Resources/            # System fonts (.ttf), icons, default configs
├── Users/                    # Mounted Read-Write from AES-256-XTS encrypted Data partition
│   └── admin/                # User home directory
├── Applications/             # Mounted Read-Write from encrypted Data partition
├── bin -> /System/Bin        # Virtual symlink in RAM for POSIX script compatibility (#!/bin/sh)
└── lib -> /System/Libraries  # Virtual symlink in RAM for ELF dynamic loader compatibility
```

---

## 7. Phased Implementation Roadmap

### Phase 1 & 2: Toolchain, Pinned Zircon Build & iris_init Handoff

* Verify host requirements (`brew install llvm cmake ninja meson qemu xorriso mtools`).
* Create `build/toolchains/` targeting Homebrew's LLVM (`/opt/homebrew/opt/llvm/bin/clang`, `ld.lld`).
* Implement `scripts/fetch_zircon.sh` to fetch a pinned commit SHA of the standalone Zircon kernel.
* Build the Zircon bringup kernel, `libzircon.so` (vDSO), and the host `zbi` tool using the CMake/Ninja wrapper.
* Compile **Gigaboot** as the UEFI bootloader (`BOOTX64.EFI` / `BOOTAA64.EFI`).
* Write a freestanding `iris_init` binary linked against `libzircon.so` that:
  1. Receives `zx_proc_args_t` from `userboot`.
  2. Extracts `PA_RESOURCE`, `PA_VMO_BOOTFS`, `PA_JOB_DEFAULT`, and `PA_VMO_VDSO`.
  3. Creates an early debug log via `zx_debuglog_create` and logs "Hello from irisOS" over serial.
* Pack `iris_a.zbi` using `zbi` and boot in QEMU via Gigaboot on both `x86_64` (q35) and `aarch64` (virt).
* **Milestone:** `iris_init` boots from BOOTFS in QEMU on both architectures and prints "Hello from irisOS" to the debug serial log.

### Phase 3: Native SDK (`libiris`) & `mlibc` Integration

* Build `sdk/libiris` (MIT) providing:
  * C wrappers for Zircon handles, channels, VMOs, and ports.
  * The 24-byte `iris_msg_hdr_t` IPC wire protocol (`<iris/ipc.h>`).
  * Process spawning primitives (`posix_spawn` via `zx_process_create`, `zx_process_start`, passing `PA_HND(PA_FD, fd)`).
* Implement `sdk/mlibc-sysdeps/iris/` (`sys_anon_allocate`, `sys_vm_map`, `sys_write`, `sys_exit`, `sys_spawn`, `sys_futex`) hooking into `libzircon.so` and `libiris`.
* Build a client-side FD table in `mlibc-sysdeps` mapping file descriptors to Zircon channels.
* Cross-compile `vendor/mlibc` using Meson + Homebrew LLVM (static first, then shared `libc.so` + `ld-iris.so`).
* **Milestone:** A standard C program using `#include <stdio.h>`, `malloc()`, and `printf()` compiles with Clang and runs on irisOS.

### Phase 4: PCIe Drivers, Storage Encryption & Filesystems

* Implement ECAM PCIe bus scanning in `iris_init` / Ring 3 drivers to discover VirtIO devices across `x86_64` and `aarch64`.
* Build the Ring 3 `virtio-blk-pci` driver using Zircon BTI (`zx_bti_pin`) for DMA and `zx_interrupt_wait()`.
* Build `iris_crypt` using `vendor/mbedtls` (AES-256-XTS + Argon2):
  * Acts as a block-level IPC channel proxy above `virtio-blk-pci`.
  * Manages the 4 KB Sector 0 metadata header on `Data`.
  * Implements the development test key fallback for automated boots.
* Build `iris_fshost` incorporating:
  * GPT partition parser reading slot attributes.
  * FatFs for reading/writing `ESP`.
  * `libufs` for mounting `System_A`/`System_B` and decrypted `Data`.
* Connect `mlibc` file syscalls (`sys_open`, `sys_read`, `sys_write`) to `iris_fshost` over `iris_msg_hdr_t` channels.
* **Milestone:** irisOS boots in QEMU, reads the GPT layout, mounts `/System` read-only from `System_A`, decrypts and mounts `/Users` read-write from `Data`, and reads/writes persistent files.

### Phase 5: Root System, PTY Server & Toybox Shell

* Place early boot servers (`iris_init`, `virtio-blk-pci`, `iris_crypt`, `iris_fshost`) into BOOTFS (`iris_a.zbi`).
* Build `iris_pty` (pseudo-terminal server) and PCIe input drivers (`virtio-input-pci` / UART).
* Build `apps/sysutils/passphrase` to prompt for the `Data` partition unlock key.
* Cross-compile `vendor/toybox` against `mlibc` using `posix_spawn()` and install into `/System/Bin`.
* Implement the A/B watchdog confirmation (`boot_successful = 1` in GPT attributes) in `iris_init` once the shell prompt starts.
* **Milestone:** irisOS boots, prompts for the disk passphrase, unlocks `/Users`, marks the active A/B slot successful, and drops into an interactive Toybox `sh` prompt with persistent storage.

### Phase 6: Zero-Copy Window System (iris_compositor) & GUI

* Build `virtio-gpu-pci` driver and UEFI GOP Framebuffer fallback.
* Cross-compile `vendor/pixman` and `vendor/freetype` into `/System/Libraries` using Meson/CMake.
* Build `iris_compositor` and `libiris_gui`: clients allocate shared window VMOs, render 2D graphics/fonts locally in RAM, and send lightweight IPC damage notifications to composite at 60 Hz.
* Build the graphical terminal app running Toybox `sh` inside a desktop window.
* **Milestone:** irisOS boots into a graphical desktop with a working mouse cursor and an interactive Terminal window.

---

## 8. Documentation & Progress Tracking Policy

To maintain code quality, ease onboarding, and ensure smooth collaboration between developers and AI agents, the following documentation and tracking rules are strictly enforced:

### 8.1 Code Documentation
* **Public APIs & Headers (`sdk/`):** Every public header file in `sdk/libiris` and `sdk/libiris_gui` must have doc-comments detailing the function/struct purpose, argument definitions, return codes (`zx_status_t`), and any capability handle requirements (e.g., handles consumed, transferred, or created).
* **Servers & Drivers (`servers/`):** Every server (`iris_init`, `iris_fshost`, `iris_crypt`, `iris_compositor`, `iris_pty`) and driver in `servers/drivers/` must include high-level module documentation explaining its lifecycle, channel endpoints, and capability dependencies.
* **Complex Low-Level Logic:** Intricate operations—including IPC message packing/unpacking, direct system calls, DMA buffer pinning (`zx_bti_pin`), interrupt handling, and cryptographic key derivations—MUST include inline comments explaining *why* the implementation is designed that way.

### 8.2 Subsystem Documentation (`docs/`)
* Maintain dedicated, concise Markdown guides in `docs/`:
  * `docs/build_system.md`: Explaining the multi-toolchain build orchestrator, Homebrew LLVM setup, and target triples.
  * `docs/ipc_protocol.md`: Detailing the 24-byte `iris_msg_hdr_t` wire protocol, opcode spaces, transaction semantics, and channel lifecycle.
  * `docs/storage_and_vfs.md`: Detailing partition schemes, GPT slot management, `iris_crypt` sector headers, and VFS routing.
  * Additional subsystem guides as components are implemented.

### 8.3 Live Progress Tracking (`PROGRESS.md`)
* `PROGRESS.md` is a living document tracking the exact state of implementation across all 6 roadmap phases.
* **Immediate Update Rule:** Upon completing any verifiable step or milestone:
  1. Record what was implemented and added.
  2. Record the exact verification command(s) and test output proving it works.
  3. Clearly state the immediate next step so any agent or developer can seamlessly pick up where the last task ended.

---

## 9. Anti-Reward Hacking & Strict Verification Policy (CRITICAL)

To prevent superficial implementations, stubbed syscalls, hardcoded offsets, or falsified test passes, all development and verification MUST adhere to the following non-negotiable rules:

### 9.1 Zero Shortcuts & No Fake Plumbing
* **No Hardcoded Magic Offsets:** Servers (`iris_fshost`, `iris_crypt`, drivers) must dynamically parse on-disk structures (MBR, GPT headers, UFS2 superblocks, ELF headers, PCIe capabilities). Hardcoding sector numbers or memory offsets generated by `mkimage.sh` is strictly forbidden.
* **No RAM-Backed Persistence Fakes:** Filesystem writes to `/Users` and `/Applications` must genuinely traverse the full stack (`mlibc` $\rightarrow$ `iris_fshost` $\rightarrow$ `libufs` $\rightarrow$ `iris_crypt` $\rightarrow$ `virtio_blk`) and flush to disk. Caching writes solely in a RAM buffer to pass single-boot readback tests is prohibited.
* **No Silent Syscall Stubs:** Functions in `sdk/mlibc-sysdeps/iris/` must not silently `return 0` (success) for unimplemented functionality. If a syscall is not yet implemented, it must log an explicit warning and return `ENOSYS` so missing functionality is immediately visible.

### 9.2 Strict Test Integrity (`its_irisOS`)
* **Full Payload & EOF Verification:** Tests reading from pipes, sockets, PTYs, or files must loop `read()` until EOF (`0` bytes) and assert the **entire** expected string or byte buffer. Partial substring checks or single-read truncations are invalid.
* **End-to-End POSIX Pathing:** User-space tests must exercise standard C/POSIX library calls (`open`, `read`, `write`, `readdir`, `stat`, `posix_spawn`) rather than bypassing `mlibc` with raw handcrafted IPC messages, unless specifically testing low-level driver wire protocols.
* **Cross-Reboot Persistence Verification:** Any storage or filesystem milestone is considered incomplete unless verified across two separate QEMU boots:
  1. **Boot 1:** Create, write, and close a file on `/Users`, then cleanly flush and power off.
  2. **Boot 2 (without recreating disk images):** Boot the existing disk image and verify the file and its contents persist intact.

### 9.3 Zero-Warning Acceptance Criteria
* A build or test run is an automatic **`FAIL`**—even if the test binary exits with `0`—if the console output contains:
  * Any `mlibc warning` or unimplemented syscall error (e.g., `Failed to flush file`).
  * Any unhandled Zircon kernel exception, bad handle fault, or BTI/PMT memory leak warning on teardown.
  * Corrupted, interleaved, or vertically split terminal output.
* **Transparency Rule:** If an implementation hits a blocker, limitation, or bug, report it candidly. Never weaken a test assertion, silence an error log, or hardcode a return value to make a milestone appear complete.
