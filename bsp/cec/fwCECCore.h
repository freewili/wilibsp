//
// fwCECCore -- the pure CEC core. The message layer: the frame type,
// the table of standard messages (opcode, name, feature, delivery, operand
// count), frame builders, the frame check, the follower's receive rules and
// the <Feature Abort> rules. The device: the address claim, the answers
// every device owes, and one switchable behaviour per feature. Shared by
// the wire layer, the host commands and the host tests
// (testprojects/cec_tests).
//
// Dependency-free: no Pico SDK header, no heap, no C++ exceptions, no RTTI,
// no mutable globals (the only static data is the message and name tables,
// never written). Everything works on caller buffers.
//
// Frame (CEC 6): 1..16 blocks of one byte. Block 0 is the header,
// initiator address in the high nibble and destination in the low nibble.
// Block 1 is the opcode and blocks 2.. are its operands (0..14). A header
// alone is a poll. Destination 15 is the broadcast address; initiator 15 is
// a device with no logical address.
//
// Message table: one row per opcode of CEC 1.4 and five CEC 2.0 additions.
// A row holds where the message may be sent (directly, broadcast or either)
// and its operand count range. Operands past the maximum are legal on the
// wire and ignored by a follower (CEC 8); fewer than the minimum is not a
// message (CEC 7.3).
//
// Names have no spaces: they go into console lines as single tokens.
//
// Style: Allman braces, b/bt/i/ui/sz prefixes (fwModbusCore).
//

#pragma once

#include <stdint.h>

// ---- sizes -----------------------------------------------------------------
#define CEC_FRAME_MAX               16      // header + opcode + operands
#define CEC_OPERANDS_MAX            14

// ---- logical addresses (CEC 10.2) ------------------------------------------
#define CEC_ADDR_TV                 0
#define CEC_ADDR_RECORDER_1         1
#define CEC_ADDR_RECORDER_2         2
#define CEC_ADDR_TUNER_1            3
#define CEC_ADDR_PLAYBACK_1         4
#define CEC_ADDR_AUDIO_SYSTEM       5
#define CEC_ADDR_TUNER_2            6
#define CEC_ADDR_TUNER_3            7
#define CEC_ADDR_PLAYBACK_2         8
#define CEC_ADDR_RECORDER_3         9
#define CEC_ADDR_TUNER_4            10
#define CEC_ADDR_PLAYBACK_3         11
#define CEC_ADDR_BACKUP_1           12
#define CEC_ADDR_BACKUP_2           13
#define CEC_ADDR_SPECIFIC           14
#define CEC_ADDR_UNREGISTERED       15      // as initiator
#define CEC_ADDR_BROADCAST          15      // as destination

// No position in the connection tree: F.F.F.F.
#define CEC_PHYS_ADDR_INVALID       0xFFFFu

