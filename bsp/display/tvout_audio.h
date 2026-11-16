// bsp/display/tvout_audio.h — sound on the TV: stereo 16-bit samples at
// 48 kHz in the audio slots of a picture sent with audio framing.
//
// Audio framing is off by default. With it on, the picture carries data
// islands (the picture format, and room for sound); a TV plays the sound, but
// a plain DVI monitor may show nothing. The BSP cannot read the display's
// EDID, so the choice is the app's.
//
// The feeder is a TIMER1 alarm interrupt on the core that called
// tvout_audio_start(), at the highest priority: it keeps the slots up to
// seven TV lines ahead of the scan-out, a tick about every 24 us, each at
// most about 23 us. Other interrupts on that core wait for it.
#ifndef TVOUT_AUDIO_H
#define TVOUT_AUDIO_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TVOUT_TONE_OFF,     // the samples from tvout_audio_write()
    TVOUT_TONE_LEFT,    // 1 kHz at -12 dBFS on the left channel only
    TVOUT_TONE_RIGHT,
    TVOUT_TONE_BOTH,
} tvout_tone_t;

typedef struct {
    uint32_t packets;    // sample packets written in time
    uint32_t underruns;  // frames sent as silence: tvout_audio_write() fell behind
    uint32_t late;       // TV lines that went out before their packet was written
    uint32_t repeats;    // of those, lines that sent the previous packet's samples again
    uint32_t lost;       // frames dropped on those lines
} tvout_audio_stats_t;

// Audio framing from the next tvout_start_test() or tvout_start(), or
// plain DVI (the default). A running picture keeps the framing it started
// with. Takes 5.2 KB of static SRAM, linked only into apps that call this.
void tvout_set_audio_framing(bool on);
bool tvout_audio_framing(void);

// Starts filling the audio slots of the running picture. True if the feeder
// runs (also when it already did); false with tvout_audio_last_error() naming
// the cause: no picture is running, it was started without audio framing, no
// free TIMER1 alarm, or TIMER1 does not count microseconds. Starts in
// silence, or the tone last chosen.
bool tvout_audio_start(void);

// The slots go back to null packets and the feeder stops. tvout_stop() calls
// it. Nothing when the feeder is not running.
void tvout_audio_stop(void);

bool tvout_audio_running(void);

const char *tvout_audio_last_error(void);

// The built-in tone in place of the written samples, or TVOUT_TONE_OFF for
// the written samples. While the tone plays, written samples are taken at the
// same rate and dropped. May be called at any time; kept across starts.
void tvout_audio_tone(tvout_tone_t tone);

// frames stereo frames, interleaved left then right, at 48 kHz, into a
// 2048-frame buffer (42.7 ms; TVOUT_AUDIO_BUFFER_FRAMES); returns how many it
// took, 0 while the feeder is stopped. Frames the TV needs while the buffer
// is empty go out as silence and count in underruns. All writes from one
// context: the main loop of one core, or one interrupt handler (a timer's,
// say). Never call tvout_audio_stop() or tvout_stop() from an interrupt
// handler while a write may be running.
unsigned tvout_audio_write(const int16_t *stereo, unsigned frames);

// Frames tvout_audio_write() would take now; 0 while the feeder is stopped.
// An app makes this many frames and writes them, once per loop pass.
unsigned tvout_audio_room(void);

// Counts since the last tvout_audio_start().
void tvout_audio_get_stats(tvout_audio_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif
