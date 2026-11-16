// bsp/cec/cec_place.h — this board's place in the TV's connection tree: the
// DISPLAY firmware's record of it, its text form on the SD card, and learning
// it from the routes the TV announces. For cec.c, cec_saved_onewili.h and the
// host tests. No hardware and no clock.
//
// Called from the main loop only.
#ifndef CEC_PLACE_H
#define CEC_PLACE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cec.h"

#ifdef __cplusplus
extern "C" {
#endif

// The DISPLAY firmware's app-info record, written by its app launcher into
// watchdog scratch registers just before the jump to an app; these values are
// the firmware's and do not change: scratch[0] the magic, its low byte the layout
// version; scratch[1] the physical address in bits 0..15, its complement in
// bits 16..31, 0xFFFF when the firmware did not know it.
#define CEC_PLACE_INFO_SCRATCH       0
#define CEC_PLACE_INFO_ADDR_SCRATCH  1
#define CEC_PLACE_INFO_MAGIC         0xCEC0A101u

// True, with *phys_addr, when the two words hold a record with a known address.
bool cec_place_from_firmware(uint32_t magic_word, uint32_t addr_word, uint16_t *phys_addr);

// "A.B.C.D", hex digits, into out (at least 8 bytes).
void cec_place_format(uint16_t phys_addr, char *out);
// The address of "A.B.C.D" (hex digits 0..F; leading and trailing white space
// allowed), in text of len bytes. False for anything else, and for 0.0.0.0
// (the TV's own) and F.F.F.F.
bool cec_place_parse(const char *text, size_t len, uint16_t *phys_addr);

// How long a route the TV announced waits for the device there to answer
// <Active Source> or <Routing Information>: the second a follower has (CEC
// 9.2), and the time the answer itself takes to get onto the wire.
#define CEC_PLACE_ANSWER_MS 2000u

// Nothing waiting.
void cec_place_learn_reset(void);

// A frame for this board or a broadcast, own the address in use. A <Routing
// Change> or <Set Stream Path> from the TV to an address but own starts a
// wait; <Active Source> or <Routing Information> naming that address, or one
// below it, ends the wait: a device there answered.
void cec_place_on_rx(const cec_frame_t *frame, uint16_t own, uint32_t now_ms);

// True once, with *phys_addr, when the wait ran out unanswered: the TV is
// showing an input no other device holds.
bool cec_place_learned(uint32_t now_ms, uint16_t *phys_addr);

#ifdef __cplusplus
}
#endif
#endif
