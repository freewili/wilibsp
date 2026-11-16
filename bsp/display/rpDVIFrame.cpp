#include "rpDVIFrame.h"
#include "rpDVIPacket.h"

namespace {

// TMDS control symbols for the (vertical sync, horizontal sync) levels 00, 01, 10, 11.
const uint32_t TMDS_CTRL[4] = { 0x354u, 0x0abu, 0x154u, 0x2abu };

// White, yellow, cyan, green, magenta, red, blue, black.
const uint16_t BAR_COLOURS[8] = { 0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000 };

// Audio framing, in list words and pixel clocks.
const unsigned int ISLAND_WORDS = 39;           // preamble 2, guard 2, raw command 1, packet 32, guard 2
const unsigned int ISLAND_CLOCKS = 44;          // preamble 8, guard 2, packet 32, guard 2
const unsigned int ISLAND_PACKET = 5;           // packet words from the start of an island
const unsigned int LINE_PACKET = 2 + ISLAND_PACKET;     // from the start of a line: front porch, island
const unsigned int VIDEO_LEAD_CLOCKS = 10;      // video preamble 8, guard 2
const unsigned int CONTROL_MIN_CLOCKS = 4;      // plain control clocks before each preamble
const unsigned int HSYNC_ISLAND_WORDS = 49;     // front porch 2, island 39, sync 2, back porch 2, video lead 4
const unsigned int BLANK_ISLAND_WORDS = 47;     // front porch 2, island 39, sync 2, back porch and active 2 x 2
const unsigned int BORDER_ISLAND_WORDS = HSYNC_ISLAND_WORDS + 2;
const unsigned int BARS_ISLAND_WORDS = HSYNC_ISLAND_WORDS + 16;

// Vertical back porch lines 0 and 1: AVI and Audio InfoFrame.
const unsigned int VBP_FIXED_LINES = 2;

// The vertical front porch, and the back porch after its fixed lines, are
// blocks of four lines: a clock regeneration line, then three lines with an
// audio slot. The pool holds two blocks, sent in turn, so a slot comes round
// again after eight block lines.
const unsigned int BLOCK_LINES = 4;
const unsigned int BLOCK_COUNT = 2;
const unsigned int BLOCK_WORDS = BLOCK_LINES * BLANK_ISLAND_WORDS;
static_assert(BLOCK_COUNT * (BLOCK_LINES - 1u) == rpDVIFrame::AUDIO_BLANK_SLOTS, "a slot per block line after the first");
static_assert(BLOCK_COUNT * BLOCK_LINES >= rpDVIFrame::AUDIO_RING_SLOTS,
              "a blanking slot is not reused within a ring's length of lines");

// Pool layout: the two fixed vertical blanking lines, the blocks, then the
// audio slots of the active lines.
const unsigned int POOL_AVI = 0;
const unsigned int POOL_AUDIO_INFO = BLANK_ISLAND_WORDS;
const unsigned int POOL_BLOCKS = 2u * BLANK_ISLAND_WORDS;
const unsigned int POOL_SLOTS = POOL_BLOCKS + BLOCK_COUNT * BLOCK_WORDS;

inline unsigned int minimum(unsigned int iA, unsigned int iB)
{
	return iA < iB ? iA : iB;
}

// Block line of the first back porch line after the fixed lines: the front
// porch's count rounded up to a whole block.
inline unsigned int vbpBlockStart(const stDVIMode & stMode)
{
	return (stMode.iVFrontPorch + BLOCK_LINES - 1u) / BLOCK_LINES * BLOCK_LINES;
}

// Block line of a vertical blanking line, or -1 for the sync and fixed lines.
inline int blockLine(const stDVIMode & stMode, unsigned int iLine)
{
	if (iLine < stMode.iVFrontPorch)
		return (int)iLine;
	const unsigned int iFirst = (unsigned int)stMode.iVFrontPorch + stMode.iVSync + VBP_FIXED_LINES;
	const unsigned int iEnd = (unsigned int)stMode.iVFrontPorch + stMode.iVSync + stMode.iVBackPorch;
	if (iLine < iFirst || iLine >= iEnd)
		return -1;
	return (int)(vbpBlockStart(stMode) + iLine - iFirst);
}

// The block lines iFirst to iFirst + iLines - 1 in calls, one per block: fnCall
// (pool offset of the first line, lines).
template <typename F>
bool forBlockRuns(unsigned int iFirst, unsigned int iLines, F fnCall)
{
	while (iLines != 0)
	{
		const unsigned int iRun = minimum(BLOCK_LINES - iFirst % BLOCK_LINES, iLines);
		const unsigned int iAt = POOL_BLOCKS + (iFirst / BLOCK_LINES % BLOCK_COUNT) * BLOCK_WORDS +
			(iFirst % BLOCK_LINES) * BLANK_ISLAND_WORDS;
		if (!fnCall(iAt, iRun))
			return false;
		iFirst += iRun;
		iLines -= iRun;
	}
	return true;
}

}

