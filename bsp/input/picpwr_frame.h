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
#define PICPWR_ZONE_COMPUTE     17  /* compute module — MAIN powers it; apps leave it alone (docs) */

/* Every switchable zone (1..17). Mask bits 18-20 of the same wire field
 * are not rails but board control lines (docs/drivers/power.md); an
 * app never sets them through awake/sleep. */
#define PICPWR_ZONE_MASK_ALL 0x01FFFFu

/* Control lines in the awake field, addressed like zones. The device
 * drives lines 18 and 19 to their bits in each frame it applies, so a
 * frame must repeat their current levels or it moves them:
 *   18 WIO_RST   — PIC RG3; the LoRa module reset on rev 1 boards only
 *   19 CM0_RUNPG — compute module run (low holds it in reset)
 * Zone 20 (MAIN_PWR_RST) is not written by the rail walk and is always
 * sent as zero. */
#define PICPWR_LINE_WIO_RST   18
#define PICPWR_LINE_CM0_RUNPG 19
#define PICPWR_LINE_MASK      0x060000u

static inline uint32_t picpwr_zone_bit(int zone) {
    return 1u << (zone - 1);
}

/* Full power configuration: one atomic write of all fields. The device
 * offers no readback of sleep/wake — callers must send complete values
 * every time (see docs/drivers/power.md). awake and sleep carry zones
 * 1..17 only; lines carries the control-line levels (PICPWR_LINE_MASK
 * bits) placed in the awake field. picpwr_send() fills lines from live
 * status, so a caller's value never reaches the wire through it. */
typedef struct {
    uint32_t awake;   /* 24-bit zone mask: rails enabled while awake */
    uint32_t sleep;   /* 24-bit zone mask: rails kept through sleep */
    uint8_t  wake;    /* wake-source field */
    uint8_t  wake2;   /* wake-source overflow field */
    uint32_t lines;   /* control lines 18-19 (PICPWR_LINE_MASK), high = released */
} picpwr_cfg_t;

/* Serialize a power-zone command frame into out[PICPWR_FRAME_LEN].
 * Returns the frame length. */
size_t picpwr_frame_build(uint8_t out[PICPWR_FRAME_LEN],
                          const picpwr_cfg_t *cfg);

/* Decode live rail state (zones 1..17 -> bits 0..16) from a 20-byte
 * coprocessor status payload (the bytes between sync/id and checksum).
 * High = rail powered. */
uint32_t picpwr_rails_decode(const uint8_t payload[20]);

/* Decode the live control-line levels (zones 18 and 19 -> bits 17 and 18,
 * high = released) from the same status payload. Zone 20 is not
 * decoded: its readback stays high while the correct command is zero. */
uint32_t picpwr_lines_decode(const uint8_t payload[20]);

/* Control-line levels for the next frame, from the last two distinct
 * status frames (prev, cur) and the awake mask being sent. A line read
 * high in either frame is kept high: a spurious high leaves a line where
 * MAIN put it, while a spurious low (a glitch, or the module's own
 * run/power-good net dipping) would reset the part. CM0_RUNPG is only
 * carried high while the same frame keeps zone 17 on, so a frame never
 * drives the run line into an unpowered module. */
static inline uint32_t picpwr_lines_carry(uint32_t prev, uint32_t cur,
                                          uint32_t awake) {
    uint32_t lines = (prev | cur) & PICPWR_LINE_MASK;
    if (!(awake & picpwr_zone_bit(PICPWR_ZONE_COMPUTE)))
        lines &= ~picpwr_zone_bit(PICPWR_LINE_CM0_RUNPG);
    return lines;
}

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
