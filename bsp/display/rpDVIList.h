#pragma once

#include <stdint.h>
#include "fwPortability.h"
#include "rpDVIFrame.h"

// One entry of the scan-out list: the four words the list channel writes into
// the pixel channel's control, write address, transfer count and read address
// registers. The last of the four starts the pixel channel.
struct stDVIListEntry
{
	uint32_t iCtrl;
	uint32_t iWriteAddr;
	uint32_t iCount;
	uint32_t iReadAddr;
};

// One picture row after the first in the compact list: the two words the row
// channel writes into the pixel channel's control and read address registers.
// The row channel then starts the pixel channel; its transfer count is the
// first row's, kept from that row's entry.
struct stDVIRowEntry
{
	uint32_t iCtrl;
	uint32_t iReadAddr;
};

// Entries alike, one after another, for the scan position. Picture rows have
// iLineWords 0.
struct stDVIScanRun
{
	uint16_t iEntry;        // the run's first entry
	uint16_t iPiece;        // the frame piece its first entry starts with
	uint16_t iFirstWords;   // lines: words of each entry's first line; rows: the rows
	uint8_t iLineWords;     // lines: words of each line after the first
	uint8_t iLines;         // lines: lines per entry
};

// The scan-out list of one frame, built from the pieces rpDVIFrame hands its
// sink. No hardware access: register addresses and control words are given.
//
// Plain (the colour bars): one entry per piece, lines included.
//
// Compact (a picture): lines handed together are one entry.
// Only the first picture row is an entry; the rows after it are row entries in
// a table of their own. A row chains to the join channel, which sends the next
// join of the row ring and chains to the row channel; the row channel loads the
// next row's row entry into the pixel channel and starts it. A row whose join
// is the ring's first chains to the restart channel instead, which puts the
// join channel back at slot 0 and starts it. The last row chains to the list
// channel. Ahead of the first row an entry of one unpaced word puts the row
// channel back at the table's first row entry. With DVI framing every join is
// the same one, a ring of one slot: every row chains to the restart channel.
//
// The scan position is counted in frame pieces in both forms: the index of the
// piece after the one being sent, a piece being one sink call with every line
// and join a call of its own (the pieces rpDVIFrame's picturePieceCount and
// barsPieceCount count).
class rpDVIList : public rpDVIFrameSink
{
public:
	static const unsigned int SCAN_RUNS = 16;

	struct stConfig
	{
		uint32_t iCtrlWords;        // pixel channel: 32-bit words to the FIFO, then the list channel
		uint32_t iCtrlPixels;       // pixel channel: 16-bit pixels to the FIFO, then the list channel
		uint32_t iCtrlCopy;         // pixel channel: one word, unpaced, then the list channel
		uint32_t iChainBits;        // the control word's chain field
		uint32_t iChainShift;
		uint32_t iJoinChannel;
		uint32_t iRestartChannel;   // puts the join channel back at slot 0 and starts it
		uint32_t iRewindChannel;
		uint32_t iFifo;             // the HSTX FIFO
		uint32_t iRowReadAddr;      // the row channel's READ_ADDR register
	};

	// pRuns, SCAN_RUNS long, and pRows, iRowSize long, are used in the compact
	// form only. stConfig is read until finish().
	void begin(stDVIListEntry * pList, unsigned int iSize, stDVIScanRun * pRuns, stDVIRowEntry * pRows,
	           unsigned int iRowSize, const stConfig & stConfig, bool bCompact);
	// The last piece chains to the rewind channel. False if the frame did not
	// build into the list.
	bool finish();

	bool compact() const { return m_bCompact; }
	stDVIListEntry * list() const { return m_pList; }
	unsigned int count() const { return m_iCount; }

	// The word that holds picture row iRow's read address; it may be rewritten
	// while video runs.
	uint32_t * rowReadAddress(unsigned int iRow) const
	{
		if (!m_bCompact)
			return &m_pList[m_iFirstRowEntry + iRow * 2u].iReadAddr;
		return iRow == 0 ? &m_pList[m_iFirstRowEntry].iReadAddr : &m_pRows[iRow - 1u].iReadAddr;
	}
	unsigned int rows() const { return m_iRows; }
	// Frame pieces, and the piece of the first picture row.
	unsigned int scanCount() const { return m_bCompact ? m_iPieces : m_iCount; }
	unsigned int scanFirstRow() const { return m_iFirstRowPiece; }

	// The join channel's first read address and transfer count, and the word
	// the restart channel copies into its read address. Valid in the compact
	// form with joins.
	bool joins() const { return m_iJoins != 0; }
	uint32_t joinStart() const { return m_iJoinStart; }
	const uint32_t * joinStartWord() const { return &m_iJoinStart; }
	unsigned int joinWords() const { return m_iJoinWords; }
	// The row table's address, which the row channel starts each frame from,
	// and the list entries loaded while it walks the rows (0 without a table).
	uint32_t rowStart() const { return m_iRowStart; }
	unsigned int rowsLoadedAt() const { return m_iRowCount != 0 ? m_iFirstRowEntry + 1u : 0u; }

