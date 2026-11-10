/* hello_wilirfid: 125 kHz RFID example built on the WILiRFID library.
 *
 * Reads every supported 125 kHz card format, shows the card that was read,
 * and acts on it with the five coloured buttons along the bottom edge (this
 * app's own choice; your app may map any function to any button), each
 * labelled directly above it:
 *
 *   GREY    LOAD     load the next saved card from /rfid/saved on the SD card
 *                    and lock it as the selected card
 *   YELLOW  FORGET   unlock and forget the selected card (the same card then
 *                    reads again)
 *   GREEN   CLONE    first press: lock the selected card and prompt to remove
 *                    it and lay a blank. Second press: write it onto the
 *                    blank (a T5577 card or fob) laid flat on the back of the
 *                    FreeWili, and check it by reading it back. The card
 *                    stays locked; every further copy takes the same two
 *                    presses.
 *   BLUE    EMULATE  start or stop presenting the selected card to an
 *                    external reader, such as a door reader: hold the back
 *                    of the FreeWili to it (FORGET, and a LOAD that loads a
 *                    card, also stop it)
 *   RED     SAVE     save the selected card to /rfid/saved/<name>.rfid
 *
 * Cards are read through the antenna on the back of the FreeWili: lay them
 * flat against it. While unlocked, the selected card is the newest card read.
 * While locked, cards read (such as the blank laid down for a copy) are only
 * shown on a SEEN line and never replace the selected card. Saved cards
 * use the device's own record format, so cards saved here load on the device
 * and the other way round.
 *
 * The SD card belongs to the main CPU and is reached over the OneWili display
 * link. When the main CPU does not answer, SAVE and LOAD show NO SD LINK and
 * card reading carries on as normal; the next press opens the link again.
 * A card loaded from SD in a format other than EM4100, HID Prox or Indala is
 * for display only: CLONE and EMULATE show LOADED CARD: DISPLAY ONLY. Hold
 * HOME for five seconds to leave the app. */
#include "fw2.h"
#include "platform/diag.h"
#include "pico/stdlib.h"
#include "onewili.h"
#include "onewili_sd.h"
#include "input/app_recovery_onewili.h"
#include <stdio.h>
#include <string.h>

/* The LCD takes colours byte-swapped; write them in ordinary RGB565. */
static inline uint16_t be16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
#define C_BLACK be16(0x0000)
#define C_WHITE be16(0xFFFF)
#define C_GREEN be16(0x07E0)
#define C_RED   be16(0xF800)
#define C_YEL   be16(0xFFE0)
#define C_CYAN  be16(0x07FF)
#define C_GREY  be16(0x8410)
/* The colours of the five buttons. */
#define C_KEY_GREY   be16(0xD69A)
#define C_KEY_YELLOW be16(0xFF06)
#define C_KEY_GREEN  be16(0x1200)
#define C_KEY_BLUE   be16(0x00F8)
#define C_KEY_RED    be16(0x8007)

/* ~37 KB of link buffers: far too big for the stack. */
static ow_device dev;
static bool      link_up;

/* The selected card: clone, emulate and save act on it. While locked, new
 * reads go to last_seen instead. */
static WILiRFID_tag_t selected;
static bool           have_selected;
static bool           locked;
static bool           armed;   /* GREEN pressed once: the next GREEN writes a blank */
static WILiRFID_tag_t last_seen;
static bool           have_last_seen;

/* Result of the last button action, shown on its own line. */
static char     result_text[64];
static uint16_t result_color;
static bool     clone_running;   /* show the clone's progress on the result line */

/* Saved cards found by the last LOAD, and the file loaded last. */
#define MAX_SAVED   32
#define NAME_MAX_CH (WILiRFID_TAG_NAME_MAX + 8)   /* a stem plus ".rfid" */
static char saved_names[MAX_SAVED][NAME_MAX_CH];
static int  saved_count;
static char last_loaded[NAME_MAX_CH];

/* ── screen ─────────────────────────────────────────────────────────────── */
#define LINE_COUNT  8
#define LINE_SEEN   6
#define LINE_RESULT 7
static const int line_y[LINE_COUNT] = { 40, 62, 92, 114, 136, 158, 188, 218 };
static char      shown[LINE_COUNT][48];
static uint16_t  shown_color[LINE_COUNT];

