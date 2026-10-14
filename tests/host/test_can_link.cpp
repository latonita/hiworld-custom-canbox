/* ============================================================================
 * test_can_link.cpp -- logic::linkStateMachine, the 8 ms vehicle-side task.
 *
 * Written from the BEHAVIOURAL SPEC in reference/decompile/app/can.c (the Ghidra decompile of
 * can_autobaud_state_machine @0x0800755C), not from can_link.cpp. The state
 * numbers, the reload values, the 0x416 payload and the two status bytes below
 * are restated here from the decompile rather than included from the module.
 *
 * WHAT BREAKS IF THIS UNIT IS WRONG
 * This is the only thing that drives the car side of the box once the superloop
 * is running: it runs the decoder, answers steering-wheel keys, steps the
 * outbound table and decides whether the vehicle bus is still there. A wrong
 * transition either makes the box reset in a loop or leaves it transmitting
 * into a bus that is no longer powered.
 *
 * NOT COVERED, deliberately: the two "force a watchdog reset" exits (kBusDown
 * with the bus dead, and kBusRunning with the activity counter expired). Both
 * end in wwdg_force_reset(), which is declared noreturn, so on the host the
 * compiler emits no return path after the call and the process would run off
 * the end of the function. They are reachable only from states this test can
 * simply not set up.
 * ==========================================================================*/

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>

#include "app_config.hpp"
#include "can_bus.hpp"
#include "can_link.hpp"
#include "fake_hal.hpp"
#include "ram_map.hpp"

namespace {

/* ---- the spec, restated ------------------------------------------------- */

/* The SWC reply is always `can_tx_enqueue(0x416, 0, 8, p_swc_tx_buf)`. */
constexpr std::uint32_t kIdSwcReply  = 0x416U;
constexpr std::uint8_t  kDlcSwcReply = 8U;

/* `p_swc_tx_buf[2] = 0x3f` on the way up, `= 0xbf` on the way down. */
constexpr std::uint8_t kStatusUp   = 0x3FU;
constexpr std::uint8_t kStatusLost = 0xBFU;

/* The one-shot hello payload the stock case 3 writes byte by byte. */
constexpr std::uint8_t kHello[8] = {0xFEU, 0x16U, 0x3FU, 0xFFU,
                                    0xFFU, 0xFFU, 0xFFU, 0xFFU};

/* `*p_swc_flag_118b = '('` -- 0x28 = 40 counts of the 8 ms misc-timer hook. */
constexpr std::uint8_t kSwcRepeatDelay = 0x28U;

/* `*p_profile0_activity_ctr = '\n'` / `= 'd'` -- 10 and 100 counts of the
 * 100 ms CAN timer hook. */
constexpr std::uint8_t kSettleTicks = 10U;
constexpr std::uint8_t kLostTicks   = 100U;

/* `*p_canerr_hold_timer = '\x05'`, and the bit that is cleared once it has
 * expired is bit 5 (`status &= 0xdf`) -- NOT the bit 2 that is tested. */
constexpr std::uint8_t kCanErrHoldReload = 5U;
constexpr std::uint8_t kCanErrLatchBit   = 0x20U;
constexpr std::uint8_t kCanErrTestBit    = 0x04U;   /* VehicleState::kFlagReverse */

/* dispatchGate() reloads this to 800 on every pass with ACC off. Used here only
 * as proof that the decoder really was called. */
constexpr std::uint32_t kBusPresentDebounce = 800U;

/* ---- helpers ------------------------------------------------------------ */

ram::AppRam& R() noexcept { return ram::app(); }

void setAcc(bool on) noexcept
{
    R().vehicle.setFlag(ram::VehicleState::kFlagAccOn, on);
}

/* Park the periodic table on a phase that transmits nothing, so the only
 * frames a test sees are the ones the link machine itself sent. The periodic
 * scheduler also returns immediately whenever ACC has been off long enough for
 * the hold timer to reach 0, which is the usual case here. */
void quietPeriodicTable() noexcept
{
    R().periodicTxPhase = 0x40U;
    R().vehicle.at(0x12U) = R().vehicle.accOn() ? 1U : 0U;  /* settle the edge */
}

/* Put the machine somewhere the bus-presence step will not try to reset. */
void parkRunning() noexcept
{
    R().canBusStatus   = cfg::kBusRunning;
    R().canActivityCtr = 50U;        /* non-zero: no "wedged link" reset */
    setAcc(true);                    /* bus active: stay in kBusRunning  */
}

void step() noexcept
{
    logic::linkStateMachine();
    svc::can::txDrain();
}

std::size_t countId(std::uint32_t id) noexcept
{
    std::size_t n = 0U;
    for (std::size_t i = 0U; i < fake::canFrameCount(); ++i) {
        if (fake::canFrames()[i].id == id) { ++n; }
    }
    return n;
}

}  // namespace

