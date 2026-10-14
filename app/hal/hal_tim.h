/* ============================================================================
 * hal_tim.h -- vendor TIM register driver subset (HAL layer).
 *
 * Ported from the register half of the retired app/port/hw_tim.c, which was itself ported
 * from reference/decompile/app/tim.c (Ghidra output). Addresses in the comments are the
 * ORIGINAL flash addresses of the stock functions this reproduces.
 *
 * LAYER RULES: C11, MMIO only, no RAM state. The board-policy TIM2 time base
 * (tim2_init -- this board's 1 ms tick), TIM2_IRQHandler and the tick hooks it
 * drives are NOT here; they belong to the services layer, which builds them on
 * top of these primitives.
 *
 * Only the time-base path of the vendor driver is reconstructed: the stock
 * image uses the output-compare / PWM / break-dead-time / encoder / slave-mode
 * entry points nowhere, so they are deliberately not ported (reference/decompile/app/tim.c
 * stays the spec in case a later feature needs them).
 *
 * REGISTER LAYOUT -- this part is a Nations N32L406CBL7, NOT an STM32.
 * Field names below are the vendor SDK's (TIM_Module in n32l406_hw.h):
 *     CTRL1 +0x00   CTRL2 +0x04   SMCTRL +0x08   DINTEN +0x0C   STS +0x10
 *     EVTGEN +0x14  CCMOD1 +0x18  CCMOD2 +0x1C   CCEN +0x20     CNT +0x24
 *     PSC   +0x28   AR    +0x2C   REPCNT +0x30
 * These are exactly the byte offsets the decompiled driver pokes (DINTEN
 * +0x0C, STS +0x10, EVTGEN +0x14, PSC +0x28, AR +0x2C), so the stock driver
 * and this header agree.
 * ==========================================================================*/

#ifndef HAL_TIM_H
#define HAL_TIM_H

#include <stdint.h>
#include "n32l406_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- TIM register bits used by this subsystem --------------------------- */
#define TIM_CTRL1_CEN        0x0001U   /* counter enable                     */
#define TIM_CTRL1_DIR_CMS    0x0070U   /* DIR + CMS ("CounterMode") field    */
#define TIM_CTRL1_CKD        0x0300U   /* clock division ("ClkDiv") field    */
#define TIM_EVTGEN_UG        0x0001U   /* update generation                  */
#define TIM_INT_UPDATE       0x0001U   /* DINTEN.UIE / STS.UIF               */

/* Time-base init block. Layout copied byte-for-byte from the stock vendor
 * struct as the decompile of 0x08006140 indexes it, so the field offsets are
 * the SDK's TIM_TimeBaseInitType offsets, not a redesign. DO NOT REORDER --
 * the offsets are load-bearing.
 *
 * The trailing uint8_t fields are vendor extension flags whose meaning is NOT
 * identified: 0x08006140 copies them into CTRL1 bit 11, CTRL1 bit 15 and (for
 * TIM2/TIM4 only) CTRL2 bit 8. The stock tim2_init never initialises them (it
 * fills only the first four u16s and leaves the rest of the stack frame
 * undefined), so a caller should zero them, which leaves those bits at their
 * reset value. See the CAUTION note in hal_tim.c. */
typedef struct {
    uint16_t Prescaler;    /* +0x00 -> PSC                                   */
    uint16_t CntMode;      /* +0x02 -> CTRL1[6:4]  (pre-shifted by caller)    */
    uint16_t Period;       /* +0x04 -> AR                                    */
    uint16_t ClkDiv;       /* +0x06 -> CTRL1[9:8]  (pre-shifted by caller)    */
    uint8_t  RepetCnt;     /* +0x08 -> REPCNT, advanced timers (TIM1) only    */
    uint8_t  Ctrl1Bit11;   /* +0x09 -> CTRL1 bit 11, purpose unidentified     */
    uint8_t  Reserved[3];  /* +0x0A -> CTRL1 bits 12..14, TIM9 only (unused)  */
    uint8_t  Ctrl1Bit15;   /* +0x0D -> CTRL1 bit 15, purpose unidentified     */
    uint8_t  Ctrl2Bit8;    /* +0x0E -> CTRL2 bit 8,  purpose unidentified     */
} tim_timebase_init_t;

/* ---- vendor TIM driver subset (app/hal/hal_tim.c) ----------------------- */
void tim_timebase_init(TIM_Module *tim,
                       const tim_timebase_init_t *init);        /* 0x08006140 */
void tim_prescaler_config(TIM_Module *tim, uint16_t prescaler,
                          uint16_t reload_mode);                /* 0x08006a54 */
void tim_it_config(TIM_Module *tim, uint16_t it, int enable);   /* 0x0800693c */
void tim_cmd(TIM_Module *tim, int enable);                      /* 0x08006908 */
int  tim_get_it_status(TIM_Module *tim, uint16_t it);           /* 0x08006fb4 */
void tim_clear_it_pending(TIM_Module *tim, uint16_t it);        /* 0x08006fd6 */

#ifdef __cplusplus
}
#endif

#endif /* HAL_TIM_H */
