/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/drivers/virtio_input/src/virtio_input.c
 * Purpose: Ring 3 User-Space VirtIO Input / Keyboard Driver Implementation.
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 driver source.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <zircon/types.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/iommu.h>

#include <iris/pci.h>
#include <iris/init.h>
#include <servers/drivers/pci/pci.h>
#include <servers/drivers/virtio_input/include/virtio_input.h>

#define PAGE_SIZE 4096ULL
#define PAGE_MASK (~(PAGE_SIZE - 1ULL))

static inline uint64_t align_page(uint64_t val) {
    return (val + PAGE_SIZE - 1ULL) & PAGE_MASK;
}

int virtio_input_translate_key(uint16_t code, bool shift, bool ctrl, bool caps) {
    if (ctrl) {
        switch (code) {
            case KEY_A: return 1;
            case KEY_B: return 2;
            case KEY_C: return 3;  /* Ctrl+C (SIGINT) */
            case KEY_D: return 4;  /* Ctrl+D (EOF) */
            case KEY_E: return 5;
            case KEY_F: return 6;
            case KEY_G: return 7;
            case KEY_H: return 8;  /* Backspace */
            case KEY_I: return '\t';
            case KEY_J: return '\n';
            case KEY_K: return 11;
            case KEY_L: return 12;
            case KEY_M: return '\r';
            case KEY_N: return 14;
            case KEY_O: return 15;
            case KEY_P: return 16;
            case KEY_Q: return 17;
            case KEY_R: return 18;
            case KEY_S: return 19;
            case KEY_T: return 20;
            case KEY_U: return 21;
            case KEY_V: return 22;
            case KEY_W: return 23;
            case KEY_X: return 24;
            case KEY_Y: return 25;
            case KEY_Z: return 26;
            case KEY_LEFTBRACE: return 27; /* ESC */
            case KEY_BACKSLASH: return 28;
            case KEY_RIGHTBRACE: return 29;
            default: break;
        }
    }

    switch (code) {
        case KEY_ESC: return 0x1B;
        case KEY_1: return shift ? '!' : '1';
        case KEY_2: return shift ? '@' : '2';
        case KEY_3: return shift ? '#' : '3';
        case KEY_4: return shift ? '$' : '4';
        case KEY_5: return shift ? '%' : '5';
        case KEY_6: return shift ? '^' : '6';
        case KEY_7: return shift ? '&' : '7';
        case KEY_8: return shift ? '*' : '8';
        case KEY_9: return shift ? '(' : '9';
        case KEY_0: return shift ? ')' : '0';
        case KEY_MINUS: return shift ? '_' : '-';
        case KEY_EQUAL: return shift ? '+' : '=';
        case KEY_BACKSPACE: return '\b';
        case KEY_TAB: return '\t';
        case KEY_Q: return (shift ^ caps) ? 'Q' : 'q';
        case KEY_W: return (shift ^ caps) ? 'W' : 'w';
        case KEY_E: return (shift ^ caps) ? 'E' : 'e';
        case KEY_R: return (shift ^ caps) ? 'R' : 'r';
        case KEY_T: return (shift ^ caps) ? 'T' : 't';
        case KEY_Y: return (shift ^ caps) ? 'Y' : 'y';
        case KEY_U: return (shift ^ caps) ? 'U' : 'u';
        case KEY_I: return (shift ^ caps) ? 'I' : 'i';
        case KEY_O: return (shift ^ caps) ? 'O' : 'o';
        case KEY_P: return (shift ^ caps) ? 'P' : 'p';
        case KEY_LEFTBRACE: return shift ? '{' : '[';
        case KEY_RIGHTBRACE: return shift ? '}' : ']';
        case KEY_ENTER: return '\r';
        case KEY_A: return (shift ^ caps) ? 'A' : 'a';
        case KEY_S: return (shift ^ caps) ? 'S' : 's';
        case KEY_D: return (shift ^ caps) ? 'D' : 'd';
        case KEY_F: return (shift ^ caps) ? 'F' : 'f';
        case KEY_G: return (shift ^ caps) ? 'G' : 'g';
        case KEY_H: return (shift ^ caps) ? 'H' : 'h';
        case KEY_J: return (shift ^ caps) ? 'J' : 'j';
        case KEY_K: return (shift ^ caps) ? 'K' : 'k';
        case KEY_L: return (shift ^ caps) ? 'L' : 'l';
        case KEY_SEMICOLON: return shift ? ':' : ';';
        case KEY_APOSTROPHE: return shift ? '"' : '\'';
        case KEY_GRAVE: return shift ? '~' : '`';
        case KEY_BACKSLASH: return shift ? '|' : '\\';
        case KEY_Z: return (shift ^ caps) ? 'Z' : 'z';
        case KEY_X: return (shift ^ caps) ? 'X' : 'x';
        case KEY_C: return (shift ^ caps) ? 'C' : 'c';
        case KEY_V: return (shift ^ caps) ? 'V' : 'v';
        case KEY_B: return (shift ^ caps) ? 'B' : 'b';
        case KEY_N: return (shift ^ caps) ? 'N' : 'n';
        case KEY_M: return (shift ^ caps) ? 'M' : 'm';
        case KEY_COMMA: return shift ? '<' : ',';
        case KEY_DOT: return shift ? '>' : '.';
        case KEY_SLASH: return shift ? '?' : '/';
        case KEY_SPACE: return ' ';
        case KEY_KPSLASH: return '/';
        case KEY_KPASTERISK: return '*';
        case KEY_KPMINUS: return '-';
        case KEY_KPPLUS: return '+';
        case KEY_KPENTER: return '\r';
        case KEY_KP1: return '1';
        case KEY_KP2: return '2';
        case KEY_KP3: return '3';
        case KEY_KP4: return '4';
        case KEY_KP5: return '5';
        case KEY_KP6: return '6';
        case KEY_KP7: return '7';
        case KEY_KP8: return '8';
        case KEY_KP9: return '9';
        case KEY_KP0: return '0';
        case KEY_KPDOT: return '.';
        default: return -1;
    }
}

