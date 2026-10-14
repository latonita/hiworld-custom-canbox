/* ============================================================================
 * periodic_tx.hpp -- the outbound half of the vehicle link: the 151-phase
 * periodic CAN transmit table and the head unit's cluster-text message.
 * Namespace `logic`.
 *
 * Ported from reference/decompile/app/can.c (0x08008110) and reference/decompile/app/uart.c (0x08007DA8),
 * Ghidra output,. Every entry point carries the
 * ORIGINAL flash address of the stock function it reproduces; that annotation
 * is how a future reader diffs this file against the decompile.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS UNIT IS FOR
 *
 * Six fixed-address frame buffers in ram::AppRam (txFrameAmp, txFrameF0,
 * txFrame326, txFrame190, txFrame18c, txFrame9f) hold what this box tells the
 * CAR. Nothing transmits them directly: the decoders and the head-unit command
 * path WRITE those buffers, and the phase table here is the single place that
 * hands them to svc::can::txEnqueue().
 *
 * Which CAN identifier each buffer goes out as depends on the latched profile
 * (ram::AppRam::profileActive) -- the payloads are shared between the two cars,
 * the identifiers are not. Profile 1 has no 0x09F equivalent at all.
 * ==========================================================================*/

#ifndef PERIODIC_TX_HPP
#define PERIODIC_TX_HPP

namespace logic {

/* --- 0x08008110 can_periodic_tx_scheduler ---
 * Reached once per 8 ms from logic::linkStateMachine(). Four parts:
 *
 *   1. ACC edge detect. A shadow copy of the ACC bit lives in the vehicle
 *      block; when it changes, txFrame9f[0] is primed with 1 (ACC on) or
 *      7 (ACC off).
 *   2. The post-ACC hold timer. Pinned at 400 while ACC is on, counted down one
 *      per call once it drops, so the box keeps transmitting for 400 * 8 ms =
 *      3.2 s after ACC goes away and then goes quiet. Everything below only
 *      runs while that timer is non-zero.
 *   3. The phase table: one step per call through 151 phases, one frame emitted
 *      on each odd phase 1..0x0B. 151 * 8 ms = ~1.2 s per lap.
 *   4. The two pending head-unit commands -- the 0xD2 cluster text (which calls
 *      clusterTextToCan() below) and the 0xAD tone/volume change, which is
 *      clamped, mirrored into the amplifier frame, the vehicle block and the
 *      parameter page, and transmitted immediately rather than waiting for its
 *      phase to come round. */
/* Rebuild every outbound CAN frame template for the latched profile. Must be
 * called right after svc::params::validateDefaults(), which latches it. Split
 * out of that function, where the stock image kept it -- see the note on the
 * definition. */
void primeTxTemplates() noexcept;                 /* 0x08007B6C part */

void periodicTxScheduler() noexcept;

/* --- 0x08007DA8 uart_D2_cluster_text_to_can ---
 * CAN-TX side of the head unit's 0xD2 "cluster text" message. The parsed
 * command sits in ram::AppRam::hostCmd.cluster: byte 0 is the message class and
 * bytes 1..8 are ASCII digits. The result is folded into three of the outbound
 * frame buffers -- txFrame190 (class + sub-code), txFrame18c and txFrame9f
 * (the 16-bit numeric value) -- which the phase table then transmits.
 *
 * Driven from periodicTxScheduler() when hostCmd.clusterNewData goes to 1; it
 * is public because that flag/consumer pairing is part of the documented
 * contract and the unit tests drive it directly. */
void clusterTextToCan() noexcept;

}  // namespace logic

#endif /* PERIODIC_TX_HPP */
