// bsp/wilirfid/WILiRFID.h — 125 kHz RFID library (precompiled).
/*
 * WILiRFID: 125 kHz RFID for the FreeWili 2 board
 * ================================================
 *
 * What the library does
 *   The reader (this library) reads the common 125 kHz card formats (see
 *   WILiRFID_format_t), reports each card once as it arrives, and then tells you
 *   whether it is still there. It can write or clone
 *   a card onto a blank (a writable T5577 card or fob) and check the result by
 *   reading it back, read and write the eight blocks of a T5577 directly, and
 *   make the board present itself to an external reader as a card (emulation).
 *   Saved-card helpers turn a card into one line of text and back, in the format
 *   the device itself uses.
 *
 * The three-call pattern
 *   1. Start:   WILiRFID_start() once (or WILiRFID_start_ex() with options).
 *   2. Service: WILiRFID_service() on every pass of the main loop. All work
 *               happens inside this call.
 *   3. Poll:    WILiRFID_poll() after WILiRFID_service(); it returns true once
 *               for each new card.
 *
 *       fw2_app_recovery_init();      (NFC_RFID declared in POWER_ZONES)
 *       if (!WILiRFID_start()) { report WILiRFID_last_error(); }
 *       for (;;) {
 *           WILiRFID_service();
 *           WILiRFID_tag_t tag;
 *           if (WILiRFID_poll(&tag)) { use tag.format_name, tag.id, ... }
 *       }
 *
 * Power
 *   The RFID hardware sits in the NFC_RFID power zone. An app that calls
 *   fw2_app_recovery_init() only needs to declare NFC_RFID in its POWER_ZONES:
 *   that call keeps the declared zones awake and waits until they are powered.
 *   An app that does not must keep the zone awake for as long as the reader runs
 *   and wait until picpwr_rails() reports it powered (both are declared in
 *   input/picpwr.h):
 *       picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_NFC_RFID));
 *   With the zone asleep (or the RFID board missing) the reader still starts, but
 *   it never reaches READY: it stays in STARTING or shows NO_SIGNAL. If the state
 *   is not READY within about two seconds after start, check that the RFID board
 *   is seated and powered: call WILiRFID_stop(), fix it, and start again.
 *
 * Service cadence
 *   Call WILiRFID_service() at least every 20 ms. A normal call takes usually a
 *   few milliseconds, occasionally a few tens of milliseconds. Longer gaps between
 *   calls are tolerated and never break anything, but a card presented briefly may
 *   be missed and reports arrive later. Some calls block for longer:
 *   WILiRFID_write_begin() and WILiRFID_t5577_write_block() block for up to about
 *   two seconds, and while a write-and-check or a block read is in progress a
 *   WILiRFID_service() call can block for up to about two seconds. Show a "hold
 *   still" message before starting a write or a block read.
 *
 * Resources the reader holds from WILiRFID_start() until WILiRFID_stop()
 *   Everything below is claimed at start, including what emulation needs, and
 *   held until stop; a driver started after the reader cannot take any of it.
 *   Emulation uses these reserved resources and claims nothing new.
 *   - GPIO 34, 35, 36, 44, 46.
 *   - PWM slice 9. The haptic motor cannot be used while the reader runs.
 *   - The ADC. Do not use board_die_temp_c() or any other ADC reader while the
 *     reader runs.
 *   - One PIO block: two state machines and 21 instruction slots. The reader sets
 *     that block's GPIO base to 16, so the block must either be completely unused
 *     or already have its base at 16. Blocks are searched in the order 1, 2, 0
 *     unless WILiRFID_config_t.pio_block pins one. With the stock board_init(),
 *     WILiRFID_PIO_AUTO always finds a free block; pin one only when your own
 *     app uses PIO. If start fails with
 *     WILiRFID_HW_UNAVAILABLE_PIO, start the reader before the other PIO users,
 *     or pin a block you keep free with WILiRFID_config_t.pio_block.
 *   - DMA: three DMA channels, claimed dynamically (any free channels).
 *   - About 92 KB of code and constants plus under 64 KB of data. In an app
 *     loaded to RAM all of it sits in SRAM, and linking any WILiRFID_ function
 *     brings in the whole library.
 *   - No interrupts: the reader installs no interrupt handlers.
 *   Everything is released by WILiRFID_stop(). The reader and OneWili can be
 *   started in either order.
 *
 * Coexistence
 *   - Start the reader before the LED, microphone, IR or radio drivers, so it can
 *     claim a free PIO block first; or pin a block that stays free with
 *     WILiRFID_config_t.pio_block.
 *   - Never run the open rfid_* driver at the same time as this library; the two
 *     conflict.
 *   - Do not change the system clock while the reader runs.
 *   - Call every function from the main loop of one core. None is safe to call
 *     from an interrupt handler.
 *
 * Blanks
 *   To write or clone a card, buy T5577-based cards or key fobs, usually sold as
 *   "T5577" or "125 kHz rewritable" (also "writable" or "copyable"). Read-only
 *   cards cannot be written. A blank holds eight 32-bit blocks: block 0 sets how
 *   the card answers (and so which format a reader sees), blocks 1 to 6 hold data
 *   and block 7 an optional password. Bit 4 of block 0 (value & 0x00000010) turns
 *   password mode on: the card then accepts writes and block reads only with the
 *   password held in block 7.
 *
 * Presenting a card
 *   The antenna is on the back of the FreeWili. Lay the card flat against the
 *   back of the FreeWili, over the antenna, and hold it still, within a few
 *   centimetres. A card at the edge of range, or moving, may take longer to
 *   report, or be reported as removed and then arrive again. Hold it in place for
 *   the whole of a write or block read.
 *
 * States in practice (WILiRFID_state())
 *   OFF        not started, or stopped. Only the constant-time getters, the format
 *              and preset tables and the saved-card helpers do anything useful.
 *   STARTING   starting up (just started, or recovering after NO_SIGNAL); cards
 *              are reported once READY, typically in under a second. Still
 *              STARTING after about two seconds: check that the RFID board is
 *              seated and powered (see Power).
 *   READY      reading normally; cards are reported through WILiRFID_poll().
 *   NO_SIGNAL  running, but the RFID board is not responding (unseated or
 *              unpowered). The reader returns to READY by itself (through
 *              STARTING) once the board is usable. Writes are refused meanwhile.
 *   FAULT      an internal error occurred. Call WILiRFID_stop() and start
 *              again.
 *   EMULATING  the board is presenting a card to an external reader instead of
 *              reading (see Emulation below).
 *
 * Terms used below: "card" is a 125 kHz card or fob in the user's hand; "blank"
 * is a writable T5577 card or fob; "the reader" is this library running on the
 * board; "an external reader" is some other reader, such as a door reader.
 */
