// ew_render.h — Emberward's TV picture: 400x300 RGB565, high byte first (the
// TV's and the LCD's wire order), one buffer in SRAM that the TV reads in
// place. A frame is composed in 10-row bands in a small SRAM strip and each
// band is copied into the picture whole, top to bottom, so every pixel is
// written once with its final value. Started just after the TV leaves the
// picture rows (tvout_show() of the same buffer is the wait), the copy stays
// ahead of the scan and the TV never shows a half-drawn frame.
// Presentation state (particles, the ward's turning animation, flashes)
// lives here and is driven by the game's events.
#ifndef EW_RENDER_H
#define EW_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "ew_game.h"
#include "ew_scores.h"

#define EW_FB_W 400
#define EW_FB_H 300
#define EW_BAND_H 10

// RGB565 in wire order.
#define EW_RGB(r, g, b) ((uint16_t)(((((r) & 0xF8) | ((g) >> 5)) & 0xFF) | ((((((g) & 0x1C) << 3) | ((b) >> 3)) & 0xFF) << 8)))

typedef struct {
    bool active;          // the game-over panel asks for initials
    char name[4];
    uint8_t cursor;       // 0..2, 3 = the OK box
    int8_t rank;          // place in the table, -1 none
} ew_entry_ui_t;

void ew_render_init(void);

// Clears particles and animations for a new game.
void ew_render_reset(const ew_game_t *g);

// One game event: particles, score pop-ups, flashes, shake.
void ew_render_event(const ew_game_t *g, const ew_event_t *ev);

// One 1/60 s tick of the animations.
void ew_render_tick(const ew_game_t *g);

// Title screen with the high-score table.
void ew_render_title(uint16_t *fb, const ew_save_t *save, uint32_t frame, bool sd_ok);

// The game field; paused draws the pause panel over it.
void ew_render_game(uint16_t *fb, const ew_game_t *g, uint32_t frame, bool paused);

// The game field with the game-over panel (and the initials entry).
void ew_render_over(uint16_t *fb, const ew_game_t *g, uint32_t frame, const ew_entry_ui_t *ui,
                    const ew_save_t *save);

// Width in pixels of 5x7 text at a scale.
int ew_text_width(int scale, const char *s);

#endif
