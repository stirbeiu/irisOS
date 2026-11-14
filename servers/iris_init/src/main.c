/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_init/src/main.c
 * Purpose: Initial userspace bootstrap supervisor (first Ring 3 process spawned by userboot).
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 system server.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#include <zircon/types.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/log.h>
#include <zircon/syscalls/object.h>
#include <zircon/syscalls/system.h>

#ifndef ZX_IOMMU_TYPE_DUMMY
#define ZX_IOMMU_TYPE_DUMMY 0x1u
typedef struct { uint8_t reserved; } zx_iommu_desc_dummy_t;
#endif

#include <iris/ipc.h>
#include <iris/pci.h>
#include <servers/drivers/pci/pci.h>

#ifndef PA_FD
#define PA_FD 0x30u
#endif
#ifndef PA_NS_DIR
#define PA_NS_DIR 0x20u
#endif

#define BOOTFS_MAGIC 0xa56d3ff9u
#define PAGE_SIZE    4096ULL
#define PAGE_MASK    (~(PAGE_SIZE - 1ULL))
#define IRIS_BOOTSTRAP_MAX_HANDLES 64
#define IRIS_BOOTSTRAP_MSG_BYTES   4096

#ifndef ZX_SEC
#define ZX_SEC(n) ((zx_duration_t)(n) * 1000000000ULL)
#endif

#if defined(__x86_64__)
#define IRIS_ARCH_NAME "x86_64"
#elif defined(__aarch64__)
#define IRIS_ARCH_NAME "aarch64"
#else
#define IRIS_ARCH_NAME "unknown"
#endif

/*
 * System Node / Hostname Configuration:
 * Defaults to "irisOS-dev" for development builds.
 * For production release builds, this can be overridden via -DIRIS_SYSTEM_NODE="..."
 * or configured down the road via system configuration / kernel cmdline.
 */
#ifndef IRIS_SYSTEM_NODE
#define IRIS_SYSTEM_NODE "irisOS-dev"
#endif

/* 4-character ANSI status tags for perfect 6-character bracket alignment */
#define STATUS_OKAY "\033[32m OKAY \033[0m"
#define STATUS_WARN "\033[33m WARN \033[0m"
#define STATUS_FAIL "\033[31m FAIL \033[0m"

/* Root capabilities extracted from userboot */
static zx_handle_t g_root_resource   = ZX_HANDLE_INVALID;
static zx_handle_t g_bootfs_vmo      = ZX_HANDLE_INVALID;
static zx_handle_t g_root_job        = ZX_HANDLE_INVALID;
static zx_handle_t g_vdso_vmo        = ZX_HANDLE_INVALID;
static zx_handle_t g_vmar_root_self  = ZX_HANDLE_INVALID;
static zx_handle_t g_debuglog_handle = ZX_HANDLE_INVALID;
static uintptr_t   g_vdso_base_self  = 0;
static const uint8_t* g_bootfs_data  = NULL;

/* ==============================================================================
 * Freestanding 64-bit ELF & vDSO Dynamic Resolver
 * ============================================================================== */

typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version; uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags; uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; } Elf64_Phdr;
typedef struct { int64_t d_tag; union { uint64_t d_val, d_ptr; } d_un; } Elf64_Dyn;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } Elf64_Sym;

static void* vdso_lookup(uintptr_t base, const char* name) {
    if (!base || !name) return NULL;
    const uint8_t* p = (const uint8_t*)base;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)p;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return NULL;
    const Elf64_Phdr* ph = (const Elf64_Phdr*)(p + eh->e_phoff);
    const Elf64_Dyn* dyn = NULL;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == 2 /* PT_DYNAMIC */) { dyn = (const Elf64_Dyn*)(p + ph[i].p_vaddr); break; }
    }
    if (!dyn) return NULL;
    const Elf64_Sym* sym = NULL; const char* str = NULL; size_t strsz = 0;
    for (const Elf64_Dyn* d = dyn; d->d_tag != 0; d++) {
        if (d->d_tag == 6) sym = (const Elf64_Sym*)(p + d->d_un.d_ptr);
        else if (d->d_tag == 5) str = (const char*)(p + d->d_un.d_ptr);
        else if (d->d_tag == 10) strsz = d->d_un.d_val;
    }
    if (!sym || !str) return NULL;
    size_t count = ((uintptr_t)str - (uintptr_t)sym) / sizeof(Elf64_Sym);
    for (size_t i = 0; i < count; i++) {
        if (sym[i].st_name < strsz) {
            const char *s = str + sym[i].st_name, *n = name;
            while (*s && *n && (*s == *n)) { s++; n++; }
            if (*s == '\0' && *n == '\0') return (void*)(p + sym[i].st_value);
        }
    }
    return NULL;
}

static void* vdso_find(uintptr_t base, const char* name, const char* alt) {
    void* ptr = vdso_lookup(base, alt);
    return ptr ? ptr : vdso_lookup(base, name);
}

/* System call function stubs */
static zx_status_t (*g_zx_channel_read)(zx_handle_t, uint32_t, void*, zx_handle_t*, uint32_t, uint32_t, uint32_t*, uint32_t*) = NULL;
static zx_status_t (*g_zx_channel_create)(uint32_t, zx_handle_t*, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_channel_write)(zx_handle_t, uint32_t, const void*, uint32_t, const zx_handle_t*, uint32_t) = NULL;
static zx_status_t (*g_zx_handle_close)(zx_handle_t) = NULL;
static zx_status_t (*g_zx_handle_duplicate)(zx_handle_t, zx_rights_t, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_debuglog_create)(zx_handle_t, uint32_t, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_debuglog_write)(zx_handle_t, uint32_t, const void*, size_t) = NULL;
static zx_status_t (*g_zx_debug_write)(const char*, size_t) = NULL;
static zx_status_t (*g_zx_vmo_create)(uint64_t, uint32_t, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_vmo_get_size)(zx_handle_t, uint64_t*) = NULL;
static zx_status_t (*g_zx_vmo_write)(zx_handle_t, const void*, uint64_t, size_t) = NULL;
static zx_status_t (*g_zx_vmo_replace_as_executable)(zx_handle_t, zx_handle_t, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_vmar_map)(zx_handle_t, zx_vm_option_t, uint64_t, zx_handle_t, uint64_t, uint64_t, zx_vaddr_t*) = NULL;
static zx_status_t (*g_zx_vmar_unmap)(zx_handle_t, zx_vaddr_t, uint64_t) = NULL;
static zx_status_t (*g_zx_vmar_allocate)(zx_handle_t, zx_vm_option_t, uint64_t, uint64_t, zx_handle_t*, zx_vaddr_t*) = NULL;
static zx_status_t (*g_zx_process_create)(zx_handle_t, const char*, size_t, uint32_t, zx_handle_t*, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_thread_create)(zx_handle_t, const char*, size_t, uint32_t, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_process_start)(zx_handle_t, zx_handle_t, zx_vaddr_t, zx_vaddr_t, zx_handle_t, uintptr_t) = NULL;
static zx_status_t (*g_zx_object_wait_one)(zx_handle_t, zx_signals_t, zx_time_t, zx_signals_t*) = NULL;
static zx_status_t (*g_zx_object_wait_many)(zx_wait_item_t*, size_t, zx_time_t) = NULL;
static zx_status_t (*g_zx_task_kill)(zx_handle_t) = NULL;
static zx_status_t (*g_zx_system_powerctl)(zx_handle_t, uint32_t, const zx_system_powerctl_arg_t*) = NULL;
static zx_status_t (*g_zx_nanosleep)(zx_time_t) = NULL;
static zx_time_t   (*g_zx_deadline_after)(zx_duration_t) = NULL;
static zx_status_t (*g_zx_vmo_create_physical)(zx_handle_t, uintptr_t, size_t, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_iommu_create)(zx_handle_t, uint32_t, const void*, size_t, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_bti_create)(zx_handle_t, uint32_t, uint64_t, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_interrupt_create)(zx_handle_t, uint32_t, uint32_t, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_object_get_info)(zx_handle_t, uint32_t, void*, size_t, size_t*, size_t*) = NULL;
static zx_status_t (*g_zx_ioports_request)(zx_handle_t, uint16_t, uint16_t) = NULL;
static zx_status_t (*g_zx_clock_adjust)(zx_handle_t, uint32_t, int64_t) = NULL;
static zx_time_t   (*g_zx_clock_get_monotonic)(void) = NULL;

static void vdso_init(uintptr_t base) {
    #define RESOLVE(fn) g_##fn = (void*)vdso_find(base, #fn, "_" #fn)
    RESOLVE(zx_channel_read); RESOLVE(zx_channel_create); RESOLVE(zx_channel_write);
    RESOLVE(zx_handle_close); RESOLVE(zx_handle_duplicate); RESOLVE(zx_debuglog_create);
    RESOLVE(zx_debuglog_write); RESOLVE(zx_debug_write); RESOLVE(zx_vmo_create);
    RESOLVE(zx_vmo_get_size); RESOLVE(zx_vmo_write); RESOLVE(zx_vmo_replace_as_executable);
    RESOLVE(zx_vmar_map); RESOLVE(zx_vmar_unmap); RESOLVE(zx_vmar_allocate);
    RESOLVE(zx_process_create); RESOLVE(zx_thread_create); RESOLVE(zx_process_start);
    RESOLVE(zx_object_wait_one); RESOLVE(zx_object_wait_many); RESOLVE(zx_task_kill);
    RESOLVE(zx_system_powerctl); RESOLVE(zx_nanosleep); RESOLVE(zx_deadline_after);
    RESOLVE(zx_vmo_create_physical); RESOLVE(zx_iommu_create); RESOLVE(zx_bti_create);
    RESOLVE(zx_interrupt_create); RESOLVE(zx_object_get_info);
    RESOLVE(zx_ioports_request); RESOLVE(zx_clock_adjust);
    RESOLVE(zx_clock_get_monotonic);
    #undef RESOLVE
}

static zx_time_t iris_deadline_after(zx_duration_t n) {
    if (g_zx_deadline_after) return g_zx_deadline_after(n);
    return ZX_TIME_INFINITE;
}

/* ==============================================================================
 * Freestanding String & Buffered Formatting Helpers
 * ============================================================================== */

static void* iris_memcpy(void* d, const void* s, size_t n) {
    uint8_t* dp = (uint8_t*)d; const uint8_t* sp = (const uint8_t*)s;
    for (size_t i = 0; i < n; i++) dp[i] = sp[i];
    return d;
}

static void* iris_memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s; for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

static int iris_memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t *p1 = (const uint8_t*)s1, *p2 = (const uint8_t*)s2;
    for (size_t i = 0; i < n; i++) if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    return 0;
}

static size_t iris_strlen(const char* s) {
    size_t len = 0; while (s && s[len]) len++;
    return len;
}

static int iris_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static void buf_putc(char* buf, size_t size, size_t* idx, char c) {
    if (size > 0 && *idx + 1 < size) buf[*idx] = c;
    (*idx)++;
}

