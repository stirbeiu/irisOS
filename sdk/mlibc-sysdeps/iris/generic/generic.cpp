/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/mlibc-sysdeps/iris/generic/generic.cpp
 * Purpose: irisOS sysdeps implementation for mlibc (memory, console, futex, TCB, process).
 *
 * Licensing: MIT License. Permissive Zone 1 sysdeps source.
 */

#include <bits/ensure.h>
#include <mlibc/debug.hpp>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/tcb.hpp>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include <zircon/types.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>
#include <zircon/process.h>
#include <zircon/processargs.h>

#ifndef PA_FD
#define PA_FD 0x30u
#endif

/* ==============================================================================
 * Global Capability Handle Storage
 * ============================================================================== */

#define IRIS_MAX_STARTUP_HANDLES 64
static zx_handle_t g_startup_handles[IRIS_MAX_STARTUP_HANDLES];
static uint32_t g_startup_handle_info[IRIS_MAX_STARTUP_HANDLES];
static size_t g_num_startup_handles = 0;

static zx_handle_t g_process_self = ZX_HANDLE_INVALID;
static zx_handle_t g_thread_self = ZX_HANDLE_INVALID;
static zx_handle_t g_vmar_root_self = ZX_HANDLE_INVALID;
static zx_handle_t g_job_default = ZX_HANDLE_INVALID;
static zx_handle_t g_debuglog_handle = ZX_HANDLE_INVALID;

#include <iris/fd_table.h>
#if !MLIBC_BUILDING_RTLD
#include <iris/spawn.h>
#endif

static zx_handle_t g_vdso_vmo = ZX_HANDLE_INVALID;
static zx_handle_t g_bootfs_vmo = ZX_HANDLE_INVALID;

extern "C" void iris_set_vmar_root(zx_handle_t handle) {
    g_vmar_root_self = handle;
}

extern "C" void iris_set_job_default(zx_handle_t handle) {
    g_job_default = handle;
}

extern "C" void iris_set_vdso_vmo(zx_handle_t handle) {
    g_vdso_vmo = handle;
}

extern "C" void iris_set_bootfs_vmo(zx_handle_t handle) {
    g_bootfs_vmo = handle;
}

/* ==============================================================================
 * Client-Side File Descriptor Table (int fd -> zx_handle_t)
 * ============================================================================== */

static iris_fd_entry_t g_fd_table[IRIS_MAX_FD];
static bool g_fd_table_initialized = false;

extern "C" void iris_fd_table_init(void) {
    if (g_fd_table_initialized) {
        return;
    }
    for (int i = 0; i < IRIS_MAX_FD; i++) {
        g_fd_table[i].handle = ZX_HANDLE_INVALID;
        g_fd_table[i].flags = 0;
        g_fd_table[i].offset = 0;
        g_fd_table[i].type = IRIS_FD_CLOSED;
    }

    // Default stdio (FDs 0, 1, 2) to debuglog until channels are bound
    g_fd_table[0].handle = g_debuglog_handle;
    g_fd_table[0].flags = O_RDONLY;
    g_fd_table[0].offset = 0;
    g_fd_table[0].type = IRIS_FD_DEBUGLOG;

    g_fd_table[1].handle = g_debuglog_handle;
    g_fd_table[1].flags = O_WRONLY;
    g_fd_table[1].offset = 0;
    g_fd_table[1].type = IRIS_FD_DEBUGLOG;

    g_fd_table[2].handle = g_debuglog_handle;
    g_fd_table[2].flags = O_WRONLY;
    g_fd_table[2].offset = 0;
    g_fd_table[2].type = IRIS_FD_DEBUGLOG;

    g_fd_table_initialized = true;
}

extern "C" void iris_set_debuglog(zx_handle_t handle) {
    g_debuglog_handle = handle;
    if (!g_fd_table_initialized) {
        iris_fd_table_init();
    }
    for (int fd = 0; fd < 3; fd++) {
        if (g_fd_table[fd].type == IRIS_FD_DEBUGLOG) {
            g_fd_table[fd].handle = handle;
        }
    }
}

extern "C" int iris_fd_bind(int fd, zx_handle_t handle, iris_fd_type_t type, int flags) {
    if (!g_fd_table_initialized) {
        iris_fd_table_init();
    }
    if (fd < 0 || fd >= IRIS_MAX_FD) {
        return -1;
    }
    if (g_fd_table[fd].type == IRIS_FD_CHANNEL && g_fd_table[fd].handle != ZX_HANDLE_INVALID) {
        zx_handle_close(g_fd_table[fd].handle);
    }
    g_fd_table[fd].handle = handle;
    g_fd_table[fd].flags = flags;
    g_fd_table[fd].offset = 0;
    g_fd_table[fd].type = type;
    return 0;
}

extern "C" int iris_fd_alloc(zx_handle_t handle, iris_fd_type_t type, int flags) {
    if (!g_fd_table_initialized) {
        iris_fd_table_init();
    }
    for (int fd = 3; fd < IRIS_MAX_FD; fd++) {
        if (g_fd_table[fd].type == IRIS_FD_CLOSED) {
            g_fd_table[fd].handle = handle;
            g_fd_table[fd].flags = flags;
            g_fd_table[fd].offset = 0;
            g_fd_table[fd].type = type;
            return fd;
        }
    }
    return -1;
}

extern "C" iris_fd_entry_t* iris_fd_get(int fd) {
    if (!g_fd_table_initialized) {
        iris_fd_table_init();
    }
    if (fd < 0 || fd >= IRIS_MAX_FD) {
        return nullptr;
    }
    return &g_fd_table[fd];
}

extern "C" zx_handle_t iris_fd_get_handle(int fd) {
    iris_fd_entry_t* entry = iris_fd_get(fd);
    if (!entry || entry->type == IRIS_FD_CLOSED) {
        return ZX_HANDLE_INVALID;
    }
    return entry->handle;
}

