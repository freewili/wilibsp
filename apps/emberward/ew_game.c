// ew_game.c — Emberward game rules (see ew_game.h and DESIGN.md).
#include "ew_game.h"

#include <string.h>

#define PTS_EMBER   10u
#define PTS_DART    20u
#define PTS_GLIM    25u
#define PTS_HEART   50u
#define PTS_FLARE   15u
#define PTS_WAVE    100u

// A new spark does not spawn in a lane whose last spark is still this close
// to the spawn ring, so two sparks never overlap.
#define SPAWN_GAP_Q8 EW_Q8(28)

uint32_t ew_rand(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static unsigned rand_below(ew_game_t *g, unsigned n) {
    return n ? ew_rand(&g->rng) % n : 0;
}

static void push(ew_game_t *g, ew_event_type_t type, unsigned lane, unsigned kind,
                 uint32_t points, bool by_flare) {
    if (g->ev_count >= EW_EVENT_QUEUE) return;
    ew_event_t *ev = &g->events[(g->ev_head + g->ev_count) % EW_EVENT_QUEUE];
    ev->type = (uint8_t)type;
    ev->lane = (uint8_t)lane;
    ev->kind = (uint8_t)kind;
    ev->pad = by_flare ? 1 : 0;
    ev->points = points;
    g->ev_count++;
}

bool ew_next_event(ew_game_t *g, ew_event_t *ev) {
    if (!g->ev_count) return false;
    *ev = g->events[g->ev_head];
    g->ev_head = (uint8_t)((g->ev_head + 1) % EW_EVENT_QUEUE);
    g->ev_count--;
    return true;
}

ew_wave_params_t ew_wave_params(unsigned wave) {
    if (wave < 1) wave = 1;
    const unsigned w = wave - 1;
    ew_wave_params_t p;
    p.count = (uint16_t)(12 + 4 * wave);
    int32_t speed = 230 + 38 * (int32_t)w;
    p.speed_q8 = speed > 820 ? 820 : speed;
    int interval = 78 - 6 * (int)w;
    p.interval = (uint16_t)(interval < 16 ? 16 : interval);
    p.glim_pct = 30;
    if (wave >= 3) {
        unsigned d = 6 + 3 * (wave - 3);
        p.dart_pct = (uint8_t)(d > 25 ? 25 : d);
    } else {
        p.dart_pct = 0;
    }
    p.heart_pct = wave >= 4 ? 4 : 0;
    if (wave >= 2) {
        unsigned q = 12 + 4 * (wave - 2);
        p.pair_pct = (uint8_t)(q > 40 ? 40 : q);
    } else {
        p.pair_pct = 0;
    }
    return p;
}

static void begin_wave(ew_game_t *g, unsigned wave) {
    g->wave = (uint8_t)(wave > 255 ? 255 : wave);
    g->params = ew_wave_params(g->wave);
    g->to_spawn = g->params.count;
    g->spawn_timer = 30;
    g->phase = EW_PHASE_INTRO;
    g->phase_ticks = 0;
    push(g, EW_EV_WAVE_START, 0, 0, g->wave, false);
}

void ew_init(ew_game_t *g, uint32_t seed) {
    memset(g, 0, sizeof *g);
    g->rng = seed ? seed : 0x2545F491u;
    g->lives = EW_LIVES_START;
    g->ward = 6;   // covering up and up-right: lanes 6 and 7
    begin_wave(g, 1);
}

unsigned ew_multiplier(const ew_game_t *g) {
    unsigned m = 1u + g->combo / 8u;
    return m > 5u ? 5u : m;
}

bool ew_ward_covers(const ew_game_t *g, unsigned lane) {
    lane %= EW_LANES;
    return lane == g->ward || lane == (unsigned)((g->ward + 1) % EW_LANES);
}

void ew_turn(ew_game_t *g, int delta) {
    if (g->phase == EW_PHASE_OVER) return;
    int w = ((int)g->ward + delta) % EW_LANES;
    if (w < 0) w += EW_LANES;
    g->ward = (uint8_t)w;
    push(g, EW_EV_TURN, g->ward, 0, 0, false);
}

static void good_call(ew_game_t *g) {
    g->combo++;
    if (g->combo > g->best_combo) g->best_combo = g->combo;
    g->sparks_handled++;
}

static void add_flare(ew_game_t *g, unsigned amount) {
    const bool was_full = g->flare >= EW_FLARE_FULL;
    unsigned f = g->flare + amount;
    g->flare = (uint16_t)(f > EW_FLARE_FULL ? EW_FLARE_FULL : f);
    if (!was_full && g->flare >= EW_FLARE_FULL) push(g, EW_EV_FLARE_READY, 0, 0, 0, false);
}

static bool is_harmful(unsigned kind) {
    return kind == EW_EMBER || kind == EW_DART;
}

// A spark answered in the player's favour (blocked if harmful, absorbed if not).
static uint32_t resolve_good(ew_game_t *g, ew_mote_t *m, bool by_flare) {
    const unsigned mult = ew_multiplier(g);
    uint32_t pts;
    ew_event_type_t type;
    if (is_harmful(m->kind)) {
        pts = (by_flare ? PTS_FLARE : (m->kind == EW_DART ? PTS_DART : PTS_EMBER)) * mult;
        type = EW_EV_BLOCK;
    } else if (m->kind == EW_HEART) {
        pts = PTS_HEART * mult;
        if (g->lives < EW_LIVES_MAX) g->lives++;
        type = EW_EV_HEART;
    } else {
        pts = PTS_GLIM * mult;
        if (!by_flare) add_flare(g, EW_FLARE_PER_GLIM);
        type = EW_EV_ABSORB;
    }
    g->score += pts;
    good_call(g);
    push(g, type, m->lane, m->kind, pts, by_flare);
    m->active = false;
    return pts;
}

bool ew_flare(ew_game_t *g) {
    if (g->flare < EW_FLARE_FULL || g->phase == EW_PHASE_OVER) return false;
    g->flare = 0;
    uint32_t total = 0;
    for (int i = 0; i < EW_MAX_MOTES; i++)
        if (g->motes[i].active) total += resolve_good(g, &g->motes[i], true);
    push(g, EW_EV_FLARE, g->ward, 0, total, false);
    return true;
}

static ew_mote_t *free_mote(ew_game_t *g) {
    for (int i = 0; i < EW_MAX_MOTES; i++)
        if (!g->motes[i].active) return &g->motes[i];
    return 0;
}

static bool lane_clear(const ew_game_t *g, unsigned lane) {
    for (int i = 0; i < EW_MAX_MOTES; i++) {
        const ew_mote_t *m = &g->motes[i];
        if (m->active && m->lane == lane && m->r_q8 > EW_Q8(EW_R_SPAWN) - SPAWN_GAP_Q8) return false;
    }
    return true;
}

static int32_t kind_speed(const ew_game_t *g, unsigned kind) {
    const int32_t s = g->params.speed_q8;
    switch (kind) {
    case EW_DART:  return s * 9 / 5;
    case EW_GLIM:  return s * 9 / 10;
    case EW_HEART: return s * 4 / 5;
    default:       return s;
    }
}

static bool spawn_one(ew_game_t *g, unsigned lane, unsigned kind) {
    if (!lane_clear(g, lane)) return false;
    ew_mote_t *m = free_mote(g);
    if (!m) return false;
    m->active = true;
    m->kind = (uint8_t)kind;
    m->lane = (uint8_t)lane;
    m->r_q8 = EW_Q8(EW_R_SPAWN);
    m->speed_q8 = kind_speed(g, kind);
    push(g, EW_EV_SPAWN, lane, kind, 0, false);
    return true;
}

static unsigned pick_kind(ew_game_t *g) {
    const ew_wave_params_t *p = &g->params;
    unsigned roll = rand_below(g, 100);
    if (g->lives < EW_LIVES_MAX && roll < p->heart_pct) return EW_HEART;
    roll = rand_below(g, 100);
    if (roll < p->glim_pct) return EW_GLIM;
    roll = rand_below(g, 100);
    if (roll < p->dart_pct) return EW_DART;
    return EW_EMBER;
}

static void spawn(ew_game_t *g) {
    const unsigned lane = rand_below(g, EW_LANES);
    if (g->to_spawn >= 2 && rand_below(g, 100) < g->params.pair_pct) {
        // An adjacent pair that one ward position can answer: two embers, or
        // an ember beside a glim (the ward then covers the ember and the lane
        // on its far side).
        const unsigned other = (lane + 1) % EW_LANES;
        if (!lane_clear(g, lane) || !lane_clear(g, other)) return;
        const bool mixed = rand_below(g, 2) != 0;
        const bool glim_first = rand_below(g, 2) != 0;
        unsigned k0 = EW_EMBER, k1 = EW_EMBER;
        if (mixed) {
            if (glim_first) k0 = EW_GLIM;
            else k1 = EW_GLIM;
        }
        if (spawn_one(g, lane, k0)) g->to_spawn--;
        if (spawn_one(g, other, k1)) g->to_spawn--;
    } else {
        if (spawn_one(g, lane, pick_kind(g))) g->to_spawn--;
    }
}

static bool any_active(const ew_game_t *g) {
    for (int i = 0; i < EW_MAX_MOTES; i++)
        if (g->motes[i].active) return true;
    return false;
}

static void core_reached(ew_game_t *g, ew_mote_t *m) {
    if (!is_harmful(m->kind)) {
        resolve_good(g, m, false);
        return;
    }
    m->active = false;
    if (g->grace) {
        push(g, EW_EV_GRACE_PASS, m->lane, m->kind, 0, false);
        return;
    }
    g->combo = 0;
    if (g->lives) g->lives--;
    g->grace = EW_GRACE_TICKS;
    push(g, EW_EV_HIT, m->lane, m->kind, 0, false);
    if (!g->lives) {
        g->phase = EW_PHASE_OVER;
        g->phase_ticks = 0;
        for (int i = 0; i < EW_MAX_MOTES; i++) g->motes[i].active = false;
        push(g, EW_EV_GAME_OVER, 0, 0, g->score, false);
    }
}

static void move_motes(ew_game_t *g) {
    for (int i = 0; i < EW_MAX_MOTES && g->phase != EW_PHASE_OVER; i++) {
        ew_mote_t *m = &g->motes[i];
        if (!m->active) continue;
        const int32_t before = m->r_q8;
        m->r_q8 -= m->speed_q8;
        if (before > EW_Q8(EW_R_WARD) && m->r_q8 <= EW_Q8(EW_R_WARD) && ew_ward_covers(g, m->lane)) {
            if (is_harmful(m->kind)) {
                resolve_good(g, m, false);
            } else {
                g->combo = 0;
                m->active = false;
                push(g, EW_EV_SHATTER, m->lane, m->kind, 0, false);
            }
            continue;
        }
        if (m->r_q8 <= EW_Q8(EW_R_CORE)) core_reached(g, m);
    }
}

void ew_step(ew_game_t *g) {
    g->tick++;
    if (g->phase_ticks < 0xFFFF) g->phase_ticks++;
    if (g->grace) g->grace--;
    switch (g->phase) {
    case EW_PHASE_INTRO:
        if (g->phase_ticks >= EW_INTRO_TICKS) {
            g->phase = EW_PHASE_PLAY;
            g->phase_ticks = 0;
        }
        break;
    case EW_PHASE_PLAY:
        if (g->to_spawn) {
            if (g->spawn_timer) g->spawn_timer--;
            if (!g->spawn_timer) {
                spawn(g);
                g->spawn_timer = g->params.interval;
            }
        }
        move_motes(g);
        if (g->phase == EW_PHASE_PLAY && !g->to_spawn && !any_active(g)) {
            const uint32_t bonus = PTS_WAVE * g->wave;
            g->score += bonus;
            g->phase = EW_PHASE_CLEAR;
            g->phase_ticks = 0;
            push(g, EW_EV_WAVE_CLEAR, 0, 0, bonus, false);
        }
        break;
    case EW_PHASE_CLEAR:
        if (g->phase_ticks >= EW_CLEAR_TICKS) begin_wave(g, g->wave + 1u);
        break;
    default:
        break;
    }
}
