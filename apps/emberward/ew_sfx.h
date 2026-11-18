// ew_sfx.h — Emberward's sound effects: a sequencer of short notes ticked at
// 60 Hz, and the note's waveform rendered into a looping playback buffer.
// Pure C, host-tested in tests/test_ew_sfx.c.
#ifndef EW_SFX_H
#define EW_SFX_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { EW_WAVE_SQUARE, EW_WAVE_PULSE, EW_WAVE_TRIANGLE, EW_WAVE_NOISE } ew_wave_t;

typedef enum {
    EW_SFX_NONE,
    EW_SFX_TURN,
    EW_SFX_MENU,
    EW_SFX_CONFIRM,
    EW_SFX_BLOCK,
    EW_SFX_DART,
    EW_SFX_ABSORB,
    EW_SFX_SHATTER,
    EW_SFX_HIT,
    EW_SFX_HEART,
    EW_SFX_FLARE_READY,
    EW_SFX_FLARE,
    EW_SFX_WAVE_START,
    EW_SFX_WAVE_CLEAR,
    EW_SFX_GAME_OVER,
    EW_SFX_NEW_BEST,
    EW_SFX_COUNT
} ew_sfx_id_t;

typedef struct {
    uint16_t hz;        // 0 = rest
    uint8_t ticks;      // 1/60 s; 0 ends the effect
    uint8_t wave;       // ew_wave_t
} ew_step_t;

typedef struct {
    uint16_t hz;
    uint8_t wave;
} ew_note_t;

typedef struct {
    uint8_t id;          // the effect playing, EW_SFX_NONE when silent
    uint8_t step;        // its current step
    uint8_t ticks_left;  // in that step
    bool started;        // the effect has not been ticked yet
    ew_note_t last;      // the note last reported
} ew_sfx_t;

void ew_sfx_init(ew_sfx_t *s);

// Starts an effect if nothing plays or it ranks at least as high as the one
// playing. EW_SFX_NONE stops the sound.
void ew_sfx_play(ew_sfx_t *s, ew_sfx_id_t id);

// One 1/60 s tick: *note is the note to sound now. True when the playback
// buffer must change: the note differs from the previous tick's, or a new
// step or effect starts (a repeated note is re-struck).
bool ew_sfx_tick(ew_sfx_t *s, ew_note_t *note);

// The effect's rank (higher wins) and its total length in ticks.
unsigned ew_sfx_priority(ew_sfx_id_t id);
unsigned ew_sfx_length(ew_sfx_id_t id);

// The effect's steps, ended by a step of 0 ticks; NULL for EW_SFX_NONE or an
// unknown id. The TV mixer (ew_mix.h) plays the same notes.
const ew_step_t *ew_sfx_steps(ew_sfx_id_t id);

// The note as `frames` stereo frames ([L16|R16], the same sample both sides)
// at fs Hz, a whole number of cycles so the buffer loops without a seam: the
// cycle count is hz * frames / fs rounded, at least 1, at most frames / 2.
// Samples stay within +-amp. Returns the frequency produced, in Hz; hz 0
// renders silence and returns 0.
unsigned ew_sfx_render(uint32_t *buf, unsigned frames, unsigned hz, unsigned fs, ew_wave_t wave,
                       int16_t amp, uint32_t *noise_state);

#endif
