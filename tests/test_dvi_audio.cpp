// Host simulation of the TV sound feeder (bsp/display/rpDVIAudioCore.h).
//
// The real frame is built by rpDVIFrame with audio framing and run through a
// model of the HSTX FIFO and command expander: every FIFO word is popped at
// the pixel clock it is used, and fetched by the DMA when the word eight before
// it has left the 8-deep FIFO. From that come the list entry the list channel
// reports at any moment and the interval in which each island's 32 packet
// words are fetched.
//
// Three actors share the real core state and the real slot memory:
//   - the TV: at the end of each island's fetch, judges what the slot held: a
//     write during the fetch is a torn packet; the packet is decoded with a
//     decoder written here (its own TERC4 table, bit-serial BCH check); it must
//     have been written for this line after the previous use of the slot ended,
//     or else it is a held packet (a repeat if it carries samples);
//   - the feeder: the timer interrupt, with a random delay before each tick is
//     served and a cost in time for each step of the tick;
//   - the producer: writes a counting ramp into the sample buffer, so a repeat,
//     a reordering or a gap shows at the TV.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

#include "rpDVIFrame.h"
#include "rpDVIPacket.h"
#include "rpDVIAudioCore.h"

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

typedef int64_t ps_t;                               // picoseconds
static const ps_t PS_PER_PIXEL = 39683;             // 25.2 MHz
static const ps_t US = 1000000;
static const ps_t NEVER = INT64_MAX / 4;

static const unsigned int LINE_CLOCKS = 800;
static const unsigned int LINES = 525;
static const unsigned int FRAME_CLOCKS = LINE_CLOCKS * LINES;
static const unsigned int LINE_HZ = 31500;
static const unsigned int FIFO_WORDS = 8;
static const unsigned int MUTE_LINE = 0xfffffffeu;     // a null packet put in by stopping

// Cost of the tick's steps on the RP2350 at 240 MHz, in picoseconds.
static const ps_t COST_BEGIN = 600000;              // entry, scan reading, bookkeeping
static const ps_t COST_LINE = 50000;                // a line without a slot passed over
static const ps_t COST_MISSED = 170000;             // a line accounted as missed
static const ps_t COST_NULL = 350000;               // a null packet made
static const ps_t COST_ENCODE = 7000000;            // a sample packet made: about 1650 cycles
static const ps_t COST_COPY = 250000;               // 32 words into a slot
static const ps_t COST_END = 200000;
static const ps_t COST_COLD = 10700000;             // a seldom path's code fetched from flash: DVI_AUDIO_COST_COLD units

// ---------------------------------------------------------------------------
// Decoding, from the specification's definitions.
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

static int g_iTercValue[1024];

static void initSymbols()
{
    for (int i = 0; i < 1024; i++)
        g_iTercValue[i] = -1;
    for (int i = 0; i < 16; i++)
        g_iTercValue[bits10(TERC4_Q[i])] = i;
}

static unsigned int lane(uint32_t iWord, unsigned int iLane) { return (iWord >> (10u * iLane)) & 0x3ffu; }

// Remainder of the bit sequence, first bit the highest power, divided by
// x^8 + x^7 + x^6 + 1. A codeword with its parity appended leaves zero.
static unsigned int bchRemainder(const std::vector<int> & obBits)
{
    std::vector<int> obWork = obBits;
    static const int G[9] = { 1, 1, 1, 0, 0, 0, 0, 0, 1 };
    for (size_t i = 0; i + 8 < obWork.size(); i++)
        if (obWork[i])
            for (int j = 0; j < 9; j++)
                obWork[i + j] ^= G[j];
    unsigned int iRem = 0;
    for (size_t i = obWork.size() - 8; i < obWork.size(); i++)
        iRem = (iRem << 1) | (unsigned int)obWork[i];
    return iRem;
}

struct stPacket
{
    bool    bValid = false;
    uint8_t iHeader[3] = {};
    uint8_t iSub[4][7] = {};
};

static stPacket decodePacket(const uint32_t * pWords, bool bV, bool bH)
{
    stPacket stP;
    std::vector<int> obHeader;
    std::vector<int> obSub[4];
    const int iSync = (bV ? 2 : 0) | (bH ? 1 : 0);
    for (unsigned int k = 0; k < 32; k++)
    {
        const int i0 = g_iTercValue[lane(pWords[k], 0)];
        const int i1 = g_iTercValue[lane(pWords[k], 1)];
        const int i2 = g_iTercValue[lane(pWords[k], 2)];
        if (i0 < 0 || i1 < 0 || i2 < 0 || (i0 & 3) != iSync || ((i0 >> 3) & 1) != (k == 0 ? 0 : 1))
            return stP;
        obHeader.push_back((i0 >> 2) & 1);
        for (int i = 0; i < 4; i++)
        {
            obSub[i].push_back((i1 >> i) & 1);
            obSub[i].push_back((i2 >> i) & 1);
        }
    }
    if (bchRemainder(obHeader) != 0)
        return stP;
    for (int i = 0; i < 4; i++)
        if (bchRemainder(obSub[i]) != 0)
            return stP;
    for (int n = 0; n < 3; n++)
        for (int b = 0; b < 8; b++)
            stP.iHeader[n] |= (uint8_t)(obHeader[8 * n + b] << b);
    for (int i = 0; i < 4; i++)
        for (int n = 0; n < 7; n++)
            for (int b = 0; b < 8; b++)
                stP.iSub[i][n] |= (uint8_t)(obSub[i][8 * n + b] << b);
    stP.bValid = true;
    return stP;
}

static unsigned int ones(unsigned int iValue)
{
    unsigned int iCount = 0;
    for (; iValue; iValue >>= 1)
        iCount += iValue & 1u;
    return iCount;
}

// An Audio Sample packet as a sink reads it. False if malformed.
struct stSamples
{
    unsigned int iCount = 0;
    uint16_t iLeft[4] = {}, iRight[4] = {};
    bool bBlock[4] = {};
    unsigned int iC[4] = {};
};