static int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    size_t idx = 0;
    if (!fmt) { if (size > 0) buf[0] = '\0'; return 0; }
    while (*fmt) {
        if (*fmt != '%') { buf_putc(buf, size, &idx, *fmt++); continue; }
        fmt++;
        char pad = (*fmt == '0') ? (fmt++, '0') : ' ';
        int width = 0; while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int is_long = 0;
        if (*fmt == 'z') { is_long = 2; fmt++; }
        else { while (*fmt == 'l') { is_long++; fmt++; } }
        char spec = *fmt ? *fmt++ : '\0';
        if (spec == 's') {
            const char* s = va_arg(ap, const char*); if (!s) s = "(null)";
            while (*s) buf_putc(buf, size, &idx, *s++);
        } else if (spec == 'c') {
            buf_putc(buf, size, &idx, (char)va_arg(ap, int));
        } else if (spec == 'd' || spec == 'i') {
            int64_t v = (is_long >= 1) ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            uint64_t uv = (v < 0) ? (buf_putc(buf, size, &idx, '-'), (uint64_t)(-(v + 1)) + 1u) : (uint64_t)v;
            char tmp[24]; int n = 0;
            do { tmp[n++] = (char)('0' + (uv % 10u)); uv /= 10u; } while (uv > 0);
            while (n < width--) buf_putc(buf, size, &idx, pad);
            while (n > 0) buf_putc(buf, size, &idx, tmp[--n]);
        } else if (spec == 'u' || spec == 'x' || spec == 'X') {
            uint64_t uv = (is_long >= 1) ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
            const char* d = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            uint32_t b = (spec == 'u') ? 10u : 16u;
            char tmp[24]; int n = 0;
            do { tmp[n++] = d[uv % b]; uv /= b; } while (uv > 0);
            while (n < width--) buf_putc(buf, size, &idx, pad);
            while (n > 0) buf_putc(buf, size, &idx, tmp[--n]);
        } else if (spec == '%') {
            buf_putc(buf, size, &idx, '%');
        }
    }
    if (size > 0) buf[idx < size ? idx : size - 1] = '\0';
    return (int)idx;
}

static int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap); return ret;
}

static void log_line(const char* str) {
    if (!str) return;
    size_t len = iris_strlen(str);
    if (g_debuglog_handle != ZX_HANDLE_INVALID && g_zx_debuglog_write) {
        g_zx_debuglog_write(g_debuglog_handle, 0, str, len);
    } else if (g_zx_debug_write) {
        const char* p = str;
        while (len > 0) {
            size_t n = (len > 256) ? 256 : len;
            g_zx_debug_write(p, n);
            p += n;
            len -= n;
        }
        g_zx_debug_write("\r\n", 2);
    }
}

static void log_status(const char* status, const char* unit, const char* fmt, ...) {
    char desc[192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(desc, sizeof(desc), fmt, ap);
    va_end(ap);

    char line[256];
    int len = snprintf(line, sizeof(line), "[%s] %s - %s", status, unit, desc);
    if (len > 0) {
        size_t total = (size_t)len < sizeof(line) ? (size_t)len : sizeof(line) - 1;
        if (g_debuglog_handle != ZX_HANDLE_INVALID && g_zx_debuglog_write) {
            g_zx_debuglog_write(g_debuglog_handle, 0, line, total);
        } else if (g_zx_debug_write) {
            const char* p = line;
            while (total > 0) {
                size_t n = (total > 256) ? 256 : total;
                g_zx_debug_write(p, n);
                p += n;
                total -= n;
            }
            g_zx_debug_write("\r\n", 2);
        }
    }
}

/* ==============================================================================
 * BOOTFS Loader & Process Spawner
 * ============================================================================== */

typedef struct { uint32_t magic, dirsize, reserved0, reserved1; } bootfs_header_t;
typedef struct { uint32_t name_len, data_len, data_off; char name[]; } bootfs_entry_t;
typedef struct { zx_handle_t handle; uint32_t info; } spawn_handle_t;

static bool find_bootfs_entry(const char* path, uint32_t* out_off, uint32_t* out_len) __attribute__((noinline));
static bool find_bootfs_entry(const char* path, uint32_t* out_off, uint32_t* out_len) {
    if (!g_bootfs_data || !path) return false;
    while (*path == '/') path++;
    const bootfs_header_t* hdr = (const bootfs_header_t*)g_bootfs_data;
    if (hdr->magic != BOOTFS_MAGIC) return false;

    const char* targets[2] = { path, NULL };
    if (iris_strcmp(path, "System/Bin/toybox") == 0 || iris_strcmp(path, "bin/sh") == 0) {
        targets[1] = "bin/toybox";
    }

    for (int t = 0; t < 2; t++) {
        if (!targets[t]) continue;
        const char* target = targets[t];
        size_t target_len = iris_strlen(target) + 1;
        const uint8_t *p = (const uint8_t*)(hdr + 1), *end = p + hdr->dirsize;
        while (p < end) {
            const bootfs_entry_t* e = (const bootfs_entry_t*)p;
            size_t recsize = sizeof(bootfs_entry_t) + ((e->name_len + 3u) & ~3u);
            if (e->name_len == target_len && iris_memcmp(e->name, target, target_len) == 0) {
                *out_off = e->data_off; *out_len = e->data_len; return true;
            }
            p += recsize;
        }
    }
    return false;
}

static void add_handle(zx_handle_t* hnds, uint32_t* infos, uint32_t* count, zx_handle_t h, uint32_t info, bool dup) {
    if (h == ZX_HANDLE_INVALID) return;
    if (dup) g_zx_handle_duplicate(h, ZX_RIGHT_SAME_RIGHTS, &hnds[*count]);
    else hnds[*count] = h;
    infos[(*count)++] = info;
}

static zx_status_t spawn_process_from_elf(const char* name, const uint8_t* elf, size_t elf_len,
                                          int argc, const char* argv[],
                                          const spawn_handle_t* extra, size_t num_extra,
                                          zx_handle_t* out_proc) __attribute__((noinline));
static zx_status_t spawn_process_from_elf(const char* name, const uint8_t* elf, size_t elf_len,
                                          int argc, const char* argv[],
                                          const spawn_handle_t* extra, size_t num_extra,
                                          zx_handle_t* out_proc) {
    if (!elf || elf_len < sizeof(Elf64_Ehdr)) return ZX_ERR_INVALID_ARGS;

    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)elf;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        return ZX_ERR_NOT_FOUND;
    }
    const Elf64_Phdr* ph = (const Elf64_Phdr*)(elf + eh->e_phoff);

    zx_handle_t proc = ZX_HANDLE_INVALID, vmar = ZX_HANDLE_INVALID, thrd = ZX_HANDLE_INVALID;
    zx_status_t st = g_zx_process_create(g_root_job, name, (uint32_t)iris_strlen(name), 0, &proc, &vmar);
    if (st != ZX_OK) return st;
    st = g_zx_thread_create(proc, name, (uint32_t)iris_strlen(name), 0, &thrd);
    if (st != ZX_OK) {
        g_zx_handle_close(proc);
        g_zx_handle_close(vmar);
        return st;
    }

    uint64_t min_va = UINT64_MAX, max_va = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == 1 /* PT_LOAD */) {
            uint64_t s = ph[i].p_vaddr & PAGE_MASK;
            uint64_t e = (ph[i].p_vaddr + ph[i].p_memsz + PAGE_SIZE - 1u) & PAGE_MASK;
            if (s < min_va) min_va = s;
            if (e > max_va) max_va = e;
        }
    }

    zx_handle_t prog_vmar = vmar;
    uint64_t load_bias = 0;
    if (eh->e_type == 3 /* ET_DYN (PIE) */) {
        zx_vaddr_t base = 0;
        g_zx_vmar_allocate(vmar, ZX_VM_CAN_MAP_READ | ZX_VM_CAN_MAP_WRITE | ZX_VM_CAN_MAP_EXECUTE | ZX_VM_CAN_MAP_SPECIFIC,
                           0, max_va - min_va, &prog_vmar, &base);
        load_bias = base - min_va;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 1 || ph[i].p_memsz == 0) continue;
        uint64_t seg_va = ph[i].p_vaddr + load_bias;
        uint64_t map_va = seg_va & PAGE_MASK;
        uint64_t poff = seg_va - map_va;
        uint64_t seg_sz = (ph[i].p_memsz + poff + PAGE_SIZE - 1u) & PAGE_MASK;

        zx_handle_t vmo = ZX_HANDLE_INVALID;
        g_zx_vmo_create(seg_sz, 0, &vmo);
        if (ph[i].p_filesz > 0) g_zx_vmo_write(vmo, elf + ph[i].p_offset, poff, ph[i].p_filesz);

        zx_vm_option_t p = ZX_VM_SPECIFIC | ((ph[i].p_flags & 4) ? ZX_VM_PERM_READ : 0) |
                           ((ph[i].p_flags & 2) ? ZX_VM_PERM_WRITE : 0);
        if (ph[i].p_flags & 1) {
            p |= ZX_VM_PERM_EXECUTE;
            if (g_zx_vmo_replace_as_executable) g_zx_vmo_replace_as_executable(vmo, ZX_HANDLE_INVALID, &vmo);
        }
        zx_vaddr_t act = 0;
        g_zx_vmar_map(prog_vmar, p, map_va - (eh->e_type == 3 ? (load_bias + min_va) : 0), vmo, 0, seg_sz, &act);
        g_zx_handle_close(vmo);
    }
    if (eh->e_type == 3) g_zx_handle_close(prog_vmar);

    /* Map vDSO VMO into child */
    const Elf64_Ehdr* v_eh = (const Elf64_Ehdr*)g_vdso_base_self;
    const Elf64_Phdr* v_ph = (const Elf64_Phdr*)((const uint8_t*)g_vdso_base_self + v_eh->e_phoff);
    uint64_t v_min = UINT64_MAX, v_max = 0;
    for (uint16_t j = 0; j < v_eh->e_phnum; j++) {
        if (v_ph[j].p_type == 1) {
            uint64_t s = v_ph[j].p_vaddr & PAGE_MASK;
            uint64_t e = (v_ph[j].p_vaddr + v_ph[j].p_memsz + PAGE_SIZE - 1ULL) & PAGE_MASK;
            if (s < v_min) v_min = s; if (e > v_max) v_max = e;
        }
    }
    zx_handle_t vdso_sub = ZX_HANDLE_INVALID; zx_vaddr_t vdso_base = 0;
    g_zx_vmar_allocate(vmar, ZX_VM_CAN_MAP_READ | ZX_VM_CAN_MAP_EXECUTE | ZX_VM_CAN_MAP_SPECIFIC, 0, v_max - v_min, &vdso_sub, &vdso_base);
    for (uint16_t j = 0; j < v_eh->e_phnum; j++) {
        if (v_ph[j].p_type != 1) continue;
        zx_vm_option_t p = ZX_VM_SPECIFIC | ZX_VM_PERM_READ | ((v_ph[j].p_flags & 1) ? ZX_VM_PERM_EXECUTE : 0);
        zx_vaddr_t a = 0;
        g_zx_vmar_map(vdso_sub, p, (v_ph[j].p_vaddr & PAGE_MASK) - v_min, g_vdso_vmo, v_ph[j].p_offset & PAGE_MASK, (v_ph[j].p_memsz + PAGE_SIZE - 1ULL) & PAGE_MASK, &a);
    }
    g_zx_handle_close(vdso_sub);

    /* Stack allocation (256 KB) */
    size_t stk_sz = 256u * 1024u;
    zx_handle_t stk_vmo = ZX_HANDLE_INVALID;
    g_zx_vmo_create(stk_sz, 0, &stk_vmo);
    zx_vaddr_t stk_base = 0;
    g_zx_vmar_map(vmar, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, stk_vmo, 0, stk_sz, &stk_base);
    zx_vaddr_t sp = (stk_base + stk_sz) & ~(uintptr_t)0xF;
#if defined(__x86_64__)
    sp -= 8;
