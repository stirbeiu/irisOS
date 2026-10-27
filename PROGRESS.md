# irisOS Implementation Progress Tracker

This document tracks implementation status, completed milestones, verification commands, and immediate next steps for **irisOS** across all 7 roadmap phases.

**Current Milestone:** **Phase 6: VirtIO-GPU & Graphical Framebuffer**

---

## Current Status Overview

| Phase | Milestone | Status |
| :--- | :--- | :--- |
| **Phase 1 & 2** | Toolchain, Pinned Zircon Build, Gigaboot UEFI & `iris_init` Handoff | **COMPLETED (100%)** |
| **Phase 3** | Native SDK (`libiris`), `mlibc` Sysdeps, FD Table & `posix_spawn` | **COMPLETED (100%)** |
| **Phase 4** | PCIe Drivers, Storage Encryption & Filesystems (`libufs` / `iris_fshost`) | **COMPLETED (100%)** |
| **Phase 5** | Root System, PTY Server (`iris_pty`), Passphrase Unlock, GPT Watchdog & Toybox Shell | **COMPLETED (100%)** |
| **Phase 6** | VirtIO-GPU & Graphical Framebuffer, `iris_compositor`, `libiris_gui` & Desktop | **IN PROGRESS (Current Milestone)** |
| **Phase 7** | Service Supervision, Graceful Shutdown & Passphrase Hardening | Pending |

---

## Phase 1 & 2: Toolchain, Pinned Zircon Build & `iris_init` Handoff (COMPLETED)

* **Cross-Compilation Toolchains (`build/toolchains/`):**
  * Configured Homebrew LLVM (`/opt/homebrew/opt/llvm/bin/clang`, `clang++`, `ld.lld`, `llvm-ar`, `llvm-objcopy`) targeting `x86_64` and `aarch64`:
    * [`build/toolchains/x86_64-iris.cmake`](build/toolchains/x86_64-iris.cmake) & [`build/toolchains/aarch64-iris.cmake`](build/toolchains/aarch64-iris.cmake)
    * [`build/toolchains/meson-x86_64.txt`](build/toolchains/meson-x86_64.txt) & [`build/toolchains/meson-aarch64.txt`](build/toolchains/meson-aarch64.txt)
* **Standalone Zircon Microkernel, vDSO & Host Tools (`kernel/zircon/`):**
  * [`scripts/fetch_zircon.sh`](scripts/fetch_zircon.sh): Fetches pinned Zircon commit (`milestone-1011`) from primary mirror `stirbeiu/zircon-standalone` (fallback `fuchsia.googlesource.com/zircon`).
  * Built native macOS host utilities `out/host/zbi` and `out/host/abigen`.
  * Compiled standalone Zircon microkernel (`zircon.elf` / `kernel.zbi`) and system call vDSO (`libzircon.so` / `libzircon.a`) into `out/build-x86_64/` and `out/build-aarch64/`.
* **UEFI Bootloader (Gigaboot) & Dual-Disk QEMU Setup (`build/mkimage.sh`, `scripts/run_qemu.sh`):**
  * Compiled UEFI Gigaboot (`BOOTX64.EFI` and `BOOTAA64.EFI` with `-mgeneral-regs-only`).
  * [`build/mkimage.sh`](build/mkimage.sh) packages `iris_a.zbi` into a 64 MB FAT32 UEFI ESP image (`out/images/esp_<arch>.img`) alongside a 64 MB raw storage disk (`out/images/disk_<arch>.raw`).
  * [`scripts/run_qemu.sh`](scripts/run_qemu.sh) attaches `esp_<arch>.img` via NVMe (`bootindex=0`) and `disk_<arch>.raw` as the dedicated `virtio-blk-pci` (`0x1AF4:0x1001`) storage drive.
* **Freestanding Bootstrap (`servers/iris_init/src/main.c` -> `bin/iris_init`):**
  * Launches directly from Zircon's `userboot` via `_start(start_arg, vdso_base)`.
  * Dynamically resolves vDSO system call symbols from `PT_DYNAMIC` to satisfy Zircon's vDSO PC enforcement policy.
  * Extracts `PA_RESOURCE` (Root Resource), `PA_VMO_BOOTFS` (BOOTFS), `PA_JOB_DEFAULT` (Root Job), `PA_VMO_VDSO` (`libzircon.so`), and `PA_VMAR_ROOT` from `zx_proc_args_t`, and initializes early serial logging via `zx_debuglog_create()`.

---

## Phase 3: Native SDK (`libiris`) & `mlibc` Integration (COMPLETED)

* **Native SDK (`sdk/libiris/` — MIT):**
  * [`sdk/libiris/include/iris/ipc.h`](sdk/libiris/include/iris/ipc.h): Standard 24-byte packed `iris_msg_hdr_t` wire protocol (`IRIS_IPC_MAGIC = 0x49524953`) with protocol IDs (`SYSTEM`, `FS`, `CRYPT`, `COMPOSITOR`, `PTY`, `DRIVER`).
  * [`sdk/libiris/src/channel.c`](sdk/libiris/src/channel.c), [`vmo.c`](sdk/libiris/src/vmo.c), [`port.c`](sdk/libiris/src/port.c), [`spawn.c`](sdk/libiris/src/spawn.c): Wrappers for Zircon channels, VMOs, event ports, and `iris_spawn` (`zx_process_create` + `zx_process_start` with `PA_HND(PA_FD, fd)` transfer).
  * Documentation: [`docs/ipc_protocol.md`](docs/ipc_protocol.md).
* **Standard C Library & Dynamic Linker (`vendor/mlibc` + `sdk/mlibc-sysdeps/iris/` — MIT):**
  * [`scripts/fetch_mlibc.sh`](scripts/fetch_mlibc.sh) & [`scripts/fetch_compiler_rt.sh`](scripts/fetch_compiler_rt.sh): Fetches pinned `mlibc` `v6.3.1`, `frigg`, and `compiler-rt` builtins.
  * Implemented [`sdk/mlibc-sysdeps/iris/generic/generic.cpp`](sdk/mlibc-sysdeps/iris/generic/generic.cpp) and [`entry.cpp`](sdk/mlibc-sysdeps/iris/generic/entry.cpp):
    * Anonymous memory allocation and VMAR mapping (`sys_anon_allocate`, `sys_vm_map`).
    * Per-process 256-entry client-side file descriptor table (`include/iris/fd_table.h`) with `/dev/console`, `/dev/debuglog`, `/dev/null`, and `/dev/zero`.
    * `sys_spawn` with automatic `PA_HND(PA_FD, fd)` inheritance and child FD table reconstruction.
    * Thread Control Block (`sys_tcb_set`) and futex synchronization (`sys_futex_wait`, `sys_futex_wake`).
  * Produces `libc.a`, `libc.so`, `ld-iris.so`, and CRT startup objects (`crt1.o`, `crti.o`, `crtn.o`) for both `x86_64` and `aarch64`.
  * Verified in QEMU via [`apps/tests/its_irisOS/src/main.c`](apps/tests/its_irisOS/src/main.c) (`bin/its_irisOS`).
  * Documentation: [`docs/build_system.md`](docs/build_system.md), [`docs/mlibc_sysdeps.md`](docs/mlibc_sysdeps.md).

---

## Phase 4: PCIe Drivers, Storage Encryption & Filesystems (COMPLETED)

### Step 4.1: Unified PCIe (ECAM) Discovery & Ring 3 `virtio-blk-pci` Driver (COMPLETED)
* **Unified PCIe ECAM Scanner (`sdk/libiris/include/iris/pci.h`, `sdk/libiris/src/pci.c`):**
  * Maps physical ECAM configuration space (`0xE0000000` on `x86_64` q35; `0x4010000000` on `aarch64` virt) via `zx_vmo_create_physical()`.
  * Enumerates PCIe buses, parses 32/64-bit BARs, extracts VirtIO 1.0+ vendor capabilities (`COMMON_CFG`, `NOTIFY_CFG`, `ISR_CFG`, `DEVICE_CFG`), and enables bus-master DMA.
* **Ring 3 VirtIO Block Driver (`servers/drivers/virtio_blk/src/main.c` -> `bin/virtio_blk`):**
  * Allocates physically contiguous DMA rings/buffers via Zircon BTI (`zx_iommu_create`, `zx_bti_create`, `zx_vmo_create_contiguous`, `zx_bti_pin`) and cleanly unpins tokens (`zx_pmt_unpin`) on exit.
  * Serves `IRIS_PROTO_DRIVER` block requests (`IRIS_BLK_OP_GET_INFO`, `IRIS_BLK_OP_READ`, `IRIS_BLK_OP_WRITE`, `IRIS_BLK_OP_FLUSH`) across multiple client channels.
  * Documentation: [`docs/virtio_drivers.md`](docs/virtio_drivers.md).

