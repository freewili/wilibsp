// RP2350 USB host controller driver. Polled, no interrupts.
// Datasheet findings (Task 4 Step 1), from SDK 2.2.0 headers:
//   - BUFF_STATUS host mapping: regs/usb.h gives EP0_IN = bit 0, EP0_OUT =
//     bit 1, EPn_IN = bit 2n, EPn_OUT = bit 2n+1. In host mode bit 0 reports
//     EPX completion, and hardware-polled interrupt endpoint N (1..15) uses
//     the EPn_IN/OUT positions: IN completion on bit 2N (OUT on 2N+1) --
//     i.e. polled endpoint N -> bit 2*N, as expected. Corroborated by
//     TinyUSB hcd_rp2040.c ("EPX is bit 0 & 1, IEP1 IN is bit 2, IEP1 OUT
//     is bit 3, IEP2 IN is bit 4, ... etc", bit = 1 << (i * 2 + j)).
//   - DPRAM 16-bit write support: structs/usb_dpram.h says of the DPSRAM:
//     "4K of DPSRAM at beginning. Note this supports 8, 16, and 32 bit
//     accesses" -- so io_rw_16 halfword writes to one half of a
//     double-buffered buffer-control word are architecturally fine (DPRAM is
//     SRAM-backed, not a register bus). The SDK structs themselves declare
//     the words io_rw_32; we may alias halves as io_rw_16 in Task 5/7.
//   - Names verified in SDK 2.2.0: usbh_dpram (usb_host_dpram_t, full 4 KB,
//     static_assert sizeof == USB_DPRAM_MAX == 4096) with fields
//     setup_packet[8], int_ep_ctrl[15].ctrl, epx_buf_ctrl, epx_ctrl,
//     int_ep_buffer_ctrl[15].ctrl, epx_data[] (offset 0x180); usb_hw fields
//     dev_addr_ctrl, int_ep_addr_ctrl[15], int_ep_ctrl, buf_status, muxing,
//     pwr, main_ctrl, sie_ctrl, sie_status; constants EP_CTRL_*,
//     USB_BUF_CTRL_*, USB_SIE_CTRL_*, USB_SIE_STATUS_*, USB_MAIN_CTRL_*,
//     USB_USB_MUXING_*, USB_USB_PWR_*, USB_ADDR_ENDP_ENDPOINT_LSB (=16).
//     All names in this file exist as written; no renames were needed.
#include "usb_hcd.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/resets.h"
#include "hardware/structs/usb.h"   // also pulls in structs/usb_dpram.h

// Clear-alias view of the USB registers (write-1-to-clear via the 0x3000
// atomic alias). SDK 2.2.0 defines no usb_hw_clear itself (verified:
// structs/usb.h only defines usb_hw); build from hw_clear_alias().
#define usb_hw_clear hw_clear_alias(usb_hw)

// SIE_CTRL bits present for every host operation: SOF/keep-alive generation
// and the host bus-pulldowns.
#define SIE_CTRL_BASE (USB_SIE_CTRL_SOF_EN_BITS | USB_SIE_CTRL_KEEP_ALIVE_EN_BITS | \
                       USB_SIE_CTRL_PULLDOWN_EN_BITS)

// DPRAM data-buffer layout (offsets from USBCTRL_DPRAM_BASE; the control/buffer
// register area occupies the bottom 0x180):
#define EPX_BUF_OFFSET    0x180u            // 2 x 64 bytes, EPX double buffer
#define INT_EP_BUF_OFFSET 0x200u            // 64 bytes, hub interrupt endpoint
#define INT_EP_BUF_STATUS_BIT USB_BUFF_STATUS_EP1_IN_BITS   // bit 2
static uint8_t *const epx_buf    = (uint8_t *)(USBCTRL_DPRAM_BASE + EPX_BUF_OFFSET);
static uint8_t *const int_ep_buf = (uint8_t *)(USBCTRL_DPRAM_BASE + INT_EP_BUF_OFFSET);

