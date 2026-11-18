// bsp/cec/cec_watch.h — events the app gets from received frames that the
// device itself does not report, for cec.c and the host tests. No hardware
// and no clock: the caller hands in each frame for this board (or a
// broadcast) before the device sees it.
//
// Called from the main loop only.
#ifndef CEC_WATCH_H
#define CEC_WATCH_H
#include <stdbool.h>
#include <stdint.h>

#include "cec.h"

#ifdef __cplusplus
extern "C" {
#endif

// Nothing seen: no key held. At every start of the device. The vendor
// setting is kept.
void cec_watch_init(void);

// True: vendor frames for this board are the app's (CEC_EV_VENDOR), and the
// device does not see them. False, the state at load: the device refuses them.
void cec_watch_accept_vendor(bool on);
bool cec_watch_accepts_vendor(void);

// The device reported key key from from going down (down) or up.
void cec_watch_on_key(bool down, uint8_t key, uint8_t from);

// A frame for this board or a broadcast, before the device takes it. True
// when *event holds an event for the app; its from and to are the frame's
// initiator and destination:
// - CEC_EV_KEY_REPEAT: <User Control Pressed> again for the key the device
//   holds down, from the same device;
// - CEC_EV_ROUTING: a broadcast <Routing Change> or <Set Stream Path>;
// - CEC_EV_VENDOR, while vendor frames are accepted: <Vendor Command>,
//   <Vendor Remote Button Down>, <Vendor Remote Button Up> or <Vendor Command
//   With ID>, the frame in event->frame.
// *for_device: false when the device must not see the frame.
bool cec_watch_on_rx(const cec_frame_t *frame, cec_event_t *event, bool *for_device);

#ifdef __cplusplus
}
#endif
#endif
