/* ============================================================================
 * test_raise_benz_tx.cpp -- the Mercedes-Benz emulation: WK CAN in,
 * Benz frames out.
 *
 * Built and run ONLY in the benz configuration (tools/test.py --protocol benz).
 *
 * THE POINT OF THIS FILE. The five frames below are checked against WORKED
 * EXAMPLES computed independently from the decompiled Raise Benz plugin
 * (my-radio-files/CanBus/plugin/1/34/P1_301.apk) and recorded in the protocol
 * analysis, not against what this firmware happens to produce. Each vector
 * fixes the page selector, the field order, the endianness, the scaling and
 * the one's-complement checksum in one go, from a source outside this repo's
 * own code. That is as close to an on-air capture as is available without a
 * head unit on the bench:
 *
 *      rpm 2000                        2E 08 02 07 D0 1E
 *      speed 60 km/h                   2E 07 02 00 3C BA
 *      odo 123456 km, ambient 23.4 C   2E 41 06 02 00 EA 01 E2 40 A9
 *      coolant 88 C, 14.2 V            2E 41 0B 03 00 58 00 00 00 8E 00 00 00 00 CA
 *      all doors closed                2E 41 02 01 00 BB
 *
 * The CAN side is driven through the real decoder (logic::decodeProfileJeep)
 * by writing the RX ring the way the CAN_RX0 ISR does, so the WK byte maps in
 * docs/JEEP-WK-CAN-ID-byte-map.md are exercised too, not bypassed.
 * ==========================================================================*/

#include "test_framework.hpp"

#include "app_config.hpp"
#include "raise_benz_tx.hpp"
#include "can_bus.hpp"
#include "can_decode.hpp"
#include "fake_hal.hpp"
#include "head_unit.hpp"
#include "ram_map.hpp"

namespace {

/* ---- RX ring geometry, as test_can_decode.cpp uses it ------------------- */
constexpr std::size_t kStride  = ram::kCanRingStride;
constexpr std::size_t kEntries = ram::kCanRingEntries;
constexpr std::size_t kIdHi = 0U, kIdLo = 2U, kIde = 4U, kDlc = 5U, kData = 6U;

void pushFrame(std::uint32_t id, const std::uint8_t* data) noexcept
{
    ram::AppRam& r = ram::app();
    volatile std::uint8_t* e = r.canRxRing
                             + static_cast<std::size_t>(r.canRing.rxHead) * kStride;
    e[kIdHi]      = static_cast<std::uint8_t>(id >> 16);
    e[kIdHi + 1U] = static_cast<std::uint8_t>(id >> 24);
    e[kIdLo]      = static_cast<std::uint8_t>(id);
    e[kIdLo + 1U] = static_cast<std::uint8_t>(id >> 8);
    e[kIde]       = 0U;
    e[kDlc]       = 8U;
    for (std::size_t i = 0U; i < 8U; ++i) {
        e[kData + i] = (data != nullptr) ? data[i] : 0U;
    }
    r.canRing.rxHead = (r.canRing.rxHead + 1U) % static_cast<std::uint32_t>(kEntries);
}

/* The decoders only run while the bus-present debounce says ACC is up. */
void accOn() noexcept
{
    ram::app().vehicle.setFlag(ram::VehicleState::kFlagAccOn, true);
    ram::app().busPresentDebounce = 0U;
}

void decodeOnce() noexcept { logic::decodeProfileJeep(); }

/* Build the frame the TX scheduler would emit for one payload, independently:
 * 2E | CMD | LEN | payload | ~(CMD+LEN+sum) -- the spec's rule, spelled out
 * here rather than reused from the firmware. */
std::size_t raiseFrame(std::uint8_t* out, std::uint8_t cmd,
                       const std::uint8_t* pl, std::uint8_t len) noexcept
{
    unsigned s = static_cast<unsigned>(cmd) + static_cast<unsigned>(len);
    out[0] = 0x2EU;
    out[1] = cmd;
    out[2] = len;
    for (std::uint8_t i = 0U; i < len; ++i) {
        out[3U + i] = pl[i];
        s += static_cast<unsigned>(pl[i]);
    }
    out[3U + len] = static_cast<std::uint8_t>(~s & 0xFFU);
    return static_cast<std::size_t>(len) + 4U;
}

/* Copy a payload out of ram::BenzFrames so CHECK_BYTES can see it. */
void grab(std::uint8_t* dst, const volatile std::uint8_t* src, std::uint8_t n) noexcept
{
    for (std::uint8_t i = 0U; i < n; ++i) {
        dst[i] = src[i];
    }
}

}  // namespace

