/* ============================================================================
 * app/hal/hal_nvic.h -- NVIC / priority-grouping register driver (HAL layer).
 *
 * LAYER CONTRACT: pure C11, MMIO only. No RAM state, no globals.h, no C++.
 *
 * IMPORTANT: IRQ numbers used with nvic_init() are N32L40x values, NOT STM32F1's.
 * The one that bites is CAN_RX0_IRQn = 54. The stock can_init passes 0x36 = 54
 * and that is correct -- see docs/notes/vector-table-slot-map.md. The IRQn_Type
 * enum lives in app/include/n32l406_hw.h.
 * ==========================================================================*/

#ifndef HAL_NVIC_H
#define HAL_NVIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void nvic_set_priority_grouping(uint32_t group);           /* 0x080033a4 */
void nvic_init(uint8_t irqn, uint8_t preempt, uint8_t sub, int enable); /* 0x080033ae */

#ifdef __cplusplus
}
#endif

#endif /* HAL_NVIC_H */