uint32_t rpDVIFrame::colourWord(uint16_t iRGB565)
{
	uint32_t iSwapped = (uint16_t)((iRGB565 >> 8) | (iRGB565 << 8));
	return (iSwapped << 16) | iSwapped;
}

// Lane 0 carries the two sync levels; lanes 1 and 2 carry the 00 symbol.
uint32_t rpDVIFrame::syncWord(bool bVSyncActive, bool bHSyncActive) const
{
	unsigned int iVLevel = (bVSyncActive != m_stMode.bVSyncActiveLow) ? 1u : 0u;
	unsigned int iHLevel = (bHSyncActive != m_stMode.bHSyncActiveLow) ? 1u : 0u;
	return TMDS_CTRL[(iVLevel << 1) | iHLevel] | (TMDS_CTRL[0] << 10) | (TMDS_CTRL[0] << 20);
}

// Front porch, sync pulse and back porch of one line: six words.
unsigned int rpDVIFrame::putHSync(uint32_t * pWords, bool bVSyncActive) const
{
	pWords[0] = CMD_RAW_REPEAT | m_stMode.iHFrontPorch;
	pWords[1] = syncWord(bVSyncActive, false);
	pWords[2] = CMD_RAW_REPEAT | m_stMode.iHSync;
	pWords[3] = syncWord(bVSyncActive, true);
	pWords[4] = CMD_RAW_REPEAT | m_stMode.iHBackPorch;
	pWords[5] = syncWord(bVSyncActive, false);
	return 6;
}

// In a blank line the back porch and the active width are one repeat.
void rpDVIFrame::buildBlankLines()
{
	putHSync(m_iLineVBlank, false);
	m_iLineVBlank[4] = CMD_RAW_REPEAT | (unsigned int)(m_stMode.iHBackPorch + m_stMode.iHActive);
	putHSync(m_iLineVSync, true);
	m_iLineVSync[4] = CMD_RAW_REPEAT | (unsigned int)(m_stMode.iHBackPorch + m_stMode.iHActive);
}

// Vertical front porch, sync and back porch.
bool rpDVIFrame::addBlanking(rpDVIFrameSink & obSink) const
{
	for (unsigned int iLine = 0; iLine < m_stMode.iVFrontPorch; iLine++)
		if (!obSink.addWords(m_iLineVBlank, 6))
			return false;
	for (unsigned int iLine = 0; iLine < m_stMode.iVSync; iLine++)
		if (!obSink.addWords(m_iLineVSync, 6))
			return false;
	for (unsigned int iLine = 0; iLine < m_stMode.iVBackPorch; iLine++)
		if (!obSink.addWords(m_iLineVBlank, 6))
			return false;
	return true;
}

unsigned int rpDVIFrame::barsPieceCount(const stDVIMode & stMode)
{
	return (unsigned int)stMode.iVFrontPorch + stMode.iVSync + stMode.iVBackPorch + stMode.iVActive;
}

bool rpDVIFrame::buildBars(const stDVIMode & stMode, rpDVIFrameSink & obSink)
{
	for (unsigned int iRun = 0; iRun < SLOT_RUNS; iRun++)
		m_iSlotCount[iRun] = 0;
	if (stMode.iHActive % 8u != 0)
		return false;
	if (m_pAudioFramingWords != nullptr)
		return buildBarsAudioFraming(stMode, obSink);
	m_stMode = stMode;
	buildBlankLines();

	unsigned int iWords = putHSync(m_iLineBars, false);
	for (unsigned int iBar = 0; iBar < 8; iBar++)
	{
		m_iLineBars[iWords++] = CMD_TMDS_REPEAT | (unsigned int)(stMode.iHActive / 8u);
		m_iLineBars[iWords++] = colourWord(BAR_COLOURS[iBar]);
	}

	if (!addBlanking(obSink))
		return false;
	for (unsigned int iLine = 0; iLine < stMode.iVActive; iLine++)
		if (!obSink.addWords(m_iLineBars, iWords))
			return false;
	return true;
}

