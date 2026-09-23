#include "usb_msc.h"
#include "usb_core.h"
#include "usb_hub.h"
#include "usb_parse.h"
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"

typedef enum { ST_DISCONNECTED, ST_HUB_WAIT, ST_READY, ST_FAILED } msc_state_t;

static msc_state_t  state = ST_DISCONNECTED;
static usb_device_t drive;          // the MSC device (addr 1 direct, 2 via hub)
static bool         via_hub;
static uint8_t      toggle_in, toggle_out;
static uint32_t     cbw_tag = 1;
static uint32_t     block_count_, block_size_;
static uint32_t     last_sense;
static char         drive_name[29];

uint32_t usb_msc_block_count(void) { return block_count_; }
uint32_t usb_msc_block_size(void)  { return block_size_; }
bool     usb_msc_ready(void)       { return state == ST_READY; }
uint32_t usb_msc_last_sense(void)  { return last_sense; }
const char *usb_msc_drive_name(void) { return drive_name; }

// ---------------------------------------------------------------------------
// BOT: CBW -> data -> CSW, with tier-1 (stall) recovery
// ---------------------------------------------------------------------------

// Returns MSC_OK with *csw_status filled (0 passed / 1 failed), or transport-
// level errors. Tier-2/3 recovery is the caller's job (Task 9).
static msc_result_t bot_command(const uint8_t *cb, uint8_t cb_len, bool dir_in,
                                void *data, uint32_t data_len,
                                uint8_t *csw_status, uint32_t timeout_ms) {
    uint8_t cbw[MSC_CBW_LEN], csw[MSC_CSW_LEN];
    uint32_t tag = cbw_tag++;
    uint32_t actual;
    msc_build_cbw(cbw, tag, data_len, dir_in, 0, cb, cb_len);

    hcd_result_t r = hcd_bulk_xfer(drive.addr, drive.cfg.bulk_out, &toggle_out,
                                   cbw, MSC_CBW_LEN, &actual, 1000);
    if (r == HCD_ERR_DISCONNECT) return MSC_DISCONNECTED;
    if (r != HCD_OK) return MSC_TRANSPORT_ERROR;

    if (data_len) {
        uint8_t ep = dir_in ? drive.cfg.bulk_in : drive.cfg.bulk_out;
        uint8_t *tog = dir_in ? &toggle_in : &toggle_out;
        r = hcd_bulk_xfer(drive.addr, ep, tog, data, data_len, &actual, timeout_ms);
        if (r == HCD_ERR_DISCONNECT) return MSC_DISCONNECTED;
        if (r == HCD_ERR_STALL) {
            // Tier 1: device rejected the data phase. Clear the halt and
            // fall through to read the CSW, which explains why.
            if (core_clear_endpoint_halt(drive.addr, ep) != HCD_OK)
                return MSC_TRANSPORT_ERROR;
            *tog = 0;
        } else if (r != HCD_OK) {
            return MSC_TRANSPORT_ERROR;
        }
    }

    r = hcd_bulk_xfer(drive.addr, drive.cfg.bulk_in, &toggle_in,
                      csw, MSC_CSW_LEN, &actual, 1000);
    if (r == HCD_ERR_STALL) {
        // CSW itself stalled: clear halt, retry CSW once (BOT spec figure 2)
        if (core_clear_endpoint_halt(drive.addr, drive.cfg.bulk_in) != HCD_OK)
            return MSC_TRANSPORT_ERROR;
        toggle_in = 0;
        r = hcd_bulk_xfer(drive.addr, drive.cfg.bulk_in, &toggle_in,
                          csw, MSC_CSW_LEN, &actual, 1000);
    }
    if (r == HCD_ERR_DISCONNECT) return MSC_DISCONNECTED;
    if (r != HCD_OK || actual != MSC_CSW_LEN) return MSC_TRANSPORT_ERROR;

    if (!msc_parse_csw(csw, tag, csw_status)) return MSC_TRANSPORT_ERROR;
    if (*csw_status == 2) return MSC_TRANSPORT_ERROR;   // phase error -> tier 2
    return MSC_OK;
}

// ---------------------------------------------------------------------------
// SCSI bring-up
// ---------------------------------------------------------------------------

static msc_result_t scsi_request_sense(void) {
    uint8_t cb[6] = {0x03, 0, 0, 0, 18, 0};
    uint8_t sense[18] = {0};
    uint8_t st;
    msc_result_t r = bot_command(cb, 6, true, sense, 18, &st, 1000);
    if (r != MSC_OK) return r;
    last_sense = ((uint32_t)(sense[2] & 0x0F) << 16)   // sense key
               | ((uint32_t)sense[12] << 8)            // ASC
               | sense[13];                            // ASCQ
    return MSC_OK;
}

