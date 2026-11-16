// Emberward — guard the core: an original arcade game for the TV output, with
// the LCD as a companion screen and the TV remote (CEC) as a controller.
// Rules and design: DESIGN.md. How to play: README.md.
//
// Frame loop, once per TV frame: wait until the TV has left the picture rows
// (tvout_show() of the one buffer) -> compose the 400x300 picture top to
// bottom ahead of the scan (ew_render.h) -> input (front panel, nav pad,
// touch, TV remote) -> fixed 60 Hz game ticks -> sound -> LCD companion.
// TV sound runs beside the loop, from a 1 ms timer interrupt (see "sound").
#include "fw2.h"
#include "platform/diag.h"
#include "pico/stdlib.h"
#include "onewili.h"
#include "onewili_fwgui.h"
#include "onewili_sd.h"
#include "input/app_recovery_onewili.h"
#include "cec/cec_saved_onewili.h"

#include <stdio.h>
#include <string.h>

#include "ew_game.h"
#include "ew_input.h"
#include "ew_lcd.h"
#include "ew_mix.h"
#include "ew_render.h"
#include "ew_scores.h"
#include "ew_sfx.h"

#define TICK_US        16667u
#define MAX_CATCHUP    4
#define SAVE_DIR_ROOT  "/appdata"
#define SAVE_DIR       "/appdata/emberward"
#define SAVE_PATH      SAVE_DIR "/scores.bin"
#define TV_INPUT_MAX   4   // the TV inputs BLUE steps through on the title screen

// The TV picture, in SRAM, read in place by the TV (240,000 bytes).
static uint16_t s_fb[EW_FB_W * EW_FB_H];

// OneWili link for the SD card (~37 KB of link buffers).
static ow_device s_dev;
static bool s_sd_ok;

static ew_game_t s_game;
static ew_input_t s_in;
static ew_save_t s_save;
static uint32_t s_save_gen;
static ew_entry_ui_t s_entry;
static ew_screen_t s_screen = EW_SCREEN_TITLE;
static bool s_lcd_view;
static uint32_t s_frame;
static bool s_cec_ok;
static bool s_tv_seen;

// ------------------------------------------------------------------ sound
//
// Two outputs, one at a time. With a TV connected and TV sound on, the picture
// runs with audio framing and the TV plays the effects, mixed at 48 kHz
// (ew_mix.h); the speaker is powered down. Otherwise the speaker plays them,
// one note at a time. A repeating timer (the SDK's default alarm pool, as
// bsp/input/app_recovery_onewili.h:53 uses) tops up the TV feeder's
// 2048-frame buffer (42.7 ms) every millisecond in interrupt context, so the
// loop only posts effects to the mixer's queue and no tvout_show() or LCD
// flush wait leaves the TV short.

// Speaker: each note is one 512-frame loop (32 ms at 16 kHz), a whole number of
// cycles; two buffers so the next note is rendered off the playing one.
#define SND_FRAMES 512
#define SND_FS     16009u               // the codec's real rate (docs/drivers/audio.md)
#define SND_AMP    5000                 // quiet: the speaker is rated 0.5 W
static uint32_t s_snd[2][SND_FRAMES] __attribute__((aligned(SND_FRAMES * 4)));
static int s_snd_cur;
static bool s_audio_ok, s_snd_playing;
static uint32_t s_noise = 0xACE1u;
static ew_sfx_t s_sfx;

#define TV_AMP     6000                 // per voice; the sum is clipped at full scale
#define TV_FEED_US 1000                 // top-up period, well inside the feeder's 42.7 ms
static ew_mix_t s_mix;
static struct repeating_timer s_tv_timer;
static bool s_tv_timer_on;
static bool s_tv_sound_on;              // the TV carries the sound now
static int8_t s_speaker = -1;           // speaker output stage: -1 not set yet, 0 off, 1 on