extern "C" int iris_fd_close(int fd) {
    iris_fd_entry_t* entry = iris_fd_get(fd);
    if (!entry || entry->type == IRIS_FD_CLOSED) {
        return -1;
    }
    if (entry->type == IRIS_FD_CHANNEL && entry->handle != ZX_HANDLE_INVALID) {
        zx_handle_close(entry->handle);
    }
    entry->handle = ZX_HANDLE_INVALID;
    entry->flags = 0;
    entry->offset = 0;
    entry->type = IRIS_FD_CLOSED;
    return 0;
}

extern "C" int iris_fd_count_open(void) {
    if (!g_fd_table_initialized) {
        iris_fd_table_init();
    }
    int count = 0;
    for (int i = 0; i < IRIS_MAX_FD; i++) {
        if (g_fd_table[i].type != IRIS_FD_CLOSED) {
            count++;
        }
    }
    return count;
}

extern "C" void iris_init_startup_handles(size_t nhandles, const zx_handle_t handles[], const uint32_t handle_info[]) {
    if (nhandles > IRIS_MAX_STARTUP_HANDLES) {
        nhandles = IRIS_MAX_STARTUP_HANDLES;
    }
    for (size_t i = 0; i < nhandles; ++i) {
        g_startup_handles[i] = handles[i];
        g_startup_handle_info[i] = handle_info[i];
    }
    g_num_startup_handles = nhandles;
}

extern "C" zx_handle_t zx_take_startup_handle(uint32_t hnd_info) {
    for (size_t i = 0; i < g_num_startup_handles; ++i) {
        if (g_startup_handle_info[i] == hnd_info) {
            zx_handle_t h = g_startup_handles[i];
            g_startup_handles[i] = ZX_HANDLE_INVALID;
            g_startup_handle_info[i] = 0;
            return h;
        }
    }
    return ZX_HANDLE_INVALID;
}

extern "C" void iris_set_process_handles(zx_handle_t proc, zx_handle_t thrd, zx_handle_t vmar, zx_handle_t job) {
    if (proc != ZX_HANDLE_INVALID) g_process_self = proc;
    if (thrd != ZX_HANDLE_INVALID) g_thread_self = thrd;
    if (vmar != ZX_HANDLE_INVALID) g_vmar_root_self = vmar;
    if (job != ZX_HANDLE_INVALID) g_job_default = job;
}

extern "C" zx_handle_t zx_process_self(void) {
    if (g_process_self == ZX_HANDLE_INVALID) {
        g_process_self = zx_take_startup_handle(PA_HND(PA_PROC_SELF, 0));
    }
    return g_process_self;
}

extern "C" zx_handle_t zx_thread_self(void) {
    if (g_thread_self == ZX_HANDLE_INVALID) {
        g_thread_self = zx_take_startup_handle(PA_HND(PA_THREAD_SELF, 0));
    }
    return g_thread_self;
}

extern "C" zx_handle_t zx_vmar_root_self(void) {
    if (g_vmar_root_self == ZX_HANDLE_INVALID) {
        g_vmar_root_self = zx_take_startup_handle(PA_HND(PA_VMAR_ROOT, 0));
    }
    return g_vmar_root_self;
}

extern "C" zx_handle_t zx_job_default(void) {
    if (g_job_default == ZX_HANDLE_INVALID) {
        g_job_default = zx_take_startup_handle(PA_HND(PA_JOB_DEFAULT, 0));
    }
    return g_job_default;
}

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
} Elf64_Ehdr_local;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr_local;

typedef struct {
    int64_t d_tag;
    union {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
} Elf64_Dyn_local;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym_local;

#define PT_DYNAMIC 2
#define DT_NULL    0
#define DT_STRTAB  5
#define DT_SYMTAB  6
#define DT_STRSZ   10

static void* vdso_sym_lookup(uintptr_t base, const char* name) {
    if (base == 0 || name == nullptr) {
        return nullptr;
    }
    const uint8_t* base_ptr = reinterpret_cast<const uint8_t*>(base);
    const Elf64_Ehdr_local* ehdr = reinterpret_cast<const Elf64_Ehdr_local*>(base_ptr);
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        return nullptr;
    }

    const Elf64_Phdr_local* phdr = reinterpret_cast<const Elf64_Phdr_local*>(base_ptr + ehdr->e_phoff);
    const Elf64_Dyn_local* dyn = nullptr;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const Elf64_Dyn_local*>(base_ptr + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) {
        return nullptr;
    }

    const Elf64_Sym_local* symtab = nullptr;
    const char* strtab = nullptr;
    size_t strsz = 0;

    for (const Elf64_Dyn_local* d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_SYMTAB) {
            symtab = reinterpret_cast<const Elf64_Sym_local*>(base_ptr + d->d_un.d_ptr);
        } else if (d->d_tag == DT_STRTAB) {
            strtab = reinterpret_cast<const char*>(base_ptr + d->d_un.d_ptr);
        } else if (d->d_tag == DT_STRSZ) {
            strsz = d->d_un.d_val;
        }
    }

    if (!symtab || !strtab) {
        return nullptr;
    }

    size_t num_syms = (reinterpret_cast<uintptr_t>(strtab) - reinterpret_cast<uintptr_t>(symtab)) / sizeof(Elf64_Sym_local);
    for (size_t i = 0; i < num_syms; i++) {
        if (symtab[i].st_name < strsz) {
            const char* s = strtab + symtab[i].st_name;
            const char* n = name;
            while (*s && *n && (*s == *n)) {
                s++;
                n++;
            }
            if (*s == '\0' && *n == '\0') {
                return const_cast<void*>(reinterpret_cast<const void*>(base_ptr + symtab[i].st_value));
            }
        }
    }
    return nullptr;
}

/* Function pointers to vDSO syscall implementations */
static zx_status_t (*pfn_zx_channel_read)(
    zx_handle_t handle, uint32_t options, void* bytes,
    zx_handle_t* handles, uint32_t num_bytes, uint32_t num_handles,
    uint32_t* actual_bytes, uint32_t* actual_handles) = nullptr;