#endif

    /* Assemble Bootstrap Message */
    zx_handle_t hnds[IRIS_BOOTSTRAP_MAX_HANDLES]; uint32_t infos[IRIS_BOOTSTRAP_MAX_HANDLES]; uint32_t nh = 0;
    add_handle(hnds, infos, &nh, proc, PA_HND(PA_PROC_SELF, 0), true);
    add_handle(hnds, infos, &nh, thrd, PA_HND(PA_THREAD_SELF, 0), true);
    add_handle(hnds, infos, &nh, vmar, PA_HND(PA_VMAR_ROOT, 0), true);
    add_handle(hnds, infos, &nh, stk_vmo, PA_HND(PA_VMO_STACK, 0), true);
    add_handle(hnds, infos, &nh, g_vdso_vmo, PA_HND(PA_VMO_VDSO, 0), true);
    add_handle(hnds, infos, &nh, g_root_job, PA_HND(PA_JOB_DEFAULT, 0), true);
    add_handle(hnds, infos, &nh, g_debuglog_handle, PA_HND(PA_FDIO_LOGGER, 0), true);

    bool is_daemon = (iris_strcmp(name, "virtio_blk") == 0 ||
                      iris_strcmp(name, "virtio_gpu") == 0 ||
                      iris_strcmp(name, "iris_compositor") == 0 ||
                      iris_strcmp(name, "iris_crypt") == 0 ||
                      iris_strcmp(name, "iris_fshost") == 0 ||
                      iris_strcmp(name, "iris_pty") == 0);
    bool has_pty = false;
    if (!is_daemon) {
        for (size_t i = 0; i < num_extra; ++i) {
            if (extra[i].info == PA_HND(PA_USER0, 0) && extra[i].handle != ZX_HANDLE_INVALID) {
                has_pty = true;
                break;
            }
        }
    }
    if (!has_pty) {
        bool has_fd0 = false;
        for (size_t i = 0; i < num_extra; ++i) if (extra[i].info == PA_HND(PA_FD, 0)) has_fd0 = true;
        if (!has_fd0) add_handle(hnds, infos, &nh, g_debuglog_handle, PA_HND(PA_FD, 0), true);
        add_handle(hnds, infos, &nh, g_debuglog_handle, PA_HND(PA_FD, 1), true);
        add_handle(hnds, infos, &nh, g_debuglog_handle, PA_HND(PA_FD, 2), true);
    }
    add_handle(hnds, infos, &nh, g_bootfs_vmo, PA_HND(PA_VMO_BOOTFS, 0), true);

    for (size_t i = 0; i < num_extra; ++i) {
        if (extra[i].handle != ZX_HANDLE_INVALID) add_handle(hnds, infos, &nh, extra[i].handle, extra[i].info, false);
    }

    uint8_t msg[2048]; iris_memset(msg, 0, sizeof(msg));
    zx_proc_args_t* pa = (zx_proc_args_t*)msg;
    pa->protocol = ZX_PROCARGS_PROTOCOL; pa->version = ZX_PROCARGS_VERSION;
    pa->handle_info_off = sizeof(zx_proc_args_t);
    iris_memcpy(msg + sizeof(zx_proc_args_t), infos, nh * sizeof(uint32_t));

    const char* fallback_argv[2] = { name ? name : "iris_daemon", NULL };
    if (argc <= 0 || argv == NULL) {
        argc = 1;
        argv = fallback_argv;
    }

    uint32_t cur_off = (uint32_t)(sizeof(zx_proc_args_t) + nh * sizeof(uint32_t));
    pa->args_off = cur_off;
    pa->args_num = (uint32_t)argc;
    for (int i = 0; i < argc; i++) {
        if (argv[i] == NULL) break;
        size_t slen = iris_strlen(argv[i]) + 1;
        if (cur_off + slen < sizeof(msg)) {
            iris_memcpy(msg + cur_off, argv[i], slen);
            cur_off += (uint32_t)slen;
        }
    }

    zx_handle_t b_tx = ZX_HANDLE_INVALID, b_rx = ZX_HANDLE_INVALID;
    g_zx_channel_create(0, &b_tx, &b_rx);
    g_zx_channel_write(b_tx, 0, msg, cur_off, hnds, nh);
    g_zx_handle_close(b_tx);

    st = g_zx_process_start(proc, thrd, eh->e_entry + load_bias, sp, b_rx, vdso_base);
    g_zx_handle_close(stk_vmo); g_zx_handle_close(vmar); g_zx_handle_close(thrd);
    if (out_proc != NULL) {
        *out_proc = proc;
    } else {
        g_zx_handle_close(proc);
    }
    return st;
}

static zx_status_t spawn_bootfs_binary(const char* name, const char* path,
                                       int argc, const char* argv[],
                                       const spawn_handle_t* extra, size_t num_extra,
                                       zx_handle_t* out_proc) __attribute__((noinline));
static zx_status_t spawn_bootfs_binary(const char* name, const char* path,
                                       int argc, const char* argv[],
                                       const spawn_handle_t* extra, size_t num_extra,
                                       zx_handle_t* out_proc) {
    uint32_t file_off = 0, file_len = 0;
    if (!find_bootfs_entry(path, &file_off, &file_len)) return ZX_ERR_NOT_FOUND;
    return spawn_process_from_elf(name, g_bootfs_data + file_off, (size_t)file_len, argc, argv, extra, num_extra, out_proc);
}

static zx_status_t vfs_read_file_to_vmo(zx_handle_t fs_ch, const char* path,
                                        zx_handle_t* out_vmo, uint64_t* out_size,
                                        const uint8_t** out_data) __attribute__((noinline));
static zx_status_t vfs_read_file_to_vmo(zx_handle_t fs_ch, const char* path,
                                        zx_handle_t* out_vmo, uint64_t* out_size,
                                        const uint8_t** out_data) {
    if (fs_ch == ZX_HANDLE_INVALID || !path || !out_vmo || !out_size || !out_data) {
        return ZX_ERR_INVALID_ARGS;
    }

    static uint32_t s_fs_req_txid = 1000;
    iris_fs_open_req_t open_req;
    iris_memset(&open_req, 0, sizeof(open_req));
    open_req.flags = 0;
    size_t plen = iris_strlen(path);
    if (plen >= sizeof(open_req.path)) plen = sizeof(open_req.path) - 1;
    iris_memcpy(open_req.path, path, plen);
    open_req.path[plen] = '\0';

    iris_msg_hdr_t hdr;
    iris_msg_hdr_init(&hdr, IRIS_PROTO_FS, IRIS_FS_OP_OPEN, ++s_fs_req_txid, sizeof(open_req));

    uint8_t tx_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_fs_open_req_t)];
    iris_memcpy(tx_buf, &hdr, sizeof(hdr));
    iris_memcpy(tx_buf + sizeof(hdr), &open_req, sizeof(open_req));

    zx_status_t st = g_zx_channel_write(fs_ch, 0, tx_buf, sizeof(tx_buf), NULL, 0);
    if (st != ZX_OK) return st;

    zx_signals_t obs = 0;
    st = g_zx_object_wait_one(fs_ch, ZX_CHANNEL_READABLE, iris_deadline_after(ZX_SEC(5)), &obs);
    if (st != ZX_OK) return st;

    uint8_t rx_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_fs_open_resp_t) + 64];
    uint32_t act_b = 0, act_h = 0;
    st = g_zx_channel_read(fs_ch, 0, rx_buf, NULL, sizeof(rx_buf), 0, &act_b, &act_h);
    if (st != ZX_OK || act_b < sizeof(iris_msg_hdr_t) + sizeof(iris_fs_open_resp_t)) {
        return (st != ZX_OK) ? st : ZX_ERR_IO;
    }

    const iris_msg_hdr_t* rep_hdr = (const iris_msg_hdr_t*)rx_buf;
    if (rep_hdr->status != ZX_OK) return rep_hdr->status;

    const iris_fs_open_resp_t* open_resp = (const iris_fs_open_resp_t*)(rx_buf + sizeof(iris_msg_hdr_t));
    uint32_t vfs_handle = open_resp->vfs_handle;
    uint64_t file_size = open_resp->size;
    if (file_size == 0) return ZX_ERR_NOT_FOUND;

    uint64_t vmo_sz = (file_size + PAGE_SIZE - 1u) & PAGE_MASK;
    zx_handle_t vmo = ZX_HANDLE_INVALID;
    st = g_zx_vmo_create(vmo_sz, 0, &vmo);
    if (st != ZX_OK) return st;

    zx_vaddr_t va = 0;
    st = g_zx_vmar_map(g_vmar_root_self, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo, 0, vmo_sz, &va);
    if (st != ZX_OK) {
        g_zx_handle_close(vmo);
        return st;
    }

    uint64_t bytes_read = 0;
    uint8_t r_buf[sizeof(iris_msg_hdr_t) + 4096];
    while (bytes_read < file_size) {
        uint32_t chunk = (uint32_t)(file_size - bytes_read);
        if (chunk > 4096u) chunk = 4096u;

        iris_fs_read_req_t r_req;
        r_req.vfs_handle = vfs_handle;
        r_req.count = chunk;

        iris_msg_hdr_init(&hdr, IRIS_PROTO_FS, IRIS_FS_OP_READ, ++s_fs_req_txid, sizeof(r_req));
        uint8_t r_tx[sizeof(iris_msg_hdr_t) + sizeof(iris_fs_read_req_t)];
        iris_memcpy(r_tx, &hdr, sizeof(hdr));
        iris_memcpy(r_tx + sizeof(hdr), &r_req, sizeof(r_req));

        st = g_zx_channel_write(fs_ch, 0, r_tx, sizeof(r_tx), NULL, 0);
        if (st != ZX_OK) break;
        st = g_zx_object_wait_one(fs_ch, ZX_CHANNEL_READABLE, iris_deadline_after(ZX_SEC(5)), &obs);
        if (st != ZX_OK) break;

        st = g_zx_channel_read(fs_ch, 0, r_buf, NULL, sizeof(r_buf), 0, &act_b, &act_h);
        if (st != ZX_OK || act_b <= sizeof(iris_msg_hdr_t)) {
            st = ZX_ERR_IO;
            break;
        }

        const iris_msg_hdr_t* rd_hdr = (const iris_msg_hdr_t*)r_buf;
        if (rd_hdr->status != ZX_OK || rd_hdr->payload_len == 0) break;

        iris_memcpy((uint8_t*)(va + bytes_read), r_buf + sizeof(iris_msg_hdr_t), rd_hdr->payload_len);
        bytes_read += rd_hdr->payload_len;
    }

    /* Close file on server */
    iris_fs_close_req_t c_req;
    c_req.vfs_handle = vfs_handle;
    c_req.reserved = 0;
    iris_msg_hdr_init(&hdr, IRIS_PROTO_FS, IRIS_FS_OP_CLOSE, ++s_fs_req_txid, sizeof(c_req));
    uint8_t c_tx[sizeof(iris_msg_hdr_t) + sizeof(iris_fs_close_req_t)];
    iris_memcpy(c_tx, &hdr, sizeof(hdr));
    iris_memcpy(c_tx + sizeof(hdr), &c_req, sizeof(c_req));
    g_zx_channel_write(fs_ch, 0, c_tx, sizeof(c_tx), NULL, 0);
    g_zx_object_wait_one(fs_ch, ZX_CHANNEL_READABLE, iris_deadline_after(ZX_SEC(1)), &obs);
    g_zx_channel_read(fs_ch, 0, r_buf, NULL, sizeof(r_buf), 0, &act_b, &act_h);

    if (bytes_read < file_size) {
        if (g_zx_vmar_unmap) g_zx_vmar_unmap(g_vmar_root_self, va, vmo_sz);
        g_zx_handle_close(vmo);
        return ZX_ERR_IO;
    }

    *out_vmo = vmo;
    *out_size = file_size;
    *out_data = (const uint8_t*)va;
    return ZX_OK;
}

