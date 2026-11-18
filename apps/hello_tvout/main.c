// hello_tvout — the app's own picture on a TV and on the LCD at once, using
// bsp/display/tvout (docs/drivers/tvout.md).
//
// Two 320x240 framebuffers live in SRAM. Each frame is drawn into the hidden
// one, shown on the TV with tvout_show() (a page flip: the TV never shows a
// half-drawn frame) and flushed to the LCD, centred. Pixels are RGB565 with
// the high byte first, the LCD's wire order, so one buffer feeds both.
//
// GREEN shows the same buffers at twice their size, filling the TV screen
// (tvout_start_2x()), and back to 1x. YELLOW turns sound on the TV on and
// off: a 500 Hz tone the app writes, carried by a picture started with audio
// framing. Plugging the TV cable in and out changes the plug line in the
// picture (needs coprocessor firmware that reads the hot-plug line).
//
// What you should see: a yellow square bouncing on a blue picture, on the TV
// and the LCD together; at 2x the TV picture fills the screen; with sound on
// the TV plays the tone.
#include "fw2.h"
#include "display/dvi_osd.h"
#include "platform/diag.h"
#include "pico/stdlib.h"
#include <math.h>
#include <string.h>

// RGB565 written in the LCD's wire order (high byte first).
#define WIRE(rgb) ((uint16_t)((((rgb) & 0xFFu) << 8) | (((rgb) >> 8) & 0xFFu)))
#define C_BLACK   WIRE(0x0000)
#define C_WHITE   WIRE(0xFFFF)
#define C_NAVY    WIRE(0x0010)
#define C_YELLOW  WIRE(0xFFE0)

// The boxes above the GREEN and YELLOW buttons (AGENTS.md, the button row).
#define KEY_W     ((ST7796_W - 12) / 5)
#define KEY_PITCH (KEY_W + 3)
#define KEY_H     40
#define KEY_Y     (ST7796_H - KEY_H)

#define FB_W  320
#define FB_H  240
#define LCD_X ((ST7796_W - FB_W) / 2)
#define LCD_Y ((KEY_Y - FB_H) / 2)
#define BOX   32
#define TOP   72                        // the square stays below the text

static uint16_t s_fb[2][FB_W * FB_H];   // in SRAM, where the TV reads it in place
static int s_back;                      // the buffer drawn next; the other one is shown
static bool s_2x, s_sound;
static const char *s_plug = "TV NOT SEEN YET";

// The written tone: 500 Hz at 48 kHz is 96 frames a period.
#define TONE_STEPS 96
static int16_t s_tone[TONE_STEPS];
static unsigned s_phase;

// (Re)starts the picture from the buffer shown last. Audio framing is fixed
// when a picture starts, so turning sound on or off means a restart.
static void start_picture(void) {
    tvout_stop();
    tvout_set_audio_framing(s_sound);   // off by default: a plain DVI monitor may not show it
    const uint16_t *shown = s_fb[1 - s_back];
    const bool ok = s_2x ? tvout_start_2x(shown, FB_W, FB_H) : tvout_start(shown, FB_W, FB_H);
    if (!ok) DIAG("hello_tvout: %s\n", tvout_last_error());
    if (ok && s_sound && !tvout_audio_start()) DIAG("hello_tvout: %s\n", tvout_audio_last_error());
}

// Fills whatever room the TV's sound buffer has (0 while sound is off).
// Once per loop pass is enough: the buffer holds 42 ms.
static void feed_sound(void) {
    int16_t stereo[2 * 32];
    for (unsigned room = tvout_audio_room(); room > 0;) {
        const unsigned n = room < 32 ? room : 32;
        for (unsigned i = 0; i < n; i++)
            stereo[2 * i] = stereo[2 * i + 1] = s_tone[(s_phase + i) % TONE_STEPS];
        const unsigned taken = tvout_audio_write(stereo, n);
        s_phase = (s_phase + taken) % TONE_STEPS;
        if (taken < n) break;
        room -= n;
    }
}