#ifndef WILIRFID_H
#define WILIRFID_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Version of this header's interface. The start macros pass it to the library so
 * a mismatch between header and library is caught at start. */
#define WILiRFID_API_VERSION 1u

/* ---- Lifecycle: results, states, start/stop/service ---- */

/* Version of the library as "MAJOR.MINOR.PATCH". Returns a constant string;
 * never NULL. Safe to call at any time, including before WILiRFID_start(). */
const char *WILiRFID_version(void);

/* Outcome of a library call. Calls that return a result also record it for
 * WILiRFID_last_error(). */
typedef enum {
    WILiRFID_OK = 0,                /* the call succeeded */
    WILiRFID_NOT_RUNNING,           /* the reader is not started; for writes this also
                                     * means the RFID board is not responding; check
                                     * WILiRFID_state() to tell the two apart */
    WILiRFID_BUSY,                  /* a write-and-check, block read or emulation is in
                                     * progress; try again later */
    WILiRFID_BAD_ARGUMENT,          /* an argument is NULL or out of range, or a card
                                     * record lacks the numbers its format needs */
    WILiRFID_UNSUPPORTED_FORMAT,    /* the card format cannot be used by this call (see
                                     * WILiRFID_format_can_write) */
    WILiRFID_PROTECTED_BLOCK,       /* block 0 or 7 was to be written without
                                     * confirm_config_block */
    WILiRFID_HW_UNAVAILABLE_PIO,    /* start: no PIO block had the free state machines
                                     * or instruction space */
    WILiRFID_HW_UNAVAILABLE_DMA,    /* start: three DMA channels could not be claimed,
                                     * or the reader failed to start */
    WILiRFID_API_VERSION_MISMATCH,  /* start: the application was built against a
                                     * different header version */
    WILiRFID_FAILED                 /* the operation did not complete */
} WILiRFID_result_t;

/* What the reader is doing, as returned by WILiRFID_state(). See "States in
 * practice" at the top of this header. */
typedef enum {
    WILiRFID_STATE_OFF = 0,     /* not started, or stopped */
    WILiRFID_STATE_STARTING,    /* starting up; cards are reported once READY,
                                 * typically in under a second */
    WILiRFID_STATE_READY,       /* reading; cards held at the antenna are reported */
    WILiRFID_STATE_NO_SIGNAL,   /* the RFID board is not responding (unseated or
                                 * unpowered); the reader recovers by itself once the
                                 * board is usable */
    WILiRFID_STATE_FAULT,       /* an internal error occurred; call WILiRFID_stop() and
                                 * start again */
    WILiRFID_STATE_EMULATING    /* presenting an emulated card to an external reader
                                 * instead of reading */
} WILiRFID_state_t;

/* Which card formats the reader reports. */
typedef enum {
    WILiRFID_SCAN_ALL = 0,  /* every supported format; a card is usually reported
                             * within about a second (default) */
    WILiRFID_SCAN_FAST      /* reports EM4100 and Indala cards only, and reports them
                             * sooner */
} WILiRFID_scan_mode_t;

/* pio_block value that lets the reader choose the PIO block (tries 1, then 2, then 0). */
#define WILiRFID_PIO_AUTO (-1)

/* Options for WILiRFID_start_ex(). */
typedef struct {
    int pio_block;                  /* WILiRFID_PIO_AUTO, or 0, 1 or 2 to use only that
                                     * block */
    WILiRFID_scan_mode_t scan_mode; /* formats to report once started */
} WILiRFID_config_t;