	// List entries, row entries and heap bytes (list, scan runs, row table and
	// the audio framing pool) of a frame; 0 if it cannot be built.
	static bool isCompact(bool bPicture) { return bPicture; }
	static unsigned int entryCount(const stDVIMode & stMode, bool bPicture, unsigned int iWidth, unsigned int iHeight, bool bAudioFraming);
	static unsigned int rowEntryCount(bool bPicture, unsigned int iHeight);
	static unsigned int frameBytes(const stDVIMode & stMode, bool bPicture, unsigned int iWidth, unsigned int iHeight, bool bAudioFraming);

	// The scan position from the entries the list channel has loaded, the pixel
	// channel's transfers left and the row entries the row channel has loaded,
	// read while the list channel and the row channel were idle.
	FW_ALWAYS_INLINE uint32_t scanPiece(uint32_t iLoaded, uint32_t iLeft, uint32_t iRowsLoaded) const
	{
		if (!m_bCompact)
			return iLoaded;
		if (iLoaded == 0 || m_iRunCount == 0)
			return 0;
		const uint32_t iEntry = iLoaded - 1u;
		uint32_t iRun = m_iRunCount - 1u;
		while (iRun > 0 && m_pRuns[iRun].iEntry > iEntry)
			iRun--;
		const stDVIScanRun & stRun = m_pRuns[iRun];
		const uint32_t iAt = iEntry - stRun.iEntry;
		if (stRun.iLineWords == 0)
		{
			// Rows: with a row table, the row channel's rewind, then the first
			// row's entry, the rows after it from the row channel. A finished
			// row is followed by its join, except the last.
			if (m_iRowCount != 0 && iAt == 0)
				return stRun.iPiece;
			const uint32_t iRow = m_iRowCount == 0 ? 0u : (iRowsLoaded < m_iRowCount ? iRowsLoaded : m_iRowCount);
			if (iLeft != 0 || iRow + 1u >= stRun.iFirstWords)
				return stRun.iPiece + 2u * iRow + 1u;
			return stRun.iPiece + 2u * iRow + 2u;
		}
		const uint32_t iLines = stRun.iLines;
		const uint32_t iBase = stRun.iPiece + iAt * iLines;
		if (iLines == 1u || iLeft == 0)
			return iBase + iLines;
		const uint32_t iWords = stRun.iFirstWords + (iLines - 1u) * stRun.iLineWords;
		if (iLeft >= iWords)
			return iBase + 1u;
		const uint32_t iDone = iWords - iLeft;
		if (iDone < stRun.iFirstWords)
			return iBase + 1u;
		return iBase + 2u + (iDone - stRun.iFirstWords) / stRun.iLineWords;
	}

protected:
	// The bus address of p as an entry holds it.
	virtual uint32_t busAddress(const void * p) const { return (uint32_t)(uintptr_t)p; }

private:
	bool addWords(const uint32_t * pWords, unsigned int iCount) override;
	bool addPixels(const uint16_t * pPixels, unsigned int iCount) override;
	bool addLines(const uint32_t * pWords, unsigned int iFirstWords, unsigned int iLineWords, unsigned int iLines) override;
	bool addJoin(const uint32_t * pWords, unsigned int iCount) override;

	bool addEntry(uint32_t iCtrl, uint32_t iWriteAddr, uint32_t iCount, uint32_t iReadAddr);
	bool addRun(unsigned int iFirstWords, unsigned int iLineWords, unsigned int iLines);
	bool insertRowRewind();
	bool closeRows();
	uint32_t * rowCtrl(unsigned int iRow) const;

	stDVIListEntry * m_pList = nullptr;
	unsigned int m_iSize = 0;
	unsigned int m_iCount = 0;
	stDVIScanRun * m_pRuns = nullptr;
	unsigned int m_iRunCount = 0;
	stDVIRowEntry * m_pRows = nullptr;
	unsigned int m_iRowSize = 0;
	unsigned int m_iRowCount = 0;           // row entries so far
	const stConfig * m_pConfig = nullptr;   // during the build
	bool m_bCompact = false;
	bool m_bFailed = false;
	bool m_bLastIsRowEntry = false;         // the last piece so far is a row entry

	unsigned int m_iPieces = 0;             // frame pieces so far
	unsigned int m_iRows = 0;
	unsigned int m_iFirstRowEntry = 0;
	unsigned int m_iFirstRowPiece = 0;
	bool m_bInRows = false;
	bool m_bRowsDone = false;

	unsigned int m_iJoins = 0;
	unsigned int m_iJoinWords = 0;
	unsigned int m_iJoinPeriod = 0;         // rows from one restart to the next; 0 until the ring wraps
	const uint32_t * m_pJoinFirst = nullptr;
	const uint32_t * m_pJoinNext = nullptr;
	uint32_t m_iJoinStart = 0;              // read by the restart channel: RAM
	uint32_t m_iRowStart = 0;               // read by the row channel's rewind entry: RAM
};
