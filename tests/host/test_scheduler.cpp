/* ============================================================================
 * test_scheduler.cpp -- svc::sched, the 1 ms time base and the cadence flags.
 *
 * Written from the BEHAVIOURAL SPEC, not from scheduler.cpp:
 *   reference/decompile/app/tim.c   0x080027F8 tim2_init
 *                   0x08002F72 tim2_isr_scheduler_tick
 *                   0x08007A30 tim2_sched_rate_flags
 *   reference/decompile/app/sys.c   0x0800853C sys_cycle_counter_tick
 *                   0x080088D8 main_loop (how the flags are consumed)
 *
 * The cadence table below is restated as (period, phase) -- "fires on every
 * millisecond c where c % period == phase" -- rather than as the bit masks the
 * implementation uses, so a transcription slip in either form shows up as a
 * disagreement instead of cancelling out.
 *
 * Without a car on a bench this file is the only equivalence evidence for the
 * tick there is, so the cases below drive the counter past 4096 ms: the two
 * slowest flags (1024 ms and 2048 ms) are the ones a narrowed counter would
 * silently make unreachable, and an unreachable 2048 ms flag hangs phase 1
 * forever.
 * ==========================================================================*/

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "fake_hal.hpp"
#include "ram_map.hpp"
#include "scheduler.hpp"

extern "C" {
#include "hal_nvic.h"
#include "hal_rcc.h"
#include "hal_tim.h"
}

namespace {

constexpr std::size_t kFlagCount = 10U;

/* Restated from the retired app/port/core.h's flag table, which was derived from the
 * decompile at 0x08007A30. Index order matches the RAM layout at 0x2000110C. */
constexpr std::uint32_t kPeriod[kFlagCount] = {
    4U, 8U, 16U, 32U, 64U, 128U, 256U, 512U, 1024U, 2048U
};
constexpr std::uint32_t kPhase[kFlagCount] = {
    1U, 3U, 7U, 15U, 63U, 127U, 255U, 511U, 1023U, 2047U
};
constexpr const char* kName[kFlagCount] = {
    "fast/4ms", "med/8ms", "div16", "div32", "div64",
    "div128", "div256", "div512", "slow/1024ms", "autobaud/2048ms"
};

std::uint8_t flagAt(std::size_t i) noexcept
{
    const ram::Scheduler& s = ram::app().sched;
    switch (i) {
        case 0U: return s.flagFast;
        case 1U: return s.flagMed;
        case 2U: return s.flagDiv16;
        case 3U: return s.flagDiv32;
        case 4U: return s.flagDiv64;
        case 5U: return s.flagDiv128;
        case 6U: return s.flagDiv256;
        case 7U: return s.flagDiv512;
        case 8U: return s.flagSlow;
        default: return s.flagAutobaud;
    }
}

void clearFlags() noexcept
{
    ram::Scheduler& s = ram::app().sched;
    s.flagFast = 0U;  s.flagMed = 0U;      s.flagDiv16 = 0U;  s.flagDiv32 = 0U;
    s.flagDiv64 = 0U; s.flagDiv128 = 0U;   s.flagDiv256 = 0U; s.flagDiv512 = 0U;
    s.flagSlow = 0U;  s.flagAutobaud = 0U;
}

/* How many milliseconds every case below sweeps. Two full 2048 ms windows, so
 * the slowest flag is observed firing more than once rather than by accident. */
constexpr std::uint32_t kSweepMs = 4096U;

}  // namespace

/* ---------------------------------------------------------------------------
 * The cadence flags
 * ------------------------------------------------------------------------- */