/* Start the reader with the default options ({WILiRFID_PIO_AUTO, WILiRFID_SCAN_ALL}).
 *
 * Call once from the main loop, with the NFC_RFID power zone awake, before any
 * call that needs a running reader. From then until WILiRFID_stop() the reader
 * owns the resources listed at the top of this header; leave them alone.
 *
 * Returns true when the reader started, and sets WILiRFID_last_error() to
 * WILiRFID_OK. Calling it again while the reader runs returns true and changes
 * nothing (options are not re-applied and the last error is left as it was).
 * Returns false when it cannot start, with WILiRFID_last_error() set to:
 *   WILiRFID_API_VERSION_MISMATCH  the application was compiled against a
 *                                  different header version. This is checked
 *                                  only here, at start: rebuild against the
 *                                  header that came with the library;
 *   WILiRFID_HW_UNAVAILABLE_PIO    no PIO block had two free state machines and
 *                                  21 free instruction slots with a usable GPIO
 *                                  base; start the reader before other PIO users,
 *                                  or pin a free block with WILiRFID_start_ex();
 *   WILiRFID_HW_UNAVAILABLE_DMA    the three DMA channels could not be claimed, or
 *                                  the reader itself failed to start; free some
 *                                  DMA channels and start again;
 *   WILiRFID_BAD_ARGUMENT          (WILiRFID_start_ex() only) invalid options.
 * After a false return nothing is left claimed and the state is OFF.
 *
 * Does not wait for the reader to finish starting up: the state is STARTING and
 * moves to READY while WILiRFID_service() is called.
 *
 * WILiRFID_start() and WILiRFID_start_ex() are macros that pass the
 * WILiRFID_API_VERSION of the header the application was compiled with. Call the
 * macros, not the _v functions. */
bool WILiRFID_start_v(uint32_t api_version);

/* As WILiRFID_start(), with options. cfg may be NULL for the defaults. A
 * pio_block other than WILiRFID_PIO_AUTO, 0, 1 or 2, or a scan_mode outside the
 * enum, returns false with WILiRFID_BAD_ARGUMENT. When pio_block names a block,
 * only that block is tried. Options apply only to a start from stopped; calling it
 * while the reader runs returns true and changes nothing. */
bool WILiRFID_start_ex_v(const WILiRFID_config_t *cfg, uint32_t api_version);

#define WILiRFID_start() WILiRFID_start_v(WILiRFID_API_VERSION)
#define WILiRFID_start_ex(cfg) WILiRFID_start_ex_v((cfg), WILiRFID_API_VERSION)

/* Stop the reader: end any emulation, abandon any write-and-check (its status
 * returns to WILiRFID_WRITE_IDLE) and any block read (it ends as
 * WILiRFID_READ_FAILED), drop a read not yet taken by WILiRFID_poll(), and release
 * every resource the reader took. The last read card stays available from
 * WILiRFID_last_tag(). Safe to call when not started. Does not wait for the
 * antenna; afterwards WILiRFID_state() is WILiRFID_STATE_OFF. */
void WILiRFID_stop(void);

/* Let the reader do its work. Call on every pass of the main loop while the
 * reader runs, at least every 20 ms; reading, write checking, block reads and
 * emulation progress only inside this call. Does nothing when not started. It
 * never waits for a card; a normal call takes usually a few milliseconds,
 * occasionally a few tens of milliseconds. Exception: while a write-and-check or a
 * block read is in progress one call can block for up to about two seconds. */
void WILiRFID_service(void);

/* Current reader state. WILiRFID_STATE_OFF when not started. Constant time; safe
 * at any time. */
WILiRFID_state_t WILiRFID_state(void);

/* The result recorded by the most recent start (successful or not) or by the most
 * recent call that returns a WILiRFID_result_t (write, block write, block read,
 * emulate); WILiRFID_OK before any such call. Other calls leave it unchanged,
 * except WILiRFID_emulate_stop(), which records WILiRFID_HW_UNAVAILABLE_PIO or
 * _DMA when the reader could not take its resources back afterwards.
 *
 * After WILiRFID_start() returns false it says why.
 * WILiRFID_HW_UNAVAILABLE_PIO means no PIO block had the state machines or the
 * instruction space (two free state machines, 21 free instruction slots, and the
 * block either unused or already at GPIO base 16): start the reader before the
 * LED, microphone, IR or radio drivers, or require a block you keep free with
 * WILiRFID_config_t.pio_block and WILiRFID_start_ex().
 * WILiRFID_HW_UNAVAILABLE_DMA means the three DMA channels could not be claimed,
 * or the reader itself failed to start: release some DMA channels and start
 * again. Constant time; safe at any time. */
WILiRFID_result_t WILiRFID_last_error(void);

/* Short constant name for a state or result, for logs (for example "READY",
 * "BUSY"): the enum constant without its prefix. Returns "?" for a value outside
 * the enum. Never NULL. Constant time; safe at any time. */
const char *WILiRFID_state_name(WILiRFID_state_t state);
const char *WILiRFID_result_name(WILiRFID_result_t result);

/* ---- Formats: format enum, tag record, capability flags ---- */

