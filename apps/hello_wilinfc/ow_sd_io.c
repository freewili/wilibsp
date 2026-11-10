/* ow_sd_io - see ow_sd_io.h. One file may be open at a time, which is all the
 * library needs: it opens, reads or writes lines, then closes. */
#include "ow_sd_io.h"
#include "fw2.h"
#include "onewili_sd.h"
#include <string.h>

static ow_device *s_dev;
static ow_sd_file s_file;
static bool s_open;

/* Read-ahead buffer: SD reads are round trips to the main CPU, so fetch a
 * block at a time and hand out one line per read_line call. */
static char s_buf[256];
static size_t s_buf_len, s_buf_pos;
static bool s_eof;     /* the file has no more bytes */
static bool s_error;   /* an SD read failed; every later read_line reports it */

static bool io_open(void *ctx, const char *path, bool write)
{
    (void)ctx;
    if (!s_dev || s_open) return false;
    if (ow_sd_open(s_dev, &s_file, path, write ? OW_SD_WRITE : OW_SD_READ) != OW_OK)
        return false;
    s_open = true;
    s_buf_len = s_buf_pos = 0;
    s_eof = false;
    s_error = false;
    return true;
}

/* Make sure the read-ahead buffer holds at least one byte. Returns 1 when it
 * does, 0 at end of file and -1 after an SD read error. */
static int fill(void)
{
    if (s_error) return -1;
    if (s_buf_pos < s_buf_len) return 1;
    if (s_eof) return 0;
    size_t got = 0;
    fw2_app_recovery_task();
    if (ow_sd_read(&s_file, s_buf, sizeof s_buf, &got) != OW_OK) {
        s_error = true;
        return -1;
    }
    /* got < len is end of file (onewili_sd.h). */
    s_buf_len = got;
    s_eof = got < sizeof s_buf;
    s_buf_pos = 0;
    if (got == 0) {
        s_eof = true;
        return 0;
    }
    return 1;
}

/* Writes the next line into buf, NUL-terminated and including its line end
 * when it has one (the library strips it). Returns the number of bytes before
 * the NUL, 0 at end of file, and -1 after an SD read error or for a line too
 * long for buf (the library does not support longer lines). */
static int io_read_line(void *ctx, char *buf, int max_len)
{
    (void)ctx;
    if (!s_open || max_len < 2) return -1;
    int n = 0;
    for (;;) {
        int f = fill();
        if (f < 0) return -1;
        if (f == 0) break;                     /* end of file */
        char c = s_buf[s_buf_pos];
        if (n == max_len - 1) {
            /* buf is full: only the line end itself may follow. */
            if (c == '\n') { s_buf_pos++; break; }
            if (c == '\r') { s_buf_pos++; continue; }
            return -1;
        }
        s_buf_pos++;
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return n;
}

/* The library passes the line WITHOUT a newline, then calls again with "\n";
 * so this simply appends the text it is given. */
static bool io_write_line(void *ctx, const char *line)
{
    (void)ctx;
    if (!s_open) return false;
    size_t len = strlen(line);
    if (len == 0) return true;
    fw2_app_recovery_task();
    return ow_sd_write(&s_file, line, len) == OW_OK;
}

static bool io_close(void *ctx)
{
    (void)ctx;
    if (!s_open) return false;
    s_open = false;
    /* Writes are fire-and-forget; close is where a dropped chunk shows up. */
    return ow_sd_close(&s_file) == OW_OK;
}

/* True for a directory that already exists, as the library expects. */
static bool io_mkdir(void *ctx, const char *path)
{
    (void)ctx;
    if (!s_dev) return false;
    if (ow_sd_mkdir(s_dev, path) == OW_OK) return true;
    bool is_dir = false;
    uint32_t size = 0;
    return ow_sd_stat(s_dev, path, &is_dir, &size) == OW_OK && is_dir;
}

static const WILiNFC_file_io_t s_io = {
    NULL, io_open, io_read_line, io_write_line, io_close, io_mkdir
};

void ow_sd_io_init(ow_device *dev)
{
    s_dev = dev;
    s_open = false;
}

const WILiNFC_file_io_t *ow_sd_io_get(void) { return &s_io; }

bool ow_sd_io_ensure_dir(void)
{
    return io_mkdir(NULL, "/appdata") && io_mkdir(NULL, OW_SD_IO_DIR);
}
