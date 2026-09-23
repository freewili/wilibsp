// src/display/st7796.h — 480x320 LCD driver (ST7789-class controller) over the RP2350
// hardware SPI peripheral (SPI1: SCLK=GPIO10, MOSI=GPIO11). Blocking transfers for
// bring-up paths plus a non-blocking IRQ/DMA async-flush API for the LVGL display port.
#ifndef ST7796_DRIVER_H
#define ST7796_DRIVER_H
#include <stdint.h>
#include <stdbool.h>

#define ST7796_W 480
#define ST7796_H 320

// Initialize SPI1 + the panel. Backlight is controlled separately (board.c).
void st7796_init(void);

// Set the GRAM address window for subsequent pixel writes.
void st7796_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

// Fill the whole panel with one big-endian RGB565 color (block write).
void st7796_fill_screen(uint16_t color_be);

// Fill a rectangle with one big-endian RGB565 color (single block window, the
// proven write shape). Clipped to the panel. For small overlays (e.g. an OSD
// progress bar); clear them by repainting bars.
void st7796_fill_rect(int x, int y, int w, int h, uint16_t color_be);

// Blocking blit of a contiguous big-endian RGB565 buffer into [x0,y0]..[x1,y1]
// (one block window). Pixels must already be in wire (byte-swapped) order. The
// caller sequences this against the async flush (spin on !st7796_flush_busy()).
void st7796_blit_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                      const uint16_t *pixels_be);

// Draw ASCII text with the built-in 5x7 font at integer scale (1..4): each
// glyph cell is 6*scale x 8*scale, drawn as one block window per character
// (the proven write shape). Full printable ASCII 0x20-0x7E; unknown chars are
// blanks. Text that would overrun the panel edge is clipped at whole chars.
void st7796_draw_text(int x, int y, int scale, uint16_t fg_be, uint16_t bg_be,
                      const char *s);

// ---- Async DMA flush (for LVGL display port) ----
// Callback type invoked from the DMA_IRQ_0 completion handler when the flush
// is fully done (SPI drained, CS raised).
typedef void (*st7796_flush_done_cb)(void);

// Non-blocking flush: sets the address window (CASET/RASET, blocking but tiny),
// opens the RAMWR session, starts a DMA of `pixels` (already byte-swapped to
// wire order, 2 bytes per pixel) to the SPI TX FIFO, and returns immediately.
// On DMA completion the IRQ drains the SPI FIFO, raises CS, and calls done().
// The pixel buffer must remain valid until done() fires.
void st7796_flush_async(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                        const uint16_t *pixels, st7796_flush_done_cb done);

// Returns true while an async flush is in progress (from the moment the DMA is
// triggered until after CS is raised and the done-callback fires). Plan 3's
// super-loop / SPI bus arbiter spins on !st7796_flush_busy() before taking the
// shared SPI1 bus for a CC1101 burst, enforcing the single-SPI-owner invariant.
bool st7796_flush_busy(void);

/* Wait until an async DMA flush has drained SPI and released the panel CS. */
void st7796_flush_wait(void);

/* Temporarily discard app drawing while a modal BSP overlay is visible. */
void st7796_set_write_suppressed(bool suppressed);

#endif
