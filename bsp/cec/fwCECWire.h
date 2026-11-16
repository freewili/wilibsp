//
// fwCECWire -- the CEC bit engine: start bit, data bits, end of message,
// acknowledge, arbitration, the bus-free wait, retransmission and the error
// signal, as a state machine with no hardware in it. Its caller owns the pin
// and one timer alarm: rmpLib/fwCEC on the target, a bus simulator in the host
// tests (testprojects/cec_tests). The caller reports every edge of the line
// and every alarm, and applies what comes back: hold the line low or release
// it, and when to call again.
//
// The line is open drain: a device pulls it low or lets it go, and it reads
// low while any device pulls. The engine never asks for a driven high.
//
// Receiving (CEC 5, 6): a bit is measured from the edges. The time from one
// falling edge to the next is the bit period; the low time tells a 1 from a
// 0. A block is ten bits: eight of data, most significant first, the end of
// message bit, the acknowledge bit. A frame directly addressed to one of the
// addresses in the acknowledge mask is acknowledged by pulling the
// acknowledge bit low from its falling edge. A broadcast is never pulled low:
// that would reject it. A bit period shorter than the minimum is answered
// with the error signal (CEC 7.4), only when
// the times that measured it are clean: a frame whose times a late handler or
// two edges in one call may have moved is dropped with no acknowledge and no
// error signal. While an edge of a frame is awaited, the alarm comes back to
// check that the handlers run on time: once in the low of a bit, every
// CEC_WIRE_LATE_MAX_US before the next falling edge.
//
// Sending (CEC 7, 9): a frame waits for the line to have been free for 3 bit
// periods when it is a retry, 5 when another device sent last, 7 when this
// device did, counted from the start of the last bit. The sender reads the
// line back in every bit it sends as a 1: during its own address bits a low
// means another initiator won the arbitration, and the engine carries on as
// the receiver of that frame; later it means a line error. In the acknowledge
// bit a low means acknowledged, or for a broadcast rejected. A frame that
// fails is sent again up to CEC_WIRE_ATTEMPTS times in all, a poll
// CEC_WIRE_POLL_ATTEMPTS, and the result is reported once. At the end of
// every low it pulls the sender reads the line: high there means its pull
// does not reach the line, and the frame fails at once. An alarm of a frame
// in progress served more than CEC_WIRE_LATE_MAX_US late ends the attempt: a
// bit whose low is already too long is held low past any bit's length, so no
// device reads it as a bit, and the frame is sent again after the free time.
//
// Contexts: cecWireOnEdge(), cecWireOnEdgePair() and cecWireOnAlarm() run in
// interrupt handlers on one core at one priority, so none interrupts another. Everything else
// runs in the main loop. cecWireSend() and cecWireCancel() are called with
// those two interrupts held off. The setters store one value each and need no
// guard. The two rings are filled by the handlers and emptied by the main
// loop without a lock.
//
// On the target every function a handler reaches, and the two called with
// the interrupts held off, are placed in RAM and call nothing in flash.
//
// Style: Allman braces, b/bt/i/ui prefixes (fwModbusCore).
//

#pragma once

#include <stdint.h>

#include "fwCECCore.h"

// ---- bit timing in microseconds (CEC 5) -------------------------------------
#define CEC_WIRE_BIT_US             2400u   // data bit period
#define CEC_WIRE_START_LOW_US       3700u
#define CEC_WIRE_START_US           4500u   // start bit period
#define CEC_WIRE_ONE_LOW_US         600u
#define CEC_WIRE_ZERO_LOW_US        1500u
#define CEC_WIRE_SAMPLE_US          1050u   // where the initiator reads the line back

// What a follower pulls is timed from the falling edge as its handler saw it
// and released by the alarm. Each may run late and neither early, so both
// holds sit just above the shortest the standard allows: an acknowledge is
// low for 1.3..1.7 ms from the edge, the error signal for 1.4..1.6 bit
// periods, 3.36..3.84 ms.
#define CEC_WIRE_ACK_HOLD_US        1350u
#define CEC_WIRE_ERROR_LOW_US       3400u

// Receive limits: the standard's windows widened by the slack, which covers
// the delay of the two edge handlers that bound a measurement.
#define CEC_WIRE_SLACK_US           200u
#define CEC_WIRE_START_LOW_MIN_US   (3500u - CEC_WIRE_SLACK_US)
#define CEC_WIRE_START_LOW_MAX_US   (3900u + CEC_WIRE_SLACK_US)
#define CEC_WIRE_START_MIN_US       (4300u - CEC_WIRE_SLACK_US)
#define CEC_WIRE_START_MAX_US       (4700u + CEC_WIRE_SLACK_US)
#define CEC_WIRE_BIT_MIN_US         (2050u - CEC_WIRE_SLACK_US)
#define CEC_WIRE_BIT_MAX_US         (2750u + CEC_WIRE_SLACK_US)
#define CEC_WIRE_ONE_LOW_MIN_US     (400u - CEC_WIRE_SLACK_US)
#define CEC_WIRE_ONE_LOW_MAX_US     (800u + CEC_WIRE_SLACK_US)
#define CEC_WIRE_ZERO_LOW_MIN_US    (1300u - CEC_WIRE_SLACK_US)
#define CEC_WIRE_ZERO_LOW_MAX_US    (1700u + CEC_WIRE_SLACK_US)
// How long after a limit the receiver gives up waiting for the edge.
#define CEC_WIRE_GIVE_UP_US         100u

