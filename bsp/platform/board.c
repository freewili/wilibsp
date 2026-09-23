// src/platform/board.c
// Adapted from evaderkrub/usbcamfw — MIT, (c) 2026 Dave Robins.
#include "platform/board.h"
#include "platform/ioexp.h"
#include "platform/spi_bus.h"
#include "platform/diag.h"
#include "leds/ws2812_driver.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/psram.h"
#include "hardware/resets.h"
#include "hardware/vreg.h"

static bool s_psram_bootstrapped;

void board_init(void) {
    if (s_psram_bootstrapped) {
        board_init_inherited();
        return;
    }
    board_init_clk(BOARD_SYS_CLOCK_KHZ);
}

static void board_init_peripherals(bool clear_leds) {
    if (clear_leds) {
    DIAG("board: leds\n");
    /* Loadable apps inherit peripheral register state from the previous
     * DISPLAY image, but start with fresh SDK claim bookkeeping in .bss.
     * Reset PIO1 before claiming an SM so hardware and software agree. */
    reset_block(RESETS_RESET_PIO1_BITS);
    unreset_block_wait(RESETS_RESET_PIO1_BITS);
    ws2812_clear_once(pio1, PIN_LED_DATA);
    }
    DIAG("board: spi\n");
    spi_bus_init();
    DIAG("board: radio cs\n");
    gpio_init(PIN_CC1101_CS);
    gpio_set_dir(PIN_CC1101_CS, GPIO_OUT);
    gpio_put(PIN_CC1101_CS, 1);
    DIAG("board: backlight\n");
    gpio_init(PIN_LCD_BL);
    gpio_set_dir(PIN_LCD_BL, GPIO_OUT);
    board_backlight_set(0);
    DIAG("board: i2c\n");
    board_i2c1_init();
    DIAG("board: ioexp\n");
    ioexp_init();
    DIAG("board: peripherals ready\n");
}

void board_init_inherited(void) { board_init_peripherals(true); }

void board_init_clk(uint32_t sys_clock_khz) {
    DIAG("board: vreg\n");
    // Raise the core voltage before overclocking. The earlier 252 MHz fault was
    // marginal Vcore at 1.15 V during the heavy st7796 bring-up; the firmware runs
    // from RAM (copy_to_ram) so flash XIP timing doesn't cap clk_sys. 1.25 V gives
    // solid headroom for the overclock (250 MHz default; DVI apps pass 252).
    vreg_set_voltage(VREG_VOLTAGE_1_25);
    DIAG("board: vreg delay\n");
    busy_wait_us_32(10000);
    DIAG("board: sys clock\n");
    set_sys_clock_khz(sys_clock_khz, true);

    // After overclocking sys, re-source the peripheral clock from clk_sys so the
    // hardware SPI peripheral has a valid clock. WITHOUT this the SPI clock is dead
    // and the LCD shows nothing — the working reference driver does exactly this.
    uint32_t f = clock_get_hz(clk_sys);
    DIAG("board: peri clock\n");
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, f, f);

    // WS2812 pixels retain their last latched colors across an RP2350 reset.
    // Clear them before the app claims pio1. The helper sends the required
    // second frame and releases its state machine and instruction memory.
    // Re-time PSRAM for the new clk_sys. The SDK brings PSRAM up during
    // runtime_init, BEFORE main() — i.e. at the boot clock — and nothing in
    // hardware_clocks re-runs it when clk_sys changes, so the QMI M1 timing is
    // stale the moment we overclock. Both calls are required: psram_set_params()
    // (reached via psram_configure_params) only stores the values in statics; the
    // QMI timing register is written by psram_reinitialize(). psram_reinitialize()
    // is documented unsafe while executing from flash or PSRAM — safe here because
    // every app is copy_to_ram (invariant 2) and core1 is not running yet.
    DIAG("board: psram params\n");
    psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,
                           PICO_DEFAULT_PSRAM_MAX_SELECT,
                           PICO_DEFAULT_PSRAM_MIN_DESELECT);
    DIAG("board: psram retime\n");
    psram_reinitialize();
    DIAG("board: psram ready\n");

    // Bring up the shared SPI1 bus now that clk_peri is live, so BOTH the display
    // and a radio-only app (which never calls st7796_init) find the SSP enabled.
    // Without this, cc1101 SPI reads spin forever on a disabled peripheral.
    // Park the CC1101 radio's SPI CS high before any LCD traffic so it never
    // drives lines shared with the LCD.
    // Backlight as a plain GPIO, matching the working reference (its PWM path is
    // disabled). PWM dimming returns in a later phase — one fewer bring-up variable.
    // I2C1 for touch (FT6336U), the PCAL6524 I/O expander, and sensors.
    // PCAL6524: release LCD reset, enable I2C pulls, route the backlight to the
    // RP2350, set SPI1 buffer directions, and SELECT the CC1101 + sub-GHz antenna
    // (V1_1/V2_1). The CC1101 is off the shared bus until this runs.
    board_init_peripherals(true);
}

