# TV output (`bsp/display/tvout`)

640×480 at 60 Hz DVI out of the TV connector, over the RP2350 HSTX block
(GPIO 12–19). It shows an eight-colour-bar test picture, or the app's own
RGB565 framebuffer, centred with a black border: read in place from SRAM, or
from PSRAM through a ring of eight SRAM line buffers that an interrupt keeps
ahead of the TV. An SRAM picture can be shown at twice its size (a 320×240
buffer filling the screen) and page-flipped between two buffers, and
the module reports the TV being plugged in and removed. With audio
framing (opt-in) the picture carries stereo sound to a TV. The clock plan
leaves `clk_sys` alone.

**Hardware status.** Verified on the board 2026-11-14: the test picture and
clocks (250 MHz, 50 stop/start cycles), the SRAM picture with page flip and
plug detection, a PSRAM-executing app showing an SRAM picture, and the PSRAM
picture through the line ring. **Not yet run on a board:** the picture at 2x
(`tvout_start_2x()`); the PSRAM picture since it moved to
`tvout_start_psram()` (the bench run started it through `tvout_start()`);
the LCD mirror of a PSRAM picture with `st7796_blit_rect()`; and all sound,
the 2048-frame buffer and `tvout_audio_room()` included. See
`docs/superpowers/findings/2026-11-14-tvout-test-picture-e2e.md` (bars,
clocks, stop/start) and
`docs/superpowers/findings/2026-11-14-tvout-app-picture-e2e.md`
(framebuffer, page flip, plug detection, PSRAM-executing app) and
`docs/superpowers/findings/2026-11-14-tvout-psram-picture-e2e.md` (PSRAM
picture through the line ring) and
`docs/superpowers/findings/2026-11-14-tvout-sound-e2e.md` (sound). The same
scan-out lists and clock arrangement are what the stock DISPLAY firmware uses
for its TV output. Page flipping exists only here.

`bsp/display/hstx_dvi` (`docs/drivers/dvi.md`) is unchanged and still the
driver behind `hello_dvi` and the AgentIO DVI surface. The two own the same
block and pins: use one per app.

## Quick start

```c
#include "fw2.h"

static uint16_t fb[320 * 240];      // SRAM, RGB565 high byte first

int main(void) {
    board_init();
    fw2_app_recovery_init();
    draw(fb);
    if (!tvout_start(fb, 320, 240))
        DIAG("tv: %s\n", tvout_last_error());
    while (true) {
        fw2_app_recovery_task();
        tvout_plug_event_t ev = tvout_task();
        if (ev == TVOUT_PLUG_CONNECTED) DIAG("tv: connected\n");
        if (ev == TVOUT_PLUG_REMOVED)   DIAG("tv: removed\n");
        draw(fb);                    // the TV shows each change as it is made
    }
}
```

`apps/hello_tvout` is the example app: two 320×240 buffers in SRAM, each
frame drawn into the hidden one, page-flipped onto the TV with `tvout_show()`
and flushed to the LCD, so the TV and the LCD show the same picture. GREEN
switches the TV between 1x and 2x (`tvout_start_2x()`), YELLOW restarts the
picture with audio framing and writes a 500 Hz tone with
`tvout_audio_write()`, and the picture names the last plug event. The bench
runs that verified the driver (stop/start cycles, FIFO levels, the PSRAM
picture and its LCD mirror, a PSRAM-executing app, the tone channels) are
recorded in the findings files listed above; the apps they used were folded
into this one example on 2026-11-16. `apps/emberward` is the full worked
example: a game with its picture and sound on the TV.

## API (`bsp/display/tvout.h`)

