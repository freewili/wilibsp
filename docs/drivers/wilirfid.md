# 125 kHz RFID library (`bsp/wilirfid/`)

## What it does

WILiRFID is the full 125 kHz RFID driver for the FreeWili 2 board. It ships
precompiled: the header `bsp/wilirfid/WILiRFID.h` and the archive
`bsp/wilirfid/libWILiRFID.a`, already linked into `freewili2_bsp`. The header
is the complete API reference; this page shows how to use it.

With it an app can:

- read the common 125 kHz card formats (see the [formats table](#formats)),
  report each card once as it arrives, and then follow whether it is still
  there;
- write or clone a card onto a blank (a writable T5577 card or fob) and check
  the result by reading it back;
- read and write the eight blocks of a T5577 directly;
- make the board present itself to an external reader, such as a door reader,
  as a card (emulation);
- turn a card into one line of text and back, in the same format the device
  itself uses for saved cards, so cards saved on the device load in apps and
  the other way round.

Every call is plain C and runs from the main loop. The usage pattern is three
calls: start once, service on every pass of the loop, poll for new cards.

`apps/hello_wilirfid` is a complete example app: read, clone, emulate, save
and load. Start from it when in doubt.

Terms used on this page: "card" is a 125 kHz card or fob in the user's hand;
"blank" is a writable T5577 card or fob; "the reader" is this library running
on the board; "an external reader" is some other reader, such as a door
reader.

## Quick start: read a card

```c
#include "fw2.h"            // pulls in wilirfid/WILiRFID.h
#include "pico/stdlib.h"
#include <stdio.h>

// The LCD takes colours byte-swapped; write them in ordinary RGB565.
static inline uint16_t be16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

int main(void) {
    board_init();
    // Keeps every zone listed in POWER_ZONES (NFC_RFID among them) awake and
    // waits until they are on, so no power call of our own is needed here.
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd();
    st7796_fill_screen(be16(0x0000));
    board_backlight_set(1);

    if (!WILiRFID_start()) {
        char msg[48];
        snprintf(msg, sizeof msg, "RFID START FAILED: %s",
                 WILiRFID_result_name(WILiRFID_last_error()));
        st7796_draw_text(8, 8, 2, be16(0xF800), be16(0x0000), msg);
        for (;;) { fw2_app_recovery_task(); picpwr_task(); }
    }

    st7796_draw_text(8, 8, 2, be16(0xFFFF), be16(0x0000), "LAY A CARD ON THE BACK...");

    for (;;) {
        fw2_app_recovery_task();
        picpwr_task();
        WILiRFID_service();             // all reader work happens in here

        WILiRFID_tag_t tag;
        if (WILiRFID_poll(&tag)) {      // true once per new card
            char buf[48];
            if (tag.has_facility_card) {
                snprintf(buf, sizeof buf, "%s %lu:%lu", tag.format_name,
                         (unsigned long)tag.facility,
                         (unsigned long)tag.card_number);
            } else {
                int n = snprintf(buf, sizeof buf, "%s ", tag.format_name);
                for (unsigned i = 0; i < tag.id_len && n < (int)sizeof buf - 2; i++)
                    n += snprintf(buf + n, sizeof buf - n, "%02X", tag.id[i]);
            }
            st7796_fill_rect(8, 40, 464, 20, be16(0x0000));
            st7796_draw_text(8, 40, 2, be16(0x07E0), be16(0x0000), buf);
        }
    }
}
```

The antenna is on the back of the FreeWili: lay the card flat against the
back, over the antenna (see [Presenting a card](#presenting-a-card)).

`fw2_app_recovery_init()` keeps the zones declared in `POWER_ZONES` awake and
waits for them before it returns, so an app that lists `NFC_RFID` needs neither
a `picpwr_keep_awake()` call nor a wait loop of its own. It keeps waiting until
every declared zone is on, so if one never comes on the screen stays dark (HOME
still leaves the app).

If `WILiRFID_state()` is not `WILiRFID_STATE_READY` within about two seconds
of start, check that the RFID board is seated and powered: call
`WILiRFID_stop()`, fix it, and start again.

## CMakeLists.txt for an RFID app

```cmake
add_executable(my_rfid_app main.c)
target_link_libraries(my_rfid_app freewili2_bsp)
fw2_display_app(my_rfid_app
    POWER_ZONES DISPLAY NFC_RFID
    VERSION 001
    DESCRIPTION "My RFID app")
```

`freewili2_bsp` already links the library and puts its header on the include
path; there is nothing else to add. A C application that links `pico_stdlib`
needs no extra library, and `freewili2_bsp` links `pico_stdlib` for every app
in this repository. An app that also saves or loads cards on the SD card links
OneWili and lists the SD card's power zone:

```cmake
target_link_libraries(my_rfid_app freewili2_bsp onewili_fwgui)
fw2_display_app(my_rfid_app
    POWER_ZONES DISPLAY NFC_RFID SDCARD
    VERSION 001
    DESCRIPTION "My RFID app")
```

### Build and run the example

`python tools/fw.py build hello_wilirfid` builds the example and
`python tools/fw.py test` runs the host tests. As the repository's README
describes, a built app is copied to the SD card's `/apps` folder with
`python tools/fw.py install-app build/apps/hello_wilirfid/hello_wilirfid.uf2`
and started with `python tools/fw.py run-app hello_wilirfid.uf2`.

Buttons: GREY LOAD, YELLOW FORGET, GREEN CLONE, BLUE EMULATE, RED SAVE; hold
PAGE for five seconds for the About screen and HOME for five seconds to leave
the app. This map is the example's own choice, not a rule of the library or
the BSP: your app may map any function to any button.

## Reading cards

### The poll loop

Call `WILiRFID_service()` on every pass of the main loop, at least every
20 ms, then `WILiRFID_poll()`. `WILiRFID_poll()` returns true once for each new
card and fills a `WILiRFID_tag_t`:

| Field | Meaning |
|-------|---------|
| `format` | the card's format (`WILiRFID_FORMAT_*`); `WILiRFID_FORMAT_UNKNOWN` when not recognised |
| `format_name` | display name of the format, static text, never NULL ("Unknown" for UNKNOWN) |
| `id[8]`, `id_len` | card id; the first `id_len` bytes (0 to 8) are valid, the rest are zero |
| `has_facility_card` | true when `facility` and `card_number` are meaningful for this card |
| `facility`, `card_number` | facility (site) code and card number; 0 when `has_facility_card` is false |
| `raw[28]`, `raw_bits` | the card's complete frame as read, MSB first, `raw_bits` (0 to 224) valid bits; 0 for a card loaded from text |

The record is plain data: copy it freely, keep it, write it to a blank or
emulate it later.

Which of `id` and `facility`/`card_number` a card fills depends on its format;
use `id_len` and `has_facility_card` from the record rather than assuming
either. The standard id width is 5 bytes (10 hex digits) for every format
except Indala, and 8 bytes (16 hex digits) for Indala; a saved card's id is
used on load only when it has exactly that width (see
[Saved cards](#saved-cards)).

A card held still at the antenna is reported once, not repeatedly. It is
reported again only after it has been removed (or `WILiRFID_forget()` was
called) and is read again. The reader holds one read: if two cards are read
between polls, only the newer is returned. No cards are reported while
emulating, and cards are normally not reported during a block read.

With `WILiRFID_SCAN_ALL` (the default) a card is usually reported within about
a second.

### Presenting a card

The antenna is on the back of the FreeWili. Lay the card flat against the back
of the FreeWili, over the antenna, and hold it still, within a few
centimetres. A card at the edge of range, or moving, may take longer to
report, or be reported as removed and then arrive again. Hold it in place for
the whole of a write or block read.

### Formats

The Write and Emulate columns are what `WILiRFID_format_can_write()` and
`WILiRFID_format_can_emulate()` return: whether a card of that format, as read
by the reader, can be written to a blank or emulated. The last column is for a
card loaded from a saved-card line, which has no frame (see
[Saved cards](#saved-cards)).

EM4100, HID Prox and Indala cards have been tested on hardware. The other
formats use the same reading path but have not yet been tried with real cards;
treat them as best effort until they have.

| `WILiRFID_format_t` | `format_name` | Read | Write | Emulate | Loaded from a saved card |
|---------------------|---------------|------|-------|---------|--------------------------|
| `WILiRFID_FORMAT_EM4100` (EM4100 / EM4102, 5-byte id) | EM4100 | Yes | Yes | Yes | write and emulate |
| `WILiRFID_FORMAT_INDALA` (Indala 64-bit) | Indala | Yes | Yes | Yes | write and emulate |
| `WILiRFID_FORMAT_INDALA_224` (Indala 224-bit) | Indala 224 | Yes | No | No | display only |
| `WILiRFID_FORMAT_HID_PROX` (facility and card number) | HID Prox | Yes | Yes | Yes | write and emulate, from its numbers |
| `WILiRFID_FORMAT_HID_PROX_LONG` (long card formats) | HID Prox (long) | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_AWID` | AWID | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_IOPROX` (Kantech ioProx) | ioProx | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_PARADOX` | Paradox | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_PYRAMID` (Farpointe Pyramid) | Pyramid | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_DESTRON` (Destron animal tag) | Destron | Yes | No | No | display only |
| `WILiRFID_FORMAT_FDX_B` (animal tag, ISO 11784/11785) | FDX-B | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_VIKING` | Viking | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_NORALSY` | Noralsy | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_PRESCO` | Presco | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_SECURAKEY` | Securakey | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_JABLOTRON` | Jablotron | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_VISA2000` | Visa2000 | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_PAC` (PAC / Stanley) | PAC | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_GUARD_PROX_II` (Guardall G-Prox II) | G-Prox II | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_NEDAP_64` (Nedap 64-bit) | Nedap 64 | Yes | Yes | Yes | display only |
| `WILiRFID_FORMAT_NEDAP_128` (Nedap 128-bit) | Nedap 128 | Yes | Yes | Yes | display only |

`WILiRFID_FORMAT_UNKNOWN` means not recognised, or no format given;
`WILiRFID_FORMAT_COUNT` is the number of format values, not a format. Format
values are stable across versions. Check with the library at run time rather
than hard-coding this table:

```c
if (WILiRFID_format_can_write(tag.format)) { /* offer "Clone" */ }
if (WILiRFID_format_can_emulate(tag.format)) { /* offer "Emulate" */ }
```

`WILiRFID_format_name()` gives the display name of any format value ("Unknown"
for UNKNOWN and out-of-range values). Both helpers and the name lookup work
without a running reader.

### Scan modes

| Mode | Reports | Use it when |
|------|---------|-------------|
| `WILiRFID_SCAN_ALL` (default) | every supported format; a card is usually reported within about a second | the card's format is not known in advance |
| `WILiRFID_SCAN_FAST` | EM4100 and Indala cards only, and reports them sooner | the app only cares about EM4100 or Indala cards; cards of other formats are not reported |

Choose the mode at start with `WILiRFID_start_ex()` or change it at any time
with `WILiRFID_set_scan_mode()`; `WILiRFID_scan_mode()` returns the mode in
effect. While the reader runs a new mode applies from the next
`WILiRFID_service()` call, and the card state, the last read card and a read
not yet polled are kept. Each start uses the mode in its config
(`WILiRFID_SCAN_ALL` for `WILiRFID_start()`), so a mode set while stopped does
not carry over to the next start.

```c
WILiRFID_config_t cfg = { WILiRFID_PIO_AUTO, WILiRFID_SCAN_FAST };
if (!WILiRFID_start_ex(&cfg)) { /* see WILiRFID_last_error() */ }
```

### Card present and removed

`WILiRFID_poll()` reports arrivals. To follow whether the card is still there,
use `WILiRFID_card_state()`:

| `WILiRFID_card_state_t` | Meaning |
|-------------------------|---------|
| `WILiRFID_CARD_NONE` | no card is being followed: reader stopped, nothing read yet, or forgotten |
| `WILiRFID_CARD_PRESENT` | a card has been read and is still at the antenna |
| `WILiRFID_CARD_REMOVED` | the last card has left; its details stay in `WILiRFID_last_tag()` |

A new card read moves the state to PRESENT; the card leaving moves it to
REMOVED shortly afterwards. Removal is reported only while the reader runs;
stopping the reader reports NONE. During emulation the state keeps its value
until emulation ends. During a block read it usually keeps its value, but may
still change for cards seen by the normal reads.

```c
static WILiRFID_card_state_t shown = WILiRFID_CARD_NONE;
WILiRFID_card_state_t now = WILiRFID_card_state();
if (now != shown) {
    shown = now;
    if (now == WILiRFID_CARD_REMOVED) { /* grey out the card on screen */ }
}
```

`WILiRFID_last_tag()` copies the most recently read card. It stays available
after the card is removed and after the reader stops, until a new card is read
or `WILiRFID_forget()` is called.

### Forget

`WILiRFID_forget()` drops the remembered card: `WILiRFID_last_tag()` returns
false until a card is read again, and a REMOVED state becomes NONE (a PRESENT
state stays PRESENT until the card leaves). The next card read, even the same
card still at the antenna, is reported by `WILiRFID_poll()` as new. Use it for
a "read again" button.

## Writing and cloning

### Blanks to buy

To write or clone a card, buy T5577-based cards or key fobs, usually sold as
"T5577" or "125 kHz rewritable" (also "writable" or "copyable"). Read-only
cards cannot be written.

### The flow

1. Read the card to copy (a `WILiRFID_tag_t` from `WILiRFID_poll()`), or load
   one from a saved-card line. Check `WILiRFID_format_can_write(tag.format)`.
   A card loaded from a saved record can only be written for EM4100, HID Prox
   and Indala (see [Saved cards](#saved-cards)).
2. Have the user remove the original card, then lay the blank flat against the
   back of the FreeWili, over the antenna, and hold it still.
   `WILiRFID_write_begin()` does not check what is at the antenna: the write is
   sent even with no blank or a read-only card there, so the app must make sure
   the original is gone first (`apps/hello_wilirfid` locks the card and prompts
   for the swap before the second CLONE press).
3. Show a "hold still" message, then call `WILiRFID_write_begin(&tag, NULL)`.
4. Keep calling `WILiRFID_service()` and watch `WILiRFID_write_status()` until
   it is `WILiRFID_WRITE_VERIFIED` or `WILiRFID_WRITE_MISMATCH`. The blank must
   stay still on the back of the FreeWili until then.

```c
WILiRFID_result_t r = WILiRFID_write_begin(&tag, NULL);
if (r != WILiRFID_OK) {
    // If WILiRFID_write_status() is WILiRFID_WRITE_FAILED the blank may be
    // partly written (see WILiRFID_write_cancel); otherwise nothing was sent.
} else {
    for (;;) {
        fw2_app_recovery_task();
        picpwr_task();
        WILiRFID_service();
        WILiRFID_write_status_t s = WILiRFID_write_status();
        if (s == WILiRFID_WRITE_VERIFIED) { /* done: the blank is now a copy */ break; }
        if (s == WILiRFID_WRITE_MISMATCH) { /* see the table below */ break; }
        if (s == WILiRFID_WRITE_FAILED)   { /* the whole write could not be sent */ break; }
    }
}
```

`WILiRFID_OK` from `WILiRFID_write_begin()` does not mean the card was
written: it means the write was sent and the check has begun. The outcome
comes from `WILiRFID_write_status()` and is known within about ten seconds of
the call.

A record as read (`raw_bits` not 0) is reproduced exactly when its format can
be copied; otherwise the card is rebuilt from the record's facility and card
numbers (HID Prox) or its id (EM4100, Indala).

When the copy is confirmed the status becomes VERIFIED; the new card may also
be reported by `WILiRFID_poll()`; other cards are still reported meanwhile. The
copy is confirmed only by `WILiRFID_write_status()`; cards reported by
`WILiRFID_poll()` during the check are informational.

### Timing

`WILiRFID_write_begin()` and `WILiRFID_t5577_write_block()` block for up to
about two seconds. While a write-and-check or a block read is in progress, a
`WILiRFID_service()` call can block for up to about two seconds. Show a "hold
still" message first.

### Write status

| `WILiRFID_write_status_t` | Meaning | What to do |
|---------------------------|---------|------------|
| `WILiRFID_WRITE_IDLE` | no write since the reader started, or it was cancelled | nothing |
| `WILiRFID_WRITE_WRITING` | the card's blocks are being written (only seen inside `WILiRFID_write_begin()`) | wait |
| `WILiRFID_WRITE_VERIFYING` | the copy is being checked | keep calling `WILiRFID_service()`; tell the user to keep the blank still |
| `WILiRFID_WRITE_VERIFIED` | the copy is confirmed | done; the blank is now a copy |
| `WILiRFID_WRITE_MISMATCH` | the copy could not be confirmed within about ten seconds (no blank, read-only card, wrong password, or card moved); the blank may be partly written | see below |
| `WILiRFID_WRITE_FAILED` | the reader could not send the whole write; the blank may be partly written | check `WILiRFID_state()` (NO_SIGNAL or FAULT) and the result of `WILiRFID_write_begin()` |

`WILiRFID_WRITE_MISMATCH` is what a write that did not take looks like. The
write is still sent, and the status ends as MISMATCH, in each of these cases:

- no blank was at the reader;
- the card is read-only (not a T5577 blank);
- the blank is password-protected and the password was wrong or missing;
- the card was moved away too early.

Tell the user to lay a T5577 blank flat against the back of the FreeWili, over
the antenna, hold it still and try again. A final status (VERIFIED, MISMATCH or
FAILED) stays until the next `WILiRFID_write_begin()`, `WILiRFID_write_cancel()`
or `WILiRFID_stop()`.

`WILiRFID_write_cancel()` abandons a write-and-check in progress and sets the
status to IDLE. The written blocks are left as they are; only the read-back is
stopped. A cancelled or failed clone can leave the blank with mixed contents;
writing a preset to block 0 (`WILiRFID_t5577_write_block`) and cloning again
recovers it.

### Results that send nothing

These results leave the status unchanged and write nothing:

- `WILiRFID_NOT_RUNNING`: the reader is not started;
- `WILiRFID_BUSY`: emulating, during another write-and-check, or during a
  block read;
- `WILiRFID_BAD_ARGUMENT`: `tag` is NULL, or the record lacks the numbers or
  id its format needs, or they are out of range;
- `WILiRFID_UNSUPPORTED_FORMAT`: the record cannot be written (see the
  formats table).

When the reader could not send the whole write (the blank may be partly
written), the status becomes `WILiRFID_WRITE_FAILED` and the result is
`WILiRFID_NOT_RUNNING` (the board is not responding: state NO_SIGNAL or FAULT)
or `WILiRFID_FAILED`.

Writes need the reader in READY or STARTING (not NO_SIGNAL, FAULT or
EMULATING).

### Passwords

The second argument of `WILiRFID_write_begin()` is the blank's access
password, or NULL. Pass a password only for a card you know is
password-protected. Passing one to a card that is not can corrupt it, so pass
NULL otherwise.

To protect a blank with a password, write the password to block 7 while the
card is still unprotected, then set bit 4 of block 0 (see
[Layout of a blank](#layout-of-a-blank)). Keep a record of the password: the
card answers writes and block reads only with it from then on.

1. Read block 0 with no password (see [Reading a block](#reading-a-block))
   and keep the value, `config`.
2. `WILiRFID_t5577_write_block(7, password, NULL, true)`: the card is not
   protected yet, so no password is passed.
3. `WILiRFID_t5577_write_block(0, config | 0x00000010u, NULL, true)`: this
   turns password mode on.
4. Check it: read block 0 again, now passing `&password`, and test that bit 4
   is set. If step 3 did not take, the card is still unprotected.

From then on pass `&password` to `WILiRFID_write_begin()`,
`WILiRFID_t5577_write_block()` and `WILiRFID_t5577_read_block_begin()` for
this card.

## Tag memory and presets

### Layout of a blank

A T5577 blank holds eight 32-bit blocks, numbered 0 to 7:

| Block | Holds |
|-------|-------|
| 0 | configuration: how the card answers, and so which format a reader sees |
| 1 to 6 | the card's data |
| 7 | an optional password |

Bit 4 of block 0 (`value & 0x00000010`) turns password mode on: the card then
accepts writes and block reads only with the password held in block 7.

### Presets

A preset is a ready-made block-0 value for one card format. List them at run
time:

```c
for (unsigned i = 0; i < WILiRFID_t5577_preset_count(); i++) {
    const WILiRFID_t5577_preset_t *p = WILiRFID_t5577_preset(i);
    printf("%s  %s  0x%08lX\n", p->name, p->description, (unsigned long)p->config);
}
```

Each preset has a short key (`name`), the plain name of the card format it
selects (`description`) and the block-0 value (`config`). All text is static
and never NULL; `WILiRFID_t5577_preset()` returns NULL for an index past the
end.

`WILiRFID_t5577_config_for(format)` returns the block-0 value a fresh
`WILiRFID_write_begin()` of that format would use, or 0 for a format the reader
does not write.

### Writing a block

```c
WILiRFID_result_t r = WILiRFID_t5577_write_block(block, value, NULL, false);
```

- `block` is 0 to 7. Blocks 0 and 7 are written only when the last argument,
  `confirm_config_block`, is true; otherwise the call returns
  `WILiRFID_PROTECTED_BLOCK`. For blocks 1 to 6 it is ignored.
- The third argument is the card's access password, or NULL (the usual case).
- This is a direct write with no read-back: `WILiRFID_OK` means the write was
  sent, even when no card was there to take it. Check it with a block read.
- It blocks for up to about two seconds; show a "hold still" message first.
- Results: `WILiRFID_OK`; `WILiRFID_NOT_RUNNING` (not started, or the board is
  not responding); `WILiRFID_BUSY` (emulating, during a write-and-check or
  during a block read); `WILiRFID_BAD_ARGUMENT` (block above 7);
  `WILiRFID_PROTECTED_BLOCK`; `WILiRFID_FAILED` (the block could not be
  written).

> **Warning: block 0.** Writing a wrong block 0 can leave a card that no reader
> recognises. To make it readable again, write the matching preset to block 0
> with `confirm_config_block` set:
>
> ```c
> WILiRFID_t5577_write_block(0, WILiRFID_t5577_config_for(WILiRFID_FORMAT_EM4100),
>                            NULL, true);
> ```
>
> (or the `config` of the preset for the format you want, from
> `WILiRFID_t5577_preset()`). A preset alone makes the card readable again.

> **Warning: block 7 and passwords.** Writing block 7 on a card whose block 0
> enables the password locks the card behind that password: keep a record of
> it. Pass a password to any call only for a card you know is
> password-protected; passing one to a card that is not can corrupt it.

### Reading a block

Block reads run in the background inside `WILiRFID_service()`:

```c
uint32_t value;
if (WILiRFID_t5577_read_block_begin(0, NULL) == WILiRFID_OK) {
    WILiRFID_read_status_t s;
    while ((s = WILiRFID_t5577_read_block_status(&value)) == WILiRFID_READ_READING) {
        fw2_app_recovery_task();
        picpwr_task();
        WILiRFID_service();
    }
    if (s == WILiRFID_READ_DONE) {
        bool protected_card = (value & 0x00000010u) != 0;
        /* ... */
    }
}
```

| `WILiRFID_read_status_t` | Meaning |
|--------------------------|---------|
| `WILiRFID_READ_IDLE` | no block read has been started yet |
| `WILiRFID_READ_READING` | a block read is in progress |
| `WILiRFID_READ_DONE` | the block was read; its value is written to `*value_out` |
| `WILiRFID_READ_FAILED` | the block could not be read (no card, the read did not succeed, or the reader stopped) |

- A block read completes within about six seconds. While it runs,
  `WILiRFID_service()` may take longer than usual, cards are normally not
  reported, and writes and emulation return `WILiRFID_BUSY`.
  `WILiRFID_t5577_read_block_begin()` itself returns within a few tens of
  milliseconds.
- READ_DONE and READ_FAILED stay until the next successful begin. `value_out`
  may be NULL, and is left untouched for every status other than READ_DONE.
- A missing card is not reported by the begin call: the read ends as
  READ_FAILED.
- Begin results: `WILiRFID_OK`; `WILiRFID_NOT_RUNNING`; `WILiRFID_BUSY`
  (emulating, during a write-and-check or another block read);
  `WILiRFID_BAD_ARGUMENT` (block above 7).

Read block 0 first: on some cards data blocks read reliably only after block 0
of the same card has been read, and block 0 says whether the card is
password-protected. Pass a password only for a card whose block 0 says it is
password-protected.

> **Warning: protected cards.** A protected card read without its password
> ends as READ_FAILED or returns an unreliable value; check block 0 first.

A block read does not change `WILiRFID_state()`: it keeps showing the
reader's state, normally READY. Follow the read with
`WILiRFID_t5577_read_block_status()`.

## Emulation

Emulation makes the board present itself to an external reader as the card in
a tag record, instead of reading cards. Hold the back of the FreeWili, where the
antenna is, against the external reader the way you would hold the card it
stands for.

```c
// The reader must already be running; emulation does not start it.
// user_pressed_stop() and show_in_range() are the app's own functions.
if (WILiRFID_emulate_begin(&tag) == WILiRFID_OK) {
    while (!user_pressed_stop()) {
        fw2_app_recovery_task();
        picpwr_task();
        WILiRFID_service();                     // keep servicing while emulating
        WILiRFID_emu_status_t st = WILiRFID_emulate_status();
        show_in_range(st.reader_field_detected); // "close enough" indicator
    }
    WILiRFID_emulate_stop();                    // reading resumes by itself
}
```

The formats that can be emulated are those `WILiRFID_format_can_emulate()`
accepts; a card loaded from text can be emulated only as described in
[Saved cards](#saved-cards).

### If an external reader does not pick it up

Choose an emulation profile. DEFAULT is the profile to try first; if a reader
does not pick up the card, try ALT_1, then ALT_2, then the same three with
`alternate` set. Give each one a few seconds, with the back of the FreeWili
held still against the external reader, before you move on to the next.
Pass the options to `WILiRFID_emulate_begin_ex()`:

```c
WILiRFID_emu_options_t opt = { .profile = WILiRFID_EMU_ALT_1, .alternate = false };
WILiRFID_emulate_begin_ex(&tag, &opt);
```

| Option | Values |
|--------|--------|
| `profile` (`WILiRFID_emu_profile_t`) | `WILiRFID_EMU_DEFAULT` (the profile to try first), `WILiRFID_EMU_ALT_1` (first alternative profile, for readers DEFAULT does not reach), `WILiRFID_EMU_ALT_2` (second alternative profile) |
| `alternate` | an alternative some readers need; try it after the three profiles |

Stop the current emulation with `WILiRFID_emulate_stop()` before beginning the
next. After each `WILiRFID_emulate_stop()`, check `WILiRFID_last_error()`:
`WILiRFID_HW_UNAVAILABLE_PIO` or `_DMA` there means the reader keeps running
without the resources it could not take back; stop and start it to recover.

### Status

`WILiRFID_emulate_status()` returns a snapshot:

| Field | Meaning |
|-------|---------|
| `active` | true while a card is being emulated |
| `reader_field_detected` | true when an external reader is detected at the board; false when not active. Use it to show the user that the board is close enough. It does not mean the external reader accepted the card: the library has no such signal, so watch the external reader itself |
| `elapsed_ms` | milliseconds since the current emulation began, 0 when not active |
| `format` | the emulated card's format, `WILiRFID_FORMAT_UNKNOWN` when not active |

`WILiRFID_emulating()` is true while a card is being emulated.

While emulating, the state is `WILiRFID_STATE_EMULATING` (a fault still
shows as `WILiRFID_STATE_FAULT`), no cards are reported, and
`WILiRFID_write_begin()`, `WILiRFID_t5577_write_block()` and
`WILiRFID_t5577_read_block_begin()` return `WILiRFID_BUSY`. Emulation runs
until `WILiRFID_emulate_stop()` or `WILiRFID_stop()`.

### Emulation results

`WILiRFID_emulate_begin()` returns `WILiRFID_OK` once the board is presenting
the card, or:

- `WILiRFID_BAD_ARGUMENT` when `tag` is NULL (checked first, even when not
  running), or when the record lacks the numbers its format needs or they are
  out of range; `WILiRFID_emulate_begin_ex()` also returns it for an
  out-of-range `profile` (checked before the running check);
- `WILiRFID_NOT_RUNNING` when the reader is not started;
- `WILiRFID_BUSY` when a card is already being emulated, or during a
  write-and-check or a block read;
- `WILiRFID_UNSUPPORTED_FORMAT` when the format cannot be emulated;
- `WILiRFID_FAILED` when the board could not start presenting the card: stop
  and start the reader, and retry.

## Saved cards

### The record

A saved card is one line of plain text ending in a newline:

```
rfid1 fmt=<format number> fc=<facility> cn=<card number> id=<hex>
```

The numbers are decimal. The format number is an internal code; let these
helpers convert it rather than writing the line by hand. The id is upper-case
hex: the id bytes as the card has them. The `id=` field is always written and
is empty when the card has no id.

| Helper | Does |
|--------|------|
| `WILiRFID_tag_to_string(tag, buf, len)` | writes the line (with its newline), NUL-terminated; returns false only when `tag` or `buf` is NULL or `len` is too small. A line is always shorter than 96 bytes |
| `WILiRFID_tag_from_string(text, out)` | reads a line into `*out`; returns false when the text is not a saved card (no leading record tag, or a missing format, facility or card number, or one with no digit right after its `=`), and then clears `*out` |
| `WILiRFID_tag_name(tag, buf, len)` | writes the file-name stem the device uses for this card: lower-case letters, digits and `-` only; the id for EM4100, facility and card number for every other format. A buffer of `WILiRFID_TAG_NAME_MAX` bytes is always enough |

None of the helpers needs a running reader.

Every card the reader reports can be saved. On load the id is used when it has
exactly the standard width (5 bytes, 8 for Indala); otherwise the card loads
from its numbers, which is how cards with facility and card numbers, such as HID
Prox, travel. An id that is absent, shorter, longer or not hex is treated as no
id. An unrecognised format number reads as UNKNOWN. A loaded card has
`raw_bits` 0. For screen layout, use `id_len` (0 to 8) and `has_facility_card`
from the record rather than assuming either from the format.

The stem is `<format>-<id>` for EM4100 and `<format>-fc<facility>-cn<card
number>` for every other format, with the id in lower-case hex and the numbers
in decimal: for example `em4100-0123456789` and `hid-prox-fc12-cn3456`.
`<format>` is the device's own name for the format, in lower case with every
run of other characters turned into one `-`. It is `format_name` treated that
way except for three formats: HID Prox (long) gives `hid-prox`, PAC gives
`pac-stanley` and G-Prox II gives `guard-prox-ii`. An unrecognised format gives
`card`.

A stem is at most `WILiRFID_TAG_NAME_MAX - 1` (39) characters. It comes from
the card alone, so saving the same card again with `ow_sd_put_mem()` (`append`
false) replaces its file. A card whose format has no facility and card number
names as `<format>-fc0-cn0`, so two such cards of one format get the same
stem: add something of your own to the name, such as the id, when both must
be kept.

What a loaded card can do:

- EM4100 and Indala write and emulate exactly as the original card.
- HID Prox writes and emulates from its facility and card numbers, written as
  a standard HID card with those numbers; a reader expecting another HID
  layout may see a different card.
- Every other format loads for display only: `WILiRFID_write_begin()` and
  `WILiRFID_emulate_begin()` return `WILiRFID_UNSUPPORTED_FORMAT` for it.

### Where the device keeps them

The device keeps saved cards on its SD card in `/rfid/saved`
(`WILiRFID_SAVED_DIR`), one `<name>.rfid` file per card (`WILiRFID_SAVED_EXT`),
named with `WILiRFID_tag_name()`. Cards saved there by an app load on the
device, and cards saved on the device load in apps.

`/rfid/saved` is the deliberate interoperability exception to the rule that
app-owned data lives under `/appdata/<app>/`: the folder is shared with the
device on purpose. See [App-owned data](../app-storage.md#app-owned-data).

### Reading and writing them from an app

The SD card belongs to the main CPU; an app reaches it through OneWili. The
calls are the ones `apps/hello_sdcard` uses (open the link with
`fw2_app_recovery_open_onewili()`, then `fw2_app_recovery_wrap_sd()`), and
`apps/hello_wilirfid` saves and loads cards with them. Add `onewili_fwgui` and
the `SDCARD` power zone to the app's CMakeLists.txt (see above).

The reader and OneWili can be started in either order. `apps/hello_wilirfid`
starts the reader first and sets `ow_sd_set_timeout_ms(1000)` so an SD call
returns within about a second even when the main CPU does not answer. Cards
are not read while a SAVE or LOAD runs: about a second for each SD call when
the main CPU does not answer, so a SAVE can take a few seconds.

> **Warning: only EM4100, HID Prox and Indala can be cloned again after
> loading.** Every other format is display-only once reloaded from a saved
> record: the record holds numbers, not the whole card. When a clone (or an
> emulation) of such a format must follow, keep the live card's
> `WILiRFID_tag_t` in RAM until it is made.

```c
#include "onewili.h"
#include "onewili_fwgui.h"
#include "onewili_sd.h"
#include "input/app_recovery_onewili.h"
#include <stdio.h>
#include <string.h>

static ow_device dev;   // large: keep it static, not on the stack

// Call once, after WILiRFID_start(): a few attempts, never forever. Opening
// the link does not reach the main CPU; the stat call does. False means the
// main CPU did not answer; report it and carry on without saving.
static bool open_sd(void) {
    for (int i = 0; i < 3; i++) {
        if (fw2_app_recovery_open_onewili(&dev) == OW_OK &&
            fw2_app_recovery_wrap_sd() == OW_OK) {
            bool is_dir; uint32_t size;
            ow_sd_set_timeout_ms(1000);
            ow_status p = ow_sd_stat(&dev, "/", &is_dir, &size);
            if (p != OW_ERR_TIMEOUT && p != OW_ERR_IO) return true;  // any answer means it is up
        }
        fw2_app_recovery_sleep_ms(100);
    }
    return false;
}

static bool save_card(const WILiRFID_tag_t *tag) {
    char line[96], stem[WILiRFID_TAG_NAME_MAX], path[96];
    if (!WILiRFID_tag_to_string(tag, line, sizeof line)) return false;
    if (!WILiRFID_tag_name(tag, stem, sizeof stem)) return false;
    snprintf(path, sizeof path, "%s/%s%s", WILiRFID_SAVED_DIR, stem, WILiRFID_SAVED_EXT);
    // An existing folder returns an error; only a lost link (timeout) matters here.
    ow_sd_mkdir(&dev, "/rfid");
    ow_sd_mkdir(&dev, WILiRFID_SAVED_DIR);
    return ow_sd_put_mem(&dev, path, line, strlen(line), false) == OW_OK;
}

static bool load_card(const char *path, WILiRFID_tag_t *out) {
    char line[96];
    size_t got = 0;
    if (ow_sd_get_mem(&dev, path, line, sizeof line - 1, &got) != OW_OK) return false;
    line[got] = '\0';
    return WILiRFID_tag_from_string(line, out);
}
```

To list the saved cards, call `ow_sd_list(&dev, WILiRFID_SAVED_DIR, cb, user)`
with a callback
`void cb(const char *name, bool is_dir, uint32_t size, void *user)`, and keep
the names that end in `.rfid`.

## Dependencies

Everything below is claimed by `WILiRFID_start()`, including what emulation
needs, and held until `WILiRFID_stop()`, which releases all of it. A driver
started after the reader cannot take any of it. Emulation uses these reserved
resources and claims nothing new.

- **GPIO 34, 35, 36, 44, 46.**
- **PWM slice 9.** The haptic motor cannot be used while the reader runs.
- **The ADC.** Do not use `board_die_temp_c()` or any other ADC reader while
  the reader runs.
- **One PIO block: two state machines and 21 instruction slots.** The reader
  sets that block's GPIO base to 16, so the block must either be completely
  unused or already have its base at 16. Blocks are searched in the order 1,
  2, 0, unless `WILiRFID_config_t.pio_block` pins one (0, 1 or 2; only that
  block is tried). **Init-order rule:** start the reader before the LED,
  microphone, IR or radio drivers, so it can claim a free PIO block first; or
  pin a block that stays free with `WILiRFID_config_t.pio_block` and
  `WILiRFID_start_ex()`. If start fails with `WILiRFID_HW_UNAVAILABLE_PIO`,
  that is the fix. With the stock `board_init()`, `WILiRFID_PIO_AUTO` always
  finds a free block; pin one only when your own app uses PIO.
- **Three DMA channels**, claimed dynamically (any free channels).
- **About 92 KB of code and constants plus under 64 KB of data.** In an app
  loaded to RAM all of it sits in SRAM, and linking any `WILiRFID_` function
  brings in the whole library.
- **No interrupts.** The reader installs no interrupt handlers. Call every
  function from the main loop of one core; none is safe to call from an
  interrupt handler.
- **Power zone `NFC_RFID`.** List `NFC_RFID` in `POWER_ZONES`;
  `fw2_app_recovery_init()` then keeps it awake and waits for it, so no
  `picpwr_keep_awake()` call is needed. An app that does not call
  `fw2_app_recovery_init()` must keep the zone awake with
  `picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_NFC_RFID))` for as long as the
  reader runs and wait until `picpwr_rails()` reports it powered (both declared
  in `input/picpwr.h`). With the zone asleep (or the RFID board
  missing) the reader still starts, but it never reaches READY: it stays in
  STARTING or shows NO_SIGNAL.

Service cadence:

- Call `WILiRFID_service()` at least every 20 ms. A normal call usually takes
  a few milliseconds, occasionally a few tens of milliseconds. It never waits
  for a card.
- Longer gaps between calls are tolerated and never break anything, but a card
  presented briefly may be missed and reports arrive later.
- `WILiRFID_write_begin()` and `WILiRFID_t5577_write_block()` block for up to
  about two seconds. While a write-and-check or a block read is in progress, a
  `WILiRFID_service()` call can block for up to about two seconds. Show a
  "hold still" message first. Keep `fw2_app_recovery_task()` and
  `picpwr_task()` in the same loop.

Exclusions:

- **Never run the open rfid_* driver** ([rfid.md](rfid.md)) at the same time as
  this library; the two conflict.
- **Do not change the system clock** while the reader runs.

Coexistence:

- **The NFC library may run in the same app.** The WILiNFC library
  ([wilinfc.md](wilinfc.md)) uses only I2C1, which this library does not use,
  and shares power zone `NFC_RFID`: declare the zone once in `POWER_ZONES`.
  Call both service functions in the main loop; each library's blocking calls
  delay the other's servicing. With an NFC card in the field, one
  `WILiNFC_service()` call can take about 50 ms, longer than this library's
  20 ms service interval. A 125 kHz card presented briefly at the same time
  may be missed. Whether the two fields disturb each other has not been
  verified on hardware. See
  [Using both libraries in one app](#using-both-libraries-in-one-app).

## Using both libraries in one app

One app may run the WILiRFID library ([wilirfid.md](wilirfid.md)) and the
WILiNFC library ([wilinfc.md](wilinfc.md)) together. The rules:

- **Start order.** Either library may be started first. List `NFC_RFID` once
  in `POWER_ZONES`; it powers both front ends.
- **Service both on every loop pass.** Call `WILiRFID_service()` and
  `WILiNFC_service()` in the same main loop, together with
  `fw2_app_recovery_task()` and `picpwr_task()`.
- **Blocking calls.** Do not run a blocking operation of one library while
  the other must keep tracking a card. An RFID write or block read, and a
  MIFARE Classic dictionary run or dump, stop the other library's servicing
  until they return.
- **One link and one SD adapter.** Open one `ow_device` and the SD link once,
  and use them for both libraries: the WILiNFC file hooks and the WILiRFID
  saved-card files go through the same `ow_device`.
- **Buttons.** Which buttons each part of the app owns is the app's choice;
  neither library reads the buttons.

## States and results

### `WILiRFID_state_t`

`WILiRFID_state()` returns the state; `WILiRFID_state_name()` gives its short
name for logs (for example "READY").

| State | Meaning | What to do |
|-------|---------|------------|
| `WILiRFID_STATE_OFF` | not started, or stopped | call `WILiRFID_start()`. Only the constant-time getters, the format and preset tables and the saved-card helpers do anything useful |
| `WILiRFID_STATE_STARTING` | starting up (just started, or recovering after NO_SIGNAL); cards are reported once READY, typically in under a second | keep calling `WILiRFID_service()`. Still STARTING after about two seconds: check that the RFID board is seated and powered; call `WILiRFID_stop()`, fix it, start again |
| `WILiRFID_STATE_READY` | reading normally; cards are reported through `WILiRFID_poll()` | poll for cards |
| `WILiRFID_STATE_NO_SIGNAL` | the RFID board is not responding (unseated or unpowered); the reader recovers by itself once the board is usable | check the board and the power zone. Writes are refused meanwhile |
| `WILiRFID_STATE_FAULT` | an internal error occurred | call `WILiRFID_stop()` and start again. How often to retry, and when to give up, is the app's choice: `apps/hello_wilirfid` stops the reader, starts it again at once, and then tries every two seconds until a start succeeds |
| `WILiRFID_STATE_EMULATING` | the board is presenting a card to an external reader instead of reading | call `WILiRFID_emulate_stop()` to return to reading |

### `WILiRFID_result_t`

Calls that return a result also record it for `WILiRFID_last_error()`, as does
every start (successful or not). Other calls leave it unchanged, except
`WILiRFID_emulate_stop()`, which records `WILiRFID_HW_UNAVAILABLE_PIO` or `_DMA`
when the reader could not take its resources back. It is `WILiRFID_OK` before
any such call. `WILiRFID_result_name()` gives the short
name for logs (for example "BUSY"). Both name helpers return "?" for a value
outside the enum and never return NULL.

| Result | Meaning | What to do |
|--------|---------|------------|
| `WILiRFID_OK` | the call succeeded | for a write, watch `WILiRFID_write_status()`; for a block read, `WILiRFID_t5577_read_block_status()` |
| `WILiRFID_NOT_RUNNING` | the reader is not started; for writes this also means the board is not responding | check `WILiRFID_state()` to tell the two apart: start the reader (OFF), or check the RFID board and the power zone (NO_SIGNAL or FAULT) |
| `WILiRFID_BUSY` | a write-and-check, block read or emulation is in progress | try again later, or stop the emulation / cancel the write |
| `WILiRFID_BAD_ARGUMENT` | an argument is NULL or out of range, or a card record lacks the numbers its format needs | fix the call; for a loaded card, check its numbers and id |
| `WILiRFID_UNSUPPORTED_FORMAT` | the card format cannot be used by this call | check `WILiRFID_format_can_write()` / `WILiRFID_format_can_emulate()`; for a loaded card, use the record as read instead |
| `WILiRFID_PROTECTED_BLOCK` | block 0 or 7 was to be written without `confirm_config_block` | pass `confirm_config_block` true only when you mean it |
| `WILiRFID_HW_UNAVAILABLE_PIO` | start: no PIO block had two free state machines and 21 free instruction slots with a usable GPIO base | start the reader before the LED, microphone, IR or radio drivers, or pin a free block with `WILiRFID_start_ex()` |
| `WILiRFID_HW_UNAVAILABLE_DMA` | start: three DMA channels could not be claimed, or the reader failed to start | free some DMA channels and start again |
| `WILiRFID_API_VERSION_MISMATCH` | start: the app was built against a different header version | rebuild against the header that came with the library |
| `WILiRFID_FAILED` | the operation did not complete | for emulation, stop and start the reader and retry; otherwise retry, then report it |

After `WILiRFID_start()` returns false nothing is left claimed and the state is
OFF. Calling `WILiRFID_start()` again while the reader runs returns true and
changes nothing. `WILiRFID_start_ex()` also returns false with
`WILiRFID_BAD_ARGUMENT` for a `pio_block` other than `WILiRFID_PIO_AUTO`, 0, 1
or 2, or a `scan_mode` outside the enum. Always call the `WILiRFID_start()` and
`WILiRFID_start_ex()` macros, not the `_v` functions behind them: the macros
pass the header's `WILiRFID_API_VERSION` so a header and library mismatch is
caught at start.

## Reporting a problem

Open an issue on the repository with:

- the library version, from `WILiRFID_version()`;
- the state, from `WILiRFID_state_name(WILiRFID_state())`;
- the last error, from `WILiRFID_result_name(WILiRFID_last_error())`;
- for a write, the final `WILiRFID_write_status()` and the result of
  `WILiRFID_write_begin()`; for a block read, the final
  `WILiRFID_t5577_read_block_status()`;
- the card: its `format_name`, and its saved-card line from
  `WILiRFID_tag_to_string()`;
- for a write or clone, what the blank was sold as;
- for emulation, the profile and `alternate` setting tried,
  `reader_field_detected`, and the make of the external reader;
- what the app was doing, and which other drivers it starts and in what order.
