// bsp/cec/cec_engine.cpp — see cec_engine.h.
#include "cec_engine.h"

#include <stddef.h>

#include "cec.h"

#include "fwCECCore.h"
#include "fwCECWire.h"

// The SDK puts PICO_ON_DEVICE=1 on every target compile line; the host tests
// never see an SDK header.
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define CEC_ENGINE_RAMFUNC(fn) __not_in_flash_func(fn)
#else
#define CEC_ENGINE_RAMFUNC(fn) fn
#endif

static_assert(CEC_ENGINE_FRAME_MAX == CEC_FRAME_MAX && CEC_FRAME_BLOCKS == CEC_FRAME_MAX, "frame size differs");
static_assert(CEC_ENGINE_OUT_FRAMES_MAX == CEC_OUT_FRAMES_MAX, "out frame count differs");
static_assert(CEC_ENGINE_OUT_EVENTS_MAX == CEC_OUT_EVENTS_MAX, "out event count differs");
static_assert(CEC_ENGINE_ADDR_NONE == CEC_ADDR_UNREGISTERED && CEC_ENGINE_ADDR_NONE == CEC_ADDR_BROADCAST, "address 15");
static_assert(CEC_ENGINE_ADDR_TV == CEC_ADDR_TV, "TV address differs");
static_assert(CEC_ENGINE_PHYS_ADDR_NONE == CEC_PHYS_ADDR_INVALID, "no physical address differs");
static_assert(CEC_ENGINE_RX_ACKED == CEC_WIRE_RX_ACKED && CEC_ENGINE_RX_FOR_US == CEC_WIRE_RX_FOR_US, "rx flags differ");
static_assert(CEC_ENGINE_LATE_MAX_US == CEC_WIRE_LATE_MAX_US, "lateness limit differs");
static_assert(CEC_KEY_SELECT == CEC_UI_SELECT && CEC_KEY_UP == CEC_UI_UP && CEC_KEY_DOWN == CEC_UI_DOWN &&
              CEC_KEY_LEFT == CEC_UI_LEFT && CEC_KEY_RIGHT == CEC_UI_RIGHT && CEC_KEY_ROOT_MENU == CEC_UI_ROOT_MENU &&
              CEC_KEY_EXIT == CEC_UI_EXIT && CEC_KEY_PLAY == CEC_UI_PLAY && CEC_KEY_STOP == CEC_UI_STOP &&
              CEC_KEY_PAUSE == CEC_UI_PAUSE && CEC_KEY_F1_BLUE == CEC_UI_F1_BLUE && CEC_KEY_F2_RED == CEC_UI_F2_RED &&
              CEC_KEY_F3_GREEN == CEC_UI_F3_GREEN && CEC_KEY_F4_YELLOW == CEC_UI_F4_YELLOW &&
              CEC_KEY_POWER == CEC_UI_POWER && CEC_KEY_VOLUME_UP == CEC_UI_VOLUME_UP &&
              CEC_KEY_VOLUME_DOWN == CEC_UI_VOLUME_DOWN && CEC_KEY_MUTE == CEC_UI_MUTE, "key codes differ");
static_assert(CEC_PHYS_ADDR_NONE == CEC_PHYS_ADDR_INVALID && CEC_VENDOR_NONE == CEC_VENDOR_ID_NONE &&
              CEC_VENDOR_ID_DEFAULT == CEC_VENDOR_ID_PRODUCT, "address or vendor values differ");
static_assert(CEC_LOGICAL_TV == CEC_ADDR_TV && CEC_LOGICAL_RECORDER_1 == CEC_ADDR_RECORDER_1 &&
              CEC_LOGICAL_TUNER_1 == CEC_ADDR_TUNER_1 && CEC_LOGICAL_PLAYBACK_1 == CEC_ADDR_PLAYBACK_1 &&
              CEC_LOGICAL_AUDIO_SYSTEM == CEC_ADDR_AUDIO_SYSTEM && CEC_LOGICAL_ALL == CEC_ADDR_BROADCAST,
              "logical addresses differ");
static_assert(CEC_TYPE_TV == CEC_DEVTYPE_TV && CEC_TYPE_RECORDER == CEC_DEVTYPE_RECORDER &&
              CEC_TYPE_TUNER == CEC_DEVTYPE_TUNER && CEC_TYPE_PLAYBACK == CEC_DEVTYPE_PLAYBACK &&
              CEC_TYPE_AUDIO_SYSTEM == CEC_DEVTYPE_AUDIO_SYSTEM && CEC_TYPE_NONE == CEC_DEVTYPE_NONE,
              "device types differ");
