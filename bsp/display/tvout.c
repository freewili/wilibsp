// bsp/display/tvout.c — see tvout.h.
//
// Scan-out. The frame is a list of four-word entries (tvout_list.h). Three
// DMA channels and no interrupt: the pixel channel writes one entry's words to
// the HSTX FIFO; when it finishes, the list channel copies the next entry into
// the pixel channel's registers, which starts it again; the last entry of a
// frame chains to the rewind channel, which points the list channel back at
// the first entry.
//
// A picture has a compact list and three more channels. A row chains to the
// join channel, which sends the join between two rows and chains to the row
// channel; the row channel copies the next row's control word and read
// address from the row table into the pixel channel and chains to it. With
// DVI framing every row's join is the same one, and the restart channel puts
// the join channel back at it after each row; with audio framing the joins
// are a ring of slots and the restart channel puts it back at the first.
// The last row chains to the list channel. Pixels go to the FIFO with 16-bit
// transfers: one pixel per FIFO word, in both halves of the word.
//
// A picture shown at twice its size is built as a frame twice as wide and
// tall whose rows read the picture's rows twice each (tvout_list.h), and the
// HSTX encoder sends every FIFO word twice: each pixel twice.
//
// A PSRAM picture is read through the line ring (tvout_ring.h), which
// tvout_start_psram() hands in, so only apps that show one link it: the list
// is built over the ring's line buffers and every row stays pointed at its
// line buffer while the ring's reader keeps them filled.
#include "display/tvout.h"
#include "display/tvout_audio.h"
#include "display/tvout_internal.h"
#include "display/tvout_list.h"
#include "input/uartkbd.h"
#include "rfid/rfid.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pll.h"
#include "hardware/resets.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define TVOUT_PIN_FIRST        12
#define TVOUT_PIN_LAST         19
#define HSTX_CLOCK_HZ          (126u * MHZ)
#define HSTX_CLOCKS_PER_PIXEL  5u
#define USB_ADC_CLOCK_HZ       (48u * MHZ)
#define DMA_CHANNELS_BARS      3
#define DMA_CHANNELS_PICTURE   6
// Readings of the compact list's scan position before one is taken as it is.
#define SCAN_TRIES             8u
// A frame lasts 16.7 ms; a flip waits at most about three.
#define FLIP_TIMEOUT_MS        50u

// USB PLL settings, with the 12 MHz crystal as the reference.
typedef struct {
    uint32_t fbdiv;
    uint32_t postdiv1;
    uint32_t postdiv2;
} usb_pll_setting_t;
_Static_assert(XOSC_HZ == 12 * MHZ, "the USB PLL settings assume a 12 MHz crystal");
static const usb_pll_setting_t PLL_USB_126MHZ = { 126, 6, 2 };   // 1512 MHz VCO / 6 / 2
static const usb_pll_setting_t PLL_USB_48MHZ  = { 100, 5, 5 };   // 1200 MHz VCO / 5 / 5, the boot setting

// The colour bars' entries, or a picture's entries, scan runs and row table.
static uint32_t s_list[TVOUT_LIST_BYTES / sizeof(uint32_t)];
static uint32_t s_list_start;            // the word the rewind channel copies
static int s_ch_pixel = -1;              // writes words to the HSTX FIFO
static int s_ch_list = -1;               // loads the pixel channel from the list
static int s_ch_rewind = -1;             // restarts the list at the end of a frame
static int s_ch_join = -1;               // picture: sends the join after each row
static int s_ch_row = -1;                // picture: loads each row after the first
static int s_ch_restart = -1;            // picture: puts the join channel back at its join
static int *const s_channels[DMA_CHANNELS_PICTURE] = {
    &s_ch_pixel, &s_ch_list, &s_ch_rewind, &s_ch_join, &s_ch_row, &s_ch_restart,
};
static bool s_running;
static bool s_picture;                   // running a picture, not the bars
static unsigned s_scale = 1;             // frame pixels per picture pixel, each way
static tvout_list_picture_t s_pic;
static const char *s_error = "";

