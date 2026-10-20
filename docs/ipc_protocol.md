# irisOS IPC Wire Protocol & Native SDK Specification

## 1. Overview
In **irisOS**, all Ring 3 user-space servers (`iris_fshost`, `iris_crypt`, `iris_compositor`, `iris_pty`), device drivers, system utilities, and user applications communicate across process boundaries using raw **Zircon channels** (`zx_channel_*`) governed by a standardized **24-byte C-ABI header** (`iris_msg_hdr_t`).

Higher-layer Google Fuchsia frameworks—including FIDL, Scenic, Dart, Flutter, and the Fuchsia Component Framework—are strictly excluded per Section 2 of `AGENTS.md`. Instead, irisOS relies on this high-performance, deterministic, zero-copy-capable message protocol.

---

## 2. Wire Format Specification

### 2.1 Binary Layout (`iris_msg_hdr_t`)
The header is declared in [`sdk/libiris/include/iris/ipc.h`](../sdk/libiris/include/iris/ipc.h) and packed with `#pragma pack(push, 4)`. Compile-time static assertions (`_Static_assert(sizeof(iris_msg_hdr_t) == 24)`) enforce its exact size across both `x86_64` and `aarch64`.

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      magic (0x49524953)                       | 0x00
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           protocol                            | 0x04
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                            opcode                             | 0x08
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                             txid                              | 0x0C
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                            status                             | 0x10
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          payload_len                          | 0x14
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|             Trailing Data Payload (payload_len bytes)         | 0x18...
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### 2.2 Header Fields
| Offset | Field | Type | Description |
|---|---|---|---|
| `0x00` | `magic` | `uint32_t` | Magic identifier. Must equal `IRIS_IPC_MAGIC` (`0x49524953`, ASCII `'IRIS'`). Messages failing this check are rejected with `ZX_ERR_INVALID_ARGS`. |
| `0x04` | `protocol` | `uint32_t` | Subsystem protocol identifier (`IRIS_PROTO_*`). Dispatches messages to the appropriate subsystem handler. |
| `0x08` | `opcode` | `uint32_t` | Command or event opcode defined within the protocol's opcode space. |
| `0x0C` | `txid` | `uint32_t` | Transaction ID. Set by the request initiator; replies must echo the identical `txid` to correlate responses. |
| `0x10` | `status` | `int32_t` | Status code (`zx_status_t`). Zero (`ZX_OK`) indicates success; negative values indicate Zircon standard errors (`ZX_ERR_*`). |
| `0x14` | `payload_len` | `uint32_t` | Byte length of the trailing payload data. The maximum payload per channel message is `65,512` bytes (`65536 - 24`). |

---

## 3. Protocol Spaces & Standard Opcodes

### 3.1 Subsystem Protocols (`protocol`)
```c
#define IRIS_PROTO_SYSTEM     0x0001u // Core lifecycle, ping/pong, heartbeat
#define IRIS_PROTO_FS         0x0002u // VFS mount router & file operations (iris_fshost)
#define IRIS_PROTO_CRYPT      0x0003u // Block cipher & partition unlock (iris_crypt)
#define IRIS_PROTO_COMPOSITOR 0x0004u // Zero-copy window compositor (iris_compositor)
#define IRIS_PROTO_PTY        0x0005u // Pseudo-terminal server & signals (iris_pty)
#define IRIS_PROTO_DRIVER     0x0006u // Ring 3 device drivers & PCIe enumeration
```

### 3.2 Standard Opcodes (`opcode`)
#### IRIS_PROTO_SYSTEM (`0x0001`)
* `IRIS_SYS_OP_PING` (`0x0001`): Liveness check. Peer replies with `IRIS_SYS_OP_PONG`.
* `IRIS_SYS_OP_PONG` (`0x0002`): Response to ping containing peer timestamp.
* `IRIS_SYS_OP_SHUTDOWN` (`0x0003`): Initiate clean system power-off.
* `IRIS_SYS_OP_REBOOT` (`0x0004`): Initiate system reboot.
* `IRIS_SYS_OP_GET_VERSION` (`0x0005`): Query kernel and user-space version string.
* `IRIS_SYS_OP_HEARTBEAT` (`0x0006`): Watchdog heartbeat signal.