// USB DPRAM is Device memory on the M33: every access must be naturally
// aligned. Newlib's optimized memcpy uses unaligned word/halfword accesses
// for tails and dst-alignment fixups, which hardfault against DPRAM
// (UNALIGNED UsageFault, verified on hardware). This copy uses only
// naturally aligned word accesses plus a byte tail.
static void dpram_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0) {
        for (; n >= 4; n -= 4, d += 4, s += 4)
            *(uint32_t *)d = *(const uint32_t *)s;
    }
    while (n--) *d++ = *s++;
}

// Endpoint-type encoding for the EP_CONTROL ENDPOINT_TYPE field (bits 27:26,
// EP_CTRL_BUFFER_TYPE_LSB). SDK 2.2.0 defines no USB_TRANSFER_TYPE_* names
// (verified: absent from regs/usb.h and structs/usb_dpram.h), so define them
// here per the RP2350 datasheet: 0 control, 1 isochronous, 2 bulk,
// 3 interrupt. Same values TinyUSB feeds this field (tusb_xfer_type_t).
#define USB_TRANSFER_TYPE_CONTROL   0u
#define USB_TRANSFER_TYPE_BULK      2u
#define USB_TRANSFER_TYPE_INTERRUPT 3u

static uint8_t ep0_mps = 8;

void hcd_set_ep0_mps(uint8_t mps) { ep0_mps = mps; }

void fw2_native_usb_hcd_init(void) {
    reset_block_num(RESET_USBCTRL);
    unreset_block_num_wait_blocking(RESET_USBCTRL);
    // usb_host_dpram_t covers the full 4 KB DPRAM (static_assert in
    // usb_dpram.h), so this clears the data-buffer region at 0x180+ too.
    // Plain memset is alignment-safe here (unlike memcpy elsewhere in this
    // file, see dpram_copy): newlib's memset aligns to dst then uses word
    // stores, and dst (0x50100000) and size (4096) are both word-aligned.
    memset((void *)usbh_dpram, 0, sizeof(*usbh_dpram));

    // Connect controller to the on-chip PHY, force VBUS-detect high (our VBUS
    // is hardwired on), enable controller in host mode, start SOF generation.
    usb_hw->muxing = USB_USB_MUXING_TO_PHY_BITS | USB_USB_MUXING_SOFTCON_BITS;
    usb_hw->pwr = USB_USB_PWR_VBUS_DETECT_BITS | USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS;
    usb_hw->main_ctrl = USB_MAIN_CTRL_CONTROLLER_EN_BITS | USB_MAIN_CTRL_HOST_NDEVICE_BITS;
    usb_hw->sie_ctrl = SIE_CTRL_BASE;
    ep0_mps = 8;
}

hcd_speed_t hcd_port_speed(void) {
    return (hcd_speed_t)((usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS)
                         >> USB_SIE_STATUS_SPEED_LSB);
}

void hcd_bus_reset(void) {
    hw_set_bits(&usb_hw->sie_ctrl, USB_SIE_CTRL_RESET_BUS_BITS);
    sleep_ms(50);    // SIE drives SE0; bit self-clears when reset completes
    sleep_ms(10);    // post-reset recovery before first transfer
    ep0_mps = 8;
}

// ---------------------------------------------------------------------------
// Shared transfer machinery
// ---------------------------------------------------------------------------

// Write one 16-bit half of the EPX buffer control. Datasheet requires the
// AVAILABLE bit to be set after the rest of the halfword has settled
// (>= 3 usb_clk cycles), or the SIE can read a torn value.
static void epx_buf_ctrl_write_half(int half, uint16_t val) {
    io_rw_16 *bc = (io_rw_16 *)&usbh_dpram->epx_buf_ctrl;
    if (val & USB_BUF_CTRL_AVAIL) {
        bc[half] = val & ~USB_BUF_CTRL_AVAIL;
        busy_wait_at_least_cycles(12);
    }
    bc[half] = val;
}

static uint16_t epx_buf_ctrl_read_half(int half) {
    io_rw_16 *bc = (io_rw_16 *)&usbh_dpram->epx_buf_ctrl;
    return bc[half];
}

// Start a transfer on EPX. dir_bits = SEND_SETUP / SEND_DATA / RECEIVE_DATA.
// Per RP2040/RP2350 erratum, START_TRANS must be set a few cycles after the
// rest of SIE_CTRL.
static void sie_start_transfer(uint32_t dir_bits) {
    // clear any stale completion from a previous errored transfer
    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS;
    usb_hw->sie_ctrl = SIE_CTRL_BASE | dir_bits;
    busy_wait_at_least_cycles(12);
    usb_hw->sie_ctrl = SIE_CTRL_BASE | dir_bits | USB_SIE_CTRL_START_TRANS_BITS;
}