// Timer callback, interrupt context: the only caller of tvout_audio_write().
static bool tv_feed(struct repeating_timer *t) {
    (void)t;
    ew_mix_feed(&s_mix, tvout_audio_write);
    return true;
}

static void snd_silence(void) {
    if (s_audio_ok && s_snd_playing) audio_i2s_duplex_play_stop();
    s_snd_playing = false;
}

static void snd_note(const ew_note_t *n) {
    if (!s_audio_ok || !s_save.sound || s_tv_sound_on || !n->hz) {
        snd_silence();
        return;
    }
    const int next = s_snd_cur ^ 1;
    ew_sfx_render(s_snd[next], SND_FRAMES, n->hz, SND_FS, (ew_wave_t)n->wave, SND_AMP, &s_noise);
    audio_i2s_duplex_play_stop();
    audio_i2s_duplex_play_loop(s_snd[next], SND_FRAMES);
    s_snd_cur = next;
    s_snd_playing = true;
}

static void sfx(ew_sfx_id_t id) {
    if (!s_save.sound) return;
    if (s_tv_sound_on) ew_mix_post(&s_mix, id);
    else ew_sfx_play(&s_sfx, id);
}

static void snd_tick(void) {
    ew_note_t n;
    if (ew_sfx_tick(&s_sfx, &n)) snd_note(&n);
}

// The speaker plays when sound is on and the TV does not carry it.
static void speaker_apply(void) {
    const int on = s_save.sound && !s_tv_sound_on;
    if (!s_audio_ok || on == s_speaker) return;
    s_speaker = (int8_t)on;
    if (on) {
        codec_nau88c10_dac_mute(false);
        codec_nau88c10_set_output(CODEC_OUT_SPEAKER);
    } else {
        ew_sfx_play(&s_sfx, EW_SFX_NONE);
        snd_silence();
        codec_nau88c10_speaker_low_power();
    }
}

static void sound_changed(void) {
    if (!s_save.sound) ew_mix_post(&s_mix, EW_SFX_NONE);
    speaker_apply();
}

static void snd_init(void) {
    codec_nau88c10_init();
    audio_i2s_duplex_init(16000);
    s_audio_ok = true;
    ew_sfx_init(&s_sfx);
    ew_mix_init(&s_mix, TV_AMP);
    speaker_apply();
}

// (Re)starts the TV picture, with audio framing and the TV sound (feeder and
// top-up timer) or without, and moves the sound to the TV or the speaker.
// With framing refused, the picture runs without it and the speaker plays.
static void video_start(bool framing) {
    if (s_tv_timer_on) cancel_repeating_timer(&s_tv_timer);
    s_tv_timer_on = false;
    s_tv_sound_on = false;
    tvout_stop();
    tvout_set_audio_framing(framing);
    bool ok = tvout_start(s_fb, EW_FB_W, EW_FB_H);
    if (!ok && framing) {
        DIAG("emberward: tv with audio framing: %s\n", tvout_last_error());
        framing = false;
        tvout_set_audio_framing(false);
        ok = tvout_start(s_fb, EW_FB_W, EW_FB_H);
    }
    if (!ok) {
        DIAG("emberward: tv: %s\n", tvout_last_error());
    } else if (framing) {
        if (!tvout_audio_start()) {
            DIAG("emberward: tv sound: %s\n", tvout_audio_last_error());
        } else if (!add_repeating_timer_us(-(int64_t)TV_FEED_US, tv_feed, NULL, &s_tv_timer)) {
            DIAG("emberward: tv sound: no timer\n");
            tvout_audio_stop();
        } else {
            s_tv_timer_on = true;
            s_tv_sound_on = true;
        }
    }
    ew_mix_post(&s_mix, EW_SFX_NONE);   // nothing carried over from an earlier run
    speaker_apply();
}