/* Card formats the reader recognises. Values are stable across versions. */
typedef enum {
    WILiRFID_FORMAT_UNKNOWN = 0,    /* not recognised, or no format given */
    WILiRFID_FORMAT_EM4100,         /* EM4100 / EM4102, 5-byte id */
    WILiRFID_FORMAT_INDALA,         /* Indala 64-bit */
    WILiRFID_FORMAT_INDALA_224,     /* Indala 224-bit */
    WILiRFID_FORMAT_HID_PROX,       /* HID Prox, facility and card number */
    WILiRFID_FORMAT_HID_PROX_LONG,  /* HID Prox, long card formats */
    WILiRFID_FORMAT_AWID,           /* AWID */
    WILiRFID_FORMAT_IOPROX,         /* Kantech ioProx */
    WILiRFID_FORMAT_PARADOX,        /* Paradox */
    WILiRFID_FORMAT_PYRAMID,        /* Farpointe Pyramid */
    WILiRFID_FORMAT_DESTRON,        /* Destron animal tag */
    WILiRFID_FORMAT_FDX_B,          /* FDX-B animal tag (ISO 11784/11785) */
    WILiRFID_FORMAT_VIKING,         /* Viking */
    WILiRFID_FORMAT_NORALSY,        /* Noralsy */
    WILiRFID_FORMAT_PRESCO,         /* Presco */
    WILiRFID_FORMAT_SECURAKEY,      /* Securakey */
    WILiRFID_FORMAT_JABLOTRON,      /* Jablotron */
    WILiRFID_FORMAT_VISA2000,       /* Visa2000 */
    WILiRFID_FORMAT_PAC,            /* PAC / Stanley */
    WILiRFID_FORMAT_GUARD_PROX_II,  /* Guardall G-Prox II */
    WILiRFID_FORMAT_NEDAP_64,       /* Nedap 64-bit */
    WILiRFID_FORMAT_NEDAP_128,      /* Nedap 128-bit */
    WILiRFID_FORMAT_COUNT           /* number of values above; not a format */
} WILiRFID_format_t;

/* One card, as read (from WILiRFID_poll() or WILiRFID_last_tag()) or as loaded
 * (from WILiRFID_tag_from_string()). WILiRFID_write_begin() and
 * WILiRFID_emulate_begin() take the same record. Plain data: copy it freely. */
typedef struct {
    WILiRFID_format_t format;   /* the card's format; WILiRFID_FORMAT_UNKNOWN when not
                                 * recognised */
    const char *format_name;    /* display name of format, static text, never NULL;
                                 * "Unknown" for UNKNOWN */
    uint8_t  id[8];             /* card id, first id_len bytes valid, rest zero */
    uint8_t  id_len;            /* number of valid bytes in id, 0 to 8 */
    bool     has_facility_card; /* true when facility and card_number are meaningful
                                 * for this card */
    uint32_t facility;          /* facility (site) code; 0 when has_facility_card is
                                 * false */
    uint32_t card_number;       /* card number; 0 when has_facility_card is false */
    uint8_t  raw[28];           /* the card's complete frame as read, MSB first, first
                                 * raw_bits bits valid, rest zero */
    uint16_t raw_bits;          /* number of valid bits in raw, 0 to 224; 0 for a card
                                 * loaded from text */
} WILiRFID_tag_t;

/* Display name for a format, for example "HID Prox"; "Unknown" for UNKNOWN and
 * for out-of-range values. Static text, never NULL. Constant time; safe at any
 * time. */
const char *WILiRFID_format_name(WILiRFID_format_t format);
/* True when a card of this format, as read by the reader, can be written to a
 * blank with WILiRFID_write_begin(). A card of the same format loaded from text
 * may still be refused (see Saved cards below). False for UNKNOWN and out-of-range
 * values. Safe at any time; does not need a running reader. */
bool WILiRFID_format_can_write(WILiRFID_format_t format);
/* True when the reader can emulate a card of this format with
 * WILiRFID_emulate_begin(). In this version, the same set as
 * WILiRFID_format_can_write(). Safe at any time; does not need a running reader. */
bool WILiRFID_format_can_emulate(WILiRFID_format_t format);

/* ---- Reading: poll, scan mode ---- */

/* Take the newest card read and copy it into *out.
 *
 * Returns true once for each new read; returns false (leaving *out untouched)
 * when nothing new has been read since the last call, when out is NULL, or when
 * the reader is not started. A card held still at the antenna is reported once,
 * not repeatedly; it is reported again only after it has been removed (or
 * WILiRFID_forget() was called) and is read again. Use WILiRFID_card_state() and
 * WILiRFID_last_tag() to follow whether the card is still there afterwards. Holds
 * one read: if two cards are read between calls, only the newer is returned.
 * Cards are read inside WILiRFID_service(), so call this from the main loop after
 * it. No cards are reported while emulating, and cards are normally not reported
 * during a block read. Constant time; never blocks. */
bool WILiRFID_poll(WILiRFID_tag_t *out);

/* Choose which formats the reader reports (see WILiRFID_scan_mode_t).
 *
 * WILiRFID_SCAN_FAST reports EM4100 and Indala cards only, and reports them
 * sooner; cards of other formats are not reported. WILiRFID_SCAN_ALL reports every
 * supported format; a card is usually reported within about a second. During a
 * write-and-check (WILiRFID_write_begin()) every format is read back and reported,
 * whatever the scan mode; the mode set here applies again when the write ends.
 *
 * May be called at any time. While the reader runs, the new mode applies from the
 * next WILiRFID_service() call; the card state, the last read card and a read not
 * yet taken by WILiRFID_poll() are kept. Each start uses the scan_mode of its
 * config (WILiRFID_SCAN_ALL for WILiRFID_start()), so a mode set while stopped
 * does not carry over to the next start. A value outside the enum is ignored.
 * Never blocks. */