static bool audioSamples(const stPacket & stP, stSamples & stS)
{
    if (stP.iHeader[0] != 0x02)
        return false;
    const unsigned int iPresent = stP.iHeader[1] & 0x0fu;
    if ((stP.iHeader[1] & 0xf0u) != 0 || (stP.iHeader[2] & 0x0fu) != 0)
        return false;
    if (iPresent != 0x0 && iPresent != 0x1 && iPresent != 0x3 && iPresent != 0x7 && iPresent != 0xf)
        return false;
    stS.iCount = ones(iPresent);
    for (unsigned int i = 0; i < 4; i++)
    {
        const uint8_t * pS = stP.iSub[i];
        if (i >= stS.iCount)
        {
            for (int n = 0; n < 7; n++)
                if (pS[n] != 0)
                    return false;
            if (stP.iHeader[2] & (0x10u << i))
                return false;
            continue;
        }
        if (pS[0] != 0 || pS[3] != 0)
            return false;
        stS.iLeft[i] = (uint16_t)(pS[1] | (pS[2] << 8));
        stS.iRight[i] = (uint16_t)(pS[4] | (pS[5] << 8));
        const unsigned int iVUCPL = pS[6] & 0x0fu;
        const unsigned int iVUCPR = pS[6] >> 4;
        if ((iVUCPL & 3u) != 0 || (iVUCPR & 3u) != 0)                  // valid, no user data
            return false;
        if ((ones(stS.iLeft[i]) + ones(iVUCPL)) % 2 != 0 || (ones(stS.iRight[i]) + ones(iVUCPR)) % 2 != 0)
            return false;
        if (((iVUCPL >> 2) & 1u) != ((iVUCPR >> 2) & 1u))
            return false;
        stS.iC[i] = (iVUCPL >> 2) & 1u;
        stS.bBlock[i] = (stP.iHeader[2] & (0x10u << i)) != 0;
    }
    return true;
}

static bool isNullPacket(const stPacket & stP)
{
    if (stP.iHeader[0] != 0 || stP.iHeader[1] != 0 || stP.iHeader[2] != 0)
        return false;
    for (int i = 0; i < 4; i++)
        for (int n = 0; n < 7; n++)
            if (stP.iSub[i][n] != 0)
                return false;
    return true;
}

// ---------------------------------------------------------------------------
// The frame, the FIFO and the command expander.
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

struct stIsland
{
    unsigned int iLine;
    int          iSlot;
    ps_t         iFetchStart;       // within the frame
    ps_t         iFetchEnd;
};

struct stRig
{
    rpDVIFrame obFrame;
    std::vector<uint32_t> obPool;
    std::vector<uint16_t> obPixels;
    recordingSink obSink;
    unsigned int iRate = 48000;
    bool bBars = false;

    uint32_t iListCount = 0;
    uint32_t iFirstRowEntry = 0;
    uint32_t iRows = 0;
    std::vector<ps_t> obEntryFrom;      // from this time in the frame the list channel reports entry + 1
    std::vector<stIsland> obIslands;    // audio slot islands, in time order
    ps_t iFrame = 0;

    bool build(bool bBarsIn, unsigned int iWidth, unsigned int iHeight, unsigned int iRateIn)
    {
        const stDVIMode & stMode = DVI_MODE_640x480_60;
        bBars = bBarsIn;
        iRate = iRateIn;
        const unsigned int iWords = bBars ? rpDVIFrame::audioFramingBarsWords(stMode) : rpDVIFrame::audioFramingPictureWords(stMode, iWidth, iHeight);
        if (iWords == 0)
            return false;
        obPool.assign(iWords, 0);
        obPixels.assign((size_t)iWidth * iHeight + 1, 0);
        obFrame.setAudioFraming(obPool.data(), iWords, iRate);
        const bool bBuilt = bBars ? obFrame.buildBars(stMode, obSink)
                                  : obFrame.buildPicture(stMode, obPixels.data(), iWidth, iHeight, iWidth, obSink);
        if (!bBuilt)
            return false;
        iListCount = (uint32_t)obSink.obPieces.size();
        iFirstRowEntry = 0;
        iRows = 0;
        for (uint32_t i = 0; i < iListCount; i++)
            if (obSink.obPieces[i].bPixels && iRows++ == 0)
                iFirstRowEntry = i;
        return model();
    }

    // Pop clock of every FIFO word, then the fetch clock: a word enters the FIFO
    // once the word eight before it has left.
    bool model()
    {
        struct stItem
        {
            uint32_t iEntry;
            const uint32_t * pWord;     // nullptr for a pixel
        };
        std::vector<stItem> obItems;
        std::vector<size_t> obEntryLast;
        for (uint32_t e = 0; e < iListCount; e++)
        {
            const stPiece & stP = obSink.obPieces[e];
            for (unsigned int i = 0; i < stP.iCount; i++)
                obItems.push_back({ e, stP.bPixels ? nullptr : stP.pWords + i });
            obEntryLast.push_back(obItems.size() - 1);
        }
        const size_t N = obItems.size();
        std::vector<int64_t> obPop(N, -1);
        struct stFound { size_t iFirst; int64_t iClock; };
        std::vector<stFound> obFound;
        size_t i = 0;
        int64_t t = 0;
        while (i < N)
        {
            if (obItems[i].pWord == nullptr)
                return false;
            const uint32_t iCmd = *obItems[i].pWord;
            const uint32_t iOp = iCmd & ~0xfffu;
            const unsigned int n = iCmd & 0xfffu;
            obPop[i] = t;
            if (iOp == rpDVIFrame::CMD_RAW_REPEAT || iOp == rpDVIFrame::CMD_TMDS_REPEAT)
            {
                if (i + 1 >= N)
                    return false;
                obPop[i + 1] = t;
                i += 2;
            }
            else if (iOp == rpDVIFrame::CMD_RAW || iOp == rpDVIFrame::CMD_TMDS)
            {
                if (i + n >= N)
                    return false;
                if (iOp == rpDVIFrame::CMD_RAW && n == 32)
                    obFound.push_back({ i + 1, t });
                for (unsigned int k = 0; k < n; k++)
                    obPop[i + 1 + k] = t + k;
                i += 1 + n;
            }
            else
                return false;
            t += n;
        }
        if (t != (int64_t)FRAME_CLOCKS)
            return false;
        auto fetch = [&](size_t j) -> int64_t {
            return j >= FIFO_WORDS ? obPop[j - FIFO_WORDS] : obPop[N + j - FIFO_WORDS] - (int64_t)FRAME_CLOCKS;
        };
        iFrame = (ps_t)FRAME_CLOCKS * PS_PER_PIXEL;
        obEntryFrom.assign(iListCount, 0);
        obEntryFrom[0] = (fetch(N - 1) - (int64_t)FRAME_CLOCKS) * PS_PER_PIXEL;
        for (uint32_t e = 1; e < iListCount; e++)
            obEntryFrom[e] = fetch(obEntryLast[e - 1]) * PS_PER_PIXEL;
        obIslands.clear();
        for (const stFound & stF : obFound)
        {
            int iSlot = -1;
            for (unsigned int s = 0; s < obFrame.audioSlotCount(); s++)
                if (obFrame.audioSlotWords(s) == obItems[stF.iFirst].pWord)
                    iSlot = (int)s;
            if (iSlot < 0)
                continue;                                   // a fixed island of the vertical back porch
            obIslands.push_back({ (unsigned int)(stF.iClock / LINE_CLOCKS), iSlot,
                                  fetch(stF.iFirst) * PS_PER_PIXEL, fetch(stF.iFirst + 31) * PS_PER_PIXEL });
        }
        return true;
    }

