// bsp/rfid/rfid_demod.c — RFID signal demodulator.
#include "rfid/rfid_demod.h"
#include "rfid/rfid_em4100.h"
#include <string.h>

#define ENV_BLOCK       16
#define ADC_MAX         4095
#define SAMPLE_US       4
#define BIT_PERIOD_US   512
#define LONG_THRESH_US  384
#define MIN_PULSE_US    40
#define MAX_PULSE_US    2048
#define MIN_SWING       200
#define CAL_BLOCKS      2000
#define HYST_DIV        6
#define MIN_HYST        40

static uint16_t s_env_min, s_env_max;
static uint16_t s_threshold, s_hysteresis;
static bool     s_calibrated;
static uint32_t s_cal_count;

static uint16_t s_block_peak;
static uint16_t s_block_count;

static bool     s_level;
static uint32_t s_level_us;

static uint8_t  s_bits[RFID_DEMOD_MAX_BITS];
static uint8_t  s_bit_pos;
static uint8_t  s_bit_total;
static bool     s_skip_next;

void rfid_demod_reset(void) {
    s_env_min = ADC_MAX;
    s_env_max = 0;
    s_threshold = ADC_MAX / 2;
    s_hysteresis = MIN_HYST;
    s_calibrated = false;
    s_cal_count = 0;
    s_block_peak = 0;
    s_block_count = 0;
    s_level = false;
    s_level_us = 0;
    memset(s_bits, 0, sizeof(s_bits));
    s_bit_pos = 0;
    s_bit_total = 0;
    s_skip_next = false;
}

bool rfid_demod_calibrated(void) { return s_calibrated; }

static void update_threshold(void) {
    const uint16_t swing = s_env_max > s_env_min ? s_env_max - s_env_min : 0;
    s_threshold = s_env_min + swing / 2;
    s_hysteresis = swing / HYST_DIV;
    if (s_hysteresis < MIN_HYST) s_hysteresis = MIN_HYST;
}

static void push_bit(uint8_t b) {
    s_bits[s_bit_pos] = b;
    s_bit_pos = (s_bit_pos + 1) % RFID_DEMOD_MAX_BITS;
    if (s_bit_total < RFID_DEMOD_MAX_BITS) s_bit_total++;
}

static bool try_decode(rfid_demod_tag_t *out) {
    if (s_bit_total < RFID_EM4100_FRAME_BITS) return false;
    uint8_t linear[RFID_DEMOD_MAX_BITS];
    for (uint8_t i = 0; i < s_bit_total; ++i)
        linear[i] = s_bits[(s_bit_pos - s_bit_total + i + RFID_DEMOD_MAX_BITS) % RFID_DEMOD_MAX_BITS];
    if (rfid_em4100_decode(linear, s_bit_total, out->id)) {
        out->valid = true;
        s_bit_total = 0;
        s_bit_pos = 0;
        return true;
    }
    return false;
}

bool rfid_demod_feed(const uint16_t *samples, uint32_t count,
                     rfid_demod_tag_t *out) {
    if (out) out->valid = false;

    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t raw = samples[i] & 0x0FFF;
        if (raw > s_block_peak) s_block_peak = raw;
        if (++s_block_count < ENV_BLOCK) continue;

        const uint16_t env = s_block_peak;
        s_block_peak = 0;
        s_block_count = 0;

        if (env < s_env_min) s_env_min = env;
        if (env > s_env_max) s_env_max = env;

        if (!s_calibrated) {
            if (++s_cal_count >= CAL_BLOCKS) {
                if ((s_env_max - s_env_min) >= MIN_SWING) {
                    update_threshold();
                    s_calibrated = true;
                } else {
                    s_env_min = ADC_MAX;
                    s_env_max = 0;
                    s_cal_count = 0;
                }
            }
            continue;
        }

        if (s_env_min < ADC_MAX) s_env_min++;
        if (s_env_max > 0) s_env_max--;
        update_threshold();

        const uint16_t hi = s_threshold + s_hysteresis / 2;
        const uint16_t lo = s_threshold > s_hysteresis / 2
                            ? s_threshold - s_hysteresis / 2 : 0;
        bool new_level = s_level;
        if (!s_level && env >= hi) new_level = true;
        else if (s_level && env <= lo) new_level = false;

        s_level_us += SAMPLE_US * ENV_BLOCK;

        if (new_level == s_level) continue;

        const uint32_t dur = s_level_us;
        s_level = new_level;
        s_level_us = 0;

        if (dur < MIN_PULSE_US) continue;
        if (dur > MAX_PULSE_US) {
            s_bit_total = 0;
            s_bit_pos = 0;
            s_skip_next = false;
            continue;
        }

        bool emit = false;
        if (dur >= LONG_THRESH_US) {
            emit = true;
            s_skip_next = true;
        } else if (s_skip_next) {
            s_skip_next = false;
        } else {
            emit = true;
            s_skip_next = true;
        }

        if (emit) {
            push_bit(s_level ? 0 : 1);
            if (out && !out->valid && try_decode(out))
                return true;
        }
    }
    return false;
}
