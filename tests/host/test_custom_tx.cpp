/* ============================================================================
 * test_custom_tx.cpp -- the custom 0xB0-0xB8 telemetry block: WK CAN in,
 * Hiworld payloads out.
 *
 * Built and run ONLY in the jeep configuration (tools/test.py --protocol jeep).
 *
 * THE POINT OF THIS FILE. The frames below are checked against the WORKED
 * EXAMPLES in docs/CUSTOM-TX-SPEC.md section 7, which were computed from the
 * section 1 framing rule independently of this firmware. Each vector fixes the
 * field order, the endianness, the scaling and the checksum in one go. The
 * framer in this file is written out from that rule rather than reused from
 * hu_hiworld.cpp, so a mistake in the firmware's emit() cannot hide by being
 * made twice -- the spec itself records that a restatement of the rule got it
 * wrong once already.
 *
 * The CAN side is driven through the REAL decoder (logic::decodeProfileJeep)
 * by writing the RX ring the way the CAN_RX0 ISR does, so the WK byte maps in
 * docs/JEEP-WK-CAN-ID-byte-map.md are exercised too, not bypassed.
 *
 * ---------------------------------------------------------------------------
 * THE SENTINELS, AND THE ONE FIELD THAT HOLDS INSTEAD.
 *
 * 0xFF is the RESERVED "not available" code for every +40-biased temperature
 * and 0xFFFF for speed, rpm and range -- so no frame ever has to be suppressed
 * to hide one bad field, which matters because every command here carries
 * several and the others are still good. 0xB4 alone carries coolant, intake,
 * fuel % and voltage.
 *
 * Fuel is the exception that HOLDS rather than blanking when its gate trips,
 * and that is physical rather than lazy: the gate asserts at ignition-off and
 * a parked car's tank does not change. 0xB0 bit 5 is what says the figure is
 * no longer being refreshed.
 * ==========================================================================*/

#include "test_framework.hpp"

#include "app_config.hpp"
#include "can_bus.hpp"
#include "can_decode.hpp"
#include "custom_decode.hpp"
#include "custom_tx.hpp"
#include "fake_hal.hpp"
#include "head_unit.hpp"
#include "ram_map.hpp"