    // What the list channel's read address gives at time iTime.
    uint32_t nextEntry(ps_t iTime) const
    {
        const ps_t iIn = iTime - (iTime / iFrame) * iFrame;
        if (iIn >= iFrame + obEntryFrom[0])
            return 1;
        const size_t e = (size_t)(std::upper_bound(obEntryFrom.begin(), obEntryFrom.end(), iIn) - obEntryFrom.begin()) - 1;
        return (uint32_t)e + 1;
    }

    // The core set up as rpDVIAudio::start() does it.
    bool startCore(stDVIAudioCore & st, std::vector<uint8_t> & obBuffer) const
    {
        obBuffer.assign(audioBufferBytes(LINES), 0xa5);
        uint8_t iStatus[DVI_CHANNEL_STATUS_BYTES];
        if (!dviChannelStatus(iRate, iStatus))
            return false;
        audioInit(st, obBuffer.data(), LINES, iListCount, iFirstRowEntry, iRows);
        for (unsigned int iLine = 0; iLine < LINES; iLine++)
            audioSetLine(st, iLine, obFrame.audioLineSlot(iLine));
        for (unsigned int iSlot = 0; iSlot < obFrame.audioSlotCount(); iSlot++)
            audioSetSlot(st, iSlot, obFrame.audioSlotWords(iSlot));
        return audioConfigure(st, obFrame.audioSlotCount(), iRate, LINE_HZ, obFrame.audioVLevel(), obFrame.audioHLevel(), iStatus);
    }
};

// ---------------------------------------------------------------------------
// The simulation.
// ---------------------------------------------------------------------------

enum eSource { sourceRamp, sourceRampPauses, sourceRampFlood, sourceTone };

struct stConfig
{
    const char * szName;
    int iFrames;
    ps_t iSmallMax;                 // usual delay before a tick is served
    double fBigChance;              // chance of a long delay instead
    ps_t iBigMin;
    ps_t iBigMax;
    unsigned int iSeed;
    eSource eSrc;
    uint32_t iTone;
};

struct stWrite
{
    ps_t iFrom;
    ps_t iTo;
    unsigned int iLine;             // the frame line it was made for
    uint32_t iWords[32];
};

class audioSim
{
public:
    audioSim(stRig & obRig, const stConfig & stC) : m_obRig(obRig), m_stC(stC), m_obRng(stC.iSeed)
    {
        nullSlots(obRig);
        m_bOk = obRig.startCore(m_stCore, m_obBuffer);
        m_stCore.iTone = stC.iTone;
        m_obStatus.assign(DVI_CHANNEL_STATUS_BYTES, 0);
        dviChannelStatus(obRig.iRate, m_obStatus.data());
        resetSlots();
    }

    bool ok() const { return m_bOk; }

    // Every slot to a null packet, as the frame is built and as stopping leaves it.
    static void nullSlots(stRig & obRig)
    {
        stDVIPacket stNull;
        dviPacketNull(stNull);
        for (unsigned int s = 0; s < obRig.obFrame.audioSlotCount(); s++)
            dviPacketWords(stNull, obRig.obFrame.audioVLevel(), obRig.obFrame.audioHLevel(), obRig.obFrame.audioSlotWords(s));
    }

    void run(ps_t iStart)
    {
        if (!m_bOk)
            return;
        m_iNow = iStart;
        m_iTickAt = iStart + uniform(0, 3 * US);
        m_iProducerAt = iStart;
        m_iJudgeFrom = m_iTickAt + 3 * 31746 * 1000;
        m_iFirstWholeFrame = (m_iJudgeFrom / m_obRig.iFrame + 1);
        m_stCore.bRunning = true;
        runUntil(iStart + (ps_t)m_stC.iFrames * m_obRig.iFrame);
        // A frame served at once lets the core account for a hold-off the run
        // ended in, and the TV see the gap it left.
        m_bSettle = true;
        runUntil(m_iNow + m_obRig.iFrame);
        judgeUntil(m_iNow);
    }

    // Stop as rpDVIAudio::stop() does it, then two frames with nothing written.
    void stopAndWatch()
    {
        if (!m_bOk)
            return;
        m_stCore.bMuting = true;
        const ps_t iFrom = m_iNow;
        // stop() raises the tick at once: no gap is kept before that one.
        m_iTickAt = std::min(m_iTickAt, m_iNow + tickDelay());
        m_iLastTickEnd = -NEVER;
        while (!m_stCore.bMuted && m_iNow - iFrom < 5000 * US)
            step();
        iMuteUs = (m_iNow - iFrom) / US;
        m_stCore.bRunning = false;
        m_iTickAt = NEVER;
        m_iProducerAt = NEVER;
        m_iMuteDone = m_iNow;
        runUntil(m_iNow + 2 * m_obRig.iFrame);
        judgeUntil(m_iNow);
    }

    // Results.
    int iIslands = 0;               // islands judged
    int iTorn = 0;                  // written while the list channel fetched them
    int iInvalid = 0;               // failed the decoder
    int iWrongLine = 0;             // written for another line than the one that sent them
    int iWrongWrite = 0;            // written outside the slot's window
    int iHeldNull = 0;              // a null packet sent again
    int iHeldAudio = 0;             // a packet with samples sent again
    int iAfterMute = 0;             // samples sent after stopping
    int64_t iSamples = 0;           // frames sent
    int64_t iSilence = 0;           // frames of silence sent
    int64_t iBackward = 0;          // frames not after the frame before
    int64_t iGap = 0;               // ramp values skipped
    int64_t iBadTone = 0;
    int64_t iBadStatus = 0;
    int64_t iOffered = 0, iAccepted = 0;
    int iBadAccept = 0;
    int iWholeFrames = 0;
    int iWrongFrameCount = 0;       // whole frames that did not carry the frame's samples
    ps_t iTickMax = 0;
    ps_t iGapMin = NEVER;
    int64_t iMuteUs = 0;
    std::vector<int> obFrameSamples;
    const stDVIAudioCore & core() const { return m_stCore; }

private:
    ps_t uniform(ps_t iLow, ps_t iHigh) { return std::uniform_int_distribution<ps_t>(iLow, iHigh)(m_obRng); }
    ps_t tickDelay()
    {
        if (m_bSettle)
            return uniform(0, 3 * US);
        if (std::uniform_real_distribution<double>(0.0, 1.0)(m_obRng) < m_stC.fBigChance)
            return uniform(m_stC.iBigMin, m_stC.iBigMax);
        return uniform(0, m_stC.iSmallMax);
    }

    void resetSlots()
    {
        const unsigned int iSlots = m_obRig.obFrame.audioSlotCount();
        m_obWrites.assign(iSlots, {});
        m_obLastFetchEnd.assign(iSlots, -NEVER);
        for (unsigned int s = 0; s < iSlots; s++)
        {
            stWrite stW = { -NEVER - 1, -NEVER - 1, 0xffffffffu, {} };     // never fresh
            std::memcpy(stW.iWords, m_obRig.obFrame.audioSlotWords(s), sizeof(stW.iWords));
            m_obWrites[s].push_back(stW);
        }
        m_iIsland = 0;
        m_iIslandFrame = -1;
    }

