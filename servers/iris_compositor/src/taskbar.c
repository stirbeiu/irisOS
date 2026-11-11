/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_compositor/src/taskbar.c
 * Purpose: Windows-like bottom taskbar module:
 *          - Dynamic hardware RTC clock & date with per-minute damage updates
 *          - Material Icons system tray (Volume, Bluetooth, Network, Battery, Bell, Chevron)
 *          - Centered application dock with active running indicator pills
 *          - App lifecycle (launch via posix_spawn, focus, minimize, restore)
 *          - Start Menu, Calendar, and Volume flyouts with click outside dismissal
 *
 * Licensing: MIT License. Permissive Zone 1 system server module.
 */

#include "taskbar.h"
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

/* ==============================================================================
 * Taskbar State
 * ============================================================================== */

static time_t           g_last_clock_min = 0;
static char             g_time_str[16]   = "12:00 PM";
static char             g_date_str[24]   = "Mon, 01 Jan";

static uint8_t          g_volume_pct     = 75;
static bool             g_volume_muted   = false;
static uint8_t          g_battery_pct    = 100;
static bool             g_battery_charge = true;
static bool             g_net_online     = true;

static taskbar_flyout_t g_active_flyout   = FLYOUT_NONE;
static int32_t          g_flyout_x       = 0;
static int32_t          g_flyout_y       = 0;
static uint32_t         g_flyout_w       = 0;
static uint32_t         g_flyout_h       = 0;

#define TASKBAR_APP_MAX 8

/* Registered Application Definition */
typedef struct taskbar_app {
    const char* id;
    const char* name;
    const char* bin_path;
    bool        is_pinned;
    uint32_t    window_id;
    bool        is_running;
    bool        is_focused;
    bool        is_minimized;
} taskbar_app_t;

static taskbar_app_t g_dock_apps[TASKBAR_APP_MAX];
static size_t        g_num_dock_apps = 0;

/* Channel inherited for spawning apps with PTY capability */
static zx_handle_t   g_taskbar_pty_channel = ZX_HANDLE_INVALID;

/* Helper to draw thick lines */
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

/* ==============================================================================
 * Material Icons System Tray Renderers
 * ============================================================================== */

/* Volume Icon (volume_up or volume_off) */
static void draw_material_volume(pixman_image_t* surf, int32_t x, int32_t y, uint32_t color, bool muted) {
    /* Speaker cone body */
    fill_rect_pixman(surf, x + 2, y + 6, 4, 6, color);
    fill_rect_pixman(surf, x + 6, y + 4, 2, 10, color);
    fill_rect_pixman(surf, x + 8, y + 2, 2, 14, color);

    if (muted) {
        /* Mute slash */
        draw_line_thick(surf, x + 12, y + 4, x + 17, y + 14, color);
        draw_line_thick(surf, x + 17, y + 4, x + 12, y + 14, color);
    } else {
        /* Sound waves */
        fill_rect_pixman(surf, x + 12, y + 6, 2, 6, color);
        fill_rect_pixman(surf, x + 15, y + 4, 2, 10, color);
    }
}

/* Bluetooth Icon */
static void draw_material_bluetooth(pixman_image_t* surf, int32_t x, int32_t y, uint32_t color) {
    fill_rect_pixman(surf, x + 7, y + 2, 2, 14, color);
    draw_line_thick(surf, x + 7, y + 2, x + 12, y + 6, color);
    draw_line_thick(surf, x + 12, y + 6, x + 3, y + 12, color);
    draw_line_thick(surf, x + 3, y + 6, x + 12, y + 12, color);
    draw_line_thick(surf, x + 12, y + 12, x + 7, y + 16, color);
}