unsigned int rpDVIFrame::picturePieceCount(const stDVIMode & stMode, unsigned int iWidth, unsigned int iHeight)
{
	if (iWidth == 0 || iHeight == 0 || iWidth > stMode.iHActive || iHeight > stMode.iVActive)
		return 0;
	unsigned int iBorderLines = stMode.iVActive - iHeight;
	unsigned int iTail = (iWidth < stMode.iHActive) ? 1u : 0u;
	// Blanking, border lines, the first head, then per row its pixels and the
	// piece that joins it to the next row, and the last tail.
	return (unsigned int)stMode.iVFrontPorch + stMode.iVSync + stMode.iVBackPorch
		+ iBorderLines + 1u + iHeight + (iHeight - 1u) + iTail;
}

unsigned int rpDVIFrame::pictureCalls(const stDVIMode & stMode, unsigned int iWidth, unsigned int iHeight)
{
	const unsigned int iPieces = picturePieceCount(stMode, iWidth, iHeight);
	return iPieces != 0 ? iPieces - (iHeight - 1u) : 0u;
}

bool rpDVIFrame::buildPicture(const stDVIMode & stMode, const uint16_t * pPixels,
	unsigned int iWidth, unsigned int iHeight, unsigned int iStridePixels,
	rpDVIFrameSink & obSink)
{
	for (unsigned int iRun = 0; iRun < SLOT_RUNS; iRun++)
		m_iSlotCount[iRun] = 0;
	if (pPixels == nullptr || picturePieceCount(stMode, iWidth, iHeight) == 0)
		return false;
	// The border is split evenly, so it has to divide by two.
	if ((stMode.iHActive - iWidth) % 2u != 0)
		return false;
	if (m_pAudioFramingWords != nullptr)
		return buildPictureAudioFraming(stMode, pPixels, iWidth, iHeight, iStridePixels, obSink);
	m_stMode = stMode;
	buildBlankLines();

	const unsigned int iBorderX = (stMode.iHActive - iWidth) / 2u;
	const unsigned int iBorderTop = (stMode.iVActive - iHeight) / 2u;
	const unsigned int iBorderBottom = stMode.iVActive - iHeight - iBorderTop;

	unsigned int iWords = putHSync(m_iLineBorder, false);
	m_iLineBorder[iWords++] = CMD_TMDS_REPEAT | stMode.iHActive;
	m_iLineBorder[iWords++] = colourWord(0x0000);

	// A repeat of zero pixels is not a valid command, so a full-width picture
	// has no border words at all.
	m_iTailWords = 0;
	if (iBorderX != 0)
	{
		m_iLineTail[m_iTailWords++] = CMD_TMDS_REPEAT | iBorderX;
		m_iLineTail[m_iTailWords++] = colourWord(0x0000);
	}

	m_iHeadWords = putHSync(m_iLineHead, false);
	for (unsigned int iWord = 0; iWord < m_iTailWords; iWord++)
		m_iLineHead[m_iHeadWords++] = m_iLineTail[iWord];
	m_iLineHead[m_iHeadWords++] = CMD_TMDS | iWidth;

	unsigned int iJoinWords = 0;
	for (unsigned int iWord = 0; iWord < m_iTailWords; iWord++)
		m_iLineTailHead[iJoinWords++] = m_iLineTail[iWord];
	for (unsigned int iWord = 0; iWord < m_iHeadWords; iWord++)
		m_iLineTailHead[iJoinWords++] = m_iLineHead[iWord];

	if (!addBlanking(obSink))
		return false;
	for (unsigned int iLine = 0; iLine < iBorderTop; iLine++)
		if (!obSink.addWords(m_iLineBorder, 8))
			return false;

	if (!obSink.addWords(m_iLineHead, m_iHeadWords))
		return false;
	for (unsigned int iRow = 0; iRow < iHeight; iRow++)
	{
		if (!obSink.addPixels(pPixels + iRow * iStridePixels, iWidth))
			return false;
		if (iRow + 1u < iHeight && !obSink.addJoin(m_iLineTailHead, iJoinWords))
			return false;
	}
	if (m_iTailWords != 0 && !obSink.addWords(m_iLineTail, m_iTailWords))
		return false;

	for (unsigned int iLine = 0; iLine < iBorderBottom; iLine++)
		if (!obSink.addWords(m_iLineBorder, 8))
			return false;
	return true;
}

// ----------------------------------------------------------------------------
// Audio framing

void rpDVIFrame::setAudioFraming(uint32_t * pWords, unsigned int iWords, unsigned int iAudioRateHz)
{
	m_pAudioFramingWords = pWords;
	m_iAudioFramingWords = pWords != nullptr ? iWords : 0u;
	m_iAudioRateHz = iAudioRateHz;
}