    void runUntil(ps_t iUntil)
    {
        while (m_iNow < iUntil)
            step(iUntil);
    }

    void step(ps_t iUntil = NEVER)
    {
        const ps_t iNext = std::min({ m_iTickAt, m_iProducerAt, iUntil });
        if (iNext == NEVER)
        {
            m_iNow = iUntil;
            return;
        }
        m_iNow = iNext;
        judgeUntil(m_iNow);
        if (iNext == m_iTickAt)
            tick();
        else if (iNext == m_iProducerAt)
            produce();
    }

    // ---- the producer
    void produce()
    {
        m_iProducerAt = m_iNow + 250 * US;
        if (m_stC.eSrc == sourceRampPauses && (m_iNow / (20000 * US)) % 4 == 3)
            return;                                         // 20 ms in 80 with no samples
        const unsigned int iOffer = m_stC.eSrc == sourceRampFlood ? 1000u : DVI_AUDIO_PCM_FRAMES;
        std::vector<int16_t> obStereo(2u * iOffer);
        for (unsigned int i = 0; i < iOffer; i++)
        {
            const uint32_t iValue = m_iRamp + i;
            obStereo[2u * i] = (int16_t)(uint16_t)iValue;
            obStereo[2u * i + 1u] = (int16_t)(uint16_t)(iValue >> 16);
        }
        const uint32_t iFree = DVI_AUDIO_PCM_FRAMES - (m_stCore.iPcmHead - m_stCore.iPcmTail);
        const uint32_t iTaken = audioPcmWrite(m_stCore, obStereo.data(), iOffer);
        if (iTaken != std::min(iOffer, iFree))
            iBadAccept++;
        iOffered += iOffer;
        iAccepted += iTaken;
        m_iRamp += iTaken;
    }

    // ---- the feeder
    void tick()
    {
        const ps_t iStart = m_iNow;
        if (m_iLastTickEnd > -NEVER)
            iGapMin = std::min(iGapMin, iStart - m_iLastTickEnd);
        ps_t t = iStart;
        if (!m_stCore.bRunning)
        {
            m_iTickAt = NEVER;
            return;
        }
        const bool bFirst = !m_stCore.bStarted;
        uint32_t iNextBefore = m_stCore.iNext;
        const bool bWrite = audioTickBegin(m_stCore, m_obRig.nextEntry(t));
        t += COST_BEGIN;
        if (!bWrite || (!bFirst && m_stCore.iTickCost >= DVI_AUDIO_COST_COLD))
            t += COST_COLD;
        if (bWrite && !bFirst)
            t += COST_MISSED * (ps_t)(uint32_t)(m_stCore.iNext - iNextBefore);
        if (!bWrite)
            recordMuteWrites(t);
        if (bWrite)
        {
            uint32_t iWords[32];
            for (;;)
            {
                iNextBefore = m_stCore.iNext;
                const uint32_t iCostBefore = m_stCore.iTickCost;
                const int iSlot = audioTickPrepare(m_stCore, iWords);
                t += COST_LINE * (ps_t)(uint32_t)(m_stCore.iNext - iNextBefore);
                if (iSlot < 0)
                    break;
                t += m_stCore.iTickCost - iCostBefore == DVI_AUDIO_COST_SAMPLES ? COST_ENCODE : COST_NULL;
                const unsigned int iLine = m_stCore.iNextLine;
                const uint32_t iLateBefore = m_stCore.iLate;
                iNextBefore = m_stCore.iNext;
                audioTickCommit(m_stCore, m_obRig.nextEntry(t), iWords);
                if (m_stCore.iLate == iLateBefore)
                {
                    stWrite stW = { t, t + COST_COPY, iLine, {} };
                    std::memcpy(stW.iWords, m_obRig.obFrame.audioSlotWords((unsigned int)iSlot), sizeof(stW.iWords));
                    checkWindow((unsigned int)iSlot, stW);
                    m_obWrites[iSlot].push_back(stW);
                    if (m_obWrites[iSlot].size() > 16)
                        m_obWrites[iSlot].pop_front();
                    t += COST_COPY;
                }
                else
                    t += COST_MISSED * (ps_t)(uint32_t)(m_stCore.iNext - iNextBefore);
            }
        }
        const ps_t iGap = (ps_t)audioTickEnd(m_stCore) * US;
        t += COST_END;
        iTickMax = std::max(iTickMax, t - iStart);
        m_iLastTickEnd = t;
        m_iTickAt = iGap ? t + iGap + tickDelay() : NEVER;
        m_iNow = std::max(m_iNow, t);
    }

    // Slots stopping has put a null packet in.
    void recordMuteWrites(ps_t & t)
    {
        for (unsigned int s = 0; s < m_obWrites.size(); s++)
        {
            const uint32_t * pSlot = m_obRig.obFrame.audioSlotWords(s);
            if (std::memcmp(pSlot, m_obWrites[s].back().iWords, sizeof(stWrite::iWords)) == 0)
                continue;
            stWrite stW = { t, t + COST_COPY, MUTE_LINE, {} };
            std::memcpy(stW.iWords, pSlot, sizeof(stW.iWords));
            checkWindow(s, stW);
            m_obWrites[s].push_back(stW);
            if (m_obWrites[s].size() > 16)
                m_obWrites[s].pop_front();
            t += COST_COPY;
        }
    }

    // The write must not overlap a fetch of the slot, and the next island on
    // the slot after it must be the line it was made for.
    void checkWindow(unsigned int iSlot, const stWrite & stW)
    {
        const std::vector<stIsland> & obIs = m_obRig.obIslands;
        const ps_t iFrameLen = m_obRig.iFrame;
        const ps_t iBase = (stW.iFrom / iFrameLen) * iFrameLen;
        int iNextLine = -1;
        for (int iPass = -1; iPass <= 1 && iNextLine < 0; iPass++)
            for (const stIsland & stI : obIs)
            {
                if (stI.iSlot != (int)iSlot)
                    continue;
                const ps_t iFrom = iBase + iPass * iFrameLen + stI.iFetchStart;
                const ps_t iTo = iBase + iPass * iFrameLen + stI.iFetchEnd;
                if (iTo <= stW.iFrom)
                    continue;
                if (iFrom < stW.iTo)
                {
                    iWrongWrite++;
                    return;
                }
                iNextLine = (int)stI.iLine;
                break;
            }
        if (stW.iLine != MUTE_LINE && iNextLine != (int)stW.iLine)
            iWrongWrite++;
    }

