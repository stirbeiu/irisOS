# Project Specification: irisOS

## 1. Role & Core Constraints
You are assisting in the architecture and development of **`irisOS`** (one word), a modern 64-bit microkernel operating system.
* **Target Architectures:** `x86_64` and `AArch64` (`aarch64`) ONLY.
* **Zero Legacy Bloat:** Pure UEFI boot only (`Gigaboot` -> `BOOTX64.EFI` / `BOOTAA64.EFI`). No 16/32-bit real/protected mode or BIOS code. Limine is excluded.
* **Microkernel Architecture:** Based on a standalone pinned **Zircon** microkernel (`bringup` target only). All hardware drivers, filesystems, encryption layers, and window compositors run as isolated user-space processes in **Ring 3 (`x86_64`) / EL0 (`AArch64`)**.
* **No Google UI/Runtime Frameworks:** Use ONLY the Zircon microkernel, its C vDSO (`libzircon.so`), and the `zbi` host packaging tool. Strictly exclude Dart, Flutter, FIDL, Scenic, Fuchsia Component Manager, `jiri`, and full Fuchsia `GN`.
* **Reuse Proven FOSS:** Use battle-tested open-source libraries in `vendor/`.

---

## 2. Two-Zone Licensing Policy (CRITICAL)
End-users and businesses must be able to develop and distribute **100% proprietary, closed-source applications and hardware drivers** on `irisOS` without source disclosure obligations.
1. **Zone 1 — SDK, Kernel & Shared Libraries (STRICTLY MIT / BSD / ISC / Apache 2.0 / FTL ONLY):**
   * Strictly **NO GPL or LGPL** in `kernel/`, `sdk/`, or shared libraries (`libiris`, `libiris_gui`, `mlibc`, `frigg`, `libufs`, `fatfs`, `mbedtls`, `pixman`, `freetype`, `libzircon.so`). FreeType is explicitly elected under the **FreeType License (`FTL`, Permissive BSD-style)**.
2. **Zone 2 — Standalone User-Space Executables (MIT / BSD / GPL Allowed):**
   * Standalone executables running in their own address space and communicating only via IPC or standard streams (`toybox`, optional CLI tools) may use any FOSS license.

---

## 3. Locked-In Technical Stack

### 3.1 Host Environment & Cross-Toolchain (`Darwin arm64`, Apple Silicon)
* **Required Host Packages:** `brew install llvm cmake ninja meson qemu xorriso mtools`
* **Toolchain Binaries:** Apple's `/usr/bin/clang` targets Mach-O only and lacks `ld.lld`. All toolchain definitions in `build/toolchains/` (`x86_64-iris.cmake`, `arm64-iris.cmake`, `meson-x86_64.txt`, `meson-arm64.txt`) MUST explicitly use Homebrew LLVM (`/opt/homebrew/opt/llvm/bin/{clang,clang++,ld.lld,llvm-ar,llvm-objcopy,llvm-ranlib}`) with target triples `x86_64-unknown-elf` and `aarch64-unknown-elf` (or `*-unknown-linux-gnu` for SysV ELF ABI selection in `mlibc`).

### 3.2 Bootloader, Dual-ZBI & Bootstrap ABI (`Gigaboot` -> `userboot` -> `iris_init`)
* **Dual-ZBI Layout on FAT32 ESP:** Stores `/EFI/BOOT/iris_a.zbi` (Slot A) and `/EFI/BOOT/iris_b.zbi` (Slot B). Gigaboot reads GPT partition attributes (`Priority`, `Tries Remaining`, `Successful`) to boot the active slot.
* **Bootstrap Handle Extraction (`<zircon/processargs.h>`):** `iris_init` is launched by `userboot` without standard `argc`/`argv` and extracts:
  1. `PA_HND(PA_RESOURCE, 0)`: Root Resource handle (derives MMIO, PCIe ECAM, IRQ, and BTI capabilities).
  2. `PA_HND(PA_VMO_BOOTFS, 0)`: BOOTFS VMO handle (early servers and drivers).
  3. `PA_HND(PA_JOB_DEFAULT, 0)`: Root Job handle (spawns isolated child jobs/daemons).
  4. `PA_HND(PA_VMO_VDSO, 0)`: vDSO VMO handle (`libzircon.so`).
  5. Early serial logging via `zx_debuglog_create(root_resource, 0, &log_handle)`.

### 3.3 IPC Wire Protocol (`sdk/libiris/include/iris/ipc.h`)
* All Ring 3 processes communicate over raw Zircon channels (`zx_channel_*`) using a packed **24-byte C-ABI header**:
  ```c
  #define IRIS_IPC_MAGIC 0x49524953u // 'IRIS'
  #pragma pack(push, 4)
  typedef struct iris_msg_hdr {
      uint32_t magic;       // Must equal IRIS_IPC_MAGIC
      uint32_t protocol;    // IRIS_PROTO_{SYSTEM,FS,CRYPT,COMPOSITOR,PTY,DRIVER}
      uint32_t opcode;      // Subsystem command/event opcode
      uint32_t txid;        // Transaction ID for request/response pairing
      int32_t  status;      // zx_status_t (ZX_OK or negative error)
      uint32_t payload_len; // Trailing payload bytes (max 65,512)
  } iris_msg_hdr_t;
  #pragma pack(pop)
  ```