void WILiRFID_set_scan_mode(WILiRFID_scan_mode_t mode);

/* The scan mode in effect. When stopped, the mode last set or started with.
 * Constant time; safe at any time. */
WILiRFID_scan_mode_t WILiRFID_scan_mode(void);

/* ---- Card presence: present or removed, last tag ---- */

/* Whether a card is at the reader, from WILiRFID_card_state(). Removal is
 * reported only while the reader runs; stopping the reader reports NONE. */
typedef enum {
    WILiRFID_CARD_NONE = 0,     /* no card is being followed: reader stopped, nothing
                                 * read yet, or forgotten */
    WILiRFID_CARD_PRESENT,      /* a card has been read and is still at the antenna */
    WILiRFID_CARD_REMOVED       /* the last card has left; its details stay in
                                 * WILiRFID_last_tag() */
} WILiRFID_card_state_t;

/* Current card state. A new card read moves it to PRESENT; the card leaving moves
 * it to REMOVED shortly afterwards. During a write-and-check it works as usual
 * (the written card shows PRESENT once it is read). During emulation the state
 * keeps its value until emulation ends. During a block read it usually keeps its
 * value, but may still change for cards seen by the normal reads. Constant time;
 * safe at any time. */
WILiRFID_card_state_t WILiRFID_card_state(void);
/* Copy the most recently read card into *out and return true; return false
 * (leaving *out untouched) when there is none or out is NULL. The card stays
 * available after it is removed and after the reader stops, until a new card is
 * read or WILiRFID_forget() is called. Constant time; safe at any time. */
bool WILiRFID_last_tag(WILiRFID_tag_t *out);
/* Drop the remembered card: WILiRFID_last_tag() returns false until a card is read
 * again, and a REMOVED state becomes NONE (a PRESENT state stays PRESENT until the
 * card leaves). The next card read, even the same card still at the antenna, is
 * reported by WILiRFID_poll() as new. Never blocks; safe at any time. */
void WILiRFID_forget(void);

/* ---- Writing: write, clone, verify, presets, block write ---- */

/* A blank (a writable T5577 card) holds eight 32-bit blocks. Block 0 is the
 * configuration block: it tells the card how to answer, so it decides which format
 * a reader sees. Blocks 1 to 6 carry the card's data. Block 7 holds an optional
 * password. A preset is a ready-made block-0 value for one card format.
 *
 * Writing a wrong block 0 can leave a card that no reader recognises; writing the
 * correct preset to block 0 (with confirm_config_block set) makes it readable
 * again. Writing block 7 on a card whose block 0 enables the password locks the
 * card behind that password: keep a record of it.
 *
 * Writes need the reader in READY or STARTING (not NO_SIGNAL, FAULT or
 * EMULATING), with the card held still on the board for the whole write. */

/* How a write-and-check is progressing, from WILiRFID_write_status(). */
typedef enum {
    WILiRFID_WRITE_IDLE = 0,    /* none has run since the reader started, or it was
                                 * cancelled */
    WILiRFID_WRITE_WRITING,     /* the card's blocks are being written (only inside
                                 * WILiRFID_write_begin()) */
    WILiRFID_WRITE_VERIFYING,   /* the copy is being checked */
    WILiRFID_WRITE_VERIFIED,    /* the copy is confirmed */
    WILiRFID_WRITE_MISMATCH,    /* the copy could not be confirmed within about ten
                                 * seconds (no blank, read-only card, wrong password,
                                 * or card moved); the blank may be partly written */
    WILiRFID_WRITE_FAILED       /* the reader could not send the whole write; the blank
                                 * may be partly written */
} WILiRFID_write_status_t;

/* Write the card described by *tag onto the blank at the reader, then check the
 * result.
 *
 * How to use it: have the user lay the blank flat on the back of the FreeWili,
 * over the antenna (show a "hold still" message first); call
 * WILiRFID_write_begin(); then keep calling WILiRFID_service() and watch
 * WILiRFID_write_status() until it is WILiRFID_WRITE_VERIFIED or
 * WILiRFID_WRITE_MISMATCH. The blank must stay still on the back of the FreeWili
 * until then. When the copy is confirmed the status becomes
 * WILiRFID_WRITE_VERIFIED; the new card may also be reported by WILiRFID_poll();
 * other cards are still reported meanwhile. The copy is confirmed only by
 * WILiRFID_write_status(); cards reported by WILiRFID_poll() during the check are
 * informational.
 *
 * WILiRFID_OK does not mean the card was written: it means the write was sent and
 * the check has begun, and the outcome comes from WILiRFID_write_status(). With no
 * blank at the reader, a read-only card, a wrong password, or a card moved away
 * too early, the write is still sent and the status ends as
 * WILiRFID_WRITE_MISMATCH.
 *
 * Needs a running reader that is not emulating. *tag is a card record as read or
 * loaded (which loaded records can be written: see Saved cards below).
 * password_or_null, when not NULL, is the blank's access password. Pass a password
 * only for a card you know is password-protected; passing one to a card that is
 * not can corrupt it, so pass NULL otherwise. A record as read (raw_bits not 0) is
 * reproduced exactly when its format can be copied; otherwise the card is rebuilt
 * from the record's facility and card numbers (HID Prox) or its id (EM4100,
 * Indala).
 *
 * Timing: this call blocks for up to about two seconds while the write is sent.
 * The outcome is known within about ten seconds of the call; while the check is
 * in progress one WILiRFID_service() call can block for up to about two seconds.
 *
 * Results that send nothing and leave the status unchanged: WILiRFID_NOT_RUNNING
 * when the reader is not started; WILiRFID_BUSY when emulating, during another
 * write-and-check or during a block read; WILiRFID_BAD_ARGUMENT when tag is NULL,
 * or the record lacks the numbers or id its format needs, or they are out of
 * range; WILiRFID_UNSUPPORTED_FORMAT when the record cannot be written. When the
 * reader could not send the whole write (the blank may be partly written), the
 * status becomes WILiRFID_WRITE_FAILED and the result is WILiRFID_NOT_RUNNING
 * (the RFID board is not responding: state NO_SIGNAL or FAULT) or
 * WILiRFID_FAILED. To recover such a blank, see WILiRFID_write_cancel(). */
