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
 *     - The essential root capabilities required by Section 4.4 are extracted:
 *         * `PA_HND(PA_RESOURCE, 0)`    -> Root Resource handle (for MMIO, IRQs, ports)
 *         * `PA_HND(PA_VMO_BOOTFS, 0)`   -> BOOTFS VMO handle (early boot files/drivers)
 *         * `PA_HND(PA_JOB_DEFAULT, 0)`  -> Root Job handle (child process/job isolation)
 *         * `PA_HND(PA_VMO_VDSO, 0)`     -> vDSO VMO handle (libzircon.so system calls)
 *         * `PA_HND(PA_VMAR_ROOT, 0)`    -> Process Root VMAR handle
 *
 *  4. Single-Buffer Systemd-Style Serial Logging:
 *     - Creates an early kernel debug log handle using `zx_debuglog_create(root_resource, 0, &log)`
 *       to stream early bootstrap diagnostics to UART/console before `iris_pty` is initialized.
 *     - Formats every log message into a single line buffer via `snprintf` before calling
 *       `zx_debuglog_write()` to eliminate split lines and fragmented timestamp prefixes.
 *     - Displays the irisOS ASCII banner, node identity (`irisOS-dev`), and standardized
 *       `[  OK  ]` status lines.
 *
 *  5. Child Process Spawning & Driver Verification:
 *     - Locates and executes `bin/its_irisOS` (Interactive Test Suite) from BOOTFS.
 *     - Discovers PCIe VirtIO Block hardware via ECAM, spawns `bin/virtio_blk`, and
 *       verifies sector read/write over IPC (`iris_msg_hdr_t`).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#include <zircon/types.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/log.h>

#include <iris/ipc.h>
#include <iris/pci.h>
#include <servers/drivers/pci/pci.h>

#ifndef PA_FD
#define PA_FD 0x30u
#endif

#define IRIS_BOOTSTRAP_MAX_HANDLES 64
#define IRIS_BOOTSTRAP_MSG_BYTES   4096

#define BOOTFS_MAGIC               (0xa56d3ff9u)
#define PAGE_SIZE                  (4096ULL)
#define PAGE_MASK                  (~(PAGE_SIZE - 1ULL))

#if defined(__x86_64__)
#define IRIS_ARCH_NAME "x86_64"
#elif defined(__aarch64__)
#define IRIS_ARCH_NAME "arm64"
#else
#define IRIS_ARCH_NAME "unknown"
#endif

/* Global capabilities extracted from userboot bootstrap channel */
static zx_handle_t g_root_resource   = ZX_HANDLE_INVALID;
static zx_handle_t g_bootfs_vmo      = ZX_HANDLE_INVALID;
static zx_handle_t g_root_job        = ZX_HANDLE_INVALID;
static zx_handle_t g_vdso_vmo        = ZX_HANDLE_INVALID;
static zx_handle_t g_vmar_root_self  = ZX_HANDLE_INVALID;
static zx_handle_t g_debuglog_handle = ZX_HANDLE_INVALID;
static uintptr_t   g_vdso_base_self  = 0;

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

static void* vdso_find(uintptr_t base, const char* name, const char* name_with_underscore) {
    void* ptr = vdso_lookup(base, name_with_underscore);
    if (!ptr) {
        ptr = vdso_lookup(base, name);
    }
    return ptr;
}

/* Function pointers to vDSO syscall implementations */
static zx_status_t (*g_zx_channel_read)(
    zx_handle_t handle, uint32_t options, void* bytes,
    zx_handle_t* handles, uint32_t num_bytes, uint32_t num_handles,
    uint32_t* actual_bytes, uint32_t* actual_handles) = NULL;

static zx_status_t (*g_zx_channel_create)(uint32_t flags, zx_handle_t* out0, zx_handle_t* out1) = NULL;
static zx_status_t (*g_zx_channel_write)(zx_handle_t handle, uint32_t options, const void* bytes, uint32_t num_bytes,
                                        const zx_handle_t* handles, uint32_t num_handles) = NULL;
static zx_status_t (*g_zx_handle_close)(zx_handle_t handle) = NULL;
static zx_status_t (*g_zx_handle_duplicate)(zx_handle_t handle, zx_rights_t rights, zx_handle_t* out) = NULL;
static zx_status_t (*g_zx_debuglog_create)(zx_handle_t resource, uint32_t options, zx_handle_t* out) = NULL;
static zx_status_t (*g_zx_debuglog_write)(zx_handle_t handle, uint32_t options, const void* buffer, size_t buffer_size) = NULL;
static zx_status_t (*g_zx_vmo_create)(uint64_t size, uint32_t options, zx_handle_t* out) = NULL;
static zx_status_t (*g_zx_vmo_get_size)(zx_handle_t handle, uint64_t* size) = NULL;
static zx_status_t (*g_zx_vmo_write)(zx_handle_t handle, const void* buffer, uint64_t offset, size_t buffer_size) = NULL;
static zx_status_t (*g_zx_vmo_replace_as_executable)(zx_handle_t vmo, zx_handle_t vmex, zx_handle_t* out) = NULL;
static zx_status_t (*g_zx_vmar_map)(zx_handle_t vmar, zx_vm_option_t options, uint64_t vmar_offset,
                                    zx_handle_t vmo, uint64_t vmo_offset, uint64_t len, zx_vaddr_t* ptr) = NULL;