/* ---------------------------------------------------------------------------
 * Step 1 -- the decoder runs first.
 * ------------------------------------------------------------------------- */

TEST(link_runs_the_can_decoder)
{
    fake::reset();
    setAcc(false);
    R().canBusStatus   = cfg::kBusRunning;
    R().canActivityCtr = 50U;
    R().busPresentDebounce = 0U;

    logic::linkStateMachine();

    /* dispatchGate() reloads the bus-present debounce whenever ACC is off.
     * Nothing else in this task touches that word, so seeing 800 in it is
     * proof the decoder was entered. */
    CHECK_EQ(R().busPresentDebounce, kBusPresentDebounce);
}

TEST(link_drains_the_can_rx_ring)
{
    fake::reset();
    setAcc(false);
    R().canBusStatus   = cfg::kBusRunning;
    R().canActivityCtr = 50U;

    /* One frame waiting, with an identifier neither profile decodes. */
    R().canRxRing[ram::kCanEntryIdLo] = 0x77U;   /* id 0x0777 */
    R().canRxRing[ram::kCanEntryIdLo + 1U] = 0x07U;
    R().canRxRing[ram::kCanEntryDlc]  = 8U;
    R().canRing.rxHead = 1U;
    R().canRing.rxTail = 0U;

    logic::linkStateMachine();

    CHECK_EQ(R().canRing.rxTail, 1U);
}

/* ---------------------------------------------------------------------------
 * Step 2 -- the two SWC reply paths.
 * ------------------------------------------------------------------------- */

TEST(link_immediate_swc_reply_sends_0x416_and_arms_the_repeat)
{
    fake::reset();
    parkRunning();
    quietPeriodicTable();

    for (std::size_t i = 0U; i < 8U; ++i) {
        R().swcTxBuf[i] = static_cast<std::uint8_t>(0x90U + i);
    }
    R().miscTimers.swcRespMode = 1U;

    step();

    CHECK_EQ(R().miscTimers.swcRespMode, 0U);
    CHECK_EQ(R().miscTimers.swcFlag, kSwcRepeatDelay);

    CHECK_EQ(countId(kIdSwcReply), 1U);
    const fake::CanFrame* f = fake::lastCanFrame();
    CHECK(f != nullptr);
    if (f != nullptr) {
        CHECK_EQ(f->dlc, kDlcSwcReply);
        CHECK_EQ(f->ide, 0U);
        for (std::size_t i = 0U; i < 8U; ++i) {
            CHECK_EQ(f->data[i], 0x90U + i);
        }
    }
}

TEST(link_repeat_swc_reply_releases_the_old_slot_and_reports_the_next_key)
{
    fake::reset();
    parkRunning();
    quietPeriodicTable();

    /* The scan last reported slot 5; slot 0x20 is also still running. Clearing
     * the current slot first is what stops 5 from winning again, and the scan
     * starts at 0x17, so 0x20 is what should be published. */
    R().swc.keydataRef    = 5U;
    R().softTimers[5]     = 100;
    R().softTimers[0x20]  = 100;
    R().miscTimers.swcFlag = 1U;

    step();

    CHECK_EQ(R().miscTimers.swcFlag, 0U);
    CHECK_EQ(R().softTimers[5], 0);           /* clearCurrent() ran */
    CHECK_EQ(R().swc.keydataRef, 0x20U);      /* scanActive() ran   */
    CHECK_EQ(R().swcTxBuf[1], 0x20U);         /* and byte 1 carries it */

    CHECK_EQ(countId(kIdSwcReply), 1U);
}

TEST(link_sends_no_swc_reply_when_neither_path_is_armed)
{
    fake::reset();
    parkRunning();
    quietPeriodicTable();

    R().miscTimers.swcRespMode = 0U;
    R().miscTimers.swcFlag     = 0U;

    step();

    CHECK_EQ(countId(kIdSwcReply), 0U);
}

TEST(link_repeat_path_waits_for_the_timer_to_reach_exactly_one)
{
    fake::reset();
    parkRunning();
    quietPeriodicTable();

    /* The timer floors at 1 in the 10 ms hook, so only the value 1 fires the
     * repeat; 2 and above are still counting. */
    R().miscTimers.swcFlag = 2U;

    step();

    CHECK_EQ(R().miscTimers.swcFlag, 2U);
    CHECK_EQ(countId(kIdSwcReply), 0U);
}

