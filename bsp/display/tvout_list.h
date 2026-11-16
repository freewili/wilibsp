// bsp/display/tvout_list.h — C entry points to the scan-out list builder
// (rpDVIFrame, rpDVIList) for tvout.c. No hardware access: the DMA control
// word, channel number and FIFO address the entries carry are given.
#ifndef TVOUT_LIST_H
#define TVOUT_LIST_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One list entry: the four words the list channel writes into the pixel
// channel's control, write address, transfer count and read address
// registers. The last of the four starts the pixel channel.
typedef struct {
    uint32_t ctrl;
    uint32_t write_addr;
    uint32_t count;
    uint32_t read_addr;
} tvout_list_entry_t;

// Entries of the colour-bar frame: 640x480 at 60 Hz, either framing.
#define TVOUT_LIST_BARS_ENTRIES 525u

// Bytes of the one block that holds either frame's list: the colour bars'
// entries (8400), or a picture's entries, scan runs and row table. The
// largest picture list, 8568 bytes, rounded up to a whole entry; every
// picture that fits the video mode fits it (tests/test_tvout_list.c).
#define TVOUT_LIST_BYTES 8576u

// Scan pieces that must still come before the first picture row for a page
// flip to start (tvout_list_flip_window()).
#define TVOUT_LIST_FLIP_MARGIN 3u

typedef struct {
    uint32_t ctrl_words;      // pixel channel: 32-bit words to the FIFO, paced by HSTX, then the list channel
    uint32_t chain_bits;      // the control word's chain field
    uint32_t chain_shift;
    uint32_t rewind_channel;  // the last entry chains to it
    uint32_t fifo;            // the HSTX FIFO's address
    // Picture only.
    uint32_t ctrl_pixels;     // pixel channel: 16-bit pixels to the FIFO, paced by HSTX, then the list channel
    uint32_t ctrl_copy;       // pixel channel: one word, unpaced, then the list channel
    uint32_t join_channel;    // sends the join after each picture row
    uint32_t restart_channel; // puts the join channel back at the ring's first slot
    uint32_t row_read_addr;   // the row channel's READ_ADDR register
} tvout_list_config_t;

// A built picture list: what tvout.c needs to start the channels, read the
// scan position and re-point the rows.
typedef struct {
    bool joins;                       // the join, row and restart channels run
    uint32_t join_start;              // join channel: first read address
    const uint32_t *join_start_word;  // the word the restart channel copies
    unsigned join_words;              // join channel: transfers per join
    uint32_t row_start;               // row channel: first read address (the row table)
    unsigned rows_loaded_at;          // list entries loaded while the row channel walks the rows
    unsigned first_row;               // scan piece of the first picture row
    unsigned rows;                    // frame rows: picture rows times the scale
    unsigned pieces;                  // scan pieces per frame
    // Row 0's read address is *row0_read_addr; row r >= 1's is
    // row_read_addrs[(r - 1) * 2]. Both may be rewritten while video runs.
    uint32_t *row0_read_addr;
    uint32_t *row_read_addrs;
} tvout_list_picture_t;

// Entries the colour-bar frame needs.
unsigned tvout_list_bars_entries(void);

// Builds the colour-bar frame into list[0..entries). The entries read words
// held in this module, which stay valid until the next build. False if the
// frame does not fit.
bool tvout_list_build_bars(tvout_list_entry_t *list, unsigned entries,
                           const tvout_list_config_t *cfg);

// Bytes of the list block a width x height picture shown at scale (1, or 2
// for twice its size) needs with the framing tvout_list_set_audio_framing()
// set, the pool not counted; 0 for another scale or if the picture at that
// scale does not fit the video mode (wider than 640, taller than 480, or an
// odd width, which cannot be centred).
unsigned tvout_list_picture_bytes(unsigned width, unsigned height, unsigned scale);

// Builds the list of a width x height picture read in place from pixels (row
// y at pixels + y * width) into block, bytes long and word-aligned, shown at
// scale. At scale 2 the frame has 2 * height rows, row r reading picture row
// r / 2 with width pixels, and the HSTX encoder must send each pixel twice.
// False if the picture does not fit the video mode or the block.
bool tvout_list_build_picture(void *block, unsigned bytes, const uint16_t *pixels,
                              unsigned width, unsigned height, unsigned scale,
                              const tvout_list_config_t *cfg, tvout_list_picture_t *out);

// The scan position of the last built picture: the frame piece after the one
// being sent, from the entries the list channel has loaded, the pixel
// channel's transfers left and the row entries the row channel has loaded.
unsigned tvout_list_scan_piece(uint32_t loaded, uint32_t left, uint32_t rows_loaded);

// True when every row read address can be rewritten at scan piece `piece`
// without the frame being sent mixing two pictures: the last row has been
// sent, or at least TVOUT_LIST_FLIP_MARGIN pieces remain before the first row.
bool tvout_list_flip_window(unsigned piece, unsigned first_row, unsigned rows);

// Pixel clock of the video mode, kHz (25200).
unsigned tvout_list_pixel_clock_khz(void);

// Pool words audio framing needs: 990 for the colour bars, at most 1310
// for any picture that fits the video mode (tests/test_tvout_audio.cpp).
#define TVOUT_LIST_AUDIO_POOL_WORDS 1310u

// Audio framing for the builds that follow: pool, words long, holds the lines
// that carry a data island and outlives every list built from it; NULL is DVI
// framing, the default. A picture's list is shorter with audio framing (lines
// in the pool go out as one entry).
void tvout_list_set_audio_framing(uint32_t *pool, unsigned words);

#ifdef __cplusplus
}
#endif
#endif