// The last picture started or shown, for the automatic start; NULL is the bars.
static const uint16_t *s_last_pixels;
static int s_last_width;
static int s_last_height;
static unsigned s_last_scale = 1;

static tvout_stats_t s_stats;
static const tvout_ring_ops_t *s_ring;   // the line ring, from the first PSRAM start on
static bool s_last_ring;                 // the last picture was read through it
static uint32_t *s_framing_pool;         // audio framing for the next start; NULL = DVI
static unsigned s_framing_words;
static bool s_framed;                    // the running picture has audio framing
static tvout_plug_t s_plug;
static bool s_auto;

// Restarts the USB PLL with new dividers. False if it does not lock in 20 ms.
static bool usb_pll_set(const usb_pll_setting_t *setting) {
    reset_block(RESETS_RESET_PLL_USB_BITS);
    unreset_block_wait(RESETS_RESET_PLL_USB_BITS);
    pll_usb_hw->cs = 1;
    pll_usb_hw->fbdiv_int = setting->fbdiv;
    hw_clear_bits(&pll_usb_hw->pwr, PLL_PWR_PD_BITS | PLL_PWR_VCOPD_BITS);
    absolute_time_t until = make_timeout_time_ms(20);
    while (!(pll_usb_hw->cs & PLL_CS_LOCK_BITS)) {
        if (time_reached(until)) return false;
    }
    pll_usb_hw->prim = (setting->postdiv1 << PLL_PRIM_POSTDIV1_LSB) |
                       (setting->postdiv2 << PLL_PRIM_POSTDIV2_LSB);
    hw_clear_bits(&pll_usb_hw->pwr, PLL_PWR_POSTDIVPD_BITS);
    return true;
}

static void clock_stop_video(void) {
    reset_block(RESETS_RESET_HSTX_BITS);
    clock_stop(clk_hstx);
    // If the USB PLL does not come back, clk_usb and clk_adc stay where
    // clock_start_video() left them: on the system PLL divide, or stopped.
    if (usb_pll_set(&PLL_USB_48MHZ)) {
        clock_configure(clk_usb, 0, CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                        USB_ADC_CLOCK_HZ, USB_ADC_CLOCK_HZ);
        clock_configure(clk_adc, 0, CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                        USB_ADC_CLOCK_HZ, USB_ADC_CLOCK_HZ);
    }
}

static bool clock_start_video(void) {
    if (tvout_list_pixel_clock_khz() * 1000u * HSTX_CLOCKS_PER_PIXEL != HSTX_CLOCK_HZ) return false;

    // clk_usb and clk_adc take only a whole divide; with no whole divide of
    // clk_sys to 48 MHz they are stopped, not run at another rate.
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    if (sys_hz % USB_ADC_CLOCK_HZ == 0) {
        clock_configure(clk_usb, 0, CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                        sys_hz, USB_ADC_CLOCK_HZ);
        clock_configure(clk_adc, 0, CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                        sys_hz, USB_ADC_CLOCK_HZ);
    } else {
        clock_stop(clk_usb);
        clock_stop(clk_adc);
    }

    if (!usb_pll_set(&PLL_USB_126MHZ)) {
        clock_stop_video();
        return false;
    }

    // HSTX is held in reset while its clock source changes.
    reset_block(RESETS_RESET_HSTX_BITS);
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    HSTX_CLOCK_HZ, HSTX_CLOCK_HZ);
    unreset_block_wait(RESETS_RESET_HSTX_BITS);
    return true;
}

