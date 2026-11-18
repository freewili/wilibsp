#pragma once

#include <stdint.h>

// Audio slot bookkeeping and packet coding of the TV sound feeder
// (rpDVIAudio). No hardware and no clock: the caller reads the scan-out
// position and the core decides which slots to write, encodes the packets and
// writes them. The tick functions run in an interrupt on one core;
// audioPcmWrite() runs on either core, one caller at a time.
//
// Lines are numbered as rpDVIFrame numbers them, 0 being the first line of the
// vertical front porch. A line with an audio slot sends the packet in that slot
// near its start. Slots are shared by lines in rings, so the packet for a line
// is written after the list channel has fetched the slot for the line that used
// it last, and before it starts on the line itself.
//
// The samples each packet carries follow from the line count alone
// (audioCadenceBuild). A line that is not written in time sends what its slot
// still holds: the packet of the line before it in the same ring, which the TV
// has already played. Nothing written after the fact can stop that, so such
// lines are counted, their own samples are dropped to keep the rest on time,
// and a held packet that carries samples is counted as a repeat.

inline constexpr uint32_t DVI_AUDIO_MAX_SLOTS = 25;            // two rings of eight and the vertical blanking's nine
inline constexpr uint32_t DVI_AUDIO_SAMPLES_PER_PACKET = 4;
inline constexpr uint32_t DVI_AUDIO_LINES_AHEAD = 7;          // lines written past the scan line; at most the ring length less 1
inline constexpr uint32_t DVI_AUDIO_TICK_BUDGET = 140;        // work per tick, in units of about 40 cycles: 23 us at 240 MHz
inline constexpr uint32_t DVI_AUDIO_COST_SAMPLES = 45;        // a sample packet made and written: about 1800 cycles
inline constexpr uint32_t DVI_AUDIO_COST_NULL = 4;            // a null packet written
inline constexpr uint32_t DVI_AUDIO_COST_MISSED = 1;          // a line accounted as missed
inline constexpr uint32_t DVI_AUDIO_COST_COLD = 64;           // missed lines accounted from flash (DVI_AUDIO_COLD): about 2600 cycles
inline constexpr uint32_t DVI_AUDIO_TICK_GAP_US = 8;          // from a tick that stopped at a limit to the next: other interrupts run here
inline constexpr uint32_t DVI_AUDIO_TICK_IDLE_US = 24;        // from a tick that caught up to the next: a tick about every TV line
inline constexpr uint32_t DVI_AUDIO_PCM_FRAMES = 128;          // stereo frames buffered: 2.7 ms at 48 kHz
inline constexpr uint32_t DVI_AUDIO_TONE_STEPS = 48;           // one period of 1 kHz at 48 kHz
inline constexpr uint32_t DVI_AUDIO_TONE_HZ = 1000;
inline constexpr uint32_t DVI_AUDIO_TONE_LEFT = 1;
inline constexpr uint32_t DVI_AUDIO_TONE_RIGHT = 2;

static_assert((DVI_AUDIO_PCM_FRAMES & (DVI_AUDIO_PCM_FRAMES - 1)) == 0, "the sample buffer index wraps by masking");
static_assert(DVI_AUDIO_MAX_SLOTS <= 31, "a line's slot number and one are held in five bits");

// One period of the tone, -12 dBFS.
inline constexpr int16_t DVI_AUDIO_TONE[DVI_AUDIO_TONE_STEPS] = {
	0, 1069, 2120, 3135, 4096, 4987, 5793, 6499, 7094, 7568, 7913, 8122,
	8192, 8122, 7913, 7568, 7094, 6499, 5793, 4987, 4096, 3135, 2120, 1069,
	0, -1069, -2120, -3135, -4096, -4987, -5793, -6499, -7094, -7568, -7913, -8122,
	-8192, -8122, -7913, -7568, -7094, -6499, -5793, -4987, -4096, -3135, -2120, -1069
};

// TERC4 symbols, q[9:0], for the values 0 to 15.
inline constexpr uint16_t DVI_AUDIO_TERC4[16] = {
	0x29c, 0x263, 0x2e4, 0x2e2, 0x171, 0x11e, 0x18e, 0x13c,
	0x2cc, 0x139, 0x19c, 0x2c6, 0x28e, 0x271, 0x163, 0x2c3
};

// BCH parity register after four input bits, for each low nibble of the
// register. Generator x^8 + x^7 + x^6 + 1, bits taken lowest first.
inline constexpr uint8_t DVI_AUDIO_BCH_NIBBLE[16] = {
	0x00, 0xb3, 0x61, 0xd2, 0xc2, 0x71, 0xa3, 0x10,
	0x83, 0x30, 0xe2, 0x51, 0x41, 0xf2, 0x20, 0x93
};

