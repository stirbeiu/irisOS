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
static zx_status_t (*g_zx_vmo_create)(uint64_t, uint32_t, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_vmo_get_size)(zx_handle_t, uint64_t*) = NULL;
static zx_status_t (*g_zx_vmo_write)(zx_handle_t, const void*, uint64_t, size_t) = NULL;
static zx_status_t (*g_zx_vmo_replace_as_executable)(zx_handle_t, zx_handle_t, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_vmar_map)(zx_handle_t, zx_vm_option_t, uint64_t, zx_handle_t, uint64_t, uint64_t, zx_vaddr_t*) = NULL;
static zx_status_t (*g_zx_vmar_allocate)(zx_handle_t, zx_vm_option_t, uint64_t, uint64_t, zx_handle_t*, zx_vaddr_t*) = NULL;
static zx_status_t (*g_zx_process_create)(zx_handle_t, const char*, size_t, uint32_t, zx_handle_t*, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_thread_create)(zx_handle_t, const char*, size_t, uint32_t, zx_handle_t*) = NULL;
static zx_status_t (*g_zx_process_start)(zx_handle_t, zx_handle_t, zx_vaddr_t, zx_vaddr_t, zx_handle_t, uintptr_t) = NULL;
static zx_status_t (*g_zx_object_wait_one)(zx_handle_t, zx_signals_t, zx_time_t, zx_signals_t*) = NULL;
static zx_status_t (*g_zx_nanosleep)(zx_time_t) = NULL;
static zx_status_t (*g_zx_vmo_create_physical)(zx_handle_t, uintptr_t, size_t, zx_handle_t*) = NULL;

static void vdso_init(uintptr_t base) {
    #define RESOLVE(fn) g_##fn = (void*)vdso_find(base, #fn, "_" #fn)
    RESOLVE(zx_channel_read); RESOLVE(zx_channel_create); RESOLVE(zx_channel_write);
    RESOLVE(zx_handle_close); RESOLVE(zx_handle_duplicate); RESOLVE(zx_debuglog_create);
    RESOLVE(zx_debuglog_write); RESOLVE(zx_vmo_create); RESOLVE(zx_vmo_get_size);
    RESOLVE(zx_vmo_write); RESOLVE(zx_vmo_replace_as_executable); RESOLVE(zx_vmar_map);
    RESOLVE(zx_vmar_allocate); RESOLVE(zx_process_create); RESOLVE(zx_thread_create);
    RESOLVE(zx_process_start); RESOLVE(zx_object_wait_one); RESOLVE(zx_nanosleep);
    RESOLVE(zx_vmo_create_physical);
    #undef RESOLVE
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
    if (g_debuglog_handle != ZX_HANDLE_INVALID && g_zx_debuglog_write && str) {
        g_zx_debuglog_write(g_debuglog_handle, 0, str, iris_strlen(str));
    }
}