static zx_status_t (*pfn_zx_channel_write)(
    zx_handle_t handle, uint32_t options, const void* bytes, uint32_t num_bytes,
    const zx_handle_t* handles, uint32_t num_handles) = nullptr;
static zx_status_t (*pfn_zx_channel_create)(uint32_t options, zx_handle_t* out0, zx_handle_t* out1) = nullptr;
static zx_status_t (*pfn_zx_channel_call)(
    zx_handle_t handle, uint32_t options, zx_time_t deadline,
    const zx_channel_call_args_t* args, uint32_t* actual_bytes, uint32_t* actual_handles) = nullptr;

static zx_status_t (*pfn_zx_handle_close)(zx_handle_t handle) = nullptr;
static zx_status_t (*pfn_zx_handle_duplicate)(zx_handle_t handle, zx_rights_t rights, zx_handle_t* out) = nullptr;
static zx_status_t (*pfn_zx_debuglog_write)(zx_handle_t handle, uint32_t options, const void* buffer, size_t buffer_size) = nullptr;
static zx_status_t (*pfn_zx_vmo_create)(uint64_t size, uint32_t options, zx_handle_t* out) = nullptr;
static zx_status_t (*pfn_zx_vmo_get_size)(zx_handle_t handle, uint64_t* size) = nullptr;
static zx_status_t (*pfn_zx_vmo_set_size)(zx_handle_t handle, uint64_t size) = nullptr;
static zx_status_t (*pfn_zx_vmo_read)(zx_handle_t handle, void* data, uint64_t offset, size_t len) = nullptr;
static zx_status_t (*pfn_zx_vmo_write)(zx_handle_t handle, const void* data, uint64_t offset, size_t len) = nullptr;
static zx_status_t (*pfn_zx_vmar_map)(zx_handle_t vmar, zx_vm_option_t options, uint64_t vmar_offset,
                                      zx_handle_t vmo, uint64_t vmo_offset, uint64_t len, zx_vaddr_t* ptr) = nullptr;
static zx_status_t (*pfn_zx_vmar_unmap)(zx_handle_t vmar, zx_vaddr_t addr, uint64_t len) = nullptr;
static zx_status_t (*pfn_zx_vmar_allocate)(zx_handle_t parent_vmar, zx_vm_option_t options,
                                           uint64_t offset, uint64_t size,
                                           zx_handle_t* child_vmar, zx_vaddr_t* child_addr) = nullptr;
static zx_status_t (*pfn_zx_process_create)(zx_handle_t job, const char* name, size_t name_len,
                                            uint32_t options, zx_handle_t* proc_handle, zx_handle_t* vmar_handle) = nullptr;
static zx_status_t (*pfn_zx_process_start)(zx_handle_t handle, zx_handle_t thread,
                                           zx_vaddr_t entry, zx_vaddr_t stack,
                                           zx_handle_t arg1, uintptr_t arg2) = nullptr;
static void        (*pfn_zx_process_exit)(int64_t retcode) = nullptr;
static zx_status_t (*pfn_zx_thread_create)(zx_handle_t process, const char* name, size_t name_len,
                                           uint32_t options, zx_handle_t* out) = nullptr;
static zx_status_t (*pfn_zx_thread_start)(zx_handle_t handle, zx_vaddr_t thread_entry,
                                          zx_vaddr_t stack, uintptr_t arg1, uintptr_t arg2) = nullptr;
static void        (*pfn_zx_thread_exit)(void) = nullptr;
static zx_status_t (*pfn_zx_object_set_property)(zx_handle_t handle, uint32_t property, const void* value, size_t size) = nullptr;
static zx_status_t (*pfn_zx_nanosleep)(zx_time_t deadline) = nullptr;
static zx_time_t   (*pfn_zx_clock_get)(zx_clock_t clock_id) = nullptr;
static zx_time_t   (*pfn_zx_clock_get_monotonic)(void) = nullptr;
static zx_status_t (*pfn_zx_futex_wait)(const zx_futex_t* value_ptr, int current_value, zx_time_t deadline) = nullptr;
static zx_status_t (*pfn_zx_futex_wake)(const zx_futex_t* value_ptr, uint32_t count) = nullptr;

static zx_status_t (*pfn_zx_vmo_create_physical)(zx_handle_t rsrc, zx_paddr_t paddr, size_t size, zx_handle_t* out) = nullptr;
static zx_status_t (*pfn_zx_vmo_create_contiguous)(zx_handle_t bti, size_t size, uint32_t alignment_log2, zx_handle_t* out) = nullptr;
static zx_status_t (*pfn_zx_iommu_create)(zx_handle_t rsrc, uint32_t type, const void* desc, size_t desc_len, zx_handle_t* out) = nullptr;
static zx_status_t (*pfn_zx_bti_create)(zx_handle_t iommu, uint32_t options, uint64_t bti_id, zx_handle_t* out) = nullptr;
static zx_status_t (*pfn_zx_bti_pin)(zx_handle_t bti, uint32_t options, zx_handle_t vmo, uint64_t offset, uint64_t size, zx_paddr_t* addrs, size_t addrs_count, zx_handle_t* pmt) = nullptr;
static zx_status_t (*pfn_zx_pmt_unpin)(zx_handle_t pmt) = nullptr;
static zx_status_t (*pfn_zx_object_wait_one)(zx_handle_t handle, zx_signals_t signals, zx_time_t deadline, zx_signals_t* observed) = nullptr;

static void* vdso_find(uintptr_t base, const char* name, const char* name_with_underscore) {
    void* ptr = vdso_sym_lookup(base, name_with_underscore);
    if (!ptr) {
        ptr = vdso_sym_lookup(base, name);
    }
    return ptr;
}

