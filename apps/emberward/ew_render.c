// ew_render.c — Emberward's TV picture (see ew_render.h).
//
// Every drawing primitive clips to the current band (rows s_y0..s_y1-1) and
// writes into the band strip; render_bands() runs the screen's draw function
// once per band and copies each finished band into the picture.
#include "ew_render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "display/font5x7.h"

#define W EW_FB_W
#define H EW_FB_H
#define CX 200
#define CY 157
#define HUD_H 15
#define BAR_Y (H - 6)

// The ward ring and the core, in screen pixels.
#define WARD_IN   43
#define WARD_OUT  52
#define MAP_R     54                 // quarter angle map covers 0..MAP_R-1
#define CORE_R    17
#define HALO_R    28

#define MAX_PARTICLES 128
#define MAX_POPUPS    8
#define STARS         90

// Palette.
#define C_BLACK   EW_RGB(0, 0, 0)
#define C_WHITE   EW_RGB(255, 255, 255)
#define C_DIM     EW_RGB(110, 120, 160)
#define C_TEXT    EW_RGB(230, 236, 255)
#define C_GOLD    EW_RGB(255, 206, 64)
#define C_GOLD_D  EW_RGB(150, 104, 20)
#define C_EMBER   EW_RGB(255, 72, 40)
#define C_EMBER_D EW_RGB(130, 24, 16)
#define C_DART    EW_RGB(80, 230, 255)
#define C_DART_D  EW_RGB(20, 90, 130)
#define C_HEART   EW_RGB(90, 255, 120)
#define C_HEART_D EW_RGB(20, 110, 40)
#define C_WARD    EW_RGB(150, 220, 255)
#define C_WARD_E  EW_RGB(60, 120, 190)
#define C_RING    EW_RGB(34, 42, 72)
#define C_PANEL   EW_RGB(14, 16, 34)
#define C_PANEL_E EW_RGB(90, 110, 170)
#define C_RED     EW_RGB(255, 60, 60)

typedef struct {
    int32_t x, y, vx, vy;   // Q8
    uint16_t colour;
    uint8_t life, size;
} particle_t;

typedef struct {
    int16_t x, y;
    uint8_t life;
    uint16_t colour;
    char text[10];
} popup_t;

typedef struct {
    uint16_t angle;         // 0..65535 around the circle
    uint8_t radius;
    uint8_t speed;
    uint8_t phase;
} star_t;

// The band being composed.
static uint16_t s_band[W * EW_BAND_H];
static int s_y0, s_y1;

static uint8_t s_quarter[MAP_R * MAP_R];     // angle 0..64 in the lower-right quarter
static uint32_t s_rows[H];                   // background per row: two dithered pixels
static int16_t s_sin[256];                   // Q14
static star_t s_stars[STARS];
static particle_t s_parts[MAX_PARTICLES];
static popup_t s_pops[MAX_POPUPS];
static uint32_t s_rng = 0x9E3779B9u;

static uint16_t s_ward_disp;    // displayed ward centre, 0..65535
static uint8_t s_core_flash;    // ticks of white flash (absorb)
static uint8_t s_core_hurt;     // ticks of red flash (hit)
static uint8_t s_shake;
static uint8_t s_flare_wave;    // expanding flare ring
static uint8_t s_shatter_flash[EW_LANES];
static int s_ox, s_oy;          // shake offset for this frame

// Lane geometry: unit vectors (Q8) and how far each lane reaches on screen.
static const int16_t DIR_X[EW_LANES] = { 256, 181, 0, -181, -256, -181, 0, 181 };
static const int16_t DIR_Y[EW_LANES] = { 0, 181, 256, 181, 0, -181, -256, -181 };
static const int16_t LANE_EXTENT[EW_LANES] = { 192, 184, 130, 184, 192, 184, 134, 184 };

static uint32_t rnd(void) {
    return ew_rand(&s_rng);
}

static int isin(unsigned a) { return s_sin[a & 255]; }
static int icos(unsigned a) { return s_sin[(a + 64) & 255]; }

static uint16_t rgb(int r, int g, int b) {
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return EW_RGB(r, g, b);
}