// ---- opcodes ---------------------------------------------------------------
#define CEC_OP_FEATURE_ABORT                    0x00
#define CEC_OP_IMAGE_VIEW_ON                    0x04
#define CEC_OP_TUNER_STEP_INCREMENT             0x05
#define CEC_OP_TUNER_STEP_DECREMENT             0x06
#define CEC_OP_TUNER_DEVICE_STATUS              0x07
#define CEC_OP_GIVE_TUNER_DEVICE_STATUS         0x08
#define CEC_OP_RECORD_ON                        0x09
#define CEC_OP_RECORD_STATUS                    0x0A
#define CEC_OP_RECORD_OFF                       0x0B
#define CEC_OP_TEXT_VIEW_ON                     0x0D
#define CEC_OP_RECORD_TV_SCREEN                 0x0F
#define CEC_OP_GIVE_DECK_STATUS                 0x1A
#define CEC_OP_DECK_STATUS                      0x1B
#define CEC_OP_SET_MENU_LANGUAGE                0x32
#define CEC_OP_CLEAR_ANALOGUE_TIMER             0x33
#define CEC_OP_SET_ANALOGUE_TIMER               0x34
#define CEC_OP_TIMER_STATUS                     0x35
#define CEC_OP_STANDBY                          0x36
#define CEC_OP_PLAY                             0x41
#define CEC_OP_DECK_CONTROL                     0x42
#define CEC_OP_TIMER_CLEARED_STATUS             0x43
#define CEC_OP_USER_CONTROL_PRESSED             0x44
#define CEC_OP_USER_CONTROL_RELEASED            0x45
#define CEC_OP_GIVE_OSD_NAME                    0x46
#define CEC_OP_SET_OSD_NAME                     0x47
#define CEC_OP_SET_OSD_STRING                   0x64
#define CEC_OP_SET_TIMER_PROGRAM_TITLE          0x67
#define CEC_OP_SYSTEM_AUDIO_MODE_REQUEST        0x70
#define CEC_OP_GIVE_AUDIO_STATUS                0x71
#define CEC_OP_SET_SYSTEM_AUDIO_MODE            0x72
#define CEC_OP_SET_AUDIO_VOLUME_LEVEL           0x73
#define CEC_OP_REPORT_AUDIO_STATUS              0x7A
#define CEC_OP_GIVE_SYSTEM_AUDIO_MODE_STATUS    0x7D
#define CEC_OP_SYSTEM_AUDIO_MODE_STATUS         0x7E
#define CEC_OP_ROUTING_CHANGE                   0x80
#define CEC_OP_ROUTING_INFORMATION              0x81
#define CEC_OP_ACTIVE_SOURCE                    0x82
#define CEC_OP_GIVE_PHYSICAL_ADDRESS            0x83
#define CEC_OP_REPORT_PHYSICAL_ADDRESS          0x84
#define CEC_OP_REQUEST_ACTIVE_SOURCE            0x85
#define CEC_OP_SET_STREAM_PATH                  0x86
#define CEC_OP_DEVICE_VENDOR_ID                 0x87
#define CEC_OP_VENDOR_COMMAND                   0x89
#define CEC_OP_VENDOR_REMOTE_BUTTON_DOWN        0x8A
#define CEC_OP_VENDOR_REMOTE_BUTTON_UP          0x8B
#define CEC_OP_GIVE_DEVICE_VENDOR_ID            0x8C
#define CEC_OP_MENU_REQUEST                     0x8D
#define CEC_OP_MENU_STATUS                      0x8E
#define CEC_OP_GIVE_DEVICE_POWER_STATUS         0x8F
#define CEC_OP_REPORT_POWER_STATUS              0x90
#define CEC_OP_GET_MENU_LANGUAGE                0x91
#define CEC_OP_SELECT_ANALOGUE_SERVICE          0x92
#define CEC_OP_SELECT_DIGITAL_SERVICE           0x93
#define CEC_OP_SET_DIGITAL_TIMER                0x97
#define CEC_OP_CLEAR_DIGITAL_TIMER              0x99
#define CEC_OP_SET_AUDIO_RATE                   0x9A
#define CEC_OP_INACTIVE_SOURCE                  0x9D
#define CEC_OP_CEC_VERSION                      0x9E
#define CEC_OP_GET_CEC_VERSION                  0x9F
#define CEC_OP_VENDOR_COMMAND_WITH_ID           0xA0
#define CEC_OP_CLEAR_EXTERNAL_TIMER             0xA1
#define CEC_OP_SET_EXTERNAL_TIMER               0xA2
#define CEC_OP_REPORT_SHORT_AUDIO_DESCRIPTOR    0xA3
#define CEC_OP_REQUEST_SHORT_AUDIO_DESCRIPTOR   0xA4
#define CEC_OP_GIVE_FEATURES                    0xA5
#define CEC_OP_REPORT_FEATURES                  0xA6
#define CEC_OP_REQUEST_CURRENT_LATENCY          0xA7
#define CEC_OP_REPORT_CURRENT_LATENCY           0xA8
#define CEC_OP_INITIATE_ARC                     0xC0
#define CEC_OP_REPORT_ARC_INITIATED             0xC1
#define CEC_OP_REPORT_ARC_TERMINATED            0xC2
#define CEC_OP_REQUEST_ARC_INITIATION           0xC3
#define CEC_OP_REQUEST_ARC_TERMINATION          0xC4
#define CEC_OP_TERMINATE_ARC                    0xC5
#define CEC_OP_CDC_MESSAGE                      0xF8
#define CEC_OP_ABORT                            0xFF

// ---- <Feature Abort> reasons (CEC 17, [Abort Reason]) ----------------------
#define CEC_ABORT_UNRECOGNIZED      0   // unrecognized opcode
#define CEC_ABORT_WRONG_MODE        1   // not in the correct mode to respond
#define CEC_ABORT_NO_SOURCE         2   // cannot provide source
#define CEC_ABORT_INVALID_OPERAND   3
#define CEC_ABORT_REFUSED           4
#define CEC_ABORT_UNDETERMINED      5   // unable to determine

// ---- frame -----------------------------------------------------------------
struct fwCecFrame
{
    uint8_t btLen;                      // blocks in use, 1..CEC_FRAME_MAX
    uint8_t abtBlock[CEC_FRAME_MAX];    // [0] header, [1] opcode, [2..] operands
};

static inline uint8_t cecInitiator(const fwCecFrame & stFrame)   { return (uint8_t)(stFrame.abtBlock[0] >> 4); }
static inline uint8_t cecDestination(const fwCecFrame & stFrame) { return (uint8_t)(stFrame.abtBlock[0] & 0x0F); }
static inline bool cecIsPoll(const fwCecFrame & stFrame)         { return stFrame.btLen == 1; }
static inline bool cecIsBroadcast(const fwCecFrame & stFrame)    { return cecDestination(stFrame) == CEC_ADDR_BROADCAST; }
// Opcode and operands of a frame that is not a poll.
static inline uint8_t cecOpcode(const fwCecFrame & stFrame)      { return stFrame.abtBlock[1]; }
static inline int cecOperandCount(const fwCecFrame & stFrame)    { return stFrame.btLen >= 2 ? stFrame.btLen - 2 : 0; }
static inline const uint8_t * cecOperands(const fwCecFrame & stFrame) { return &stFrame.abtBlock[2]; }

