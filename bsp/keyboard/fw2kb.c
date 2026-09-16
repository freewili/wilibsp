#include "fw2kb.h"
#include <string.h>

/* Page tables ported verbatim from rpFw2KeyBoard.cpp (stMenuLevels).
 * 6-byte slots preserve the original zero padding: short groups ("F", "/z",
 * "") read as '\0' beyond their end, exactly like the firmware struct. */
typedef struct { char s[6]; } fw2kb_group;

static const fw2kb_group k_pages[5][5] = {
    { {"ABCDE"}, {"FGHIJ"}, {"KLMNO"}, {"PQRST"}, {"UVWXY"} },
    { {"abcde"}, {"fghij"}, {"klmno"}, {"pqrst"}, {"uvwxy"} },
    { {"01234"}, {"56789"}, {"+^.<>"}, {"=*/~-"}, {"()[]z"} },
    { {"!@#$%"}, {"^&_`~"}, {"{}\\|;"}, {"\"',=?"}, {"/z"}   },
    { {"01234"}, {"56789"}, {"ABCDE"}, {"F"},     {""}      },
};

enum { PAGE_UPPER = 0, PAGE_LOWER, PAGE_NUMBERS, PAGE_SYMBOLS, PAGE_HEX };

/* internal: shared with fw2kb_hidmap.c */
void fw2kb_push_event(fw2kb_t *kb, fw2kb_key key, char ch)
{
    if (kb->ring_count == FW2KB_EVENT_RING) {           /* drop oldest */
        kb->ring_head = (kb->ring_head + 1) % FW2KB_EVENT_RING;
        kb->ring_count--;
    }
    int tail = (kb->ring_head + kb->ring_count) % FW2KB_EVENT_RING;
    kb->ring[tail].key = key;
    kb->ring[tail].ch = ch;
    kb->ring_count++;
}

void fw2kb_init(fw2kb_t *kb)
{
    memset(kb, 0, sizeof *kb);
    kb->touch_threshold = 260;
    fw2kb_reset(kb);
}

void fw2kb_reset(fw2kb_t *kb)
{
    kb->group_state = true;
    kb->sub = 0;
    fw2kb_set_mode(kb, FW2KB_MODE_ALL);
}

void fw2kb_set_mode(fw2kb_t *kb, fw2kb_mode mode)
{
    kb->mode = mode;
    switch (mode) {
        case FW2KB_MODE_ALL:
        case FW2KB_MODE_LOWER:   kb->page = PAGE_LOWER;   break;
        case FW2KB_MODE_UPPER:   kb->page = PAGE_UPPER;   break;
        case FW2KB_MODE_NUMBERS: kb->page = PAGE_NUMBERS; break;
        case FW2KB_MODE_HEX:     kb->page = PAGE_HEX;     break;
    }
}

static void cycle_page(fw2kb_t *kb)
{
    switch (kb->mode) {
        case FW2KB_MODE_ALL:
            kb->page++;
            if (kb->page == PAGE_SYMBOLS) kb->page = PAGE_UPPER;
            break;
        case FW2KB_MODE_LOWER:
            kb->page = (kb->page == PAGE_LOWER) ? PAGE_NUMBERS : PAGE_LOWER;
            break;
        case FW2KB_MODE_UPPER:
            kb->page = (kb->page == PAGE_UPPER) ? PAGE_SYMBOLS : PAGE_UPPER;
            break;
        case FW2KB_MODE_NUMBERS: kb->page = PAGE_NUMBERS; break;
        case FW2KB_MODE_HEX:     kb->page = PAGE_HEX;     break;
    }
}

void fw2kb_press(fw2kb_t *kb, fw2kb_btn btn)
{
    if (btn >= FW2KB_BTN_GRAY && btn <= FW2KB_BTN_RED) {
        int i = (int)btn;
        if (kb->group_state) {
            kb->sub = i;
            kb->group_state = false;
            const char *g = k_pages[kb->page][kb->sub].s;
            for (int n = 0; n < 5; n++) {
                kb->scratch[n][0] = g[n];
                kb->scratch[n][1] = 0;
            }
        } else {
            char ch = k_pages[kb->page][kb->sub].s[i];
            kb->group_state = true;
            if (ch != 0)
                fw2kb_push_event(kb, FW2KB_KEY_CHAR, ch);
        }
        return;
    }
    if (btn == FW2KB_BTN_CTRL_DOWN || btn == FW2KB_BTN_AI) {
        if (kb->group_state) cycle_page(kb);
        else kb->group_state = true;    /* cancel half-entered chord */
        return;
    }
    /* FW2KB_BTN_CTRL_UP reserved; unknown values ignored */
}

void fw2kb_touch(fw2kb_t *kb, int x, int y)
{
    (void)x;   /* firmware splits on y only (rpTextEditor::touchEvent) */
    if (y > kb->touch_threshold)
        fw2kb_push_event(kb, FW2KB_KEY_CHAR, ' ');
    else
        fw2kb_push_event(kb, FW2KB_KEY_BACKSPACE, 0);
}

void fw2kb_set_touch_threshold(fw2kb_t *kb, int y)
{
    kb->touch_threshold = y;
}

bool fw2kb_next_event(fw2kb_t *kb, fw2kb_event *out)
{
    if (kb->ring_count == 0) return false;
    *out = kb->ring[kb->ring_head];
    kb->ring_head = (kb->ring_head + 1) % FW2KB_EVENT_RING;
    kb->ring_count--;
    return true;
}

void fw2kb_get_labels(const fw2kb_t *kb, const char *labels[5])
{
    for (int n = 0; n < 5; n++)
        labels[n] = kb->group_state ? k_pages[kb->page][n].s : kb->scratch[n];
}

bool fw2kb_in_chord(const fw2kb_t *kb)
{
    return !kb->group_state;
}

bool fw2kb_chord_for(const fw2kb_t *kb, char ch, fw2kb_btn *out, int *n)
{
    if (ch == 0) return false;          /* group padding, never typable */

    fw2kb_t tmp = *kb;                  /* simulate; never touch the caller's */
    int count = 0;

    if (!tmp.group_state) {             /* cancel a half-entered chord first */
        out[count++] = FW2KB_BTN_AI;
        tmp.group_state = true;
    }

    /* Try the current page, then each page reachable by cycling. Six hops is
     * more than any mode needs (MODE_ALL reaches three pages), and revisiting
     * a page is harmless because the first match wins. */
    for (int hop = 0; hop <= 5; hop++) {
        for (int g = 0; g < 5; g++) {
            for (int i = 0; i < 5; i++) {
                if (k_pages[tmp.page][g].s[i] != ch) continue;
                if (count + 2 > FW2KB_CHORD_MAX) return false;
                out[count++] = (fw2kb_btn)g;
                out[count++] = (fw2kb_btn)i;
                *n = count;
                return true;
            }
        }
        if (count + 1 > FW2KB_CHORD_MAX) return false;
        out[count++] = FW2KB_BTN_AI;
        cycle_page(&tmp);
    }
    return false;
}