/* ===========================================================================
 * 0. Configuration sanity.
 * ========================================================================= */
TEST(benz_build_has_five_tx_slots)
{
    CHECK(cfg::kIsBenz);
    CHECK_EQ(static_cast<unsigned>(cfg::kTxSlotCount), 5U);
    CHECK_EQ(static_cast<unsigned>(svc::hu::kTxSlots), 5U);
}

/* The page selectors must be in place before the first transmit; a zero page
 * byte matches no page and the head unit silently drops the frame. */
TEST(benz_prime_writes_the_page_selectors)
{
    fake::reset();
    logic::benzPrimeFrames();

    CHECK_EQ(ram::app().benzFrames.door41[0], 0x01U);
    CHECK_EQ(ram::app().benzFrames.trip41[0], 0x02U);
    CHECK_EQ(ram::app().benzFrames.engine41[0], 0x03U);
}

/* ===========================================================================
 * 1. The worked examples, end to end from ram::Telemetry.
 * ========================================================================= */

TEST(benz_rpm_frame_matches_the_worked_example)
{
    fake::reset();
    logic::benzPrimeFrames();
    ram::app().telemetry.rpm = 2000U;
    logic::benzPublish();

    std::uint8_t payload[2];
    grab(payload, ram::app().benzFrames.rpm08, 2U);

    std::uint8_t wire[8];
    const std::size_t n = raiseFrame(wire, 0x08U, payload, 2U);
    const std::uint8_t want[6] = { 0x2EU, 0x08U, 0x02U, 0x07U, 0xD0U, 0x1EU };
    CHECK_EQ(n, 6U);
    CHECK_BYTES(wire, want, 6U);
}

TEST(benz_speed_frame_matches_the_worked_example)
{
    fake::reset();
    logic::benzPrimeFrames();
    ram::app().telemetry.speedKph = 60U;
    logic::benzPublish();

    std::uint8_t payload[2];
    grab(payload, ram::app().benzFrames.speed07, 2U);

    std::uint8_t wire[8];
    raiseFrame(wire, 0x07U, payload, 2U);
    const std::uint8_t want[6] = { 0x2EU, 0x07U, 0x02U, 0x00U, 0x3CU, 0xBAU };
    CHECK_BYTES(wire, want, 6U);
}

TEST(benz_trip_page_matches_the_worked_example)
{
    fake::reset();
    logic::benzPrimeFrames();
    ram::app().telemetry.odometerKm   = 123456U;
    ram::app().telemetry.ambientDeciC = 234U;   /* 23.4 C */
    logic::benzPublish();

    std::uint8_t payload[6];
    grab(payload, ram::app().benzFrames.trip41, 6U);

    std::uint8_t wire[16];
    const std::size_t n = raiseFrame(wire, 0x41U, payload, 6U);
    const std::uint8_t want[10] = { 0x2EU, 0x41U, 0x06U, 0x02U, 0x00U, 0xEAU,
                                    0x01U, 0xE2U, 0x40U, 0xA9U };
    CHECK_EQ(n, 10U);
    CHECK_BYTES(wire, want, 10U);
}

