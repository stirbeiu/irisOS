/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/include/iris/channel.h
 * Purpose: Ergonomic C wrappers for sending/receiving IPC messages and handles over Zircon channels.
 *
 * Licensing: MIT License. Permissive Zone 1 SDK header.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef IRIS_CHANNEL_H_
#define IRIS_CHANNEL_H_

/**
 * @file channel.h
 * @brief Ergonomic channel IPC wrappers for irisOS.
 *
 * ARCHITECTURAL DESIGN & CAPABILITY MODEL:
 *  Channels in Zircon are bidirectional, datagram-oriented IPC primitives.
 *  This module provides safe, high-level C abstractions for sending and receiving
 *  messages formatted according to the 24-byte `iris_msg_hdr_t` wire protocol,
 *  alongside capability handle transfers.
 *
 * CAPABILITY LIFECYCLE:
 *  - Handles passed to `iris_channel_send()` or `iris_channel_call()` in the `handles`
 *    array are CONSUMED (transferred to the recipient process) on success (`ZX_OK`).
 *    On failure, ownership remains with the caller.
 *  - Handles returned by `iris_channel_recv()` or `iris_channel_call()` in the `out_handles`
 *    array are ACQUIRED by the caller; the caller is responsible for eventual closure
 *    via `zx_handle_close()`.
 */

#include <iris/ipc.h>
#include <zircon/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a connected pair of bidirectional Zircon channel endpoints.
 *
 * @param flags Creation flags (currently 0 reserved for future use).
 * @param[out] out_ch0 Returns the first endpoint capability handle.
 * @param[out] out_ch1 Returns the second endpoint capability handle.
 *
 * @return ZX_OK on success.
 * @return ZX_ERR_NO_MEMORY if system resources were insufficient.
 * @return ZX_ERR_INVALID_ARGS if either pointer is NULL.
 *
 * @note Capability Lifecycle: On success, the caller acquires ownership of both
 *       `out_ch0` and `out_ch1` with full default rights (`ZX_RIGHT_TRANSFER`,
 *       `ZX_RIGHT_READ`, `ZX_RIGHT_WRITE`, etc.).
 */
zx_status_t iris_channel_create(uint32_t flags,
                                zx_handle_t* out_ch0,
                                zx_handle_t* out_ch1);

/**
 * @brief Send an IPC message (header + optional payload) and capability handles over a channel.
 *
 * @param channel The channel handle on which to send the message.
 * @param hdr Pointer to the 24-byte message header (`iris_msg_hdr_t`). Must have magic `IRIS_IPC_MAGIC`.
 * @param payload Pointer to trailing payload bytes (may be NULL if `hdr->payload_len == 0`).
 * @param handles Optional array of capability handles to transfer with the message (may be NULL).
 * @param num_handles Number of handles in `handles` (up to IRIS_IPC_MAX_HANDLES).
 *
 * @return ZX_OK on success.
 * @return ZX_ERR_BAD_HANDLE if `channel` is invalid or lacks `ZX_RIGHT_WRITE`.
 * @return ZX_ERR_PEER_CLOSED if the remote channel endpoint has been closed.
 * @return ZX_ERR_INVALID_ARGS if `hdr` is invalid or payload size exceeds `IRIS_IPC_MAX_PAYLOAD`.
 *
 * @note Capability Lifecycle: On `ZX_OK`, all handles in `handles` are consumed and
 *       ownership is transferred to the channel peer. On error, caller retains ownership.
 */
zx_status_t iris_channel_send(zx_handle_t channel,
                              const iris_msg_hdr_t* hdr,
                              const void* payload,
                              const zx_handle_t* handles,
                              uint32_t num_handles);

