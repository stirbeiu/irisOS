/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/src/channel.c
 * Purpose: Ergonomic C wrappers for sending/receiving IPC messages and handles over Zircon channels.
 *
 * Licensing: MIT License. Permissive Zone 1 SDK source.
 */

#include <iris/channel.h>
#include <iris/ipc.h>
#include <iris/vmo.h>
#include <zircon/syscalls.h>
#include <zircon/process.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Stack buffer threshold for zero-allocation IPC marshalling.
 * Messages with total wire size <= 4 KB are assembled directly on the stack.
 * Messages exceeding 4 KB (up to the 64 KB channel maximum) utilize a temporary
 * anonymous VMO mapping to avoid dynamic heap (malloc) dependencies in early boot.
 */
#define IRIS_STACK_MSG_LIMIT 4096u

/**
 * Freestanding memory copy implementation to eliminate external C runtime dependencies.
 */
static void* iris_memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

static void iris_close_handles(zx_handle_t* handles, uint32_t count) {
    if (!handles) {
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (handles[i] != ZX_HANDLE_INVALID) {
            zx_handle_close(handles[i]);
            handles[i] = ZX_HANDLE_INVALID;
        }
    }
}

static zx_status_t iris_temp_vmo_map(uint32_t size, zx_handle_t* out_vmo, zx_vaddr_t* out_vaddr) {
    zx_handle_t vmo = ZX_HANDLE_INVALID;
    zx_status_t status = zx_vmo_create(size, 0, &vmo);
    if (status != ZX_OK) {
        return status;
    }

    zx_vaddr_t vaddr = 0;
    status = zx_vmar_map(zx_vmar_root_self(),
                         ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                         0, vmo, 0, size, &vaddr);
    if (status != ZX_OK) {
        zx_handle_close(vmo);
        return status;
    }

    *out_vmo = vmo;
    *out_vaddr = vaddr;
    return ZX_OK;
}

static void iris_temp_vmo_unmap(zx_handle_t vmo, zx_vaddr_t vaddr, uint32_t size) {
    if (vaddr != 0) {
        zx_vmar_unmap(zx_vmar_root_self(), vaddr, size);
    }
    if (vmo != ZX_HANDLE_INVALID) {
        zx_handle_close(vmo);
    }
}

zx_status_t iris_channel_create(uint32_t flags,
                                zx_handle_t* out_ch0,
                                zx_handle_t* out_ch1) {
    if (!out_ch0 || !out_ch1) {
        return ZX_ERR_INVALID_ARGS;
    }
    /*
     * Wraps zx_channel_create(). On success, two bidirectional channel endpoints
     * are created with full default rights (TRANSFER, READ, WRITE, WAIT, etc.).
     */
    return zx_channel_create(flags, out_ch0, out_ch1);
}

zx_status_t iris_channel_send(zx_handle_t channel,
                              const iris_msg_hdr_t* hdr,
                              const void* payload,
                              const zx_handle_t* handles,
                              uint32_t num_handles) {
    /* Validate capability handle and input header invariants */
    if (channel == ZX_HANDLE_INVALID || !hdr) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (hdr->magic != IRIS_IPC_MAGIC) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (hdr->payload_len > IRIS_IPC_MAX_PAYLOAD) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (num_handles > IRIS_IPC_MAX_HANDLES) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (num_handles > 0 && !handles) {
        return ZX_ERR_INVALID_ARGS;
    }

    /* Fast path: Header-only message with zero trailing payload */
    if (hdr->payload_len == 0) {
        return zx_channel_write(channel, 0, hdr, sizeof(iris_msg_hdr_t),
                                handles, num_handles);
    }

    if (!payload) {
        return ZX_ERR_INVALID_ARGS;
    }

    uint32_t total_size = (uint32_t)sizeof(iris_msg_hdr_t) + hdr->payload_len;

    /*
     * Optimized Path (<= 8 KB):
     * Assemble header and payload contiguous on the thread stack.
     * This avoids VMO allocation and TLB invalidation overhead for high-frequency RPCs.
     */
    if (total_size <= IRIS_STACK_MSG_LIMIT) {
        uint8_t buf[IRIS_STACK_MSG_LIMIT];
        iris_memcpy(buf, hdr, sizeof(iris_msg_hdr_t));
        iris_memcpy(buf + sizeof(iris_msg_hdr_t), payload, hdr->payload_len);
        return zx_channel_write(channel, 0, buf, total_size, handles, num_handles);
    }

    /*
     * Heap-Free Fallback for Large Messages (> 4 KB up to 64 KB):
     * Allocate a temporary anonymous VMO, map into current address space,
     * pack the buffer, issue zx_channel_write(), and tear down the mapping.
     */
    zx_handle_t temp_vmo = ZX_HANDLE_INVALID;
    zx_vaddr_t vaddr = 0;
    zx_status_t status = iris_temp_vmo_map(total_size, &temp_vmo, &vaddr);
    if (status != ZX_OK) {
        return status;
    }

    iris_memcpy((void*)vaddr, hdr, sizeof(iris_msg_hdr_t));
    iris_memcpy((void*)(vaddr + sizeof(iris_msg_hdr_t)), payload, hdr->payload_len);

    status = zx_channel_write(channel, 0, (const void*)vaddr, total_size,
                              handles, num_handles);

    iris_temp_vmo_unmap(temp_vmo, vaddr, total_size);
    return status;
}