zx_status_t virtio_input_init(uintptr_t ecam_vaddr, zx_handle_t root_resource, virtio_input_dev_t* out_dev) {
    if (!ecam_vaddr || root_resource == ZX_HANDLE_INVALID || !out_dev) {
        return ZX_ERR_INVALID_ARGS;
    }
    memset(out_dev, 0, sizeof(*out_dev));
    out_dev->bti = ZX_HANDLE_INVALID;
    out_dev->vq_pmt = ZX_HANDLE_INVALID;
    out_dev->dma_pmt = ZX_HANDLE_INVALID;

    /* 1. Discover virtio-input device on PCIe ECAM */
    if (!pci_find_virtio_device(ecam_vaddr, 16, VIRTIO_DEV_ID_INPUT, &out_dev->pci_dev)) {
        return ZX_ERR_NOT_FOUND;
    }

    /* 2. Map required device BARs */
    uint8_t bars_to_map[4] = {
        out_dev->pci_dev.common_bar,
        out_dev->pci_dev.notify_bar,
        out_dev->pci_dev.isr_bar,
        out_dev->pci_dev.device_bar
    };

    for (int i = 0; i < 4; i++) {
        uint8_t b = bars_to_map[i];
        if (b > 5) continue;
        if (out_dev->pci_dev.bars[b].vaddr != 0) continue;

        uint64_t bar_phys = out_dev->pci_dev.bars[b].phys_addr;
        if (bar_phys == 0) continue;

        uint64_t map_size = 65536u;
        zx_handle_t bar_vmo = ZX_HANDLE_INVALID;
        zx_status_t st = zx_vmo_create_physical(root_resource, bar_phys, map_size, &bar_vmo);
        if (st != ZX_OK) return st;

        zx_vaddr_t bar_vaddr = 0;
        st = zx_vmar_map(zx_vmar_root_self(),
                         ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_MAP_RANGE,
                         0, bar_vmo, 0, map_size, &bar_vaddr);
        zx_handle_close(bar_vmo);
        if (st != ZX_OK) return st;

        out_dev->pci_dev.bars[b].vaddr = bar_vaddr;
    }

    out_dev->common_cfg = (volatile virtio_pci_common_cfg_t*)(
        out_dev->pci_dev.bars[out_dev->pci_dev.common_bar].vaddr + out_dev->pci_dev.common_offset);
    out_dev->doorbell_vaddr = out_dev->pci_dev.bars[out_dev->pci_dev.notify_bar].vaddr + out_dev->pci_dev.notify_offset;

    /* 3. Reset device and acknowledge */
    out_dev->common_cfg->device_status = VIRTIO_STATUS_DEVICE_RESET;
    __sync_synchronize();

    out_dev->common_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    __sync_synchronize();

    /* 4. Feature negotiation (VIRTIO_F_VERSION_1) */
    out_dev->common_cfg->device_feature_select = 1;
    uint32_t dev_feat_1 = out_dev->common_cfg->device_feature;
    out_dev->common_cfg->driver_feature_select = 1;
    out_dev->common_cfg->driver_feature = (dev_feat_1 & 1u);

    out_dev->common_cfg->driver_feature_select = 0;
    out_dev->common_cfg->driver_feature = 0;
    __sync_synchronize();

    out_dev->common_cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
    __sync_synchronize();

    if (!(out_dev->common_cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    /* 5. Set up Queue 0 (eventq) */
    out_dev->common_cfg->queue_select = 0;
    out_dev->queue_size = out_dev->common_cfg->queue_size;
    if (out_dev->queue_size == 0) {
        return ZX_ERR_NOT_FOUND;
    }

    /* Create dummy IOMMU and BTI */
    zx_iommu_desc_dummy_t dummy_desc = {0};
    zx_handle_t iommu = ZX_HANDLE_INVALID;
    zx_status_t status = zx_iommu_create(root_resource, ZX_IOMMU_TYPE_DUMMY, &dummy_desc, sizeof(dummy_desc), &iommu);
    if (status != ZX_OK) return status;

    status = zx_bti_create(iommu, 0, 0, &out_dev->bti);
    zx_handle_close(iommu);
    if (status != ZX_OK) return status;

    /* Allocate physically contiguous memory for Virtqueue 0 (16 KB) */
    size_t vq_bytes = 16384u;
    zx_handle_t vq_vmo = ZX_HANDLE_INVALID;
    status = zx_vmo_create_contiguous(out_dev->bti, vq_bytes, 0, &vq_vmo);
    if (status != ZX_OK) return status;

    zx_paddr_t vq_paddr = 0;
    status = zx_bti_pin(out_dev->bti, ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE | ZX_BTI_CONTIGUOUS,
                        vq_vmo, 0, vq_bytes, &vq_paddr, 1, &out_dev->vq_pmt);
    if (status != ZX_OK) {
        zx_handle_close(vq_vmo);
        return status;
    }

    zx_vaddr_t vq_vaddr = 0;
    status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vq_vmo, 0, vq_bytes, &vq_vaddr);
    zx_handle_close(vq_vmo);
    if (status != ZX_OK) return status;

    memset((void*)vq_vaddr, 0, vq_bytes);

    size_t desc_size = out_dev->queue_size * sizeof(vring_desc_t);
    size_t avail_size = sizeof(vring_avail_t) + (out_dev->queue_size * sizeof(uint16_t));
    uint64_t used_offset = align_page(desc_size + avail_size);

    out_dev->vq_desc_vaddr = vq_vaddr;
    out_dev->vq_avail_vaddr = vq_vaddr + desc_size;
    out_dev->vq_used_vaddr = vq_vaddr + used_offset;

    out_dev->common_cfg->queue_desc = vq_paddr;
    out_dev->common_cfg->queue_driver = vq_paddr + desc_size;
    out_dev->common_cfg->queue_device = vq_paddr + used_offset;

    /* 6. Allocate DMA buffer for input event packets (4096 bytes) */
    size_t dma_bytes = 4096u;
    zx_handle_t dma_vmo = ZX_HANDLE_INVALID;
    status = zx_vmo_create_contiguous(out_dev->bti, dma_bytes, 0, &dma_vmo);
    if (status != ZX_OK) return status;

    zx_paddr_t dma_paddr = 0;
    status = zx_bti_pin(out_dev->bti, ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE | ZX_BTI_CONTIGUOUS,
                        dma_vmo, 0, dma_bytes, &dma_paddr, 1, &out_dev->dma_pmt);
    if (status != ZX_OK) {
        zx_handle_close(dma_vmo);
        return status;
    }

    zx_vaddr_t dma_vaddr = 0;
    status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, dma_vmo, 0, dma_bytes, &dma_vaddr);
    zx_handle_close(dma_vmo);
    if (status != ZX_OK) return status;

    memset((void*)dma_vaddr, 0, dma_bytes);
    out_dev->dma_vaddr = dma_vaddr;

    /* 7. Populate Virtqueue 0 Descriptors with Device-Writable Event Buffers */
    vring_desc_t* desc = (vring_desc_t*)out_dev->vq_desc_vaddr;
    volatile vring_avail_t* avail = (volatile vring_avail_t*)out_dev->vq_avail_vaddr;

    uint16_t num_bufs = (out_dev->queue_size < 64) ? out_dev->queue_size : 64;
    for (uint16_t i = 0; i < num_bufs; i++) {
        desc[i].addr = dma_paddr + i * sizeof(virtio_input_event_t);
        desc[i].len = (uint32_t)sizeof(virtio_input_event_t);
        desc[i].flags = VRING_DESC_F_WRITE;
        desc[i].next = 0;
        avail->ring[i] = i;
    }

    __sync_synchronize();
    avail->idx = num_bufs;
    out_dev->last_used_idx = 0;

    /* 8. Enable queue and ring doorbell */
    out_dev->common_cfg->queue_enable = 1;
    __sync_synchronize();

    volatile uint16_t* doorbell = (volatile uint16_t*)(
        out_dev->doorbell_vaddr + out_dev->common_cfg->queue_notify_off * out_dev->pci_dev.notify_mult);
    *doorbell = 0;

    /* 9. Set DRIVER_OK */
    out_dev->common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
    __sync_synchronize();

    out_dev->active = true;
    return ZX_OK;
}