void board_init_psram(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_25);
    busy_wait_us_32(10000);
    set_sys_clock_khz(BOARD_SYS_CLOCK_KHZ, true);
    uint32_t f = clock_get_hz(clk_sys);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, f, f);
    psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,
                           PICO_DEFAULT_PSRAM_MAX_SELECT,
                           PICO_DEFAULT_PSRAM_MIN_DESELECT);
    psram_reinitialize();
    board_init_peripherals(false);
    /* A PSRAM executable still calls board_init() at the start of main like
     * every FW2 app.  Remember that clocks/QMI are already ready so that call
     * initializes inherited peripherals without re-timing the executing bus. */
    s_psram_bootstrapped = true;
}

void board_backlight_set(uint8_t level) {
    gpio_put(PIN_LCD_BL, level != 0);
}

/* A device interrupted mid-transfer — a supply that moved under it, or a
 * reset while it was driving — can hold SDA low and wedge the bus for
 * every other device on it. Clocking SCL with SDA released lets that
 * device finish the byte it thinks it is sending, after which a STOP
 * returns the bus to idle. Runs before the I2C function is assigned. */
static void i2c1_bus_recover(void) {
    gpio_init(PIN_I2C1_SDA);
    gpio_init(PIN_I2C1_SCL);
    gpio_set_dir(PIN_I2C1_SDA, GPIO_IN);          /* released, pulled up */
    gpio_pull_up(PIN_I2C1_SDA);
    gpio_pull_up(PIN_I2C1_SCL);
    gpio_set_dir(PIN_I2C1_SCL, GPIO_OUT);
    gpio_put(PIN_I2C1_SCL, 1);
    busy_wait_us_32(10);
    if (gpio_get(PIN_I2C1_SDA)) {                 /* bus already idle */
        gpio_set_dir(PIN_I2C1_SCL, GPIO_IN);
        return;
    }
    for (int i = 0; i < 9 && !gpio_get(PIN_I2C1_SDA); i++) {
        gpio_put(PIN_I2C1_SCL, 0);
        busy_wait_us_32(5);
        gpio_put(PIN_I2C1_SCL, 1);
        busy_wait_us_32(5);
    }
    /* STOP: SDA low->high while SCL is high. */
    gpio_set_dir(PIN_I2C1_SDA, GPIO_OUT);
    gpio_put(PIN_I2C1_SDA, 0);
    busy_wait_us_32(5);
    gpio_put(PIN_I2C1_SCL, 1);
    busy_wait_us_32(5);
    gpio_set_dir(PIN_I2C1_SDA, GPIO_IN);
    busy_wait_us_32(5);
    gpio_set_dir(PIN_I2C1_SCL, GPIO_IN);
}

void board_i2c1_init(void) {
    i2c1_bus_recover();
    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(PIN_I2C1_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C1_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C1_SDA);
    gpio_pull_up(PIN_I2C1_SCL);
}

void board_i2c1_recover(void) {
    i2c_deinit(i2c1);
    board_i2c1_init();
}
