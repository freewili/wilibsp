// Host tests for bsp/display/rpDVIFrame: the frame it builds for 640x480 at 60 Hz is
// decoded here the way the HSTX command expander consumes it, and checked pixel
// clock by pixel clock.
//
// The decoder follows one pixel per FIFO word for TMDS data, which is the
// format the driver programs for the LCD buffer's 16-bit pixels, sent once,
// or twice for a picture shown at 2x (two encoder shifts per word).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rpDVIFrame.h"
#include "rpDVIList.h"
#include "rpDVIPacket.h"
#include "rpDVIAudioCore.h"
#include "tvout_list.h"
#include "tvout_list_frame.h"

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

// ---------------------------------------------------------------------------
// A sink that records the pieces.
// ---------------------------------------------------------------------------

struct stPiece
{
    bool            bPixels;
    const uint32_t *pWords;
    const uint16_t *pPixels;
    unsigned int    iCount;
};

class recordingSink : public rpDVIFrameSink
{
public:
    std::vector<stPiece> obPieces;
    unsigned int iLimit = 0;        // 0 = no limit

    bool addWords(const uint32_t * pWords, unsigned int iCount) override
    {
        if (iLimit && obPieces.size() >= iLimit)
            return false;
        obPieces.push_back({ false, pWords, nullptr, iCount });
        return true;
    }
    bool addPixels(const uint16_t * pPixels, unsigned int iCount) override
    {
        if (iLimit && obPieces.size() >= iLimit)
            return false;
        obPieces.push_back({ true, nullptr, pPixels, iCount });
        return true;
    }
};

// ---------------------------------------------------------------------------
// The decoder: one sample per pixel clock.
// ---------------------------------------------------------------------------

struct stSample
{
    bool     bActive;       // TMDS data, not a control symbol
    bool     bHLevel;       // sync levels, valid when !bActive
    bool     bVLevel;
    uint32_t iData;         // the data word, valid when bActive
};

// Lane 0's control symbol back to its (vertical, horizontal) levels.
static bool syncLevels(uint32_t iWord, bool & bV, bool & bH)
{
    if (((iWord >> 10) & 0x3ffu) != 0x354u || ((iWord >> 20) & 0x3ffu) != 0x354u)
        return false;
    switch (iWord & 0x3ffu)
    {
        case 0x354u: bV = false; bH = false; return true;
        case 0x0abu: bV = false; bH = true;  return true;
        case 0x154u: bV = true;  bH = false; return true;
        case 0x2abu: bV = true;  bH = true;  return true;
        default:     return false;
    }
}

// The pieces as the FIFO word stream. False if a piece is empty.
static bool flatten(const std::vector<stPiece> & obPieces, std::vector<uint32_t> & obFifo)
{
    for (const stPiece & stP : obPieces)
    {
        if (stP.iCount == 0)
            return false;                           // no piece may be empty
        for (unsigned int i = 0; i < stP.iCount; i++)
        {
            if (stP.bPixels)
            {
                // A 16-bit DMA write reaches the FIFO in both halves of the word.
                uint32_t iPixel = stP.pPixels[i];
                obFifo.push_back((iPixel << 16) | iPixel);
            }
            else
            {
                obFifo.push_back(stP.pWords[i]);
            }
        }
    }
    return true;
}

// iEncShifts: TMDS symbols each data word of a TMDS command gives, the word
// rotated right by 16 bits after each one.
static bool decodeFifo(const std::vector<uint32_t> & obFifo, std::vector<stSample> & obSamples,
                       unsigned int iEncShifts = 1);

// Flattens the pieces into the FIFO word stream and runs the command expander.
static bool decode(const std::vector<stPiece> & obPieces, std::vector<stSample> & obSamples,
                   unsigned int iEncShifts = 1)
{
    std::vector<uint32_t> obFifo;
    if (!flatten(obPieces, obFifo))
        return false;
    return decodeFifo(obFifo, obSamples, iEncShifts);
}

static bool decodeFifo(const std::vector<uint32_t> & obFifo, std::vector<stSample> & obSamples,
                       unsigned int iEncShifts)
{
    size_t iPos = 0;
    while (iPos < obFifo.size())
    {
        uint32_t iCmd = obFifo[iPos++];
        uint32_t iOp = iCmd & ~0xfffu;
        unsigned int iCount = iCmd & 0xfffu;
        if (iCount == 0)
            return false;
        if (iOp == rpDVIFrame::CMD_RAW_REPEAT)
        {
            if (iPos >= obFifo.size())
                return false;
            bool bV = false, bH = false;
            if (!syncLevels(obFifo[iPos++], bV, bH))
                return false;
            for (unsigned int i = 0; i < iCount; i++)
                obSamples.push_back({ false, bH, bV, 0 });
        }
        else if (iOp == rpDVIFrame::CMD_TMDS_REPEAT)
        {
            if (iPos >= obFifo.size())
                return false;
            uint32_t iData = obFifo[iPos++];
            for (unsigned int i = 0; i < iCount; i++)
                obSamples.push_back({ true, false, false, iData });
        }
        else if (iOp == rpDVIFrame::CMD_TMDS)
        {
            if (iCount % iEncShifts != 0 || iPos + iCount / iEncShifts > obFifo.size())
                return false;
            for (unsigned int i = 0; i < iCount; i++)
            {
                const uint32_t iWord = obFifo[iPos + i / iEncShifts];
                const unsigned int iRot = 16u * (i % iEncShifts) % 32u;
                obSamples.push_back({ true, false, false, iRot ? (iWord >> iRot) | (iWord << (32u - iRot)) : iWord });
            }
            iPos += iCount / iEncShifts;
        }
        else
        {
            return false;
        }
    }
    return true;
}

// Sync pulses, blanking and the active area sit where the mode puts them.
static void checkTiming(const stDVIMode & stMode, const std::vector<stSample> & obSamples)
{
    const unsigned int iHTotal = stMode.iHFrontPorch + stMode.iHSync + stMode.iHBackPorch + stMode.iHActive;
    const unsigned int iVTotal = stMode.iVFrontPorch + stMode.iVSync + stMode.iVBackPorch + stMode.iVActive;
    CHECK(obSamples.size() == (size_t)iHTotal * iVTotal);
    if (obSamples.size() != (size_t)iHTotal * iVTotal)
        return;

    const unsigned int iHBlank = iHTotal - stMode.iHActive;
    const unsigned int iVBlank = iVTotal - stMode.iVActive;
    for (unsigned int iY = 0; iY < iVTotal; iY++)
    {
        const bool bVSync = iY >= stMode.iVFrontPorch && iY < (unsigned int)(stMode.iVFrontPorch + stMode.iVSync);
        for (unsigned int iX = 0; iX < iHTotal; iX++)
        {
            const stSample & stS = obSamples[(size_t)iY * iHTotal + iX];
            const bool bHSync = iX >= stMode.iHFrontPorch && iX < (unsigned int)(stMode.iHFrontPorch + stMode.iHSync);
            const bool bShouldBeActive = iY >= iVBlank && iX >= iHBlank;
            CHECK(stS.bActive == bShouldBeActive);
            if (!stS.bActive)
            {
                // Both pulses are low while active in this mode.
                CHECK(stS.bHLevel == (bHSync != stMode.bHSyncActiveLow));
                CHECK(stS.bVLevel == (bVSync != stMode.bVSyncActiveLow));
            }
        }
    }
}

