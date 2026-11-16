//
// fwCECCore -- see fwCECCore.h.
//

#include "fwCECCore.h"

// ---- message table -----------------------------------------------------------
// Ascending opcode. Sources: the CEC specification, version 1.4, CEC 15 (Tables 8-26) and CEC 17 (Table
// 29); CEC 2.0 for 0x73 and 0xA5-0xA8. <Vendor Command> and <Vendor Remote
// Button Down> have a maximum only. <Report Power Status> is directly
// addressed in 1.4 and also broadcast in 2.0.
#define D   CEC_TO_DIRECT
#define B   CEC_TO_BROADCAST
#define E   CEC_TO_EITHER
#define U   CEC_MSG_FROM_UNREGISTERED
#define R   CEC_MSG_REPORT
#define F(x) fwCecFeature::x

static const fwCecMsgInfo s_astMsg[] =
{
    // opcode                                 to min max flags feature                        name
    { CEC_OP_FEATURE_ABORT,                   D,  2,  2, R, F(General),                  "feature_abort" },
    { CEC_OP_IMAGE_VIEW_ON,                   D,  0,  0, 0, F(OneTouchPlay),             "image_view_on" },
    { CEC_OP_TUNER_STEP_INCREMENT,            D,  0,  0, 0, F(TunerControl),             "tuner_step_increment" },
    { CEC_OP_TUNER_STEP_DECREMENT,            D,  0,  0, 0, F(TunerControl),             "tuner_step_decrement" },
    { CEC_OP_TUNER_DEVICE_STATUS,             D,  5,  8, R, F(TunerControl),             "tuner_device_status" },
    { CEC_OP_GIVE_TUNER_DEVICE_STATUS,        D,  1,  1, 0, F(TunerControl),             "give_tuner_device_status" },
    { CEC_OP_RECORD_ON,                       D,  1,  8, 0, F(OneTouchRecord),           "record_on" },
    { CEC_OP_RECORD_STATUS,                   D,  1,  1, R, F(OneTouchRecord),           "record_status" },
    { CEC_OP_RECORD_OFF,                      D,  0,  0, 0, F(OneTouchRecord),           "record_off" },
    { CEC_OP_TEXT_VIEW_ON,                    D,  0,  0, 0, F(OneTouchPlay),             "text_view_on" },
    { CEC_OP_RECORD_TV_SCREEN,                D,  0,  0, 0, F(OneTouchRecord),           "record_tv_screen" },
    { CEC_OP_GIVE_DECK_STATUS,                D,  1,  1, 0, F(DeckControl),              "give_deck_status" },
    { CEC_OP_DECK_STATUS,                     D,  1,  1, R, F(DeckControl),              "deck_status" },
    { CEC_OP_SET_MENU_LANGUAGE,               B,  3,  3, 0, F(SystemInformation),        "set_menu_language" },
    { CEC_OP_CLEAR_ANALOGUE_TIMER,            D, 11, 11, 0, F(TimerProgramming),         "clear_analogue_timer" },
    { CEC_OP_SET_ANALOGUE_TIMER,              D, 11, 11, 0, F(TimerProgramming),         "set_analogue_timer" },
    { CEC_OP_TIMER_STATUS,                    D,  1,  3, R, F(TimerProgramming),         "timer_status" },
    { CEC_OP_STANDBY,                         E,  0,  0, U, F(SystemStandby),            "standby" },
    { CEC_OP_PLAY,                            D,  1,  1, 0, F(DeckControl),              "play" },
    { CEC_OP_DECK_CONTROL,                    D,  1,  1, 0, F(DeckControl),              "deck_control" },
    { CEC_OP_TIMER_CLEARED_STATUS,            D,  1,  1, R, F(TimerProgramming),         "timer_cleared_status" },
    { CEC_OP_USER_CONTROL_PRESSED,            D,  1,  5, 0, F(RemoteControlPassThrough), "user_control_pressed" },
    { CEC_OP_USER_CONTROL_RELEASED,           D,  0,  0, 0, F(RemoteControlPassThrough), "user_control_released" },
    { CEC_OP_GIVE_OSD_NAME,                   D,  0,  0, 0, F(DeviceOsdName),            "give_osd_name" },
    { CEC_OP_SET_OSD_NAME,                    D,  1, 14, R, F(DeviceOsdName),            "set_osd_name" },
    { CEC_OP_SET_OSD_STRING,                  D,  1, 14, 0, F(OsdDisplay),               "set_osd_string" },
    { CEC_OP_SET_TIMER_PROGRAM_TITLE,         D,  1, 14, 0, F(TimerProgramming),         "set_timer_program_title" },
    { CEC_OP_SYSTEM_AUDIO_MODE_REQUEST,       D,  0,  2, 0, F(SystemAudioControl),       "system_audio_mode_request" },
    { CEC_OP_GIVE_AUDIO_STATUS,               D,  0,  0, 0, F(SystemAudioControl),       "give_audio_status" },
    { CEC_OP_SET_SYSTEM_AUDIO_MODE,           E,  1,  1, R, F(SystemAudioControl),       "set_system_audio_mode" },
    { CEC_OP_SET_AUDIO_VOLUME_LEVEL,          D,  1,  1, 0, F(SystemAudioControl),       "set_audio_volume_level" },
    { CEC_OP_REPORT_AUDIO_STATUS,             D,  1,  1, R, F(SystemAudioControl),       "report_audio_status" },
    { CEC_OP_GIVE_SYSTEM_AUDIO_MODE_STATUS,   D,  0,  0, 0, F(SystemAudioControl),       "give_system_audio_mode_status" },
    { CEC_OP_SYSTEM_AUDIO_MODE_STATUS,        D,  1,  1, R, F(SystemAudioControl),       "system_audio_mode_status" },
    { CEC_OP_ROUTING_CHANGE,                  B,  4,  4, U, F(RoutingControl),           "routing_change" },
    { CEC_OP_ROUTING_INFORMATION,             B,  2,  2, U, F(RoutingControl),           "routing_information" },
    { CEC_OP_ACTIVE_SOURCE,                   B,  2,  2, U, F(OneTouchPlay),             "active_source" },
    { CEC_OP_GIVE_PHYSICAL_ADDRESS,           D,  0,  0, U, F(SystemInformation),        "give_physical_address" },
    { CEC_OP_REPORT_PHYSICAL_ADDRESS,         B,  3,  3, U, F(SystemInformation),        "report_physical_address" },
    { CEC_OP_REQUEST_ACTIVE_SOURCE,           B,  0,  0, U, F(RoutingControl),           "request_active_source" },
    { CEC_OP_SET_STREAM_PATH,                 B,  2,  2, U, F(RoutingControl),           "set_stream_path" },
    { CEC_OP_DEVICE_VENDOR_ID,                B,  3,  3, 0, F(VendorSpecific),           "device_vendor_id" },
    { CEC_OP_VENDOR_COMMAND,                  D,  0, 14, 0, F(VendorSpecific),           "vendor_command" },
    { CEC_OP_VENDOR_REMOTE_BUTTON_DOWN,       E,  0, 14, 0, F(VendorSpecific),           "vendor_remote_button_down" },
    { CEC_OP_VENDOR_REMOTE_BUTTON_UP,         E,  0,  0, 0, F(VendorSpecific),           "vendor_remote_button_up" },
    { CEC_OP_GIVE_DEVICE_VENDOR_ID,           D,  0,  0, U, F(VendorSpecific),           "give_device_vendor_id" },
    { CEC_OP_MENU_REQUEST,                    D,  1,  1, 0, F(DeviceMenuControl),        "menu_request" },
    { CEC_OP_MENU_STATUS,                     D,  1,  1, R, F(DeviceMenuControl),        "menu_status" },
    { CEC_OP_GIVE_DEVICE_POWER_STATUS,        D,  0,  0, 0, F(PowerStatus),              "give_device_power_status" },
    { CEC_OP_REPORT_POWER_STATUS,             E,  1,  1, R, F(PowerStatus),              "report_power_status" },
    { CEC_OP_GET_MENU_LANGUAGE,               D,  0,  0, U, F(SystemInformation),        "get_menu_language" },
    { CEC_OP_SELECT_ANALOGUE_SERVICE,         D,  4,  4, 0, F(TunerControl),             "select_analogue_service" },
    { CEC_OP_SELECT_DIGITAL_SERVICE,          D,  7,  7, 0, F(TunerControl),             "select_digital_service" },
    { CEC_OP_SET_DIGITAL_TIMER,               D, 14, 14, 0, F(TimerProgramming),         "set_digital_timer" },
    { CEC_OP_CLEAR_DIGITAL_TIMER,             D, 14, 14, 0, F(TimerProgramming),         "clear_digital_timer" },
    { CEC_OP_SET_AUDIO_RATE,                  D,  1,  1, 0, F(AudioRateControl),         "set_audio_rate" },
    { CEC_OP_INACTIVE_SOURCE,                 D,  2,  2, R, F(RoutingControl),           "inactive_source" },
    { CEC_OP_CEC_VERSION,                     D,  1,  1, R, F(SystemInformation),        "cec_version" },
    { CEC_OP_GET_CEC_VERSION,                 D,  0,  0, 0, F(SystemInformation),        "get_cec_version" },
    { CEC_OP_VENDOR_COMMAND_WITH_ID,          E,  3, 14, 0, F(VendorSpecific),           "vendor_command_with_id" },
    { CEC_OP_CLEAR_EXTERNAL_TIMER,            D,  9, 10, 0, F(TimerProgramming),         "clear_external_timer" },
    { CEC_OP_SET_EXTERNAL_TIMER,              D,  9, 10, 0, F(TimerProgramming),         "set_external_timer" },
    { CEC_OP_REPORT_SHORT_AUDIO_DESCRIPTOR,   D,  3, 12, R, F(SystemAudioControl),       "report_short_audio_descriptor" },
    { CEC_OP_REQUEST_SHORT_AUDIO_DESCRIPTOR,  D,  1,  4, 0, F(SystemAudioControl),       "request_short_audio_descriptor" },
    { CEC_OP_GIVE_FEATURES,                   D,  0,  0, U, F(SystemInformation),        "give_features" },
    { CEC_OP_REPORT_FEATURES,                 B,  4, 14, 0, F(SystemInformation),        "report_features" },
    { CEC_OP_REQUEST_CURRENT_LATENCY,         B,  2,  2, U, F(DynamicAutoLipsync),       "request_current_latency" },
    { CEC_OP_REPORT_CURRENT_LATENCY,          B,  4,  5, 0, F(DynamicAutoLipsync),       "report_current_latency" },
    { CEC_OP_INITIATE_ARC,                    D,  0,  0, 0, F(AudioReturnChannel),       "initiate_arc" },
    { CEC_OP_REPORT_ARC_INITIATED,            D,  0,  0, R, F(AudioReturnChannel),       "report_arc_initiated" },
    { CEC_OP_REPORT_ARC_TERMINATED,           D,  0,  0, R, F(AudioReturnChannel),       "report_arc_terminated" },
    { CEC_OP_REQUEST_ARC_INITIATION,          D,  0,  0, 0, F(AudioReturnChannel),       "request_arc_initiation" },
    { CEC_OP_REQUEST_ARC_TERMINATION,         D,  0,  0, 0, F(AudioReturnChannel),       "request_arc_termination" },
    { CEC_OP_TERMINATE_ARC,                   D,  0,  0, 0, F(AudioReturnChannel),       "terminate_arc" },
    { CEC_OP_CDC_MESSAGE,                     B,  3, 14, U, F(CapabilityDiscovery),      "cdc_message" },
    { CEC_OP_ABORT,                           D,  0,  0, 0, F(General),                  "abort" },
};

