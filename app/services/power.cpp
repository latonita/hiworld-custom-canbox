/* ============================================================================
 * power.cpp -- see power.hpp.
 * Ported from reference/decompile/app/sys.c, reference/decompile/app/misc.c and reference/decompile/app/can.c (Ghidra output).
 * ==========================================================================*/

#include "power.hpp"

#include "board_io.hpp"
#include "can_bus.hpp"
#include "head_unit.hpp"
#include "ram_map.hpp"

extern "C" {
#include "hal_adc.h"
#include "hal_pwr.h"
#include "hal_rcc.h"
#include "hal_wwdg.h"
}

/* --- 0x08002E64 fatal_error_reset ---
 * Record the code where a debugger or the next boot can find it, re-arm the
 * watchdog (in case the fault happened before the main bring-up ran), then
 * force a reset. Never returns: the reset drops back into the IAP bootloader,
 * which is why a wedged app can always still be reflashed.
 *
 * The outer loop is the stock shape and is what makes the [[noreturn]] contract
 * true even if a future wwdg_force_reset() somehow came back. */
extern "C" void fatal_error_reset(std::uint8_t code)
{
    for (;;) {
        ram::app().fatalErrorCode = code;
        wwdg_init();
        wwdg_force_reset();
    }
}

namespace svc {
namespace power {

void disableWatchdogClock() noexcept
{
    rcc_apb1_periph_clock(RCC_APB1_WWDG, 0);
}

void shutdownDeepSleep() noexcept
{
    /* If the controller will not go to sleep, do nothing at all. The caller
     * keeps spinning and the (still clocked) watchdog resolves it. */
    if (!svc::can::enterSleep()) {
        return;
    }

    svc::io::setCanTransceiverStandby(true);  /* PB12 high: transceiver asleep */
    svc::io::statusLinesShutdown();           /* outputs low, PB3/PB2 to input */

    /* The ADC is never clocked by this firmware, so these three writes are
     * inert on hardware. Reproduced because the stock image has them. */
    adc_dma_cmd(ADC_BASE_ADDR, 0);
    adc_enable(ADC_BASE_ADDR, 0);
    adc_sw_start_regular(ADC_BASE_ADDR, 0);

    svc::hu::disable();                       /* USART2 off */
    disableWatchdogClock();

    /* *** The only wake source armed before the WFI below. *** Without it the
     * box sleeps and never comes back short of a power cycle. */
    svc::io::configurePb8Wake();

    pwr_enter_stop(1, PWR_CTRL3_WUF_VAL);

    /* On wake. The watchdog is unclocked by now, so this write lands on a dead
     * peripheral and the reset actually comes from the EXTI9_5 handler
     * (-> fatal_error_reset(0x85) -> wwdg_init() -> wwdg_force_reset()).
     * Reproduced because the stock image has it, and because it is the correct
     * fallback if the clock ever is still running. */
    wwdg_force_reset();
}

}  // namespace power
}  // namespace svc