// scale: times each FIFO word of pixels is sent, 1 or 2.
static void hstx_setup(unsigned scale) {
    // Serial output config: clock period of 5 cycles, pop from command
    // expander every 5 cycles, shift the output shiftreg by 2 every cycle.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        HSTX_CLOCKS_PER_PIXEL << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // HSTX outputs 0 through 7 appear on GPIO 12 through 19:
    //
    //   GP12 CK-  GP13 CK+
    //   GP14 D0-  GP15 D0+
    //   GP16 D1-  GP17 D1+
    //   GP18 D2-  GP19 D2+
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    static const int lane_to_output_bit[3] = { 2, 4, 6 };
    for (uint lane = 0; lane < 3; ++lane) {
        int bit = lane_to_output_bit[lane];
        // Output even bits during first half of each HSTX cycle, and odd bits
        // during second half. The shifter advances by two bits each cycle.
        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        // The two halves of each pair get identical data, but one pin is inverted.
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }

    // A pixel reaches the encoder as S = (low byte << 8) | high byte of RGB565,
    // repeated in both halves of the FIFO word. Red is S[7:3], blue is S[12:8],
    // green is S[2:0] above S[15:13]; the copy in the upper half makes the green
    // field contiguous across the wrap.
    hstx_ctrl_hw->expand_tmds =
        4  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        5  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        27 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        4  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        21 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // One pixel per FIFO word, sent scale times: each shift rotates the word
    // by its half, which holds the same pixel. A raw word is used once.
    hstx_ctrl_hw->expand_shift =
        scale << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB    |
        1  << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0  << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    for (int pin = TVOUT_PIN_FIRST; pin <= TVOUT_PIN_LAST; ++pin)
        gpio_set_function(pin, GPIO_FUNC_HSTX);
}

static void scan_start(void) {
    dma_channel_config c;

    // Four words from the list into the pixel channel's CTRL, WRITE_ADDR,
    // TRANS_COUNT and READ_ADDR_TRIG; the write address wraps every 16 bytes.
    c = dma_channel_get_default_config((uint)s_ch_list);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, 4);
    dma_channel_configure((uint)s_ch_list, &c, &dma_hw->ch[s_ch_pixel].al3_ctrl,
                          s_list, 4, false);

    // One word, the list's address, into the list channel's READ_ADDR_TRIG.
    c = dma_channel_get_default_config((uint)s_ch_rewind);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure((uint)s_ch_rewind, &c, &dma_hw->ch[s_ch_list].al3_read_addr_trig,
                          &s_list_start, 1, false);

    if (s_picture && s_pic.joins) {
        // The join: one start sends it, then the row channel.
        c = dma_channel_get_default_config((uint)s_ch_join);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, DREQ_HSTX);
        channel_config_set_chain_to(&c, (uint)s_ch_row);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        dma_channel_configure((uint)s_ch_join, &c, &hstx_fifo_hw->fifo,
                              (const void *)(uintptr_t)s_pic.join_start, s_pic.join_words, false);

        // Two words from the row table into the pixel channel's CTRL and
        // READ_ADDR, the write address wrapping every 8 bytes, then the pixel
        // channel.
        c = dma_channel_get_default_config((uint)s_ch_row);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, true);
        channel_config_set_ring(&c, true, 3);
        channel_config_set_chain_to(&c, (uint)s_ch_pixel);
        dma_channel_configure((uint)s_ch_row, &c, &dma_hw->ch[s_ch_pixel].al1_ctrl,
                              (const void *)(uintptr_t)s_pic.row_start, 2, false);

        // One word, the join's address, into the join channel's READ_ADDR_TRIG.
        c = dma_channel_get_default_config((uint)s_ch_restart);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, false);
        dma_channel_configure((uint)s_ch_restart, &c, &dma_hw->ch[s_ch_join].al3_read_addr_trig,
                              s_pic.join_start_word, 1, false);
    }

    dma_channel_start((uint)s_ch_list);
}

static void scan_stop(void) {
    // Chained channels restart each other if one is aborted while its partner
    // is still enabled (RP2350-E5): enables off on all of them, then one abort
    // for all.
    uint32_t mask = 0;
    for (unsigned i = 0; i < DMA_CHANNELS_PICTURE; i++) {
        const int ch = *s_channels[i];
        if (ch < 0) continue;
        mask |= 1u << ch;
        hw_clear_bits(&dma_hw->ch[ch].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    }
    dma_hw->abort = mask;
    while (dma_hw->abort & mask) tight_loop_contents();

    hstx_ctrl_hw->csr = 0;

    for (int pin = TVOUT_PIN_FIRST; pin <= TVOUT_PIN_LAST; ++pin)
        gpio_set_function(pin, GPIO_FUNC_NULL);
}

// After scan_stop(): the ring's reader reads the scan position from the
// channels until it is closed.
static void release(void) {
    if (s_ring != NULL) s_ring->close();
    for (unsigned i = 0; i < DMA_CHANNELS_PICTURE; i++) {
        if (*s_channels[i] >= 0) dma_channel_unclaim((uint)*s_channels[i]);
        *s_channels[i] = -1;
    }
}

static bool claim_channels(unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        *s_channels[i] = dma_claim_unused_channel(false);
        if (*s_channels[i] < 0) return false;
    }
    return true;
}

