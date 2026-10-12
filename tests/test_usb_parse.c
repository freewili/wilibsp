#include <assert.h>
#include <string.h>
#include "usbhost/usb_parse.h"

// Descriptor builder: append TLVs into a flat configuration buffer the way a
// device hands one back, so each case below reads like the bytes on the wire.
typedef struct {
    uint8_t  buf[256];
    uint16_t len;
} desc_t;

static void d_reset(desc_t *d) {
    memset(d, 0, sizeof *d);
}

static void d_append(desc_t *d, const uint8_t *bytes, uint8_t n) {
    memcpy(d->buf + d->len, bytes, n);
    d->len = (uint16_t)(d->len + n);
}

static void d_config(desc_t *d, uint8_t config_value) {
    const uint8_t cfg[9] = {9, 2, 0, 0, 1, config_value, 0, 0x80, 50};
    d_append(d, cfg, 9);
}

static void d_interface(desc_t *d, uint8_t num, uint8_t alt, uint8_t cls,
                        uint8_t sub, uint8_t proto) {
    const uint8_t itf[9] = {9, 4, num, alt, 2, cls, sub, proto, 0};
    d_append(d, itf, 9);
}

static void d_endpoint(desc_t *d, uint8_t addr, uint8_t attr, uint16_t mps,
                       uint8_t interval) {
    const uint8_t ep[7] = {7, 5, addr, attr, (uint8_t)mps, (uint8_t)(mps >> 8),
                           interval};
    d_append(d, ep, 7);
}

static void msc_config(desc_t *d, uint16_t in_mps, uint16_t out_mps) {
    d_reset(d);
    d_config(d, 1);
    d_interface(d, 0, 0, USB_CLASS_MSC, MSC_SUBCLASS_SCSI, MSC_PROTO_BOT);
    d_endpoint(d, 0x81, 0x02, in_mps, 0);
    d_endpoint(d, 0x02, 0x02, out_mps, 0);
}

static void hub_config(desc_t *d, uint16_t int_mps, uint8_t interval) {
    d_reset(d);
    d_config(d, 1);
    d_interface(d, 0, 0, USB_CLASS_HUB, 0, 0);
    d_endpoint(d, 0x81, 0x03, int_mps, interval);
}

static void test_msc(void) {
    desc_t d;
    usb_cfg_info_t out;

    msc_config(&d, 64, 64);
    assert(usb_parse_config(d.buf, d.len, &out));
    assert(out.is_msc && !out.is_hub);
    assert(out.config_value == 1);
    assert(out.msc_itf == 0);
    assert(out.bulk_in == 0x81 && out.bulk_in_mps == 64);
    assert(out.bulk_out == 0x02 && out.bulk_out_mps == 64);

    // A bulk interface missing one direction is not usable.
    d_reset(&d);
    d_config(&d, 1);
    d_interface(&d, 0, 0, USB_CLASS_MSC, MSC_SUBCLASS_SCSI, MSC_PROTO_BOT);
    d_endpoint(&d, 0x81, 0x02, 64, 0);
    assert(!usb_parse_config(d.buf, d.len, &out));

    // Only bulk endpoints count; an interrupt endpoint is not a bulk pipe.
    d_reset(&d);
    d_config(&d, 1);
    d_interface(&d, 0, 0, USB_CLASS_MSC, MSC_SUBCLASS_SCSI, MSC_PROTO_BOT);
    d_endpoint(&d, 0x81, 0x02, 64, 0);
    d_endpoint(&d, 0x02, 0x03, 64, 8);
    assert(!usb_parse_config(d.buf, d.len, &out));

    // Endpoints under a non-zero alternate setting belong to that alt, not to
    // the interface we selected.
    d_reset(&d);
    d_config(&d, 1);
    d_interface(&d, 0, 0, USB_CLASS_MSC, MSC_SUBCLASS_SCSI, MSC_PROTO_BOT);
    d_endpoint(&d, 0x81, 0x02, 64, 0);
    d_endpoint(&d, 0x02, 0x02, 64, 0);
    d_interface(&d, 0, 1, USB_CLASS_MSC, MSC_SUBCLASS_SCSI, MSC_PROTO_BOT);
    d_endpoint(&d, 0x83, 0x02, 8, 0);
    assert(usb_parse_config(d.buf, d.len, &out));
    assert(out.bulk_in == 0x81 && out.bulk_in_mps == 64);

    // Wrong subclass or protocol is not BOT/SCSI mass storage.
    d_reset(&d);
    d_config(&d, 1);
    d_interface(&d, 0, 0, USB_CLASS_MSC, 0x04, MSC_PROTO_BOT);
    d_endpoint(&d, 0x81, 0x02, 64, 0);
    d_endpoint(&d, 0x02, 0x02, 64, 0);
    assert(!usb_parse_config(d.buf, d.len, &out));
}