#undef D
#undef B
#undef E
#undef U
#undef R
#undef F

static const int s_iMsgCount = (int)(sizeof(s_astMsg) / sizeof(s_astMsg[0]));

int cecMsgCount()
{
    return s_iMsgCount;
}

const fwCecMsgInfo & cecMsgAt(int iIndex)
{
    return s_astMsg[iIndex];
}

const fwCecMsgInfo * cecMsgFind(uint8_t btOpcode)
{
    int iLow = 0;
    int iHigh = s_iMsgCount - 1;
    while (iLow <= iHigh)
    {
        const int iMid = (iLow + iHigh) / 2;
        if (s_astMsg[iMid].btOpcode == btOpcode)
            return &s_astMsg[iMid];
        if (s_astMsg[iMid].btOpcode < btOpcode)
            iLow = iMid + 1;
        else
            iHigh = iMid - 1;
    }
    return nullptr;
}

// ---- build -------------------------------------------------------------------
bool cecBuild(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint8_t btOpcode,
              const uint8_t * pOperands, int iOperandCount)
{
    if (btInitiator > 15 || btDestination > 15)
        return false;
    if (iOperandCount < 0 || iOperandCount > CEC_OPERANDS_MAX)
        return false;
    if (iOperandCount > 0 && pOperands == nullptr)
        return false;

    stFrame.abtBlock[0] = (uint8_t)((btInitiator << 4) | btDestination);
    stFrame.abtBlock[1] = btOpcode;
    for (int i = 0; i < iOperandCount; i++)
        stFrame.abtBlock[2 + i] = pOperands[i];
    stFrame.btLen = (uint8_t)(2 + iOperandCount);
    return true;
}

bool cecBuildPoll(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination)
{
    if (btInitiator > 15 || btDestination > 15)
        return false;

    stFrame.abtBlock[0] = (uint8_t)((btInitiator << 4) | btDestination);
    stFrame.btLen = 1;
    return true;
}

bool cecBuildFeatureAbort(fwCecFrame & stReply, const fwCecFrame & stRx, uint8_t btReason)
{
    if (stRx.btLen < 2 || stRx.btLen > CEC_FRAME_MAX)
        return false;
    if (cecIsBroadcast(stRx) || cecInitiator(stRx) == CEC_ADDR_UNREGISTERED)
        return false;
    if (cecOpcode(stRx) == CEC_OP_FEATURE_ABORT)
        return false;

    const uint8_t abtOperands[2] = { cecOpcode(stRx), btReason };
    return cecBuild(stReply, cecDestination(stRx), cecInitiator(stRx), CEC_OP_FEATURE_ABORT, abtOperands, 2);
}

// ---- check -------------------------------------------------------------------
fwCecCheck cecCheck(const fwCecFrame & stFrame)
{
    if (stFrame.btLen == 0 || stFrame.btLen > CEC_FRAME_MAX)
        return fwCecCheck::BadLength;
    if (cecIsPoll(stFrame))
        return fwCecCheck::Poll;

    const fwCecMsgInfo * pInfo = cecMsgFind(cecOpcode(stFrame));
    if (pInfo == nullptr)
        return fwCecCheck::UnknownOpcode;
    if ((pInfo->btTo & (cecIsBroadcast(stFrame) ? CEC_TO_BROADCAST : CEC_TO_DIRECT)) == 0)
        return fwCecCheck::WrongDelivery;
    if (cecOperandCount(stFrame) < pInfo->btOperandsMin)
        return fwCecCheck::TooShort;
    return fwCecCheck::Ok;
}

// ---- follower ----------------------------------------------------------------
fwCecRx cecClassifyRx(const fwCecFrame & stFrame, uint16_t uiOwnMask)
{
    const fwCecCheck eCheck = cecCheck(stFrame);
    if (eCheck == fwCecCheck::BadLength)
        return fwCecRx::Ignore;

    const bool bBroadcast = cecIsBroadcast(stFrame);
    if (!bBroadcast && ((uiOwnMask >> cecDestination(stFrame)) & 1u) == 0)
        return fwCecRx::NotForUs;

    const bool bUnregistered = (cecInitiator(stFrame) == CEC_ADDR_UNREGISTERED);
    switch (eCheck)
    {
        case fwCecCheck::Poll:
            return bBroadcast ? fwCecRx::Ignore : fwCecRx::Poll;
        case fwCecCheck::UnknownOpcode:
            return (bBroadcast || bUnregistered) ? fwCecRx::Ignore : fwCecRx::Unsupported;
        case fwCecCheck::Ok:
            if (bUnregistered && (cecMsgFind(cecOpcode(stFrame))->btFlags & CEC_MSG_FROM_UNREGISTERED) == 0)
                return fwCecRx::Ignore;
            return fwCecRx::Handle;
        default:    // TooShort, WrongDelivery
            return fwCecRx::Ignore;
    }
}

// ---- device: helpers ---------------------------------------------------------
// The logical addresses a device type tries, in order (CEC 10.2.1). 0 for a
// type that claims none.
static int typeAddresses(uint8_t btDeviceType, const uint8_t ** ppList)
{
    static const uint8_t s_abtRecorder[] = { CEC_ADDR_RECORDER_1, CEC_ADDR_RECORDER_2, CEC_ADDR_RECORDER_3 };
    static const uint8_t s_abtTuner[]    = { CEC_ADDR_TUNER_1, CEC_ADDR_TUNER_2, CEC_ADDR_TUNER_3, CEC_ADDR_TUNER_4 };
    static const uint8_t s_abtPlayback[] = { CEC_ADDR_PLAYBACK_1, CEC_ADDR_PLAYBACK_2, CEC_ADDR_PLAYBACK_3 };
    static const uint8_t s_abtAudio[]    = { CEC_ADDR_AUDIO_SYSTEM };

    switch (btDeviceType)
    {
        case CEC_DEVTYPE_RECORDER:     *ppList = s_abtRecorder; return 3;
        case CEC_DEVTYPE_TUNER:        *ppList = s_abtTuner;    return 4;
        case CEC_DEVTYPE_PLAYBACK:     *ppList = s_abtPlayback; return 3;
        case CEC_DEVTYPE_AUDIO_SYSTEM: *ppList = s_abtAudio;    return 1;
    }
    *ppList = nullptr;
    return 0;
}

// A deck answers <Give Deck Status>, <Play> and <Deck Control>.
static bool isDeckType(uint8_t btDeviceType)
{
    return btDeviceType == CEC_DEVTYPE_PLAYBACK || btDeviceType == CEC_DEVTYPE_RECORDER;
}

static bool isPrintable(char ch)
{
    return ch >= 0x20 && ch <= 0x7E;
}

static bool behaves(const fwCecDevice & stDevice, uint16_t uiBehaviour)
{
    return (stDevice.stConfig.uiBehaviours & uiBehaviour) != 0;
}

static void outEvent(fwCecOut & stOut, fwCecEventType eType, uint8_t btPeer, uint8_t btOwn,
                     uint8_t bt0 = 0, uint8_t bt1 = 0, uint8_t bt2 = 0)
{
    if (stOut.btEvents >= CEC_OUT_EVENTS_MAX)
    {
        stOut.bOverflow = true;
        return;
    }
    fwCecEvent & stEvent = stOut.astEvent[stOut.btEvents++];
    stEvent.eType      = eType;
    stEvent.btPeer     = btPeer;
    stEvent.btOwn      = btOwn;
    stEvent.abtData[0] = bt0;
    stEvent.abtData[1] = bt1;
    stEvent.abtData[2] = bt2;
}

// The next free frame of stOut, or nullptr with the overflow flagged.
static fwCecFrame * outSlot(fwCecOut & stOut)
{
    if (stOut.btFrames >= CEC_OUT_FRAMES_MAX)
    {
        stOut.bOverflow = true;
        return nullptr;
    }
    return &stOut.astFrame[stOut.btFrames];
}

static void outSend(fwCecOut & stOut, uint8_t btFrom, uint8_t btTo, uint8_t btOpcode,
                    const uint8_t * pOperands = nullptr, int iOperandCount = 0)
{
    fwCecFrame * pFrame = outSlot(stOut);
    if (pFrame != nullptr && cecBuild(*pFrame, btFrom, btTo, btOpcode, pOperands, iOperandCount))
        stOut.btFrames++;
}

