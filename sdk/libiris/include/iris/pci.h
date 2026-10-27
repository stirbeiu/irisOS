/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/include/iris/pci.h
 * Purpose: Unified PCIe Enhanced Configuration Access Mechanism (ECAM) definitions,
 *          PCI register offsets, and VirtIO 1.0+ capability parsing structures.
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

#ifndef IRIS_PCI_H_
#define IRIS_PCI_H_

/**
 * @file pci.h
 * @brief Unified PCIe (ECAM) and VirtIO 1.0 PCI structures and discovery helpers.
 *
 * ARCHITECTURAL DESIGN:
 *  Per Section 4.7 of AGENTS.md, irisOS standardizes on PCIe (ECAM) for device
 *  discovery across BOTH x86_64 (QEMU -M q35) and ARM64 (QEMU -M virt).
 *  This architecture allows iris_init and Ring 3 user-space device drivers
 *  (virtio-blk-pci, virtio-gpu-pci, virtio-input-pci) to share a unified
 *  PCIe enumeration and register access codebase without architecture-specific
 *  legacy port I/O hacks.
 *
 * PHYSICAL ECAM BASE ADDRESSES:
 *  - x86_64 (QEMU q35): 0xB0000000 (PCI Express MMCFG / ECAM, length 256 MB)
 *  - ARM64  (QEMU virt): 0x4010000000 (PCIe ECAM Window, length 256 MB)
 *
 * VIRTIO 1.0 SPECIFICATION CONFORMANCE:
 *  Modern VirtIO devices expose capabilities in standard PCI configuration space
 *  using Vendor-Specific Capability headers (ID 0x09). The capabilities specify
 *  the BAR index, byte offset, and length for:
 *   - Common configuration (device status, feature bits, virtqueue configuration)
 *   - Notifications (doorbell registers to notify device of new descriptors)
 *   - ISR status (interrupt handling flags)
 *   - Device-specific configuration (disk capacity, geometry, network MAC, etc.)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zircon/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================================================
 * PCIe ECAM Base Addresses & Dimensions
 * ============================================================================== */

#define PCI_ECAM_PHYS_BASE_X86_64   0xE0000000ULL
#define PCI_ECAM_PHYS_BASE_ARM64    0x4010000000ULL
#define PCI_ECAM_SIZE_256_BUSES     (256ULL * 1024ULL * 1024ULL) /* 256 MB */
#define PCI_ECAM_SIZE_16_BUSES      (16ULL * 1024ULL * 1024ULL)  /* 16 MB */

/**
 * @brief Returns the default physical ECAM base address for the target architecture.
 */
static inline uint64_t pci_get_default_ecam_base(void) {
#if defined(__x86_64__)
    return PCI_ECAM_PHYS_BASE_X86_64;
#elif defined(__aarch64__)
    return PCI_ECAM_PHYS_BASE_ARM64;
#else
    return 0;
#endif
}

/* ==============================================================================
 * Standard PCI Configuration Space Registers & Offsets
 * ============================================================================== */

#define PCI_CFG_VENDOR_ID           0x00u
#define PCI_CFG_DEVICE_ID           0x02u
#define PCI_CFG_COMMAND             0x04u
#define PCI_CFG_STATUS              0x06u
#define PCI_CFG_REVISION_ID         0x08u
#define PCI_CFG_PROG_IF             0x09u
#define PCI_CFG_SUBCLASS            0x0Au
#define PCI_CFG_CLASS_CODE          0x0Bu
#define PCI_CFG_CACHE_LINE_SIZE     0x0Cu
#define PCI_CFG_LATENCY_TIMER       0x0Du
#define PCI_CFG_HEADER_TYPE         0x0Eu
#define PCI_CFG_BIST                0x0Fu

#define PCI_CFG_BAR0                0x10u
#define PCI_CFG_BAR1                0x14u
#define PCI_CFG_BAR2                0x18u
#define PCI_CFG_BAR3                0x1Cu
#define PCI_CFG_BAR4                0x20u
#define PCI_CFG_BAR5                0x24u

