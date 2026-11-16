// bsp/display/tvout_internal.h — calls between tvout.c and tvout_audio.c.
// Not for apps.
#ifndef TVOUT_INTERNAL_H
#define TVOUT_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>
#include "display/tvout.h"
#include "display/tvout_list.h"

#ifdef __cplusplus
extern "C" {
#endif

// tvout.c: the audio framing pool for the starts that follow; NULL is DVI
// framing.
void tvout_set_framing_pool(uint32_t *pool, unsigned words);

// tvout.c: the running picture was built with audio framing.
bool tvout_framed(void);

// tvout.c: the scan position while video runs, the frame piece after the one
// being sent; 0 while stopped. Runs from SRAM, for interrupts.
uint32_t tvout_scan_entry(void);

// The line ring (tvout_ring.c), as tvout.c drives it for a PSRAM picture, in
// this order: open() before the list is built (from lines()); attach() after
// the build and before the channels start; run() after they start; close()
// after they stop and before they are freed (nothing when not open).
typedef struct {
    // Checks the picture and takes the ring's alarms and channel. False with
    // error() set.
    bool (*open)(const uint16_t *pixels, unsigned width, unsigned height);
    // The line buffers, row stride width: an SRAM address to build the list from.
    const uint16_t *(*lines)(void);
    // Points every row of the list at its line buffer. scan_piece is called
    // from the reader's interrupt: SRAM-resident, never blocking.
    void (*attach)(const tvout_list_picture_t *pic, uint32_t (*scan_piece)(void));
    // Starts the reader and waits until the rows the TV reads next are
    // buffered. False with error() set.
    bool (*run)(void);
    void (*close)(void);
    bool (*is_open)(void);
    const char *(*error)(void);
    // Puts the reader's counts in *out.
    void (*stats)(tvout_stats_t *out);
} tvout_ring_ops_t;

// tvout.c: tvout_start() for a picture read through ring.
bool tvout_start_ring(const uint16_t *pixels, int width, int height, const tvout_ring_ops_t *ring);

#ifdef __cplusplus
}
#endif
#endif
