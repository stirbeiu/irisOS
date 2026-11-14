/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_compositor/include/taskbar.h
 * Purpose: Windows-like bottom taskbar subsystem declarations:
 *          Dynamic RTC clock/date, Material Icons system tray,
 *          pinned/running application dock with drag-and-drop pinning,
 *          Start Menu, Notification Center, Tray Overflow, and Privacy Indicator.
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
    FLYOUT_VOLUME,
    FLYOUT_BLUETOOTH,
    FLYOUT_NETWORK,
    FLYOUT_BATTERY,
    FLYOUT_NOTIFICATIONS,
    FLYOUT_TRAY_OVERFLOW,
    FLYOUT_PRIVACY
} taskbar_flyout_t;

#define IRIS_PRIVACY_NONE     0x00u
#define IRIS_PRIVACY_MIC      0x01u
#define IRIS_PRIVACY_CAMERA   0x02u
#define IRIS_PRIVACY_LOCATION 0x04u

/**
 * @brief Initialize taskbar subsystem state, registered apps, pins, and RTC clock.
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
 * @brief Render the bottom 46px translucent gradient taskbar into the destination surface.
 */
void taskbar_render(uint32_t* dst_pixels, pixman_image_t* dst_surface);

/**
 * @brief Handle mouse button press on taskbar, dock icons, tray, or active flyout.
 * @return true if the event was captured by the taskbar subsystem, false otherwise.
 */
bool taskbar_handle_mouse_down(int32_t mx, int32_t my);

/**
 * @brief Handle pointer movement for dock/Start-Menu drag-and-drop.
 * @return true if taskbar state consumed/updated pointer movement.
 */
bool taskbar_handle_mouse_move(int32_t mx, int32_t my);

/**
 * @brief Update hover highlight state for Start button and dock icons, dirtying only changed button rects.
 */
void taskbar_update_hover(int32_t mx, int32_t my);

/**
 * @brief Handle mouse button release for click activation or drag-and-drop pin/unpin.
 * @return true if the event was handled by the taskbar subsystem, false otherwise.
 */
bool taskbar_handle_mouse_up(int32_t mx, int32_t my);

/**
 * @brief Legacy single-click helper (calls down/up).
 */
bool taskbar_handle_click(int32_t mx, int32_t my);

/**
 * @brief Query if a flyout popup is currently visible.
 */
bool taskbar_has_active_flyout(void);

/**
 * @brief Query if a flyout, drag-and-drop ghost, or sleep screen overlay is active.
 */
bool taskbar_has_active_overlay(void);

/**
 * @brief Dismiss any active flyout popup.
 */
void taskbar_close_flyout(void);

/**
 * @brief Render the active flyout, drag ghost, or sleep overlay above the desktop and taskbar.
 */
void taskbar_render_flyout(uint32_t* dst_pixels, pixman_image_t* dst_surface);

/**
 * @brief Update the active privacy sensor indicator bitmask (mic/camera/location).
 */
void taskbar_set_privacy_active(uint32_t mask, const char* owner_app);

/**
 * @brief Append a notification to the Notification Center flyout.
 */
void taskbar_push_notification(const char* title, const char* body);

/**
 * @brief Wake the display if currently in sleep state.
 * @return true if the compositor was sleeping and woke up, false otherwise.
 */
bool taskbar_wake_from_sleep(void);
void taskbar_enter_sleep(void);

#endif /* IRIS_COMPOSITOR_TASKBAR_H */