// A line's byte in the line table: slot number plus one (0 = no slot) in bits
// 0 to 4, samples its packet carries in bits 5 to 7.
inline constexpr uint32_t DVI_AUDIO_LINE_SLOT_MASK = 0x1f;
inline constexpr uint32_t DVI_AUDIO_LINE_COUNT_SHIFT = 5;

struct stDVIAudioCore
{
	// The frame.
	uint32_t iLines;                    // lines per frame
	uint32_t iListCount;                // list entries per frame
	uint32_t iFirstRowEntry;            // entry of the first row's pixels; 0 with one entry per line
	uint32_t iBelowEntry;               // entry of the first line below the picture
	uint32_t iSlots;
	uint32_t iAhead;                    // lines written past the scan line
	uint8_t * pLine;                    // the line table, iLines bytes
	uint32_t * pSlot[DVI_AUDIO_MAX_SLOTS];

	// Packet coding, copied to RAM.
	uint32_t iSync;                     // sync wire levels: vertical bit 1, horizontal bit 0
	uint32_t iNull[2];                  // null packet: first word, every other word
	uint16_t iTERC4[16];
	uint8_t iBCH[16];
	uint8_t iStatus[24];                // IEC 60958 channel status block
	uint32_t iFrame;                    // IEC 60958 frame number of the next sample sent

	// Sample sources.
	uint32_t * pPcm;                    // DVI_AUDIO_PCM_FRAMES frames, left in the low half
	volatile uint32_t iPcmHead;         // frames written, written by audioPcmWrite()
	volatile uint32_t iPcmTail;         // frames taken, written by the tick
	volatile uint32_t iTone;            // DVI_AUDIO_TONE_LEFT and DVI_AUDIO_TONE_RIGHT bits; 0 = buffer
	uint32_t iTonePhase;                // 16.16 steps into DVI_AUDIO_TONE
	uint32_t iToneStep;

	// Position. Lines are counted from the first tick on; differences wrap.
	bool bStarted;
	uint32_t iScan;                     // audioScanLine() at the last reading
	uint32_t iScanLine;                 // the same, within the frame
	uint32_t iNext;                     // the next line to write
	uint32_t iNextLine;                 // the same, within the frame
	uint32_t iSlotAudio;                // bit per slot: it holds a packet with samples

	// The tick.
	volatile bool bRunning;
	volatile bool bMuting;              // stop: every slot to a null packet
	volatile bool bMuted;
	uint32_t iTickCost;                 // DVI_AUDIO_TICK_BUDGET units spent in this tick
	bool bMore;                         // the tick stopped at a limit
	uint32_t iPendSlot;                 // the packet audioTickPrepare() made
	uint32_t iPendCount;
	uint32_t iPendTaken;                // of those, frames taken from the source
	uint32_t iPendFrame;

	// Counts since audioInit().
	volatile uint32_t iPackets;         // sample packets written in time
	volatile uint32_t iUnderruns;       // frames sent as silence: the buffer was empty
	volatile uint32_t iLate;            // lines the TV reached before they were written
	volatile uint32_t iRepeats;         // of those, lines that sent a packet with samples again
	volatile uint32_t iLost;            // frames dropped on those lines
};

#define DVI_AUDIO_INLINE static inline __attribute__((always_inline))
// Tick paths taken seldom: lines missed, stopping. Out of line, so the tick's
// own code holds the packet path only; on the RP2350 they run from flash
// (rpDVIAudio.cpp).
#define DVI_AUDIO_COLD static __attribute__((noinline))

// Bytes of the buffer audioInit() takes: the line table, then the sample buffer.
DVI_AUDIO_INLINE uint32_t audioBufferBytes(uint32_t iLines)
{
	return ((iLines + 3u) & ~3u) + DVI_AUDIO_PCM_FRAMES * 4u;
}

// ----------------------------------------------------------------------------
// Set-up, before the first tick.

