// ew_scores.h — Emberward's high-score table and settings, and their file
// format on the SD card. Pure C, host-tested in tests/test_ew_scores.c.
#ifndef EW_SCORES_H
#define EW_SCORES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EW_SCORES       5
#define EW_NAME_LEN     3
#define EW_SAVE_VERSION 1
#define EW_SAVE_BYTES   52

typedef struct {
    char name[EW_NAME_LEN + 1];   // A-Z, 0-9 or space, NUL-terminated
    uint32_t score;
    uint8_t wave;
} ew_score_t;

typedef struct {
    ew_score_t entry[EW_SCORES];  // best first
    uint8_t sound;                // 1 = sound on
    uint8_t tv_sound;             // 1 = sound on the TV (audio framing) while a TV is connected
} ew_save_t;

// The table a fresh card starts with, sound on, TV sound on.
void ew_save_defaults(ew_save_t *s);

// The place (0 = best) a score would take, or -1 if it does not make the table.
// A tie goes below the existing entry.
int ew_scores_rank(const ew_save_t *s, uint32_t score);

// Inserts the score and returns its place, or -1 (table unchanged).
int ew_scores_insert(ew_save_t *s, const char *name, uint32_t score, uint8_t wave);

// True for the characters a name may hold.
bool ew_name_char_ok(char c);

// The next / previous name character in the entry order (A..Z, 0..9, space).
char ew_name_char_step(char c, int delta);

// File image: "EWHS", version, sound, a byte written 0 and ignored on read
// (files from before the BSP kept the TV input held it there), TV sound off
// (1) or on (0: what files from before the setting hold), five entries of name (3), wave
// (1), score (4, little-endian), then FNV-1a 32 of everything before it.
void ew_save_encode(const ew_save_t *s, uint8_t out[EW_SAVE_BYTES]);

// False (and s untouched) for a short, foreign, damaged or out-of-range image.
bool ew_save_decode(ew_save_t *s, const uint8_t *in, size_t len);

#endif
