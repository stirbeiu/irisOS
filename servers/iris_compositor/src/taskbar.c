/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_compositor/src/taskbar.c
 * Purpose: Windows-like translucent gradient taskbar subsystem:
 *          - Start Menu with user profile, app list, recent apps & power/login controls
 *          - Centered pinned + running dock with line/dot indicators & drag-and-drop pinning
 *          - Material Icons system tray (3rd-party overflow ^, Volume, Bluetooth, Network, Power)
 *          - Separate Notification Center flyout, dynamic RTC Calendar & Privacy Indicator
 *
 * Licensing: MIT License. Permissive Zone 1 system server module.
 */

#include "taskbar.h"
#include "login.h"
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

extern char** environ;

/* ==============================================================================
 * Taskbar State & Catalog Definitions
 * ============================================================================== */

#define TASKBAR_APP_MAX      16
#define TASKBAR_RECENT_MAX   3
#define TASKBAR_NOTIF_MAX    8
#define TASKBAR_DOCK_ICON_SZ 30
#define TASKBAR_DOCK_STEP    40
#define TASKBAR_PINS_PATH    "/Users/admin/.taskbar_pins"

typedef enum {
    APP_ICON_TERMINAL = 0,
    APP_ICON_FILES,
    APP_ICON_SYSINFO,
    APP_ICON_GENERIC
} app_icon_kind_t;

typedef struct taskbar_app {
    char            id[24];
    char            name[32];
    char            subtitle[48];
    char            bin_path[64];
    app_icon_kind_t icon_kind;
    bool            is_catalog;
    bool            is_pinned;
    uint32_t        window_id;
    bool            is_running;
    bool            is_focused;
    bool            is_minimized;
} taskbar_app_t;

typedef struct taskbar_notif {
    char title[40];
    char body[80];
    char time_str[12];
} taskbar_notif_t;

typedef enum {
    DRAG_MODE_NONE = 0,
    DRAG_MODE_DOCK,
    DRAG_MODE_START_MENU
} taskbar_drag_mode_t;

static time_t           g_last_clock_min = 0;
static char             g_time_str[16]   = "12:00 PM";
static char             g_date_str[24]   = "Mon, 01 Jan";
static int              g_cur_day        = 1;
static int              g_cur_mon        = 0;
static int              g_cur_year       = 2026;
static int              g_cur_wday       = 1;

static uint8_t          g_volume_pct     = 75;
static bool             g_volume_muted   = false;
static bool             g_bt_enabled     = true;
static bool             g_net_online     = true;
static uint8_t          g_battery_pct    = 100;
static bool             g_battery_charge = true;

static uint32_t         g_privacy_mask   = IRIS_PRIVACY_NONE;
static char             g_privacy_owner[32] = "None";

static bool             g_is_sleeping    = false;
static bool             g_pins_loaded    = false;

static taskbar_flyout_t g_active_flyout  = FLYOUT_NONE;
static int32_t          g_flyout_x       = 0;
static int32_t          g_flyout_y       = 0;
static uint32_t         g_flyout_w       = 0;
static uint32_t         g_flyout_h       = 0;

static taskbar_app_t    g_apps[TASKBAR_APP_MAX];
static size_t           g_num_apps       = 0;
static size_t           g_dock_slots[TASKBAR_APP_MAX];
static size_t           g_num_dock_slots = 0;

static char             g_recent_ids[TASKBAR_RECENT_MAX][24];
static size_t           g_num_recent     = 0;

static taskbar_notif_t  g_notifs[TASKBAR_NOTIF_MAX];
static size_t           g_num_notifs     = 0;
static bool             g_unread_notifs  = false;

/* Pointer & Drag-and-Drop State */
static taskbar_drag_mode_t g_drag_mode    = DRAG_MODE_NONE;
static bool             g_drag_pending    = false;
static bool             g_drag_active     = false;
static size_t           g_drag_app_idx    = 0;
static int32_t          g_drag_start_x    = 0;
static int32_t          g_drag_start_y    = 0;
static int32_t          g_drag_cur_x      = 0;
static int32_t          g_drag_cur_y      = 0;

/* ==============================================================================
 * Geometric Fallback Helpers
 * ============================================================================== */

static void draw_line_thick_tb(pixman_image_t* surf, int32_t x0, int32_t y0,
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

static void draw_tray_glyph(uint32_t* dst_pixels, pixman_image_t* dst_surface,
                            int32_t x, int32_t y, uint32_t size,
                            uint32_t codepoint, uint32_t color) {
    if (draw_material_icon(dst_pixels, g_fb_width, g_fb_height, x, y, size, codepoint, color)) {
        return;
    }
    /* Minimal geometric fallback if MaterialIcons font is unavailable */
    fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, x + 2, y + 2, size - 4, size - 4, 3, color);
    (void)dst_surface;
}

static void draw_app_icon(uint32_t* dst_pixels, pixman_image_t* dst_surface,
                          int32_t x, int32_t y, uint32_t sz, app_icon_kind_t kind) {
    if (kind == APP_ICON_TERMINAL) {
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, x, y, sz, sz, 7, 0xFF1C1C22u);
        fill_rect_vgradient_over(dst_pixels, g_fb_width, g_fb_height, x + 3, y + 2, sz - 6, 4,
                                 0x30FFFFFFu, 0x10FFFFFFu);
        if (!draw_material_icon(dst_pixels, g_fb_width, g_fb_height,
                                x + 5, y + 6, sz - 10, MAT_ICON_TERMINAL, 0xFFFFFFFFu)) {
            draw_line_thick_tb(dst_surface, x + 6, y + 11, x + 11, y + 15, 0xFFFFFFFFu);
            draw_line_thick_tb(dst_surface, x + 11, y + 15, x + 6, y + 19, 0xFFFFFFFFu);
            fill_rect_pixman(dst_surface, x + 14, y + 19, 7, 2, 0xFFFFFFFFu);
        }
    } else if (kind == APP_ICON_FILES) {
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, x, y, sz, sz, 7, 0xFFF3F4F6u);
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, x + 1, y + 1, sz - 2, sz - 2, 6, 0xFFFFFFFFu);
        if (!draw_material_icon(dst_pixels, g_fb_width, g_fb_height,
                                x + 4, y + 4, sz - 8, MAT_ICON_FOLDER, 0xFF2563EBu)) {
            fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, x + 4, y + 7, sz - 8, sz - 12, 3, 0xFF2563EBu);
        }
    } else if (kind == APP_ICON_SYSINFO) {
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, x, y, sz, sz, 7, 0xFF2563EBu);
        if (!draw_material_icon(dst_pixels, g_fb_width, g_fb_height,
                                x + 4, y + 4, sz - 8, MAT_ICON_INFO, 0xFFFFFFFFu)) {
            draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height,
                            x + 11, y + 7, 15, FONT_FAMILY_INTER_DISPLAY,
                            FONT_WEIGHT_BOLD, false, "i", 0xFFFFFFFFu);
        }
    } else {
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, x, y, sz, sz, 7, 0xFF4B5563u);
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, x + 7, y + 7, sz - 14, sz - 14, 3, 0xFFFFFFFFu);
    }
}

/* ==============================================================================
 * Pin Persistence (/Users/admin/.taskbar_pins) & Recent Tracking
 * ============================================================================== */

static void save_pinned_apps(void) {
    struct stat st;
    if (stat("/Users/admin", &st) != 0) {
        return;
    }
    FILE* fp = fopen(TASKBAR_PINS_PATH, "w");
    if (!fp) {
        return;
    }
    for (size_t i = 0; i < g_num_apps; ++i) {
        if (g_apps[i].is_pinned) {
            fprintf(fp, "%s\n", g_apps[i].id);
        }
    }
    fclose(fp);
}

