/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_init/src/main.c
 * Purpose: Initial userspace bootstrap server (first Ring 3 process spawned by userboot).
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 system server.
 */

/**
 * @file main.c
 * @brief Early bootstrap process (iris_init) for irisOS.
 *
 * MODULE LIFECYCLE & RESPONSIBILITIES:
 *  1. Process Entry:
 *     - iris_init is executed directly by Zircon's kernel-mode / early-boot userboot
 *       as the primary user-mode process in Ring 3 (x86_64) or EL0 (ARM64).
 *     - In accordance with Section 4.4 of AGENTS.md, iris_init does NOT use standard
 *       C runtime entry (`int main(int argc, char** argv)`).
 *     - The entry point is `_start(void* start_arg, void* vdso_base)`, where `start_arg` conveys
 *       the initial bootstrap channel handle and `vdso_base` conveys the virtual address
 *       of the mapped vDSO (libzircon.so) passed via `zx_process_start()`.
 *
 *  2. vDSO System Call Resolution:
 *     - Zircon microkernel strictly requires all system calls to originate from inside the
 *       vDSO mapping (`valid_pc(pc - vdso_code_address)`). Direct `syscall` instructions
 *       in user space binaries trigger a kernel `ZX_EXCP_POLICY_ERROR`.
 *     - iris_init resolves its early system call stubs dynamically from `vdso_base`'s
 *       ELF dynamic symbol table without requiring any external dynamic linker.
 *
 *  3. Capability Handle Extraction:
 *     - iris_init consumes the bootstrap channel using `zx_channel_read()` to receive
 *       the `zx_proc_args_t` message and all initial capability handles transferred by userboot.
 *     - The transferred capability handles are indexed by an array of uint32_t handle info
 *       identifiers packed at `handle_info_off` in the message payload.
 *     - The four essential root capabilities required by Section 4.4 are extracted:
 *         * `PA_HND(PA_RESOURCE, 0)`    -> Root Resource handle (for MMIO, IRQs, ports)
 *         * `PA_HND(PA_VMO_BOOTFS, 0)`   -> BOOTFS VMO handle (early boot files/drivers)
 *         * `PA_HND(PA_JOB_DEFAULT, 0)`  -> Root Job handle (child process/job isolation)
 *         * `PA_HND(PA_VMO_VDSO, 0)`     -> vDSO VMO handle (libzircon.so system calls)
 *
 *  4. Early Serial Logging:
 *     - Creates an early kernel debug log handle using `zx_debuglog_create(root_resource, 0, &log)`
 *       to stream early bootstrap diagnostics to UART/console before `iris_pty` is initialized.
 *     - Outputs the official irisOS bootstrap banner:
 *       `"[irisOS] Welcome to irisOS (x86_64 / arm64) - iris_init bootstrap successful!\n"`.
 *
 *  5. Future Execution Flow (Phase 4 & 5):
 *     - Scan PCIe (ECAM) for VirtIO block, input, and GPU devices.
 *     - Launch `virtio-blk-pci`, `iris_crypt`, and `iris_fshost`.
 *     - Enter idle wait / server coordination loop via `zx_nanosleep()`.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <zircon/types.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/log.h>

/* Maximum handles expected in the bootstrap message */
#define IRIS_BOOTSTRAP_MAX_HANDLES 64

/* Buffer size for reading the zx_proc_args_t message and metadata */
#define IRIS_BOOTSTRAP_MSG_BYTES 4096

/* Global capabilities extracted from userboot bootstrap channel */
static zx_handle_t g_root_resource   = ZX_HANDLE_INVALID;
static zx_handle_t g_bootfs_vmo      = ZX_HANDLE_INVALID;
static zx_handle_t g_root_job        = ZX_HANDLE_INVALID;
static zx_handle_t g_vdso_vmo        = ZX_HANDLE_INVALID;
static zx_handle_t g_debuglog_handle = ZX_HANDLE_INVALID;