// A physical address is two operand bytes, high byte first: A.B.C.D is 0xABCD.
static inline uint16_t cecGetPhysAddr(const uint8_t * pOperand)
{
    return (uint16_t)(((uint16_t)pOperand[0] << 8) | pOperand[1]);
}
static inline void cecPutPhysAddr(uint8_t * pOperand, uint16_t uiPhysAddr)
{
    pOperand[0] = (uint8_t)(uiPhysAddr >> 8);
    pOperand[1] = (uint8_t)(uiPhysAddr & 0xFF);
}

// ---- message table ---------------------------------------------------------
// Where a message may be sent.
#define CEC_TO_DIRECT               0x01    // to one logical address
#define CEC_TO_BROADCAST            0x02    // to address 15
#define CEC_TO_EITHER               0x03

// CEC_MSG_FROM_UNREGISTERED: a follower acts on the message even from an
// initiator with no logical address (CEC 12.2): <Standby>, <Active Source>,
// <Report Physical Address>, a switch's routing messages, CDC, and the
// requests whose answer is a broadcast.
#define CEC_MSG_FROM_UNREGISTERED   0x01
// CEC_MSG_REPORT: the message answers a request or announces a state. A
// device with no use for one drops it; it is not refused with <Feature Abort>.
#define CEC_MSG_REPORT              0x02

// Declaration order is the console value.
enum class fwCecFeature : uint8_t
{
    General = 0,
    OneTouchPlay,
    RoutingControl,
    SystemStandby,
    OneTouchRecord,
    TimerProgramming,
    SystemInformation,
    DeckControl,
    TunerControl,
    VendorSpecific,
    OsdDisplay,
    DeviceOsdName,
    DeviceMenuControl,
    RemoteControlPassThrough,
    PowerStatus,
    SystemAudioControl,
    AudioRateControl,
    AudioReturnChannel,
    DynamicAutoLipsync,
    CapabilityDiscovery,
};

struct fwCecMsgInfo
{
    uint8_t      btOpcode;
    uint8_t      btTo;              // CEC_TO_*
    uint8_t      btOperandsMin;
    uint8_t      btOperandsMax;     // <= CEC_OPERANDS_MAX
    uint8_t      btFlags;           // CEC_MSG_*
    fwCecFeature eFeature;
    const char * szName;            // "active_source"
};

// The table, in ascending opcode order.
int cecMsgCount();
const fwCecMsgInfo & cecMsgAt(int iIndex);              // 0..cecMsgCount() - 1
const fwCecMsgInfo * cecMsgFind(uint8_t btOpcode);      // nullptr: not a table message

// ---- build -----------------------------------------------------------------
// Header, opcode and operands into stFrame. Any opcode can be built; the
// table is not consulted. False (stFrame untouched) for an address above 15,
// a negative count or more than CEC_OPERANDS_MAX operands.
bool cecBuild(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint8_t btOpcode,
              const uint8_t * pOperands = nullptr, int iOperandCount = 0);
// A header-only frame. False for an address above 15.
bool cecBuildPoll(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination);
// <Feature Abort> answering stRx: from its destination to its initiator,
// operands {opcode, reason}. False (stReply untouched) where no answer is
// allowed: a poll, a broadcast, <Feature Abort> itself, or an unregistered
// initiator.
bool cecBuildFeatureAbort(fwCecFrame & stReply, const fwCecFrame & stRx, uint8_t btReason);

// ---- check -----------------------------------------------------------------
// Declaration order is the console value.
enum class fwCecCheck : uint8_t
{
    Ok = 0,         // a table message, sent where the table allows, with at least its minimum operands
    Poll,           // header only
    BadLength,      // no blocks, or more than CEC_FRAME_MAX
    UnknownOpcode,  // not a table message
    TooShort,       // fewer operands than the message needs
    WrongDelivery,  // a broadcast-only message sent directly, or the reverse
};
fwCecCheck cecCheck(const fwCecFrame & stFrame);

// ---- follower --------------------------------------------------------------
// What a device does with a received frame (CEC 12). A directly addressed
// message it does not support is answered with <Feature Abort>; a broadcast
// is never answered that way, and neither is a <Feature Abort>, which would
// loop. Declaration order is the console value.
enum class fwCecRx : uint8_t
{
    NotForUs = 0,   // directly addressed to another device
    Poll,           // a poll of one of our addresses: the acknowledge is the whole answer
    Ignore,         // dropped without an answer
    Handle,         // a valid table message for us
    Unsupported,    // directly addressed to us and not a table message: <Feature Abort>, unrecognized opcode
};
// uiOwnMask: bit n set = this device holds logical address n. A broadcast is
// for every device. Ignore covers a malformed frame, a poll of the broadcast
// address, a message with too few operands or the wrong delivery, a broadcast
// that is not a table message, and a message from an unregistered initiator
// that lacks CEC_MSG_FROM_UNREGISTERED.
fwCecRx cecClassifyRx(const fwCecFrame & stFrame, uint16_t uiOwnMask);

// ---- operand values (CEC 17) -----------------------------------------------
// [Device Type] of <Report Physical Address>.
#define CEC_DEVTYPE_TV              0
#define CEC_DEVTYPE_RECORDER        1
#define CEC_DEVTYPE_TUNER           3
#define CEC_DEVTYPE_PLAYBACK        4
#define CEC_DEVTYPE_AUDIO_SYSTEM    5
#define CEC_DEVTYPE_SWITCH          6
#define CEC_DEVTYPE_PROCESSOR       7
#define CEC_DEVTYPE_NONE            0xFF    // no device in this slot