static void maybe_load_pinned_apps(void) {
    if (g_pins_loaded) return;
    struct stat st;
    if (stat("/Users/admin", &st) != 0) {
        return;
    }
    g_pins_loaded = true;
    FILE* fp = fopen(TASKBAR_PINS_PATH, "r");
    if (!fp) {
        return;
    }

    bool any_read = false;
    bool new_pin[TASKBAR_APP_MAX];
    memset(new_pin, 0, sizeof(new_pin));

    char line[64];
    size_t reorder_pos = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;
        for (size_t i = 0; i < g_num_apps; ++i) {
            if (strcmp(g_apps[i].id, line) == 0) {
                any_read = true;
                g_apps[i].is_pinned = true;
                new_pin[i] = true;
                if (i != reorder_pos && reorder_pos < g_num_apps) {
                    taskbar_app_t tmp = g_apps[reorder_pos];
                    g_apps[reorder_pos] = g_apps[i];
                    g_apps[i] = tmp;
                    new_pin[i] = new_pin[reorder_pos];
                    new_pin[reorder_pos] = true;
                }
                reorder_pos++;
                break;
            }
        }
    }
    fclose(fp);

    if (any_read) {
        for (size_t i = 0; i < g_num_apps; ++i) {
            if (!new_pin[i]) {
                g_apps[i].is_pinned = false;
            }
        }
    }
}