// iListCount entries, iRows of them picture rows from iFirstRowEntry on (none for
// a frame of one entry per line).
DVI_AUDIO_INLINE void audioInit(stDVIAudioCore & st, void * pBuffer, uint32_t iLines, uint32_t iListCount,
	uint32_t iFirstRowEntry, uint32_t iRows)
{
	st.iLines = iLines;
	st.iListCount = iListCount;
	st.iFirstRowEntry = iFirstRowEntry;
	st.iBelowEntry = iListCount + iFirstRowEntry + iRows - iLines - 1u;
	st.iSlots = 0;
	st.iAhead = 0;
	st.pLine = (uint8_t *)pBuffer;
	st.pPcm = (uint32_t *)((uint8_t *)pBuffer + ((iLines + 3u) & ~3u));
	for (uint32_t iLine = 0; iLine < iLines; iLine++)
		st.pLine[iLine] = 0;
	for (uint32_t iSlot = 0; iSlot < DVI_AUDIO_MAX_SLOTS; iSlot++)
		st.pSlot[iSlot] = nullptr;
	st.iFrame = 0;
	st.iPcmHead = 0;
	st.iPcmTail = 0;
	st.iTonePhase = 0;
	st.iToneStep = 0;
	st.bStarted = false;
	st.iScan = 0;
	st.iScanLine = 0;
	st.iNext = 0;
	st.iNextLine = 0;
	st.iSlotAudio = 0;
	st.bRunning = false;
	st.bMuting = false;
	st.bMuted = false;
	st.iTickCost = 0;
	st.bMore = false;
	st.iPendSlot = 0;
	st.iPendCount = 0;
	st.iPendTaken = 0;
	st.iPendFrame = 0;
	st.iPackets = 0;
	st.iUnderruns = 0;
	st.iLate = 0;
	st.iRepeats = 0;
	st.iLost = 0;
}

// iSlot < 0: the line has no audio slot.
DVI_AUDIO_INLINE void audioSetLine(stDVIAudioCore & st, uint32_t iLine, int iSlot)
{
	if (iLine < st.iLines)
		st.pLine[iLine] = (iSlot >= 0 && iSlot < (int)DVI_AUDIO_MAX_SLOTS) ? (uint8_t)(iSlot + 1) : 0;
}

DVI_AUDIO_INLINE void audioSetSlot(stDVIAudioCore & st, uint32_t iSlot, uint32_t * pWords)
{
	if (iSlot < DVI_AUDIO_MAX_SLOTS)
		st.pSlot[iSlot] = pWords;
}

// Samples each line's packet carries. The count due grows by iRateHz / iLineHz
// on every line. A line with a slot sends four once four are due, unless the
// line with a slot before it sent samples; the last line with a slot before a
// line without one sends what is due, up to four. Lines without a slot (vertical
// sync, the InfoFrame and clock regeneration lines) send nothing; the samples
// due on them go out on the lines after them at up to four every second line,
// two a line against the 1.52 that accrue at 48 kHz. Whole packets on no two lines in a row halve the packets to
// encode against sending what is due on each line, and keep it to one every
// second line however far behind the feeder is.
//
// The pattern repeats every frame when a frame holds a whole number of samples
// (800 at 48 kHz, 735 at 44.1 kHz with 525 lines at 31.5 kHz). The first two
// frames settle it, the third is checked against the second. False when the
// rate does not divide, or a frame does not send all it accrues.
DVI_AUDIO_INLINE bool audioCadenceBuild(uint8_t * pLine, uint32_t iLines, uint32_t iRateHz, uint32_t iLineHz)
{
	if (iLines == 0 || iLineHz == 0 || ((uint64_t)iRateHz * iLines) % iLineHz != 0)
		return false;
	const uint32_t iPerFrame = (uint32_t)(((uint64_t)iRateHz * iLines) / iLineHz);
	uint32_t iAcc = 0;
	uint32_t iDue = 0;
	bool bSent = false;
	for (uint32_t iPass = 0; iPass < 3; iPass++)
	{
		uint32_t iSent = 0;
		for (uint32_t iLine = 0; iLine < iLines; iLine++)
		{
			iAcc += iRateHz;
			while (iAcc >= iLineHz)
			{
				iAcc -= iLineHz;
				iDue++;
			}
			uint32_t iCount = 0;
			if ((pLine[iLine] & DVI_AUDIO_LINE_SLOT_MASK) != 0)
			{
				const bool bFlush = (pLine[iLine + 1u == iLines ? 0u : iLine + 1u] & DVI_AUDIO_LINE_SLOT_MASK) == 0;
				if (bFlush || (!bSent && iDue >= DVI_AUDIO_SAMPLES_PER_PACKET))
					iCount = iDue < DVI_AUDIO_SAMPLES_PER_PACKET ? iDue : DVI_AUDIO_SAMPLES_PER_PACKET;
				iDue -= iCount;
				bSent = iCount != 0;
			}
			iSent += iCount;
			const uint8_t iByte = (uint8_t)((pLine[iLine] & DVI_AUDIO_LINE_SLOT_MASK) | (iCount << DVI_AUDIO_LINE_COUNT_SHIFT));
			if (iPass == 1)
				pLine[iLine] = iByte;
			else if (iPass == 2 && pLine[iLine] != iByte)
				return false;
		}
		if (iPass == 2 && iSent != iPerFrame)
			return false;
	}
	return true;
}

