/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/mlibc-sysdeps/iris/generic/entry.cpp
 * Purpose: C runtime initialization and main() invocation for irisOS processes.
 *
 * Licensing: MIT License. Permissive Zone 1 sysdeps source.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <bits/ensure.h>
#include <mlibc/tcb.hpp>
#include <mlibc/all-sysdeps.hpp>

#include <zircon/types.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#ifndef PA_FD
#define PA_FD 0x30u
#endif

#include <iris/fd_table.h>

extern "C" void iris_vdso_init(uintptr_t base);
extern "C" void iris_init_startup_handles(size_t nhandles, const zx_handle_t handles[], const uint32_t handle_info[]);
extern "C" void iris_set_debuglog(zx_handle_t handle);
extern "C" void iris_set_vdso_vmo(zx_handle_t handle);
extern "C" void iris_set_bootfs_vmo(zx_handle_t handle);
extern "C" void iris_set_process_handles(zx_handle_t proc, zx_handle_t thrd, zx_handle_t vmar, zx_handle_t job);

extern "C" [[gnu::weak]] void __dlapi_enter(uintptr_t *);

#include <elf.h>

extern char **environ;

extern "C" __attribute__((visibility("hidden"))) char __ehdr_start[];
extern "C" __attribute__((weak, visibility("hidden"))) Elf64_Dyn _DYNAMIC[];
extern "C" __attribute__((weak, visibility("hidden"))) void (*__init_array_start[])(void);
extern "C" __attribute__((weak, visibility("hidden"))) void (*__init_array_end[])(void);

static void self_relocate() {
    uintptr_t base = reinterpret_cast<uintptr_t>(__ehdr_start);
    if (!base || reinterpret_cast<uintptr_t>(_DYNAMIC) == 0)
        return;

    const Elf64_Dyn* dyn = _DYNAMIC;
    const Elf64_Rela* rela = nullptr;
    size_t relasz = 0;
    size_t relaent = sizeof(Elf64_Rela);

    for (const Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_RELA) {
            rela = reinterpret_cast<const Elf64_Rela*>(base + d->d_un.d_ptr);
        } else if (d->d_tag == DT_RELASZ) {
            relasz = d->d_un.d_val;
        } else if (d->d_tag == DT_RELAENT) {
            relaent = d->d_un.d_val;
        }
    }

    if (rela && relasz > 0) {
        size_t count = relasz / relaent;
        for (size_t i = 0; i < count; i++) {
            const Elf64_Rela* r = reinterpret_cast<const Elf64_Rela*>(
                reinterpret_cast<const uint8_t*>(rela) + i * relaent);
            uint32_t type = static_cast<uint32_t>(r->r_info & 0xffffffff);
#if defined(__x86_64__)
            if (type == 8 /* R_X86_64_RELATIVE */) {
                uint64_t* target = reinterpret_cast<uint64_t*>(base + r->r_offset);
                *target = base + r->r_addend;
            }
#elif defined(__aarch64__)
            if (type == 1027 /* R_AARCH64_RELATIVE */) {
                uint64_t* target = reinterpret_cast<uint64_t*>(base + r->r_offset);
                *target = base + r->r_addend;
            }
#endif
        }
    }
}

static void run_init_array() {
    if (reinterpret_cast<uintptr_t>(__init_array_start) == 0 ||
        reinterpret_cast<uintptr_t>(__init_array_end) == 0)
        return;
    size_t count = __init_array_end - __init_array_start;
    for (size_t i = 0; i < count; i++) {
        if (__init_array_start[i]) {
            __init_array_start[i]();
        }
    }
}

/**
 * @brief Primary irisOS C runtime entry point.
 *
 * Invoked by crt1.S (_start) with register arguments provided by zx_process_start():
 *  - start_arg: Channel capability handle containing the zx_proc_args_t bootstrap message.
 *  - vdso_base: Virtual address where the kernel mapped the vDSO (libzircon.so).
 *  - main_fn:   Function pointer to user application's main().
 */
