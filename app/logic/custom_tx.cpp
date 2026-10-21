/* ============================================================================
 * custom_tx.cpp -- see custom_tx.hpp.
 *
 * Spec: docs/CUSTOM-TX-SPEC.md section 3 (the payloads) and section 7 (the
 * test vectors every one of these packings is checked against in
 * tests/host/test_custom_tx.cpp). WK signal sources and their confidence
 * levels: docs/JEEP-WK-CAN-ID-byte-map.md.
 *
 * Modelled on raise_benz_tx.cpp, which does the same job for the Mercedes
 * emulation: decoded signals in physical units in, packed payloads out, no
 * arithmetic beyond endianness. All the unit conversion and all the sentinel
 * logic is upstream in custom_decode.cpp, so this file has exactly one
 * decision in it -- the heartbeat cadence.
 * ==========================================================================*/

#include "custom_tx.hpp"

#include "app_config.hpp"
#include "custom_decode.hpp"
#include "head_unit.hpp"
#include "ram_map.hpp"

namespace logic {
namespace {

/* ---------------------------------------------------------------------------
 * The heartbeat cadence, in 8 ms publish passes.
 *
 * 125 x 8 ms = 1000 ms. Counted here rather than hung off the scheduler's
 * 1024 ms flagSlow, which is already the parameter-page save and is a
 * test-and-clear: a second consumer would have to either steal it from
 * params::save() or turn it into a shared latch. A local divider is four
 * bytes of .bss and couples nothing.
 * ------------------------------------------------------------------------- */
constexpr std::uint8_t kHeartbeatPasses = 125U;

/* Engine load: the contract defines 0x00 as unknown, and it stays unknown.
 *
 * 0x02C b2 is the load-shaped channel -- r = +0.988 against pedal position --
 * but its 0..136 range has never been calibrated against a scan tool, so any
 * scale factor would be invented. A field that reads 0 is uninstrumented; a
 * plausible-looking fabricated curve is a number someone eventually believes.
 * Same reasoning as kNoSource in raise_benz_tx.cpp.
 *
 * Note this is the one field in the block whose unknown encoding is NOT
 * all-ones, and the reason is that 0xFF would mean 255 % load. 0 % is at least
 * outside the set of values the field can legitimately report as a surprise. */
constexpr std::uint8_t kLoadParked = ram::kCustomLoadUnknown;

/* Range to empty: no source. The box would need a consumption history it does
 * not keep -- 0x1A7 carries an instantaneous injection rate whose divisor is
 * still a candidate, and integrating a candidate into a distance is how a
 * dashboard ends up confidently wrong. 0xFFFF is outside any plausible range
 * and is rejected for free by a consumer that range-checks at all. */
constexpr std::uint16_t kRangeNoSource = ram::kCustomUnknown16;

void put16(volatile std::uint8_t* p, std::uint16_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v & 0xFFU);
}

/* The 24-bit odometer. Masked per byte rather than clamped: the value can only
 * come from a three-byte CAN field, so it is already in range -- the mask is
 * there so a future writer cannot silently truncate the high byte instead. */
void put24(volatile std::uint8_t* p, std::uint32_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<std::uint8_t>(v & 0xFFU);
}

/* The one signed field. Two's complement on the wire, so the packing is the
 * same bit pattern as the unsigned case -- spelled out separately anyway,
 * because the cast is the whole point and an implicit one here is how a
 * -7.5 degC becomes 6529.6 degC. */
void put16s(volatile std::uint8_t* p, std::int16_t v) noexcept
{
    put16(p, static_cast<std::uint16_t>(v));
}

}  // namespace

/* ============================================================================
 * Prime the payloads AND the signals behind them.
 *
 * Must run before the first transmit, for the same reason benzPrimeFrames()
 * must: the scheduler starts sending ~48 ms into phase 2, which is sooner than
 * some of these sources arrive. 0x02C is a ~500 ms frame and 0x159 a ~1 s one,
 * so a cold boot WILL emit 0xB4 and 0xB8 before the car has reported either.
 * Priming is what makes those first frames say "no value" instead of -40 degC
 * and 0.0 litres.
 *
 * The signals are primed through customPrimeSignals() rather than duplicated
 * here, and then packed once, so the payloads and the signals cannot disagree
 * about what "unknown" looks like.
 * ==========================================================================*/
void customPrimeFrames() noexcept
{
    customPrimeSignals();
    customPublish();

    /* customPublish() has just run its heartbeat divider once. Reset it so the
     * first heartbeat lands a full second after boot rather than 8 ms early --
     * cosmetic, but it keeps the cadence exactly 1 Hz from the first frame. */
    ram::app().customSignals.heartbeatDiv = 0U;
}

