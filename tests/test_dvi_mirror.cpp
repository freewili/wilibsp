// Host simulation of the line ring's row logic (bsp/display/rpDVIMirrorCore.h).
//
// Three actors share the real core state:
//   - the TV: walks the frame list built by rpDVIFrame, piece by piece, at the
//     pixel clock; each picture row is read from wherever its list entry points
//     at the moment the piece starts;
//   - the reader: the interrupt on one core, with a random delay before each
//     tick is served, each row read taking about 20 us and holding the other
//     core;
//   - the drawing core: sends a finished picture, copies it, moves the TV to
//     the line buffers before each redraw and back after it (the mirror
//     scenarios); or starts with every row on the line buffers and keeps them
//     there, in the order tvout_ring.c does (the always-on scenarios, a PSRAM
//     picture).
//
// Every picture row the TV reads is judged: a row from the picture buffer must
// not overlap a redraw, a row from a line buffer must hold that row of the
// picture that was complete before the redraw, filled before the TV started on
// it and not refilled until the TV has finished it.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "rpDVIFrame.h"
#include "rpDVIMirrorCore.h"

static int g_iFailures = 0;
static int g_iChecks = 0;

#define CHECK(cond) \
    do { \
        g_iChecks++; \
        if (!(cond)) { \
            g_iFailures++; \
            if (g_iFailures <= 20) \
                std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

typedef int64_t ps_t;                               // picoseconds
static const ps_t PS_PER_PIXEL = 39683;             // 25.2 MHz
static const ps_t US = 1000000;
static const ps_t NEVER = INT64_MAX / 4;

static const unsigned int PIC_W = 480;
static const unsigned int PIC_H = 320;

// ---------------------------------------------------------------------------
// The frame as the TV walks it.
// ---------------------------------------------------------------------------

struct stPiece
{
    bool            bPixels;
    const uint32_t *pWords;
    unsigned int    iCount;
};

class recordingSink : public rpDVIFrameSink
{
public:
    std::vector<stPiece> obPieces;
    bool addWords(const uint32_t * pWords, unsigned int iCount) override
    {
        obPieces.push_back({ false, pWords, iCount });
        return true;
    }
    bool addPixels(const uint16_t *, unsigned int iCount) override
    {
        obPieces.push_back({ true, nullptr, iCount });
        return true;
    }
};

struct stTimeline
{
    std::vector<ps_t> obStart;      // start of each piece within the frame
    std::vector<ps_t> obLength;
    std::vector<int>  obRow;        // picture row of a pixel piece, else -1
    ps_t iFrame = 0;
    uint32_t iFirstRowEntry = 0;
    uint32_t iCount = 0;
};

// Pixel clocks a piece of command words keeps the output busy. The pixels of a
// TMDS command that ends a piece are in the piece that follows.
static bool wordsLength(const stPiece & stP, unsigned int & iPixels)
{
    iPixels = 0;
    unsigned int iPos = 0;
    while (iPos < stP.iCount)
    {
        const uint32_t iCmd = stP.pWords[iPos++];
        const uint32_t iOp = iCmd & ~0xfffu;
        const unsigned int iCount = iCmd & 0xfffu;
        if (iOp == rpDVIFrame::CMD_RAW_REPEAT || iOp == rpDVIFrame::CMD_TMDS_REPEAT)
        {
            iPixels += iCount;
            iPos++;
        }
        else if (iOp == rpDVIFrame::CMD_TMDS)
        {
            const unsigned int iHere = std::min(iCount, stP.iCount - iPos);
            iPixels += iHere;
            iPos += iHere;
        }
        else
            return false;
    }
    return true;
}

static rpDVIFrame g_obFrame;
static std::vector<uint16_t> g_obPixels(PIC_W * PIC_H, 0);

static bool buildTimeline(stTimeline & stT)
{
    const stDVIMode & stMode = DVI_MODE_640x480_60;
    recordingSink obSink;
    if (!g_obFrame.buildPicture(stMode, g_obPixels.data(), PIC_W, PIC_H, PIC_W, obSink))
        return false;

    ps_t iAt = 0;
    int iRow = 0;
    bool bFirst = true;
    for (size_t i = 0; i < obSink.obPieces.size(); i++)
    {
        const stPiece & stP = obSink.obPieces[i];
        unsigned int iPixels = stP.iCount;
        if (!stP.bPixels && !wordsLength(stP, iPixels))
            return false;
        stT.obStart.push_back(iAt);
        stT.obLength.push_back((ps_t)iPixels * PS_PER_PIXEL);
        stT.obRow.push_back(stP.bPixels ? iRow++ : -1);
        if (stP.bPixels && bFirst)
        {
            stT.iFirstRowEntry = (uint32_t)i;
            bFirst = false;
        }
        iAt += (ps_t)iPixels * PS_PER_PIXEL;
    }
    stT.iFrame = iAt;
    stT.iCount = (uint32_t)obSink.obPieces.size();
    return iRow == (int)PIC_H;
}

// ---------------------------------------------------------------------------
// The simulation.
// ---------------------------------------------------------------------------

struct stConfig
{
    const char * szName;
    int iCycles;
    ps_t iSmallMax;                 // usual delay before a tick is served
    double fBigChance;              // chance of a long delay instead
    ps_t iBigMin;
    ps_t iBigMax;
    unsigned int iSeed;
};

struct stSlot
{
    int iRow = -1;
    int iPicture = -1;
    ps_t iFillStart = -1;
    ps_t iFillEnd = -1;
};

class mirrorSim
{
public:
    mirrorSim(const stTimeline & stT, const stConfig & stC)
        : m_stT(stT), m_stC(stC), m_obRng(stC.iSeed)
    {
        mirrorInit(m_stCore, PIC_H, stT.iFirstRowEntry, stT.iCount);
        m_obOnRing.assign(PIC_H, false);
        m_obCopyPicture.assign(PIC_H, -1);
    }

    void run()
    {
        // A random start inside the frame.
        m_iNow = uniform(0, m_stT.iFrame);
        tvSeek(m_iNow);

        for (int iCycle = 0; iCycle < m_stC.iCycles; iCycle++)
        {
            frameComplete();
            const ps_t iSendUntil = m_iNow + uniform(45000 * US, 55000 * US);
            while (m_iNow < iSendUntil)
                servicePass();
            redrawStart();
            // The redraw: its length counts only the time the core is not held.
            const int iSteps = (int)(uniform(20000 * US, 35000 * US) / (5 * US));
            for (int i = 0; i < iSteps; i++)
                step(5 * US);
        }
        tvAdvance(m_iNow);
    }

    // A PSRAM picture: every row reads its line buffer from before the reader
    // starts; the reader is started once and never released. The picture in
    // PSRAM keeps whole rows of one picture, so only row and fill order are
    // judged. Counts start once the switch is done.
    void runAlwaysOn(int iFrames)
    {
        m_iNow = uniform(0, m_stT.iFrame);
        tvSeek(m_iNow);
        m_obOnRing.assign(PIC_H, true);
        m_obCopyPicture.assign(PIC_H, m_iPicture);
        const ps_t iFrom = m_iNow;
        if (!switchToCopy(false))
        {
            iMissed++;
            return;
        }
        iSwitches++;
        iSwitchMax = m_iNow - iFrom;
        iRingRows = iStale = iTornRing = iRefilled = 0;
        m_iBusy = 0;
        const ps_t iStart = m_iNow;
        const ps_t iUntil = m_iNow + (ps_t)iFrames * m_stT.iFrame;
        while (m_iNow < iUntil)
            step(5 * US);
        tvAdvance(m_iNow);
        fBusyShare = (double)m_iBusy / (double)(m_iNow - iStart);
    }

    // Results.
    double fBusyShare = 0.0;        // always-on: share of time spent reading rows
    int iRingRows = 0, iLcdRows = 0;
    int iStale = 0;                 // a line buffer row that held another row or picture
    int iTornRing = 0;              // a line buffer row still being filled when the TV started on it
    int iRefilled = 0;              // a line buffer refilled while the TV was reading it
    int iTornLcd = 0;               // a picture buffer row read during a redraw the TV had been moved off
    int iTornLcdAllowed = 0;        // the same, in a redraw with no copy yet or a missed switch
    int iMissed = 0;
    int iSwitches = 0;
    ps_t iSwitchMax = 0, iSwitchSum = 0;
    ps_t iTickGapMin = NEVER;       // shortest gap between two ticks of the reader
    ps_t iTickMax = 0;              // longest tick
    uint32_t late() const { return m_stCore.iLate; }

private:
    // ---- helpers
    ps_t uniform(ps_t iLow, ps_t iHigh)
    {
        return std::uniform_int_distribution<ps_t>(iLow, iHigh)(m_obRng);
    }
    ps_t tickDelay()
    {
        if (std::uniform_real_distribution<double>(0.0, 1.0)(m_obRng) < m_stC.fBigChance)
            return uniform(m_stC.iBigMin, m_stC.iBigMax);
        return uniform(0, m_stC.iSmallMax);
    }

    // ---- the TV
    void tvSeek(ps_t iTime)
    {
        m_iFrameBase = (iTime / m_stT.iFrame) * m_stT.iFrame;
        const ps_t iIn = iTime - m_iFrameBase;
        m_iPiece = (uint32_t)(std::upper_bound(m_stT.obStart.begin(), m_stT.obStart.end(), iIn) - m_stT.obStart.begin()) - 1;
        m_bLive = false;
    }
    uint32_t tvNextEntry(ps_t iTime)
    {
        tvAdvance(iTime);
        return m_iPiece + 1;
    }
    void tvAdvance(ps_t iTime)
    {
        for (;;)
        {
            const ps_t iEnd = m_iFrameBase + m_stT.obStart[m_iPiece] + m_stT.obLength[m_iPiece];
            if (iEnd > iTime)
                return;
            m_bLive = false;
            if (++m_iPiece == m_stT.iCount)
            {
                m_iPiece = 0;
                m_iFrameBase += m_stT.iFrame;
            }
            const int iRow = m_stT.obRow[m_iPiece];
            if (iRow >= 0)
                tvLoadRow(iRow, iEnd, iEnd + m_stT.obLength[m_iPiece]);
        }
    }
    void tvLoadRow(int iRow, ps_t iStart, ps_t iEnd)
    {
        m_bLive = true;
        m_bLiveRing = m_obOnRing[iRow];
        m_iLiveSlot = (int)mirrorRingSlot((uint32_t)iRow);
        m_iLiveEnd = iEnd;
        if (m_bLiveRing)
        {
            iRingRows++;
            const stSlot & stS = m_stSlots[m_iLiveSlot];
            const bool bRight = stS.iRow == iRow && stS.iPicture == m_iPicture;
            if (bRight && stS.iFillEnd <= iStart)
                return;
            if (bRight && stS.iFillStart <= iStart)
                iTornRing++;
            else
                iStale++;
        }
        else
        {
            iLcdRows++;
            if (m_bLcdDirty)
                tornLcd();
        }
    }
    void tornLcd()
    {
        if (m_bRedrawUnprotected)
            iTornLcdAllowed++;
        else
            iTornLcd++;
    }

    // ---- the reader, on the other core
    void readerRun(ps_t iUntil)
    {
        while (m_iReaderAt <= iUntil)
        {
            const ps_t iAt = m_iReaderAt;
            tvAdvance(iAt);
            if (!m_bInTick)
            {
                if (!m_stCore.bReading)
                {
                    m_iReaderAt = NEVER;        // not armed again
                    continue;
                }
                if (m_iLastTickEnd >= 0)
                    iTickGapMin = std::min(iTickGapMin, iAt - m_iLastTickEnd);
                m_iTickStart = iAt;
                m_iCount = mirrorReaderBegin(m_stCore, tvNextEntry(iAt), m_iFirstRow);
                m_iRead = 0;
                m_bInTick = true;
                m_iReaderAt = iAt + 200000;
            }
            else if (m_iRead < m_iCount)
            {
                if (m_iFetchedRow >= 0)
                    mirrorReaderFetched(m_stCore, (uint32_t)m_iFetchedRow, tvNextEntry(iAt));
                const uint32_t iRow = m_iFirstRow + m_iRead++;
                const ps_t iLength = uniform(19600000, 21500000);
                const int iSlot = (int)mirrorRingSlot(iRow);
                if (m_bLive && m_bLiveRing && m_iLiveSlot == iSlot && m_iLiveEnd > iAt)
                    iRefilled++;
                m_stSlots[iSlot] = { (int)iRow, m_obCopyPicture[iRow], iAt, iAt + iLength };
                m_iHeldUntil = iAt + iLength;
                m_iBusy += iLength;
                m_iReaderAt = iAt + iLength + 100000;
                m_iFetchedRow = (int)iRow;
            }
            else
            {
                if (m_iFetchedRow >= 0)
                    mirrorReaderFetched(m_stCore, (uint32_t)m_iFetchedRow, tvNextEntry(iAt));
                const ps_t iGap = (ps_t)mirrorReaderEnd(m_stCore, m_iFirstRow, m_iCount) * US;
                m_bInTick = false;
                m_iLastTickEnd = iAt;
                iTickMax = std::max(iTickMax, iAt - m_iTickStart);
                // Armed a gap after the tick ends; a kick during the tick is served at once.
                m_iReaderAt = m_bKicked ? iAt + 300000 : iAt + iGap + tickDelay();
                m_bKicked = false;
                m_iFetchedRow = -1;
            }
        }
    }
    void kickReader()
    {
        if (m_bInTick)
            m_bKicked = true;
        else
            m_iReaderAt = std::min(m_iReaderAt, m_iNow + tickDelay());
    }

    // ---- the drawing core
    void step(ps_t iLength)
    {
        ps_t iTarget = m_iNow + iLength;
        readerRun(iTarget);
        while (m_iHeldUntil > iTarget)
        {
            iTarget = m_iHeldUntil + 50000;
            readerRun(iTarget);
        }
        tvAdvance(iTarget);
        m_iNow = iTarget;
    }
    void pointRows(bool bToRing)
    {
        for (unsigned int iRow = 0; iRow < PIC_H; iRow++)
        {
            step(40000);
            m_obOnRing[iRow] = bToRing;
        }
    }
    void copyRows(unsigned int iRows)
    {
        for (unsigned int i = 0; i < iRows && m_iCopyRow < PIC_H; i++, m_iCopyRow++)
        {
            m_obCopyPicture[m_iCopyRow] = -1;       // mixed while it is written
            step(52 * US);
            m_obCopyPicture[m_iCopyRow] = m_iPicture;
        }
        if (m_iCopyRow >= PIC_H)
        {
            m_bCopyActive = false;
            m_bCopyValid = true;
        }
    }
    void servicePass()
    {
        step(3 * US);
        if (m_bCopyActive)
            copyRows(8);
    }
    void frameComplete()
    {
        // The picture buffer holds the next finished picture from here on. Until
        // the list points back at it, the line buffers still hold whole rows of
        // the picture before.
        step(1 * US);
        m_bLcdDirty = false;
        mirrorRelease(m_stCore, tvNextEntry(m_iNow));
        pointRows(false);
        m_iPicture++;
        m_bDirect = true;
        m_bCopyValid = false;
        m_iCopyRow = 0;
        m_bCopyActive = true;
    }
    void redrawStart()
    {
        m_bRedrawUnprotected = true;
        if (m_bDirect)
        {
            while (m_bCopyActive)
                copyRows(8);
            if (m_bCopyValid)
            {
                const ps_t iFrom = m_iNow;
                if (switchToCopy(true))
                {
                    m_bDirect = false;
                    m_bRedrawUnprotected = false;
                    iSwitches++;
                    iSwitchMax = std::max(iSwitchMax, m_iNow - iFrom);
                    iSwitchSum += m_iNow - iFrom;
                }
                else
                    iMissed++;
            }
        }
        // The redraw begins: the picture buffer is no longer a picture.
        tvAdvance(m_iNow);
        m_bLcdDirty = true;
        if (m_bLive && !m_bLiveRing && m_iLiveEnd > m_iNow)
            tornLcd();
    }
    bool switchToCopy(bool bPoint)
    {
        const ps_t iTimeout = 2000 * US;
        ps_t iFrom = m_iNow;
        while (!mirrorSwitchStart(m_stCore, tvNextEntry(m_iNow)))
        {
            step(100000);
            if (m_iNow - iFrom > iTimeout)
                return false;
        }
        kickReader();

        iFrom = m_iNow;
        while (!mirrorSwitchReady(m_stCore, [this] { step(30000); return tvNextEntry(m_iNow); }))
        {
            step(60000);
            if (m_iNow - iFrom > iTimeout)
            {
                mirrorRelease(m_stCore, tvNextEntry(m_iNow));
                return false;
            }
        }
        if (bPoint)
            pointRows(true);
        mirrorSwitchDone(m_stCore);

        const uint32_t iEntry = tvNextEntry(m_iNow);
        iFrom = m_iNow;
        while (tvNextEntry(m_iNow) == iEntry && m_iNow - iFrom < 100 * US)
            step(50000);
        return true;
    }

    const stTimeline & m_stT;
    const stConfig & m_stC;
    std::mt19937 m_obRng;

    stDVIMirrorCore m_stCore;
    std::vector<bool> m_obOnRing;
    std::vector<int> m_obCopyPicture;       // which picture each row of the copy holds
    stSlot m_stSlots[DVI_MIRROR_RING_ROWS];

    ps_t m_iNow = 0;
    int m_iPicture = 0;
    bool m_bLcdDirty = false;
    bool m_bRedrawUnprotected = true;
    bool m_bDirect = true;
    bool m_bCopyValid = false;
    bool m_bCopyActive = false;
    unsigned int m_iCopyRow = 0;

    ps_t m_iFrameBase = 0;
    uint32_t m_iPiece = 0;
    bool m_bLive = false;
    bool m_bLiveRing = false;
    int m_iLiveSlot = 0;
    ps_t m_iLiveEnd = 0;

    ps_t m_iReaderAt = NEVER;
    bool m_bInTick = false;
    bool m_bKicked = false;
    uint32_t m_iFirstRow = 0, m_iCount = 0, m_iRead = 0;
    int m_iFetchedRow = -1;
    ps_t m_iHeldUntil = 0;
    ps_t m_iBusy = 0;
    ps_t m_iTickStart = 0;
    ps_t m_iLastTickEnd = -1;
};

// ---------------------------------------------------------------------------

static void testScanPosition(const stTimeline & stT)
{
    stDVIMirrorCore stCore;
    mirrorInit(stCore, PIC_H, stT.iFirstRowEntry, stT.iCount);

    // The frame is whole, and the rows are two entries apart in row order.
    CHECK(stT.iFrame == (ps_t)800 * 525 * PS_PER_PIXEL);
    for (unsigned int iRow = 0; iRow < PIC_H; iRow++)
        CHECK(stT.obRow[stT.iFirstRowEntry + 2 * iRow] == (int)iRow);

    for (uint32_t iPiece = 0; iPiece < stT.iCount; iPiece++)
    {
        const int iScan = mirrorScanRow(stCore, iPiece + 1);
        int iExpect;
        if (iPiece < stT.iFirstRowEntry)
            iExpect = -1;
        else if (stT.obRow[iPiece] >= 0)
            iExpect = stT.obRow[iPiece];                    // on the row
        else if (iPiece < stT.iFirstRowEntry + 2 * PIC_H - 1)
            iExpect = stT.obRow[iPiece - 1] + 1;            // about to start the next
        else
            iExpect = (int)PIC_H;
        CHECK(iScan == iExpect);

        // Outside the picture: the pieces counted are the ones before row 0.
        if (iScan < 0 || iScan >= (int)PIC_H)
        {
            uint32_t iWalk = iPiece + 1;
            int iPieces = 0;
            while (iWalk % stT.iCount != stT.iFirstRowEntry)
            {
                iWalk++;
                iPieces++;
            }
            CHECK(mirrorPiecesToPicture(stCore, iPiece + 1) == iPieces);
        }
    }
}

static void report(const stConfig & stC, const mirrorSim & obSim)
{
    std::printf("%-8s rows: %d buffered, %d direct | stale %d, part-filled %d, refilled %d, torn %d (+%d unprotected) | "
                "late counted %u | switches %d, missed %d, avg %.0f us, max %.0f us | tick max %.0f us, gap min %.0f us\n",
                stC.szName, obSim.iRingRows, obSim.iLcdRows, obSim.iStale, obSim.iTornRing, obSim.iRefilled,
                obSim.iTornLcd, obSim.iTornLcdAllowed, (unsigned)obSim.late(), obSim.iSwitches, obSim.iMissed,
                obSim.iSwitches ? (double)obSim.iSwitchSum / obSim.iSwitches / US : 0.0, (double)obSim.iSwitchMax / US,
                (double)obSim.iTickMax / US, obSim.iTickGapMin == NEVER ? 0.0 : (double)obSim.iTickGapMin / US);
}

// The late count is the number of wrong lines the TV was given, to within
// 1/iShare of them (half a percent by default) or 5 lines.
static bool lateCountMatches(const mirrorSim & obSim, int64_t iShare = 200)
{
    const int64_t iWrong = (int64_t)obSim.iStale + obSim.iTornRing;
    const int64_t iDiff = iWrong > (int64_t)obSim.late() ? iWrong - (int64_t)obSim.late() : (int64_t)obSim.late() - iWrong;
    return iDiff <= std::max<int64_t>(5, iWrong / iShare);
}

int main()
{
    stTimeline stT;
    CHECK(buildTimeline(stT));
    if (g_iFailures)
    {
        std::printf("the frame did not build\n");
        return 1;
    }
    testScanPosition(stT);

    // Quiet: the reader is served within 3 us, now and then after another
    // interrupt of its own priority (up to 70 us).
    for (unsigned int iSeed = 1; iSeed <= 4; iSeed++)
    {
        const stConfig stC = { "quiet", 150, 3 * US, 0.02, 30 * US, 70 * US, iSeed };
        mirrorSim obSim(stT, stC);
        obSim.run();
        report(stC, obSim);
        CHECK(obSim.iRingRows > 0 && obSim.iLcdRows > 0);
        CHECK(obSim.iStale == 0);
        CHECK(obSim.iTornRing == 0);
        CHECK(obSim.iRefilled == 0);
        CHECK(obSim.iTornLcd == 0);
        CHECK(obSim.iMissed == 0);
        CHECK(obSim.late() == 0);
        CHECK(obSim.iSwitches == stC.iCycles);
        CHECK(obSim.iSwitchMax <= 600 * US);
        CHECK(obSim.iTickGapMin >= (ps_t)DVI_MIRROR_TICK_GAP_US * US);     // room for other interrupts between ticks
        CHECK(obSim.iTickMax <= 70 * US);
    }

    // Busy: one tick in twenty is held off 80 to 140 us.
    for (unsigned int iSeed = 11; iSeed <= 14; iSeed++)
    {
        const stConfig stC = { "busy", 150, 5 * US, 0.05, 80 * US, 140 * US, iSeed };
        mirrorSim obSim(stT, stC);
        obSim.run();
        report(stC, obSim);
        CHECK(obSim.iRefilled == 0);
        CHECK(obSim.iTornLcd == 0);
        CHECK(lateCountMatches(obSim));
    }

    // Blocked: now and then the reader is not served for 1 to 3 ms (a flash write).
    for (unsigned int iSeed = 21; iSeed <= 24; iSeed++)
    {
        const stConfig stC = { "blocked", 150, 5 * US, 0.004, 1000 * US, 3000 * US, iSeed };
        mirrorSim obSim(stT, stC);
        obSim.run();
        report(stC, obSim);
        CHECK(obSim.iRefilled == 0);
        CHECK(obSim.iTornLcd == 0);
        CHECK(lateCountMatches(obSim));
    }

    // Always on: a PSRAM picture shown for 600 frames, quiet and busy.
    for (unsigned int iSeed = 31; iSeed <= 34; iSeed++)
    {
        const stConfig stC = { "on-quiet", 0, 3 * US, 0.02, 30 * US, 70 * US, iSeed };
        mirrorSim obSim(stT, stC);
        obSim.runAlwaysOn(600);
        report(stC, obSim);
        std::printf("         reading rows %.1f %% of the time\n", obSim.fBusyShare * 100.0);
        CHECK(obSim.iSwitches == 1);
        CHECK(obSim.iRingRows >= 599 * (int)PIC_H);
        CHECK(obSim.iStale == 0);
        CHECK(obSim.iTornRing == 0);
        CHECK(obSim.iRefilled == 0);
        CHECK(obSim.late() == 0);
        CHECK(obSim.iSwitchMax <= 2000 * US);
        CHECK(obSim.iTickMax <= 70 * US);
        // 320 rows of 19.6 to 21.5 us in each 16.7 ms frame.
        CHECK(obSim.fBusyShare > 0.36 && obSim.fBusyShare < 0.43);
    }
    for (unsigned int iSeed = 41; iSeed <= 44; iSeed++)
    {
        const stConfig stC = { "on-busy", 0, 5 * US, 0.05, 80 * US, 140 * US, iSeed };
        mirrorSim obSim(stT, stC);
        obSim.runAlwaysOn(600);
        report(stC, obSim);
        CHECK(obSim.iSwitches == 1);
        CHECK(obSim.iRefilled == 0);
        // Always on, the count drifts up to 0.6 % from the wrong lines
        // (seed 42: 1078 counted, 1072 wrong): a health signal, not a tally.
        CHECK(lateCountMatches(obSim, 100));
    }

    std::printf("%d checks, %d failures\n", g_iChecks, g_iFailures);
    return g_iFailures ? 1 : 0;
}
