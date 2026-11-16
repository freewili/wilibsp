// hello_cec — the board as a CEC playback device on the TV connector, using
// bsp/cec (docs/drivers/cec.md).
//
// cec_start() joins the bus: the board claims a logical address and shows up
// in the TV's input list as "FW2 hello_cec". The colour bars go to the TV
// (bsp/display/tvout) so the board's input shows a picture.
//
// GREEN wakes the TV and switches it to this board (cec_wake_tv()); RED puts
// the TV in standby. YELLOW asks the TV for its power status: a request runs
// in the background, driven by cec_task(), and its answer is picked up later
// with cec_request_result(). With the TV on this board's input, the TV
// remote's arrows move the square and every key is named on the LCD; the TV
// going to standby is reported too.
//
// The physical address (which TV input the board is on) comes from the
// firmware when the app is started from the FreeWili app list or the
// DISPLAY's own menu; started another way it is 1.0.0.0 until the TV is
// switched to this board's input once, which teaches it.
//
// What you should see: the TV lists "FW2 hello_cec"; GREEN turns the TV on
// to the colour bars; RED turns it off; the remote's arrows move the square.
#include "fw2.h"
#include "platform/diag.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

// Colours in the LCD's wire order (high byte first).
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_YELLOW 0x06FF

// The boxes above the YELLOW, GREEN and RED buttons (AGENTS.md, the button row).
#define KEY_W     ((ST7796_W - 12) / 5)
#define KEY_PITCH (KEY_W + 3)
#define KEY_H     40
#define KEY_Y     (ST7796_H - KEY_H)

// The area the remote's arrows move the square in.
#define PAD_Y  130
#define PAD_H  (KEY_Y - 8 - PAD_Y)
#define SQUARE 16

static int s_x = (ST7796_W - SQUARE) / 2, s_y = PAD_Y + (PAD_H - SQUARE) / 2;

// One line of 2x text on the LCD, also over RTT.
static void say(int row, const char *s) {
    const int y = 8 + row * 22;
    st7796_fill_rect(0, y, ST7796_W, 16, C_BLACK);
    st7796_draw_text(8, y, 2, C_WHITE, C_BLACK, s);
    DIAG("hello_cec: %s\n", s);
}

static void key(int slot, uint16_t bg, uint16_t fg, const char *label) {
    const int x = slot * KEY_PITCH;
    st7796_fill_rect(x, KEY_Y, KEY_W, KEY_H, bg);
    st7796_draw_text(x + (KEY_W - (int)strlen(label) * 6) / 2, KEY_Y + (KEY_H - 8) / 2, 1, fg, bg, label);
}

static void move_square(int dx, int dy) {
    st7796_fill_rect(s_x, s_y, SQUARE, SQUARE, C_BLACK);
    s_x += dx;
    s_y += dy;
    if (s_x < 0) s_x = 0;
    if (s_x > ST7796_W - SQUARE) s_x = ST7796_W - SQUARE;
    if (s_y < PAD_Y) s_y = PAD_Y;
    if (s_y > PAD_Y + PAD_H - SQUARE) s_y = PAD_Y + PAD_H - SQUARE;
    st7796_fill_rect(s_x, s_y, SQUARE, SQUARE, C_YELLOW);
}

static void on_event(const cec_event_t *e) {
    char s[48];
    switch (e->type) {
    case CEC_EV_ADDRESS: {
        const uint16_t pa = cec_physical_address();
        snprintf(s, sizeof s, "ADDRESS %u, INPUT %u.%u.%u.%u (%s)", e->data, pa >> 12, (pa >> 8) & 15,
                 (pa >> 4) & 15, pa & 15, cec_address_source_name(cec_physical_address_source()));
        say(1, s);
        break;
    }
    case CEC_EV_KEY_DOWN:
    case CEC_EV_KEY_REPEAT:     // a held key repeats
        if (e->data == CEC_KEY_UP) move_square(0, -SQUARE);
        if (e->data == CEC_KEY_DOWN) move_square(0, SQUARE);
        if (e->data == CEC_KEY_LEFT) move_square(-SQUARE, 0);
        if (e->data == CEC_KEY_RIGHT) move_square(SQUARE, 0);
        snprintf(s, sizeof s, "KEY %s", cec_key_name(e->data));
        say(2, s);
        break;
    case CEC_EV_STANDBY:
        say(3, "TV WENT TO STANDBY");
        break;
    case CEC_EV_ACTIVE_SOURCE:
        say(3, e->data ? "TV SHOWS THIS BOARD" : "TV SWITCHED AWAY");
        break;
    default:
        break;
    }
}

int main(void) {
    board_init();
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd();
    st7796_fill_screen(C_BLACK);
    board_backlight_set(1);
    DIAG("\n=== hello_cec ===\n");

    if (!tvout_start_test()) DIAG("hello_cec: TV picture: %s\n", tvout_last_error());
    say(0, "HELLO CEC");
    if (!cec_start("FW2 hello_cec")) say(1, cec_last_error());
    key(UARTKBD_BTN_YELLOW, 0x06FF, C_BLACK, "TV POWER?");
    key(UARTKBD_BTN_GREEN, 0x0012, C_WHITE, "WAKE TV");
    key(UARTKBD_BTN_RED, 0x0780, C_WHITE, "TV STANDBY");
    move_square(0, 0);

    while (true) {
        fw2_app_recovery_task();
        picpwr_task();
        cec_task();             // every pass: moves frames between the wire and the device

        cec_event_t e;
        while (cec_next_event(&e)) on_event(&e);

        cec_reply_t r;
        if (cec_request_result(&r)) {
            char s[48];
            if (r.result == CEC_RESULT_ANSWERED)
                snprintf(s, sizeof s, "TV POWER: %s", cec_power_status_name(r.power_status));
            else
                snprintf(s, sizeof s, "TV POWER: %s", cec_result_name(r.result));
            say(4, s);
        }

        uartkbd_event_t ev;
        while (uartkbd_next_event(&ev)) {
            if (!ev.pressed) continue;
            if (ev.btn == UARTKBD_BTN_GREEN) cec_wake_tv();
            if (ev.btn == UARTKBD_BTN_RED) cec_tv_standby();
            if (ev.btn == UARTKBD_BTN_YELLOW && !cec_request_busy()) cec_request_power_status(CEC_LOGICAL_TV);
        }
        fw2_app_recovery_sleep_ms(2);
    }
}