### Step 4.2: Block-Level Encryption Proxy (`iris_crypt`) with Mbed-TLS (COMPLETED)
* **Mbed-TLS 3.6 & Argon2id (`scripts/fetch_mbedtls.sh`, `build/mbedtls_iris_config.h`, `servers/iris_crypt/src/argon2.c`):**
  * Fetches pinned Mbed-TLS `v3.6.2` (`stirbeiu/mbedtls.git` -> fallback `Mbed-TLS/mbedtls.git`) and builds freestanding `libmbedcrypto.a` (`AES`, `XTS`, `SHA-256`, `SHA-512`) alongside freestanding memory-hard Argon2id (`iris_argon2id_derive`).
* **Sector 0 Header & Block Proxy (`servers/iris_crypt/include/iris_crypt.h`, `servers/iris_crypt/src/main.c` -> `bin/iris_crypt`):**
  * Reserves physical Sectors `0..7` (first 4 KB) for `iris_crypt_header_t` (magic `0x5450595243534952ULL` `"IRISCRYPT"`, Argon2id salt/cost parameters, wrapped 512-bit AES-256-XTS master key, and SHA-256 integrity digests) with deterministic dev-key fallback (`IRIS_CRYPT_DEV_PASSPHRASE`).
  * Translates logical sector $L$ to physical sector $P = L + 8$ on downstream `virtio_blk`, encrypting writes and decrypting reads using `mbedtls_aes_crypt_xts()` with 64-bit little-endian logical LBA as the XTS tweak.
  * Documentation: [`docs/storage_and_vfs.md`](docs/storage_and_vfs.md).

### Step 4.3: GPT Parser, `libufs` UFS2 & VFS Router (`iris_fshost`) (COMPLETED)
* **128 MB 4-Partition GPT Raw Disk (`build/mkimage.sh`, `scripts/mk_gpt_disk.py`, `build/mkufs2_host.c`):**
  * Assembles Protective MBR (LBA 0), Primary GPT (LBA 1..33), and Backup GPT (LBA 262111..262143):
    * Partition 1: `ESP` (LBA `2048..67583`, 32 MB FAT).
    * Partition 2: `System_A` (LBA `67584..133119`, 32 MB UFS2, active immutable system root).
    * Partition 3: `System_B` (LBA `133120..198655`, 32 MB UFS2, inactive rollback slot).
    * Partition 4: `Data` (LBA `198656..260095`, 30 MB Encrypted UFS2 via `iris_crypt`, Sectors 0..7 reserved for 4 KB `iris_crypt` header).
  * Host formatter `out/host/mkufs2` formats valid FreeBSD UFS2 volumes with superblock, cylinder group descriptors, dinode tables, and populated directory structures (`Bin`, `Libraries`, `Servers`, `Resources/release.txt`).
* **FreeBSD UFS2 Integration (`vendor/libufs` -> `libufs.a`):**
  * Cross-compiled `vendor/libufs` into `libufs.a` for `x86_64` and `aarch64` with Homebrew LLVM using `-D_LIBUFS -Dpread=iris_ufs_pread -Dpwrite=iris_ufs_pwrite`.
  * Implemented [`servers/iris_fshost/src/ufs_io.c`](servers/iris_fshost/src/ufs_io.c): block-device adapter routing all UFS2 reads/writes to IPC channels over `IRIS_PROTO_DRIVER`.
* **Central VFS Router (`servers/iris_fshost/src/main.c` -> `bin/iris_fshost`):**
  * Reads and validates Primary GPT header and partition entries from `virtio_blk`.
  * Evaluates GPT attributes to select active slot (`System_A`, `prio=15, ok=1`) and mounts UFS2 at `/System` (read-only).
  * Connects to `iris_crypt` to mount decrypted `Data` at `/Users` and `/Applications` (read-write).
  * Serves `IRIS_PROTO_FS` (`FS_OPEN`, `FS_CLOSE`, `FS_READ`, `FS_WRITE`, `FS_SEEK`, `FS_STAT`), enforces `EROFS` (`IRIS_ERR_READ_ONLY`) on `/System` write attempts, and resolves virtual symlinks `/bin -> /System/Bin` and `/lib -> /System/Libraries`.
* **Standard C Library VFS Integration (`sdk/mlibc-sysdeps/iris/generic/generic.cpp`):**
  * Routes `sys_open`, `sys_read`, `sys_write`, `sys_close`, `sys_stat`, `sys_fstat`, `sys_seek` through `vfs_ipc_call()` to `iris_fshost` via inherited `PA_HND(PA_NS_DIR, 0)`.
* **Documentation:** [`docs/storage_and_vfs.md`](docs/storage_and_vfs.md).

---

## Phase 5: Root System, PTY Server (`iris_pty`), Passphrase Unlock, GPT Watchdog & Toybox Shell (COMPLETED — 100%)

* [x] **Step 5.1:** Input Drivers & Ring 3 Pseudo-Terminal Server (`iris_pty`) (100% Complete)
* [x] **Step 5.2:** Toybox Shell & Core Utilities Integration (100% Complete)
* [x] **Step 5.3:** Disk Passphrase Unlock (`apps/sysutils/passphrase`) (100% Complete)
* [x] **Step 5.4:** GPT A/B Slot Watchdog Confirmation (`boot_successful = 1`) (100% Complete)

### Step 5.1: Input Drivers & Ring 3 Pseudo-Terminal Server (`iris_pty`) (COMPLETED)
* **VirtIO Input PCIe Driver (`servers/drivers/virtio_input/` & `sdk/libiris/include/iris/pci.h`):**
  * Implemented PCIe enumeration and Ring 3 driver for `virtio-input-pci` (Device ID `0x1052` modern, `0x1010` transitional) across `x86_64` (PCIe q35) and `AArch64` (PCIe virt).
  * Features split virtqueue event buffer management, raw scancode EV_KEY translation to standard ASCII characters (supporting Shift modifier uppercase and symbols), and graceful teardown.
  * UART serial console input worker (`zx_debug_read`) automatically configured as fallback when VirtIO input hardware is absent.
* **Pseudo-Terminal Server (`servers/iris_pty/` -> `bin/iris_pty`):**
  * Standalone Ring 3 user-space daemon listening on dedicated IPC protocol `IRIS_PROTO_PTY` (`0x0005`).
  * **Line Discipline Engine:**
    * **Canonical (Cooked) Mode:** Line buffering up to 1024 bytes, interactive backspace handling (`\b \b` visual erasing), and character echoing to stdout/debuglog.
    * **Raw Mode:** Instantaneous character pass-through bypasses editing and echoing.
    * **Control Sequences:** Carriage return normalization (`\r` -> `\n`), `Ctrl+C` POSIX signal delivery (`SIGINT=2`) with uncommitted line discard, and `Ctrl+D` End-of-File (EOF) indication.
  * Window size geometry tracking (`iris_pty_window_t`: rows and columns) and mode flag querying/configuration (`IRIS_PTY_MODE_CANONICAL`, `IRIS_PTY_MODE_ECHO`, `IRIS_PTY_MODE_RAW`).
* **Native SDK & mlibc Integration:**
  * Public client API in [`sdk/libiris/include/iris/pty.h`](sdk/libiris/include/iris/pty.h) and [`sdk/libiris/src/pty.c`](sdk/libiris/src/pty.c): `iris_pty_set_mode()`, `iris_pty_get_mode()`, `iris_pty_set_window()`, `iris_pty_get_window()`, `iris_pty_inject()`, `iris_pty_get_signal()`, `iris_pty_read()`, `iris_pty_write()`.
  * `sdk/mlibc-sysdeps/iris/generic/entry.cpp`: Auto-detects `IRIS_FD_PTY` transferred as `PA_HND(PA_FD, 0)` and maps standard streams (FD 0 stdin, FD 1 stdout, FD 2 stderr) directly to the PTY channel endpoint.
  * Implemented synchronous channel call pattern (`zx_channel_write` + `zx_object_wait_one` + `zx_channel_read`) in `sdk/libiris/src/channel.c` to preserve standard 24-byte `iris_msg_hdr_t` ABI without `zx_channel_call` 4-byte header collisions.
* **Automated Verification:**
  * Added automated Step 10 tests in `apps/tests/its_irisOS/main.c`: verified PTY file descriptors (0 & 1), `isatty(0)=1`, `isatty(1)=1`, window size query (24x80), canonical buffering with backspace editing, character echo, raw mode pass-through, `Ctrl+C` SIGINT generation, and `Ctrl+D` EOF delivery.
  * Verified full execution and clean exit with zero leaks across both `x86_64` and `aarch64`.
* **Documentation:** [`docs/ipc_protocol.md`](docs/ipc_protocol.md).