static ssize_t find_app_by_id(const char* id) {
    if (!id) return -1;
    for (size_t i = 0; i < g_num_apps; ++i) {
        if (strcmp(g_apps[i].id, id) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static void record_recent_app(size_t app_idx) {
    if (app_idx >= g_num_apps) return;
    const char* id = g_apps[app_idx].id;
    size_t existing = TASKBAR_RECENT_MAX;
    for (size_t i = 0; i < g_num_recent; ++i) {
        if (strcmp(g_recent_ids[i], id) == 0) {
            existing = i;
            break;
        }
    }
    if (existing < TASKBAR_RECENT_MAX) {
        for (size_t i = existing; i > 0; --i) {
            snprintf(g_recent_ids[i], sizeof(g_recent_ids[i]), "%s", g_recent_ids[i - 1]);
        }
        snprintf(g_recent_ids[0], sizeof(g_recent_ids[0]), "%s", id);
    } else {
        size_t limit = (g_num_recent < TASKBAR_RECENT_MAX) ? g_num_recent : (TASKBAR_RECENT_MAX - 1);
        for (size_t i = limit; i > 0; --i) {
            snprintf(g_recent_ids[i], sizeof(g_recent_ids[i]), "%s", g_recent_ids[i - 1]);
        }
        snprintf(g_recent_ids[0], sizeof(g_recent_ids[0]), "%s", id);
        if (g_num_recent < TASKBAR_RECENT_MAX) {
            g_num_recent++;
        }
    }
}

/* ==============================================================================
 * Notifications & Privacy Indicator API
 * ============================================================================== */

void taskbar_push_notification(const char* title, const char* body) {
    if (!title || !body) return;
    if (g_num_notifs >= TASKBAR_NOTIF_MAX) {
        for (size_t i = 0; i + 1 < TASKBAR_NOTIF_MAX; ++i) {
            g_notifs[i] = g_notifs[i + 1];
        }
        g_num_notifs = TASKBAR_NOTIF_MAX - 1;
    }
    /* Prepend newest at index 0 */
    for (size_t i = g_num_notifs; i > 0; --i) {
        g_notifs[i] = g_notifs[i - 1];
    }
    g_num_notifs++;

    snprintf(g_notifs[0].title, sizeof(g_notifs[0].title), "%s", title);
    snprintf(g_notifs[0].body, sizeof(g_notifs[0].body), "%s", body);
    snprintf(g_notifs[0].time_str, sizeof(g_notifs[0].time_str), "%s", g_time_str);
    g_unread_notifs = true;

    if (g_fb_width >= 260u && g_fb_height >= IRIS_TASKBAR_H) {
        dirty_rect_union((int32_t)g_fb_width - 260, (int32_t)g_fb_height - IRIS_TASKBAR_H, 260, IRIS_TASKBAR_H);
    }
}

void taskbar_set_privacy_active(uint32_t mask, const char* owner_app) {
    g_privacy_mask = mask;
    if (owner_app && owner_app[0] != '\0') {
        snprintf(g_privacy_owner, sizeof(g_privacy_owner), "%s", owner_app);
    } else {
        snprintf(g_privacy_owner, sizeof(g_privacy_owner), "%s", "System Service");
    }
    if (g_fb_width >= 40u && g_fb_height >= IRIS_TASKBAR_H) {
        dirty_rect_union((int32_t)g_fb_width - 40, (int32_t)g_fb_height - IRIS_TASKBAR_H, 40, IRIS_TASKBAR_H);
    }
}

bool taskbar_wake_from_sleep(void) {
    if (!g_is_sleeping) return false;
    g_is_sleeping = false;
    dirty_rect_union(0, 0, g_fb_width, g_fb_height);
    return true;
}

void taskbar_enter_sleep(void) {
    g_is_sleeping = true;
    dirty_rect_union(0, 0, g_fb_width, g_fb_height);
}

/* ==============================================================================
 * Taskbar Lifecycle & Window Sync
 * ============================================================================== */

static void add_catalog_app(const char* id, const char* name, const char* sub,
                            const char* bin, app_icon_kind_t kind, bool pinned) {
    if (g_num_apps >= TASKBAR_APP_MAX) return;
    taskbar_app_t* a = &g_apps[g_num_apps++];
    memset(a, 0, sizeof(*a));
    snprintf(a->id, sizeof(a->id), "%s", id);
    snprintf(a->name, sizeof(a->name), "%s", name);
    snprintf(a->subtitle, sizeof(a->subtitle), "%s", sub);
    snprintf(a->bin_path, sizeof(a->bin_path), "%s", bin);
    a->icon_kind  = kind;
    a->is_catalog = true;
    a->is_pinned  = pinned;
}

void taskbar_init(void) {
    g_num_apps = 0;
    g_num_recent = 0;
    g_num_notifs = 0;

    add_catalog_app("files", "Files", "Browse /Users & /System", "/System/Bin/terminal", APP_ICON_FILES, true);
    add_catalog_app("terminal", "Terminal", "Command Line & Shell", "/System/Bin/terminal", APP_ICON_TERMINAL, true);
    add_catalog_app("sysinfo", "System Info", "irisOS 64-bit Microkernel", "", APP_ICON_SYSINFO, false);

    record_recent_app(1); /* Terminal */

    taskbar_update_clock();
    taskbar_push_notification("AES-256-XTS Storage", "Encrypted /Users & /Applications mounted.");
    taskbar_push_notification("Welcome to irisOS", "codename:alexia desktop session active.");
    g_unread_notifs = false;
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

        g_cur_day  = tm->tm_mday;
        g_cur_mon  = tm->tm_mon;
        g_cur_year = tm->tm_year + 1900;
        g_cur_wday = tm->tm_wday;
    }

    if (g_fb_width >= 100u && g_fb_height >= IRIS_TASKBAR_H) {
        dirty_rect_union((int32_t)g_fb_width - 104, (int32_t)g_fb_height - IRIS_TASKBAR_H, 104, IRIS_TASKBAR_H);
    }
}

void taskbar_sync_windows(void) {
    maybe_load_pinned_apps();

    for (size_t a = 0; a < g_num_apps; ++a) {
        g_apps[a].is_running   = false;
        g_apps[a].is_focused   = false;
        g_apps[a].is_minimized = false;
        g_apps[a].window_id    = 0;
    }

    /* Match open windows to existing entries or register dynamic unpinned entries */
    for (size_t w = 0; w < g_num_windows; ++w) {
        comp_window_t* win = &g_windows[w];
        bool is_top = (w + 1 == g_num_windows);
        size_t matched = TASKBAR_APP_MAX;

        for (size_t a = 0; a < g_num_apps; ++a) {
            if (strcmp(g_apps[a].id, "terminal") == 0 &&
                (strstr(win->title, "toybox") != NULL || strstr(win->title, "terminal") != NULL ||
                 strstr(win->title, "Terminal") != NULL)) {
                if (!g_apps[a].is_running) {
                    matched = a;
                    break;
                }
            } else if (!g_apps[a].is_catalog && strcmp(g_apps[a].name, win->title) == 0 && !g_apps[a].is_running) {
                matched = a;
                break;
            }
        }

        if (matched == TASKBAR_APP_MAX && g_num_apps < TASKBAR_APP_MAX) {
            matched = g_num_apps++;
            taskbar_app_t* dyn = &g_apps[matched];
            memset(dyn, 0, sizeof(*dyn));
            snprintf(dyn->id, sizeof(dyn->id), "win_%u", win->window_id);
            snprintf(dyn->name, sizeof(dyn->name), "%s", win->title[0] ? win->title : "App");
            snprintf(dyn->subtitle, sizeof(dyn->subtitle), "Running Window");
            dyn->icon_kind  = (strstr(win->title, "toybox") != NULL) ? APP_ICON_TERMINAL : APP_ICON_GENERIC;
            dyn->is_catalog = false;
            dyn->is_pinned  = false;
        }

        if (matched < g_num_apps) {
            g_apps[matched].is_running   = true;
            g_apps[matched].window_id    = win->window_id;
            g_apps[matched].is_minimized = win->is_minimized;
            g_apps[matched].is_focused   = is_top && !win->is_minimized;
        }
    }

    if (g_num_windows == 0 && g_welcome_surface != NULL) {
        for (size_t a = 0; a < g_num_apps; ++a) {
            if (strcmp(g_apps[a].id, "terminal") == 0) {
                g_apps[a].is_running   = true;
                g_apps[a].window_id    = 0;
                g_apps[a].is_minimized = g_welcome_win.is_minimized;
                g_apps[a].is_focused   = !g_welcome_win.is_minimized;
                break;
            }
        }
    }

    /* Rebuild visible dock slots: pinned apps first, then running unpinned apps */
    g_num_dock_slots = 0;
    for (size_t a = 0; a < g_num_apps; ++a) {
        if (g_apps[a].is_pinned) {
            g_dock_slots[g_num_dock_slots++] = a;
        }
    }
    for (size_t a = 0; a < g_num_apps; ++a) {
        if (!g_apps[a].is_pinned && g_apps[a].is_running) {
            g_dock_slots[g_num_dock_slots++] = a;
        }
    }
}

static int32_t get_dock_start_x(void) {
    if (g_num_dock_slots == 0) {
        return (int32_t)(g_fb_width / 2u);
    }
    int32_t total_w = (int32_t)((g_num_dock_slots - 1) * TASKBAR_DOCK_STEP + TASKBAR_DOCK_ICON_SZ);
    return (int32_t)(g_fb_width / 2u) - (total_w / 2);
}

/* ==============================================================================
 * Application Launch / Focus Helper
 * ============================================================================== */

static void launch_or_focus_app(size_t app_idx) {
    if (app_idx >= g_num_apps) return;
    taskbar_sync_windows();
    taskbar_app_t* app = &g_apps[app_idx];
    record_recent_app(app_idx);

    if (strcmp(app->id, "sysinfo") == 0) {
        taskbar_push_notification("irisOS System Information", "64-bit Zircon Microkernel • Inter Display + JetBrains Mono");
        g_unread_notifs = true;
        return;
    }

    if (!app->is_running) {
        if (app->bin_path[0] != '\0') {
            char* argv[] = { (char*)app->id, NULL };
            pid_t pid = -1;
            posix_spawn(&pid, app->bin_path, NULL, NULL, argv, environ);
        }
        return;
    }

    comp_window_t* w = find_window(app->window_id, NULL);
    if (w) {
        if (w->is_minimized || !app->is_focused) {
            w->is_minimized = false;
            size_t widx = 0;
            find_window(w->window_id, &widx);
            if (widx + 1 < g_num_windows) {
                comp_window_t picked = g_windows[widx];
                for (size_t j = widx; j + 1 < g_num_windows; ++j) {
                    g_windows[j] = g_windows[j + 1];
                }
                g_windows[g_num_windows - 1] = picked;
            }
        } else {
            w->is_minimized = true;
        }
    } else if (g_welcome_surface != NULL && g_num_windows == 0) {
        g_welcome_win.is_minimized = !g_welcome_win.is_minimized;
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

    /* 1. Translucent gradient taskbar (#FFFFFF 100% -> 85%) + subtle top hairline */
    fill_rect_vgradient_over(dst_pixels, g_fb_width, g_fb_height,
                             0, tb_y, g_fb_width, 1u, 0x60B8C0CCu, 0x60B8C0CCu);
    fill_rect_vgradient_over(dst_pixels, g_fb_width, g_fb_height,
                             0, tb_y + 1, g_fb_width, IRIS_TASKBAR_H - 1u,
                             0xFFFFFFFFu, 0xD9FFFFFFu);

    /* 2. Far Left: irisOS Start Logo */
    if (g_cursor_y >= tb_y && g_cursor_x >= 10 && g_cursor_x <= 66) {
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, 10, tb_y + 5, 55, 36, 8, 0x16141418u);
    }
    render_eye_light_svg(dst_pixels, g_fb_width, g_fb_height, 16, tb_y + 9, 43, 28);

    /* 3. Center: Pinned + Running Application Dock */
    taskbar_sync_windows();
    int32_t dock_x = get_dock_start_x();
    int32_t dock_y = tb_y + 6;

    for (size_t s = 0; s < g_num_dock_slots; ++s) {
        size_t app_idx = g_dock_slots[s];
        const taskbar_app_t* app = &g_apps[app_idx];
        int32_t ix = dock_x + (int32_t)(s * TASKBAR_DOCK_STEP);

        /* Subtle hover highlight */
        if (!g_drag_active && g_cursor_y >= tb_y &&
            g_cursor_x >= ix - 4 && g_cursor_x < ix + TASKBAR_DOCK_ICON_SZ + 4) {
            fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height,
                                   ix - 4, tb_y + 4, TASKBAR_DOCK_ICON_SZ + 8, 34, 8, 0x14141418u);
        }

        draw_app_icon(dst_pixels, dst_surface, ix, dock_y, TASKBAR_DOCK_ICON_SZ, app->icon_kind);

        /* Running / Minimized / Unfocused state indicator */
        if (app->is_running) {
            if (app->is_focused && !app->is_minimized) {
                /* Active running horizontal black pill */
                fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height,
                                       ix + 8, tb_y + 40, 14, 3, 1, 0xFF141418u);
            } else {
                /* Minimized or unfocused compact dark gray / black dot */
                uint32_t dot_col = app->is_minimized ? 0xFF4B5563u : 0xFF1F2937u;
                fill_circle_over(dst_pixels, g_fb_width, g_fb_height,
                                 ix + 15, tb_y + 41, 2, dot_col);
            }
        }
    }

    /* 4. Far Right: Overflow ^, System Status Material Icons, Notification Bell, Clock & Privacy */
    if (g_fb_width >= 520u) {
        int32_t tray_y   = tb_y + 14;
        uint32_t icon_col = 0xFF141418u;

        /* 4a. 3rd-party / overflow tray chevron button */
        int32_t chv_x = (int32_t)g_fb_width - 244;
        fill_circle_over(dst_pixels, g_fb_width, g_fb_height, chv_x + 9, tray_y + 9, 9, 0xFF141418u);
        draw_tray_glyph(dst_pixels, dst_surface, chv_x + 1, tray_y + 1, 16,
                        MAT_ICON_EXPAND_LESS, 0xFFFFFFFFu);

        /* 4b. Volume */
        int32_t vol_x = (int32_t)g_fb_width - 218;
        draw_tray_glyph(dst_pixels, dst_surface, vol_x, tray_y, 18,
                        g_volume_muted ? MAT_ICON_VOLUME_OFF : MAT_ICON_VOLUME_UP, icon_col);

        /* 4c. Bluetooth */
        int32_t bt_x = (int32_t)g_fb_width - 194;
        draw_tray_glyph(dst_pixels, dst_surface, bt_x, tray_y, 18,
                        MAT_ICON_BLUETOOTH, g_bt_enabled ? icon_col : 0xFF9CA3AFu);

        /* 4d. Network */
        int32_t net_x = (int32_t)g_fb_width - 170;
        draw_tray_glyph(dst_pixels, dst_surface, net_x, tray_y, 18,
                        MAT_ICON_DESKTOP_WINDOWS, g_net_online ? icon_col : 0xFFEF4444u);

        /* 4e. Battery / Power */
        int32_t bat_x = (int32_t)g_fb_width - 146;
        draw_tray_glyph(dst_pixels, dst_surface, bat_x, tray_y, 18,
                        g_battery_charge ? MAT_ICON_BATTERY_CHARGING : MAT_ICON_BATTERY_FULL, icon_col);

        /* 4f. Notification Center (separate from Clock & Date) */
        int32_t bel_x = (int32_t)g_fb_width - 122;
        draw_tray_glyph(dst_pixels, dst_surface, bel_x, tray_y, 18,
                        MAT_ICON_NOTIFICATIONS, icon_col);
        if (g_unread_notifs) {
            fill_circle_over(dst_pixels, g_fb_width, g_fb_height, bel_x + 15, tray_y + 3, 3, 0xFF2563EBu);
        }

        /* 4g. Two-line Clock & Date in Inter Display */
        int32_t clk_x = (int32_t)g_fb_width - 96;
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height,
                        clk_x, tb_y + 9, 12, FONT_FAMILY_INTER_DISPLAY,
                        FONT_WEIGHT_BOLD, false, g_time_str, 0xFF141418u);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height,
                        clk_x - 3, tb_y + 24, 11, FONT_FAMILY_INTER_DISPLAY,
                        FONT_WEIGHT_REGULAR, false, g_date_str, 0xFF374151u);

        /* 4h. Always-visible Privacy Indicator dot (Camera / Mic / Location) */
        if (g_privacy_mask != IRIS_PRIVACY_NONE) {
            int32_t prv_cx = (int32_t)g_fb_width - 14;
            int32_t prv_cy = tb_y + 23;
            uint32_t dot_fill = (g_privacy_mask == IRIS_PRIVACY_CAMERA) ? 0xFF22C55Eu : 0xFFF59E0Bu;
            fill_circle_over(dst_pixels, g_fb_width, g_fb_height, prv_cx, prv_cy, 6, 0xFF141418u);
            fill_circle_over(dst_pixels, g_fb_width, g_fb_height, prv_cx, prv_cy, 5, dot_fill);
        }
    }
}

