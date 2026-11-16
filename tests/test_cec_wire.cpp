// Host tests for rmpLib/fwCECWire -- the CEC bit engine -- on a simulated
// line. The line is low while anything on it pulls. On it sit engines, each
// with its own handler delay drawn per interrupt, and devices that are not
// the engine: a scripted sender that plays a waveform from a table of times,
// and an acknowledger that follows the line edge by edge. Every change of the
// line is recorded and decoded by a third piece of code that holds the
// waveform to the limits of the standard (CEC 5) with no allowance, so what
// the engine sends and how it acknowledges are judged the way a strict device
// on the cable would judge them.
#include "cec_test_harness.h"
#include "fwCECWire.h"

#include <deque>
#include <functional>
#include <vector>

namespace
{

// ---- the standard's limits, microseconds (CEC 5) -----------------------------
const uint32_t kStartLowMin = 3500, kStartLowMax = 3900;
const uint32_t kStartMin = 4300, kStartMax = 4700;
const uint32_t kOneLowMin = 400, kOneLowMax = 800;
const uint32_t kZeroLowMin = 1300, kZeroLowMax = 1700;
const uint32_t kBitMin = 2050, kBitMax = 2750;
const uint32_t kAckPullMax = 350;                           // an acknowledge is low by then
const uint32_t kErrorLowMin = 3360, kErrorLowMax = 3840;    // 1.4..1.6 bit periods
const uint32_t kBit = 2400;

// The worst handler delay measured on the board with the four-wire flash mode
// was 119 us; the tests that draw delays use it as their upper end.
const uint32_t kDelayMeasured = 120;

struct Transition
{
    uint32_t uiUs;
    bool bLow;
};

// ---- an engine and its two handlers -------------------------------------------
struct Node
{
    fwCecWire stWire;
    fwCecWireOut stOut = {};
    bool bDrive = false;
    uint32_t uiDelayMax = 0;                // a handler runs 0..uiDelayMax after its cause
    bool bNoDrain = false;                  // the main loop is not emptying the rings
    bool bPullDead = false;                 // its pull never reaches the line
    // Added to the delay of each alarm when it is armed, from the engine's state after the call.
    std::function<uint32_t(const fwCecWire &)> fnAlarmExtra;
    // Interrupts held off: every handler due in [uiBlockFrom, uiBlockTo) runs at
    // uiBlockTo, the alarm before the edges (the timer's vector is the lower one).
    bool bBlock = false;
    uint32_t uiBlockFrom = 0;
    uint32_t uiBlockTo = 0;
    // The falling (rising) edge of this index, counted from 0 per direction,
    // reaches the handler together with the edge after it.
    int iPairFall = -1;
    int iPairRise = -1;
    int iFalls = 0;
    int iRises = 0;
    bool bHeldEdge = false;

    struct Edge
    {
        uint32_t uiAt;
        bool bFalling;                      // a pair: the direction of its first edge
        bool bPair = false;
    };
    std::deque<Edge> qEdges;
    bool bAlarm = false;
    uint32_t uiAlarmTarget = 0;
    uint32_t uiAlarmAt = 0;

    bool blocked(uint32_t uiAt) const
    {
        return bBlock && (int32_t)(uiAt - uiBlockFrom) >= 0 && (int32_t)(uiAt - uiBlockTo) < 0;
    }

    std::vector<Transition> vDrive;         // when this engine pulled and released
    std::vector<fwCecWireRxFrame> vRx;
    std::vector<fwCecWireTxDone> vDone;
};

// ---- a sender that is not the engine: a table of times ------------------------
struct Script
{
    std::vector<Transition> vSteps;         // ascending
    size_t iNext = 0;
    bool bDrive = false;
    std::vector<uint32_t> vSampleAt;        // where it reads the acknowledge
    std::vector<bool> vSampledLow;
};

struct BitTiming
{
    uint32_t uiStartLow = 3700;
    uint32_t uiStart = 4500;
    uint32_t uiOneLow = 600;
    uint32_t uiZeroLow = 1500;
    uint32_t uiBit = 2400;
};

struct Fault
{
    int iBit = -1;                  // the bit, counted over the whole frame, that is changed
    uint32_t uiLow = 0;             // its low time, when not 0
    uint32_t uiPeriod = 0;          // its period, when not 0
    int iStopAfterBits = -1;        // the frame stops after this many bits
};

// Appends the waveform of the blocks. Returns one bit period after the last bit began.
uint32_t scriptBlocks(Script & stScript, uint32_t uiT0, const uint8_t * pBlocks, int iLen,
                      const BitTiming & stTiming = {}, const Fault & stFault = {})
{
    stScript.vSteps.push_back({uiT0, true});
    stScript.vSteps.push_back({uiT0 + stTiming.uiStartLow, false});
    uint32_t uiBitAt = uiT0 + stTiming.uiStart;
    int iIndex = 0;
    for (int iBlock = 0; iBlock < iLen; iBlock++)
    {
        for (int i = 0; i < 10; i++, iIndex++)
        {
            if (stFault.iStopAfterBits >= 0 && iIndex >= stFault.iStopAfterBits)
                return uiBitAt;
            const bool bOne = i < 8 ? ((pBlocks[iBlock] >> (7 - i)) & 1) != 0 : i == 8 ? iBlock == iLen - 1 : true;
            uint32_t uiLow = bOne ? stTiming.uiOneLow : stTiming.uiZeroLow;
            uint32_t uiPeriod = stTiming.uiBit;
            if (iIndex == stFault.iBit)
            {
                if (stFault.uiLow != 0)
                    uiLow = stFault.uiLow;
                if (stFault.uiPeriod != 0)
                    uiPeriod = stFault.uiPeriod;
            }
            stScript.vSteps.push_back({uiBitAt, true});
            stScript.vSteps.push_back({uiBitAt + uiLow, false});
            if (i == 9)
                stScript.vSampleAt.push_back(uiBitAt + 1050);
            uiBitAt += uiPeriod;
        }
    }
    return uiBitAt;
}

uint32_t scriptFrame(Script & stScript, uint32_t uiT0, const fwCecFrame & stFrame,
                     const BitTiming & stTiming = {}, const Fault & stFault = {})
{
    return scriptBlocks(stScript, uiT0, stFrame.abtBlock, stFrame.btLen, stTiming, stFault);
}

bool allSampled(const Script & stScript, size_t iCount, bool bLow)
{
    if (stScript.vSampledLow.size() != iCount)
        return false;
    for (bool b : stScript.vSampledLow)
        if (b != bLow)
            return false;
    return true;
}

// ---- an acknowledger that is not the engine: follows the line edge by edge ----
struct Acker
{
    int iAddress = -1;              // -1: every directly addressed frame
    bool bOnBroadcast = false;      // pulls in the acknowledge bits of a broadcast: rejects it
    uint32_t uiPullDelay = 0;
    uint32_t uiHoldUs = 1500;       // released this long after the falling edge

    bool bDrive = false;
    bool bInFrame = false;
    bool bFirstBit = false;
    int iBit = 0;
    int iBlock = 0;
    uint8_t btByte = 0;
    uint8_t btHeader = 0;
    bool bEom = false;
    uint32_t uiFall = 0;
    bool bPullPending = false;
    bool bReleasePending = false;
    uint32_t uiPullAt = 0;
    uint32_t uiReleaseAt = 0;

    void onLine(bool bLow, uint32_t uiNow)
    {
        if (bLow)
        {
            const uint32_t uiSince = uiNow - uiFall;
            uiFall = uiNow;
            if (!bInFrame)
                return;
            if (uiSince > (bFirstBit ? 5000u : 3200u))
            {
                bInFrame = false;       // the frame stopped; this may be a new start bit
                return;
            }
            bFirstBit = false;
            if (iBit == 9)
            {
                const uint8_t btDestination = (uint8_t)(btHeader & 0x0F);
                const bool bAck = btDestination == CEC_ADDR_BROADCAST ? bOnBroadcast
                                                                      : (iAddress < 0 || btDestination == iAddress);
                if (bAck)
                {
                    bPullPending = true;
                    uiPullAt = uiNow + uiPullDelay;
                    bReleasePending = true;
                    uiReleaseAt = uiNow + uiHoldUs;
                }
            }
            return;
        }

        const uint32_t uiLow = uiNow - uiFall;
        if (!bInFrame)
        {
            if (uiLow >= 3300 && uiLow <= 4100)
            {
                bInFrame = true;
                bFirstBit = true;
                iBit = 0;
                iBlock = 0;
                btByte = 0;
            }
            return;
        }
        const bool bOne = uiLow < 1050;
        if (iBit < 8)
        {
            btByte = (uint8_t)((btByte << 1) | (bOne ? 1 : 0));
            iBit++;
            if (iBit == 8 && iBlock == 0)
                btHeader = btByte;
        }
        else if (iBit == 8)
        {
            bEom = bOne;
            iBit++;
        }
        else if (bEom)
        {
            bInFrame = false;
        }
        else
        {
            iBit = 0;
            iBlock++;
            btByte = 0;
        }
    }
};

// ---- the line ------------------------------------------------------------------
struct Sim
{
    uint32_t uiNow;
    uint32_t uiSenseUs = 20;                // an engine sees the line as it was this long ago
    bool bLow = false;
    bool bHeld = false;                     // something holds the line low
    bool bStuck = false;                    // the simulation stopped making progress
    std::vector<Transition> vLine;
    std::vector<Node *> vNodes;
    std::vector<Script *> vScripts;
    std::vector<Acker *> vAckers;

    explicit Sim(uint32_t uiStart = 100000) : uiNow(uiStart) {}

    void add(Node & stNode, uint16_t uiAckMask, uint32_t uiDelayMax = 0)
    {
        cecWireInit(stNode.stWire, uiNow);
        cecWireSetAckMask(stNode.stWire, uiAckMask);
        stNode.uiDelayMax = uiDelayMax;
        vNodes.push_back(&stNode);
    }

    static uint32_t delayOf(const Node & stNode)
    {
        return stNode.uiDelayMax != 0 ? rng() % (stNode.uiDelayMax + 1) : 0;
    }

    bool due(uint32_t uiAt) const { return (int32_t)(uiNow - uiAt) >= 0; }

    // The line as an engine's pin shows it now.
    bool sensedLow() const
    {
        const uint32_t uiAt = uiNow - uiSenseUs;
        for (size_t i = vLine.size(); i-- > 0;)
            if ((int32_t)(uiAt - vLine[i].uiUs) >= 0)
                return vLine[i].bLow;
        return false;
    }

    // What the engine asked for after a call: the pin and the alarm.
    void apply(Node & stNode)
    {
        if (stNode.stOut.bDriveLow != stNode.bDrive)
        {
            stNode.bDrive = stNode.stOut.bDriveLow;
            stNode.vDrive.push_back({uiNow, stNode.bDrive});
        }
        if (!stNode.stOut.bAlarm)
        {
            stNode.bAlarm = false;
        }
        else if (!stNode.bAlarm || stNode.uiAlarmTarget != stNode.stOut.uiAlarmUs)
        {
            stNode.bAlarm = true;
            stNode.uiAlarmTarget = stNode.stOut.uiAlarmUs;
            const uint32_t uiBase = (int32_t)(stNode.uiAlarmTarget - uiNow) > 0 ? stNode.uiAlarmTarget : uiNow;
            stNode.uiAlarmAt = uiBase + delayOf(stNode);
            if (stNode.fnAlarmExtra)
                stNode.uiAlarmAt += stNode.fnAlarmExtra(stNode.stWire);
            if (stNode.blocked(stNode.uiAlarmAt))
                stNode.uiAlarmAt = stNode.uiBlockTo;
        }
        if (!stNode.bNoDrain)
        {
            fwCecWireRxFrame stRx;
            while (cecWirePopRx(stNode.stWire, stRx))
                stNode.vRx.push_back(stRx);
            fwCecWireTxDone stDone;
            while (cecWirePopTxDone(stNode.stWire, stDone))
                stNode.vDone.push_back(stDone);
        }
        settle();
    }

