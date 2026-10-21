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
#define IRIS_PAGE_SIZE 4096ULL
#define IRIS_PAGE_MASK (~(IRIS_PAGE_SIZE - 1ULL))

/* ==============================================================================
 * Minimal 64-bit ELF Parser Structures & Constants
 * ============================================================================== */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} iris_elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} iris_elf64_phdr_t;

#define PT_LOAD 1u
#define PF_X    1u
#define PF_W    2u
#define PF_R    4u

#define ET_EXEC 2u
#define ET_DYN  3u

#if defined(__x86_64__)
#define IRIS_ELF_MACHINE 62u /* EM_X86_64 */
#elif defined(__aarch64__)
#define IRIS_ELF_MACHINE 183u /* EM_AARCH64 */
#else
#define IRIS_ELF_MACHINE 0u
#endif

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

/**
 * @brief Parse and map 64-bit ELF segments into a target process's root VMAR.
 *
 * @param vmar Target root VMAR handle.
 * @param vmo_elf Valid executable VMO containing ELF file.
 * @param[out] out_entry Returns the calculated runtime entry point address (may be NULL).
 * @param[out] out_base Returns the calculated runtime base address (may be NULL).
 * @return ZX_OK on success, or error status code.
 */
static zx_status_t iris_elf_load_segments(zx_handle_t vmar,
                                         zx_handle_t vmo_elf,
                                         zx_vaddr_t* out_entry,
                                         zx_vaddr_t* out_base) {
    iris_elf64_ehdr_t ehdr;
    zx_status_t status = zx_vmo_read(vmo_elf, &ehdr, 0, sizeof(ehdr));
    if (status != ZX_OK) {
        return status;
    }

    /* Verify ELF Magic and 64-bit Little Endian format */
    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L'  || ehdr.e_ident[3] != 'F' ||
        ehdr.e_ident[4] != 2    /* 64-bit */ ||
        ehdr.e_ident[5] != 1    /* Little Endian */) {
        return ZX_ERR_NOT_FOUND;
    }

    if (ehdr.e_phnum == 0 || ehdr.e_phnum > 32) {
        return ZX_ERR_INVALID_ARGS;
    }

    iris_elf64_phdr_t phdrs[32];
    size_t phdr_bytes = ehdr.e_phnum * sizeof(iris_elf64_phdr_t);
    status = zx_vmo_read(vmo_elf, phdrs, ehdr.e_phoff, phdr_bytes);
    if (status != ZX_OK) {
        return status;
    }

    /* Compute address span across all PT_LOAD segments */
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            uint64_t start = phdrs[i].p_vaddr & IRIS_PAGE_MASK;
            uint64_t end = (phdrs[i].p_vaddr + phdrs[i].p_memsz + IRIS_PAGE_SIZE - 1u) & IRIS_PAGE_MASK;
            if (start < min_vaddr) min_vaddr = start;
            if (end > max_vaddr) max_vaddr = end;
        }
    }

    if (min_vaddr >= max_vaddr) {
        return ZX_ERR_INVALID_ARGS;
    }

    size_t total_span = max_vaddr - min_vaddr;
    zx_handle_t prog_vmar = ZX_HANDLE_INVALID;
    zx_vaddr_t prog_base = 0;
    uint64_t load_bias = 0;

    if (ehdr.e_type == ET_DYN) {
        /* Position-Independent Executable (PIE): allocate a dedicated VMAR subregion */
        status = zx_vmar_allocate(vmar,
                                  ZX_VM_CAN_MAP_READ | ZX_VM_CAN_MAP_WRITE |
                                  ZX_VM_CAN_MAP_EXECUTE | ZX_VM_CAN_MAP_SPECIFIC,
                                  0, total_span, &prog_vmar, &prog_base);
        if (status != ZX_OK) {
            return status;
        }
        load_bias = prog_base - min_vaddr;
    } else {
        /* Fixed-address executable */
        prog_vmar = vmar;
        load_bias = 0;
    }

    /* Map each PT_LOAD segment */
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        const iris_elf64_phdr_t* ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }

        zx_vm_option_t zx_options = ZX_VM_SPECIFIC;
        if (ph->p_flags & PF_R) zx_options |= ZX_VM_PERM_READ;
        if (ph->p_flags & PF_W) zx_options |= ZX_VM_PERM_WRITE;
        if (ph->p_flags & PF_X) zx_options |= ZX_VM_PERM_EXECUTE;

        uint64_t seg_vaddr = ph->p_vaddr + load_bias;
        uint64_t file_offset = ph->p_offset & IRIS_PAGE_MASK;
        uint64_t map_vaddr = seg_vaddr & IRIS_PAGE_MASK;
        uint64_t page_offset = ph->p_offset - file_offset;

        if (ph->p_flags & PF_W) {
            /*
             * Writable segment (data / bss):
             * Allocate an anonymous VMO for the full memory size plus page alignment offset.
             * Copy the file content into the VMO and zero-initialize trailing BSS bytes.
             */
            uint64_t seg_mem_total = (ph->p_memsz + page_offset + IRIS_PAGE_SIZE - 1u) & IRIS_PAGE_MASK;
            zx_handle_t data_vmo = ZX_HANDLE_INVALID;
            status = zx_vmo_create(seg_mem_total, 0, &data_vmo);
            if (status != ZX_OK) {
                if (ehdr.e_type == ET_DYN) zx_handle_close(prog_vmar);
                return status;
            }

            if (ph->p_filesz > 0) {
                /* Temporary buffer for copying file data into writable VMO */
                uint8_t copy_buf[IRIS_PAGE_SIZE];
                uint64_t rem = ph->p_filesz;
                uint64_t src_off = ph->p_offset;
                uint64_t dst_off = page_offset;
                while (rem > 0) {
                    size_t chunk = rem > sizeof(copy_buf) ? sizeof(copy_buf) : (size_t)rem;
                    status = zx_vmo_read(vmo_elf, copy_buf, src_off, chunk);
                    if (status != ZX_OK) {
                        zx_handle_close(data_vmo);
                        if (ehdr.e_type == ET_DYN) zx_handle_close(prog_vmar);
                        return status;
                    }
                    status = zx_vmo_write(data_vmo, copy_buf, dst_off, chunk);
                    if (status != ZX_OK) {
                        zx_handle_close(data_vmo);
                        if (ehdr.e_type == ET_DYN) zx_handle_close(prog_vmar);
                        return status;
                    }
                    rem -= chunk;
                    src_off += chunk;
                    dst_off += chunk;
                }
            }

            zx_vaddr_t actual_vaddr = 0;
            status = zx_vmar_map(prog_vmar, zx_options,
                                 map_vaddr - (ehdr.e_type == ET_DYN ? prog_base : 0),
                                 data_vmo, 0, seg_mem_total, &actual_vaddr);
            zx_handle_close(data_vmo);

            if (status != ZX_OK) {
                if (ehdr.e_type == ET_DYN) zx_handle_close(prog_vmar);
                return status;
            }
        } else {
            /*
             * Read-only or executable segment (text / rodata):
             * Map directly from the file's VMO into the target VMAR.
             */
            uint64_t seg_file_total = (ph->p_filesz + page_offset + IRIS_PAGE_SIZE - 1u) & IRIS_PAGE_MASK;
            zx_vaddr_t actual_vaddr = 0;
            status = zx_vmar_map(prog_vmar, zx_options,
                                 map_vaddr - (ehdr.e_type == ET_DYN ? prog_base : 0),
                                 vmo_elf, file_offset, seg_file_total, &actual_vaddr);
            if (status != ZX_OK) {
                if (ehdr.e_type == ET_DYN) zx_handle_close(prog_vmar);
                return status;
            }
        }
    }

    if (ehdr.e_type == ET_DYN) {
        zx_handle_close(prog_vmar);
    }

    if (out_entry) {
        *out_entry = ehdr.e_entry + load_bias;
    }
    if (out_base) {
        *out_base = (ehdr.e_type == ET_DYN) ? prog_base : min_vaddr;
    }
    return ZX_OK;
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

    /* 3. Determine Entry Point & Load ELF Segments if Executable VMO Provided */
    zx_vaddr_t entry_point = args->entry;
    if (args->vmo_executable != ZX_HANDLE_INVALID) {
        status = iris_elf_load_segments(vmar, args->vmo_executable, &entry_point, NULL);
        if (status != ZX_OK) {
            zx_handle_close(thread);
            zx_handle_close(vmar);
            zx_handle_close(proc);
            return status;
        }
    }

    if (entry_point == 0) {
        zx_handle_close(thread);
        zx_handle_close(vmar);
        zx_handle_close(proc);
        return ZX_ERR_INVALID_ARGS;
    }

    /* 4. Allocate and Map Initial Stack VMO */
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

    /*
     * Align initial stack pointer:
     * - On x86_64, SysV ABI specifies (rsp + 8) % 16 == 0 upon function entry.
     * - On aarch64, SP must be 16-byte aligned at all times.
     */
    zx_vaddr_t sp = (stack_base + stack_size) & ~(uintptr_t)0xF;
