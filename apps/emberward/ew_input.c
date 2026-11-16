// ew_input.c — logical keys with per-source hold and key repeat.
#include "ew_input.h"

#include <string.h>

#define MAX_PENDING 3

void ew_input_init(ew_input_t *in) {
    memset(in, 0, sizeof *in);
}

bool ew_input_repeats(ew_key_t key) {
    return key == EW_KEY_LEFT || key == EW_KEY_RIGHT || key == EW_KEY_UP || key == EW_KEY_DOWN;
}

static void add_press(ew_input_t *in, ew_key_t key) {
    if (in->queue_count[key] < MAX_PENDING) in->queue_count[key]++;
    in->queue |= (uint16_t)(1u << key);
}

void ew_input_set(ew_input_t *in, ew_source_t src, ew_key_t key, bool down, uint32_t now_ms) {
    if ((unsigned)key >= EW_KEY_COUNT || (unsigned)src >= EW_SRC_COUNT) return;
    const uint8_t bit = (uint8_t)(1u << src);
    const bool was_held = in->held[key] != 0;
    if (down) {
        in->held[key] |= bit;
        if (!was_held) {
            add_press(in, key);
            in->next_repeat_ms[key] = now_ms + EW_REPEAT_DELAY_MS;
        }
    } else {
        in->held[key] &= (uint8_t)~bit;
    }
}

void ew_input_release_source(ew_input_t *in, ew_source_t src) {
    const uint8_t bit = (uint8_t)(1u << src);
    for (int k = 0; k < EW_KEY_COUNT; k++) in->held[k] &= (uint8_t)~bit;
}

void ew_input_tick(ew_input_t *in, uint32_t now_ms) {
    for (int k = 0; k < EW_KEY_COUNT; k++) {
        if (!in->held[k] || !ew_input_repeats((ew_key_t)k)) continue;
        // Signed difference: correct across the 32-bit millisecond wrap.
        int guard = 0;
        while ((int32_t)(now_ms - in->next_repeat_ms[k]) >= 0 && guard++ < MAX_PENDING) {
            add_press(in, (ew_key_t)k);
            in->next_repeat_ms[k] += EW_REPEAT_RATE_MS;
        }
        if ((int32_t)(now_ms - in->next_repeat_ms[k]) >= 0)
            in->next_repeat_ms[k] = now_ms + EW_REPEAT_RATE_MS;
    }
}

bool ew_input_take(ew_input_t *in, ew_key_t key) {
    if ((unsigned)key >= EW_KEY_COUNT || !in->queue_count[key]) return false;
    if (--in->queue_count[key] == 0) in->queue &= (uint16_t)~(1u << key);
    return true;
}

void ew_input_flush(ew_input_t *in) {
    in->queue = 0;
    memset(in->queue_count, 0, sizeof in->queue_count);
}

bool ew_input_held(const ew_input_t *in, ew_key_t key) {
    return (unsigned)key < EW_KEY_COUNT && in->held[key] != 0;
}