static uint32_t activeData(const stDVIMode & stMode, const std::vector<stSample> & obSamples,
                           unsigned int iX, unsigned int iY)
{
    const unsigned int iHTotal = stMode.iHFrontPorch + stMode.iHSync + stMode.iHBackPorch + stMode.iHActive;
    const unsigned int iVBlank = stMode.iVFrontPorch + stMode.iVSync + stMode.iVBackPorch;
    const unsigned int iHBlank = iHTotal - stMode.iHActive;
    return obSamples[(size_t)(iVBlank + iY) * iHTotal + iHBlank + iX].iData;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void testColourWord()
{
    CHECK(rpDVIFrame::colourWord(0x0000) == 0x00000000u);
    CHECK(rpDVIFrame::colourWord(0xFFFF) == 0xFFFFFFFFu);
    CHECK(rpDVIFrame::colourWord(0xF800) == 0x00F800F8u);   // red: high byte first
    CHECK(rpDVIFrame::colourWord(0x001F) == 0x1F001F00u);   // blue
    CHECK(rpDVIFrame::colourWord(0x07E0) == 0xE007E007u);   // green
}

static void testBars()
{
    const stDVIMode & stMode = DVI_MODE_640x480_60;
    rpDVIFrame obFrame;
    recordingSink obSink;
    CHECK(obFrame.buildBars(stMode, obSink));
    CHECK(obSink.obPieces.size() == rpDVIFrame::barsPieceCount(stMode));
    CHECK(rpDVIFrame::barsPieceCount(stMode) == 525u);
    for (const stPiece & stP : obSink.obPieces)
        CHECK(!stP.bPixels);

    std::vector<stSample> obSamples;
    CHECK(decode(obSink.obPieces, obSamples));
    checkTiming(stMode, obSamples);
    if (obSamples.size() != 800u * 525u)
        return;

    static const uint16_t iBars[8] = { 0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000 };
    for (unsigned int iY = 0; iY < 480; iY += 53)
        for (unsigned int iX = 0; iX < 640; iX++)
            CHECK(activeData(stMode, obSamples, iX, iY) == rpDVIFrame::colourWord(iBars[iX / 80]));
}

static void testPicture(unsigned int iWidth, unsigned int iHeight)
{
    const stDVIMode & stMode = DVI_MODE_640x480_60;
    std::vector<uint16_t> obPixels((size_t)iWidth * iHeight);
    for (size_t i = 0; i < obPixels.size(); i++)
        obPixels[i] = (uint16_t)(i * 2654435761u >> 7);

    rpDVIFrame obFrame;
    recordingSink obSink;
    CHECK(obFrame.buildPicture(stMode, obPixels.data(), iWidth, iHeight, iWidth, obSink));
    CHECK(obSink.obPieces.size() == rpDVIFrame::picturePieceCount(stMode, iWidth, iHeight));

    unsigned int iPixelPieces = 0;
    for (const stPiece & stP : obSink.obPieces)
    {
        CHECK(stP.iCount != 0);
        if (stP.bPixels)
        {
            CHECK(stP.iCount == iWidth);
            CHECK(stP.pPixels == obPixels.data() + (size_t)iPixelPieces * iWidth);
            iPixelPieces++;
        }
    }
    CHECK(iPixelPieces == iHeight);

    std::vector<stSample> obSamples;
    CHECK(decode(obSink.obPieces, obSamples));
    checkTiming(stMode, obSamples);
    if (obSamples.size() != 800u * 525u)
        return;

    const unsigned int iLeft = (640 - iWidth) / 2;
    const unsigned int iTop = (480 - iHeight) / 2;
    for (unsigned int iY = 0; iY < 480; iY++)
    {
        for (unsigned int iX = 0; iX < 640; iX++)
        {
            const bool bInside = iX >= iLeft && iX < iLeft + iWidth && iY >= iTop && iY < iTop + iHeight;
            uint32_t iExpect = 0;
            if (bInside)
            {
                uint32_t iPixel = obPixels[(size_t)(iY - iTop) * iWidth + (iX - iLeft)];
                iExpect = (iPixel << 16) | iPixel;
            }
            CHECK(activeData(stMode, obSamples, iX, iY) == iExpect);
        }
    }
}

// A picture at 2x (tvoutListBuildPicture, scale 2): each frame row reads
// picture row r / 2, the full picture width, and with two encoder shifts per
// word every TV pixel is picture pixel (x / 2, y / 2), centred.
static void testPicture2x(unsigned int iWidth, unsigned int iHeight)
{
    const stDVIMode & stMode = DVI_MODE_640x480_60;
    std::vector<uint16_t> obPixels((size_t)iWidth * iHeight);
    for (size_t i = 0; i < obPixels.size(); i++)
        obPixels[i] = (uint16_t)(i * 2654435761u >> 9);

    rpDVIFrame obFrame;
    recordingSink obSink;
    CHECK(tvoutListBuildPicture(obFrame, obPixels.data(), iWidth, iHeight, 2u, obSink));
    CHECK(obSink.obPieces.size() == rpDVIFrame::picturePieceCount(stMode, 2u * iWidth, 2u * iHeight));

    unsigned int iRows = 0;
    for (const stPiece & stP : obSink.obPieces)
    {
        if (!stP.bPixels)
            continue;
        CHECK(stP.iCount == iWidth);
        CHECK(stP.pPixels == obPixels.data() + (size_t)(iRows / 2u) * iWidth);
        iRows++;
    }
    CHECK(iRows == 2u * iHeight);

    std::vector<stSample> obSamples;
    CHECK(decode(obSink.obPieces, obSamples, 2u));
    checkTiming(stMode, obSamples);
    if (obSamples.size() != 800u * 525u)
        return;

    const unsigned int iLeft = (640 - 2u * iWidth) / 2;
    const unsigned int iTop = (480 - 2u * iHeight) / 2;
    for (unsigned int iY = 0; iY < 480; iY++)
    {
        for (unsigned int iX = 0; iX < 640; iX++)
        {
            const bool bInside = iX >= iLeft && iX < iLeft + 2u * iWidth && iY >= iTop && iY < iTop + 2u * iHeight;
            uint32_t iExpect = 0;
            if (bInside)
            {
                uint32_t iPixel = obPixels[(size_t)((iY - iTop) / 2u) * iWidth + (iX - iLeft) / 2u];
                iExpect = (iPixel << 16) | iPixel;
            }
            CHECK(activeData(stMode, obSamples, iX, iY) == iExpect);
        }
    }

    // Scale 1 builds the picture as it is (an odd width cannot be centred);
    // no other scale builds.
    recordingSink obSame;
    CHECK(tvoutListBuildPicture(obFrame, obPixels.data(), iWidth, iHeight, 1u, obSame) == (iWidth % 2u == 0));
    CHECK(iWidth % 2u != 0 || obSame.obPieces.size() == rpDVIFrame::picturePieceCount(stMode, iWidth, iHeight));
    recordingSink obNone;
    CHECK(!tvoutListBuildPicture(obFrame, obPixels.data(), iWidth, iHeight, 3u, obNone));
}

static void testPieceCounts()
{
    const stDVIMode & stMode = DVI_MODE_640x480_60;
    // 45 blank lines, 160 border lines, one head, 320 rows, 319 joins, one tail.
    CHECK(rpDVIFrame::picturePieceCount(stMode, 480, 320) == 846u);
    // Full size: no border lines and no tail.
    CHECK(rpDVIFrame::picturePieceCount(stMode, 640, 480) == 45u + 1u + 480u + 479u);
    CHECK(rpDVIFrame::picturePieceCount(stMode, 0, 320) == 0u);
    CHECK(rpDVIFrame::picturePieceCount(stMode, 642, 320) == 0u);
    CHECK(rpDVIFrame::picturePieceCount(stMode, 480, 481) == 0u);
}

static void testRefusals()
{
    const stDVIMode & stMode = DVI_MODE_640x480_60;
    uint16_t iPixels[4] = {};
    rpDVIFrame obFrame;

    recordingSink obSink;
    CHECK(!obFrame.buildPicture(stMode, nullptr, 2, 2, 2, obSink));
    CHECK(!obFrame.buildPicture(stMode, iPixels, 0, 2, 2, obSink));
    CHECK(!obFrame.buildPicture(stMode, iPixels, 641, 1, 641, obSink));     // odd border and too wide
    CHECK(!obFrame.buildPicture(stMode, iPixels, 3, 1, 3, obSink));         // border not even
    CHECK(obSink.obPieces.empty());

    // A sink that runs out of room stops the build.
    recordingSink obFull;
    obFull.iLimit = 100;
    CHECK(!obFrame.buildBars(stMode, obFull));
    CHECK(obFull.obPieces.size() == 100u);

    stDVIMode stOdd = stMode;
    stOdd.iHActive = 636;                                                    // not eight equal bars
    recordingSink obOdd;
    CHECK(!obFrame.buildBars(stOdd, obOdd));
}

// ---------------------------------------------------------------------------
// The builder as it was before audio framing, kept as the reference for DVI
// framing: with audio framing off, the class must emit exactly these pieces.
// ---------------------------------------------------------------------------

namespace ref {

const uint32_t TMDS_CTRL[4] = { 0x354u, 0x0abu, 0x154u, 0x2abu };
const uint16_t BAR_COLOURS[8] = { 0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000 };

class frame
{
public:
    bool buildBars(const stDVIMode & stMode, rpDVIFrameSink & obSink)
    {
        if (stMode.iHActive % 8u != 0)
            return false;
        m_stMode = stMode;
        buildBlankLines();
        unsigned int iWords = putHSync(m_iLineBars, false);
        for (unsigned int iBar = 0; iBar < 8; iBar++)
        {
            m_iLineBars[iWords++] = rpDVIFrame::CMD_TMDS_REPEAT | (unsigned int)(stMode.iHActive / 8u);
            m_iLineBars[iWords++] = rpDVIFrame::colourWord(BAR_COLOURS[iBar]);
        }
        if (!addBlanking(obSink))
            return false;
        for (unsigned int iLine = 0; iLine < stMode.iVActive; iLine++)
            if (!obSink.addWords(m_iLineBars, iWords))
                return false;
        return true;
    }

    bool buildPicture(const stDVIMode & stMode, const uint16_t * pPixels,
        unsigned int iWidth, unsigned int iHeight, unsigned int iStridePixels, rpDVIFrameSink & obSink)
    {
        if (pPixels == nullptr || rpDVIFrame::picturePieceCount(stMode, iWidth, iHeight) == 0)
            return false;
        if ((stMode.iHActive - iWidth) % 2u != 0)
            return false;
        m_stMode = stMode;
        buildBlankLines();
        const unsigned int iBorderX = (stMode.iHActive - iWidth) / 2u;
        const unsigned int iBorderTop = (stMode.iVActive - iHeight) / 2u;
        const unsigned int iBorderBottom = stMode.iVActive - iHeight - iBorderTop;
        unsigned int iWords = putHSync(m_iLineBorder, false);
        m_iLineBorder[iWords++] = rpDVIFrame::CMD_TMDS_REPEAT | stMode.iHActive;
        m_iLineBorder[iWords++] = rpDVIFrame::colourWord(0x0000);
        m_iTailWords = 0;
        if (iBorderX != 0)
        {
            m_iLineTail[m_iTailWords++] = rpDVIFrame::CMD_TMDS_REPEAT | iBorderX;
            m_iLineTail[m_iTailWords++] = rpDVIFrame::colourWord(0x0000);
        }
        m_iHeadWords = putHSync(m_iLineHead, false);
        for (unsigned int iWord = 0; iWord < m_iTailWords; iWord++)
            m_iLineHead[m_iHeadWords++] = m_iLineTail[iWord];
        m_iLineHead[m_iHeadWords++] = rpDVIFrame::CMD_TMDS | iWidth;
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
            if (iRow + 1u < iHeight && !obSink.addWords(m_iLineTailHead, iJoinWords))
                return false;
        }
        if (m_iTailWords != 0 && !obSink.addWords(m_iLineTail, m_iTailWords))
            return false;
        for (unsigned int iLine = 0; iLine < iBorderBottom; iLine++)
            if (!obSink.addWords(m_iLineBorder, 8))
                return false;
        return true;
    }

private:
    uint32_t syncWord(bool bVSyncActive, bool bHSyncActive) const
    {
        unsigned int iVLevel = (bVSyncActive != m_stMode.bVSyncActiveLow) ? 1u : 0u;
        unsigned int iHLevel = (bHSyncActive != m_stMode.bHSyncActiveLow) ? 1u : 0u;
        return TMDS_CTRL[(iVLevel << 1) | iHLevel] | (TMDS_CTRL[0] << 10) | (TMDS_CTRL[0] << 20);
    }
    unsigned int putHSync(uint32_t * pWords, bool bVSyncActive) const
    {
        pWords[0] = rpDVIFrame::CMD_RAW_REPEAT | m_stMode.iHFrontPorch;
        pWords[1] = syncWord(bVSyncActive, false);
        pWords[2] = rpDVIFrame::CMD_RAW_REPEAT | m_stMode.iHSync;
        pWords[3] = syncWord(bVSyncActive, true);
        pWords[4] = rpDVIFrame::CMD_RAW_REPEAT | m_stMode.iHBackPorch;
        pWords[5] = syncWord(bVSyncActive, false);
        return 6;
    }
    void buildBlankLines()
    {
        putHSync(m_iLineVBlank, false);
        m_iLineVBlank[4] = rpDVIFrame::CMD_RAW_REPEAT | (unsigned int)(m_stMode.iHBackPorch + m_stMode.iHActive);
        putHSync(m_iLineVSync, true);
        m_iLineVSync[4] = rpDVIFrame::CMD_RAW_REPEAT | (unsigned int)(m_stMode.iHBackPorch + m_stMode.iHActive);
    }
    bool addBlanking(rpDVIFrameSink & obSink) const
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

    stDVIMode m_stMode = {};
    uint32_t m_iLineVBlank[6] = {};
    uint32_t m_iLineVSync[6] = {};
    uint32_t m_iLineBorder[8] = {};
    uint32_t m_iLineBars[22] = {};
    uint32_t m_iLineHead[9] = {};
    uint32_t m_iLineTailHead[11] = {};
    uint32_t m_iLineTail[2] = {};
    unsigned int m_iHeadWords = 0;
    unsigned int m_iTailWords = 0;
};

}