### 3.4 Capability Model, POSIX Runtime (`mlibc`) & VFS (`iris_fshost`)
* **No `fork()`:** Process creation uses **`posix_spawn()`** (`iris_spawn` over `zx_process_create` + `zx_process_start`), inheriting open FDs via `PA_HND(PA_FD, fd)`.
* **C Library & Dynamic Loader:** `vendor/mlibc` + `sdk/mlibc-sysdeps/iris/` producing `libc.a`, `libc.so`, and `ld-iris.so`.
* **1-IPC Executable VMO Spawning:** `sys_spawn()` requests cached read-only executable VMOs for `/System` binaries via `IRIS_FS_OP_GET_EXEC_VMO` before falling back to `#!` script or streaming reads.

### 3.5 PCIe Discovery, Storage, Encryption & Graphics
* **Unified PCIe (ECAM):** Used on both `x86_64` (QEMU `q35`, `0xE0000000`) and `aarch64` (QEMU `virt`, `0x4010000000`) for `virtio-blk-pci`, `virtio-gpu-pci`, and `virtio-input-pci`. Drivers use Zircon BTI (`zx_bti_pin` with `ZX_BTI_CONTIGUOUS`) for safe DMA.
* **4-Partition GPT Disk (`128 MB`):**
  1. `ESP` (`32 MB` FAT32 — Gigaboot + `iris_a.zbi` / `iris_b.zbi`).
  2. `System_A` (`32 MB` Read-Only FreeBSD UFS2 via `libufs`).
  3. `System_B` (`32 MB` Inactive Read-Only UFS2 slot for A/B updates).
  4. `Data` (`30 MB` Read-Write UFS2 encrypted by `iris_crypt`).
* **Block Encryption (`iris_crypt`):** Sits between `libufs` and `virtio_blk` (`[libufs] <==> [iris_crypt] <==> [virtio_blk]`), encrypting `Data` with **AES-256-XTS + Argon2id** (`mbedtls`). Physical sectors `0..7` (4 KB) store `iris_crypt_header_t`; logical sectors start at `+8`.
* **Graphics & Shell:** `virtio_gpu` + UEFI GOP fallback (`gop_fb`), `iris_compositor` (zero-copy shared VMO surfaces, Pixman SIMD compositing with occlusion culling, and O(1) FreeType glyph cache), `libiris_gui`, `iris_pty`, and `Toybox` (`0BSD`).

---

## 4. Repository Layout & Runtime VFS Hierarchy

### 4.1 Git Repository Structure
```text
irisOS/
├── Makefile / iris           # Top-level CLI & build/run/test entrypoints
├── build/                    # CMakeLists.txt, Makefile, toolchains/, mkimage.sh
├── docs/                     # Concise subsystem guides (build, IPC, sysdeps, storage, drivers)
├── scripts/                  # fetch_*.sh, run_qemu.sh, verify_qemu.py, mk_gpt_disk.py
├── kernel/zircon/            # [Kernel Zone - MIT] Pinned Zircon microkernel source
├── sdk/                      # [SDK Zone - MIT/BSD ONLY] libiris, libiris_gui, mlibc-sysdeps/iris
├── servers/                  # [Server Zone - MIT/BSD] iris_init, iris_fshost, iris_crypt,
│                             #   iris_compositor, iris_pty, drivers/{pci,virtio_blk,virtio_gpu,virtio_input,gop_fb}
├── apps/                     # [App Zone] terminal, sysutils/{passphrase,abctl,sys_overlay}, tests/its_irisOS
├── vendor/                   # Untouched upstream FOSS (mlibc, frigg, toybox, libufs, fatfs, mbedtls, pixman, freetype)
├── PROGRESS.md               # Concise milestone tracker (NO raw boot logs)
└── out/                      # Git-ignored build artifacts & disk images
```

### 4.2 Runtime VFS Hierarchy (`iris_fshost` — macOS-Style, No `/usr` Clutter)
```text
/
├── System/                   # Mounted Read-Only from active GPT slot (System_A or System_B)
│   ├── Bin/                  # toybox binary, POSIX symlinks (sh, ls, cat, etc.), sysutils
│   ├── Libraries/            # ld-iris.so, libc.so, libzircon.so, libiris.so, libpixman-1.so, libfreetype.so
│   ├── Servers/              # iris_compositor, drivers, background daemons
│   └── Resources/            # Fonts (InterDisplay-*.otf), system release metadata
├── Users/admin/              # Mounted Read-Write from AES-256-XTS encrypted Data partition
├── Applications/             # Mounted Read-Write from encrypted Data partition
├── bin -> /System/Bin        # In-memory virtual symlink for POSIX compatibility (#!/bin/sh)
└── lib -> /System/Libraries  # In-memory virtual symlink for ELF dynamic loader
```

