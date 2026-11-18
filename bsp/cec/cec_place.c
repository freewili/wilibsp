// bsp/cec/cec_place.c — see cec_place.h.
#include "cec_place.h"

#define OP_ACTIVE_SOURCE        0x82    // [Physical Address], broadcast
#define OP_ROUTING_CHANGE       0x80    // [Original Address][New Address], broadcast
#define OP_ROUTING_INFORMATION  0x81    // [Physical Address], broadcast
#define OP_SET_STREAM_PATH      0x86    // [Physical Address], broadcast

static bool     s_waiting;
static uint16_t s_route;                // the address the TV routed to
static uint32_t s_due_ms;

bool cec_place_from_firmware(uint32_t magic_word, uint32_t addr_word, uint16_t *phys_addr) {
    if (magic_word != CEC_PLACE_INFO_MAGIC) return false;
    const uint16_t addr = (uint16_t)addr_word;
    if ((uint16_t)(addr_word >> 16) != (uint16_t)~addr || addr == CEC_PHYS_ADDR_NONE) return false;
    *phys_addr = addr;
    return true;
}

void cec_place_format(uint16_t phys_addr, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 4; i++) {
        out[2 * i] = hex[(phys_addr >> (12 - 4 * i)) & 0xFu];
        out[2 * i + 1] = i < 3 ? '.' : 0;
    }
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool cec_place_parse(const char *text, size_t len, uint16_t *phys_addr) {
    while (len > 0 && is_space(text[0])) { text++; len--; }
    while (len > 0 && is_space(text[len - 1])) len--;
    if (len != 7) return false;
    uint16_t addr = 0;
    for (int i = 0; i < 4; i++) {
        const int d = hex_digit(text[2 * i]);
        if (d < 0 || (i < 3 && text[2 * i + 1] != '.')) return false;
        addr = (uint16_t)((addr << 4) | (uint16_t)d);
    }
    if (addr == 0 || addr == CEC_PHYS_ADDR_NONE) return false;
    *phys_addr = addr;
    return true;
}

void cec_place_learn_reset(void) { s_waiting = false; }

// at is route, or a place below it: the digits route has match, the rest are free.
static bool within(uint16_t route, uint16_t at) {
    for (int shift = 12; shift >= 0; shift -= 4) {
        const uint16_t d = (uint16_t)((route >> shift) & 0xFu);
        if (d == 0) return true;
        if (((at >> shift) & 0xFu) != d) return false;
    }
    return true;
}

static uint16_t addr_at(const cec_frame_t *frame, int at) {
    return (uint16_t)((frame->block[at] << 8) | frame->block[at + 1]);
}

void cec_place_on_rx(const cec_frame_t *frame, uint16_t own, uint32_t now_ms) {
    if (frame->len < 2 || (frame->block[0] & 0x0Fu) != CEC_LOGICAL_ALL) return;
    const uint8_t opcode = frame->block[1];
    const bool from_tv = (frame->block[0] >> 4) == CEC_LOGICAL_TV;

    int at = -1;
    if (from_tv && opcode == OP_ROUTING_CHANGE && frame->len == 6) at = 4;
    if (from_tv && opcode == OP_SET_STREAM_PATH && frame->len == 4) at = 2;
    if (at >= 0) {
        const uint16_t route = addr_at(frame, at);
        s_waiting = route != own && route != 0 && route != CEC_PHYS_ADDR_NONE;
        s_route = route;
        s_due_ms = now_ms + CEC_PLACE_ANSWER_MS;
        return;
    }
    if (s_waiting && (opcode == OP_ACTIVE_SOURCE || opcode == OP_ROUTING_INFORMATION) && frame->len == 4 &&
        within(s_route, addr_at(frame, 2)))
        s_waiting = false;
}

bool cec_place_learned(uint32_t now_ms, uint16_t *phys_addr) {
    if (!s_waiting || (int32_t)(now_ms - s_due_ms) < 0) return false;
    s_waiting = false;
    *phys_addr = s_route;
    return true;
}
