/* ============================================================================
 * hw_wwdg.c -- window watchdog, ported from app/src/wwdg.c + sys.c (Ghidra).
 *
 * The WWDG is the app's liveness guarantee AND its reset mechanism: every fatal
 * path deliberately writes an out-of-window counter to force a reset, which
 * re-enters the bootloader (so a wedged app always comes back flashable).
 *
 * Timing with APB1 = 16 MHz:
 *     WWDG base = PCLK1 / 4096             = 3906.25 Hz
 *     WDGTB = 0b10 (/4)                    =  976.56 Hz  -> 1.024 ms per count
 *     counter 0x7F -> 0x3F is 64 counts    = ~65.5 ms timeout
 * which matches the documented ~65.5 ms. main_loop must kick faster than that.
 *
 * Register layout verified against the vendor SDK (WWDG_Module):
 *     CTRL +0x00 (bit7 WDGA | counter[6:0])
 *     CFG  +0x04 (bit9 EWINT | bits8:7 WDGTB | window[6:0])
 * ==========================================================================*/

#include "port.h"

#define WWDG_CTRL_WDGA      0x80U
#define WWDG_CFG_WDGTB_MASK 0x180U
#define WWDG_CFG_W_MASK     0x7FU

/* --- 0x08002d3e --- Reload to the maximum counter. Called every main_loop
 * pass; the window is wide open (W = 0x7F) so an early kick is never a fault. */
void wwdg_kick(void)
{
    WWDG->CTRL = 0xFFU;
}

/* --- 0x08002e26 / 0x08002e2e --- Force an immediate reset.
 * 0xBF is WDGA set with counter 0x3F, i.e. already at/below the reload floor,
 * so the watchdog fires at once. Looping keeps us here until it does. */
void wwdg_force_reset(void)
{
    for (;;)
        WWDG->CTRL = 0xBFU;
}

/* --- 0x080073be --- CFG.WDGTB = prescaler (pass the value pre-shifted, as the
 * stock callers do: 0x100 = 0b10 = /4). */
void wwdg_set_prescaler(uint32_t wdgtb)
{
    WWDG->CFG = (WWDG->CFG & ~WWDG_CFG_WDGTB_MASK) | wdgtb;
}

/* --- 0x080073d0 --- CFG.W = window value. */
void wwdg_set_window(uint32_t window)
{
    WWDG->CFG = (WWDG->CFG & ~WWDG_CFG_W_MASK) | (window & WWDG_CFG_W_MASK);
}

/* --- 0x08007406 --- CTRL = WDGA | counter. Writing WDGA is one-way: the
 * watchdog can only be stopped by a reset. */
void wwdg_enable_set_counter(uint32_t counter)
{
    WWDG->CTRL = counter | WWDG_CTRL_WDGA;
}

/* --- 0x08002dec (named reinit_can_filters_and_state in the decompile, but it
 * is wwdg_init) --- Clear a latched WWDG reset flag, clock the peripheral,
 * then arm it: prescaler /4, window fully open, counter 0x7F. */
void wwdg_init(void)
{
    if (rcc_get_flag_status(RCC_FLAG_WWDGRST))
        rcc_clear_reset_flags();

    rcc_apb1_periph_clock(RCC_APB1_WWDG, 1);
    wwdg_set_prescaler(0x100U);   /* WDGTB = 0b10 = /4 */
    wwdg_set_window(0x7FU);
    wwdg_enable_set_counter(0x7FU);
}