/* ==============================================================================
 * Freestanding 64-bit ELF vDSO Dynamic Resolver
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
} Elf64_Ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr_t;

typedef struct {
    int64_t d_tag;
    union {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
} Elf64_Dyn_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym_t;

#define PT_DYNAMIC 2
#define DT_NULL    0
#define DT_STRTAB  5
#define DT_SYMTAB  6
#define DT_STRSZ   10

static void* vdso_lookup(uintptr_t base, const char* name) {
    if (base == 0 || name == NULL) {
        return NULL;
    }
    const uint8_t* base_ptr = (const uint8_t*)base;
    const Elf64_Ehdr_t* ehdr = (const Elf64_Ehdr_t*)base_ptr;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        return NULL;
    }

    const Elf64_Phdr_t* phdr = (const Elf64_Phdr_t*)(base_ptr + ehdr->e_phoff);
    const Elf64_Dyn_t* dyn = NULL;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = (const Elf64_Dyn_t*)(base_ptr + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) {
        return NULL;
    }

    const Elf64_Sym_t* symtab = NULL;
    const char* strtab = NULL;
    size_t strsz = 0;

    for (const Elf64_Dyn_t* d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_SYMTAB) {
            symtab = (const Elf64_Sym_t*)(base_ptr + d->d_un.d_ptr);
        } else if (d->d_tag == DT_STRTAB) {
            strtab = (const char*)(base_ptr + d->d_un.d_ptr);
        } else if (d->d_tag == DT_STRSZ) {
            strsz = d->d_un.d_val;
        }
    }

    if (!symtab || !strtab) {
        return NULL;
    }

    size_t num_syms = ((uintptr_t)strtab - (uintptr_t)symtab) / sizeof(Elf64_Sym_t);
    for (size_t i = 0; i < num_syms; i++) {
        if (symtab[i].st_name < strsz) {
            const char* s = strtab + symtab[i].st_name;
            const char* n = name;
            while (*s && *n && (*s == *n)) {
                s++;
                n++;
            }
            if (*s == '\0' && *n == '\0') {
                return (void*)(base_ptr + symtab[i].st_value);
            }
        }
    }
    return NULL;
}

/* Function pointers to vDSO syscall implementations */
static zx_status_t (*g_zx_channel_read)(
    zx_handle_t handle, uint32_t options, void* bytes,
    zx_handle_t* handles, uint32_t num_bytes, uint32_t num_handles,
    uint32_t* actual_bytes, uint32_t* actual_handles) = NULL;

static zx_status_t (*g_zx_handle_close)(zx_handle_t handle) = NULL;

static zx_status_t (*g_zx_debuglog_create)(
    zx_handle_t resource, uint32_t options, zx_handle_t* out) = NULL;

static zx_status_t (*g_zx_debuglog_write)(
    zx_handle_t handle, uint32_t options, const void* buffer, size_t buffer_size) = NULL;

static zx_status_t (*g_zx_nanosleep)(zx_time_t deadline) = NULL;

static void vdso_init(uintptr_t base) {
    g_zx_channel_read = (void*)vdso_lookup(base, "_zx_channel_read");
    if (!g_zx_channel_read)
        g_zx_channel_read = (void*)vdso_lookup(base, "zx_channel_read");

    g_zx_handle_close = (void*)vdso_lookup(base, "_zx_handle_close");
    if (!g_zx_handle_close)
        g_zx_handle_close = (void*)vdso_lookup(base, "zx_handle_close");

    g_zx_debuglog_create = (void*)vdso_lookup(base, "_zx_debuglog_create");
    if (!g_zx_debuglog_create)
        g_zx_debuglog_create = (void*)vdso_lookup(base, "zx_debuglog_create");

    g_zx_debuglog_write = (void*)vdso_lookup(base, "_zx_debuglog_write");
    if (!g_zx_debuglog_write)
        g_zx_debuglog_write = (void*)vdso_lookup(base, "zx_debuglog_write");

    g_zx_nanosleep = (void*)vdso_lookup(base, "_zx_nanosleep");
    if (!g_zx_nanosleep)
        g_zx_nanosleep = (void*)vdso_lookup(base, "zx_nanosleep");
}

