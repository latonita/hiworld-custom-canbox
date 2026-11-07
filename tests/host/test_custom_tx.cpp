/* ============================================================================
 * test_custom_tx.cpp -- the custom telemetry block, layout v2: WK CAN in,
 * Hiworld payloads out, and the priority lane in the head-unit scheduler.
 *
 * Built and run ONLY in the jeep configuration (tools/test.py --protocol jeep).
 *
 * THE POINT OF THIS FILE. The frames below are checked against the TEST
 * VECTORS in docs/TELEMETRY-V2-SPEC.md section 10, which were computed from the
 * section 1 framing rule independently of this firmware. The framer in this
 * file is written out from that rule rather than reused from hu_hiworld.cpp,
 * so a mistake in the firmware's emit() cannot hide by being made twice.
 *
 * The CAN side is driven through the REAL decoder (logic::decodeProfileJeep)
 * by writing the RX ring the way the CAN_RX0 ISR does, and the clock is the
 * box's own ram::app().sched.counter set by hand, because v2 is all about
 * time: source ages, the 700 ms indicator hold, the 100 ms fuel-flow window.
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

/* ---- RX ring geometry, as test_can_decode.cpp writes it ------------------ */
constexpr std::size_t kStride  = ram::kCanRingStride;
constexpr std::size_t kEntries = ram::kCanRingEntries;
constexpr std::size_t kIdHi = 0U, kIdLo = 2U, kIde = 4U, kDlc = 5U, kData = 6U;

void pushFrame(std::uint32_t id, const std::uint8_t* data,
               std::uint8_t dlc = 8U) noexcept
{
    ram::AppRam& r = ram::app();
    volatile std::uint8_t* e = r.canRxRing
                             + static_cast<std::size_t>(r.canRing.rxHead) * kStride;
    e[kIdHi]      = static_cast<std::uint8_t>(id >> 16);
    e[kIdHi + 1U] = static_cast<std::uint8_t>(id >> 24);
    e[kIdLo]      = static_cast<std::uint8_t>(id);
    e[kIdLo + 1U] = static_cast<std::uint8_t>(id >> 8);
    e[kIde]       = 0U;
    e[kDlc]       = dlc;
    for (std::size_t i = 0U; i < 8U; ++i) {
        e[kData + i] = (data != nullptr) ? data[i] : 0U;
    }
    r.canRing.rxHead = (r.canRing.rxHead + 1U) % static_cast<std::uint32_t>(kEntries);
}

void decodeOnce() noexcept { logic::decodeProfileJeep(); }

void clockAt(std::uint32_t ms) noexcept { ram::app().sched.counter = ms; }
std::uint32_t clockNow() noexcept { return ram::app().sched.counter; }

/* Start from a primed box at t = 0: sentinels everywhere, nothing seen. */
void primed() noexcept
{
    fake::reset();
    logic::customPrimeFrames();
}

/* Send one frame at the current clock and decode it. */
void send(std::uint32_t id, std::uint8_t b0, std::uint8_t b1 = 0U,
          std::uint8_t b2 = 0U, std::uint8_t b3 = 0U, std::uint8_t b4 = 0U,
          std::uint8_t b5 = 0U, std::uint8_t b6 = 0U, std::uint8_t b7 = 0U,
          std::uint8_t dlc = 8U) noexcept
{
    const std::uint8_t d[8] = { b0, b1, b2, b3, b4, b5, b6, b7 };
    pushFrame(id, d, dlc);
    decodeOnce();
}

/* ---- named CAN builders (byte map ids) ----------------------------------- */
void ign(std::uint8_t b0) noexcept { send(0x000U, b0, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 6U); }
void eng(std::uint16_t rpm, std::uint16_t speedRaw) noexcept
{
    send(0x002U, static_cast<std::uint8_t>(rpm >> 8), static_cast<std::uint8_t>(rpm),
         static_cast<std::uint8_t>(speedRaw >> 8), static_cast<std::uint8_t>(speedRaw));
}
void cluster(std::uint8_t b0, std::uint8_t b1, std::uint8_t lever,
             std::uint8_t brake, std::uint8_t ascii) noexcept
{
    send(0x003U, b0, b1, lever, brake, ascii, 0xFFU, 0U, 0U, 7U);
}
void lights(std::uint8_t b0, std::uint8_t b1 = 0x22U) noexcept
{
    send(0x006U, b0, b1, 0x04U, 0x3CU, 0x3CU, 0x02U, 0x58U, 0U, 7U);
}
void key(std::uint8_t b2) noexcept { send(0x012U, 0U, 0x30U, b2, 0x40U, 0U, 0U, 0U, 0U, 6U); }
void body(std::uint8_t b0, std::uint8_t b2) noexcept
{
    send(0x013U, b0, 0U, b2, 0x49U, 0x99U, 0U, 0U, 0U, 5U);
}
void trip(std::uint32_t odo, std::uint16_t amb, std::uint8_t dlc = 7U) noexcept
{
    send(0x014U, static_cast<std::uint8_t>(odo >> 16), static_cast<std::uint8_t>(odo >> 8),
         static_cast<std::uint8_t>(odo), 0xFFU, static_cast<std::uint8_t>(amb >> 8),
         static_cast<std::uint8_t>(amb), 0x57U, 0U, dlc);
}
void volts(std::uint8_t b1) noexcept { send(0x015U, 0x70U, b1, 0x10U, 0xFFU, 0U, 0U, 0U, 0U, 6U); }
void bay(std::uint8_t oil, std::uint8_t coolant, std::uint8_t pedal = 0U) noexcept
{
    send(0x02CU, oil, coolant, pedal, 0x64U, 0U, 0U, 0U, 0U, 6U);
}
void park(std::uint8_t b0) noexcept { send(0x0D0U, b0, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U); }
void fuel(std::uint8_t minutes, std::uint8_t gate, std::uint8_t b5) noexcept
{
    send(0x159U, minutes, 0xFFU, 0xFFU, 0xFFU, gate, b5, 0U, 0U, 6U);
}
void flow(std::uint16_t q) noexcept
{
    send(0x1A7U, static_cast<std::uint8_t>(q >> 8), static_cast<std::uint8_t>(q),
         0U, 0U, 0U, 0U, 0U, 0U, 6U);
}
void transfer(std::uint8_t b1) noexcept { send(0x1B2U, 0U, b1, 0U, 0U, 0U, 0U, 0U, 0U, 6U); }
void compass(std::uint8_t b0, std::uint8_t b1) noexcept
{
    send(0x1B4U, b0, b1, 0x07U, 0U, 0U, 0U, 0U, 0U, 6U);
}
void boost(std::uint8_t b4) noexcept
{
    send(0x2C0U, 0xFFU, 0xFFU, 0x17U, 0x0BU, b4, 0xFFU, 0x17U, 0x82U);
}

/* ---- payload shorthand ---------------------------------------------------- */
const volatile std::uint8_t* b0() noexcept { return ram::app().customFrames.b0; }
const volatile std::uint8_t* b2() noexcept { return ram::app().customFrames.b2; }
const volatile std::uint8_t* b3() noexcept { return ram::app().customFrames.b3; }
const volatile std::uint8_t* b4() noexcept { return ram::app().customFrames.b4; }
const volatile std::uint8_t* b5() noexcept { return ram::app().customFrames.b5; }
const volatile std::uint8_t* b7() noexcept { return ram::app().customFrames.b7; }
const volatile std::uint8_t* b8() noexcept { return ram::app().customFrames.b8; }


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
    ram::app().customSignals.tpmsPollDiv = 1249U;   /* kTpmsPollPasses - 1 */
}