/* Draw one scale-2 text line padded to the full width; skip it when unchanged. */
static void line_at(int idx, uint16_t fg, const char *text) {
    char padded[48];
    const int width = (ST7796_W - 16) / 12;   /* 38 characters */
    int n = (int)strlen(text);
    if (n > width) n = width;
    memcpy(padded, text, (size_t)n);
    memset(padded + n, ' ', (size_t)(width - n));
    padded[width] = 0;
    if (shown_color[idx] == fg && strcmp(shown[idx], padded) == 0) return;
    strcpy(shown[idx], padded);
    shown_color[idx] = fg;
    st7796_draw_text(8, line_y[idx], 2, fg, C_BLACK, padded);
}

/* One box directly above each button: the buttons are equally spaced along
 * the bottom edge, so the boxes are fixed fifths of the width. */
#define KEY_W     ((ST7796_W - 12) / 5)   /* 93 */
#define KEY_PITCH (KEY_W + 3)             /* 96 */
#define KEY_Y     276
#define KEY_H     40

static void key_label(int slot, uint16_t bg, uint16_t fg, const char *label) {
    int x = slot * KEY_PITCH;
    st7796_fill_rect(x, KEY_Y, KEY_W, KEY_H, bg);
    st7796_draw_text(x + (KEY_W - (int)strlen(label) * 12) / 2, KEY_Y + (KEY_H - 16) / 2,
                     2, fg, bg, label);
}

static void screen_static(void) {
    st7796_draw_text(8, 6, 3, C_WHITE, C_BLACK, "125 kHz RFID");
    st7796_draw_text(8, 256, 1, C_GREY, C_BLACK,
                     "LAY A CARD FLAT ON THE BACK.   HOLD HOME 5 S TO EXIT");
    key_label(UARTKBD_BTN_GREY,   C_KEY_GREY,   C_BLACK, "LOAD");
    key_label(UARTKBD_BTN_YELLOW, C_KEY_YELLOW, C_BLACK, "FORGET");
    key_label(UARTKBD_BTN_GREEN,  C_KEY_GREEN,  C_WHITE, "CLONE");
    key_label(UARTKBD_BTN_BLUE,   C_KEY_BLUE,   C_WHITE, "EMULATE");
    key_label(UARTKBD_BTN_RED,    C_KEY_RED,    C_WHITE, "SAVE");
}

/* Record the result of an action; fmt takes at most one %s. */
static void set_result(uint16_t color, const char *fmt, const char *arg) {
    snprintf(result_text, sizeof result_text, fmt, arg ? arg : "");
    result_color = color;
    clone_running = false;   /* a new message replaces the clone status */
    DIAG("hello_wilirfid: %s\n", result_text);
}

static const char *card_state_text(WILiRFID_card_state_t s) {
    switch (s) {
    case WILiRFID_CARD_PRESENT: return "PRESENT";
    case WILiRFID_CARD_REMOVED: return "REMOVED";
    default:                    return "NONE";
    }
}

static const char *write_status_text(WILiRFID_write_status_t s) {
    switch (s) {
    case WILiRFID_WRITE_VERIFYING: return "VERIFYING";
    case WILiRFID_WRITE_VERIFIED:  return "VERIFIED";
    case WILiRFID_WRITE_MISMATCH:  return "MISMATCH";
    case WILiRFID_WRITE_FAILED:    return "FAILED";
    default:                       return "IDLE";
    }
}

/* A card's id as hex after a prefix; "-" when it has none. */
static void id_text(const WILiRFID_tag_t *t, const char *prefix, char *s, size_t len) {
    int n = snprintf(s, len, "%s", prefix);
    if (t->id_len == 0) snprintf(s + n, len - (size_t)n, "-");
    for (unsigned i = 0; i < t->id_len && i < sizeof t->id; ++i)
        n += snprintf(s + n, len - (size_t)n, "%02X", t->id[i]);
}