#define PCI_CFG_CAP_PTR             0x34u
#define PCI_CFG_INTERRUPT_LINE      0x3Cu
#define PCI_CFG_INTERRUPT_PIN       0x3Du

/* PCI Command Register Bits */
#define PCI_COMMAND_IO_ENABLE       (1u << 0)
#define PCI_COMMAND_MEMORY_ENABLE   (1u << 1)
#define PCI_COMMAND_BUS_MASTER      (1u << 2)

/* PCI Status Register Bits */
#define PCI_STATUS_CAP_LIST         (1u << 4)

/* PCI Capability IDs */
#define PCI_CAP_ID_VNDR             0x09u

/* ==============================================================================
 * VirtIO PCI Identifiers & Capability Types
 * ============================================================================== */

#define VIRTIO_PCI_VENDOR_ID        0x1AF4u

/* VirtIO 1.0 Non-Transitional Device IDs (0x1040 + Subsystem Device ID) */
#define VIRTIO_DEV_ID_NET           0x1041u
#define VIRTIO_DEV_ID_BLK           0x1042u
#define VIRTIO_DEV_ID_CONSOLE       0x1043u
#define VIRTIO_DEV_ID_ENTROPY       0x1044u
#define VIRTIO_DEV_ID_BALLOON       0x1045u
#define VIRTIO_DEV_ID_GPU           0x1050u
#define VIRTIO_DEV_ID_INPUT         0x1052u

/* VirtIO Transitional / Legacy Device IDs (0x1000..0x103F) */
#define VIRTIO_DEV_ID_NET_TRANS     0x1000u
#define VIRTIO_DEV_ID_BLK_TRANS     0x1001u
#define VIRTIO_DEV_ID_INPUT_TRANS   0x1010u
#define VIRTIO_DEV_ID_GPU_TRANS     0x1014u

/* VirtIO 1.0 PCI Capability Types (virtio_pci_cap::cfg_type) */
#define VIRTIO_PCI_CAP_COMMON_CFG   1u /**< Common configuration */
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2u /**< Notifications / Doorbell */
#define VIRTIO_PCI_CAP_ISR_CFG      3u /**< ISR Status */
#define VIRTIO_PCI_CAP_DEVICE_CFG   4u /**< Device-specific configuration */
#define VIRTIO_PCI_CAP_PCI_CFG      5u /**< PCI configuration access */

/* ==============================================================================
 * VirtIO 1.0 Device Status & Feature Bits
 * ============================================================================== */

#define VIRTIO_STATUS_ACKNOWLEDGE   1u
#define VIRTIO_STATUS_DRIVER        2u
#define VIRTIO_STATUS_DRIVER_OK     4u
#define VIRTIO_STATUS_FEATURES_OK   8u
#define VIRTIO_STATUS_DEVICE_RESET  0u
#define VIRTIO_STATUS_FAILED        128u

/* Standard VirtIO Feature Bits */
#define VIRTIO_F_VERSION_1          (1ULL << 32) /**< VirtIO 1.0 compliant device */

/* VirtIO Block Request Types */
#define VIRTIO_BLK_T_IN             0u /**< Read from device */
#define VIRTIO_BLK_T_OUT            1u /**< Write to device */
#define VIRTIO_BLK_T_FLUSH          4u /**< Flush write cache */

/* VirtIO Block Status Codes */
#define VIRTIO_BLK_S_OK             0u
#define VIRTIO_BLK_S_IOERR          1u
#define VIRTIO_BLK_S_UNSUPP         2u

/* Virtqueue Descriptor Flags */
#define VRING_DESC_F_NEXT           1u /**< Buffer continues via next field */
#define VRING_DESC_F_WRITE          2u /**< Buffer is device-writable (device reads: 0) */
#define VRING_DESC_F_INDIRECT       4u /**< Buffer contains indirect descriptors */

/* ==============================================================================
 * VirtIO 1.0 Specification Structures
 * ============================================================================== */

