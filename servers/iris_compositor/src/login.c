/**
 * @file login.c
 * @brief Graphical Login Window, Per-User Vault Unlock & Session Switcher.
 */

#include "login.h"
#include "compositor.h"
#include "taskbar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iris/ipc.h>
#include <zircon/syscalls.h>

#define LOGIN_CARD_W        580u
#define LOGIN_CARD_H        390u
#define AVATAR_SIZE         68
#define AVATAR_SPACING      120

static zx_handle_t             g_login_fs_ch  = ZX_HANDLE_INVALID;
static zx_handle_t             g_login_sys_ch = ZX_HANDLE_INVALID;
static bool                    g_login_active = false;

static iris_fs_user_list_resp_t g_user_list;
static uint32_t*               g_avatar_bufs[IRIS_MAX_USER_PROFILES];
static int                     g_selected_idx = -1;
static char                    g_pw[128];
static size_t                  g_pw_len = 0;
static char                    g_status_msg[96] = "";
static uint32_t                g_status_col = 0xFF374151u;

static char                    g_active_user[32]         = "admin";
static char                    g_active_display_name[64] = "Administrator";
static bool                    g_active_is_guest         = false;

static void free_avatar_cache(void) {
    for (uint32_t i = 0; i < IRIS_MAX_USER_PROFILES; i++) {
        if (g_avatar_bufs[i]) {
            free(g_avatar_bufs[i]);
            g_avatar_bufs[i] = NULL;
        }
    }
}

static void load_user_avatars(void) {
    free_avatar_cache();
    for (uint32_t i = 0; i < g_user_list.count && i < IRIS_MAX_USER_PROFILES; i++) {
        const iris_user_profile_info_t* u = &g_user_list.users[i];
        if (u->is_guest || u->avatar_path[0] == '\0') continue;

        uint32_t* buf = (uint32_t*)calloc(AVATAR_SIZE * AVATAR_SIZE, sizeof(uint32_t));
        if (!buf) continue;
        pixman_image_t* img = pixman_image_create_bits(
            PIXMAN_x8r8g8b8, AVATAR_SIZE, AVATAR_SIZE, buf, AVATAR_SIZE * 4);
        if (img) {
            if (load_qoi_wallpaper_aspect_fill(u->avatar_path, img, AVATAR_SIZE, AVATAR_SIZE)) {
                g_avatar_bufs[i] = buf;
            } else {
                free(buf);
            }
            pixman_image_unref(img);
        } else {
            free(buf);
        }
    }
}

static void fetch_user_list(void) {
    memset(&g_user_list, 0, sizeof(g_user_list));
    if (g_login_fs_ch != ZX_HANDLE_INVALID) {
        static uint32_t txid = 3100;
        iris_msg_hdr_t req;
        iris_msg_hdr_init(&req, IRIS_PROTO_FS, IRIS_FS_OP_LIST_USERS, txid++, 0);
        if (zx_channel_write(g_login_fs_ch, 0, &req, sizeof(req), NULL, 0) == ZX_OK) {
            zx_signals_t obs = 0;
            if (zx_object_wait_one(g_login_fs_ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                   zx_clock_get_monotonic() + 3000000000ULL, &obs) == ZX_OK &&
                (obs & ZX_CHANNEL_READABLE)) {
                uint8_t rep[sizeof(iris_msg_hdr_t) + sizeof(iris_fs_user_list_resp_t)] __attribute__((aligned(8)));
                uint32_t ab = 0, ah = 0;
                if (zx_channel_read(g_login_fs_ch, 0, rep, NULL, sizeof(rep), 0, &ab, &ah) == ZX_OK &&
                    ab >= sizeof(rep)) {
                    const iris_msg_hdr_t* rh = (const iris_msg_hdr_t*)rep;
                    if (rh->status == ZX_OK) {
                        memcpy(&g_user_list, rep + sizeof(iris_msg_hdr_t), sizeof(g_user_list));
                    }
                }
            }
        }
    }

    if (g_user_list.count == 0) {
        g_user_list.count = 2;
        snprintf(g_user_list.users[0].username, sizeof(g_user_list.users[0].username), "admin");
        snprintf(g_user_list.users[0].display_name, sizeof(g_user_list.users[0].display_name), "Administrator");
        snprintf(g_user_list.users[0].avatar_path, sizeof(g_user_list.users[0].avatar_path),
                 "/System/Resources/avatar_admin.qoi");
        snprintf(g_user_list.users[0].wallpaper_path, sizeof(g_user_list.users[0].wallpaper_path),
                 "/System/Resources/wallpaper_admin.qoi");
        snprintf(g_user_list.users[0].vault_path, sizeof(g_user_list.users[0].vault_path),
                 "/Vaults/admin.img");
        g_user_list.users[0].is_guest = 0;

        snprintf(g_user_list.users[1].username, sizeof(g_user_list.users[1].username), "guest");
        snprintf(g_user_list.users[1].display_name, sizeof(g_user_list.users[1].display_name), "Guest");
        snprintf(g_user_list.users[1].wallpaper_path, sizeof(g_user_list.users[1].wallpaper_path),
                 "/System/Resources/wallpaper_guest.qoi");
        g_user_list.users[1].is_guest = 1;
    }

    load_user_avatars();
}