bool rpDVIFrame::vLevel(bool bVSyncActive) const
{
	return bVSyncActive != m_stMode.bVSyncActiveLow;
}

bool rpDVIFrame::hLevel(bool bHSyncActive) const
{
	return bHSyncActive != m_stMode.bHSyncActiveLow;
}

// Preamble 8, leading guard 2, packet 32, trailing guard 2: 44 clocks with the
// horizontal sync pulse active.
unsigned int rpDVIFrame::putIsland(uint32_t * pWords, bool bVSyncActive, const uint32_t * pPacket) const
{
	const bool bV = vLevel(bVSyncActive);
	const bool bH = hLevel(true);
	pWords[0] = CMD_RAW_REPEAT | DVI_PREAMBLE_CLOCKS;
	pWords[1] = dviIslandPreambleWord(bV, bH);
	pWords[2] = CMD_RAW_REPEAT | DVI_GUARD_CLOCKS;
	pWords[3] = dviIslandGuardWord(bV, bH);
	pWords[4] = CMD_RAW | DVI_PACKET_CLOCKS;
	for (unsigned int iWord = 0; iWord < DVI_PACKET_CLOCKS; iWord++)
		pWords[ISLAND_PACKET + iWord] = pPacket[iWord];
	pWords[37] = CMD_RAW_REPEAT | DVI_GUARD_CLOCKS;
	pWords[38] = dviIslandGuardWord(bV, bH);
	return ISLAND_WORDS;
}

// Front porch, island at the start of the sync pulse, rest of the pulse and back
// porch: 45 words. With bVideo the back porch ends in the video preamble and
// guard band: 49 words.
unsigned int rpDVIFrame::putHSyncIsland(uint32_t * pWords, bool bVSyncActive, bool bVideo, const uint32_t * pPacket) const
{
	unsigned int iWords = 0;
	pWords[iWords++] = CMD_RAW_REPEAT | m_stMode.iHFrontPorch;
	pWords[iWords++] = syncWord(bVSyncActive, false);
	iWords += putIsland(pWords + iWords, bVSyncActive, pPacket);
	pWords[iWords++] = CMD_RAW_REPEAT | (unsigned int)(m_stMode.iHSync - ISLAND_CLOCKS);
	pWords[iWords++] = syncWord(bVSyncActive, true);
	if (!bVideo)
	{
		pWords[iWords++] = CMD_RAW_REPEAT | m_stMode.iHBackPorch;
		pWords[iWords++] = syncWord(bVSyncActive, false);
		return iWords;
	}
	pWords[iWords++] = CMD_RAW_REPEAT | (unsigned int)(m_stMode.iHBackPorch - VIDEO_LEAD_CLOCKS);
	pWords[iWords++] = syncWord(bVSyncActive, false);
	pWords[iWords++] = CMD_RAW_REPEAT | DVI_PREAMBLE_CLOCKS;
	pWords[iWords++] = dviVideoPreambleWord(vLevel(bVSyncActive), hLevel(false));
	pWords[iWords++] = CMD_RAW_REPEAT | DVI_GUARD_CLOCKS;
	pWords[iWords++] = dviVideoGuardWord();
	return iWords;
}

// The island fits in the sync pulse with control clocks after it, at least four
// control clocks come before each preamble, and the vertical back porch holds
// the fixed islands and a clock regeneration line.
bool rpDVIFrame::audioFramingTimingFits(const stDVIMode & stMode)
{
	return stMode.iVIC != 0 &&
		stMode.iHFrontPorch >= CONTROL_MIN_CLOCKS &&
		stMode.iHSync > ISLAND_CLOCKS &&
		stMode.iHBackPorch >= VIDEO_LEAD_CLOCKS + CONTROL_MIN_CLOCKS &&
		stMode.iVBackPorch > VBP_FIXED_LINES;
}

unsigned int rpDVIFrame::audioFramingBarsWords(const stDVIMode & stMode)
{
	if (stMode.iHActive % 8u != 0 || stMode.iVActive == 0 || !audioFramingTimingFits(stMode))
		return 0;
	return POOL_SLOTS + minimum(AUDIO_RING_SLOTS, stMode.iVActive) * BARS_ISLAND_WORDS;
}

unsigned int rpDVIFrame::audioFramingRowSlots(unsigned int iHeight)
{
	return iHeight != 0 ? minimum(AUDIO_RING_SLOTS, iHeight - 1u) : 0u;
}