namespace {

/* ---- RX ring geometry, as test_can_decode.cpp and test_raise_benz_tx.cpp
 * both write it ----------------------------------------------------------- */
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

void decodeOnce() noexcept { logic::decodeProfileJeep(); }

/* Start from a primed box: sentinels everywhere, nothing live. */
void primed() noexcept
{
    fake::reset();
    logic::customPrimeFrames();
}

/* ---------------------------------------------------------------------------
 * The Hiworld frame, built from docs/CUSTOM-TX-SPEC.md section 1 and nothing
 * else:
 *     AA 55 | LEN | CMD | payload[LEN] | CKS        wire length = LEN + 5
 *     LEN = PAYLOAD LENGTH ONLY
 *     CKS = (0xAA + 0x55 + LEN + CMD + sum(payload)) & 0xFF
 *
 * Note this writes the checksum as the FULL byte sum including the preamble,
 * which is the definition; the firmware computes the algebraically identical
 * (LEN + CMD + sum - 1) form. Spelling it the other way round here is the
 * point: the two forms agreeing on every vector is the evidence.
 * ------------------------------------------------------------------------- */
std::size_t hiworldFrame(std::uint8_t* out, std::uint8_t cmd,
                         const volatile std::uint8_t* pl, std::uint8_t len) noexcept
{
    unsigned s = 0xAAU + 0x55U + static_cast<unsigned>(len) + static_cast<unsigned>(cmd);
    out[0] = 0xAAU;
    out[1] = 0x55U;
    out[2] = len;
    out[3] = cmd;
    for (std::uint8_t i = 0U; i < len; ++i) {
        out[4U + i] = pl[i];
        s += static_cast<unsigned>(pl[i]);
    }
    out[4U + len] = static_cast<std::uint8_t>(s & 0xFFU);
    return static_cast<std::size_t>(len) + 5U;
}

/* ---- the CAN payloads the vectors need, as named builders ---------------- */

/* 0x000 b0: 00 wake, 01 off, 63 ACC, 85 running, A9 crank. */
void ignition(std::uint8_t b0) noexcept
{
    const std::uint8_t d[8] = { b0, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    pushFrame(logic::canid::kJeepIgnition, d);
}

/* 0x002: b0:b1 rpm, b2:b3 speed x128. */
void engine(std::uint16_t rpm, std::uint16_t speedRaw) noexcept
{
    const std::uint8_t d[8] = {
        static_cast<std::uint8_t>(rpm >> 8),      static_cast<std::uint8_t>(rpm),
        static_cast<std::uint8_t>(speedRaw >> 8), static_cast<std::uint8_t>(speedRaw),
        0U, 0U, 0U, 0U
    };
    pushFrame(logic::canid::kJeepEngine, d);
}

/* 0x003 b4: the ASCII PRNDL character. b2 is the lever bitfield the STOCK
 * reverse flag reads, kept consistent so the two readers cannot be confused
 * for one another. */
void gear(std::uint8_t ascii, std::uint8_t lever) noexcept
{
    const std::uint8_t d[8] = { 0U, 0U, lever, 0U, ascii, 0xFFU, 0U, 0U };
    pushFrame(logic::canid::kJeepReverse, d);
}

/* 0x0D0 b0: bit7 parking brake, over the 0x04 released-state baseline. */
void parkBrake(std::uint8_t b0) noexcept
{
    const std::uint8_t d[8] = { b0, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    pushFrame(logic::canid::kJeepParkBrake, d);
}

/* 0x014: b0:b1:b2 odometer at 0.1 km/count, b4 ambient in whole degrees C.
 * ONE FRAME, TWO INDEPENDENT SIGNALS with separate gates -- which is the whole
 * point of several tests below, so the builder takes both. */
void trip(std::uint32_t odoRaw, std::uint8_t b4) noexcept
{
    const std::uint8_t d[8] = {
        static_cast<std::uint8_t>((odoRaw >> 16) & 0xFFU),
        static_cast<std::uint8_t>((odoRaw >> 8) & 0xFFU),
        static_cast<std::uint8_t>(odoRaw & 0xFFU),
        0xFFU, b4, 0U, 0U, 0U
    };
    pushFrame(logic::canid::kJeepTrip, d);
}

/* Ambient alone, with the odometer held at a known-good value so a test that
 * is about temperature cannot be perturbed by the distance field. */
void ambient(std::uint8_t b4) noexcept { trip(0x12D687U, b4); }

/* 0x1B4: b0 8-way sector enum, b1 magnetic heading at 2 deg/count. b2 is a
 * constant 0x07 in 2 953/2 953 frames and is neither forwarded nor acted on. */
void compass(std::uint8_t b0, std::uint8_t b1) noexcept
{
    const std::uint8_t d[8] = { b0, b1, 0x07U, 0U, 0U, 0U, 0U, 0U };
    pushFrame(logic::canid::kJeepCompass, d);
}

/* 0x015: b1 volts 0.1 V/count, b2 intake air degrees C. */
void voltsIntake(std::uint8_t b1, std::uint8_t b2) noexcept
{
    const std::uint8_t d[8] = { 0U, b1, b2, 0xFFU, 0U, 0U, 0U, 0U };
    pushFrame(logic::canid::kJeepSteering, d);
}

/* 0x02C b1: coolant, already degC+40. */
void coolant(std::uint8_t b1) noexcept
{
    const std::uint8_t d[8] = { 0U, b1, 0U, 0x64U, 0U, 0U, 0U, 0U };
    pushFrame(logic::canid::kJeepCoolant, d);
}

/* The all-FF "module asleep" form of 0x02C. */
void coolantUnavailable() noexcept
{
    const std::uint8_t d[8] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU,
                                0xFFU, 0xFFU, 0xFFU, 0xFFU };
    pushFrame(logic::canid::kJeepCoolant, d);
}

/* 0x159: b4 validity gate (0 good, 1 means b5 is not a reading), b5 level at
 * 0.5 L/count. */
void fuel(std::uint8_t b4, std::uint8_t b5) noexcept
{
    const std::uint8_t d[8] = { 0xFEU, 0xFFU, 0xFFU, 0xFFU, b4, b5, 0U, 0U };
    pushFrame(logic::canid::kJeepFuel, d);
}

/* 0x2C0: b2 pedal, b6 its mirror. Pass a different b6 to forge a corrupt
 * frame. */
void throttle(std::uint8_t b2, std::uint8_t b6) noexcept
{
    const std::uint8_t d[8] = { 0xFFU, 0xFFU, b2, static_cast<std::uint8_t>(b2 / 2U),
                                0x7EU, 0xFFU, b6, 0x02U };
    pushFrame(logic::canid::kJeepThrottle, d);
}

/* Shorthand: the payload of one custom command, after a publish. */
const volatile std::uint8_t* b0() noexcept { return ram::app().customFrames.b0; }
const volatile std::uint8_t* b2() noexcept { return ram::app().customFrames.b2; }
const volatile std::uint8_t* b3() noexcept { return ram::app().customFrames.b3; }
const volatile std::uint8_t* b4() noexcept { return ram::app().customFrames.b4; }
const volatile std::uint8_t* b5() noexcept { return ram::app().customFrames.b5; }
const volatile std::uint8_t* b8() noexcept { return ram::app().customFrames.b8; }
const volatile std::uint8_t* bB() noexcept { return ram::app().customFrames.bB; }
const volatile std::uint8_t* bD() noexcept { return ram::app().customFrames.bD; }

std::uint8_t validMask() noexcept { return ram::app().customSignals.valid; }
std::uint8_t ignByte() noexcept   { return ram::app().customSignals.ignBits; }

}  // namespace

/* ===========================================================================
 * 0. Configuration sanity -- the five couplings section 6 of the spec warns
 *    about, four of which fail silently.
 * ========================================================================= */
TEST(custom_jeep_build_has_fourteen_tx_slots)
{
    CHECK_FALSE(cfg::kIsBenz);
    CHECK_EQ(static_cast<unsigned>(cfg::kTxSlotCount), 14U);
    CHECK_EQ(static_cast<unsigned>(svc::hu::kTxSlots), 14U);
    /* The slot-state array is sized from cfg::kTxSlotCount and is immediately
     * followed by uartTxSlotIdx: a short array means the scheduler corrupts
     * its own index every sweep and the tests keep passing. */
    CHECK_EQ(sizeof(ram::AppRam::uartSlotState), 14U);
}

/* The index really does survive a full sweep -- the symptom of a short
 * uartSlotState is that the scheduler quietly overwrites its own index, which
 * a size assert alone would not catch if the two ever drifted. */
TEST(custom_full_sweep_leaves_the_slot_index_intact)
{
    fake::reset();
    svc::hu::resetState();
    logic::customPrimeFrames();

    for (int i = 0; i < 6 * 60; ++i) {
        svc::hu::service();
        ram::app().uartFrame[svc::hu::kFbTxBusy] = 0U;
    }
    CHECK(ram::app().uartTxSlotIdx <= svc::hu::kTxSlots);
}

/* The budget section 6 sets out: 12 slots at one frame per 48 ms is 576 ms
 * worst case per signal and a 12 x 720 ms = 8.6 s idle keep-alive. The
 * keep-alive crosses a 15 s consumer staleness check at 21 slots, so this
 * guards the arithmetic rather than the constant. */
TEST(custom_tx_budget_stays_inside_the_staleness_window)
{
    const unsigned worstCaseMs = static_cast<unsigned>(svc::hu::kTxSlots)
                               * static_cast<unsigned>(svc::hu::kTicksPerFrame) * 8U;
    const unsigned keepAliveMs = static_cast<unsigned>(svc::hu::kTxSlots)
                               * static_cast<unsigned>(svc::hu::kIdleDelay)
                               * static_cast<unsigned>(svc::hu::kTicksPerFrame) * 8U;
    CHECK_EQ(worstCaseMs, 672U);
    CHECK_EQ(keepAliveMs, 10080U);     /* 14 x 720 ms = 10.1 s */
    CHECK(keepAliveMs < 15000U);
}

/* ===========================================================================
 * 1. Priming -- every field says "no value" before the car has spoken.
 *
 * .bss is zeroed at startup and zero is a plausible reading for most of these:
 * a zeroed coolant byte is -40 degC, a zeroed volts field is 0.00 V. The
 * scheduler starts transmitting ~48 ms into phase 2, well before a ~500 ms
 * 0x02C or a ~1 s 0x159 has arrived, so these first frames are real.
 * ========================================================================= */
TEST(custom_prime_puts_every_field_in_its_no_value_state)
{
    primed();

    CHECK_EQ(b0()[0], ram::kCustomProtoRev);
    CHECK_EQ(b0()[1], 0U);                 /* nothing live yet */
    CHECK_EQ(b2()[0], 0U);

    CHECK_EQ(b3()[0], 0xFFU);  CHECK_EQ(b3()[1], 0xFFU);   /* speed  */
    CHECK_EQ(b3()[2], 0xFFU);  CHECK_EQ(b3()[3], 0xFFU);   /* rpm    */
    CHECK_EQ(b3()[4], 0x00U);                              /* load, parked */
    CHECK_EQ(b3()[5], 0xFFU);                              /* throttle */

    CHECK_EQ(b4()[0], 0xFFU);                              /* coolant */
    CHECK_EQ(b4()[1], 0xFFU);                              /* intake  */
    CHECK_EQ(b4()[2], 0xFFU);                              /* fuel %  */
    CHECK_EQ(b4()[3], 0xFFU);  CHECK_EQ(b4()[4], 0xFFU);   /* volts   */

    /* The one field that cannot use all-ones: 0xFFFF would decode as -0.1 degC,
     * a plausible reading. 0x8000 is the most negative s16. */
    CHECK_EQ(b5()[0], 0x80U);  CHECK_EQ(b5()[1], 0x00U);

    CHECK_EQ(b8()[0], 0xFFU);  CHECK_EQ(b8()[1], 0xFFU);   /* litres */
    CHECK_EQ(b8()[2], 0xFFU);  CHECK_EQ(b8()[3], 0xFFU);   /* range  */

    /* These two matter most of all: a zeroed CustomSignals would publish
     * "0.0 km, due north, 0 deg", which reads as MORE plausible than -40 degC
     * coolant, not less. 0 is a real heading. */
    CHECK_EQ(bB()[0], 0xFFU);  CHECK_EQ(bB()[1], 0xFFU);  CHECK_EQ(bB()[2], 0xFFU);
    CHECK_EQ(bD()[0], 0xFFU);  CHECK_EQ(bD()[1], 0xFFU);  CHECK_EQ(bD()[2], 0xFFU);
}

/* ===========================================================================
 * 2. The worked examples from CUSTOM-TX-SPEC.md section 7.
 * ========================================================================= */

TEST(custom_heartbeat_matches_the_worked_example)
{
    primed();
    /* Everything live except 0xB7, which has no source on this car. */
    ram::app().customSignals.valid = static_cast<std::uint8_t>(
        ram::kValidB2 | ram::kValidB3 | ram::kValidB4 | ram::kValidB5
        | ram::kValidB8);
    logic::customPublish();

    std::uint8_t wire[8];
    const std::size_t n = hiworldFrame(wire, 0xB0U, b0(), 2U);
    const std::uint8_t want[7] = { 0xAAU, 0x55U, 0x02U, 0xB0U,
                                   0x01U, 0x2FU, 0xE1U };
    CHECK_EQ(n, 7U);
    CHECK_BYTES(wire, want, 7U);
}

/* ACC on, engine running, not in reverse, parking brake off -> 0x03. */
TEST(custom_ignition_matches_the_worked_example)
{
    primed();
    ram::app().customSignals.ignBits =
        static_cast<std::uint8_t>(ram::kIgnAccOn | ram::kIgnEngineRun);
    logic::customPublish();

    std::uint8_t wire[8];
    const std::size_t n = hiworldFrame(wire, 0xB2U, b2(), 1U);
    const std::uint8_t want[6] = { 0xAAU, 0x55U, 0x01U, 0xB2U, 0x03U, 0xB5U };
    CHECK_EQ(n, 6U);
    CHECK_BYTES(wire, want, 6U);
}

/* 63 km/h, 2701 rpm, load unknown, throttle 44 %. */
TEST(custom_drivetrain_matches_the_worked_example)
{
    primed();
    ram::CustomSignals& c = ram::app().customSignals;
    c.speedKph    = 63U;
    c.rpm         = 2701U;
    c.throttlePct = 44U;
    logic::customPublish();

    std::uint8_t wire[16];
    const std::size_t n = hiworldFrame(wire, 0xB3U, b3(), 6U);
    const std::uint8_t want[11] = { 0xAAU, 0x55U, 0x06U, 0xB3U,
                                    0x00U, 0x3FU,   /* 63 km/h       */
                                    0x0AU, 0x8DU,   /* 2701 rpm      */
                                    0x00U,          /* load, parked  */
                                    0x2CU,          /* throttle 44 % */
                                    0xBAU };
    CHECK_EQ(n, 11U);
    CHECK_BYTES(wire, want, 11U);
}

/* coolant 93 degC, intake 45 degC, fuel 92 %, 14.30 V. */
TEST(custom_engine_matches_the_worked_example)
{
    primed();
    ram::CustomSignals& c = ram::app().customSignals;
    c.coolantBias40 = static_cast<std::uint8_t>(93U + 40U);
    c.intakeBias40  = static_cast<std::uint8_t>(45U + 40U);
    c.fuelPct       = 92U;
    c.centiVolts    = 1430U;
    logic::customPublish();

    std::uint8_t wire[16];
    const std::size_t n = hiworldFrame(wire, 0xB4U, b4(), 5U);
    const std::uint8_t want[10] = { 0xAAU, 0x55U, 0x05U, 0xB4U,
                                    0x85U,          /* 93 degC + 40  */
                                    0x55U,          /* 45 degC + 40  */
                                    0x5CU,          /* fuel 92 %     */
                                    0x05U, 0x96U,   /* 14.30 V       */
                                    0x89U };
    CHECK_EQ(n, 10U);
    CHECK_BYTES(wire, want, 10U);
}

TEST(custom_climate_matches_the_worked_example)
{
    primed();
    ram::app().customSignals.outsideDeciC = 250;    /* +25.0 degC */
    logic::customPublish();

    std::uint8_t wire[8];
    const std::size_t n = hiworldFrame(wire, 0xB5U, b5(), 2U);
    const std::uint8_t want[7] = { 0xAAU, 0x55U, 0x02U, 0xB5U,
                                   0x00U, 0xFAU, 0xB0U };
    CHECK_EQ(n, 7U);
    CHECK_BYTES(wire, want, 7U);
}

/* THE SIGN CHECK. The decoder cannot currently produce a negative -- the car's
 * negative encoding has never been captured, so customAmbient() rejects any
 * source byte >= 0x80 -- but the PACKING must already be right, or the day a
 * cold start is captured the field silently reads 6529.6 degC. */
TEST(custom_climate_packs_a_negative_as_twos_complement)
{
    primed();
    ram::app().customSignals.outsideDeciC = -75;    /* -7.5 degC */
    logic::customPublish();

    std::uint8_t wire[8];
    const std::size_t n = hiworldFrame(wire, 0xB5U, b5(), 2U);
    const std::uint8_t want[7] = { 0xAAU, 0x55U, 0x02U, 0xB5U,
                                   0xFFU, 0xB5U, 0x6AU };
    CHECK_EQ(n, 7U);
    CHECK_BYTES(wire, want, 7U);
}

/* 71.0 L in the tank, range unknown. */
TEST(custom_fuel_matches_the_worked_example)
{
    primed();
    ram::app().customSignals.fuelDecilitres = 710U;
    logic::customPublish();

    std::uint8_t wire[16];
    const std::size_t n = hiworldFrame(wire, 0xB8U, b8(), 4U);
    const std::uint8_t want[9] = { 0xAAU, 0x55U, 0x04U, 0xB8U,
                                   0x02U, 0xC6U,   /* 710 dL = 71.0 L */
                                   0xFFU, 0xFFU,   /* range, no source */
                                   0x81U };
    CHECK_EQ(n, 9U);
    CHECK_BYTES(wire, want, 9U);
}

/* ===========================================================================
 * 3. The CAN decode path, end to end.
 * ========================================================================= */

TEST(custom_engine_frame_fills_speed_and_rpm)
{
    primed();
    /* 2701 rpm; 63 km/h is 63 * 128 = 8064 raw. */
    engine(2701U, 8064U);
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(b3()[0], 0x00U);  CHECK_EQ(b3()[1], 0x3FU);   /* 63 km/h */
    CHECK_EQ(b3()[2], 0x0AU);  CHECK_EQ(b3()[3], 0x8DU);   /* 2701    */
    CHECK((validMask() & ram::kValidB3) != 0U);
}

/* The spec rounds rather than truncates: half an LSB of the destination is
 * 64 raw counts. 8063 raw is 62.99 km/h and must read 63, not 62. */
TEST(custom_speed_rounds_rather_than_truncates)
{
    primed();
    engine(1000U, 8063U);
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(b3()[1], 63U);
}

TEST(custom_coolant_is_a_verbatim_byte)
{
    primed();
    coolant(133U);                 /* 133 - 40 = 93 degC */
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(b4()[0], 133U);
    CHECK((validMask() & ram::kValidB4) != 0U);
}

TEST(custom_intake_and_volts_come_from_one_frame)
{
    primed();
    voltsIntake(143U, 45U);        /* 14.3 V, 45 degC */
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(b4()[1], 85U);                              /* 45 + 40     */
    CHECK_EQ(b4()[3], 0x05U);  CHECK_EQ(b4()[4], 0x96U); /* 1430 cV     */
}

/* 77.6 L tank at 0.5 L/count = 155.2 counts, so percent is raw * 0.6443 with a
 * round-half-up. raw 143 -> 71.5 L -> 92 %. */
TEST(custom_fuel_scales_litres_and_percent_from_one_byte)
{
    primed();
    fuel(0U, 143U);
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(b4()[2], 92U);                              /* percent     */
    CHECK_EQ(b8()[0], 0x02U);  CHECK_EQ(b8()[1], 0xCBU); /* 715 dL      */
    CHECK((validMask() & ram::kValidB8) != 0U);
}

/* The percentage saturates rather than wrapping: raw 255 is 164 % unclamped. */
TEST(custom_fuel_percent_clamps_at_one_hundred)
{
    primed();
    fuel(0U, 255U);
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(b4()[2], 100U);
}

TEST(custom_throttle_scales_the_pedal_to_percent)
{
    primed();
    throttle(116U, 116U);          /* (116 - 23) * 100 / 211 = 44 % */
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(b3()[5], 44U);

    /* Rest position is 23 counts, not 0 -- below it the subtraction would
     * wrap and read 97 %. */
    throttle(23U, 23U);
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(b3()[5], 0U);

    throttle(234U, 234U);          /* floored */
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(b3()[5], 100U);
}

TEST(custom_ambient_scales_to_decidegrees)
{
    primed();
    ambient(25U);
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(b5()[0], 0x00U);  CHECK_EQ(b5()[1], 0xFAU);  /* 250 = 25.0 */
    CHECK((validMask() & ram::kValidB5) != 0U);
}

/* 0x0D0 b0 reads 0x04 released and 0x84 engaged. MASK BIT 7 ONLY: the 0x04
 * baseline is not the brake, and testing b0 != 0 would report it permanently
 * engaged on a 2026 car. */
TEST(custom_park_brake_masks_bit_seven_only)
{
    primed();
    parkBrake(0x04U);
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnParkBrake, 0U);

