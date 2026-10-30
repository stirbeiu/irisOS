/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_compositor/src/render.c
 * Purpose: GPU scanout connection, 18x24 macOS cursor, wallpaper, window compositing,
 *          and dirty-region screen presentation.
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 system server.
 */

#include "compositor.h"

/* ==============================================================================
 * 18x24 RGBA Classic macOS Arrow Cursor Sprite
 * ============================================================================== */

static const char* const k_macos_cursor_18x24[CURSOR_SPRITE_H] = {
    "W.................",
    "WW................",
    "WBW...............",
    "WBBW..............",
    "WBBBW.............",
    "WBBBBW............",
    "WBBBBBW...........",
    "WBBBBBBW..........",
    "WBBBBBBBW.........",
    "WBBBBBBBBW........",
    "WBBBBBBBBBW.......",
    "WBBBBBBBBBBW......",
    "WBBBBBBBBBBBW.....",
    "WBBBBBBWWWWWW.....",
    "WBBW.WBBW.........",
    "WBW..WBBW.........",
    "WW...WBBW.........",
    "W.....WBBW........",
    "......WBBW........",
    "......WBBW........",
    ".......WBBW.......",
    ".......WBBW.......",
    "........WBBW......",
    "........WWWW......"
};

zx_status_t init_cursor_sprite(void) {
    g_cursor_pixels = (uint32_t*)calloc((size_t)CURSOR_SPRITE_W * CURSOR_SPRITE_H, sizeof(uint32_t));
    if (!g_cursor_pixels) {
        return ZX_ERR_NO_MEMORY;
    }

    for (uint32_t y = 0; y < CURSOR_SPRITE_H; ++y) {
        const char* row = k_macos_cursor_18x24[y];
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

    g_cursor_x       = (int32_t)(g_fb_width  / 2u);
    g_cursor_y       = (int32_t)(g_fb_height / 2u);
    g_last_cursor_x  = g_cursor_x;
    g_last_cursor_y  = g_cursor_y;
    g_cursor_visible = false;
    g_cursor_dirty   = false;
    return ZX_OK;
}

/* ==============================================================================
 * Pixman Primitive Helpers & Boundary Clamping
 * ============================================================================== */

inline pixman_color_t argb_to_pixman_color(uint32_t argb) {
    uint16_t a = (uint16_t)(((argb >> 24) & 0xFFu) * 257u);
    uint16_t r = (uint16_t)(((argb >> 16) & 0xFFu) * 257u);
    uint16_t g = (uint16_t)(((argb >>  8) & 0xFFu) * 257u);
    uint16_t b = (uint16_t)(((argb >>  0) & 0xFFu) * 257u);
    pixman_color_t c = { r, g, b, a };
    return c;
}

void fill_rect_pixman(pixman_image_t* dst, int32_t x, int32_t y,
                      uint32_t w, uint32_t h, uint32_t argb) {
    if (!dst || w == 0 || h == 0) {
        return;
    }
    int32_t dst_w = pixman_image_get_width(dst);
    int32_t dst_h = pixman_image_get_height(dst);

    int32_t rw = (int32_t)w;
    int32_t rh = (int32_t)h;

    /* Clamp left */
    if (x < 0) {
        int32_t off = -x;
        if (off >= rw) return;
        rw -= off;
        x = 0;
    }
    /* Clamp top */
    if (y < 0) {
        int32_t off = -y;
        if (off >= rh) return;
        rh -= off;
        y = 0;
    }
    /* Off-screen right or bottom */
    if (x >= dst_w || y >= dst_h) {
        return;
    }
    /* Clamp right */
    if (x + rw > dst_w) {
        rw = dst_w - x;
    }
    /* Clamp bottom */
    if (y + rh > dst_h) {
        rh = dst_h - y;
    }
    if (rw <= 0 || rh <= 0) {
        return;
    }

    pixman_color_t col = argb_to_pixman_color(argb);
    pixman_rectangle16_t rect = {
        (int16_t)x,
        (int16_t)y,
        (uint16_t)rw,
        (uint16_t)rh
    };
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, dst, &col, 1, &rect);
}

