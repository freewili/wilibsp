// bsp/wilinfc/WILiNFC.h — 13.56 MHz NFC library (precompiled).
/*
 * All symbols carry the WILiNFC_ prefix. The header is plain C and is safe
 * to include from C++.
 */
#ifndef WILINFC_H
#define WILINFC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Interface revision; bumped on any incompatible change. */
#define WILiNFC_API_VERSION 1u

/* Library version as a dotted string, for example "1.0.0"; never NULL. Safe
 * to call at any time, including before WILiNFC_start(). */
const char *WILiNFC_version(void);

/* Result codes returned by library calls. */
typedef enum {
    WILiNFC_OK = 0,
    WILiNFC_NOT_RUNNING,          /* WILiNFC_start has not succeeded */
    WILiNFC_BUSY,                 /* emulation or a raw session is using the
                                     front end; stop it, then retry */
    WILiNFC_BAD_ARGUMENT,         /* a NULL pointer, bad length or out-of-range
                                     sector/block; fix the call */
    WILiNFC_UNSUPPORTED,          /* the selected card cannot do this */
    WILiNFC_NO_CARD,              /* no card is selected; wait for WILiNFC_poll */
    WILiNFC_CARD_LOST,            /* the card stopped answering; hold it against
                                     the back of the FreeWili again */
    WILiNFC_AUTH_FAILED,          /* no key, or a wrong key, for the sector */
    WILiNFC_CRC_ERROR,            /* a corrupted exchange; retry */
    WILiNFC_COLLISION,            /* more than one card in the field */
    WILiNFC_PROTECTED,            /* the card refused (access bits or a
                                     protected block) */
    WILiNFC_NO_FRONT_END,         /* the NFC hardware did not answer on the bus
                                     (not "no card") */
    WILiNFC_INIT_TIMEOUT,         /* the NFC hardware did not become ready */
    WILiNFC_API_VERSION_MISMATCH, /* header and library are different releases */
    WILiNFC_STORAGE_UNAVAILABLE,  /* no usable file hooks are registered */
    WILiNFC_RAW_NOT_ACTIVE,       /* a raw call without WILiNFC_raw_begin */
    WILiNFC_FAILED                /* any other failure, including a file hook
                                     error */
} WILiNFC_result_t;

/* Short constant name for a result code; never NULL. */
const char *WILiNFC_result_name(WILiNFC_result_t result);

/*
 * ---- Application-supplied file access ----
 *
 * The library has no filesystem of its own. Saved cards, the MIFARE Classic
 * key cache and dictionary runs go through these hooks, one file at a time
 * (the library never has two files open).
 *
 * Required hooks: open, read_line, write_line and close. If any of them is
 * NULL, or WILiNFC_set_file_io was never called or was given NULL, every
 * storage call returns WILiNFC_STORAGE_UNAVAILABLE. mkdir is optional: when
 * it is NULL no directory is created, so directories must already exist.
 *
 * Which hooks each operation calls:
 *     reading a file (WILiNFC_card_load, WILiNFC_mfc_key_cache_load, the
 *     cache load when a MIFARE Classic card is selected, WILiNFC_mfc_dict_run
 *     reading a dictionary): open (write=false), read_line one line at a
 *     time (the library may stop before the end of the file), close.
 *     writing a file (WILiNFC_card_save, WILiNFC_mfc_key_cache_save, the
 *     cache save when a card leaves, the cache rewrite by
 *     WILiNFC_mfc_dict_run): mkdir on the fixed directory listed under
 *     "Storage paths" (/nfc for WILiNFC_card_save, /nfc/cache for the
 *     cache), open (write=true), write_line for each line, close. The
 *     directory of a path you pass is never created: create it yourself.
 *
 * open(ctx, path, write)
 *     write=false: open an existing file for reading; return false if it does
 *     not exist. write=true: create the file or truncate it.
 * read_line(ctx, buf, max_len)
 *     Write the next line into buf, NUL-terminated, at most max_len bytes
 *     including the NUL. The library passes max_len = 128 for saved-card and
 *     key-cache files and 32 for dictionary files, so lines are at most 127
 *     and 31 characters. A longer line is not supported. The line may end in
 *     LF, CRLF or nothing; the library strips the ending. Return > 0 when a
 *     line was read, 0 at end of file, and a negative value for an I/O error.
 *     An error stops the read, and the storage call returns WILiNFC_FAILED.
 * write_line(ctx, line)
 *     Write the text exactly as given, adding nothing. The library sends each
 *     line without its end and then calls write_line once more with "\n".
 *     Return false on error; the storage call then returns WILiNFC_FAILED.
 * close(ctx)
 *     Called exactly once after every open that returned true, including
 *     after a read or write error. false for a file opened for writing means
 *     the data may not be stored, and the storage call returns
 *     WILiNFC_FAILED; for a file opened for reading the result is ignored.
 * mkdir(ctx, path)
 *     Create one directory. Return true if it was created or already exists.
 *     The library ignores the result and goes on to open the file.
 *
 * Paths are absolute, use '/', and are at most WILiNFC_PATH_MAX - 1
 * characters. The library passes either the path you gave it or one of the
 * fixed paths listed under "Storage paths" below.
 *
 * Hooks may block. The card is not tracked while a hook runs, so a card may be
 * reported removed afterwards, and the calling operation takes longer by the
 * hook's duration. Keep hooks short where you can: an SD call normally takes
 * tens of milliseconds, and capping it at about one second is acceptable.
 */