    void settle()
    {
        bool bNow = bHeld;
        for (const Node * p : vNodes)
            bNow = bNow || (p->bDrive && !p->bPullDead);
        for (const Script * p : vScripts)
            bNow = bNow || p->bDrive;
        for (const Acker * p : vAckers)
            bNow = bNow || p->bDrive;
        if (bNow == bLow)
            return;
        bLow = bNow;
        vLine.push_back({uiNow, bLow});
        for (Acker * p : vAckers)
            p->onLine(bLow, uiNow);
        for (Node * p : vNodes)
        {
            uint32_t uiAt = uiNow + uiSenseUs + delayOf(*p);
            if (p->blocked(uiAt))
                uiAt = p->uiBlockTo;
            if (!p->qEdges.empty() && (int32_t)(uiAt - p->qEdges.back().uiAt) < 0)
                uiAt = p->qEdges.back().uiAt;       // handlers run in the order of their causes
            const bool bHold = bLow ? p->iFalls++ == p->iPairFall : p->iRises++ == p->iPairRise;
            if (bHold)
            {
                p->bHeldEdge = true;                // reaches the handler with the next edge
                continue;
            }
            if (p->bHeldEdge)
            {
                p->bHeldEdge = false;
                p->qEdges.push_back({uiAt, !bLow, true});
                continue;
            }
            p->qEdges.push_back({uiAt, bLow});
        }
    }

    // One edge handler call. As the target's handler does, an edge reported
    // with the line already back where it was before it is reported together
    // with the edge that took it back.
    void deliverEdge(Node & stNode)
    {
        Node::Edge stEdge = stNode.qEdges.front();
        stNode.qEdges.pop_front();
        if (!stEdge.bPair && sensedLow() != stEdge.bFalling && !stNode.qEdges.empty() &&
            stNode.qEdges.front().bFalling != stEdge.bFalling && !stNode.qEdges.front().bPair)
        {
            stNode.qEdges.pop_front();
            stEdge.bPair = true;
        }
        if (stEdge.bPair)
            cecWireOnEdgePair(stNode.stWire, stEdge.bFalling, uiNow, stNode.stOut);
        else
            cecWireOnEdge(stNode.stWire, stEdge.bFalling, uiNow, stNode.stOut);
        apply(stNode);
    }

    void deliverAlarm(Node & stNode)
    {
        if (!stNode.bAlarm || !due(stNode.uiAlarmAt))
            return;
        stNode.bAlarm = false;
        cecWireOnAlarm(stNode.stWire, uiNow, !sensedLow(), stNode.stOut);
        apply(stNode);
    }

    bool send(Node & stNode, const fwCecFrame & stFrame)
    {
        const bool bOk = cecWireSend(stNode.stWire, stFrame, uiNow, stNode.stOut);
        apply(stNode);
        return bOk;
    }

    bool cancel(Node & stNode)
    {
        const bool bOk = cecWireCancel(stNode.stWire, stNode.stOut);
        apply(stNode);
        return bOk;
    }

    void hold(bool bOn)
    {
        bHeld = bOn;
        settle();
    }

    void run(uint32_t uiDurationUs)
    {
        const uint32_t uiEnd = uiNow + uiDurationUs;
        int iGuard = 0;
        while (!bStuck)
        {
            const uint32_t uiLeft = uiEnd - uiNow;
            uint32_t uiBest = uiLeft;
            auto consider = [&](uint32_t uiAt)
            {
                const int32_t iIn = (int32_t)(uiAt - uiNow);
                const uint32_t uiIn = iIn < 0 ? 0u : (uint32_t)iIn;
                if (uiIn < uiBest)
                    uiBest = uiIn;
            };
            for (const Node * p : vNodes)
            {
                if (!p->qEdges.empty())
                    consider(p->qEdges.front().uiAt);
                if (p->bAlarm)
                    consider(p->uiAlarmAt);
            }
            for (const Script * p : vScripts)
            {
                if (p->iNext < p->vSteps.size())
                    consider(p->vSteps[p->iNext].uiUs);
                if (p->vSampledLow.size() < p->vSampleAt.size())
                    consider(p->vSampleAt[p->vSampledLow.size()]);
            }
            for (const Acker * p : vAckers)
            {
                if (p->bPullPending)
                    consider(p->uiPullAt);
                if (p->bReleasePending)
                    consider(p->uiReleaseAt);
            }
            if (uiBest >= uiLeft)
            {
                uiNow = uiEnd;
                return;
            }
            uiNow += uiBest;
            if (uiBest != 0)
                iGuard = 0;
            else if (++iGuard > 100000)
                bStuck = true;

            for (Script * p : vScripts)
            {
                while (p->vSampledLow.size() < p->vSampleAt.size() && due(p->vSampleAt[p->vSampledLow.size()]))
                    p->vSampledLow.push_back(bLow);
                while (p->iNext < p->vSteps.size() && due(p->vSteps[p->iNext].uiUs))
                    p->bDrive = p->vSteps[p->iNext++].bLow;
            }
            settle();
            for (Acker * p : vAckers)
            {
                if (p->bPullPending && due(p->uiPullAt))
                {
                    p->bPullPending = false;
                    p->bDrive = true;
                }
                if (p->bReleasePending && due(p->uiReleaseAt))
                {
                    p->bReleasePending = false;
                    p->bDrive = false;
                }
            }
            settle();
            for (Node * p : vNodes)
            {
                if (p->bBlock && uiNow == p->uiBlockTo)
                    deliverAlarm(*p);
                while (!p->qEdges.empty() && due(p->qEdges.front().uiAt))
                    deliverEdge(*p);
                deliverAlarm(*p);
            }
        }
    }

