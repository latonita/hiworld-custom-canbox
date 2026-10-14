/* ============================================================================
 * app/hal/hal_wwdg.c -- window watchdog (HAL layer).
 * Ported from the retired app/port/hw_wwdg.c, itself from reference/decompile/app/wwdg.c + sys.c (Ghidra).
 *
 * MMIO only -- this file touches no RAM state. See hal_wwdg.h for the timing
 * arithmetic and the register-layout provenance.
 * ==========================================================================*/

#include "n32l406_hw.h"
#include "hal_wwdg.h"
#include "hal_rcc.h"

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
