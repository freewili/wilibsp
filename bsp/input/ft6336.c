// src/input/ft6336.c — FT6336U over I2C1, polled. No INT; RST shares the panel HW reset.
// Mirrors the proven freewili reference driver (rmpLib/rpCAPTouchFT6336): a single
// 6-byte read of regs 0x02..0x07 so TD_STATUS and the point latch are coherent.
// Coordinate mapping is delegated to ft6336_map_point (host-tested pure logic).
#include "input/ft6336.h"
#include "input/ft6336_map.h"
#include "hardware/i2c.h"
#include "pico/time.h"
#include "platform/board.h"
#include "platform/diag.h"

/* Bounded transfers: this bus is shared, and a device that loses its
 * supply or is reset mid-transfer can hold it. */
#define I2C_XFER_TIMEOUT_US 2000
#define I2C_FAILURES_BEFORE_RECOVERY 8u
#define I2C_RECOVERY_MIN_INTERVAL_MS 250u

#define FT6336_I2C             i2c1
#define FT6336_ADDR            0x38
#define FT6336_REG_DEVICE_MODE 0x00   // 0 = normal operating mode
#define FT6336_REG_TD_STATUS   0x02   // low nibble = number of touch points
#define FT6336_REG_CHIP_ID     0xA3   // chip-id register (presence check)

// Write the register pointer (no stop), then read n bytes. TIMEOUT reads:
// this poll runs every UI-loop pass, and an untimed blocking read turned a
// wedged I2C bus into a permanently hung UI (bench incident 2026-07-29,
// first real two-finger session — cores alive, UI parked in i2c waits).
// 2 ms is ~10x a normal 11-byte transaction at 400 kHz.
static uint32_t s_i2c_errors;
static uint32_t s_i2c_recoveries;
static uint32_t s_consecutive_errors;
static absolute_time_t s_next_recovery;

static void ft_xfer_result(bool ok) {
    if (ok) {
        s_consecutive_errors = 0;
        return;
    }
    s_i2c_errors++;
    if (++s_consecutive_errors < I2C_FAILURES_BEFORE_RECOVERY ||
        !time_reached(s_next_recovery))
        return;
    board_i2c1_recover();
    s_i2c_recoveries++;
    s_consecutive_errors = 0;
    s_next_recovery = make_timeout_time_ms(I2C_RECOVERY_MIN_INTERVAL_MS);
    DIAG("ft6336: I2C recovery errors=%u recoveries=%u\n",
         (unsigned)s_i2c_errors, (unsigned)s_i2c_recoveries);
}

static bool ft_rd(uint8_t reg, uint8_t* buf, uint8_t n) {

    bool ok = i2c_write_timeout_us(FT6336_I2C, FT6336_ADDR, &reg, 1, true,
                                   I2C_XFER_TIMEOUT_US) == 1;
    if (ok)
        ok = i2c_read_timeout_us(FT6336_I2C, FT6336_ADDR, buf, n, false,
                                 I2C_XFER_TIMEOUT_US) == (int)n;
    ft_xfer_result(ok);
    return ok;
}

// Write one register = val.
static bool ft_wr(uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    bool ok = i2c_write_timeout_us(FT6336_I2C, FT6336_ADDR, b, 2, false,
                                   I2C_XFER_TIMEOUT_US) == 2;
    ft_xfer_result(ok);
    return ok;
}

bool ft6336_init(void) {
    s_i2c_errors = 0;
    s_i2c_recoveries = 0;
    s_consecutive_errors = 0;
    s_next_recovery = get_absolute_time();
    uint8_t id = 0;
    bool ok = ft_rd(FT6336_REG_CHIP_ID, &id, 1);
    (void)ft_wr(FT6336_REG_DEVICE_MODE, 0x00); // non-fatal: mode defaults to normal; don't gate ok on this
    DIAG("ft6336: init %s id=0x%02X\n", ok ? "ok" : "NO-ACK", (unsigned)id);
    return ok;
}

static uint16_t s_inj_x, s_inj_y;
static bool     s_inj_down;
static uint32_t s_inj_reads;

void ft6336_inject_set(uint16_t x, uint16_t y, bool down) {
    // An injected point arrives from the agentio channel as plain text, so it
    // has not been through the chip mapping that bounds a real touch. Clamp it
    // to the panel: ft6336_poll promises screen coordinates either way.
    ft_point_t p = ft6336_clamp_point((int)x, (int)y);
    s_inj_x = p.x;
    s_inj_y = p.y;
    s_inj_down = down;
    if (down) s_inj_reads = 0;   // fresh count per injected press
}

uint32_t ft6336_inject_reads(void) { return s_inj_reads; }
uint32_t ft6336_i2c_errors(void) { return s_i2c_errors; }
uint32_t ft6336_i2c_recoveries(void) { return s_i2c_recoveries; }

int ft6336_poll2(uint16_t* x1, uint16_t* y1, uint16_t* x2, uint16_t* y2) {
    if (s_inj_down) {            // injected point wins; no I2C traffic at all
        *x1 = s_inj_x;
        *y1 = s_inj_y;
        s_inj_reads++;
        return 1;
    }
    // One transaction over regs 0x02..0x0C: TD_STATUS + both point latches
    // coherent (P1 at 0x03, weight/misc at 0x07/0x08, P2 at 0x09).
    uint8_t b[11];
    if (!ft_rd(FT6336_REG_TD_STATUS, b, 11)) return 0;
    int n = b[0] & 0x0F;
    if (n < 1 || n > 2) return 0;
    ft_point_t p = ft6336_map_point((((int)(b[1] & 0x0F)) << 8) | b[2],
                                    (((int)(b[3] & 0x0F)) << 8) | b[4]);
    *x1 = p.x;
    *y1 = p.y;
    if (n == 2) {
        p = ft6336_map_point((((int)(b[7] & 0x0F)) << 8) | b[8],
                             (((int)(b[9] & 0x0F)) << 8) | b[10]);
        *x2 = p.x;
        *y2 = p.y;
    }
    return n;
}

bool ft6336_poll(uint16_t* x, uint16_t* y) {
    if (s_inj_down) {            // injected point wins; no I2C traffic at all
        *x = s_inj_x;
        *y = s_inj_y;
        s_inj_reads++;
        return true;
    }
    // One transaction over regs 0x02..0x07 so TD_STATUS and the point latch are coherent.
    uint8_t b[6];
    if (!ft_rd(FT6336_REG_TD_STATUS, b, 6)) return false;
    if ((b[0] & 0x0F) != 1) return false;            // require exactly one touch point
    // b[1]=P1_XH b[2]=P1_XL (chip X) ; b[3]=P1_YH b[4]=P1_YL (chip Y).
    int chip_x = (((int)(b[1] & 0x0F)) << 8) | b[2];
    int chip_y = (((int)(b[3] & 0x0F)) << 8) | b[4];
    ft_point_t p = ft6336_map_point(chip_x, chip_y);
    *x = p.x;
    *y = p.y;
    return true;
}
