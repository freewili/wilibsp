/* Compile check for the public NFC header, on the host compiler.
 *
 * Includes wilinfc/WILiNFC.h on its own (it must need nothing else) and
 * checks every function against the signature written out below, so a changed
 * or mistyped declaration fails the build. The library itself is built for the
 * board, not the host, so each check sits inside sizeof: the compiler checks
 * the types but never emits a reference to the function.
 *
 * test_wilinfc_header_cpp.cpp compiles this same file as C++. */
#include "wilinfc/WILiNFC.h"

#include <stdio.h>

#ifdef __cplusplus
#define STATIC_ASSERT static_assert
#else
#define STATIC_ASSERT _Static_assert
#endif

STATIC_ASSERT(WILiNFC_API_VERSION == 1u, "WILiNFC_API_VERSION must be 1");
STATIC_ASSERT(WILiNFC_OK == 0, "WILiNFC_OK must be 0");
STATIC_ASSERT(WILiNFC_MFC_KEY_SIZE == 6, "a MIFARE Classic key is 6 bytes");
STATIC_ASSERT(WILiNFC_MFC_BLOCK_SIZE == 16, "a MIFARE Classic block is 16 bytes");
STATIC_ASSERT(WILiNFC_MFC_MAX_SECTORS == 40, "a 4K card has 40 sectors");
STATIC_ASSERT(WILiNFC_MFC_MAX_BLOCKS_IN_SECTOR == 16, "the largest sector has 16 blocks");
STATIC_ASSERT(WILiNFC_RAW_TX_MAX == 240, "raw transmit limit as documented");
STATIC_ASSERT(WILiNFC_RAW_RX_MAX == 129, "raw receive limit as documented");
STATIC_ASSERT(WILiNFC_PATH_MAX == 128, "hook path limit as documented");
STATIC_ASSERT(WILiNFC_LINE_MAX == 128, "hook line limit as documented");
STATIC_ASSERT(WILiNFC_STATE_OFF == 0, "WILiNFC_STATE_OFF must be 0");
STATIC_ASSERT(sizeof(((WILiNFC_card_t *)0)->uid) == 10, "a UID holds up to 10 bytes");
STATIC_ASSERT(sizeof(((WILiNFC_card_t *)0)->atqa) == 2, "ATQA is 2 bytes");
STATIC_ASSERT(sizeof(((WILiNFC_card_t *)0)->ats) == 20, "an ATS holds up to 20 bytes");
STATIC_ASSERT(sizeof(((WILiNFC_mfc_dump_result_t *)0)->sector_known) ==
                  WILiNFC_MFC_MAX_SECTORS * sizeof(bool),
              "one sector_known entry per sector of the largest card");

struct wilinfc_api {
    const char *(*version)(void);
    const char *(*result_name)(WILiNFC_result_t);
    void (*set_file_io)(const WILiNFC_file_io_t *);
    const char *(*state_name)(WILiNFC_state_t);
    bool (*start_v)(uint32_t);
    bool (*start_ex_v)(const WILiNFC_config_t *, uint32_t);
    void (*stop)(void);
    void (*service)(void);
    WILiNFC_state_t (*state)(void);
    WILiNFC_result_t (*last_error)(void);
    bool (*poll)(WILiNFC_card_t *);
    WILiNFC_card_state_t (*card_state)(void);
    bool (*last_card)(WILiNFC_card_t *);
    WILiNFC_result_t (*card_save)(const char *, const WILiNFC_card_t *);
    WILiNFC_result_t (*card_load)(const char *, WILiNFC_card_t *);
    void (*forget)(void);
    bool (*halt)(void);
    WILiNFC_mfc_variant_t (*mfc_variant_of)(const WILiNFC_card_t *);
    uint8_t (*mfc_sector_count)(WILiNFC_mfc_variant_t);
    uint8_t (*mfc_blocks_in_sector)(WILiNFC_mfc_variant_t, uint8_t);
    uint8_t (*mfc_sector_to_block)(WILiNFC_mfc_variant_t, uint8_t);
    uint8_t (*mfc_trailer_block)(WILiNFC_mfc_variant_t, uint8_t);
    WILiNFC_result_t (*mfc_auth)(uint8_t, WILiNFC_mfc_keytype_t, const uint8_t *);
    WILiNFC_result_t (*mfc_read_block)(uint8_t, uint8_t *);
    WILiNFC_result_t (*mfc_write_block)(uint8_t, const uint8_t *);
    WILiNFC_result_t (*mfc_read_value)(uint8_t, int32_t *);
    WILiNFC_result_t (*mfc_write_value)(uint8_t, int32_t);
    WILiNFC_result_t (*mfc_increment_value)(uint8_t, int32_t);
    WILiNFC_result_t (*mfc_decrement_value)(uint8_t, int32_t);
    WILiNFC_result_t (*mfc_transfer_value)(uint8_t);
    WILiNFC_result_t (*mfc_restore_value)(uint8_t);
    WILiNFC_result_t (*mfc_read_sector)(uint8_t, uint8_t *, uint16_t, bool *);
    WILiNFC_result_t (*mfc_write_sector)(uint8_t, const uint8_t *, uint16_t);
    WILiNFC_result_t (*mfc_access_bits)(uint8_t, uint8_t *);
    WILiNFC_result_t (*mfc_key_cache_load)(void);
    WILiNFC_result_t (*mfc_key_cache_save)(void);
    WILiNFC_result_t (*mfc_dict_run)(const char *, void (*)(uint8_t, uint16_t, uint16_t));
    WILiNFC_result_t (*mfc_dump)(uint8_t *, uint32_t, WILiNFC_mfc_dump_result_t *);
    WILiNFC_result_t (*emulate_begin)(const uint8_t *, uint8_t, const uint8_t *, uint8_t);
    WILiNFC_result_t (*emulate_card)(const WILiNFC_card_t *);
    void (*emulate_stop)(void);
    bool (*emulating)(void);
    WILiNFC_result_t (*emulate_status)(WILiNFC_emu_status_t *);
    WILiNFC_result_t (*raw_begin)(void);
    WILiNFC_result_t (*raw_end)(void);
    WILiNFC_result_t (*raw_field)(bool);
    WILiNFC_result_t (*raw_reg_write)(uint8_t, uint8_t);
    WILiNFC_result_t (*raw_reg_read)(uint8_t, uint8_t *);
    WILiNFC_result_t (*raw_cmd)(uint8_t);
    WILiNFC_result_t (*raw_transceive)(const uint8_t *, uint16_t, uint8_t, WILiNFC_raw_flags_t,
                                       uint8_t *, uint16_t, uint16_t *,
                                       WILiNFC_raw_status_t *);
};