TEST(sched_every_cadence_flag_fires_on_exactly_the_right_millisecond)
{
    fake::reset();

    std::uint32_t mismatches[kFlagCount] = {};
    std::uint32_t firstBad[kFlagCount]   = {};

    for (std::uint32_t ms = 1U; ms <= kSweepMs; ++ms) {
        clearFlags();
        svc::sched::onMillisecond();

        CHECK_EQ(ram::app().sched.counter, ms);

        for (std::size_t f = 0U; f < kFlagCount; ++f) {
            const bool want = (ms % kPeriod[f]) == kPhase[f];
            const bool got  = flagAt(f) != 0U;
            if (want != got) {
                if (mismatches[f] == 0U) { firstBad[f] = ms; }
                ++mismatches[f];
            }
        }
    }

    for (std::size_t f = 0U; f < kFlagCount; ++f) {
        /* kName[f] identifies which flag drifted; firstBad[f] says when. */
        CHECK(kName[f] != nullptr);
        CHECK_EQ(mismatches[f], 0U);
        CHECK_EQ(firstBad[f], 0U);
    }
}

/* The counter must be 32 bits wide. With an 8-bit counter the 0x3FF and 0x7FF
 * tests can never match, so the 1024 ms parameter save and the 2048 ms autobaud
 * retry would never fire and main_loop's phase 1 would spin forever. */
TEST(sched_the_1024ms_and_2048ms_flags_are_reachable_at_all)
{
    fake::reset();

    bool sawSlow     = false;
    bool sawAutobaud = false;

    for (std::uint32_t ms = 1U; ms <= 2048U; ++ms) {
        clearFlags();
        svc::sched::onMillisecond();

        if (ram::app().sched.flagSlow != 0U) {
            sawSlow = true;
            CHECK_EQ(ram::app().sched.counter % 1024U, 1023U);
        }
        if (ram::app().sched.flagAutobaud != 0U) {
            sawAutobaud = true;
            CHECK_EQ(ram::app().sched.counter, 2047U);
        }
    }

    CHECK(sawSlow);
    CHECK(sawAutobaud);
    CHECK_EQ(ram::app().sched.counter, 2048U);
    CHECK_EQ(sizeof(ram::app().sched.counter), 4U);
}

/* The staggered phases (1, 3, 7, 15) exist so the 4/8/16/32 ms jobs never land
 * on the same millisecond -- that is what keeps the worst-case main_loop pass
 * inside the ~65.5 ms watchdog. */
TEST(sched_the_four_fast_cadences_never_coincide)
{
    fake::reset();

    std::uint32_t worstCollision = 0U;

    for (std::uint32_t ms = 1U; ms <= kSweepMs; ++ms) {
        clearFlags();
        svc::sched::onMillisecond();

        std::uint32_t fired = 0U;
        for (std::size_t f = 0U; f < 4U; ++f) {
            if (flagAt(f) != 0U) { ++fired; }
        }
        if (fired > 1U && worstCollision == 0U) { worstCollision = ms; }
    }

    CHECK_EQ(worstCollision, 0U);
}

/* From 64 ms down every flag triggers on an all-ones mask, so they DO coincide.
 * That is stock, not a transcription slip, and the 2047th millisecond is the
 * one where all six of them plus nothing else land together. */
TEST(sched_the_slow_cadences_do_coincide_at_2047ms)
{
    fake::reset();

    for (std::uint32_t ms = 1U; ms < 2047U; ++ms) {
        clearFlags();
        svc::sched::onMillisecond();
    }
    clearFlags();
    svc::sched::onMillisecond();            /* ms 2047 */

    CHECK_EQ(ram::app().sched.counter, 2047U);
    for (std::size_t f = 4U; f < kFlagCount; ++f) {
        CHECK(flagAt(f) != 0U);             /* 64, 128, 256, 512, 1024, 2048 */
    }
    for (std::size_t f = 0U; f < 4U; ++f) {
        CHECK(flagAt(f) == 0U);             /* 4, 8, 16, 32 stay clear */
    }
}

/* ---------------------------------------------------------------------------
 * Flag consumption -- main_loop @0x080088D8
 * ------------------------------------------------------------------------- */

