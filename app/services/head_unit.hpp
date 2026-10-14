/* ============================================================================
 * head_unit.hpp -- USART2 and the head-unit "AA 55" framing.  namespace svc::hu
 *
 * Ported from reference/decompile/app/uart.c plus the usart2_init half of reference/decompile/app/misc.c
 * (Ghidra output), via the annotated C port in the retired app/port/hw_uart.c. Every
 * function carries the flash address of the stock routine it reproduces, so
 * this file can be diffed against Ghidra by anyone who comes after.
 *
 * The box talks to the Android head unit over USART2 (PA2 = TX, PA3 = RX) at
 * 38400 8N1. Framing, verified against captures, against the Android plugin's
 * own decoder and against the decompile (docs/notes/hiworld-frame-format.md,
 * docs/hiworld-firmware-analysis.md section 4):
 *
 *      AA 55 | LEN | CMD | payload[LEN] | CKS          wire length = LEN + 5
 *
 *   - LEN counts the PAYLOAD ONLY (no preamble, no CMD, no checksum).
 *   - CMD is at offset 3, i.e. AFTER the length byte. That is what separates
 *     Hiworld from the sibling "Raise" protocol (2E CMD LEN ...).
 *   - CKS = (sum of all bytes incl. the AA 55 preamble) & 0xFF, which the
 *     firmware computes in the algebraically identical form
 *          CKS = (LEN + CMD + sum(payload) - 1) & 0xFF
 *     because 0xAA + 0x55 = 0xFF == -1 (mod 256).
 *   - There is NO escaping/stuffing anywhere: a payload byte may legitimately
 *     be 0xAA or 0x55. Resynchronisation relies purely on the LEN/checksum
 *     check failing and the parser restarting at state 0.
 *   - LEN >= 0x38 is rejected outright.
 *   - Only the AA 55 dialect is accepted; 5A A5 is not.
 *
 * CADENCE. service() is called from the 8 ms slot of the main scheduler and
 * only emits on every 6th pass, i.e. ONE FRAME PER 48 ms while any payload is
 * changing. When a full round-robin sweep finds nothing changed it falls back
 * to a keep-alive: kIdleDelay = 15 passes of 48 ms = ~720 ms between frames.
 * The first transmission therefore happens ~48 ms into phase 2.
 *
 * The box free-runs: it never waits to be polled, and the capture order
 * 73 F0 12 32 72 A6 is just this module's table walk.
 * ==========================================================================*/

#ifndef HEAD_UNIT_HPP
#define HEAD_UNIT_HPP

#include <cstddef>
#include <cstdint>

namespace svc {
namespace hu {

/* ===========================================================================
 * Layout of the shared working buffer, ram::AppRam::uartFrame (0x200019F8,
 * 0x109 bytes). This is the old g_uart_frame_buf and the layout is FIXED --
 * the ISR and the parser agree on these offsets and nothing else describes
 * them, so they are named here rather than left as literals in the code.
 *
 *   [0x00 .. 0x3B]  outbound frame being clocked out by the ISR (60 bytes,
 *                   which is exactly LEN(0x37) + 5 rounded up)
 *   [0x3C]          tx length, in total wire bytes
 *   [0x3D]          tx cursor, advanced by the ISR
 *   [0x3E]          tx busy flag (1 = a frame is in flight)
 *   [0x3F .. 0x106] the 200-byte RX ring filled by the ISR
 *   [0x107]         RX ring head  (written by the ISR)
 *   [0x108]         RX ring tail  (written by the parser)
 * ========================================================================= */
inline constexpr std::size_t   kFbTxFrame   = 0x00U;
inline constexpr std::size_t   kFbTxLen     = 0x3CU;
inline constexpr std::size_t   kFbTxPos     = 0x3DU;
inline constexpr std::size_t   kFbTxBusy    = 0x3EU;
inline constexpr std::size_t   kFbRxRing    = 0x3FU;
inline constexpr std::size_t   kFbRxHead    = 0x107U;
inline constexpr std::size_t   kFbRxTail    = 0x108U;
inline constexpr std::uint32_t kRxRingSize  = 200U;

/* ===========================================================================
 * The ACK queue, ram::AppRam::uartAckQueue (0x20001B3D, 12 bytes): ten entries
 * plus a producer index at [0x0A] and a consumer index at [0x0B].
 *
 * DEAD ON THE WIRE IN THIS BUILD: nothing in the image ever advances the
 * producer index, so the queue is always empty and txAck() is never reached
 * from the scheduler. It is reproduced because it is stock behaviour and
 * because the 0xFF ACK is the only frame shape the box can be asked for.
 * ========================================================================= */
inline constexpr std::size_t kAckQueueEntries = 10U;
inline constexpr std::size_t kAckQueueHead    = 0x0AU;
inline constexpr std::size_t kAckQueueTail    = 0x0BU;

/* ===========================================================================
 * Descriptor-table geometry and scheduler constants.
 * ========================================================================= */
inline constexpr std::uint8_t kTxSlots       = 6U;   /* flash table 0x08009F2C */
inline constexpr std::uint8_t kRxSlots       = 2U;   /* flash table 0x08009F74 */
inline constexpr std::uint8_t kTicksPerFrame = 6U;   /* 6 x 8 ms  =  48 ms     */
inline constexpr std::uint8_t kIdleDelay     = 15U;  /* 15 x 48 ms = ~720 ms   */

/* Slot states held in ram::AppRam::uartSlotState[kTxSlots] (0x200011AB).
 * 2 and 3 are not distinguished anywhere; both mean "already built this
 * sweep", and the scheduler tests for them separately only because the stock
 * code does. */
inline constexpr std::uint8_t kSlotIdle     = 0U;
inline constexpr std::uint8_t kSlotRequest  = 1U;   /* explicit send-now       */
inline constexpr std::uint8_t kSlotSent     = 3U;

/* ===========================================================================
 * The module. Flash addresses are the stock routines these reproduce.
 * ========================================================================= */

/* 0x08008A54 -- uart_reset_state + usart2_init(38400) + publish the 17-byte
 * version string into ram::g_versionString, which the flashing tool reads
 * back over SWD to prove the image booted. */
void init() noexcept;

/* 0x08008A18 -- drop any frame in flight, empty both rings, park the slot
 * index on the "nothing to send" sentinel. */
void resetState() noexcept;

/* 0x08008FE2 -- the 8 ms task: parse at most one inbound frame, then run the
 * transmit scheduler. */
void service() noexcept;

/* 0x0800286E -- the USART2 interrupt BODY (RXNE into the ring, TXE out of the
 * frame buffer). The `extern "C" void USART2_IRQHandler(void)` wrapper that
 * crt0.S's veneer branches to lives in app/logic/interrupts.cpp, NOT here. */
void onInterrupt() noexcept;

/* 0x08003088 -- shut the port down; used by the deep-sleep path. */
void disable() noexcept;

/* 0x08008CCE -- the fixed 6-byte frame  AA 55 01 <cmd> <arg> <cks>, where
 * cks = cmd + arg (which is the general rule, since LEN = 1 and 1 - 1 = 0).
 * Only ever called as txAck(0xFF, code). */
void txAck(std::uint8_t cmd, std::uint8_t arg) noexcept;

}  // namespace hu
}  // namespace svc

#endif /* HEAD_UNIT_HPP */
