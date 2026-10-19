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
 * Messages with total wire size <= 8 KB are assembled directly on the stack.
 * Messages exceeding 8 KB (up to the 64 KB channel maximum) utilize a temporary
 * anonymous VMO mapping to avoid dynamic heap (malloc) dependencies in early boot.
 */
#define IRIS_STACK_MSG_LIMIT 8192u

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
     * Heap-Free Fallback for Large Messages (> 8 KB up to 64 KB):
     * Allocate a temporary anonymous VMO, map into current address space,
     * pack the buffer, issue zx_channel_write(), and tear down the mapping.
     */
    zx_handle_t temp_vmo = ZX_HANDLE_INVALID;
    zx_status_t status = zx_vmo_create(total_size, 0, &temp_vmo);
    if (status != ZX_OK) {
        return status;
    }

    zx_vaddr_t vaddr = 0;
    status = zx_vmar_map(zx_vmar_root_self(),
                         ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                         0, temp_vmo, 0, total_size, &vaddr);
    if (status != ZX_OK) {
        zx_handle_close(temp_vmo);
        return status;
    }

    iris_memcpy((void*)vaddr, hdr, sizeof(iris_msg_hdr_t));
    iris_memcpy((void*)(vaddr + sizeof(iris_msg_hdr_t)), payload, hdr->payload_len);

    status = zx_channel_write(channel, 0, (const void*)vaddr, total_size,
                              handles, num_handles);

    zx_vmar_unmap(zx_vmar_root_self(), vaddr, total_size);
    zx_handle_close(temp_vmo);
    return status;
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

    uint32_t max_bytes = (uint32_t)sizeof(iris_msg_hdr_t) + max_payload_len;
    if (max_bytes > IRIS_IPC_MAX_MSG_BYTES) {
        max_bytes = IRIS_IPC_MAX_MSG_BYTES;
    }

    uint32_t act_bytes = 0;
    uint32_t act_handles = 0;

    /*
     * Stack Path (<= 8 KB):
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

        if (act_bytes < sizeof(iris_msg_hdr_t)) {
            return ZX_ERR_INVALID_ARGS;
        }

        const iris_msg_hdr_t* in_hdr = (const iris_msg_hdr_t*)buf;
        if (in_hdr->magic != IRIS_IPC_MAGIC) {
            return ZX_ERR_INVALID_ARGS;
        }

        uint32_t recv_payload_len = act_bytes - (uint32_t)sizeof(iris_msg_hdr_t);
        if (in_hdr->payload_len != recv_payload_len) {
            return ZX_ERR_INVALID_ARGS;
        }

        iris_memcpy(out_hdr, in_hdr, sizeof(iris_msg_hdr_t));

        if (recv_payload_len > 0) {
            if (!out_payload || recv_payload_len > max_payload_len) {
                return ZX_ERR_BUFFER_TOO_SMALL;
            }
            iris_memcpy(out_payload, buf + sizeof(iris_msg_hdr_t), recv_payload_len);
        }

        if (actual_payload_len) {
            *actual_payload_len = recv_payload_len;
        }
        if (actual_handles) {
            *actual_handles = act_handles;
        }
        return ZX_OK;
    }

    /*
     * Heap-Free Fallback for Large Messages (> 8 KB up to 64 KB):
     * Temporary VMO mapping for datagram read.
     */
    zx_handle_t temp_vmo = ZX_HANDLE_INVALID;
    zx_status_t status = zx_vmo_create(max_bytes, 0, &temp_vmo);
    if (status != ZX_OK) {
        return status;
    }

    zx_vaddr_t vaddr = 0;
    status = zx_vmar_map(zx_vmar_root_self(),
                         ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                         0, temp_vmo, 0, max_bytes, &vaddr);
    if (status != ZX_OK) {
        zx_handle_close(temp_vmo);
        return status;
    }

    status = zx_channel_read(channel, 0, (void*)vaddr, out_handles,
                             max_bytes, max_handles,
                             &act_bytes, &act_handles);
    if (status != ZX_OK) {
        zx_vmar_unmap(zx_vmar_root_self(), vaddr, max_bytes);
        zx_handle_close(temp_vmo);
        return status;
    }

    if (act_bytes < sizeof(iris_msg_hdr_t)) {
        zx_vmar_unmap(zx_vmar_root_self(), vaddr, max_bytes);
        zx_handle_close(temp_vmo);
        return ZX_ERR_INVALID_ARGS;
    }

    const iris_msg_hdr_t* in_hdr = (const iris_msg_hdr_t*)vaddr;
    if (in_hdr->magic != IRIS_IPC_MAGIC) {
        zx_vmar_unmap(zx_vmar_root_self(), vaddr, max_bytes);
        zx_handle_close(temp_vmo);
        return ZX_ERR_INVALID_ARGS;
    }

    uint32_t recv_payload_len = act_bytes - (uint32_t)sizeof(iris_msg_hdr_t);
    iris_memcpy(out_hdr, in_hdr, sizeof(iris_msg_hdr_t));

    if (recv_payload_len > 0) {
        if (!out_payload || recv_payload_len > max_payload_len) {
            zx_vmar_unmap(zx_vmar_root_self(), vaddr, max_bytes);
            zx_handle_close(temp_vmo);
            return ZX_ERR_BUFFER_TOO_SMALL;
        }
        iris_memcpy(out_payload, (const void*)(vaddr + sizeof(iris_msg_hdr_t)), recv_payload_len);
    }

    if (actual_payload_len) {
        *actual_payload_len = recv_payload_len;
    }
    if (actual_handles) {
        *actual_handles = act_handles;
    }

    zx_vmar_unmap(zx_vmar_root_self(), vaddr, max_bytes);
    zx_handle_close(temp_vmo);
    return ZX_OK;
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
    if (req_hdr->magic != IRIS_IPC_MAGIC) {
        return ZX_ERR_INVALID_ARGS;
    }

    uint32_t wr_size = (uint32_t)sizeof(iris_msg_hdr_t) + req_hdr->payload_len;
    uint32_t rd_size = (uint32_t)sizeof(iris_msg_hdr_t) + max_resp_payload_len;

    /* Assemble request in stack or temporary VMO buffers */
    uint8_t wr_buf[IRIS_STACK_MSG_LIMIT];
    uint8_t rd_buf[IRIS_STACK_MSG_LIMIT];
    void* wr_ptr = wr_buf;
    void* rd_ptr = rd_buf;
    zx_handle_t wr_vmo = ZX_HANDLE_INVALID;
    zx_handle_t rd_vmo = ZX_HANDLE_INVALID;
    zx_vaddr_t wr_vaddr = 0;
    zx_vaddr_t rd_vaddr = 0;

    if (wr_size > IRIS_STACK_MSG_LIMIT) {
        zx_status_t st = zx_vmo_create(wr_size, 0, &wr_vmo);
        if (st != ZX_OK) return st;
        st = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                         0, wr_vmo, 0, wr_size, &wr_vaddr);
        if (st != ZX_OK) {
            zx_handle_close(wr_vmo);
            return st;
        }
        wr_ptr = (void*)wr_vaddr;
    }

    if (rd_size > IRIS_STACK_MSG_LIMIT) {
        zx_status_t st = zx_vmo_create(rd_size, 0, &rd_vmo);
        if (st != ZX_OK) {
            if (wr_vaddr) {
                zx_vmar_unmap(zx_vmar_root_self(), wr_vaddr, wr_size);
                zx_handle_close(wr_vmo);
            }
            return st;
        }
        st = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                         0, rd_vmo, 0, rd_size, &rd_vaddr);
        if (st != ZX_OK) {
            zx_handle_close(rd_vmo);
            if (wr_vaddr) {
                zx_vmar_unmap(zx_vmar_root_self(), wr_vaddr, wr_size);
                zx_handle_close(wr_vmo);
            }
            return st;
        }
        rd_ptr = (void*)rd_vaddr;
    }

    iris_memcpy(wr_ptr, req_hdr, sizeof(iris_msg_hdr_t));
    if (req_hdr->payload_len > 0 && req_payload) {
        iris_memcpy((uint8_t*)wr_ptr + sizeof(iris_msg_hdr_t), req_payload, req_hdr->payload_len);
    }

    zx_channel_call_args_t call_args = {
        .wr_bytes = wr_ptr,
        .wr_handles = req_handles,
        .rd_bytes = rd_ptr,
        .rd_handles = resp_handles,
        .wr_num_bytes = wr_size,
        .wr_num_handles = num_req_handles,
        .rd_num_bytes = rd_size,
        .rd_num_handles = max_resp_handles,
    };

    uint32_t act_bytes = 0;
    uint32_t act_handles = 0;

    /* Issue synchronous channel call: atomically writes request and awaits reply */
    zx_status_t status = zx_channel_call(channel, 0, deadline, &call_args,
                                         &act_bytes, &act_handles);

    if (wr_vaddr) {
        zx_vmar_unmap(zx_vmar_root_self(), wr_vaddr, wr_size);
        zx_handle_close(wr_vmo);
    }

    if (status != ZX_OK) {
        if (rd_vaddr) {
            zx_vmar_unmap(zx_vmar_root_self(), rd_vaddr, rd_size);
            zx_handle_close(rd_vmo);
        }
        return status;
    }

    if (act_bytes < sizeof(iris_msg_hdr_t)) {
        if (rd_vaddr) {
            zx_vmar_unmap(zx_vmar_root_self(), rd_vaddr, rd_size);
            zx_handle_close(rd_vmo);
        }
        return ZX_ERR_INVALID_ARGS;
    }

    const iris_msg_hdr_t* in_hdr = (const iris_msg_hdr_t*)rd_ptr;
    if (in_hdr->magic != IRIS_IPC_MAGIC || in_hdr->txid != req_hdr->txid) {
        if (rd_vaddr) {
            zx_vmar_unmap(zx_vmar_root_self(), rd_vaddr, rd_size);
            zx_handle_close(rd_vmo);
        }
        return ZX_ERR_INVALID_ARGS;
    }

    iris_memcpy(resp_hdr, in_hdr, sizeof(iris_msg_hdr_t));
    uint32_t resp_payload_len = act_bytes - (uint32_t)sizeof(iris_msg_hdr_t);

    if (resp_payload_len > 0) {
        if (!resp_payload || resp_payload_len > max_resp_payload_len) {
            if (rd_vaddr) {
                zx_vmar_unmap(zx_vmar_root_self(), rd_vaddr, rd_size);
                zx_handle_close(rd_vmo);
            }
            return ZX_ERR_BUFFER_TOO_SMALL;
        }
        iris_memcpy(resp_payload, (const uint8_t*)rd_ptr + sizeof(iris_msg_hdr_t), resp_payload_len);
    }

    if (actual_resp_payload_len) {
        *actual_resp_payload_len = resp_payload_len;
    }
    if (actual_resp_handles) {
        *actual_resp_handles = act_handles;
    }

    if (rd_vaddr) {
        zx_vmar_unmap(zx_vmar_root_self(), rd_vaddr, rd_size);
        zx_handle_close(rd_vmo);
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
