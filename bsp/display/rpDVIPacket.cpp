#include "rpDVIPacket.h"

namespace {

// TERC4 symbols, q[9:0], for the values 0 to 15.
const uint16_t TERC4[16] = {
	0x29c, 0x263, 0x2e4, 0x2e2, 0x171, 0x11e, 0x18e, 0x13c,
	0x2cc, 0x139, 0x19c, 0x2c6, 0x28e, 0x271, 0x163, 0x2c3
};

// TMDS control symbols for the bit pairs 00, 01, 10, 11 (D1 high, D0 low).
const uint16_t CTRL[4] = { 0x354, 0x0ab, 0x154, 0x2ab };

// Guard band symbols.
const uint32_t GUARD_133 = 0x133u;          // lanes 1 and 2 of an island guard band, lane 1 of a video one
const uint32_t GUARD_2CC = 0x2ccu;          // lanes 0 and 2 of a video guard band

// Parity register after four input bits, for each low nibble of the register.
// Generator x^8 + x^7 + x^6 + 1, bits taken lowest first.
const uint8_t BCH_NIBBLE[16] = {
	0x00, 0xb3, 0x61, 0xd2, 0xc2, 0x71, 0xa3, 0x10,
	0x83, 0x30, 0xe2, 0x51, 0x41, 0xf2, 0x20, 0x93
};

// Recommended N for each supported sample rate.
struct stClockN
{
	unsigned int iRateHz;
	unsigned int iN;
	uint8_t iStatusRate;                    // channel status byte 3
};
const stClockN CLOCK_N[3] = {
	{ 32000, 4096, 0x03 },
	{ 44100, 6272, 0x00 },
	{ 48000, 6144, 0x02 },
};

inline uint32_t rawWord(uint32_t iLane0, uint32_t iLane1, uint32_t iLane2)
{
	return iLane0 | (iLane1 << 10) | (iLane2 << 20);
}

inline unsigned int syncBits(bool bVLevel, bool bHLevel)
{
	return (bVLevel ? 2u : 0u) | (bHLevel ? 1u : 0u);
}

// Even parity of 16 bits.
inline unsigned int parity16(unsigned int iValue)
{
	iValue ^= iValue >> 8;
	iValue ^= iValue >> 4;
	iValue ^= iValue >> 2;
	iValue ^= iValue >> 1;
	return iValue & 1u;
}

const stClockN * findRate(unsigned int iSampleRateHz)
{
	for (const stClockN & stRate : CLOCK_N)
		if (stRate.iRateHz == iSampleRateHz)
			return &stRate;
	return nullptr;
}

// InfoFrame: header, payload bytes PB1 onwards, and the PB0 checksum that makes
// the header and payload sum to zero.
void setInfoFrame(stDVIPacket & stPacket, uint8_t iType, uint8_t iVersion, const uint8_t * pPayload, unsigned int iLength)
{
	uint8_t * pBytes = &stPacket.iSub[0][0];
	for (unsigned int i = 0; i < sizeof(stPacket.iSub); i++)
		pBytes[i] = 0;
	stPacket.iHeader[0] = iType;
	stPacket.iHeader[1] = iVersion;
	stPacket.iHeader[2] = (uint8_t)iLength;

	// PB0 to PB6 in subpacket 0, PB7 to PB13 in subpacket 1, and so on.
	unsigned int iSum = iType + iVersion + iLength;
	for (unsigned int i = 0; i < iLength; i++)
	{
		const unsigned int iPB = i + 1u;
		stPacket.iSub[iPB / 7u][iPB % 7u] = pPayload[i];
		iSum += pPayload[i];
	}
	stPacket.iSub[0][0] = (uint8_t)(0x100u - (iSum & 0xffu));
}

}

uint32_t dviTERC4(unsigned int iValue)
{
	return TERC4[iValue & 0xfu];
}

uint32_t dviControlWord(bool bVLevel, bool bHLevel)
{
	return rawWord(CTRL[syncBits(bVLevel, bHLevel)], CTRL[0], CTRL[0]);
}

uint32_t dviIslandPreambleWord(bool bVLevel, bool bHLevel)
{
	return rawWord(CTRL[syncBits(bVLevel, bHLevel)], CTRL[1], CTRL[1]);
}