#define CHECK(member, fn) ((void)sizeof(api.member = &(fn)), ++checked)

int main(void) {
    struct wilinfc_api api;
    unsigned checked = 0;

    CHECK(version, WILiNFC_version);
    CHECK(result_name, WILiNFC_result_name);
    CHECK(set_file_io, WILiNFC_set_file_io);
    CHECK(state_name, WILiNFC_state_name);
    CHECK(start_v, WILiNFC_start_v);
    CHECK(start_ex_v, WILiNFC_start_ex_v);
    CHECK(stop, WILiNFC_stop);
    CHECK(service, WILiNFC_service);
    CHECK(state, WILiNFC_state);
    CHECK(last_error, WILiNFC_last_error);
    CHECK(poll, WILiNFC_poll);
    CHECK(card_state, WILiNFC_card_state);
    CHECK(last_card, WILiNFC_last_card);
    CHECK(card_save, WILiNFC_card_save);
    CHECK(card_load, WILiNFC_card_load);
    CHECK(forget, WILiNFC_forget);
    CHECK(halt, WILiNFC_halt);
    CHECK(mfc_variant_of, WILiNFC_mfc_variant_of);
    CHECK(mfc_sector_count, WILiNFC_mfc_sector_count);
    CHECK(mfc_blocks_in_sector, WILiNFC_mfc_blocks_in_sector);
    CHECK(mfc_sector_to_block, WILiNFC_mfc_sector_to_block);
    CHECK(mfc_trailer_block, WILiNFC_mfc_trailer_block);
    CHECK(mfc_auth, WILiNFC_mfc_auth);
    CHECK(mfc_read_block, WILiNFC_mfc_read_block);
    CHECK(mfc_write_block, WILiNFC_mfc_write_block);
    CHECK(mfc_read_value, WILiNFC_mfc_read_value);
    CHECK(mfc_write_value, WILiNFC_mfc_write_value);
    CHECK(mfc_increment_value, WILiNFC_mfc_increment_value);
    CHECK(mfc_decrement_value, WILiNFC_mfc_decrement_value);
    CHECK(mfc_transfer_value, WILiNFC_mfc_transfer_value);
    CHECK(mfc_restore_value, WILiNFC_mfc_restore_value);
    CHECK(mfc_read_sector, WILiNFC_mfc_read_sector);
    CHECK(mfc_write_sector, WILiNFC_mfc_write_sector);
    CHECK(mfc_access_bits, WILiNFC_mfc_access_bits);
    CHECK(mfc_key_cache_load, WILiNFC_mfc_key_cache_load);
    CHECK(mfc_key_cache_save, WILiNFC_mfc_key_cache_save);
    CHECK(mfc_dict_run, WILiNFC_mfc_dict_run);
    CHECK(mfc_dump, WILiNFC_mfc_dump);
    CHECK(emulate_begin, WILiNFC_emulate_begin);
    CHECK(emulate_card, WILiNFC_emulate_card);
    CHECK(emulate_stop, WILiNFC_emulate_stop);
    CHECK(emulating, WILiNFC_emulating);
    CHECK(emulate_status, WILiNFC_emulate_status);
    CHECK(raw_begin, WILiNFC_raw_begin);
    CHECK(raw_end, WILiNFC_raw_end);
    CHECK(raw_field, WILiNFC_raw_field);
    CHECK(raw_reg_write, WILiNFC_raw_reg_write);
    CHECK(raw_reg_read, WILiNFC_raw_reg_read);
    CHECK(raw_cmd, WILiNFC_raw_cmd);
    CHECK(raw_transceive, WILiNFC_raw_transceive);

    /* The start macros must expand to calls returning bool (a NULL config
     * means the defaults), and the default configuration must be all zero. */
    WILiNFC_config_t cfg = WILiNFC_config_default();
    (void)sizeof(WILiNFC_start());
    (void)sizeof(WILiNFC_start_ex(&cfg));
    (void)sizeof(WILiNFC_start_ex(NULL));
    if (cfg.reserved != 0) {
        printf("FAIL: WILiNFC_config_default() reserved\n");
        return 1;
    }

    if (checked != sizeof(struct wilinfc_api) / sizeof(api.version)) {
        printf("FAIL: checked %u of %u functions\n", checked,
               (unsigned)(sizeof(struct wilinfc_api) / sizeof(api.version)));
        return 1;
    }
    printf("PASS: %u functions\n", checked);
    return 0;
}
