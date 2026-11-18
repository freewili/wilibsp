/* hello_wilinfc - 13.56 MHz NFC example for the FreeWili 2 display CPU.
 *
 * Shows the reader state and the last card seen (type, UID, ATQA/SAK), and can
 * present that card's identity to another reader. Hold the card flat against
 * the back of the FreeWili, where the antenna is. Buttons (this app's own
 * choice; your app may map any function to any button), each labelled
 * directly above it along the bottom:
 *
 *   YELLOW  SAVE     write the last card to /appdata/hello_wilinfc/ as a
 *                    Flipper .nfc file, through the ow_sd_io file-io adapter
 *   GREEN   READ     forget the remembered card and read the one in the field
 *   BLUE    EMULATE  start or stop presenting the last card's UID/ATQA/SAK
 *
 * Grey and red do nothing here and are left unlabelled. Hold PAGE for five
 * seconds for the About screen (version and repository link).
 *
 * The SD card is owned by the main CPU, which must run the stock FreeWili 2
 * firmware; without it the app still reads cards and SAVE is disabled. */
#include "fw2.h"
#include "platform/diag.h"
#include "pico/stdlib.h"
#include "onewili.h"
#include "onewili_fwgui.h"
#include "onewili_sd.h"
#include "input/app_recovery_onewili.h"
#include "ow_sd_io.h"
#include <stdio.h>
#include <string.h>

static inline uint16_t be16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
#define C_BLACK be16(0x0000)
#define C_WHITE be16(0xFFFF)
#define C_GREEN be16(0x07E0)
#define C_RED   be16(0xF800)
#define C_YEL   be16(0xFFE0)
#define C_CYAN  be16(0x07FF)
#define C_GREY  be16(0x8410)
/* The colours of the front-panel buttons; the LCD takes byte-swapped colours. */
#define C_KEY_YELLOW be16(0xFF06)
#define C_KEY_GREEN  be16(0x1200)
#define C_KEY_BLUE   be16(0x00F8)

/* ~37 KB of link buffers: far too big for the stack. */
static ow_device dev;
static bool sd_ok;

static char message[40] = "";   /* result of the last button action */

/* One box directly above each front-panel button: fixed fifths of the width. */
#define KEY_W     ((ST7796_W - 12) / 5)   /* 93 */
#define KEY_PITCH (KEY_W + 3)             /* 96 */
#define KEY_Y     276
#define KEY_H     40

static void key_label(int slot, uint16_t bg, uint16_t fg, const char *label)
{
    int x = slot * KEY_PITCH;
    st7796_fill_rect(x, KEY_Y, KEY_W, KEY_H, bg);
    st7796_draw_text(x + (KEY_W - (int)strlen(label) * 12) / 2, KEY_Y + (KEY_H - 16) / 2,
                     2, fg, bg, label);
}

/* Draw a line padded to the full width so it overwrites what was there. */
static void row(int y, int scale, uint16_t fg, const char *text)
{
    char padded[81];
    int cols = ST7796_W / (6 * scale);
    if (cols > 80) cols = 80;
    snprintf(padded, sizeof padded, "%-*.*s", cols, cols, text);
    st7796_draw_text(0, y, scale, fg, C_BLACK, padded);
}

static const char *type_name(WILiNFC_card_type_t t)
{
    switch (t) {
    case WILiNFC_CARD_TYPE_T1T:            return "NFC FORUM TYPE 1";
    case WILiNFC_CARD_TYPE_T2T:            return "NFC FORUM TYPE 2";
    case WILiNFC_CARD_TYPE_T4T:            return "NFC FORUM TYPE 4 (ISO-DEP)";
    case WILiNFC_CARD_TYPE_NFC_DEP:        return "NFC-DEP PEER";
    case WILiNFC_CARD_TYPE_MIFARE_CLASSIC: return "MIFARE CLASSIC";
    default:                               return "UNKNOWN";
    }
}

