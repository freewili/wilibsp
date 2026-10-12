# LED driver (`bsp/leds/`) — WS2812 x16

**What it does:** drives the board's **16** WS2812 RGB LEDs (single data
line, `PIN_LED_DATA` = GPIO 21) via a `pio1` state machine
(`bsp/leds/ws2812.pio`, header-generated into the build tree by
`pico_generate_pio_header` in `bsp/CMakeLists.txt`). Output is inverted at
the driver-chip level, handled inside `ws2812_driver.c`.
`bsp/leds/led_color.{c,h}` provides pure `rgb_t` pack/scale helpers.
**16 is the verified count** (`WS2812_NUM_PIXELS`).

`bsp/leds/led_ui.{c,h}` layers a higher-level "signal indicator" API
(breathing fade, spectrum-to-LED mapping) on top, backed by
`bsp/gfx/palette.h`'s `inferno_rgb565()`. The `.c` implementation
(`bsp/gfx/palette.c`) is harvested and wired into `bsp/CMakeLists.txt`, so
`led_ui_spectrum()` / `led_spectrum_map()` are usable — the current v1 apps
still avoid it and use the lower-level `ws2812_*` API directly.

**How to use it (low-level `ws2812_*` API, as used by `hello_display`):**

```c
#include "fw2.h"
#include "hardware/pio.h"

int main(void) {
    board_init();
    ...
    ws2812_init(pio1, 0, PIN_LED_DATA);
    ws2812_set_brightness(64);
    rgb_t green = { .r = 0, .g = 255, .b = 0 };
    ws2812_fill(green);
    ws2812_show();

    for (;;) {
        // ... app work ...
        ws2812_show();   // refresh periodically — see the note below
    }
}
```

**Important — refresh the LEDs, don't `show()` once and stop.** The **first**
`ws2812_show()` data frame after the PIO state machine starts does **not**
reliably latch all pixels — typically only pixel 0 lights, the rest stay dark.
A subsequent `ws2812_show()` latches the whole strip. This is a known WS2812
behavior on this board (verified on hardware 2026-07-01), **not** a driver bug —
the driver is byte-for-byte the proven `sensorview`/`wilidispval` implementation.
Real apps redraw their LED state every frame, so they never notice it. If your
app sets the LEDs once and then does other work, call `ws2812_show()` again
periodically (e.g. every ~250 ms in the main loop). `apps/hello_display/main.c`
does exactly this. See `docs/hardware/facts.md` → "WS2812 first-frame latch".

**Key APIs** (`bsp/leds/ws2812_driver.h`): `ws2812_init(PIO pio, uint sm,
uint gpio)`, `ws2812_set_pixel(uint i, rgb_t c)`, `ws2812_fill(rgb_t c)`,
`ws2812_clear(void)`, `ws2812_set_brightness(uint8_t level)` /
`ws2812_get_brightness(void)`, `ws2812_show(void)`. `WS2812_NUM_PIXELS` /
`FW2_LED_COUNT` = 16.

**Dependencies:** Pico SDK `hardware_pio`; no I2C/board dependency beyond
`board_init()` having run (for clocks). LEDs and PDM microphones share `pio1` in
this BSP — don't reuse it for another PIO program without checking state
machine availability.

`board_init()` clears the physical strip before handing control to the app, so
a RAM app does not inherit another app's frozen LED colors. This startup clear
transmits twice for the first-frame latch behavior, then releases its temporary
state machine and PIO instructions. It neither enables the RGB power zone nor
reserves `pio1`; microphone capture and an app's normal `ws2812_init()` can
claim resources afterward.
