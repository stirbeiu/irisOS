/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/src/spawn.c
 * Purpose: User-space process spawning primitives wrapping zx_process_create/start and zx_proc_args_t.
 *
 * Licensing: MIT License. Permissive Zone 1 SDK source.
 */

#include <iris/spawn.h>
#include <iris/init.h>
#include <zircon/syscalls.h>
#include <zircon/process.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define IRIS_SPAWN_MAX_TOTAL_HANDLES 64u
#define IRIS_SPAWN_BOOTSTRAP_BUF_SIZE 4096u

static size_t iris_strlen(const char* s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

static void* iris_memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

static void* iris_memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

zx_status_t iris_spawn(const iris_spawn_args_t* args,
                       zx_handle_t* out_process,
                       zx_handle_t* out_thread) {
    if (!args || !args->name || !out_process) {
        return ZX_ERR_INVALID_ARGS;
    }

    zx_handle_t job = args->job;
    if (job == ZX_HANDLE_INVALID) {
        job = iris_job_default();
    }
    if (job == ZX_HANDLE_INVALID) {
        job = zx_job_default();
    }
    if (job == ZX_HANDLE_INVALID) {
        return ZX_ERR_BAD_HANDLE;
    }

    size_t name_len = iris_strlen(args->name);

    /* 1. Create Child Process & Root VMAR */
    zx_handle_t proc = ZX_HANDLE_INVALID;
    zx_handle_t vmar = ZX_HANDLE_INVALID;
    zx_status_t status = zx_process_create(job, args->name, name_len, 0, &proc, &vmar);
    if (status != ZX_OK) {
        return status;
    }

    /* 2. Create Initial Thread */
    zx_handle_t thread = ZX_HANDLE_INVALID;
    status = zx_thread_create(proc, args->name, name_len, 0, &thread);
    if (status != ZX_OK) {
        zx_handle_close(vmar);
        zx_handle_close(proc);
        return status;
    }

    /* 3. Allocate and Map Initial Stack VMO */
    size_t stack_size = args->stack_size ? args->stack_size : IRIS_SPAWN_DEFAULT_STACK_SIZE;
    zx_handle_t stack_vmo = ZX_HANDLE_INVALID;
    status = zx_vmo_create((uint64_t)stack_size, 0, &stack_vmo);
    if (status != ZX_OK) {
        zx_handle_close(thread);
        zx_handle_close(vmar);
        zx_handle_close(proc);
        return status;
    }

    zx_vaddr_t stack_base = 0;
    status = zx_vmar_map(vmar, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0,
                         stack_vmo, 0, stack_size, &stack_base);
    if (status != ZX_OK) {
        zx_handle_close(stack_vmo);
        zx_handle_close(thread);
        zx_handle_close(vmar);
        zx_handle_close(proc);
        return status;
    }

    /* Align stack pointer to 16-byte boundary */
    zx_vaddr_t sp = (stack_base + stack_size) & ~(uintptr_t)0xF;
#if defined(__x86_64__)
    /* SysV ABI function entry alignment: (rsp + 8) % 16 == 0 */
    sp -= 8;
#endif

    /* 4. Map vDSO into child VMAR if handle provided */
    zx_vaddr_t vdso_base = args->vdso_base;
    if (args->vmo_vdso != ZX_HANDLE_INVALID && vdso_base == 0) {
        uint64_t vdso_size = 0;
        zx_vmo_get_size(args->vmo_vdso, &vdso_size);
        if (vdso_size > 0) {
            status = zx_vmar_map(vmar, ZX_VM_PERM_READ | ZX_VM_PERM_EXECUTE, 0,
                                 args->vmo_vdso, 0, vdso_size, &vdso_base);
            if (status != ZX_OK) {
                zx_handle_close(stack_vmo);
                zx_handle_close(thread);
                zx_handle_close(vmar);
                zx_handle_close(proc);
                return status;
            }
        }
    }

    /* 5. Assemble Handle Table for Bootstrap Message */
    zx_handle_t hnd_array[IRIS_SPAWN_MAX_TOTAL_HANDLES];
    uint32_t info_array[IRIS_SPAWN_MAX_TOTAL_HANDLES];
    uint32_t total_handles = 0;

    /* Transfer PA_PROC_SELF */
    zx_handle_t proc_copy = ZX_HANDLE_INVALID;
    if (zx_handle_duplicate(proc, ZX_RIGHT_SAME_RIGHTS, &proc_copy) == ZX_OK) {
        hnd_array[total_handles] = proc_copy;
        info_array[total_handles] = PA_HND(PA_PROC_SELF, 0);
        total_handles++;
    }

    /* Transfer PA_THREAD_SELF */
    zx_handle_t thread_copy = ZX_HANDLE_INVALID;
    if (zx_handle_duplicate(thread, ZX_RIGHT_SAME_RIGHTS, &thread_copy) == ZX_OK) {
        hnd_array[total_handles] = thread_copy;
        info_array[total_handles] = PA_HND(PA_THREAD_SELF, 0);
        total_handles++;
    }

    /* Transfer PA_VMAR_ROOT */
    zx_handle_t vmar_copy = ZX_HANDLE_INVALID;
    if (zx_handle_duplicate(vmar, ZX_RIGHT_SAME_RIGHTS, &vmar_copy) == ZX_OK) {
        hnd_array[total_handles] = vmar_copy;
        info_array[total_handles] = PA_HND(PA_VMAR_ROOT, 0);
        total_handles++;
    }

    /* Transfer PA_VMO_STACK */
    zx_handle_t stack_copy = ZX_HANDLE_INVALID;
    if (zx_handle_duplicate(stack_vmo, ZX_RIGHT_SAME_RIGHTS, &stack_copy) == ZX_OK) {
        hnd_array[total_handles] = stack_copy;
        info_array[total_handles] = PA_HND(PA_VMO_STACK, 0);
        total_handles++;
    }

    /* Transfer PA_VMO_VDSO if available */
    if (args->vmo_vdso != ZX_HANDLE_INVALID) {
        zx_handle_t vdso_copy = ZX_HANDLE_INVALID;
        if (zx_handle_duplicate(args->vmo_vdso, ZX_RIGHT_SAME_RIGHTS, &vdso_copy) == ZX_OK) {
            hnd_array[total_handles] = vdso_copy;
            info_array[total_handles] = PA_HND(PA_VMO_VDSO, 0);
            total_handles++;
        }
    }

    /* Transfer PA_JOB_DEFAULT */
    zx_handle_t job_copy = ZX_HANDLE_INVALID;
    if (zx_handle_duplicate(job, ZX_RIGHT_SAME_RIGHTS, &job_copy) == ZX_OK) {
        hnd_array[total_handles] = job_copy;
        info_array[total_handles] = PA_HND(PA_JOB_DEFAULT, 0);
        total_handles++;
    }

    /* Transfer user-specified actions (file descriptors and capability handles) */
    if (args->actions && args->num_actions > 0) {
        for (size_t i = 0; i < args->num_actions; i++) {
            if (total_handles >= IRIS_SPAWN_MAX_TOTAL_HANDLES) {
                break;
            }
            const iris_spawn_action_t* act = &args->actions[i];
            if (act->type == IRIS_SPAWN_ACTION_TRANSFER_HANDLE) {
                hnd_array[total_handles] = act->handle;
                info_array[total_handles] = act->handle_info;
                total_handles++;
            } else if (act->type == IRIS_SPAWN_ACTION_ADD_FD) {
                hnd_array[total_handles] = act->handle;
                info_array[total_handles] = PA_HND(PA_FD, (uint32_t)act->target_fd);
                total_handles++;
            }
        }
    }

    /* 6. Serialize zx_proc_args_t Bootstrap Message Payload */
    uint8_t msg_buf[IRIS_SPAWN_BOOTSTRAP_BUF_SIZE];
    iris_memset(msg_buf, 0, sizeof(msg_buf));

    zx_proc_args_t* pargs = (zx_proc_args_t*)msg_buf;
    pargs->protocol = ZX_PROCARGS_PROTOCOL;
    pargs->version = ZX_PROCARGS_VERSION;

    size_t cursor = sizeof(zx_proc_args_t);

    /* Handle Info Table offset */
    pargs->handle_info_off = (uint32_t)cursor;
    size_t info_bytes = total_handles * sizeof(uint32_t);
    iris_memcpy(msg_buf + cursor, info_array, info_bytes);
    cursor += info_bytes;

    /* Pack Argument Strings */
    pargs->args_off = (uint32_t)cursor;
    pargs->args_num = args->argc;
    if (args->argv && args->argc > 0) {
        for (uint32_t a = 0; a < args->argc; a++) {
            const char* str = args->argv[a];
            size_t slen = iris_strlen(str) + 1;
            if (cursor + slen < sizeof(msg_buf)) {
                iris_memcpy(msg_buf + cursor, str, slen);
                cursor += slen;
            }
        }
    }

    /* Pack Environment Strings */
    pargs->environ_off = (uint32_t)cursor;
    pargs->environ_num = args->envc;
    if (args->envp && args->envc > 0) {
        for (uint32_t e = 0; e < args->envc; e++) {
            const char* str = args->envp[e];
            size_t slen = iris_strlen(str) + 1;
            if (cursor + slen < sizeof(msg_buf)) {
                iris_memcpy(msg_buf + cursor, str, slen);
                cursor += slen;
            }
        }
    }

    /* 7. Create Bootstrap Channel and Transmit State to Child */
    zx_handle_t bstrap_tx = ZX_HANDLE_INVALID;
    zx_handle_t bstrap_rx = ZX_HANDLE_INVALID;
    status = zx_channel_create(0, &bstrap_tx, &bstrap_rx);
    if (status != ZX_OK) {
        for (uint32_t h = 0; h < total_handles; h++) {
            zx_handle_close(hnd_array[h]);
        }
        zx_handle_close(stack_vmo);
        zx_handle_close(thread);
        zx_handle_close(vmar);
        zx_handle_close(proc);
        return status;
    }

    status = zx_channel_write(bstrap_tx, 0, msg_buf, (uint32_t)cursor,
                              hnd_array, total_handles);
    zx_handle_close(bstrap_tx);

    if (status != ZX_OK) {
        zx_handle_close(bstrap_rx);
        zx_handle_close(stack_vmo);
        zx_handle_close(thread);
        zx_handle_close(vmar);
        zx_handle_close(proc);
        return status;
    }

    /* 8. Launch Child Process Thread */
    status = zx_process_start(proc, thread, args->entry, sp, bstrap_rx, vdso_base);

    zx_handle_close(stack_vmo);
    zx_handle_close(vmar);

    if (status != ZX_OK) {
        zx_handle_close(thread);
        zx_handle_close(proc);
        return status;
    }

    if (out_thread) {
        *out_thread = thread;
    } else {
        zx_handle_close(thread);
    }

    *out_process = proc;
    return ZX_OK;
}

zx_status_t iris_spawn_simple(zx_handle_t job,
                              const char* name,
                              zx_vaddr_t entry,
                              zx_handle_t vmo_vdso,
                              zx_handle_t* out_process) {
    iris_spawn_args_t args = {
        .job = job,
        .name = name,
        .entry = entry,
        .stack_size = 0,
        .vmo_vdso = vmo_vdso,
        .vdso_base = 0,
        .argv = NULL,
        .argc = 0,
        .envp = NULL,
        .envc = 0,
        .actions = NULL,
        .num_actions = 0,
    };
    return iris_spawn(&args, out_process, NULL);
}