// Every window of the standard is its nominal time +-200 us: a 1 low 600 of
// 400..800, a 0 low 1500 of 1300..1700, the start low 3700 of 3500..3900, the
// start period 4500 of 4300..4700. A sender's alarm served later than this
// moves an edge out of its window: a release lengthens the low past its
// maximum, a bit start shortens the low that the on-time release ends below
// its minimum. Also the step of the receiver's lateness checks.
#define CEC_WIRE_LATE_MAX_US        200u
// A bit period below this is answered with the error signal. A period short
// by no more than CEC_WIRE_SLACK_US + CEC_WIRE_LATE_MAX_US (2050 - 400 =
// 1650 us) can come from a falling-edge handler late by less than the
// lateness checks see; the frame is dropped instead.
#define CEC_WIRE_ERROR_PERIOD_MAX_US    (CEC_WIRE_BIT_MIN_US - CEC_WIRE_LATE_MAX_US)
// Where the receiver checks its lateness inside the low of a bit: after the
// longest 1, early enough that a rise handler held past it far enough to make
// a 1 read as a 0 (CEC_WIRE_ZERO_LOW_MIN_US) shows as more than
// CEC_WIRE_LATE_MAX_US late.
#define CEC_WIRE_LOW_CHECK_US       (CEC_WIRE_ONE_LOW_US + CEC_WIRE_LATE_MAX_US)

// ---- sending (CEC 7.1, 9.1) --------------------------------------------------
#define CEC_WIRE_FREE_RETRY_BITS    3       // bit periods the line is free before a retry
#define CEC_WIRE_FREE_NEW_BITS      5       // ... before a frame when another device sent last
#define CEC_WIRE_FREE_SAME_BITS     7       // ... before a frame when this device sent last
#define CEC_WIRE_ATTEMPTS           4       // sends of one frame in all
#define CEC_WIRE_POLL_ATTEMPTS      2       // sends of one poll in all

#define CEC_WIRE_RX_RING            8       // powers of two
#define CEC_WIRE_TX_DONE_RING       4

// ---- what the caller applies after every call --------------------------------
struct fwCecWireOut
{
    bool     bDriveLow;     // hold the line low; false = release it
    bool     bAlarm;        // call cecWireOnAlarm() at uiAlarmUs; false = no alarm wanted
    uint32_t uiAlarmUs;
};

// ---- what the handlers hand to the main loop ---------------------------------
#define CEC_WIRE_RX_ACKED           0x01    // every block acknowledged; a broadcast: not rejected
#define CEC_WIRE_RX_FOR_US          0x02    // a broadcast, or addressed to the acknowledge mask
static_assert(CEC_WIRE_RX_ACKED == CEC_RX_ACKED && CEC_WIRE_RX_FOR_US == CEC_RX_FOR_US, "CEC_RX_* drifted");

struct fwCecWireRxFrame
{
    fwCecFrame stFrame;
    uint8_t    btFlags;     // CEC_WIRE_RX_*
};

struct fwCecWireTxDone
{
    fwCecFrame    stFrame;
    fwCecTxResult eResult;
    uint8_t       btAttempts;
};

// Declaration order is the console value.
enum class fwCecWireRxState : uint8_t
{
    Idle = 0,
    StartLow,       // low: a start bit, if it stays low long enough
    StartHigh,      // a valid start bit low; waiting for the first data bit
    BitLow,         // inside the low part of a bit
    BitHigh,        // the bit is read; waiting for the next one
    LowWait,        // held low outside any bit; waiting for the release
};

// Declaration order is the console value.
enum class fwCecWireTxState : uint8_t
{
    Idle = 0,
    WaitFree,       // a frame is waiting for the line to be free
    StartLow,
    StartHigh,
    BitLow,
    BitSample,      // a 1 has been released; the line is read back next
    BitHigh,
    Void,           // a bit served too late, held low past any bit's length; the attempt fails at its release
};

struct fwCecWire
{
    // Set from the main loop.
    uint16_t uiAckMask;                 // bit n: frames to logical address n are acknowledged
    bool     bMonitorAll;               // hand every frame on the wire to the main loop

    // The line.
    bool     bTxLow;                    // the sender is pulling
    bool     bFollowLow;                // the receiver is pulling: an acknowledge or the error signal
    bool     bLastFrameOurs;            // this device sent the last frame
    uint32_t uiLastBitUs;               // falling edge of the last bit on the line; cecWireInit(): its time

