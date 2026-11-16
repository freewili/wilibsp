#pragma once

#include <stdint.h>
#include "rpDVIModes.h"

// Receives one video frame as a sequence of pieces, in the order HSTX consumes them.
class rpDVIFrameSink
{
public:
	// iCount 32-bit HSTX command and data words.
	virtual bool addWords(const uint32_t * pWords, unsigned int iCount) = 0;
	// iCount 16-bit pixels of one picture row, in the LCD buffer's byte order
	// (RGB565, high byte first).
	virtual bool addPixels(const uint16_t * pPixels, unsigned int iCount) = 0;
	// iLines whole lines, one after another from pWords: the first iFirstWords
	// words long, each after it iLineWords. By default one piece per line.
	virtual bool addLines(const uint32_t * pWords, unsigned int iFirstWords, unsigned int iLineWords, unsigned int iLines)
	{
		for (unsigned int iLine = 0; iLine < iLines; iLine++)
			if (!addWords(iLine == 0 ? pWords : pWords + iFirstWords + (iLine - 1u) * iLineWords,
			              iLine == 0 ? iFirstWords : iLineWords))
				return false;
		return true;
	}
	// The piece that joins a picture row to the next. By default a piece of words.
	virtual bool addJoin(const uint32_t * pWords, unsigned int iCount)
	{
		return addWords(pWords, iCount);
	}
};

// Builds one video frame as a list of pieces: a few shared lines of HSTX command
// words, plus the pixel rows of a picture window. No hardware access. The shared
// lines live in this object, so it outlives every list built from it.
//
// With audio framing on, every active line starts with a video preamble and guard
// band, and the sync pulse of every line but the vertical sync lines holds a
// one-packet data island. Vertical back porch lines 0 and 1 carry the AVI and
// Audio InfoFrames. The front porch, and the back porch from line 2, are blocks
// of four lines: an Audio Clock Regeneration packet, then three audio slots. The
// islands of active lines are audio slots too. Slots hold null packets until
// written. Lines with an island live in a word pool the caller supplies. Piece
// counts are the same with audio framing on or off.
//
// With audio framing, lines that lie one after another in the pool are handed
// to the sink together (addLines), and the joins of a picture walk the row
// ring in order (addJoin): slot 0 follows the last slot. With DVI framing every
// join of a picture is the same words (addJoin), a ring of one slot.
class rpDVIFrame
{
public:
	// HSTX command expander opcodes; the low 12 bits carry the repeat count.
	static const uint32_t CMD_RAW         = 0x0u << 12;
	static const uint32_t CMD_RAW_REPEAT  = 0x1u << 12;
	static const uint32_t CMD_TMDS        = 0x2u << 12;
	static const uint32_t CMD_TMDS_REPEAT = 0x3u << 12;

	// Audio slots in each ring: picture rows, border lines, bar lines.
	static const unsigned int AUDIO_RING_SLOTS = 8;
	// Audio slots of the vertical blanking lines.
	static const unsigned int AUDIO_BLANK_SLOTS = 6;

	// Eight vertical colour bars across the whole active area. No pixel data.
	bool buildBars(const stDVIMode & stMode, rpDVIFrameSink & obSink);

	// A picture window of iWidth x iHeight pixels centred in the active area,
	// black outside it. Row n is the iWidth pixels at pPixels + n * iStridePixels.
	bool buildPicture(const stDVIMode & stMode, const uint16_t * pPixels,
		unsigned int iWidth, unsigned int iHeight, unsigned int iStridePixels,
		rpDVIFrameSink & obSink);

	// Number of pieces each build emits.
	static unsigned int barsPieceCount(const stDVIMode & stMode);
	static unsigned int picturePieceCount(const stDVIMode & stMode, unsigned int iWidth, unsigned int iHeight);
	// Sink calls a picture build with DVI framing makes, joins not counted; 0 if
	// it cannot be built.
	static unsigned int pictureCalls(const stDVIMode & stMode, unsigned int iWidth, unsigned int iHeight);

	// One RGB565 colour as the data word of a repeat command: the pixel in the
	// LCD buffer's byte order, in both halves of the word.
	static uint32_t colourWord(uint16_t iRGB565);

	// Audio framing for the builds that follow. pWords, iWords long, holds the
	// lines that carry an island and outlives every list built from it; nullptr
	// is DVI framing, the default. A build with audio framing fails when the pool
	// is short, the mode has no VIC, the timing has no room for the islands, or
	// iAudioRateHz has no exact clock regeneration values at the pixel clock.
	void setAudioFraming(uint32_t * pWords, unsigned int iWords, unsigned int iAudioRateHz = 48000);

