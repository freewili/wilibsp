//
// fwCECWire -- see fwCECWire.h.
//
// Layout of this file:
//   1. placement, small helpers, what the caller applies
//   2. receiver: the edges of the line
//   3. sender: the steps of a frame
//   4. the alarm, the edge entry, the main-loop calls
//

#include "fwCECWire.h"

// ---------------------------------------------------------------------------
// 1. Placement and helpers
// ---------------------------------------------------------------------------

// The SDK puts PICO_ON_DEVICE=1 on every target compile line; the host tests
// never see an SDK header. A copy loop left to the optimiser can become a call
// to memcpy, which is in flash: the attribute keeps the loops as loops.
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"                   // __not_in_flash_func (target builds only)
#define CEC_RAMFUNC(fn)     __attribute__((optimize("no-tree-loop-distribute-patterns"))) __not_in_flash_func(fn)
#else
#define CEC_RAMFUNC(fn)     fn
#endif

static void CEC_RAMFUNC(copyFrame)(fwCecFrame & stTo, const fwCecFrame & stFrom)
{
    stTo.btLen = stFrom.btLen;
    for (int i = 0; i < CEC_FRAME_MAX; i++)
        stTo.abtBlock[i] = stFrom.abtBlock[i];
}

static bool CEC_RAMFUNC(txActive)(const fwCecWire & stWire)
{
    return stWire.eTx != fwCecWireTxState::Idle && stWire.eTx != fwCecWireTxState::WaitFree;
}

// The alarm due at uiAtUs is served more than CEC_WIRE_LATE_MAX_US late.
static bool CEC_RAMFUNC(servedLate)(uint32_t uiAtUs, uint32_t uiNowUs)
{
    return (int32_t)(uiNowUs - uiAtUs) > (int32_t)CEC_WIRE_LATE_MAX_US;
}

// The line state and the earliest of the three alarm times.
static void CEC_RAMFUNC(wireOut)(const fwCecWire & stWire, fwCecWireOut & stOut)
{
    stOut.bDriveLow = stWire.bTxLow || stWire.bFollowLow;
    stOut.bAlarm    = false;
    stOut.uiAlarmUs = 0;

    if (stWire.bTxAlarm)
    {
        stOut.bAlarm    = true;
        stOut.uiAlarmUs = stWire.uiTxAt;
    }
    if (stWire.bReleaseAlarm && (!stOut.bAlarm || (int32_t)(stWire.uiReleaseAt - stOut.uiAlarmUs) < 0))
    {
        stOut.bAlarm    = true;
        stOut.uiAlarmUs = stWire.uiReleaseAt;
    }
    if (stWire.bRxAlarm && (!stOut.bAlarm || (int32_t)(stWire.uiRxAt - stOut.uiAlarmUs) < 0))
    {
        stOut.bAlarm    = true;
        stOut.uiAlarmUs = stWire.uiRxAt;
    }
}

// The start time of a waiting frame: the line free for long enough, counted
// from the start of the last bit.
static void CEC_RAMFUNC(txSchedule)(fwCecWire & stWire, uint32_t uiNowUs)
{
    const uint32_t uiFreeUs = (uint32_t)stWire.btTxFreeBits * CEC_WIRE_BIT_US;
    uint32_t uiAt = uiNowUs;
    if ((uint32_t)(uiNowUs - stWire.uiLastBitUs) < uiFreeUs)
        uiAt = stWire.uiLastBitUs + uiFreeUs;
    stWire.uiTxAt   = uiAt;
    stWire.bTxAlarm = true;
}

// ---------------------------------------------------------------------------
// 2. Receiver
// ---------------------------------------------------------------------------

static void CEC_RAMFUNC(rxIdle)(fwCecWire & stWire, uint32_t uiNowUs)
{
    stWire.eRx      = fwCecWireRxState::Idle;
    stWire.bRxAlarm = false;
    if (stWire.eTx == fwCecWireTxState::WaitFree)
        txSchedule(stWire, uiNowUs);
}

// Waits for the next edge: lateness checks from uiCheckAt, the give-up at
// uiGiveUpAt. The checks stop at the give-up.
static void CEC_RAMFUNC(rxWait)(fwCecWire & stWire, uint32_t uiCheckAt, uint32_t uiGiveUpAt)
{
    stWire.uiRxGiveUpAt = uiGiveUpAt;
    stWire.uiRxAt       = uiCheckAt;
    stWire.bRxAlarm     = true;
}

