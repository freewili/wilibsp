#pragma once

#include <stdint.h>

// Data island coding for an HSTX raw word stream: one 32-bit word per pixel
// clock, lane 0 in bits 0 to 9, lane 1 in bits 10 to 19, lane 2 in bits 20 to 29,
// each symbol with its first transmitted bit lowest. No hardware access, no state:
// every function is reentrant.

// One packet: three header bytes and four subpackets of seven bytes, parity
// bytes not included.
struct stDVIPacket
{
	uint8_t iHeader[3];
	uint8_t iSub[4][7];
};

inline constexpr unsigned int DVI_PACKET_CLOCKS = 32;            // pixel clocks, and raw words, per packet
inline constexpr unsigned int DVI_PREAMBLE_CLOCKS = 8;
inline constexpr unsigned int DVI_GUARD_CLOCKS = 2;
inline constexpr unsigned int DVI_IEC_BLOCK_FRAMES = 192;        // frames per IEC 60958 channel status block
inline constexpr unsigned int DVI_CHANNEL_STATUS_BYTES = 24;     // 192 bits

// 10-bit TERC4 symbol of a 4-bit value.
uint32_t dviTERC4(unsigned int iValue);

// Raw words of the periods around packets. bVLevel and bHLevel are the sync
// wire levels, not "pulse active".
uint32_t dviControlWord(bool bVLevel, bool bHLevel);             // plain control period
uint32_t dviIslandPreambleWord(bool bVLevel, bool bHLevel);      // control, "data island follows"
uint32_t dviVideoPreambleWord(bool bVLevel, bool bHLevel);       // control, "video follows"
uint32_t dviIslandGuardWord(bool bVLevel, bool bHLevel);         // leading and trailing island guard band
uint32_t dviVideoGuardWord();                                    // video leading guard band

// BCH parity byte of iCount bytes: 3 for a header, 7 for a subpacket.
uint8_t dviParity(const uint8_t * pBytes, unsigned int iCount);

// The 32 raw words of one packet as the first packet of a data island.
void dviPacketWords(const stDVIPacket & stPacket, bool bVLevel, bool bHLevel, uint32_t * pWords);

// Packet builders. Each sets every byte of stPacket.
void dviPacketNull(stDVIPacket & stPacket);
// AVI InfoFrame: RGB, full range, 4:3 picture, no bars, no scan information.
void dviPacketAVI(stDVIPacket & stPacket, unsigned int iVIC);
// Audio InfoFrame: 2-channel L-PCM, coding, size and rate from the stream.
void dviPacketAudioInfo(stDVIPacket & stPacket);
// Audio Clock Regeneration: 20-bit N and CTS in all four subpackets.
void dviPacketClock(stDVIPacket & stPacket, unsigned int iN, unsigned int iCTS);
// Audio Sample: iCount (0 to 4) stereo frames of 16-bit samples, interleaved
// left then right. iFrame (0 to 191) is the IEC 60958 frame number of the first;
// pStatus is the 24-byte channel status block. Returns the frame number after
// the last.
unsigned int dviPacketAudio(stDVIPacket & stPacket, const int16_t * pSamples, unsigned int iCount,
	unsigned int iFrame, const uint8_t * pStatus);

// N and CTS for a sample rate at a pixel clock. False unless the rate is 32000,
// 44100 or 48000 Hz and CTS comes out a whole number.
bool dviClockValues(unsigned int iSampleRateHz, unsigned int iPixelClockKHz, unsigned int & iN, unsigned int & iCTS);

// Consumer channel status block for 2-channel 16-bit L-PCM, copying permitted.
// False for a rate other than 32000, 44100 or 48000 Hz.
bool dviChannelStatus(unsigned int iSampleRateHz, uint8_t * pStatus);
