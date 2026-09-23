#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    MSC_OK = 0,
    MSC_NOT_READY,        // no drive enumerated / drive still spinning up
    MSC_MEDIA_ERROR,      // command failed; sense data in usb_msc_last_sense()
    MSC_TRANSPORT_ERROR,  // unrecoverable USB-level failure
    MSC_DISCONNECTED,     // device removed
} msc_result_t;

void     usb_msc_init(void);     // native USB HCD init + state machine reset
void     usb_msc_task(void);     // call from main loop; drives hotplug + enum
bool     usb_msc_ready(void);
uint32_t usb_msc_block_count(void);
uint32_t usb_msc_block_size(void);

msc_result_t usb_msc_read (uint32_t lba, uint32_t count, void *buf);
msc_result_t usb_msc_write(uint32_t lba, uint32_t count, const void *buf);

// Last SCSI sense key/asc/ascq captured after MSC_MEDIA_ERROR (0 if none).
uint32_t usb_msc_last_sense(void);

// Drive identity from INQUIRY (valid when ready): "VENDOR  PRODUCT REV"
const char *usb_msc_drive_name(void);
