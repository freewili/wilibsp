#pragma once
#include <stdint.h>
#include <stdbool.h>

#define USB_CLASS_HUB     0x09
#define USB_CLASS_MSC     0x08
#define MSC_SUBCLASS_SCSI 0x06
#define MSC_PROTO_BOT     0x50

// Largest packet a full-speed bulk or interrupt pipe can carry (USB 2.0
// 5.7.3/5.8.3). This controller is full-speed only, and the host controller's
// per-endpoint DPRAM windows are sized for it, so a descriptor asking for more
// than this is out of spec and cannot be honoured.
#define USB_FS_MAX_PACKET 64u

// wMaxPacketSize bits 10:0 carry the size; bits 12:11 are the high-speed
// additional-transaction count and are not part of it (USB 2.0 9.6.6).
#define USB_EP_MPS_MASK   0x07FFu

typedef struct {
    uint8_t  config_value;    // bConfigurationValue
    bool     is_hub;
    bool     is_msc;
    // valid when is_msc:
    uint8_t  msc_itf;         // bInterfaceNumber
    uint8_t  bulk_in;         // endpoint address (0x8x)
    uint8_t  bulk_out;        // endpoint address (0x0x)
    uint16_t bulk_in_mps;
    uint16_t bulk_out_mps;
    // valid when is_hub:
    uint8_t  hub_int_ep;      // status-change endpoint address (0x8x)
    uint16_t hub_int_mps;
    uint8_t  hub_int_interval;
} usb_cfg_info_t;

// Walk a full configuration descriptor (config + interface + endpoint TLVs).
// Returns false on malformed/truncated input.
bool usb_parse_config(const uint8_t *d, uint16_t len, usb_cfg_info_t *out);

#define MSC_CBW_LEN 31
#define MSC_CSW_LEN 13

// Fill a 31-byte Command Block Wrapper. cb_len <= 16.
void msc_build_cbw(uint8_t cbw[MSC_CBW_LEN], uint32_t tag, uint32_t data_len,
                   bool dir_in, uint8_t lun, const uint8_t *cb, uint8_t cb_len);

// Validate a 13-byte Command Status Wrapper (signature, tag, status <= 2).
// On success writes bCSWStatus (0=passed, 1=failed, 2=phase error) to *status.
bool msc_parse_csw(const uint8_t csw[MSC_CSW_LEN], uint32_t tag, uint8_t *status);