#### IRIS_PROTO_FS (`0x0002`)
* `IRIS_FS_OP_OPEN` (`0x0010`): Open a file or directory node. Returns a channel endpoint to the file.
* `IRIS_FS_OP_CLOSE` (`0x0011`): Release file descriptor channel.
* `IRIS_FS_OP_READ` (`0x0012`): Read file content into payload.
* `IRIS_FS_OP_WRITE` (`0x0013`): Write payload content into file.
* `IRIS_FS_OP_SEEK` (`0x0014`): Update seek position.
* `IRIS_FS_OP_STAT` (`0x0015`): Query POSIX stat metadata.
* `IRIS_FS_OP_READDIR` (`0x0016`): Read directory contents.
* `IRIS_FS_OP_MKDIR` (`0x0017`): Create a new directory.
* `IRIS_FS_OP_UNLINK` (`0x0018`): Delete a file or empty directory.
* `IRIS_FS_OP_RENAME` (`0x0019`): Rename or move a filesystem path.
* `IRIS_FS_OP_TRUNCATE` (`0x001A`): Resize file to specified length.
* `IRIS_FS_OP_SYNC` (`0x001B`): Flush dirty cache blocks to disk.

#### IRIS_PROTO_CRYPT (`0x0003`)
* `IRIS_CRYPT_OP_STATUS` (`0x0020`): Query partition lock state and Sector 0 header metadata.
* `IRIS_CRYPT_OP_UNLOCK` (`0x0021`): Derive master key with Argon2 and unlock Data partition.
* `IRIS_CRYPT_OP_LOCK` (`0x0022`): Zero master key and lock storage.
* `IRIS_CRYPT_OP_READ` (`0x0023`): Read and decrypt blocks via AES-256-XTS.
* `IRIS_CRYPT_OP_WRITE` (`0x0024`): Encrypt and write blocks via AES-256-XTS.

#### IRIS_PROTO_COMPOSITOR (`0x0004`)
* `IRIS_COMP_OP_CREATE` (`0x0030`): Allocate client window surface.
* `IRIS_COMP_OP_DESTROY` (`0x0031`): Release window surface.
* `IRIS_COMP_OP_ATTACH_VMO` (`0x0032`): Transfer pixel buffer VMO to compositor.
* `IRIS_COMP_OP_DAMAGE` (`0x0033`): Send bounding box rectangle update.
* `IRIS_COMP_OP_PRESENT` (`0x0034`): Trigger surface composite at next vblank.
* `IRIS_COMP_OP_POLL_EVENT` (`0x0035`): Fetch keyboard or mouse input events.

#### IRIS_PROTO_PTY (`0x0005`)
* `IRIS_PTY_OP_READ` (`0x0040`): Read buffered input from terminal.
* `IRIS_PTY_OP_WRITE` (`0x0041`): Output text stream to terminal.
* `IRIS_PTY_OP_SET_WINDOW` (`0x0042`): Update row and column dimensions.
* `IRIS_PTY_OP_GET_WINDOW` (`0x0043`): Query terminal dimensions.
* `IRIS_PTY_OP_DRAIN` (`0x0044`): Flush pending I/O buffers.
* `IRIS_PTY_OP_SIGNAL` (`0x0045`): Send POSIX signal (`SIGINT`, `SIGWINCH`).

#### IRIS_PROTO_DRIVER (`0x0006`)
* `IRIS_BLK_OP_GET_INFO` (`0x0050`): Query block device geometry (sector count and sector size).
* `IRIS_BLK_OP_READ` (`0x0051`): Read contiguous sectors via DMA or encrypted filter.
* `IRIS_BLK_OP_WRITE` (`0x0052`): Write contiguous sectors via DMA or encrypted filter.
* `IRIS_BLK_OP_FLUSH` (`0x0053`): Flush write cache to non-volatile storage.

---

## 4. Capability Model & Handle Transfer

### 4.1 Handle Ownership Semantics
Zircon channels support transferring up to **64 handles** in a single message (`ZX_CHANNEL_MAX_MSG_HANDLES`).
* **Sending (`iris_channel_send` / `iris_channel_call`):**
  When passing an array of `zx_handle_t` to send primitives, ownership is transferred to the channel peer on `ZX_OK`. If the send fails, ownership remains with the sender.
* **Receiving (`iris_channel_recv` / `iris_channel_call`):**
  On `ZX_OK`, handles populated into `out_handles` are ACQUIRED by the caller. The caller is responsible for eventually closing them with `zx_handle_close()`.

### 4.2 File Descriptor Inheritance (`PA_HND(PA_FD, fd)`)
Per Section 4.6 of `AGENTS.md`, file descriptors are inherited across process boundaries using Zircon's standard process argument protocol (`<zircon/processargs.h>`).
Each inherited file descriptor is assigned the handle info tag:
```c
#define PA_FD 0x30u
uint32_t info = PA_HND(PA_FD, target_fd);
```
When `iris_spawn()` executes, it writes these tags into the bootstrap message, enabling the child's `mlibc-sysdeps` layer to populate its internal table mapping `int fd -> zx_handle_t channel`.