static msc_result_t scsi_bringup(void) {
    uint8_t st;

    // INQUIRY (36 bytes): identity string
    uint8_t inq_cb[6] = {0x12, 0, 0, 0, 36, 0};
    uint8_t inq[36] = {0};
    msc_result_t r = bot_command(inq_cb, 6, true, inq, 36, &st, 2000);
    if (r != MSC_OK) return r;
    if (st != 0) return MSC_MEDIA_ERROR;
    memcpy(drive_name, inq + 8, 28);
    drive_name[28] = '\0';

    // TEST UNIT READY until the drive comes up (some sticks need ~1 s)
    uint8_t tur_cb[6] = {0};
    bool ready = false;
    for (int i = 0; i < 30; i++) {
        r = bot_command(tur_cb, 6, false, NULL, 0, &st, 1000);
        if (r != MSC_OK) return r;
        if (st == 0) { ready = true; break; }
        r = scsi_request_sense();                 // required after CHECK CONDITION
        if (r != MSC_OK) return r;
        sleep_ms(100);
    }
    if (!ready) return MSC_NOT_READY;

    // READ CAPACITY (10): last LBA + block size, big-endian
    uint8_t cap_cb[10] = {0x25, 0};
    uint8_t cap[8];
    r = bot_command(cap_cb, 10, true, cap, 8, &st, 2000);
    if (r != MSC_OK) return r;
    if (st != 0) return MSC_MEDIA_ERROR;
    block_count_ = (((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) |
                    ((uint32_t)cap[2] << 8) | cap[3]) + 1;
    block_size_  = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) |
                   ((uint32_t)cap[6] << 8) | cap[7];
    return MSC_OK;
}

// ---------------------------------------------------------------------------
// Hotplug / enumeration state machine
// ---------------------------------------------------------------------------

static absolute_time_t failed_retry_at;
static absolute_time_t hub_poll_at;   // ST_HUB_WAIT: throttle usb_hub_poll_drive() calls

static void teardown(void) {
    state = ST_DISCONNECTED;
    block_count_ = block_size_ = 0;
    toggle_in = toggle_out = 0;
    last_sense = 0;
    if (via_hub) usb_hub_detach();
    via_hub = false;
}

// Failed enumerations retry every 2 s while the device stays attached, so a
// drive plugged into an already-enumerated hub gets picked up.
static void enter_failed(void) {
    state = ST_FAILED;
    failed_retry_at = make_timeout_time_ms(2000);
}

void usb_msc_init(void) {
    fw2_native_usb_hcd_init();
    teardown();
}

void usb_msc_task(void) {
    switch (state) {
    case ST_DISCONNECTED: {
        if (hcd_port_speed() != HCD_SPEED_FULL) return;
        sleep_ms(100);                            // attach debounce
        hcd_bus_reset();
        usb_device_t dev;
        hcd_result_t er = core_enumerate(1, &dev);
        if (er != HCD_OK) {
            printf("msc: enumeration failed (%d)\n", (int)er);
            enter_failed();
            return;
        }

        if (dev.cfg.is_hub) {
            // Hub passthrough (Task 10): find a drive on a hub port,
            // reset it, enumerate it at address 2. Polled non-blockingly
            // from ST_HUB_WAIT below -- don't block the caller here (this
            // stalled the whole app, e.g. an LVGL+touch loop, for up to 5s
            // at a time whenever a hub was attached with nothing plugged
            // into it).
            if (usb_hub_attach(&dev) != HCD_OK) {
                printf("msc: hub attach failed\n");
                enter_failed();
                return;
            }
            hub_poll_at = make_timeout_time_ms(50);
            state = ST_HUB_WAIT;
            return;
        } else if (dev.cfg.is_msc) {
            drive = dev;
            via_hub = false;
        } else {
            printf("msc: device %04x:%04x is neither MSC nor hub\n", dev.vid, dev.pid);
            enter_failed();
            return;
        }
        toggle_in = toggle_out = 0;
        msc_result_t br = scsi_bringup();
        if (br == MSC_OK) {
            state = ST_READY;
        } else {
            printf("msc: scsi bring-up failed (%d), sense=%06lx\n",
                   (int)br, (unsigned long)last_sense);
            enter_failed();
        }
        return;
    }
    case ST_HUB_WAIT:
        // The CH334F hub is always on-board, so "attached with nothing
        // plugged into it" is the normal idle state, not a failure -- stay
        // here indefinitely (just watching port status at a throttled ~50 ms
        // cadence) instead of tearing down and re-doing the whole bus
        // reset/enumerate/hub-attach sequence every retry, which is what
        // caused the remaining periodic ~100-300 ms main-loop stalls.
        if (hcd_port_speed() == HCD_SPEED_NONE) { teardown(); return; }
        if (!time_reached(hub_poll_at)) return;
        hub_poll_at = make_timeout_time_ms(50);
        if (usb_hub_poll_drive(&drive)) {
            via_hub = true;
            toggle_in = toggle_out = 0;
            msc_result_t br = scsi_bringup();
            if (br == MSC_OK) {
                state = ST_READY;
            } else {
                printf("msc: scsi bring-up failed (%d), sense=%06lx\n",
                       (int)br, (unsigned long)last_sense);
                enter_failed();
            }
        }
        return;
    case ST_READY:
        if (hcd_port_speed() == HCD_SPEED_NONE) { teardown(); return; }
        if (via_hub && !usb_hub_drive_present()) { teardown(); return; }
        return;
    case ST_FAILED:
        if (hcd_port_speed() == HCD_SPEED_NONE) { teardown(); return; }
        if (time_reached(failed_retry_at)) teardown();   // retry from scratch
        return;
    }
}