static bool in_sram(const uint16_t *pixels, int width, int height) {
    const uintptr_t first = (uintptr_t)pixels;
    const uintptr_t end = first + (uintptr_t)width * (uintptr_t)height * sizeof(uint16_t);
    return first >= SRAM_BASE && end <= SRAM_END && end > first;
}

static uint32_t __not_in_flash_func(scan_piece)(void);

// Builds the list of the bars (pixels NULL) or of a picture shown at scale
// into s_list. False with s_error set.
static bool build_list(const uint16_t *pixels, int width, int height, unsigned scale) {
    // Every entry hands back to the list channel, except a picture row followed
    // by a join, and the last of the frame.
    dma_channel_config c = dma_channel_get_default_config((uint)s_ch_pixel);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_HSTX);
    channel_config_set_chain_to(&c, (uint)s_ch_list);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    tvout_list_config_t cfg = {
        .ctrl_words = channel_config_get_ctrl_value(&c),
        .chain_bits = DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS,
        .chain_shift = DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB,
        .rewind_channel = (uint32_t)s_ch_rewind,
        .fifo = (uint32_t)(uintptr_t)&hstx_fifo_hw->fifo,
    };
    s_list_start = (uint32_t)(uintptr_t)s_list;

    if (pixels == NULL) {
        const unsigned entries = tvout_list_bars_entries();
        if (entries == 0 || entries > TVOUT_LIST_BARS_ENTRIES) {
            s_error = "The picture could not start: it does not fit the TV format.";
            return false;
        }
        if (!tvout_list_build_bars((tvout_list_entry_t *)s_list, entries, &cfg)) {
            s_error = "The picture could not start.";
            return false;
        }
        return true;
    }

    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    cfg.ctrl_pixels = channel_config_get_ctrl_value(&c);
    // One word into the row channel's read address, unpaced.
    channel_config_set_read_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_FORCE);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    cfg.ctrl_copy = channel_config_get_ctrl_value(&c);
    cfg.join_channel = (uint32_t)s_ch_join;
    cfg.restart_channel = (uint32_t)s_ch_restart;
    cfg.row_read_addr = (uint32_t)(uintptr_t)&dma_hw->ch[s_ch_row].read_addr;

    if (!tvout_list_build_picture(s_list, sizeof s_list, pixels, (unsigned)width, (unsigned)height, scale,
                                  &cfg, &s_pic)) {
        s_error = "The picture could not start.";
        return false;
    }
    return true;
}