/* ...and make that request the pressure read (21 3B), the start of the cycle. */
void armPollA() noexcept
{
    ram::app().customSignals.tpmsStep = 0U;
    armPoll();
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


/* 0xB7 is "live" (v1: validity bit 4) while fewer than four pressure polls in
 * a row have failed. */
bool b7Live() noexcept { return ram::app().customSignals.tpmsFailures < 4U; }

/* ---------------------------------------------------------------------------
 * The Hiworld frame, from docs/TELEMETRY-V2-SPEC.md section 1 and nothing
 * else. Checksum written as the FULL byte sum including the preamble; the
 * firmware computes the algebraically identical (LEN + CMD + sum - 1).
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

/* Frame a payload and compare with a section 10 vector. */
void checkVector(std::uint8_t cmd, const volatile std::uint8_t* pl, std::uint8_t len,
                 const std::uint8_t* want) noexcept
{
    std::uint8_t wire[32];
    const std::size_t n = hiworldFrame(wire, cmd, pl, len);
    CHECK_EQ(n, static_cast<std::size_t>(len) + 5U);
    CHECK_BYTES(wire, want, n);
}

/* Run the 100 ms producer once at the current clock. */
void tick() noexcept { logic::customTick100(); }

/* ---- the scheduler simulator ----------------------------------------------
 * Every 8 ms: advance the clock, run customPublish() and svc::hu::service()
 * exactly as the 8 ms task does, then let the "ISR" finish the frame (each
 * frame is <= 22 B = 5.7 ms at 38400 baud, inside one pass). Every emitted
 * frame is logged with its time and command. */
struct Emitted {
    std::uint32_t ms;
    std::uint8_t  cmd;
};
constexpr std::size_t kLogMax = 4096U;
Emitted g_log[kLogMax];
std::size_t g_logN = 0U;

void pass8() noexcept
{
    clockAt(clockNow() + 8U);
    logic::customPublish();
    /* A frame was built this pass iff the busy flag is up afterwards (the
     * fake HAL's call log saturates, so it cannot count long runs). */
    svc::hu::service();
    if (ram::app().uartFrame[svc::hu::kFbTxBusy] != 0U) {
        if (g_logN < kLogMax) {
            g_log[g_logN].ms  = clockNow();
            g_log[g_logN].cmd = ram::app().uartFrame[3];
            ++g_logN;
        }
    }
    ram::app().uartFrame[svc::hu::kFbTxBusy] = 0U;   /* the ISR finished it */
}

std::size_t countCmd(std::uint8_t cmd) noexcept
{
    std::size_t n = 0U;
    for (std::size_t i = 0U; i < g_logN; ++i) {
        if (g_log[i].cmd == cmd) { ++n; }
    }
    return n;
}

/* Longest gap between two sends of `cmd` in the log (0 if sent < 2 times). */
std::uint32_t maxGap(std::uint8_t cmd) noexcept
{
    std::uint32_t last = 0U, gap = 0U;
    bool have = false;
    for (std::size_t i = 0U; i < g_logN; ++i) {
        if (g_log[i].cmd != cmd) { continue; }
        if (have && (g_log[i].ms - last) > gap) { gap = g_log[i].ms - last; }
        last = g_log[i].ms;
        have = true;
    }
    return gap;
}

/* A box on a live bus: every STATE source fresh at the current clock, the
 * values of the first STATE vector (ON, engine running, key in, FL door,
 * position + low + left flashing, MIL, 4H, D). RAP is clear in that vector. */
void stateVectorInputs() noexcept
{
    ign(0x85U);
    eng(2000U, 0U); eng(2000U, 0U); eng(2000U, 0U);
    lights(0x19U, 0x00U);
    key(0x02U);
    cluster(0x00U, 0x01U, 0x84U, 0x00U, 0x44U);
    body(0x80U, 0x01U);
    park(0x04U);
    transfer(0x02U);
}

}  // namespace

/* ===========================================================================
 * 0. Configuration -- the couplings spec section 7.3 lists.
 * ========================================================================= */
TEST(v2_jeep_build_has_thirteen_tx_slots)
{
    CHECK_EQ(static_cast<unsigned>(cfg::kTxSlotCount), 13U);
    CHECK_EQ(static_cast<unsigned>(svc::hu::kTxSlots), 13U);
    CHECK_EQ(sizeof(ram::AppRam::uartSlotState), 13U);
    CHECK_EQ(static_cast<unsigned>(svc::hu::kIdleDelay), 2U);
    CHECK_EQ(static_cast<unsigned>(logic::kCustomCapability), 0x01BDU);
}

TEST(v2_payload_sizes_match_the_spec)
{
    CHECK_EQ(sizeof(ram::CustomFrames::b0), 4U);
    CHECK_EQ(sizeof(ram::CustomFrames::b2), 7U);
    CHECK_EQ(sizeof(ram::CustomFrames::b3), 11U);
    CHECK_EQ(sizeof(ram::CustomFrames::b4), 5U);
    CHECK_EQ(sizeof(ram::CustomFrames::b5), 3U);
    CHECK_EQ(sizeof(ram::CustomFrames::b7), 5U);
    CHECK_EQ(sizeof(ram::CustomFrames::b8), 6U);
}

/* ===========================================================================
 * 1. Section 10 vectors.
 * ========================================================================= */
TEST(v2_rule_check_upgrade_trigger)
{
    const std::uint8_t pl[2] = { 0U, 0U };
    const std::uint8_t want[7] = { 0xAAU, 0x55U, 0x02U, 0xE0U, 0x00U, 0x00U, 0xE1U };
    checkVector(0xE0U, pl, 2U, want);
}

TEST(v2_heartbeat_vectors)
{
    primed();
    ram::app().customSignals.heartbeat = 0x2AU;
    logic::customTick1000();
    const std::uint8_t w1[9] = { 0xAAU, 0x55U, 0x04U, 0xB0U, 0x02U, 0x01U, 0xBDU, 0x2AU, 0x9DU };
    checkVector(0xB0U, b0(), 4U, w1);
    CHECK_EQ(ram::app().customSignals.heartbeat, 0x2BU);

    ram::app().customSignals.heartbeat = 0xFFU;
    logic::customTick1000();
    logic::customTick1000();             /* FF went out, now 00 */
    const std::uint8_t w2[9] = { 0xAAU, 0x55U, 0x04U, 0xB0U, 0x02U, 0x01U, 0xBDU, 0x00U, 0x73U };
    checkVector(0xB0U, b0(), 4U, w2);
}

/* 63.0 km/h, 2701 rpm, Q = 1536 (12.00 L/h) over the whole window, oil 3C,
 * boost raw 0x90, pedal raw 85 (34 %). Load from the derivation = 27 %. */
TEST(v2_fast_vector_driving)
{
    primed();
    clockAt(1000U);
    tick();                              /* window starts here */
    eng(2701U, 8064U);
    flow(1536U);
    bay(0x3CU, 0x85U, 85U);
    boost(0x90U);
    clockAt(1100U);
    eng(2701U, 8064U);
    boost(0x90U);
    tick();
    const std::uint8_t want[16] = { 0xAAU, 0x55U, 0x0BU, 0xB3U, 0x02U, 0x76U, 0x0AU, 0x8DU,
                                    0x1BU, 0x04U, 0xB0U, 0x3CU, 0x04U, 0x80U, 0x22U, 0x7DU };
    checkVector(0xB3U, b3(), 11U, want);
}

/* Stopped at idle: 0 km/h, 580 rpm, Q = 128 (1.00 L/h), load 11 %, oil 40 x 4
 * = 160 kPa, MAP 0x7E x 8 = 1008 hPa. */
TEST(v2_fast_vector_idle)
{
    primed();
    clockAt(1000U);
    tick();
    eng(580U, 0U);
    flow(128U);
    bay(0x28U, 0x85U);
    boost(0x7EU);
    clockAt(1100U);
    eng(580U, 0U);
    boost(0x7EU);
    tick();
    const std::uint8_t want[16] = { 0xAAU, 0x55U, 0x0BU, 0xB3U, 0x00U, 0x00U, 0x02U, 0x44U,
                                    0x0BU, 0x00U, 0x64U, 0x28U, 0x03U, 0xF0U, 0x00U, 0x8DU };
    checkVector(0xB3U, b3(), 11U, want);
}

TEST(v2_fast_vector_everything_unavailable)
{
    primed();
    clockAt(500U);
    tick();
    const std::uint8_t want[16] = { 0xAAU, 0x55U, 0x0BU, 0xB3U, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
                                    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xB2U };
    checkVector(0xB3U, b3(), 11U, want);
    /* ...and it is still queued: FAST is the liveness frame. */
    CHECK_EQ(ram::app().uartSlotState[7], svc::hu::kSlotRequest);
}

TEST(v2_state_vector_all_alive)
{
    primed();
    clockAt(1000U);
    stateVectorInputs();
    tick();
    const std::uint8_t want[12] = { 0xAAU, 0x55U, 0x07U, 0xB2U, 0x2BU, 0x04U, 0x19U, 0x10U,
                                    0x01U, 0x03U, 0xFFU, 0x13U };
    checkVector(0xB2U, b2(), 7U, want);
}

TEST(v2_state_vector_002_dead)
{
    primed();
    clockAt(1000U);
    stateVectorInputs();
    clockAt(1200U);                      /* 002 is 200 ms old (> 150), the rest
                                          * is inside its limit except 000/006 */
    ign(0x85U);
    lights(0x19U, 0x00U);
    tick();
    const std::uint8_t want[12] = { 0xAAU, 0x55U, 0x07U, 0xB2U, 0x23U, 0x04U, 0x19U, 0x10U,
                                    0x01U, 0x03U, 0xFDU, 0x09U };
    checkVector(0xB2U, b2(), 7U, want);
}

TEST(v2_state_vector_key_in_at_off)
{
    primed();
    clockAt(1000U);
    ign(0x01U);
    key(0x02U);
    park(0x84U);
    tick();
    const std::uint8_t want[12] = { 0xAAU, 0x55U, 0x07U, 0xB2U, 0xA1U, 0x00U, 0x00U, 0x00U,
                                    0x00U, 0xFFU, 0xA1U, 0xF9U };
    checkVector(0xB2U, b2(), 7U, want);
}

