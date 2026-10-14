/* ============================================================================
 * scheduler.cpp -- see scheduler.hpp.
 *
 * Ported from reference/decompile/app/tim.c (0x080027F8, 0x08002F72, 0x08007A30) and
 * reference/decompile/app/sys.c (0x0800853C), Ghidra output, and
 * the retired app/port/core.c.
 * ==========================================================================*/

#include "scheduler.hpp"

#include <cstdint>

#include "app_config.hpp"
#include "can_bus.hpp"
#include "ram_map.hpp"
#include "soft_clock.hpp"
#include "soft_timer.hpp"

extern "C" {
#include "hal_nvic.h"
#include "hal_rcc.h"
#include "hal_tim.h"
}

namespace svc {
namespace sched {

/* --- 0x080027F8 tim2_init --- */
void start() noexcept
{
    /* PRIGROUP = 3. Part of tim2_init in the stock image, not a separate step. */
    nvic_set_priority_grouping(kNvicPriorityGroup);
    nvic_init(static_cast<std::uint8_t>(TIM2_IRQn), kTim2Preempt, kTim2Subpriority, 1);

    rcc_apb1_periph_clock(RCC_APB1_TIM2, 1);

    /* DELIBERATE DEVIATION, carried over from the retired app/port/hw_tim.c:
     * the stock tim2_init fills only Prescaler / CntMode / Period / ClkDiv and
     * leaves the rest of its stack frame undefined, so in the stock image
     * CTRL1 bits 11 and 15 and CTRL2 bit 8 take whatever happened to be on the
     * stack. Zero-initialising the block clears those three bits
     * deterministically, which is their reset value.
     *
     * Every member is written out by hand rather than value-initialised with
     * `{}`: at -Os the compiler turns the aggregate initialiser into a call to
     * memset, and this image links with -nostdlib. */
    tim_timebase_init_t tb;
    tb.Prescaler    = 0U;                  /* overwritten immediately below */
    tb.CntMode      = 0U;                  /* up-counting */
    tb.Period       = cfg::kTim2Period;    /* AR = 4000 */
    tb.ClkDiv       = 0U;                  /* CKD = /1 */
    tb.RepetCnt     = 0U;                  /* TIM1-only, unused on TIM2 */
    tb.Ctrl1Bit11   = 0U;
    tb.Reserved[0]  = 0U;
    tb.Reserved[1]  = 0U;
    tb.Reserved[2]  = 0U;
    tb.Ctrl1Bit15   = 0U;
    tb.Ctrl2Bit8    = 0U;
    tim_timebase_init(TIM2, &tb);

    /* The prescaler is programmed twice over: tim_timebase_init wrote PSC = 0,
     * and this writes PSC = 7 plus an EVTGEN update event so the new value is
     * latched at once rather than at the next natural update.
     * 32 MHz / 8 / 4000 = 1 kHz. That is what the stock code does; kept. */
    tim_prescaler_config(TIM2, cfg::kTim2Prescaler, TIM_EVTGEN_UG);

    tim_it_config(TIM2, TIM_INT_UPDATE, 1);
    tim_cmd(TIM2, 1);
}

/* --- 0x08002F72 TIM2_IRQHandler (body) ---
 * Three independent `if`s, not `else if` -- see the structure note in the
 * header. The dividers compare against 9 in the stock code, i.e. "has reached
 * the divisor", which is cfg::kDiv10ms / kDiv100ms / kDiv1s = 10 each. */
void onTimerInterrupt() noexcept
{
    if (tim_get_it_status(TIM2, TIM_INT_UPDATE) == 0) {
        return;
    }
    tim_clear_it_pending(TIM2, TIM_INT_UPDATE);

    onMillisecond();                                   /*   1 ms */

    ram::AppRam& r = ram::app();

    ++r.tickDiv10ms;
    if (r.tickDiv10ms >= cfg::kDiv10ms) {
        r.tickDiv10ms = 0U;
        timers::tickShortTimers();                     /*  10 ms */
        tickCycleCounter();
        ++r.tickDiv100ms;
    }
    if (r.tickDiv100ms >= cfg::kDiv100ms) {
        r.tickDiv100ms = 0U;
        clock::tick();                                 /* 100 ms */
        can::timersTick();
        ++r.tickDiv1s;
    }
    if (r.tickDiv1s >= cfg::kDiv1s) {
        r.tickDiv1s = 0U;                              /*   1 s  */
    }
}

/* --- 0x08007A30 tim2_sched_rate_flags ---
 * The counter is 32-bit on purpose: narrowing it would put the 0x3FF and 0x7FF
 * tests permanently out of reach. See the header. */
void onMillisecond() noexcept
{
    ram::Scheduler& s = ram::app().sched;

    s.counter = s.counter + 1U;
    const std::uint32_t c = s.counter;

    /* Staggered phases: 4/8/16/32 ms never coincide ... */
    if ((c & 0x003U) == 0x001U) { s.flagFast     = 1U; }  /*    4 ms */
    if ((c & 0x007U) == 0x003U) { s.flagMed      = 1U; }  /*    8 ms */
    if ((c & 0x00FU) == 0x007U) { s.flagDiv16    = 1U; }  /*   16 ms */
    if ((c & 0x01FU) == 0x00FU) { s.flagDiv32    = 1U; }  /*   32 ms */
    /* ... and from here down they all fire on an all-ones mask, so they DO. */
    if ((c & 0x03FU) == 0x03FU) { s.flagDiv64    = 1U; }  /*   64 ms */
    if ((c & 0x07FU) == 0x07FU) { s.flagDiv128   = 1U; }  /*  128 ms */
    if ((c & 0x0FFU) == 0x0FFU) { s.flagDiv256   = 1U; }  /*  256 ms */
    if ((c & 0x1FFU) == 0x1FFU) { s.flagDiv512   = 1U; }  /*  512 ms */
    if ((c & 0x3FFU) == 0x3FFU) { s.flagSlow     = 1U; }  /* 1024 ms */
    if ((c & 0x7FFU) == 0x7FFU) { s.flagAutobaud = 1U; }  /* 2048 ms */
}

/* --- 0x0800853C sys_cycle_counter_tick ---
 * Rolls over 1 .. (cycleCtrMax - 1); wraps back to 1, not 0. The comparison is
 * against the post-increment value, so cycleCtrMax itself is never observed by
 * a consumer. */
void tickCycleCounter() noexcept
{
    ram::AppRam& r = ram::app();

    ++r.cycleCtr;
    if (r.cycleCtr >= r.cycleCtrMax) {
        r.cycleCtr = 1U;
    }
}

/* ---------------------------------------------------------------------------
 * Flag consumers. The stock main_loop tests `!= 0` and stores 0, in that
 * order, for all three.
 * ------------------------------------------------------------------------- */
bool takeFast() noexcept
{
    if (ram::app().sched.flagFast == 0U) { return false; }
    ram::app().sched.flagFast = 0U;
    return true;
}

bool takeMed() noexcept
{
    if (ram::app().sched.flagMed == 0U) { return false; }
    ram::app().sched.flagMed = 0U;
    return true;
}

bool takeSlow() noexcept
{
    if (ram::app().sched.flagSlow == 0U) { return false; }
    ram::app().sched.flagSlow = 0U;
    return true;
}

/* READS the flag and leaves it alone -- see the header for why this must not
 * become a test-and-clear. The stock phase-1 test is `== 1`, not `!= 0`;
 * onMillisecond only ever stores 1, so the two agree, and the narrower form is
 * kept because it is what the reference image does. */
bool autobaudDue() noexcept
{
    return ram::app().sched.flagAutobaud == 1U;
}

void clearAutobaud() noexcept
{
    ram::app().sched.flagAutobaud = 0U;
}

}  // namespace sched
}  // namespace svc