static void draw_static(void)
{
    st7796_draw_text(8, 6, 3, C_WHITE, C_BLACK, "13.56 MHz NFC");
    row(256, 1, C_GREY, "HOLD A CARD FLAT ON THE BACK.   HOLD PAGE 5 S FOR ABOUT");
    key_label(1, C_KEY_YELLOW, C_BLACK, "SAVE");
    key_label(2, C_KEY_GREEN, C_WHITE, "READ");
    key_label(3, C_KEY_BLUE, C_WHITE, "EMULATE");
}

static void draw_status(void)
{
    char s[64];
    WILiNFC_state_t st = WILiNFC_state();
    uint16_t col = (st == WILiNFC_STATE_READY || st == WILiNFC_STATE_EMULATING) ? C_GREEN
                 : st == WILiNFC_STATE_NO_SIGNAL ? C_YEL : C_RED;
    snprintf(s, sizeof s, "STATE %s", WILiNFC_state_name(st));
    row(36, 2, col, s);

    if (st == WILiNFC_STATE_FAULT ||
        (st == WILiNFC_STATE_OFF && WILiNFC_last_error() != WILiNFC_OK)) {
        WILiNFC_result_t e = WILiNFC_last_error();
        row(60, 2, C_RED, e == WILiNFC_NO_FRONT_END ? "NO NFC FRONT END FOUND"
                        : e == WILiNFC_INIT_TIMEOUT ? "NFC START TIMED OUT"
                        : WILiNFC_result_name(e));
        row(84, 1, C_GREY, e == WILiNFC_NO_FRONT_END ? "NOTHING ANSWERED ON I2C1"
                         : e == WILiNFC_INIT_TIMEOUT ? "IT ANSWERED BUT NEVER BECAME READY"
                         : "");
    } else {
        static const char *cs[] = { "NO CARD YET", "CARD PRESENT", "CARD REMOVED" };
        int c = (int)WILiNFC_card_state();
        row(60, 2, c == WILiNFC_CARD_PRESENT ? C_GREEN : C_WHITE, cs[c >= 0 && c <= 2 ? c : 0]);
        row(84, 1, C_GREY, "");
    }

    WILiNFC_card_t card;
    if (WILiNFC_last_card(&card)) {
        char hex[24];
        int n = 0;
        for (unsigned i = 0; i < card.uid_len && i < 10; ++i)
            n += snprintf(hex + n, sizeof hex - n, "%02X", card.uid[i]);
        row(104, 2, C_WHITE, type_name(card.type));
        snprintf(s, sizeof s, "UID %s", hex);
        row(128, 2, C_YEL, s);
        snprintf(s, sizeof s, "ATQA %02X%02X  SAK %02X  UID %uB", card.atqa[0], card.atqa[1],
                 card.sak, card.uid_len);
        row(152, 2, C_CYAN, s);
        row(176, 1, C_GREY, card.is_iso_dep ? "ISO-DEP YES" : "ISO-DEP NO");
    } else {
        row(104, 2, C_GREY, "HOLD A CARD FLAT ON THE BACK");
        row(128, 2, C_GREY, "");
        row(152, 2, C_GREY, "");
        row(176, 1, C_GREY, "");
    }

    WILiNFC_emu_status_t emu;
    if (WILiNFC_emulate_status(&emu) == WILiNFC_OK && emu.active)
        row(196, 2, C_GREEN, emu.reader_engaged ? "EMULATING  READER ENGAGED"
                                                : "EMULATING  NO READER");
    else
        row(196, 2, C_GREY, "EMULATION OFF");
    row(224, 2, C_WHITE, message);
}

static void say(const char *what, WILiNFC_result_t r)
{
    snprintf(message, sizeof message, "%s %s", what, WILiNFC_result_name(r));
    DIAG("hello_wilinfc: %s\n", message);
}

