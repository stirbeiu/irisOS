/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_compositor/include/compositor.h
 * Purpose: Shared definitions, structures, and function declarations for iris_compositor.
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 system server.
 */

#ifndef IRIS_COMPOSITOR_H
#define IRIS_COMPOSITOR_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <zircon/types.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/time.h>

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
#define CURSOR_SPRITE_W         18
#define CURSOR_SPRITE_H         24

/* Window frame decoration constants */
#define IRIS_WIN_BORDER_PX      1
#define IRIS_WIN_TITLEBAR_H     24
#define IRIS_TOPBAR_H           28

/* Font Paths */
#define INTER_REGULAR_PATH      "/System/Resources/Fonts/InterDisplay-Regular.otf"
#define INTER_BOLD_PATH         "/System/Resources/Fonts/InterDisplay-Bold.otf"
#define INTER_ITALIC_PATH       "/System/Resources/Fonts/InterDisplay-Italic.otf"
#define INTER_LEGACY_PATH       "/System/Resources/Fonts/Inter.otf"

/**
 * @brief Font style selector for FreeType typography.
 */
typedef enum {
    FONT_STYLE_REGULAR = 0,
    FONT_STYLE_BOLD    = 1,
    FONT_STYLE_ITALIC  = 2,
    FONT_STYLE_COUNT   = 3
} font_style_t;

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

/**
 * @brief Bounding dirty damage rectangle.
 */
typedef struct comp_dirty_rect {
    int32_t  x;
    int32_t  y;
    uint32_t w;
    uint32_t h;
} comp_dirty_rect_t;

/* Global Compositor State (defined in main.c) */
extern zx_handle_t       g_server_channel;
extern zx_handle_t       g_gpu_channel;
extern zx_handle_t       g_root_resource;
extern uint32_t          g_gpu_txid;

extern uint32_t          g_fb_width;
extern uint32_t          g_fb_height;
extern uint32_t          g_fb_pitch;
extern uint64_t          g_fb_size;
extern zx_handle_t       g_fb_vmo;
extern zx_vaddr_t        g_fb_vaddr;

extern uint32_t*         g_backbuf_pixels;
extern pixman_image_t*   g_backbuf_surface;
extern pixman_image_t*   g_scanout_surface;
extern pixman_image_t*   g_wallpaper_surface;

/* 18x24 RGBA Software Cursor State & Save-Under Tracking */
extern uint32_t*         g_cursor_pixels;
extern pixman_image_t*   g_cursor_surface;
extern int32_t           g_cursor_x;
extern int32_t           g_cursor_y;
extern int32_t           g_last_cursor_x;
extern int32_t           g_last_cursor_y;
extern bool              g_cursor_visible;
extern bool              g_cursor_dirty;
extern bool              g_btn_left_down;
extern uint32_t          g_drag_window_id;
extern bool              g_drag_welcome;
extern bool              g_drag_crash;
extern int32_t           g_drag_off_x;
extern int32_t           g_drag_off_y;

/* Default welcome window shown when no client windows are open */
extern uint32_t*         g_welcome_pixels;
extern pixman_image_t*   g_welcome_surface;
extern comp_window_t     g_welcome_win;
extern char              g_welcome_typed[48];
extern size_t            g_welcome_typed_len;

/* Modal Crash Recovery Dialog */
extern bool              g_crash_dialog_active;
extern comp_window_t     g_crash_win;
extern uint32_t*         g_crash_pixels;
extern pixman_image_t*   g_crash_surface;
extern char              g_crash_details[512];

/* VirtIO Input & UART Serial State */
extern virtio_input_dev_t g_input_devs[MAX_INPUT_DEVS];
extern size_t            g_num_input_devs;
extern volatile bool     g_shutdown;
extern volatile uint8_t  g_uart_lock;
extern char              g_uart_fifo[256];
extern uint32_t          g_uart_head;
extern uint32_t          g_uart_tail;
extern uint32_t          g_uart_count;

/* Window List State */
extern comp_window_t     g_windows[MAX_COMP_WINDOWS];
extern size_t            g_num_windows;
extern uint32_t          g_next_window_id;

/* Dirty Damage Tracking */
extern comp_dirty_rect_t g_dirty_rect;
extern bool              g_dirty_active;

/* ==============================================================================
 * Subsystem Function Declarations
 * ============================================================================== */

/* font.c */
zx_status_t init_font_subsystem(void);
void cleanup_font_subsystem(void);
void draw_string_8x8(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                     int32_t x, int32_t y, const char* str, uint32_t color);
void draw_text_ft(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                  int32_t x, int32_t y, uint32_t pixel_size,
                  const char* str, uint32_t argb_color);
void draw_text_ft_styled(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                         int32_t x, int32_t y, uint32_t pixel_size,
                         font_style_t style, const char* str, uint32_t argb_color);

/* render.c */
pixman_color_t argb_to_pixman_color(uint32_t argb);
void fill_rect_pixman(pixman_image_t* dst, int32_t x, int32_t y,
                      uint32_t w, uint32_t h, uint32_t argb);
void blit_surface_clamped(pixman_image_t* src, int32_t src_w, int32_t src_h,
                         pixman_image_t* dst, int32_t dst_w, int32_t dst_h,
                         int32_t dst_x, int32_t dst_y, pixman_op_t op);
zx_status_t connect_to_virtio_gpu(void);
zx_status_t flush_gpu_scanout(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
zx_status_t init_cursor_sprite(void);
zx_status_t init_desktop_wallpaper(void);
void render_welcome_surface_contents(void);
void redraw_welcome_input_strip(void);
void init_crash_dialog(const char* details);
void render_crash_dialog_contents(void);
void close_crash_dialog(void);
void composite_window(const comp_window_t* win, bool is_focused);
void update_screen(int32_t dmg_x, int32_t dmg_y, uint32_t dmg_w, uint32_t dmg_h);
void composite_and_flush(int32_t dmg_x, int32_t dmg_y, uint32_t dmg_w, uint32_t dmg_h);
void dirty_rect_union(int32_t x, int32_t y, uint32_t w, uint32_t h);
void dirty_rect_union_window(const comp_window_t* win);

/* input.c */
void init_input_subsystems(void);
void poll_input_devices(void);
void route_key_to_focused_window(char ch);
void handle_mouse_button(bool pressed);

/* main.c */
comp_window_t* find_window(uint32_t window_id, size_t* out_idx);
void release_window_resources(comp_window_t* win);
void destroy_client_windows(int client_slot);
void window_push_key(comp_window_t* win, char ch);
char window_pop_key(comp_window_t* win);

#endif /* IRIS_COMPOSITOR_H */