/* ============================================================================
 * Repack the signals into the eight payloads.
 *
 * Called from the 8 ms task. Nothing here decides what is valid -- the
 * decoders have already written either a reading or a sentinel into every
 * field, and the validity mask with it. This function only moves bytes.
 *
 * THE SCHEDULER DOES THE REST. svc::hu::txSched() compares each payload with
 * its shadow and sends the ones that differ, one frame per 48 ms, so writing
 * an unchanged payload every 8 ms costs nothing on the wire. That is also why
 * there is no rate limiting here and no "dirty" flag: the shadow IS the dirty
 * flag.
 *
 * DEADBAND. A payload that changes on noise would claim a 48 ms slot for
 * nothing, so every field is quantised to a step a human can see -- and all of
 * them already are, by construction rather than by code: speed and throttle
 * are whole percent and km/h, temperatures are whole degrees, fuel is 0.5 L,
 * and centivolts always move in steps of ten because the source is 0.1 V.
 * The one field that is not quantised is rpm, which is genuinely 1 rpm/count
 * and genuinely changes that fast; it is the reason the block is 12 slots and
 * not 6.
 * ==========================================================================*/
void customPublish() noexcept
{
    ram::AppRam&        r = ram::app();
    ram::CustomSignals& c = r.customSignals;
    ram::CustomFrames&  f = r.customFrames;

    /* ---- 0xB0 HEARTBEAT: protocol revision, then the validity mask ------- */
    f.b0[0] = ram::kCustomProtoRev;
    f.b0[1] = c.valid;

    /* ---- 0xB2 IGNITION: the whole byte is maintained by the decoders ----- */
    f.b2[0] = c.ignBits;

    /* ---- 0xB3 DRIVETRAIN: speed, rpm, load, throttle --------------------- */
    put16(&f.b3[0], c.speedKph);
    put16(&f.b3[2], c.rpm);
    f.b3[4] = kLoadParked;
    f.b3[5] = c.throttlePct;

    /* ---- 0xB4 ENGINE: coolant, intake, fuel %, volts --------------------- */
    f.b4[0] = c.coolantBias40;
    f.b4[1] = c.intakeBias40;
    f.b4[2] = c.fuelPct;
    put16(&f.b4[3], c.centiVolts);

    /* ---- 0xB5 CLIMATE: outside air, signed decidegrees ------------------- */
    put16s(&f.b5[0], c.outsideDeciC);

    /* ---- 0xB7 TYRE: ONE corner per frame, rotating -----------------------
     * The position byte is advanced by customTpmsPoll() every 5 s, so this
     * packs whichever corner is current. Temperature is ALWAYS the reserved
     * not-available code: 0x213B carries no temperature at all, and the
     * per-sensor identifiers that do have one decode it by a hypothesis
     * (raw - 40) that nobody has confirmed.
     *
     * The flags byte reports only "no signal", and only when the data has gone
     * stale. Battery, leak and sensor state live in per-tyre fault identifiers
     * this build deliberately does not read, so claiming them would be
     * inventing data. */
    {
        const std::uint8_t  pos = c.tpmsPosition;
        const std::uint16_t kpa = c.tyreKpa[pos];
        const bool          live = (kpa != ram::kCustomUnknown16);

        f.b7[0] = pos;
        put16(&f.b7[1], kpa);
        f.b7[3] = ram::kCustomUnknown8;
        f.b7[4] = live ? 0U : ram::kTyreFlagNoSignal;
    }

    /* ---- 0xB8 FUEL: decilitres, range ------------------------------------ */
    put16(&f.b8[0], c.fuelDecilitres);
    put16(&f.b8[2], kRangeNoSource);

    /* ---- 0xBB ODOMETER: 24-bit, 0.1 km/count ---------------------------- */
    put24(&f.bB[0], c.odometerDeciKm);

    /* ---- 0xBD COMPASS: sector byte, then magnetic heading ----------------
     * The sector is forwarded verbatim and is NOT re-derived from the heading
     * -- see customCompass() for why the two are allowed to disagree at a
     * boundary and why that disagreement must survive to the consumer. */
    f.bD[0] = c.compassSector;
    put16(&f.bD[1], c.headingDeg);

    /* ---- the 1 Hz heartbeat request -------------------------------------
     * 0xB0's payload may be identical for hours -- a validity mask that does
     * not change is the NORMAL case, not a stuck one -- so the change-driven
     * round-robin would only send it on the ~8.6 s keep-alive sweep. Nothing
     * is toggled artificially to force it out; instead the slot is marked for
     * an explicit send, which is what ram::AppRam::uartSlotState's
     * kSlotRequest state is for.
     *
     * requestSend() is a no-op if 0xB0 is not in the active dialect's table,
     * so this needs no protocol guard of its own. */
    c.heartbeatDiv = static_cast<std::uint8_t>(c.heartbeatDiv + 1U);
    if (c.heartbeatDiv >= kHeartbeatPasses) {
        c.heartbeatDiv = 0U;
        svc::hu::requestSend(kCmdHeartbeat);
    }
}

}  // namespace logic
