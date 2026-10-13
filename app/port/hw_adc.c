/* ============================================================================
 * hw_adc.c -- the three ADC control helpers the shutdown path calls.
 * Ported from app/src/adc.c (Ghidra output).
 *
 * The app NEVER enables the ADC clock (no APB/AHB enable for it exists anywhere
 * in the image -- see docs/hiworld-startup-sequence.md §3), and these three are
 * reached only from sys_shutdown_deep_sleep @0x080032f6, which calls each with
 * enable = 0 just before entering STOP. So on real hardware these writes land
 * on an unclocked peripheral and do nothing.
 *
 * They are ported anyway rather than left as no-op stubs for two reasons:
 * fidelity to the stock image, and because a weak no-op is indistinguishable
 * from "forgot to implement it" in tools/check_stubs.py. Reproducing them costs
 * nine lines and removes the ambiguity.
 *
 * All three are read-modify-writes on ADC + 0x08, which is ADC_Module.CTRL2 in
 * the vendor SDK layout (STS +0x00, CTRL1 +0x04, CTRL2 +0x08). ADC base is
 * 0x40020800 on this part -- NOT the 0x40012400 an STM32F1 would use.
 * ==========================================================================*/

#include "port.h"
#include "hw_adc.h"

#define ADC_CTRL2(base)     (*(volatile uint32_t *)((base) + 0x08U))

/* --- 0x080034ee --- CTRL2.ADCON (bit 0) */
void adc_enable(uint32_t base, int enable)
{
    if (enable)
        ADC_CTRL2(base) |= 0x00000001U;
    else
        ADC_CTRL2(base) &= 0xFFFFFFFEU;
}

/* --- 0x08003504 --- CTRL2.DMA (bit 8) */
void adc_dma_cmd(uint32_t base, int enable)
{
    if (enable)
        ADC_CTRL2(base) |= 0x00000100U;
    else
        ADC_CTRL2(base) &= 0xFFFFFEFFU;
}

/* --- 0x0800355a --- CTRL2 bits 22 and 20 together (SWSTART | EXTTRIG).
 * Note the stock clear mask is 0xFFAFFFFF, which clears bit 20 but NOT bit 22 --
 * asymmetric with the 0x500000 set. Reproduced verbatim. */
void adc_sw_start_regular(uint32_t base, int enable)
{
    if (enable)
        ADC_CTRL2(base) |= 0x00500000U;
    else
        ADC_CTRL2(base) &= 0xFFAFFFFFU;
}