static void outPoll(fwCecOut & stOut, uint8_t btAddress)
{
    fwCecFrame * pFrame = outSlot(stOut);
    if (pFrame != nullptr && cecBuildPoll(*pFrame, btAddress, btAddress))
        stOut.btFrames++;
}

static void outFeatureAbort(fwCecOut & stOut, const fwCecFrame & stRx, uint8_t btReason)
{
    fwCecFrame * pFrame = outSlot(stOut);
    if (pFrame != nullptr && cecBuildFeatureAbort(*pFrame, stRx, btReason))
        stOut.btFrames++;
}

// The role holding a logical address, or -1.
static int roleOfAddress(const fwCecDevice & stDevice, uint8_t btAddress)
{
    if (btAddress >= CEC_ADDR_UNREGISTERED)
        return -1;
    for (int i = 0; i < CEC_ROLES_MAX; i++)
        if (stDevice.abtAddress[i] == btAddress)
            return i;
    return -1;
}

// The address of the first role that is a deck, or 15.
static uint8_t deckAddress(const fwCecDevice & stDevice)
{
    for (int i = 0; i < CEC_ROLES_MAX; i++)
        if (isDeckType(stDevice.stConfig.abtDeviceType[i]) && stDevice.abtAddress[i] < CEC_ADDR_UNREGISTERED)
            return stDevice.abtAddress[i];
    return CEC_ADDR_UNREGISTERED;
}

// Ready, with an address to send from.
static bool canSend(const fwCecDevice & stDevice)
{
    return stDevice.eState == fwCecState::Ready && cecDeviceSourceAddress(stDevice) != CEC_ADDR_UNREGISTERED;
}

static void sendReportPhysAddr(const fwCecDevice & stDevice, uint8_t btFrom, uint8_t btDeviceType, fwCecOut & stOut)
{
    uint8_t abtReport[3];
    cecPutPhysAddr(abtReport, stDevice.uiPhysAddr);
    abtReport[2] = btDeviceType;
    outSend(stOut, btFrom, CEC_ADDR_BROADCAST, CEC_OP_REPORT_PHYSICAL_ADDRESS, abtReport, 3);
}

// <Device Vendor ID> from btFrom. False when the device has none to give.
static bool sendVendorId(const fwCecDevice & stDevice, uint8_t btFrom, fwCecOut & stOut)
{
    const uint32_t uiId = stDevice.stConfig.uiVendorId;
    if (!behaves(stDevice, CEC_BEH_VENDOR_ID) || uiId == CEC_VENDOR_ID_NONE)
        return false;

    const uint8_t abtId[3] = { (uint8_t)(uiId >> 16), (uint8_t)(uiId >> 8), (uint8_t)uiId };
    outSend(stOut, btFrom, CEC_ADDR_BROADCAST, CEC_OP_DEVICE_VENDOR_ID, abtId, 3);
    return true;
}

// <Active Source> from a device holding an address.
static void sendActiveSource(fwCecDevice & stDevice, fwCecOut & stOut)
{
    const uint8_t btFrom = cecDeviceSourceAddress(stDevice);
    if (btFrom == CEC_ADDR_UNREGISTERED)
        return;

    uint8_t abtPhys[2];
    cecPutPhysAddr(abtPhys, stDevice.uiPhysAddr);
    outSend(stOut, btFrom, CEC_ADDR_BROADCAST, CEC_OP_ACTIVE_SOURCE, abtPhys, 2);
    if (!stDevice.bActiveSource)
    {
        stDevice.bActiveSource = true;
        outEvent(stOut, fwCecEventType::ActiveSource, CEC_ADDR_UNREGISTERED, btFrom, 1);
    }
}

static void loseActiveSource(fwCecDevice & stDevice, fwCecOut & stOut)
{
    if (!stDevice.bActiveSource)
        return;
    stDevice.bActiveSource = false;
    outEvent(stOut, fwCecEventType::ActiveSource, CEC_ADDR_UNREGISTERED, cecDeviceSourceAddress(stDevice), 0);
}

static void keyUp(fwCecDevice & stDevice, fwCecOut & stOut)
{
    if (!stDevice.bKeyDown)
        return;
    stDevice.bKeyDown = false;
    outEvent(stOut, fwCecEventType::KeyUp, stDevice.btKeyFrom, stDevice.btKeyTo, stDevice.btKeyCode);
}

// ---- device: claim -----------------------------------------------------------
// The address being polled, or 15 when the current role has none left.
static uint8_t claimCandidate(const fwCecDevice & stDevice)
{
    if (stDevice.btClaimRole >= CEC_ROLES_MAX)
        return CEC_ADDR_UNREGISTERED;

    const uint8_t * pList  = nullptr;
    const int       iCount = typeAddresses(stDevice.stConfig.abtDeviceType[stDevice.btClaimRole], &pList);
    return stDevice.btClaimIndex < iCount ? pList[stDevice.btClaimIndex] : CEC_ADDR_UNREGISTERED;
}

static void claimNextRole(fwCecDevice & stDevice)
{
    stDevice.btClaimRole++;
    stDevice.btClaimIndex = 0;
    stDevice.btClaimSends = 0;
}

// Sends the poll for the next address still to be tried, or ends the claim.
static void claimNext(fwCecDevice & stDevice, fwCecOut & stOut)
{
    for (; stDevice.btClaimRole < CEC_ROLES_MAX; claimNextRole(stDevice))
    {
        const uint8_t btCandidate = claimCandidate(stDevice);
        if (btCandidate != CEC_ADDR_UNREGISTERED)
        {
            outPoll(stOut, btCandidate);
            return;
        }

        const uint8_t btType = stDevice.stConfig.abtDeviceType[stDevice.btClaimRole];
        const uint8_t btRole = (uint8_t)(1u << stDevice.btClaimRole);
        if (btType != CEC_DEVTYPE_NONE && (stDevice.btNoneReported & btRole) == 0)
        {
            stDevice.btNoneReported = (uint8_t)(stDevice.btNoneReported | btRole);
            outEvent(stOut, fwCecEventType::AddressNone, CEC_ADDR_UNREGISTERED, CEC_ADDR_UNREGISTERED, btType);
        }
    }

    stDevice.eState = fwCecState::Ready;
    const uint8_t btFrom = cecDeviceSourceAddress(stDevice);
    if (btFrom != CEC_ADDR_UNREGISTERED)
        sendVendorId(stDevice, btFrom, stOut);
    else if (stDevice.bClaimFailed)
    {
        stDevice.bClaimRetry = true;
        stDevice.bClaimTimed = false;
    }
}

// Starts the claim again from the first role.
static void claimRestart(fwCecDevice & stDevice, fwCecOut & stOut)
{
    stDevice.bClaimRetry  = false;
    stDevice.bClaimFailed = false;
    stDevice.btClaimRole  = 0;
    stDevice.btClaimIndex = 0;
    stDevice.btClaimSends = 0;
    stDevice.eState       = fwCecState::Claiming;
    claimNext(stDevice, stOut);
}

// ---- device --------------------------------------------------------------------
void cecConfigDefault(fwCecConfig & stConfig)
{
    stConfig.abtDeviceType[0] = CEC_DEVTYPE_PLAYBACK;
    for (int i = 1; i < CEC_ROLES_MAX; i++)
        stConfig.abtDeviceType[i] = CEC_DEVTYPE_NONE;
    stConfig.btCecVersion = CEC_VERSION_1_4;
    stConfig.uiBehaviours = CEC_BEH_ALL;
    stConfig.uiVendorId   = CEC_VENDOR_ID_NONE;
    for (int i = 0; i < CEC_OSD_NAME_MAX; i++)
        stConfig.achOsdName[i] = 0;
    cecConfigSetOsdName(stConfig, CEC_OSD_NAME_DEFAULT);
}

void cecConfigSetOsdName(fwCecConfig & stConfig, const char * szName)
{
    stConfig.btOsdNameLen = 0;
    if (szName == nullptr)
        return;
    for (const char * p = szName; *p != 0 && stConfig.btOsdNameLen < CEC_OSD_NAME_MAX; p++)
    {
        if (isPrintable(*p))
            stConfig.achOsdName[stConfig.btOsdNameLen++] = *p;
    }
}

static void resetRuntime(fwCecDevice & stDevice)
{
    stDevice.uiPhysAddr = CEC_PHYS_ADDR_INVALID;
    for (int i = 0; i < CEC_ROLES_MAX; i++)
        stDevice.abtAddress[i] = CEC_ADDR_UNREGISTERED;
    stDevice.btClaimRole    = 0;
    stDevice.btClaimIndex   = 0;
    stDevice.btClaimSends   = 0;
    stDevice.btNoneReported = 0;
    stDevice.bClaimFailed   = false;
    stDevice.bClaimRetry    = false;
    stDevice.bClaimTimed    = false;
    stDevice.uiClaimRetryMs = 0;
    stDevice.uiClaimWaitMs  = CEC_CLAIM_RETRY_MS;
    stDevice.bActiveSource  = false;
    stDevice.btDeckStatusTo = CEC_ADDR_UNREGISTERED;
    stDevice.bKeyDown       = false;
    stDevice.btKeyCode      = 0;
    stDevice.btKeyFrom      = 0;
    stDevice.btKeyTo        = 0;
    stDevice.uiKeyMs        = 0;
}

void cecDeviceInit(fwCecDevice & stDevice, const fwCecConfig & stConfig)
{
    stDevice.stConfig = stConfig;
    // One logical address per device type.
    for (int i = 1; i < CEC_ROLES_MAX; i++)
        for (int j = 0; j < i; j++)
            if (stDevice.stConfig.abtDeviceType[i] == stDevice.stConfig.abtDeviceType[j])
                stDevice.stConfig.abtDeviceType[i] = CEC_DEVTYPE_NONE;

    stDevice.eState        = fwCecState::Off;
    stDevice.btPowerStatus = CEC_POWER_ON;
    stDevice.btMenuState   = CEC_MENU_STATE_DEACTIVATED;
    stDevice.btDeckInfo    = CEC_DECK_INFO_STOP;
    resetRuntime(stDevice);
}