static void log_status(const char* status, const char* unit, const char* fmt, ...) {
    if (g_debuglog_handle == ZX_HANDLE_INVALID || !g_zx_debuglog_write) return;
    char desc[192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(desc, sizeof(desc), fmt, ap);
    va_end(ap);

    char line[256];
    int len = snprintf(line, sizeof(line), "[%s] %s - %s", status, unit, desc);
    if (len > 0) {
        size_t wlen = (size_t)len < sizeof(line) ? (size_t)len : sizeof(line) - 1;
        g_zx_debuglog_write(g_debuglog_handle, 0, line, wlen);
    }
}

/* ==============================================================================
 * BOOTFS Loader & Process Spawner
 * ============================================================================== */

typedef struct { uint32_t magic, dirsize, reserved0, reserved1; } bootfs_header_t;
typedef struct { uint32_t name_len, data_len, data_off; char name[]; } bootfs_entry_t;
typedef struct { zx_handle_t handle; uint32_t info; } spawn_handle_t;

static bool find_bootfs_entry(const char* path, uint32_t* out_off, uint32_t* out_len) {
    if (!g_bootfs_data) return false;
    const bootfs_header_t* hdr = (const bootfs_header_t*)g_bootfs_data;
    if (hdr->magic != BOOTFS_MAGIC) return false;

    const uint8_t *p = (const uint8_t*)(hdr + 1), *end = p + hdr->dirsize;
    size_t target_len = iris_strlen(path) + 1;
    while (p < end) {
        const bootfs_entry_t* e = (const bootfs_entry_t*)p;
        size_t recsize = sizeof(bootfs_entry_t) + ((e->name_len + 3u) & ~3u);
        if (e->name_len == target_len && iris_memcmp(e->name, path, target_len) == 0) {
            *out_off = e->data_off; *out_len = e->data_len; return true;
        }
        p += recsize;
    }
    return false;
}

static void add_handle(zx_handle_t* hnds, uint32_t* infos, uint32_t* count, zx_handle_t h, uint32_t info, bool dup) {
    if (h == ZX_HANDLE_INVALID) return;
    if (dup) g_zx_handle_duplicate(h, ZX_RIGHT_SAME_RIGHTS, &hnds[*count]);
    else hnds[*count] = h;
    infos[(*count)++] = info;
}

static zx_status_t spawn_elf_binary(const char* name, const char* path, const spawn_handle_t* extra, size_t num_extra) {
    uint32_t file_off = 0, file_len = 0;
    if (!find_bootfs_entry(path, &file_off, &file_len)) return ZX_ERR_NOT_FOUND;

    zx_handle_t proc = ZX_HANDLE_INVALID, vmar = ZX_HANDLE_INVALID, thrd = ZX_HANDLE_INVALID;
    g_zx_process_create(g_root_job, name, (uint32_t)iris_strlen(name), 0, &proc, &vmar);
    g_zx_thread_create(proc, name, (uint32_t)iris_strlen(name), 0, &thrd);

    const uint8_t* elf = g_bootfs_data + file_off;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)elf;
    const Elf64_Phdr* ph = (const Elf64_Phdr*)(elf + eh->e_phoff);

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

    bool has_fd0 = false;
    for (size_t i = 0; i < num_extra; ++i) if (extra[i].info == PA_HND(PA_FD, 0)) has_fd0 = true;
    if (!has_fd0) add_handle(hnds, infos, &nh, g_debuglog_handle, PA_HND(PA_FD, 0), true);
    add_handle(hnds, infos, &nh, g_debuglog_handle, PA_HND(PA_FD, 1), true);
    add_handle(hnds, infos, &nh, g_debuglog_handle, PA_HND(PA_FD, 2), true);
    add_handle(hnds, infos, &nh, g_bootfs_vmo, PA_HND(PA_VMO_BOOTFS, 0), true);

    for (size_t i = 0; i < num_extra; ++i) {
        if (extra[i].handle != ZX_HANDLE_INVALID) add_handle(hnds, infos, &nh, extra[i].handle, extra[i].info, false);
    }

    uint8_t msg[1024]; iris_memset(msg, 0, sizeof(msg));
    zx_proc_args_t* pa = (zx_proc_args_t*)msg;
    pa->protocol = ZX_PROCARGS_PROTOCOL; pa->version = ZX_PROCARGS_VERSION;
    pa->handle_info_off = sizeof(zx_proc_args_t);
    iris_memcpy(msg + sizeof(zx_proc_args_t), infos, nh * sizeof(uint32_t));

    zx_handle_t b_tx = ZX_HANDLE_INVALID, b_rx = ZX_HANDLE_INVALID;
    g_zx_channel_create(0, &b_tx, &b_rx);
    g_zx_channel_write(b_tx, 0, msg, (uint32_t)(sizeof(zx_proc_args_t) + nh * sizeof(uint32_t)), hnds, nh);
    g_zx_handle_close(b_tx);

    zx_status_t st = g_zx_process_start(proc, thrd, eh->e_entry + load_bias, sp, b_rx, vdso_base);
    g_zx_handle_close(stk_vmo); g_zx_handle_close(vmar); g_zx_handle_close(thrd); g_zx_handle_close(proc);
    return st;
}

