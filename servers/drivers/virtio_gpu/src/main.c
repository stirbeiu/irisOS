/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/drivers/virtio_gpu/src/main.c
 * Purpose: Standalone Ring 3 User-Space VirtIO GPU 2D Driver Daemon (bin/virtio_gpu).
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 system driver.
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

/**
 * @file main.c
 * @brief Standalone Ring 3 VirtIO GPU 2D Driver Daemon (bin/virtio_gpu).
 *
 * ARCHITECTURAL LIFECYCLE & HARDWARE INTERACTIONS:
 *  1. Process Startup & Capability Extraction:
 *     - Spawned by `iris_init` as an isolated user-space Ring 3 daemon.
 *     - Extracts the Root Resource handle (`PA_HND(PA_RESOURCE, 0)`), the
 *       driver IPC server channel (`PA_HND(PA_USER0, 0)`), and any pre-derived
 *       BTI (`PA_HND(PA_USER1, 0)`) or PCIe MMIO VMO handles passed by `iris_init`.
 *  2. PCIe (ECAM) Device Discovery:
 *     - Maps physical ECAM configuration space (`0xE0000000` on x86_64 q35;
 *       `0x4010000000` on aarch64 virt) via `zx_vmo_create_physical()`.
 *     - Scans PCIe buses for Vendor ID `0x1AF4` and Device ID `0x1050` (VirtIO 1.0 GPU)
 *       or `0x1014` (Transitional GPU).
 *     - Parses VirtIO 1.0 PCI capabilities (`COMMON_CFG`, `NOTIFY_CFG`, `ISR_CFG`,
 *       `DEVICE_CFG`) and enables Bus Master DMA + Memory Decoding.
 *  3. VirtIO 1.0 Feature Negotiation & Control Queue (`ctrlq`) Setup:
 *     - Resets device (`device_status = 0`), sets `ACKNOWLEDGE | DRIVER`.
 *     - Negotiates `VIRTIO_F_VERSION_1` (Bit 32) and verifies `FEATURES_OK`.
 *     - Allocates DMA-safe physically contiguous memory for Virtqueue 0 (`ctrlq`)
 *       and command/response buffers via Zircon BTI (`zx_bti_pin`).
 *  4. 2D Display Initialization Handshake:
 *     - `VIRTIO_GPU_CMD_GET_DISPLAY_INFO`: Queries Scanout 0 preferred resolution
 *       (falling back to 1024x768 if unconfigured).
 *     - Allocates and BTI-pins a physically contiguous framebuffer VMO (`width * height * 4`).
 *     - `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D`: Creates Resource 1 (`B8G8R8X8_UNORM`).
 *     - `VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING`: Attaches the pinned physical
 *       framebuffer address to Resource 1.
 *     - `VIRTIO_GPU_CMD_SET_SCANOUT`: Binds Scanout 0 to Resource 1.
 *     - Fills the framebuffer with the solid irisOS desktop background color
 *       (`0xFF1E1E2E`), then executes `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D` and
 *       `VIRTIO_GPU_CMD_RESOURCE_FLUSH`.
 *  5. IPC Server Loop (`IRIS_PROTO_DRIVER`):
 *     - `IRIS_GPU_OP_GET_INFO`: Returns `iris_gpu_info_t` (width, height, pitch,
 *       format, fb_size) along with a duplicated handle to the shared framebuffer VMO.
 *     - `IRIS_GPU_OP_FLUSH`: Executes `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH`
 *       for the requested damage rectangle.
 *  6. Clean DMA Teardown:
 *     - Resets the VirtIO GPU device and explicitly unpins all Pinned Memory Tokens
 *       (`g_vq_pmt`, `g_cmd_pmt`, `g_fb_pmt`) via `zx_pmt_unpin()` prior to exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <zircon/types.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/iommu.h>

#include <iris/ipc.h>
#include <iris/init.h>
#include <iris/pci.h>
#include <iris/log.h>
#include <servers/drivers/pci/pci.h>
#include <servers/drivers/virtio_gpu/include/virtio_gpu.h>

#define PAGE_SIZE 4096ULL
#define PAGE_MASK (~(PAGE_SIZE - 1ULL))

#ifndef ZX_SEC
#define ZX_SEC(n) ((zx_duration_t)(n) * 1000000000ULL)
#endif

static inline uint64_t align_page(uint64_t val) {
    return (val + PAGE_SIZE - 1ULL) & PAGE_MASK;
}

/* Global driver state */
static zx_handle_t g_root_resource  = ZX_HANDLE_INVALID;
static zx_handle_t g_server_channel = ZX_HANDLE_INVALID;

static virtio_pci_dev_t g_pci_dev;
static volatile virtio_pci_common_cfg_t* g_common_cfg = NULL;
static uintptr_t g_doorbell_vaddr = 0;

