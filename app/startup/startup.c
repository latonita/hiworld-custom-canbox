/* ============================================================================
 * app/startup/startup.c  --  GCC-native replacement for the ARM C-library
 * __main / __scatterload / __rt_entry chain that the stock image runs.
 *
 * Stock path (reference image):
 *     0x08002202 veneer -> __main @0x08002400
 *                            __scatterload (Region$$Table @0x08002434): RW copy + ZI zero
 *                          -> __rt_entry @0x0800200c
 *                               __rt_stackheap_init   (no-op for us, see below)
 *                            -> call_main @0x0800220a -> main_loop
 *
 * Our path:
 *     0x08002202 veneer -> _start_c -> (RW copy + ZI zero) -> main_loop
 *
 * Deliberately NOT done here, matching the stock app:
 *   - no SP setup      : the bootloader already set MSP = 0x20000600
 *   - no VTOR relocate : the app runs on the bootloader's table at 0x08000000
 *   - no clock setup   : the bootloader's InitializeSysClock already did
 *                        HSE 16 MHz -> PLL -> SYSCLK 64 MHz (APB1 = 16 MHz)
 *   - no heap          : the stock app never calls malloc
 *
 * The app's own named globals live at FIXED absolute addresses (globals.h) and
 * are NOT linker-allocated, so they are not covered by the copy/zero below.
 * They keep whatever the stock code explicitly initialises at runtime -- which
 * is what the stock firmware does too (its Region$$Table ZI-zeroes that block;
 * see _zero_fixed_globals below).
 * ==========================================================================*/

#include <stdint.h>

/* Provided by app/linker/app.ld */
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

extern void main_loop(void);
extern void fatal_error_reset(uint8_t code);

/* The stock image's Region$$Table ZI-zeroes the fixed global block. Reproduce
 * that: everything from the first global to the end of the block is cleared
 * before main_loop runs, so statics behave as the original did. Bounds come
 * from app/include/globals.h / app/MEMORY_MAP.md. */
#define FIXED_GLOBALS_START  0x20001000U
#define FIXED_GLOBALS_END    0x20001C24U

__attribute__((used))
void _start_c(void)
{
    uint32_t *src, *dst;

    /* RW data: copy initialisers from flash to SRAM. */
    for (src = &_sidata, dst = &_sdata; dst < &_edata; )
        *dst++ = *src++;

    /* ZI data: zero linker-managed .bss. */
    for (dst = &_sbss; dst < &_ebss; )
        *dst++ = 0;

    /* ZI data: zero the fixed absolute global block (stock Region$$Table does
     * the same; the linker does not know about these addresses). */
    for (dst = (uint32_t *)FIXED_GLOBALS_START;
         dst < (uint32_t *)FIXED_GLOBALS_END; )
        *dst++ = 0;

    main_loop();

    /* main_loop never returns. If it somehow does, take the stock exit path's
     * end state: fatal_error_reset(100) -> WWDG reset. */
    fatal_error_reset(100);
    for (;;)
        ;
}
