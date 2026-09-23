#include "fw2.h"

int main(void) {
    board_init();
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd();
    st7796_fill_screen(0x0000);
    board_backlight_set(1);
    st7796_draw_text(8, 8, 2, 0xFFFF, 0x0000, "HELLO FREEWILI2");

    /* Power: request the rails this app's peripherals need with
     * picpwr_keep_awake() here, then release the inherited ones it does not
     * use (the audio codec rail above all). docs/drivers/power.md. */
    picpwr_release_unused();

    for (;;) {
        fw2_app_recovery_task();
        picpwr_task();
        tight_loop_contents();
    }
}
