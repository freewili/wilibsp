// src/platform/board.h — pin map + board bring-up for the FreeWili2 (RP2350B) +
// 480x320 ST7789-class panel. Adapted from evaderkrub/usbcamfw — MIT, (c) 2026 Dave Robins.
// There is NO LCD reset GPIO on this board — the panel RESX is hardware-handled,
// so the driver relies on SWRESET only.
#ifndef BOARD_H
#define BOARD_H
#include <stdint.h>

// --- LCD (ST7789-class panel, 480x320, over SPI1) ---
#define PIN_LCD_DC     8    // LCD_DC_D: data/command (also CC1101 MISO — see PIN_CC1101_MISO)
#define PIN_LCD_CS     9    // LCD_CS_D: chip select, active low
#define PIN_LCD_SCLK   10   // LCD_SCLK_D: SPI1 SCK
#define PIN_LCD_MOSI   11   // LCD_MOSI_D: SPI1 TX
#define PIN_LCD_TE     33   // LCD_TE: tearing-effect output (unused)
#define PIN_LCD_BL     25   // backlight: plain on/off

// --- CC1101 sub-GHz radio (shares SPI1 with the LCD) ---
#define PIN_CC1101_CS    40  // CC1101 chip-select (active low); park HIGH before LCD traffic
#define PIN_CC1101_MISO  8   // == PIN_LCD_DC. This pin is an OUTPUT (DC) for the LCD and
                             // must be switched to SPI1 RX (input) around CC1101 access.
#define PIN_CC1101_GDO0  32  // live data / sync (PIO-sampled, Plan 3/5)
#define PIN_CC1101_GDO2  37

// --- WS2812 RGB LEDs (16 pixels, single data line) ---
#define PIN_LED_DATA   21   // WS2812 data; driven by a pio1 state machine

// --- IR receiver/transmitter (PIO2 SMs) ---
#define PIN_IR_TX  20   // IR transmitter LED (PIO carrier-modulated, pio2)
#define PIN_IR_RX  24   // IR receiver, TSOP-style demodulated envelope: idle HIGH, mark = LOW
#define PIN_PIO_USB_DP 42
#define PIN_PIO_USB_DM 43

// --- I2C1 (touch, sensors) ---
#define PIN_I2C1_SDA   26
#define PIN_I2C1_SCL   27

// --- I2S audio (NAU88C10 codec; pins from FW2Display_pin_definitions.h) ---
#define PIN_AUDIO_DATA 5    // SPK_DIN:  I2S data into codec (PIO out / DAC)
#define PIN_AUDIO_DIN  4    // SPK_DOUT: codec ADC data into MCU (PIO in)
#define PIN_AUDIO_LRCK 6    // SPK_LRCK: I2S word clock  (PIO sideset bit 0)
#define PIN_AUDIO_BCLK 7    // SPK_BCLK: I2S bit clock   (PIO sideset bit 1)
#define PIN_AUDIO_MCLK 22   // SPK_MCLK: 256*fs square wave from PWM

// --- PDM microphones (4 MEMS mics, 2 data lines x 2 clock phases) ---
#define PIN_MIC_CLK    28   // shared PDM clock out (PIO side-set)
#define PIN_MIC_SIG1   29   // data line 1: Mic A (clk-high) + Mic B (clk-low)
#define PIN_MIC_SIG2   30   // data line 2: Mic C (clk-high) + Mic D (clk-low)
// MIC_SIG1/2 are consecutive so one PIO `in pins, 2` reads both.
// PDM clock 1.024 MHz (= 16 kHz x 64): the FW2 MEMS mics did NOT output at the
// datasheet-typical 3.072 MHz on this board (measured in microphonearray repo);
// 1.024 MHz matches the known-working movieplayer mic on this hardware.
#define PDM_CLK_HZ     1024000u

// --- External PSRAM (APS6404L 8MB) on the QSPI/QMI second chip select ---
// GPIO47 = XIP_CS1n (function 9), NOT on SPI1. Brought up + memory-mapped by the
// SDK's hardware_psram at boot (see bsp/boards/freewili2.h for the CS pin/size the
// SDK reads), then re-timed for the overclock in board_init_clk().
#define PIN_PSRAM_CS   47

// --- PIO-USB device port (Pico-PIO-USB, RP2350 device via PIO). Faces the PC. ---
#define PIN_USB_DP  42   // USB D+
#define PIN_USB_DM  43   // USB D- (must be D+ + 1 for Pico-PIO-USB)

// --- DMA-IRQ ownership model ---
// DMA_IRQ_0 is a SHARED handler line. The ST7796 display flush registers a
// shared handler (via irq_add_shared_handler) that acts only on its own DMA
// channel. Any future radio/PIO DMA (Plan 3/5) must likewise use
// irq_add_shared_handler(DMA_IRQ_0, ...) and guard on its own channel status.
// Do NOT use irq_set_exclusive_handler on DMA_IRQ_0.

// --- SPI1 baud rates (shared bus). LCD runs fast; CC1101 is limited. ---
#define LCD_SPI_BAUD     (100u * 1000u * 1000u)   // 100 MHz (divider-limited by clk_peri)
#define CC1101_SPI_BAUD  (5u * 1000u * 1000u)     // 5 MHz (CC1101 SPI ceiling ~6.5 MHz)

// --- Clocks: default overclock to 250 MHz @ vreg 1.25 V, run from RAM
// (copy_to_ram). 250 MHz is the default because it is audio-optimal: the NAU88C10
// MCLK is an integer divide clk_sys/61 = 4.0984 MHz (~16 kHz fs) at 250. Apps that
// need a different even-MHz clock pass it to board_init_clk() (e.g. DVI uses 252
// MHz for an exact 25.2 MHz pixel clock, trading ~0.8% audio pitch). ---
#define BOARD_SYS_CLOCK_KHZ 250000

// Full board bring-up at the DEFAULT clock (BOARD_SYS_CLOCK_KHZ = 250 MHz): vreg
// 1.25 V, clk_peri re-source, RGB LEDs off without retaining PIO resources,
// SPI1 bus, park CC1101 CS, backlight off, I2C1
// @ 400 kHz, ioexp_init.
void board_init(void);
void board_init_psram(void);

// Same bring-up at a caller-chosen system clock (kHz; use an even MHz so downstream
// /2 dividers like DVI's clk_hstx are exact). board_init() == board_init_clk(BOARD_SYS_CLOCK_KHZ).
void board_init_clk(uint32_t sys_clock_khz);

// Peripheral-only bring-up for a PSRAM app launched by the DISPLAY loader.
// Preserves the loader-owned clock and QMI/PSRAM configuration.
void board_init_inherited(void);

// Backlight: 0 = off, nonzero = on (plain GPIO).
void board_backlight_set(uint8_t level);

// I2C1 @ 400 kHz on GPIO 26 (SDA) / 27 (SCL). Called from board_init_clk().
void board_i2c1_init(void);

// Recover a timed-out/wedged I2C1 bus, then restore the controller and pins.
// Drivers may call this after repeated bounded-transfer failures.
void board_i2c1_recover(void);

#endif