/**
 * @brief Receive an IPC message (header + optional payload) and capability handles from a channel.
 *
 * @param channel The channel handle from which to read.
 * @param[out] out_hdr Pointer to header structure to receive message header.
 * @param[out] out_payload Pointer to caller buffer to receive payload (may be NULL if caller expects 0 payload).
 * @param max_payload_len Maximum capacity of `out_payload` in bytes.
 * @param[out] actual_payload_len Optional pointer to store the actual number of payload bytes received.
 * @param[out] out_handles Optional array to receive transferred capability handles.
 * @param max_handles Maximum capacity of `out_handles` array.
 * @param[out] actual_handles Optional pointer to store the actual count of handles received.
 *
 * @return ZX_OK on success.
 * @return ZX_ERR_SHOULD_WAIT if no message is currently available on the channel.
 * @return ZX_ERR_PEER_CLOSED if the remote channel endpoint has been closed.
 * @return ZX_ERR_BUFFER_TOO_SMALL if message payload exceeds `max_payload_len` or handles exceed `max_handles`.
 * @return ZX_ERR_INVALID_ARGS if received message header has invalid magic.
 *
 * @note Capability Lifecycle: On `ZX_OK`, caller acquires ownership of all handles
 *       populated in `out_handles` and must close them when done.
 */
zx_status_t iris_channel_recv(zx_handle_t channel,
                              iris_msg_hdr_t* out_hdr,
                              void* out_payload,
                              uint32_t max_payload_len,
                              uint32_t* actual_payload_len,
                              zx_handle_t* out_handles,
                              uint32_t max_handles,
                              uint32_t* actual_handles);

/**
 * @brief Perform a synchronous request/response transaction over a channel.
 *
 * Issues a request message, writes any associated capability handles, and blocks
 * until a matching reply with the same `txid` is received or `deadline` expires.
 *
 * @param channel The channel endpoint on which to conduct the transaction.
 * @param req_hdr Outgoing request header (must have magic `IRIS_IPC_MAGIC` and unique `txid`).
 * @param req_payload Outgoing request payload bytes (may be NULL if 0 bytes).
 * @param req_handles Array of capability handles to transfer in the request (may be NULL).
 * @param num_req_handles Number of handles in `req_handles`.
 * @param[out] resp_hdr Pointer to buffer to receive the server's reply header.
 * @param[out] resp_payload Pointer to caller buffer to receive reply payload data.
 * @param max_resp_payload_len Capacity of `resp_payload` in bytes.
 * @param[out] actual_resp_payload_len Optional pointer to receive actual reply payload length.
 * @param[out] resp_handles Optional array to receive transferred capability handles from server.
 * @param max_resp_handles Capacity of `resp_handles` array.
 * @param[out] actual_resp_handles Optional pointer to receive actual count of reply handles.
 * @param deadline Absolute deadline timestamp in nanoseconds (`ZX_TIME_INFINITE` to wait indefinitely).
 *
 * @return ZX_OK on success.
 * @return ZX_ERR_TIMED_OUT if `deadline` passed before reply arrived.
 * @return ZX_ERR_PEER_CLOSED if server channel closed before or during call.
 * @return ZX_ERR_BUFFER_TOO_SMALL if response exceeds provided buffer capacity.
 *
 * @note Capability Lifecycle: On success, `req_handles` are consumed and `resp_handles`
 *       are acquired by the caller.
 */
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
                              zx_time_t deadline);

/**
 * @brief Convenience helper: send a header-only message (0-byte payload, 0 handles).
 *
 * Ideal for ping/pong, heartbeats, and simple command triggers.
 *
 * @param channel The channel handle.
 * @param protocol Target protocol ID (IRIS_PROTO_*).
 * @param opcode Command opcode.
 * @param txid Transaction ID.
 *
 * @return ZX_OK on success, or standard Zircon error code.
 */
zx_status_t iris_channel_send_simple(zx_handle_t channel,
                                     uint32_t protocol,
                                     uint32_t opcode,
                                     uint32_t txid);

/**
 * @brief Convenience helper: send a reply message matching an incoming request.
 *
 * Automatically populates reply magic, protocol, opcode, and matching `txid`.
 *
 * @param channel The channel handle.
 * @param req_hdr Pointer to the request header being answered.
 * @param status Status code to return to the caller (e.g. ZX_OK or ZX_ERR_*).
 * @param payload Optional reply payload bytes.
 * @param payload_len Byte length of reply payload.
 * @param handles Optional array of handles to transfer back to the client.
 * @param num_handles Count of handles in `handles`.
 *
 * @return ZX_OK on success, or standard Zircon error code.
 */
zx_status_t iris_channel_reply(zx_handle_t channel,
                               const iris_msg_hdr_t* req_hdr,
                               int32_t status,
                               const void* payload,
                               uint32_t payload_len,
                               const zx_handle_t* handles,
                               uint32_t num_handles);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_CHANNEL_H_ */