// Angle (0..255, 0 = east, clockwise on screen) of the pixel at dx, dy from
// the centre, from the quarter map by symmetry.
static uint8_t angle_of(int dx, int dy) {
    const int ix = dx >= 0 ? dx : -dx - 1;
    const int iy = dy >= 0 ? dy : -dy - 1;
    const uint8_t a = s_quarter[iy * MAP_R + ix];
    if (dx >= 0) return dy >= 0 ? a : (uint8_t)(256 - a);
    return dy >= 0 ? (uint8_t)(128 - a) : (uint8_t)(128 + a);
}

void ew_render_init(void) {
    for (int i = 0; i < 256; i++) s_sin[i] = (int16_t)lrintf(16384.0f * sinf((float)i * 6.2831853f / 256.0f));
    for (int y = 0; y < MAP_R; y++)
        for (int x = 0; x < MAP_R; x++)
            s_quarter[y * MAP_R + x] =
                (uint8_t)lrintf(atan2f((float)y + 0.5f, (float)x + 0.5f) * 256.0f / 6.2831853f);
    for (int y = 0; y < H; y++) {
        // Deep blue at the top to violet-black at the bottom, in eighths of
        // a level, dithered between the two nearest RGB565 colours so the
        // gradient shows no bands.
        const int t = y * 256 / H;
        const int r8 = 6 * 8 + t * 8 / 24, g8 = 8 * 8 + t * 8 / 40, b8 = 26 * 8 - t * 8 / 16;
        const int lo_r = (r8 / 8) & ~7, lo_g = (g8 / 8) & ~3, lo_b = (b8 / 8) & ~7;
        const int hi_r = r8 / 8 - lo_r >= 4 ? lo_r + 8 : lo_r;
        const int hi_g = g8 / 8 - lo_g >= 2 ? lo_g + 4 : lo_g;
        const int hi_b = b8 / 8 - lo_b >= 4 ? lo_b + 8 : lo_b;
        const uint16_t lo = rgb(lo_r, lo_g, lo_b), hi = rgb(hi_r, hi_g, hi_b);
        s_rows[y] = (y & 1) ? (uint32_t)hi | (uint32_t)lo << 16 : (uint32_t)lo | (uint32_t)hi << 16;
    }
    for (int i = 0; i < STARS; i++) {
        s_stars[i].angle = (uint16_t)rnd();
        s_stars[i].radius = (uint8_t)(30 + rnd() % 220);
        s_stars[i].speed = (uint8_t)(1 + rnd() % 3);
        s_stars[i].phase = (uint8_t)rnd();
    }
    s_ward_disp = (uint16_t)((6 * 32 + 16) << 8);
}

void ew_render_reset(const ew_game_t *g) {
    memset(s_parts, 0, sizeof s_parts);
    memset(s_pops, 0, sizeof s_pops);
    memset(s_shatter_flash, 0, sizeof s_shatter_flash);
    s_core_flash = s_core_hurt = s_shake = s_flare_wave = 0;
    s_ward_disp = (uint16_t)((g->ward * 32 + 16) << 8);
}

// ---------------------------------------------------------------- pixels

static inline bool rows_hit(int y, int h) {
    return y < s_y1 && y + h > s_y0;
}

static inline void px(int x, int y, uint16_t c) {
    if ((unsigned)x < W && y >= s_y0 && y < s_y1) s_band[(y - s_y0) * W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < s_y0) { h -= s_y0 - y; y = s_y0; }
    if (x + w > W) w = W - x;
    if (y + h > s_y1) h = s_y1 - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint16_t *row = s_band + (y - s_y0 + j) * W + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static void frame_rect(int x, int y, int w, int h, uint16_t c) {
    fill_rect(x, y, w, 1, c);
    fill_rect(x, y + h - 1, w, 1, c);
    fill_rect(x, y, 1, h, c);
    fill_rect(x + w - 1, y, 1, h, c);
}

static void disc(int cx, int cy, int r, uint16_t c) {
    if (!rows_hit(cy - r, 2 * r + 1)) return;
    for (int dy = -r; dy <= r; dy++) {
        if (cy + dy < s_y0 || cy + dy >= s_y1) continue;
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r + r) px(cx + dx, cy + dy, c);
    }
}