uint32_t dviVideoPreambleWord(bool bVLevel, bool bHLevel)
{
	return rawWord(CTRL[syncBits(bVLevel, bHLevel)], CTRL[1], CTRL[0]);
}

// Lane 0 carries 1, 1, VSYNC, HSYNC.
uint32_t dviIslandGuardWord(bool bVLevel, bool bHLevel)
{
	return rawWord(TERC4[0xcu | syncBits(bVLevel, bHLevel)], GUARD_133, GUARD_133);
}

uint32_t dviVideoGuardWord()
{
	return rawWord(GUARD_2CC, GUARD_133, GUARD_2CC);
}

uint8_t dviParity(const uint8_t * pBytes, unsigned int iCount)
{
	unsigned int iReg = 0;
	for (unsigned int i = 0; i < iCount; i++)
	{
		iReg ^= pBytes[i];
		iReg = (iReg >> 4) ^ BCH_NIBBLE[iReg & 0xfu];
		iReg = (iReg >> 4) ^ BCH_NIBBLE[iReg & 0xfu];
	}
	return (uint8_t)iReg;
}

// Packet clock k: lane 0 bit 2 is header bit k, bit 3 is 0 on clock 0 only;
// lane 1 bit i is subpacket i bit 2k, lane 2 bit i is subpacket i bit 2k + 1.
void dviPacketWords(const stDVIPacket & stPacket, bool bVLevel, bool bHLevel, uint32_t * pWords)
{
	const uint32_t iHeader = (uint32_t)stPacket.iHeader[0] | ((uint32_t)stPacket.iHeader[1] << 8) |
		((uint32_t)stPacket.iHeader[2] << 16) | ((uint32_t)dviParity(stPacket.iHeader, 3) << 24);

	uint64_t iSub[4];
	for (unsigned int i = 0; i < 4; i++)
	{
		uint64_t iBits = (uint64_t)dviParity(stPacket.iSub[i], 7) << 56;
		for (unsigned int iByte = 0; iByte < 7; iByte++)
			iBits |= (uint64_t)stPacket.iSub[i][iByte] << (8u * iByte);
		iSub[i] = iBits;
	}

	const unsigned int iSync = syncBits(bVLevel, bHLevel);
	for (unsigned int k = 0; k < DVI_PACKET_CLOCKS; k++)
	{
		const unsigned int iLane0 = iSync | (((iHeader >> k) & 1u) << 2) | (k != 0 ? 8u : 0u);
		unsigned int iLane1 = 0;
		unsigned int iLane2 = 0;
		for (unsigned int i = 0; i < 4; i++)
		{
			iLane1 |= (unsigned int)(iSub[i] & 1u) << i;
			iLane2 |= (unsigned int)((iSub[i] >> 1) & 1u) << i;
			iSub[i] >>= 2;
		}
		pWords[k] = rawWord(TERC4[iLane0], TERC4[iLane1], TERC4[iLane2]);
	}
}

void dviPacketNull(stDVIPacket & stPacket)
{
	uint8_t * pBytes = &stPacket.iHeader[0];
	for (unsigned int i = 0; i < sizeof(stPacket); i++)
		pBytes[i] = 0;
}