// [Power Status].
#define CEC_POWER_ON                0
#define CEC_POWER_STANDBY           1
#define CEC_POWER_TO_ON             2       // in transition, standby to on
#define CEC_POWER_TO_STANDBY        3       // in transition, on to standby

// [CEC Version].
#define CEC_VERSION_1_3A            0x04
#define CEC_VERSION_1_4             0x05    // 1.4, 1.4a and 1.4b
#define CEC_VERSION_2_0             0x06

// [Menu Request Type] and [Menu State].
#define CEC_MENU_REQUEST_ACTIVATE   0
#define CEC_MENU_REQUEST_DEACTIVATE 1
#define CEC_MENU_REQUEST_QUERY      2
#define CEC_MENU_STATE_ACTIVATED    0
#define CEC_MENU_STATE_DEACTIVATED  1

// [Status Request] of <Give Deck Status>.
#define CEC_STATUS_REQUEST_ON       1       // now, and on every change
#define CEC_STATUS_REQUEST_OFF      2
#define CEC_STATUS_REQUEST_ONCE     3

// [Deck Info] of <Deck Status>.
#define CEC_DECK_INFO_PLAY          0x11
#define CEC_DECK_INFO_STILL         0x14
#define CEC_DECK_INFO_NO_MEDIA      0x19
#define CEC_DECK_INFO_STOP          0x1A
#define CEC_DECK_INFO_OTHER         0x1F

// [Display Control] of <Set OSD String>.
#define CEC_DISPLAY_DEFAULT_TIME    0x00
#define CEC_DISPLAY_UNTIL_CLEARED   0x40
#define CEC_DISPLAY_CLEAR           0x80
#define CEC_OSD_STRING_MAX          13
#define CEC_OSD_NAME_MAX            14
#define CEC_OSD_NAME_DEFAULT        "Freewili 2"    // the name in a TV's input list

// [UI Command] of <User Control Pressed>: the keys a remote sends to move a
// menu. cecUiCommandName() names the whole table.
#define CEC_UI_SELECT               0x00
#define CEC_UI_UP                   0x01
#define CEC_UI_DOWN                 0x02
#define CEC_UI_LEFT                 0x03
#define CEC_UI_RIGHT                0x04
#define CEC_UI_ROOT_MENU            0x09
#define CEC_UI_EXIT                 0x0D
#define CEC_UI_CHANNEL_UP           0x30
#define CEC_UI_CHANNEL_DOWN         0x31
#define CEC_UI_POWER                0x40
#define CEC_UI_VOLUME_UP            0x41
#define CEC_UI_VOLUME_DOWN          0x42
#define CEC_UI_MUTE                 0x43
#define CEC_UI_PLAY                 0x44
#define CEC_UI_STOP                 0x45
#define CEC_UI_PAUSE                0x46
#define CEC_UI_POWER_TOGGLE         0x6B
#define CEC_UI_POWER_OFF            0x6C
#define CEC_UI_POWER_ON             0x6D
#define CEC_UI_F1_BLUE              0x71
#define CEC_UI_F2_RED               0x72
#define CEC_UI_F3_GREEN             0x73
#define CEC_UI_F4_YELLOW            0x74

// A key with no <User Control Released> counts as released after this long
// without a repeat: the follower safety timeout of CEC 13.13.3.
#define CEC_KEY_RELEASE_MS          550u

// ---- device ----------------------------------------------------------------
// One physical device on the bus: the address claim, the answers every
// device owes, and one behaviour per feature. Pure: it reads no clock and
// sends nothing. The caller hands it received frames, the result of each
// frame it sent, and the time; it hands back frames to send and events.
//
// A device presents up to CEC_ROLES_MAX logical devices (playback, recorder,
// tuner, audio system), each with its own logical address. An answer leaves
// from the address the request was sent to. A recorder does not also take a
// playback address (CEC 10.2.1); the caller's configuration keeps to that.
//
// Claim (CEC 10.2.1): for each role, poll that type's addresses in order. A
// poll nobody acknowledges means the address is free and is taken; when all
// are in use the role stays at address 15. A claimed role broadcasts
// <Report Physical Address>. With no valid physical address nothing is
// claimed. A device at address 15 can still send <Standby> and
// <Image View On>.
//
// A claim that ends with no address, after at least one address was given up
// because its polls never got onto the wire, starts again: CEC_CLAIM_RETRY_MS
// after the next tick, the wait doubling up to CEC_CLAIM_RETRY_MAX_MS, for as
// long as the device runs. Each role reports AddressNone once per start.
#define CEC_ROLES_MAX               2
#define CEC_OUT_FRAMES_MAX          6
#define CEC_OUT_EVENTS_MAX          4
#define CEC_CLAIM_SEND_TRIES        3       // sends of one poll that end Failed before the next address is tried
#define CEC_CLAIM_RETRY_MS          1000u   // first wait before a claim lost to send failures starts again
#define CEC_CLAIM_RETRY_MAX_MS      8000u   // longest wait between claims
#define CEC_VENDOR_ID_NONE          0xFFFFFFFFu
// The vendor ID this product reports: a borrowed IEEE company ID, the one
// libcec-based players report, held until the product has one of its own.
#define CEC_VENDOR_ID_PRODUCT       0x001582u