typedef struct {
    void *ctx;
    bool (*open)(void *ctx, const char *path, bool write);
    int  (*read_line)(void *ctx, char *buf, int max_len);
    bool (*write_line)(void *ctx, const char *line);
    bool (*close)(void *ctx);
    bool (*mkdir)(void *ctx, const char *path);
} WILiNFC_file_io_t;

#define WILiNFC_PATH_MAX 128   /* longest path passed to a hook, with its NUL */
#define WILiNFC_LINE_MAX 128   /* largest max_len passed to read_line */

/* Register the file hooks. The struct is copied, so it need not outlive the
 * call. NULL disables storage. */
void WILiNFC_set_file_io(const WILiNFC_file_io_t *io);

/*
 * ---- Storage paths ----
 *
 * The library reads or writes only these fixed paths, plus the paths you pass
 * to WILiNFC_card_save, WILiNFC_card_load and WILiNFC_mfc_dict_run. The paths
 * are fixed so that key caches and dictionaries work the same on every
 * FreeWili device.
 *
 *   /nfc
 *       WILiNFC_card_save calls mkdir on it before it opens your path.
 *   /nfc/cache/<UID>.nfc
 *       The MIFARE Classic key cache: one file per card. <UID> is the card's
 *       UID in upper-case hex with no separators (for example
 *       /nfc/cache/04A1B2C3.nfc). The file is Flipper "NFC keys" text
 *       ("Key A sector N: XX XX XX XX XX XX" lines). WILiNFC_mfc_key_cache_save
 *       calls mkdir on /nfc/cache only (not on /nfc first) and then truncates
 *       and rewrites the file; the save when a card leaves and the rewrite by
 *       WILiNFC_mfc_dict_run do the same. /nfc must therefore already exist:
 *       WILiNFC_card_save creates it, or create /nfc yourself before relying
 *       on the cache. WILiNFC_mfc_key_cache_load reads the file.
 *   /nfc/assets/mf_classic_dict.nfc
 *   /nfc/assets/mf_classic_dict_user.nfc
 *       Key dictionaries, one 12-hex-digit key per line; '#' starts a
 *       comment. WILiNFC_mfc_dict_run with a NULL path reads the first file,
 *       then the second. The library never writes either file.
 */

/* ---- Lifecycle ---- */

/* Overall library state. */
typedef enum {
    WILiNFC_STATE_OFF = 0,   /* not started (or stopped) */
    WILiNFC_STATE_READY,     /* running, looking for or holding a card */
    WILiNFC_STATE_NO_SIGNAL, /* running but the reader field is not active */
    WILiNFC_STATE_FAULT,     /* the front end is absent or failed to start,
                                or the reader failed later and could not be
                                restarted */
    WILiNFC_STATE_EMULATING  /* presenting a card identity to an external reader */
} WILiNFC_state_t;