static zx_status_t iris_channel_parse_msg(const uint8_t* raw_buf,
                                          uint32_t act_bytes,
                                          zx_handle_t* out_handles,
                                          uint32_t act_handles,
                                          iris_msg_hdr_t* out_hdr,
                                          void* out_payload,
                                          uint32_t max_payload_len,
                                          uint32_t* actual_payload_len,
                                          uint32_t* actual_handles) {
    if (act_bytes < sizeof(iris_msg_hdr_t)) {
        iris_close_handles(out_handles, act_handles);
        return ZX_ERR_INVALID_ARGS;
    }

    const iris_msg_hdr_t* in_hdr = (const iris_msg_hdr_t*)raw_buf;
    if (in_hdr->magic != IRIS_IPC_MAGIC) {
        iris_close_handles(out_handles, act_handles);
        return ZX_ERR_INVALID_ARGS;
    }

    uint32_t recv_payload_len = act_bytes - (uint32_t)sizeof(iris_msg_hdr_t);
    if (in_hdr->payload_len != recv_payload_len) {
        iris_close_handles(out_handles, act_handles);
        return ZX_ERR_INVALID_ARGS;
    }

    iris_memcpy(out_hdr, in_hdr, sizeof(iris_msg_hdr_t));

    if (recv_payload_len > 0) {
        if (!out_payload || recv_payload_len > max_payload_len) {
            iris_close_handles(out_handles, act_handles);
            return ZX_ERR_BUFFER_TOO_SMALL;
        }
        iris_memcpy(out_payload, raw_buf + sizeof(iris_msg_hdr_t), recv_payload_len);
    }

    if (actual_payload_len) {
        *actual_payload_len = recv_payload_len;
    }
    if (actual_handles) {
        *actual_handles = act_handles;
    }
    return ZX_OK;
}

