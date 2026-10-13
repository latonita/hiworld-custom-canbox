/* ============================================================================
 * app/startup/startup.c  --  GCC-native replacement for the ARM C-library
 * __main / __scatterload / __rt_entry chain that the stock image runs.
 *
 * Stock path (reference image):
 *     0x08002202 veneer -> __main @0x08002400
 *                            __scatterload over Region$$Table @0x08009F8C
 *                          -> __rt_entry @0x0800200c
 *                            -> call_main @0x0800220a -> main_loop
 *
 * Our path:
 *     0x08002202 veneer -> _start_c -> (RAM init) -> main_loop
 *
 * ---- THE RAM INIT THIS MUST REPRODUCE -------------------------------------
 * Decoded from the stock scatter table itself (Region$$Table @0x08009F8C ->
 * three 16-byte {src,dst,size,fn} entries; fn 0x08002478 is
 * __scatterload_zeroinit, fn 0x0800243C is __scatterload_decompress):
 *
 *   dst 0x20000000  size 0x0600  zero-init   the stack region
 *   dst 0x20001000  size 0x01D0  decompress  RW data
 *   dst 0x200011D0  size 0x1E58  zero-init   -> ends at 0x20003028
 *
 * Decompressing that 0x1D0 RW block yields 464 bytes that are ALL ZERO except
 * three bytes at 0x200010DD..DF, which together with the zero at 0x200010DC
 * form the word:
 *
 *     *(uint32_t *)0x200010DC = 0x03D09000 = 64000000
 *
 * i.e. g_boot_sysclk_hz (CMSIS SystemCoreClock) = 64 MHz -- an independent
 * confirmation of the HSE 16 MHz -> PLL -> 64 MHz tree the bootloader sets up.
 *
 * So the whole of this firmware's initialised data is that single word, and
 * correct RAM init is: zero 0x20001000..0x20003028, then write SystemCoreClock.
 *
 * NOTE on the stack region: the stock table zero-inits 0x20000000..0x20000600,
 * but we deliberately do NOT. We are *running on* that stack (the bootloader
 * left MSP = 0x20000600 and it grows down), so zeroing it would clobber our own
 * frame. Zeroing stack has no semantic effect -- nothing reads uninitialised
 * stack -- so skipping it is safe and strictly less risky than reproducing
 * whatever register-only ordering lets the ARM C-lib get away with it.
 *
 * Deliberately NOT done here, matching the stock app:
 *   - no SP setup      : the bootloader already set MSP = 0x20000600
 *   - no VTOR relocate : the app runs on the bootloader's table at 0x08000000
 *   - no clock setup   : the bootloader already did HSE -> PLL -> 64 MHz
 *   - no heap          : the stock app never calls malloc
 * ==========================================================================*/

#include <stdint.h>

/* Provided by app/linker/app.ld. These cover only statics the rebuilt C adds;
 * they live ABOVE the stock block (VMA 0x20003040) and never overlap it. */
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

extern void main_loop(void);
extern void fatal_error_reset(uint8_t code);

/* Stock RAM block -- bounds from the stock Region$$Table, NOT from globals.h
 * (which only names symbols up to 0x20001C20 and is incomplete). */
#define APP_RAM_START    0x20001000U
#define APP_RAM_END      0x20003028U

/* The firmware's only initialised datum. */
#define SYSCLK_SHADOW    0x200010DCU
#define SYSCLK_HZ        64000000U

__attribute__((used))
void _start_c(void)
{
    uint32_t *src, *dst;

    /* 1. Zero the stock app's RAM block (RW + ZI regions together). */
    for (dst = (uint32_t *)APP_RAM_START; dst < (uint32_t *)APP_RAM_END; )
        *dst++ = 0;

    /* 2. The one word of initialised data the stock scatter table carries. */
    *(volatile uint32_t *)SYSCLK_SHADOW = SYSCLK_HZ;

    /* 3. Linker-managed statics introduced by the rebuilt C (above the stock
     *    block). .data is empty today but will not stay that way. */
    for (src = &_sidata, dst = &_sdata; dst < &_edata; )
        *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; )
        *dst++ = 0;

    main_loop();

    /* main_loop never returns. If it somehow does, take the stock exit path's
     * end state: fatal_error_reset(100) -> WWDG reset -> back in the loader. */
    fatal_error_reset(100);
    for (;;)
        ;
}
