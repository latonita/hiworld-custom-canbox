/* ============================================================================
 * hal_pwr.c -- PWR / SCB deep-sleep (STOP mode) entry.
 *
 * Extracted from the file-static pwr_enter_stop() in the retired app/port/core.c (around
 * line 308), which reproduces the stock vendor routine at 0x0800563c.
 *
 * LAYER RULES: C11, MMIO only, no RAM state.
 * ==========================================================================*/

#include "n32l406_hw.h"
#include "hal_pwr.h"

/* --- 0x0800563c pwr_enter_stop, the sleep primitive ---
 * PWR->CTRL3[13:12] = cfg, PWR->CTRL1[2:0] = LPMS(2), SCB->SCR |= SLEEPDEEP,
 * then WFI. The stock code clears SLEEPDEEP again after the wake; kept for
 * fidelity even though the app's call site never gets that far on a real wake.
 *
 * (In the retired app/port/core.c this was static on purpose -- the rcc port did not export
 * it and keeping it file local avoided colliding with it if it ever did. In the
 * HAL layer it is the one canonical definition, so the collision concern is
 * gone.)
 */
void pwr_enter_stop(int use_wfi, uint32_t cfg)
{
    PWR->CTRL3 = (PWR->CTRL3 & ~PWR_CTRL3_WUF_MASK) | cfg;
    PWR->CTRL1 = (PWR->CTRL1 & ~PWR_CTRL1_LPMS_MASK) | PWR_CTRL1_LPMS_STOP;
    SCB_SCR |= SCB_SCR_SLEEPDEEP;

    if (use_wfi == 1) {
        wait_for_irq();
    } else {
        /* The stock WFE branch really does issue two WFEs -- the first clears
         * a stale event-register bit so the second actually sleeps. */
        __asm volatile ("wfe");
        __asm volatile ("wfe");
    }

    SCB_SCR &= ~SCB_SCR_SLEEPDEEP;
}