/* coolant 93 degC, 14.3 V, fuel b5 = 142 -> 71.0 L = 91 %. */
TEST(v2_engine_vectors)
{
    primed();
    clockAt(1000U);
    bay(0x28U, 0x85U);
    volts(0x8FU);
    fuel(0x0AU, 0x00U, 142U);
    tick();
    logic::customTick1000();
    const std::uint8_t w1[10] = { 0xAAU, 0x55U, 0x05U, 0xB4U, 0x85U, 0x8FU, 0x5BU, 0x02U, 0xC6U, 0xEFU };
    checkVector(0xB4U, b4(), 5U, w1);

    /* 0x02C goes stale (1.5 s), volts and fuel stay fresh */
    clockAt(2600U);
    volts(0x8FU);
    fuel(0x0AU, 0x00U, 142U);
    tick();
    logic::customTick1000();
    const std::uint8_t w2[10] = { 0xAAU, 0x55U, 0x05U, 0xB4U, 0xFFU, 0x8FU, 0x5BU, 0x02U, 0xC6U, 0x69U };
    checkVector(0xB4U, b4(), 5U, w2);
}

TEST(v2_ambient_vectors)
{
    primed();
    clockAt(1000U);
    trip(0x12D687U, 6500U);              /* +25.00 degC */
    fuel(0x0AU, 0x00U, 142U);
    tick();
    logic::customTick1000();
    const std::uint8_t w1[8] = { 0xAAU, 0x55U, 0x03U, 0xB5U, 0x00U, 0xFAU, 0x0AU, 0xBBU };
    checkVector(0xB5U, b5(), 3U, w1);

    trip(0x12D687U, 3250U);              /* -7.50 degC */
    fuel(0xFFU, 0x00U, 142U);            /* minutes FF = transient re-init */
    tick();
    logic::customTick1000();
    const std::uint8_t w2[8] = { 0xAAU, 0x55U, 0x03U, 0xB5U, 0xFFU, 0xB5U, 0xFFU, 0x6AU };
    checkVector(0xB5U, b5(), 3U, w2);
}

TEST(v2_trip_vectors)
{
    primed();
    clockAt(1000U);
    trip(0x12D687U, 6500U);
    compass(6U, 133U);                   /* W, 266 deg */
    tick();
    logic::customTick1000();
    const std::uint8_t w1[11] = { 0xAAU, 0x55U, 0x06U, 0xB8U, 0x12U, 0xD6U, 0x87U,
                                  0x06U, 0x01U, 0x0AU, 0x3DU };
    checkVector(0xB8U, b8(), 6U, w1);

    compass(0U, 0U);                     /* North 0 deg is a reading */
    tick();
    logic::customTick1000();
    const std::uint8_t w2[11] = { 0xAAU, 0x55U, 0x06U, 0xB8U, 0x12U, 0xD6U, 0x87U,
                                  0x00U, 0x00U, 0x00U, 0x2CU };
    checkVector(0xB8U, b8(), 6U, w2);

    primed();
    clockAt(1000U);
    tick();
    logic::customTick1000();
    const std::uint8_t w3[11] = { 0xAAU, 0x55U, 0x06U, 0xB8U, 0xFFU, 0xFFU, 0xFFU,
                                  0xFFU, 0xFFU, 0xFFU, 0xB7U };
    checkVector(0xB8U, b8(), 6U, w3);
}

/* ===========================================================================
 * 2. Field rules.
 * ========================================================================= */
TEST(v2_002_all_ff_blanks_speed_rpm_load_and_engine_bit)
{
    primed();
    clockAt(1000U);
    ign(0x63U);
    send(0x002U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU);
    flow(128U);
    tick();
    CHECK_EQ(b3()[0], 0xFFU); CHECK_EQ(b3()[1], 0xFFU);
    CHECK_EQ(b3()[2], 0xFFU); CHECK_EQ(b3()[3], 0xFFU);
    CHECK_EQ(b3()[4], 0xFFU);
    CHECK_EQ(b2()[0] & 0x08U, 0U);
    CHECK_EQ(b2()[6] & 0x02U, 0x02U);   /* the SOURCE is alive; the value is n/a */
}

TEST(v2_speed_has_its_own_sentinel_and_rounds)
{
    primed();
    clockAt(1000U);
    eng(0xFFFFU, 8064U);                 /* rpm n/a, speed live */
    tick();
    CHECK_EQ(b3()[0], 0x02U); CHECK_EQ(b3()[1], 0x76U);
    CHECK_EQ(b3()[2], 0xFFU); CHECK_EQ(b3()[3], 0xFFU);
    eng(600U, 0xFFFFU);
    tick();
    CHECK_EQ(b3()[0], 0xFFU); CHECK_EQ(b3()[1], 0xFFU);
    eng(600U, 0xFFFEU);                  /* largest reading: 5120, not FFFF */
    tick();
    CHECK_EQ(b3()[0], 0x14U); CHECK_EQ(b3()[1], 0x00U);
    eng(600U, 7U);                       /* 0.547 -> 1 (0.1 km/h), rounded */
    tick();
    CHECK_EQ(b3()[1], 0x01U);
}

TEST(v2_002_silent_151_ms_goes_to_sentinels)
{
    primed();
    clockAt(1000U);
    eng(800U, 1280U);
    clockAt(1150U);
    tick();
    CHECK_EQ(b3()[2], 0x03U);            /* 150 ms old: still alive */
    clockAt(1151U);
    tick();
    CHECK_EQ(b3()[0], 0xFFU); CHECK_EQ(b3()[2], 0xFFU);
    CHECK_EQ(b2()[6] & 0x02U, 0U);
    eng(800U, 1280U);                    /* a returning frame revives it at once */
    tick();
    CHECK_EQ(b3()[2], 0x03U);
}

TEST(v2_fuel_gate_holds_and_stale_source_blanks)
{
    primed();
    clockAt(1000U);
    fuel(0x05U, 0x01U, 0xFFU);           /* gated before any reading */
    tick(); logic::customTick1000();
    CHECK_EQ(b4()[2], 0xFFU); CHECK_EQ(b4()[3], 0xFFU); CHECK_EQ(b4()[4], 0xFFU);
    CHECK_EQ(b5()[2], 0x05U);

    fuel(0x05U, 0x00U, 0x4DU);           /* 77 -> 38.5 L, 50 % */
    tick(); logic::customTick1000();
    CHECK_EQ(b4()[2], 50U); CHECK_EQ(b4()[3], 0x01U); CHECK_EQ(b4()[4], 0x81U);

    clockAt(2000U);
    fuel(0x00U, 0x01U, 0xFFU);           /* key-off gate: HOLD */
    tick(); logic::customTick1000();
    CHECK_EQ(b4()[2], 50U); CHECK_EQ(b4()[4], 0x81U);

    clockAt(5001U);                      /* 3.001 s silent: stale */
    tick(); logic::customTick1000();
    CHECK_EQ(b4()[2], 0xFFU); CHECK_EQ(b4()[3], 0xFFU); CHECK_EQ(b4()[4], 0xFFU);
    CHECK_EQ(b5()[2], 0xFFU);
}

TEST(v2_fuel_percent_clamps)
{
    primed();
    clockAt(1000U);
    fuel(0U, 0U, 0xFEU);
    tick(); logic::customTick1000();
    CHECK_EQ(b4()[2], 100U);
    CHECK_EQ(b4()[3], 0x04U); CHECK_EQ(b4()[4], 0xF6U);   /* 1270 */
}

TEST(v2_ambient_holds_over_ffff_and_starts_unknown)
{
    primed();
    clockAt(1000U);
    trip(0x000100U, 0xFFFFU);
    tick(); logic::customTick1000();
    CHECK_EQ(b5()[0], 0x80U); CHECK_EQ(b5()[1], 0x00U);
    CHECK_EQ(b8()[2], 0x00U);            /* the odometer is live meanwhile */
    trip(0x000100U, 5950U);              /* 19.50 */
    tick(); logic::customTick1000();
    CHECK_EQ(b5()[0], 0x00U); CHECK_EQ(b5()[1], 0xC3U);
    trip(0x000100U, 0xFFFFU);
    tick(); logic::customTick1000();
    CHECK_EQ(b5()[1], 0xC3U);
    trip(0x000100U, 6000U, 5U);          /* no b5: ignored for air */
    tick(); logic::customTick1000();
    CHECK_EQ(b5()[1], 0xC3U);
}

TEST(v2_compass_out_of_range_is_not_stamped)
{
    primed();
    clockAt(1000U);
    compass(2U, 45U);
    clockAt(2000U);
    compass(9U, 45U);                    /* rejected: holds, not refreshed */
    tick(); logic::customTick1000();
    CHECK_EQ(b8()[3], 2U);
    clockAt(2501U);
    compass(2U, 180U);                   /* rejected too */
    tick(); logic::customTick1000();
    CHECK_EQ(b8()[3], 0xFFU);            /* 1.5 s since the last good one */
    CHECK_EQ(b8()[4], 0xFFU); CHECK_EQ(b8()[5], 0xFFU);
}