static zx_status_t send_sys_op(uint32_t opcode, const void* payload, uint32_t plen) {
    if (g_login_sys_ch == ZX_HANDLE_INVALID) return ZX_ERR_UNAVAILABLE;
    static uint32_t txid = 3200;
    uint8_t buf[sizeof(iris_msg_hdr_t) + 128] __attribute__((aligned(8)));
    if (plen > 128) return ZX_ERR_INVALID_ARGS;
    iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)buf;
    iris_msg_hdr_init(hdr, IRIS_PROTO_SYSTEM, opcode, txid++, plen);
    if (plen > 0 && payload) memcpy(buf + sizeof(iris_msg_hdr_t), payload, plen);
    zx_status_t st = zx_channel_write(g_login_sys_ch, 0, buf, (uint32_t)(sizeof(iris_msg_hdr_t) + plen), NULL, 0);
    if (st != ZX_OK) return st;

    zx_signals_t obs = 0;
    st = zx_object_wait_one(g_login_sys_ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                            zx_clock_get_monotonic() + 3000000000ULL, &obs);
    if (st != ZX_OK || !(obs & ZX_CHANNEL_READABLE)) return ZX_ERR_PEER_CLOSED;
    iris_msg_hdr_t rep;
    uint32_t ab = 0, ah = 0;
    st = zx_channel_read(g_login_sys_ch, 0, &rep, NULL, sizeof(rep), 0, &ab, &ah);
    return (st == ZX_OK) ? rep.status : st;
}

