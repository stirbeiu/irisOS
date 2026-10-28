/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris_gui/include/iris/gui.h
 * Purpose: Zero-copy shared-memory client windowing library for irisOS.
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

#ifndef IRIS_GUI_H_
#define IRIS_GUI_H_

/**
 * @file gui.h
 * @brief Client windowing API for zero-copy VMO surfaces and Pixman 2D rendering.
 *
 * ARCHITECTURAL DESIGN & ZERO-COPY RENDERING:
 *  Under irisOS's microkernel graphics architecture, client applications never
 *  serialize pixel buffers over IPC channels. Instead:
 *   1. `iris_window_create()` allocates a Zircon Virtual Memory Object (VMO)
 *      sized for `width * height * 4` bytes (32-bit ARGB8888 / XRGB8888) and maps
 *      it into the client's virtual address space (`zx_vmar_map()`).
 *   2. The mapped buffer is wrapped in a 32-bit Pixman image (`PIXMAN_a8r8g8b8`)
 *      so the client can render 2D primitives and FreeType glyphs locally in RAM
 *      at native memory speed.
 *   3. A duplicated capability handle to the VMO is transferred once to
 *      `iris_compositor` via `IRIS_COMPOSITOR_OP_CREATE_WINDOW`.
 *   4. Whenever the client updates a region of its surface, `iris_window_damage()`
 *      sends a lightweight 24-byte + 20-byte IPC damage notification to
 *      `iris_compositor`, which composites the shared VMO onto the hardware
 *      scanout buffer and flushes `virtio_gpu`.
 *
 * CAPABILITY LIFECYCLE:
 *  - `iris_window_create()` creates a new VMO capability, maps it into
 *    `zx_vmar_root_self()`, and transfers a duplicated VMO handle (`ZX_RIGHT_SAME_RIGHTS`)
 *    to `iris_compositor` over the process's compositor IPC channel.
 *  - `iris_window_destroy()` sends `IRIS_COMPOSITOR_OP_DESTROY_WINDOW`, unreferences
 *    the Pixman surface, unmaps the VMAR region, and closes the local VMO handle.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zircon/types.h>
#include <pixman.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Client window object managing a shared Zircon VMO and Pixman 2D surface.
 */
typedef struct iris_window {
    uint32_t        window_id;    /**< Server-assigned window identifier (> 0) */
    uint32_t        width;        /**< Client surface width in pixels */
    uint32_t        height;       /**< Client surface height in pixels */
    uint32_t        stride;       /**< Row stride in bytes (width * 4) */
    zx_handle_t     vmo;          /**< Shared Zircon VMO handle backing pixel surface */
    zx_vaddr_t      vaddr;        /**< Mapped virtual address in client address space */
    uint64_t        vmo_size;     /**< Page-aligned mapped VMO byte size */
    uint32_t*       pixels;       /**< Pointer to 32-bit ARGB8888 pixel array */
    pixman_image_t* surface;      /**< 32-bit Pixman image (PIXMAN_a8r8g8b8) wrapping pixels */
    zx_handle_t     comp_channel; /**< Borrowed IPC channel endpoint to iris_compositor */
    char            title[64];    /**< Null-terminated window title string */
} iris_window_t;

/**
 * @brief Explicitly bind the IPC channel handle used to communicate with `iris_compositor`.
 *
 * If not called explicitly, `libiris_gui` automatically discovers the compositor
 * channel inherited via `PA_HND(PA_USER0, 1)` (or `PA_HND(PA_USER0, 0)`).
 *
 * @param comp_channel Connected channel capability handle to `iris_compositor` (borrowed).
 */
void iris_gui_set_channel(zx_handle_t comp_channel);

/**
 * @brief Retrieve the active IPC channel handle to `iris_compositor`.
 *
 * @return Valid `zx_handle_t` channel if available, or `ZX_HANDLE_INVALID`.
 */
zx_handle_t iris_gui_get_channel(void);

/**
 * @brief Create a new desktop window backed by a zero-copy shared VMO and Pixman surface.
 *
 * Allocates a Zircon VMO of size `width * height * 4` bytes (page-aligned), maps it
 * into the current process address space via `zx_vmar_map()`, wraps the buffer in a
 * 32-bit `PIXMAN_a8r8g8b8` Pixman image, and sends a duplicated VMO handle to
 * `iris_compositor` via `IRIS_COMPOSITOR_OP_CREATE_WINDOW`.
 *
 * @param title  Null-terminated UTF-8 window title string displayed in the 24px title bar.
 * @param width  Client surface width in pixels (must be > 0).
 * @param height Client surface height in pixels (must be > 0).
 *
 * @return Pointer to allocated `iris_window_t` on success, or `NULL` on failure.
 * @note Capability Lifecycle: Creates and retains `win->vmo`, and transfers a duplicate
 *       handle to `iris_compositor`. Caller must eventually release via `iris_window_destroy()`.
 */
iris_window_t* iris_window_create(const char *title, uint32_t width, uint32_t height);

/**
 * @brief Destroy a client window, unmap its shared VMO, and release its Pixman surface.
 *
 * Issues `IRIS_COMPOSITOR_OP_DESTROY_WINDOW` to `iris_compositor`, unreferences the
 * Pixman image, unmaps the pixel buffer from `zx_vmar_root_self()`, closes the VMO
 * handle, and frees the `iris_window_t` structure.
 *
 * @param win Pointer to the window returned by `iris_window_create()` (safe if NULL).
 */
void iris_window_destroy(iris_window_t *win);

/**
 * @brief Notify `iris_compositor` that a rectangular region of the window surface was modified.
 *
 * Triggers `IRIS_COMPOSITOR_OP_DAMAGE` over IPC so the compositor blends the updated
 * window surface onto the root framebuffer using Pixman and flushes `virtio_gpu`.
 *
 * @param win Pointer to target `iris_window_t`.
 * @param x   Top-left X coordinate of dirty rectangle relative to client surface.
 * @param y   Top-left Y coordinate of dirty rectangle relative to client surface.
 * @param w   Width of dirty rectangle in pixels (0 = full window width).
 * @param h   Height of dirty rectangle in pixels (0 = full window height).
 */
void iris_window_damage(iris_window_t *win, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/**
 * @brief Retrieve the 32-bit Pixman image (`PIXMAN_a8r8g8b8`) wrapping the window's VMO buffer.
 *
 * @param win Pointer to `iris_window_t`.
 * @return `pixman_image_t*` surface pointer, or `NULL` if `win` is NULL.
 */
pixman_image_t* iris_window_get_surface(iris_window_t *win);

/**
 * @brief Move a window to new desktop coordinates `(x, y)`.
 *
 * Sends `IRIS_COMPOSITOR_OP_MOVE_WINDOW` to `iris_compositor` and triggers a
 * desktop re-composite for both the old and new window bounds.
 *
 * @param win Pointer to target `iris_window_t`.
 * @param x   New desktop X coordinate in pixels.
 * @param y   New desktop Y coordinate in pixels.
 * @return `ZX_OK` on success, or negative `zx_status_t` error code.
 */
zx_status_t iris_window_move(iris_window_t *win, int32_t x, int32_t y);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_GUI_H_ */
