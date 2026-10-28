/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_compositor/src/main.c
 * Purpose: Zero-Copy VMO Window Compositor Daemon (bin/iris_compositor).
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 system server.
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
 * @brief Ring 3 Zero-Copy VMO Window Compositor (`bin/iris_compositor`).
 *
 * ARCHITECTURAL LIFECYCLE & SUBSYSTEM INTERACTION:
 *  1. Startup Capability Extraction:
 *     - Receives its compositor IPC server channel at `PA_HND(PA_USER0, 0)`,
 *       its `virtio_gpu` driver client channel at `PA_HND(PA_USER1, 0)`,
 *       its VFS client channel at `PA_HND(PA_NS_DIR, 0)` (for `/System/Resources/Fonts/Inter.otf`),
 *       and (in GUI mode) the Root Resource at `PA_HND(PA_RESOURCE, 0)` for VirtIO Input & UART.
 *  2. Scanout Framebuffer Mapping (`IRIS_PROTO_DRIVER`):
 *     - Issues `IRIS_GPU_OP_GET_INFO` to `virtio_gpu` to retrieve active scanout
 *       dimensions (`1280x800` default) and a duplicated handle to the BTI-pinned
 *       shared framebuffer VMO.
 *     - Maps the shared scanout VMO into `iris_compositor`'s address space via
 *       `zx_vmar_map()` and wraps both a local backbuffer and the hardware scanout
 *       in 32-bit Pixman images (`PIXMAN_x8r8g8b8` / `PIXMAN_a8r8g8b8`).
 *  3. FreeType 2 Typography (`/System/Resources/Fonts/Inter.otf`):
 *     - Initializes FreeType 2 with `/System/Resources/Fonts/Inter.otf` and renders
 *       anti-aliased text onto the top desktop menu bar, window title bars, and UI surfaces.
 *  4. Pointer & Keyboard Input Routing:
 *     - Binds all PCIe `virtio-input` devices (`virtio-keyboard-pci` and `virtio-tablet-pci`)
 *       and runs a UART serial reader worker (`zx_debug_read`).
 *     - Tracks absolute tablet pointer coordinates `(g_cursor_x, g_cursor_y)`, handles
 *       window focus and title-bar dragging, composites a 32x32 RGBA classic macOS arrow
 *       cursor sprite (solid black arrow with crisp 1px white border) onto dirty damage
 *       regions before calling `flush_gpu_scanout()`, and routes keyboard events to the
 *       active focused window.
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

#include <pixman.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <iris/ipc.h>
#include <iris/channel.h>
#include <iris/init.h>
#include <iris/pci.h>
#include <iris/log.h>
#include <servers/drivers/pci/pci.h>
#include <servers/drivers/virtio_input/include/virtio_input.h>

#ifndef ZX_MSEC
#define ZX_MSEC(n) ((zx_duration_t)(n) * 1000000ULL)
#endif

#define COMP_PAGE_SIZE          4096ULL
#define COMP_PAGE_MASK          (~(COMP_PAGE_SIZE - 1ULL))
#define MAX_COMP_CLIENTS        32
#define MAX_COMP_WINDOWS        32
#define MAX_INPUT_DEVS          4
#define WIN_KEY_FIFO_MAX        256
#define CURSOR_SPRITE_W         32
#define CURSOR_SPRITE_H         32

/* Window frame decoration constants */
#define IRIS_WIN_BORDER_PX      1
#define IRIS_WIN_TITLEBAR_H     24
#define IRIS_TOPBAR_H           28

#define INTER_FONT_PATH         "/System/Resources/Fonts/Inter.otf"

/**
 * @brief Compositor window entry in Z-order (index 0 = bottom, g_num_windows - 1 = top/focused).
 */
typedef struct comp_window {
    uint32_t        window_id;      /**< Unique window ID (> 0) */
    int             owner_client;   /**< Client slot index that created this window */
    int32_t         x;              /**< Desktop X coordinate of outer window frame */
    int32_t         y;              /**< Desktop Y coordinate of outer window frame */
    uint32_t        client_w;       /**< Client surface width in pixels */
    uint32_t        client_h;       /**< Client surface height in pixels */
    uint32_t        flags;          /**< Window flags */
    zx_handle_t     vmo;            /**< Mapped shared client VMO handle */
    zx_vaddr_t      vaddr;          /**< Mapped virtual address of client pixels */
    uint64_t        vmo_size;       /**< Mapped VMO byte size */
    pixman_image_t* client_surface; /**< Pixman image wrapping client VMO */
    char            title[64];      /**< Window title string */

    /* Per-window keyboard input FIFO */
    char            key_fifo[WIN_KEY_FIFO_MAX];
    uint32_t        key_head;
    uint32_t        key_tail;
    uint32_t        key_count;
} comp_window_t;

/* Global Compositor State */
static zx_handle_t       g_server_channel    = ZX_HANDLE_INVALID;
static zx_handle_t       g_gpu_channel       = ZX_HANDLE_INVALID;
static zx_handle_t       g_root_resource     = ZX_HANDLE_INVALID;
static uint32_t          g_gpu_txid          = 1u;

static uint32_t          g_fb_width          = 1280u;
static uint32_t          g_fb_height         = 800u;
static uint32_t          g_fb_pitch          = 5120u;
static uint64_t          g_fb_size           = 0;
static zx_handle_t       g_fb_vmo            = ZX_HANDLE_INVALID;
static zx_vaddr_t        g_fb_vaddr          = 0;

static uint32_t*         g_backbuf_pixels    = NULL;
static pixman_image_t*   g_backbuf_surface   = NULL;
static pixman_image_t*   g_scanout_surface   = NULL;
static pixman_image_t*   g_wallpaper_surface = NULL;

/* 32x32 RGBA Software Cursor State */
static uint32_t*         g_cursor_pixels     = NULL;
static pixman_image_t*   g_cursor_surface    = NULL;
static int32_t           g_cursor_x          = 640;
static int32_t           g_cursor_y          = 400;
static bool              g_btn_left_down     = false;
static uint32_t          g_drag_window_id    = 0;
static bool              g_drag_welcome      = false;
static int32_t           g_drag_off_x        = 0;
static int32_t           g_drag_off_y        = 0;

/* Default welcome window shown when no client windows are open */
static uint32_t*         g_welcome_pixels    = NULL;
static pixman_image_t*   g_welcome_surface   = NULL;
static comp_window_t     g_welcome_win;
static char              g_welcome_typed[48];
static size_t            g_welcome_typed_len = 0;

/* FreeType 2 State */
static FT_Library        g_ft_lib            = NULL;
static FT_Face           g_ft_face           = NULL;

/* VirtIO Input & UART Serial State */
static virtio_input_dev_t g_input_devs[MAX_INPUT_DEVS];
static size_t            g_num_input_devs    = 0;
static volatile bool     g_shutdown          = false;
static volatile uint8_t  g_uart_lock         = 0;
static char              g_uart_fifo[256];
static uint32_t          g_uart_head         = 0;
static uint32_t          g_uart_tail         = 0;
static uint32_t          g_uart_count        = 0;

static comp_window_t     g_windows[MAX_COMP_WINDOWS];
static size_t            g_num_windows       = 0;
static uint32_t          g_next_window_id    = 1u;

/* ==============================================================================
 * Embedded 8x8 Bitmap Font (Fallback) & FreeType 2 Anti-Aliased Text Renderer
 * ============================================================================== */

