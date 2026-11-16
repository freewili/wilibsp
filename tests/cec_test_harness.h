// Shared by the test files of cec_tests: the counters, the CHECK macros, the
// seeded random source and the frame helpers.
#pragma once

#include "fwCECCore.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

inline int s_iFailures = 0;
inline int s_iChecks = 0;
inline int s_iTests = 0;

#define CHECK(cond) do { \
    s_iChecks++; \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        s_iFailures++; \
    } \
} while (0)

// For checks inside loops over the table: report the opcode.
#define CHECK_OP(cond, opcode) do { \
    s_iChecks++; \
    if (!(cond)) { \
        printf("FAIL %s:%d (opcode 0x%02X): %s\n", __FILE__, __LINE__, (unsigned)(opcode), #cond); \
        s_iFailures++; \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    s_iChecks++; \
    auto _va = (a); auto _vb = (b); \
    if (!(_va == _vb)) { \
        printf("FAIL %s:%d: %s == %s (%llu vs %llu)\n", __FILE__, __LINE__, #a, #b, \
               (unsigned long long)_va, (unsigned long long)_vb); \
        s_iFailures++; \
    } \
} while (0)

#define CHECK_RES(a, b) CHECK_EQ((int)(a), (int)(b))

#define TEST(name) static void name(); \
    struct name##_reg { name##_reg() { s_iTests++; } } name##_reg_inst; \
    static void name()

// The wire tests (test_wire.cpp).
void runWireTests();
// The controller tests (test_controller.cpp).
void runControllerTests();
// The receive queue tests (test_rx_queue.cpp).
void runRxQueueTests();
// The held remote key tests (test_key_hold.cpp).
void runKeyHoldTests();

// xorshift32: deterministic across compilers, seeded per test.
inline uint32_t s_uiRng = 1;
inline void rngSeed(uint32_t uiSeed) { s_uiRng = uiSeed ? uiSeed : 1; }
inline uint32_t rng()
{
    uint32_t x = s_uiRng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_uiRng = x;
    return x;
}

// A frame from literal blocks.
inline fwCecFrame frameOf(const uint8_t * pBlocks, int iLen)
{
    fwCecFrame stFrame = {};
    stFrame.btLen = (uint8_t)iLen;
    for (int i = 0; i < iLen && i < CEC_FRAME_MAX; i++)
        stFrame.abtBlock[i] = pBlocks[i];
    return stFrame;
}
#define FRAME(...) ([]{ static const uint8_t a[] = { __VA_ARGS__ }; return frameOf(a, (int)sizeof(a)); }())

// Random length, random blocks.
inline fwCecFrame randomFrame()
{
    fwCecFrame stFrame = {};
    stFrame.btLen = (uint8_t)(1 + rng() % CEC_FRAME_MAX);
    for (int i = 0; i < stFrame.btLen; i++)
        stFrame.abtBlock[i] = (uint8_t)rng();
    return stFrame;
}

inline bool sameFrame(const fwCecFrame & stA, const fwCecFrame & stB)
{
    return stA.btLen == stB.btLen && memcmp(stA.abtBlock, stB.abtBlock, stA.btLen) == 0;
}