// Audio framing only while a TV is connected and TV sound is on: a plain DVI
// monitor may blank with it, and the BSP cannot read the display's EDID.
static void tv_sound_apply(void) {
    const bool want = s_save.tv_sound && s_tv_seen;
    if (want != tvout_audio_framing()) video_start(want);
}

// ------------------------------------------------------------------ SD

static void sd_open(void) {
    ew_lcd_message("CONNECTING TO MAIN...");
    if (fw2_app_recovery_open_onewili(&s_dev) != OW_OK || fw2_app_recovery_wrap_sd() != OW_OK) {
        DIAG("emberward: OneWili link not available, scores stay in RAM\n");
        return;
    }
    uint8_t buf[EW_SAVE_BYTES + 8];
    size_t got = 0;
    ew_lcd_message("LOADING SCORES...");
    const ow_status st = ow_sd_get_mem(&s_dev, SAVE_PATH, buf, sizeof buf, &got);
    fw2_app_recovery_task();
    cec_saved_load(&s_dev);   // the TV input any app saved
    if (st == OW_OK) {
        s_sd_ok = true;
        if (!ew_save_decode(&s_save, buf, got)) DIAG("emberward: %s unreadable, using defaults\n", SAVE_PATH);
    } else if (st == OW_ERR_FAILED && ow_sd_last_error() == SDFS_ERR_NOT_FOUND) {
        s_sd_ok = true;   // a card with no save yet
        DIAG("emberward: no save yet\n");
    } else {
        DIAG("emberward: SD not reachable (%d / sdfs %d), scores stay in RAM\n", (int)st, (int)ow_sd_last_error());
    }
}

static void sd_save(void) {
    if (!s_sd_ok) return;
    snd_silence();   // a looping note would drone through the blocking calls
    uint8_t buf[EW_SAVE_BYTES];
    ew_save_encode(&s_save, buf);
    ow_sd_mkdir(&s_dev, SAVE_DIR_ROOT);   // an existing directory reports a clean error
    fw2_app_recovery_task();
    ow_sd_mkdir(&s_dev, SAVE_DIR);
    fw2_app_recovery_task();
    const ow_status st = ow_sd_put_mem(&s_dev, SAVE_PATH, buf, sizeof buf, false);
    fw2_app_recovery_task();
    if (st != OW_OK) {
        DIAG("emberward: save failed (%d / sdfs %d)\n", (int)st, (int)ow_sd_last_error());
        s_sd_ok = false;
    }
}

// ------------------------------------------------------------------ input

// The TV input this board is on: the first digit of its physical address.
static unsigned tv_input(void) { return cec_physical_address() >> 12; }

// What a front-panel slot means on the current screen (titles: yellow is the
// sound switch and blue the TV input; elsewhere they turn the ward).
static ew_key_t slot_key(int slot) {
    switch (slot) {
    case 0: return EW_KEY_VIEW;
    case 1: return s_screen == EW_SCREEN_TITLE ? EW_KEY_ALT : EW_KEY_LEFT;
    case 2: return EW_KEY_OK;
    case 3: return s_screen == EW_SCREEN_TITLE ? EW_KEY_ALT2 : EW_KEY_RIGHT;
    default: return s_screen == EW_SCREEN_TITLE ? EW_KEY_ALT3 : EW_KEY_PAUSE;
    }
}

static int panel_key(uartkbd_btn_t b) {
    switch (b) {
    case UARTKBD_BTN_GREY: case UARTKBD_BTN_YELLOW: case UARTKBD_BTN_GREEN:
    case UARTKBD_BTN_BLUE: case UARTKBD_BTN_RED:
        return slot_key((int)b);
    case UARTKBD_BTN_NAV_LEFT:   return EW_KEY_LEFT;
    case UARTKBD_BTN_NAV_RIGHT:  return EW_KEY_RIGHT;
    case UARTKBD_BTN_NAV_UP:     return EW_KEY_UP;
    case UARTKBD_BTN_NAV_DOWN:   return EW_KEY_DOWN;
    case UARTKBD_BTN_NAV_CENTER: case UARTKBD_BTN_OK: return EW_KEY_OK;
    case UARTKBD_BTN_CANCEL:     return EW_KEY_PAUSE;
    default:                     return -1;
    }
}