TEST(v2_boost_rules)
{
    primed();
    clockAt(1000U);
    boost(0xFEU);
    tick();
    CHECK_EQ(b3()[8], 0x07U); CHECK_EQ(b3()[9], 0xF0U);  /* saturated, 2032 */
    boost(0xFFU);
    tick();
    CHECK_EQ(b3()[8], 0xFFU); CHECK_EQ(b3()[9], 0xFFU);
    boost(0x00U);
    tick();
    CHECK_EQ(b3()[8], 0x00U); CHECK_EQ(b3()[9], 0x00U);
}

TEST(v2_pedal_percent)
{
    primed();
    clockAt(1000U);
    bay(0x28U, 0x85U, 250U);
    tick();
    CHECK_EQ(b3()[10], 100U);
    bay(0x28U, 0x85U, 254U);             /* above 250 clamps */
    tick();
    CHECK_EQ(b3()[10], 100U);
    bay(0x28U, 0x85U, 1U);               /* 0.4 -> 0, half up: 2 -> 0.8 -> 1 */
    tick();
    CHECK_EQ(b3()[10], 0U);
    bay(0x28U, 0x85U, 2U);
    tick();
    CHECK_EQ(b3()[10], 1U);
    clockAt(2501U);                      /* 02C stale */
    tick();
    CHECK_EQ(b3()[10], 0xFFU);
}

TEST(v2_oil_and_coolant_are_verbatim)
{
    primed();
    clockAt(1000U);
    bay(0x00U, 0xFEU);                   /* engine off oil is a real 0 */
    tick(); logic::customTick1000();
    CHECK_EQ(b3()[7], 0x00U);
    CHECK_EQ(b4()[0], 0xFEU);
    send(0x02CU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0U, 0U, 6U);
    tick(); logic::customTick1000();
    CHECK_EQ(b3()[7], 0xFFU);
    CHECK_EQ(b4()[0], 0xFFU);
    CHECK_EQ(b3()[10], 0xFFU);           /* pedal FF too */
}

/* ---- fuel flow and load -------------------------------------------------- */
TEST(v2_flow_rules)
{
    primed();
    clockAt(1000U); tick();
    flow(0U);
    clockAt(1100U); tick();
    CHECK_EQ(b3()[5], 0x00U); CHECK_EQ(b3()[6], 0x00U);   /* overrun is a real 0 */

    flow(0xFFFFU);
    clockAt(1200U); tick();
    CHECK_EQ(b3()[5], 0xFFU); CHECK_EQ(b3()[6], 0xFFU);

    flow(1536U);
    clockAt(1300U); tick();
    CHECK_EQ(b3()[5], 0x04U); CHECK_EQ(b3()[6], 0xB0U);   /* 12.00 L/h */

    /* time-weighted: 50 ms at 2560, 50 ms at 0 -> mean 1280 -> 10.00 L/h */
    flow(2560U);
    clockAt(1350U);
    flow(0U);
    clockAt(1400U); tick();
    CHECK_EQ(b3()[5], 0x03U); CHECK_EQ(b3()[6], 0xE8U);
}

TEST(v2_flow_held_sample_ages_out_at_1200_ms)
{
    primed();
    clockAt(1000U); tick();
    flow(1536U);
    clockAt(2100U); tick();              /* 1100 ms old: held */
    CHECK_EQ(b3()[5], 0x04U); CHECK_EQ(b3()[6], 0xB0U);
    clockAt(2300U); tick();              /* 1300 ms old: gone */
    CHECK_EQ(b3()[5], 0xFFU); CHECK_EQ(b3()[6], 0xFFU);
}

TEST(v2_load_rules)
{
    CHECK_EQ(logic::customLoad(2000U, 4147U), 100U);
    CHECK_EQ(logic::customLoad(2000U, 9000U), 100U);      /* clamps */
    CHECK_EQ(logic::customLoad(2000U, 0U), 0U);
    CHECK_EQ(logic::customLoad(580U, 128U), 11U);
    CHECK_EQ(logic::customLoad(250U, 128U), 0xFFU);
    CHECK_EQ(logic::customLoad(0xFFFFU, 128U), 0xFFU);
    CHECK_EQ(logic::customLoad(2000U, 0xFFFFU), 0xFFU);
    CHECK_EQ(logic::customLoad(300U, 65534U), 100U);      /* no overflow */

    primed();
    clockAt(1000U); tick();
    eng(2000U, 0U);
    flow(4147U);
    clockAt(1100U); eng(2000U, 0U); tick();
    CHECK_EQ(b3()[4], 100U);
    clockAt(2400U); eng(2000U, 0U); tick();               /* 1A7 silent 1.4 s */
    CHECK_EQ(b3()[4], 0xFFU);
}

/* ---- engine running ------------------------------------------------------ */
TEST(v2_engine_running_crank_and_set)
{
    primed();
    clockAt(1000U);
    ign(0xA9U);
    eng(410U, 0U); eng(600U, 0U); eng(600U, 0U); eng(600U, 0U);
    tick();
    CHECK_EQ(b2()[0] & 0x08U, 0U);       /* cranking vetoes */
    CHECK_EQ(b2()[0] & 0x07U, 4U);
    ign(0x85U);
    eng(600U, 0U); eng(600U, 0U);
    tick();
    CHECK_EQ(b2()[0] & 0x08U, 0U);       /* two frames: not yet */
    eng(600U, 0U);
    tick();
    CHECK_EQ(b2()[0] & 0x08U, 0x08U);    /* the third sets it */
    eng(400U, 0U);
    tick();
    CHECK_EQ(b2()[0] & 0x08U, 0x08U);    /* hysteresis band holds */
    eng(299U, 0U);
    tick();
    CHECK_EQ(b2()[0] & 0x08U, 0U);       /* clears at once */
}

/* ---- enums --------------------------------------------------------------- */
TEST(v2_ignition_enum)
{
    CHECK_EQ(logic::customIgnitionEnum(0x00U), 0U);
    CHECK_EQ(logic::customIgnitionEnum(0x01U), 1U);
    CHECK_EQ(logic::customIgnitionEnum(0x63U), 2U);
    CHECK_EQ(logic::customIgnitionEnum(0x85U), 3U);
    CHECK_EQ(logic::customIgnitionEnum(0xA9U), 4U);
    CHECK_EQ(logic::customIgnitionEnum(0x42U), 7U);

    primed();
    clockAt(1000U);
    tick();
    CHECK_EQ(b2()[0] & 0x07U, 7U);       /* 000 never seen */
    ign(0x63U);
    tick();
    CHECK_EQ(b2()[0] & 0x07U, 2U);
    clockAt(1301U);
    tick();
    CHECK_EQ(b2()[0] & 0x07U, 7U);       /* 000 dead after 300 ms */
}

TEST(v2_gear_enum_and_reverse)
{
    const std::uint8_t ascii[8] = { 0x50U, 0x52U, 0x4EU, 0x44U, 0x31U, 0x32U, 0x33U, 0x34U };
    for (std::uint8_t i = 0U; i < 8U; ++i) {
        CHECK_EQ(logic::customGearEnum(0x84U, ascii[i]), i);
    }
    CHECK_EQ(logic::customGearEnum(0xC0U, 0x50U), 0xFFU);
    CHECK_EQ(logic::customGearEnum(0x84U, 0x53U), 0xFFU);

    primed();
    clockAt(1000U);
    cluster(0U, 0U, 0x81U, 0U, 0x52U);
    tick();
    CHECK_EQ(b2()[5], 1U);
    CHECK_EQ(b2()[0] & 0x40U, 0x40U);
    cluster(0x93U, 0x05U, 0xC0U, 0xC0U, 0x50U);   /* key-off frame */
    tick();
    CHECK_EQ(b2()[5], 0xFFU);
    CHECK_EQ(b2()[0] & 0x40U, 0U);
    CHECK_EQ(b2()[1] & 0x01U, 0U);       /* b3 C0: brake bit 0 clear */
}

TEST(v2_transfer_case_enum)
{
    CHECK_EQ(logic::customTransferEnum(0x02U), 1U);
    CHECK_EQ(logic::customTransferEnum(0x01U), 2U);
    CHECK_EQ(logic::customTransferEnum(0x00U), 3U);
    CHECK_EQ(logic::customTransferEnum(0x07U), 0U);
}