    parkBrake(0x84U);
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnParkBrake, ram::kIgnParkBrake);
}

/* Reverse comes from 0x003 b4, the ASCII PRNDL character. */
TEST(custom_reverse_comes_from_the_ascii_prndl_byte)
{
    primed();
    gear(0x44U, 0x84U);            /* 'D' */
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnReverse, 0U);

    gear(0x52U, 0x81U);            /* 'R' */
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnReverse, ram::kIgnReverse);
}

/* The ACC bit follows the documented 0x000 ladder, and 0xB2 goes live on the
 * first 0x000 frame of any kind. */
TEST(custom_acc_bit_follows_the_ignition_ladder)
{
    primed();
    CHECK_EQ(validMask() & ram::kValidB2, 0U);

    ignition(0x01U);               /* off */
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnAccOn, 0U);
    CHECK_EQ(validMask() & ram::kValidB2, ram::kValidB2);

    ignition(0x63U);               /* ACC */
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnAccOn, ram::kIgnAccOn);

    ignition(0x85U);               /* running */
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnAccOn, ram::kIgnAccOn);

    ignition(0x00U);               /* wake/uninitialised */
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnAccOn, 0U);
}

/* ===========================================================================
 * 4. The sentinels -- section 4 of the spec, "had it, lost it".
 * ========================================================================= */