	// Pool words each build needs with audio framing; 0 if it cannot be built.
	static unsigned int audioFramingBarsWords(const stDVIMode & stMode);
	static unsigned int audioFramingPictureWords(const stDVIMode & stMode, unsigned int iWidth, unsigned int iHeight);
	// Sink calls a picture build with audio framing makes, joins not counted, and
	// the slots of its row ring; 0 if it cannot be built.
	static unsigned int audioFramingPictureCalls(const stDVIMode & stMode, unsigned int iWidth, unsigned int iHeight);
	static unsigned int audioFramingRowSlots(unsigned int iHeight);

	// Audio slots of the last build; none after a DVI build. A slot is the 32
	// packet words of one island, rewritten in place while video runs.
	unsigned int audioSlotCount() const;
	uint32_t * audioSlotWords(unsigned int iSlot) const;
	// The slot sent on line iLine (0 = first line of the vertical front porch),
	// or -1 if that line has no audio slot.
	int audioLineSlot(unsigned int iLine) const;
	// Sync wire levels every audio slot's packet is encoded with.
	bool audioVLevel() const;
	bool audioHLevel() const;

private:
	uint32_t syncWord(bool bVSyncActive, bool bHSyncActive) const;
	unsigned int putHSync(uint32_t * pWords, bool bVSyncActive) const;
	void buildBlankLines();
	bool addBlanking(rpDVIFrameSink & obSink) const;

	bool vLevel(bool bVSyncActive) const;
	bool hLevel(bool bHSyncActive) const;
	unsigned int putIsland(uint32_t * pWords, bool bVSyncActive, const uint32_t * pPacket) const;
	unsigned int putHSyncIsland(uint32_t * pWords, bool bVSyncActive, bool bVideo, const uint32_t * pPacket) const;
	static bool audioFramingTimingFits(const stDVIMode & stMode);
	bool startAudioFraming(const stDVIMode & stMode, unsigned int iPoolWords);
	bool addBlankingAudioFraming(rpDVIFrameSink & obSink) const;
	bool addBorderAudioFraming(rpDVIFrameSink & obSink, const uint32_t * pBorders, unsigned int iSlots,
		unsigned int iFirst, unsigned int iLines) const;
	static unsigned int blankingAudioFramingCalls(const stDVIMode & stMode);
	static unsigned int borderAudioFramingCalls(unsigned int iSlots, unsigned int iFirst, unsigned int iLines);
	bool buildBarsAudioFraming(const stDVIMode & stMode, rpDVIFrameSink & obSink);
	bool buildPictureAudioFraming(const stDVIMode & stMode, const uint16_t * pPixels,
		unsigned int iWidth, unsigned int iHeight, unsigned int iStridePixels,
		rpDVIFrameSink & obSink);

	stDVIMode m_stMode = {};
	uint32_t m_iLineVBlank[6] = {};       // blank line, vertical sync inactive
	uint32_t m_iLineVSync[6] = {};        // blank line, vertical sync active
	uint32_t m_iLineBorder[8] = {};       // active line, all border
	uint32_t m_iLineBars[22] = {};        // active line of eight bars
	uint32_t m_iLineHead[9] = {};         // sync, left border, "pixels follow"
	uint32_t m_iLineTailHead[11] = {};    // right border, then the next line's head
	uint32_t m_iLineTail[2] = {};         // right border
	unsigned int m_iHeadWords = 0;
	unsigned int m_iTailWords = 0;

	uint32_t * m_pAudioFramingWords = nullptr;    // the caller's pool; nullptr = DVI framing
	unsigned int m_iAudioFramingWords = 0;
	unsigned int m_iAudioRateHz = 48000;
	// Audio slots of the last build, in four runs: bar ring, or a picture's
	// first row's own slot, row ring and border ring; then the vertical blanking
	// slots. Each run: slot count, pool offset of its first packet word, words
	// from one line to the next (the blanking run skips each block's first line).
	static const unsigned int SLOT_RUNS = 4;
	static const unsigned int SLOT_RUN_BLANK = 3;
	unsigned int m_iSlotCount[SLOT_RUNS] = {};
	unsigned int m_iSlotFirst[SLOT_RUNS] = {};
	unsigned int m_iSlotStride[SLOT_RUNS] = {};
	bool m_bSlotBars = false;
	unsigned int m_iSlotTop = 0;          // picture: border lines above the first row
	unsigned int m_iSlotRows = 0;         // picture: rows
	unsigned int m_iSlotHead = 0;         // picture: the first row's slot
};