static void action_save(void)
{
    WILiNFC_card_t card;
    if (!sd_ok) { snprintf(message, sizeof message, "NO SD: SAVE DISABLED"); return; }
    if (!WILiNFC_last_card(&card)) { snprintf(message, sizeof message, "NO CARD TO SAVE"); return; }
    char path[64];
    int n = snprintf(path, sizeof path, OW_SD_IO_DIR "/");
    for (unsigned i = 0; i < card.uid_len && i < 10; ++i)
        n += snprintf(path + n, sizeof path - n, "%02X", card.uid[i]);
    snprintf(path + n, sizeof path - n, ".nfc");
    say("SAVE", WILiNFC_card_save(path, &card));
}

static void action_emulate(void)
{
    if (WILiNFC_emulating()) {
        WILiNFC_emulate_stop();
        snprintf(message, sizeof message, "EMULATION STOPPED");
        return;
    }
    WILiNFC_card_t card;
    if (!WILiNFC_last_card(&card)) {
        snprintf(message, sizeof message, "NO CARD TO EMULATE");
        return;
    }
    say("EMULATE", WILiNFC_emulate_card(&card));
}

static void action_read(void)
{
    if (WILiNFC_emulating()) { snprintf(message, sizeof message, "STOP EMULATION FIRST"); return; }
    WILiNFC_forget();   /* a card still in the field is reported again */
    snprintf(message, sizeof message, "HOLD A CARD FLAT ON THE BACK");
}

static void buttons_poll(void)
{
    uartkbd_event_t ev;
    while (uartkbd_next_event(&ev)) {
        if (!ev.pressed) continue;
        switch (ev.btn) {
        case UARTKBD_BTN_YELLOW: action_save(); break;
        case UARTKBD_BTN_GREEN:  action_read(); break;
        case UARTKBD_BTN_BLUE:   action_emulate(); break;
        default: break;
        }
    }
}

int main(void)
{
    board_init();
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd();            /* hold PAGE 5 s: version + repository */
    st7796_fill_screen(C_BLACK);        /* the whole surface, before the backlight */
    draw_static();
    board_backlight_set(1);
    DIAG("\n=== hello_wilinfc ===\n");

    /* The NFC front end needs no power call here: fw2_app_recovery_init()
     * keeps the zones listed in POWER_ZONES (NFC_RFID and SDCARD among them)
     * awake and waited until they were on. Switch off the inherited rails
     * this app does not use. */
    picpwr_release_unused();

    /* SD link to the main CPU, for saved cards. Bounded: the reader works
     * without it. */
    for (int i = 0; i < 3 && !sd_ok; i++) {
        if (fw2_app_recovery_open_onewili(&dev) == OW_OK
            && fw2_app_recovery_wrap_sd() == OW_OK) {   /* recovery-aware SD transport */
            /* From here on an SD call returns within about a second even if
             * the main CPU does not answer; cards are not read while one is
             * in progress. */
            ow_sd_set_timeout_ms(1000);
            ow_sd_io_init(&dev);
            sd_ok = ow_sd_io_ensure_dir();
            if (sd_ok) WILiNFC_set_file_io(ow_sd_io_get());
        }
        fw2_app_recovery_task();
    }
    DIAG("hello_wilinfc: sd %s\n", sd_ok ? "ok" : "unavailable");

    bool started = WILiNFC_start();
    DIAG("hello_wilinfc: start %d (%s)\n", (int)started, WILiNFC_result_name(WILiNFC_last_error()));
    snprintf(message, sizeof message, sd_ok ? "SD READY" : "NO SD: SAVE DISABLED");

    uint32_t next_draw = 0;
    while (true) {
        fw2_app_recovery_task();
        picpwr_task();
        WILiNFC_service();

        WILiNFC_card_t fresh;
        if (WILiNFC_poll(&fresh)) {
            snprintf(message, sizeof message, "CARD READ");
            DIAG("hello_wilinfc: card type %d uid_len %u sak %02X\n", (int)fresh.type,
                 fresh.uid_len, fresh.sak);
        }
        buttons_poll();

        uint32_t now = time_us_32();
        if ((int32_t)(now - next_draw) >= 0) {
            next_draw = now + 200000;
            draw_status();
        }
        fw2_app_recovery_sleep_ms(2);
    }
}