// A falling edge that may begin a start bit.
static void CEC_RAMFUNC(rxStartCandidate)(fwCecWire & stWire, uint32_t uiNowUs)
{
    const uint32_t uiGiveUpAt = uiNowUs + CEC_WIRE_START_LOW_MAX_US + CEC_WIRE_GIVE_UP_US;
    stWire.eRx        = fwCecWireRxState::StartLow;
    stWire.uiRxFallUs = uiNowUs;
    stWire.bRxDoubt   = false;
    rxWait(stWire, uiGiveUpAt, uiGiveUpAt);
    stWire.bTxAlarm   = false;          // a waiting frame is rescheduled when the line is free again
}

// The times of the frame in progress cannot be trusted: it is dropped with no
// acknowledge and no error signal. An acknowledge already on the line runs to
// its release.
static void CEC_RAMFUNC(rxDrop)(fwCecWire & stWire, uint32_t uiNowUs)
{
    stWire.uiRxDropped++;
    rxIdle(stWire, uiNowUs);
}

// A lateness check while an edge is awaited. Served late with the line
// already past that edge, the edge's handler is held off as long: the bit is
// not trusted. In the low of a bit one check is enough (CEC_WIRE_LOW_CHECK_US);
// before a falling edge they repeat every CEC_WIRE_LATE_MAX_US.
static void CEC_RAMFUNC(rxCheck)(fwCecWire & stWire, uint32_t uiNowUs, bool bLineHigh)
{
    const bool bInLow = stWire.eRx == fwCecWireRxState::BitLow;
    if (servedLate(stWire.uiRxAt, uiNowUs) && bLineHigh == bInLow)
        stWire.bRxDoubt = true;

    uint32_t uiNext = uiNowUs + CEC_WIRE_LATE_MAX_US;
    if (bInLow || (int32_t)(uiNext - stWire.uiRxGiveUpAt) > 0)
        uiNext = stWire.uiRxGiveUpAt;
    stWire.uiRxAt = uiNext;
}

// The destination of the frame in progress, once its header block is in.
static uint8_t CEC_RAMFUNC(rxDestination)(const fwCecWire & stWire)
{
    return (uint8_t)((stWire.stRx.btLen == 0 ? stWire.btRxByte : stWire.stRx.abtBlock[0]) & 0x0F);
}

// The falling edge that begins a data, end of message or acknowledge bit.
static void CEC_RAMFUNC(rxBitFall)(fwCecWire & stWire, uint32_t uiNowUs)
{
    stWire.eRx        = fwCecWireRxState::BitLow;
    stWire.uiRxFallUs = uiNowUs;
    rxWait(stWire, uiNowUs + CEC_WIRE_LOW_CHECK_US, uiNowUs + CEC_WIRE_ZERO_LOW_MAX_US + CEC_WIRE_GIVE_UP_US);

    if (stWire.btRxBit == 9 && stWire.stRx.btLen < CEC_FRAME_MAX)
    {
        const uint8_t btDestination = rxDestination(stWire);
        if (btDestination != CEC_ADDR_BROADCAST && ((stWire.uiAckMask >> btDestination) & 1u) != 0)
        {
            stWire.bFollowLow    = true;
            stWire.uiReleaseAt   = uiNowUs + CEC_WIRE_ACK_HOLD_US;
            stWire.bReleaseAlarm = true;
        }
    }
}

// A bit period shorter than the minimum: the error signal.
static void CEC_RAMFUNC(rxShortPeriod)(fwCecWire & stWire, uint32_t uiNowUs)
{
    stWire.uiRxErrors++;
    stWire.uiErrorSignals++;
    stWire.bFollowLow    = true;
    stWire.uiReleaseAt   = uiNowUs + CEC_WIRE_ERROR_LOW_US;
    stWire.bReleaseAlarm = true;
    stWire.eRx           = fwCecWireRxState::LowWait;
    stWire.bRxAlarm      = false;
    stWire.bTxAlarm      = false;
}