static zx_status_t attempt_user_login(int idx, const char* passphrase, size_t plen) {
    if (idx < 0 || (uint32_t)idx >= g_user_list.count || g_login_fs_ch == ZX_HANDLE_INVALID) {
        return ZX_ERR_INVALID_ARGS;
    }
    const iris_user_profile_info_t* u = &g_user_list.users[idx];

    static uint32_t txid = 3300;
    uint8_t buf[sizeof(iris_msg_hdr_t) + sizeof(iris_fs_user_login_req_t)] __attribute__((aligned(8)));
    iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)buf;
    iris_fs_user_login_req_t* req = (iris_fs_user_login_req_t*)(buf + sizeof(iris_msg_hdr_t));
    iris_msg_hdr_init(hdr, IRIS_PROTO_FS, IRIS_FS_OP_USER_LOGIN, txid++, sizeof(iris_fs_user_login_req_t));
    memset(req, 0, sizeof(*req));
    req->is_guest = u->is_guest;
    snprintf(req->username, sizeof(req->username), "%s", u->username);
    if (!u->is_guest && passphrase && plen > 0) {
        if (plen >= sizeof(req->passphrase)) plen = sizeof(req->passphrase) - 1;
        req->passphrase_len = (uint32_t)plen;
        memcpy(req->passphrase, passphrase, plen);
    }

    zx_status_t st = zx_channel_write(g_login_fs_ch, 0, buf, sizeof(buf), NULL, 0);
    memset(buf, 0, sizeof(buf));
    if (st != ZX_OK) return st;

    zx_signals_t obs = 0;
    st = zx_object_wait_one(g_login_fs_ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                            zx_clock_get_monotonic() + 10000000000ULL, &obs);
    if (st != ZX_OK || !(obs & ZX_CHANNEL_READABLE)) return ZX_ERR_PEER_CLOSED;

    iris_msg_hdr_t rep;
    uint32_t ab = 0, ah = 0;
    st = zx_channel_read(g_login_fs_ch, 0, &rep, NULL, sizeof(rep), 0, &ab, &ah);
    if (st != ZX_OK) return st;
    if (rep.status != ZX_OK) return rep.status;

    snprintf(g_active_user, sizeof(g_active_user), "%s", u->username);
    snprintf(g_active_display_name, sizeof(g_active_display_name), "%s", u->display_name);
    g_active_is_guest = (u->is_guest != 0);

    /* Snapshot the complete login screen before transitioning to the desktop */
    compositor_capture_boot_splash_frame();

    iris_sys_session_req_t sreq;
    memset(&sreq, 0, sizeof(sreq));
    sreq.is_guest = u->is_guest;
    snprintf(sreq.username, sizeof(sreq.username), "%s", u->username);
    send_sys_op(IRIS_SYS_OP_SESSION_LOGIN, &sreq, sizeof(sreq));

    /* Load user's personal or profile wallpaper if present */
    char user_wp[160];
    snprintf(user_wp, sizeof(user_wp), "/Users/%s/wallpaper.qoi", u->username);
    FILE* f = fopen(user_wp, "rb");
    if (f) {
        fclose(f);
        if (!load_qoi_wallpaper_aspect_fill(user_wp, g_wallpaper_surface, g_fb_width, g_fb_height)) {
            load_qoi_wallpaper_aspect_fill(WALLPAPER_QOI_PATH, g_wallpaper_surface, g_fb_width, g_fb_height);
        }
    } else if (u->wallpaper_path[0] != '\0') {
        if (!load_qoi_wallpaper_aspect_fill(u->wallpaper_path, g_wallpaper_surface, g_fb_width, g_fb_height)) {
            load_qoi_wallpaper_aspect_fill(WALLPAPER_QOI_PATH, g_wallpaper_surface, g_fb_width, g_fb_height);
        }
    }

    g_login_active = false;
    g_selected_idx = -1;
    memset(g_pw, 0, sizeof(g_pw));
    g_pw_len = 0;
    compositor_begin_boot_to_login_crossfade();
    return ZX_OK;
}

void login_init(zx_handle_t fs_ch, zx_handle_t sys_ctrl_ch) {
    g_login_fs_ch  = fs_ch;
    g_login_sys_ch = sys_ctrl_ch;
}

void login_set_fs_channel(zx_handle_t fs_ch) {
    g_login_fs_ch = fs_ch;
}

void login_show(void) {
    fetch_user_list();
    g_selected_idx = -1;
    memset(g_pw, 0, sizeof(g_pw));
    g_pw_len = 0;
    g_status_msg[0] = '\0';
    g_login_active = true;
    dirty_rect_union(0, 0, g_fb_width, g_fb_height);
}

bool login_is_active(void) {
    return g_login_active;
}

const char* login_active_username(void) {
    return g_active_user;
}

const char* login_active_display_name(void) {
    return g_active_display_name;
}

bool login_active_is_guest(void) {
    return g_active_is_guest;
}

void login_trigger_logout(void) {
    compositor_capture_boot_splash_frame();
    send_sys_op(IRIS_SYS_OP_SESSION_LOGOUT, NULL, 0);
    load_qoi_wallpaper_aspect_fill(WALLPAPER_QOI_PATH, g_wallpaper_surface, g_fb_width, g_fb_height);
    login_show();
    compositor_begin_boot_to_login_crossfade();
}

static void draw_circular_avatar(uint32_t* dst, uint32_t fb_w, uint32_t fb_h,
                                 int32_t cx, int32_t cy, int32_t radius,
                                 const uint32_t* src_rgb, bool selected) {
    int32_t r2 = radius * radius;
    int32_t border_r = radius + 2;
    int32_t br2 = border_r * border_r;
    uint32_t ring_col = selected ? 0xFF2563EBu : 0xFF374151u;

    for (int32_t dy = -border_r; dy <= border_r; dy++) {
        int32_t sy = cy + dy;
        if (sy < 0 || (uint32_t)sy >= fb_h) continue;
        for (int32_t dx = -border_r; dx <= border_r; dx++) {
            int32_t sx = cx + dx;
            if (sx < 0 || (uint32_t)sx >= fb_w) continue;
            int32_t d2 = dx * dx + dy * dy;
            if (d2 <= r2) {
                int32_t tx = dx + radius;
                int32_t ty = dy + radius;
                if (tx >= 0 && tx < AVATAR_SIZE && ty >= 0 && ty < AVATAR_SIZE) {
                    dst[(size_t)sy * fb_w + (size_t)sx] = 0xFF000000u | src_rgb[ty * AVATAR_SIZE + tx];
                }
            } else if (d2 <= br2) {
                dst[(size_t)sy * fb_w + (size_t)sx] = ring_col;
            }
        }
    }
}

