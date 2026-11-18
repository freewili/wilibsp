// The receive queue of fwCECCore: the mode filter, the order, the drop of the
// oldest when full, the lost count and the reset.

#include "cec_test_harness.h"

namespace {

const uint8_t FOR_US = CEC_RX_FOR_US | CEC_RX_ACKED;
const uint8_t OTHERS = CEC_RX_ACKED;

}

// What each mode keeps.
TEST(testRxQueueWants)
{
    const fwCecFrame stKeyDown  = FRAME(0x04, CEC_OP_USER_CONTROL_PRESSED, 0x01);
    const fwCecFrame stKeyUp    = FRAME(0x04, CEC_OP_USER_CONTROL_RELEASED);
    const fwCecFrame stStandby  = FRAME(0x0F, CEC_OP_STANDBY);
    const fwCecFrame stPoll     = FRAME(0x04);
    const fwCecFrame stOther    = FRAME(0x05, CEC_OP_GIVE_DEVICE_POWER_STATUS);

    CHECK(!cecRxQueueWants(fwCecRxQueueMode::Off, stKeyDown, FOR_US));

    CHECK(cecRxQueueWants(fwCecRxQueueMode::Ours, stKeyDown, FOR_US));
    CHECK(cecRxQueueWants(fwCecRxQueueMode::Ours, stStandby, FOR_US));
    CHECK(!cecRxQueueWants(fwCecRxQueueMode::Ours, stOther, OTHERS));

    CHECK(cecRxQueueWants(fwCecRxQueueMode::All, stOther, OTHERS));
    CHECK(cecRxQueueWants(fwCecRxQueueMode::All, stOther, 0));
    CHECK(cecRxQueueWants(fwCecRxQueueMode::All, stStandby, FOR_US));

    CHECK(cecRxQueueWants(fwCecRxQueueMode::Keys, stKeyDown, FOR_US));
    CHECK(cecRxQueueWants(fwCecRxQueueMode::Keys, stKeyUp, FOR_US));
    CHECK(!cecRxQueueWants(fwCecRxQueueMode::Keys, stStandby, FOR_US));
    CHECK(!cecRxQueueWants(fwCecRxQueueMode::Keys, stKeyDown, OTHERS));
    // A key to every device is not a key for this one.
    CHECK(!cecRxQueueWants(fwCecRxQueueMode::Keys, FRAME(0x0F, CEC_OP_USER_CONTROL_PRESSED, 0x01), FOR_US));

    // A poll is never kept; nor is a frame of no blocks or too many.
    for (int iMode = 0; iMode <= CEC_RX_QUEUE_MODE_LAST; iMode++)
    {
        CHECK(!cecRxQueueWants((fwCecRxQueueMode)iMode, stPoll, FOR_US));
        fwCecFrame stBad = stKeyDown;
        stBad.btLen = 0;
        CHECK(!cecRxQueueWants((fwCecRxQueueMode)iMode, stBad, FOR_US));
        stBad.btLen = CEC_FRAME_MAX + 1;
        CHECK(!cecRxQueueWants((fwCecRxQueueMode)iMode, stBad, FOR_US));
    }
}

// Off without a ring: nothing kept, nothing counted, nothing popped.
TEST(testRxQueueOff)
{
    fwCecRxQueue stQueue;
    cecRxQueueReset(stQueue, nullptr, fwCecRxQueueMode::All);
    CHECK(stQueue.eMode == fwCecRxQueueMode::Off);
    CHECK(!cecRxQueuePush(stQueue, FRAME(0x04, CEC_OP_STANDBY), FOR_US, 1));
    cecRxQueueLost(stQueue, 5);
    CHECK_EQ(stQueue.uiDropped, 0u);
    fwCecRxRecord stOut;
    CHECK(!cecRxQueuePop(stQueue, stOut));
}

// First in, first out, with the flags and time of each.
TEST(testRxQueueOrder)
{
    fwCecRxRecord astRing[CEC_RX_QUEUE_DEPTH];
    fwCecRxQueue stQueue;
    cecRxQueueReset(stQueue, astRing, fwCecRxQueueMode::All);
    CHECK(cecRxQueuePush(stQueue, FRAME(0x04, CEC_OP_USER_CONTROL_PRESSED, 0x01), FOR_US, 100));
    CHECK(cecRxQueuePush(stQueue, FRAME(0x05, CEC_OP_STANDBY), OTHERS, 200));
    CHECK(!cecRxQueuePush(stQueue, FRAME(0x04), FOR_US, 300));
    CHECK_EQ(stQueue.iCount, 2);

    fwCecRxRecord stOut;
    CHECK(cecRxQueuePop(stQueue, stOut));
    CHECK(sameFrame(stOut.stFrame, FRAME(0x04, CEC_OP_USER_CONTROL_PRESSED, 0x01)));
    CHECK_EQ(stOut.uiTimestampUs, 100u);
    CHECK_EQ(stOut.btFlags, FOR_US);
    CHECK_EQ(stQueue.iCount, 1);
    CHECK(cecRxQueuePop(stQueue, stOut));
    CHECK(sameFrame(stOut.stFrame, FRAME(0x05, CEC_OP_STANDBY)));
    CHECK_EQ(stOut.uiTimestampUs, 200u);
    CHECK(!cecRxQueuePop(stQueue, stOut));
    CHECK_EQ(stQueue.uiDropped, 0u);
}