// Same pieces in the same order: kind, length, pixel address, and every word.
static void checkSamePieces(const recordingSink & obA, const recordingSink & obB)
{
    CHECK(obA.obPieces.size() == obB.obPieces.size());
    if (obA.obPieces.size() != obB.obPieces.size())
        return;
    for (size_t i = 0; i < obA.obPieces.size(); i++)
    {
        const stPiece & stA = obA.obPieces[i];
        const stPiece & stB = obB.obPieces[i];
        CHECK(stA.bPixels == stB.bPixels);
        CHECK(stA.iCount == stB.iCount);
        if (stA.bPixels != stB.bPixels || stA.iCount != stB.iCount)
            continue;
        if (stA.bPixels)
        {
            CHECK(stA.pPixels == stB.pPixels);
            continue;
        }
        bool bSame = true;
        for (unsigned int iWord = 0; iWord < stA.iCount; iWord++)
            bSame = bSame && stA.pWords[iWord] == stB.pWords[iWord];
        CHECK(bSame);
    }
}

// Audio framing off, the default and after an audio framing build, emits the reference
// pieces exactly.
static void testDVIFramingUnchanged()
{
    const stDVIMode & stMode = DVI_MODE_640x480_60;
    static const unsigned int iSizes[5][2] = { { 480, 320 }, { 640, 480 }, { 2, 1 }, { 638, 2 }, { 640, 1 } };
    std::vector<uint16_t> obPixels(640u * 480u, 0x1234);
    std::vector<uint32_t> obPool(4096);

    for (int iPass = 0; iPass < 2; iPass++)
    {
        rpDVIFrame obFrame;
        ref::frame obRef;
        if (iPass == 1)
        {
            // An audio framing build first, then back to DVI framing on the same object.
            recordingSink obAudioFraming;
            obFrame.setAudioFraming(obPool.data(), (unsigned int)obPool.size());
            CHECK(obFrame.buildPicture(stMode, obPixels.data(), 480, 320, 480, obAudioFraming));
            CHECK(obFrame.audioSlotCount() != 0);
            obFrame.setAudioFraming(nullptr, 0);
        }

        recordingSink obNew, obOld;
        CHECK(obFrame.buildBars(stMode, obNew));
        CHECK(obRef.buildBars(stMode, obOld));
        checkSamePieces(obNew, obOld);
        CHECK(obFrame.audioSlotCount() == 0);

        for (const auto & iSize : iSizes)
        {
            recordingSink obPicNew, obPicOld;
            CHECK(obFrame.buildPicture(stMode, obPixels.data(), iSize[0], iSize[1], 640, obPicNew));
            CHECK(obRef.buildPicture(stMode, obPixels.data(), iSize[0], iSize[1], 640, obPicOld));
            checkSamePieces(obPicNew, obPicOld);
            CHECK(obFrame.audioSlotCount() == 0);
            CHECK(obFrame.audioLineSlot(100) == -1);
        }
    }
}

// ---------------------------------------------------------------------------
// The scan-out list (rpDVIList) on a model of the DMA channels.
//
// The model runs the list the way the channels do: the list channel copies an
// entry into the pixel channel's alias 3 registers through a 16-byte write
// ring and its last copy starts the pixel channel; a channel that has done its
// transfers starts the channel its control word chains to; a start loads the
// transfer count from its reload value; the read address of a restarted
// channel runs on. One step is one transfer, read and write. The FIFO words
// the model writes must be the words of the frame's pieces in order, for two
// frames, and decode as above. After every step the scan position, read the way
// rpDVI reads it, must be one the plain list could show there.
// ---------------------------------------------------------------------------

namespace model {

const uint32_t CTRL_EN = 1u << 0;
const uint32_t CTRL_SIZE_LSB = 2;
const uint32_t CTRL_INCR_READ = 1u << 4;
const uint32_t CTRL_INCR_WRITE = 1u << 6;
const uint32_t CTRL_RING_SIZE_LSB = 8;
const uint32_t CTRL_RING_SEL = 1u << 12;
const uint32_t CTRL_CHAIN_LSB = 13;
const uint32_t CTRL_CHAIN_BITS = 0xfu << 13;
const uint32_t CTRL_TREQ_LSB = 17;
const uint32_t TREQ_HSTX = 52;
const uint32_t TREQ_FORCE = 63;
const uint32_t COUNT_BITS = 0x0fffffffu;

const uint32_t CH_PIXEL = 0, CH_LIST = 1, CH_REWIND = 2, CH_JOIN = 3, CH_ROW = 4, CH_RESTART = 5, CHANNELS = 6;
const uint32_t REG_BASE = 0x50000000u;
const uint32_t REG_FIFO = 0x50100000u;

inline uint32_t reg(uint32_t iChannel, uint32_t iOffset)
{
    return REG_BASE + 0x40u * iChannel + iOffset;
}

inline uint32_t ctrl(uint32_t iSizeLog2, bool bIncrRead, bool bIncrWrite, uint32_t iTreq, uint32_t iChain,
                     uint32_t iRingBits = 0, bool bRingWrite = false)
{
    return CTRL_EN | (iSizeLog2 << CTRL_SIZE_LSB) | (bIncrRead ? CTRL_INCR_READ : 0u) |
           (bIncrWrite ? CTRL_INCR_WRITE : 0u) | (iRingBits << CTRL_RING_SIZE_LSB) |
           (bRingWrite ? CTRL_RING_SEL : 0u) | (iChain << CTRL_CHAIN_LSB) | (iTreq << CTRL_TREQ_LSB);
}

// Host memory under 32-bit bus addresses.
class bus
{
public:
    uint32_t add(const void * p, size_t iBytes)
    {
        const stRegion st = { (uint8_t *)p, iBytes, m_iNext };
        m_obRegions.push_back(st);
        m_iNext += (uint32_t)((iBytes + 0xffu) & ~(size_t)0xffu) + 0x100u;
        return st.iBase;
    }
    uint32_t to(const void * p) const
    {
        const uint8_t * pByte = (const uint8_t *)p;
        for (const stRegion & st : m_obRegions)
            if (pByte >= st.p && pByte < st.p + st.iBytes)
                return st.iBase + (uint32_t)(pByte - st.p);
        return 0;
    }
    uint8_t * at(uint32_t iAddr, unsigned int iBytes) const
    {
        for (const stRegion & st : m_obRegions)
            if (iAddr >= st.iBase && iAddr + iBytes <= st.iBase + st.iBytes)
                return st.p + (iAddr - st.iBase);
        return nullptr;
    }

private:
    struct stRegion
    {
        uint8_t * p;
        size_t iBytes;
        uint32_t iBase;
    };
    std::vector<stRegion> m_obRegions;
    uint32_t m_iNext = 0x20000000u;
};

class list : public rpDVIList
{
public:
    const bus * pBus = nullptr;
protected:
    uint32_t busAddress(const void * p) const override { return pBus->to(p); }
};

struct stChannel
{
    uint32_t iCtrl = 0;
    uint32_t iRead = 0;
    uint32_t iWrite = 0;
    uint32_t iCount = 0;
    uint32_t iReload = 0;
    bool bBusy = false;
};

// What the reader can see after a step.
struct stState
{
    uint32_t iListRead;
    uint32_t iListCount;
    uint32_t iPixelCount;
    uint32_t iRowRead;
    uint32_t iRowCount;
    size_t iWords;              // FIFO words written so far
};

class dma
{
public:
    explicit dma(const bus & obBus) : m_obBus(obBus) {}

