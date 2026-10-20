# irisOS mlibc Sysdeps Architecture

## 1. Overview & Architectural Role

Per Section 4.6 and Section 3 of `AGENTS.md`, **irisOS** uses **`mlibc`** as its standard C runtime and dynamic linker. The C library and dynamic linker (`libc.so`, `ld-iris.so`, `libc.a`) sit in **Zone 1** (Permissive SDK / System Libraries) under the **MIT License**, ensuring proprietary and open-source applications alike can link against standard POSIX C APIs without licensing contamination.

The `mlibc` sysdeps layer bridges standard C library calls (`malloc`, `printf`, `exit`, `pthread`, `mmap`) to the underlying Zircon microkernel primitives and irisOS user-space servers (`iris_fshost`, `iris_pty`).

The sysdeps source tree is located in:
```text
sdk/mlibc-sysdeps/iris/
├── crt-x86_64/              # x86_64 C runtime startup (crt1.S, crti.S, crtn.S)
├── crt-aarch64/             # AArch64 C runtime startup (crt1.S, crti.S, crtn.S)
├── generic/
│   ├── entry.cpp            # Process entry point __mlibc_entry invoking main()
│   ├── generic.cpp          # Core sysdeps (memory, logging, exit, futex, TCB, VFS)
│   ├── thread.cpp           # Thread stack allocation & __mlibc_enter_thread
│   ├── thread_entry_x86_64.S# x86_64 thread register unpacking trampoline
│   └── thread_entry_aarch64.S# AArch64 thread register unpacking trampoline
├── include/
│   ├── abi-bits/            # POSIX standard ABI headers (53 headers)
│   ├── iris/
│   │   └── fd_table.h       # Per-process client-side FD table & channel mappings
│   └── mlibc/
│       └── thread-entry.hpp # Internal thread declarations
└── meson.build              # Meson build definitions for sysdeps
```

---

## 2. Implemented Subsystems

### 2.1 Memory Management (VMOs & VMARs)
* **`sys_anon_allocate(size_t size, void **pointer)`**:
  Allocates anonymous memory using `zx_vmo_create(size, 0, &vmo)` followed by `zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, ...)`. Closes the temporary VMO handle immediately since the mapping retains the underlying physical memory reference.
* **`sys_anon_free(void *pointer, size_t size)`**:
  Unmaps virtual memory from the root VMAR via `zx_vmar_unmap(zx_vmar_root_self(), ...)`.
* **`sys_vm_map(void *hint, size_t size, int prot, int flags, int fd, off_t offset, void **window)`**:
  Translates standard POSIX `PROT_*` and `MAP_*` flags to Zircon `ZX_VM_PERM_*` and `ZX_VM_SPECIFIC`, creates an anonymous VMO (or maps an existing handle in future VFS stages), and maps it into the address space.
* **`sys_vm_unmap(void *pointer, size_t size)`**:
  Delegates directly to `sys_anon_free`.

### 2.2 Client-Side File Descriptor Table (`include/iris/fd_table.h`)
Each process maintains an internal 256-slot table mapping `int fd -> zx_handle_t`:
* `IRIS_FD_CONSOLE`: Bound to standard streams (0, 1, 2) or `/dev/console`, routing writes to serial debuglog (`zx_debuglog_write()`).
* `IRIS_FD_VFS`: Bound to files and directories mounted by `iris_fshost`, carrying a server-side VFS handle.
* `IRIS_FD_DEV_NULL` & `IRIS_FD_DEV_ZERO`: Pseudo-devices for `/dev/null` and `/dev/zero`.

### 2.3 VFS File Syscalls over `IRIS_PROTO_FS`
POSIX file syscalls query the central VFS router (`iris_fshost`) via the inherited channel `PA_HND(PA_NS_DIR, 0)`:
* **`sys_open(const char *pathname, int flags, mode_t mode, int *fd)`**: Issues `IRIS_FS_OP_OPEN`. Expands `/bin` and `/lib` symlinks, opens existing files, or creates new files on `/Users` / `/Applications`. Rejects `/System` write attempts with `EROFS`.
* **`sys_read(int fd, void *buf, size_t count, ssize_t *bytes_read)`**: Routes to `iris_fshost` via `IRIS_FS_OP_READ`.
* **`sys_write(int fd, const void *buf, size_t count, ssize_t *bytes_written)`**: Routes console fds (1, 2) to serial debuglog, and regular file fds to `iris_fshost` via `IRIS_FS_OP_WRITE`.
* **`sys_seek(int fd, off_t offset, int whence, off_t *new_offset)`**: Updates file position via `IRIS_FS_OP_SEEK`.
* **`sys_close(int fd)`**: Issues `IRIS_FS_OP_CLOSE` and releases the client FD table entry.
* **`sys_stat(const char *pathname, struct stat *statbuf)`** & **`sys_fstat(int fd, struct stat *statbuf)`**: Queries UFS2 dinode metadata via `IRIS_FS_OP_STAT`.