static void test_hub(void) {
    desc_t d;
    usb_cfg_info_t out;

    hub_config(&d, 1, 12);
    assert(usb_parse_config(d.buf, d.len, &out));
    assert(out.is_hub && !out.is_msc);
    assert(out.hub_int_ep == 0x81);
    assert(out.hub_int_mps == 1);
    assert(out.hub_int_interval == 12);

    // A hub with no status-change endpoint cannot report port changes.
    d_reset(&d);
    d_config(&d, 1);
    d_interface(&d, 0, 0, USB_CLASS_HUB, 0, 0);
    assert(!usb_parse_config(d.buf, d.len, &out));

    // An OUT interrupt endpoint is not the status-change pipe.
    d_reset(&d);
    d_config(&d, 1);
    d_interface(&d, 0, 0, USB_CLASS_HUB, 0, 0);
    d_endpoint(&d, 0x01, 0x03, 1, 12);
    assert(!usb_parse_config(d.buf, d.len, &out));
}

static void test_malformed(void) {
    desc_t d;
    usb_cfg_info_t out;

    // Too short to hold a configuration descriptor.
    msc_config(&d, 64, 64);
    assert(!usb_parse_config(d.buf, 8, &out));

    // Not a configuration descriptor.
    msc_config(&d, 64, 64);
    d.buf[1] = 4;
    assert(!usb_parse_config(d.buf, d.len, &out));

    // bLength below the 9-byte configuration header.
    msc_config(&d, 64, 64);
    d.buf[0] = 8;
    assert(!usb_parse_config(d.buf, d.len, &out));

    // A descriptor whose bLength runs past the end of the buffer must not be
    // walked: the parser would read bytes the device never sent.
    msc_config(&d, 64, 64);
    d.buf[9] = 200;
    assert(!usb_parse_config(d.buf, d.len, &out));

    // bLength 0 or 1 cannot advance the walk; it must be rejected rather than
    // spun on.
    msc_config(&d, 64, 64);
    d.buf[9] = 0;
    assert(!usb_parse_config(d.buf, d.len, &out));
    msc_config(&d, 64, 64);
    d.buf[9] = 1;
    assert(!usb_parse_config(d.buf, d.len, &out));

    // An interface descriptor truncated below its 9 fixed bytes.
    d_reset(&d);
    d_config(&d, 1);
    {
        const uint8_t stub[4] = {4, 4, 0, 0};
        d_append(&d, stub, 4);
    }
    assert(!usb_parse_config(d.buf, d.len, &out));

    // An endpoint descriptor truncated below its 7 fixed bytes.
    d_reset(&d);
    d_config(&d, 1);
    d_interface(&d, 0, 0, USB_CLASS_MSC, MSC_SUBCLASS_SCSI, MSC_PROTO_BOT);
    {
        const uint8_t stub[5] = {5, 5, 0x81, 0x02, 64};
        d_append(&d, stub, 5);
    }
    assert(!usb_parse_config(d.buf, d.len, &out));

    // A device that is neither mass storage nor a hub.
    d_reset(&d);
    d_config(&d, 1);
    d_interface(&d, 0, 0, 0x03, 0, 0);
    d_endpoint(&d, 0x81, 0x03, 8, 10);
    assert(!usb_parse_config(d.buf, d.len, &out));
}