static int remote_key(uint8_t k) {
    switch (k) {
    case CEC_KEY_LEFT:  return EW_KEY_LEFT;
    case CEC_KEY_RIGHT: return EW_KEY_RIGHT;
    case CEC_KEY_UP:    return EW_KEY_UP;
    case CEC_KEY_DOWN:  return EW_KEY_DOWN;
    case CEC_KEY_SELECT: case CEC_KEY_PLAY: return EW_KEY_OK;
    case CEC_KEY_PAUSE: case CEC_KEY_STOP: case CEC_KEY_EXIT: return EW_KEY_PAUSE;
    case CEC_KEY_F4_YELLOW: return slot_key(1);
    case CEC_KEY_F3_GREEN:  return slot_key(2);
    case CEC_KEY_F1_BLUE:   return slot_key(3);
    case CEC_KEY_F2_RED:    return slot_key(4);
    default: return -1;
    }
}

// The key each physical control went down as, so its release lifts the same
// key even if the screen (and so the mapping) changed while it was held.
static int8_t s_panel_down[UARTKBD_BTN_COUNT];
static int8_t s_remote_down[256];
static int8_t s_touch_down = -1;

static void input_init(void) {
    memset(s_panel_down, -1, sizeof s_panel_down);
    memset(s_remote_down, -1, sizeof s_remote_down);
    ew_input_init(&s_in);
}

static void pause_game(void);
static void lcd_restore(void);

static void poll_panel(uint32_t now) {
    uartkbd_event_t ev;
    while (uartkbd_next_event(&ev)) {
        if ((unsigned)ev.btn >= UARTKBD_BTN_COUNT) continue;
        if (ev.pressed) {
            const int k = panel_key(ev.btn);
            if (k < 0) continue;
            s_panel_down[ev.btn] = (int8_t)k;
            ew_input_set(&s_in, EW_SRC_PANEL, (ew_key_t)k, true, now);
        } else if (s_panel_down[ev.btn] >= 0) {
            ew_input_set(&s_in, EW_SRC_PANEL, (ew_key_t)s_panel_down[ev.btn], false, now);
            s_panel_down[ev.btn] = -1;
        }
    }
}

static void poll_touch(uint32_t now) {
    static uint32_t next_ms;
    if ((int32_t)(now - next_ms) < 0) return;
    next_ms = now + 30;   // the touch controller shares I2C with the codec
    uint16_t x, y;
    const bool down = ft6336_poll(&x, &y);
    if (down && s_touch_down < 0) {
        int key = -1;
        if (s_lcd_view) {
            // The mirrored picture fills the LCD: left third turns left,
            // right third turns right, the middle is OK.
            key = x < 160 ? EW_KEY_LEFT : x >= 320 ? EW_KEY_RIGHT : EW_KEY_OK;
        } else {
            const int slot = ew_lcd_touch_slot(x, y);
            if (slot >= 0) key = slot_key(slot);
        }
        if (key >= 0) {
            s_touch_down = (int8_t)key;
            ew_input_set(&s_in, EW_SRC_TOUCH, (ew_key_t)key, true, now);
        }
    } else if (!down && s_touch_down >= 0) {
        ew_input_set(&s_in, EW_SRC_TOUCH, (ew_key_t)s_touch_down, false, now);
        s_touch_down = -1;
    }
}

