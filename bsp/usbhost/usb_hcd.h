#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    HCD_OK = 0,
    HCD_ERR_STALL,        // endpoint returned STALL
    HCD_ERR_TIMEOUT,      // device not responding / software timeout
    HCD_ERR_DATA,         // CRC, bit-stuff, DATA-seq, overflow
    HCD_ERR_DISCONNECT,   // device went away mid-transfer
} hcd_result_t;

typedef enum {
    HCD_SPEED_NONE = 0,
    HCD_SPEED_LOW  = 1,
    HCD_SPEED_FULL = 2,
} hcd_speed_t;

void        fw2_native_usb_hcd_init(void);
hcd_speed_t hcd_port_speed(void);   // current SIE_STATUS.SPEED
void        hcd_bus_reset(void);    // drive USB reset + recovery delay

// EP0 max packet size used by control transfers (8 until first descriptor read).
void hcd_set_ep0_mps(uint8_t mps);

// Blocking control transfer. setup = 8-byte setup packet. data/inout_len for
// the data stage (NULL/0 for none); *inout_len in = buffer size, out = actual.
// For OUT data stages the caller must supply exactly wLength bytes.
hcd_result_t hcd_control_xfer(uint8_t dev_addr, const uint8_t setup[8],
                              void *data, uint16_t *inout_len);

// Blocking bulk transfer on EPX, double-buffered. ep_addr bit 7 = IN.
// *toggle = DATA toggle for this pipe (0/1), updated on return.
hcd_result_t hcd_bulk_xfer(uint8_t dev_addr, uint8_t ep_addr, uint8_t *toggle,
                           void *buf, uint32_t len, uint32_t *actual,
                           uint32_t timeout_ms);

// Hardware-polled interrupt IN endpoint (hub status pipe). One supported.
void hcd_int_ep_install(uint8_t dev_addr, uint8_t ep_addr, uint16_t mps,
                        uint8_t interval_ms);
void hcd_int_ep_remove(void);
// >0: bytes copied to buf; 0: no new data (a zero-length report is indistinguishable)
int  hcd_int_ep_poll(uint8_t *buf, uint8_t maxlen);
