// bsp/display/tvout_ring.h — the line ring that shows a PSRAM picture
// (tvout_start_psram()). Every picture row of the scan-out list reads one of
// eight SRAM line buffers; a TIMER1 interrupt at top priority on the core that
// started video keeps the buffers up to six rows ahead of the TV, fetching
// each row from PSRAM through the XIP streaming FIFO with one DMA channel that
// writes SRAM only. Core 1, once registered with tvout_core1_init(), is held
// in an interrupt for each row read. Row bookkeeping: tvout_ring_core.h; how
// tvout.c drives it: tvout_ring_ops_t in tvout_internal.h.
#ifndef TVOUT_RING_H
#define TVOUT_RING_H

// Line buffer width; a wider PSRAM picture is refused. A target may define a
// larger value (at most 640); the ring takes 16 bytes per pixel of width.
#ifndef TVOUT_RING_WIDTH_MAX
#define TVOUT_RING_WIDTH_MAX 480
#endif

#endif