/* ONE sentinel governs BOTH fields of 0x002: when the engine is off the whole
 * frame goes to FF. Computing 0xFFFF / 128 would publish 512 km/h. */
TEST(custom_engine_all_ff_blanks_speed_and_rpm_and_clears_validity)
{
    primed();
    engine(2000U, 8064U);
    decodeOnce();
    CHECK((validMask() & ram::kValidB3) != 0U);

    engine(0xFFFFU, 0xFFFFU);
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(b3()[0], 0xFFU);  CHECK_EQ(b3()[1], 0xFFU);
    CHECK_EQ(b3()[2], 0xFFU);  CHECK_EQ(b3()[3], 0xFFU);
    CHECK_EQ(validMask() & ram::kValidB3, 0U);
}

/* THE TRAP. 0xFF - 40 = 215 degC is a value the head unit accepts, so a
 * verbatim copy of the unavailable sentinel would render as a plausible
 * temperature. The byte goes out as 0xFF with the block's "no value" meaning
 * and 0xB0 bit 2 is cleared. */
TEST(custom_coolant_all_ff_publishes_the_sentinel_not_215_degrees)
{
    primed();
    coolant(133U);
    decodeOnce();
    CHECK((validMask() & ram::kValidB4) != 0U);

    coolantUnavailable();
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(b4()[0], 0xFFU);
    CHECK_EQ(validMask() & ram::kValidB4, 0U);
}