static zx_status_t (*g_zx_vmar_allocate)(zx_handle_t parent_vmar, zx_vm_option_t options, uint64_t offset,
                                         uint64_t size, zx_handle_t* child_vmar, zx_vaddr_t* child_addr) = NULL;
static zx_status_t (*g_zx_process_create)(zx_handle_t job, const char* name, size_t name_len,
                                         uint32_t options, zx_handle_t* proc, zx_handle_t* vmar) = NULL;
static zx_status_t (*g_zx_thread_create)(zx_handle_t process, const char* name, size_t name_len,
                                        uint32_t options, zx_handle_t* thread) = NULL;
static zx_status_t (*g_zx_process_start)(zx_handle_t process, zx_handle_t thread, zx_vaddr_t entry,
                                        zx_vaddr_t stack, zx_handle_t arg1, uintptr_t arg2) = NULL;
static zx_status_t (*g_zx_object_wait_one)(zx_handle_t handle, zx_signals_t signals,
                                          zx_time_t deadline, zx_signals_t* observed) = NULL;
static zx_status_t (*g_zx_nanosleep)(zx_time_t deadline) = NULL;
static zx_status_t (*g_zx_vmo_create_physical)(zx_handle_t resource, uintptr_t paddr, size_t size, zx_handle_t* out) = NULL;

static void vdso_init(uintptr_t base) {
    g_zx_channel_read       = (void*)vdso_find(base, "zx_channel_read", "_zx_channel_read");
    g_zx_channel_create     = (void*)vdso_find(base, "zx_channel_create", "_zx_channel_create");
    g_zx_channel_write      = (void*)vdso_find(base, "zx_channel_write", "_zx_channel_write");
    g_zx_handle_close       = (void*)vdso_find(base, "zx_handle_close", "_zx_handle_close");
    g_zx_handle_duplicate   = (void*)vdso_find(base, "zx_handle_duplicate", "_zx_handle_duplicate");
    g_zx_debuglog_create    = (void*)vdso_find(base, "zx_debuglog_create", "_zx_debuglog_create");
    g_zx_debuglog_write     = (void*)vdso_find(base, "zx_debuglog_write", "_zx_debuglog_write");
    g_zx_vmo_create         = (void*)vdso_find(base, "zx_vmo_create", "_zx_vmo_create");
    g_zx_vmo_get_size       = (void*)vdso_find(base, "zx_vmo_get_size", "_zx_vmo_get_size");
    g_zx_vmo_write          = (void*)vdso_find(base, "zx_vmo_write", "_zx_vmo_write");
    g_zx_vmo_replace_as_executable = (void*)vdso_find(base, "zx_vmo_replace_as_executable", "_zx_vmo_replace_as_executable");
    g_zx_vmar_map           = (void*)vdso_find(base, "zx_vmar_map", "_zx_vmar_map");
    g_zx_vmar_allocate      = (void*)vdso_find(base, "zx_vmar_allocate", "_zx_vmar_allocate");
    g_zx_process_create     = (void*)vdso_find(base, "zx_process_create", "_zx_process_create");
    g_zx_thread_create      = (void*)vdso_find(base, "zx_thread_create", "_zx_thread_create");
    g_zx_process_start      = (void*)vdso_find(base, "zx_process_start", "_zx_process_start");
    g_zx_object_wait_one    = (void*)vdso_find(base, "zx_object_wait_one", "_zx_object_wait_one");
    g_zx_nanosleep          = (void*)vdso_find(base, "zx_nanosleep", "_zx_nanosleep");
    g_zx_vmo_create_physical= (void*)vdso_find(base, "zx_vmo_create_physical", "_zx_vmo_create_physical");
}

/* ==============================================================================
 * Minimal Freestanding Memory & Formatting Helpers (snprintf)
 * ============================================================================== */

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

static int iris_memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* p1 = (const uint8_t*)s1;
    const uint8_t* p2 = (const uint8_t*)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

