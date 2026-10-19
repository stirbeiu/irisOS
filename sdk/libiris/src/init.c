/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/src/init.c
 * Purpose: Runtime initialization and self capability accessors for irisOS processes.
 *
 * Licensing: MIT License. Permissive Zone 1 SDK source.
 */

#include <iris/init.h>
#include <zircon/process.h>

static zx_handle_t g_iris_proc_self = ZX_HANDLE_INVALID;
static zx_handle_t g_iris_vmar_self = ZX_HANDLE_INVALID;
static zx_handle_t g_iris_job_self  = ZX_HANDLE_INVALID;

void iris_init_runtime(zx_handle_t proc, zx_handle_t vmar, zx_handle_t job) {
    g_iris_proc_self = proc;
    g_iris_vmar_self = vmar;
    g_iris_job_self  = job;
}

zx_handle_t iris_process_self(void) {
    return g_iris_proc_self;
}

zx_handle_t iris_vmar_root_self(void) {
    return g_iris_vmar_self;
}

zx_handle_t iris_job_default(void) {
    return g_iris_job_self;
}

/* Weak fallbacks for standard Zircon process accessors if unprovided by libc */
__attribute__((weak)) zx_handle_t _zx_process_self(void) {
    return g_iris_proc_self;
}
__attribute__((weak, alias("_zx_process_self"))) zx_handle_t zx_process_self(void);

__attribute__((weak)) zx_handle_t _zx_vmar_root_self(void) {
    return g_iris_vmar_self;
}
__attribute__((weak, alias("_zx_vmar_root_self"))) zx_handle_t zx_vmar_root_self(void);

__attribute__((weak)) zx_handle_t _zx_job_default(void) {
    return g_iris_job_self;
}
__attribute__((weak, alias("_zx_job_default"))) zx_handle_t zx_job_default(void);
