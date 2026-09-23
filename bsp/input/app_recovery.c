#include "input/app_recovery.h"

#ifndef HOST_TEST
#include "hardware/watchdog.h"
#include "pico/time.h"
#include "input/uartkbd.h"
#include "input/app_about.h"
#include "input/picpwr.h"
#include "platform/diag.h"
#endif

void fw2_app_recovery_state_init(fw2_app_recovery_state_t *state) {
    state->pressed_at_ms = 0;
    state->tracking = false;
    state->fired = false;
}

bool fw2_app_recovery_state_update(fw2_app_recovery_state_t *state,
                                   bool home_down, uint32_t now_ms) {
    if (!home_down) {
        state->tracking = false;
        state->fired = false;
        return false;
    }
    if (!state->tracking) {
        state->pressed_at_ms = now_ms;
        state->tracking = true;
        return false;
    }
    if (!state->fired &&
        (uint32_t)(now_ms - state->pressed_at_ms) >= FW2_APP_RECOVERY_HOLD_MS) {
        state->fired = true;
        return true;
    }
    return false;
}

#ifndef HOST_TEST
static fw2_app_recovery_state_t recovery;
extern const uint32_t fw2app_power_zones;
#define FW2_APP_RECOVERY_FRAME_MAX_AGE_MS 1100u

void fw2_app_recovery_init(void) {
    uartkbd_init();
    fw2_app_recovery_state_init(&recovery);
    if (fw2app_power_zones != 0u) {
        picpwr_keep_awake(fw2app_power_zones);
        DIAG("app: required power zones=0x%x\n", fw2app_power_zones);
        uint32_t rails = 0;
        uint32_t waited_ms = 0;
        while (!picpwr_rails(&rails) ||
               (rails & fw2app_power_zones) != fw2app_power_zones) {
            fw2_app_recovery_task();
            busy_wait_us_32(10000u);
            waited_ms += 10u;
            if (waited_ms % 5000u == 0u)
                DIAG("app: waiting for power zones have=0x%x need=0x%x\n",
                     rails, fw2app_power_zones);
        }
        DIAG("app: power zones ready=0x%x\n", rails);
    }
}

void fw2_app_recovery_task(void) {
    uartkbd_task();
    picpwr_task();
    fw2_app_about_task();
    const bool home = uartkbd_button_down_fresh(
        UARTKBD_BTN_HOME, FW2_APP_RECOVERY_FRAME_MAX_AGE_MS);
    if (fw2_app_recovery_state_update(&recovery, home,
                                      (uint32_t)(time_us_64() / 1000u))) {
        DIAG("app: HOME held, rebooting to recovery loader\n");
        watchdog_reboot(0, 0, 0);
        while (true) tight_loop_contents();
    }
}

void fw2_app_recovery_sleep_ms(uint32_t duration_ms) {
    while (duration_ms != 0) {
        fw2_app_recovery_task();
        const uint32_t slice_ms = duration_ms < 10u ? duration_ms : 10u;
        busy_wait_us_32(slice_ms * 1000u);
        duration_ms -= slice_ms;
    }
    fw2_app_recovery_task();
}
#endif
