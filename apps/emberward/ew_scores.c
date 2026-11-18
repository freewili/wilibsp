// ew_scores.c — high-score table and its file format.
#include "ew_scores.h"

#include <string.h>

static const char NAME_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
#define NAME_CHAR_COUNT ((int)(sizeof NAME_CHARS - 1))

static const ew_score_t DEFAULTS[EW_SCORES] = {
    { "EMB", 6000, 6 }, { "WRD", 4000, 5 }, { "GLM", 2500, 4 }, { "COR", 1200, 3 }, { "SPK", 500, 2 },
};

void ew_save_defaults(ew_save_t *s) {
    memcpy(s->entry, DEFAULTS, sizeof DEFAULTS);
    s->sound = 1;
    s->tv_sound = 1;
}

int ew_scores_rank(const ew_save_t *s, uint32_t score) {
    if (score == 0) return -1;
    for (int i = 0; i < EW_SCORES; i++)
        if (score > s->entry[i].score) return i;
    return -1;
}

bool ew_name_char_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ';
}

char ew_name_char_step(char c, int delta) {
    int at = 0;
    for (int i = 0; i < NAME_CHAR_COUNT; i++)
        if (NAME_CHARS[i] == c) at = i;
    at = (at + delta) % NAME_CHAR_COUNT;
    if (at < 0) at += NAME_CHAR_COUNT;
    return NAME_CHARS[at];
}

int ew_scores_insert(ew_save_t *s, const char *name, uint32_t score, uint8_t wave) {
    const int rank = ew_scores_rank(s, score);
    if (rank < 0) return -1;
    memmove(&s->entry[rank + 1], &s->entry[rank], sizeof(ew_score_t) * (size_t)(EW_SCORES - 1 - rank));
    ew_score_t *e = &s->entry[rank];
    for (int i = 0; i < EW_NAME_LEN; i++) {
        const char c = name && name[i] ? name[i] : ' ';
        e->name[i] = ew_name_char_ok(c) ? c : ' ';
        if (!name || !name[i]) name = 0;
    }
    e->name[EW_NAME_LEN] = '\0';
    e->score = score;
    e->wave = wave;
    return rank;
}

static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void ew_save_encode(const ew_save_t *s, uint8_t out[EW_SAVE_BYTES]) {
    memset(out, 0, EW_SAVE_BYTES);
    memcpy(out, "EWHS", 4);
    out[4] = EW_SAVE_VERSION;
    out[5] = s->sound ? 1 : 0;
    out[7] = s->tv_sound ? 0 : 1;
    uint8_t *p = out + 8;
    for (int i = 0; i < EW_SCORES; i++, p += 8) {
        memcpy(p, s->entry[i].name, EW_NAME_LEN);
        p[3] = s->entry[i].wave;
        put32(p + 4, s->entry[i].score);
    }
    put32(out + EW_SAVE_BYTES - 4, fnv1a(out, EW_SAVE_BYTES - 4));
}

bool ew_save_decode(ew_save_t *s, const uint8_t *in, size_t len) {
    if (!in || len != EW_SAVE_BYTES) return false;
    if (memcmp(in, "EWHS", 4) != 0 || in[4] != EW_SAVE_VERSION) return false;
    if (get32(in + EW_SAVE_BYTES - 4) != fnv1a(in, EW_SAVE_BYTES - 4)) return false;
    if (in[5] > 1 || in[7] > 1) return false;
    ew_save_t t;
    const uint8_t *p = in + 8;
    uint32_t prev = 0xFFFFFFFFu;
    for (int i = 0; i < EW_SCORES; i++, p += 8) {
        for (int c = 0; c < EW_NAME_LEN; c++) {
            if (!ew_name_char_ok((char)p[c])) return false;
            t.entry[i].name[c] = (char)p[c];
        }
        t.entry[i].name[EW_NAME_LEN] = '\0';
        t.entry[i].wave = p[3];
        t.entry[i].score = get32(p + 4);
        if (t.entry[i].score > prev) return false;   // the table is kept best first
        prev = t.entry[i].score;
    }
    t.sound = in[5];
    t.tv_sound = in[7] ? 0 : 1;
    *s = t;
    return true;
}
