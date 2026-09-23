/*
 * picpwr — power-zone control over the coprocessor link (uartkbd's UART).
 * The coprocessor sequences every switched rail on the board; rails
 * outside its boot subset stay off until an application requests them
 * (the audio codec and CAN rails are the common casualties). Protocol:
 * docs/drivers/power.md. Polled/foreground like the rest of this BSP;
 * uartkbd_init() must have run first.
 */
#ifndef PICPWR_H
#define PICPWR_H

#include <stdint.h>
#include <stdbool.h>
#include "picpwr_frame.h"

/* Send a complete power configuration. All four fields go in one atomic
 * write — the device has no partial form and no readback of the sleep or
 * wake fields, so callers must always provide complete values. Any zone
 * bit left clear in awake switches that rail OFF. Only zones 1..17 are
 * expressible: mask bits above that are reserved-zero and this API
 * clamps them out of every frame it sends (docs/drivers/power.md).
 * Rate-limited: returns false without sending
 * while a previous command's rail walk (~1 s, blocking on the device
 * side) may still be running — retry later. The cfg is cached and seeds
 * picpwr_ensure_awake(). */
bool picpwr_send(const picpwr_cfg_t *cfg);

/* Live rail state decoded from the last status frame
 * (zones 1..17 -> bits 0..16, high = powered). False until a status
 * frame has been parsed. This is the only trustworthy source of rail
 * state — other actors move rails without notice, so never trust a
 * local cache for readback. */
bool picpwr_rails(uint32_t *rails_out);

/* Convenience: ensure the given rails are enabled, additively — no rail
 * that currently reads as powered is ever cleared. Seeds the awake mask
 * from LIVE rail state (never the cache), requiring two agreeing reads so
 * an unsettled first frame cannot drop rails it failed to report; ORs in
 * zone_bits; carries the cached sleep/wake fields. Comparisons and the
 * sent mask cover zones 1..17 only — reserved bits are always zero.
 * Returns true immediately when the rails already read as on; false when
 * stable live state is unavailable or the rate limit is active (retry
 * later). */
bool picpwr_ensure_awake(uint32_t zone_bits);

/* Like picpwr_ensure_awake(), and additionally remembers the rails so
 * picpwr_task() re-asserts them if they later read as off (the device
 * changes rails on its own: sleep/wake, USB attach/detach, watchdog).
 * The primary entry point for applications: request the rails your
 * peripherals need once at startup, poll picpwr_task() from the main
 * loop, and the driver handles the rest. */
bool picpwr_keep_awake(uint32_t zone_bits);

/* Switch the given rails OFF, preserving every other live rail, and drop
 * them from the keep-awake set. Non-blocking: the request is carried out
 * by picpwr_task() once two status frames agree on the rail state and the
 * send rate limit allows it (typically 2-3 s). Quiesce the peripheral
 * first (mute the codec, stop the radio); its pins should not be left
 * driving an unpowered part. */
void picpwr_release(uint32_t zone_bits);

/* Startup power policy for a standalone app: release every app-owned rail
 * (PICPWR_ZONE_MASK_APP_OWNED: audio codec, sub-GHz/LoRa, RGB LEDs,
 * NFC/RFID) that has not been requested with picpwr_keep_awake().
 *
 * An app does not start from the documented boot-on set. It inherits the
 * rails of whatever ran before it — the stock firmware halted mid-boot by
 * `fw ramrun`, or the loader — and nothing else will ever switch them off:
 * the zone manager that does so is part of the firmware the app replaced.
 * The audio rail is the expensive one: it is inherited with the codec's
 * speaker amplifier and 5 V boost enabled and its clocks stopped, and the
 * board runs hot for as long as the app does. Call this after the app's
 * picpwr_keep_awake() requests; rails that serve the main CPU are never
 * touched. */
void picpwr_release_unused(void);

/* Power-cycle selected rails while preserving every other live rail. If a
 * selected rail is already off it remains off. Blocks through the two
 * sequencer walks while continuing to service app recovery/status input. */
bool picpwr_cycle(uint32_t zone_bits);

/* Poll from the application's main loop (cheap; acts at most once per
 * received status frame). Re-asserts rails registered with
 * picpwr_keep_awake() when they read as off — debounced over two
 * consecutive status frames and spaced by the send rate limit, so a
 * single stale snapshot or an in-progress apply never causes a storm. */
void picpwr_task(void);

#endif /* PICPWR_H */