static void poll_cec(uint32_t now) {
    cec_task();
    if (s_sd_ok && cec_saved_task(&s_dev)) DIAG("emberward: learned TV input %u saved\n", (unsigned)tv_input());
    cec_event_t ev;
    while (cec_next_event(&ev)) {
        switch (ev.type) {
        case CEC_EV_ADDRESS:
            s_cec_ok = ev.data != 15;
            DIAG("emberward: CEC address %u\n", (unsigned)ev.data);
            if (s_cec_ok) cec_wake_tv();   // turn the TV on and switch it to this input
            break;
        case CEC_EV_KEY_DOWN: {
            const int k = remote_key(ev.data);
            if (k < 0) break;
            s_remote_down[ev.data] = (int8_t)k;
            ew_input_set(&s_in, EW_SRC_REMOTE, (ew_key_t)k, true, now);
            break;
        }
        case CEC_EV_KEY_UP:
            if (s_remote_down[ev.data] >= 0) {
                ew_input_set(&s_in, EW_SRC_REMOTE, (ew_key_t)s_remote_down[ev.data], false, now);
                s_remote_down[ev.data] = -1;
            }
            break;
        case CEC_EV_STANDBY:
            pause_game();
            ew_input_release_source(&s_in, EW_SRC_REMOTE);
            break;
        case CEC_EV_ACTIVE_SOURCE:
            if (!ev.data) pause_game();   // the TV switched to another input
            break;
        default:
            break;
        }
    }
}

// ------------------------------------------------------------------ screens

static void go(ew_screen_t screen) {
    s_screen = screen;
    ew_input_flush(&s_in);
}

static void start_game(void) {
    ew_init(&s_game, time_us_32() ^ (s_frame * 2654435761u));
    ew_render_reset(&s_game);
    sfx(EW_SFX_CONFIRM);
    go(EW_SCREEN_PLAY);
}

static void pause_game(void) {
    if (s_screen == EW_SCREEN_PLAY) {
        go(EW_SCREEN_PAUSE);
        sfx(EW_SFX_MENU);
    }
}

static void finish_game(void) {
    const int rank = ew_scores_rank(&s_save, s_game.score);
    memset(&s_entry, 0, sizeof s_entry);
    s_entry.rank = (int8_t)rank;
    if (rank >= 0) {
        s_entry.active = true;
        memcpy(s_entry.name, "AAA", 4);
        go(EW_SCREEN_ENTRY);
    } else {
        go(EW_SCREEN_OVER);
    }
}

static void game_events(void) {
    ew_event_t ev;
    while (ew_next_event(&s_game, &ev)) {
        ew_render_event(&s_game, &ev);
        switch (ev.type) {
        case EW_EV_BLOCK:       if (!ev.pad) sfx(ev.kind == EW_DART ? EW_SFX_DART : EW_SFX_BLOCK); break;
        case EW_EV_ABSORB:      if (!ev.pad) sfx(EW_SFX_ABSORB); break;
        case EW_EV_HEART:       sfx(EW_SFX_HEART); break;
        case EW_EV_SHATTER:     sfx(EW_SFX_SHATTER); break;
        case EW_EV_HIT:         sfx(EW_SFX_HIT); break;
        case EW_EV_TURN:        sfx(EW_SFX_TURN); break;
        case EW_EV_FLARE:       sfx(EW_SFX_FLARE); break;
        case EW_EV_FLARE_READY: sfx(EW_SFX_FLARE_READY); break;
        case EW_EV_WAVE_START:  sfx(EW_SFX_WAVE_START); break;
        case EW_EV_WAVE_CLEAR:  sfx(EW_SFX_WAVE_CLEAR); break;
        case EW_EV_GAME_OVER:
            sfx(ew_scores_rank(&s_save, s_game.score) >= 0 ? EW_SFX_NEW_BEST : EW_SFX_GAME_OVER);
            DIAG("emberward: game over, score %u wave %u\n", (unsigned)s_game.score, (unsigned)s_game.wave);
            break;
        default: break;
        }
    }
}

static void toggle_view(void) {
    st7796_flush_wait();
    s_lcd_view = !s_lcd_view;
    sfx(EW_SFX_MENU);
    lcd_restore();
}