static const uint8_t k_font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* '!' */
    {0x6C,0x6C,0x48,0x00,0x00,0x00,0x00,0x00}, /* '"' */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* '#' */
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, /* '$' */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* '%' */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* '&' */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ''' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* '(' */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* ')' */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* '*' */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* '+' */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* ',' */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* '.' */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* '/' */
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00}, /* '0' */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* '1' */
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, /* '2' */
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, /* '3' */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* '4' */
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, /* '5' */
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, /* '6' */
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, /* '7' */
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, /* '8' */
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, /* '9' */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* ':' */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ';' */
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, /* '<' */
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00}, /* '=' */
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, /* '>' */
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, /* '?' */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, /* '@' */
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* 'A' */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* 'B' */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* 'C' */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* 'D' */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* 'E' */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* 'F' */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* 'G' */
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* 'H' */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 'I' */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* 'J' */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* 'K' */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* 'L' */
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* 'M' */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* 'N' */
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* 'O' */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* 'P' */
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06}, /* 'Q' */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* 'R' */
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, /* 'S' */
    {0x7E,0x7E,0x5A,0x18,0x18,0x18,0x3C,0x00}, /* 'T' */
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* 'U' */
    {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* 'V' */
    {0xC6,0xC6,0xD6,0xFE,0xFE,0xEE,0xC6,0x00}, /* 'W' */
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, /* 'X' */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, /* 'Y' */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* 'Z' */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* '[' */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* '\' */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ']' */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* '_' */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* '`' */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* 'a' */
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, /* 'b' */
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, /* 'c' */
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, /* 'd' */
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, /* 'e' */
    {0x3C,0x66,0x60,0xF8,0x60,0x60,0xF0,0x00}, /* 'f' */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /* 'g' */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* 'h' */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* 'i' */
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C}, /* 'j' */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* 'k' */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 'l' */
    {0x00,0x00,0xEC,0xFE,0xD6,0xC6,0xC6,0x00}, /* 'm' */
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, /* 'n' */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, /* 'o' */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /* 'p' */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /* 'q' */
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, /* 'r' */
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, /* 's' */
    {0x30,0x30,0xFC,0x30,0x30,0x36,0x1C,0x00}, /* 't' */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* 'u' */
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* 'v' */
    {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00}, /* 'w' */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* 'x' */
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0xFC}, /* 'y' */
    {0x00,0x00,0xFE,0x8C,0x18,0x32,0xFE,0x00}, /* 'z' */
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /* '{' */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* '|' */
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /* '}' */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, /* '~' */
};

static void draw_string_8x8(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                            int32_t x, int32_t y, const char* str, uint32_t color) {
    if (!pixels || !str || y < 0 || (uint32_t)(y + 8) > buf_h) {
        return;
    }

    int32_t cx = x;
    for (size_t i = 0; str[i] != '\0'; ++i) {
        uint8_t ch = (uint8_t)str[i];
        if (ch < 32 || ch > 126) {
            ch = '?';
        }
        const uint8_t* glyph = k_font8x8[ch - 32];
        for (int row = 0; row < 8; ++row) {
            int32_t py = y + row;
            if (py < 0 || (uint32_t)py >= buf_h) continue;
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; ++col) {
                if (bits & (0x80u >> col)) {
                    int32_t px = cx + col;
                    if (px >= 0 && (uint32_t)px < buf_w) {
                        pixels[(size_t)py * buf_w + (size_t)px] = color;
                    }
                }
            }
        }
        cx += 8;
        if ((uint32_t)cx >= buf_w) {
            break;
        }
    }
}

/**
 * @brief Render an anti-aliased string using FreeType 2 (`Inter.otf`) onto an ARGB8888 buffer.
 *        Falls back to `draw_string_8x8` if `g_ft_face` is not loaded.
 */
static void draw_text_ft(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                         int32_t x, int32_t y, uint32_t pixel_size,
                         const char* str, uint32_t argb_color) {
    if (!pixels || !str) {
        return;
    }
    if (!g_ft_face) {
        draw_string_8x8(pixels, buf_w, buf_h, x, y, str, argb_color);
        return;
    }

    FT_Set_Pixel_Sizes(g_ft_face, 0, pixel_size);

    uint32_t fg_r = (argb_color >> 16) & 0xFFu;
    uint32_t fg_g = (argb_color >>  8) & 0xFFu;
    uint32_t fg_b = (argb_color >>  0) & 0xFFu;

    int32_t pen_x      = x;
    int32_t baseline_y = y + (int32_t)pixel_size - 2;

    for (size_t i = 0; str[i] != '\0'; ++i) {
        uint8_t ch = (uint8_t)str[i];
        if (ch < 32) {
            continue;
        }
        if (FT_Load_Char(g_ft_face, (FT_ULong)ch, FT_LOAD_RENDER) != 0) {
            continue;
        }

        FT_GlyphSlot slot = g_ft_face->glyph;
        FT_Bitmap*   bmp  = &slot->bitmap;

        if (bmp->buffer != NULL && bmp->width > 0 && bmp->rows > 0) {
            int pitch = bmp->pitch;
            if (pitch < 0) pitch = -pitch;

            for (unsigned int row = 0; row < bmp->rows; ++row) {
                int32_t dst_y = baseline_y - slot->bitmap_top + (int32_t)row;
                if (dst_y < 0 || (uint32_t)dst_y >= buf_h) {
                    continue;
                }
                const uint8_t* src_row = bmp->buffer + (row * (unsigned int)pitch);
                uint32_t*      dst_row = pixels + ((size_t)dst_y * buf_w);

                for (unsigned int col = 0; col < bmp->width; ++col) {
                    int32_t dst_x = pen_x + slot->bitmap_left + (int32_t)col;
                    if (dst_x < 0 || (uint32_t)dst_x >= buf_w) {
                        continue;
                    }
                    uint32_t cov = src_row[col];
                    if (cov == 0) {
                        continue;
                    }
                    if (cov >= 255u) {
                        dst_row[dst_x] = 0xFF000000u | (fg_r << 16) | (fg_g << 8) | fg_b;
                    } else {
                        uint32_t bg    = dst_row[dst_x];
                        uint32_t bg_r  = (bg >> 16) & 0xFFu;
                        uint32_t bg_g  = (bg >>  8) & 0xFFu;
                        uint32_t bg_b  = (bg >>  0) & 0xFFu;
                        uint32_t inv   = 255u - cov;
                        uint32_t out_r = (fg_r * cov + bg_r * inv + 127u) / 255u;
                        uint32_t out_g = (fg_g * cov + bg_g * inv + 127u) / 255u;
                        uint32_t out_b = (fg_b * cov + bg_b * inv + 127u) / 255u;
                        dst_row[dst_x] = 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
                    }
                }
            }
        }

        pen_x += (int32_t)(slot->advance.x >> 6);
        if (pen_x >= (int32_t)buf_w) {
            break;
        }
    }
}

/* ==============================================================================
 * 32x32 RGBA Classic macOS Arrow Cursor Sprite
 * ============================================================================== */

/**
 * 32x32 RGBA software cursor sprite representing a classic macOS arrow:
 *   - 'W': Crisp 1px white border (0xFFFFFFFF)
 *   - 'B': Solid black arrow interior (0xFF000000)
 *   - '.': Transparent background (0x00000000)
 */
static const char* const k_macos_cursor_32x32[CURSOR_SPRITE_H] = {
    "W...............................",
    "WW..............................",
    "WBW.............................",
    "WBBW............................",
    "WBBBW...........................",
    "WBBBBW..........................",
    "WBBBBBW.........................",
    "WBBBBBBW........................",
    "WBBBBBBBW.......................",
    "WBBBBBBBBW......................",
    "WBBBBBBBBBW.....................",
    "WBBBBBBBBBBW....................",
    "WBBBBBBBBBBBW...................",
    "WBBBBBBBBBBBBW..................",
    "WBBBBBBBBBBBBBW.................",
    "WBBBBBBBBBBBBBBW................",
    "WBBBBBBBBBBBBBBBW...............",
    "WBBBBBBBBWWWWWWWW...............",
    "WBBBBBBBW.......................",
    "WBBBBWBBBW......................",
    "WBBBW.WBBBW.....................",
    "WBBW..WBBBW.....................",
    "WBW....WBBBW....................",
    "WW.....WBBBW....................",
    "W.......WBBBW...................",
    "........WBBBW...................",
    ".........WBBW...................",
    ".........WWWW...................",
    "................................",
    "................................",
    "................................",
    "................................"
};

static zx_status_t init_cursor_sprite(void) {
    g_cursor_pixels = (uint32_t*)calloc((size_t)CURSOR_SPRITE_W * CURSOR_SPRITE_H, sizeof(uint32_t));
    if (!g_cursor_pixels) {
        return ZX_ERR_NO_MEMORY;
    }

    for (uint32_t y = 0; y < CURSOR_SPRITE_H; ++y) {
        const char* row = k_macos_cursor_32x32[y];
        for (uint32_t x = 0; x < CURSOR_SPRITE_W; ++x) {
            char c = row[x];
            uint32_t pixel = 0x00000000u;
            if (c == 'W') {
                pixel = 0xFFFFFFFFu; /* Crisp 1px white border */
            } else if (c == 'B') {
                pixel = 0xFF000000u; /* Solid black arrow interior */
            }
            g_cursor_pixels[y * CURSOR_SPRITE_W + x] = pixel;
        }
    }

    g_cursor_surface = pixman_image_create_bits(PIXMAN_a8r8g8b8,
                                                CURSOR_SPRITE_W,
                                                CURSOR_SPRITE_H,
                                                g_cursor_pixels,
                                                CURSOR_SPRITE_W * 4);
    if (!g_cursor_surface) {
        free(g_cursor_pixels);
        g_cursor_pixels = NULL;
        return ZX_ERR_NO_MEMORY;
    }

    g_cursor_x = (int32_t)(g_fb_width  / 2u);
    g_cursor_y = (int32_t)(g_fb_height / 2u);
    return ZX_OK;
}

/**
 * @brief Convert a 32-bit ARGB8888 color (0xAARRGGBB) to a `pixman_color_t`.
 */
static inline pixman_color_t argb_to_pixman_color(uint32_t argb) {
    uint16_t a = (uint16_t)(((argb >> 24) & 0xFFu) * 257u);
    uint16_t r = (uint16_t)(((argb >> 16) & 0xFFu) * 257u);
    uint16_t g = (uint16_t)(((argb >>  8) & 0xFFu) * 257u);
    uint16_t b = (uint16_t)(((argb >>  0) & 0xFFu) * 257u);
    pixman_color_t c = { r, g, b, a };
    return c;
}

/**
 * @brief Fill a solid rectangle on a Pixman image using `pixman_image_fill_rectangles`.
 */
static void fill_rect_pixman(pixman_image_t* dst, int32_t x, int32_t y,
                             uint32_t w, uint32_t h, uint32_t argb) {
    if (!dst || w == 0 || h == 0) {
        return;
    }
    pixman_color_t col = argb_to_pixman_color(argb);
    pixman_rectangle16_t rect = {
        (int16_t)x,
        (int16_t)y,
        (uint16_t)w,
        (uint16_t)h
    };
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, dst, &col, 1, &rect);
}

/* ==============================================================================
 * VirtIO GPU Driver IPC Helpers
 * ============================================================================== */

/**
 * @brief Connect to `virtio_gpu` via `IRIS_GPU_OP_GET_INFO` and map the shared scanout VMO.
 */
static zx_status_t connect_to_virtio_gpu(void) {
    if (g_gpu_channel == ZX_HANDLE_INVALID) {
        return ZX_ERR_BAD_HANDLE;
    }

    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_t resp_hdr;
    iris_gpu_info_t info;
    memset(&info, 0, sizeof(info));

    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_DRIVER, IRIS_GPU_OP_GET_INFO, ++g_gpu_txid, 0);

    uint32_t act_bytes = 0;
    uint32_t act_handles = 0;
    zx_handle_t fb_vmo = ZX_HANDLE_INVALID;

    zx_status_t st = iris_channel_call(g_gpu_channel,
                                       &req_hdr, NULL,
                                       NULL, 0,
                                       &resp_hdr, &info, sizeof(info), &act_bytes,
                                       &fb_vmo, 1, &act_handles,
                                       ZX_TIME_INFINITE);
    if (st != ZX_OK || resp_hdr.status != ZX_OK || fb_vmo == ZX_HANDLE_INVALID) {
        log_status(STATUS_FAIL, "iris_compositor", "Failed to query virtio_gpu scanout info (st=%d, resp=%d).",
                   st, resp_hdr.status);
        return (st != ZX_OK) ? st : resp_hdr.status;
    }

    g_fb_width  = (info.width  > 0) ? info.width  : 1280u;
    g_fb_height = (info.height > 0) ? info.height : 800u;
    g_fb_pitch  = (info.pitch  > 0) ? info.pitch  : (g_fb_width * 4u);
    g_fb_size   = (info.fb_size > 0) ? info.fb_size : ((uint64_t)g_fb_pitch * g_fb_height);
    g_fb_vmo    = fb_vmo;

    st = zx_vmar_map(zx_vmar_root_self(),
                     ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                     0, g_fb_vmo, 0, g_fb_size, &g_fb_vaddr);
    if (st != ZX_OK || g_fb_vaddr == 0) {
        log_status(STATUS_FAIL, "iris_compositor", "zx_vmar_map for scanout VMO failed (%d).", st);
        zx_handle_close(g_fb_vmo);
        g_fb_vmo = ZX_HANDLE_INVALID;
        return st;
    }

    g_scanout_surface = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                                 (int)g_fb_width,
                                                 (int)g_fb_height,
                                                 (uint32_t*)g_fb_vaddr,
                                                 (int)g_fb_pitch);

    g_backbuf_pixels = (uint32_t*)calloc((size_t)g_fb_width * g_fb_height, sizeof(uint32_t));
    if (!g_backbuf_pixels) {
        return ZX_ERR_NO_MEMORY;
    }
    g_backbuf_surface = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                                 (int)g_fb_width,
                                                 (int)g_fb_height,
                                                 g_backbuf_pixels,
                                                 (int)g_fb_pitch);

    if (!g_scanout_surface || !g_backbuf_surface) {
        return ZX_ERR_NO_MEMORY;
    }

    log_status(STATUS_OKAY, "iris_compositor", "Connected to virtio_gpu scanout (%ux%u, pitch %u, mapped shared VMO).",
               g_fb_width, g_fb_height, g_fb_pitch);
    return ZX_OK;
}

/**
 * @brief Send `IRIS_GPU_OP_FLUSH` to `virtio_gpu` for a damaged screen rectangle.
 */
static zx_status_t flush_gpu_scanout(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (g_gpu_channel == ZX_HANDLE_INVALID) {
        return ZX_ERR_BAD_HANDLE;
    }

    __sync_synchronize();

    iris_gpu_flush_req_t freq;
    freq.x      = x;
    freq.y      = y;
    freq.width  = w;
    freq.height = h;

    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_t resp_hdr;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_DRIVER, IRIS_GPU_OP_FLUSH, ++g_gpu_txid, sizeof(freq));

    return iris_channel_call(g_gpu_channel,
                             &req_hdr, &freq,
                             NULL, 0,
                             &resp_hdr, NULL, 0, NULL,
                             NULL, 0, NULL,
                             ZX_TIME_INFINITE);
}

/* ==============================================================================
 * Desktop Wallpaper, Window Decorations & Pixman Compositing Pipeline
 * ============================================================================== */

static void render_welcome_surface_contents(void) {
    if (!g_welcome_surface || !g_welcome_pixels) {
        return;
    }
    uint32_t ww = g_welcome_win.client_w;
    uint32_t wh = g_welcome_win.client_h;

    fill_rect_pixman(g_welcome_surface, 0, 0, ww, wh, 0xFF1E1E2Eu);
    fill_rect_pixman(g_welcome_surface, 16, 16, ww - 32u, 38u, 0xFF313244u);

    draw_text_ft(g_welcome_pixels, ww, wh, 28, 26, 15,
                 "irisOS Zero-Copy Window Compositor (1280x800)", 0xFF89B4FAu);
    draw_text_ft(g_welcome_pixels, ww, wh, 24, 72, 13,
                 "Protocol:     IRIS_PROTO_COMPOSITOR (24-byte IPC)", 0xFFCDD6F4u);
    draw_text_ft(g_welcome_pixels, ww, wh, 24, 96, 13,
                 "Typography:   FreeType 2 + Inter-Regular.otf", 0xFFCDD6F4u);
    draw_text_ft(g_welcome_pixels, ww, wh, 24, 120, 13,
                 "Input:        VirtIO Tablet (EV_ABS) + Keyboard + UART", 0xFFA6E3A1u);
    draw_text_ft(g_welcome_pixels, ww, wh, 24, 144, 13,
                 "Interaction:  Drag title bar with mouse or type below", 0xFFF9E2AFu);

    /* Input echo box for testing live keyboard/UART routing */
    fill_rect_pixman(g_welcome_surface, 24, 174, ww - 48u, 32u, 0xFF181825u);
    char input_line[80];
    snprintf(input_line, sizeof(input_line), "Focused Input: %s_", g_welcome_typed);
    draw_text_ft(g_welcome_pixels, ww, wh, 32, 183, 13, input_line, 0xFFF5E0DCu);

    /* Subtle accent bar at bottom of welcome surface */
    fill_rect_pixman(g_welcome_surface, 24, wh - 20u, ww - 48u, 4u, 0xFF89B4FAu);
}

/**
 * @brief Pre-render the Catppuccin Mocha / Slate gradient desktop wallpaper and top bar.
 */
static zx_status_t init_desktop_wallpaper(void) {
    uint32_t* wp_pixels = (uint32_t*)calloc((size_t)g_fb_width * g_fb_height, sizeof(uint32_t));
    if (!wp_pixels) {
        return ZX_ERR_NO_MEMORY;
    }

    g_wallpaper_surface = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                                   (int)g_fb_width,
                                                   (int)g_fb_height,
                                                   wp_pixels,
                                                   (int)g_fb_pitch);
    if (!g_wallpaper_surface) {
        free(wp_pixels);
        return ZX_ERR_NO_MEMORY;
    }

    /* Create a smooth diagonal/vertical linear gradient across the desktop using Pixman */
    pixman_point_fixed_t p1 = { pixman_int_to_fixed(0), pixman_int_to_fixed(0) };
    pixman_point_fixed_t p2 = { pixman_int_to_fixed((int)g_fb_width), pixman_int_to_fixed((int)g_fb_height) };

    pixman_gradient_stop_t stops[3] = {
        { 0x00000, argb_to_pixman_color(0xFF181825u) }, /* Catppuccin Mantle */
        { 0x08000, argb_to_pixman_color(0xFF1E1E2Eu) }, /* Catppuccin Base   */
        { 0x10000, argb_to_pixman_color(0xFF313244u) }  /* Catppuccin Surface0 */
    };

    pixman_image_t* grad = pixman_image_create_linear_gradient(&p1, &p2, stops, 3);
    if (grad) {
        pixman_image_composite32(PIXMAN_OP_SRC,
                                 grad, NULL, g_wallpaper_surface,
                                 0, 0, 0, 0, 0, 0,
                                 (int)g_fb_width, (int)g_fb_height);
        pixman_image_unref(grad);
    } else {
        fill_rect_pixman(g_wallpaper_surface, 0, 0, g_fb_width, g_fb_height, 0xFF1E1E2Eu);
    }

    /* Render 28px top desktop menu bar (#11111B) with 1px bottom border (#45475A) */
    fill_rect_pixman(g_wallpaper_surface, 0, 0, g_fb_width, IRIS_TOPBAR_H - 1, 0xFF11111Bu);
    fill_rect_pixman(g_wallpaper_surface, 0, IRIS_TOPBAR_H - 1, g_fb_width, 1, 0xFF45475Au);

    draw_text_ft(wp_pixels, g_fb_width, g_fb_height,
                 16, 7, 14, "irisOS", 0xFF89B4FAu);
    draw_text_ft(wp_pixels, g_fb_width, g_fb_height,
                 84, 7, 13, "Desktop     System     Window     Help", 0xFFCDD6F4u);
    if (g_fb_width >= 320u) {
        draw_text_ft(wp_pixels, g_fb_width, g_fb_height,
                     (int32_t)g_fb_width - 230, 7, 13, "1280x800  |  Zircon 1011", 0xFFA6ADC8u);
    }

    /* Initialize default welcome window surface (shown when 0 client windows are open) */
    uint32_t ww = 520u;
    uint32_t wh = 236u;
    g_welcome_pixels = (uint32_t*)calloc((size_t)ww * wh, sizeof(uint32_t));
    if (g_welcome_pixels) {
        g_welcome_surface = pixman_image_create_bits(PIXMAN_a8r8g8b8,
                                                     (int)ww, (int)wh,
                                                     g_welcome_pixels,
                                                     (int)(ww * 4u));
        if (g_welcome_surface) {
            memset(&g_welcome_win, 0, sizeof(g_welcome_win));
            g_welcome_win.window_id      = 0xFFFFu;
            g_welcome_win.client_w       = ww;
            g_welcome_win.client_h       = wh;
            g_welcome_win.x              = (int32_t)(g_fb_width  - (ww + 2u * IRIS_WIN_BORDER_PX)) / 2;
            g_welcome_win.y              = (int32_t)(g_fb_height - (wh + IRIS_WIN_TITLEBAR_H + 2u * IRIS_WIN_BORDER_PX)) / 2;
            g_welcome_win.client_surface = g_welcome_surface;
            memcpy(g_welcome_win.title, "irisOS Compositor", 18);

            render_welcome_surface_contents();
        }
    }

    return ZX_OK;
}

/**
 * @brief Render a single window's decorations (1px border + 24px title bar + title text)
 *        and composite its client surface onto `g_backbuf_surface`.
 */
static void composite_window(const comp_window_t* win, bool is_focused) {
    if (!win || !win->client_surface) {
        return;
    }

    uint32_t frame_w = win->client_w + (2u * IRIS_WIN_BORDER_PX);
    uint32_t frame_h = win->client_h + IRIS_WIN_TITLEBAR_H + (2u * IRIS_WIN_BORDER_PX);

    uint32_t border_color   = is_focused ? 0xFF89B4FAu : 0xFF585B70u; /* Lavender vs Surface2 */
    uint32_t titlebar_color = is_focused ? 0xFF313244u : 0xFF181825u; /* Surface0 vs Mantle */
    uint32_t title_fg_color = is_focused ? 0xFFCDD6F4u : 0xFF9399B2u; /* Text vs Overlay2 */

    /* 1. Outer 1px border around entire window frame */
    fill_rect_pixman(g_backbuf_surface, win->x, win->y, frame_w, IRIS_WIN_BORDER_PX, border_color);
    fill_rect_pixman(g_backbuf_surface, win->x, win->y + (int32_t)frame_h - IRIS_WIN_BORDER_PX,
                     frame_w, IRIS_WIN_BORDER_PX, border_color);
    fill_rect_pixman(g_backbuf_surface, win->x, win->y, IRIS_WIN_BORDER_PX, frame_h, border_color);
    fill_rect_pixman(g_backbuf_surface, win->x + (int32_t)frame_w - IRIS_WIN_BORDER_PX, win->y,
                     IRIS_WIN_BORDER_PX, frame_h, border_color);

    /* 2. 24px Title Bar inside the 1px border */
    int32_t tb_x = win->x + IRIS_WIN_BORDER_PX;
    int32_t tb_y = win->y + IRIS_WIN_BORDER_PX;
    fill_rect_pixman(g_backbuf_surface, tb_x, tb_y, win->client_w, IRIS_WIN_TITLEBAR_H - 1, titlebar_color);
    /* 1px separator line between title bar and client surface */
    fill_rect_pixman(g_backbuf_surface, tb_x, tb_y + IRIS_WIN_TITLEBAR_H - 1,
                     win->client_w, 1, border_color);

    /* Window control buttons (Close #F38BA8, Minimize #F9E2AF, Maximize #A6E3A1) */
    fill_rect_pixman(g_backbuf_surface, tb_x + 10, tb_y + 8, 8, 8, 0xFFF38BA8u);
    fill_rect_pixman(g_backbuf_surface, tb_x + 24, tb_y + 8, 8, 8, 0xFFF9E2AFu);
    fill_rect_pixman(g_backbuf_surface, tb_x + 38, tb_y + 8, 8, 8, 0xFFA6E3A1u);

    /* Window title string rendered in title bar using Inter font */
    draw_text_ft(g_backbuf_pixels, g_fb_width, g_fb_height,
                 tb_x + 58, tb_y + 5, 13, win->title, title_fg_color);

    /* 3. Blend client VMO surface onto backbuffer using Pixman */
    int32_t client_dst_x = win->x + IRIS_WIN_BORDER_PX;
    int32_t client_dst_y = win->y + IRIS_WIN_BORDER_PX + IRIS_WIN_TITLEBAR_H;

    pixman_image_composite32(PIXMAN_OP_OVER,
                             win->client_surface,
                             NULL,
                             g_backbuf_surface,
                             0, 0,
                             0, 0,
                             client_dst_x, client_dst_y,
                             (int)win->client_w, (int)win->client_h);
}

/**
 * @brief Copy a damaged region from `g_backbuf_surface` to `g_scanout_surface`,
 *        composite the 32x32 macOS pointer cursor on top, and flush `virtio_gpu`.
 */
static void present_region_with_cursor(int32_t dmg_x, int32_t dmg_y, uint32_t dmg_w, uint32_t dmg_h) {
    if (!g_backbuf_surface || !g_scanout_surface) {
        return;
    }

    if (dmg_x < 0) {
        if ((int32_t)dmg_w + dmg_x <= 0) return;
        dmg_w = (uint32_t)((int32_t)dmg_w + dmg_x);
        dmg_x = 0;
    }
    if (dmg_y < 0) {
        if ((int32_t)dmg_h + dmg_y <= 0) return;
        dmg_h = (uint32_t)((int32_t)dmg_h + dmg_y);
        dmg_y = 0;
    }
    if ((uint32_t)dmg_x >= g_fb_width || (uint32_t)dmg_y >= g_fb_height) {
        return;
    }
    if (dmg_w == 0 || (uint32_t)dmg_x + dmg_w > g_fb_width) {
        dmg_w = g_fb_width - (uint32_t)dmg_x;
    }
    if (dmg_h == 0 || (uint32_t)dmg_y + dmg_h > g_fb_height) {
        dmg_h = g_fb_height - (uint32_t)dmg_y;
    }

    /* 1. Copy clean scene from backbuffer to shared scanout VMO */
    pixman_image_composite32(PIXMAN_OP_SRC,
                             g_backbuf_surface,
                             NULL,
                             g_scanout_surface,
                             dmg_x, dmg_y,
                             0, 0,
                             dmg_x, dmg_y,
                             (int)dmg_w, (int)dmg_h);

    /* 2. Composite 32x32 RGBA macOS arrow cursor on top of scanout before flushing */
    if (g_cursor_surface != NULL) {
        pixman_image_composite32(PIXMAN_OP_OVER,
                                 g_cursor_surface,
                                 NULL,
                                 g_scanout_surface,
                                 0, 0,
                                 0, 0,
                                 g_cursor_x, g_cursor_y,
                                 CURSOR_SPRITE_W, CURSOR_SPRITE_H);
    }

    /* 3. Flush damaged rectangle to VirtIO GPU scanout */
    flush_gpu_scanout((uint32_t)dmg_x, (uint32_t)dmg_y, dmg_w, dmg_h);
}

/**
 * @brief Composite the desktop wallpaper and all active windows in Z-order,
 *        composite the software cursor on top, and flush `virtio_gpu`.
 */
static void composite_and_flush(int32_t dmg_x, int32_t dmg_y, uint32_t dmg_w, uint32_t dmg_h) {
    if (!g_backbuf_surface || !g_scanout_surface || !g_wallpaper_surface) {
        return;
    }

    /* 1. Blit desktop wallpaper into backbuffer */
    pixman_image_composite32(PIXMAN_OP_SRC,
                             g_wallpaper_surface,
                             NULL,
                             g_backbuf_surface,
                             0, 0,
                             0, 0,
                             0, 0,
                             (int)g_fb_width, (int)g_fb_height);

    /* 2. Composite windows in Z-order (bottom to top) */
    if (g_num_windows == 0 && g_welcome_surface != NULL) {
        composite_window(&g_welcome_win, true);
    } else {
        for (size_t i = 0; i < g_num_windows; ++i) {
            bool is_top = (i + 1 == g_num_windows);
            composite_window(&g_windows[i], is_top);
        }
    }

    /* 3. Present damaged region with software cursor overlay and flush GPU */
    present_region_with_cursor(dmg_x, dmg_y, dmg_w, dmg_h);
}

/* ==============================================================================
 * Window Table Management
 * ============================================================================== */

static comp_window_t* find_window(uint32_t window_id, size_t* out_idx) {
    for (size_t i = 0; i < g_num_windows; ++i) {
        if (g_windows[i].window_id == window_id) {
            if (out_idx) *out_idx = i;
            return &g_windows[i];
        }
    }
    return NULL;
}

static void release_window_resources(comp_window_t* win) {
    if (!win) return;
    if (win->client_surface) {
        pixman_image_unref(win->client_surface);
        win->client_surface = NULL;
    }
    if (win->vaddr != 0 && win->vmo_size > 0) {
        zx_vmar_unmap(zx_vmar_root_self(), win->vaddr, win->vmo_size);
        win->vaddr = 0;
    }
    if (win->vmo != ZX_HANDLE_INVALID) {
        zx_handle_close(win->vmo);
        win->vmo = ZX_HANDLE_INVALID;
    }
}

static void destroy_client_windows(int client_slot) {
    bool removed = false;
    size_t i = 0;
    while (i < g_num_windows) {
        if (g_windows[i].owner_client == client_slot) {
            if (g_drag_window_id == g_windows[i].window_id) {
                g_drag_window_id = 0;
            }
            release_window_resources(&g_windows[i]);
            for (size_t j = i; j + 1 < g_num_windows; ++j) {
                g_windows[j] = g_windows[j + 1];
            }
            g_num_windows--;
            removed = true;
        } else {
            i++;
        }
    }
    if (removed) {
        composite_and_flush(0, 0, g_fb_width, g_fb_height);
    }
}

static void window_push_key(comp_window_t* win, char ch) {
    if (!win) return;
    if (win->key_count < WIN_KEY_FIFO_MAX) {
        win->key_fifo[win->key_tail] = ch;
        win->key_tail = (win->key_tail + 1u) % WIN_KEY_FIFO_MAX;
        win->key_count++;
    }
}

static char window_pop_key(comp_window_t* win) {
    if (!win || win->key_count == 0) return 0;
    char ch = win->key_fifo[win->key_head];
    win->key_head = (win->key_head + 1u) % WIN_KEY_FIFO_MAX;
    win->key_count--;
    return ch;
}

/* ==============================================================================
 * VirtIO Tablet, VirtIO Keyboard & UART Serial Input Handling
 * ============================================================================== */

static void uart_lock(void) {
    while (__atomic_test_and_set(&g_uart_lock, __ATOMIC_ACQUIRE)) {}
}

static void uart_unlock(void) {
    __atomic_clear(&g_uart_lock, __ATOMIC_RELEASE);
}

static void uart_rx_thread_entry(uintptr_t arg) {
    zx_handle_t rsrc = (zx_handle_t)arg;
    char rx_buf[64];
    while (!g_shutdown) {
        size_t len = sizeof(rx_buf);
        zx_status_t st = zx_debug_read(rsrc, rx_buf, &len);
        if (st == ZX_OK && len > 0) {
            uart_lock();
            for (size_t i = 0; i < len; ++i) {
                if (g_uart_count < sizeof(g_uart_fifo)) {
                    g_uart_fifo[g_uart_tail] = rx_buf[i];
                    g_uart_tail = (g_uart_tail + 1u) % sizeof(g_uart_fifo);
                    g_uart_count++;
                }
            }
            uart_unlock();
        } else {
            zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
        }
    }
    zx_thread_exit();
}

static void init_input_subsystems(void) {
    if (g_root_resource == ZX_HANDLE_INVALID) {
        return;
    }

    /* 1. Discover and bind all PCIe VirtIO Input devices (keyboard + tablet) */
    uint64_t ecam_phys = pci_get_default_ecam_base();
    zx_handle_t ecam_vmo = ZX_HANDLE_INVALID;
    if (ecam_phys != 0 &&
        zx_vmo_create_physical(g_root_resource, ecam_phys, PCI_ECAM_SIZE_16_BUSES, &ecam_vmo) == ZX_OK) {
        zx_vaddr_t ecam_vaddr = 0;
        zx_status_t st = zx_vmar_map(zx_vmar_root_self(),
                                     ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_MAP_RANGE,
                                     0, ecam_vmo, 0, PCI_ECAM_SIZE_16_BUSES, &ecam_vaddr);
        zx_handle_close(ecam_vmo);
        if (st == ZX_OK && ecam_vaddr != 0) {
            for (uint32_t idx = 0; idx < MAX_INPUT_DEVS; ++idx) {
                if (virtio_input_init_nth(ecam_vaddr, g_root_resource, idx,
                                          &g_input_devs[g_num_input_devs]) == ZX_OK) {
                    virtio_pci_dev_t* pdev = &g_input_devs[g_num_input_devs].pci_dev;
                    log_status(STATUS_OKAY, "iris_compositor",
                               "Bound VirtIO Input device #%zu at PCI %02x:%02x.%x.",
                               g_num_input_devs, pdev->bus, pdev->dev, pdev->func);
                    g_num_input_devs++;
                } else {
                    break;
                }
            }
        }
    }

    /* 2. Start UART serial console reader thread */
    zx_handle_t proc = zx_process_self();
    if (proc == ZX_HANDLE_INVALID) proc = iris_process_self();
    if (proc != ZX_HANDLE_INVALID) {
        zx_handle_t thrd = ZX_HANDLE_INVALID;
        if (zx_thread_create(proc, "comp_uart_rx", 12, 0, &thrd) == ZX_OK) {
            zx_handle_t stack_vmo = ZX_HANDLE_INVALID;
            zx_vmo_create(65536, 0, &stack_vmo);
            zx_vaddr_t stack_base = 0;
            zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                        0, stack_vmo, 0, 65536, &stack_base);
            zx_handle_close(stack_vmo);
            uintptr_t sp = (stack_base + 65536) & ~(uintptr_t)0xF;
#if defined(__x86_64__)
            sp -= 8;
#endif
            zx_thread_start(thrd, (uintptr_t)uart_rx_thread_entry, sp, (uintptr_t)g_root_resource, 0);
            zx_handle_close(thrd);
            log_status(STATUS_OKAY, "iris_compositor", "Routing VirtIO Input & UART serial events to focused window.");
        }
    }
}

/**
 * @brief Route a translated keyboard character to the currently focused window.
 */
static void route_key_to_focused_window(char ch) {
    if (g_num_windows > 0) {
        comp_window_t* focused = &g_windows[g_num_windows - 1];
        window_push_key(focused, ch);
    } else if (g_welcome_surface != NULL) {
        window_push_key(&g_welcome_win, ch);
        if (ch == '\b' || (uint8_t)ch == 0x7Fu) {
            if (g_welcome_typed_len > 0) {
                g_welcome_typed[--g_welcome_typed_len] = '\0';
            }
        } else if (ch == '\r' || ch == '\n') {
            g_welcome_typed_len = 0;
            g_welcome_typed[0] = '\0';
        } else if ((uint8_t)ch >= 32 && (uint8_t)ch <= 126) {
            if (g_welcome_typed_len + 1 < sizeof(g_welcome_typed)) {
                g_welcome_typed[g_welcome_typed_len++] = ch;
                g_welcome_typed[g_welcome_typed_len] = '\0';
            } else {
                memmove(g_welcome_typed, g_welcome_typed + 1, g_welcome_typed_len - 1);
                g_welcome_typed[g_welcome_typed_len - 1] = ch;
            }
        }
        render_welcome_surface_contents();
        uint32_t fw = g_welcome_win.client_w + (2u * IRIS_WIN_BORDER_PX);
        uint32_t fh = g_welcome_win.client_h + IRIS_WIN_TITLEBAR_H + (2u * IRIS_WIN_BORDER_PX);
        composite_and_flush(g_welcome_win.x, g_welcome_win.y, fw, fh);
    }
}

/**
 * @brief Handle left mouse button press/release at `(g_cursor_x, g_cursor_y)`:
 *        focuses clicked window, brings it to the top of Z-order, and starts
 *        title-bar window dragging if clicked in the 24px title bar.
 */
static void handle_mouse_button(bool pressed) {
    g_btn_left_down = pressed;
    if (!pressed) {
        g_drag_window_id = 0;
        g_drag_welcome   = false;
        return;
    }

    if (g_num_windows == 0 && g_welcome_surface != NULL) {
        uint32_t fw = g_welcome_win.client_w + (2u * IRIS_WIN_BORDER_PX);
        int32_t  tb_bottom = g_welcome_win.y + IRIS_WIN_BORDER_PX + IRIS_WIN_TITLEBAR_H;
        if (g_cursor_x >= g_welcome_win.x && g_cursor_x < g_welcome_win.x + (int32_t)fw &&
            g_cursor_y >= g_welcome_win.y && g_cursor_y < tb_bottom) {
            g_drag_welcome = true;
            g_drag_off_x   = g_cursor_x - g_welcome_win.x;
            g_drag_off_y   = g_cursor_y - g_welcome_win.y;
        }
        return;
    }

    /* Hit-test windows from top of Z-order down to bottom */
    for (size_t idx = g_num_windows; idx > 0; --idx) {
        size_t i = idx - 1;
        comp_window_t* w = &g_windows[i];
        uint32_t fw = w->client_w + (2u * IRIS_WIN_BORDER_PX);
        uint32_t fh = w->client_h + IRIS_WIN_TITLEBAR_H + (2u * IRIS_WIN_BORDER_PX);

        if (g_cursor_x >= w->x && g_cursor_x < w->x + (int32_t)fw &&
            g_cursor_y >= w->y && g_cursor_y < w->y + (int32_t)fh) {
            /* Bring window to front of Z-order if not already top */
            if (i + 1 < g_num_windows) {
                comp_window_t picked = g_windows[i];
                for (size_t j = i; j + 1 < g_num_windows; ++j) {
                    g_windows[j] = g_windows[j + 1];
                }
                g_windows[g_num_windows - 1] = picked;
                w = &g_windows[g_num_windows - 1];
                composite_and_flush(0, 0, g_fb_width, g_fb_height);
            }

            /* Check if click is inside the 24px title bar to initiate drag */
            if (g_cursor_y < w->y + IRIS_WIN_BORDER_PX + IRIS_WIN_TITLEBAR_H) {
                g_drag_window_id = w->window_id;
                g_drag_off_x     = g_cursor_x - w->x;
                g_drag_off_y     = g_cursor_y - w->y;
            }
            break;
        }
    }
}

/**
 * @brief Poll all VirtIO Input devices (keyboard + tablet) and UART serial input.
 */
static void poll_input_devices(void) {
    int32_t old_cx = g_cursor_x;
    int32_t old_cy = g_cursor_y;
    bool    cursor_moved = false;

    for (size_t d = 0; d < g_num_input_devs; ++d) {
        virtio_input_dev_t* dev = &g_input_devs[d];
        if (!dev->active) continue;

        virtio_input_event_t evs[32];
        size_t n_evs = 0;
        if (virtio_input_poll_raw(dev, evs, 32, &n_evs) != ZX_OK || n_evs == 0) {
            continue;
        }

        for (size_t i = 0; i < n_evs; ++i) {
            uint16_t type = evs[i].type;
            uint16_t code = evs[i].code;
            uint32_t val  = evs[i].value;

            if (type == EV_ABS) {
                if (code == ABS_X) {
                    if (val > ABS_MAX_VAL) val = ABS_MAX_VAL;
                    int32_t nx = (int32_t)(((uint64_t)val * (uint64_t)g_fb_width) / (ABS_MAX_VAL + 1ULL));
                    if (nx < 0) nx = 0;
                    if ((uint32_t)nx >= g_fb_width) nx = (int32_t)g_fb_width - 1;
                    if (nx != g_cursor_x) {
                        g_cursor_x = nx;
                        cursor_moved = true;
                    }
                } else if (code == ABS_Y) {
                    if (val > ABS_MAX_VAL) val = ABS_MAX_VAL;
                    int32_t ny = (int32_t)(((uint64_t)val * (uint64_t)g_fb_height) / (ABS_MAX_VAL + 1ULL));
                    if (ny < 0) ny = 0;
                    if ((uint32_t)ny >= g_fb_height) ny = (int32_t)g_fb_height - 1;
                    if (ny != g_cursor_y) {
                        g_cursor_y = ny;
                        cursor_moved = true;
                    }
                }
            } else if (type == EV_REL) {
                int32_t delta = (int32_t)val;
                if (code == REL_X && delta != 0) {
                    int32_t nx = g_cursor_x + delta;
                    if (nx < 0) nx = 0;
                    if ((uint32_t)nx >= g_fb_width) nx = (int32_t)g_fb_width - 1;
                    g_cursor_x = nx;
                    cursor_moved = true;
                } else if (code == REL_Y && delta != 0) {
                    int32_t ny = g_cursor_y + delta;
                    if (ny < 0) ny = 0;
                    if ((uint32_t)ny >= g_fb_height) ny = (int32_t)g_fb_height - 1;
                    g_cursor_y = ny;
                    cursor_moved = true;
                }
            } else if (type == EV_KEY) {
                if (code == BTN_LEFT) {
                    handle_mouse_button(val != 0);
                } else if (code == KEY_LEFTSHIFT) {
                    dev->shift_l = (val != 0);
                    dev->shift_down = (dev->shift_l || dev->shift_r);
                } else if (code == KEY_RIGHTSHIFT) {
                    dev->shift_r = (val != 0);
                    dev->shift_down = (dev->shift_l || dev->shift_r);
                } else if (code == KEY_LEFTCTRL) {
                    dev->ctrl_l = (val != 0);
                    dev->ctrl_down = (dev->ctrl_l || dev->ctrl_r);
                } else if (code == KEY_RIGHTCTRL) {
                    dev->ctrl_r = (val != 0);
                    dev->ctrl_down = (dev->ctrl_l || dev->ctrl_r);
                } else if (code == KEY_LEFTALT) {
                    dev->alt_l = (val != 0);
                    dev->alt_down = (dev->alt_l || dev->alt_r);
                } else if (code == KEY_RIGHTALT) {
                    dev->alt_r = (val != 0);
                    dev->alt_down = (dev->alt_l || dev->alt_r);
                } else if (code == KEY_CAPSLOCK && val == 1) {
                    dev->caps_lock = !dev->caps_lock;
                } else if (val == 1 || val == 2) {
                    int ch = virtio_input_translate_key(code, dev->shift_down, dev->ctrl_down, dev->caps_lock);
                    if (ch >= 0) {
                        route_key_to_focused_window((char)ch);
                    }
                }
            }
        }
    }

    /* Drain any UART serial input characters */
    for (;;) {
        char uch = 0;
        bool has_uch = false;
        uart_lock();
        if (g_uart_count > 0) {
            uch = g_uart_fifo[g_uart_head];
            g_uart_head = (g_uart_head + 1u) % sizeof(g_uart_fifo);
            g_uart_count--;
            has_uch = true;
        }
        uart_unlock();
        if (!has_uch) break;
        route_key_to_focused_window(uch);
    }

    if (cursor_moved) {
        if (g_btn_left_down && g_drag_welcome && g_num_windows == 0 && g_welcome_surface != NULL) {
            int32_t old_wx = g_welcome_win.x;
            int32_t old_wy = g_welcome_win.y;
            uint32_t fw = g_welcome_win.client_w + (2u * IRIS_WIN_BORDER_PX);
            uint32_t fh = g_welcome_win.client_h + IRIS_WIN_TITLEBAR_H + (2u * IRIS_WIN_BORDER_PX);

            g_welcome_win.x = g_cursor_x - g_drag_off_x;
            g_welcome_win.y = g_cursor_y - g_drag_off_y;
            if (g_welcome_win.y < IRIS_TOPBAR_H) {
                g_welcome_win.y = IRIS_TOPBAR_H;
            }

            int32_t min_x = (old_wx < g_welcome_win.x) ? old_wx : g_welcome_win.x;
            int32_t min_y = (old_wy < g_welcome_win.y) ? old_wy : g_welcome_win.y;
            int32_t max_x = ((old_wx + (int32_t)fw) > (g_welcome_win.x + (int32_t)fw))
                            ? (old_wx + (int32_t)fw) : (g_welcome_win.x + (int32_t)fw);
            int32_t max_y = ((old_wy + (int32_t)fh) > (g_welcome_win.y + (int32_t)fh))
                            ? (old_wy + (int32_t)fh) : (g_welcome_win.y + (int32_t)fh);
            if (old_cx < min_x) min_x = old_cx;
            if (old_cy < min_y) min_y = old_cy;
            if (old_cx + CURSOR_SPRITE_W > max_x) max_x = old_cx + CURSOR_SPRITE_W;
            if (old_cy + CURSOR_SPRITE_H > max_y) max_y = old_cy + CURSOR_SPRITE_H;

            composite_and_flush(min_x, min_y, (uint32_t)(max_x - min_x), (uint32_t)(max_y - min_y));
        } else if (g_btn_left_down && g_drag_window_id != 0) {
            comp_window_t* win = find_window(g_drag_window_id, NULL);
            if (win) {
                int32_t old_wx = win->x;
                int32_t old_wy = win->y;
                uint32_t fw = win->client_w + (2u * IRIS_WIN_BORDER_PX);
                uint32_t fh = win->client_h + IRIS_WIN_TITLEBAR_H + (2u * IRIS_WIN_BORDER_PX);

                win->x = g_cursor_x - g_drag_off_x;
                win->y = g_cursor_y - g_drag_off_y;
                if (win->y < IRIS_TOPBAR_H) {
                    win->y = IRIS_TOPBAR_H;
                }

                int32_t min_x = (old_wx < win->x) ? old_wx : win->x;
                int32_t min_y = (old_wy < win->y) ? old_wy : win->y;
                int32_t max_x = ((old_wx + (int32_t)fw) > (win->x + (int32_t)fw))
                                ? (old_wx + (int32_t)fw) : (win->x + (int32_t)fw);
                int32_t max_y = ((old_wy + (int32_t)fh) > (win->y + (int32_t)fh))
                                ? (old_wy + (int32_t)fh) : (win->y + (int32_t)fh);
                if (old_cx < min_x) min_x = old_cx;
                if (old_cy < min_y) min_y = old_cy;
                if (old_cx + CURSOR_SPRITE_W > max_x) max_x = old_cx + CURSOR_SPRITE_W;
                if (old_cy + CURSOR_SPRITE_H > max_y) max_y = old_cy + CURSOR_SPRITE_H;

                composite_and_flush(min_x, min_y, (uint32_t)(max_x - min_x), (uint32_t)(max_y - min_y));
            }
        } else {
            /* Fast path: only pointer moved, restore backbuffer under old cursor and draw new cursor */
            int32_t min_x = (old_cx < g_cursor_x) ? old_cx : g_cursor_x;
            int32_t min_y = (old_cy < g_cursor_y) ? old_cy : g_cursor_y;
            int32_t max_x = ((old_cx + CURSOR_SPRITE_W) > (g_cursor_x + CURSOR_SPRITE_W))
                            ? (old_cx + CURSOR_SPRITE_W) : (g_cursor_x + CURSOR_SPRITE_W);
            int32_t max_y = ((old_cy + CURSOR_SPRITE_H) > (g_cursor_y + CURSOR_SPRITE_H))
                            ? (old_cy + CURSOR_SPRITE_H) : (g_cursor_y + CURSOR_SPRITE_H);
            present_region_with_cursor(min_x, min_y,
                                       (uint32_t)(max_x - min_x),
                                       (uint32_t)(max_y - min_y));
        }
    }
}

/* ==============================================================================
 * Compositor IPC Request Handlers (`IRIS_PROTO_COMPOSITOR`)
 * ============================================================================== */

static zx_status_t handle_create_window(int client_slot,
                                        const iris_compositor_create_window_req_t* req,
                                        zx_handle_t client_vmo,
                                        uint32_t* out_window_id) {
    if (!req || !out_window_id || client_vmo == ZX_HANDLE_INVALID) {
        if (client_vmo != ZX_HANDLE_INVALID) zx_handle_close(client_vmo);
        return ZX_ERR_INVALID_ARGS;
    }
    if (req->width == 0 || req->height == 0 || req->width > 4096u || req->height > 4096u) {
        zx_handle_close(client_vmo);
        return ZX_ERR_INVALID_ARGS;
    }
    if (g_num_windows >= MAX_COMP_WINDOWS) {
        zx_handle_close(client_vmo);
        return ZX_ERR_NO_RESOURCES;
    }

    uint64_t raw_bytes = (uint64_t)req->width * (uint64_t)req->height * 4ULL;
    uint64_t map_size  = (raw_bytes + COMP_PAGE_SIZE - 1ULL) & COMP_PAGE_MASK;

    zx_vaddr_t mapped_va = 0;
    zx_status_t st = zx_vmar_map(zx_vmar_root_self(),
                                 ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                                 0, client_vmo, 0, map_size, &mapped_va);
    if (st != ZX_OK || mapped_va == 0) {
        zx_handle_close(client_vmo);
        return (st != ZX_OK) ? st : ZX_ERR_NO_MEMORY;
    }

    pixman_image_t* surf = pixman_image_create_bits(PIXMAN_a8r8g8b8,
                                                    (int)req->width,
                                                    (int)req->height,
                                                    (uint32_t*)mapped_va,
                                                    (int)(req->width * 4u));
    if (!surf) {
        zx_vmar_unmap(zx_vmar_root_self(), mapped_va, map_size);
        zx_handle_close(client_vmo);
        return ZX_ERR_NO_MEMORY;
    }

    comp_window_t* win = &g_windows[g_num_windows];
    memset(win, 0, sizeof(*win));
    win->window_id      = g_next_window_id++;
    win->owner_client   = client_slot;
    win->client_w       = req->width;
    win->client_h       = req->height;
    win->flags          = req->flags;
    win->vmo            = client_vmo;
    win->vaddr          = mapped_va;
    win->vmo_size       = map_size;
    win->client_surface = surf;

    memcpy(win->title, req->title, sizeof(win->title) - 1);
    win->title[sizeof(win->title) - 1] = '\0';
    if (win->title[0] == '\0') {
        memcpy(win->title, "irisOS Window", 14);
    }

    uint32_t frame_w = req->width  + (2u * IRIS_WIN_BORDER_PX);
    uint32_t frame_h = req->height + IRIS_WIN_TITLEBAR_H + (2u * IRIS_WIN_BORDER_PX);

    if (req->x >= 0 && req->y >= 0) {
        win->x = req->x;
        win->y = req->y;
    } else {
        /* Center first window; cascade subsequent windows by 28px */
        int32_t base_x = (g_fb_width > frame_w)  ? (int32_t)((g_fb_width - frame_w) / 2u)  : 16;
        int32_t base_y = (g_fb_height > frame_h) ? (int32_t)((g_fb_height - frame_h) / 2u) : (IRIS_TOPBAR_H + 12);
        int32_t offset = (int32_t)(g_num_windows % 6u) * 28;
        win->x = base_x + offset;
        win->y = base_y + offset;
        if (win->y < IRIS_TOPBAR_H + 4) {
            win->y = IRIS_TOPBAR_H + 4;
        }
    }

    g_num_windows++;
    *out_window_id = win->window_id;

    composite_and_flush(0, 0, g_fb_width, g_fb_height);
    return ZX_OK;
}

static zx_status_t handle_destroy_window(const iris_compositor_destroy_window_req_t* req) {
    if (!req || req->window_id == 0) {
        return ZX_ERR_INVALID_ARGS;
    }

    size_t idx = 0;
    comp_window_t* win = find_window(req->window_id, &idx);
    if (!win) {
        return ZX_ERR_NOT_FOUND;
    }

    if (g_drag_window_id == win->window_id) {
        g_drag_window_id = 0;
    }
    release_window_resources(win);
    for (size_t j = idx; j + 1 < g_num_windows; ++j) {
        g_windows[j] = g_windows[j + 1];
    }
    g_num_windows--;

    composite_and_flush(0, 0, g_fb_width, g_fb_height);
    return ZX_OK;
}

static zx_status_t handle_damage_window(const iris_compositor_damage_req_t* req) {
    if (!req || req->window_id == 0) {
        return ZX_ERR_INVALID_ARGS;
    }

    comp_window_t* win = find_window(req->window_id, NULL);
    if (!win) {
        return ZX_ERR_NOT_FOUND;
    }

    uint32_t frame_w = win->client_w + (2u * IRIS_WIN_BORDER_PX);
    uint32_t frame_h = win->client_h + IRIS_WIN_TITLEBAR_H + (2u * IRIS_WIN_BORDER_PX);

    composite_and_flush(win->x, win->y, frame_w, frame_h);
    return ZX_OK;
}

static zx_status_t handle_move_window(const iris_compositor_move_window_req_t* req) {
    if (!req || req->window_id == 0) {
        return ZX_ERR_INVALID_ARGS;
    }

    comp_window_t* win = find_window(req->window_id, NULL);
    if (!win) {
        return ZX_ERR_NOT_FOUND;
    }

    win->x = req->x;
    win->y = req->y;
    if (win->y < IRIS_TOPBAR_H) {
        win->y = IRIS_TOPBAR_H;
    }

    composite_and_flush(0, 0, g_fb_width, g_fb_height);
    return ZX_OK;
}

static zx_status_t handle_poll_event(const iris_compositor_poll_event_req_t* req,
                                     iris_compositor_poll_event_resp_t* resp) {
    if (!req || !resp) {
        return ZX_ERR_INVALID_ARGS;
    }
    memset(resp, 0, sizeof(*resp));

    comp_window_t* win = find_window(req->window_id, NULL);
    if (!win) {
        return ZX_ERR_NOT_FOUND;
    }

    resp->mouse_x = g_cursor_x - (win->x + IRIS_WIN_BORDER_PX);
    resp->mouse_y = g_cursor_y - (win->y + IRIS_WIN_BORDER_PX + IRIS_WIN_TITLEBAR_H);
    resp->buttons = g_btn_left_down ? 1u : 0u;

    uint32_t n = 0;
    while (win->key_count > 0 && n < sizeof(resp->chars)) {
        resp->chars[n++] = window_pop_key(win);
    }
    resp->num_chars = n;
    return ZX_OK;
}

/* ==============================================================================
 * Main Compositor IPC & Input Event Loop
 * ============================================================================== */

static void run_compositor_loop(void) {
    log_status(STATUS_OKAY, "iris_compositor", "IPC server listening on control channel.");

    zx_handle_t clients[MAX_COMP_CLIENTS];
    for (int i = 0; i < MAX_COMP_CLIENTS; ++i) {
        clients[i] = ZX_HANDLE_INVALID;
    }
    clients[0] = g_server_channel;
    g_server_channel = ZX_HANDLE_INVALID;
    int active_clients = 1;

    uint8_t req_buf[1024] __attribute__((aligned(8)));
    uint8_t rep_buf[256]  __attribute__((aligned(8)));
    zx_handle_t handles[IRIS_IPC_MAX_HANDLES];

    bool has_input_polling = (g_num_input_devs > 0 || g_root_resource != ZX_HANDLE_INVALID);

    while (active_clients > 0) {
        if (has_input_polling) {
            poll_input_devices();
        }

        bool did_work = false;

        for (int c = 0; c < MAX_COMP_CLIENTS; ++c) {
            zx_handle_t ch = clients[c];
            if (ch == ZX_HANDLE_INVALID) {
                continue;
            }

            uint32_t act_bytes   = 0;
            uint32_t act_handles = 0;
            zx_status_t st = zx_channel_read(ch, 0,
                                             req_buf, handles,
                                             sizeof(req_buf), IRIS_IPC_MAX_HANDLES,
                                             &act_bytes, &act_handles);
            if (st == ZX_ERR_SHOULD_WAIT) {
                continue;
            }
            if (st != ZX_OK) {
                if (st == ZX_ERR_PEER_CLOSED) {
                    destroy_client_windows(c);
                    zx_handle_close(ch);
                    clients[c] = ZX_HANDLE_INVALID;
                    active_clients--;
                }
                continue;
            }

            did_work = true;

            if (act_bytes < sizeof(iris_msg_hdr_t)) {
                for (uint32_t h = 0; h < act_handles; ++h) {
                    if (handles[h] != ZX_HANDLE_INVALID) zx_handle_close(handles[h]);
                }
                continue;
            }

            const iris_msg_hdr_t* req_hdr = (const iris_msg_hdr_t*)req_buf;
            if (!iris_msg_hdr_is_valid(req_hdr, IRIS_PROTO_COMPOSITOR)) {
                for (uint32_t h = 0; h < act_handles; ++h) {
                    if (handles[h] != ZX_HANDLE_INVALID) zx_handle_close(handles[h]);
                }
                continue;
            }

            iris_msg_hdr_t* rep_hdr = (iris_msg_hdr_t*)rep_buf;
            uint32_t rep_total_len  = sizeof(iris_msg_hdr_t);
            zx_handle_t rep_handle  = ZX_HANDLE_INVALID;
            uint32_t num_rep_hnds   = 0;

            switch (req_hdr->opcode) {
                case IRIS_COMPOSITOR_OP_CREATE_WINDOW: {
                    zx_handle_t vmo_hnd = (act_handles >= 1) ? handles[0] : ZX_HANDLE_INVALID;
                    for (uint32_t h = 1; h < act_handles; ++h) {
                        if (handles[h] != ZX_HANDLE_INVALID) zx_handle_close(handles[h]);
                    }
                    if (req_hdr->payload_len < sizeof(iris_compositor_create_window_req_t)) {
                        if (vmo_hnd != ZX_HANDLE_INVALID) zx_handle_close(vmo_hnd);
                        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
                        break;
                    }
                    const iris_compositor_create_window_req_t* creq =
                        (const iris_compositor_create_window_req_t*)(req_buf + sizeof(iris_msg_hdr_t));
                    iris_compositor_create_window_resp_t* cresp =
                        (iris_compositor_create_window_resp_t*)(rep_buf + sizeof(iris_msg_hdr_t));
                    cresp->window_id = 0;

                    zx_status_t cst = handle_create_window(c, creq, vmo_hnd, &cresp->window_id);
                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, cst, sizeof(*cresp));
                    rep_total_len = sizeof(iris_msg_hdr_t) + sizeof(*cresp);
                    break;
                }

                case IRIS_COMPOSITOR_OP_DESTROY_WINDOW: {
                    for (uint32_t h = 0; h < act_handles; ++h) {
                        if (handles[h] != ZX_HANDLE_INVALID) zx_handle_close(handles[h]);
                    }
                    if (req_hdr->payload_len < sizeof(iris_compositor_destroy_window_req_t)) {
                        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
                        break;
                    }
                    const iris_compositor_destroy_window_req_t* dreq =
                        (const iris_compositor_destroy_window_req_t*)(req_buf + sizeof(iris_msg_hdr_t));
                    zx_status_t dst = handle_destroy_window(dreq);
                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, dst, 0);
                    break;
                }

                case IRIS_COMPOSITOR_OP_DAMAGE: {
                    for (uint32_t h = 0; h < act_handles; ++h) {
                        if (handles[h] != ZX_HANDLE_INVALID) zx_handle_close(handles[h]);
                    }
                    if (req_hdr->payload_len < sizeof(iris_compositor_damage_req_t)) {
                        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
                        break;
                    }
                    const iris_compositor_damage_req_t* dmg_req =
                        (const iris_compositor_damage_req_t*)(req_buf + sizeof(iris_msg_hdr_t));
                    zx_status_t dmg_st = handle_damage_window(dmg_req);
                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, dmg_st, 0);
                    break;
                }

                case IRIS_COMPOSITOR_OP_MOVE_WINDOW: {
                    for (uint32_t h = 0; h < act_handles; ++h) {
                        if (handles[h] != ZX_HANDLE_INVALID) zx_handle_close(handles[h]);
                    }
                    if (req_hdr->payload_len < sizeof(iris_compositor_move_window_req_t)) {
                        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
                        break;
                    }
                    const iris_compositor_move_window_req_t* mreq =
                        (const iris_compositor_move_window_req_t*)(req_buf + sizeof(iris_msg_hdr_t));
                    zx_status_t mst = handle_move_window(mreq);
                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, mst, 0);
                    break;
                }

                case IRIS_COMPOSITOR_OP_POLL_EVENT: {
                    for (uint32_t h = 0; h < act_handles; ++h) {
                        if (handles[h] != ZX_HANDLE_INVALID) zx_handle_close(handles[h]);
                    }
                    if (req_hdr->payload_len < sizeof(iris_compositor_poll_event_req_t)) {
                        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
                        break;
                    }
                    const iris_compositor_poll_event_req_t* preq =
                        (const iris_compositor_poll_event_req_t*)(req_buf + sizeof(iris_msg_hdr_t));
                    iris_compositor_poll_event_resp_t* presp =
                        (iris_compositor_poll_event_resp_t*)(rep_buf + sizeof(iris_msg_hdr_t));
                    zx_status_t pst = handle_poll_event(preq, presp);
                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, pst, sizeof(*presp));
                    rep_total_len = sizeof(iris_msg_hdr_t) + sizeof(*presp);
                    break;
                }

                case IRIS_COMPOSITOR_OP_OPEN_CLIENT: {
                    for (uint32_t h = 0; h < act_handles; ++h) {
                        if (handles[h] != ZX_HANDLE_INVALID) zx_handle_close(handles[h]);
                    }
                    zx_handle_t srv_ep = ZX_HANDLE_INVALID;
                    zx_handle_t cli_ep = ZX_HANDLE_INVALID;
                    zx_status_t ch_st = zx_channel_create(0, &srv_ep, &cli_ep);
                    if (ch_st == ZX_OK) {
                        bool added = false;
                        for (int slot = 0; slot < MAX_COMP_CLIENTS; ++slot) {
                            if (clients[slot] == ZX_HANDLE_INVALID) {
                                clients[slot] = srv_ep;
                                active_clients++;
                                added = true;
                                break;
                            }
                        }
                        if (!added) {
                            zx_handle_close(srv_ep);
                            zx_handle_close(cli_ep);
                            ch_st = ZX_ERR_NO_RESOURCES;
                        } else {
                            rep_handle   = cli_ep;
                            num_rep_hnds = 1;
                        }
                    }
                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, ch_st, 0);
                    break;
                }

                default: {
                    for (uint32_t h = 0; h < act_handles; ++h) {
                        if (handles[h] != ZX_HANDLE_INVALID) zx_handle_close(handles[h]);
                    }
                    iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_NOT_SUPPORTED, 0);
                    break;
                }
            }

            zx_channel_write(ch, 0, rep_buf, rep_total_len,
                             num_rep_hnds > 0 ? &rep_handle : NULL, num_rep_hnds);
        }

        if (!did_work && active_clients > 0) {
            zx_wait_item_t items[MAX_COMP_CLIENTS];
            int item_map[MAX_COMP_CLIENTS];
            uint32_t count = 0;

            for (int c = 0; c < MAX_COMP_CLIENTS; ++c) {
                if (clients[c] != ZX_HANDLE_INVALID) {
                    items[count].handle  = clients[c];
                    items[count].waitfor = ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED;
                    items[count].pending = 0;
                    item_map[count]      = c;
                    count++;
                }
            }

            if (count > 0) {
                zx_time_t deadline = has_input_polling
                                     ? zx_deadline_after(ZX_MSEC(8))
                                     : ZX_TIME_INFINITE;
                zx_status_t wst = zx_object_wait_many(items, count, deadline);
                if (wst != ZX_OK && wst != ZX_ERR_TIMED_OUT) {
                    break;
                }
                if (wst == ZX_OK) {
                    for (uint32_t i = 0; i < count; ++i) {
                        if ((items[i].pending & ZX_CHANNEL_PEER_CLOSED) &&
                            !(items[i].pending & ZX_CHANNEL_READABLE)) {
                            int slot = item_map[i];
                            destroy_client_windows(slot);
                            zx_handle_close(clients[slot]);
                            clients[slot] = ZX_HANDLE_INVALID;
                            active_clients--;
                        }
                    }
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    g_server_channel = zx_take_startup_handle(PA_HND(PA_USER0, 0));
    g_gpu_channel    = zx_take_startup_handle(PA_HND(PA_USER1, 0));
    g_root_resource  = iris_root_resource();
    if (g_root_resource == ZX_HANDLE_INVALID) {
        g_root_resource = zx_take_startup_handle(PA_HND(PA_RESOURCE, 0));
    }

    if (g_server_channel == ZX_HANDLE_INVALID || g_gpu_channel == ZX_HANDLE_INVALID) {
        log_status(STATUS_FAIL, "iris_compositor", "Missing required startup channel handles.");
        return 1;
    }

    /* 1. Initialize FreeType 2 font engine and load /System/Resources/Fonts/Inter.otf */
    if (FT_Init_FreeType(&g_ft_lib) == 0) {
        if (FT_New_Face(g_ft_lib, INTER_FONT_PATH, 0, &g_ft_face) == 0 && g_ft_face != NULL) {
            FT_Set_Pixel_Sizes(g_ft_face, 0, 14);
            log_status(STATUS_OKAY, "iris_compositor",
                       "Initialized FreeType 2 with %s (%s %s).",
                       INTER_FONT_PATH,
                       g_ft_face->family_name ? g_ft_face->family_name : "Inter",
                       g_ft_face->style_name  ? g_ft_face->style_name  : "Regular");
        } else {
            log_status(STATUS_OKAY, "iris_compositor",
                       "Initialized FreeType 2 font engine and Pixman 2D compositor.");
        }
    }

    /* 2. Connect to virtio_gpu scanout (1280x800) */
    zx_status_t st = connect_to_virtio_gpu();
    if (st != ZX_OK) {
        return 1;
    }

    /* 3. Initialize 32x32 RGBA classic macOS arrow software cursor sprite */
    st = init_cursor_sprite();
    if (st != ZX_OK) {
        log_status(STATUS_FAIL, "iris_compositor", "Failed to initialize cursor sprite (%d).", st);
        return 1;
    }

    /* 4. Pre-render desktop wallpaper + menu bar + welcome window */
    st = init_desktop_wallpaper();
    if (st != ZX_OK) {
        log_status(STATUS_FAIL, "iris_compositor", "Failed to initialize desktop wallpaper (%d).", st);
        return 1;
    }

    /* 5. Initialize VirtIO Input (keyboard + tablet) and UART serial routing */
    init_input_subsystems();

    /* 6. Composite initial desktop wallpaper + welcome surface + cursor and flush scanout */
    composite_and_flush(0, 0, g_fb_width, g_fb_height);
    log_status(STATUS_OKAY, "iris_compositor", "Rendered 1280x800 desktop, Inter typography, and macOS pointer cursor.");

    run_compositor_loop();

    /* 7. Clean teardown */
    g_shutdown = true;
    for (size_t d = 0; d < g_num_input_devs; ++d) {
        virtio_input_teardown(&g_input_devs[d]);
    }
    g_num_input_devs = 0;

    for (size_t i = 0; i < g_num_windows; ++i) {
        release_window_resources(&g_windows[i]);
    }
    g_num_windows = 0;

    if (g_cursor_surface)    pixman_image_unref(g_cursor_surface);
    if (g_cursor_pixels)     free(g_cursor_pixels);
    if (g_welcome_surface)    pixman_image_unref(g_welcome_surface);
    if (g_welcome_pixels)     free(g_welcome_pixels);
    if (g_wallpaper_surface)  pixman_image_unref(g_wallpaper_surface);
    if (g_backbuf_surface)    pixman_image_unref(g_backbuf_surface);
    if (g_backbuf_pixels)     free(g_backbuf_pixels);
    if (g_scanout_surface)    pixman_image_unref(g_scanout_surface);

    if (g_fb_vaddr != 0 && g_fb_size > 0) {
        zx_vmar_unmap(zx_vmar_root_self(), g_fb_vaddr, g_fb_size);
    }
    if (g_fb_vmo != ZX_HANDLE_INVALID) {
        zx_handle_close(g_fb_vmo);
    }
    if (g_gpu_channel != ZX_HANDLE_INVALID) {
        zx_handle_close(g_gpu_channel);
    }
    if (g_ft_face) {
        FT_Done_Face(g_ft_face);
    }
    if (g_ft_lib) {
        FT_Done_FreeType(g_ft_lib);
    }

    log_status(STATUS_OKAY, "iris_compositor", "Daemon exited cleanly.");
    return 0;
}