zx_status_t iris_channel_recv(zx_handle_t channel,
                              iris_msg_hdr_t* out_hdr,
                              void* out_payload,
                              uint32_t max_payload_len,
                              uint32_t* actual_payload_len,
                              zx_handle_t* out_handles,
                              uint32_t max_handles,
                              uint32_t* actual_handles) {
    if (channel == ZX_HANDLE_INVALID || !out_hdr) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (max_handles > 0 && !out_handles) {
        return ZX_ERR_INVALID_ARGS;
    }

    uint32_t max_bytes = (uint32_t)sizeof(iris_msg_hdr_t) + max_payload_len;
    if (max_bytes > IRIS_IPC_MAX_MSG_BYTES || max_bytes < sizeof(iris_msg_hdr_t)) {
        max_bytes = IRIS_IPC_MAX_MSG_BYTES;
    }

    uint32_t act_bytes = 0;
    uint32_t act_handles = 0;

    /*
     * Stack Path (<= 4 KB):
     * Read the message datagram into stack buffer, validate magic and lengths,
     * and copy output payload back into caller's buffer.
     */
    if (max_bytes <= IRIS_STACK_MSG_LIMIT) {
        uint8_t buf[IRIS_STACK_MSG_LIMIT];
        zx_status_t status = zx_channel_read(channel, 0, buf, out_handles,
                                             max_bytes, max_handles,
                                             &act_bytes, &act_handles);
        if (status != ZX_OK) {
            return status;
        }

        return iris_channel_parse_msg(buf, act_bytes, out_handles, act_handles,
                                      out_hdr, out_payload, max_payload_len,
                                      actual_payload_len, actual_handles);
    }

    /*
     * Heap-Free Fallback for Large Messages (> 4 KB up to 64 KB):
     * Temporary VMO mapping for datagram read.
     */
    zx_handle_t temp_vmo = ZX_HANDLE_INVALID;
    zx_vaddr_t vaddr = 0;
    zx_status_t status = iris_temp_vmo_map(max_bytes, &temp_vmo, &vaddr);
    if (status != ZX_OK) {
        return status;
    }

    status = zx_channel_read(channel, 0, (void*)vaddr, out_handles,
                             max_bytes, max_handles,
                             &act_bytes, &act_handles);
    if (status == ZX_OK) {
        status = iris_channel_parse_msg((const uint8_t*)vaddr, act_bytes,
                                        out_handles, act_handles,
                                        out_hdr, out_payload, max_payload_len,
                                        actual_payload_len, actual_handles);
    }

    iris_temp_vmo_unmap(temp_vmo, vaddr, max_bytes);
    return status;
}

zx_status_t iris_channel_call(zx_handle_t channel,
                              const iris_msg_hdr_t* req_hdr,
                              const void* req_payload,
                              const zx_handle_t* req_handles,
                              uint32_t num_req_handles,
                              iris_msg_hdr_t* resp_hdr,
                              void* resp_payload,
                              uint32_t max_resp_payload_len,
                              uint32_t* actual_resp_payload_len,
                              zx_handle_t* resp_handles,
                              uint32_t max_resp_handles,
                              uint32_t* actual_resp_handles,
                              zx_time_t deadline) {
    if (channel == ZX_HANDLE_INVALID || !req_hdr || !resp_hdr) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (max_resp_handles > 0 && !resp_handles) {
        return ZX_ERR_INVALID_ARGS;
    }

    zx_status_t status = iris_channel_send(channel, req_hdr, req_payload,
                                           req_handles, num_req_handles);
    if (status != ZX_OK) {
        return status;
    }

    zx_signals_t observed = 0;
    status = zx_object_wait_one(channel, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                deadline, &observed);
    if (status != ZX_OK) {
        return status;
    }
    if ((observed & ZX_CHANNEL_PEER_CLOSED) && !(observed & ZX_CHANNEL_READABLE)) {
        return ZX_ERR_PEER_CLOSED;
    }

    uint32_t act_handles = 0;
    status = iris_channel_recv(channel, resp_hdr, resp_payload, max_resp_payload_len,
                               actual_resp_payload_len, resp_handles, max_resp_handles,
                               &act_handles);
    if (status != ZX_OK) {
        return status;
    }

    if (!iris_msg_hdr_is_reply(resp_hdr, req_hdr)) {
        iris_close_handles(resp_handles, act_handles);
        return ZX_ERR_INVALID_ARGS;
    }

    if (actual_resp_handles) {
        *actual_resp_handles = act_handles;
    }
    return ZX_OK;
}

zx_status_t iris_channel_send_simple(zx_handle_t channel,
                                     uint32_t protocol,
                                     uint32_t opcode,
                                     uint32_t txid) {
    iris_msg_hdr_t hdr;
    iris_msg_hdr_init(&hdr, protocol, opcode, txid, 0);
    return iris_channel_send(channel, &hdr, NULL, NULL, 0);
}

zx_status_t iris_channel_reply(zx_handle_t channel,
                               const iris_msg_hdr_t* req_hdr,
                               int32_t status,
                               const void* payload,
                               uint32_t payload_len,
                               const zx_handle_t* handles,
                               uint32_t num_handles) {
    iris_msg_hdr_t reply_hdr;
    iris_msg_hdr_init_reply(&reply_hdr, req_hdr, status, payload_len);
    return iris_channel_send(channel, &reply_hdr, payload, handles, num_handles);
}
