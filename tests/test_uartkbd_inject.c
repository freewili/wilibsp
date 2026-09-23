#include <string.h>
#include "uartkbd_parse.h"
#include "test_util.h"

/* Idle (all-released) wire values for the button bytes — active-low. */
#define IDLE2 0x3Fu
#define IDLE3 0x39u
#define IDLE4 0x80u
#define IDLE5 0x07u

static void mk_frame(uint8_t f[UARTKBD_FRAME_LEN],
                     uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5)
{
    memset(f, 0, UARTKBD_FRAME_LEN);
    f[0] = 0xBD; f[1] = 0x1D;
    f[2] = (uint8_t)(IDLE2 ^ b2); f[3] = (uint8_t)(IDLE3 ^ b3);
    f[4] = (uint8_t)(IDLE4 ^ b4); f[5] = (uint8_t)(IDLE5 ^ b5);
    uint8_t sum = 0;
    for (int i = 0; i < UARTKBD_FRAME_LEN - 1; i++) sum = (uint8_t)(sum + f[i]);
    f[UARTKBD_FRAME_LEN - 1] = sum;
}

static void feed(uartkbd_parser_t *p, const uint8_t *d, int n)
{
    for (int i = 0; i < n; i++) uartkbd_parse_byte(p, d[i]);
}

static void feed_idle(uartkbd_parser_t *p)
{
    uint8_t f[UARTKBD_FRAME_LEN];
    mk_frame(f, 0, 0, 0, 0);
    feed(p, f, UARTKBD_FRAME_LEN);
}

int main(void)
{
    uartkbd_parser_t p;
    uartkbd_event_t ev;

    /* Injected buttons produce real press/release edges with no wire traffic
     * carrying them. First frame primes, second frame carries the edge. */
    uartkbd_parse_init(&p);
    feed_idle(&p);                                  /* primes */
    uartkbd_parse_set_inject(&p, 1u << UARTKBD_BTN_GREEN);
    feed_idle(&p);
    ASSERT_TRUE(uartkbd_parse_next_event(&p, &ev));
    ASSERT_EQ(ev.btn, UARTKBD_BTN_GREEN);
    ASSERT_EQ(ev.pressed, 1);
    ASSERT_EQ(uartkbd_parse_buttons(&p) & (1u << UARTKBD_BTN_GREEN),
              1u << UARTKBD_BTN_GREEN);

    /* Clearing the mask releases it. */
    uartkbd_parse_set_inject(&p, 0);
    feed_idle(&p);
    ASSERT_TRUE(uartkbd_parse_next_event(&p, &ev));
    ASSERT_EQ(ev.btn, UARTKBD_BTN_GREEN);
    ASSERT_EQ(ev.pressed, 0);

    /* A HELD injection survives real frames reporting the button idle — this
     * is the whole reason the mask is applied inside decode. */
    uartkbd_parse_init(&p);
    feed_idle(&p);
    uartkbd_parse_set_inject(&p, 1u << UARTKBD_BTN_RED);
    feed_idle(&p);
    ASSERT_TRUE(uartkbd_parse_next_event(&p, &ev));   /* press */
    ASSERT_EQ(ev.pressed, 1);
    feed_idle(&p);
    feed_idle(&p);
    ASSERT_TRUE(!uartkbd_parse_next_event(&p, &ev));  /* no spurious release */
    ASSERT_EQ(uartkbd_parse_buttons(&p) & (1u << UARTKBD_BTN_RED),
              1u << UARTKBD_BTN_RED);

    /* A real press of a DIFFERENT button still works while injecting. */
    uint8_t f[UARTKBD_FRAME_LEN];
    mk_frame(f, 0x04, 0, 0, 0);                       /* GREEN held on the wire */
    feed(&p, f, UARTKBD_FRAME_LEN);
    ASSERT_TRUE(uartkbd_parse_next_event(&p, &ev));
    ASSERT_EQ(ev.btn, UARTKBD_BTN_GREEN);
    ASSERT_EQ(ev.pressed, 1);

    /* Injecting a button the wire also reports held produces exactly one edge. */
    uartkbd_parse_init(&p);
    feed_idle(&p);
    uartkbd_parse_set_inject(&p, 1u << UARTKBD_BTN_GREEN);
    mk_frame(f, 0x04, 0, 0, 0);
    feed(&p, f, UARTKBD_FRAME_LEN);
    ASSERT_TRUE(uartkbd_parse_next_event(&p, &ev));
    ASSERT_EQ(ev.pressed, 1);
    ASSERT_TRUE(!uartkbd_parse_next_event(&p, &ev));

    /* The getter reports what was set. */
    ASSERT_EQ(uartkbd_parse_inject(&p), 1u << UARTKBD_BTN_GREEN);

    /* Charger telemetry is untouched by injection. */
    uartkbd_parse_init(&p);
    memset(f, 0, sizeof f);
    f[0] = 0xBD; f[1] = 0x1D;
    f[2] = IDLE2; f[3] = IDLE3; f[4] = IDLE4; f[5] = IDLE5;
    f[10] = 0x42;
    uint8_t sum = 0;
    for (int i = 0; i < UARTKBD_FRAME_LEN - 1; i++) sum = (uint8_t)(sum + f[i]);
    f[UARTKBD_FRAME_LEN - 1] = sum;
    feed(&p, f, UARTKBD_FRAME_LEN);
    uartkbd_charger_t chg;
    ASSERT_TRUE(uartkbd_parse_charger(&p, &chg));

    /* The 20-byte status payload — where picpwr_rails_decode() reads live power
     * rail state — is latched from EVERY checksum-valid frame, with no notion
     * of whether that frame came off the wire. Anything that fabricates a frame
     * to carry an injected button edge therefore has to save and restore it, or
     * an injected press reports every rail as off and an application holding
     * rails with picpwr_keep_awake() re-asserts an awake mask built from that
     * all-zero snapshot — switching off every zone missing from it, the LCD and
     * the debug probe included. These two asserts pin the behaviour that makes
     * the save/restore in uartkbd.c's synth_frame() necessary. */
    uartkbd_parse_init(&p);
    memset(f, 0, sizeof f);
    f[0] = 0xBD; f[1] = 0x1D;
    f[2] = IDLE2; f[3] = IDLE3; f[4] = IDLE4; f[5] = IDLE5;
    f[6] = 0xA2; f[7] = 0xC7;                  /* rail bits, arbitrary non-zero */
    sum = 0;
    for (int i = 0; i < UARTKBD_FRAME_LEN - 1; i++) sum = (uint8_t)(sum + f[i]);
    f[UARTKBD_FRAME_LEN - 1] = sum;
    feed(&p, f, UARTKBD_FRAME_LEN);
    uint8_t status[20];
    ASSERT_TRUE(uartkbd_parse_status_raw(&p, status));
    ASSERT_EQ(status[4], 0xA2);                /* payload[i] == frame[2 + i] */
    ASSERT_EQ(status[5], 0xC7);

    /* An idle frame with an all-zero payload — exactly what a synthetic
     * injection frame carries — overwrites it. */
    feed_idle(&p);
    ASSERT_TRUE(uartkbd_parse_status_raw(&p, status));
    ASSERT_EQ(status[4], 0x00);
    ASSERT_EQ(status[5], 0x00);

    TEST_RETURN();
}
