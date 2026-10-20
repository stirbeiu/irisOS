# irisOS Implementation Progress Tracker

This document tracks implementation status, completed milestones, verification commands, and immediate next steps for **irisOS** across all 6 roadmap phases.

---

## Current Status Overview

| Phase | Milestone | Status |
| :--- | :--- | :--- |
| **Phase 1 & 2** | Toolchain, Pinned Zircon Build, Gigaboot UEFI & `iris_init` Handoff | **COMPLETED (100%)** |
| **Phase 3** | Safe SDK (`libiris`), `mlibc` Sysdeps, FD Table & `posix_spawn` | **COMPLETED (100%)** |
| **Phase 4.1** | Unified PCIe (ECAM) Discovery & Ring 3 `virtio-blk-pci` Driver | **COMPLETED (100%)** |
| **Phase 4.2** | Block-Level Encryption Proxy (`iris_crypt` — AES-256-XTS + Argon2id) | **COMPLETED (100%)** |
| **Phase 4.3** | GPT Parser, FatFs (`ESP`), `libufs` UFS2 & VFS Router (`iris_fshost`) | **NEXT UP** |
| **Phase 5** | Root System, PTY Server (`iris_pty`), Passphrase Unlock & Toybox Shell | Pending |
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

## Phase 3: The Safe SDK (`libiris`) & `mlibc` Integration (COMPLETED)

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

## Phase 4: PCIe Drivers, Storage Encryption & Filesystems (IN PROGRESS)

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

### Step 4.3: GPT Parser, FatFs, `libufs` UFS2 & VFS Router (`iris_fshost`) (NEXT UP)
* **Remaining Tasks for Step 4.3:**
  1. Cross-compile `vendor/fatfs` (FAT32 for `ESP`) and `vendor/libufs` (UFS2/FFS for `System_A`, `System_B`, and `Data`).
  2. Update `build/mkimage.sh` to format a 4-partition GPT disk image (`ESP`, `System_A`, `System_B`, `Data`).
  3. Implement `servers/iris_fshost` with GPT parser, A/B slot selector, `/System` read-only UFS2 mount, `/Users` & `/Applications` read-write encrypted UFS2 mount (via `iris_crypt`), and `/bin` + `/lib` virtual symlinks.
  4. Connect `mlibc` file syscalls (`sys_open`, `sys_read`, `sys_write`, `sys_seek`, `sys_close`, `sys_stat`) to `iris_fshost` over `IRIS_PROTO_FS`.

---

## Build & Verification Commands