static void handle_keys(void) {
    if (ew_input_take(&s_in, EW_KEY_VIEW)) toggle_view();
    switch (s_screen) {
    case EW_SCREEN_TITLE:
        if (ew_input_take(&s_in, EW_KEY_OK)) start_game();
        if (ew_input_take(&s_in, EW_KEY_ALT)) {
            s_save.sound = !s_save.sound;
            sound_changed();
            sd_save();
            sfx(EW_SFX_CONFIRM);
        }
        if (ew_input_take(&s_in, EW_KEY_ALT2)) {
            // Overrides the address for this run, and saves it for every app
            // as the place to start when the firmware does not hand one over.
            const uint16_t pa = (uint16_t)((tv_input() % TV_INPUT_MAX + 1) << 12);
            cec_set_physical_address(pa);
            if (s_sd_ok) {
                snd_silence();
                cec_saved_store(&s_dev, pa);
            }
            sfx(EW_SFX_MENU);
        }
        if (ew_input_take(&s_in, EW_KEY_ALT3)) {
            s_save.tv_sound = !s_save.tv_sound;
            sd_save();
            tv_sound_apply();
            sfx(EW_SFX_CONFIRM);
        }
        break;
    case EW_SCREEN_PLAY:
        if (ew_input_take(&s_in, EW_KEY_PAUSE)) { pause_game(); break; }
        while (ew_input_take(&s_in, EW_KEY_LEFT)) ew_turn(&s_game, -1);
        while (ew_input_take(&s_in, EW_KEY_RIGHT)) ew_turn(&s_game, +1);
        while (ew_input_take(&s_in, EW_KEY_UP)) ew_turn(&s_game, 4);
        while (ew_input_take(&s_in, EW_KEY_DOWN)) ew_turn(&s_game, 4);
        if (ew_input_take(&s_in, EW_KEY_OK) && !ew_flare(&s_game)) sfx(EW_SFX_MENU);
        break;
    case EW_SCREEN_PAUSE:
        if (ew_input_take(&s_in, EW_KEY_OK)) { go(EW_SCREEN_PLAY); sfx(EW_SFX_CONFIRM); }
        else if (ew_input_take(&s_in, EW_KEY_PAUSE)) { go(EW_SCREEN_TITLE); sfx(EW_SFX_MENU); }
        break;
    case EW_SCREEN_ENTRY: {
        int c = s_entry.cursor;
        if (ew_input_take(&s_in, EW_KEY_LEFT) && c > 0) { s_entry.cursor--; sfx(EW_SFX_MENU); }
        if (ew_input_take(&s_in, EW_KEY_RIGHT) && c < 3) { s_entry.cursor++; sfx(EW_SFX_MENU); }
        c = s_entry.cursor;
        if (c < 3) {
            if (ew_input_take(&s_in, EW_KEY_UP)) { s_entry.name[c] = ew_name_char_step(s_entry.name[c], 1); sfx(EW_SFX_TURN); }
            if (ew_input_take(&s_in, EW_KEY_DOWN)) { s_entry.name[c] = ew_name_char_step(s_entry.name[c], -1); sfx(EW_SFX_TURN); }
        }
        if (ew_input_take(&s_in, EW_KEY_OK)) {
            if (c < 3) {
                s_entry.cursor++;
                sfx(EW_SFX_MENU);
            } else {
                s_entry.rank = (int8_t)ew_scores_insert(&s_save, s_entry.name, s_game.score, s_game.wave);
                s_entry.active = false;
                s_save_gen++;
                sfx(EW_SFX_CONFIRM);
                ew_lcd_message("SAVING...");
                sd_save();
                go(EW_SCREEN_OVER);
            }
        }
        break;
    }
    case EW_SCREEN_OVER:
        if (ew_input_take(&s_in, EW_KEY_OK)) start_game();
        else if (ew_input_take(&s_in, EW_KEY_PAUSE)) { go(EW_SCREEN_TITLE); sfx(EW_SFX_MENU); }
        break;
    }
}

