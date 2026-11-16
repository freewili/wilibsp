#include "rpDVIList.h"

void rpDVIList::begin(stDVIListEntry * pList, unsigned int iSize, stDVIScanRun * pRuns, stDVIRowEntry * pRows,
                      unsigned int iRowSize, const stConfig & stConfig, bool bCompact)
{
	m_pList = pList;
	m_iSize = pList != nullptr ? iSize : 0u;
	m_iCount = 0;
	m_pRuns = bCompact ? pRuns : nullptr;
	m_iRunCount = 0;
	m_pRows = bCompact ? pRows : nullptr;
	m_iRowSize = m_pRows != nullptr ? iRowSize : 0u;
	m_iRowCount = 0;
	m_pConfig = &stConfig;
	m_bCompact = bCompact && pRuns != nullptr;
	m_bFailed = bCompact && pRuns == nullptr;
	m_bLastIsRowEntry = false;
	m_iPieces = 0;
	m_iRows = 0;
	m_iFirstRowEntry = 0;
	m_iFirstRowPiece = 0;
	m_bInRows = false;
	m_bRowsDone = false;
	m_iJoins = 0;
	m_iJoinWords = 0;
	m_iJoinPeriod = 0;
	m_pJoinFirst = nullptr;
	m_pJoinNext = nullptr;
	m_iJoinStart = 0;
	m_iRowStart = m_pRows != nullptr ? busAddress(m_pRows) : 0u;
}

bool rpDVIList::addEntry(uint32_t iCtrl, uint32_t iWriteAddr, uint32_t iCount, uint32_t iReadAddr)
{
	// A transfer count of zero would stop the chain.
	if (m_bFailed || m_iCount >= m_iSize || iCount == 0)
		return false;
	stDVIListEntry & stEntry = m_pList[m_iCount++];
	stEntry.iCtrl = iCtrl;
	stEntry.iWriteAddr = iWriteAddr;
	stEntry.iCount = iCount;
	stEntry.iReadAddr = iReadAddr;
	m_bLastIsRowEntry = false;
	return true;
}

// The run for the entry about to be added: the open one when it is alike.
bool rpDVIList::addRun(unsigned int iFirstWords, unsigned int iLineWords, unsigned int iLines)
{
	if (iLines == 1u)
		iFirstWords = iLineWords = 1u;
	if (iLineWords == 0 || iLineWords > 0xffu || iLines > 0xffu || iFirstWords > 0xffffu ||
		m_iCount > 0xffffu || m_iPieces > 0xffffu)
		return false;
	if (m_iRunCount != 0)
	{
		const stDVIScanRun & stLast = m_pRuns[m_iRunCount - 1u];
		if (stLast.iLineWords == iLineWords && stLast.iLines == iLines && stLast.iFirstWords == iFirstWords)
			return true;
	}
	if (m_iRunCount >= SCAN_RUNS)
		return false;
	stDVIScanRun & stRun = m_pRuns[m_iRunCount++];
	stRun.iEntry = (uint16_t)m_iCount;
	stRun.iPiece = (uint16_t)m_iPieces;
	stRun.iFirstWords = (uint16_t)iFirstWords;
	stRun.iLineWords = (uint8_t)iLineWords;
	stRun.iLines = (uint8_t)iLines;
	return true;
}

// The row channel back to the table's first row entry, ahead of the first row,
// the entry just added.
bool rpDVIList::insertRowRewind()
{
	if (m_iCount == 0 || m_iCount >= m_iSize)
		return false;
	m_pList[m_iCount] = m_pList[m_iCount - 1u];
	stDVIListEntry & stEntry = m_pList[m_iCount - 1u];
	stEntry.iCtrl = m_pConfig->iCtrlCopy;
	stEntry.iWriteAddr = m_pConfig->iRowReadAddr;
	stEntry.iCount = 1;
	stEntry.iReadAddr = busAddress(&m_iRowStart);
	m_iCount++;
	m_iFirstRowEntry++;
	return true;
}

// The control word of picture row iRow: its list entry or its row entry.
uint32_t * rpDVIList::rowCtrl(unsigned int iRow) const
{
	return iRow == 0 ? &m_pList[m_iFirstRowEntry].iCtrl : &m_pRows[iRow - 1u].iCtrl;
}

// The rows have ended: their run records them. Every row after the first has
// its row entry and follows a join.
bool rpDVIList::closeRows()
{
	if (!m_bInRows)
		return true;
	m_bInRows = false;
	m_bRowsDone = true;
	if (m_iRowCount + 1u != m_iRows || m_iJoins + 1u != m_iRows)
		return false;
	stDVIScanRun & stRun = m_pRuns[m_iRunCount - 1u];
	stRun.iFirstWords = (uint16_t)m_iRows;
	stRun.iLines = 0;
	return true;
}

bool rpDVIList::addWords(const uint32_t * pWords, unsigned int iCount)
{
	if (m_bCompact && (!closeRows() || !addRun(1, 1, 1)))
		return false;
	if (!addEntry(m_pConfig->iCtrlWords, m_pConfig->iFifo, iCount, busAddress(pWords)))
		return false;
	m_iPieces++;
	return true;
}

bool rpDVIList::addLines(const uint32_t * pWords, unsigned int iFirstWords, unsigned int iLineWords, unsigned int iLines)
{
	if (!m_bCompact)
		return rpDVIFrameSink::addLines(pWords, iFirstWords, iLineWords, iLines);
	if (iLines == 0)
		return true;
	if (!closeRows() || !addRun(iFirstWords, iLineWords, iLines) ||
		!addEntry(m_pConfig->iCtrlWords, m_pConfig->iFifo, iFirstWords + (iLines - 1u) * iLineWords, busAddress(pWords)))
		return false;
	m_iPieces += iLines;
	return true;
}