void login_render(uint32_t* dst_pixels, pixman_image_t* dst_surface,
                  uint32_t fb_w, uint32_t fb_h) {
    (void)dst_surface;
    if (!g_login_active || !dst_pixels) return;

    int32_t card_x = ((int32_t)fb_w - (int32_t)LOGIN_CARD_W) / 2;
    int32_t card_y = ((int32_t)fb_h - (int32_t)LOGIN_CARD_H) / 2;

    /* 4-stop vertical frosted gradient matching Figma 'loginwindow' (80% -> 100% -> 100% -> 80% white) */
    uint32_t q1 = LOGIN_CARD_H / 4u;
    uint32_t q3 = (LOGIN_CARD_H * 3u) / 4u;
    fill_rect_vgradient_over(dst_pixels, fb_w, fb_h, card_x, card_y,
                             LOGIN_CARD_W, q1, 0xCCFFFFFFu, 0xFFFFFFFFu);
    fill_rect_vgradient_over(dst_pixels, fb_w, fb_h, card_x, card_y + (int32_t)q1,
                             LOGIN_CARD_W, q3 - q1, 0xFFFFFFFFu, 0xFFFFFFFFu);
    fill_rect_vgradient_over(dst_pixels, fb_w, fb_h, card_x, card_y + (int32_t)q3,
                             LOGIN_CARD_W, LOGIN_CARD_H - q3, 0xFFFFFFFFu, 0xCCFFFFFFu);

    /* "Welcome" in Instrument Serif 48px */
    const char* title = "Welcome";
    int32_t tw = measure_text_ft_ex(FONT_FAMILY_INSTRUMENT_SERIF, FONT_WEIGHT_REGULAR, false, 48, title);
    draw_text_ft_ex(dst_pixels, fb_w, fb_h, card_x + ((int32_t)LOGIN_CARD_W - tw) / 2, card_y + 24,
                    48, FONT_FAMILY_INSTRUMENT_SERIF, FONT_WEIGHT_REGULAR, false, title, 0xFF111111u);

    const char* sub = "Click on your user name to begin";
    int32_t sw = measure_text_ft_ex(FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, 13, sub);
    draw_text_ft_ex(dst_pixels, fb_w, fb_h, card_x + ((int32_t)LOGIN_CARD_W - sw) / 2, card_y + 82,
                    13, FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, sub, 0xFF374151u);

    /* User avatars row */
    uint32_t cnt = g_user_list.count;
    if (cnt > 0) {
        int32_t total_w = (int32_t)((cnt - 1u) * AVATAR_SPACING);
        int32_t start_cx = card_x + (int32_t)(LOGIN_CARD_W / 2u) - total_w / 2;
        int32_t cy = card_y + 165;

        for (uint32_t i = 0; i < cnt; i++) {
            int32_t cx = start_cx + (int32_t)(i * AVATAR_SPACING);
            bool sel = ((int)i == g_selected_idx);

            if (g_user_list.users[i].is_guest) {
                fill_circle_over(dst_pixels, fb_w, fb_h, cx, cy, (AVATAR_SIZE / 2) + 2, 0xFF374151u);
                fill_circle_over(dst_pixels, fb_w, fb_h, cx, cy, AVATAR_SIZE / 2, 0xFFE5E7EBu);
                draw_material_icon(dst_pixels, fb_w, fb_h, cx - 15, cy - 15, 30, 0xE8F9u, 0xFF111111u);
            } else if (g_avatar_bufs[i]) {
                draw_circular_avatar(dst_pixels, fb_w, fb_h, cx, cy, AVATAR_SIZE / 2, g_avatar_bufs[i], sel);
            } else {
                fill_circle_over(dst_pixels, fb_w, fb_h, cx, cy, (AVATAR_SIZE / 2) + 2,
                                 sel ? 0xFF2563EBu : 0xFF374151u);
                fill_circle_over(dst_pixels, fb_w, fb_h, cx, cy, AVATAR_SIZE / 2, 0xFFD1D5DBu);
                draw_material_icon(dst_pixels, fb_w, fb_h, cx - 15, cy - 15, 30, 0xE7FDu, 0xFF111111u);
            }

            const char* dname = g_user_list.users[i].display_name;
            int32_t nw = measure_text_ft_ex(FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false, 14, dname);
            draw_text_ft_ex(dst_pixels, fb_w, fb_h, cx - nw / 2, cy + (AVATAR_SIZE / 2) + 10,
                            14, FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false, dname, 0xFF111111u);
        }
    }

    /* Inline passphrase prompt when a persistent user is selected */
    if (g_selected_idx >= 0 && (uint32_t)g_selected_idx < g_user_list.count &&
        !g_user_list.users[g_selected_idx].is_guest) {
        int32_t box_w = 260;
        int32_t box_h = 32;
        int32_t box_x = card_x + ((int32_t)LOGIN_CARD_W - box_w) / 2;
        int32_t box_y = card_y + 236;

        fill_rounded_rect_over(dst_pixels, fb_w, fb_h, box_x - 1, box_y - 1,
                               (uint32_t)box_w + 2, (uint32_t)box_h + 2, 6, 0xFF2563EBu);
        fill_rounded_rect_over(dst_pixels, fb_w, fb_h, box_x, box_y,
                               (uint32_t)box_w, (uint32_t)box_h, 5, 0xFFFFFFFFu);

        char masked[40];
        size_t vis = (g_pw_len < 36) ? g_pw_len : 36;
        memset(masked, '*', vis);
        masked[vis] = '\0';
        if (vis > 0) {
            draw_text_ft_ex(dst_pixels, fb_w, fb_h, box_x + 12, box_y + 8, 15,
                            FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, masked, 0xFF111111u);
        } else {
            draw_text_ft_ex(dst_pixels, fb_w, fb_h, box_x + 12, box_y + 9, 13,
                            FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false,
                            "Enter passphrase...", 0xFF9CA3AFu);
        }

        const char* st_txt = (g_status_msg[0] != '\0') ? g_status_msg : "Press Enter to unlock (dev: irisos)";
        int32_t st_w = measure_text_ft_ex(FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, 12, st_txt);
        draw_text_ft_ex(dst_pixels, fb_w, fb_h, card_x + ((int32_t)LOGIN_CARD_W - st_w) / 2, box_y + 38,
                        12, FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, st_txt, g_status_col);
    }

    /* Bottom Action Buttons: Sleep (0xEF44), Restart (0xF053), Shutdown (0xE8AC) */
    static const struct {
        uint32_t    cp;
        const char* label;
    } k_actions[3] = {
        { 0xEF44u, "Sleep" },
        { 0xF053u, "Restart" },
        { 0xE8ACu, "Shutdown" }
    };
    int32_t act_y = card_y + (int32_t)LOGIN_CARD_H - 64;
    int32_t act_start_cx = card_x + (int32_t)(LOGIN_CARD_W / 2u) - 88;
    for (int a = 0; a < 3; a++) {
        int32_t ax = act_start_cx + a * 88;
        draw_material_icon(dst_pixels, fb_w, fb_h, ax - 12, act_y, 24, k_actions[a].cp, 0xFF111111u);
        int32_t lw = measure_text_ft_ex(FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, 12, k_actions[a].label);
        draw_text_ft_ex(dst_pixels, fb_w, fb_h, ax - lw / 2, act_y + 28,
                        12, FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, k_actions[a].label, 0xFF111111u);
    }
}