// One 1/60 s tick of everything that moves.
static void tick(void) {
    if (s_screen == EW_SCREEN_PLAY) {
        ew_step(&s_game);
        game_events();
        if (s_game.phase == EW_PHASE_OVER) finish_game();
    } else if (s_screen == EW_SCREEN_OVER || s_screen == EW_SCREEN_ENTRY) {
        ew_step(&s_game);   // lets the last particles settle; nothing spawns once over
        game_events();
    }
    if (s_screen != EW_SCREEN_PAUSE) ew_render_tick(&s_game);
    snd_tick();
    s_frame++;
}

static void draw(uint16_t *fb) {
    switch (s_screen) {
    case EW_SCREEN_TITLE: ew_render_title(fb, &s_save, s_frame, s_sd_ok); break;
    case EW_SCREEN_PLAY:  ew_render_game(fb, &s_game, s_frame, false); break;
    case EW_SCREEN_PAUSE: ew_render_game(fb, &s_game, s_frame, true); break;
    default:              ew_render_over(fb, &s_game, s_frame, &s_entry, &s_save); break;
    }
}

static void lcd_info(ew_lcd_info_t *li) {
    memset(li, 0, sizeof *li);
    li->screen = (uint8_t)s_screen;
    li->lcd_view = s_lcd_view;
    li->score = s_game.score;
    li->wave = s_game.wave;
    li->lives = s_game.lives;
    li->mult = (uint8_t)ew_multiplier(&s_game);
    li->combo = s_game.combo;
    li->flare = s_game.flare;
    li->tv_seen = s_tv_seen;
    li->tv_running = tvout_running();
    li->cec_ok = s_cec_ok;
    li->sound = s_save.sound != 0;
    li->tv_sound = s_save.tv_sound != 0;
    li->tv_sound_on = s_tv_sound_on;
    li->tv_input = tv_input();
    li->sd_ok = s_sd_ok;
    li->save = &s_save;
    li->save_gen = s_save_gen;
}

static void lcd_restore(void) {
    ew_lcd_info_t li;
    lcd_info(&li);
    ew_lcd_full(&li);
}

// PAGE held for five seconds: the About page on the LCD. The loop leaves the
// LCD alone while it shows; releasing PAGE redraws the companion screen.
static bool s_about;

static void about_render(const char *name, uint16_t version, const char *repository) {
    char line[48];
    s_about = true;
    st7796_flush_wait();
    const uint16_t bg = EW_RGB(8, 10, 26);
    st7796_fill_screen(bg);
    st7796_draw_text(12, 20, 3, EW_RGB(255, 90, 50), bg, "ABOUT");
    st7796_draw_text(12, 70, 2, EW_RGB(255, 206, 64), bg, name);
    snprintf(line, sizeof line, "VERSION %03u", (unsigned)version);
    st7796_draw_text(12, 105, 2, EW_RGB(230, 236, 255), bg, line);
    st7796_draw_text(12, 155, 1, EW_RGB(110, 120, 160), bg, "SOURCE:");
    st7796_draw_text(12, 180, 1, EW_RGB(230, 236, 255), bg, repository);
    st7796_draw_text(12, 285, 1, EW_RGB(230, 236, 255), bg, "RELEASE PAGE TO RETURN");
}

static void about_restore(void) {
    s_about = false;
    lcd_restore();
}