// Behaviours, one bit each. With a bit clear the device takes no part in that
// traffic: a directly addressed request for it is refused like any
// unsupported request. With CEC_BEH_ABORT clear as well nothing is refused,
// and the caller owns every answer the device does not give.
#define CEC_BEH_SYSTEM_INFO         0x0001  // <Give Physical Address>, <Get CEC Version>, <Set Menu Language>
#define CEC_BEH_OSD_NAME            0x0002  // <Give OSD Name>
#define CEC_BEH_POWER_STATUS        0x0004  // <Give Device Power Status>
#define CEC_BEH_VENDOR_ID           0x0008  // <Give Device Vendor ID>, and <Device Vendor ID> after the claim
#define CEC_BEH_ROUTING             0x0010  // <Set Stream Path>, <Request Active Source>, another source going active
#define CEC_BEH_STANDBY             0x0020  // <Standby>
#define CEC_BEH_REMOTE_KEYS         0x0040  // <User Control Pressed>, <User Control Released>
#define CEC_BEH_MENU                0x0080  // <Menu Request>
#define CEC_BEH_DECK                0x0100  // <Give Deck Status>, <Play>, <Deck Control>
#define CEC_BEH_ABORT               0x0200  // <Feature Abort> for <Abort> and for every request nothing above answered
#define CEC_BEH_ALL                 0x03FF

struct fwCecConfig
{
    uint32_t uiVendorId;                    // 24-bit IEEE company ID, or CEC_VENDOR_ID_NONE
    uint16_t uiBehaviours;                  // CEC_BEH_*
    uint8_t  abtDeviceType[CEC_ROLES_MAX];  // CEC_DEVTYPE_*; CEC_DEVTYPE_NONE = slot unused; a repeated type counts once
    uint8_t  btCecVersion;                  // CEC_VERSION_*
    uint8_t  btOsdNameLen;                  // 0..CEC_OSD_NAME_MAX; 0 = no name
    char     achOsdName[CEC_OSD_NAME_MAX];  // ASCII, not terminated
};
// One playback device, CEC 1.4, every behaviour, no vendor ID, the name
// CEC_OSD_NAME_DEFAULT.
void cecConfigDefault(fwCecConfig & stConfig);
// Copies up to CEC_OSD_NAME_MAX printable ASCII characters of szName.
void cecConfigSetOsdName(fwCecConfig & stConfig, const char * szName);

// Declaration order is the console value.
enum class fwCecState : uint8_t
{
    Off = 0,
    Claiming,       // polling for an address
    Ready,          // the claim is over; a role may still be at address 15
};

// Result of a frame the wire layer finished sending. The wire layer owns
// retransmission (CEC 7.1) and reports once, with the final result.
enum class fwCecTxResult : uint8_t
{
    Acked = 0,      // every block acknowledged (a broadcast: nobody rejected it)
    NotAcked,       // sent, and the destination did not acknowledge
    Failed,         // never got onto the wire intact: arbitration lost or a line error
};

// Declaration order is the console value.
enum class fwCecEventType : uint8_t
{
    AddressClaimed = 0, // btOwn = the logical address taken, abtData[0] = its device type
    AddressNone,        // a role stays at address 15; abtData[0] = its device type (CEC_DEVTYPE_NONE: no physical address)
    Standby,            // <Standby> received
    ActiveSource,       // abtData[0] = 1 this device became the active source, 0 it stopped being it
    KeyDown,            // btPeer = the remote's device, abtData[0] = [UI Command]
    KeyUp,              // as KeyDown
    MenuRequest,        // abtData[0] = [Menu Request Type]
    MenuLanguage,       // abtData[0..2] = the TV's ISO 639-2 language code
    DeckPlay,           // <Play>; abtData[0] = [Play Mode]
    DeckControl,        // <Deck Control>; abtData[0] = [Deck Control Mode]
    FeatureAbort,       // btPeer refused a message of ours; abtData[0] = its opcode, [1] = the reason
};

struct fwCecEvent
{
    fwCecEventType eType;
    uint8_t        btPeer;      // the other device; CEC_ADDR_UNREGISTERED = none
    uint8_t        btOwn;       // this device's address the event concerns; 15 = none, or a broadcast
    uint8_t        abtData[3];
};

// Frames to send, in order, and events. A call appends; the caller clears.
struct fwCecOut
{
    uint8_t    btFrames;
    uint8_t    btEvents;
    bool       bOverflow;                       // something did not fit and was dropped
    fwCecFrame astFrame[CEC_OUT_FRAMES_MAX];
    fwCecEvent astEvent[CEC_OUT_EVENTS_MAX];
};
static inline void cecOutClear(fwCecOut & stOut) { stOut.btFrames = 0; stOut.btEvents = 0; stOut.bOverflow = false; }

