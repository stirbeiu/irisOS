/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/drivers/virtio_gpu/include/virtio_gpu.h
 * Purpose: Standalone Ring 3 VirtIO GPU 2D Driver Specification & Wire Structures.
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 driver header.
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

#ifndef SERVERS_DRIVERS_VIRTIO_GPU_H_
#define SERVERS_DRIVERS_VIRTIO_GPU_H_

/**
 * @file virtio_gpu.h
 * @brief VirtIO GPU 2D Protocol Structures & Constants (VirtIO Spec Section 5.7).
 *
 * ARCHITECTURAL DESIGN & HARDWARE HANDSHAKE:
 *  The VirtIO GPU device exposes two virtqueues:
 *    - Queue 0 (`controlq` / `ctrlq`): 2D/3D command and response exchange.
 *    - Queue 1 (`cursorq`): Hardware cursor updates.
 *
 *  For unaccelerated 2D scanout initialization in Ring 3, `bin/virtio_gpu`
 *  executes the following command sequence over `ctrlq`:
 *    1. `VIRTIO_GPU_CMD_GET_DISPLAY_INFO`: Queries active scanouts and preferred
 *       display geometry (defaulting to 1024x768 if unconfigured).
 *    2. `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D`: Allocates a 2D host resource (ID 1)
 *       with pixel format `VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM` (32-bit BGRX/BGRA).
 *    3. `VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING`: Binds the BTI-pinned guest
 *       framebuffer physical address (`zx_bti_pin`) as the backing store for
 *       Resource 1.
 *    4. `VIRTIO_GPU_CMD_SET_SCANOUT`: Connects Scanout 0 to Resource 1.
 *    5. `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D`: Copies damaged guest backing RAM
 *       rectangles into the host resource.
 *    6. `VIRTIO_GPU_CMD_RESOURCE_FLUSH`: Flushes the updated host resource to
 *       the active display scanout.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================================================
 * VirtIO GPU Control Command & Response Types (VirtIO 1.1 Section 5.7.6)
 * ============================================================================== */

/* 2D Commands */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101u
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102u
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103u
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105u
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106u
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107u
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO          0x0108u
#define VIRTIO_GPU_CMD_GET_CAPSET               0x0109u
#define VIRTIO_GPU_CMD_GET_EDID                 0x010Au

/* Cursor Commands */
#define VIRTIO_GPU_CMD_UPDATE_CURSOR            0x0300u
#define VIRTIO_GPU_CMD_MOVE_CURSOR              0x0301u

/* Success Responses */
#define VIRTIO_GPU_RESP_OK_NODATA               0x1100u
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101u
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO          0x1102u
#define VIRTIO_GPU_RESP_OK_CAPSET               0x1103u
#define VIRTIO_GPU_RESP_OK_EDID                 0x1104u

/* Error Responses */
#define VIRTIO_GPU_RESP_ERR_UNSPEC              0x1200u
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY       0x1201u
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID  0x1202u
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203u
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID  0x1204u
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER   0x1205u

/* ==============================================================================
 * VirtIO GPU Pixel Formats & Constants
 * ============================================================================== */

#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM        1u
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM        2u
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM        3u
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM        4u
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM        67u
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM        68u
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM        121u
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM        134u

#define VIRTIO_GPU_FLAG_FENCE                   (1u << 0)
#define VIRTIO_GPU_MAX_SCANOUTS                 16u

/* Default fallback display dimensions and background color */
#define VIRTIO_GPU_DEFAULT_WIDTH                1024u
#define VIRTIO_GPU_DEFAULT_HEIGHT               768u
#define VIRTIO_GPU_RESOURCE_ID_FB               1u
#define IRIS_DESKTOP_BG_COLOR                   0xFF1E1E2Eu /* Dark Slate / Iris Blue */

/* ==============================================================================
 * VirtIO GPU Packed Wire Structures
 * ============================================================================== */

#pragma pack(push, 1)

/**
 * @brief Standard 24-byte VirtIO GPU Control Header prepended to all commands and responses.
 */
