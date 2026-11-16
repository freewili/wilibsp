// ew_lcd.h — Emberward's companion screen on the 480x320 LCD: score, wave,
// lives, flare, high scores, TV and remote status and the five button labels;
// or, in LCD view, the whole TV picture mirrored on the LCD.
#ifndef EW_LCD_H
#define EW_LCD_H

#include <stdbool.h>
#include <stdint.h>

#include "ew_scores.h"

typedef enum { EW_SCREEN_TITLE, EW_SCREEN_PLAY, EW_SCREEN_PAUSE, EW_SCREEN_OVER, EW_SCREEN_ENTRY } ew_screen_t;

// Where the mirrored field sits on the LCD in LCD view.
#define EW_LCD_FIELD_X 40
#define EW_LCD_FIELD_Y 10

// The button bar (AGENTS.md "The five front-panel buttons").
#define EW_LCD_KEY_W 93
#define EW_LCD_KEY_PITCH 96
#define EW_LCD_KEY_H 40
#define EW_LCD_KEY_Y (320 - EW_LCD_KEY_H)

typedef struct {
    uint8_t screen;          // ew_screen_t
    bool lcd_view;
    uint32_t score;
    uint8_t wave;
    uint8_t lives;
    uint8_t mult;
    uint16_t combo;
    uint16_t flare;          // 0..EW_FLARE_FULL
    bool tv_seen;            // the TV's hot-plug line says connected
    bool tv_running;
    bool cec_ok;             // an address is claimed
    bool sound;
    bool tv_sound;           // the setting: sound on the TV while one is connected
    bool tv_sound_on;        // the TV carries the sound now (the speaker is silent)
    uint8_t tv_input;
    bool sd_ok;
    const ew_save_t *save;
    uint32_t save_gen;       // bumps when the table changes
} ew_lcd_info_t;

// Clears and draws the whole screen for the info's screen and view.
void ew_lcd_full(const ew_lcd_info_t *info);

// Redraws only the fields that changed since the last draw. A screen or view
// change redraws the whole screen.
void ew_lcd_update(const ew_lcd_info_t *info);

// A one-line message across the middle of the LCD (boot, saving).
void ew_lcd_message(const char *text);

// The front-panel slot (0 grey .. 4 red) under an LCD touch, or -1.
int ew_lcd_touch_slot(int x, int y);

#endif
