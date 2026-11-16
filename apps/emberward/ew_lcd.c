// ew_lcd.c — Emberward's companion screen (see ew_lcd.h).
#include "ew_lcd.h"

#include <stdio.h>
#include <string.h>

#include "display/st7796.h"
#include "ew_game.h"
#include "ew_render.h"

#define L_BG     EW_RGB(8, 10, 26)
#define L_TEXT   EW_RGB(230, 236, 255)
#define L_DIM    EW_RGB(110, 120, 160)
#define L_GOLD   EW_RGB(255, 206, 64)
#define L_EMBER  EW_RGB(255, 90, 50)
#define L_GOOD   EW_RGB(90, 230, 120)
#define L_RING   EW_RGB(40, 48, 80)
#define L_BLACK  EW_RGB(0, 0, 0)
#define L_WHITE  EW_RGB(255, 255, 255)

// Button box colours in wire order (AGENTS.md).
static const uint16_t KEY_BG[5] = { 0x9AD6, 0x06FF, 0x0012, 0xF800, 0x0780 };

static ew_lcd_info_t s_last;
static bool s_valid;

static void text(int x, int y, int scale, uint16_t fg, const char *s) {
    st7796_draw_text(x, y, scale, fg, L_BG, s);
}

// Text padded to a fixed number of columns so a shorter value covers a longer one.
static void field(int x, int y, int scale, uint16_t fg, int cols, const char *s) {
    char padded[48];
    snprintf(padded, sizeof padded, "%-*.*s", cols, cols, s);
    text(x, y, scale, fg, padded);
}

static void key_labels(const ew_lcd_info_t *in, const char *labels[5]) {
    for (int i = 0; i < 5; i++) labels[i] = 0;
    labels[0] = in->lcd_view ? "TV VIEW" : "LCD VIEW";
    static char sound[12], tvin[12], tvsnd[16];
    switch (in->screen) {
    case EW_SCREEN_TITLE:
        snprintf(sound, sizeof sound, "SOUND %s", in->sound ? "OFF" : "ON");
        snprintf(tvin, sizeof tvin, "TV INPUT %u", (unsigned)in->tv_input);
        labels[1] = sound;
        labels[2] = "START";
        labels[3] = tvin;
        snprintf(tvsnd, sizeof tvsnd, "TV SOUND %s", in->tv_sound ? "OFF" : "ON");
        labels[4] = tvsnd;
        break;
    case EW_SCREEN_PLAY:
        labels[1] = "< TURN";
        labels[2] = "FLARE";
        labels[3] = "TURN >";
        labels[4] = "PAUSE";
        break;
    case EW_SCREEN_PAUSE:
        labels[2] = "RESUME";
        labels[4] = "QUIT";
        break;
    case EW_SCREEN_ENTRY:
        labels[1] = "<";
        labels[2] = "OK";
        labels[3] = ">";
        break;
    default:
        labels[2] = "PLAY AGAIN";
        labels[4] = "TITLE";
        break;
    }
}

static void draw_keys(const ew_lcd_info_t *in) {
    const char *labels[5];
    key_labels(in, labels);
    st7796_fill_rect(0, EW_LCD_KEY_Y, 480, EW_LCD_KEY_H, L_BG);
    for (int i = 0; i < 5; i++) {
        if (!labels[i]) continue;
        const int x = i * EW_LCD_KEY_PITCH;
        const uint16_t fg = i <= 1 ? L_BLACK : L_WHITE;
        st7796_fill_rect(x, EW_LCD_KEY_Y, EW_LCD_KEY_W, EW_LCD_KEY_H, KEY_BG[i]);
        const int tx = x + (EW_LCD_KEY_W - (int)strlen(labels[i]) * 6) / 2;
        st7796_draw_text(tx, EW_LCD_KEY_Y + (EW_LCD_KEY_H - 8) / 2, 1, fg, KEY_BG[i], labels[i]);
    }
}

int ew_lcd_touch_slot(int x, int y) {
    if (y + 4 < EW_LCD_KEY_Y || x < 0 || x >= 480) return -1;
    const int slot = x / EW_LCD_KEY_PITCH;
    if (slot > 4 || x - slot * EW_LCD_KEY_PITCH >= EW_LCD_KEY_W) return -1;
    return slot;
}

static void lives_row(int x, int y, int lives) {
    for (int i = 0; i < EW_LIVES_MAX; i++)
        st7796_fill_rect(x + i * 22, y, 16, 16, i < lives ? L_EMBER : L_RING);
}

static void flare_bar(int x, int y, int w, uint16_t flare) {
    const bool full = flare >= EW_FLARE_FULL;
    const int fw = flare * (w - 4) / EW_FLARE_FULL;
    st7796_fill_rect(x, y, w, 14, L_RING);
    if (fw) st7796_fill_rect(x + 2, y + 2, fw, 10, full ? L_WHITE : L_GOLD);
}

static void status_lines(const ew_lcd_info_t *in, int x, int y, int cols) {
    char s[48];
    snprintf(s, sizeof s, "TV     %s", in->tv_seen ? "CONNECTED" : (in->tv_running ? "VIDEO ON" : "OFF"));
    field(x, y, 1, in->tv_seen ? L_GOOD : L_DIM, cols, s);
    snprintf(s, sizeof s, "REMOTE %s", in->cec_ok ? "READY" : "WAITING");
    field(x, y + 12, 1, in->cec_ok ? L_GOOD : L_DIM, cols, s);
    snprintf(s, sizeof s, "SCORES %s  SOUND %s", in->sd_ok ? "SAVED" : "RAM",
             !in->sound ? "OFF" : in->tv_sound_on ? "ON TV" : "SPEAKER");
    field(x, y + 24, 1, L_DIM, cols, s);
}

