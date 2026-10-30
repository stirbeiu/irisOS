/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_compositor/src/main.c
 * Purpose: Zero-Copy VMO Window Compositor Daemon (bin/iris_compositor).
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 system server.
 */

#include "compositor.h"

/* ==============================================================================
 * Global Compositor State Definitions
 * ============================================================================== */

zx_handle_t       g_server_channel    = ZX_HANDLE_INVALID;
zx_handle_t       g_gpu_channel       = ZX_HANDLE_INVALID;
zx_handle_t       g_root_resource     = ZX_HANDLE_INVALID;
uint32_t          g_gpu_txid          = 1u;

uint32_t          g_fb_width          = 1280u;
uint32_t          g_fb_height         = 800u;
uint32_t          g_fb_pitch          = 5120u;
uint64_t          g_fb_size           = 0;
zx_handle_t       g_fb_vmo            = ZX_HANDLE_INVALID;
zx_vaddr_t        g_fb_vaddr          = 0;

uint32_t*         g_backbuf_pixels    = NULL;
pixman_image_t*   g_backbuf_surface   = NULL;
pixman_image_t*   g_scanout_surface   = NULL;
pixman_image_t*   g_wallpaper_surface = NULL;

/* 18x24 RGBA Software Cursor State & Save-Under Tracking */
uint32_t*         g_cursor_pixels     = NULL;
pixman_image_t*   g_cursor_surface    = NULL;
int32_t           g_cursor_x          = 640;
int32_t           g_cursor_y          = 400;
int32_t           g_last_cursor_x     = 640;
int32_t           g_last_cursor_y     = 400;
bool              g_cursor_visible    = false;
bool              g_cursor_dirty      = false;
bool              g_btn_left_down     = false;
uint32_t          g_drag_window_id    = 0;
bool              g_drag_welcome      = false;
bool              g_drag_crash        = false;
int32_t           g_drag_off_x        = 0;
int32_t           g_drag_off_y        = 0;

/* Default welcome window shown when no client windows are open */
uint32_t*         g_welcome_pixels    = NULL;
pixman_image_t*   g_welcome_surface   = NULL;
comp_window_t     g_welcome_win;
char              g_welcome_typed[48] = {0};
size_t            g_welcome_typed_len = 0;

/* Modal Crash Recovery Dialog */
bool              g_crash_dialog_active = false;
comp_window_t     g_crash_win;
uint32_t*         g_crash_pixels        = NULL;
pixman_image_t*   g_crash_surface       = NULL;
char              g_crash_details[512]  = {0};

/* VirtIO Input & UART Serial State */
virtio_input_dev_t g_input_devs[MAX_INPUT_DEVS];
size_t            g_num_input_devs    = 0;
volatile bool     g_shutdown          = false;
volatile uint8_t  g_uart_lock         = 0;
char              g_uart_fifo[256];
uint32_t          g_uart_head         = 0;
uint32_t          g_uart_tail         = 0;
uint32_t          g_uart_count        = 0;

/* Window List State */
comp_window_t     g_windows[MAX_COMP_WINDOWS];
size_t            g_num_windows       = 0;
uint32_t          g_next_window_id    = 1u;

/* Dirty Damage Tracking */
comp_dirty_rect_t g_dirty_rect        = {0, 0, 0, 0};
bool              g_dirty_active      = false;

/* ==============================================================================
 * Window Table Management
 * ============================================================================== */

comp_window_t* find_window(uint32_t window_id, size_t* out_idx) {
    for (size_t i = 0; i < g_num_windows; ++i) {
        if (g_windows[i].window_id == window_id) {
            if (out_idx) *out_idx = i;
            return &g_windows[i];
        }
    }
    return NULL;
}

void release_window_resources(comp_window_t* win) {
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

void destroy_client_windows(int client_slot) {
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
        dirty_rect_union(0, 0, g_fb_width, g_fb_height);
    }
}

void window_push_key(comp_window_t* win, char ch) {
    if (!win) return;
    if (win->key_count < WIN_KEY_FIFO_MAX) {
        win->key_fifo[win->key_tail] = ch;
        win->key_tail = (win->key_tail + 1u) % WIN_KEY_FIFO_MAX;
        win->key_count++;
    }
}

