// bsp/cec/cec_control.cpp — see cec_control.h.
//
// Layout of this file:
//   1. state and helpers
//   2. one exchange: queued, in flight, awaiting its answer
//   3. the jobs, step by step
//   4. starting a request
//   5. the capture
#include "cec_control.h"

#include <stddef.h>
#include <string.h>

#include "fwCECCore.h"

static_assert(sizeof(cec_frame_t) == sizeof(fwCecFrame) && offsetof(cec_frame_t, block) == offsetof(fwCecFrame, abtBlock),
              "frame layouts differ");
static_assert(CEC_CONTROL_ANSWER_MS == CEC_REPLY_MS, "answer time differs from the standard's");
static_assert((CEC_CONTROL_CAPTURE & (CEC_CONTROL_CAPTURE - 1)) == 0, "capture size not a power of two");
static_assert(CEC_ANSWER_NONE == CEC_REPLY_NONE, "no-answer value differs");
static_assert(CEC_SCAN_ADDRESSES == 15, "scan covers 0..14");
static_assert(sizeof(((cec_reply_t *)0)->name) == CEC_OSD_NAME_MAX + 1 &&
              sizeof(((cec_scan_entry_t *)0)->name) == CEC_OSD_NAME_MAX + 1, "name sizes differ");

// ---------------------------------------------------------------------------
// 1. State and helpers
// ---------------------------------------------------------------------------

namespace {

enum class Job : uint8_t
{
    None = 0,
    Single,         // one exchange
    SystemInfo,     // physical address, then CEC version, vendor ID, and the TV's menu language
    Key,            // press, repeats while held, release
    Volume,         // press and release, again and again
    Scan,           // the bus scan of the core
};

// Steps of the jobs with more than one exchange.
const uint8_t STEP_INFO_PHYS_ADDR = 0;
const uint8_t STEP_INFO_VERSION   = 1;
const uint8_t STEP_INFO_VENDOR    = 2;
const uint8_t STEP_INFO_LANGUAGE  = 3;
const uint8_t STEP_KEY_PRESS      = 0;
const uint8_t STEP_KEY_REPEAT     = 1;
const uint8_t STEP_KEY_RELEASE    = 2;
const uint8_t STEP_VOLUME_PRESS   = 0;
const uint8_t STEP_VOLUME_RELEASE = 1;

// The initiator of a frame before it leaves; cec_control_next() writes the real one.
const uint8_t FROM_LATER = CEC_ADDR_UNREGISTERED;

struct Control
{
    Job         eJob;
    uint8_t     btStep;
    uint8_t     btDestination;

    // The exchange: queued until it is due, in flight until its result, then
    // awaiting its answer.
    bool        bQueued;
    bool        bTimed;             // queued with a time to wait for (uiDueMs)
    bool        bInFlight;
    bool        bAwaiting;
    bool        bWatchRefusal;      // the answer awaited is a refusal: none is SENT, one is REFUSED
    int         iAnswer;            // the opcode awaited, CEC_REPLY_NONE
    uint32_t    uiDueMs;
    uint32_t    uiLimitMs;          // in flight or awaiting: given up at
    fwCecFrame  stFrame;

    uint8_t     btKey;              // Key, Volume: [UI Command]
    uint32_t    uiHoldMs;           // Key
    uint32_t    uiHoldEndMs;        // Key: the release is due
    uint8_t     btPresses;          // Volume

    bool        bFirstDone;         // stReply.result holds the first exchange's outcome
    bool        bResultReady;       // a finished request not taken yet
    cec_reply_t stReply;