static uint16_t g_queue_size    = 0;
static uint16_t g_last_used_idx = 0;

/* BTI & Pinned Memory Tokens (PMTs) for clean teardown */
static zx_handle_t g_bti     = ZX_HANDLE_INVALID;
static zx_handle_t g_vq_pmt  = ZX_HANDLE_INVALID;
static zx_handle_t g_cmd_pmt = ZX_HANDLE_INVALID;
static zx_handle_t g_fb_pmt  = ZX_HANDLE_INVALID;

/* Virtqueue 0 (ctrlq) memory mappings */
static zx_vaddr_t g_vq_desc_vaddr  = 0;
static zx_vaddr_t g_vq_avail_vaddr = 0;
static zx_vaddr_t g_vq_used_vaddr  = 0;

/* Command & Response DMA Scratch Buffer (8 KB: 0..4095 = req, 4096..8191 = resp) */
static zx_handle_t g_cmd_vmo   = ZX_HANDLE_INVALID;
static zx_paddr_t  g_cmd_paddr = 0;
static zx_vaddr_t  g_cmd_vaddr = 0;

/* Shared 2D Framebuffer VMO & DMA Mappings */
static zx_handle_t g_fb_vmo    = ZX_HANDLE_INVALID;
static zx_paddr_t  g_fb_paddr  = 0;
static zx_vaddr_t  g_fb_vaddr  = 0;
static uint64_t    g_fb_size   = 0;

/* Active Scanout Geometry */
static uint32_t g_display_width  = VIRTIO_GPU_DEFAULT_WIDTH;
static uint32_t g_display_height = VIRTIO_GPU_DEFAULT_HEIGHT;
static uint32_t g_display_pitch  = VIRTIO_GPU_DEFAULT_WIDTH * 4u;
static uint32_t g_display_format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;

/**
 * @brief Submit a synchronous command on Virtqueue 0 (`ctrlq`) and await response.
 *
 * Uses a 2-descriptor chain in `ctrlq`:
 *   - Descriptor 0 (device-readable): Command payload at `g_cmd_paddr + 0`.
 *   - Descriptor 1 (device-writable): Response buffer at `g_cmd_paddr + 4096`.
 *
 * @param req_ptr Pointer to command structure.
 * @param req_len Size of command structure in bytes.
 * @param resp_ptr Optional pointer to copy response structure into.
 * @param resp_len Size of response structure in bytes.
 * @param expected_resp_type Expected `virtio_gpu_ctrl_hdr_t::type` (e.g. `VIRTIO_GPU_RESP_OK_NODATA`).
 * @return ZX_OK on success, or negative error code.
 */
static zx_status_t virtio_gpu_exec_cmd(const void* req_ptr, size_t req_len,
                                       void* resp_ptr, size_t resp_len,
                                       uint32_t expected_resp_type) {
    if (!req_ptr || req_len == 0 || req_len > 4096u || resp_len > 4096u) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (!g_cmd_vaddr || !g_vq_desc_vaddr || !g_doorbell_vaddr) {
        return ZX_ERR_BAD_STATE;
    }

    uint8_t* dma_req  = (uint8_t*)g_cmd_vaddr;
    uint8_t* dma_resp = (uint8_t*)(g_cmd_vaddr + 4096u);

    memcpy(dma_req, req_ptr, req_len);
    size_t clr_len = (resp_len >= sizeof(virtio_gpu_ctrl_hdr_t)) ? resp_len : sizeof(virtio_gpu_ctrl_hdr_t);
    memset(dma_resp, 0, clr_len);

    volatile vring_desc_t*  desc  = (volatile vring_desc_t*)g_vq_desc_vaddr;
    volatile vring_avail_t* avail = (volatile vring_avail_t*)g_vq_avail_vaddr;
    volatile vring_used_t*  used  = (volatile vring_used_t*)g_vq_used_vaddr;

    /* Descriptor 0: Device-readable request */
    desc[0].addr  = g_cmd_paddr + 0;
    desc[0].len   = (uint32_t)req_len;
    desc[0].flags = VRING_DESC_F_NEXT;
    desc[0].next  = 1;

    /* Descriptor 1: Device-writable response */
    desc[1].addr  = g_cmd_paddr + 4096u;
    desc[1].len   = (uint32_t)clr_len;
    desc[1].flags = VRING_DESC_F_WRITE;
    desc[1].next  = 0;

    uint16_t avail_idx = avail->idx;
    avail->ring[avail_idx % g_queue_size] = 0;
    __sync_synchronize();
    avail->idx = avail_idx + 1;
    __sync_synchronize();

    /* Ring queue 0 doorbell */
    *(volatile uint16_t*)g_doorbell_vaddr = 0;
    __sync_synchronize();

    /* Poll Used Ring for completion with 3-second timeout */
    uint16_t last_used = g_last_used_idx;
    zx_time_t deadline = zx_deadline_after(ZX_SEC(3));
    while (used->idx == last_used) {
        __sync_synchronize();
        if (zx_clock_get_monotonic() > deadline) {
            log_status(STATUS_FAIL, "virtio_gpu", "Control queue command 0x%04x timed out.",
                       ((const virtio_gpu_ctrl_hdr_t*)req_ptr)->type);
            return ZX_ERR_TIMED_OUT;
        }
    }
    g_last_used_idx = used->idx;

    const virtio_gpu_ctrl_hdr_t* resp_hdr = (const virtio_gpu_ctrl_hdr_t*)dma_resp;
    if (resp_hdr->type != expected_resp_type) {
        log_status(STATUS_FAIL, "virtio_gpu", "Command 0x%04x failed with response 0x%04x (expected 0x%04x).",
                   ((const virtio_gpu_ctrl_hdr_t*)req_ptr)->type, resp_hdr->type, expected_resp_type);
        return ZX_ERR_IO;
    }

    if (resp_ptr && resp_len > 0) {
        memcpy(resp_ptr, dma_resp, resp_len);
    }

    return ZX_OK;
}