    // Runs until the engine has reported iCount results, at most uiMaxUs.
    bool runUntilDone(const Node & stNode, size_t iCount, uint32_t uiMaxUs)
    {
        for (uint32_t ui = 0; ui < uiMaxUs && stNode.vDone.size() < iCount && !bStuck; ui += 100)
            run(100);
        return stNode.vDone.size() >= iCount;
    }
};

// ---- the recorded line, decoded with no allowance ------------------------------
struct LineFrame
{
    uint32_t uiStartUs = 0;             // falling edge of the start bit
    uint32_t uiLastBitUs = 0;           // falling edge of the last bit
    int iLen = 0;
    uint8_t abtBlock[20] = {};
    bool abAckLow[20] = {};
    uint32_t auiAckFallUs[20] = {};
    bool bComplete = false;             // ended on a block with the end of message bit set
    int iHeadViolations = 0;            // the start bit and the four initiator bits, where two may pull
    int iBodyViolations = 0;            // every bit after them
};

struct LineView
{
    std::vector<LineFrame> vFrames;
    std::vector<uint32_t> vOtherLowUs;  // low pulses that belong to no frame
};

LineView decodeLine(const std::vector<Transition> & vLine)
{
    struct Pulse
    {
        uint32_t uiFall;
        uint32_t uiRise;
    };
    std::vector<Pulse> vPulses;
    for (size_t i = 0; i + 1 < vLine.size(); i++)
        if (vLine[i].bLow && !vLine[i + 1].bLow)
            vPulses.push_back({vLine[i].uiUs, vLine[i + 1].uiUs});

    LineView stView;
    size_t i = 0;
    while (i < vPulses.size())
    {
        const uint32_t uiLow = vPulses[i].uiRise - vPulses[i].uiFall;
        const bool bNext = i + 1 < vPulses.size();
        const uint32_t uiPeriod = bNext ? vPulses[i + 1].uiFall - vPulses[i].uiFall : 0xFFFFFFFFu;
        if (!(uiLow >= 3300 && uiLow <= 4100 && uiPeriod >= 4100 && uiPeriod <= 4900))
        {
            stView.vOtherLowUs.push_back(uiLow);
            i++;
            continue;
        }

        LineFrame stFrame;
        stFrame.uiStartUs = vPulses[i].uiFall;
        if (uiLow < kStartLowMin || uiLow > kStartLowMax)
            stFrame.iHeadViolations++;
        if (uiPeriod < kStartMin || uiPeriod > kStartMax)
            stFrame.iHeadViolations++;
        i++;

        int iBit = 0;
        uint8_t btByte = 0;
        bool bEom = false;
        while (i < vPulses.size() && stFrame.iLen < 20)
        {
            const uint32_t uiBitLow = vPulses[i].uiRise - vPulses[i].uiFall;
            const bool bBitNext = i + 1 < vPulses.size();
            const uint32_t uiBitPeriod = bBitNext ? vPulses[i + 1].uiFall - vPulses[i].uiFall : 0xFFFFFFFFu;
            const bool bOne = uiBitLow < 1050;
            int & iViolations = (stFrame.iLen == 0 && iBit < 4) ? stFrame.iHeadViolations : stFrame.iBodyViolations;
            const bool bLowOk = bOne ? (uiBitLow >= kOneLowMin && uiBitLow <= kOneLowMax)
                                     : (uiBitLow >= kZeroLowMin && uiBitLow <= kZeroLowMax);
            if (!bLowOk)
                iViolations++;
            stFrame.uiLastBitUs = vPulses[i].uiFall;

            bool bLast = false;
            if (iBit < 8)
            {
                btByte = (uint8_t)((btByte << 1) | (bOne ? 1 : 0));
                iBit++;
            }
            else if (iBit == 8)
            {
                bEom = bOne;
                iBit++;
            }
            else
            {
                stFrame.abtBlock[stFrame.iLen] = btByte;
                stFrame.abAckLow[stFrame.iLen] = !bOne;
                stFrame.auiAckFallUs[stFrame.iLen] = vPulses[i].uiFall;
                stFrame.iLen++;
                iBit = 0;
                btByte = 0;
                if (bEom)
                {
                    stFrame.bComplete = true;
                    bLast = true;
                }
            }
            i++;
            if (bLast || uiBitPeriod > 3200)
                break;
            if (uiBitPeriod < kBitMin || uiBitPeriod > kBitMax)
                iViolations++;
        }
        stView.vFrames.push_back(stFrame);
    }
    return stView;
}

// The low pulses of that length on the recorded line.
int lineLowCount(const std::vector<Transition> & vLine, uint32_t uiMin, uint32_t uiMax)
{
    int iCount = 0;
    for (size_t i = 0; i + 1 < vLine.size(); i++)
    {
        const uint32_t uiLow = vLine[i + 1].uiUs - vLine[i].uiUs;
        if (vLine[i].bLow && !vLine[i + 1].bLow && uiLow >= uiMin && uiLow <= uiMax)
            iCount++;
    }
    return iCount;
}

bool lineFrameIs(const LineFrame & stLine, const fwCecFrame & stFrame)
{
    return stLine.iLen == stFrame.btLen && memcmp(stLine.abtBlock, stFrame.abtBlock, stFrame.btLen) == 0;
}

bool allAcked(const LineFrame & stLine, bool bLow)
{
    for (int i = 0; i < stLine.iLen; i++)
        if (stLine.abAckLow[i] != bLow)
            return false;
    return true;
}

// The engine pulled in the acknowledge bit of the block inside the standard's
// limits: low by 350 us after the falling edge, released 1.3..1.7 ms after it.
bool pulledAck(const Node & stNode, uint32_t uiFallUs)
{
    for (size_t i = 0; i + 1 < stNode.vDrive.size(); i++)
    {
        if (!stNode.vDrive[i].bLow || stNode.vDrive[i + 1].bLow)
            continue;
        const uint32_t uiPull = stNode.vDrive[i].uiUs - uiFallUs;
        const uint32_t uiRelease = stNode.vDrive[i + 1].uiUs - uiFallUs;
        if (uiPull <= kAckPullMax && uiRelease >= kZeroLowMin && uiRelease <= kZeroLowMax)
            return true;
    }
    return false;
}

bool pulledEveryAck(const Node & stNode, const LineFrame & stLine)
{
    for (int i = 0; i < stLine.iLen; i++)
        if (!pulledAck(stNode, stLine.auiAckFallUs[i]))
            return false;
    return true;
}

bool isIdle(const Node & stNode)
{
    return stNode.stWire.eRx == fwCecWireRxState::Idle && stNode.stWire.eTx == fwCecWireTxState::Idle &&
           !stNode.bDrive && !stNode.stWire.bTxLow && !stNode.stWire.bFollowLow;
}

const uint16_t kMask4 = 1u << 4;
const uint8_t kRxBoth = CEC_WIRE_RX_ACKED | CEC_WIRE_RX_FOR_US;

// ------------------------------------------------------------------ receiving

TEST(testWireReceivesAndAcknowledges)
{
    for (uint32_t uiDelay : {0u, kDelayMeasured})
    {
        rngSeed(11 + uiDelay);
        Sim sim;
        Node n;
        sim.add(n, kMask4, uiDelay);
        Script s;
        sim.vScripts.push_back(&s);
        const fwCecFrame f = FRAME(0x04, 0x44, 0x41);
        scriptFrame(s, sim.uiNow + 20000, f);
        sim.run(200000);

        CHECK_EQ(n.vRx.size(), 1u);
        if (n.vRx.size() == 1)
        {
            CHECK(sameFrame(n.vRx[0].stFrame, f));
            CHECK_EQ(n.vRx[0].btFlags, kRxBoth);
        }
        CHECK(allSampled(s, 3, true));
        const LineView v = decodeLine(sim.vLine);
        CHECK_EQ(v.vFrames.size(), 1u);
        if (v.vFrames.size() == 1)
        {
            CHECK(v.vFrames[0].bComplete);
            CHECK(lineFrameIs(v.vFrames[0], f));
            CHECK(allAcked(v.vFrames[0], true));
            CHECK_EQ(v.vFrames[0].iHeadViolations + v.vFrames[0].iBodyViolations, 0);
            CHECK(pulledEveryAck(n, v.vFrames[0]));
        }
        CHECK_EQ(n.vDrive.size(), 6u);          // three acknowledges and nothing else
        CHECK_EQ(n.stWire.uiRxFrames, 1u);
        CHECK_EQ(n.stWire.uiRxErrors, 0u);
        CHECK(isIdle(n));
        CHECK(!sim.bStuck);
    }
}

TEST(testWireBroadcastIsNeverPulled)
{
    Sim sim;
    Node n;
    sim.add(n, kMask4);
    Script s;
    sim.vScripts.push_back(&s);
    const fwCecFrame f = FRAME(0x0F, 0x36);
    scriptFrame(s, sim.uiNow + 20000, f);
    sim.run(200000);

    CHECK_EQ(n.vRx.size(), 1u);
    if (n.vRx.size() == 1)
    {
        CHECK(sameFrame(n.vRx[0].stFrame, f));
        CHECK_EQ(n.vRx[0].btFlags, kRxBoth);
    }
    CHECK(allSampled(s, 2, false));
    CHECK(n.vDrive.empty());
    CHECK(isIdle(n));
}

TEST(testWireBroadcastRejectedByAnother)
{
    Sim sim;
    Node n;
    sim.add(n, kMask4);
    Script s;
    sim.vScripts.push_back(&s);
    Acker a;
    a.bOnBroadcast = true;
    sim.vAckers.push_back(&a);
    const fwCecFrame f = FRAME(0x0F, 0x36);
    scriptFrame(s, sim.uiNow + 20000, f);
    sim.run(200000);

    CHECK_EQ(n.vRx.size(), 1u);
    if (n.vRx.size() == 1)
        CHECK_EQ(n.vRx[0].btFlags, CEC_WIRE_RX_FOR_US);     // rejected on the wire
    CHECK(n.vDrive.empty());
}

TEST(testWireFramesForOthers)
{
    const fwCecFrame f = FRAME(0x05, 0x8F);
    {
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        Script s;
        sim.vScripts.push_back(&s);
        scriptFrame(s, sim.uiNow + 20000, f);
        sim.run(200000);
        CHECK(n.vRx.empty());
        CHECK(n.vDrive.empty());
        CHECK(allSampled(s, 2, false));
        CHECK_EQ(n.stWire.uiRxFrames, 1u);
        CHECK(isIdle(n));
    }
    {
        // Monitoring: handed over, with the acknowledge another device gave.
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        cecWireSetMonitorAll(n.stWire, true);
        Script s;
        sim.vScripts.push_back(&s);
        Acker a;
        a.iAddress = 5;
        sim.vAckers.push_back(&a);
        scriptFrame(s, sim.uiNow + 20000, f);
        scriptFrame(s, sim.uiNow + 120000, FRAME(0x06, 0x8F));     // nobody at 6
        sim.run(300000);
        CHECK_EQ(n.vRx.size(), 2u);
        if (n.vRx.size() == 2)
        {
            CHECK(sameFrame(n.vRx[0].stFrame, f));
            CHECK_EQ(n.vRx[0].btFlags, CEC_WIRE_RX_ACKED);
            CHECK_EQ(n.vRx[1].btFlags, 0);
        }
        CHECK(n.vDrive.empty());
    }
}

TEST(testWirePoll)
{
    const fwCecFrame f = FRAME(0x04);
    for (int iMonitor = 0; iMonitor < 2; iMonitor++)
    {
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        cecWireSetMonitorAll(n.stWire, iMonitor != 0);
        Script s;
        sim.vScripts.push_back(&s);
        scriptFrame(s, sim.uiNow + 20000, f);
        sim.run(100000);
        CHECK(allSampled(s, 1, true));              // acknowledged on the wire
        CHECK_EQ(n.vRx.size(), iMonitor != 0 ? 1u : 0u);
        CHECK_EQ(n.stWire.uiRxFrames, 1u);
        CHECK(isIdle(n));
    }
}

TEST(testWireAcknowledgesASetOfAddresses)
{
    const uint16_t uiMask = (1u << 1) | (1u << 4) | (1u << 8);
    for (int iAddress = 0; iAddress < 15; iAddress++)
    {
        Sim sim;
        Node n;
        sim.add(n, uiMask);
        Script s;
        sim.vScripts.push_back(&s);
        const uint8_t abt[2] = {(uint8_t)(0xE0 | iAddress), 0x83};
        const fwCecFrame f = frameOf(abt, 2);
        scriptFrame(s, sim.uiNow + 20000, f);
        sim.run(100000);
        const bool bOurs = ((uiMask >> iAddress) & 1) != 0;
        CHECK(allSampled(s, 2, bOurs));
        CHECK_EQ(n.vRx.size(), bOurs ? 1u : 0u);
    }
    // Address 15 is never acknowledged, whatever the mask is set to.
    Node n;
    Sim sim;
    sim.add(n, 0xFFFF);
    CHECK_EQ(n.stWire.uiAckMask, 0x7FFFu);
}

TEST(testWireTimingCorners)
{
    const fwCecFrame f = FRAME(0x04, 0xA5, 0x5A, 0x00, 0xFF);
    for (uint32_t uiDelay : {0u, kDelayMeasured})
    {
        for (int iCase = 0; iCase < 32; iCase++)
        {
            rngSeed(1000 + iCase * 7 + uiDelay);
            BitTiming t;
            t.uiStartLow = (iCase & 1) ? kStartLowMax : kStartLowMin;
            t.uiStart = (iCase & 2) ? kStartMax : kStartMin;
            t.uiOneLow = (iCase & 4) ? kOneLowMax : kOneLowMin;
            t.uiZeroLow = (iCase & 8) ? kZeroLowMax : kZeroLowMin;
            t.uiBit = (iCase & 16) ? kBitMax : kBitMin;
            Sim sim;
            Node n;
            sim.add(n, kMask4, uiDelay);
            Script s;
            sim.vScripts.push_back(&s);
            scriptFrame(s, sim.uiNow + 20000, f, t);
            sim.run(300000);

            CHECK_EQ(n.vRx.size(), 1u);
            if (n.vRx.size() == 1)
                CHECK(sameFrame(n.vRx[0].stFrame, f));
            CHECK(allSampled(s, 5, true));
            const LineView v = decodeLine(sim.vLine);
            CHECK_EQ(v.vFrames.size(), 1u);
            if (v.vFrames.size() == 1)
                CHECK(pulledEveryAck(n, v.vFrames[0]));
            CHECK_EQ(n.stWire.uiRxErrors, 0u);
            CHECK_EQ(n.stWire.uiErrorSignals, 0u);
            CHECK(isIdle(n));
        }
    }
}

// The next frame after each fault is received: the receiver recovered.
const fwCecFrame kAfter = FRAME(0x04, 0x8F);

TEST(testWireShortBitPeriod)
{
    const fwCecFrame f = FRAME(0x04, 0xFF, 0x00);
    Fault stFault;
    stFault.iBit = 12;              // a 1 in the second block
    stFault.uiPeriod = 1500;
    stFault.iStopAfterBits = 14;
    for (uint32_t uiDelay : {0u, kDelayMeasured})
    {
        rngSeed(31 + uiDelay);
        Sim sim;
        Node n;
        sim.add(n, kMask4, uiDelay);
        Script s;
        sim.vScripts.push_back(&s);
        scriptFrame(s, sim.uiNow + 20000, f, {}, stFault);
        scriptFrame(s, sim.uiNow + 120000, kAfter);
        sim.run(300000);

        CHECK_EQ(n.stWire.uiRxErrors, 1u);
        CHECK_EQ(n.vRx.size(), 1u);
        if (n.vRx.size() == 1)
            CHECK(sameFrame(n.vRx[0].stFrame, kAfter));
        // The line shows the two start bits and the error signal.
        CHECK_EQ(lineLowCount(sim.vLine, kErrorLowMin, kErrorLowMax), 3);
        CHECK_EQ(n.stWire.uiErrorSignals, 1u);
        CHECK(isIdle(n));
    }
}

TEST(testWireReceiveFaults)
{
    const fwCecFrame f = FRAME(0x04, 0xFF, 0x00);
    {
        // A low time between a 1 and a 0.
        Fault stFault;
        stFault.iBit = 12;
        stFault.uiLow = 1050;
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        Script s;
        sim.vScripts.push_back(&s);
        scriptFrame(s, sim.uiNow + 20000, f, {}, stFault);
        scriptFrame(s, sim.uiNow + 120000, kAfter);
        sim.run(300000);
        CHECK_EQ(n.stWire.uiRxErrors, 1u);
        CHECK_EQ(n.stWire.uiErrorSignals, 0u);
        CHECK_EQ(n.vRx.size(), 1u);
        if (n.vRx.size() == 1)
            CHECK(sameFrame(n.vRx[0].stFrame, kAfter));
        CHECK(isIdle(n));
    }
    {
        // The frame stops in the middle of a block.
        Fault stFault;
        stFault.iStopAfterBits = 13;
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        Script s;
        sim.vScripts.push_back(&s);
        scriptFrame(s, sim.uiNow + 20000, f, {}, stFault);
        scriptFrame(s, sim.uiNow + 120000, kAfter);
        sim.run(300000);
        CHECK_EQ(n.stWire.uiRxErrors, 1u);
        CHECK_EQ(n.vRx.size(), 1u);
        CHECK(isIdle(n));
    }
    {
        // Low pulses that are no start bit, and a start bit nothing follows.
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        Script s;
        sim.vScripts.push_back(&s);
        const uint32_t uiT = sim.uiNow;
        s.vSteps.push_back({uiT + 10000, true});
        s.vSteps.push_back({uiT + 10100, false});       // 100 us
        s.vSteps.push_back({uiT + 20000, true});
        s.vSteps.push_back({uiT + 22000, false});       // 2 ms
        s.vSteps.push_back({uiT + 40000, true});
        s.vSteps.push_back({uiT + 45000, false});       // 5 ms
        s.vSteps.push_back({uiT + 60000, true});
        s.vSteps.push_back({uiT + 63700, false});       // a start bit, then silence
        scriptFrame(s, uiT + 120000, kAfter);
        sim.run(300000);
        CHECK_EQ(n.stWire.uiRxErrors, 1u);              // only the start bit nothing followed
        CHECK_EQ(n.stWire.uiErrorSignals, 0u);
        CHECK_EQ(n.vRx.size(), 1u);
        CHECK(isIdle(n));
    }
    {
        // Seventeen blocks: sixteen acknowledged, the frame dropped.
        uint8_t abt[17];
        abt[0] = 0x04;
        for (int i = 1; i < 17; i++)
            abt[i] = (uint8_t)(i * 17);
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        Script s;
        sim.vScripts.push_back(&s);
        scriptBlocks(s, sim.uiNow + 20000, abt, 17);
        scriptFrame(s, sim.uiNow + 520000, kAfter);
        sim.run(700000);
        CHECK_EQ(s.vSampledLow.size(), 19u);
        for (size_t i = 0; i < s.vSampledLow.size(); i++)
            CHECK_EQ(s.vSampledLow[i], i != 16);
        CHECK_EQ(n.stWire.uiRxErrors, 1u);
        CHECK_EQ(n.vRx.size(), 1u);
        if (n.vRx.size() == 1)
            CHECK(sameFrame(n.vRx[0].stFrame, kAfter));
        CHECK(isIdle(n));
    }
}

TEST(testWireRingOverrun)
{
    Sim sim;
    Node n;
    sim.add(n, kMask4);
    n.bNoDrain = true;
    Script s;
    sim.vScripts.push_back(&s);
    for (int i = 0; i < 9; i++)
    {
        const uint8_t abt[2] = {0x04, (uint8_t)i};
        scriptFrame(s, sim.uiNow + 20000 + (uint32_t)i * 80000, frameOf(abt, 2));
    }
    sim.run(900000);
    CHECK_EQ(n.stWire.uiRxFrames, 9u);
    CHECK_EQ(n.stWire.uiRxOverruns, 2u);        // a ring of eight holds seven
    CHECK(allSampled(s, 18, true));             // still acknowledged on the wire
    fwCecWireRxFrame stRx;
    int iCount = 0;
    while (cecWirePopRx(n.stWire, stRx))
    {
        CHECK_EQ(stRx.stFrame.abtBlock[1], iCount);
        iCount++;
    }
    CHECK_EQ(iCount, 7);
}

// ------------------------------------------------------------------ sending

TEST(testWireSendsAndIsAcknowledged)
{
    const fwCecFrame f = FRAME(0x40, 0xA5, 0x5A, 0x00, 0xFF);
    for (uint32_t uiDelay : {0u, kDelayMeasured})
    {
        rngSeed(51 + uiDelay);
        Sim sim;
        Node n;
        sim.add(n, kMask4, uiDelay);
        Acker a;
        a.iAddress = 0;
        sim.vAckers.push_back(&a);
        const uint32_t uiInit = sim.uiNow;

        CHECK(cecWireCanSend(n.stWire));
        CHECK(sim.send(n, f));
        CHECK(!cecWireCanSend(n.stWire));
        CHECK(!sim.send(n, kAfter));                // one frame at a time
        sim.run(300000);

        CHECK_EQ(n.vDone.size(), 1u);
        if (n.vDone.size() == 1)
        {
            CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Acked);
            CHECK_EQ(n.vDone[0].btAttempts, 1);
            CHECK(sameFrame(n.vDone[0].stFrame, f));
        }
        const LineView v = decodeLine(sim.vLine);
        CHECK_EQ(v.vFrames.size(), 1u);
        if (v.vFrames.size() == 1)
        {
            CHECK(v.vFrames[0].bComplete);
            CHECK(lineFrameIs(v.vFrames[0], f));
            CHECK(allAcked(v.vFrames[0], true));
            CHECK_EQ(v.vFrames[0].iHeadViolations + v.vFrames[0].iBodyViolations, 0);
            // The line counted as busy until the engine was started.
            CHECK(v.vFrames[0].uiStartUs - uiInit >= CEC_WIRE_FREE_NEW_BITS * kBit);
            CHECK(v.vFrames[0].uiStartUs - uiInit <= CEC_WIRE_FREE_NEW_BITS * kBit + uiDelay);
        }
        CHECK(v.vOtherLowUs.empty());
        CHECK(n.vRx.empty());
        CHECK(cecWireCanSend(n.stWire));
        CHECK(isIdle(n));
        CHECK(!sim.bStuck);
    }
}

