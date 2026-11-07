/* ============================================================================
 * custom_tx.cpp -- see custom_tx.hpp.
 *
 * Spec: docs/TELEMETRY-V2-SPEC.md sections 3 (payloads), 6 (staleness),
 * 7 (scheduler) and 10 (test vectors, checked in tests/host/test_custom_tx.cpp).
 *
 * The decoders store raw bytes and stamp their source; THIS file is the one
 * place a dead source turns into a sentinel. Every packer reads the source's
 * alive bit from the tick's srcAlive and writes either the reading or the
 * field's "no value" code -- nothing is ever withheld to hide a bad field.
 * ==========================================================================*/

#include "custom_tx.hpp"

#include "app_config.hpp"
#include "custom_decode.hpp"
#include "head_unit.hpp"
#include "ram_map.hpp"

namespace logic {
namespace {

constexpr std::uint32_t kTickMs       = 100U;
constexpr std::uint8_t  kTicksPerSec  = 10U;
/* A loop that fell this far behind resynchronises instead of replaying the
 * missed ticks in a burst. */
constexpr std::uint32_t kResyncMs     = 1000U;
/* 0xB2 is repeated after this many ticks without a change (1 s). */
constexpr std::uint8_t  kStateRepeatTicks = 10U;

void put16(volatile std::uint8_t* p, std::uint16_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v & 0xFFU);
}

void put24(volatile std::uint8_t* p, std::uint32_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<std::uint8_t>(v & 0xFFU);
}

bool alive(std::uint16_t mask, std::uint8_t src) noexcept
{
    return (mask & (1U << src)) != 0U;
}

std::uint8_t bitIf(bool on, std::uint8_t bit) noexcept
{
    return on ? bit : 0U;
}

/* ---- 0xB3 FAST ------------------------------------------------------------
 * speed 0.1 km/h = round(raw x 10 / 128) = (5 raw + 32) / 64; raw FFFE gives
 * 5120 (512.0 km/h), never the sentinel. rpm verbatim. */
void packFast(std::uint16_t a, std::uint16_t flow, std::uint8_t load) noexcept
{
    const ram::CustomSignals& c = ram::app().customSignals;
    volatile std::uint8_t* const f = ram::app().customFrames.b3;

    const bool live002 = alive(a, ram::kSrc002);
    std::uint16_t speed = ram::kCustomUnknown16;
    std::uint16_t rpm   = ram::kCustomUnknown16;
    if (live002) {
        rpm = c.rpm;
        if (c.speedRaw != ram::kCustomUnknown16) {
            speed = static_cast<std::uint16_t>(
                ((5U * static_cast<std::uint32_t>(c.speedRaw)) + 32U) / 64U);
        }
    }
    put16(&f[0], speed);
    put16(&f[2], rpm);
    f[4] = load;
    put16(&f[5], flow);
    f[7] = alive(a, ram::kSrc02C) ? c.oil : ram::kCustomUnknown8;

    std::uint16_t boost = ram::kCustomUnknown16;
    if (alive(a, ram::kSrc2C0) && (c.boost != ram::kCustomUnknown8)) {
        boost = static_cast<std::uint16_t>(static_cast<std::uint16_t>(c.boost) * 8U);
    }
    put16(&f[8], boost);

    /* Accelerator pedal % = b2 x 0.4 = (2 b2 + 2) / 5 rounded half up,
     * clamped to 100; b2 FF (all-FF frame) -> FF. The driver's request, not
     * the engine load (that is f[4]). */
    std::uint8_t pedal = ram::kCustomUnknown8;
    if (alive(a, ram::kSrc02C) && (c.pedal != ram::kCustomUnknown8)) {
        std::uint32_t pct = ((2U * static_cast<std::uint32_t>(c.pedal)) + 2U) / 5U;
        if (pct > 100U) {
            pct = 100U;
        }
        pedal = static_cast<std::uint8_t>(pct);
    }
    f[10] = pedal;
}

/* ---- 0xB2 STATE -------------------------------------------------------- */
void packState(std::uint16_t a) noexcept
{
    const ram::CustomSignals& c = ram::app().customSignals;
    volatile std::uint8_t* const f = ram::app().customFrames.b2;

    const bool l000 = alive(a, ram::kSrc000);
    const bool l002 = alive(a, ram::kSrc002);
    const bool l003 = alive(a, ram::kSrc003);
    const bool l006 = alive(a, ram::kSrc006);
    const bool l012 = alive(a, ram::kSrc012);
    const bool l013 = alive(a, ram::kSrc013);
    const bool l0D0 = alive(a, ram::kSrc0D0);
    const bool l1B2 = alive(a, ram::kSrc1B2);

    const std::uint8_t gear = l003 ? customGearEnum(c.lever, c.prndl)
                                   : ram::kCustomUnknown8;

    /* S0 POWER */
    std::uint8_t s0 = l000 ? customIgnitionEnum(c.ign) : ram::kIgnEnumUnknown;
    s0 = static_cast<std::uint8_t>(s0
        | bitIf(l002 && (c.engineRun != 0U),          0x08U)
        | bitIf(l006 && ((c.rapByte & 0x02U) != 0U),  0x10U)
        | bitIf(l012 && ((c.keyByte & 0x02U) != 0U),  0x20U)
        | bitIf(gear == 1U,                           0x40U)   /* reverse = gear R */
        | bitIf(l0D0 && ((c.parkByte & 0x80U) != 0U), 0x80U));

    /* S1 BODY: brake, belt unfastened, FL FR RR RL hatch (CAN order). */
    std::uint8_t s1 = 0U;
    if (l003) {
        s1 = static_cast<std::uint8_t>(s1 | (c.brake & 0x01U));
    }
    if (l013) {
        s1 = static_cast<std::uint8_t>(s1
            | bitIf((c.beltByte & 0x08U) != 0U, 0x02U)
            | static_cast<std::uint8_t>((c.doorByte & 0x1FU) << 2));
    }

    /* S2 LIGHTS: debounced indicators + raw lamp bits 3..7. */
    std::uint8_t s2 = 0U;
    if (l006) {
        const std::uint8_t ind = c.indActive;
        const bool hazard = (ind & ram::kIndHazard) != 0U;
        s2 = static_cast<std::uint8_t>((c.lights & 0xF8U)
            | bitIf(hazard || ((ind & ram::kIndLeft) != 0U),  0x01U)
            | bitIf(hazard || ((ind & ram::kIndRight) != 0U), 0x02U)
            | bitIf(hazard, 0x04U));
    }

    /* S3 ASSIST / LAMPS A and S4 LAMPS B / 4WD, from 0x003 b0/b1 and 0x1B2. */
    std::uint8_t s3 = 0U;
    std::uint8_t s4 = l1B2 ? customTransferEnum(c.transfer) : ram::kTransferUnknown;
    if (l003) {
        const std::uint8_t b0 = c.lamps0;
        const std::uint8_t b1 = c.lamps1;
        s3 = static_cast<std::uint8_t>(
              bitIf((b1 & 0x80U) != 0U, 0x01U)    /* cruise on       */
            | bitIf((b1 & 0x40U) != 0U, 0x02U)    /* cruise engaged  */
            | bitIf((b1 & 0x20U) != 0U, 0x04U)    /* A/C compressor  */
            | bitIf((b0 & 0x04U) != 0U, 0x08U)    /* ESP off         */
            | bitIf((b1 & 0x01U) != 0U, 0x10U)    /* MIL             */
            | bitIf((b1 & 0x04U) != 0U, 0x20U)    /* oil lamp        */
            | bitIf((b0 & 0x80U) != 0U, 0x40U)    /* charge lamp     */
            | bitIf((b0 & 0x01U) != 0U, 0x80U));  /* ABS lamp        */
        s4 = static_cast<std::uint8_t>(s4
            | bitIf((b0 & 0x02U) != 0U, 0x08U)    /* ESP lamp        */
            | bitIf((b1 & 0x08U) != 0U, 0x10U)    /* wait-to-start   */
            | bitIf((b1 & 0x10U) != 0U, 0x20U));  /* water in fuel   */
    }

    /* S6 SRC: the eight sources STATE depends on. */
    const std::uint8_t s6 = static_cast<std::uint8_t>(
          bitIf(l000, 0x01U) | bitIf(l002, 0x02U) | bitIf(l003, 0x04U)
        | bitIf(l006, 0x08U) | bitIf(l013, 0x10U) | bitIf(l0D0, 0x20U)
        | bitIf(l1B2, 0x40U) | bitIf(l012, 0x80U));

    f[0] = s0;
    f[1] = s1;
    f[2] = s2;
    f[3] = s3;
    f[4] = s4;
    f[5] = gear;
    f[6] = s6;
}

/* ---- the slow payloads ------------------------------------------------- */
void packSlow(std::uint16_t a) noexcept
{
    ram::AppRam&              r = ram::app();
    const ram::CustomSignals& c = r.customSignals;
    ram::CustomFrames&        f = r.customFrames;

    /* 0xB4 ENGINE */
    const bool l159 = alive(a, ram::kSrc159);
    f.b4[0] = alive(a, ram::kSrc02C) ? c.coolant : ram::kCustomUnknown8;
    f.b4[1] = alive(a, ram::kSrc015) ? c.volts : ram::kCustomUnknown8;
    f.b4[2] = l159 ? c.fuelPct : ram::kCustomUnknown8;
    put16(&f.b4[3], l159 ? c.fuelDecilitres : ram::kCustomUnknown16);

    /* 0xB5 AMBIENT */
    const bool l014 = alive(a, ram::kSrc014);
    put16(&f.b5[0], static_cast<std::uint16_t>(
        l014 ? c.outsideDeciC : ram::kCustomUnknownS16));
    f.b5[2] = l159 ? c.minutesOff : ram::kCustomUnknown8;

    /* 0xB7 TYRE: ONE corner per frame, rotated by customTpmsPoll() every 5 s.
     * Layout unchanged from feature/tpms-temp (docs/TPMS-POLL-SPEC.md). */
    {
        const std::uint8_t  pos  = c.tpmsPosition;
        const std::uint16_t kpa  = c.tyreKpa[pos];
        const bool          live = (kpa != ram::kCustomUnknown16);

        f.b7[0] = pos;
        put16(&f.b7[1], kpa);
        f.b7[3] = live ? c.tyreTemp[pos] : ram::kCustomUnknown8;
        f.b7[4] = live ? c.tyreExtra[pos] : ram::kTyreFlagNoSignal;
    }

    /* 0xB8 TRIP */
    put24(&f.b8[0], l014 ? c.odometerDeciKm : ram::kCustomUnknown24);
    const bool l1B4 = alive(a, ram::kSrc1B4);
    f.b8[3] = l1B4 ? c.compassSector : ram::kCustomUnknown8;
    put16(&f.b8[4], l1B4 ? c.headingDeg : ram::kCustomUnknown16);
}

void packHeartbeat() noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    volatile std::uint8_t* const f = ram::app().customFrames.b0;