TEST(benz_engine_page_matches_the_worked_example)
{
    fake::reset();
    logic::benzPrimeFrames();
    ram::app().telemetry.coolantC  = 88U;
    ram::app().telemetry.deciVolts = 142U;      /* 14.2 V */
    logic::benzPublish();

    std::uint8_t payload[11];
    grab(payload, ram::app().benzFrames.engine41, 11U);

    std::uint8_t wire[24];
    const std::size_t n = raiseFrame(wire, 0x41U, payload, 11U);
    const std::uint8_t want[15] = { 0x2EU, 0x41U, 0x0BU, 0x03U,
                                    0x00U, 0x58U,          /* coolant 88 C   */
                                    0x00U, 0x00U,          /* oil, no source */
                                    0x00U, 0x8EU,          /* 14.2 V         */
                                    0x00U, 0x00U,          /* power          */
                                    0x00U, 0x00U,          /* torque         */
                                    0xCAU };
    CHECK_EQ(n, 15U);
    CHECK_BYTES(wire, want, 15U);
}

TEST(benz_door_page_matches_the_worked_example)
{
    fake::reset();
    logic::benzPrimeFrames();
    logic::benzPublish();                       /* nothing open */

    std::uint8_t payload[2];
    grab(payload, ram::app().benzFrames.door41, 2U);

    std::uint8_t wire[8];
    raiseFrame(wire, 0x41U, payload, 2U);
    const std::uint8_t want[6] = { 0x2EU, 0x41U, 0x02U, 0x01U, 0x00U, 0xBBU };
    CHECK_BYTES(wire, want, 6U);
}

/* ===========================================================================
 * 2. The door bit remap -- the two protocols disagree about the rear pair.
 * ========================================================================= */

TEST(benz_door_bits_are_remapped_not_copied)
{
    fake::reset();
    logic::benzPrimeFrames();

    /* Left rear only, in the Hiworld layout the stock decoder produces. */
    ram::app().vehicle.setDoorBit(ram::VehicleState::kDoorLeftRear, true);
    logic::benzPublish();
    CHECK_EQ(ram::app().benzFrames.door41[1], 0x04U);   /* Benz bit 2 */

    /* Right rear only -- must land on bit 3, NOT on bit 2. */
    ram::app().vehicle.setDoorBit(ram::VehicleState::kDoorLeftRear, false);
    ram::app().vehicle.setDoorBit(ram::VehicleState::kDoorRightRear, true);
    logic::benzPublish();
    CHECK_EQ(ram::app().benzFrames.door41[1], 0x08U);   /* Benz bit 3 */
}

TEST(benz_door_bits_cover_every_door)
{
    fake::reset();
    logic::benzPrimeFrames();

    ram::VehicleState& v = ram::app().vehicle;
    v.setDoorBit(ram::VehicleState::kDoorLeftFront, true);
    v.setDoorBit(ram::VehicleState::kDoorRightFront, true);
    v.setDoorBit(ram::VehicleState::kDoorLeftRear, true);
    v.setDoorBit(ram::VehicleState::kDoorRightRear, true);
    v.setDoorBit(ram::VehicleState::kDoorTailgate, true);
    logic::benzPublish();

    CHECK_EQ(ram::app().benzFrames.door41[1], 0x1FU);
}

/* ===========================================================================
 * 3. The WK decoders, driven through the real dispatcher.
 * ========================================================================= */

/* 0x002: b0:b1 rpm, b2:b3 speed at raw/128 km/h. */
TEST(benz_decodes_rpm_and_speed_from_0x002)
{
    fake::reset();
    accOn();

    /* 2000 rpm, and 88 km/h -> raw 88*128 = 11264 = 0x2C00 */
    const std::uint8_t d[8] = { 0x07U, 0xD0U, 0x2CU, 0x00U, 0U, 0U, 0U, 0U };
    pushFrame(0x002U, d);
    decodeOnce();

    CHECK_EQ(ram::app().telemetry.rpm, 2000U);
    CHECK_EQ(ram::app().telemetry.speedKph, 88U);
}

