#include "platform/diag.h"
// Created 1/18/26 by bkidwell
// https://docs.tinyusb.org/en/latest/integration.html

#include <stdlib.h>

#include "tusb_config.h"
#include "fwUSBHost.h"
#include "pico/stdlib.h"
#include "pio_usb.h"
extern "C" {
#include "platform/board.h"
#include "platform/ioexp.h"
#include "input/picpwr.h"
#include "input/app_recovery.h"

#include "tusb.h"
}

fwUSBHost obUSBHost;

extern "C" uint32_t tusb_time_millis_api(void) {
    return (uint32_t)(time_us_64() / 1000u);
}

fwUSBHost::fwUSBHost() {
}

bool fwUSBHost::init() {
    const uint32_t usb_zone = picpwr_zone_bit(PICPWR_ZONE_USB_HUB);
    picpwr_keep_awake(usb_zone);
    absolute_time_t deadline = make_timeout_time_ms(10000);
    uint32_t rails = 0;
    while (!time_reached(deadline)) {
        fw2_app_recovery_task();
        picpwr_task();
        if (picpwr_rails(&rails) && (rails & usb_zone) == usb_zone) break;
        fw2_app_recovery_sleep_ms(25);
    }
    if (!picpwr_rails(&rails) || (rails & usb_zone) != usb_zone) {
        DIAG("PIO-USB: zone 8 failed to become ready\n");
        return false;
    }
    ioexp_usb_pwr(true);
    pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
    cfg.pin_dp = PIN_PIO_USB_DP;
    cfg.tx_ch = 9;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg);
    return tuh_init(BOARD_TUH_RHPORT);
}

void fwUSBHost::task() {
    picpwr_task();
    tuh_task();

    // Run child tasks
    m_obCDC.task();
    m_obHID.task();
}

//------------- TinyUSB Callbacks -------------//

extern "C" {

void tuh_mount_cb(uint8_t dev_addr) {
    tusb_desc_device_t desc;
    if (tuh_descriptor_get_device_sync(dev_addr, &desc, sizeof(desc)) == XFER_RESULT_SUCCESS) {
        DIAG("[USB] dev %d: VID=%04x PID=%04x class=%d subclass=%d protocol=%d\n",
            dev_addr, desc.idVendor, desc.idProduct,
            desc.bDeviceClass, desc.bDeviceSubClass, desc.bDeviceProtocol);
    } else {
        DIAG("[USB] dev %d: mounted (descriptor read failed)\n", dev_addr);
    }
}

void tuh_umount_cb(uint8_t dev_addr) {
    (void)dev_addr;
}

} // extern "C"
