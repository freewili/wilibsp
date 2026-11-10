/* Compile check for the public RFID header, on the host compiler, in C and
 * (included from the C++ test next to it) in C++.
 *
 * Includes wilirfid/WILiRFID.h on its own (it must need nothing else) and
 * checks every function against the signature written out below, so a changed
 * or mistyped declaration fails the build. The library itself is built for the
 * board, not the host, so each check sits inside sizeof: the compiler checks
 * the types but never emits a reference to the function. */
#include "wilirfid/WILiRFID.h"

#include <stdio.h>

#ifdef __cplusplus
#define STATIC_ASSERT static_assert
#else
#define STATIC_ASSERT _Static_assert
#endif

STATIC_ASSERT(WILiRFID_API_VERSION == 1u, "WILiRFID_API_VERSION must be 1");
STATIC_ASSERT(WILiRFID_TAG_NAME_MAX > 0, "WILiRFID_TAG_NAME_MAX must be a buffer size");

struct wilirfid_api {
    const char *(*version)(void);
    bool (*start_v)(uint32_t);
    bool (*start_ex_v)(const WILiRFID_config_t *, uint32_t);
    void (*stop)(void);
    void (*service)(void);
    WILiRFID_state_t (*state)(void);
    WILiRFID_result_t (*last_error)(void);
    const char *(*state_name)(WILiRFID_state_t);
    const char *(*result_name)(WILiRFID_result_t);
    const char *(*format_name)(WILiRFID_format_t);
    bool (*format_can_write)(WILiRFID_format_t);
    bool (*format_can_emulate)(WILiRFID_format_t);
    bool (*poll)(WILiRFID_tag_t *);
    void (*set_scan_mode)(WILiRFID_scan_mode_t);
    WILiRFID_scan_mode_t (*scan_mode)(void);
    WILiRFID_card_state_t (*card_state)(void);
    bool (*last_tag)(WILiRFID_tag_t *);
    void (*forget)(void);
    WILiRFID_result_t (*write_begin)(const WILiRFID_tag_t *, const uint32_t *);
    WILiRFID_write_status_t (*write_status)(void);
    void (*write_cancel)(void);
    WILiRFID_result_t (*t5577_write_block)(uint8_t, uint32_t, const uint32_t *, bool);
    unsigned (*t5577_preset_count)(void);
    const WILiRFID_t5577_preset_t *(*t5577_preset)(unsigned);
    uint32_t (*t5577_config_for)(WILiRFID_format_t);
    WILiRFID_result_t (*t5577_read_block_begin)(uint8_t, const uint32_t *);
    WILiRFID_read_status_t (*t5577_read_block_status)(uint32_t *);
    WILiRFID_result_t (*emulate_begin)(const WILiRFID_tag_t *);
    WILiRFID_result_t (*emulate_begin_ex)(const WILiRFID_tag_t *, const WILiRFID_emu_options_t *);
    void (*emulate_stop)(void);
    bool (*emulating)(void);
    WILiRFID_emu_status_t (*emulate_status)(void);
    bool (*tag_to_string)(const WILiRFID_tag_t *, char *, size_t);
    bool (*tag_from_string)(const char *, WILiRFID_tag_t *);
    bool (*tag_name)(const WILiRFID_tag_t *, char *, size_t);
};

#define CHECK(member, fn) ((void)sizeof(api.member = &(fn)), ++checked)

int main(void) {
    struct wilirfid_api api;
    unsigned checked = 0;

    CHECK(version, WILiRFID_version);
    CHECK(start_v, WILiRFID_start_v);
    CHECK(start_ex_v, WILiRFID_start_ex_v);
    CHECK(stop, WILiRFID_stop);
    CHECK(service, WILiRFID_service);
    CHECK(state, WILiRFID_state);
    CHECK(last_error, WILiRFID_last_error);
    CHECK(state_name, WILiRFID_state_name);
    CHECK(result_name, WILiRFID_result_name);
    CHECK(format_name, WILiRFID_format_name);
    CHECK(format_can_write, WILiRFID_format_can_write);
    CHECK(format_can_emulate, WILiRFID_format_can_emulate);
    CHECK(poll, WILiRFID_poll);
    CHECK(set_scan_mode, WILiRFID_set_scan_mode);
    CHECK(scan_mode, WILiRFID_scan_mode);
    CHECK(card_state, WILiRFID_card_state);
    CHECK(last_tag, WILiRFID_last_tag);
    CHECK(forget, WILiRFID_forget);
    CHECK(write_begin, WILiRFID_write_begin);
    CHECK(write_status, WILiRFID_write_status);
    CHECK(write_cancel, WILiRFID_write_cancel);
    CHECK(t5577_write_block, WILiRFID_t5577_write_block);
    CHECK(t5577_preset_count, WILiRFID_t5577_preset_count);
    CHECK(t5577_preset, WILiRFID_t5577_preset);
    CHECK(t5577_config_for, WILiRFID_t5577_config_for);
    CHECK(t5577_read_block_begin, WILiRFID_t5577_read_block_begin);
    CHECK(t5577_read_block_status, WILiRFID_t5577_read_block_status);
    CHECK(emulate_begin, WILiRFID_emulate_begin);
    CHECK(emulate_begin_ex, WILiRFID_emulate_begin_ex);
    CHECK(emulate_stop, WILiRFID_emulate_stop);
    CHECK(emulating, WILiRFID_emulating);
    CHECK(emulate_status, WILiRFID_emulate_status);
    CHECK(tag_to_string, WILiRFID_tag_to_string);
    CHECK(tag_from_string, WILiRFID_tag_from_string);
    CHECK(tag_name, WILiRFID_tag_name);

    /* The start macros expand to calls of the _v functions; the config and the
     * emulation options take their members in the documented order. */
    WILiRFID_config_t cfg = { WILiRFID_PIO_AUTO, WILiRFID_SCAN_ALL };
    WILiRFID_emu_options_t opt = { WILiRFID_EMU_ALT_2, true };
    (void)sizeof(WILiRFID_start());
    (void)sizeof(WILiRFID_start_ex(&cfg));
    (void)sizeof(WILiRFID_emulate_begin_ex((const WILiRFID_tag_t *)0, &opt));

    if (checked != sizeof(struct wilirfid_api) / sizeof(api.version)) {
        printf("FAIL: checked %u of %u functions\n", checked,
               (unsigned)(sizeof(struct wilirfid_api) / sizeof(api.version)));
        return 1;
    }
    printf("PASS: %u functions\n", checked);
    return 0;
}
