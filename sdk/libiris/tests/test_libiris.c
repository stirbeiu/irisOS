/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/tests/test_libiris.c
 * Purpose: Standalone verification test for libiris headers, ABI, and primitives.
 *
 * Licensing: MIT License. Permissive Zone 1 SDK test.
 */

#include <iris/iris.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ==============================================================================
 * Compile-Time Static Assertions (ABI Layout & Alignment Guarantees)
 * ============================================================================== */

_Static_assert(sizeof(iris_msg_hdr_t) == 24, "iris_msg_hdr_t must be exactly 24 bytes");
_Static_assert(__alignof__(iris_msg_hdr_t) <= 4, "iris_msg_hdr_t alignment must be <= 4");

_Static_assert(offsetof(iris_msg_hdr_t, magic) == 0x00, "magic offset must be 0x00");
_Static_assert(offsetof(iris_msg_hdr_t, protocol) == 0x04, "protocol offset must be 0x04");
_Static_assert(offsetof(iris_msg_hdr_t, opcode) == 0x08, "opcode offset must be 0x08");
_Static_assert(offsetof(iris_msg_hdr_t, txid) == 0x0C, "txid offset must be 0x0C");
_Static_assert(offsetof(iris_msg_hdr_t, status) == 0x10, "status offset must be 0x10");
_Static_assert(offsetof(iris_msg_hdr_t, payload_len) == 0x14, "payload_len offset must be 0x14");

_Static_assert(IRIS_IPC_MAGIC == 0x49524953u, "IRIS_IPC_MAGIC must be 'IRIS' (0x49524953)");
_Static_assert(IRIS_IPC_MAX_PAYLOAD == (65536u - 24u), "IRIS_IPC_MAX_PAYLOAD must be 65512");
_Static_assert(IRIS_PROTO_FS == 0x0002u, "IRIS_PROTO_FS must be 0x0002");
_Static_assert(IRIS_PROTO_CRYPT == 0x0003u, "IRIS_PROTO_CRYPT must be 0x0003");
_Static_assert(IRIS_PROTO_COMPOSITOR == 0x0004u, "IRIS_PROTO_COMPOSITOR must be 0x0004");
_Static_assert(IRIS_PROTO_PTY == 0x0005u, "IRIS_PROTO_PTY must be 0x0005");

/* ==============================================================================
 * Runtime Test Cases
 * ============================================================================== */

static int test_ipc_headers(void) {
    iris_msg_hdr_t req;
    iris_msg_hdr_init(&req, IRIS_PROTO_FS, IRIS_FS_OP_OPEN, 42, 128);

    if (req.magic != IRIS_IPC_MAGIC) return -1;
    if (req.protocol != IRIS_PROTO_FS) return -2;
    if (req.opcode != IRIS_FS_OP_OPEN) return -3;
    if (req.txid != 42) return -4;
    if (req.status != ZX_OK) return -5;
    if (req.payload_len != 128) return -6;

    if (!iris_msg_hdr_is_valid(&req, IRIS_PROTO_FS)) return -7;
    if (iris_msg_hdr_is_valid(&req, IRIS_PROTO_CRYPT)) return -8;

    iris_msg_hdr_t resp;
    iris_msg_hdr_init_reply(&resp, &req, ZX_OK, 256);

    if (resp.magic != IRIS_IPC_MAGIC) return -9;
    if (resp.protocol != IRIS_PROTO_FS) return -10;
    if (resp.opcode != IRIS_FS_OP_OPEN) return -11;
    if (resp.txid != 42) return -12;
    if (resp.status != ZX_OK) return -13;
    if (resp.payload_len != 256) return -14;

    if (!iris_msg_hdr_is_reply(&resp, &req)) return -15;

    return 0;
}

static int test_spawn_args_and_pa_fd(void) {
    /* Verify PA_HND calculation for file descriptor inheritance */
    uint32_t fd0_info = PA_HND(PA_FD, 0);
    uint32_t fd1_info = PA_HND(PA_FD, 1);
    uint32_t fd2_info = PA_HND(PA_FD, 2);

    if (PA_HND_TYPE(fd0_info) != PA_FD) return -20;
    if (PA_HND_ARG(fd0_info) != 0) return -21;

    if (PA_HND_TYPE(fd1_info) != PA_FD) return -22;
    if (PA_HND_ARG(fd1_info) != 1) return -23;

    if (PA_HND_TYPE(fd2_info) != PA_FD) return -24;
    if (PA_HND_ARG(fd2_info) != 2) return -25;

    iris_spawn_action_t actions[2] = {
        {
            .type = IRIS_SPAWN_ACTION_ADD_FD,
            .handle = 100,
            .handle_info = 0,
            .target_fd = 0,
        },
        {
            .type = IRIS_SPAWN_ACTION_TRANSFER_HANDLE,
            .handle = 101,
            .handle_info = PA_HND(PA_USER0, 5),
            .target_fd = -1,
        }
    };

    const char* argv[] = { "test_proc", "--debug", NULL };
    iris_spawn_args_t spawn_args = {
        .job = ZX_HANDLE_INVALID,
        .name = "test_proc",
        .entry = 0x10000,
        .stack_size = 64 * 1024,
        .vmo_vdso = ZX_HANDLE_INVALID,
        .vdso_base = 0,
        .argv = argv,
        .argc = 2,
        .envp = NULL,
        .envc = 0,
        .actions = actions,
        .num_actions = 2,
    };

    if (spawn_args.argc != 2) return -26;
    if (spawn_args.num_actions != 2) return -27;

    return 0;
}

static int test_api_linkage(void) {
    /* Verify symbol linkage across all modules */
    volatile void* p_chan_create = (void*)&iris_channel_create;
    volatile void* p_chan_send   = (void*)&iris_channel_send;
    volatile void* p_chan_recv   = (void*)&iris_channel_recv;
    volatile void* p_chan_call   = (void*)&iris_channel_call;
    volatile void* p_vmo_create  = (void*)&iris_vmo_create;
    volatile void* p_vmo_map     = (void*)&iris_vmo_map;
    volatile void* p_vmo_unmap   = (void*)&iris_vmo_unmap;
    volatile void* p_spawn       = (void*)&iris_spawn;
    volatile void* p_init        = (void*)&iris_init_runtime;

    if (!p_chan_create || !p_chan_send || !p_chan_recv || !p_chan_call) return -30;
    if (!p_vmo_create || !p_vmo_map || !p_vmo_unmap) return -31;
    if (!p_spawn || !p_init) return -32;

    return 0;
}

/* Freestanding entry point for freestanding cross-compilation testing */
void _start(void* start_arg, void* vdso_base) {
    (void)start_arg;
    (void)vdso_base;

    int res = test_ipc_headers();
    if (res != 0) {
        zx_process_exit((int64_t)res);
    }

    res = test_spawn_args_and_pa_fd();
    if (res != 0) {
        zx_process_exit((int64_t)res);
    }

    res = test_api_linkage();
    if (res != 0) {
        zx_process_exit((int64_t)res);
    }

    /* All verification checks passed cleanly */
    zx_process_exit(0);
}