#if defined(__x86_64__)
    sp -= 8;
#endif

    /* 5. Map vDSO into child VMAR if handle provided */
    zx_vaddr_t vdso_base = args->vdso_base;
    if (args->vmo_vdso != ZX_HANDLE_INVALID && vdso_base == 0) {
        zx_vaddr_t dummy_entry = 0;
        status = iris_elf_load_segments(vmar, args->vmo_vdso, &dummy_entry, &vdso_base);
        if (status != ZX_OK) {
            zx_handle_close(stack_vmo);
            zx_handle_close(thread);
            zx_handle_close(vmar);
            zx_handle_close(proc);
            return status;
        }
    }

    /* 6. Assemble Handle Table for Bootstrap Message */
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
                if (act->handle != ZX_HANDLE_INVALID) {
                    hnd_array[total_handles] = act->handle;
                    info_array[total_handles] = act->handle_info;
                    total_handles++;
                }
            } else if (act->type == IRIS_SPAWN_ACTION_ADD_FD) {
                if (act->handle != ZX_HANDLE_INVALID) {
                    hnd_array[total_handles] = act->handle;
                    info_array[total_handles] = PA_HND(PA_FD, (uint32_t)act->target_fd);
                    total_handles++;
                }
            }
        }
    }

    /* 7. Serialize zx_proc_args_t Bootstrap Message Payload */
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

    /* 8. Create Bootstrap Channel and Transmit State to Child */
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

    /* 9. Launch Child Process Thread */
    status = zx_process_start(proc, thread, entry_point, sp, bstrap_rx, vdso_base);

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

zx_status_t iris_spawn_elf(zx_handle_t job,
                           const char* name,
                           zx_handle_t vmo_elf,
                           zx_handle_t vmo_vdso,
                           const char* const* argv,
                           uint32_t argc,
                           const iris_spawn_action_t* actions,
                           size_t num_actions,
                           zx_handle_t* out_process,
                           zx_handle_t* out_thread) {
    iris_spawn_args_t args = {
        .job = job,
        .name = name,
        .vmo_executable = vmo_elf,
        .entry = 0,
        .stack_size = 0,
        .vmo_vdso = vmo_vdso,
        .vdso_base = 0,
        .argv = argv,
        .argc = argc,
        .envp = NULL,
        .envc = 0,
        .actions = actions,
        .num_actions = num_actions,
    };
    return iris_spawn(&args, out_process, out_thread);
}

zx_status_t iris_spawn_simple(zx_handle_t job,
                              const char* name,
                              zx_vaddr_t entry,
                              zx_handle_t vmo_vdso,
                              zx_handle_t* out_process) {
    iris_spawn_args_t args = {
        .job = job,
        .name = name,
        .vmo_executable = ZX_HANDLE_INVALID,
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