// Checks the slot map, builds the cadence and the coding tables. False when the
// map refers to a slot it was not given, the rate does not fit the line rate,
// or two lines sharing a slot are too close to write between them.
DVI_AUDIO_INLINE bool audioConfigure(stDVIAudioCore & st, uint32_t iSlots, uint32_t iRateHz, uint32_t iLineHz,
	bool bVLevel, bool bHLevel, const uint8_t * pStatus)
{
	if (iSlots == 0 || iSlots > DVI_AUDIO_MAX_SLOTS || iRateHz == 0)
		return false;
	for (uint32_t iSlot = 0; iSlot < iSlots; iSlot++)
		if (st.pSlot[iSlot] == nullptr)
			return false;
	for (uint32_t iLine = 0; iLine < st.iLines; iLine++)
		if ((st.pLine[iLine] & DVI_AUDIO_LINE_SLOT_MASK) > iSlots)
			return false;
	if (!audioCadenceBuild(st.pLine, st.iLines, iRateHz, iLineHz))
		return false;

	// The shortest run from one line to the next line on the same slot, round
	// the end of the frame. A slot is written once the scan line (audioScanLine)
	// is past its last use and before it reaches the next, so a line is written
	// at most that run less one line ahead of the scan line.
	uint32_t iLast[DVI_AUDIO_MAX_SLOTS];
	for (uint32_t iSlot = 0; iSlot < DVI_AUDIO_MAX_SLOTS; iSlot++)
		iLast[iSlot] = UINT32_MAX;
	uint32_t iShortest = 2u * st.iLines;
	for (uint32_t i = 0; i < 2u * st.iLines; i++)
	{
		const uint32_t iSlotBits = st.pLine[i < st.iLines ? i : i - st.iLines] & DVI_AUDIO_LINE_SLOT_MASK;
		if (iSlotBits == 0)
			continue;
		if (iLast[iSlotBits - 1u] != UINT32_MAX && i - iLast[iSlotBits - 1u] < iShortest)
			iShortest = i - iLast[iSlotBits - 1u];
		iLast[iSlotBits - 1u] = i;
	}
	if (iShortest < 2)
		return false;
	st.iSlots = iSlots;
	st.iAhead = iShortest - 1u < DVI_AUDIO_LINES_AHEAD ? iShortest - 1u : DVI_AUDIO_LINES_AHEAD;

	for (uint32_t i = 0; i < 16; i++)
	{
		st.iTERC4[i] = DVI_AUDIO_TERC4[i];
		st.iBCH[i] = DVI_AUDIO_BCH_NIBBLE[i];
	}
	for (uint32_t i = 0; i < 24; i++)
		st.iStatus[i] = pStatus[i];
	// A null packet is all zero with zero parity: lane 0 carries the sync
	// levels, and bit 3 on every clock but the first.
	st.iSync = (bVLevel ? 2u : 0u) | (bHLevel ? 1u : 0u);
	const uint32_t iZero = ((uint32_t)st.iTERC4[0] << 10) | ((uint32_t)st.iTERC4[0] << 20);
	st.iNull[0] = st.iTERC4[st.iSync] | iZero;
	st.iNull[1] = st.iTERC4[st.iSync | 8u] | iZero;
	st.iToneStep = (uint32_t)(((uint64_t)DVI_AUDIO_TONE_STEPS * DVI_AUDIO_TONE_HZ << 16) / iRateHz);
	return true;
}

// ----------------------------------------------------------------------------
// The scan position.

// The scan line, from the scan position (the piece after the one being sent,
// counted as if each piece were one list entry, rpDVI::scanEntry): the line
// whose island is being fetched or comes next. Islands of earlier
// lines have been fetched. The next island after it is at least a whole line
// piece or a picture row away, and so at least 4 us, the 100 pixel clocks of
// sync and back porch, after the list channel has left the piece it is on: a
// slot is written straight after the scan line is read.
//
// A line outside the picture, and the first row's head, is one entry. A picture
// row is two entries: its pixels, then the piece that joins it to the next row
// and holds the next row's island. A row's pixels end a few pixel clocks before
// that island, so the scan line there is already the next row's.
DVI_AUDIO_INLINE uint32_t audioScanLine(const stDVIAudioCore & st, uint32_t iNextEntry)
{
	if (iNextEntry == 0 || iNextEntry > st.iListCount)
		iNextEntry = st.iListCount;                         // rewinding: the last entry
	const uint32_t iEntry = iNextEntry - 1u;
	if (st.iListCount == st.iLines || iEntry < st.iFirstRowEntry)
		return iEntry;
	if (iEntry < st.iBelowEntry)
		return st.iFirstRowEntry + (iEntry - st.iFirstRowEntry) / 2u;
	return iEntry + st.iLines - st.iListCount;
}