/* The worked example for exactly that case: one dead sensor must not cost the
 * three good signals sharing 0xB4. The frame is STILL EMITTED, coolant reads
 * the reserved code, and intake / fuel % / volts are untouched. */
TEST(custom_engine_unavailable_coolant_matches_the_worked_example)
{
    primed();
    ram::CustomSignals& c = ram::app().customSignals;
    c.coolantBias40 = 0xFFU;                        /* not available */
    c.intakeBias40  = static_cast<std::uint8_t>(45U + 40U);
    c.fuelPct       = 92U;
    c.centiVolts    = 1430U;
    logic::customPublish();

    std::uint8_t wire[16];
    const std::size_t n = hiworldFrame(wire, 0xB4U, b4(), 5U);
    const std::uint8_t want[10] = { 0xAAU, 0x55U, 0x05U, 0xB4U,
                                    0xFFU,          /* coolant unavailable */
                                    0x55U,          /* 45 degC + 40        */
                                    0x5CU,          /* fuel 92 %           */
                                    0x05U, 0x96U,   /* 14.30 V             */
                                    0x03U };
    CHECK_EQ(n, 10U);
    CHECK_BYTES(wire, want, 10U);
}

/* The reserved code costs exactly one representable value. 0xFE = 214 degC is
 * still a reading and must NOT clear the validity bit -- no coolant or intake
 * on this car gets there, but the boundary is where an off-by-one would hide. */
TEST(custom_coolant_fe_is_a_reading_not_a_sentinel)
{
    primed();
    coolant(0xFEU);
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(b4()[0], 0xFEU);
    CHECK_EQ(validMask() & ram::kValidB4, ram::kValidB4);
}

/* 0x159 b4 is a hard gate: b4 == 1 means b5 = 0xFF is not a reading. Without
 * it, key-off would publish a full tank at raw 255.
 *
 * The gate clears 0xB8's validity bit and NOT 0xB4's -- 0xB4's other three
 * fields (coolant, intake, volts) are still good, so the percentage reports
 * its own loss with its 0xFF instead. */
TEST(custom_fuel_gate_holds_both_destinations_and_clears_only_b8_validity)
{
    primed();
    coolant(133U);                 /* keep 0xB4 otherwise live */
    fuel(0U, 143U);
    decodeOnce();
    logic::customPublish();
    CHECK((validMask() & ram::kValidB8) != 0U);
    CHECK((validMask() & ram::kValidB4) != 0U);
    CHECK_EQ(b4()[2], 92U);

    fuel(1U, 0xFFU);               /* ignition off: b5 is not a reading */
    decodeOnce();
    logic::customPublish();

    /* HELD, not blanked -- a parked car's tank does not change, so the last
     * reading is the true one. Without the gate this would have published a
     * full tank at raw 255. */
    CHECK_EQ(b4()[2], 92U);
    CHECK_EQ(b8()[0], 0x02U);  CHECK_EQ(b8()[1], 0xCBU);

    /* ...and 0xB0 bit 5 is what tells a consumer it is no longer refreshed,
     * while 0xB4's bit stays set because coolant, intake and volts are good. */
    CHECK_EQ(validMask() & ram::kValidB8, 0U);
    CHECK_EQ(validMask() & ram::kValidB4, ram::kValidB4);
}

/* Before the FIRST reading there is nothing to hold, so the primed sentinels
 * must stand: a cold boot says "unknown", not "empty". */
TEST(custom_fuel_gate_before_any_reading_leaves_the_sentinels)
{
    primed();
    fuel(1U, 0xFFU);
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(b4()[2], 0xFFU);
    CHECK_EQ(b8()[0], 0xFFU);  CHECK_EQ(b8()[1], 0xFFU);
    CHECK_EQ(validMask() & ram::kValidB8, 0U);
}

/* b6 mirrors b2 in all 14 457 frames of drive.log, so a mismatch is a corrupt
 * frame and the sample is discarded rather than published. */
TEST(custom_throttle_mismatched_mirror_holds_the_previous_value)
{
    primed();
    throttle(116U, 116U);
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(b3()[5], 44U);

    throttle(234U, 0x00U);         /* b6 disagrees */
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(b3()[5], 44U);        /* unchanged */
}

/* The car's negative ambient encoding has never been captured, so a byte that
 * looks like a two's-complement negative is suspect, not cold. Hold and blank
 * rather than guess the sign convention. */
TEST(custom_ambient_suspect_byte_holds_and_clears_validity)
{
    primed();
    ambient(25U);
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(b5()[1], 0xFAU);
    CHECK((validMask() & ram::kValidB5) != 0U);

    ambient(0x90U);
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(b5()[0], 0x00U);  CHECK_EQ(b5()[1], 0xFAU);  /* held */
    CHECK_EQ(validMask() & ram::kValidB5, 0U);
}

/* INTAKE IS CLAMPED BEFORE BIASING, never computed blindly: 0xFF + 40 truncates
 * to 0x27, which decodes as -1 degC -- a plausible-looking lie and the exact
 * trap the reserved code exists to close. Unavailable or suspect publishes
 * 0xFF; it does NOT hold, because an unavailable sensor must read "unknown"
 * rather than "whatever it said last".
 *
 * Intake shares 0xB4's validity bit with coolant and does not clear it: one
 * dead sensor must not cost the three good signals in this command. */
TEST(custom_intake_unavailable_publishes_the_reserved_code)
{
    primed();
    coolant(133U);                 /* keep 0xB4 live */
    voltsIntake(143U, 45U);
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(b4()[1], 85U);

    voltsIntake(143U, 0xFFU);      /* unavailable */
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(b4()[1], 0xFFU);                            /* NOT 0x27 / -1 degC */
    CHECK_EQ(validMask() & ram::kValidB4, ram::kValidB4); /* coolant still good */

    /* Anything from 0x80 up is rejected the same way: a two's-complement
     * negative has never been observed on this car, and from 0xD7 the bias
     * would run into the reserved code anyway. */
    voltsIntake(143U, 0x80U);
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(b4()[1], 0xFFU);

    /* 0xD6 is the last byte that still biases below the reserved code. */
    voltsIntake(143U, 45U);
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(b4()[1], 85U);
}

/* ===========================================================================
 * 5. The engine-running state machine -- section 5 of the spec.
 *
 * Neither rpm nor the ignition byte works alone: crank (0x000 b0 == 0xA9)
 * reaches 410 rpm and run (0x85) also shows 0. The 300/500 band sits between
 * crank-max 410 and idle-min 577.
 * ========================================================================= */

TEST(custom_engine_running_needs_three_consecutive_frames)
{
    primed();
    ignition(0x85U);
    decodeOnce();

    engine(600U, 0U);
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnEngineRun, 0U);   /* first  */

    engine(600U, 0U);
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnEngineRun, 0U);   /* second */

    engine(600U, 0U);
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnEngineRun,
             ram::kIgnEngineRun);                   /* third  */
}

