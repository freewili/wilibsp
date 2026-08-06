/* Runtime-init override for a FreeWili 2 display PSRAM app.
 *
 * An app executing from the 0x11000000 PSRAM window has the same constraint a
 * flash app has -- do not disturb the memory you are fetching instructions
 * from -- but the SDK only knows about the flash case.
 *
 * pico_runtime_init/runtime_init.c spares the QSPI pads when it resets
 * peripherals, because flash lives on the dedicated QSPI pads. The FreeWili 2's
 * PSRAM is an APS6404L on QMI CS1 whose chip select is GPIO47 -- an ordinary
 * BANK-0 pin, muxed to GPIO_FUNC_XIP_CS1 by whoever brought the PSRAM up
 * (FW2PsramStub, here). RESET_IO_BANK0 / RESET_PADS_BANK0 are NOT spared, so
 * runtime_init_early_resets() reverts GPIO47 to its default function and the
 * window stops decoding, mid runtime-init -- before main(), before even the
 * static constructors. The core ends up in LOCKUP rather than reporting a
 * HardFault, because VTOR still points at the loader stub's vector table.
 *
 * The fix is to spare the IO bank the PSRAM chip select lives in, exactly as
 * the SDK already spares the QSPI pads for flash. Diagnosis and the
 * tried-and-reverted alternative (un-muxing GPIO47 and restoring it by hand --
 * the window does not survive being un-muxed even briefly) are recorded in
 * fw2_nes: piconesPlus/pico_shared/fw2_psram_runtime.c.
 *
 * KNOWN COST: every bank-0 pad keeps whatever the loader stub left it as. In
 * fw2_nes that left the I2C1 devices unreachable -- and this app needs two of
 * them (the PCAL6524 expander, which releases the LCD reset per wilibsp
 * invariant 7, and the FT6336 touch controller). Unlike fw2_nes, the stock
 * display firmware runs before the launch and configures both, so they may
 * already be in a working state; if not, the recovery is to restore the GPIO
 * 26/27 pad state by hand before board_i2c1_init().
 */
#if FW2_PSRAM_APP

#include "hardware/resets.h"
#include "pico/runtime_init.h"

/* Mirrors runtime_init_early_resets() from pico_runtime_init, plus
 * RESET_IO_BANK0 / RESET_PADS_BANK0 in the spared set. Kept in SRAM so no part
 * of it is fetched through the window it is protecting. */
void __no_inline_not_in_flash_func(runtime_init_early_resets)(void) {
    reset_block_mask(~(
            (1u << RESET_IO_QSPI) |
            (1u << RESET_PADS_QSPI) |
            /* --- PSRAM app: XIP CS1 is GPIO47, a bank-0 pin. Resetting these
             * two un-muxes it and kills the window we execute from. --- */
            (1u << RESET_IO_BANK0) |
            (1u << RESET_PADS_BANK0) |
            /* --- end addition --- */
            (1u << RESET_PLL_USB) |
            (1u << RESET_USBCTRL) |
            (1u << RESET_SYSCFG) |
            (1u << RESET_PLL_SYS)
    ));

    /* Remove reset from peripherals which are clocked only by clk_sys and
     * clk_ref. Other peripherals stay in reset until clocks are configured. */
    unreset_block_mask_wait_blocking(RESETS_RESET_BITS & ~(
            (1u << RESET_HSTX) |
            (1u << RESET_ADC) |
            (1u << RESET_SPI0) |
            (1u << RESET_SPI1) |
            (1u << RESET_UART0) |
            (1u << RESET_UART1) |
            (1u << RESET_USBCTRL)
    ));
}

#endif /* FW2_PSRAM_APP */
