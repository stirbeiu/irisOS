/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/drivers/virtio_blk/src/main.c
 * Purpose: Standalone Ring 3 User-Space VirtIO Block Device Driver Daemon.
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
 * @brief Standalone Ring 3 VirtIO Block Driver Daemon (bin/virtio_blk).
 *
 * ARCHITECTURAL LIFECYCLE & HARDWARE INTERACTIONS:
 *  1. Process Startup & Capability Extraction:
 *     - Started by iris_init or devmgr as an isolated user-space Ring 3 process.
 *     - Extracts the Root Resource handle (`PA_HND(PA_RESOURCE, 0)`) via
 *       `iris_root_resource()` to map PCIe configuration space and device BARs.
 *     - Extracts the driver IPC server channel endpoint (`PA_HND(PA_USER0, 0)`).
 *  2. PCIe (ECAM) Device Discovery:
 *     - Maps physical ECAM configuration space via `zx_vmo_create_physical()` and
 *       `zx_vmar_map()`.
 *     - Scans PCIe buses for Vendor ID 0x1AF4 (Red Hat / VirtIO) and Device ID
 *       0x1042 (VirtIO 1.0 Block) or 0x1001 (Transitional Block).
 *     - Parses standard VirtIO 1.0+ PCI capabilities (Common CFG, Notify, ISR, Device-specific).
 *  3. VirtIO 1.0 Feature Negotiation & Queue Configuration:
 *     - Maps device BARs into virtual memory.
 *     - Enables Bus Master DMA and Memory Decoding in PCI Command register.
 *     - Resets device (`device_status = 0`), sets `ACKNOWLEDGE | DRIVER`.
 *     - Negotiates `VIRTIO_F_VERSION_1` (Bit 32) and confirms `FEATURES_OK`.
 *     - Allocates DMA-safe physically contiguous memory for Virtqueue 0 using
 *       Zircon Bus Transaction Initiator (BTI) (`zx_iommu_create`, `zx_bti_create`,
 *       `zx_vmo_create_contiguous`, `zx_bti_pin`).
 *     - Sets 64-bit physical addresses for Descriptor Table, Available Ring, and Used Ring.
 *     - Sets `queue_enable = 1` and `device_status |= DRIVER_OK`.
 *  4. IPC Command Processing (IRIS_PROTO_DRIVER):
 *     - Listens on `server_ch` using standard 24-byte `iris_msg_hdr_t` wire protocol.
 *     - IRIS_BLK_OP_GET_INFO: returns total sector count and sector size (512 bytes).
 *     - IRIS_BLK_OP_READ: reads N sectors into reply payload via virtqueue DMA.
 *     - IRIS_BLK_OP_WRITE: writes N sectors from request payload to disk via virtqueue DMA.
 *  5. Clean DMA Teardown:
 *     - Resets the VirtIO device to halt DMA engines before releasing memory.
 *     - Explicitly unpins both Pinned Memory Tokens (`g_vq_pmt` and `g_dma_pmt`) via
 *       `zx_pmt_unpin()` prior to exit so the microkernel BTI reports zero leaked pages.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include <zircon/types.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/iommu.h>

#include <iris/ipc.h>
#include <iris/init.h>
#include <iris/pci.h>
#include <servers/drivers/pci/pci.h>

#define PAGE_SIZE 4096ULL
#define PAGE_MASK (~(PAGE_SIZE - 1ULL))

#ifndef ZX_SEC
#define ZX_SEC(n) ((zx_duration_t)(n) * 1000000000ULL)
#endif

/* Global driver state */
static zx_handle_t g_root_resource = ZX_HANDLE_INVALID;
static zx_handle_t g_server_channel = ZX_HANDLE_INVALID;

static virtio_pci_dev_t g_pci_dev;
static volatile virtio_pci_common_cfg_t* g_common_cfg = NULL;
static volatile void* g_device_cfg = NULL;
static uintptr_t g_doorbell_vaddr = 0;

static uint16_t g_queue_size = 0;
static uint16_t g_last_used_idx = 0;

/* BTI & Pinned Memory Token (PMT) handles for clean teardown */
static zx_handle_t g_bti     = ZX_HANDLE_INVALID;
static zx_handle_t g_vq_pmt  = ZX_HANDLE_INVALID;
static zx_handle_t g_dma_pmt = ZX_HANDLE_INVALID;