static size_t iris_strlen(const char* s) {
    size_t len = 0;
    if (!s) return 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

static void buf_putc(char* buf, size_t size, size_t* idx, char c) {
    if (size > 0 && *idx + 1 < size) {
        buf[*idx] = c;
    }
    (*idx)++;
}

static int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    size_t idx = 0;
    if (!fmt) {
        if (size > 0) buf[0] = '\0';
        return 0;
    }

    while (*fmt) {
        if (*fmt != '%') {
            buf_putc(buf, size, &idx, *fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        char pad_char = ' ';
        if (*fmt == '0') {
            pad_char = '0';
            fmt++;
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int is_long = 0;
        if (*fmt == 'z') {
            is_long = 2;
            fmt++;
        } else {
            while (*fmt == 'l') {
                is_long++;
                fmt++;
            }
        }

        char spec = *fmt ? *fmt++ : '\0';
        switch (spec) {
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                while (*s) {
                    buf_putc(buf, size, &idx, *s++);
                }
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                buf_putc(buf, size, &idx, c);
                break;
            }
            case 'd':
            case 'i': {
                int64_t val = (is_long >= 1) ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
                uint64_t uval;
                if (val < 0) {
                    buf_putc(buf, size, &idx, '-');
                    uval = (uint64_t)(-(val + 1)) + 1u;
                    if (width > 0) width--;
                } else {
                    uval = (uint64_t)val;
                }
                char tmp[24];
                int n = 0;
                do {
                    tmp[n++] = (char)('0' + (uval % 10u));
                    uval /= 10u;
                } while (uval > 0);
                while (n < width) {
                    buf_putc(buf, size, &idx, pad_char);
                    width--;
                }
                while (n > 0) {
                    buf_putc(buf, size, &idx, tmp[--n]);
                }
                break;
            }
            case 'u': {
                uint64_t uval = (is_long >= 1) ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                char tmp[24];
                int n = 0;
                do {
                    tmp[n++] = (char)('0' + (uval % 10u));
                    uval /= 10u;
                } while (uval > 0);
                while (n < width) {
                    buf_putc(buf, size, &idx, pad_char);
                    width--;
                }
                while (n > 0) {
                    buf_putc(buf, size, &idx, tmp[--n]);
                }
                break;
            }
            case 'x':
            case 'X': {
                uint64_t uval = (is_long >= 1) ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                const char* hex = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                char tmp[24];
                int n = 0;
                do {
                    tmp[n++] = hex[uval & 0xFu];
                    uval >>= 4;
                } while (uval > 0);
                while (n < width) {
                    buf_putc(buf, size, &idx, pad_char);
                    width--;
                }
                while (n > 0) {
                    buf_putc(buf, size, &idx, tmp[--n]);
                }
                break;
            }
            case '%':
                buf_putc(buf, size, &idx, '%');
                break;
            default:
                buf_putc(buf, size, &idx, spec);
                break;
        }
    }

    if (size > 0) {
        buf[idx < size ? idx : size - 1] = '\0';
    }
    return (int)idx;
}

static int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

/**
 * @brief Formats a message into a single line buffer using snprintf and writes it
 *        atomically to zx_debuglog_write() to eliminate split lines.
 */
static void log_printf(const char* fmt, ...) {
    if (g_debuglog_handle == ZX_HANDLE_INVALID || !g_zx_debuglog_write) {
        return;
    }
    char line_buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line_buf, sizeof(line_buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line_buf) ? (size_t)n : sizeof(line_buf) - 1;
        g_zx_debuglog_write(g_debuglog_handle, 0, line_buf, len);
    }
}

/* ==============================================================================
 * BOOTFS Entry Structures
 * ============================================================================== */

typedef struct {
    uint32_t magic;
    uint32_t dirsize;
    uint32_t reserved0;
    uint32_t reserved1;
} bootfs_header_t;

typedef struct {
    uint32_t name_len;
    uint32_t data_len;
    uint32_t data_off;
    char name[];
} bootfs_entry_t;

/**
 * @brief Locates and launches an ELF executable from the BOOTFS VMO.
 */
static zx_status_t spawn_elf_binary(
    const char* name,
    const char* path,
    zx_handle_t extra_handle1,
    uint32_t extra_info1,
    zx_handle_t extra_handle2,
    uint32_t extra_info2,
    zx_handle_t* out_proc
) {
    char line_buf[256];

    if (g_bootfs_vmo == ZX_HANDLE_INVALID || g_root_job == ZX_HANDLE_INVALID ||
        g_vdso_vmo == ZX_HANDLE_INVALID || g_vmar_root_self == ZX_HANDLE_INVALID) {
        int len = snprintf(line_buf, sizeof(line_buf),
                           "[FAILED] iris_init: Missing required capabilities to spawn %s.", path);
        g_zx_debuglog_write(g_debuglog_handle, 0, line_buf, (size_t)len);
        return ZX_ERR_BAD_STATE;
    }

    /* 1. Map BOOTFS VMO to inspect directory */
    uint64_t bootfs_size = 0;
    zx_status_t status = g_zx_vmo_get_size(g_bootfs_vmo, &bootfs_size);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: Failed to query BOOTFS size (%d).", status);
        return status;
    }

    zx_vaddr_t bootfs_addr = 0;
    status = g_zx_vmar_map(g_vmar_root_self, ZX_VM_PERM_READ, 0,
                           g_bootfs_vmo, 0, bootfs_size, &bootfs_addr);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: Failed to map BOOTFS VMO (%d).", status);
        return status;
    }

    const bootfs_header_t* hdr = (const bootfs_header_t*)bootfs_addr;
    if (hdr->magic != BOOTFS_MAGIC) {
        log_printf("[FAILED] iris_init: Invalid BOOTFS magic (0x%08x).", hdr->magic);
        return ZX_ERR_IO;
    }

    /* 2. Search BOOTFS for binary path */
    const uint8_t* p = (const uint8_t*)(hdr + 1);
    const uint8_t* end = p + hdr->dirsize;
    uint32_t file_off = 0;
    uint32_t file_len = 0;
    size_t path_len = iris_strlen(path);
    size_t target_len = path_len + 1; /* BOOTFS name_len includes null terminator */

    while (p < end) {
        const bootfs_entry_t* e = (const bootfs_entry_t*)p;
        size_t recsize = sizeof(bootfs_entry_t) + ((e->name_len + 3u) & ~3u);
        if (e->name_len == target_len && iris_memcmp(e->name, path, target_len) == 0) {
            file_off = e->data_off;
            file_len = e->data_len;
            break;
        }
        p += recsize;
    }

    if (file_off == 0 || file_len == 0) {
        log_printf("[FAILED] iris_init: %s not found in BOOTFS.", path);
        return ZX_ERR_NOT_FOUND;
    }

    log_printf("[  OK  ] Located %s in BOOTFS (%u bytes).", path, file_len);

    /* 3. Create Child Process & Root VMAR */
    size_t name_len = iris_strlen(name);

    zx_handle_t child_proc = ZX_HANDLE_INVALID;
    zx_handle_t child_vmar = ZX_HANDLE_INVALID;
    status = g_zx_process_create(g_root_job, name, (uint32_t)name_len, 0, &child_proc, &child_vmar);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: zx_process_create failed for %s (%d).", name, status);
        return status;
    }

    /* 4. Create Initial Thread */
    zx_handle_t child_thread = ZX_HANDLE_INVALID;
    status = g_zx_thread_create(child_proc, name, (uint32_t)name_len, 0, &child_thread);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: zx_thread_create failed for %s (%d).", name, status);
        g_zx_handle_close(child_vmar);
        g_zx_handle_close(child_proc);
        return status;
    }

    /* 5. Parse ELF Headers and Map Segments into Child */
    const uint8_t* elf_data = (const uint8_t*)bootfs_addr + file_off;
    const Elf64_Ehdr_t* ehdr = (const Elf64_Ehdr_t*)elf_data;

    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        log_printf("[FAILED] iris_init: Invalid ELF magic in %s.", path);
        g_zx_handle_close(child_thread);
        g_zx_handle_close(child_vmar);
        g_zx_handle_close(child_proc);
        return ZX_ERR_NOT_SUPPORTED;
    }

    const Elf64_Phdr_t* phdrs = (const Elf64_Phdr_t*)(elf_data + ehdr->e_phoff);
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == 1 /* PT_LOAD */) {
            uint64_t s = phdrs[i].p_vaddr & PAGE_MASK;
            uint64_t e = (phdrs[i].p_vaddr + phdrs[i].p_memsz + PAGE_SIZE - 1u) & PAGE_MASK;
            if (s < min_vaddr) min_vaddr = s;
            if (e > max_vaddr) max_vaddr = e;
        }
    }

    size_t span = max_vaddr - min_vaddr;
    zx_handle_t prog_vmar = ZX_HANDLE_INVALID;
    zx_vaddr_t prog_base = 0;
    uint64_t load_bias = 0;

    if (ehdr->e_type == 3 /* ET_DYN (PIE) */) {
        status = g_zx_vmar_allocate(child_vmar,
                                    ZX_VM_CAN_MAP_READ | ZX_VM_CAN_MAP_WRITE |
                                    ZX_VM_CAN_MAP_EXECUTE | ZX_VM_CAN_MAP_SPECIFIC,
                                    0, span, &prog_vmar, &prog_base);
        if (status != ZX_OK) {
            log_printf("[FAILED] iris_init: zx_vmar_allocate failed for %s (%d).", name, status);
            g_zx_handle_close(child_thread);
            g_zx_handle_close(child_vmar);
            g_zx_handle_close(child_proc);
            return status;
        }
        load_bias = prog_base - min_vaddr;
    } else {
        prog_vmar = child_vmar;
        load_bias = 0;
    }

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr_t* ph = &phdrs[i];
        if (ph->p_type != 1 /* PT_LOAD */ || ph->p_memsz == 0) continue;

        uint64_t seg_vaddr = ph->p_vaddr + load_bias;
        uint64_t map_vaddr = seg_vaddr & PAGE_MASK;
        uint64_t page_offset = seg_vaddr - map_vaddr;
        uint64_t seg_mem_total = (ph->p_memsz + page_offset + PAGE_SIZE - 1u) & PAGE_MASK;

        zx_handle_t seg_vmo = ZX_HANDLE_INVALID;
        status = g_zx_vmo_create(seg_mem_total, 0, &seg_vmo);
        if (status != ZX_OK) {
            log_printf("[FAILED] iris_init: zx_vmo_create for segment failed (%d).", status);
            return status;
        }

        if (ph->p_filesz > 0) {
            g_zx_vmo_write(seg_vmo, elf_data + ph->p_offset, page_offset, ph->p_filesz);
        }

        zx_vm_option_t perms = ZX_VM_SPECIFIC;
        if (ph->p_flags & 4 /* PF_R */) perms |= ZX_VM_PERM_READ;
        if (ph->p_flags & 2 /* PF_W */) perms |= ZX_VM_PERM_WRITE;
        if (ph->p_flags & 1 /* PF_X */) {
            perms |= ZX_VM_PERM_EXECUTE;
            if (g_zx_vmo_replace_as_executable) {
                (void)g_zx_vmo_replace_as_executable(seg_vmo, ZX_HANDLE_INVALID, &seg_vmo);
            }
        }

        uint64_t map_offset = map_vaddr - (ehdr->e_type == 3 ? prog_base : 0);
        zx_vaddr_t actual = 0;
        status = g_zx_vmar_map(prog_vmar, perms, map_offset, seg_vmo, 0, seg_mem_total, &actual);
        g_zx_handle_close(seg_vmo);

        if (status != ZX_OK) {
            log_printf("[FAILED] iris_init: zx_vmar_map for segment failed (%d).", status);
            return status;
        }
    }

    if (ehdr->e_type == 3) {
        g_zx_handle_close(prog_vmar);
    }

    zx_vaddr_t entry_point = ehdr->e_entry + load_bias;

    /* 6. Map vDSO VMO into Child VMAR using ELF program headers */
    const Elf64_Ehdr_t* v_ehdr = (const Elf64_Ehdr_t*)g_vdso_base_self;
    const Elf64_Phdr_t* v_phdrs = (const Elf64_Phdr_t*)((const uint8_t*)g_vdso_base_self + v_ehdr->e_phoff);
    uint64_t v_min = UINT64_MAX, v_max = 0;
    for (uint16_t j = 0; j < v_ehdr->e_phnum; j++) {
        if (v_phdrs[j].p_type == 1 /* PT_LOAD */) {
            uint64_t s = v_phdrs[j].p_vaddr & PAGE_MASK;
            uint64_t e = (v_phdrs[j].p_vaddr + v_phdrs[j].p_memsz + PAGE_SIZE - 1ULL) & PAGE_MASK;
            if (s < v_min) v_min = s;
            if (e > v_max) v_max = e;
        }
    }
    size_t v_span = v_max - v_min;
    zx_handle_t vdso_sub_vmar = ZX_HANDLE_INVALID;
    zx_vaddr_t child_vdso_base = 0;
    status = g_zx_vmar_allocate(child_vmar,
                                ZX_VM_CAN_MAP_READ | ZX_VM_CAN_MAP_EXECUTE | ZX_VM_CAN_MAP_SPECIFIC,
                                0, v_span, &vdso_sub_vmar, &child_vdso_base);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: Failed to allocate vDSO sub-VMAR (%d).", status);
        return status;
    }

    for (uint16_t j = 0; j < v_ehdr->e_phnum; j++) {
        const Elf64_Phdr_t* ph = &v_phdrs[j];
        if (ph->p_type != 1 /* PT_LOAD */) continue;
        zx_vm_option_t p = ZX_VM_SPECIFIC | ZX_VM_PERM_READ;
        if (ph->p_flags & 1 /* PF_X */) p |= ZX_VM_PERM_EXECUTE;
        uint64_t s_off = ph->p_vaddr & PAGE_MASK;
        uint64_t s_len = (ph->p_memsz + PAGE_SIZE - 1ULL) & PAGE_MASK;
        zx_vaddr_t a = 0;
        status = g_zx_vmar_map(vdso_sub_vmar, p, s_off - v_min, g_vdso_vmo, ph->p_offset & PAGE_MASK, s_len, &a);
        if (status != ZX_OK) {
            log_printf("[FAILED] iris_init: Failed to map vDSO segment (%d).", status);
            return status;
        }
    }
    g_zx_handle_close(vdso_sub_vmar);

    /* 7. Allocate and Map Initial Stack VMO (256 KB) */
    size_t stack_size = 256u * 1024u;
    zx_handle_t stack_vmo = ZX_HANDLE_INVALID;
    g_zx_vmo_create(stack_size, 0, &stack_vmo);

    zx_vaddr_t stack_base = 0;
    status = g_zx_vmar_map(child_vmar, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0,
                           stack_vmo, 0, stack_size, &stack_base);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: Failed to map stack into child (%d).", status);
        return status;
    }

    zx_vaddr_t sp = (stack_base + stack_size) & ~(uintptr_t)0xF;