/* Exported ABI wrappers matching <zircon/syscalls.h> definitions */
zx_status_t zx_channel_read(zx_handle_t handle, uint32_t options, void* bytes,
                            zx_handle_t* handles, uint32_t num_bytes, uint32_t num_handles,
                            uint32_t* actual_bytes, uint32_t* actual_handles) {
    if (g_zx_channel_read) {
        return g_zx_channel_read(handle, options, bytes, handles, num_bytes, num_handles, actual_bytes, actual_handles);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t zx_handle_close(zx_handle_t handle) {
    if (g_zx_handle_close) {
        return g_zx_handle_close(handle);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t zx_debuglog_create(zx_handle_t resource, uint32_t options, zx_handle_t* out) {
    if (g_zx_debuglog_create) {
        return g_zx_debuglog_create(resource, options, out);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t zx_debuglog_write(zx_handle_t handle, uint32_t options, const void* buffer, size_t buffer_size) {
    if (g_zx_debuglog_write) {
        return g_zx_debuglog_write(handle, options, buffer, buffer_size);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t zx_nanosleep(zx_time_t deadline) {
    if (g_zx_nanosleep) {
        return g_zx_nanosleep(deadline);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

/**
 * @brief Writes an ASCII string to the early serial debug log.
 *
 * @param log_handle Handle to the debuglog dispatcher.
 * @param str Null-terminated string to output.
 */
static void early_log_puts(zx_handle_t log_handle, const char* str) {
    if (log_handle == ZX_HANDLE_INVALID || str == NULL) {
        return;
    }
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    zx_debuglog_write(log_handle, 0, str, len);
}

/**
 * @brief Entry point for iris_init.
 *
 * Consumes the bootstrap channel handle from start_arg and extracts initial
 * capabilities per Section 4.4 of AGENTS.md.
 *
 * @param start_arg Pointer / integer argument passed by zx_process_start().
 *                  Contains the initial bootstrap channel handle.
 * @param vdso_base Virtual base address of the mapped vDSO (libzircon.so) in
 *                  iris_init's address space.
 */
__attribute__((noreturn)) void _start(void* start_arg, void* vdso_base) {
    /* Initialize vDSO syscall dispatch table */
    if (vdso_base != NULL) {
        vdso_init((uintptr_t)vdso_base);
    }

    /*
     * Extract the bootstrap channel handle.
     * When userboot launches the initial process via zx_process_start(),
     * the second argument to zx_process_start() is provided in the register
     * designated for start_arg (rdi on x86_64, x0 on ARM64).
     */
    zx_handle_t bootstrap_channel = (zx_handle_t)(uintptr_t)start_arg;

    /* Channel message receive buffers */
    uint8_t msg_buffer[IRIS_BOOTSTRAP_MSG_BYTES] __attribute__((aligned(8)));
    zx_handle_t handles[IRIS_BOOTSTRAP_MAX_HANDLES];
    uint32_t actual_bytes = 0;
    uint32_t actual_handles = 0;

    /* Read the bootstrap message containing zx_proc_args_t and capability handles */
    zx_status_t status = zx_channel_read(
        bootstrap_channel,
        0,
        msg_buffer,
        handles,
        sizeof(msg_buffer),
        IRIS_BOOTSTRAP_MAX_HANDLES,
        &actual_bytes,
        &actual_handles
    );

    if (status == ZX_OK && actual_bytes >= sizeof(zx_proc_args_t)) {
        const zx_proc_args_t* args = (const zx_proc_args_t*)msg_buffer;

        /* Validate protocol and version to protect against ABI mismatch */
        if (args->protocol == ZX_PROCARGS_PROTOCOL && args->version == ZX_PROCARGS_VERSION) {
            /* Verify handle_info array bounds within the received message buffer */
            size_t handle_info_size = (size_t)actual_handles * sizeof(uint32_t);
            if ((size_t)args->handle_info_off + handle_info_size <= (size_t)actual_bytes) {
                const uint32_t* handle_info = (const uint32_t*)(msg_buffer + args->handle_info_off);

                /* Iterate and extract required root capabilities */
                for (uint32_t i = 0; i < actual_handles; i++) {
                    uint32_t info = handle_info[i];
                    zx_handle_t h = handles[i];

                    if (info == PA_HND(PA_RESOURCE, 0)) {
                        g_root_resource = h;
                    } else if (info == PA_HND(PA_VMO_BOOTFS, 0)) {
                        g_bootfs_vmo = h;
                    } else if (info == PA_HND(PA_JOB_DEFAULT, 0)) {
                        g_root_job = h;
                    } else if (info == PA_HND(PA_VMO_VDSO, 0)) {
                        g_vdso_vmo = h;
                    }
                }
            }
        }
    }

    /* Bootstrap channel is consumed; close our end */
    if (bootstrap_channel != ZX_HANDLE_INVALID) {
        zx_handle_close(bootstrap_channel);
    }

    /*
     * Initialize early debuglog using the Root Resource handle.
     * This creates a write-only debuglog handle connected to the kernel serial log.
     */
    status = zx_debuglog_create(g_root_resource, 0, &g_debuglog_handle);
    if (status == ZX_OK) {
#if defined(__x86_64__)
        early_log_puts(g_debuglog_handle,
            "[irisOS] Welcome to irisOS (x86_64) - iris_init bootstrap successful!\n");
#elif defined(__aarch64__)
        early_log_puts(g_debuglog_handle,
            "[irisOS] Welcome to irisOS (arm64) - iris_init bootstrap successful!\n");
#else
        early_log_puts(g_debuglog_handle,
            "[irisOS] Welcome to irisOS (unknown) - iris_init bootstrap successful!\n");
#endif

        if (g_root_resource != ZX_HANDLE_INVALID) {
            early_log_puts(g_debuglog_handle, "[iris_init] Root Resource: OK\n");
        }
        if (g_bootfs_vmo != ZX_HANDLE_INVALID) {
            early_log_puts(g_debuglog_handle, "[iris_init] BOOTFS VMO: OK\n");
        }
        if (g_root_job != ZX_HANDLE_INVALID) {
            early_log_puts(g_debuglog_handle, "[iris_init] Root Job: OK\n");
        }
        if (g_vdso_vmo != ZX_HANDLE_INVALID) {
            early_log_puts(g_debuglog_handle, "[iris_init] vDSO VMO: OK\n");
        }
    }

    /*
     * For Phase 1-2 bootstrap verification, suspend the bootstrap thread
     * indefinitely. Subsequent phases will initialize the Ring 3 PCIe drivers,
     * filesystems (iris_fshost), and security servers (iris_crypt).
     */
    for (;;) {
        zx_nanosleep(ZX_TIME_INFINITE);
    }
}