// Fixed and blank lines, the first row's head unless a full row ring holds it
// (its last slot without the right border), the row ring (right border and the
// next row's head) and the border ring.
unsigned int rpDVIFrame::audioFramingPictureWords(const stDVIMode & stMode, unsigned int iWidth, unsigned int iHeight)
{
	if (picturePieceCount(stMode, iWidth, iHeight) == 0 || (stMode.iHActive - iWidth) % 2u != 0 ||
		!audioFramingTimingFits(stMode))
		return 0;
	const unsigned int iTailWords = (iWidth < stMode.iHActive) ? 2u : 0u;
	const unsigned int iHeadWords = HSYNC_ISLAND_WORDS + iTailWords + 1u;
	const unsigned int iRowSlots = audioFramingRowSlots(iHeight);
	return POOL_SLOTS + (iRowSlots == AUDIO_RING_SLOTS ? 0u : iHeadWords) + iRowSlots * (iTailWords + iHeadWords) +
		minimum(AUDIO_RING_SLOTS, stMode.iVActive - iHeight) * BORDER_ISLAND_WORDS;
}

// Front porch blocks, sync lines, the fixed lines, back porch blocks.
unsigned int rpDVIFrame::blankingAudioFramingCalls(const stDVIMode & stMode)
{
	unsigned int iCalls = stMode.iVSync + 1u;
	auto fnCount = [&](unsigned int, unsigned int) { iCalls++; return true; };
	forBlockRuns(0, stMode.iVFrontPorch, fnCount);
	forBlockRuns(vbpBlockStart(stMode), stMode.iVBackPorch - VBP_FIXED_LINES, fnCount);
	return iCalls;
}

unsigned int rpDVIFrame::borderAudioFramingCalls(unsigned int iSlots, unsigned int iFirst, unsigned int iLines)
{
	unsigned int iCalls = 0;
	for (unsigned int iLine = 0; iLine < iLines; iCalls++)
		iLine += minimum(iSlots - (iFirst + iLine) % iSlots, iLines - iLine);
	return iCalls;
}

// Blanking, the border above in runs of the ring, the first row's head, the
// rows, the last tail and the border below.
unsigned int rpDVIFrame::audioFramingPictureCalls(const stDVIMode & stMode, unsigned int iWidth, unsigned int iHeight)
{
	if (audioFramingPictureWords(stMode, iWidth, iHeight) == 0)
		return 0;
	const unsigned int iBorderLines = stMode.iVActive - iHeight;
	const unsigned int iBorderTop = iBorderLines / 2u;
	const unsigned int iBorderSlots = minimum(AUDIO_RING_SLOTS, iBorderLines);
	return blankingAudioFramingCalls(stMode) +
		borderAudioFramingCalls(iBorderSlots, 0, iBorderTop) + 1u + iHeight + (iWidth < stMode.iHActive ? 1u : 0u) +
		borderAudioFramingCalls(iBorderSlots, iBorderTop, iBorderLines - iBorderTop);
}

// Checks the framing can be built and writes the two fixed blanking lines and
// the blocks: a clock regeneration line, then three lines whose islands are
// audio slots, null packets until written.
bool rpDVIFrame::startAudioFraming(const stDVIMode & stMode, unsigned int iPoolWords)
{
	unsigned int iN = 0;
	unsigned int iCTS = 0;
	if (iPoolWords == 0 || iPoolWords > m_iAudioFramingWords ||
		!dviClockValues(m_iAudioRateHz, stMode.iPixelClockKHz, iN, iCTS))
		return false;

	stDVIPacket stPacket;
	uint32_t iPacket[DVI_PACKET_CLOCKS];
	const bool bV = vLevel(false);
	const bool bH = hLevel(true);

	// A blank line with an island: the back porch and the active width in two
	// repeats, so the words of the next line's island are read from the pool
	// no sooner than half that time after the line's last word.
	const unsigned int iBlankRest = 2u + ISLAND_WORDS + 2u;
	const unsigned int iBlankClocks = (unsigned int)stMode.iHBackPorch + stMode.iHActive;
	auto fnBlankLine = [&](uint32_t * pLine, const uint32_t * pIsland) {
		putHSyncIsland(pLine, false, false, pIsland);
		pLine[iBlankRest] = CMD_RAW_REPEAT | (iBlankClocks - iBlankClocks / 2u);
		pLine[iBlankRest + 2u] = CMD_RAW_REPEAT | (iBlankClocks / 2u);
		pLine[iBlankRest + 3u] = pLine[iBlankRest + 1u];
	};

	dviPacketAVI(stPacket, stMode.iVIC);
	dviPacketWords(stPacket, bV, bH, iPacket);
	fnBlankLine(m_pAudioFramingWords + POOL_AVI, iPacket);

	dviPacketAudioInfo(stPacket);
	dviPacketWords(stPacket, bV, bH, iPacket);
	fnBlankLine(m_pAudioFramingWords + POOL_AUDIO_INFO, iPacket);

	uint32_t iClock[DVI_PACKET_CLOCKS];
	dviPacketClock(stPacket, iN, iCTS);
	dviPacketWords(stPacket, bV, bH, iClock);
	dviPacketNull(stPacket);
	dviPacketWords(stPacket, bV, bH, iPacket);
	for (unsigned int iLine = 0; iLine < BLOCK_COUNT * BLOCK_LINES; iLine++)
	{
		fnBlankLine(m_pAudioFramingWords + POOL_BLOCKS + iLine * BLANK_ISLAND_WORDS, iLine % BLOCK_LINES == 0 ? iClock : iPacket);
	}
	m_iSlotCount[SLOT_RUN_BLANK] = AUDIO_BLANK_SLOTS;
	m_iSlotFirst[SLOT_RUN_BLANK] = POOL_BLOCKS + LINE_PACKET;
	m_iSlotStride[SLOT_RUN_BLANK] = BLANK_ISLAND_WORDS;
	return true;
}