static zx_status_t vfs_write_file_from_buffer(zx_handle_t fs_ch, const char* path,
                                              const void* data, size_t len) {
    if (fs_ch == ZX_HANDLE_INVALID || !path || !data) return ZX_ERR_INVALID_ARGS;

    static uint32_t s_fs_write_txid = 2000;
    iris_fs_open_req_t open_req;
    iris_memset(&open_req, 0, sizeof(open_req));
    open_req.flags = 0x0001u | 0x0008u | 0x0200u; /* VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC */
    open_req.mode  = 0644;
    size_t plen = iris_strlen(path);
    if (plen >= sizeof(open_req.path)) plen = sizeof(open_req.path) - 1;
    iris_memcpy(open_req.path, path, plen);
    open_req.path[plen] = '\0';

    iris_msg_hdr_t hdr;
    iris_msg_hdr_init(&hdr, IRIS_PROTO_FS, IRIS_FS_OP_OPEN, ++s_fs_write_txid, sizeof(open_req));

    uint8_t tx_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_fs_open_req_t)];
    iris_memcpy(tx_buf, &hdr, sizeof(hdr));
    iris_memcpy(tx_buf + sizeof(hdr), &open_req, sizeof(open_req));

    zx_status_t st = g_zx_channel_write(fs_ch, 0, tx_buf, sizeof(tx_buf), NULL, 0);
    if (st != ZX_OK) return st;

    zx_signals_t obs = 0;
    st = g_zx_object_wait_one(fs_ch, ZX_CHANNEL_READABLE, iris_deadline_after(ZX_SEC(5)), &obs);
    if (st != ZX_OK) return st;

    uint8_t rx_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_fs_open_resp_t) + 64];
    uint32_t act_b = 0, act_h = 0;
    st = g_zx_channel_read(fs_ch, 0, rx_buf, NULL, sizeof(rx_buf), 0, &act_b, &act_h);
    if (st != ZX_OK || act_b < sizeof(iris_msg_hdr_t) + sizeof(iris_fs_open_resp_t)) {
        return (st != ZX_OK) ? st : ZX_ERR_IO;
    }

    const iris_msg_hdr_t* rep_hdr = (const iris_msg_hdr_t*)rx_buf;
    if (rep_hdr->status != ZX_OK) return rep_hdr->status;

    const iris_fs_open_resp_t* open_resp = (const iris_fs_open_resp_t*)(rx_buf + sizeof(iris_msg_hdr_t));
    uint32_t vfs_handle = open_resp->vfs_handle;

    /* Write data in chunks */
    size_t bytes_written = 0;
    uint8_t w_tx[sizeof(iris_msg_hdr_t) + sizeof(iris_fs_write_req_t) + 4096];
    while (bytes_written < len) {
        uint32_t chunk = (uint32_t)(len - bytes_written);
        if (chunk > 4096u) chunk = 4096u;

        iris_fs_write_req_t w_req;
        w_req.vfs_handle = vfs_handle;
        w_req.count      = chunk;

        iris_msg_hdr_init(&hdr, IRIS_PROTO_FS, IRIS_FS_OP_WRITE, ++s_fs_write_txid,
                          sizeof(w_req) + chunk);
        iris_memcpy(w_tx, &hdr, sizeof(hdr));
        iris_memcpy(w_tx + sizeof(hdr), &w_req, sizeof(w_req));
        iris_memcpy(w_tx + sizeof(hdr) + sizeof(w_req), (const uint8_t*)data + bytes_written, chunk);

        st = g_zx_channel_write(fs_ch, 0, w_tx, (uint32_t)(sizeof(hdr) + sizeof(w_req) + chunk), NULL, 0);
        if (st != ZX_OK) break;

        st = g_zx_object_wait_one(fs_ch, ZX_CHANNEL_READABLE, iris_deadline_after(ZX_SEC(5)), &obs);
        if (st != ZX_OK) break;

        uint8_t w_rx[sizeof(iris_msg_hdr_t) + sizeof(iris_fs_write_resp_t) + 64];
        st = g_zx_channel_read(fs_ch, 0, w_rx, NULL, sizeof(w_rx), 0, &act_b, &act_h);
        if (st != ZX_OK || act_b < sizeof(iris_msg_hdr_t)) break;

        const iris_msg_hdr_t* wr_hdr = (const iris_msg_hdr_t*)w_rx;
        if (wr_hdr->status != ZX_OK) break;

        bytes_written += chunk;
    }

    /* Close file on server */
    iris_fs_close_req_t c_req;
    c_req.vfs_handle = vfs_handle;
    c_req.reserved   = 0;
    iris_msg_hdr_init(&hdr, IRIS_PROTO_FS, IRIS_FS_OP_CLOSE, ++s_fs_write_txid, sizeof(c_req));
    uint8_t c_tx[sizeof(iris_msg_hdr_t) + sizeof(iris_fs_close_req_t)];
    iris_memcpy(c_tx, &hdr, sizeof(hdr));
    iris_memcpy(c_tx + sizeof(hdr), &c_req, sizeof(c_req));
    g_zx_channel_write(fs_ch, 0, c_tx, sizeof(c_tx), NULL, 0);
    g_zx_object_wait_one(fs_ch, ZX_CHANNEL_READABLE, iris_deadline_after(ZX_SEC(1)), &obs);
    uint8_t c_rx[sizeof(iris_msg_hdr_t) + 64];
    g_zx_channel_read(fs_ch, 0, c_rx, NULL, sizeof(c_rx), 0, &act_b, &act_h);

    return (bytes_written == len) ? ZX_OK : ZX_ERR_IO;
}

static zx_status_t spawn_vfs_binary(zx_handle_t fs_ch, const char* name, const char* path,
                                    int argc, const char* argv[],
                                    const spawn_handle_t* extra, size_t num_extra,
                                    zx_handle_t* out_proc) __attribute__((noinline, unused));
static zx_status_t spawn_vfs_binary(zx_handle_t fs_ch, const char* name, const char* path,
                                    int argc, const char* argv[],
                                    const spawn_handle_t* extra, size_t num_extra,
                                    zx_handle_t* out_proc) {
    zx_handle_t vmo = ZX_HANDLE_INVALID;
    uint64_t sz = 0;
    const uint8_t* data = NULL;
    zx_status_t st = vfs_read_file_to_vmo(fs_ch, path, &vmo, &sz, &data);
    if (st != ZX_OK) return st;

    st = spawn_process_from_elf(name, data, (size_t)sz, argc, argv, extra, num_extra, out_proc);

    uint64_t vmo_sz = (sz + PAGE_SIZE - 1u) & PAGE_MASK;
    if (g_zx_vmar_unmap) g_zx_vmar_unmap(g_vmar_root_self, (zx_vaddr_t)data, vmo_sz);
    g_zx_handle_close(vmo);
    return st;
}

static void register_blk_client(zx_handle_t blk_ch, zx_handle_t new_srv) {
    iris_msg_hdr_t req; iris_msg_hdr_init(&req, IRIS_PROTO_DRIVER, IRIS_BLK_OP_GET_INFO, 1, 0);
    g_zx_channel_write(blk_ch, 0, &req, sizeof(req), &new_srv, 1);
    zx_signals_t obs = 0; g_zx_object_wait_one(blk_ch, ZX_CHANNEL_READABLE, ZX_TIME_INFINITE, &obs);
    uint8_t rep[256]; uint32_t act_b = 0, act_h = 0;
    g_zx_channel_read(blk_ch, 0, rep, NULL, sizeof(rep), 0, &act_b, &act_h);
}

static void register_gpu_client(zx_handle_t gpu_ch, zx_handle_t new_srv) {
    if (gpu_ch == ZX_HANDLE_INVALID || new_srv == ZX_HANDLE_INVALID) return;
    iris_msg_hdr_t req; iris_msg_hdr_init(&req, IRIS_PROTO_DRIVER, IRIS_GPU_OP_GET_INFO, 2, 0);
    g_zx_channel_write(gpu_ch, 0, &req, sizeof(req), &new_srv, 1);
    zx_signals_t obs = 0; g_zx_object_wait_one(gpu_ch, ZX_CHANNEL_READABLE, ZX_TIME_INFINITE, &obs);
    uint8_t rep[256]; zx_handle_t ret_hnd = ZX_HANDLE_INVALID; uint32_t act_b = 0, act_h = 0;
    g_zx_channel_read(gpu_ch, 0, rep, &ret_hnd, sizeof(rep), 1, &act_b, &act_h);
    if (act_h > 0 && ret_hnd != ZX_HANDLE_INVALID) {
        g_zx_handle_close(ret_hnd);
    }
}

static zx_handle_t request_compositor_client_channel(zx_handle_t comp_ch) {
    if (comp_ch == ZX_HANDLE_INVALID) return ZX_HANDLE_INVALID;
    static uint32_t s_comp_txid = 800;
    iris_msg_hdr_t req;
    iris_msg_hdr_init(&req, IRIS_PROTO_COMPOSITOR, IRIS_COMPOSITOR_OP_OPEN_CLIENT, ++s_comp_txid, 0);

    zx_status_t st = g_zx_channel_write(comp_ch, 0, &req, sizeof(req), NULL, 0);
    if (st != ZX_OK) return ZX_HANDLE_INVALID;

    zx_signals_t obs = 0;
    st = g_zx_object_wait_one(comp_ch, ZX_CHANNEL_READABLE, ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK) return ZX_HANDLE_INVALID;

    uint8_t rep[sizeof(iris_msg_hdr_t) + 64];
    zx_handle_t cli = ZX_HANDLE_INVALID;
    uint32_t act_b = 0, act_h = 0;
    st = g_zx_channel_read(comp_ch, 0, rep, &cli, sizeof(rep), 1, &act_b, &act_h);
    if (st != ZX_OK || act_h < 1) {
        if (cli != ZX_HANDLE_INVALID) g_zx_handle_close(cli);
        return ZX_HANDLE_INVALID;
    }
    return cli;
}

static zx_handle_t request_pty_client_channel(zx_handle_t pty_ch) {
    if (pty_ch == ZX_HANDLE_INVALID) return ZX_HANDLE_INVALID;
    static uint32_t s_pty_txid = 500;
    iris_msg_hdr_t req;
    iris_msg_hdr_init(&req, IRIS_PROTO_PTY, IRIS_PTY_OP_OPEN_CLIENT, ++s_pty_txid, 0);

    zx_status_t st = g_zx_channel_write(pty_ch, 0, &req, sizeof(req), NULL, 0);
    if (st != ZX_OK) return ZX_HANDLE_INVALID;

    zx_signals_t obs = 0;
    st = g_zx_object_wait_one(pty_ch, ZX_CHANNEL_READABLE, ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK) return ZX_HANDLE_INVALID;

    uint8_t rep[sizeof(iris_msg_hdr_t) + 64];
    zx_handle_t cli = ZX_HANDLE_INVALID;
    uint32_t act_b = 0, act_h = 0;
    st = g_zx_channel_read(pty_ch, 0, rep, &cli, sizeof(rep), 1, &act_b, &act_h);
    if (st != ZX_OK || act_h < 1) {
        if (cli != ZX_HANDLE_INVALID) g_zx_handle_close(cli);
        return ZX_HANDLE_INVALID;
    }
    return cli;
}

