// bsp/rfid/rfid_capture.c — RFID signal capture.
#include "rfid/rfid_capture.h"
#include "platform/board.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"

static uint16_t s_ring[RFID_ADC_RING_WORDS]
    __attribute__((aligned(RFID_ADC_RING_WORDS * sizeof(uint16_t))));
static int      s_dma_chan = -1;
static bool     s_active;

bool rfid_capture_start(void) {
    if (s_active) return true;

    adc_init();
    adc_gpio_init(PIN_RFID_COIL_ADC);
    adc_select_input(RFID_ADC_CHANNEL);

    const uint32_t adc_clk = clock_get_hz(clk_adc);
    const uint32_t div = adc_clk / RFID_ADC_SAMPLE_HZ;
    adc_set_clkdiv((float)(div - 1));
    adc_fifo_setup(true, true, 1, false, false);

    // Skip reserved DMA channel.
    int chan = dma_claim_unused_channel(false);
    if (chan == 5) {
        const int again = dma_claim_unused_channel(false);
        dma_channel_unclaim(5);
        chan = again;
    }
    s_dma_chan = chan;
    if (s_dma_chan < 0) {
        adc_run(false);
        adc_fifo_setup(false, false, 0, false, false);
        return false;
    }

    dma_channel_config c = dma_channel_get_default_config((uint)s_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_ADC);
    channel_config_set_ring(&c, true, 14);

    dma_channel_configure((uint)s_dma_chan, &c,
                          s_ring, &adc_hw->fifo, 0xFFFFFFFFu, false);
    dma_channel_start((uint)s_dma_chan);
    adc_run(true);

    s_active = true;
    return true;
}

void rfid_capture_stop(void) {
    if (!s_active) return;
    adc_run(false);
    adc_fifo_drain();
    if (s_dma_chan >= 0) {
        dma_channel_abort((uint)s_dma_chan);
        dma_channel_unclaim((uint)s_dma_chan);
        s_dma_chan = -1;
    }
    adc_fifo_setup(false, false, 0, false, false);
    s_active = false;
}

bool rfid_capture_active(void) { return s_active; }

const uint16_t *rfid_capture_ring(void) { return s_ring; }

uint32_t rfid_capture_write_pos(void) {
    if (!s_active) return 0;
    const uintptr_t addr = (uintptr_t)dma_channel_hw_addr((uint)s_dma_chan)->write_addr;
    return (uint32_t)((addr - (uintptr_t)s_ring) / sizeof(uint16_t));
}
