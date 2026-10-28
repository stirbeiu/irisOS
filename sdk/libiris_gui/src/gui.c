/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris_gui/src/gui.c
 * Purpose: Client windowing library implementation (VMO allocation, VMAR mapping,
 *          Pixman surface wrapping, and IRIS_PROTO_COMPOSITOR IPC).
 *
 * Licensing: MIT License. Permissive Zone 1 SDK library.
 */

#include <iris/gui.h>
#include <iris/ipc.h>
#include <iris/channel.h>
#include <iris/init.h>

#include <stdlib.h>
#include <string.h>
#include <zircon/syscalls.h>
#include <zircon/process.h>
#include <zircon/processargs.h>

#define GUI_PAGE_SIZE 4096ULL
#define GUI_PAGE_MASK (~(GUI_PAGE_SIZE - 1ULL))

#ifndef ZX_SEC
#define ZX_SEC(n) ((zx_duration_t)(n) * 1000000000ULL)
#endif

/* Weak reference to mlibc sysdeps compositor channel accessor if linked with mlibc */
__attribute__((weak)) zx_handle_t iris_get_compositor_channel(void);

static zx_handle_t g_comp_channel = ZX_HANDLE_INVALID;
static uint32_t    g_gui_txid     = 1000u;

void iris_gui_set_channel(zx_handle_t comp_channel) {
    g_comp_channel = comp_channel;
}

zx_handle_t iris_gui_get_channel(void) {
    if (g_comp_channel != ZX_HANDLE_INVALID) {
        return g_comp_channel;
    }
    if (iris_get_compositor_channel) {
        g_comp_channel = iris_get_compositor_channel();
        if (g_comp_channel != ZX_HANDLE_INVALID) {
            return g_comp_channel;
        }
    }
    g_comp_channel = zx_take_startup_handle(PA_HND(PA_USER0, 1));
    if (g_comp_channel == ZX_HANDLE_INVALID) {
        g_comp_channel = zx_take_startup_handle(PA_HND(PA_USER0, 0));
    }
    return g_comp_channel;
}

iris_window_t* iris_window_create(const char *title, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > 8192u || height > 8192u) {
        return NULL;
    }

    zx_handle_t comp_ch = iris_gui_get_channel();
    if (comp_ch == ZX_HANDLE_INVALID) {
        return NULL;
    }

    iris_window_t* win = (iris_window_t*)calloc(1, sizeof(iris_window_t));
    if (!win) {
        return NULL;
    }

    win->width        = width;
    win->height       = height;
    win->stride       = width * 4u;
    win->comp_channel = comp_ch;
    win->vmo          = ZX_HANDLE_INVALID;

    if (title) {
        size_t tlen = strlen(title);
        if (tlen >= sizeof(win->title)) {
            tlen = sizeof(win->title) - 1;
        }
        memcpy(win->title, title, tlen);
        win->title[tlen] = '\0';
    } else {
        memcpy(win->title, "irisOS Window", 14);
    }

    /* 1. Allocate page-aligned Zircon VMO for 32-bit ARGB8888 surface */
    uint64_t raw_bytes = (uint64_t)win->stride * (uint64_t)height;
    win->vmo_size = (raw_bytes + GUI_PAGE_SIZE - 1ULL) & GUI_PAGE_MASK;

    zx_status_t st = zx_vmo_create(win->vmo_size, 0, &win->vmo);
    if (st != ZX_OK || win->vmo == ZX_HANDLE_INVALID) {
        free(win);
        return NULL;
    }

    /* 2. Map shared VMO into current process address space */
    zx_vaddr_t mapped_va = 0;
    st = zx_vmar_map(zx_vmar_root_self(),
                     ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                     0, win->vmo, 0, win->vmo_size, &mapped_va);
    if (st != ZX_OK || mapped_va == 0) {
        zx_handle_close(win->vmo);
        free(win);
        return NULL;
    }

    win->vaddr  = mapped_va;
    win->pixels = (uint32_t*)mapped_va;

    /* Initialize surface to opaque dark Catppuccin base (0xFF1E1E2E) */
    size_t total_px = (size_t)width * (size_t)height;
    for (size_t i = 0; i < total_px; ++i) {
        win->pixels[i] = 0xFF1E1E2Eu;
    }

    /* 3. Wrap mapped pixel buffer in a 32-bit Pixman image (PIXMAN_a8r8g8b8) */
    win->surface = pixman_image_create_bits(PIXMAN_a8r8g8b8,
                                            (int)width,
                                            (int)height,
                                            win->pixels,
                                            (int)win->stride);
    if (!win->surface) {
        zx_vmar_unmap(zx_vmar_root_self(), win->vaddr, win->vmo_size);
        zx_handle_close(win->vmo);
        free(win);
        return NULL;
    }

    /* 4. Duplicate VMO handle and transfer to iris_compositor via IPC */
    zx_handle_t dup_vmo = ZX_HANDLE_INVALID;
    st = zx_handle_duplicate(win->vmo, ZX_RIGHT_SAME_RIGHTS, &dup_vmo);
    if (st != ZX_OK || dup_vmo == ZX_HANDLE_INVALID) {
        pixman_image_unref(win->surface);
        zx_vmar_unmap(zx_vmar_root_self(), win->vaddr, win->vmo_size);
        zx_handle_close(win->vmo);
        free(win);
        return NULL;
    }

    iris_compositor_create_window_req_t req;
    memset(&req, 0, sizeof(req));
    req.width  = width;
    req.height = height;
    req.flags  = 0;
    req.x      = -1; /* Automatic centering/cascade */
    req.y      = -1;
    memcpy(req.title, win->title, sizeof(req.title));

    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_t resp_hdr;
    iris_compositor_create_window_resp_t resp;
    memset(&resp, 0, sizeof(resp));

    uint32_t txid = ++g_gui_txid;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_COMPOSITOR,
                      IRIS_COMPOSITOR_OP_CREATE_WINDOW, txid, sizeof(req));

    uint32_t act_payload = 0;
    st = iris_channel_call(comp_ch,
                           &req_hdr, &req,
                           &dup_vmo, 1,
                           &resp_hdr, &resp, sizeof(resp), &act_payload,
                           NULL, 0, NULL,
                           ZX_TIME_INFINITE);
    if (st != ZX_OK || resp_hdr.status != ZX_OK || resp.window_id == 0) {
        if (st != ZX_OK) {
            zx_handle_close(dup_vmo);
        }
        pixman_image_unref(win->surface);
        zx_vmar_unmap(zx_vmar_root_self(), win->vaddr, win->vmo_size);
        zx_handle_close(win->vmo);
        free(win);
        return NULL;
    }

    win->window_id = resp.window_id;
    return win;
}

