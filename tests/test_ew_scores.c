// Host tests for apps/emberward/ew_scores.c: the high-score table and its
// file format.
#include "ew_scores.h"
#include "test_util.h"

#include <string.h>

static void test_defaults_sorted(void) {
    ew_save_t s;
    ew_save_defaults(&s);
    ASSERT_EQ(s.sound, 1);
    ASSERT_EQ(s.tv_sound, 1);
    for (int i = 1; i < EW_SCORES; i++) ASSERT_TRUE(s.entry[i - 1].score >= s.entry[i].score);
}

static void test_rank(void) {
    ew_save_t s;
    ew_save_defaults(&s);
    ASSERT_EQ(ew_scores_rank(&s, 0), -1);
    ASSERT_EQ(ew_scores_rank(&s, s.entry[4].score), -1);       // a tie goes below
    ASSERT_EQ(ew_scores_rank(&s, s.entry[4].score + 1), 4);
    ASSERT_EQ(ew_scores_rank(&s, s.entry[0].score + 1), 0);
    ASSERT_EQ(ew_scores_rank(&s, s.entry[2].score), 3);
}

static void test_insert(void) {
    ew_save_t s;
    ew_save_defaults(&s);
    const uint32_t old_top = s.entry[0].score, old_second = s.entry[1].score;
    ASSERT_EQ(ew_scores_insert(&s, "ZED", old_top + 10, 9), 0);
    ASSERT_TRUE(strcmp(s.entry[0].name, "ZED") == 0);
    ASSERT_EQ(s.entry[0].score, old_top + 10);
    ASSERT_EQ(s.entry[0].wave, 9);
    ASSERT_EQ(s.entry[1].score, old_top);
    ASSERT_EQ(s.entry[2].score, old_second);
    // Not good enough: unchanged.
    ew_save_t before = s;
    ASSERT_EQ(ew_scores_insert(&s, "LOW", 1, 1), -1);
    ASSERT_TRUE(memcmp(&before, &s, sizeof s) == 0);
    // Short and bad names are padded and cleaned.
    ASSERT_EQ(ew_scores_insert(&s, "a", old_top + 20, 2), 0);
    ASSERT_TRUE(strcmp(s.entry[0].name, "   ") == 0);
    ASSERT_EQ(ew_scores_insert(&s, "Q", old_top + 30, 2), 0);
    ASSERT_TRUE(strcmp(s.entry[0].name, "Q  ") == 0);
    ASSERT_EQ(ew_scores_insert(&s, 0, old_top + 40, 2), 0);
    ASSERT_TRUE(strcmp(s.entry[0].name, "   ") == 0);
}

static void test_name_chars(void) {
    ASSERT_EQ(ew_name_char_step('A', 1), 'B');
    ASSERT_EQ(ew_name_char_step('Z', 1), '0');
    ASSERT_EQ(ew_name_char_step('9', 1), ' ');
    ASSERT_EQ(ew_name_char_step(' ', 1), 'A');
    ASSERT_EQ(ew_name_char_step('A', -1), ' ');
    ASSERT_TRUE(ew_name_char_ok('M') && ew_name_char_ok('5') && ew_name_char_ok(' '));
    ASSERT_TRUE(!ew_name_char_ok('a') && !ew_name_char_ok('!') && !ew_name_char_ok(0));
}

static void test_round_trip(void) {
    ew_save_t s, t;
    ew_save_defaults(&s);
    ew_scores_insert(&s, "ABC", 123456, 12);
    s.sound = 0;
    s.tv_sound = 0;
    uint8_t buf[EW_SAVE_BYTES];
    ew_save_encode(&s, buf);
    ASSERT_TRUE(memcmp(buf, "EWHS", 4) == 0);
    ASSERT_EQ(buf[4], EW_SAVE_VERSION);
    memset(&t, 0, sizeof t);
    ASSERT_TRUE(ew_save_decode(&t, buf, sizeof buf));
    ASSERT_EQ(t.sound, 0);
    ASSERT_EQ(t.tv_sound, 0);
    s.tv_sound = 1;
    ew_save_encode(&s, buf);
    ASSERT_EQ(buf[7], 0);   // a file from before the setting reads as TV sound on
    ASSERT_TRUE(ew_save_decode(&t, buf, sizeof buf));
    ASSERT_EQ(t.tv_sound, 1);
    for (int i = 0; i < EW_SCORES; i++) {
        ASSERT_TRUE(strcmp(t.entry[i].name, s.entry[i].name) == 0);
        ASSERT_EQ(t.entry[i].score, s.entry[i].score);
        ASSERT_EQ(t.entry[i].wave, s.entry[i].wave);
    }
}

static void test_decode_rejects(void) {
    ew_save_t s, t;
    ew_save_defaults(&s);
    uint8_t buf[EW_SAVE_BYTES], bad[EW_SAVE_BYTES];
    ew_save_encode(&s, buf);
    ew_save_defaults(&t);
    t.sound = 0;
    const ew_save_t keep = t;

    ASSERT_TRUE(!ew_save_decode(&t, buf, sizeof buf - 1));     // short
    ASSERT_TRUE(!ew_save_decode(&t, 0, sizeof buf));
    memcpy(bad, buf, sizeof bad);
    bad[0] = 'X';                                              // foreign
    ASSERT_TRUE(!ew_save_decode(&t, bad, sizeof bad));
    memcpy(bad, buf, sizeof bad);
    bad[20] ^= 1;                                              // damaged
    ASSERT_TRUE(!ew_save_decode(&t, bad, sizeof bad));
    ASSERT_TRUE(memcmp(&t, &keep, sizeof t) == 0);             // untouched

    // Well-formed checksum over out-of-range contents is still refused.
    ew_save_t odd;
    ew_save_encode(&s, bad);
    bad[7] = 2;                                                // TV sound out of range
    {
        uint32_t h = 2166136261u;
        for (int i = 0; i < EW_SAVE_BYTES - 4; i++) { h ^= bad[i]; h *= 16777619u; }
        for (int i = 0; i < 4; i++) bad[EW_SAVE_BYTES - 4 + i] = (uint8_t)(h >> (8 * i));
    }
    ASSERT_TRUE(!ew_save_decode(&t, bad, sizeof bad));
    odd = s;
    odd.entry[0].name[1] = 'a';
    ew_save_encode(&odd, bad);
    ASSERT_TRUE(!ew_save_decode(&t, bad, sizeof bad));
    odd = s;
    odd.entry[3].score = odd.entry[0].score + 1;               // not best first
    ew_save_encode(&odd, bad);
    ASSERT_TRUE(!ew_save_decode(&t, bad, sizeof bad));
}

int main(void) {
    test_defaults_sorted();
    test_rank();
    test_insert();
    test_name_chars();
    test_round_trip();
    test_decode_rejects();
    printf("test_ew_scores: %s\n", g_failures ? "FAIL" : "ok");
    TEST_RETURN();
}