bool rpDVIList::addPixels(const uint16_t * pPixels, unsigned int iCount)
{
	if (m_bCompact && !m_bInRows)
	{
		// One run of rows per frame, its fields filled in when the rows end.
		if (m_bRowsDone || m_iRunCount >= SCAN_RUNS || m_iCount > 0xffffu || m_iPieces > 0xffffu)
			return false;
		stDVIScanRun & stRun = m_pRuns[m_iRunCount++];
		stRun.iEntry = (uint16_t)m_iCount;
		stRun.iPiece = (uint16_t)m_iPieces;
		stRun.iFirstWords = 0;
		stRun.iLineWords = 0;
		stRun.iLines = 0;
		m_bInRows = true;
	}
	if (m_iRows == 0)
	{
		m_iFirstRowEntry = m_iCount;
		m_iFirstRowPiece = m_iPieces;
	}
	if (m_bCompact && m_iRows != 0)
	{
		// Reached through the join before it, with the first row's transfer count.
		if (m_iJoins != m_iRows || m_iRowCount >= m_iRowSize || iCount != m_pList[m_iFirstRowEntry].iCount)
			return false;
		stDVIRowEntry & stRow = m_pRows[m_iRowCount++];
		stRow.iCtrl = m_pConfig->iCtrlPixels;
		stRow.iReadAddr = busAddress(pPixels);
		m_bLastIsRowEntry = true;
	}
	else if (!addEntry(m_pConfig->iCtrlPixels, m_pConfig->iFifo, iCount, busAddress(pPixels)))
		return false;
	m_iRows++;
	m_iPieces++;
	return true;
}

// The row just added chains to the join channel, or to the restart channel
// when its join is the ring's first. The joins must walk one ring of equal
// slots in order, back to the first slot at a fixed period.
bool rpDVIList::addJoin(const uint32_t * pWords, unsigned int iCount)
{
	if (!m_bCompact)
		return addWords(pWords, iCount);
	if (!m_bInRows || m_iCount == 0 || iCount == 0 || m_iJoins + 1u != m_iRows || m_pRows == nullptr)
		return false;
	const unsigned int iRow = m_iRows - 1u;
	bool bRestart;
	if (m_iJoins == 0)
	{
		m_pJoinFirst = pWords;
		m_iJoinWords = iCount;
		m_iJoinStart = busAddress(pWords);
		bRestart = true;
	}
	else
	{
		bRestart = pWords == m_pJoinFirst;
		if (bRestart && m_iJoinPeriod == 0)
			m_iJoinPeriod = iRow;
		if (iCount != m_iJoinWords || (!bRestart && pWords != m_pJoinNext) ||
			(m_iJoinPeriod != 0 && bRestart != (iRow % m_iJoinPeriod == 0)))
			return false;
	}
	if (m_iJoins == 0 && !insertRowRewind())
		return false;
	uint32_t * pCtrl = rowCtrl(iRow);
	const uint32_t iChannel = bRestart ? m_pConfig->iRestartChannel : m_pConfig->iJoinChannel;
	*pCtrl = (*pCtrl & ~m_pConfig->iChainBits) | (iChannel << m_pConfig->iChainShift);
	m_pJoinNext = pWords + iCount;
	m_iJoins++;
	m_iPieces++;
	return true;
}

bool rpDVIList::finish()
{
	if (m_bFailed || (m_bCompact && !closeRows()) || m_iCount == 0)
		return false;
	uint32_t & iLast = m_bLastIsRowEntry ? m_pRows[m_iRowCount - 1u].iCtrl : m_pList[m_iCount - 1u].iCtrl;
	iLast = (iLast & ~m_pConfig->iChainBits) | (m_pConfig->iRewindChannel << m_pConfig->iChainShift);
	m_pConfig = nullptr;
	return true;
}

unsigned int rpDVIList::entryCount(const stDVIMode & stMode, bool bPicture, unsigned int iWidth, unsigned int iHeight, bool bAudioFraming)
{
	if (!bPicture)
		return rpDVIFrame::barsPieceCount(stMode);
	const unsigned int iCalls = bAudioFraming ? rpDVIFrame::audioFramingPictureCalls(stMode, iWidth, iHeight)
	                                          : rpDVIFrame::pictureCalls(stMode, iWidth, iHeight);
	if (iCalls == 0)
		return 0;
	// The rows after the first are row entries; the row channel's rewind is an entry.
	const unsigned int iRowEntries = rowEntryCount(bPicture, iHeight);
	return iCalls - iRowEntries + (iRowEntries != 0 ? 1u : 0u);
}

unsigned int rpDVIList::rowEntryCount(bool bPicture, unsigned int iHeight)
{
	return isCompact(bPicture) && iHeight > 1u ? iHeight - 1u : 0u;
}

unsigned int rpDVIList::frameBytes(const stDVIMode & stMode, bool bPicture, unsigned int iWidth, unsigned int iHeight, bool bAudioFraming)
{
	const unsigned int iEntries = entryCount(stMode, bPicture, iWidth, iHeight, bAudioFraming);
	const unsigned int iWords = !bAudioFraming ? 0u :
		(bPicture ? rpDVIFrame::audioFramingPictureWords(stMode, iWidth, iHeight) : rpDVIFrame::audioFramingBarsWords(stMode));
	if (iEntries == 0 || (bAudioFraming && iWords == 0))
		return 0;
	const unsigned int iRuns = isCompact(bPicture) ? SCAN_RUNS : 0u;
	return (unsigned int)(sizeof(stDVIListEntry) * iEntries + sizeof(stDVIScanRun) * iRuns +
		sizeof(stDVIRowEntry) * rowEntryCount(bPicture, iHeight) + sizeof(uint32_t) * iWords);
}
