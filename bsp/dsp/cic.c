// src/dsp/cic.c — 3rd-order CIC. Gain = CIC_DECIMATE^CIC_ORDER; we right-shift
// to land a full-scale PDM swing near int16 range.
#include "dsp/cic.h"

// log2(64^3) = 18 bits of CIC growth; shift by 20 (not 18) to leave ~6 dB of
// headroom: full-scale DC -> 2^18 >> 4 = 16384, well below int16 clip.
#define CIC_SHIFT 20

void cic_init(cic_t* c) {
    for (int i = 0; i < CIC_ORDER; i++) { c->integ[i] = 0; c->comb[i] = 0; }
    c->phase = 0;
}

int cic_push_bit(cic_t* c, int bit, int16_t* out) {
    uint32_t x = bit ? 1u : UINT32_MAX;   // map PDM {0,1} -> {+1,-1}
    // Integrator cascade (runs at full PDM rate). A steady input drives these
    // past the end of their range within seconds; unsigned makes that wrap
    // defined rather than undefined.
    c->integ[0] += x;
    for (int i = 1; i < CIC_ORDER; i++) c->integ[i] += c->integ[i-1];

    if (++c->phase < CIC_DECIMATE) return 0;
    c->phase = 0;

    // Comb cascade (runs at decimated rate). The differences undo the
    // integrators' wrapping, so the result is back in range here.
    uint32_t v = c->integ[CIC_ORDER-1];
    for (int i = 0; i < CIC_ORDER; i++) {
        uint32_t d = v - c->comb[i];
        c->comb[i] = v;
        v = d;
    }
    int32_t s = (int32_t)v >> (CIC_SHIFT - 16);  // scale toward int16
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    *out = (int16_t)s;
    return 1;
}