static void screen_update(void) {
    static bool     was_starting;
    static uint32_t starting_since;   /* when the reader entered STARTING */
    char s[64];
    WILiRFID_state_t state = WILiRFID_state();
    if (state != WILiRFID_STATE_STARTING) {
        was_starting = false;
    } else if (!was_starting) {
        was_starting = true;
        starting_since = time_us_32();
    }
    /* Still STARTING after about two seconds: the board needs checking too. */
    bool check_board = state == WILiRFID_STATE_NO_SIGNAL ||
                       (state == WILiRFID_STATE_STARTING &&
                        time_us_32() - starting_since >= 2000000u);
    uint16_t state_color = state == WILiRFID_STATE_READY     ? C_GREEN
                         : state == WILiRFID_STATE_EMULATING ? C_CYAN
                         : state == WILiRFID_STATE_STARTING  ? C_YEL : C_RED;
    snprintf(s, sizeof s, "READER %s%s", WILiRFID_state_name(state),
             check_board ? "  CHECK RFID BOARD" : "");
    line_at(0, state_color, s);

    WILiRFID_card_state_t card = WILiRFID_card_state();
    snprintf(s, sizeof s, "CARD   %s", card_state_text(card));
    line_at(1, card == WILiRFID_CARD_PRESENT ? C_GREEN : C_WHITE, s);

    if (have_selected) {
        line_at(2, locked ? C_CYAN : C_WHITE, locked ? "SELECTED (LOCKED)" : "SELECTED");
        line_at(3, C_YEL, selected.format_name);
        if (selected.has_facility_card)
            snprintf(s, sizeof s, "FC %lu  CARD %lu", (unsigned long)selected.facility,
                     (unsigned long)selected.card_number);
        else
            s[0] = 0;
        line_at(4, C_WHITE, s);
        id_text(&selected, "ID ", s, sizeof s);
        line_at(5, C_WHITE, s);
    } else {
        line_at(2, C_GREY, "NO CARD SELECTED");
        line_at(3, C_GREY, "");
        line_at(4, C_GREY, "");
        line_at(5, C_GREY, "");
    }

    if (locked && have_last_seen) {
        int n = snprintf(s, sizeof s, "SEEN %s ", last_seen.format_name);
        id_text(&last_seen, "", s + n, sizeof s - (size_t)n);
        line_at(LINE_SEEN, C_GREY, s);
    } else {
        line_at(LINE_SEEN, C_GREY, "");
    }

    if (WILiRFID_emulating()) {
        WILiRFID_emu_status_t es = WILiRFID_emulate_status();
        snprintf(s, sizeof s, "EMULATING %s  %s", WILiRFID_format_name(es.format),
                 es.reader_field_detected ? "READER SEEN" : "NO READER");
        line_at(LINE_RESULT, C_CYAN, s);
    } else if (clone_running) {
        WILiRFID_write_status_t ws = WILiRFID_write_status();
        snprintf(s, sizeof s, "CLONE %s", write_status_text(ws));
        line_at(LINE_RESULT,
                ws == WILiRFID_WRITE_VERIFIED ? C_GREEN
                : (ws == WILiRFID_WRITE_MISMATCH || ws == WILiRFID_WRITE_FAILED) ? C_RED
                : C_YEL, s);
    } else {
        line_at(LINE_RESULT, result_color, result_text);
    }
}

/* ── SD card ────────────────────────────────────────────────────────────── */
/* Cards are not read while a SAVE or LOAD runs: about a second for each SD
 * call when the main CPU does not answer, so a SAVE can take a few seconds. */

/* Try again to reach the main CPU when the link is not yet up. Opening the
 * link does not reach the main CPU; the stat call does, so the link is up
 * only once the main CPU has answered it. Every SD call returns within about
 * a second even when the main CPU does not answer. */
static bool link_open(int attempts) {
    for (int i = 0; i < attempts && !link_up; i++) {
        if (fw2_app_recovery_open_onewili(&dev) == OW_OK &&
            fw2_app_recovery_wrap_sd() == OW_OK) {
            bool is_dir; uint32_t size;
            ow_sd_set_timeout_ms(1000);
            ow_status p = ow_sd_stat(&dev, "/", &is_dir, &size);
            link_up = (p != OW_ERR_TIMEOUT && p != OW_ERR_IO);   /* any answer means it is up */
            if (!link_up) fw2_app_recovery_sleep_ms(100);
        } else {
            fw2_app_recovery_sleep_ms(100);
        }
    }
    return link_up;
}

