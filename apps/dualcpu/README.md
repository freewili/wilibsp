# dualcpu

The DISPLAY half of a combined DISPLAY + ESP32 app. The two halves run on
different processors and share nothing but **OneWili peer streams**:
best-effort datagrams (up to 128 bytes) that the main CPU routes between its
OneWili clients (`libs/onewili/wilibsp/include/onewili_stream.h`).

This half:

- pings the ESP32 once a second and times the answers (last, average, min,
  max, lost, late);
- shows the telemetry the ESP32 sends about once a second: uptime, chip
  temperature, free internal RAM and PSRAM, the ESP32's stream drop count,
  how many main-CPU GPIO reports and text events reached the ESP32, the state
  of its RGB LED, and the four strongest Wi-Fi access points of its last
  scan;
- sets the ESP32's RGB LED and starts a Wi-Fi scan from touch buttons and the
  keypad;
- shows its own side of the link: datagrams sent, received and refused,
  `ow_stream_drops()`, the FwGUI link's stream counters, whether the main CPU
  has confirmed the stream link, the telemetry age, and the ESP32 link state
  (waiting for the ESP32, live, or stale after 3 s of silence).

## Message protocol

Every datagram starts with a type byte; multi-byte fields are little-endian.

| Type | Direction | Payload after the type byte |
| --- | --- | --- |
| `0x01` TELEMETRY | ESP32 to DISPLAY, about 1 Hz | `seq u16, uptime_s u32, temp_dC i16` (tenths of a degree, `INT16_MIN` if unknown), `int_free u32, psram_free u32, stream_drops u32, gpio_events u32, text_events u32, led_mode u8, led_r u8, led_g u8, led_b u8, ap_total u8, n u8`, then `n` (at most 4) records of `rssi i8, channel u8, ssid_len u8, ssid[ssid_len]` (at most 12 bytes). 35 bytes before the records, 95 at most. |
| `0x02` SET_LED | DISPLAY to ESP32 | `mode u8` (0 off, 1 solid, 2 rainbow), `r u8, g u8, b u8` |
| `0x03` PING | DISPLAY to ESP32 | `id u32, display_ms u32` |
| `0x04` PONG | ESP32 to DISPLAY, at once | `id u32` (the PING's), `esp_ms u32` |
| `0x05` SCAN_NOW | DISPLAY to ESP32 | none; the results arrive in the next TELEMETRY |

Unknown types are ignored. Datagrams that are too short or inconsistent are
counted as malformed, and a bad TELEMETRY leaves the last good one on screen.
Datagrams from any peer other than the ESP32 are counted and ignored.

## Screen and controls

A title bar, the ESP32's telemetry and access-point list on the left, link,
round-trip and stream counters on the right, and a row of touch buttons:
**RED**, **GREEN**, **BLUE**, **RAINBOW**, **OFF** and **SCAN**. The layout is
drawn once; after that only fields whose text changed are redrawn.

| Key | Action |
| --- | --- |
| red, green, blue | LED solid in that colour |
| yellow | LED rainbow |
| grey | LED off |
| OK or nav centre | Wi-Fi scan |
| HOME held 5 s | leave the app |
| PAGE held 5 s | About |

The PING timer starts once the main CPU has confirmed the stream link and the
ESP32 has sent its first datagram, so a board that is still booting does not
count lost PINGs.

## Bench control over RTT

`fw rtt` shows a `stats:` line every 2 s with every counter as `key=value`,
for a host script to parse. Channel 0 also takes one-line commands; every
reply is one line starting with `=`:

| Command | Reply |
| --- | --- |
| `stats` | the stats line now, as `=stats: ...` |
| `ping` | one PING: `=ping id=N ok` |
| `burst <n>` | `n` PINGs back to back (at most 1000): `=burst n=... accepted=... refused=...`. Once the stream link's credit window (768 bytes, each PING counted as 9 bytes + 10 of framing = 19, so 40 PINGs) is full, writes are refused until the main CPU's next credit. |
| `led <mode> <r> <g> <b>` | one SET_LED; `mode` is 0, 1, 2 or `off`, `solid`, `rainbow` |
| `scan` | one SCAN_NOW |
| `espmode [0\|1]` | set the main CPU's ESP32 Mode again (blocks for one command round trip, up to 5 s) |
| `help` | the command list |

## Power

The app declares `POWER_ZONES DISPLAY RGB_LEDS`. Zone 10 (`RGB_LEDS`) is also
the 5 V rail of the ESP32's LED, which `picpwr_release_unused()` would
otherwise switch off. The ESP32-C5 itself is zone 5, which has no
`POWER_ZONES` name, so `main.c` keeps it with
`picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_WIFI_BT))`. The stock DISPLAY
firmware keeps zone 5 on for the main CPU; a BSP app replaces that firmware,
so it has to. The app also reports the live rails to the main CPU with
`ow_fwgui_send_power_zones()`.

## Running both halves

1. **Main CPU.** It needs a FreeWili 2 firmware build with OneWili peer
   streams: firmware branch `feat/esp32-onewili-streams` (based on
   `release/v08-preview.3`), which adds the stream router, the `h\a\w`,
   `h\a\p` and `h\a\c` System commands (stable ids 625-627) and
   **Wireless > ESP32 Mode** (`w\e`, id 624). This app calls them by their
   text paths, so it does not depend on the numeric ids (the WASM bindings
   do). On an older firmware the stream link is never confirmed ("main link
   NO CREDIT YET") and every write is refused.
2. **ESP32-C5.** Build the ESP32 (Bottlenose) firmware in
   `freewilibottlenose/` of the same firmware branch, with its dual-CPU app
   selected: menuconfig **FreeWili Bottlenose > BSP app on the ESP32 >
   Dual-CPU app, ESP32 half**, which is `CONFIG_BNOSE_BSP_APP_DUALCPU=y`.
   The committed overlay `sdkconfig.bsp-dualcpu` keeps it out of the
   defaults (ESP-IDF v6.0.1):

   ```sh
   idf.py -B build.esp32c5-dualcpu -D IDF_TARGET=esp32c5 \
          -D SDKCONFIG=build.esp32c5-dualcpu/sdkconfig \
          -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bsp-dualcpu" build
   ```

   Flash it over the ESP32-C5's own USB Serial/JTAG port, which also carries
   its log (a `dualcpu:` summary line every 5 s).
3. **ESP32 Mode.** The main CPU answers the ESP32's OneWili traffic only when
   **Wireless > ESP32 Mode** is **OneWili API**. This app sets it once at
   launch with `ow_wireless_e_sp32_mode(&dev, 1)` and shows the result
   ("esp32 mode: OneWili API set", or FAILED with the status). The setting is
   saved on the main CPU. In the other mode the main CPU neither answers the
   ESP32 nor routes its datagrams, so this app stays at "waiting for esp32".
   A main-CPU firmware without the setting reports FAILED; it has no peer
   streams either.
4. **DISPLAY.** From the repository root:

   ```sh
   tools/fw build dualcpu
   tools/fw ramrun dualcpu     # SRAM app: fw flash cannot start it
   tools/fw rtt                # stats lines; type commands here
   ```

   Or install it non-destructively with
   `tools/fw install-app build/apps/dualcpu/dualcpu.uf2` and launch it with
   `tools/fw run-app dualcpu.uf2`.

Expect "waiting for esp32" until the ESP32 app has booted and its link is
confirmed, then "esp32 LIVE", telemetry filling the left column, and PING
round trips on the right. Touch SCAN (or press OK) and the access-point list
fills with the next telemetry.

## Status

Verified on hardware on 2026-09-27 with both halves running together: link
bring-up, ping/pong (about 6 ms round trip), telemetry, LED and scan control,
the credit window under a 150-ping burst, and the ESP32 Mode gate (also
closed and reopened while the ESP32 kept streaming). Retested the same day on
images rebuilt with the final review fixes. Record and screenshot:
`docs/superpowers/findings/2026-09-27-dualcpu-peer-streams-e2e.md`.

That run used the pre-release development tree, where the ESP32 Mode and
stream commands had ids 614-617. On `feat/esp32-onewili-streams` they were
renumbered to 624-627 after ISO-TP (614-623); the code is otherwise the
same. Rerun on the rebased images on 2026-09-28 with the same results:
`docs/superpowers/findings/2026-09-28-dualcpu-renumbered-ids-rerun.md`.
