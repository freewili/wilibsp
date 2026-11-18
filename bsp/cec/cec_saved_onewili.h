/* The physical address saved on the SD card, through OneWili's SDFS: one
 * setting every BSP app shares, so an address one app learned or was given is
 * where the next app starts. The file holds "A.B.C.D" (cec_place.h); it can be
 * written from a PC as well. Include this only in apps that link libs/onewili,
 * after their SD link is armed (ow_sd_arm(), or fw2_app_recovery_wrap_sd()).
 * See docs/drivers/cec.md for where it ranks. */
#ifndef CEC_SAVED_ONEWILI_H
#define CEC_SAVED_ONEWILI_H

#include "cec/cec.h"
#include "cec/cec_place.h"
#include "onewili.h"
#include "onewili_sd.h"

#define CEC_SAVED_DIR_ROOT "/appdata"
#define CEC_SAVED_DIR      "/appdata/wilibsp"
#define CEC_SAVED_PATH     CEC_SAVED_DIR "/cec_address.txt"

/* Reads the saved address and hands it to cec_use_saved_address(). False when
 * there is none, or it is not "A.B.C.D". */
static inline bool cec_saved_load(ow_device *dev) {
    char text[16];
    size_t got = 0;
    uint16_t phys_addr;
    if (ow_sd_get_mem(dev, CEC_SAVED_PATH, text, sizeof text, &got) != OW_OK ||
        !cec_place_parse(text, got, &phys_addr))
        return false;
    cec_use_saved_address(phys_addr);
    return true;
}

/* Saves phys_addr for every app. */
static inline bool cec_saved_store(ow_device *dev, uint16_t phys_addr) {
    char text[8];
    cec_place_format(phys_addr, text);
    text[7] = '\n';
    ow_sd_mkdir(dev, CEC_SAVED_DIR_ROOT);   /* an existing directory reports a clean error */
    ow_sd_mkdir(dev, CEC_SAVED_DIR);
    return ow_sd_put_mem(dev, CEC_SAVED_PATH, text, 8, false) == OW_OK;
}

/* Every main-loop pass, or after CEC_EV_ADDRESS: saves an address cec_task()
 * learned. True when one was saved. */
static inline bool cec_saved_task(ow_device *dev) {
    uint16_t phys_addr;
    return cec_take_learned_address(&phys_addr) && cec_saved_store(dev, phys_addr);
}

#endif