    stChannel ch[CHANNELS];
    std::vector<uint32_t> obFifo;
    bool bFault = false;
    // Each list or row channel read of a word in a watched range: the FIFO
    // words written by then.
    uint32_t iWatchFrom = 0, iWatchTo = 0, iWatchRowsFrom = 0, iWatchRowsTo = 0;
    std::vector<std::pair<uint32_t, size_t>> obWatched;

    void trigger(uint32_t iChannel)
    {
        stChannel & stC = ch[iChannel];
        if (stC.bBusy || !(stC.iCtrl & CTRL_EN) || stC.iReload == 0)
        {
            bFault = true;
            return;
        }
        stC.iCount = stC.iReload;
        stC.bBusy = true;
    }

    // One transfer of the busy channel. False when none is busy.
    bool step()
    {
        int iBusy = -1;
        for (uint32_t i = 0; i < CHANNELS; i++)
            if (ch[i].bBusy)
            {
                if (iBusy >= 0)
                    bFault = true;                  // the chain runs one channel at a time
                iBusy = (int)i;
            }
        if (iBusy < 0 || bFault)
            return false;
        stChannel & stC = ch[iBusy];
        const unsigned int iSize = 1u << ((stC.iCtrl >> CTRL_SIZE_LSB) & 3u);
        const uint8_t * pSrc = m_obBus.at(stC.iRead, iSize);
        if (pSrc == nullptr)
        {
            bFault = true;
            return false;
        }
        uint32_t iValue = 0;
        std::memcpy(&iValue, pSrc, iSize);
        if ((iBusy == (int)CH_LIST && stC.iRead >= iWatchFrom && stC.iRead < iWatchTo) ||
            (iBusy == (int)CH_ROW && stC.iRead >= iWatchRowsFrom && stC.iRead < iWatchRowsTo))
            obWatched.push_back({ stC.iRead, obFifo.size() });
        if (stC.iWrite == REG_FIFO)
            obFifo.push_back(iSize == 2 ? ((iValue << 16) | iValue) : iValue);
        else if (iSize == 4 && stC.iWrite >= REG_BASE && stC.iWrite < reg(CHANNELS, 0))
            writeRegister(stC.iWrite, iValue);
        else
            bFault = true;
        if (stC.iCtrl & CTRL_INCR_READ)
            stC.iRead += iSize;
        if (stC.iCtrl & CTRL_INCR_WRITE)
        {
            const uint32_t iRing = (stC.iCtrl >> CTRL_RING_SIZE_LSB) & 0xfu;
            const uint32_t iNext = stC.iWrite + iSize;
            if (iRing != 0 && (stC.iCtrl & CTRL_RING_SEL))
                stC.iWrite = (stC.iWrite & ~((1u << iRing) - 1u)) | (iNext & ((1u << iRing) - 1u));
            else
                stC.iWrite = iNext;
        }
        if (--stC.iCount == 0)
        {
            stC.bBusy = false;
            const uint32_t iChain = (stC.iCtrl & CTRL_CHAIN_BITS) >> CTRL_CHAIN_LSB;
            if (iChain != (uint32_t)iBusy)
                trigger(iChain);
        }
        return !bFault;
    }

    stState state() const
    {
        return { ch[CH_LIST].iRead, ch[CH_LIST].iCount, ch[CH_PIXEL].iCount, ch[CH_ROW].iRead, ch[CH_ROW].iCount, obFifo.size() };
    }

private:
    void writeRegister(uint32_t iAddr, uint32_t iValue)
    {
        const uint32_t iChannel = (iAddr - REG_BASE) / 0x40u;
        stChannel & stC = ch[iChannel];
        switch ((iAddr - REG_BASE) % 0x40u)
        {
            case 0x00: case 0x14: case 0x28: stC.iRead = iValue; break;
            case 0x04: case 0x18: case 0x34: stC.iWrite = iValue; break;
            case 0x08: case 0x24: case 0x38: stC.iReload = iValue & COUNT_BITS; break;
            case 0x10: case 0x20: case 0x30: stC.iCtrl = iValue; break;
            case 0x0c: stC.iCtrl = iValue; trigger(iChannel); break;
            case 0x1c: stC.iReload = iValue & COUNT_BITS; trigger(iChannel); break;
            case 0x2c: stC.iWrite = iValue; trigger(iChannel); break;
            case 0x3c: stC.iRead = iValue; trigger(iChannel); break;
            default: bFault = true; break;
        }
    }

    const bus & m_obBus;
};

}

static unsigned int g_iScanReads = 0;       // scan positions taken and checked
static unsigned int g_iScanRetries = 0;     // reads rpDVI would repeat

// The scan positions the plain list can show once iWords FIFO words are
// written: inside a piece, the piece after it; between two pieces, any from
// the one after the earlier to the one after the later; between two frames,
// the end, the rewind (0) or the first piece.
static bool scanAllowed(const std::vector<uint32_t> & obPieceOf, size_t iWords, uint32_t iValue)
{
    const size_t iTotal = obPieceOf.size();
    const size_t iAt = iWords % iTotal;
    const uint32_t iPieces = obPieceOf.back() + 1u;
    if (iAt == 0)
        return iValue == iPieces || iValue == 0 || iValue == 1u;
    const uint32_t iBefore = obPieceOf[iAt - 1u];
    const uint32_t iNext = obPieceOf[iAt];
    return iValue >= iBefore + 1u && iValue <= iNext + 1u;
}

