#ifndef FW2_PIO_USB_HOST_H
#define FW2_PIO_USB_HOST_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*fw2_usb_key_cb)(uint8_t keycode, char ascii, bool pressed, uint8_t modifiers);
typedef void (*fw2_usb_controller_cb)(uint8_t dev_addr, uint8_t instance,
                                      const uint8_t *report, uint16_t len,
                                      uint16_t vid, uint16_t pid, int player);
typedef struct {
    uint16_t buttons;
    int16_t lx, ly, rx, ry;
    uint8_t left_trigger, right_trigger;
} fw2_usb_xinput_pad_t;
typedef void (*fw2_usb_xinput_cb)(const fw2_usb_xinput_pad_t *pad, int player);

bool fw2_pio_usb_host_init(void);
void fw2_pio_usb_host_task(void);
void fw2_pio_usb_host_set_key_callback(fw2_usb_key_cb cb);
void fw2_pio_usb_host_set_controller_callback(fw2_usb_controller_cb cb);
void fw2_pio_usb_host_set_xinput_callback(fw2_usb_xinput_cb cb);
uint8_t fw2_pio_usb_host_modifiers(void);
bool fw2_pio_usb_host_mouse(int32_t *dx, int32_t *dy, int32_t *wheel, uint8_t *buttons);
unsigned fw2_pio_usb_host_keyboard_count(void);
unsigned fw2_pio_usb_host_mouse_count(void);
unsigned fw2_pio_usb_host_controller_count(void);
unsigned fw2_pio_usb_host_xinput_count(void);
#ifdef __cplusplus
}
#endif
#endif