zx_status_t virtio_input_poll(virtio_input_dev_t* dev, char* out_chars, size_t max_chars, size_t* out_count) {
    if (!dev || !dev->active || !dev->vq_used_vaddr || !out_chars || max_chars == 0) {
        if (out_count) *out_count = 0;
        return ZX_ERR_INVALID_ARGS;
    }

    volatile vring_used_t* used = (volatile vring_used_t*)dev->vq_used_vaddr;
    volatile vring_avail_t* avail = (volatile vring_avail_t*)dev->vq_avail_vaddr;
    size_t count = 0;
    uint16_t recycled = 0;

    while (dev->last_used_idx != used->idx && count < max_chars) {
        uint32_t id = used->ring[dev->last_used_idx % dev->queue_size].id;
        volatile virtio_input_event_t* ev = (volatile virtio_input_event_t*)(
            dev->dma_vaddr + id * sizeof(virtio_input_event_t));

        if (ev->type == EV_KEY) {
            if (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT) {
                dev->shift_down = (ev->value != 0);
            } else if (ev->code == KEY_LEFTCTRL || ev->code == KEY_RIGHTCTRL) {
                dev->ctrl_down = (ev->value != 0);
            } else if (ev->code == KEY_LEFTALT || ev->code == KEY_RIGHTALT) {
                dev->alt_down = (ev->value != 0);
            } else if (ev->code == KEY_CAPSLOCK && ev->value == 1) {
                dev->caps_lock = !dev->caps_lock;
            } else if (ev->value == 1 || ev->value == 2) {
                int ch = virtio_input_translate_key(ev->code, dev->shift_down, dev->ctrl_down, dev->caps_lock);
                if (ch >= 0 && count < max_chars) {
                    out_chars[count++] = (char)ch;
                }
            }
        }

        /* Recycle descriptor buffer into available ring */
        avail->ring[avail->idx % dev->queue_size] = (uint16_t)id;
        __sync_synchronize();
        avail->idx++;
        dev->last_used_idx++;
        recycled++;
    }

    if (recycled > 0) {
        __sync_synchronize();
        volatile uint16_t* doorbell = (volatile uint16_t*)(
            dev->doorbell_vaddr + dev->common_cfg->queue_notify_off * dev->pci_dev.notify_mult);
        *doorbell = 0;
    }

    if (out_count) *out_count = count;
    return ZX_OK;
}

void virtio_input_teardown(virtio_input_dev_t* dev) {
    if (!dev || !dev->active) return;

    if (dev->common_cfg) {
        dev->common_cfg->device_status = VIRTIO_STATUS_DEVICE_RESET;
        __sync_synchronize();
    }

    if (dev->dma_pmt != ZX_HANDLE_INVALID) {
        zx_pmt_unpin(dev->dma_pmt);
        dev->dma_pmt = ZX_HANDLE_INVALID;
    }

    if (dev->vq_pmt != ZX_HANDLE_INVALID) {
        zx_pmt_unpin(dev->vq_pmt);
        dev->vq_pmt = ZX_HANDLE_INVALID;
    }

    if (dev->bti != ZX_HANDLE_INVALID) {
        zx_handle_close(dev->bti);
        dev->bti = ZX_HANDLE_INVALID;
    }

    dev->active = false;
}
