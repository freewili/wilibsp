# 125 kHz RFID (`bsp/rfid/`)

Read EM4100 tags and write T5577 tags over the 125 kHz RFID front end.

## Quick start — read a tag

```c
#include "fw2.h"
#include <stdio.h>

int main(void) {
    board_init();
    fw2_app_recovery_init();
    st7796_init();
    board_backlight_set(1);
    picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_NFC_RFID));

    if (!rfid_reader_start()) {
        st7796_draw_text(8, 8, 2, 0xF800, 0x0000, "RFID START FAILED");
        for (;;) { fw2_app_recovery_task(); picpwr_task(); }
    }

    st7796_draw_text(8, 8, 2, 0xFFFF, 0x0000, "PRESENT A TAG...");

    for (;;) {
        rfid_reader_service();

        rfid_tag_t tag;
        if (rfid_reader_poll(&tag)) {
            char buf[40];
            int n = 0;
            for (unsigned i = 0; i < tag.id_len; i++)
                n += snprintf(buf + n, sizeof(buf) - n, "%02X", tag.id[i]);
            st7796_fill_rect(8, 40, 300, 20, 0x0000);
            st7796_draw_text(8, 40, 2, 0x07E0, 0x0000, buf);
        }

        fw2_app_recovery_task();
        picpwr_task();
    }
}
```

## CMakeLists.txt for an RFID app

```cmake
add_executable(my_rfid_app main.c)
target_link_libraries(my_rfid_app freewili2_bsp)
fw2_display_app(my_rfid_app
    POWER_ZONES DISPLAY NFC_RFID
    VERSION 001
    DESCRIPTION "My RFID app")
```

## Clone an EM4100 tag onto a T5577

```c
#include "rfid/rfid_carrier.h"
#include "rfid/rfid_t5577.h"
#include "rfid/rfid_em4100.h"

// Carrier must be running. If you already called rfid_reader_start(),
// it is — skip rfid_carrier_start().
rfid_carrier_start();

// The 5-byte EM4100 ID to write.
uint8_t id[5] = {0x12, 0x34, 0x56, 0x78, 0x9A};

// Encode into the 64-bit EM4100 frame (one bit per byte).
uint8_t bits[64];
rfid_em4100_encode(id, bits);

// Pack into two 32-bit data words.
uint32_t blk1 = 0, blk2 = 0;
for (int i = 0; i < 32; i++) blk1 |= (uint32_t)bits[i] << (31 - i);
for (int i = 0; i < 32; i++) blk2 |= (uint32_t)bits[32 + i] << (31 - i);

// Write config (block 0), then data (blocks 1–2).
uint32_t config = rfid_em4100_t5577_config();
rfid_t5577_write_block(0, 0, config, RFID_T5577_CONFIRM_TOKEN);
rfid_t5577_write_block(0, 1, blk1, 0);
rfid_t5577_write_block(0, 2, blk2, 0);

// Reset the tag so it starts transmitting.
rfid_t5577_reset();
```

Block 0 (config) and block 7 (password) require `RFID_T5577_CONFIRM_TOKEN`
to prevent accidental writes.

## Writing a custom T5577 config

The T5577 is a programmable tag that can emulate many formats. Build a
config word by OR-ing a modulation, a data rate, and a max-blocks count:

```c
// Example: HID-compatible config (FSK1, RF/50, 3 data blocks)
uint32_t config = RFID_T5577_MOD_FSK1
                | RFID_T5577_RF50
                | RFID_T5577_MAXBLK(3);
rfid_t5577_write_block(0, 0, config, RFID_T5577_CONFIRM_TOKEN);
```

Available modulations: `RFID_T5577_MOD_DIRECT`, `_MANCH`, `_BIPHASE`,
`_PSK1`/`2`/`3`, `_FSK1`/`2`/`_FSK1A`/`_FSK2A`.

Available data rates: `RFID_T5577_RF8`, `_RF16`, `_RF32`, `_RF40`, `_RF50`,
`_RF64`, `_RF100`, `_RF128`.

## API reference

### Reader (`rfid.h`)

| Function | Description |
|----------|-------------|
| `bool rfid_reader_start(void)` | Start carrier and capture. Returns false on hardware fault. |
| `void rfid_reader_stop(void)` | Stop carrier and capture, release resources. |
| `void rfid_reader_service(void)` | Process captured samples. Call from your main loop. |
| `bool rfid_reader_poll(rfid_tag_t *out)` | Get the next decoded tag. Returns true if a tag was read. |
| `int rfid_reader_state(void)` | Current state: 0=idle, 1=starting, 2=reading, 3=faulted. |
| `bool rfid_reader_calibrated(void)` | True once the reader has locked onto the signal. |
| `unsigned rfid_reader_carrier_hz(void)` | Actual carrier frequency in Hz. |

### EM4100 (`rfid_em4100.h`)

| Function | Description |
|----------|-------------|
| `bool rfid_em4100_decode(bits, count, id_out)` | Decode an EM4100 bitstream into a 5-byte ID. |
| `bool rfid_em4100_encode(id, bits_out)` | Encode a 5-byte ID into a 64-bit EM4100 frame. |
| `uint32_t rfid_em4100_t5577_config(void)` | T5577 config word for EM4100-compatible operation. |

### T5577 writer (`rfid_t5577.h`)

| Function | Description |
|----------|-------------|
| `rfid_t5577_write_block(page, block, data, confirm)` | Write a data block. |
| `rfid_t5577_write_block_pwd(page, block, data, pwd, confirm)` | Write with password. |
| `rfid_t5577_read_block(page, block)` | Send a read command. |
| `rfid_t5577_read_block_pwd(page, block, pwd)` | Read with password. |
| `rfid_t5577_reset(void)` | Reset the tag to re-read its config. |

All T5577 functions return `RFID_T5577_OK` (0) on success, or an error code.
The carrier must be running before calling any T5577 function.

### Carrier (`rfid_carrier.h`)

| Function | Description |
|----------|-------------|
| `void rfid_carrier_start(void)` | Start the 125 kHz carrier. |
| `void rfid_carrier_stop(void)` | Stop the carrier. |
| `void rfid_carrier_gate(bool on)` | Gate carrier on/off (used internally by T5577 writer). |
| `bool rfid_carrier_running(void)` | True if carrier is active. |
| `unsigned rfid_carrier_hz(void)` | Actual carrier frequency. |

### Capture (`rfid_capture.h`)

| Function | Description |
|----------|-------------|
| `bool rfid_capture_start(void)` | Start ADC capture into a ring buffer. |
| `void rfid_capture_stop(void)` | Stop capture and release DMA. |
| `bool rfid_capture_active(void)` | True if capture is running. |
| `const uint16_t *rfid_capture_ring(void)` | Pointer to the sample ring buffer. |
| `uint32_t rfid_capture_write_pos(void)` | Current DMA write position in the ring. |

## Notes

- Hold the `NFC_RFID` power zone awake before starting the reader.
- Call `rfid_reader_service()` frequently from your main loop — it processes buffered samples.
- `rfid_reader_poll()` returns one tag at a time; the pending tag is cleared after each successful poll.
- The reader transitions through IDLE → STARTING → READING. Calibration takes roughly 100–200 ms.
- Call `rfid_reader_stop()` when done to release hardware resources.
- This driver cannot run in the same app as the WILiRFID library
  (`bsp/wilirfid`): both use the same RFID board and pins.
- For custom signal processing, use `rfid_capture_ring()` and `rfid_capture_write_pos()` to access raw samples directly.
