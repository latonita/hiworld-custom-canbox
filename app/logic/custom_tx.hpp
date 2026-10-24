/* ============================================================================
 * custom_tx.hpp -- the custom 0xB0-0xBD telemetry block: decoded WK signals
 * in, Hiworld payloads out. Namespace `logic`.
 *
 * The contract is docs/CUSTOM-TX-SPEC.md. This header is the summary a reader
 * needs to use the module; the spec is the summary they need to CHANGE it.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IS FOR
 *
 * The stock Hiworld protocol carries doors, gear, rpm, reverse, ACC,
 * illumination and steering, and nothing else. The Jeep WK body bus carries
 * coolant, intake air, outside air, battery volts, throttle, fuel level, road
 * speed and the parking brake as well. None of those has a stock field, so a
 * jeep build adds eight commands of its own in the free 0xB0-0xBF range,
 * and the WK compass and odometer go out the same way.
 *
 * This is a DIVERGENCE from stock, like the Benz emulation and unlike the rest
 * of app/logic/. The six stock TX slots are untouched and still byte-for-byte
 * what 0x08009F2C describes; these eight are additional.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS BUILT, AND WHAT IS DELIBERATELY NOT
 *
 *   0xB0 HEARTBEAT   2 B   protocol revision + validity mask
 *   0xB1 VERSION     --    NOT built: stock 0xF0 already carries it
 *   0xB2 IGNITION    1 B   ACC, engine running, reverse, parking brake
 *   0xB3 DRIVETRAIN  6 B   speed, rpm, engine load, throttle
 *   0xB4 ENGINE      5 B   coolant, intake air, fuel %, volts
 *   0xB5 CLIMATE     2 B   outside air, signed decidegrees
 *   0xB6 BODY        --    NOT built: stock 0x12 already carries the doors
 *   0xB7 TYRE        5 B   ONE tyre: position, kPa, temp, flags (polled)
 *   0xB8 FUEL        4 B   litres, range
 *   0xB9 KEY         --    NOT built: no verified source for SWC on this car
 *   0xBB ODOMETER    3 B   24-bit distance at 0.1 km/count
 *   0xBD COMPASS     3 B   8-way sector + magnetic heading
 *   0xBC FLOW        2 B   fuel flow, u16 0.1 L/h, 1 s average, 0xFFFF = n/a
 *
 * The four that are not built stay RESERVED, and so do the GAPS at 0xBA and
 * 0xBE (0xBC, once reserved for GEAR and struck out, is now FLOW): ../canbus-to-canbox-signal-map.md section 9 has already allocated
 * them to LIGHTING, BODY2 and a spare. 0xBB and 0xBD are that allocation, not
 * free choices -- do not compact the numbering. Those three groups have
 * confirmed sources but no consumer yet, so they are not sent.
 *
 * Never emit 0xE0 (the head unit's own reboot trigger), 0xFF/0xFE (ACK/NAK) or
 * 0xD2 (cluster text) from this range -- hu_hiworld.cpp static_asserts that.
 *
 * ---------------------------------------------------------------------------
 * UNITS AND SENTINELS
 *
 * Multi-byte values are BIG-ENDIAN. Temperatures are one byte biased +40.
 * Every field has an explicit "no value" encoding and holds it until the car
 * has actually reported that signal -- the sentinel block in ram_map.hpp says
 * which, and why outside air is the one field that cannot use all-ones.
 *
 * A consumer therefore has two independent ways to know a number is not real:
 * the field's own sentinel, and the 0xB0 validity mask. Both are published
 * honestly; neither is a substitute for the other.
 *
 * ---------------------------------------------------------------------------
 * KNOWN OPEN ISSUE, NOT THIS MODULE'S TO FIX
 *
 * A consumer that ignores 0xB0 and has no receive timeout will hold the last
 * good value for ever. Every sentinel here is outside the range such a
 * consumer accepts, so it will reject rather than display -- but it will keep
 * showing the stale number. That half is consumer-side work.
 * ==========================================================================*/

#ifndef CUSTOM_TX_HPP
#define CUSTOM_TX_HPP

#include <cstdint>

namespace logic {

/* The command bytes, so the heartbeat can name its own slot without the
 * logic layer reaching into the services layer's descriptor table. */
inline constexpr std::uint8_t kCmdHeartbeat  = 0xB0U;
inline constexpr std::uint8_t kCmdIgnition   = 0xB2U;
inline constexpr std::uint8_t kCmdDrivetrain = 0xB3U;
inline constexpr std::uint8_t kCmdEngine     = 0xB4U;
inline constexpr std::uint8_t kCmdClimate    = 0xB5U;
inline constexpr std::uint8_t kCmdTyre       = 0xB7U;
inline constexpr std::uint8_t kCmdFuel       = 0xB8U;
inline constexpr std::uint8_t kCmdOdometer   = 0xBBU;
inline constexpr std::uint8_t kCmdCompass    = 0xBDU;
inline constexpr std::uint8_t kCmdFlow       = 0xBCU;

/* Put the signals and the payloads into their "no value" state. Called once
 * from the boot sequence, BEFORE the first transmit -- a zeroed payload is a
 * set of plausible-looking readings, not a blank. */
void customPrimeFrames() noexcept;

/* Repack ram::CustomSignals into the eight payloads and, once a second, ask the
 * head-unit scheduler to send the heartbeat. Called from the 8 ms task,
 * alongside the Benz publisher it is modelled on. */
void customPublish() noexcept;

}  // namespace logic

#endif /* CUSTOM_TX_HPP */
