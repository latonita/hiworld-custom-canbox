/* ============================================================================
 * custom_tx.hpp -- the custom telemetry block, layout v2: decoded WK signals
 * in, Hiworld payloads out. Namespace `logic`.
 *
 * The contract is docs/TELEMETRY-V2-SPEC.md (section 14 is the wire summary).
 * This header is the summary a reader needs to use the module.
 *
 *   0xB0 HEARTBEAT  4 B  request lane, 1 Hz     version 02, capability 01BD, counter
 *   0xB2 STATE      7 B  priority, on change at the next 100 ms tick + every 1 s
 *   0xB3 FAST      11 B  priority, every 100 ms, always (it is the liveness frame)
 *   0xB4 ENGINE     5 B  slow lane              coolant, volts, fuel %, fuel litres
 *   0xB5 AMBIENT    3 B  slow lane              outside air, minutes since RUN
 *   0xB7 TYRE       5 B  slow lane              one corner (docs/TPMS-POLL-SPEC.md)
 *   0xB8 TRIP       6 B  slow lane              odometer, compass sector + heading
 *
 * 0xB1, 0xB6 and 0xB9-0xBF are never emitted. Never emit 0xE0 (the head
 * unit's reboot trigger), 0xFF/0xFE (ACK/NAK) or 0xD2 (cluster text) --
 * hu_hiworld.cpp static_asserts that.
 *
 * Multi-byte values are BIG-ENDIAN. Every field has its own sentinel; a STATE
 * bit is meaningful only while its source-alive bit in S6 is set.
 * ==========================================================================*/

#ifndef CUSTOM_TX_HPP
#define CUSTOM_TX_HPP

#include <cstdint>

namespace logic {

inline constexpr std::uint8_t kCmdHeartbeat = 0xB0U;
inline constexpr std::uint8_t kCmdState     = 0xB2U;
inline constexpr std::uint8_t kCmdFast      = 0xB3U;
inline constexpr std::uint8_t kCmdEngine    = 0xB4U;
inline constexpr std::uint8_t kCmdAmbient   = 0xB5U;
inline constexpr std::uint8_t kCmdTyre      = 0xB7U;
inline constexpr std::uint8_t kCmdTrip      = 0xB8U;

/* 0xB0 bytes 1:2: bit n set = this build sends command 0xB0 + n. */
inline constexpr std::uint16_t kCustomCapability =
    static_cast<std::uint16_t>((1U << 0) | (1U << 2) | (1U << 3) | (1U << 4)
                               | (1U << 5) | (1U << 7) | (1U << 8));
static_assert(kCustomCapability == 0x01BDU, "capability bitmap");

/* Put the signals and the payloads into their "no value" state. Called once
 * from the boot sequence, BEFORE the first transmit. Queues nothing. */
void customPrimeFrames() noexcept;

/* The 8 ms entry point: runs customTick100() on a drift-free 100 ms divider
 * of the 1 kHz counter (next += 100), and customTick1000() on every tenth
 * tick. Called from the 8 ms task right before svc::hu::service(). */
void customPublish() noexcept;

/* The 100 ms producer: age the sources, expire the indicator timers, close
 * the fuel-flow window, pack 0xB3 and 0xB2, queue 0xB3 always and 0xB2 when it
 * changed or 1 s has passed. Public for the host tests. */
void customTick100() noexcept;

/* The 1 s producer: pack the slow payloads (0xB4 0xB5 0xB7 0xB8) and the
 * heartbeat, and request the heartbeat. Public for the host tests. */
void customTick1000() noexcept;

}  // namespace logic

#endif /* CUSTOM_TX_HPP */
