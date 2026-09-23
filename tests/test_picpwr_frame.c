#include <string.h>
#include "input/picpwr_frame.h"
#include "test_util.h"

/* Frame construction against the documented worked example:
 * zones 1-17 awake, sleep and wake fields zero. */
static void test_frame_worked_example(void)
{
    const uint8_t expect[PICPWR_FRAME_LEN] = {
        0xB0, 0x00, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAF,
    };
    picpwr_cfg_t cfg = { .awake = 0x01FFFFu, .sleep = 0, .wake = 0,
                         .wake2 = 0 };
    uint8_t out[PICPWR_FRAME_LEN];
    ASSERT_EQ(picpwr_frame_build(out, &cfg), PICPWR_FRAME_LEN);
    ASSERT_TRUE(memcmp(out, expect, sizeof expect) == 0);
}

/* Masks serialize most-significant byte first, independently per field. */
static void test_frame_mask_placement(void)
{
    picpwr_cfg_t cfg = { .awake = 0x013456u, .sleep = 0x01CDEFu,
                         .wake = 0x5A, .wake2 = 0xA5 };
    uint8_t out[PICPWR_FRAME_LEN];
    picpwr_frame_build(out, &cfg);
    ASSERT_EQ(out[2], 0x01); ASSERT_EQ(out[3], 0x34); ASSERT_EQ(out[4], 0x56);
    ASSERT_EQ(out[5], 0x01); ASSERT_EQ(out[6], 0xCD); ASSERT_EQ(out[7], 0xEF);
    ASSERT_EQ(out[8], 0x5A); ASSERT_EQ(out[9], 0xA5);
    uint8_t sum = 0;
    for (int i = 0; i < PICPWR_FRAME_LEN - 1; i++)
        sum = (uint8_t)(sum + out[i]);
    ASSERT_EQ(out[PICPWR_FRAME_LEN - 1], sum);
}

/* Reserved mask bits (above zone 17) can never reach the wire: the
 * builder strips them from both masks regardless of the caller's cfg
 * (docs/drivers/power.md). This test pins that guarantee. */
static void test_frame_reserved_bits_stripped(void)
{
    picpwr_cfg_t cfg = { .awake = 0xFFFFFFu, .sleep = 0xFE0000u,
                         .wake = 0, .wake2 = 0 };
    uint8_t out[PICPWR_FRAME_LEN];
    picpwr_frame_build(out, &cfg);
    ASSERT_EQ(out[2], 0x01);   /* awake byte 0: only zone 17 survives */
    ASSERT_EQ(out[3], 0xFF);
    ASSERT_EQ(out[4], 0xFF);
    ASSERT_EQ(out[5], 0x00);   /* sleep byte 0: reserved-only mask -> 0 */
    ASSERT_EQ(out[6], 0x00);
    ASSERT_EQ(out[7], 0x00);
    ASSERT_EQ(PICPWR_ZONE_MASK_ALL, 0x01FFFFu);
}

/* Zone bit helper: zone N = bit N-1. */
static void test_zone_bits(void)
{
    ASSERT_EQ(picpwr_zone_bit(PICPWR_ZONE_SENSORS), 0x000001u);
    ASSERT_EQ(picpwr_zone_bit(PICPWR_ZONE_DISPLAY), 0x000002u);
    ASSERT_EQ(picpwr_zone_bit(PICPWR_ZONE_AUDIO), 0x000004u);
    ASSERT_EQ(picpwr_zone_bit(PICPWR_ZONE_STATUS_LED), 0x000100u);
    ASSERT_EQ(picpwr_zone_bit(PICPWR_ZONE_CAN), 0x004000u);
    ASSERT_EQ(picpwr_zone_bit(PICPWR_ZONE_DEBUG_PROBE), 0x008000u);
    ASSERT_EQ(picpwr_zone_bit(PICPWR_ZONE_COMPUTE), 0x010000u);
}

/* Rail-state extraction from a synthetic status payload. */
static void test_rails_decode(void)
{
    uint8_t p[20];
    memset(p, 0, sizeof p);
    ASSERT_EQ(picpwr_rails_decode(p), 0);

    p[3] = 0x28;                 /* zones 1 (0x08) + 3 (0x20) */
    p[6] = 0x04;                 /* zone 15 */
    ASSERT_EQ(picpwr_rails_decode(p),
              picpwr_zone_bit(1) | picpwr_zone_bit(3)
                  | picpwr_zone_bit(15));

    /* All payload bits set: exactly zones 1-17, nothing above leaks in. */
    memset(p, 0xFF, sizeof p);
    ASSERT_EQ(picpwr_rails_decode(p), 0x01FFFFu);
}