WILiRFID_result_t WILiRFID_write_begin(const WILiRFID_tag_t *tag,
                                       const uint32_t *password_or_null);

/* The state of the most recent WILiRFID_write_begin(). WILiRFID_WRITE_IDLE when
 * none has run since the reader started. A final status (VERIFIED, MISMATCH or
 * FAILED) stays until the next WILiRFID_write_begin(), WILiRFID_write_cancel() or
 * WILiRFID_stop(). Constant time; safe at any time. */
WILiRFID_write_status_t WILiRFID_write_status(void);

/* Abandon a write-and-check in progress and set the status to
 * WILiRFID_WRITE_IDLE. The written blocks are left as they are; only the read-back
 * is stopped. When none is in progress it only clears a final status to IDLE.
 * Stopping the reader also abandons one. A cancelled or failed clone can leave the
 * blank with mixed contents; writing a preset to block 0 and cloning again
 * recovers it. Constant time; safe at any time. */
void WILiRFID_write_cancel(void);

/* Write one 32-bit value to a single block (0 to 7) of the blank at the reader.
 *
 * Needs a running reader that is not emulating, with no write-and-check or block
 * read in progress. Block 0 (configuration) and block 7 (password) are written
 * only when confirm_config_block is true, since a wrong value there can make the
 * card unreadable or locked; for blocks 1 to 6 confirm_config_block is ignored.
 * password_or_null, when not NULL, is the card's access password; pass a password
 * only for a card you know is password-protected, since passing one to a card that
 * is not can corrupt it, and pass NULL otherwise. This is a direct write with no
 * read-back: WILiRFID_OK means the write was sent, even when no card was there to
 * take it; use WILiRFID_t5577_read_block_begin() to check it. Blocks for up to
 * about two seconds while the write is sent.
 *
 * Returns WILiRFID_OK when the write was sent; WILiRFID_NOT_RUNNING when the
 * reader is not started or the RFID board is not responding; WILiRFID_BUSY when
 * emulating, during a write-and-check or during a block read;
 * WILiRFID_BAD_ARGUMENT when block is greater than 7; WILiRFID_PROTECTED_BLOCK
 * for block 0 or 7 without confirm_config_block; WILiRFID_FAILED when the block
 * could not be written. To recover a card made unreadable by a wrong block 0,
 * write the matching preset value (see WILiRFID_t5577_config_for() and
 * WILiRFID_t5577_preset()) to block 0 with confirm_config_block true. */
WILiRFID_result_t WILiRFID_t5577_write_block(uint8_t block, uint32_t value,
                                             const uint32_t *password_or_null,
                                             bool confirm_config_block);

/* One named configuration-block preset. All text is static and never NULL. */
typedef struct {
    const char *name;           /* short key for the preset */
    const char *description;    /* plain name of the card format it selects */
    uint32_t config;            /* the block-0 value */
} WILiRFID_t5577_preset_t;

/* How many presets WILiRFID_t5577_preset() offers. Constant; safe at any time. */
unsigned WILiRFID_t5577_preset_count(void);
/* The preset at index i (0 to WILiRFID_t5577_preset_count() - 1), or NULL for a
 * larger i. The returned record is static. Safe at any time. */
const WILiRFID_t5577_preset_t *WILiRFID_t5577_preset(unsigned i);

/* The block-0 value a fresh WILiRFID_write_begin() of this format would use, or 0
 * for a format the reader does not write. Useful to set block 0 directly with
 * WILiRFID_t5577_write_block(). Constant time; safe at any time. */
uint32_t WILiRFID_t5577_config_for(WILiRFID_format_t format);

/* ---- Block read (asynchronous) ---- */

/* Reading the memory of a T5577 card: eight 32-bit blocks, numbered 0 to 7.
 * Block 0 is the card's configuration (how it answers a reader); blocks 1 to 6
 * hold the card's data; block 7 holds the password when the card is
 * password-protected. A block read takes the reader away from normal reading
 * for a moment, then hands it back.
 *
 * Read block 0 first: on some cards data blocks read reliably only after block 0
 * of the same card has been read, and block 0 says whether the card is
 * password-protected (bit 4, see Blanks at the top of this header). A protected
 * card read without its password ends as WILiRFID_READ_FAILED or returns an
 * unreliable value; check block 0 first. */