int ew_text_width(int scale, const char *s) {
    const int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

static void glyphs(int x, int y, int scale, uint16_t colour, const char *s) {
    if (!rows_hit(y, 7 * scale)) return;
    for (; *s; s++, x += 6 * scale) {
        unsigned ch = (unsigned char)*s;
        if (ch == ' ') continue;
        if (ch < FONT5X7_FIRST || ch > FONT5X7_LAST) ch = '?';
        const uint8_t *g = font5x7[ch - FONT5X7_FIRST];
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (!(g[col] >> row & 1)) continue;
                if (scale == 1) px(x + col, y + row, colour);
                else fill_rect(x + col * scale, y + row * scale, scale, scale, colour);
            }
        }
    }
}

// Transparent text, with a one-pixel dark shadow when shadow.
static void text(int x, int y, int scale, uint16_t colour, bool shadow, const char *s) {
    if (shadow) glyphs(x + 1, y + 1, scale, C_BLACK, s);
    glyphs(x, y, scale, colour, s);
}

static void text_centre(int y, int scale, uint16_t colour, const char *s) {
    text((W - ew_text_width(scale, s)) / 2, y, scale, colour, true, s);
}

// ---------------------------------------------------------------- bands

typedef void (*draw_fn_t)(void);

static void render_bands(uint16_t *fb, draw_fn_t draw) {
    for (int y = 0; y < H; y += EW_BAND_H) {
        s_y0 = y;
        s_y1 = y + EW_BAND_H < H ? y + EW_BAND_H : H;
        draw();
        memcpy(fb + y * W, s_band, (size_t)(s_y1 - s_y0) * W * 2);
    }
}

// ---------------------------------------------------------------- scene

static void background(void) {
    for (int y = s_y0; y < s_y1; y++) {
        const uint32_t c = s_rows[y];
        uint32_t *row = (uint32_t *)(s_band + (y - s_y0) * W);
        for (int i = 0; i < W / 2; i++) row[i] = c;
    }
}

static void stars(uint32_t frame) {
    for (int i = 0; i < STARS; i++) {
        const star_t *s = &s_stars[i];
        const unsigned a = (unsigned)((s->angle + frame * s->speed * 9u) >> 8);
        const int y = CY + s_oy + (isin(a) * s->radius >> 14) * 3 / 4;
        if (y < s_y0 || y >= s_y1) continue;
        const int x = CX + s_ox + (icos(a) * s->radius >> 14);
        const int tw = isin(s->phase + frame * (2u + s->speed)) >> 9;   // -32..32
        const int b = 110 + s->speed * 30 + tw;
        px(x, y, rgb(b, b, b + 30));
    }
}

// Screen position of a lane at game radius r (Q8): beyond the ward each
// lane is stretched to reach the edge of the field, so every spark is in
// view for the same time whatever its direction.
static void lane_pos(unsigned lane, int32_t r_q8, int *x, int *y) {
    // Q16 stretch per lane: (extent - ward) / (spawn - ward). A multiply, no
    // divide: this runs for every spark and guide dot in every band.
    static int32_t stretch_q16[EW_LANES];
    if (!stretch_q16[0])
        for (int i = 0; i < EW_LANES; i++)
            stretch_q16[i] = (LANE_EXTENT[i] - EW_R_WARD) * 65536 / (EW_R_SPAWN - EW_R_WARD);
    int32_t rs = r_q8;
    if (r_q8 > EW_Q8(EW_R_WARD))
        rs = EW_Q8(EW_R_WARD) + (int32_t)(((int64_t)(r_q8 - EW_Q8(EW_R_WARD)) * stretch_q16[lane]) >> 16);
    *x = CX + s_ox + (int)((DIR_X[lane] * rs) >> 16);
    *y = CY + s_oy + (int)((DIR_Y[lane] * rs) >> 16);
}

static void lane_guides(uint32_t frame) {
    for (unsigned lane = 0; lane < EW_LANES; lane++) {
        const uint16_t c = s_shatter_flash[lane] ? C_GOLD_D : C_RING;
        for (int r = EW_R_WARD + 14; r < EW_R_SPAWN; r += 8) {
            int x, y;
            lane_pos(lane, EW_Q8(r) - (int32_t)((frame % 8) << 8), &x, &y);
            px(x, y, c);
        }
    }
}