void blit_surface_clamped(pixman_image_t* src, int32_t src_w, int32_t src_h,
                         pixman_image_t* dst, int32_t dst_w, int32_t dst_h,
                         int32_t dst_x, int32_t dst_y, pixman_op_t op) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    int32_t src_x = 0;
    int32_t src_y = 0;
    int32_t w = src_w;
    int32_t h = src_h;

    /* Trim left if dst_x < 0 */
    if (dst_x < 0) {
        int32_t off = -dst_x;
        if (off >= w) return;
        src_x += off;
        w -= off;
        dst_x = 0;
    }
    /* Trim top if dst_y < 0 */
    if (dst_y < 0) {
        int32_t off = -dst_y;
        if (off >= h) return;
        src_y += off;
        h -= off;
        dst_y = 0;
    }
    /* Completely off-screen right or bottom */
    if (dst_x >= dst_w || dst_y >= dst_h) {
        return;
    }
    /* Trim right if overflowing dst_w */
    if (dst_x + w > dst_w) {
        w = dst_w - dst_x;
    }
    /* Trim bottom if overflowing dst_h */
    if (dst_y + h > dst_h) {
        h = dst_h - dst_y;
    }

    if (w <= 0 || h <= 0) {
        return;
    }

    pixman_image_composite32(op,
                             src, NULL, dst,
                             src_x, src_y,
                             0, 0,
                             dst_x, dst_y,
                             w, h);
}

/* ==============================================================================
 * VirtIO GPU Driver IPC Helpers
 * ============================================================================== */