static zx_handle_t request_fs_client_channel(zx_handle_t fs_ch) {
    if (fs_ch == ZX_HANDLE_INVALID) return ZX_HANDLE_INVALID;
    static uint32_t s_fs_txid = 600;
    iris_msg_hdr_t req;
    iris_msg_hdr_init(&req, IRIS_PROTO_FS, IRIS_FS_OP_OPEN_CLIENT, ++s_fs_txid, 0);

    zx_status_t st = g_zx_channel_write(fs_ch, 0, &req, sizeof(req), NULL, 0);
    if (st != ZX_OK) return ZX_HANDLE_INVALID;

    zx_signals_t obs = 0;
    st = g_zx_object_wait_one(fs_ch, ZX_CHANNEL_READABLE, ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK) return ZX_HANDLE_INVALID;

    uint8_t rep[sizeof(iris_msg_hdr_t) + 64];
    zx_handle_t cli = ZX_HANDLE_INVALID;
    uint32_t act_b = 0, act_h = 0;
    st = g_zx_channel_read(fs_ch, 0, rep, &cli, sizeof(rep), 1, &act_b, &act_h);
    if (st != ZX_OK || act_h < 1) {
        if (cli != ZX_HANDLE_INVALID) g_zx_handle_close(cli);
        return ZX_HANDLE_INVALID;
    }
    return cli;
}

static zx_status_t confirm_boot_with_fshost(zx_handle_t fs_ch) {
    if (fs_ch == ZX_HANDLE_INVALID) return ZX_ERR_BAD_HANDLE;
    static uint32_t s_confirm_txid = 700;
    iris_msg_hdr_t req;
    iris_msg_hdr_init(&req, IRIS_PROTO_FS, IRIS_FS_OP_CONFIRM_BOOT, ++s_confirm_txid, 0);

    zx_status_t st = g_zx_channel_write(fs_ch, 0, &req, sizeof(req), NULL, 0);
    if (st != ZX_OK) return st;

    zx_signals_t obs = 0;
    st = g_zx_object_wait_one(fs_ch, ZX_CHANNEL_READABLE, iris_deadline_after(ZX_SEC(5)), &obs);
    if (st != ZX_OK) return st;

    iris_msg_hdr_t rep;
    uint32_t act_b = 0, act_h = 0;
    st = g_zx_channel_read(fs_ch, 0, &rep, NULL, sizeof(rep), 0, &act_b, &act_h);
    if (st != ZX_OK || act_b < sizeof(iris_msg_hdr_t)) return ZX_ERR_IO;
    return rep.status;
}

static inline void rtc_outb(uint16_t port, uint8_t val) {
#if defined(__x86_64__)
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
#else
    (void)port; (void)val;
#endif
}

static inline uint8_t rtc_inb(uint16_t port) {
#if defined(__x86_64__)
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
#else
    (void)port; return 0;
#endif
}

static void init_hardware_rtc(void) {
#if defined(__x86_64__)
    if (!g_zx_ioports_request || !g_zx_clock_adjust || g_root_resource == ZX_HANDLE_INVALID) return;
    if (g_zx_ioports_request(g_root_resource, 0x70, 2) != ZX_OK) return;

    /* Wait if update in progress (reg A bit 7) */
    for (int i = 0; i < 1000; i++) {
        rtc_outb(0x70, 0x0A);
        if (!(rtc_inb(0x71) & 0x80)) break;
    }

    rtc_outb(0x70, 0x00); uint8_t sec  = rtc_inb(0x71);
    rtc_outb(0x70, 0x02); uint8_t min  = rtc_inb(0x71);
    rtc_outb(0x70, 0x04); uint8_t hour = rtc_inb(0x71);
    rtc_outb(0x70, 0x07); uint8_t day  = rtc_inb(0x71);
    rtc_outb(0x70, 0x08); uint8_t mon  = rtc_inb(0x71);
    rtc_outb(0x70, 0x09); uint8_t year = rtc_inb(0x71);
    rtc_outb(0x70, 0x0B); uint8_t regb = rtc_inb(0x71);

    bool bcd = !(regb & 0x04);
    if (bcd) {
        sec  = ((sec >> 4) * 10) + (sec & 0x0F);
        min  = ((min >> 4) * 10) + (min & 0x0F);
        hour = (((hour & 0x70) >> 4) * 10) + (hour & 0x0F);
        day  = ((day >> 4) * 10) + (day & 0x0F);
        mon  = ((mon >> 4) * 10) + (mon & 0x0F);
        year = ((year >> 4) * 10) + (year & 0x0F);
    }

    uint16_t full_year = 2000 + (uint16_t)year;
    if (full_year < 2024) full_year += 100;

    static const uint64_t days_before_month[] = {
        0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    uint64_t days = 0;
    for (uint16_t y = 1970; y < full_year; ++y) {
        bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
        days += leap ? 366 : 365;
    }
    if (mon >= 1 && mon <= 12) {
        days += days_before_month[mon];
        bool leap = ((full_year % 4 == 0) && (full_year % 100 != 0)) || (full_year % 400 == 0);
        if (mon > 2 && leap) days++;
    }
    if (day >= 1) days += (day - 1);

    uint64_t epoch_sec = days * 86400ULL + (uint64_t)hour * 3600ULL + (uint64_t)min * 60ULL + sec;
    zx_time_t monotonic = g_zx_clock_get_monotonic ? g_zx_clock_get_monotonic() : 0;
    int64_t offset = (int64_t)(epoch_sec * 1000000000ULL) - (int64_t)monotonic;
    g_zx_clock_adjust(g_root_resource, ZX_CLOCK_UTC, offset);
    log_status(STATUS_OKAY, "iris_init", "Synchronized hardware RTC clock (UTC %04u-%02u-%02u %02u:%02u:%02u).",
               full_year, mon, day, hour, min, sec);
#elif defined(__aarch64__)
    if (!g_zx_vmo_create_physical || !g_zx_vmar_map || !g_zx_vmar_unmap || !g_zx_clock_adjust ||
        g_root_resource == ZX_HANDLE_INVALID || g_vmar_root_self == ZX_HANDLE_INVALID) {
        return;
    }
    /* QEMU ARM64 'virt' PL031 RTC MMIO base = 0x09010000, RTCDR (Unix epoch seconds) at +0x00 */
    zx_handle_t rtc_vmo = ZX_HANDLE_INVALID;
    if (g_zx_vmo_create_physical(g_root_resource, 0x09010000ULL, 4096, &rtc_vmo) != ZX_OK) {
        return;
    }
    zx_vaddr_t rtc_va = 0;
    if (g_zx_vmar_map(g_vmar_root_self, ZX_VM_PERM_READ | ZX_VM_MAP_RANGE, 0, rtc_vmo, 0, 4096, &rtc_va) == ZX_OK &&
        rtc_va != 0) {
        uint32_t epoch_sec = *(volatile const uint32_t*)(rtc_va + 0x00);
        g_zx_vmar_unmap(g_vmar_root_self, rtc_va, 4096);
        if (epoch_sec > 0) {
            zx_time_t monotonic = g_zx_clock_get_monotonic ? g_zx_clock_get_monotonic() : 0;
            int64_t offset = (int64_t)((uint64_t)epoch_sec * 1000000000ULL) - (int64_t)monotonic;
            g_zx_clock_adjust(g_root_resource, ZX_CLOCK_UTC, offset);
            log_status(STATUS_OKAY, "iris_init", "Synchronized PL031 hardware RTC clock (UTC epoch %u).", epoch_sec);
        }
    }
    g_zx_handle_close(rtc_vmo);
#endif
}

static void send_compositor_boot_stage(zx_handle_t comp_ch, uint32_t stage, zx_handle_t fs_ch_for_comp) {
    if (comp_ch == ZX_HANDLE_INVALID) {
        if (fs_ch_for_comp != ZX_HANDLE_INVALID) g_zx_handle_close(fs_ch_for_comp);
        return;
    }
    static uint32_t txid = 800;
    uint8_t buf[sizeof(iris_msg_hdr_t) + sizeof(iris_compositor_boot_stage_req_t)] __attribute__((aligned(8)));
    iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)buf;
    iris_compositor_boot_stage_req_t* req = (iris_compositor_boot_stage_req_t*)(buf + sizeof(iris_msg_hdr_t));
    iris_msg_hdr_init(hdr, IRIS_PROTO_COMPOSITOR, IRIS_COMPOSITOR_OP_SET_BOOT_STAGE, txid++,
                      sizeof(iris_compositor_boot_stage_req_t));
    req->stage = stage;

    uint32_t nh = (fs_ch_for_comp != ZX_HANDLE_INVALID) ? 1u : 0u;
    zx_status_t st = g_zx_channel_write(comp_ch, 0, buf, sizeof(buf),
                                        nh ? &fs_ch_for_comp : NULL, nh);
    if (st == ZX_OK) {
        zx_signals_t obs = 0;
        if (g_zx_object_wait_one(comp_ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                 iris_deadline_after(5000000000ULL), &obs) == ZX_OK &&
            (obs & ZX_CHANNEL_READABLE)) {
            iris_msg_hdr_t rep;
            uint32_t ab = 0, ah = 0;
            g_zx_channel_read(comp_ch, 0, &rep, NULL, sizeof(rep), 0, &ab, &ah);
        }
    } else if (fs_ch_for_comp != ZX_HANDLE_INVALID) {
        g_zx_handle_close(fs_ch_for_comp);
    }
}

static void send_fshost_user_logout(zx_handle_t fs_ch) {
    if (fs_ch == ZX_HANDLE_INVALID) return;
    iris_msg_hdr_t req;
    iris_msg_hdr_init(&req, IRIS_PROTO_FS, IRIS_FS_OP_USER_LOGOUT, 704, 0);
    if (g_zx_channel_write(fs_ch, 0, &req, sizeof(req), NULL, 0) == ZX_OK) {
        zx_signals_t obs = 0;
        if (g_zx_object_wait_one(fs_ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                 iris_deadline_after(3000000000ULL), &obs) == ZX_OK &&
            (obs & ZX_CHANNEL_READABLE)) {
            iris_msg_hdr_t rep;
            uint32_t ab = 0, ah = 0;
            g_zx_channel_read(fs_ch, 0, &rep, NULL, sizeof(rep), 0, &ab, &ah);
        }
    }
}

static void send_crypt_lock(zx_handle_t crypt_ch) {
    if (crypt_ch == ZX_HANDLE_INVALID) return;
    iris_msg_hdr_t req;
    iris_msg_hdr_init(&req, IRIS_PROTO_CRYPT, IRIS_CRYPT_OP_LOCK, 702, 0);
    if (g_zx_channel_write(crypt_ch, 0, &req, sizeof(req), NULL, 0) == ZX_OK) {
        zx_signals_t obs = 0;
        if (g_zx_object_wait_one(crypt_ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                 iris_deadline_after(2000000000ULL), &obs) == ZX_OK &&
            (obs & ZX_CHANNEL_READABLE)) {
            iris_msg_hdr_t rep;
            uint32_t ab = 0, ah = 0;
            g_zx_channel_read(crypt_ch, 0, &rep, NULL, sizeof(rep), 0, &ab, &ah);
        }
    }
}

static void send_fshost_sync(zx_handle_t fs_ch) {
    if (fs_ch == ZX_HANDLE_INVALID) return;
    iris_msg_hdr_t req;
    iris_msg_hdr_init(&req, IRIS_PROTO_FS, IRIS_FS_OP_SYNC, 703, 0);
    if (g_zx_channel_write(fs_ch, 0, &req, sizeof(req), NULL, 0) == ZX_OK) {
        zx_signals_t obs = 0;
        if (g_zx_object_wait_one(fs_ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                 iris_deadline_after(3000000000ULL), &obs) == ZX_OK &&
            (obs & ZX_CHANNEL_READABLE)) {
            iris_msg_hdr_t rep;
            uint32_t ab = 0, ah = 0;
            g_zx_channel_read(fs_ch, 0, &rep, NULL, sizeof(rep), 0, &ab, &ah);
        }
    }
}