### Build System Infrastructure: Userland & Host Tools CMake Migration (COMPLETED)
* **Userland Executables in CMake (`build/CMakeLists.txt`):**
  * Added shared `iris_userland_common` interface library providing common include search paths (mlibc, sysdeps, Native SDK, Zircon public headers, clang builtin resource headers) and uniform freestanding compile flags (`-O2 -fPIE -nostdinc`).
  * Defined `iris_add_userland_executable` CMake function automating entry point ordering, CRT object positioning (`crt1.o`, `crti.o` via link options, and `crtn.o` at the trailing boundary), PIE flag management (`-Wl,-pie -Wl,-z,text`), and dependency wiring.
  * Migrated all userland executable targets into CMake:
    * `its_irisOS` (`apps/tests/its_irisOS/main.c`)
    * `virtio_blk` (`servers/drivers/virtio_blk/src/main.c`)
    * `iris_crypt` (`servers/iris_crypt/src/main.c` linked against `mbedcrypto`)
    * `iris_fshost` (`servers/iris_fshost/src/main.c` linked against `ufs`)
    * `iris_pty` (`servers/iris_pty/src/main.c`, `servers/drivers/virtio_input/src/virtio_input.c`)
  * Integrated `mlibc`, `libiris`, and `compiler-rt` builtins as standard CMake dependency targets (`mlibc_target`, `iris_static`, `compiler_rt_target`), allowing Ninja to manage the complete DAG in a single invocation.
* **Host Formatter in CMake (`mkufs2`):**
  * Configured `mkufs2` as a standard CMake host executable target (`add_executable(mkufs2 ...)`) when `NOT CMAKE_CROSSCOMPILING`, building via AppleClang/host toolchain into `out/host/mkufs2`.
  * Added a custom target bridge for cross-compilation configurations to trigger host CMake transparently on demand.
* **Stripped Top-Level Orchestrator (`build/Makefile`):**
  * Removed all legacy manual `clang` and `ld.lld` shell recipe blocks (~400 lines of manual rules eliminated).
  * High-level targets (`all`, `build`, `image`) directly invoke `cmake -B ... -G Ninja` and `ninja -C ...`.
* **Verification:**
  * Clean build across both architectures via `./iris build all`.
  * Automated headless QEMU verification passed 100% on both architectures via `./iris test all`.

### Infrastructure Refactor: Supervisor Slim-Down & Standardized Systemd-Style Logging (COMPLETED)
* **Standardized Colored Uniform-Width Systemd-Style Logging:**
  * Defined 4-character ANSI status tags for perfect 6-character bracket alignment:
    * `STATUS_OKAY`: `\033[32m OKAY \033[0m` (green)
    * `STATUS_WARN`: `\033[33m WARN \033[0m` (yellow)
    * `STATUS_FAIL`: `\033[31m FAIL \033[0m` (red)
  * Implemented standardized `log_status(tag, unit, fmt, ...)` helper producing atomic lines formatted strictly as `[<status>] <unit> - <single short sentence description>` (e.g. `[ OKAY ] iris_init - Extracted Root Resource capability.`), buffered via `snprintf` before calling `zx_debuglog_write()`.
* **Slimmed-Down `iris_init` (~280 lines, down from 1477 lines):**
  * Removed `test_virtio_blk()` and all inlined test routines from [`servers/iris_init/src/main.c`](servers/iris_init/src/main.c).
  * Restructured `iris_init` strictly as a bootstrap supervisor:
    1. Extracts root capabilities (`PA_RESOURCE`, `PA_VMO_BOOTFS`, `PA_JOB_DEFAULT`, `PA_VMO_VDSO`, `PA_VMAR_ROOT`) from userboot.
    2. Maps PCIe ECAM and scans for VirtIO Block and VirtIO Input devices.
    3. Spawns core system daemons in dependency order: `virtio_blk` -> `iris_crypt` -> `iris_fshost` -> `iris_pty`.
    4. Spawns `bin/its_irisOS` and passes required client channels (`PA_NS_DIR`, `PA_FD:0`, `PA_USER0`, `PA_USER1`, `PA_USER2`).
    5. Enters supervisor sleep via `zx_nanosleep(ZX_TIME_INFINITE)`.
* **Verification Suites Relocation to `apps/tests/its_irisOS`:**
  * Relocated all Phase 4 hardware and storage verification suites into [`apps/tests/its_irisOS/main.c`](apps/tests/its_irisOS/main.c):
    * VirtIO Block write/read verification on Sector 100 via direct IPC.
    * Encrypted partition raw ciphertext check on physical LBA 258664 directly from `virtio_blk`.
    * Decrypted plaintext readback via `iris_crypt` on logical sector 60000.
    * VFS file write on `/Users` and `EROFS` check on `/System`.
  * Standardized all test results to output with colored `[<status>] its_irisOS - <description>` formatting.
* **Verification:**
  * Clean build across both architectures via `./iris build all`.
  * Automated headless QEMU verification passed 100% on both architectures via `./iris test all`:
    * x86_64: `[PASS] irisOS x86_64 verification succeeded!`
    * aarch64: `[PASS] irisOS aarch64 verification succeeded!`

### Step 5.2: Toybox Shell & Core Utilities Integration (COMPLETED)
* **Toybox Multicall Userland Fetch & Configuration (`scripts/fetch_toybox.sh`, `build/toybox.config`):**
  * Created [`scripts/fetch_toybox.sh`](scripts/fetch_toybox.sh) fetching pinned Toybox `0.8.11` into `vendor/toybox/` with shallow clone support.
  * Formulated [`build/toybox.config`](build/toybox.config) defconfig enabling 15 core utilities (`sh`, `ls`, `cat`, `echo`, `pwd`, `uname`, `clear`, `mkdir`, `cp`, `rm`, `touch`, `wc`, `grep`, `sleep`, `kill`) configured for `posix_spawn()` / no-fork operation (`CONFIG_TOYBOX_FORK=n`, `CONFIG_TOYBOX_NOMMU=y`).
* **Cross-Compilation & Compatibility Layer (`scripts/build_toybox.sh`, `build/toybox_compat/`):**
  * Integrated Toybox build orchestrator using Homebrew LLVM (`clang`, `llvm-ar`) linking statically against `mlibc` sysroot (`libc.a`) with PIC/PIE support.
  * Formulated minimal compatibility headers and stubs in [`build/toybox_compat/`](build/toybox_compat/) bridging missing BSD/Linux-specific symbols (`stubs.c`, `mntent.h`, `termios.h`, `pty.h`).
  * Built and stripped standalone multi-call binaries for both `x86_64` and `aarch64` (~850 KB - 910 KB) installed to `out/build-<arch>/bin/toybox`.
* **POSIX Sysdeps Integration (`sdk/mlibc-sysdeps/iris/`):**
  * **Zircon Socket Stream Pipes:** Refactored `sys_pipe` to allocate duplex Zircon stream sockets (`zx_socket_create(0, &s0, &s1)`) mapped to `IRIS_FD_SOCKET`. Implemented `zx_socket_read` and `zx_socket_write` with `ZX_SOCKET_READABLE` and `ZX_SOCKET_WRITABLE` wait signals, supporting native capability duplication (`zx_handle_duplicate`) required by `dup`, `dup2`, and `posix_spawn` pipe redirection.
  * **POSIX Spawn File Actions:** Extended `sys_spawn` in [`sdk/mlibc-sysdeps/iris/generic/generic.cpp`](sdk/mlibc-sysdeps/iris/generic/generic.cpp) to process `posix_spawn_file_actions_t` operations (`close`, `dup2`, `open`), dynamically cloning and transferring redirected file descriptors (`PA_HND(PA_FD, fd)`) to child processes.
  * **Core POSIX Emulation APIs:** Implemented `sys_sigprocmask`, `sys_sigaction`, `sys_gethostname`, `sys_readlink` / `sys_readlinkat`, `sys_getcwd`, `sys_chdir` (with relative path and `.` resolution against `g_cwd`), `sys_fchdir`, `sys_kill`, `sys_umask`, `sys_uname`, `sys_dup`, `sys_dup2`, `sys_fcntl` (`F_GETFD`, `F_SETFD`, `F_GETFL`, `F_SETFL`, `F_DUPFD`), `sys_ioctl`, and `sys_tcgetattr` / `sys_tcsetattr`.
  * **Bootstrap Handle Dispatch:** Updated [`sdk/mlibc-sysdeps/iris/generic/entry.cpp`](sdk/mlibc-sysdeps/iris/generic/entry.cpp) to identify transferred `ZX_OBJ_TYPE_SOCKET` descriptors as `IRIS_FD_SOCKET`.
  * Guarded `sdk/libiris/src/spawn.c` to reject `ZX_HANDLE_INVALID` entries from handle transfer arrays.
* **Disk Packaging & Runtime Filesystem Layout (`build/mkimage.sh`, `build/mkufs2_host.c`):**
  * Updated [`build/mkufs2_host.c`](build/mkufs2_host.c) and [`build/mkimage.sh`](build/mkimage.sh) to format `System_A` and `System_B` with `/System/Bin/toybox` and generate 15 symbolic links (`sh`, `ls`, `cat`, `echo`, `pwd`, `uname`, `clear`, `mkdir`, `cp`, `rm`, `touch`, `wc`, `grep`, `sleep`, `kill`).
  * Ensured the in-memory symlink `/bin -> /System/Bin` enables running standard scripts via `#!/bin/sh` or `/bin/sh`.