/**
 * @brief Transfer a damaged 2D rectangle from guest backing memory to the host resource
 *        and flush the scanout display.
 */
static zx_status_t virtio_gpu_flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (width == 0 || x + width > g_display_width) {
        x = 0;
        width = g_display_width;
    }
    if (height == 0 || y + height > g_display_height) {
        y = 0;
        height = g_display_height;
    }

    /* 1. VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D */
    virtio_gpu_transfer_to_host_2d_t xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer.r.x         = x;
    xfer.r.y         = y;
    xfer.r.width     = width;
    xfer.r.height    = height;
    xfer.offset      = ((uint64_t)y * g_display_pitch) + ((uint64_t)x * 4ULL);
    xfer.resource_id = VIRTIO_GPU_RESOURCE_ID_FB;

    zx_status_t st = virtio_gpu_exec_cmd(&xfer, sizeof(xfer), NULL, 0, VIRTIO_GPU_RESP_OK_NODATA);
    if (st != ZX_OK) {
        return st;
    }

    /* 2. VIRTIO_GPU_CMD_RESOURCE_FLUSH */
    virtio_gpu_resource_flush_t flush;
    memset(&flush, 0, sizeof(flush));
    flush.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r.x         = x;
    flush.r.y         = y;
    flush.r.width     = width;
    flush.r.height    = height;
    flush.resource_id = VIRTIO_GPU_RESOURCE_ID_FB;

    return virtio_gpu_exec_cmd(&flush, sizeof(flush), NULL, 0, VIRTIO_GPU_RESP_OK_NODATA);
}

/**
 * @brief Discover VirtIO GPU on PCIe ECAM, configure Virtqueue 0 (`ctrlq`),
 *        execute the 2D scanout initialization handshake, and clear the display.
 */
