// bsp/rfid/rfid_carrier.c — 125 kHz carrier control.
#include "rfid/rfid_carrier.h"
#include "platform/board.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

#define CARRIER_HZ  125000
#define PWM_WRAP    255
#define PWM_LEVEL   128

static unsigned s_slice;
static unsigned s_actual_hz;
static bool     s_running;

void rfid_carrier_start(void) {
    if (s_running) return;

    gpio_set_function(PIN_RFID_AUX, GPIO_FUNC_SIO);
    gpio_set_dir(PIN_RFID_AUX, GPIO_OUT);
    gpio_put(PIN_RFID_AUX, 0);

    gpio_set_function(PIN_RFID_PULL, GPIO_FUNC_SIO);
    gpio_set_dir(PIN_RFID_PULL, GPIO_OUT);
    gpio_put(PIN_RFID_PULL, 0);

    gpio_set_function(PIN_RFID_CARRIER, GPIO_FUNC_PWM);
    s_slice = pwm_gpio_to_slice_num(PIN_RFID_CARRIER);

    const uint32_t sys_hz = clock_get_hz(clk_sys);
    const float divider = (float)sys_hz / ((float)CARRIER_HZ * (float)(PWM_WRAP + 1));

    pwm_set_clkdiv(s_slice, divider);
    pwm_set_wrap(s_slice, PWM_WRAP);
    pwm_set_gpio_level(PIN_RFID_CARRIER, PWM_LEVEL);
    pwm_set_enabled(s_slice, true);

    s_actual_hz = (unsigned)((float)sys_hz / (divider * (float)(PWM_WRAP + 1)));
    s_running = true;
}

void rfid_carrier_gate(bool on) {
    if (!s_running) return;
    pwm_set_enabled(s_slice, on);
}

void rfid_carrier_stop(void) {
    if (!s_running) return;
    pwm_set_enabled(s_slice, false);
    gpio_set_function(PIN_RFID_CARRIER, GPIO_FUNC_SIO);
    gpio_set_dir(PIN_RFID_CARRIER, GPIO_OUT);
    gpio_put(PIN_RFID_CARRIER, 0);
    s_running = false;
    s_actual_hz = 0;
}

bool     rfid_carrier_running(void) { return s_running; }
unsigned rfid_carrier_hz(void)      { return s_actual_hz; }
