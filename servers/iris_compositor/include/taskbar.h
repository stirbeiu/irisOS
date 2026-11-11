/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_compositor/include/taskbar.h
 * Purpose: Windows-like bottom taskbar subsystem declarations:
 *          Dynamic RTC clock/date, Material Icons system tray,
 *          centered application dock, and Start Menu/tray flyouts.
 *
 * Licensing: MIT License. Permissive Zone 1 system server module.
 */

#ifndef IRIS_COMPOSITOR_TASKBAR_H
#define IRIS_COMPOSITOR_TASKBAR_H

#include "compositor.h"

typedef enum {
    FLYOUT_NONE = 0,
    FLYOUT_START_MENU,
    FLYOUT_CALENDAR,
    FLYOUT_VOLUME
} taskbar_flyout_t;

/**
 * @brief Initialize taskbar subsystem state, registered apps, and RTC clock.
 */
void taskbar_init(void);

/**
 * @brief Check wall-clock time and dirty the clock bounds if the minute changed.
 */
void taskbar_update_clock(void);

/**
 * @brief Synchronize active window IDs, minimization, and focus states with the dock.
 */
void taskbar_sync_windows(void);

/**
 * @brief Render the bottom 46px taskbar into the destination surface.
 */
void taskbar_render(uint32_t* dst_pixels, pixman_image_t* dst_surface);

/**
 * @brief Handle a mouse click event occurring on the taskbar or active flyout.
 * @return true if the event was handled by the taskbar subsystem, false otherwise.
 */
bool taskbar_handle_click(int32_t mx, int32_t my);

/**
 * @brief Query if a flyout popup (Start Menu, Calendar, Volume) is currently visible.
 */
bool taskbar_has_active_flyout(void);

/**
 * @brief Dismiss any active flyout popup.
 */
void taskbar_close_flyout(void);

/**
 * @brief Render the active flyout overlay above the desktop and taskbar.
 */
void taskbar_render_flyout(uint32_t* dst_pixels, pixman_image_t* dst_surface);

#endif /* IRIS_COMPOSITOR_TASKBAR_H */
