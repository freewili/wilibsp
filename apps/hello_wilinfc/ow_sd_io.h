/* ow_sd_io - a WILiNFC file-io backend that stores files on the SD card
 * through the OneWili SD client.
 *
 * WILiNFC has no filesystem of its own. Saved cards and MIFARE key lists go
 * through the hooks in WILiNFC_file_io_t, and this file is a ready-made set of
 * hooks you can copy into your own app. It is app source, not part of the
 * library: change it freely. read_line reports an SD read error, or a line
 * too long for the library's buffer, with a negative return, so the storage
 * call fails instead of reading a short file.
 *
 * Usage:
 *   fw2_app_recovery_open_onewili(&dev); fw2_app_recovery_wrap_sd();
 *   ow_sd_io_init(&dev);
 *   if (ow_sd_io_ensure_dir())                // creates /appdata/hello_wilinfc
 *       WILiNFC_set_file_io(ow_sd_io_get());
 */
#ifndef OW_SD_IO_H
#define OW_SD_IO_H

#include <stdbool.h>
#include "wilinfc/WILiNFC.h"
#include "onewili.h"

/* App-owned storage lives under /appdata/<app-name>/ (docs/app-storage.md). */
#define OW_SD_IO_DIR "/appdata/hello_wilinfc"

/* Remember the link. Call fw2_app_recovery_wrap_sd() first (it arms the SD
 * client so the recovery task keeps running during SD transfers). */
void ow_sd_io_init(ow_device *dev);

/* The hooks, ready for WILiNFC_set_file_io(). The pointer is valid forever. */
const WILiNFC_file_io_t *ow_sd_io_get(void);

/* Create /appdata and /appdata/hello_wilinfc if missing. */
bool ow_sd_io_ensure_dir(void);

#endif