/* A timeout or transport error means the main CPU is not answering. The
 * link is then marked down, so the next SAVE or LOAD opens it again. */
static bool link_lost(ow_status s) {
    if (s != OW_ERR_TIMEOUT && s != OW_ERR_IO) return false;
    link_up = false;
    return true;
}

/* The main CPU answered that the SD card is missing or not mounted. */
static bool no_sd_card(ow_status s) {
    if (s != OW_ERR_FAILED) return false;
    sdfs_status_t e = ow_sd_last_error();
    return e == SDFS_ERR_NO_CARD || e == SDFS_ERR_NOT_MOUNTED;
}

static void save_selected(void) {
    char text[96], name[WILiRFID_TAG_NAME_MAX], path[96];
    if (!have_selected) { set_result(C_RED, "NO CARD SELECTED", NULL); return; }
    if (!WILiRFID_tag_to_string(&selected, text, sizeof text) ||
        !WILiRFID_tag_name(&selected, name, sizeof name)) {
        set_result(C_RED, "SAVE FAILED", NULL);
        return;
    }
    if (!link_open(1)) { set_result(C_RED, "NO SD LINK", NULL); return; }
    /* An existing folder returns an error; only a lost link (timeout) matters here. */
    ow_status s = ow_sd_mkdir(&dev, "/rfid");
    if (link_lost(s)) { set_result(C_RED, "NO SD LINK", NULL); return; }
    ow_sd_mkdir(&dev, WILiRFID_SAVED_DIR);
    snprintf(path, sizeof path, "%s/%s%s", WILiRFID_SAVED_DIR, name, WILiRFID_SAVED_EXT);
    s = ow_sd_put_mem(&dev, path, text, strlen(text), false);
    if (s == OW_OK)         set_result(C_GREEN, "SAVED %.24s", name);
    else if (link_lost(s))  set_result(C_RED, "NO SD LINK", NULL);
    else if (no_sd_card(s)) set_result(C_RED, "SAVE FAILED NO SD CARD", NULL);
    else {
        char code[12];   /* the ow_status value, as onewili.h numbers it */
        snprintf(code, sizeof code, "%d", (int)s);
        set_result(C_RED, "SAVE FAILED %s", code);
    }
}

static bool has_saved_ext(const char *name) {
    size_t n = strlen(name), e = strlen(WILiRFID_SAVED_EXT);
    if (n <= e) return false;
    for (size_t i = 0; i < e; i++) {
        char c = name[n - e + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != WILiRFID_SAVED_EXT[i]) return false;
    }
    return true;
}

static void collect_saved(const char *name, bool is_dir, uint32_t size, void *user) {
    (void)size; (void)user;
    if (is_dir || !has_saved_ext(name) || saved_count >= MAX_SAVED) return;
    if (strlen(name) >= NAME_MAX_CH) return;
    strcpy(saved_names[saved_count++], name);
}