| Call | What it does |
|---|---|
| `bool tvout_start_test(void)` | Eight vertical bars: white, yellow, cyan, green, magenta, red, blue, black. True if video runs (also if it already did). |
| `bool tvout_start(const uint16_t *pixels, int width, int height)` | The app's picture: `width`×`height` RGB565 pixels, high byte first, row `y` at `pixels + y * width`, centred, black around. `width` even and at most 640, `height` at most 480. The whole buffer in SRAM, read in place. If video already runs, nothing changes (stop first). |
| `bool tvout_start_psram(const uint16_t *pixels, int width, int height)` | The same for a buffer wholly in PSRAM, read through the line ring (`width` at most 480 by default; see "PSRAM picture"). Only apps that call it link the ring. |
| `bool tvout_start_2x(const uint16_t *pixels, int width, int height)` | The app's picture at twice its size: every pixel sent as two pixels and every row as two rows, `width` at most 320, `height` at most 240, centred, black around. Wholly in SRAM; a PSRAM picture is refused. `tvout_show()` flips it. |
| `uint16_t *tvout_psram_uncached(uint16_t *pixels)` | The uncached alias of a PSRAM address, to draw a PSRAM picture through. |
| `void tvout_core1_init(void)` | Call once on core 1, before it touches PSRAM, in an app that runs code there while a PSRAM picture shows. |
| `bool tvout_show(const uint16_t *pixels)` | Page flip: points every row at another buffer of the same size while the TV is outside the picture rows. Waits for that point, at most about three frames. On true, the previous buffer is no longer read. |
| `void tvout_stop(void)` | Stops video, frees the DMA channels, restores the clocks. |
| `bool tvout_running(void)` | Video is running. |
| `const char *tvout_last_error(void)` | Why the last start or flip was refused: the RFID reader is running, no free DMA channel, the picture does not fit the TV format, the pixels are not in SRAM, the video clock did not start, no picture is running, or the flip point never came. |
| `bool tvout_adc_available(void)` | `clk_adc` is running (and `clk_usb`, which runs under the same conditions); `board_die_temp_c()` checks it. |
| `unsigned tvout_fifo_level(void)` | Words in the HSTX FIFO now (0–8); 0 when stopped. Sample it to find the lowest level; 0 while running means it ran empty. |
| `void tvout_get_stats(tvout_stats_t *out)` | `flips` (rows re-pointed) and `flips_late` (rewrites that ended inside the margin before the first row, or later). For a PSRAM picture, since its start: `rows_late`, `row_reads`, `row_read_timeouts`, `row_read_us_max`, and `reader_us` over `reader_window_us`, the reader's share of its core. |
| `tvout_plug_event_t tvout_task(void)` | Reads the hot-plug flag; returns `TVOUT_PLUG_CONNECTED` or `TVOUT_PLUG_REMOVED` on the pass that settles a change, otherwise `TVOUT_PLUG_NONE`. Call after `fw2_app_recovery_task()` on every pass. |
| `bool tvout_tv_connected(void)` | The settled plug state. |
| `void tvout_set_auto(bool on)` | Automatic start, off by default (see below). |

## The picture

Pixels are RGB565 with the high byte first, the LCD's wire order
(`docs/drivers/display.md`), so one buffer can go to the LCD with
`st7796_flush_async()` and to the TV at the same time: both read SRAM. Pixels
go to the HSTX FIFO with 16-bit DMA transfers, one pixel per FIFO word.

Nothing is copied. A buffer drawn while the TV reads it shows partly drawn
frames: rows from before and after the change in one frame. Writing each
pixel once with its final value leaves only that row split. For no split at
all, draw into a second buffer and call `tvout_show()`, as `hello_tvout`
does.

The buffer must stay where it is while video runs and must lie wholly in
SRAM (`0x20000000`–`0x20082000`), or wholly in PSRAM with
`tvout_start_psram()` (next section); a flash address is refused. A
PSRAM-executing app (`fw2_psram_app()`) keeps ordinary `.bss` in SRAM, so a
static framebuffer there is in SRAM too, and its code leaves the rest of SRAM
free. A PSRAM-executing build of the bench app passed the same bench lines
as the SRAM build on the board (FIFO minimum at least 6 of 8), so its code
fetches from PSRAM did not starve the HSTX feed there
(`docs/superpowers/findings/2026-11-14-tvout-app-picture-e2e.md`).