/* Cranking VETOES "running" however high the rpm goes: a crank reaching 410
 * would otherwise trip a naive threshold three frames in. */
TEST(custom_engine_running_stays_clear_while_cranking)
{
    primed();
    ignition(0xA9U);               /* crank */
    decodeOnce();

    for (int i = 0; i < 5; ++i) {
        engine(410U, 0U);
        decodeOnce();
    }
    CHECK_EQ(ignByte() & ram::kIgnEngineRun, 0U);

    /* Even above the set threshold, the crank latch wins. */
    for (int i = 0; i < 5; ++i) {
        engine(600U, 0U);
        decodeOnce();
    }
    CHECK_EQ(ignByte() & ram::kIgnEngineRun, 0U);
}

/* Asymmetric on purpose: slow to claim the engine is running, instant to give
 * up. A false "running" shows a live engine on a dead one. */
TEST(custom_engine_running_clears_on_the_first_bad_frame)
{
    primed();
    ignition(0x85U);
    decodeOnce();
    for (int i = 0; i < 3; ++i) {
        engine(600U, 0U);
        decodeOnce();
    }
    CHECK_EQ(ignByte() & ram::kIgnEngineRun, ram::kIgnEngineRun);

    engine(0xFFFFU, 0xFFFFU);      /* shutdown */
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnEngineRun, 0U);
}

/* The band between 300 and 500 is neither set nor clear: the bit HOLDS. That
 * is the hysteresis, and without it a diesel settling onto idle would flicker.
 */
TEST(custom_engine_running_holds_inside_the_hysteresis_band)
{
    primed();
    ignition(0x85U);
    decodeOnce();
    for (int i = 0; i < 3; ++i) {
        engine(600U, 0U);
        decodeOnce();
    }
    CHECK_EQ(ignByte() & ram::kIgnEngineRun, ram::kIgnEngineRun);

    engine(400U, 0U);              /* inside the band */
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnEngineRun, ram::kIgnEngineRun);

    engine(299U, 0U);              /* below the clear threshold */
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnEngineRun, 0U);

    /* ...and coming back up it needs the full three frames again, not one. */
    engine(600U, 0U);
    decodeOnce();
    CHECK_EQ(ignByte() & ram::kIgnEngineRun, 0U);
}

/* ===========================================================================
 * 6. The heartbeat -- the one thing in this block that needs the scheduler.
 * ========================================================================= */

/* 0xB0's payload may be identical for hours, so the change-driven round-robin
 * would only send it on the ~8.6 s keep-alive sweep. The publisher marks the
 * slot for an explicit send once a second instead. */
TEST(custom_heartbeat_requests_its_slot_once_a_second)
{
    primed();

    /* 124 passes: not yet. */
    for (int i = 0; i < 124; ++i) {
        logic::customPublish();
    }
    bool anyRequest = false;
    for (std::uint8_t i = 0U; i < svc::hu::kTxSlots; ++i) {
        if (ram::app().uartSlotState[i] == svc::hu::kSlotRequest) {
            anyRequest = true;
        }
    }
    CHECK_FALSE(anyRequest);

    /* The 125th pass is 1000 ms. */
    logic::customPublish();

    std::uint8_t requested = svc::hu::kTxSlots;
    for (std::uint8_t i = 0U; i < svc::hu::kTxSlots; ++i) {
        if (ram::app().uartSlotState[i] == svc::hu::kSlotRequest) {
            requested = i;
        }
    }
    CHECK(requested < svc::hu::kTxSlots);
}

/* requestSend() must find the row by COMMAND, not by a hardcoded index, or a
 * reordered table silently requests the wrong message. */
TEST(custom_request_send_marks_the_slot_carrying_that_command)
{
    fake::reset();
    svc::hu::resetState();

    svc::hu::requestSend(0xB0U);

    std::uint8_t found = svc::hu::kTxSlots;
    for (std::uint8_t i = 0U; i < svc::hu::kTxSlots; ++i) {
        if (ram::app().uartSlotState[i] == svc::hu::kSlotRequest) {
            found = i;
        }
    }
    CHECK(found < svc::hu::kTxSlots);

    /* An unknown command is a silent no-op -- the caller is protocol-agnostic
     * and a benz image genuinely has no 0xB0 row. */
    svc::hu::resetState();
    svc::hu::requestSend(0x01U);
    for (std::uint8_t i = 0U; i < svc::hu::kTxSlots; ++i) {
        CHECK_EQ(ram::app().uartSlotState[i], svc::hu::kSlotIdle);
    }
}

/* THE REASON THE SCHEDULER NEEDED A CHANGE, pinned so it cannot regress.
 *
 * On an idle link a sweep that finds nothing changed parks the index on the
 * kTxSlots sentinel; the next pass clamps it back to 0 AND clears
 * slotState[0] on the way. So a request on slot 0 was wiped before it could
 * be tested, and a request on any other slot was never reached, because the
 * index is reset to 0 on every pass. The state was unreachable as written.
 *
 * This sets up the exact idle state and arranges for the KEEP-ALIVE to want a
 * different slot: the auxiliary index is parked on 0, so without the request
 * the frame that leaves is nextSlot(0) = slot 1 = 0xA6. With it, 0xB0. The two
 * outcomes are distinguishable, which the earlier version of this test was
 * not -- it happened to find the aux index already sitting on the heartbeat.
 */
TEST(custom_heartbeat_request_is_honoured_on_an_idle_link)
{
    fake::reset();
    svc::hu::resetState();
    logic::customPrimeFrames();

    /* Settle: sweep until every shadow matches its payload. 6 passes is one
     * 48 ms frame opportunity, and 12 slots need at most 12 of those. */
    for (int i = 0; i < 6 * 40; ++i) {
        svc::hu::service();
    }

    /* The idle-link state, made explicit rather than inferred: nothing in
     * flight, the index parked on the sentinel, every slot idle, and the
     * keep-alive armed and pointing somewhere that is NOT the heartbeat. */
    ram::AppRam& r = ram::app();
    r.uartFrame[svc::hu::kFbTxBusy] = 0U;
    r.uartTxTick     = 0U;
    r.uartTxSlotIdx  = svc::hu::kTxSlots;
    r.uartAuxSlotIdx = 0U;          /* keep-alive would build slot 1 = 0xA6 */
    r.uartIdleDelay  = 0U;          /* ...and it is due this pass           */
    for (std::uint8_t i = 0U; i < svc::hu::kTxSlots; ++i) {
        r.uartSlotState[i] = svc::hu::kSlotIdle;
    }
    for (std::size_t i = 0U; i < 8U; ++i) {
        r.uartFrame[i] = 0U;        /* so a stale frame cannot pass for new */
    }

    svc::hu::requestSend(0xB0U);
    for (int i = 0; i < 6; ++i) {   /* one 48 ms frame opportunity */
        svc::hu::service();
    }

    /* The frame in the buffer is the heartbeat, AA 55 02 B0 .., and NOT the
     * 0xA6 the keep-alive would otherwise have sent. */
    CHECK_EQ(r.uartFrame[0], 0xAAU);
    CHECK_EQ(r.uartFrame[1], 0x55U);
    CHECK_EQ(r.uartFrame[2], 0x02U);
    CHECK_EQ(r.uartFrame[3], 0xB0U);
}