static void scores(const ew_lcd_info_t *in, int x, int y, int scale, int pitch) {
    char s[32];
    for (int i = 0; i < EW_SCORES; i++) {
        const ew_score_t *e = &in->save->entry[i];
        snprintf(s, sizeof s, "%d %s %7lu", i + 1, e->name, (unsigned long)e->score);
        text(x, y + i * pitch, scale, i == 0 ? L_GOLD : L_TEXT, s);
    }
}

static const char *help(const ew_lcd_info_t *in, int line) {
    static const char *const t[][2] = {
        [EW_SCREEN_TITLE] = { "BLOCK RED EMBERS, LET GOLD GLIMS IN.", "TV REMOTE: ARROWS TURN, SELECT STARTS." },
        [EW_SCREEN_PLAY]  = { "LEFT/RIGHT TURN THE WARD, UP/DOWN FLIP IT.", "OK OR SELECT FIRES A FULL FLARE." },
        [EW_SCREEN_PAUSE] = { "PAUSED.", "OK RESUMES, PAUSE AGAIN QUITS." },
        [EW_SCREEN_OVER]  = { "THE CORE WENT DARK.", "OK PLAYS AGAIN." },
        [EW_SCREEN_ENTRY] = { "NEW HIGH SCORE!", "UP/DOWN PICK A LETTER, LEFT/RIGHT MOVE." },
    };
    return t[in->screen][line];
}

// ------------------------------------------------------------ TV view

static void tv_view_values(const ew_lcd_info_t *in, bool all) {
    char s[32];
    if (all || in->score != s_last.score) {
        snprintf(s, sizeof s, "%07lu", (unsigned long)in->score);
        text(12, 76, 4, L_TEXT, s);
    }
    if (all || in->wave != s_last.wave || in->mult != s_last.mult || in->combo != s_last.combo) {
        snprintf(s, sizeof s, "WAVE %u", (unsigned)in->wave);
        field(12, 120, 2, L_TEXT, 8, s);
        snprintf(s, sizeof s, "x%u  COMBO %u", (unsigned)in->mult, (unsigned)in->combo);
        field(120, 124, 1, in->mult > 1 ? L_GOLD : L_DIM, 18, s);
    }
    if (all || in->lives != s_last.lives) lives_row(12, 150, in->lives);
    if (all || in->flare != s_last.flare) {
        flare_bar(12, 196, 220, in->flare);
        field(12, 214, 1, in->flare >= EW_FLARE_FULL ? L_WHITE : L_DIM, 20,
              in->flare >= EW_FLARE_FULL ? "FLARE READY: PRESS OK" : "FLARE");
    }
}

static void tv_view_full(const ew_lcd_info_t *in) {
    st7796_fill_screen(L_BG);
    text(12, 10, 3, L_EMBER, "EMBERWARD");
    text(12, 58, 2, L_DIM, "SCORE");
    text(12, 178, 1, L_DIM, "LIVES / FLARE");
    text(260, 58, 2, L_DIM, "HIGH SCORES");
    scores(in, 260, 84, 2, 22);
    tv_view_values(in, true);
}

// ------------------------------------------------------------ LCD view

// The picture fills the LCD but for a black frame; the app flushes it.
static void lcd_view_full(void) {
    st7796_fill_screen(L_BLACK);
}

// ------------------------------------------------------------ both

void ew_lcd_full(const ew_lcd_info_t *in) {
    st7796_flush_wait();
    if (in->lcd_view) {
        lcd_view_full();
        s_last = *in;
        s_valid = true;
        return;
    } else {
        tv_view_full(in);
        status_lines(in, 250, 6, 36);
        field(12, 240, 1, L_TEXT, 76, help(in, 0));
        field(12, 254, 1, L_DIM, 76, help(in, 1));
    }
    draw_keys(in);
    s_last = *in;
    s_valid = true;
}

void ew_lcd_update(const ew_lcd_info_t *in) {
    if (!s_valid || in->screen != s_last.screen || in->lcd_view != s_last.lcd_view) {
        ew_lcd_full(in);
        return;
    }
    if (in->lcd_view) {
        s_last = *in;
        return;
    }
    tv_view_values(in, false);
    if (in->save_gen != s_last.save_gen) scores(in, 260, 84, 2, 22);
    if (in->tv_seen != s_last.tv_seen || in->tv_running != s_last.tv_running || in->cec_ok != s_last.cec_ok ||
        in->sd_ok != s_last.sd_ok || in->sound != s_last.sound || in->tv_sound_on != s_last.tv_sound_on)
        status_lines(in, 250, 6, 36);
    if (in->sound != s_last.sound || in->tv_input != s_last.tv_input || in->tv_sound != s_last.tv_sound)
        draw_keys(in);
    s_last = *in;
}

void ew_lcd_message(const char *msg) {
    st7796_flush_wait();
    st7796_fill_rect(0, 140, 480, 40, L_BG);
    st7796_draw_text((480 - (int)strlen(msg) * 12) / 2, 152, 2, L_TEXT, L_BG, msg);
}