static void core(const ew_game_t *g, uint32_t frame) {
    const int x0 = CX + s_ox, y0 = CY + s_oy;
    if (!rows_hit(y0 - HALO_R, 2 * HALO_R + 1)) return;
    // Colour by health: white-hot at full, dull red near the end.
    int r = 255, gg = 170, b = 70;
    if (g->lives <= 1) { gg = 70; b = 40; }
    else if (g->lives == 2) { gg = 120; b = 50; }
    else if (g->lives >= 4) { gg = 210; b = 120; }
    if (g->phase == EW_PHASE_OVER) { r = 60; gg = 40; b = 50; }
    if (s_core_flash) { gg += s_core_flash * 8; b += s_core_flash * 10; }
    if (s_core_hurt && (s_core_hurt & 4)) { r = 255; gg = 30; b = 30; }
    const int pulse = isin(frame * 3) >> 11;   // -8..8
    const int core_r = CORE_R + (pulse > 0 ? pulse / 4 : 0);
    const int cr2 = core_r * core_r, hr2 = HALO_R * HALO_R;
    for (int dy = -HALO_R; dy <= HALO_R; dy++) {
        const int y = y0 + dy;
        if (y < s_y0 || y >= s_y1) continue;
        for (int dx = -HALO_R; dx <= HALO_R; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > hr2) continue;
            uint16_t c;
            if (d2 <= cr2) {
                // Hot centre to the rim.
                const int t = d2 * 256 / cr2;
                c = rgb(r - t / 10, gg + 60 - t / 3 - t / 5, b + 40 - t / 4 - t / 7);
            } else {
                // Halo, dithered into the background.
                const int t = (d2 - cr2) * 256 / (hr2 - cr2);
                if (t > 230 || (((dx + dy + (int)frame) & 1) && t > 150)) continue;
                const int k = 256 - t;
                c = rgb(r * k / 700, gg * k / 900, b * k / 900 + 10);
            }
            px(x0 + dx, y, c);
        }
    }
}

static void ward(const ew_game_t *g, uint32_t frame) {
    const int x0 = CX + s_ox, y0 = CY + s_oy;
    if (!rows_hit(y0 - WARD_OUT, 2 * WARD_OUT + 1)) return;
    const uint8_t centre = (uint8_t)(s_ward_disp >> 8);
    const bool ready = g->flare >= EW_FLARE_FULL;
    const int glow = ready ? (isin(frame * 6) >> 10) + 16 : 0;   // 0..32
    const uint16_t edge = ready ? C_GOLD_D : C_WARD_E;
    const uint16_t body = ready ? rgb(255, 220 + glow / 2, 120 + glow * 3) : C_WARD;
    const uint16_t inner = ready ? C_GOLD : rgb(210, 240, 255);
    const int in2 = WARD_IN * WARD_IN, out2 = WARD_OUT * WARD_OUT;
    for (int dy = -WARD_OUT; dy <= WARD_OUT; dy++) {
        const int y = y0 + dy;
        if (y < s_y0 || y >= s_y1) continue;
        for (int dx = -WARD_OUT; dx <= WARD_OUT; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 < in2 || d2 > out2) continue;
            const uint8_t a = angle_of(dx, dy);
            const int diff = (int8_t)(uint8_t)(a - centre);
            const int ad = diff < 0 ? -diff : diff;
            uint16_t c;
            if (ad <= 30) {
                const bool rim = d2 < (WARD_IN + 2) * (WARD_IN + 2) || d2 > (WARD_OUT - 2) * (WARD_OUT - 2);
                c = rim ? edge : (d2 < 48 * 48 ? inner : body);
            } else if (ad <= 32) {
                c = edge;
            } else {
                // The rest of the ring: a faint track with a notch between lanes.
                if (((a + 16) & 31) < 2 || d2 > 48 * 48 || d2 < 47 * 47) continue;
                c = C_RING;
            }
            px(x0 + dx, y, c);
        }
    }
}

