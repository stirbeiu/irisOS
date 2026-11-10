# irisOS Development Progress Tracker

## 1. Roadmap Completion Matrix

| Phase | Milestone | Status | Verification Command |
| :--- | :--- | :---: | :--- |
| **Phase 1 & 2** | Homebrew LLVM Toolchains, Pinned Zircon Kernel + vDSO + `zbi`, Gigaboot UEFI & `iris_init` Bootstrap | **100% Complete** | `./iris test all` |
| **Phase 3** | Native SDK (`libiris`), 24-Byte IPC Protocol, `mlibc` Sysdeps, FD Table & `posix_spawn` | **100% Complete** | `./iris test all` |
| **Phase 4** | Unified PCIe (ECAM), `virtio_blk` (BTI DMA), `iris_crypt` (AES-256-XTS + Argon2id) & `iris_fshost` (GPT + UFS2) | **100% Complete** | `./iris test all` |
| **Phase 5** | Root System, `iris_pty`, `virtio_input`, `passphrase`, `toybox` Shell & A/B Slot Watchdog Confirmation | **100% Complete** | `./iris test all` |
| **Phase 6 (6.1–6.3)** | `virtio_gpu` + GOP Fallback, `pixman` + `freetype`, `iris_compositor` & `libiris_gui` + Responsiveness Tiers 1–4 | **Complete** | `./iris test all` |
| **Phase 6 (6.4)** | Graphical Terminal Emulator (`apps/terminal`) Running Interactive Toybox `sh` in a Desktop Window | **NEXT UP** | — |
| **Phase 7** | Service Supervision (`iris_init`), Reverse-Dependency Graceful Shutdown & Blocking Passphrase Hardening | Pending | — |

---

## 2. Verified Subsystem Inventory

### Phases 1 & 2: Cross-Toolchain, Zircon Microkernel & Bootstrap
* **Toolchains (`build/toolchains/`):** Configured for macOS Apple Silicon using Homebrew LLVM (`/opt/homebrew/opt/llvm/bin/clang`, `ld.lld`) targeting `x86_64-unknown-elf` and `aarch64-unknown-elf`, plus `libclang_rt.builtins-<arch>.a`.
* **Zircon & Gigaboot (`kernel/zircon/`):** Standalone CMake/Ninja build producing `zircon.elf`, `libzircon.so` (vDSO), host `zbi`, and UEFI Gigaboot (`BOOTX64.EFI` / `BOOTAA64.EFI`).
* **Bootstrap (`servers/iris_init/`):** Extracts `PA_RESOURCE`, `PA_VMO_BOOTFS`, `PA_JOB_DEFAULT`, and `PA_VMO_VDSO` from `userboot`, initializes serial debuglog, enumerates PCIe via ECAM, and launches Ring 3 servers.

### Phase 3: Native SDK (`libiris`) & Standard C Runtime (`mlibc`)
* **`sdk/libiris`:** Implements the 24-byte `iris_msg_hdr_t` wire protocol (`<iris/ipc.h>`), channel/VMO wrappers, and `iris_spawn()` ELF loader with `PA_HND(PA_FD, fd)` inheritance.
* **`sdk/mlibc-sysdeps/iris/`:** Full `mlibc` port (`libc.a`, `libc.so`, `ld-iris.so`) with 256-slot client FD table (`CONSOLE`, `VFS`, `PTY`, `PIPE`, `DEV_NULL`, `DEV_ZERO`), `sys_spawn` (with 1-IPC `IRIS_FS_OP_GET_EXEC_VMO` fast-path and `#!` shebang support), `sys_futex_*`, TLS (`%fs` / `tpidr_el0`), and `termios`/`ioctl`.

