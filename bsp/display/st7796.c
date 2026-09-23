// src/display/st7796.c
//
// HARDWARE-SPI driver for the FreeWili 2 panel (a Sitronix ST7789-class controller
// run at 480x320). Byte-for-byte the known-good reference's executed init path
// (freewili-firmware/rmpLib/st7789.cpp, WILI_TWO_DISPLAY 480x320 path):
//   - SPI1 on SCLK=GPIO10 / MOSI=GPIO11, <=100 MHz (divider-limited from clk_peri)
//   - DC=GPIO8, CS=GPIO9 as SIO; all four signals at 12 mA drive
//   - NO hardware reset: FW2 exposes no LCD reset GPIO; panel RESX is handled in
//     hardware and the controller is reset with SWRESET (0x01) only.
//
// REQUIRES board_init() to have re-sourced clk_peri from clk_sys after the overclock
// — otherwise the SPI peripheral has no clock and the panel stays dark.

#include "display/st7796.h"
#include "display/font5x7.h"
#include "platform/board.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "agentio/agentio.h"

#define LCD_SPI spi1

// ---- Async DMA flush state (declared early; st7796_init() uses them) ----
// flush_in_flight: Plan 3's super-loop / SPI bus arbiter spins on
// !st7796_flush_busy() before taking the shared SPI1 bus for a CC1101 burst,
// enforcing the single-SPI-owner invariant.
static volatile int                  flush_dma_chan  = -1;
static volatile st7796_flush_done_cb flush_done_cb   = 0;
static volatile bool                 flush_in_flight = false;
static bool                          s_write_suppressed;
static void st7796_dma_irq(void); // forward declaration

// Command byte (DC=0) then optional params (DC=1), framed by CS, over hardware SPI.
// The NOPs guard CS/DC setup/hold timing exactly like the proven reference command()
// (rmpLib/st7789.cpp): without them, edges land too close to SPI clock activity at
// 200 MHz core / 100 MHz SPI and the panel drops commands. Guard counts are doubled
// relative to the reference to keep absolute margins when the core is overclocked
// (NOPs scale with core clock).
static void cmd(uint8_t c, const uint8_t *params, size_t nparams) {
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    gpio_put(PIN_LCD_CS, 0);
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    gpio_put(PIN_LCD_DC, 0);
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    spi_write_blocking(LCD_SPI, &c, 1);
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    if (params && nparams) {
        gpio_put(PIN_LCD_DC, 1);
        asm volatile("nop");
        asm volatile("nop");
        asm volatile("nop");
        asm volatile("nop");
        spi_write_blocking(LCD_SPI, params, nparams);
    }
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    gpio_put(PIN_LCD_CS, 1);
}