/* All-ones is the car's "unavailable" marker. The Benz plugin cannot blank a
 * field, so the previous reading must survive rather than becoming 65535. */
TEST(benz_holds_last_value_when_the_car_reports_unavailable)
{
    fake::reset();
    accOn();

    const std::uint8_t good[8] = { 0x07U, 0xD0U, 0x2CU, 0x00U, 0U, 0U, 0U, 0U };
    pushFrame(0x002U, good);
    decodeOnce();
    CHECK_EQ(ram::app().telemetry.rpm, 2000U);

    const std::uint8_t gone[8] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0U, 0U, 0U, 0U };
    pushFrame(0x002U, gone);
    decodeOnce();
    CHECK_EQ(ram::app().telemetry.rpm, 2000U);          /* held, not 65535 */
    CHECK_EQ(ram::app().telemetry.speedKph, 88U);
}

/* 0x014: b0:b1:b2 odometer at 0.1 km/count, b4 ambient in whole degrees. */
TEST(benz_decodes_odometer_and_ambient_from_0x014)
{
    fake::reset();
    accOn();

    /* 1 234 567 tenths = 123 456.7 km -> 123456 km after the /10 */
    const std::uint8_t d[8] = { 0x12U, 0xD6U, 0x87U, 0xFFU, 23U, 0U, 0U, 0U };
    pushFrame(0x014U, d);
    decodeOnce();

    CHECK_EQ(ram::app().telemetry.odometerKm, 123456U);
    CHECK_EQ(ram::app().telemetry.ambientDeciC, 230U);   /* 23.0 C */
}

/* The Benz ambient field is UNSIGNED 0.1 C. A sub-zero reading would wrap to
 * about 6553 C, so it is clamped to zero instead -- an acknowledged loss. */
TEST(benz_clamps_sub_zero_ambient_rather_than_wrapping)
{
    fake::reset();
    accOn();

    const std::uint8_t d[8] = { 0x00U, 0x00U, 0x00U, 0xFFU, 0xF6U, 0U, 0U, 0U };
    pushFrame(0x014U, d);
    decodeOnce();

    CHECK_EQ(ram::app().telemetry.ambientDeciC, 0U);
}

/* An unavailable odometer must not be published as 1677721 km. */
TEST(benz_ignores_an_unavailable_odometer)
{
    fake::reset();
    accOn();

    const std::uint8_t good[8] = { 0x12U, 0xD6U, 0x87U, 0xFFU, 23U, 0U, 0U, 0U };
    pushFrame(0x014U, good);
    decodeOnce();

    const std::uint8_t gone[8] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU, 23U, 0U, 0U, 0U };
    pushFrame(0x014U, gone);
    decodeOnce();

    CHECK_EQ(ram::app().telemetry.odometerKm, 123456U);
}

/* 0x02C b1: coolant, raw - 40 degrees C. */
TEST(benz_decodes_coolant_from_0x02C)
{
    fake::reset();
    accOn();

    const std::uint8_t d[8] = { 0U, 128U, 0U, 0U, 0U, 0U, 0U, 0U };
    pushFrame(0x02CU, d);
    decodeOnce();

    CHECK_EQ(ram::app().telemetry.coolantC, 88U);        /* 128 - 40 */
}

TEST(benz_ignores_an_unavailable_coolant)
{
    fake::reset();
    accOn();

    const std::uint8_t good[8] = { 0U, 128U, 0U, 0U, 0U, 0U, 0U, 0U };
    pushFrame(0x02CU, good);
    decodeOnce();

    const std::uint8_t gone[8] = { 0xFFU, 0xFFU, 0U, 0U, 0U, 0U, 0U, 0U };
    pushFrame(0x02CU, gone);
    decodeOnce();

    CHECK_EQ(ram::app().telemetry.coolantC, 88U);
}