/* Short constant name for a state; never NULL. */
const char *WILiNFC_state_name(WILiNFC_state_t state);

/* Options for WILiNFC_start_ex(). This release has none: reserved is
 * ignored; set it to 0 (WILiNFC_config_default() does). */
typedef struct {
    uint32_t reserved;
} WILiNFC_config_t;

/* A configuration with every field at its normal value. */
static inline WILiNFC_config_t WILiNFC_config_default(void)
{
    WILiNFC_config_t c;
    c.reserved = 0;
    return c;
}

/*
 * Bring the front end up and begin looking for cards. The call blocks until
 * the reader is running or has failed: normally well under 100 ms, and at
 * most about 1.1 s. NFC hardware that does not answer on the bus fails at
 * once with WILiNFC_NO_FRONT_END. Hardware that answers but is not ready
 * within 1 s fails with WILiNFC_INIT_TIMEOUT. An application compiled against
 * a different header version fails with WILiNFC_API_VERSION_MISMATCH. Returns
 * false on failure; WILiNFC_last_error() says why. Calling it while already
 * running returns true.
 *
 * WILiNFC_start() and WILiNFC_start_ex() are macros that pass the
 * WILiNFC_API_VERSION of the header the application was compiled with. Call
 * the macros, not the _v functions.
 */
bool WILiNFC_start_v(uint32_t api_version);

/* As WILiNFC_start(), with options. config may be NULL for the defaults; a
 * NULL config behaves exactly like WILiNFC_start(). */
bool WILiNFC_start_ex_v(const WILiNFC_config_t *config, uint32_t api_version);

#define WILiNFC_start() WILiNFC_start_v(WILiNFC_API_VERSION)
#define WILiNFC_start_ex(config) WILiNFC_start_ex_v((config), WILiNFC_API_VERSION)

/* Turn the reader off and release the front end. Safe when not running. */
void WILiNFC_stop(void);

/*
 * Drive the library. Call it continuously from the main loop: each call does
 * one step of card detection and returns. A call takes up to about 10 ms with
 * no card in the field and at most about 50 ms with one; apart from the file
 * hooks it calls (the key-cache load when a MIFARE Classic card is selected
 * and the save when it leaves), it never waits longer than that. Card
 * detection, removal detection and the timings below assume no gap of more
 * than a few tens of milliseconds between calls.
 */
void WILiNFC_service(void);

/* Current state. After a successful WILiNFC_start it is READY (or EMULATING
 * while emulating); NO_SIGNAL means the reader is not looking for cards
 * (for example during a raw session); FAULT means the front end is absent or
 * failed to start, or the reader failed later and could not be restarted. */
WILiNFC_state_t WILiNFC_state(void);

/* Reason for the most recent failure; WILiNFC_OK if none. */
WILiNFC_result_t WILiNFC_last_error(void);

/* ---- Card detection ---- */

typedef enum {
    WILiNFC_CARD_TYPE_UNKNOWN = 0,
    WILiNFC_CARD_TYPE_T1T,
    WILiNFC_CARD_TYPE_T2T,
    WILiNFC_CARD_TYPE_T4T,
    WILiNFC_CARD_TYPE_NFC_DEP,
    WILiNFC_CARD_TYPE_MIFARE_CLASSIC
} WILiNFC_card_type_t;

typedef enum {
    WILiNFC_CARD_NONE = 0,   /* no card seen since start or forget */
    WILiNFC_CARD_PRESENT,    /* a card is in the field */
    WILiNFC_CARD_REMOVED     /* the card that was present has left */
} WILiNFC_card_state_t;

typedef struct {
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t atqa[2];
    uint8_t sak;
    WILiNFC_card_type_t type;
    uint8_t ats[20];
    uint8_t ats_len;         /* 0 unless is_iso_dep */
    bool is_iso_dep;
} WILiNFC_card_t;

/*
 * The reader's antenna is on the back of the FreeWili: hold the card flat
 * against the back of the FreeWili and keep it still.
 *
 * Returns true once for each newly selected card and fills *out. Returns
 * false when there is nothing new (or the library is not running). With
 * WILiNFC_service called continuously, a card is reported within about 50 ms
 * of entering the field (plus any file-hook time).
 */