/* ---------------------------------------------------------------------------
 * Step 3 -- the periodic table is driven from here.
 * ------------------------------------------------------------------------- */

TEST(link_steps_the_periodic_tx_table)
{
    fake::reset();
    parkRunning();
    R().vehicle.at(0x12U) = 1U;     /* ACC edge already settled */
    R().profileActive   = 0U;
    R().periodicTxPhase = 0U;

    step();

    /* One phase per call, and phase 1 is the amplifier frame (0x3D0, DLC 7). */
    CHECK_EQ(R().periodicTxPhase, 1U);
    CHECK_EQ(countId(0x3D0U), 1U);
}

/* ---------------------------------------------------------------------------
 * Step 4 -- the CAN-error hold.
 *
 * The bit TESTED is bit 2 and the bit CLEARED is bit 5. That asymmetry is in
 * the image; these two cases pin it down so a future "tidy-up" cannot quietly
 * collapse the two into one bit.
 * ------------------------------------------------------------------------- */

TEST(link_canerr_hold_is_rearmed_while_bit_2_is_set)
{
    fake::reset();
    parkRunning();
    quietPeriodicTable();

    R().vehicle.at(ram::VehicleState::kFlags) =
        static_cast<std::uint8_t>(R().vehicle.at(ram::VehicleState::kFlags)
                                  | kCanErrTestBit | kCanErrLatchBit);
    R().canErrHoldTimer = 0U;

    step();

    CHECK_EQ(R().canErrHoldTimer, kCanErrHoldReload);
    /* bit 5 is NOT cleared while the hold is being re-armed */
    CHECK_EQ(R().vehicle.at(ram::VehicleState::kFlags) & kCanErrLatchBit,
             kCanErrLatchBit);
}

TEST(link_canerr_latch_bit_5_is_cleared_once_the_hold_expires)
{
    fake::reset();
    parkRunning();
    quietPeriodicTable();

    R().vehicle.setFlag(kCanErrTestBit, false);
    R().vehicle.at(ram::VehicleState::kFlags) =
        static_cast<std::uint8_t>(R().vehicle.at(ram::VehicleState::kFlags)
                                  | kCanErrLatchBit);
    R().canErrHoldTimer = 0U;

    step();

    CHECK_EQ(R().vehicle.at(ram::VehicleState::kFlags) & kCanErrLatchBit, 0U);
}

TEST(link_canerr_latch_survives_while_the_hold_is_still_running)
{
    fake::reset();
    parkRunning();
    quietPeriodicTable();

    R().vehicle.setFlag(kCanErrTestBit, false);
    R().vehicle.at(ram::VehicleState::kFlags) =
        static_cast<std::uint8_t>(R().vehicle.at(ram::VehicleState::kFlags)
                                  | kCanErrLatchBit);
    R().canErrHoldTimer = 3U;

    step();

    CHECK_EQ(R().vehicle.at(ram::VehicleState::kFlags) & kCanErrLatchBit,
             kCanErrLatchBit);
    /* and the hold is not touched here -- it is aged by the 100 ms CAN hook */
    CHECK_EQ(R().canErrHoldTimer, 3U);
}

/* ---------------------------------------------------------------------------
 * Step 5 -- the five-state bus-presence machine.
 * ------------------------------------------------------------------------- */

TEST(link_hello_emits_the_0x416_greeting_once_and_drops_to_running)
{
    fake::reset();
    setAcc(false);                       /* keeps the periodic table silent */
    R().canBusStatus = cfg::kBusHello;

    step();

    CHECK_EQ(R().canBusStatus, cfg::kBusRunning);
    CHECK_EQ(fake::canFrameCount(), 1U);

    const fake::CanFrame* f = fake::lastCanFrame();
    CHECK(f != nullptr);
    if (f != nullptr) {
        CHECK_EQ(f->id, kIdSwcReply);
        CHECK_EQ(f->ide, 0U);
        CHECK_EQ(f->dlc, kDlcSwcReply);
        CHECK_BYTES(f->data, kHello, 8U);
    }

    /* The buffer itself keeps the greeting, which is what the next reply
     * builds on. */
    CHECK_BYTES(R().swcTxBuf, kHello, 8U);

    /* Entered ONCE: a second pass must not greet again. ACC is off, so the
     * machine now walks kBusRunning -> kBusLost without sending anything. */
    step();
    CHECK_EQ(fake::canFrameCount(), 1U);
    CHECK_EQ(R().canBusStatus, cfg::kBusLost);
}

