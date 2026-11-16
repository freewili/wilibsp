// Host tests for audio framing in bsp/display/rpDVIFrame and the packet coding in
// bsp/display/rpDVIPacket. The frame is run through a model of the HSTX command
// expander and decoded pixel clock by pixel clock with a decoder written here
// from the specification's definitions: its own TERC4 and control symbol tables, and a
// bit-serial BCH check by polynomial division. None of the encoder's tables or
// functions is used to decode.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rpDVIFrame.h"
#include "rpDVIPacket.h"

static int g_iFailures = 0;
static int g_iChecks = 0;

#define CHECK(cond) \
    do { \
        g_iChecks++; \
        if (!(cond)) { \
            g_iFailures++; \
            if (g_iFailures <= 30) \
                std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static const unsigned int H_TOTAL = 800;
static const unsigned int V_TOTAL = 525;
static const unsigned int H_BLANK = 160;
static const unsigned int V_BLANK = 45;

// Vertical blanking with audio framing, from the frame's documented layout:
// front porch lines 0 to 9 and back porch lines 14 to 44 are blocks of four, a
// clock regeneration line and three audio slot lines; sync lines 10 and 11
// carry no island; back porch lines 12 and 13 the AVI and Audio InfoFrames.
static bool clockLine(unsigned int iLine)
{
    return (iLine < 10 && iLine % 4 == 0) || (iLine >= 14 && iLine < V_BLANK && (iLine - 14) % 4 == 0);
}

static bool blankSlotLine(unsigned int iLine)
{
    return iLine < V_BLANK && iLine != 10 && iLine != 11 && iLine != 12 && iLine != 13 && !clockLine(iLine);
}

static const unsigned int BLANK_SLOT_LINES = 30;    // 7 in the front porch, 23 in the back porch

// ---------------------------------------------------------------------------
// Symbols, from the specification's tables, written q9 first.
// ---------------------------------------------------------------------------

static unsigned int bits10(const char * szBits)
{
    unsigned int iValue = 0;
    for (int i = 0; i < 10; i++)
        iValue = (iValue << 1) | (szBits[i] == '1' ? 1u : 0u);
    return iValue;
}

static const char * TERC4_Q[16] = {
    "1010011100", "1001100011", "1011100100", "1011100010",
    "0101110001", "0100011110", "0110001110", "0100111100",
    "1011001100", "0100111001", "0110011100", "1011000110",
    "1010001110", "1001110001", "0101100011", "1011000011",
};

static const char * CTRL_Q[4] = { "1101010100", "0010101011", "0101010100", "1010101011" };

static int g_iTercValue[1024];
static int g_iCtrlValue[1024];

static void initSymbols()
{
    for (int i = 0; i < 1024; i++)
    {
        g_iTercValue[i] = -1;
        g_iCtrlValue[i] = -1;
    }
    for (int i = 0; i < 16; i++)
        g_iTercValue[bits10(TERC4_Q[i])] = i;
    for (int i = 0; i < 4; i++)
        g_iCtrlValue[bits10(CTRL_Q[i])] = i;
}

static int tercValue(unsigned int iSymbol) { return g_iTercValue[iSymbol & 0x3ffu]; }
static int ctrlValue(unsigned int iSymbol) { return g_iCtrlValue[iSymbol & 0x3ffu]; }

static unsigned int lane(uint32_t iWord, unsigned int iLane) { return (iWord >> (10u * iLane)) & 0x3ffu; }

// Remainder of the bit sequence, first bit the highest power, divided by
// x^8 + x^7 + x^6 + 1. A codeword with its parity appended leaves zero.
static unsigned int bchRemainder(const std::vector<int> & obBits)
{
    std::vector<int> obWork = obBits;
    static const int G[9] = { 1, 1, 1, 0, 0, 0, 0, 0, 1 };      // x^8, x^7, ... x^0
    for (size_t i = 0; i + 8 < obWork.size(); i++)
        if (obWork[i])
            for (int j = 0; j < 9; j++)
                obWork[i + j] ^= G[j];
    unsigned int iRem = 0;
    for (size_t i = obWork.size() - 8; i < obWork.size(); i++)
        iRem = (iRem << 1) | (unsigned int)obWork[i];
    return iRem;
}

// ---------------------------------------------------------------------------
// The list, the command expander and the per-clock classification.
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
    bool addWords(const uint32_t * pWords, unsigned int iCount) override
    {
        obPieces.push_back({ false, pWords, nullptr, iCount });
        return true;
    }
    bool addPixels(const uint16_t * pPixels, unsigned int iCount) override
    {
        obPieces.push_back({ true, nullptr, pPixels, iCount });
        return true;
    }
};

struct stClock
{
    bool     bVideo;        // TMDS-encoded data
    uint32_t iWord;         // the raw word, or the data word
};

static bool expand(const std::vector<stPiece> & obPieces, std::vector<stClock> & obClocks)
{
    std::vector<uint32_t> obFifo;
    for (const stPiece & stP : obPieces)
    {
        if (stP.iCount == 0)
            return false;
        for (unsigned int i = 0; i < stP.iCount; i++)
            obFifo.push_back(stP.bPixels ? (((uint32_t)stP.pPixels[i] << 16) | stP.pPixels[i]) : stP.pWords[i]);
    }
    size_t iPos = 0;
    while (iPos < obFifo.size())
    {
        const uint32_t iCmd = obFifo[iPos++];
        const uint32_t iOp = iCmd >> 12;
        const unsigned int iCount = iCmd & 0xfffu;
        if (iCount == 0)
            return false;
        if (iOp == 0x0 || iOp == 0x2)                   // raw, TMDS: one word per clock
        {
            if (iPos + iCount > obFifo.size())
                return false;
            for (unsigned int i = 0; i < iCount; i++)
                obClocks.push_back({ iOp == 0x2, obFifo[iPos++] });
        }
        else if (iOp == 0x1 || iOp == 0x3)              // raw repeat, TMDS repeat
        {
            if (iPos >= obFifo.size())
                return false;
            const uint32_t iWord = obFifo[iPos++];
            for (unsigned int i = 0; i < iCount; i++)
                obClocks.push_back({ iOp == 0x3, iWord });
        }
        else
            return false;
    }
    return true;
}

enum eKind { K_CONTROL, K_ISLAND_GUARD, K_VIDEO_GUARD, K_DATA, K_VIDEO, K_INVALID };

// Control: lanes 1 and 2 CTL bit pairs in iCtl (lane 2 high).
static eKind classify(const stClock & stC, int & iCtl)
{
    iCtl = -1;
    if (stC.bVideo)
        return K_VIDEO;
    const unsigned int i0 = lane(stC.iWord, 0), i1 = lane(stC.iWord, 1), i2 = lane(stC.iWord, 2);
    if ((stC.iWord >> 30) != 0)
        return K_INVALID;
    if (ctrlValue(i0) >= 0 && ctrlValue(i1) >= 0 && ctrlValue(i2) >= 0)
    {
        iCtl = (ctrlValue(i2) << 2) | ctrlValue(i1);
        return K_CONTROL;
    }
    if (i0 == bits10("1011001100") && i1 == bits10("0100110011") && i2 == bits10("1011001100"))
        return K_VIDEO_GUARD;
    if (i1 == bits10("0100110011") && i2 == bits10("0100110011") && tercValue(i0) >= 0 && (tercValue(i0) & 0xc) == 0xc)
        return K_ISLAND_GUARD;
    if (tercValue(i0) >= 0 && tercValue(i1) >= 0 && tercValue(i2) >= 0)
        return K_DATA;
    return K_INVALID;
}

// Sync wire levels a clock carries on lane 0; false if it carries none.
static bool clockLevels(const stClock & stC, bool & bV, bool & bH)
{
    int iCtl;
    const eKind eK = classify(stC, iCtl);
    int iValue = -1;
    if (eK == K_CONTROL)
        iValue = ctrlValue(lane(stC.iWord, 0));
    else if (eK == K_ISLAND_GUARD || eK == K_DATA)
        iValue = tercValue(lane(stC.iWord, 0));
    if (iValue < 0)
        return false;
    bV = (iValue & 2) != 0;
    bH = (iValue & 1) != 0;
    return true;
}

// ---------------------------------------------------------------------------
// The stream with audio framing: periods, islands, packets.
// ---------------------------------------------------------------------------

struct stPacketSeen
{
    unsigned int iLine;
    unsigned int iX;            // clock of the first packet symbol within the line
    uint8_t      iHeader[3];
    uint8_t      iSub[4][7];
};

struct stStream
{
    std::vector<stPacketSeen> obPackets;
    std::vector<unsigned int> obIslandLines;
    std::vector<unsigned int> obIslandStartX;   // first preamble clock
    std::vector<unsigned int> obVideoLines;
    std::vector<unsigned int> obVideoStartX;
    std::vector<unsigned int> obVideoLength;
};

// Decodes one packet from 32 data clocks.
static void decodePacket(const std::vector<stClock> & obClocks, size_t iAt, stPacketSeen & stP)
{
    std::vector<int> obHeader;
    std::vector<int> obSub[4];
    for (unsigned int k = 0; k < 32; k++)
    {
        const uint32_t iWord = obClocks[iAt + k].iWord;
        const int i0 = tercValue(lane(iWord, 0));
        const int i1 = tercValue(lane(iWord, 1));
        const int i2 = tercValue(lane(iWord, 2));
        CHECK(((i0 >> 3) & 1) == (k == 0 ? 0 : 1));
        obHeader.push_back((i0 >> 2) & 1);
        for (int i = 0; i < 4; i++)
        {
            obSub[i].push_back((i1 >> i) & 1);
            obSub[i].push_back((i2 >> i) & 1);
        }
    }
    // Bit 8n + b of a sequence, in the order sent, is bit b of byte n.
    CHECK(bchRemainder(obHeader) == 0);
    for (int i = 0; i < 4; i++)
        CHECK(bchRemainder(obSub[i]) == 0);
    for (int n = 0; n < 3; n++)
    {
        unsigned int iByte = 0;
        for (int b = 0; b < 8; b++)
            iByte |= (unsigned int)obHeader[8 * n + b] << b;
        stP.iHeader[n] = (uint8_t)iByte;
    }
    for (int i = 0; i < 4; i++)
        for (int n = 0; n < 7; n++)
        {
            unsigned int iByte = 0;
            for (int b = 0; b < 8; b++)
                iByte |= (unsigned int)obSub[i][8 * n + b] << b;
            stP.iSub[i][n] = (uint8_t)iByte;
        }
}

// Walks the whole frame: every preamble is 8 clocks after at least 4 plain
// control clocks and is followed by its guard band; every island is guard 2,
// 32 x n data clocks, guard 2, then control; every video period follows a video
// guard band of 2. Anything else fails.
static void parseStream(const std::vector<stClock> & obClocks, stStream & stS)
{
    const size_t iN = obClocks.size();
    std::vector<eKind> obKind(iN);
    std::vector<int> obCtl(iN);
    for (size_t i = 0; i < iN; i++)
        obKind[i] = classify(obClocks[i], obCtl[i]);

    auto runLength = [&](size_t iFrom, eKind eK) {
        size_t j = iFrom;
        while (j < iN && obKind[j] == eK)
            j++;
        return j - iFrom;
    };

    unsigned int iPlain = 1000;         // the frame starts in a long control period
    size_t i = 0;
    while (i < iN)
    {
        const eKind eK = obKind[i];
        if (eK == K_CONTROL)
        {
            const int iCtl = obCtl[i];
            size_t j = i;
            while (j < iN && obKind[j] == K_CONTROL && obCtl[j] == iCtl)
                j++;
            const unsigned int iLen = (unsigned int)(j - i);
            if (iCtl == 0)
            {
                iPlain = iLen;
                i = j;
                continue;
            }
            const bool bIsland = iCtl == ((1 << 2) | 1);   // CTL0..3 = 1, 0, 1, 0
            const bool bVideo = iCtl == 1;                  // CTL0..3 = 1, 0, 0, 0
            CHECK(bIsland || bVideo);
            CHECK(iLen == 8);
            CHECK(iPlain >= 4);
            iPlain = 0;
            if (!bIsland && !bVideo)
            {
                i = j;
                continue;
            }
            if (bIsland)
            {
                const size_t iG1 = runLength(j, K_ISLAND_GUARD);
                CHECK(iG1 == 2);
                const size_t iD = runLength(j + iG1, K_DATA);
                CHECK(iD >= 32 && iD % 32 == 0 && iD <= 18 * 32);
                const size_t iG2 = runLength(j + iG1 + iD, K_ISLAND_GUARD);
                CHECK(iG2 == 2);
                const size_t iEnd = j + iG1 + iD + iG2;
                CHECK(iEnd < iN && obKind[iEnd] == K_CONTROL && obCtl[iEnd] == 0);
                stS.obIslandLines.push_back((unsigned int)(i / H_TOTAL));
                stS.obIslandStartX.push_back((unsigned int)(i % H_TOTAL));
                for (size_t iPk = 0; iPk + 32 <= iD; iPk += 32)
                {
                    stPacketSeen stP = {};
                    stP.iLine = (unsigned int)((j + iG1 + iPk) / H_TOTAL);
                    stP.iX = (unsigned int)((j + iG1 + iPk) % H_TOTAL);
                    decodePacket(obClocks, j + iG1 + iPk, stP);
                    stS.obPackets.push_back(stP);
                }
                i = iEnd;
            }
            else
            {
                const size_t iG = runLength(j, K_VIDEO_GUARD);
                CHECK(iG == 2);
                const size_t iV = runLength(j + iG, K_VIDEO);
                CHECK(iV > 0);
                const size_t iEnd = j + iG + iV;
                CHECK(iEnd == iN || (obKind[iEnd] == K_CONTROL && obCtl[iEnd] == 0));
                stS.obVideoLines.push_back((unsigned int)((j + iG) / H_TOTAL));
                stS.obVideoStartX.push_back((unsigned int)((j + iG) % H_TOTAL));
                stS.obVideoLength.push_back((unsigned int)iV);
                i = iEnd;
            }
            continue;
        }
        // Nothing else may start a period.
        CHECK(eK == K_CONTROL);
        i++;
    }
}

// ---------------------------------------------------------------------------
// Frame checks.
// ---------------------------------------------------------------------------

static bool isZero(const uint8_t * pBytes, unsigned int iCount)
{
    for (unsigned int i = 0; i < iCount; i++)
        if (pBytes[i] != 0)
            return false;
    return true;
}

// Payload byte PBn of an InfoFrame packet.
static uint8_t infoByte(const stPacketSeen & stP, unsigned int iPB)
{
    return stP.iSub[iPB / 7][iPB % 7];
}

static bool infoChecksumZero(const stPacketSeen & stP)
{
    unsigned int iSum = stP.iHeader[0] + stP.iHeader[1] + stP.iHeader[2];
    for (unsigned int iPB = 0; iPB <= stP.iHeader[2]; iPB++)
        iSum += infoByte(stP, iPB);
    return (iSum & 0xffu) == 0;
}

// Sync levels and active area against the DVI frame, clock by clock.
static void checkAgainstDVI(const std::vector<stClock> & obAudioFraming, const std::vector<stClock> & obDVI)
{
    CHECK(obAudioFraming.size() == (size_t)H_TOTAL * V_TOTAL);
    CHECK(obDVI.size() == (size_t)H_TOTAL * V_TOTAL);
    if (obAudioFraming.size() != obDVI.size() || obDVI.size() != (size_t)H_TOTAL * V_TOTAL)
        return;
    unsigned int iLevelMismatch = 0, iActiveMismatch = 0, iDataMismatch = 0, iGuardMisplaced = 0, iUnread = 0;
    for (size_t i = 0; i < obAudioFraming.size(); i++)
    {
        const unsigned int iX = (unsigned int)(i % H_TOTAL), iY = (unsigned int)(i / H_TOTAL);
        const stClock & stH = obAudioFraming[i];
        const stClock & stD = obDVI[i];
        if (stH.bVideo != stD.bVideo)
            iActiveMismatch++;
        if (stD.bVideo)
        {
            if (stH.iWord != stD.iWord)
                iDataMismatch++;
            continue;
        }
        bool bDV = false, bDH = false, bHV = false, bHH = false;
        if (!clockLevels(stD, bDV, bDH))
        {
            iUnread++;
            continue;
        }
        if (!clockLevels(stH, bHV, bHH))
        {
            int iCtl;
            // Only the video guard band carries no levels, just before active video.
            if (classify(stH, iCtl) != K_VIDEO_GUARD || iY < V_BLANK || iX < H_BLANK - 2 || iX >= H_BLANK)
                iGuardMisplaced++;
            continue;
        }
        if (bHV != bDV || bHH != bDH)
            iLevelMismatch++;
    }
    CHECK(iActiveMismatch == 0);
    CHECK(iDataMismatch == 0);
    CHECK(iLevelMismatch == 0);
    CHECK(iGuardMisplaced == 0);
    CHECK(iUnread == 0);
}

// The fixed islands, the audio slot islands, and video on every active line.
static void checkStream(const stStream & stS, unsigned int iRate, bool bSlotsNull)
{
    // Islands: every line but the two vertical sync lines, each inside the sync
    // pulse: preamble from clock 16, packet from 26.
    std::vector<unsigned int> obExpect;
    for (unsigned int iY = 0; iY < V_TOTAL; iY++)
        if (iY != 10 && iY != 11)
            obExpect.push_back(iY);
    CHECK(stS.obIslandLines == obExpect);
    for (unsigned int iX : stS.obIslandStartX)
        CHECK(iX == 16);
    CHECK(stS.obPackets.size() == obExpect.size());
    for (const stPacketSeen & stP : stS.obPackets)
        CHECK(stP.iX == 26 && stP.iX + 32 + 2 <= 16 + 96);

    unsigned int iAVI = 0, iAudioInfo = 0, iClock = 0, iNull = 0, iOther = 0;
    for (const stPacketSeen & stP : stS.obPackets)
    {
        if (stP.iHeader[0] == 0x82)
        {
            iAVI++;
            CHECK(stP.iLine == 12);
            CHECK(stP.iHeader[1] == 0x02 && stP.iHeader[2] == 0x0d);
            CHECK(infoChecksumZero(stP));
            CHECK(infoByte(stP, 0) == 0x3e);
            CHECK(infoByte(stP, 1) == 0x10);        // RGB, active format present
            CHECK(infoByte(stP, 2) == 0x18);        // 4:3, same as picture
            CHECK(infoByte(stP, 3) == 0x08);        // full range
            CHECK(infoByte(stP, 4) == 1);           // VIC 1
            for (unsigned int iPB = 5; iPB < 28; iPB++)
                CHECK(infoByte(stP, iPB) == 0);
        }
        else if (stP.iHeader[0] == 0x84)
        {
            iAudioInfo++;
            CHECK(stP.iLine == 13);
            CHECK(stP.iHeader[1] == 0x01 && stP.iHeader[2] == 0x0a);
            CHECK(infoChecksumZero(stP));
            CHECK(infoByte(stP, 1) == 0x01);        // two channels
            for (unsigned int iPB = 2; iPB < 28; iPB++)
                CHECK(infoByte(stP, iPB) == 0);
        }
        else if (stP.iHeader[0] == 0x01)
        {
            iClock++;
            CHECK(clockLine(stP.iLine));
            CHECK(stP.iHeader[1] == 0 && stP.iHeader[2] == 0);
            for (int i = 0; i < 4; i++)
            {
                const uint8_t * pSB = stP.iSub[i];
                const unsigned int iCTS = ((pSB[1] & 0x0fu) << 16) | (pSB[2] << 8) | pSB[3];
                const unsigned int iN = ((pSB[4] & 0x0fu) << 16) | (pSB[5] << 8) | pSB[6];
                CHECK(pSB[0] == 0 && (pSB[1] >> 4) == 0 && (pSB[4] >> 4) == 0);
                // 128 fs = 25.2 MHz x N / CTS, exactly.
                CHECK((uint64_t)128 * iRate * iCTS == (uint64_t)25200000 * iN);
                if (iRate == 48000)
                    CHECK(iN == 6144 && iCTS == 25200);
                if (iRate == 44100)
                    CHECK(iN == 6272 && iCTS == 28000);
                if (iRate == 32000)
                    CHECK(iN == 4096 && iCTS == 25200);
            }
        }
        else if (isZero(stP.iHeader, 3) && isZero(&stP.iSub[0][0], 28))
        {
            iNull++;
            CHECK(stP.iLine >= V_BLANK || blankSlotLine(stP.iLine));
        }
        else
            iOther++;
    }
    CHECK(iAVI == 1);
    CHECK(iAudioInfo == 1);
    CHECK(iClock == 11);
    if (bSlotsNull)
    {
        CHECK(iNull == 480 + BLANK_SLOT_LINES);
        CHECK(iOther == 0);
    }

    // Video on every active line, from clock 160, 640 clocks.
    CHECK(stS.obVideoLines.size() == 480);
    for (size_t i = 0; i < stS.obVideoLines.size(); i++)
    {
        CHECK(stS.obVideoLines[i] == V_BLANK + i);
        CHECK(stS.obVideoStartX[i] == H_BLANK);
        CHECK(stS.obVideoLength[i] == 640);
    }
}

// Audio slots: every active line and every blanking slot line has one, no
// other line has; a slot is not reused within AUDIO_RING_SLOTS lines, counted
// round the frame; writing a distinct packet into each slot shows it on exactly
// the lines that slot claims.
static void checkSlots(rpDVIFrame & obFrame, const std::vector<stPiece> & obPieces, unsigned int iRate)
{
    const unsigned int iSlots = obFrame.audioSlotCount();
    CHECK(iSlots > 0);
    std::vector<std::vector<unsigned int>> obLines(iSlots);
    for (unsigned int iLine = 0; iLine < V_TOTAL + 10; iLine++)
    {
        const int iSlot = obFrame.audioLineSlot(iLine);
        if ((iLine < V_BLANK && !blankSlotLine(iLine)) || iLine >= V_TOTAL)
        {
            CHECK(iSlot == -1);
            continue;
        }
        CHECK(iSlot >= 0 && iSlot < (int)iSlots);
        if (iSlot >= 0 && iSlot < (int)iSlots)
            obLines[iSlot].push_back(iLine);
    }
    for (unsigned int iSlot = 0; iSlot < iSlots; iSlot++)
    {
        CHECK(!obLines[iSlot].empty());
        const std::vector<unsigned int> & obL = obLines[iSlot];
        for (size_t i = 0; i + 1 < obL.size(); i++)
            CHECK(obL[i + 1] - obL[i] >= rpDVIFrame::AUDIO_RING_SLOTS);
        if (obL.size() > 1)
            CHECK(obL.front() + V_TOTAL - obL.back() >= rpDVIFrame::AUDIO_RING_SLOTS);
        CHECK(obFrame.audioSlotWords(iSlot) != nullptr);
    }
    CHECK(obFrame.audioSlotWords(iSlots) == nullptr);

    // Slot s carries a one-sample packet whose left sample is s + 1.
    uint8_t iStatus[DVI_CHANNEL_STATUS_BYTES];
    CHECK(dviChannelStatus(48000, iStatus));
    for (unsigned int iSlot = 0; iSlot < iSlots; iSlot++)
    {
        const int16_t iSample[2] = { (int16_t)(iSlot + 1), (int16_t)-(int)(iSlot + 1) };
        stDVIPacket stPacket;
        dviPacketAudio(stPacket, iSample, 1, 5, iStatus);
        dviPacketWords(stPacket, obFrame.audioVLevel(), obFrame.audioHLevel(), obFrame.audioSlotWords(iSlot));
    }
    std::vector<stClock> obClocks;
    CHECK(expand(obPieces, obClocks));
    stStream stS;
    parseStream(obClocks, stS);
    checkStream(stS, iRate, false);
    unsigned int iAudio = 0;
    for (const stPacketSeen & stP : stS.obPackets)
    {
        if (stP.iHeader[0] != 0x02)
            continue;
        iAudio++;
        const int iSlot = obFrame.audioLineSlot(stP.iLine);
        const int iLeft = (int16_t)(stP.iSub[0][1] | (stP.iSub[0][2] << 8));
        CHECK(iSlot >= 0 && iLeft == iSlot + 1);
        CHECK(stP.iHeader[1] == 0x01);
    }
    CHECK(iAudio == 480 + BLANK_SLOT_LINES);

    // No more than five lines in a row without an audio slot (vertical sync,
    // the two InfoFrame lines and a clock regeneration line), round the frame.
    unsigned int iRun = 0, iRunMax = 0;
    for (unsigned int iLine = 0; iLine < 2 * V_TOTAL; iLine++)
    {
        iRun = obFrame.audioLineSlot(iLine % V_TOTAL) < 0 ? iRun + 1 : 0;
        iRunMax = iRun > iRunMax ? iRun : iRunMax;
    }
    CHECK(iRunMax == 5);

    // Silence again: null packets in every slot.
    stDVIPacket stNull;
    dviPacketNull(stNull);
    for (unsigned int iSlot = 0; iSlot < iSlots; iSlot++)
        dviPacketWords(stNull, obFrame.audioVLevel(), obFrame.audioHLevel(), obFrame.audioSlotWords(iSlot));
}

// The TV mirror's property: picture rows are two entries apart, in row order,
// pixels first.
static void checkRowEntries(const std::vector<stPiece> & obPieces, const uint16_t * pPixels,
                            unsigned int iWidth, unsigned int iHeight, unsigned int iStride)
{
    size_t iFirst = obPieces.size();
    unsigned int iRows = 0;
    for (size_t i = 0; i < obPieces.size(); i++)
    {
        if (!obPieces[i].bPixels)
            continue;
        if (iRows == 0)
            iFirst = i;
        CHECK(i == iFirst + 2u * iRows);
        CHECK(obPieces[i].pPixels == pPixels + (size_t)iRows * iStride);
        CHECK(obPieces[i].iCount == iWidth);
        iRows++;
    }
    CHECK(iRows == iHeight);
}

static std::vector<stClock> dviClocksPicture(const uint16_t * pPixels, unsigned int iWidth, unsigned int iHeight, unsigned int iStride)
{
    rpDVIFrame obFrame;
    recordingSink obSink;
    std::vector<stClock> obClocks;
    CHECK(obFrame.buildPicture(DVI_MODE_640x480_60, pPixels, iWidth, iHeight, iStride, obSink));
    CHECK(expand(obSink.obPieces, obClocks));
    return obClocks;
}

static void testPictureAudioFraming(unsigned int iWidth, unsigned int iHeight, unsigned int iRate)
{
    const stDVIMode & stMode = DVI_MODE_640x480_60;
    std::vector<uint16_t> obPixels((size_t)640 * 480);
    for (size_t i = 0; i < obPixels.size(); i++)
        obPixels[i] = (uint16_t)(i * 2654435761u >> 9);

    const unsigned int iWords = rpDVIFrame::audioFramingPictureWords(stMode, iWidth, iHeight);
    CHECK(iWords != 0);
    std::vector<uint32_t> obPool(iWords, 0xdeadbeefu);
    rpDVIFrame obFrame;
    obFrame.setAudioFraming(obPool.data(), iWords, iRate);
    recordingSink obSink;
    CHECK(obFrame.buildPicture(stMode, obPixels.data(), iWidth, iHeight, 640, obSink));
    CHECK(obSink.obPieces.size() == rpDVIFrame::picturePieceCount(stMode, iWidth, iHeight));
    checkRowEntries(obSink.obPieces, obPixels.data(), iWidth, iHeight, 640);

    // Every word of the pool is written by the build.
    bool bAllWritten = true;
    for (uint32_t iW : obPool)
        bAllWritten = bAllWritten && iW != 0xdeadbeefu;
    CHECK(bAllWritten);

    std::vector<stClock> obClocks;
    CHECK(expand(obSink.obPieces, obClocks));
    checkAgainstDVI(obClocks, dviClocksPicture(obPixels.data(), iWidth, iHeight, 640));
    stStream stS;
    parseStream(obClocks, stS);
    checkStream(stS, iRate, true);
    checkSlots(obFrame, obSink.obPieces, iRate);

    // The pool one word short is refused.
    rpDVIFrame obShort;
    obShort.setAudioFraming(obPool.data(), iWords - 1u, iRate);
    recordingSink obNone;
    CHECK(!obShort.buildPicture(stMode, obPixels.data(), iWidth, iHeight, 640, obNone));
}

static void testBarsAudioFraming()
{
    const stDVIMode & stMode = DVI_MODE_640x480_60;
    const unsigned int iWords = rpDVIFrame::audioFramingBarsWords(stMode);
    CHECK(iWords == 470u + 8u * 65u);
    std::vector<uint32_t> obPool(iWords);
    rpDVIFrame obFrame;
    obFrame.setAudioFraming(obPool.data(), iWords);
    recordingSink obSink;
    CHECK(obFrame.buildBars(stMode, obSink));
    CHECK(obSink.obPieces.size() == rpDVIFrame::barsPieceCount(stMode));

    rpDVIFrame obDVIFrame;
    recordingSink obDVISink;
    CHECK(obDVIFrame.buildBars(stMode, obDVISink));
    std::vector<stClock> obClocks, obDVI;
    CHECK(expand(obSink.obPieces, obClocks));
    CHECK(expand(obDVISink.obPieces, obDVI));
    checkAgainstDVI(obClocks, obDVI);
    stStream stS;
    parseStream(obClocks, stS);
    checkStream(stS, 48000, true);
    CHECK(obFrame.audioSlotCount() == rpDVIFrame::AUDIO_RING_SLOTS + rpDVIFrame::AUDIO_BLANK_SLOTS);
    checkSlots(obFrame, obSink.obPieces, 48000);
}

static void testPoolSizes()
{
    const stDVIMode & stMode = DVI_MODE_640x480_60;
    // Fixed lines 2 x 47, blocks 2 x 4 x 47 (470 in all), rows 8 x 54 (the
    // head is the last row slot's), borders 8 x 51.
    CHECK(rpDVIFrame::audioFramingPictureWords(stMode, 480, 320) == 470u + 8u * 54u + 8u * 51u);
    // Full size: no border words, no border ring.
    CHECK(rpDVIFrame::audioFramingPictureWords(stMode, 640, 480) == 470u + 8u * 50u);
    // One row: the head in its own words.
    CHECK(rpDVIFrame::audioFramingPictureWords(stMode, 2, 1) == 470u + 52u + 0u + 8u * 51u);
    // Fewer rows than the ring: the head in its own words.
    CHECK(rpDVIFrame::audioFramingPictureWords(stMode, 638, 2) == 470u + 52u + 1u * 54u + 8u * 51u);
    CHECK(rpDVIFrame::audioFramingPictureWords(stMode, 3, 1) == 0u);
    CHECK(rpDVIFrame::audioFramingPictureWords(stMode, 0, 1) == 0u);

    stDVIMode stNoVIC = stMode;
    stNoVIC.iVIC = 0;
    CHECK(rpDVIFrame::audioFramingPictureWords(stNoVIC, 480, 320) == 0u);
    CHECK(rpDVIFrame::audioFramingBarsWords(stNoVIC) == 0u);
    stDVIMode stShortSync = stMode;
    stShortSync.iHSync = 44;
    stShortSync.iHBackPorch = 100;
    CHECK(rpDVIFrame::audioFramingBarsWords(stShortSync) == 0u);
    stDVIMode stShortPorch = stMode;
    stShortPorch.iVBackPorch = 2;
    CHECK(rpDVIFrame::audioFramingBarsWords(stShortPorch) == 0u);

    // An audio rate without exact values is refused at build.
    std::vector<uint32_t> obPool(2048);
    std::vector<uint16_t> obPixels(480 * 320);
    rpDVIFrame obFrame;
    recordingSink obSink;
    obFrame.setAudioFraming(obPool.data(), (unsigned int)obPool.size(), 22050);
    CHECK(!obFrame.buildPicture(stMode, obPixels.data(), 480, 320, 480, obSink));
    CHECK(obFrame.audioSlotCount() == 0);
    obFrame.setAudioFraming(obPool.data(), (unsigned int)obPool.size(), 44100);
    CHECK(obFrame.buildPicture(stMode, obPixels.data(), 480, 320, 480, obSink));
}

// ---------------------------------------------------------------------------
// Packet coding.
// ---------------------------------------------------------------------------

static void testSymbols()
{
    for (unsigned int i = 0; i < 16; i++)
        CHECK(dviTERC4(i) == bits10(TERC4_Q[i]));
    for (int iV = 0; iV < 2; iV++)
        for (int iH = 0; iH < 2; iH++)
        {
            const unsigned int iSync = (unsigned int)(iV * 2 + iH);
            const unsigned int iCtl00 = bits10(CTRL_Q[0]), iCtl01 = bits10(CTRL_Q[1]);
            uint32_t iW = dviControlWord(iV, iH);
            CHECK(lane(iW, 0) == bits10(CTRL_Q[iSync]) && lane(iW, 1) == iCtl00 && lane(iW, 2) == iCtl00);
            iW = dviIslandPreambleWord(iV, iH);
            CHECK(lane(iW, 0) == bits10(CTRL_Q[iSync]) && lane(iW, 1) == iCtl01 && lane(iW, 2) == iCtl01);
            iW = dviVideoPreambleWord(iV, iH);
            CHECK(lane(iW, 0) == bits10(CTRL_Q[iSync]) && lane(iW, 1) == iCtl01 && lane(iW, 2) == iCtl00);
            iW = dviIslandGuardWord(iV, iH);
            CHECK(lane(iW, 0) == bits10(TERC4_Q[0xc | iSync]));
            CHECK(lane(iW, 1) == bits10("0100110011") && lane(iW, 2) == bits10("0100110011"));
            CHECK((iW >> 30) == 0);
        }
    const uint32_t iVG = dviVideoGuardWord();
    CHECK(lane(iVG, 0) == bits10("1011001100") && lane(iVG, 1) == bits10("0100110011") && lane(iVG, 2) == bits10("1011001100"));
}

// Random packets with every sync level pair decode back to their bytes.
static void testPacketWords()
{
    uint32_t iSeed = 12345;
    for (int iRound = 0; iRound < 64; iRound++)
    {
        stDVIPacket stPacket;
        for (int i = 0; i < 3; i++)
        {
            iSeed = iSeed * 1103515245u + 12345u;
            stPacket.iHeader[i] = (uint8_t)(iSeed >> 16);
        }
        for (int i = 0; i < 4; i++)
            for (int n = 0; n < 7; n++)
            {
                iSeed = iSeed * 1103515245u + 12345u;
                stPacket.iSub[i][n] = (uint8_t)(iSeed >> 16);
            }
        const bool bV = (iRound & 1) != 0, bH = (iRound & 2) != 0;
        uint32_t iWords[32];
        dviPacketWords(stPacket, bV, bH, iWords);
        std::vector<stClock> obClocks;
        for (uint32_t iW : iWords)
            obClocks.push_back({ false, iW });
        for (const stClock & stC : obClocks)
        {
            int iCtl;
            CHECK(classify(stC, iCtl) == K_DATA);
            bool bCV, bCH;
            CHECK(clockLevels(stC, bCV, bCH) && bCV == bV && bCH == bH);
        }
        stPacketSeen stSeen = {};
        decodePacket(obClocks, 0, stSeen);
        CHECK(std::memcmp(stSeen.iHeader, stPacket.iHeader, 3) == 0);
        CHECK(std::memcmp(stSeen.iSub, stPacket.iSub, 28) == 0);
    }
    // Parity of an all-zero block is zero; of a single 1 bit it is the
    // remainder of x^(n+8-1) by the generator, checked by division.
    const uint8_t iZero[7] = {};
    CHECK(dviParity(iZero, 7) == 0);
    for (unsigned int iBit = 0; iBit < 24; iBit++)
    {
        uint8_t iBytes[3] = {};
        iBytes[iBit / 8] = (uint8_t)(1u << (iBit % 8));
        const uint8_t iP = dviParity(iBytes, 3);
        std::vector<int> obBits;
        for (int n = 0; n < 3; n++)
            for (int b = 0; b < 8; b++)
                obBits.push_back((iBytes[n] >> b) & 1);
        for (int b = 0; b < 8; b++)
            obBits.push_back((iP >> b) & 1);
        CHECK(bchRemainder(obBits) == 0);
    }
}

static void testClockValues()
{
    unsigned int iN = 0, iCTS = 0;
    CHECK(dviClockValues(48000, 25200, iN, iCTS) && iN == 6144 && iCTS == 25200);
    CHECK(dviClockValues(44100, 25200, iN, iCTS) && iN == 6272 && iCTS == 28000);
    CHECK(dviClockValues(32000, 25200, iN, iCTS) && iN == 4096 && iCTS == 25200);
    CHECK(!dviClockValues(44100, 25175, iN, iCTS));
    CHECK(!dviClockValues(22050, 25200, iN, iCTS));
    stDVIPacket stPacket;
    dviPacketClock(stPacket, 6144, 25200);
    CHECK(stPacket.iHeader[0] == 0x01 && stPacket.iHeader[1] == 0 && stPacket.iHeader[2] == 0);
    for (int i = 0; i < 4; i++)
    {
        CHECK(stPacket.iSub[i][0] == 0);
        CHECK(stPacket.iSub[i][1] == 0x00 && stPacket.iSub[i][2] == 0x62 && stPacket.iSub[i][3] == 0x70);   // 25200
        CHECK(stPacket.iSub[i][4] == 0x00 && stPacket.iSub[i][5] == 0x18 && stPacket.iSub[i][6] == 0x00);   // 6144
    }
}

// The 192 channel status bits, from IEC 60958-3 consumer fields: bit 2 copying
// permitted; bits 24 to 27 sample rate; bits 33 to 35 = 1, 0, 0 for 16-bit words
// with bit 32 = 0 (20-bit maximum).
static bool expectedStatusBit(unsigned int iBit, unsigned int iRate)
{
    if (iBit == 2 || iBit == 33)
        return true;
    if (iRate == 48000)
        return iBit == 25;              // 24..27 = 0, 1, 0, 0
    if (iRate == 32000)
        return iBit == 24 || iBit == 25; // 1, 1, 0, 0
    return false;                       // 44.1 kHz: 0, 0, 0, 0
}

static unsigned int ones(unsigned int iValue)
{
    unsigned int iCount = 0;
    for (; iValue; iValue >>= 1)
        iCount += iValue & 1u;
    return iCount;
}

static void testAudioSamples(unsigned int iRate)
{
    uint8_t iStatus[DVI_CHANNEL_STATUS_BYTES];
    CHECK(dviChannelStatus(iRate, iStatus));

    // Three blocks, four frames per packet.
    const unsigned int iFrames = 3 * 192;
    unsigned int iFrame = 0;
    unsigned int iNext = 0;
    std::vector<int> obC, obB;
    unsigned int iParityBad = 0, iSampleBad = 0, iVUBad = 0, iLRDiffer = 0;
    while (iFrame < iFrames)
    {
        int16_t iSamples[8];
        for (int i = 0; i < 8; i++)
            iSamples[i] = (int16_t)((iFrame * 7919u + (unsigned int)i * 104729u) & 0xffffu);
        stDVIPacket stPacket;
        const unsigned int iAfter = dviPacketAudio(stPacket, iSamples, 4, iNext, iStatus);
        CHECK(iAfter == (iNext + 4) % 192);
        CHECK(stPacket.iHeader[0] == 0x02);
        CHECK(stPacket.iHeader[1] == 0x0f);         // four samples present, layout 0
        CHECK((stPacket.iHeader[2] & 0x0f) == 0);   // no flat samples
        for (int i = 0; i < 4; i++)
        {
            const uint8_t * pSB = stPacket.iSub[i];
            // 24-bit sample words, the 16-bit sample in the top 16 bits.
            const unsigned int iL24 = pSB[0] | (pSB[1] << 8) | (pSB[2] << 16);
            const unsigned int iR24 = pSB[3] | (pSB[4] << 8) | (pSB[5] << 16);
            if (iL24 != ((unsigned int)(uint16_t)iSamples[2 * i] << 8) ||
                iR24 != ((unsigned int)(uint16_t)iSamples[2 * i + 1] << 8))
                iSampleBad++;
            const unsigned int iVL = pSB[6] & 1, iUL = (pSB[6] >> 1) & 1, iCL = (pSB[6] >> 2) & 1, iPL = (pSB[6] >> 3) & 1;
            const unsigned int iVR = (pSB[6] >> 4) & 1, iUR = (pSB[6] >> 5) & 1, iCR = (pSB[6] >> 6) & 1, iPR = (pSB[6] >> 7) & 1;
            if (iVL || iUL || iVR || iUR)
                iVUBad++;
            if (iCL != iCR)
                iLRDiffer++;
            if ((ones(iL24) + iVL + iUL + iCL + iPL) % 2 != 0 || (ones(iR24) + iVR + iUR + iCR + iPR) % 2 != 0)
                iParityBad++;
            obC.push_back((int)iCL);
            obB.push_back((stPacket.iHeader[2] >> (4 + i)) & 1);
        }
        iNext = iAfter;
        iFrame += 4;
    }
    CHECK(iSampleBad == 0);
    CHECK(iParityBad == 0);
    CHECK(iVUBad == 0);
    CHECK(iLRDiffer == 0);
    unsigned int iCBad = 0, iBBad = 0;
    for (unsigned int i = 0; i < iFrames; i++)
    {
        if (obC[i] != (expectedStatusBit(i % 192, iRate) ? 1 : 0))
            iCBad++;
        if (obB[i] != (i % 192 == 0 ? 1 : 0))
            iBBad++;
    }
    CHECK(iCBad == 0);
    CHECK(iBBad == 0);
}

static void testAudioPartial()
{
    uint8_t iStatus[DVI_CHANNEL_STATUS_BYTES];
    CHECK(dviChannelStatus(48000, iStatus));
    const int16_t iSamples[8] = { 0x1234, -2, 0x7fff, -32768, 1, 2, 3, 4 };
    stDVIPacket stPacket;

    // Frames 190, 191, 0: the block starts in subpacket 2.
    CHECK(dviPacketAudio(stPacket, iSamples, 3, 190, iStatus) == 1);
    CHECK(stPacket.iHeader[1] == 0x07);
    CHECK(stPacket.iHeader[2] == 0x40);
    for (int n = 0; n < 7; n++)
        CHECK(stPacket.iSub[3][n] == 0);
    CHECK(stPacket.iSub[0][1] == 0x34 && stPacket.iSub[0][2] == 0x12);
    CHECK(stPacket.iSub[0][4] == 0xfe && stPacket.iSub[0][5] == 0xff);
    CHECK(stPacket.iSub[1][4] == 0x00 && stPacket.iSub[1][5] == 0x80);

    CHECK(dviPacketAudio(stPacket, iSamples, 0, 7, iStatus) == 7);
    CHECK(stPacket.iHeader[0] == 0x02 && stPacket.iHeader[1] == 0 && stPacket.iHeader[2] == 0);

    // Channel status bytes.
    CHECK(iStatus[0] == 0x04 && iStatus[1] == 0 && iStatus[2] == 0 && iStatus[3] == 0x02 && iStatus[4] == 0x02);
    CHECK(isZero(iStatus + 5, DVI_CHANNEL_STATUS_BYTES - 5));
    CHECK(!dviChannelStatus(96000, iStatus));
}

// Audio Sample packets against bytes written out by hand from the published
// layout: HB1 bits 0 to 3 sample_present, bit 4 layout; HB2 bits 4 to 7 B.0 to
// B.3; each subpacket SB0 to SB2 the left 24-bit word (bits 4 to 27 of the IEC
// 60958 subframe, least significant byte first), SB3 to SB5 the right, SB6 V.L,
// U.L, C.L, P.L, V.R, U.R, C.R, P.R from bit 0. P is even parity over the 24
// sample bits with V, U and C.
static void testAudioSampleLayout()
{
    uint8_t iStatus[DVI_CHANNEL_STATUS_BYTES];
    CHECK(dviChannelStatus(48000, iStatus));
    const int16_t iSamples[8] = { 0x1234, (int16_t)0xabcd, 0x1234, (int16_t)0xabcd, 0x1234, (int16_t)0xabcd, 0, 0 };
    stDVIPacket stPacket;
    CHECK(dviPacketAudio(stPacket, iSamples, 3, 0, iStatus) == 3);
    // Frames 0, 1, 2: block start on subpacket 0; channel status bits 0 and 1
    // are 0, bit 2 (copying permitted) is 1.
    const uint8_t iExpect[3 + 28] = {
        0x02, 0x07, 0x10,
        0x00, 0x34, 0x12, 0x00, 0xcd, 0xab, 0x08,       // 0x1234 odd: P.L 1; 0xabcd even: P.R 0
        0x00, 0x34, 0x12, 0x00, 0xcd, 0xab, 0x08,
        0x00, 0x34, 0x12, 0x00, 0xcd, 0xab, 0xc4,       // C 1 on both: P.L 0, P.R 1
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    CHECK(std::memcmp(stPacket.iHeader, iExpect, 3) == 0);
    CHECK(std::memcmp(stPacket.iSub, iExpect + 3, 28) == 0);

    // Header parity by the shift register of the open-source encoders: shift
    // right, feeding back 0x83 when the outgoing bit differs from the input bit.
    unsigned int iEcc = 0;
    for (unsigned int iBit = 0; iBit < 24; iBit++)
    {
        const unsigned int iIn = (iExpect[iBit / 8] >> (iBit % 8)) & 1u;
        iEcc = (iEcc >> 1) ^ (((iEcc ^ iIn) & 1u) ? 0x83u : 0u);
    }
    CHECK(dviParity(iExpect, 3) == iEcc);
}

static void testInfoFrames()
{
    stDVIPacket stPacket;
    dviPacketAVI(stPacket, 1);
    const uint8_t iAVI[3 + 28] = {
        0x82, 0x02, 0x0d,
        0x3e, 0x10, 0x18, 0x08, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    CHECK(std::memcmp(stPacket.iHeader, iAVI, 3) == 0);
    CHECK(std::memcmp(stPacket.iSub, iAVI + 3, 28) == 0);

    dviPacketAudioInfo(stPacket);
    CHECK(stPacket.iHeader[0] == 0x84 && stPacket.iHeader[1] == 0x01 && stPacket.iHeader[2] == 0x0a);
    CHECK(stPacket.iSub[0][0] == 0x70 && stPacket.iSub[0][1] == 0x01);
    CHECK(isZero(&stPacket.iSub[0][2], 26));

    dviPacketNull(stPacket);
    CHECK(isZero(stPacket.iHeader, 3) && isZero(&stPacket.iSub[0][0], 28));
}

int main()
{
    initSymbols();
    testSymbols();
    testPacketWords();
    testClockValues();
    testInfoFrames();
    testAudioSamples(48000);
    testAudioSamples(44100);
    testAudioSamples(32000);
    testAudioPartial();
    testAudioSampleLayout();
    testPoolSizes();
    testBarsAudioFraming();
    testPictureAudioFraming(480, 320, 48000);       // the LCD's size, bordered
    testPictureAudioFraming(640, 480, 48000);       // full size: no border lines, no border words
    testPictureAudioFraming(2, 1, 44100);           // smallest window: no row ring
    testPictureAudioFraming(638, 2, 32000);         // one join, 478 border lines

    std::printf("audio_framing_tests: %d checks, %d failures\n", g_iChecks, g_iFailures);
    return g_iFailures == 0 ? 0 : 1;
}
