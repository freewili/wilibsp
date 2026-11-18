// ew_mix.h — Emberward's sound effects for the TV: several effects at once,
// synthesised sample by sample at 48 kHz stereo and mixed, for
// tvout_audio_write(). The effects and their notes are ew_sfx's.
//
// Two contexts share one mixer: the game posts effects (ew_mix_post), and a
// periodic timer callback feeds the TV (ew_mix_feed). The post queue is the
// only state both touch: one writer each side, so no lock is needed when both
// run on the same core. Pure C, host-tested in tests/test_ew_mix.c.
#ifndef EW_MIX_H
#define EW_MIX_H

#include <stdbool.h>
#include <stdint.h>

#include "ew_sfx.h"

#define EW_MIX_FS       48000u
#define EW_MIX_TICK     (EW_MIX_FS / 60u)   // frames per 1/60 s sequencer tick: 800
#define EW_MIX_VOICES   4
#define EW_MIX_QUEUE    16                  // posts held between feeds; one slot stays empty
#define EW_MIX_BLOCK    32                  // frames synthesised at a time by ew_mix_feed
#define EW_MIX_FEED_MAX 8                   // blocks one ew_mix_feed call renders at most

typedef struct {
    uint8_t id;          // EW_SFX_NONE when the voice is free
    uint8_t step;
    uint8_t wave;
    int16_t level;       // the noise wave's current level
    uint32_t left;       // frames left in the step
    uint32_t phase;      // one cycle is 2^32
    uint32_t inc;        // phase step per frame, 0 for a rest
    uint32_t serial;     // start order, for choosing a voice to take over
} ew_voice_t;

typedef unsigned (*ew_mix_write_fn)(const int16_t *stereo, unsigned frames);

typedef struct {
    ew_voice_t voice[EW_MIX_VOICES];
    volatile uint8_t queue[EW_MIX_QUEUE];
    volatile uint8_t head;   // written by ew_mix_post only
    volatile uint8_t tail;   // written by ew_mix_render only
    uint32_t serial;
    uint32_t rng;
    int16_t amp;             // each voice stays within +-amp; the sum is clipped
    int16_t pending[2 * EW_MIX_BLOCK];
    uint16_t pending_at, pending_count;
} ew_mix_t;

void ew_mix_init(ew_mix_t *m, int16_t amp);

// Queues an effect for the next render; EW_SFX_NONE silences every voice.
// False when the queue is full (the effect is dropped).
bool ew_mix_post(ew_mix_t *m, ew_sfx_id_t id);

// Starts the queued effects, then writes `frames` stereo frames (left, right;
// the same sample both sides). An effect already playing restarts in its
// voice; otherwise a free voice takes it, else the voice of the lowest rank
// (the oldest of equal rank) if the new effect ranks at least as high, else
// the effect is dropped. Notes step every EW_MIX_TICK frames, the sequencer's
// 1/60 s, exactly; rendering in pieces gives the same samples as at once.
void ew_mix_render(ew_mix_t *m, int16_t *stereo, unsigned frames);

// Voices sounding.
unsigned ew_mix_active(const ew_mix_t *m);

// Feeds a writer that takes up to `frames` and returns how many it took
// (tvout_audio_write). Renders EW_MIX_BLOCK frames at a time and writes until
// the writer takes less than offered or EW_MIX_FEED_MAX blocks were rendered;
// frames not taken are kept and offered first next time, so none is lost or
// repeated. Returns the frames taken.
unsigned ew_mix_feed(ew_mix_t *m, ew_mix_write_fn write);

#endif