// One frame through rpDVIList and the model. pPixels == nullptr: the bars.
static void testScanOut(const char * szName, bool bAudioFraming, const uint16_t * pPixels,
                        unsigned int iWidth, unsigned int iHeight)
{
    const stDVIMode & stMode = DVI_MODE_640x480_60;
    const bool bPicture = pPixels != nullptr;
    using namespace model;

    // The frame's pieces, from the same kind of build: the reference.
    std::vector<uint32_t> obPool(bAudioFraming ? (bPicture ? rpDVIFrame::audioFramingPictureWords(stMode, iWidth, iHeight)
                                                           : rpDVIFrame::audioFramingBarsWords(stMode)) : 0u);
    rpDVIFrame obFrame;
    if (bAudioFraming)
    {
        CHECK(!obPool.empty());
        obFrame.setAudioFraming(obPool.data(), (unsigned int)obPool.size());
    }
    recordingSink obRef;
    CHECK(bPicture ? obFrame.buildPicture(stMode, pPixels, iWidth, iHeight, iWidth, obRef) : obFrame.buildBars(stMode, obRef));
    std::vector<uint32_t> obRefWords;
    CHECK(flatten(obRef.obPieces, obRefWords));
    std::vector<uint32_t> obPieceOf;
    std::vector<size_t> obPieceStart;
    for (size_t i = 0; i < obRef.obPieces.size(); i++)
    {
        obPieceStart.push_back(obPieceOf.size());
        for (unsigned int iWord = 0; iWord < obRef.obPieces[i].iCount; iWord++)
            obPieceOf.push_back((uint32_t)i);
    }

    // The list, as rpDVI builds it.
    const unsigned int iEntries = rpDVIList::entryCount(stMode, bPicture, iWidth, iHeight, bAudioFraming);
    CHECK(iEntries != 0);
    std::vector<stDVIListEntry> obEntries(iEntries + 1u);
    std::vector<stDVIScanRun> obRuns(rpDVIList::SCAN_RUNS);
    const unsigned int iRowEntries = rpDVIList::rowEntryCount(bPicture, iHeight);
    std::vector<stDVIRowEntry> obRows(iRowEntries + 1u);
    std::vector<uint16_t> obOther(bPicture ? (size_t)iWidth * iHeight : 1u, 0x5a5a);
    uint32_t iListStart = 0;

    bus obBus;
    list obList;
    obList.pBus = &obBus;
    obBus.add(&obFrame, sizeof(obFrame));
    obBus.add(&obList, sizeof(obList));
    if (!obPool.empty())
        obBus.add(obPool.data(), obPool.size() * 4u);
    if (bPicture)
        obBus.add(pPixels, (size_t)iWidth * iHeight * 2u);
    obBus.add(obOther.data(), obOther.size() * 2u);
    const uint32_t iListBus = obBus.add(obEntries.data(), obEntries.size() * sizeof(stDVIListEntry));
    obBus.add(&iListStart, 4);
    iListStart = iListBus;
    const uint32_t iRowsBus = obBus.add(obRows.data(), obRows.size() * sizeof(stDVIRowEntry));

    rpDVIList::stConfig stConfig;
    stConfig.iCtrlWords = ctrl(2, true, false, TREQ_HSTX, CH_LIST);
    stConfig.iCtrlPixels = ctrl(1, true, false, TREQ_HSTX, CH_LIST);
    stConfig.iCtrlCopy = ctrl(2, false, false, TREQ_FORCE, CH_LIST);
    stConfig.iChainBits = CTRL_CHAIN_BITS;
    stConfig.iChainShift = CTRL_CHAIN_LSB;
    stConfig.iJoinChannel = CH_JOIN;
    stConfig.iRestartChannel = CH_RESTART;
    stConfig.iRewindChannel = CH_REWIND;
    stConfig.iFifo = REG_FIFO;
    stConfig.iRowReadAddr = reg(CH_ROW, 0x00);
    const bool bCompact = rpDVIList::isCompact(bPicture);
    obList.begin(obEntries.data(), iEntries, obRuns.data(), obRows.data(), iRowEntries, stConfig, bCompact);
    rpDVIFrame obListFrame;
    if (bAudioFraming)
        obListFrame.setAudioFraming(obPool.data(), (unsigned int)obPool.size());
    obBus.add(&obListFrame, sizeof(obListFrame));
    CHECK(bPicture ? obListFrame.buildPicture(stMode, pPixels, iWidth, iHeight, iWidth, obList) : obListFrame.buildBars(stMode, obList));
    CHECK(obList.finish());
    CHECK(obList.count() == iEntries);
    CHECK(obList.compact() == bCompact);
    CHECK(obList.scanCount() == obRef.obPieces.size());
    if (obList.count() != iEntries || obList.scanCount() != obRef.obPieces.size())
        return;

    // Rows: the word rpDVI hands out for each row reads that row.
    if (bPicture)
    {
        CHECK(obList.rows() == iHeight);
        for (unsigned int iRow = 0; iRow < iHeight; iRow++)
            CHECK(*obList.rowReadAddress(iRow) == obBus.to(pPixels + (size_t)iRow * iWidth));
        CHECK(obRef.obPieces[obList.scanFirstRow()].bPixels);
        CHECK(obList.scanFirstRow() == 0 || !obRef.obPieces[obList.scanFirstRow() - 1u].bPixels);
        // DVI framing: the joins are one slot, the piece after the first row.
        if (!bAudioFraming)
        {
            CHECK(obList.joins() == (iHeight > 1u));
            if (iHeight > 1u)
            {
                const stPiece & stJoin = obRef.obPieces[obList.scanFirstRow() + 1u];
                const uint8_t * pSlot = obBus.at(obList.joinStart(), stJoin.iCount * 4u);
                CHECK(obList.joinWords() == stJoin.iCount && pSlot != nullptr &&
                      std::memcmp(pSlot, stJoin.pWords, stJoin.iCount * 4u) == 0);
            }
        }
    }

    // The channels as rpDVI sets them up.
    dma obDMA(obBus);
    stChannel & stList = obDMA.ch[CH_LIST];
    stList.iCtrl = ctrl(2, true, true, TREQ_FORCE, CH_LIST, 4, true);
    stList.iRead = iListBus;
    stList.iWrite = reg(CH_PIXEL, 0x30);
    stList.iReload = 4;
    stChannel & stRewind = obDMA.ch[CH_REWIND];
    stRewind.iCtrl = ctrl(2, false, false, TREQ_FORCE, CH_REWIND);
    stRewind.iRead = obBus.to(&iListStart);
    stRewind.iWrite = reg(CH_LIST, 0x3c);
    stRewind.iReload = 1;
    if (obList.joins())
    {
        CHECK(obList.rowStart() == iRowsBus);
        stChannel & stJoin = obDMA.ch[CH_JOIN];
        stJoin.iCtrl = ctrl(2, true, false, TREQ_HSTX, CH_ROW);
        stJoin.iRead = obList.joinStart();
        stJoin.iWrite = REG_FIFO;
        stJoin.iReload = obList.joinWords();
        stChannel & stRow = obDMA.ch[CH_ROW];
        stRow.iCtrl = ctrl(2, true, true, TREQ_FORCE, CH_PIXEL, 3, true);
        stRow.iRead = obList.rowStart();
        stRow.iWrite = reg(CH_PIXEL, 0x10);
        stRow.iReload = 2;
        stChannel & stRestart = obDMA.ch[CH_RESTART];
        stRestart.iCtrl = ctrl(2, false, false, TREQ_FORCE, CH_RESTART);
        stRestart.iRead = obBus.to(obList.joinStartWord());
        stRestart.iWrite = reg(CH_JOIN, 0x3c);
        stRestart.iReload = 1;
    }
    obDMA.iWatchFrom = iListBus;
    obDMA.iWatchTo = iListBus + iEntries * sizeof(stDVIListEntry);
    obDMA.iWatchRowsFrom = iRowsBus;
    obDMA.iWatchRowsTo = iRowsBus + iRowEntries * sizeof(stDVIRowEntry);
    obDMA.trigger(CH_LIST);

    // Two frames, the rows of the second pointed at another buffer between them.
    const size_t iTotal = obRefWords.size();
    std::vector<stState> obStates;
    obStates.push_back(obDMA.state());
    bool bRepointed = false;
    while (obDMA.obFifo.size() < 2u * iTotal && obDMA.step())
    {
        obStates.push_back(obDMA.state());
        if (bPicture && !bRepointed && obDMA.obFifo.size() == iTotal)
        {
            bRepointed = true;
            for (unsigned int iRow = 0; iRow < iHeight; iRow++)
                *obList.rowReadAddress(iRow) = obBus.to(obOther.data() + (size_t)iRow * iWidth);
        }
    }
    CHECK(!obDMA.bFault);
    CHECK(obDMA.obFifo.size() == 2u * iTotal);
    if (obDMA.bFault || obDMA.obFifo.size() != 2u * iTotal)
        return;

    // The FIFO words: the pieces in order; the second frame's rows from the other buffer.
    bool bSame = true;
    for (size_t i = 0; i < iTotal; i++)
        bSame = bSame && obDMA.obFifo[i] == obRefWords[i];
    CHECK(bSame);
    bool bSecond = true;
    for (size_t i = 0; i < iTotal; i++)
    {
        const stPiece & stP = obRef.obPieces[obPieceOf[i]];
        uint32_t iExpect = obRefWords[i];
        if (stP.bPixels)
        {
            const uint32_t iPixel = obOther[(size_t)(stP.pPixels - pPixels) + (i - obPieceStart[obPieceOf[i]])];
            iExpect = (iPixel << 16) | iPixel;
        }
        bSecond = bSecond && obDMA.obFifo[iTotal + i] == iExpect;
    }
    CHECK(bSecond);

    // A row's read address is read just before its pixels: the FIFO holds
    // every word before the row and none of it, as with one entry per piece.
    if (bPicture)
    {
        unsigned int iRowReads = 0;
        for (const auto & stW : obDMA.obWatched)
        {
            if (stW.second >= iTotal)
                break;
            for (unsigned int iRow = 0; iRow < iHeight; iRow++)
            {
                const uint32_t iField = obBus.to(obList.rowReadAddress(iRow));
                if (stW.first != iField)
                    continue;
                const size_t iRowPiece = obList.scanFirstRow() + (iHeight > 1 ? 2u * iRow : 0u);
                CHECK(stW.second == obPieceStart[iRowPiece]);
                iRowReads++;
            }
        }
        CHECK(iRowReads == iHeight);
    }

    // Page flips (tvout_list_flip_window, DVI framing): wherever a scan
    // position read in one step opens the window, the frame being sent has
    // read the address of every row or of none.
    if (bPicture && !bAudioFraming)
    {
        std::vector<size_t> obRowWords;
        for (const auto & stW : obDMA.obWatched)
            if (stW.second < iTotal)
                for (unsigned int iRow = 0; iRow < iHeight; iRow++)
                    if (stW.first == obBus.to(obList.rowReadAddress(iRow)))
                        obRowWords.push_back(stW.second);
        CHECK(obRowWords.size() == iHeight);
        unsigned int iOpen = 0, iMixed = 0;
        for (const stState & stA : obStates)
        {
            if (stA.iWords >= iTotal || stA.iListCount != 0 || stA.iRowCount != 0)
                continue;
            const uint32_t iValue = obList.scanPiece((stA.iListRead - iListBus) / (uint32_t)sizeof(stDVIListEntry),
                                                     stA.iPixelCount & COUNT_BITS,
                                                     (stA.iRowRead - iRowsBus) / (uint32_t)sizeof(stDVIRowEntry));
            if (!tvout_list_flip_window(iValue, obList.scanFirstRow(), obList.rows()))
                continue;
            iOpen++;
            size_t iBefore = 0, iAtOrBefore = 0;
            for (size_t iWords : obRowWords)
            {
                iBefore += iWords < stA.iWords ? 1u : 0u;
                iAtOrBefore += iWords <= stA.iWords ? 1u : 0u;
            }
            if (iBefore != obRowWords.size() && iAtOrBefore != 0)
                iMixed++;
        }
        CHECK(iOpen > 0);
        CHECK(iMixed == 0);
    }

    // The FIFO stream decoded clock by clock.
    std::vector<uint32_t> obFrame1(obDMA.obFifo.begin(), obDMA.obFifo.begin() + (long)iTotal);
    if (!bAudioFraming)
    {
        std::vector<stSample> obSamples;
        CHECK(decodeFifo(obFrame1, obSamples));
        checkTiming(stMode, obSamples);
        if (bPicture && obSamples.size() == 800u * 525u)
        {
            const unsigned int iLeft = (640 - iWidth) / 2;
            const unsigned int iTop = (480 - iHeight) / 2;
            bool bPixelsOk = true;
            for (unsigned int iY = 0; iY < 480; iY++)
                for (unsigned int iX = 0; iX < 640; iX++)
                {
                    const bool bInside = iX >= iLeft && iX < iLeft + iWidth && iY >= iTop && iY < iTop + iHeight;
                    const uint32_t iPixel = bInside ? pPixels[(size_t)(iY - iTop) * iWidth + (iX - iLeft)] : 0u;
                    bPixelsOk = bPixelsOk && activeData(stMode, obSamples, iX, iY) == ((iPixel << 16) | iPixel);
                }
            CHECK(bPixelsOk);
        }
    }
    else
    {
        // Audio framing: every clock's symbol word or TMDS data, against the
        // pieces' own; TMDS data exactly over the active area.
        struct stClock { bool bTMDS; uint32_t iWord; };
        auto clocks = [](const std::vector<uint32_t> & obWords, std::vector<stClock> & obOut) -> bool
        {
            size_t iPos = 0;
            while (iPos < obWords.size())
            {
                const uint32_t iCmd = obWords[iPos++];
                const uint32_t iOp = iCmd & ~0xfffu;
                const unsigned int iCount = iCmd & 0xfffu;
                if (iCount == 0)
                    return false;
                const bool bTMDS = iOp == rpDVIFrame::CMD_TMDS || iOp == rpDVIFrame::CMD_TMDS_REPEAT;
                if (iOp == rpDVIFrame::CMD_RAW_REPEAT || iOp == rpDVIFrame::CMD_TMDS_REPEAT)
                {
                    if (iPos >= obWords.size())
                        return false;
                    const uint32_t iWord = obWords[iPos++];
                    for (unsigned int i = 0; i < iCount; i++)
                        obOut.push_back({ bTMDS, iWord });
                }
                else if (iOp == rpDVIFrame::CMD_RAW || iOp == rpDVIFrame::CMD_TMDS)
                {
                    if (iPos + iCount > obWords.size())
                        return false;
                    for (unsigned int i = 0; i < iCount; i++)
                        obOut.push_back({ bTMDS, obWords[iPos++] });
                }
                else
                    return false;
            }
            return true;
        };
        std::vector<stClock> obClocks, obRefClocks;
        CHECK(clocks(obFrame1, obClocks));
        CHECK(clocks(obRefWords, obRefClocks));
        CHECK(obClocks.size() == 800u * 525u);
        CHECK(obClocks.size() == obRefClocks.size());
        if (obClocks.size() == 800u * 525u && obRefClocks.size() == obClocks.size())
        {
            bool bClocksOk = true;
            for (size_t i = 0; i < obClocks.size(); i++)
            {
                const unsigned int iY = (unsigned int)(i / 800u);
                const unsigned int iX = (unsigned int)(i % 800u);
                bClocksOk = bClocksOk && obClocks[i].bTMDS == (iY >= 45u && iX >= 160u) &&
                            obClocks[i].bTMDS == obRefClocks[i].bTMDS && obClocks[i].iWord == obRefClocks[i].iWord;
            }
            CHECK(bClocksOk);
        }
    }

    // The scan position after every step, its registers read as rpDVI reads
    // them, a step or two apart.
    unsigned int iBad = 0, iReads = 0, iRetries = 0;
    for (size_t s = 0; s < obStates.size(); s++)
    {
        const stState & stA = obStates[s];
        if (!bCompact)
        {
            const uint32_t iValue = (stA.iListRead - iListBus) / (uint32_t)sizeof(stDVIListEntry);
            iReads++;
            if (!scanAllowed(obPieceOf, stA.iWords, iValue))
                iBad++;
            continue;
        }
        for (size_t d1 = 0; d1 <= 2 && s + d1 < obStates.size(); d1++)
            for (size_t d2 = d1; d2 <= d1 + 2 && s + d2 < obStates.size(); d2++)
            {
                const stState & stC = obStates[s + d1];
                const stState & stB = obStates[s + d2];
                const uint32_t iLoaded = (stA.iListRead - iListBus) / (uint32_t)sizeof(stDVIListEntry);
                const bool bRowsIdle = obList.rowsLoadedAt() == 0 || iLoaded != obList.rowsLoadedAt() ||
                                       (stA.iRowCount == 0 && stB.iRowCount == 0 && stA.iRowRead == stB.iRowRead);
                if (stA.iListCount != 0 || stB.iListCount != 0 || stA.iListRead != stB.iListRead || !bRowsIdle)
                {
                    iRetries++;
                    continue;
                }
                const uint32_t iValue = obList.scanPiece(iLoaded, stC.iPixelCount & COUNT_BITS,
                                                         (stA.iRowRead - iRowsBus) / (uint32_t)sizeof(stDVIRowEntry));
                bool bOk = false;
                for (size_t t = s; t <= s + d2; t++)
                    bOk = bOk || scanAllowed(obPieceOf, obStates[t].iWords, iValue);
                iReads++;
                if (!bOk)
                {
                    if (iBad < 5)
                        std::printf("  %s: step %zu (+%zu,+%zu) words %zu read %u\n", szName, s, d1, d2, stA.iWords, iValue);
                    iBad++;
                }
            }
        // A read in one step, the list and row channels idle: never repeated.
        if (stA.iListCount == 0 && stA.iRowCount == 0)
        {
            const uint32_t iValue = obList.scanPiece((stA.iListRead - iListBus) / (uint32_t)sizeof(stDVIListEntry),
                                                     stA.iPixelCount & COUNT_BITS,
                                                     (stA.iRowRead - iRowsBus) / (uint32_t)sizeof(stDVIRowEntry));
            CHECK(scanAllowed(obPieceOf, stA.iWords, iValue));
        }
    }
    CHECK(iBad == 0);
    g_iScanReads += iReads;
    g_iScanRetries += iRetries;
    std::printf("scan-out %-24s entries %4u (pieces %4u) runs %2u words %7zu steps %7zu reads %8u\n",
                szName, obList.count(), obList.scanCount(), bCompact ? 1u : 0u, iTotal, obStates.size(), iReads);
}

