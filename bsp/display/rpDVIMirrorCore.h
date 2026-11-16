#pragma once

#include <stdint.h>

// Row bookkeeping of the TV line ring (tvout_ring.c). No hardware and no clock:
// the caller reads the scan-out position and carries out what these functions
// decide. The reader functions run in an interrupt, the switch functions
// outside it: on the other core, or on the same core below the interrupt.
//
// The scan-out position is the index of the frame piece after the one being
// sent (rpDVIList::scanPiece), counted as if each piece were one list entry. A
// picture row is two pieces: its pixels, then the piece that joins it to the
// next row.

inline constexpr uint32_t DVI_MIRROR_RING_ROWS = 8;         // line buffers
inline constexpr uint32_t DVI_MIRROR_READ_AHEAD = 6;        // rows the reader keeps ahead of the TV
inline constexpr uint32_t DVI_MIRROR_START_MARGIN = 2;      // rows between the TV and the reader's first row
inline constexpr uint32_t DVI_MIRROR_READY_ROWS = 4;        // rows from the TV's row on that are buffered before the switch
inline constexpr uint32_t DVI_MIRROR_READS_PER_TICK = 3;    // bounds how long the reader's interrupt runs
inline constexpr uint32_t DVI_MIRROR_TICK_GAP_US = 8;       // from a tick that fetched rows to the next: other interrupts run here
inline constexpr uint32_t DVI_MIRROR_TICK_IDLE_US = 30;     // from a tick with nothing to fetch to the next: under one TV line

static_assert(DVI_MIRROR_RING_ROWS > DVI_MIRROR_READ_AHEAD + 1, "a line buffer would be refilled before the TV has shown it");
static_assert(DVI_MIRROR_READY_ROWS <= DVI_MIRROR_READ_AHEAD, "the reader never gets that far ahead");

struct stDVIMirrorCore
{
	uint32_t iRows;
	uint32_t iFirstRowEntry;
	uint32_t iListCount;

	volatile bool bReading;             // the reader runs
	volatile bool bShown;               // the TV is reading the line buffers
	volatile uint32_t iSeq;             // odd while the reader is inside a tick
	volatile uint32_t iStartRow;        // the line buffers hold rows iStartRow up to iNextRow - 1
	volatile uint32_t iNextRow;
	volatile uint32_t iLate;            // rows the TV reached before they were fetched, while shown
};

#define DVI_MIRROR_INLINE static inline __attribute__((always_inline))

DVI_MIRROR_INLINE void mirrorInit(stDVIMirrorCore & st, uint32_t iRows, uint32_t iFirstRowEntry, uint32_t iListCount)
{
	st.iRows = iRows;
	st.iFirstRowEntry = iFirstRowEntry;
	st.iListCount = iListCount;
	st.bReading = false;
	st.bShown = false;
	st.iSeq = 0;
	st.iStartRow = 0;
	st.iNextRow = 0;
	st.iLate = 0;
}

DVI_MIRROR_INLINE uint32_t mirrorRingSlot(uint32_t iRow)
{
	return iRow % DVI_MIRROR_RING_ROWS;
}

// The row the TV is on, or is about to start: -1 above the picture, 0 to
// iRows - 1 in it, iRows below it.
DVI_MIRROR_INLINE int mirrorScanRow(const stDVIMirrorCore & st, uint32_t iNextEntry)
{
	const int iOffset = (int)iNextEntry - 1 - (int)st.iFirstRowEntry;
	if (iOffset < 0)
		return -1;
	const int iRow = (iOffset + 1) / 2;
	return iRow >= (int)st.iRows ? (int)st.iRows : iRow;
}

// Pieces left before the first picture row, counted round the end of the list.
// Outside the picture a piece is one line or a part of one.
DVI_MIRROR_INLINE int mirrorPiecesToPicture(const stDVIMirrorCore & st, uint32_t iNextEntry)
{
	int iPieces = (int)st.iFirstRowEntry - (int)iNextEntry;
	if (iPieces < 0)
		iPieces += (int)st.iListCount;
	return iPieces;
}

// ----------------------------------------------------------------------------
// The reader's tick.

// Returns how many rows to fetch now, from iFirstRow up. mirrorReaderEnd()
// follows, whatever the count.
DVI_MIRROR_INLINE uint32_t mirrorReaderBegin(stDVIMirrorCore & st, uint32_t iNextEntry, uint32_t & iFirstRow)
{
	st.iSeq = st.iSeq + 1;

	const int iScan = mirrorScanRow(st, iNextEntry);
	uint32_t iNext = st.iNextRow;
	uint32_t iWant;
	if (iScan < 0 || iScan >= (int)st.iRows)
	{
		// Outside the picture: the first rows of the next frame. A row number
		// past those belongs to the frame that has just ended.
		if (iNext > DVI_MIRROR_READ_AHEAD)
		{
			// Rows of that frame the TV passed before they were fetched.
			if (st.bShown && iNext < st.iRows)
				st.iLate = st.iLate + st.iRows - iNext;
			iNext = 0;
			st.iStartRow = 0;
		}
		iWant = DVI_MIRROR_READ_AHEAD - 1;
	}
	else
	{
		iWant = (uint32_t)iScan + DVI_MIRROR_READ_AHEAD;
		if (iWant > st.iRows - 1)
			iWant = st.iRows - 1;
		if (iNext <= (uint32_t)iScan || iNext > iWant + 1u)
		{
			// The TV has reached rows that were not fetched: the rows from
			// iNext on, or, with a row number left from the frame before, the
			// rest of that frame and every row of this one down to the TV's.
			if (st.bShown)
			{
				if (iNext <= (uint32_t)iScan)
					st.iLate = st.iLate + (uint32_t)iScan + 1u - iNext;
				else
					st.iLate = st.iLate + (iNext < st.iRows ? st.iRows - iNext : 0u) + (uint32_t)iScan + 1u;
			}
			iNext = (uint32_t)iScan + 1u;
			st.iStartRow = iNext;
		}
	}

	iFirstRow = iNext;
	if (iNext > iWant)
		return 0;
	const uint32_t iCount = iWant - iNext + 1u;
	return iCount > DVI_MIRROR_READS_PER_TICK ? DVI_MIRROR_READS_PER_TICK : iCount;
}