// Check-and-clear SIE error flags. Returns HCD_OK if none set.
static hcd_result_t sie_check_errors(void) {
    uint32_t s = usb_hw->sie_status;
    if (s & USB_SIE_STATUS_STALL_REC_BITS) {
        usb_hw_clear->sie_status = USB_SIE_STATUS_STALL_REC_BITS;
        return HCD_ERR_STALL;
    }
    if (s & USB_SIE_STATUS_RX_TIMEOUT_BITS) {
        usb_hw_clear->sie_status = USB_SIE_STATUS_RX_TIMEOUT_BITS;
        return HCD_ERR_TIMEOUT;
    }
    uint32_t data_err = USB_SIE_STATUS_RX_OVERFLOW_BITS | USB_SIE_STATUS_DATA_SEQ_ERROR_BITS |
                        USB_SIE_STATUS_CRC_ERROR_BITS | USB_SIE_STATUS_BIT_STUFF_ERROR_BITS;
    if (s & data_err) {
        usb_hw_clear->sie_status = data_err;
        return HCD_ERR_DATA;
    }
    return HCD_OK;
}

// Cancel any in-flight EPX transfer so the SIE no longer owns the EPX
// buffers. Called on error exits from transfers (timeouts above all: the SIE
// may still be endlessly retrying a NAKed transaction, and starting the next
// transfer over that corrupts it). Resolves the former Task-5 TODO here.
//
// Mechanism (SDK 2.2.0 regs/usb.h): the EP_ABORT / EP_ABORT_DONE registers
// are NOT applicable -- both register descriptions begin "Device only:"
// ("Device only: Can be set to ignore the buffer control register ..." /
// "Device only: Used in conjunction with `EP_ABORT`...").  The documented
// host-mode control is SIE_CTRL.STOP_TRANS: "Host: Stop transaction",
// access type "SC" (self-clearing). Write it together with the base host
// bits -- the same write also drops any pending SEND_DATA/RECEIVE_DATA
// request -- then wait (bounded) for the self-clear, then reclaim EPX.
static void epx_abort(void) {
    usb_hw->sie_ctrl = SIE_CTRL_BASE | USB_SIE_CTRL_STOP_TRANS_BITS;
    // Bounded wait for the self-clearing bit; 1 ms >> one max-size FS packet
    // time (~50 us), so any packet already on the wire has finished too.
    absolute_time_t deadline = make_timeout_time_ms(1);
    while ((usb_hw->sie_ctrl & USB_SIE_CTRL_STOP_TRANS_BITS)
           && !time_reached(deadline)) {
        tight_loop_contents();
    }
    // Reclaim EPX: revoke both buffer-control halves, drop the EPX buffer
    // flag and any completion/error status the aborted transfer left behind
    // (a stale error bit would otherwise fail the next transfer's first
    // sie_check_errors()).
    epx_buf_ctrl_write_half(0, 0);
    epx_buf_ctrl_write_half(1, 0);
    usb_hw_clear->buf_status = 1u;
    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS
                             | USB_SIE_STATUS_STALL_REC_BITS
                             | USB_SIE_STATUS_RX_TIMEOUT_BITS
                             | USB_SIE_STATUS_RX_OVERFLOW_BITS
                             | USB_SIE_STATUS_DATA_SEQ_ERROR_BITS
                             | USB_SIE_STATUS_CRC_ERROR_BITS
                             | USB_SIE_STATUS_BIT_STUFF_ERROR_BITS;
}

