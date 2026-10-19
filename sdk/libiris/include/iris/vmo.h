/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/include/iris/vmo.h
 * Purpose: Safe C wrappers for Virtual Memory Object (VMO) allocation, mapping, and rights duplication.
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

#ifndef IRIS_VMO_H_
#define IRIS_VMO_H_

/**
 * @file vmo.h
 * @brief Virtual Memory Object (VMO) management primitives for irisOS.
 *
 * ARCHITECTURAL DESIGN & MEMORY MODEL:
 *  Virtual Memory Objects (VMOs) represent paged physical memory in the Zircon
 *  microkernel. In irisOS, VMOs serve as the primary medium for:
 *   1. Zero-copy inter-process shared memory (e.g., framebuffer surfaces in `iris_compositor`).
 *   2. Safe direct memory access (DMA) backing buffers pinned via Zircon BTI in device drivers.
 *   3. Child process stacks and ELF binary image mappings.
 *
 * CAPABILITY LIFECYCLE:
 *  - Handles returned by `iris_vmo_create()` or `iris_vmo_duplicate()` are ACQUIRED
 *    by the caller; the caller is responsible for eventual closure via `zx_handle_close()`.
 *  - Memory mapped via `iris_vmo_map()` or `iris_vmo_map_offset()` must eventually
 *    be unmapped via `iris_vmo_unmap()` or `iris_vmo_unmap_from()`.
 */

#include <stdint.h>
#include <stddef.h>
#include <zircon/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocate a new anonymous Virtual Memory Object (VMO).
 *
 * @param size Requested capacity in bytes (automatically rounded up to PAGE_SIZE by kernel).
 * @param options Allocation options (e.g., 0 for default anonymous).
 * @param[out] out_vmo Returns the newly created VMO capability handle.
 *
 * @return ZX_OK on success.
 * @return ZX_ERR_NO_MEMORY if system physical memory is exhausted.
 * @return ZX_ERR_INVALID_ARGS if `out_vmo` is NULL.
 *
 * @note Capability Lifecycle: On success, caller acquires ownership of `out_vmo`
 *       with full default rights (`ZX_RIGHTS_BASIC`, `ZX_RIGHT_READ`, `ZX_RIGHT_WRITE`,
 *       `ZX_RIGHT_MAP`, `ZX_RIGHT_GET_PROPERTY`, `ZX_RIGHT_SET_PROPERTY`).
 */
zx_status_t iris_vmo_create(size_t size,
                            uint32_t options,
                            zx_handle_t* out_vmo);

/**
 * @brief Map the entirety of a VMO into the current process's root address space.
 *
 * Queries the size of `vmo` and maps it starting at offset 0 into `zx_vmar_root_self()`.
 *
 * @param vmo Valid VMO capability handle with `ZX_RIGHT_MAP` and appropriate read/write rights.
 * @param options VM mapping flags (e.g. `ZX_VM_PERM_READ | ZX_VM_PERM_WRITE`). If 0,
 *                defaults to `ZX_VM_PERM_READ | ZX_VM_PERM_WRITE`.
 * @param[out] out_addr Returns the base virtual address of the mapped region.
 * @param[out] out_mapped_size Optional pointer to receive the mapped byte length.
 *
 * @return ZX_OK on success.
 * @return ZX_ERR_BAD_HANDLE if `vmo` is invalid or lacks necessary rights.
 * @return ZX_ERR_NO_MEMORY if virtual address space is exhausted.
 * @return ZX_ERR_INVALID_ARGS if `out_addr` is NULL.
 */
zx_status_t iris_vmo_map(zx_handle_t vmo,
                         zx_vm_option_t options,
                         uintptr_t* out_addr,
                         size_t* out_mapped_size);

/**
 * @brief Map a specific sub-range of a VMO into a target VMAR.
 *
 * @param vmar Target VMAR handle (pass `ZX_HANDLE_INVALID` to target current root VMAR).
 * @param vmo Valid VMO capability handle.
 * @param vmo_offset Byte offset into the VMO to begin mapping (must be page-aligned).
 * @param len Number of bytes to map (must be page-aligned).
 * @param options VM mapping permissions (`ZX_VM_PERM_READ`, `ZX_VM_PERM_WRITE`, `ZX_VM_PERM_EXECUTE`).
 * @param[out] out_addr Returns the base virtual address of the mapped sub-region.
 *
 * @return ZX_OK on success, or standard Zircon error code.
 */
