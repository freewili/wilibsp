/* Host tests for bsp/display/tvout_list: the C entry points tvout.c calls,
 * built from C against the C++ list builder. The scan-out of a picture list
 * and the page-flip window on a model of the DMA channels are in
 * test_dvi_frame.cpp. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "test_util.h"
#include "display/tvout_list.h"

#define CTRL_WORDS   0x0000A5A0u
#define CHAIN_BITS   0x00001E00u
#define CHAIN_SHIFT  9u
#define REWIND       7u
#define FIFO         0x50120004u
#define CTRL_PIXELS  0x0000A590u
#define CTRL_COPY    0x0000BF20u
#define JOIN         8u
#define RESTART      9u
#define ROW_READ     0x50000280u

static tvout_list_entry_t s_list[TVOUT_LIST_BARS_ENTRIES];
static uint32_t s_block[TVOUT_LIST_BYTES / sizeof(uint32_t)];
static uint16_t s_pixels[640 * 480];

static void test_bars_entry_count(void) {
    ASSERT_TRUE(tvout_list_bars_entries() == TVOUT_LIST_BARS_ENTRIES);
    ASSERT_TRUE(tvout_list_pixel_clock_khz() == 25200u);
}

static void test_bars_build(void) {
    const tvout_list_config_t cfg = { CTRL_WORDS, CHAIN_BITS, CHAIN_SHIFT, REWIND, FIFO };
    memset(s_list, 0, sizeof s_list);
    ASSERT_TRUE(tvout_list_build_bars(s_list, TVOUT_LIST_BARS_ENTRIES, &cfg));
    /* Every entry writes the FIFO; all but the last hand back to the list
     * channel unchanged, the last chains to the rewind channel. */
    unsigned words = 0;
    for (unsigned i = 0; i < TVOUT_LIST_BARS_ENTRIES; i++) {
        ASSERT_TRUE(s_list[i].write_addr == FIFO);
        ASSERT_TRUE(s_list[i].count != 0);
        ASSERT_TRUE(s_list[i].read_addr != 0);
        words += s_list[i].count;
        if (i + 1 < TVOUT_LIST_BARS_ENTRIES)
            ASSERT_TRUE(s_list[i].ctrl == CTRL_WORDS);
    }
    const uint32_t last = s_list[TVOUT_LIST_BARS_ENTRIES - 1].ctrl;
    ASSERT_TRUE((last & CHAIN_BITS) == (REWIND << CHAIN_SHIFT));
    ASSERT_TRUE((last & ~CHAIN_BITS) == (CTRL_WORDS & ~CHAIN_BITS));
    ASSERT_TRUE(words > 0);
}

static void test_bars_refused_when_short(void) {
    const tvout_list_config_t cfg = { CTRL_WORDS, CHAIN_BITS, CHAIN_SHIFT, REWIND, FIFO };
    ASSERT_TRUE(!tvout_list_build_bars(s_list, TVOUT_LIST_BARS_ENTRIES - 1u, &cfg));
}

/* Every picture the video mode takes fits the one static block, and only
 * those are sized. */
static void test_every_picture_fits_the_block(void) {
    unsigned largest = 0, sized = 0;
    for (unsigned w = 1; w <= 642; w++)
        for (unsigned h = 1; h <= 481; h++) {
            const unsigned bytes = tvout_list_picture_bytes(w, h, 1u);
            const bool fits_mode = w <= 640 && h <= 480 && (640 - w) % 2 == 0;
            ASSERT_TRUE((bytes != 0) == fits_mode);
            if (bytes > largest) largest = bytes;
            if (bytes != 0) sized++;
        }
    ASSERT_TRUE(largest <= TVOUT_LIST_BYTES);
    ASSERT_EQ(sized, 320u * 480u);
    printf("largest picture list %u of %u bytes\n", largest, (unsigned)TVOUT_LIST_BYTES);
}

/* At 2x every picture up to 320x240 fits, odd widths included (the frame is
 * twice as wide), as its frame-sized picture does; nothing larger, and no
 * other scale. */
static void test_every_2x_picture_fits_the_block(void) {
    unsigned sized = 0;
    for (unsigned w = 1; w <= 322; w++)
        for (unsigned h = 1; h <= 241; h++) {
            const unsigned bytes = tvout_list_picture_bytes(w, h, 2u);
            ASSERT_TRUE((bytes != 0) == (w <= 320 && h <= 240));
            if (bytes != 0) {
                ASSERT_EQ(bytes, tvout_list_picture_bytes(2u * w, 2u * h, 1u));
                sized++;
            }
        }
    ASSERT_EQ(sized, 320u * 240u);
    ASSERT_EQ(tvout_list_picture_bytes(320, 240, 0u), 0u);
    ASSERT_EQ(tvout_list_picture_bytes(160, 120, 3u), 0u);
}

