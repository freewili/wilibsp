// bsp/display/tvout.h — TV output: 640x480 at 60 Hz DVI on the HSTX block,
// GPIO 12-19, fed by a looped DMA list with no interrupt.
//
// Clocks. clk_sys is not touched. While video runs the USB PLL runs at
// 126 MHz for clk_hstx alone (five HSTX clocks per pixel: 25.2 MHz exactly).
// When clk_sys is a whole multiple of 48 MHz (240, 288 MHz), clk_usb and
// clk_adc move to the system PLL divided down to 48 MHz and keep running.
// Otherwise (the 250 MHz default) clk_usb and clk_adc are stopped while video
// runs: native USB host and the ADC are unavailable, and the RFID reader and
// board_die_temp_c() refuse. tvout_stop() puts the USB PLL back at 48 MHz and
// clk_usb and clk_adc back on it.
//
// While video runs:
// - never call set_sys_clock_khz() or board_init_clk(): the SDK parks clk_sys
//   on the USB PLL while it retunes, and the USB PLL is not at 48 MHz;
// - no DMA may read or write PSRAM: it holds up the channel feeding HSTX;
// - bsp/display/hstx_dvi must not run (it owns the same block and pins).
//
// Call after board_init().
#ifndef TVOUT_H
#define TVOUT_H
#include <stdbool.h>
#include <stdint.h>
#include "display/tvout_plug.h"

#ifdef __cplusplus
extern "C" {
#endif

// Eight vertical colour bars (white, yellow, cyan, green, magenta, red, blue,
// black) across the whole picture. Takes three DMA channels and 8.4 KB of
// static SRAM. True if video runs (also when it already did); false with
// tvout_last_error() naming the cause: the RFID reader is running, no free
// DMA channel, or the video clock did not start.
bool tvout_start_test(void);

// The app's picture: width x height pixels of RGB565, high byte first (the
// LCD's wire order, so one buffer can feed both), row y at pixels + y * width,
// centred in the 640x480 picture with black around. width at most 640 and
// even, height at most 480. The pixels wholly in SRAM, read in place for as
// long as video runs. Takes six DMA channels and the same 8.4 KB static list
// as the bars. True if video runs; if video already ran, nothing changes
// (stop first). False with tvout_last_error() naming the cause.
bool tvout_start(const uint16_t *pixels, int width, int height);

// A picture whose pixels are in PSRAM, otherwise as tvout_start(). Only apps
// that call this link the line ring (7.5 KB of SRAM at 480 wide).
//
// The pixels (either window, 4-byte aligned, width at most
// TVOUT_RING_WIDTH_MAX, 480 unless the target defines more) are copied row by
// row into eight SRAM line buffers (7.5 KB at 480) a few rows ahead of the TV
// by a top-priority TIMER1 interrupt on the calling core, through the XIP
// streaming FIFO and a seventh DMA channel that writes SRAM only. Takes two
// TIMER1 alarms, kept after the first start. Costs about 20 us of that core
// per 480-pixel row, about 38 % of it at 480x320 (an estimate from the stock
// firmware's measurement; tvout_get_stats() measures it). While it runs:
// - draw through tvout_psram_uncached(pixels): rows are read from PSRAM
//   itself, and a write still held in the cache may not be there yet
//   (drawing through the cache has not been tried);
// - nothing may DMA to or from PSRAM (see above), the LCD flush from a PSRAM
//   buffer included;
// - an app running code on core 1 calls tvout_core1_init() there first.
// Rows the TV reaches before they are read count in rows_late and show the
// line buffer's previous row. No tvout_show() for a PSRAM picture.
bool tvout_start_psram(const uint16_t *pixels, int width, int height);

// The app's picture at twice its size: each pixel sent as two pixels across
// and every row as two rows, so a 320x240 buffer fills the 640x480 picture.
// width at most 320, height at most 240, centred with black around; pixels
// as for tvout_start(), wholly in SRAM (a PSRAM picture is refused). Same
// channels and list block as tvout_start(); tvout_show() flips it.
bool tvout_start_2x(const uint16_t *pixels, int width, int height);

// The uncached alias of a PSRAM address (other addresses unchanged), for
// drawing a PSRAM picture that tvout_start_psram() shows.
uint16_t *tvout_psram_uncached(uint16_t *pixels);

// Call once on core 1, before it touches PSRAM, in an app that runs code on
// core 1 while a PSRAM picture shows: core 1 is then held in a top-priority
// interrupt, off PSRAM, for each row read. Without it core 1's PSRAM accesses
// break the reads up and rows come late. Takes the two TIMER1 alarms if no
// PSRAM picture has yet.
void tvout_core1_init(void);

// Page flip for a picture started with tvout_start() or tvout_start_2x():
// every row is pointed at pixels (same size, wholly in SRAM) while the TV is
// outside the picture rows, so no frame mixes the two buffers. Waits for that
// point, at most about three frames. When it returns true the buffer shown
// before is no longer read and may be drawn into. False with tvout_last_error() set if no
// picture runs, the buffer is not in SRAM, or the point never came.
bool tvout_show(const uint16_t *pixels);

// Stops video, frees the DMA channels and restores the clocks. Nothing when
// video is not running.
void tvout_stop(void);

bool tvout_running(void);

// The cause of the last refused start, or "".
const char *tvout_last_error(void);

// True while clk_adc runs (the ADC usable; native USB host is usable under
// the same conditions). False only while video runs on a clk_sys that is not
// a whole multiple of 48 MHz, or if the USB PLL did not return to 48 MHz at
// the last stop.
bool tvout_adc_available(void);

// Words in the HSTX FIFO now (0-8); 0 while video is not running. A level of
// 0 while video runs means the FIFO ran empty.
unsigned tvout_fifo_level(void);

typedef struct {
    uint32_t flips;        // tvout_show() calls that re-pointed the rows
    uint32_t flips_late;   // of those, the ones whose rewrite ended inside the
                           // margin before the first row or later; such a
                           // frame may mix the two buffers
    // PSRAM picture: counts since its start.
    uint32_t rows_late;          // rows the TV reached before they were read
    uint32_t row_reads;
    uint32_t row_read_timeouts;  // reads given up after 300 us: part of a row old
    uint32_t row_read_us_max;    // the longest row read
    uint32_t reader_us;          // time inside the reader's interrupt (wraps at 2^32)
    uint32_t reader_window_us;   // time since the start; reader_us / reader_window_us
                                 // is the reader's share of its core
} tvout_stats_t;

void tvout_get_stats(tvout_stats_t *out);

// TV plug detection from the coprocessor's hot-plug flag (UARTKBD_FLAG_HOTPLUG),
// believed after 300 ms connected or 1000 ms removed. Reads correctly only
// with coprocessor firmware that sets the hot-plug input to TTL levels; older
// firmware never reports a TV. Call tvout_task() on every main-loop pass after
// fw2_app_recovery_task(), which reads the flag; it returns the settled
// change this pass made, if any.
tvout_plug_event_t tvout_task(void);

// The settled plug state; false until a TV has been seen for 300 ms. False
// means "not seen": nothing in the keyboard frame says whether the
// coprocessor can detect a TV, so an empty connector and older coprocessor
// firmware read the same.
bool tvout_tv_connected(void);

// Automatic start, off by default. While on, tvout_task() starts the last
// picture on a settled connect (the colour bars if none was started) and
// stops video on a settled removal.
void tvout_set_auto(bool on);

#ifdef __cplusplus
}
#endif
#endif
