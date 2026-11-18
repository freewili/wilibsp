// Host unit tests for rmpLib/fwCECCore -- the pure CEC message core.
// No Pico SDK, no hardware, no clock. The message table is checked for its
// own consistency, every row is built and checked back at its operand limits
// and through each delivery it allows, frames are compared byte for byte
// with hand-built ones, and the follower and <Feature Abort> rules are
// driven case by case, then with seeded-random frames. The device is driven
// through scripted exchanges: the address claim, the standard answers and
// each behaviour, with the frames it sends compared byte for byte.
// Exit code 0 = pass.
#include "cec_test_harness.h"

// A table message with its minimum operands, all zero, sent where the table
// allows: directly to playback 1 when it can be, else broadcast.
static fwCecFrame minimalFrame(uint8_t btOpcode, uint8_t btInitiator)
{
    static const uint8_t abtOperands[CEC_OPERANDS_MAX] = {};
    const fwCecMsgInfo * pInfo = cecMsgFind(btOpcode);
    fwCecFrame stFrame = {};
    if (pInfo != nullptr)
        cecBuild(stFrame, btInitiator,
                 (pInfo->btTo & CEC_TO_DIRECT) != 0 ? CEC_ADDR_PLAYBACK_1 : CEC_ADDR_BROADCAST,
                 btOpcode, abtOperands, pInfo->btOperandsMin);
    return stFrame;
}

static bool isToken(const char * sz)
{
    if (sz == nullptr || sz[0] == 0)
        return false;
    for (const char * p = sz; *p; p++)
    {
        const bool bOk = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_';
        if (!bOk)
            return false;
    }
    return true;
}

static const uint16_t kPlayback1 = 1u << CEC_ADDR_PLAYBACK_1;

// ------------------------------------------------------------------ table

TEST(testTableIntegrity)
{
    const int iCount = cecMsgCount();
    CHECK(iCount > 0);

    for (int i = 0; i < iCount; i++)
    {
        const fwCecMsgInfo & st = cecMsgAt(i);
        if (i > 0)
            CHECK_OP(cecMsgAt(i - 1).btOpcode < st.btOpcode, st.btOpcode);
        CHECK_OP(st.btTo >= CEC_TO_DIRECT && st.btTo <= CEC_TO_EITHER, st.btOpcode);
        CHECK_OP(st.btOperandsMin <= st.btOperandsMax, st.btOpcode);
        CHECK_OP(st.btOperandsMax <= CEC_OPERANDS_MAX, st.btOpcode);
        CHECK_OP((st.btFlags & ~(CEC_MSG_FROM_UNREGISTERED | CEC_MSG_REPORT)) == 0, st.btOpcode);
        CHECK_OP(isToken(st.szName), st.btOpcode);
        CHECK_OP(isToken(cecFeatureName(st.eFeature)), st.btOpcode);
        CHECK_OP(cecMsgFind(st.btOpcode) == &st, st.btOpcode);
        CHECK_OP(strcmp(cecOpcodeName(st.btOpcode), st.szName) == 0, st.btOpcode);

        for (int j = 0; j < i; j++)
            CHECK_OP(strcmp(cecMsgAt(j).szName, st.szName) != 0, st.btOpcode);
    }

    // An opcode with no row has no name.
    for (int iOp = 0; iOp < 256; iOp++)
        if (cecMsgFind((uint8_t)iOp) == nullptr)
            CHECK_OP(strcmp(cecOpcodeName((uint8_t)iOp), "unknown") == 0, iOp);
}

// Rows whose numbers are fixed by the standard and that the behaviours rest on.
TEST(testTableKnownRows)
{
    struct stRow { uint8_t btOpcode, btTo, btMin, btMax; };
    static const stRow kRows[] = {
        { CEC_OP_FEATURE_ABORT,            CEC_TO_DIRECT,    2, 2 },
        { CEC_OP_IMAGE_VIEW_ON,            CEC_TO_DIRECT,    0, 0 },
        { CEC_OP_TEXT_VIEW_ON,             CEC_TO_DIRECT,    0, 0 },
        { CEC_OP_STANDBY,                  CEC_TO_EITHER,    0, 0 },
        { CEC_OP_USER_CONTROL_RELEASED,    CEC_TO_DIRECT,    0, 0 },
        { CEC_OP_GIVE_OSD_NAME,            CEC_TO_DIRECT,    0, 0 },
        { CEC_OP_SET_OSD_NAME,             CEC_TO_DIRECT,    1, 14 },
        { CEC_OP_SET_OSD_STRING,           CEC_TO_DIRECT,    1, 14 },
        { CEC_OP_VENDOR_COMMAND,           CEC_TO_DIRECT,    0, 14 },
        { CEC_OP_VENDOR_COMMAND_WITH_ID,   CEC_TO_EITHER,    3, 14 },
        { CEC_OP_SET_EXTERNAL_TIMER,       CEC_TO_DIRECT,    9, 10 },
        { CEC_OP_USER_CONTROL_PRESSED,     CEC_TO_DIRECT,    1, 5 },
        { CEC_OP_CDC_MESSAGE,              CEC_TO_BROADCAST, 3, 14 },
        { CEC_OP_ROUTING_CHANGE,           CEC_TO_BROADCAST, 4, 4 },
        { CEC_OP_ROUTING_INFORMATION,      CEC_TO_BROADCAST, 2, 2 },
        { CEC_OP_ACTIVE_SOURCE,            CEC_TO_BROADCAST, 2, 2 },
        { CEC_OP_GIVE_PHYSICAL_ADDRESS,    CEC_TO_DIRECT,    0, 0 },
        { CEC_OP_REPORT_PHYSICAL_ADDRESS,  CEC_TO_BROADCAST, 3, 3 },
        { CEC_OP_REQUEST_ACTIVE_SOURCE,    CEC_TO_BROADCAST, 0, 0 },
        { CEC_OP_SET_STREAM_PATH,          CEC_TO_BROADCAST, 2, 2 },
        { CEC_OP_DEVICE_VENDOR_ID,         CEC_TO_BROADCAST, 3, 3 },
        { CEC_OP_GIVE_DEVICE_VENDOR_ID,    CEC_TO_DIRECT,    0, 0 },
        { CEC_OP_MENU_REQUEST,             CEC_TO_DIRECT,    1, 1 },
        { CEC_OP_MENU_STATUS,              CEC_TO_DIRECT,    1, 1 },
        { CEC_OP_GIVE_DEVICE_POWER_STATUS, CEC_TO_DIRECT,    0, 0 },
        { CEC_OP_GET_MENU_LANGUAGE,        CEC_TO_DIRECT,    0, 0 },
        { CEC_OP_SET_MENU_LANGUAGE,        CEC_TO_BROADCAST, 3, 3 },
        { CEC_OP_INACTIVE_SOURCE,          CEC_TO_DIRECT,    2, 2 },
        { CEC_OP_CEC_VERSION,              CEC_TO_DIRECT,    1, 1 },
        { CEC_OP_GET_CEC_VERSION,          CEC_TO_DIRECT,    0, 0 },
        { CEC_OP_ABORT,                    CEC_TO_DIRECT,    0, 0 },
    };

    for (const stRow & stWant : kRows)
    {
        const fwCecMsgInfo * p = cecMsgFind(stWant.btOpcode);
        CHECK_OP(p != nullptr, stWant.btOpcode);
        if (p == nullptr)
            continue;
        CHECK_OP(p->btTo == stWant.btTo, stWant.btOpcode);
        CHECK_OP(p->btOperandsMin == stWant.btMin, stWant.btOpcode);
        CHECK_OP(p->btOperandsMax == stWant.btMax, stWant.btOpcode);
    }
}

// Every row, through each delivery it allows, at its operand limits.
TEST(testEveryRowBuildsAndChecks)
{
    uint8_t abtOperands[CEC_OPERANDS_MAX];
    for (int i = 0; i < CEC_OPERANDS_MAX; i++)
        abtOperands[i] = (uint8_t)(0xA0 + i);

    for (int i = 0; i < cecMsgCount(); i++)
    {
        const fwCecMsgInfo & st = cecMsgAt(i);

        for (int iDelivery = 0; iDelivery < 2; iDelivery++)
        {
            const bool    bBroadcast = (iDelivery == 1);
            const uint8_t btDest     = bBroadcast ? CEC_ADDR_BROADCAST : CEC_ADDR_TV;
            const bool    bAllowed   = (st.btTo & (bBroadcast ? CEC_TO_BROADCAST : CEC_TO_DIRECT)) != 0;

            for (int iCount : { (int)st.btOperandsMin, (int)st.btOperandsMax })
            {
                fwCecFrame stFrame = {};
                CHECK_OP(cecBuild(stFrame, CEC_ADDR_PLAYBACK_1, btDest, st.btOpcode, abtOperands, iCount), st.btOpcode);
                CHECK_OP(stFrame.btLen == 2 + iCount, st.btOpcode);
                CHECK_OP(cecInitiator(stFrame) == CEC_ADDR_PLAYBACK_1, st.btOpcode);
                CHECK_OP(cecDestination(stFrame) == btDest, st.btOpcode);
                CHECK_OP(cecOpcode(stFrame) == st.btOpcode, st.btOpcode);
                CHECK_OP(cecOperandCount(stFrame) == iCount, st.btOpcode);
                CHECK_OP(memcmp(cecOperands(stFrame), abtOperands, (size_t)iCount) == 0, st.btOpcode);
                CHECK_OP(cecCheck(stFrame) == (bAllowed ? fwCecCheck::Ok : fwCecCheck::WrongDelivery), st.btOpcode);
            }

            if (st.btOperandsMin > 0 && bAllowed)
            {
                fwCecFrame stShort = {};
                CHECK_OP(cecBuild(stShort, CEC_ADDR_PLAYBACK_1, btDest, st.btOpcode, abtOperands, st.btOperandsMin - 1),
                         st.btOpcode);
                CHECK_OP(cecCheck(stShort) == fwCecCheck::TooShort, st.btOpcode);
            }

            // Operands past the maximum are ignored, not an error.
            if (st.btOperandsMax < CEC_OPERANDS_MAX && bAllowed)
            {
                fwCecFrame stLong = {};
                CHECK_OP(cecBuild(stLong, CEC_ADDR_PLAYBACK_1, btDest, st.btOpcode, abtOperands, st.btOperandsMax + 1),
                         st.btOpcode);
                CHECK_OP(cecCheck(stLong) == fwCecCheck::Ok, st.btOpcode);
            }
        }
    }
}

// ------------------------------------------------------------------ build