void cecDeviceStart(fwCecDevice & stDevice, uint16_t uiPhysAddr, fwCecOut & stOut)
{
    resetRuntime(stDevice);
    stDevice.uiPhysAddr = uiPhysAddr;

    if (uiPhysAddr == CEC_PHYS_ADDR_INVALID)
    {
        stDevice.eState = fwCecState::Ready;
        outEvent(stOut, fwCecEventType::AddressNone, CEC_ADDR_UNREGISTERED, CEC_ADDR_UNREGISTERED, CEC_DEVTYPE_NONE);
        return;
    }

    claimRestart(stDevice, stOut);
}

void cecDeviceStop(fwCecDevice & stDevice, fwCecOut & stOut)
{
    if (stDevice.eState == fwCecState::Off)
        return;

    cecDeviceInactiveSource(stDevice, stOut);
    keyUp(stDevice, stOut);
    resetRuntime(stDevice);
    stDevice.eState = fwCecState::Off;
}

void cecDeviceOnTxDone(fwCecDevice & stDevice, const fwCecFrame & stSent, fwCecTxResult eResult, fwCecOut & stOut)
{
    if (stDevice.eState != fwCecState::Claiming || !cecIsPoll(stSent))
        return;

    const uint8_t btCandidate = claimCandidate(stDevice);
    if (btCandidate == CEC_ADDR_UNREGISTERED ||
        cecInitiator(stSent) != btCandidate || cecDestination(stSent) != btCandidate)
        return;

    switch (eResult)
    {
        case fwCecTxResult::NotAcked:
        {
            // Nobody answers at this address: it is free.
            const uint8_t btType = stDevice.stConfig.abtDeviceType[stDevice.btClaimRole];
            stDevice.abtAddress[stDevice.btClaimRole] = btCandidate;
            outEvent(stOut, fwCecEventType::AddressClaimed, CEC_ADDR_UNREGISTERED, btCandidate, btType);
            sendReportPhysAddr(stDevice, btCandidate, btType, stOut);
            claimNextRole(stDevice);
            break;
        }
        case fwCecTxResult::Acked:
            // In use by another device.
            stDevice.btClaimIndex++;
            stDevice.btClaimSends = 0;
            break;
        case fwCecTxResult::Failed:
            stDevice.btClaimSends++;
            if (stDevice.btClaimSends >= CEC_CLAIM_SEND_TRIES)
            {
                stDevice.btClaimIndex++;
                stDevice.btClaimSends = 0;
                stDevice.bClaimFailed = true;
            }
            break;
    }
    claimNext(stDevice, stOut);
}

// One valid message for this device. True when a behaviour took it; false
// leaves a directly addressed request to be refused.
static bool rxHandle(fwCecDevice & stDevice, const fwCecFrame & stRx, uint32_t uiNowMs, fwCecOut & stOut)
{
    const uint8_t   btFrom = cecInitiator(stRx);
    const uint8_t   btTo   = cecDestination(stRx);
    const uint8_t * pOp    = cecOperands(stRx);
    // The role the message was sent to; none for a broadcast.
    const int       iRole  = roleOfAddress(stDevice, btTo);
    const uint8_t   btType = iRole >= 0 ? stDevice.stConfig.abtDeviceType[iRole] : (uint8_t)CEC_DEVTYPE_NONE;

    switch (cecOpcode(stRx))
    {
        case CEC_OP_GIVE_PHYSICAL_ADDRESS:
            if (!behaves(stDevice, CEC_BEH_SYSTEM_INFO) || iRole < 0)
                return false;
            sendReportPhysAddr(stDevice, btTo, btType, stOut);
            return true;

        case CEC_OP_GET_CEC_VERSION:
            if (!behaves(stDevice, CEC_BEH_SYSTEM_INFO))
                return false;
            outSend(stOut, btTo, btFrom, CEC_OP_CEC_VERSION, &stDevice.stConfig.btCecVersion, 1);
            return true;

        case CEC_OP_SET_MENU_LANGUAGE:
            if (!behaves(stDevice, CEC_BEH_SYSTEM_INFO))
                return false;
            outEvent(stOut, fwCecEventType::MenuLanguage, btFrom, btTo, pOp[0], pOp[1], pOp[2]);
            return true;

        case CEC_OP_GIVE_OSD_NAME:
            if (!behaves(stDevice, CEC_BEH_OSD_NAME) || stDevice.stConfig.btOsdNameLen == 0)
                return false;
            outSend(stOut, btTo, btFrom, CEC_OP_SET_OSD_NAME,
                    (const uint8_t *)stDevice.stConfig.achOsdName, stDevice.stConfig.btOsdNameLen);
            return true;

        case CEC_OP_GIVE_DEVICE_POWER_STATUS:
            if (!behaves(stDevice, CEC_BEH_POWER_STATUS))
                return false;
            outSend(stOut, btTo, btFrom, CEC_OP_REPORT_POWER_STATUS, &stDevice.btPowerStatus, 1);
            return true;

        case CEC_OP_GIVE_DEVICE_VENDOR_ID:
            return sendVendorId(stDevice, btTo, stOut);

        case CEC_OP_STANDBY:
            if (!behaves(stDevice, CEC_BEH_STANDBY))
                return false;
            loseActiveSource(stDevice, stOut);
            outEvent(stOut, fwCecEventType::Standby, btFrom, btTo);
            return true;

        case CEC_OP_SET_STREAM_PATH:
            if (!behaves(stDevice, CEC_BEH_ROUTING))
                return false;
            if (cecGetPhysAddr(pOp) == stDevice.uiPhysAddr)
                sendActiveSource(stDevice, stOut);
            else
                loseActiveSource(stDevice, stOut);
            return true;

        case CEC_OP_REQUEST_ACTIVE_SOURCE:
            if (!behaves(stDevice, CEC_BEH_ROUTING))
                return false;
            if (stDevice.bActiveSource)
                sendActiveSource(stDevice, stOut);
            return true;

        case CEC_OP_ACTIVE_SOURCE:
            if (!behaves(stDevice, CEC_BEH_ROUTING))
                return false;
            if (cecGetPhysAddr(pOp) != stDevice.uiPhysAddr)
                loseActiveSource(stDevice, stOut);
            return true;

        case CEC_OP_ROUTING_CHANGE:
            if (!behaves(stDevice, CEC_BEH_ROUTING))
                return false;
            if (cecGetPhysAddr(pOp + 2) != stDevice.uiPhysAddr)
                loseActiveSource(stDevice, stOut);
            return true;

        case CEC_OP_USER_CONTROL_PRESSED:
            if (!behaves(stDevice, CEC_BEH_REMOTE_KEYS))
                return false;
            if (stDevice.bKeyDown && (stDevice.btKeyCode != pOp[0] || stDevice.btKeyFrom != btFrom))
                keyUp(stDevice, stOut);
            if (!stDevice.bKeyDown)
            {
                stDevice.bKeyDown  = true;
                stDevice.btKeyCode = pOp[0];
                stDevice.btKeyFrom = btFrom;
                stDevice.btKeyTo   = btTo;
                outEvent(stOut, fwCecEventType::KeyDown, btFrom, btTo, pOp[0]);
            }
            stDevice.uiKeyMs = uiNowMs;
            return true;

        case CEC_OP_USER_CONTROL_RELEASED:
            if (!behaves(stDevice, CEC_BEH_REMOTE_KEYS))
                return false;
            if (stDevice.btKeyFrom == btFrom)
                keyUp(stDevice, stOut);
            return true;

        case CEC_OP_MENU_REQUEST:
            if (!behaves(stDevice, CEC_BEH_MENU))
                return false;
            if (pOp[0] == CEC_MENU_REQUEST_ACTIVATE)
                stDevice.btMenuState = CEC_MENU_STATE_ACTIVATED;
            else if (pOp[0] == CEC_MENU_REQUEST_DEACTIVATE)
                stDevice.btMenuState = CEC_MENU_STATE_DEACTIVATED;
            outEvent(stOut, fwCecEventType::MenuRequest, btFrom, btTo, pOp[0]);
            outSend(stOut, btTo, btFrom, CEC_OP_MENU_STATUS, &stDevice.btMenuState, 1);
            return true;

        case CEC_OP_GIVE_DECK_STATUS:
            if (!behaves(stDevice, CEC_BEH_DECK) || !isDeckType(btType))
                return false;
            switch (pOp[0])
            {
                case CEC_STATUS_REQUEST_ON:
                    stDevice.btDeckStatusTo = btFrom;
                    outSend(stOut, btTo, btFrom, CEC_OP_DECK_STATUS, &stDevice.btDeckInfo, 1);
                    break;
                case CEC_STATUS_REQUEST_ONCE:
                    outSend(stOut, btTo, btFrom, CEC_OP_DECK_STATUS, &stDevice.btDeckInfo, 1);
                    break;
                case CEC_STATUS_REQUEST_OFF:
                    if (stDevice.btDeckStatusTo == btFrom)
                        stDevice.btDeckStatusTo = CEC_ADDR_UNREGISTERED;
                    break;
                default:
                    outFeatureAbort(stOut, stRx, CEC_ABORT_INVALID_OPERAND);
                    break;
            }
            return true;

        case CEC_OP_PLAY:
            if (!behaves(stDevice, CEC_BEH_DECK) || !isDeckType(btType))
                return false;
            outEvent(stOut, fwCecEventType::DeckPlay, btFrom, btTo, pOp[0]);
            return true;

        case CEC_OP_DECK_CONTROL:
            if (!behaves(stDevice, CEC_BEH_DECK) || !isDeckType(btType))
                return false;
            outEvent(stOut, fwCecEventType::DeckControl, btFrom, btTo, pOp[0]);
            return true;

        case CEC_OP_FEATURE_ABORT:
            outEvent(stOut, fwCecEventType::FeatureAbort, btFrom, btTo, pOp[0], pOp[1]);
            return true;

        case CEC_OP_ABORT:
            if (!behaves(stDevice, CEC_BEH_ABORT))
                return false;
            outFeatureAbort(stOut, stRx, CEC_ABORT_REFUSED);
            return true;

        default:
            return false;
    }
}