/* Progress of the most recent block read, from WILiRFID_t5577_read_block_status(). */
typedef enum {
    WILiRFID_READ_IDLE = 0,     /* no block read has been started yet */
    WILiRFID_READ_READING,      /* a block read is in progress */
    WILiRFID_READ_DONE,         /* the block was read; its value is available */
    WILiRFID_READ_FAILED        /* the block could not be read (no card, the read did
                                 * not succeed, or the reader stopped) */
} WILiRFID_read_status_t;

/* Start reading one block of the T5577 card held at the reader.
 *
 * Needs a running reader that is not emulating, with no write-and-check or other
 * block read in progress. block is 0 to 7. password_or_null is NULL for a card
 * that is not password-protected, which is the usual case. Pass a password only
 * for a card you know is password-protected (block 0 of such a card says so);
 * passing one to a card that is not password-protected can corrupt the card. Pass
 * NULL otherwise.
 *
 * Returns within a few tens of milliseconds; the read completes inside
 * WILiRFID_service(). Follow it with WILiRFID_t5577_read_block_status(). A block
 * read completes within about six seconds. While it runs, WILiRFID_service() may
 * take longer than usual (see Service cadence at the top of this header), cards
 * are normally not reported, and writes and emulation return WILiRFID_BUSY.
 *
 * Returns WILiRFID_OK when the read has started; WILiRFID_NOT_RUNNING when the
 * reader is not started; WILiRFID_BUSY while emulating, during a write-and-check
 * or during another block read; WILiRFID_BAD_ARGUMENT when block is above 7. A
 * missing card is not reported here: the read ends as WILiRFID_READ_FAILED. */
WILiRFID_result_t WILiRFID_t5577_read_block_begin(uint8_t block,
                                                  const uint32_t *password_or_null);

/* Progress of the most recent block read. Constant time; safe at any time.
 *
 * Returns WILiRFID_READ_READING from a successful WILiRFID_t5577_read_block_begin()
 * until the read ends. WILiRFID_READ_DONE and WILiRFID_READ_FAILED stay until the
 * next successful begin. On WILiRFID_READ_DONE the block's 32-bit value is written
 * to *value_out; value_out may be NULL, and is left untouched for every other
 * status. Stopping the reader during a read ends it as WILiRFID_READ_FAILED. */
WILiRFID_read_status_t WILiRFID_t5577_read_block_status(uint32_t *value_out);

/* ---- Emulation ---- */

/* Emulation makes the board present itself to an external reader (a door reader,
 * for example) as the card in a tag record, instead of reading cards. Hold the
 * back of the FreeWili, where the antenna is, against the external reader the
 * way you would hold the card it stands for.
 *
 * While emulating, the state is WILiRFID_STATE_EMULATING, no cards are reported,
 * and WILiRFID_write_begin(), WILiRFID_t5577_write_block() and
 * WILiRFID_t5577_read_block_begin() return WILiRFID_BUSY. They work again once
 * emulation stops, and reading resumes by itself. Emulation runs until
 * WILiRFID_emulate_stop() or WILiRFID_stop(); keep calling WILiRFID_service(). */

/* Emulation profile. DEFAULT is the profile to try first; if a reader does not
 * pick up the card, try ALT_1, then ALT_2, then the same three with `alternate`
 * set. */
typedef enum {
    WILiRFID_EMU_DEFAULT = 0,   /* the profile to try first */
    WILiRFID_EMU_ALT_1,         /* first alternative profile, for readers DEFAULT does
                                 * not reach */
    WILiRFID_EMU_ALT_2          /* second alternative profile */
} WILiRFID_emu_profile_t;

/* Options for WILiRFID_emulate_begin_ex(). */
typedef struct {
    WILiRFID_emu_profile_t profile; /* emulation profile (see above) */
    bool alternate;                 /* an alternative some readers need; try it after
                                     * the three profiles */
} WILiRFID_emu_options_t;

/* A snapshot of emulation, from WILiRFID_emulate_status(). */
typedef struct {
    bool active;                  /* true while a card is being emulated */
    bool reader_field_detected;   /* true when an external reader is detected at the
                                   * board; false when not active */
    uint32_t elapsed_ms;          /* milliseconds since the current emulation began, 0
                                   * when not active */
    WILiRFID_format_t format;     /* the emulated card's format,
                                   * WILiRFID_FORMAT_UNKNOWN when not active */
} WILiRFID_emu_status_t;

/* Begin emulating the card in *tag with the default options
 * ({WILiRFID_EMU_DEFAULT, false}).
 *
 * The reader must already be running: this call does not start it. The formats
 * that can be emulated are those WILiRFID_format_can_emulate() accepts. Does not
 * wait for an external reader.
 *
 * Returns WILiRFID_OK once the board is presenting the card. Returns
 * WILiRFID_BAD_ARGUMENT when tag is NULL (checked first, even when not running);
 * WILiRFID_NOT_RUNNING when the reader is not started; WILiRFID_BUSY when a card
 * is already being emulated, or during a write-and-check or a block read;
 * WILiRFID_UNSUPPORTED_FORMAT when the format cannot be emulated;
 * WILiRFID_BAD_ARGUMENT when the record lacks the numbers its format needs or they
 * are out of range; WILiRFID_FAILED when the board could not start presenting the
 * card. After WILiRFID_FAILED, stop and start the reader and retry. */
