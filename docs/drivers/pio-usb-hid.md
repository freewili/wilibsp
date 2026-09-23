# PIO-USB HID host

The FreeWili 2 DISPLAY CPU can host USB keyboards, mice, HID gamepads, and
XInput controllers through Pico-PIO-USB on GPIO42 (D+) and GPIO43 (D-).
The public C wrapper is `pio_usb_host/fw2_pio_usb_host.h`.

Set `PICO_PIO_USB_PATH` to the bundled `bsp/third_party/Pico-PIO-USB` before
`pico_sdk_init()`. Linking `freewili2_bsp` provides the host stack.

```c
if (!fw2_pio_usb_host_init()) {
    /* Keep servicing recovery or continue without USB input. */
}
fw2_pio_usb_host_set_key_callback(on_key);
while (true) {
    fw2_app_recovery_task();
    fw2_pio_usb_host_task();
}
```

Initialization requests and maintains power zone 8, enables both external
USB-host ports through the DISPLAY I/O expander, configures root port 1 for
GPIO42/43, and starts TinyUSB host polling. Applications must call
`fw2_pio_usb_host_task()` frequently.
Initialization waits up to ten seconds for a live zone-8 status bit. It returns
`false` without enabling port power or starting the controller if that proof
never arrives; callers should continue servicing recovery and either retry or
run without USB input.

The API exposes key events/modifiers, accumulated mouse deltas/buttons, raw HID
controller reports with VID/PID and stable player assignment, normalized XInput
reports, and mounted-device counts.

Constraints:

- PIO-USB reserves its PIO state machines and DMA channel 9.
- The stack is polled; long blocking work delays input reports.
- Diagnostics remain on SEGGER RTT.
- This offline port has not been run on FreeWili 2 hardware. GPIO42/43
  operation, enumeration, controller compatibility, and port-power sequencing
  remain hardware-unverified.