TEST(v2_lamp_bits_map_from_003)
{
    primed();
    clockAt(1000U);
    /* b0: ABS 01, ESP 02, ESP-off 04, charge 80; b1: MIL 01, oil 04, glow 08,
     * WIF 10, A/C 20, engaged 40, cruise on 80 */
    cluster(0x87U, 0xFDU, 0x84U, 0x01U, 0x44U);
    tick();
    CHECK_EQ(b2()[3], 0xFFU);
    CHECK_EQ(b2()[4] & 0x38U, 0x38U);
    CHECK_EQ(b2()[1] & 0x01U, 0x01U);    /* brake pedal */
    cluster(0x00U, 0x20U, 0x84U, 0x00U, 0x44U);   /* A/C compressor only */
    tick();
    CHECK_EQ(b2()[3], 0x04U);
    CHECK_EQ(b2()[4] & 0x38U, 0U);
}

TEST(v2_body_bits_follow_can_order)
{
    primed();
    clockAt(1000U);
    body(0x88U, 0x1FU);                  /* belt unfastened, all doors + hatch */
    tick();
    CHECK_EQ(b2()[1], 0x7EU);
    body(0x80U, 0x04U);                  /* RR only */
    tick();
    CHECK_EQ(b2()[1], 0x10U);
}

TEST(v2_lights_raw_bits_and_rap)
{
    primed();
    clockAt(1000U);
    ign(0x85U);
    lights(0xF8U, 0x22U);                /* position, low, high, both fogs */
    tick();
    CHECK_EQ(b2()[2], 0xF8U);
    CHECK_EQ(b2()[0] & 0x10U, 0x10U);    /* RAP */
    lights(0x00U, 0x20U);
    tick();
    CHECK_EQ(b2()[2], 0x00U);
    CHECK_EQ(b2()[0] & 0x10U, 0U);
}

/* ---- the indicator debounce (spec 3.2.1) --------------------------------- */
TEST(v2_indicator_pulsing_is_steady_and_releases_in_time)
{
    primed();
    clockAt(1000U);
    ign(0x85U);
    std::uint32_t lastSet = 0U;
    bool everCleared = false;
    /* 006 every 100 ms, bit 0 on for 400 ms then off for 400 ms, 4 s */
    for (std::uint32_t t = 1000U; t < 5000U; t += 100U) {
        clockAt(t);
        ign(0x85U);
        const bool on = ((t - 1000U) % 800U) < 400U;
        lights(on ? 0x19U : 0x18U, 0x22U);
        if (on) { lastSet = t; }
        tick();
        if ((b2()[2] & 0x01U) == 0U) { everCleared = true; }
    }
    CHECK(!everCleared);
    /* release: keep sending 0x18 */
    std::uint32_t clearedAt = 0U;
    for (std::uint32_t t = 5000U; t < 6500U; t += 100U) {
        clockAt(t);
        ign(0x85U);
        lights(0x18U, 0x22U);
        tick();
        if ((clearedAt == 0U) && ((b2()[2] & 0x01U) == 0U)) { clearedAt = t; }
    }
    CHECK(clearedAt >= lastSet + 700U);
    CHECK(clearedAt <= lastSet + 800U);
    CHECK_EQ(b2()[2], 0x18U);
}

TEST(v2_indicator_held_bit_works_too)
{
    primed();
    clockAt(1000U);
    for (std::uint32_t t = 1000U; t <= 3000U; t += 100U) {
        clockAt(t);
        ign(0x85U);
        lights(0x1AU, 0x22U);            /* right, held steady */
        tick();
        CHECK_EQ(b2()[2] & 0x07U, 0x02U);
    }
    clockAt(3700U); ign(0x85U); lights(0x18U); tick();
    CHECK_EQ(b2()[2] & 0x02U, 0U);
}

TEST(v2_hazard_sets_all_three_bits)
{
    primed();
    clockAt(1000U);
    ign(0x85U);
    lights(0x1BU);
    tick();
    CHECK_EQ(b2()[2] & 0x07U, 0x07U);
    clockAt(1400U); ign(0x85U); lights(0x18U); tick();
    CHECK_EQ(b2()[2] & 0x07U, 0x07U);    /* off phase: held */
    clockAt(1800U); ign(0x85U); lights(0x18U); tick();
    CHECK_EQ(b2()[2] & 0x07U, 0U);
}

TEST(v2_lock_flash_with_key_out_is_ignored)
{
    primed();
    clockAt(1000U);
    ign(0x00U);
    lights(0x03U, 0x22U);
    tick();
    CHECK_EQ(b2()[2] & 0x07U, 0U);
    ign(0x01U);
    lights(0x03U, 0x22U);
    tick();
    CHECK_EQ(b2()[2] & 0x07U, 0U);
    ign(0x63U);                          /* key at ACC: shown */
    lights(0x03U, 0x22U);
    tick();
    CHECK_EQ(b2()[2] & 0x07U, 0x07U);
}

TEST(v2_dead_006_clears_lights)
{
    primed();
    clockAt(1000U);
    ign(0x85U);
    lights(0x1BU);
    tick();
    clockAt(1301U);
    tick();
    CHECK_EQ(b2()[2], 0U);
    CHECK_EQ(b2()[6] & 0x08U, 0U);
}

TEST(v2_short_frames_are_rejected_and_not_stamped)
{
    primed();
    clockAt(1000U);
    send(0x003U, 0U, 0U, 0x84U, 0U, 0x44U, 0U, 0U, 0U, 4U);    /* no b4 */
    send(0x1B2U, 0U, 0x02U, 0U, 0U, 0U, 0U, 0U, 0U, 1U);       /* no b1 */
    send(0x002U, 0x03U, 0x00U, 0U, 0U, 0U, 0U, 0U, 0U, 3U);    /* no b3 */
    tick();
    CHECK_EQ(b2()[6], 0U);
    CHECK_EQ(b2()[5], 0xFFU);
    CHECK_EQ(b3()[2], 0xFFU);
}

/* ===========================================================================
 * 3. Cadence and the scheduler (spec 7, 9, 12.1).
 * ========================================================================= */
TEST(v2_state_queued_on_change_and_every_second_only)
{
    primed();
    clockAt(1000U);
    ign(0x85U);
    tick();                               /* first tick always sends */
    CHECK_EQ(ram::app().uartSlotState[8], svc::hu::kSlotRequest);
    ram::app().uartSlotState[8] = svc::hu::kSlotIdle;

    for (std::uint8_t i = 1U; i < 10U; ++i) {
        clockAt(1000U + 100U * i);
        ign(0x85U);
        tick();
        CHECK_EQ(ram::app().uartSlotState[8], svc::hu::kSlotIdle);
    }
    clockAt(2000U);
    ign(0x85U);
    tick();                               /* 1 s since the last */
    CHECK_EQ(ram::app().uartSlotState[8], svc::hu::kSlotRequest);
    ram::app().uartSlotState[8] = svc::hu::kSlotIdle;

    clockAt(2100U);
    ign(0x63U);                           /* a change */
    tick();
    CHECK_EQ(ram::app().uartSlotState[8], svc::hu::kSlotRequest);
}

TEST(v2_scheduler_cadences_on_a_live_bus)
{
    primed();
    g_logN = 0U;
    clockAt(0U);
    ram::app().customSignals.nextTickMs = 100U;

    /* 20 s. Every pass: engine frames (rpm changes every frame), and the
     * stock 0x72/0x12/0x32 windows changing constantly -- the worst case
     * for the slow lane. STATE changes every tick via the high-beam bit. */
    for (std::uint32_t p = 0U; p < 2500U; ++p) {
        ign(0x85U);
        eng(static_cast<std::uint16_t>(700U + (p % 50U)), 1280U);
        lights(((p / 12U) % 2U) != 0U ? 0x38U : 0x18U, 0x22U);
        ram::app().vehicle.at(ram::VehicleState::kWin72Off) = static_cast<std::uint8_t>(p);
        ram::app().vehicle.at(ram::VehicleState::kWin12Off) = static_cast<std::uint8_t>(p);
        ram::app().vehicle.at(ram::VehicleState::kWin32Off + 1U) =
            static_cast<std::uint8_t>(p);
        pass8();
        CHECK(ram::app().uartTxSlotIdx <= svc::hu::kTxSlots);
    }

    /* FAST: 10 Hz, drift-free: 20 s -> 200 frames (+-1), gaps 100 +- 8 ms */
    const std::size_t fast = countCmd(0xB3U);
    CHECK(fast >= 199U && fast <= 201U);
    CHECK(maxGap(0xB3U) <= 108U);
    /* STATE changes every ~96 ms here: it may go every tick, never starves */
    CHECK(countCmd(0xB2U) >= 100U);
    /* heartbeat 1 Hz */
    const std::size_t hb = countCmd(0xB0U);
    CHECK(hb >= 19U && hb <= 20U);
    CHECK(maxGap(0xB0U) <= 1056U);
    /* every slow row at least every 2 s, even under constant traffic */
    const std::uint8_t slow[10] = { 0x72U, 0xA6U, 0x73U, 0xF0U, 0x12U, 0x32U,
                                    0xB4U, 0xB5U, 0xB7U, 0xB8U };
    for (std::uint8_t i = 0U; i < 10U; ++i) {
        CHECK(countCmd(slow[i]) >= 9U);
        CHECK(maxGap(slow[i]) <= 2000U);
    }
    /* no retired command ever went out */
    for (std::uint8_t c = 0xB9U; c <= 0xBFU; ++c) {
        CHECK_EQ(countCmd(c), 0U);
    }
    CHECK_EQ(countCmd(0xB1U), 0U);
    CHECK_EQ(countCmd(0xB6U), 0U);
}

