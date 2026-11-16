/*
 * tvout_plug — pure settle logic for the TV hot-plug line as the
 * coprocessor reports it (UARTKBD_FLAG_HOTPLUG). A level is believed after it
 * has held for the settle time: a TV drops the line for a moment when it
 * changes input or re-reads the source. No hardware includes — host-testable.
 */
#ifndef TVOUT_PLUG_H
#define TVOUT_PLUG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TVOUT_PLUG_CONNECT_SETTLE_MS 300u
#define TVOUT_PLUG_REMOVE_SETTLE_MS  1000u

typedef enum {
    TVOUT_PLUG_NONE = 0,
    TVOUT_PLUG_CONNECTED,
    TVOUT_PLUG_REMOVED
} tvout_plug_event_t;

/* All zero is the start state: no TV, nothing settling. */
typedef struct {
    bool     connected;   /* the settled state */
    bool     settling;
    bool     pending;     /* the level settling */
    uint32_t since_ms;    /* when it was first seen */
} tvout_plug_t;

/* Feeds the line's level at now_ms. Returns the settled change, if this call
 * made one. */
tvout_plug_event_t tvout_plug_update(tvout_plug_t *plug, bool line, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* TVOUT_PLUG_H */