static void testScanOuts()
{
    std::vector<uint16_t> obPixels(640u * 480u);
    for (size_t i = 0; i < obPixels.size(); i++)
        obPixels[i] = (uint16_t)(i * 2654435761u >> 11);
    static const unsigned int iSizes[][2] = {
        { 480, 320 }, { 640, 480 }, { 2, 1 }, { 638, 2 }, { 640, 1 }, { 480, 9 }, { 480, 17 }, { 480, 5 }, { 320, 240 } };
    char szName[40];
    for (int iFraming = 0; iFraming < 2; iFraming++)
    {
        std::snprintf(szName, sizeof(szName), "%s bars", iFraming ? "audio" : "dvi");
        testScanOut(szName, iFraming != 0, nullptr, 0, 0);
        for (const auto & iSize : iSizes)
        {
            std::snprintf(szName, sizeof(szName), "%s %ux%u", iFraming ? "audio" : "dvi", iSize[0], iSize[1]);
            testScanOut(szName, iFraming != 0, obPixels.data(), iSize[0], iSize[1]);
        }
    }

    // Heap bytes of the frames TV output starts: list, scan runs, pool.
    const stDVIMode & stMode = DVI_MODE_640x480_60;
    CHECK(rpDVIList::frameBytes(stMode, false, 0, 0, false) == 525u * 16u);
    // Blanking 45, borders 80 + 80, head, the row channel's rewind, the first
    // row, tail; runs; 319 row entries.
    CHECK(rpDVIList::entryCount(stMode, true, 480, 320, false) == 45u + 160u + 1u + 1u + 1u + 1u);
    CHECK(rpDVIList::frameBytes(stMode, true, 480, 320, false) == 209u * 16u + 16u * 8u + 319u * 8u);
    CHECK(rpDVIList::frameBytes(stMode, false, 0, 0, true) == 525u * 16u + 4u * (470u + 8u * 65u));
    // Blanking 14 (front porch blocks 3, sync 2, fixed lines 1, back porch blocks
    // 8), borders 10 + 10, head, the row channel's rewind, the first row, tail;
    // runs; 319 row entries; pool.
    CHECK(rpDVIList::entryCount(stMode, true, 480, 320, true) == 14u + 10u + 1u + 1u + 1u + 1u + 10u);
    CHECK(rpDVIList::frameBytes(stMode, true, 480, 320, true) ==
          38u * 16u + 16u * 8u + 319u * 8u + 4u * (470u + 8u * 54u + 8u * 51u));
    // The picture without audio framing takes no more than with it.
    CHECK(rpDVIList::frameBytes(stMode, true, 480, 320, false) <= rpDVIList::frameBytes(stMode, true, 480, 320, true));
    std::printf("heap: dvi bars %u, dvi 480x320 %u, audio bars %u, audio 480x320 %u\n",
                rpDVIList::frameBytes(stMode, false, 0, 0, false), rpDVIList::frameBytes(stMode, true, 480, 320, false),
                rpDVIList::frameBytes(stMode, false, 0, 0, true), rpDVIList::frameBytes(stMode, true, 480, 320, true));
}


// ---------------------------------------------------------------------------
// The sound feeder (rpDVIAudioCore.h) on the compact chain, as rpDVIAudio runs
// it: the scan position is read from the model's channels the way
// rpDVI::scanEntry reads it, and the slots the feeder writes are the pool words
// the channels send. A word enters the FIFO once the word eight before it has
// left; the unpaced copies take no time. A packet is copied into its slot in
// the time after the scan reading that let it. Every island sent must hold,
// whole, the packet written for its own line in this frame, or, for a line the
// feeder counted as missed, what the slot held before.
// ---------------------------------------------------------------------------

