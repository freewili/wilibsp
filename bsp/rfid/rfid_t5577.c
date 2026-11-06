// bsp/rfid/rfid_t5577.c — T5577 tag writer.
#include "rfid/rfid_t5577.h"
#include "rfid/rfid_carrier.h"
#include "pico/stdlib.h"

#define START_GAP_FC   30
#define WRITE_GAP_FC   17
#define BIT0_FC        24
#define BIT1_FC        56
#define PROGRAM_FC     1500
#define FIELD_RESET_FC 1250
#define READ_SETTLE_FC 137
#define WRITE_ATTEMPTS 5
#define INTERFRAME_FC  500

static uint32_t s_fc_period_ns;

static void wait_fc(uint32_t fc) {
    busy_wait_us_32((uint32_t)(((uint64_t)fc * (uint64_t)s_fc_period_ns) / 1000u));
}

static void send_bit(bool bit) {
    rfid_carrier_gate(false);
    wait_fc(WRITE_GAP_FC);
    rfid_carrier_gate(true);
    wait_fc(bit ? BIT1_FC : BIT0_FC);
}

static void send_bits_msb(uint32_t data, unsigned nbits) {
    for (int i = (int)nbits - 1; i >= 0; i--)
        send_bit((data >> i) & 1u);
}

static bool check_carrier(void) {
    if (!rfid_carrier_running()) return false;
    unsigned hz = rfid_carrier_hz();
    if (hz == 0) return false;
    s_fc_period_ns = (uint32_t)(1000000000ull / (uint64_t)hz);
    return true;
}

static bool block_protected(uint8_t block, uint32_t token) {
    return (block == 0 || block == 7) && token != RFID_T5577_CONFIRM_TOKEN;
}

rfid_t5577_result_t rfid_t5577_write_block(uint8_t page, uint8_t block,
                                           uint32_t data, uint32_t confirm_token) {
    if (!check_carrier()) return RFID_T5577_ERR_CARRIER;
    if (page > 1 || block > 7) return RFID_T5577_ERR_ARGS;
    if (block_protected(block, confirm_token)) return RFID_T5577_ERR_ARGS;

    const uint8_t opcode = 0x02u | (page & 1u);

    for (unsigned attempt = 0; attempt < WRITE_ATTEMPTS; ++attempt) {
        if (attempt) {
            rfid_carrier_gate(true);
            wait_fc(INTERFRAME_FC);
        }

        rfid_carrier_gate(true);
        wait_fc(START_GAP_FC);
        rfid_carrier_gate(false);
        wait_fc(START_GAP_FC);
        rfid_carrier_gate(true);

        send_bits_msb(opcode, 2);
        send_bit(false);
        send_bits_msb(data, 32);
        send_bits_msb(block, 3);

        rfid_carrier_gate(false);
        wait_fc(WRITE_GAP_FC);
        rfid_carrier_gate(true);
        wait_fc(PROGRAM_FC);

        rfid_carrier_gate(false);
        wait_fc(FIELD_RESET_FC);
        rfid_carrier_gate(true);
    }
    return RFID_T5577_OK;
}

rfid_t5577_result_t rfid_t5577_write_block_pwd(uint8_t page, uint8_t block,
                                               uint32_t data, uint32_t password,
                                               uint32_t confirm_token) {
    if (!check_carrier()) return RFID_T5577_ERR_CARRIER;
    if (page > 1 || block > 7) return RFID_T5577_ERR_ARGS;
    if (block_protected(block, confirm_token)) return RFID_T5577_ERR_ARGS;

    const uint8_t opcode = 0x02u | (page & 1u);

    for (unsigned attempt = 0; attempt < WRITE_ATTEMPTS; ++attempt) {
        if (attempt) {
            rfid_carrier_gate(true);
            wait_fc(INTERFRAME_FC);
        }

        rfid_carrier_gate(true);
        wait_fc(START_GAP_FC);
        rfid_carrier_gate(false);
        wait_fc(START_GAP_FC);
        rfid_carrier_gate(true);

        send_bits_msb(opcode, 2);
        send_bits_msb(password, 32);
        send_bit(false);
        send_bits_msb(data, 32);
        send_bits_msb(block, 3);

        rfid_carrier_gate(false);
        wait_fc(WRITE_GAP_FC);
        rfid_carrier_gate(true);
        wait_fc(PROGRAM_FC);

        rfid_carrier_gate(false);
        wait_fc(FIELD_RESET_FC);
        rfid_carrier_gate(true);
    }
    return RFID_T5577_OK;
}

rfid_t5577_result_t rfid_t5577_read_block(uint8_t page, uint8_t block) {
    if (!check_carrier()) return RFID_T5577_ERR_CARRIER;
    if (page > 1 || block > 7) return RFID_T5577_ERR_ARGS;

    const uint8_t opcode = 0x02u | (page & 1u);

    rfid_carrier_gate(true);
    wait_fc(START_GAP_FC);
    rfid_carrier_gate(false);
    wait_fc(START_GAP_FC);
    rfid_carrier_gate(true);

    send_bits_msb(opcode, 2);
    send_bit(false);
    send_bits_msb(block, 3);

    rfid_carrier_gate(true);
    wait_fc(READ_SETTLE_FC);
    wait_fc(READ_SETTLE_FC);

    return RFID_T5577_OK;
}

rfid_t5577_result_t rfid_t5577_read_block_pwd(uint8_t page, uint8_t block,
                                              uint32_t password) {
    if (!check_carrier()) return RFID_T5577_ERR_CARRIER;
    if (page > 1 || block > 7) return RFID_T5577_ERR_ARGS;

    const uint8_t opcode = 0x02u | (page & 1u);

    rfid_carrier_gate(true);
    wait_fc(START_GAP_FC);
    rfid_carrier_gate(false);
    wait_fc(START_GAP_FC);
    rfid_carrier_gate(true);

    send_bits_msb(opcode, 2);
    send_bits_msb(password, 32);
    send_bit(false);
    send_bits_msb(block, 3);

    rfid_carrier_gate(true);
    wait_fc(READ_SETTLE_FC);
    wait_fc(READ_SETTLE_FC);

    return RFID_T5577_OK;
}

rfid_t5577_result_t rfid_t5577_reset(void) {
    if (!check_carrier()) return RFID_T5577_ERR_CARRIER;

    rfid_carrier_gate(true);
    wait_fc(START_GAP_FC);
    rfid_carrier_gate(false);
    wait_fc(START_GAP_FC);
    rfid_carrier_gate(true);

    send_bits_msb(0x00, 2);

    rfid_carrier_gate(true);
    wait_fc(READ_SETTLE_FC);

    return RFID_T5577_OK;
}
