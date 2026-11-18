// bsp/display/tvout_ring_core.cpp — see tvout_ring_core.h.
#include "tvout_ring_core.h"
#include "rpDVIMirrorCore.h"

static_assert(TVOUT_RING_ROWS == DVI_MIRROR_RING_ROWS, "ring sizes differ");
static_assert(TVOUT_RING_READ_AHEAD == DVI_MIRROR_READ_AHEAD, "read-ahead differs");

namespace {

stDVIMirrorCore s_core;

}

void tvout_ring_core_init(uint32_t rows, uint32_t first_row_piece, uint32_t pieces)
{
    mirrorInit(s_core, rows, first_row_piece, pieces);
}

uint32_t tvout_ring_core_slot(uint32_t row)
{
    return mirrorRingSlot(row);
}

uint32_t tvout_ring_core_reader_begin(uint32_t next_piece, uint32_t *first_row)
{
    if (!s_core.bReading)
        return 0;
    return mirrorReaderBegin(s_core, next_piece, *first_row);
}

void tvout_ring_core_reader_fetched(uint32_t row, uint32_t next_piece)
{
    mirrorReaderFetched(s_core, row, next_piece);
}

uint32_t tvout_ring_core_reader_end(uint32_t first_row, uint32_t fetched)
{
    return mirrorReaderEnd(s_core, first_row, fetched);
}

bool tvout_ring_core_reading(void)
{
    return s_core.bReading;
}

bool tvout_ring_core_switch_start(uint32_t next_piece)
{
    return mirrorSwitchStart(s_core, next_piece);
}

bool tvout_ring_core_switch_ready(uint32_t (*next_piece)(void))
{
    return mirrorSwitchReady(s_core, next_piece);
}

void tvout_ring_core_switch_done(void)
{
    mirrorSwitchDone(s_core);
}

void tvout_ring_core_release(uint32_t next_piece)
{
    mirrorRelease(s_core, next_piece);
}

uint32_t tvout_ring_core_late(void)
{
    return s_core.iLate;
}
