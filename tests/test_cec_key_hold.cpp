// The held remote key of fwCECCore: the press step, the repeat delay, the
// repeat and fast rates, keys that do not repeat, the release and the wrap of
// the millisecond clock.

#include "cec_test_harness.h"

namespace {

// The keypad's auto-repeat (rpButtonStateManager.h BUTTON_REPEAT_*).
const fwCecKeyTiming KEYPAD = { 450, 120, 60, 8 };

// Repeat steps due between uiFrom and uiTo, polled every millisecond.
int stepsBetween(fwCecKeyHold & stHold, uint32_t uiFrom, uint32_t uiTo)
{
    int iSteps = 0;
    for (uint32_t t = uiFrom; t != uiTo; t++)
        if (cecKeyHoldDue(stHold, t, KEYPAD))
            iSteps++;
    return iSteps;
}

}

// A tap: no repeat before the delay, the release owed once.
TEST(testKeyHoldTap)
{
    fwCecKeyHold stHold;
    cecKeyHoldReset(stHold);
    CHECK(!cecKeyHoldRelease(stHold));
    cecKeyHoldPress(stHold, CEC_UI_DOWN, true, 1000, KEYPAD);
    CHECK(stHold.bHeld);
    CHECK_EQ(stepsBetween(stHold, 1000, 1449), 0);
    CHECK(cecKeyHoldRelease(stHold));
    CHECK(!cecKeyHoldRelease(stHold));
    CHECK_EQ(stepsBetween(stHold, 1449, 3000), 0);
}

// Held: the first repeat at the delay, then the repeat rate, then the fast rate.
TEST(testKeyHoldRepeat)
{
    fwCecKeyHold stHold;
    cecKeyHoldReset(stHold);
    cecKeyHoldPress(stHold, CEC_UI_DOWN, true, 0, KEYPAD);
    CHECK(!cecKeyHoldDue(stHold, 449, KEYPAD));
    CHECK(cecKeyHoldDue(stHold, 450, KEYPAD));
    CHECK(!cecKeyHoldDue(stHold, 450, KEYPAD));
    CHECK(!cecKeyHoldDue(stHold, 569, KEYPAD));
    CHECK(cecKeyHoldDue(stHold, 570, KEYPAD));
    // Repeats 3..8 at 120 ms, from 9 on at 60 ms.
    CHECK_EQ(stepsBetween(stHold, 571, 571 + 6 * 120), 6);
    CHECK_EQ(stHold.btRepeatCount, 8);
    CHECK_EQ(stepsBetween(stHold, 571 + 6 * 120, 571 + 6 * 120 + 600), 10);
    CHECK(cecKeyHoldRelease(stHold));
    CHECK(!cecKeyHoldDue(stHold, 100000, KEYPAD));
}

// A key that does not repeat steps once however long it is held.
TEST(testKeyHoldNoRepeat)
{
    fwCecKeyHold stHold;
    cecKeyHoldReset(stHold);
    cecKeyHoldPress(stHold, CEC_UI_SELECT, false, 0, KEYPAD);
    CHECK_EQ(stepsBetween(stHold, 0, 5000), 0);
    CHECK(cecKeyHoldRelease(stHold));
}

// A new press restarts the delay.
TEST(testKeyHoldNewPress)
{
    fwCecKeyHold stHold;
    cecKeyHoldReset(stHold);
    cecKeyHoldPress(stHold, CEC_UI_DOWN, true, 0, KEYPAD);
    CHECK(cecKeyHoldDue(stHold, 450, KEYPAD));
    cecKeyHoldPress(stHold, CEC_UI_UP, true, 500, KEYPAD);
    CHECK_EQ(stHold.btKey, CEC_UI_UP);
    CHECK_EQ(stepsBetween(stHold, 500, 950), 0);
    CHECK(cecKeyHoldDue(stHold, 950, KEYPAD));
}

// The millisecond clock wrapping under a hold.
TEST(testKeyHoldWrap)
{
    fwCecKeyHold stHold;
    cecKeyHoldReset(stHold);
    const uint32_t uiStart = 0xFFFFFF00u;
    cecKeyHoldPress(stHold, CEC_UI_DOWN, true, uiStart, KEYPAD);
    CHECK(!cecKeyHoldDue(stHold, 0xFFFFFFFFu, KEYPAD));
    CHECK(!cecKeyHoldDue(stHold, uiStart + 449, KEYPAD));
    CHECK(cecKeyHoldDue(stHold, uiStart + 450, KEYPAD));
}

void runKeyHoldTests()
{
    testKeyHoldTap();
    testKeyHoldRepeat();
    testKeyHoldNoRepeat();
    testKeyHoldNewPress();
    testKeyHoldWrap();
}
