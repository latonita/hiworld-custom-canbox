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

/* ---- TPMS ---------------------------------------------------------------
 * The WCM's answer on 0x500: 06 61 3B p1 p2 p3 p4 <junk>. The trailing byte is
 * deliberately NOT zero in these builders -- it is whatever the responder had
 * in its buffer last, and a decoder that reads it as a fifth pressure must
 * fail a test rather than ship. */
void tpmsReply(std::uint8_t lf, std::uint8_t rf,
               std::uint8_t rr, std::uint8_t lr) noexcept
{
    const std::uint8_t d[8] = { 0x06U, 0x61U, 0x3BU, lf, rf, rr, lr, 0xD2U };
    pushFrame(logic::canid::kJeepVendorEcho, d);
}

/* An arbitrary 0x500 frame that is not a well-formed answer. */
void raw500(std::uint8_t b0, std::uint8_t b1, std::uint8_t b2) noexcept
{
    const std::uint8_t d[8] = { b0, b1, b2, 0U, 0U, 0U, 0U, 0U };
    pushFrame(logic::canid::kJeepVendorEcho, d);
}

/* Bring the box to the state in which a poll is allowed: bus running, and the
 * poll divider wound to the point where the next pass fires a request. */
void armPoll() noexcept
{
    ram::app().canBusStatus = cfg::kBusRunning;
    ram::app().customSignals.tpmsPollDiv = 3749U;   /* kTpmsPollPasses - 1 */
}

/* One 8 ms pass of the poll state machine, plus the 4 ms task's drain --
 * txEnqueue() only fills the ring; txDrain() is what hands a frame to a
 * mailbox and so to fake::lastCanFrame(). */
void pollPass() noexcept
{
    logic::customTpmsPoll();
    svc::can::txDrain();
}

/* Drive the state machine until the outstanding request times out. */
void runOutDeadline() noexcept
{
    for (int i = 0; i < 70; ++i) {
        logic::customTpmsPoll();
    }
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
const volatile std::uint8_t* b7() noexcept { return ram::app().customFrames.b7; }
const volatile std::uint8_t* bB() noexcept { return ram::app().customFrames.bB; }
const volatile std::uint8_t* bD() noexcept { return ram::app().customFrames.bD; }

std::uint8_t validMask() noexcept { return ram::app().customSignals.valid; }
std::uint8_t ignByte() noexcept   { return ram::app().customSignals.ignBits; }

}  // namespace

/* ===========================================================================
 * 0. Configuration sanity -- the five couplings section 6 of the spec warns
 *    about, four of which fail silently.
 * ========================================================================= */