static zx_status_t init_virtio_gpu_device(void) {
    uint64_t ecam_phys = pci_get_default_ecam_base();
    if (ecam_phys == 0) {
        log_status(STATUS_FAIL, "virtio_gpu", "Unsupported architecture for ECAM.");
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_handle_t ecam_vmo = ZX_HANDLE_INVALID;
    zx_status_t status = zx_vmo_create_physical(g_root_resource, ecam_phys, PCI_ECAM_SIZE_16_BUSES, &ecam_vmo);
    if (status != ZX_OK) {
        log_status(STATUS_FAIL, "virtio_gpu", "Failed to create ECAM physical VMO (%d).", status);
        return status;
    }

    zx_vaddr_t ecam_vaddr = 0;
    status = zx_vmar_map(zx_vmar_root_self(),
                         ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_MAP_RANGE,
                         0, ecam_vmo, 0, PCI_ECAM_SIZE_16_BUSES, &ecam_vaddr);
    zx_handle_close(ecam_vmo);
    if (status != ZX_OK) {
        log_status(STATUS_FAIL, "virtio_gpu", "Failed to map ECAM window (%d).", status);
        return status;
    }

    if (!pci_find_virtio_device(ecam_vaddr, 16, VIRTIO_DEV_ID_GPU, &g_pci_dev)) {
        log_status(STATUS_FAIL, "virtio_gpu", "No VirtIO GPU device discovered on PCIe bus.");
        return ZX_ERR_NOT_FOUND;
    }

    log_status(STATUS_OKAY, "virtio_gpu", "Bound to PCI %02x:%02x.%x (Vendor: 0x%04x, Device: 0x%04x).",
               g_pci_dev.bus, g_pci_dev.dev, g_pci_dev.func, g_pci_dev.vendor_id, g_pci_dev.device_id);

    /* Map device BARs containing VirtIO capabilities */
    uint8_t bars_to_map[4] = {
        g_pci_dev.common_bar,
        g_pci_dev.notify_bar,
        g_pci_dev.isr_bar,
        g_pci_dev.device_bar
    };
    for (int i = 0; i < 4; i++) {
        uint8_t b = bars_to_map[i];
        if (b > 5) continue;
        if (g_pci_dev.bars[b].vaddr != 0) continue;

        uint64_t bar_phys = g_pci_dev.bars[b].phys_addr;
        if (bar_phys == 0) continue;

        uint64_t map_size = 65536u;
        zx_handle_t bar_vmo = ZX_HANDLE_INVALID;
        status = zx_vmo_create_physical(g_root_resource, bar_phys, map_size, &bar_vmo);
        if (status != ZX_OK) {
            log_status(STATUS_FAIL, "virtio_gpu", "zx_vmo_create_physical for BAR %u failed (%d).", b, status);
            return status;
        }

        zx_vaddr_t bar_vaddr = 0;
        status = zx_vmar_map(zx_vmar_root_self(),
                             ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_MAP_RANGE,
                             0, bar_vmo, 0, map_size, &bar_vaddr);
        zx_handle_close(bar_vmo);
        if (status != ZX_OK) {
            log_status(STATUS_FAIL, "virtio_gpu", "zx_vmar_map for BAR %u failed (%d).", b, status);
            return status;
        }

        g_pci_dev.bars[b].vaddr = bar_vaddr;
    }

    g_common_cfg = (volatile virtio_pci_common_cfg_t*)(
        g_pci_dev.bars[g_pci_dev.common_bar].vaddr + g_pci_dev.common_offset);

    /* 1. Reset device and set ACKNOWLEDGE | DRIVER */
    g_common_cfg->device_status = VIRTIO_STATUS_DEVICE_RESET;
    __sync_synchronize();

    g_common_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    __sync_synchronize();

    /* 2. Feature negotiation (require VIRTIO_F_VERSION_1) */
    g_common_cfg->device_feature_select = 1;
    uint32_t dev_feat_1 = g_common_cfg->device_feature;
    g_common_cfg->driver_feature_select = 1;
    g_common_cfg->driver_feature = (dev_feat_1 & 1u); /* VIRTIO_F_VERSION_1 */

    g_common_cfg->driver_feature_select = 0;
    g_common_cfg->driver_feature = 0;
    __sync_synchronize();

    g_common_cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
    __sync_synchronize();

    if (!(g_common_cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        log_status(STATUS_FAIL, "virtio_gpu", "VirtIO GPU rejected feature negotiation.");
        return ZX_ERR_NOT_SUPPORTED;
    }

    /* 3. Configure Virtqueue 0 (ctrlq) */
    g_common_cfg->queue_select = 0;
    g_queue_size = g_common_cfg->queue_size;
    if (g_queue_size == 0) {
        log_status(STATUS_FAIL, "virtio_gpu", "Control queue 0 unavailable on device.");
        return ZX_ERR_NOT_FOUND;
    }

    /* Ensure BTI handle is available (use startup handle if passed by iris_init, otherwise derive) */
    if (g_bti == ZX_HANDLE_INVALID) {
        zx_iommu_desc_dummy_t dummy_desc = {0};
        zx_handle_t iommu = ZX_HANDLE_INVALID;
        status = zx_iommu_create(g_root_resource, ZX_IOMMU_TYPE_DUMMY, &dummy_desc, sizeof(dummy_desc), &iommu);
        if (status != ZX_OK) {
            log_status(STATUS_FAIL, "virtio_gpu", "zx_iommu_create failed (%d).", status);
            return status;
        }
        status = zx_bti_create(iommu, 0, 0, &g_bti);
        zx_handle_close(iommu);
        if (status != ZX_OK) {
            log_status(STATUS_FAIL, "virtio_gpu", "zx_bti_create failed (%d).", status);
            return status;
        }
    }

    /* Allocate physically contiguous memory for Virtqueue 0 (16 KB) */
    size_t vq_bytes = 16384u;
    zx_handle_t vq_vmo = ZX_HANDLE_INVALID;
    status = zx_vmo_create_contiguous(g_bti, vq_bytes, 0, &vq_vmo);
    if (status != ZX_OK) {
        log_status(STATUS_FAIL, "virtio_gpu", "zx_vmo_create_contiguous for ctrlq failed (%d).", status);
        return status;
    }

    zx_paddr_t vq_paddr = 0;
    status = zx_bti_pin(g_bti, ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE | ZX_BTI_CONTIGUOUS,
                        vq_vmo, 0, vq_bytes, &vq_paddr, 1, &g_vq_pmt);
    if (status != ZX_OK) {
        zx_handle_close(vq_vmo);
        log_status(STATUS_FAIL, "virtio_gpu", "zx_bti_pin for ctrlq failed (%d).", status);
        return status;
    }

    zx_vaddr_t vq_vaddr = 0;
    status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vq_vmo, 0, vq_bytes, &vq_vaddr);
    zx_handle_close(vq_vmo);
    if (status != ZX_OK) {
        log_status(STATUS_FAIL, "virtio_gpu", "zx_vmar_map for ctrlq failed (%d).", status);
        return status;
    }
    memset((void*)vq_vaddr, 0, vq_bytes);

    size_t desc_size = g_queue_size * sizeof(vring_desc_t);
    size_t avail_size = sizeof(vring_avail_t) + (g_queue_size * sizeof(uint16_t));
    uint64_t used_offset = align_page(desc_size + avail_size);

    g_vq_desc_vaddr  = vq_vaddr;
    g_vq_avail_vaddr = vq_vaddr + desc_size;
    g_vq_used_vaddr  = vq_vaddr + used_offset;

    g_common_cfg->queue_desc   = vq_paddr;
    g_common_cfg->queue_driver = vq_paddr + desc_size;
    g_common_cfg->queue_device = vq_paddr + used_offset;
    __sync_synchronize();

    g_common_cfg->queue_enable = 1;
    __sync_synchronize();

    uint16_t notify_off = g_common_cfg->queue_notify_off;
    g_doorbell_vaddr = g_pci_dev.bars[g_pci_dev.notify_bar].vaddr +
                       g_pci_dev.notify_offset +
                       ((uintptr_t)notify_off * g_pci_dev.notify_mult);

    /* Allocate 8 KB DMA buffer for control queue command & response payloads */
    size_t cmd_bytes = 8192u;
    status = zx_vmo_create_contiguous(g_bti, cmd_bytes, 0, &g_cmd_vmo);
    if (status != ZX_OK) {
        log_status(STATUS_FAIL, "virtio_gpu", "zx_vmo_create_contiguous for cmd buffer failed (%d).", status);
        return status;
    }

    status = zx_bti_pin(g_bti, ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE | ZX_BTI_CONTIGUOUS,
                        g_cmd_vmo, 0, cmd_bytes, &g_cmd_paddr, 1, &g_cmd_pmt);
    if (status != ZX_OK) {
        log_status(STATUS_FAIL, "virtio_gpu", "zx_bti_pin for cmd buffer failed (%d).", status);
        return status;
    }

    status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, g_cmd_vmo, 0, cmd_bytes, &g_cmd_vaddr);
    if (status != ZX_OK) {
        log_status(STATUS_FAIL, "virtio_gpu", "zx_vmar_map for cmd buffer failed (%d).", status);
        return status;
    }
    memset((void*)g_cmd_vaddr, 0, cmd_bytes);

    /* 4. Set DRIVER_OK before issuing control queue commands */
    g_common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
    __sync_synchronize();

    /* 5. Handshake Step A: VIRTIO_GPU_CMD_GET_DISPLAY_INFO */
    virtio_gpu_ctrl_hdr_t get_info_req;
    memset(&get_info_req, 0, sizeof(get_info_req));
    get_info_req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    virtio_gpu_resp_display_info_t disp_info;
    memset(&disp_info, 0, sizeof(disp_info));
    status = virtio_gpu_exec_cmd(&get_info_req, sizeof(get_info_req),
                                 &disp_info, sizeof(disp_info),
                                 VIRTIO_GPU_RESP_OK_DISPLAY_INFO);
    if (status == ZX_OK && disp_info.pmodes[0].enabled &&
        disp_info.pmodes[0].r.width > 0 && disp_info.pmodes[0].r.height > 0) {
        g_display_width  = disp_info.pmodes[0].r.width;
        g_display_height = disp_info.pmodes[0].r.height;
    } else {
        g_display_width  = VIRTIO_GPU_DEFAULT_WIDTH;
        g_display_height = VIRTIO_GPU_DEFAULT_HEIGHT;
    }
    g_display_pitch  = g_display_width * 4u;
    g_display_format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;

    log_status(STATUS_OKAY, "virtio_gpu", "Display scanout 0 geometry: %ux%u (pitch %u, format B8G8R8X8).",
               g_display_width, g_display_height, g_display_pitch);

    /* 6. Allocate and BTI-pin physically contiguous Framebuffer VMO */
    g_fb_size = align_page((uint64_t)g_display_pitch * (uint64_t)g_display_height);
    status = zx_vmo_create_contiguous(g_bti, g_fb_size, 0, &g_fb_vmo);
    if (status != ZX_OK) {
        log_status(STATUS_FAIL, "virtio_gpu", "zx_vmo_create_contiguous for framebuffer (%llu bytes) failed (%d).",
                   (unsigned long long)g_fb_size, status);
        return status;
    }

    status = zx_bti_pin(g_bti, ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE | ZX_BTI_CONTIGUOUS,
                        g_fb_vmo, 0, g_fb_size, &g_fb_paddr, 1, &g_fb_pmt);
    if (status != ZX_OK) {
        log_status(STATUS_FAIL, "virtio_gpu", "zx_bti_pin for framebuffer failed (%d).", status);
        return status;
    }

    status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, g_fb_vmo, 0, g_fb_size, &g_fb_vaddr);
    if (status != ZX_OK) {
        log_status(STATUS_FAIL, "virtio_gpu", "zx_vmar_map for framebuffer failed (%d).", status);
        return status;
    }

    /* 7. Handshake Step B: VIRTIO_GPU_CMD_RESOURCE_CREATE_2D */
    virtio_gpu_resource_create_2d_t create_2d;
    memset(&create_2d, 0, sizeof(create_2d));
    create_2d.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create_2d.resource_id = VIRTIO_GPU_RESOURCE_ID_FB;
    create_2d.format      = g_display_format;
    create_2d.width       = g_display_width;
    create_2d.height      = g_display_height;

    status = virtio_gpu_exec_cmd(&create_2d, sizeof(create_2d), NULL, 0, VIRTIO_GPU_RESP_OK_NODATA);
    if (status != ZX_OK) {
        return status;
    }

    /* 8. Handshake Step C: VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING */
    virtio_gpu_attach_backing_single_t attach;
    memset(&attach, 0, sizeof(attach));
    attach.req.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.req.resource_id = VIRTIO_GPU_RESOURCE_ID_FB;
    attach.req.nr_entries  = 1;
    attach.entry.addr      = g_fb_paddr;
    attach.entry.length    = (uint32_t)((uint64_t)g_display_pitch * (uint64_t)g_display_height);

    status = virtio_gpu_exec_cmd(&attach, sizeof(attach), NULL, 0, VIRTIO_GPU_RESP_OK_NODATA);
    if (status != ZX_OK) {
        return status;
    }

    /* 9. Handshake Step D: VIRTIO_GPU_CMD_SET_SCANOUT */
    virtio_gpu_set_scanout_t scanout;
    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.r.x         = 0;
    scanout.r.y         = 0;
    scanout.r.width     = g_display_width;
    scanout.r.height    = g_display_height;
    scanout.scanout_id  = 0;
    scanout.resource_id = VIRTIO_GPU_RESOURCE_ID_FB;

    status = virtio_gpu_exec_cmd(&scanout, sizeof(scanout), NULL, 0, VIRTIO_GPU_RESP_OK_NODATA);
    if (status != ZX_OK) {
        return status;
    }

    /* 10. Clear framebuffer to solid desktop background color (0xFF1E1E2E),
     *     draw Visual Test Card (top 100-px RGBW color bars + centered 400x200 iris rectangle),
     *     and flush to scanout 0. */
    uint32_t* pixels = (uint32_t*)g_fb_vaddr;
    size_t total_pixels = (size_t)g_display_width * (size_t)g_display_height;
    for (size_t i = 0; i < total_pixels; i++) {
        pixels[i] = IRIS_DESKTOP_BG_COLOR;
    }

    /* Top 100 rows: 4 equal-width RGBW color bars (Red, Green, Blue, White) */
    uint32_t bar_h = (g_display_height >= 100u) ? 100u : g_display_height;
    uint32_t bar_w = g_display_width / 4u;
    const uint32_t bar_colors[4] = {
        0xFFFF0000u, /* Red   */
        0xFF00FF00u, /* Green */
        0xFF0000FFu, /* Blue  */
        0xFFFFFFFFu  /* White */
    };
    for (uint32_t y = 0; y < bar_h; y++) {
        uint32_t* row = pixels + ((size_t)y * g_display_width);
        for (uint32_t x = 0; x < g_display_width; x++) {
            uint32_t idx = (bar_w > 0) ? (x / bar_w) : 0;
            if (idx > 3u) idx = 3u;
            row[x] = bar_colors[idx];
        }
    }

    /* Centered 400x200 bright cyan/iris rectangle (0xFF89B4FA) */
    uint32_t rect_w = (g_display_width >= 400u) ? 400u : g_display_width;
    uint32_t rect_h = (g_display_height >= 200u) ? 200u : g_display_height;
    uint32_t start_x = (g_display_width - rect_w) / 2u;
    uint32_t start_y = (g_display_height - rect_h) / 2u;
    for (uint32_t y = start_y; y < start_y + rect_h; y++) {
        uint32_t* row = pixels + ((size_t)y * g_display_width);
        for (uint32_t x = start_x; x < start_x + rect_w; x++) {
            row[x] = 0xFF89B4FAu;
        }
    }
    __sync_synchronize();

    status = virtio_gpu_flush_rect(0, 0, g_display_width, g_display_height);
    if (status != ZX_OK) {
        return status;
    }

    log_status(STATUS_OKAY, "virtio_gpu", "Initialized 2D scanout resource 1, rendered visual test card, and flushed display.");
    return ZX_OK;
}

