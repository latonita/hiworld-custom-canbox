/* ============================================================================
 * test_power.cpp -- svc::power: the fault path and the give-up path.
 *
 * Written from the behaviour in reference/decompile/app/sys.c (fatal_error_reset
 * @0x08002E64) and reference/decompile/app/misc.c (the shutdown path
 * @0x080032F6), not from app/services/power.cpp.
 *
 * WHY THIS FILE MATTERS MORE THAN ITS SIZE SUGGESTS
 * shutdownDeepSleep() is the one function in this firmware whose failure mode is
 * unrecoverable in the field: if PB8 is not armed as an EXTI wake source before
 * the WFI, the box sleeps and never comes back without a power cycle. The order
 * is load-bearing and was previously documented but unenforced -- these cases
 * are the enforcement.
 *
 * Reset paths are reached through fake::expectReset(), because the host fake
 * models wwdg_force_reset() as a throw (see fake_hal.hpp); on target it spins
 * on the watchdog and never returns.
 * ==========================================================================*/

#include "test_framework.hpp"

#include <initializer_list>

#include "fake_hal.hpp"
#include "power.hpp"
#include "ram_map.hpp"

extern "C" {
#include "hal_adc.h"
#include "hal_gpio.h"
#include "hal_pwr.h"
#include "hal_rcc.h"
}

namespace {

/* Index of the first call to `name` in the recorded log, or a huge sentinel if
 * it never happened. Lets a test assert ORDER by comparing positions. */
std::size_t posOf(const char* name) noexcept
{
    for (std::size_t i = 0U; i < fake::callCount(); ++i) {
        if (std::strcmp(fake::calls()[i].name, name) == 0) { return i; }
    }
    return 9999U;
}

constexpr std::uint32_t kTagGpiob = 0xB0U;   /* fake_hal's port tags */
constexpr std::uint32_t kPinPb12  = 0x1000U; /* transceiver standby  */

}  // namespace

TEST(power_fault_codes_do_not_collide_with_the_crt0_trap_codes)
{
    fake::reset();

    /* app/startup/crt0.S assigns 100..164 to the stock veneer traps and
     * 217/229/232/237/247 to the five slots that should carry a real handler.
     * Ours must be distinguishable from both or the byte is useless. */
    for (const std::uint8_t c : { svc::power::kFatalBusDown,
                                  svc::power::kFatalLinkWedged,
                                  svc::power::kFatalHostCommand }) {
        CHECK(c > 164U);
        CHECK(c != 217U);
        CHECK(c != 229U);
        CHECK(c != 232U);
        CHECK(c != 237U);
        CHECK(c != 247U);
    }
}

/* ===========================================================================
 * disableWatchdogClock @0x08002E18
 * ========================================================================= */

TEST(power_disable_watchdog_clock_gates_wwdg_not_can)
{
    fake::reset();

    svc::power::disableWatchdogClock();

    const fake::Call* c = fake::lastCall("rcc_apb1_periph_clock");
    CHECK(c != nullptr);
    if (c != nullptr) {
        /* The stock decompile misnames this one "can_clock_disable". 0x800 is
         * the WWDG enable bit; CAN's is 0x2000000. Getting this wrong would
         * leave the watchdog clocked through the sleep and reset the box
         * instead of letting it sleep. */
        CHECK_EQ(c->a0, RCC_APB1_WWDG);
        CHECK_EQ(c->a0, 0x00000800U);
        CHECK_EQ(c->a1, 0U);              /* disable */
    }
}

/* ===========================================================================
 * shutdownDeepSleep @0x080032F6 -- the give-up path
 * ========================================================================= */

TEST(power_shutdown_does_nothing_at_all_when_can_refuses_to_sleep)
{
    fake::reset();
    fake::returns().canSleepResult = 0;    /* controller did not acknowledge */

    svc::power::shutdownDeepSleep();

    /* Stock returns immediately and the caller keeps spinning; the watchdog is
     * still clocked at that point, so a wedge ends in a reset rather than a
     * hang. Nothing may be torn down on this path. */
    CHECK_EQ(fake::countOf("pwr_enter_stop"), 0U);
    CHECK_EQ(fake::countOf("usart_disable"), 0U);
    CHECK_EQ(fake::countOf("adc_enable"), 0U);
    CHECK_EQ(fake::countOf("exti_line_init"), 0U);
    CHECK_EQ(fake::countOf("gpio_write_pin"), 0U);
    CHECK_EQ(fake::countOf("wwdg_force_reset"), 0U);
}