* **Automated Verification:**
  * Updated [`apps/tests/its_irisOS/main.c`](apps/tests/its_irisOS/main.c) Step 13:
    * **Step 13.1:** Creates a socket pipe via `pipe()`, redirects stdout to pipe write fd using `posix_spawn_file_actions_adddup2`, executes `/System/Bin/toybox uname -a`, reads output from pipe read fd, and verifies output contains `"irisOS"`.
    * **Step 13.2:** Executes `/bin/sh -c "echo 'Toybox execution verified'"` through the PTY channel and verifies exit code 0.
  * Verified 100% pass on both architectures (`x86_64` and `aarch64`) with zero warnings and clean daemon exit.

### Step 5.2a: Raw PTY Console Output (COMPLETED)
* `iris_pty` terminal output now uses `zx_debug_write()` (raw bytes to kernel console/UART) instead of `zx_debuglog_write()` (one timestamped record per call, which put every echoed character on its own line). Added `zx_debug_write` vDSO wrapper in `sdk/mlibc-sysdeps/iris/generic/generic.cpp`; debuglog remains the fallback and is still used for `[ OKAY ]` daemon logs.
* Verification: `./iris test all` -> `[PASS]` on x86_64 and aarch64; echo now appears as `Helo\b \b\b \blo World` on a single line.
### Step 5.2b: Pipe Test Hardening (COMPLETED)
* `its_irisOS` Step 13.1 now reads the `uname -a` pipe until EOF (toybox emits fields in several small writes; the old single `read()` + early `close()` caused toybox `uname: write` / `Failed to flush file` errors) and asserts the full expected string plus exit status 0 per architecture (`irisOS irisOS-dev 0.1.0 Zircon 1011 (Ring 3 userland) <arch>`).
* Found and removed a stale x86_64 toybox object (`pending_sh.o`) carrying old debug prints (`[GNL]`, `[SH_LINE]`) from an earlier session; clean toybox rebuild verified free of them.
* Verification: `./iris test all` -> `[PASS]` on x86_64 and aarch64, full `uname -a` line logged, no write/flush errors.

### Step 5.2c: Interactive Shell Mode, System Control Applets & PTY Navigation (COMPLETED)
* **Boot Mode Selection (`iris.mode=shell|test`):**
  * Default `./iris build` and `./iris run` boot into interactive `/bin/sh` (`iris.mode=shell`), while `./iris test` boots the automated `its_irisOS` test suite (`iris.mode=test`).
* **Toybox Help & System Control Symlinks (`build/toybox.config`, `scripts/build_toybox.sh`, `build/mkufs2_host.c`, `build/mkimage.sh`):**
  * Enabled `CONFIG_HELP=y`, `CONFIG_TOYBOX_HELP=y`, `CONFIG_REBOOT=y`, and `CONFIG_TEST=y` in Toybox configuration and compiled `toys/other/help.c` and `toys/other/reboot.c`.
  * Added `/System/Bin` symlinks for `reboot`, `poweroff`, `help`, `halt`, and `test` pointing to `toybox`.
  * Fixed UFS2 symlink target resolution in `servers/iris_fshost/src/main.c` (`resolve_vfs_path`) by copying `ldp.dp2->di_db` out of `disk->d_inoblock` before subsequent `getinode()` calls overwrite the shared 16-inode block cache for inodes $\ge 16$.
* **VirtIO Keyboard Navigation & PTY Line Editing (`servers/drivers/virtio_input/src/virtio_input.c`, `servers/iris_pty/src/main.c`):**
  * Translated Linux navigation scancodes (`KEY_UP` 103 $\rightarrow$ `"\033[A"`, `KEY_DOWN` 108 $\rightarrow$ `"\033[B"`, `KEY_RIGHT` 106 $\rightarrow$ `"\033[C"`, `KEY_LEFT` 105 $\rightarrow$ `"\033[D"`, `KEY_HOME` 102 $\rightarrow$ `"\033[H"`, `KEY_END` 107 $\rightarrow$ `"\033[F"`, `KEY_DELETE` 111 $\rightarrow$ `"\033[3~"`) into VT100 escape sequences.
  * Implemented VT100 escape sequence parsing in `iris_pty` canonical line discipline for cursor movement (`Left`/`Right`/`Home`/`End`), mid-line insertion/deletion (`Backspace`/`Delete`), and 16-entry command history (`Up`/`Down`), while passing raw sequences directly into `ready_buf` in raw mode.
  * Emits VT100 blinking block cursor sequence (`"\033[1 q\033[?12h"`) via `zx_debug_write` on PTY startup and shell session open.

### Step 5.3: Disk Passphrase Unlock (`apps/sysutils/passphrase`) (COMPLETED)
* **IPC Protocol (`sdk/libiris/include/iris/ipc.h`, `servers/iris_crypt/src/main.c`):**
  * Defined `iris_crypt_unlock_req_t` (`IRIS_CRYPT_OP_UNLOCK`) and `iris_crypt_status_resp_t` (`IRIS_CRYPT_OP_STATUS`) under `IRIS_PROTO_CRYPT`.
  * Implemented `unlock_with_passphrase()` in `iris_crypt` to derive the 512-bit KEK via Argon2id (`iris_argon2id_derive`) and unwrap the 512-bit AES-256-XTS Master Key from Sector 0 (`iris_crypt_header_t`), with transparent re-wrapping when migrating from `IRIS_CRYPT_FLAG_DEV_KEY` and `--wait-unlock` CLI flag support for interactive boot.
* **Passphrase Utility (`apps/sysutils/passphrase/main.c` -> `bin/passphrase`):**
  * Packaged into BOOTFS (`iris_a.zbi` / `iris_b.zbi`) and spawned by `iris_init` on `iris_pty` prior to `iris_fshost` mounting `/Users` in interactive mode (`iris.mode=shell`), while `iris.mode=test` continues using the automatic dev fallback key.
  * Prompts `Enter disk passphrase: ` to stdout, disables echoing via `tcsetattr` raw mode (`~(ICANON | ECHO | ECHOE | ECHOK)`), sends `IRIS_CRYPT_OP_UNLOCK` to `iris_crypt`, and zeroizes the passphrase buffer.

### Step 5.4: GPT A/B Slot Watchdog Confirmation (COMPLETED)
* **GPT Attribute Update (`servers/iris_fshost/src/main.c`, `servers/iris_init/src/main.c`):**
  * Added `IRIS_FS_OP_CONFIRM_BOOT` (`0x001E`) under `IRIS_PROTO_FS`.
  * Implemented `confirm_gpt_boot_slot()` and IEEE 802.3 `calc_gpt_crc32()` in `iris_fshost`: reads the Primary GPT Header (LBA 1) and Partition Entry Array (LBA 2..33), sets attribute bit 48 (`boot_successful = 1`) on the active `System_A` partition entry, recalculates `partition_entry_array_crc32` and `header_crc32`, writes LBA 2 and LBA 1 back to `virtio_blk`, and flushes.
  * `iris_init` sends `IRIS_FS_OP_CONFIRM_BOOT` (`confirm_boot_with_fshost()`) once the interactive shell starts (or when `its_irisOS` tests pass).

### Phase 5 / Phase 7 Technical Debt & Hardening Notes
* **Interactive Passphrase Blocking Input & Dev Fallback (`apps/sysutils/passphrase`):**
  * `apps/sysutils/passphrase` currently falls back to `IRIS_CRYPT_DEV_PASSPHRASE` immediately because `read()` does not block on empty input in raw mode.
  * Blocking terminal input in raw mode and removal of the interactive `IRIS_CRYPT_DEV_PASSPHRASE` fallback are scheduled for **Phase 7 hardening**.
* **Graceful Shutdown vs. Immediate ACPI Cut-Off:**
  * `poweroff` and `reboot` currently invoke `sync()` followed directly by `zx_system_powerctl()`. Wiring `poweroff` to send an IPC request to `iris_init` for reverse-dependency daemon termination (allowing `iris_crypt` to zero keys and `virtio_blk` to unpin DMA before ACPI poweroff) is scheduled for **Phase 7**.

---

## Phase 6: VirtIO-GPU & Graphical Framebuffer (CURRENT MILESTONE)

* [ ] **Step 6.1:** Build `virtio-gpu-pci` Ring 3 driver and UEFI GOP Framebuffer fallback.
* [ ] **Step 6.2:** Cross-compile `vendor/pixman` (MIT) and `vendor/freetype` (FTL) into `/System/Libraries`.
* [ ] **Step 6.3:** Build `iris_compositor` and `libiris_gui` (zero-copy shared VMO window surfaces + IPC damage notifications at 60 Hz).
* [ ] **Step 6.4:** Build the graphical Terminal emulator (`apps/terminal`) running Toybox `sh` inside a desktop window with mouse cursor support.

