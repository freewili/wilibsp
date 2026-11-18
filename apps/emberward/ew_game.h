// ew_game.h — Emberward game rules. Pure C, no SDK: host-tested in
// tests/test_ew_game.c. One call to ew_step() is one 1/60 s tick.
#ifndef EW_GAME_H
#define EW_GAME_H

#include <stdbool.h>
#include <stdint.h>

#define EW_LANES        8
#define EW_MAX_MOTES    24
#define EW_EVENT_QUEUE  32
#define EW_LIVES_START  3
#define EW_LIVES_MAX    5
#define EW_FLARE_FULL   1000
#define EW_FLARE_PER_GLIM 200
#define EW_GRACE_TICKS  60
#define EW_INTRO_TICKS  120
#define EW_CLEAR_TICKS  90

// Radii in pixels, Q8 fixed point for positions.
#define EW_R_SPAWN      176
#define EW_R_WARD       48
#define EW_R_CORE       20
#define EW_Q8(x)        ((int32_t)(x) << 8)

typedef enum { EW_EMBER, EW_GLIM, EW_DART, EW_HEART } ew_kind_t;

typedef enum {
    EW_PHASE_INTRO,      // "WAVE n" banner, nothing spawns
    EW_PHASE_PLAY,       // sparks spawn and fly
    EW_PHASE_CLEAR,      // wave cleared banner
    EW_PHASE_OVER        // core dark; the app takes it from here
} ew_phase_t;

typedef enum {
    EW_EV_SPAWN,         // lane, kind
    EW_EV_BLOCK,         // an ember or dart stopped by the ward
    EW_EV_ABSORB,        // a glim fed the core
    EW_EV_HEART,         // a heart gave a life
    EW_EV_SHATTER,       // a glim or heart blocked by the ward
    EW_EV_HIT,           // an ember or dart reached the core
    EW_EV_GRACE_PASS,    // reached the core during the grace period
    EW_EV_TURN,          // the ward moved
    EW_EV_FLARE,         // the flare fired
    EW_EV_FLARE_READY,   // the meter just filled
    EW_EV_WAVE_START,    // wave = the new wave
    EW_EV_WAVE_CLEAR,
    EW_EV_GAME_OVER
} ew_event_type_t;

typedef struct {
    uint8_t type;        // ew_event_type_t
    uint8_t lane;
    uint8_t kind;        // ew_kind_t
    uint8_t pad;
    uint32_t points;     // points this event scored
} ew_event_t;

typedef struct {
    bool active;
    uint8_t kind;
    uint8_t lane;
    int32_t r_q8;        // distance from the core centre
    int32_t speed_q8;    // pixels per tick, Q8
} ew_mote_t;

typedef struct {
    uint16_t count;          // sparks in the wave
    int32_t speed_q8;        // base speed
    uint16_t interval;       // ticks between spawns
    uint8_t glim_pct;        // chance of a glim
    uint8_t dart_pct;        // chance of a dart
    uint8_t heart_pct;       // chance of a heart (when below max lives)
    uint8_t pair_pct;        // chance a spawn is an adjacent pair
} ew_wave_params_t;

typedef struct {
    uint32_t rng;
    uint32_t tick;
    uint32_t score;
    uint16_t combo;
    uint16_t best_combo;
    uint8_t lives;
    uint8_t wave;
    uint8_t ward;            // the ward covers lanes ward and ward+1 (mod 8)
    uint8_t phase;           // ew_phase_t
    uint16_t phase_ticks;    // ticks spent in the phase
    uint16_t flare;          // 0..EW_FLARE_FULL
    uint16_t grace;          // ticks left of the grace period
    uint16_t to_spawn;       // sparks still to spawn this wave
    uint16_t spawn_timer;    // ticks to the next spawn
    uint32_t sparks_handled; // statistics for the game-over screen
    ew_wave_params_t params;
    ew_mote_t motes[EW_MAX_MOTES];
    ew_event_t events[EW_EVENT_QUEUE];
    uint8_t ev_head, ev_count;
} ew_game_t;

// New game at wave 1. seed 0 is replaced by a fixed non-zero seed.
void ew_init(ew_game_t *g, uint32_t seed);

// The parameters of wave n (n >= 1).
ew_wave_params_t ew_wave_params(unsigned wave);

// Turn the ward by delta lanes (-1, +1, or 4 for a flip). Ignored once over.
void ew_turn(ew_game_t *g, int delta);

// Fire the flare. True if it was full and fired.
bool ew_flare(ew_game_t *g);

// One tick.
void ew_step(ew_game_t *g);

// The oldest queued event; false when none.
bool ew_next_event(ew_game_t *g, ew_event_t *ev);

// The multiplier for the current combo: 1 + combo / 8, at most 5.
unsigned ew_multiplier(const ew_game_t *g);

// True when the ward covers the lane.
bool ew_ward_covers(const ew_game_t *g, unsigned lane);

// Deterministic PRNG step (xorshift32), exposed for the renderer's effects.
uint32_t ew_rand(uint32_t *state);

#endif
