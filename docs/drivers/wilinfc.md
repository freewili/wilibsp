# 13.56 MHz NFC library (`bsp/wilinfc/`)

## What it does

WILiNFC reads NFC and MIFARE Classic cards, reads and writes MIFARE Classic
blocks, presents a card's identity to another reader, saves cards as Flipper
`.nfc` files, and offers a raw channel for experts. It ships as a precompiled
library (`bsp/wilinfc/libWILiNFC.a`) plus one header (`WILiNFC.h`). It is
already part of `freewili2_bsp`, and `fw2.h` includes it.

`apps/hello_wilinfc` is a complete example app: read, emulate, and save to
the SD card. Start from it when in doubt.

The reader's antenna is on the back of the FreeWili: every card is held flat
against the back of the FreeWili.

This document and the header are all you need to use every part of the
library except the raw channel, which is expert use outside the supported
contract (see [The raw channel](#the-raw-channel)).

The 125 kHz tag family (EM4100, T5577 and the other 125 kHz formats) is a
different front end with its own drivers: the WILiRFID library
([wilirfid.md](wilirfid.md)) or the open `rfid_*` driver ([rfid.md](rfid.md)).

## Quick start: read a card

```c
#include "fw2.h"
#include <stdio.h>

// The LCD takes colours byte-swapped; write them in ordinary RGB565.
static inline uint16_t be16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

int main(void) {
    board_init();
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd();
    st7796_fill_screen(be16(0x0000));
    board_backlight_set(1);

    // fw2_app_recovery_init() keeps every zone listed in POWER_ZONES (NFC_RFID
    // among them) awake and waits until they are on, so no power call of our
    // own is needed. Switch off the inherited rails this app does not use.
    picpwr_release_unused();

    if (!WILiNFC_start()) {
        st7796_draw_text(8, 8, 2, be16(0xF800), be16(0x0000), WILiNFC_result_name(WILiNFC_last_error()));
        for (;;) { fw2_app_recovery_task(); picpwr_task(); }
    }
    st7796_draw_text(8, 8, 2, be16(0xFFFF), be16(0x0000), "HOLD A CARD FLAT ON THE BACK");

    for (;;) {
        fw2_app_recovery_task();
        picpwr_task();
        WILiNFC_service();

        WILiNFC_card_t card;
        if (WILiNFC_poll(&card)) {
            char buf[40];
            int n = 0;
            for (unsigned i = 0; i < card.uid_len; i++)
                n += snprintf(buf + n, sizeof buf - n, "%02X", card.uid[i]);
            st7796_fill_rect(8, 40, 464, 20, be16(0x0000));
            st7796_draw_text(8, 40, 2, be16(0x07E0), be16(0x0000), buf);
        }
    }
}
```

`fw2_app_recovery_init()` keeps the zones declared in `POWER_ZONES` awake and
waits for them before it returns, so an app that lists `NFC_RFID` needs neither
a `picpwr_keep_awake()` call nor a wait loop of its own. It keeps waiting until
every declared zone is on, so if one never comes on the screen stays dark (HOME
still leaves the app).

The full example, with a labelled button bar, emulation and saving to the SD
card, is `apps/hello_wilinfc/`.

## CMakeLists.txt for an NFC app

```cmake
add_executable(my_nfc_app main.c)
target_link_libraries(my_nfc_app freewili2_bsp)
fw2_display_app(my_nfc_app
    POWER_ZONES DISPLAY NFC_RFID
    VERSION 001
    DESCRIPTION "My NFC app")
```

`freewili2_bsp` already links the library and puts its header on the include
path; there is nothing else to add. A C application that links `pico_stdlib`
needs no extra library, and `freewili2_bsp` links `pico_stdlib` for every app
in this repository. An app that also saves or loads files on the SD card (see
[Saving files](#saving-files-the-file-hooks)) links OneWili and lists the SD
card's power zone:

```cmake
target_link_libraries(my_nfc_app freewili2_bsp onewili_fwgui)
fw2_display_app(my_nfc_app
    POWER_ZONES DISPLAY NFC_RFID SDCARD
    VERSION 001
    DESCRIPTION "My NFC app")
```

### Build and run the example

`python tools/fw.py build hello_wilinfc` builds the example and
`python tools/fw.py test` runs the host tests. As the repository's README
describes, a built app is copied to the SD card's `/apps` folder with
`python tools/fw.py install-app build/apps/hello_wilinfc/hello_wilinfc.uf2`
and started with `python tools/fw.py run-app hello_wilinfc.uf2`.

Buttons: YELLOW SAVE, GREEN READ, BLUE EMULATE; hold PAGE for five seconds
for the About screen and HOME for five seconds to leave the app. This map is
the example's own choice, not a rule of the library or the BSP: your app may
map any function to any button.

## Lifecycle

```c
#define WILiNFC_API_VERSION 1u
#define WILiNFC_start()          WILiNFC_start_v(WILiNFC_API_VERSION)
#define WILiNFC_start_ex(config) WILiNFC_start_ex_v((config), WILiNFC_API_VERSION)

bool WILiNFC_start_v(uint32_t api_version);
bool WILiNFC_start_ex_v(const WILiNFC_config_t *config, uint32_t api_version);
void WILiNFC_stop(void);
void WILiNFC_service(void);
WILiNFC_state_t  WILiNFC_state(void);
WILiNFC_result_t WILiNFC_last_error(void);
const char *WILiNFC_version(void);                /* for example "1.0.0" */
const char *WILiNFC_result_name(WILiNFC_result_t result);   /* never NULL */
const char *WILiNFC_state_name(WILiNFC_state_t state);      /* never NULL */
```

`WILiNFC_start()` and `WILiNFC_start_ex()` are macros that pass the
`WILiNFC_API_VERSION` of the header the application was compiled with; call
the macros, not the `_v` functions. `WILiNFC_start()` brings the front end up
and begins looking for cards. It returns `false` on failure and
`WILiNFC_last_error()` says why. Calling it while already running returns
`true`. `WILiNFC_stop()` turns the reader off and releases the front end; it
is safe when not running.

`WILiNFC_start_ex(config)` is the same, with options. This release has none:
`WILiNFC_config_t` holds only `reserved`, which is ignored; set it to 0
(`WILiNFC_config_default()` does). `config` may be NULL for the defaults, and
a NULL config behaves exactly like `WILiNFC_start()`.

`WILiNFC_service()` drives the library: call it continuously from the main
loop (see [Timing](#timing)). `WILiNFC_version()` returns the library version
as a dotted string, for example `"1.0.0"`. It is never NULL and may be called
at any time, including before `WILiNFC_start()`.

### Start failures

Start blocks until it has succeeded or failed, and reports the reason in
`WILiNFC_last_error()`:

- **`WILiNFC_NO_FRONT_END`**: the NFC hardware did not answer on the bus
  (this is about the reader, not a card). Start fails at once. The usual
  causes are that the `NFC_RFID` zone is not on (`NFC_RFID` missing from
  `POWER_ZONES`; or, in an app that does not call `fw2_app_recovery_init()`,
  no `picpwr_keep_awake()` or no wait for the rail), or that the board has no
  NFC front end. Check the zone first.
- **`WILiNFC_INIT_TIMEOUT`**: the hardware answered but was not ready within
  1 s. Power cycle the board; if it persists, the part or its supply is
  unhealthy.
- **`WILiNFC_API_VERSION_MISMATCH`**: the application was compiled against a
  different header version than the library's. Rebuild against the matching
  `WILiNFC.h`.

## Timing

| What | How long |
|---|---|
| `WILiNFC_start()` | Blocks until the reader runs or has failed: normally well under 100 ms, at most about 1.1 s. |
| `WILiNFC_service()` | One step of card detection per call: up to about 10 ms with no card in the field, at most about 50 ms with one. Apart from the file hooks it calls (the key-cache load when a MIFARE Classic card is selected and the save when it leaves), it never waits longer than that. |
| A new card | Reported by `WILiNFC_poll()` within about 50 ms of entering the field (plus any file-hook time). |
| A card that leaves | `WILiNFC_CARD_REMOVED` within about 150 ms. |
| One exchange with the card | Waits at most 50 ms for an answer. |
| Authentication, block read, block write | Normally a few milliseconds; a card that stops answering makes one call wait about 100-150 ms before it fails. |
| `WILiNFC_mfc_dict_run()` | Roughly 10-20 ms per key tried, per unknown sector key: a large dictionary against a card with many unknown keys can take minutes. |
| `WILiNFC_mfc_dump()` | About 1 s for a 1K card, a few seconds for a 4K card. |
| `WILiNFC_raw_transceive()` | Waits at most 50 ms for an answer. |

The detection and removal times assume no gap of more than a few tens of
milliseconds between `WILiNFC_service()` calls. The MIFARE calls block: they
need no `WILiNFC_service()` while they run, and the field is not polled for
new cards until they return. Only `WILiNFC_mfc_dict_run` takes a callback
(`progress`); call `fw2_app_recovery_task()` from it. Your file hooks are also
called during these operations, and the sample SD backend services recovery
in them.

## Reading cards

### The poll loop

```c
typedef struct {
    uint8_t uid[10]; uint8_t uid_len;
    uint8_t atqa[2]; uint8_t sak;
    WILiNFC_card_type_t type;
    uint8_t ats[20]; uint8_t ats_len;   /* 0 unless is_iso_dep */
    bool is_iso_dep;
} WILiNFC_card_t;

bool WILiNFC_poll(WILiNFC_card_t *out);          /* true once per newly selected card */
WILiNFC_card_state_t WILiNFC_card_state(void);   /* NONE, PRESENT, REMOVED */
bool WILiNFC_last_card(WILiNFC_card_t *out);     /* most recent card; false if none yet */
void WILiNFC_forget(void);                       /* a card still in the field is reported again */
bool WILiNFC_halt(void);                         /* put the present card to sleep; false if none */
```

- `WILiNFC_poll` returns `true` exactly once for each newly selected card,
  within about 50 ms of the card entering the field, and `false` when there is
  nothing new or the library is not running. `WILiNFC_service()` must be
  called continuously for it to see anything.
- A card that `WILiNFC_poll` returns is ready: MIFARE calls may start at once,
  with no further `WILiNFC_service()` call. For a MIFARE Classic card the
  automatic key-cache load has already finished by then.
- `WILiNFC_card_state` tells you whether a card is `WILiNFC_CARD_PRESENT` right
  now, `WILiNFC_CARD_REMOVED` (it was here and left) or `WILiNFC_CARD_NONE`
  (no card seen since start or forget). REMOVED is reported within about
  150 ms of the card leaving, and stays until a new card is selected or
  `WILiNFC_forget` is called; it does not go back to NONE by itself.
- `WILiNFC_last_card` keeps the most recent card after it leaves, which is what
  an emulate or save button wants.
- `WILiNFC_forget` makes a card that is still held against the back of the
  FreeWili be reported again by the next poll: use it for a "read again"
  button.
- `WILiNFC_halt` puts the card to sleep; it is reported again after it leaves
  and returns, or after `WILiNFC_forget()`.

Card types (`WILiNFC_card_type_t`): `UNKNOWN`, `T1T`, `T2T` (for example
Ultralight and NTAG), `T4T` (ISO-DEP, for example payment-style and many
access cards), `NFC_DEP` (a peer device) and `MIFARE_CLASSIC`. The library
works the type out from the card's ATQA and SAK (see
[Concepts](#concepts-uid-atqa-and-sak)).

Only MIFARE Classic memory is read and written by the supported calls. For
every other type, including Ultralight and NTAG (`T2T`), the library reports
the card's identity and nothing more: there is no call that reads or writes
their memory, and none that reads or writes NDEF messages. Only the raw
channel, which is outside the supported contract, can reach them.

### Presenting a card

- The antenna is on the back of the FreeWili. Hold **one** card flat against
  the back of the FreeWili and keep it still until the read finishes. Several
  cards at once give `WILiNFC_COLLISION`.
- Metal behind or between the card and the back of the FreeWili weakens or
  blocks reading. Keychain fobs and thick wallets can be marginal; try the
  card alone.
- A card that has been read stays "remembered": to read it again without
  removing it, call `WILiNFC_forget()`.
- The poll cannot tell "no card" from a card that is too far away or still
  moving: a card is reported only once it has answered and been selected, so
  in both cases nothing is reported. If nothing arrives within a second or so,
  tell the user to hold the card flat against the back of the FreeWili and
  keep it still. A card that drifts away after it was reported shows
  `WILiNFC_CARD_REMOVED`, and a MIFARE call on it returns `WILiNFC_CARD_LOST`
  or `WILiNFC_CRC_ERROR`.
- If the state is `WILiNFC_STATE_NO_SIGNAL`, the reader is not looking for
  cards: end any raw session. If it is `WILiNFC_STATE_FAULT`, check
  `WILiNFC_last_error()` and that the power zone is on.

## MIFARE Classic

### The memory model

A MIFARE Classic card is a small block of memory with simple built-in access
control:

- Memory is split into **blocks** of 16 bytes (`WILiNFC_MFC_BLOCK_SIZE`).
- Blocks are grouped into **sectors**. A 1K card has 16 sectors of 4 blocks
  (64 blocks, 1 KB). A Mini has 5 sectors of 4 blocks. A 4K card has 32
  sectors of 4 blocks followed by 8 sectors of 16 blocks (256 blocks, 4 KB).
- **Block 0** is the manufacturer block: the UID and manufacturer data. On
  ordinary cards it cannot be written.
- The **last block of each sector is the sector trailer.** It holds **Key A**
  (6 bytes), the **access bits** (bytes 6-9 of the trailer, with a spare
  byte) and **Key B** (6 bytes). The other blocks of the sector are data
  blocks.
- The **access bits** say, per block, what each key may do: read, write,
  increment, decrement. A data block can be set up as a **value block**, a
  special layout (a 32-bit signed number stored redundantly with an address)
  that supports safe increment and decrement, as used for stored-value cards.
- To touch a block you need a key for its sector, Key A or Key B. The card
  does not tell you the keys: factory cards usually carry well-known default
  keys (such as all `FF` bytes), and real deployments use their own.

> **Warning.** Writing a sector trailer with wrong keys or wrong access bits
> can permanently lock that sector, and there is no recovery. Never write a
> trailer you have not checked. Only write to cards you own.

### Variant and geometry

```c
WILiNFC_mfc_variant_t WILiNFC_mfc_variant_of(const WILiNFC_card_t *card);
uint8_t WILiNFC_mfc_sector_count(WILiNFC_mfc_variant_t v);
uint8_t WILiNFC_mfc_blocks_in_sector(WILiNFC_mfc_variant_t v, uint8_t sector);
uint8_t WILiNFC_mfc_sector_to_block(WILiNFC_mfc_variant_t v, uint8_t sector); /* first block */
uint8_t WILiNFC_mfc_trailer_block(WILiNFC_mfc_variant_t v, uint8_t sector);
```

`WILiNFC_mfc_variant_of` gives a card's variant from its SAK: `0x09` is
`WILiNFC_MFC_MINI`, `0x08` or `0x88` is `WILiNFC_MFC_CLASSIC_1K`, `0x18` or
`0x98` is `WILiNFC_MFC_CLASSIC_4K`, and anything else (or NULL) is
`WILiNFC_MFC_UNKNOWN`. The MIFARE calls use this same variant for their range
checks; for a card of unknown variant they return `WILiNFC_NO_CARD`. Pass the
variant to the geometry helpers to size buffers before reading.

The geometry helpers need no card. Sectors 0-31 have 4 blocks and sectors
32-39 (4K only) have 16. A sector at or beyond the variant's sector count
returns 0. Constants: `WILiNFC_MFC_KEY_SIZE` (6), `WILiNFC_MFC_BLOCK_SIZE` (16),
`WILiNFC_MFC_MAX_SECTORS` (40), `WILiNFC_MFC_MAX_BLOCKS_IN_SECTOR` (16).

### Keys and call order

Every call below needs a selected MIFARE Classic card. They return
`WILiNFC_NOT_RUNNING` before `WILiNFC_start`, `WILiNFC_BUSY` while emulation
or a raw session is active, `WILiNFC_NO_CARD` when no card is selected, and
`WILiNFC_UNSUPPORTED` when the card is not MIFARE Classic.

- **Key slots.** The library keeps a key slot per sector (Key A and Key B) for
  the selected card. Slots are filled by `WILiNFC_mfc_auth`, by
  `WILiNFC_mfc_key_cache_load` and by `WILiNFC_mfc_dict_run`. When the card is
  selected the library also loads its cache file (see
  [Storage paths](#storage-paths)) if file hooks are registered.
- **Authentication is automatic.** Every block, sector and value call
  authenticates by itself from those slots, so you do not need to call
  `WILiNFC_mfc_auth` first when a key is already known. A read tries Key A
  and then Key B; a write tries Key B and then Key A. Without a usable key the
  call returns `WILiNFC_AUTH_FAILED`.
- An authentication lasts across calls for as long as they stay in the same
  sector; a call in another sector authenticates again.
- **Keys belong to the selected card.** They are kept for as long as it stays
  in the field (`WILiNFC_forget` does not clear them) and are discarded when it
  leaves. As it leaves, any keys found are written to its cache file when
  storage is available.
- **After `WILiNFC_halt`.** Halting puts the card to sleep, which ends its
  authentication. Before further MIFARE calls, take the card out of the field,
  or call `WILiNFC_forget` and wait for `WILiNFC_poll` to report it again.

### Block and sector operations

```c
WILiNFC_result_t WILiNFC_mfc_auth(uint8_t sector, WILiNFC_mfc_keytype_t keytype,
                                  const uint8_t key[WILiNFC_MFC_KEY_SIZE]);
WILiNFC_result_t WILiNFC_mfc_read_block(uint8_t block, uint8_t out[WILiNFC_MFC_BLOCK_SIZE]);
WILiNFC_result_t WILiNFC_mfc_write_block(uint8_t block, const uint8_t in[WILiNFC_MFC_BLOCK_SIZE]);
WILiNFC_result_t WILiNFC_mfc_read_sector(uint8_t sector, uint8_t *out, uint16_t out_len, bool *block_ok);
WILiNFC_result_t WILiNFC_mfc_write_sector(uint8_t sector, const uint8_t *in, uint16_t in_len);
WILiNFC_result_t WILiNFC_mfc_access_bits(uint8_t sector, uint8_t out[4]);
```

- `WILiNFC_mfc_auth` authenticates with a 6-byte key and stores it in that
  sector's slot. Its first argument is a **sector number** (0 to
  sector_count - 1), not a block number; `keytype` is `WILiNFC_MFC_KEY_A` or
  `WILiNFC_MFC_KEY_B`. `key[0]` is the first byte as the key is written in
  hex: `A0A1A2A3A4A5` is `{ 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5 }`, the same
  order as a dictionary line and as the key's bytes in the sector trailer.
- `read_block` / `write_block` work on one 16-byte block. A block beyond the
  card returns `WILiNFC_BAD_ARGUMENT`.
- `read_sector` fills `out` with every block of the sector (`out_len` at least
  `blocks_in_sector * 16`). The optional `block_ok` array gets one `bool` per
  block: `true` if that block's bytes came from the card; a block that could
  not be read is zero-filled. It returns `WILiNFC_OK` if at least one block
  was read and `WILiNFC_AUTH_FAILED` if none was.
- `write_sector` writes every non-trailer block of a sector from `in`
  (`in_len` exactly `(blocks_in_sector - 1) * 16`); the trailer is never
  touched. Blocks are written in order and the call stops at the first
  failure, returning that block's result, so it is **not all-or-nothing**. In
  sector 0 the first block is block 0: a card that is not a magic card rejects
  it and nothing is written; a magic card accepts it, so the whole sector is
  written. A sector at or beyond the card's sector count returns
  `WILiNFC_BAD_ARGUMENT`.
- `access_bits` returns the four access-bit bytes (bytes 6-9 of the sector
  trailer).

Value blocks (like the block calls, they authenticate from the key slots by
themselves):

```c
WILiNFC_result_t WILiNFC_mfc_read_value(uint8_t block, int32_t *value);
WILiNFC_result_t WILiNFC_mfc_write_value(uint8_t block, int32_t value);
WILiNFC_result_t WILiNFC_mfc_increment_value(uint8_t block, int32_t delta);
WILiNFC_result_t WILiNFC_mfc_decrement_value(uint8_t block, int32_t delta);
WILiNFC_result_t WILiNFC_mfc_transfer_value(uint8_t block);
WILiNFC_result_t WILiNFC_mfc_restore_value(uint8_t block);
```

Increment and decrement stage a change on the card; `transfer_value` commits
it to the block, and `restore_value` loads a block's value into the card's
staging area (so `restore` then `transfer` to another block of the same
sector copies a value).

### A worked write: poll, authenticate, write, verify

This writes 16 bytes to block 4, the first block of sector 1 (a data block),
of a card you own whose Key A for sector 1 is the factory key, all `FF`
bytes:

```c
#include <string.h>

static const uint8_t key_a[WILiNFC_MFC_KEY_SIZE] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t data[WILiNFC_MFC_BLOCK_SIZE] = "HELLO NFC CARD!";  /* 15 + NUL */

WILiNFC_card_t card;
if (WILiNFC_poll(&card) && card.type == WILiNFC_CARD_TYPE_MIFARE_CLASSIC) {
    WILiNFC_mfc_variant_t v = WILiNFC_mfc_variant_of(&card);
    uint8_t block = WILiNFC_mfc_sector_to_block(v, 1);              /* 4 */
    uint8_t back[WILiNFC_MFC_BLOCK_SIZE];

    WILiNFC_result_t r = WILiNFC_mfc_auth(1, WILiNFC_MFC_KEY_A, key_a);
    if (r == WILiNFC_OK) r = WILiNFC_mfc_write_block(block, data);
    if (r == WILiNFC_OK) r = WILiNFC_mfc_read_block(block, back);
    bool verified = (r == WILiNFC_OK) && memcmp(back, data, sizeof data) == 0;
    /* r says why it failed: AUTH_FAILED (wrong key), PROTECTED (the access
       bits forbid the write), CARD_LOST (read the block back: it may or may
       not have been written). */
}
```

- When the key is not known in advance, call `WILiNFC_mfc_dict_run()` in
  place of `WILiNFC_mfc_auth()`; the write then authenticates from the key
  slots by itself.
- Never pick a sector trailer by mistake: compare the block with
  `WILiNFC_mfc_trailer_block(v, sector)` before writing.
- The card must stay flat against the back of the FreeWili, and still, from
  the poll to the read-back.

### Known-key dictionary and key cache

A **dictionary** is a plain text file of candidate keys, one 12-hex-digit key
per line (for example `FFFFFFFFFFFF`); `#` starts a comment.
`WILiNFC_mfc_dict_run` tries every key against each sector key that is not
yet known and stores the ones that work in the key slots, so you can read a
card whose keys are in your list. It does not discover keys that are not in
the dictionary: such a sector stays locked to you.

The library ships no dictionary and never creates or writes one: you put the
file on the SD card yourself, for example from a PC, or from your app with
`ow_sd_put_mem()`. The format is plain text, one key of exactly 12 hex digits
per line (upper or lower case), at most 31 characters per line; `#` starts a
comment and empty lines are skipped. With a NULL path the library reads the
two fixed files `/nfc/assets/mf_classic_dict.nfc` and
`/nfc/assets/mf_classic_dict_user.nfc` (see [Storage paths](#storage-paths)).
Any other path you pass is read the same way.

```c
WILiNFC_result_t WILiNFC_mfc_dict_run(const char *dict_path,
                                      void (*progress)(uint8_t sector, uint16_t found, uint16_t total));
WILiNFC_result_t WILiNFC_mfc_key_cache_load(void);
WILiNFC_result_t WILiNFC_mfc_key_cache_save(void);
```

- `dict_path` is your dictionary file. NULL means the two fixed dictionaries,
  read in turn (see [Storage paths](#storage-paths)). Found keys go into the
  key slots and the card's cache file is rewritten. `progress` may be NULL; it
  is called after each sector with the sector number, keys found so far and
  keys needed. It returns `WILiNFC_OK` if at least one key is known
  afterwards and `WILiNFC_AUTH_FAILED` if none is.
- The **key cache** is one file per card. `key_cache_load` reads the selected
  card's file into the key slots. `key_cache_save` writes every key found so
  far; with no key found it writes nothing and returns `WILiNFC_OK`. The
  library also loads the cache when a card is selected and saves it when the
  card leaves.
- All three return `WILiNFC_STORAGE_UNAVAILABLE` when storage is not
  available. See [Timing](#timing) for how long a dictionary run takes.

### Full-card dump

```c
typedef struct {
    uint8_t sector_count;                        /* sectors in the dumped card */
    bool sector_known[WILiNFC_MFC_MAX_SECTORS];  /* true: a key was available */
    uint32_t dump_len;                           /* bytes written into buf */
} WILiNFC_mfc_dump_result_t;

WILiNFC_result_t WILiNFC_mfc_dump(uint8_t *buf, uint32_t buf_len,
                                  WILiNFC_mfc_dump_result_t *out);
```

`WILiNFC_mfc_dump` reads every sector into `buf` with the keys in the key
slots and returns when the dump is complete. `buf_len` must be at least
blocks-in-card x 16: 320 bytes for a Mini, 1024 for a 1K card, 4096 for a 4K
(size it with `WILiNFC_mfc_sector_count` and `WILiNFC_mfc_blocks_in_sector`).
A sector with no usable key, and a block that could not be read, is
zero-filled; a sector with no usable key has its `sector_known` entry false.

It returns `WILiNFC_OK` if at least one sector had a usable key,
`WILiNFC_AUTH_FAILED` if none had, and `WILiNFC_BAD_ARGUMENT` for a NULL `buf`
or a `buf_len` that is too small (nothing is written then). `out` may be NULL;
when given it is filled on every return, and `dump_len` is 0 when nothing was
written. Typical order: `WILiNFC_mfc_dict_run`, then `WILiNFC_mfc_dump`.

A dump is not part of a saved card: `WILiNFC_card_save` writes only the
card's identity. To keep a dump, write the buffer yourself, for example as a
binary file of `dump_len` bytes in block order with `ow_sd_put_mem()` under
`/appdata/<your-app-name>/`. Keep `sector_known` with it if you need to tell a
zero-filled sector from one that really holds zeros.

## Emulation

```c
WILiNFC_result_t WILiNFC_emulate_begin(const uint8_t *uid, uint8_t uid_len,
                                       const uint8_t atqa[2], uint8_t sak);
WILiNFC_result_t WILiNFC_emulate_card(const WILiNFC_card_t *card);
void WILiNFC_emulate_stop(void);
bool WILiNFC_emulating(void);
WILiNFC_result_t WILiNFC_emulate_status(WILiNFC_emu_status_t *out);
```

`emulate_begin` presents a UID (`uid_len` 4, 7 or 10), ATQA (2 bytes) and SAK
to an external reader: hold the back of the FreeWili flat against the reader
and keep it still. Its errors are `WILiNFC_BAD_ARGUMENT`,
`WILiNFC_NOT_RUNNING`, `WILiNFC_BUSY` (during a raw session) and
`WILiNFC_FAILED`. `emulate_card` does the same from any card record: one
from `WILiNFC_poll`, `WILiNFC_last_card` or `WILiNFC_card_load`. Only its
`uid`, `uid_len`, `atqa` and `sak` are used. Emulation has no time limit: it
runs until `emulate_stop` or `WILiNFC_stop`. While emulating, reader and
MIFARE calls return `WILiNFC_BUSY` and `WILiNFC_poll` reports nothing;
`emulate_stop` resumes reading and is a no-op when not emulating.

Keep calling `WILiNFC_service()` on every loop pass while emulating, as at any
other time: the front end answers the external reader by itself, and the
service calls record the selection that `reader_engaged` reports.

### What emulation can and cannot do

Emulation presents **only the card's identity**: the UID, ATQA and SAK that a
reader sees when it first detects a card (the NFC-A anticollision step). A
reader that only checks the UID will see a card. It does **not**:

- answer MIFARE Classic authentication (it has no keys and no memory);
- serve any sectors or blocks: a reader that tries to read data gets nothing;
- act as an ISO-DEP / Type 4 card or hold an NDEF message.

If a system checks more than the UID, emulation will not satisfy it. Emulate
only cards you own or have permission to test.

### Status

`emulate_status` fills `WILiNFC_emu_status_t`: `active` (an identity is being
presented) and `reader_engaged` (an external reader has selected the identity
since emulation began; it stays true until the next `emulate_begin` or
`emulate_stop`).

## Saved cards

```c
WILiNFC_result_t WILiNFC_card_save(const char *path, const WILiNFC_card_t *card);
WILiNFC_result_t WILiNFC_card_load(const char *path, WILiNFC_card_t *out);
```

Both go through the registered file hooks (see
[Saving files](#saving-files-the-file-hooks)). They return
`WILiNFC_BAD_ARGUMENT` for a NULL argument, `WILiNFC_STORAGE_UNAVAILABLE`
when storage is not available, and `WILiNFC_FAILED` when the file cannot be
written or parsed or a hook reports an error. `WILiNFC_card_save` also returns
`WILiNFC_BAD_ARGUMENT` for a card whose `uid_len` is 0 or above 10.

- Neither call needs the reader: both work before `WILiNFC_start()` and
  during emulation, as long as file hooks are registered.
- `WILiNFC_card_save` calls `mkdir` on `/nfc` before it opens your path, so
  saving creates `/nfc` on the SD card even when your path is under
  `/appdata`.
- It never creates the directory of your path: create it yourself (the
  example's `ow_sd_io_ensure_dir()` creates `/appdata/hello_wilinfc`).

### The record

The file is the plain-text Flipper Zero `.nfc` format, so other tools that
read it can use a card you saved here. It is a list of `Key: value` lines:

| Field | Meaning |
|---|---|
| `Filetype`, `Version` | Format header (`Flipper NFC device`, version number). |
| `Device type` | Card family, for example `ISO14443-3A`, `ISO14443-4A`, `Mifare Classic`, `NTAG/Ultralight`. |
| `UID` | The card UID as hex bytes separated by spaces. |
| `ATQA` | The two ATQA bytes as hex. |
| `SAK` | The SAK byte as hex. |

WILiNFC writes the identity fields above. The card type survives only as far
as the format expresses it: MIFARE Classic and ISO-DEP (Type 4) round-trip;
other types are stored as a plain ISO 14443-3A card and load back as
`WILiNFC_CARD_TYPE_UNKNOWN` (an Ultralight/NTAG file loads as `T2T`). The ATS
is filled on load when the file has one.

## Saving files: the file hooks

WILiNFC has no filesystem. Saved cards, the MIFARE Classic key cache and
dictionary runs go through file hooks you register with
`WILiNFC_set_file_io()`, one file at a time (the library never has two files
open):

```c
typedef struct {
    void *ctx;
    bool (*open)(void *ctx, const char *path, bool write);
    int  (*read_line)(void *ctx, char *buf, int max_len);
    bool (*write_line)(void *ctx, const char *line);
    bool (*close)(void *ctx);
    bool (*mkdir)(void *ctx, const char *path);
} WILiNFC_file_io_t;

void WILiNFC_set_file_io(const WILiNFC_file_io_t *io);
```

The struct is copied, so it need not outlive the call. NULL disables storage.

**Required hooks:** `open`, `read_line`, `write_line` and `close`. If any of
them is NULL, or `WILiNFC_set_file_io` was never called or was given NULL,
every storage call returns `WILiNFC_STORAGE_UNAVAILABLE`. `mkdir` is
optional: when it is NULL no directory is created, so directories must
already exist.

### Which hooks each operation calls

- **Reading a file** (`WILiNFC_card_load`, `WILiNFC_mfc_key_cache_load`, the
  cache load when a MIFARE Classic card is selected, `WILiNFC_mfc_dict_run`
  reading a dictionary): `open` with `write` false, `read_line` one line at a
  time (the library may stop before the end of the file), `close`.
- **Writing a file** (`WILiNFC_card_save`, `WILiNFC_mfc_key_cache_save`, the
  cache save when a card leaves, the cache rewrite by `WILiNFC_mfc_dict_run`):
  `mkdir` on the fixed directory listed under [Storage paths](#storage-paths)
  (`/nfc` for `WILiNFC_card_save`, `/nfc/cache` for the cache), `open` with
  `write` true, `write_line` for each line, `close`. The directory of a path
  you pass is never created: create it yourself.

### The hook contract

- `open(ctx, path, write)`: `write == false` opens an existing file for
  reading and returns false if it does not exist. `write == true` creates the
  file or truncates it.
- `read_line(ctx, buf, max_len)`: write the next line into `buf`,
  NUL-terminated, at most `max_len` bytes including the NUL. The library passes
  `max_len` = 128 (`WILiNFC_LINE_MAX`) for saved-card and key-cache files and
  32 for dictionary files, so lines are at most 127 and 31 characters; a
  longer line is not supported. The line may end in LF, CRLF or nothing; the
  library strips the ending. Return **> 0** when a line was read, **0** at end
  of file, and a **negative value** for an I/O error. An error stops the read,
  and the storage call returns `WILiNFC_FAILED`.
- `write_line(ctx, line)`: write the text exactly as given, adding nothing.
  The library sends each line without its end and then calls `write_line`
  once more with `"\n"`. Return false on error; the storage call then returns
  `WILiNFC_FAILED`.
- `close(ctx)`: called exactly once after every `open` that returned true,
  including after a read or write error. false for a file opened for writing
  means the data may not be stored, and the storage call returns
  `WILiNFC_FAILED`; for a file opened for reading the result is ignored.
- `mkdir(ctx, path)`: create one directory. Return true if it was created or
  already exists. The library ignores the result and goes on to open the
  file.

Paths are absolute, use `/`, and are at most `WILiNFC_PATH_MAX` - 1 (127)
characters. The library passes either the path you gave it or one of the
fixed paths under [Storage paths](#storage-paths).

**Hooks may block.** The card is not tracked while a hook runs, so a card may
be reported removed afterwards, and the calling operation takes longer by the
hook's duration. Keep hooks short where you can: an SD call normally takes
tens of milliseconds, and capping it at about one second is acceptable.

### Storage paths

The library reads or writes only these fixed paths, plus the paths you pass
to `WILiNFC_card_save`, `WILiNFC_card_load` and `WILiNFC_mfc_dict_run`. The
paths are fixed so that key caches and dictionaries work the same on every
FreeWili device.

| Path | Use |
|---|---|
| `/nfc` | `WILiNFC_card_save` calls `mkdir` on it before it opens your path. |
| `/nfc/cache/<UID>.nfc` | The MIFARE Classic key cache: one file per card. `<UID>` is the card's UID in upper-case hex with no separators (for example `/nfc/cache/04A1B2C3.nfc`). The file is Flipper "NFC keys" text (`Key A sector N: XX XX XX XX XX XX` lines). `WILiNFC_mfc_key_cache_save` calls `mkdir` on `/nfc/cache` only (not on `/nfc` first) and then truncates and rewrites the file; the save when a card leaves and the rewrite by `WILiNFC_mfc_dict_run` do the same. `/nfc` must therefore already exist: `WILiNFC_card_save` creates it, or create `/nfc` yourself before relying on the cache. `WILiNFC_mfc_key_cache_load` reads the file. |
| `/nfc/assets/mf_classic_dict.nfc`, `/nfc/assets/mf_classic_dict_user.nfc` | Key dictionaries, one 12-hex-digit key per line; `#` starts a comment. `WILiNFC_mfc_dict_run` with a NULL path reads the first file, then the second. The library never writes either file. |

`/nfc` is the deliberate interoperability exception to the rule that
app-owned data lives under `/appdata/<app>/`: the folder is shared with the
device on purpose, so a key cache or dictionary on the SD card works in every
app and on the device. Files your app chooses itself, such as saved cards,
still belong under `/appdata/<your-app-name>/`. See
[App-owned data](../app-storage.md#app-owned-data).

### An SD-card backend (`apps/hello_wilinfc/ow_sd_io.c`)

The SD card belongs to the main CPU; an app reaches it through OneWili with
the `ow_sd_*` calls. `apps/hello_wilinfc/ow_sd_io.c` and `ow_sd_io.h` are a
sample backend to copy and adapt. The example saves cards under
`/appdata/hello_wilinfc/` (the app-owned storage rule: use
`/appdata/<your-app-name>/`). In outline:

```c
static bool io_open(void *ctx, const char *path, bool write) {
    return ow_sd_open(s_dev, &s_file, path, write ? OW_SD_WRITE : OW_SD_READ) == OW_OK;
}
static bool io_write_line(void *ctx, const char *line) {   /* written as given */
    return ow_sd_write(&s_file, line, strlen(line)) == OW_OK;
}
static bool io_close(void *ctx)  { return ow_sd_close(&s_file) == OW_OK; }
/* read_line buffers a block of the file and returns one NUL-terminated line
   per call: > 0 for a line, 0 at end of file, -1 for an SD read error or a
   line too long for buf. mkdir returns true when the directory already
   exists. */
```

Using it:

```c
#include "onewili.h"
#include "onewili_fwgui.h"
#include "onewili_sd.h"
#include "input/app_recovery_onewili.h"
#include "ow_sd_io.h"
static ow_device dev;                  /* about 37 KB: never on the stack */
...
if (fw2_app_recovery_open_onewili(&dev) == OW_OK && fw2_app_recovery_wrap_sd() == OW_OK) {
    ow_sd_set_timeout_ms(1000);        /* from here an SD call gives up after about a second */
    ow_sd_io_init(&dev);
    if (ow_sd_io_ensure_dir())         /* creates OW_SD_IO_DIR; rename it for your app */
        WILiNFC_set_file_io(ow_sd_io_get());
}
```

The SD client needs `onewili_fwgui` in `target_link_libraries` and the
`SDCARD` zone in `POWER_ZONES` (see
[CMakeLists.txt for an NFC app](#cmakeliststxt-for-an-nfc-app)), and the main
CPU must run the stock FreeWili 2 firmware. `apps/hello_wilinfc` calls
`ow_sd_set_timeout_ms(1000)` right after the SD wrap succeeds and before its
first SD call, so every later SD call returns within about a second even when
the main CPU does not answer. Opening the link itself runs under the default
timeout. Cards are not tracked while an SD call is in progress (up to about a
second per call).

## The raw channel

```c
WILiNFC_result_t WILiNFC_raw_begin(void);
WILiNFC_result_t WILiNFC_raw_end(void);
WILiNFC_result_t WILiNFC_raw_field(bool on);
WILiNFC_result_t WILiNFC_raw_reg_write(uint8_t addr, uint8_t value);
WILiNFC_result_t WILiNFC_raw_reg_read(uint8_t addr, uint8_t *out);
WILiNFC_result_t WILiNFC_raw_cmd(uint8_t command);
WILiNFC_result_t WILiNFC_raw_transceive(const uint8_t *tx, uint16_t tx_len, uint8_t tx_last_bits,
                                        WILiNFC_raw_flags_t flags,
                                        uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                                        WILiNFC_raw_status_t *status);
```

**The raw channel is expert use, outside the supported contract.** These calls
pass register values, command bytes and frames to the NFC front end
unchanged, and the library does not define what they do. Nothing else in
this document needs the raw channel. The session rules below say how the raw
channel shares the front end with the rest of the library.

Rules:

- `raw_begin` takes exclusive ownership: it stops polling and any emulation,
  and the state becomes `WILiNFC_STATE_NO_SIGNAL`. Until `raw_end`, reader,
  MIFARE and emulation calls return `WILiNFC_BUSY`. `raw_end` ends the session
  and resumes reading.
- Every other raw call returns `WILiNFC_RAW_NOT_ACTIVE` unless a raw session
  is active, and `WILiNFC_NOT_RUNNING` before `WILiNFC_start`.
- `raw_transceive` sends `tx` (`tx_len` bytes, at most `WILiNFC_RAW_TX_MAX` =
  240) and receives up to `rx_cap` bytes (clamped to `WILiNFC_RAW_RX_MAX` =
  129). `tx_last_bits` is 0 for whole bytes, otherwise the valid bits in the
  final byte (`tx_len` still counts that byte, so the 240-byte cap applies to
  it too). Of `flags`, only `WILiNFC_RAW_APPEND_CRC` is acted on: it appends a
  check value on transmit. The call waits at most 50 ms for an answer;
  `status.timeout` means nothing answered in that time.
- `WILiNFC_OK` from `raw_transceive` means the exchange ran. Inspect
  `WILiNFC_raw_status_t` (`ok`, `timeout`, `collision`, `crc`, `parity`,
  `framing`) for the outcome. Receive check-value problems are reported only
  in `status.crc`.

## Concepts: UID, ATQA and SAK

When a card enters the reader's field the reader runs a short exchange called
**anticollision and selection** (the NFC-A / ISO 14443-3A step). It yields
three facts, which WILiNFC returns in `WILiNFC_card_t`:

- **UID**: the card's serial number, 4, 7 or 10 bytes (`uid_len`). Many
  access systems identify a card by this alone. On most MIFARE cards it is also
  stored in block 0. Some cards present a random UID on purpose.
- **ATQA**: two bytes the card sends first, saying what size UID it has and
  roughly what family it belongs to.
- **SAK**: one byte sent when the card is selected. Its bits say whether the
  card also speaks the higher-level ISO-DEP protocol and, together with the
  ATQA, which family it is: for example `SAK 08` is MIFARE Classic 1K, `SAK 18`
  is Classic 4K, `SAK 09` is Mini, `SAK 00` is Ultralight/NTAG and `SAK 20` is a
  card that supports ISO-DEP.

If the card supports ISO-DEP (`is_iso_dep`), the reader also asks for its
**ATS** (answer to select), returned in `ats` / `ats_len`. WILiNFC combines
ATQA and SAK into `WILiNFC_card_type_t`.

## Blanks to buy

- **MIFARE Classic 1K** (or 4K): the standard practice card for the sector and
  key model. Buy ordinary ones to learn on.
- **NTAG213/215/216 or MIFARE Ultralight**: cheap Type 2 tags for NDEF
  messages. Reading identifies them; their memory is not a MIFARE Classic
  memory, and the supported calls do not read or write it.
- **"Magic" or "changeable-UID" MIFARE Classic cards**: if you want to write
  block 0 (copy a UID onto a blank), you need these; ordinary cards reject
  block 0 writes.

Always use cards you own and want to experiment with.

## Dependencies

- **I2C1, and nothing else.** The library uses the board's I2C1, which
  `board_init()` sets up and which other BSP drivers also use. It uses the
  bus as `board_init()` leaves it and does not reconfigure it. `board_init()`
  must have run before `WILiNFC_start()`. When the bus needs recovery the
  library calls the BSP's `board_i2c1_recover()` (an app built without the
  BSP must provide that function). Do not reconfigure I2C1 yourself while
  WILiNFC is running, and drive I2C1 from one core only.
- **Power zone `NFC_RFID` (zone 13).** The front end is off unless this zone
  is on. List `NFC_RFID` in `POWER_ZONES`; `fw2_app_recovery_init()` then
  keeps it awake and waits for it, so no `picpwr_keep_awake()` call is needed.
  An app that does not call `fw2_app_recovery_init()` must keep the zone awake
  with `picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_NFC_RFID))` for as long
  as the reader runs, wait until `picpwr_rails()` reports it powered, and keep
  calling `picpwr_task()` in its main loop (all declared in `input/picpwr.h`).
- **No PIO, DMA, PWM, ADC or interrupts.** WILiNFC is polled: call
  `WILiNFC_service()` on every main-loop pass.
- **Footprint.** The library adds about 29 KB of code and constants and about
  2.4 KB of data to an app that uses every call; a loaded app holds all of it
  in SRAM. The library never allocates from the heap, and a C application
  that links `pico_stdlib` needs no extra library.

Exclusions:

- **One NFC user at a time.** Do not run a second NFC driver or your own code
  on the same front end alongside WILiNFC (except through the raw channel,
  which takes exclusive ownership).

Coexistence:

- **A 125 kHz RFID driver may run in the same app.** The same zone powers the
  125 kHz RFID front end, driven by either the WILiRFID library
  ([wilirfid.md](wilirfid.md)) or the open `rfid_*` driver
  ([rfid.md](rfid.md)), never both. The zone is shared, not owned: declare it
  once in `POWER_ZONES` and both front ends are powered. Neither 125 kHz RFID
  driver uses I2C1, so one app may run WILiNFC with either of them, calling
  each driver's service function in its main loop. Each one's blocking calls
  delay the other's servicing. With an NFC card in the field, one
  `WILiNFC_service()` call can take about 50 ms, longer than the RFID
  library's 20 ms service interval. A 125 kHz card presented briefly at the
  same time may be missed. Whether the two fields disturb each other has not
  been verified on hardware. See
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

### `WILiNFC_state_t`

| State | Meaning |
|---|---|
| `WILiNFC_STATE_OFF` | Not started, or stopped. |
| `WILiNFC_STATE_READY` | Running; looking for a card or holding one. This is the state after a successful start. |
| `WILiNFC_STATE_NO_SIGNAL` | Running, but the reader is not looking for cards (for example during a raw session). |
| `WILiNFC_STATE_FAULT` | The front end is absent or failed to start, or the reader failed later and could not be restarted. |
| `WILiNFC_STATE_EMULATING` | Presenting a card identity to an external reader. |

`WILiNFC_state_name(state)` returns a short constant name; it is never NULL.

### `WILiNFC_result_t`

`WILiNFC_result_name(result)` returns the constant name (never NULL).
`WILiNFC_last_error()` returns the reason for the most recent failure, or
`WILiNFC_OK` if there was none.

| Code | Meaning | What to do |
|---|---|---|
| `WILiNFC_OK` | Success. | Carry on. |
| `WILiNFC_NOT_RUNNING` | `WILiNFC_start()` has not succeeded. | Start the reader; if start fails, see [Start failures](#start-failures). |
| `WILiNFC_BUSY` | Emulation or a raw session is using the front end. | Stop it (`WILiNFC_emulate_stop()` or `WILiNFC_raw_end()`), then retry. |
| `WILiNFC_BAD_ARGUMENT` | A NULL pointer, bad length or out-of-range sector/block. | Fix the call: size buffers and ranges with the geometry helpers. |
| `WILiNFC_UNSUPPORTED` | The selected card cannot do this (for example a MIFARE call on a card that is not MIFARE Classic). | Check `card.type` before calling; use a card of the right type. |
| `WILiNFC_NO_CARD` | No card is selected. MIFARE calls also return it for a MIFARE Classic card whose variant is `WILiNFC_MFC_UNKNOWN`. | Wait for `WILiNFC_poll()` to report a card, then retry. A card of unknown variant cannot be used with the MIFARE calls. |
| `WILiNFC_CARD_LOST` | The card stopped answering. | Hold it flat against the back of the FreeWili again, keep it still, and retry. After a lost write, read the block back: it may or may not have been written. |
| `WILiNFC_AUTH_FAILED` | No key, or a wrong key, for the sector. | Supply a key: `WILiNFC_mfc_auth()`, `WILiNFC_mfc_key_cache_load()` or `WILiNFC_mfc_dict_run()`. A sector whose key you do not have cannot be read. |
| `WILiNFC_CRC_ERROR` | A corrupted exchange. | Retry; hold the card flat against the back of the FreeWili and keep it still. |
| `WILiNFC_COLLISION` | More than one card in the field. | Remove the other cards and hold one flat against the back of the FreeWili. |
| `WILiNFC_PROTECTED` | The card refused the operation: its access bits, or a protected block (for example block 0 of a card that is not a magic card). | Do not retry. Check `WILiNFC_mfc_access_bits()`; the operation may not be allowed at all. `AUTH_FAILED` instead means the key was missing or wrong. |
| `WILiNFC_NO_FRONT_END` | The NFC hardware did not answer on the bus (not "no card"). | Check the `NFC_RFID` power zone, then the board. |
| `WILiNFC_INIT_TIMEOUT` | The NFC hardware did not become ready. | Power cycle the board. |
| `WILiNFC_API_VERSION_MISMATCH` | Header and library are different releases. | Rebuild against the matching `WILiNFC.h`. |
| `WILiNFC_STORAGE_UNAVAILABLE` | No usable file hooks are registered. | Register hooks with `WILiNFC_set_file_io()`, with `open`, `read_line`, `write_line` and `close` all set. |
| `WILiNFC_RAW_NOT_ACTIVE` | A raw call without `WILiNFC_raw_begin()`. | Call `WILiNFC_raw_begin()` first. |
| `WILiNFC_FAILED` | Any other failure, including a file hook error. | Check the storage (a hook returned an error, or the file cannot be written or parsed) or the card, retry once, and report it if it persists (see [Reporting a problem](#reporting-a-problem)). |

## Reporting a problem

Open an issue on the repository with:

- the library version, from `WILiNFC_version()`;
- the state, from `WILiNFC_state_name(WILiNFC_state())`;
- the last error, from `WILiNFC_result_name(WILiNFC_last_error())`;
- the card: its `type`, UID, ATQA and SAK, or the file `WILiNFC_card_save()`
  wrote for it;
- for a MIFARE Classic or storage call, which call it was and the result it
  returned;
- for emulation, `reader_engaged` and the make of the external reader;
- what the app was doing, and which other drivers it starts and in what order.