namespace feeder {

typedef int64_t ps_t;
const ps_t PS_PER_PIXEL = 39683;            // 25.2 MHz
const ps_t US = 1000000;
const unsigned int LINES = 525;
const unsigned int LINE_HZ = 31500;
const ps_t FRAME_PS = (ps_t)800 * LINES * PS_PER_PIXEL;
const ps_t COST_BEGIN = 600000;             // entry, scan reading, bookkeeping
const ps_t COST_COLD = 10700000;            // a seldom path's code fetched from flash
const ps_t COST_NULL = 350000;              // a null packet made
const ps_t COST_ENCODE = 7000000;           // a sample packet made
const ps_t COST_COPY = 250000;              // 32 words into a slot
const ps_t COST_END = 200000;

struct stWrite
{
    ps_t iFrom;
    ps_t iTo;
    unsigned int iLine;
    uint32_t iWords[32];
};

struct stIslandAt
{
    size_t iFirst;          // FIFO index, within the frame, of the packet's first word
    unsigned int iLine;
    unsigned int iSlot;
};

}

// iBigPerMille ticks in a thousand are held off between iBigMin and iBigMax,
// the others up to 3 us.
static void testFeederOnChain(unsigned int iWidth, unsigned int iHeight, unsigned int iSeed, int iFrames,
                              unsigned int iBigPerMille, feeder::ps_t iBigMin, feeder::ps_t iBigMax)
{
    using namespace model;
    using namespace feeder;
    const stDVIMode & stMode = DVI_MODE_640x480_60;

    std::vector<uint32_t> obPool(rpDVIFrame::audioFramingPictureWords(stMode, iWidth, iHeight));
    std::vector<uint16_t> obPixels((size_t)iWidth * iHeight);
    for (size_t i = 0; i < obPixels.size(); i++)
        obPixels[i] = (uint16_t)(i * 2654435761u >> 9);

    // The list and its frame, as rpDVI builds them.
    const unsigned int iEntries = rpDVIList::entryCount(stMode, true, iWidth, iHeight, true);
    const unsigned int iRowEntries = rpDVIList::rowEntryCount(true, iHeight);
    std::vector<stDVIListEntry> obEntries(iEntries + 1u);
    std::vector<stDVIScanRun> obRuns(rpDVIList::SCAN_RUNS);
    std::vector<stDVIRowEntry> obRows(iRowEntries + 1u);
    uint32_t iListStart = 0;
    bus obBus;
    list obList;
    obList.pBus = &obBus;
    rpDVIFrame obListFrame;
    obListFrame.setAudioFraming(obPool.data(), (unsigned int)obPool.size());
    obBus.add(&obListFrame, sizeof(obListFrame));
    obBus.add(&obList, sizeof(obList));
    obBus.add(obPool.data(), obPool.size() * 4u);
    obBus.add(obPixels.data(), obPixels.size() * 2u);
    const uint32_t iListBus = obBus.add(obEntries.data(), obEntries.size() * sizeof(stDVIListEntry));
    obBus.add(&iListStart, 4);
    iListStart = iListBus;
    const uint32_t iRowsBus = obBus.add(obRows.data(), obRows.size() * sizeof(stDVIRowEntry));
    rpDVIList::stConfig stConfig;
    stConfig.iCtrlWords = ctrl(2, true, false, TREQ_HSTX, CH_LIST);
    stConfig.iCtrlPixels = ctrl(1, true, false, TREQ_HSTX, CH_LIST);
    stConfig.iCtrlCopy = ctrl(2, false, false, TREQ_FORCE, CH_LIST);
    stConfig.iChainBits = CTRL_CHAIN_BITS;
    stConfig.iChainShift = CTRL_CHAIN_LSB;
    stConfig.iJoinChannel = CH_JOIN;
    stConfig.iRestartChannel = CH_RESTART;
    stConfig.iRewindChannel = CH_REWIND;
    stConfig.iFifo = REG_FIFO;
    stConfig.iRowReadAddr = reg(CH_ROW, 0x00);
    obList.begin(obEntries.data(), iEntries, obRuns.data(), obRows.data(), iRowEntries, stConfig, true);
    CHECK(obListFrame.buildPicture(stMode, obPixels.data(), iWidth, iHeight, iWidth, obList));
    CHECK(obList.finish());
    CHECK(obList.joins());

    // The same frame as pieces: the FIFO words, the clock each leaves the FIFO
    // at, and the audio slot islands.
    recordingSink obRef;
    rpDVIFrame obRefFrame;
    obRefFrame.setAudioFraming(obPool.data(), (unsigned int)obPool.size());
    CHECK(obRefFrame.buildPicture(stMode, obPixels.data(), iWidth, iHeight, iWidth, obRef));
    std::vector<uint32_t> obWords;
    std::vector<const uint32_t *> obSource;
    for (const stPiece & stP : obRef.obPieces)
        for (unsigned int i = 0; i < stP.iCount; i++)
        {
            obWords.push_back(stP.bPixels ? ((uint32_t)stP.pPixels[i] << 16) | stP.pPixels[i] : stP.pWords[i]);
            obSource.push_back(stP.bPixels ? nullptr : stP.pWords + i);
        }
    const size_t N = obWords.size();
    std::vector<int64_t> obPop(N, 0);
    std::vector<stIslandAt> obIslands;
    {
        size_t i = 0;
        int64_t t = 0;
        bool bOk = true;
        while (i < N && bOk)
        {
            const uint32_t iOp = obWords[i] & ~0xfffu;
            const unsigned int n = obWords[i] & 0xfffu;
            obPop[i] = t;
            if (iOp == rpDVIFrame::CMD_RAW_REPEAT || iOp == rpDVIFrame::CMD_TMDS_REPEAT)
            {
                bOk = i + 1 < N;
                if (bOk)
                    obPop[i + 1] = t;
                i += 2;
            }
            else if ((iOp == rpDVIFrame::CMD_RAW || iOp == rpDVIFrame::CMD_TMDS) && i + n < N)
            {
                if (iOp == rpDVIFrame::CMD_RAW && n == 32)
                    for (unsigned int s = 0; s < obListFrame.audioSlotCount(); s++)
                        if (obListFrame.audioSlotWords(s) == obSource[i + 1])
                            obIslands.push_back({ i + 1, (unsigned int)(t / 800), s });
                for (unsigned int k = 0; k < n; k++)
                    obPop[i + 1 + k] = t + k;
                i += 1 + n;
            }
            else
                bOk = false;
            t += n;
        }
        CHECK(bOk && t == 800 * (int64_t)LINES);
    }
    CHECK(!obIslands.empty());
    auto fetchAt = [&](size_t g) -> ps_t {
        if (g < 8)
            return 0;
        const size_t j = g - 8;
        return ((ps_t)(j / N) * 800 * LINES + obPop[j % N]) * PS_PER_PIXEL;
    };

    // The channels as rpDVI sets them up.
    dma obDMA(obBus);
    stChannel & stList = obDMA.ch[CH_LIST];
    stList.iCtrl = ctrl(2, true, true, TREQ_FORCE, CH_LIST, 4, true);
    stList.iRead = iListBus;
    stList.iWrite = reg(CH_PIXEL, 0x30);
    stList.iReload = 4;
    stChannel & stRewind = obDMA.ch[CH_REWIND];
    stRewind.iCtrl = ctrl(2, false, false, TREQ_FORCE, CH_REWIND);
    stRewind.iRead = obBus.to(&iListStart);
    stRewind.iWrite = reg(CH_LIST, 0x3c);
    stRewind.iReload = 1;
    stChannel & stJoin = obDMA.ch[CH_JOIN];
    stJoin.iCtrl = ctrl(2, true, false, TREQ_HSTX, CH_ROW);
    stJoin.iRead = obList.joinStart();
    stJoin.iWrite = REG_FIFO;
    stJoin.iReload = obList.joinWords();
    stChannel & stRow = obDMA.ch[CH_ROW];
    stRow.iCtrl = ctrl(2, true, true, TREQ_FORCE, CH_PIXEL, 3, true);
    stRow.iRead = obList.rowStart();
    stRow.iWrite = reg(CH_PIXEL, 0x10);
    stRow.iReload = 2;
    stChannel & stRestart = obDMA.ch[CH_RESTART];
    stRestart.iCtrl = ctrl(2, false, false, TREQ_FORCE, CH_RESTART);
    stRestart.iRead = obBus.to(obList.joinStartWord());
    stRestart.iWrite = reg(CH_JOIN, 0x3c);
    stRestart.iReload = 1;
    obDMA.trigger(CH_LIST);

    // Runs the channels up to time iTime: every transfer but a FIFO word due later.
    auto advanceTo = [&](ps_t iTime) {
        while (!obDMA.bFault)
        {
            int iBusy = -1;
            for (uint32_t c = 0; c < CHANNELS; c++)
                if (obDMA.ch[c].bBusy)
                    iBusy = (int)c;
            if (iBusy < 0)
            {
                obDMA.bFault = true;
                break;
            }
            if (obDMA.ch[iBusy].iWrite == REG_FIFO && fetchAt(obDMA.obFifo.size()) > iTime)
                break;
            obDMA.step();
        }
    };
    // rpDVI::scanEntry on the channels as they stand.
    auto scanEntry = [&]() -> uint32_t {
        const stState stS = obDMA.state();
        return obList.scanPiece((stS.iListRead - iListBus) / (uint32_t)sizeof(stDVIListEntry), stS.iPixelCount & COUNT_BITS,
                                (stS.iRowRead - iRowsBus) / (uint32_t)sizeof(stDVIRowEntry));
    };

    // The feeder, as rpDVIAudio::start() sets it up, the tone in both channels.
    std::vector<uint8_t> obBuffer(audioBufferBytes(LINES), 0xa5);
    stDVIAudioCore stCore;
    audioInit(stCore, obBuffer.data(), LINES, obList.scanCount(), obList.scanFirstRow(), obList.rows());
    for (unsigned int iLine = 0; iLine < LINES; iLine++)
        audioSetLine(stCore, iLine, obListFrame.audioLineSlot(iLine));
    const unsigned int iSlots = obListFrame.audioSlotCount();
    for (unsigned int s = 0; s < iSlots; s++)
        audioSetSlot(stCore, s, obListFrame.audioSlotWords(s));
    uint8_t iStatus[DVI_CHANNEL_STATUS_BYTES];
    CHECK(dviChannelStatus(48000, iStatus));
    CHECK(audioConfigure(stCore, iSlots, 48000, LINE_HZ, obListFrame.audioVLevel(), obListFrame.audioHLevel(), iStatus));
    stCore.iTone = DVI_AUDIO_TONE_LEFT | DVI_AUDIO_TONE_RIGHT;
    stCore.bRunning = true;

    std::vector<std::vector<stWrite>> obWrites(iSlots);
    for (unsigned int s = 0; s < iSlots; s++)
    {
        stWrite stW = { -FRAME_PS * 4, -FRAME_PS * 4, 0xffffffffu, {} };
        std::memcpy(stW.iWords, obListFrame.audioSlotWords(s), sizeof(stW.iWords));
        obWrites[s].push_back(stW);
    }
    std::vector<std::pair<ps_t, unsigned int>> obMissed;     // time accounted, line

    // Ticks, as the alarm serves them: a gap after each, plus the time other
    // interrupts of the same priority hold it off.
    uint64_t iRng = 0x9e3779b97f4a7c15ull * (iSeed + 1u);
    auto random = [&](ps_t iMax) -> ps_t {
        iRng = iRng * 6364136223846793005ull + 1442695040888963407ull;
        return iMax <= 0 ? 0 : (ps_t)((iRng >> 33) % (uint64_t)iMax);
    };
    const ps_t iEnd = FRAME_PS * iFrames;
    ps_t iTickAt = FRAME_PS / 3 + random(FRAME_PS / 2);
    unsigned int iTicks = 0;
    while (iTickAt < iEnd && !obDMA.bFault)
    {
        ps_t t = iTickAt;
        advanceTo(t);
        const bool bFirst = !stCore.bStarted;
        const uint32_t iNextBefore = stCore.iNext, iNextLineBefore = stCore.iNextLine;
        audioTickBegin(stCore, scanEntry());
        t += COST_BEGIN;
        if (!bFirst)
            for (uint32_t k = 0; k < stCore.iNext - iNextBefore; k++)
                obMissed.push_back({ t, (iNextLineBefore + k) % LINES });
        if (!bFirst && stCore.iTickCost >= DVI_AUDIO_COST_COLD)
            t += COST_COLD;
        uint32_t iWords[32];
        for (;;)
        {
            const uint32_t iCostBefore = stCore.iTickCost;
            const int iSlot = audioTickPrepare(stCore, iWords);
            if (iSlot < 0)
                break;
            t += stCore.iTickCost - iCostBefore == DVI_AUDIO_COST_SAMPLES ? COST_ENCODE : COST_NULL;
            advanceTo(t);
            const unsigned int iLine = stCore.iNextLine;
            const uint32_t iLate = stCore.iLate;
            const uint32_t iScan = scanEntry();
            advanceTo(t + COST_COPY);
            audioTickCommit(stCore, iScan, iWords);
            if (stCore.iLate == iLate)
            {
                stWrite stW = { t, t + COST_COPY, iLine, {} };
                std::memcpy(stW.iWords, obListFrame.audioSlotWords((unsigned int)iSlot), sizeof(stW.iWords));
                obWrites[(unsigned int)iSlot].push_back(stW);
            }
            else
                obMissed.push_back({ t, iLine });
            t += COST_COPY;
        }
        t += COST_END;
        const ps_t iGap = (ps_t)audioTickEnd(stCore) * US;
        const ps_t iHeldOff = random(1000) < (ps_t)iBigPerMille ? iBigMin + random(iBigMax - iBigMin) : random(3 * US);
        iTickAt = t + iGap + iHeldOff;
        iTicks++;
    }
    advanceTo(iEnd);
    CHECK(!obDMA.bFault);

    // Every island of the frames after the first, against the writes.
    ps_t iMarginMin = FRAME_PS;
    unsigned int iJudged = 0, iTorn = 0, iWrongLine = 0, iStale = 0, iHeld = 0, iWordsWrong = 0;
    const size_t iFifoWords = obDMA.obFifo.size();
    for (size_t iF = 1; (iF + 1) * N <= iFifoWords; iF++)
        for (const stIslandAt & stI : obIslands)
        {
            const size_t g = iF * N + stI.iFirst;
            const ps_t iFrom = fetchAt(g), iTo = fetchAt(g + 31);
            const std::vector<stWrite> & obW = obWrites[stI.iSlot];
            const stWrite * pLast = nullptr;
            bool bTorn = false;
            for (const stWrite & stW : obW)
            {
                if (stW.iTo <= iFrom)
                    pLast = &stW;
                else if (stW.iFrom <= iTo)
                    bTorn = true;
            }
            iJudged++;
            if (bTorn)
            {
                if (iTorn++ < 5)
                    std::printf("  feeder: island line %u slot %u written while sent (frame %zu)\n", stI.iLine, stI.iSlot, iF);
                continue;
            }
            if (pLast == nullptr || std::memcmp(pLast->iWords, &obDMA.obFifo[g], sizeof(pLast->iWords)) != 0)
            {
                iWordsWrong++;
                continue;
            }
            const bool bThisFrame = iFrom - pLast->iTo < FRAME_PS / 2;
            if (pLast->iLine == stI.iLine && bThisFrame)
            {
                iMarginMin = std::min(iMarginMin, iFrom - pLast->iTo);
                continue;
            }
            bool bMissed = false;
            for (const auto & stM : obMissed)
                if (stM.second == stI.iLine && stM.first > iFrom - FRAME_PS / 2 && stM.first < iFrom + FRAME_PS / 2)
                    bMissed = true;
            if (bMissed)
                iHeld++;
            else if (pLast->iLine == stI.iLine)
                iStale++;
            else if (iWrongLine++ < 5)
                std::printf("  feeder: island line %u slot %u holds the packet of line %u, written %.1f us before (frame %zu)\n",
                            stI.iLine, stI.iSlot, pLast->iLine, (double)(iFrom - pLast->iTo) / US, iF);
        }
    // Every other word sent is the frame's own: nothing outside a slot is written.
    std::vector<bool> obInSlot(N, false);
    for (const stIslandAt & stI : obIslands)
        for (unsigned int k = 0; k < 32; k++)
            obInSlot[stI.iFirst + k] = true;
    unsigned int iOutside = 0;
    for (size_t iF = 1; (iF + 1) * N <= iFifoWords; iF++)
        for (size_t k = 0; k < N; k++)
            if (!obInSlot[k] && obDMA.obFifo[iF * N + k] != obWords[k] && iOutside++ < 5)
                std::printf("  feeder: word %zu of the frame (clock %lld) changed (frame %zu)\n", k, (long long)obPop[k], iF);
    CHECK(iOutside == 0);
    CHECK(iJudged > 0);
    CHECK(iTorn == 0);
    CHECK(iWrongLine == 0);
    CHECK(iWordsWrong == 0);
    CHECK(iStale == 0);
    CHECK(iMarginMin >= 4 * US);
    if (iBigPerMille == 0)
        CHECK(stCore.iLate == 0 && iHeld == 0);
    std::printf("feeder on chain %ux%u seed %u: islands %u, written while sent %u, wrong line %u, words %u, stale %u, "
                "held %u, written %.1f us or more before sent | ticks %u packets %u late %u\n",
                iWidth, iHeight, iSeed, iJudged, iTorn, iWrongLine, iWordsWrong, iStale, iHeld, (double)iMarginMin / US,
                iTicks, (unsigned)stCore.iPackets, (unsigned)stCore.iLate);
}