bool WILiNFC_poll(WILiNFC_card_t *out);

/* CARD_REMOVED is reported within about 150 ms of the card leaving the field.
 * The state then stays CARD_REMOVED until a new card is selected or
 * WILiNFC_forget is called; it does not go back to CARD_NONE by itself. */
WILiNFC_card_state_t WILiNFC_card_state(void);

/* Copy the most recently seen card; false if none has been seen. */
bool WILiNFC_last_card(WILiNFC_card_t *out);

/*
 * Saved cards: write a card's identity to / read it back from a Flipper
 * ".nfc" text file, over the registered file-io hooks. The file carries the
 * UID, ATQA and SAK plus a device-type line, so a saved card is also readable
 * by other tools that use the format. The card type survives only as far as
 * the format expresses it: MIFARE Classic and ISO-DEP (T4T) round-trip;
 * other types are stored as a plain ISO 14443-3A card and load back as
 * WILiNFC_CARD_TYPE_UNKNOWN (an Ultralight/NTAG file loads as T2T). ATS is
 * filled on load when the file has one. Errors: WILiNFC_BAD_ARGUMENT for a
 * NULL argument; WILiNFC_STORAGE_UNAVAILABLE when no file-io is registered;
 * WILiNFC_FAILED when the file cannot be written or parsed.
 */
WILiNFC_result_t WILiNFC_card_save(const char *path, const WILiNFC_card_t *card);
WILiNFC_result_t WILiNFC_card_load(const char *path, WILiNFC_card_t *out);

/* Forget the remembered card; a card still in the field is reported again. */
void WILiNFC_forget(void);

/* Put the present card to sleep. Returns false if there is none. */
bool WILiNFC_halt(void);

/* ---- MIFARE Classic ---- */

typedef enum {
    WILiNFC_MFC_UNKNOWN = 0,
    WILiNFC_MFC_MINI,
    WILiNFC_MFC_CLASSIC_1K,
    WILiNFC_MFC_CLASSIC_4K
} WILiNFC_mfc_variant_t;

typedef enum {
    WILiNFC_MFC_KEY_A = 0,
    WILiNFC_MFC_KEY_B = 1
} WILiNFC_mfc_keytype_t;

#define WILiNFC_MFC_KEY_SIZE    6
#define WILiNFC_MFC_BLOCK_SIZE  16
#define WILiNFC_MFC_MAX_SECTORS 40  /* 4K, the largest variant */
#define WILiNFC_MFC_MAX_BLOCKS_IN_SECTOR 16

/*
 * The variant of a card, from its SAK: 0x09 Mini, 0x08 or 0x88 Classic 1K,
 * 0x18 or 0x98 Classic 4K, anything else (or NULL) WILiNFC_MFC_UNKNOWN. The
 * MIFARE calls below use this same variant for their range checks. A card
 * whose variant is WILiNFC_MFC_UNKNOWN is detected but cannot be used with
 * them; they return WILiNFC_NO_CARD for it. Pass the variant to the geometry
 * helpers to size buffers before reading.
 */
WILiNFC_mfc_variant_t WILiNFC_mfc_variant_of(const WILiNFC_card_t *card);

/*
 * Card geometry; no card needed. Sectors 0-31 have 4 blocks each and sectors
 * 32-39 (4K only) have 16. Each returns 0 for a sector at or beyond
 * WILiNFC_mfc_sector_count(variant).
 */
uint8_t WILiNFC_mfc_sector_count(WILiNFC_mfc_variant_t variant);
uint8_t WILiNFC_mfc_blocks_in_sector(WILiNFC_mfc_variant_t variant, uint8_t sector);
uint8_t WILiNFC_mfc_sector_to_block(WILiNFC_mfc_variant_t variant, uint8_t sector);
uint8_t WILiNFC_mfc_trailer_block(WILiNFC_mfc_variant_t variant, uint8_t sector);