    // Receiver.
    fwCecWireRxState eRx;
    uint8_t    btRxBit;                 // 0..7 data, 8 end of message, 9 acknowledge
    uint8_t    btRxByte;
    bool       bRxEom;
    bool       bRxAllAcked;
    bool       bRxDoubt;                // a handler of this frame ran late: no decision rests on its times
    uint32_t   uiRxFallUs;              // falling edge of the bit in progress
    uint32_t   uiRxGiveUpAt;            // the receiver gives up on the edge it waits for
    fwCecFrame stRx;                    // blocks received so far

    // Sender.
    fwCecWireTxState eTx;
    uint8_t    btTxBlock;
    uint8_t    btTxBit;
    bool       bTxBitOne;
    bool       bTxNack;                 // a block was not acknowledged, or a broadcast was rejected
    bool       bTxLineError;            // the last failed attempt ended on the line, not on an acknowledge
    uint8_t    btTxAttempt;
    uint8_t    btTxAttemptsMax;
    uint8_t    btTxFreeBits;
    uint32_t   uiTxBitUs;               // start of the bit being sent
    fwCecFrame stTx;

    // The three things the alarm is for.
    bool     bTxAlarm;
    bool     bReleaseAlarm;             // end of the acknowledge or of the error signal
    bool     bRxAlarm;                  // the receiver checks its lateness, or gives up on the edge it waits for
    uint32_t uiTxAt;
    uint32_t uiReleaseAt;
    uint32_t uiRxAt;

    // Rings: head written by the handlers, tail by the main loop.
    volatile uint8_t btRxHead;
    volatile uint8_t btRxTail;
    volatile uint8_t btTxDoneHead;
    volatile uint8_t btTxDoneTail;
    fwCecWireRxFrame astRx[CEC_WIRE_RX_RING];
    fwCecWireTxDone  astTxDone[CEC_WIRE_TX_DONE_RING];

    // Counters.
    uint32_t uiRxFrames;                // complete frames seen on the line, whoever they were for
    uint32_t uiRxErrors;                // frames given up on: a bad bit, a missing edge, too many blocks
    uint32_t uiRxOverruns;              // frames dropped with the ring full
    uint32_t uiErrorSignals;            // error signals sent
    uint32_t uiArbitrationLost;
    uint32_t uiLineErrors;              // sends stopped by a low nobody should have pulled
    uint32_t uiTxRetries;
    uint32_t uiPullNotSeen;             // lows this device pulled that the line did not show
    uint32_t uiLateAborts;              // attempts ended by an alarm served too late
    uint32_t uiRxDropped;               // frames dropped silently: their times could not be trusted
};

// The line counts as busy until uiNowUs: the first frame waits its free time from there.
void cecWireInit(fwCecWire & stWire, uint32_t uiNowUs);

// ---- main loop, with the two interrupts held off ------------------------------
// Queues one frame. False, and nothing changes, while a frame is already
// waiting or being sent, or for a frame of no blocks or
// more than CEC_FRAME_MAX.
bool cecWireSend(fwCecWire & stWire, const fwCecFrame & stFrame, uint32_t uiNowUs, fwCecWireOut & stOut);
// Gives up on a frame that is still waiting for the line, which a line held
// low never frees: it is reported as Failed. False once the frame is being sent.
bool cecWireCancel(fwCecWire & stWire, fwCecWireOut & stOut);

// ---- main loop ----------------------------------------------------------------
void cecWireSetAckMask(fwCecWire & stWire, uint16_t uiAckMask);
void cecWireSetMonitorAll(fwCecWire & stWire, bool bMonitorAll);
static inline bool cecWireCanSend(const fwCecWire & stWire)
{
    return stWire.eTx == fwCecWireTxState::Idle;
}
bool cecWirePopRx(fwCecWire & stWire, fwCecWireRxFrame & stRx);
bool cecWirePopTxDone(fwCecWire & stWire, fwCecWireTxDone & stDone);

// ---- handlers -----------------------------------------------------------------
// An edge of the line, with the time the handler read. Edges this device
// causes are reported like any other.
void cecWireOnEdge(fwCecWire & stWire, bool bFalling, uint32_t uiNowUs, fwCecWireOut & stOut);
// Both edges of a pulse before the handler ran, bFirstFalling the direction
// of the first: the handler saw both flags at once, or one flag with the line
// already back where it was. Neither time is known.
void cecWireOnEdgePair(fwCecWire & stWire, bool bFirstFalling, uint32_t uiNowUs, fwCecWireOut & stOut);
// The alarm, with the time and the level of the line the handler read before
// it applies anything: where this device was pulling, the level with the pull
// still on. The lateness is the time against the alarm the engine asked for.
void cecWireOnAlarm(fwCecWire & stWire, uint32_t uiNowUs, bool bLineHigh, fwCecWireOut & stOut);