void st7796_init(void) {
    // Request 100 MHz — the panel's proven rate. The SSP divider yields the
    // closest achievable rate NOT exceeding the request (100 MHz at a 200 MHz
    // clk_peri; 66 MHz at 264 MHz). Never let SPI scale past the proven rate.
    // 8-bit mode-0 MSB-first (default).
    spi_init(LCD_SPI, LCD_SPI_BAUD);
    gpio_set_function(PIN_LCD_SCLK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_LCD_MOSI, GPIO_FUNC_SPI);
    gpio_set_drive_strength(PIN_LCD_SCLK, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(PIN_LCD_MOSI, GPIO_DRIVE_STRENGTH_12MA);

    // DC / CS as SIO outputs at 12 mA (reference drives these hard too).
    gpio_init(PIN_LCD_DC); gpio_set_dir(PIN_LCD_DC, GPIO_OUT);
    gpio_set_drive_strength(PIN_LCD_DC, GPIO_DRIVE_STRENGTH_12MA);
    gpio_init(PIN_LCD_CS); gpio_set_dir(PIN_LCD_CS, GPIO_OUT);
    gpio_set_drive_strength(PIN_LCD_CS, GPIO_DRIVE_STRENGTH_12MA);
    gpio_put(PIN_LCD_CS, 1);   // CS idle high

    // Init sequence — byte-for-byte the reference's EXECUTED 480x320 path. Note: the
    // reference sends NO power/porch/gamma commands for this panel size (that block
    // is commented out in st7789.cpp), and its mySleepa() doubles every delay.
    cmd(0x01, NULL, 0); busy_wait_us_32(300000);                         // SWRESET (ref mySleepa(150) = 300 ms)
    cmd(0x35, NULL, 0);                                                 // TEON
    cmd(0x3A, (const uint8_t[]){0x05}, 1);                              // COLMOD: 16bpp

    cmd(0x21, NULL, 0);                                                 // INVON
    cmd(0x11, NULL, 0);                                                 // SLPOUT
    cmd(0x29, NULL, 0); busy_wait_us_32(200000);                         // DISPON (ref mySleepa(100) = 200 ms)
    cmd(0x2A, (const uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4);            // CASET 0..479
    cmd(0x2B, (const uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4);            // RASET 0..319
    cmd(0x36, (const uint8_t[]){0x2C}, 1);                              // MADCTL

    // One-time DMA channel claim for async flushes (called here, after SPI/GPIO are up).
    // DMA_IRQ_0 uses irq_add_shared_handler so future radio/PIO DMA can co-exist safely.
    flush_dma_chan = dma_claim_unused_channel(true);
    dma_channel_set_irq0_enabled(flush_dma_chan, true);
    irq_add_shared_handler(DMA_IRQ_0, st7796_dma_irq,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);
}

void st7796_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    if (s_write_suppressed) return;
    cmd(0x2A, (const uint8_t[]){x0 >> 8, x0, x1 >> 8, x1}, 4);
    cmd(0x2B, (const uint8_t[]){y0 >> 8, y0, y1 >> 8, y1}, 4);
    agentio_shadow_note_window(x0, y0, x1, y1);
}

// Open a RAMWR data session for the previously set window: CS low, RAMWR, DC high.
// NOP guards as in cmd() — the panel needs CS/DC setup/hold margin at this clock.
static void push_begin(void) {
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    gpio_put(PIN_LCD_CS, 0);
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    gpio_put(PIN_LCD_DC, 0);
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    uint8_t ramwr = 0x2C;
    spi_write_blocking(LCD_SPI, &ramwr, 1);
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    gpio_put(PIN_LCD_DC, 1);
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
}

static void push_end(void) {
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    asm volatile("nop");
    gpio_put(PIN_LCD_CS, 1);
}

// RAMWR + raw pixel bytes for the previously set window. CS/DC framed here.
static void push_pixels(const uint8_t *bytes, size_t n) {
    push_begin();
    spi_write_blocking(LCD_SPI, bytes, n);
    agentio_shadow_note_pixels(bytes, n);
    push_end();
}

void st7796_fill_screen(uint16_t color_be) {
    if (s_write_suppressed) return;
    static uint16_t line[ST7796_W];
    for (int x = 0; x < ST7796_W; x++) line[x] = color_be;
    st7796_set_window(0, 0, ST7796_W - 1, ST7796_H - 1);
    push_begin();
    for (int y = 0; y < ST7796_H; y++) {
        spi_write_blocking(LCD_SPI, (const uint8_t *)line, sizeof line);
        agentio_shadow_note_pixels((const uint8_t *)line, sizeof line);
    }
    push_end();
}

void st7796_fill_rect(int x, int y, int w, int h, uint16_t color_be) {
    if (s_write_suppressed) return;
    if (x < 0) { w += x; x = 0; }            // clip to the panel
    if (y < 0) { h += y; y = 0; }
    if (x + w > ST7796_W) w = ST7796_W - x;
    if (y + h > ST7796_H) h = ST7796_H - y;
    if (w <= 0 || h <= 0) return;
    static uint16_t line[ST7796_W];
    for (int i = 0; i < w; i++) line[i] = color_be;
    st7796_set_window(x, y, x + w - 1, y + h - 1);   // one block window
    push_begin();
    for (int row = 0; row < h; row++) {
        spi_write_blocking(LCD_SPI, (const uint8_t *)line, (size_t)w * 2);
        agentio_shadow_note_pixels((const uint8_t *)line, (size_t)w * 2);
    }
    push_end();
}

void st7796_blit_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                      const uint16_t *pixels_be) {
    if (s_write_suppressed) return;
    uint32_t n = (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1);
    st7796_set_window(x0, y0, x1, y1);
    push_pixels((const uint8_t *)pixels_be, (size_t)n * 2);
}

void st7796_draw_text(int x, int y, int scale, uint16_t fg_be, uint16_t bg_be,
                      const char *s) {
    if (s_write_suppressed) return;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    static uint16_t glyph[6 * 8 * 4 * 4];     // max cell at scale 4
    const int w = 6 * scale, h = 8 * scale;
    for (; *s; s++, x += w) {
        if (x + w > ST7796_W || y + h > ST7796_H || x < 0 || y < 0) break;
        char c = *s;
        const uint8_t *cols = (c >= FONT5X7_FIRST && c <= FONT5X7_LAST)
                                  ? font5x7[c - FONT5X7_FIRST]
                                  : font5x7[0];
        for (int gy = 0; gy < h; gy++) {
            int row = gy / scale;             // 0..7 (row 7 = inter-line gap)
            for (int gx = 0; gx < w; gx++) {
                int col = gx / scale;         // 0..5 (col 5 = inter-char gap)
                bool on = col < 5 && row < 7 && ((cols[col] >> row) & 1);
                glyph[gy * w + gx] = on ? fg_be : bg_be;
            }
        }
        st7796_set_window(x, y, x + w - 1, y + h - 1);
        push_pixels((const uint8_t *)glyph, (size_t)w * h * 2);
    }
}

// ---- Async DMA flush (DMA_IRQ_0) ----
// Channel claimed in st7796_init() and held for program lifetime.
// DMA_IRQ_0 is a SHARED handler line — see board.h for the ownership model.

static void st7796_dma_irq(void) {
    if (dma_channel_get_irq0_status(flush_dma_chan)) {
        dma_channel_acknowledge_irq0(flush_dma_chan);
        // DMA done != shifted out: drain the SPI before raising CS (short spin).
        while (spi_get_hw(LCD_SPI)->sr & SPI_SSPSR_BSY_BITS) tight_loop_contents();
        gpio_put(PIN_LCD_CS, 1);                 // close the RAMWR session
        if (flush_done_cb) flush_done_cb();
        flush_in_flight = false;                 // clear LAST — "busy" brackets entire flush incl. CS-raise
    }
}

bool st7796_flush_busy(void) {
    return flush_in_flight;
}

void st7796_flush_wait(void) {
    while (flush_in_flight) tight_loop_contents();
}

void st7796_set_write_suppressed(bool suppressed) {
    s_write_suppressed = suppressed;
}

void st7796_flush_async(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                        const uint16_t *pixels, st7796_flush_done_cb done) {
    if (s_write_suppressed) {
        if (done) done();
        return;
    }
    flush_done_cb = done;
    st7796_set_window(x0, y0, x1, y1);            // CASET/RASET (blocking, small)
    // Open RAMWR data session (CS low, RAMWR, DC high) — reuse push_begin().
    push_begin();
    // Mirror at DMA-trigger time: the caller guarantees the pixel buffer stays
    // valid until done() fires, so reading it here is safe and avoids doing
    // work in the DMA IRQ.
    agentio_shadow_note_pixels((const uint8_t *)pixels,
                               (size_t)(x1 - x0 + 1) * (y1 - y0 + 1) * 2);
    uint32_t n = (uint32_t)(x1 - x0 + 1) * (y1 - y0 + 1);  // pixel count
    dma_channel_config c = dma_channel_get_default_config(flush_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8); // 8-bit SPI, 2 bytes/px
    channel_config_set_dreq(&c, spi_get_dreq(LCD_SPI, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    flush_in_flight = true;                         // set immediately before DMA trigger
    dma_channel_configure(flush_dma_chan, &c,
        &spi_get_hw(LCD_SPI)->dr, pixels, n * 2, true);    // n*2 bytes; start now
    // Returns immediately; st7796_dma_irq() raises CS + calls done() on completion.
}