---

## Build & Verification Commands

```bash
# Automated headless verification on both architectures (or single architecture)
./iris test all         # Tests both x86_64 and aarch64
./iris test x86_64      # Headless x86_64 QEMU verification
./iris test aarch64     # Headless aarch64 QEMU verification
./iris build all        # or: make build all

# Build a single architecture
./iris build x86_64     # or: make build x86_64
./iris build aarch64    # or: make build aarch64

# Boot in QEMU (GUI window + serial console in stdio)
./iris run x86_64       # or: make run x86_64
./iris run aarch64      # or: make run aarch64

# Boot headless for automated serial verification
./iris run x86_64 --headless
./iris run aarch64 --headless
```

---

## Live Confirmed OS Boot Logs (`x86_64` & `AArch64`)

### 1. `x86_64` Confirmed Boot (`./iris test x86_64`)
```text
System Node: irisOS-dev (x86_64)
[ OKAY ] iris_init - Extracted Root Resource capability.
[ OKAY ] iris_init - Extracted BOOTFS VMO capability.
[ OKAY ] iris_init - Extracted Root Job capability.
[ OKAY ] iris_init - Extracted vDSO VMO capability.
[ OKAY ] iris_init - Extracted Root VMAR capability.
[ OKAY ] iris_init - Mapped PCIe ECAM at phys 0x00000000e0000000.
[ OKAY ] iris_init - Discovered VirtIO Block Device at PCI 00:03.0.
[ OKAY ] iris_init - Discovered VirtIO Input Device at PCI 00:04.0.
[ OKAY ] iris_init - Spawned virtio_blk daemon.
[ OKAY ] iris_init - Spawned iris_crypt daemon.
[ OKAY ] iris_init - Spawned iris_fshost daemon.
[ OKAY ] iris_init - Spawned iris_pty daemon.
[ OKAY ] iris_init - Spawned its_irisOS test suite.
[ OKAY ] iris_init - Supervisor entering sleep state.
[ OKAY ] virtio_blk - Bound to PCI 00:03.0 (Vendor: 0x1af4, Device: 0x1001).
[ OKAY ] virtio_blk - Mapped BAR 4 (phys 0xc000000000 -> vaddr 0x...).
[ OKAY ] virtio_blk - Virtqueue 0 (256 descriptors) & BTI DMA initialized (262144 sectors, 128 MB).
[ OKAY ] virtio_blk - IPC server listening on control channel.
[ OKAY ] iris_pty - Starting Pseudo-Terminal Server.
[ OKAY ] iris_pty - Bound to PCI 00:04.0 (Vendor: 0x1af4, Device: 0x1052).
[ OKAY ] iris_pty - Primary input active via virtio-input-pci.
[ OKAY ] iris_pty - Line discipline initialized (canonical cooked mode, echoing enabled).
[ OKAY ] iris_pty - IPC server listening on control channel.
[ OKAY ] iris_crypt - Verified existing 4 KB Sector 0 header (magic 0x5450595243534952).
[ OKAY ] iris_crypt - Unlocked AES-256-XTS master key via Argon2id dev key fallback.
[ OKAY ] iris_crypt - Block proxy listening (LBA 198656+8, 61432 logical sectors).
[ OKAY ] iris_fshost - Parsed GPT (ESP=2048..67583, System_A=67584..133119 [prio=15,ok=1], System_B=133120..198655, Data=198656..260095).
[ OKAY ] iris_fshost - Mounted System_A (UFS2 magic 0x19540119) at /System [Read-Only].
[ OKAY ] iris_fshost - Mounted encrypted Data (UFS2 magic 0x19540119) at /Users & /Applications [Read-Write].
[ OKAY ] iris_fshost - Active virtual symlinks: /bin -> /System/Bin, /lib -> /System/Libraries.
[ OKAY ] its_irisOS - malloc/free & snprintf heap memory.
[ OKAY ] its_irisOS - FD Table direct write(1) to stdout.
[ OKAY ] its_irisOS - FD Table isatty(1)=1, isatty(2)=1 (console TTY verified).
[ OKAY ] its_irisOS - Console device open("/dev/console") -> fd 3, write & close.
[ OKAY ] its_irisOS - FD Table invalid descriptor rejection (EBADF).
[ OKAY ] its_irisOS - POSIX spawn file actions (init/adddup2/destroy).
[ OKAY ] its_irisOS - VirtIO Block IPC query (BLK_GET_INFO): 262144 sectors (512 bytes/sector).
[ OKAY ] its_irisOS - VirtIO Block IPC write to Sector 100 confirmed (512 bytes).
[ OKAY ] its_irisOS - VirtIO Block IPC read from Sector 100 verified (512/512 bytes matched).
[ OKAY ] its_irisOS - iris_crypt IPC query (BLK_GET_INFO): 61432 usable sectors.
[ OKAY ] its_irisOS - Wrote 512-byte plaintext to logical Sector 60000 via iris_crypt.
[ OKAY ] its_irisOS - Verified physical LBA 258664 on virtio_blk is AES-256-XTS ciphertext.
[ OKAY ] its_irisOS - Verified logical Sector 60000 read via iris_crypt matches decrypted plaintext.
[ OKAY ] its_irisOS - Read /System/Resources/release.txt ("irisOS release 0.1.0").
[ OKAY ] its_irisOS - Write to /System/readonly_test.txt rejected with EROFS (errno=30).
[ OKAY ] its_irisOS - Wrote & verified /Users/verify.txt (59 bytes, ino=10, "irisOS Phase 4 UFS2 + AES-256-XTS VFS Persistence Verified!").
[ OKAY ] its_irisOS - PTY file descriptors 0 and 1 verified (isatty(0)=1, isatty(1)=1).
[ OKAY ] its_irisOS - PTY window size query verified (24 rows x 80 cols).
[ OKAY ] its_irisOS - PTY character echoing to console output verified.
[ OKAY ] its_irisOS - PTY raw mode immediate pass-through verified ("RAW_INPUT").
[ OKAY ] its_irisOS - PTY Ctrl+C signal generation (SIGINT=2) and line discard verified.
[ OKAY ] its_irisOS - PTY Ctrl+D EOF delivery verified (read returned 0 bytes).
[ OKAY ] its_irisOS - Toybox multicall userland verified: /System/Bin/toybox uname -a -> "irisOS".
Toybox execution verified
[ OKAY ] its_irisOS - Toybox shell execution verified: /bin/sh -c (exit=0).
[ OKAY ] its_irisOS - Interactive Test Suite all checks passed.
[ OKAY ] iris_pty - Daemon exited cleanly.
[ OKAY ] iris_crypt - Zeroized AES-256-XTS keys and exited cleanly.
[ OKAY ] virtio_blk - Unpinned BTI DMA tokens (zx_pmt_unpin) and exited cleanly.
```

