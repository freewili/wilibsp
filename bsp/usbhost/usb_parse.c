#include "usb_parse.h"
#include <string.h>

enum { DT_CONFIG = 2, DT_INTERFACE = 4, DT_ENDPOINT = 5 };

// wMaxPacketSize arrives from the device and ends up as the buffer length the
// host controller is told it may write into a fixed DPRAM window, so strip the
// high-speed transaction bits and cap the size before anyone stores it. A
// device asking for more than a full-speed pipe can carry is out of spec;
// honouring it would let the controller run past the end of that window.
static uint16_t ep_packet_size(const uint8_t *p) {
    uint16_t mps = (uint16_t)(p[4] | (p[5] << 8)) & USB_EP_MPS_MASK;
    return mps > USB_FS_MAX_PACKET ? (uint16_t)USB_FS_MAX_PACKET : mps;
}

bool usb_parse_config(const uint8_t *d, uint16_t len, usb_cfg_info_t *out) {
    memset(out, 0, sizeof *out);
    if (len < 9 || d[0] < 9 || d[1] != DT_CONFIG) return false;
    out->config_value = d[5];

    uint8_t cur_itf = 0;
    bool in_msc_itf = false, in_hub_itf = false;
    uint16_t off = 0;
    while (off < len) {
        uint8_t dlen = d[off];
        if (dlen < 2 || (uint32_t)off + dlen > len) return false;
        const uint8_t *p = d + off;
        switch (p[1]) {
        case DT_INTERFACE:
            if (dlen < 9) return false;
            if (p[3] != 0) {
                // Non-zero bAlternateSetting: ignore this interface and its endpoints
                in_msc_itf = false;
                in_hub_itf = false;
                break;
            }
            cur_itf = p[2];
            in_msc_itf = (p[5] == USB_CLASS_MSC && p[6] == MSC_SUBCLASS_SCSI &&
                          p[7] == MSC_PROTO_BOT && !out->is_msc);
            in_hub_itf = (p[5] == USB_CLASS_HUB && !out->is_hub);
            if (in_msc_itf) { out->is_msc = true; out->msc_itf = cur_itf; }
            if (in_hub_itf) out->is_hub = true;
            break;
        case DT_ENDPOINT: {
            if (dlen < 7) return false;
            uint8_t  addr = p[2], attr = p[3] & 0x03;
            uint16_t mps  = ep_packet_size(p);
            // A zero-length pipe cannot carry a transfer. Leaving it
            // unrecorded lets the completeness checks below reject the device
            // rather than arming an endpoint that can never move data.
            if (mps == 0) break;
            if (in_msc_itf && attr == 0x02) {           // bulk
                if (addr & 0x80) { out->bulk_in = addr;  out->bulk_in_mps = mps; }
                else             { out->bulk_out = addr; out->bulk_out_mps = mps; }
            }
            if (in_hub_itf && attr == 0x03 && (addr & 0x80)) {  // interrupt IN
                out->hub_int_ep = addr;
                out->hub_int_mps = mps;
                out->hub_int_interval = p[6];
            }
            break;
        }
        default:
            break;
        }
        off += dlen;
    }
    if (out->is_msc && (!out->bulk_in || !out->bulk_out)) return false;
    if (out->is_hub && !out->hub_int_ep) return false;
    return out->is_msc || out->is_hub;
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void msc_build_cbw(uint8_t cbw[MSC_CBW_LEN], uint32_t tag, uint32_t data_len,
                   bool dir_in, uint8_t lun, const uint8_t *cb, uint8_t cb_len) {
    memset(cbw, 0, MSC_CBW_LEN);
    put_le32(cbw, 0x43425355u);          // dCBWSignature 'USBC'
    put_le32(cbw + 4, tag);
    put_le32(cbw + 8, data_len);
    cbw[12] = dir_in ? 0x80 : 0x00;
    cbw[13] = lun;
    if (cb_len > 16) cb_len = 16;    // CB field is exactly 16 bytes (BOT spec)
    cbw[14] = cb_len;
    memcpy(cbw + 15, cb, cb_len);
}

bool msc_parse_csw(const uint8_t csw[MSC_CSW_LEN], uint32_t tag, uint8_t *status) {
    if (get_le32(csw) != 0x53425355u) return false;   // dCSWSignature 'USBS'
    if (get_le32(csw + 4) != tag) return false;
    if (csw[12] > 2) return false;
    *status = csw[12];
    return true;
}
