// The controller part of fwCECCore: the answer each request waits for, the
// matching of a received frame against a request, the builders and readers of
// the messages with an operand layout, and the bus scan.

#include "cec_test_harness.h"

// Every pair of CEC 15 where one message is the answer to another.
TEST(testReplyOpcodes)
{
    struct { uint8_t btRequest; int iReply; } astPair[] =
    {
        { CEC_OP_GIVE_TUNER_DEVICE_STATUS,       CEC_OP_TUNER_DEVICE_STATUS },
        { CEC_OP_RECORD_ON,                      CEC_OP_RECORD_STATUS },
        { CEC_OP_RECORD_TV_SCREEN,               CEC_OP_RECORD_ON },
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

    // Every table message: the pairs above answer, nothing else does.
    int iAnswered = 0;
    for (int i = 0; i < cecMsgCount(); i++)
    {
        const fwCecMsgInfo & st = cecMsgAt(i);
        fwCecFrame stFrame = {};
        const uint8_t abtOperands[CEC_OPERANDS_MAX] = { CEC_STATUS_REQUEST_ONCE };
        CHECK_OP(cecBuild(stFrame, 4, 0, st.btOpcode, abtOperands, st.btOperandsMin), st.btOpcode);

        int iExpected = CEC_REPLY_NONE;
        for (const auto & stPair : astPair)
            if (stPair.btRequest == st.btOpcode)
                iExpected = stPair.iReply;
        if (st.btOpcode == CEC_OP_GIVE_DECK_STATUS)
            iExpected = CEC_OP_DECK_STATUS;
        CHECK_OP(cecReplyOpcode(stFrame) == iExpected, st.btOpcode);
        if (iExpected != CEC_REPLY_NONE)
        {
            iAnswered++;
            CHECK_OP(cecMsgFind((uint8_t)iExpected) != nullptr, st.btOpcode);
            // A request is never itself only an answer.
            CHECK_OP((st.btFlags & CEC_MSG_REPORT) == 0, st.btOpcode);
        }
    }
    CHECK_EQ(iAnswered, (int)(sizeof(astPair) / sizeof(astPair[0])) + 1);

    // <Give Deck Status>: "off" asks for no answer.
    CHECK_EQ(cecReplyOpcode(FRAME(0x40, 0x1A, 0x01)), CEC_OP_DECK_STATUS);
    CHECK_EQ(cecReplyOpcode(FRAME(0x40, 0x1A, 0x03)), CEC_OP_DECK_STATUS);
    CHECK_EQ(cecReplyOpcode(FRAME(0x40, 0x1A, 0x02)), CEC_REPLY_NONE);
    CHECK_EQ(cecReplyOpcode(FRAME(0x40, 0x1A)), CEC_REPLY_NONE);

    // A poll, a message nobody answers, an opcode outside the table.
    CHECK_EQ(cecReplyOpcode(FRAME(0x40)), CEC_REPLY_NONE);
    CHECK_EQ(cecReplyOpcode(FRAME(0x40, 0x04)), CEC_REPLY_NONE);
    CHECK_EQ(cecReplyOpcode(FRAME(0x40, 0x44, 0x01)), CEC_REPLY_NONE);
    CHECK_EQ(cecReplyOpcode(FRAME(0x40, 0x36)), CEC_REPLY_NONE);
    CHECK_EQ(cecReplyOpcode(FRAME(0x40, 0x50)), CEC_REPLY_NONE);
    fwCecFrame stBad = {};
    CHECK_EQ(cecReplyOpcode(stBad), CEC_REPLY_NONE);
    stBad.btLen = CEC_FRAME_MAX + 1;
    stBad.abtBlock[1] = CEC_OP_GIVE_OSD_NAME;
    CHECK_EQ(cecReplyOpcode(stBad), CEC_REPLY_NONE);
}

TEST(testMatchReply)
{
    // <Give OSD Name> to the TV: <Set OSD Name> from the TV.
    CHECK_RES(cecMatchReply(FRAME(0x04, 0x47, 0x54, 0x56), 0, 0x46, 0x47), fwCecMatch::Reply);
    CHECK_RES(cecMatchReply(FRAME(0x54, 0x47, 0x54, 0x56), 0, 0x46, 0x47), fwCecMatch::None);   // another device
    CHECK_RES(cecMatchReply(FRAME(0x04, 0x47), 0, 0x46, 0x47), fwCecMatch::None);               // no name
    CHECK_RES(cecMatchReply(FRAME(0x04, 0x00, 0x46, 0x00), 0, 0x46, 0x47), fwCecMatch::Refused);
    CHECK_RES(cecMatchReply(FRAME(0x04, 0x00, 0x8F, 0x00), 0, 0x46, 0x47), fwCecMatch::None);   // refuses another message
    CHECK_RES(cecMatchReply(FRAME(0x04, 0x00, 0x46), 0, 0x46, 0x47), fwCecMatch::None);         // too short to be a refusal
    CHECK_RES(cecMatchReply(FRAME(0x04), 0, 0x46, 0x47), fwCecMatch::None);                     // a poll

    // A broadcast answer: <Report Physical Address> to <Give Physical Address>.
    CHECK_RES(cecMatchReply(FRAME(0x0F, 0x84, 0x00, 0x00, 0x00), 0, 0x83, 0x84), fwCecMatch::Reply);
    CHECK_RES(cecMatchReply(FRAME(0x0F, 0x84, 0x00, 0x00), 0, 0x83, 0x84), fwCecMatch::None);

    // Any device answers <Request Active Source>.
    CHECK_RES(cecMatchReply(FRAME(0x8F, 0x82, 0x21, 0x00), CEC_ADDR_BROADCAST, 0x85, 0x82), fwCecMatch::Reply);
    CHECK_RES(cecMatchReply(FRAME(0x3F, 0x82, 0x30, 0x00), CEC_ADDR_BROADCAST, 0x85, 0x82), fwCecMatch::Reply);

    // <Abort> is answered by its <Feature Abort>, which is then the reply.
    CHECK_RES(cecMatchReply(FRAME(0x04, 0x00, 0xFF, 0x04), 0, 0xFF, 0x00), fwCecMatch::Reply);
    CHECK_RES(cecMatchReply(FRAME(0x04, 0x00, 0x46, 0x04), 0, 0xFF, 0x00), fwCecMatch::None);

    // A reply opcode outside the table is taken with any operands.
    CHECK_RES(cecMatchReply(FRAME(0x04, 0x50), 0, 0x89, 0x50), fwCecMatch::Reply);

    // Random frames: a reply is always from the peer, with the reply opcode or
    // a <Feature Abort> naming the request.
    rngSeed(0x5EEDC0DEu);
    for (int iRound = 0; iRound < 100000; iRound++)
    {
        const fwCecFrame stRx = randomFrame();
        const uint8_t btPeer = (uint8_t)(rng() & 0x0F);
        const uint8_t btRequest = (uint8_t)rng();
        const uint8_t btReply = (rng() & 1u) ? cecMsgAt((int)(rng() % (uint32_t)cecMsgCount())).btOpcode : (uint8_t)rng();
        const fwCecMatch eMatch = cecMatchReply(stRx, btPeer, btRequest, btReply);
        if (eMatch == fwCecMatch::None)
            continue;
        CHECK(stRx.btLen >= 2);
        CHECK(btPeer == CEC_ADDR_BROADCAST || cecInitiator(stRx) == btPeer);
        if (eMatch == fwCecMatch::Reply)
            CHECK_EQ(cecOpcode(stRx), btReply);
        else
        {
            CHECK_EQ(cecOpcode(stRx), CEC_OP_FEATURE_ABORT);
            CHECK_EQ(cecOperands(stRx)[0], btRequest);
            CHECK(btReply != CEC_OP_FEATURE_ABORT);
        }
    }

    CHECK(strcmp(cecMatchName(fwCecMatch::Refused), "refused") == 0);
}

TEST(testControllerBuilders)
{
    fwCecFrame stFrame = {};

    CHECK(cecBuildRequestActiveSource(stFrame, 4));
    CHECK(sameFrame(stFrame, FRAME(0x4F, 0x85)));
    CHECK(cecBuildSetStreamPath(stFrame, 4, 0x1000));
    CHECK(sameFrame(stFrame, FRAME(0x4F, 0x86, 0x10, 0x00)));
    CHECK(cecBuildRoutingChange(stFrame, 0, 0x1000, 0x2100));
    CHECK(sameFrame(stFrame, FRAME(0x0F, 0x80, 0x10, 0x00, 0x21, 0x00)));
    CHECK(cecBuildRoutingInformation(stFrame, 5, 0x1100));
    CHECK(sameFrame(stFrame, FRAME(0x5F, 0x81, 0x11, 0x00)));

    CHECK(cecBuildUserControlPressed(stFrame, 4, 0, CEC_UI_VOLUME_UP));
    CHECK(sameFrame(stFrame, FRAME(0x40, 0x44, 0x41)));
    CHECK(cecBuildSystemAudioModeRequest(stFrame, 4, 5, 0x1000));
    CHECK(sameFrame(stFrame, FRAME(0x45, 0x70, 0x10, 0x00)));
    CHECK(cecBuildSystemAudioModeRequest(stFrame, 4, 5, CEC_PHYS_ADDR_INVALID));
    CHECK(sameFrame(stFrame, FRAME(0x45, 0x70)));

    CHECK(cecBuildSetOsdString(stFrame, 4, 0, CEC_DISPLAY_UNTIL_CLEARED, "Hi", 2));
    CHECK(sameFrame(stFrame, FRAME(0x40, 0x64, 0x40, 0x48, 0x69)));
    CHECK(cecBuildSetOsdString(stFrame, 4, 0, CEC_DISPLAY_CLEAR, nullptr, 0));
    CHECK(sameFrame(stFrame, FRAME(0x40, 0x64, 0x80)));
    CHECK(cecBuildSetOsdString(stFrame, 4, 0, CEC_DISPLAY_DEFAULT_TIME, "1234567890123", 13));
    CHECK_EQ(stFrame.btLen, CEC_FRAME_MAX);

    CHECK(cecBuildSetTimerProgramTitle(stFrame, 4, 1, "News", 4));
    CHECK(sameFrame(stFrame, FRAME(0x41, 0x67, 0x4E, 0x65, 0x77, 0x73)));
    CHECK(cecBuildSetTimerProgramTitle(stFrame, 4, 1, "12345678901234", 14));
    CHECK_EQ(stFrame.btLen, CEC_FRAME_MAX);

    const uint8_t abtData[11] = { 0x01, 0x02, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    CHECK(cecBuildVendorCommandWithId(stFrame, 4, 0, 0x0000F0, abtData, 2));
    CHECK(sameFrame(stFrame, FRAME(0x40, 0xA0, 0x00, 0x00, 0xF0, 0x01, 0x02)));
    CHECK(cecBuildVendorCommandWithId(stFrame, 4, CEC_ADDR_BROADCAST, 0x123456, nullptr, 0));
    CHECK(sameFrame(stFrame, FRAME(0x4F, 0xA0, 0x12, 0x34, 0x56)));
    CHECK(cecBuildVendorCommandWithId(stFrame, 4, 0, 0x123456, abtData, 11));
    CHECK_EQ(stFrame.btLen, CEC_FRAME_MAX);

    CHECK(cecBuildFeatureAbortTo(stFrame, 4, 0, CEC_OP_GIVE_DEVICE_POWER_STATUS, CEC_ABORT_REFUSED));
    CHECK(sameFrame(stFrame, FRAME(0x40, 0x00, 0x8F, 0x04)));

    // Refusals leave the frame as it was.
    const fwCecFrame stBefore = stFrame;
    CHECK(!cecBuildRequestActiveSource(stFrame, 16));
    CHECK(!cecBuildSetStreamPath(stFrame, 16, 0x1000));
    CHECK(!cecBuildUserControlPressed(stFrame, 4, CEC_ADDR_BROADCAST, 0x00));
    CHECK(!cecBuildUserControlPressed(stFrame, 4, 16, 0x00));
    CHECK(!cecBuildSystemAudioModeRequest(stFrame, 4, CEC_ADDR_BROADCAST, 0x1000));
    CHECK(!cecBuildSetOsdString(stFrame, 4, 0, CEC_DISPLAY_DEFAULT_TIME, "12345678901234", 14));
    CHECK(!cecBuildSetOsdString(stFrame, 4, 0, CEC_DISPLAY_DEFAULT_TIME, "", 0));
    CHECK(!cecBuildSetOsdString(stFrame, 4, 0, 0xC0, "Hi", 2));
    CHECK(!cecBuildSetOsdString(stFrame, 4, 0, CEC_DISPLAY_DEFAULT_TIME, "a\tb", 3));
    CHECK(!cecBuildSetOsdString(stFrame, 4, 0, CEC_DISPLAY_DEFAULT_TIME, nullptr, 2));
    CHECK(!cecBuildSetOsdString(stFrame, 4, CEC_ADDR_BROADCAST, CEC_DISPLAY_DEFAULT_TIME, "Hi", 2));
    CHECK(!cecBuildSetTimerProgramTitle(stFrame, 4, 1, "", 0));
    CHECK(!cecBuildSetTimerProgramTitle(stFrame, 4, 1, "123456789012345", 15));
    CHECK(!cecBuildVendorCommandWithId(stFrame, 4, 0, 0x1000000, abtData, 1));
    CHECK(!cecBuildVendorCommandWithId(stFrame, 4, 0, 0x123456, abtData, 12));
    CHECK(!cecBuildVendorCommandWithId(stFrame, 4, 0, 0x123456, nullptr, 1));
    CHECK(!cecBuildFeatureAbortTo(stFrame, 4, CEC_ADDR_BROADCAST, 0x8F, 0));
    CHECK(sameFrame(stFrame, stBefore));

    // What the builders make passes the frame check.
    fwCecFrame astBuilt[8];
    CHECK(cecBuildRequestActiveSource(astBuilt[0], 4));
    CHECK(cecBuildSetStreamPath(astBuilt[1], 4, 0x1234));
    CHECK(cecBuildRoutingChange(astBuilt[2], 4, 0x1000, 0x2000));
    CHECK(cecBuildRoutingInformation(astBuilt[3], 4, 0x1000));
    CHECK(cecBuildSystemAudioModeRequest(astBuilt[4], 4, 5, 0x1000));
    CHECK(cecBuildSetOsdString(astBuilt[5], 4, 0, CEC_DISPLAY_DEFAULT_TIME, "x", 1));
    CHECK(cecBuildVendorCommandWithId(astBuilt[6], 4, 0, 0x0000F0, abtData, 0));
    CHECK(cecBuildFeatureAbortTo(astBuilt[7], 4, 0, 0x46, 0));
    for (const fwCecFrame & stBuilt : astBuilt)
        CHECK_RES(cecCheck(stBuilt), fwCecCheck::Ok);
}

TEST(testParsers)
{
    uint16_t uiPhys = 0;
    uint8_t btType = 0;
    CHECK(cecParseReportPhysicalAddress(FRAME(0x0F, 0x84, 0x00, 0x00, 0x00), uiPhys, btType));
    CHECK_EQ(uiPhys, 0x0000);
    CHECK_EQ(btType, CEC_DEVTYPE_TV);
    CHECK(cecParseReportPhysicalAddress(FRAME(0x5F, 0x84, 0x12, 0x34, 0x05), uiPhys, btType));
    CHECK_EQ(uiPhys, 0x1234);
    CHECK_EQ(btType, CEC_DEVTYPE_AUDIO_SYSTEM);
    CHECK(!cecParseReportPhysicalAddress(FRAME(0x5F, 0x84, 0x12, 0x34), uiPhys, btType));
    CHECK(!cecParseReportPhysicalAddress(FRAME(0x5F, 0x87, 0x12, 0x34, 0x05), uiPhys, btType));

    uint32_t uiVendor = 0;
    CHECK(cecParseDeviceVendorId(FRAME(0x0F, 0x87, 0x00, 0xE0, 0x91), uiVendor));
    CHECK_EQ(uiVendor, 0x00E091u);
    CHECK(!cecParseDeviceVendorId(FRAME(0x0F, 0x87, 0x00, 0xE0), uiVendor));
    CHECK(!cecParseDeviceVendorId(FRAME(0x0F), uiVendor));

    char szName[CEC_OSD_NAME_MAX + 1];
    CHECK_EQ(cecParseOsdName(FRAME(0x04, 0x47, 0x54, 0x56), szName), 2);
    CHECK(strcmp(szName, "TV") == 0);
    CHECK_EQ(cecParseOsdName(FRAME(0x04, 0x47, 0x41, 0x0A, 0x42), szName), 3);
    CHECK(strcmp(szName, "A?B") == 0);
    CHECK_EQ(cecParseOsdName(FRAME(0x04, 0x47, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
                                   0x31, 0x32, 0x33, 0x34), szName), 14);
    CHECK(strcmp(szName, "12345678901234") == 0);
    CHECK_EQ(cecParseOsdName(FRAME(0x04, 0x47), szName), -1);
    CHECK_EQ(cecParseOsdName(FRAME(0x04, 0x46), szName), -1);
}

// A simulated bus answering the scan.
struct ScanBus
{
    bool     abPresent[16] = {};
    bool     abAnswer[16][CEC_SCAN_QUESTIONS] = {};
    bool     abAckQuestions[16] = {};
    int      iFrames = 0;
    fwCecFrame astSent[256];

    // The answer to stFrame from its destination, or false.
    bool answer(const fwCecFrame & stFrame, fwCecFrame & stReply) const
    {
        const uint8_t btFrom = cecDestination(stFrame);
        const uint8_t btTo = cecInitiator(stFrame);
        int iQuestion = -1;
        switch (cecOpcode(stFrame))
        {
            case CEC_OP_GIVE_PHYSICAL_ADDRESS:    iQuestion = CEC_SCAN_Q_PHYS_ADDR; break;
            case CEC_OP_GIVE_DEVICE_VENDOR_ID:    iQuestion = CEC_SCAN_Q_VENDOR_ID; break;
            case CEC_OP_GIVE_OSD_NAME:            iQuestion = CEC_SCAN_Q_OSD_NAME; break;
            case CEC_OP_GIVE_DEVICE_POWER_STATUS: iQuestion = CEC_SCAN_Q_POWER; break;
            case CEC_OP_GET_CEC_VERSION:          iQuestion = CEC_SCAN_Q_VERSION; break;
        }
        if (iQuestion < 0)
            return false;
        if (!abAnswer[btFrom][iQuestion])
        {
            const uint8_t abt[2] = { cecOpcode(stFrame), CEC_ABORT_UNRECOGNIZED };
            return cecBuild(stReply, btFrom, btTo, CEC_OP_FEATURE_ABORT, abt, 2);
        }
        switch (iQuestion)
        {
            case CEC_SCAN_Q_PHYS_ADDR:
            {
                const uint8_t abt[3] = { (uint8_t)(btFrom << 4), 0x00, btFrom == 0 ? (uint8_t)CEC_DEVTYPE_TV : (uint8_t)CEC_DEVTYPE_AUDIO_SYSTEM };
                return cecBuild(stReply, btFrom, CEC_ADDR_BROADCAST, CEC_OP_REPORT_PHYSICAL_ADDRESS, abt, 3);
            }
            case CEC_SCAN_Q_VENDOR_ID:
            {
                const uint8_t abt[3] = { 0x00, 0x00, (uint8_t)(0xF0 + btFrom) };
                return cecBuild(stReply, btFrom, CEC_ADDR_BROADCAST, CEC_OP_DEVICE_VENDOR_ID, abt, 3);
            }
            case CEC_SCAN_Q_OSD_NAME:
            {
                const uint8_t abt[3] = { 'D', 'e', (uint8_t)('0' + btFrom % 10) };
                return cecBuild(stReply, btFrom, btTo, CEC_OP_SET_OSD_NAME, abt, 3);
            }
            case CEC_SCAN_Q_POWER:
            {
                const uint8_t bt = CEC_POWER_STANDBY;
                return cecBuild(stReply, btFrom, btTo, CEC_OP_REPORT_POWER_STATUS, &bt, 1);
            }
            default:
            {
                const uint8_t bt = CEC_VERSION_1_4;
                return cecBuild(stReply, btFrom, btTo, CEC_OP_CEC_VERSION, &bt, 1);
            }
        }
    }

    // The scan run to its end against this bus, from address 4.
    void run(fwCecScan & stScan, uint16_t uiSkip)
    {
        cecScanStart(stScan, uiSkip);
        fwCecFrame stFrame;
        iFrames = 0;
        while (cecScanNext(stScan, 4, stFrame) && iFrames < 256)
        {
            astSent[iFrames++] = stFrame;
            const uint8_t btTo = cecDestination(stFrame);
            const bool bAcked = abPresent[btTo] && (cecIsPoll(stFrame) || abAckQuestions[btTo]);
            fwCecFrame stReply;
            const bool bReply = bAcked && !cecIsPoll(stFrame) && answer(stFrame, stReply);
            // The reply as matching passes it on: a refusal is not an answer.
            const bool bTaken = bReply && cecMatchReply(stReply, btTo, cecOpcode(stFrame),
                                                        (uint8_t)cecReplyOpcode(stFrame)) == fwCecMatch::Reply;
            cecScanTake(stScan, bAcked, bTaken ? &stReply : nullptr);
        }
    }
};

TEST(testScanSequence)
{
    ScanBus obBus;
    // The TV answers everything; an audio system answers its physical
    // address only; address 8 acknowledges the poll and nothing after it.
    obBus.abPresent[0] = true;
    obBus.abAckQuestions[0] = true;
    for (int i = 0; i < CEC_SCAN_QUESTIONS; i++)
        obBus.abAnswer[0][i] = true;
    obBus.abPresent[5] = true;
    obBus.abAckQuestions[5] = true;
    obBus.abAnswer[5][CEC_SCAN_Q_PHYS_ADDR] = true;
    obBus.abPresent[8] = true;
    obBus.abPresent[4] = true;          // this device: skipped

    static fwCecScan stScan;
    obBus.run(stScan, 1u << 4);

    // 14 polls, five questions each to 0 and 5, one to 8.
    CHECK_EQ(obBus.iFrames, 14 + 5 + 5 + 1);
    CHECK(sameFrame(obBus.astSent[0], FRAME(0x40)));
    CHECK(sameFrame(obBus.astSent[1], FRAME(0x40, 0x83)));
    CHECK(sameFrame(obBus.astSent[2], FRAME(0x40, 0x8C)));
    CHECK(sameFrame(obBus.astSent[3], FRAME(0x40, 0x46)));
    CHECK(sameFrame(obBus.astSent[4], FRAME(0x40, 0x8F)));
    CHECK(sameFrame(obBus.astSent[5], FRAME(0x40, 0x9F)));
    CHECK(sameFrame(obBus.astSent[6], FRAME(0x41)));
    CHECK(sameFrame(obBus.astSent[7], FRAME(0x42)));
    CHECK(sameFrame(obBus.astSent[8], FRAME(0x43)));
    CHECK(sameFrame(obBus.astSent[9], FRAME(0x45)));            // 4 skipped
    CHECK(sameFrame(obBus.astSent[10], FRAME(0x45, 0x83)));
    for (int i = 0; i < obBus.iFrames; i++)
        CHECK(cecDestination(obBus.astSent[i]) != 4 && cecDestination(obBus.astSent[i]) != CEC_ADDR_BROADCAST);

    CHECK_EQ(cecScanFoundMask(stScan), (1u << 0) | (1u << 5) | (1u << 8));

    const fwCecScanEntry & stTv = stScan.astEntry[0];
    CHECK(stTv.bPresent);
    CHECK_EQ(stTv.btAnswers, 0x1F);
    CHECK_EQ(stTv.uiPhysAddr, 0x0000);
    CHECK_EQ(stTv.btDeviceType, CEC_DEVTYPE_TV);
    CHECK_EQ(stTv.uiVendorId, 0x0000F0u);
    CHECK(strcmp(stTv.szName, "De0") == 0);
    CHECK_EQ(stTv.btPowerStatus, CEC_POWER_STANDBY);
    CHECK_EQ(stTv.btCecVersion, CEC_VERSION_1_4);

    const fwCecScanEntry & stAudio = stScan.astEntry[5];
    CHECK(stAudio.bPresent);
    CHECK_EQ(stAudio.btAnswers, 1u << CEC_SCAN_Q_PHYS_ADDR);
    CHECK_EQ(stAudio.uiPhysAddr, 0x5000);
    CHECK_EQ(stAudio.btDeviceType, CEC_DEVTYPE_AUDIO_SYSTEM);
    CHECK_EQ(stAudio.uiVendorId, CEC_VENDOR_ID_NONE);
    CHECK(stAudio.szName[0] == 0);
    CHECK_EQ(stAudio.btPowerStatus, 0xFF);

    const fwCecScanEntry & stMute = stScan.astEntry[8];
    CHECK(stMute.bPresent);
    CHECK_EQ(stMute.btAnswers, 0);
    CHECK(!stScan.astEntry[4].bPresent);
    CHECK(!stScan.astEntry[1].bPresent);

    // Over: nothing more to send, and a late outcome changes nothing.
    fwCecFrame stFrame;
    CHECK(!cecScanNext(stScan, 4, stFrame));
    cecScanTake(stScan, true, nullptr);
    CHECK_EQ(cecScanFoundMask(stScan), (1u << 0) | (1u << 5) | (1u << 8));

    // Every address skipped: nothing is sent.
    ScanBus obEmpty;
    obEmpty.run(stScan, 0x7FFF);
    CHECK_EQ(obEmpty.iFrames, 0);
    // An empty bus: 15 polls.
    obEmpty.run(stScan, 0);
    CHECK_EQ(obEmpty.iFrames, CEC_SCAN_ADDRESSES);
    CHECK_EQ(cecScanFoundMask(stScan), 0);

    // An answer from another device is not taken.
    cecScanStart(stScan, 0);
    cecScanTake(stScan, true, nullptr);                                     // 0 acknowledges its poll
    const fwCecFrame stOther = FRAME(0x5F, 0x84, 0x10, 0x00, 0x05);
    cecScanTake(stScan, true, &stOther);                                    // 5 answers instead
    CHECK_EQ(stScan.astEntry[0].btAnswers, 0);
    CHECK_EQ(stScan.astEntry[0].uiPhysAddr, CEC_PHYS_ADDR_INVALID);
}

// Random buses: the scan always ends, polls each address once, asks only a
// device that acknowledged, and records exactly the devices that did.
TEST(testScanFuzz)
{
    rngSeed(0xC0FFEE11u);
    static ScanBus obBus;
    static fwCecScan stScan;
    for (int iRound = 0; iRound < 2000; iRound++)
    {
        obBus = ScanBus();
        for (int a = 0; a < 16; a++)
        {
            obBus.abPresent[a] = (rng() & 3u) == 0;
            obBus.abAckQuestions[a] = (rng() & 3u) != 0;
            for (int q = 0; q < CEC_SCAN_QUESTIONS; q++)
                obBus.abAnswer[a][q] = (rng() & 1u) != 0;
        }
        const uint16_t uiSkip = (uint16_t)(rng() & 0x7FFF & rng());
        obBus.run(stScan, uiSkip);

        CHECK(obBus.iFrames < 256);
        int aiPolls[16] = {};
        uint16_t uiExpected = 0;
        for (int i = 0; i < obBus.iFrames; i++)
        {
            const fwCecFrame & st = obBus.astSent[i];
            const uint8_t btTo = cecDestination(st);
            CHECK(btTo < CEC_SCAN_ADDRESSES);
            CHECK(((uiSkip >> btTo) & 1u) == 0);
            CHECK_EQ(cecInitiator(st), 4);
            if (cecIsPoll(st))
                aiPolls[btTo]++;
            else
            {
                CHECK(obBus.abPresent[btTo]);
                CHECK_RES(cecCheck(st), fwCecCheck::Ok);
            }
        }
        for (int a = 0; a < CEC_SCAN_ADDRESSES; a++)
        {
            const bool bSkipped = ((uiSkip >> a) & 1u) != 0;
            CHECK_EQ(aiPolls[a], bSkipped ? 0 : 1);
            if (!bSkipped && obBus.abPresent[a])
                uiExpected = (uint16_t)(uiExpected | (1u << a));
            const fwCecScanEntry & st = stScan.astEntry[a];
            if (st.btAnswers != 0)
                CHECK(obBus.abPresent[a] && obBus.abAckQuestions[a]);
            for (int q = 0; q < CEC_SCAN_QUESTIONS; q++)
                if ((st.btAnswers >> q) & 1u)
                    CHECK(obBus.abAnswer[a][q]);
            if (!bSkipped && obBus.abPresent[a] && obBus.abAckQuestions[a])
            {
                for (int q = 0; q < CEC_SCAN_QUESTIONS; q++)
                    CHECK_EQ(((st.btAnswers >> q) & 1u) != 0, obBus.abAnswer[a][q]);
            }
        }
        CHECK_EQ(cecScanFoundMask(stScan), uiExpected);
    }
}

void runControllerTests()
{
    testReplyOpcodes();
    testMatchReply();
    testControllerBuilders();
    testParsers();
    testScanSequence();
    testScanFuzz();
}