#if defined(__x86_64__)
    sp -= 8;
#endif

    /* 8. Assemble Bootstrap Message & Handle Table */
    zx_handle_t hnd_array[24];
    uint32_t info_array[24];
    uint32_t num_h = 0;

    g_zx_handle_duplicate(child_proc, ZX_RIGHT_SAME_RIGHTS, &hnd_array[num_h]);
    info_array[num_h++] = PA_HND(PA_PROC_SELF, 0);

    g_zx_handle_duplicate(child_thread, ZX_RIGHT_SAME_RIGHTS, &hnd_array[num_h]);
    info_array[num_h++] = PA_HND(PA_THREAD_SELF, 0);

    g_zx_handle_duplicate(child_vmar, ZX_RIGHT_SAME_RIGHTS, &hnd_array[num_h]);
    info_array[num_h++] = PA_HND(PA_VMAR_ROOT, 0);

    g_zx_handle_duplicate(stack_vmo, ZX_RIGHT_SAME_RIGHTS, &hnd_array[num_h]);
    info_array[num_h++] = PA_HND(PA_VMO_STACK, 0);

    g_zx_handle_duplicate(g_vdso_vmo, ZX_RIGHT_SAME_RIGHTS, &hnd_array[num_h]);
    info_array[num_h++] = PA_HND(PA_VMO_VDSO, 0);

    g_zx_handle_duplicate(g_root_job, ZX_RIGHT_SAME_RIGHTS, &hnd_array[num_h]);
    info_array[num_h++] = PA_HND(PA_JOB_DEFAULT, 0);

    /* Transfer debuglog handle as PA_FDIO_LOGGER so printf output routes directly to UART console */
    g_zx_handle_duplicate(g_debuglog_handle, ZX_RIGHT_SAME_RIGHTS, &hnd_array[num_h]);
    info_array[num_h++] = PA_HND(PA_FDIO_LOGGER, 0);

    /* Transfer standard inherited file descriptors 0 (stdin), 1 (stdout), 2 (stderr) */
    g_zx_handle_duplicate(g_debuglog_handle, ZX_RIGHT_SAME_RIGHTS, &hnd_array[num_h]);
    info_array[num_h++] = PA_HND(PA_FD, 0);

    g_zx_handle_duplicate(g_debuglog_handle, ZX_RIGHT_SAME_RIGHTS, &hnd_array[num_h]);
    info_array[num_h++] = PA_HND(PA_FD, 1);

    g_zx_handle_duplicate(g_debuglog_handle, ZX_RIGHT_SAME_RIGHTS, &hnd_array[num_h]);
    info_array[num_h++] = PA_HND(PA_FD, 2);

    /* Transfer BOOTFS VMO handle for child process asset/executable access */
    g_zx_handle_duplicate(g_bootfs_vmo, ZX_RIGHT_SAME_RIGHTS, &hnd_array[num_h]);
    info_array[num_h++] = PA_HND(PA_VMO_BOOTFS, 0);

    /* Transfer optional extra handles (e.g. server channel, root resource) */
    if (extra_handle1 != ZX_HANDLE_INVALID) {
        hnd_array[num_h] = extra_handle1;
        info_array[num_h] = extra_info1;
        num_h++;
    }
    if (extra_handle2 != ZX_HANDLE_INVALID) {
        hnd_array[num_h] = extra_handle2;
        info_array[num_h] = extra_info2;
        num_h++;
    }

    uint8_t msg_buf[1024];
    iris_memset(msg_buf, 0, sizeof(msg_buf));
    zx_proc_args_t* pargs = (zx_proc_args_t*)msg_buf;
    pargs->protocol = ZX_PROCARGS_PROTOCOL;
    pargs->version = ZX_PROCARGS_VERSION;
    pargs->handle_info_off = sizeof(zx_proc_args_t);
    iris_memcpy(msg_buf + sizeof(zx_proc_args_t), info_array, num_h * sizeof(uint32_t));
    size_t msg_len = sizeof(zx_proc_args_t) + num_h * sizeof(uint32_t);

    zx_handle_t bstrap_tx = ZX_HANDLE_INVALID;
    zx_handle_t bstrap_rx = ZX_HANDLE_INVALID;
    g_zx_channel_create(0, &bstrap_tx, &bstrap_rx);
    g_zx_channel_write(bstrap_tx, 0, msg_buf, (uint32_t)msg_len, hnd_array, num_h);
    g_zx_handle_close(bstrap_tx);

    /* 9. Start Child Process */
    status = g_zx_process_start(child_proc, child_thread, entry_point, sp, bstrap_rx, child_vdso_base);
    g_zx_handle_close(stack_vmo);
    g_zx_handle_close(child_vmar);
    g_zx_handle_close(child_thread);

    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: zx_process_start failed for %s (%d).", name, status);
        g_zx_handle_close(child_proc);
        return status;
    }

    log_printf("[  OK  ] Started process %s (%s).", name, path);

    if (out_proc != NULL) {
        *out_proc = child_proc;
    } else {
        g_zx_handle_close(child_proc);
    }
    return ZX_OK;
}