static void CEC_RAMFUNC(rxFall)(fwCecWire & stWire, uint32_t uiNowUs)
{
    const uint32_t uiPeriodUs = uiNowUs - stWire.uiRxFallUs;
    stWire.uiLastBitUs = uiNowUs;

    switch (stWire.eRx)
    {
        case fwCecWireRxState::Idle:
        case fwCecWireRxState::LowWait:     // the release was not reported, or is still to be
            rxStartCandidate(stWire, uiNowUs);
            break;

        case fwCecWireRxState::StartHigh:
            if (stWire.bRxDoubt)
            {
                stWire.uiRxDropped++;
                rxStartCandidate(stWire, uiNowUs);
                break;
            }
            if (uiPeriodUs < CEC_WIRE_START_MIN_US || uiPeriodUs > CEC_WIRE_START_MAX_US)
            {
                stWire.uiRxErrors++;
                rxStartCandidate(stWire, uiNowUs);
                break;
            }
            stWire.stRx.btLen     = 0;
            stWire.btRxBit        = 0;
            stWire.btRxByte       = 0;
            stWire.bRxEom         = false;
            stWire.bRxAllAcked    = true;
            stWire.bLastFrameOurs = false;
            rxBitFall(stWire, uiNowUs);
            break;

        case fwCecWireRxState::BitHigh:
            // Timed by a late handler, or short only by what one can explain.
            if (stWire.bRxDoubt || (uiPeriodUs < CEC_WIRE_BIT_MIN_US && uiPeriodUs >= CEC_WIRE_ERROR_PERIOD_MAX_US))
            {
                stWire.uiRxDropped++;
                rxStartCandidate(stWire, uiNowUs);
                break;
            }
            if (uiPeriodUs < CEC_WIRE_BIT_MIN_US)
            {
                rxShortPeriod(stWire, uiNowUs);
                break;
            }
            if (uiPeriodUs > CEC_WIRE_BIT_MAX_US)
            {
                stWire.uiRxErrors++;
                rxStartCandidate(stWire, uiNowUs);
                break;
            }
            rxBitFall(stWire, uiNowUs);
            break;

        default:
            // A falling edge inside the low part of a bit: an edge went missing.
            stWire.uiRxErrors++;
            rxStartCandidate(stWire, uiNowUs);
            break;
    }
}

// The frame in stRx is complete.
static void CEC_RAMFUNC(rxFrame)(fwCecWire & stWire)
{
    const uint8_t btDestination = (uint8_t)(stWire.stRx.abtBlock[0] & 0x0F);
    const bool    bForUs = btDestination == CEC_ADDR_BROADCAST || ((stWire.uiAckMask >> btDestination) & 1u) != 0;

    stWire.uiRxFrames++;
    // A poll is answered by its acknowledge alone.
    if (!stWire.bMonitorAll && !(bForUs && stWire.stRx.btLen >= 2))
        return;

    const uint8_t btHead = stWire.btRxHead;
    const uint8_t btNext = (uint8_t)((btHead + 1u) & (CEC_WIRE_RX_RING - 1u));
    if (btNext == stWire.btRxTail)
    {
        stWire.uiRxOverruns++;
        return;
    }
    copyFrame(stWire.astRx[btHead].stFrame, stWire.stRx);
    stWire.astRx[btHead].btFlags = (uint8_t)((stWire.bRxAllAcked ? CEC_WIRE_RX_ACKED : 0) |
                                             (bForUs ? CEC_WIRE_RX_FOR_US : 0));
    stWire.btRxHead = btNext;
}