/* Virtqueue memory mappings */
static zx_vaddr_t g_vq_desc_vaddr = 0;
static zx_vaddr_t g_vq_avail_vaddr = 0;
static zx_vaddr_t g_vq_used_vaddr = 0;

/* DMA buffer mappings (for request headers, data, and status byte) */
static zx_handle_t g_dma_vmo = ZX_HANDLE_INVALID;
static zx_paddr_t  g_dma_paddr = 0;
static zx_vaddr_t  g_dma_vaddr = 0;

static uint64_t g_disk_capacity = 0;

/**
 * @brief Formats a single-line log message using snprintf and writes it atomically.
 */
static void blk_log(const char* fmt, ...) {
    char line_buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line_buf, sizeof(line_buf), fmt, args);
    va_end(args);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line_buf) ? (size_t)n : sizeof(line_buf) - 1;
        write(1, line_buf, len);
    }
}

/**
 * @brief Execute a synchronous VirtIO block transfer.
 *
 * @param type VIRTIO_BLK_T_IN (read) or VIRTIO_BLK_T_OUT (write).
 * @param sector Starting 512-byte sector number.
 * @param num_sectors Number of sectors to transfer.
 * @return ZX_OK on success, or appropriate error code.
 */
static zx_status_t virtio_blk_do_io(uint32_t type, uint64_t sector, uint32_t num_sectors) {
    if (num_sectors == 0 || num_sectors > 120) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (!g_dma_vaddr || !g_vq_desc_vaddr || !g_doorbell_vaddr) {
        return ZX_ERR_BAD_STATE;
    }

    volatile virtio_blk_req_hdr_t* req_hdr = (volatile virtio_blk_req_hdr_t*)g_dma_vaddr;
    volatile uint8_t* status_byte = (volatile uint8_t*)(g_dma_vaddr + 16);

    req_hdr->type = type;
    req_hdr->reserved = 0;
    req_hdr->sector = sector;
    *status_byte = 0xFF; /* Sentinel indicating in-flight operation */

    uint32_t data_len = num_sectors * 512u;

    volatile vring_desc_t* desc = (volatile vring_desc_t*)g_vq_desc_vaddr;
    volatile vring_avail_t* avail = (volatile vring_avail_t*)g_vq_avail_vaddr;
    volatile vring_used_t* used = (volatile vring_used_t*)g_vq_used_vaddr;

    /* Descriptor 0: Request Header (device reads header) */
    desc[0].addr = g_dma_paddr + 0;
    desc[0].len = sizeof(virtio_blk_req_hdr_t);
    desc[0].flags = VRING_DESC_F_NEXT;
    desc[0].next = 1;

    /* Descriptor 1: Data Buffer (device writes on read, reads on write) */
    desc[1].addr = g_dma_paddr + 512;
    desc[1].len = data_len;
    desc[1].flags = VRING_DESC_F_NEXT | ((type == VIRTIO_BLK_T_IN) ? VRING_DESC_F_WRITE : 0);
    desc[1].next = 2;

    /* Descriptor 2: Status Byte (device writes 1-byte status) */
    desc[2].addr = g_dma_paddr + 16;
    desc[2].len = 1;
    desc[2].flags = VRING_DESC_F_WRITE;
    desc[2].next = 0;

    /* Available Ring: place head descriptor 0 into available ring */
    uint16_t avail_idx = avail->idx;
    avail->ring[avail_idx % g_queue_size] = 0;
    __sync_synchronize();
    avail->idx = avail_idx + 1;
    __sync_synchronize();

    /* Ring Doorbell */
    *(volatile uint16_t*)g_doorbell_vaddr = 0;
    __sync_synchronize();

    /* Wait for completion with timeout */
    uint16_t last_used = g_last_used_idx;
    zx_time_t deadline = zx_deadline_after(ZX_SEC(3));
    while (used->idx == last_used) {
        __sync_synchronize();
        if (zx_clock_get_monotonic() > deadline) {
            blk_log("[FAILED] virtio_blk: Transfer timed out on sector %llu.\n",
                    (unsigned long long)sector);
            return ZX_ERR_TIMED_OUT;
        }
    }
    g_last_used_idx = used->idx;

    if (*status_byte != VIRTIO_BLK_S_OK) {
        blk_log("[FAILED] virtio_blk: Device reported transfer error 0x%02x.\n", *status_byte);
        return ZX_ERR_IO;
    }

    return ZX_OK;
}

