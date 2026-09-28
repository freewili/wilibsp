// bsp/audio/audio_i2s_duplex.h — full-duplex I2S on PIO0 SM0 (DAC out GPIO5, ADC in GPIO4).
#ifndef AUDIO_I2S_DUPLEX_H
#define AUDIO_I2S_DUPLEX_H
#include <stdint.h>
#include "hardware/pio.h"

// Start MCLK (256*fs PWM on GPIO22), load the duplex program on PIO0 SM0, set
// pindirs (GPIO5/6/7 out, GPIO4 in) + clkdiv, enable the SM. The DAC idles at
// mid-scale silence until play_loop() is armed.
//
// RX (autopush) starts DISABLED so that playback works standalone. One state
// machine clocks both directions, so an RX FIFO that nobody drains stalls the SM
// and silences the DAC too -- call audio_i2s_duplex_rx_enable() only alongside a
// consumer. audio_capture_start() does this for you.
void audio_i2s_duplex_init(uint32_t sample_rate);

// Enable RX autopush. ONLY call when something drains the RX FIFO every frame --
// audio_capture_start() already does. Playback-only apps must never call this.
void audio_i2s_duplex_rx_enable(void);

// RX FIFO plumbing for audio_capture (same shape as the old RX driver).
volatile const void *audio_i2s_duplex_rxf(void);
uint audio_i2s_duplex_rx_dreq(void);

// Arm a zero-CPU DMA read-ring that loops `buf` (frames, each uint32 = [L16|R16])
// to the TX FIFO forever. `buf` MUST be aligned to its byte size and the byte size
// MUST be a power of two (ring requirement). For 64 frames -> 256 bytes, aligned(256).
void audio_i2s_duplex_play_loop(const uint32_t *buf, uint frames);

// Same idea, but refilled from an interrupt in 8192-frame chunks, so `buf` does
// not have to be ring-aligned. `frames` MUST be a non-zero multiple of 8192 and
// at least 16384: the second DMA channel is armed one chunk in, and the refill
// advances by whole chunks modulo `frames`. A shorter or unaligned length points
// a chunk-sized read past the end of the buffer, and zero divides by zero in the
// refill. Callers pad up to that today (see apps/retrochat/audio_glue.c).
void audio_i2s_duplex_play_stream_loop(const uint32_t *buf, uint frames);

// Stop playback: abort the TX DMA and clear the TX FIFO (DAC mid-scale silence).
void audio_i2s_duplex_play_stop(void);

#endif