TEST(testHandBuiltFrames)
{
    fwCecFrame stFrame = {};

    // <Image View On>, playback 1 to the TV.
    CHECK(cecBuild(stFrame, CEC_ADDR_PLAYBACK_1, CEC_ADDR_TV, CEC_OP_IMAGE_VIEW_ON));
    CHECK(sameFrame(stFrame, FRAME(0x40, 0x04)));

    // <Active Source> 1.0.0.0, broadcast.
    uint8_t abtPhys[2];
    cecPutPhysAddr(abtPhys, 0x1000);
    CHECK(cecBuild(stFrame, CEC_ADDR_PLAYBACK_1, CEC_ADDR_BROADCAST, CEC_OP_ACTIVE_SOURCE, abtPhys, 2));
    CHECK(sameFrame(stFrame, FRAME(0x4F, 0x82, 0x10, 0x00)));
    CHECK_EQ(cecGetPhysAddr(cecOperands(stFrame)), 0x1000);

    // <Standby>, broadcast.
    CHECK(cecBuild(stFrame, CEC_ADDR_PLAYBACK_1, CEC_ADDR_BROADCAST, CEC_OP_STANDBY));
    CHECK(sameFrame(stFrame, FRAME(0x4F, 0x36)));

    // <Report Physical Address> 2.1.0.0, device type 4 (playback).
    const uint8_t abtReport[3] = { 0x21, 0x00, 0x04 };
    CHECK(cecBuild(stFrame, CEC_ADDR_PLAYBACK_1, CEC_ADDR_BROADCAST, CEC_OP_REPORT_PHYSICAL_ADDRESS, abtReport, 3));
    CHECK(sameFrame(stFrame, FRAME(0x4F, 0x84, 0x21, 0x00, 0x04)));

    // <Set OSD Name> "FreeWili".
    const uint8_t abtName[8] = { 'F', 'r', 'e', 'e', 'W', 'i', 'l', 'i' };
    CHECK(cecBuild(stFrame, CEC_ADDR_PLAYBACK_1, CEC_ADDR_TV, CEC_OP_SET_OSD_NAME, abtName, 8));
    CHECK(sameFrame(stFrame, FRAME(0x40, 0x47, 0x46, 0x72, 0x65, 0x65, 0x57, 0x69, 0x6C, 0x69)));

    // <User Control Pressed> Select, TV to playback 1.
    const uint8_t btSelect = 0x00;
    CHECK(cecBuild(stFrame, CEC_ADDR_TV, CEC_ADDR_PLAYBACK_1, CEC_OP_USER_CONTROL_PRESSED, &btSelect, 1));
    CHECK(sameFrame(stFrame, FRAME(0x04, 0x44, 0x00)));

    // A poll of playback 1 by itself: the address-claim frame.
    CHECK(cecBuildPoll(stFrame, CEC_ADDR_PLAYBACK_1, CEC_ADDR_PLAYBACK_1));
    CHECK(sameFrame(stFrame, FRAME(0x44)));
    CHECK(cecIsPoll(stFrame));
}

TEST(testBuildLimits)
{
    const uint8_t abtOperands[CEC_OPERANDS_MAX + 1] = {};
    const fwCecFrame stBefore = FRAME(0x40, 0x04);
    fwCecFrame stFrame = stBefore;

    CHECK(!cecBuild(stFrame, 16, CEC_ADDR_TV, CEC_OP_STANDBY));
    CHECK(!cecBuild(stFrame, CEC_ADDR_TV, 16, CEC_OP_STANDBY));
    CHECK(!cecBuild(stFrame, CEC_ADDR_TV, CEC_ADDR_PLAYBACK_1, CEC_OP_VENDOR_COMMAND, abtOperands, CEC_OPERANDS_MAX + 1));
    CHECK(!cecBuild(stFrame, CEC_ADDR_TV, CEC_ADDR_PLAYBACK_1, CEC_OP_VENDOR_COMMAND, abtOperands, -1));
    CHECK(!cecBuild(stFrame, CEC_ADDR_TV, CEC_ADDR_PLAYBACK_1, CEC_OP_VENDOR_COMMAND, nullptr, 1));
    CHECK(!cecBuildPoll(stFrame, 16, CEC_ADDR_TV));
    CHECK(!cecBuildPoll(stFrame, CEC_ADDR_TV, 16));
    CHECK(sameFrame(stFrame, stBefore));        // every refusal left it untouched

    // The longest frame: 14 operands, 16 blocks.
    CHECK(cecBuild(stFrame, CEC_ADDR_TV, CEC_ADDR_PLAYBACK_1, CEC_OP_VENDOR_COMMAND, abtOperands, CEC_OPERANDS_MAX));
    CHECK_EQ(stFrame.btLen, CEC_FRAME_MAX);

    // An opcode outside the table still builds: the raw path.
    CHECK(cecMsgFind(0x01) == nullptr);
    CHECK(cecBuild(stFrame, CEC_ADDR_TV, CEC_ADDR_PLAYBACK_1, 0x01));
    CHECK(sameFrame(stFrame, FRAME(0x04, 0x01)));
}

// ------------------------------------------------------------------ check

TEST(testCheck)
{
    fwCecFrame stFrame = FRAME(0x40, 0x04);
    CHECK_RES(cecCheck(stFrame), fwCecCheck::Ok);

    stFrame.btLen = 0;
    CHECK_RES(cecCheck(stFrame), fwCecCheck::BadLength);
    stFrame.btLen = CEC_FRAME_MAX + 1;
    CHECK_RES(cecCheck(stFrame), fwCecCheck::BadLength);

    CHECK_RES(cecCheck(FRAME(0x44)), fwCecCheck::Poll);
    CHECK_RES(cecCheck(FRAME(0x4F)), fwCecCheck::Poll);
    CHECK_RES(cecCheck(FRAME(0x40, 0x01)), fwCecCheck::UnknownOpcode);
    CHECK_RES(cecCheck(FRAME(0x4F, 0x01)), fwCecCheck::UnknownOpcode);

    CHECK_RES(cecCheck(FRAME(0x4F, 0x82, 0x10)), fwCecCheck::TooShort);             // <Active Source>, one byte short
    CHECK_RES(cecCheck(FRAME(0x40, 0x82, 0x10, 0x00)), fwCecCheck::WrongDelivery);  // <Active Source> sent directly
    CHECK_RES(cecCheck(FRAME(0x4F, 0x04)), fwCecCheck::WrongDelivery);              // <Image View On> broadcast
    CHECK_RES(cecCheck(FRAME(0x40, 0x36)), fwCecCheck::Ok);                         // <Standby> either way
    CHECK_RES(cecCheck(FRAME(0x4F, 0x36)), fwCecCheck::Ok);
    CHECK_RES(cecCheck(FRAME(0x4F, 0x36, 0x55)), fwCecCheck::Ok);                   // a surplus operand is ignored

    CHECK(isToken(cecCheckName(fwCecCheck::Ok)));
    CHECK(isToken(cecCheckName(fwCecCheck::WrongDelivery)));
}

// ------------------------------------------------------------------ follower

TEST(testClassifyRx)
{
    // Directly addressed.
    CHECK_RES(cecClassifyRx(FRAME(0x04, 0x8F), kPlayback1), fwCecRx::Handle);       // <Give Device Power Status> to us
    CHECK_RES(cecClassifyRx(FRAME(0x08, 0x8F), kPlayback1), fwCecRx::NotForUs);     // to playback 2
    CHECK_RES(cecClassifyRx(FRAME(0x04, 0x01), kPlayback1), fwCecRx::Unsupported);  // not a table message
    CHECK_RES(cecClassifyRx(FRAME(0x04, 0x47), kPlayback1), fwCecRx::Ignore);       // <Set OSD Name> with no name
    CHECK_RES(cecClassifyRx(FRAME(0x04, 0x82, 0x10, 0x00), kPlayback1), fwCecRx::Ignore);   // broadcast-only, sent directly

    // Broadcast: for every device.
    CHECK_RES(cecClassifyRx(FRAME(0x0F, 0x36), kPlayback1), fwCecRx::Handle);       // <Standby>
    CHECK_RES(cecClassifyRx(FRAME(0x0F, 0x86, 0x10, 0x00), kPlayback1), fwCecRx::Handle);   // <Set Stream Path>
    CHECK_RES(cecClassifyRx(FRAME(0x0F, 0x01), kPlayback1), fwCecRx::Ignore);       // not a table message: no answer
    CHECK_RES(cecClassifyRx(FRAME(0x0F, 0x04), kPlayback1), fwCecRx::Ignore);       // directed-only, broadcast
    CHECK_RES(cecClassifyRx(FRAME(0x0F, 0x86, 0x10), kPlayback1), fwCecRx::Ignore); // too short

    // Polls.
    CHECK_RES(cecClassifyRx(FRAME(0x04), kPlayback1), fwCecRx::Poll);
    CHECK_RES(cecClassifyRx(FRAME(0x44), kPlayback1), fwCecRx::Poll);               // another device claiming our address
    CHECK_RES(cecClassifyRx(FRAME(0x08), kPlayback1), fwCecRx::NotForUs);
    CHECK_RES(cecClassifyRx(FRAME(0x0F), kPlayback1), fwCecRx::Ignore);

    // An unregistered initiator.
    CHECK_RES(cecClassifyRx(FRAME(0xFF, 0x36), kPlayback1), fwCecRx::Handle);       // <Standby>, broadcast
    CHECK_RES(cecClassifyRx(FRAME(0xF4, 0x36), kPlayback1), fwCecRx::Handle);       // <Standby>, direct
    CHECK_RES(cecClassifyRx(FRAME(0xF4, 0x83), kPlayback1), fwCecRx::Handle);       // <Give Physical Address>: broadcast answer
    CHECK_RES(cecClassifyRx(FRAME(0xFF, 0x85), kPlayback1), fwCecRx::Handle);       // <Request Active Source>
    CHECK_RES(cecClassifyRx(FRAME(0xFF, 0x82, 0x10, 0x00), kPlayback1), fwCecRx::Handle);   // <Active Source>
    CHECK_RES(cecClassifyRx(FRAME(0xFF, 0x87, 0x00, 0x00, 0xF0), kPlayback1), fwCecRx::Ignore); // <Device Vendor ID>
    CHECK_RES(cecClassifyRx(FRAME(0xF4, 0x8F), kPlayback1), fwCecRx::Ignore);       // <Give Device Power Status>
    CHECK_RES(cecClassifyRx(FRAME(0xF4, 0x01), kPlayback1), fwCecRx::Ignore);       // nothing to answer to
    CHECK_RES(cecClassifyRx(FRAME(0xF4), kPlayback1), fwCecRx::Poll);

    // Malformed.
    fwCecFrame stBad = FRAME(0x04, 0x8F);
    stBad.btLen = 0;
    CHECK_RES(cecClassifyRx(stBad, kPlayback1), fwCecRx::Ignore);
    stBad.btLen = CEC_FRAME_MAX + 1;
    CHECK_RES(cecClassifyRx(stBad, kPlayback1), fwCecRx::Ignore);

    // No address held: only broadcasts are ours.
    CHECK_RES(cecClassifyRx(FRAME(0x04, 0x8F), 0), fwCecRx::NotForUs);
    CHECK_RES(cecClassifyRx(FRAME(0x0F, 0x36), 0), fwCecRx::Handle);

    // Two addresses held.
    const uint16_t uiTwo = kPlayback1 | (1u << CEC_ADDR_RECORDER_1);
    CHECK_RES(cecClassifyRx(FRAME(0x01, 0x8F), uiTwo), fwCecRx::Handle);
    CHECK_RES(cecClassifyRx(FRAME(0x04, 0x8F), uiTwo), fwCecRx::Handle);
    CHECK_RES(cecClassifyRx(FRAME(0x02, 0x8F), uiTwo), fwCecRx::NotForUs);

    // From an unregistered initiator (CEC 12.2) a follower acts on <Standby>,
    // <Active Source>, <Report Physical Address>, a switch's routing messages,
    // CDC, and the requests whose answer is a broadcast. The rest is dropped.
    static const uint8_t kFromUnregistered[] = {
        CEC_OP_STANDBY, CEC_OP_ACTIVE_SOURCE, CEC_OP_REPORT_PHYSICAL_ADDRESS,
        CEC_OP_ROUTING_CHANGE, CEC_OP_ROUTING_INFORMATION, CEC_OP_CDC_MESSAGE,
        CEC_OP_GIVE_PHYSICAL_ADDRESS, CEC_OP_GET_MENU_LANGUAGE, CEC_OP_GIVE_DEVICE_VENDOR_ID,
        CEC_OP_REQUEST_ACTIVE_SOURCE, CEC_OP_SET_STREAM_PATH,
    };
    for (uint8_t btOpcode : kFromUnregistered)
        CHECK_OP(cecClassifyRx(minimalFrame(btOpcode, CEC_ADDR_UNREGISTERED), kPlayback1) == fwCecRx::Handle,
                 btOpcode);
    static const uint8_t kNotFromUnregistered[] = {
        CEC_OP_GIVE_DEVICE_POWER_STATUS, CEC_OP_IMAGE_VIEW_ON, CEC_OP_GIVE_OSD_NAME,
        CEC_OP_USER_CONTROL_PRESSED, CEC_OP_ABORT, CEC_OP_DEVICE_VENDOR_ID,
    };
    for (uint8_t btOpcode : kNotFromUnregistered)
    {
        CHECK_OP(cecClassifyRx(minimalFrame(btOpcode, CEC_ADDR_UNREGISTERED), kPlayback1) == fwCecRx::Ignore,
                 btOpcode);
        CHECK_OP(cecClassifyRx(minimalFrame(btOpcode, CEC_ADDR_TV), kPlayback1) == fwCecRx::Handle, btOpcode);
    }

    CHECK(isToken(cecRxName(fwCecRx::NotForUs)));
    CHECK(isToken(cecRxName(fwCecRx::Unsupported)));
}