#pragma pack(push, 1)

/**
 * @brief VirtIO PCI Capability structure header found in PCI config space.
 */
typedef struct virtio_pci_cap {
    uint8_t  cap_vndr;    /**< Generic PCI capability ID (must be 0x09) */
    uint8_t  cap_next;    /**< Offset to next capability in config space */
    uint8_t  cap_len;     /**< Length of this capability structure in bytes */
    uint8_t  cfg_type;    /**< VirtIO capability type (VIRTIO_PCI_CAP_*) */
    uint8_t  bar;         /**< BAR index containing this structure (0..5) */
    uint8_t  id;          /**< Identifier within cfg_type */
    uint8_t  padding[2];
    uint32_t offset;      /**< Byte offset within the referenced BAR */
    uint32_t length;      /**< Length of region within the BAR */
} virtio_pci_cap_t;

/**
 * @brief VirtIO Notification Capability extending virtio_pci_cap.
 */
typedef struct virtio_pci_notify_cap {
    virtio_pci_cap_t cap;
    uint32_t notify_off_multiplier; /**< Multiplier for queue_notify_off */
} virtio_pci_notify_cap_t;

/**
 * @brief VirtIO 1.0 Common Configuration structure (mapped inside device BAR).
 */
typedef struct virtio_pci_common_cfg {
    /* Device-wide configuration */
    uint32_t device_feature_select; /**< Selects feature bit word (0 or 1) */
    uint32_t device_feature;        /**< Device-advertised features (read-only) */
    uint32_t driver_feature_select; /**< Selects feature bit word for driver */
    uint32_t driver_feature;        /**< Driver-accepted features (read-write) */
    uint16_t config_msix_vector;    /**< MSI-X vector for config changes */
    uint16_t num_queues;            /**< Maximum virtqueues supported */
    uint8_t  device_status;         /**< Device status flags (VIRTIO_STATUS_*) */
    uint8_t  config_generation;     /**< Configuration generation counter */

    /* Virtqueue-specific configuration */
    uint16_t queue_select;          /**< Selects virtqueue index (0, 1, ...) */
    uint16_t queue_size;            /**< Queue size (number of descriptors) */
    uint16_t queue_msix_vector;     /**< MSI-X vector for this queue */
    uint16_t queue_enable;          /**< Enable bit (1 = enabled) */
    uint16_t queue_notify_off;      /**< Doorbell register offset index */
    uint64_t queue_desc;            /**< Physical address of descriptor table */
    uint64_t queue_driver;          /**< Physical address of available ring */
    uint64_t queue_device;          /**< Physical address of used ring */
} virtio_pci_common_cfg_t;

/**
 * @brief VirtIO Block Request Header prepended to every disk transfer.
 */
typedef struct virtio_blk_req_hdr {
    uint32_t type;     /**< VIRTIO_BLK_T_IN or VIRTIO_BLK_T_OUT */
    uint32_t reserved; /**< Reserved (0) */
    uint64_t sector;   /**< Starting 512-byte sector number */
} virtio_blk_req_hdr_t;

/**
 * @brief Virtqueue Descriptor Entry.
 */
typedef struct vring_desc {
    uint64_t addr;   /**< Physical address of memory buffer */
    uint32_t len;    /**< Length of buffer in bytes */
    uint16_t flags;  /**< VRING_DESC_F_* */
    uint16_t next;   /**< Next descriptor in chain if VRING_DESC_F_NEXT */
} vring_desc_t;

/**
 * @brief Virtqueue Available Ring Header.
 */
typedef struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[]; /**< Array of descriptor table head indices */
} vring_avail_t;

/**
 * @brief Virtqueue Used Ring Element.
 */
typedef struct vring_used_elem {
    uint32_t id;  /**< Index of start of completed descriptor chain */
    uint32_t len; /**< Number of bytes written by device */
} vring_used_elem_t;

/**
 * @brief Virtqueue Used Ring Header.
 */
typedef struct vring_used {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[];
} vring_used_t;