/**
 * @brief Locates and launches bin/its_irisOS (Interactive Test Suite) from BOOTFS.
 */
static void spawn_its_irisOS(void) {
    zx_handle_t child_proc = ZX_HANDLE_INVALID;
    zx_status_t status = spawn_elf_binary("its_irisOS", "bin/its_irisOS",
                                          ZX_HANDLE_INVALID, 0,
                                          ZX_HANDLE_INVALID, 0,
                                          &child_proc);
    if (status == ZX_OK && child_proc != ZX_HANDLE_INVALID) {
        zx_signals_t observed = 0;
        g_zx_object_wait_one(child_proc, ZX_PROCESS_TERMINATED, ZX_TIME_INFINITE, &observed);
        log_printf("[  OK  ] Process its_irisOS finished execution.");
        g_zx_handle_close(child_proc);
    }
}

/**
 * @brief Phase 4 Step 4.1: Unified PCIe Discovery & Ring 3 VirtIO Block Driver Verification.
 */
static void test_virtio_blk(void) {
    /* 1. Map PCIe ECAM configuration space */
    uint64_t ecam_phys = pci_get_default_ecam_base();

    zx_handle_t ecam_vmo = ZX_HANDLE_INVALID;
    zx_status_t status = g_zx_vmo_create_physical(g_root_resource, ecam_phys, PCI_ECAM_SIZE_16_BUSES, &ecam_vmo);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: zx_vmo_create_physical failed for ECAM (%d).", status);
        return;
    }

    zx_vaddr_t ecam_vaddr = 0;
    status = g_zx_vmar_map(g_vmar_root_self,
                           ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_MAP_RANGE,
                           0, ecam_vmo, 0, PCI_ECAM_SIZE_16_BUSES, &ecam_vaddr);
    g_zx_handle_close(ecam_vmo);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: zx_vmar_map failed for ECAM (%d).", status);
        return;
    }

    log_printf("[  OK  ] Mapped PCIe ECAM configuration space at phys 0x%016llx.",
               (unsigned long long)ecam_phys);

    /* 2. Scan PCIe for VirtIO Block Device */
    virtio_pci_dev_t blk_dev;
    iris_memset(&blk_dev, 0, sizeof(blk_dev));
    if (!pci_find_virtio_device(ecam_vaddr, 16, 0, &blk_dev)) {
        log_printf("[FAILED] iris_init: No VirtIO Block device found on PCIe bus.");
        return;
    }

    log_printf("[  OK  ] Discovered VirtIO Block Device at PCI %02x:%02x.%x (Vendor: 0x%04x, Device: 0x%04x).",
               blk_dev.bus, blk_dev.dev, blk_dev.func, blk_dev.vendor_id, blk_dev.device_id);

    /* 3. Create Driver Control Channel Pair */
    zx_handle_t client_ch = ZX_HANDLE_INVALID;
    zx_handle_t server_ch = ZX_HANDLE_INVALID;
    status = g_zx_channel_create(0, &client_ch, &server_ch);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: zx_channel_create failed (%d).", status);
        return;
    }

    /* 4. Spawn bin/virtio_blk from BOOTFS */
    zx_handle_t root_res_dup = ZX_HANDLE_INVALID;
    g_zx_handle_duplicate(g_root_resource, ZX_RIGHT_SAME_RIGHTS, &root_res_dup);

    zx_handle_t blk_proc = ZX_HANDLE_INVALID;
    status = spawn_elf_binary("virtio_blk", "bin/virtio_blk",
                              server_ch, PA_HND(PA_USER0, 0),
                              root_res_dup, PA_HND(PA_RESOURCE, 0),
                              &blk_proc);
    g_zx_handle_close(server_ch);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: Failed to spawn bin/virtio_blk (%d).", status);
        g_zx_handle_close(client_ch);
        return;
    }

    /* 5. Query Block Device Info via IPC */
    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_DRIVER, IRIS_BLK_OP_GET_INFO, 1, 0);

    status = g_zx_channel_write(client_ch, 0, &req_hdr, sizeof(req_hdr), NULL, 0);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: zx_channel_write failed for BLK_GET_INFO (%d).", status);
        return;
    }

    zx_signals_t observed = 0;
    status = g_zx_object_wait_one(client_ch, ZX_CHANNEL_READABLE, ZX_TIME_INFINITE, &observed);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: Wait for BLK_GET_INFO reply failed (%d).", status);
        return;
    }

    uint8_t rep_buf[256];
    uint32_t act_bytes = 0, act_handles = 0;
    status = g_zx_channel_read(client_ch, 0, rep_buf, NULL, sizeof(rep_buf), 0, &act_bytes, &act_handles);
    if (status != ZX_OK || act_bytes < sizeof(iris_msg_hdr_t) + sizeof(iris_blk_info_t)) {
        log_printf("[FAILED] iris_init: Invalid BLK_GET_INFO reply.");
        return;
    }

    iris_msg_hdr_t* rep = (iris_msg_hdr_t*)rep_buf;
    iris_blk_info_t* info = (iris_blk_info_t*)(rep_buf + sizeof(iris_msg_hdr_t));
    if (rep->status != ZX_OK || info->sector_count == 0) {
        log_printf("[FAILED] iris_init: Driver returned error or 0 capacity.");
        return;
    }

    log_printf("[  OK  ] VirtIO Block IPC query (BLK_GET_INFO): %llu sectors (%u bytes/sector).",
               (unsigned long long)info->sector_count, info->sector_size);

    /* 6. Test Write to Sector 100 */
    uint8_t write_msg[sizeof(iris_msg_hdr_t) + sizeof(iris_blk_rw_req_t) + 512];
    iris_msg_hdr_t* wr_hdr = (iris_msg_hdr_t*)write_msg;
    iris_blk_rw_req_t* wr_req = (iris_blk_rw_req_t*)(write_msg + sizeof(iris_msg_hdr_t));
    uint8_t* pattern = (uint8_t*)(wr_req + 1);

    iris_msg_hdr_init(wr_hdr, IRIS_PROTO_DRIVER, IRIS_BLK_OP_WRITE, 2, sizeof(iris_blk_rw_req_t) + 512);
    wr_req->start_sector = 100;
    wr_req->num_sectors = 1;
    wr_req->flags = 0;

    /* Fill pattern with distinctive signature and pseudo-random bytes */
    pattern[0] = 0x49; pattern[1] = 0x52; pattern[2] = 0x49; pattern[3] = 0x53; /* 'IRIS' */
    pattern[4] = 0x4F; pattern[5] = 0x53; pattern[6] = 0x5F; pattern[7] = 0x34; /* 'OS_4' */
    pattern[8] = 0xDE; pattern[9] = 0xAD; pattern[10] = 0xBE; pattern[11] = 0xEF;
    pattern[12] = 0xCA; pattern[13] = 0xFE; pattern[14] = 0xBA; pattern[15] = 0xBE;
    for (int i = 16; i < 512; i++) {
        pattern[i] = (uint8_t)((i * 37 + 101) ^ 0x5A);
    }

    status = g_zx_channel_write(client_ch, 0, write_msg, sizeof(write_msg), NULL, 0);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: Failed to send Sector 100 write request (%d).", status);
        return;
    }

    observed = 0;
    g_zx_object_wait_one(client_ch, ZX_CHANNEL_READABLE, ZX_TIME_INFINITE, &observed);
    status = g_zx_channel_read(client_ch, 0, rep_buf, NULL, sizeof(rep_buf), 0, &act_bytes, &act_handles);
    rep = (iris_msg_hdr_t*)rep_buf;
    if (status != ZX_OK || rep->status != ZX_OK) {
        log_printf("[FAILED] iris_init: Sector 100 write failed.");
        return;
    }
    log_printf("[  OK  ] VirtIO Block IPC write to Sector 100 confirmed (512 bytes).");

    /* 7. Test Read from Sector 100 */
    uint8_t read_msg[sizeof(iris_msg_hdr_t) + sizeof(iris_blk_rw_req_t)];
    iris_msg_hdr_t* rd_hdr = (iris_msg_hdr_t*)read_msg;
    iris_blk_rw_req_t* rd_req = (iris_blk_rw_req_t*)(read_msg + sizeof(iris_msg_hdr_t));

    iris_msg_hdr_init(rd_hdr, IRIS_PROTO_DRIVER, IRIS_BLK_OP_READ, 3, sizeof(iris_blk_rw_req_t));
    rd_req->start_sector = 100;
    rd_req->num_sectors = 1;
    rd_req->flags = 0;

    status = g_zx_channel_write(client_ch, 0, read_msg, sizeof(read_msg), NULL, 0);
    if (status != ZX_OK) {
        log_printf("[FAILED] iris_init: Failed to send Sector 100 read request (%d).", status);
        return;
    }

    uint8_t read_rep[sizeof(iris_msg_hdr_t) + 512 + 64];
    observed = 0;
    g_zx_object_wait_one(client_ch, ZX_CHANNEL_READABLE, ZX_TIME_INFINITE, &observed);
    status = g_zx_channel_read(client_ch, 0, read_rep, NULL, sizeof(read_rep), 0, &act_bytes, &act_handles);
    rep = (iris_msg_hdr_t*)read_rep;
    if (status != ZX_OK || rep->status != ZX_OK || act_bytes < sizeof(iris_msg_hdr_t) + 512) {
        log_printf("[FAILED] iris_init: Sector 100 read failed.");
        return;
    }

    /* 8. Compare Read Data with Written Pattern */
    const uint8_t* read_data = read_rep + sizeof(iris_msg_hdr_t);
    bool match = true;
    for (int i = 0; i < 512; i++) {
        if (read_data[i] != pattern[i]) {
            match = false;
            log_printf("[FAILED] iris_init: Sector 100 data mismatch at byte %d.", i);
            break;
        }
    }

    if (match) {
        log_printf("[  OK  ] VirtIO Block IPC read from Sector 100 verified (512/512 bytes matched).");
    } else {
        log_printf("[FAILED] iris_init: Sector 100 verification failed.");
    }

    /* Close control channel and wait for virtio_blk to unpin BTI PMTs and exit cleanly */
    g_zx_handle_close(client_ch);
    if (blk_proc != ZX_HANDLE_INVALID) {
        observed = 0;
        g_zx_object_wait_one(blk_proc, ZX_PROCESS_TERMINATED, ZX_TIME_INFINITE, &observed);
        g_zx_handle_close(blk_proc);
    }

    log_printf("[  OK  ] Phase 4 Step 4.1 PCIe & VirtIO Block verification complete.");
}