static void stop_process_gracefully(const char* name, zx_handle_t* proc, zx_handle_t* ch, bool kill_first) {
    if (ch && *ch != ZX_HANDLE_INVALID) {
        g_zx_handle_close(*ch);
        *ch = ZX_HANDLE_INVALID;
    }
    if (proc && *proc != ZX_HANDLE_INVALID) {
        if (kill_first && g_zx_task_kill) {
            g_zx_task_kill(*proc);
        }
        zx_signals_t obs = 0;
        zx_status_t wst = g_zx_object_wait_one(*proc, ZX_PROCESS_TERMINATED,
                                               iris_deadline_after(2000000000ULL), &obs);
        if (wst != ZX_OK && !kill_first && g_zx_task_kill) {
            g_zx_task_kill(*proc);
            g_zx_object_wait_one(*proc, ZX_PROCESS_TERMINATED,
                                 iris_deadline_after(1000000000ULL), &obs);
        }
        g_zx_handle_close(*proc);
        *proc = ZX_HANDLE_INVALID;
        log_status(STATUS_OKAY, "iris_init", "Stopped %s cleanly.", name);
    }
}

static zx_status_t spawn_compositor(zx_handle_t gpu_ch, zx_handle_t fs_ch,
                                    zx_handle_t sys_ctrl_cli,
                                    bool boot_splash,
                                    bool show_crash_dialog,
                                    zx_handle_t* out_comp_client,
                                    zx_handle_t* out_proc_comp) {
    if (out_comp_client) *out_comp_client = ZX_HANDLE_INVALID;
    if (out_proc_comp) *out_proc_comp = ZX_HANDLE_INVALID;

    zx_handle_t gpu_for_comp = ZX_HANDLE_INVALID, gpu_srv_comp = ZX_HANDLE_INVALID;
    g_zx_channel_create(0, &gpu_for_comp, &gpu_srv_comp);
    register_gpu_client(gpu_ch, gpu_srv_comp);

    zx_handle_t comp_fs_client = (fs_ch != ZX_HANDLE_INVALID) ? request_fs_client_channel(fs_ch) : ZX_HANDLE_INVALID;

    zx_handle_t comp_client = ZX_HANDLE_INVALID, comp_server = ZX_HANDLE_INVALID;
    g_zx_channel_create(0, &comp_client, &comp_server);

    spawn_handle_t comp_hnds[6];
    size_t num_comp_hnds = 0;
    comp_hnds[num_comp_hnds++] = (spawn_handle_t){ comp_server,  PA_HND(PA_USER0, 0) };
    comp_hnds[num_comp_hnds++] = (spawn_handle_t){ gpu_for_comp, PA_HND(PA_USER1, 0) };
    if (comp_fs_client != ZX_HANDLE_INVALID) {
        comp_hnds[num_comp_hnds++] = (spawn_handle_t){ comp_fs_client, PA_HND(PA_NS_DIR, 0) };
    }
    if (g_bootfs_vmo != ZX_HANDLE_INVALID) {
        zx_handle_t bootfs_dup = ZX_HANDLE_INVALID;
        if (g_zx_handle_duplicate(g_bootfs_vmo, ZX_RIGHT_SAME_RIGHTS, &bootfs_dup) == ZX_OK) {
            comp_hnds[num_comp_hnds++] = (spawn_handle_t){ bootfs_dup, PA_HND(PA_VMO_BOOTFS, 0) };
        }
    }
    if (g_root_resource != ZX_HANDLE_INVALID) {
        zx_handle_t root_res_comp = ZX_HANDLE_INVALID;
        if (g_zx_handle_duplicate(g_root_resource, ZX_RIGHT_SAME_RIGHTS, &root_res_comp) == ZX_OK) {
            comp_hnds[num_comp_hnds++] = (spawn_handle_t){ root_res_comp, PA_HND(PA_RESOURCE, 0) };
        }
    }
    if (sys_ctrl_cli != ZX_HANDLE_INVALID) {
        zx_handle_t sys_dup = ZX_HANDLE_INVALID;
        if (g_zx_handle_duplicate(sys_ctrl_cli, ZX_RIGHT_SAME_RIGHTS, &sys_dup) == ZX_OK) {
            comp_hnds[num_comp_hnds++] = (spawn_handle_t){ sys_dup, PA_HND(PA_USER2, 1) };
        }
    }

    const char* comp_argv[3];
    int comp_argc = 0;
    comp_argv[comp_argc++] = "iris_compositor";
    if (boot_splash) comp_argv[comp_argc++] = "--boot-splash";
    if (show_crash_dialog) comp_argv[comp_argc++] = "--show-crash-dialog";

    zx_handle_t proc_comp = ZX_HANDLE_INVALID;
    zx_status_t st = spawn_bootfs_binary("iris_compositor", "bin/iris_compositor",
                                         comp_argc, comp_argv,
                                         comp_hnds, num_comp_hnds, &proc_comp);
    g_zx_handle_close(comp_server);
    g_zx_handle_close(gpu_for_comp);

    if (st == ZX_OK) {
        if (out_comp_client) *out_comp_client = comp_client;
        if (out_proc_comp)   *out_proc_comp   = proc_comp;
    } else {
        if (comp_client != ZX_HANDLE_INVALID) g_zx_handle_close(comp_client);
    }
    return st;
}

/* ==============================================================================
 * Bootstrap Supervisor Entry Point
 * ============================================================================== */