TEST(testWireSendRefused)
{
    Sim sim;
    Node n;
    sim.add(n, kMask4);
    fwCecFrame stEmpty = {};
    CHECK(!sim.send(n, stEmpty));
    fwCecFrame stLong = FRAME(0x40, 0x00);
    stLong.btLen = CEC_FRAME_MAX + 1;
    CHECK(!sim.send(n, stLong));
    CHECK(isIdle(n));
    CHECK(!n.stOut.bAlarm);
    sim.run(100000);
    CHECK(sim.vLine.empty());
    CHECK(n.vDone.empty());
}

TEST(testWireNotAcknowledged)
{
    struct Case
    {
        fwCecFrame stFrame;
        int iAttempts;
    };
    const Case aCases[] = {
        {FRAME(0x40, 0x04), CEC_WIRE_ATTEMPTS},
        {FRAME(0x44), CEC_WIRE_POLL_ATTEMPTS},
    };
    for (const Case & c : aCases)
    {
        for (uint32_t uiDelay : {0u, kDelayMeasured})
        {
            rngSeed(61 + uiDelay);
            Sim sim;
            Node n;
            sim.add(n, kMask4, uiDelay);
            CHECK(sim.send(n, c.stFrame));
            sim.run(400000);

            CHECK_EQ(n.vDone.size(), 1u);
            if (n.vDone.size() == 1)
            {
                CHECK_RES(n.vDone[0].eResult, fwCecTxResult::NotAcked);
                CHECK_EQ(n.vDone[0].btAttempts, c.iAttempts);
            }
            CHECK_EQ(n.stWire.uiTxRetries, (uint32_t)(c.iAttempts - 1));
            // Every attempt stops after the block nobody acknowledged.
            const LineView v = decodeLine(sim.vLine);
            CHECK_EQ(v.vFrames.size(), (size_t)c.iAttempts);
            for (size_t i = 0; i < v.vFrames.size(); i++)
            {
                CHECK_EQ(v.vFrames[i].iLen, 1);
                CHECK_EQ(v.vFrames[i].abtBlock[0], c.stFrame.abtBlock[0]);
                CHECK(!v.vFrames[i].abAckLow[0]);
                CHECK_EQ(v.vFrames[i].iHeadViolations + v.vFrames[i].iBodyViolations, 0);
                if (i > 0)
                {
                    const uint32_t uiGap = v.vFrames[i].uiStartUs - v.vFrames[i - 1].uiLastBitUs;
                    CHECK(uiGap >= CEC_WIRE_FREE_RETRY_BITS * kBit);
                    CHECK(uiGap <= CEC_WIRE_FREE_RETRY_BITS * kBit + uiDelay);
                }
            }
            CHECK(isIdle(n));
        }
    }
}

TEST(testWireBroadcastSend)
{
    const fwCecFrame f = FRAME(0x4F, 0x82, 0x10, 0x00);
    {
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        CHECK(sim.send(n, f));
        sim.run(300000);
        CHECK_EQ(n.vDone.size(), 1u);
        if (n.vDone.size() == 1)
        {
            CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Acked);
            CHECK_EQ(n.vDone[0].btAttempts, 1);
        }
        const LineView v = decodeLine(sim.vLine);
        CHECK_EQ(v.vFrames.size(), 1u);
        if (v.vFrames.size() == 1)
        {
            CHECK(lineFrameIs(v.vFrames[0], f));
            CHECK(v.vFrames[0].bComplete);
        }
    }
    {
        // Another device rejects it.
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        Acker a;
        a.bOnBroadcast = true;
        sim.vAckers.push_back(&a);
        CHECK(sim.send(n, f));
        sim.run(400000);
        CHECK_EQ(n.vDone.size(), 1u);
        if (n.vDone.size() == 1)
        {
            CHECK_RES(n.vDone[0].eResult, fwCecTxResult::NotAcked);
            CHECK_EQ(n.vDone[0].btAttempts, CEC_WIRE_ATTEMPTS);
        }
        CHECK(isIdle(n));
    }
}

// The device's answers on the line: every block in place, the end of message
// bit on the last block only, every bit inside the standard's limits.
TEST(testWireSendsTheAnswers)
{
    const fwCecFrame kAnswers[] = {
        FRAME(0x40, 0x47, 'F', 'r', 'e', 'e', 'w', 'i', 'l', 'i', ' ', '2'),
        FRAME(0x40, 0x90, 0x00),
        FRAME(0x40, 0x9E, 0x05),
        FRAME(0x4F, 0x87, 0x00, 0x15, 0x82),
        FRAME(0x40, 0x00, 0x8C, 0x00),
    };
    for (const fwCecFrame & f : kAnswers)
    {
        for (uint32_t uiDelay : {0u, kDelayMeasured})
        {
            Sim sim;
            Node n;
            sim.add(n, kMask4, uiDelay);
            Acker a;
            a.iAddress = 0;
            sim.vAckers.push_back(&a);
            CHECK(sim.send(n, f));
            sim.run(500000);

            CHECK_EQ(n.vDone.size(), 1u);
            if (n.vDone.size() == 1)
            {
                CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Acked);
                CHECK_EQ(n.vDone[0].btAttempts, 1);
            }
            const LineView v = decodeLine(sim.vLine);
            CHECK_EQ(v.vFrames.size(), 1u);
            if (v.vFrames.size() == 1)
            {
                CHECK(v.vFrames[0].bComplete);
                CHECK(lineFrameIs(v.vFrames[0], f));
                CHECK_EQ(v.vFrames[0].iHeadViolations + v.vFrames[0].iBodyViolations, 0);
            }
            CHECK(v.vOtherLowUs.empty());
            CHECK(isIdle(n));
            CHECK(!sim.bStuck);
        }
    }
}