    // ---- the TV
    void judgeUntil(ps_t iTime)
    {
        const std::vector<stIsland> & obIs = m_obRig.obIslands;
        for (;;)
        {
            if (m_iIslandFrame < 0)
            {
                m_iIslandFrame = m_iJudgeFrom / m_obRig.iFrame;
                m_iIsland = 0;
                while (m_iIslandFrame * m_obRig.iFrame + obIs[m_iIsland].iFetchStart < m_iJudgeFrom)
                    if (++m_iIsland == obIs.size())
                    {
                        m_iIsland = 0;
                        m_iIslandFrame++;
                    }
            }
            const stIsland & stI = obIs[m_iIsland];
            const ps_t iBase = m_iIslandFrame * m_obRig.iFrame;
            if (iBase + stI.iFetchEnd > iTime)
                return;
            judge(stI, iBase + stI.iFetchStart, iBase + stI.iFetchEnd);
            if (++m_iIsland == obIs.size())
            {
                if (m_iIslandFrame >= m_iFirstWholeFrame && m_iMuteDone == NEVER)
                {
                    iWholeFrames++;
                    obFrameSamples.push_back(m_iFrameSamples);
                }
                m_iFrameSamples = 0;
                m_iIsland = 0;
                m_iIslandFrame++;
            }
        }
    }

    void judge(const stIsland & stI, ps_t iFrom, ps_t iTo)
    {
        iIslands++;
        std::deque<stWrite> & obW = m_obWrites[stI.iSlot];
        const stWrite * pHeld = nullptr;
        bool bTorn = false;
        for (const stWrite & stW : obW)
        {
            if (stW.iTo <= iFrom)
                pHeld = &stW;
            else if (stW.iFrom <= iTo)
                bTorn = true;
        }
        const ps_t iPrevEnd = m_obLastFetchEnd[stI.iSlot];
        m_obLastFetchEnd[stI.iSlot] = iTo;
        if (bTorn)
        {
            iTorn++;
            return;
        }
        if (pHeld == nullptr)
        {
            iInvalid++;                                     // history too short to tell
            return;
        }
        const stPacket stP = decodePacket(pHeld->iWords, m_obRig.obFrame.audioVLevel(), m_obRig.obFrame.audioHLevel());
        stSamples stS;
        const bool bNull = stP.bValid && isNullPacket(stP);
        if (!stP.bValid || (!bNull && !audioSamples(stP, stS)))
        {
            iInvalid++;
            return;
        }
        const bool bFresh = pHeld->iFrom >= iPrevEnd;
        if (bFresh && pHeld->iLine == MUTE_LINE && !bNull)
        {
            iInvalid++;
            return;
        }
        if (bFresh && pHeld->iLine != stI.iLine && pHeld->iLine != MUTE_LINE)
        {
            iWrongLine++;
            return;
        }
        if (!bFresh)
        {
            if (bNull)
                iHeldNull++;
            else
                iHeldAudio++;
        }
        if (bNull)
            return;
        if (m_iMuteDone != NEVER && iFrom > m_iMuteDone)
            iAfterMute++;
        for (unsigned int i = 0; i < stS.iCount; i++)
            sample(stS, i, bFresh);
    }

    void sample(const stSamples & stS, unsigned int i, bool bFresh)
    {
        iSamples++;
        if (bFresh)
            m_iFrameSamples++;
        // Channel status, in order, from the first block start on.
        if (bFresh)
        {
            if (stS.bBlock[i])
            {
                if (m_iIec >= 0 && m_iIec != 0)
                    iBadStatus++;
                m_iIec = 0;
            }
            if (m_iIec >= 0)
            {
                const unsigned int iBit = (m_obStatus[(unsigned)m_iIec >> 3] >> ((unsigned)m_iIec & 7u)) & 1u;
                if (stS.iC[i] != iBit)
                    iBadStatus++;
                m_iIec = (m_iIec + 1) % 192;
            }
        }
        const uint16_t iLeft = stS.iLeft[i];
        const uint16_t iRight = stS.iRight[i];
        if (m_stC.eSrc == sourceTone)
        {
            toneSample(iLeft, iRight);
            return;
        }
        if (iLeft == 0 && iRight == 0)
        {
            if (bFresh)
                iSilence++;
            return;
        }
        const int64_t iValue = (int64_t)iLeft | ((int64_t)iRight << 16);
        if (iValue < m_iExpect)
            iBackward++;
        else
        {
            if (m_iExpect > 0)
                iGap += iValue - m_iExpect;
            m_iExpect = iValue + 1;
        }
    }

    void toneSample(uint16_t iLeft, uint16_t iRight)
    {
        const bool bL = (m_stC.iTone & DVI_AUDIO_TONE_LEFT) != 0;
        const bool bR = (m_stC.iTone & DVI_AUDIO_TONE_RIGHT) != 0;
        const int16_t iOn = (int16_t)(bL ? iLeft : iRight);
        if ((!bL && iLeft != 0) || (!bR && iRight != 0) || (bL && bR && iLeft != iRight))
        {
            iBadTone++;
            return;
        }
        m_obToneSeen.push_back(iOn);
        // At 48 kHz the phase is found from the first three samples, then every
        // sample is the next step of the table; at other rates every sample is
        // a step of the table.
        if (m_obRig.iRate != 48000)
        {
            bool bStep = false;
            for (int p = 0; p < (int)DVI_AUDIO_TONE_STEPS; p++)
                bStep = bStep || DVI_AUDIO_TONE[p] == iOn;
            if (!bStep)
                iBadTone++;
            return;
        }
        if (m_iTonePhase < 0)
        {
            if (m_obToneSeen.size() < 3)
                return;
            for (int p = 0; p < (int)DVI_AUDIO_TONE_STEPS; p++)
                if (DVI_AUDIO_TONE[p] == m_obToneSeen[0] && DVI_AUDIO_TONE[(p + 1) % 48] == m_obToneSeen[1] &&
                    DVI_AUDIO_TONE[(p + 2) % 48] == m_obToneSeen[2])
                    m_iTonePhase = (p + 3) % 48;
            if (m_iTonePhase < 0)
                iBadTone++;
            return;
        }
        if (DVI_AUDIO_TONE[m_iTonePhase] != iOn)
            iBadTone++;
        m_iTonePhase = (m_iTonePhase + 1) % 48;
    }

    stRig & m_obRig;
    const stConfig & m_stC;
    std::mt19937 m_obRng;
    bool m_bOk = false;
    stDVIAudioCore m_stCore;
    std::vector<uint8_t> m_obBuffer;
    std::vector<uint8_t> m_obStatus;

    ps_t m_iNow = 0;
    ps_t m_iTickAt = NEVER;
    ps_t m_iProducerAt = NEVER;
    ps_t m_iLastTickEnd = -NEVER;
    ps_t m_iJudgeFrom = 0;
    ps_t m_iMuteDone = NEVER;
    int64_t m_iFirstWholeFrame = 0;
    uint32_t m_iRamp = 1;
    bool m_bSettle = false;

