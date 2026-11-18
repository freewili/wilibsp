// Host tests for bsp/display/tvout_audio_core and the audio framing entry
// points of bsp/display/tvout_list: the pool size, the list sizes with audio
// framing, and the feeder set up from the frame tvout_list built and ticked on
// a scan position that moves one frame piece a tick.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "test_util.h"
#include "rpDVIAudioCore.h"
#include "rpDVIFrame.h"
#include "rpDVIList.h"
#include "tvout_audio_core.h"
#include "tvout_list.h"
#include "tvout_list_frame.h"

namespace {

const tvout_list_config_t CFG = { 0x0000A5A0u, 0x00001E00u, 9u, 7u, 0x50120004u,
                                  0x0000A4A0u, 0x0000A1A0u, 4u, 5u, 0x50000180u };

uint32_t s_pool[TVOUT_LIST_AUDIO_POOL_WORDS];
uint32_t s_block[TVOUT_LIST_BYTES / sizeof(uint32_t)];
uint16_t s_pixels[480 * 320];

uint32_t s_piece;
uint32_t s_pieces;

uint32_t nextEntry()
{
    return s_piece;
}

// Ticks while the scan moves one piece a tick, for frames whole frames.
void runFrames(unsigned frames)
{
    for (unsigned i = 0; i < frames * s_pieces; i++) {
        s_piece = s_piece + 1u > s_pieces ? 1u : s_piece + 1u;
        tvout_audio_core_tick(nextEntry);
    }
}

void testPoolCoversEveryFrame()
{
    const stDVIMode &mode = DVI_MODE_640x480_60;
    ASSERT_EQ(rpDVIFrame::audioFramingBarsWords(mode), 990);
    unsigned most = 0;
    unsigned listMost = 0;
    tvout_list_set_audio_framing(s_pool, TVOUT_LIST_AUDIO_POOL_WORDS);
    for (unsigned w = 2; w <= 640; w += 2)
        for (unsigned h = 1; h <= 480; h++) {
            const unsigned words = rpDVIFrame::audioFramingPictureWords(mode, w, h);
            if (words > most) most = words;
            const unsigned bytes = tvout_list_picture_bytes(w, h, 1u);
            ASSERT_TRUE(bytes != 0);
            if (bytes > listMost) listMost = bytes;
        }
    tvout_list_set_audio_framing(nullptr, 0);
    ASSERT_EQ(most, TVOUT_LIST_AUDIO_POOL_WORDS);
    ASSERT_TRUE(listMost <= TVOUT_LIST_BYTES);
    // With audio framing a picture's list is shorter than with DVI framing.
    const unsigned dvi = tvout_list_picture_bytes(480, 320, 1u);
    tvout_list_set_audio_framing(s_pool, TVOUT_LIST_AUDIO_POOL_WORDS);
    ASSERT_TRUE(tvout_list_picture_bytes(480, 320, 1u) < dvi);
    // A 320x240 picture at 2x is the frame of a 640x480 one.
    ASSERT_EQ(tvout_list_picture_bytes(320, 240, 2u), tvout_list_picture_bytes(640, 480, 1u));
    tvout_list_set_audio_framing(nullptr, 0);
    std::printf("audio framing: pool %u words, largest picture list %u bytes\n", most, listMost);
}

void testDviFrameRefused()
{
    tvout_list_set_audio_framing(nullptr, 0);
    ASSERT_TRUE(tvout_list_build_bars(reinterpret_cast<tvout_list_entry_t *>(s_block), TVOUT_LIST_BARS_ENTRIES, &CFG));
    ASSERT_TRUE(!tvout_audio_core_configure());
}

void testShortPoolRefused()
{
    tvout_list_set_audio_framing(s_pool, 989);
    ASSERT_TRUE(!tvout_list_build_bars(reinterpret_cast<tvout_list_entry_t *>(s_block), TVOUT_LIST_BARS_ENTRIES, &CFG));
    tvout_list_set_audio_framing(nullptr, 0);
}

void testBarsTone()
{
    tvout_list_set_audio_framing(s_pool, TVOUT_LIST_AUDIO_POOL_WORDS);
    ASSERT_TRUE(tvout_list_build_bars(reinterpret_cast<tvout_list_entry_t *>(s_block), TVOUT_LIST_BARS_ENTRIES, &CFG));
    ASSERT_TRUE(tvout_audio_core_configure());
    ASSERT_TRUE(!tvout_audio_core_running());
    ASSERT_EQ(tvout_audio_core_tick(nextEntry), 0);
    s_pieces = tvoutListList().scanCount();
    ASSERT_EQ(s_pieces, TVOUT_LIST_BARS_ENTRIES);
    s_piece = 1;
    tvout_audio_core_set_tone(TVOUT_AUDIO_CORE_TONE_LEFT | TVOUT_AUDIO_CORE_TONE_RIGHT);
    tvout_audio_core_run(true);
    runFrames(10);

    // 800 frames a frame at 48 kHz, four a packet: about 200 packets a frame.
    tvout_audio_core_counts_t c;
    tvout_audio_core_get_counts(&c);
    ASSERT_TRUE(c.packets >= 9u * 200u && c.packets <= 11u * 200u);
    ASSERT_EQ(c.late, 0);
    ASSERT_EQ(c.lost, 0);
    ASSERT_EQ(c.repeats, 0);
    ASSERT_EQ(c.underruns, 0);

    // Stopping: every slot back to a null packet, then no more ticks.
    tvout_audio_core_mute();
    for (unsigned i = 0; i < 4u * s_pieces && !tvout_audio_core_muted(); i++) {
        s_piece = s_piece + 1u > s_pieces ? 1u : s_piece + 1u;
        tvout_audio_core_tick(nextEntry);
    }
    ASSERT_TRUE(tvout_audio_core_muted());
    ASSERT_EQ(tvout_audio_core_tick(nextEntry), 0);
    tvout_audio_core_run(false);
    rpDVIFrame &frame = tvoutListFrame();
    std::vector<uint32_t> nullWords(frame.audioSlotWords(0), frame.audioSlotWords(0) + 32);
    for (unsigned slot = 0; slot < frame.audioSlotCount(); slot++)
        for (unsigned k = 0; k < 32; k++)
            ASSERT_TRUE(frame.audioSlotWords(slot)[k] == nullWords[k]);
    tvout_list_set_audio_framing(nullptr, 0);
}

void testPictureWrite()
{
    tvout_list_set_audio_framing(s_pool, TVOUT_LIST_AUDIO_POOL_WORDS);
    tvout_list_picture_t pic;
    ASSERT_TRUE(tvout_list_build_picture(s_block, sizeof s_block, s_pixels, 480, 320, 1u, &CFG, &pic));
    ASSERT_TRUE(tvout_audio_core_configure());
    s_pieces = tvoutListList().scanCount();
    s_piece = 1;
    tvout_audio_core_set_tone(0);

    // The sample buffer takes TVOUT_AUDIO_BUFFER_FRAMES frames, and says so.
    int16_t stereo[2 * 200];
    for (unsigned i = 0; i < 200; i++) {
        stereo[2 * i] = (int16_t)i;
        stereo[2 * i + 1] = (int16_t)-i;
    }
    ASSERT_EQ(tvout_audio_core_room(), TVOUT_AUDIO_BUFFER_FRAMES);
    unsigned written = 0;
    while (tvout_audio_core_write(stereo, 200) == 200)
        written += 200;
    written += TVOUT_AUDIO_BUFFER_FRAMES % 200u;
    ASSERT_EQ(tvout_audio_core_room(), 0);
    ASSERT_EQ(tvout_audio_core_write(stereo, 1), 0);
    tvout_audio_core_run(true);

    // A full buffer lasts its length with no writes: 2048 frames is more
    // than two TV frames of 800.
    runFrames(2);
    tvout_audio_core_counts_t c;
    tvout_audio_core_get_counts(&c);
    ASSERT_EQ(c.underruns, 0);
    ASSERT_TRUE(tvout_audio_core_room() >= 2u * 800u - DVI_AUDIO_PCM_FRAMES);

    // Kept topped up: no frame goes out as silence.
    for (unsigned i = 0; i < 4u * s_pieces; i++) {
        s_piece = s_piece + 1u > s_pieces ? 1u : s_piece + 1u;
        tvout_audio_core_tick(nextEntry);
        written += tvout_audio_core_write(stereo, 200);
    }
    tvout_audio_core_get_counts(&c);
    ASSERT_EQ(c.underruns, 0);
    ASSERT_EQ(c.late, 0);
    ASSERT_TRUE(c.packets > 0);
    ASSERT_TRUE(written >= 3u * 800u);

    // Left to run dry: the rest goes out as silence and is counted.
    runFrames(4);
    tvout_audio_core_get_counts(&c);
    ASSERT_TRUE(c.underruns > 0);
    ASSERT_EQ(c.late, 0);
    tvout_audio_core_run(false);
    tvout_list_set_audio_framing(nullptr, 0);
}

}

int main()
{
    testPoolCoversEveryFrame();
    testDviFrameRefused();
    testShortPoolRefused();
    testBarsTone();
    testPictureWrite();
    TEST_RETURN();
}
