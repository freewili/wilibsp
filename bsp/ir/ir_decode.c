// bsp/ir/ir_decode.c — pure-logic protocol decoders (no Pico SDK).
#include "ir_decode.h"
#include "ir_protocols.h"

// Pulse-distance bits: constant mark, space width selects the bit.
// Consumes exactly 2*n_bits entries starting at t[0] (a mark).
// Checks the wider space first: tolerance bands can overlap at the margin,
// and wide-first resolves the tie toward the more specific match.
static bool pd_bits(const uint32_t *t, uint32_t n, uint32_t n_bits,
                    uint32_t mark, uint32_t space0, uint32_t space1,
                    bool lsb_first, uint64_t *out) {
    if (n < 2u * n_bits) return false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n_bits; i++) {
        if (!ir_match_us(t[2*i], mark)) return false;
        uint32_t sp = t[2*i + 1];
        uint32_t bit;
        if (ir_match_us(sp, space1)) bit = 1;
        else if (ir_match_us(sp, space0)) bit = 0;
        else return false;
        if (bit) v |= 1ull << (lsb_first ? i : (n_bits - 1u - i));
    }
    *out = v;
    return true;
}

// Pulse-width bits: mark width selects the bit, constant separator space.
// Consumes 2*n_bits - 1 entries (no space after the final mark).
static bool pw_bits(const uint32_t *t, uint32_t n, uint32_t n_bits,
                    uint32_t mark0, uint32_t mark1, uint32_t space,
                    uint64_t *out) {
    if (n < 2u * n_bits - 1u) return false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n_bits; i++) {
        uint32_t m = t[2*i];
        if (ir_match_us(m, mark1)) v |= 1ull << i;          // wide-first (see pd_bits)
        else if (!ir_match_us(m, mark0)) return false;
        if (i + 1 < n_bits && !ir_match_us(t[2*i + 1], space)) return false;
    }
    *out = v;
    return true;
}

bool ir_decode_nec(const uint32_t *t, uint32_t n, ir_message_t *out) {
    // Repeat frame: 9000 mark, 2250 space, 560 stop mark.
    if (n == 3 && ir_match_us(t[0], 9000) && ir_match_us(t[1], 2250) &&
        ir_match_us(t[2], 560)) {
        out->protocol = IR_PROTO_NEC; out->address = 0; out->command = 0;
        out->repeat = true;
        return true;
    }
    if (n != 67) return false;
    if (!ir_match_us(t[0], 9000) || !ir_match_us(t[1], 4500)) return false;
    uint64_t v;
    if (!pd_bits(t + 2, n - 2, 32, 560, 560, 1690, true, &v)) return false;
    if (!ir_match_us(t[66], 560)) return false;             // stop mark
    uint8_t a = (uint8_t)v, na = (uint8_t)(v >> 8);
    uint8_t c = (uint8_t)(v >> 16), nc = (uint8_t)(v >> 24);
    out->repeat = false;
    if ((uint8_t)(a ^ na) == 0xFF && (uint8_t)(c ^ nc) == 0xFF) {
        out->protocol = IR_PROTO_NEC; out->address = a; out->command = c;
    } else {
        // Flipper NECext: 16-bit address + 16-bit command, no parity demands.
        out->protocol = IR_PROTO_NECEXT;
        out->address = (uint32_t)(v & 0xFFFFu);
        out->command = (uint32_t)((v >> 16) & 0xFFFFu);
    }
    return true;
}

bool ir_decode_samsung32(const uint32_t *t, uint32_t n, ir_message_t *out) {
    if (n != 67) return false;
    if (!ir_match_us(t[0], 4500) || !ir_match_us(t[1], 4500)) return false;
    uint64_t v;
    if (!pd_bits(t + 2, n - 2, 32, 550, 550, 1650, true, &v)) return false;
    if (!ir_match_us(t[66], 550)) return false;             // stop mark
    uint8_t a1 = (uint8_t)v, a2 = (uint8_t)(v >> 8);
    uint8_t c = (uint8_t)(v >> 16), nc = (uint8_t)(v >> 24);
    if (a1 != a2 || (uint8_t)(c ^ nc) != 0xFF) return false;
    out->protocol = IR_PROTO_SAMSUNG32;
    out->address = a1; out->command = c; out->repeat = false;
    return true;
}