/* A re-assert is a SUPERSET of what was already requested. Because any
 * zone bit left clear in awake switches that rail OFF, a mask rebuilt
 * from a status snapshot switches off every rail the snapshot failed to
 * report (docs/drivers/power.md). These cases pin that it cannot. */
static void test_reassert_is_superset(void)
{
    const uint32_t audio = picpwr_zone_bit(PICPWR_ZONE_AUDIO);
    const uint32_t probe = picpwr_zone_bit(PICPWR_ZONE_DEBUG_PROBE);
    const uint32_t disp  = picpwr_zone_bit(PICPWR_ZONE_DISPLAY);

    /* The regression: a zone that was sent, but that a stale snapshot
     * reports off and that is not in `desired`, must still survive.
     * Rebuilding from (rails | desired) alone would drop it. */
    ASSERT_EQ(picpwr_reassert_awake(disp | probe, audio, audio),
              disp | probe | audio);

    /* Every input is retained; nothing a caller asked for is cleared. */
    ASSERT_EQ(picpwr_reassert_awake(disp, probe, audio),
              disp | probe | audio);

    /* An all-zero snapshot cannot clear the cached request. */
    ASSERT_EQ(picpwr_reassert_awake(disp | audio, 0, 0), disp | audio);

    /* Monotonic: the result is always a superset of each input. */
    uint32_t r = picpwr_reassert_awake(disp, probe, audio);
    ASSERT_EQ(r & disp, disp);
    ASSERT_EQ(r & probe, probe);
    ASSERT_EQ(r & audio, audio);

    /* Reserved bits above zone 17 never survive, whichever input
     * carried them. */
    ASSERT_EQ(picpwr_reassert_awake(0xFE0000u, 0, audio), audio);
    ASSERT_EQ(picpwr_reassert_awake(0, 0xFE0000u, audio), audio);
    ASSERT_EQ(picpwr_reassert_awake(0, 0, 0xFE0000u | audio), audio);
    ASSERT_EQ(picpwr_reassert_awake(0xFFFFFFu, 0xFFFFFFu, 0xFFFFFFu),
              PICPWR_ZONE_MASK_ALL);
}

/* A release clears exactly the released rails: every other live rail is
 * carried over, and a rail the app keeps survives even when the release
 * mask names it. */
static void test_release_clears_only_the_released(void)
{
    const uint32_t audio = picpwr_zone_bit(PICPWR_ZONE_AUDIO);
    const uint32_t leds  = picpwr_zone_bit(PICPWR_ZONE_RGB_LEDS);
    const uint32_t can   = picpwr_zone_bit(PICPWR_ZONE_CAN);
    const uint32_t disp  = picpwr_zone_bit(PICPWR_ZONE_DISPLAY);
    const uint32_t probe = picpwr_zone_bit(PICPWR_ZONE_DEBUG_PROBE);

    /* The bench case: rails inherited from the stock firmware (0xE3C7),
     * CAN kept, app-owned rails released -> audio and RGB LEDs go, the
     * rest stays. */
    ASSERT_EQ(picpwr_release_awake(0x00E3C7u, can, PICPWR_ZONE_MASK_APP_OWNED),
              0x00E3C7u & ~(audio | leds));

    /* A kept rail is never cleared, even if the release mask names it. */
    ASSERT_EQ(picpwr_release_awake(disp | audio | probe, audio, audio),
              disp | audio | probe);

    /* A kept rail that reads off is switched on by the same frame. */
    ASSERT_EQ(picpwr_release_awake(disp | audio, can, audio), disp | can);

    /* Releasing a rail that is already off changes nothing else. */
    ASSERT_EQ(picpwr_release_awake(disp | probe, 0, audio), disp | probe);

    /* Reserved bits never survive. */
    ASSERT_EQ(picpwr_release_awake(0xFFFFFFu, 0, audio),
              PICPWR_ZONE_MASK_ALL & ~audio);
}

/* The app-owned set is the display CPU's own peripherals and nothing that
 * serves the main CPU or the session (display, SD, USB, probe, CAN...). */
static void test_app_owned_set(void)
{
    ASSERT_EQ(PICPWR_ZONE_MASK_APP_OWNED,
              picpwr_zone_bit(PICPWR_ZONE_AUDIO) | picpwr_zone_bit(PICPWR_ZONE_SUBGHZ) |
              picpwr_zone_bit(PICPWR_ZONE_RGB_LEDS) | picpwr_zone_bit(PICPWR_ZONE_NFC_RFID));
}

int main(void)
{
    test_release_clears_only_the_released();
    test_app_owned_set();
    test_frame_worked_example();
    test_frame_mask_placement();
    test_frame_reserved_bits_stripped();
    test_zone_bits();
    test_rails_decode();
    test_reassert_is_superset();
    if (g_failures == 0) printf("test_picpwr_frame: all passed\n");
    TEST_RETURN();
}