struct fwCecDevice
{
    fwCecConfig stConfig;
    uint32_t    uiKeyMs;                    // time of the last press or repeat
    uint32_t    uiClaimRetryMs;             // bClaimTimed: when the claim starts again
    uint16_t    uiPhysAddr;
    uint16_t    uiClaimWaitMs;              // the wait before the next claim restart
    fwCecState  eState;
    uint8_t     abtAddress[CEC_ROLES_MAX];  // logical address of each role; CEC_ADDR_UNREGISTERED = none
    uint8_t     btClaimRole;                // Claiming: the role being claimed
    uint8_t     btClaimIndex;               // Claiming: which of that type's addresses is being polled
    uint8_t     btClaimSends;               // Claiming: sends of the current poll that ended Failed
    uint8_t     btNoneReported;             // bit n set = role n has reported AddressNone since the start
    bool        bClaimFailed;               // Claiming: an address was given up after CEC_CLAIM_SEND_TRIES Failed sends
    bool        bClaimRetry;                // Ready: the claim starts again after a wait
    bool        bClaimTimed;                // bClaimRetry: uiClaimRetryMs is set
    bool        bActiveSource;
    uint8_t     btPowerStatus;              // CEC_POWER_*
    uint8_t     btMenuState;                // CEC_MENU_STATE_*
    uint8_t     btDeckInfo;                 // CEC_DECK_INFO_*
    uint8_t     btDeckStatusTo;             // who asked for deck status on every change; CEC_ADDR_UNREGISTERED = nobody
    bool        bKeyDown;
    uint8_t     btKeyCode;
    uint8_t     btKeyFrom;                  // the remote's device
    uint8_t     btKeyTo;                    // the address of this device the key was sent to
};

void cecDeviceInit(fwCecDevice & stDevice, const fwCecConfig & stConfig);
// Starts the claim for a place in the connection tree. With CEC_PHYS_ADDR_INVALID
// the device goes straight to Ready at address 15.
void cecDeviceStart(fwCecDevice & stDevice, uint16_t uiPhysAddr, fwCecOut & stOut);
// Gives up the addresses. An active source says <Inactive Source> first.
void cecDeviceStop(fwCecDevice & stDevice, fwCecOut & stOut);

// The result of a frame taken from an fwCecOut. Frames from any other source
// (a script's raw send) are not reported here.
void cecDeviceOnTxDone(fwCecDevice & stDevice, const fwCecFrame & stSent, fwCecTxResult eResult, fwCecOut & stOut);
void cecDeviceOnRx(fwCecDevice & stDevice, const fwCecFrame & stRx, uint32_t uiNowMs, fwCecOut & stOut);
void cecDeviceOnTick(fwCecDevice & stDevice, uint32_t uiNowMs, fwCecOut & stOut);

// Bit n set = logical address n is held: what the wire layer acknowledges.
uint16_t cecDeviceAddressMask(const fwCecDevice & stDevice);
// The address frames leave from: the first claimed role, else 15.
uint8_t cecDeviceSourceAddress(const fwCecDevice & stDevice);

// ---- requests --------------------------------------------------------------
// Each returns false, adding nothing, when the device is in no state to send
// that message.
// One Touch Play: <Image View On> (or <Text View On>) to the TV, then
// <Active Source>. At address 15 only the first is sent.
bool cecDeviceOneTouchPlay(fwCecDevice & stDevice, bool bTextView, fwCecOut & stOut);
// <Inactive Source> to the TV, when this device is the active source.
bool cecDeviceInactiveSource(fwCecDevice & stDevice, fwCecOut & stOut);
// <Standby> to one device, or to all with CEC_ADDR_BROADCAST.
bool cecDeviceStandby(fwCecDevice & stDevice, uint8_t btDestination, fwCecOut & stOut);
// <Set OSD String> to the TV: 1..CEC_OSD_STRING_MAX characters.
bool cecDeviceOsdString(fwCecDevice & stDevice, uint8_t btDisplayControl, const char * pText, int iLen, fwCecOut & stOut);
// <User Control Pressed> / <User Control Released> to another device.
bool cecDeviceKeyPress(fwCecDevice & stDevice, uint8_t btDestination, uint8_t btUiCommand, fwCecOut & stOut);
bool cecDeviceKeyRelease(fwCecDevice & stDevice, uint8_t btDestination, fwCecOut & stOut);
// Local state the device reports. A change is announced to whoever asked to
// be told: <Deck Status> to the status-request holder, <Menu Status> to the TV.
void cecDeviceSetPowerStatus(fwCecDevice & stDevice, uint8_t btPowerStatus);
void cecDeviceSetDeckInfo(fwCecDevice & stDevice, uint8_t btDeckInfo, fwCecOut & stOut);
void cecDeviceSetMenuState(fwCecDevice & stDevice, uint8_t btMenuState, fwCecOut & stOut);

// ---- controller --------------------------------------------------------------
// Messages this device sends to ask or tell another device something, and the
// answers to them. A follower answers within CEC_REPLY_MS (CEC 9.2).
#define CEC_REPLY_MS                1000u
#define CEC_REPLY_NONE              (-1)

// The opcode of the message that answers stRequest, or CEC_REPLY_NONE: a
// poll, a message nothing answers, and <Give Deck Status> with "off".
int cecReplyOpcode(const fwCecFrame & stRequest);