extern "C" [[noreturn]] void __iris_start(void* start_arg, void* vdso_base, int (*main_fn)(int argc, char *argv[], char *env[])) {
    /* 0. Self-relocate static-PIE binary if loaded at a non-zero base */
    self_relocate();

    /* 1. Initialize vDSO dynamic system call dispatch from mapped libzircon.so */
    if (vdso_base != nullptr) {
        iris_vdso_init(reinterpret_cast<uintptr_t>(vdso_base));
    }

    /* 2. Process bootstrap channel if valid */
    int argc = 1;
    static char app_name[] = "iris_app";
    static char* default_argv[] = { app_name, nullptr };
    char** argv = default_argv;

    static char* default_env[] = { nullptr };
    if (!environ) {
        environ = default_env;
    }

    zx_handle_t bstrap = static_cast<zx_handle_t>(reinterpret_cast<uintptr_t>(start_arg));
    /* A valid handle is non-zero and fits in 32 bits (unlike user virtual stack pointers) */
    if (bstrap != ZX_HANDLE_INVALID && (reinterpret_cast<uintptr_t>(start_arg) < 0x100000000ULL)) {
        alignas(8) uint8_t msg_buf[4096];
        zx_handle_t handles[64];
        uint32_t act_bytes = 0;
        uint32_t act_handles = 0;

        zx_status_t status = zx_channel_read(
            bstrap, 0, msg_buf, handles, sizeof(msg_buf),
            64, &act_bytes, &act_handles);

        if (status == ZX_OK && act_bytes >= sizeof(zx_proc_args_t)) {
            const zx_proc_args_t* pargs = reinterpret_cast<const zx_proc_args_t*>(msg_buf);
            if (pargs->protocol == ZX_PROCARGS_PROTOCOL && pargs->version == ZX_PROCARGS_VERSION) {
                const uint32_t* info_table = reinterpret_cast<const uint32_t*>(msg_buf + pargs->handle_info_off);
                iris_init_startup_handles(act_handles, handles, info_table);

                zx_handle_t proc_self = ZX_HANDLE_INVALID;
                zx_handle_t thrd_self = ZX_HANDLE_INVALID;
                zx_handle_t vmar_root = ZX_HANDLE_INVALID;
                zx_handle_t job_def   = ZX_HANDLE_INVALID;

                iris_fd_table_init();

                for (uint32_t i = 0; i < act_handles; i++) {
                    uint32_t info = info_table[i];
                    zx_handle_t h = handles[i];
                    uint32_t htype = PA_HND_TYPE(info);

                    if (htype == PA_FD) {
                        int target_fd = static_cast<int>(PA_HND_ARG(info));
                        iris_fd_type_t ftype = (target_fd <= 2) ? IRIS_FD_DEBUGLOG : IRIS_FD_CHANNEL;
                        iris_fd_bind(target_fd, h, ftype, (target_fd == 0) ? O_RDONLY : O_WRONLY);
                    } else if (info == PA_HND(PA_FDIO_LOGGER, 0)) {
                        iris_set_debuglog(h);
                    } else if (info == PA_HND(PA_PROC_SELF, 0)) {
                        proc_self = h;
                    } else if (info == PA_HND(PA_THREAD_SELF, 0)) {
                        thrd_self = h;
                    } else if (info == PA_HND(PA_VMAR_ROOT, 0)) {
                        vmar_root = h;
                    } else if (info == PA_HND(PA_JOB_DEFAULT, 0)) {
                        job_def = h;
                    } else if (info == PA_HND(PA_VMO_VDSO, 0)) {
                        iris_set_vdso_vmo(h);
                    } else if (info == PA_HND(PA_VMO_BOOTFS, 0)) {
                        iris_set_bootfs_vmo(h);
                    }
                }
                iris_set_process_handles(proc_self, thrd_self, vmar_root, job_def);
            }
        }
        zx_handle_close(bstrap);
    }

    /* 3. Setup initial thread control block (TCB) for TLS / thread safety */
    static Tcb initial_tcb;
    initial_tcb.selfPointer = &initial_tcb;
    initial_tcb.tid = 1;
    mlibc::sys_tcb_set(&initial_tcb);

    /* 4. Execute global initializers / C++ constructors (.init_array) */
    run_init_array();

    /* 5. Execute application entry point */
    int result = main_fn(argc, argv, environ);

    /* 6. Clean process termination */
    fflush(nullptr);
    mlibc::sys_exit(result);
}

/**
 * Compatibility fallback for standard mlibc __mlibc_entry.
 */
extern "C" void __mlibc_entry(uintptr_t *entry_stack, int (*main_fn)(int argc, char *argv[], char *env[])) {
    if (__dlapi_enter) {
        __dlapi_enter(entry_stack);
    }
    __iris_start(nullptr, nullptr, main_fn);
}