/**
 * @brief Entry point for iris_init.
 */
__attribute__((noreturn)) void _start(void* start_arg, void* vdso_base) {
    /* Initialize vDSO syscall dispatch table */
    if (vdso_base != NULL) {
        g_vdso_base_self = (uintptr_t)vdso_base;
        vdso_init((uintptr_t)vdso_base);
    }

    zx_handle_t bootstrap_channel = (zx_handle_t)(uintptr_t)start_arg;

    /* Channel message receive buffers */
    uint8_t msg_buffer[IRIS_BOOTSTRAP_MSG_BYTES] __attribute__((aligned(8)));
    zx_handle_t handles[IRIS_BOOTSTRAP_MAX_HANDLES];
    uint32_t actual_bytes = 0;
    uint32_t actual_handles = 0;

    /* Read the bootstrap message containing zx_proc_args_t and capability handles */
    zx_status_t status = g_zx_channel_read(
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

        if (args->protocol == ZX_PROCARGS_PROTOCOL && args->version == ZX_PROCARGS_VERSION) {
            size_t handle_info_size = (size_t)actual_handles * sizeof(uint32_t);
            if ((size_t)args->handle_info_off + handle_info_size <= (size_t)actual_bytes) {
                const uint32_t* handle_info = (const uint32_t*)(msg_buffer + args->handle_info_off);

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
                    } else if (info == PA_HND(PA_VMAR_ROOT, 0)) {
                        g_vmar_root_self = h;
                    }
                }
            }
        }
    }

    if (bootstrap_channel != ZX_HANDLE_INVALID) {
        g_zx_handle_close(bootstrap_channel);
    }

    /* Initialize early debuglog using the Root Resource handle */
    status = g_zx_debuglog_create(g_root_resource, 0, &g_debuglog_handle);
    if (status == ZX_OK) {
        /* Display the irisOS ASCII banner on boot (buffered per line via snprintf) */
        log_printf("  _      _      ____   _____ ");
        log_printf(" (_)    (_)    / __ \\ / ____|");
        log_printf("  _ _ __ _ ___| |  | | (___  ");
        log_printf(" | | '__| / __| |  | |\\___ \\ ");
        log_printf(" | | |  | \\__ \\ |__| |____) |");
        log_printf(" |_|_|  |_|___/\\____/|_____/ ");
        log_printf("System Node: irisOS-dev (%s)", IRIS_ARCH_NAME);

        if (g_root_resource != ZX_HANDLE_INVALID) {
            log_printf("[  OK  ] Extracted Root Resource capability.");
        }
        if (g_bootfs_vmo != ZX_HANDLE_INVALID) {
            log_printf("[  OK  ] Extracted BOOTFS VMO capability.");
        }
        if (g_root_job != ZX_HANDLE_INVALID) {
            log_printf("[  OK  ] Extracted Root Job capability.");
        }
        if (g_vdso_vmo != ZX_HANDLE_INVALID) {
            log_printf("[  OK  ] Extracted vDSO VMO capability.");
        }
        if (g_vmar_root_self != ZX_HANDLE_INVALID) {
            log_printf("[  OK  ] Extracted Root VMAR capability.");
        }

        /* Spawn and execute Interactive Test Suite (bin/its_irisOS) */
        spawn_its_irisOS();

        /* Discover PCIe VirtIO block device and test Ring 3 block driver */
        test_virtio_blk();
    }

    for (;;) {
        g_zx_nanosleep(ZX_TIME_INFINITE);
    }
}
