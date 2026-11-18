// bsp/platform/timer1.h — TIMER1 helpers for the drivers that time from its
// alarms: bsp/cec, the TV sound feeder and the TV line ring (bsp/display).
#ifndef TIMER1_H
#define TIMER1_H
#include <stdbool.h>
#include <stdint.h>
#include "pico.h"
#include "hardware/timer.h"
#include "hardware/structs/timer.h"

// TIMER1 counts microseconds: a 200 us busy wait reads 150-250 us on it.
static inline bool timer1_counts_us(void) {
    const uint32_t a = timer1_hw->timerawl;
    busy_wait_us(200);
    const uint32_t b = timer1_hw->timerawl;
    return b - a >= 150u && b - a <= 250u;
}

// Arms alarm (bit mask mask) gap_us from now. If that time passed while
// arming and the alarm is not already pending, raises it through the force
// register. Always inlined, for handlers that run from SRAM.
static __force_inline void timer1_arm_after(uint32_t alarm, uint32_t mask, uint32_t gap_us) {
    const uint32_t next_us = timer1_hw->timerawl + gap_us;
    timer1_hw->alarm[alarm] = next_us;
    if ((int32_t)(next_us - timer1_hw->timerawl) <= 0 && !(timer1_hw->intr & mask))
        hw_set_bits(&timer1_hw->intf, mask);
}

#endif