// Declaration order is the console value.
enum class fwCecMatch : uint8_t
{
    None = 0,       // not the answer
    Reply,          // the answer
    Refused,        // the peer's <Feature Abort> of the request
};
// stRx against a request of opcode btRequest sent to btPeer, waiting for a
// message of opcode btReply. With btPeer CEC_ADDR_BROADCAST any device may
// answer. A reply has at least the operands its table row needs; with
// btReply <Feature Abort> it is the one that names btRequest.
fwCecMatch cecMatchReply(const fwCecFrame & stRx, uint8_t btPeer, uint8_t btRequest, uint8_t btReply);

// Builders of the messages whose operands have a layout. Each returns false,
// leaving stFrame untouched, for an address above 15, a directly addressed
// message to address 15, or operands out of range.
bool cecBuildRequestActiveSource(fwCecFrame & stFrame, uint8_t btInitiator);
bool cecBuildSetStreamPath(fwCecFrame & stFrame, uint8_t btInitiator, uint16_t uiPhysAddr);
bool cecBuildRoutingChange(fwCecFrame & stFrame, uint8_t btInitiator, uint16_t uiFrom, uint16_t uiTo);
bool cecBuildRoutingInformation(fwCecFrame & stFrame, uint8_t btInitiator, uint16_t uiPhysAddr);
// [UI Command] alone.
bool cecBuildUserControlPressed(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint8_t btUiCommand);
// With CEC_PHYS_ADDR_INVALID the request has no operand: system audio off.
bool cecBuildSystemAudioModeRequest(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint16_t uiPhysAddr);
// 1..CEC_OSD_STRING_MAX printable characters; none with CEC_DISPLAY_CLEAR.
bool cecBuildSetOsdString(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint8_t btDisplayControl,
                          const char * pText, int iLen);
// 1..14 printable characters.
bool cecBuildSetTimerProgramTitle(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination,
                                  const char * pText, int iLen);
// The 24-bit vendor ID, then up to 11 bytes of the vendor's own.
bool cecBuildVendorCommandWithId(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint32_t uiVendorId,
                                 const uint8_t * pData, int iLen);
// <Feature Abort> of btOpcode, with an [Abort Reason].
bool cecBuildFeatureAbortTo(fwCecFrame & stFrame, uint8_t btInitiator, uint8_t btDestination, uint8_t btOpcode,
                            uint8_t btReason);

// Readers of answers. Each returns false for another opcode or too few operands.
bool cecParseReportPhysicalAddress(const fwCecFrame & stFrame, uint16_t & uiPhysAddr, uint8_t & btDeviceType);
bool cecParseDeviceVendorId(const fwCecFrame & stFrame, uint32_t & uiVendorId);
// The name of <Set OSD Name>, terminated, its characters outside printable
// ASCII as '?'. szOut holds at least CEC_OSD_NAME_MAX + 1 characters. Returns
// the length, or -1.
int cecParseOsdName(const fwCecFrame & stFrame, char * szOut);

// ---- bus scan ----------------------------------------------------------------
// Every logical address but the broadcast one is polled. A device that
// acknowledges its poll is asked, in order, for its physical address, vendor
// ID, name, power status and CEC version. A question that is not acknowledged
// ends the questions to that device.
#define CEC_SCAN_ADDRESSES          15
#define CEC_SCAN_Q_PHYS_ADDR        0       // <Give Physical Address>
#define CEC_SCAN_Q_VENDOR_ID        1       // <Give Device Vendor ID>
#define CEC_SCAN_Q_OSD_NAME         2       // <Give OSD Name>
#define CEC_SCAN_Q_POWER            3       // <Give Device Power Status>
#define CEC_SCAN_Q_VERSION          4       // <Get CEC Version>
#define CEC_SCAN_QUESTIONS          5

struct fwCecScanEntry
{
    bool     bPresent;                          // acknowledged its poll
    uint8_t  btAnswers;                         // bit n: question CEC_SCAN_Q_* n answered
    uint16_t uiPhysAddr;                        // CEC_PHYS_ADDR_INVALID unanswered
    uint8_t  btDeviceType;                      // CEC_DEVTYPE_NONE unanswered
    uint8_t  btPowerStatus;                     // 0xFF unanswered
    uint8_t  btCecVersion;                      // 0 unanswered
    uint32_t uiVendorId;                        // CEC_VENDOR_ID_NONE unanswered
    char     szName[CEC_OSD_NAME_MAX + 1];      // "" unanswered
};

struct fwCecScan
{
    uint16_t       uiSkipMask;                  // bit n: address n is not polled
    uint8_t        btAddress;                   // CEC_SCAN_ADDRESSES: the scan is over
    uint8_t        btStep;                      // 0 the poll, 1 + n question n
    fwCecScanEntry astEntry[CEC_SCAN_ADDRESSES];
};

void cecScanStart(fwCecScan & stScan, uint16_t uiSkipMask);
// The next frame to send, from btInitiator. False when the scan is over.
bool cecScanNext(const fwCecScan & stScan, uint8_t btInitiator, fwCecFrame & stFrame);
// The outcome of the frame cecScanNext() gave: acknowledged or not, and its
// answer (nullptr: none came).
void cecScanTake(fwCecScan & stScan, bool bAcked, const fwCecFrame * pReply);
// Bit n set = address n acknowledged its poll.
uint16_t cecScanFoundMask(const fwCecScan & stScan);