TEST(custom_jeep_build_has_sixteen_tx_slots)
{
    CHECK_FALSE(cfg::kIsBenz);
    CHECK_EQ(static_cast<unsigned>(cfg::kTxSlotCount), 16U);
    CHECK_EQ(static_cast<unsigned>(svc::hu::kTxSlots), 16U);
    /* The slot-state array is sized from cfg::kTxSlotCount and is immediately
     * followed by uartTxSlotIdx: a short array means the scheduler corrupts
     * its own index every sweep and the tests keep passing. */
    CHECK_EQ(sizeof(ram::AppRam::uartSlotState), 16U);
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
    CHECK_EQ(worstCaseMs, 768U);
    CHECK_EQ(keepAliveMs, 11520U);     /* 16 x 720 ms = 11.52 s */
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

    /* 0xBC: a zeroed field would claim "0.0 L/h", a real overrun reading the
     * car never sent. The 0xFFFF sentinel is the only absent signal. */
    CHECK_EQ(ram::app().customFrames.bC[0], 0xFFU);
    CHECK_EQ(ram::app().customFrames.bC[1], 0xFFU);
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
 * 9. 0xB7 TYRE -- polling the WCM. docs/TPMS-POLL-SPEC.md.
 * ========================================================================= */

/* THE REQUEST. The PDU is `21 3B`, NOT `21 21 3B` -- a builder that re-emits
 * the service byte for a service-qualified identifier makes every module on
 * this bus answer nothing. DLC 3, unpadded. */
TEST(tpms_poll_sends_the_kwp_request_on_0x600)
{
    primed();
    armPoll();
    pollPass();

    const fake::CanFrame* f = fake::lastCanFrame();
    CHECK(f != nullptr);
    if (f != nullptr) {
        CHECK_EQ(f->id, 0x600U);
        CHECK_EQ(f->dlc, 3U);
        CHECK_EQ(f->data[0], 0x02U);   /* ISO-TP single frame, 2 PDU bytes */
        CHECK_EQ(f->data[1], 0x21U);   /* readDataByLocalIdentifier        */
        CHECK_EQ(f->data[2], 0x3BU);   /* ...and NOT a second 0x21 here    */
    }
}

/* THE BUS-PRESENCE GATE. abom = 0 and nart = 0 mean an unACKed frame is
 * retried for ever and takes the controller bus-off permanently. On a bench
 * with no second node that is exactly what a poll would cause, so the poll
 * must not fire unless the bus is known alive. */
TEST(tpms_poll_is_silent_unless_the_bus_is_running)
{
    primed();
    armPoll();
    ram::app().canBusStatus = cfg::kBusDown;
    pollPass();
    CHECK_EQ(fake::canFrameCount(), 0U);

    primed();
    armPoll();
    ram::app().canBusStatus = cfg::kBusSettling;
    pollPass();
    CHECK_EQ(fake::canFrameCount(), 0U);

    primed();
    armPoll();
    pollPass();                       /* kBusRunning */
    CHECK_EQ(fake::canFrameCount(), 1U);
}

/* Only one request in flight at a time. */
TEST(tpms_poll_does_not_stack_requests)
{
    primed();
    armPoll();
    pollPass();
    CHECK_EQ(fake::canFrameCount(), 1U);

    armPoll();                        /* wind the divider again */
    pollPass();
    CHECK_EQ(fake::canFrameCount(), 1U);   /* still one: a reply is pending */
}

/* THE WORKED EXAMPLE: the real captured reply 06 61 3B 21 20 21 1E, decoded
 * into the four corners and packed as the head unit expects them. */
TEST(tpms_reply_decodes_the_captured_frame)
{
    primed();
    armPoll();
    pollPass();

    tpmsReply(0x21U, 0x20U, 0x21U, 0x1EU);   /* 33, 32, 33, 30 psi */
    decodeOnce();

    const ram::CustomSignals& c = ram::app().customSignals;
    CHECK_EQ(c.tyreKpa[ram::kTyrePosLeftFront],  228U);
    CHECK_EQ(c.tyreKpa[ram::kTyrePosRightFront], 221U);
    CHECK_EQ(c.tyreKpa[ram::kTyrePosRightRear],  228U);
    CHECK_EQ(c.tyreKpa[ram::kTyrePosLeftRear],   207U);
    CHECK_EQ(validMask() & ram::kValidB7, ram::kValidB7);
}

/* ...and on the wire, one corner per frame. These are the four section-6
 * vectors. */
TEST(tpms_frames_match_the_worked_examples)
{
    static const std::uint8_t want[4][10] = {
        { 0xAAU, 0x55U, 0x05U, 0xB7U, 0x00U, 0x00U, 0xE4U, 0xFFU, 0x00U, 0x9EU },
        { 0xAAU, 0x55U, 0x05U, 0xB7U, 0x01U, 0x00U, 0xDDU, 0xFFU, 0x00U, 0x98U },
        { 0xAAU, 0x55U, 0x05U, 0xB7U, 0x02U, 0x00U, 0xE4U, 0xFFU, 0x00U, 0xA0U },
        { 0xAAU, 0x55U, 0x05U, 0xB7U, 0x03U, 0x00U, 0xCFU, 0xFFU, 0x00U, 0x8CU },
    };

    primed();
    armPoll();
    pollPass();
    tpmsReply(0x21U, 0x20U, 0x21U, 0x1EU);
    decodeOnce();

    for (std::uint8_t pos = 0U; pos < ram::kTyreCount; ++pos) {
        ram::app().customSignals.tpmsPosition = pos;
        logic::customPublish();

        std::uint8_t wire[16];
        const std::size_t n = hiworldFrame(wire, 0xB7U, b7(), 5U);
        CHECK_EQ(n, 10U);
        CHECK_BYTES(wire, want[pos], 10U);
    }
}

/* TRUST THE PCI NIBBLE, NOT THE DLC. The byte after the declared length of 6
 * is stale buffer content; reading it as a fifth pressure would publish
 * garbage. Only four corners exist and the junk byte is never consulted. */
TEST(tpms_reply_ignores_the_byte_past_the_declared_length)
{
    primed();
    armPoll();
    pollPass();
    tpmsReply(0x21U, 0x20U, 0x21U, 0x1EU);   /* trailing byte is 0xD2 */
    decodeOnce();

    /* 0xD2 would be 210 psi = 1448 kPa if it were ever read as a pressure. */
    const ram::CustomSignals& c = ram::app().customSignals;
    for (std::uint8_t i = 0U; i < ram::kTyreCount; ++i) {
        CHECK(c.tyreKpa[i] != 1448U);
    }
}

/* A malformed 0x500 leaves the poll open and publishes nothing. */
TEST(tpms_reply_rejects_a_frame_that_is_not_an_answer)
{
    primed();
    armPoll();
    pollPass();

    raw500(0x06U, 0x61U, 0x40U);      /* right service, WRONG identifier */
    decodeOnce();
    CHECK_EQ(validMask() & ram::kValidB7, 0U);
    CHECK_EQ(ram::app().customSignals.tpmsPending, 1U);   /* still waiting */

    tpmsReply(0x21U, 0x20U, 0x21U, 0x1EU);
    decodeOnce();
    CHECK_EQ(validMask() & ram::kValidB7, ram::kValidB7);
}

/* A reply with no poll outstanding is ignored -- and, crucially, NOTHING is
 * transmitted: the stock 0x500 -> 0x516 mirror is off in a jeep build. */
TEST(tpms_reply_without_an_outstanding_poll_is_ignored_and_silent)
{
    primed();
    ram::app().canBusStatus = cfg::kBusRunning;

    tpmsReply(0x21U, 0x20U, 0x21U, 0x1EU);
    decodeOnce();

    CHECK_EQ(validMask() & ram::kValidB7, 0U);
    CHECK_EQ(fake::canFrameCount(), 0U);
}

/* A negative response closes the request and counts as a failure, but carries
 * no data and must not be echoed either. */
TEST(tpms_negative_response_is_consumed_and_counted)
{
    primed();
    armPoll();
    pollPass();
    const unsigned before = fake::canFrameCount();

    raw500(0x03U, 0x7FU, 0x21U);      /* 7F 21 xx */
    decodeOnce();

    CHECK_EQ(ram::app().customSignals.tpmsPending, 0U);
    CHECK_EQ(validMask() & ram::kValidB7, 0U);
    CHECK_EQ(fake::canFrameCount(), before);   /* nothing transmitted */
}

/* Four consecutive misses -- two minutes -- blank every corner, raise the
 * no-signal flag and clear the validity bit. */
TEST(tpms_goes_stale_after_four_missed_polls)
{
    primed();
    armPoll();
    pollPass();
    tpmsReply(0x21U, 0x20U, 0x21U, 0x1EU);
    decodeOnce();
    CHECK_EQ(validMask() & ram::kValidB7, ram::kValidB7);

    for (int miss = 0; miss < 4; ++miss) {
        armPoll();
        pollPass();                   /* request goes out */
        runOutDeadline();             /* ...and times out */
    }

    logic::customPublish();
    CHECK_EQ(validMask() & ram::kValidB7, 0U);
    CHECK_EQ(b7()[1], 0xFFU);  CHECK_EQ(b7()[2], 0xFFU);
    CHECK_EQ(b7()[4], ram::kTyreFlagNoSignal);
}

/* Three misses is not yet stale -- the readings stand. */
TEST(tpms_survives_three_missed_polls)
{
    primed();
    armPoll();
    pollPass();
    tpmsReply(0x21U, 0x20U, 0x21U, 0x1EU);
    decodeOnce();

    for (int miss = 0; miss < 3; ++miss) {
        armPoll();
        pollPass();
        runOutDeadline();
    }

    CHECK_EQ(validMask() & ram::kValidB7, ram::kValidB7);
    CHECK_EQ(ram::app().customSignals.tyreKpa[0], 228U);
}

/* Before any answer at all: sentinels, no-signal flag, bit 4 clear. The frame
 * still goes out -- the scheduler cannot be told to skip a slot -- so every
 * field has to be honest on its own. */
TEST(tpms_before_the_first_answer_publishes_no_value)
{
    primed();
    logic::customPublish();

    std::uint8_t wire[16];
    hiworldFrame(wire, 0xB7U, b7(), 5U);
    const std::uint8_t want[10] = { 0xAAU, 0x55U, 0x05U, 0xB7U,
                                    0x00U, 0xFFU, 0xFFU, 0xFFU, 0x04U, 0xBCU };
    CHECK_BYTES(wire, want, 10U);
    CHECK_EQ(validMask() & ram::kValidB7, 0U);
}

/* The position byte rotates 0->1->2->3->0 and never reaches 4: the spare is
 * deferred, and publishing a corner the box never fills would show a
 * permanently dead fifth tyre. */
TEST(tpms_position_rotates_through_four_corners_only)
{
    primed();
    ram::app().canBusStatus = cfg::kBusRunning;

    std::uint8_t seen[6] = {0U, 0U, 0U, 0U, 0U, 0U};
    std::uint8_t prev = ram::app().customSignals.tpmsPosition;
    seen[prev] = 1U;

    /* 625 passes per step, so five steps is 3125 passes. */
    for (int i = 0; i < 3200; ++i) {
        pollPass();
        const std::uint8_t pos = ram::app().customSignals.tpmsPosition;
        CHECK(pos < ram::kTyreCount);
        if (pos != prev) {
            CHECK_EQ(pos, static_cast<std::uint8_t>((prev + 1U) % ram::kTyreCount));
            seen[pos] = 1U;
            prev = pos;
        }
    }
    for (std::uint8_t i = 0U; i < ram::kTyreCount; ++i) {
        CHECK_EQ(seen[i], 1U);
    }
    CHECK_EQ(seen[ram::kTyrePosSpare], 0U);
}

/* psi -> kPa rounds rather than truncating: 33 psi is 227.5 kPa. */
TEST(tpms_pressure_converts_psi_to_kilopascal_rounded)
{
    primed();
    armPoll();
    pollPass();
    tpmsReply(33U, 1U, 0U, 255U);
    decodeOnce();

    const ram::CustomSignals& c = ram::app().customSignals;
    CHECK_EQ(c.tyreKpa[0], 228U);     /* 227.49 -> 228, not 227 */
    CHECK_EQ(c.tyreKpa[1], 7U);       /* 6.89   -> 7            */
    CHECK_EQ(c.tyreKpa[2], 0U);
    CHECK_EQ(c.tyreKpa[3], 1758U);    /* full scale still fits u16 */
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

/* ===========================================================================
 * 10. Addendum B -- 0xBC FLOW (fuel flow, 0.1 L/h), spec section 10.
 *
 * Driven through the REAL decoder with the box's own millisecond counter set by
 * hand (ram::app().sched.counter), because the whole point of 0xBC is that it
 * is a time-weighted average. The window opens at counter 0 after primed() and
 * closes on the first customPublish() at or after 1000 ms.
 *
 * L/h = Q / 160, so deci-L/h = Q / 16: Q = 1600 is 10.0 L/h, Q = 1968 is
 * 12.3 L/h exactly. The expected values below are worked out by hand from that
 * and never read back from the firmware.
 * ========================================================================= */

namespace {

const volatile std::uint8_t* bC() noexcept { return ram::app().customFrames.bC; }

void clockAt(std::uint32_t ms) noexcept { ram::app().sched.counter = ms; }

/* 0x1A7: b0:b1 = Q big-endian, b2..b5 = 0, DLC 6 on the real bus. The test
 * ring writes DLC 8; the decoder never reads it, like every other custom one. */
void flowFrame(std::uint32_t atMs, std::uint16_t q) noexcept
{
    clockAt(atMs);
    const std::uint8_t d[8] = { static_cast<std::uint8_t>(q >> 8),
                                static_cast<std::uint8_t>(q), 0U, 0U, 0U, 0U, 0U, 0U };
    pushFrame(logic::canid::kJeepFuelFlow, d);
    decodeOnce();
}

/* Close the window at t = 1000 and build the packed 0xBC wire frame. */
void closeWindow(std::uint8_t* wire) noexcept
{
    clockAt(1000U);
    logic::customPublish();
    hiworldFrame(wire, 0xBCU, bC(), 2U);
}

/* Feed Q every 100 ms from t = 0 through t = 1000 inclusive. */
void steadyFlow(std::uint16_t q) noexcept
{
    for (std::uint32_t t = 0U; t <= 1000U; t += 100U) {
        flowFrame(t, q);
    }
}

}  // namespace

/* Spec vector: 12.3 L/h = Q 1968 steady -> 123 deci-L/h = 0x007B. */
TEST(flow_normal_value_matches_the_worked_example)
{
    primed();
    steadyFlow(1968U);
    std::uint8_t wire[8];
    closeWindow(wire);
    const std::uint8_t want[7] = { 0xAAU, 0x55U, 0x02U, 0xBCU,
                                   0x00U, 0x7BU, 0x38U };
    CHECK_BYTES(wire, want, 7U);
}

/* Spec vector, and THE TRAP: Q = 0 is fuel cut on overrun, a real 0.0 L/h that
 * must be published as zero -- not skipped, and not confused with 0xFFFF. */
TEST(flow_zero_overrun_is_a_reading_not_a_blank)
{
    primed();
    steadyFlow(0U);
    std::uint8_t wire[8];
    closeWindow(wire);
    const std::uint8_t want[7] = { 0xAAU, 0x55U, 0x02U, 0xBCU,
                                   0x00U, 0x00U, 0xBDU };
    CHECK_BYTES(wire, want, 7U);
}

/* Spec vector: no frame at all in the window -> 0xFFFF. Also the cold-boot
 * case in time: the first second after power-up is a silent second. */
TEST(flow_silent_second_publishes_unavailable)
{
    primed();
    std::uint8_t wire[8];
    closeWindow(wire);
    const std::uint8_t want[7] = { 0xAAU, 0x55U, 0x02U, 0xBCU,
                                   0xFFU, 0xFFU, 0xBBU };
    CHECK_BYTES(wire, want, 7U);
}

TEST(flow_cold_boot_primes_unavailable_before_the_first_window)
{
    primed();
    logic::customPublish();            /* counter still 0: window not due */
    CHECK_EQ(bC()[0], 0xFFU);
    CHECK_EQ(bC()[1], 0xFFU);
    CHECK_EQ(ram::app().customSignals.flowDeciLph, 0xFFFFU);
}

/* A silent bus AFTER a good window must not hold the old value. */
TEST(flow_bus_going_silent_returns_to_unavailable)
{
    primed();
    steadyFlow(1600U);
    std::uint8_t wire[8];
    closeWindow(wire);
    CHECK_EQ(bC()[0], 0x00U);
    CHECK_EQ(bC()[1], 0x64U);          /* 10.0 L/h */

    clockAt(2000U);
    logic::customPublish();            /* second window: no frames */
    CHECK_EQ(bC()[0], 0xFFU);
    CHECK_EQ(bC()[1], 0xFFU);
}

/* 600 ms at Q = 1600 (10.0 L/h) then 400 ms at Q = 0 -> 0.6 x 10 = 6.0 L/h,
 * i.e. 60 = 0x3C. A plain frame average would say 6/11 x 10 = 5.45. */
TEST(flow_mixed_values_are_time_weighted)
{
    primed();
    for (std::uint32_t t = 0U; t <= 600U; t += 100U) { flowFrame(t, 1600U); }
    for (std::uint32_t t = 700U; t <= 1000U; t += 100U) { flowFrame(t, 0U); }
    std::uint8_t wire[8];
    closeWindow(wire);
    const std::uint8_t want[7] = { 0xAAU, 0x55U, 0x02U, 0xBCU,
                                   0x00U, 0x3CU, 0xF9U };
    CHECK_BYTES(wire, want, 7U);
}

/* Unequal spacing is weighted by dt, not by frame count: Q = 3200 over one
 * 250 ms interval and Q = 0 over one 50 ms interval -> 3200*250/300 = 2666.7
 * -> /16 = 166.7 -> 167. */
TEST(flow_weights_by_interval_length_not_frame_count)
{
    primed();
    flowFrame(0U, 3200U);
    flowFrame(250U, 3200U);
    flowFrame(300U, 0U);
    std::uint8_t wire[8];
    closeWindow(wire);
    CHECK_EQ(bC()[0], 0x00U);
    CHECK_EQ(bC()[1], 0xA7U);          /* 167 */
}

/* A 0xFFFF frame is rejected whole AND voids the interval after it. Timeline:
 *   t=0   Q 1600   first frame, nothing to integrate
 *   t=100 Q 1600   +100 ms @ 1600
 *   t=200 FFFF     rejected, lastFlowMs invalidated
 *   t=300 Q 3200   NOT counted (its dt would span the hole)
 *   t=400 Q 1600   +100 ms @ 1600
 * -> 1600 exactly = 10.0 L/h = 100. If the interval after the FFFF were
 * counted the answer would be 640000/300 = 2133 = 13.3 L/h, and if the 0xFFFF
 * were read as a Q it would be hundreds of L/h. */
TEST(flow_unavailable_frame_is_rejected_and_skips_the_next_interval)
{
    primed();
    flowFrame(0U, 1600U);
    flowFrame(100U, 1600U);
    flowFrame(200U, 0xFFFFU);
    flowFrame(300U, 3200U);
    flowFrame(400U, 1600U);
    std::uint8_t wire[8];
    closeWindow(wire);
    CHECK_EQ(bC()[0], 0x00U);
    CHECK_EQ(bC()[1], 0x64U);
}

/* A window holding only 0xFFFF frames has integrated nothing: 0xFFFF out. */
TEST(flow_all_unavailable_frames_publish_unavailable)
{
    primed();
    for (std::uint32_t t = 0U; t <= 1000U; t += 100U) { flowFrame(t, 0xFFFFU); }
    std::uint8_t wire[8];
    closeWindow(wire);
    CHECK_EQ(bC()[0], 0xFFU);
    CHECK_EQ(bC()[1], 0xFFU);
}

/* A gap over 300 ms is not integrated. t=100 -> t=500 is 400 ms: the 16000
 * frame that ends it is dropped (it would be 100 L/h), and only the 100 ms
 * intervals around it count. */
TEST(flow_gap_over_300ms_is_dropped)
{
    primed();
    flowFrame(0U, 1600U);
    flowFrame(100U, 1600U);
    flowFrame(500U, 16000U);           /* 400 ms gap: stamp only */
    flowFrame(600U, 1600U);            /* +100 ms @ 1600 */
    std::uint8_t wire[8];
    closeWindow(wire);
    CHECK_EQ(bC()[0], 0x00U);
    CHECK_EQ(bC()[1], 0x64U);
}

/* ...and 300 ms exactly is still integrated (the rule is dt > 300). */
TEST(flow_gap_of_exactly_300ms_is_kept)
{
    primed();
    flowFrame(0U, 1600U);
    flowFrame(300U, 3200U);            /* 300 ms @ 3200 = 20.0 L/h */
    std::uint8_t wire[8];
    closeWindow(wire);
    CHECK_EQ(bC()[0], 0x00U);
    CHECK_EQ(bC()[1], 0xC8U);          /* 200 */
}

/* Integer rounding is half-up: Q 1960 = 122.5 -> 123, Q 1959 = 122.4375 -> 122. */
TEST(flow_rounds_half_up)
{
    primed();
    steadyFlow(1960U);
    std::uint8_t wire[8];
    closeWindow(wire);
    CHECK_EQ(bC()[1], 123U);

    primed();
    steadyFlow(1959U);
    closeWindow(wire);
    CHECK_EQ(bC()[1], 122U);
}

/* Largest representable reading. Q = 0xFFFE is 409.6 L/h = 4096 = 0x1000, so
 * the 0xFFFE clamp is defensive only -- but the result must never be 0xFFFF,
 * which is the sentinel. */
TEST(flow_maximum_reading_is_not_confused_with_the_sentinel)
{
    primed();
    steadyFlow(0xFFFEU);
    std::uint8_t wire[8];
    closeWindow(wire);
    const std::uint8_t want[7] = { 0xAAU, 0x55U, 0x02U, 0xBCU,
                                   0x10U, 0x00U, 0xCDU };
    CHECK_BYTES(wire, want, 7U);
    CHECK(ram::app().customSignals.flowDeciLph != 0xFFFFU);
}

/* Each window stands alone: the sums reset on close, so a quiet second after
 * a busy one is not contaminated. */
TEST(flow_windows_do_not_leak_into_each_other)
{
    primed();
    steadyFlow(1600U);
    std::uint8_t wire[8];
    closeWindow(wire);
    CHECK_EQ(bC()[1], 0x64U);

    for (std::uint32_t t = 1100U; t <= 2000U; t += 100U) { flowFrame(t, 0U); }
    clockAt(2000U);
    logic::customPublish();
    CHECK_EQ(bC()[0], 0x00U);
    CHECK_EQ(bC()[1], 0x00U);          /* 0.0, and not 5.0 carried over */
}

/* 0xBC has NO validity bit (the 0xB0 mask is full): accepting flow frames must
 * leave the mask alone, and 0x1A7 must not touch the tank level of 0xB8. */
TEST(flow_has_no_validity_bit_and_leaves_fuel_level_alone)
{
    primed();
    fuel(0U, 0x8BU);
    decodeOnce();
    logic::customPublish();
    const std::uint8_t maskBefore = validMask();
    const std::uint8_t litre0 = b8()[0];
    const std::uint8_t litre1 = b8()[1];

    steadyFlow(1968U);
    std::uint8_t wire[8];
    closeWindow(wire);
    CHECK_EQ(validMask(), maskBefore);
    CHECK_EQ(b8()[0], litre0);
    CHECK_EQ(b8()[1], litre1);
}

/* Sixteen slots, the geometry intact, and 0xBC really leaves on the wire. */
TEST(flow_block_is_sixteen_slots_and_0xBC_reaches_the_wire)
{
    CHECK_EQ(static_cast<unsigned>(cfg::kTxSlotCount), 16U);
    CHECK_EQ(static_cast<unsigned>(svc::hu::kTxSlots), 16U);
    CHECK_EQ(sizeof(ram::AppRam::uartSlotState), 16U);
    CHECK_EQ(sizeof(ram::UartShadows::msgBC), 2U);
    CHECK_EQ(sizeof(ram::CustomFrames::bC), 2U);

    /* The fake USART records only what the TXE interrupt clocks out, and holds
     * 256 bytes, so run the scheduler and the ISR together and scan after every
     * frame rather than once at the end. */
    fake::reset();
    svc::hu::resetState();
    logic::customPrimeFrames();
    fake::returns().usartItStatus = 1;
    fake::returns().usartRxByte   = 0x00U;

    const std::uint8_t want[7] = { 0xAAU, 0x55U, 0x02U, 0xBCU, 0xFFU, 0xFFU, 0xBBU };
    bool found = false;
    for (int pass = 0; pass < 8 * 60 && !found; ++pass) {
        svc::hu::service();
        for (int k = 0; k < 24; ++k) { svc::hu::onInterrupt(); }
        const std::uint8_t* tx = fake::usartTxBytes();
        const std::size_t   n  = fake::usartTxCount();
        for (std::size_t a = 0U; (a + 7U) <= n; ++a) {
            bool m = true;
            for (std::size_t b = 0U; b < 7U; ++b) { m = m && (tx[a + b] == want[b]); }
            found = found || m;
        }
    }
    CHECK(ram::app().uartTxSlotIdx <= svc::hu::kTxSlots);
    CHECK(found);
}