static_assert(CEC_MENU_ACTIVATE == CEC_MENU_REQUEST_ACTIVATE && CEC_MENU_DEACTIVATE == CEC_MENU_REQUEST_DEACTIVATE &&
              CEC_MENU_QUERY == CEC_MENU_REQUEST_QUERY && CEC_MENU_OPEN == CEC_MENU_STATE_ACTIVATED &&
              CEC_MENU_CLOSED == CEC_MENU_STATE_DEACTIVATED, "menu values differ");
static_assert(CEC_OSD_SHOW_DEFAULT == CEC_DISPLAY_DEFAULT_TIME && CEC_OSD_SHOW_UNTIL_CLEARED == CEC_DISPLAY_UNTIL_CLEARED &&
              CEC_OSD_CLEAR == CEC_DISPLAY_CLEAR, "display control values differ");
static_assert(CEC_REASON_UNRECOGNIZED == CEC_ABORT_UNRECOGNIZED && CEC_REASON_WRONG_MODE == CEC_ABORT_WRONG_MODE &&
              CEC_REASON_NO_SOURCE == CEC_ABORT_NO_SOURCE && CEC_REASON_INVALID_OPERAND == CEC_ABORT_INVALID_OPERAND &&
              CEC_REASON_REFUSED == CEC_ABORT_REFUSED && CEC_REASON_UNDETERMINED == CEC_ABORT_UNDETERMINED,
              "abort reasons differ");
static_assert(CEC_POWER_STATUS_ON == CEC_POWER_ON && CEC_POWER_STATUS_STANDBY == CEC_POWER_STANDBY &&
              CEC_POWER_STATUS_TO_ON == CEC_POWER_TO_ON && CEC_POWER_STATUS_TO_STANDBY == CEC_POWER_TO_STANDBY,
              "power status values differ");

static_assert(sizeof(cec_frame_t) == sizeof(fwCecFrame) && offsetof(cec_frame_t, block) == offsetof(fwCecFrame, abtBlock),
              "frame layouts differ");
static_assert(sizeof(cec_wire_out_t) == sizeof(fwCecWireOut) &&
              offsetof(cec_wire_out_t, drive_low) == offsetof(fwCecWireOut, bDriveLow) &&
              offsetof(cec_wire_out_t, alarm) == offsetof(fwCecWireOut, bAlarm) &&
              offsetof(cec_wire_out_t, alarm_us) == offsetof(fwCecWireOut, uiAlarmUs),
              "wire out layouts differ");
static_assert(sizeof(cec_engine_event_t) == sizeof(fwCecEvent) &&
              offsetof(cec_engine_event_t, peer) == offsetof(fwCecEvent, btPeer) &&
              offsetof(cec_engine_event_t, own) == offsetof(fwCecEvent, btOwn) &&
              offsetof(cec_engine_event_t, data) == offsetof(fwCecEvent, abtData),
              "event layouts differ");
static_assert(sizeof(cec_engine_out_t) == sizeof(fwCecOut) &&
              offsetof(cec_engine_out_t, events) == offsetof(fwCecOut, btEvents) &&
              offsetof(cec_engine_out_t, overflow) == offsetof(fwCecOut, bOverflow) &&
              offsetof(cec_engine_out_t, frame) == offsetof(fwCecOut, astFrame) &&
              offsetof(cec_engine_out_t, event) == offsetof(fwCecOut, astEvent),
              "out layouts differ");

static_assert((int)CEC_ENGINE_TX_ACKED == (int)fwCecTxResult::Acked &&
              (int)CEC_ENGINE_TX_NOT_ACKED == (int)fwCecTxResult::NotAcked &&
              (int)CEC_ENGINE_TX_FAILED == (int)fwCecTxResult::Failed, "tx results differ");