/* ==============================================================================
 * Flyout Overlays & Drag Ghost Rendering
 * ============================================================================== */

bool taskbar_has_active_flyout(void) {
    return g_active_flyout != FLYOUT_NONE;
}

bool taskbar_has_active_overlay(void) {
    return (g_active_flyout != FLYOUT_NONE) || g_drag_active || g_is_sleeping;
}

void taskbar_close_flyout(void) {
    if (g_active_flyout != FLYOUT_NONE) {
        g_active_flyout = FLYOUT_NONE;
        dirty_rect_union(g_flyout_x - 6, g_flyout_y - 6, g_flyout_w + 12, g_flyout_h + 12);
    }
}

static void open_flyout(taskbar_flyout_t type, int32_t x, uint32_t w, uint32_t h) {
    if (g_active_flyout != FLYOUT_NONE) {
        dirty_rect_union(g_flyout_x - 6, g_flyout_y - 6, g_flyout_w + 12, g_flyout_h + 12);
    }
    int32_t tb_top = (int32_t)g_fb_height - IRIS_TASKBAR_H;
    g_active_flyout = type;
    g_flyout_w      = w;
    g_flyout_h      = h;
    g_flyout_x      = x;
    if (g_flyout_x + (int32_t)w > (int32_t)g_fb_width - 8) {
        g_flyout_x = (int32_t)g_fb_width - (int32_t)w - 8;
    }
    if (g_flyout_x < 8) g_flyout_x = 8;
    g_flyout_y = tb_top - (int32_t)h - 8;
    dirty_rect_union(g_flyout_x - 6, g_flyout_y - 6, g_flyout_w + 12, g_flyout_h + 12);
}

static void draw_flyout_card(uint32_t* dst_pixels, int32_t fx, int32_t fy, uint32_t fw, uint32_t fh) {
    fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx - 1, fy - 1, fw + 2, fh + 2, 11, 0xB0C5CCD6u);
    fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx, fy, fw, fh, 10, 0xFAFFFFFFu);
}

static void render_calendar_flyout(uint32_t* dst_pixels, int32_t fx, int32_t fy, uint32_t fw, uint32_t fh) {
    draw_flyout_card(dst_pixels, fx, fy, fw, fh);

    fill_rect_vgradient_over(dst_pixels, g_fb_width, g_fb_height, fx, fy, fw, 48, 0xFFF4F6FAu, 0xFFEAEFF7u);
    draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 10, 15,
                    FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_BOLD, false, g_time_str, 0xFF141418u);
    draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 28, 11,
                    FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, g_date_str, 0xFF4B5563u);

    draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 56, 11,
                    FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false,
                    "Su   Mo   Tu   We   Th   Fr   Sa", 0xFF6B7280u);

    static const int k_mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int days_in_mon = (g_cur_mon >= 0 && g_cur_mon < 12) ? k_mdays[g_cur_mon] : 31;
    if (g_cur_mon == 1 && ((g_cur_year % 4 == 0 && g_cur_year % 100 != 0) || (g_cur_year % 400 == 0))) {
        days_in_mon = 29;
    }
    int first_wday = ((g_cur_wday - (g_cur_day - 1)) % 7 + 7) % 7;

    int row = 0;
    int col = first_wday;
    for (int d = 1; d <= days_in_mon; ++d) {
        int32_t cx = fx + 14 + col * 29;
        int32_t cy = fy + 78 + row * 22;
        if (d == g_cur_day) {
            fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, cx - 3, cy - 2, 22, 18, 6, 0xFF2563EBu);
        }
        char dbuf[8];
        snprintf(dbuf, sizeof(dbuf), "%2d", d);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, cx, cy, 11,
                        FONT_FAMILY_INTER_DISPLAY,
                        (d == g_cur_day) ? FONT_WEIGHT_BOLD : FONT_WEIGHT_REGULAR,
                        false, dbuf, (d == g_cur_day) ? 0xFFFFFFFFu : 0xFF374151u);
        col++;
        if (col >= 7) {
            col = 0;
            row++;
        }
    }
}