DVI_AUDIO_INLINE uint32_t audioLineSlotBits(const stDVIAudioCore & st, uint32_t iLine)
{
	return st.pLine[iLine] & DVI_AUDIO_LINE_SLOT_MASK;
}

// The scan line moves forward only, by less than a frame between two readings.
DVI_AUDIO_INLINE void audioScanTo(stDVIAudioCore & st, uint32_t iLine)
{
	const uint32_t iDelta = iLine >= st.iScanLine ? iLine - st.iScanLine : iLine + st.iLines - st.iScanLine;
	st.iScan += iDelta;
	st.iScanLine = iLine;
}

DVI_AUDIO_INLINE void audioStep(stDVIAudioCore & st)
{
	st.iNext++;
	st.iNextLine = st.iNextLine + 1u == st.iLines ? 0u : st.iNextLine + 1u;
}

// ----------------------------------------------------------------------------
// Samples.

// iFrames stereo frames into the buffer; returns how many fitted.
DVI_AUDIO_INLINE uint32_t audioPcmWrite(stDVIAudioCore & st, const int16_t * pStereo, uint32_t iFrames)
{
	const uint32_t iHead = st.iPcmHead;
	const uint32_t iTail = st.iPcmTail;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	const uint32_t iFree = DVI_AUDIO_PCM_FRAMES - (iHead - iTail);
	const uint32_t iCount = iFrames < iFree ? iFrames : iFree;
	for (uint32_t i = 0; i < iCount; i++)
		st.pPcm[(iHead + i) & (DVI_AUDIO_PCM_FRAMES - 1u)] =
			(uint32_t)(uint16_t)pStereo[2u * i] | ((uint32_t)(uint16_t)pStereo[2u * i + 1u] << 16);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	st.iPcmHead = iHead + iCount;
	return iCount;
}

// Up to iFrames frames out of the buffer into pStereo, or only dropped when
// pStereo is nullptr. Returns how many there were.
DVI_AUDIO_INLINE uint32_t audioPcmTake(stDVIAudioCore & st, int16_t * pStereo, uint32_t iFrames)
{
	const uint32_t iTail = st.iPcmTail;
	const uint32_t iHead = st.iPcmHead;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	const uint32_t iCount = iFrames < iHead - iTail ? iFrames : iHead - iTail;
	if (pStereo != nullptr)
		for (uint32_t i = 0; i < iCount; i++)
		{
			const uint32_t iWord = st.pPcm[(iTail + i) & (DVI_AUDIO_PCM_FRAMES - 1u)];
			pStereo[2u * i] = (int16_t)(uint16_t)iWord;
			pStereo[2u * i + 1u] = (int16_t)(uint16_t)(iWord >> 16);
		}
	__atomic_thread_fence(__ATOMIC_RELEASE);
	st.iPcmTail = iTail + iCount;
	return iCount;
}

// The frames of one packet: the tone, or the buffer with silence for what it
// is short of. With the tone on, the buffer is drained at the same rate.
// Returns the frames that are not silence put in for a short buffer.
DVI_AUDIO_INLINE uint32_t audioTake(stDVIAudioCore & st, int16_t * pStereo, uint32_t iFrames)
{
	const uint32_t iTone = st.iTone;
	if (iTone != 0)
	{
		audioPcmTake(st, nullptr, iFrames);
		for (uint32_t i = 0; i < iFrames; i++)
		{
			const int16_t iValue = DVI_AUDIO_TONE[st.iTonePhase >> 16];
			st.iTonePhase += st.iToneStep;
			if (st.iTonePhase >= (DVI_AUDIO_TONE_STEPS << 16))
				st.iTonePhase -= DVI_AUDIO_TONE_STEPS << 16;
			pStereo[2u * i] = (iTone & DVI_AUDIO_TONE_LEFT) ? iValue : (int16_t)0;
			pStereo[2u * i + 1u] = (iTone & DVI_AUDIO_TONE_RIGHT) ? iValue : (int16_t)0;
		}
		return iFrames;
	}
	const uint32_t iHad = audioPcmTake(st, pStereo, iFrames);
	for (uint32_t i = iHad; i < iFrames; i++)
	{
		pStereo[2u * i] = 0;
		pStereo[2u * i + 1u] = 0;
	}
	st.iUnderruns = st.iUnderruns + (iFrames - iHad);
	return iHad;
}