/* Network Icon (Ethernet / WiFi) */
static void draw_material_network(pixman_image_t* surf, int32_t x, int32_t y, uint32_t color, bool online) {
    if (online) {
        /* Monitor + stand */
        fill_rect_pixman(surf, x + 2, y + 2, 14, 10, color);
        fill_rect_pixman(surf, x + 4, y + 4, 10, 6, 0xFFF2F4F8u);
        fill_rect_pixman(surf, x + 7, y + 12, 4, 2, color);
        fill_rect_pixman(surf, x + 5, y + 14, 8, 2, color);
    } else {
        fill_rect_pixman(surf, x + 2, y + 2, 14, 10, 0xFF9CA3AFu);
        fill_rect_pixman(surf, x + 4, y + 4, 10, 6, 0xFFF2F4F8u);
        draw_line_thick(surf, x + 6, y + 5, x + 11, y + 9, 0xFFEF4444u);
        draw_line_thick(surf, x + 11, y + 5, x + 6, y + 9, 0xFFEF4444u);
    }
}

/* Battery Icon (battery_full with lightning bolt) */
static void draw_material_battery(pixman_image_t* surf, int32_t x, int32_t y, uint32_t color, uint8_t pct, bool charging) {
    /* Battery positive terminal nub */
    fill_rect_pixman(surf, x + 5, y + 1, 4, 2, color);
    /* Outer battery case */
    fill_rect_pixman(surf, x + 2, y + 3, 10, 14, color);
    fill_rect_pixman(surf, x + 4, y + 5, 6, 10, 0xFFF2F4F8u);

    /* Fill level */
    uint32_t fill_h = (uint32_t)((pct * 10) / 100);
    if (fill_h > 10) fill_h = 10;
    if (fill_h > 0) {
        uint32_t fill_col = (pct <= 20) ? 0xFFEF4444u : color;
        fill_rect_pixman(surf, x + 4, y + 15 - (int32_t)fill_h, 6, fill_h, fill_col);
    }

    if (charging) {
        /* Small lightning bolt overlay */
        fill_rect_pixman(surf, x + 6, y + 6, 2, 3, 0xFFF59E0Bu);
        fill_rect_pixman(surf, x + 5, y + 8, 4, 2, 0xFFF59E0Bu);
        fill_rect_pixman(surf, x + 6, y + 10, 2, 3, 0xFFF59E0Bu);
    }
}

/* Notification Bell Icon */
static void draw_material_bell(pixman_image_t* surf, int32_t x, int32_t y, uint32_t color) {
    fill_rect_pixman(surf, x + 7, y + 2, 2, 2, color);
    fill_rect_pixman(surf, x + 4, y + 4, 8, 8, color);
    fill_rect_pixman(surf, x + 2, y + 12, 12, 2, color);
    fill_rect_pixman(surf, x + 6, y + 14, 4, 2, color);
}

/* Chevron Up Icon */
static void draw_material_chevron_up(pixman_image_t* surf, int32_t x, int32_t y, uint32_t color) {
    /* Circular button badge */
    fill_rect_pixman(surf, x + 3, y, 10, 16, color);
    fill_rect_pixman(surf, x, y + 3, 16, 10, color);
    fill_rect_pixman(surf, x + 1, y + 1, 14, 14, color);
    /* Up arrow glyph */
    draw_line_thick(surf, x + 4, y + 9, x + 8, y + 5, 0xFFFFFFFFu);
    draw_line_thick(surf, x + 8, y + 5, x + 12, y + 9, 0xFFFFFFFFu);
}