    bool        bScanValid;         // stScan holds a finished or running scan
    fwCecScan   stScan;
};

struct Capture
{
    cec_capture_mode_t  eMode;
    uint8_t             btHead;     // the oldest frame
    uint8_t             btCount;
    uint32_t            uiDropped;
    cec_capture_frame_t astFrame[CEC_CONTROL_CAPTURE];
};

Control s_control;
Capture s_capture;

inline const fwCecFrame &core_frame(const cec_frame_t *frame) { return *reinterpret_cast<const fwCecFrame *>(frame); }

inline bool reached(uint32_t now_ms, uint32_t at_ms) { return (int32_t)(now_ms - at_ms) >= 0; }

// The frame went onto the wire and was acknowledged.
bool acked(cec_result_t result)
{
    return result == CEC_RESULT_ANSWERED || result == CEC_RESULT_SENT || result == CEC_RESULT_REFUSED ||
           result == CEC_RESULT_NO_ANSWER;
}

void reply_clear(cec_reply_t &reply)
{
    memset(&reply, 0, sizeof reply);
    reply.result       = CEC_RESULT_FAILED;
    reply.abort_reason = 0xFF;
    reply.power_status = CEC_POWER_STATUS_NONE;
    reply.menu_state   = 0xFF;
    reply.source       = CEC_ADDR_UNREGISTERED;
    reply.phys_addr    = CEC_PHYS_ADDR_NONE;
    reply.device_type  = CEC_TYPE_NONE;
    reply.vendor_id    = CEC_VENDOR_NONE;
}

// What an answer or a refusal tells, into the reply.
void take_answer(cec_reply_t &reply, const fwCecFrame &answer)
{
    const uint8_t *op = cecOperands(answer);
    const int      n  = cecOperandCount(answer);
    switch (cecOpcode(answer))
    {
    case CEC_OP_FEATURE_ABORT:
        if (n >= 2) reply.abort_reason = op[1];
        break;
    case CEC_OP_REPORT_POWER_STATUS:
        if (n >= 1) reply.power_status = op[0];
        break;
    case CEC_OP_MENU_STATUS:
        if (n >= 1) reply.menu_state = op[0];
        break;
    case CEC_OP_ACTIVE_SOURCE:
        if (n >= 2)
        {
            reply.source    = cecInitiator(answer);
            reply.phys_addr = cecGetPhysAddr(op);
        }
        break;
    case CEC_OP_REPORT_PHYSICAL_ADDRESS:
        cecParseReportPhysicalAddress(answer, reply.phys_addr, reply.device_type);
        break;
    case CEC_OP_DEVICE_VENDOR_ID:
        cecParseDeviceVendorId(answer, reply.vendor_id);
        break;
    case CEC_OP_CEC_VERSION:
        if (n >= 1) reply.cec_version = op[0];
        break;
    case CEC_OP_SET_MENU_LANGUAGE:
        if (n >= 3)
        {
            for (int i = 0; i < 3; i++)
            {
                const char ch = (char)op[i];
                reply.language[i] = ch > ' ' && ch <= '~' ? ch : '?';
            }
            reply.language[3] = 0;
        }
        break;
    case CEC_OP_SET_OSD_NAME:
        cecParseOsdName(answer, reply.name);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 2. One exchange
// ---------------------------------------------------------------------------

// frame (initiator FROM_LATER) leaves when due: now with timed false.
void queue(const fwCecFrame &frame, int answer, bool watch_refusal, bool timed, uint32_t due_ms)
{
    s_control.stFrame       = frame;
    s_control.iAnswer       = answer;
    s_control.bWatchRefusal = watch_refusal;
    s_control.bTimed        = timed;
    s_control.uiDueMs       = due_ms;
    s_control.bQueued       = true;
}

// A question with the answer the standard pairs with it.
void queue_question(uint8_t destination, uint8_t opcode)
{
    fwCecFrame frame;
    cecBuild(frame, FROM_LATER, destination, opcode);
    queue(frame, cecReplyOpcode(frame), false, false, 0);
}

void queue_key_frame(bool press, bool timed, uint32_t due_ms)
{
    fwCecFrame frame;
    if (press)
        cecBuildUserControlPressed(frame, FROM_LATER, s_control.btDestination, s_control.btKey);
    else
        cecBuild(frame, FROM_LATER, s_control.btDestination, CEC_OP_USER_CONTROL_RELEASED);
    queue(frame, CEC_REPLY_NONE, false, timed, due_ms);
}

void done(void)
{
    s_control.eJob         = Job::None;
    s_control.bQueued      = false;
    s_control.bInFlight    = false;
    s_control.bAwaiting    = false;
    s_control.bResultReady = true;
}

void advance(cec_result_t result, const fwCecFrame *answer, uint32_t now_ms);

void finish_exchange(cec_result_t result, const fwCecFrame *answer, uint32_t now_ms)
{
    s_control.bInFlight = false;
    s_control.bAwaiting = false;
    if (s_control.bWatchRefusal)
    {
        if (result == CEC_RESULT_ANSWERED)
            result = CEC_RESULT_REFUSED;
        else if (result == CEC_RESULT_NO_ANSWER)
            result = CEC_RESULT_SENT;
    }
    if (s_control.eJob != Job::Scan)
    {
        if (!s_control.bFirstDone)
        {
            s_control.bFirstDone   = true;
            s_control.stReply.result = result;
            if (answer != nullptr)
                *reinterpret_cast<fwCecFrame *>(&s_control.stReply.answer) = *answer;
        }
        if (answer != nullptr && (result == CEC_RESULT_ANSWERED || result == CEC_RESULT_REFUSED))
            take_answer(s_control.stReply, *answer);
    }
    advance(result, answer, now_ms);
}

// ---------------------------------------------------------------------------
// 3. The jobs
// ---------------------------------------------------------------------------

// The next repeat of a held key, or its release at the end of the hold.
void key_next(uint32_t now_ms)
{
    if ((int32_t)(s_control.uiHoldEndMs - (now_ms + CEC_CONTROL_REPEAT_MS)) > 0)
    {
        s_control.btStep = STEP_KEY_REPEAT;
        queue_key_frame(true, true, now_ms + CEC_CONTROL_REPEAT_MS);
    }
    else
    {
        s_control.btStep = STEP_KEY_RELEASE;
        queue_key_frame(false, true, s_control.uiHoldEndMs);
    }
}

void scan_next(void)
{
    fwCecFrame frame;
    if (cecScanNext(s_control.stScan, FROM_LATER, frame))
    {
        queue(frame, cecReplyOpcode(frame), false, false, 0);
        return;
    }
    const uint16_t mask = cecScanFoundMask(s_control.stScan);
    s_control.stReply.result     = CEC_RESULT_SENT;
    s_control.stReply.found_mask = mask;
    for (int i = 0; i < CEC_SCAN_ADDRESSES; i++)
        if ((mask >> i) & 1u) s_control.stReply.found++;
    done();
}

void advance(cec_result_t result, const fwCecFrame *answer, uint32_t now_ms)
{
    switch (s_control.eJob)
    {
    case Job::SystemInfo:
        if (s_control.btStep == STEP_INFO_PHYS_ADDR && !acked(result))
        {
            done();
            return;
        }
        s_control.btStep++;
        if (s_control.btStep == STEP_INFO_VERSION)
            queue_question(s_control.btDestination, CEC_OP_GET_CEC_VERSION);
        else if (s_control.btStep == STEP_INFO_VENDOR)
            queue_question(s_control.btDestination, CEC_OP_GIVE_DEVICE_VENDOR_ID);
        else if (s_control.btStep == STEP_INFO_LANGUAGE && s_control.btDestination == CEC_ADDR_TV)
            queue_question(CEC_ADDR_TV, CEC_OP_GET_MENU_LANGUAGE);
        else
            done();
        return;

    case Job::Key:
        if (s_control.btStep == STEP_KEY_RELEASE)
        {
            done();
        }
        else if (s_control.btStep == STEP_KEY_PRESS)
        {
            if (!acked(result))
            {
                done();
            }
            else if (s_control.uiHoldMs == 0)
            {
                s_control.btStep = STEP_KEY_RELEASE;
                queue_key_frame(false, false, 0);
            }
            else
            {
                s_control.uiHoldEndMs = now_ms + s_control.uiHoldMs;
                key_next(now_ms);
            }
        }
        else
        {
            key_next(now_ms);
        }
        return;

    case Job::Volume:
        if (s_control.btStep == STEP_VOLUME_PRESS)
        {
            if (!acked(result))
            {
                done();
                return;
            }
            s_control.btStep = STEP_VOLUME_RELEASE;
            queue_key_frame(false, false, 0);
            return;
        }
        s_control.stReply.sent++;
        if (s_control.stReply.sent >= s_control.btPresses)
        {
            done();
            return;
        }
        s_control.btStep = STEP_VOLUME_PRESS;
        queue_key_frame(true, true, now_ms + CEC_CONTROL_VOLUME_GAP_MS);
        return;

    case Job::Scan:
        cecScanTake(s_control.stScan, acked(result), result == CEC_RESULT_ANSWERED ? answer : nullptr);
        scan_next();
        return;

    default:
        done();
        return;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 4. Starting a request
// ---------------------------------------------------------------------------

void cec_control_init(void)
{
    memset(&s_control, 0, sizeof s_control);
    reply_clear(s_control.stReply);
    memset(&s_capture, 0, sizeof s_capture);
}

// A new request of job, if none runs.
static bool start(Job job, uint8_t destination)
{
    if (s_control.eJob != Job::None)
        return false;
    s_control.eJob          = job;
    s_control.btStep        = 0;
    s_control.btDestination = destination;
    s_control.bQueued       = false;
    s_control.bInFlight     = false;
    s_control.bAwaiting     = false;
    s_control.bFirstDone    = false;
    s_control.bResultReady  = false;
    reply_clear(s_control.stReply);
    return true;
}

// frame, built, as a request of one exchange.
static bool start_single(const fwCecFrame &frame, int answer, bool watch_refusal)
{
    if (!start(Job::Single, cecDestination(frame)))
        return false;
    queue(frame, answer, watch_refusal, false, 0);
    return true;
}

static bool start_question(uint8_t destination, uint8_t opcode, const uint8_t *operands, int count)
{
    fwCecFrame frame;
    if (destination >= CEC_ADDR_BROADCAST || !cecBuild(frame, FROM_LATER, destination, opcode, operands, count))
        return false;
    return start_single(frame, cecReplyOpcode(frame), false);
}

bool cec_control_raw(uint8_t destination, const uint8_t *data, int len, int answer)
{
    if (destination > CEC_ADDR_BROADCAST || len < 0 || len > CEC_FRAME_MAX - 1 || (len > 0 && data == nullptr) ||
        answer < CEC_ANSWER_STANDARD || answer > 0xFF)
        return false;
    fwCecFrame frame;
    if (len == 0)
    {
        cecBuildPoll(frame, FROM_LATER, destination);
        return start_single(frame, CEC_REPLY_NONE, false);
    }
    cecBuild(frame, FROM_LATER, destination, data[0], data + 1, len - 1);
    return start_single(frame, answer == CEC_ANSWER_STANDARD ? cecReplyOpcode(frame) : answer, false);
}

bool cec_control_power_status(uint8_t destination)
{
    return start_question(destination, CEC_OP_GIVE_DEVICE_POWER_STATUS, nullptr, 0);
}

bool cec_control_system_info(uint8_t destination)
{
    if (destination >= CEC_ADDR_BROADCAST || !start(Job::SystemInfo, destination))
        return false;
    s_control.btStep = STEP_INFO_PHYS_ADDR;
    queue_question(destination, CEC_OP_GIVE_PHYSICAL_ADDRESS);
    return true;
}

bool cec_control_osd_name(uint8_t destination)
{
    return start_question(destination, CEC_OP_GIVE_OSD_NAME, nullptr, 0);
}

bool cec_control_tell_osd_name(const char *name)
{
    if (name == nullptr)
        return false;
    const int len = (int)strnlen(name, CEC_OSD_NAME_MAX + 1);
    if (len < 1 || len > CEC_OSD_NAME_MAX)
        return false;
    for (int i = 0; i < len; i++)
        if (name[i] < 0x20 || name[i] > 0x7E) return false;
    fwCecFrame frame;
    cecBuild(frame, FROM_LATER, CEC_ADDR_TV, CEC_OP_SET_OSD_NAME, (const uint8_t *)name, len);
    // A TV that does not take the name refuses it; one that takes it says nothing.
    if (!start_single(frame, CEC_OP_FEATURE_ABORT, true))
        return false;
    memcpy(s_control.stReply.name, name, (size_t)len);
    s_control.stReply.name[len] = 0;
    return true;
}

bool cec_control_osd_string(uint8_t display_control, const char *text)
{
    const int len = text != nullptr ? (int)strnlen(text, CEC_OSD_STRING_MAX + 1) : 0;
    fwCecFrame frame;
    if (len > CEC_OSD_STRING_MAX || (display_control == CEC_DISPLAY_CLEAR && len != 0) ||
        !cecBuildSetOsdString(frame, FROM_LATER, CEC_ADDR_TV, display_control, text, len))
        return false;
    return start_single(frame, CEC_OP_FEATURE_ABORT, true);
}

bool cec_control_volume(int steps, uint8_t destination)
{
    if (steps < -CEC_CONTROL_VOLUME_MAX || steps > CEC_CONTROL_VOLUME_MAX || destination >= CEC_ADDR_BROADCAST ||
        !start(Job::Volume, destination))
        return false;
    s_control.btKey     = steps > 0 ? CEC_UI_VOLUME_UP : steps < 0 ? CEC_UI_VOLUME_DOWN : CEC_UI_MUTE;
    s_control.btPresses = (uint8_t)(steps > 0 ? steps : steps < 0 ? -steps : 1);
    s_control.btStep    = STEP_VOLUME_PRESS;
    queue_key_frame(true, false, 0);
    return true;
}

bool cec_control_key(uint8_t destination, uint8_t ui_command, uint32_t hold_ms)
{
    if (destination >= CEC_ADDR_BROADCAST || hold_ms > CEC_CONTROL_HOLD_MAX_MS || !start(Job::Key, destination))
        return false;
    s_control.btKey    = ui_command;
    s_control.uiHoldMs = hold_ms;
    s_control.btStep   = STEP_KEY_PRESS;
    queue_key_frame(true, false, 0);
    return true;
}

bool cec_control_active_source(void)
{
    fwCecFrame frame;
    cecBuildRequestActiveSource(frame, FROM_LATER);
    return start_single(frame, cecReplyOpcode(frame), false);
}

bool cec_control_stream_path(uint16_t phys_addr)
{
    fwCecFrame frame;
    cecBuildSetStreamPath(frame, FROM_LATER, phys_addr);
    return start_single(frame, cecReplyOpcode(frame), false);
}

bool cec_control_menu(uint8_t destination, uint8_t menu_request)
{
    if (menu_request > CEC_MENU_REQUEST_QUERY)
        return false;
    return start_question(destination, CEC_OP_MENU_REQUEST, &menu_request, 1);
}

bool cec_control_abort(uint8_t destination)
{
    return start_question(destination, CEC_OP_ABORT, nullptr, 0);
}

bool cec_control_feature_abort(uint8_t destination, uint8_t opcode, uint8_t reason)
{
    fwCecFrame frame;
    if (reason > CEC_ABORT_UNDETERMINED || !cecBuildFeatureAbortTo(frame, FROM_LATER, destination, opcode, reason))
        return false;
    return start_single(frame, CEC_REPLY_NONE, false);
}

bool cec_control_vendor_command(uint8_t destination, const uint8_t *data, int len)
{
    fwCecFrame frame;
    if (destination >= CEC_ADDR_BROADCAST || (len > 0 && data == nullptr) ||
        !cecBuild(frame, FROM_LATER, destination, CEC_OP_VENDOR_COMMAND, data, len))
        return false;
    return start_single(frame, CEC_REPLY_NONE, false);
}

bool cec_control_vendor_command_with_id(uint8_t destination, uint32_t vendor_id, const uint8_t *data, int len)
{
    fwCecFrame frame;
    if (!cecBuildVendorCommandWithId(frame, FROM_LATER, destination, vendor_id, data, len))
        return false;
    return start_single(frame, CEC_REPLY_NONE, false);
}

bool cec_control_bus_scan(uint16_t skip_mask)
{
    if (!start(Job::Scan, CEC_ADDR_BROADCAST))
        return false;
    cecScanStart(s_control.stScan, skip_mask);
    s_control.bScanValid = true;
    scan_next();
    return true;
}

bool cec_control_busy(void) { return s_control.eJob != Job::None; }

bool cec_control_result(cec_reply_t *out)
{
    if (!s_control.bResultReady)
        return false;
    s_control.bResultReady = false;
    *out = s_control.stReply;
    return true;
}

bool cec_control_scan_entry(uint8_t address, cec_scan_entry_t *out)
{
    if (!s_control.bScanValid || address >= CEC_SCAN_ADDRESSES || !s_control.stScan.astEntry[address].bPresent)
        return false;
    const fwCecScanEntry &entry = s_control.stScan.astEntry[address];
    out->phys_addr    = entry.uiPhysAddr;
    out->device_type  = entry.btDeviceType;
    out->power_status = entry.btPowerStatus;
    out->cec_version  = entry.btCecVersion;
    out->vendor_id    = entry.uiVendorId;
    memcpy(out->name, entry.szName, sizeof out->name);
    return true;
}

bool cec_control_next(uint8_t initiator, uint32_t now_ms, cec_frame_t *out)
{
    if (!s_control.bQueued || (s_control.bTimed && !reached(now_ms, s_control.uiDueMs)))
        return false;
    s_control.stFrame.abtBlock[0] = (uint8_t)((initiator << 4) | (s_control.stFrame.abtBlock[0] & 0x0F));
    s_control.bQueued   = false;
    s_control.bInFlight = true;
    s_control.uiLimitMs = now_ms + CEC_CONTROL_SEND_MS;
    *reinterpret_cast<fwCecFrame *>(out) = s_control.stFrame;
    return true;
}

void cec_control_on_sent(const cec_frame_t *frame, cec_engine_tx_result_t result, uint32_t now_ms)
{
    if (!s_control.bInFlight || frame->len != s_control.stFrame.btLen ||
        memcmp(frame->block, s_control.stFrame.abtBlock, frame->len) != 0)
        return;
    if (result == CEC_ENGINE_TX_ACKED)
    {
        if (s_control.iAnswer != CEC_REPLY_NONE)
        {
            s_control.bInFlight = false;
            s_control.bAwaiting = true;
            s_control.uiLimitMs = now_ms + CEC_CONTROL_ANSWER_MS;
            return;
        }
        finish_exchange(CEC_RESULT_SENT, nullptr, now_ms);
    }
    else
    {
        finish_exchange(result == CEC_ENGINE_TX_NOT_ACKED ? CEC_RESULT_NOT_ACKED : CEC_RESULT_FAILED, nullptr, now_ms);
    }
}

void cec_control_on_rx(const cec_frame_t *frame, uint32_t now_ms)
{
    if (!s_control.bAwaiting)
        return;
    const fwCecMatch match = cecMatchReply(core_frame(frame), cecDestination(s_control.stFrame),
                                           cecOpcode(s_control.stFrame), (uint8_t)s_control.iAnswer);
    if (match != fwCecMatch::None)
        finish_exchange(match == fwCecMatch::Reply ? CEC_RESULT_ANSWERED : CEC_RESULT_REFUSED, &core_frame(frame), now_ms);
}

void cec_control_tick(uint32_t now_ms)
{
    if (s_control.bInFlight && reached(now_ms, s_control.uiLimitMs))
        finish_exchange(CEC_RESULT_FAILED, nullptr, now_ms);
    else if (s_control.bAwaiting && reached(now_ms, s_control.uiLimitMs))
        finish_exchange(CEC_RESULT_NO_ANSWER, nullptr, now_ms);
}

void cec_control_stop(void)
{
    if (s_control.eJob == Job::None)
        return;
    s_control.stReply.result = CEC_RESULT_FAILED;
    done();
}

const char *cec_control_result_name(cec_result_t result)
{
    switch (result)
    {
    case CEC_RESULT_ANSWERED:  return "answered";
    case CEC_RESULT_SENT:      return "sent";
    case CEC_RESULT_REFUSED:   return "refused";
    case CEC_RESULT_NO_ANSWER: return "no_answer";
    case CEC_RESULT_NOT_ACKED: return "not_acked";
    case CEC_RESULT_FAILED:    return "failed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 5. The capture
// ---------------------------------------------------------------------------

void cec_control_capture_mode(cec_capture_mode_t mode)
{
    s_capture.eMode     = mode;
    s_capture.btHead    = 0;
    s_capture.btCount   = 0;
    s_capture.uiDropped = 0;
}

cec_capture_mode_t cec_control_capture_mode_now(void) { return s_capture.eMode; }

void cec_control_capture_push(const cec_frame_t *frame, uint8_t flags, uint32_t time_us)
{
    if (s_capture.eMode == CEC_CAPTURE_OFF ||
        (s_capture.eMode == CEC_CAPTURE_OURS && (flags & (CEC_FRAME_FOR_US | CEC_FRAME_SENT)) == 0))
        return;
    if (s_capture.btCount == CEC_CONTROL_CAPTURE)
    {
        s_capture.btHead = (uint8_t)((s_capture.btHead + 1u) & (CEC_CONTROL_CAPTURE - 1u));
        s_capture.btCount--;
        s_capture.uiDropped++;
    }
    cec_capture_frame_t &slot = s_capture.astFrame[(s_capture.btHead + s_capture.btCount) & (CEC_CONTROL_CAPTURE - 1u)];
    slot.time_us = time_us;
    slot.flags   = flags;
    slot.frame   = *frame;
    s_capture.btCount++;
}

bool cec_control_capture_pop(cec_capture_frame_t *out)
{
    if (s_capture.btCount == 0)
        return false;
    *out = s_capture.astFrame[s_capture.btHead];
    s_capture.btHead = (uint8_t)((s_capture.btHead + 1u) & (CEC_CONTROL_CAPTURE - 1u));
    s_capture.btCount--;
    return true;
}

uint32_t cec_control_capture_dropped(void) { return s_capture.uiDropped; }

void cec_control_capture_clear_dropped(void) { s_capture.uiDropped = 0; }