/*
 * The calls below need a selected MIFARE Classic card. They return
 * WILiNFC_NOT_RUNNING before WILiNFC_start, WILiNFC_BUSY while emulation or a
 * raw session is active, WILiNFC_NO_CARD when no card is selected, and
 * WILiNFC_UNSUPPORTED when the card is not MIFARE Classic.
 *
 * Keys: the library keeps a key slot per sector (Key A and Key B) for the
 * selected card. Slots are filled by WILiNFC_mfc_auth, by
 * WILiNFC_mfc_key_cache_load and by WILiNFC_mfc_dict_run. When the card is
 * selected the library also loads its cache file (see "Storage paths") if
 * file hooks are registered. Every block, sector and value call authenticates
 * by itself from those slots, so you do not need to call WILiNFC_mfc_auth
 * first when a key is already known. A read tries Key A and then Key B; a
 * write tries Key B and then Key A. Without a usable key the call returns
 * WILiNFC_AUTH_FAILED.
 *
 * An authentication lasts across calls for as long as they stay in the same
 * sector; a call in another sector authenticates again. Keys belong to the
 * selected card. They are kept for as long as it stays in the field
 * (WILiNFC_forget does not clear them) and are discarded when it leaves. As
 * it leaves, any keys found are written to its cache file when storage is
 * available. WILiNFC_halt puts the card to sleep, which ends its
 * authentication: before further MIFARE calls, take it out of the field or
 * call WILiNFC_forget and wait for WILiNFC_poll to report it again.
 */

/* Authenticate with a 6-byte key and store it in that sector's slot. The
 * first argument is a sector number (0 to sector_count - 1), not a block
 * number. */
WILiNFC_result_t WILiNFC_mfc_auth(uint8_t sector, WILiNFC_mfc_keytype_t keytype,
                                   const uint8_t key[WILiNFC_MFC_KEY_SIZE]);

WILiNFC_result_t WILiNFC_mfc_read_block(uint8_t block,
                                         uint8_t out[WILiNFC_MFC_BLOCK_SIZE]);
/* WARNING: writing a sector trailer with wrong keys or access bits can
 * permanently lock that sector. Block 0 (manufacturer block) is rejected by
 * non-magic cards. A block beyond the card gives BAD_ARGUMENT. */
WILiNFC_result_t WILiNFC_mfc_write_block(uint8_t block,
                                          const uint8_t in[WILiNFC_MFC_BLOCK_SIZE]);

/* Value-block operations; like the block calls they authenticate from the
 * key slots by themselves. Increment and decrement stage a change on the
 * card; WILiNFC_mfc_transfer_value commits it to the block, and
 * WILiNFC_mfc_restore_value loads a block's value into the card's staging
 * area (so restore then transfer to another block of the same sector copies
 * a value). */
WILiNFC_result_t WILiNFC_mfc_read_value(uint8_t block, int32_t *value);
WILiNFC_result_t WILiNFC_mfc_write_value(uint8_t block, int32_t value);
WILiNFC_result_t WILiNFC_mfc_increment_value(uint8_t block, int32_t delta);
WILiNFC_result_t WILiNFC_mfc_decrement_value(uint8_t block, int32_t delta);
WILiNFC_result_t WILiNFC_mfc_transfer_value(uint8_t block);
WILiNFC_result_t WILiNFC_mfc_restore_value(uint8_t block);

/*
 * Read every block of a sector into out (out_len must be at least
 * blocks_in_sector * 16). block_ok, if non-NULL, gets one bool per block
 * (true if that block's bytes came from the card; a block that could not be
 * read is zero-filled). Returns WILiNFC_OK if at least one block was read and
 * WILiNFC_AUTH_FAILED if none was.
 */
WILiNFC_result_t WILiNFC_mfc_read_sector(uint8_t sector, uint8_t *out, uint16_t out_len,
                                          bool *block_ok);

/*
 * Write every non-trailer block of a sector from in (in_len must be exactly
 * (blocks_in_sector - 1) * 16; the trailer block is never touched). Blocks
 * are written in order and the call stops at the first failure, returning
 * that block's result, so it is not all-or-nothing. In sector 0 the first
 * block is block 0 (the manufacturer block). A card that is not a magic card
 * rejects it and nothing is written; a magic card accepts it, so the whole
 * sector is written. A sector at or beyond the card's sector count gives
 * BAD_ARGUMENT.
 */
