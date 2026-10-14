/* ============================================================================
 * test_interrupts.cpp -- the five hardware entry points.
 *
 * Written from the stock handlers in reference/decompile/app/{tim,can,uart,gpio}.c,
 * not from app/logic/interrupts.cpp.
 *
 * WHAT THESE CAN AND CANNOT CATCH
 * The handlers' NAMES are an ABI: app/startup/crt0.S branches to them by symbol,
 * so a renamed or C++-mangled one links cleanly and silently leaves the weak
 * trap in place. That failure is caught by tools/check_stubs.py against the
 * linked ELF, not here -- the host build has no crt0.
 *
 * What IS checkable here, and was not checked anywhere before, is the other half:
 * that each shim acknowledges the right hardware condition and delegates to a
 * body that does the stock thing. A handler with the right name that forgets to
 * clear its pending flag re-enters forever; one that forgets to delegate leaves
 * the firmware deaf on that peripheral.
 *
 * NOT COVERED, deliberately: the two EXTI paths with a pending flag set. Both
 * end in fatal_error_reset(), whose body is `for (;;) { ...; wwdg_force_reset(); }`
 * -- and the host fake returns from that call rather than resetting, so the loop
 * would spin forever. See the contract note in tests/host/fake_hal.hpp. The
 * not-pending path IS covered below, which is the branch that decides whether
 * the reset happens at all.
 * ==========================================================================*/

#include "test_framework.hpp"

#include "can_bus.hpp"
#include "fake_hal.hpp"
#include "head_unit.hpp"
#include "ram_map.hpp"
#include "scheduler.hpp"

extern "C" {
#include "hal_exti.h"
#include "hal_tim.h"
}

/* The handlers, as crt0.S spells them. Declared here rather than included from
 * a header on purpose: this is the ABI, and restating it is the test. */
extern "C" void TIM2_IRQHandler(void);
extern "C" void CAN_RX0_IRQHandler(void);
extern "C" void USART2_IRQHandler(void);
extern "C" void EXTI2_IRQHandler(void);
extern "C" void EXTI9_5_IRQHandler(void);

namespace {

constexpr std::uint32_t kTagTim2 = 0x02U;   /* fake_hal port/peripheral tags */
constexpr std::uint32_t kLine2   = 0x0004U; /* PB2 */
constexpr std::uint32_t kLine8   = 0x0100U; /* PB8, the deep-sleep wake line */

}  // namespace

/* ===========================================================================
 * TIM2 -- veneer slot 37, IRQ 28. The 1 ms time base.
 * ========================================================================= */

TEST(isr_tim2_does_nothing_when_the_update_flag_is_clear)
{
    fake::reset();
    fake::returns().timItStatus = 0;        /* no update event pending */

    TIM2_IRQHandler();

    /* Stock gates its whole body on TIM_GetIntStatus(TIM2, UPDATE). Running the
     * tick on a spurious entry would advance time that did not pass. */
    CHECK_EQ(fake::countOf("tim_clear_it_pending"), 0U);
    CHECK_EQ(ram::app().sched.counter, 0U);
    CHECK_EQ(ram::app().tickDiv10ms, 0U);
}

TEST(isr_tim2_acknowledges_before_running_the_tick)
{
    fake::reset();
    fake::returns().timItStatus = 1;

    TIM2_IRQHandler();

    const fake::Call* c = fake::lastCall("tim_clear_it_pending");
    CHECK(c != nullptr);
    if (c != nullptr) {
        CHECK_EQ(c->a0, kTagTim2);
        CHECK_EQ(c->a1, TIM_INT_UPDATE);
    }
    /* ...and the tick actually advanced. */
    CHECK_EQ(ram::app().sched.counter, 1U);
}

TEST(isr_tim2_drives_the_divider_chain_at_the_stock_rates)
{
    fake::reset();
    fake::returns().timItStatus = 1;

    /* 1000 interrupts == one second: 100 ten-ms hooks, 10 hundred-ms hooks. */
    for (int i = 0; i < 1000; ++i) { TIM2_IRQHandler(); }

    CHECK_EQ(ram::app().sched.counter, 1000U);
    /* The soft clock advances once per 100 ms, so one second of ticks is ten
     * tenths -- i.e. exactly one second on the firmware's only clock. */
    CHECK_EQ(ram::app().softClock.second, 1U);
    CHECK_EQ(ram::app().softClock.tenths, 0U);
}

