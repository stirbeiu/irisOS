# irisOS

irisOS is a modern, 64-bit, microkernel-based operating system designed for `x86_64` and `AArch64` (`aarch64`) architectures.

## Core Philosophy

- **True Microkernel Architecture:** Built upon a stripped-down Zircon microkernel (`bringup` target). Drivers, filesystems, cryptographic services, and window compositors run strictly in user-space (Ring 3 on x86_64 / EL0 on AArch64).
- **Zero Legacy Bloat:** Pure UEFI boot only (Gigaboot). No 16/32-bit legacy BIOS or real-mode code.
- **Two-Zone Licensing Policy:** 
  - **Zone 1 (SDK & Shared Libraries):** Strictly permissive (MIT, BSD, Apache 2.0, FTL). Zero GPL/LGPL contamination.
  - **Zone 2 (Standalone Executables):** Independent address spaces communicating over IPC/pipes (GPL allowed for user-installed utilities).
- **No Google Runtime Frameworks:** Uses only the Zircon microkernel, `libzircon.so` (vDSO), and the host `zbi` packaging tool. Dart, Flutter, FIDL, and Scenic are completely excluded.
- **Unified IPC Protocol:** Raw Zircon channels structured by a standard 24-byte C-ABI header (`iris_msg_hdr_t` in `<iris/ipc.h>`).
- **Modern POSIX Runtime:** `mlibc` C library + `posix_spawn()` process model (no copy-on-write `fork()`) + `toybox` CLI shell.
- **Resilient Storage & A/B Rollback:** Dual-ZBI layout on FAT32 ESP (`iris_a.zbi`, `iris_b.zbi`) paired with GPT A/B slot rollback watchdog, read-only system images (`System_A` / `System_B`), and AES-256-XTS + Argon2 encrypted user data (`/Users`).

## Repository Layout

```text
irisOS/
├── Makefile                  # Root Makefile orchestrating build, run, and test commands
├── iris                      # Standalone CLI build & run manager (scripts/iris.sh)
├── build/                    # Build orchestrator & toolchain configs
│   ├── Makefile              # Subsystem build orchestrator
│   ├── toolchains/           # Homebrew LLVM toolchains (x86_64, aarch64)
│   └── mkimage.sh            # Dual-ZBI and 4-partition GPT disk image packager
├── docs/                     # Subsystem guides (IPC, VFS, build system, drivers)
├── scripts/                  # Fetch, build, test, and QEMU run scripts
├── kernel/                   # [Kernel Zone - MIT] Pinned Zircon microkernel
├── sdk/                      # [SDK Zone - MIT/BSD] Native APIs & mlibc sysdeps
├── servers/                  # [Server Zone - MIT/BSD] Ring 3 daemons & PCIe drivers
├── apps/                     # [App Zone] Terminal emulator and system utilities
├── vendor/                   # Untouched upstream third-party dependencies
├── PROGRESS.md               # Live milestone and development progress tracker
└── AGENTS.md                 # Complete project specification and architecture rules
```

## Host Prerequisites (macOS Apple Silicon)

```bash
brew install llvm cmake ninja meson qemu xorriso mtools
```

Cross-compilation is configured via `/opt/homebrew/opt/llvm` binaries in `build/toolchains/`.

## Building, Running & Testing

irisOS provides high-level commands via `make` or the `./iris` CLI helper.

### 1. Building System Images

Choose from 3 target commands depending on your architecture target:

```bash
# Build for BOTH x86_64 and aarch64
make build all

# Build ONLY for x86_64 (creates out/images/esp_x86_64.img and disk_x86_64.raw)
make build x86_64

# Build ONLY for aarch64 (creates out/images/esp_aarch64.img and disk_aarch64.raw)
make build aarch64
```

### 2. Running in QEMU (Window + Serial Console)

All VM runs launch in a **native QEMU Cocoa window** with the **serial console concurrently streamed to your terminal** for instant debugging. Existing disk images are reused directly without rebuilding:

```bash
# Boot x86_64 in QEMU
make run x86_64

# Boot aarch64 in QEMU
make run aarch64

# Boot BOTH architectures concurrently (prompts with confirmation warning)
make run all
```

*(Press `Ctrl+A` then `X` in the terminal to exit QEMU, or close the Cocoa window)*.

### 3. Automated Headless Verification (Test Suite)

Run automated headless QEMU test suites with non-blocking stdio and strict timeouts to verify hardware driver enumeration, AES-256-XTS encryption, GPT parsing, UFS2 mounting, and persistent VFS operations:

```bash
# Verify BOTH architectures
make test all

# Verify x86_64 in QEMU
make test x86_64

# Verify aarch64 in QEMU
make test aarch64
```

### 4. Alternative CLI: `./iris`

All build, run, and test operations can also be executed directly via the `./iris` CLI:

```bash
./iris build all
./iris build x86_64
./iris build aarch64

./iris run x86_64
./iris run aarch64
./iris run all

./iris test all
./iris test x86_64
./iris test aarch64

./iris clean
./iris help
```

## Subsystem Documentation

Detailed architectural and developer guides are maintained in [`docs/`](docs/):
- **[Build System & Toolchains](docs/build_system.md)**: Heterogeneous orchestrator (CMake, Meson, Make), Homebrew LLVM setup, and target triples (`x86_64-unknown-elf`, `aarch64-unknown-elf`).
- **[IPC Wire Protocol](docs/ipc_protocol.md)**: 24-byte C-ABI header (`iris_msg_hdr_t`), handle passing, and subsystem opcode spaces.
- **[Standard C Library & Sysdeps](docs/mlibc_sysdeps.md)**: `mlibc` port, client-side FD table (`/dev/console`, `/dev/null`), and `posix_spawn()`.
- **[PCIe & VirtIO Drivers](docs/virtio_drivers.md)**: ECAM discovery, VirtIO 1.0+ configuration, and Ring 3 BTI DMA (`virtio-blk-pci`).
- **[Storage, Encryption & VFS](docs/storage_and_vfs.md)**: 4-partition GPT disk layout, `iris_crypt` AES-256-XTS proxy with Argon2id, FreeBSD UFS2 (`libufs`), and `iris_fshost` VFS routing.

## Roadmap & Status

| Phase | Milestone | Status |
| :--- | :--- | :--- |
| **Phase 1 & 2** | Toolchain, Pinned Zircon Build, Gigaboot UEFI & `iris_init` Handoff | **COMPLETED (100%)** |
| **Phase 3** | Safe SDK (`libiris`), `mlibc` Sysdeps, FD Table & `posix_spawn` | **COMPLETED (100%)** |
| **Phase 4** | PCIe Drivers, Storage Encryption & Filesystems (`libufs` / `iris_fshost`) | **COMPLETED (100%)** |
| **Phase 5** | Root System, PTY Server (`iris_pty`), Passphrase Unlock & Toybox Shell | **NEXT UP** |
| **Phase 6** | Zero-Copy Window System (`iris_compositor`), `libiris_gui` & Desktop | Pending |

See [`PROGRESS.md`](PROGRESS.md) for live step-by-step progress and verification logs.

## Maintainers & Contributors

- **Stefan Stirbeiu** ([@stirbeiu](https://github.com/stirbeiu)) — Lead Architect & Creator
- **Antigravity / Gemini** — AI Co-Architect & Engineering Pair Programmer

## License

irisOS source code is licensed under the [MIT License](LICENSE). All third-party upstream components adhere to the Two-Zone Licensing Policy defined in [`AGENTS.md`](AGENTS.md).