extern "C" void iris_vdso_init(uintptr_t base) {
    if (base == 0) return;

    pfn_zx_channel_read       = (decltype(pfn_zx_channel_read))vdso_find(base, "zx_channel_read", "_zx_channel_read");
    pfn_zx_channel_write      = (decltype(pfn_zx_channel_write))vdso_find(base, "zx_channel_write", "_zx_channel_write");
    pfn_zx_channel_create     = (decltype(pfn_zx_channel_create))vdso_find(base, "zx_channel_create", "_zx_channel_create");
    pfn_zx_channel_call       = (decltype(pfn_zx_channel_call))vdso_find(base, "zx_channel_call", "_zx_channel_call");
    pfn_zx_handle_close       = (decltype(pfn_zx_handle_close))vdso_find(base, "zx_handle_close", "_zx_handle_close");
    pfn_zx_handle_duplicate   = (decltype(pfn_zx_handle_duplicate))vdso_find(base, "zx_handle_duplicate", "_zx_handle_duplicate");
    pfn_zx_debuglog_write     = (decltype(pfn_zx_debuglog_write))vdso_find(base, "zx_debuglog_write", "_zx_debuglog_write");
    pfn_zx_vmo_create         = (decltype(pfn_zx_vmo_create))vdso_find(base, "zx_vmo_create", "_zx_vmo_create");
    pfn_zx_vmo_get_size       = (decltype(pfn_zx_vmo_get_size))vdso_find(base, "zx_vmo_get_size", "_zx_vmo_get_size");
    pfn_zx_vmo_set_size       = (decltype(pfn_zx_vmo_set_size))vdso_find(base, "zx_vmo_set_size", "_zx_vmo_set_size");
    pfn_zx_vmo_read           = (decltype(pfn_zx_vmo_read))vdso_find(base, "zx_vmo_read", "_zx_vmo_read");
    pfn_zx_vmo_write          = (decltype(pfn_zx_vmo_write))vdso_find(base, "zx_vmo_write", "_zx_vmo_write");
    pfn_zx_vmar_map           = (decltype(pfn_zx_vmar_map))vdso_find(base, "zx_vmar_map", "_zx_vmar_map");
    pfn_zx_vmar_unmap         = (decltype(pfn_zx_vmar_unmap))vdso_find(base, "zx_vmar_unmap", "_zx_vmar_unmap");
    pfn_zx_vmar_allocate      = (decltype(pfn_zx_vmar_allocate))vdso_find(base, "zx_vmar_allocate", "_zx_vmar_allocate");
    pfn_zx_process_create     = (decltype(pfn_zx_process_create))vdso_find(base, "zx_process_create", "_zx_process_create");
    pfn_zx_process_start      = (decltype(pfn_zx_process_start))vdso_find(base, "zx_process_start", "_zx_process_start");
    pfn_zx_process_exit       = (decltype(pfn_zx_process_exit))vdso_find(base, "zx_process_exit", "_zx_process_exit");
    pfn_zx_thread_create      = (decltype(pfn_zx_thread_create))vdso_find(base, "zx_thread_create", "_zx_thread_create");
    pfn_zx_thread_start       = (decltype(pfn_zx_thread_start))vdso_find(base, "zx_thread_start", "_zx_thread_start");
    pfn_zx_thread_exit        = (decltype(pfn_zx_thread_exit))vdso_find(base, "zx_thread_exit", "_zx_thread_exit");
    pfn_zx_object_set_property= (decltype(pfn_zx_object_set_property))vdso_find(base, "zx_object_set_property", "_zx_object_set_property");
    pfn_zx_nanosleep          = (decltype(pfn_zx_nanosleep))vdso_find(base, "zx_nanosleep", "_zx_nanosleep");
    pfn_zx_clock_get          = (decltype(pfn_zx_clock_get))vdso_find(base, "zx_clock_get", "_zx_clock_get");
    pfn_zx_clock_get_monotonic= (decltype(pfn_zx_clock_get_monotonic))vdso_find(base, "zx_clock_get_monotonic", "_zx_clock_get_monotonic");
    pfn_zx_futex_wait         = (decltype(pfn_zx_futex_wait))vdso_find(base, "zx_futex_wait", "_zx_futex_wait");
    pfn_zx_futex_wake         = (decltype(pfn_zx_futex_wake))vdso_find(base, "zx_futex_wake", "_zx_futex_wake");
    pfn_zx_vmo_create_physical   = (decltype(pfn_zx_vmo_create_physical))vdso_find(base, "zx_vmo_create_physical", "_zx_vmo_create_physical");
    pfn_zx_vmo_create_contiguous = (decltype(pfn_zx_vmo_create_contiguous))vdso_find(base, "zx_vmo_create_contiguous", "_zx_vmo_create_contiguous");
    pfn_zx_iommu_create          = (decltype(pfn_zx_iommu_create))vdso_find(base, "zx_iommu_create", "_zx_iommu_create");
    pfn_zx_bti_create            = (decltype(pfn_zx_bti_create))vdso_find(base, "zx_bti_create", "_zx_bti_create");
    pfn_zx_bti_pin               = (decltype(pfn_zx_bti_pin))vdso_find(base, "zx_bti_pin", "_zx_bti_pin");
    pfn_zx_pmt_unpin             = (decltype(pfn_zx_pmt_unpin))vdso_find(base, "zx_pmt_unpin", "_zx_pmt_unpin");
    pfn_zx_object_wait_one       = (decltype(pfn_zx_object_wait_one))vdso_find(base, "zx_object_wait_one", "_zx_object_wait_one");
}

