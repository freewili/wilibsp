// bsp/display/tvout_ring_core.h — C entry points to the line ring's row
// bookkeeping (rpDVIMirrorCore.h) for tvout_ring.c. No hardware and no clock:
// the caller reads the scan position (the frame piece after the one being
// sent, rpDVIList's numbering) and carries out what these functions decide.
// One ring; its state is held in this module.
#ifndef TVOUT_RING_CORE_H
#define TVOUT_RING_CORE_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Line buffers in the ring, and rows the reader keeps ahead of the TV.
#define TVOUT_RING_ROWS        8u
#define TVOUT_RING_READ_AHEAD  6u

// rows: picture rows. first_row_piece: the piece of picture row 0.
// pieces: pieces per frame. Clears the counts.
void tvout_ring_core_init(uint32_t rows, uint32_t first_row_piece, uint32_t pieces);

// The line buffer that holds row.
uint32_t tvout_ring_core_slot(uint32_t row);

// The reader's tick, in an interrupt: begin() returns how many rows to fetch
// now, from *first_row up; fetched() follows each row once it is in its line
// buffer, with the scan position read after the fetch; end() follows, whatever
// the count, and returns the microseconds to the next tick. begin() returns 0
// while the reader is not running.
uint32_t tvout_ring_core_reader_begin(uint32_t next_piece, uint32_t *first_row);
void tvout_ring_core_reader_fetched(uint32_t row, uint32_t next_piece);
uint32_t tvout_ring_core_reader_end(uint32_t first_row, uint32_t fetched);
bool tvout_ring_core_reading(void);

// Starting the reader with every row already pointed at its line buffer,
// outside the interrupt. start(), repeated until true, gives the reader its
// first row; the caller then raises the first tick. ready(), repeated until
// true, says the rows the TV reads next are buffered; next_piece is called
// between two ticks. done() starts counting late rows.
bool tvout_ring_core_switch_start(uint32_t next_piece);
bool tvout_ring_core_switch_ready(uint32_t (*next_piece)(void));
void tvout_ring_core_switch_done(void);

// Stops the reader at its next tick and counts the rows the TV has reached
// unfetched.
void tvout_ring_core_release(uint32_t next_piece);

// Rows the TV reached before they were fetched, since init.
uint32_t tvout_ring_core_late(void);

#ifdef __cplusplus
}
#endif
#endif