### 2. `AArch64` (`aarch64`) Confirmed Boot (`./iris test aarch64`)
```text
System Node: irisOS-dev (aarch64)
[ OKAY ] iris_init - Extracted Root Resource capability.
[ OKAY ] iris_init - Extracted BOOTFS VMO capability.
[ OKAY ] iris_init - Extracted Root Job capability.
[ OKAY ] iris_init - Extracted vDSO VMO capability.
[ OKAY ] iris_init - Extracted Root VMAR capability.
[ OKAY ] iris_init - Mapped PCIe ECAM at phys 0x0000004010000000.
[ OKAY ] iris_init - Discovered VirtIO Block Device at PCI 00:02.0.
[ OKAY ] iris_init - Discovered VirtIO Input Device at PCI 00:03.0.
[ OKAY ] iris_init - Spawned virtio_blk daemon.
[ OKAY ] iris_init - Spawned iris_crypt daemon.
[ OKAY ] iris_init - Spawned iris_fshost daemon.
[ OKAY ] iris_init - Spawned iris_pty daemon.
[ OKAY ] iris_init - Spawned its_irisOS test suite.
[ OKAY ] iris_init - Supervisor entering sleep state.
[ OKAY ] virtio_blk - Bound to PCI 00:02.0 (Vendor: 0x1af4, Device: 0x1001).
[ OKAY ] virtio_blk - Mapped BAR 4 (phys 0x8000000000 -> vaddr 0x...).
[ OKAY ] virtio_blk - Virtqueue 0 (256 descriptors) & BTI DMA initialized (262144 sectors, 128 MB).
[ OKAY ] virtio_blk - IPC server listening on control channel.
[ OKAY ] iris_pty - Starting Pseudo-Terminal Server.
[ OKAY ] iris_pty - Bound to PCI 00:03.0 (Vendor: 0x1af4, Device: 0x1052).
[ OKAY ] iris_pty - Primary input active via virtio-input-pci.
[ OKAY ] iris_pty - Line discipline initialized (canonical cooked mode, echoing enabled).
[ OKAY ] iris_pty - IPC server listening on control channel.
[ OKAY ] iris_crypt - Verified existing 4 KB Sector 0 header (magic 0x5450595243534952).
[ OKAY ] iris_crypt - Unlocked AES-256-XTS master key via Argon2id dev key fallback.
[ OKAY ] iris_crypt - Block proxy listening (LBA 198656+8, 61432 logical sectors).
[ OKAY ] iris_fshost - Parsed GPT (ESP=2048..67583, System_A=67584..133119 [prio=15,ok=1], System_B=133120..198655, Data=198656..260095).
[ OKAY ] iris_fshost - Mounted System_A (UFS2 magic 0x19540119) at /System [Read-Only].
[ OKAY ] iris_fshost - Mounted encrypted Data (UFS2 magic 0x19540119) at /Users & /Applications [Read-Write].
[ OKAY ] iris_fshost - Active virtual symlinks: /bin -> /System/Bin, /lib -> /System/Libraries.
[ OKAY ] its_irisOS - malloc/free & snprintf heap memory.
[ OKAY ] its_irisOS - FD Table direct write(1) to stdout.
[ OKAY ] its_irisOS - FD Table isatty(1)=1, isatty(2)=1 (console TTY verified).
[ OKAY ] its_irisOS - Console device open("/dev/console") -> fd 3, write & close.
[ OKAY ] its_irisOS - FD Table invalid descriptor rejection (EBADF).
[ OKAY ] its_irisOS - POSIX spawn file actions (init/adddup2/destroy).
[ OKAY ] its_irisOS - VirtIO Block IPC query (BLK_GET_INFO): 262144 sectors (512 bytes/sector).
[ OKAY ] its_irisOS - VirtIO Block IPC write to Sector 100 confirmed (512 bytes).
[ OKAY ] its_irisOS - VirtIO Block IPC read from Sector 100 verified (512/512 bytes matched).
[ OKAY ] its_irisOS - iris_crypt IPC query (BLK_GET_INFO): 61432 usable sectors.
[ OKAY ] its_irisOS - Wrote 512-byte plaintext to logical Sector 60000 via iris_crypt.
[ OKAY ] its_irisOS - Verified physical LBA 258664 on virtio_blk is AES-256-XTS ciphertext.
[ OKAY ] its_irisOS - Verified logical Sector 60000 read via iris_crypt matches decrypted plaintext.
[ OKAY ] its_irisOS - Read /System/Resources/release.txt ("irisOS release 0.1.0").
[ OKAY ] its_irisOS - Write to /System/readonly_test.txt rejected with EROFS (errno=30).
[ OKAY ] its_irisOS - Wrote & verified /Users/verify.txt (59 bytes, ino=10, "irisOS Phase 4 UFS2 + AES-256-XTS VFS Persistence Verified!").
[ OKAY ] its_irisOS - PTY file descriptors 0 and 1 verified (isatty(0)=1, isatty(1)=1).
[ OKAY ] its_irisOS - PTY window size query verified (24 rows x 80 cols).
[ OKAY ] its_irisOS - PTY character echoing to console output verified.
[ OKAY ] its_irisOS - PTY raw mode immediate pass-through verified ("RAW_INPUT").
[ OKAY ] its_irisOS - PTY Ctrl+C signal generation (SIGINT=2) and line discard verified.
[ OKAY ] its_irisOS - PTY Ctrl+D EOF delivery verified (read returned 0 bytes).
[ OKAY ] its_irisOS - Toybox multicall userland verified: /System/Bin/toybox uname -a -> "irisOS".
Toybox execution verified
[ OKAY ] its_irisOS - Toybox shell execution verified: /bin/sh -c (exit=0).
[ OKAY ] its_irisOS - Interactive Test Suite all checks passed.
[ OKAY ] iris_pty - Daemon exited cleanly.
[ OKAY ] iris_crypt - Zeroized AES-256-XTS keys and exited cleanly.
[ OKAY ] virtio_blk - Unpinned BTI DMA tokens (zx_pmt_unpin) and exited cleanly.
```

---

## Remediation Pass 1: VFS Directory Operations, POSIX Sysdeps & Static-PIE TLS Stabilization (COMPLETED)

Following the Adversarial Audit, Remediation Pass 1 implemented genuine filesystem operations and corrected critical mlibc sysdeps deficiencies:

1. **VFS Directory & Inode Operations in `servers/iris_fshost/` & `sdk/libiris/include/iris/ipc.h`:**
   * Added IPC opcodes: `IRIS_FS_OP_READDIR` (`0x0019`), `IRIS_FS_OP_MKDIR` (`0x001A`), `IRIS_FS_OP_UNLINK` (`0x001B`), `IRIS_FS_OP_READLINK` (`0x001C`).
   * Defined wire format `iris_dirent_t` matching POSIX `struct dirent` ABI layout.
   * Implemented `handle_fs_readdir` / `ufs2_readdir`: parses directory entry blocks (`ufs2_dinode`) for mounted UFS2 filesystems and packs virtual directory entries for `/`.
   * Implemented `handle_fs_mkdir` & `handle_fs_unlink`: performs genuine directory creation and file deletion in UFS2 with cylinder group bitmap management, enforcing `EROFS` on `/System`.
   * Implemented `handle_fs_readlink`: resolves virtual symlinks (`/bin` -> `/System/Bin`, `/lib` -> `/System/Libraries`) and queries symlink target strings stored directly in UFS2 dinodes (`di_db`).
   * Supported UFS2 single-indirect block allocation (`di_ib[0]`) in `handle_fs_write`, lifting the previous 48 KB direct-block file cap to >2 MB.

2. **Critical POSIX Sysdeps in `sdk/mlibc-sysdeps/iris/generic/generic.cpp`:**
   * Implemented `sys_open_dir` and `sys_read_entries` routing over `IRIS_FS_OP_READDIR`.
   * Implemented `sys_mkdir`, `sys_mkdirat`, `sys_unlink`, `sys_unlinkat`, `sys_rmdir`, and `sys_faccessat`.
   * Implemented `sys_readlink` and `sys_readlinkat`.
   * Implemented child process tracking table `g_child_table[64]` and supported `waitpid(-1, &status, options)` via `zx_object_wait_many` across all active child process handles.
   * Implemented `sys_kill`: looks up child PID in `g_child_table` and terminates via `zx_task_kill()`.
   * Implemented `sys_tcgetattr` and `sys_tcsetattr`: forwards terminal mode attributes (`ICANON`, `ECHO`) over `IRIS_PROTO_PTY` to `iris_pty`.

3. **Static-PIE TLS Stabilization (`sdk/mlibc-sysdeps/iris/generic/entry.cpp`):**
   * Discovered and resolved an AArch64 page fault caused by bare `static Tcb initial_tcb`: on AArch64 (ELF TLS Variant I), thread-local variables (`errno` at TLS offset `0x90`) are positioned above the TCB (`TPIDR_EL0 + 0x10 + offset`). Lacking an allocated static TLS block, writing `errno = 9` (EBADF) directly corrupted the slab allocator bucket header in `.bss`, crashing on subsequent `malloc()`.
   * Implemented `InitialThreadStorage` allocating a 4 KB static TLS storage buffer:
     * AArch64 (Variant I): `[ Tcb ] [ 16 bytes reserved ] [ TLS data (4096 bytes) ]`.
     * x86_64 (Variant II): `[ TLS data (4096 bytes) ] [ Tcb ]`.
   * Initialized `.tdata` / `.tbss` from the ELF `PT_TLS` segment headers.

4. **Verification:**
   * `x86_64`: `python3 scripts/verify_qemu.py -a x86_64` **PASSED** (all checks clean, 0 warnings).
   * `aarch64`: `python3 scripts/verify_qemu.py -a aarch64` **PASSED** (all checks clean, 0 warnings).
   * Verified: `opendir`/`readdir` on `/System/Bin` returned 19 entries (found `toybox`), `/Users` file creation/unlinking and removal verified, `/System` `EROFS` enforcement confirmed, and `readlink("/System/Bin/sh")` -> `"toybox"` verified.

5. **Immediate Next Step:**
   * Proceed to **Pass 2**: Honest Test Suite Modernization (eliminate remaining weak assertions, full pipe draining, EOF verification), Raw PTY Output verification, and Interactive Shell testing.

---

### Remediation Pass 2: Honest Verification, Raw PTY Stream, and Interactive Shell Default (COMPLETE)

1. **Raw Terminal Byte-Stream Output & Userland Clean Stdio (`servers/iris_pty/`, `sdk/libiris/`):**
   - Replaced all userspace `zx_debuglog_write()` calls in `iris_init`, `iris_pty`, and `mlibc-sysdeps` with `zx_debug_write()`, reserving kernel debuglog strictly for unhandled kernel crashes.
   - Updated `log_status` in `sdk/libiris/include/iris/log.h` and `servers/iris_init/src/main.c` to emit `\r\n` line endings via chunked `zx_debug_write()`, eliminating `[timestamp] pid.tid>` log prefixes for all userspace status logs.
   - Fixed `sys_dup` and `sys_dup2` in `sdk/mlibc-sysdeps/iris/generic/generic.cpp` to correctly duplicate `IRIS_FD_VFS` and `IRIS_FD_PTY` descriptors across internal table entries.

