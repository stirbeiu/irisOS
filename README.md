# irisOS

**irisOS** is a modern, 64-bit, microkernel-based operating system targeting `x86_64` and `AArch64` (`aarch64`).

## Architectural Highlights

- **Pure Ring 3 / EL0 Microkernel Design:** Built on a pinned standalone **Zircon** microkernel (`bringup` target). All device drivers (`virtio_blk`, `virtio_gpu`, `virtio_input`, `gop_fb`), filesystems (`iris_fshost` / `libufs`), disk encryption (`iris_crypt`), PTY line discipline (`iris_pty`), and the window compositor (`iris_compositor`) run as isolated user-space processes.
- **Zero Legacy Bloat:** Pure UEFI boot via **Gigaboot** (`BOOTX64.EFI` / `BOOTAA64.EFI`) with Dual-ZBI (`iris_a.zbi`, `iris_b.zbi`) and GPT A/B rollback protection.
- **Two-Zone Licensing Policy:**
  - **Zone 1 (SDK, Kernel & Shared Libraries):** Strictly permissive (`MIT`, `BSD`, `Apache 2.0`, `FTL`). Zero GPL/LGPL contamination.
  - **Zone 2 (Standalone Executables):** Isolated user-space utilities communicating exclusively via IPC/streams (`Toybox` `0BSD`).
- **Deterministic 24-Byte IPC Wire Protocol:** Raw Zircon channels (`zx_channel_*`) structured by `iris_msg_hdr_t` (`<iris/ipc.h>`), excluding FIDL, Dart, Flutter, and Scenic.
- **High-Responsiveness Desktop & VFS Stack:**
  - Zero-copy VMO window compositing (`iris_compositor` + `libiris_gui`) with Pixman SIMD acceleration, front-to-back opaque occlusion culling, sub-rectangle damage tracking, and an O(1) FreeType glyph cache.
  - 1-IPC executable VMO process spawning (`IRIS_FS_OP_GET_EXEC_VMO`) and UFS2 indirect block caching with 32 KB contiguous block coalescing.
  - Block-level **AES-256-XTS + Argon2id** encryption (`iris_crypt`) protecting `/Users` and `/Applications`.

## Host Prerequisites (macOS Apple Silicon)

```bash
brew install llvm cmake ninja meson qemu xorriso mtools
```

## Quick Start (`./iris` CLI or `make`)

```bash
# 1. Build kernel, SDK, servers, apps, and GPT disk images (all | x86_64 | aarch64)
./iris build all
./iris build x86_64
./iris build aarch64

# 2. Boot in QEMU Cocoa GUI window + live serial console (x86_64 | aarch64 | all)
./iris run x86_64
./iris run aarch64

# 3. Run automated headless verification suite across both architectures
./iris test all
./iris test x86_64
./iris test aarch64
```

*(All commands also work via `make build <arch>`, `make run <arch>`, and `make test <arch>`).*

## Documentation Index

- **[AGENTS.md](AGENTS.md)**: Master architectural specification, licensing rules, and anti-reward-hacking policy.
- **[PROGRESS.md](PROGRESS.md)**: Live roadmap status matrix and verified subsystem inventory.
- **[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)**: Two-Zone license ledger and FreeType FTL attribution.
- **[docs/build_system.md](docs/build_system.md)**: Multi-toolchain orchestrator (CMake, Meson, Make) and Homebrew LLVM configuration.
- **[docs/ipc_protocol.md](docs/ipc_protocol.md)**: 24-byte `iris_msg_hdr_t` wire protocol, opcodes, and `libiris` / `libiris_gui` SDK reference.
- **[docs/mlibc_sysdeps.md](docs/mlibc_sysdeps.md)**: `mlibc` POSIX runtime port, FD table, and 1-IPC `posix_spawn()` architecture.
- **[docs/storage_and_vfs.md](docs/storage_and_vfs.md)**: 4-partition GPT layout, `iris_crypt` AES-256-XTS proxy, `libufs`, and `iris_fshost`.
- **[docs/virtio_drivers.md](docs/virtio_drivers.md)**: Unified PCIe ECAM discovery, Zircon BTI DMA, and Ring 3 drivers (`virtio_blk`, `virtio_gpu`, `virtio_input`, `gop_fb`).

## Maintainers & License

- **Stefan Stirbeiu** ([@stirbeiu](https://github.com/stirbeiu)) — Lead Architect & Creator
- **License:** [MIT License](LICENSE) (see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for third-party components).