bool login_handle_click(int32_t mx, int32_t my, uint32_t fb_w, uint32_t fb_h) {
    if (taskbar_wake_from_sleep()) return true;
    if (!g_login_active) return false;

    int32_t card_x = ((int32_t)fb_w - (int32_t)LOGIN_CARD_W) / 2;
    int32_t card_y = ((int32_t)fb_h - (int32_t)LOGIN_CARD_H) / 2;

    /* User avatars hit-test */
    uint32_t cnt = g_user_list.count;
    if (cnt > 0) {
        int32_t total_w = (int32_t)((cnt - 1u) * AVATAR_SPACING);
        int32_t start_cx = card_x + (int32_t)(LOGIN_CARD_W / 2u) - total_w / 2;
        int32_t cy = card_y + 165;
        for (uint32_t i = 0; i < cnt; i++) {
            int32_t cx = start_cx + (int32_t)(i * AVATAR_SPACING);
            int32_t dx = mx - cx;
            int32_t dy = my - cy;
            if (dx * dx + dy * dy <= (AVATAR_SIZE / 2 + 10) * (AVATAR_SIZE / 2 + 10) ||
                (mx >= cx - 44 && mx <= cx + 44 && my >= cy + 32 && my <= cy + 60)) {
                if (g_user_list.users[i].is_guest) {
                    attempt_user_login((int)i, NULL, 0);
                } else {
                    g_selected_idx = (int)i;
                    memset(g_pw, 0, sizeof(g_pw));
                    g_pw_len = 0;
                    g_status_msg[0] = '\0';
                    g_status_col = 0xFF374151u;
                    dirty_rect_union(card_x, card_y, LOGIN_CARD_W, LOGIN_CARD_H);
                }
                return true;
            }
        }
    }

    /* Bottom action buttons */
    int32_t act_y = card_y + (int32_t)LOGIN_CARD_H - 64;
    int32_t act_start_cx = card_x + (int32_t)(LOGIN_CARD_W / 2u) - 88;
    for (int a = 0; a < 3; a++) {
        int32_t ax = act_start_cx + a * 88;
        if (mx >= ax - 36 && mx <= ax + 36 && my >= act_y - 4 && my <= act_y + 46) {
            if (a == 0) {
                taskbar_enter_sleep();
            } else if (a == 1) {
                send_sys_op(IRIS_SYS_OP_REBOOT, NULL, 0);
            } else if (a == 2) {
                send_sys_op(IRIS_SYS_OP_SHUTDOWN, NULL, 0);
            }
            return true;
        }
    }

    return true;
}