// Wait (polling) until TRANS_COMPLETE, an error, disconnect, or timeout.
// int_seen: caller must sample (usb_hw->buf_status & INT_EP_BUF_STATUS_BIT)
// BEFORE calling sie_start_transfer() so the snapshot precedes any completion
// that could be attributed to the EPX transfer rather than the int EP.
static hcd_result_t sie_wait_trans_complete(uint32_t timeout_ms, bool int_seen) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    for (;;) {
        hcd_result_t err = sie_check_errors();
        if (err != HCD_OK) return err;
        if (usb_hw->sie_status & USB_SIE_STATUS_TRANS_COMPLETE_BITS) {
            usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS;
            // Int-EP completions also raise TRANS_COMPLETE (regs/usb.h: "An IN
            // packet is received and LAST_BUFF is set"). If a NEW int-EP
            // completion appeared during this wait and EPX shows no buffer
            // completion of its own, the TC was the int EP's: keep waiting.
            if (!int_seen && (usb_hw->buf_status & INT_EP_BUF_STATUS_BIT)
                          && !(usb_hw->buf_status & 1u)) {
                int_seen = true;
                continue;
            }
            return HCD_OK;
        }
        if (hcd_port_speed() == HCD_SPEED_NONE) return HCD_ERR_DISCONNECT;
        if (time_reached(deadline)) return HCD_ERR_TIMEOUT;
    }
}

// Configure EPX for a single-buffered transfer of one packet and run it.
// Used by control transfers (small, simplicity over speed).
// dir_in: true = IN; buf/len: packet to send (OUT) or space to receive (IN).
// Returns bytes moved via *actual.
static hcd_result_t epx_single_packet(uint8_t dev_addr, uint8_t ep_num,
                                      bool dir_in, uint8_t toggle,
                                      void *buf, uint16_t len, uint16_t *actual,
                                      uint32_t timeout_ms) {
    usbh_dpram->epx_ctrl = EP_CTRL_ENABLE_BITS | EP_CTRL_INTERRUPT_PER_BUFFER
                         | (USB_TRANSFER_TYPE_CONTROL << EP_CTRL_BUFFER_TYPE_LSB)
                         | EPX_BUF_OFFSET;
    usb_hw->dev_addr_ctrl = (uint32_t)dev_addr
                          | ((uint32_t)ep_num << USB_ADDR_ENDP_ENDPOINT_LSB);

    uint16_t bc = (uint16_t)(len | USB_BUF_CTRL_LAST | USB_BUF_CTRL_AVAIL
                  | (toggle ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID));
    if (!dir_in) {
        if (len) dpram_copy(epx_buf, buf, len);
        bc |= USB_BUF_CTRL_FULL;
    }
    usb_hw_clear->buf_status = 1u;               // stale EPX buffer flag
    epx_buf_ctrl_write_half(0, bc);
    bool int_seen = (usb_hw->buf_status & INT_EP_BUF_STATUS_BIT) != 0;
    sie_start_transfer(dir_in ? USB_SIE_CTRL_RECEIVE_DATA_BITS
                              : USB_SIE_CTRL_SEND_DATA_BITS);
    hcd_result_t r = sie_wait_trans_complete(timeout_ms, int_seen);
    if (r != HCD_OK) {
        // On timeout the SIE may still be retrying a NAKed transaction and
        // still owns the EPX buffer; cancel before the caller moves on.
        if (r == HCD_ERR_TIMEOUT) epx_abort();
        return r;
    }

    if (dir_in) {
        uint16_t rx = epx_buf_ctrl_read_half(0) & USB_BUF_CTRL_LEN_MASK;
        if (rx > len) rx = len;
        dpram_copy(buf, epx_buf, rx);
        *actual = rx;
    } else {
        *actual = len;
    }
    usb_hw_clear->buf_status = 1u;
    return HCD_OK;
}