TEST(testFeatureAbort)
{
    fwCecFrame stReply = {};

    // <Get Menu Language> from the TV to playback 1, not supported.
    CHECK(cecBuildFeatureAbort(stReply, FRAME(0x04, 0x91), CEC_ABORT_UNRECOGNIZED));
    CHECK(sameFrame(stReply, FRAME(0x40, 0x00, 0x91, 0x00)));
    CHECK_RES(cecCheck(stReply), fwCecCheck::Ok);

    // <Abort> is answered the same way, with any reason.
    CHECK(cecBuildFeatureAbort(stReply, FRAME(0x04, 0xFF), CEC_ABORT_REFUSED));
    CHECK(sameFrame(stReply, FRAME(0x40, 0x00, 0xFF, 0x04)));

    // No answer is allowed to these.
    const fwCecFrame stBefore = stReply;
    CHECK(!cecBuildFeatureAbort(stReply, FRAME(0x0F, 0x36), CEC_ABORT_UNRECOGNIZED));           // a broadcast
    CHECK(!cecBuildFeatureAbort(stReply, FRAME(0x04), CEC_ABORT_UNRECOGNIZED));                 // a poll
    CHECK(!cecBuildFeatureAbort(stReply, FRAME(0x04, 0x00, 0x91, 0x00), CEC_ABORT_UNRECOGNIZED));   // <Feature Abort>
    CHECK(!cecBuildFeatureAbort(stReply, FRAME(0xF4, 0x8F), CEC_ABORT_UNRECOGNIZED));           // unregistered initiator
    fwCecFrame stBad = FRAME(0x04, 0x91);
    stBad.btLen = 0;
    CHECK(!cecBuildFeatureAbort(stReply, stBad, CEC_ABORT_UNRECOGNIZED));
    CHECK(sameFrame(stReply, stBefore));

    for (int iReason = CEC_ABORT_UNRECOGNIZED; iReason <= CEC_ABORT_UNDETERMINED; iReason++)
    {
        CHECK(isToken(cecAbortReasonName((uint8_t)iReason)));
        CHECK(strcmp(cecAbortReasonName((uint8_t)iReason), "unknown") != 0);
    }
    CHECK(strcmp(cecAbortReasonName(6), "unknown") == 0);
}

// ------------------------------------------------------------------ names

TEST(testNames)
{
    CHECK(strcmp(cecOpcodeName(CEC_OP_ACTIVE_SOURCE), "active_source") == 0);
    CHECK(strcmp(cecOpcodeName(CEC_OP_GIVE_DEVICE_POWER_STATUS), "give_device_power_status") == 0);

    for (int iAddr = 0; iAddr < 16; iAddr++)
    {
        CHECK(isToken(cecAddressName((uint8_t)iAddr)));
        for (int j = 0; j < iAddr; j++)
            CHECK(strcmp(cecAddressName((uint8_t)iAddr), cecAddressName((uint8_t)j)) != 0);
    }
    CHECK(strcmp(cecAddressName(CEC_ADDR_TV), "tv") == 0);
    CHECK(strcmp(cecAddressName(CEC_ADDR_PLAYBACK_1), "playback1") == 0);
    CHECK(strcmp(cecAddressName(CEC_ADDR_UNREGISTERED), "unregistered") == 0);
    CHECK(strcmp(cecAddressName(16), "unknown") == 0);

    for (int iFeature = 0; iFeature <= (int)fwCecFeature::CapabilityDiscovery; iFeature++)
        CHECK(isToken(cecFeatureName((fwCecFeature)iFeature)));
}

// ------------------------------------------------------------------ fuzz

// Random frames: a frame for another device is left alone, and one that is
// refused can always be refused back to its sender.
TEST(testFuzzFollower)
{
    rngSeed(0xCEC0CEC0u);
    for (int iRound = 0; iRound < 200000; iRound++)
    {
        const fwCecFrame stFrame = randomFrame();
        const uint16_t   uiMask  = (uint16_t)(rng() & 0x7FFF);
        const fwCecRx    eRx     = cecClassifyRx(stFrame, uiMask);
        const bool       bOurs   = cecIsBroadcast(stFrame) || ((uiMask >> cecDestination(stFrame)) & 1u) != 0;

        CHECK((eRx == fwCecRx::NotForUs) == !bOurs);
        if (eRx == fwCecRx::Unsupported)
        {
            fwCecFrame stReply = {};
            CHECK(cecBuildFeatureAbort(stReply, stFrame, CEC_ABORT_UNRECOGNIZED));
            CHECK_RES(cecCheck(stReply), fwCecCheck::Ok);
            CHECK_EQ(cecDestination(stReply), cecInitiator(stFrame));
            CHECK_EQ(cecInitiator(stReply), cecDestination(stFrame));
        }
    }
}

// ------------------------------------------------------------------ device

static const uint16_t kPhys = 0x1000;       // 1.0.0.0

static bool outHasFrame(const fwCecOut & stOut, const fwCecFrame & stWant)
{
    for (int i = 0; i < stOut.btFrames; i++)
        if (sameFrame(stOut.astFrame[i], stWant))
            return true;
    return false;
}

static const fwCecEvent * outFindEvent(const fwCecOut & stOut, fwCecEventType eType)
{
    for (int i = 0; i < stOut.btEvents; i++)
        if (stOut.astEvent[i].eType == eType)
            return &stOut.astEvent[i];
    return nullptr;
}

// A device and the output of the last call made on it.
struct Rig
{
    fwCecDevice stDevice;
    fwCecOut    stOut;

    explicit Rig(const fwCecConfig * pConfig = nullptr)
    {
        fwCecConfig stConfig;
        if (pConfig != nullptr)
            stConfig = *pConfig;
        else
            cecConfigDefault(stConfig);
        cecDeviceInit(stDevice, stConfig);
        cecOutClear(stOut);
    }

    void start(uint16_t uiPhysAddr = kPhys)
    {
        cecOutClear(stOut);
        cecDeviceStart(stDevice, uiPhysAddr, stOut);
    }
    // The wire layer's verdict on the poll the device asked for last.
    void pollResult(fwCecTxResult eResult)
    {
        fwCecFrame stPoll = {};
        for (int i = 0; i < stOut.btFrames; i++)
            if (cecIsPoll(stOut.astFrame[i]))
                stPoll = stOut.astFrame[i];
        cecOutClear(stOut);
        cecDeviceOnTxDone(stDevice, stPoll, eResult, stOut);
    }
    // Started, with the first address of every role free.
    void ready(uint16_t uiPhysAddr = kPhys)
    {
        start(uiPhysAddr);
        for (int i = 0; i < 8 && stDevice.eState == fwCecState::Claiming; i++)
            pollResult(fwCecTxResult::NotAcked);
        cecOutClear(stOut);
    }
    void rx(const fwCecFrame & stFrame, uint32_t uiNowMs = 0)
    {
        cecOutClear(stOut);
        cecDeviceOnRx(stDevice, stFrame, uiNowMs, stOut);
    }
    void tick(uint32_t uiNowMs)
    {
        cecOutClear(stOut);
        cecDeviceOnTick(stDevice, uiNowMs, stOut);
    }
    // The output, emptied, to hand to a call.
    fwCecOut & fresh()
    {
        cecOutClear(stOut);
        return stOut;
    }
    // Only the active source answers <Request Active Source>.
    bool isActiveSource()
    {
        rx(FRAME(0x0F, 0x85));
        return stOut.btFrames == 1;
    }
    bool silent() const { return stOut.btFrames == 0 && stOut.btEvents == 0 && !stOut.bOverflow; }
};

