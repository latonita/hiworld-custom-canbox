/* ============================================================================
 * can_bus.hpp -- the CAN service: bring-up, the two 50-entry rings and the
 * bus-presence counters. Namespace `svc::can`.
 *
 * Ported from reference/decompile/app/can.c (Ghidra output). The
 * StdPeriph-level register driver that used to sit underneath this has moved
 * to app/hal/hal_can.c; everything here is Hiworld's own plumbing on top of it
 * and is the ONLY code that touches the CAN ring state in ram::AppRam.
 *
 * This is the vehicle-side interface: PB8 = CAN_RX, PB9 = CAN_TX (both AF5),
 * PB12 = transceiver standby (owned by svc::io, called from here).
 *
 * Data flow:
 *     bus -> FIFO0 -> CAN_RX0_IRQHandler (IRQ 54) -> onRxInterrupt()
 *                  -> ram::AppRam::canRxRing -> rxAvailable/rxId/rxData/rxAdvance
 *                  -> logic::can_decode
 *     txEnqueue() -> ram::AppRam::canTxRing -> txDrain() -> TX mailboxes
 *
 * Both rings are 50 entries x 16 bytes (ram::kCanRingEntries / kCanRingStride)
 * and the four indices are the 32-bit words in ram::AppRam::canRing. The RX
 * head is written ONLY from the ISR and the TX tail is advanced ONLY from the
 * main loop, so no critical sections are needed -- and the stock code has none.
 *
 * Addresses in comments are the ORIGINAL flash addresses of the stock function
 * each entry point reproduces. They are how a future reader diffs this against
 * Ghidra; keep them.
 * ==========================================================================*/

#ifndef CAN_BUS_HPP
#define CAN_BUS_HPP

#include <cstdint>

namespace svc {
namespace can {

/* ===========================================================================
 * Bring-up
 *
 * BIT TIMING: BRP = 2000 / baud_kbps at 8 tq/bit with a 62.5 % sample point,
 * which is exact only at f_APB1 = 16 MHz. The 83.333 kbit/s case deliberately
 * truncates to 83 (2000 / 83 = 24 -> 16e6 / (24 * 8) = 83 1/3 kbit/s). The full
 * derivation is in the header comment of app/hal/hal_can.h; pass
 * cfg::kCanBaudJeepKbps / cfg::kCanBaudFcaKbps, never a bare literal.
 * ========================================================================= */

/* Full bring-up at `baudKbps`: NVIC (IRQ 54) -> APB2 clocks + PB8/PB9 as AF ->
 * CAN clock + reset pulse -> bit timing -> accept-all filter -> FMPIE0.
 * `mode` is a CAN_MODE_* value from hal_can.h. */
void init(int baudKbps, std::uint8_t mode) noexcept;        /* 0x08002A90 */

void initNormal(int baudKbps) noexcept;                     /* 0x08002B76 */

/* Present in the stock image but never called by live code. Kept so one stock
 * function has exactly one reconstruction. */
void initSilent(int baudKbps) noexcept;                     /* 0x08002B84 */

/* Re-baud + transceiver wake-up + bus-presence reset. The stock image carries
 * two byte-identical copies of this; both are kept because the two call sites
 * (setBaudByProfile / applyBitrateB) each reference a specific one. */
void reinitBitrateA(int baudKbps) noexcept;                 /* 0x08007482 */
void reinitBitrateB(int baudKbps) noexcept;                 /* 0x0800744E */

/* Baud is locked to the stored decode profile: profile 1 -> 125 kbit/s,
 * anything else is forced back to profile 0 -> 83.333 kbit/s. */
void setBaudByProfile() noexcept;                           /* 0x08007B2C */
void applyBitrateB() noexcept;                              /* 0x08007B4C */

/* ===========================================================================
 * TX ring
 * ========================================================================= */

/* Producer side. Drops the frame and sets ram::kCanErrTxRingFull when the ring
 * is full -- one slot is always left unused, which is how head == tail stays
 * unambiguously "empty".
 *
 * `data` is `const volatile` so the decoders can hand over a pointer straight
 * into the fixed-address RAM block (a ring entry, g_swc_tx_buf, ...) without a
 * cast at every call site. */
void txEnqueue(std::uint32_t canId, std::uint8_t ide, std::uint8_t dlc,
               const volatile std::uint8_t* data) noexcept; /* 0x080074C0 */

/* TX pump, run from the 4 ms scheduler slot. Drains at most 10 entries per
 * pass and bails out the moment all three mailboxes are busy. */
void txDrain() noexcept;                                    /* 0x08007698 */

/* Hand one ring entry to a free TX mailbox.
 *
 * POLARITY: returns TRUE when NO mailbox was free, i.e. true means "nothing was
 * sent, stop draining". That is the stock `can_transmit(...) == 4` test, and
 * txDrain() depends on it; do not invert it to the more natural "sent ok". */
[[nodiscard]] bool transmitEntry(const volatile std::uint8_t* entry) noexcept;
                                                            /* 0x08002B98 */

/* Drop everything queued. Used by the bus-down probe in the SWC service block:
 * txTail = txHead. */
void txFlush() noexcept;

/* ===========================================================================
 * RX ring
 * ========================================================================= */

/* The CAN_RX0 interrupt BODY: pop one FIFO0 message and push it into the RX
 * ring. The `extern "C" void CAN_RX0_IRQHandler(void)` veneer that calls this
 * lives in app/logic/interrupts.cpp (see docs/ARCHITECTURE.md section 5).
 *
 * Needs at least 2 free slots; otherwise the frame is still popped -- that is
 * what releases the FIFO and clears the FMP0 condition, so the interrupt would
 * re-enter forever if it were skipped -- but discarded, with
 * ram::kCanErrRxRingFull raised. */
void onRxInterrupt() noexcept;                              /* 0x08002C0A */

/* The reader the decoders use. Only the main loop advances the tail. */
[[nodiscard]] bool rxAvailable() noexcept;

/* id = (u16 at entry+0) << 16 | (u16 at entry+2) -- exactly the stock CONCAT22,
 * for both standard and extended frames. */
[[nodiscard]] std::uint32_t rxId() noexcept;

/* -> data[0] of the tail entry (entry + ram::kCanEntryData). */
[[nodiscard]] const volatile std::uint8_t* rxData() noexcept;

void rxAdvance() noexcept;   /* tail = (tail + 1) % 50 */

/* ===========================================================================
 * State / shutdown
 * ========================================================================= */

/* Bit 0 of the decoded vehicle-state block = "vehicle bus alive". */
[[nodiscard]] std::uint8_t busActiveFlag() noexcept;        /* 0x080074B6 */

/* The 100 ms hook: age the two CAN hold-off counters. */
void timersTick() noexcept;                                 /* 0x080077E8 */

/* Put the controller into sleep mode; true when it ACKNOWLEDGED.
 *
 * The decompile names this "can_rx0_msg_pending", which is simply wrong: it is
 * the entry gate of the shutdown sequence. svc::power::shutdownDeepSleep()
 * does NOTHING AT ALL when this returns false -- it does not re-arm PB8, does
 * not gate the watchdog clock and does not WFI -- so a controller that refuses
 * sleep ends in a watchdog reset rather than a box that never wakes up. */
[[nodiscard]] bool enterSleep() noexcept;                   /* 0x08002D1A */

}  // namespace can
}  // namespace svc

#endif /* CAN_BUS_HPP */