int main(void) {
    board_init();
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_set_renderer(about_render, about_restore);
    st7796_fill_screen(EW_RGB(8, 10, 26));
    board_backlight_set(1);
    picpwr_release_unused();
    DIAG("\n=== emberward ===\n");

    ew_save_defaults(&s_save);
    input_init();
    ew_render_init();
    ew_init(&s_game, 1);
    ew_lcd_message("EMBERWARD");

    if (!ft6336_init()) DIAG("emberward: touch not answering\n");
    sd_open();
    snd_init();

    // The title on the TV from the first frame; DVI runs whether or not a TV
    // is plugged in.
    draw(s_fb);
    video_start(false);   // audio framing once a TV is reported (tv_sound_apply)
    if (!cec_start("Emberward")) DIAG("emberward: cec: %s\n", cec_last_error());

    lcd_restore();
    uint64_t last_us = time_us_64();
    uint64_t acc_us = 0;
    uint64_t frame_start_us = time_us_64();
    uint32_t vsync_misses = 0, slow_frames = 0;
    uint32_t sound_report_frame = 0;   // TV sound statistics over RTT every ten seconds

    while (true) {
        fw2_app_recovery_task();

        // Wait for the TV to leave the picture rows: from 12 ms after the last
        // window it is in the next frame's picture, so this waits for the
        // end of it. Re-pointing the rows at the same buffer changes nothing.
        while (time_us_64() - frame_start_us < 12000u) fw2_app_recovery_task();
        if (tvout_running()) {
            if (!tvout_show(s_fb) && ++vsync_misses % 60 == 1)
                DIAG("emberward: vsync: %s\n", tvout_last_error());
        } else {
            // No video (refused at start): pace the loop by hand.
            while (time_us_64() - frame_start_us < TICK_US) fw2_app_recovery_task();
        }
        frame_start_us = time_us_64();

        // In LCD view the LCD reads the picture through DMA; the picture is
        // drawn whatever it does (the LCD may show a frame boundary), and the
        // next flush starts once the last one is done.
        draw(s_fb);
        const uint32_t draw_us = (uint32_t)(time_us_64() - frame_start_us);
        if (draw_us > 10000u && ++slow_frames % 60 == 1)
            DIAG("emberward: slow frame %u us (%u so far)\n", (unsigned)draw_us, (unsigned)slow_frames);
        if (s_lcd_view && !s_about && !st7796_flush_busy())
            st7796_flush_async(EW_LCD_FIELD_X, EW_LCD_FIELD_Y, EW_LCD_FIELD_X + EW_FB_W - 1,
                               EW_LCD_FIELD_Y + EW_FB_H - 1, s_fb, NULL);

        picpwr_task();
        const tvout_plug_event_t plug = tvout_task();
        if (plug == TVOUT_PLUG_CONNECTED) { s_tv_seen = true; tv_sound_apply(); }
        if (plug == TVOUT_PLUG_REMOVED) { s_tv_seen = false; tv_sound_apply(); pause_game(); }
        if (s_tv_sound_on && s_frame - sound_report_frame >= 600) {
            sound_report_frame = s_frame;
            tvout_audio_stats_t st;
            tvout_audio_get_stats(&st);
            DIAG("emberward: tv sound packets %u underruns %u late %u lost %u repeats %u\n",
                 (unsigned)st.packets, (unsigned)st.underruns, (unsigned)st.late, (unsigned)st.lost,
                 (unsigned)st.repeats);
        }

        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        poll_panel(now_ms);
        poll_touch(now_ms);
        poll_cec(now_ms);
        ew_input_tick(&s_in, now_ms);
        handle_keys();

        // Fixed 60 Hz game time, whatever the frame rate.
        const uint64_t now_us = time_us_64();
        acc_us += now_us - last_us;
        last_us = now_us;
        int steps = 0;
        while (acc_us >= TICK_US && steps < MAX_CATCHUP) {
            tick();
            acc_us -= TICK_US;
            steps++;
        }
        if (acc_us > TICK_US * MAX_CATCHUP) acc_us = 0;   // after a long blocking call

        if (!s_lcd_view && !s_about) {
            ew_lcd_info_t li;
            lcd_info(&li);
            ew_lcd_update(&li);
        }
    }
}
