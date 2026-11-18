// bsp/display/tvout_audio_core.h — C entry points to the TV sound feeder's
// slot bookkeeping and packet coding (rpDVIAudioCore.h) for tvout_audio.c.
// No hardware access: the caller supplies the scan position and runs the
// tick from its timer interrupt.
#ifndef TVOUT_AUDIO_CORE_H
#define TVOUT_AUDIO_CORE_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The rate of the frame's clock regeneration packet and of every sample.
#define TVOUT_AUDIO_RATE_HZ 48000u

// Stereo frames the app's samples wait in before the feeder's own 128-frame
// buffer: 2048, 42.7 ms at 48 kHz, 8 KB of SRAM. A target may define another
// power of two, at least 128.
#ifndef TVOUT_AUDIO_BUFFER_FRAMES
#define TVOUT_AUDIO_BUFFER_FRAMES 2048u
#endif

// Tone bits for tvout_audio_core_set_tone(); none is the sample buffer.
#define TVOUT_AUDIO_CORE_TONE_LEFT  1u
#define TVOUT_AUDIO_CORE_TONE_RIGHT 2u

typedef struct {
    uint32_t packets;    // sample packets written in time
    uint32_t underruns;  // frames sent as silence: the sample buffer was empty
    uint32_t late;       // lines the TV reached before they were written
    uint32_t repeats;    // of those, lines that sent a packet with samples again
    uint32_t lost;       // frames dropped on those lines
} tvout_audio_core_counts_t;

// Sets the feeder up for the frame tvout_list built last, empties the sample
// buffer and clears the counts; the tick stays idle until
// tvout_audio_core_run(). False when that
// frame has no audio slots or its slots and line rate do not fit the feeder.
bool tvout_audio_core_configure(void);

// Lets the tick in (true) or keeps it out (false).
void tvout_audio_core_run(bool on);
bool tvout_audio_core_running(void);

// One tick: first moves what the sample buffer holds into the feeder's own,
// then fills slots. next_entry returns the scan position: the frame piece
// after the one being sent. Returns the microseconds to the next tick, or 0
// when stopping is done or the feeder does not run.
uint32_t tvout_audio_core_tick(uint32_t (*next_entry)(void));

// frames stereo frames, left then right, into the sample buffer
// (TVOUT_AUDIO_BUFFER_FRAMES); returns how many fitted. One caller at a
// time, on either core, an interrupt handler included; the tick is the only
// reader.
unsigned tvout_audio_core_write(const int16_t *stereo, unsigned frames);

// Frames tvout_audio_core_write() would take now.
unsigned tvout_audio_core_room(void);

void tvout_audio_core_set_tone(unsigned bits);

// Stopping: the ticks that follow put a null packet in every slot, each once
// the TV has finished with it. tvout_audio_core_muted() says when all are.
void tvout_audio_core_mute(void);
bool tvout_audio_core_muted(void);
// A null packet in every slot at once, for a mute that did not finish.
void tvout_audio_core_null_all(void);

void tvout_audio_core_get_counts(tvout_audio_core_counts_t *out);

#ifdef __cplusplus
}
#endif
#endif