static tvout_list_config_t picture_config(void) {
    const tvout_list_config_t cfg = {
        .ctrl_words = CTRL_WORDS, .chain_bits = CHAIN_BITS, .chain_shift = CHAIN_SHIFT,
        .rewind_channel = REWIND, .fifo = FIFO, .ctrl_pixels = CTRL_PIXELS, .ctrl_copy = CTRL_COPY,
        .join_channel = JOIN, .restart_channel = RESTART, .row_read_addr = ROW_READ,
    };
    return cfg;
}

/* The row words handed out are the ones the list reads: each holds its
 * frame row's address, picture row r / scale, and the first row's is in the
 * list itself. */
static void test_picture_rows(unsigned w, unsigned h, unsigned scale) {
    const tvout_list_config_t cfg = picture_config();
    tvout_list_picture_t pic;
    memset(s_block, 0, sizeof s_block);
    ASSERT_TRUE(tvout_list_build_picture(s_block, sizeof s_block, s_pixels, w, h, scale, &cfg, &pic));
    const unsigned rows = h * scale;
    ASSERT_EQ(pic.rows, rows);
    ASSERT_TRUE(pic.joins == (rows > 1));
    ASSERT_TRUE(pic.first_row >= TVOUT_LIST_FLIP_MARGIN);
    /* The last row's piece, first_row + 2 (rows - 1), lies inside the frame. */
    ASSERT_TRUE(pic.pieces > pic.first_row + 2u * (rows - 1u));
    const uint8_t *lo = (const uint8_t *)s_block, *hi = lo + sizeof s_block;
    ASSERT_TRUE((const uint8_t *)pic.row0_read_addr >= lo && (const uint8_t *)pic.row0_read_addr < hi);
    ASSERT_EQ(*pic.row0_read_addr, (uint32_t)(uintptr_t)s_pixels);
    unsigned good = 0;
    for (unsigned r = 1; r < rows; r++) {
        const uint32_t *word = &pic.row_read_addrs[(r - 1) * 2];
        if ((const uint8_t *)word >= lo && (const uint8_t *)(word + 1) <= hi &&
            *word == (uint32_t)(uintptr_t)(s_pixels + r / scale * w))
            good++;
    }
    ASSERT_EQ(good, rows - 1);
}

static void test_picture_refusals(void) {
    const tvout_list_config_t cfg = picture_config();
    tvout_list_picture_t pic;
    ASSERT_TRUE(!tvout_list_build_picture(s_block, sizeof s_block, s_pixels, 481, 320, 1u, &cfg, &pic));
    ASSERT_TRUE(!tvout_list_build_picture(s_block, sizeof s_block, s_pixels, 480, 481, 1u, &cfg, &pic));
    ASSERT_TRUE(!tvout_list_build_picture(s_block, sizeof s_block, NULL, 480, 320, 1u, &cfg, &pic));
    ASSERT_TRUE(!tvout_list_build_picture(s_block, tvout_list_picture_bytes(480, 320, 1u) - 4u,
                                          s_pixels, 480, 320, 1u, &cfg, &pic));
    ASSERT_TRUE(!tvout_list_build_picture(s_block, sizeof s_block, s_pixels, 321, 240, 2u, &cfg, &pic));
    ASSERT_TRUE(!tvout_list_build_picture(s_block, sizeof s_block, s_pixels, 320, 241, 2u, &cfg, &pic));
    ASSERT_TRUE(!tvout_list_build_picture(s_block, sizeof s_block, s_pixels, 160, 120, 3u, &cfg, &pic));
}

/* Rows are pieces first, first + 2, ...; the window is shut from the margin
 * before the first row until the last row has been sent. */
static void test_flip_window(void) {
    const unsigned first = 126, rows = 320, m = TVOUT_LIST_FLIP_MARGIN;
    ASSERT_TRUE(tvout_list_flip_window(0, first, rows));
    ASSERT_TRUE(tvout_list_flip_window(first - m, first, rows));
    ASSERT_TRUE(!tvout_list_flip_window(first - m + 1, first, rows));
    ASSERT_TRUE(!tvout_list_flip_window(first, first, rows));
    ASSERT_TRUE(!tvout_list_flip_window(first + 2 * rows - 1, first, rows));
    ASSERT_TRUE(tvout_list_flip_window(first + 2 * rows, first, rows));
    ASSERT_TRUE(!tvout_list_flip_window(first + 1, first, 1));
    ASSERT_TRUE(tvout_list_flip_window(first + 2, first, 1));
}

int main(void) {
    test_bars_entry_count();
    test_bars_build();
    test_bars_refused_when_short();
    test_every_picture_fits_the_block();
    test_every_2x_picture_fits_the_block();
    test_picture_rows(480, 320, 1u);
    test_picture_rows(320, 240, 1u);
    test_picture_rows(640, 480, 1u);
    test_picture_rows(2, 1, 1u);
    test_picture_rows(320, 240, 2u);
    test_picture_rows(240, 160, 2u);
    test_picture_rows(1, 1, 2u);
    test_picture_refusals();
    test_flip_window();
    TEST_RETURN();
}