/* Terminal Dock Icon */
static void draw_terminal_icon(pixman_image_t* surf, int32_t x, int32_t y) {
    /* Rounded 30x30 dark charcoal badge (#1C1C22) */
    fill_rect_pixman(surf, x + 3, y, 24, 30, 0xFF1C1C22u);
    fill_rect_pixman(surf, x, y + 3, 30, 24, 0xFF1C1C22u);
    fill_rect_pixman(surf, x + 1, y + 1, 28, 28, 0xFF1C1C22u);

    /* Mini caption buttons on top right of terminal icon */
    fill_rect_pixman(surf, x + 16, y + 4, 3, 2, 0xFF9CA3AFu);
    fill_rect_pixman(surf, x + 20, y + 4, 3, 2, 0xFF9CA3AFu);
    fill_rect_pixman(surf, x + 24, y + 4, 3, 2, 0xFF9CA3AFu);

    /* Prompt glyph: >_ */
    draw_line_thick(surf, x + 6, y + 11, x + 11, y + 15, 0xFFFFFFFFu);
    draw_line_thick(surf, x + 11, y + 15, x + 6, y + 19, 0xFFFFFFFFu);
    fill_rect_pixman(surf, x + 14, y + 19, 7, 2, 0xFFFFFFFFu);
}

/* ==============================================================================
 * Taskbar Subsystem Lifecycle & Sync
 * ============================================================================== */

void taskbar_init(void) {
    g_num_dock_apps = 0;

    /* Register default Terminal application */
    taskbar_app_t* term = &g_dock_apps[g_num_dock_apps++];
    term->id           = "terminal";
    term->name         = "Terminal";
    term->bin_path     = "/System/Bin/terminal";
    term->is_pinned    = true;
    term->window_id    = 0;
    term->is_running   = false;
    term->is_focused   = false;
    term->is_minimized = false;

    /* Initial clock format */
    taskbar_update_clock();
}

void taskbar_update_clock(void) {
    time_t now = time(NULL);
    if (now == (time_t)-1 || now <= 0) {
        return;
    }

    time_t cur_min = now / 60;
    if (cur_min == g_last_clock_min) {
        return;
    }
    g_last_clock_min = cur_min;

    struct tm tm_val;
    struct tm* tm = localtime_r(&now, &tm_val);
    if (tm) {
        int hour12 = tm->tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        const char* ampm = (tm->tm_hour >= 12) ? "PM" : "AM";
        snprintf(g_time_str, sizeof(g_time_str), "%d:%02d %s", hour12, tm->tm_min, ampm);

        static const char* const k_days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        static const char* const k_months[] = {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
        };
        const char* day_name = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? k_days[tm->tm_wday] : "Day";
        const char* mon_name = (tm->tm_mon >= 0 && tm->tm_mon < 12) ? k_months[tm->tm_mon] : "Mon";
        snprintf(g_date_str, sizeof(g_date_str), "%s, %02d %s", day_name, tm->tm_mday, mon_name);
    }

    /* Dirty taskbar clock area */
    if (g_fb_width >= 80u && g_fb_height >= IRIS_TASKBAR_H) {
        dirty_rect_union((int32_t)g_fb_width - 80, (int32_t)g_fb_height - IRIS_TASKBAR_H, 80, IRIS_TASKBAR_H);
    }
}

void taskbar_sync_windows(void) {
    for (size_t a = 0; a < g_num_dock_apps; ++a) {
        taskbar_app_t* app = &g_dock_apps[a];
        app->is_running   = false;
        app->is_focused   = false;
        app->is_minimized = false;
        app->window_id    = 0;

        for (size_t w = 0; w < g_num_windows; ++w) {
            comp_window_t* win = &g_windows[w];
            /* Match Terminal window by title prefix */
            if (strcmp(app->id, "terminal") == 0 &&
                (strstr(win->title, "toybox") != NULL || strstr(win->title, "terminal") != NULL)) {
                app->is_running   = true;
                app->window_id    = win->window_id;
                app->is_minimized = win->is_minimized;
                app->is_focused   = (w + 1 == g_num_windows);
                break;
            }
        }

        if (!app->is_running && g_num_windows == 0 && g_welcome_surface != NULL) {
            if (strcmp(app->id, "terminal") == 0) {
                app->is_running   = true;
                app->window_id    = 0;
                app->is_minimized = g_welcome_win.is_minimized;
                app->is_focused   = !g_welcome_win.is_minimized;
            }
        }
    }
}