// The acknowledge as early and short, and as late and long, as a device may give it.
TEST(testWireAcknowledgeCorners)
{
    const fwCecFrame f = FRAME(0x40, 0xFF, 0x00);
    for (int iCase = 0; iCase < 2; iCase++)
    {
        rngSeed(71 + iCase);
        Sim sim;
        Node n;
        sim.add(n, kMask4, kDelayMeasured);
        Acker a;
        a.iAddress = 0;
        a.uiPullDelay = iCase == 0 ? 0 : kAckPullMax;
        a.uiHoldUs = iCase == 0 ? kZeroLowMin : kZeroLowMax;
        sim.vAckers.push_back(&a);
        CHECK(sim.send(n, f));
        sim.run(300000);
        CHECK_EQ(n.vDone.size(), 1u);
        if (n.vDone.size() == 1)
        {
            CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Acked);
            CHECK_EQ(n.vDone[0].btAttempts, 1);
        }
    }
}

TEST(testWireFreeTime)
{
    for (uint32_t uiDelay : {0u, kDelayMeasured})
    {
        rngSeed(81 + uiDelay);
        Sim sim;
        Node n;
        sim.add(n, kMask4, uiDelay);
        Acker a;
        a.iAddress = 0;
        sim.vAckers.push_back(&a);
        Script s;
        sim.vScripts.push_back(&s);

        // Two frames of its own, the second queued the moment the first is done.
        CHECK(sim.send(n, FRAME(0x40, 0x04)));
        CHECK(sim.runUntilDone(n, 1, 400000));
        CHECK(sim.send(n, FRAME(0x40, 0x0D)));
        CHECK(sim.runUntilDone(n, 2, 400000));

        // Another device sends; a frame queued in the middle of it waits its turn.
        const uint32_t uiOther = sim.uiNow + 30000;
        scriptFrame(s, uiOther, FRAME(0x0F, 0x85));
        sim.run(30000 + 20000);
        CHECK(sim.send(n, FRAME(0x40, 0x8F)));
        CHECK(sim.runUntilDone(n, 3, 400000));

        // A frame queued long after anything was on the line goes out at once.
        sim.run(100000);
        const uint32_t uiQueued = sim.uiNow;
        CHECK(sim.send(n, FRAME(0x40, 0x9F)));
        CHECK(sim.runUntilDone(n, 4, 400000));

        const LineView v = decodeLine(sim.vLine);
        CHECK_EQ(v.vFrames.size(), 5u);
        if (v.vFrames.size() == 5)
        {
            const uint32_t uiSame = v.vFrames[1].uiStartUs - v.vFrames[0].uiLastBitUs;
            CHECK(uiSame >= CEC_WIRE_FREE_SAME_BITS * kBit);
            // The result is reported one bit period after the last bit began.
            CHECK(uiSame <= CEC_WIRE_FREE_SAME_BITS * kBit + 2 * uiDelay + 100);
            CHECK_EQ(v.vFrames[2].uiStartUs, uiOther);
            const uint32_t uiNew = v.vFrames[3].uiStartUs - v.vFrames[2].uiLastBitUs;
            CHECK(uiNew >= CEC_WIRE_FREE_NEW_BITS * kBit);
            CHECK(uiNew <= CEC_WIRE_FREE_NEW_BITS * kBit + 2 * uiDelay + sim.uiSenseUs);
            CHECK(v.vFrames[4].uiStartUs - uiQueued <= uiDelay);
            for (const LineFrame & stLine : v.vFrames)
                CHECK_EQ(stLine.iHeadViolations + stLine.iBodyViolations, 0);
        }
        for (const fwCecWireTxDone & stDone : n.vDone)
        {
            CHECK_RES(stDone.eResult, fwCecTxResult::Acked);
            CHECK_EQ(stDone.btAttempts, 1);
        }
        CHECK_EQ(n.vRx.size(), 1u);         // the broadcast of the other device
        CHECK(isIdle(n));
    }
}

TEST(testWireArbitration)
{
    // The pair differs in the first, second, third and fourth initiator bit.
    const int aiPairs[][2] = {{4, 8}, {0, 4}, {4, 6}, {4, 5}};
    // Two engines begin a frame together when neither has seen the other's
    // start bit yet: both are due at the same moment, and the handler delays
    // are shorter than the time the line takes to show a change.
    const uint32_t auiTiming[][2] = {{0, 20}, {50, 60}};        // handler delay, sensing lag
    for (const auto & aiPair : aiPairs)
    {
        for (const auto & auiCase : auiTiming)
        {
            const uint32_t uiDelay = auiCase[0];
            rngSeed(91 + uiDelay + (uint32_t)aiPair[1]);
            const int iLow = aiPair[0], iHigh = aiPair[1];
            Sim sim;
            sim.uiSenseUs = auiCase[1];
            Node lo, hi;
            sim.add(lo, (uint16_t)(1u << iLow), uiDelay);
            sim.add(hi, (uint16_t)(1u << iHigh), uiDelay);
            const uint8_t abtLo[3] = {(uint8_t)((iLow << 4) | iHigh), 0x8F, 0x11};
            const uint8_t abtHi[3] = {(uint8_t)((iHigh << 4) | iLow), 0x8F, 0x22};
            const fwCecFrame fLo = frameOf(abtLo, 3), fHi = frameOf(abtHi, 3);
            CHECK(sim.send(hi, fHi));
            CHECK(sim.send(lo, fLo));
            sim.run(400000);

            // The lower address goes first; the other receives it and then sends.
            CHECK_EQ(lo.vDone.size(), 1u);
            CHECK_EQ(hi.vDone.size(), 1u);
            if (lo.vDone.size() == 1 && hi.vDone.size() == 1)
            {
                CHECK_RES(lo.vDone[0].eResult, fwCecTxResult::Acked);
                CHECK_EQ(lo.vDone[0].btAttempts, 1);
                CHECK_RES(hi.vDone[0].eResult, fwCecTxResult::Acked);
                CHECK_EQ(hi.vDone[0].btAttempts, 2);
            }
            CHECK_EQ(lo.stWire.uiArbitrationLost, 0u);
            CHECK_EQ(hi.stWire.uiArbitrationLost, 1u);
            CHECK_EQ(hi.stWire.uiLineErrors, 0u);
            CHECK_EQ(hi.vRx.size(), 1u);
            if (hi.vRx.size() == 1)
            {
                CHECK(sameFrame(hi.vRx[0].stFrame, fLo));
                CHECK_EQ(hi.vRx[0].btFlags, kRxBoth);
            }
            CHECK_EQ(lo.vRx.size(), 1u);
            if (lo.vRx.size() == 1)
                CHECK(sameFrame(lo.vRx[0].stFrame, fHi));

            const LineView v = decodeLine(sim.vLine);
            CHECK_EQ(v.vFrames.size(), 2u);
            if (v.vFrames.size() == 2)
            {
                CHECK(lineFrameIs(v.vFrames[0], fLo));
                CHECK(lineFrameIs(v.vFrames[1], fHi));
                CHECK(allAcked(v.vFrames[0], true) && allAcked(v.vFrames[1], true));
                CHECK_EQ(v.vFrames[0].iBodyViolations + v.vFrames[1].iBodyViolations, 0);
                CHECK_EQ(v.vFrames[1].iHeadViolations, 0);
                CHECK(pulledEveryAck(hi, v.vFrames[0]));
                CHECK(v.vFrames[1].uiStartUs - v.vFrames[0].uiLastBitUs >= CEC_WIRE_FREE_RETRY_BITS * kBit);
            }
            CHECK(isIdle(lo) && isIdle(hi));
            CHECK(!sim.bStuck);
        }
    }
}

// A low pulse that covers the point where the sender reads the line back.
TEST(testWireLineErrorWhileSending)
{
    const fwCecFrame f = FRAME(0x40, 0xFF, 0x00);
    struct Case
    {
        int iBit;               // counted over the frame; a bit sent as a 1
        uint32_t uiLost;        // arbitration lost
        uint32_t uiLine;        // line error
    };
    // Bit 1 is an initiator bit: taken as another initiator. Bit 10 is data.
    const Case aCases[] = {{1, 1, 0}, {10, 0, 1}};
    for (const Case & c : aCases)
    {
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        Acker a;
        a.iAddress = 0;
        sim.vAckers.push_back(&a);
        Script s;
        sim.vScripts.push_back(&s);
        const uint32_t uiStart = sim.uiNow + CEC_WIRE_FREE_NEW_BITS * kBit;
        const uint32_t uiBitAt = uiStart + CEC_WIRE_START_US + (uint32_t)c.iBit * kBit;
        s.vSteps.push_back({uiBitAt + 900, true});
        s.vSteps.push_back({uiBitAt + 1200, false});
        CHECK(sim.send(n, f));
        sim.run(400000);

        CHECK_EQ(n.stWire.uiArbitrationLost, c.uiLost);
        CHECK_EQ(n.stWire.uiLineErrors, c.uiLine);
        CHECK_EQ(n.stWire.uiTxRetries, 1u);
        CHECK_EQ(n.vDone.size(), 1u);
        if (n.vDone.size() == 1)
        {
            CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Acked);
            CHECK_EQ(n.vDone[0].btAttempts, 2);
        }
        CHECK(n.vRx.empty());
        CHECK(isIdle(n));
    }
    {
        // The pulse comes back on every attempt: the result is Failed.
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        Acker a;
        a.iAddress = 0;
        sim.vAckers.push_back(&a);
        Script s;
        sim.vScripts.push_back(&s);
        // Bit 10 of each attempt; an attempt ends at the pulse and the next
        // starts three bit periods after the pulse is released.
        uint32_t uiStart = sim.uiNow + CEC_WIRE_FREE_NEW_BITS * kBit;
        for (int i = 0; i < CEC_WIRE_ATTEMPTS; i++)
        {
            const uint32_t uiBitAt = uiStart + CEC_WIRE_START_US + 10 * kBit;
            s.vSteps.push_back({uiBitAt + 900, true});
            s.vSteps.push_back({uiBitAt + 1200, false});
            uiStart = uiBitAt + 1200 + CEC_WIRE_FREE_RETRY_BITS * kBit;
        }
        CHECK(sim.send(n, f));
        sim.run(600000);
        CHECK_EQ(n.stWire.uiLineErrors, (uint32_t)CEC_WIRE_ATTEMPTS);
        CHECK_EQ(n.vDone.size(), 1u);
        if (n.vDone.size() == 1)
        {
            CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Failed);
            CHECK_EQ(n.vDone[0].btAttempts, CEC_WIRE_ATTEMPTS);
        }
        CHECK(isIdle(n));
    }
}

TEST(testWireLineHeldLow)
{
    {
        // Held low since before the engine started: no edge was ever reported.
        Sim sim;
        sim.hold(true);
        Node n;
        sim.add(n, kMask4);
        Acker a;
        a.iAddress = 0;
        sim.vAckers.push_back(&a);
        const fwCecFrame f = FRAME(0x40, 0x04);
        CHECK(sim.send(n, f));
        sim.run(3000000);
        CHECK(n.vDone.empty());
        CHECK(n.vDrive.empty());
        CHECK(!cecWireCanSend(n.stWire));

        CHECK(sim.cancel(n));
        CHECK(!sim.cancel(n));
        CHECK_EQ(n.vDone.size(), 1u);
        if (n.vDone.size() == 1)
        {
            CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Failed);
            CHECK_EQ(n.vDone[0].btAttempts, 0);
            CHECK(sameFrame(n.vDone[0].stFrame, f));
        }
        CHECK(cecWireCanSend(n.stWire));

        // Queued while held, sent once the line is let go.
        CHECK(sim.send(n, f));
        sim.run(100000);
        const uint32_t uiReleased = sim.uiNow;
        sim.hold(false);
        sim.run(300000);
        CHECK_EQ(n.vDone.size(), 2u);
        if (n.vDone.size() == 2)
            CHECK_RES(n.vDone[1].eResult, fwCecTxResult::Acked);
        const LineView v = decodeLine(sim.vLine);
        CHECK_EQ(v.vFrames.size(), 1u);
        if (v.vFrames.size() == 1)
            CHECK(v.vFrames[0].uiStartUs - uiReleased >= CEC_WIRE_FREE_NEW_BITS * kBit);
        CHECK(isIdle(n));
    }
    {
        // Pulled low while a frame waits: the frame goes out after the release.
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        Acker a;
        a.iAddress = 0;
        sim.vAckers.push_back(&a);
        sim.run(50000);
        sim.hold(true);
        sim.run(50000);
        CHECK(sim.send(n, FRAME(0x40, 0x04)));
        sim.run(500000);
        CHECK(n.vDone.empty());
        // A send in progress cannot be cancelled; this one has not begun.
        CHECK(n.stWire.eTx == fwCecWireTxState::WaitFree);
        sim.hold(false);
        sim.run(300000);
        CHECK_EQ(n.vDone.size(), 1u);
        if (n.vDone.size() == 1)
            CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Acked);
        CHECK_EQ(n.stWire.uiRxErrors, 0u);
        CHECK(isIdle(n));
    }
}