// wMaxPacketSize arrives from the device and is programmed into a fixed-size
// DPRAM window, so the parser must never hand back a size the hardware buffer
// cannot hold.
static void test_max_packet_size(void) {
    desc_t d;
    usb_cfg_info_t out;

    // Bits 12:11 are the high-speed additional-transaction field, not part of
    // the size. A device that sets them must not inflate the packet size.
    hub_config(&d, 0x1808, 12);   // 2 additional transactions, 8-byte packets
    assert(usb_parse_config(d.buf, d.len, &out));
    assert(out.hub_int_mps == 8);

    // An out-of-spec size must be clamped to what a full-speed pipe and the
    // host controller's buffer can actually take.
    hub_config(&d, 512, 12);
    assert(usb_parse_config(d.buf, d.len, &out));
    assert(out.hub_int_mps <= USB_FS_MAX_PACKET);

    hub_config(&d, 0xFFFF, 12);
    assert(usb_parse_config(d.buf, d.len, &out));
    assert(out.hub_int_mps <= USB_FS_MAX_PACKET);

    msc_config(&d, 0xFFFF, 0x0400);
    assert(usb_parse_config(d.buf, d.len, &out));
    assert(out.bulk_in_mps <= USB_FS_MAX_PACKET);
    assert(out.bulk_out_mps <= USB_FS_MAX_PACKET);

    // A zero-length packet size is not a usable pipe.
    hub_config(&d, 0, 12);
    assert(!usb_parse_config(d.buf, d.len, &out));

    // Only the transaction bits set: the size field itself is zero, so this is
    // the zero-length case however large the raw field looks.
    hub_config(&d, 0x0800, 12);
    assert(!usb_parse_config(d.buf, d.len, &out));
}

static void test_cbw(void) {
    uint8_t cbw[MSC_CBW_LEN];
    const uint8_t cb[10] = {0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0};

    msc_build_cbw(cbw, 0x11223344u, 512, true, 0, cb, sizeof cb);
    assert(cbw[0] == 0x55 && cbw[1] == 0x53 && cbw[2] == 0x42 && cbw[3] == 0x43);
    assert(cbw[4] == 0x44 && cbw[5] == 0x33 && cbw[6] == 0x22 && cbw[7] == 0x11);
    assert(cbw[8] == 0x00 && cbw[9] == 0x02 && cbw[10] == 0 && cbw[11] == 0);
    assert(cbw[12] == 0x80);
    assert(cbw[13] == 0);
    assert(cbw[14] == sizeof cb);
    assert(memcmp(cbw + 15, cb, sizeof cb) == 0);
    // Unused command bytes stay zeroed.
    for (unsigned i = 15 + sizeof cb; i < MSC_CBW_LEN; i++) assert(cbw[i] == 0);

    msc_build_cbw(cbw, 1, 0, false, 3, cb, sizeof cb);
    assert(cbw[12] == 0x00);
    assert(cbw[13] == 3);

    // An over-long command block is clamped to the 16-byte CB field rather
    // than running off the end of the wrapper.
    uint8_t big[32];
    memset(big, 0xAB, sizeof big);
    msc_build_cbw(cbw, 2, 0, false, 0, big, sizeof big);
    assert(cbw[14] == 16);
    for (unsigned i = 15; i < MSC_CBW_LEN; i++) assert(cbw[i] == 0xAB);
}

static void test_csw(void) {
    uint8_t csw[MSC_CSW_LEN] = {0x55, 0x53, 0x42, 0x53,
                                0x44, 0x33, 0x22, 0x11,
                                0, 0, 0, 0, 0};
    uint8_t status = 0xFF;

    assert(msc_parse_csw(csw, 0x11223344u, &status));
    assert(status == 0);

    csw[12] = 2;
    assert(msc_parse_csw(csw, 0x11223344u, &status));
    assert(status == 2);

    // Reserved status values are not a verdict we can act on.
    csw[12] = 3;
    assert(!msc_parse_csw(csw, 0x11223344u, &status));

    // A reply carrying another command's tag must not be accepted as this
    // command's result.
    csw[12] = 0;
    assert(!msc_parse_csw(csw, 0x11223345u, &status));

    csw[0] = 0x56;
    assert(!msc_parse_csw(csw, 0x11223344u, &status));
}

int main(void) {
    test_msc();
    test_hub();
    test_malformed();
    test_max_packet_size();
    test_cbw();
    test_csw();
    return 0;
}