bool ir_decode_sirc(const uint32_t *t, uint32_t n, ir_message_t *out) {
    if (n < 25) return false;
    if (!ir_match_us(t[0], 2400) || !ir_match_us(t[1], 600)) return false;
    // Variant by exact edge count: 2 header + 2*bits - 1.
    uint32_t bits;
    ir_protocol_t proto;
    if      (n == 25) { bits = 12; proto = IR_PROTO_SIRC;   }
    else if (n == 31) { bits = 15; proto = IR_PROTO_SIRC15; }
    else if (n == 41) { bits = 20; proto = IR_PROTO_SIRC20; }
    else return false;
    uint64_t v;
    if (!pw_bits(t + 2, n - 2, bits, 600, 1200, 600, &v)) return false;
    out->protocol = proto;
    out->command  = (uint32_t)(v & 0x7Fu);                  // cmd7 first, LSB-first
    out->address  = (uint32_t)(v >> 7);                     // addr5 / addr8 / addr13
    out->repeat   = false;
    return true;
}

bool ir_decode_rca(const uint32_t *t, uint32_t n, ir_message_t *out) {
    if (n != 51) return false;
    if (!ir_match_us(t[0], 4000) || !ir_match_us(t[1], 4000)) return false;
    uint64_t v;
    if (!pd_bits(t + 2, n - 2, 24, 500, 1000, 2000, false, &v)) return false;  // MSB-first
    if (!ir_match_us(t[50], 500)) return false;             // stop mark
    uint32_t a  = (uint32_t)((v >> 20) & 0xFu);
    uint32_t c  = (uint32_t)((v >> 12) & 0xFFu);
    uint32_t na = (uint32_t)((v >> 8)  & 0xFu);
    uint32_t nc = (uint32_t)( v        & 0xFFu);
    if ((a ^ na) != 0xFu || (c ^ nc) != 0xFFu) return false;
    out->protocol = IR_PROTO_RCA;
    out->address = a; out->command = c; out->repeat = false;
    return true;
}

bool ir_decode_rc5(const uint32_t *t, uint32_t n, ir_message_t *out) {
    if (n < 10 || n > 28) return false;
    // Expand durations into 889us half-bit levels. Even index = mark.
    // Prepend the invisible leading space half (S1's first half).
    uint8_t hb[32]; uint32_t nh = 0;
    hb[nh++] = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t units;
        if (ir_match_us(t[i], 889)) units = 1;
        else if (ir_match_us(t[i], 1778)) units = 2;
        else return false;
        uint8_t lvl = (i & 1u) ? 0 : 1;
        for (uint32_t u = 0; u < units; u++) {
            if (nh >= 32) return false;
            hb[nh++] = lvl;
        }
    }
    if (nh == 27) hb[nh++] = 0;      // final 0-bit's space half eaten by the gap
    if (nh != 28) return false;
    uint32_t bits = 0;
    for (uint32_t b = 0; b < 14; b++) {
        uint8_t h0 = hb[2*b], h1 = hb[2*b + 1];
        if (h0 == 0 && h1 == 1)      bits = (bits << 1) | 1u;  // space->mark = 1
        else if (h0 == 1 && h1 == 0) bits = (bits << 1);
        else return false;
    }
    if (!(bits & (1u << 13))) return false;      // S1 must be 1
    uint32_t s2 = (bits >> 12) & 1u;
    out->protocol = IR_PROTO_RC5;
    out->address  = (bits >> 6) & 0x1Fu;
    out->command  = (bits & 0x3Fu) | ((s2 ^ 1u) << 6);   // RC5X extension bit
    out->repeat   = false;                               // toggle (bit 11) ignored
    return true;
}