static void CEC_RAMFUNC(rxRise)(fwCecWire & stWire, uint32_t uiNowUs)
{
    const uint32_t uiLowUs = uiNowUs - stWire.uiRxFallUs;

    switch (stWire.eRx)
    {
        case fwCecWireRxState::StartLow:
            if (uiLowUs < CEC_WIRE_START_LOW_MIN_US || uiLowUs > CEC_WIRE_START_LOW_MAX_US)
            {
                rxIdle(stWire, uiNowUs);        // not a start bit
                break;
            }
            stWire.eRx = fwCecWireRxState::StartHigh;
            rxWait(stWire, stWire.uiRxFallUs + CEC_WIRE_START_MIN_US,
                   stWire.uiRxFallUs + CEC_WIRE_START_MAX_US + CEC_WIRE_GIVE_UP_US);
            break;

        case fwCecWireRxState::BitLow:
        {
            if (stWire.bRxDoubt)
            {
                rxDrop(stWire, uiNowUs);
                break;
            }

            bool bOne;
            if (uiLowUs >= CEC_WIRE_ONE_LOW_MIN_US && uiLowUs <= CEC_WIRE_ONE_LOW_MAX_US)
                bOne = true;
            else if (uiLowUs >= CEC_WIRE_ZERO_LOW_MIN_US && uiLowUs <= CEC_WIRE_ZERO_LOW_MAX_US)
                bOne = false;
            else
            {
                stWire.uiRxErrors++;
                rxIdle(stWire, uiNowUs);
                break;
            }

            if (stWire.btRxBit < 8)
            {
                stWire.btRxByte = (uint8_t)((stWire.btRxByte << 1) | (bOne ? 1u : 0u));
                stWire.btRxBit++;
            }
            else if (stWire.btRxBit == 8)
            {
                stWire.bRxEom = bOne;
                stWire.btRxBit++;
            }
            else
            {
                // Acknowledge: a low is an acknowledge, or for a broadcast a rejection.
                const bool bBroadcast = rxDestination(stWire) == CEC_ADDR_BROADCAST;
                if (bBroadcast ? !bOne : bOne)
                    stWire.bRxAllAcked = false;

                if (stWire.stRx.btLen >= CEC_FRAME_MAX)
                {
                    stWire.uiRxErrors++;
                    rxIdle(stWire, uiNowUs);
                    break;
                }
                stWire.stRx.abtBlock[stWire.stRx.btLen++] = stWire.btRxByte;
                if (stWire.bRxEom)
                {
                    rxFrame(stWire);
                    rxIdle(stWire, uiNowUs);
                    break;
                }
                stWire.btRxBit  = 0;
                stWire.btRxByte = 0;
            }
            stWire.eRx = fwCecWireRxState::BitHigh;
            rxWait(stWire, stWire.uiRxFallUs + CEC_WIRE_BIT_MIN_US,
                   stWire.uiRxFallUs + CEC_WIRE_BIT_MAX_US + CEC_WIRE_GIVE_UP_US);
            break;
        }

        case fwCecWireRxState::LowWait:
            stWire.uiLastBitUs = uiNowUs;
            rxIdle(stWire, uiNowUs);
            break;

        default:
            break;      // a rising edge with the line taken as high
    }
}

