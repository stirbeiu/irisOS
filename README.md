# irisOS

irisOS is a modern, 64-bit, microkernel-based operating system designed for `x86_64` and `ARM64` (`aarch64`) architectures.

## Core Philosophy

- **True Microkernel Architecture:** Built upon a stripped-down Zircon microkernel (`bringup` target). Drivers, filesystems, cryptographic services, and window compositors run strictly in user-space (Ring 3 on x86_64 / EL0 on ARM64).
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
├── Makefile                  # Root Makefile orchestrating build and run commands
├── iris                      # Standalone CLI build & run manager (scripts/iris.sh)
├── build/                    # Build orchestrator & toolchain configs
│   ├── Makefile              # Subsystem build orchestrator
│   ├── toolchains/           # Homebrew LLVM toolchains (x86_64, arm64)
│   └── mkimage.sh            # Dual-ZBI and GPT disk image packager
├── docs/                     # Subsystem guides (IPC, VFS, build system, drivers)
├── scripts/                  # Fetch, build, and QEMU run scripts
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

## Building & Running

irisOS provides high-level commands via `make` or the `./iris` CLI helper.

### 1. Building System Images

Choose from 3 target commands depending on your architecture target:

```bash
# Build for BOTH x86_64 and aarch64
make build all

# Build ONLY for x86_64 (creates out/images/esp_x86_64.img)
make build x86_64

# Build ONLY for aarch64 (creates out/images/esp_arm64.img)
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

### 3. Alternative CLI: `./iris`

All build and run operations can also be executed directly via the `./iris` CLI:

```bash
./iris build all
./iris build x86_64
./iris build aarch64

./iris run x86_64
./iris run aarch64
./iris run all

./iris clean
./iris help
```

## Documentation & Roadmap

- See [`AGENTS.md`](AGENTS.md) for full architectural specifications, licensing boundaries, and the 6-phase roadmap.
- See [`PROGRESS.md`](PROGRESS.md) for live step-by-step progress and verification logs.
- See [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) for licensing details of upstream third-party dependencies.

## Maintainers & Contributors

- **Stefan Stirbeiu** ([@stirbeiu](https://github.com/stirbeiu)) — Lead Architect & Creator
- **Antigravity / Gemini** — AI Co-Architect & Engineering Pair Programmer

## License

irisOS source code is licensed under the [MIT License](LICENSE). All third-party upstream components adhere to the Two-Zone Licensing Policy defined in [`AGENTS.md`](AGENTS.md).
