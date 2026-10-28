/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/drivers/pci/pci.c
 * Purpose: Unified PCIe (ECAM) Configuration Space Scanner & VirtIO 1.0+ Capability Parser.
 *
 * Licensing: MIT License. Permissive Zone 1 driver source.
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

#include "pci.h"
#include <iris/pci.h>

/**
 * @file pci.c
 * @brief Unified ECAM PCIe bus scanner and VirtIO 1.0 capability extractor.
 *
 * HARDWARE INTERACTION & REGISTER PROTOCOL:
 *  1. ECAM MMIO Translation:
 *     PCIe configuration space is memory-mapped. The physical address for any
 *     register is calculated as:
 *       ECAM_BASE + (bus << 20) + (dev << 15) + (func << 12) + reg_offset.
 *  2. Capability Traversal:
 *     If the PCI Status register bit 4 (Capabilities List) is set, offset 0x34
 *     contains the pointer to the first capability structure. Each capability
 *     header has:
 *       - Offset 0: Capability ID (0x09 = Vendor-Specific for VirtIO 1.0)
 *       - Offset 1: Next capability pointer (0x00 = list termination)
 *       - Offset 2: Capability structure length
 *       - Offset 3: VirtIO cfg_type (1=Common, 2=Notify, 3=ISR, 4=Device)
 *       - Offset 4: BAR index containing the MMIO registers
 *       - Offset 8: Offset within the specified BAR
 *       - Offset 12: Byte length of the structure
 *  3. Bus Master Activation:
 *     Before a PCIe endpoint can execute DMA transactions or receive MMIO accesses,
 *     Bit 1 (Memory Space Enable) and Bit 2 (Bus Master Enable) must be set in
 *     PCI_CFG_COMMAND (offset 0x04).
 */

void pci_parse_bars(uintptr_t ecam, uint8_t bus, uint8_t dev, uint8_t func, pci_bar_t bars[6]) {
    for (uint8_t i = 0; i < 6; i++) {
        bars[i].phys_addr = 0;
        bars[i].size = 0;
        bars[i].vaddr = 0;
        bars[i].is_io = false;
        bars[i].is_64bit = false;
        bars[i].is_prefetchable = false;
    }

    for (uint8_t i = 0; i < 6; i++) {
        uint16_t bar_off = PCI_CFG_BAR0 + (i * 4);
        uint32_t bar_val = pci_read32(ecam, bus, dev, func, bar_off);

        if (bar_val == 0 || bar_val == 0xFFFFFFFFu) {
            continue;
        }

        if (bar_val & 1u) {
            /* I/O Space BAR */
            bars[i].is_io = true;
            bars[i].phys_addr = bar_val & ~0x3u;
        } else {
            /* Memory Space BAR */
            bars[i].is_io = false;
            bars[i].is_prefetchable = ((bar_val & (1u << 3)) != 0);

            uint8_t type = (bar_val >> 1) & 0x3u;
            if (type == 0x2u) {
                /* 64-bit Memory Space BAR */
                bars[i].is_64bit = true;
                if (i < 5) {
                    uint32_t bar_high = pci_read32(ecam, bus, dev, func, bar_off + 4);
                    bars[i].phys_addr = (((uint64_t)bar_high) << 32) | (bar_val & ~0xFULL);
                    i++; /* Skip upper 32-bit BAR register */
                } else {
                    bars[i].phys_addr = bar_val & ~0xFULL;
                }
            } else {
                /* 32-bit Memory Space BAR */
                bars[i].phys_addr = bar_val & ~0xFULL;
            }
        }
    }
}

bool pci_parse_virtio_caps(uintptr_t ecam, virtio_pci_dev_t* dev) {
    if (!dev) return false;

    dev->has_common_cfg = false;
    dev->has_notify_cfg = false;
    dev->has_isr_cfg    = false;
    dev->has_device_cfg = false;

    uint16_t status = pci_read16(ecam, dev->bus, dev->dev, dev->func, PCI_CFG_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) {
        return false;
    }

    uint8_t cap_ptr = pci_read8(ecam, dev->bus, dev->dev, dev->func, PCI_CFG_CAP_PTR) & ~0x3u;

    while (cap_ptr >= 0x40u && cap_ptr <= 0xFCu) {
        uint8_t cap_vndr = pci_read8(ecam, dev->bus, dev->dev, dev->func, cap_ptr);
        uint8_t cap_next = pci_read8(ecam, dev->bus, dev->dev, dev->func, cap_ptr + 1) & ~0x3u;
        uint8_t cap_len  = pci_read8(ecam, dev->bus, dev->dev, dev->func, cap_ptr + 2);

        if (cap_vndr == PCI_CAP_ID_VNDR && cap_len >= sizeof(virtio_pci_cap_t)) {
            uint8_t  cfg_type = pci_read8(ecam, dev->bus, dev->dev, dev->func, cap_ptr + 3);
            uint8_t  bar      = pci_read8(ecam, dev->bus, dev->dev, dev->func, cap_ptr + 4);
            uint32_t offset   = pci_read32(ecam, dev->bus, dev->dev, dev->func, cap_ptr + 8);
            uint32_t length   = pci_read32(ecam, dev->bus, dev->dev, dev->func, cap_ptr + 12);

            switch (cfg_type) {
                case VIRTIO_PCI_CAP_COMMON_CFG:
                    dev->has_common_cfg = true;
                    dev->common_bar = bar;
                    dev->common_offset = offset;
                    dev->common_length = length;
                    break;

                case VIRTIO_PCI_CAP_NOTIFY_CFG:
                    dev->has_notify_cfg = true;
                    dev->notify_bar = bar;
                    dev->notify_offset = offset;
                    dev->notify_length = length;
                    if (cap_len >= sizeof(virtio_pci_notify_cap_t)) {
                        dev->notify_mult = pci_read32(ecam, dev->bus, dev->dev, dev->func, cap_ptr + 16);
                    } else {
                        dev->notify_mult = 0;
                    }
                    break;

                case VIRTIO_PCI_CAP_ISR_CFG:
                    dev->has_isr_cfg = true;
                    dev->isr_bar = bar;
                    dev->isr_offset = offset;
                    dev->isr_length = length;
                    break;

                case VIRTIO_PCI_CAP_DEVICE_CFG:
                    dev->has_device_cfg = true;
                    dev->device_bar = bar;
                    dev->device_offset = offset;
                    dev->device_length = length;
                    break;

                default:
                    break;
            }
        }

        cap_ptr = cap_next;
    }

    return dev->has_common_cfg && dev->has_notify_cfg;
}

