/* ============================================================================
 * hal_pwr.h -- PWR / SCB deep-sleep (STOP mode) entry.
 *
 * Extracted from the file-static pwr_enter_stop() in the retired app/port/core.c, which
 * reproduces the stock vendor routine at 0x0800563c. It was kept file-local
 * there only because no port unit exported it; as a pure register sequence it
 * belongs in the HAL.
 *
 * LAYER RULES: C11, MMIO only, no RAM state. The POLICY of when to sleep (the
 * phase-1 give-up path after 8 failed autobaud attempts, the CAN/GPIO/UART
 * teardown that must precede it, the WWDG fallback on wake) is NOT here -- it
 * stays with sys_shutdown_deep_sleep @0x080032f6 in the upper layers.
 * ==========================================================================*/

#ifndef HAL_PWR_H
#define HAL_PWR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- PWR / SCB bits used by the deep-sleep entry ------------------------ */
#define PWR_CTRL1_LPMS_MASK 0x00000007U
#define PWR_CTRL1_LPMS_STOP 0x00000002U
#define PWR_CTRL3_WUF_MASK  0x00003000U   /* bits 13:12, the stock field */
#define PWR_CTRL3_WUF_VAL   0x00001000U   /* the value the app passes as cfg */
#define SCB_SCR_SLEEPDEEP   0x00000004U

/* Enter STOP mode. `cfg` is the raw PWR->CTRL3[13:12] wake-source field value
 * (the app always passes PWR_CTRL3_WUF_VAL); `use_wfi` selects WFI (1) over the
 * stock WFE branch (anything else). */
void pwr_enter_stop(int use_wfi, uint32_t cfg);                 /* 0x0800563c */

#ifdef __cplusplus
}
#endif

#endif /* HAL_PWR_H */