/**
 * @brief Initialize VirtIO Block device hardware and queues.
 */
static zx_status_t init_virtio_block_device(void) {
    uint64_t ecam_phys = pci_get_default_ecam_base();
    if (ecam_phys == 0) {
        blk_log("[FAILED] virtio_blk: Unsupported architecture for ECAM.\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_handle_t ecam_vmo = ZX_HANDLE_INVALID;
    zx_status_t status = zx_vmo_create_physical(g_root_resource, ecam_phys, PCI_ECAM_SIZE_16_BUSES, &ecam_vmo);
    if (status != ZX_OK) {
        blk_log("[FAILED] virtio_blk: Failed to create ECAM physical VMO (%d).\n", status);
        return status;
    }

    zx_vaddr_t ecam_vaddr = 0;
    status = zx_vmar_map(zx_vmar_root_self(),
                         ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_MAP_RANGE,
                         0, ecam_vmo, 0, PCI_ECAM_SIZE_16_BUSES, &ecam_vaddr);
    zx_handle_close(ecam_vmo);
    if (status != ZX_OK) {
        blk_log("[FAILED] virtio_blk: Failed to map ECAM window (%d).\n", status);
        return status;
    }

    if (!pci_find_virtio_device(ecam_vaddr, 16, 0, &g_pci_dev)) {
        blk_log("[FAILED] virtio_blk: No VirtIO block device discovered on PCIe bus.\n");
        return ZX_ERR_NOT_FOUND;
    }

    blk_log("[  OK  ] virtio_blk: Bound to PCI %02x:%02x.%x (Vendor: 0x%04x, Device: 0x%04x).\n",
            g_pci_dev.bus, g_pci_dev.dev, g_pci_dev.func, g_pci_dev.vendor_id, g_pci_dev.device_id);

    /* Map device BARs containing capabilities */
    uint8_t bars_to_map[4] = { g_pci_dev.common_bar, g_pci_dev.notify_bar, g_pci_dev.isr_bar, g_pci_dev.device_bar };
    for (int i = 0; i < 4; i++) {
        uint8_t b = bars_to_map[i];
        if (b > 5) continue;
        if (g_pci_dev.bars[b].vaddr != 0) continue; /* Already mapped */

        uint64_t bar_phys = g_pci_dev.bars[b].phys_addr;
        if (bar_phys == 0) {
            blk_log("[FAILED] virtio_blk: BAR %u has invalid physical address 0.\n", b);
            return ZX_ERR_BAD_STATE;
        }

        uint64_t map_size = 65536u; /* 64 KB covers VirtIO BAR MMIO regions */
        zx_handle_t bar_vmo = ZX_HANDLE_INVALID;
        status = zx_vmo_create_physical(g_root_resource, bar_phys, map_size, &bar_vmo);
        if (status != ZX_OK) {
            blk_log("[FAILED] virtio_blk: zx_vmo_create_physical for BAR %u failed (%d).\n", b, status);
            return status;
        }

        zx_vaddr_t bar_vaddr = 0;
        status = zx_vmar_map(zx_vmar_root_self(),
                             ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_MAP_RANGE,
                             0, bar_vmo, 0, map_size, &bar_vaddr);
        zx_handle_close(bar_vmo);
        if (status != ZX_OK) {
            blk_log("[FAILED] virtio_blk: zx_vmar_map for BAR %u failed (%d).\n", b, status);
            return status;
        }

        g_pci_dev.bars[b].vaddr = bar_vaddr;
        blk_log("[  OK  ] virtio_blk: Mapped BAR %u (phys 0x%llx -> vaddr 0x%llx).\n",
                b, (unsigned long long)bar_phys, (unsigned long long)bar_vaddr);
    }

    g_common_cfg = (volatile virtio_pci_common_cfg_t*)(g_pci_dev.bars[g_pci_dev.common_bar].vaddr + g_pci_dev.common_offset);
    g_doorbell_vaddr = g_pci_dev.bars[g_pci_dev.notify_bar].vaddr + g_pci_dev.notify_offset;
    g_device_cfg = (volatile void*)(g_pci_dev.bars[g_pci_dev.device_bar].vaddr + g_pci_dev.device_offset);

    /* 1. Reset device */
    g_common_cfg->device_status = VIRTIO_STATUS_DEVICE_RESET;
    __sync_synchronize();

    /* 2. Acknowledge and announce driver */
    g_common_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    __sync_synchronize();

    /* 3. Feature negotiation (require VIRTIO_F_VERSION_1) */
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
        blk_log("[FAILED] virtio_blk: VirtIO device rejected feature negotiation.\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    /* 4. Virtqueue 0 Setup */
    g_common_cfg->queue_select = 0;
    g_queue_size = g_common_cfg->queue_size;
    if (g_queue_size == 0) {
        blk_log("[FAILED] virtio_blk: Queue 0 unavailable on device.\n");
        return ZX_ERR_NOT_FOUND;
    }

    /* Create dummy IOMMU and BTI */
    zx_iommu_desc_dummy_t dummy_desc = {0};
    zx_handle_t iommu = ZX_HANDLE_INVALID;
    status = zx_iommu_create(g_root_resource, ZX_IOMMU_TYPE_DUMMY, &dummy_desc, sizeof(dummy_desc), &iommu);
    if (status != ZX_OK) {
        blk_log("[FAILED] virtio_blk: zx_iommu_create failed (%d).\n", status);
        return status;
    }

    status = zx_bti_create(iommu, 0, 0, &g_bti);
    zx_handle_close(iommu);
    if (status != ZX_OK) {
        blk_log("[FAILED] virtio_blk: zx_bti_create failed (%d).\n", status);
        return status;
    }

    /* Allocate physically contiguous memory for Virtqueue 0 (16 KB) */
    size_t vq_bytes = 16384u;
    zx_handle_t vq_vmo = ZX_HANDLE_INVALID;
    status = zx_vmo_create_contiguous(g_bti, vq_bytes, 0, &vq_vmo);
    if (status != ZX_OK) {
        blk_log("[FAILED] virtio_blk: zx_vmo_create_contiguous for virtqueue failed (%d).\n", status);
        return status;
    }

    /* Pin Virtqueue VMO via BTI and store g_vq_pmt token for teardown unpinning */
    zx_paddr_t vq_paddr = 0;
    status = zx_bti_pin(g_bti, ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE | ZX_BTI_CONTIGUOUS,
                        vq_vmo, 0, vq_bytes, &vq_paddr, 1, &g_vq_pmt);
    if (status != ZX_OK) {
        blk_log("[FAILED] virtio_blk: zx_bti_pin for virtqueue failed (%d).\n", status);
        return status;
    }

    zx_vaddr_t vq_vaddr = 0;
    status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vq_vmo, 0, vq_bytes, &vq_vaddr);
    zx_handle_close(vq_vmo);
    if (status != ZX_OK) {
        blk_log("[FAILED] virtio_blk: zx_vmar_map for virtqueue failed (%d).\n", status);
        return status;
    }
    memset((void*)vq_vaddr, 0, vq_bytes);

    /* Split Virtqueue layout */
    g_vq_desc_vaddr = vq_vaddr + 0;
    g_vq_avail_vaddr = vq_vaddr + 4096;
    g_vq_used_vaddr = vq_vaddr + 6144;

    g_common_cfg->queue_desc = vq_paddr + 0;
    g_common_cfg->queue_driver = vq_paddr + 4096;
    g_common_cfg->queue_device = vq_paddr + 6144;
    __sync_synchronize();

    g_common_cfg->queue_enable = 1;
    __sync_synchronize();

    /* Doorbell register address calculation */
    uint16_t notify_off = g_common_cfg->queue_notify_off;
    g_doorbell_vaddr = g_pci_dev.bars[g_pci_dev.notify_bar].vaddr +
                       g_pci_dev.notify_offset +
                       ((uintptr_t)notify_off * g_pci_dev.notify_mult);

    /* Allocate DMA buffer for block transfer headers and sector data (64 KB) */
    size_t dma_bytes = 65536u;
    status = zx_vmo_create_contiguous(g_bti, dma_bytes, 0, &g_dma_vmo);
    if (status != ZX_OK) {
        blk_log("[FAILED] virtio_blk: zx_vmo_create_contiguous for DMA failed (%d).\n", status);
        return status;
    }

    /* Pin DMA transfer buffer via BTI and store g_dma_pmt token for teardown unpinning */
    status = zx_bti_pin(g_bti, ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE | ZX_BTI_CONTIGUOUS,
                        g_dma_vmo, 0, dma_bytes, &g_dma_paddr, 1, &g_dma_pmt);
    if (status != ZX_OK) {
        blk_log("[FAILED] virtio_blk: zx_bti_pin for DMA buffer failed (%d).\n", status);
        return status;
    }

    status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, g_dma_vmo, 0, dma_bytes, &g_dma_vaddr);
    if (status != ZX_OK) {
        blk_log("[FAILED] virtio_blk: zx_vmar_map for DMA buffer failed (%d).\n", status);
        return status;
    }
    memset((void*)g_dma_vaddr, 0, dma_bytes);

    /* 5. Set DRIVER_OK */
    g_common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
    __sync_synchronize();

    /* Read capacity in 512-byte sectors */
    g_disk_capacity = *(volatile uint64_t*)g_device_cfg;

    blk_log("[  OK  ] virtio_blk: Virtqueue 0 (%u descriptors) & BTI DMA initialized (%llu sectors, %llu MB).\n",
            g_queue_size,
            (unsigned long long)g_disk_capacity,
            (unsigned long long)(g_disk_capacity * 512u) / (1024u * 1024u));

    return ZX_OK;
}

/**
 * @brief Reset VirtIO hardware and explicitly unpin BTI Pinned Memory Tokens (PMTs).
 *
 * Zircon's BusTransactionInitiatorDispatcher quarantines and flags any PMT whose
 * handle is closed without first invoking `zx_pmt_unpin()`. Unpinning both
 * `g_vq_pmt` and `g_dma_pmt` guarantees zero leaked pages on driver teardown.
 */
static void cleanup_virtio_block_device(void) {
    if (g_common_cfg != NULL) {
        g_common_cfg->device_status = VIRTIO_STATUS_DEVICE_RESET;
        __sync_synchronize();
    }

    if (g_vq_pmt != ZX_HANDLE_INVALID) {
        zx_pmt_unpin(g_vq_pmt);
        g_vq_pmt = ZX_HANDLE_INVALID;
    }

    if (g_dma_pmt != ZX_HANDLE_INVALID) {
        zx_pmt_unpin(g_dma_pmt);
        g_dma_pmt = ZX_HANDLE_INVALID;
    }

    if (g_dma_vmo != ZX_HANDLE_INVALID) {
        zx_handle_close(g_dma_vmo);
        g_dma_vmo = ZX_HANDLE_INVALID;
    }

    if (g_bti != ZX_HANDLE_INVALID) {
        zx_handle_close(g_bti);
        g_bti = ZX_HANDLE_INVALID;
    }
}

/**
 * @brief Primary IPC event loop serving block requests over raw Zircon channels.
 *        Supports multiple concurrent client channels (e.g. iris_init / iris_fshost
 *        and iris_crypt) transferred via capability handle passing.
 */
#define MAX_BLK_CLIENTS 8

static void run_ipc_server(void) {
    blk_log("[  OK  ] virtio_blk: IPC server listening on control channel.\n");

    zx_handle_t clients[MAX_BLK_CLIENTS];
    for (int i = 0; i < MAX_BLK_CLIENTS; ++i) {
        clients[i] = ZX_HANDLE_INVALID;
    }
    clients[0] = g_server_channel;
    g_server_channel = ZX_HANDLE_INVALID;
    int active_clients = 1;

    uint8_t req_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));
    uint8_t rep_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));
    zx_handle_t handles[IRIS_IPC_MAX_HANDLES];
    uint32_t act_bytes = 0;
    uint32_t act_handles = 0;

    while (active_clients > 0) {
        bool did_work = false;

        for (int c = 0; c < MAX_BLK_CLIENTS; ++c) {
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
                    for (int slot = 0; slot < MAX_BLK_CLIENTS; ++slot) {
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

            switch (req_hdr->opcode) {
                case IRIS_BLK_OP_GET_INFO: {
                    iris_blk_info_t* info = (iris_blk_info_t*)(rep_buf + sizeof(iris_msg_hdr_t));
                    info->sector_count = g_disk_capacity;
                    info->sector_size = 512u;
                    info->max_transfer = 120u;

                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_OK, sizeof(iris_blk_info_t));
                    rep_total_len = sizeof(iris_msg_hdr_t) + sizeof(iris_blk_info_t);
                    break;
                }

                case IRIS_BLK_OP_WRITE: {
                    if (req_hdr->payload_len < sizeof(iris_blk_rw_req_t)) {
                        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
                        break;
                    }
                    const iris_blk_rw_req_t* rw = (const iris_blk_rw_req_t*)(req_buf + sizeof(iris_msg_hdr_t));
                    uint32_t data_bytes = rw->num_sectors * 512u;
                    if (req_hdr->payload_len < sizeof(iris_blk_rw_req_t) + data_bytes) {
                        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_BUFFER_TOO_SMALL, 0);
                        break;
                    }

                    /* Copy write data into DMA buffer */
                    const uint8_t* src_data = (const uint8_t*)(rw + 1);
                    memcpy((void*)(g_dma_vaddr + 512), src_data, data_bytes);

                    /* Perform disk write */
                    zx_status_t io_st = virtio_blk_do_io(VIRTIO_BLK_T_OUT, rw->start_sector, rw->num_sectors);
                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, io_st, 0);
                    break;
                }

                case IRIS_BLK_OP_READ: {
                    if (req_hdr->payload_len < sizeof(iris_blk_rw_req_t)) {
                        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
                        break;
                    }
                    const iris_blk_rw_req_t* rw = (const iris_blk_rw_req_t*)(req_buf + sizeof(iris_msg_hdr_t));
                    uint32_t data_bytes = rw->num_sectors * 512u;

                    /* Perform disk read */
                    zx_status_t io_st = virtio_blk_do_io(VIRTIO_BLK_T_IN, rw->start_sector, rw->num_sectors);
                    if (io_st == ZX_OK) {
                        /* Copy read data from DMA buffer to reply payload */
                        memcpy(rep_buf + sizeof(iris_msg_hdr_t), (const void*)(g_dma_vaddr + 512), data_bytes);
                        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_OK, data_bytes);
                        rep_total_len = sizeof(iris_msg_hdr_t) + data_bytes;
                    } else {
                        iris_msg_hdr_init_reply(rep_hdr, req_hdr, io_st, 0);
                    }
                    break;
                }

                case IRIS_BLK_OP_FLUSH: {
                    zx_status_t io_st = virtio_blk_do_io(VIRTIO_BLK_T_FLUSH, 0, 0);
                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, io_st, 0);
                    break;
                }

                default:
                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_NOT_SUPPORTED, 0);
                    break;
            }

            zx_channel_write(ch, 0, rep_buf, rep_total_len, NULL, 0);
        }

        if (!did_work && active_clients > 0) {
            if (active_clients == 1) {
                for (int c = 0; c < MAX_BLK_CLIENTS; ++c) {
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
                zx_nanosleep(zx_deadline_after(100000ULL)); /* 100 us poll interval across multi-channels */
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
        blk_log("[FAILED] virtio_blk: Missing Root Resource capability handle.\n");
        return 1;
    }

    /* Extract Driver Server channel endpoint */
    g_server_channel = zx_take_startup_handle(PA_HND(PA_USER0, 0));
    if (g_server_channel == ZX_HANDLE_INVALID) {
        blk_log("[FAILED] virtio_blk: Missing Driver IPC Server channel handle (PA_USER0).\n");
        return 1;
    }

    /* Initialize hardware */
    zx_status_t status = init_virtio_block_device();
    if (status != ZX_OK) {
        blk_log("[FAILED] virtio_blk: Failed to initialize VirtIO block device (%d).\n", status);
        cleanup_virtio_block_device();
        return 1;
    }

    /* Run request processing loop */
    run_ipc_server();

    /* Unpin DMA memory tokens and release BTI resources cleanly */
    cleanup_virtio_block_device();
    if (g_server_channel != ZX_HANDLE_INVALID) {
        zx_handle_close(g_server_channel);
        g_server_channel = ZX_HANDLE_INVALID;
    }
    blk_log("[  OK  ] virtio_blk: Unpinned BTI DMA tokens (zx_pmt_unpin) and exited cleanly.\n");
    return 0;
}
