# irisOS Implementation Progress Tracker

This document tracks implementation status, completed milestones, verification commands, and immediate next steps for **irisOS** across all 6 roadmap phases.

---

## Current Status Overview

| Phase | Milestone | Status |
| :--- | :--- | :--- |
| **Phase 1 & 2** | Toolchain, Pinned Zircon Build, Gigaboot UEFI & `iris_init` Handoff | **COMPLETED (100%)** |
| **Phase 3** | Native SDK (`libiris`), `mlibc` Sysdeps, FD Table & `posix_spawn` | **COMPLETED (100%)** |
| **Phase 4** | PCIe Drivers, Storage Encryption & Filesystems (`libufs` / `iris_fshost`) | **COMPLETED (100%)** |
| **Phase 5** | Root System, PTY Server (`iris_pty`), Passphrase Unlock & Toybox Shell | **IN PROGRESS (Step 5.1 COMPLETED)** |
| **Phase 6** | Zero-Copy Window System (`iris_compositor`), `libiris_gui` & Desktop | Pending |

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

## Phase 5: Root System, PTY Server (`iris_pty`), Passphrase Unlock & Toybox Shell (IN PROGRESS)

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

* **Immediate Next Steps for Phase 5:**
  1. **Step 5.2:** Implement `apps/sysutils/passphrase` interactive unlock prompt for `Data` partition decryption keys, replacing the deterministic Argon2 dev key fallback with interactive password entry.
  2. **Step 5.3:** Compile and package `vendor/toybox` against `mlibc` (providing `sh`, `ls`, `cat`, `echo`, `cp`, `mkdir`, etc. without `fork()`, utilizing `posix_spawn()`).
  3. **Step 5.4:** Implement GPT A/B slot watchdog confirmation (`boot_successful = 1`) in `iris_init` upon successful interactive login / shell initialization.
  4. **Release Hostname Configuration:** Switch System Node name from dev default (`irisOS-dev`) to official release name via `IRIS_SYSTEM_NODE` build configuration or runtime configuration when preparing the first full production release.

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
[ OKAY ] its_irisOS - Interactive Test Suite all checks passed.
[ OKAY ] iris_pty - Daemon exited cleanly.
[ OKAY ] iris_crypt - Zeroized AES-256-XTS keys and exited cleanly.
[ OKAY ] virtio_blk - Unpinned BTI DMA tokens (zx_pmt_unpin) and exited cleanly.
```