// The frames of a line that was missed, dropped so the rest keep their time.
// Returns how many were dropped: all of them for the tone.
DVI_AUDIO_INLINE uint32_t audioDrop(stDVIAudioCore & st, uint32_t iFrames)
{
	const uint32_t iDropped = audioPcmTake(st, nullptr, iFrames);
	if (st.iTone == 0)
		return iDropped;
	st.iTonePhase += st.iToneStep * iFrames;
	while (st.iTonePhase >= (DVI_AUDIO_TONE_STEPS << 16))
		st.iTonePhase -= DVI_AUDIO_TONE_STEPS << 16;
	return iFrames;
}

// ----------------------------------------------------------------------------
// Packet coding.

DVI_AUDIO_INLINE uint32_t audioParity(const stDVIAudioCore & st, const uint8_t * pBytes, uint32_t iCount)
{
	uint32_t iReg = 0;
	for (uint32_t i = 0; i < iCount; i++)
	{
		iReg ^= pBytes[i];
		iReg = (iReg >> 4) ^ st.iBCH[iReg & 0xfu];
		iReg = (iReg >> 4) ^ st.iBCH[iReg & 0xfu];
	}
	return iReg;
}

// Even parity of 16 bits.
DVI_AUDIO_INLINE uint32_t audioParity16(uint32_t iValue)
{
	iValue ^= iValue >> 8;
	iValue ^= iValue >> 4;
	iValue ^= iValue >> 2;
	iValue ^= iValue >> 1;
	return iValue & 1u;
}

// Bits 0, 8, 16 and 24 of iBits as bits 0 to 3.
DVI_AUDIO_INLINE uint32_t audioGather(uint32_t iBits)
{
	return ((iBits & 0x01010101u) * 0x01020408u) >> 24;
}

// The 32 raw words of an Audio Sample packet with iCount (1 to 4) stereo
// frames, the first being IEC 60958 frame iFrame. Returns the frame number
// after the last. Header: HB1 bits 0 to 3 sample present, layout 0; HB2 bits 4
// to 7 the block start flag of each subpacket. Subpacket: the 16-bit left sample
// in SB1 and SB2, right in SB4 and SB5, SB6 the V, U, C and P bits of left (bits
// 0 to 3) and right (bits 4 to 7), P making the sample bits with V, U and C even.
// Packet clock k: lane 0 bit 2 is header bit k, bit 3 is 0 on clock 0 only;
// lane 1 bit i is subpacket i bit 2k, lane 2 bit i is subpacket i bit 2k + 1.
DVI_AUDIO_INLINE uint32_t audioEncode(const stDVIAudioCore & st, const int16_t * pStereo, uint32_t iCount,
	uint32_t iFrame, uint32_t * pWords)
{
	uint8_t iSub[4][8];
	uint32_t iPresent = 0;
	uint32_t iBlockStart = 0;
	for (uint32_t i = 0; i < 4; i++)
	{
		uint8_t * pSub = iSub[i];
		if (i >= iCount)
		{
			for (uint32_t iByte = 0; iByte < 8; iByte++)
				pSub[iByte] = 0;
			continue;
		}
		const uint32_t iLeft = (uint16_t)pStereo[2u * i];
		const uint32_t iRight = (uint16_t)pStereo[2u * i + 1u];
		const uint32_t iC = (st.iStatus[iFrame >> 3] >> (iFrame & 7u)) & 1u;
		iPresent |= 1u << i;
		if (iFrame == 0)
			iBlockStart |= 1u << i;
		pSub[0] = 0;
		pSub[1] = (uint8_t)iLeft;
		pSub[2] = (uint8_t)(iLeft >> 8);
		pSub[3] = 0;
		pSub[4] = (uint8_t)iRight;
		pSub[5] = (uint8_t)(iRight >> 8);
		pSub[6] = (uint8_t)((iC << 2) | ((audioParity16(iLeft) ^ iC) << 3) | (iC << 6) | ((audioParity16(iRight) ^ iC) << 7));
		pSub[7] = (uint8_t)audioParity(st, pSub, 7);
		iFrame = iFrame + 1u == 192u ? 0u : iFrame + 1u;
	}

	const uint8_t iHeader[3] = { 0x02, (uint8_t)iPresent, (uint8_t)(iBlockStart << 4) };
	const uint32_t iHeaderBits = 0x02u | (iPresent << 8) | (iBlockStart << 20) | (audioParity(st, iHeader, 3) << 24);

	// Four clocks per byte of the subpackets; lane 0 with bit 3 set, then
	// cleared on clock 0.
	const uint16_t * pTERC4 = st.iTERC4;
	const uint32_t iLane0[2] = { pTERC4[st.iSync | 8u], pTERC4[st.iSync | 12u] };
	for (uint32_t iByte = 0; iByte < 8; iByte++)
	{
		const uint32_t iColumn = (uint32_t)iSub[0][iByte] | ((uint32_t)iSub[1][iByte] << 8) |
			((uint32_t)iSub[2][iByte] << 16) | ((uint32_t)iSub[3][iByte] << 24);
		const uint32_t iHeaderNibble = iHeaderBits >> (4u * iByte);
		uint32_t * pOut = pWords + 4u * iByte;
		pOut[0] = iLane0[iHeaderNibble & 1u] | ((uint32_t)pTERC4[audioGather(iColumn)] << 10) |
			((uint32_t)pTERC4[audioGather(iColumn >> 1)] << 20);
		pOut[1] = iLane0[(iHeaderNibble >> 1) & 1u] | ((uint32_t)pTERC4[audioGather(iColumn >> 2)] << 10) |
			((uint32_t)pTERC4[audioGather(iColumn >> 3)] << 20);
		pOut[2] = iLane0[(iHeaderNibble >> 2) & 1u] | ((uint32_t)pTERC4[audioGather(iColumn >> 4)] << 10) |
			((uint32_t)pTERC4[audioGather(iColumn >> 5)] << 20);
		pOut[3] = iLane0[(iHeaderNibble >> 3) & 1u] | ((uint32_t)pTERC4[audioGather(iColumn >> 6)] << 10) |
			((uint32_t)pTERC4[audioGather(iColumn >> 7)] << 20);
	}
	pWords[0] = (pWords[0] & ~0x3ffu) | pTERC4[st.iSync | ((iHeaderBits & 1u) << 2)];
	return iFrame;
}

