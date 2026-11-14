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
 * 24x24 8x8-MSAA Vector Pointer Cursor with Anti-Aliased Outline & Drop Shadow
 * ============================================================================== */

typedef struct vec2f {
    float x;
    float y;
} vec2f_t;

static const vec2f_t k_cursor_poly[7] = {
    {  2.0f,  2.0f }, /* 0: Tip */
    {  2.0f, 17.8f }, /* 1: Left vertical wing tip */
    {  5.8f, 14.0f }, /* 2: Inner-left wing-to-stem notch */
    {  8.3f, 20.0f }, /* 3: Stem bottom-left */
    { 10.5f, 19.1f }, /* 4: Stem bottom-right */
    {  8.0f, 13.2f }, /* 5: Inner-right stem-to-wing notch */
    { 13.2f, 13.2f }  /* 6: Right wing tip */
};

static bool point_in_cursor_poly(float px, float py) {
    bool inside = false;
    for (int i = 0, j = 6; i < 7; j = i++) {
        float xi = k_cursor_poly[i].x, yi = k_cursor_poly[i].y;
        float xj = k_cursor_poly[j].x, yj = k_cursor_poly[j].y;
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

static float min_dist_sq_to_cursor_poly(float px, float py) {
    float min_d2 = 1e9f;
    for (int i = 0, j = 6; i < 7; j = i++) {
        float ax = k_cursor_poly[j].x, ay = k_cursor_poly[j].y;
        float bx = k_cursor_poly[i].x, by = k_cursor_poly[i].y;
        float vx = bx - ax, vy = by - ay;
        float wx = px - ax, wy = py - ay;
        float len2 = vx * vx + vy * vy;
        float t = (len2 > 0.0f) ? ((wx * vx + wy * vy) / len2) : 0.0f;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
        float dx = px - (ax + t * vx);
        float dy = py - (ay + t * vy);
        float d2 = dx * dx + dy * dy;
        if (d2 < min_d2) {
            min_d2 = d2;
        }
    }
    return min_d2;
}

zx_status_t init_cursor_sprite(void) {
    g_cursor_pixels = (uint32_t*)calloc((size_t)CURSOR_SPRITE_W * CURSOR_SPRITE_H, sizeof(uint32_t));
    if (!g_cursor_pixels) {
        return ZX_ERR_NO_MEMORY;
    }

    const float border_r2      = 1.30f * 1.30f; /* 1.30px crisp white outline */
    const float shadow_core_r2 = 1.15f * 1.15f;
    const float shadow_max_r2  = 2.35f * 2.35f;

    for (uint32_t y = 0; y < CURSOR_SPRITE_H; ++y) {
        for (uint32_t x = 0; x < CURSOR_SPRITE_W; ++x) {
            uint32_t sum_r = 0;
            uint32_t sum_g = 0;
            uint32_t sum_b = 0;
            uint32_t sum_a = 0;

            /* 8x8 (64x) subpixel supersampling in premultiplied ARGB8888 */
            for (uint32_t sy = 0; sy < 8; ++sy) {
                float fy = (float)y + ((float)sy + 0.5f) * 0.125f;
                for (uint32_t sx = 0; sx < 8; ++sx) {
                    float fx = (float)x + ((float)sx + 0.5f) * 0.125f;

                    if (point_in_cursor_poly(fx, fy)) {
                        /* Deep obsidian pointer body (#101016, A=255) */
                        sum_r += 16u;
                        sum_g += 16u;
                        sum_b += 22u;
                        sum_a += 255u;
                    } else {
                        float d2 = min_dist_sq_to_cursor_poly(fx, fy);
                        if (d2 <= border_r2) {
                            /* Pure white anti-aliased border (#FFFFFF, A=255) */
                            sum_r += 255u;
                            sum_g += 255u;
                            sum_b += 255u;
                            sum_a += 255u;
                        } else {
                            /* Soft offset drop shadow at (-0.85, -1.35) */
                            float sh_x = fx - 0.85f;
                            float sh_y = fy - 1.35f;
                            if (point_in_cursor_poly(sh_x, sh_y)) {
                                sum_a += 72u;
                            } else {
                                float sh_d2 = min_dist_sq_to_cursor_poly(sh_x, sh_y);
                                if (sh_d2 <= shadow_core_r2) {
                                    sum_a += 72u;
                                } else if (sh_d2 < shadow_max_r2) {
                                    float t = (shadow_max_r2 - sh_d2) / (shadow_max_r2 - shadow_core_r2);
                                    sum_a += (uint32_t)(72.0f * t);
                                }
                            }
                        }
                    }
                }
            }

            uint32_t r = (sum_r + 32u) >> 6;
            uint32_t g = (sum_g + 32u) >> 6;
            uint32_t b = (sum_b + 32u) >> 6;
            uint32_t a = (sum_a + 32u) >> 6;
            if (r > a) r = a;
            if (g > a) g = a;
            if (b > a) b = a;

            g_cursor_pixels[y * CURSOR_SPRITE_W + x] = (a << 24) | (r << 16) | (g << 8) | b;
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

static inline uint32_t alpha_blend_pixel(uint32_t bg, uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    if (a == 0) return bg;
    if (a >= 255u) return 0xFF000000u | (r << 16) | (g << 8) | b;
    uint32_t bg_a = (bg >> 24) & 0xFFu;
    uint32_t bg_r = (bg >> 16) & 0xFFu;
    uint32_t bg_g = (bg >>  8) & 0xFFu;
    uint32_t bg_b = (bg >>  0) & 0xFFu;
    uint32_t inv  = 255u - a;
    uint32_t ta   = 255u * a + bg_a * inv + 128u;
    uint32_t tr   = r * a + bg_r * inv + 128u;
    uint32_t tg   = g * a + bg_g * inv + 128u;
    uint32_t tb   = b * a + bg_b * inv + 128u;
    return (((ta + (ta >> 8)) >> 8) << 24) |
           (((tr + (tr >> 8)) >> 8) << 16) |
           (((tg + (tg >> 8)) >> 8) <<  8) |
           (((tb + (tb >> 8)) >> 8));
}

static void compute_pixel_clip(const uint32_t* dst_pixels, uint32_t buf_w, uint32_t buf_h,
                               int32_t* cx0, int32_t* cy0, int32_t* cx1, int32_t* cy1) {
    *cx0 = 0;
    *cy0 = 0;
    *cx1 = (int32_t)buf_w;
    *cy1 = (int32_t)buf_h;
    if (dst_pixels == g_backbuf_pixels && g_active_clip.w > 0 && g_active_clip.h > 0) {
        if (g_active_clip.x > *cx0) *cx0 = g_active_clip.x;
        if (g_active_clip.y > *cy0) *cy0 = g_active_clip.y;
        int32_t ax1 = g_active_clip.x + (int32_t)g_active_clip.w;
        int32_t ay1 = g_active_clip.y + (int32_t)g_active_clip.h;
        if (ax1 < *cx1) *cx1 = ax1;
        if (ay1 < *cy1) *cy1 = ay1;
    }
}

void fill_rect_vgradient_over(uint32_t* dst_pixels, uint32_t buf_w, uint32_t buf_h,
                              int32_t x, int32_t y, uint32_t w, uint32_t h,
                              uint32_t top_argb, uint32_t bot_argb) {
    if (!dst_pixels || w == 0 || h == 0) {
        return;
    }

    int32_t cx0, cy0, cx1, cy1;
    compute_pixel_clip(dst_pixels, buf_w, buf_h, &cx0, &cy0, &cx1, &cy1);

    int32_t x0 = (x > cx0) ? x : cx0;
    int32_t y0 = (y > cy0) ? y : cy0;
    int32_t x1 = (x + (int32_t)w < cx1) ? (x + (int32_t)w) : cx1;
    int32_t y1 = (y + (int32_t)h < cy1) ? (y + (int32_t)h) : cy1;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    int32_t ta = (int32_t)((top_argb >> 24) & 0xFFu);
    int32_t tr = (int32_t)((top_argb >> 16) & 0xFFu);
    int32_t tg = (int32_t)((top_argb >>  8) & 0xFFu);
    int32_t tb = (int32_t)((top_argb >>  0) & 0xFFu);

    int32_t ba = (int32_t)((bot_argb >> 24) & 0xFFu);
    int32_t br = (int32_t)((bot_argb >> 16) & 0xFFu);
    int32_t bg = (int32_t)((bot_argb >>  8) & 0xFFu);
    int32_t bb = (int32_t)((bot_argb >>  0) & 0xFFu);

    int32_t denom = (h > 1u) ? (int32_t)(h - 1u) : 1;

    for (int32_t py = y0; py < y1; ++py) {
        int32_t rel_y = py - y;
        uint32_t a = (uint32_t)(ta + ((ba - ta) * rel_y) / denom);
        uint32_t r = (uint32_t)(tr + ((br - tr) * rel_y) / denom);
        uint32_t g = (uint32_t)(tg + ((bg - tg) * rel_y) / denom);
        uint32_t b = (uint32_t)(tb + ((bb - tb) * rel_y) / denom);
        uint32_t* dst_row = dst_pixels + ((size_t)py * buf_w);

        if (a >= 255u) {
            uint32_t solid = 0xFF000000u | (r << 16) | (g << 8) | b;
            for (int32_t px = x0; px < x1; ++px) {
                dst_row[px] = solid;
            }
        } else if (a > 0) {
            for (int32_t px = x0; px < x1; ++px) {
                dst_row[px] = alpha_blend_pixel(dst_row[px], a, r, g, b);
            }
        }
    }
}

void fill_rounded_rect_over(uint32_t* dst_pixels, uint32_t buf_w, uint32_t buf_h,
                            int32_t x, int32_t y, uint32_t w, uint32_t h,
                            uint32_t radius, uint32_t argb) {
    if (!dst_pixels || w == 0 || h == 0) {
        return;
    }

    uint32_t a = (argb >> 24) & 0xFFu;
    uint32_t r = (argb >> 16) & 0xFFu;
    uint32_t g = (argb >>  8) & 0xFFu;
    uint32_t b = (argb >>  0) & 0xFFu;
    if (a == 0) return;

    int32_t rad = (int32_t)radius;
    if (rad * 2 > (int32_t)w) rad = (int32_t)w / 2;
    if (rad * 2 > (int32_t)h) rad = (int32_t)h / 2;
    if (rad <= 0) {
        fill_rect_vgradient_over(dst_pixels, buf_w, buf_h, x, y, w, h, argb, argb);
        return;
    }

    int32_t cx0, cy0, cx1, cy1;
    compute_pixel_clip(dst_pixels, buf_w, buf_h, &cx0, &cy0, &cx1, &cy1);

    int32_t x0 = (x > cx0) ? x : cx0;
    int32_t y0 = (y > cy0) ? y : cy0;
    int32_t x1 = (x + (int32_t)w < cx1) ? (x + (int32_t)w) : cx1;
    int32_t y1 = (y + (int32_t)h < cy1) ? (y + (int32_t)h) : cy1;
    if (x0 >= x1 || y0 >= y1) return;

    int32_t r2 = rad * rad;
    int32_t r_outer2 = (rad + 1) * (rad + 1);

    for (int32_t py = y0; py < y1; ++py) {
        int32_t ly = py - y;
        int32_t dy = 0;
        if (ly < rad) {
            dy = rad - ly;
        } else if (ly >= (int32_t)h - rad) {
            dy = ly - ((int32_t)h - rad - 1);
        }
        uint32_t* dst_row = dst_pixels + ((size_t)py * buf_w);

        for (int32_t px = x0; px < x1; ++px) {
            int32_t lx = px - x;
            int32_t dx = 0;
            if (lx < rad) {
                dx = rad - lx;
            } else if (lx >= (int32_t)w - rad) {
                dx = lx - ((int32_t)w - rad - 1);
            }
            if (dx == 0 || dy == 0) {
                dst_row[px] = alpha_blend_pixel(dst_row[px], a, r, g, b);
                continue;
            }
            int32_t d2 = dx * dx + dy * dy;
            if (d2 <= r2) {
                dst_row[px] = alpha_blend_pixel(dst_row[px], a, r, g, b);
            } else if (d2 < r_outer2) {
                uint32_t fringe = (uint32_t)(((r_outer2 - d2) * 255) / (r_outer2 - r2));
                uint32_t eff_a = (a * fringe + 128u) >> 8;
                dst_row[px] = alpha_blend_pixel(dst_row[px], eff_a, r, g, b);
            }
        }
    }
}

void fill_circle_over(uint32_t* dst_pixels, uint32_t buf_w, uint32_t buf_h,
                      int32_t cx, int32_t cy, int32_t radius, uint32_t argb) {
    if (!dst_pixels || radius <= 0) return;

    uint32_t a = (argb >> 24) & 0xFFu;
    uint32_t r = (argb >> 16) & 0xFFu;
    uint32_t g = (argb >>  8) & 0xFFu;
    uint32_t b = (argb >>  0) & 0xFFu;
    if (a == 0) return;

    int32_t clip_x0, clip_y0, clip_x1, clip_y1;
    compute_pixel_clip(dst_pixels, buf_w, buf_h, &clip_x0, &clip_y0, &clip_x1, &clip_y1);

    int32_t x0 = (cx - radius > clip_x0) ? (cx - radius) : clip_x0;
    int32_t y0 = (cy - radius > clip_y0) ? (cy - radius) : clip_y0;
    int32_t x1 = (cx + radius + 1 < clip_x1) ? (cx + radius + 1) : clip_x1;
    int32_t y1 = (cy + radius + 1 < clip_y1) ? (cy + radius + 1) : clip_y1;
    if (x0 >= x1 || y0 >= y1) return;

    int32_t r2 = radius * radius;
    int32_t r_inner2 = (radius > 1) ? ((radius - 1) * (radius - 1)) : 0;

    for (int32_t py = y0; py < y1; ++py) {
        int32_t dy = py - cy;
        uint32_t* dst_row = dst_pixels + ((size_t)py * buf_w);
        for (int32_t px = x0; px < x1; ++px) {
            int32_t dx = px - cx;
            int32_t d2 = dx * dx + dy * dy;
            if (d2 <= r_inner2) {
                dst_row[px] = alpha_blend_pixel(dst_row[px], a, r, g, b);
            } else if (d2 <= r2) {
                int32_t span = (r2 > r_inner2) ? (r2 - r_inner2) : 1;
                uint32_t fringe = (uint32_t)(((r2 - d2) * 255) / span);
                uint32_t eff_a = (a * fringe + 128u) >> 8;
                dst_row[px] = alpha_blend_pixel(dst_row[px], eff_a, r, g, b);
            }
        }
    }
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

/* ==============================================================================
 * Blocky Pixel-Art Banner ("codename:alexia") & Translucent Default Terminal
 * ============================================================================== */

#define WELCOME_BG_TRANSLUCENT 0xC8101014u /* ~78% opacity premultiplied dark */

/* Tall 5x9 condensed blocky bitmap glyphs for "codename:alexia" */
static const uint8_t k_block_glyphs[11][9] = {
    /* 0: 'c' */ {0x00, 0x00, 0x1F, 0x18, 0x18, 0x18, 0x18, 0x1F, 0x00},
    /* 1: 'o' */ {0x00, 0x00, 0x1F, 0x1B, 0x1B, 0x1B, 0x1B, 0x1F, 0x00},
    /* 2: 'd' */ {0x03, 0x03, 0x1F, 0x1B, 0x1B, 0x1B, 0x1B, 0x1F, 0x00},
    /* 3: 'e' */ {0x00, 0x00, 0x1F, 0x1B, 0x1F, 0x18, 0x18, 0x1F, 0x00},
    /* 4: 'n' */ {0x00, 0x00, 0x1F, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x00},
    /* 5: 'a' */ {0x00, 0x00, 0x1F, 0x03, 0x1F, 0x1B, 0x1B, 0x1F, 0x00},
    /* 6: 'm' */ {0x00, 0x00, 0x1F, 0x15, 0x15, 0x15, 0x15, 0x15, 0x00},
    /* 7: ':' */ {0x00, 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00, 0x00},
    /* 8: 'l' */ {0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0E, 0x00},
    /* 9: 'x' */ {0x00, 0x00, 0x1B, 0x1B, 0x0E, 0x0E, 0x1B, 0x1B, 0x00},
    /* 10:'i' */ {0x0C, 0x00, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x00}
};

static int block_glyph_index(char ch) {
    switch (ch) {
        case 'c': return 0;
        case 'o': return 1;
        case 'd': return 2;
        case 'e': return 3;
        case 'n': return 4;
        case 'a': return 5;
        case 'm': return 6;
        case ':': return 7;
        case 'l': return 8;
        case 'x': return 9;
        case 'i': return 10;
        default:  return -1;
    }
}

static void draw_codename_banner(pixman_image_t* surf, int32_t x, int32_t y) {
    const char* text = "codename:alexia";
    const uint32_t sx = 3u;
    const uint32_t sy = 4u;
    int32_t pen_x = x;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        int idx = block_glyph_index(text[i]);
        if (idx >= 0) {
            for (int r = 0; r < 9; ++r) {
                uint8_t row_bits = k_block_glyphs[idx][r];
                for (int c = 0; c < 5; ++c) {
                    if (row_bits & (0x10u >> c)) {
                        fill_rect_pixman(surf,
                                         pen_x + c * (int32_t)sx,
                                         y + r * (int32_t)sy,
                                         sx, sy, 0xFFFFFFFFu);
                    }
                }
            }
        }
        pen_x += (text[i] == ':' || text[i] == 'i' || text[i] == 'l') ? 14 : 18;
    }
}

void redraw_welcome_input_strip(void) {
    if (!g_welcome_surface || !g_welcome_pixels || g_welcome_win.is_minimized) return;
    uint32_t ww = g_welcome_win.client_w;
    uint32_t wh = g_welcome_win.client_h;

    /* Redraw the prompt line strip (y = 58..84) with translucent background */
    fill_rect_pixman(g_welcome_surface, 8, 58, ww - 16u, 26u, WELCOME_BG_TRANSLUCENT);
    char input_line[96];
    snprintf(input_line, sizeof(input_line), "admin@irisOS-dev [~] $ %s", g_welcome_typed);
    draw_text_ft(g_welcome_pixels, ww, wh, 12, 62, 13, input_line, 0xFFE5E7EBu);

    int32_t strip_x = g_welcome_win.x + IRIS_WIN_BORDER_PX + 8;
    int32_t strip_y = g_welcome_win.y + IRIS_WIN_BORDER_PX + IRIS_WIN_TITLEBAR_H + 58;
    dirty_rect_union(strip_x, strip_y, ww - 16u, 26u);
}

void render_welcome_surface_contents(void) {
    if (!g_welcome_surface || !g_welcome_pixels) {
        return;
    }
    uint32_t ww = g_welcome_win.client_w;
    uint32_t wh = g_welcome_win.client_h;

    /* Fill surface with premultiplied translucent dark (0xC8101014) */
    fill_rect_pixman(g_welcome_surface, 0, 0, ww, wh, WELCOME_BG_TRANSLUCENT);

    /* Render blocky pixel banner "codename:alexia" */
    draw_codename_banner(g_welcome_surface, 12, 14);

    /* Render dynamic-style shell prompt line */
    char input_line[96];
    snprintf(input_line, sizeof(input_line), "admin@irisOS-dev [~] $ %s", g_welcome_typed);
    draw_text_ft(g_welcome_pixels, ww, wh, 12, 62, 13, input_line, 0xFFE5E7EBu);
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

bool                    g_unlock_active    = false;
static comp_window_t    g_unlock_win;
static pixman_image_t*  g_unlock_surface   = NULL;
static uint32_t*        g_unlock_pixels    = NULL;
static zx_handle_t      g_unlock_crypt_ch  = ZX_HANDLE_INVALID;
static char             g_unlock_pw[128];
static size_t           g_unlock_pw_len    = 0;
static char             g_unlock_status[96] = "Enter passphrase to unlock /Users and /Applications.";
static uint32_t         g_unlock_status_col = 0xFFA6ADC8u;

static void render_unlock_prompt_contents(void) {
    if (!g_unlock_surface || !g_unlock_pixels) return;
    uint32_t cw = g_unlock_win.client_w;
    uint32_t ch = g_unlock_win.client_h;

    fill_rect_pixman(g_unlock_surface, 0, 0, cw, ch, 0xFF1E1E2Eu);

    draw_text_ft_styled(g_unlock_pixels, cw, ch, 24, 22, 16,
                        FONT_STYLE_BOLD, "Disk Encryption Unlock", 0xFFCDD6F4u);
    draw_text_ft_styled(g_unlock_pixels, cw, ch, 24, 52, 13,
                        FONT_STYLE_REGULAR, g_unlock_status, g_unlock_status_col);

    uint32_t box_x = 24u, box_y = 84u, box_w = cw - 48u, box_h = 36u;
    fill_rect_pixman(g_unlock_surface, (int32_t)box_x, (int32_t)box_y, box_w, box_h, 0xFF11111Bu);
    fill_rect_pixman(g_unlock_surface, (int32_t)box_x, (int32_t)box_y, box_w, 1u, 0xFF89B4FAu);
    fill_rect_pixman(g_unlock_surface, (int32_t)box_x, (int32_t)(box_y + box_h - 1u), box_w, 1u, 0xFF89B4FAu);
    fill_rect_pixman(g_unlock_surface, (int32_t)box_x, (int32_t)box_y, 1u, box_h, 0xFF89B4FAu);
    fill_rect_pixman(g_unlock_surface, (int32_t)(box_x + box_w - 1u), (int32_t)box_y, 1u, box_h, 0xFF89B4FAu);

    char masked[128];
    size_t vis = g_unlock_pw_len < 48 ? g_unlock_pw_len : 48;
    for (size_t i = 0; i < vis; ++i) masked[i] = '*';
    masked[vis] = '_';
    masked[vis + 1] = '\0';
    draw_string_8x8(g_unlock_pixels, cw, ch, (int32_t)box_x + 12, (int32_t)box_y + 14, masked, 0xFFCDD6F4u);

    draw_text_ft_styled(g_unlock_pixels, cw, ch, 24, 136, 12,
                        FONT_STYLE_REGULAR, "Press Enter to unlock (AES-256-XTS + Argon2id)", 0xFF7F849Cu);
}

void close_unlock_prompt(void) {
    if (!g_unlock_active && g_unlock_surface == NULL) return;
    int32_t wx = g_unlock_win.x;
    int32_t wy = g_unlock_win.y;
    uint32_t fw = g_unlock_win.client_w + 2u * IRIS_WIN_BORDER_PX;
    uint32_t fh = g_unlock_win.client_h + IRIS_WIN_TITLEBAR_H + 2u * IRIS_WIN_BORDER_PX;

    g_unlock_active = false;
    memset(g_unlock_pw, 0, sizeof(g_unlock_pw));
    g_unlock_pw_len = 0;
    if (g_unlock_crypt_ch != ZX_HANDLE_INVALID) {
        zx_handle_close(g_unlock_crypt_ch);
        g_unlock_crypt_ch = ZX_HANDLE_INVALID;
    }
    if (g_unlock_surface) { pixman_image_unref(g_unlock_surface); g_unlock_surface = NULL; }
    if (g_unlock_pixels)  { free(g_unlock_pixels); g_unlock_pixels = NULL; }
    dirty_rect_union(wx, wy, fw, fh);
}

void init_unlock_prompt(zx_handle_t crypt_ch) {
    if (g_unlock_active || crypt_ch == ZX_HANDLE_INVALID) return;
    uint32_t cw = 460u, ch = 172u;
    g_unlock_pixels = (uint32_t*)calloc((size_t)cw * ch, sizeof(uint32_t));
    if (!g_unlock_pixels) { zx_handle_close(crypt_ch); return; }
    g_unlock_surface = pixman_image_create_bits(PIXMAN_a8r8g8b8, (int)cw, (int)ch, g_unlock_pixels, (int)(cw * 4u));
    if (!g_unlock_surface) { free(g_unlock_pixels); g_unlock_pixels = NULL; zx_handle_close(crypt_ch); return; }

    g_unlock_crypt_ch = crypt_ch;
    memset(g_unlock_pw, 0, sizeof(g_unlock_pw));
    g_unlock_pw_len = 0;
    snprintf(g_unlock_status, sizeof(g_unlock_status), "Enter passphrase to unlock /Users and /Applications.");
    g_unlock_status_col = 0xFFA6ADC8u;

    memset(&g_unlock_win, 0, sizeof(g_unlock_win));
    g_unlock_win.window_id = 0xFFFDu;
    g_unlock_win.client_w = cw;
    g_unlock_win.client_h = ch;
    g_unlock_win.x = (int32_t)(g_fb_width - (cw + 2u * IRIS_WIN_BORDER_PX)) / 2;
    g_unlock_win.y = (int32_t)(g_fb_height - (ch + IRIS_WIN_TITLEBAR_H + 2u * IRIS_WIN_BORDER_PX)) / 2;
    g_unlock_win.client_surface = g_unlock_surface;
    strncpy(g_unlock_win.title, "irisOS Security", sizeof(g_unlock_win.title) - 1);

    render_unlock_prompt_contents();
    g_unlock_active = true;
    dirty_rect_union(g_unlock_win.x, g_unlock_win.y,
                     cw + 2u * IRIS_WIN_BORDER_PX,
                     ch + IRIS_WIN_TITLEBAR_H + 2u * IRIS_WIN_BORDER_PX);
}

void unlock_prompt_handle_key(char ch) {
    if (!g_unlock_active) return;

    if (ch == '\b' || (uint8_t)ch == 0x7Fu) {
        if (g_unlock_pw_len > 0) {
            g_unlock_pw[--g_unlock_pw_len] = '\0';
        }
    } else if (ch == '\r' || ch == '\n') {
        snprintf(g_unlock_status, sizeof(g_unlock_status), "Deriving key (Argon2id)...");
        g_unlock_status_col = 0xFF89B4FAu;
        render_unlock_prompt_contents();
        composite_and_flush(g_unlock_win.x, g_unlock_win.y,
                            g_unlock_win.client_w + 2u * IRIS_WIN_BORDER_PX,
                            g_unlock_win.client_h + IRIS_WIN_TITLEBAR_H + 2u * IRIS_WIN_BORDER_PX);

        uint8_t msg[sizeof(iris_msg_hdr_t) + sizeof(iris_crypt_unlock_req_t)] __attribute__((aligned(8)));
        memset(msg, 0, sizeof(msg));
        iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)msg;
        iris_crypt_unlock_req_t* req = (iris_crypt_unlock_req_t*)(msg + sizeof(iris_msg_hdr_t));
        req->passphrase_len = (uint32_t)g_unlock_pw_len;
        if (g_unlock_pw_len > 0) {
            memcpy(req->passphrase, g_unlock_pw, g_unlock_pw_len);
        }
        memset(g_unlock_pw, 0, sizeof(g_unlock_pw));
        g_unlock_pw_len = 0;

        static uint32_t s_unlock_txid = 1;
        iris_msg_hdr_init(hdr, IRIS_PROTO_CRYPT, IRIS_CRYPT_OP_UNLOCK, ++s_unlock_txid, sizeof(iris_crypt_unlock_req_t));
        zx_status_t st = zx_channel_write(g_unlock_crypt_ch, 0, msg, sizeof(msg), NULL, 0);
        memset(req, 0, sizeof(*req));

        if (st == ZX_OK) {
            zx_signals_t obs = 0;
            st = zx_object_wait_one(g_unlock_crypt_ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                    ZX_TIME_INFINITE, &obs);
            if (st == ZX_OK && (obs & ZX_CHANNEL_READABLE)) {
                iris_msg_hdr_t rep;
                uint32_t ab = 0, ah = 0;
                if (zx_channel_read(g_unlock_crypt_ch, 0, &rep, NULL, sizeof(rep), 0, &ab, &ah) == ZX_OK &&
                    ab >= sizeof(rep)) {
                    st = rep.status;
                } else {
                    st = ZX_ERR_IO;
                }
            } else {
                st = ZX_ERR_PEER_CLOSED;
            }
        }

        if (st == ZX_OK) {
            log_status(STATUS_OKAY, "iris_compositor", "Data partition unlocked via graphical prompt.");
            close_unlock_prompt();
            return;
        }
        snprintf(g_unlock_status, sizeof(g_unlock_status), "Invalid passphrase. Please try again.");
        g_unlock_status_col = 0xFFF38BA8u;
    } else if ((uint8_t)ch >= 32 && (uint8_t)ch <= 126) {
        if (g_unlock_pw_len + 1 < sizeof(g_unlock_pw)) {
            g_unlock_pw[g_unlock_pw_len++] = ch;
            g_unlock_pw[g_unlock_pw_len] = '\0';
        }
    }

    render_unlock_prompt_contents();
    dirty_rect_union(g_unlock_win.x, g_unlock_win.y,
                     g_unlock_win.client_w + 2u * IRIS_WIN_BORDER_PX,
                     g_unlock_win.client_h + IRIS_WIN_TITLEBAR_H + 2u * IRIS_WIN_BORDER_PX);
}

/* ==============================================================================
 * Lossless QOI Decoder & Dynamic Aspect-Fill Wallpaper Scaler
 * ============================================================================== */

static bool load_qoi_wallpaper_aspect_fill(const char* path, pixman_image_t* dst_surface,
                                           uint32_t dst_w, uint32_t dst_h) {
    if (!path || !dst_surface || dst_w == 0 || dst_h == 0) {
        return false;
    }

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long fsize = ftell(fp);
    if (fsize < 22 || fsize > 16 * 1024 * 1024) {
        fclose(fp);
        return false;
    }
    rewind(fp);

    uint8_t* data = (uint8_t*)malloc((size_t)fsize);
    if (!data) {
        fclose(fp);
        return false;
    }

    size_t nread = fread(data, 1, (size_t)fsize, fp);
    fclose(fp);
    if (nread != (size_t)fsize) {
        free(data);
        return false;
    }

    /* Validate 14-byte QOI header ("qoif") */
    if (data[0] != 'q' || data[1] != 'o' || data[2] != 'i' || data[3] != 'f') {
        free(data);
        return false;
    }

    uint32_t src_w = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                     ((uint32_t)data[6] << 8)  |  (uint32_t)data[7];
    uint32_t src_h = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) |
                     ((uint32_t)data[10] << 8) |  (uint32_t)data[11];
    uint8_t channels = data[12];

    if (src_w == 0 || src_h == 0 || src_w > 8192u || src_h > 8192u || (channels != 3 && channels != 4)) {
        free(data);
        return false;
    }

    size_t total_px = (size_t)src_w * (size_t)src_h;
    uint32_t* src_pixels = (uint32_t*)malloc(total_px * sizeof(uint32_t));
    if (!src_pixels) {
        free(data);
        return false;
    }

    uint32_t index[64];
    memset(index, 0, sizeof(index));

    uint8_t r = 0, g = 0, b = 0, a = 255;
    size_t pos = 14;
    size_t end_pos = (size_t)fsize - 8;
    uint32_t run = 0;

    for (size_t px = 0; px < total_px; ++px) {
        if (run > 0) {
            run--;
        } else if (pos < end_pos) {
            uint8_t b1 = data[pos++];
            if (b1 == 0xFEu) { /* QOI_OP_RGB */
                if (pos + 3 <= end_pos) {
                    r = data[pos++];
                    g = data[pos++];
                    b = data[pos++];
                }
            } else if (b1 == 0xFFu) { /* QOI_OP_RGBA */
                if (pos + 4 <= end_pos) {
                    r = data[pos++];
                    g = data[pos++];
                    b = data[pos++];
                    a = data[pos++];
                }
            } else if ((b1 & 0xC0u) == 0x00u) { /* QOI_OP_INDEX */
                uint32_t c = index[b1 & 63u];
                r = (uint8_t)((c >> 16) & 0xFFu);
                g = (uint8_t)((c >> 8)  & 0xFFu);
                b = (uint8_t)(c & 0xFFu);
                a = (uint8_t)((c >> 24) & 0xFFu);
            } else if ((b1 & 0xC0u) == 0x40u) { /* QOI_OP_DIFF */
                r = (uint8_t)(r + ((int)((b1 >> 4) & 0x03u) - 2));
                g = (uint8_t)(g + ((int)((b1 >> 2) & 0x03u) - 2));
                b = (uint8_t)(b + ((int)(b1 & 0x03u) - 2));
            } else if ((b1 & 0xC0u) == 0x80u) { /* QOI_OP_LUMA */
                if (pos < end_pos) {
                    uint8_t b2 = data[pos++];
                    int vg = (int)(b1 & 0x3Fu) - 32;
                    r = (uint8_t)(r + vg - 8 + ((int)((b2 >> 4) & 0x0Fu)));
                    g = (uint8_t)(g + vg);
                    b = (uint8_t)(b + vg - 8 + ((int)(b2 & 0x0Fu)));
                }
            } else if ((b1 & 0xC0u) == 0xC0u) { /* QOI_OP_RUN */
                run = (uint32_t)(b1 & 0x3Fu);
            }
            uint32_t hash = ((uint32_t)r * 3u + (uint32_t)g * 5u + (uint32_t)b * 7u + (uint32_t)a * 11u) & 63u;
            index[hash] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
        src_pixels[px] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    free(data);

    pixman_image_t* src_img = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                                       (int)src_w, (int)src_h,
                                                       src_pixels, (int)(src_w * 4u));
    if (!src_img) {
        free(src_pixels);
        return false;
    }

    /* Compute dynamic aspect-fill ("cover") transform from dst (dst_w x dst_h) -> src (src_w x src_h) */
    double scale_x = (double)src_w / (double)dst_w;
    double scale_y = (double)src_h / (double)dst_h;
    double scale   = (scale_x < scale_y) ? scale_x : scale_y;
    double off_x   = ((double)src_w - (double)dst_w * scale) * 0.5;
    double off_y   = ((double)src_h - (double)dst_h * scale) * 0.5;

    pixman_transform_t xform;
    pixman_transform_init_identity(&xform);
    xform.matrix[0][0] = pixman_double_to_fixed(scale);
    xform.matrix[1][1] = pixman_double_to_fixed(scale);
    xform.matrix[0][2] = pixman_double_to_fixed(off_x);
    xform.matrix[1][2] = pixman_double_to_fixed(off_y);

    pixman_image_set_transform(src_img, &xform);
    pixman_image_set_filter(src_img, PIXMAN_FILTER_BILINEAR, NULL, 0);
    pixman_image_set_repeat(src_img, PIXMAN_REPEAT_PAD);

    pixman_image_composite32(PIXMAN_OP_SRC,
                             src_img, NULL, dst_surface,
                             0, 0, 0, 0, 0, 0,
                             (int)dst_w, (int)dst_h);

    pixman_image_unref(src_img);
    free(src_pixels);
    return true;
}

/* ==============================================================================
 * SVG Cubic-Bezier Path Rasterizer for /System/Resources/eye_light.svg
 * ============================================================================== */

typedef struct svg_seg {
    float x0, y0, x1, y1;
} svg_seg_t;

#define MAX_SVG_SEGS 512

typedef struct svg_path_poly {
    svg_seg_t segs[MAX_SVG_SEGS];
    size_t    num_segs;
    bool      is_gradient;
} svg_path_poly_t;

static const char* const k_eye_light_paths[9] = {
    "M329.845 58.8253L315.171 68.016C315.171 68.016 312.978 64.7302 307.943 59.7036C310.791 55.5469 313.195 51.6085 315.171 47.6765C324.586 54.2416 329.845 58.8253 329.845 58.8253Z",
    "M323.053 0C323.053 0 328.088 22.0868 315.171 47.6766C311.444 45.051 306.846 42.2081 301.812 38.9297C312.541 28.209 320.641 15.3045 323.053 0Z",
    "M315.171 47.6766C313.195 51.6085 310.79 55.547 307.943 59.7037C304.216 56.2071 298.964 51.8258 291.953 47.2421C295.455 44.8337 298.739 41.9898 301.812 38.9297C306.846 42.208 311.444 45.0509 315.171 47.6766Z",
    "M307.943 59.7038C298.964 72.1645 285.168 84.6335 263.92 95.3468C250.117 89.4429 234.353 84.6335 216.831 81.3626C228.44 78.5113 264.791 68.2334 291.954 47.2422C298.964 51.8259 304.216 56.2073 307.943 59.7038Z",
    "M290.421 108.251C307.29 109.781 349.99 108.912 380 67.5808C380 67.5808 373.869 122.903 318.455 128.59C311.009 121.808 301.812 114.816 290.421 108.251Z",
    "M178.722 129.25C180.254 136.242 184.199 142.153 189.895 145.867C192.081 151.337 193.178 157.459 193.178 163.791C193.178 188.075 175.874 207.964 154.85 207.964C133.601 207.964 116.305 188.075 116.305 163.791C116.305 139.303 133.601 119.617 154.85 119.617C163.829 119.617 172.154 123.338 178.722 129.25Z",
    "M160.546 11.8154C221.212 0.435396 272.02 21.6513 301.812 38.9297C298.739 41.9898 295.456 44.8337 291.954 47.2421C273.778 35.4257 243.332 22.5222 195.147 19.0257C104.471 12.469 28.2584 98.6325 23.6603 103.877C37.6733 86.1644 73.815 28.2089 160.546 11.8154Z",
    "M242.236 107.816C263.92 206.876 203.908 245.153 146.308 245.153C143.46 245.153 140.837 245.153 138.208 244.935C79.2919 241.215 73.815 153.302 87.3919 120.937C109.076 103.877 132.505 90.5308 153.753 88.5644C180.473 85.9462 213.33 95.1294 242.236 107.816ZM229.537 123.338C229.537 108.686 218.146 96.6669 203.908 96.6669C189.676 96.6669 178.061 108.686 178.061 123.338C178.061 125.304 178.278 127.277 178.722 129.25C172.154 123.338 163.829 119.617 154.85 119.617C133.601 119.617 116.305 139.303 116.305 163.791C116.305 188.075 133.601 207.964 154.85 207.964C175.874 207.964 193.178 188.075 193.178 163.791C193.178 157.459 192.081 151.337 189.895 145.867C193.832 148.704 198.649 150.241 203.908 150.241C218.146 150.241 229.537 138.207 229.537 123.338Z",
    "M318.454 128.59C331.378 139.956 338.164 150.459 338.606 155.928C340.14 172.981 290.421 232.249 282.096 236.181C273.996 239.901 262.605 231.154 262.605 231.154C262.605 231.154 293.704 201.632 308.161 178.893C312.977 171.233 316.043 164.24 315.171 159.424C313.639 148.486 281.216 124.869 242.236 107.816C213.33 95.1295 180.473 85.9462 153.753 88.5645C132.505 90.5309 109.076 103.877 87.3919 120.938C56.2853 145.643 28.913 177.79 16.8676 194.85C12.0445 201.414 9.42226 205.999 9.42226 207.529C9.85743 214.754 38.5529 229.841 38.5529 229.841C38.5529 229.841 0 227.868 0 210.38C0 192.877 42.9334 76.3285 159.667 75.8782C180.473 75.8782 199.535 77.8586 216.831 81.3625C234.353 84.6334 250.117 89.4428 263.92 95.3468C273.777 99.2852 282.539 103.659 290.421 108.251C301.812 114.815 311.009 121.807 318.454 128.59Z"
};

static const char* svg_skip_ws(const char* p) {
    while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static float svg_parse_float(const char** io_p) {
    const char* p = svg_skip_ws(*io_p);
    float sign = 1.0f;
    if (*p == '-') { sign = -1.0f; p++; }
    else if (*p == '+') { p++; }
    float val = 0.0f;
    while (*p >= '0' && *p <= '9') {
        val = val * 10.0f + (float)(*p - '0');
        p++;
    }
    if (*p == '.') {
        p++;
        float frac = 0.1f;
        while (*p >= '0' && *p <= '9') {
            val += (float)(*p - '0') * frac;
            frac *= 0.1f;
            p++;
        }
    }
    *io_p = p;
    return val * sign;
}

static void svg_add_seg(svg_path_poly_t* poly, float x0, float y0, float x1, float y1) {
    if (poly->num_segs < MAX_SVG_SEGS && (x0 != x1 || y0 != y1)) {
        poly->segs[poly->num_segs++] = (svg_seg_t){ x0, y0, x1, y1 };
    }
}

static void svg_parse_path_string(const char* d_str, bool is_grad, svg_path_poly_t* out_poly) {
    memset(out_poly, 0, sizeof(*out_poly));
    out_poly->is_gradient = is_grad;

    const char* p = d_str;
    float cur_x = 0.0f, cur_y = 0.0f;
    float start_x = 0.0f, start_y = 0.0f;
    char cmd = 0;

    while (*p) {
        p = svg_skip_ws(p);
        if (!*p) break;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            cmd = *p++;
        }
        if (cmd == 'M') {
            cur_x = start_x = svg_parse_float(&p);
            cur_y = start_y = svg_parse_float(&p);
            cmd = 'L';
        } else if (cmd == 'L') {
            float nx = svg_parse_float(&p);
            float ny = svg_parse_float(&p);
            svg_add_seg(out_poly, cur_x, cur_y, nx, ny);
            cur_x = nx;
            cur_y = ny;
        } else if (cmd == 'C') {
            float x1 = svg_parse_float(&p);
            float y1 = svg_parse_float(&p);
            float x2 = svg_parse_float(&p);
            float y2 = svg_parse_float(&p);
            float x3 = svg_parse_float(&p);
            float y3 = svg_parse_float(&p);
            float px = cur_x, py = cur_y;
            const int steps = 10;
            for (int s = 1; s <= steps; ++s) {
                float t = (float)s / (float)steps;
                float u = 1.0f - t;
                float b0 = u * u * u;
                float b1 = 3.0f * u * u * t;
                float b2 = 3.0f * u * t * t;
                float b3 = t * t * t;
                float qx = b0 * cur_x + b1 * x1 + b2 * x2 + b3 * x3;
                float qy = b0 * cur_y + b1 * y1 + b2 * y2 + b3 * y3;
                svg_add_seg(out_poly, px, py, qx, qy);
                px = qx;
                py = qy;
            }
            cur_x = x3;
            cur_y = y3;
        } else if (cmd == 'Z' || cmd == 'z') {
            svg_add_seg(out_poly, cur_x, cur_y, start_x, start_y);
            cur_x = start_x;
            cur_y = start_y;
            cmd = 0;
        } else {
            p++;
        }
    }
}

static bool svg_point_in_poly(const svg_path_poly_t* poly, float px, float py) {
    bool inside = false;
    for (size_t i = 0; i < poly->num_segs; ++i) {
        float x0 = poly->segs[i].x0, y0 = poly->segs[i].y0;
        float x1 = poly->segs[i].x1, y1 = poly->segs[i].y1;
        if (((y0 > py) != (y1 > py)) &&
            (px < (x1 - x0) * (py - y0) / (y1 - y0) + x0)) {
            inside = !inside;
        }
    }
    return inside;
}

void render_eye_light_svg(uint32_t* dst_pixels, uint32_t fb_w, uint32_t fb_h,
                          int32_t dst_x, int32_t dst_y, uint32_t dst_w, uint32_t dst_h) {
    svg_path_poly_t* polys = (svg_path_poly_t*)calloc(9, sizeof(svg_path_poly_t));
    if (!polys) return;

    size_t num_polys = 0;
    /* Attempt to parse /System/Resources/eye_light.svg directly from VFS */
    FILE* fp = fopen(EYE_LIGHT_SVG_PATH, "rb");
    if (fp) {
        char svg_buf[4096];
        size_t nr = fread(svg_buf, 1, sizeof(svg_buf) - 1, fp);
        fclose(fp);
        svg_buf[nr] = '\0';

        const char* cur = svg_buf;
        while (num_polys < 9 && (cur = strstr(cur, "<path")) != NULL) {
            const char* tag_end = strchr(cur, '>');
            if (!tag_end) break;
            const char* d_attr = strstr(cur, "d=\"");
            if (d_attr && d_attr < tag_end) {
                d_attr += 3;
                const char* d_end = strchr(d_attr, '"');
                if (d_end && d_end < tag_end) {
                    size_t d_len = (size_t)(d_end - d_attr);
                    char d_copy[1024];
                    if (d_len >= sizeof(d_copy)) d_len = sizeof(d_copy) - 1;
                    memcpy(d_copy, d_attr, d_len);
                    d_copy[d_len] = '\0';

                    const char* fill_url = strstr(cur, "fill=\"url(");
                    bool is_grad = (fill_url != NULL && fill_url < tag_end);
                    svg_parse_path_string(d_copy, is_grad, &polys[num_polys++]);
                }
            }
            cur = tag_end + 1;
        }
    }

    /* Fallback to embedded paths from eye_light.svg if VFS file was not loaded */
    if (num_polys == 0) {
        for (size_t i = 0; i < 9; ++i) {
            svg_parse_path_string(k_eye_light_paths[i], (i == 7), &polys[i]);
        }
        num_polys = 9;
    }

    const float vb_w = 380.0f;
    const float vb_h = 248.0f;

    for (uint32_t py = 0; py < dst_h; ++py) {
        int32_t sy_screen = dst_y + (int32_t)py;
        if (sy_screen < 0 || (uint32_t)sy_screen >= fb_h) continue;

        for (uint32_t px = 0; px < dst_w; ++px) {
            int32_t sx_screen = dst_x + (int32_t)px;
            if (sx_screen < 0 || (uint32_t)sx_screen >= fb_w) continue;

            uint32_t sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;

            /* 4x4 subpixel supersampling in SVG viewBox (0..380 x 0..248) */
            for (int sub_y = 0; sub_y < 4; ++sub_y) {
                float vy = ((float)py + ((float)sub_y + 0.5f) * 0.25f) * (vb_h / (float)dst_h);
                for (int sub_x = 0; sub_x < 4; ++sub_x) {
                    float vx = ((float)px + ((float)sub_x + 0.5f) * 0.25f) * (vb_w / (float)dst_w);

                    bool hit = false;
                    bool hit_grad = false;
                    for (size_t p = 0; p < num_polys; ++p) {
                        if (svg_point_in_poly(&polys[p], vx, vy)) {
                            hit = true;
                            hit_grad = polys[p].is_gradient;
                        }
                    }

                    if (hit) {
                        sum_a += 255u;
                        if (hit_grad) {
                            /* Linear gradient #54B5FF (y=88.12) -> #326C99 (y=245.15) */
                            float t = (vy - 88.1167f) / (245.153f - 88.1167f);
                            if (t < 0.0f) t = 0.0f;
                            else if (t > 1.0f) t = 1.0f;
                            sum_r += (uint32_t)(84.0f  + (50.0f  - 84.0f)  * t);
                            sum_g += (uint32_t)(181.0f + (108.0f - 181.0f) * t);
                            sum_b += (uint32_t)(255.0f + (153.0f - 255.0f) * t);
                        } else {
                            sum_r += 12u;
                            sum_g += 14u;
                            sum_b += 18u;
                        }
                    }
                }
            }

            if (sum_a > 0) {
                uint32_t cov = (sum_a + 8u) >> 4;
                uint32_t fg_r = sum_r / (sum_a / 255u);
                uint32_t fg_g = sum_g / (sum_a / 255u);
                uint32_t fg_b = sum_b / (sum_a / 255u);

                uint32_t* dst_px = &dst_pixels[(size_t)sy_screen * fb_w + (size_t)sx_screen];
                uint32_t bg = *dst_px;
                uint32_t bg_r = (bg >> 16) & 0xFFu;
                uint32_t bg_g = (bg >> 8)  & 0xFFu;
                uint32_t bg_b = bg & 0xFFu;
                uint32_t inv = 255u - cov;

                uint32_t out_r = (fg_r * cov + bg_r * inv + 127u) / 255u;
                uint32_t out_g = (fg_g * cov + bg_g * inv + 127u) / 255u;
                uint32_t out_b = (fg_b * cov + bg_b * inv + 127u) / 255u;
                *dst_px = 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
            }
        }
    }

    free(polys);
}

/* ==============================================================================
 * Windows-Like Bottom Taskbar (Start Eye Logo, Terminal Icon & System Tray)
 * ============================================================================== */

static void draw_line_thick(pixman_image_t* surf, int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1, uint32_t color) {
    int32_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int32_t dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int32_t steps = (dx > dy) ? dx : dy;
    if (steps == 0) {
        fill_rect_pixman(surf, x0, y0, 2, 2, color);
        return;
    }
    for (int32_t s = 0; s <= steps; ++s) {
        int32_t px = x0 + (x1 - x0) * s / steps;
        int32_t py = y0 + (y1 - y0) * s / steps;
        fill_rect_pixman(surf, px, py, 2, 2, color);
    }
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

    /* 1. Try loading photographic wallpaper from /System/Resources/wallpaper.qoi with dynamic aspect-fill */
    if (!load_qoi_wallpaper_aspect_fill(WALLPAPER_QOI_PATH, g_wallpaper_surface, g_fb_width, g_fb_height)) {
        /* 2. Fallback: smooth diagonal/vertical linear gradient across desktop using Pixman */
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
    }

    /* 3. Pure photographic wallpaper surface (taskbar rendered dynamically on top) */

    /* 4. Initialize default translucent terminal welcome window ("toybox - ~/") */
    uint32_t ww = 580u;
    uint32_t wh = 360u;
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
            g_welcome_win.flags          = IRIS_WIN_FLAG_TRANSLUCENT;
            g_welcome_win.x              = 52;
            g_welcome_win.y              = 72;
            g_welcome_win.client_surface = g_welcome_surface;
            memcpy(g_welcome_win.title, "toybox - ~/", 12);

            render_welcome_surface_contents();
        }
    }

    return ZX_OK;
}

/* ==============================================================================
 * Window Compositing Pipeline (With Damage Culling & Occlusion Fast-Path)
 * ============================================================================== */

comp_dirty_rect_t g_active_clip = { 0, 0, 0, 0 };

static inline bool rects_intersect(int32_t ax, int32_t ay, int32_t aw, int32_t ah,
                                   int32_t bx, int32_t by, int32_t bw, int32_t bh) {
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) {
        return false;
    }
    return (ax < bx + bw) && (ax + aw > bx) &&
           (ay < by + bh) && (ay + ah > by);
}

static inline bool rect_contains(int32_t outer_x, int32_t outer_y, int32_t outer_w, int32_t outer_h,
                                 int32_t inner_x, int32_t inner_y, int32_t inner_w, int32_t inner_h) {
    return (inner_x >= outer_x) &&
           (inner_y >= outer_y) &&
           (inner_x + inner_w <= outer_x + outer_w) &&
           (inner_y + inner_h <= outer_y + outer_h);
}

void composite_window(const comp_window_t* win, bool is_focused) {
    if (!win || !win->client_surface || win->is_minimized) {
        return;
    }

    uint32_t frame_w = win->client_w + (2u * IRIS_WIN_BORDER_PX);
    uint32_t frame_h = win->client_h + IRIS_WIN_TITLEBAR_H + (2u * IRIS_WIN_BORDER_PX);

    /* Quick reject if window is completely outside screen bounds */
    if (win->x + (int32_t)frame_w <= 0 || win->x >= (int32_t)g_fb_width ||
        win->y + (int32_t)frame_h <= 0 || win->y >= (int32_t)g_fb_height) {
        return;
    }

    /* Quick reject if window does not intersect the active damage clip */
    if (g_active_clip.w > 0 && g_active_clip.h > 0) {
        if (!rects_intersect(win->x, win->y, (int32_t)frame_w, (int32_t)frame_h,
                             g_active_clip.x, g_active_clip.y,
                             (int32_t)g_active_clip.w, (int32_t)g_active_clip.h)) {
            return;
        }
    }

    int32_t client_dst_x = win->x + IRIS_WIN_BORDER_PX;
    int32_t client_dst_y = win->y + IRIS_WIN_BORDER_PX + IRIS_WIN_TITLEBAR_H;

    /* Check if damage is strictly contained inside the client content area */
    bool client_only = (g_active_clip.w > 0 && g_active_clip.h > 0) &&
                       rect_contains(client_dst_x, client_dst_y,
                                     (int32_t)win->client_w, (int32_t)win->client_h,
                                     g_active_clip.x, g_active_clip.y,
                                     (int32_t)g_active_clip.w, (int32_t)g_active_clip.h);

    if (!client_only) {
        uint32_t border_color   = is_focused ? 0xFFD0D5DDu : 0xFF9CA3AFu;
        uint32_t tb_top_color   = is_focused ? 0xFFFFFFFFu : 0xF2F4F5F7u;
        uint32_t tb_bot_color   = is_focused ? 0xBFFFFFFFu : 0xB3E5E7EBu;
        uint32_t title_fg_color = is_focused ? 0xFF141418u : 0xFF6B7280u;

        /* 1. Outer 1px border around entire window frame */
        fill_rect_pixman(g_backbuf_surface, win->x, win->y, frame_w, IRIS_WIN_BORDER_PX, border_color);
        fill_rect_pixman(g_backbuf_surface, win->x, win->y + (int32_t)frame_h - IRIS_WIN_BORDER_PX,
                         frame_w, IRIS_WIN_BORDER_PX, border_color);
        fill_rect_pixman(g_backbuf_surface, win->x, win->y, IRIS_WIN_BORDER_PX, frame_h, border_color);
        fill_rect_pixman(g_backbuf_surface, win->x + (int32_t)frame_w - IRIS_WIN_BORDER_PX, win->y,
                         IRIS_WIN_BORDER_PX, frame_h, border_color);

        /* 2. 30px Gradient Light Title Bar (Figma topbarlightbg: #FFFFFF 100% -> 75%) */
        int32_t tb_x = win->x + IRIS_WIN_BORDER_PX;
        int32_t tb_y = win->y + IRIS_WIN_BORDER_PX;
        bool tb_intersects = (g_active_clip.w == 0 || g_active_clip.h == 0) ||
                             rects_intersect(tb_x, tb_y, (int32_t)win->client_w, IRIS_WIN_TITLEBAR_H,
                                             g_active_clip.x, g_active_clip.y,
                                             (int32_t)g_active_clip.w, (int32_t)g_active_clip.h);
        if (tb_intersects) {
            fill_rect_vgradient_over(g_backbuf_pixels, g_fb_width, g_fb_height,
                                     tb_x, tb_y, win->client_w, IRIS_WIN_TITLEBAR_H,
                                     tb_top_color, tb_bot_color);
            fill_rect_vgradient_over(g_backbuf_pixels, g_fb_width, g_fb_height,
                                     tb_x, tb_y + IRIS_WIN_TITLEBAR_H - 1, win->client_w, 1u,
                                     0x22141418u, 0x22141418u);

            /* Left-aligned window title string in Inter Display */
            draw_text_ft_ex(g_backbuf_pixels, g_fb_width, g_fb_height,
                            tb_x + 12, tb_y + 8, 13,
                            FONT_FAMILY_INTER_DISPLAY,
                            is_focused ? FONT_WEIGHT_MEDIUM : FONT_WEIGHT_REGULAR,
                            false, win->title, title_fg_color);

            /* Right-aligned modern Material caption buttons (26x22 hit targets) */
            if (win->client_w >= 110u) {
                int32_t btn_y     = tb_y + 4;
                int32_t btn_min_x = tb_x + (int32_t)win->client_w - 88;
                int32_t btn_max_x = tb_x + (int32_t)win->client_w - 58;
                int32_t btn_cls_x = tb_x + (int32_t)win->client_w - 28;

                bool can_hover = is_focused && !taskbar_has_active_flyout();
                bool hover_min = can_hover && (g_cursor_x >= btn_min_x && g_cursor_x < btn_min_x + 26 &&
                                               g_cursor_y >= btn_y && g_cursor_y < btn_y + 22);
                bool hover_max = can_hover && (g_cursor_x >= btn_max_x && g_cursor_x < btn_max_x + 26 &&
                                               g_cursor_y >= btn_y && g_cursor_y < btn_y + 22);
                bool hover_cls = can_hover && (g_cursor_x >= btn_cls_x && g_cursor_x < btn_cls_x + 26 &&
                                               g_cursor_y >= btn_y && g_cursor_y < btn_y + 22);

                uint32_t glyph_fg = is_focused ? 0xFF374151u : 0xFF6B7280u;

                /* Minimize button */
                fill_rounded_rect_over(g_backbuf_pixels, g_fb_width, g_fb_height,
                                       btn_min_x, btn_y, 26, 22, 6,
                                       hover_min ? 0x22141418u : 0x0A141418u);
                if (!draw_material_icon(g_backbuf_pixels, g_fb_width, g_fb_height,
                                        btn_min_x + 5, btn_y + 3, 15, MAT_ICON_REMOVE, glyph_fg)) {
                    fill_rect_pixman(g_backbuf_surface, btn_min_x + 7, btn_y + 13, 12, 2, glyph_fg);
                }

                /* Maximize / Restore button */
                fill_rounded_rect_over(g_backbuf_pixels, g_fb_width, g_fb_height,
                                       btn_max_x, btn_y, 26, 22, 6,
                                       hover_max ? 0x22141418u : 0x0A141418u);
                uint32_t max_icon = win->is_maximized ? MAT_ICON_FILTER_NONE : MAT_ICON_CROP_SQUARE;
                if (!draw_material_icon(g_backbuf_pixels, g_fb_width, g_fb_height,
                                        btn_max_x + 6, btn_y + 4, 14, max_icon, glyph_fg)) {
                    fill_rect_pixman(g_backbuf_surface, btn_max_x + 8, btn_y + 5, 10, 2, glyph_fg);
                    fill_rect_pixman(g_backbuf_surface, btn_max_x + 8, btn_y + 15, 10, 2, glyph_fg);
                    fill_rect_pixman(g_backbuf_surface, btn_max_x + 8, btn_y + 5, 2, 12, glyph_fg);
                    fill_rect_pixman(g_backbuf_surface, btn_max_x + 16, btn_y + 5, 2, 12, glyph_fg);
                }

                /* Close button */
                fill_rounded_rect_over(g_backbuf_pixels, g_fb_width, g_fb_height,
                                       btn_cls_x, btn_y, 26, 22, 6,
                                       hover_cls ? 0xFFEF4444u : 0x0A141418u);
                uint32_t cls_fg = hover_cls ? 0xFFFFFFFFu : glyph_fg;
                if (!draw_material_icon(g_backbuf_pixels, g_fb_width, g_fb_height,
                                        btn_cls_x + 5, btn_y + 3, 15, MAT_ICON_CLOSE, cls_fg)) {
                    draw_line_thick(g_backbuf_surface, btn_cls_x + 8, btn_y + 6,
                                    btn_cls_x + 17, btn_y + 15, cls_fg);
                    draw_line_thick(g_backbuf_surface, btn_cls_x + 17, btn_y + 6,
                                    btn_cls_x + 8, btn_y + 15, cls_fg);
                }
            }
        }
    }

    /* 3. Blit client VMO surface onto backbuffer (PIXMAN_OP_OVER if translucent, PIXMAN_OP_SRC if opaque) */
    pixman_op_t client_op = (win->flags & IRIS_WIN_FLAG_TRANSLUCENT) ? PIXMAN_OP_OVER : PIXMAN_OP_SRC;
    blit_surface_clamped(win->client_surface, (int32_t)win->client_w, (int32_t)win->client_h,
                         g_backbuf_surface, (int32_t)g_fb_width, (int32_t)g_fb_height,
                         client_dst_x, client_dst_y, client_op);
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

    int32_t ox = g_last_cursor_x - CURSOR_HOTSPOT_X;
    int32_t oy = g_last_cursor_y - CURSOR_HOTSPOT_Y;
    int32_t ow = CURSOR_SPRITE_W;
    int32_t oh = CURSOR_SPRITE_H;
    bool has_old_cursor = false;

    /* 1. Copy old_cursor_rect from g_backbuf_surface to g_scanout_surface (PIXMAN_OP_SRC) */
    if (g_cursor_visible) {
        if (clamp_rect_to_screen(&ox, &oy, &ow, &oh, sw, sh)) {
            pixman_image_composite32(PIXMAN_OP_SRC,
                                     g_backbuf_surface,
                                     NULL,
                                     g_scanout_surface,
                                     ox, oy,
                                     0, 0,
                                     ox, oy,
                                     ow, oh);
            has_old_cursor = true;
            if (dmg_w > 0 && dmg_h > 0) {
                expand_flush_bounds(ox, oy, ow, oh, &flush_min_x, &flush_min_y, &flush_max_x, &flush_max_y, &has_flush);
            }
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
     *    and then composite g_cursor_surface onto g_scanout_surface using PIXMAN_OP_OVER */
    int32_t nx = g_cursor_x - CURSOR_HOTSPOT_X;
    int32_t ny = g_cursor_y - CURSOR_HOTSPOT_Y;
    int32_t nw = CURSOR_SPRITE_W;
    int32_t nh = CURSOR_SPRITE_H;
    bool has_new_cursor = false;

    if (g_cursor_surface != NULL) {
        if (clamp_rect_to_screen(&nx, &ny, &nw, &nh, sw, sh)) {
            pixman_image_composite32(PIXMAN_OP_SRC,
                                     g_backbuf_surface,
                                     NULL,
                                     g_scanout_surface,
                                     nx, ny,
                                     0, 0,
                                     nx, ny,
                                     nw, nh);
            has_new_cursor = true;
            if (dmg_w > 0 && dmg_h > 0) {
                expand_flush_bounds(nx, ny, nw, nh, &flush_min_x, &flush_min_y, &flush_max_x, &flush_max_y, &has_flush);
            }
        }

        blit_surface_clamped(g_cursor_surface, CURSOR_SPRITE_W, CURSOR_SPRITE_H,
                             g_scanout_surface, sw, sh,
                             g_cursor_x - CURSOR_HOTSPOT_X,
                             g_cursor_y - CURSOR_HOTSPOT_Y,
                             PIXMAN_OP_OVER);

        g_cursor_visible = true;
        g_last_cursor_x  = g_cursor_x;
        g_last_cursor_y  = g_cursor_y;
    }

    /* 4. Flush dirty region(s) to VirtIO GPU scanout */
    if (dmg_w == 0 && dmg_h == 0) {
        if (has_old_cursor && has_new_cursor &&
            !rects_intersect(ox, oy, ow, oh, nx, ny, nw, nh)) {
            flush_gpu_scanout((uint32_t)ox, (uint32_t)oy, (uint32_t)ow, (uint32_t)oh);
            flush_gpu_scanout((uint32_t)nx, (uint32_t)ny, (uint32_t)nw, (uint32_t)nh);
            return;
        }
        if (has_old_cursor) {
            expand_flush_bounds(ox, oy, ow, oh, &flush_min_x, &flush_min_y, &flush_max_x, &flush_max_y, &has_flush);
        }
        if (has_new_cursor) {
            expand_flush_bounds(nx, ny, nw, nh, &flush_min_x, &flush_min_y, &flush_max_x, &flush_max_y, &has_flush);
        }
    }

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

    g_active_clip.x = dmg_x;
    g_active_clip.y = dmg_y;
    g_active_clip.w = dmg_w;
    g_active_clip.h = dmg_h;

    /* Front-to-back occlusion check: find highest Z-order OPAQUE window that completely covers dmg_rect */
    size_t start_win_idx = 0;
    bool   skip_wallpaper = false;

    if (g_num_windows == 0 && g_welcome_surface != NULL &&
        !g_welcome_win.is_minimized && !(g_welcome_win.flags & IRIS_WIN_FLAG_TRANSLUCENT)) {
        if (rect_contains(g_welcome_win.x + IRIS_WIN_BORDER_PX,
                          g_welcome_win.y + IRIS_WIN_BORDER_PX + IRIS_WIN_TITLEBAR_H,
                          (int32_t)g_welcome_win.client_w, (int32_t)g_welcome_win.client_h,
                          dmg_x, dmg_y, (int32_t)dmg_w, (int32_t)dmg_h)) {
            skip_wallpaper = true;
        }
    } else if (g_num_windows > 0) {
        for (size_t i = g_num_windows; i > 0; --i) {
            const comp_window_t* w = &g_windows[i - 1];
            if (w->is_minimized || (w->flags & IRIS_WIN_FLAG_TRANSLUCENT)) {
                continue;
            }
            if (rect_contains(w->x + IRIS_WIN_BORDER_PX,
                              w->y + IRIS_WIN_BORDER_PX + IRIS_WIN_TITLEBAR_H,
                              (int32_t)w->client_w, (int32_t)w->client_h,
                              dmg_x, dmg_y, (int32_t)dmg_w, (int32_t)dmg_h)) {
                start_win_idx  = i - 1;
                skip_wallpaper = true;
                break;
            }
        }
    }

    /* 1. Blit desktop wallpaper for the damaged region into backbuffer (unless occluded) */
    if (!skip_wallpaper) {
        pixman_image_composite32(PIXMAN_OP_SRC,
                                 g_wallpaper_surface,
                                 NULL,
                                 g_backbuf_surface,
                                 dmg_x, dmg_y,
                                 0, 0,
                                 dmg_x, dmg_y,
                                 (int)dmg_w, (int)dmg_h);
    }

    /* 2. Composite visible windows in Z-order starting from first non-occluded layer */
    if (g_num_windows == 0 && g_welcome_surface != NULL) {
        composite_window(&g_welcome_win, !g_crash_dialog_active);
    } else {
        for (size_t i = start_win_idx; i < g_num_windows; ++i) {
            bool is_top = (i + 1 == g_num_windows) && !g_crash_dialog_active;
            composite_window(&g_windows[i], is_top);
        }
    }

    /* 2b. Render dynamic 46px taskbar layered above standard desktop windows */
    int32_t tb_top = (int32_t)(g_fb_height - IRIS_TASKBAR_H);
    if (dmg_y + (int32_t)dmg_h > tb_top) {
        taskbar_render(g_backbuf_pixels, g_backbuf_surface);
    }

    /* 2c. Render active taskbar flyout, drag ghost, or sleep overlay */
    if (taskbar_has_active_overlay()) {
        taskbar_render_flyout(g_backbuf_pixels, g_backbuf_surface);
    }

    /* 3. Composite modal crash dialog or unlock prompt on top if active */
    if (g_crash_dialog_active && g_crash_surface != NULL) {
        composite_window(&g_crash_win, true);
    }
    if (g_unlock_active && g_unlock_surface != NULL) {
        composite_window(&g_unlock_win, true);
    }

    g_active_clip.w = 0;
    g_active_clip.h = 0;

    /* Clear clip region on backbuffer */
    pixman_image_set_clip_region32(g_backbuf_surface, NULL);
    pixman_region32_fini(&clip);

    /* 4. Present damaged region with software cursor overlay and flush dirty rect */
    update_screen(dmg_x, dmg_y, dmg_w, dmg_h);
}
