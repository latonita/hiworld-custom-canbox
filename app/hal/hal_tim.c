/* ============================================================================
 * hal_tim.c -- vendor TIM register driver subset (HAL layer).
 *
 * Ported from the register half of the retired app/port/hw_tim.c (itself ported from
 * reference/decompile/app/tim.c, Ghidra output with names applied). Every function carries the
 * original flash address of the stock code it reproduces.
 *
 * LAYER RULES: C11, MMIO only, no RAM state. Nothing here knows this board's
 * tick rate -- the 1 ms TIM2 time base (tim2_init), TIM2_IRQHandler and the
 * scheduler hooks are board/service policy and live in the services layer.
 *
 * ---------------------------------------------------------------------------
 * CONTEXT -- what the services layer builds on top of this (kept here because
 * it is the reverse-engineering record for why these primitives exist at all;
 * the numbers themselves are NOT this file's policy):
 *
 *     SYSCLK = 64 MHz, APB1 prescaler = /4  ->  PCLK1 = 16 MHz
 *     TIM2 kernel clock = 2 x PCLK1 = 32 MHz
 *         (the x2 multiplier applies whenever PPRE1 != /1; with PPRE1 = /1 the
 *          timer clock would be PCLK1 and the tick would come out at 500 Hz)
 *
 *     PSC    = 7      -> divide by 8    -> counter clock 4 MHz
 *     Period = 4000   -> AR = 4000      -> update every 4000 counts
 *
 *     update rate = 32 MHz / ((7 + 1) * 4000) = 1000 Hz  ->  1 ms tick
 *
 * The prescaler is programmed twice over in the stock init: tim_timebase_init
 * writes Prescaler = 0 into PSC, then the caller immediately calls
 * tim_prescaler_config to write PSC = 7 plus an EVTGEN update event so the new
 * value is latched at once rather than at the next update. That double write
 * is the stock sequence; the caller in the services layer keeps it verbatim.
 *
 * ---------------------------------------------------------------------------
 * Vendor TIM driver subset
 *
 * The stock driver is written against the whole N32L40x timer family and
 * dispatches on the instance pointer (TIM1, TIM2..TIM5, TIM6/TIM7, TIM8,
 * TIM9). Only TIM1 and TIM2 exist on this package / in n32l406_hw.h, and the
 * app only ever drives TIM2, so the per-instance branches that can never be
 * taken here are reduced to the TIM1/TIM2 cases and noted where they were.
 *
 * The rest of the stock TIM driver (output compare, PWM, break/dead-time,
 * slave mode, encoder interface) has no live caller in the app and is not
 * ported; reference/decompile/app/tim.c remains the spec if a later feature needs it.
 * ==========================================================================*/

#include "hal_tim.h"

#define TIM_CTRL1_BIT11      0x0800U   /* vendor extension, purpose unknown  */
#define TIM_CTRL1_BIT15      0x8000U   /* vendor extension, purpose unknown  */
#define TIM_CTRL2_BIT8       0x0100U   /* vendor extension, TIM2/TIM4 only   */

/* --- 0x08006140 tim_timebase_init ---
 * Stock order of operations, preserved exactly: CTRL1 read-modify-write for
 * CntMode/ClkDiv, then AR, PSC, REPCNT, then EVTGEN = UG to load the shadow
 * registers, then a SECOND CTRL1 read-modify-write for the extension bits,
 * then CTRL2.
 *
 * CAUTION / deliberate deviation: the stock tim2_init passes an init block
 * whose bytes +0x09, +0x0D and +0x0E are never written -- it fills only
 * Prescaler/CntMode/Period/ClkDiv and leaves the rest of its stack frame
 * undefined, so in the stock image CTRL1 bits 11 and 15 and CTRL2 bit 8 take
 * whatever happened to be on the stack. The port passes a zero-initialised
 * block, which clears those three bits deterministically (their reset value).
 */
void tim_timebase_init(TIM_Module *tim, const tim_timebase_init_t *init)
{
    uint32_t ctrl1;
    uint32_t ctrl2;

    ctrl1 = tim->CTRL1;
    /* CntMode: every timer except the basic TIM6/TIM7, which this part lacks. */
    ctrl1 = (ctrl1 & ~(uint32_t)TIM_CTRL1_DIR_CMS) | init->CntMode;
    /* ClkDiv: likewise. */
    ctrl1 = (ctrl1 & ~(uint32_t)TIM_CTRL1_CKD) | init->ClkDiv;
    tim->CTRL1 = ctrl1;

    tim->AR  = init->Period;
    tim->PSC = init->Prescaler;

    if (tim == TIM1)                       /* advanced-control timers only */
        tim->REPCNT = init->RepetCnt;

    /* Update event: load the shadow PSC/AR now. */
    tim->EVTGEN = TIM_EVTGEN_UG;

    ctrl1 = tim->CTRL1;
    if (init->Ctrl1Bit11)
        ctrl1 |= TIM_CTRL1_BIT11;
    else
        ctrl1 &= ~(uint32_t)TIM_CTRL1_BIT11;
    /* CTRL1 bits 12..14 are TIM9-only in the stock driver; no TIM9 here. */
    if (init->Ctrl1Bit15)
        ctrl1 |= TIM_CTRL1_BIT15;
    else
        ctrl1 &= ~(uint32_t)TIM_CTRL1_BIT15;
    tim->CTRL1 = ctrl1;

    if (tim == TIM2) {                     /* stock: TIM2 and TIM4 only */
        ctrl2 = tim->CTRL2;
        if (init->Ctrl2Bit8)
            ctrl2 |= TIM_CTRL2_BIT8;
        else
            ctrl2 &= ~(uint32_t)TIM_CTRL2_BIT8;
        tim->CTRL2 = ctrl2;
    }
}

/* --- 0x08006a54 tim_prescaler_config ---
 * PSC = prescaler, then EVTGEN = reload_mode. reload_mode is the raw EVTGEN
 * value the caller wants: 1 (UG) latches the new prescaler immediately, 0
 * defers it to the next natural update event. */
void tim_prescaler_config(TIM_Module *tim, uint16_t prescaler,
                          uint16_t reload_mode)
{
    tim->PSC    = prescaler;
    tim->EVTGEN = reload_mode;
}

/* --- 0x0800693c tim_it_config --- DINTEN set/clear. */
void tim_it_config(TIM_Module *tim, uint16_t it, int enable)
{
    if (enable)
        tim->DINTEN |= it;
    else
        tim->DINTEN &= (uint16_t)~it;
}

/* --- 0x08006908 tim_cmd --- CTRL1.CEN set/clear. */
void tim_cmd(TIM_Module *tim, int enable)
{
    if (enable)
        tim->CTRL1 |= TIM_CTRL1_CEN;
    else
        tim->CTRL1 &= ~(uint32_t)TIM_CTRL1_CEN;
}

/* --- 0x08006fb4 tim_get_it_status --- true only when the status flag AND the
 * matching enable bit are both set. */
int tim_get_it_status(TIM_Module *tim, uint16_t it)
{
    return ((tim->STS & it) != 0U) && ((tim->DINTEN & it) != 0U);
}

/* --- 0x08006fd6 tim_clear_it_pending --- STS is write-0-to-clear; the stock
 * code does a read-modify-write of the whole register. */
void tim_clear_it_pending(TIM_Module *tim, uint16_t it)
{
    tim->STS &= ~(uint32_t)it;
}