DVI_AUDIO_INLINE void audioNullWords(const stDVIAudioCore & st, uint32_t * pWords)
{
	pWords[0] = st.iNull[0];
	for (uint32_t k = 1; k < 32; k++)
		pWords[k] = st.iNull[1];
}

// ----------------------------------------------------------------------------
// The tick.

// A line the TV reached before it was written. iTaken is the frames already
// taken for its packet, or -1 if none were.
DVI_AUDIO_INLINE void audioMissed(stDVIAudioCore & st, uint32_t iSlotBits, uint32_t iCount, int iTaken)
{
	if (iSlotBits == 0 || st.bMuting)
		return;
	st.iLate = st.iLate + 1u;
	st.iLost = st.iLost + (iTaken >= 0 ? (uint32_t)iTaken : audioDrop(st, iCount));
	if (st.iSlotAudio & (1u << (iSlotBits - 1u)))
		st.iRepeats = st.iRepeats + 1u;
}

// Accounts the lines up to the scan line that were not written, as many as the
// tick's budget allows. Entered at the start of a tick only, with the budget
// whole.
DVI_AUDIO_COLD void audioPassReached(stDVIAudioCore & st)
{
	st.iTickCost += DVI_AUDIO_COST_COLD;
	while ((int32_t)(st.iScan - st.iNext) >= 0)
	{
		if (st.iTickCost + DVI_AUDIO_COST_MISSED > DVI_AUDIO_TICK_BUDGET)
		{
			st.bMore = true;
			return;
		}
		st.iTickCost += DVI_AUDIO_COST_MISSED;
		const uint32_t iByte = st.pLine[st.iNextLine];
		audioMissed(st, iByte & DVI_AUDIO_LINE_SLOT_MASK, iByte >> DVI_AUDIO_LINE_COUNT_SHIFT, -1);
		audioStep(st);
	}
}

// Stopping: every slot that holds samples gets a null packet, except those of
// the scan line and the line before, which the next tick takes.
DVI_AUDIO_COLD void audioMuteStep(stDVIAudioCore & st)
{
	st.iNext = st.iScan + 1u;
	st.iNextLine = st.iScanLine + 1u == st.iLines ? 0u : st.iScanLine + 1u;
	const uint32_t iBefore = st.iScanLine == 0 ? st.iLines - 1u : st.iScanLine - 1u;
	uint32_t iBusy = 0;
	if (audioLineSlotBits(st, st.iScanLine) != 0)
		iBusy |= 1u << (audioLineSlotBits(st, st.iScanLine) - 1u);
	if (audioLineSlotBits(st, iBefore) != 0)
		iBusy |= 1u << (audioLineSlotBits(st, iBefore) - 1u);
	for (uint32_t iSlot = 0; iSlot < st.iSlots; iSlot++)
	{
		const uint32_t iBit = 1u << iSlot;
		if ((st.iSlotAudio & iBit) && !(iBusy & iBit))
		{
			audioNullWords(st, st.pSlot[iSlot]);
			st.iSlotAudio &= ~iBit;
		}
	}
	if (st.iSlotAudio == 0)
		st.bMuted = true;
}