TEST(power_shutdown_arms_the_pb8_wake_source_before_sleeping)
{
    fake::reset();

    /* THE ONE THAT MATTERS. EXTI line 8 on PB8 is the only wake source armed
     * before the WFI. If pwr_enter_stop runs first, the box sleeps with no way
     * back and needs a power cycle. */
    svc::power::shutdownDeepSleep();

    CHECK(posOf("exti_line_init") < posOf("pwr_enter_stop"));
    CHECK(posOf("exti_set_line_source") < posOf("pwr_enter_stop"));
    CHECK(posOf("nvic_init") < posOf("pwr_enter_stop"));
    CHECK_EQ(fake::countOf("pwr_enter_stop"), 1U);
}

TEST(power_shutdown_follows_the_stock_teardown_order)
{
    fake::reset();

    svc::power::shutdownDeepSleep();

    /* CAN must acknowledge sleep FIRST -- PB8 is only re-armed as the EXTI
     * input once the CAN peripheral has released it. */
    CHECK(posOf("can_sleep") < posOf("gpio_write_pin"));
    /* ...then the transceiver to standby, the status lines down, the ADC off,
     * the UART off, the watchdog unclocked, and only then the wake arm. */
    CHECK(posOf("gpio_write_pin")   < posOf("adc_dma_cmd"));
    CHECK(posOf("adc_dma_cmd")      < posOf("usart_disable"));
    CHECK(posOf("usart_disable")    < posOf("rcc_apb1_periph_clock"));
    CHECK(posOf("rcc_apb1_periph_clock") < posOf("exti_line_init"));
    CHECK(posOf("exti_line_init")   < posOf("pwr_enter_stop"));
}

TEST(power_shutdown_puts_the_transceiver_into_standby)
{
    fake::reset();

    svc::power::shutdownDeepSleep();

    /* PB12 HIGH = transceiver asleep. Driving it the wrong way leaves the
     * transceiver drawing current with the MCU stopped. */
    const fake::Call* c = fake::nthCall("gpio_write_pin", 0U);
    CHECK(c != nullptr);
    if (c != nullptr) {
        CHECK_EQ(c->a0, kTagGpiob);
        CHECK_EQ(c->a1, kPinPb12);
        CHECK_EQ(c->a2, 1U);
    }
}

TEST(power_shutdown_turns_all_three_adc_blocks_off)
{
    fake::reset();

    svc::power::shutdownDeepSleep();

    /* Inert on this board -- the ADC is never clocked -- but reproduced from
     * stock, so a missing call is a divergence worth catching. */
    CHECK_EQ(fake::countOf("adc_dma_cmd"), 1U);
    CHECK_EQ(fake::countOf("adc_enable"), 1U);
    CHECK_EQ(fake::countOf("adc_sw_start_regular"), 1U);
    for (const char* n : { "adc_dma_cmd", "adc_enable", "adc_sw_start_regular" }) {
        const fake::Call* c = fake::lastCall(n);
        CHECK(c != nullptr);
        if (c != nullptr) {
            CHECK_EQ(c->a0, ADC_BASE_ADDR);
            CHECK_EQ(c->a1, 0U);           /* all disabled */
        }
    }
}

TEST(power_shutdown_enters_stop_with_wfi_and_the_stock_wake_config)
{
    fake::reset();

    svc::power::shutdownDeepSleep();

    const fake::Call* c = fake::lastCall("pwr_enter_stop");
    CHECK(c != nullptr);
    if (c != nullptr) {
        CHECK_EQ(c->a0, 1U);                  /* WFI, not the double-WFE path */
        CHECK_EQ(c->a1, PWR_CTRL3_WUF_VAL);
    }
}

TEST(power_shutdown_forces_a_reset_on_wake)
{
    fake::reset();

    /* The watchdog is unclocked by this point, so on real hardware this write
     * lands on a dead peripheral and the reset actually comes from the EXTI9_5
     * handler. Reproduced from stock, and it is the correct fallback if the
     * clock ever IS still running -- so the call must still be made. */
    svc::power::shutdownDeepSleep();

    CHECK_EQ(fake::countOf("wwdg_force_reset"), 1U);
    CHECK(posOf("pwr_enter_stop") < posOf("wwdg_force_reset"));
}