TEST(testConfigDefault)
{
    fwCecConfig stConfig;
    cecConfigDefault(stConfig);
    CHECK_EQ(stConfig.abtDeviceType[0], CEC_DEVTYPE_PLAYBACK);
    CHECK_EQ(stConfig.abtDeviceType[1], CEC_DEVTYPE_NONE);
    CHECK_EQ(stConfig.btCecVersion, CEC_VERSION_1_4);
    CHECK_EQ(stConfig.uiBehaviours, CEC_BEH_ALL);
    CHECK(stConfig.uiVendorId == CEC_VENDOR_ID_NONE);
    CHECK_EQ(stConfig.btOsdNameLen, 10);
    CHECK(memcmp(stConfig.achOsdName, "Freewili 2", 10) == 0);

    cecConfigSetOsdName(stConfig, "FreeWili");
    CHECK_EQ(stConfig.btOsdNameLen, 8);
    CHECK(memcmp(stConfig.achOsdName, "FreeWili", 8) == 0);
    cecConfigSetOsdName(stConfig, "A name of twenty char");     // cut at 14
    CHECK_EQ(stConfig.btOsdNameLen, CEC_OSD_NAME_MAX);
    CHECK(memcmp(stConfig.achOsdName, "A name of twen", 14) == 0);
    cecConfigSetOsdName(stConfig, "a\tb\x7F" "c");                 // not printable: left out
    CHECK_EQ(stConfig.btOsdNameLen, 3);
    CHECK(memcmp(stConfig.achOsdName, "abc", 3) == 0);
    cecConfigSetOsdName(stConfig, nullptr);
    CHECK_EQ(stConfig.btOsdNameLen, 0);
}

TEST(testClaimFirstFree)
{
    Rig obRig;
    CHECK_RES(obRig.stDevice.eState, fwCecState::Off);
    CHECK_EQ(cecDeviceAddressMask(obRig.stDevice), 0);

    obRig.start();
    CHECK_RES(obRig.stDevice.eState, fwCecState::Claiming);
    CHECK_EQ(obRig.stOut.btFrames, 1);
    CHECK(outHasFrame(obRig.stOut, FRAME(0x44)));               // poll of playback 1 by itself
    CHECK_EQ(obRig.stOut.btEvents, 0);
    CHECK_EQ(cecDeviceAddressMask(obRig.stDevice), 0);          // nothing is acknowledged while claiming

    obRig.pollResult(fwCecTxResult::NotAcked);                  // nobody there: the address is free
    CHECK_RES(obRig.stDevice.eState, fwCecState::Ready);
    CHECK_EQ(cecDeviceAddressMask(obRig.stDevice), kPlayback1);
    CHECK_EQ(cecDeviceSourceAddress(obRig.stDevice), CEC_ADDR_PLAYBACK_1);
    CHECK_EQ(obRig.stOut.btFrames, 1);
    CHECK(outHasFrame(obRig.stOut, FRAME(0x4F, 0x84, 0x10, 0x00, 0x04)));   // <Report Physical Address>
    const fwCecEvent * pEvent = outFindEvent(obRig.stOut, fwCecEventType::AddressClaimed);
    CHECK(pEvent != nullptr && pEvent->btOwn == CEC_ADDR_PLAYBACK_1 && pEvent->abtData[0] == CEC_DEVTYPE_PLAYBACK);
}

TEST(testClaimSkipsAddressesInUse)
{
    Rig obRig;
    obRig.start();
    obRig.pollResult(fwCecTxResult::Acked);                     // playback 1 is taken
    CHECK(outHasFrame(obRig.stOut, FRAME(0x88)));
    CHECK_EQ(obRig.stOut.btEvents, 0);
    obRig.pollResult(fwCecTxResult::Acked);                     // playback 2 is taken
    CHECK(outHasFrame(obRig.stOut, FRAME(0xBB)));
    obRig.pollResult(fwCecTxResult::NotAcked);
    CHECK_RES(obRig.stDevice.eState, fwCecState::Ready);
    CHECK_EQ(cecDeviceSourceAddress(obRig.stDevice), CEC_ADDR_PLAYBACK_3);
    CHECK(outHasFrame(obRig.stOut, FRAME(0xBF, 0x84, 0x10, 0x00, 0x04)));
}

TEST(testClaimAllInUse)
{
    Rig obRig;
    obRig.start();
    obRig.pollResult(fwCecTxResult::Acked);
    obRig.pollResult(fwCecTxResult::Acked);
    obRig.pollResult(fwCecTxResult::Acked);
    CHECK_RES(obRig.stDevice.eState, fwCecState::Ready);
    CHECK_EQ(obRig.stOut.btFrames, 0);
    CHECK_EQ(cecDeviceAddressMask(obRig.stDevice), 0);
    CHECK_EQ(cecDeviceSourceAddress(obRig.stDevice), CEC_ADDR_UNREGISTERED);
    const fwCecEvent * pEvent = outFindEvent(obRig.stOut, fwCecEventType::AddressNone);
    CHECK(pEvent != nullptr && pEvent->btOwn == CEC_ADDR_UNREGISTERED && pEvent->abtData[0] == CEC_DEVTYPE_PLAYBACK);
}

TEST(testClaimRetriesAFailedSend)
{
    Rig obRig;
    obRig.start();
    for (int i = 1; i < CEC_CLAIM_SEND_TRIES; i++)
    {
        obRig.pollResult(fwCecTxResult::Failed);
        CHECK(outHasFrame(obRig.stOut, FRAME(0x44)));           // the same address again
        CHECK_EQ(obRig.stOut.btEvents, 0);
    }
    obRig.pollResult(fwCecTxResult::Failed);                    // given up on: the next address
    CHECK(outHasFrame(obRig.stOut, FRAME(0x88)));
    obRig.pollResult(fwCecTxResult::NotAcked);
    CHECK_EQ(cecDeviceSourceAddress(obRig.stDevice), CEC_ADDR_PLAYBACK_2);
}

// Every poll of every playback address ends Failed: the claim ends at 15.
static void claimAllFailed(Rig & obRig)
{
    for (int i = 0; i < 3 * CEC_CLAIM_SEND_TRIES; i++)
        obRig.pollResult(fwCecTxResult::Failed);
}

TEST(testClaimRestartsAfterSendFailures)
{
    Rig obRig;
    obRig.start();
    claimAllFailed(obRig);
    CHECK_RES(obRig.stDevice.eState, fwCecState::Ready);
    CHECK_EQ(cecDeviceSourceAddress(obRig.stDevice), CEC_ADDR_UNREGISTERED);
    CHECK(outFindEvent(obRig.stOut, fwCecEventType::AddressNone) != nullptr);

    // The wait starts at the first tick: 1 s, then 2, 4, 8, 8.
    const uint32_t auiWaitMs[] = { 1000, 2000, 4000, 8000, 8000 };
    uint32_t uiNowMs = 5000;
    for (uint32_t uiWaitMs : auiWaitMs)
    {
        obRig.tick(uiNowMs);
        CHECK(obRig.silent());
        obRig.tick(uiNowMs + uiWaitMs - 1);
        CHECK(obRig.silent());
        obRig.tick(uiNowMs + uiWaitMs);
        CHECK_RES(obRig.stDevice.eState, fwCecState::Claiming);
        CHECK(outHasFrame(obRig.stOut, FRAME(0x44)));           // from playback 1 again
        claimAllFailed(obRig);
        CHECK_RES(obRig.stDevice.eState, fwCecState::Ready);
        CHECK(outFindEvent(obRig.stOut, fwCecEventType::AddressNone) == nullptr);  // reported once per start
        uiNowMs += uiWaitMs + 10;
    }

    // The TV goes quiet: the next claim takes playback 1.
    obRig.tick(uiNowMs);
    obRig.tick(uiNowMs + CEC_CLAIM_RETRY_MAX_MS);
    CHECK_RES(obRig.stDevice.eState, fwCecState::Claiming);
    obRig.pollResult(fwCecTxResult::NotAcked);
    CHECK_RES(obRig.stDevice.eState, fwCecState::Ready);
    CHECK_EQ(cecDeviceSourceAddress(obRig.stDevice), CEC_ADDR_PLAYBACK_1);
    CHECK(outFindEvent(obRig.stOut, fwCecEventType::AddressClaimed) != nullptr);
    obRig.tick(uiNowMs + 4 * CEC_CLAIM_RETRY_MAX_MS);
    CHECK(obRig.silent());

    // A new start reports AddressNone again and waits 1 s again.
    obRig.start();
    claimAllFailed(obRig);
    CHECK(outFindEvent(obRig.stOut, fwCecEventType::AddressNone) != nullptr);
    obRig.tick(100);
    obRig.tick(100 + CEC_CLAIM_RETRY_MS);
    CHECK_RES(obRig.stDevice.eState, fwCecState::Claiming);

    // Stopped while waiting: nothing restarts.
    claimAllFailed(obRig);
    obRig.tick(200);
    cecDeviceStop(obRig.stDevice, obRig.fresh());
    obRig.tick(200 + CEC_CLAIM_RETRY_MAX_MS);
    CHECK(obRig.silent());
    CHECK_RES(obRig.stDevice.eState, fwCecState::Off);
}

TEST(testClaimAllInUseDoesNotRestart)
{
    Rig obRig;
    obRig.start();
    obRig.pollResult(fwCecTxResult::Acked);
    obRig.pollResult(fwCecTxResult::Acked);
    obRig.pollResult(fwCecTxResult::Acked);
    CHECK_RES(obRig.stDevice.eState, fwCecState::Ready);
    obRig.tick(0);
    obRig.tick(10 * CEC_CLAIM_RETRY_MAX_MS);
    CHECK(obRig.silent());
    CHECK_RES(obRig.stDevice.eState, fwCecState::Ready);

    // One address given up to failures, the others in use: the claim starts again.
    Rig obMixed;
    obMixed.start();
    for (int i = 0; i < CEC_CLAIM_SEND_TRIES; i++)
        obMixed.pollResult(fwCecTxResult::Failed);
    obMixed.pollResult(fwCecTxResult::Acked);
    obMixed.pollResult(fwCecTxResult::Acked);
    CHECK_EQ(cecDeviceSourceAddress(obMixed.stDevice), CEC_ADDR_UNREGISTERED);
    obMixed.tick(0);
    obMixed.tick(CEC_CLAIM_RETRY_MS);
    CHECK_RES(obMixed.stDevice.eState, fwCecState::Claiming);
}

TEST(testClaimIgnoresOtherTxResults)
{
    Rig obRig;
    obRig.start();

    cecDeviceOnTxDone(obRig.stDevice, FRAME(0x88), fwCecTxResult::NotAcked, obRig.fresh());       // not the poll in flight
    CHECK(obRig.silent());
    cecDeviceOnTxDone(obRig.stDevice, FRAME(0x40, 0x04), fwCecTxResult::NotAcked, obRig.stOut); // not a poll
    CHECK(obRig.silent());
    CHECK_RES(obRig.stDevice.eState, fwCecState::Claiming);

    obRig.ready();
    cecDeviceOnTxDone(obRig.stDevice, FRAME(0x44), fwCecTxResult::Acked, obRig.stOut);          // the claim is over
    CHECK(obRig.silent());
    CHECK_EQ(cecDeviceSourceAddress(obRig.stDevice), CEC_ADDR_PLAYBACK_1);
}