Memory for common sizes (RGB565, 2 bytes per pixel):

| Picture | Framebuffer | Notes |
|---|---|---|
| 480×320 | 307,200 | One buffer; an app with it (the v003 bench build of `hello_tvout`) took 376 KB of SRAM in all, before stacks and heap. |
| 320×240 | 153,600 | Two buffers for page flipping take the same 307,200. |
| 640×480 | 614,400 | Does not fit SRAM. |
| 320×240 at 2x | 153,600 | `tvout_start_2x()`: fills the 640×480 picture; two buffers for page flipping take 307,200. |

The OneWili link (SD card, main-CPU commands, peer streams) takes about 116
KB of SRAM of its own, whatever the app uses it for (AGENTS.md, "The OneWili
link costs about 116 KB"). An app with one 480×320 buffer used 376,100
bytes of the 512 KB before its 8 KB of stacks and heap, so with the link a
480×320 SRAM picture leaves about 24 KB for the rest of an app: a larger app takes a smaller picture (a 320×240 buffer at
2x fills the TV) or a PSRAM picture.

## PSRAM picture

A picture whose pixels are in PSRAM (`0x11000000`–`0x11800000`, or the same
memory through the uncached window at `0x15000000`) is not read in place:
DMA from PSRAM while video runs holds up the HSTX feed. Instead every picture
row of the list reads one of eight SRAM line buffers (7,680 bytes at 480
pixels wide), and a TIMER1 interrupt at the highest priority, on the core that
called `tvout_start_psram()`, keeps them up to six rows ahead of the TV. Each row is
fetched by the XIP streaming FIFO into its line buffer by a seventh DMA
channel that writes SRAM only. A tick reads at most three rows, then leaves
8 µs for other interrupts. The row logic is the stock firmware's TV mirror
logic, harvested unchanged (`rpDVIMirrorCore.h`).

**The framebuffer.** Declare it static with `__uninitialized_psram`, 4-byte
aligned, in an app built with `fw2_display_app()` (or `fw2_psram_app()`):

```c
static uint16_t __uninitialized_psram("tv_fb") __attribute__((aligned(4))) fb[480 * 320];

uint16_t *draw = tvout_psram_uncached(fb);   // draw and read through this
tvout_start_psram(fb, 480, 320);
```

`fw2_display_app()` maps the PSRAM sections so such a buffer takes no SRAM
and no UF2 bytes (its contents start undefined: draw every pixel before
starting). The About page's backup (`bsp/input/app_about.c`) is built this way.
Never cast `PSRAM_BASE`: the linker places its own PSRAM sections there.

**The LCD.** `st7796_flush_async()` is a DMA and must not read PSRAM while
the TV runs. `st7796_blit_rect()` sends the same pixels by CPU (blocking,
about as long as the flush would take) and may read the PSRAM buffer
directly: pass the uncached pointer the app draws through, so the CPU reads
what the TV reads and no stale cache line. To keep the button labels below
the picture, blit only the rows above them:
`st7796_blit_rect(0, 0, 479, KEY_Y - 1, draw)`, after each finished picture
(run on the board with the PSRAM picture bench app, `LATE 0` with the mirror
on). The CPU copy runs on the
core that draws, between the reader's interrupts; `rows_late` says whether
it disturbs the TV.

Rules for an app with a PSRAM picture on the TV:

- **No DMA may read or write PSRAM while the TV runs**, from the app or any
  driver: no `st7796_flush_async()` from a PSRAM buffer, no PSRAM capture
  ring. Feed the LCD with `st7796_blit_rect()` (above), or from SRAM.
- **Draw through `tvout_psram_uncached(pixels)`.** Rows are read from PSRAM
  itself, which is how the stock firmware writes its PSRAM copy; whether a
  write still held in the cache would be missed has not been tried.
- **If core 1 runs code, call `tvout_core1_init()` on core 1 first.** Core 1
  is then held, off PSRAM, in a top-priority interrupt for each row read.
  Without it the stock firmware measured unheld reads averaging 35.7 µs and
  peaking at 163 µs against a 31.8 µs TV line.
- **Budget the reader.** A held 480-pixel row read took 19.9 µs in the stock
  firmware; 320 rows a frame is about 38 % of the core every frame
  (estimate), about 19 % at 320×240. `tvout_get_stats()` measures it on the
  board (`reader_us` / `reader_window_us`).
- **Watch `rows_late`.** A row the TV reaches before it was read shows the
  previous row of that line buffer. Other interrupts at the highest priority
  on the same core, or interrupts kept off for long, make rows late.

There is no page flip for a PSRAM picture (`tvout_show()` refuses); a row
being drawn while it is read shows partly drawn. Two TIMER1 alarms are claimed
at the first PSRAM start (or `tvout_core1_init()`) and kept. The start refuses
if TIMER1 does not count microseconds, if no alarm or DMA channel is free, or
if the buffer is not 4-byte aligned or wider than `TVOUT_RING_WIDTH_MAX` (480;
a target may define up to 640, at 16 bytes of SRAM per pixel of width).

## Picture at 2x

`tvout_start_2x()` builds the frame of a picture twice as wide and tall whose
rows read the buffer's rows twice each (frame row `r` reads buffer row
`r / 2`, in the row table the page flip rewrites), and sets the HSTX encoder
to send every FIFO word twice: each pixel goes out as two. The DMA moves the
same bytes as for the buffer at 1x; the list block and channels are those of
any picture, and audio framing works the same way. A PSRAM picture is
refused: the line ring reads each frame row once. That the frame decodes to
each buffer pixel as a 2×2 block is checked on the host
(`tests/test_dvi_frame.cpp`, a model of the command expander with two encoder
shifts per word); the encoder setting has not been run on a board.

