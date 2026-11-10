# irisOS `mlibc` Sysdeps & POSIX Runtime Architecture

## 1. Overview
`irisOS` uses **`mlibc`** (`MIT`, Zone 1 permissive) with custom system dependencies in [`sdk/mlibc-sysdeps/iris/`](../sdk/mlibc-sysdeps/iris/) to produce `libc.a`, `libc.so`, and `ld-iris.so`.

```text
sdk/mlibc-sysdeps/iris/
├── crt-x86_64/ & crt-aarch64/ # Freestanding _start (crt1.S, crti.S, crtn.S)
├── generic/
│   ├── entry.cpp              # Unpacks zx_proc_args_t, initializes FD table & invokes main()
│   ├── generic.cpp            # Memory, VFS, PTY, Pipe, Process/Spawn, Clock, and Futex sysdeps
│   ├── thread.cpp             # Thread stack allocation & __mlibc_enter_thread
│   └── thread_entry_*.S       # Per-arch thread register trampoline
└── include/iris/fd_table.h    # 256-slot client-side FD table & channel capabilities
```

---

## 2. Client-Side File Descriptor Table (`fd_table.h`)
Each user-space process maintains a 256-slot table mapping `int fd` to an `iris_fd_entry_t`:
* **`IRIS_FD_CONSOLE`:** Debug serial stream (`zx_debug_write` / `zx_debuglog_write`).
* **`IRIS_FD_VFS`:** Open file or directory slot on `iris_fshost` (`IRIS_PROTO_FS`).
* **`IRIS_FD_PTY`:** Interactive terminal session channel on `iris_pty` (`IRIS_PROTO_PTY`).
* **`IRIS_FD_PIPE_READ` / `IRIS_FD_PIPE_WRITE`:** Unidirectional POSIX pipe endpoints backed by Zircon channels (`sys_pipe`).
* **`IRIS_FD_DEV_NULL` / `IRIS_FD_DEV_ZERO`:** In-process pseudo-devices for `/dev/null` and `/dev/zero`.

During `sys_spawn()`, open descriptors and service channels (`PA_NS_DIR` for VFS, `PA_USER0` for PTY, `PA_USER1` for Compositor) are cloned via `IRIS_FS_OP_OPEN_CLIENT` / `IRIS_PTY_OP_OPEN_CLIENT` / `IRIS_COMPOSITOR_OP_OPEN_CLIENT` and passed to the child in `zx_proc_args_t` using `PA_HND(PA_FD, target_fd)`.

---

## 3. Implemented Sysdep Subsystems (`generic.cpp`)

| Category | Implemented `mlibc` Hooks | Underlying Mechanism |
| :--- | :--- | :--- |
| **Memory** | `sys_anon_allocate`, `sys_anon_free`, `sys_vm_map`, `sys_vm_unmap` | `zx_vmo_create` + `zx_vmar_map` / `zx_vmar_unmap` on `zx_vmar_root_self()` |
| **VFS Files & Dirs** | `sys_open`, `sys_open_dir`, `sys_read`, `sys_write`, `sys_seek`, `sys_close`, `sys_stat`, `sys_read_entries`, `sys_mkdir`, `sys_unlinkat`, `sys_rmdir`, `sys_rename`, `sys_readlink`, `sys_dup`, `sys_dup2`, `sys_fcntl`, `sys_fsync` | IPC transactions over `IRIS_PROTO_FS` to `iris_fshost` (enforces `EROFS` on `/System`) |
| **Pipes & TTY** | `sys_pipe`, `sys_isatty`, `sys_tcgetattr`, `sys_tcsetattr`, `sys_ioctl` (`TIOCGWINSZ`, `TIOCSWINSZ`) | Zircon channel pairs for pipes; `IRIS_PROTO_PTY` mode and window size calls for TTYs |
| **Process & Spawn** | `sys_spawn`, `sys_waitpid`, `sys_exit`, `sys_getpid`, `sys_getppid`, `sys_getuid`, `sys_uname` | **1-IPC Fast-Path (`IRIS_FS_OP_GET_EXEC_VMO`)** for `/System` ELF binaries, `#!` shebang interpreter expansion, and `zx_object_wait_one(ZX_PROCESS_TERMINATED)` + `zx_object_get_info` for `waitpid` |
| **Threads & TLS** | `sys_tcb_set`, `sys_prepare_stack`, `sys_clone`, `sys_thread_exit`, `sys_futex_wait`, `sys_futex_wake` | `ZX_PROP_REGISTER_FS` (`x86_64`) / `tpidr_el0` (`aarch64`), `zx_thread_create`/`start`, and `zx_futex_*` |
| **Time & Sleep** | `sys_clock_get`, `sys_sleep` | `zx_clock_get_monotonic()` and `zx_nanosleep()` |
