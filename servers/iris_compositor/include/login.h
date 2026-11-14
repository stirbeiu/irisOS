#ifndef IRIS_COMPOSITOR_LOGIN_H
#define IRIS_COMPOSITOR_LOGIN_H

#include <stdbool.h>
#include <stdint.h>
#include <pixman.h>
#include <zircon/types.h>

#ifdef __cplusplus
extern "C" {
#endif

void login_init(zx_handle_t fs_ch, zx_handle_t sys_ctrl_ch);
void login_set_fs_channel(zx_handle_t fs_ch);
void login_show(void);
bool login_is_active(void);

const char* login_active_username(void);
const char* login_active_display_name(void);
bool        login_active_is_guest(void);

void login_render(uint32_t* dst_pixels, pixman_image_t* dst_surface,
                  uint32_t fb_w, uint32_t fb_h);
bool login_handle_click(int32_t mx, int32_t my, uint32_t fb_w, uint32_t fb_h);
void login_handle_key(char ch);
void login_trigger_logout(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_COMPOSITOR_LOGIN_H */
