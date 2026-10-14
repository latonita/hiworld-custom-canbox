/* ============================================================================
 * app/hal/hal_nvic.c -- NVIC / priority-grouping helpers (HAL layer).
 * Ported from the retired app/port/hw_nvic.c; stock NVIC_Init @0x080033ae and the AIRCR
 * setter @0x080033a4.
 *
 * MMIO only -- this file touches no RAM state.
 *
 * IMPORTANT: IRQ numbers here are N32L40x values, NOT STM32F1's. The one that
 * bites is CAN_RX0_IRQn = 54. The stock can_init passes 0x36 = 54 and that is
 * correct -- see docs/notes/vector-table-slot-map.md.
 *
 * The stock code computes the ISER address as 0xE000E000 + (IRQn>>5)*4 with a
 * +0x100 store displacement rather than loading an 0xE000E100 literal, which is
 * why no such literal appears in the image.
 * ==========================================================================*/

#include "n32l406_hw.h"
#include "hal_nvic.h"

/* --- 0x080033a4 --- SCB->AIRCR = VECTKEY | group. Stock calls this with 0x300
 * (PRIGROUP = 3 -> 4 bits of pre-emption, 0 of sub-priority). */
void nvic_set_priority_grouping(uint32_t group)
{
    SCB_AIRCR = 0x05FA0000U | group;
}

/* --- 0x080033ae NVIC_Init ---
 * Splits `preempt`/`sub` according to the PRIGROUP currently in AIRCR, writes
 * IPR[IRQn], then enables (ISER) or disables (ICER) the line.
 * Priority is left-aligned into the top 4 implemented bits (<< 4). */
void nvic_init(uint8_t irqn, uint8_t preempt, uint8_t sub, int enable)
{
    if (enable) {
        uint32_t prigroup = (SCB_AIRCR & 0x700U);
        uint32_t shift    = (0x700U - prigroup) >> 8;   /* 0..7 */
        uint32_t submask  = 0x0FU >> shift;
        uint32_t prio     = ((uint32_t)preempt << (4U - shift)) | (sub & submask);

        NVIC_IPR[irqn] = (uint8_t)(prio << 4);
        NVIC_ISER[irqn >> 5] = 1U << (irqn & 0x1FU);
    } else {
        NVIC_ICER[irqn >> 5] = 1U << (irqn & 0x1FU);
    }
}