    std::vector<std::deque<stWrite>> m_obWrites;
    std::vector<ps_t> m_obLastFetchEnd;
    size_t m_iIsland = 0;
    int64_t m_iIslandFrame = -1;
    int m_iFrameSamples = 0;
    int m_iIec = -1;
    int64_t m_iExpect = 0;
    std::vector<int16_t> m_obToneSeen;
    int m_iTonePhase = -1;
};

// ---------------------------------------------------------------------------
// Unit tests.
// ---------------------------------------------------------------------------

// The core's encoder against rpDVIPacket, and against the decoder here.
static void testEncoder()
{
    std::mt19937 obRng(7);
    std::vector<uint8_t> obBuffer(audioBufferBytes(LINES));
    for (unsigned int iRate : { 48000u, 44100u })
        for (int iSync = 0; iSync < 4; iSync++)
        {
            stDVIAudioCore st;
            audioInit(st, obBuffer.data(), LINES, LINES, 0, 0);
            uint32_t iWords[32];
            audioSetSlot(st, 0, iWords);
            for (unsigned int iLine = 45; iLine < LINES; iLine++)
                audioSetLine(st, iLine, (int)((iLine - 45) % 8));
            for (unsigned int s = 0; s < 8; s++)
                audioSetSlot(st, s, iWords);
            uint8_t iStatus[DVI_CHANNEL_STATUS_BYTES];
            CHECK(dviChannelStatus(iRate, iStatus));
            const bool bV = (iSync & 2) != 0, bH = (iSync & 1) != 0;
            CHECK(audioConfigure(st, 8, iRate, LINE_HZ, bV, bH, iStatus));

            uint32_t iNull[32], iNullRef[32];
            audioNullWords(st, iNull);
            stDVIPacket stNull;
            dviPacketNull(stNull);
            dviPacketWords(stNull, bV, bH, iNullRef);
            CHECK(std::memcmp(iNull, iNullRef, sizeof(iNull)) == 0);

            for (int iTrial = 0; iTrial < 400; iTrial++)
            {
                int16_t iStereo[8];
                for (int i = 0; i < 8; i++)
                    iStereo[i] = (int16_t)(obRng() & 0xffff);
                const unsigned int iCount = 1 + obRng() % 4;
                const unsigned int iFrame = iTrial < 8 ? 188 + iTrial % 4 : obRng() % 192;
                uint32_t iMine[32], iRef[32];
                const unsigned int iAfter = audioEncode(st, iStereo, iCount, iFrame, iMine);
                stDVIPacket stRef;
                const unsigned int iAfterRef = dviPacketAudio(stRef, iStereo, iCount, iFrame, iStatus);
                dviPacketWords(stRef, bV, bH, iRef);
                CHECK(iAfter == iAfterRef);
                CHECK(std::memcmp(iMine, iRef, sizeof(iMine)) == 0);

                const stPacket stP = decodePacket(iMine, bV, bH);
                stSamples stS;
                CHECK(stP.bValid && audioSamples(stP, stS));
                CHECK(stS.iCount == iCount);
                for (unsigned int i = 0; i < iCount && i < stS.iCount; i++)
                {
                    const unsigned int f = (iFrame + i) % 192;
                    CHECK(stS.iLeft[i] == (uint16_t)iStereo[2 * i] && stS.iRight[i] == (uint16_t)iStereo[2 * i + 1]);
                    CHECK(stS.bBlock[i] == (f == 0));
                    CHECK(stS.iC[i] == ((iStatus[f >> 3] >> (f & 7)) & 1u));
                }
            }
        }
}

// The cadence of a frame's line map: samples per frame, at most four a
// packet, never ahead of what has accrued, and how long the backlog after the
// lines without a slot takes to drain.
static void testCadence(const char * szName, const stRig & obRig, unsigned int iRate, unsigned int iPerFrame)
{
    std::vector<uint8_t> obLine(LINES);
    for (unsigned int iLine = 0; iLine < LINES; iLine++)
        obLine[iLine] = (uint8_t)(obRig.obFrame.audioLineSlot(iLine) + 1);
    CHECK(audioCadenceBuild(obLine.data(), LINES, iRate, LINE_HZ));

    unsigned int iSent = 0;
    bool bOnlySlots = true, bAtMostFour = true;
    for (unsigned int iLine = 0; iLine < LINES; iLine++)
    {
        const unsigned int iCount = obLine[iLine] >> DVI_AUDIO_LINE_COUNT_SHIFT;
        iSent += iCount;
        bAtMostFour = bAtMostFour && iCount <= 4;
        bOnlySlots = bOnlySlots && (iCount == 0 || obRig.obFrame.audioLineSlot(iLine) >= 0);
    }
    CHECK(iSent == iPerFrame);
    CHECK(bAtMostFour);
    CHECK(bOnlySlots);

    // Accrued samples, exact: iRate / LINE_HZ a line from the frame's start,
    // three frames in a row.
    int64_t iBacklogMax = -1, iBacklogMin = INT64_MAX;
    unsigned int iDrainMax = 0;
    unsigned int iLastBlank = 0;
    for (unsigned int iLine = 0; iLine < LINES; iLine++)
        if (obRig.obFrame.audioLineSlot(iLine) < 0)
            iLastBlank = iLine;
    int64_t iSentAll = 0;
    for (unsigned int iPass = 0; iPass < 3; iPass++)
        for (unsigned int iLine = 0; iLine < LINES; iLine++)
        {
            const int64_t iAt = (int64_t)iPass * LINES + iLine + 1;
            const int64_t iAccrued = iAt * iRate / LINE_HZ;
            iSentAll += obLine[iLine] >> DVI_AUDIO_LINE_COUNT_SHIFT;
            const int64_t iBacklog = iAccrued - iSentAll;
            if (iPass == 0)
                continue;
            iBacklogMax = std::max(iBacklogMax, iBacklog);
            iBacklogMin = std::min(iBacklogMin, iBacklog);
            if (iLine > iLastBlank && iBacklog >= 4 + 4)
                iDrainMax = std::max(iDrainMax, iLine - iLastBlank);
        }
    CHECK(iBacklogMin >= -4);                               // never sends a packet before it has accrued
    CHECK(iBacklogMax <= 12);                               // blanking lines carry samples too
    CHECK(iDrainMax <= 200);
    std::printf("cadence %-8s %5u Hz: %u frames a frame, backlog %lld to %lld frames, back under 8 within %u lines of the last blank line\n",
                szName, iRate, iSent, (long long)iBacklogMin, (long long)iBacklogMax, iDrainMax + 1);
}