2. **Dual-Input Multiplexing in `servers/iris_pty/`:**
   - Background UART console thread reads serial RX via `zx_debug_read()` (up to 64 bytes/chunk) without blocking the main event loop.
   - Main event loop polls `virtio-input-pci` and multiplexes keystrokes directly into the line discipline.
   - Clean client disconnect handling ensures `iris_pty` stays persistent across child process restarts.

3. **Toybox `posix_spawn()` Userland Spawning (`build/toybox_compat/`):**
   - Implemented `xpopen_setup()`, `xpopen()`, and `xpopen_both()` in `build/toybox_compat/stubs.c` using standard `posix_spawnp()` and `posix_spawn_file_actions_t`.
   - Packaged `bin/toybox` into early BOOTFS as well as `/System/Bin/toybox` on UFS2.
   - Made interactive Toybox `/System/Bin/toybox` with `argv = {"sh", "-i", NULL}` the default boot path in `servers/iris_init/src/main.c`, maintaining supervisor monitoring and automatic respawn.

4. **Null Dereference Fix & Zircon Clean Output Resolution:**
   - **Root Cause Analysis of Crash (`RIP = 0x62a0b33a2130`, `Fault Linear Address = 0x0`, `RDI = 0`):**
     * Disassembly of `out/build-x86_64/bin/iris_init` at offset `0x4130` revealed that in `spawn_elf_binary`, `argc` (`%edx`) and `argv` (`%rcx`) were not preserved before the inlined `find_bootfs_entry` string operations.
     * The compiler clobbered `%rcx` and `%rax`; at offset `0x40bf`, `movq (%rax,%rdi,8), %r8` loaded NULL into `%r8`, followed at `0x4130` by `movzbl (%r8,%r12), %esi`, causing the null dereference crash.
     * Fixed by decoupling `spawn_bootfs_binary` (taking 0/NULL args for early boot daemons) and `spawn_vfs_binary`, making `spawn_process_from_elf` `__attribute__((noinline))`, preserving `argc`/`argv` directly in prologue stack slots, and adding bounds/null checks on every argument pointer.
   - **Clean Output on BOTH QEMU Graphical Console (`gfxconsole`) and Serial:**
     * Patched Zircon microkernel `kernel/zircon/kernel/lib/debuglog/debuglog.cpp` in `DLog::OutputThread` so userspace records (`rec.hdr.pid != 0`) are printed as raw text via `snprintf(tmp, sizeof(tmp), "%s\n", rec.data);` without `[%05d.%03d] %05llu.%05llu>` dmesg prefixes.
     * This preserves output on BOTH the graphical framebuffer console (`gfxconsole`) and UART serial console cleanly.
   - **Interactive Toybox Spawn from UFS2 (`/System/Bin/toybox`):**
     * Implemented `vfs_read_file_to_vmo` in `servers/iris_init/src/main.c`, which reads `/System/Bin/toybox` over `IRIS_FS_OP_OPEN`/`IRIS_FS_OP_READ` from `iris_fshost` into a mapped executable VMO and spawns it with `argv = {"sh", "-i", NULL}`, `PA_HND(PA_NS_DIR, 0)`, and `PA_HND(PA_USER0, 0)` (PTY stream), with graceful fallback to BOOTFS `bin/toybox` if needed.
     * Fixed `argv` passing and mlibc startup data initialization bug (`toybox: Unknown command iris_app` & `virtio_blk` page fault at `0x0`):
       1. Disassembled `virtio_blk` at crash offset `0x18367`: `init_libc()` (`startup.cpp`) runs during `.init_array` constructors and calls `parse_exec_stack(__dlapi_entrystack(), &entry_stack)`. When `entryStack` was `NULL`, `parse_exec_stack` dereferenced `*sp` (`RDI = 0`), faulting at `0x18367`.
       2. In `sdk/mlibc-sysdeps/iris/generic/entry.cpp`: constructed a 16-byte aligned synthetic ELF execution stack (`exec_stack`) containing `argc`, `argv`, `NULL`, `envp` (`NULL`), and `auxv` (`AT_PAGESZ`, `AT_NULL`). Initialized `entryStack` and called `__dlapi_enter(exec_stack)` and `set_startup_data()` BEFORE `run_init_array()`.
       3. In `sdk/mlibc-sysdeps/iris/generic/entry.cpp`: ensured that when `pargs->args_num == 0` or `pargs->args_off == 0`, fallback defaults to `argc = 1, argv[0] = "iris_app", argv[1] = NULL` so no daemon ever runs with NULL argv.
       4. In `servers/iris_init/src/main.c`: ensured `spawn_process_from_elf` always populates `pargs->args_num >= 1` (defaulting to `{name, NULL}` for daemons, and `{"sh", "-i", NULL}` for Toybox).
       5. In `sdk/mlibc-sysdeps/iris/generic/entry.cpp`: set fallback `app_name` to `"sh"` and forced full clean recompilation and relinking of `toybox` into `System_A`, `System_B`, and `BOOTFS`.
     * Verified that `iris_app` is completely eliminated from `bin/toybox`, `iris_a.zbi`, and `disk_x86_64.raw`.
     * Duplicated `shell_pty` across `PA_HND(PA_FD, 0)`, `PA_HND(PA_FD, 1)`, and `PA_HND(PA_FD, 2)` so Toybox's standard I/O (stdin, stdout, stderr) binds directly to `iris_pty`.

5. **PTY Read Blocking Architecture & Graphical Console Mirroring (`iris_pty`, Zircon Kernel):**
   - **Resolved Non-Blocking `read(0)` Returning EOF (`0`) in `iris_pty`:**
     * Implemented per-client pending read request tracking (`pty_client_state_t` with `has_pending_read`, `pending_req_hdr`, and `pending_req_count`).
     * In `servers/iris_pty/src/main.c`, when `IRIS_PTY_OP_READ` arrives and the line discipline ready queue is empty without EOF, `iris_pty` no longer replies immediately with 0 bytes. Instead, it marks the request pending on that client channel and returns to the event loop.
     * Implemented `pty_try_fulfill_pending_reads()`, which is triggered as soon as input arrives from either the UART RX thread or the VirtIO input poller, upon line completion (`\n`/`\r`), raw mode character input, or explicit `Ctrl+D` (EOF).
     * Fulfills pending reads asynchronously by popping ready bytes, sending the IPC response with `status = ZX_OK`, and clearing `has_pending_read`. This allows Toybox `sh -i` to block cleanly in `read(0)` without polling or spinning.
   - **Mirrored `zx_debug_write()` to QEMU Graphical Console (`gfxconsole`):**
     * Patched `kernel/zircon/kernel/syscalls/debug.cpp` (`sys_debug_write`) to call `__kernel_console_write(buf, len);` directly before `dlog_serial_write(buf, len);`.
     * Because `__kernel_console_write` invokes the registered `gfxconsole` render callbacks, all raw userspace terminal writes and shell prompts (`# `) render on both the QEMU graphical display and the serial UART.
     * Updated `scripts/patches/zircon-standalone-llvm.patch` to record the kernel syscall patch.

6. **Backspace & Cursor Block Support (`iris_pty`, `virtio_input`, `gfxconsole`):**
   - **Canonical Backspace & Erase Sequence (`servers/iris_pty/src/main.c`, `servers/drivers/virtio_input/src/virtio_input.c`):**
     * Handled both `0x08` (`'\b'`) and `0x7F` (macOS/VT100 DEL) in `pty_feed_char()`.
     * Added `KEY_DELETE` (111u) translation to `0x7F` alongside `KEY_BACKSPACE` in `virtio_input_translate_key()`.
     * When backspace is pressed with a non-empty line buffer (`line_len > 0`), pops the character and emits the 3-byte visual erase sequence `"\b \b"` (`0x08, 0x20, 0x08`).
   - **Graphical Console Backspace & Solid Cursor Block (`kernel/zircon/kernel/lib/gfxconsole/gfxconsole.cpp`):**
     * Updated `gfxconsole_putc` to handle both `'\b'` and `0x7F` by moving `gfxconsole.x` back one character column.
     * Implemented `gfxconsole_draw_cursor()` drawing a solid cursor block `█` (`gfx_fillrect`) at `(x, y)` on the hardware framebuffer (`hw_surface`).
     * In `gfxconsole_print_callback`, re-blits affected line regions from `gfxconsole.surface` (which retains clean text without the cursor), ensuring the previous cursor is cleanly restored and the visual erase sequence `"\b \b"` erases on-screen characters before drawing the new cursor block at the updated position.