static_assert((int)CEC_ENGINE_EV_ADDRESS_CLAIMED == (int)fwCecEventType::AddressClaimed &&
              (int)CEC_ENGINE_EV_ADDRESS_NONE == (int)fwCecEventType::AddressNone &&
              (int)CEC_ENGINE_EV_STANDBY == (int)fwCecEventType::Standby &&
              (int)CEC_ENGINE_EV_ACTIVE_SOURCE == (int)fwCecEventType::ActiveSource &&
              (int)CEC_ENGINE_EV_KEY_DOWN == (int)fwCecEventType::KeyDown &&
              (int)CEC_ENGINE_EV_KEY_UP == (int)fwCecEventType::KeyUp &&
              (int)CEC_ENGINE_EV_MENU_REQUEST == (int)fwCecEventType::MenuRequest &&
              (int)CEC_ENGINE_EV_MENU_LANGUAGE == (int)fwCecEventType::MenuLanguage &&
              (int)CEC_ENGINE_EV_DECK_PLAY == (int)fwCecEventType::DeckPlay &&
              (int)CEC_ENGINE_EV_DECK_CONTROL == (int)fwCecEventType::DeckControl &&
              (int)CEC_ENGINE_EV_FEATURE_ABORT == (int)fwCecEventType::FeatureAbort, "event types differ");

namespace {

fwCecWire s_wire;
fwCecDevice s_device;

inline fwCecWireOut &wire_out(cec_wire_out_t *out) { return *reinterpret_cast<fwCecWireOut *>(out); }
inline const fwCecFrame &frame_in(const cec_frame_t *frame) { return *reinterpret_cast<const fwCecFrame *>(frame); }
inline fwCecOut &device_out(cec_engine_out_t *out) { return *reinterpret_cast<fwCecOut *>(out); }

}

// ---- engine: main loop ------------------------------------------------------

void cec_engine_wire_init(uint32_t now_us) { cecWireInit(s_wire, now_us); }

void cec_engine_wire_set_ack_mask(uint16_t mask) { cecWireSetAckMask(s_wire, mask); }

void cec_engine_wire_set_monitor_all(bool monitor_all) { cecWireSetMonitorAll(s_wire, monitor_all); }

bool cec_engine_wire_pop_rx(cec_frame_t *frame, uint8_t *flags)
{
    fwCecWireRxFrame rx;
    if (!cecWirePopRx(s_wire, rx))
        return false;
    *reinterpret_cast<fwCecFrame *>(frame) = rx.stFrame;
    *flags = rx.btFlags;
    return true;
}

bool cec_engine_wire_pop_tx_done(cec_frame_t *frame, cec_engine_tx_result_t *result)
{
    fwCecWireTxDone done;
    if (!cecWirePopTxDone(s_wire, done))
        return false;
    *reinterpret_cast<fwCecFrame *>(frame) = done.stFrame;
    *result = (cec_engine_tx_result_t)done.eResult;
    return true;
}

void cec_engine_wire_counts(cec_engine_wire_counts_t *out)
{
    out->rx_frames = s_wire.uiRxFrames;
    out->rx_errors = s_wire.uiRxErrors;
    out->rx_overruns = s_wire.uiRxOverruns;
    out->error_signals = s_wire.uiErrorSignals;
    out->tx_retries = s_wire.uiTxRetries;
    out->arbitration_lost = s_wire.uiArbitrationLost;
    out->line_errors = s_wire.uiLineErrors;
    out->pull_not_seen = s_wire.uiPullNotSeen;
    out->late_aborts = s_wire.uiLateAborts;
    out->rx_dropped = s_wire.uiRxDropped;
}

// The handlers add to some of these while they are cleared: a count may then
// be one too low.
void cec_engine_wire_clear_counts(void)
{
    s_wire.uiRxFrames = 0;
    s_wire.uiRxErrors = 0;
    s_wire.uiRxOverruns = 0;
    s_wire.uiErrorSignals = 0;
    s_wire.uiTxRetries = 0;
    s_wire.uiArbitrationLost = 0;
    s_wire.uiLineErrors = 0;
    s_wire.uiPullNotSeen = 0;
    s_wire.uiLateAborts = 0;
    s_wire.uiRxDropped = 0;
}

// ---- engine: handlers held off, and the handlers ------------------------------

bool CEC_ENGINE_RAMFUNC(cec_engine_wire_send)(const cec_frame_t *frame, uint32_t now_us, cec_wire_out_t *out)
{
    return cecWireSend(s_wire, frame_in(frame), now_us, wire_out(out));
}

bool CEC_ENGINE_RAMFUNC(cec_engine_wire_cancel)(cec_wire_out_t *out)
{
    return cecWireCancel(s_wire, wire_out(out));
}