void taskbar_render_flyout(uint32_t* dst_pixels, pixman_image_t* dst_surface) {
    if (!dst_pixels || !dst_surface) {
        return;
    }

    if (g_is_sleeping) {
        fill_rect_vgradient_over(dst_pixels, g_fb_width, g_fb_height,
                                 0, 0, g_fb_width, g_fb_height, 0xE80A0C12u, 0xF0050608u);
        const char* wake_msg = "irisOS Sleeping - Click or press any key to wake";
        int32_t tw = measure_text_ft_ex(FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false, 14, wake_msg);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height,
                        ((int32_t)g_fb_width - tw) / 2, (int32_t)(g_fb_height / 2u) - 8, 14,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false, wake_msg, 0xFFD1D5DBu);
        return;
    }

    int32_t fx = g_flyout_x;
    int32_t fy = g_flyout_y;
    uint32_t fw = g_flyout_w;
    uint32_t fh = g_flyout_h;

    if (g_active_flyout == FLYOUT_START_MENU) {
        draw_flyout_card(dst_pixels, fx, fy, fw, fh);

        /* Header: Profile Picture, Name, Switch User & Log Out quick buttons */
        fill_rect_vgradient_over(dst_pixels, g_fb_width, g_fb_height, fx, fy, fw, 54, 0xFFF4F6FAu, 0xFFECEFF4u);
        fill_circle_over(dst_pixels, g_fb_width, g_fb_height, fx + 30, fy + 27, 16, 0xFF2563EBu);
        draw_tray_glyph(dst_pixels, dst_surface, fx + 20, fy + 17, 20, MAT_ICON_PERSON, 0xFFFFFFFFu);

        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 54, fy + 13, 13,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false, login_active_display_name(), 0xFF141418u);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 54, fy + 29, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false,
                        login_active_is_guest() ? "Guest Session • Ephemeral" : "Administrator • irisOS", 0xFF6B7280u);

        /* Quick session icons on header right: Switch User, Log Out */
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx + (int32_t)fw - 70, fy + 13, 28, 28, 6, 0x10141418u);
        draw_tray_glyph(dst_pixels, dst_surface, fx + (int32_t)fw - 65, fy + 18, 18, MAT_ICON_SWITCH_ACCOUNT, 0xFF374151u);
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx + (int32_t)fw - 38, fy + 13, 28, 28, 6, 0x10141418u);
        draw_tray_glyph(dst_pixels, dst_surface, fx + (int32_t)fw - 33, fy + 18, 18, MAT_ICON_LOGOUT, 0xFF374151u);

        /* Applications List */
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 16, fy + 64, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false, "Applications (drag to pin)", 0xFF6B7280u);

        size_t cat_drawn = 0;
        for (size_t i = 0; i < g_num_apps && cat_drawn < 3; ++i) {
            if (!g_apps[i].is_catalog) continue;
            int32_t item_y = fy + 84 + (int32_t)(cat_drawn * 44);
            fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx + 12, item_y, fw - 24, 38, 8, 0xFFF8FAFCu);
            draw_app_icon(dst_pixels, dst_surface, fx + 18, item_y + 4, 30, g_apps[i].icon_kind);
            draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 56, item_y + 7, 13,
                            FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false, g_apps[i].name, 0xFF141418u);
            draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 56, item_y + 22, 11,
                            FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, g_apps[i].subtitle, 0xFF6B7280u);
            if (g_apps[i].is_pinned) {
                draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + (int32_t)fw - 54, item_y + 13, 10,
                                FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false, "Pinned", 0xFF2563EBu);
            }
            cat_drawn++;
        }

        /* Recently Opened Apps Section */
        int32_t rec_hdr_y = fy + 222;
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 16, rec_hdr_y, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false, "Recently Opened", 0xFF6B7280u);
        for (size_t r = 0; r < g_num_recent && r < 2; ++r) {
            ssize_t app_idx_s = find_app_by_id(g_recent_ids[r]);
            if (app_idx_s < 0) continue;
            size_t app_idx = (size_t)app_idx_s;
            int32_t rx = fx + 12 + (int32_t)(r * 150);
            int32_t ry = rec_hdr_y + 20;
            fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, rx, ry, 144, 42, 8, 0xFFF4F6FAu);
            draw_app_icon(dst_pixels, dst_surface, rx + 8, ry + 6, 28, g_apps[app_idx].icon_kind);
            draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, rx + 42, ry + 14, 12,
                            FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false, g_apps[app_idx].name, 0xFF141418u);
        }

        /* Bottom Power & Session Action Bar (Sleep, Restart, Shutdown, Log Out, Switch) */
        int32_t bot_y = fy + (int32_t)fh - 52;
        fill_rect_vgradient_over(dst_pixels, g_fb_width, g_fb_height, fx, bot_y, fw, 52, 0xFFF4F6FAu, 0xFFE9EDF5u);

        static const struct {
            uint32_t    icon;
            const char* label;
            uint32_t    fg;
        } k_pwr_btns[5] = {
            { MAT_ICON_BEDTIME,        "Sleep",    0xFF374151u },
            { MAT_ICON_RESTART,        "Restart",  0xFF374151u },
            { MAT_ICON_POWER,          "Shutdown", 0xFFDC2626u },
            { MAT_ICON_LOGOUT,         "Log Out",  0xFF374151u },
            { MAT_ICON_SWITCH_ACCOUNT, "Switch",   0xFF374151u },
        };
        for (int b = 0; b < 5; ++b) {
            int32_t bx = fx + 10 + b * 60;
            fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, bx, bot_y + 6, 56, 40, 7, 0x0E141418u);
            draw_tray_glyph(dst_pixels, dst_surface, bx + 19, bot_y + 8, 18, k_pwr_btns[b].icon, k_pwr_btns[b].fg);
            int32_t lw = measure_text_ft_ex(FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false, 10, k_pwr_btns[b].label);
            draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, bx + (56 - lw) / 2, bot_y + 29, 10,
                            FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false, k_pwr_btns[b].label, k_pwr_btns[b].fg);
        }
    } else if (g_active_flyout == FLYOUT_VOLUME) {
        draw_flyout_card(dst_pixels, fx, fy, fw, fh);
        char vol_txt[32];
        snprintf(vol_txt, sizeof(vol_txt), g_volume_muted ? "Volume: Muted" : "Volume: %u%%", g_volume_pct);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 12, 12,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false, vol_txt, 0xFF141418u);

        /* Mute toggle badge on top-right */
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx + (int32_t)fw - 58, fy + 8, 46, 20, 6,
                               g_volume_muted ? 0xFFEF4444u : 0xFFE5E7EBu);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + (int32_t)fw - 50, fy + 12, 10,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_BOLD, false,
                        g_volume_muted ? "Muted" : "Mute", g_volume_muted ? 0xFFFFFFFFu : 0xFF374151u);

        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 40, fw - 28, 6, 3, 0xFFE5E7EBu);
        uint32_t bar_w = ((fw - 28) * g_volume_pct) / 100u;
        if (bar_w > 0) {
            fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 40, bar_w, 6, 3,
                                   g_volume_muted ? 0xFF9CA3AFu : 0xFF2563EBu);
        }
        fill_circle_over(dst_pixels, g_fb_width, g_fb_height, fx + 14 + (int32_t)bar_w, fy + 43, 6, 0xFF141418u);
    } else if (g_active_flyout == FLYOUT_BLUETOOTH) {
        draw_flyout_card(dst_pixels, fx, fy, fw, fh);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 12, 13,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false, "Bluetooth", 0xFF141418u);
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx + (int32_t)fw - 52, fy + 10, 38, 20, 10,
                               g_bt_enabled ? 0xFF2563EBu : 0xFFD1D5DBu);
        fill_circle_over(dst_pixels, g_fb_width, g_fb_height,
                         fx + (int32_t)fw - (g_bt_enabled ? 22 : 42), fy + 20, 7, 0xFFFFFFFFu);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 40, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false,
                        g_bt_enabled ? "Adapter ready • No paired devices" : "Bluetooth turned off", 0xFF6B7280u);
    } else if (g_active_flyout == FLYOUT_NETWORK) {
        draw_flyout_card(dst_pixels, fx, fy, fw, fh);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 12, 13,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false, "Network", 0xFF141418u);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 34, 12,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false,
                        g_net_online ? "VirtIO Link: Connected" : "Disconnected",
                        g_net_online ? 0xFF16A34Au : 0xFFDC2626u);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 52, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, "Local loopback & IPC active", 0xFF6B7280u);
    } else if (g_active_flyout == FLYOUT_BATTERY) {
        draw_flyout_card(dst_pixels, fx, fy, fw, fh);
        char btxt[40];
        snprintf(btxt, sizeof(btxt), "Power: %u%% (%s)", g_battery_pct, g_battery_charge ? "AC Charging" : "Battery");
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 12, 13,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false, btxt, 0xFF141418u);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 36, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, "Power profile: Balanced", 0xFF6B7280u);
    } else if (g_active_flyout == FLYOUT_NOTIFICATIONS) {
        draw_flyout_card(dst_pixels, fx, fy, fw, fh);
        fill_rect_vgradient_over(dst_pixels, g_fb_width, g_fb_height, fx, fy, fw, 40, 0xFFF4F6FAu, 0xFFECEFF4u);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 12, 13,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false, "Notification Center", 0xFF141418u);

        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx + (int32_t)fw - 68, fy + 8, 56, 24, 6, 0x12141418u);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + (int32_t)fw - 60, fy + 14, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false, "Clear All", 0xFF374151u);

        if (g_num_notifs == 0) {
            draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 72, fy + 110, 12,
                            FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, "No new notifications", 0xFF9CA3AFu);
        } else {
            for (size_t i = 0; i < g_num_notifs && i < 4; ++i) {
                int32_t ny = fy + 48 + (int32_t)(i * 52);
                fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx + 10, ny, fw - 20, 46, 8, 0xFFF8FAFCu);
                draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 18, ny + 8, 12,
                                FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false, g_notifs[i].title, 0xFF141418u);
                draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + (int32_t)fw - 66, ny + 9, 10,
                                FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, g_notifs[i].time_str, 0xFF9CA3AFu);
                draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 18, ny + 25, 11,
                                FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, g_notifs[i].body, 0xFF6B7280u);
            }
        }
    } else if (g_active_flyout == FLYOUT_TRAY_OVERFLOW) {
        draw_flyout_card(dst_pixels, fx, fy, fw, fh);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 12, fy + 10, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false, "Background & 3rd-Party Apps", 0xFF6B7280u);
        /* 3rd-party / background tray items */
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx + 10, fy + 32, fw - 20, 32, 6, 0xFFF4F6FAu);
        draw_tray_glyph(dst_pixels, dst_surface, fx + 16, fy + 39, 18, MAT_ICON_MIC,
                        (g_privacy_mask & IRIS_PRIVACY_MIC) ? 0xFFF59E0Bu : 0xFF4B5563u);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 42, fy + 41, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false,
                        (g_privacy_mask & IRIS_PRIVACY_MIC) ? "Mic Indicator: Active" : "Mic Indicator: Off", 0xFF141418u);

        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx + 10, fy + 70, fw - 20, 32, 6, 0xFFF4F6FAu);
        draw_tray_glyph(dst_pixels, dst_surface, fx + 16, fy + 77, 18, MAT_ICON_VIDEOCAM,
                        (g_privacy_mask & IRIS_PRIVACY_CAMERA) ? 0xFF22C55Eu : 0xFF4B5563u);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 42, fy + 79, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false,
                        (g_privacy_mask & IRIS_PRIVACY_CAMERA) ? "Camera Indicator: Active" : "Camera Indicator: Off", 0xFF141418u);

        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, fx + 10, fy + 108, fw - 20, 32, 6, 0xFFF4F6FAu);
        draw_tray_glyph(dst_pixels, dst_surface, fx + 16, fy + 115, 18, MAT_ICON_LOCATION_ON,
                        (g_privacy_mask & IRIS_PRIVACY_LOCATION) ? 0xFFF59E0Bu : 0xFF4B5563u);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 42, fy + 117, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false,
                        (g_privacy_mask & IRIS_PRIVACY_LOCATION) ? "Location Indicator: Active" : "Location Indicator: Off", 0xFF141418u);
    } else if (g_active_flyout == FLYOUT_PRIVACY) {
        draw_flyout_card(dst_pixels, fx, fy, fw, fh);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 12, 12,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_SEMIBOLD, false, "Privacy Indicator", 0xFF141418u);
        char pinfo[64];
        snprintf(pinfo, sizeof(pinfo), "In use by: %s", g_privacy_owner);
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 32, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, pinfo, 0xFF4B5563u);
        char sensors[64];
        snprintf(sensors, sizeof(sensors), "Sensors:%s%s%s",
                 (g_privacy_mask & IRIS_PRIVACY_MIC) ? " Mic" : "",
                 (g_privacy_mask & IRIS_PRIVACY_CAMERA) ? " Camera" : "",
                 (g_privacy_mask & IRIS_PRIVACY_LOCATION) ? " Location" : "");
        draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, fx + 14, fy + 48, 11,
                        FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_MEDIUM, false, sensors, 0xFFD97706u);
    } else if (g_active_flyout == FLYOUT_CALENDAR) {
        render_calendar_flyout(dst_pixels, fx, fy, fw, fh);
    }

    /* Render floating Drag-and-Drop Ghost Icon if actively dragging */
    if (g_drag_active && g_drag_app_idx < g_num_apps) {
        int32_t gx = g_drag_cur_x - 15;
        int32_t gy = g_drag_cur_y - 15;
        fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, gx - 4, gy - 4, 38, 38, 9, 0xCCFFFFFFu);
        draw_app_icon(dst_pixels, dst_surface, gx, gy, 30, g_apps[g_drag_app_idx].icon_kind);

        int32_t tb_top = (int32_t)g_fb_height - IRIS_TASKBAR_H;
        const char* badge = NULL;
        uint32_t badge_col = 0xFF2563EBu;
        if (g_drag_mode == DRAG_MODE_DOCK && g_apps[g_drag_app_idx].is_pinned && g_drag_cur_y < tb_top - 24) {
            badge = "Unpin";
            badge_col = 0xFFDC2626u;
        } else if (g_drag_cur_y >= tb_top - 8 && !g_apps[g_drag_app_idx].is_pinned) {
            badge = "Pin";
        }
        if (badge) {
            fill_rounded_rect_over(dst_pixels, g_fb_width, g_fb_height, gx - 4, gy + 32, 38, 16, 5, badge_col);
            int32_t bw = measure_text_ft_ex(FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_BOLD, false, 10, badge);
            draw_text_ft_ex(dst_pixels, g_fb_width, g_fb_height, gx - 4 + (38 - bw) / 2, gy + 34, 10,
                            FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_BOLD, false, badge, 0xFFFFFFFFu);
        }
    }
}