void cecDeviceOnRx(fwCecDevice & stDevice, const fwCecFrame & stRx, uint32_t uiNowMs, fwCecOut & stOut)
{
    if (stDevice.eState == fwCecState::Off)
        return;

    const bool bRefuse = behaves(stDevice, CEC_BEH_ABORT);
    switch (cecClassifyRx(stRx, cecDeviceAddressMask(stDevice)))
    {
        case fwCecRx::Unsupported:
            if (bRefuse)
                outFeatureAbort(stOut, stRx, CEC_ABORT_UNRECOGNIZED);
            break;

        case fwCecRx::Handle:
            // A directly addressed request no behaviour took is refused. An
            // answer or an announcement is dropped.
            if (!rxHandle(stDevice, stRx, uiNowMs, stOut) && bRefuse && !cecIsBroadcast(stRx) &&
                (cecMsgFind(cecOpcode(stRx))->btFlags & CEC_MSG_REPORT) == 0)
                outFeatureAbort(stOut, stRx, CEC_ABORT_UNRECOGNIZED);
            break;

        default:
            break;
    }
}

void cecDeviceOnTick(fwCecDevice & stDevice, uint32_t uiNowMs, fwCecOut & stOut)
{
    if (stDevice.bKeyDown && (uint32_t)(uiNowMs - stDevice.uiKeyMs) >= CEC_KEY_RELEASE_MS)
        keyUp(stDevice, stOut);

    if (stDevice.eState != fwCecState::Ready || !stDevice.bClaimRetry)
        return;
    if (!stDevice.bClaimTimed)
    {
        // The claim ended without a clock: the wait starts at the first tick after it.
        stDevice.bClaimTimed    = true;
        stDevice.uiClaimRetryMs = uiNowMs + stDevice.uiClaimWaitMs;
        stDevice.uiClaimWaitMs  = (uint16_t)(stDevice.uiClaimWaitMs * 2u < CEC_CLAIM_RETRY_MAX_MS
                                             ? stDevice.uiClaimWaitMs * 2u : CEC_CLAIM_RETRY_MAX_MS);
        return;
    }
    if ((int32_t)(uiNowMs - stDevice.uiClaimRetryMs) >= 0)
        claimRestart(stDevice, stOut);
}

uint16_t cecDeviceAddressMask(const fwCecDevice & stDevice)
{
    uint16_t uiMask = 0;
    for (int i = 0; i < CEC_ROLES_MAX; i++)
        if (stDevice.abtAddress[i] < CEC_ADDR_UNREGISTERED)
            uiMask = (uint16_t)(uiMask | (1u << stDevice.abtAddress[i]));
    return uiMask;
}

uint8_t cecDeviceSourceAddress(const fwCecDevice & stDevice)
{
    for (int i = 0; i < CEC_ROLES_MAX; i++)
        if (stDevice.abtAddress[i] < CEC_ADDR_UNREGISTERED)
            return stDevice.abtAddress[i];
    return CEC_ADDR_UNREGISTERED;
}

// ---- requests ------------------------------------------------------------------
bool cecDeviceOneTouchPlay(fwCecDevice & stDevice, bool bTextView, fwCecOut & stOut)
{
    if (stDevice.eState != fwCecState::Ready)
        return false;

    outSend(stOut, cecDeviceSourceAddress(stDevice), CEC_ADDR_TV,
            bTextView ? CEC_OP_TEXT_VIEW_ON : CEC_OP_IMAGE_VIEW_ON);
    sendActiveSource(stDevice, stOut);
    return true;
}

bool cecDeviceInactiveSource(fwCecDevice & stDevice, fwCecOut & stOut)
{
    if (!canSend(stDevice) || !stDevice.bActiveSource)
        return false;

    uint8_t abtPhys[2];
    cecPutPhysAddr(abtPhys, stDevice.uiPhysAddr);
    outSend(stOut, cecDeviceSourceAddress(stDevice), CEC_ADDR_TV, CEC_OP_INACTIVE_SOURCE, abtPhys, 2);
    loseActiveSource(stDevice, stOut);
    return true;
}

bool cecDeviceStandby(fwCecDevice & stDevice, uint8_t btDestination, fwCecOut & stOut)
{
    if (stDevice.eState != fwCecState::Ready || btDestination > 15)
        return false;
    if (roleOfAddress(stDevice, btDestination) >= 0)
        return false;

    outSend(stOut, cecDeviceSourceAddress(stDevice), btDestination, CEC_OP_STANDBY);
    return true;
}

bool cecDeviceOsdString(fwCecDevice & stDevice, uint8_t btDisplayControl, const char * pText, int iLen, fwCecOut & stOut)
{
    if (!canSend(stDevice))
        return false;

    const bool bClear = (btDisplayControl == CEC_DISPLAY_CLEAR);
    if (!bClear && btDisplayControl != CEC_DISPLAY_DEFAULT_TIME && btDisplayControl != CEC_DISPLAY_UNTIL_CLEARED)
        return false;
    if (iLen < (bClear ? 0 : 1) || iLen > CEC_OSD_STRING_MAX || (iLen > 0 && pText == nullptr))
        return false;

    uint8_t abtOperands[1 + CEC_OSD_STRING_MAX];
    abtOperands[0] = btDisplayControl;
    for (int i = 0; i < iLen; i++)
    {
        if (!isPrintable(pText[i]))
            return false;
        abtOperands[1 + i] = (uint8_t)pText[i];
    }
    outSend(stOut, cecDeviceSourceAddress(stDevice), CEC_ADDR_TV, CEC_OP_SET_OSD_STRING, abtOperands, 1 + iLen);
    return true;
}

// Another device, to send a key to.
static bool canSendKeyTo(const fwCecDevice & stDevice, uint8_t btDestination)
{
    return canSend(stDevice) && btDestination < CEC_ADDR_BROADCAST && roleOfAddress(stDevice, btDestination) < 0;
}

bool cecDeviceKeyPress(fwCecDevice & stDevice, uint8_t btDestination, uint8_t btUiCommand, fwCecOut & stOut)
{
    if (!canSendKeyTo(stDevice, btDestination))
        return false;
    outSend(stOut, cecDeviceSourceAddress(stDevice), btDestination, CEC_OP_USER_CONTROL_PRESSED, &btUiCommand, 1);
    return true;
}

bool cecDeviceKeyRelease(fwCecDevice & stDevice, uint8_t btDestination, fwCecOut & stOut)
{
    if (!canSendKeyTo(stDevice, btDestination))
        return false;
    outSend(stOut, cecDeviceSourceAddress(stDevice), btDestination, CEC_OP_USER_CONTROL_RELEASED);
    return true;
}

void cecDeviceSetPowerStatus(fwCecDevice & stDevice, uint8_t btPowerStatus)
{
    stDevice.btPowerStatus = btPowerStatus;
}

void cecDeviceSetDeckInfo(fwCecDevice & stDevice, uint8_t btDeckInfo, fwCecOut & stOut)
{
    if (stDevice.btDeckInfo == btDeckInfo)
        return;
    stDevice.btDeckInfo = btDeckInfo;

    const uint8_t btFrom = deckAddress(stDevice);
    if (stDevice.eState == fwCecState::Ready && btFrom != CEC_ADDR_UNREGISTERED &&
        stDevice.btDeckStatusTo != CEC_ADDR_UNREGISTERED)
        outSend(stOut, btFrom, stDevice.btDeckStatusTo, CEC_OP_DECK_STATUS, &stDevice.btDeckInfo, 1);
}

void cecDeviceSetMenuState(fwCecDevice & stDevice, uint8_t btMenuState, fwCecOut & stOut)
{
    if (stDevice.btMenuState == btMenuState)
        return;
    stDevice.btMenuState = btMenuState;

    if (canSend(stDevice))
        outSend(stOut, cecDeviceSourceAddress(stDevice), CEC_ADDR_TV, CEC_OP_MENU_STATUS, &stDevice.btMenuState, 1);
}