static void load_next(void) {
    if (!link_open(1)) { set_result(C_RED, "NO SD LINK", NULL); return; }
    saved_count = 0;
    ow_status s = ow_sd_list(&dev, WILiRFID_SAVED_DIR, collect_saved, NULL);
    if (link_lost(s)) { set_result(C_RED, "NO SD LINK", NULL); return; }
    /* A missing folder only means nothing has been saved yet. */
    if (s == OW_ERR_FAILED && ow_sd_last_error() == SDFS_ERR_NOT_FOUND) s = OW_OK;
    if (no_sd_card(s)) { set_result(C_RED, "NO SD CARD", NULL); return; }
    if (s != OW_OK)    { set_result(C_RED, "SD ERROR", NULL); return; }
    if (saved_count == 0) { set_result(C_YEL, "NO SAVED CARDS", NULL); return; }

    /* Each press loads the next file in alphabetical order, wrapping round. */
    int pick = -1, first = 0;
    for (int i = 0; i < saved_count; i++) {
        if (strcmp(saved_names[i], saved_names[first]) < 0) first = i;
        if (strcmp(saved_names[i], last_loaded) > 0 &&
            (pick < 0 || strcmp(saved_names[i], saved_names[pick]) < 0))
            pick = i;
    }
    if (pick < 0) pick = first;

    char path[96], text[129], stem[NAME_MAX_CH];
    size_t got = 0;
    snprintf(path, sizeof path, "%s/%s", WILiRFID_SAVED_DIR, saved_names[pick]);
    strcpy(last_loaded, saved_names[pick]);
    strcpy(stem, saved_names[pick]);
    stem[strlen(stem) - strlen(WILiRFID_SAVED_EXT)] = 0;

    s = ow_sd_get_mem(&dev, path, text, sizeof text - 1, &got);   /* at most 128 bytes */
    if (link_lost(s)) { set_result(C_RED, "NO SD LINK", NULL); return; }
    if (s != OW_OK) { set_result(C_RED, "LOAD FAILED %.24s", stem); return; }
    text[got] = 0;

    WILiRFID_tag_t tag;
    if (!WILiRFID_tag_from_string(text, &tag)) {
        set_result(C_RED, "NOT A SAVED CARD %.21s", stem);
        return;
    }
    /* Never keep presenting a card that is no longer selected, nor go on
     * checking a copy of it: its result could no longer be shown. */
    if (WILiRFID_emulating()) WILiRFID_emulate_stop();
    if (WILiRFID_write_status() == WILiRFID_WRITE_VERIFYING) WILiRFID_write_cancel();
    selected = tag;
    have_selected = true;
    locked = true;          /* a loaded card stays selected until FORGET */
    armed = false;          /* CLONE still prompts for a blank first */
    have_last_seen = false;
    set_result(C_GREEN, "LOADED %.24s", stem);
}

/* ── buttons ────────────────────────────────────────────────────────────── */
/* A card loaded from a saved record has no frame (raw_bits 0). Only EM4100,
 * HID Prox and Indala can be rebuilt from the record; any other format loaded
 * that way is for display only. */
static bool display_only(const WILiRFID_tag_t *t) {
    return t->raw_bits == 0 &&
           t->format != WILiRFID_FORMAT_EM4100 &&
           t->format != WILiRFID_FORMAT_HID_PROX &&
           t->format != WILiRFID_FORMAT_INDALA;
}

static void clone_selected(void) {
    if (WILiRFID_emulating()) { set_result(C_RED, "STOP EMULATION FIRST", NULL); return; }
    /* A copy still being checked: let it finish, and keep showing its status. */
    if (WILiRFID_write_status() == WILiRFID_WRITE_VERIFYING) {
        set_result(C_YEL, "CLONE STILL CHECKING", NULL);
        clone_running = true;
        return;
    }
    if (!have_selected) { set_result(C_RED, "NO CARD SELECTED", NULL); return; }
    if (display_only(&selected)) {
        armed = false;
        set_result(C_RED, "LOADED CARD: DISPLAY ONLY", NULL);
        return;
    }
    if (!WILiRFID_format_can_write(selected.format)) {
        set_result(C_RED, "CLONE %s", WILiRFID_result_name(WILiRFID_UNSUPPORTED_FORMAT));
        return;
    }
    if (!armed) {
        /* Before every write: lock the card, so the blank laid down next
         * cannot replace it, and prompt to swap the card for a blank. */
        locked = true;
        armed = true;
        have_last_seen = false;
        set_result(C_YEL, "REMOVE CARD, BLANK ON BACK, THEN CLONE", NULL);
        return;
    }
    armed = false;
    /* Sending the write takes up to about two seconds; say so first. */
    line_at(LINE_RESULT, C_YEL, "CLONE WRITING  HOLD THE BLANK STILL");
    WILiRFID_result_t r = WILiRFID_write_begin(&selected, NULL);
    if (r == WILiRFID_OK) clone_running = true;   /* follow the status on screen */
    else set_result(C_RED, "CLONE %s", WILiRFID_result_name(r));
}

static void toggle_emulation(void) {
    if (WILiRFID_emulating()) {
        WILiRFID_emulate_stop();
        set_result(C_WHITE, "EMULATION STOPPED", NULL);
        return;
    }
    if (!have_selected) { set_result(C_RED, "NO CARD SELECTED", NULL); return; }
    if (display_only(&selected)) {
        set_result(C_RED, "LOADED CARD: DISPLAY ONLY", NULL);
        return;
    }
    if (!WILiRFID_format_can_emulate(selected.format)) {
        set_result(C_RED, "EMULATE %s", WILiRFID_result_name(WILiRFID_UNSUPPORTED_FORMAT));
        return;
    }
    WILiRFID_result_t r = WILiRFID_emulate_begin(&selected);
    if (r == WILiRFID_OK) {
        armed = false;   /* after emulation, CLONE prompts for a blank again */
        set_result(C_CYAN, "EMULATING", NULL);
    }
    else set_result(C_RED, "EMULATE %s", WILiRFID_result_name(r));
}

