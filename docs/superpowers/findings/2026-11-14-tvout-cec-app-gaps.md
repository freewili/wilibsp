# TV output and CEC: gaps app developers found (2026-11-14)

Six apps were written against the TV output and CEC drivers by developers
working from the BSP documentation only. This record lists what their reports
changed in the BSP, how each change was checked, and what is still open.
**Nothing here has been run on a board.** Every item below is checked by
host tests and builds only; the bench checks are listed at the end.

## Changed

| Gap | Change | Checked by |
|---|---|---|
| The OneWili link's SRAM cost was undocumented | AGENTS.md and `docs/drivers/tvout.md` give 115,866 bytes (measured from the `hello_sdcard` and `toggleled` linker maps) and its parts; `FW2_ONEWILI_TEXT_EVENTS` exposes the one existing size option | Map files; `ow_device` 37,168 bytes at 8, 8,496 at 1; 0 refused at configure |
| No scaled picture | `tvout_start_2x()`: up to 320x240 filling the TV, page flip unchanged | `tests/test_dvi_frame.cpp` (frame decoded with two encoder shifts per word), `tests/test_tvout_list.c` |
| PSRAM picture ergonomics | Documented `__uninitialized_psram` framebuffer and `st7796_blit_rect()` from the uncached pointer for the LCD | `hello_tvout_ring` builds; UF2 check passes |
| "No TV" vs "cannot detect" | Documented: the keyboard frame carries nothing that tells them apart | Reading of the frame and the coprocessor's firmware version reply |
| CEC physical address is a guess | `CEC_EV_ROUTING` carries the address a TV announces when switched by its own remote; recipe in `docs/drivers/cec.md` | `tests/test_cec_watch.c` |
| PSRAM line ring linked into every TV app | `tvout_start_psram()`; `tvout_start()` is SRAM only | `nm`: `hello_tvout` has no `s_lines`, `hello_tvout_ring` has it |
| TV sound needs a top-up every 2.7 ms | 2048-frame buffer (42.7 ms), `tvout_audio_room()`, interrupt-context rule, sound on/off without a restart | `tests/test_tvout_audio.cpp` |
| CEC held key vs tap | `CEC_EV_KEY_REPEAT`, key-up time-out flag, `time_ms` on every event | `tests/test_cec_watch.c` |
| CEC vendor frames refused | `cec_accept_vendor_frames()`, `CEC_EV_VENDOR` with the whole frame | `tests/test_cec_watch.c` |
| About restore API failed the contract test | The test accepts `fw2_app_about_use_lcd_restore()` | `tests/test_app_contract.py` |
| AGENTS.md invariant 2 stale | Rewritten: `no_flash` apps can use `__uninitialized_psram` | `hello_tvout_ring` and the About backup build that way |

## Open

- **Plug-detect capability.** Needs a coprocessor firmware change (a frame bit
  or a version raised with the TTL input setting); nothing the display CPU can
  read separates the cases today.
- **Physical address from the EDID.** Needs a MAIN-side OneWili command; the
  routing event is a user-assisted substitute, and which TVs announce manual
  input changes is not known.
- **HSTX two-shift encoding** behind `tvout_start_2x()` is from the RP2350
  register description and the host model; not seen on a TV.
- **TV sound pull callback.** Not added: it would run app code in the
  top-priority feeder tick next to CEC's timing margin. Changing audio framing
  still needs a restart (the running list would have to be rebuilt).
- **OneWili buffers** other than the text-event queue are fixed in the OneWili
  submodule, and its README was not changed here; an SD-only link would be a
  OneWili change.
- **Reported but not taken up in this pass:** vertical-blank wait or scan row,
  a per-driver DMA table and the combined interrupt budget (PSRAM reader,
  sound feeder, CEC), a CEC standalone `<Image View On>`, request priority or
  cancel, an active-source getter, speaker streaming and the I2S DMA-claim
  panic, IR transmit tail and docs, a larger font, asynchronous OneWili calls
  and RTC behaviour, `fw build -j`, the missing-submodule configure message,
  the new-app skill, the private path in `uartkbd_parse.h`, a string compare
  in `tests/test_util.h`, a BSP wire-order colour macro, CEC source address
  per request and system audio control.

## Bench checks still to run

`hello_tvout` 2x mode (fills the screen, `FLIP LATE 0 MISS 0 PASS`);
`hello_tvout_ring` LCD mirror (`LATE 0 PASS` with the mirror on);
`hello_tvsound` written tone (`UNDERRUN 0 PASS`); `hello_cec` route lines,
key repeats with their spacing, vendor frames with no `<Feature Abort>` sent
back.

## Note, 2026-11-16

The bench apps named above were folded into one example per driver: `apps/hello_tvout` is now a short introduction (320x240 page flip on TV and LCD, 2x, plug events, a written tone) with no PASS/FAIL lines, and `hello_tvout_psram`, `hello_tvout_ring` and `hello_tvsound` were removed. The results recorded here were measured with the apps as they were then (in git history before commit "chore(apps): one TV output example app, as for the other drivers").