static void register_blk_client(zx_handle_t blk_ch, zx_handle_t new_srv) {
    iris_msg_hdr_t req; iris_msg_hdr_init(&req, IRIS_PROTO_DRIVER, IRIS_BLK_OP_GET_INFO, 1, 0);
    g_zx_channel_write(blk_ch, 0, &req, sizeof(req), &new_srv, 1);
    zx_signals_t obs = 0; g_zx_object_wait_one(blk_ch, ZX_CHANNEL_READABLE, ZX_TIME_INFINITE, &obs);
    uint8_t rep[256]; uint32_t act_b = 0, act_h = 0;
    g_zx_channel_read(blk_ch, 0, rep, NULL, sizeof(rep), 0, &act_b, &act_h);
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
         *    virtio_blk -> iris_crypt -> iris_fshost -> iris_pty */
        zx_handle_t blk_client = ZX_HANDLE_INVALID, blk_server = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &blk_client, &blk_server);
        zx_handle_t root_res_blk = ZX_HANDLE_INVALID;
        g_zx_handle_duplicate(g_root_resource, ZX_RIGHT_SAME_RIGHTS, &root_res_blk);
        spawn_handle_t blk_hnds[] = { { blk_server, PA_HND(PA_USER0, 0) }, { root_res_blk, PA_HND(PA_RESOURCE, 0) } };
        spawn_elf_binary("virtio_blk", "bin/virtio_blk", blk_hnds, 2);
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
        spawn_elf_binary("iris_crypt", "bin/iris_crypt", crypt_hnds, 3);
        g_zx_handle_close(crypt_srv_for_its); g_zx_handle_close(crypt_srv_for_fshost); g_zx_handle_close(blk_for_crypt);
        log_status(STATUS_OKAY, "iris_init", "Spawned Storage Encryption Daemon (bin/iris_crypt).");

        zx_handle_t fshost_client = ZX_HANDLE_INVALID, fshost_server = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &fshost_client, &fshost_server);
        spawn_handle_t fshost_hnds[] = {
            { fshost_server,           PA_HND(PA_USER0, 0) },
            { blk_for_fshost,          PA_HND(PA_USER1, 0) },
            { crypt_client_for_fshost, PA_HND(PA_USER2, 0) }
        };
        spawn_elf_binary("iris_fshost", "bin/iris_fshost", fshost_hnds, 3);
        g_zx_handle_close(fshost_server); g_zx_handle_close(blk_for_fshost); g_zx_handle_close(crypt_client_for_fshost);
        log_status(STATUS_OKAY, "iris_init", "Spawned VFS Router & Filesystem Daemon (bin/iris_fshost).");

        zx_handle_t pty_client = ZX_HANDLE_INVALID, pty_server = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &pty_client, &pty_server);
        zx_handle_t pty_its_client = ZX_HANDLE_INVALID, pty_its_srv = ZX_HANDLE_INVALID;
        g_zx_channel_create(0, &pty_its_client, &pty_its_srv);
        zx_handle_t root_res_pty = ZX_HANDLE_INVALID;
        g_zx_handle_duplicate(g_root_resource, ZX_RIGHT_SAME_RIGHTS, &root_res_pty);
        spawn_handle_t pty_hnds[] = {
            { pty_server,   PA_HND(PA_USER0, 0) },
            { root_res_pty, PA_HND(PA_RESOURCE, 0) },
            { pty_its_srv,  PA_HND(PA_USER0, 1) }
        };
        spawn_elf_binary("iris_pty", "bin/iris_pty", pty_hnds, 3);
        g_zx_handle_close(pty_server); g_zx_handle_close(pty_its_srv);
        log_status(STATUS_OKAY, "iris_init", "Spawned Pseudo-Terminal Server (bin/iris_pty).");

        /* 4. Spawn bin/its_irisOS and pass client channels */
        spawn_handle_t its_hnds[] = {
            { fshost_client,        PA_HND(PA_NS_DIR, 0) },
            { pty_its_client,       PA_HND(PA_FD, 0) },
            { pty_client,           PA_HND(PA_USER0, 0) },
            { blk_client,           PA_HND(PA_USER1, 0) },
            { crypt_client_for_its, PA_HND(PA_USER2, 0) }
        };
        spawn_elf_binary("its_irisOS", "bin/its_irisOS", its_hnds, 5);
        g_zx_handle_close(fshost_client); g_zx_handle_close(pty_its_client);
        g_zx_handle_close(pty_client); g_zx_handle_close(blk_client);
        g_zx_handle_close(crypt_client_for_its);
        log_status(STATUS_OKAY, "iris_init", "Spawned Interactive Test Suite (bin/its_irisOS).");

        /* 5. Supervise and sleep */
        log_status(STATUS_OKAY, "iris_init", "Bootstrap supervisor active, entering supervisor sleep.");
    }

    for (;;) {
        g_zx_nanosleep(ZX_TIME_INFINITE);
    }
}