## Page flip

`tvout_show()` reads the scan position from the DMA channels and rewrites
every row's read address while the TV is between the end of the last picture
row and three scan pieces before the first. Interrupts are off for the
rewrite, one store per row (a few microseconds, estimate). If the scan
position after the rewrite has left that window, the flip counts in
`flips_late`: the frame then being sent may mix the two buffers. That a
rewrite inside the window never mixes them is checked on a model of the DMA
channels in `tests/test_dvi_frame.cpp`; the timing on the chip is not.

## Plug detection

The coprocessor reports the TV connector's hot-plug line in its keyboard
frame (`UARTKBD_FLAG_HOTPLUG`, `docs/drivers/keyboard.md`). `tvout_task()`
believes a new level after it has held for 300 ms (connected) or 1000 ms
(removed): a TV drops the line for a moment when it changes input. The line
reaches the coprocessor through a divider, and a connected TV reads correctly
only with coprocessor firmware that sets that input to TTL levels; with older
firmware no TV is ever reported. Which firmware a board carries is not known:
if `hello_tvout` never shows "TV CONNECTED" with a TV plugged in, the board
has the older one. The polarity (1 = connected) was confirmed on the board
2026-11-14: unplugging and replugging printed "TV removed" and "TV
connected".

**"No TV" and "cannot detect" look the same, and the BSP cannot tell them
apart.** The keyboard frame carries the hot-plug flag and nothing else about
it: no bit says the coprocessor reads the input at TTL levels, and the
coprocessor's firmware version is not in the frame and is not raised by that
setting, so no reading the display CPU can make separates an older
coprocessor from an empty connector. `tvout_tv_connected()` false therefore
means "not seen", never "no TV". An app should word it that way ("TV not
seen") and must not refuse to start video on it; `tvout_set_auto()` stays off
by default for the same reason. Seeing `TVOUT_PLUG_CONNECTED` once proves
the board detects; until then the app knows nothing.

`tvout_set_auto(true)` makes `tvout_task()` start the last picture started
or shown (the colour bars if none) on a settled connect and stop video on a
settled removal. It is off by default, so an app decides when video runs and
does not depend on the coprocessor firmware.

## Sound (`bsp/display/tvout_audio.h`)

Stereo 16-bit samples at 48 kHz, sent in the audio slots of a picture built
with audio framing: data islands in the blanking of every line but the
vertical sync lines, which also carry the picture-format (AVI) and audio
InfoFrames and the audio clock regeneration packet. A TV plays the sound. **A
plain DVI monitor may show nothing with audio framing on**, and the BSP cannot
read the display's EDID to find out, so framing is off by default and the app
chooses.

```c
tvout_set_audio_framing(true);      // from the next start
tvout_start(fb, 480, 320);          // or tvout_start_test()
tvout_audio_start();
tvout_audio_tone(TVOUT_TONE_LEFT);  // 1 kHz, -12 dBFS, left channel only
...
tvout_audio_tone(TVOUT_TONE_OFF);   // back to the app's samples
tvout_audio_write(stereo, frames);  // 48 kHz, left then right; returns frames taken
```

| Call | What it does |
|---|---|
| `void tvout_set_audio_framing(bool on)` | Audio framing (`true`) or plain DVI (`false`, the default) from the next start. A running picture keeps its framing. |
| `bool tvout_audio_framing(void)` | The setting. |
| `bool tvout_audio_start(void)` | Starts filling the running picture's audio slots, in silence or the tone last chosen. False with `tvout_audio_last_error()` set: no picture with audio framing runs, no free TIMER1 alarm, or TIMER1 does not count microseconds. |
| `void tvout_audio_stop(void)` | Every slot back to a null packet, then the feeder stops. `tvout_stop()` calls it. |
| `bool tvout_audio_running(void)` | The feeder runs. |
| `const char *tvout_audio_last_error(void)` | Why the last `tvout_audio_start()` was refused, or "". |
| `void tvout_audio_tone(tvout_tone_t)` | `TVOUT_TONE_LEFT`, `_RIGHT`, `_BOTH`: the 1 kHz tone in place of the written samples, which are taken and dropped at the same rate. `TVOUT_TONE_OFF`: the written samples. Kept across starts. |
| `unsigned tvout_audio_write(const int16_t *stereo, unsigned frames)` | Into a 2048-frame buffer (42.7 ms, `TVOUT_AUDIO_BUFFER_FRAMES`); returns how many fitted, 0 while the feeder is stopped. All writes from one context: one core's main loop, or one interrupt handler. |
| `unsigned tvout_audio_room(void)` | Frames `tvout_audio_write()` would take now; 0 while the feeder is stopped. |
| `void tvout_audio_get_stats(tvout_audio_stats_t *)` | Since the last start: `packets` written in time; `underruns`, frames sent as silence because the buffer was empty; `late`, lines the TV reached before their packet was written; `repeats`, of those, lines that sent the previous packet's samples again; `lost`, frames dropped on late lines. A clean run has `late`, `repeats` and `lost` at 0. |

The app's samples wait in a 2048-frame buffer (8 KB of SRAM, linked only
into apps that use the sound) and each feeder tick moves them on into the
feeder's own 128-frame buffer. An app that writes its own sound must top it
up at least every 42 ms: once per pass of a 60 Hz game loop is enough, with
room to spare for a `tvout_show()` wait (one frame, usually) or an LCD flush
wait. The pattern is to make `tvout_audio_room()` frames and write them.
What the app misses goes out as silence and counts in `underruns`. The
buffer adds up to 42.7 ms between a write and the TV playing it.