zx_status_t iris_vmo_map_offset(zx_handle_t vmar,
                                zx_handle_t vmo,
                                uint64_t vmo_offset,
                                size_t len,
                                zx_vm_option_t options,
                                uintptr_t* out_addr);

/**
 * @brief Unmap a previously mapped virtual memory range from the current process's root VMAR.
 *
 * @param addr Base virtual address previously returned by `iris_vmo_map()`.
 * @param size Byte length of the mapping.
 *
 * @return ZX_OK on success.
 * @return ZX_ERR_NOT_FOUND if `addr` is not currently mapped.
 * @return ZX_ERR_INVALID_ARGS if `addr` is unaligned.
 */
zx_status_t iris_vmo_unmap(uintptr_t addr, size_t size);

/**
 * @brief Unmap a virtual memory range from an explicit target VMAR.
 *
 * @param vmar Target VMAR handle.
 * @param addr Base virtual address.
 * @param size Byte length of the mapping.
 *
 * @return ZX_OK on success, or standard Zircon error code.
 */
zx_status_t iris_vmo_unmap_from(zx_handle_t vmar, uintptr_t addr, size_t size);

/**
 * @brief Duplicate a VMO handle with specific rights.
 *
 * Commonly used to produce restricted handles for IPC transfer (e.g. creating
 * a read-only VMO handle to transfer a shared window surface to `iris_compositor`
 * without permitting client processes to modify it unexpectedly).
 *
 * @param vmo Existing VMO handle to duplicate.
 * @param rights Desired rights mask (e.g. `ZX_RIGHT_SAME_RIGHTS`, or `ZX_RIGHTS_BASIC | ZX_RIGHT_READ | ZX_RIGHT_MAP`).
 * @param[out] out_handle Returns the newly duplicated VMO handle.
 *
 * @return ZX_OK on success.
 * @return ZX_ERR_BAD_HANDLE if `vmo` is invalid or lacks `ZX_RIGHT_DUPLICATE`.
 * @return ZX_ERR_ACCESS_DENIED if requested `rights` exceed the source handle's rights.
 *
 * @note Capability Lifecycle: On success, caller acquires ownership of `out_handle`.
 */
zx_status_t iris_vmo_duplicate(zx_handle_t vmo,
                               zx_rights_t rights,
                               zx_handle_t* out_handle);

/**
 * @brief Query the current byte size of a VMO.
 *
 * @param vmo Valid VMO capability handle.
 * @param[out] out_size Returns the size in bytes.
 *
 * @return ZX_OK on success.
 * @return ZX_ERR_BAD_HANDLE if `vmo` is invalid.
 * @return ZX_ERR_INVALID_ARGS if `out_size` is NULL.
 */
zx_status_t iris_vmo_get_size(zx_handle_t vmo, uint64_t* out_size);

/**
 * @brief Resize a VMO to a new capacity.
 *
 * @param vmo Valid VMO capability handle with `ZX_RIGHT_WRITE`.
 * @param new_size New requested capacity in bytes.
 *
 * @return ZX_OK on success.
 * @return ZX_ERR_NO_MEMORY if system resources could not fulfill the expansion.
 * @return ZX_ERR_ACCESS_DENIED if handle lacks `ZX_RIGHT_WRITE`.
 */
zx_status_t iris_vmo_set_size(zx_handle_t vmo, uint64_t new_size);

/**
 * @brief Read bytes directly from a VMO without mapping it into virtual memory.
 *
 * @param vmo Valid VMO capability handle with `ZX_RIGHT_READ`.
 * @param[out] buffer Destination buffer to receive data.
 * @param offset Byte offset into the VMO.
 * @param buffer_size Number of bytes to read.
 *
 * @return ZX_OK on success, or standard Zircon error code.
 */
zx_status_t iris_vmo_read(zx_handle_t vmo,
                          void* buffer,
                          uint64_t offset,
                          size_t buffer_size);

/**
 * @brief Write bytes directly into a VMO without mapping it into virtual memory.
 *
 * @param vmo Valid VMO capability handle with `ZX_RIGHT_WRITE`.
 * @param buffer Source buffer containing data to write.
 * @param offset Byte offset into the VMO.
 * @param buffer_size Number of bytes to write.
 *
 * @return ZX_OK on success, or standard Zircon error code.
 */
zx_status_t iris_vmo_write(zx_handle_t vmo,
                           const void* buffer,
                           uint64_t offset,
                           size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_VMO_H_ */