/* Exported ABI wrappers dispatching into kernel-registered vDSO */
extern "C" zx_status_t zx_channel_read(zx_handle_t handle, uint32_t options, void* bytes,
                                       zx_handle_t* handles, uint32_t num_bytes, uint32_t num_handles,
                                       uint32_t* actual_bytes, uint32_t* actual_handles) {
    if (pfn_zx_channel_read) {
        return pfn_zx_channel_read(handle, options, bytes, handles, num_bytes, num_handles, actual_bytes, actual_handles);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_channel_write(zx_handle_t handle, uint32_t options, const void* bytes,
                                        uint32_t num_bytes, const zx_handle_t* handles, uint32_t num_handles) {
    if (pfn_zx_channel_write) {
        return pfn_zx_channel_write(handle, options, bytes, num_bytes, handles, num_handles);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_channel_create(uint32_t options, zx_handle_t* out0, zx_handle_t* out1) {
    if (pfn_zx_channel_create) {
        return pfn_zx_channel_create(options, out0, out1);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_channel_call(zx_handle_t handle, uint32_t options, zx_time_t deadline,
                                       const zx_channel_call_args_t* args,
                                       uint32_t* actual_bytes, uint32_t* actual_handles) {
    if (pfn_zx_channel_call) {
        return pfn_zx_channel_call(handle, options, deadline, args, actual_bytes, actual_handles);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_handle_close(zx_handle_t handle) {
    if (pfn_zx_handle_close) {
        return pfn_zx_handle_close(handle);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_handle_duplicate(zx_handle_t handle, zx_rights_t rights, zx_handle_t* out) {
    if (pfn_zx_handle_duplicate) {
        return pfn_zx_handle_duplicate(handle, rights, out);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_debuglog_write(zx_handle_t handle, uint32_t options, const void* buffer, size_t buffer_size) {
    if (pfn_zx_debuglog_write) {
        return pfn_zx_debuglog_write(handle, options, buffer, buffer_size);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_vmo_create(uint64_t size, uint32_t options, zx_handle_t* out) {
    if (pfn_zx_vmo_create) {
        return pfn_zx_vmo_create(size, options, out);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_vmo_get_size(zx_handle_t handle, uint64_t* size) {
    if (pfn_zx_vmo_get_size) {
        return pfn_zx_vmo_get_size(handle, size);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_vmo_set_size(zx_handle_t handle, uint64_t size) {
    if (pfn_zx_vmo_set_size) {
        return pfn_zx_vmo_set_size(handle, size);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_vmo_read(zx_handle_t handle, void* data, uint64_t offset, size_t len) {
    if (pfn_zx_vmo_read) {
        return pfn_zx_vmo_read(handle, data, offset, len);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_vmo_write(zx_handle_t handle, const void* data, uint64_t offset, size_t len) {
    if (pfn_zx_vmo_write) {
        return pfn_zx_vmo_write(handle, data, offset, len);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_vmar_map(zx_handle_t vmar, zx_vm_option_t options, uint64_t vmar_offset,
                                   zx_handle_t vmo, uint64_t vmo_offset, uint64_t len, zx_vaddr_t* ptr) {
    if (pfn_zx_vmar_map) {
        return pfn_zx_vmar_map(vmar, options, vmar_offset, vmo, vmo_offset, len, ptr);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_vmar_unmap(zx_handle_t vmar, zx_vaddr_t addr, uint64_t len) {
    if (pfn_zx_vmar_unmap) {
        return pfn_zx_vmar_unmap(vmar, addr, len);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_vmar_allocate(zx_handle_t parent_vmar, zx_vm_option_t options,
                                        uint64_t offset, uint64_t size,
                                        zx_handle_t* child_vmar, zx_vaddr_t* child_addr) {
    if (pfn_zx_vmar_allocate) {
        return pfn_zx_vmar_allocate(parent_vmar, options, offset, size, child_vmar, child_addr);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_process_create(zx_handle_t job, const char* name, size_t name_len,
                                         uint32_t options, zx_handle_t* proc_handle, zx_handle_t* vmar_handle) {
    if (pfn_zx_process_create) {
        return pfn_zx_process_create(job, name, name_len, options, proc_handle, vmar_handle);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_process_start(zx_handle_t handle, zx_handle_t thread,
                                        zx_vaddr_t entry, zx_vaddr_t stack,
                                        zx_handle_t arg1, uintptr_t arg2) {
    if (pfn_zx_process_start) {
        return pfn_zx_process_start(handle, thread, entry, stack, arg1, arg2);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" void zx_process_exit(int64_t retcode) {
    if (pfn_zx_process_exit) {
        pfn_zx_process_exit(retcode);
    }
    for (;;) {
        __builtin_trap();
    }
}

extern "C" zx_status_t zx_thread_create(zx_handle_t process, const char* name, size_t name_len,
                                        uint32_t options, zx_handle_t* out) {
    if (pfn_zx_thread_create) {
        return pfn_zx_thread_create(process, name, name_len, options, out);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_thread_start(zx_handle_t handle, zx_vaddr_t thread_entry,
                                       zx_vaddr_t stack, uintptr_t arg1, uintptr_t arg2) {
    if (pfn_zx_thread_start) {
        return pfn_zx_thread_start(handle, thread_entry, stack, arg1, arg2);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" void zx_thread_exit(void) {
    if (pfn_zx_thread_exit) {
        pfn_zx_thread_exit();
    }
    for (;;) {
        __builtin_trap();
    }
}

extern "C" zx_status_t zx_object_set_property(zx_handle_t handle, uint32_t property, const void* value, size_t size) {
    if (pfn_zx_object_set_property) {
        return pfn_zx_object_set_property(handle, property, value, size);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_nanosleep(zx_time_t deadline) {
    if (pfn_zx_nanosleep) {
        return pfn_zx_nanosleep(deadline);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_time_t zx_clock_get(zx_clock_t clock_id) {
    if (pfn_zx_clock_get) {
        return pfn_zx_clock_get(clock_id);
    }
    return 0;
}

extern "C" zx_time_t zx_clock_get_monotonic(void) {
    if (pfn_zx_clock_get_monotonic) {
        return pfn_zx_clock_get_monotonic();
    }
    if (pfn_zx_clock_get) {
        return pfn_zx_clock_get(ZX_CLOCK_MONOTONIC);
    }
    return 0;
}

extern "C" zx_time_t zx_deadline_after(zx_duration_t nanoseconds) {
    zx_time_t now = zx_clock_get_monotonic();
    return (nanoseconds > (ZX_TIME_INFINITE - now)) ? ZX_TIME_INFINITE : (now + nanoseconds);
}

extern "C" zx_status_t zx_futex_wait(const zx_futex_t* value_ptr, int current_value, zx_time_t deadline) {
    if (pfn_zx_futex_wait) {
        return pfn_zx_futex_wait(value_ptr, current_value, deadline);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_futex_wake(const zx_futex_t* value_ptr, uint32_t count) {
    if (pfn_zx_futex_wake) {
        return pfn_zx_futex_wake(value_ptr, count);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_vmo_create_physical(zx_handle_t rsrc, zx_paddr_t paddr, size_t size, zx_handle_t* out) {
    if (pfn_zx_vmo_create_physical) return pfn_zx_vmo_create_physical(rsrc, paddr, size, out);
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_vmo_create_contiguous(zx_handle_t bti, size_t size, uint32_t alignment_log2, zx_handle_t* out) {
    if (pfn_zx_vmo_create_contiguous) return pfn_zx_vmo_create_contiguous(bti, size, alignment_log2, out);
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_iommu_create(zx_handle_t rsrc, uint32_t type, const void* desc, size_t desc_len, zx_handle_t* out) {
    if (pfn_zx_iommu_create) return pfn_zx_iommu_create(rsrc, type, desc, desc_len, out);
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_bti_create(zx_handle_t iommu, uint32_t options, uint64_t bti_id, zx_handle_t* out) {
    if (pfn_zx_bti_create) return pfn_zx_bti_create(iommu, options, bti_id, out);
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_bti_pin(zx_handle_t bti, uint32_t options, zx_handle_t vmo, uint64_t offset, uint64_t size, zx_paddr_t* addrs, size_t addrs_count, zx_handle_t* pmt) {
    if (pfn_zx_bti_pin) return pfn_zx_bti_pin(bti, options, vmo, offset, size, addrs, addrs_count, pmt);
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_pmt_unpin(zx_handle_t pmt) {
    if (pfn_zx_pmt_unpin) return pfn_zx_pmt_unpin(pmt);
    return ZX_ERR_NOT_SUPPORTED;
}

extern "C" zx_status_t zx_object_wait_one(zx_handle_t handle, zx_signals_t signals, zx_time_t deadline, zx_signals_t* observed) {
    if (pfn_zx_object_wait_one) return pfn_zx_object_wait_one(handle, signals, deadline, observed);
    return ZX_ERR_NOT_SUPPORTED;
}



/* ==============================================================================
 * mlibc Sysdeps Implementation
 * ============================================================================== */

namespace mlibc {

void sys_libc_log(const char *message) {
    size_t len = 0;
    while (message[len]) {
        len++;
    }
    ssize_t written = 0;
    sys_write(2, message, len, &written);
    sys_write(2, "\n", 1, &written);
}

[[noreturn]] void sys_libc_panic() {
    sys_libc_log("irisOS mlibc: unrecoverable panic!");
    sys_exit(1);
}

[[noreturn]] void sys_exit(int status) {
    zx_process_exit((int64_t)status);
    __builtin_unreachable();
}

int sys_tcb_set(void *pointer) {
#if defined(__x86_64__)
    zx_status_t status = zx_object_set_property(zx_thread_self(),
                                                ZX_PROP_REGISTER_FS,
                                                &pointer, sizeof(uintptr_t));
    if (status != ZX_OK) {
        return -1;
    }
    return 0;
#elif defined(__aarch64__)
    uintptr_t thread_data = reinterpret_cast<uintptr_t>(pointer) + sizeof(Tcb) - 0x10;
    asm volatile ("msr tpidr_el0, %0" :: "r"(thread_data) : "memory");
    return 0;
#else
    #error "Unsupported architecture for sys_tcb_set"
#endif
}

int sys_anon_allocate(size_t size, void **pointer) {
    zx_handle_t vmo = ZX_HANDLE_INVALID;
    zx_status_t status = zx_vmo_create((uint64_t)size, 0, &vmo);
    if (status != ZX_OK) {
        return ENOMEM;
    }

    zx_vaddr_t vaddr = 0;
    status = zx_vmar_map(zx_vmar_root_self(),
                         ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                         0, vmo, 0, (uint64_t)size, &vaddr);
    zx_handle_close(vmo);
    if (status != ZX_OK) {
        return ENOMEM;
    }

    *pointer = reinterpret_cast<void*>(vaddr);
    return 0;
}

int sys_anon_free(void *pointer, size_t size) {
    if (!pointer || size == 0) {
        return EINVAL;
    }
    zx_status_t status = zx_vmar_unmap(zx_vmar_root_self(),
                                       reinterpret_cast<zx_vaddr_t>(pointer),
                                       (uint64_t)size);
    if (status != ZX_OK) {
        return EINVAL;
    }
    return 0;
}

int sys_vm_map(void *hint, size_t size, int prot, int flags,
               int fd, off_t offset, void **window) {
    (void)fd;
    (void)offset;

    zx_vm_option_t zx_options = 0;
    if (prot & PROT_READ) zx_options |= ZX_VM_PERM_READ;
    if (prot & PROT_WRITE) zx_options |= ZX_VM_PERM_WRITE;
    if (prot & PROT_EXEC) zx_options |= ZX_VM_PERM_EXECUTE;
    if (flags & MAP_FIXED) zx_options |= ZX_VM_SPECIFIC;

    zx_handle_t vmo = ZX_HANDLE_INVALID;
    zx_status_t status = zx_vmo_create((uint64_t)size, 0, &vmo);
    if (status != ZX_OK) {
        return ENOMEM;
    }

    zx_vaddr_t vaddr = reinterpret_cast<zx_vaddr_t>(hint);
    status = zx_vmar_map(zx_vmar_root_self(), zx_options,
                         (flags & MAP_FIXED) ? vaddr : 0,
                         vmo, 0, (uint64_t)size, &vaddr);
    zx_handle_close(vmo);
    if (status != ZX_OK) {
        return ENOMEM;
    }

    *window = reinterpret_cast<void*>(vaddr);
    return 0;
}

int sys_vm_unmap(void *pointer, size_t size) {
    return sys_anon_free(pointer, size);
}

int sys_futex_wait(int *pointer, int expected, const struct timespec *time) {
    zx_time_t deadline = ZX_TIME_INFINITE;
    if (time) {
        zx_duration_t dur = (zx_duration_t)time->tv_sec * 1000000000ull + time->tv_nsec;
        deadline = zx_deadline_after(dur);
    }

    zx_status_t status = zx_futex_wait(reinterpret_cast<const zx_futex_t*>(pointer),
                                       expected, deadline);
    if (status == ZX_OK) {
        return 0;
    } else if (status == ZX_ERR_BAD_STATE) {
        return EAGAIN;
    } else if (status == ZX_ERR_TIMED_OUT) {
        return ETIMEDOUT;
    }
    return EINVAL;
}

int sys_futex_wake(int *pointer) {
    zx_status_t status = zx_futex_wake(reinterpret_cast<const zx_futex_t*>(pointer), 1);
    return (status == ZX_OK) ? 0 : -1;
}

int sys_write(int fd, const void *buf, size_t count, ssize_t *bytes_written) {
    iris_fd_entry_t* entry = iris_fd_get(fd);
    if (!entry || entry->type == IRIS_FD_CLOSED) {
        return EBADF;
    }

    if (entry->type == IRIS_FD_DEBUGLOG) {
        if (entry->handle == ZX_HANDLE_INVALID) {
            entry->handle = g_debuglog_handle;
            if (entry->handle == ZX_HANDLE_INVALID) {
                entry->handle = zx_take_startup_handle(PA_HND(PA_FDIO_LOGGER, 0));
                if (entry->handle == ZX_HANDLE_INVALID) {
                    entry->handle = zx_take_startup_handle(PA_HND(PA_FD, 1));
                }
                g_debuglog_handle = entry->handle;
            }
        }
        if (entry->handle != ZX_HANDLE_INVALID) {
            zx_debuglog_write(entry->handle, 0, buf, count);
        }
        *bytes_written = (ssize_t)count;
        return 0;
    }

    if (entry->type == IRIS_FD_CHANNEL) {
        if (entry->handle != ZX_HANDLE_INVALID) {
            zx_status_t st = zx_channel_write(entry->handle, 0, buf, (uint32_t)count, nullptr, 0);
            if (st != ZX_OK && g_debuglog_handle != ZX_HANDLE_INVALID) {
                zx_debuglog_write(g_debuglog_handle, 0, buf, count);
            }
        }
        *bytes_written = (ssize_t)count;
        return 0;
    }

    return EBADF;
}

int sys_read(int fd, void *buf, size_t count, ssize_t *bytes_read) {
    iris_fd_entry_t* entry = iris_fd_get(fd);
    if (!entry || entry->type == IRIS_FD_CLOSED) {
        return EBADF;
    }
    if (entry->type == IRIS_FD_DEBUGLOG) {
        *bytes_read = 0;
        return 0;
    }
    if (entry->type == IRIS_FD_CHANNEL) {
        if (entry->handle != ZX_HANDLE_INVALID) {
            uint32_t act_bytes = 0;
            uint32_t act_handles = 0;
            zx_status_t st = zx_channel_read(entry->handle, 0, buf, nullptr, (uint32_t)count, 0, &act_bytes, &act_handles);
            if (st == ZX_OK) {
                *bytes_read = (ssize_t)act_bytes;
                return 0;
            }
        }
        *bytes_read = 0;
        return 0;
    }
    return EBADF;
}

static int iris_strcmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return -1;
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int sys_open(const char *path, int flags, mode_t mode, int *fd) {
    (void)mode;
    if (!path || !fd) return EINVAL;

    // Standard character devices
    if (iris_strcmp(path, "/dev/null") == 0 || iris_strcmp(path, "/dev/console") == 0 ||
        iris_strcmp(path, "/dev/debuglog") == 0 || iris_strcmp(path, "/dev/zero") == 0) {
        int new_fd = iris_fd_alloc(g_debuglog_handle, IRIS_FD_DEBUGLOG, flags);
        if (new_fd < 0) {
            return EMFILE;
        }
        *fd = new_fd;
        return 0;
    }

    // Prepared for Phase 4 iris_fshost VFS IPC channel routing
    return ENOENT;
}

int sys_close(int fd) {
    if (iris_fd_close(fd) != 0) {
        return EBADF;
    }
    return 0;
}

int sys_seek(int fd, off_t offset, int whence, off_t *new_offset) {
    iris_fd_entry_t* entry = iris_fd_get(fd);
    if (!entry || entry->type == IRIS_FD_CLOSED) {
        return EBADF;
    }
    if (entry->type == IRIS_FD_DEBUGLOG) {
        return ESPIPE;
    }
    if (entry->type == IRIS_FD_CHANNEL) {
        if (whence == SEEK_SET) {
            entry->offset = offset;
        } else if (whence == SEEK_CUR) {
            entry->offset += offset;
        }
        if (new_offset) {
            *new_offset = entry->offset;
        }
        return 0;
    }
    return EBADF;
}

int sys_isatty(int fd) {
    iris_fd_entry_t* entry = iris_fd_get(fd);
    if (!entry || entry->type == IRIS_FD_CLOSED) {
        return ENOTTY;
    }
    if (entry->type == IRIS_FD_DEBUGLOG) {
        return 0; /* 0 in mlibc indicates isatty is true */
    }
    return ENOTTY;
}

int sys_fstat(int fd, struct stat *statbuf) {
    iris_fd_entry_t* entry = iris_fd_get(fd);
    if (!entry || entry->type == IRIS_FD_CLOSED) {
        return EBADF;
    }
    if (statbuf) {
        memset(statbuf, 0, sizeof(*statbuf));
        if (entry->type == IRIS_FD_DEBUGLOG) {
            statbuf->st_mode = S_IFCHR | 0666;
        } else {
            statbuf->st_mode = S_IFREG | 0644;
            statbuf->st_size = entry->offset;
        }
    }
    return 0;
}

#if !MLIBC_BUILDING_RTLD
int sys_spawn(pid_t *__restrict res, const char *__restrict path,
              const posix_spawn_file_actions_t *file_actions,
              const posix_spawnattr_t *__restrict attrs,
              char *const argv[], char *const envp[]) {
    (void)attrs;
    if (!path) return EINVAL;

    uint32_t argc = 0;
    while (argv && argv[argc]) argc++;

    uint32_t envc = 0;
    while (envp && envp[envc]) envc++;

    iris_spawn_action_t actions[IRIS_SPAWN_MAX_ACTIONS];
    size_t num_actions = 0;

    // Propagate open file descriptors from the client-side FD table
    for (int fd = 0; fd < IRIS_MAX_FD && num_actions < IRIS_SPAWN_MAX_ACTIONS; fd++) {
        iris_fd_entry_t* entry = iris_fd_get(fd);
        if (entry && entry->type != IRIS_FD_CLOSED && entry->handle != ZX_HANDLE_INVALID) {
            zx_handle_t child_h = ZX_HANDLE_INVALID;
            if (zx_handle_duplicate(entry->handle, ZX_RIGHT_SAME_RIGHTS, &child_h) == ZX_OK) {
                actions[num_actions].type = IRIS_SPAWN_ACTION_ADD_FD;
                actions[num_actions].handle = child_h;
                actions[num_actions].target_fd = fd;
                num_actions++;
            }
        }
    }

    // Process posix_spawn file actions if provided
    if (file_actions && file_actions->__actions) {
        struct fdop_header {
            struct fdop_header *next, *prev;
            int cmd;
            int fd, srcfd;
            char *path;
            int oflag;
            mode_t mode;
        };
        fdop_header* op = nullptr;
        for (op = (fdop_header*)file_actions->__actions; op->next; op = op->next);
        for (; op; op = op->prev) {
            if (op->cmd == 0 /* FDOP_CLOSE */) {
                for (size_t i = 0; i < num_actions; i++) {
                    if (actions[i].type == IRIS_SPAWN_ACTION_ADD_FD && actions[i].target_fd == op->fd) {
                        if (actions[i].handle != ZX_HANDLE_INVALID) {
                            zx_handle_close(actions[i].handle);
                            actions[i].handle = ZX_HANDLE_INVALID;
                        }
                    }
                }
            } else if (op->cmd == 1 /* FDOP_DUP2 */) {
                zx_handle_t src_h = ZX_HANDLE_INVALID;
                for (size_t i = 0; i < num_actions; i++) {
                    if (actions[i].type == IRIS_SPAWN_ACTION_ADD_FD && actions[i].target_fd == op->srcfd) {
                        src_h = actions[i].handle;
                        break;
                    }
                }
                if (src_h != ZX_HANDLE_INVALID && num_actions < IRIS_SPAWN_MAX_ACTIONS) {
                    zx_handle_t dup_h = ZX_HANDLE_INVALID;
                    if (zx_handle_duplicate(src_h, ZX_RIGHT_SAME_RIGHTS, &dup_h) == ZX_OK) {
                        actions[num_actions].type = IRIS_SPAWN_ACTION_ADD_FD;
                        actions[num_actions].handle = dup_h;
                        actions[num_actions].target_fd = op->fd;
                        num_actions++;
                    }
                }
            }
        }
    }

    iris_spawn_args_t sargs = {};
    sargs.job = zx_job_default();
    sargs.name = path;
    sargs.vmo_executable = ZX_HANDLE_INVALID; // In Phase 4, fetched via VFS
    sargs.vmo_vdso = g_vdso_vmo;
    sargs.argv = const_cast<const char* const*>(argv);
    sargs.argc = argc;
    sargs.envp = const_cast<const char* const*>(envp);
    sargs.envc = envc;
    sargs.actions = actions;
    sargs.num_actions = num_actions;

    zx_handle_t child_proc = ZX_HANDLE_INVALID;
    zx_handle_t child_thread = ZX_HANDLE_INVALID;
    zx_status_t status = iris_spawn(&sargs, &child_proc, &child_thread);
    if (status != ZX_OK) {
        for (size_t i = 0; i < num_actions; i++) {
            if (actions[i].handle != ZX_HANDLE_INVALID) {
                zx_handle_close(actions[i].handle);
            }
        }
        return (status == ZX_ERR_NO_MEMORY) ? ENOMEM :
               (status == ZX_ERR_NOT_FOUND) ? ENOENT :
               (status == ZX_ERR_INVALID_ARGS) ? EINVAL : EIO;
    }

    if (child_thread != ZX_HANDLE_INVALID) {
        zx_handle_close(child_thread);
    }

    if (res) {
        *res = static_cast<pid_t>(child_proc);
    }
    return 0;
}
#endif

int sys_stat(fsfd_target fsfdt, int fd, const char *path, int flags, struct stat *statbuf) {
    (void)fsfdt;
    (void)fd;
    (void)path;
    (void)flags;
    if (statbuf) {
        memset(statbuf, 0, sizeof(*statbuf));
        statbuf->st_mode = S_IFCHR | 0666;
    }
    return 0;
}

int sys_clock_get(int clock, time_t *secs, long *nanos) {
    (void)clock;
    zx_time_t now = zx_clock_get_monotonic();
    if (secs) {
        *secs = static_cast<time_t>(now / 1000000000ULL);
    }
    if (nanos) {
        *nanos = static_cast<long>(now % 1000000000ULL);
    }
    return 0;
}

int sys_clock_getres(int clock, time_t *secs, long *nanos) {
    (void)clock;
    if (secs) *secs = 0;
    if (nanos) *nanos = 1;
    return 0;
}

} // namespace mlibc