// Full: the oldest goes and is counted; the newest DEPTH remain, in order.
TEST(testRxQueueOverflow)
{
    fwCecRxRecord astRing[CEC_RX_QUEUE_DEPTH];
    fwCecRxQueue stQueue;
    cecRxQueueReset(stQueue, astRing, fwCecRxQueueMode::Ours);
    const int iPushed = CEC_RX_QUEUE_DEPTH + 7;
    for (int i = 0; i < iPushed; i++)
    {
        const uint8_t abtBlocks[3] = { 0x04, CEC_OP_USER_CONTROL_PRESSED, (uint8_t)i };
        CHECK(cecRxQueuePush(stQueue, frameOf(abtBlocks, 3), FOR_US, (uint32_t)i));
        CHECK(stQueue.iCount <= CEC_RX_QUEUE_DEPTH);
    }
    CHECK_EQ(stQueue.iCount, CEC_RX_QUEUE_DEPTH);
    CHECK_EQ(stQueue.uiDropped, 7u);

    // Interleaved pops and pushes past the wrap keep the order.
    fwCecRxRecord stOut;
    int iNext = 7;
    for (int i = 0; i < 5; i++)
    {
        CHECK(cecRxQueuePop(stQueue, stOut));
        CHECK_EQ(stOut.stFrame.abtBlock[2], (uint8_t)iNext);
        CHECK_EQ(stOut.uiTimestampUs, (uint32_t)iNext);
        iNext++;
    }
    for (int i = iPushed; i < iPushed + 5; i++)
    {
        const uint8_t abtBlocks[3] = { 0x04, CEC_OP_USER_CONTROL_PRESSED, (uint8_t)i };
        CHECK(cecRxQueuePush(stQueue, frameOf(abtBlocks, 3), FOR_US, (uint32_t)i));
    }
    CHECK_EQ(stQueue.uiDropped, 7u);
    while (cecRxQueuePop(stQueue, stOut))
    {
        CHECK_EQ(stOut.stFrame.abtBlock[2], (uint8_t)iNext);
        iNext++;
    }
    CHECK_EQ(iNext, iPushed + 5);
    CHECK_EQ(stQueue.iCount, 0);
}

// The lost count adds to the drops; a mode change keeps the contents; a reset
// empties the queue and zeroes the drops.
TEST(testRxQueueLostAndReset)
{
    fwCecRxRecord astRing[CEC_RX_QUEUE_DEPTH];
    fwCecRxQueue stQueue;
    cecRxQueueReset(stQueue, astRing, fwCecRxQueueMode::Ours);
    cecRxQueueLost(stQueue, 3);
    cecRxQueueLost(stQueue, 0);
    CHECK_EQ(stQueue.uiDropped, 3u);

    CHECK(cecRxQueuePush(stQueue, FRAME(0x0F, CEC_OP_STANDBY), FOR_US, 1));
    stQueue.eMode = fwCecRxQueueMode::Keys;
    CHECK(!cecRxQueuePush(stQueue, FRAME(0x0F, CEC_OP_STANDBY), FOR_US, 2));
    CHECK_EQ(stQueue.iCount, 1);

    cecRxQueueReset(stQueue, astRing, fwCecRxQueueMode::All);
    CHECK_EQ(stQueue.iCount, 0);
    CHECK_EQ(stQueue.uiDropped, 0u);
    fwCecRxRecord stOut;
    CHECK(!cecRxQueuePop(stQueue, stOut));
}

// Random pushes and pops against a model of the queue.
TEST(testRxQueueFuzz)
{
    rngSeed(0xCEC5u);
    fwCecRxRecord astRing[CEC_RX_QUEUE_DEPTH];
    fwCecRxQueue stQueue;
    cecRxQueueReset(stQueue, astRing, fwCecRxQueueMode::All);

    uint32_t auiModel[4096];
    int iModelHead = 0, iModelTail = 0;
    uint32_t uiDropped = 0;
    uint32_t uiStamp = 0;
    for (int iStep = 0; iStep < 4000; iStep++)
    {
        if (rng() % 3 != 0)
        {
            fwCecFrame stFrame = randomFrame();
            const bool bKept = cecRxQueuePush(stQueue, stFrame, OTHERS, uiStamp);
            CHECK_EQ(bKept, stFrame.btLen >= 2);
            if (bKept)
            {
                if (iModelTail - iModelHead == CEC_RX_QUEUE_DEPTH)
                {
                    iModelHead++;
                    uiDropped++;
                }
                auiModel[iModelTail++ % 4096] = uiStamp;
            }
            uiStamp++;
        }
        else
        {
            fwCecRxRecord stOut;
            const bool bPopped = cecRxQueuePop(stQueue, stOut);
            CHECK_EQ(bPopped, iModelTail != iModelHead);
            if (bPopped)
                CHECK_EQ(stOut.uiTimestampUs, auiModel[iModelHead++ % 4096]);
        }
        CHECK_EQ(stQueue.iCount, iModelTail - iModelHead);
        CHECK_EQ(stQueue.uiDropped, uiDropped);
    }
}

void runRxQueueTests()
{
    testRxQueueWants();
    testRxQueueOff();
    testRxQueueOrder();
    testRxQueueOverflow();
    testRxQueueLostAndReset();
    testRxQueueFuzz();
}
