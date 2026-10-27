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
static zx_status_t (*g_zx_nanosleep)(zx_time_t) = NULL;
static zx_time_t   (*g_zx_deadline_after)(zx_duration_t) = NULL;
static zx_status_t (*g_zx_vmo_create_physical)(zx_handle_t, uintptr_t, size_t, zx_handle_t*) = NULL;

static void vdso_init(uintptr_t base) {
    #define RESOLVE(fn) g_##fn = (void*)vdso_find(base, #fn, "_" #fn)
    RESOLVE(zx_channel_read); RESOLVE(zx_channel_create); RESOLVE(zx_channel_write);
    RESOLVE(zx_handle_close); RESOLVE(zx_handle_duplicate); RESOLVE(zx_debuglog_create);
    RESOLVE(zx_debuglog_write); RESOLVE(zx_debug_write); RESOLVE(zx_vmo_create);
    RESOLVE(zx_vmo_get_size); RESOLVE(zx_vmo_write); RESOLVE(zx_vmo_replace_as_executable);
    RESOLVE(zx_vmar_map); RESOLVE(zx_vmar_unmap); RESOLVE(zx_vmar_allocate);
    RESOLVE(zx_process_create); RESOLVE(zx_thread_create); RESOLVE(zx_process_start);
    RESOLVE(zx_object_wait_one); RESOLVE(zx_nanosleep); RESOLVE(zx_deadline_after);
    RESOLVE(zx_vmo_create_physical);
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

static const char* iris_strstr(const char* haystack, const char* needle) {
    if (!needle || !*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return haystack;
    }
    return NULL;
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

static zx_status_t spawn_vfs_binary(zx_handle_t fs_ch, const char* name, const char* path,
                                    int argc, const char* argv[],
                                    const spawn_handle_t* extra, size_t num_extra,
                                    zx_handle_t* out_proc) __attribute__((noinline));
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

/* Boot mode selected by the kernel command line (iris.mode=test|shell). userboot
 * forwards the kernel cmdline to the first process as environment strings in the
 * bootstrap message. Default is "shell" when absent. */
static bool g_test_mode = false;

static void parse_boot_mode(const uint8_t* msg, uint32_t len, const zx_proc_args_t* args) {
    if (args->environ_off == 0 || args->environ_num == 0 || args->environ_off >= len) return;
    const char* p = (const char*)(msg + args->environ_off);
    const char* end = (const char*)(msg + len);
    while (p < end) {
        if (iris_strstr(p, "iris.mode=test") != NULL) {
            g_test_mode = true;
            return;
        }
        while (p < end && *p != '\0') p++;
        p++;
    }
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
            parse_boot_mode(msg_buf, act_bytes, args);
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
        if (pci_find_virtio_device(ecam_va, 16, VIRTIO_DEV_ID_INPUT, &input_dev)) {
            log_status(STATUS_OKAY, "iris_init", "Discovered VirtIO Input Device at PCI %02x:%02x.%x.", input_dev.bus, input_dev.dev, input_dev.func);
        } else {
            log_status(STATUS_WARN, "iris_init", "VirtIO Input Device not detected, using UART fallback.");
        }

        /* 3. Spawn core system daemons in dependency order:
         *    virtio_blk -> iris_crypt -> iris_pty -> [passphrase] -> iris_fshost */
        zx_handle_t blk_client = ZX_HANDLE_INVALID, blk_server = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &blk_client, &blk_server);
        zx_handle_t root_res_blk = ZX_HANDLE_INVALID;
        g_zx_handle_duplicate(g_root_resource, ZX_RIGHT_SAME_RIGHTS, &root_res_blk);
        spawn_handle_t blk_hnds[] = { { blk_server, PA_HND(PA_USER0, 0) }, { root_res_blk, PA_HND(PA_RESOURCE, 0) } };
        zx_handle_t proc_blk = ZX_HANDLE_INVALID;
        spawn_bootfs_binary("virtio_blk", "bin/virtio_blk", 0, NULL, blk_hnds, 2, &proc_blk);
        g_zx_handle_close(blk_server);
        log_status(STATUS_OKAY, "iris_init", "Spawned VirtIO Block Driver (bin/virtio_blk).");

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
        const char* crypt_argv_wait[] = { "iris_crypt", "--wait-unlock", NULL };
        zx_handle_t proc_crypt = ZX_HANDLE_INVALID;
        if (g_test_mode) {
            spawn_bootfs_binary("iris_crypt", "bin/iris_crypt", 0, NULL, crypt_hnds, 3, &proc_crypt);
        } else {
            spawn_bootfs_binary("iris_crypt", "bin/iris_crypt", 2, crypt_argv_wait, crypt_hnds, 3, &proc_crypt);
        }
        g_zx_handle_close(crypt_srv_for_its); g_zx_handle_close(crypt_srv_for_fshost); g_zx_handle_close(blk_for_crypt);
        log_status(STATUS_OKAY, "iris_init", "Spawned Storage Encryption Daemon (bin/iris_crypt).");

        zx_handle_t pty_client = ZX_HANDLE_INVALID, pty_server = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &pty_client, &pty_server);
        zx_handle_t root_res_pty = ZX_HANDLE_INVALID;
        g_zx_handle_duplicate(g_root_resource, ZX_RIGHT_SAME_RIGHTS, &root_res_pty);
        spawn_handle_t pty_hnds[] = {
            { pty_server,   PA_HND(PA_USER0, 0) },
            { root_res_pty, PA_HND(PA_RESOURCE, 0) }
        };
        zx_handle_t proc_pty = ZX_HANDLE_INVALID;
        spawn_bootfs_binary("iris_pty", "bin/iris_pty", 0, NULL, pty_hnds, 2, &proc_pty);
        g_zx_handle_close(pty_server);
        log_status(STATUS_OKAY, "iris_init", "Spawned Pseudo-Terminal Server (bin/iris_pty).");

        if (!g_test_mode) {
            /* Step 5.3: Interactive mode — spawn apps/sysutils/passphrase on iris_pty
             *           to unlock the Data partition before iris_fshost mounts /Users. */
            zx_handle_t pw_pty = request_pty_client_channel(pty_client);
            zx_handle_t pw_fd0 = ZX_HANDLE_INVALID, pw_fd1 = ZX_HANDLE_INVALID, pw_fd2 = ZX_HANDLE_INVALID;
            if (pw_pty != ZX_HANDLE_INVALID) {
                g_zx_handle_duplicate(pw_pty, ZX_RIGHT_SAME_RIGHTS, &pw_fd0);
                g_zx_handle_duplicate(pw_pty, ZX_RIGHT_SAME_RIGHTS, &pw_fd1);
                g_zx_handle_duplicate(pw_pty, ZX_RIGHT_SAME_RIGHTS, &pw_fd2);
            }
            spawn_handle_t pw_hnds[] = {
                { pw_pty,               PA_HND(PA_USER0, 0) },
                { pw_fd0,               PA_HND(PA_FD, 0) },
                { pw_fd1,               PA_HND(PA_FD, 1) },
                { pw_fd2,               PA_HND(PA_FD, 2) },
                { crypt_client_for_its, PA_HND(PA_USER2, 0) }
            };
            const char* pw_argv[] = { "passphrase", NULL };
            zx_handle_t proc_pw = ZX_HANDLE_INVALID;
            zx_status_t pw_st = spawn_bootfs_binary("passphrase", "bin/passphrase", 1, pw_argv, pw_hnds, 5, &proc_pw);
            if (pw_st == ZX_OK && proc_pw != ZX_HANDLE_INVALID) {
                log_status(STATUS_OKAY, "iris_init", "Spawned Interactive Disk Passphrase Prompt (bin/passphrase).");
                zx_signals_t obs = 0;
                g_zx_object_wait_one(proc_pw, ZX_PROCESS_TERMINATED, ZX_TIME_INFINITE, &obs);
                g_zx_handle_close(proc_pw);
            } else {
                log_status(STATUS_FAIL, "iris_init", "Failed to spawn bin/passphrase (%d).", pw_st);
            }
        }

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

        if (g_test_mode) {
            /* 4a. iris.mode=test: spawn the automated Interactive Test Suite */
            zx_handle_t its_fs_client = request_fs_client_channel(fshost_client);
            spawn_handle_t its_hnds[] = {
                { its_fs_client != ZX_HANDLE_INVALID ? its_fs_client : fshost_client, PA_HND(PA_NS_DIR, 0) },
                { pty_client,           PA_HND(PA_USER0, 0) },
                { blk_client,           PA_HND(PA_USER1, 0) },
                { crypt_client_for_its, PA_HND(PA_USER2, 0) }
            };
            zx_handle_t proc_its = ZX_HANDLE_INVALID;
            spawn_bootfs_binary("its_irisOS", "bin/its_irisOS", 0, NULL, its_hnds, 4, &proc_its);
            log_status(STATUS_OKAY, "iris_init", "Boot mode: test. Spawned Interactive Test Suite (bin/its_irisOS).");

            /* Wait for its_irisOS to terminate */
            zx_signals_t obs = 0;
            if (proc_its != ZX_HANDLE_INVALID) {
                g_zx_object_wait_one(proc_its, ZX_PROCESS_TERMINATED, ZX_TIME_INFINITE, &obs);
                g_zx_handle_close(proc_its);
            }
            log_status(STATUS_OKAY, "iris_init", "its_irisOS process terminated.");

            /* Step 5.4: Confirm active A/B GPT slot boot (boot_successful = 1) */
            if (its_fs_client != ZX_HANDLE_INVALID && fshost_client != ZX_HANDLE_INVALID) {
                if (confirm_boot_with_fshost(fshost_client) == ZX_OK) {
                    log_status(STATUS_OKAY, "iris_init", "Confirmed active A/B GPT slot (boot_successful=1).");
                }
                g_zx_handle_close(fshost_client);
                fshost_client = ZX_HANDLE_INVALID;
            }

            /* Clean daemon teardown for automated testing */
            if (proc_pty != ZX_HANDLE_INVALID) {
                g_zx_object_wait_one(proc_pty, ZX_PROCESS_TERMINATED, iris_deadline_after(ZX_SEC(5)), &obs);
                g_zx_handle_close(proc_pty);
            }
            if (proc_fshost != ZX_HANDLE_INVALID) {
                g_zx_object_wait_one(proc_fshost, ZX_PROCESS_TERMINATED, iris_deadline_after(ZX_SEC(5)), &obs);
                g_zx_handle_close(proc_fshost);
            }
            if (proc_crypt != ZX_HANDLE_INVALID) {
                g_zx_object_wait_one(proc_crypt, ZX_PROCESS_TERMINATED, iris_deadline_after(ZX_SEC(5)), &obs);
                g_zx_handle_close(proc_crypt);
            }
            if (proc_blk != ZX_HANDLE_INVALID) {
                g_zx_object_wait_one(proc_blk, ZX_PROCESS_TERMINATED, iris_deadline_after(ZX_SEC(5)), &obs);
                g_zx_handle_close(proc_blk);
            }

            log_status(STATUS_OKAY, "iris_init", "All daemons cleanly terminated.");
            log_status(STATUS_OKAY, "iris_init", "Test run completed cleanly.");
        } else {
            /* 4b. Default boot mode: immediately spawn /System/Bin/toybox (sh -i) attached to iris_pty */
            bool boot_confirmed = false;
            for (;;) {
                zx_handle_t shell_pty = request_pty_client_channel(pty_client);
                zx_handle_t shell_fs  = request_fs_client_channel(fshost_client);

                zx_handle_t pty_fd0 = ZX_HANDLE_INVALID;
                zx_handle_t pty_fd1 = ZX_HANDLE_INVALID;
                zx_handle_t pty_fd2 = ZX_HANDLE_INVALID;
                if (shell_pty != ZX_HANDLE_INVALID) {
                    g_zx_handle_duplicate(shell_pty, ZX_RIGHT_SAME_RIGHTS, &pty_fd0);
                    g_zx_handle_duplicate(shell_pty, ZX_RIGHT_SAME_RIGHTS, &pty_fd1);
                    g_zx_handle_duplicate(shell_pty, ZX_RIGHT_SAME_RIGHTS, &pty_fd2);
                }

                spawn_handle_t sh_hnds[8];
                size_t num_sh_hnds = 0;
                if (g_root_resource != ZX_HANDLE_INVALID) {
                    zx_handle_t root_res_shell = ZX_HANDLE_INVALID;
                    if (g_zx_handle_duplicate(g_root_resource, ZX_RIGHT_SAME_RIGHTS, &root_res_shell) == ZX_OK) {
                        sh_hnds[num_sh_hnds++] = (spawn_handle_t){ root_res_shell, PA_HND(PA_RESOURCE, 0) };
                    }
                }
                if (shell_fs != ZX_HANDLE_INVALID) {
                    sh_hnds[num_sh_hnds++] = (spawn_handle_t){ shell_fs,  PA_HND(PA_NS_DIR, 0) };
                }
                if (shell_pty != ZX_HANDLE_INVALID) {
                    sh_hnds[num_sh_hnds++] = (spawn_handle_t){ shell_pty, PA_HND(PA_USER0, 0) };
                }
                if (pty_fd0 != ZX_HANDLE_INVALID) {
                    sh_hnds[num_sh_hnds++] = (spawn_handle_t){ pty_fd0,   PA_HND(PA_FD, 0) };
                }
                if (pty_fd1 != ZX_HANDLE_INVALID) {
                    sh_hnds[num_sh_hnds++] = (spawn_handle_t){ pty_fd1,   PA_HND(PA_FD, 1) };
                }
                if (pty_fd2 != ZX_HANDLE_INVALID) {
                    sh_hnds[num_sh_hnds++] = (spawn_handle_t){ pty_fd2,   PA_HND(PA_FD, 2) };
                }

                const char* sh_argv[] = { "sh", "-i", NULL };
                zx_handle_t proc_shell = ZX_HANDLE_INVALID;
                zx_status_t sp_st = ZX_ERR_NOT_FOUND;

                if (fshost_client != ZX_HANDLE_INVALID) {
                    sp_st = spawn_vfs_binary(fshost_client, "sh", "/System/Bin/toybox", 2, sh_argv, sh_hnds, num_sh_hnds, &proc_shell);
                }
                if (sp_st != ZX_OK) {
                    sp_st = spawn_bootfs_binary("sh", "bin/toybox", 2, sh_argv, sh_hnds, num_sh_hnds, &proc_shell);
                }
                if (sp_st == ZX_OK && proc_shell != ZX_HANDLE_INVALID) {
                    if (!boot_confirmed && fshost_client != ZX_HANDLE_INVALID) {
                        if (confirm_boot_with_fshost(fshost_client) == ZX_OK) {
                            log_status(STATUS_OKAY, "iris_init", "Confirmed active A/B GPT slot (boot_successful=1).");
                        }
                        boot_confirmed = true;
                    }
                    log_status(STATUS_OKAY, "iris_init", "Interactive shell spawned (/System/Bin/toybox sh -i).");
                    zx_signals_t obs = 0;
                    g_zx_object_wait_one(proc_shell, ZX_PROCESS_TERMINATED, ZX_TIME_INFINITE, &obs);
                    g_zx_handle_close(proc_shell);
                    log_status(STATUS_WARN, "iris_init", "Shell exited, respawning in 1s...");
                    g_zx_nanosleep(iris_deadline_after(ZX_SEC(1)));
                } else {
                    log_status(STATUS_FAIL, "iris_init", "Failed to spawn shell (%d), retrying in 2s...", sp_st);
                    for (size_t i = 0; i < num_sh_hnds; i++) {
                        if (sh_hnds[i].handle != ZX_HANDLE_INVALID) {
                            g_zx_handle_close(sh_hnds[i].handle);
                        }
                    }
                    g_zx_nanosleep(iris_deadline_after(ZX_SEC(2)));
                }
            }
        }
    }

    for (;;) {
        g_zx_nanosleep(ZX_TIME_INFINITE);
    }
}
