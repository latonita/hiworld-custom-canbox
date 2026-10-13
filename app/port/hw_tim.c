/* ============================================================================
 * hw_tim.c -- TIM2 time base + the 1 ms scheduler tick.
 *
 * Ported from app/src/tim.c (Ghidra output, names applied). Every function
 * carries the original flash address of the stock code it reproduces.
 *
 * ---------------------------------------------------------------------------
 * THE TICK MATHS (load-bearing -- do not "tidy" these numbers)
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
 * Confirmed independently of the clock-tree reconstruction: this ISR drives a
 * soft real-time clock (sys_soft_clock_tick, 100 ms cadence) whose seconds
 * only advance at the right speed if the interrupt fires at exactly 1 kHz.
 *
 * The prescaler is programmed twice over: tim_timebase_init writes
 * Prescaler = 0 into PSC, then tim2_init immediately calls
 * tim_prescaler_config to write PSC = 7 plus an EVTGEN update event so the
 * new value is latched at once rather than at the next update. That is what
 * the stock code does; it is kept verbatim.
 *
 * ---------------------------------------------------------------------------
 * REGISTER LAYOUT -- this part is a Nations N32L406CBL7, NOT an STM32.
 * Field names below are the vendor SDK's (TIM_Module in n32l406_hw.h):
 *     CTRL1 +0x00   CTRL2 +0x04   SMCTRL +0x08   DINTEN +0x0C   STS +0x10
 *     EVTGEN +0x14  CCMOD1 +0x18  CCMOD2 +0x1C   CCEN +0x20     CNT +0x24
 *     PSC   +0x28   AR    +0x2C   REPCNT +0x30
 * These are exactly the byte offsets the decompiled driver pokes (DINTEN
 * +0x0C, STS +0x10, EVTGEN +0x14, PSC +0x28, AR +0x2C), so the stock driver
 * and this header agree.
 * ==========================================================================*/

#include "port.h"
#include "hw_tim.h"

#define TIM_CTRL1_BIT11      0x0800U   /* vendor extension, purpose unknown  */
#define TIM_CTRL1_BIT15      0x8000U   /* vendor extension, purpose unknown  */
#define TIM_CTRL2_BIT8       0x0100U   /* vendor extension, TIM2/TIM4 only   */

#define TIM2_PRESCALER       7U        /* 32 MHz / (7+1) = 4 MHz counter     */
#define TIM2_PERIOD          4000U     /* 4 MHz / 4000   = 1 kHz = 1 ms      */

/* ============================================================================
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
 * ported; app/src/tim.c remains the spec if a later feature needs it.
 * ==========================================================================*/

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
 * whatever happened to be on the stack. This port passes a zero-initialised
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

/* ============================================================================
 * The app's TIM2 timebase
 * ==========================================================================*/

/* --- 0x080027f8 tim2_init ---
 * The NVIC priority-grouping write is part of the stock tim2_init, not a
 * separate step: 0x300 selects PRIGROUP = 3. TIM2 (IRQ 28) then gets
 * preempt 4, sub 0, enabled.
 *
 * Note the ordering: the interrupt is unmasked in the NVIC BEFORE the timer
 * is started, exactly as the stock code does. That is safe because main_loop
 * runs this whole init sequence with PRIMASK set (CPSID i) and only enables
 * interrupts once every subsystem is up. */