### 2.4 Process & Thread Control
* **`sys_exit(int status)`**: Invokes `zx_process_exit((int64_t)status)`.
* **`sys_thread_exit()`**: Invokes `zx_thread_exit()`.
* **`sys_tcb_set(void *pointer)`**:
  Configures the Thread Control Block (TCB) base pointer for Thread Local Storage (TLS):
  * **x86_64:** Calls `zx_object_set_property(zx_thread_self(), ZX_PROP_REGISTER_FS, &pointer, sizeof(uintptr_t))` to program the `%fs` segment base register.
  * **AArch64:** Computes `pointer + sizeof(Tcb) - 0x10` and executes `msr tpidr_el0, %0` directly in EL0 user space.
* **`sys_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, ...)`**:
  Implements POSIX process creation over `iris_spawn` without requiring copy-on-write `fork()`. Propagates inherited file descriptors via `PA_HND(PA_FD, target_fd)` into the child's `zx_proc_args_t` bootstrap message.

### 2.5 Fast User-Space Synchronization (Futex)
* **`sys_futex_wait(int *pointer, int expected, const struct timespec *time)`**:
  Maps POSIX `timespec` deadlines to Zircon nanosecond timestamps (`zx_duration_t`) and issues `zx_futex_wait((const zx_futex_t*)pointer, expected, deadline)`. Correctly maps `ZX_ERR_BAD_STATE` to `EAGAIN` and `ZX_ERR_TIMED_OUT` to `ETIMEDOUT`.
* **`sys_futex_wake(int *pointer)`**:
  Issues `zx_futex_wake((const zx_futex_t*)pointer, 1)` to awaken waiting threads.

### 2.6 C Runtime Entry & Process Trampoline (`crt1.o`)
* **`crt1.S` (`_start`)**:
  Freestanding assembly entry point for user processes.
  * **x86_64:** Sets `%rdi = %rsp` (entry stack pointer) and `%rsi = $main`, then calls `__mlibc_entry`.
  * **AArch64:** Sets `x0 = sp` and `x1 = main` via `adrp`/`add`, then branches to `__mlibc_entry`.
* **`entry.cpp` (`__mlibc_entry`)**:
  Receives the initial stack and `main` function pointer, invokes `__dlapi_enter()` for dynamic linker setup if dynamically linked, executes `main(argc, argv, envp)`, and terminates via `exit(result)`.

---

## 3. Toolchain & Cross-Build Integration

Cross-compilation is orchestrated via Meson cross-files (`build/toolchains/meson-x86_64.txt` and `meson-arm64.txt`) using Homebrew LLVM (`clang`, `clang++`, `ld.lld`, `llvm-ar`):

1. **Target Triples:**
   * `x86_64-unknown-linux-gnu` (x86_64 System V AMD64 ELF)
   * `aarch64-unknown-linux-gnu` (AArch64 standard 64-bit ELF)
   Both configurations use `-nostdlib -nostdinc++ -ffreestanding -D_GNU_SOURCE` ensuring 100% pure freestanding builds with zero host contamination.
2. **Microkernel Stubs Linking:**
   The static stub library `libzircon.a` from `out/build-${arch}/lib/` is linked into both `ld-iris.so` and `libc.so`, providing direct, self-contained system calls (`zx_vmo_create`, `zx_vmar_map`, `zx_debuglog_write`, etc.) without dynamic loader dependencies.
3. **Compiler Builtins (`compiler-rt`):**
   A dedicated builtins archive `libclang_rt.builtins-${arch}.a` compiled from LLVM `compiler-rt/lib/builtins` provides soft-float quadruple precision (128-bit `long double` for AArch64) and C99 complex multiplication routines.
4. **Master Orchestrator Targets:**
   * `make -C build mlibc-x86_64`: Builds static `libc.a` and dynamic `libc.so` + `ld-iris.so` for x86_64.
   * `make -C build mlibc-aarch64`: Builds static `libc.a` and dynamic `libc.so` + `ld-iris.so` for aarch64.
   * `make -C build mlibc`: Builds both architectures.