WILiRFID_result_t WILiRFID_emulate_begin(const WILiRFID_tag_t *tag);

/* As WILiRFID_emulate_begin(), with options. opt may be NULL for the defaults. An
 * out-of-range profile returns WILiRFID_BAD_ARGUMENT (checked before the running
 * check). */
WILiRFID_result_t WILiRFID_emulate_begin_ex(const WILiRFID_tag_t *tag,
                                            const WILiRFID_emu_options_t *opt);

/* Stop emulating; reading resumes by itself. Does nothing when not emulating.
 * Safe at any time. If the reader cannot take its resources back afterwards,
 * WILiRFID_last_error() becomes WILiRFID_HW_UNAVAILABLE_PIO or _DMA and the
 * reader keeps running without them; stop and start it to recover. */
void WILiRFID_emulate_stop(void);

/* True while a card is being emulated. Constant time; safe at any time. */
bool WILiRFID_emulating(void);

/* A snapshot of emulation. When not emulating, active and reader_field_detected
 * are false, elapsed_ms is 0 and format is WILiRFID_FORMAT_UNKNOWN. Use
 * reader_field_detected to show the user that the board is close enough to the
 * external reader. Constant time; safe at any time. */
WILiRFID_emu_status_t WILiRFID_emulate_status(void);

/* ---- Saved cards: text helpers ---- */

/* The device keeps saved cards under /rfid/saved as <name>.rfid files. Cards saved
 * by an application with these helpers load on the device, and the other way
 * round. /rfid/saved is the deliberate exception to the BSP rule that an app keeps
 * its files under /appdata/<app>, so that the device and apps share these files.
 * None of these helpers needs a running reader; all are safe at any time. */
#define WILiRFID_SAVED_DIR "/rfid/saved"   /* folder of saved cards on the device */
#define WILiRFID_SAVED_EXT ".rfid"         /* file-name extension of a saved card */
/* Longest name WILiRFID_tag_name() writes, terminator included. */
#define WILiRFID_TAG_NAME_MAX 40

/* A saved card is one line of plain text ending in a newline:
 *     rfid1 fmt=<format number> fc=<facility> cn=<card number> id=<hex>
 * The numbers are decimal. The format number is an internal code; let these
 * helpers convert it rather than writing the line by hand. The id is upper-case
 * hex: the id bytes as the card has them. The id= field is always written and is
 * empty when the card has no id.
 *
 * Every card the reader reports can be saved. The line keeps the facility and card
 * numbers and the card's id bytes. On load the id is used when it has exactly the
 * standard width (5 bytes, 8 for Indala); otherwise the card loads from its
 * numbers, which is how cards with facility and card numbers, such as HID Prox,
 * travel.
 *
 * Only EM4100, HID Prox and Indala can be cloned again after loading from a saved
 * record. Every other format is display-only after reload, because the record
 * holds numbers, not the whole card; an app that must clone such a card keeps the
 * live WILiRFID_tag_t, as read, in RAM.
 *
 * What a loaded card can do (it has no frame, so raw_bits is 0):
 *   - EM4100 and Indala write and emulate exactly as the original card.
 *   - HID Prox writes and emulates from its facility and card numbers, written
 *     as a standard HID card with those numbers; a reader expecting another HID
 *     layout may see a different card.
 *   - Every other format loads for display only: WILiRFID_write_begin() and
 *     WILiRFID_emulate_begin() return WILiRFID_UNSUPPORTED_FORMAT for it. */

/* Write the saved-card line for a card into buf (including the newline),
 * NUL-terminated, and return true. The id bytes are written exactly as present
 * (id_len 0 to 8). Returns false only when tag or buf is NULL or len is too small
 * for the line and its terminator; buf then holds an empty string when len is at
 * least 1. A line is always shorter than 96 bytes. */
bool WILiRFID_tag_to_string(const WILiRFID_tag_t *tag, char *buf, size_t len);
/* Read a saved-card line into *out and return true. Only the text up to the first
 * newline is read, so a whole file can be passed. Returns false when out or text
 * is NULL or that line is not a saved card (no leading record tag, or a format,
 * facility or card number that is missing or has no digit right after its "=");
 * *out is then cleared (format UNKNOWN, no id) when out is not NULL. The id is used
 * only when it is exactly the standard width (10 hex digits, 16 for Indala)
 * followed by a space, a tab, a carriage return or the end of the line; an id that
 * is absent, shorter, longer or not hex is treated as no id (id_len 0), never cut
 * short, and the card still loads from its numbers. An unrecognised format number
 * reads as UNKNOWN. facility and card_number are filled only for formats that have
 * them; raw_bits is always 0. */
bool WILiRFID_tag_from_string(const char *text, WILiRFID_tag_t *out);
/* Write the file-name stem the device uses when it saves this card, NUL-terminated,
 * for use between WILiRFID_SAVED_DIR "/" and WILiRFID_SAVED_EXT. The stem holds only
 * lower-case letters, digits and '-': the id for EM4100, facility and card number
 * for every other format. A buf of WILiRFID_TAG_NAME_MAX bytes is always large
 * enough. Returns false when tag or buf is NULL, buf is too small, or the card has
 * nothing to name. */
bool WILiRFID_tag_name(const WILiRFID_tag_t *tag, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