TEST(isr_tim2_raises_the_cadence_flags_the_main_loop_waits_on)
{
    fake::reset();
    fake::returns().timItStatus = 1;

    /* Far enough to reach the slowest flag the superloop consumes (1024 ms). */
    for (int i = 0; i < 1024; ++i) { TIM2_IRQHandler(); }

    CHECK(svc::sched::takeFast());
    CHECK(svc::sched::takeMed());
    CHECK(svc::sched::takeSlow());
}

/* ===========================================================================
 * CAN_RX0 -- veneer slot 29, IRQ 54 (not the STM32F1 value 20).
 * ========================================================================= */

TEST(isr_can_rx0_pushes_a_frame_into_the_ring)
{
    fake::reset();

    CAN_RX0_IRQHandler();

    /* The stock handler is a bare call to the ring push: it does NOT test a
     * status bit first, because releasing the FIFO is what clears FMP0. */
    CHECK_EQ(fake::countOf("can_receive_fifo"), 1U);
    CHECK_EQ(ram::app().canRing.rxHead, 1U);
}

TEST(isr_can_rx0_reads_fifo_zero)
{
    fake::reset();

    CAN_RX0_IRQHandler();

    const fake::Call* c = fake::lastCall("can_receive_fifo");
    CHECK(c != nullptr);
    if (c != nullptr) {
        CHECK_EQ(c->a1, 0U);     /* FIFO0 -- the only one this firmware enables */
    }
}

/* ===========================================================================
 * USART2 -- veneer slot 47, IRQ 38. The head-unit link.
 * ========================================================================= */

TEST(isr_usart2_stores_a_received_byte_in_the_rx_ring)
{
    fake::reset();
    fake::returns().usartItStatus = 1;      /* RXNE asserted */
    fake::returns().usartRxByte   = 0xAAU;

    USART2_IRQHandler();

    CHECK_EQ(fake::countOf("usart_receive_data"), 1U);
    /* Landed at the ring head, and the head moved on. */
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbRxRing], 0xAAU);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbRxHead], 1U);
}

TEST(isr_usart2_does_nothing_when_no_source_is_asserted)
{
    fake::reset();
    fake::returns().usartItStatus = 0;

    USART2_IRQHandler();

    CHECK_EQ(fake::countOf("usart_receive_data"), 0U);
    CHECK_EQ(fake::countOf("usart_send_data"), 0U);
    CHECK_EQ(ram::app().uartFrame[svc::hu::kFbRxHead], 0U);
}

/* ===========================================================================
 * EXTI2 / EXTI9_5 -- the two deliberate reset paths.
 * ========================================================================= */

TEST(isr_exti2_ignores_an_interrupt_that_is_not_its_line)
{
    fake::reset();
    fake::returns().extiPending = 0;

    EXTI2_IRQHandler();

    /* No clear, and crucially no reset: the handler is shared, so it must test
     * its own line before acting. */
    CHECK_EQ(fake::countOf("exti_clear_it_pending"), 0U);
    CHECK_EQ(fake::countOf("wwdg_force_reset"), 0U);
    CHECK_EQ(ram::app().fatalErrorCode, 0U);
}

TEST(isr_exti2_tests_line_2)
{
    fake::reset();
    fake::returns().extiPending = 0;

    EXTI2_IRQHandler();

    const fake::Call* c = fake::lastCall("exti_get_it_status");
    CHECK(c != nullptr);
    if (c != nullptr) { CHECK_EQ(c->a0, kLine2); }
}

TEST(isr_exti9_5_ignores_an_interrupt_that_is_not_line_8)
{
    fake::reset();
    fake::returns().extiPending = 0;

    /* EXTI9_5 is shared across lines 5..9; only line 8 (PB8, the CAN_RX pin
     * re-purposed as the deep-sleep wake source) is ever armed. */
    EXTI9_5_IRQHandler();

    CHECK_EQ(fake::countOf("exti_clear_it_pending"), 0U);
    CHECK_EQ(fake::countOf("wwdg_force_reset"), 0U);
}

TEST(isr_exti9_5_tests_line_8)
{
    fake::reset();
    fake::returns().extiPending = 0;

    EXTI9_5_IRQHandler();

    const fake::Call* c = fake::lastCall("exti_get_it_status");
    CHECK(c != nullptr);
    if (c != nullptr) { CHECK_EQ(c->a0, kLine8); }
}