    f[0] = ram::kCustomLayoutVersion;
    put16(&f[1], kCustomCapability);
    f[3] = c.heartbeat;
}

bool stateChanged() noexcept
{
    const ram::CustomSignals& c = ram::app().customSignals;
    const volatile std::uint8_t* const f = ram::app().customFrames.b2;
    for (std::uint8_t i = 0U; i < sizeof(c.stateLast); ++i) {
        if (c.stateLast[i] != f[i]) {
            return true;
        }
    }
    return false;
}

}  // namespace

/* ============================================================================
 * Prime the signals and pack every payload once, so the first frames the
 * scheduler can reach say "no value". Nothing is queued: the first 0xB3 and
 * 0xB2 leave on the first 100 ms tick, the first heartbeat on the tenth.
 * ==========================================================================*/
void customPrimeFrames() noexcept
{
    customPrimeSignals();

    ram::CustomSignals& c = ram::app().customSignals;
    packFast(0U, ram::kCustomUnknown16, ram::kCustomUnknown8);
    packState(0U);
    packSlow(0U);
    packHeartbeat();
    /* Forces a 0xB2 send on the first tick even if nothing changed. */
    c.stateAge = kStateRepeatTicks;
}

void customTick100() noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint32_t now = ram::app().sched.counter;

    const std::uint16_t a = customAgeSources(now);
    customIndicatorTick(now);

    std::uint16_t qAvg = ram::kCustomUnknown16;
    const std::uint16_t flow = customFlowClose(now, &qAvg);
    c.flowCentiLph = flow;
    c.loadPct = alive(a, ram::kSrc002) ? customLoad(c.rpm, qAvg) : ram::kCustomUnknown8;

    packFast(a, flow, c.loadPct);
    packState(a);

    /* FAST unconditionally: it is the liveness frame. */
    svc::hu::requestPriority(kCmdFast);

    if (c.stateAge < kStateRepeatTicks) {
        c.stateAge = static_cast<std::uint8_t>(c.stateAge + 1U);
    }
    if (stateChanged() || (c.stateAge >= kStateRepeatTicks)) {
        const volatile std::uint8_t* const f = ram::app().customFrames.b2;
        for (std::uint8_t i = 0U; i < sizeof(c.stateLast); ++i) {
            c.stateLast[i] = f[i];
        }
        c.stateAge = 0U;
        svc::hu::requestPriority(kCmdState);
    }
}

