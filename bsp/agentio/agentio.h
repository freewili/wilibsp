/*
 * agentio — remote input injection + screen capture for agent E2E verification.
 *
 * Lets a host drive the board over the debug probe: press buttons, touch the
 * panel, type text, and capture the screen as pixels it can turn into a PNG.
 * Transport is a dedicated SEGGER RTT channel pair (see agentio_proto.h);
 * channel 0 stays exclusively DIAG().
 *
 * Usage:
 *     agentio_init();                 // after board_init() and st7796_init()
 *     agentio_bind_keyboard(&kb);     // optional; required only for TYPE
 *     for (;;) { ...app work...; agentio_task(); }
 *
 * Build switch: FW2_AGENTIO (CMake option, default ON). When 0 every entry
 * point below is an empty inline, so the calls can stay in an app
 * unconditionally and cost nothing.
 */
#ifndef AGENTIO_H
#define AGENTIO_H

#include <stddef.h>
#include <stdint.h>
#include "keyboard/fw2kb.h"

#ifndef FW2_AGENTIO
#define FW2_AGENTIO 1
#endif

#if FW2_AGENTIO

void agentio_init(void);
void agentio_task(void);
void agentio_bind_keyboard(fw2kb_t *kb);

/* Called by st7796.c only — mirrors panel writes into the shadow framebuffer. */
void agentio_shadow_note_window(int x0, int y0, int x1, int y1);
void agentio_shadow_note_pixels(const uint8_t *bytes, size_t n);

/* The shadow framebuffer: ST7796_W * ST7796_H pixels, wire order (2 bytes per
 * pixel, MSB first). NULL when the module is disabled. */
const uint8_t *agentio_shadow_fb(void);

/* Mark a region whose TIMING is the result.
 *
 * agentio is not a free observer. A capture RLE-encodes and streams the whole
 * shadow -- 307,200 bytes for the LCD -- out over RTT from the app's own main
 * loop. That cost lands BETWEEN whatever the app is timing rather than inside
 * it, so the app's own microsecond accounting misses it entirely while
 * wall-clock throughput collapses. A downstream benchmark published figures 5x
 * and 15x low this way, and the wrong numbers were written up as a hardware
 * finding before anyone noticed a `fw rtt` session had been attached.
 *
 * Wrap a timed region in these and a capture arriving mid-measurement is
 * refused with "ERR measuring" instead of silently corrupting it. Input
 * injection is unaffected -- it is cheap, and it is often how a run gets
 * started in the first place.
 *
 * Nesting is not supported: end() clears the flag regardless of depth. */
void agentio_measure_begin(void);
void agentio_measure_end(void);

#else

static inline void agentio_init(void) {}
static inline void agentio_task(void) {}
static inline void agentio_bind_keyboard(fw2kb_t *kb) { (void)kb; }
static inline void agentio_shadow_note_window(int x0, int y0, int x1, int y1)
{ (void)x0; (void)y0; (void)x1; (void)y1; }
static inline void agentio_shadow_note_pixels(const uint8_t *b, size_t n)
{ (void)b; (void)n; }
static inline const uint8_t *agentio_shadow_fb(void) { return 0; }
static inline void agentio_measure_begin(void) {}
static inline void agentio_measure_end(void) {}

#endif /* FW2_AGENTIO */

#endif /* AGENTIO_H */