/* The request is one-shot: once served the slot drops back out of the request
 * state, or the heartbeat would claim a frame slot every 48 ms for ever. */
TEST(custom_heartbeat_request_is_consumed_once)
{
    fake::reset();
    svc::hu::resetState();
    logic::customPrimeFrames();

    for (int i = 0; i < 6 * 40; ++i) {
        svc::hu::service();
    }

    ram::AppRam& r = ram::app();
    r.uartFrame[svc::hu::kFbTxBusy] = 0U;
    r.uartTxTick    = 0U;
    r.uartTxSlotIdx = svc::hu::kTxSlots;

    svc::hu::requestSend(0xB0U);
    for (int i = 0; i < 6; ++i) {
        svc::hu::service();
    }

    for (std::uint8_t i = 0U; i < svc::hu::kTxSlots; ++i) {
        CHECK(r.uartSlotState[i] != svc::hu::kSlotRequest);
    }
}

/* ===========================================================================
 * 8. Addendum A -- 0xBB ODOMETER and 0xBD COMPASS, spec section 9.
 * ========================================================================= */

TEST(custom_odometer_matches_the_worked_example)
{
    primed();
    ram::app().customSignals.odometerDeciKm = 0x12D687U;   /* 123 456.7 km */
    logic::customPublish();

    std::uint8_t wire[16];
    const std::size_t n = hiworldFrame(wire, 0xBBU, bB(), 3U);
    const std::uint8_t want[8] = { 0xAAU, 0x55U, 0x03U, 0xBBU,
                                   0x12U, 0xD6U, 0x87U, 0x2CU };
    CHECK_EQ(n, 8U);
    CHECK_BYTES(wire, want, 8U);
}

TEST(custom_odometer_unavailable_matches_the_worked_example)
{
    primed();
    logic::customPublish();

    std::uint8_t wire[16];
    hiworldFrame(wire, 0xBBU, bB(), 3U);
    const std::uint8_t want[8] = { 0xAAU, 0x55U, 0x03U, 0xBBU,
                                   0xFFU, 0xFFU, 0xFFU, 0xBAU };
    CHECK_BYTES(wire, want, 8U);
}

/* The real 1B4 payload 06 85 07 00 00 00: sector 6 = W, heading 133 * 2. */
TEST(custom_compass_matches_the_worked_example)
{
    primed();
    compass(0x06U, 0x85U);
    decodeOnce();
    logic::customPublish();

    std::uint8_t wire[16];
    const std::size_t n = hiworldFrame(wire, 0xBDU, bD(), 3U);
    const std::uint8_t want[8] = { 0xAAU, 0x55U, 0x03U, 0xBDU,
                                   0x06U,          /* W            */
                                   0x01U, 0x0AU,   /* 266 degrees  */
                                   0xD0U };
    CHECK_EQ(n, 8U);
    CHECK_BYTES(wire, want, 8U);
}

/* 0 IS A REAL HEADING -- due north -- not a blank. If this ever starts
 * reading as the sentinel, a consumer loses north. */
TEST(custom_compass_north_zero_is_a_reading_not_a_blank)
{
    primed();
    compass(0x00U, 0x00U);
    decodeOnce();
    logic::customPublish();

    std::uint8_t wire[16];
    hiworldFrame(wire, 0xBDU, bD(), 3U);
    const std::uint8_t want[8] = { 0xAAU, 0x55U, 0x03U, 0xBDU,
                                   0x00U, 0x00U, 0x00U, 0xBFU };
    CHECK_BYTES(wire, want, 8U);
    CHECK_EQ(validMask() & ram::kValidBD, ram::kValidBD);
}

/* North at 358 degrees: sector and heading legitimately look inconsistent at
 * the wrap, and both are forwarded as the car sent them. */
TEST(custom_compass_wrap_sends_both_bytes_as_the_car_sent_them)
{
    primed();
    compass(0x00U, 179U);          /* 179 * 2 = 358 */
    decodeOnce();
    logic::customPublish();

    std::uint8_t wire[16];
    hiworldFrame(wire, 0xBDU, bD(), 3U);
    const std::uint8_t want[8] = { 0xAAU, 0x55U, 0x03U, 0xBDU,
                                   0x00U, 0x01U, 0x66U, 0x26U };
    CHECK_BYTES(wire, want, 8U);
}

TEST(custom_compass_unavailable_matches_the_worked_example)
{
    primed();
    logic::customPublish();

    std::uint8_t wire[16];
    hiworldFrame(wire, 0xBDU, bD(), 3U);
    const std::uint8_t want[8] = { 0xAAU, 0x55U, 0x03U, 0xBDU,
                                   0xFFU, 0xFFU, 0xFFU, 0xBCU };
    CHECK_BYTES(wire, want, 8U);
}

/* The mask is now full: 0x2F | 0x40 | 0x80 = 0xEF. */
TEST(custom_heartbeat_with_all_eight_commands_live)
{
    primed();
    ram::app().customSignals.valid = static_cast<std::uint8_t>(
        ram::kValidB2 | ram::kValidB3 | ram::kValidB4 | ram::kValidB5
        | ram::kValidB8 | ram::kValidBB | ram::kValidBD);
    logic::customPublish();

    std::uint8_t wire[8];
    hiworldFrame(wire, 0xB0U, b0(), 2U);
    const std::uint8_t want[7] = { 0xAAU, 0x55U, 0x02U, 0xB0U,
                                   0x01U, 0xEFU, 0xA1U };
    CHECK_BYTES(wire, want, 7U);
}

/* ---- the two gates on 0x014 are independent, proved in both directions --- */

