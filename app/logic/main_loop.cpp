/* ============================================================================
 * main_loop.cpp -- 0x080088D8, the application's entry point.
 *
 * Reached from app/startup/startup.c's _start_c, which the bootloader enters
 * via `bl 0x08002202`. Never returns.
 *
 * Runs on the bootloader's vector table (VTOR = 0x08000000) and the
 * bootloader's stack (MSP = 0x20000600). It sets neither.
 *
 * ---- PHASE 1: find the bus ------------------------------------------------
 * The box does not know its CAN bit rate at boot. Interrupts are unmasked and
 * the loop spins until three frames have landed in the RX ring. Every 2048 ms
 * with none, it toggles the stored profile byte, rewrites the parameter page
 * and re-initialises CAN at the other rate, up to 8 times. After the 8th
 * failure it takes the shutdown path: CAN asleep, peripherals off, WFI.
 *
 * On a bench with no bus attached the box goes quiet ~18.4 s after boot. Note
 * that is NOT 8 x 2048 ms: the 8th retry lands at 16.38 s, and the give-up
 * branch is only taken on the FOLLOWING tick, when the counter is already 8.
 * Waiting 16.4 s and finding the box still alive is correct, not a fault --
 * see app/REBUILD.md §6.
 *
 * ---- PHASE 2: the steady superloop ---------------------------------------
 * Latches the winning profile and services three cadence flags raised by the
 * 1 ms TIM2 interrupt:
 *      4 ms     heartbeat edge detect, CAN TX drain, soft-timer scan
 *      8 ms     CAN decode + link state machine, head-unit service, status pins
 *      1024 ms  parameter page save, if dirty
 *
 * ---- THE WATCHDOG --------------------------------------------------------
 * Kicked at the top of BOTH loops. It is armed with a ~65.5 ms timeout, so
 * every path through either loop must stay well inside that. This is the
 * constraint that limits how much work any of the tasks above may do.
 * ==========================================================================*/

#include <cstdint>

#include "app_config.hpp"
#include "ram_map.hpp"

#include "raise_benz_tx.hpp"
#include "board_io.hpp"
#include "custom_tx.hpp"
#include "can_bus.hpp"
#include "can_link.hpp"
#include "periodic_tx.hpp"
#include "head_unit.hpp"
#include "param_store.hpp"
#include "power.hpp"
#include "scheduler.hpp"
#include "soft_clock.hpp"
#include "soft_timer.hpp"

extern "C" {
#include "hal_cpu.h"
#include "hal_rcc.h"
#include "hal_wwdg.h"
}