/* Nominal 10 x 144 ms = 1.44 s; priority frames landing on a 48 ms boundary
 * push it by a pass, and the heartbeat takes one boundary a second, so the
 * simulated worst case is ~1.61 s. Bound: 1.7 s. */
TEST(v2_scheduler_idle_link_slow_cycle_is_under_1700_ms)
{
    primed();
    g_logN = 0U;
    clockAt(0U);
    ram::app().customSignals.nextTickMs = 100U;
    for (std::uint32_t p = 0U; p < 1500U; ++p) {    /* 12 s, nothing changes */
        pass8();
    }
    const std::uint8_t slow[10] = { 0x72U, 0xA6U, 0x73U, 0xF0U, 0x12U, 0x32U,
                                    0xB4U, 0xB5U, 0xB7U, 0xB8U };
    for (std::uint8_t i = 0U; i < 10U; ++i) {
        CHECK(maxGap(slow[i]) <= 1700U);
    }
    CHECK(maxGap(0xB3U) <= 108U);
}

TEST(v2_scheduler_slot_index_survives_a_million_passes)
{
    primed();
    clockAt(0U);
    ram::app().customSignals.nextTickMs = 100U;
    for (std::uint32_t p = 0U; p < 1000000U; ++p) {
        clockAt(clockNow() + 8U);
        logic::customPublish();
        if ((p % 7U) == 0U) {
            ram::app().vehicle.at(ram::VehicleState::kWin72Off) = static_cast<std::uint8_t>(p);
        }
        svc::hu::service();
        ram::app().uartFrame[svc::hu::kFbTxBusy] = 0U;
        if (ram::app().uartTxSlotIdx > svc::hu::kTxSlots) {
            CHECK(false);
            break;
        }
    }
    CHECK(ram::app().uartAuxSlotIdx < svc::hu::kTxSlots);
}

/* Stock 0x32: only [0..1] are compared, so a moving rpm does not claim a
 * 48 ms slot; a gear change does. */
TEST(v2_stock_0x32_compares_gear_only)
{
    fake::reset();
    svc::hu::resetState();
    /* settle: send everything once */
    for (int i = 0; i < 200; ++i) {
        svc::hu::service();
        ram::app().uartFrame[svc::hu::kFbTxBusy] = 0U;
    }
    ram::app().vehicle.setRpm(0x0AU, 0x8DU);
    bool sent32 = false;
    for (int i = 0; i < 6; ++i) {
        const std::size_t before = fake::countOf("usart_tx_start");
        svc::hu::service();
        if (fake::countOf("usart_tx_start") != before && ram::app().uartFrame[3] == 0x32U) {
            sent32 = true;
        }
        ram::app().uartFrame[svc::hu::kFbTxBusy] = 0U;
    }
    CHECK(!sent32);

    ram::app().vehicle.at(ram::VehicleState::kGear) = 0x05U;
    sent32 = false;
    for (int i = 0; i < 6; ++i) {
        const std::size_t before = fake::countOf("usart_tx_start");
        svc::hu::service();
        if (fake::countOf("usart_tx_start") != before && ram::app().uartFrame[3] == 0x32U) {
            sent32 = true;
            CHECK_EQ(ram::app().uartFrame[4 + 2], 0x0AU);   /* rpm rides along */
        }
        ram::app().uartFrame[svc::hu::kFbTxBusy] = 0U;
    }
    CHECK(sent32);
}

TEST(v2_priority_frame_is_built_from_the_buffer)
{
    primed();
    clockAt(1000U);
    eng(2701U, 8064U);
    tick();
    svc::hu::service();                  /* pass 1: priority lane, no 48 ms wait */
    CHECK_EQ(ram::app().uartFrame[3], 0xB3U);   /* FAST first */
    CHECK_EQ(ram::app().uartFrame[2], 11U);
    CHECK_EQ(ram::app().uartFrame[4], 0x02U);
    CHECK_EQ(ram::app().uartFrame[5], 0x76U);
    CHECK_EQ(ram::app().uartSlotState[7], svc::hu::kSlotIdle);
    ram::app().uartFrame[svc::hu::kFbTxBusy] = 0U;
    svc::hu::service();
    CHECK_EQ(ram::app().uartFrame[3], 0xB2U);   /* STATE on the next pass */
    CHECK_EQ(ram::app().uartSlotState[8], svc::hu::kSlotIdle);
}

/* requestPriority() ignores a non-priority row; requestSend() is not served
 * from the priority lane. */
TEST(v2_lane_requests_are_kept_apart)
{
    fake::reset();
    svc::hu::resetState();
    svc::hu::requestPriority(0xB4U);
    CHECK_EQ(ram::app().uartSlotState[9], svc::hu::kSlotIdle);
    svc::hu::requestPriority(0x72U);
    CHECK_EQ(ram::app().uartSlotState[0], svc::hu::kSlotIdle);
    svc::hu::requestSend(0xB0U);
    CHECK_EQ(ram::app().uartSlotState[6], svc::hu::kSlotRequest);
    svc::hu::service();                  /* pass 1: nothing on the priority lane */
    CHECK_EQ(fake::countOf("usart_tx_start"), 0U);
}

/* ===========================================================================
 * 4. The stock slots are untouched.
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

    /* Stock 0x32 still carries rpm at payload [2..3] (sent on the slow cycle
     * only since v2) -- 0xB3 carries the live rpm. */
    ram::app().vehicle.setRpm(0x0AU, 0x8DU);
    const volatile std::uint8_t* win32 =
        ram::app().vehicle.window(ram::VehicleState::kWin32Off);
    CHECK_EQ(win32[2], 0x0AU);
    CHECK_EQ(win32[3], 0x8DU);
}


/* ===========================================================================
 * 5. 0xB7 TYRE -- polling the WCM (unchanged by v2; packed by the 1 s task). docs/TPMS-POLL-SPEC.md.
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
    CHECK(b7Live());
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
        logic::customTick1000();

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
    CHECK(!b7Live());
    CHECK_EQ(ram::app().customSignals.tpmsPending, 1U);   /* still waiting */

    tpmsReply(0x21U, 0x20U, 0x21U, 0x1EU);
    decodeOnce();
    CHECK(b7Live());
}

/* A reply with no poll outstanding is ignored -- and, crucially, NOTHING is
 * transmitted: the stock 0x500 -> 0x516 mirror is off in a jeep build. */
TEST(tpms_reply_without_an_outstanding_poll_is_ignored_and_silent)
{
    primed();
    ram::app().canBusStatus = cfg::kBusRunning;

    tpmsReply(0x21U, 0x20U, 0x21U, 0x1EU);
    decodeOnce();

    CHECK(!b7Live());
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
    CHECK(!b7Live());
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
    CHECK(b7Live());

    for (int miss = 0; miss < 4; ++miss) {
        armPollA();
        pollPass();                   /* request goes out */
        runOutDeadline();             /* ...and times out */
    }

    logic::customTick1000();
    CHECK(!b7Live());
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
        armPollA();
        pollPass();
        runOutDeadline();
    }

    CHECK(b7Live());
    CHECK_EQ(ram::app().customSignals.tyreKpa[0], 228U);
}

/* Before any answer at all: sentinels, no-signal flag, bit 4 clear. The frame
 * still goes out -- the scheduler cannot be told to skip a slot -- so every
 * field has to be honest on its own. */
TEST(tpms_before_the_first_answer_publishes_no_value)
{
    primed();
    logic::customTick1000();

    std::uint8_t wire[16];
    hiworldFrame(wire, 0xB7U, b7(), 5U);
    const std::uint8_t want[10] = { 0xAAU, 0x55U, 0x05U, 0xB7U,
                                    0x00U, 0xFFU, 0xFFU, 0xFFU, 0x04U, 0xBCU };
    CHECK_BYTES(wire, want, 10U);
    CHECK(!b7Live());
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
    tpmsReply(33U, 6U, 34U, 254U);
    decodeOnce();

    const ram::CustomSignals& c = ram::app().customSignals;
    CHECK_EQ(c.tyreKpa[0], 228U);     /* 227.49 -> 228, not 227 */
    CHECK_EQ(c.tyreKpa[1], 41U);      /* 6 psi, just above the absent floor */
    CHECK_EQ(c.tyreKpa[2], 234U);
    CHECK_EQ(c.tyreKpa[3], 1751U);    /* near full scale still fits u16 */
}

/* A corner with no sensor reads 5 psi (or 0 / 0xFF): that tyre alone is "no
 * signal" -- pressure 0xFFFF and flag bit2 on the wire; the rest are normal. */
