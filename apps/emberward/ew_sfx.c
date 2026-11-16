// ew_sfx.c — sound-effect sequencer and note rendering.
#include "ew_sfx.h"

#include <string.h>

#define SQ EW_WAVE_SQUARE
#define PU EW_WAVE_PULSE
#define TR EW_WAVE_TRIANGLE
#define NO EW_WAVE_NOISE
#define END { 0, 0, 0 }

static const ew_step_t S_TURN[]   = { { 1400, 2, PU }, END };
static const ew_step_t S_MENU[]   = { { 880, 3, PU }, END };
static const ew_step_t S_CONF[]   = { { 660, 3, SQ }, { 1320, 6, SQ }, END };
static const ew_step_t S_BLOCK[]  = { { 660, 3, SQ }, { 990, 4, SQ }, END };
static const ew_step_t S_DART[]   = { { 880, 2, SQ }, { 1320, 2, SQ }, { 1760, 3, SQ }, END };
static const ew_step_t S_ABSORB[] = { { 523, 3, TR }, { 659, 3, TR }, { 784, 5, TR }, END };
static const ew_step_t S_SHAT[]   = { { 3000, 3, NO }, { 1500, 5, NO }, END };
static const ew_step_t S_HIT[]    = { { 900, 6, NO }, { 140, 10, SQ }, { 100, 12, SQ }, END };
static const ew_step_t S_HEART[]  = { { 523, 4, TR }, { 784, 4, TR }, { 1047, 10, TR }, END };
static const ew_step_t S_READY[]  = { { 1047, 3, PU }, { 1319, 3, PU }, { 1568, 3, PU }, { 2093, 8, PU }, END };
static const ew_step_t S_FLARE[]  = { { 4000, 4, NO }, { 2500, 5, NO }, { 1500, 6, NO }, { 800, 10, NO }, END };
static const ew_step_t S_WSTART[] = { { 392, 8, SQ }, { 0, 2, SQ }, { 523, 8, SQ }, { 0, 2, SQ }, { 659, 14, SQ }, END };
static const ew_step_t S_WCLEAR[] = { { 523, 6, TR }, { 659, 6, TR }, { 784, 6, TR }, { 1047, 16, TR }, END };
static const ew_step_t S_OVER[]   = { { 392, 14, TR }, { 330, 14, TR }, { 262, 14, TR }, { 196, 36, TR }, END };
static const ew_step_t S_BEST[]   = { { 784, 5, PU }, { 1047, 5, PU }, { 784, 5, PU }, { 1047, 5, PU }, { 1568, 16, PU }, END };

typedef struct {
    const ew_step_t *steps;
    uint8_t priority;
} ew_effect_t;

static const ew_effect_t EFFECTS[EW_SFX_COUNT] = {
    [EW_SFX_NONE]        = { 0, 0 },
    [EW_SFX_TURN]        = { S_TURN, 1 },
    [EW_SFX_MENU]        = { S_MENU, 1 },
    [EW_SFX_CONFIRM]     = { S_CONF, 3 },
    [EW_SFX_BLOCK]       = { S_BLOCK, 2 },
    [EW_SFX_DART]        = { S_DART, 2 },
    [EW_SFX_ABSORB]      = { S_ABSORB, 3 },
    [EW_SFX_SHATTER]     = { S_SHAT, 3 },
    [EW_SFX_HIT]         = { S_HIT, 5 },
    [EW_SFX_HEART]       = { S_HEART, 4 },
    [EW_SFX_FLARE_READY] = { S_READY, 4 },
    [EW_SFX_FLARE]       = { S_FLARE, 6 },
    [EW_SFX_WAVE_START]  = { S_WSTART, 6 },
    [EW_SFX_WAVE_CLEAR]  = { S_WCLEAR, 6 },
    [EW_SFX_GAME_OVER]   = { S_OVER, 7 },
    [EW_SFX_NEW_BEST]    = { S_BEST, 7 },
};

void ew_sfx_init(ew_sfx_t *s) {
    memset(s, 0, sizeof *s);
}

unsigned ew_sfx_priority(ew_sfx_id_t id) {
    return (unsigned)id < EW_SFX_COUNT ? EFFECTS[id].priority : 0;
}