/* ==============================================================================
 * Taskbar Rendering
 * ============================================================================== */

void taskbar_render(uint32_t* dst_pixels, pixman_image_t* dst_surface) {
    if (!dst_pixels || !dst_surface || g_fb_height < IRIS_TASKBAR_H) {
        return;
    }

    int32_t tb_y = (int32_t)(g_fb_height - IRIS_TASKBAR_H);

    /* 1. Light taskbar background (#F2F4F8) with 1px top separator (#D0D5DD) */
    fill_rect_pixman(dst_surface, 0, tb_y, g_fb_width, 1, 0xFFD0D5DDu);
    fill_rect_pixman(dst_surface, 0, tb_y + 1, g_fb_width, IRIS_TASKBAR_H - 1, 0xFFF2F4F8u);

    /* 2. Left: irisOS Eye Start Logo from /System/Resources/eye_light.svg */
    render_eye_light_svg(dst_pixels, g_fb_width, g_fb_height, 16, tb_y + 9, 43, 28);

    /* 3. Center: Application Dock */
    taskbar_sync_windows();
    int32_t dock_x = (int32_t)(g_fb_width / 2u) - 15;
    int32_t dock_y = tb_y + 6;

    draw_terminal_icon(dst_surface, dock_x, dock_y);

    /* Active running indicator pill underneath terminal icon */
    if (g_dock_apps[0].is_running) {
        uint32_t pill_color = g_dock_apps[0].is_focused ? 0xFF141418u : 0xFF6B7280u;
        if (!g_dock_apps[0].is_minimized) {
            fill_rect_pixman(dst_surface, dock_x + 8, tb_y + 40, 14, 3, pill_color);
        } else {
            /* Dot indicator when minimized */
            fill_rect_pixman(dst_surface, dock_x + 12, tb_y + 40, 6, 3, 0xFF9CA3AFu);
        }
    }

    /* 4. Right: Material Icons System Tray & Dynamic Clock */
    if (g_fb_width >= 480u) {
        int32_t tray_y   = tb_y + 15;
        uint32_t icon_col = 0xFF141418u;

        /* 4a. Chevron Up (expand/hidden) */
        int32_t chv_x = (int32_t)g_fb_width - 216;
        draw_material_chevron_up(dst_surface, chv_x, tray_y, icon_col);

        /* 4b. Volume Speaker */
        int32_t vol_x = (int32_t)g_fb_width - 190;
        draw_material_volume(dst_surface, vol_x, tray_y, icon_col, g_volume_muted);

        /* 4c. Bluetooth */
        int32_t bt_x = (int32_t)g_fb_width - 166;
        draw_material_bluetooth(dst_surface, bt_x, tray_y, icon_col);

        /* 4d. Network */
        int32_t net_x = (int32_t)g_fb_width - 144;
        draw_material_network(dst_surface, net_x, tray_y, icon_col, g_net_online);

        /* 4e. Battery with lightning bolt */
        int32_t bat_x = (int32_t)g_fb_width - 120;
        draw_material_battery(dst_surface, bat_x, tray_y, icon_col, g_battery_pct, g_battery_charge);

        /* 4f. Notification Bell */
        int32_t bel_x = (int32_t)g_fb_width - 98;
        draw_material_bell(dst_surface, bel_x, tray_y, icon_col);

        /* 4g. Two-line Real-Time Clock & Date */
        int32_t clk_x = (int32_t)g_fb_width - 76;
        draw_text_ft_styled(dst_pixels, g_fb_width, g_fb_height,
                            clk_x, tb_y + 9, 12, FONT_STYLE_BOLD, g_time_str, 0xFF141418u);
        draw_text_ft(dst_pixels, g_fb_width, g_fb_height,
                     clk_x - 4, tb_y + 24, 11, g_date_str, 0xFF374151u);
    }
}