static void mote(const ew_mote_t *m, uint32_t frame) {
    uint16_t hot, dim;
    switch (m->kind) {
    case EW_GLIM:  hot = C_GOLD;  dim = C_GOLD_D;  break;
    case EW_DART:  hot = C_DART;  dim = C_DART_D;  break;
    case EW_HEART: hot = C_HEART; dim = C_HEART_D; break;
    default:       hot = C_EMBER; dim = C_EMBER_D; break;
    }
    int x, y;
    lane_pos(m->lane, m->r_q8, &x, &y);
    if (!rows_hit(y - 40, 81)) return;   // trail and glints included
    // A short trail behind the spark.
    const int trail = m->kind == EW_DART ? 5 : 3;
    for (int t = trail; t >= 1; t--) {
        int tx, ty;
        lane_pos(m->lane, m->r_q8 + m->speed_q8 * t * 3, &tx, &ty);
        disc(tx, ty, t == 1 ? 2 : 1, dim);
    }
    // Small for the first pixels of the lane: the spark fades in.
    const int age = EW_Q8(EW_R_SPAWN) - m->r_q8;
    const int size = age < EW_Q8(6) ? 2 : 5;
    disc(x, y, size, dim);
    disc(x, y, size - 1, hot);
    px(x, y, C_WHITE);
    if (m->kind == EW_HEART) {
        fill_rect(x - 1, y - 3, 3, 7, C_WHITE);
        fill_rect(x - 3, y - 1, 7, 3, C_WHITE);
    } else if (m->kind == EW_GLIM && (frame / 4) % 2) {
        px(x - 7, y, C_GOLD);
        px(x + 7, y, C_GOLD);
        px(x, y - 7, C_GOLD);
        px(x, y + 7, C_GOLD);
    }
}

static void particles(void) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        const particle_t *p = &s_parts[i];
        if (!p->life) continue;
        const int x = (p->x >> 8) + s_ox, y = (p->y >> 8) + s_oy;
        if (p->size > 1 && p->life > 8) fill_rect(x, y, 2, 2, p->colour);
        else px(x, y, p->colour);
    }
}

static void popups(void) {
    for (int i = 0; i < MAX_POPUPS; i++) {
        const popup_t *p = &s_pops[i];
        if (!p->life) continue;
        text(p->x - ew_text_width(1, p->text) / 2, p->y, 1, p->colour, true, p->text);
    }
}

static void hud(const ew_game_t *g, uint32_t frame) {
    char s[32];
    if (rows_hit(0, HUD_H + 1)) {
        fill_rect(0, 0, W, HUD_H, C_PANEL);
        fill_rect(0, HUD_H, W, 1, C_RING);
        snprintf(s, sizeof s, "%07lu", (unsigned long)g->score);
        text(5, 4, 1, C_TEXT, false, s);
        snprintf(s, sizeof s, "WAVE %u", (unsigned)g->wave);
        text((W - ew_text_width(1, s)) / 2, 4, 1, C_TEXT, false, s);
        if (ew_multiplier(g) > 1) {
            snprintf(s, sizeof s, "x%u", ew_multiplier(g));
            text(54, 4, 1, C_GOLD, false, s);
        }
        // Lives: small diamonds at the right.
        for (int i = 0; i < EW_LIVES_MAX; i++) {
            const int x = W - 10 - i * 12, y = 7;
            const uint16_t c = i < g->lives ? C_EMBER : C_RING;
            for (int k = 0; k <= 4; k++) {
                fill_rect(x - k, y - 4 + k, 2 * k + 1, 1, c);
                fill_rect(x - k, y + 4 - k, 2 * k + 1, 1, c);
            }
        }
    }
    // Flare meter along the bottom.
    const bool ready = g->flare >= EW_FLARE_FULL;
    if (rows_hit(BAR_Y, 6)) {
        fill_rect(0, BAR_Y, W, 6, C_PANEL);
        const int fw = g->flare * (W - 4) / EW_FLARE_FULL;
        fill_rect(2, BAR_Y + 1, fw, 4, ready && (frame / 6) % 2 ? C_WHITE : C_GOLD);
    }
    if (ready && g->phase != EW_PHASE_OVER && (frame / 20) % 3)
        text_centre(BAR_Y - 12, 1, C_GOLD, "FLARE READY - PRESS OK");
}

static void flare_ring(void) {
    if (!s_flare_wave) return;
    const int r = (40 - s_flare_wave) * 7 + WARD_OUT;
    const uint16_t c = s_flare_wave > 20 ? C_WHITE : C_GOLD;
    if (!rows_hit(CY - r, 2 * r + 1)) return;
    for (unsigned a = 0; a < 512; a++) {
        const int x = CX + (s_sin[((a >> 1) + 64) & 255] * r >> 14);
        const int y = CY + (s_sin[(a >> 1) & 255] * r >> 14);
        px(x, y, c);
        px(x + 1, y, c);
    }
}

