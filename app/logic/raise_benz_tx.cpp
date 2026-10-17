/* ============================================================================
 * raise_benz_tx.cpp -- see raise_benz_tx.hpp.
 *
 * Spec: ../richest-raise-benz.md, confirmed against the decompiled plugin
 * my-radio-files/CanBus/plugin/1/34/P1_301.apk. WK signal sources and their
 * confidence levels: docs/JEEP-WK-CAN-ID-byte-map.md.
 * ==========================================================================*/

#include "raise_benz_tx.hpp"

#include "app_config.hpp"
#include "ram_map.hpp"

namespace logic {
namespace {

/* The 0x41 page selectors, payload[0] of each page. The head unit dispatches
 * on these and ignores a frame whose selector it does not know, so they are
 * protocol constants, not arbitrary tags. */
constexpr std::uint8_t kPageDoors  = 0x01U;
constexpr std::uint8_t kPageTrip   = 0x02U;
constexpr std::uint8_t kPageEngine = 0x03U;

/* The door bit positions the plugin's car_door.json declares, as shifts into
 * payload[1] of page 1. They are NOT the same order as the Hiworld message's
 * bits -- the two rear doors are swapped between the two layouts -- which is
 * exactly the kind of crossed pair that is invisible on a bench until someone
 * opens one door and watches the wrong icon light up. Hence both sides are
 * named here and the mapping is written out one line per door. */
constexpr std::uint8_t kBenzDoorLeftFront  = 0x01U;  /* bit 0 */
constexpr std::uint8_t kBenzDoorRightFront = 0x02U;  /* bit 1 */
constexpr std::uint8_t kBenzDoorLeftRear   = 0x04U;  /* bit 2 */
constexpr std::uint8_t kBenzDoorRightRear  = 0x08U;  /* bit 3 */
constexpr std::uint8_t kBenzDoorTrunk      = 0x10U;  /* bit 4 */

/* Fields the Benz dashboard has and a 2005-2010 WK cannot source.
 *
 * Engine-oil temperature is not on CAN-B at all. Power and torque are not
 * either -- on a real Benz they are gateway-computed estimates, and the
 * honest options here were to send zero or to invent a number from rpm and
 * pedal position. Zero is sent: a dash that reads 0 hp is obviously
 * uninstrumented, whereas a plausible-looking fabricated curve is a number
 * someone will eventually believe. */
constexpr std::uint16_t kNoSource = 0U;

void put16(volatile std::uint8_t* p, std::uint16_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v & 0xFFU);
}

/* The Benz odometer field is 24-bit; anything above that wraps on the wire
 * and would display as a sudden small number, so it saturates instead. A WK
 * would have to reach 16.7 million km to get here, but the clamp costs one
 * comparison and removes the question. */
constexpr std::uint32_t kOdoMax = 0x00FFFFFFU;

void put24(volatile std::uint8_t* p, std::uint32_t v) noexcept
{
    const std::uint32_t c = (v > kOdoMax) ? kOdoMax : v;
    p[0] = static_cast<std::uint8_t>((c >> 16) & 0xFFU);
    p[1] = static_cast<std::uint8_t>((c >> 8) & 0xFFU);
    p[2] = static_cast<std::uint8_t>(c & 0xFFU);
}

/* ---------------------------------------------------------------------------
 * Hiworld door bits -> Benz door bits.
 *
 * The stock decoder already turns the car's 0x013 b2 bitfield into
 * ram::VehicleState's layout, so this re-maps that rather than re-decoding
 * the frame: one source of truth for which door is open, two wire formats.
 * ------------------------------------------------------------------------- */
std::uint8_t benzDoorBits(std::uint8_t hiworld) noexcept
{
    std::uint8_t out = 0U;
    if ((hiworld & ram::VehicleState::kDoorLeftFront) != 0U)  { out |= kBenzDoorLeftFront; }
    if ((hiworld & ram::VehicleState::kDoorRightFront) != 0U) { out |= kBenzDoorRightFront; }
    if ((hiworld & ram::VehicleState::kDoorLeftRear) != 0U)   { out |= kBenzDoorLeftRear; }
    if ((hiworld & ram::VehicleState::kDoorRightRear) != 0U)  { out |= kBenzDoorRightRear; }
    if ((hiworld & ram::VehicleState::kDoorTailgate) != 0U)   { out |= kBenzDoorTrunk; }
    return out;
}

}  // namespace

void benzPrimeFrames() noexcept
{
    ram::BenzFrames& f = ram::app().benzFrames;
    f.door41[0]   = kPageDoors;
    f.trip41[0]   = kPageTrip;
    f.engine41[0] = kPageEngine;
}

void benzPublish() noexcept
{
    ram::AppRam& r = ram::app();
    const ram::Telemetry& t = r.telemetry;
    ram::BenzFrames& f = r.benzFrames;

    /* 0x08 / 0x07 -- rpm and speed, straight u16s. The plugin draws no gauge
     * for either; it re-broadcasts them as Android intents for whatever
     * dashboard app is installed. No scaling, no offset. */
    put16(f.rpm08, t.rpm);
    put16(f.speed07, t.speedKph);

    /* 0x41 page 1 -- doors. The popup only appears when the computed mask
     * CHANGES, so a repeated identical frame is free; the round-robin's
     * change detection makes that moot anyway. */
    f.door41[1] = benzDoorBits(r.vehicle.doors());

    /* 0x41 page 2 -- ambient (0.1 C, unsigned) and the 24-bit odometer. */
    put16(&f.trip41[1], t.ambientDeciC);
    put24(&f.trip41[3], t.odometerKm);

    /* 0x41 page 3 -- the engine-computer row. Coolant and voltage are real;
     * oil temperature, power and torque have no WK source and go out as
     * zero. See kNoSource. */
    put16(&f.engine41[1], t.coolantC);
    put16(&f.engine41[3], kNoSource);   /* engine-oil temperature */
    put16(&f.engine41[5], t.deciVolts);
    put16(&f.engine41[7], kNoSource);   /* power, hp              */
    put16(&f.engine41[9], kNoSource);   /* torque, lb-ft          */
}

}  // namespace logic
