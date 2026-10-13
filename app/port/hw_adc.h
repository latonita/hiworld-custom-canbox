/* ============================================================================
 * hw_adc.h -- the three ADC control helpers used by the shutdown path.
 * See hw_adc.c: the ADC is never clocked by this firmware, so these writes are
 * inert on hardware; they exist for fidelity with the stock image.
 * ==========================================================================*/

#ifndef HW_ADC_H
#define HW_ADC_H

#include <stdint.h>

#define ADC_BASE_ADDR   0x40020800U   /* N32L406: AHB, NOT the F1 0x40012400 */

void adc_enable(uint32_t base, int enable);           /* 0x080034ee */
void adc_dma_cmd(uint32_t base, int enable);          /* 0x08003504 */
void adc_sw_start_regular(uint32_t base, int enable); /* 0x0800355a */

#endif /* HW_ADC_H */