TEST(tpms_absent_sensor_is_no_signal_for_that_tyre_only)
{
    static const std::uint8_t absent[3] = { 5U, 0U, 0xFFU };

    for (std::uint8_t k = 0U; k < 3U; ++k) {
        primed();
        armPoll();
        pollPass();
        tpmsReply(absent[k], 0x20U, 0x21U, 0x1EU);
        decodeOnce();

        const ram::CustomSignals& c = ram::app().customSignals;
        CHECK_EQ(c.tyreKpa[ram::kTyrePosLeftFront],  0xFFFFU);
        CHECK_EQ(c.tyreKpa[ram::kTyrePosRightFront], 221U);
        CHECK_EQ(c.tyreKpa[ram::kTyrePosRightRear],  228U);
        CHECK_EQ(c.tyreKpa[ram::kTyrePosLeftRear],   207U);
        CHECK(b7Live());

        ram::app().customSignals.tpmsPosition = ram::kTyrePosLeftFront;
        logic::customTick1000();
        CHECK_EQ(b7()[1], 0xFFU);
        CHECK_EQ(b7()[2], 0xFFU);
        CHECK_EQ(b7()[4], ram::kTyreFlagNoSignal);

        ram::app().customSignals.tpmsPosition = ram::kTyrePosRightFront;
        logic::customTick1000();
        CHECK_EQ(b7()[4], 0U);
    }
}

/* ---------------------------------------------------------------------------
 * The per-wheel reads: 21 2n (temperature, battery, trigger) and 21 25+n
 * (low-pressure warning). One request every 10 s in the cycle
 *   21 3B, 21 2n, 21 25+n, (next wheel) 21 3B ...
 * ------------------------------------------------------------------------- */
static void armStep(std::uint8_t step, std::uint8_t wheel) noexcept
{
    ram::app().customSignals.tpmsStep  = step;
    ram::app().customSignals.tpmsWheel = wheel;
    armPoll();
    pollPass();
}

/* 21 2n answer: 61 2n P T X S M. S = frame no. | battery<<2 | trigger<<4 | state<<6 */
static void sensorReply(std::uint8_t n, std::uint8_t temp, std::uint8_t batt,
                        std::uint8_t trig, std::uint8_t state = 0U) noexcept
{
    const std::uint8_t s = static_cast<std::uint8_t>(
        1U | (batt << 2) | (trig << 4) | (state << 6));
    const std::uint8_t d[8] = { 0x07U, 0x61U, static_cast<std::uint8_t>(0x20U + n),
                                0x20U, temp, 0x00U, s, 0xD2U };
    pushFrame(logic::canid::kJeepVendorEcho, d);
}

/* 21 25+n answer: 61 2n+5 B2 B3. */
static void diagReply(std::uint8_t n, std::uint8_t b2, std::uint8_t b3) noexcept
{
    const std::uint8_t d[8] = { 0x04U, 0x61U, static_cast<std::uint8_t>(0x25U + n),
                                b2, b3, 0xD2U, 0xD2U, 0xD2U };
    pushFrame(logic::canid::kJeepVendorEcho, d);
}

/* All four pressures good (33, 32, 33, 30 psi). */
static void livePressures() noexcept
{
    primed();
    armPollA();
    pollPass();
    tpmsReply(0x21U, 0x20U, 0x21U, 0x1EU);
    decodeOnce();
}

static void publishPos(std::uint8_t pos) noexcept
{
    ram::app().customSignals.tpmsPosition = pos;
    logic::customTick1000();
}

/* One failed read of a request type: send it, let it time out. */
static void failRead(std::uint8_t step, std::uint8_t wheel) noexcept
{
    armStep(step, wheel);
    runOutDeadline();
}

TEST(tpms_cycle_order_and_ten_second_spacing)
{
    primed();
    ram::app().canBusStatus = cfg::kBusRunning;

    static const std::uint8_t want[14] = {
        0x3BU, 0x20U, 0x25U, 0x3BU, 0x21U, 0x26U, 0x3BU,
        0x22U, 0x27U, 0x3BU, 0x23U, 0x28U, 0x3BU, 0x20U };
    std::uint8_t  got[14];
    unsigned      at[14];
    unsigned      n = 0U;
    unsigned      seen = 0U;

    /* Every request is answered at once, so the 10 s gap is the divider alone. */
    for (unsigned pass = 0U; pass < 1250U * 15U && n < 14U; ++pass) {
        pollPass();
        if (fake::canFrameCount() != seen) {
            seen = static_cast<unsigned>(fake::canFrameCount());
            const fake::CanFrame* f = fake::lastCanFrame();
            CHECK_EQ(f->id, 0x600U);
            CHECK_EQ(f->dlc, 3U);
            CHECK_EQ(f->data[0], 0x02U);
            CHECK_EQ(f->data[1], 0x21U);
            got[n] = f->data[2];
            at[n]  = pass;
            ++n;
            const std::uint8_t lid = f->data[2];
            if (lid == 0x3BU)      { tpmsReply(0x21U, 0x20U, 0x21U, 0x1EU); }
            else if (lid < 0x25U)  { sensorReply(static_cast<std::uint8_t>(lid - 0x20U), 0x5AU, 0U, 0U); }
            else                   { diagReply(static_cast<std::uint8_t>(lid - 0x25U), 0U, 0U); }
            decodeOnce();
        }
    }
    CHECK_EQ(n, 14U);
    for (unsigned i = 0U; i < n && i < 14U; ++i) {
        CHECK_EQ(got[i], want[i]);
        if (i > 0U) { CHECK(at[i] - at[i - 1U] >= 1250U); }
    }
}

TEST(tpms_requests_stay_ten_seconds_apart_when_nothing_answers)
{
    primed();
    ram::app().canBusStatus = cfg::kBusRunning;
    unsigned seen = 0U;
    unsigned last = 0U;
    for (unsigned pass = 0U; pass < 1250U * 9U; ++pass) {
        pollPass();
        if (fake::canFrameCount() != seen) {
            seen = static_cast<unsigned>(fake::canFrameCount());
            if (seen > 1U) { CHECK(pass - last >= 1250U); }
            last = pass;
        }
    }
    CHECK(seen >= 8U);
}

TEST(tpms_never_requests_the_spare_21_24)
{
    primed();
    ram::app().canBusStatus = cfg::kBusRunning;
    unsigned seen = 0U;
    for (unsigned pass = 0U; pass < 1250U * 30U; ++pass) {
        pollPass();
        if (fake::canFrameCount() != seen) {
            seen = static_cast<unsigned>(fake::canFrameCount());
            CHECK(fake::lastCanFrame()->data[2] != 0x24U);
            CHECK(fake::lastCanFrame()->data[2] != 0x29U);
        }
    }
}

TEST(tpms_reply_is_matched_by_the_echoed_local_id)
{
    livePressures();
    armStep(1U, 0U);                          /* 21 20 outstanding */
    CHECK_EQ(ram::app().customSignals.tpmsLid, 0x20U);

    sensorReply(1U, 0x5AU, 1U, 2U);           /* answer for 21 21: not ours */
    decodeOnce();
    CHECK_EQ(ram::app().customSignals.tpmsPending, 1U);
    diagReply(0U, 0U, 1U);                    /* 21 25: not ours */
    decodeOnce();
    CHECK_EQ(ram::app().customSignals.tpmsPending, 1U);
    tpmsReply(0x21U, 0x20U, 0x21U, 0x1EU);    /* 21 3B: not ours */
    decodeOnce();
    CHECK_EQ(ram::app().customSignals.tpmsPending, 1U);
    CHECK_EQ(ram::app().customSignals.tyreTemp[1], 0xFFU);
    CHECK_EQ(ram::app().customSignals.tyreTemp[0], 0xFFU);

    sensorReply(0U, 0x5AU, 0U, 0U);           /* the right one */
    decodeOnce();
    CHECK_EQ(ram::app().customSignals.tpmsPending, 0U);
    CHECK_EQ(ram::app().customSignals.tyreTemp[0], 0x5AU);
}

TEST(tpms_pressure_reply_does_not_close_a_sensor_request)
{
    livePressures();
    armStep(1U, 2U);
    tpmsReply(0x30U, 0x30U, 0x30U, 0x30U);
    decodeOnce();
    CHECK_EQ(ram::app().customSignals.tpmsPending, 1U);
    CHECK_EQ(ram::app().customSignals.tyreKpa[0], 228U);   /* unchanged */
}

TEST(tpms_temperature_is_raw_byte_passed_through)
{
    livePressures();
    static const std::uint8_t in[4]  = { 0x5AU, 0x28U, 0xA0U, 0xA1U };
    static const std::uint8_t out[4] = { 0x5AU, 0x28U, 0xA0U, 0xA1U };
    for (std::uint8_t k = 0U; k < 4U; ++k) {
        armStep(1U, 0U);
        sensorReply(0U, in[k], 0U, 0U);
        decodeOnce();
        publishPos(0U);
        CHECK_EQ(b7()[3], out[k]);
    }
    armStep(1U, 0U);
    sensorReply(0U, 0xFFU, 0U, 0U);
    decodeOnce();
    publishPos(0U);
    CHECK_EQ(b7()[3], 0xFFU);
}