static void panel(int x, int y, int w, int h) {
    if (!rows_hit(y, h)) return;
    fill_rect(x, y, w, h, C_PANEL);
    frame_rect(x, y, w, h, C_PANEL_E);
}

// ---------------------------------------------------------------- screens

// What the band draw functions read: set by the public calls.
static const ew_game_t *s_g;
static const ew_save_t *s_save;
static const ew_entry_ui_t *s_ui;
static uint32_t s_frame;
static bool s_paused, s_sd_ok;

static void scene(void) {
    const ew_game_t *g = s_g;
    background();
    stars(s_frame);
    lane_guides(s_frame);
    core(g, s_frame);
    ward(g, s_frame);
    for (int i = 0; i < EW_MAX_MOTES; i++)
        if (g->motes[i].active) mote(&g->motes[i], s_frame);
    particles();
    flare_ring();
    popups();
    hud(g, s_frame);
}

static void draw_game(void) {
    const ew_game_t *g = s_g;
    char s[32];
    scene();
    if (g->phase == EW_PHASE_INTRO) {
        snprintf(s, sizeof s, "WAVE %u", (unsigned)g->wave);
        text_centre(52, 3, C_GOLD, s);
        if (g->wave == 1) {
            text_centre(246, 1, C_EMBER, "BLOCK THE RED EMBERS");
            text_centre(258, 1, C_GOLD, "LET THE GOLD GLIMS IN");
        } else if (g->wave == 2) {
            text_centre(252, 1, C_TEXT, "PAIRS: ONE WARD POSITION ANSWERS BOTH");
        } else if (g->wave == 3) {
            text_centre(252, 1, C_DART, "DARTS: FAST EMBERS");
        } else if (g->wave == 4) {
            text_centre(252, 1, C_HEART, "GREEN HEARTS GIVE A LIFE");
        }
    } else if (g->phase == EW_PHASE_CLEAR) {
        text_centre(52, 2, C_TEXT, "WAVE CLEAR");
        snprintf(s, sizeof s, "+%u", (unsigned)(100u * g->wave));
        text_centre(74, 2, C_GOLD, s);
    }
    if (s_paused) {
        panel(120, 116, 160, 44);
        text_centre(124, 2, C_TEXT, "PAUSED");
        text_centre(146, 1, C_DIM, "OK RESUME  PAUSE QUIT");
    }
}

static void score_table(int y, int highlight) {
    char s[40];
    for (int i = 0; i < EW_SCORES; i++) {
        const ew_score_t *e = &s_save->entry[i];
        snprintf(s, sizeof s, "%d. %s %7lu  W%u", i + 1, e->name, (unsigned long)e->score, (unsigned)e->wave);
        uint16_t c = i == 0 ? C_GOLD : C_TEXT;
        if (i == highlight && (s_frame / 8) % 2) c = C_WHITE;
        text_centre(y + i * 12, 1, c, s);
    }
}

static void draw_title(void) {
    static ew_game_t demo;
    const uint32_t frame = s_frame;
    if (!demo.lives) ew_init(&demo, 1);
    demo.lives = 4;
    background();
    stars(frame);
    core(&demo, frame);
    ward(&demo, frame);
    // Decorative sparks circling the core.
    for (int i = 0; i < 6; i++) {
        const unsigned a = frame * 2u + (unsigned)i * 43u;
        const int r = 80 + (isin(frame + (unsigned)i * 40u) >> 11);
        const int x = CX + (icos(a) * r >> 14), y = CY + (isin(a) * r >> 14) * 3 / 4;
        const bool gold = i % 2;
        disc(x, y, 4, gold ? C_GOLD_D : C_EMBER_D);
        disc(x, y, 3, gold ? C_GOLD : C_EMBER);
    }
    // The name, with a slow colour sweep.
    const char *title = "EMBERWARD";
    int x = (W - ew_text_width(5, title)) / 2;
    if (rows_hit(16, 38)) {
        for (int i = 0; title[i]; i++, x += 30) {
            const int k = isin(frame * 4u + (unsigned)i * 18u) >> 8;   // -64..64
            const char ch[2] = { title[i], 0 };
            text(x + 3, 19, 5, EW_RGB(90, 20, 10), false, ch);
            text(x, 16, 5, rgb(255, 150 + k, 60 + k / 2), false, ch);
        }
    }
    text_centre(58, 1, C_DIM, "GUARD THE CORE");
    if ((frame / 24) % 2) text_centre(78, 2, C_WHITE, "PRESS OK TO START");
    panel(90, 220, 220, 74);
    text_centre(225, 1, C_GOLD, "HIGH SCORES");
    score_table(237, -1);
    if (!s_sd_ok) text(3, 3, 1, C_DIM, false, "NO SD: SCORES NOT SAVED");
}