#pragma pack(pop)

/* ==============================================================================
 * Discovered Device Information Structures
 * ============================================================================== */

/**
 * @brief PCI Base Address Register (BAR) descriptor.
 */
typedef struct pci_bar {
    uint64_t phys_addr;       /**< Physical memory address of BAR */
    uint64_t size;            /**< Allocated size of BAR in bytes */
    zx_vaddr_t vaddr;         /**< Mapped virtual address (if mapped) */
    bool     is_io;           /**< True if I/O space, false if MMIO */
    bool     is_64bit;        /**< True if 64-bit BAR */
    bool     is_prefetchable; /**< True if prefetchable memory */
} pci_bar_t;

/**
 * @brief Parsed VirtIO 1.0 PCI Device Information.
 */
typedef struct virtio_pci_dev {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  revision;
    uint8_t  irq_line;

    /* BAR physical mappings */
    pci_bar_t bars[6];

    /* Common Configuration */
    bool     has_common_cfg;
    uint8_t  common_bar;
    uint32_t common_offset;
    uint32_t common_length;

    /* Notifications / Doorbell */
    bool     has_notify_cfg;
    uint8_t  notify_bar;
    uint32_t notify_offset;
    uint32_t notify_length;
    uint32_t notify_mult;

    /* ISR Status */
    bool     has_isr_cfg;
    uint8_t  isr_bar;
    uint32_t isr_offset;
    uint32_t isr_length;

    /* Device-specific Configuration */
    bool     has_device_cfg;
    uint8_t  device_bar;
    uint32_t device_offset;
    uint32_t device_length;
} virtio_pci_dev_t;

/* ==============================================================================
 * ECAM Register Access Helpers
 * ============================================================================== */

/**
 * @brief Compute the virtual address of a PCI register in a mapped ECAM window.
 */
static inline uintptr_t pci_ecam_reg_addr(uintptr_t ecam_base_vaddr,
                                         uint8_t bus,
                                         uint8_t dev,
                                         uint8_t func,
                                         uint16_t reg_offset) {
    return ecam_base_vaddr +
           (((uintptr_t)bus << 20) |
            ((uintptr_t)dev << 15) |
            ((uintptr_t)func << 12) |
            ((uintptr_t)reg_offset & 0xFFFu));
}

static inline uint8_t pci_read8(uintptr_t ecam, uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg) {
    return *(volatile uint8_t*)pci_ecam_reg_addr(ecam, bus, dev, func, reg);
}

static inline uint16_t pci_read16(uintptr_t ecam, uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg) {
    return *(volatile uint16_t*)pci_ecam_reg_addr(ecam, bus, dev, func, reg);
}

static inline uint32_t pci_read32(uintptr_t ecam, uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg) {
    return *(volatile uint32_t*)pci_ecam_reg_addr(ecam, bus, dev, func, reg);
}

static inline void pci_write8(uintptr_t ecam, uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg, uint8_t val) {
    *(volatile uint8_t*)pci_ecam_reg_addr(ecam, bus, dev, func, reg) = val;
}

static inline void pci_write16(uintptr_t ecam, uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg, uint16_t val) {
    *(volatile uint16_t*)pci_ecam_reg_addr(ecam, bus, dev, func, reg) = val;
}

static inline void pci_write32(uintptr_t ecam, uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg, uint32_t val) {
    *(volatile uint32_t*)pci_ecam_reg_addr(ecam, bus, dev, func, reg) = val;
}

/**
 * @brief Enable memory decoding and DMA bus mastering on a PCI device.
 */
static inline void pci_enable_device(uintptr_t ecam, uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t cmd = pci_read16(ecam, bus, dev, func, PCI_CFG_COMMAND);
    cmd |= (PCI_COMMAND_MEMORY_ENABLE | PCI_COMMAND_BUS_MASTER);
    pci_write16(ecam, bus, dev, func, PCI_CFG_COMMAND, cmd);
}

#ifdef __cplusplus
}
#endif

#endif /* IRIS_PCI_H_ */