void login_handle_key(char ch) {
    if (!g_login_active || g_selected_idx < 0) return;

    int32_t card_x = ((int32_t)g_fb_width - (int32_t)LOGIN_CARD_W) / 2;
    int32_t card_y = ((int32_t)g_fb_height - (int32_t)LOGIN_CARD_H) / 2;

    if (ch == 0x1B) { /* Escape */
        g_selected_idx = -1;
        memset(g_pw, 0, sizeof(g_pw));
        g_pw_len = 0;
        g_status_msg[0] = '\0';
        dirty_rect_union(card_x, card_y, LOGIN_CARD_W, LOGIN_CARD_H);
        return;
    }

    if (ch == '\b' || ch == 0x7F) {
        if (g_pw_len > 0) g_pw[--g_pw_len] = '\0';
        dirty_rect_union(card_x, card_y, LOGIN_CARD_W, LOGIN_CARD_H);
        return;
    }

    if (ch == '\r' || ch == '\n') {
        snprintf(g_status_msg, sizeof(g_status_msg), "Deriving key (Argon2id)...");
        g_status_col = 0xFF2563EBu;
        composite_and_flush(card_x, card_y, LOGIN_CARD_W, LOGIN_CARD_H);

        zx_status_t st = attempt_user_login(g_selected_idx, g_pw, g_pw_len);
        memset(g_pw, 0, sizeof(g_pw));
        g_pw_len = 0;
        if (st != ZX_OK) {
            if (st == ZX_ERR_NOT_FOUND || st == ZX_ERR_UNAVAILABLE) {
                snprintf(g_status_msg, sizeof(g_status_msg), "Vault missing - run ./iris reset-data");
            } else if (st == ZX_ERR_ACCESS_DENIED) {
                snprintf(g_status_msg, sizeof(g_status_msg), "Invalid passphrase. Please try again.");
            } else {
                snprintf(g_status_msg, sizeof(g_status_msg), "Unlock error (%d)", (int)st);
            }
            g_status_col = 0xFFDC2626u;
            dirty_rect_union(card_x, card_y, LOGIN_CARD_W, LOGIN_CARD_H);
        }
        return;
    }

    if (ch >= 32 && ch < 127 && g_pw_len + 1 < sizeof(g_pw)) {
        g_pw[g_pw_len++] = ch;
        g_pw[g_pw_len] = '\0';
        dirty_rect_union(card_x, card_y, LOGIN_CARD_W, LOGIN_CARD_H);
    }
}