// Read one RC6 Manchester bit of `width` half-units from hb at *idx.
// Returns 1 / 0, or -1 on an invalid (non-uniform or same-level) cell.
static int rc6_take_bit(const uint8_t *hb, uint32_t nh, uint32_t *idx, uint32_t width) {
    if (*idx + 2u * width > nh) return -1;
    uint8_t h0 = hb[*idx], h1 = hb[*idx + width];
    for (uint32_t u = 1; u < width; u++)
        if (hb[*idx + u] != h0 || hb[*idx + width + u] != h1) return -1;
    *idx += 2u * width;
    if (h0 == 1 && h1 == 0) return 1;            // mark->space = 1 (RC6 polarity)
    if (h0 == 0 && h1 == 1) return 0;
    return -1;
}

bool ir_decode_rc6(const uint32_t *t, uint32_t n, ir_message_t *out) {
    if (n < 20) return false;
    if (!ir_match_us(t[0], 2666) || !ir_match_us(t[1], 889)) return false;
    // Expand the rest into 444us half-bit units (1..3 units per duration).
    uint8_t hb[64]; uint32_t nh = 0;
    for (uint32_t i = 2; i < n; i++) {
        uint32_t units = 0;
        for (uint32_t u = 1; u <= 3; u++)
            if (ir_match_us(t[i], 444u * u)) { units = u; break; }
        if (!units) return false;
        uint8_t lvl = (i & 1u) ? 0 : 1;
        for (uint32_t u = 0; u < units; u++) {
            if (nh >= 64) return false;
            hb[nh++] = lvl;
        }
    }
    // start(2) + mode(6) + toggle(4) + data(32) = 44 units; trailing space
    // half of a final 1-bit (mark->space) is eaten by the gap.
    if (nh == 43) hb[nh++] = 0;
    if (nh != 44) return false;
    uint32_t idx = 0;
    if (rc6_take_bit(hb, nh, &idx, 1) != 1) return false;         // start bit = 1
    for (int i = 0; i < 3; i++)
        if (rc6_take_bit(hb, nh, &idx, 1) != 0) return false;     // mode 000
    if (rc6_take_bit(hb, nh, &idx, 2) < 0) return false;          // toggle, ignored
    uint32_t v = 0;
    for (int i = 0; i < 16; i++) {
        int b = rc6_take_bit(hb, nh, &idx, 1);
        if (b < 0) return false;
        v = (v << 1) | (uint32_t)b;                               // MSB-first
    }
    out->protocol = IR_PROTO_RC6;
    out->address  = (v >> 8) & 0xFFu;
    out->command  = v & 0xFFu;
    out->repeat   = false;
    return true;
}

bool ir_decode_kaseikyo(const uint32_t *t, uint32_t n, ir_message_t *out) {
    if (n != 99) return false;
    if (!ir_match_us(t[0], 3456) || !ir_match_us(t[1], 1728)) return false;
    uint64_t v;
    if (!pd_bits(t + 2, n - 2, 48, 432, 432, 1296, true, &v)) return false;
    if (!ir_match_us(t[98], 432)) return false;             // stop mark
    out->protocol = IR_PROTO_KASEIKYO;
    out->address  = (uint32_t)(v & 0xFFFFFFu);
    out->command  = (uint32_t)(v >> 24);
    out->repeat   = false;
    return true;
}

// Fixed order: most-constrained first. NECext is the catch-all for any
// NEC-timed 32-bit frame, so NEC (which contains it) runs first.
static bool (*const k_decoders[])(const uint32_t *, uint32_t, ir_message_t *) = {
    ir_decode_nec, ir_decode_samsung32, ir_decode_kaseikyo,
    ir_decode_rc6, ir_decode_rc5, ir_decode_sirc, ir_decode_rca,
};
#define K_NUM_DECODERS (sizeof k_decoders / sizeof k_decoders[0])

bool ir_decode(const uint32_t *t, uint32_t n, ir_message_t *out) {
    for (unsigned i = 0; i < K_NUM_DECODERS; i++)
        if (k_decoders[i](t, n, out)) return true;
    return false;
}

uint32_t ir_decode_all(const uint32_t *t, uint32_t n, ir_message_t *out, uint32_t max) {
    uint32_t k = 0;
    for (unsigned i = 0; i < K_NUM_DECODERS && k < max; i++)
        if (k_decoders[i](t, n, &out[k])) k++;
    return k;
}
