# Emberward — design note

## Pitch

A living core floats in the middle of the TV. Sparks fly at it from eight
directions. You turn a quarter-circle ward around the core: block the red
embers, let the golden glims through to feed the core. Feeding the core
charges a flare that burns every spark on screen. Waves get faster and
denser until the core goes dark.

## Rules

- Eight lanes (octants) around the core. The ward covers two adjacent lanes.
- LEFT / RIGHT turn the ward one lane. UP / DOWN flip it half a turn.
  OK (or the remote's SELECT) fires the flare when it is full.
- **Ember** (red): block it, 10 points x multiplier. Reaches the core: lose a
  life, combo broken, a short grace period follows.
- **Glim** (gold): let it in, 25 points x multiplier and +20 % flare. Block
  it and it shatters: combo broken, nothing else lost.
- **Dart** (cyan, wave 3 on): a fast ember, 20 points when blocked.
- **Heart** (green, wave 4 on, rare, only below five lives): let it in for a
  life.
- Combo counts consecutive correct calls; the multiplier is 1 + combo / 8,
  at most x5.
- **Flare** (full meter): every spark on screen is resolved in your favour,
  embers burn for 15 x multiplier, glims are absorbed.
- A wave is `12 + 4 x wave` sparks. Each wave is faster, spawns sooner, and
  from wave 2 sends adjacent pairs (two embers, or an ember beside a glim)
  that one ward position can answer. Wave clear bonus: 100 x wave.
- Three lives, at most five. Game over at zero; a top-five score asks for
  three initials and is saved.

## Screens

- **TV (400x300, centred in the 640x480 signal)**: title with the
  high-score table, the game field, wave banners, game over and initials
  entry.
- **LCD (companion)**: score, wave, combo, lives, flare meter, the high-score
  table, TV / remote status, and the five button labels. GREY switches to an
  **LCD view** that mirrors the TV picture on the LCD, for play without a TV
  (touch: left third turns left, right third turns right, middle is OK).

## Controls

| Action | Front panel | Nav pad | TV remote (CEC) |
|---|---|---|---|
| Turn left / right | YELLOW / BLUE | LEFT / RIGHT | LEFT / RIGHT |
| Flip | — | UP / DOWN | UP / DOWN |
| Flare / start / confirm | GREEN | OK / centre | SELECT, PLAY, green key |
| Pause | RED | CANCEL | PAUSE, STOP, EXIT, red key |
| View TV / LCD | GREY | — | — |

The labels are also touch targets.

## Technical plan

- RAM app (`fw2_display_app`), `clk_sys` left at 250 MHz (no ADC or native
  USB needed, so the stopped `clk_usb` / `clk_adc` do not matter).
- **Picture size and the page flip.** `tvout` does not scale: a 320x240
  picture fills a quarter of the TV. Two 320x240 buffers (the page-flip
  case) are the largest that fit SRAM, and the SD link costs about 116 KB of
  SRAM on top, so a TV-sized pair is out of reach. Emberward uses **one
  400x300 buffer in SRAM** (240,000 bytes) and gets the page flip's result
  another way: `tvout_show()` of that same buffer returns when the TV has
  just left the picture rows (about 7 ms before it reads row 0 again), and the
  frame is then composed in 10-row bands in an 8 KB strip and copied into the
  picture top to bottom, every pixel written once with its final value. The
  copy starts with that 7 ms lead and runs faster than the scan, so the TV
  should never show a half-drawn frame. Frames that take longer than 10 ms
  are counted over RTT.
- Game logic runs at a fixed 60 Hz step from `time_us_64()`, so a slow frame
  never changes game speed.
- Sound, two outputs, one at a time:
  - **TV** (a TV reported by the hot-plug line, TV sound setting on): the
    picture is restarted with audio framing and `tvout_audio_start()`. A pure
    mixer (`ew_mix`) plays up to four effects at once, the same note tables
    as the speaker's sequencer, synthesised sample by sample at 48 kHz with
    phase accumulators and stepped every 800 frames (1/60 s). The loop
    never writes the feeder's 2048-frame buffer (42.7 ms): a 1 ms repeating
    timer (the SDK's default alarm pool on TIMER0; the BSP's audio and CEC
    alarms are on TIMER1) renders 32-frame blocks and calls
    `tvout_audio_write()` until it takes less than offered, keeping what it
    did not take for the next call. The loop posts effects into a
    single-producer, single-consumer queue the timer drains. Cost: a few
    microseconds a millisecond (estimate) against the 10 ms frame budget;
    the slow-frame counter over RTT still measures the frame.
  - **Speaker** otherwise: through the NAU88C10. The pure sequencer plays
    short square, pulse, triangle and noise notes; each note is a 512-frame
    ring-aligned buffer, a whole number of cycles, switched with
    `play_stop` + `play_loop`. Quiet amplitude (5000 of 32767), speaker
    powered down when sound is off or the TV carries it.
  - Audio framing is on only while a TV is reported and the setting is on:
    a plain DVI monitor may blank with it. Unplugging restarts the picture
    without framing.
- High scores and settings in `/appdata/emberward/scores.bin` over
  `ow_sd_*` (52 bytes, checksummed), loaded once at boot and written after an
  initials entry; the game plays without the SD link, scores kept in RAM.
- CEC: `cec_start("Emberward")`, wake the TV and switch input once the
  address is claimed, remote keys as input, pause on TV standby or input
  change, unplugging the TV pauses too. The physical address is the BSP's
  (`docs/drivers/cec.md`, where it comes from): the saved one is loaded with
  `cec_saved_load()`, a learned one saved with `cec_saved_task()`; BLUE on
  the title screen overrides it with `cec_set_physical_address()` and saves
  it with `cec_saved_store()`.
- Pure logic, host-tested: game rules (`ew_game`), key repeat (`ew_input`),
  score table and save format (`ew_scores`), sound sequencer and note
  rendering (`ew_sfx`), TV mixer and its feed into a writer that takes a
  little at a time (`ew_mix`).