TEST(testClaimWithoutPhysicalAddress)
{
    Rig obRig;
    obRig.start(CEC_PHYS_ADDR_INVALID);
    CHECK_RES(obRig.stDevice.eState, fwCecState::Ready);
    CHECK_EQ(obRig.stOut.btFrames, 0);                          // no poll: nothing is claimed
    CHECK_EQ(cecDeviceAddressMask(obRig.stDevice), 0);
    const fwCecEvent * pEvent = outFindEvent(obRig.stOut, fwCecEventType::AddressNone);
    CHECK(pEvent != nullptr && pEvent->abtData[0] == CEC_DEVTYPE_NONE);
}

TEST(testClaimTwoRoles)
{
    fwCecConfig stConfig;
    cecConfigDefault(stConfig);
    stConfig.abtDeviceType[1] = CEC_DEVTYPE_AUDIO_SYSTEM;
    Rig obRig(&stConfig);

    obRig.start();
    CHECK(outHasFrame(obRig.stOut, FRAME(0x44)));
    obRig.pollResult(fwCecTxResult::NotAcked);
    CHECK_RES(obRig.stDevice.eState, fwCecState::Claiming);     // the second role is still to claim
    CHECK(outHasFrame(obRig.stOut, FRAME(0x4F, 0x84, 0x10, 0x00, 0x04)));
    CHECK(outHasFrame(obRig.stOut, FRAME(0x55)));
    CHECK_EQ(cecDeviceAddressMask(obRig.stDevice), kPlayback1);

    obRig.pollResult(fwCecTxResult::NotAcked);
    CHECK_RES(obRig.stDevice.eState, fwCecState::Ready);
    CHECK(outHasFrame(obRig.stOut, FRAME(0x5F, 0x84, 0x10, 0x00, 0x05)));
    CHECK_EQ(cecDeviceAddressMask(obRig.stDevice), kPlayback1 | (1u << CEC_ADDR_AUDIO_SYSTEM));
    CHECK_EQ(cecDeviceSourceAddress(obRig.stDevice), CEC_ADDR_PLAYBACK_1);

    // Each role answers from its own address with its own type.
    obRig.rx(FRAME(0x05, 0x83));
    CHECK(outHasFrame(obRig.stOut, FRAME(0x5F, 0x84, 0x10, 0x00, 0x05)));
    obRig.rx(FRAME(0x04, 0x83));
    CHECK(outHasFrame(obRig.stOut, FRAME(0x4F, 0x84, 0x10, 0x00, 0x04)));

    // The audio system's one address in use: that role stays at 15, playback is kept.
    Rig obTaken(&stConfig);
    obTaken.start();
    obTaken.pollResult(fwCecTxResult::NotAcked);
    obTaken.pollResult(fwCecTxResult::Acked);
    CHECK_RES(obTaken.stDevice.eState, fwCecState::Ready);
    CHECK_EQ(cecDeviceAddressMask(obTaken.stDevice), kPlayback1);
    const fwCecEvent * pEvent = outFindEvent(obTaken.stOut, fwCecEventType::AddressNone);
    CHECK(pEvent != nullptr && pEvent->abtData[0] == CEC_DEVTYPE_AUDIO_SYSTEM);

    // The same type twice is one role: one address is claimed.
    stConfig.abtDeviceType[1] = CEC_DEVTYPE_PLAYBACK;
    Rig obTwice(&stConfig);
    obTwice.start();
    obTwice.pollResult(fwCecTxResult::NotAcked);
    CHECK_RES(obTwice.stDevice.eState, fwCecState::Ready);
    CHECK_EQ(obTwice.stOut.btFrames, 1);                        // the report, and no second poll
    CHECK_EQ(obTwice.stOut.btEvents, 1);
    CHECK_EQ(cecDeviceAddressMask(obTwice.stDevice), kPlayback1);

    // A type that claims no address leaves its role at 15.
    stConfig.abtDeviceType[1] = CEC_DEVTYPE_TV;
    Rig obTv(&stConfig);
    obTv.start();
    obTv.pollResult(fwCecTxResult::NotAcked);
    CHECK_RES(obTv.stDevice.eState, fwCecState::Ready);
    pEvent = outFindEvent(obTv.stOut, fwCecEventType::AddressNone);
    CHECK(pEvent != nullptr && pEvent->abtData[0] == CEC_DEVTYPE_TV);
    CHECK_EQ(cecDeviceAddressMask(obTv.stDevice), kPlayback1);
}

TEST(testVendorId)
{
    fwCecConfig stConfig;
    cecConfigDefault(stConfig);
    stConfig.uiVendorId = 0x123456;
    Rig obRig(&stConfig);

    obRig.start();
    obRig.pollResult(fwCecTxResult::NotAcked);
    CHECK_EQ(obRig.stOut.btFrames, 2);
    CHECK(sameFrame(obRig.stOut.astFrame[0], FRAME(0x4F, 0x84, 0x10, 0x00, 0x04)));
    CHECK(sameFrame(obRig.stOut.astFrame[1], FRAME(0x4F, 0x87, 0x12, 0x34, 0x56)));    // <Device Vendor ID>, once

    obRig.rx(FRAME(0x04, 0x8C));                                // <Give Device Vendor ID>
    CHECK_EQ(obRig.stOut.btFrames, 1);
    CHECK(outHasFrame(obRig.stOut, FRAME(0x4F, 0x87, 0x12, 0x34, 0x56)));

    // With no vendor ID the request is refused and nothing is broadcast.
    Rig obNone;
    obNone.start();
    obNone.pollResult(fwCecTxResult::NotAcked);
    CHECK_EQ(obNone.stOut.btFrames, 1);
    obNone.rx(FRAME(0x04, 0x8C));
    CHECK(outHasFrame(obNone.stOut, FRAME(0x40, 0x00, 0x8C, 0x00)));
}

// The requests of a TV's log, with the product's vendor ID: every answer
// byte for byte, directly to the TV except <Device Vendor ID>, which is
// broadcast. The TV's <Feature Abort> of an answer and its remote keys
// send nothing.
TEST(testProductAnswersTvLog)
{
    fwCecConfig stConfig;
    cecConfigDefault(stConfig);
    stConfig.uiVendorId = CEC_VENDOR_ID_PRODUCT;
    Rig obRig(&stConfig);

    obRig.start();
    obRig.pollResult(fwCecTxResult::NotAcked);
    CHECK_EQ(obRig.stOut.btFrames, 2);
    CHECK(sameFrame(obRig.stOut.astFrame[0], FRAME(0x4F, 0x84, 0x10, 0x00, 0x04)));
    CHECK(sameFrame(obRig.stOut.astFrame[1], FRAME(0x4F, 0x87, 0x00, 0x15, 0x82)));

    struct Exchange
    {
        fwCecFrame stRequest;
        fwCecFrame stAnswer;
    };
    const Exchange kExchanges[] = {
        { FRAME(0x04, 0x46), FRAME(0x40, 0x47, 'F', 'r', 'e', 'e', 'w', 'i', 'l', 'i', ' ', '2') },
        { FRAME(0x04, 0x8F), FRAME(0x40, 0x90, 0x00) },
        { FRAME(0x04, 0x9F), FRAME(0x40, 0x9E, 0x05) },
        { FRAME(0x04, 0x8C), FRAME(0x4F, 0x87, 0x00, 0x15, 0x82) },
    };
    for (const Exchange & stExchange : kExchanges)
    {
        obRig.rx(stExchange.stRequest);
        CHECK_OP(obRig.stOut.btFrames == 1, cecOpcode(stExchange.stRequest));
        CHECK_OP(sameFrame(obRig.stOut.astFrame[0], stExchange.stAnswer), cecOpcode(stExchange.stRequest));
        CHECK_OP(cecCheck(obRig.stOut.astFrame[0]) == fwCecCheck::Ok, cecOpcode(stExchange.stRequest));
    }

    for (uint8_t btAborted : {0x04, 0x47, 0x90, 0x9E})
    {
        const uint8_t abtAbort[] = { 0x04, CEC_OP_FEATURE_ABORT, btAborted, CEC_ABORT_UNRECOGNIZED };
        obRig.rx(frameOf(abtAbort, 4));
        CHECK_OP(obRig.stOut.btFrames == 0, btAborted);
    }
    obRig.rx(FRAME(0x04, 0x44, 0x02));
    CHECK_EQ(obRig.stOut.btFrames, 0);
    obRig.rx(FRAME(0x04, 0x45));
    CHECK_EQ(obRig.stOut.btFrames, 0);
}

TEST(testStandardAnswers)
{
    fwCecConfig stConfig;
    cecConfigDefault(stConfig);
    cecConfigSetOsdName(stConfig, "FreeWili");
    Rig obRig(&stConfig);
    obRig.ready();

    obRig.rx(FRAME(0x04, 0x83));                                // <Give Physical Address>
    CHECK_EQ(obRig.stOut.btFrames, 1);
    CHECK(outHasFrame(obRig.stOut, FRAME(0x4F, 0x84, 0x10, 0x00, 0x04)));
    obRig.rx(FRAME(0xF4, 0x83));                                // the same from an unregistered device
    CHECK(outHasFrame(obRig.stOut, FRAME(0x4F, 0x84, 0x10, 0x00, 0x04)));

    obRig.rx(FRAME(0x04, 0x9F));                                // <Get CEC Version>
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x9E, 0x05)));

    obRig.rx(FRAME(0x04, 0x8F));                                // <Give Device Power Status>
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x90, 0x00)));
    cecDeviceSetPowerStatus(obRig.stDevice, CEC_POWER_STANDBY);
    obRig.rx(FRAME(0x04, 0x8F));
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x90, 0x01)));

    obRig.rx(FRAME(0x04, 0x46));                                // <Give OSD Name>
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x47, 0x46, 0x72, 0x65, 0x65, 0x57, 0x69, 0x6C, 0x69)));

    obRig.rx(FRAME(0x04, 0x91));                                // <Get Menu Language>: only a TV answers it
    CHECK_EQ(obRig.stOut.btFrames, 1);
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x00, 0x91, 0x00)));
    obRig.rx(FRAME(0xF4, 0x91));                                // nobody to refuse it to
    CHECK(obRig.silent());

    obRig.rx(FRAME(0x04, 0xFF));                                // <Abort>
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x00, 0xFF, 0x04)));

    obRig.rx(FRAME(0x0F, 0x32, 'e', 'n', 'g'));                 // <Set Menu Language>
    CHECK_EQ(obRig.stOut.btFrames, 0);
    const fwCecEvent * pEvent = outFindEvent(obRig.stOut, fwCecEventType::MenuLanguage);
    CHECK(pEvent != nullptr && pEvent->abtData[0] == 'e' && pEvent->abtData[1] == 'n' && pEvent->abtData[2] == 'g');

    obRig.rx(FRAME(0x04));                                      // a poll: the acknowledge is the wire layer's
    CHECK(obRig.silent());
    obRig.rx(FRAME(0x08, 0x83));                                // for another device
    CHECK(obRig.silent());

    // The default configuration answers with the default name.
    Rig obDefault;
    obDefault.ready();
    obDefault.rx(FRAME(0x04, 0x46));
    CHECK(outHasFrame(obDefault.stOut,
                      FRAME(0x40, 0x47, 'F', 'r', 'e', 'e', 'w', 'i', 'l', 'i', ' ', '2')));

    // With the name emptied the request is refused.
    fwCecConfig stNoName;
    cecConfigDefault(stNoName);
    cecConfigSetOsdName(stNoName, "");
    Rig obNoName(&stNoName);
    obNoName.ready();
    obNoName.rx(FRAME(0x04, 0x46));
    CHECK(outHasFrame(obNoName.stOut, FRAME(0x40, 0x00, 0x46, 0x00)));
}