/* ==============================================================================
 * Mouse Press / Drag / Release Handling
 * ============================================================================== */

static void dirty_drag_ghost_at(int32_t cx, int32_t cy) {
    dirty_rect_union(cx - 28, cy - 28, 56, 76);
}

bool taskbar_handle_mouse_down(int32_t mx, int32_t my) {
    if (taskbar_wake_from_sleep()) {
        return true;
    }

    int32_t tb_top = (int32_t)g_fb_height - IRIS_TASKBAR_H;

    /* 1. Active flyout hit-test */
    if (g_active_flyout != FLYOUT_NONE) {
        if (mx >= g_flyout_x && mx < g_flyout_x + (int32_t)g_flyout_w &&
            my >= g_flyout_y && my < g_flyout_y + (int32_t)g_flyout_h) {
            if (g_active_flyout == FLYOUT_START_MENU) {
                /* Quick header actions (Switch User, Log Out) */
                if (my >= g_flyout_y + 13 && my < g_flyout_y + 41) {
                    if ((mx >= g_flyout_x + (int32_t)g_flyout_w - 70 && mx < g_flyout_x + (int32_t)g_flyout_w - 42) ||
                        (mx >= g_flyout_x + (int32_t)g_flyout_w - 38 && mx < g_flyout_x + (int32_t)g_flyout_w - 10)) {
                        taskbar_close_flyout();
                        login_trigger_logout();
                        return true;
                    }
                }

                /* Application rows (click or drag to pin!) */
                size_t cat_idx = 0;
                for (size_t i = 0; i < g_num_apps && cat_idx < 3; ++i) {
                    if (!g_apps[i].is_catalog) continue;
                    int32_t item_y = g_flyout_y + 84 + (int32_t)(cat_idx * 44);
                    if (my >= item_y && my < item_y + 38 &&
                        mx >= g_flyout_x + 12 && mx < g_flyout_x + (int32_t)g_flyout_w - 12) {
                        g_drag_mode    = DRAG_MODE_START_MENU;
                        g_drag_pending = true;
                        g_drag_active  = false;
                        g_drag_app_idx = i;
                        g_drag_start_x = mx;
                        g_drag_start_y = my;
                        g_drag_cur_x   = mx;
                        g_drag_cur_y   = my;
                        return true;
                    }
                    cat_idx++;
                }

                /* Recently Opened items */
                int32_t rec_y = g_flyout_y + 242;
                if (my >= rec_y && my < rec_y + 42) {
                    for (size_t r = 0; r < g_num_recent && r < 2; ++r) {
                        int32_t rx = g_flyout_x + 12 + (int32_t)(r * 150);
                        if (mx >= rx && mx < rx + 144) {
                            ssize_t r_idx = find_app_by_id(g_recent_ids[r]);
                            if (r_idx >= 0) {
                                launch_or_focus_app((size_t)r_idx);
                            }
                            taskbar_close_flyout();
                            dirty_rect_union(0, 0, g_fb_width, g_fb_height);
                            return true;
                        }
                    }
                }

                /* Bottom Power & Session Buttons */
                int32_t bot_y = g_flyout_y + (int32_t)g_flyout_h - 52;
                if (my >= bot_y + 6 && my < bot_y + 46) {
                    for (int b = 0; b < 5; ++b) {
                        int32_t bx = g_flyout_x + 10 + b * 60;
                        if (mx >= bx && mx < bx + 56) {
                            taskbar_close_flyout();
                            if (b == 0) {
                                g_is_sleeping = true;
                                dirty_rect_union(0, 0, g_fb_width, g_fb_height);
                            } else if (b == 1) {
                                char* argv[] = { (char*)"reboot", NULL };
                                pid_t pid = -1;
                                posix_spawn(&pid, "/System/Bin/reboot", NULL, NULL, argv, environ);
                            } else if (b == 2) {
                                char* argv[] = { (char*)"poweroff", NULL };
                                pid_t pid = -1;
                                posix_spawn(&pid, "/System/Bin/poweroff", NULL, NULL, argv, environ);
                            } else if (b == 3 || b == 4) {
                                login_trigger_logout();
                            }
                            return true;
                        }
                    }
                }
            } else if (g_active_flyout == FLYOUT_VOLUME) {
                if (mx >= g_flyout_x + (int32_t)g_flyout_w - 58 && mx <= g_flyout_x + (int32_t)g_flyout_w - 12 &&
                    my >= g_flyout_y + 8 && my <= g_flyout_y + 28) {
                    g_volume_muted = !g_volume_muted;
                    dirty_rect_union(g_flyout_x - 6, g_flyout_y - 6, g_flyout_w + 12, g_flyout_h + 12);
                    dirty_rect_union((int32_t)g_fb_width - 260, tb_top, 260, IRIS_TASKBAR_H);
                    return true;
                }
                if (mx >= g_flyout_x + 14 && mx <= g_flyout_x + (int32_t)g_flyout_w - 14 &&
                    my >= g_flyout_y + 30 && my <= g_flyout_y + 54) {
                    int32_t rel = mx - (g_flyout_x + 14);
                    int32_t total = (int32_t)g_flyout_w - 28;
                    if (total > 0) {
                        int32_t pct = (rel * 100) / total;
                        if (pct < 0) pct = 0;
                        if (pct > 100) pct = 100;
                        g_volume_pct = (uint8_t)pct;
                        g_volume_muted = (pct == 0);
                        dirty_rect_union(g_flyout_x - 6, g_flyout_y - 6, g_flyout_w + 12, g_flyout_h + 12);
                        dirty_rect_union((int32_t)g_fb_width - 260, tb_top, 260, IRIS_TASKBAR_H);
                    }
                    return true;
                }
            } else if (g_active_flyout == FLYOUT_BLUETOOTH) {
                if (mx >= g_flyout_x + (int32_t)g_flyout_w - 56 && my >= g_flyout_y + 8 && my <= g_flyout_y + 32) {
                    g_bt_enabled = !g_bt_enabled;
                    dirty_rect_union(g_flyout_x - 6, g_flyout_y - 6, g_flyout_w + 12, g_flyout_h + 12);
                    dirty_rect_union((int32_t)g_fb_width - 260, tb_top, 260, IRIS_TASKBAR_H);
                }
            } else if (g_active_flyout == FLYOUT_NOTIFICATIONS) {
                if (mx >= g_flyout_x + (int32_t)g_flyout_w - 68 && my >= g_flyout_y + 8 && my <= g_flyout_y + 32) {
                    g_num_notifs = 0;
                    g_unread_notifs = false;
                    dirty_rect_union(g_flyout_x - 6, g_flyout_y - 6, g_flyout_w + 12, g_flyout_h + 12);
                    dirty_rect_union((int32_t)g_fb_width - 260, tb_top, 260, IRIS_TASKBAR_H);
                }
            } else if (g_active_flyout == FLYOUT_TRAY_OVERFLOW) {
                if (my >= g_flyout_y + 32 && my < g_flyout_y + 64) {
                    uint32_t next = g_privacy_mask ^ IRIS_PRIVACY_MIC;
                    taskbar_set_privacy_active(next, "Audio Monitor");
                    dirty_rect_union(g_flyout_x - 6, g_flyout_y - 6, g_flyout_w + 12, g_flyout_h + 12);
                } else if (my >= g_flyout_y + 70 && my < g_flyout_y + 102) {
                    uint32_t next = g_privacy_mask ^ IRIS_PRIVACY_CAMERA;
                    taskbar_set_privacy_active(next, "Camera Daemon");
                    dirty_rect_union(g_flyout_x - 6, g_flyout_y - 6, g_flyout_w + 12, g_flyout_h + 12);
                } else if (my >= g_flyout_y + 108 && my < g_flyout_y + 140) {
                    uint32_t next = g_privacy_mask ^ IRIS_PRIVACY_LOCATION;
                    taskbar_set_privacy_active(next, "Location Service");
                    dirty_rect_union(g_flyout_x - 6, g_flyout_y - 6, g_flyout_w + 12, g_flyout_h + 12);
                }
            }
            return true;
        }

        taskbar_close_flyout();
        if (my < tb_top) {
            return false;
        }
    }

    if (my < tb_top) {
        return false;
    }

    /* 2. Far Left: Start Menu Eye Button */
    if (mx >= 10 && mx <= 66) {
        if (g_active_flyout == FLYOUT_START_MENU) {
            taskbar_close_flyout();
        } else {
            open_flyout(FLYOUT_START_MENU, 12, 320u, 376u);
        }
        return true;
    }

    /* 3. Center: Dock Icons (arm drag-or-click) */
    taskbar_sync_windows();
    int32_t dock_x = get_dock_start_x();
    for (size_t s = 0; s < g_num_dock_slots; ++s) {
        int32_t ix = dock_x + (int32_t)(s * TASKBAR_DOCK_STEP);
        if (mx >= ix - 4 && mx < ix + TASKBAR_DOCK_ICON_SZ + 4) {
            g_drag_mode    = DRAG_MODE_DOCK;
            g_drag_pending = true;
            g_drag_active  = false;
            g_drag_app_idx = g_dock_slots[s];
            g_drag_start_x = mx;
            g_drag_start_y = my;
            g_drag_cur_x   = mx;
            g_drag_cur_y   = my;
            return true;
        }
    }

    /* 4. Far Right: System Tray & Privacy Indicator */
    if (g_fb_width >= 520u) {
        int32_t chv_x = (int32_t)g_fb_width - 244;
        int32_t vol_x = (int32_t)g_fb_width - 218;
        int32_t bt_x  = (int32_t)g_fb_width - 194;
        int32_t net_x = (int32_t)g_fb_width - 170;
        int32_t bat_x = (int32_t)g_fb_width - 146;
        int32_t bel_x = (int32_t)g_fb_width - 122;
        int32_t clk_x = (int32_t)g_fb_width - 96;
        int32_t prv_x = (int32_t)g_fb_width - 24;

        if (mx >= chv_x - 3 && mx < chv_x + 21) {
            if (g_active_flyout == FLYOUT_TRAY_OVERFLOW) taskbar_close_flyout();
            else open_flyout(FLYOUT_TRAY_OVERFLOW, chv_x - 100, 224u, 152u);
            return true;
        }
        if (mx >= vol_x - 3 && mx < vol_x + 21) {
            if (g_active_flyout == FLYOUT_VOLUME) taskbar_close_flyout();
            else open_flyout(FLYOUT_VOLUME, vol_x - 80, 196u, 64u);
            return true;
        }
        if (mx >= bt_x - 3 && mx < bt_x + 21) {
            if (g_active_flyout == FLYOUT_BLUETOOTH) taskbar_close_flyout();
            else open_flyout(FLYOUT_BLUETOOTH, bt_x - 90, 210u, 72u);
            return true;
        }
        if (mx >= net_x - 3 && mx < net_x + 21) {
            if (g_active_flyout == FLYOUT_NETWORK) taskbar_close_flyout();
            else open_flyout(FLYOUT_NETWORK, net_x - 90, 210u, 78u);
            return true;
        }
        if (mx >= bat_x - 3 && mx < bat_x + 21) {
            if (g_active_flyout == FLYOUT_BATTERY) taskbar_close_flyout();
            else open_flyout(FLYOUT_BATTERY, bat_x - 90, 210u, 64u);
            return true;
        }
        if (mx >= bel_x - 3 && mx < bel_x + 21) {
            if (g_active_flyout == FLYOUT_NOTIFICATIONS) {
                taskbar_close_flyout();
            } else {
                g_unread_notifs = false;
                open_flyout(FLYOUT_NOTIFICATIONS, (int32_t)g_fb_width - 292, 280u, 268u);
            }
            return true;
        }
        if (mx >= clk_x - 6 && mx < prv_x) {
            if (g_active_flyout == FLYOUT_CALENDAR) taskbar_close_flyout();
            else open_flyout(FLYOUT_CALENDAR, (int32_t)g_fb_width - 236, 224u, 212u);
            return true;
        }
        if (g_privacy_mask != IRIS_PRIVACY_NONE && mx >= prv_x) {
            if (g_active_flyout == FLYOUT_PRIVACY) taskbar_close_flyout();
            else open_flyout(FLYOUT_PRIVACY, (int32_t)g_fb_width - 208, 200u, 74u);
            return true;
        }
    }

    return true;
}