// ---------------------------------------------------------------------------
// Tier-2 recovery: Bulk-Only Mass Storage Reset + clear both halts
// ---------------------------------------------------------------------------

static bool bot_reset_recovery(void) {
    // Class request: bmRequestType 0x21, bRequest 0xFF, wIndex = interface
    const uint8_t setup[8] = {0x21, 0xFF, 0, 0, drive.cfg.msc_itf, 0, 0, 0};
    if (hcd_control_xfer(drive.addr, setup, NULL, NULL) != HCD_OK) return false;
    if (core_clear_endpoint_halt(drive.addr, drive.cfg.bulk_in) != HCD_OK) return false;
    if (core_clear_endpoint_halt(drive.addr, drive.cfg.bulk_out) != HCD_OK) return false;
    toggle_in = toggle_out = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Public read/write
// ---------------------------------------------------------------------------

#define MSC_MAX_SECTORS_PER_CMD 64u    // 32 KiB per READ(10)/WRITE(10)

static msc_result_t rw10(bool write, uint32_t lba, uint16_t count, void *buf) {
    uint8_t cb[10] = {
        write ? 0x2A : 0x28, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8),  (uint8_t)lba,
        0,
        (uint8_t)(count >> 8), (uint8_t)count,
        0,
    };
    uint32_t bytes = (uint32_t)count * block_size_;
    uint8_t st;

    // One command, with one tier-2 retry on transport-level failure.
    for (int attempt = 0; attempt < 2; attempt++) {
        msc_result_t r = bot_command(cb, 10, !write, buf, bytes, &st, 5000);
        if (r == MSC_DISCONNECTED) return r;
        if (r == MSC_TRANSPORT_ERROR) {
            if (attempt == 0 && bot_reset_recovery()) continue;   // tier 2
            teardown();                                           // tier 3:
            return MSC_TRANSPORT_ERROR;   // next usb_msc_task() re-enumerates
        }
        if (st != 0) {                    // command failed: capture sense
            scsi_request_sense();
            return MSC_MEDIA_ERROR;
        }
        return MSC_OK;
    }
    return MSC_TRANSPORT_ERROR;
}

static msc_result_t rw(bool write, uint32_t lba, uint32_t count, void *buf) {
    if (state != ST_READY) return MSC_NOT_READY;
    if (lba + count < lba || lba + count > block_count_) return MSC_MEDIA_ERROR;
    uint8_t *p = (uint8_t *)buf;
    while (count) {
        uint16_t n = count > MSC_MAX_SECTORS_PER_CMD
                   ? (uint16_t)MSC_MAX_SECTORS_PER_CMD : (uint16_t)count;
        msc_result_t r = rw10(write, lba, n, p);
        if (r != MSC_OK) return r;
        lba += n;
        count -= n;
        p += (uint32_t)n * block_size_;
    }
    return MSC_OK;
}

msc_result_t usb_msc_read(uint32_t lba, uint32_t count, void *buf) {
    return rw(false, lba, count, buf);
}

msc_result_t usb_msc_write(uint32_t lba, uint32_t count, const void *buf) {
    return rw(true, lba, count, (void *)buf);
}