// audioScanLine() against the modelled list channel, over the time the list
// channel reports each entry: islands of lines before the scan line have been
// fetched when the entry is first reported, islands after it start no sooner
// than 3 us after it is last reported. Returns that margin's minimum.
static ps_t testScanLine(const char * szName, const stRig & obRig)
{
    std::vector<uint8_t> obBuffer;
    stDVIAudioCore st;
    CHECK(obRig.startCore(st, obBuffer));
    const ps_t iFrame = obRig.iFrame;
    ps_t iMargin = NEVER;
    int iBad = 0;
    for (uint32_t e = 0; e < obRig.iListCount; e++)
    {
        const ps_t iFrom = iFrame + obRig.obEntryFrom[e];
        const ps_t iTo = e + 1 < obRig.iListCount ? iFrame + obRig.obEntryFrom[e + 1] : 2 * iFrame + obRig.obEntryFrom[0];
        const int64_t iScan = (int64_t)LINES + audioScanLine(st, e + 1);
        for (int64_t iCopy = 0; iCopy < 3; iCopy++)
            for (const stIsland & stI : obRig.obIslands)
            {
                const int64_t iLine = iCopy * LINES + stI.iLine;
                const ps_t iStart = iCopy * iFrame + stI.iFetchStart;
                const ps_t iEnd = iCopy * iFrame + stI.iFetchEnd;
                if (iLine < iScan && iEnd > iFrom)
                    iBad++;
                if (iLine > iScan)
                {
                    iMargin = std::min(iMargin, iStart - iTo);
                    if (iStart < iTo + 3 * US)
                        iBad++;
                }
            }
    }
    CHECK(iBad == 0);
    // Rewinding reads as the last entry.
    CHECK(audioScanLine(st, 0) == audioScanLine(st, obRig.iListCount));
    std::printf("scan    %-8s the next island after the scan line starts %.1f us or more after the entry is left\n",
                szName, (double)iMargin / US);
    return iMargin;
}

// The islands found in the list against the frame's line map, and the window
// the core keeps.
static void testLayout(const char * szName, const stRig & obRig, uint32_t iAheadExpected)
{
    std::vector<int> obSeen(LINES, -1);
    for (const stIsland & stI : obRig.obIslands)
    {
        CHECK(obSeen[stI.iLine] < 0);
        obSeen[stI.iLine] = stI.iSlot;
    }
    for (unsigned int iLine = 0; iLine < LINES; iLine++)
        CHECK(obSeen[iLine] == obRig.obFrame.audioLineSlot(iLine));
    std::vector<uint8_t> obBuffer;
    stDVIAudioCore st;
    CHECK(obRig.startCore(st, obBuffer));
    CHECK(st.iAhead == iAheadExpected);
    std::printf("layout  %-8s %u entries, %u slots, writes %u lines past the scan line\n",
                szName, obRig.iListCount, obRig.obFrame.audioSlotCount(), st.iAhead);
}

static void testPcm()
{
    std::vector<uint8_t> obBuffer(audioBufferBytes(LINES));
    stDVIAudioCore st;
    audioInit(st, obBuffer.data(), LINES, LINES, 0, 0);
    int16_t iIn[2 * 300], iOut[2 * 300];
    for (int i = 0; i < 600; i++)
        iIn[i] = (int16_t)(i * 37 - 5000);
    CHECK(audioPcmWrite(st, iIn, 100) == 100);
    CHECK(audioPcmWrite(st, iIn + 200, 100) == DVI_AUDIO_PCM_FRAMES - 100);
    CHECK(audioPcmTake(st, iOut, 60) == 60);
    CHECK(std::memcmp(iOut, iIn, 60 * 4) == 0);
    CHECK(audioPcmWrite(st, iIn, 300) == 60);               // wraps
    CHECK(audioPcmTake(st, iOut, 300) == DVI_AUDIO_PCM_FRAMES);
    CHECK(std::memcmp(iOut, iIn + 120, 40 * 4) == 0);
    CHECK(std::memcmp(iOut + 80, iIn + 200, 28 * 4) == 0);
    CHECK(std::memcmp(iOut + 136, iIn, 60 * 4) == 0);
    // Short of samples: what is there, then silence, counted.
    CHECK(audioPcmWrite(st, iIn, 2) == 2);
    int16_t iFour[8];
    st.iTone = 0;
    audioTake(st, iFour, 4);
    CHECK(iFour[0] == iIn[0] && iFour[3] == iIn[3] && iFour[4] == 0 && iFour[7] == 0);
    CHECK(st.iUnderruns == 2);
}

// ---------------------------------------------------------------------------

static void report(const stConfig & stC, const audioSim & obSim)
{
    const stDVIAudioCore & st = obSim.core();
    std::printf("%-9s islands %d | torn %d, invalid %d, wrong line %d, wrong write %d | held null %d, held audio %d | "
                "frames %lld, silence %lld, backward %lld, gap %lld | core: packets %u, late %u, repeats %u, lost %u, "
                "underruns %u | tick max %.1f us, gap min %.1f us\n",
                stC.szName, obSim.iIslands, obSim.iTorn, obSim.iInvalid, obSim.iWrongLine, obSim.iWrongWrite,
                obSim.iHeldNull, obSim.iHeldAudio, (long long)obSim.iSamples, (long long)obSim.iSilence,
                (long long)obSim.iBackward, (long long)obSim.iGap, (unsigned)st.iPackets, (unsigned)st.iLate,
                (unsigned)st.iRepeats, (unsigned)st.iLost, (unsigned)st.iUnderruns,
                (double)obSim.iTickMax / US, obSim.iGapMin == NEVER ? 0.0 : (double)obSim.iGapMin / US);
}

// What holds in every run: the picture's slots are only ever written whole,
// in their window, with packets that decode, for the line that sends them.
static void checkAlways(const audioSim & obSim)
{
    CHECK(obSim.iIslands > 0);
    CHECK(obSim.iTorn == 0);
    CHECK(obSim.iInvalid == 0);
    CHECK(obSim.iWrongLine == 0);
    CHECK(obSim.iWrongWrite == 0);
    CHECK(obSim.iBadStatus == 0);
    CHECK(obSim.iBadAccept == 0);
    CHECK(obSim.iTickMax <= 25 * US);
    CHECK(obSim.iGapMin >= (ps_t)DVI_AUDIO_TICK_GAP_US * US);
}

// Every line the TV was given late is one the core counted, and every held
// packet with samples a repeat it counted; every ramp value skipped was
// dropped on a counted line. A few lines at the end of the run may be counted
// by one side only.
static void checkCounted(const audioSim & obSim)
{
    const stDVIAudioCore & st = obSim.core();
    const int64_t iHeld = obSim.iHeldNull + obSim.iHeldAudio;
    CHECK(std::llabs(iHeld - (int64_t)st.iLate) <= 2);
    CHECK(std::llabs((int64_t)obSim.iHeldAudio - (int64_t)st.iRepeats) <= 2);
    CHECK(std::llabs(obSim.iGap - (int64_t)st.iLost) <= 8);
    CHECK(obSim.iBackward <= 4 * (int64_t)obSim.iHeldAudio);
}