// Starts a tick. False when it has no packets to write: the feeder is stopping.
// audioTickEnd() follows either way.
DVI_AUDIO_INLINE bool audioTickBegin(stDVIAudioCore & st, uint32_t iNextEntry)
{
	st.iTickCost = 0;
	st.bMore = false;
	const uint32_t iLine = audioScanLine(st, iNextEntry);
	if (!st.bStarted)
	{
		// The line after the scan line may start while its packet is made: the
		// first line written is the one after that.
		st.bStarted = true;
		st.iScan = iLine;
		st.iScanLine = iLine;
		st.iNext = iLine + 2u;
		st.iNextLine = iLine + 2u >= st.iLines ? iLine + 2u - st.iLines : iLine + 2u;
	}
	else
		audioScanTo(st, iLine);
	if (st.bMuting)
	{
		audioMuteStep(st);
		return false;
	}
	if ((int32_t)(st.iScan - st.iNext) >= 0)
		audioPassReached(st);
	return true;
}

// The packet for the next line to write, into pWords. Returns its slot, or -1
// when the tick is done. audioTickCommit() follows each packet.
DVI_AUDIO_INLINE int audioTickPrepare(stDVIAudioCore & st, uint32_t * pWords)
{
	for (;;)
	{
		if ((int32_t)(st.iScan - st.iNext) >= 0)
			return -1;                                      // missed lines left for the next tick
		if ((int32_t)(st.iNext - st.iScan) > (int32_t)st.iAhead)
			return -1;
		const uint32_t iByte = st.pLine[st.iNextLine];
		const uint32_t iSlotBits = iByte & DVI_AUDIO_LINE_SLOT_MASK;
		if (iSlotBits == 0)
		{
			audioStep(st);
			continue;
		}
		const uint32_t iCount = iByte >> DVI_AUDIO_LINE_COUNT_SHIFT;
		const uint32_t iCost = iCount != 0 ? DVI_AUDIO_COST_SAMPLES : DVI_AUDIO_COST_NULL;
		if (st.iTickCost + iCost > DVI_AUDIO_TICK_BUDGET)
		{
			st.bMore = true;
			return -1;
		}
		st.iTickCost += iCost;
		st.iPendSlot = iSlotBits - 1u;
		st.iPendCount = iCount;
		st.iPendTaken = 0;
		if (iCount == 0)
		{
			audioNullWords(st, pWords);
			st.iPendFrame = st.iFrame;
		}
		else
		{
			int16_t iStereo[2 * DVI_AUDIO_SAMPLES_PER_PACKET];
			st.iPendTaken = audioTake(st, iStereo, iCount);
			st.iPendFrame = audioEncode(st, iStereo, iCount, st.iFrame, pWords);
		}
		return (int)st.iPendSlot;
	}
}

// Writes the packet audioTickPrepare() made into its slot, unless the TV has
// reached its line meanwhile. iNextEntry is read after the packet was made.
// Lines reached after that one are accounted by the next tick, a short gap on.
DVI_AUDIO_INLINE void audioTickCommit(stDVIAudioCore & st, uint32_t iNextEntry, const uint32_t * pWords)
{
	audioScanTo(st, audioScanLine(st, iNextEntry));
	const uint32_t iBit = 1u << st.iPendSlot;
	if ((int32_t)(st.iNext - st.iScan) <= 0)
	{
		audioMissed(st, st.iPendSlot + 1u, st.iPendCount, (int)st.iPendTaken);
		audioStep(st);
		if ((int32_t)(st.iScan - st.iNext) >= 0)
			st.bMore = true;
		return;
	}
	uint32_t * pSlot = st.pSlot[st.iPendSlot];
	for (uint32_t k = 0; k < 32; k++)
		pSlot[k] = pWords[k];
	if (st.iPendCount != 0)
	{
		st.iSlotAudio |= iBit;
		st.iPackets = st.iPackets + 1u;
		st.iFrame = st.iPendFrame;
	}
	else
		st.iSlotAudio &= ~iBit;
	audioStep(st);
}

// Ends a tick. Returns the microseconds from now to the next, or 0 once
// stopping is done.
DVI_AUDIO_INLINE uint32_t audioTickEnd(stDVIAudioCore & st)
{
	if (st.bMuted)
		return 0;
	return st.bMore ? DVI_AUDIO_TICK_GAP_US : DVI_AUDIO_TICK_IDLE_US;
}