hcd_result_t hcd_control_xfer(uint8_t dev_addr, const uint8_t setup[8],
                              void *data, uint16_t *inout_len) {
    bool dir_in = (setup[0] & 0x80u) != 0;
    uint16_t wlen = (uint16_t)(setup[6] | (setup[7] << 8));

    // --- SETUP stage: 8 bytes from the dedicated DPRAM setup area, DATA0 ---
    dpram_copy((void *)usbh_dpram->setup_packet, setup, 8);
    usb_hw->dev_addr_ctrl = dev_addr;            // endpoint 0
    bool int_seen = (usb_hw->buf_status & INT_EP_BUF_STATUS_BIT) != 0;
    sie_start_transfer(USB_SIE_CTRL_SEND_SETUP_BITS);
    hcd_result_t r = sie_wait_trans_complete(100, int_seen);
    if (r != HCD_OK) {
        if (r == HCD_ERR_TIMEOUT) epx_abort();   // SIE may still be retrying
        return r;
    }

    // --- DATA stage: mps-sized packets, toggle starts at DATA1 ---
    uint16_t total = 0;
    if (data && inout_len && wlen) {
        uint16_t want = wlen < *inout_len ? wlen : *inout_len;
        uint8_t *p = (uint8_t *)data;
        uint8_t toggle = 1;
        while (total < want) {
            uint16_t n = (uint16_t)(want - total);
            if (n > ep0_mps) n = ep0_mps;
            uint16_t moved = 0;
            r = epx_single_packet(dev_addr, 0, dir_in, toggle,
                                  p + total, n, &moved, 100);
            if (r != HCD_OK) return r;
            total = (uint16_t)(total + moved);
            toggle ^= 1;
            if (dir_in && moved < ep0_mps) break;   // short packet ends stage
        }
    }
    if (inout_len) *inout_len = total;

    // --- STATUS stage: zero-length, always DATA1 ---
    // USB rule: status is IN unless the transfer had an IN data stage,
    // in which case status is OUT.
    bool status_in = !(dir_in && wlen && data);
    uint16_t zero = 0;
    uint8_t dummy[1];
    return epx_single_packet(dev_addr, 0, status_in, 1, dummy, 0, &zero, 100);
}

// ---------------------------------------------------------------------------
// Bulk transfers: double-buffered EPX pump
// ---------------------------------------------------------------------------

// Prime one half of the EPX double buffer for the next packet of the
// transfer. *toggle is the DATA toggle; *queued tracks bytes already
// assigned to buffers.
//
// Note (by design, see hcd_bulk_xfer): the DATA toggle advances at prime
// time, not completion time. If the transfer is aborted mid-flight the
// caller's toggle is wrong -- accepted, because every MSC error path ends in
// CLEAR_FEATURE(ENDPOINT_HALT) (which resets both sides' toggles) or a bus
// reset.
static void epx_prime_half(int half, bool dir_in, const uint8_t *src,
                           uint32_t len, uint32_t *queued, uint8_t *toggle) {
    uint32_t remaining = len - *queued;
    uint16_t n = remaining > 64 ? 64 : (uint16_t)remaining;
    uint16_t bc = (uint16_t)(n | USB_BUF_CTRL_AVAIL
                  | (*toggle ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID));
    if (n == remaining) bc |= USB_BUF_CTRL_LAST;
    if (!dir_in) {
        dpram_copy(epx_buf + half * 64, src + *queued, n);
        bc |= USB_BUF_CTRL_FULL;
    }
    *queued += n;
    *toggle ^= 1;
    epx_buf_ctrl_write_half(half, bc);
}