bool pci_find_virtio_device_nth(uintptr_t ecam, uint8_t max_buses, uint16_t target_dev_id,
                                uint32_t match_index, virtio_pci_dev_t* out_dev) {
    if (!ecam || !out_dev) return false;

    uint32_t seen_matches = 0;

    for (uint16_t bus = 0; bus < max_buses; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint16_t vendor_id = pci_read16(ecam, (uint8_t)bus, dev, 0, PCI_CFG_VENDOR_ID);
            if (vendor_id == 0xFFFFu || vendor_id == 0) {
                continue;
            }

            uint8_t header_type = pci_read8(ecam, (uint8_t)bus, dev, 0, PCI_CFG_HEADER_TYPE);
            uint8_t max_funcs = (header_type & 0x80u) ? 8 : 1;

            for (uint8_t func = 0; func < max_funcs; func++) {
                vendor_id = pci_read16(ecam, (uint8_t)bus, dev, func, PCI_CFG_VENDOR_ID);
                if (vendor_id != VIRTIO_PCI_VENDOR_ID) {
                    continue;
                }

                uint16_t device_id = pci_read16(ecam, (uint8_t)bus, dev, func, PCI_CFG_DEVICE_ID);

                bool is_match = false;
                if (target_dev_id != 0) {
                    if (target_dev_id == VIRTIO_DEV_ID_INPUT || target_dev_id == VIRTIO_DEV_ID_INPUT_TRANS) {
                        is_match = (device_id == VIRTIO_DEV_ID_INPUT || device_id == VIRTIO_DEV_ID_INPUT_TRANS);
                    } else if (target_dev_id == VIRTIO_DEV_ID_GPU || target_dev_id == VIRTIO_DEV_ID_GPU_TRANS) {
                        is_match = (device_id == VIRTIO_DEV_ID_GPU || device_id == VIRTIO_DEV_ID_GPU_TRANS);
                    } else if (target_dev_id == VIRTIO_DEV_ID_BLK || target_dev_id == VIRTIO_DEV_ID_BLK_TRANS) {
                        is_match = (device_id == VIRTIO_DEV_ID_BLK || device_id == VIRTIO_DEV_ID_BLK_TRANS);
                    } else {
                        is_match = (device_id == target_dev_id);
                    }
                } else {
                    /* Match any VirtIO block device (modern 0x1042 or transitional 0x1001) */
                    is_match = (device_id == VIRTIO_DEV_ID_BLK || device_id == VIRTIO_DEV_ID_BLK_TRANS);
                }

                if (is_match) {
                    if (seen_matches < match_index) {
                        seen_matches++;
                        continue;
                    }
                    out_dev->bus = (uint8_t)bus;
                    out_dev->dev = dev;
                    out_dev->func = func;
                    out_dev->vendor_id = vendor_id;
                    out_dev->device_id = device_id;
                    out_dev->revision = pci_read8(ecam, (uint8_t)bus, dev, func, PCI_CFG_REVISION_ID);
                    out_dev->irq_line = pci_read8(ecam, (uint8_t)bus, dev, func, PCI_CFG_INTERRUPT_LINE);

                    pci_parse_bars(ecam, (uint8_t)bus, dev, func, out_dev->bars);
                    pci_parse_virtio_caps(ecam, out_dev);

                    /* Enable device memory decoding and bus master DMA */
                    pci_enable_device(ecam, (uint8_t)bus, dev, func);

                    return true;
                }
            }
        }
    }

    return false;
}

bool pci_find_virtio_device(uintptr_t ecam, uint8_t max_buses, uint16_t target_dev_id, virtio_pci_dev_t* out_dev) {
    return pci_find_virtio_device_nth(ecam, max_buses, target_dev_id, 0, out_dev);
}