// The edge the receiver was waiting for did not come.
static void CEC_RAMFUNC(rxGiveUp)(fwCecWire & stWire, uint32_t uiNowUs)
{
    stWire.bRxAlarm = false;
    switch (stWire.eRx)
    {
        case fwCecWireRxState::StartLow:
            stWire.eRx = fwCecWireRxState::LowWait;     // held low: not a start bit
            break;
        case fwCecWireRxState::BitLow:
            stWire.uiRxErrors++;
            stWire.eRx = fwCecWireRxState::LowWait;
            break;
        case fwCecWireRxState::StartHigh:
        case fwCecWireRxState::BitHigh:
            stWire.uiRxErrors++;                        // the frame stopped before its end of message
            rxIdle(stWire, uiNowUs);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// 3. Sender
// ---------------------------------------------------------------------------

static void CEC_RAMFUNC(txDone)(fwCecWire & stWire, fwCecTxResult eResult)
{
    const uint8_t btHead = stWire.btTxDoneHead;
    const uint8_t btNext = (uint8_t)((btHead + 1u) & (CEC_WIRE_TX_DONE_RING - 1u));
    if (btNext != stWire.btTxDoneTail)
    {
        copyFrame(stWire.astTxDone[btHead].stFrame, stWire.stTx);
        stWire.astTxDone[btHead].eResult    = eResult;
        stWire.astTxDone[btHead].btAttempts = stWire.btTxAttempt;
        stWire.btTxDoneHead = btNext;
    }
    stWire.eTx      = fwCecWireTxState::Idle;
    stWire.bTxAlarm = false;
}

// The attempt in progress has failed: another one, or the result.
static void CEC_RAMFUNC(txFailed)(fwCecWire & stWire, uint32_t uiNowUs)
{
    stWire.bTxLow         = false;
    stWire.bLastFrameOurs = true;
    if (stWire.btTxAttempt >= stWire.btTxAttemptsMax)
    {
        txDone(stWire, stWire.bTxLineError ? fwCecTxResult::Failed : fwCecTxResult::NotAcked);
        return;
    }
    stWire.uiTxRetries++;
    stWire.btTxFreeBits = CEC_WIRE_FREE_RETRY_BITS;
    stWire.eTx          = fwCecWireTxState::WaitFree;
    stWire.bTxAlarm     = false;
    if (stWire.eRx == fwCecWireRxState::Idle)
        txSchedule(stWire, uiNowUs);
}

// Begins the bit at uiTxBitUs: pull low now, release after the low time of its value.
static void CEC_RAMFUNC(txBitStart)(fwCecWire & stWire, uint32_t uiNowUs)
{
    bool bOne;
    if (stWire.btTxBit < 8)
        bOne = ((stWire.stTx.abtBlock[stWire.btTxBlock] >> (7u - stWire.btTxBit)) & 1u) != 0;
    else if (stWire.btTxBit == 8)
        bOne = (stWire.btTxBlock + 1u >= stWire.stTx.btLen);     // end of message on the last block
    else
        bOne = true;                                             // the acknowledge bit is sent as a 1

    stWire.bTxBitOne   = bOne;
    stWire.bTxLow      = true;
    stWire.uiLastBitUs = uiNowUs;
    stWire.eTx         = fwCecWireTxState::BitLow;
    stWire.uiTxAt      = stWire.uiTxBitUs + (bOne ? CEC_WIRE_ONE_LOW_US : CEC_WIRE_ZERO_LOW_US);
    stWire.bTxAlarm    = true;
}

// The line read back in a bit sent as a 1.
static void CEC_RAMFUNC(txSample)(fwCecWire & stWire, uint32_t uiNowUs, bool bLineHigh)
{
    stWire.eTx    = fwCecWireTxState::BitHigh;
    stWire.uiTxAt = stWire.uiTxBitUs + CEC_WIRE_BIT_US;

    if (stWire.btTxBit == 9)
    {
        const bool bBroadcast = (stWire.stTx.abtBlock[0] & 0x0F) == CEC_ADDR_BROADCAST;
        if (bBroadcast ? !bLineHigh : bLineHigh)
            stWire.bTxNack = true;
        return;
    }
    if (bLineHigh)
        return;

    stWire.bTxLineError = true;
    if (stWire.btTxBlock == 0 && stWire.btTxBit < 4)
    {
        // Another initiator is sending a 0 in this address bit and has won.
        // The receiver takes the frame over with the bits both have sent.
        stWire.uiArbitrationLost++;
        stWire.stRx.btLen     = 0;
        stWire.btRxBit        = stWire.btTxBit;
        stWire.btRxByte       = (uint8_t)(stWire.stTx.abtBlock[0] >> (8u - stWire.btTxBit));
        stWire.bRxEom         = false;
        stWire.bRxAllAcked    = true;
        stWire.bRxDoubt       = false;
        stWire.uiRxFallUs     = stWire.uiTxBitUs;
        stWire.eRx            = fwCecWireRxState::BitLow;
        // The falling edge is this device's own bit start: no check of its handler.
        const uint32_t uiGiveUpAt = stWire.uiTxBitUs + CEC_WIRE_ZERO_LOW_MAX_US + CEC_WIRE_GIVE_UP_US;
        rxWait(stWire, uiGiveUpAt, uiGiveUpAt);
        txFailed(stWire, uiNowUs);
        stWire.bLastFrameOurs = false;
        return;
    }

    // A low nobody should be pulling.
    stWire.uiLineErrors++;
    stWire.eRx      = fwCecWireRxState::LowWait;
    stWire.bRxAlarm = false;
    txFailed(stWire, uiNowUs);
}

// The bit at uiTxBitUs is over.
static void CEC_RAMFUNC(txBitEnd)(fwCecWire & stWire, uint32_t uiNowUs)
{
    if (stWire.btTxBit < 9)
    {
        stWire.btTxBit++;
        stWire.uiTxBitUs += CEC_WIRE_BIT_US;
        txBitStart(stWire, uiNowUs);
        return;
    }

    if (stWire.bTxNack)
    {
        stWire.bTxLineError = false;
        txFailed(stWire, uiNowUs);
        return;
    }
    if (stWire.btTxBlock + 1u >= stWire.stTx.btLen)
    {
        stWire.bLastFrameOurs = true;
        txDone(stWire, fwCecTxResult::Acked);
        return;
    }
    stWire.btTxBlock++;
    stWire.btTxBit = 0;
    stWire.uiTxBitUs += CEC_WIRE_BIT_US;
    txBitStart(stWire, uiNowUs);
}

// The line read high while this device pulled it: the pull does not reach
// the line, and sending again would not change that. The frame fails now.
static void CEC_RAMFUNC(txPullNotSeen)(fwCecWire & stWire)
{
    stWire.uiPullNotSeen++;
    stWire.bTxLow = false;
    txDone(stWire, fwCecTxResult::Failed);
}

// The step due at uiTxAt moves an edge of the frame or reads the line back.
// The start of a frame, the release of a void bit and the end of the last
// bit do neither.
static bool CEC_RAMFUNC(txLateMatters)(const fwCecWire & stWire)
{
    if (stWire.eTx == fwCecWireTxState::WaitFree || stWire.eTx == fwCecWireTxState::Void ||
        stWire.eTx == fwCecWireTxState::Idle)
        return false;
    if (stWire.eTx != fwCecWireTxState::BitHigh)
        return true;
    return stWire.btTxBit < 9 || (!stWire.bTxNack && stWire.btTxBlock + 1u < stWire.stTx.btLen);
}

// A step of the frame served too late. A bit low still pulled has already
// outgrown its value: it is held until the error signal's length from the
// bit start, which no device reads as a bit, and the attempt fails at its
// release. Anything else fails the attempt now.
static void CEC_RAMFUNC(txLateAbort)(fwCecWire & stWire, uint32_t uiNowUs)
{
    stWire.uiLateAborts++;
    stWire.bTxLineError = true;
    const uint32_t uiVoidEndUs = stWire.uiTxBitUs + CEC_WIRE_ERROR_LOW_US;
    if (stWire.eTx == fwCecWireTxState::BitLow && (int32_t)(uiVoidEndUs - uiNowUs) > 0)
    {
        stWire.eTx    = fwCecWireTxState::Void;
        stWire.uiTxAt = uiVoidEndUs;
        return;
    }
    txFailed(stWire, uiNowUs);
}

static void CEC_RAMFUNC(txStep)(fwCecWire & stWire, uint32_t uiNowUs, bool bLineHigh)
{
    // bLineHigh was read with this device's pull still on.
    if (stWire.bTxLow && bLineHigh)
    {
        txPullNotSeen(stWire);
        return;
    }
    if (txLateMatters(stWire) && servedLate(stWire.uiTxAt, uiNowUs))
    {
        txLateAbort(stWire, uiNowUs);
        return;
    }

    switch (stWire.eTx)
    {
        case fwCecWireTxState::WaitFree:
            stWire.bTxAlarm = false;
            if (stWire.eRx != fwCecWireRxState::Idle)
                break;                                  // rescheduled when the receiver is idle again
            if (!bLineHigh)
            {
                stWire.eRx = fwCecWireRxState::LowWait; // held low with no edge reported
                break;
            }
            if (servedLate(stWire.uiTxAt, uiNowUs))
            {
                // An edge held off with this alarm has its handler run first.
                stWire.uiTxAt   = uiNowUs + CEC_WIRE_LATE_MAX_US;
                stWire.bTxAlarm = true;
                break;
            }
            stWire.btTxAttempt++;
            stWire.bTxNack      = false;
            stWire.bTxLineError = false;
            stWire.uiTxBitUs    = uiNowUs;
            stWire.uiLastBitUs  = uiNowUs;
            stWire.bTxLow       = true;
            stWire.eTx          = fwCecWireTxState::StartLow;
            stWire.uiTxAt       = uiNowUs + CEC_WIRE_START_LOW_US;
            stWire.bTxAlarm     = true;
            break;

        case fwCecWireTxState::StartLow:
            stWire.bTxLow = false;
            stWire.eTx    = fwCecWireTxState::StartHigh;
            stWire.uiTxAt = stWire.uiTxBitUs + CEC_WIRE_START_US;
            break;

        case fwCecWireTxState::StartHigh:
            stWire.btTxBlock = 0;
            stWire.btTxBit   = 0;
            stWire.uiTxBitUs += CEC_WIRE_START_US;
            txBitStart(stWire, uiNowUs);
            break;

        case fwCecWireTxState::BitLow:
            stWire.bTxLow = false;
            if (stWire.bTxBitOne)
            {
                stWire.eTx    = fwCecWireTxState::BitSample;
                stWire.uiTxAt = stWire.uiTxBitUs + CEC_WIRE_SAMPLE_US;
            }
            else
            {
                stWire.eTx    = fwCecWireTxState::BitHigh;
                stWire.uiTxAt = stWire.uiTxBitUs + CEC_WIRE_BIT_US;
            }
            break;

        case fwCecWireTxState::BitSample:
            txSample(stWire, uiNowUs, bLineHigh);
            break;

        case fwCecWireTxState::BitHigh:
            txBitEnd(stWire, uiNowUs);
            break;

        case fwCecWireTxState::Void:
            txFailed(stWire, uiNowUs);
            break;

        default:
            stWire.bTxAlarm = false;
            break;
    }
}

// ---------------------------------------------------------------------------
// 4. Entry points
// ---------------------------------------------------------------------------

void CEC_RAMFUNC(cecWireOnAlarm)(fwCecWire & stWire, uint32_t uiNowUs, bool bLineHigh, fwCecWireOut & stOut)
{
    if (stWire.bReleaseAlarm && (int32_t)(uiNowUs - stWire.uiReleaseAt) >= 0)
    {
        // bLineHigh was read with the acknowledge or the error signal still pulled.
        if (bLineHigh)
            stWire.uiPullNotSeen++;
        stWire.bReleaseAlarm = false;
        stWire.bFollowLow    = false;
    }
    if (stWire.bRxAlarm && (int32_t)(uiNowUs - stWire.uiRxAt) >= 0)
    {
        if ((int32_t)(uiNowUs - stWire.uiRxGiveUpAt) >= 0)
            rxGiveUp(stWire, uiNowUs);
        else
            rxCheck(stWire, uiNowUs, bLineHigh);
    }
    if (stWire.bTxAlarm && (int32_t)(uiNowUs - stWire.uiTxAt) >= 0)
        txStep(stWire, uiNowUs, bLineHigh);

    wireOut(stWire, stOut);
}

void CEC_RAMFUNC(cecWireOnEdge)(fwCecWire & stWire, bool bFalling, uint32_t uiNowUs, fwCecWireOut & stOut)
{
    // The sender reads the line from its alarm; the edges of its own frame are not decoded.
    if (!txActive(stWire))
    {
        if (bFalling)
            rxFall(stWire, uiNowUs);
        else
            rxRise(stWire, uiNowUs);
    }
    wireOut(stWire, stOut);
}

void CEC_RAMFUNC(cecWireOnEdgePair)(fwCecWire & stWire, bool bFirstFalling, uint32_t uiNowUs, fwCecWireOut & stOut)
{
    if (!txActive(stWire))
    {
        const fwCecWireRxState eRx = stWire.eRx;
        if (eRx == fwCecWireRxState::StartHigh || eRx == fwCecWireRxState::BitLow || eRx == fwCecWireRxState::BitHigh)
        {
            // Inside a frame neither time may decide a bit.
            stWire.uiLastBitUs = uiNowUs;
            rxDrop(stWire, uiNowUs);
            if (!bFirstFalling)
                rxStartCandidate(stWire, uiNowUs);  // the line is low again
        }
        else if (bFirstFalling)
        {
            // Outside a frame the pair decides nothing: a low of no length is no start bit.
            rxFall(stWire, uiNowUs);
            rxRise(stWire, uiNowUs);
        }
        else
        {
            rxRise(stWire, uiNowUs);
            rxFall(stWire, uiNowUs);
        }
    }
    wireOut(stWire, stOut);
}

bool CEC_RAMFUNC(cecWireSend)(fwCecWire & stWire, const fwCecFrame & stFrame, uint32_t uiNowUs, fwCecWireOut & stOut)
{
    const bool bOk = cecWireCanSend(stWire) && stFrame.btLen >= 1 && stFrame.btLen <= CEC_FRAME_MAX;
    if (bOk)
    {
        copyFrame(stWire.stTx, stFrame);
        stWire.btTxAttempt     = 0;
        stWire.btTxAttemptsMax = stFrame.btLen == 1 ? CEC_WIRE_POLL_ATTEMPTS : CEC_WIRE_ATTEMPTS;
        stWire.btTxFreeBits    = stWire.bLastFrameOurs ? CEC_WIRE_FREE_SAME_BITS : CEC_WIRE_FREE_NEW_BITS;
        stWire.eTx             = fwCecWireTxState::WaitFree;
        stWire.bTxAlarm        = false;
        if (stWire.eRx == fwCecWireRxState::Idle)
            txSchedule(stWire, uiNowUs);
    }
    wireOut(stWire, stOut);
    return bOk;
}

bool CEC_RAMFUNC(cecWireCancel)(fwCecWire & stWire, fwCecWireOut & stOut)
{
    const bool bOk = stWire.eTx == fwCecWireTxState::WaitFree;
    if (bOk)
        txDone(stWire, fwCecTxResult::Failed);
    wireOut(stWire, stOut);
    return bOk;
}

void cecWireInit(fwCecWire & stWire, uint32_t uiNowUs)
{
    stWire.uiAckMask      = 0;
    stWire.bMonitorAll    = false;
    stWire.bTxLow         = false;
    stWire.bFollowLow     = false;
    stWire.bLastFrameOurs = false;
    stWire.uiLastBitUs    = uiNowUs;

    stWire.eRx         = fwCecWireRxState::Idle;
    stWire.btRxBit     = 0;
    stWire.btRxByte    = 0;
    stWire.bRxEom      = false;
    stWire.bRxAllAcked  = false;
    stWire.bRxDoubt     = false;
    stWire.uiRxFallUs   = 0;
    stWire.uiRxGiveUpAt = 0;
    stWire.stRx.btLen   = 0;

    stWire.eTx             = fwCecWireTxState::Idle;
    stWire.btTxBlock       = 0;
    stWire.btTxBit         = 0;
    stWire.bTxBitOne       = false;
    stWire.bTxNack         = false;
    stWire.bTxLineError    = false;
    stWire.btTxAttempt     = 0;
    stWire.btTxAttemptsMax = 0;
    stWire.btTxFreeBits    = CEC_WIRE_FREE_NEW_BITS;
    stWire.uiTxBitUs       = 0;
    stWire.stTx.btLen      = 0;

    stWire.bTxAlarm      = false;
    stWire.bReleaseAlarm = false;
    stWire.bRxAlarm      = false;
    stWire.uiTxAt        = 0;
    stWire.uiReleaseAt   = 0;
    stWire.uiRxAt        = 0;

    stWire.btRxHead     = 0;
    stWire.btRxTail     = 0;
    stWire.btTxDoneHead = 0;
    stWire.btTxDoneTail = 0;

    stWire.uiRxFrames        = 0;
    stWire.uiRxErrors        = 0;
    stWire.uiRxOverruns      = 0;
    stWire.uiErrorSignals    = 0;
    stWire.uiArbitrationLost = 0;
    stWire.uiLineErrors      = 0;
    stWire.uiTxRetries       = 0;
    stWire.uiPullNotSeen     = 0;
    stWire.uiLateAborts      = 0;
    stWire.uiRxDropped       = 0;
}

void cecWireSetAckMask(fwCecWire & stWire, uint16_t uiAckMask)
{
    stWire.uiAckMask = (uint16_t)(uiAckMask & 0x7FFFu);     // address 15 is never acknowledged
}

void cecWireSetMonitorAll(fwCecWire & stWire, bool bMonitorAll)
{
    stWire.bMonitorAll = bMonitorAll;
}

bool cecWirePopRx(fwCecWire & stWire, fwCecWireRxFrame & stRx)
{
    const uint8_t btTail = stWire.btRxTail;
    if (btTail == stWire.btRxHead)
        return false;
    stRx = stWire.astRx[btTail];
    stWire.btRxTail = (uint8_t)((btTail + 1u) & (CEC_WIRE_RX_RING - 1u));
    return true;
}

bool cecWirePopTxDone(fwCecWire & stWire, fwCecWireTxDone & stDone)
{
    const uint8_t btTail = stWire.btTxDoneTail;
    if (btTail == stWire.btTxDoneHead)
        return false;
    stDone = stWire.astTxDone[btTail];
    stWire.btTxDoneTail = (uint8_t)((btTail + 1u) & (CEC_WIRE_TX_DONE_RING - 1u));
    return true;
}
