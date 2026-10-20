/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/drivers/virtio_input/include/virtio_input.h
 * Purpose: Standalone Ring 3 VirtIO Input / Keyboard Driver Header.
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 driver header.
 */

#ifndef SERVERS_DRIVERS_VIRTIO_INPUT_H_
#define SERVERS_DRIVERS_VIRTIO_INPUT_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zircon/types.h>
#include <iris/pci.h>
#include <servers/drivers/pci/pci.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================================================
 * Linux Input Event Types & Key Codes (VirtIO Input Specification 5.8)
 * ============================================================================== */

#define EV_SYN         0x00u
#define EV_KEY         0x01u
#define EV_REL         0x02u
#define EV_ABS         0x03u
#define EV_MSC         0x04u
#define EV_SW          0x05u
#define EV_LED         0x11u
#define EV_SND         0x12u
#define EV_REP         0x14u
#define EV_FF          0x15u

#define KEY_RESERVED   0u
#define KEY_ESC        1u
#define KEY_1          2u
#define KEY_2          3u
#define KEY_3          4u
#define KEY_4          5u
#define KEY_5          6u
#define KEY_6          7u
#define KEY_7          8u
#define KEY_8          9u
#define KEY_9          10u
#define KEY_0          11u
#define KEY_MINUS      12u
#define KEY_EQUAL      13u
#define KEY_BACKSPACE  14u
#define KEY_TAB        15u
#define KEY_Q          16u
#define KEY_W          17u
#define KEY_E          18u
#define KEY_R          19u
#define KEY_T          20u
#define KEY_Y          21u
#define KEY_U          22u
#define KEY_I          23u
#define KEY_O          24u
#define KEY_P          25u
#define KEY_LEFTBRACE  26u
#define KEY_RIGHTBRACE 27u
#define KEY_ENTER      28u
#define KEY_LEFTCTRL   29u
#define KEY_A          30u
#define KEY_S          31u
#define KEY_D          32u
#define KEY_F          33u
#define KEY_G          34u
#define KEY_H          35u
#define KEY_J          36u
#define KEY_K          37u
#define KEY_L          38u
#define KEY_SEMICOLON  39u
#define KEY_APOSTROPHE 40u
#define KEY_GRAVE      41u
#define KEY_LEFTSHIFT  42u
#define KEY_BACKSLASH  43u
#define KEY_Z          44u
#define KEY_X          45u
#define KEY_C          46u
#define KEY_V          47u
#define KEY_B          48u
#define KEY_N          49u
#define KEY_M          50u
#define KEY_COMMA      51u
#define KEY_DOT        52u
#define KEY_SLASH      53u
#define KEY_RIGHTSHIFT 54u
#define KEY_KPASTERISK 55u
#define KEY_LEFTALT    56u
#define KEY_SPACE      57u
#define KEY_CAPSLOCK   58u
#define KEY_F1         59u
#define KEY_F2         60u
#define KEY_F3         61u
#define KEY_F4         62u
#define KEY_F5         63u
#define KEY_F6         64u
#define KEY_F7         65u
#define KEY_F8         66u
#define KEY_F9         67u
#define KEY_F10        68u
#define KEY_NUMLOCK    69u
#define KEY_SCROLLLOCK 70u
#define KEY_KP7        71u
#define KEY_KP8        72u
#define KEY_KP9        73u
#define KEY_KPMINUS    74u
#define KEY_KP4        75u
#define KEY_KP5        76u
#define KEY_KP6        77u
#define KEY_KPPLUS     78u
#define KEY_KP1        79u
#define KEY_KP2        80u
#define KEY_KP3        81u
#define KEY_KP0        82u
#define KEY_KPDOT      83u
#define KEY_RIGHTCTRL  97u
#define KEY_KPSLASH    98u
#define KEY_KPENTER    96u
#define KEY_RIGHTALT   100u
#define KEY_HOME       102u
#define KEY_UP         103u
#define KEY_PAGEUP     104u
#define KEY_LEFT       105u
#define KEY_RIGHT      106u
#define KEY_END        107u
#define KEY_DOWN       108u
#define KEY_PAGEDOWN   109u
#define KEY_INSERT     110u
#define KEY_DELETE     111u

#pragma pack(push, 1)
/**
 * @brief VirtIO input event packet received over queue 0 (eventq).
 */
typedef struct virtio_input_event {
    uint16_t type;  /**< Event type (EV_KEY, EV_REL, etc.) */
    uint16_t code;  /**< Linux input keycode (KEY_*) */
    uint32_t value; /**< 0 = release, 1 = press, 2 = autorepeat */
} virtio_input_event_t;
#pragma pack(pop)

/**
 * @brief VirtIO Input device driver context.
 */
typedef struct virtio_input_dev {
    virtio_pci_dev_t                   pci_dev;
    volatile virtio_pci_common_cfg_t* common_cfg;
    uintptr_t                          doorbell_vaddr;

    zx_handle_t                        bti;
    zx_handle_t                        vq_pmt;
    zx_handle_t                        dma_pmt;

    zx_vaddr_t                         vq_desc_vaddr;
    zx_vaddr_t                         vq_avail_vaddr;
    zx_vaddr_t                         vq_used_vaddr;
    zx_vaddr_t                         dma_vaddr;

    uint16_t                           queue_size;
    uint16_t                           last_used_idx;

    bool                               shift_down;
    bool                               ctrl_down;
    bool                               alt_down;
    bool                               caps_lock;
    bool                               active;
} virtio_input_dev_t;

/**
 * @brief Discover and initialize a virtio-input-pci device on the PCIe bus.
 *
 * @param ecam_vaddr Mapped virtual address of ECAM space.
 * @param root_resource Root resource handle for BAR mapping and BTI.
 * @param out_dev Pointer to virtio_input_dev_t to initialize.
 * @return ZX_OK on success, ZX_ERR_NOT_FOUND if device absent, or error code.
 */
zx_status_t virtio_input_init(uintptr_t ecam_vaddr, zx_handle_t root_resource, virtio_input_dev_t* out_dev);

/**
 * @brief Poll the virtio-input device for input events and translate them to ASCII.
 *
 * @param dev Pointer to initialized virtio_input_dev_t.
 * @param out_chars Buffer to store translated ASCII characters.
 * @param max_chars Maximum characters to return.
 * @param out_count Actual number of characters translated.
 * @return ZX_OK if polled successfully, or error code.
 */
zx_status_t virtio_input_poll(virtio_input_dev_t* dev, char* out_chars, size_t max_chars, size_t* out_count);

/**
 * @brief Translate Linux input key code to ASCII based on modifier state.
 *
 * @param code Linux input keycode (KEY_*).
 * @param shift True if Shift is active.
 * @param ctrl True if Control is active.
 * @param caps True if CapsLock is active.
 * @return ASCII character (0..255), or -1 if non-printable / unrecognized.
 */
int virtio_input_translate_key(uint16_t code, bool shift, bool ctrl, bool caps);

/**
 * @brief Cleanly reset the virtio-input device and unpin DMA tokens.
 *
 * @param dev Pointer to initialized virtio_input_dev_t.
 */
void virtio_input_teardown(virtio_input_dev_t* dev);

#ifdef __cplusplus
}
#endif

#endif /* SERVERS_DRIVERS_VIRTIO_INPUT_H_ */