// PB1 0x10: RGB, active format present. PB2 0x18: 4:3, active format same as
// picture. PB3 0x08: full-range RGB. PB4: VIC.
void dviPacketAVI(stDVIPacket & stPacket, unsigned int iVIC)
{
	const uint8_t iPayload[13] = { 0x10, 0x18, 0x08, (uint8_t)(iVIC & 0x7fu), 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	setInfoFrame(stPacket, 0x82, 0x02, iPayload, 13);
}

// PB1 0x01: two channels, coding type from the stream. PB2 to PB5 zero: size and
// rate from the stream, front left and front right.
void dviPacketAudioInfo(stDVIPacket & stPacket)
{
	const uint8_t iPayload[10] = { 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	setInfoFrame(stPacket, 0x84, 0x01, iPayload, 10);
}

// Each subpacket: SB0 zero, SB1 to SB3 CTS high byte first, SB4 to SB6 N.
void dviPacketClock(stDVIPacket & stPacket, unsigned int iN, unsigned int iCTS)
{
	stPacket.iHeader[0] = 0x01;
	stPacket.iHeader[1] = 0;
	stPacket.iHeader[2] = 0;
	for (unsigned int i = 0; i < 4; i++)
	{
		uint8_t * pSub = stPacket.iSub[i];
		pSub[0] = 0;
		pSub[1] = (uint8_t)((iCTS >> 16) & 0x0fu);
		pSub[2] = (uint8_t)(iCTS >> 8);
		pSub[3] = (uint8_t)iCTS;
		pSub[4] = (uint8_t)((iN >> 16) & 0x0fu);
		pSub[5] = (uint8_t)(iN >> 8);
		pSub[6] = (uint8_t)iN;
	}
}

// Header: HB1 bits 0 to 3 sample present, layout 0; HB2 bits 4 to 7 the block
// start flag of each subpacket. Subpacket: 24-bit left sample in SB0 to SB2 with
// the 16-bit sample in SB1 and SB2, right sample in SB3 to SB5, SB6 the V, U, C
// and P bits of left (bits 0 to 3) and right (bits 4 to 7). P makes the 24
// sample bits with V, U, C and P even.
unsigned int dviPacketAudio(stDVIPacket & stPacket, const int16_t * pSamples, unsigned int iCount,
	unsigned int iFrame, const uint8_t * pStatus)
{
	if (iCount > 4)
		iCount = 4;
	if (iFrame >= DVI_IEC_BLOCK_FRAMES)
		iFrame %= DVI_IEC_BLOCK_FRAMES;

	unsigned int iPresent = 0;
	unsigned int iBlockStart = 0;
	for (unsigned int i = 0; i < 4; i++)
	{
		uint8_t * pSub = stPacket.iSub[i];
		if (i >= iCount)
		{
			for (unsigned int iByte = 0; iByte < 7; iByte++)
				pSub[iByte] = 0;
			continue;
		}
		const unsigned int iLeft = (uint16_t)pSamples[2u * i];
		const unsigned int iRight = (uint16_t)pSamples[2u * i + 1u];
		const unsigned int iC = (pStatus[iFrame >> 3] >> (iFrame & 7u)) & 1u;
		iPresent |= 1u << i;
		if (iFrame == 0)
			iBlockStart |= 1u << i;

		pSub[0] = 0;
		pSub[1] = (uint8_t)iLeft;
		pSub[2] = (uint8_t)(iLeft >> 8);
		pSub[3] = 0;
		pSub[4] = (uint8_t)iRight;
		pSub[5] = (uint8_t)(iRight >> 8);
		pSub[6] = (uint8_t)((iC << 2) | ((parity16(iLeft) ^ iC) << 3) |
			(iC << 6) | ((parity16(iRight) ^ iC) << 7));

		iFrame = (iFrame + 1u == DVI_IEC_BLOCK_FRAMES) ? 0u : iFrame + 1u;
	}
	stPacket.iHeader[0] = 0x02;
	stPacket.iHeader[1] = (uint8_t)iPresent;
	stPacket.iHeader[2] = (uint8_t)(iBlockStart << 4);
	return iFrame;
}

// 128 x fs = f_pixel x N / CTS.
bool dviClockValues(unsigned int iSampleRateHz, unsigned int iPixelClockKHz, unsigned int & iN, unsigned int & iCTS)
{
	const stClockN * pRate = findRate(iSampleRateHz);
	if (pRate == nullptr)
		return false;
	const uint64_t iNumerator = (uint64_t)iPixelClockKHz * 1000u * pRate->iN;
	const uint64_t iDenominator = 128u * (uint64_t)iSampleRateHz;
	if (iNumerator % iDenominator != 0 || iNumerator / iDenominator >= (1u << 20))
		return false;
	iN = pRate->iN;
	iCTS = (unsigned int)(iNumerator / iDenominator);
	return true;
}

// Byte 0 0x04: consumer, L-PCM, copying permitted, no emphasis. Byte 3: sample
// rate. Byte 4 0x02: 16-bit words, 20-bit maximum.
bool dviChannelStatus(unsigned int iSampleRateHz, uint8_t * pStatus)
{
	const stClockN * pRate = findRate(iSampleRateHz);
	if (pRate == nullptr)
		return false;
	for (unsigned int i = 0; i < DVI_CHANNEL_STATUS_BYTES; i++)
		pStatus[i] = 0;
	pStatus[0] = 0x04;
	pStatus[3] = pRate->iStatusRate;
	pStatus[4] = 0x02;
	return true;
}