TEST(link_down_state_moves_to_settling_when_the_bus_is_alive)
{
    fake::reset();
    setAcc(true);
    quietPeriodicTable();
    R().canBusStatus   = cfg::kBusDown;
    R().canActivityCtr = 0U;

    step();

    CHECK_EQ(R().canBusStatus, cfg::kBusSettling);
    CHECK_EQ(R().canActivityCtr, kSettleTicks);
}

TEST(link_settling_moves_to_running_and_publishes_the_up_status_byte)
{
    fake::reset();
    setAcc(true);
    quietPeriodicTable();
    R().canBusStatus   = cfg::kBusSettling;
    R().canActivityCtr = kSettleTicks;
    R().swcTxBuf[2]    = 0x00U;

    step();

    CHECK_EQ(R().canBusStatus, cfg::kBusRunning);
    CHECK_EQ(R().swcTxBuf[2], kStatusUp);
}

TEST(link_settling_falls_back_to_down_only_when_the_debounce_has_expired)
{
    fake::reset();
    setAcc(false);
    R().canBusStatus   = cfg::kBusSettling;
    R().canActivityCtr = 3U;

    step();
    CHECK_EQ(R().canBusStatus, cfg::kBusSettling);   /* still counting */
    CHECK_EQ(R().canActivityCtr, 3U);                /* aged elsewhere  */

    R().canActivityCtr = 0U;
    step();
    CHECK_EQ(R().canBusStatus, cfg::kBusDown);
}

TEST(link_lost_state_behaves_exactly_like_settling)
{
    fake::reset();
    setAcc(true);
    quietPeriodicTable();
    R().canBusStatus   = cfg::kBusLost;
    R().canActivityCtr = kLostTicks;
    R().swcTxBuf[2]    = 0x00U;

    step();

    CHECK_EQ(R().canBusStatus, cfg::kBusRunning);
    CHECK_EQ(R().swcTxBuf[2], kStatusUp);

    fake::reset();
    setAcc(false);
    R().canBusStatus   = cfg::kBusLost;
    R().canActivityCtr = 0U;

    step();
    CHECK_EQ(R().canBusStatus, cfg::kBusDown);
}

TEST(link_running_moves_to_lost_when_acc_goes_away)
{
    fake::reset();
    setAcc(false);
    R().canBusStatus   = cfg::kBusRunning;
    R().canActivityCtr = 50U;
    R().swcTxBuf[2]    = kStatusUp;

    step();

    CHECK_EQ(R().canBusStatus, cfg::kBusLost);
    CHECK_EQ(R().swcTxBuf[2], kStatusLost);
    CHECK_EQ(R().canActivityCtr, kLostTicks);
}

TEST(link_running_stays_put_while_the_bus_is_alive)
{
    fake::reset();
    setAcc(true);
    quietPeriodicTable();
    R().canBusStatus   = cfg::kBusRunning;
    R().canActivityCtr = 50U;

    step();

    CHECK_EQ(R().canBusStatus, cfg::kBusRunning);
    CHECK_EQ(R().canActivityCtr, 50U);
}

TEST(link_an_out_of_range_state_byte_restarts_at_settling)
{
    const std::uint8_t bogus[] = {5U, 6U, 0x7FU, 0xFFU};

    for (std::size_t i = 0U; i < sizeof bogus; ++i) {
        fake::reset();
        setAcc(false);
        R().canBusStatus = bogus[i];

        step();

        CHECK_EQ(R().canBusStatus, cfg::kBusSettling);
    }
}

/* ---------------------------------------------------------------------------
 * The whole pass, in order.
 * ------------------------------------------------------------------------- */

TEST(link_full_pass_decodes_replies_and_steps_the_table_in_one_call)
{
    fake::reset();
    parkRunning();
    R().vehicle.at(0x12U) = 1U;
    R().profileActive   = 0U;
    R().periodicTxPhase = 0U;
    R().busPresentDebounce = 7U;
    R().miscTimers.swcRespMode = 1U;

    step();

    /* decoder ran (ACC is on here, so the debounce is counted DOWN) ... */
    CHECK_EQ(R().busPresentDebounce, 6U);
    /* ... the SWC reply went out ... */
    CHECK_EQ(countId(kIdSwcReply), 1U);
    /* ... the table stepped and sent phase 1 ... */
    CHECK_EQ(R().periodicTxPhase, 1U);
    CHECK_EQ(countId(0x3D0U), 1U);
    /* ... and the bus state survived. */
    CHECK_EQ(R().canBusStatus, cfg::kBusRunning);
}