// The bars when pixels is NULL, otherwise the picture shown at scale, read
// in place from SRAM or, with ring, through the line ring.
static bool start_video(const uint16_t *pixels, int width, int height, unsigned scale,
                        const tvout_ring_ops_t *ring) {
    if (s_running) return true;
    s_error = "";
    // The list's size and layout follow the framing.
    tvout_list_set_audio_framing(s_framing_pool, s_framing_words);

    const int rfid = rfid_reader_state();
    if (rfid == RFID_STATE_STARTING || rfid == RFID_STATE_READING) {
        s_error = "The picture could not start: the RFID reader is running.";
        return false;
    }
    if (pixels != NULL) {
        if (width <= 0 || height <= 0 ||
            tvout_list_picture_bytes((unsigned)width, (unsigned)height, scale) == 0) {
            s_error = "The picture could not start: it does not fit the TV format.";
            return false;
        }
        if (ring == NULL && !in_sram(pixels, width, height)) {
            s_error = "The picture could not start: the pixels are not in SRAM (a PSRAM picture starts with tvout_start_psram()).";
            return false;
        }
    }

    const bool psram = ring != NULL;
    if (psram) s_ring = ring;
    if (psram && !ring->open(pixels, (unsigned)width, (unsigned)height)) {
        s_error = ring->error();
        return false;
    }
    s_picture = pixels != NULL;
    if (!claim_channels(s_picture ? DMA_CHANNELS_PICTURE : DMA_CHANNELS_BARS)) {
        release();
        s_error = "The picture could not start: no free video channel.";
        return false;
    }
    if (!build_list(psram ? ring->lines() : pixels, width, height, scale)) {
        release();
        return false;
    }
    if (psram) ring->attach(&s_pic, scan_piece);
    if (!clock_start_video()) {
        release();
        s_error = "The picture could not start: the video clock did not start.";
        return false;
    }

    hstx_setup(scale);
    scan_start();
    if (psram && !ring->run()) {
        scan_stop();
        clock_stop_video();
        release();
        s_error = ring->error();
        return false;
    }
    s_running = true;
    s_framed = s_framing_pool != NULL;
    s_scale = scale;
    s_last_pixels = pixels;
    s_last_width = width;
    s_last_height = height;
    s_last_scale = scale;
    s_last_ring = psram;
    return true;
}

bool tvout_start_test(void) { return start_video(NULL, 0, 0, 1u, NULL); }

static bool start_picture(const uint16_t *pixels, int width, int height, unsigned scale,
                          const tvout_ring_ops_t *ring) {
    if (pixels == NULL) {
        s_error = "The picture could not start: no pixels.";
        return false;
    }
    return start_video(pixels, width, height, scale, ring);
}

bool tvout_start(const uint16_t *pixels, int width, int height) {
    return start_picture(pixels, width, height, 1u, NULL);
}

bool tvout_start_2x(const uint16_t *pixels, int width, int height) {
    return start_picture(pixels, width, height, 2u, NULL);
}

bool tvout_start_ring(const uint16_t *pixels, int width, int height, const tvout_ring_ops_t *ring) {
    return start_picture(pixels, width, height, 1u, ring);
}

// The list channel's read address and the pixel channel's transfers left,
// taken while the list channel is idle, its count zero and its address the
// same before and after; a copy into the pixel channel's registers lasts a
// few cycles. While the row channel walks the rows its read address is taken
// the same way. A reading that never finds them idle takes the entry as done.
static uint32_t __not_in_flash_func(scan_piece)(void) {
    const dma_channel_hw_t *list = &dma_hw->ch[s_ch_list];
    const dma_channel_hw_t *pixel = &dma_hw->ch[s_ch_pixel];
    const dma_channel_hw_t *row = &dma_hw->ch[s_ch_row];
    uint32_t addr = list->read_addr;
    uint32_t row_addr = row->read_addr;
    uint32_t left = 0;
    for (uint32_t i = 0; i < SCAN_TRIES; i++) {
        const uint32_t list_left = list->transfer_count & DMA_CH0_TRANS_COUNT_COUNT_BITS;
        const uint32_t row_left = row->transfer_count & DMA_CH0_TRANS_COUNT_COUNT_BITS;
        const uint32_t pixel_left = pixel->transfer_count & DMA_CH0_TRANS_COUNT_COUNT_BITS;
        const uint32_t row_left_after = row->transfer_count & DMA_CH0_TRANS_COUNT_COUNT_BITS;
        const uint32_t list_left_after = list->transfer_count & DMA_CH0_TRANS_COUNT_COUNT_BITS;
        const uint32_t addr_after = list->read_addr;
        const uint32_t row_addr_after = row->read_addr;
        const uint32_t loaded = (addr - s_list_start) / sizeof(tvout_list_entry_t);
        const bool rows_idle = s_pic.rows_loaded_at == 0 || loaded != s_pic.rows_loaded_at ||
                               (row_left == 0 && row_left_after == 0 && row_addr_after == row_addr);
        if (list_left == 0 && list_left_after == 0 && addr_after == addr && rows_idle) {
            left = pixel_left;
            break;
        }
        addr = addr_after;
        row_addr = row_addr_after;
    }
    return tvout_list_scan_piece((addr - s_list_start) / sizeof(tvout_list_entry_t), left,
                                 (row_addr - s_pic.row_start) / (2u * sizeof(uint32_t)));
}

