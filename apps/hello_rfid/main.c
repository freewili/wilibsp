// hello_rfid — RFID reader smoke test.
#include "fw2.h"
#include "platform/diag.h"
#include "pico/stdlib.h"
#include <stdio.h>

int main(void) {
    board_init();
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd();
    st7796_fill_screen(0x0000);
    board_backlight_set(1);
    st7796_draw_text(8, 8, 2, 0xFFFF, 0x0000, "RFID EM4100 READER");
    DIAG("\n=== hello_rfid ===\n");

    picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_NFC_RFID));

    {
        absolute_time_t give_up   = make_timeout_time_ms(10000);
        absolute_time_t no_frames = make_timeout_time_ms(4000);
        uint32_t rails;
        while (!time_reached(give_up)) {
            fw2_app_recovery_task();
            picpwr_task();
            if (picpwr_rails(&rails)) {
                if (rails & picpwr_zone_bit(PICPWR_ZONE_NFC_RFID)) break;
            } else if (time_reached(no_frames)) {
                break;
            }
            fw2_app_recovery_sleep_ms(25);
        }
    }

    if (!rfid_reader_start()) {
        DIAG("rfid: reader start FAILED\n");
        st7796_draw_text(8, 40, 1, 0xF800, 0x0000, "READER START FAILED");
        while (1) { fw2_app_recovery_task(); picpwr_task(); tight_loop_contents(); }
    }

    char info[80];
    snprintf(info, sizeof(info), "CARRIER %u Hz  STATE %d",
             rfid_reader_carrier_hz(), rfid_reader_state());
    st7796_draw_text(8, 40, 1, 0x07E0, 0x0000, info);
    DIAG("rfid: reader up (%s)\n", info);
    st7796_draw_text(8, 56, 1, 0xFFFF, 0x0000, "PRESENT AN EM4100 TAG...");

    rfid_tag_t tag;
    char line[80];
    int tags = 0;

    while (true) {
        fw2_app_recovery_task();
        picpwr_task();
        rfid_reader_service();

        if (rfid_reader_poll(&tag)) {
            int n = snprintf(line, sizeof(line), "TAG #%d  EM4100  ID:", ++tags);
            for (unsigned i = 0; i < tag.id_len && n < (int)sizeof(line) - 3; ++i)
                n += snprintf(line + n, sizeof(line) - n, "%02X", tag.id[i]);
            DIAG("%s\n", line);
            st7796_fill_rect(8, 80, 464, 16, 0x0000);
            st7796_draw_text(8, 80, 2, 0xFFE0, 0x0000, line);
        }

        snprintf(info, sizeof(info), "CARRIER %u Hz  STATE %d",
                 rfid_reader_carrier_hz(), rfid_reader_state());
        st7796_fill_rect(8, 40, 464, 12, 0x0000);
        st7796_draw_text(8, 40, 1, 0x07E0, 0x0000, info);

        fw2_app_recovery_sleep_ms(2);
    }
}
