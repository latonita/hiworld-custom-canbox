/* ============================================================================
 * can_link.cpp -- 0x0800755C, the 8 ms vehicle-side task.
 *
 * Ported from reference/decompile/app/can.c (Ghidra output). The
 * comments are the reverse-engineering record; they describe the STOCK image.
 *
 * The CAN-error latch is bit 5 of the vehicle flags byte, named
 * ram::VehicleState::kFlagCanErrLatch. NOTE the deliberate stock asymmetry: the
 * hold is ARMED off bit 2 -- the same bit the decoders call reverse gear -- and
 * CLEARED here on bit 5. Do not collapse the two.
 * ==========================================================================*/

#include <cstddef>
#include <cstdint>

#include "app_config.hpp"
#include "ram_map.hpp"

#include "can_bus.hpp"
#include "power.hpp"
#include "soft_timer.hpp"

#include "can_decode.hpp"
#include "can_link.hpp"
#include "periodic_tx.hpp"

namespace logic {
namespace {

/* ===========================================================================
 * The SWC reply frame.
 *
 * ram::AppRam::swcTxBuf (0x20001148) is the box's node on the steering-wheel
 * control bus: 8 bytes, always sent as standard id 0x416. The decoder fills
 * bytes 0..1 with the key report; byte 2 is the LINK STATUS the state machine
 * below maintains (0x3F = we are up, 0xBF = the bus went away).
 * ========================================================================= */
constexpr std::uint32_t kIdSwcReply  = 0x416U;
constexpr std::uint8_t  kIdeStandard = 0U;
constexpr std::uint8_t  kDlcSwcReply = 8U;

constexpr std::size_t  kSwcByteKeyIndex = 1U;  /* the held-key index   */
constexpr std::size_t  kSwcByteStatus   = 2U;  /* link status          */
constexpr std::uint8_t kSwcStatusUp     = 0x3FU;
constexpr std::uint8_t kSwcStatusLost   = 0xBFU;

/* The one-shot "hello" payload: FE 16 3F FF FF FF FF FF. 0x16 is this box's
 * node address on the SWC bus; the trailing 0xFF are "no data". */
constexpr std::uint8_t kHelloHeader  = 0xFEU;
constexpr std::uint8_t kHelloNodeId  = 0x16U;
constexpr std::uint8_t kHelloFill    = 0xFFU;
constexpr std::size_t  kHelloFillFrom = 3U;    /* bytes 3..7 are the fill */

/* The repeat/release delay armed after an immediate reply: 40 counts of the
 * 8 ms misc-timer hook = 320 ms. The timer FLOORS AT 1 (see soft_timer.hpp), so
 * the "== 1" test below fires on exactly one pass and then latches. */
constexpr std::uint8_t kSwcRepeatDelay = 0x28U;

/* The value the UART/CAN decoder parks in miscTimers.swcRespMode to ask for an
 * immediate reply, and the latched value of miscTimers.swcFlag that asks for
 * the repeat. */
constexpr std::uint8_t kSwcReplyNow    = 1U;
constexpr std::uint8_t kSwcRepeatReady = 1U;

/* ===========================================================================
 * The CAN-error hold.
 *
 * ASYMMETRY, reproduced verbatim: the flag TESTED is bit 2 of the vehicle flags
 * byte (ram::VehicleState::kFlagReverse, which is what the decoders call it)
 * and the flag CLEARED once the hold expires is bit 5, which has no name. The
 * stock decompile is `if ((int)(status << 0x1D) < 0) hold = 5; else if
 * (hold == 0) status &= 0xDF;` -- a sign test on bit 2 and a mask clearing
 * bit 5. Do not "fix" this into one bit: it is what the image does.
 *
 * canErrHoldTimer (0x200011A0) is aged by svc::can::timersTick() on the 100 ms
 * hook, so 5 counts is half a second.
 * ========================================================================= */
constexpr std::uint8_t kCanErrHoldReload = 5U;

/* ===========================================================================
 * Bus-presence timers, both counted down by svc::can::timersTick() at 100 ms.
 * ========================================================================= */
constexpr std::uint8_t kSettleTicks = 10U;   /* 1 s to decide the bus is real */
constexpr std::uint8_t kLostTicks   = 100U;  /* 10 s of grace after it dies   */

/* --- step 2: the two SWC reply paths ------------------------------------- */
void serviceSwcReplies() noexcept
{
    ram::AppRam& r = ram::app();

    /* Immediate reply to a freshly decoded key, then arm the repeat. */
    if (r.miscTimers.swcRespMode == kSwcReplyNow) {
        r.miscTimers.swcRespMode = 0U;
        svc::can::txEnqueue(kIdSwcReply, kIdeStandard, kDlcSwcReply, r.swcTxBuf);
        r.miscTimers.swcFlag = kSwcRepeatDelay;
    }

    /* The repeat/release reply, 320 ms later. Releasing the soft-timer slot the
     * last scan reported BEFORE rescanning is what stops the key just
     * acknowledged from winning the scan again; the rescan then publishes
     * whichever key is still held (or the 0x16 "nothing" sentinel), and that
     * index is what byte 1 of the reply carries. */
    if (r.miscTimers.swcFlag == kSwcRepeatReady) {
        r.miscTimers.swcFlag = 0U;
        svc::timers::clearCurrent();                  /* 0x08007CE0 */
        svc::timers::scanActive();                    /* 0x08007D12 */
        r.swcTxBuf[kSwcByteKeyIndex] = r.swc.keydataRef;
        svc::can::txEnqueue(kIdSwcReply, kIdeStandard, kDlcSwcReply, r.swcTxBuf);
    }
}

/* --- step 4: the CAN-error hold ------------------------------------------ */
void serviceCanErrorHold() noexcept
{
    ram::AppRam&       r  = ram::app();
    ram::VehicleState& vs = r.vehicle;

    if (vs.testFlag(ram::VehicleState::kFlagReverse)) {
        r.canErrHoldTimer = kCanErrHoldReload;
    } else if (r.canErrHoldTimer == 0U) {
        vs.at(ram::VehicleState::kFlags) = static_cast<std::uint8_t>(
            vs.at(ram::VehicleState::kFlags)
            & static_cast<std::uint8_t>(~ram::VehicleState::kFlagCanErrLatch));
    } else {
        /* Hold still running: leave the latch alone. */
    }
}

/* --- step 5: the one-shot hello ------------------------------------------ */
void emitHello() noexcept
{
    ram::AppRam& r = ram::app();

    r.swcTxBuf[0]               = kHelloHeader;
    r.swcTxBuf[kSwcByteKeyIndex] = kHelloNodeId;
    r.swcTxBuf[kSwcByteStatus]  = kSwcStatusUp;
    for (std::size_t i = kHelloFillFrom; i < kDlcSwcReply; ++i) {
        r.swcTxBuf[i] = kHelloFill;
    }

    svc::can::txEnqueue(kIdSwcReply, kIdeStandard, kDlcSwcReply, r.swcTxBuf);
}

/* --- step 5: the five-state bus-presence machine -------------------------
 * The two "force a reset" exits are wwdg_init() followed by wwdg_force_reset():
 * re-arm the window watchdog and then write an out-of-window counter so it
 * bites immediately. A reset re-enters the IAP bootloader, which is why a box
 * that loses its bus is always still flashable. The stock decompile names those
 * two reinit_can_filters_and_state / wwdg_force_reset_thunk, both of which are
 * misnomers (0x08002DEC is the watchdog init).
 *
 * The bus-DEAD exit from kBusDown additionally runs the deep-sleep path FIRST
 * (0x080032F6, "exti8_bus_wake_setup" in the decompile): if CAN acknowledges
 * sleep that never returns, and if it does not, the watchdog pair below ends
 * the pass anyway. */
void serviceBusPresence() noexcept
{
    ram::AppRam& r = ram::app();

    switch (r.canBusStatus) {
    case cfg::kBusDown:
        if (svc::can::busActiveFlag() == 0U) {
            svc::power::shutdownDeepSleep();          /* 0x080032F6 */
            /* Only reached if CAN refused to sleep, so the box is awake with a
             * dead bus. Stock open-codes wwdg_init(); wwdg_force_reset() here
             * and records nothing; this records why first. See power.hpp. */
            svc::power::fatalErrorReset(svc::power::kFatalBusDown);
        } else {
            r.canBusStatus    = cfg::kBusSettling;
            r.canActivityCtr  = kSettleTicks;
        }
        break;

    case cfg::kBusSettling:
    case cfg::kBusLost:
        /* Note the test is `== 1`, not `!= 0`: busActiveFlag() returns the ACC
         * bit already masked, so the two are the same thing here. */
        if (svc::can::busActiveFlag() == 1U) {
            r.canBusStatus              = cfg::kBusRunning;
            r.swcTxBuf[kSwcByteStatus]  = kSwcStatusUp;
        } else if (r.canActivityCtr == 0U) {
            r.canBusStatus = cfg::kBusDown;
        } else {
            /* Still waiting for the debounce to run out. */
        }
        break;

    case cfg::kBusRunning:
        if (svc::can::busActiveFlag() == 0U) {
            r.canBusStatus             = cfg::kBusLost;
            r.swcTxBuf[kSwcByteStatus] = kSwcStatusLost;
            r.canActivityCtr           = kLostTicks;
        } else if (r.canActivityCtr == 0U) {
            /* Bus up but the activity counter has run out -- nothing is
             * reloading it, so the link is wedged. Force a reset. */
            svc::power::fatalErrorReset(svc::power::kFatalLinkWedged);
        } else {
            /* Normal running: nothing to do. */
        }
        break;

    case cfg::kBusHello:
        /* Entered exactly once, by main_loop, when phase 1 ends. */
        emitHello();
        r.canBusStatus = cfg::kBusRunning;
        break;

    default:
        /* Any value outside 0..4 -- which only a corrupted byte can produce --
         * re-enters the settling state rather than the probe. */
        r.canBusStatus = cfg::kBusSettling;
        break;
    }
}

}  // namespace

/* ===========================================================================
 * 0x0800755C -- can_autobaud_state_machine
 * ========================================================================= */
/* 0x0800755C */
void linkStateMachine() noexcept
{
    /* 1. Drain the RX ring through the active car profile's decoder. */
    dispatchGate();                                   /* 0x08007D44 */

    /* 2. Flush the steering-wheel-control replies. */
    serviceSwcReplies();

    /* 3. One phase of the outbound table. */
    periodicTxScheduler();                            /* 0x08008110 */

    /* 4. The CAN-error hold. */
    serviceCanErrorHold();

    /* 5. The bus-presence state machine. */
    serviceBusPresence();
}

}  // namespace logic