__attribute__((noreturn)) void _start(void* start_arg, void* vdso_base) {
    if (vdso_base != NULL) {
        g_vdso_base_self = (uintptr_t)vdso_base;
        vdso_init((uintptr_t)vdso_base);
    }

    zx_handle_t bootstrap_channel = (zx_handle_t)(uintptr_t)start_arg;
    uint8_t msg_buf[IRIS_BOOTSTRAP_MSG_BYTES] __attribute__((aligned(8)));
    zx_handle_t handles[IRIS_BOOTSTRAP_MAX_HANDLES];
    uint32_t act_bytes = 0, act_handles = 0;

    zx_status_t st = g_zx_channel_read(bootstrap_channel, 0, msg_buf, handles, sizeof(msg_buf),
                                       IRIS_BOOTSTRAP_MAX_HANDLES, &act_bytes, &act_handles);

    if (st == ZX_OK && act_bytes >= sizeof(zx_proc_args_t)) {
        const zx_proc_args_t* args = (const zx_proc_args_t*)msg_buf;
        if (args->protocol == ZX_PROCARGS_PROTOCOL && args->version == ZX_PROCARGS_VERSION) {
            const uint32_t* info = (const uint32_t*)(msg_buf + args->handle_info_off);
            for (uint32_t i = 0; i < act_handles; i++) {
                if (info[i] == PA_HND(PA_RESOURCE, 0)) g_root_resource = handles[i];
                else if (info[i] == PA_HND(PA_VMO_BOOTFS, 0)) g_bootfs_vmo = handles[i];
                else if (info[i] == PA_HND(PA_JOB_DEFAULT, 0)) g_root_job = handles[i];
                else if (info[i] == PA_HND(PA_VMO_VDSO, 0)) g_vdso_vmo = handles[i];
                else if (info[i] == PA_HND(PA_VMAR_ROOT, 0)) g_vmar_root_self = handles[i];
            }
        }
    }
    if (bootstrap_channel != ZX_HANDLE_INVALID) g_zx_handle_close(bootstrap_channel);

    st = g_zx_debuglog_create(g_root_resource, 0, &g_debuglog_handle);
    if (st == ZX_OK) {
        init_hardware_rtc();

        log_line("  _      _      ____   _____ ");
        log_line(" (_)    (_)    / __ \\ / ____|");
        log_line("  _ _ __ _ ___| |  | | (___  ");
        log_line(" | | '__| / __| |  | |\\___ \\ ");
        log_line(" | | |  | \\__ \\ |__| |____) |");
        log_line(" |_|_|  |_|___/\\____/|_____/ ");

        char node_line[64];
        snprintf(node_line, sizeof(node_line), "System Node: %s (%s)", IRIS_SYSTEM_NODE, IRIS_ARCH_NAME);
        log_line(node_line);

        /* 1. Extract root capabilities from userboot */
        if (g_root_resource != ZX_HANDLE_INVALID) log_status(STATUS_OKAY, "iris_init", "Extracted Root Resource capability.");
        if (g_bootfs_vmo != ZX_HANDLE_INVALID)    log_status(STATUS_OKAY, "iris_init", "Extracted BOOTFS VMO capability.");
        if (g_root_job != ZX_HANDLE_INVALID)      log_status(STATUS_OKAY, "iris_init", "Extracted Root Job capability.");
        if (g_vdso_vmo != ZX_HANDLE_INVALID)      log_status(STATUS_OKAY, "iris_init", "Extracted vDSO VMO capability.");
        if (g_vmar_root_self != ZX_HANDLE_INVALID)log_status(STATUS_OKAY, "iris_init", "Extracted Root VMAR capability.");

        /* Map BOOTFS VMO */
        uint64_t bsize = 0; g_zx_vmo_get_size(g_bootfs_vmo, &bsize);
        zx_vaddr_t baddr = 0; g_zx_vmar_map(g_vmar_root_self, ZX_VM_PERM_READ, 0, g_bootfs_vmo, 0, bsize, &baddr);
        g_bootfs_data = (const uint8_t*)baddr;

        /* 2. Scan PCIe ECAM for VirtIO devices */
        uint64_t ecam_phys = pci_get_default_ecam_base();
        zx_handle_t ecam_vmo = ZX_HANDLE_INVALID; zx_vaddr_t ecam_va = 0;
        st = g_zx_vmo_create_physical(g_root_resource, ecam_phys, PCI_ECAM_SIZE_16_BUSES, &ecam_vmo);
        if (st == ZX_OK) {
            st = g_zx_vmar_map(g_vmar_root_self, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_MAP_RANGE, 0, ecam_vmo, 0, PCI_ECAM_SIZE_16_BUSES, &ecam_va);
            g_zx_handle_close(ecam_vmo);
        }
        if (st == ZX_OK) log_status(STATUS_OKAY, "iris_init", "Mapped PCIe ECAM configuration space (base 0x%016llx).", (unsigned long long)ecam_phys);
        else log_status(STATUS_FAIL, "iris_init", "Failed to map PCIe ECAM configuration space.");

        virtio_pci_dev_t blk_dev; iris_memset(&blk_dev, 0, sizeof(blk_dev));
        if (pci_find_virtio_device(ecam_va, 16, 0, &blk_dev)) {
            log_status(STATUS_OKAY, "iris_init", "Discovered VirtIO Block Device at PCI %02x:%02x.%x.", blk_dev.bus, blk_dev.dev, blk_dev.func);
        } else {
            log_status(STATUS_FAIL, "iris_init", "No VirtIO Block Device discovered on PCIe bus.");
        }

        virtio_pci_dev_t input_dev; iris_memset(&input_dev, 0, sizeof(input_dev));
        if (pci_find_virtio_device_nth(ecam_va, 16, VIRTIO_DEV_ID_INPUT, 0, &input_dev)) {
            log_status(STATUS_OKAY, "iris_init", "Discovered VirtIO Input Device #0 at PCI %02x:%02x.%x.", input_dev.bus, input_dev.dev, input_dev.func);
        } else {
            log_status(STATUS_WARN, "iris_init", "VirtIO Input Device not detected, using UART fallback.");
        }

        virtio_pci_dev_t tablet_dev; iris_memset(&tablet_dev, 0, sizeof(tablet_dev));
        if (pci_find_virtio_device_nth(ecam_va, 16, VIRTIO_DEV_ID_INPUT, 1, &tablet_dev)) {
            log_status(STATUS_OKAY, "iris_init", "Discovered VirtIO Tablet Device #1 at PCI %02x:%02x.%x.", tablet_dev.bus, tablet_dev.dev, tablet_dev.func);
        }

        virtio_pci_dev_t gpu_dev; iris_memset(&gpu_dev, 0, sizeof(gpu_dev));
        bool has_gpu = pci_find_virtio_device(ecam_va, 16, VIRTIO_DEV_ID_GPU, &gpu_dev);
        if (has_gpu) {
            log_status(STATUS_OKAY, "iris_init", "Discovered VirtIO GPU Device at PCI %02x:%02x.%x (Device: 0x%04x, IRQ: %u).",
                       gpu_dev.bus, gpu_dev.dev, gpu_dev.func, gpu_dev.device_id, gpu_dev.irq_line);
        } else {
            log_status(STATUS_WARN, "iris_init", "VirtIO GPU Device not detected on PCIe bus.");
        }

        /* 3. Spawn core system daemons in dependency order:
         *    virtio_blk -> virtio_gpu -> iris_crypt -> iris_pty -> [passphrase] -> iris_fshost -> iris_compositor */
        zx_handle_t blk_client = ZX_HANDLE_INVALID, blk_server = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &blk_client, &blk_server);
        zx_handle_t root_res_blk = ZX_HANDLE_INVALID;
        zx_handle_t sys_ctrl_srv = ZX_HANDLE_INVALID, sys_ctrl_cli = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &sys_ctrl_srv, &sys_ctrl_cli);

        zx_handle_t gpu_client = ZX_HANDLE_INVALID, gpu_server = ZX_HANDLE_INVALID;
        zx_handle_t proc_gpu = ZX_HANDLE_INVALID;
        zx_handle_t comp_client = ZX_HANDLE_INVALID;
        zx_handle_t proc_comp = ZX_HANDLE_INVALID;
        if (has_gpu) {
            g_zx_channel_create(0, &gpu_client, &gpu_server);
            zx_handle_t root_res_gpu = ZX_HANDLE_INVALID;
            g_zx_handle_duplicate(g_root_resource, ZX_RIGHT_SAME_RIGHTS, &root_res_gpu);

            zx_handle_t gpu_bti = ZX_HANDLE_INVALID;
            if (g_zx_iommu_create && g_zx_bti_create) {
                zx_iommu_desc_dummy_t dummy_desc = {0};
                zx_handle_t iommu = ZX_HANDLE_INVALID;
                if (g_zx_iommu_create(g_root_resource, ZX_IOMMU_TYPE_DUMMY, &dummy_desc, sizeof(dummy_desc), &iommu) == ZX_OK) {
                    uint64_t bti_id = ((uint64_t)gpu_dev.bus << 8) | ((uint64_t)gpu_dev.dev << 3) | gpu_dev.func;
                    g_zx_bti_create(iommu, 0, bti_id, &gpu_bti);
                    g_zx_handle_close(iommu);
                }
            }

            zx_handle_t gpu_mmio_vmo = ZX_HANDLE_INVALID;
            if (gpu_dev.common_bar < 6 && gpu_dev.bars[gpu_dev.common_bar].phys_addr != 0 && g_zx_vmo_create_physical) {
                g_zx_vmo_create_physical(g_root_resource, gpu_dev.bars[gpu_dev.common_bar].phys_addr, 65536u, &gpu_mmio_vmo);
            }

            zx_handle_t gpu_irq_hnd = ZX_HANDLE_INVALID;
            if (gpu_dev.irq_line > 0 && gpu_dev.irq_line != 0xFFu && g_zx_interrupt_create) {
                g_zx_interrupt_create(g_root_resource, gpu_dev.irq_line, 0, &gpu_irq_hnd);
            }

            spawn_handle_t gpu_hnds[6];
            size_t num_gpu_hnds = 0;
            gpu_hnds[num_gpu_hnds++] = (spawn_handle_t){ gpu_server,   PA_HND(PA_USER0, 0) };
            gpu_hnds[num_gpu_hnds++] = (spawn_handle_t){ root_res_gpu, PA_HND(PA_RESOURCE, 0) };
            if (gpu_bti != ZX_HANDLE_INVALID)      gpu_hnds[num_gpu_hnds++] = (spawn_handle_t){ gpu_bti,      PA_HND(PA_USER1, 0) };
            if (gpu_mmio_vmo != ZX_HANDLE_INVALID) gpu_hnds[num_gpu_hnds++] = (spawn_handle_t){ gpu_mmio_vmo, PA_HND(PA_USER2, 0) };
            if (gpu_irq_hnd != ZX_HANDLE_INVALID)  gpu_hnds[num_gpu_hnds++] = (spawn_handle_t){ gpu_irq_hnd,  PA_HND(PA_USER2, 1) };

            spawn_bootfs_binary("virtio_gpu", "bin/virtio_gpu", 0, NULL, gpu_hnds, num_gpu_hnds, &proc_gpu);
            g_zx_handle_close(gpu_server);
            log_status(STATUS_OKAY, "iris_init", "Spawned VirtIO GPU Driver (bin/virtio_gpu).");

            if (spawn_compositor(gpu_client, ZX_HANDLE_INVALID, sys_ctrl_cli,
                                 true, false, &comp_client, &proc_comp) == ZX_OK) {
                log_status(STATUS_OKAY, "iris_init", "Spawned Window Compositor (--boot-splash).");
                send_compositor_boot_stage(comp_client, 1u, ZX_HANDLE_INVALID);
            }
        }

        zx_handle_t pty_client = ZX_HANDLE_INVALID, pty_server = ZX_HANDLE_INVALID;
        zx_handle_t proc_pty = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &pty_client, &pty_server);
        if (!has_gpu) {
            zx_handle_t root_res_pty = ZX_HANDLE_INVALID;
            g_zx_handle_duplicate(g_root_resource, ZX_RIGHT_SAME_RIGHTS, &root_res_pty);
            spawn_handle_t pty_hnds[] = {
                { pty_server,   PA_HND(PA_USER0, 0) },
                { root_res_pty, PA_HND(PA_RESOURCE, 0) }
            };
            spawn_bootfs_binary("iris_pty", "bin/iris_pty", 0, NULL, pty_hnds, 2, &proc_pty);
        } else {
            spawn_handle_t pty_hnds[] = {
                { pty_server, PA_HND(PA_USER0, 0) }
            };
            spawn_bootfs_binary("iris_pty", "bin/iris_pty", 0, NULL, pty_hnds, 1, &proc_pty);
        }
        g_zx_handle_close(pty_server);
        log_status(STATUS_OKAY, "iris_init", "Spawned Pseudo-Terminal Server (bin/iris_pty).");
        if (has_gpu) send_compositor_boot_stage(comp_client, 2u, ZX_HANDLE_INVALID);

        g_zx_handle_duplicate(g_root_resource, ZX_RIGHT_SAME_RIGHTS, &root_res_blk);
        spawn_handle_t blk_hnds[] = { { blk_server, PA_HND(PA_USER0, 0) }, { root_res_blk, PA_HND(PA_RESOURCE, 0) } };
        zx_handle_t proc_blk = ZX_HANDLE_INVALID;
        spawn_bootfs_binary("virtio_blk", "bin/virtio_blk", 0, NULL, blk_hnds, 2, &proc_blk);
        g_zx_handle_close(blk_server);
        log_status(STATUS_OKAY, "iris_init", "Spawned VirtIO Block Driver (bin/virtio_blk).");
        if (has_gpu) send_compositor_boot_stage(comp_client, 3u, ZX_HANDLE_INVALID);

        zx_handle_t blk_for_crypt = ZX_HANDLE_INVALID, blk_srv_crypt = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &blk_for_crypt, &blk_srv_crypt);
        register_blk_client(blk_client, blk_srv_crypt);

        zx_handle_t blk_for_fshost = ZX_HANDLE_INVALID, blk_srv_fshost = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &blk_for_fshost, &blk_srv_fshost);
        register_blk_client(blk_client, blk_srv_fshost);

        zx_handle_t crypt_client_for_its = ZX_HANDLE_INVALID, crypt_srv_for_its = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &crypt_client_for_its, &crypt_srv_for_its);
        zx_handle_t crypt_client_for_fshost = ZX_HANDLE_INVALID, crypt_srv_for_fshost = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &crypt_client_for_fshost, &crypt_srv_for_fshost);

        spawn_handle_t crypt_hnds[] = {
            { crypt_srv_for_its,    PA_HND(PA_USER0, 0) },
            { blk_for_crypt,        PA_HND(PA_USER1, 0) },
            { crypt_srv_for_fshost, PA_HND(PA_USER0, 1) }
        };
        zx_handle_t proc_crypt = ZX_HANDLE_INVALID;
        spawn_bootfs_binary("iris_crypt", "bin/iris_crypt", 0, NULL, crypt_hnds, 3, &proc_crypt);
        g_zx_handle_close(crypt_srv_for_its); g_zx_handle_close(crypt_srv_for_fshost);
        g_zx_handle_close(blk_for_crypt);
        log_status(STATUS_OKAY, "iris_init", "Spawned Storage Encryption Daemon (bin/iris_crypt).");

        zx_handle_t fshost_client = ZX_HANDLE_INVALID, fshost_server = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &fshost_client, &fshost_server);
        spawn_handle_t fshost_hnds[] = {
            { fshost_server,           PA_HND(PA_USER0, 0) },
            { blk_for_fshost,          PA_HND(PA_USER1, 0) },
            { crypt_client_for_fshost, PA_HND(PA_USER2, 0) }
        };
        zx_handle_t proc_fshost = ZX_HANDLE_INVALID;
        spawn_bootfs_binary("iris_fshost", "bin/iris_fshost", 0, NULL, fshost_hnds, 3, &proc_fshost);
        g_zx_handle_close(fshost_server); g_zx_handle_close(blk_for_fshost); g_zx_handle_close(crypt_client_for_fshost);
        log_status(STATUS_OKAY, "iris_init", "Spawned VFS Router & Filesystem Daemon (bin/iris_fshost).");

        if (has_gpu) {
            zx_handle_t comp_fs_ch = request_fs_client_channel(fshost_client);
            send_compositor_boot_stage(comp_client, 4u, comp_fs_ch);
        }

        /* Confirm active A/B GPT slot boot */
        if (fshost_client != ZX_HANDLE_INVALID) {
            if (confirm_boot_with_fshost(fshost_client) == ZX_OK) {
                log_status(STATUS_OKAY, "iris_init", "Confirmed active A/B GPT slot (boot_successful=1).");
            }
        }
        if (has_gpu) {
            send_compositor_boot_stage(comp_client, 5u, ZX_HANDLE_INVALID);
        }

        zx_handle_t proc_session_term = ZX_HANDLE_INVALID;

        /* ======================================================================
         * Phase 7: Full Service Supervision & Graceful Shutdown Loop
         * ====================================================================== */
        log_status(STATUS_OKAY, "iris_init", "Supervising system services and IRIS_PROTO_SYSTEM control channel.");
        bool do_shutdown = false;
        bool do_reboot = false;

        while (!do_shutdown && g_zx_object_wait_many != NULL) {
            zx_wait_item_t items[8];
            uint32_t kinds[8]; /* 0=sys_ctrl, 1=comp, 2=fshost, 3=crypt, 4=pty, 5=gpu, 6=blk */
            size_t nitems = 0;

            if (sys_ctrl_srv != ZX_HANDLE_INVALID) {
                items[nitems] = (zx_wait_item_t){ sys_ctrl_srv, ZX_CHANNEL_READABLE, 0 };
                kinds[nitems++] = 0;
            }
            if (proc_comp != ZX_HANDLE_INVALID) {
                items[nitems] = (zx_wait_item_t){ proc_comp, ZX_PROCESS_TERMINATED, 0 };
                kinds[nitems++] = 1;
            }
            if (proc_fshost != ZX_HANDLE_INVALID) {
                items[nitems] = (zx_wait_item_t){ proc_fshost, ZX_PROCESS_TERMINATED, 0 };
                kinds[nitems++] = 2;
            }
            if (proc_crypt != ZX_HANDLE_INVALID) {
                items[nitems] = (zx_wait_item_t){ proc_crypt, ZX_PROCESS_TERMINATED, 0 };
                kinds[nitems++] = 3;
            }
            if (proc_pty != ZX_HANDLE_INVALID) {
                items[nitems] = (zx_wait_item_t){ proc_pty, ZX_PROCESS_TERMINATED, 0 };
                kinds[nitems++] = 4;
            }
            if (proc_gpu != ZX_HANDLE_INVALID) {
                items[nitems] = (zx_wait_item_t){ proc_gpu, ZX_PROCESS_TERMINATED, 0 };
                kinds[nitems++] = 5;
            }
            if (proc_blk != ZX_HANDLE_INVALID) {
                items[nitems] = (zx_wait_item_t){ proc_blk, ZX_PROCESS_TERMINATED, 0 };
                kinds[nitems++] = 6;
            }

            if (nitems == 0) break;
            if (g_zx_object_wait_many(items, nitems, ZX_TIME_INFINITE) != ZX_OK) break;

            for (size_t idx = 0; idx < nitems; ++idx) {
                if (items[idx].pending == 0) continue;

                if (kinds[idx] == 0 && (items[idx].pending & ZX_CHANNEL_READABLE)) {
                    uint8_t msg_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_sys_session_req_t)] __attribute__((aligned(8)));
                    uint32_t ab = 0, ah = 0;
                    while (g_zx_channel_read(sys_ctrl_srv, 0, msg_buf, NULL, sizeof(msg_buf), 0, &ab, &ah) == ZX_OK) {
                        const iris_msg_hdr_t* req = (const iris_msg_hdr_t*)msg_buf;
                        if (ab < sizeof(iris_msg_hdr_t) || !iris_msg_hdr_is_valid(req, IRIS_PROTO_SYSTEM)) {
                            continue;
                        }
                        if (req->opcode == IRIS_SYS_OP_SHUTDOWN || req->opcode == IRIS_SYS_OP_REBOOT) {
                            do_shutdown = true;
                            do_reboot = (req->opcode == IRIS_SYS_OP_REBOOT);
                            iris_msg_hdr_t rep;
                            iris_msg_hdr_init_reply(&rep, req, ZX_OK, 0);
                            g_zx_channel_write(sys_ctrl_srv, 0, &rep, sizeof(rep), NULL, 0);
                            break;
                        } else if (req->opcode == IRIS_SYS_OP_SESSION_LOGIN) {
                            if (proc_session_term != ZX_HANDLE_INVALID) {
                                stop_process_gracefully("terminal", &proc_session_term, NULL, true);
                            }
                            if (has_gpu && comp_client != ZX_HANDLE_INVALID && pty_client != ZX_HANDLE_INVALID) {
                                zx_handle_t term_fs_client   = request_fs_client_channel(fshost_client);
                                zx_handle_t term_pty_client  = request_pty_client_channel(pty_client);
                                zx_handle_t term_comp_client = request_compositor_client_channel(comp_client);
                                zx_handle_t root_res_term    = ZX_HANDLE_INVALID;
                                g_zx_handle_duplicate(g_root_resource, ZX_RIGHT_SAME_RIGHTS, &root_res_term);
                                zx_handle_t sys_dup_term     = ZX_HANDLE_INVALID;
                                if (sys_ctrl_cli != ZX_HANDLE_INVALID) {
                                    g_zx_handle_duplicate(sys_ctrl_cli, ZX_RIGHT_SAME_RIGHTS, &sys_dup_term);
                                }
                                spawn_handle_t term_hnds[5];
                                size_t num_term_hnds = 0;
                                if (term_fs_client != ZX_HANDLE_INVALID)   term_hnds[num_term_hnds++] = (spawn_handle_t){ term_fs_client,   PA_HND(PA_NS_DIR, 0) };
                                if (term_pty_client != ZX_HANDLE_INVALID)  term_hnds[num_term_hnds++] = (spawn_handle_t){ term_pty_client,  PA_HND(PA_USER0, 0) };
                                if (term_comp_client != ZX_HANDLE_INVALID) term_hnds[num_term_hnds++] = (spawn_handle_t){ term_comp_client, PA_HND(PA_USER0, 1) };
                                if (root_res_term != ZX_HANDLE_INVALID)    term_hnds[num_term_hnds++] = (spawn_handle_t){ root_res_term,    PA_HND(PA_RESOURCE, 0) };
                                if (sys_dup_term != ZX_HANDLE_INVALID)     term_hnds[num_term_hnds++] = (spawn_handle_t){ sys_dup_term,     PA_HND(PA_USER2, 1) };
                                if (spawn_bootfs_binary("terminal", "bin/terminal", 0, NULL,
                                                        term_hnds, num_term_hnds, &proc_session_term) == ZX_OK) {
                                    log_status(STATUS_OKAY, "iris_init", "Spawned session terminal (bin/terminal).");
                                }
                            }
                            iris_msg_hdr_t rep;
                            iris_msg_hdr_init_reply(&rep, req, ZX_OK, 0);
                            g_zx_channel_write(sys_ctrl_srv, 0, &rep, sizeof(rep), NULL, 0);
                        } else if (req->opcode == IRIS_SYS_OP_SESSION_LOGOUT) {
                            if (proc_session_term != ZX_HANDLE_INVALID) {
                                stop_process_gracefully("terminal", &proc_session_term, NULL, true);
                            }
                            send_fshost_user_logout(fshost_client);
                            iris_msg_hdr_t rep;
                            iris_msg_hdr_init_reply(&rep, req, ZX_OK, 0);
                            g_zx_channel_write(sys_ctrl_srv, 0, &rep, sizeof(rep), NULL, 0);
                        } else if (req->opcode == IRIS_SYS_OP_PING) {
                            iris_msg_hdr_t rep;
                            iris_msg_hdr_init(&rep, IRIS_PROTO_SYSTEM, IRIS_SYS_OP_PONG, req->txid, 0);
                            g_zx_channel_write(sys_ctrl_srv, 0, &rep, sizeof(rep), NULL, 0);
                        } else if (req->opcode == IRIS_SYS_OP_GET_VERSION) {
                            iris_msg_hdr_t rep;
                            iris_msg_hdr_init_reply(&rep, req, ZX_OK, 0);
                            g_zx_channel_write(sys_ctrl_srv, 0, &rep, sizeof(rep), NULL, 0);
                        }
                    }
                } else if (kinds[idx] == 1 && (items[idx].pending & ZX_PROCESS_TERMINATED)) {
                    zx_info_process_t pinfo;
                    iris_memset(&pinfo, 0, sizeof(pinfo));
                    if (g_zx_object_get_info) {
                        g_zx_object_get_info(proc_comp, ZX_INFO_PROCESS, &pinfo, sizeof(pinfo), NULL, NULL);
                    }
                    g_zx_handle_close(proc_comp);
                    proc_comp = ZX_HANDLE_INVALID;
                    if (comp_client != ZX_HANDLE_INVALID) {
                        g_zx_handle_close(comp_client);
                        comp_client = ZX_HANDLE_INVALID;
                    }

                    char crash_buf[256];
                    snprintf(crash_buf, sizeof(crash_buf),
                             "Process: bin/iris_compositor\n"
                             "Return code: %lld\n"
                             "Observed signals: 0x%08x\n"
                             "Supervised recovery: iris_init respawned compositor\n",
                             (long long)pinfo.return_code, (unsigned int)items[idx].pending);

                    log_status(STATUS_FAIL, "iris_init",
                               "iris_compositor terminated (exit=%lld). Respawning with --show-crash-dialog.",
                               (long long)pinfo.return_code);
                    vfs_write_file_from_buffer(fshost_client, "/tmp/compositor_crash.log",
                                               crash_buf, iris_strlen(crash_buf));
                    if (spawn_compositor(gpu_client, fshost_client, sys_ctrl_cli,
                                         false, true, &comp_client, &proc_comp) != ZX_OK) {
                        log_status(STATUS_FAIL, "iris_init", "Failed to respawn iris_compositor.");
                    }
                } else if (items[idx].pending & ZX_PROCESS_TERMINATED) {
                    const char* sname = (kinds[idx] == 2) ? "iris_fshost" :
                                        (kinds[idx] == 3) ? "iris_crypt" :
                                        (kinds[idx] == 4) ? "iris_pty" :
                                        (kinds[idx] == 5) ? "virtio_gpu" : "virtio_blk";
                    zx_handle_t* ph = (kinds[idx] == 2) ? &proc_fshost :
                                      (kinds[idx] == 3) ? &proc_crypt :
                                      (kinds[idx] == 4) ? &proc_pty :
                                      (kinds[idx] == 5) ? &proc_gpu : &proc_blk;
                    zx_info_process_t pinfo;
                    iris_memset(&pinfo, 0, sizeof(pinfo));
                    if (g_zx_object_get_info) {
                        g_zx_object_get_info(*ph, ZX_INFO_PROCESS, &pinfo, sizeof(pinfo), NULL, NULL);
                    }
                    log_status(STATUS_FAIL, "iris_init", "Core service %s terminated unexpectedly (exit=%lld).",
                               sname, (long long)pinfo.return_code);
                    g_zx_handle_close(*ph);
                    *ph = ZX_HANDLE_INVALID;
                }
            }
        }

        if (do_shutdown) {
            log_status(STATUS_OKAY, "iris_init", "Initiating reverse-dependency graceful %s...",
                       do_reboot ? "reboot" : "poweroff");
            if (proc_session_term != ZX_HANDLE_INVALID) {
                stop_process_gracefully("terminal", &proc_session_term, NULL, true);
            }
            send_fshost_user_logout(fshost_client);
            stop_process_gracefully("iris_compositor", &proc_comp, &comp_client, true);
            stop_process_gracefully("iris_pty", &proc_pty, &pty_client, true);
            send_fshost_sync(fshost_client);
            stop_process_gracefully("iris_fshost", &proc_fshost, &fshost_client, true);
            send_crypt_lock(crypt_client_for_its);
            stop_process_gracefully("iris_crypt", &proc_crypt, &crypt_client_for_its, false);
            stop_process_gracefully("virtio_gpu", &proc_gpu, &gpu_client, false);
            stop_process_gracefully("virtio_blk", &proc_blk, &blk_client, false);

            if (g_zx_system_powerctl && g_root_resource != ZX_HANDLE_INVALID) {
                zx_system_powerctl_arg_t arg;
                iris_memset(&arg, 0, sizeof(arg));
                g_zx_system_powerctl(g_root_resource,
                                     do_reboot ? ZX_SYSTEM_POWERCTL_REBOOT : ZX_SYSTEM_POWERCTL_SHUTDOWN,
                                     &arg);
            }
        }
    }

    for (;;) {
        g_zx_nanosleep(ZX_TIME_INFINITE);
    }
}