// Vertical front porch in blocks, sync as in DVI framing, the fixed lines, then
// the back porch in blocks.
bool rpDVIFrame::addBlankingAudioFraming(rpDVIFrameSink & obSink) const
{
	auto fnBlock = [&](unsigned int iAt, unsigned int iLines) {
		return obSink.addLines(m_pAudioFramingWords + iAt, BLANK_ISLAND_WORDS, BLANK_ISLAND_WORDS, iLines);
	};
	if (!forBlockRuns(0, m_stMode.iVFrontPorch, fnBlock))
		return false;
	for (unsigned int iLine = 0; iLine < m_stMode.iVSync; iLine++)
		if (!obSink.addWords(m_iLineVSync, 6))
			return false;

	static_assert(POOL_AUDIO_INFO == POOL_AVI + BLANK_ISLAND_WORDS && VBP_FIXED_LINES == 2,
	              "the fixed lines lie in the pool in the order they are sent");
	if (!obSink.addLines(m_pAudioFramingWords + POOL_AVI, BLANK_ISLAND_WORDS, BLANK_ISLAND_WORDS, VBP_FIXED_LINES))
		return false;
	return forBlockRuns(vbpBlockStart(m_stMode), m_stMode.iVBackPorch - VBP_FIXED_LINES, fnBlock);
}

// Border lines iFirst on, counted over the border above and below, line b in
// ring slot b: one call per run of slots that lie one after another.
bool rpDVIFrame::addBorderAudioFraming(rpDVIFrameSink & obSink, const uint32_t * pBorders, unsigned int iSlots,
	unsigned int iFirst, unsigned int iLines) const
{
	unsigned int iLine = 0;
	while (iLine < iLines)
	{
		const unsigned int iSlot = (iFirst + iLine) % iSlots;
		const unsigned int iRun = minimum(iSlots - iSlot, iLines - iLine);
		if (!obSink.addLines(pBorders + iSlot * BORDER_ISLAND_WORDS, BORDER_ISLAND_WORDS, BORDER_ISLAND_WORDS, iRun))
			return false;
		iLine += iRun;
	}
	return true;
}

bool rpDVIFrame::buildBarsAudioFraming(const stDVIMode & stMode, rpDVIFrameSink & obSink)
{
	m_stMode = stMode;
	buildBlankLines();
	if (!startAudioFraming(stMode, audioFramingBarsWords(stMode)))
		return false;

	stDVIPacket stNull;
	uint32_t iNull[DVI_PACKET_CLOCKS];
	dviPacketNull(stNull);
	dviPacketWords(stNull, audioVLevel(), audioHLevel(), iNull);

	const unsigned int iSlots = minimum(AUDIO_RING_SLOTS, stMode.iVActive);
	for (unsigned int iSlot = 0; iSlot < iSlots; iSlot++)
	{
		uint32_t * pLine = m_pAudioFramingWords + POOL_SLOTS + iSlot * BARS_ISLAND_WORDS;
		unsigned int iWords = putHSyncIsland(pLine, false, true, iNull);
		for (unsigned int iBar = 0; iBar < 8; iBar++)
		{
			pLine[iWords++] = CMD_TMDS_REPEAT | (unsigned int)(stMode.iHActive / 8u);
			pLine[iWords++] = colourWord(BAR_COLOURS[iBar]);
		}
	}
	m_bSlotBars = true;
	m_iSlotCount[0] = iSlots;
	m_iSlotFirst[0] = POOL_SLOTS + LINE_PACKET;
	m_iSlotStride[0] = BARS_ISLAND_WORDS;

	if (!addBlankingAudioFraming(obSink))
		return false;
	for (unsigned int iLine = 0; iLine < stMode.iVActive; iLine++)
		if (!obSink.addWords(m_pAudioFramingWords + POOL_SLOTS + (iLine % iSlots) * BARS_ISLAND_WORDS, BARS_ISLAND_WORDS))
			return false;
	return true;
}