static void runRig(const char * szRig, stRig & obRig, unsigned int iPerFrame)
{
    std::printf("-- %s, %u Hz\n", szRig, obRig.iRate);

    // Quiet: served within 3 us.
    for (unsigned int iSeed = 1; iSeed <= 3; iSeed++)
    {
        const stConfig stC = { "quiet", 120, 3 * US, 0.0, 0, 0, iSeed, sourceRamp, 0 };
        audioSim obSim(obRig, stC);
        CHECK(obSim.ok());
        obSim.run((ps_t)iSeed * 7777777 * 1000);
        report(stC, obSim);
        checkAlways(obSim);
        CHECK(obSim.core().iLate == 0);
        CHECK(obSim.iHeldNull == 0 && obSim.iHeldAudio == 0);
        CHECK(obSim.iBackward == 0 && obSim.iGap == 0);
        CHECK(obSim.iSilence == 0 && obSim.core().iUnderruns == 0);
        CHECK(obSim.iWholeFrames >= stC.iFrames - 3);
        bool bExact = true;
        for (int iCount : obSim.obFrameSamples)
            bExact = bExact && iCount == (int)iPerFrame;
        CHECK(bExact);
    }

    // Busy: one tick in twenty is held off 80 to 140 us by other interrupts,
    // the rest up to 70 us.
    for (unsigned int iSeed = 11; iSeed <= 13; iSeed++)
    {
        const stConfig stC = { "busy", 120, 70 * US, 0.05, 80 * US, 140 * US, iSeed, sourceRamp, 0 };
        audioSim obSim(obRig, stC);
        obSim.run((ps_t)iSeed * 3333333 * 1000);
        report(stC, obSim);
        checkAlways(obSim);
        checkCounted(obSim);
        CHECK(obSim.core().iLate == 0);
        CHECK(obSim.iBackward == 0 && obSim.iGap == 0 && obSim.iSilence == 0);
    }

    // Blocked: now and then the tick is not served for 1 to 3 ms.
    for (unsigned int iSeed = 21; iSeed <= 23; iSeed++)
    {
        const stConfig stC = { "blocked", 120, 70 * US, 0.004, 1000 * US, 3000 * US, iSeed, sourceRamp, 0 };
        audioSim obSim(obRig, stC);
        obSim.run((ps_t)iSeed * 1111111 * 1000);
        report(stC, obSim);
        checkAlways(obSim);
        checkCounted(obSim);
        CHECK(obSim.core().iLate > 0);
    }

    // The source stops for 20 ms in every 80: silence, every frame of it counted.
    {
        const stConfig stC = { "underrun", 120, 3 * US, 0.0, 0, 0, 31, sourceRampPauses, 0 };
        audioSim obSim(obRig, stC);
        obSim.run(12345 * US);
        report(stC, obSim);
        checkAlways(obSim);
        CHECK(obSim.core().iLate == 0);
        CHECK(obSim.iSilence > 0);
        CHECK(std::llabs(obSim.iSilence - (int64_t)obSim.core().iUnderruns) <= 8);
        CHECK(obSim.iBackward == 0 && obSim.iGap == 0);
    }

    // The source offers far more than fits: what is taken is sent, in order.
    {
        const stConfig stC = { "overfull", 120, 3 * US, 0.0, 0, 0, 41, sourceRampFlood, 0 };
        audioSim obSim(obRig, stC);
        obSim.run(54321 * US);
        report(stC, obSim);
        checkAlways(obSim);
        CHECK(obSim.iAccepted < obSim.iOffered);
        CHECK(obSim.core().iLate == 0 && obSim.core().iUnderruns == 0);
        CHECK(obSim.iBackward == 0 && obSim.iGap == 0 && obSim.iSilence == 0);
    }

    // The tone on each side, busy.
    for (uint32_t iTone : { DVI_AUDIO_TONE_LEFT, DVI_AUDIO_TONE_RIGHT, DVI_AUDIO_TONE_LEFT | DVI_AUDIO_TONE_RIGHT })
    {
        const stConfig stC = { "tone", 30, 70 * US, 0.05, 80 * US, 140 * US, 50 + iTone, sourceTone, iTone };
        audioSim obSim(obRig, stC);
        obSim.run(999 * US);
        report(stC, obSim);
        checkAlways(obSim);
        CHECK(obSim.core().iLate == 0);
        CHECK(obSim.iBadTone == 0);
        CHECK(obSim.iSamples > 20000);
    }

    // Stop: every slot ends up null, nothing torn, no samples after; then a
    // fresh start on the same frame.
    for (int iRound = 0; iRound < 2; iRound++)
    {
        const stConfig stC = { "stop", 20, 70 * US, 0.05, 80 * US, 140 * US, 60u + iRound, sourceRamp, 0 };
        audioSim obSim(obRig, stC);
        obSim.run(4444 * US + iRound * 777 * US);
        obSim.stopAndWatch();
        report(stC, obSim);
        checkAlways(obSim);
        CHECK(obSim.core().bMuted);
        CHECK(obSim.iMuteUs <= 1000);
        CHECK(obSim.iAfterMute == 0);
        for (unsigned int s = 0; s < obRig.obFrame.audioSlotCount(); s++)
        {
            const stPacket stP = decodePacket(obRig.obFrame.audioSlotWords(s), obRig.obFrame.audioVLevel(), obRig.obFrame.audioHLevel());
            CHECK(stP.bValid && isNullPacket(stP));
        }
    }
}

int main()
{
    initSymbols();
    testEncoder();
    testPcm();

    stRig obPicture, obBars, obPicture44, obTall;
    const bool bBuilt = obPicture.build(false, 480, 320, 48000) && obBars.build(true, 0, 0, 48000) &&
                        obPicture44.build(false, 480, 320, 44100) && obTall.build(false, 640, 476, 48000);
    CHECK(bBuilt);
    if (!bBuilt)
    {
        std::printf("the frames did not build\n");
        return 1;
    }

    testLayout("480x320", obPicture, 7);
    testLayout("bars", obBars, 7);
    testLayout("640x476", obTall, 7);
    testScanLine("480x320", obPicture);
    testScanLine("bars", obBars);
    testScanLine("640x476", obTall);
    testCadence("480x320", obPicture, 48000, 800);
    testCadence("480x320", obPicture, 44100, 735);
    testCadence("bars", obBars, 48000, 800);
    testCadence("bars", obBars, 44100, 735);

    runRig("480x320 picture", obPicture, 800);
    runRig("colour bars", obBars, 800);
    runRig("480x320 picture", obPicture44, 735);

    std::printf("%d checks, %d failures\n", g_iChecks, g_iFailures);
    return g_iFailures ? 1 : 0;
}