`tvout_audio_write()` may be called from an interrupt handler (a repeating
timer, say) as long as every write comes from that one handler; never call
`tvout_audio_stop()` or `tvout_stop()` from an interrupt handler, since the
stop waits for a write in progress to end.

**Turning sound on and off without a restart.** Audio framing is fixed when
the picture starts: changing it with `tvout_set_audio_framing()` takes a
`tvout_stop()` and a start, and the TV loses the picture for a moment.
Starting and stopping the sound does not: keep audio framing on and call
`tvout_audio_stop()` (every slot back to a null packet; the TV plays
nothing) and `tvout_audio_start()` again while the picture runs, or keep the
feeder running and write silence. Only a display that cannot show audio
framing needs it off. The NAU88C10 codec runs at 16 kHz; sound
for both the speaker and the TV is converted to 48 kHz by the app.

How it runs: one TIMER1 alarm interrupt, at the highest priority, on the core
that called `tvout_audio_start()`. Each tick reads the scan position from the
DMA channels, makes the packets of the next lines (up to seven ahead) and
writes each into its slot once the TV has finished the line that used that
slot before. A tick works at most about 23 µs and the next comes 8 µs (more
to do) or 24 µs (caught up) after it ends, so other interrupts on that core
wait up to about 23 µs. The tick runs from SRAM in every app, PSRAM-executing
ones included.

