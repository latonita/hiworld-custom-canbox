/* ============================================================================
 * raise_benz_decode.hpp -- the Benz-only CAN receive decoders.
 * Namespace `logic`.
 *
 * NOT stock decodes. The Mercedes emulation has screen real estate the Hiworld
 * protocol does not -- odometer, ambient temperature, coolant temperature and
 * battery voltage all have Benz fields and no Hiworld field whatsoever -- so
 * these five readers exist only to feed it.
 *
 * ---------------------------------------------------------------------------
 * WHERE THIS SITS
 *
 * Jeep WK CAN frames in, ram::Telemetry in PHYSICAL UNITS out. Its sibling
 * raise_benz_tx.hpp packs that telemetry into the Benz wire frames; nothing
 * here touches a wire format. can_decode.cpp owns the RX ring and the dispatch
 * switch and calls into here, every call site behind
 * `if constexpr (cfg::kIsBenz)`.
 *
 * Evidence for every identifier, byte and scale below is in
 * docs/JEEP-WK-CAN-ID-byte-map.md, quoted per function in the .cpp.
 * ==========================================================================*/

#ifndef RAISE_BENZ_DECODE_HPP
#define RAISE_BENZ_DECODE_HPP

#include <cstdint>

namespace logic {

/* 0x002 d[2]:d[3] -- road speed, raw/128 km/h. */
void benzSpeed(const volatile std::uint8_t* d) noexcept;

/* 0x002 d[0]:d[1] -- engine rpm, as a number rather than two wire bytes. */
void benzRpm(const volatile std::uint8_t* d) noexcept;

/* 0x014 d[0]:d[1]:d[2] -- odometer 0.1 km/count; d[4] -- ambient temperature. */
void benzTrip(const volatile std::uint8_t* d) noexcept;

/* 0x02C d[1] -- coolant temperature, raw - 40 degrees C. */
void benzCoolant(const volatile std::uint8_t* d) noexcept;

/* 0x015 d[1] -- battery voltage at 0.1 V/count. */
void benzVoltage(const volatile std::uint8_t* d) noexcept;

}  // namespace logic

#endif /* RAISE_BENZ_DECODE_HPP */