---

## 5. Native SDK (`libiris`) API Reference

The C API is exposed via [`sdk/libiris/include/iris/iris.h`](../sdk/libiris/include/iris/iris.h):

### 5.1 Channel Primitives ([`channel.h`](../sdk/libiris/include/iris/channel.h))
* `iris_channel_create(uint32_t flags, zx_handle_t* out_ch0, zx_handle_t* out_ch1)`: Creates a bidirectional channel pair.
* `iris_channel_send(zx_handle_t channel, const iris_msg_hdr_t* hdr, const void* payload, const zx_handle_t* handles, uint32_t num_handles)`: Sends an IPC message and optional handles. Uses an internal 8 KB stack buffer for speed, falling back to temporary VMO mapping for payloads up to 64 KB without requiring a libc heap.
* `iris_channel_recv(zx_handle_t channel, iris_msg_hdr_t* out_hdr, void* out_payload, uint32_t max_payload_len, uint32_t* actual_payload_len, zx_handle_t* out_handles, uint32_t max_handles, uint32_t* actual_handles)`: Reads a message, validates `IRIS_IPC_MAGIC`, unpacks payload, and acquires handles.
* `iris_channel_call(zx_handle_t channel, const iris_msg_hdr_t* req_hdr, ...)`: Performs a synchronous request/response transaction using `zx_channel_call()`.
* `iris_channel_reply(...)`: Helper to build and transmit a matching response with identical `txid`.

### 5.2 Virtual Memory Objects ([`vmo.h`](../sdk/libiris/include/iris/vmo.h))
* `iris_vmo_create(size_t size, uint32_t options, zx_handle_t* out_vmo)`: Allocates an anonymous VMO.
* `iris_vmo_map(zx_handle_t vmo, zx_vm_option_t options, uintptr_t* out_addr, size_t* out_mapped_size)`: Maps the entire VMO into current process root VMAR.
* `iris_vmo_map_offset(...)`: Maps a sub-range into an explicit VMAR.
* `iris_vmo_unmap(uintptr_t addr, size_t size)`: Unmaps virtual memory.
* `iris_vmo_duplicate(zx_handle_t vmo, zx_rights_t rights, zx_handle_t* out_handle)`: Duplicates a VMO handle with specific rights (e.g. read-only permissions for window sharing).

### 5.3 Process Spawning ([`spawn.h`](../sdk/libiris/include/iris/spawn.h))
* `iris_spawn(const iris_spawn_args_t* args, zx_handle_t* out_process, zx_handle_t* out_thread)`:
  * Creates process and initial thread via `zx_process_create()` and `zx_thread_create()`.
  * Maps initial stack VMO and vDSO.
  * Encodes `zx_proc_args_t` message with `argv`, `envp`, core capabilities (`PA_PROC_SELF`, `PA_THREAD_SELF`, `PA_VMAR_ROOT`, `PA_VMO_STACK`, `PA_VMO_VDSO`, `PA_JOB_DEFAULT`), and file descriptors (`PA_HND(PA_FD, target_fd)`).
  * Writes to bootstrap channel and launches execution via `zx_process_start()`.
* `iris_spawn_simple(...)`: Convenience wrapper for minimal process creation.

### 5.4 Runtime Capability Accessors ([`init.h`](../sdk/libiris/include/iris/init.h))
* `iris_init_runtime(zx_handle_t proc, zx_handle_t vmar, zx_handle_t job)`: Sets process-wide capability handles.
* `iris_process_self()`, `iris_vmar_root_self()`, `iris_job_default()`: Accessors with weak fallback symbols resolving `zx_process_self()`, `zx_vmar_root_self()`, and `zx_job_default()`.

---

## 6. Verification & Test Suite
The standalone verification binary [`sdk/libiris/tests/test_libiris.c`](../sdk/libiris/tests/test_libiris.c) validates:
1. Static field offsets and 24-byte alignment guarantees.
2. `iris_msg_hdr_init()` and `iris_msg_hdr_init_reply()` request/reply pairing.
3. `iris_msg_hdr_is_valid()` protocol filtering and bounds checks.
4. `PA_HND(PA_FD, fd)` encoding for file descriptors 0, 1, and 2.
5. Clean dynamic and static symbol linkage against `libzircon` on both `x86_64` and `aarch64`.