TEST(testWireCancelOnlyWhileWaiting)
{
    Sim sim;
    Node n;
    sim.add(n, kMask4);
    Acker a;
    a.iAddress = 0;
    sim.vAckers.push_back(&a);
    CHECK(!sim.cancel(n));                      // nothing queued
    CHECK(sim.send(n, FRAME(0x40, 0x04)));
    sim.run(CEC_WIRE_FREE_NEW_BITS * kBit + 6000);      // inside the first block
    CHECK(!sim.cancel(n));
    sim.run(300000);
    CHECK_EQ(n.vDone.size(), 1u);
    if (n.vDone.size() == 1)
        CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Acked);
}

// The timer's count runs over in the middle of the exchange.
TEST(testWireTimerWrap)
{
    rngSeed(111);
    Sim sim(0xFFFFFFFFu - 60000u);
    Node n;
    sim.add(n, kMask4, kDelayMeasured);
    Acker a;
    a.iAddress = 0;
    sim.vAckers.push_back(&a);
    Script s;
    sim.vScripts.push_back(&s);
    const fwCecFrame fTx = FRAME(0x40, 0xA5, 0x5A, 0x00, 0xFF, 0x12, 0x34);
    const fwCecFrame fRx = FRAME(0x04, 0x5A, 0xA5, 0xFF, 0x00, 0x43, 0x21);
    CHECK(sim.send(n, fTx));
    scriptFrame(s, sim.uiNow + 250000, fRx);
    sim.run(600000);
    CHECK((int32_t)sim.uiNow > 0 && sim.uiNow < 1000000u);      // it did run over
    CHECK_EQ(n.vDone.size(), 1u);
    if (n.vDone.size() == 1)
    {
        CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Acked);
        CHECK_EQ(n.vDone[0].btAttempts, 1);
    }
    CHECK_EQ(n.vRx.size(), 1u);
    if (n.vRx.size() == 1)
        CHECK(sameFrame(n.vRx[0].stFrame, fRx));
    CHECK(allSampled(s, 7, true));
    CHECK_EQ(n.stWire.uiRxErrors, 0u);
    CHECK(isIdle(n));
}

// ------------------------------------------------------------------ own pull, late handlers

// Every low pulse on the line is a 1, a 0, or at least an error signal long
// (start bits, error signals, a void bit): no runt and nothing between the
// windows, so no device on the cable reads a bit that was not sent.
bool lineLowsSafe(const std::vector<Transition> & vLine)
{
    for (size_t i = 0; i + 1 < vLine.size(); i++)
    {
        if (!vLine[i].bLow || vLine[i + 1].bLow)
            continue;
        const uint32_t uiLow = vLine[i + 1].uiUs - vLine[i].uiUs;
        const bool bOk = (uiLow >= kOneLowMin && uiLow <= kOneLowMax) ||
                         (uiLow >= kZeroLowMin && uiLow <= kZeroLowMax) || uiLow >= kErrorLowMin;
        if (!bOk)
            return false;
    }
    return true;
}

// The bits of a frame as the line shows them when every block is acknowledged.
std::vector<bool> frameBits(const fwCecFrame & stFrame)
{
    std::vector<bool> v;
    for (int iBlock = 0; iBlock < stFrame.btLen; iBlock++)
    {
        for (int i = 0; i < 8; i++)
            v.push_back(((stFrame.abtBlock[iBlock] >> (7 - i)) & 1) != 0);
        v.push_back(iBlock == stFrame.btLen - 1);
        v.push_back(false);
    }
    return v;
}

// Every attempt on the line, read bit by bit after its start bit up to the
// first low that is no bit, shows the bits of the frame and no other.
bool attemptsArePrefixes(const std::vector<Transition> & vLine, const fwCecFrame & stFrame)
{
    const std::vector<bool> vBits = frameBits(stFrame);
    std::vector<uint32_t> vLow;
    for (size_t i = 0; i + 1 < vLine.size(); i++)
        if (vLine[i].bLow && !vLine[i + 1].bLow)
            vLow.push_back(vLine[i + 1].uiUs - vLine[i].uiUs);
    size_t iBit = 0;
    bool bIn = false;
    for (uint32_t uiLow : vLow)
    {
        const bool bOne = uiLow >= kOneLowMin && uiLow <= kOneLowMax;
        const bool bZero = uiLow >= kZeroLowMin && uiLow <= kZeroLowMax;
        if (!bOne && !bZero)
        {
            bIn = uiLow >= kStartLowMin && uiLow <= kStartLowMax;
            iBit = 0;
            continue;
        }
        if (!bIn || iBit >= vBits.size() || vBits[iBit] != bOne)
            return false;
        iBit++;
    }
    return true;
}

TEST(testWirePullNotSeen)
{
    {
        // Sending: the end of the start bit's low shows the pull is lost; the frame fails at once.
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        n.bPullDead = true;
        Acker a;
        a.iAddress = 0;
        sim.vAckers.push_back(&a);
        CHECK(sim.send(n, FRAME(0x40, 0x04)));
        sim.run(1000000);
        CHECK_EQ(n.vDone.size(), 1u);
        if (n.vDone.size() == 1)
        {
            CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Failed);
            CHECK_EQ(n.vDone[0].btAttempts, 1);
        }
        CHECK_EQ(n.stWire.uiPullNotSeen, 1u);
        CHECK_EQ(n.stWire.uiTxRetries, 0u);
        CHECK_EQ(n.vDrive.size(), 2u);          // one pull, one release, and nothing after
        CHECK(sim.vLine.empty());
        CHECK(isIdle(n));
        CHECK(cecWireCanSend(n.stWire));
    }
    {
        // Frames queued one after another, a poll among them: each fails once.
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        n.bPullDead = true;
        const fwCecFrame aFrames[] = {FRAME(0x40, 0x04), FRAME(0x40), FRAME(0x4F, 0x82, 0x10, 0x00)};
        for (size_t i = 0; i < 3; i++)
        {
            CHECK(sim.send(n, aFrames[i]));
            CHECK(sim.runUntilDone(n, i + 1, 400000));
        }
        sim.run(500000);
        CHECK_EQ(n.vDone.size(), 3u);
        for (const fwCecWireTxDone & stDone : n.vDone)
        {
            CHECK_RES(stDone.eResult, fwCecTxResult::Failed);
            CHECK_EQ(stDone.btAttempts, 1);
        }
        CHECK_EQ(n.stWire.uiPullNotSeen, 3u);
        CHECK_EQ(n.stWire.uiLateAborts, 0u);
        CHECK_EQ(n.vDrive.size(), 6u);
        CHECK(isIdle(n));
    }
    {
        // Receiving: each acknowledge pulled into nothing is counted; the frame is still read.
        rngSeed(121);
        Sim sim;
        Node n;
        sim.add(n, kMask4, kDelayMeasured);
        n.bPullDead = true;
        Script s;
        sim.vScripts.push_back(&s);
        const fwCecFrame f = FRAME(0x04, 0x44, 0x41);
        scriptFrame(s, sim.uiNow + 20000, f);
        sim.run(200000);
        CHECK_EQ(n.stWire.uiPullNotSeen, 3u);
        CHECK(allSampled(s, 3, false));         // the line never showed an acknowledge
        CHECK_EQ(n.vRx.size(), 1u);
        if (n.vRx.size() == 1)
        {
            CHECK(sameFrame(n.vRx[0].stFrame, f));
            CHECK_EQ(n.vRx[0].btFlags, CEC_WIRE_RX_FOR_US);
        }
        CHECK_EQ(n.stWire.uiErrorSignals, 0u);
        CHECK(isIdle(n));
    }
}

// One kind of point in a frame the sender's alarm is due at.
enum class TxPoint
{
    FrameStart,     // the line free: harmless when late
    StartRelease,
    FirstBitStart,
    OneRelease,
    ZeroRelease,
    BitStart,
    Sample,
};

bool txPointIs(const fwCecWire & stWire, TxPoint ePoint)
{
    switch (ePoint)
    {
        case TxPoint::FrameStart:    return stWire.eTx == fwCecWireTxState::WaitFree;
        case TxPoint::StartRelease:  return stWire.eTx == fwCecWireTxState::StartLow;
        case TxPoint::FirstBitStart: return stWire.eTx == fwCecWireTxState::StartHigh;
        case TxPoint::OneRelease:    return stWire.eTx == fwCecWireTxState::BitLow && stWire.bTxBitOne;
        case TxPoint::ZeroRelease:   return stWire.eTx == fwCecWireTxState::BitLow && !stWire.bTxBitOne;
        case TxPoint::BitStart:      return stWire.eTx == fwCecWireTxState::BitHigh && stWire.btTxBit < 9;
        case TxPoint::Sample:        return stWire.eTx == fwCecWireTxState::BitSample;
    }
    return false;
}