void customTick1000() noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;

    packSlow(c.srcAlive);
    packHeartbeat();
    c.heartbeat = static_cast<std::uint8_t>(c.heartbeat + 1U);

    /* 0xB0 is a REQUEST-lane row: the round-robin never sends it, this does,
     * once a second. requestSend() is a no-op outside a jeep table. */
    svc::hu::requestSend(kCmdHeartbeat);
}

/* ============================================================================
 * The 8 ms entry point. Drift-free: nextTickMs advances by exactly 100 per
 * tick, so the average rate is 10 Hz with at most one 8 ms pass of jitter.
 * A loop that fell more than a second behind (a stalled flash write, a test
 * that jumps the clock) resynchronises rather than bursting.
 * ==========================================================================*/
void customPublish() noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint32_t now = ram::app().sched.counter;

    if (static_cast<std::int32_t>(now - c.nextTickMs) < 0) {
        return;
    }
    if ((now - c.nextTickMs) >= kResyncMs) {
        c.nextTickMs = now;
    }
    c.nextTickMs = c.nextTickMs + kTickMs;

    customTick100();

    c.tickCount = static_cast<std::uint8_t>(c.tickCount + 1U);
    if (c.tickCount >= kTicksPerSec) {
        c.tickCount = 0U;
        customTick1000();
    }
}

}  // namespace logic