const ew_step_t *ew_sfx_steps(ew_sfx_id_t id) {
    return (unsigned)id < EW_SFX_COUNT ? EFFECTS[id].steps : 0;
}

unsigned ew_sfx_length(ew_sfx_id_t id) {
    if ((unsigned)id >= EW_SFX_COUNT || !EFFECTS[id].steps) return 0;
    unsigned n = 0;
    for (const ew_step_t *st = EFFECTS[id].steps; st->ticks; st++) n += st->ticks;
    return n;
}

void ew_sfx_play(ew_sfx_t *s, ew_sfx_id_t id) {
    if ((unsigned)id >= EW_SFX_COUNT) return;
    if (id == EW_SFX_NONE) {
        s->id = EW_SFX_NONE;
        s->started = false;
        return;
    }
    if (s->id != EW_SFX_NONE && EFFECTS[id].priority < EFFECTS[s->id].priority) return;
    s->id = (uint8_t)id;
    s->step = 0;
    s->ticks_left = EFFECTS[id].steps[0].ticks;
    s->started = true;
}

bool ew_sfx_tick(ew_sfx_t *s, ew_note_t *note) {
    ew_note_t now = { 0, 0 };
    bool strike = false;
    if (s->id != EW_SFX_NONE) {
        if (s->started) {
            // The first tick of an effect sounds its first step.
            s->started = false;
            strike = true;
        } else if (--s->ticks_left == 0) {
            s->step++;
            const ew_step_t *st = &EFFECTS[s->id].steps[s->step];
            if (!st->ticks) {
                s->id = EW_SFX_NONE;
            } else {
                s->ticks_left = st->ticks;
                strike = true;
            }
        }
        if (s->id != EW_SFX_NONE) {
            const ew_step_t *st = &EFFECTS[s->id].steps[s->step];
            now.hz = st->hz;
            now.wave = st->hz ? st->wave : 0;
        }
    }
    const bool changed = (strike && now.hz) || now.hz != s->last.hz || now.wave != s->last.wave;
    s->last = now;
    *note = now;
    return changed;
}

unsigned ew_sfx_render(uint32_t *buf, unsigned frames, unsigned hz, unsigned fs, ew_wave_t wave,
                       int16_t amp, uint32_t *noise_state) {
    if (!frames) return 0;
    if (!hz || !fs) {
        memset(buf, 0, (size_t)frames * 4);
        return 0;
    }
    unsigned cycles = (unsigned)(((uint64_t)hz * frames + fs / 2) / fs);
    if (cycles < 1) cycles = 1;
    if (cycles > frames / 2) cycles = frames / 2;
    uint32_t rng = noise_state && *noise_state ? *noise_state : 0x1234567u;
    int32_t level = 0;
    unsigned last_slot = ~0u;
    for (unsigned i = 0; i < frames; i++) {
        // Phase in [0, 65536) across one cycle.
        const uint32_t phase = (uint32_t)(((uint64_t)i * cycles * 65536u / frames) & 0xFFFFu);
        int32_t v;
        switch (wave) {
        case EW_WAVE_PULSE:
            v = phase < 16384u ? amp : -amp;
            break;
        case EW_WAVE_TRIANGLE: {
            // -amp at phase 0, +amp at half a cycle, back to -amp.
            const int32_t t = phase < 32768u ? (int32_t)phase : (int32_t)(65536u - phase);
            v = -amp + (int32_t)(((int64_t)t * 2 * amp) / 32768);
            break;
        }
        case EW_WAVE_NOISE: {
            // A new random level every half cycle; repeats with the buffer.
            const unsigned slot = (unsigned)(((uint64_t)i * cycles * 2) / frames);
            if (slot != last_slot) {
                last_slot = slot;
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                level = (int32_t)(rng % (2u * (uint32_t)amp + 1u)) - amp;
            }
            v = level;
            break;
        }
        default:
            v = phase < 32768u ? amp : -amp;
            break;
        }
        const uint16_t s = (uint16_t)(int16_t)v;
        buf[i] = ((uint32_t)s << 16) | s;
    }
    if (noise_state) *noise_state = rng;
    return (unsigned)(((uint64_t)cycles * fs + frames / 2) / frames);
}