// ---- controller ----------------------------------------------------------------
// Request and the message that answers it (CEC 15, CEC 2.0 for 0xA5 and 0xA7).
struct stReplyRow
{
    uint8_t btRequest;
    uint8_t btReply;
};
static const stReplyRow s_astReply[] =
{
    { CEC_OP_GIVE_TUNER_DEVICE_STATUS,       CEC_OP_TUNER_DEVICE_STATUS },
    { CEC_OP_RECORD_ON,                      CEC_OP_RECORD_STATUS },
    { CEC_OP_RECORD_TV_SCREEN,               CEC_OP_RECORD_ON },
    { CEC_OP_GIVE_DECK_STATUS,               CEC_OP_DECK_STATUS },
    { CEC_OP_CLEAR_ANALOGUE_TIMER,           CEC_OP_TIMER_CLEARED_STATUS },
    { CEC_OP_SET_ANALOGUE_TIMER,             CEC_OP_TIMER_STATUS },
    { CEC_OP_GIVE_OSD_NAME,                  CEC_OP_SET_OSD_NAME },
    { CEC_OP_SYSTEM_AUDIO_MODE_REQUEST,      CEC_OP_SET_SYSTEM_AUDIO_MODE },
    { CEC_OP_GIVE_AUDIO_STATUS,              CEC_OP_REPORT_AUDIO_STATUS },
    { CEC_OP_GIVE_SYSTEM_AUDIO_MODE_STATUS,  CEC_OP_SYSTEM_AUDIO_MODE_STATUS },
    { CEC_OP_GIVE_PHYSICAL_ADDRESS,          CEC_OP_REPORT_PHYSICAL_ADDRESS },
    { CEC_OP_REQUEST_ACTIVE_SOURCE,          CEC_OP_ACTIVE_SOURCE },
    { CEC_OP_SET_STREAM_PATH,                CEC_OP_ACTIVE_SOURCE },
    { CEC_OP_GIVE_DEVICE_VENDOR_ID,          CEC_OP_DEVICE_VENDOR_ID },
    { CEC_OP_MENU_REQUEST,                   CEC_OP_MENU_STATUS },
    { CEC_OP_GIVE_DEVICE_POWER_STATUS,       CEC_OP_REPORT_POWER_STATUS },
    { CEC_OP_GET_MENU_LANGUAGE,              CEC_OP_SET_MENU_LANGUAGE },
    { CEC_OP_SET_DIGITAL_TIMER,              CEC_OP_TIMER_STATUS },
    { CEC_OP_CLEAR_DIGITAL_TIMER,            CEC_OP_TIMER_CLEARED_STATUS },
    { CEC_OP_GET_CEC_VERSION,                CEC_OP_CEC_VERSION },
    { CEC_OP_CLEAR_EXTERNAL_TIMER,           CEC_OP_TIMER_CLEARED_STATUS },
    { CEC_OP_SET_EXTERNAL_TIMER,             CEC_OP_TIMER_STATUS },
    { CEC_OP_REQUEST_SHORT_AUDIO_DESCRIPTOR, CEC_OP_REPORT_SHORT_AUDIO_DESCRIPTOR },
    { CEC_OP_GIVE_FEATURES,                  CEC_OP_REPORT_FEATURES },
    { CEC_OP_REQUEST_CURRENT_LATENCY,        CEC_OP_REPORT_CURRENT_LATENCY },
    { CEC_OP_INITIATE_ARC,                   CEC_OP_REPORT_ARC_INITIATED },
    { CEC_OP_REQUEST_ARC_INITIATION,         CEC_OP_INITIATE_ARC },
    { CEC_OP_REQUEST_ARC_TERMINATION,        CEC_OP_TERMINATE_ARC },
    { CEC_OP_TERMINATE_ARC,                  CEC_OP_REPORT_ARC_TERMINATED },
    { CEC_OP_ABORT,                          CEC_OP_FEATURE_ABORT },
};

int cecReplyOpcode(const fwCecFrame & stRequest)
{
    if (stRequest.btLen < 2 || stRequest.btLen > CEC_FRAME_MAX)
        return CEC_REPLY_NONE;
    const uint8_t btOpcode = cecOpcode(stRequest);
    if (btOpcode == CEC_OP_GIVE_DECK_STATUS &&
        (cecOperandCount(stRequest) < 1 || cecOperands(stRequest)[0] == CEC_STATUS_REQUEST_OFF))
        return CEC_REPLY_NONE;
    for (const stReplyRow & stRow : s_astReply)
        if (stRow.btRequest == btOpcode)
            return stRow.btReply;
    return CEC_REPLY_NONE;
}

fwCecMatch cecMatchReply(const fwCecFrame & stRx, uint8_t btPeer, uint8_t btRequest, uint8_t btReply)
{
    if (stRx.btLen < 2 || stRx.btLen > CEC_FRAME_MAX)
        return fwCecMatch::None;
    if (btPeer != CEC_ADDR_BROADCAST && cecInitiator(stRx) != btPeer)
        return fwCecMatch::None;

    const uint8_t btOpcode = cecOpcode(stRx);
    const bool bAbortOfRequest = btOpcode == CEC_OP_FEATURE_ABORT && cecOperandCount(stRx) >= 2 &&
                                 cecOperands(stRx)[0] == btRequest;
    if (btOpcode == btReply)
    {
        if (btReply == CEC_OP_FEATURE_ABORT)
            return bAbortOfRequest ? fwCecMatch::Reply : fwCecMatch::None;
        const fwCecMsgInfo * pInfo = cecMsgFind(btOpcode);
        if (pInfo != nullptr && cecOperandCount(stRx) < pInfo->btOperandsMin)
            return fwCecMatch::None;
        return fwCecMatch::Reply;
    }
    return bAbortOfRequest ? fwCecMatch::Refused : fwCecMatch::None;
}

// A message to one device.
static bool directTo(uint8_t btDestination)
{
    return btDestination < CEC_ADDR_BROADCAST;
}

bool cecBuildRequestActiveSource(fwCecFrame & stFrame, uint8_t btInitiator)
{
    return cecBuild(stFrame, btInitiator, CEC_ADDR_BROADCAST, CEC_OP_REQUEST_ACTIVE_SOURCE);
}

bool cecBuildSetStreamPath(fwCecFrame & stFrame, uint8_t btInitiator, uint16_t uiPhysAddr)
{
    uint8_t abtOperands[2];
    cecPutPhysAddr(abtOperands, uiPhysAddr);
    return cecBuild(stFrame, btInitiator, CEC_ADDR_BROADCAST, CEC_OP_SET_STREAM_PATH, abtOperands, 2);
}

bool cecBuildRoutingChange(fwCecFrame & stFrame, uint8_t btInitiator, uint16_t uiFrom, uint16_t uiTo)
{
    uint8_t abtOperands[4];
    cecPutPhysAddr(abtOperands, uiFrom);
    cecPutPhysAddr(abtOperands + 2, uiTo);
    return cecBuild(stFrame, btInitiator, CEC_ADDR_BROADCAST, CEC_OP_ROUTING_CHANGE, abtOperands, 4);
}

bool cecBuildRoutingInformation(fwCecFrame & stFrame, uint8_t btInitiator, uint16_t uiPhysAddr)
{
    uint8_t abtOperands[2];
    cecPutPhysAddr(abtOperands, uiPhysAddr);
    return cecBuild(stFrame, btInitiator, CEC_ADDR_BROADCAST, CEC_OP_ROUTING_INFORMATION, abtOperands, 2);
}

bool cecBuildUserControlPressed(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint8_t btUiCommand)
{
    if (!directTo(btDestination))
        return false;
    return cecBuild(stFrame, btInitiator, btDestination, CEC_OP_USER_CONTROL_PRESSED, &btUiCommand, 1);
}

bool cecBuildSystemAudioModeRequest(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint16_t uiPhysAddr)
{
    if (!directTo(btDestination))
        return false;
    if (uiPhysAddr == CEC_PHYS_ADDR_INVALID)
        return cecBuild(stFrame, btInitiator, btDestination, CEC_OP_SYSTEM_AUDIO_MODE_REQUEST);
    uint8_t abtOperands[2];
    cecPutPhysAddr(abtOperands, uiPhysAddr);
    return cecBuild(stFrame, btInitiator, btDestination, CEC_OP_SYSTEM_AUDIO_MODE_REQUEST, abtOperands, 2);
}

// btLead, then iLen printable characters of pText.
static bool buildText(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint8_t btOpcode,
                      int iLead, uint8_t btLead, const char * pText, int iLen)
{
    uint8_t abtOperands[CEC_OPERANDS_MAX];
    if (iLen < 0 || iLead + iLen > CEC_OPERANDS_MAX || (iLen > 0 && pText == nullptr))
        return false;
    if (iLead > 0)
        abtOperands[0] = btLead;
    for (int i = 0; i < iLen; i++)
    {
        if (!isPrintable(pText[i]))
            return false;
        abtOperands[iLead + i] = (uint8_t)pText[i];
    }
    return cecBuild(stFrame, btInitiator, btDestination, btOpcode, abtOperands, iLead + iLen);
}

bool cecBuildSetOsdString(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint8_t btDisplayControl,
                          const char * pText, int iLen)
{
    if (!directTo(btDestination))
        return false;
    const bool bClear = btDisplayControl == CEC_DISPLAY_CLEAR;
    if (!bClear && btDisplayControl != CEC_DISPLAY_DEFAULT_TIME && btDisplayControl != CEC_DISPLAY_UNTIL_CLEARED)
        return false;
    if (iLen < (bClear ? 0 : 1) || iLen > CEC_OSD_STRING_MAX)
        return false;
    return buildText(stFrame, btInitiator, btDestination, CEC_OP_SET_OSD_STRING, 1, btDisplayControl, pText, iLen);
}

bool cecBuildSetTimerProgramTitle(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination,
                                  const char * pText, int iLen)
{
    if (!directTo(btDestination) || iLen < 1 || iLen > CEC_OPERANDS_MAX)
        return false;
    return buildText(stFrame, btInitiator, btDestination, CEC_OP_SET_TIMER_PROGRAM_TITLE, 0, 0, pText, iLen);
}

bool cecBuildVendorCommandWithId(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint32_t uiVendorId,
                                 const uint8_t * pData, int iLen)
{
    if (btDestination > 15 || uiVendorId > 0xFFFFFFu || iLen < 0 || iLen > CEC_OPERANDS_MAX - 3 ||
        (iLen > 0 && pData == nullptr))
        return false;
    uint8_t abtOperands[CEC_OPERANDS_MAX];
    abtOperands[0] = (uint8_t)(uiVendorId >> 16);
    abtOperands[1] = (uint8_t)(uiVendorId >> 8);
    abtOperands[2] = (uint8_t)uiVendorId;
    for (int i = 0; i < iLen; i++)
        abtOperands[3 + i] = pData[i];
    return cecBuild(stFrame, btInitiator, btDestination, CEC_OP_VENDOR_COMMAND_WITH_ID, abtOperands, 3 + iLen);
}

bool cecBuildFeatureAbortTo(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint8_t btOpcode,
                            uint8_t btReason)
{
    if (!directTo(btDestination))
        return false;
    const uint8_t abtOperands[2] = { btOpcode, btReason };
    return cecBuild(stFrame, btInitiator, btDestination, CEC_OP_FEATURE_ABORT, abtOperands, 2);
}

// A frame of btOpcode with at least iOperands operands.
static bool hasOperands(const fwCecFrame & stFrame, uint8_t btOpcode, int iOperands)
{
    return stFrame.btLen >= 2 && stFrame.btLen <= CEC_FRAME_MAX && cecOpcode(stFrame) == btOpcode &&
           cecOperandCount(stFrame) >= iOperands;
}