// The island of a row's line is in the piece before its pixels: the first row's
// head, or the joining piece after the row above. With a full row ring the head
// is its last slot without the right border, so the first row shares that slot
// with the rows the ring's length below it.
bool rpDVIFrame::buildPictureAudioFraming(const stDVIMode & stMode, const uint16_t * pPixels,
	unsigned int iWidth, unsigned int iHeight, unsigned int iStridePixels,
	rpDVIFrameSink & obSink)
{
	m_stMode = stMode;
	buildBlankLines();
	if (!startAudioFraming(stMode, audioFramingPictureWords(stMode, iWidth, iHeight)))
		return false;

	const unsigned int iBorderX = (stMode.iHActive - iWidth) / 2u;
	const unsigned int iBorderTop = (stMode.iVActive - iHeight) / 2u;
	const unsigned int iBorderBottom = stMode.iVActive - iHeight - iBorderTop;

	stDVIPacket stNull;
	uint32_t iNull[DVI_PACKET_CLOCKS];
	dviPacketNull(stNull);
	dviPacketWords(stNull, audioVLevel(), audioHLevel(), iNull);

	m_iTailWords = 0;
	if (iBorderX != 0)
	{
		m_iLineTail[m_iTailWords++] = CMD_TMDS_REPEAT | iBorderX;
		m_iLineTail[m_iTailWords++] = colourWord(0x0000);
	}

	// The head in its own words, then the row ring; with a full ring the head
	// is the last slot's.
	const unsigned int iRowSlots = audioFramingRowSlots(iHeight);
	const bool bHeadInRing = iRowSlots == AUDIO_RING_SLOTS;
	uint32_t * pFirst = m_pAudioFramingWords + POOL_SLOTS;
	const unsigned int iHeadAt = bHeadInRing ? m_iTailWords : 0u;
	for (unsigned int iWord = 0; iWord < iHeadAt; iWord++)
		pFirst[iWord] = m_iLineTail[iWord];
	unsigned int iHeadWords = putHSyncIsland(pFirst + iHeadAt, false, true, iNull);
	for (unsigned int iWord = 0; iWord < m_iTailWords; iWord++)
		pFirst[iHeadAt + iHeadWords++] = m_iLineTail[iWord];
	pFirst[iHeadAt + iHeadWords++] = CMD_TMDS | iWidth;

	const unsigned int iJoinWords = m_iTailWords + iHeadWords;
	uint32_t * pRows = bHeadInRing ? pFirst : pFirst + iHeadWords;
	for (unsigned int iSlot = bHeadInRing ? 1u : 0u; iSlot < iRowSlots; iSlot++)
	{
		uint32_t * pJoin = pRows + iSlot * iJoinWords;
		for (unsigned int iWord = 0; iWord < m_iTailWords; iWord++)
			pJoin[iWord] = m_iLineTail[iWord];
		for (unsigned int iWord = 0; iWord < iHeadWords; iWord++)
			pJoin[m_iTailWords + iWord] = pFirst[iHeadAt + iWord];
	}
	const uint32_t * pHead = bHeadInRing ? pRows + (iRowSlots - 1u) * iJoinWords + m_iTailWords : pFirst;

	const unsigned int iBorderSlots = minimum(AUDIO_RING_SLOTS, stMode.iVActive - iHeight);
	uint32_t * pBorders = pRows + iRowSlots * iJoinWords;
	for (unsigned int iSlot = 0; iSlot < iBorderSlots; iSlot++)
	{
		uint32_t * pLine = pBorders + iSlot * BORDER_ISLAND_WORDS;
		unsigned int iWords = putHSyncIsland(pLine, false, true, iNull);
		pLine[iWords++] = CMD_TMDS_REPEAT | stMode.iHActive;
		pLine[iWords++] = colourWord(0x0000);
	}

	m_bSlotBars = false;
	m_iSlotTop = iBorderTop;
	m_iSlotRows = iHeight;
	m_iSlotHead = bHeadInRing ? iRowSlots - 1u : 0u;
	m_iSlotCount[0] = bHeadInRing ? 0u : 1u;
	m_iSlotFirst[0] = POOL_SLOTS + LINE_PACKET;
	m_iSlotStride[0] = 0;
	m_iSlotCount[1] = iRowSlots;
	m_iSlotFirst[1] = (unsigned int)(pRows - m_pAudioFramingWords) + m_iTailWords + LINE_PACKET;
	m_iSlotStride[1] = iJoinWords;
	m_iSlotCount[2] = iBorderSlots;
	m_iSlotFirst[2] = (unsigned int)(pBorders - m_pAudioFramingWords) + LINE_PACKET;
	m_iSlotStride[2] = BORDER_ISLAND_WORDS;

	if (!addBlankingAudioFraming(obSink))
		return false;
	if (!addBorderAudioFraming(obSink, pBorders, iBorderSlots, 0, iBorderTop))
		return false;

	if (!obSink.addWords(pHead, iHeadWords))
		return false;
	for (unsigned int iRow = 0; iRow < iHeight; iRow++)
	{
		if (!obSink.addPixels(pPixels + iRow * iStridePixels, iWidth))
			return false;
		if (iRow + 1u < iHeight && !obSink.addJoin(pRows + (iRow % iRowSlots) * iJoinWords, iJoinWords))
			return false;
	}
	if (m_iTailWords != 0 && !obSink.addWords(m_iLineTail, m_iTailWords))
		return false;

	return addBorderAudioFraming(obSink, pBorders, iBorderSlots, iBorderTop, iBorderBottom);
}

