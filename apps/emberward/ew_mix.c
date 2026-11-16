// ew_mix.c — mixed 48 kHz sound effects for the TV (see ew_mix.h).
#include "ew_mix.h"

#include <string.h>

void ew_mix_init(ew_mix_t *m, int16_t amp) {
    memset(m, 0, sizeof *m);
    m->amp = amp;
    m->rng = 0xACE1u;
}

bool ew_mix_post(ew_mix_t *m, ew_sfx_id_t id) {
    if ((unsigned)id >= EW_SFX_COUNT) return false;
    const uint8_t head = m->head;
    const uint8_t next = (uint8_t)((head + 1u) % EW_MIX_QUEUE);
    if (next == m->tail) return false;
    m->queue[head] = (uint8_t)id;
    m->head = next;
    return true;
}

unsigned ew_mix_active(const ew_mix_t *m) {
    unsigned n = 0;
    for (int i = 0; i < EW_MIX_VOICES; i++) n += m->voice[i].id != EW_SFX_NONE;
    return n;
}

static uint32_t next_rng(ew_mix_t *m) {
    uint32_t r = m->rng;
    r ^= r << 13;
    r ^= r >> 17;
    r ^= r << 5;
    m->rng = r;
    return r;
}

static int16_t noise_level(ew_mix_t *m) {
    return (int16_t)((int32_t)(next_rng(m) % (2u * (uint32_t)m->amp + 1u)) - m->amp);
}

// Loads step `step` of the voice's effect; frees the voice past its end.
static void load_step(ew_mix_t *m, ew_voice_t *v) {
    const ew_step_t *st = &ew_sfx_steps((ew_sfx_id_t)v->id)[v->step];
    if (!st->ticks) {
        v->id = EW_SFX_NONE;
        return;
    }
    v->left = (uint32_t)st->ticks * EW_MIX_TICK;
    v->inc = (uint32_t)(((uint64_t)st->hz << 32) / EW_MIX_FS);
    v->wave = st->wave;
    if (v->wave == EW_WAVE_NOISE) v->level = noise_level(m);
}

static void start(ew_mix_t *m, ew_sfx_id_t id) {
    if (id == EW_SFX_NONE) {
        for (int i = 0; i < EW_MIX_VOICES; i++) m->voice[i].id = EW_SFX_NONE;
        return;
    }
    if (!ew_sfx_steps(id)) return;
    ew_voice_t *pick = 0;
    for (int i = 0; i < EW_MIX_VOICES && !pick; i++)
        if (m->voice[i].id == id) pick = &m->voice[i];
    for (int i = 0; i < EW_MIX_VOICES && !pick; i++)
        if (m->voice[i].id == EW_SFX_NONE) pick = &m->voice[i];
    if (!pick) {
        ew_voice_t *low = &m->voice[0];
        for (int i = 1; i < EW_MIX_VOICES; i++) {
            ew_voice_t *v = &m->voice[i];
            const unsigned pv = ew_sfx_priority((ew_sfx_id_t)v->id), pl = ew_sfx_priority((ew_sfx_id_t)low->id);
            if (pv < pl || (pv == pl && (int32_t)(v->serial - low->serial) < 0)) low = v;
        }
        if (ew_sfx_priority(id) < ew_sfx_priority((ew_sfx_id_t)low->id)) return;
        pick = low;
    }
    pick->id = (uint8_t)id;
    pick->step = 0;
    pick->phase = 0;
    pick->serial = ++m->serial;
    load_step(m, pick);
}

static int32_t voice_sample(ew_mix_t *m, ew_voice_t *v) {
    const uint32_t before = v->phase;
    const uint32_t p = before >> 16;   // [0, 65536) across one cycle
    const int32_t amp = m->amp;
    int32_t s;
    if (!v->inc) {
        s = 0;
    } else {
        switch (v->wave) {
        case EW_WAVE_PULSE:
            s = p < 16384u ? amp : -amp;
            break;
        case EW_WAVE_TRIANGLE: {
            const int32_t t = p < 32768u ? (int32_t)p : (int32_t)(65536u - p);
            s = -amp + (int32_t)(((int64_t)t * 2 * amp) / 32768);
            break;
        }
        case EW_WAVE_NOISE:
            s = v->level;
            break;
        default:
            s = p < 32768u ? amp : -amp;
            break;
        }
        v->phase = before + v->inc;
        // The noise wave takes a new level every half cycle.
        if (v->wave == EW_WAVE_NOISE && ((before ^ v->phase) & 0x80000000u)) v->level = noise_level(m);
    }
    if (--v->left == 0) {
        v->step++;
        load_step(m, v);
    }
    return s;
}

void ew_mix_render(ew_mix_t *m, int16_t *stereo, unsigned frames) {
    while (m->tail != m->head) {
        const uint8_t tail = m->tail;
        const ew_sfx_id_t id = (ew_sfx_id_t)m->queue[tail];
        m->tail = (uint8_t)((tail + 1u) % EW_MIX_QUEUE);
        start(m, id);
    }
    for (unsigned i = 0; i < frames; i++) {
        int32_t sum = 0;
        for (int k = 0; k < EW_MIX_VOICES; k++)
            if (m->voice[k].id != EW_SFX_NONE) sum += voice_sample(m, &m->voice[k]);
        if (sum > 32767) sum = 32767;
        if (sum < -32768) sum = -32768;
        stereo[2 * i] = (int16_t)sum;
        stereo[2 * i + 1] = (int16_t)sum;
    }
}

unsigned ew_mix_feed(ew_mix_t *m, ew_mix_write_fn write) {
    unsigned total = 0, blocks = 0;
    for (;;) {
        if (m->pending_at == m->pending_count) {
            if (blocks == EW_MIX_FEED_MAX) break;
            ew_mix_render(m, m->pending, EW_MIX_BLOCK);
            m->pending_at = 0;
            m->pending_count = EW_MIX_BLOCK;
            blocks++;
        }
        const unsigned offered = (unsigned)(m->pending_count - m->pending_at);
        const unsigned taken = write(&m->pending[2 * m->pending_at], offered);
        m->pending_at = (uint16_t)(m->pending_at + (taken < offered ? taken : offered));
        total += taken;
        if (taken < offered) break;
    }
    return total;
}