bool tvout_show(const uint16_t *pixels) {
    if (!s_running || !s_picture) {
        s_error = "The picture could not change: no picture is running.";
        return false;
    }
    if (s_ring != NULL && s_ring->is_open()) {
        s_error = "The picture could not change: a PSRAM picture has no page flip.";
        return false;
    }
    if (pixels == NULL || !in_sram(pixels, s_last_width, s_last_height)) {
        s_error = "The picture could not change: the pixels are not in SRAM.";
        return false;
    }
    const uint32_t width = (uint32_t)s_last_width;
    const uint32_t scale = s_scale;
    const absolute_time_t until = make_timeout_time_ms(FLIP_TIMEOUT_MS);
    while (true) {
        // Interrupts off so the rewrite, a few microseconds, stays inside the
        // margin.
        const uint32_t irq = save_and_disable_interrupts();
        if (tvout_list_flip_window(scan_piece(), s_pic.first_row, s_pic.rows)) {
            *s_pic.row0_read_addr = (uint32_t)(uintptr_t)pixels;
            for (uint32_t row = 1; row < s_pic.rows; row++)
                s_pic.row_read_addrs[(row - 1u) * 2u] = (uint32_t)(uintptr_t)(pixels + row / scale * width);
            const bool late = !tvout_list_flip_window(scan_piece(), s_pic.first_row, s_pic.rows);
            restore_interrupts(irq);
            s_stats.flips++;
            if (late) s_stats.flips_late++;
            s_last_pixels = pixels;
            return true;
        }
        restore_interrupts(irq);
        if (time_reached(until)) {
            s_error = "The picture could not change: the TV never left the picture rows.";
            return false;
        }
    }
}

void tvout_stop(void) {
    if (!s_running) return;
    tvout_audio_stop();
    scan_stop();
    clock_stop_video();
    release();
    s_running = false;
    s_picture = false;
    s_framed = false;
}

bool tvout_running(void) { return s_running; }

const char *tvout_last_error(void) { return s_error; }

bool tvout_adc_available(void) {
    return (clocks_hw->clk[clk_adc].ctrl & CLOCKS_CLK_ADC_CTRL_ENABLE_BITS) != 0;
}

unsigned tvout_fifo_level(void) {
    if (!s_running) return 0;
    return (unsigned)(hstx_fifo_hw->stat & HSTX_FIFO_STAT_LEVEL_BITS);
}

void tvout_get_stats(tvout_stats_t *out) {
    *out = s_stats;
    if (s_ring != NULL) s_ring->stats(out);
}

tvout_plug_event_t tvout_task(void) {
    const bool line = (uartkbd_flags() & UARTKBD_FLAG_HOTPLUG) != 0;
    const tvout_plug_event_t event =
        tvout_plug_update(&s_plug, line, to_ms_since_boot(get_absolute_time()));
    if (s_auto && event == TVOUT_PLUG_CONNECTED)
        start_video(s_last_pixels, s_last_width, s_last_height, s_last_scale, s_last_ring ? s_ring : NULL);
    else if (s_auto && event == TVOUT_PLUG_REMOVED)
        tvout_stop();
    return event;
}

bool tvout_tv_connected(void) { return s_plug.connected; }

void tvout_set_auto(bool on) { s_auto = on; }

// ---------------------------------------------------------------------------
// For the TV sound feeder (tvout_audio.c): see tvout_internal.h.

void tvout_set_framing_pool(uint32_t *pool, unsigned words) {
    s_framing_pool = pool;
    s_framing_words = pool != NULL ? words : 0u;
}

bool tvout_framed(void) { return s_running && s_framed; }

// The bars' list is one entry per piece: the entries the list channel has
// loaded are the scan position.
uint32_t __not_in_flash_func(tvout_scan_entry)(void) {
    if (!s_running) return 0;
    if (!s_picture) return (dma_hw->ch[s_ch_list].read_addr - s_list_start) / sizeof(tvout_list_entry_t);
    return scan_piece();
}