// After each row is in its line buffer. iNextEntry is read after the fetch: a
// TV that is on the row by then started on it before it was whole.
DVI_MIRROR_INLINE void mirrorReaderFetched(stDVIMirrorCore & st, uint32_t iRow, uint32_t iNextEntry)
{
	if (st.bShown && iNextEntry >= st.iFirstRowEntry + 2u * iRow + 1u &&
	    iNextEntry <= st.iFirstRowEntry + 2u * st.iRows)
		st.iLate = st.iLate + 1u;
}

// Returns the microseconds from now to the next tick.
DVI_MIRROR_INLINE uint32_t mirrorReaderEnd(stDVIMirrorCore & st, uint32_t iFirstRow, uint32_t iFetched)
{
	st.iNextRow = iFirstRow + iFetched;
	st.iSeq = st.iSeq + 1;
	return iFetched ? DVI_MIRROR_TICK_GAP_US : DVI_MIRROR_TICK_IDLE_US;
}

// ----------------------------------------------------------------------------
// Moving the TV from the picture buffer to the line buffers, on the other core.
// The picture buffer and its copy hold the same picture throughout, so a row may
// be read from either while the list is rewritten.

// Step 1, repeated until true: the reader is given its first row,
// DVI_MIRROR_START_MARGIN rows below the TV, or row 0 when the TV is outside the
// picture. The caller then raises the reader's first tick.
DVI_MIRROR_INLINE bool mirrorSwitchStart(stDVIMirrorCore & st, uint32_t iNextEntry)
{
	const int iScan = mirrorScanRow(st, iNextEntry);
	uint32_t iFirst;
	if (iScan < 0 || iScan >= (int)st.iRows)
	{
		if (mirrorPiecesToPicture(st, iNextEntry) <= (int)DVI_MIRROR_START_MARGIN)
			return false;
		iFirst = 0;
	}
	else
	{
		if ((uint32_t)iScan + DVI_MIRROR_START_MARGIN >= st.iRows)
			return false;
		iFirst = (uint32_t)iScan + DVI_MIRROR_START_MARGIN;
	}
	st.iStartRow = iFirst;
	st.iNextRow = iFirst;
	st.bReading = true;
	return true;
}

// Step 2, repeated until true: the rows the TV reads next are in the line
// buffers. fnNextEntry is called after the reader's row numbers have been taken
// between two of its ticks. The caller then points every row of the list at its
// line buffer and calls mirrorSwitchDone().
template <typename tNextEntry>
DVI_MIRROR_INLINE bool mirrorSwitchReady(const stDVIMirrorCore & st, tNextEntry && fnNextEntry)
{
	const uint32_t iSeq = st.iSeq;
	const uint32_t iStart = st.iStartRow;
	const uint32_t iNext = st.iNextRow;
	if ((iSeq & 1u) || iSeq != st.iSeq)
		return false;

	const int iScan = mirrorScanRow(st, fnNextEntry());
	if (iScan < 0 || iScan >= (int)st.iRows)
		return iStart == 0 && iNext >= DVI_MIRROR_READY_ROWS && iNext <= DVI_MIRROR_READ_AHEAD;
	uint32_t iNeed = (uint32_t)iScan + DVI_MIRROR_READY_ROWS;
	if (iNeed > st.iRows)
		iNeed = st.iRows;
	return (uint32_t)iScan >= iStart && iNext >= iNeed;
}

DVI_MIRROR_INLINE void mirrorSwitchDone(stDVIMirrorCore & st)
{
	st.bShown = true;
}

// The TV goes back to the picture buffer, or the switch is given up. The reader
// stops at its next tick, so rows the TV has reached without their being
// fetched are counted here, unless the reader is inside a tick and has counted
// them itself.
DVI_MIRROR_INLINE void mirrorRelease(stDVIMirrorCore & st, uint32_t iNextEntry)
{
	const bool bWasShown = st.bShown;
	st.bShown = false;
	st.bReading = false;
	if (!bWasShown || (st.iSeq & 1u))
		return;

	const int iScan = mirrorScanRow(st, iNextEntry);
	const uint32_t iNext = st.iNextRow;
	if (iScan >= 0 && iScan < (int)st.iRows)
	{
		if (iNext <= (uint32_t)iScan)
			st.iLate = st.iLate + (uint32_t)iScan + 1u - iNext;
	}
	else if (iNext > DVI_MIRROR_READ_AHEAD && iNext < st.iRows)
		st.iLate = st.iLate + st.iRows - iNext;
}