void taskbar_update_hover(int32_t mx, int32_t my) {
    static int      s_prev_hover_id = -1;
    static int32_t  s_prev_hx       = 0;
    static uint32_t s_prev_hw       = 0;

    int32_t  tb_top = (int32_t)g_fb_height - IRIS_TASKBAR_H;
    int      cur_id = -1;
    int32_t  cur_hx = 0;
    uint32_t cur_hw = 0;

    if (!g_drag_active && my >= tb_top) {
        if (mx >= 10 && mx <= 66) {
            cur_id = 0;
            cur_hx = 8;
            cur_hw = 60u;
        } else {
            int32_t dock_x = get_dock_start_x();
            for (size_t s = 0; s < g_num_dock_slots; ++s) {
                int32_t ix = dock_x + (int32_t)(s * TASKBAR_DOCK_STEP);
                if (mx >= ix - 4 && mx < ix + TASKBAR_DOCK_ICON_SZ + 4) {
                    cur_id = 1 + (int)s;
                    cur_hx = ix - 6;
                    cur_hw = (uint32_t)(TASKBAR_DOCK_ICON_SZ + 12);
                    break;
                }
            }
        }
    }

    if (cur_id != s_prev_hover_id) {
        if (s_prev_hover_id >= 0 && s_prev_hw > 0) {
            dirty_rect_union(s_prev_hx, tb_top, s_prev_hw, IRIS_TASKBAR_H);
        }
        if (cur_id >= 0 && cur_hw > 0) {
            dirty_rect_union(cur_hx, tb_top, cur_hw, IRIS_TASKBAR_H);
        }
        s_prev_hover_id = cur_id;
        s_prev_hx       = cur_hx;
        s_prev_hw       = cur_hw;
    }
}