TEST(testWireLateTransmitAlarm)
{
    // 0x40: a 1 in the second initiator bit; 0xA5, 0x5A: both values throughout.
    const fwCecFrame f = FRAME(0x40, 0xA5, 0x5A);
    struct Case
    {
        TxPoint ePoint;
        int iNth;           // the alarm of that kind in the first attempt that runs late, from 1
    };
    const Case aCases[] = {
        {TxPoint::FrameStart, 1}, {TxPoint::StartRelease, 1}, {TxPoint::FirstBitStart, 1},
        {TxPoint::OneRelease, 1}, {TxPoint::OneRelease, 3}, {TxPoint::ZeroRelease, 5},
        {TxPoint::BitStart, 12}, {TxPoint::Sample, 1}, {TxPoint::Sample, 3},
    };
    for (const Case & c : aCases)
    {
        for (uint32_t uiLate : {250u, 450u, 700u, 1000u, 1600u, 3000u})
        {
            Sim sim;
            Node n, r;
            sim.add(n, kMask4);
            sim.add(r, 1u << 0);
            int iSeen = 0;
            n.fnAlarmExtra = [&](const fwCecWire & stWire) -> uint32_t
            {
                if (stWire.btTxAttempt > 1 || !txPointIs(stWire, c.ePoint))
                    return 0;
                return ++iSeen == c.iNth ? uiLate : 0;
            };
            CHECK(sim.send(n, f));
            sim.run(600000);

            const bool bHarmless = c.ePoint == TxPoint::FrameStart;
            CHECK_EQ(iSeen >= c.iNth, true);
            CHECK_EQ(n.vDone.size(), 1u);
            if (n.vDone.size() == 1)
            {
                CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Acked);
                CHECK_EQ(n.vDone[0].btAttempts, bHarmless ? 1 : 2);
            }
            CHECK_EQ(n.stWire.uiLateAborts, bHarmless ? 0u : 1u);
            CHECK_EQ(n.stWire.uiTxRetries, bHarmless ? 0u : 1u);
            CHECK(lineLowsSafe(sim.vLine));
            CHECK(attemptsArePrefixes(sim.vLine, f));
            const LineView v = decodeLine(sim.vLine);
            CHECK(!v.vFrames.empty());
            if (!v.vFrames.empty())
            {
                const LineFrame & stLast = v.vFrames.back();
                CHECK(stLast.bComplete && lineFrameIs(stLast, f));
                CHECK_EQ(stLast.iHeadViolations + stLast.iBodyViolations, 0);
            }
            // The receiver took the frame once, from the attempt that was whole, and never signalled an error.
            CHECK_EQ(r.vRx.size(), 1u);
            if (r.vRx.size() == 1)
                CHECK(sameFrame(r.vRx[0].stFrame, f));
            CHECK_EQ(r.stWire.uiErrorSignals, 0u);
            CHECK_EQ(n.stWire.uiPullNotSeen + r.stWire.uiPullNotSeen, 0u);
            CHECK(isIdle(n) && isIdle(r));
            CHECK(!sim.bStuck);
        }
    }
    {
        // Late in every attempt: the attempts run out, the result is Failed, nothing is delivered.
        Sim sim;
        Node n, r;
        sim.add(n, kMask4);
        sim.add(r, 1u << 0);
        n.fnAlarmExtra = [](const fwCecWire & stWire) -> uint32_t
        {
            return txPointIs(stWire, TxPoint::OneRelease) && stWire.btTxBlock == 1 ? 300u : 0u;
        };
        CHECK(sim.send(n, f));
        sim.run(800000);
        CHECK_EQ(n.vDone.size(), 1u);
        if (n.vDone.size() == 1)
        {
            CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Failed);
            CHECK_EQ(n.vDone[0].btAttempts, CEC_WIRE_ATTEMPTS);
        }
        CHECK_EQ(n.stWire.uiLateAborts, (uint32_t)CEC_WIRE_ATTEMPTS);
        CHECK_EQ(n.stWire.uiTxRetries, (uint32_t)(CEC_WIRE_ATTEMPTS - 1));
        CHECK(lineLowsSafe(sim.vLine));
        CHECK(attemptsArePrefixes(sim.vLine, f));
        CHECK(r.vRx.empty());
        CHECK_EQ(r.stWire.uiErrorSignals, 0u);
        CHECK(isIdle(n) && isIdle(r));
    }
    {
        // The frame's start falls due while interrupts are held off, and another
        // device's start bit goes by in the meantime: the start waits for the
        // edges held off with it, and the frame goes out after the other one.
        Sim sim;
        Node n, r;
        sim.add(n, kMask4);
        sim.add(r, 1u << 0);
        Script s;
        sim.vScripts.push_back(&s);
        const fwCecFrame fOther = FRAME(0x30, 0x8F);
        const uint32_t uiT = sim.uiNow + 50000;
        scriptFrame(s, uiT, fOther);
        n.bBlock = true;
        n.uiBlockFrom = uiT - 100;
        n.uiBlockTo = uiT + 4000;
        sim.run(50000 + 50);
        CHECK(sim.send(n, f));
        sim.run(400000);
        CHECK_EQ(n.vDone.size(), 1u);
        if (n.vDone.size() == 1)
        {
            CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Acked);
            CHECK_EQ(n.vDone[0].btAttempts, 1);
        }
        const LineView v = decodeLine(sim.vLine);
        CHECK_EQ(v.vFrames.size(), 2u);
        if (v.vFrames.size() == 2)
        {
            CHECK(v.vFrames[0].bComplete && lineFrameIs(v.vFrames[0], fOther));
            CHECK(v.vFrames[1].bComplete && lineFrameIs(v.vFrames[1], f));
            CHECK_EQ(v.vFrames[0].iBodyViolations + v.vFrames[1].iBodyViolations, 0);
        }
        CHECK_EQ(r.vRx.size(), 2u);
        CHECK(isIdle(n) && isIdle(r));
    }
    {
        // At the limit nothing is abandoned: every release and bit start 200 us late.
        Sim sim;
        Node n, r;
        sim.add(n, kMask4);
        sim.add(r, 1u << 0);
        n.fnAlarmExtra = [](const fwCecWire & stWire) -> uint32_t
        {
            return stWire.eTx != fwCecWireTxState::WaitFree ? CEC_WIRE_LATE_MAX_US : 0u;
        };
        CHECK(sim.send(n, f));
        sim.run(400000);
        CHECK_EQ(n.vDone.size(), 1u);
        if (n.vDone.size() == 1)
        {
            CHECK_RES(n.vDone[0].eResult, fwCecTxResult::Acked);
            CHECK_EQ(n.vDone[0].btAttempts, 1);
        }
        CHECK_EQ(n.stWire.uiLateAborts, 0u);
        CHECK_EQ(r.vRx.size(), 1u);
    }
}

// A frame received with one handler of the receiver held off or given two edges at once.
struct RxFault
{
    int iPairFall = -1;
    int iPairRise = -1;
    int iBlockFall = -1;            // interrupts held off from this falling edge of the frame ...
    int iBlockRise = -1;            // ... or from this rising edge ...
    uint32_t uiBlockUs = 0;         // ... for this long
};

TEST(testWireLateOrCoalescedReceive)
{
    const fwCecFrame aFrames[] = {FRAME(0x05, 0xA5, 0x5A), FRAME(0x04, 0xA5, 0x5A)};
    BitTiming stShort;
    stShort.uiBit = kBitMin;
    stShort.uiStart = kStartMin;
    const BitTiming aTimings[] = {BitTiming{}, stShort};

    std::vector<RxFault> vFaults;
    for (int k : {1, 5, 10, 13, 20, 21, 30})
    {
        RxFault stFault;
        stFault.iPairFall = k;
        vFaults.push_back(stFault);
    }
    for (int k : {0, 4, 10, 15, 19})
    {
        RxFault stFault;
        stFault.iPairRise = k;
        vFaults.push_back(stFault);
    }
    for (int k : {1, 10, 13, 16, 20, 30})
        for (uint32_t uiBlock : {150u, 300u, 450u, 600u, 800u, 1000u, 1400u, 2000u, 3000u})
        {
            RxFault stFault;
            stFault.iBlockFall = k;
            stFault.uiBlockUs = uiBlock;
            vFaults.push_back(stFault);
        }
    // The rise of a 1 held off long enough to read as a 0.
    for (int k : {11, 13, 18})
        for (uint32_t uiBlock : {300u, 600u, 900u, 1200u})
        {
            RxFault stFault;
            stFault.iBlockRise = k;
            stFault.uiBlockUs = uiBlock;
            vFaults.push_back(stFault);
        }

    int iDropped = 0, iDelivered = 0, iPairDrops = 0;
    for (const fwCecFrame & f : aFrames)
    {
        const bool bOurs = (f.abtBlock[0] & 0x0F) == 4;
        for (const BitTiming & t : aTimings)
        {
            for (const RxFault & stFault : vFaults)
            {
                rngSeed(131 + (uint32_t)iDropped);
                Sim sim;
                Node n;
                sim.add(n, kMask4, 40);
                Script s;
                sim.vScripts.push_back(&s);
                Acker a;
                a.iAddress = 5;
                sim.vAckers.push_back(&a);
                const uint32_t uiT0 = sim.uiNow + 20000;
                scriptFrame(s, uiT0, f, t);
                scriptFrame(s, uiT0 + 150000, kAfter);
                n.iPairFall = stFault.iPairFall;
                n.iPairRise = stFault.iPairRise;
                if (stFault.iBlockFall >= 0 || stFault.iBlockRise >= 0)
                {
                    n.bBlock = true;
                    n.uiBlockFrom = stFault.iBlockFall >= 0 ? s.vSteps[2 * (size_t)stFault.iBlockFall].uiUs
                                                            : s.vSteps[2 * (size_t)stFault.iBlockRise + 1].uiUs;
                    n.uiBlockTo = n.uiBlockFrom + stFault.uiBlockUs;
                }
                sim.run(400000);

                // No error signal, on the counters or on the line: the only long lows are the two start bits.
                CHECK_EQ(n.stWire.uiErrorSignals, 0u);
                CHECK_EQ(lineLowCount(sim.vLine, kErrorLowMin, 1000000), 2);
                // What is handed over is what was sent, and the receiver recovers for the next frame.
                bool bGotF = false, bGotAfter = false;
                for (const fwCecWireRxFrame & stRx : n.vRx)
                {
                    const bool bF = sameFrame(stRx.stFrame, f);
                    const bool bAfter = sameFrame(stRx.stFrame, kAfter);
                    CHECK(bF || bAfter);
                    bGotF = bGotF || bF;
                    bGotAfter = bGotAfter || bAfter;
                }
                CHECK(bGotAfter);
                if (!bOurs)
                {
                    for (const Transition & stDrive : n.vDrive)
                        CHECK(stDrive.uiUs - uiT0 >= 150000u);      // only the acknowledges of kAfter
                    CHECK(!bGotF);
                }
                else
                {
                    // A frame dropped is not acknowledged in its last block: its sender sends it again.
                    const size_t iSamples = (size_t)(f.btLen + kAfter.btLen);
                    CHECK_EQ(s.vSampledLow.size(), iSamples);
                    if (s.vSampledLow.size() == iSamples)
                        CHECK(bGotF || !s.vSampledLow[f.btLen - 1]);
                    if (bGotF)
                        iDelivered++;
                }
                // Two edges in one call inside the frame always lose it: dropped, or
                // given up when the held edge outlasts the wait for it.
                if (stFault.iPairFall >= 1 || stFault.iPairRise >= 1)
                {
                    CHECK(n.stWire.uiRxDropped + n.stWire.uiRxErrors >= 1u);
                    CHECK(!bGotF);
                    iPairDrops += (int)n.stWire.uiRxDropped;
                }
                if (!bGotF)
                    iDropped++;
                CHECK(isIdle(n));
                CHECK(!sim.bStuck);
            }
        }
    }
    // Both outcomes occurred: short hold-offs are ridden out, long ones drop the frame.
    CHECK(iDropped > 0);
    CHECK(iDelivered > 0);
    CHECK(iPairDrops > 0);

    // A hold-off the lateness checks cannot see: bit periods of 2060 us, and a
    // falling edge held off 380 us from just after the check before it, so the
    // check inside the hold-off is served under the limit. The next period
    // measures 1680 us, short only by what the late handler explains.
    for (const fwCecFrame & f : aFrames)
    {
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        Script s;
        sim.vScripts.push_back(&s);
        BitTiming t;
        t.uiBit = 2060;
        const uint32_t uiT0 = sim.uiNow + 20000;
        scriptFrame(s, uiT0, f, t);
        scriptFrame(s, uiT0 + 150000, kAfter);
        n.bBlock = true;
        n.uiBlockFrom = s.vSteps[2 * 13].uiUs + sim.uiSenseUs;
        n.uiBlockTo = n.uiBlockFrom + 380;
        sim.run(400000);
        CHECK_EQ(n.stWire.uiErrorSignals, 0u);
        CHECK_EQ(lineLowCount(sim.vLine, kErrorLowMin, 1000000), 2);
        CHECK_EQ(n.stWire.uiRxDropped, 1u);
        CHECK_EQ(n.vRx.size(), 1u);
        if (n.vRx.size() == 1)
            CHECK(sameFrame(n.vRx[0].stFrame, kAfter));
        CHECK(isIdle(n));
    }

    // Monitoring a poll nobody acknowledges, the rise of its acknowledge bit
    // held off long enough to read as a 0: it is not handed over as acknowledged.
    for (uint32_t uiBlock : {300u, 700u, 1100u})
    {
        Sim sim;
        Node n;
        sim.add(n, kMask4);
        cecWireSetMonitorAll(n.stWire, true);
        Script s;
        sim.vScripts.push_back(&s);
        scriptFrame(s, sim.uiNow + 20000, FRAME(0x06));
        n.bBlock = true;
        n.uiBlockFrom = s.vSteps[2 * 10 + 1].uiUs;
        n.uiBlockTo = n.uiBlockFrom + uiBlock;
        sim.run(200000);
        for (const fwCecWireRxFrame & stRx : n.vRx)
            CHECK_EQ(stRx.btFlags & CEC_WIRE_RX_ACKED, 0);
        CHECK(n.vDrive.empty());
        CHECK(isIdle(n));
    }
}