hcd_result_t hcd_bulk_xfer(uint8_t dev_addr, uint8_t ep_addr, uint8_t *toggle,
                           void *buf, uint32_t len, uint32_t *actual,
                           uint32_t timeout_ms) {
    bool dir_in = (ep_addr & 0x80u) != 0;
    uint8_t *p = (uint8_t *)buf;
    uint32_t queued = 0;     // bytes assigned to a buffer half so far
    uint32_t done = 0;       // bytes confirmed transferred
    uint8_t start_toggle = (uint8_t)(*toggle & 1);   // for short-IN toggle fix-up
    uint32_t pkts_consumed = 0;                      // IN packets actually drained
    if (actual) *actual = 0;

    usbh_dpram->epx_ctrl = EP_CTRL_ENABLE_BITS | EP_CTRL_DOUBLE_BUFFERED_BITS
                         | EP_CTRL_INTERRUPT_PER_BUFFER
                         | (USB_TRANSFER_TYPE_BULK << EP_CTRL_BUFFER_TYPE_LSB)
                         | EPX_BUF_OFFSET;
    usb_hw->dev_addr_ctrl = (uint32_t)dev_addr
                          | ((uint32_t)(ep_addr & 0x0F) << USB_ADDR_ENDP_ENDPOINT_LSB);
    usb_hw_clear->buf_status = 1u;               // stale EPX flag

    // Prime both halves (half 1 only if the transfer needs a second packet).
    epx_prime_half(0, dir_in, p, len, &queued, toggle);
    if (queued < len) epx_prime_half(1, dir_in, p, len, &queued, toggle);

    sie_start_transfer(dir_in ? USB_SIE_CTRL_RECEIVE_DATA_BITS
                              : USB_SIE_CTRL_SEND_DATA_BITS);

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    int next_half = 0;                           // hardware consumes 0,1,0,1,...
    bool complete = false;

    while (!complete || done < queued) {
        // TRANS_COMPLETE can arrive together with the final buffer flag;
        // latch it but keep draining buffers until done catches up.
        if (usb_hw->sie_status & USB_SIE_STATUS_TRANS_COMPLETE_BITS) {
            usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS;
            complete = true;
        }
        hcd_result_t err = sie_check_errors();
        if (err != HCD_OK) { epx_abort(); return err; }
        if (hcd_port_speed() == HCD_SPEED_NONE) { epx_abort(); return HCD_ERR_DISCONNECT; }
        if (time_reached(deadline)) { epx_abort(); return HCD_ERR_TIMEOUT; }
        if (!(usb_hw->buf_status & 1u)) continue;

        // At least one half finished. Service in strict alternation; check
        // the half's own status bits rather than trusting the flag count.
        usb_hw_clear->buf_status = 1u;
        for (;;) {
            // Nothing in flight (queued bytes all confirmed) -> stop: a
            // drained-and-revoked half reads as 0, which for OUT would
            // otherwise be indistinguishable from "hardware finished it"
            // (AVAIL clear) and spin this loop forever.
            if (done == queued) break;
            uint16_t bc = epx_buf_ctrl_read_half(next_half);
            if (dir_in) {
                if (!(bc & USB_BUF_CTRL_FULL)) break;        // not done yet
                uint16_t rx = bc & USB_BUF_CTRL_LEN_MASK;
                uint32_t cap = len - done;                   // user-buffer headroom
                if (cap > 64) cap = 64;                      // half size
                if (rx > cap) rx = (uint16_t)cap;            // babble guard
                dpram_copy(p + done, epx_buf + next_half * 64, rx);
                done += rx;
                pkts_consumed++;
                if (rx < 64) {                               // short packet ends transfer
                    // Revoke both halves -- safe, the SIE has stopped after a
                    // short IN; the other half may still hold a stale prime.
                    epx_buf_ctrl_write_half(next_half, 0);
                    epx_buf_ctrl_write_half(next_half ^ 1, 0);
                    // The prime-time toggle advance over-counts when a
                    // transfer ends short; the wire truth is the number of
                    // packets actually consumed.
                    *toggle = (uint8_t)(start_toggle ^ (pkts_consumed & 1));
                    if (actual) *actual = done;
                    // hardware raises TRANS_COMPLETE for short IN packets;
                    // consume it if already latched, else don't wait for it
                    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS;
                    return HCD_OK;
                }
                // Clear FULL so we don't re-service; re-arm if more queued
                if (queued < len) {
                    epx_prime_half(next_half, true, p, len, &queued, toggle);
                } else {
                    epx_buf_ctrl_write_half(next_half, 0);
                }
            } else {
                if (bc & USB_BUF_CTRL_AVAIL) break;          // hw still owns it
                uint16_t tx = bc & USB_BUF_CTRL_LEN_MASK;
                done += tx;
                if (queued < len) {
                    epx_prime_half(next_half, false, p, len, &queued, toggle);
                } else {
                    epx_buf_ctrl_write_half(next_half, 0);
                }
            }
            next_half ^= 1;
        }
    }
    if (actual) *actual = done;
    return HCD_OK;
}

