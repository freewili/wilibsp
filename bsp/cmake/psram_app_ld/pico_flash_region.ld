/* PSRAM-app override: put the "flash" (XIP) region in PSRAM.
 *
 * A FreeWili 2 display PSRAM app is structurally an ordinary XIP image whose
 * execute-in-place window is the 8 MB APS6404L on QMI CS1 at 0x11000000 rather
 * than the flash on CS0 at 0x10000000. Everything else about the default layout
 * is right: .text/.rodata execute from the window, crt0 copies .data to SRAM,
 * and __not_in_flash_func() still lands hot code in SRAM at boot.
 *
 * Why not a RAM app: the fused UART bootloader stages only
 * BL_STAGE_BYTES = 192 KiB in one shot (a RAM load never sends 'F', so the
 * shell never flips its staging halves). That is the real ceiling -- NOT the
 * 448 KiB BL_LOAD_SIZE_MAX the loader handoff doc quotes -- and this app's
 * ~257 KiB image is refused by both `a\r` and `h\v\a` with "image too large".
 * FW2PsramStub stages into this window instead and has the whole 8 MB.
 */
FLASH(rx) : ORIGIN = 0x11000000, LENGTH = (8 * 1024 * 1024)