// ---- receive queue -------------------------------------------------------------
// Received messages kept for a poller, oldest first, on a caller ring of
// CEC_RX_QUEUE_DEPTH records. Full: the oldest is dropped and counted. A poll
// is not a message and is never kept.
#define CEC_RX_QUEUE_DEPTH          32
// The flags of a received frame, the values of CEC_WIRE_RX_*.
#define CEC_RX_ACKED                0x01    // every block acknowledged; a broadcast: not rejected
#define CEC_RX_FOR_US               0x02    // a broadcast, or addressed to this device

// The messages the queue keeps. Declaration order is the console value.
enum class fwCecRxQueueMode : uint8_t
{
    Off = 0,        // none: the queue has no ring
    Ours,           // addressed to this device, and broadcasts
    All,            // every message received
    Keys,           // <User Control Pressed> and <User Control Released> to this device
};
#define CEC_RX_QUEUE_MODE_LAST      3

struct fwCecRxRecord
{
    uint32_t   uiTimestampUs;               // when it was taken in
    uint8_t    btFlags;                     // CEC_RX_*
    fwCecFrame stFrame;
};

struct fwCecRxQueue
{
    fwCecRxRecord *  pRing;                 // CEC_RX_QUEUE_DEPTH records; nullptr with Off
    fwCecRxQueueMode eMode;
    int              iHead;                 // the oldest record
    int              iCount;
    uint32_t         uiDropped;             // dropped since the reset
};

// Empty, on pRing, in eMode, the drop count zero. Off with pRing nullptr.
void cecRxQueueReset(fwCecRxQueue & stQueue, fwCecRxRecord * pRing, fwCecRxQueueMode eMode);
// eMode keeps stFrame, received with btFlags.
bool cecRxQueueWants(fwCecRxQueueMode eMode, const fwCecFrame & stFrame, uint8_t btFlags);
// stFrame kept when the mode wants it. True when kept.
bool cecRxQueuePush(fwCecRxQueue & stQueue, const fwCecFrame & stFrame, uint8_t btFlags, uint32_t uiTimestampUs);
// Messages lost before they reached the queue, counted as dropped while it has a ring.
void cecRxQueueLost(fwCecRxQueue & stQueue, uint32_t uiLost);
// The oldest record, taken off. False when empty.
bool cecRxQueuePop(fwCecRxQueue & stQueue, fwCecRxRecord & stOut);

// ---- held remote key -----------------------------------------------------------
// A remote key held down, as a keypad button held down: one step at the press,
// then, for a key that repeats, a step after uiDelayMs and every uiRepeatMs,
// every uiFastMs once btAccelAfter repeats have gone by. Pure: the caller
// gives the time and acts on the steps.
struct fwCecKeyTiming
{
    uint32_t uiDelayMs;
    uint32_t uiRepeatMs;
    uint32_t uiFastMs;
    uint8_t  btAccelAfter;
};

struct fwCecKeyHold
{
    bool     bHeld;
    bool     bRepeats;
    uint8_t  btKey;                         // [UI Command]
    uint8_t  btRepeatCount;
    uint32_t uiDueMs;                       // the next repeat step
};

// Not held.
void cecKeyHoldReset(fwCecKeyHold & stHold);
// btKey pressed at uiNowMs; bRepeats: it steps again while held.
void cecKeyHoldPress(fwCecKeyHold & stHold, uint8_t btKey, bool bRepeats, uint32_t uiNowMs,
                     const fwCecKeyTiming & stTiming);
// True when a repeat step is due at uiNowMs; the next one is then timed.
bool cecKeyHoldDue(fwCecKeyHold & stHold, uint32_t uiNowMs, const fwCecKeyTiming & stTiming);
// The key let go. True when it was held: its release is owed.
bool cecKeyHoldRelease(fwCecKeyHold & stHold);

// ---- names -----------------------------------------------------------------
const char * cecOpcodeName(uint8_t btOpcode);           // "active_source", ...; "unknown"
const char * cecFeatureName(fwCecFeature eFeature);     // "one_touch_play", ...
const char * cecCheckName(fwCecCheck eCheck);           // "ok", "poll", ...
const char * cecRxName(fwCecRx eRx);                    // "not_for_us", ...
const char * cecAbortReasonName(uint8_t btReason);      // "unrecognized", ...; "unknown"
const char * cecAddressName(uint8_t btAddress);         // "tv", "playback1", ...; 15 is "unregistered"
const char * cecDeviceTypeName(uint8_t btDeviceType);   // "playback", ...; "unknown"
const char * cecPowerStatusName(uint8_t btPowerStatus); // "on", "standby", ...; "unknown"
const char * cecUiCommandName(uint8_t btUiCommand);     // "select", "up", ...; "unknown"
const char * cecStateName(fwCecState eState);           // "off", "claiming", "ready"
const char * cecEventName(fwCecEventType eType);        // "address_claimed", ...
const char * cecMatchName(fwCecMatch eMatch);           // "none", "reply", "refused"
