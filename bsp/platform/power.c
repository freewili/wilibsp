// bsp/platform/power.c — see power.h.
#include "platform/power.h"
#include "display/tvout.h"
#include "hardware/adc.h"
#include "pico/stdlib.h"
#include <math.h>

float board_die_temp_c(void) {
    // clk_adc is stopped while the TV picture runs on a clk_sys with no whole
    // 48 MHz divide; a conversion would never complete.
    if (!tvout_adc_available()) return NAN;
    static bool ready;
    if (!ready) {
        adc_init();
        adc_set_temp_sensor_enabled(true);
        ready = true;
    }
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    // Average a few conversions: single readings jitter by ~0.5 C.
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += adc_read();
    float v = (float)sum / 16.0f * 3.3f / 4096.0f;
    return 27.0f - (v - 0.706f) / 0.001721f;
}