WILiNFC_result_t WILiNFC_mfc_write_sector(uint8_t sector, const uint8_t *in,
                                           uint16_t in_len);

/* Access bits (bytes 6-9 of the sector trailer) for a sector. */
WILiNFC_result_t WILiNFC_mfc_access_bits(uint8_t sector, uint8_t out[4]);

/*
 * Key cache for the selected card (file /nfc/cache/<UID>.nfc, see "Storage
 * paths"). load reads the file into the key slots. save writes every key
 * found so far; with no key found it writes nothing and returns WILiNFC_OK.
 * Both return WILiNFC_STORAGE_UNAVAILABLE when storage is not available.
 */
WILiNFC_result_t WILiNFC_mfc_key_cache_load(void);
WILiNFC_result_t WILiNFC_mfc_key_cache_save(void);

/*
 * Try every key of a dictionary against each sector key that is not yet
 * known. dict_path is your dictionary file; NULL means the two fixed
 * dictionaries (see "Storage paths"). Found keys go into the key slots and the
 * cache file is rewritten. progress (may be NULL) is called after each sector
 * with (sector, keys found so far, keys needed). Returns WILiNFC_OK if at
 * least one key is known afterwards, WILiNFC_AUTH_FAILED if none is, and
 * WILiNFC_STORAGE_UNAVAILABLE when storage is not available.
 */
WILiNFC_result_t WILiNFC_mfc_dict_run(const char *dict_path,
                                       void (*progress)(uint8_t sector, uint16_t found,
                                                         uint16_t total));

/* What WILiNFC_mfc_dump read. */
typedef struct {
    uint8_t sector_count;                       /* sectors in the dumped card */
    bool sector_known[WILiNFC_MFC_MAX_SECTORS]; /* true: a key was available */
    uint32_t dump_len;                          /* bytes written into buf */
} WILiNFC_mfc_dump_result_t;

/*
 * Full-card dump: read every sector into buf with the keys in the key slots
 * and return when the dump is complete. buf_len must be at least
 * blocks_in_card * 16 (see WILiNFC_mfc_sector_count and
 * WILiNFC_mfc_blocks_in_sector to size it). A sector with no usable key, and
 * a block that could not be read, is zero-filled; a sector with no usable key
 * has its sector_known entry false. Returns WILiNFC_OK if at least one sector
 * had a usable key, WILiNFC_AUTH_FAILED if none had, and WILiNFC_BAD_ARGUMENT
 * for a NULL buf or a buf_len that is too small (nothing is written then).
 * out (may be NULL) is filled on every return; dump_len is 0 when nothing was
 * written.
 *
 * Durations: each exchange with the card waits at most 50 ms for an answer.
 * With a card that answers, an authentication, a block read or a block write
 * each normally completes in a few milliseconds; a card that stops answering
 * makes one call wait about 100-150 ms before it fails. WILiNFC_mfc_dict_run
 * costs roughly 10-20 ms per key tried, per unknown sector key, so a large
 * dictionary against a card with many unknown keys can take minutes.
 * WILiNFC_mfc_dump normally takes about 1 s for a 1K card and a few seconds
 * for a 4K card. These calls block; WILiNFC_service is not needed while they
 * run, and the field is not polled for new cards until they return.
 */
WILiNFC_result_t WILiNFC_mfc_dump(uint8_t *buf, uint32_t buf_len,
                                  WILiNFC_mfc_dump_result_t *out);

/* ---- UID emulation ---- */

/*
 * Present a card identity (NFC-A UID, ATQA, SAK) to an external reader:
 * hold the back of the FreeWili flat against the reader and keep it still.
 * uid_len must be 4, 7 or 10; atqa is 2 bytes. While emulating, reader and
 * MIFARE calls return WILiNFC_BUSY and WILiNFC_poll reports nothing;
 * WILiNFC_emulate_stop resumes reading. Emulation has no time limit; it runs
 * until WILiNFC_emulate_stop or WILiNFC_stop. Errors: BAD_ARGUMENT,
 * NOT_RUNNING, BUSY (during a raw session), FAILED.
 */