void iris_window_destroy(iris_window_t *win) {
    if (!win) {
        return;
    }

    if (win->comp_channel != ZX_HANDLE_INVALID && win->window_id != 0) {
        iris_compositor_destroy_window_req_t req;
        req.window_id = win->window_id;

        iris_msg_hdr_t req_hdr;
        iris_msg_hdr_t resp_hdr;
        uint32_t txid = ++g_gui_txid;
        iris_msg_hdr_init(&req_hdr, IRIS_PROTO_COMPOSITOR,
                          IRIS_COMPOSITOR_OP_DESTROY_WINDOW, txid, sizeof(req));

        iris_channel_call(win->comp_channel,
                          &req_hdr, &req,
                          NULL, 0,
                          &resp_hdr, NULL, 0, NULL,
                          NULL, 0, NULL,
                          ZX_TIME_INFINITE);
    }

    if (win->surface) {
        pixman_image_unref(win->surface);
        win->surface = NULL;
    }

    if (win->vaddr != 0 && win->vmo_size > 0) {
        zx_vmar_unmap(zx_vmar_root_self(), win->vaddr, win->vmo_size);
        win->vaddr  = 0;
        win->pixels = NULL;
    }

    if (win->vmo != ZX_HANDLE_INVALID) {
        zx_handle_close(win->vmo);
        win->vmo = ZX_HANDLE_INVALID;
    }

    free(win);
}

void iris_window_damage(iris_window_t *win, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!win || win->comp_channel == ZX_HANDLE_INVALID || win->window_id == 0) {
        return;
    }

    /* Ensure memory stores to the shared VMO are visible prior to signaling compositor */
    __sync_synchronize();

    iris_compositor_damage_req_t req;
    req.window_id = win->window_id;
    req.x         = x;
    req.y         = y;
    req.w         = (w == 0) ? win->width  : w;
    req.h         = (h == 0) ? win->height : h;

    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_t resp_hdr;
    uint32_t txid = ++g_gui_txid;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_COMPOSITOR,
                      IRIS_COMPOSITOR_OP_DAMAGE, txid, sizeof(req));

    iris_channel_call(win->comp_channel,
                      &req_hdr, &req,
                      NULL, 0,
                      &resp_hdr, NULL, 0, NULL,
                      NULL, 0, NULL,
                      ZX_TIME_INFINITE);
}

pixman_image_t* iris_window_get_surface(iris_window_t *win) {
    if (!win) {
        return NULL;
    }
    return win->surface;
}

zx_status_t iris_window_move(iris_window_t *win, int32_t x, int32_t y) {
    if (!win || win->comp_channel == ZX_HANDLE_INVALID || win->window_id == 0) {
        return ZX_ERR_INVALID_ARGS;
    }

    iris_compositor_move_window_req_t req;
    req.window_id = win->window_id;
    req.x         = x;
    req.y         = y;

    iris_msg_hdr_t req_hdr;
    iris_msg_hdr_t resp_hdr;
    uint32_t txid = ++g_gui_txid;
    iris_msg_hdr_init(&req_hdr, IRIS_PROTO_COMPOSITOR,
                      IRIS_COMPOSITOR_OP_MOVE_WINDOW, txid, sizeof(req));

    zx_status_t st = iris_channel_call(win->comp_channel,
                                       &req_hdr, &req,
                                       NULL, 0,
                                       &resp_hdr, NULL, 0, NULL,
                                       NULL, 0, NULL,
                                       ZX_TIME_INFINITE);
    if (st != ZX_OK) {
        return st;
    }
    return resp_hdr.status;
}
