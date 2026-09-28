/*
 * picpwr_frame — pure frame building and status decoding for the
 * coprocessor power-zone command. Protocol: docs/drivers/power.md.
 * No hardware includes — host-testable.
 */
#ifndef PICPWR_FRAME_H
#define PICPWR_FRAME_H

#include <stdint.h>
#include <stddef.h>

/* Power-zone command frame: sync, id, 8 payload bytes, checksum. */
#define PICPWR_FRAME_LEN 11

/* Zone assignments (zone N = mask bit N-1). Cautions and the boot-on
 * set are documented in docs/drivers/power.md — read the zone map
 * before switching anything OFF. */
#define PICPWR_ZONE_SENSORS     1   /* sensors + an I/O expander */
#define PICPWR_ZONE_DISPLAY     2   /* LCD panel + touch controller */
#define PICPWR_ZONE_AUDIO       3   /* audio codec */
#define PICPWR_ZONE_SUBGHZ      4   /* sub-GHz radio (shared rail; docs) */
#define PICPWR_ZONE_WIFI_BT     5   /* Wi-Fi / Bluetooth module */
#define PICPWR_ZONE_FPGA        6   /* FPGA + external memory */
#define PICPWR_ZONE_SDCARD      7   /* microSD + bridge (unmount first) */
#define PICPWR_ZONE_USB_HUB     8   /* USB hub (device-managed on attach) */
#define PICPWR_ZONE_STATUS_LED  9   /* flag, not a rail: status-LED enable */
#define PICPWR_ZONE_RGB_LEDS    10  /* addressable RGB LEDs */
#define PICPWR_ZONE_ANALOG      11  /* DAC, ADC, op-amps */
#define PICPWR_ZONE_AUX_DISPLAY 12  /* function not established; leave alone */
#define PICPWR_ZONE_NFC_RFID    13  /* NFC + low-frequency RFID */
#define PICPWR_ZONE_USB_SERIAL  14  /* USB-serial bridge (device-managed) */
#define PICPWR_ZONE_CAN         15  /* CAN controller + transceiver */
#define PICPWR_ZONE_DEBUG_PROBE 16  /* on-board debug probe */
#define PICPWR_ZONE_COMPUTE     17  /* compute module — not usable here (docs) */

/* Every expressible zone (1..17). Mask bits above this are RESERVED —
 * MUST BE ZERO (docs/drivers/power.md). picpwr_frame_build() clears
 * them from every frame; nothing in this API can set them. */
#define PICPWR_ZONE_MASK_ALL 0x01FFFFu

static inline uint32_t picpwr_zone_bit(int zone) {
    return 1u << (zone - 1);
}

/* Full power configuration: one atomic write of all four fields. The
 * device offers no readback of sleep/wake — callers must send complete
 * values every time (see docs/drivers/power.md). Mask bits above zone 17
 * are reserved-zero and stripped at serialization. */
typedef struct {
    uint32_t awake;   /* 24-bit zone mask: rails enabled while awake */
    uint32_t sleep;   /* 24-bit zone mask: rails kept through sleep */
    uint8_t  wake;    /* wake-source field */
    uint8_t  wake2;   /* wake-source overflow field */
} picpwr_cfg_t;

/* Serialize a power-zone command frame into out[PICPWR_FRAME_LEN].
 * Returns the frame length. */
size_t picpwr_frame_build(uint8_t out[PICPWR_FRAME_LEN],
                          const picpwr_cfg_t *cfg);

/* Decode live rail state (zones 1..17 -> bits 0..16) from a 20-byte
 * coprocessor status payload (the bytes between sync/id and checksum).
 * High = rail powered. */
uint32_t picpwr_rails_decode(const uint8_t payload[20]);

/* Awake mask for re-asserting rails after a kept rail reads off.
 *
 * A re-assert MUST be a superset of what was already requested, never a
 * mask rebuilt from a status snapshot: any zone bit left clear in awake
 * switches that rail OFF, so echoing a snapshot back switches off every
 * rail it failed to report (docs/drivers/power.md, "Usage guidance").
 * ORing the last-sent mask in makes a re-assert incapable of clearing
 * anything that was previously asked for.
 *
 *   cached_awake — awake mask of the last cfg actually sent
 *   rails        — live rail state from the status frame
 *   desired      — accumulated picpwr_keep_awake() requests
 */
static inline uint32_t picpwr_reassert_awake(uint32_t cached_awake,
                                            uint32_t rails,
                                            uint32_t desired) {
    return (cached_awake | rails | desired) & PICPWR_ZONE_MASK_ALL;
}

/* Rails whose only user is the display CPU: the audio codec, the sub-GHz /
 * LoRa radios, the RGB LEDs and NFC/RFID. A standalone app is the display
 * CPU's whole firmware, so a rail in this set that the app has not asked
 * for has no user at all. The other default-off rails (Wi-Fi/BT, FPGA,
 * analog, CAN) serve the main CPU, whose needs an app cannot see, and are
 * never released on inference. */
#define PICPWR_ZONE_MASK_APP_OWNED                                   \
    ((1u << (PICPWR_ZONE_AUDIO - 1)) | (1u << (PICPWR_ZONE_SUBGHZ - 1)) | \
     (1u << (PICPWR_ZONE_RGB_LEDS - 1)) | (1u << (PICPWR_ZONE_NFC_RFID - 1)))

/* Awake mask for switching rails OFF: live state plus every kept rail,
 * minus the rails being released. A kept rail (desired) is never cleared,
 * whatever release says.
 *
 *   rails    — live rail state from the status frame
 *   desired  — accumulated picpwr_keep_awake() requests
 *   release  — rails to switch off
 */
static inline uint32_t picpwr_release_awake(uint32_t rails,
                                           uint32_t desired,
                                           uint32_t release) {
    return ((rails & ~release) | desired) & PICPWR_ZONE_MASK_ALL;
}

#endif /* PICPWR_FRAME_H */
