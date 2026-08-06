/* PSRAM-app override: retire the separate PSRAM region.
 *
 * The default layout declares PSRAM(rwx) at 0x11000000 for the SDK's optional
 * .psram_load/.psram_noload sections. For a PSRAM app that same address IS the
 * XIP region (see pico_flash_region.ld), so leaving both at 0x11000000 gives
 * two regions claiming one address, and the empty .psram_* output sections
 * would be placed on top of the vector table.
 *
 * Length 0 keeps the region defined -- sections_psram.incl still references it
 * -- while making it impossible to place anything there by accident.
 */
PSRAM(rwx) : ORIGIN = 0x11000000, LENGTH = 0