unsigned int rpDVIFrame::audioSlotCount() const
{
	unsigned int iSlots = 0;
	for (unsigned int iRun = 0; iRun < SLOT_RUNS; iRun++)
		iSlots += m_iSlotCount[iRun];
	return iSlots;
}

// Blanking slot s is line s % 3 + 1 of block s / 3.
uint32_t * rpDVIFrame::audioSlotWords(unsigned int iSlot) const
{
	for (unsigned int iRun = 0; iRun < SLOT_RUNS; iRun++)
	{
		if (iSlot < m_iSlotCount[iRun])
		{
			const unsigned int iStep = iRun == SLOT_RUN_BLANK ?
				iSlot / (BLOCK_LINES - 1u) * BLOCK_LINES + iSlot % (BLOCK_LINES - 1u) + 1u : iSlot;
			return m_pAudioFramingWords + m_iSlotFirst[iRun] + iStep * m_iSlotStride[iRun];
		}
		iSlot -= m_iSlotCount[iRun];
	}
	return nullptr;
}

// Bars: active line y uses ring slot y. Picture: the first row uses the head
// slot, or with a full row ring that ring's last slot, row r after it row ring
// slot r - 1, border line b (counted over the top and bottom borders) border
// ring slot b, each modulo its ring. Vertical blanking: the blanking slot of
// its block line, none on the clock regeneration, sync and fixed lines.
int rpDVIFrame::audioLineSlot(unsigned int iLine) const
{
	const unsigned int iVBlank = (unsigned int)m_stMode.iVFrontPorch + m_stMode.iVSync + m_stMode.iVBackPorch;
	if (audioSlotCount() == 0 || iLine >= iVBlank + m_stMode.iVActive)
		return -1;
	if (iLine < iVBlank)
	{
		const int iBlock = blockLine(m_stMode, iLine);
		if (m_iSlotCount[SLOT_RUN_BLANK] == 0 || iBlock < 0 || iBlock % (int)BLOCK_LINES == 0)
			return -1;
		const unsigned int iFirst = m_iSlotCount[0] + m_iSlotCount[1] + m_iSlotCount[2];
		return (int)(iFirst + ((unsigned int)iBlock / BLOCK_LINES % BLOCK_COUNT) * (BLOCK_LINES - 1u) +
			(unsigned int)iBlock % BLOCK_LINES - 1u);
	}
	const unsigned int iY = iLine - iVBlank;
	if (m_bSlotBars)
		return (int)(iY % m_iSlotCount[0]);
	if (iY == m_iSlotTop)
		return (int)m_iSlotHead;
	if (iY > m_iSlotTop && iY < m_iSlotTop + m_iSlotRows)
		return (int)(m_iSlotCount[0] + (iY - m_iSlotTop - 1u) % m_iSlotCount[1]);
	const unsigned int iBorder = iY < m_iSlotTop ? iY : iY - m_iSlotRows;
	return (int)(m_iSlotCount[0] + m_iSlotCount[1] + iBorder % m_iSlotCount[2]);
}

bool rpDVIFrame::audioVLevel() const
{
	return vLevel(false);
}

bool rpDVIFrame::audioHLevel() const
{
	return hLevel(true);
}