TEST(sched_take_accessors_are_test_and_clear)
{
    fake::reset();

    CHECK_FALSE(svc::sched::takeFast());
    CHECK_FALSE(svc::sched::takeMed());
    CHECK_FALSE(svc::sched::takeSlow());

    ram::app().sched.flagFast = 1U;
    ram::app().sched.flagMed  = 1U;
    ram::app().sched.flagSlow = 1U;

    CHECK(svc::sched::takeFast());
    CHECK_EQ(ram::app().sched.flagFast, 0U);
    CHECK_FALSE(svc::sched::takeFast());

    CHECK(svc::sched::takeMed());
    CHECK_EQ(ram::app().sched.flagMed, 0U);
    CHECK_FALSE(svc::sched::takeMed());

    CHECK(svc::sched::takeSlow());
    CHECK_EQ(ram::app().sched.flagSlow, 0U);
    CHECK_FALSE(svc::sched::takeSlow());

    /* Taking one must not disturb its neighbours, which share a RAM run. */
    ram::app().sched.flagFast     = 1U;
    ram::app().sched.flagDiv16    = 1U;
    ram::app().sched.flagAutobaud = 1U;
    CHECK(svc::sched::takeFast());
    CHECK_EQ(ram::app().sched.flagDiv16, 1U);
    CHECK_EQ(ram::app().sched.flagAutobaud, 1U);
}

/* The stock phase-1 loop clears the autobaud flag in the retry branch and
 * deliberately NOT in the give-up branch, so the shutdown path runs on every
 * pass until the WFI takes effect. A test-and-clear accessor would break that,
 * so reading and clearing are separate entry points. */
TEST(sched_autobaudDue_reads_the_flag_without_clearing_it)
{
    fake::reset();

    CHECK_FALSE(svc::sched::autobaudDue());

    ram::app().sched.flagAutobaud = 1U;
    CHECK(svc::sched::autobaudDue());
    CHECK_EQ(ram::app().sched.flagAutobaud, 1U);

    /* Still due on the next pass, and the one after that -- this is what makes
     * the give-up branch re-enter the shutdown path every time round. */
    CHECK(svc::sched::autobaudDue());
    CHECK(svc::sched::autobaudDue());
    CHECK_EQ(ram::app().sched.flagAutobaud, 1U);

    svc::sched::clearAutobaud();
    CHECK_EQ(ram::app().sched.flagAutobaud, 0U);
    CHECK_FALSE(svc::sched::autobaudDue());
}

/* ---------------------------------------------------------------------------
 * The 10 ms cycle counter -- 0x0800853C
 * ------------------------------------------------------------------------- */

TEST(sched_cycle_counter_rolls_from_one_not_zero)
{
    fake::reset();

    ram::app().cycleCtrMax = 5U;
    ram::app().cycleCtr    = 0U;

    const std::uint8_t expected[] = {1U, 2U, 3U, 4U, 1U, 2U, 3U, 4U, 1U};
    for (std::size_t i = 0U; i < sizeof expected / sizeof expected[0]; ++i) {
        svc::sched::tickCycleCounter();
        CHECK_EQ(ram::app().cycleCtr, expected[i]);
    }

    /* The comparison is against the POST-increment value, so cycleCtrMax
     * itself is never observable by a consumer. */
    for (std::size_t i = 0U; i < 64U; ++i) {
        svc::sched::tickCycleCounter();
        CHECK(ram::app().cycleCtr != ram::app().cycleCtrMax);
        CHECK(ram::app().cycleCtr != 0U);
    }
}

/* ---------------------------------------------------------------------------
 * The divider chain -- 0x08002F72
 * ------------------------------------------------------------------------- */

TEST(sched_isr_does_nothing_when_the_update_flag_is_not_set)
{
    fake::reset();
    fake::returns().timItStatus = 0;

    svc::sched::onTimerInterrupt();

    CHECK_EQ(fake::countOf("tim_clear_it_pending"), 0U);
    CHECK_EQ(ram::app().sched.counter, 0U);
    CHECK_EQ(ram::app().tickDiv10ms, 0U);
}

