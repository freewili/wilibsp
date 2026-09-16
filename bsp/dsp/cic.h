// src/dsp/cic.h — 3rd-order CIC decimator: 1-bit PDM in, 16-bit PCM out.
#ifndef CIC_H
#define CIC_H
#include <stdint.h>

#define CIC_ORDER 3
#ifndef CIC_DECIMATE
#define CIC_DECIMATE 64u
#endif

// A CIC integrator is meant to wrap: the comb stage subtracts the wrapped
// values back out and recovers the right answer. Wrapping a signed type is
// undefined behaviour, so the accumulators are unsigned and the result is
// reinterpreted as signed once the comb cascade has run.
typedef struct {
    uint32_t integ[CIC_ORDER];  // integrator accumulators (modular)
    uint32_t comb[CIC_ORDER];   // comb delay registers (at decimated rate)
    uint32_t phase;             // 0..CIC_DECIMATE-1
} cic_t;

void cic_init(cic_t* c);

// Feed one PDM bit (0 or 1, mapped to -1/+1 internally). Returns 1 and writes a
// PCM sample to *out exactly once per CIC_DECIMATE bits; otherwise returns 0.
int cic_push_bit(cic_t* c, int bit, int16_t* out);

#endif // CIC_H