/* ==============================================================================
 * Flyout Overlays (Start Menu, Calendar, Volume)
 * ============================================================================== */

bool taskbar_has_active_flyout(void) {
    return g_active_flyout != FLYOUT_NONE;
}

void taskbar_close_flyout(void) {
    if (g_active_flyout != FLYOUT_NONE) {
        g_active_flyout = FLYOUT_NONE;
        dirty_rect_union(g_flyout_x - 4, g_flyout_y - 4, g_flyout_w + 8, g_flyout_h + 8);
    }
}

void taskbar_render_flyout(uint32_t* dst_pixels, pixman_image_t* dst_surface) {
    if (g_active_flyout == FLYOUT_NONE || !dst_pixels || !dst_surface) {
        return;
    }

    if (g_active_flyout == FLYOUT_START_MENU) {
        /* Windows 11 style rounded card */
        int32_t fx = g_flyout_x;
        int32_t fy = g_flyout_y;
        uint32_t fw = g_flyout_w;
        uint32_t fh = g_flyout_h;

        /* Outer 1px subtle border (#D0D5DD) & card background (#FFFFFF) */
        fill_rect_pixman(dst_surface, fx - 1, fy - 1, fw + 2, fh + 2, 0xFFD0D5DDu);
        fill_rect_pixman(dst_surface, fx, fy, fw, fh, 0xFFFFFFFFu);

        /* Header: User Profile bar (#F4F5F7) */
        fill_rect_pixman(dst_surface, fx, fy, fw, 42, 0xFFF4F5F7u);
        fill_rect_pixman(dst_surface, fx + 12, fy + 9, 24, 24, 0xFF141418u);
        draw_text_ft_styled(dst_pixels, g_fb_width, g_fb_height,
                            fx + 44, fy + 12, 13, FONT_STYLE_BOLD, "admin", 0xFF141418u);
        draw_text_ft(dst_pixels, g_fb_width, g_fb_height,
                     fx + 44, fy + 26, 11, "irisOS-dev", 0xFF6B7280u);

        /* Pinned Applications Section Header */
        draw_text_ft_styled(dst_pixels, g_fb_width, g_fb_height,
                            fx + 16, fy + 56, 12, FONT_STYLE_BOLD, "Pinned Applications", 0xFF374151u);

        /* App 1: Terminal Launcher Item */
        int32_t item_y = fy + 80;
        fill_rect_pixman(dst_surface, fx + 12, item_y, fw - 24, 38, 0xFFF9FAFBu);
        draw_terminal_icon(dst_surface, fx + 18, item_y + 4);
        draw_text_ft_styled(dst_pixels, g_fb_width, g_fb_height,
                            fx + 56, item_y + 11, 13, FONT_STYLE_BOLD, "Terminal", 0xFF141418u);
        draw_text_ft(dst_pixels, g_fb_width, g_fb_height,
                     fx + 56, item_y + 25, 11, "Command Line & Shell", 0xFF6B7280u);

        /* App 2: System Info Item */
        int32_t item2_y = fy + 126;
        fill_rect_pixman(dst_surface, fx + 12, item2_y, fw - 24, 38, 0xFFF9FAFBu);
        fill_rect_pixman(dst_surface, fx + 18, item2_y + 4, 30, 30, 0xFF3B82F6u);
        draw_text_ft_styled(dst_pixels, g_fb_width, g_fb_height,
                            fx + 29, item2_y + 10, 15, FONT_STYLE_BOLD, "i", 0xFFFFFFFFu);
        draw_text_ft_styled(dst_pixels, g_fb_width, g_fb_height,
                            fx + 56, item2_y + 11, 13, FONT_STYLE_BOLD, "System Information", 0xFF141418u);
        draw_text_ft(dst_pixels, g_fb_width, g_fb_height,
                     fx + 56, item2_y + 25, 11, "irisOS 64-bit Microkernel", 0xFF6B7280u);

        /* Bottom Action Bar (#F4F5F7) */
        int32_t bot_y = fy + (int32_t)fh - 44;
        fill_rect_pixman(dst_surface, fx, bot_y, fw, 44, 0xFFF4F5F7u);

        /* Power Off / Restart button badges */
        fill_rect_pixman(dst_surface, fx + (int32_t)fw - 96, bot_y + 8, 84, 28, 0xFFE5E7EBu);
        draw_text_ft_styled(dst_pixels, g_fb_width, g_fb_height,
                            fx + (int32_t)fw - 84, bot_y + 16, 11, FONT_STYLE_BOLD, "Power Off", 0xFFDC2626u);
    } else if (g_active_flyout == FLYOUT_VOLUME) {
        int32_t fx = g_flyout_x;
        int32_t fy = g_flyout_y;
        uint32_t fw = g_flyout_w;
        uint32_t fh = g_flyout_h;

        fill_rect_pixman(dst_surface, fx - 1, fy - 1, fw + 2, fh + 2, 0xFFD0D5DDu);
        fill_rect_pixman(dst_surface, fx, fy, fw, fh, 0xFFFFFFFFu);

        /* Title & percentage */
        char vol_txt[32];
        snprintf(vol_txt, sizeof(vol_txt), "Volume: %u%%", g_volume_pct);
        draw_text_ft_styled(dst_pixels, g_fb_width, g_fb_height,
                            fx + 12, fy + 12, 12, FONT_STYLE_BOLD, vol_txt, 0xFF141418u);

        /* Slider track (#E5E7EB) */
        fill_rect_pixman(dst_surface, fx + 12, fy + 36, fw - 24, 6, 0xFFE5E7EBu);

        /* Slider active fill (#2563EBu) */
        uint32_t bar_w = ((fw - 24) * g_volume_pct) / 100u;
        fill_rect_pixman(dst_surface, fx + 12, fy + 36, bar_w, 6, 0xFF2563EBu);

        /* Slider thumb badge */
        fill_rect_pixman(dst_surface, fx + 12 + (int32_t)bar_w - 4, fy + 32, 8, 14, 0xFF141418u);
    } else if (g_active_flyout == FLYOUT_CALENDAR) {
        int32_t fx = g_flyout_x;
        int32_t fy = g_flyout_y;
        uint32_t fw = g_flyout_w;
        uint32_t fh = g_flyout_h;

        fill_rect_pixman(dst_surface, fx - 1, fy - 1, fw + 2, fh + 2, 0xFFD0D5DDu);
        fill_rect_pixman(dst_surface, fx, fy, fw, fh, 0xFFFFFFFFu);

        /* Header with time & date */
        fill_rect_pixman(dst_surface, fx, fy, fw, 46, 0xFFF4F5F7u);
        draw_text_ft_styled(dst_pixels, g_fb_width, g_fb_height,
                            fx + 14, fy + 12, 14, FONT_STYLE_BOLD, g_time_str, 0xFF141418u);
        draw_text_ft(dst_pixels, g_fb_width, g_fb_height,
                     fx + 14, fy + 28, 11, g_date_str, 0xFF4B5563u);

        /* Calendar grid preview */
        draw_text_ft_styled(dst_pixels, g_fb_width, g_fb_height,
                            fx + 14, fy + 60, 11, FONT_STYLE_BOLD, "Su  Mo  Tu  We  Th  Fr  Sa", 0xFF6B7280u);

        /* Days sample */
        draw_text_ft(dst_pixels, g_fb_width, g_fb_height,
                     fx + 14, fy + 80, 11, " 1   2   3   4   5   6   7", 0xFF374151u);
        draw_text_ft(dst_pixels, g_fb_width, g_fb_height,
                     fx + 14, fy + 102, 11, " 8   9  10  11  12  13  14", 0xFF374151u);
        draw_text_ft(dst_pixels, g_fb_width, g_fb_height,
                     fx + 14, fy + 124, 11, "15  16  17  18  19  20  21", 0xFF374151u);
        draw_text_ft(dst_pixels, g_fb_width, g_fb_height,
                     fx + 14, fy + 146, 11, "22  23  24  25  26  27  28", 0xFF374151u);
        draw_text_ft(dst_pixels, g_fb_width, g_fb_height,
                     fx + 14, fy + 168, 11, "29  30  31", 0xFF374151u);
    }
}

