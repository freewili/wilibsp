// ew_input.h — Emberward's logical keys: the front panel, the nav pad, touch
// and the TV remote all set them; presses come out with key repeat for the
// turn and arrow keys. Pure C, host-tested in tests/test_ew_input.c.
#ifndef EW_INPUT_H
#define EW_INPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EW_KEY_LEFT,
    EW_KEY_RIGHT,
    EW_KEY_UP,
    EW_KEY_DOWN,
    EW_KEY_OK,      // flare, start, confirm
    EW_KEY_PAUSE,   // pause, back
    EW_KEY_VIEW,    // TV / LCD view
    EW_KEY_ALT,     // the yellow-slot action off the game field (sound)
    EW_KEY_ALT2,    // the blue-slot action off the game field (TV input)
    EW_KEY_ALT3,    // the red-slot action on the title (TV sound)
    EW_KEY_COUNT
} ew_key_t;

// Each source holds its own bit per key, so a remote key released while the
// front-panel button is still down keeps the key down.
typedef enum { EW_SRC_PANEL, EW_SRC_REMOTE, EW_SRC_TOUCH, EW_SRC_COUNT } ew_source_t;

#define EW_REPEAT_DELAY_MS 260u
#define EW_REPEAT_RATE_MS  110u

typedef struct {
    uint8_t held[EW_KEY_COUNT];         // bit per source
    uint32_t next_repeat_ms[EW_KEY_COUNT];
    uint16_t queue;                     // pending presses, bit per key
    uint8_t queue_count[EW_KEY_COUNT];  // presses not yet taken
} ew_input_t;

void ew_input_init(ew_input_t *in);

// A source's key went down or up at now_ms. A down while the key is already
// held by another source adds no press.
void ew_input_set(ew_input_t *in, ew_source_t src, ew_key_t key, bool down, uint32_t now_ms);

// Releases every key a source holds (the remote went away, touch lifted).
void ew_input_release_source(ew_input_t *in, ew_source_t src);

// Adds repeat presses for held repeating keys up to now_ms.
void ew_input_tick(ew_input_t *in, uint32_t now_ms);

// Takes one press of a key; false when none is pending.
bool ew_input_take(ew_input_t *in, ew_key_t key);

// Drops every pending press (between screens).
void ew_input_flush(ew_input_t *in);

bool ew_input_held(const ew_input_t *in, ew_key_t key);

// Keys that repeat while held.
bool ew_input_repeats(ew_key_t key);

#endif
