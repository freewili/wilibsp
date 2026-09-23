// bsp/platform/power.h — power diagnostics for a standalone app.
//
// Rail control lives in input/picpwr.h (docs/drivers/power.md); this is the
// measuring side of the same work.
#ifndef BOARD_POWER_H
#define BOARD_POWER_H

// RP2350 die temperature, degrees C (on-chip sensor, ADC channel 8 on the
// RP2350B). Enables the ADC and the sensor on first use. Typical accuracy is a
// few degrees; good for before/after comparisons, not for absolute readings.
// It follows the whole board's temperature, not just the core's (sleeping the
// core 97% of the time moved it ~1 C), and it has minutes of thermal lag:
// compare readings at equilibrium, not right after a change. Leaves the temperature input selected: an app that uses the ADC itself must
// re-select its own input afterwards.
float board_die_temp_c(void);

#endif