TEST(testRefusalsAndReports)
{
    Rig obRig;
    obRig.ready();

    obRig.rx(FRAME(0x04, 0x01));                                // not a table message
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x00, 0x01, 0x00)));
    obRig.rx(FRAME(0x04, 0x09, 0x01));                          // <Record On>: a recorder's message
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x00, 0x09, 0x00)));
    obRig.rx(FRAME(0x04, 0x71));                                // <Give Audio Status>: an audio system's
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x00, 0x71, 0x00)));

    // Answers and announcements are dropped, not refused.
    obRig.rx(FRAME(0x04, 0x90, 0x00));                          // <Report Power Status>
    CHECK(obRig.silent());
    obRig.rx(FRAME(0x04, 0x47, 'T', 'V'));                      // <Set OSD Name>
    CHECK(obRig.silent());
    obRig.rx(FRAME(0x04, 0x9E, 0x05));                          // <CEC Version>
    CHECK(obRig.silent());

    // A broadcast is never refused.
    obRig.rx(FRAME(0x0F, 0x01));
    CHECK(obRig.silent());
    obRig.rx(FRAME(0x0F, 0x87, 0x00, 0x00, 0xF0));              // another device's <Device Vendor ID>
    CHECK(obRig.silent());

    // Every answer and announcement is dropped; every request gets one frame
    // back, an answer or a refusal.
    static const uint8_t kReports[] = {
        CEC_OP_FEATURE_ABORT, CEC_OP_REPORT_POWER_STATUS, CEC_OP_CEC_VERSION, CEC_OP_SET_OSD_NAME,
        CEC_OP_MENU_STATUS, CEC_OP_DECK_STATUS, CEC_OP_INACTIVE_SOURCE, CEC_OP_RECORD_STATUS,
        CEC_OP_TIMER_STATUS, CEC_OP_REPORT_AUDIO_STATUS,
    };
    for (uint8_t btOpcode : kReports)
    {
        obRig.rx(minimalFrame(btOpcode, CEC_ADDR_TV));
        CHECK_OP(obRig.stOut.btFrames == 0, btOpcode);
    }
    static const uint8_t kRequests[] = {
        CEC_OP_GIVE_DEVICE_POWER_STATUS, CEC_OP_GET_CEC_VERSION, CEC_OP_GIVE_OSD_NAME,
        CEC_OP_MENU_REQUEST, CEC_OP_GIVE_DECK_STATUS, CEC_OP_RECORD_ON, CEC_OP_ABORT,
        CEC_OP_GIVE_AUDIO_STATUS, CEC_OP_SET_OSD_STRING, CEC_OP_VENDOR_COMMAND,
    };
    for (uint8_t btOpcode : kRequests)
    {
        obRig.rx(minimalFrame(btOpcode, CEC_ADDR_TV));
        CHECK_OP(obRig.stOut.btFrames == 1, btOpcode);
    }

    // A device refusing one of ours is reported.
    obRig.rx(FRAME(0x04, 0x00, 0x64, 0x04));                    // the TV refuses <Set OSD String>
    CHECK_EQ(obRig.stOut.btFrames, 0);
    const fwCecEvent * pEvent = outFindEvent(obRig.stOut, fwCecEventType::FeatureAbort);
    CHECK(pEvent != nullptr && pEvent->btPeer == CEC_ADDR_TV && pEvent->btOwn == CEC_ADDR_PLAYBACK_1 &&
          pEvent->abtData[0] == 0x64 && pEvent->abtData[1] == CEC_ABORT_REFUSED);
}

TEST(testBehaviourSwitches)
{
    static const fwCecFrame kRequests[] = {
        FRAME(0x04, 0x83), FRAME(0x04, 0x9F), FRAME(0x04, 0x8F), FRAME(0x04, 0x46), FRAME(0x04, 0x8C),
        FRAME(0x0F, 0x36), FRAME(0x0F, 0x86, 0x10, 0x00), FRAME(0x04, 0x44, 0x00), FRAME(0x04, 0x45),
        FRAME(0x04, 0x8D, 0x00), FRAME(0x04, 0x1A, 0x03), FRAME(0x04, 0x41, 0x24), FRAME(0x04, 0x42, 0x03),
        FRAME(0x04, 0xFF), FRAME(0x04, 0x01), FRAME(0x0F, 0x32, 'e', 'n', 'g'),
    };

    // Every behaviour off: the device answers nothing and reports nothing.
    fwCecConfig stConfig;
    cecConfigDefault(stConfig);
    cecConfigSetOsdName(stConfig, "FreeWili");
    stConfig.uiVendorId   = 0x123456;
    stConfig.uiBehaviours = 0;
    Rig obOff(&stConfig);
    obOff.start();
    obOff.pollResult(fwCecTxResult::NotAcked);
    CHECK_EQ(obOff.stOut.btFrames, 1);                          // the claim still announces itself, with no vendor ID
    for (const fwCecFrame & stRequest : kRequests)
    {
        obOff.rx(stRequest);
        CHECK_OP(obOff.silent(), cecOpcode(stRequest));
    }

    // Only the refusals off: what a behaviour answers is still answered.
    stConfig.uiBehaviours = CEC_BEH_ALL & ~CEC_BEH_ABORT;
    Rig obNoAbort(&stConfig);
    obNoAbort.ready();
    obNoAbort.rx(FRAME(0x04, 0x01));
    CHECK(obNoAbort.silent());
    obNoAbort.rx(FRAME(0x04, 0xFF));
    CHECK(obNoAbort.silent());
    obNoAbort.rx(FRAME(0x04, 0x91));
    CHECK(obNoAbort.silent());
    obNoAbort.rx(FRAME(0x04, 0x8F));
    CHECK(outHasFrame(obNoAbort.stOut, FRAME(0x40, 0x90, 0x00)));

    // One behaviour off: its request falls to the refusal.
    stConfig.uiBehaviours = CEC_BEH_ALL & ~CEC_BEH_POWER_STATUS;
    Rig obNoPower(&stConfig);
    obNoPower.ready();
    obNoPower.rx(FRAME(0x04, 0x8F));
    CHECK(outHasFrame(obNoPower.stOut, FRAME(0x40, 0x00, 0x8F, 0x00)));
}

TEST(testOneTouchPlay)
{
    Rig obRig;
    CHECK(!cecDeviceOneTouchPlay(obRig.stDevice, false, obRig.fresh()));          // off
    obRig.start();
    CHECK(!cecDeviceOneTouchPlay(obRig.stDevice, false, obRig.fresh()));          // still claiming
    CHECK(obRig.silent());

    obRig.ready();
    CHECK(cecDeviceOneTouchPlay(obRig.stDevice, false, obRig.stOut));
    CHECK_EQ(obRig.stOut.btFrames, 2);
    CHECK(sameFrame(obRig.stOut.astFrame[0], FRAME(0x40, 0x04)));               // <Image View On>
    CHECK(sameFrame(obRig.stOut.astFrame[1], FRAME(0x4F, 0x82, 0x10, 0x00)));   // <Active Source>
    const fwCecEvent * pEvent = outFindEvent(obRig.stOut, fwCecEventType::ActiveSource);
    CHECK(pEvent != nullptr && pEvent->abtData[0] == 1);
    CHECK(obRig.isActiveSource());

    CHECK(cecDeviceOneTouchPlay(obRig.stDevice, true, obRig.fresh()));
    CHECK(sameFrame(obRig.stOut.astFrame[0], FRAME(0x40, 0x0D)));               // <Text View On>
    CHECK_EQ(obRig.stOut.btEvents, 0);                                          // already the active source

    // At address 15 the TV can still be woken; nothing is claimed for it.
    Rig obUnregistered;
    obUnregistered.start(CEC_PHYS_ADDR_INVALID);
    CHECK(cecDeviceOneTouchPlay(obUnregistered.stDevice, false, obUnregistered.fresh()));
    CHECK_EQ(obUnregistered.stOut.btFrames, 1);
    CHECK(outHasFrame(obUnregistered.stOut, FRAME(0xF0, 0x04)));
    CHECK(!obUnregistered.isActiveSource());
}

TEST(testRouting)
{
    Rig obRig;
    obRig.ready();

    obRig.rx(FRAME(0x0F, 0x85));                                // <Request Active Source>: not us
    CHECK(obRig.silent());

    obRig.rx(FRAME(0x0F, 0x86, 0x10, 0x00));                    // <Set Stream Path> to our place
    CHECK(outHasFrame(obRig.stOut, FRAME(0x4F, 0x82, 0x10, 0x00)));
    const fwCecEvent * pEvent = outFindEvent(obRig.stOut, fwCecEventType::ActiveSource);
    CHECK(pEvent != nullptr && pEvent->abtData[0] == 1);

    obRig.rx(FRAME(0x0F, 0x85));                                // now it is us
    CHECK_EQ(obRig.stOut.btEvents, 0);
    CHECK(outHasFrame(obRig.stOut, FRAME(0x4F, 0x82, 0x10, 0x00)));

    obRig.rx(FRAME(0x0F, 0x80, 0x00, 0x00, 0x10, 0x00));        // <Routing Change> onto our place: kept
    CHECK(obRig.silent());
    CHECK(obRig.isActiveSource());

    obRig.rx(FRAME(0x5F, 0x82, 0x20, 0x00));                    // another device goes active
    CHECK_EQ(obRig.stOut.btFrames, 0);
    pEvent = outFindEvent(obRig.stOut, fwCecEventType::ActiveSource);
    CHECK(pEvent != nullptr && pEvent->abtData[0] == 0);
    CHECK(!obRig.isActiveSource());
    obRig.rx(FRAME(0x5F, 0x82, 0x20, 0x00));                    // nothing left to lose
    CHECK(obRig.silent());

    obRig.rx(FRAME(0x0F, 0x86, 0x10, 0x00));
    obRig.rx(FRAME(0x0F, 0x86, 0x20, 0x00));                    // the TV picks another input
    pEvent = outFindEvent(obRig.stOut, fwCecEventType::ActiveSource);
    CHECK(obRig.stOut.btFrames == 0 && pEvent != nullptr && pEvent->abtData[0] == 0);

    obRig.rx(FRAME(0x0F, 0x86, 0x10, 0x00));
    obRig.rx(FRAME(0x0F, 0x80, 0x10, 0x00, 0x30, 0x00));        // <Routing Change> away
    pEvent = outFindEvent(obRig.stOut, fwCecEventType::ActiveSource);
    CHECK(pEvent != nullptr && pEvent->abtData[0] == 0);

    // <Inactive Source> on request, only while active.
    CHECK(!cecDeviceInactiveSource(obRig.stDevice, obRig.fresh()));
    CHECK(obRig.silent());
    obRig.rx(FRAME(0x0F, 0x86, 0x10, 0x00));
    CHECK(cecDeviceInactiveSource(obRig.stDevice, obRig.fresh()));
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x9D, 0x10, 0x00)));
    pEvent = outFindEvent(obRig.stOut, fwCecEventType::ActiveSource);
    CHECK(pEvent != nullptr && pEvent->abtData[0] == 0);
}