static void draw_over(void) {
    const ew_game_t *g = s_g;
    const ew_entry_ui_t *ui = s_ui;
    char s[40];
    scene();
    panel(70, 40, 260, 220);
    text_centre(50, 3, C_EMBER, "CORE DARK");
    snprintf(s, sizeof s, "SCORE %lu", (unsigned long)g->score);
    text_centre(80, 2, C_TEXT, s);
    snprintf(s, sizeof s, "WAVE %u   BEST COMBO %u", (unsigned)g->wave, (unsigned)g->best_combo);
    text_centre(100, 1, C_DIM, s);
    if (ui->active) {
        text_centre(118, 1, C_GOLD, "NEW HIGH SCORE - YOUR INITIALS");
        for (int i = 0; i < 3; i++) {
            const int bx = 150 + i * 28;
            const bool sel = ui->cursor == i;
            if (rows_hit(132, 30)) {
                fill_rect(bx, 132, 24, 30, sel ? EW_RGB(40, 50, 100) : C_PANEL);
                frame_rect(bx, 132, 24, 30, sel ? C_GOLD : C_PANEL_E);
            }
            const char ch[2] = { ui->name[i], 0 };
            text(bx + 4, 137, 3, sel && (s_frame / 10) % 2 ? C_WHITE : C_TEXT, false, ch);
        }
        const bool ok_sel = ui->cursor == 3;
        if (rows_hit(138, 18)) {
            fill_rect(238, 138, 30, 18, ok_sel ? C_GOLD : C_PANEL);
            frame_rect(238, 138, 30, 18, C_GOLD);
        }
        text(247, 144, 1, ok_sel ? C_BLACK : C_GOLD, false, "OK");
        text_centre(170, 1, C_DIM, "UP/DOWN LETTER  LEFT/RIGHT MOVE");
        score_table(186, -1);
    } else {
        score_table(122, ui->rank);
        if ((s_frame / 24) % 2) text_centre(200, 1, C_WHITE, "PRESS OK TO PLAY AGAIN");
        text_centre(216, 1, C_DIM, "PAUSE FOR TITLE");
    }
}

static void shake_offset(void) {
    s_ox = s_oy = 0;
    if (s_shake) {
        s_ox = (int)(rnd() % 7) - 3;
        s_oy = (int)(rnd() % 7) - 3;
    }
}

void ew_render_title(uint16_t *fb, const ew_save_t *save, uint32_t frame, bool sd_ok) {
    s_ox = s_oy = 0;
    s_save = save;
    s_frame = frame;
    s_sd_ok = sd_ok;
    s_ward_disp = (uint16_t)(frame * 90u);
    render_bands(fb, draw_title);
}

void ew_render_game(uint16_t *fb, const ew_game_t *g, uint32_t frame, bool paused) {
    shake_offset();
    s_g = g;
    s_frame = frame;
    s_paused = paused;
    render_bands(fb, draw_game);
}

void ew_render_over(uint16_t *fb, const ew_game_t *g, uint32_t frame, const ew_entry_ui_t *ui,
                    const ew_save_t *save) {
    shake_offset();
    s_g = g;
    s_ui = ui;
    s_save = save;
    s_frame = frame;
    render_bands(fb, draw_over);
}

// ---------------------------------------------------------------- effects

static void burst(int x, int y, int n, uint16_t c1, uint16_t c2, int speed, int life) {
    for (int i = 0, made = 0; i < MAX_PARTICLES && made < n; i++) {
        particle_t *p = &s_parts[i];
        if (p->life) continue;
        const unsigned a = rnd() & 255;
        const int v = speed / 2 + (int)(rnd() % (unsigned)(speed + 1));
        p->x = x << 8;
        p->y = y << 8;
        p->vx = icos(a) * v >> 12;
        p->vy = isin(a) * v >> 12;
        p->colour = (rnd() & 1) ? c1 : c2;
        p->life = (uint8_t)(life / 2 + rnd() % (unsigned)(life / 2 + 1));
        p->size = (uint8_t)(1 + (rnd() & 1));
        made++;
    }
}