```bash
# Build system and disk images for both x86_64 and aarch64
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

## Live Confirmed OS Boot Logs (`x86_64` & `aarch64`)

### 1. `x86_64` Confirmed Boot (`./iris run x86_64 --headless`)
```text
[00005.271] 01051.01054> System Node: irisOS-dev (x86_64)
[00005.271] 01051.01054> [  OK  ] Extracted Root Resource capability.
[00005.271] 01051.01054> [  OK  ] Extracted BOOTFS VMO capability.
[00005.271] 01051.01054> [  OK  ] Extracted Root Job capability.
[00005.271] 01051.01054> [  OK  ] Extracted vDSO VMO capability.
[00005.271] 01051.01054> [  OK  ] Extracted Root VMAR capability.
[00005.275] 01051.01054> [  OK  ] Started process its_irisOS (bin/its_irisOS).
[00005.364] 01064.01066> [  OK  ] its_irisOS: Interactive Test Suite all checks passed.
[00005.389] 01051.01054> [  OK  ] Mapped PCIe ECAM configuration space at phys 0x00000000e0000000.
[00005.394] 01051.01054> [  OK  ] Discovered VirtIO Block Device at PCI 00:03.0 (Vendor: 0x1af4, Device: 0x1001).
[00005.434] 01081.01083> [  OK  ] virtio_blk: Bound to PCI 00:03.0 (Vendor: 0x1af4, Device: 0x1001).
[00005.449] 01081.01083> [  OK  ] virtio_blk: Virtqueue 0 (256 descriptors) & BTI DMA initialized (131072 sectors, 64 MB).
[00005.471] 01051.01054> [  OK  ] VirtIO Block IPC read from Sector 100 verified (512/512 bytes matched).
[00005.471] 01051.01054> [  OK  ] Phase 4 Step 4.1 PCIe & VirtIO Block verification complete.
[00005.509] 01107.01109> [  OK  ] iris_crypt: Verified existing 4 KB Sector 0 header (magic 0x5450595243534952).
[00005.513] 01107.01109> [  OK  ] iris_crypt: Unlocked AES-256-XTS master key via Argon2id dev key fallback.
[00005.519] 01107.01109> [  OK  ] iris_crypt: Block proxy listening (logical Sector 0 -> physical Sector 8, 131064 sectors).
[00005.537] 01051.01054> [  OK  ] Verified physical Sector 8 on virtio_blk is AES-256-XTS ciphertext (plain[0..3]=49524953 != cipher[0..3]=5ba520ff).
[00005.544] 01051.01054> [  OK  ] Verified logical Sector 0 read via iris_crypt matches original plaintext (512/512 bytes matched).
[00005.548] 01107.01109> [  OK  ] iris_crypt: Zeroized AES-256-XTS keys and exited cleanly.
[00005.555] 01081.01083> [  OK  ] virtio_blk: Unpinned BTI DMA tokens (zx_pmt_unpin) and exited cleanly.
[00005.561] 01051.01054> [  OK  ] Phase 4 Step 4.2 iris_crypt AES-256-XTS proxy verification complete.
```

### 2. `aarch64` Confirmed Boot (`./iris run aarch64 --headless`)
```text
[00008.889] 01044.01047> System Node: irisOS-dev (aarch64)
[00008.889] 01044.01047> [  OK  ] Extracted Root Resource capability.
[00008.889] 01044.01047> [  OK  ] Extracted BOOTFS VMO capability.
[00008.889] 01044.01047> [  OK  ] Extracted Root Job capability.
[00008.889] 01044.01047> [  OK  ] Extracted vDSO VMO capability.
[00008.889] 01044.01047> [  OK  ] Extracted Root VMAR capability.
[00008.892] 01044.01047> [  OK  ] Started process its_irisOS (bin/its_irisOS).
[00008.902] 01057.01059> [  OK  ] its_irisOS: Interactive Test Suite all checks passed.
[00008.907] 01044.01047> [  OK  ] Mapped PCIe ECAM configuration space at phys 0x0000004010000000.
[00008.908] 01044.01047> [  OK  ] Discovered VirtIO Block Device at PCI 00:02.0 (Vendor: 0x1af4, Device: 0x1001).
[00008.929] 01074.01076> [  OK  ] virtio_blk: Bound to PCI 00:02.0 (Vendor: 0x1af4, Device: 0x1001).
[00008.933] 01074.01076> [  OK  ] virtio_blk: Virtqueue 0 (256 descriptors) & BTI DMA initialized (131072 sectors, 64 MB).
[00008.938] 01044.01047> [  OK  ] VirtIO Block IPC read from Sector 100 verified (512/512 bytes matched).
[00008.938] 01044.01047> [  OK  ] Phase 4 Step 4.1 PCIe & VirtIO Block verification complete.
[00008.954] 01100.01102> [  OK  ] iris_crypt: Verified existing 4 KB Sector 0 header (magic 0x5450595243534952).
[00008.954] 01100.01102> [  OK  ] iris_crypt: Unlocked AES-256-XTS master key via Argon2id dev key fallback.
[00008.955] 01100.01102> [  OK  ] iris_crypt: Block proxy listening (logical Sector 0 -> physical Sector 8, 131064 sectors).
[00008.958] 01044.01047> [  OK  ] Verified physical Sector 8 on virtio_blk is AES-256-XTS ciphertext (plain[0..3]=49524953 != cipher[0..3]=5ba520ff).
[00008.959] 01044.01047> [  OK  ] Verified logical Sector 0 read via iris_crypt matches original plaintext (512/512 bytes matched).
[00008.959] 01100.01102> [  OK  ] iris_crypt: Zeroized AES-256-XTS keys and exited cleanly.
[00008.962] 01074.01076> [  OK  ] virtio_blk: Unpinned BTI DMA tokens (zx_pmt_unpin) and exited cleanly.
[00008.969] 01044.01047> [  OK  ] Phase 4 Step 4.2 iris_crypt AES-256-XTS proxy verification complete.
```
