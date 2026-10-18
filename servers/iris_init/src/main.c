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
 *     - The entry point is `_start(void* start_arg)`, where `start_arg` conveys the
 *       initial bootstrap channel handle transferred via `zx_process_start()`.
 *
 *  2. Capability Handle Extraction:
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
 *  3. Early Serial Logging:
 *     - Creates an early kernel debug log handle using `zx_debuglog_create(root_resource, 0, &log)`
 *       to stream early bootstrap diagnostics to UART/console before `iris_pty` is initialized.
 *     - Outputs the official irisOS bootstrap banner:
 *       `"[irisOS] Welcome to irisOS (x86_64 / arm64) - iris_init bootstrap successful!\n"`.
 *
 *  4. Future Execution Flow (Phase 4 & 5):
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
static zx_handle_t g_root_resource = ZX_HANDLE_INVALID;
static zx_handle_t g_bootfs_vmo    = ZX_HANDLE_INVALID;
static zx_handle_t g_root_job       = ZX_HANDLE_INVALID;
static zx_handle_t g_vdso_vmo      = ZX_HANDLE_INVALID;
static zx_handle_t g_debuglog_handle = ZX_HANDLE_INVALID;

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
 */
__attribute__((noreturn)) void _start(void* start_arg) {
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