static void popup(int x, int y, uint16_t colour, const char *s) {
    int slot = 0;
    for (int i = 0; i < MAX_POPUPS; i++) {
        if (!s_pops[i].life) { slot = i; break; }
        if (s_pops[i].life < s_pops[slot].life) slot = i;
    }
    popup_t *p = &s_pops[slot];
    p->x = (int16_t)x;
    p->y = (int16_t)y;
    p->life = 40;
    p->colour = colour;
    snprintf(p->text, sizeof p->text, "%s", s);
}

void ew_render_event(const ew_game_t *g, const ew_event_t *ev) {
    (void)g;
    int x, y;
    char s[12];
    s_ox = s_oy = 0;
    switch (ev->type) {
    case EW_EV_BLOCK:
        lane_pos(ev->lane, ev->pad ? EW_Q8(100) : EW_Q8(EW_R_WARD + 4), &x, &y);
        burst(x, y, ev->kind == EW_DART ? 16 : 12, ev->kind == EW_DART ? C_DART : C_EMBER, C_WARD, 3, 24);
        snprintf(s, sizeof s, "+%lu", (unsigned long)ev->points);
        popup(x, y - 12, C_TEXT, s);
        break;
    case EW_EV_ABSORB:
    case EW_EV_HEART:
        s_core_flash = 12;
        burst(CX, CY, 14, ev->type == EW_EV_HEART ? C_HEART : C_GOLD, C_WHITE, 2, 30);
        if (ev->type == EW_EV_HEART) snprintf(s, sizeof s, "+LIFE");
        else snprintf(s, sizeof s, "+%lu", (unsigned long)ev->points);
        popup(CX, CY - 36, ev->type == EW_EV_HEART ? C_HEART : C_GOLD, s);
        break;
    case EW_EV_SHATTER:
        lane_pos(ev->lane, EW_Q8(EW_R_WARD + 4), &x, &y);
        burst(x, y, 10, C_GOLD_D, C_DIM, 2, 20);
        popup(x, y - 12, C_DIM, "LOST");
        s_shatter_flash[ev->lane % EW_LANES] = 20;
        break;
    case EW_EV_HIT:
        s_core_hurt = 32;
        s_shake = 14;
        burst(CX, CY, 30, C_EMBER, C_RED, 4, 36);
        break;
    case EW_EV_GRACE_PASS:
        burst(CX, CY, 6, C_EMBER_D, C_DIM, 2, 16);
        break;
    case EW_EV_FLARE:
        s_flare_wave = 40;
        s_core_flash = 20;
        snprintf(s, sizeof s, "+%lu", (unsigned long)ev->points);
        popup(CX, CY + 36, C_GOLD, s);
        break;
    case EW_EV_GAME_OVER:
        s_shake = 30;
        burst(CX, CY, 60, C_EMBER, C_GOLD, 5, 60);
        break;
    case EW_EV_WAVE_CLEAR:
        burst(CX, CY, 40, C_GOLD, C_WARD, 4, 50);
        break;
    default:
        break;
    }
}

void ew_render_tick(const ew_game_t *g) {
    // The ward eases towards its lane pair, the short way round.
    const uint16_t target = (uint16_t)((g->ward * 32 + 16) << 8);
    const int16_t diff = (int16_t)(uint16_t)(target - s_ward_disp);
    if (diff > -96 && diff < 96) s_ward_disp = target;
    else s_ward_disp = (uint16_t)(s_ward_disp + diff / 3);
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particle_t *p = &s_parts[i];
        if (!p->life) continue;
        p->life--;
        p->x += p->vx;
        p->y += p->vy;
        p->vx = p->vx * 15 / 16;
        p->vy = p->vy * 15 / 16;
    }
    for (int i = 0; i < MAX_POPUPS; i++) {
        if (!s_pops[i].life) continue;
        s_pops[i].life--;
        if (s_pops[i].life % 2) s_pops[i].y--;
    }
    for (int i = 0; i < EW_LANES; i++)
        if (s_shatter_flash[i]) s_shatter_flash[i]--;
    if (s_core_flash) s_core_flash--;
    if (s_core_hurt) s_core_hurt--;
    if (s_shake) s_shake--;
    if (s_flare_wave) s_flare_wave--;
}
