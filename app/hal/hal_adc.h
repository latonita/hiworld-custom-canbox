/* ============================================================================
 * hal_adc.h -- the three ADC control helpers used by the shutdown path.
 * See hal_adc.c: the ADC is never clocked by this firmware, so these writes are
 * inert on hardware; they exist for fidelity with the stock image.
 *
 * LAYER RULES: C11, MMIO only, no RAM state.
 *
 * These keep the stock driver's `uint32_t base` parameter rather than taking a
 * typed instance pointer: the ADC has no struct in n32l406_hw.h (the app never
 * uses the peripheral beyond these three writes), so there is nothing to point
 * at and the raw base is what the decompile shows being passed.
 * ==========================================================================*/

#ifndef HAL_ADC_H
#define HAL_ADC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_BASE_ADDR   0x40020800U   /* N32L406: AHB, NOT the F1 0x40012400 */

void adc_enable(uint32_t base, int enable);           /* 0x080034ee */
void adc_dma_cmd(uint32_t base, int enable);          /* 0x08003504 */
void adc_sw_start_regular(uint32_t base, int enable); /* 0x0800355a */

#ifdef __cplusplus
}
#endif

#endif /* HAL_ADC_H */