TEST(tpms_each_flag_bit_has_its_own_source)
{
    livePressures();

    armStep(1U, 1U);  sensorReply(1U, 0x5AU, 1U, 0U);  decodeOnce();
    publishPos(1U);
    CHECK_EQ(b7()[4], 0x01U);                 /* battery low */

    armStep(1U, 1U);  sensorReply(1U, 0x5AU, 0U, 2U);  decodeOnce();
    publishPos(1U);
    CHECK_EQ(b7()[4], 0x02U);                 /* leak: trigger 2 */

    armStep(1U, 1U);  sensorReply(1U, 0x5AU, 2U, 1U);  decodeOnce();
    publishPos(1U);
    CHECK_EQ(b7()[4], 0x00U);                 /* other values: no flags */

    armStep(1U, 1U);  sensorReply(1U, 0x5AU, 0U, 0U, 2U);  decodeOnce();
    publishPos(1U);
    CHECK_EQ(b7()[3], 0x5AU);                 /* park mode: temp still sent */
    CHECK_EQ(b7()[4], 0x00U);

    armStep(2U, 1U);  diagReply(1U, 0x1FU, 0x01U);  decodeOnce();
    publishPos(1U);
    CHECK_EQ(b7()[4], 0x08U);                 /* low-pressure warning */

    armStep(1U, 1U);  sensorReply(1U, 0x5AU, 1U, 2U);  decodeOnce();
    publishPos(1U);
    CHECK_EQ(b7()[4], 0x0BU);                 /* all three; warning kept */

    armStep(2U, 1U);  diagReply(1U, 0x00U, 0xFEU);  decodeOnce();
    publishPos(1U);
    CHECK_EQ(b7()[4], 0x03U);                 /* byte3 bit0 = 0 clears the warning */
}

TEST(tpms_frame_with_temperature_and_flags_matches_the_vector)
{
    livePressures();
    armStep(1U, 0U);  sensorReply(0U, 0x5AU, 1U, 0U);  decodeOnce();
    armStep(2U, 0U);  diagReply(0U, 0U, 1U);  decodeOnce();
    publishPos(0U);

    const std::uint8_t want[10] = { 0xAAU, 0x55U, 0x05U, 0xB7U,
                                    0x00U, 0x00U, 0xE4U, 0x5AU, 0x09U, 0x02U };
    std::uint8_t wire[16];
    CHECK_EQ(hiworldFrame(wire, 0xB7U, b7(), 5U), 10U);
    CHECK_BYTES(wire, want, 10U);
}

TEST(tpms_no_sensor_wheel_overrides_temperature_and_flags)
{
    primed();
    armPollA();
    pollPass();
    tpmsReply(5U, 0x20U, 0x21U, 0x1EU);       /* LF: the placeholder 5 psi */
    decodeOnce();

    armStep(1U, 0U);  sensorReply(0U, 0x5AU, 1U, 2U);  decodeOnce();
    armStep(2U, 0U);  diagReply(0U, 0U, 1U);  decodeOnce();

    publishPos(0U);
    CHECK_EQ(b7()[1], 0xFFU);  CHECK_EQ(b7()[2], 0xFFU);
    CHECK_EQ(b7()[3], 0xFFU);
    CHECK_EQ(b7()[4], ram::kTyreFlagNoSignal);
}

TEST(tpms_sensor_extras_go_after_three_failed_reads_of_that_wheel)
{
    livePressures();
    armStep(1U, 3U);  sensorReply(3U, 0x50U, 1U, 2U);  decodeOnce();
    armStep(2U, 3U);  diagReply(3U, 0U, 1U);  decodeOnce();
    publishPos(3U);
    CHECK_EQ(b7()[4], 0x0BU);

    failRead(1U, 3U);  failRead(1U, 3U);      /* two misses: held */
    publishPos(3U);
    CHECK_EQ(b7()[3], 0x50U);
    CHECK_EQ(b7()[4], 0x0BU);

    failRead(1U, 3U);                         /* third: temp, bits 0/1 go */
    publishPos(3U);
    CHECK_EQ(b7()[3], 0xFFU);
    CHECK_EQ(b7()[4], 0x08U);                 /* bit 3 belongs to the other read */

    failRead(2U, 3U);  failRead(2U, 3U);
    publishPos(3U);
    CHECK_EQ(b7()[4], 0x08U);
    failRead(2U, 3U);
    publishPos(3U);
    CHECK_EQ(b7()[4], 0x00U);
}

TEST(tpms_a_good_read_resets_the_wheel_miss_count)
{
    livePressures();
    armStep(1U, 0U);  sensorReply(0U, 0x50U, 0U, 0U);  decodeOnce();
    failRead(1U, 0U);  failRead(1U, 0U);
    armStep(1U, 0U);  sensorReply(0U, 0x51U, 0U, 0U);  decodeOnce();
    failRead(1U, 0U);  failRead(1U, 0U);
    publishPos(0U);
    CHECK_EQ(b7()[3], 0x51U);
}

TEST(tpms_failures_of_one_wheel_leave_the_others_alone)
{
    livePressures();
    armStep(1U, 0U);  sensorReply(0U, 0x50U, 0U, 0U);  decodeOnce();
    armStep(1U, 1U);  sensorReply(1U, 0x52U, 0U, 0U);  decodeOnce();
    failRead(1U, 0U);  failRead(1U, 0U);  failRead(1U, 0U);
    publishPos(0U);  CHECK_EQ(b7()[3], 0xFFU);
    publishPos(1U);  CHECK_EQ(b7()[3], 0x52U);
}

TEST(tpms_negative_response_to_a_wheel_read_counts_as_a_miss)
{
    livePressures();
    armStep(1U, 2U);  sensorReply(2U, 0x50U, 0U, 0U);  decodeOnce();
    for (int i = 0; i < 3; ++i) {
        armStep(1U, 2U);
        raw500(0x03U, 0x7FU, 0x21U);
        decodeOnce();
        CHECK_EQ(ram::app().customSignals.tpmsPending, 0U);
    }
    publishPos(2U);
    CHECK_EQ(b7()[3], 0xFFU);
    CHECK(b7Live());   /* pressures unaffected */
}

TEST(tpms_wheel_read_failures_do_not_stale_the_pressures)
{
    livePressures();
    for (int i = 0; i < 6; ++i) { failRead(1U, 0U); }
    CHECK(b7Live());
    CHECK_EQ(ram::app().customSignals.tyreKpa[0], 228U);
}

TEST(tpms_stale_pressures_drop_every_wheels_extras)
{
    livePressures();
    for (std::uint8_t w = 0U; w < 4U; ++w) {
        armStep(1U, w);  sensorReply(w, 0x50U, 1U, 2U);  decodeOnce();
        armStep(2U, w);  diagReply(w, 0U, 1U);  decodeOnce();
    }
    for (int miss = 0; miss < 4; ++miss) { failRead(0U, 0U); }

    for (std::uint8_t w = 0U; w < 4U; ++w) {
        publishPos(w);
        CHECK_EQ(b7()[1], 0xFFU);  CHECK_EQ(b7()[2], 0xFFU);
        CHECK_EQ(b7()[3], 0xFFU);
        CHECK_EQ(b7()[4], ram::kTyreFlagNoSignal);
        CHECK_EQ(ram::app().customSignals.tyreTemp[w], 0xFFU);
        CHECK_EQ(ram::app().customSignals.tyreExtra[w], 0U);
    }
}

TEST(tpms_boot_state_has_no_extras)
{
    primed();
    for (std::uint8_t w = 0U; w < 4U; ++w) {
        CHECK_EQ(ram::app().customSignals.tyreTemp[w], 0xFFU);
        CHECK_EQ(ram::app().customSignals.tyreExtra[w], 0U);
        publishPos(w);
        CHECK_EQ(b7()[3], 0xFFU);
        CHECK_EQ(b7()[4], ram::kTyreFlagNoSignal);
    }
    CHECK_EQ(ram::app().customSignals.tpmsStep, 0U);
    CHECK_EQ(ram::app().customSignals.tpmsWheel, 0U);
}

TEST(tpms_a_short_pci_length_is_not_read_past)
{
    livePressures();
    armStep(1U, 0U);
    const std::uint8_t d[8] = { 0x03U, 0x61U, 0x20U, 0x20U, 0x5AU, 0x00U, 0x14U, 0x00U };
    pushFrame(logic::canid::kJeepVendorEcho, d);
    decodeOnce();
    CHECK_EQ(ram::app().customSignals.tpmsPending, 1U);   /* too short: ignored */
    CHECK_EQ(ram::app().customSignals.tyreTemp[0], 0xFFU);
}

