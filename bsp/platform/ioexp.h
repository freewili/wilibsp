// bsp/platform/ioexp.h — PCAL6524 I/O expander on I2C1 (addr 0x23).
//
// On the FreeWili2 the sub-GHz section has TWO radios (CC1101 + LoRa) selected by
// the expander pins V1_1 and V2_1: driving BOTH high routes the CC1101 + sub-GHz
// antenna onto the shared SPI1 bus (verified on-target). At power-on the expander
// pins are high-Z, so the CC1101 is off the bus until this runs. The CC1101 shares
// SPI1 with the LCD; its MISO is GPIO8 (muxed with LCD DC by the spi_bus arbiter)
// and its chip-select is GPIO40.
//
// ioexp_init() also releases the LCD reset (SCREEN_NRST), routes the backlight
// (GPIO25) to the RP2350, sets the SPI1 bus buffer directions, and leaves P1_6
// an input (the CEC wire on father board rev 2, the header I2C0 pull-up
// enable on rev 1). Call after I2C1 is up and before using the display or radio.
// It also puts the user-GPIO reference voltage on the external pin — see the
// VIO section below before driving any header GPIO.
#ifndef IOEXP_H
#define IOEXP_H
#include <stdbool.h>
#include <stdint.h>

// Antenna select via the expander pins V1_1 (P1 bit3) + V2_1 (P1 bit1). Per the
// FreeWili2 schematic the 2-bit value routes one of four antennas. Values 1..3
// keep the CC1101 on SPI1; value 0 routes the LoRa path.
enum {
    ANT_LORA            = 0,   // LoRa radio + LoRa antenna
    ANT_CC1101_433      = 1,   // CC1101 + 433 MHz antenna
    ANT_CC1101_315_415  = 2,   // CC1101 + 315/415 MHz antenna
    ANT_CC1101_915      = 3,   // CC1101 + 915 MHz antenna
};

bool ioexp_init(void);         // returns true on I2C ACK; defaults to the CC1101 433 antenna
void ioexp_antenna(uint8_t sel);   // route one of ANT_* (drives V1_1/V2_1)
// MIC_PWR (P1 bit 7, active-high): power rail for the 4 PDM MEMS microphones.
// Off at power-on and after ioexp_init(); pdm_capture_init() turns it on and
// waits ~50 ms for the mics to settle. Preserves the antenna-select bits.
void ioexp_mic_pwr(bool on);
// IR_PWR (P2 bit 0, active-high): power rail for the IR receiver (and possibly
// the TX LED driver). Off at power-on and after ioexp_init(); ir_capture_init()
// turns it on. Pin table: sensorview ioexp_pcal6524.h (IOEXP_IR_PWR = P2_0).
void ioexp_ir_pwr(bool on);
// USB host port power (CH334F hub rails): HP1 = P0 bit 0, HP2 = P1 bit 4,
// both active-high (pin table: sensorview ioexp_pcal6524.h). Off at power-on;
// usb_store_init() turns both on before enumeration.
void ioexp_usb_pwr(bool on);
// USB DEVICE D+ 1.5K pull-up enable (PCAL6524 P2 bit 1, active-high). Distinct
// from ioexp_usb_pwr() (the CH334F HOST hub port power). Off after ioexp_init()
// (P2_1 is configured as an output driven low = device detached); the PIO-USB
// device firmware asserts it AFTER its stack is ready to signal USB attach.
void ioexp_usb_dplus(bool on);

// ---------------------------------------------------------------------------
// GPIO reference voltage (VIO). READ THIS BEFORE DRIVING ANY HEADER GPIO.
//
// The FreeWili2's user GPIO header is LEVEL SHIFTED, and the shifters are dead
// until one of four expander pins connects a rail to VIO. A main-CPU GPIO with
// no VIO still toggles internally and still reads back correctly over OneWili —
// the header pin simply does not move. There is no error and nothing in the log
// to tell you: the only symptom is a dead pin. If you are driving the header,
// you must know which rail is on it.
//
// ioexp_init() defaults to VREF_EXT_PIN, matching the stock FreeWili2 firmware
// (Fw2Display.cpp boots at fw2VREFConnection::vVIO). That is a rail supplied by
// whatever is wired to the external Trig_IN/VREF pin — so it is the right
// default for a board in a system, and NOT a usable level on a bare board.
// **An app that drives the header at a known logic level must call ioexp_vref()
// explicitly** (see apps/toggleled, apps/hello_vref).
//
// The four pins are mutually exclusive: ioexp_vref() clears all of them and then
// asserts at most one. Pin table + semantics mirror
// fw2IOExpanderDisplay::setVREFConnection() in the stock firmware; the labels in
// parentheses are what its "GPIO Voltage" panel shows.
//
// Verified on hardware 2026-07-26 — including what was NOT confirmed:
// docs/superpowers/findings/2026-07-26-gpio-vref-e2e.md.
enum {
    VREF_NONE      = 0,   // all four disconnected — header pins cannot drive
    VREF_3V3       = 1,   // internal 3.3 V rail       ("3.3V",      P3V3_VREF, P2 bit 6)
    VREF_5V0       = 2,   // internal 5.0 V rail       ("5.0",       P5V_VREF,  P2 bit 5)
    VREF_EXT_PIN   = 3,   // external Trig_IN/VREF pin ("Ext Pin",   EXT_VREF,  P2 bit 3) — ioexp_init() default
    VREF_PROG_VOUT = 4,   // programmable Vout         ("Prog Vout", INT_VREF,  P2 bit 4)
                          //   needs the MAIN cpu to enable Vout first
                          //   (ow_io_analog_out_set_v_prog_vout) — untested with Vout on
};
void ioexp_vref(uint8_t sel);
// Which rail ioexp_vref() last selected (VREF_* ). VREF_EXT_PIN after ioexp_init().
uint8_t ioexp_vref_get(void);
#endif
