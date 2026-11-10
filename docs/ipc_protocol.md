# irisOS IPC Wire Protocol & Native SDK Specification

## 1. 24-Byte C-ABI Wire Header (`iris_msg_hdr_t`)
All Ring 3 servers, drivers, and client libraries communicate over raw Zircon channels (`zx_channel_*`) using the packed 24-byte header defined in [`sdk/libiris/include/iris/ipc.h`](../sdk/libiris/include/iris/ipc.h):

| Offset | Field | Type | Description |
| :--- | :--- | :--- | :--- |
| `0x00` | `magic` | `uint32_t` | Must equal `IRIS_IPC_MAGIC` (`0x49524953`, ASCII `'IRIS'`). |
| `0x04` | `protocol` | `uint32_t` | Subsystem ID (`IRIS_PROTO_*`). |
| `0x08` | `opcode` | `uint32_t` | Command or event opcode within the protocol space. |
| `0x0C` | `txid` | `uint32_t` | Transaction ID echoed in replies to pair requests/responses. |
| `0x10` | `status` | `int32_t` | `zx_status_t` (`ZX_OK = 0`, negative `ZX_ERR_*`, or `IRIS_ERR_READ_ONLY = -56`). |
| `0x14` | `payload_len` | `uint32_t` | Trailing payload bytes (`0 .. 65,512`). Up to 64 handles may accompany a message. |

---

## 2. Protocol Spaces & Opcodes

### 2.1 `IRIS_PROTO_SYSTEM` (`0x0001`)
* `IRIS_SYS_OP_PING` (`0x0001`) / `IRIS_SYS_OP_PONG` (`0x0002`): Liveness echo and peer timestamp.
* `IRIS_SYS_OP_SHUTDOWN` (`0x0003`) / `IRIS_SYS_OP_REBOOT` (`0x0004`): System power control.
* `IRIS_SYS_OP_GET_VERSION` (`0x0005`) / `IRIS_SYS_OP_HEARTBEAT` (`0x0006`): Version query and watchdog heartbeat.

### 2.2 `IRIS_PROTO_FS` (`0x0002` — `iris_fshost`)
* `IRIS_FS_OP_OPEN` (`0x0010`), `CLOSE` (`0x0011`), `READ` (`0x0012`), `WRITE` (`0x0013`), `SEEK` (`0x0014`), `STAT` (`0x0015`), `READDIR` (`0x0016`), `MKDIR` (`0x0017`), `UNLINK` (`0x0018`), `RENAME` (`0x0019`), `TRUNCATE` (`0x001A`), `SYNC` (`0x001B`), `READLINK` (`0x001C`).
* `IRIS_FS_OP_OPEN_CLIENT` (`0x001D`): Allocates a dedicated VFS client channel endpoint (`handles[0]`).
* `IRIS_FS_OP_CONFIRM_BOOT` (`0x001E`): Marks the active GPT slot (`System_A` or `System_B`) `boot_successful = 1`.
* `IRIS_FS_OP_GET_EXEC_VMO` (`0x001F`): Returns a duplicated read-only executable VMO (`handles[0]`) for an immutable `/System` ELF binary in **1 IPC round-trip**.

### 2.3 `IRIS_PROTO_CRYPT` (`0x0003` — `iris_crypt`)
* `IRIS_CRYPT_OP_STATUS` (`0x0020`): Queries lock state, Sector 0 header validity, and usable logical sectors.
* `IRIS_CRYPT_OP_UNLOCK` (`0x0021`): Derives KEK via Argon2id and unwraps the 512-bit AES-256-XTS master key.
* `IRIS_CRYPT_OP_LOCK` (`0x0022`): Zeroes the master key in RAM and locks `Data`.
* `IRIS_CRYPT_OP_READ` (`0x0023`) / `WRITE` (`0x0024`): Encrypted block I/O.

### 2.4 `IRIS_PROTO_COMPOSITOR` (`0x0004` — `iris_compositor`)
* `IRIS_COMPOSITOR_OP_CREATE_WINDOW` (`0x0030`): Registers a client window with shared pixel VMO (`handles[0]`) and returns `window_id`.
* `IRIS_COMPOSITOR_OP_DESTROY_WINDOW` (`0x0031`): Unmaps and closes a window surface.
* `IRIS_COMPOSITOR_OP_DAMAGE` (`0x0032`): Marks client-relative sub-rectangle `(x, y, w, h)` dirty for the next 60 Hz frame tick.
* `IRIS_COMPOSITOR_OP_MOVE_WINDOW` (`0x0033`): Moves outer window frame to desktop `(x, y)`.
* `IRIS_COMPOSITOR_OP_OPEN_CLIENT` (`0x0034`): Allocates a dedicated compositor client channel (`handles[0]`).
* `IRIS_COMPOSITOR_OP_POLL_EVENT` (`0x0035`): Returns window-relative mouse coordinates, buttons, and queued keystrokes (`iris_compositor_poll_event_resp_t`).

### 2.5 `IRIS_PROTO_PTY` (`0x0005` — `iris_pty`)
* `IRIS_PTY_OP_READ` (`0x0040`) / `WRITE` (`0x0041`): Terminal stream I/O (supports deferred blocking reads).
* `IRIS_PTY_OP_SET_WINDOW` (`0x0042`) / `GET_WINDOW` (`0x0043`): Terminal dimensions (`iris_pty_window_t`).
* `IRIS_PTY_OP_DRAIN` (`0x0044`), `SIGNAL` (`0x0045`, e.g., `SIGINT = 2`), `SET_MODE` (`0x0046`), `GET_MODE` (`0x0047`), `INJECT` (`0x0048`), `OPEN_CLIENT` (`0x0049`).
* Mode flags: `IRIS_PTY_MODE_CANONICAL` (`1 << 0`), `IRIS_PTY_MODE_ECHO` (`1 << 1`), `IRIS_PTY_MODE_RAW` (`0`).

### 2.6 `IRIS_PROTO_DRIVER` (`0x0006` — `virtio_blk`, `virtio_gpu`, `gop_fb`)
* **Block Storage:** `IRIS_BLK_OP_GET_INFO` (`0x0050`), `READ` (`0x0051`), `WRITE` (`0x0052`), `FLUSH` (`0x0053`).
* **Display Scanout:** `IRIS_GPU_OP_GET_INFO` (`0x0060`, returns `iris_gpu_info_t` + shared framebuffer VMO at `handles[0]`), `IRIS_GPU_OP_FLUSH` (`0x0061`, transfers damaged `(x, y, width, height)` rectangle and flushes scanout).

---

## 3. Native SDKs (`libiris` & `libiris_gui`)
* **`sdk/libiris` (`<iris/iris.h>`):**
  * `iris_channel_{create,send,recv,call,reply}`: Zero-malloc IPC helpers (stack fast-path up to 8 KB, VMO fallback up to 64 KB).
  * `iris_vmo_{create,map,map_offset,unmap,duplicate}`: Shared memory management.
  * `iris_spawn` / `iris_spawn_simple`: ELF64/PIE process loader passing `argv`, `envp`, and `PA_HND(PA_FD, fd)` handles in `zx_proc_args_t`.
  * `iris_pty_*`: Client wrappers for PTY mode, window size, signal polling, and synthetic keystroke injection.
* **`sdk/libiris_gui` (`<iris/gui.h>`):**
  * `iris_gui_connect`, `iris_window_create`, `iris_window_destroy`, `iris_window_damage`, `iris_window_move`, `iris_window_poll_event`, `iris_window_get_surface` (`pixman_image_t*`).