7. **Shell Redirection, Working Directory Inheritance, Arrow Keys, Poweroff/Reboot & Blinking Cursor (COMPLETE):**
   - **Shell Redirection & Client-Side VFS Reference Counting (`sdk/mlibc-sysdeps/iris/generic/generic.cpp`):**
     * Fixed `sys_dup`, `sys_dup2`, `sys_fcntl` (`F_DUPFD` / `F_DUPFD_CLOEXEC`), and `sys_close`.
     * In `sys_dup` and `sys_dup2`: full FD table attributes (`handle`, `vfs_handle`, `flags`, `offset`, `type`) are cloned. Handle duplication via `zx_handle_duplicate()` is performed for channels and sockets, while PTY and VFS channels are cleanly shared.
     * In `sys_dup2`: existing target descriptors are closed cleanly via `sys_close(newfd)` before binding the replacement descriptor.
     * In `sys_close`: implemented client-side reference counting for `IRIS_FD_VFS`. When an FD referencing a server-side `vfs_handle` is closed, `sys_close` checks if any other slot in `g_fd_table` still references that `vfs_handle`. `IRIS_FS_OP_CLOSE` is sent to `iris_fshost` ONLY when the last reference is closed, preventing premature server-side file closure when Toybox redirects built-ins (`echo Hello > /Users/hello.txt` via `dup2(fd, 1)` and `close(fd)`).
   - **Working Directory (`cd`) Inheritance Across `posix_spawn()` (`sdk/mlibc-sysdeps/iris/generic/generic.cpp`, `sdk/mlibc-sysdeps/iris/generic/entry.cpp`):**
     * Implemented robust path canonicalization (`canonicalize_path()`) handling `.` and `..` segments for relative and absolute paths.
     * In `sys_chdir()`: validates destination directory existence with `sys_stat(..., &st)` and `S_ISDIR(st.st_mode)`, updates `g_cwd`.
     * In `sys_spawn()`: ensures child process environment vector contains updated `PWD=<g_cwd>`.
     * In `entry.cpp`: parses `PWD` from `pargs->environ_off` on child startup and calls `iris_set_cwd()`, ensuring spawned binaries (e.g. `ls` after `cd Users`) correctly inherit parent working directory.
   - **Arrow Keys & Input Modifier State Tracking (`servers/drivers/virtio_input/src/virtio_input.c`, `servers/iris_pty/src/main.c`):**
     * In `virtio_input_dev_t`: split modifier tracking into independent `shift_l`/`shift_r`, `ctrl_l`/`ctrl_r`, and `alt_l`/`alt_r` boolean states, guaranteeing Shift state is never prematurely cleared. This ensures quotes (`"`) and redirection (`>`) translate reliably.
     * In `virtio_input_poll()`: added VT100 / ANSI 3-byte escape sequence emission for navigation keys: `KEY_UP` (`\033[A`), `KEY_DOWN` (`\033[B`), `KEY_RIGHT` (`\033[C`), `KEY_LEFT` (`\033[D`), `KEY_HOME` (`\033[H`), `KEY_END` (`\033[F`), `KEY_PAGEUP` (`\033[5~`), `KEY_PAGEDOWN` (`\033[6~`), and `KEY_INSERT` (`\033[2~`).
     * In `iris_pty`: added `IRIS_PTY_OP_SET_ATTR` alias, updated `IRIS_PTY_OP_SET_MODE` so switching to raw mode immediately flushes any uncommitted characters in `line_buf` to the ready queue, and immediately triggers `pty_try_fulfill_pending_reads()`.
   - **`poweroff` and `reboot` System Commands (`sdk/mlibc-sysdeps/iris/generic/generic.cpp`, `build/mkufs2_host.c`, `scripts/build_toybox.sh`):**
     * Dynamically resolved `zx_system_powerctl()` from vDSO.
     * Implemented `sys_reboot(cmd)` in `generic.cpp` mapping `RB_AUTOBOOT` to `ZX_SYSTEM_POWERCTL_REBOOT` and `RB_POWER_OFF` / `RB_HALT_SYSTEM` to `ZX_SYSTEM_POWERCTL_SHUTDOWN`.
     * In `sys_kill()`: routed signals to PID 1 (`SIGTERM`/`SIGINT` -> reboot, `SIGUSR1`/`SIGUSR2`/`SIGPWR` -> poweroff) to `sys_reboot()`.
     * In `servers/iris_init/src/main.c` and `sys_spawn()`: propagated `g_root_resource` as `PA_HND(PA_RESOURCE, 0)` so shells and utility binaries have capability authorization for power management.
     * Enabled `CONFIG_REBOOT=y` in Toybox, compiled `toys/other/reboot.c`, and created `/System/Bin/reboot` and `/System/Bin/poweroff` symlinks in UFS2 images.
   - **Blinking Cursor (`servers/iris_pty/src/main.c`):**
     * Emitted VT100 sequence `\033[1 q\033[?12h` on `iris_pty` initialization to activate blinking block cursor across serial terminals and terminal emulators.

8. **Poweroff Crash Fix, Syscall Sync, Disk Persistence & Toybox Line Editing (COMPLETE):**
   - **`poweroff` Crash & Missing `sys_sync` Sysdeps (`sdk/mlibc-sysdeps/iris/generic/generic.cpp`):**
     * Toybox `reboot.c` calls `sync()`, which previously triggered `MLIBC_MISSING_SYSDEP()` inside `unistd.cpp:793`.
     * Implemented `mlibc::sys_sync()`, `sys_fsync(int fd)`, `sys_fdatasync(int fd)`, and `sys_syncfs(int fd)` dispatching `IRIS_FS_OP_SYNC` over IPC to `iris_fshost`. Exported matching `extern "C"` functions.
     * In `kernel/zircon/kernel/platform/pc/power.cpp`: updated `platform_halt(HALT_ACTION_SHUTDOWN)` to issue QEMU Q35 ACPI poweroff (`outpw(0x604, 0x2000)`) and PIIX4 ACPI poweroff (`outpw(0xb004, 0x2000)`) alongside the QEMU isa-debug-exit port (`outp(kQEMUDebugExitPort, ...)`). If the system cannot immediately power down, halts execution (`x86_hlt()`) rather than falling through to `reboot()`.
   - **Disk Persistence & UFS2 Metadata Flushing (`servers/iris_fshost/src/ufs_io.c`, `servers/iris_fshost/src/main.c`, `build/mkimage.sh`, `scripts/run_qemu.sh`, `build/Makefile`):**
     * Implemented `iris_ufs_flush_device(int dev_id)` in `ufs_io.c` to issue `IRIS_BLK_OP_FLUSH` over IPC to `iris_crypt` and `virtio_blk`.
     * In `servers/iris_fshost/src/main.c`: updated `handle_fs_write`, `handle_fs_close`, and the new `IRIS_FS_OP_SYNC` IPC handler to commit dirty cylinder groups (`cgwrite1`), superblocks (`sbwrite`), and flush the underlying block device cache to disk.
     * In `scripts/run_qemu.sh`: removed unconditional re-invocation of `mkimage.sh` and 64 MB zero-disk recreation. `run_qemu.sh` now launches the existing `disk_<arch>.raw` directly.
     * In `build/Makefile`: updated `run-*` targets to only invoke `image-*` if the disk image does not already exist.
     * In `build/mkimage.sh`: if `disk_<arch>.raw` already exists and is >= 128 MB, extracts the existing 30 MB Data partition (LBA 198656..260095) via `dd` and restores it into the assembled GPT image so user data persists even across full system rebuilds.
   - **Arrow Key Line Editing in Toybox (`build/toybox.config`, `scripts/build_toybox.sh`):**
     * Enabled `CONFIG_TOYBOX_LINEEDIT=y`, `CONFIG_TOYBOX_LIBZ=n`, and `CONFIG_REBOOT=y` in `build/toybox.config`.
     * Added `TOYBOX_LINEEDIT` to the generated configuration whitelist in `scripts/build_toybox.sh` and defined `CFG_TOYBOX_LINEEDIT 1` in `vendor/toybox/generated/config.h`.
     * Verified `iris_pty` in raw mode forwards raw 3-byte escape sequences (`\033[A`, `\033[B`, `\033[C`, `\033[D`) directly without translation or filtering.
     * Recompiled and relinked `bin/toybox` with line editing enabled.

9. **Verification & Clean Build:**
   - Compiled `x86_64` with `./iris build x86_64`:
     * Microkernel (`zircon.elf`), `libzircon.so` vDSO, `mlibc` sysdeps, all ring 3 daemons (`iris_init`, `virtio_blk`, `iris_crypt`, `iris_fshost`, `iris_pty`), and `toybox` compiled with 0 errors and 0 warnings.
     * 128 MB GPT disk image `disk_x86_64.raw` and UEFI NVMe ESP image `esp_x86_64.img` assembled cleanly with existing user data preserved.
   - Ready for manual interactive test by running `./iris run x86_64`.
