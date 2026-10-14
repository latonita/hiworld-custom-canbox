/* ============================================================================
 * can_link.hpp -- the 8 ms vehicle-side task. Namespace `logic`.
 *
 * Ported from reference/decompile/app/can.c (0x0800755C, "can_autobaud_state_machine" in the
 * decompile).
 *
 * THE NAME IN THE DECOMPILE IS MISLEADING. By the time this function runs the
 * autobaud is over -- that is main_loop's phase 1. What is left is the
 * bus-presence supervisor plus the SWC reply pump: the one task that drives
 * everything on the car side of the box once the superloop is running.
 * ==========================================================================*/

#ifndef CAN_LINK_HPP
#define CAN_LINK_HPP

namespace logic {

/* --- 0x0800755C can_autobaud_state_machine ---
 * main_loop's 8 ms slot. In order:
 *
 *   1. logic::dispatchGate() -- drain the RX ring through the active profile
 *      decoder.
 *   2. The two steering-wheel-control reply paths, both sending the 8-byte
 *      ram::AppRam::swcTxBuf as CAN id 0x416: the immediate reply to a key
 *      press, and the repeat/release reply 320 ms later.
 *   3. logic::periodicTxScheduler() -- one phase of the outbound table.
 *   4. The CAN-error hold: a flag bit re-arms a 5-tick hold, and once that has
 *      expired a DIFFERENT bit of the same byte is cleared. The asymmetry is in
 *      the image and is reproduced; see the implementation.
 *   5. The five-state bus-presence machine in ram::AppRam::canBusStatus, using
 *      the cfg::kBus* names:
 *
 *        kBusDown (0)     bus active  -> kBusSettling, arm 10 ticks
 *                         bus dead    -> deep sleep, then force a reset
 *        kBusSettling (1) bus active  -> kBusRunning, swcTxBuf[2] = 0x3F
 *        kBusLost (4)     timer out   -> kBusDown
 *        kBusRunning (2)  bus lost    -> kBusLost, swcTxBuf[2] = 0xBF, 100 ticks
 *                         timer out while still up -> force a reset
 *        kBusHello (3)    emit 0x416 FE 16 3F FF FF FF FF FF -> kBusRunning
 *        anything else    -> kBusSettling
 *
 *      kBusHello is entered exactly ONCE, by main_loop, the moment phase 1
 *      ends. The shared countdown is ram::AppRam::canActivityCtr, aged by
 *      svc::can::timersTick() on the 100 ms hook. */
void linkStateMachine() noexcept;

}  // namespace logic

#endif /* CAN_LINK_HPP */