The stock firmware's other source, the TV playing what the codec plays by
reading the codec's own buffers, is not carried over: the BSP's I2S driver
loops one stereo buffer whose length need not be a power of two, at 16 kHz,
where the firmware's reader expects two mono buffers of a power-of-two
length. An app gets the same result by writing its samples to both.

`apps/hello_tvout` shows the pattern (YELLOW): a picture restarted with
audio framing and a tone written each loop pass from `tvout_audio_room()`.

## Clocks

`clk_sys` is never changed. While video runs, the USB PLL runs at 126 MHz
(1512 MHz VCO / 6 / 2) and drives `clk_hstx` alone; HSTX takes five clocks
per pixel, so the pixel clock is exactly 25.2 MHz at any `clk_sys`, including
in PSRAM-executing apps, which cannot change `clk_sys`.

The USB PLL normally clocks `clk_usb` and `clk_adc` at 48 MHz, and those two
take only a whole divider. So at start:

| `clk_sys` | `clk_usb`, `clk_adc` while video runs | Native USB host | ADC (RFID reader, `board_die_temp_c()`) |
|---|---|---|---|
| A whole multiple of 48 MHz (240, 288) | System PLL / n = 48 MHz | Usable | Usable |
| Anything else, including the 250 MHz default | Stopped | Unavailable | Unavailable |

The ADC is stopped rather than run at another rate (250 / 5 = 50 MHz or
250 / 6 = 41.7 MHz): whether the ADC is in specification at those rates has
not been checked. `tvout_stop()` returns the USB PLL to 48 MHz and puts both
clocks back on it. An app that needs native USB host or the ADC while the TV
runs should bring the board up at 240 MHz (`board_init_clk(240000)`), which
moves the audio codec's MCLK pitch by about 1 %.

**While video runs, never call `set_sys_clock_khz()` or `board_init_clk()`.**
The SDK parks `clk_sys` on the USB PLL while it retunes the system PLL, and the
USB PLL is then at 126 MHz, not 48.

## Refusals

- `rfid_reader_start()` returns false while video runs (the reader stays
  idle), and `tvout_start_test()` and `tvout_start()` refuse while the RFID
  reader is running. The stock firmware refuses the same pair because the
  reader's capture and the TV picture together run the HSTX FIFO short; that
  has not been measured with this BSP.
- `board_die_temp_c()` returns `NAN` while `clk_adc` is stopped; an ADC
  conversion would never finish.

## Resources