TEST(testStandby)
{
    Rig obRig;
    obRig.ready();

    obRig.rx(FRAME(0x0F, 0x36));                                // from the TV, to all
    CHECK_EQ(obRig.stOut.btFrames, 0);
    CHECK_EQ(obRig.stOut.btEvents, 1);
    const fwCecEvent * pEvent = outFindEvent(obRig.stOut, fwCecEventType::Standby);
    CHECK(pEvent != nullptr && pEvent->btPeer == CEC_ADDR_TV && pEvent->btOwn == CEC_ADDR_BROADCAST);

    obRig.rx(FRAME(0x04, 0x36));                                // to this device alone
    CHECK(obRig.stOut.btFrames == 0 && outFindEvent(obRig.stOut, fwCecEventType::Standby) != nullptr);
    obRig.rx(FRAME(0xFF, 0x36));                                // from an unregistered device
    CHECK(outFindEvent(obRig.stOut, fwCecEventType::Standby) != nullptr);

    // The active source stops being it first.
    obRig.rx(FRAME(0x0F, 0x86, 0x10, 0x00));
    obRig.rx(FRAME(0x0F, 0x36));
    CHECK_EQ(obRig.stOut.btEvents, 2);
    CHECK_RES(obRig.stOut.astEvent[0].eType, fwCecEventType::ActiveSource);
    CHECK_EQ(obRig.stOut.astEvent[0].abtData[0], 0);
    CHECK_RES(obRig.stOut.astEvent[1].eType, fwCecEventType::Standby);

    // Sending it.
    CHECK(cecDeviceStandby(obRig.stDevice, CEC_ADDR_BROADCAST, obRig.fresh()));
    CHECK(outHasFrame(obRig.stOut, FRAME(0x4F, 0x36)));
    CHECK(cecDeviceStandby(obRig.stDevice, CEC_ADDR_TV, obRig.fresh()));
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x36)));
    CHECK(!cecDeviceStandby(obRig.stDevice, CEC_ADDR_PLAYBACK_1, obRig.fresh()));     // its own address
    CHECK(!cecDeviceStandby(obRig.stDevice, 16, obRig.stOut));
    CHECK(obRig.silent());

    // An unregistered device can still put the system to standby.
    Rig obUnregistered;
    obUnregistered.start(CEC_PHYS_ADDR_INVALID);
    CHECK(cecDeviceStandby(obUnregistered.stDevice, CEC_ADDR_BROADCAST, obUnregistered.fresh()));
    CHECK(outHasFrame(obUnregistered.stOut, FRAME(0xFF, 0x36)));
}

TEST(testRemoteKeys)
{
    Rig obRig;
    obRig.ready();

    obRig.rx(FRAME(0x04, 0x44, 0x00), 1000);                    // Select pressed
    CHECK_EQ(obRig.stOut.btFrames, 0);
    const fwCecEvent * pEvent = outFindEvent(obRig.stOut, fwCecEventType::KeyDown);
    CHECK(pEvent != nullptr && pEvent->btPeer == CEC_ADDR_TV && pEvent->btOwn == CEC_ADDR_PLAYBACK_1 &&
          pEvent->abtData[0] == CEC_UI_SELECT);

    obRig.rx(FRAME(0x04, 0x44, 0x00), 1450);                    // the held key repeating
    CHECK(obRig.silent());
    obRig.tick(1450 + CEC_KEY_RELEASE_MS - 1);
    CHECK(obRig.silent());
    obRig.tick(1450 + CEC_KEY_RELEASE_MS);                      // no repeat and no release: taken as released
    pEvent = outFindEvent(obRig.stOut, fwCecEventType::KeyUp);
    CHECK(pEvent != nullptr && pEvent->btPeer == CEC_ADDR_TV && pEvent->btOwn == CEC_ADDR_PLAYBACK_1 &&
          pEvent->abtData[0] == CEC_UI_SELECT);
    obRig.tick(1450 + 2 * CEC_KEY_RELEASE_MS);
    CHECK(obRig.silent());

    obRig.rx(FRAME(0x04, 0x44, 0x01), 5000);                    // Up pressed, then released
    CHECK(outFindEvent(obRig.stOut, fwCecEventType::KeyDown) != nullptr);
    obRig.rx(FRAME(0x54, 0x45), 5050);                          // a release from another device: not this key
    CHECK(obRig.silent());
    obRig.rx(FRAME(0x04, 0x45), 5100);
    pEvent = outFindEvent(obRig.stOut, fwCecEventType::KeyUp);
    CHECK(pEvent != nullptr && pEvent->abtData[0] == CEC_UI_UP);
    obRig.rx(FRAME(0x04, 0x45), 5150);                          // already up
    CHECK(obRig.silent());

    obRig.rx(FRAME(0x04, 0x44, 0x03), 6000);                    // Left, then Right with no release between
    obRig.rx(FRAME(0x04, 0x44, 0x04), 6100);
    CHECK_EQ(obRig.stOut.btEvents, 2);
    CHECK_RES(obRig.stOut.astEvent[0].eType, fwCecEventType::KeyUp);
    CHECK_EQ(obRig.stOut.astEvent[0].abtData[0], CEC_UI_LEFT);
    CHECK_RES(obRig.stOut.astEvent[1].eType, fwCecEventType::KeyDown);
    CHECK_EQ(obRig.stOut.astEvent[1].abtData[0], CEC_UI_RIGHT);

    // The time wraps without a false release.
    obRig.rx(FRAME(0x04, 0x45), 6200);
    obRig.rx(FRAME(0x04, 0x44, 0x00), 0xFFFFFF00u);
    obRig.tick(0x00000010u);                                    // 272 ms later
    CHECK(obRig.silent());
    obRig.tick(0x00000200u);
    CHECK(outFindEvent(obRig.stOut, fwCecEventType::KeyUp) != nullptr);

    // A key held when the device stops is released.
    obRig.rx(FRAME(0x04, 0x44, 0x00), 9000);
    cecDeviceStop(obRig.stDevice, obRig.fresh());
    CHECK(outFindEvent(obRig.stOut, fwCecEventType::KeyUp) != nullptr);
    CHECK_RES(obRig.stDevice.eState, fwCecState::Off);

    // Sending keys to another device.
    obRig.ready();
    CHECK(cecDeviceKeyPress(obRig.stDevice, CEC_ADDR_TV, 0x41, obRig.stOut));       // Volume Up
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x44, 0x41)));
    CHECK(cecDeviceKeyRelease(obRig.stDevice, CEC_ADDR_TV, obRig.fresh()));
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x45)));
    CHECK(!cecDeviceKeyPress(obRig.stDevice, CEC_ADDR_BROADCAST, 0x41, obRig.fresh()));
    CHECK(!cecDeviceKeyPress(obRig.stDevice, CEC_ADDR_PLAYBACK_1, 0x41, obRig.stOut));
    CHECK(!cecDeviceKeyRelease(obRig.stDevice, CEC_ADDR_BROADCAST, obRig.stOut));
    CHECK(obRig.silent());
}

TEST(testMenu)
{
    Rig obRig;
    obRig.ready();

    obRig.rx(FRAME(0x04, 0x8D, 0x02));                          // query: not showing a menu
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x8E, 0x01)));
    obRig.rx(FRAME(0x04, 0x8D, 0x00));                          // activate
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x8E, 0x00)));
    const fwCecEvent * pEvent = outFindEvent(obRig.stOut, fwCecEventType::MenuRequest);
    CHECK(pEvent != nullptr && pEvent->btPeer == CEC_ADDR_TV && pEvent->abtData[0] == CEC_MENU_REQUEST_ACTIVATE);
    obRig.rx(FRAME(0x04, 0x8D, 0x02));
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x8E, 0x00)));
    obRig.rx(FRAME(0x04, 0x8D, 0x01));                          // deactivate
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x8E, 0x01)));

    // A change made on the device itself is announced to the TV, once.
    cecDeviceSetMenuState(obRig.stDevice, CEC_MENU_STATE_ACTIVATED, obRig.fresh());
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x8E, 0x00)));
    cecDeviceSetMenuState(obRig.stDevice, CEC_MENU_STATE_ACTIVATED, obRig.fresh());
    CHECK(obRig.silent());
}