TEST(sched_isr_acknowledges_the_update_flag_and_ticks_once_per_call)
{
    fake::reset();
    fake::returns().timItStatus = 1;

    svc::sched::onTimerInterrupt();

    CHECK_EQ(fake::countOf("tim_clear_it_pending"), 1U);
    const fake::Call* ack = fake::lastCall("tim_clear_it_pending");
    CHECK(ack != nullptr);
    if (ack != nullptr) {
        CHECK_EQ(ack->a0, 0x02U);           /* TIM2, per fake::tag() */
        CHECK_EQ(ack->a1, TIM_INT_UPDATE);
    }
    CHECK_EQ(ram::app().sched.counter, 1U);
    CHECK_EQ(ram::app().tickDiv10ms, 1U);
}

/* 10 interrupts -> the 10 ms divider wraps and the 10 ms hooks run once. The
 * evidence is the software-timer pool ageing (svc::timers::tickShortTimers)
 * and the cycle counter advancing (tickCycleCounter). */
TEST(sched_isr_runs_the_10ms_hooks_every_tenth_interrupt)
{
    fake::reset();
    fake::returns().timItStatus = 1;

    ram::app().cycleCtrMax     = 100U;
    ram::app().softTimers[0x20] = 150;

    for (std::uint32_t i = 0U; i < 9U; ++i) { svc::sched::onTimerInterrupt(); }
    CHECK_EQ(ram::app().tickDiv10ms, 9U);
    CHECK_EQ(ram::app().softTimers[0x20], 150);   /* not yet */
    CHECK_EQ(ram::app().cycleCtr, 0U);

    svc::sched::onTimerInterrupt();               /* the 10th */
    CHECK_EQ(ram::app().tickDiv10ms, 0U);
    CHECK_EQ(ram::app().tickDiv100ms, 1U);
    CHECK_EQ(ram::app().softTimers[0x20], 149);
    CHECK_EQ(ram::app().cycleCtr, 1U);

    for (std::uint32_t i = 0U; i < 90U; ++i) { svc::sched::onTimerInterrupt(); }
    CHECK_EQ(ram::app().softTimers[0x20], 140);   /* 100 ms = 10 agings */
    CHECK_EQ(ram::app().cycleCtr, 10U);
}

/* 100 interrupts -> the 100 ms hooks run once. The evidence is the soft clock
 * advancing by one tenth (svc::clock::tick). */
TEST(sched_isr_runs_the_100ms_hooks_every_hundredth_interrupt)
{
    fake::reset();
    fake::returns().timItStatus = 1;
    ram::app().cycleCtrMax = 100U;

    for (std::uint32_t i = 0U; i < 99U; ++i) { svc::sched::onTimerInterrupt(); }
    CHECK_EQ(ram::app().softClock.tenths, 0U);

    svc::sched::onTimerInterrupt();               /* the 100th */
    CHECK_EQ(ram::app().softClock.tenths, 1U);
    CHECK_EQ(ram::app().tickDiv100ms, 0U);
    CHECK_EQ(ram::app().tickDiv1s, 1U);

    for (std::uint32_t i = 0U; i < 400U; ++i) { svc::sched::onTimerInterrupt(); }
    CHECK_EQ(ram::app().softClock.tenths, 5U);
}

/* 1000 interrupts -> one second. The 1 s divider wraps back to zero and the
 * soft clock reads exactly one second, which is the firmware's own proof that
 * the interrupt rate is 1 kHz. */
TEST(sched_isr_divider_chain_yields_exactly_one_second_per_1000_interrupts)
{
    fake::reset();
    fake::returns().timItStatus = 1;
    ram::app().cycleCtrMax = 100U;

    for (std::uint32_t i = 0U; i < 1000U; ++i) { svc::sched::onTimerInterrupt(); }

    CHECK_EQ(ram::app().sched.counter, 1000U);
    CHECK_EQ(ram::app().softClock.second, 1U);
    CHECK_EQ(ram::app().softClock.tenths, 0U);
    CHECK_EQ(ram::app().tickDiv10ms, 0U);
    CHECK_EQ(ram::app().tickDiv100ms, 0U);
    CHECK_EQ(ram::app().tickDiv1s, 0U);          /* wrapped at 10 */

    for (std::uint32_t i = 0U; i < 59000U; ++i) { svc::sched::onTimerInterrupt(); }
    CHECK_EQ(ram::app().softClock.minute, 1U);
    CHECK_EQ(ram::app().softClock.second, 0U);
}

