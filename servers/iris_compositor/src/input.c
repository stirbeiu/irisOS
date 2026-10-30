/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_compositor/src/input.c
 * Purpose: VirtIO Input (keyboard/tablet) and UART Serial event processing.
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 system server.
 */

#include "compositor.h"

/* ==============================================================================
 * UART Serial Console RX Thread
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

/* ==============================================================================
 * Input Subsystems Initialization
 * ============================================================================== */

void init_input_subsystems(void) {
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

/* ==============================================================================
 * Keyboard Event Routing (Decoupled from Rendering)
 * ============================================================================== */

void route_key_to_focused_window(char ch) {
    if (g_crash_dialog_active) {
        if (ch == '\r' || ch == '\n' || ch == ' ' || (uint8_t)ch == 27) {
            close_crash_dialog();
            return;
        }
    }

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
        /* ONLY redraw the 32px input box strip and mark dirty (no full flush!) */
        redraw_welcome_input_strip();
    }
}

/* ==============================================================================
 * Mouse Button Handling
 * ============================================================================== */

void handle_mouse_button(bool pressed) {
    g_btn_left_down = pressed;
    if (!pressed) {
        g_drag_window_id = 0;
        g_drag_welcome   = false;
        g_drag_crash     = false;
        return;
    }

    if (g_crash_dialog_active && g_crash_surface != NULL) {
        uint32_t fw = g_crash_win.client_w + (2u * IRIS_WIN_BORDER_PX);
        int32_t  tb_bottom = g_crash_win.y + IRIS_WIN_BORDER_PX + IRIS_WIN_TITLEBAR_H;

        /* Check red close button on title bar (tb_x + 10, tb_y + 8, size 8x8) */
        int32_t tb_x = g_crash_win.x + IRIS_WIN_BORDER_PX;
        int32_t tb_y = g_crash_win.y + IRIS_WIN_BORDER_PX;
        if (g_cursor_x >= tb_x + 6 && g_cursor_x <= tb_x + 22 &&
            g_cursor_y >= tb_y + 4 && g_cursor_y <= tb_y + 20) {
            close_crash_dialog();
            return;
        }

        /* Check "OK" button in client area (btn_x = cw - 110, btn_y = ch - 44, size 90x28) */
        int32_t client_top_x = g_crash_win.x + IRIS_WIN_BORDER_PX;
        int32_t client_top_y = g_crash_win.y + IRIS_WIN_BORDER_PX + IRIS_WIN_TITLEBAR_H;
        int32_t btn_x = client_top_x + (int32_t)g_crash_win.client_w - 110;
        int32_t btn_y = client_top_y + (int32_t)g_crash_win.client_h - 44;
        if (g_cursor_x >= btn_x && g_cursor_x < btn_x + 90 &&
            g_cursor_y >= btn_y && g_cursor_y < btn_y + 28) {
            close_crash_dialog();
            return;
        }

        /* Check if dragging title bar */
        if (g_cursor_x >= g_crash_win.x && g_cursor_x < g_crash_win.x + (int32_t)fw &&
            g_cursor_y >= g_crash_win.y && g_cursor_y < tb_bottom) {
            g_drag_crash = true;
            g_drag_off_x = g_cursor_x - g_crash_win.x;
            g_drag_off_y = g_cursor_y - g_crash_win.y;
        }
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
                dirty_rect_union(0, 0, g_fb_width, g_fb_height);
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

/* ==============================================================================
 * Input Device Polling & Event Coalescing
 * ============================================================================== */

void poll_input_devices(void) {
    bool cursor_moved = false;

    for (size_t d = 0; d < g_num_input_devs; ++d) {
        virtio_input_dev_t* dev = &g_input_devs[d];
        if (!dev->active) continue;

        virtio_input_event_t evs[32];
        size_t n_evs = 0;
        /* Coalesce: drain all pending events on this device */
        while (virtio_input_poll_raw(dev, evs, 32, &n_evs) == ZX_OK && n_evs > 0) {
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

    /* Update drag state and dirty rects on cursor move */
    if (cursor_moved) {
        g_cursor_dirty = true;

        if (g_btn_left_down && g_drag_crash && g_crash_dialog_active && g_crash_surface != NULL) {
            dirty_rect_union_window(&g_crash_win);
            g_crash_win.x = g_cursor_x - g_drag_off_x;
            g_crash_win.y = g_cursor_y - g_drag_off_y;
            if (g_crash_win.y < IRIS_TOPBAR_H) {
                g_crash_win.y = IRIS_TOPBAR_H;
            }
            dirty_rect_union_window(&g_crash_win);
        } else if (g_btn_left_down && g_drag_welcome && g_num_windows == 0 && g_welcome_surface != NULL) {
            dirty_rect_union_window(&g_welcome_win);
            g_welcome_win.x = g_cursor_x - g_drag_off_x;
            g_welcome_win.y = g_cursor_y - g_drag_off_y;
            if (g_welcome_win.y < IRIS_TOPBAR_H) {
                g_welcome_win.y = IRIS_TOPBAR_H;
            }
            dirty_rect_union_window(&g_welcome_win);
        } else if (g_btn_left_down && g_drag_window_id != 0) {
            comp_window_t* win = find_window(g_drag_window_id, NULL);
            if (win) {
                dirty_rect_union_window(win);
                win->x = g_cursor_x - g_drag_off_x;
                win->y = g_cursor_y - g_drag_off_y;
                if (win->y < IRIS_TOPBAR_H) {
                    win->y = IRIS_TOPBAR_H;
                }
                dirty_rect_union_window(win);
            }
        }
    }
}