bool taskbar_handle_mouse_move(int32_t mx, int32_t my) {
    if (g_active_flyout == FLYOUT_VOLUME && !g_drag_pending) {
        if (mx >= g_flyout_x + 14 && mx <= g_flyout_x + (int32_t)g_flyout_w - 14 &&
            my >= g_flyout_y + 24 && my <= g_flyout_y + 60) {
            int32_t rel = mx - (g_flyout_x + 14);
            int32_t total = (int32_t)g_flyout_w - 28;
            if (total > 0) {
                int32_t pct = (rel * 100) / total;
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                if ((uint8_t)pct != g_volume_pct) {
                    int32_t tb_top = (int32_t)g_fb_height - IRIS_TASKBAR_H;
                    g_volume_pct = (uint8_t)pct;
                    g_volume_muted = (pct == 0);
                    dirty_rect_union(g_flyout_x - 6, g_flyout_y - 6, g_flyout_w + 12, g_flyout_h + 12);
                    dirty_rect_union((int32_t)g_fb_width - 260, tb_top, 260, IRIS_TASKBAR_H);
                }
            }
            return true;
        }
    }

    if (!g_drag_pending) {
        return false;
    }

    int32_t dx = mx - g_drag_start_x;
    int32_t dy = my - g_drag_start_y;
    if (!g_drag_active && (dx * dx + dy * dy >= 36)) {
        g_drag_active = true;
    }

    if (g_drag_active) {
        dirty_drag_ghost_at(g_drag_cur_x, g_drag_cur_y);
        g_drag_cur_x = mx;
        g_drag_cur_y = my;
        dirty_drag_ghost_at(g_drag_cur_x, g_drag_cur_y);
        return true;
    }
    return false;
}

bool taskbar_handle_mouse_up(int32_t mx, int32_t my) {
    if (!g_drag_pending) {
        return false;
    }

    bool was_active = g_drag_active;
    taskbar_drag_mode_t mode = g_drag_mode;
    size_t app_idx = g_drag_app_idx;

    g_drag_pending = false;
    g_drag_active  = false;
    g_drag_mode    = DRAG_MODE_NONE;

    if (was_active) {
        dirty_drag_ghost_at(g_drag_cur_x, g_drag_cur_y);
        int32_t tb_top = (int32_t)g_fb_height - IRIS_TASKBAR_H;

        if (mode == DRAG_MODE_START_MENU) {
            if (my >= tb_top - 12 && app_idx < g_num_apps) {
                g_apps[app_idx].is_pinned = true;
                save_pinned_apps();
                taskbar_close_flyout();
            }
        } else if (mode == DRAG_MODE_DOCK && app_idx < g_num_apps) {
            if (my < tb_top - 24) {
                /* Dragged out of taskbar -> unpin (macOS Dock style) */
                g_apps[app_idx].is_pinned = false;
                save_pinned_apps();
            } else if (my >= tb_top - 12) {
                /* Dropped inside taskbar: pin running app & reorder pinned order */
                if (!g_apps[app_idx].is_pinned) {
                    g_apps[app_idx].is_pinned = true;
                }
                int32_t dock_x = get_dock_start_x();
                int32_t rel = (mx - dock_x + (TASKBAR_DOCK_STEP / 2)) / TASKBAR_DOCK_STEP;
                if (rel < 0) rel = 0;
                if (g_num_dock_slots > 0) {
                    if ((size_t)rel >= g_num_dock_slots) rel = (int32_t)g_num_dock_slots - 1;
                    size_t dst_idx = g_dock_slots[(size_t)rel];
                    if (dst_idx != app_idx && dst_idx < g_num_apps) {
                        taskbar_app_t moved = g_apps[app_idx];
                        if (app_idx < dst_idx) {
                            for (size_t k = app_idx; k < dst_idx; ++k) g_apps[k] = g_apps[k + 1];
                        } else {
                            for (size_t k = app_idx; k > dst_idx; --k) g_apps[k] = g_apps[k - 1];
                        }
                        g_apps[dst_idx] = moved;
                    }
                }
                save_pinned_apps();
            }
        }
        dirty_rect_union(0, 0, g_fb_width, g_fb_height);
        return true;
    }

    /* No drag occurred -> handle as click! */
    if (mode == DRAG_MODE_START_MENU || mode == DRAG_MODE_DOCK) {
        launch_or_focus_app(app_idx);
        if (mode == DRAG_MODE_START_MENU) {
            taskbar_close_flyout();
        }
        dirty_rect_union(0, 0, g_fb_width, g_fb_height);
        return true;
    }

    return false;
}

bool taskbar_handle_click(int32_t mx, int32_t my) {
    bool handled = taskbar_handle_mouse_down(mx, my);
    taskbar_handle_mouse_up(mx, my);
    return handled;
}