/**
 * @brief Reset VirtIO GPU hardware and explicitly unpin BTI Pinned Memory Tokens (PMTs).
 */
static void cleanup_virtio_gpu_device(void) {
    if (g_common_cfg != NULL) {
        g_common_cfg->device_status = VIRTIO_STATUS_DEVICE_RESET;
        __sync_synchronize();
    }

    if (g_fb_pmt != ZX_HANDLE_INVALID) {
        zx_pmt_unpin(g_fb_pmt);
        g_fb_pmt = ZX_HANDLE_INVALID;
    }

    if (g_cmd_pmt != ZX_HANDLE_INVALID) {
        zx_pmt_unpin(g_cmd_pmt);
        g_cmd_pmt = ZX_HANDLE_INVALID;
    }

    if (g_vq_pmt != ZX_HANDLE_INVALID) {
        zx_pmt_unpin(g_vq_pmt);
        g_vq_pmt = ZX_HANDLE_INVALID;
    }

    if (g_fb_vmo != ZX_HANDLE_INVALID) {
        zx_handle_close(g_fb_vmo);
        g_fb_vmo = ZX_HANDLE_INVALID;
    }

    if (g_cmd_vmo != ZX_HANDLE_INVALID) {
        zx_handle_close(g_cmd_vmo);
        g_cmd_vmo = ZX_HANDLE_INVALID;
    }

    if (g_bti != ZX_HANDLE_INVALID) {
        zx_handle_close(g_bti);
        g_bti = ZX_HANDLE_INVALID;
    }
}