/* 0x015 b1: battery voltage, already in tenths of a volt on both sides. */
TEST(benz_decodes_voltage_from_0x015)
{
    fake::reset();
    accOn();

    const std::uint8_t d[8] = { 0U, 142U, 0U, 0U, 0U, 0U, 0U, 0U };
    pushFrame(0x015U, d);
    decodeOnce();

    CHECK_EQ(ram::app().telemetry.deciVolts, 142U);      /* 14.2 V */
}

/* ===========================================================================
 * 4. Nothing fabricated.
 *
 * Engine-oil temperature, power and torque have Benz fields and no WK
 * source. They must go out as zero -- an obviously uninstrumented reading --
 * rather than as a plausible number somebody would believe.
 * ========================================================================= */
TEST(benz_sends_zero_for_fields_the_wk_cannot_source)
{
    fake::reset();
    logic::benzPrimeFrames();
    ram::app().telemetry.rpm      = 4000U;
    ram::app().telemetry.coolantC = 90U;
    logic::benzPublish();

    const volatile std::uint8_t* e = ram::app().benzFrames.engine41;
    CHECK_EQ(e[3], 0U);  CHECK_EQ(e[4], 0U);    /* oil temperature */
    CHECK_EQ(e[7], 0U);  CHECK_EQ(e[8], 0U);    /* power           */
    CHECK_EQ(e[9], 0U);  CHECK_EQ(e[10], 0U);   /* torque          */
}

/* ===========================================================================
 * 5. The odometer is 24-bit on the wire; saturate rather than wrap.
 * ========================================================================= */
TEST(benz_saturates_an_over_range_odometer)
{
    fake::reset();
    logic::benzPrimeFrames();
    ram::app().telemetry.odometerKm = 0x01000000U;       /* one past 24 bits */
    logic::benzPublish();

    const volatile std::uint8_t* t = ram::app().benzFrames.trip41;
    CHECK_EQ(t[3], 0xFFU);
    CHECK_EQ(t[4], 0xFFU);
    CHECK_EQ(t[5], 0xFFU);
}

/* ===========================================================================
 * 6. The link actually transmits them.
 *
 * Everything above checks the payload buffers. This checks that svc::hu's
 * round-robin picks a changed Benz payload up and clocks a well-formed frame
 * out of the port -- i.e. that the table, the shadows and the framing are
 * wired to each other.
 * ========================================================================= */
TEST(benz_scheduler_emits_a_well_formed_frame_after_a_change)
{
    fake::reset();
    svc::hu::resetState();
    logic::benzPrimeFrames();

    ram::app().telemetry.rpm = 2000U;
    logic::benzPublish();

    /* The scheduler emits on every 6th 8 ms pass. */
    for (std::uint8_t i = 0U; i < svc::hu::kTicksPerFrame * 8U; ++i) {
        svc::hu::service();
    }

    /* Whatever it chose, it must be a syntactically valid Raise frame. */
    const volatile std::uint8_t* f = ram::app().uartFrame;
    const std::uint8_t wireLen = ram::app().uartFrame[svc::hu::kFbTxLen];
    CHECK(wireLen >= 5U);
    CHECK_EQ(f[0], 0x2EU);

    const std::uint8_t len = f[2];
    CHECK_EQ(static_cast<unsigned>(wireLen), static_cast<unsigned>(len) + 4U);
    CHECK(len >= cfg::kRaiseMinPayload);
    CHECK(len <= cfg::kRaiseMaxPayload);

    unsigned s = 0U;
    for (std::uint8_t i = 1U; i < static_cast<std::uint8_t>(len + 3U); ++i) {
        s += static_cast<unsigned>(f[i]);
    }
    CHECK_EQ(f[len + 3U], static_cast<std::uint8_t>(~s & 0xFFU));
}
