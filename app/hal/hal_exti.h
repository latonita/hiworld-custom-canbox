/* ============================================================================
 * app/hal/hal_exti.h -- EXTI + AFIO line-source register driver (HAL layer).
 *
 * LAYER CONTRACT: pure C11, MMIO only. No RAM state, no globals.h, no C++.
 * Every entry point carries the flash address of the stock function it
 * reproduces so the rebuild can be diffed against
 * reference/2429-H1N4JPF20A-220524.app.bin.
 *
 * Ported from reference/decompile/app/gpio.c / reference/decompile/app/misc.c (Ghidra). On the N32L406 the
 * EXTI block is the familiar { IMASK, EMASK, RT_CFG, FT_CFG, SWIE, PEND,
 * TS_SEL } file, but the line->port selector is NOT an F1-style AFIO_EXTICR
 * named as such: AFIO on this part is only { RMP_CFG, EXTI_CFG[4] }, and
 * EXTI_CFG[] is used solely as that selector. See hal_exti.c.
 *
 * NOT IN THIS UNIT (board/product policy, not registers): arming PB8 (CAN_RX)
 * as a falling-edge wake source on the sleep / firmware-update path, and the
 * EXTI2 / EXTI9_5 interrupt handlers -- those reset the box through the
 * watchdog and belong to the logic layer.
 * ==========================================================================*/

#ifndef HAL_EXTI_H
#define HAL_EXTI_H

#include <stdint.h>
#include "n32l406_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- EXTI line masks ----------------------------------------------------
 * One bit per line; line n == GPIO pin n of whichever port EXTI_CFG selects.
 * The two named lines record what the stock firmware actually arms. */
#define EXTI_LINE(n)            (1UL << (n))
#define EXTI_LINE2              0x00000004U   /* PB2                          */
#define EXTI_LINE8              0x00000100U   /* PB8                          */

/* ---- AFIO EXTI port selectors -------------------------------------------
 * Value written into the 4-bit-spaced slot of AFIO->EXTI_CFG[] that picks
 * which port drives a given line. Only 2 bits are significant (ports A..D). */
#define AFIO_EXTI_PORT_A        0U
#define AFIO_EXTI_PORT_B        1U
#define AFIO_EXTI_PORT_C        2U
#define AFIO_EXTI_PORT_D        3U

/* ---- exti_line_init() selectors -----------------------------------------
 * Offsets into EXTI_Module, used the way the stock HAL uses them: the caller
 * passes a REGISTER OFFSET, not an enum. Interrupt vs event is IMASK/EMASK;
 * trigger is RT_CFG/FT_CFG, with 0x10 meaning "both" (0x10 is EXTI.SWIE's
 * offset and is never written as a register here -- the vendor SDK simply
 * reuses the value as a sentinel). These are part of the public API because
 * they are literally the arguments exti_line_init() takes. */
#define EXTI_MODE_INTERRUPT     0x00U   /* EXTI->IMASK  */
#define EXTI_MODE_EVENT         0x04U   /* EXTI->EMASK  */
#define EXTI_TRIG_RISING        0x08U   /* EXTI->RT_CFG */
#define EXTI_TRIG_FALLING       0x0CU   /* EXTI->FT_CFG */
#define EXTI_TRIG_BOTH          0x10U   /* sentinel: set RT_CFG and FT_CFG    */

/* ---- API ----------------------------------------------------------------
 *
 * exti_set_line_source -- point one EXTI line at one GPIO port.
 *   port_sel  AFIO_EXTI_PORT_A..D: which port drives the line.
 *   line_num  EXTI line NUMBER 0..15 (i.e. the pin number), not a mask.
 *
 * exti_line_init -- mask / trigger setup for one or more lines.
 *   line_mask EXTI_LINE(n) / EXTI_LINE* bits; several may be OR-ed together.
 *   mode_ofs  EXTI_MODE_INTERRUPT or EXTI_MODE_EVENT -- a byte offset into
 *             EXTI_Module naming the mask register to drive.
 *   trig_ofs  EXTI_TRIG_RISING / _FALLING / _BOTH -- likewise a byte offset,
 *             with _BOTH acting as the vendor sentinel for "set both".
 *             Ignored when enable == 0.
 *   enable    non-zero arms the line(s): both mask registers are cleared
 *             first, then mode_ofs and the trigger are set. Zero only clears
 *             the line(s) in the mode_ofs register and returns.
 *
 * exti_get_it_status  -- 1 if any line in line_mask is pending AND unmasked.
 * exti_clear_it_pending -- write-1-to-clear the pending bits in line_mask.
 */
void exti_set_line_source(uint32_t port_sel, uint32_t line_num);          /* 0x080054a0 */
void exti_line_init(uint32_t line_mask, uint32_t mode_ofs,
                    uint32_t trig_ofs, int enable);                       /* 0x080044ea */
int  exti_get_it_status(uint32_t line_mask);                              /* 0x080045b6 */
void exti_clear_it_pending(uint32_t line_mask);                           /* 0x080045d8 */

#ifdef __cplusplus
}
#endif

#endif /* HAL_EXTI_H */