// ---------------------------------------------------------------------------
// Hardware-polled interrupt endpoint (one supported; used by the hub)
// ---------------------------------------------------------------------------
// The controller polls registered interrupt endpoints autonomously every
// `interval` frames; completions appear in BUFF_STATUS. We use slot 0
// (= "interrupt endpoint 1" in register terms).
//
// Register evidence (SDK 2.2.0, verified for Task 10):
//   - BUFF_STATUS bit for slot 0: regs/usb.h USB_BUFF_STATUS_EP1_IN_BITS
//     = 0x00000004 (bit 2), consistent with the EPn_IN = bit 2n mapping
//     noted at the top of this file.
//   - Interval field: usb_dpram.h EP_CTRL_HOST_INTERRUPT_INTERVAL_LSB = 16.
//     The SDK headers carry no semantics comment; TinyUSB hcd_rp2040.c
//     writes ((bmInterval - 1) << EP_CTRL_HOST_INTERRUPT_INTERVAL_LSB), so
//     the field holds interval-in-ms MINUS 1 (0 => poll every frame).
//   - Address/enable: structs/usb.h int_ep_addr_ctrl[15] starts at
//     ADDR_ENDP1 (ADDRESS bits 6:0, ENDPOINT bits 19:16, INTEP_DIR bit 25
//     "In=0, Out=1" -- IN is the reset value, so we leave it clear), and
//     USB_INT_EP_CTRL_INT_EP_ACTIVE is bits 15:1 "Host: Enable interrupt
//     endpoint 1 -> 15" -- slot 0 ("EP1") enables via bit 1.
//   - INTEP_PREAMBLE (ADDR_ENDPn bit 26) is only for "a low speed device on
//     a full speed hub"; we support FS only, so it stays 0.

static uint8_t  int_ep_toggle;
static uint16_t int_ep_mps;

static void int_ep_arm(void) {
    uint16_t bc = (uint16_t)(int_ep_mps | USB_BUF_CTRL_AVAIL | USB_BUF_CTRL_LAST
                  | (int_ep_toggle ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID));
    io_rw_16 *p = (io_rw_16 *)&usbh_dpram->int_ep_buffer_ctrl[0].ctrl;
    p[0] = bc & ~USB_BUF_CTRL_AVAIL;
    busy_wait_at_least_cycles(12);
    p[0] = bc;
}

void hcd_int_ep_install(uint8_t dev_addr, uint8_t ep_addr, uint16_t mps,
                        uint8_t interval_ms) {
    int_ep_toggle = 0;
    int_ep_mps = mps;
    usb_hw->int_ep_addr_ctrl[0] = (uint32_t)dev_addr
        | ((uint32_t)(ep_addr & 0x0F) << USB_ADDR_ENDP_ENDPOINT_LSB);
    // Interval field semantics: value = poll interval in ms minus 1.
    usbh_dpram->int_ep_ctrl[0].ctrl = EP_CTRL_ENABLE_BITS | EP_CTRL_INTERRUPT_PER_BUFFER
        | (USB_TRANSFER_TYPE_INTERRUPT << EP_CTRL_BUFFER_TYPE_LSB)
        | ((uint32_t)(interval_ms ? interval_ms - 1 : 0) << EP_CTRL_HOST_INTERRUPT_INTERVAL_LSB)
        | INT_EP_BUF_OFFSET;
    usb_hw_clear->buf_status = INT_EP_BUF_STATUS_BIT;  // clear any stale flag before arming
    int_ep_arm();
    hw_set_bits(&usb_hw->int_ep_ctrl, 1u << 1);   // enable int-ep slot 0 ("EP1")
}

void hcd_int_ep_remove(void) {
    hw_clear_bits(&usb_hw->int_ep_ctrl, 1u << 1);
    usbh_dpram->int_ep_ctrl[0].ctrl = 0;
    usb_hw_clear->buf_status = INT_EP_BUF_STATUS_BIT;
}

int hcd_int_ep_poll(uint8_t *buf, uint8_t maxlen) {
    if (!(usb_hw->buf_status & INT_EP_BUF_STATUS_BIT)) return 0;
    usb_hw_clear->buf_status = INT_EP_BUF_STATUS_BIT;
    uint16_t bc = ((io_rw_16 *)&usbh_dpram->int_ep_buffer_ctrl[0].ctrl)[0];
    uint16_t n = bc & USB_BUF_CTRL_LEN_MASK;
    if (n > maxlen) n = maxlen;
    dpram_copy(buf, int_ep_buf, n);
    // The hardware retries NAKed polls invisibly and only completes (raising
    // BUFF_STATUS) when a report actually lands, so the toggle advances
    // exactly once per delivered report.
    int_ep_toggle ^= 1;
    int_ep_arm();
    return (int)n;
}
