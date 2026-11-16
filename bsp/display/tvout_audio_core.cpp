// bsp/display/tvout_audio_core.cpp — see tvout_audio_core.h.
//
// Every line of a frame with audio framing but the vertical sync and the
// InfoFrame and clock regeneration lines sends one packet from an audio slot
// near its start, and lines share a few slots in rings. Which lines are
// written, how many samples each carries and when a slot may be written are
// decided in rpDVIAudioCore.h; this file feeds it the frame's slot map and
// runs its tick. The tick and the core functions it inlines run from SRAM in
// every app, PSRAM-executing ones included.
#include "tvout_audio_core.h"
#include "rpDVIAudioCore.h"
#include "rpDVIPacket.h"
#include "tvout_list_frame.h"

#ifdef HOST_TEST
#define TVOUT_AUDIO_RAM(name) name
#else
#include "pico.h"
#define TVOUT_AUDIO_RAM(name) __not_in_flash_func(name)
#endif

namespace {

constexpr const stDVIMode &MODE = DVI_MODE_640x480_60;
constexpr unsigned LINES = (unsigned)MODE.iVFrontPorch + MODE.iVSync + MODE.iVBackPorch + MODE.iVActive;
constexpr unsigned LINE_CLOCKS = (unsigned)MODE.iHFrontPorch + MODE.iHSync + MODE.iHBackPorch + MODE.iHActive;

static_assert(DVI_AUDIO_MAX_SLOTS >= 2 * rpDVIFrame::AUDIO_RING_SLOTS + rpDVIFrame::AUDIO_BLANK_SLOTS,
              "a picture frame has a row ring with the first row's slot, a border ring and the blanking slots");
static_assert(DVI_AUDIO_LINES_AHEAD < rpDVIFrame::AUDIO_RING_SLOTS, "a slot would be written while the TV may still read it");
static_assert(TVOUT_AUDIO_CORE_TONE_LEFT == DVI_AUDIO_TONE_LEFT && TVOUT_AUDIO_CORE_TONE_RIGHT == DVI_AUDIO_TONE_RIGHT,
              "tone bits differ");
static_assert((TVOUT_AUDIO_BUFFER_FRAMES & (TVOUT_AUDIO_BUFFER_FRAMES - 1u)) == 0 &&
              TVOUT_AUDIO_BUFFER_FRAMES >= DVI_AUDIO_PCM_FRAMES,
              "the sample buffer is a power of two no smaller than the feeder's own");

// The line table, then the sample buffer (audioBufferBytes(LINES)).
alignas(4) uint8_t s_buffer[((LINES + 3u) & ~3u) + DVI_AUDIO_PCM_FRAMES * 4u];
stDVIAudioCore s_core;

// The sample buffer: frames written by the app at s_head, taken by the tick
// at s_tail, both counting up and wrapping by masking.
int16_t s_samples[2u * TVOUT_AUDIO_BUFFER_FRAMES];
volatile uint32_t s_head;
volatile uint32_t s_tail;

// The sample buffer into the feeder's own, as far as it takes.
inline void moveSamples()
{
    const uint32_t head = s_head;
    uint32_t tail = s_tail;
    __sync_synchronize();                   // the frames before head are written
    while (tail != head) {
        const uint32_t at = tail & (TVOUT_AUDIO_BUFFER_FRAMES - 1u);
        const uint32_t run = head - tail < TVOUT_AUDIO_BUFFER_FRAMES - at ? head - tail : TVOUT_AUDIO_BUFFER_FRAMES - at;
        const uint32_t taken = audioPcmWrite(s_core, &s_samples[2u * at], run);
        tail += taken;
        if (taken < run)
            break;
    }
    __sync_synchronize();                   // the frames are copied before their room is given back
    s_tail = tail;
}

}

bool tvout_audio_core_configure(void)
{
    rpDVIFrame &frame = tvoutListFrame();
    const rpDVIList &list = tvoutListList();
    if (audioBufferBytes(LINES) > sizeof s_buffer || frame.audioSlotCount() == 0 ||
        (MODE.iPixelClockKHz * 1000u) % LINE_CLOCKS != 0)
        return false;

    uint8_t status[DVI_CHANNEL_STATUS_BYTES];
    s_head = 0;
    s_tail = 0;
    audioInit(s_core, s_buffer, LINES, list.scanCount(), list.scanFirstRow(), list.rows());
    for (unsigned line = 0; line < LINES; line++)
        audioSetLine(s_core, line, frame.audioLineSlot(line));
    for (unsigned slot = 0; slot < frame.audioSlotCount() && slot < DVI_AUDIO_MAX_SLOTS; slot++)
        audioSetSlot(s_core, slot, frame.audioSlotWords(slot));
    return dviChannelStatus(TVOUT_AUDIO_RATE_HZ, status) &&
           audioConfigure(s_core, frame.audioSlotCount(), TVOUT_AUDIO_RATE_HZ, MODE.iPixelClockKHz * 1000u / LINE_CLOCKS,
                          frame.audioVLevel(), frame.audioHLevel(), status);
}

void tvout_audio_core_run(bool on)
{
    s_core.bRunning = on;
}

bool tvout_audio_core_running(void)
{
    return s_core.bRunning;
}

// The slot copy stays a loop, not a call to memcpy().
__attribute__((optimize("no-tree-loop-distribute-patterns")))
uint32_t TVOUT_AUDIO_RAM(tvout_audio_core_tick)(uint32_t (*next_entry)(void))
{
    if (!s_core.bRunning)
        return 0;
    moveSamples();
    if (audioTickBegin(s_core, next_entry()))
    {
        uint32_t words[32];
        while (audioTickPrepare(s_core, words) >= 0)
            audioTickCommit(s_core, next_entry(), words);
    }
    return audioTickEnd(s_core);
}

unsigned tvout_audio_core_write(const int16_t *stereo, unsigned frames)
{
    const uint32_t head = s_head;
    const uint32_t room = TVOUT_AUDIO_BUFFER_FRAMES - (head - s_tail);
    const uint32_t count = frames < room ? frames : room;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t at = (head + i) & (TVOUT_AUDIO_BUFFER_FRAMES - 1u);
        s_samples[2u * at] = stereo[2u * i];
        s_samples[2u * at + 1u] = stereo[2u * i + 1u];
    }
    __sync_synchronize();                   // the frames are written before head moves past them
    s_head = head + count;
    return count;
}

unsigned tvout_audio_core_room(void)
{
    return TVOUT_AUDIO_BUFFER_FRAMES - (s_head - s_tail);
}

void tvout_audio_core_set_tone(unsigned bits)
{
    s_core.iTone = bits & (DVI_AUDIO_TONE_LEFT | DVI_AUDIO_TONE_RIGHT);
}

void tvout_audio_core_mute(void)
{
    s_core.bMuting = true;
}

bool tvout_audio_core_muted(void)
{
    return s_core.bMuted;
}

void tvout_audio_core_null_all(void)
{
    for (uint32_t slot = 0; slot < s_core.iSlots; slot++)
        audioNullWords(s_core, s_core.pSlot[slot]);
    s_core.iSlotAudio = 0;
}

void tvout_audio_core_get_counts(tvout_audio_core_counts_t *out)
{
    out->packets = s_core.iPackets;
    out->underruns = s_core.iUnderruns;
    out->late = s_core.iLate;
    out->repeats = s_core.iRepeats;
    out->lost = s_core.iLost;
}
