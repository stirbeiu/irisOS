/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/src/vmo.c
 * Purpose: Safe C wrappers for Virtual Memory Object (VMO) allocation, mapping, and rights duplication.
 *
 * Licensing: MIT License. Permissive Zone 1 SDK source.
 */

#include <iris/vmo.h>
#include <zircon/syscalls.h>
#include <zircon/process.h>
#include <stdint.h>
#include <stddef.h>

zx_status_t iris_vmo_create(size_t size,
                            uint32_t options,
                            zx_handle_t* out_vmo) {
    if (!out_vmo) {
        return ZX_ERR_INVALID_ARGS;
    }
    return zx_vmo_create((uint64_t)size, options, out_vmo);
}

zx_status_t iris_vmo_map(zx_handle_t vmo,
                         zx_vm_option_t options,
                         uintptr_t* out_addr,
                         size_t* out_mapped_size) {
    if (vmo == ZX_HANDLE_INVALID || !out_addr) {
        return ZX_ERR_INVALID_ARGS;
    }

    uint64_t size = 0;
    zx_status_t status = zx_vmo_get_size(vmo, &size);
    if (status != ZX_OK) {
        return status;
    }

    if (options == 0) {
        options = ZX_VM_PERM_READ | ZX_VM_PERM_WRITE;
    }

    zx_vaddr_t vaddr = 0;
    status = zx_vmar_map(zx_vmar_root_self(), options, 0, vmo, 0, size, &vaddr);
    if (status != ZX_OK) {
        return status;
    }

    *out_addr = (uintptr_t)vaddr;
    if (out_mapped_size) {
        *out_mapped_size = (size_t)size;
    }
    return ZX_OK;
}

zx_status_t iris_vmo_map_offset(zx_handle_t vmar,
                                zx_handle_t vmo,
                                uint64_t vmo_offset,
                                size_t len,
                                zx_vm_option_t options,
                                uintptr_t* out_addr) {
    if (vmo == ZX_HANDLE_INVALID || !out_addr) {
        return ZX_ERR_INVALID_ARGS;
    }

    zx_handle_t target_vmar = (vmar != ZX_HANDLE_INVALID) ? vmar : zx_vmar_root_self();
    zx_vaddr_t vaddr = 0;
    zx_status_t status = zx_vmar_map(target_vmar, options, 0, vmo, vmo_offset, (uint64_t)len, &vaddr);
    if (status != ZX_OK) {
        return status;
    }

    *out_addr = (uintptr_t)vaddr;
    return ZX_OK;
}

zx_status_t iris_vmo_unmap(uintptr_t addr, size_t size) {
    if (addr == 0 || size == 0) {
        return ZX_ERR_INVALID_ARGS;
    }
    return zx_vmar_unmap(zx_vmar_root_self(), (zx_vaddr_t)addr, (uint64_t)size);
}

zx_status_t iris_vmo_unmap_from(zx_handle_t vmar, uintptr_t addr, size_t size) {
    if (vmar == ZX_HANDLE_INVALID || addr == 0 || size == 0) {
        return ZX_ERR_INVALID_ARGS;
    }
    return zx_vmar_unmap(vmar, (zx_vaddr_t)addr, (uint64_t)size);
}

zx_status_t iris_vmo_duplicate(zx_handle_t vmo,
                               zx_rights_t rights,
                               zx_handle_t* out_handle) {
    if (vmo == ZX_HANDLE_INVALID || !out_handle) {
        return ZX_ERR_INVALID_ARGS;
    }
    return zx_handle_duplicate(vmo, rights, out_handle);
}

zx_status_t iris_vmo_get_size(zx_handle_t vmo, uint64_t* out_size) {
    if (vmo == ZX_HANDLE_INVALID || !out_size) {
        return ZX_ERR_INVALID_ARGS;
    }
    return zx_vmo_get_size(vmo, out_size);
}

zx_status_t iris_vmo_set_size(zx_handle_t vmo, uint64_t new_size) {
    if (vmo == ZX_HANDLE_INVALID) {
        return ZX_ERR_INVALID_ARGS;
    }
    return zx_vmo_set_size(vmo, new_size);
}

zx_status_t iris_vmo_read(zx_handle_t vmo,
                          void* buffer,
                          uint64_t offset,
                          size_t buffer_size) {
    if (vmo == ZX_HANDLE_INVALID || !buffer) {
        return ZX_ERR_INVALID_ARGS;
    }
    return zx_vmo_read(vmo, buffer, offset, buffer_size);
}

zx_status_t iris_vmo_write(zx_handle_t vmo,
                           const void* buffer,
                           uint64_t offset,
                           size_t buffer_size) {
    if (vmo == ZX_HANDLE_INVALID || !buffer) {
        return ZX_ERR_INVALID_ARGS;
    }
    return zx_vmo_write(vmo, buffer, offset, buffer_size);
}