void tim2_init(void)
{
    tim_timebase_init_t tb;

    nvic_set_priority_grouping(0x300U);
    nvic_init((uint8_t)TIM2_IRQn, 4U, 0U, 1);      /* IRQ 28, preempt 4 */

    rcc_apb1_periph_clock(RCC_APB1_TIM2, 1);

    tb.Prescaler   = 0U;                           /* overwritten below */
    tb.CntMode     = 0U;                           /* up-counting */
    tb.Period      = TIM2_PERIOD;
    tb.ClkDiv      = 0U;                           /* CKD = /1 */
    tb.RepetCnt    = 0U;                           /* TIM1-only, unused */
    tb.Ctrl1Bit11  = 0U;                           /* see the CAUTION above */
    tb.Reserved[0] = 0U;
    tb.Reserved[1] = 0U;
    tb.Reserved[2] = 0U;
    tb.Ctrl1Bit15  = 0U;
    tb.Ctrl2Bit8   = 0U;
    tim_timebase_init(TIM2, &tb);

    /* PSC = 7 with an immediate reload: 32 MHz / 8 / 4000 = 1 kHz. */
    tim_prescaler_config(TIM2, TIM2_PRESCALER, TIM_EVTGEN_UG);

    tim_it_config(TIM2, TIM_INT_UPDATE, 1);
    tim_cmd(TIM2, 1);
}

/* --- 0x08002f72 TIM2_IRQHandler ---
 * The 1 ms tick and the whole soft scheduler cadence:
 *
 *   every   1 ms : tim2_sched_rate_flags  (tick counter + the cadence flags)
 *   every  10 ms : sys_misc_timers_tick, sys_cycle_counter_tick
 *   every 100 ms : sys_soft_clock_tick, can_autobaud_timers_tick
 *   every   1 s  : nothing but the divider wrap (g_tick_1s_div is readable by
 *                  other subsystems as a phase marker)
 *
 * The three dividers live at fixed RAM addresses 0x20001000..0x20001002 and
 * are shared firmware state -- the globals.h macros, never local statics.
 *
 * Structure note: the stock code uses three independent `if`s, NOT `else if`,
 * so the 100 ms and 1 s branches are evaluated in the same interrupt that
 * incremented their counter. That is why the cascade settles in a single pass
 * and the 10 ms work and the 100 ms work always land on the same tick. Kept
 * bit-for-bit compatible.
 *
 * (The decompile ends in an "UNRECOVERED_JUMPTABLE" indirect call; that is a
 * Ghidra artifact of the tail epilogue, not a real call.)
 *
 * The name must match the weak symbol in app/startup/crt0.S exactly (veneer
 * slot 37, IRQ 28) -- and must NOT itself be weak. */
void TIM2_IRQHandler(void)
{
    if (tim_get_it_status(TIM2, TIM_INT_UPDATE)) {
        tim_clear_it_pending(TIM2, TIM_INT_UPDATE);

        tim2_sched_rate_flags();                   /* 1 ms */

        g_tick_10ms_div++;
        if (g_tick_10ms_div > 9U) {
            g_tick_10ms_div = 0U;
            sys_misc_timers_tick();                /* 10 ms */
            sys_cycle_counter_tick();
            g_tick_100ms_div++;
        }
        if (g_tick_100ms_div > 9U) {
            g_tick_100ms_div = 0U;
            sys_soft_clock_tick();                 /* 100 ms */
            can_autobaud_timers_tick();
            g_tick_1s_div++;
        }
        if (g_tick_1s_div > 9U) {
            g_tick_1s_div = 0U;                    /* 1 s */
        }
    }
}

/* ============================================================================
 * TEMPORARY SCAFFOLDING -- DELETE WHEN THE OWNING SUBSYSTEMS LAND.
 *
 * These five hooks belong to other subsystems (hw_tim.h names the stock
 * address and the owner of each). They are weak no-ops purely so the image
 * links today; the real, strong definitions override them silently as they
 * arrive, with no change needed here.
 *
 * An image still running on these stubs has a live 1 ms interrupt that does
 * NOTHING -- no tick counter, no cadence flags, no soft clock, no CAN
 * autobaud timeout. It is not a functional replacement for the stock
 * firmware.
 * ==========================================================================*/

__attribute__((weak)) void tim2_sched_rate_flags(void)    { }
__attribute__((weak)) void sys_misc_timers_tick(void)     { }
__attribute__((weak)) void sys_cycle_counter_tick(void)   { }
__attribute__((weak)) void sys_soft_clock_tick(void)      { }
__attribute__((weak)) void can_autobaud_timers_tick(void) { }
