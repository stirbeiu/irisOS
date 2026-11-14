# irisOS Development Progress Tracker

## 1. Roadmap Completion Matrix

| Phase | Milestone | Status | Verification Command |
| :--- | :--- | :---: | :--- |
| **Phase 1 & 2** | Homebrew LLVM Toolchains, Pinned Zircon Kernel + vDSO + `zbi`, Gigaboot UEFI & `iris_init` Bootstrap | **100% Complete** | `./iris test all` |
| **Phase 3** | Native SDK (`libiris`), 24-Byte IPC Protocol, `mlibc` Sysdeps, FD Table & `posix_spawn` | **100% Complete** | `./iris test all` |
| **Phase 4** | Unified PCIe (ECAM), `virtio_blk` (BTI DMA), `iris_crypt` (AES-256-XTS + Argon2id) & `iris_fshost` (GPT + UFS2) | **100% Complete** | `./iris test all` |
| **Phase 5** | Root System, `iris_pty`, `virtio_input`, `passphrase`, `toybox` Shell & A/B Slot Watchdog Confirmation | **100% Complete** | `./iris test all` |
| **Phase 6 (6.1–6.3)** | `virtio_gpu` + GOP Fallback, `pixman` + `freetype`, `iris_compositor` & `libiris_gui` + Responsiveness Tiers 1–4 | **Complete** | Interactive GUI |
| **Phase 6 (6.4)** | Graphical Terminal Emulator (`apps/terminal`) with JetBrains Mono Variable Font (`400`/`700`) & PTY Shell | **Complete** | `./iris build all` |
| **Phase 6 (6.5)** | Figma Taskbar Overhaul (`taskbar.c`): Gradient Chrome, Start Menu (Profile, Recent Apps, Power/Session), Drag-and-Drop Pin Dock (`/Users/admin/.taskbar_pins`), Overflow Tray, Separate Notification Center & Calendar, Privacy Indicator | **Complete** | `./iris build all` |
| **Phase 6 (6.6)** | Variable Font & Material Icons Engine (`font.c`, `render.c`): Inter Display / Inter Tight / Inter Variable + Material Icons TTF, Translucent Titlebar Gradient & Hoverable Caption Buttons | **Complete** | `./iris build all` |
| **Phase 7** | Full Service Supervision (`zx_object_wait_many`), Reverse-Dependency Graceful Shutdown, Graphical Passphrase Unlock, VFS `rename`/`truncate` & `aarch64` PL031 RTC | **Complete** | `./iris build all` |

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
* **Rendering Libraries (`vendor/pixman/`, `vendor/freetype/`):** Cross-compiled into `/System/Libraries/` (`libpixman-1`, `libfreetype` under FTL) with bundled variable fonts (`Inter`, `Inter Display`, `Inter Tight`, `JetBrains Mono`) and `MaterialIcons-Regular.ttf` in `/System/Resources/Fonts/`.
* **Window Compositor (`servers/iris_compositor/`) & Client SDK (`sdk/libiris_gui/`):**
  * Zero-copy shared VMO client window surfaces (`IRIS_COMPOSITOR_OP_CREATE_WINDOW`, `DESTROY_WINDOW`, `DAMAGE`, `MOVE_WINDOW`, `POLL_EVENT`, `OPEN_CLIENT`).
  * **Responsiveness & Chrome:** O(1) FreeType variable-font (`wght`/`opsz`) & Material Icon glyph cache with `FT_Set_Var_Design_Coordinates` (`font.c`), translucent linear-gradient title bars (`#FFFFFF` 100% -> 75% alpha) with hoverable Material caption buttons & Maximize/Restore (`render.c`, `input.c`), front-to-back client occlusion culling, 250 Hz (`4ms`) input sampling, and multi-threaded QEMU TCG.

### Phase 6 (Steps 6.4–6.6): Terminal Emulator & Figma Taskbar Overhaul
* **Monospace Terminal (`apps/terminal`):** Interactive graphical terminal window using `JetBrainsMono-Variable.ttf` (`400` Regular / `700` Bold), PTY connection to `/bin/sh`, translucent dark theme, and damage-tracked scrolling.
* **Modular Taskbar & System UI (`servers/iris_compositor/src/taskbar.c`):**
  * Hardware RTC synchronization in `iris_init` (CMOS `0x70/0x71` on `x86_64`, PL031 `0x09010000` on `aarch64`) adjusting `ZX_CLOCK_UTC` with live minute-boundary clock updates.
  * Translucent linear-gradient bottom bar (`#FFFFFF` 100% -> 85% alpha), left-side `eye_light` Start button opening Start Menu (admin profile avatar, application list, recently opened apps, Sleep/Restart/Shutdown/Log Out/Switch User), and centered pinned+running dock with focused line / unfocused dot indicators and drag-and-drop pinning persisted to `/Users/admin/.taskbar_pins`.
  * Material Icons system tray: 3rd-party overflow chevron (`^`), Volume/Bluetooth/Network/Battery status icons + interactive flyouts, separate Notification Center flyout (`FLYOUT_NOTIFICATIONS`), independent Calendar flyout (`FLYOUT_CALENDAR`), and persistent Privacy Indicator dot (`FLYOUT_PRIVACY`).
* **Image Packaging (`build/mkufs2_host.c`, `build/mkimage.sh`, `scripts/fetch_fonts.sh`):** Fixed `build_dir_block()` multi-chunk UFS2 directory spanning up to 4096 bytes and added `/System/Resources/Fonts/` legacy symlinks.
* **Test Code Clean-up:** Automated test suite (`its_irisOS`, `verify_qemu.py`, boot test flags) removed per user direction; interactive manual testing enabled.

---

## 3. Phase 7 Summary: Supervision, Graceful Shutdown & Hardening
* **Reverse-Dependency Graceful Shutdown:** `mlibc` `sys_reboot()` routes `IRIS_SYS_OP_SHUTDOWN`/`IRIS_SYS_OP_REBOOT` over `IRIS_PROTO_SYSTEM` (`PA_HND(PA_USER2, 1)`) to `iris_init`, which stops `iris_compositor` and `iris_pty`, syncs `iris_fshost` (`IRIS_FS_OP_SYNC`), locks/zeroizes `iris_crypt` (`IRIS_CRYPT_OP_LOCK`), and closes driver channels so `virtio_gpu` and `virtio_blk` unpin all BTI PMTs before `zx_system_powerctl()`.
* **Full Service Supervision:** `iris_init` supervises `virtio_blk`, `virtio_gpu`, `iris_crypt`, `iris_pty`, `iris_fshost`, `iris_compositor`, and `IRIS_PROTO_SYSTEM` via `zx_object_wait_many`.
* **Passphrase Hardening:** `iris_crypt` always runs with `--wait-unlock` (no empty-passphrase dev fallback); `iris_compositor` displays a centered graphical unlock prompt (`--unlock-prompt`) while `iris_fshost` defers `/Users` & `/Applications` mounting until unlock.
* **Subsystem Fixes:** Implemented `IRIS_FS_OP_RENAME` & `IRIS_FS_OP_TRUNCATE` (`sys_rename`, `sys_ftruncate`) in `iris_fshost` + `mlibc`, added `aarch64` PL031 RTC (`0x09010000`) sync in `iris_init`, and removed unused `IRIS_COMP_OP_*` / `IRIS_CRYPT_OP_{READ,WRITE}` / `IRIS_PTY_OP_SET_ATTR` opcodes from `ipc.h`.
