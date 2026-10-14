/* ============================================================================
 * app/hal/hal_wwdg.h -- window watchdog register driver (HAL layer).
 *
 * LAYER CONTRACT: pure C11, MMIO only. No RAM state, no globals.h, no C++.
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

#ifndef HAL_WWDG_H
#define HAL_WWDG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WWDG_CTRL_WDGA      0x80U
#define WWDG_CFG_WDGTB_MASK 0x180U
#define WWDG_CFG_W_MASK     0x7FU

void wwdg_kick(void);                                      /* 0x08002d3e */
void wwdg_set_prescaler(uint32_t wdgtb);                   /* 0x080073be */
void wwdg_set_window(uint32_t window);                     /* 0x080073d0 */
void wwdg_enable_set_counter(uint32_t counter);            /* 0x08007406 */
void wwdg_init(void);                                      /* 0x08002dec */
/* On target this never returns: it opens the window and spins until the window
 * watchdog resets the part.
 *
 * The attribute is dropped in the host test build, and that is NOT cosmetic.
 * The fake returns from this call (see tests/host/fake_hal.cpp for why nothing
 * else works). A caller compiled against the noreturn declaration emits an
 * `unreachable` straight after the call, so a fake that returns drops the
 * process into undefined behaviour -- which showed up as a test runner that
 * printed nothing at all and exited 0, because the crash took the unflushed
 * stdout buffer with it. */
#if defined(CANBOX_HOST_TEST)
void wwdg_force_reset(void);
#else
__attribute__((noreturn)) void wwdg_force_reset(void);
#endif     /* 0x08002e26/2e2e */

#ifdef __cplusplus
}
#endif

#endif /* HAL_WWDG_H */