// ------------------------------------------------------------------ fuzz

// Three engines at addresses 0, 4 and 8 send random frames to each other, to
// an address nobody has, and to everyone, at random moments. What must hold is
// counted here and judged by the caller.
struct FuzzTally
{
    int iSent = 0;
    int iAcked = 0;
    int iNotAcked = 0;
    int iFailed = 0;
    int iWrongResult = 0;       // not acknowledged though the destination is there, or the reverse
    int iWrongDelivery = 0;     // a frame missing, repeated, altered or out of order at its destination
    int iNotIdle = 0;           // an engine not back at rest at the end
    int iBodyViolations = 0;    // bits on the line outside the standard's limits
    int iErrorSignals = 0;
    int iLineErrors = 0;
    int iOverruns = 0;
    int iArbitrationLost = 0;
    int iLateAborts = 0;
    int iRxDropped = 0;
    bool bStuck = false;

    bool clean() const
    {
        return iWrongResult == 0 && iWrongDelivery == 0 && iNotIdle == 0 && iBodyViolations == 0 &&
               iErrorSignals == 0 && iLineErrors == 0 && iOverruns == 0 && !bStuck && iSent == iAcked + iNotAcked + iFailed;
    }
};

FuzzTally fuzzRun(uint32_t uiSeed, uint32_t uiDelayMax, int iFrames)
{
    rngSeed(uiSeed);
    const int aiAddress[3] = {0, 4, 8};
    const int iAbsent = 5;
    Sim sim;
    Node aNodes[3];
    for (int i = 0; i < 3; i++)
        sim.add(aNodes[i], (uint16_t)(1u << aiAddress[i]), uiDelayMax);

    FuzzTally stTally;
    std::vector<fwCecFrame> avExpected[3];
    fwCecFrame aPending[3] = {};
    bool abPending[3] = {};
    uint32_t auiArbAtSend[3] = {};
    size_t aiDoneSeen[3] = {};
    uint32_t auiNextAt[3];
    for (int i = 0; i < 3; i++)
        auiNextAt[i] = sim.uiNow + rng() % 60000;

    while ((stTally.iSent < iFrames || abPending[0] || abPending[1] || abPending[2]) && !sim.bStuck)
    {
        sim.run(200);
        for (int i = 0; i < 3; i++)
        {
            Node & n = aNodes[i];
            while (aiDoneSeen[i] < n.vDone.size())
            {
                const fwCecWireTxDone & stDone = n.vDone[aiDoneSeen[i]++];
                const fwCecFrame & f = aPending[i];
                const int iDestination = f.abtBlock[0] & 0x0F;
                abPending[i] = false;
                auiNextAt[i] = sim.uiNow + rng() % 60000;
                if (!sameFrame(stDone.stFrame, f))
                    stTally.iWrongResult++;
                const bool bThere = iDestination != iAbsent;
                if (stDone.eResult == fwCecTxResult::Acked)
                {
                    stTally.iAcked++;
                    if (!bThere)
                        stTally.iWrongResult++;
                    // Handed to the destination, or to both others for a broadcast; a poll to nobody.
                    if (f.btLen >= 2)
                        for (int k = 0; k < 3; k++)
                            if (k != i && (iDestination == CEC_ADDR_BROADCAST || iDestination == aiAddress[k]))
                                avExpected[k].push_back(f);
                }
                else
                {
                    if (stDone.eResult == fwCecTxResult::NotAcked)
                        stTally.iNotAcked++;
                    else
                        stTally.iFailed++;
                    // Failed with arbitration lost in every attempt: it never reached the destination.
                    const bool bLostAll = stDone.eResult == fwCecTxResult::Failed &&
                                          n.stWire.uiArbitrationLost - auiArbAtSend[i] >= stDone.btAttempts;
                    if (bThere && !bLostAll)
                        stTally.iWrongResult++;
                }
            }
            if (!abPending[i] && stTally.iSent < iFrames && (int32_t)(sim.uiNow - auiNextAt[i]) >= 0)
            {
                fwCecFrame f = {};
                const uint32_t uiLen = rng() % 10;
                f.btLen = (uint8_t)(uiLen < 6 ? 1 + uiLen : uiLen < 9 ? 7 + rng() % 9 : CEC_FRAME_MAX);
                for (int k = 1; k < f.btLen; k++)
                    f.abtBlock[k] = (uint8_t)rng();
                const uint32_t uiTo = rng() % 8;
                const int iTo = uiTo < 5 ? aiAddress[(i + 1 + (int)(uiTo % 2)) % 3]
                                         : uiTo < 6 ? iAbsent : CEC_ADDR_BROADCAST;
                f.abtBlock[0] = (uint8_t)((aiAddress[i] << 4) | iTo);
                auiArbAtSend[i] = n.stWire.uiArbitrationLost;
                if (sim.send(n, f))
                {
                    aPending[i] = f;
                    abPending[i] = true;
                    stTally.iSent++;
                }
                else
                {
                    stTally.iWrongResult++;
                }
            }
        }
    }
    sim.run(50000);

    for (int i = 0; i < 3; i++)
    {
        const Node & n = aNodes[i];
        if (!isIdle(n))
            stTally.iNotIdle++;
        if (n.vRx.size() != avExpected[i].size())
            stTally.iWrongDelivery++;
        for (size_t k = 0; k < n.vRx.size() && k < avExpected[i].size(); k++)
            if (!sameFrame(n.vRx[k].stFrame, avExpected[i][k]) || n.vRx[k].btFlags != kRxBoth)
                stTally.iWrongDelivery++;
        stTally.iErrorSignals += (int)n.stWire.uiErrorSignals;
        stTally.iLineErrors += (int)n.stWire.uiLineErrors;
        stTally.iOverruns += (int)n.stWire.uiRxOverruns;
        stTally.iArbitrationLost += (int)n.stWire.uiArbitrationLost;
        stTally.iLateAborts += (int)n.stWire.uiLateAborts;
        stTally.iRxDropped += (int)n.stWire.uiRxDropped;
    }
    for (const LineFrame & stLine : decodeLine(sim.vLine).vFrames)
        stTally.iBodyViolations += stLine.iBodyViolations;
    stTally.bStuck = sim.bStuck;
    return stTally;
}

TEST(testWireFuzz)
{
    int iSent = 0, iAcked = 0, iArbitration = 0;
    for (uint32_t uiSeed = 1; uiSeed <= 12; uiSeed++)
    {
        const FuzzTally t = fuzzRun(0xCEC00 + uiSeed, (uiSeed % 3) * (kDelayMeasured / 2), 150);
        CHECK_EQ(t.iSent, 150);
        CHECK_EQ(t.iWrongResult, 0);
        CHECK_EQ(t.iWrongDelivery, 0);
        CHECK_EQ(t.iNotIdle, 0);
        CHECK_EQ(t.iBodyViolations, 0);
        CHECK_EQ(t.iErrorSignals, 0);
        CHECK_EQ(t.iLineErrors, 0);
        CHECK_EQ(t.iOverruns, 0);
        CHECK(!t.bStuck);
        CHECK(t.clean());
        iSent += t.iSent;
        iAcked += t.iAcked;
        iArbitration += t.iArbitrationLost;
    }
    // The run did exercise what it is for.
    CHECK(iAcked > iSent / 2);
    CHECK(iArbitration > 0);
}

// How late the two handlers may run before something on the line breaks.
// Each delay is drawn between 0 and the bound, per interrupt.
uint32_t s_uiDelayTolerated = 0;

TEST(testWireHandlerDelayMargin)
{
    const uint32_t auiDelay[] = {0, 60, 120, 140, 160, 180, 200, 240, 300, 400};
    uint32_t uiTolerated = 0;
    bool bBroken = false;
    for (uint32_t uiDelay : auiDelay)
    {
        bool bClean = true;
        for (uint32_t uiSeed = 1; uiSeed <= 4 && bClean; uiSeed++)
            bClean = fuzzRun(0xDE1A00 + uiSeed * 31 + uiDelay, uiDelay, 80).clean();
        if (bClean && !bBroken)
            uiTolerated = uiDelay;
        else
            bBroken = true;
    }
    s_uiDelayTolerated = uiTolerated;
    CHECK(uiTolerated >= kDelayMeasured);
    CHECK(bBroken);                 // the list reaches past the limit, so the limit is a measured one
}

// Past the delay the line tolerates, frames are lost and sent again, but a
// legal frame never draws the error signal and the engines never stick.
TEST(testWireLateHandlersNoErrorSignal)
{
    int iLateAborts = 0, iDropped = 0;
    for (uint32_t uiDelay : {CEC_WIRE_LATE_MAX_US, 240u, 300u, 400u})
    {
        for (uint32_t uiSeed = 1; uiSeed <= 3; uiSeed++)
        {
            const FuzzTally t = fuzzRun(0x1A7E00 + uiSeed * 37 + uiDelay, uiDelay, 60);
            CHECK_EQ(t.iErrorSignals, 0);
            CHECK_EQ(t.iNotIdle, 0);
            CHECK_EQ(t.iOverruns, 0);
            CHECK(!t.bStuck);
            CHECK_EQ(t.iSent, t.iAcked + t.iNotAcked + t.iFailed);
            iLateAborts += t.iLateAborts;
            iDropped += t.iRxDropped;
        }
    }
    // The delays did reach past the limit on both sides of the line.
    CHECK(iLateAborts > 0);
    CHECK(iDropped > 0);
}

} // namespace

void runWireTests()
{
    testWireReceivesAndAcknowledges();
    testWireBroadcastIsNeverPulled();
    testWireBroadcastRejectedByAnother();
    testWireFramesForOthers();
    testWirePoll();
    testWireAcknowledgesASetOfAddresses();
    testWireTimingCorners();
    testWireShortBitPeriod();
    testWireReceiveFaults();
    testWireRingOverrun();

    testWireSendsAndIsAcknowledged();
    testWireSendRefused();
    testWireNotAcknowledged();
    testWireBroadcastSend();
    testWireSendsTheAnswers();
    testWireAcknowledgeCorners();
    testWireFreeTime();
    testWireArbitration();
    testWireLineErrorWhileSending();
    testWireLineHeldLow();
    testWireCancelOnlyWhileWaiting();
    testWireTimerWrap();

    testWirePullNotSeen();
    testWireLateTransmitAlarm();
    testWireLateOrCoalescedReceive();

    testWireFuzz();
    testWireHandlerDelayMargin();
    testWireLateHandlersNoErrorSignal();

    printf("cec_tests: wire engine keeps to the standard's limits with handler delays up to %u us (simulated line)\n",
           (unsigned)s_uiDelayTolerated);
}
