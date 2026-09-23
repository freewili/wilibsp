#include <stdint.h>
#include "hardware/sync.h"
#include "pico/time.h"
#include "platform/diag.h"

extern uint32_t __data_load_source__, __data_start__, __data_end__;
extern uint32_t __bss_start__, __bss_end__;
extern uint32_t __vectors;
typedef void (*init_fn_t)(void);
extern init_fn_t __init_array_start[], __init_array_end[];
extern void board_init_psram(void);
extern void runtime_init_mutex(void);
extern int main(void);

static __attribute__((section(".sram_bootstrap")))
void run_init_array(init_fn_t *begin, init_fn_t *end) {
    while (begin < end)
        (*begin++)();
}

__attribute__((section(".sram_bootstrap"), noreturn, used,
               optimize("no-tree-loop-distribute-patterns")))
void fw2_psram_bootstrap(void) {
    volatile uint32_t *src = &__data_load_source__;
    for (volatile uint32_t *dst = &__data_start__; dst < &__data_end__;)
        *dst++ = *src++;
    for (volatile uint32_t *dst = &__bss_start__; dst < &__bss_end__;)
        *dst++ = 0;
    *(volatile uint32_t *)0xe000ed08u = (uint32_t)(uintptr_t)&__vectors;
    /* The UART loader is another SDK application and can hand off while a
     * hardware spin lock used by its alarm pool remains owned.  Our .bss has
     * fresh claim bookkeeping, so inherited lock state is otherwise invisible
     * and the first sleep_us()/sleep_ms() blocks forever.  Recreate the two
     * runtime services RAM apps rely on without running the full cold-boot
     * runtime initializer (which would reset QMI beneath this PSRAM image). */
    spin_locks_reset();
    runtime_init_mutex();
    runtime_init_default_alarm_pool();
    DIAG("psram: bootstrap copied\n");
    /* The second-stage loader enters with interrupts disabled.  Keep them
     * disabled while board_init_psram() changes clocks and re-times QMI: the
     * vector table and normal handlers live in PSRAM, so servicing an IRQ in
     * that window would fetch instructions from the bus being reconfigured. */
    board_init_psram();
    __asm volatile("cpsie i" ::: "memory");
    /* The normal SDK reset handler runs these before main(). PSRAM apps
     * bypass that handler so it cannot reset QMI beneath their image. */
    /* Do not run .preinit_array: Pico SDK runtime initializers live there
     * and include cold-boot hardware setup that is unsafe for an inherited
     * PSRAM image. .init_array contains the language-level constructors. */
    run_init_array(__init_array_start, __init_array_end);
    DIAG("psram: constructors complete\n");
    (void)main();
    for (;;) {}
}
