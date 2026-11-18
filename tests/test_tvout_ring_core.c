/* Host tests for bsp/display/tvout_ring_core: the C entry points tvout_ring.c
 * calls, built from C against the C++ row logic (rpDVIMirrorCore.h). The
 * logic itself is simulated in test_dvi_mirror.cpp. */
#include <stdint.h>
#include <stdio.h>
#include "test_util.h"
#include "display/tvout_ring_core.h"

/* A frame of 40 pieces before row 0, 10 rows of two pieces each, 30 after. */
#define FIRST  40u
#define ROWS   10u
#define PIECES (FIRST + 2u * ROWS + 30u)

static uint32_t s_piece;
static uint32_t scan(void) { return s_piece; }

static void test_slots(void) {
    for (uint32_t row = 0; row < 3u * TVOUT_RING_ROWS; row++)
        ASSERT_EQ(tvout_ring_core_slot(row), row % TVOUT_RING_ROWS);
}

/* The always-on start with the TV above the picture: the reader takes row 0
 * first, fetches up to the read-ahead in ticks of at most three rows, and the
 * start is ready once rows 0 to 3 are in. */
static void test_start_above_picture(void) {
    tvout_ring_core_init(ROWS, FIRST, PIECES);
    uint32_t first = 99;
    ASSERT_EQ(tvout_ring_core_reader_begin(5, &first), 0u);   /* not running */

    s_piece = 5;
    ASSERT_TRUE(tvout_ring_core_switch_start(s_piece));
    ASSERT_TRUE(tvout_ring_core_reading());
    ASSERT_TRUE(!tvout_ring_core_switch_ready(scan));

    uint32_t n = tvout_ring_core_reader_begin(s_piece, &first);
    ASSERT_EQ(first, 0u);
    ASSERT_EQ(n, 3u);
    for (uint32_t i = 0; i < n; i++) tvout_ring_core_reader_fetched(first + i, s_piece);
    ASSERT_EQ(tvout_ring_core_reader_end(first, n), 8u);      /* the gap after a fetching tick */
    ASSERT_TRUE(!tvout_ring_core_switch_ready(scan));

    n = tvout_ring_core_reader_begin(s_piece, &first);
    ASSERT_EQ(first, 3u);
    ASSERT_EQ(n, TVOUT_RING_READ_AHEAD - 3u);
    for (uint32_t i = 0; i < n; i++) tvout_ring_core_reader_fetched(first + i, s_piece);
    tvout_ring_core_reader_end(first, n);
    ASSERT_TRUE(tvout_ring_core_switch_ready(scan));
    tvout_ring_core_switch_done();

    n = tvout_ring_core_reader_begin(s_piece, &first);
    ASSERT_EQ(n, 0u);
    ASSERT_EQ(tvout_ring_core_reader_end(first, n), 30u);     /* nothing to fetch: idle gap */
    ASSERT_EQ(tvout_ring_core_late(), 0u);
}

/* A TV that reaches rows before the reader counts them late, and release
 * stops the reader. */
static void test_late_and_release(void) {
    uint32_t first = 0;
    s_piece = FIRST + 2u * 7u + 1u;                            /* on row 7 */
    const uint32_t n = tvout_ring_core_reader_begin(s_piece, &first);
    ASSERT_EQ(first, 8u);
    ASSERT_TRUE(tvout_ring_core_late() == 2u);                 /* rows 6 and 7 */
    tvout_ring_core_reader_end(first, n);
    tvout_ring_core_release(s_piece);
    ASSERT_TRUE(!tvout_ring_core_reading());
    ASSERT_EQ(tvout_ring_core_reader_begin(s_piece, &first), 0u);
}

int main(void) {
    test_slots();
    test_start_above_picture();
    test_late_and_release();
    TEST_RETURN();
}