WILiNFC_result_t WILiNFC_emulate_begin(const uint8_t *uid, uint8_t uid_len,
                                       const uint8_t atqa[2], uint8_t sak);

/* Emulate the identity of a card: one from WILiNFC_poll, WILiNFC_last_card
 * or WILiNFC_card_load. Only uid, uid_len, atqa and sak are used. */
WILiNFC_result_t WILiNFC_emulate_card(const WILiNFC_card_t *card);

/* Stop emulating and resume reading. No-op when not emulating. */
void WILiNFC_emulate_stop(void);

bool WILiNFC_emulating(void);

typedef struct {
    bool active;          /* an identity is being presented */
    bool reader_engaged;  /* an external reader has selected the identity
                             since emulation began; stays true until the
                             next emulate_begin or emulate_stop */
} WILiNFC_emu_status_t;

WILiNFC_result_t WILiNFC_emulate_status(WILiNFC_emu_status_t *out);

/* ---- Raw transceiver ---- */

/*
 * Expert use, outside the supported contract: these calls pass register
 * values, command bytes and frames to the front end unchanged, and the
 * library does not define what they do.
 *
 * WILiNFC_raw_begin takes exclusive ownership (stops polling and any
 * emulation); until WILiNFC_raw_end, reader, MIFARE and emulation calls
 * return WILiNFC_BUSY. Every other raw call returns WILiNFC_RAW_NOT_ACTIVE
 * unless a raw session is active, and WILiNFC_NOT_RUNNING before
 * WILiNFC_start. WILiNFC_raw_end ends the session and resumes reading.
 */
#define WILiNFC_RAW_RX_MAX 129   /* largest receive buffer honoured */
#define WILiNFC_RAW_TX_MAX 240   /* largest transmit frame accepted */

typedef uint8_t WILiNFC_raw_flags_t;
#define WILiNFC_RAW_APPEND_CRC    0x01  /* bit0: append a check value on transmit */

typedef struct {
    bool ok;
    bool timeout;
    bool collision;
    bool crc;
    bool parity;
    bool framing;
} WILiNFC_raw_status_t;

WILiNFC_result_t WILiNFC_raw_begin(void);
WILiNFC_result_t WILiNFC_raw_end(void);
WILiNFC_result_t WILiNFC_raw_field(bool on);
WILiNFC_result_t WILiNFC_raw_reg_write(uint8_t addr, uint8_t value);
WILiNFC_result_t WILiNFC_raw_reg_read(uint8_t addr, uint8_t *out);
WILiNFC_result_t WILiNFC_raw_cmd(uint8_t command);

/*
 * Send tx (tx_len bytes, at most WILiNFC_RAW_TX_MAX) and receive up to rx_cap
 * bytes (clamped to WILiNFC_RAW_RX_MAX). tx_last_bits is 0 for whole bytes,
 * otherwise the number of valid bits in the final byte (tx_len still counts
 * that partial byte, so the WILiNFC_RAW_TX_MAX cap applies to it too). Only
 * WILiNFC_RAW_APPEND_CRC is acted on in flags; receive check-value problems
 * are reported only via status.crc. The call waits at most 50 ms for an
 * answer; status.timeout means nothing answered in that time. WILiNFC_OK
 * means the exchange ran; inspect status for the outcome.
 */
WILiNFC_result_t WILiNFC_raw_transceive(const uint8_t *tx, uint16_t tx_len,
                                        uint8_t tx_last_bits, WILiNFC_raw_flags_t flags,
                                        uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                                        WILiNFC_raw_status_t *status);

/*
 * ---- Footprint ----
 *
 * The library adds about 29 KB of code and constants and about 2.4 KB of
 * data to an app that uses every call; a loaded app holds all of it in SRAM.
 * The library never allocates from the heap, and a C application that links
 * pico_stdlib needs no extra library. The library uses the board's I2C1 as board_init() leaves it
 * and does not reconfigure the bus. It needs the BSP's board layer:
 * board_init() must have run, and it calls the BSP's board_i2c1_recover()
 * when the bus needs recovery (an app built without the BSP must provide that
 * function).
 */

#ifdef __cplusplus
}
#endif

#endif /* WILINFC_H */
