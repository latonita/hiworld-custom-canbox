/* ============================================================================
 * raise_benz_decode.cpp -- see raise_benz_decode.hpp.
 *
 * Evidence for every identifier, byte, scale and sentinel below:
 * docs/JEEP-WK-CAN-ID-byte-map.md. The destination side -- why these units and
 * why these clamps -- is raise_benz_tx.hpp and docs/raise-benz-emulation.md.
 * ==========================================================================*/

#include "raise_benz_decode.hpp"

#include <cstdint>

#include "ram_map.hpp"

namespace logic {

/* ===========================================================================
 * BENZ-ONLY DECODES.
 *
 * These exist because the Mercedes emulation has screen real estate the
 * Hiworld protocol does not: odometer, ambient temperature, coolant
 * temperature and battery voltage all have Benz fields and no Hiworld field
 * whatsoever. Decoding them in a jeep build would be pure work thrown away,
 * and that build is supposed to stay equivalent to stock, so every one of
 * them is behind `if constexpr (cfg::kIsBenz)` at the call site.
 *
 * They write ram::Telemetry in physical units -- never ram::VehicleState,
 * whose byte offsets ARE the Hiworld wire format.
 *
 * THE SENTINEL RULE. The WK marks "unavailable" with all-ones on several of
 * these ids. The Benz plugin cannot blank a field: it prints whatever number
 * arrives and then holds it. Forwarding 0xFFFF would show 65535 rpm or
 * 6553.5 C. So an unavailable reading LEAVES THE PREVIOUS VALUE ALONE, which
 * is the same thing the plugin does by itself between frames.
 *
 * THIS TU IS COMPILED AND TYPE-CHECKED IN EVERY BUILD -- the build globs
 * app/logic/, so a jeep image compiles these decoders too. The calls are then
 * dropped by `if constexpr (cfg::kIsBenz)` and the code itself by
 * --gc-sections at link, which is the point: a mistake in a Benz decoder shows
 * up on every build rather than waiting for someone to pass --protocol benz.
 * ========================================================================= */

/* 0x002 d[2]:d[3] -- road speed, raw/128 km/h, all-FF = unavailable.
 * Evidence: JEEP-WK-CAN-ID-byte-map.md id 0x002, marked Confirmed. The /128
 * is integer division on purpose: the Benz 0x07 field is whole km/h. */
void benzSpeed(const volatile std::uint8_t* d) noexcept
{
    const std::uint16_t raw =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(d[2]) << 8) | d[3]);
    if (raw != 0xFFFFU) {
        ram::app().telemetry.speedKph = static_cast<std::uint16_t>(raw / 128U);
    }
}

/* 0x002 d[0]:d[1] -- rpm again, but as a NUMBER rather than two bytes.
 * jeepEngine() copies the pair verbatim into the Hiworld window because
 * 0xFFFF happens to be the radio's "blank this field" sentinel there. Benz
 * has no such sentinel, so here the sentinel is dropped instead of sent. */
void benzRpm(const volatile std::uint8_t* d) noexcept
{
    const std::uint16_t raw =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(d[0]) << 8) | d[1]);
    if (raw != 0xFFFFU) {
        ram::app().telemetry.rpm = raw;
    }
}

/* 0x014 -- b0:b1:b2 odometer at 0.1 km/count (0xFFFFFF unavailable),
 * b4 ambient temperature in whole degrees C.
 * Evidence: JEEP-WK-CAN-ID-byte-map.md id 0x014 -- odometer Confirmed,
 * ambient high confidence.
 *
 * Two unit conversions, both forced by the destination:
 *   - the Benz odometer field is whole KILOMETRES, the WK counts tenths, so
 *     the count is divided by 10 and the resolution is simply lost;
 *   - the Benz ambient field is 0.1 C but UNSIGNED, with no room for a
 *     negative. Sub-zero would wrap to ~6553 C, so it is clamped to 0. A WK
 *     in a Finnish winter will read 0.0 C on the Benz HVAC bar and there is
 *     no fix for that inside this protocol. */
void benzTrip(const volatile std::uint8_t* d) noexcept
{
    ram::Telemetry& t = ram::app().telemetry;

    const std::uint32_t odoRaw = (static_cast<std::uint32_t>(d[0]) << 16)
                               | (static_cast<std::uint32_t>(d[1]) << 8)
                               |  static_cast<std::uint32_t>(d[2]);
    if (odoRaw != 0x00FFFFFFU) {
        t.odometerKm = odoRaw / 10U;
    }

    /* Whole degrees, and the byte is the temperature directly for the
     * positive range the captures cover. Negative encoding is unconfirmed on
     * this car, and the destination cannot show it either way, so anything
     * that looks like a two's-complement negative is clamped to zero. */
    const std::uint8_t ambient = d[4];
    t.ambientDeciC = (ambient >= 0x80U)
                         ? 0U
                         : static_cast<std::uint16_t>(ambient * 10U);
}

/* 0x02C b1 -- coolant temperature, raw - 40 degrees C, all-FF unavailable.
 * Evidence: JEEP-WK-CAN-ID-byte-map.md id 0x02C, coolant high confidence,
 * scale probable (a cold start climbs 68 -> 132 raw = 28 -> 92 C).
 * The Benz field is unsigned whole degrees, so a below-minus-40 reading --
 * which this scale cannot represent anyway -- clamps to 0. */
void benzCoolant(const volatile std::uint8_t* d) noexcept
{
    const std::uint8_t raw = d[1];
    if (raw != 0xFFU) {
        ram::app().telemetry.coolantC =
            (raw >= 40U) ? static_cast<std::uint16_t>(raw - 40U) : 0U;
    }
}

/* 0x015 b1 -- battery voltage at 0.1 V/count. Evidence: id 0x015, Confirmed
 * (125 = 12.5 V key-on, 143-146 running, 103 cranking).
 *
 * Note the happy accident: the WK counts tenths of a volt and the Benz field
 * is also tenths of a volt, so this is a widening copy with no arithmetic.
 * 0x015 is already decoded for steering angle (d[4]:d[5]); this is a second
 * reader of the same frame, not a second case. */
void benzVoltage(const volatile std::uint8_t* d) noexcept
{
    ram::app().telemetry.deciVolts = d[1];
}

}  // namespace logic