TEST(testDeck)
{
    Rig obRig;
    obRig.ready();

    obRig.rx(FRAME(0x04, 0x1A, 0x03));                          // <Give Deck Status> once
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x1B, 0x1A)));   // stopped
    cecDeviceSetDeckInfo(obRig.stDevice, CEC_DECK_INFO_PLAY, obRig.fresh());
    CHECK(obRig.silent());                                      // nobody asked to be told

    obRig.rx(FRAME(0x04, 0x1A, 0x01));                          // on: now, and on every change
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x1B, 0x11)));
    cecDeviceSetDeckInfo(obRig.stDevice, CEC_DECK_INFO_STILL, obRig.fresh());
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x1B, 0x14)));
    cecDeviceSetDeckInfo(obRig.stDevice, CEC_DECK_INFO_STILL, obRig.fresh());     // no change
    CHECK(obRig.silent());

    obRig.rx(FRAME(0x04, 0x1A, 0x02));                          // off
    CHECK(obRig.silent());
    cecDeviceSetDeckInfo(obRig.stDevice, CEC_DECK_INFO_STOP, obRig.stOut);
    CHECK(obRig.silent());

    obRig.rx(FRAME(0x04, 0x1A, 0x07));                          // not a status request
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x00, 0x1A, 0x03)));

    obRig.rx(FRAME(0x04, 0x41, 0x24));                          // <Play> forward
    const fwCecEvent * pEvent = outFindEvent(obRig.stOut, fwCecEventType::DeckPlay);
    CHECK(obRig.stOut.btFrames == 0 && pEvent != nullptr && pEvent->abtData[0] == 0x24);
    obRig.rx(FRAME(0x04, 0x42, 0x03));                          // <Deck Control> stop
    pEvent = outFindEvent(obRig.stOut, fwCecEventType::DeckControl);
    CHECK(obRig.stOut.btFrames == 0 && pEvent != nullptr && pEvent->abtData[0] == 0x03);

    // An audio system is not a deck.
    fwCecConfig stConfig;
    cecConfigDefault(stConfig);
    stConfig.abtDeviceType[0] = CEC_DEVTYPE_AUDIO_SYSTEM;
    Rig obAudio(&stConfig);
    obAudio.ready();
    CHECK_EQ(cecDeviceSourceAddress(obAudio.stDevice), CEC_ADDR_AUDIO_SYSTEM);
    obAudio.rx(FRAME(0x05, 0x1A, 0x03));
    CHECK(outHasFrame(obAudio.stOut, FRAME(0x50, 0x00, 0x1A, 0x00)));
    obAudio.rx(FRAME(0x05, 0x41, 0x24));
    CHECK(outHasFrame(obAudio.stOut, FRAME(0x50, 0x00, 0x41, 0x00)));
}

TEST(testOsdString)
{
    Rig obRig;
    obRig.ready();

    CHECK(cecDeviceOsdString(obRig.stDevice, CEC_DISPLAY_DEFAULT_TIME, "Hello", 5, obRig.stOut));
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x64, 0x00, 0x48, 0x65, 0x6C, 0x6C, 0x6F)));
    CHECK(cecDeviceOsdString(obRig.stDevice, CEC_DISPLAY_UNTIL_CLEARED, "1234567890123", 13, obRig.fresh()));
    CHECK_EQ(obRig.stOut.astFrame[0].btLen, CEC_FRAME_MAX);
    CHECK(cecDeviceOsdString(obRig.stDevice, CEC_DISPLAY_CLEAR, nullptr, 0, obRig.fresh()));
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x64, 0x80)));

    CHECK(!cecDeviceOsdString(obRig.stDevice, CEC_DISPLAY_DEFAULT_TIME, "12345678901234", 14, obRig.fresh()));
    CHECK(!cecDeviceOsdString(obRig.stDevice, CEC_DISPLAY_DEFAULT_TIME, "", 0, obRig.stOut));
    CHECK(!cecDeviceOsdString(obRig.stDevice, CEC_DISPLAY_DEFAULT_TIME, nullptr, 3, obRig.stOut));
    CHECK(!cecDeviceOsdString(obRig.stDevice, CEC_DISPLAY_DEFAULT_TIME, "a\nb", 3, obRig.stOut));
    CHECK(!cecDeviceOsdString(obRig.stDevice, 0xC0, "Hello", 5, obRig.stOut));
    CHECK(obRig.silent());

    Rig obUnregistered;
    obUnregistered.start(CEC_PHYS_ADDR_INVALID);
    CHECK(!cecDeviceOsdString(obUnregistered.stDevice, CEC_DISPLAY_DEFAULT_TIME, "Hello", 5, obUnregistered.fresh()));
}

TEST(testStopAndRestart)
{
    Rig obRig;
    obRig.ready();
    obRig.rx(FRAME(0x0F, 0x86, 0x10, 0x00));                    // become the active source
    cecOutClear(obRig.stOut);

    cecDeviceStop(obRig.stDevice, obRig.stOut);
    CHECK(outHasFrame(obRig.stOut, FRAME(0x40, 0x9D, 0x10, 0x00)));     // <Inactive Source>
    CHECK_RES(obRig.stDevice.eState, fwCecState::Off);
    CHECK_EQ(cecDeviceAddressMask(obRig.stDevice), 0);

    obRig.rx(FRAME(0x04, 0x8F));                                // off: deaf
    CHECK(obRig.silent());
    cecDeviceStop(obRig.stDevice, obRig.stOut);                 // twice is harmless
    CHECK(obRig.silent());

    obRig.start(0x2000);                                        // moved to another input
    obRig.pollResult(fwCecTxResult::NotAcked);
    CHECK(outHasFrame(obRig.stOut, FRAME(0x4F, 0x84, 0x20, 0x00, 0x04)));
    CHECK(!obRig.isActiveSource());
}

TEST(testOutOverflow)
{
    Rig obRig;
    obRig.ready();
    cecOutClear(obRig.stOut);
    for (int i = 0; i < CEC_OUT_FRAMES_MAX; i++)
        CHECK(cecDeviceStandby(obRig.stDevice, CEC_ADDR_TV, obRig.stOut));
    CHECK_EQ(obRig.stOut.btFrames, CEC_OUT_FRAMES_MAX);
    CHECK(!obRig.stOut.bOverflow);
    cecDeviceStandby(obRig.stDevice, CEC_ADDR_TV, obRig.stOut);
    CHECK_EQ(obRig.stOut.btFrames, CEC_OUT_FRAMES_MAX);
    CHECK(obRig.stOut.bOverflow);
}

TEST(testDeviceNames)
{
    CHECK(strcmp(cecDeviceTypeName(CEC_DEVTYPE_PLAYBACK), "playback") == 0);
    CHECK(strcmp(cecDeviceTypeName(2), "unknown") == 0);
    CHECK(strcmp(cecPowerStatusName(CEC_POWER_STANDBY), "standby") == 0);
    CHECK(strcmp(cecPowerStatusName(4), "unknown") == 0);
    CHECK(strcmp(cecUiCommandName(CEC_UI_SELECT), "select") == 0);
    CHECK(strcmp(cecUiCommandName(CEC_UI_F4_YELLOW), "f4_yellow") == 0);
    CHECK(strcmp(cecUiCommandName(0x0E), "unknown") == 0);
    CHECK(strcmp(cecUiCommandName(0xFF), "unknown") == 0);
    CHECK(strcmp(cecStateName(fwCecState::Claiming), "claiming") == 0);
    CHECK(strcmp(cecEventName(fwCecEventType::KeyDown), "key_down") == 0);

    // Named codes are tokens, and no two share a name.
    int iNamed = 0;
    for (int iCode = 0; iCode < 256; iCode++)
    {
        const char * szName = cecUiCommandName((uint8_t)iCode);
        if (strcmp(szName, "unknown") == 0)
            continue;
        iNamed++;
        CHECK_OP(isToken(szName), iCode);
        for (int j = 0; j < iCode; j++)
            CHECK_OP(strcmp(cecUiCommandName((uint8_t)j), szName) != 0, iCode);
    }
    CHECK_EQ(iNamed, 85);        // the codes CEC Table 30 assigns

    for (int iType = 0; iType <= (int)fwCecEventType::FeatureAbort; iType++)
        CHECK(isToken(cecEventName((fwCecEventType)iType)));
}

// Random traffic at a ready device: what it sends is always well formed, from
// an address it holds, and never an answer to something that was not for it.
TEST(testFuzzDevice)
{
    rngSeed(0x0D15EA5Eu);
    fwCecConfig stConfig;
    cecConfigDefault(stConfig);
    cecConfigSetOsdName(stConfig, "FreeWili");
    stConfig.abtDeviceType[1] = CEC_DEVTYPE_AUDIO_SYSTEM;
    stConfig.uiVendorId       = 0x123456;
    Rig obRig(&stConfig);
    obRig.ready();
    const uint16_t uiMask = cecDeviceAddressMask(obRig.stDevice);
    uint32_t uiNowMs = 0;

    for (int iRound = 0; iRound < 200000; iRound++)
    {
        fwCecFrame stFrame = randomFrame();
        // Half the frames carry a table opcode, so the behaviours are reached.
        if (stFrame.btLen >= 2 && (rng() & 1u) != 0)
            stFrame.abtBlock[1] = cecMsgAt((int)(rng() % (uint32_t)cecMsgCount())).btOpcode;

        uiNowMs += rng() % 700u;
        obRig.rx(stFrame, uiNowMs);
        const fwCecRx eRx = cecClassifyRx(stFrame, uiMask);

        CHECK(!obRig.stOut.bOverflow);
        if (eRx != fwCecRx::Handle && eRx != fwCecRx::Unsupported)
            CHECK(obRig.silent());

        for (int i = 0; i < obRig.stOut.btFrames; i++)
        {
            const fwCecFrame & stSent = obRig.stOut.astFrame[i];
            CHECK_RES(cecCheck(stSent), fwCecCheck::Ok);
            CHECK(((uiMask >> cecInitiator(stSent)) & 1u) != 0);
            if (cecOpcode(stSent) == CEC_OP_FEATURE_ABORT)
            {
                CHECK(!cecIsBroadcast(stFrame));
                CHECK_EQ(cecDestination(stSent), cecInitiator(stFrame));
                CHECK_EQ(cecInitiator(stSent), cecDestination(stFrame));
                CHECK_EQ(cecOperands(stSent)[0], cecOpcode(stFrame));
                CHECK(cecOpcode(stFrame) != CEC_OP_FEATURE_ABORT);
            }
        }

        obRig.tick(uiNowMs);
        CHECK(obRig.stOut.btFrames == 0);
        CHECK_RES(obRig.stDevice.eState, fwCecState::Ready);
        CHECK_EQ(cecDeviceAddressMask(obRig.stDevice), uiMask);
    }
}

int main()
{
    testTableIntegrity();
    testTableKnownRows();
    testEveryRowBuildsAndChecks();
    testHandBuiltFrames();
    testBuildLimits();
    testCheck();
    testClassifyRx();
    testFeatureAbort();
    testNames();
    testFuzzFollower();

    testConfigDefault();
    testClaimFirstFree();
    testClaimSkipsAddressesInUse();
    testClaimAllInUse();
    testClaimRetriesAFailedSend();
    testClaimRestartsAfterSendFailures();
    testClaimAllInUseDoesNotRestart();
    testClaimIgnoresOtherTxResults();
    testClaimWithoutPhysicalAddress();
    testClaimTwoRoles();
    testVendorId();
    testProductAnswersTvLog();
    testStandardAnswers();
    testRefusalsAndReports();
    testBehaviourSwitches();
    testOneTouchPlay();
    testRouting();
    testStandby();
    testRemoteKeys();
    testMenu();
    testDeck();
    testOsdString();
    testStopAndRestart();
    testOutOverflow();
    testDeviceNames();
    testFuzzDevice();

    runControllerTests();
    runRxQueueTests();
    runKeyHoldTests();
    runWireTests();

    printf("cec_tests: %d messages, %d tests, %d checks, %d failures\n",
           cecMsgCount(), s_iTests, s_iChecks, s_iFailures);
    return s_iFailures == 0 ? 0 : 1;
}