/* ---------------------------------------------------------------------------
 * Bring-up -- 0x080027F8
 * ------------------------------------------------------------------------- */

TEST(sched_start_programs_the_1ms_timebase)
{
    fake::reset();

    svc::sched::start();

    /* PRIGROUP = 3, written by tim2_init itself in the stock image. */
    const fake::Call* grp = fake::lastCall("nvic_set_priority_grouping");
    CHECK(grp != nullptr);
    if (grp != nullptr) { CHECK_EQ(grp->a0, 0x300U); }

    /* TIM2 is IRQ 28 on the N32L40x (the stock code passes 0x1C), preempt 4,
     * sub 0, enabled. The fake packs preempt<<8 | sub into a1. */
    const fake::Call* irq = fake::lastCall("nvic_init");
    CHECK(irq != nullptr);
    if (irq != nullptr) {
        CHECK_EQ(irq->a0, 28U);
        CHECK_EQ(irq->a1, (4U << 8) | 0U);
        CHECK_EQ(irq->a2, 1U);
    }

    const fake::Call* clk = fake::lastCall("rcc_apb1_periph_clock");
    CHECK(clk != nullptr);
    if (clk != nullptr) {
        CHECK_EQ(clk->a0, RCC_APB1_TIM2);
        CHECK_EQ(clk->a1, 1U);
    }

    /* AR = 4000. The prescaler in the init block is 0: the stock code writes
     * PSC twice, and only the second write is the real one. */
    const fake::Call* tb = fake::lastCall("tim_timebase_init");
    CHECK(tb != nullptr);
    if (tb != nullptr) {
        CHECK_EQ(tb->a0, 0x02U);            /* TIM2 */
        CHECK_EQ(tb->a1, 0U);               /* Prescaler as passed in the block */
        CHECK_EQ(tb->a2, 4000U);            /* Period */
    }

    /* PSC = 7 with an immediate reload (EVTGEN = UG):
     * 32 MHz / (7 + 1) / 4000 = 1000 Hz. */
    const fake::Call* psc = fake::lastCall("tim_prescaler_config");
    CHECK(psc != nullptr);
    if (psc != nullptr) {
        CHECK_EQ(psc->a0, 0x02U);
        CHECK_EQ(psc->a1, 7U);
        CHECK_EQ(psc->a2, TIM_EVTGEN_UG);
    }

    const fake::Call* it = fake::lastCall("tim_it_config");
    CHECK(it != nullptr);
    if (it != nullptr) {
        CHECK_EQ(it->a1, TIM_INT_UPDATE);
        CHECK_EQ(it->a2, 1U);
    }

    const fake::Call* run = fake::lastCall("tim_cmd");
    CHECK(run != nullptr);
    if (run != nullptr) { CHECK_EQ(run->a1, 1U); }
}

/* The interrupt is unmasked in the NVIC BEFORE the counter is started, exactly
 * as the stock code does. Safe only because main_loop runs the whole init
 * sequence with PRIMASK set. */
TEST(sched_start_enables_the_nvic_before_the_counter)
{
    fake::reset();

    svc::sched::start();

    std::size_t nvicAt = fake::kMaxCalls;
    std::size_t cmdAt  = fake::kMaxCalls;
    for (std::size_t i = 0U; i < fake::callCount(); ++i) {
        const char* n = fake::calls()[i].name;
        if (nvicAt == fake::kMaxCalls && std::strcmp(n, "nvic_init") == 0) {
            nvicAt = i;
        }
        if (cmdAt == fake::kMaxCalls && std::strcmp(n, "tim_cmd") == 0) {
            cmdAt = i;
        }
    }
    CHECK(nvicAt < fake::callCount());
    CHECK(cmdAt < fake::callCount());
    CHECK(nvicAt < cmdAt);
}