zx_status_t connect_to_virtio_gpu(void) {
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

zx_status_t flush_gpu_scanout(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
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
 * Dirty Damage Rect Tracking Helpers
 * ============================================================================== */

void dirty_rect_union(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return;
    if (!g_dirty_active) {
        g_dirty_rect.x = x;
        g_dirty_rect.y = y;
        g_dirty_rect.w = w;
        g_dirty_rect.h = h;
        g_dirty_active = true;
        return;
    }

    int32_t r1_x2 = g_dirty_rect.x + (int32_t)g_dirty_rect.w;
    int32_t r1_y2 = g_dirty_rect.y + (int32_t)g_dirty_rect.h;
    int32_t r2_x2 = x + (int32_t)w;
    int32_t r2_y2 = y + (int32_t)h;

    int32_t min_x = (x < g_dirty_rect.x) ? x : g_dirty_rect.x;
    int32_t min_y = (y < g_dirty_rect.y) ? y : g_dirty_rect.y;
    int32_t max_x = (r2_x2 > r1_x2) ? r2_x2 : r1_x2;
    int32_t max_y = (r2_y2 > r1_y2) ? r2_y2 : r1_y2;

    g_dirty_rect.x = min_x;
    g_dirty_rect.y = min_y;
    g_dirty_rect.w = (uint32_t)(max_x - min_x);
    g_dirty_rect.h = (uint32_t)(max_y - min_y);
}

void dirty_rect_union_window(const comp_window_t* win) {
    if (!win) return;
    uint32_t fw = win->client_w + (2u * IRIS_WIN_BORDER_PX);
    uint32_t fh = win->client_h + IRIS_WIN_TITLEBAR_H + (2u * IRIS_WIN_BORDER_PX);
    dirty_rect_union(win->x, win->y, fw, fh);
}

/* ==============================================================================
 * Desktop Wallpaper, Welcome Window & Crash Dialog
 * ============================================================================== */

void redraw_welcome_input_strip(void) {
    if (!g_welcome_surface || !g_welcome_pixels) return;
    uint32_t ww = g_welcome_win.client_w;
    uint32_t wh = g_welcome_win.client_h;

    /* ONLY redraw the 32px input box strip (y = 174..206) */
    fill_rect_pixman(g_welcome_surface, 24, 174, ww - 48u, 32u, 0xFF181825u);
    char input_line[80];
    snprintf(input_line, sizeof(input_line), "Focused Input: %s_", g_welcome_typed);
    draw_text_ft(g_welcome_pixels, ww, wh, 32, 183, 13, input_line, 0xFFF5E0DCu);

    /* Union this strip's desktop coordinates with g_dirty_rect */
    int32_t strip_x = g_welcome_win.x + IRIS_WIN_BORDER_PX + 24;
    int32_t strip_y = g_welcome_win.y + IRIS_WIN_BORDER_PX + IRIS_WIN_TITLEBAR_H + 174;
    dirty_rect_union(strip_x, strip_y, ww - 48u, 32u);
}

void render_welcome_surface_contents(void) {
    if (!g_welcome_surface || !g_welcome_pixels) {
        return;
    }
    uint32_t ww = g_welcome_win.client_w;
    uint32_t wh = g_welcome_win.client_h;

    fill_rect_pixman(g_welcome_surface, 0, 0, ww, wh, 0xFF1E1E2Eu);
    fill_rect_pixman(g_welcome_surface, 16, 16, ww - 32u, 38u, 0xFF313244u);

    draw_text_ft_styled(g_welcome_pixels, ww, wh, 28, 26, 15,
                        FONT_STYLE_BOLD, "irisOS Zero-Copy Window Compositor (1280x800)", 0xFF89B4FAu);
    draw_text_ft(g_welcome_pixels, ww, wh, 24, 72, 13,
                 "Protocol:     IRIS_PROTO_COMPOSITOR (24-byte IPC)", 0xFFCDD6F4u);
    draw_text_ft(g_welcome_pixels, ww, wh, 24, 96, 13,
                 "Typography:   FreeType 2 + Inter Display (Regular/Bold/Italic)", 0xFFCDD6F4u);
    draw_text_ft(g_welcome_pixels, ww, wh, 24, 120, 13,
                 "Input:        VirtIO Tablet (EV_ABS) + Keyboard + UART", 0xFFA6E3A1u);
    draw_text_ft_styled(g_welcome_pixels, ww, wh, 24, 144, 13,
                        FONT_STYLE_ITALIC, "Interaction:  Drag title bar with mouse or type below", 0xFFF9E2AFu);

    /* Initial render of the input echo box */
    fill_rect_pixman(g_welcome_surface, 24, 174, ww - 48u, 32u, 0xFF181825u);
    char input_line[80];
    snprintf(input_line, sizeof(input_line), "Focused Input: %s_", g_welcome_typed);
    draw_text_ft(g_welcome_pixels, ww, wh, 32, 183, 13, input_line, 0xFFF5E0DCu);

    /* Subtle accent bar at bottom of welcome surface */
    fill_rect_pixman(g_welcome_surface, 24, wh - 20u, ww - 48u, 4u, 0xFF89B4FAu);
}

void close_crash_dialog(void) {
    if (!g_crash_dialog_active) return;
    g_crash_dialog_active = false;
    g_drag_crash = false;
    if (g_crash_surface) {
        pixman_image_unref(g_crash_surface);
        g_crash_surface = NULL;
    }
    if (g_crash_pixels) {
        free(g_crash_pixels);
        g_crash_pixels = NULL;
    }
    dirty_rect_union(0, 0, g_fb_width, g_fb_height);
}

void render_crash_dialog_contents(void) {
    if (!g_crash_surface || !g_crash_pixels) return;
    uint32_t cw = g_crash_win.client_w;
    uint32_t ch = g_crash_win.client_h;

    /* Base background: Surface0 (#1E1E2E) */
    fill_rect_pixman(g_crash_surface, 0, 0, cw, ch, 0xFF1E1E2Eu);

    /* Warning icon / circle badge */
    fill_rect_pixman(g_crash_surface, 20, 16, 16, 16, 0xFFF38BA8u); /* Red/Maroon */
    draw_string_8x8(g_crash_pixels, cw, ch, 25, 20, "!", 0xFF11111Bu);

    /* Message header */
    draw_text_ft_styled(g_crash_pixels, cw, ch, 44, 18, 14,
                        FONT_STYLE_BOLD, "The compositor restarted because of a problem.", 0xFFCDD6F4u);

    /* Monospace details container (#11111B) */
    uint32_t box_x = 20u;
    uint32_t box_y = 44u;
    uint32_t box_w = cw - 40u;
    uint32_t box_h = 136u;
    fill_rect_pixman(g_crash_surface, (int32_t)box_x, (int32_t)box_y, box_w, box_h, 0xFF11111Bu);
    /* Border around details box (#45475A) */
    fill_rect_pixman(g_crash_surface, (int32_t)box_x, (int32_t)box_y, box_w, 1u, 0xFF45475Au);
    fill_rect_pixman(g_crash_surface, (int32_t)box_x, (int32_t)(box_y + box_h - 1u), box_w, 1u, 0xFF45475Au);
    fill_rect_pixman(g_crash_surface, (int32_t)box_x, (int32_t)box_y, 1u, box_h, 0xFF45475Au);
    fill_rect_pixman(g_crash_surface, (int32_t)(box_x + box_w - 1u), (int32_t)box_y, 1u, box_h, 0xFF45475Au);

    /* Render details text line by line using monospace 8x8 font */
    char line[128];
    const char* ptr = g_crash_details;
    int32_t line_y = (int32_t)box_y + 10;
    while (*ptr != '\0' && (uint32_t)line_y + 10u < box_y + box_h) {
        size_t len = 0;
        while (ptr[len] != '\0' && ptr[len] != '\n' && len < sizeof(line) - 1) {
            line[len] = ptr[len];
            len++;
        }
        line[len] = '\0';
        draw_string_8x8(g_crash_pixels, cw, ch, (int32_t)box_x + 12, line_y, line, 0xFFA6ADC8u);
        line_y += 14;
        if (ptr[len] == '\n') len++;
        ptr += len;
    }

    /* "OK" Button at bottom right */
    int32_t btn_x = (int32_t)cw - 110;
    int32_t btn_y = (int32_t)ch - 44;
    uint32_t btn_w = 90u;
    uint32_t btn_h = 28u;
    fill_rect_pixman(g_crash_surface, btn_x, btn_y, btn_w, btn_h, 0xFF89B4FAu);
    draw_text_ft_styled(g_crash_pixels, cw, ch, btn_x + 34, btn_y + 7, 13,
                        FONT_STYLE_BOLD, "OK", 0xFF11111Bu);
}

void init_crash_dialog(const char* details) {
    if (g_crash_dialog_active) return;
    uint32_t cw = 540u;
    uint32_t ch = 236u;
    g_crash_pixels = (uint32_t*)calloc((size_t)cw * ch, sizeof(uint32_t));
    if (!g_crash_pixels) return;
    g_crash_surface = pixman_image_create_bits(PIXMAN_a8r8g8b8, (int)cw, (int)ch, g_crash_pixels, (int)(cw * 4u));
    if (!g_crash_surface) {
        free(g_crash_pixels);
        g_crash_pixels = NULL;
        return;
    }

    memset(&g_crash_win, 0, sizeof(g_crash_win));
    g_crash_win.window_id = 0xFFFEu;
    g_crash_win.client_w = cw;
    g_crash_win.client_h = ch;
    g_crash_win.x = (int32_t)(g_fb_width - (cw + 2u * IRIS_WIN_BORDER_PX)) / 2;
    g_crash_win.y = (int32_t)(g_fb_height - (ch + IRIS_WIN_TITLEBAR_H + 2u * IRIS_WIN_BORDER_PX)) / 2;
    g_crash_win.client_surface = g_crash_surface;
    strncpy(g_crash_win.title, "System Notice", sizeof(g_crash_win.title) - 1);

    if (details && details[0] != '\0') {
        strncpy(g_crash_details, details, sizeof(g_crash_details) - 1);
    } else {
        strncpy(g_crash_details, "The compositor restarted because of a problem.", sizeof(g_crash_details) - 1);
    }

    render_crash_dialog_contents();
    g_crash_dialog_active = true;
    dirty_rect_union(g_crash_win.x, g_crash_win.y,
                     cw + 2u * IRIS_WIN_BORDER_PX,
                     ch + IRIS_WIN_TITLEBAR_H + 2u * IRIS_WIN_BORDER_PX);
}

zx_status_t init_desktop_wallpaper(void) {
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

    /* Create smooth diagonal/vertical linear gradient across desktop using Pixman */
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

    draw_text_ft_styled(wp_pixels, g_fb_width, g_fb_height,
                        16, 7, 14, FONT_STYLE_BOLD, "irisOS", 0xFF89B4FAu);
    draw_text_ft(wp_pixels, g_fb_width, g_fb_height,
                 84, 7, 13, "Desktop     System     Window     Help", 0xFFCDD6F4u);
    if (g_fb_width >= 320u) {
        draw_text_ft(wp_pixels, g_fb_width, g_fb_height,
                     (int32_t)g_fb_width - 230, 7, 13, "1280x800  |  Zircon 1011", 0xFFA6ADC8u);
    }

    /* Initialize default welcome window surface */
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

/* ==============================================================================
 * Window Compositing Pipeline
 * ============================================================================== */

void composite_window(const comp_window_t* win, bool is_focused) {
    if (!win || !win->client_surface) {
        return;
    }

    uint32_t frame_w = win->client_w + (2u * IRIS_WIN_BORDER_PX);
    uint32_t frame_h = win->client_h + IRIS_WIN_TITLEBAR_H + (2u * IRIS_WIN_BORDER_PX);

    /* Quick reject if window is completely outside screen bounds */
    if (win->x + (int32_t)frame_w <= 0 || win->x >= (int32_t)g_fb_width ||
        win->y + (int32_t)frame_h <= 0 || win->y >= (int32_t)g_fb_height) {
        return;
    }

    uint32_t border_color   = is_focused ? 0xFF89B4FAu : 0xFF585B70u;
    uint32_t titlebar_color = is_focused ? 0xFF313244u : 0xFF181825u;
    uint32_t title_fg_color = is_focused ? 0xFFCDD6F4u : 0xFF9399B2u;

    /* 1. Outer 1px border around entire window frame */
    fill_rect_pixman(g_backbuf_surface, win->x, win->y, frame_w, IRIS_WIN_BORDER_PX, border_color);
    fill_rect_pixman(g_backbuf_surface, win->x, win->y + (int32_t)frame_h - IRIS_WIN_BORDER_PX,
                     frame_w, IRIS_WIN_BORDER_PX, border_color);
    fill_rect_pixman(g_backbuf_surface, win->x, win->y, IRIS_WIN_BORDER_PX, frame_h, border_color);
    fill_rect_pixman(g_backbuf_surface, win->x + (int32_t)frame_w - IRIS_WIN_BORDER_PX, win->y,
                     IRIS_WIN_BORDER_PX, frame_h, border_color);

    /* 2. 24px Title Bar inside 1px border */
    int32_t tb_x = win->x + IRIS_WIN_BORDER_PX;
    int32_t tb_y = win->y + IRIS_WIN_BORDER_PX;
    fill_rect_pixman(g_backbuf_surface, tb_x, tb_y, win->client_w, IRIS_WIN_TITLEBAR_H - 1, titlebar_color);
    fill_rect_pixman(g_backbuf_surface, tb_x, tb_y + IRIS_WIN_TITLEBAR_H - 1,
                     win->client_w, 1, border_color);

    /* Window control buttons (Close, Minimize, Maximize) */
    fill_rect_pixman(g_backbuf_surface, tb_x + 10, tb_y + 8, 8, 8, 0xFFF38BA8u);
    fill_rect_pixman(g_backbuf_surface, tb_x + 24, tb_y + 8, 8, 8, 0xFFF9E2AFu);
    fill_rect_pixman(g_backbuf_surface, tb_x + 38, tb_y + 8, 8, 8, 0xFFA6E3A1u);

    /* Window title string */
    draw_text_ft_styled(g_backbuf_pixels, g_fb_width, g_fb_height,
                        tb_x + 58, tb_y + 5, 13, FONT_STYLE_BOLD, win->title, title_fg_color);

    /* 3. Blend client VMO surface onto backbuffer with strict boundary clipping */
    int32_t client_dst_x = win->x + IRIS_WIN_BORDER_PX;
    int32_t client_dst_y = win->y + IRIS_WIN_BORDER_PX + IRIS_WIN_TITLEBAR_H;

    blit_surface_clamped(win->client_surface, (int32_t)win->client_w, (int32_t)win->client_h,
                         g_backbuf_surface, (int32_t)g_fb_width, (int32_t)g_fb_height,
                         client_dst_x, client_dst_y, PIXMAN_OP_OVER);
}

/* ==============================================================================
 * Presentation & Dirty Screen Update (Double Buffering + Cursor Save-Under)
 * ============================================================================== */

static inline bool clamp_rect_to_screen(int32_t* in_out_x, int32_t* in_out_y,
                                        int32_t* in_out_w, int32_t* in_out_h,
                                        int32_t screen_w, int32_t screen_h) {
    int32_t x = *in_out_x;
    int32_t y = *in_out_y;
    int32_t w = *in_out_w;
    int32_t h = *in_out_h;

    if (w <= 0 || h <= 0) return false;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x >= screen_w || y >= screen_h || w <= 0 || h <= 0) {
        return false;
    }
    if (x + w > screen_w) {
        w = screen_w - x;
    }
    if (y + h > screen_h) {
        h = screen_h - y;
    }
    if (w <= 0 || h <= 0) return false;

    *in_out_x = x;
    *in_out_y = y;
    *in_out_w = w;
    *in_out_h = h;
    return true;
}

static inline void expand_flush_bounds(int32_t rx, int32_t ry, int32_t rw, int32_t rh,
                                       int32_t* min_x, int32_t* min_y,
                                       int32_t* max_x, int32_t* max_y,
                                       bool* has_flush) {
    if (!(*has_flush)) {
        *min_x = rx;
        *min_y = ry;
        *max_x = rx + rw;
        *max_y = ry + rh;
        *has_flush = true;
    } else {
        if (rx < *min_x) *min_x = rx;
        if (ry < *min_y) *min_y = ry;
        if (rx + rw > *max_x) *max_x = rx + rw;
        if (ry + rh > *max_y) *max_y = ry + rh;
    }
}

void update_screen(int32_t dmg_x, int32_t dmg_y, uint32_t dmg_w, uint32_t dmg_h) {
    if (!g_backbuf_surface || !g_scanout_surface) {
        return;
    }

    int32_t flush_min_x = 0;
    int32_t flush_min_y = 0;
    int32_t flush_max_x = 0;
    int32_t flush_max_y = 0;
    bool has_flush = false;

    int32_t sw = (int32_t)g_fb_width;
    int32_t sh = (int32_t)g_fb_height;

    /* 1. Copy old_cursor_rect from g_backbuf_surface to g_scanout_surface (PIXMAN_OP_SRC) */
    if (g_cursor_visible) {
        int32_t ox = g_last_cursor_x;
        int32_t oy = g_last_cursor_y;
        int32_t ow = CURSOR_SPRITE_W;
        int32_t oh = CURSOR_SPRITE_H;
        if (clamp_rect_to_screen(&ox, &oy, &ow, &oh, sw, sh)) {
            pixman_image_composite32(PIXMAN_OP_SRC,
                                     g_backbuf_surface,
                                     NULL,
                                     g_scanout_surface,
                                     ox, oy,
                                     0, 0,
                                     ox, oy,
                                     ow, oh);
            expand_flush_bounds(ox, oy, ow, oh, &flush_min_x, &flush_min_y, &flush_max_x, &flush_max_y, &has_flush);
        }
    }

    /* 2. Copy any dmg_rect from g_backbuf_surface to g_scanout_surface (PIXMAN_OP_SRC) */
    if (dmg_w > 0 && dmg_h > 0) {
        int32_t dx = dmg_x;
        int32_t dy = dmg_y;
        int32_t dw = (int32_t)dmg_w;
        int32_t dh = (int32_t)dmg_h;
        if (clamp_rect_to_screen(&dx, &dy, &dw, &dh, sw, sh)) {
            pixman_image_composite32(PIXMAN_OP_SRC,
                                     g_backbuf_surface,
                                     NULL,
                                     g_scanout_surface,
                                     dx, dy,
                                     0, 0,
                                     dx, dy,
                                     dw, dh);
            expand_flush_bounds(dx, dy, dw, dh, &flush_min_x, &flush_min_y, &flush_max_x, &flush_max_y, &has_flush);
        }
    }

    /* 3. Copy new_cursor_rect from g_backbuf_surface to g_scanout_surface (PIXMAN_OP_SRC),
     *    and then composite g_cursor_surface onto g_scanout_surface at (g_cursor_x, g_cursor_y) using PIXMAN_OP_OVER */
    if (g_cursor_surface != NULL) {
        int32_t nx = g_cursor_x;
        int32_t ny = g_cursor_y;
        int32_t nw = CURSOR_SPRITE_W;
        int32_t nh = CURSOR_SPRITE_H;
        if (clamp_rect_to_screen(&nx, &ny, &nw, &nh, sw, sh)) {
            pixman_image_composite32(PIXMAN_OP_SRC,
                                     g_backbuf_surface,
                                     NULL,
                                     g_scanout_surface,
                                     nx, ny,
                                     0, 0,
                                     nx, ny,
                                     nw, nh);
            expand_flush_bounds(nx, ny, nw, nh, &flush_min_x, &flush_min_y, &flush_max_x, &flush_max_y, &has_flush);
        }

        blit_surface_clamped(g_cursor_surface, CURSOR_SPRITE_W, CURSOR_SPRITE_H,
                             g_scanout_surface, sw, sh,
                             g_cursor_x, g_cursor_y, PIXMAN_OP_OVER);

        g_cursor_visible = true;
        g_last_cursor_x  = g_cursor_x;
        g_last_cursor_y  = g_cursor_y;
    }

    /* 4. Flush dirty bounding box to VirtIO GPU scanout */
    if (has_flush && flush_max_x > flush_min_x && flush_max_y > flush_min_y) {
        flush_gpu_scanout((uint32_t)flush_min_x, (uint32_t)flush_min_y,
                          (uint32_t)(flush_max_x - flush_min_x),
                          (uint32_t)(flush_max_y - flush_min_y));
    }
}

void composite_and_flush(int32_t dmg_x, int32_t dmg_y, uint32_t dmg_w, uint32_t dmg_h) {
    if (!g_backbuf_surface || !g_scanout_surface || !g_wallpaper_surface) {
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

    /* Apply pixman_region32_t clip to g_backbuf_surface so off-damage pixels are skipped */
    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, dmg_x, dmg_y, dmg_w, dmg_h);
    pixman_image_set_clip_region32(g_backbuf_surface, &clip);

    /* 1. Blit desktop wallpaper for the damaged region into backbuffer */
    pixman_image_composite32(PIXMAN_OP_SRC,
                             g_wallpaper_surface,
                             NULL,
                             g_backbuf_surface,
                             dmg_x, dmg_y,
                             0, 0,
                             dmg_x, dmg_y,
                             (int)dmg_w, (int)dmg_h);

    /* 2. Composite windows in Z-order */
    if (g_num_windows == 0 && g_welcome_surface != NULL) {
        composite_window(&g_welcome_win, !g_crash_dialog_active);
    } else {
        for (size_t i = 0; i < g_num_windows; ++i) {
            bool is_top = (i + 1 == g_num_windows) && !g_crash_dialog_active;
            composite_window(&g_windows[i], is_top);
        }
    }

    /* 3. Composite modal crash dialog on top if active */
    if (g_crash_dialog_active && g_crash_surface != NULL) {
        composite_window(&g_crash_win, true);
    }

    /* Clear clip region on backbuffer */
    pixman_image_set_clip_region32(g_backbuf_surface, NULL);
    pixman_region32_fini(&clip);

    /* 4. Present damaged region with software cursor overlay and flush dirty rect */
    update_screen(dmg_x, dmg_y, dmg_w, dmg_h);
}