static void testFeederOnChains()
{
    using feeder::US;
    for (unsigned int iSeed = 1; iSeed <= 3; iSeed++)
        testFeederOnChain(480, 320, iSeed, 6, 0, 0, 0);
    for (unsigned int iSeed = 4; iSeed <= 5; iSeed++)
        testFeederOnChain(480, 320, iSeed, 6, 30, 30 * US, 80 * US);           // busy
    for (unsigned int iSeed = 6; iSeed <= 7; iSeed++)
        testFeederOnChain(480, 320, iSeed, 6, 4, 1000 * US, 3000 * US);        // blocked
    testFeederOnChain(640, 480, 8, 4, 0, 0, 0);
    testFeederOnChain(320, 240, 9, 4, 0, 0, 0);
}

int main()
{
    testColourWord();
    testBars();
    testPicture(480, 320);      // the LCD's size, bordered
    testPicture(640, 480);      // full size, no border
    testPicture(2, 1);          // smallest window
    testPicture2x(320, 240);    // fills the picture
    testPicture2x(240, 160);    // bordered
    testPicture2x(1, 1);        // smallest window, odd width
    testPieceCounts();
    testRefusals();
    testDVIFramingUnchanged();
    testScanOuts();
    testFeederOnChains();

    std::printf("scan positions read %u, repeated %u\n", g_iScanReads, g_iScanRetries);
    std::printf("dvi_tests: %d checks, %d failures\n", g_iChecks, g_iFailures);
    return g_iFailures == 0 ? 0 : 1;
}