/* ==============================================================================
 * Application Spawning Helper
 * ============================================================================== */

static void spawn_terminal_app(void) {
    char* argv[] = { (char*)"terminal", NULL };
    pid_t pid = -1;
    posix_spawn(&pid, "/System/Bin/terminal", NULL, NULL, argv, environ);
}

/* ==============================================================================
 * Taskbar & Flyout Click Handling
 * ============================================================================== */

bool taskbar_handle_click(int32_t mx, int32_t my) {
    int32_t tb_top = (int32_t)g_fb_height - IRIS_TASKBAR_H;

    /* 1. If an active flyout is open, check if click is inside */
    if (g_active_flyout != FLYOUT_NONE) {
        if (mx >= g_flyout_x && mx < g_flyout_x + (int32_t)g_flyout_w &&
            my >= g_flyout_y && my < g_flyout_y + (int32_t)g_flyout_h) {
            /* Inside flyout */
            if (g_active_flyout == FLYOUT_START_MENU) {
                /* Check Terminal item click */
                int32_t item_y = g_flyout_y + 80;
                if (my >= item_y && my < item_y + 38) {
                    taskbar_sync_windows();
                    if (!g_dock_apps[0].is_running) {
                        spawn_terminal_app();
                    } else {
                        comp_window_t* w = find_window(g_dock_apps[0].window_id, NULL);
                        if (w) {
                            w->is_minimized = false;
                        }
                    }
                    taskbar_close_flyout();
                    dirty_rect_union(0, 0, g_fb_width, g_fb_height);
                    return true;
                }

                /* Check Power Off button click */
                int32_t bot_y = g_flyout_y + (int32_t)g_flyout_h - 44;
                if (my >= bot_y + 8 && my < bot_y + 36 &&
                    mx >= g_flyout_x + (int32_t)g_flyout_w - 96 && mx < g_flyout_x + (int32_t)g_flyout_w - 12) {
                    /* Request reboot / poweroff */
                    char* p_argv[] = { (char*)"poweroff", NULL };
                    pid_t p_pid = -1;
                    posix_spawn(&p_pid, "/System/Bin/poweroff", NULL, NULL, p_argv, environ);
                    taskbar_close_flyout();
                    return true;
                }
            } else if (g_active_flyout == FLYOUT_VOLUME) {
                /* Adjust volume slider */
                if (mx >= g_flyout_x + 12 && mx <= g_flyout_x + (int32_t)g_flyout_w - 12 &&
                    my >= g_flyout_y + 26 && my <= g_flyout_y + 50) {
                    int32_t rel = mx - (g_flyout_x + 12);
                    int32_t total = (int32_t)g_flyout_w - 24;
                    if (total > 0) {
                        int32_t pct = (rel * 100) / total;
                        if (pct < 0) pct = 0;
                        if (pct > 100) pct = 100;
                        g_volume_pct = (uint8_t)pct;
                        dirty_rect_union(g_flyout_x, g_flyout_y, g_flyout_w, g_flyout_h);
                    }
                    return true;
                }
            }
            return true;
        }

        /* Click outside active flyout: close it */
        taskbar_close_flyout();
        /* If the click is above the taskbar, allow it to pass through to windows */
        if (my < tb_top) {
            return false;
        }
    }

    if (my < tb_top) {
        return false;
    }

    /* 2. Left: irisOS Eye Start Button click (x = 12..64) */
    if (mx >= 12 && mx <= 64) {
        if (g_active_flyout == FLYOUT_START_MENU) {
            taskbar_close_flyout();
        } else {
            g_active_flyout = FLYOUT_START_MENU;
            g_flyout_w      = 280u;
            g_flyout_h      = 260u;
            g_flyout_x      = 12;
            g_flyout_y      = tb_top - (int32_t)g_flyout_h - 8;
            dirty_rect_union(g_flyout_x - 2, g_flyout_y - 2, g_flyout_w + 4, g_flyout_h + 4);
        }
        return true;
    }

    /* 3. Center: Terminal Dock Icon click */
    int32_t dock_x = (int32_t)(g_fb_width / 2u) - 15;
    if (mx >= dock_x - 6 && mx <= dock_x + 36) {
        taskbar_sync_windows();
        if (!g_dock_apps[0].is_running) {
            /* App not running: launch it! */
            spawn_terminal_app();
        } else {
            /* App is running: toggle minimize / bring to focus */
            comp_window_t* w = find_window(g_dock_apps[0].window_id, NULL);
            if (w) {
                if (w->is_minimized) {
                    w->is_minimized = false;
                    /* Bring to front of Z-order */
                    size_t widx = 0;
                    find_window(w->window_id, &widx);
                    if (widx + 1 < g_num_windows) {
                        comp_window_t picked = g_windows[widx];
                        for (size_t j = widx; j + 1 < g_num_windows; ++j) {
                            g_windows[j] = g_windows[j + 1];
                        }
                        g_windows[g_num_windows - 1] = picked;
                    }
                } else if (g_dock_apps[0].is_focused) {
                    /* Minimize when clicked while already top focused */
                    w->is_minimized = true;
                } else {
                    /* Bring to front */
                    size_t widx = 0;
                    find_window(w->window_id, &widx);
                    if (widx + 1 < g_num_windows) {
                        comp_window_t picked = g_windows[widx];
                        for (size_t j = widx; j + 1 < g_num_windows; ++j) {
                            g_windows[j] = g_windows[j + 1];
                        }
                        g_windows[g_num_windows - 1] = picked;
                    }
                }
            } else if (g_welcome_surface != NULL && g_num_windows == 0) {
                g_welcome_win.is_minimized = !g_welcome_win.is_minimized;
            }
        }
        dirty_rect_union(0, 0, g_fb_width, g_fb_height);
        return true;
    }

    /* 4. Right: System Tray Icon clicks */
    int32_t vol_x = (int32_t)g_fb_width - 190;
    if (mx >= vol_x - 4 && mx <= vol_x + 20) {
        if (g_active_flyout == FLYOUT_VOLUME) {
            taskbar_close_flyout();
        } else {
            g_active_flyout = FLYOUT_VOLUME;
            g_flyout_w      = 180u;
            g_flyout_h      = 60u;
            g_flyout_x      = (int32_t)g_fb_width - 210;
            g_flyout_y      = tb_top - (int32_t)g_flyout_h - 8;
            dirty_rect_union(g_flyout_x - 2, g_flyout_y - 2, g_flyout_w + 4, g_flyout_h + 4);
        }
        return true;
    }

    /* Clock & Date click -> Calendar Flyout */
    int32_t clk_x = (int32_t)g_fb_width - 80;
    if (mx >= clk_x - 8) {
        if (g_active_flyout == FLYOUT_CALENDAR) {
            taskbar_close_flyout();
        } else {
            g_active_flyout = FLYOUT_CALENDAR;
            g_flyout_w      = 220u;
            g_flyout_h      = 195u;
            g_flyout_x      = (int32_t)g_fb_width - 230;
            g_flyout_y      = tb_top - (int32_t)g_flyout_h - 8;
            dirty_rect_union(g_flyout_x - 2, g_flyout_y - 2, g_flyout_w + 4, g_flyout_h + 4);
        }
        return true;
    }

    return true;
}