### Phase 4: PCIe Drivers, Block Encryption & Filesystems
* **Unified PCIe (`servers/drivers/pci/`):** ECAM bus scanner (`0xE0000000` on `x86_64 q35`, `0x4010000000` on `aarch64 virt`) with VirtIO 1.0+ capability parser.
* **Block Driver (`servers/drivers/virtio_blk/`):** Ring 3 PCIe driver using Zircon BTI (`zx_bti_pin` with `ZX_BTI_CONTIGUOUS`) and split Virtqueue 0.
* **Encryption Proxy (`servers/iris_crypt/`):** Mbed-TLS 3.6 AES-256-XTS + Argon2id block filter managing the 4 KB Sector 0 header (`iris_crypt_header_t`) on Partition 4 (`Data`, LBA `+8` translation).
* **VFS & UFS2 (`servers/iris_fshost/`):** Parses 4-partition GPT (`ESP`, `System_A`, `System_B`, `Data`), mounts `System_A` read-only at `/System` (`EROFS` enforced) and decrypted `Data` read-write at `/Users` and `/Applications` via `vendor/libufs`, resolves `/bin` and `/lib` virtual symlinks, caches UFS2 indirect blocks (`di_ib[0]`/`di_ib[1]`) with 32 KB contiguous block coalescing, and serves cached `/System` executable VMOs via `IRIS_FS_OP_GET_EXEC_VMO`.

### Phase 5: PTY Server, Input Drivers, System Utilities & Toybox Shell
* **PTY & Input (`servers/iris_pty/`, `servers/drivers/virtio_input/`):** Canonical (`ICANON`, `ECHO`, backspace editing, `Ctrl+C` `SIGINT`, `Ctrl+D` EOF) and raw line discipline fed by `virtio-keyboard-pci` and 2ms low-latency UART fallback.
* **System Utilities (`apps/sysutils/`):** `passphrase` (`Data` partition unlock utility), `abctl` (A/B slot status & switching), and `sys_overlay` (developer RAM overlay utility).
* **Shell (`vendor/toybox/`):** Cross-compiled against `mlibc` into `/System/Bin/toybox` with 200+ POSIX command symlinks (`sh`, `ls`, `cat`, `cp`, `mkdir`, `rm`, `uname`, etc.) and A/B boot watchdog confirmation (`IRIS_FS_OP_CONFIRM_BOOT`).

### Phase 6 (Steps 6.1–6.3) & Responsiveness Tiers 1–4: Graphics & Compositor
* **Display Drivers (`servers/drivers/virtio_gpu/`, `servers/drivers/gop_fb/`):** Ring 3 `virtio-gpu-pci` 2D scanout driver (`1280x800` `B8G8R8X8`, BTI-pinned shared framebuffer VMO) and UEFI GOP framebuffer fallback.
* **Rendering Libraries (`vendor/pixman/`, `vendor/freetype/`):** Cross-compiled into `/System/Libraries/` (`libpixman-1`, `libfreetype` under FTL) with bundled `InterDisplay-{Regular,Bold,Italic}.otf` fonts in `/System/Resources/Fonts/`.
* **Window Compositor (`servers/iris_compositor/`) & Client SDK (`sdk/libiris_gui/`):**
  * Zero-copy shared VMO client window surfaces (`IRIS_COMPOSITOR_OP_CREATE_WINDOW`, `DESTROY_WINDOW`, `DAMAGE`, `MOVE_WINDOW`, `POLL_EVENT`, `OPEN_CLIENT`).
  * **Responsiveness Optimizations:** O(1) FreeType glyph bitmap & metrics cache with division-free alpha blending (`font.c`), sub-rectangle client damage tracking (`main.c`), front-to-back opaque window occlusion culling + `PIXMAN_OP_SRC` interior blits (`render.c`), 250 Hz (`4ms`) input sampling, and multi-threaded QEMU TCG (`-accel tcg,thread=multi,tb-size=512`).

---

## 3. Immediate Next Step: Phase 6, Step 6.4 — Graphical Terminal Emulator (`apps/terminal`)
1. Implement `apps/terminal` using `libiris_gui`, `pixman`, and `freetype` to create a desktop window (`"Terminal"`).
2. Open a PTY session with `iris_pty`, spawn `/System/Bin/sh` attached to the PTY, render the character grid with cursor and scrolling inside the shared window VMO, and forward `iris_window_poll_event()` keyboard input into the PTY.
3. Verify interactive graphical shell execution on both `x86_64` and `aarch64` via `./iris test all`.
