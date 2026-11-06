// bsp/rfid/rfid_t5577.h — T5577 tag writer.
// Requires rfid_carrier to be running.
#ifndef RFID_T5577_H
#define RFID_T5577_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// T5577 modulation config words.
#define RFID_T5577_MOD_DIRECT      0x00000000u
#define RFID_T5577_MOD_PSK1        0x00100000u
#define RFID_T5577_MOD_PSK2        0x00200000u
#define RFID_T5577_MOD_PSK3        0x00300000u
#define RFID_T5577_MOD_FSK1        0x00400000u
#define RFID_T5577_MOD_FSK2        0x00500000u
#define RFID_T5577_MOD_FSK1A       0x00600000u
#define RFID_T5577_MOD_FSK2A       0x00700000u
#define RFID_T5577_MOD_MANCH       0x00800000u
#define RFID_T5577_MOD_BIPHASE     0x00900000u

// T5577 data rate config.
#define RFID_T5577_RF8             0x00000000u
#define RFID_T5577_RF16            0x00008000u
#define RFID_T5577_RF32            0x00010000u
#define RFID_T5577_RF40            0x00018000u
#define RFID_T5577_RF50            0x00020000u
#define RFID_T5577_RF64            0x00028000u
#define RFID_T5577_RF100           0x00030000u
#define RFID_T5577_RF128           0x00038000u

// Max blocks field.
#define RFID_T5577_MAXBLK(n)       (((uint32_t)(n) & 7u) << 2)

typedef enum {
    RFID_T5577_OK = 0,
    RFID_T5577_ERR_CARRIER,
    RFID_T5577_ERR_ARGS,
    RFID_T5577_ERR_ENCODE,
} rfid_t5577_result_t;

// Write a single data block. block: 0-7 (block 0 = config word, block 7 =
// password — both require confirm_token == 0xC0FFEE to prevent accidental
// brick). Returns RFID_T5577_OK on success.
rfid_t5577_result_t rfid_t5577_write_block(uint8_t page, uint8_t block,
                                           uint32_t data, uint32_t confirm_token);

// Write a block with password authentication.
rfid_t5577_result_t rfid_t5577_write_block_pwd(uint8_t page, uint8_t block,
                                               uint32_t data, uint32_t password,
                                               uint32_t confirm_token);

// Send a direct-access read command.
rfid_t5577_result_t rfid_t5577_read_block(uint8_t page, uint8_t block);

// Send a direct-access read with password.
rfid_t5577_result_t rfid_t5577_read_block_pwd(uint8_t page, uint8_t block,
                                              uint32_t password);

// Send a reset command — the tag re-reads its config and starts modulating.
rfid_t5577_result_t rfid_t5577_reset(void);

#define RFID_T5577_CONFIRM_TOKEN 0xC0FFEEu

#ifdef __cplusplus
}
#endif
#endif