---

## 5. Phased Implementation Roadmap
* **Phase 1 & 2 (Complete):** Homebrew LLVM toolchains, standalone Zircon + vDSO + `zbi`, Gigaboot UEFI, `iris_init` bootstrap handoff on `x86_64` & `aarch64`.
* **Phase 3 (Complete):** `sdk/libiris` (24-byte IPC, VMO, `iris_spawn`), `sdk/mlibc-sysdeps/iris/`, client FD table, `libc.a` / `libc.so` / `ld-iris.so`.
* **Phase 4 (Complete):** ECAM PCIe discovery, `virtio_blk` (BTI DMA), `iris_crypt` (AES-256-XTS + Argon2id), `iris_fshost` (GPT + `libufs` UFS2 RO `/System` & RW `/Users`).
* **Phase 5 (Complete):** `iris_pty`, `virtio_input` + UART fallback, `passphrase`, `toybox` shell (`posix_spawn`), A/B GPT watchdog confirmation (`boot_successful = 1`).
* **Phase 6 (In Progress):**
  * Steps 6.1–6.3 (Complete): `virtio_gpu` + `gop_fb`, `pixman` + `freetype` ports, `iris_compositor` + `libiris_gui` with O(1) glyph cache & damage culling.
  * Step 6.4 (Next Up): Graphical terminal emulator app (`apps/terminal`) running Toybox `sh` in a desktop window.
* **Phase 7 (Pending):** Service supervision & crash recovery in `iris_init`, reverse-dependency graceful shutdown (`poweroff`/`reboot` IPC to `iris_init` to zero keys and unpin BTI DMA before ACPI poweroff), and strict blocking input in `apps/sysutils/passphrase` (removing `IRIS_CRYPT_DEV_PASSPHRASE` interactive fallback).

---

## 6. Documentation & Context Efficiency Policy
1. **Token-Efficient Navigation (MANDATORY):** Before grepping across the codebase or reading large source files (`>250` LOC), consult [`docs/repo_graph.md`](docs/repo_graph.md) or run `./iris graph op|file|target <term>` to locate exact IPC handlers, CMake dependencies, and function `Lstart-Lend` ranges, then call `read_file` on only the relevant line range. Never load `docs/repo_graph.json` directly into context. Refresh the graph with `./iris graph` after adding or moving files, functions, or IPC opcodes (also auto-runs on `./iris build`).
2. **Code Documentation:** Public SDK headers (`sdk/`) require doc-comments detailing purpose, arguments, `zx_status_t` returns, and handle ownership. Servers/drivers (`servers/`) require module lifecycle headers and inline explanations for DMA (`zx_bti_pin`), IPC packing, and crypto.
3. **Concise Markdown & Zero Boot Logs:** Never paste raw multi-line QEMU boot logs or duplicate prose into `PROGRESS.md` or `docs/*.md`. Keep all Markdown files concise, structured in tables/lists, and token-efficient.
4. **Live Progress Tracking (`PROGRESS.md`):** Upon completing a milestone, record the concise capability added, the exact verification command (`./iris test all`), and the immediate next step.

---

## 7. Anti-Reward Hacking & Strict Verification Policy (CRITICAL)

### 7.1 Zero Shortcuts & No Fake Plumbing
* **No Hardcoded Magic Offsets:** Servers and drivers must dynamically parse on-disk/hardware structures (MBR, GPT headers, UFS2 superblocks, ELF headers, PCIe capabilities). Hardcoding sector numbers generated by `mkimage.sh` is forbidden.
* **No RAM-Backed Persistence Fakes:** Writes to `/Users` and `/Applications` must genuinely traverse `mlibc -> iris_fshost -> libufs -> iris_crypt -> virtio_blk` and persist to disk. Caching is permitted ONLY for immutable read-only `/System` VMOs/blocks and write-through UFS2 indirect pointer lookups.
* **No Silent Syscall Stubs:** Unimplemented functions in `sdk/mlibc-sysdeps/iris/` must log an explicit warning and return `ENOSYS` (never silently `return 0`).

### 7.2 Strict Test Integrity (`its_irisOS`) & Zero-Warning Criteria
* **Full Payload & EOF Verification:** Tests reading from pipes, PTYs, or files must loop `read()` until EOF (`0`) and assert the entire expected buffer.
* **End-to-End POSIX Pathing:** User-space tests must exercise standard POSIX APIs (`open`, `read`, `write`, `readdir`, `stat`, `posix_spawn`) rather than bypassing `mlibc` with raw IPC, unless specifically testing low-level driver wire protocols.
* **Cross-Reboot Persistence Verification:** Storage milestones must be verified across two separate QEMU boots (Boot 1 writes and flushes; Boot 2 verifies persistence without recreating disk images).
* **Zero-Warning Acceptance:** Any build or test run is an automatic **`FAIL`** if console output contains any `mlibc warning`, unhandled Zircon exception, bad handle fault, BTI/PMT leak on teardown, or corrupted terminal output. Report blockers candidly; never weaken assertions.