TEST(custom_odometer_unavailable_does_not_disturb_ambient)
{
    primed();
    trip(0x12D687U, 25U);
    decodeOnce();
    logic::customPublish();
    CHECK((validMask() & ram::kValidBB) != 0U);
    CHECK((validMask() & ram::kValidB5) != 0U);

    trip(0x00FFFFFFU, 25U);        /* odometer unavailable, ambient fine */
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(bB()[0], 0xFFU);  CHECK_EQ(bB()[1], 0xFFU);  CHECK_EQ(bB()[2], 0xFFU);
    CHECK_EQ(validMask() & ram::kValidBB, 0U);
    /* Ambient has its own gate on the same frame and is untouched. */
    CHECK_EQ(bD()[0], 0xFFU);                       /* compass also untouched */
    CHECK_EQ(b5()[1], 0xFAU);
    CHECK_EQ(validMask() & ram::kValidB5, ram::kValidB5);
}

TEST(custom_suspect_ambient_does_not_disturb_the_odometer)
{
    primed();
    trip(0x12D687U, 25U);
    decodeOnce();
    logic::customPublish();

    trip(0x12D688U, 0x90U);        /* ambient suspect, odometer still moving */
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(bB()[2], 0x88U);                            /* odometer advanced */
    CHECK_EQ(validMask() & ram::kValidBB, ram::kValidBB);
    CHECK_EQ(b5()[1], 0xFAU);                            /* ambient held      */
    CHECK_EQ(validMask() & ram::kValidB5, 0U);
}

/* The odometer keeps its tenths. benzTrip() divides by 10 because the Benz
 * field is whole km; this block has no such constraint and dropping the digit
 * would cost a consumer the ability to measure a 100 m leg. */
TEST(custom_odometer_keeps_tenths_of_a_kilometre)
{
    primed();
    trip(0x12D687U, 25U);
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(bB()[2], 0x87U);

    trip(0x12D688U, 25U);          /* +100 m */
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(bB()[2], 0x88U);
}

/* ---- the compass plausibility gate --------------------------------------- */

/* b1 == 0xFF is 255 counts = 510 degrees, which is not a heading. Reject the
 * sample whole and hold -- NOT publish 0x01FE. */
TEST(custom_compass_out_of_range_heading_holds_both_values)
{
    primed();
    compass(0x06U, 0x85U);
    decodeOnce();
    logic::customPublish();
    CHECK_EQ(bD()[1], 0x01U);  CHECK_EQ(bD()[2], 0x0AU);

    compass(0x06U, 0xFFU);
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(bD()[0], 0x06U);                      /* sector held */
    CHECK_EQ(bD()[1], 0x01U);  CHECK_EQ(bD()[2], 0x0AU);  /* heading held */
    CHECK_EQ(validMask() & ram::kValidBD, 0U);
}

/* A sector of 9 means the byte no longer means what the decoder thinks.
 * Reject the sample WHOLE -- masking it to 0x01 would publish a confident
 * "NE". Nothing is masked and the heading is not taken either. */
TEST(custom_compass_out_of_range_sector_rejects_the_whole_sample)
{
    primed();
    compass(0x06U, 0x85U);
    decodeOnce();
    logic::customPublish();

    compass(0x09U, 0x20U);         /* bad sector, plausible heading */
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(bD()[0], 0x06U);                             /* not 0x09, not 0x01 */
    CHECK_EQ(bD()[1], 0x01U);  CHECK_EQ(bD()[2], 0x0AU);  /* heading not taken  */
    CHECK_EQ(validMask() & ram::kValidBD, 0U);
}

/* THE DOCUMENTED BOUNDARY DISAGREEMENT, and it must be ACCEPTED. b1 = 146 is
 * 292 degrees, half a degree shy of the 292.5 degree NW boundary, so the
 * round(b1*2/45) formula says 6/W while the car says 7/NW. The car wins and
 * nothing is discarded -- this is why b0 is not cross-checked against b1 the
 * way 2C0 b2 is against b6. */
TEST(custom_compass_boundary_disagreement_is_accepted_not_discarded)
{
    primed();
    compass(0x07U, 146U);
    decodeOnce();
    logic::customPublish();

    CHECK_EQ(bD()[0], 0x07U);                       /* the car's sector, kept */
    CHECK_EQ(bD()[1], 0x01U);  CHECK_EQ(bD()[2], 0x24U);  /* 292 degrees */
    CHECK_EQ(validMask() & ram::kValidBD, ram::kValidBD);
}

/* 179 is the last legal count; 180 is 360 degrees and is not. */
TEST(custom_compass_heading_boundary_is_one_seventy_nine)
{
    primed();
    compass(0x00U, 179U);
    decodeOnce();
    CHECK_EQ(validMask() & ram::kValidBD, ram::kValidBD);

    primed();
    compass(0x00U, 180U);
    decodeOnce();
    CHECK_EQ(validMask() & ram::kValidBD, 0U);
}

/* Cold boot: neither frame has arrived, so both read as unavailable and both
 * bits are 0. */
TEST(custom_cold_boot_leaves_odometer_and_compass_unavailable)
{
    primed();
    logic::customPublish();

    CHECK_EQ(bB()[0], 0xFFU);  CHECK_EQ(bB()[1], 0xFFU);  CHECK_EQ(bB()[2], 0xFFU);
    CHECK_EQ(bD()[0], 0xFFU);  CHECK_EQ(bD()[1], 0xFFU);  CHECK_EQ(bD()[2], 0xFFU);
    CHECK_EQ(validMask() & ram::kValidBB, 0U);
    CHECK_EQ(validMask() & ram::kValidBD, 0U);
}

/* ===========================================================================
 * 7. The stock slots are untouched.
 *
 * The whole point of keeping the block in 0xB0-0xBF is that a jeep image is
 * still stock-equivalent for the six original messages. If one of those moved
 * or changed length, the Android plugin would misparse it.
 * ========================================================================= */
TEST(custom_block_leaves_the_six_stock_slots_alone)
{
    primed();

    /* Stock 0x73 is eight zero bytes for ever, and 0x12 still carries the
     * doors at payload byte 2 -- which is why 0xB6 BODY is not built. */
    for (std::size_t i = 0U; i < sizeof(ram::AppRam::buf73); ++i) {
        CHECK_EQ(ram::app().buf73[i], 0U);
    }

    ram::app().vehicle.setDoorBit(ram::VehicleState::kDoorLeftFront, true);
    const volatile std::uint8_t* win12 =
        ram::app().vehicle.window(ram::VehicleState::kWin12Off);
    CHECK_EQ(win12[2], ram::VehicleState::kDoorLeftFront);

    /* Stock 0x32 still carries rpm at payload [2..3] -- which is why 0xB3
     * keeping rpm is a deliberate duplicate, not an oversight. */
    ram::app().vehicle.setRpm(0x0AU, 0x8DU);
    const volatile std::uint8_t* win32 =
        ram::app().vehicle.window(ram::VehicleState::kWin32Off);
    CHECK_EQ(win32[2], 0x0AU);
    CHECK_EQ(win32[3], 0x8DU);
}