/**
 * @brief Primary IPC event loop serving GPU display info and flush requests
 *        over `IRIS_PROTO_DRIVER`.
 */
#define MAX_GPU_CLIENTS 8

static void run_ipc_server(void) {
    log_status(STATUS_OKAY, "virtio_gpu", "IPC server listening on control channel.");

    zx_handle_t clients[MAX_GPU_CLIENTS];
    for (int i = 0; i < MAX_GPU_CLIENTS; ++i) {
        clients[i] = ZX_HANDLE_INVALID;
    }
    clients[0] = g_server_channel;
    g_server_channel = ZX_HANDLE_INVALID;
    int active_clients = 1;

    uint8_t req_buf[512] __attribute__((aligned(8)));
    uint8_t rep_buf[512] __attribute__((aligned(8)));
    zx_handle_t handles[IRIS_IPC_MAX_HANDLES];
    uint32_t act_bytes = 0;
    uint32_t act_handles = 0;

    while (active_clients > 0) {
        bool did_work = false;

        for (int c = 0; c < MAX_GPU_CLIENTS; ++c) {
            zx_handle_t ch = clients[c];
            if (ch == ZX_HANDLE_INVALID) {
                continue;
            }

            act_bytes = 0;
            act_handles = 0;
            zx_status_t status = zx_channel_read(
                ch,
                0,
                req_buf,
                handles,
                sizeof(req_buf),
                IRIS_IPC_MAX_HANDLES,
                &act_bytes,
                &act_handles
            );

            if (status == ZX_ERR_SHOULD_WAIT) {
                continue;
            }

            if (status != ZX_OK) {
                if (status == ZX_ERR_PEER_CLOSED) {
                    zx_handle_close(ch);
                    clients[c] = ZX_HANDLE_INVALID;
                    active_clients--;
                }
                continue;
            }

            did_work = true;

            /* Register any newly transferred client channel endpoints */
            for (uint32_t h = 0; h < act_handles; ++h) {
                if (handles[h] != ZX_HANDLE_INVALID) {
                    bool added = false;
                    for (int slot = 0; slot < MAX_GPU_CLIENTS; ++slot) {
                        if (clients[slot] == ZX_HANDLE_INVALID) {
                            clients[slot] = handles[h];
                            active_clients++;
                            added = true;
                            break;
                        }
                    }
                    if (!added) {
                        zx_handle_close(handles[h]);
                    }
                }
            }

            if (act_bytes < sizeof(iris_msg_hdr_t)) {
                continue;
            }

            const iris_msg_hdr_t* req_hdr = (const iris_msg_hdr_t*)req_buf;
            if (!iris_msg_hdr_is_valid(req_hdr, IRIS_PROTO_DRIVER)) {
                continue;
            }

            iris_msg_hdr_t* rep_hdr = (iris_msg_hdr_t*)rep_buf;
            uint32_t rep_total_len = sizeof(iris_msg_hdr_t);
            zx_handle_t rep_handle = ZX_HANDLE_INVALID;
            uint32_t num_rep_handles = 0;

            switch (req_hdr->opcode) {
                case IRIS_GPU_OP_GET_INFO: {
                    iris_gpu_info_t* info = (iris_gpu_info_t*)(rep_buf + sizeof(iris_msg_hdr_t));
                    info->width   = g_display_width;
                    info->height  = g_display_height;
                    info->pitch   = g_display_pitch;
                    info->format  = g_display_format;
                    info->fb_size = g_fb_size;

                    zx_status_t dup_st = zx_handle_duplicate(g_fb_vmo, ZX_RIGHT_SAME_RIGHTS, &rep_handle);
                    if (dup_st == ZX_OK && rep_handle != ZX_HANDLE_INVALID) {
                        num_rep_handles = 1;
                    }

                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, dup_st, sizeof(iris_gpu_info_t));
                    rep_total_len = sizeof(iris_msg_hdr_t) + sizeof(iris_gpu_info_t);
                    break;
                }

                case IRIS_GPU_OP_FLUSH: {
                    uint32_t rx = 0, ry = 0, rw = g_display_width, rh = g_display_height;
                    if (req_hdr->payload_len >= sizeof(iris_gpu_flush_req_t)) {
                        const iris_gpu_flush_req_t* freq = (const iris_gpu_flush_req_t*)(req_buf + sizeof(iris_msg_hdr_t));
                        rx = freq->x;
                        ry = freq->y;
                        rw = freq->width;
                        rh = freq->height;
                    }
                    zx_status_t fl_st = virtio_gpu_flush_rect(rx, ry, rw, rh);
                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, fl_st, 0);
                    break;
                }

                default:
                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_NOT_SUPPORTED, 0);
                    break;
            }

            zx_channel_write(ch, 0, rep_buf, rep_total_len,
                             num_rep_handles > 0 ? &rep_handle : NULL, num_rep_handles);
        }

        if (!did_work && active_clients > 0) {
            if (active_clients == 1) {
                for (int c = 0; c < MAX_GPU_CLIENTS; ++c) {
                    if (clients[c] != ZX_HANDLE_INVALID) {
                        zx_signals_t observed = 0;
                        zx_status_t st = zx_object_wait_one(
                            clients[c],
                            ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                            ZX_TIME_INFINITE,
                            &observed
                        );
                        if (st != ZX_OK || ((observed & ZX_CHANNEL_PEER_CLOSED) && !(observed & ZX_CHANNEL_READABLE))) {
                            zx_handle_close(clients[c]);
                            clients[c] = ZX_HANDLE_INVALID;
                            active_clients--;
                        }
                        break;
                    }
                }
            } else {
                zx_nanosleep(zx_deadline_after(100000ULL));
            }
        }
    }
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    /* Extract Root Resource capability handle */
    g_root_resource = iris_root_resource();
    if (g_root_resource == ZX_HANDLE_INVALID) {
        g_root_resource = zx_take_startup_handle(PA_HND(PA_RESOURCE, 0));
    }
    if (g_root_resource == ZX_HANDLE_INVALID) {
        log_status(STATUS_FAIL, "virtio_gpu", "Missing Root Resource capability handle.");
        return 1;
    }

    /* Extract Driver Server channel endpoint */
    g_server_channel = zx_take_startup_handle(PA_HND(PA_USER0, 0));
    if (g_server_channel == ZX_HANDLE_INVALID) {
        log_status(STATUS_FAIL, "virtio_gpu", "Missing Driver IPC Server channel handle (PA_USER0).");
        return 1;
    }

    /* Extract optional pre-created BTI handle (PA_USER1:0) and MMIO/IRQ handles passed by iris_init */
    zx_handle_t startup_bti = zx_take_startup_handle(PA_HND(PA_USER1, 0));
    if (startup_bti != ZX_HANDLE_INVALID) {
        g_bti = startup_bti;
    }
    zx_handle_t startup_mmio = zx_take_startup_handle(PA_HND(PA_USER2, 0));
    if (startup_mmio != ZX_HANDLE_INVALID) {
        zx_handle_close(startup_mmio);
    }
    zx_handle_t startup_irq = zx_take_startup_handle(PA_HND(PA_USER2, 1));
    if (startup_irq != ZX_HANDLE_INVALID) {
        zx_handle_close(startup_irq);
    }

    /* Initialize VirtIO GPU hardware and 2D scanout */
    zx_status_t status = init_virtio_gpu_device();
    if (status != ZX_OK) {
        log_status(STATUS_FAIL, "virtio_gpu", "Failed to initialize VirtIO GPU device (%d).", status);
        cleanup_virtio_gpu_device();
        return 1;
    }

    /* Run IPC server loop */
    run_ipc_server();

    /* Unpin DMA memory tokens and release BTI resources cleanly */
    cleanup_virtio_gpu_device();
    if (g_server_channel != ZX_HANDLE_INVALID) {
        zx_handle_close(g_server_channel);
        g_server_channel = ZX_HANDLE_INVALID;
    }
    log_status(STATUS_OKAY, "virtio_gpu", "Unpinned BTI DMA tokens (zx_pmt_unpin) and exited cleanly.");
    return 0;
}