static void forget_selected(void) {
    /* Never keep presenting a card that is no longer selected, nor go on
     * checking a copy of it: its result could no longer be shown. */
    if (WILiRFID_emulating()) WILiRFID_emulate_stop();
    if (WILiRFID_write_status() == WILiRFID_WRITE_VERIFYING) WILiRFID_write_cancel();
    WILiRFID_forget();
    memset(&selected, 0, sizeof selected);
    have_selected = false;
    locked = false;
    armed = false;
    have_last_seen = false;
    set_result(C_WHITE, "FORGOTTEN", NULL);
}

static void buttons_poll(void) {
    uartkbd_event_t ev;
    while (uartkbd_next_event(&ev)) {
        if (!ev.pressed) continue;
        switch (ev.btn) {
        case UARTKBD_BTN_GREY:   load_next();        break;
        case UARTKBD_BTN_YELLOW: forget_selected();  break;
        case UARTKBD_BTN_GREEN:  clone_selected();   break;
        case UARTKBD_BTN_BLUE:   toggle_emulation(); break;
        case UARTKBD_BTN_RED:    save_selected();    break;
        default: break;
        }
    }
}

int main(void) {
    board_init();
    fw2_app_recovery_init();   /* also powers the POWER_ZONES zones */
    st7796_init();
    fw2_app_about_use_lcd();
    st7796_fill_screen(C_BLACK);    /* the whole screen, before the backlight */
    screen_static();
    board_backlight_set(1);
    DIAG("\n=== hello_wilirfid (WILiRFID %s) ===\n", WILiRFID_version());

    if (!WILiRFID_start()) {
        char s[64];
        snprintf(s, sizeof s, "START FAILED %s",
                 WILiRFID_result_name(WILiRFID_last_error()));
        DIAG("hello_wilirfid: reader %s\n", s);
        line_at(0, C_RED, s);
        while (true) {
            fw2_app_recovery_task();
            picpwr_task();
            fw2_app_recovery_sleep_ms(10);
        }
    }

    /* The SD link: a few attempts only, so start-up never waits long for it. */
    line_at(LINE_RESULT, C_YEL, "CONNECTING TO SD CARD...");
    if (link_open(3)) set_result(C_WHITE, "LAY A CARD ON THE BACK", NULL);
    else set_result(C_RED, "NO SD LINK", NULL);

    uint32_t next_screen = time_us_32();
    bool     reader_down = false;   /* restart after a fault failed; tries again */
    uint32_t next_start  = 0;
    for (;;) {
        fw2_app_recovery_task();
        picpwr_task();
        WILiRFID_service();

        WILiRFID_tag_t tag;
        if (WILiRFID_poll(&tag)) {
            if (locked) {
                last_seen = tag;
                have_last_seen = true;
            } else {
                selected = tag;
                have_selected = true;
            }
            DIAG("hello_wilirfid: read %s\n", tag.format_name);
        }

        /* An internal error was reported; stop, then start the reader again
         * every two seconds until it starts. */
        if (WILiRFID_state() == WILiRFID_STATE_FAULT) {
            WILiRFID_stop();
            reader_down = true;
            next_start = time_us_32();
        }
        if (reader_down && (int32_t)(time_us_32() - next_start) >= 0) {
            next_start = time_us_32() + 2000000;
            if (WILiRFID_start()) {
                reader_down = false;
                set_result(C_WHITE, "READER STARTED AGAIN", NULL);
            } else {
                set_result(C_RED, "START FAILED %s",
                           WILiRFID_result_name(WILiRFID_last_error()));
            }
        }

        buttons_poll();

        if ((int32_t)(time_us_32() - next_screen) >= 0) {
            next_screen = time_us_32() + 100000;
            screen_update();
        }
    }
}
