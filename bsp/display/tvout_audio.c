// bsp/display/tvout_audio.c — see tvout_audio.h.
//
// The feeder's hardware side: one TIMER1 alarm, claimed once and kept, whose
// interrupt runs the core's tick (tvout_audio_core.h). Each tick is armed a
// time after the previous one ended, so other interrupts of the same priority
// on the core are served between two ticks. The handler runs from SRAM.
#include "display/tvout_audio.h"
#include "display/tvout_audio_core.h"
#include "display/tvout_internal.h"
#include "display/tvout_list.h"
#include "display/tvout.h"
#include "platform/timer1.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/structs/timer.h"
#include "pico/stdlib.h"

#define STOP_TIMEOUT_US 5000u

static uint32_t s_pool[TVOUT_LIST_AUDIO_POOL_WORDS];
static bool s_framing;
static tvout_tone_t s_tone = TVOUT_TONE_OFF;
static volatile bool s_running;
static volatile bool s_in_write;
static const char *s_error = "";

static bool s_alarm_claimed;
static bool s_tick_installed;
static uint32_t s_tick_alarm;
static uint32_t s_tick_mask;
static uint32_t s_tick_irq;

static void __not_in_flash_func(tick_irq)(void) {
    const uint32_t mask = s_tick_mask;
    hw_clear_bits(&timer1_hw->intf, mask);
    timer1_hw->intr = mask;
    const uint32_t gap_us = tvout_audio_core_tick(tvout_scan_entry);
    if (gap_us == 0) return;

    timer1_arm_after(s_tick_alarm, mask, gap_us);
}

void tvout_set_audio_framing(bool on) {
    s_framing = on;
    tvout_set_framing_pool(on ? s_pool : NULL, TVOUT_LIST_AUDIO_POOL_WORDS);
}

bool tvout_audio_framing(void) { return s_framing; }

static void apply_tone(void) {
    tvout_audio_core_set_tone(
        (s_tone == TVOUT_TONE_LEFT || s_tone == TVOUT_TONE_BOTH ? TVOUT_AUDIO_CORE_TONE_LEFT : 0u) |
        (s_tone == TVOUT_TONE_RIGHT || s_tone == TVOUT_TONE_BOTH ? TVOUT_AUDIO_CORE_TONE_RIGHT : 0u));
}

bool tvout_audio_start(void) {
    if (s_running) return true;
    s_error = "";
    if (!tvout_running() || !tvout_framed()) {
        s_error = "The TV sound could not start: no picture with audio framing is running.";
        return false;
    }

    if (!s_alarm_claimed) {
        const int alarm = timer_hardware_alarm_claim_unused(timer1_hw, false);
        if (alarm < 0) {
            s_error = "The TV sound could not start: no free TIMER1 alarm.";
            return false;
        }
        s_tick_alarm = (uint32_t)alarm;
        s_tick_mask = 1u << alarm;
        s_tick_irq = timer_hardware_alarm_get_irq_num(timer1_hw, (uint)alarm);
        s_alarm_claimed = true;
    }

    if (!timer1_counts_us()) {
        s_error = "The TV sound could not start: TIMER1 does not count microseconds.";
        return false;
    }

    if (!tvout_audio_core_configure()) {
        s_error = "The TV sound could not start: the picture's audio slots do not fit.";
        return false;
    }
    apply_tone();

    if (!s_tick_installed) {
        s_tick_installed = true;
        irq_set_exclusive_handler(s_tick_irq, tick_irq);
        irq_set_priority(s_tick_irq, PICO_HIGHEST_IRQ_PRIORITY);
        hw_set_bits(&timer1_hw->inte, s_tick_mask);
        irq_set_enabled(s_tick_irq, true);
    }

    // Everything the tick and tvout_audio_write() use is in place before they
    // are let in.
    __dmb();
    tvout_audio_core_run(true);
    s_running = true;
    hw_set_bits(&timer1_hw->intf, s_tick_mask);
    return true;
}

void tvout_audio_stop(void) {
    if (!s_running) return;

    // tvout_audio_write() and this never run inside each other: each side
    // writes its flag, then reads the other's.
    s_running = false;
    __dmb();
    while (s_in_write) tight_loop_contents();

    // The tick puts a null packet in every slot, a slot at a time once the TV
    // has finished with it, then stops arming itself.
    tvout_audio_core_mute();
    __dmb();
    hw_set_bits(&timer1_hw->intf, s_tick_mask);
    const uint32_t from_us = time_us_32();
    while (!tvout_audio_core_muted() && tvout_audio_core_running() && time_us_32() - from_us < STOP_TIMEOUT_US)
        tight_loop_contents();

    tvout_audio_core_run(false);
    __dmb();
    busy_wait_us(100);                      // a tick in flight ends and is not re-armed
    timer1_hw->armed = s_tick_mask;

    // Not finished in time (the tick was held off): the rest at once. A slot
    // the TV is reading meanwhile sends one packet that fails its parity check
    // and is dropped by the TV.
    if (!tvout_audio_core_muted() && tvout_running())
        tvout_audio_core_null_all();
}

bool tvout_audio_running(void) { return s_running; }

const char *tvout_audio_last_error(void) { return s_error; }

void tvout_audio_tone(tvout_tone_t tone) {
    s_tone = tone;
    apply_tone();
}

unsigned tvout_audio_write(const int16_t *stereo, unsigned frames) {
    if (stereo == NULL) return 0;
    s_in_write = true;
    __dmb();
    if (!s_running) {
        s_in_write = false;
        return 0;
    }
    const unsigned taken = tvout_audio_core_write(stereo, frames);
    s_in_write = false;
    return taken;
}

unsigned tvout_audio_room(void) {
    return s_running ? tvout_audio_core_room() : 0u;
}

void tvout_audio_get_stats(tvout_audio_stats_t *out) {
    tvout_audio_core_counts_t counts;
    tvout_audio_core_get_counts(&counts);
    out->packets = counts.packets;
    out->underruns = counts.underruns;
    out->late = counts.late;
    out->repeats = counts.repeats;
    out->lost = counts.lost;
}