bool cecParseReportPhysicalAddress(const fwCecFrame & stFrame, uint16_t & uiPhysAddr, uint8_t & btDeviceType)
{
    if (!hasOperands(stFrame, CEC_OP_REPORT_PHYSICAL_ADDRESS, 3))
        return false;
    uiPhysAddr = cecGetPhysAddr(cecOperands(stFrame));
    btDeviceType = cecOperands(stFrame)[2];
    return true;
}

bool cecParseDeviceVendorId(const fwCecFrame & stFrame, uint32_t & uiVendorId)
{
    if (!hasOperands(stFrame, CEC_OP_DEVICE_VENDOR_ID, 3))
        return false;
    const uint8_t * pOp = cecOperands(stFrame);
    uiVendorId = ((uint32_t)pOp[0] << 16) | ((uint32_t)pOp[1] << 8) | pOp[2];
    return true;
}

int cecParseOsdName(const fwCecFrame & stFrame, char * szOut)
{
    if (!hasOperands(stFrame, CEC_OP_SET_OSD_NAME, 1))
        return -1;
    int iLen = cecOperandCount(stFrame);
    if (iLen > CEC_OSD_NAME_MAX)
        iLen = CEC_OSD_NAME_MAX;
    for (int i = 0; i < iLen; i++)
    {
        const char ch = (char)cecOperands(stFrame)[i];
        szOut[i] = isPrintable(ch) ? ch : '?';
    }
    szOut[iLen] = 0;
    return iLen;
}

// ---- bus scan ----------------------------------------------------------------
static const uint8_t s_abtScanQuestion[CEC_SCAN_QUESTIONS] =
{
    CEC_OP_GIVE_PHYSICAL_ADDRESS,
    CEC_OP_GIVE_DEVICE_VENDOR_ID,
    CEC_OP_GIVE_OSD_NAME,
    CEC_OP_GIVE_DEVICE_POWER_STATUS,
    CEC_OP_GET_CEC_VERSION,
};

// The first address from btFrom on that is polled, or CEC_SCAN_ADDRESSES.
static uint8_t scanAddressFrom(const fwCecScan & stScan, uint8_t btFrom)
{
    while (btFrom < CEC_SCAN_ADDRESSES && ((stScan.uiSkipMask >> btFrom) & 1u) != 0)
        btFrom++;
    return btFrom;
}

void cecScanStart(fwCecScan & stScan, uint16_t uiSkipMask)
{
    stScan.uiSkipMask = uiSkipMask;
    stScan.btStep = 0;
    for (int i = 0; i < CEC_SCAN_ADDRESSES; i++)
    {
        fwCecScanEntry & stEntry = stScan.astEntry[i];
        stEntry.bPresent      = false;
        stEntry.btAnswers     = 0;
        stEntry.uiPhysAddr    = CEC_PHYS_ADDR_INVALID;
        stEntry.btDeviceType  = CEC_DEVTYPE_NONE;
        stEntry.btPowerStatus = 0xFF;
        stEntry.btCecVersion  = 0;
        stEntry.uiVendorId    = CEC_VENDOR_ID_NONE;
        stEntry.szName[0]     = 0;
    }
    stScan.btAddress = scanAddressFrom(stScan, 0);
}

bool cecScanNext(const fwCecScan & stScan, uint8_t btInitiator, fwCecFrame & stFrame)
{
    if (stScan.btAddress >= CEC_SCAN_ADDRESSES)
        return false;
    if (stScan.btStep == 0)
        return cecBuildPoll(stFrame, btInitiator, stScan.btAddress);
    return cecBuild(stFrame, btInitiator, stScan.btAddress, s_abtScanQuestion[stScan.btStep - 1]);
}

static void scanNextAddress(fwCecScan & stScan)
{
    stScan.btStep = 0;
    stScan.btAddress = scanAddressFrom(stScan, (uint8_t)(stScan.btAddress + 1));
}

void cecScanTake(fwCecScan & stScan, bool bAcked, const fwCecFrame * pReply)
{
    if (stScan.btAddress >= CEC_SCAN_ADDRESSES)
        return;
    fwCecScanEntry & stEntry = stScan.astEntry[stScan.btAddress];
    if (!bAcked)
    {
        scanNextAddress(stScan);
        return;
    }
    if (stScan.btStep == 0)
    {
        stEntry.bPresent = true;
        stScan.btStep = 1;
        return;
    }

    const int iQuestion = stScan.btStep - 1;
    if (pReply != nullptr && cecInitiator(*pReply) == stScan.btAddress)
    {
        bool bAnswered = false;
        switch (iQuestion)
        {
            case CEC_SCAN_Q_PHYS_ADDR:
                bAnswered = cecParseReportPhysicalAddress(*pReply, stEntry.uiPhysAddr, stEntry.btDeviceType);
                break;
            case CEC_SCAN_Q_VENDOR_ID:
                bAnswered = cecParseDeviceVendorId(*pReply, stEntry.uiVendorId);
                break;
            case CEC_SCAN_Q_OSD_NAME:
                bAnswered = cecParseOsdName(*pReply, stEntry.szName) >= 0;
                break;
            case CEC_SCAN_Q_POWER:
                bAnswered = hasOperands(*pReply, CEC_OP_REPORT_POWER_STATUS, 1);
                if (bAnswered)
                    stEntry.btPowerStatus = cecOperands(*pReply)[0];
                break;
            case CEC_SCAN_Q_VERSION:
                bAnswered = hasOperands(*pReply, CEC_OP_CEC_VERSION, 1);
                if (bAnswered)
                    stEntry.btCecVersion = cecOperands(*pReply)[0];
                break;
        }
        if (bAnswered)
            stEntry.btAnswers = (uint8_t)(stEntry.btAnswers | (1u << iQuestion));
    }

    stScan.btStep++;
    if (stScan.btStep > CEC_SCAN_QUESTIONS)
        scanNextAddress(stScan);
}

uint16_t cecScanFoundMask(const fwCecScan & stScan)
{
    uint16_t uiMask = 0;
    for (int i = 0; i < CEC_SCAN_ADDRESSES; i++)
        if (stScan.astEntry[i].bPresent)
            uiMask = (uint16_t)(uiMask | (1u << i));
    return uiMask;
}

// ---- receive queue -------------------------------------------------------------
void cecRxQueueReset(fwCecRxQueue & stQueue, fwCecRxRecord * pRing, fwCecRxQueueMode eMode)
{
    stQueue.pRing     = pRing;
    stQueue.eMode     = pRing != nullptr ? eMode : fwCecRxQueueMode::Off;
    stQueue.iHead     = 0;
    stQueue.iCount    = 0;
    stQueue.uiDropped = 0;
}

bool cecRxQueueWants(fwCecRxQueueMode eMode, const fwCecFrame & stFrame, uint8_t btFlags)
{
    if (stFrame.btLen < 2 || stFrame.btLen > CEC_FRAME_MAX)
        return false;
    const bool bForUs = (btFlags & CEC_RX_FOR_US) != 0;
    switch (eMode)
    {
        case fwCecRxQueueMode::Off:  return false;
        case fwCecRxQueueMode::Ours: return bForUs;
        case fwCecRxQueueMode::All:  return true;
        case fwCecRxQueueMode::Keys:
            return bForUs && !cecIsBroadcast(stFrame) &&
                   (cecOpcode(stFrame) == CEC_OP_USER_CONTROL_PRESSED || cecOpcode(stFrame) == CEC_OP_USER_CONTROL_RELEASED);
    }
    return false;
}

bool cecRxQueuePush(fwCecRxQueue & stQueue, const fwCecFrame & stFrame, uint8_t btFlags, uint32_t uiTimestampUs)
{
    if (stQueue.pRing == nullptr || !cecRxQueueWants(stQueue.eMode, stFrame, btFlags))
        return false;
    if (stQueue.iCount >= CEC_RX_QUEUE_DEPTH)
    {
        // Full: the oldest record goes.
        stQueue.iHead = (stQueue.iHead + 1) % CEC_RX_QUEUE_DEPTH;
        stQueue.iCount = CEC_RX_QUEUE_DEPTH - 1;
        stQueue.uiDropped++;
    }
    fwCecRxRecord & stRecord = stQueue.pRing[(stQueue.iHead + stQueue.iCount) % CEC_RX_QUEUE_DEPTH];
    stRecord.uiTimestampUs = uiTimestampUs;
    stRecord.btFlags = btFlags;
    stRecord.stFrame = stFrame;
    stQueue.iCount++;
    return true;
}

void cecRxQueueLost(fwCecRxQueue & stQueue, uint32_t uiLost)
{
    if (stQueue.pRing != nullptr)
        stQueue.uiDropped += uiLost;
}

bool cecRxQueuePop(fwCecRxQueue & stQueue, fwCecRxRecord & stOut)
{
    if (stQueue.pRing == nullptr || stQueue.iCount <= 0)
        return false;
    stOut = stQueue.pRing[stQueue.iHead];
    stQueue.iHead = (stQueue.iHead + 1) % CEC_RX_QUEUE_DEPTH;
    stQueue.iCount--;
    return true;
}

// ---- held remote key -----------------------------------------------------------
void cecKeyHoldReset(fwCecKeyHold & stHold)
{
    stHold.bHeld         = false;
    stHold.bRepeats      = false;
    stHold.btKey         = 0;
    stHold.btRepeatCount = 0;
    stHold.uiDueMs       = 0;
}

void cecKeyHoldPress(fwCecKeyHold & stHold, uint8_t btKey, bool bRepeats, uint32_t uiNowMs,
                     const fwCecKeyTiming & stTiming)
{
    stHold.bHeld         = true;
    stHold.bRepeats      = bRepeats;
    stHold.btKey         = btKey;
    stHold.btRepeatCount = 0;
    stHold.uiDueMs       = uiNowMs + stTiming.uiDelayMs;
}