static void key(int slot, uint16_t bg, uint16_t fg, const char *label) {
    const int x = slot * KEY_PITCH;
    st7796_fill_rect(x, KEY_Y, KEY_W, KEY_H, bg);
    st7796_draw_text(x + (KEY_W - (int)strlen(label) * 6) / 2, KEY_Y + (KEY_H - 8) / 2, 1, fg, bg, label);
}

static void draw_keys(void) {
    key(UARTKBD_BTN_YELLOW, 0x06FF, C_BLACK, s_sound ? "SOUND OFF" : "SOUND ON");
    key(UARTKBD_BTN_GREEN, 0x0012, C_WHITE, s_2x ? "1X" : "2X");
}

static void draw_frame(uint16_t *fb, int x, int y) {
    dvi_osd_fill_rect(fb, FB_W, FB_W, FB_H, 0, 0, FB_W, FB_H, C_NAVY);
    dvi_osd_fill_rect(fb, FB_W, FB_W, FB_H, x, y, BOX, BOX, C_YELLOW);
    dvi_osd_text(fb, FB_W, FB_W, FB_H, 8, 8, 2, C_WHITE, C_NAVY, "HELLO TV");
    dvi_osd_text(fb, FB_W, FB_W, FB_H, 8, 32, 1, C_WHITE, C_NAVY, s_2x ? "2X: FILLS THE SCREEN" : "1X: CENTRED");
    dvi_osd_text(fb, FB_W, FB_W, FB_H, 8, 44, 1, C_WHITE, C_NAVY, s_sound ? "SOUND ON" : "SOUND OFF");
    dvi_osd_text(fb, FB_W, FB_W, FB_H, 8, 56, 1, C_WHITE, C_NAVY, s_plug);
}

int main(void) {
    board_init();
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd();
    st7796_fill_screen(C_BLACK);
    board_backlight_set(1);
    DIAG("\n=== hello_tvout ===\n");

    for (int i = 0; i < TONE_STEPS; i++)
        s_tone[i] = (int16_t)lrintf(8192.0f * sinf(2.0f * 3.14159265f * (float)i / TONE_STEPS));

    int x = 0, y = TOP, dx = 2, dy = 2;
    draw_frame(s_fb[0], x, y);
    s_back = 1;
    start_picture();
    draw_keys();

    while (true) {
        fw2_app_recovery_task();
        picpwr_task();

        // Plug events: settled changes of the TV's hot-plug line.
        const tvout_plug_event_t plug = tvout_task();
        if (plug == TVOUT_PLUG_CONNECTED) s_plug = "TV CONNECTED";
        if (plug == TVOUT_PLUG_REMOVED) s_plug = "TV REMOVED";
        if (plug != TVOUT_PLUG_NONE) DIAG("hello_tvout: %s\n", s_plug);

        uartkbd_event_t ev;
        while (uartkbd_next_event(&ev)) {
            if (!ev.pressed) continue;
            if (ev.btn == UARTKBD_BTN_GREEN) s_2x = !s_2x;
            else if (ev.btn == UARTKBD_BTN_YELLOW) s_sound = !s_sound;
            else continue;
            st7796_flush_wait();
            start_picture();
            draw_keys();
        }

        feed_sound();

        x += dx;
        y += dy;
        if (x <= 0 || x + BOX >= FB_W) dx = -dx;
        if (y <= TOP || y + BOX >= FB_H) dy = -dy;

        // One LCD flush at a time. Then draw the hidden buffer, flip the TV to
        // it (tvout_show() waits for the TV to leave the picture rows) and send
        // it to the LCD.
        st7796_flush_wait();
        uint16_t *fb = s_fb[s_back];
        draw_frame(fb, x, y);
        if (tvout_running()) tvout_show(fb);
        st7796_flush_async(LCD_X, LCD_Y, LCD_X + FB_W - 1, LCD_Y + FB_H - 1, fb, NULL);
        s_back = 1 - s_back;
    }
}