typedef struct virtio_gpu_ctrl_hdr {
    uint32_t type;       /**< VIRTIO_GPU_CMD_* or VIRTIO_GPU_RESP_* */
    uint32_t flags;      /**< VIRTIO_GPU_FLAG_FENCE or 0 */
    uint64_t fence_id;   /**< Fence ID when VIRTIO_GPU_FLAG_FENCE is set */
    uint32_t ctx_id;     /**< Rendering context ID (0 for 2D mode) */
    uint32_t padding;    /**< Reserved padding (0) */
} virtio_gpu_ctrl_hdr_t;

_Static_assert(sizeof(virtio_gpu_ctrl_hdr_t) == 24, "virtio_gpu_ctrl_hdr_t must be 24 bytes");

/**
 * @brief 2D rectangle specification in pixels.
 */
typedef struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} virtio_gpu_rect_t;

/**
 * @brief Per-scanout display mode returned in VIRTIO_GPU_RESP_OK_DISPLAY_INFO.
 */
typedef struct virtio_gpu_display_one {
    virtio_gpu_rect_t r;       /**< Preferred scanout dimensions */
    uint32_t          enabled; /**< 1 if scanout is connected/enabled */
    uint32_t          flags;   /**< Scanout flags */
} virtio_gpu_display_one_t;

/**
 * @brief Response payload for VIRTIO_GPU_CMD_GET_DISPLAY_INFO.
 */
typedef struct virtio_gpu_resp_display_info {
    virtio_gpu_ctrl_hdr_t    hdr;
    virtio_gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} virtio_gpu_resp_display_info_t;

/**
 * @brief Command payload for VIRTIO_GPU_CMD_RESOURCE_CREATE_2D.
 */
typedef struct virtio_gpu_resource_create_2d {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t              resource_id; /**< Unique non-zero resource identifier */
    uint32_t              format;      /**< VIRTIO_GPU_FORMAT_* */
    uint32_t              width;       /**< Width in pixels */
    uint32_t              height;      /**< Height in pixels */
} virtio_gpu_resource_create_2d_t;

/**
 * @brief Command payload for VIRTIO_GPU_CMD_RESOURCE_UNREF.
 */
typedef struct virtio_gpu_resource_unref {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t              resource_id;
    uint32_t              padding;
} virtio_gpu_resource_unref_t;

/**
 * @brief Scatter-gather physical memory entry for VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING.
 */
typedef struct virtio_gpu_mem_entry {
    uint64_t addr;    /**< Guest physical DMA address */
    uint32_t length;  /**< Byte length of contiguous region */
    uint32_t padding; /**< Reserved (0) */
} virtio_gpu_mem_entry_t;

/**
 * @brief Command payload for VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING.
 */
typedef struct virtio_gpu_resource_attach_backing {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t              resource_id; /**< Target resource ID */
    uint32_t              nr_entries;  /**< Number of following virtio_gpu_mem_entry_t items */
} virtio_gpu_resource_attach_backing_t;

/**
 * @brief Combined single-entry attach backing request for contiguous BTI VMO.
 */
typedef struct virtio_gpu_attach_backing_single {
    virtio_gpu_resource_attach_backing_t req;
    virtio_gpu_mem_entry_t               entry;
} virtio_gpu_attach_backing_single_t;

/**
 * @brief Command payload for VIRTIO_GPU_CMD_SET_SCANOUT.
 */
typedef struct virtio_gpu_set_scanout {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t     r;           /**< Scanout rectangle within resource */
    uint32_t              scanout_id;  /**< Scanout index (0) */
    uint32_t              resource_id; /**< Resource ID (or 0 to disable scanout) */
} virtio_gpu_set_scanout_t;

/**
 * @brief Command payload for VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D.
 */
typedef struct virtio_gpu_transfer_to_host_2d {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t     r;           /**< Rectangle to transfer */
    uint64_t              offset;      /**< Byte offset in backing store */
    uint32_t              resource_id; /**< Target resource ID */
    uint32_t              padding;
} virtio_gpu_transfer_to_host_2d_t;

/**
 * @brief Command payload for VIRTIO_GPU_CMD_RESOURCE_FLUSH.
 */
typedef struct virtio_gpu_resource_flush {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t     r;           /**< Rectangle to flush to display */
    uint32_t              resource_id; /**< Target resource ID */
    uint32_t              padding;
} virtio_gpu_resource_flush_t;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* SERVERS_DRIVERS_VIRTIO_GPU_H_ */
