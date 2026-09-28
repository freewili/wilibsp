# Keyboard (`bsp/keyboard/`, `bsp/input/uartkbd*`) — fw2kb chord engine + FW2 UART keyboard

**What it does:** two-press chord text entry on the 5 colored FW2 buttons
(gray, yellow, green, blue, red): first press picks a group of 5 characters,
second press picks the character — every a-y letter is exactly two presses.
PAGE cycles pages (upper / lower / numbers / symbols / hex; mid-chord it
cancels the chord). Touch is space (below the app's split line) or
backspace (above). Button state arrives from the FW2 UART keyboard
(button coprocessor) as unsolicited 23-byte frames.

**Pieces:**
- `keyboard/fw2kb.{h,c}`, `keyboard/fw2kb_hidmap.c` — the chord engine,
  harvested verbatim from the owner's `wilikeyboard` repo (poll-based,
  no malloc, no callbacks; host-tested by `tests/test_fw2kb*.c`). The HID
  map translates USB HID usage+modifier pairs — unused on-device today,
  available for a future USB/PC-bridge input source.
- `input/uartkbd_parse.{h,c}` — pure frame parser + press/release edge
  detection (host-tested by `tests/test_uartkbd_parse.c`). Protocol per
  `FreeWilli/vibe/Wilikeyboard.md`: sync 0xBD 0x1D, buttons in bytes 2-5,
  additive 8-bit checksum of bytes 0-21 in byte 22. The first checksum-valid
  frame only primes the button baseline (the coprocessor's boot frames
  carry garbage bits); press/release edges are emitted from the second
  frame on.
- `input/uartkbd.{h,c}` — UART1 binding @ 62500 8N1, GPIO38 (TX, claimed,
  never driven) / GPIO39 (RX), `GPIO_FUNC_UART_AUX` (the plain UART
  function on these pins is CTS/RTS). RX-only, polled.
  RX bytes are drained by a dedicated DMA channel (endless mode, no IRQ)
  into a 1 KB SRAM ring; `uartkbd_task()` reads behind the DMA write
  pointer, so keyboard input survives CPU stalls up to ~164 ms (e.g. long
  blocking renders) without losing frames.

**How to use:**

    #include "fw2.h"

    fw2kb_t kb;
    fw2_app_recovery_init();
    fw2kb_init(&kb);
    fw2kb_set_touch_threshold(&kb, 136);

    for (;;) {
        fw2_app_recovery_task(); /* drain UART and service HOME recovery */
        uartkbd_event_t bev;
        while (uartkbd_next_event(&bev)) {
            if (!bev.pressed) continue;
            if (bev.btn <= UARTKBD_BTN_RED) fw2kb_press(&kb, (fw2kb_btn)bev.btn);
            else if (bev.btn == UARTKBD_BTN_PAGE) fw2kb_press(&kb, FW2KB_BTN_AI);
        }
        fw2kb_event kev;
        while (fw2kb_next_event(&kb, &kev)) { /* CHAR/BACKSPACE/... */ }
        const char *labels[5];
        fw2kb_get_labels(&kb, labels);          /* draw on the soft buttons */
    }

`apps/hello_keyboard` is the worked example (soft-button bar + text area +
touch zones). Draw each label in a box directly above its physical button:
five boxes at `x = i * 96`, width 93, in the button's colour (see AGENTS.md,
"The five front-panel buttons"). Link health: `uartkbd_frames()` / `uartkbd_errors()` — a
healthy link shows frames climbing and errors static.

**Charger telemetry:** the same status frame carries charger data in bytes
10-21. The parser captures the raw bytes on every checksum-valid frame
(readable from the first frame — no priming wait); scaling to engineering
units runs only when you ask:

    uartkbd_charger_t chg;
    if (uartkbd_charger(&chg)) {
        /* chg.vbus_mv / vsys_mv / vbatt_mv / current_ma / temp_tspct
           (tenths of %), chg.charge_status (UARTKBD_CHG_*), chg.vbus_status
           (UARTKBD_VBUS_*), chg.fault (UARTKBD_FAULT_*), chg.temp_rank
           (UARTKBD_RANK_*), chg.cc_tier (UARTKBD_CC_*),
           chg.vsys_regulation / thermal_regulation / vbus_attached,
           chg.cc1_mv / cc2_mv */
    }
    float c = uartkbd_charger_temp_c(chg.temp_tspct);  /* NTC math; links libm;
                                                          <= -100 = no reading */

Returns false until the first valid frame. Enum-coded fields carry the
frame's raw code verbatim, so undocumented codes pass through unclamped.
`apps/hello_charger` is the worked example (live full-frame view).

**Display note:** `font5x7`/`st7796_draw_text` cover full printable ASCII
0x20-0x7E (lowercase included) as of the fw2kb work — the earlier
uppercase-folding caveat no longer applies. The RTT `fw2kb char` log remains
the authoritative record of generated characters.

**Protocol facts (hardware-verified 2026-07-23 against the final keyboard
firmware):** checksum = additive 8-bit sum of bytes 0-21 (confirmed —
hundreds of frames, 0 errors); button bits are **active-low** — a button's
wire bit reads 1 idle, 0 while held (`decode_buttons` inverts, so the
public bitmap/event API keeps bit set = pressed). Still unverified:
AUDIO/HOTPLUG/USB detect flag polarity (assumed level, 1 = detected). All
of this lives in `uartkbd_parse.c` only.

**Dependencies:** none beyond `pico_stdlib` (`hardware_uart` comes with it).
UART1 must stay free for this driver (UART0 is the OneWili/FwGUI link).