| Resource | Use |
|---|---|
| HSTX, GPIO 12–19 | Owned while running; the pins go back to no function at stop. |
| DMA | Three channels for the bars, six for a picture, claimed with `dma_claim_unused_channel(false)`; no DMA interrupt. PIO-USB takes channel 9 at its own start: start PIO-USB first if both are used. |
| SRAM | One 8,576-byte static list block (the bars' 525 entries, or a picture's entries, scan runs and row table; the largest picture needs 8,568) plus about 0.3 KB of shared line words. Only apps that call `tvout_*` link them. Audio framing adds a 5,240-byte static pool (the lines with a data island; 1,310 words, the most any picture needs) and the feeder about 1.3 KB plus its 8,192-byte sample buffer, linked only into apps that call `tvout_set_audio_framing()`. With audio framing a picture's list is shorter (4,264 bytes at most). |
| Interrupts, timers, PIO | None for an SRAM picture without sound. `tvout_show()` turns interrupts off for its rewrite. A PSRAM picture: two TIMER1 alarm interrupts at the highest priority, the reader's tick on the starting core and, after `tvout_core1_init()`, the hold on core 1. Sound: one TIMER1 alarm, claimed at the first `tvout_audio_start()` and kept, its interrupt at the highest priority. A PSRAM picture with sound and CEC uses all four TIMER1 alarms. |
| SRAM, PSRAM picture | 7,680 bytes of line buffers (8 × `TVOUT_RING_WIDTH_MAX` × 2), static, linked only into apps that call `tvout_start_psram()`. |
| PSRAM | Read only by the XIP streaming FIFO, for a PSRAM picture. **No DMA may read or write PSRAM while video runs**: it holds up the channel feeding HSTX. The LCD's async flush from a PSRAM buffer is such a DMA. |

## Source layout

`tvout.c` (C) holds the clocks, HSTX setup, DMA chain, scan position, page
flip and plug policy. The scan-out list is built by C++ classes harvested
unchanged from the stock firmware's TV output (`rpDVIFrame`, `rpDVIList`,
`rpDVIPacket`, `rpDVIModes.h`, `fwPortability.h`), behind the C entry points in
`tvout_list.{h,cpp}`, which also hold the page-flip window rule.
`tvout_plug.{h,c}` is the pure settle logic. `tvout_ring.{h,c}` (C) is the
PSRAM reader: alarms, the stream channel, the reader and hold interrupts;
its row logic is the stock firmware's `rpDVIMirrorCore.h`, harvested
unchanged behind the C entry points in `tvout_ring_core.{h,cpp}`. Host tests:
`tests/test_dvi_frame.cpp` (the frame decoded the way the HSTX command
expander reads it, the list run on a model of the DMA channels, and the
page-flip window against that model), `tests/test_tvout_list.c` (the C entry
points: every picture size fits the list block, the row words, the window
bounds), `tests/test_tvout_plug.c` (settle times),
`tests/test_dvi_mirror.cpp` (the row logic on a simulated TV, reader and
drawing core, including the always-on PSRAM case) and
`tests/test_tvout_ring_core.c` (its C entry points).

Sound: `tvout_audio.c` (C) holds the TIMER1 alarm and its interrupt;
`tvout_audio_core.{h,cpp}` is the C entry point to the feeder's slot
bookkeeping and packet coding, `rpDVIAudioCore.h`, harvested unchanged from
the stock firmware. Host tests: `tests/test_dvi_audio_framing.cpp` (audio
framing and packets checked by an independent decoder),
`tests/test_dvi_audio.cpp` (the feeder on a simulated TV, with random
interrupt delays: no torn, stale or repeated packet), the feeder section of
`tests/test_dvi_frame.cpp` (the feeder on the DMA model of each picture's
list) and `tests/test_tvout_audio.cpp` (the C entry points, the pool size and
the list sizes with audio framing).

## Not yet

A tear-free mode for one buffer, page flip for PSRAM pictures, choosing
audio framing from the display's EDID.