namespace {

/* ---------------------------------------------------------------------------
 * The boot sequence, with interrupts masked throughout.
 *
 * The stock image wrapped the first three steps in one glue function
 * (clock_tim2_pwr_init @0x080032D2); they are spelled out here because the
 * ORDER is the interesting part and it is not obvious from a call to a wrapper.
 *
 * In particular: the bus prescalers must be in place BEFORE the timer is set
 * up, because TIM2's reload arithmetic assumes PCLK1 is already divided. Get
 * that backwards and the tick runs at the wrong rate, which silently changes
 * every cadence in the firmware.
 * ------------------------------------------------------------------------- */
void bootSequence() noexcept
{
    svc::params::load();                 /* 0x08002DBC  32 bytes <- flash page */

    rcc_set_bus_prescalers_div4();       /* PPRE1 = PPRE2 = /4, so PCLK = 16 MHz */
    svc::sched::start();                 /* 0x080027F8  TIM2 -> 1 ms interrupt   */
    rcc_apb1_periph_clock(RCC_APB1_PWR, 1);
    /* PWREN is enabled here, not in the shutdown path, even though the PWR
     * block is only ever touched by the deep-sleep entry. Stock behaviour. */

    svc::io::statusLinesInit();          /* 0x08008458  PB5/PC15/PB6/PB4, PB2 in */
    svc::clock::init();                  /* 0x080078DA  zero the soft RTC        */
    svc::hu::init();                     /* 0x08008A54  USART2 + version string  */
    svc::params::validateDefaults();     /* 0x08007B6C  magic 0xAA, clamp, latch */
    logic::primeTxTemplates();           /* 0x08007B6C  the CAN frame templates */
    /* The 0x41 page selectors. Must happen before the first transmit: a zero
     * page byte matches no page and the head unit drops the frame. */
    if constexpr (cfg::kIsBenz) {
        logic::benzPrimeFrames();
    } else {
        /* The custom telemetry block's "no value" sentinels. Must happen
         * before the first transmit for the same reason: a zeroed payload is
         * not a blank, it is -40 degC coolant and 0.00 V. */
        logic::customPrimeFrames();
    }
    svc::can::setBaudByProfile();        /* 0x08007B2C  first CAN bring-up       */

    wwdg_init();                         /* 0x08002DEC  arm the watchdog         */
}

/* ---------------------------------------------------------------------------
 * Phase 1. Returns once the bus has answered; may never return at all if it
 * does not (the give-up path sleeps, and the wake forces a reset).
 * ------------------------------------------------------------------------- */
void waitForBus() noexcept
{
    ram::app().autobaudTries = 0U;

    cpu_irq_enable();  /* the clock starts here */

    /* The watchdog is kicked BEFORE the exit test, matching the stock
     * `while (wwdg_kick(), rxHead < 3)` -- so the pass that leaves the loop has
     * kicked too, and phase 2 starts with a full window. */
    for (;;) {
        wwdg_kick();
        if (ram::app().canRing.rxHead >= cfg::kAutobaudFramesNeeded) {
            return;
        }

        if (!svc::sched::autobaudDue()) {
            continue;
        }

        /* Tell the head unit we are still looking. */
        ram::app().vehicle.setFlag(ram::VehicleState::kFlagAutobaud, true);

        if (ram::app().autobaudTries >= cfg::kAutobaudMaxTries) {
            /* Out of attempts. Note that the stock code deliberately does NOT
             * clear the autobaud flag here, so this runs on every pass until
             * the WFI inside takes effect. Reproduced. */
            svc::power::shutdownDeepSleep();   /* 0x080032F6 */
            continue;
        }

        /* Toggle the profile: 1 -> 0, anything else -> 1. This is a FLIP-FLOP,
         * not a cycle -- with a third profile it would never reach the third. */
        static_assert(cfg::kProfileCount == 2U,
                      "the autobaud toggle below alternates between exactly two "
                      "profiles; more would need a modular step");
        svc::params::setProfile(svc::params::profile() != cfg::kProfileFca125k
                                    ? cfg::kProfileFca125k
                                    : cfg::kProfileJeep83k);
        svc::params::save();

        cpu_irq_disable();
        svc::can::setBaudByProfile();
        cpu_irq_enable();

        ++ram::app().autobaudTries;
        svc::sched::clearAutobaud();
    }
}

/* ---------------------------------------------------------------------------
 * Phase 2. Never returns.
 * ------------------------------------------------------------------------- */
[[noreturn]] void superLoop() noexcept
{
    for (;;) {
        wwdg_kick();

        /* Edge-detect on the vehicle state's change counter: when the decoded
         * state changed, bump the counter the head unit watches. */
        ram::VehicleState& vs = ram::app().vehicle;
        if (vs.at(ram::VehicleState::kChangeShadow)
            != vs.at(ram::VehicleState::kChangeSource)) {
            vs.at(ram::VehicleState::kChangeShadow) =
                vs.at(ram::VehicleState::kChangeSource);
            ++vs.at(ram::VehicleState::kChangeCount);
        }

        if (svc::sched::takeFast()) {            /* 4 ms */
            svc::clock::blinkEdgeDetect();       /* 0x08007858 */
            svc::can::txDrain();                 /* 0x08007698 */
            svc::timers::scanActive();           /* 0x08007D12 */
        }

        if (svc::sched::takeMed()) {             /* 8 ms */
            logic::linkStateMachine();           /* 0x0800755C */
            /* Repack the decoded telemetry into Benz payloads before the
             * link looks at them. Compiled out entirely in a jeep build, so
             * the stock 8 ms task keeps its exact shape. */
            if constexpr (cfg::kIsBenz) {
                logic::benzPublish();
            } else {
                /* The same job for the custom 0xB0-0xB8 block, and in the
                 * same place: repack before the link looks at the payloads.
                 * Compiled out entirely in a benz build, so each dialect's
                 * 8 ms task keeps exactly one publisher. */
                logic::customPublish();
            }
            svc::hu::service();                  /* 0x08008FE2 */
            svc::io::writeStatusLines();         /* 0x080084F6 */
        }

        if (svc::sched::takeSlow()) {            /* 1024 ms */
            svc::params::save();                 /* 0x08002D46 */
        }
    }
}

}  // namespace

extern "C" void main_loop(void)
{
    cpu_irq_disable();
    bootSequence();

    waitForBus();

    /* The bus answered. Latch the profile that won and bring CAN up on it for
     * good, then announce ourselves: state kBusHello makes the link state
     * machine emit the 0x416 frame once and drop to kBusRunning. */
    ram::app().profileActive = svc::params::profile();
    svc::params::validateDefaults();
    logic::primeTxTemplates();
    /* Deliberately NOT bracketed in cpu_irq_disable(), unlike the phase-1
     * re-init above: the stock image masks around can_set_baud_by_profile()
     * inside the loop and leaves can_apply_bitrate_B() unmasked here. Kept so
     * this matches 0x080088d8.
     *
     * The cost is small and known: this path pulses the CAN peripheral reset, so
     * a frame already pending in FIFO0 can make CAN_RX0 run against a resetting
     * block and push one bogus entry (id 0, DLC 0) into the RX ring. The decoder
     * discards it, and the only side effect is one spurious bus-alive reload. */
    svc::can::applyBitrateB();                   /* 0x08007B4C */
    ram::app().canBusStatus = cfg::kBusHello;
    ram::app().busPresentDebounce = cfg::kBusPresentDebounce;

    superLoop();
}