void CEC_ENGINE_RAMFUNC(cec_engine_on_edge)(bool falling, uint32_t now_us, cec_wire_out_t *out)
{
    cecWireOnEdge(s_wire, falling, now_us, wire_out(out));
}

void CEC_ENGINE_RAMFUNC(cec_engine_on_edge_pair)(bool first_falling, uint32_t now_us, cec_wire_out_t *out)
{
    cecWireOnEdgePair(s_wire, first_falling, now_us, wire_out(out));
}

void CEC_ENGINE_RAMFUNC(cec_engine_on_alarm)(uint32_t now_us, bool line_high, cec_wire_out_t *out)
{
    cecWireOnAlarm(s_wire, now_us, line_high, wire_out(out));
}

// ---- device -------------------------------------------------------------------

// The name osd_name, or the default when it has no printable character.
static void set_osd_name(fwCecConfig &config, const char *osd_name)
{
    if (osd_name != nullptr)
        cecConfigSetOsdName(config, osd_name);
    else
        config.btOsdNameLen = 0;
    if (config.btOsdNameLen == 0)
        cecConfigSetOsdName(config, CEC_OSD_NAME_DEFAULT);
}

void cec_engine_device_init(const char *osd_name, uint32_t vendor_id, uint8_t first_type, uint8_t second_type)
{
    static_assert(CEC_ROLES_MAX == 2, "two device types");
    fwCecConfig config;
    cecConfigDefault(config);
    config.uiVendorId = vendor_id;
    config.abtDeviceType[0] = first_type;
    config.abtDeviceType[1] = second_type;
    set_osd_name(config, osd_name);
    cecDeviceInit(s_device, config);
}

void cec_engine_device_set_osd_name(const char *osd_name) { set_osd_name(s_device.stConfig, osd_name); }

void cec_engine_device_set_vendor_id(uint32_t vendor_id) { s_device.stConfig.uiVendorId = vendor_id; }

void cec_engine_device_osd_name(char *out)
{
    const fwCecConfig &config = s_device.stConfig;
    for (int i = 0; i < config.btOsdNameLen; i++)
        out[i] = config.achOsdName[i];
    out[config.btOsdNameLen] = 0;
}

bool cec_engine_device_ready(void) { return s_device.eState == fwCecState::Ready; }

void cec_engine_device_start(uint16_t phys_addr, cec_engine_out_t *out)
{
    cecDeviceStart(s_device, phys_addr, device_out(out));
}

void cec_engine_device_stop(cec_engine_out_t *out) { cecDeviceStop(s_device, device_out(out)); }

void cec_engine_device_on_tx_done(const cec_frame_t *sent, cec_engine_tx_result_t result, cec_engine_out_t *out)
{
    cecDeviceOnTxDone(s_device, frame_in(sent), (fwCecTxResult)result, device_out(out));
}

void cec_engine_device_on_rx(const cec_frame_t *frame, uint32_t now_ms, cec_engine_out_t *out)
{
    cecDeviceOnRx(s_device, frame_in(frame), now_ms, device_out(out));
}

void cec_engine_device_on_tick(uint32_t now_ms, cec_engine_out_t *out)
{
    cecDeviceOnTick(s_device, now_ms, device_out(out));
}

uint16_t cec_engine_device_address_mask(void) { return cecDeviceAddressMask(s_device); }

uint8_t cec_engine_device_address(void) { return cecDeviceSourceAddress(s_device); }

bool cec_engine_device_one_touch_play(cec_engine_out_t *out)
{
    return cecDeviceOneTouchPlay(s_device, false, device_out(out));
}

bool cec_engine_device_standby(uint8_t destination, cec_engine_out_t *out)
{
    return cecDeviceStandby(s_device, destination, device_out(out));
}

const char *cec_engine_key_name(uint8_t ui_command) { return cecUiCommandName(ui_command); }

const char *cec_engine_opcode_name(uint8_t opcode) { return cecOpcodeName(opcode); }

const char *cec_engine_device_type_name(uint8_t device_type) { return cecDeviceTypeName(device_type); }

const char *cec_engine_power_status_name(uint8_t power_status) { return cecPowerStatusName(power_status); }

const char *cec_engine_abort_reason_name(uint8_t reason) { return cecAbortReasonName(reason); }