bool cecKeyHoldDue(fwCecKeyHold & stHold, uint32_t uiNowMs, const fwCecKeyTiming & stTiming)
{
    if (!stHold.bHeld || !stHold.bRepeats || (int32_t)(uiNowMs - stHold.uiDueMs) < 0)
        return false;
    if (stHold.btRepeatCount < 0xFF)
        stHold.btRepeatCount++;
    stHold.uiDueMs = uiNowMs + (stHold.btRepeatCount >= stTiming.btAccelAfter ? stTiming.uiFastMs : stTiming.uiRepeatMs);
    return true;
}

bool cecKeyHoldRelease(fwCecKeyHold & stHold)
{
    const bool bWasHeld = stHold.bHeld;
    cecKeyHoldReset(stHold);
    return bWasHeld;
}

// ---- names -------------------------------------------------------------------
const char * cecOpcodeName(uint8_t btOpcode)
{
    const fwCecMsgInfo * pInfo = cecMsgFind(btOpcode);
    return pInfo != nullptr ? pInfo->szName : "unknown";
}

const char * cecFeatureName(fwCecFeature eFeature)
{
    switch (eFeature)
    {
        case fwCecFeature::General:                  return "general";
        case fwCecFeature::OneTouchPlay:             return "one_touch_play";
        case fwCecFeature::RoutingControl:           return "routing_control";
        case fwCecFeature::SystemStandby:            return "system_standby";
        case fwCecFeature::OneTouchRecord:           return "one_touch_record";
        case fwCecFeature::TimerProgramming:         return "timer_programming";
        case fwCecFeature::SystemInformation:        return "system_information";
        case fwCecFeature::DeckControl:              return "deck_control";
        case fwCecFeature::TunerControl:             return "tuner_control";
        case fwCecFeature::VendorSpecific:           return "vendor_specific";
        case fwCecFeature::OsdDisplay:               return "osd_display";
        case fwCecFeature::DeviceOsdName:            return "device_osd_name";
        case fwCecFeature::DeviceMenuControl:        return "device_menu_control";
        case fwCecFeature::RemoteControlPassThrough: return "remote_control_pass_through";
        case fwCecFeature::PowerStatus:              return "power_status";
        case fwCecFeature::SystemAudioControl:       return "system_audio_control";
        case fwCecFeature::AudioRateControl:         return "audio_rate_control";
        case fwCecFeature::AudioReturnChannel:       return "audio_return_channel";
        case fwCecFeature::DynamicAutoLipsync:       return "dynamic_auto_lipsync";
        case fwCecFeature::CapabilityDiscovery:      return "capability_discovery";
    }
    return "unknown";
}

const char * cecCheckName(fwCecCheck eCheck)
{
    switch (eCheck)
    {
        case fwCecCheck::Ok:            return "ok";
        case fwCecCheck::Poll:          return "poll";
        case fwCecCheck::BadLength:     return "bad_length";
        case fwCecCheck::UnknownOpcode: return "unknown_opcode";
        case fwCecCheck::TooShort:      return "too_short";
        case fwCecCheck::WrongDelivery: return "wrong_delivery";
    }
    return "unknown";
}

const char * cecRxName(fwCecRx eRx)
{
    switch (eRx)
    {
        case fwCecRx::NotForUs:    return "not_for_us";
        case fwCecRx::Poll:        return "poll";
        case fwCecRx::Ignore:      return "ignore";
        case fwCecRx::Handle:      return "handle";
        case fwCecRx::Unsupported: return "unsupported";
    }
    return "unknown";
}

const char * cecAbortReasonName(uint8_t btReason)
{
    switch (btReason)
    {
        case CEC_ABORT_UNRECOGNIZED:    return "unrecognized";
        case CEC_ABORT_WRONG_MODE:      return "wrong_mode";
        case CEC_ABORT_NO_SOURCE:       return "no_source";
        case CEC_ABORT_INVALID_OPERAND: return "invalid_operand";
        case CEC_ABORT_REFUSED:         return "refused";
        case CEC_ABORT_UNDETERMINED:    return "undetermined";
    }
    return "unknown";
}

const char * cecAddressName(uint8_t btAddress)
{
    static const char * const s_aszName[16] =
    {
        "tv", "recorder1", "recorder2", "tuner1", "playback1", "audio_system", "tuner2", "tuner3",
        "playback2", "recorder3", "tuner4", "playback3", "backup1", "backup2", "specific", "unregistered",
    };
    return btAddress < 16 ? s_aszName[btAddress] : "unknown";
}

const char * cecDeviceTypeName(uint8_t btDeviceType)
{
    switch (btDeviceType)
    {
        case CEC_DEVTYPE_TV:           return "tv";
        case CEC_DEVTYPE_RECORDER:     return "recorder";
        case CEC_DEVTYPE_TUNER:        return "tuner";
        case CEC_DEVTYPE_PLAYBACK:     return "playback";
        case CEC_DEVTYPE_AUDIO_SYSTEM: return "audio_system";
        case CEC_DEVTYPE_SWITCH:       return "switch";
        case CEC_DEVTYPE_PROCESSOR:    return "processor";
        case CEC_DEVTYPE_NONE:         return "none";
    }
    return "unknown";
}

const char * cecPowerStatusName(uint8_t btPowerStatus)
{
    switch (btPowerStatus)
    {
        case CEC_POWER_ON:         return "on";
        case CEC_POWER_STANDBY:    return "standby";
        case CEC_POWER_TO_ON:      return "to_on";
        case CEC_POWER_TO_STANDBY: return "to_standby";
    }
    return "unknown";
}

// [UI Command] of CEC Table 30, ascending.
struct stUiName
{
    uint8_t      btCode;
    const char * szName;
};
static const stUiName s_astUi[] =
{
    { 0x00, "select" },             { 0x01, "up" },                 { 0x02, "down" },
    { 0x03, "left" },               { 0x04, "right" },              { 0x05, "right_up" },
    { 0x06, "right_down" },         { 0x07, "left_up" },            { 0x08, "left_down" },
    { 0x09, "root_menu" },          { 0x0A, "setup_menu" },         { 0x0B, "contents_menu" },
    { 0x0C, "favorite_menu" },      { 0x0D, "exit" },               { 0x10, "media_top_menu" },
    { 0x11, "media_context_menu" }, { 0x1D, "number_entry_mode" },  { 0x1E, "number_11" },
    { 0x1F, "number_12" },          { 0x20, "number_0" },           { 0x21, "number_1" },
    { 0x22, "number_2" },           { 0x23, "number_3" },           { 0x24, "number_4" },
    { 0x25, "number_5" },           { 0x26, "number_6" },           { 0x27, "number_7" },
    { 0x28, "number_8" },           { 0x29, "number_9" },           { 0x2A, "dot" },
    { 0x2B, "enter" },              { 0x2C, "clear" },              { 0x2F, "next_favorite" },
    { 0x30, "channel_up" },         { 0x31, "channel_down" },       { 0x32, "previous_channel" },
    { 0x33, "sound_select" },       { 0x34, "input_select" },       { 0x35, "display_information" },
    { 0x36, "help" },               { 0x37, "page_up" },            { 0x38, "page_down" },
    { 0x40, "power" },              { 0x41, "volume_up" },          { 0x42, "volume_down" },
    { 0x43, "mute" },               { 0x44, "play" },               { 0x45, "stop" },
    { 0x46, "pause" },              { 0x47, "record" },             { 0x48, "rewind" },
    { 0x49, "fast_forward" },       { 0x4A, "eject" },              { 0x4B, "forward" },
    { 0x4C, "backward" },           { 0x4D, "stop_record" },        { 0x4E, "pause_record" },
    { 0x50, "angle" },              { 0x51, "sub_picture" },        { 0x52, "video_on_demand" },
    { 0x53, "electronic_program_guide" }, { 0x54, "timer_programming" }, { 0x55, "initial_configuration" },
    { 0x56, "select_broadcast_type" },    { 0x57, "select_sound_presentation" },
    { 0x60, "play_function" },      { 0x61, "pause_play_function" },    { 0x62, "record_function" },
    { 0x63, "pause_record_function" },    { 0x64, "stop_function" },    { 0x65, "mute_function" },
    { 0x66, "restore_volume_function" },  { 0x67, "tune_function" },    { 0x68, "select_media_function" },
    { 0x69, "select_av_input_function" }, { 0x6A, "select_audio_input_function" },
    { 0x6B, "power_toggle_function" },    { 0x6C, "power_off_function" },   { 0x6D, "power_on_function" },
    { 0x71, "f1_blue" },            { 0x72, "f2_red" },             { 0x73, "f3_green" },
    { 0x74, "f4_yellow" },          { 0x75, "f5" },                 { 0x76, "data" },
};

const char * cecUiCommandName(uint8_t btUiCommand)
{
    for (const stUiName & stName : s_astUi)
        if (stName.btCode == btUiCommand)
            return stName.szName;
    return "unknown";
}

const char * cecStateName(fwCecState eState)
{
    switch (eState)
    {
        case fwCecState::Off:      return "off";
        case fwCecState::Claiming: return "claiming";
        case fwCecState::Ready:    return "ready";
    }
    return "unknown";
}

const char * cecEventName(fwCecEventType eType)
{
    switch (eType)
    {
        case fwCecEventType::AddressClaimed: return "address_claimed";
        case fwCecEventType::AddressNone:    return "address_none";
        case fwCecEventType::Standby:        return "standby";
        case fwCecEventType::ActiveSource:   return "active_source";
        case fwCecEventType::KeyDown:        return "key_down";
        case fwCecEventType::KeyUp:          return "key_up";
        case fwCecEventType::MenuRequest:    return "menu_request";
        case fwCecEventType::MenuLanguage:   return "menu_language";
        case fwCecEventType::DeckPlay:       return "deck_play";
        case fwCecEventType::DeckControl:    return "deck_control";
        case fwCecEventType::FeatureAbort:   return "feature_abort";
    }
    return "unknown";
}

const char * cecMatchName(fwCecMatch eMatch)
{
    switch (eMatch)
    {
        case fwCecMatch::None:    return "none";
        case fwCecMatch::Reply:   return "reply";
        case fwCecMatch::Refused: return "refused";
    }
    return "unknown";
}