char window_pop_key(comp_window_t* win) {
    if (!win || win->key_count == 0) return 0;
    char ch = win->key_fifo[win->key_head];
    win->key_head = (win->key_head + 1u) % WIN_KEY_FIFO_MAX;
    win->key_count--;
    return ch;
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

    dirty_rect_union(0, 0, g_fb_width, g_fb_height);
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
    dirty_rect_union_window(win);
    release_window_resources(win);
    for (size_t j = idx; j + 1 < g_num_windows; ++j) {
        g_windows[j] = g_windows[j + 1];
    }
    g_num_windows--;

    dirty_rect_union(0, 0, g_fb_width, g_fb_height);
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

    dirty_rect_union(win->x, win->y, frame_w, frame_h);
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

    dirty_rect_union_window(win);
    win->x = req->x;
    win->y = req->y;
    if (win->y < IRIS_TOPBAR_H) {
        win->y = IRIS_TOPBAR_H;
    }
    dirty_rect_union_window(win);

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
 * Main Compositor Loop (True 60 FPS Render Tick, 16.67ms Pacing)
 * ============================================================================== */

static void run_compositor_loop(void) {
    log_status(STATUS_OKAY, "iris_compositor", "IPC server listening on control channel (60 FPS render tick).");

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

    const zx_duration_t FRAME_PERIOD_NS = 16666667ULL; /* 60 FPS (~16.67 ms) */
    zx_time_t next_frame_deadline = zx_clock_get_monotonic() + FRAME_PERIOD_NS;

    while (active_clients > 0) {
        /* 1. Poll input devices non-blocking */
        if (has_input_polling) {
            poll_input_devices();
        }

        /* 2. Drain all pending IPC messages non-blocking across all active client channels */
        for (int c = 0; c < MAX_COMP_CLIENTS; ++c) {
            zx_handle_t ch = clients[c];
            if (ch == ZX_HANDLE_INVALID) continue;

            for (;;) {
                uint32_t act_bytes   = 0;
                uint32_t act_handles = 0;
                zx_status_t st = zx_channel_read(ch, 0,
                                                 req_buf, handles,
                                                 sizeof(req_buf), IRIS_IPC_MAX_HANDLES,
                                                 &act_bytes, &act_handles);
                if (st == ZX_ERR_SHOULD_WAIT) {
                    break;
                }
                if (st != ZX_OK) {
                    if (st == ZX_ERR_PEER_CLOSED) {
                        destroy_client_windows(c);
                        zx_handle_close(ch);
                        clients[c] = ZX_HANDLE_INVALID;
                        active_clients--;
                    }
                    break;
                }

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
        }

        /* 3. Render Tick Pacing: when deadline reached, perform AT MOST one render pass */
        zx_time_t now = zx_clock_get_monotonic();
        if (now >= next_frame_deadline) {
            if (g_dirty_active) {
                composite_and_flush(g_dirty_rect.x, g_dirty_rect.y, g_dirty_rect.w, g_dirty_rect.h);
                g_dirty_active = false;
                g_cursor_dirty = false;
            } else if (g_cursor_dirty) {
                update_screen(0, 0, 0, 0);
                g_cursor_dirty = false;
            }

            next_frame_deadline += FRAME_PERIOD_NS;
            if (next_frame_deadline <= now) {
                next_frame_deadline = now + FRAME_PERIOD_NS;
            }
        }

        /* 4. Sleep or wait on channel readability until next frame tick */
        now = zx_clock_get_monotonic();
        if (next_frame_deadline > now) {
            zx_duration_t wait_dur = next_frame_deadline - now;
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
                zx_time_t deadline = zx_clock_get_monotonic() + wait_dur;
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
            } else {
                zx_nanosleep(zx_clock_get_monotonic() + wait_dur);
            }
        }
    }
}

/* ==============================================================================
 * Process Entry Point
 * ============================================================================== */

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

    /* 1. Initialize FreeType 2 font engine and load Inter Display fonts */
    zx_status_t st = init_font_subsystem();
    if (st != ZX_OK) {
        log_status(STATUS_WARN, "iris_compositor", "Font subsystem initialized with fallback.");
    }

    /* 2. Connect to virtio_gpu scanout (1280x800) */
    st = connect_to_virtio_gpu();
    if (st != ZX_OK) {
        return 1;
    }

    /* 3. Initialize 18x24 RGBA classic macOS arrow software cursor sprite */
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

    /* Check command line arguments and /tmp/compositor_crash.log for crash recovery dialog */
    bool show_crash = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && strcmp(argv[i], "--show-crash-dialog") == 0) {
            show_crash = true;
            break;
        }
    }

    char crash_log_buf[512];
    memset(crash_log_buf, 0, sizeof(crash_log_buf));
    FILE* fp = fopen("/tmp/compositor_crash.log", "r");
    if (fp) {
        show_crash = true;
        size_t nr = fread(crash_log_buf, 1, sizeof(crash_log_buf) - 1, fp);
        if (nr > 0) {
            crash_log_buf[nr] = '\0';
        }
        fclose(fp);
        unlink("/tmp/compositor_crash.log");
    }

    if (show_crash) {
        if (crash_log_buf[0] == '\0') {
            snprintf(crash_log_buf, sizeof(crash_log_buf),
                     "Process: bin/iris_compositor\n"
                     "Status: Restarted by supervisor\n"
                     "Action: Auto-recovered\n");
        }
        init_crash_dialog(crash_log_buf);
        log_status(STATUS_OKAY, "iris_compositor", "Spawned modal crash recovery dialog (System Notice).");
    }

    /* 5. Initialize VirtIO Input (keyboard + tablet) and UART serial routing */
    init_input_subsystems();

    /* 6. Composite initial desktop wallpaper + welcome surface + cursor and flush scanout */
    composite_and_flush(0, 0, g_fb_width, g_fb_height);
    log_status(STATUS_OKAY, "iris_compositor", "Rendered 1280x800 desktop, Inter Display typography, and 18x24 macOS cursor.");

    run_compositor_loop();

    /* 7. Clean teardown */
    g_shutdown = true;
    if (g_crash_dialog_active) {
        close_crash_dialog();
    }
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
    cleanup_font_subsystem();

    log_status(STATUS_OKAY, "iris_compositor", "Daemon exited cleanly.");
    return 0;
}
