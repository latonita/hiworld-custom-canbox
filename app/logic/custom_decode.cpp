/* ============================================================================
 * custom_decode.cpp -- see custom_decode.hpp.
 *
 * Evidence for every identifier, byte, scale and sentinel below:
 * docs/JEEP-WK-CAN-ID-byte-map.md, quoted per function. The destination side
 * -- why these units and why these clamps -- is docs/CUSTOM-TX-SPEC.md
 * section 3 and custom_tx.hpp.
 *
 * THIS TU IS COMPILED AND TYPE-CHECKED IN EVERY BUILD -- tools/build.py and
 * tools/test.py glob app/logic/, so a benz image compiles these decoders too.
 * The calls are then dropped by `if constexpr (!cfg::kIsBenz)` at the dispatch
 * site and the code itself by --gc-sections at link, which is the point: a
 * mistake here shows up on every build rather than waiting for someone to
 * build the other variant.
 * ==========================================================================*/

#include "custom_decode.hpp"

#include <cstdint>

#include "app_config.hpp"
#include "can_bus.hpp"
#include "can_decode.hpp"
#include "ram_map.hpp"
#include "ambient_temp.hpp"

namespace logic {
namespace {

/* ---------------------------------------------------------------------------
 * The 0x000 b0 ignition ladder. Evidence: id 0x000, Confirmed.
 *     00 wake/uninitialised   01 off   63 ACC   85 ignition ON   A9 crank
 * Anything else is left alone rather than guessed at: the ladder above covers
 * every value in every capture, so an unknown byte means the capture set is
 * incomplete, not that the key turned off.
 * ------------------------------------------------------------------------- */
constexpr std::uint8_t kIgnWake  = 0x00U;
constexpr std::uint8_t kIgnOff   = 0x01U;
constexpr std::uint8_t kIgnAcc   = 0x63U;
constexpr std::uint8_t kIgnRun   = 0x85U;
constexpr std::uint8_t kIgnCrank = 0xA9U;

/* ---------------------------------------------------------------------------
 * The engine-running hysteresis band. CUSTOM-TX-SPEC.md section 5.
 *
 * Neither rpm nor the ignition byte works alone: grouped by ignition state
 * over jeep-can-b-logs/playback.log, CRANK (0xA9) reaches 410 rpm and RUN
 * (0x85) also shows 0. Idle on this 3.0 CRD is 577-580, so 300/500 is a clean
 * gap between crank-max and idle-min.
 *
 * Asymmetric ON PURPOSE: slow to claim the engine is running (three
 * consecutive 0x002 frames, 150 ms at the 50 ms cadence), instant to give up.
 * A false "running" drives a consumer to show a live engine on a dead one; a
 * false "stopped" only blanks a gauge for 150 ms.
 * ------------------------------------------------------------------------- */
constexpr std::uint16_t kRunSetRpm      = 500U;
constexpr std::uint16_t kRunClearRpm    = 300U;
constexpr std::uint8_t  kRunStreakNeeded = 3U;

/* 0x1A7 fuel flow (spec section 10): the averaging window, the longest
 * inter-frame gap still integrated (three missed 100 ms frames), and the top
 * of the published range -- 0xFFFF is the sentinel, so 0xFFFE is the ceiling.
 * It is DEFENSIVE: Q <= 0xFFFE gives at most 5120 deci-L/h (512.0 L/h), so the
 * clamp cannot bind today; it is there so a future change to the divisor can
 * never make a reading collide with the sentinel. */
constexpr std::uint32_t kFlowWindowMs = 1000U;
constexpr std::uint32_t kFlowMaxGapMs = 300U;
constexpr std::uint32_t kFlowMaxDeci  = 0xFFFEU;

/* 0x003 b4 is the ASCII code of the character the cluster displays. */
constexpr std::uint8_t kAsciiR = 0x52U;   /* 'R' */

/* 0x0D0 b0 bit 7. Evidence: id 0x0D0, Confirmed by the 2026-10-13 stationary
 * test (04 00 released -> 84 00 lever pulled). MASK BIT 7 ONLY: the 0x04
 * baseline bit was set throughout that session with the brake released, so
 * testing the whole byte would report the brake permanently engaged. */
constexpr std::uint8_t kParkBrakeBit = 0x80U;

/* 0x02C b2, accelerator pedal: 0.4 %/count, 250 = 100 %. Evidence: byte map,
 * confirmed (r = 0.9995 against the TCM pedal reading). So pct = b2 * 2 / 5,
 * done as (2 * b2 + 2) / 5 for round-half-up. */
constexpr std::uint32_t kPedalNum   = 2U;
constexpr std::uint32_t kPedalDen   = 5U;
constexpr std::uint32_t kPedalRound = 2U;

/* Engine load from the injection quantity (byte map, "Derived values"):
 *   inj_mm3_stroke = 43.4 * Q / rpm,  load % = 100 * inj / 90
 *   => load % = 4340 * Q / (90 * rpm), rounded half-up in integers.
 * Below kLoadMinRpm (cranking / stalling) the quotient is meaningless. Q must
 * also be fresh: 0x1A7 runs at ~100 ms, so older than kLoadMaxAgeMs means the
 * stream stopped. 4340 * 65534 = 284 M, inside u32. */
constexpr std::uint32_t kLoadNum      = 4340U;
constexpr std::uint32_t kLoadDen      = 90U;
constexpr std::uint16_t kLoadMinRpm   = 300U;
constexpr std::uint32_t kLoadMaxAgeMs = 500U;

/* 0x159 b4 is a hard validity gate: b4 == 1 <-> b5 == 0xFF is not a reading.
 * True with zero exceptions across every log in the repo. */
constexpr std::uint8_t kFuelGateInvalid = 0x01U;

/* b5 is litres at 0.5 L/count, and the tank is 77.6 L = 155.2 counts. Percent
 * is therefore raw / 155.2 * 100 = raw * 0.6443, done in integer with a
 * round-half-up rather than a truncation. The widest intermediate is
 * 255 * 6443 = 1 642 965, comfortably inside 32 bits. */
constexpr std::uint32_t kFuelPctNum   = 6443U;
constexpr std::uint32_t kFuelPctDen   = 10000U;
constexpr std::uint32_t kFuelPctRound = kFuelPctDen / 2U;
constexpr std::uint8_t  kPercentMax   = 100U;

/* b5 is 0.5 L/count and the 0xB8 field is decilitres, so x5 exactly. */
constexpr std::uint16_t kFuelDeciPerCount = 5U;

/* ---------------------------------------------------------------------------
 * Temperatures on this block are one byte biased +40, and 0xFF IS THE RESERVED
 * "not available" CODE for every one of them (CUSTOM-TX-SPEC.md section 3,
 * amended 2026-10-21; the consumer decodes it as
 * XyautoProtocol.TEMP_NOT_AVAILABLE). The cost is that exactly 215 degC is
 * unrepresentable -- the usable top is 0xFE = 214 degC, which no coolant or
 * intake reaches.
 *
 * Two separate reasons a source byte at or above 0x80 is rejected, and the
 * tighter one is what the constant encodes:
 *   - 0xFF + 40 TRUNCATES to 0x27, which decodes as -1 degC: a plausible-
 *     looking lie, and the trap that makes blind biasing wrong. Anything from
 *     0xD7 up biases into or past the reserved code.
 *   - a byte at or above 0x80 would be a two's-complement negative, an
 *     encoding never observed on this car.
 * 0x80 covers both, so that is the gate. The sample is published as the
 * reserved code rather than held: an unavailable sensor must read "unknown",
 * not "whatever it said last".
 * ------------------------------------------------------------------------- */
/* 0x015 b1 is 0.1 V/count and the 0xB4 field is centivolts: x10 exactly. */
constexpr std::uint16_t kCentiVoltsPerCount = 10U;

/* 0x002 b2:b3 is km/h x128. Rounded, not truncated -- the +64 is half an LSB
 * of the destination. */
constexpr std::uint16_t kSpeedShift = 7U;    /* /128 */
constexpr std::uint16_t kSpeedHalf  = 64U;

/* ---------------------------------------------------------------------------
 * 0x1B4, the compass. Evidence: id 0x1B4, direction and heading Confirmed
 * against two independent sessions (2026 n=2953, 2019 n=1584).
 *
 * b0 is an 8-way sector enum 0=N..7=NW and b1 is magnetic heading at
 * 2 deg/count, so the only legal values are b0 <= 7 and b1 <= 179 (358 deg).
 * The gate below tests BOTH TOGETHER and rejects the sample whole: a b0 of 9
 * means the byte no longer means what the table says, and masking it to 1
 * would confidently publish "NE".
 * ------------------------------------------------------------------------- */
constexpr std::uint8_t  kCompassSectorMax = 7U;
constexpr std::uint8_t  kHeadingCountMax  = 179U;   /* 179 * 2 = 358 deg */
constexpr std::uint16_t kDegPerCount      = 2U;

/* ---------------------------------------------------------------------------
 * TPMS over KWP2000. docs/TPMS-POLL-SPEC.md.
 *
 * The request PDU is TWO bytes -- service 0x21, local identifier 0x3B -- under
 * an ISO-TP single-frame PCI of 0x02. DLC 3, unpadded, which is what ISO 15765
 * asks for and what this module was proven to accept.
 *
 * THE TRAP: the PDU is `21 3B`, NOT `21 21 3B`. A builder that re-emits the
 * service byte for a service-qualified identifier sends three bytes and EVERY
 * MODULE ANSWERS NOTHING. That cost a sibling project a debugging session.
 * ------------------------------------------------------------------------- */
constexpr std::uint8_t kTpmsRequestDlc = 3U;
constexpr std::uint8_t kTpmsRequestPci = 0x02U;   /* single frame, 2 PDU bytes */
constexpr std::uint8_t kTpmsService    = 0x21U;   /* readDataByLocalIdentifier */

/* The three requests, one every 10 s, in a repeating cycle:
 *   A  21 3B        all four pressures
 *   B  21 20+n      wheel n: temperature, battery, trigger
 *   C  21 25+n      wheel n: diagnostic DB (faults, low-pressure warning)
 * after C the wheel n advances. 21 24 (the spare) is never read. */
constexpr std::uint8_t kTpmsLidPressures  = 0x3BU;
constexpr std::uint8_t kTpmsLidSensorBase = 0x20U;
constexpr std::uint8_t kTpmsLidDiagBase   = 0x25U;
constexpr std::uint8_t kTpmsStepPressures = 0U;
constexpr std::uint8_t kTpmsStepSensor    = 1U;
constexpr std::uint8_t kTpmsStepDiag      = 2U;

/* Replies: [PCI] 61 <lid> ... . Trust the PCI nibble, never the DLC -- the
 * byte after the declared length is whatever was in the responder's buffer
 * last, and it is neither data nor zero. Offsets below are frame bytes
 * (d[0] = PCI, d[1] = 0x61). */
constexpr std::uint8_t kTpmsReplyService = 0x61U;   /* 0x21 + 0x40 = positive    */
constexpr std::uint8_t kKwpNegative      = 0x7FU;
constexpr std::size_t  kTpmsFirstPsiByte = 3U;
constexpr std::uint8_t kTpmsPressuresPdu = 6U;      /* 61 3B p1 p2 p3 p4         */
constexpr std::uint8_t kTpmsSensorPdu    = 6U;      /* 61 2n P T X S             */
constexpr std::uint8_t kTpmsDiagPdu      = 4U;      /* 61 2n B2 B3               */
constexpr std::uint8_t kTpmsPduMax       = 7U;      /* single frame holds 7      */

/* 21 2n: 61 2n P T X S M. Bytes counted from the 0x61 as byte 0, so frame
 * offset = byte + 1.
 *   byte2 pressure (raw-1 psi, ignored: pressures come from 21 3B)
 *   byte3 air temperature, degC = raw - 40   <- INFERRED, not confirmed
 *   byte5 bits0-1 frame number (ignored), bits2-3 battery (1 = low),
 *         bits4-5 transmission trigger (2 = Pressure Loss),
 *         bits6-7 sensor state (2 = park mode; ignored, temp still sent)
 * The temperature offset is the one place to change if the inference is wrong. */
constexpr std::size_t  kTpmsSensorTempByte  = 4U;   /* frame offset of byte3 */
constexpr std::size_t  kTpmsSensorStateByte = 6U;   /* frame offset of byte5 */
constexpr std::uint8_t kTpmsBattShift        = 2U;
constexpr std::uint8_t kTpmsBattLow          = 1U;
constexpr std::uint8_t kTpmsTrigShift        = 4U;
constexpr std::uint8_t kTpmsTrigPressureLoss = 2U;
constexpr std::uint8_t kTpmsTwoBits          = 0x03U;
/* raw temp above this is > 120 degC: not a tyre, publish "none". */
constexpr std::uint8_t kTpmsTempRawMax       = 160U;

/* 21 25+n: 61 2n+5 B2 B3, from the vendor diagnostic DB ONLY -- NOT VERIFIED
 * ON THE CAR.
 *   byte2 bit0 pressure sensor HW fault, bit1 pressure SW over/underflow,
 *         bit2 temp sensor HW fault, bit3 temp SW over/underflow,
 *         bit4 accel sensor HW fault   (none of these are decoded for output)
 *   byte3 bit0 "Tire n Low Pressure Warning"                                */
constexpr std::size_t  kTpmsDiagWarnByte = 4U;      /* frame offset of byte3 */
constexpr std::uint8_t kTpmsDiagWarnBit  = 0x01U;

/* Cadences, in 8 ms passes of the periodic-TX task. A uint8_t divider
 * saturates at 2.04 s, which is why the counters are 16-bit. */
constexpr std::uint16_t kTpmsPollPasses   = 1250U;  /* 10 s between requests    */
constexpr std::uint16_t kTpmsRotatePasses = 625U;   /*  5 s between corners     */
constexpr std::uint8_t  kTpmsReplyPasses  = 63U;    /* ~500 ms to answer        */
/* Four consecutive missed pressure polls (A) -- two minutes -- before the data
 * is called stale. The exchange was measured at 47/47 on a real drive, so this
 * is a backstop, not an expectation. */
constexpr std::uint8_t  kTpmsStaleMisses  = 4U;
/* Failed reads of one wheel's 21 2n (or 21 25+n) before its extras are dropped. */
constexpr std::uint8_t  kTpmsExtraMisses  = 3U;

/* psi -> kPa. The WCM reports plain psi at 1 psi/count; the head-unit contract
 * wants u16 kPa. x 6.894759, rounded. Widest intermediate 255 * 68948 =
 * 17 581 740, comfortably inside 32 bits. */
constexpr std::uint32_t kKpaNum   = 68948U;
constexpr std::uint32_t kKpaDen   = 10000U;
constexpr std::uint32_t kKpaRound = kKpaDen / 2U;

/* A corner with no TPMS sensor is answered with a placeholder, not an error:
 * the WCM reports 5 psi (also seen: 0 and 0xFF). At or below this floor the
 * tyre is "absent" -- published as unknown kPa so 0xB7 flags it no-signal. */
constexpr std::uint8_t kTyreAbsentPsi = 5U;
constexpr std::uint8_t kTyreInvalidPsi = 0xFFU;

std::uint16_t psiToKpa(std::uint8_t psi) noexcept
{
    if ((psi <= kTyreAbsentPsi) || (psi == kTyreInvalidPsi)) {
        return ram::kCustomUnknown16;
    }
    return static_cast<std::uint16_t>(
        ((static_cast<std::uint32_t>(psi) * kKpaNum) + kKpaRound) / kKpaDen);
}


void setValid(std::uint8_t bit, bool live) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint8_t cur = c.valid;

    c.valid = live ? static_cast<std::uint8_t>(cur | bit)
                   : static_cast<std::uint8_t>(cur & static_cast<std::uint8_t>(~bit));
}

/* 0xB0 bit 2 means "the 0xB4 frame carries at least one valid field". Every
 * field has its own sentinel (coolant/intake 0xFF, fuel % 0xFF, volts 0xFFFF),
 * so the bit is derived from the fields, not owned by one source. Call after
 * any of them changes. */
void updateB4Valid() noexcept
{
    const ram::CustomSignals& c = ram::app().customSignals;
    const bool any = (c.coolantBias40 != ram::kCustomUnknown8)
                  || (c.intakeBias40  != ram::kCustomUnknown8)
                  || (c.fuelPct       != ram::kCustomUnknown8)
                  || (c.centiVolts    != ram::kCustomUnknown16);
    setValid(ram::kValidB4, any);
}

void setIgnBit(std::uint8_t bit, bool on) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint8_t cur = c.ignBits;

    c.ignBits = on ? static_cast<std::uint8_t>(cur | bit)
                   : static_cast<std::uint8_t>(cur & static_cast<std::uint8_t>(~bit));
}

std::uint16_t be16(const volatile std::uint8_t* d, std::size_t i) noexcept
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(d[i]) << 8) | d[i + 1U]);
}
/* Blank every corner and say so. Used before the first answer and after the
 * data goes stale -- the frame still goes out, because the scheduler's
 * keep-alive cannot be told to skip a slot, so every field has to be honest on
 * its own. */
void tpmsBlank() noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;

    for (std::uint8_t i = 0U; i < ram::kTyreCount; ++i) {
        c.tyreKpa[i]      = ram::kCustomUnknown16;
        c.tyreTemp[i]     = ram::kCustomUnknown8;
        c.tyreExtra[i]    = 0U;
        c.tyreMissTemp[i] = kTpmsExtraMisses;
        c.tyreMissDiag[i] = kTpmsExtraMisses;
    }
    setValid(ram::kValidB7, false);
}

/* One failed read (timeout or negative) of the request in flight. */
void tpmsFail() noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint8_t  lid = c.tpmsLid;

    c.tpmsPending = 0U;
    if (lid == kTpmsLidPressures) {
        if (c.tpmsFailures < kTpmsStaleMisses) {
            c.tpmsFailures = static_cast<std::uint8_t>(c.tpmsFailures + 1U);
        }
        if (c.tpmsFailures >= kTpmsStaleMisses) {
            tpmsBlank();
        }
    } else if (lid < kTpmsLidDiagBase) {
        const std::uint8_t n = static_cast<std::uint8_t>(lid - kTpmsLidSensorBase);
        if (n < ram::kTyreCount) {
            if (c.tyreMissTemp[n] < kTpmsExtraMisses) {
                c.tyreMissTemp[n] = static_cast<std::uint8_t>(c.tyreMissTemp[n] + 1U);
            }
            if (c.tyreMissTemp[n] >= kTpmsExtraMisses) {
                c.tyreTemp[n]  = ram::kCustomUnknown8;
                c.tyreExtra[n] = static_cast<std::uint8_t>(
                    c.tyreExtra[n] & ram::kTyreFlagLowPressure);
            }
        }
    } else {
        const std::uint8_t n = static_cast<std::uint8_t>(lid - kTpmsLidDiagBase);
        if (n < ram::kTyreCount) {
            if (c.tyreMissDiag[n] < kTpmsExtraMisses) {
                c.tyreMissDiag[n] = static_cast<std::uint8_t>(c.tyreMissDiag[n] + 1U);
            }
            if (c.tyreMissDiag[n] >= kTpmsExtraMisses) {
                c.tyreExtra[n] = static_cast<std::uint8_t>(
                    c.tyreExtra[n] & static_cast<std::uint8_t>(~ram::kTyreFlagLowPressure));
            }
        }
    }
}

}  // namespace

/* ============================================================================
 * Prime every signal to its "no value" encoding.
 *
 * MUST run before the first transmit. .bss is zeroed at startup, and zero is a
 * perfectly plausible reading for most of these: a zeroed coolant byte is
 * -40 degC, a zeroed volts field is 0.00 V and a zeroed throttle is a closed
 * pedal. The validity mask would say "not live", but section 4 of the spec is
 * explicit that a consumer may ignore 0xB0 -- so the FIELDS have to be honest
 * on their own too.
 * ==========================================================================*/
void customPrimeSignals() noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;

    c.speedKph       = ram::kCustomUnknown16;
    c.rpm            = ram::kCustomUnknown16;
    c.throttlePct    = ram::kCustomUnknown8;
    c.loadPct        = ram::kCustomLoadUnknown;
    c.flowLastQ      = ram::kCustomUnknown16;
    c.coolantBias40  = ram::kCustomUnknown8;
    c.intakeBias40   = ram::kCustomUnknown8;
    c.fuelPct        = ram::kCustomUnknown8;
    c.centiVolts     = ram::kCustomUnknown16;
    c.outsideDeciC   = ram::kCustomUnknownS16;
    c.fuelDecilitres = ram::kCustomUnknown16;

    /* A zeroed CustomSignals would publish "0.0 km, due north, 0 deg" as
     * though the car had said it -- and unlike -40 degC coolant those read as
     * MORE plausible than a real reading, not less. Priming matters most
     * exactly where the fabricated value looks reasonable. */
    c.odometerDeciKm = ram::kCustomUnknown24;
    c.headingDeg     = ram::kCustomUnknown16;
    c.compassSector  = ram::kCustomUnknown8;

    /* Fuel flow: a zeroed field would claim "0.0 L/h" -- a real reading (fuel
     * cut on overrun) the car never sent. 0xFFFF is the ONLY absent signal for
     * 0xBC, there being no validity bit. The window starts now so the first
     * close is a full second after boot, and no previous frame exists yet. */
    c.flowDeciLph    = ram::kCustomUnknown16;
    c.flowSumQdt     = 0U;
    c.flowSumDt      = 0U;
    c.flowLastMs     = 0U;
    c.flowHaveLast   = 0U;
    c.flowWindowMs   = ram::app().sched.counter;

    for (std::uint8_t i = 0U; i < ram::kTyreCount; ++i) {
        c.tyreKpa[i] = ram::kCustomUnknown16;
    }
    c.tpmsPollDiv   = 0U;
    c.tpmsRotateDiv = 0U;
    c.tpmsPosition  = ram::kTyrePosLeftFront;
    c.tpmsPending   = 0U;
    c.tpmsDeadline  = 0U;
    c.tpmsFailures  = kTpmsStaleMisses;   /* nothing heard yet == stale */
    c.tpmsLid       = 0U;
    c.tpmsStep      = kTpmsStepPressures;
    c.tpmsWheel     = 0U;
    for (std::uint8_t i = 0U; i < ram::kTyreCount; ++i) {
        c.tyreTemp[i]     = ram::kCustomUnknown8;
        c.tyreExtra[i]    = 0U;
        c.tyreMissTemp[i] = kTpmsExtraMisses;
        c.tyreMissDiag[i] = kTpmsExtraMisses;
    }

    c.ignBits      = 0U;
    c.valid        = 0U;    /* nothing is live until the car says so */
    c.runStreak    = 0U;
    c.cranking     = 0U;
    c.heartbeatDiv = 0U;
}

/* ============================================================================
 * 0x000 b0 -- ignition state.  Evidence: id 0x000, Confirmed.
 *
 * Two outputs. The ACC bit of 0xB2, and the crank latch the engine-running
 * state machine in customEngine() reads: cranking must VETO "running", because
 * a crank reaches 410 rpm and would otherwise trip a naive rpm threshold.
 *
 * This is a second reader of a frame the stock decoder already handles --
 * shared::decodeIgnition() derives ram::VehicleState::kFlagAccOn from the same
 * byte with a different rule ((b0 & 0xE0) > 0x10). The two deliberately do not
 * share: that rule is stock behaviour that gates the whole CAN TX block, and
 * the ladder here is the documented one. They agree on every observed value.
 *
 * Validity: 0xB2's bit goes live on the first 0x000 frame of any kind and
 * never goes back -- the ladder has no "unavailable" code, and 0x000 arriving
 * at all is itself the evidence that the source is fitted.
 * ==========================================================================*/
void customIgnition(std::uint8_t b0) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;

    c.cranking = (b0 == kIgnCrank) ? 1U : 0U;

    if ((b0 == kIgnAcc) || (b0 == kIgnRun) || (b0 == kIgnCrank)) {
        setIgnBit(ram::kIgnAccOn, true);
    } else if ((b0 == kIgnWake) || (b0 == kIgnOff)) {
        setIgnBit(ram::kIgnAccOn, false);
    } else {
        /* Outside the documented ladder: hold. */
    }

    setValid(ram::kValidB2, true);

    /* Cranking clears "running" immediately, without waiting for the next
     * 0x002. 0x000 runs at 100 ms and 0x002 at 50 ms, so this only ever wins
     * the race by a frame -- but the cost of being wrong in this direction is
     * a blanked gauge and in the other direction a false reading. */
    if (c.cranking != 0U) {
        c.runStreak = 0U;
        setIgnBit(ram::kIgnEngineRun, false);
    }
}

/* ============================================================================
 * 0x002 -- b0:b1 engine rpm at 1 rpm/count, b2:b3 road speed at raw/128 km/h.
 * Evidence: id 0x002, DLC 8, 50 ms, Confirmed. All-FF = unavailable.
 *
 * ONE SENTINEL GOVERNS BOTH FIELDS, which is what the spec says and what the
 * logs show: when the engine is off the whole frame goes to FF, not just the
 * rpm half. So b0:b1 == 0xFFFF invalidates speed too, and the 0xB3 validity
 * bit tracks exactly that.
 *
 * DO NOT compute 0xFFFF / 128 -- that is 512 km/h, a number a consumer accepts.
 *
 * Note this is a second reader of a frame jeepEngine() already handles for the
 * stock 0x32 message, where the rpm pair is copied VERBATIM because the car's
 * unavailable sentinel and the radio's blank-this-field sentinel happen to be
 * the same value. Here the sentinel is explicit instead of lucky.
 * ==========================================================================*/
void customEngine(const volatile std::uint8_t* d) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;

    const std::uint16_t rpmRaw   = be16(d, 0U);
    const std::uint16_t speedRaw = be16(d, 2U);
    const bool          rpmValid = (rpmRaw != ram::kCustomUnknown16);

    if (!rpmValid) {
        c.rpm      = ram::kCustomUnknown16;
        c.speedKph = ram::kCustomUnknown16;
        setValid(ram::kValidB3, false);
    } else {
        c.rpm = rpmRaw;
        c.speedKph = static_cast<std::uint16_t>(
            (speedRaw + kSpeedHalf) >> kSpeedShift);
        setValid(ram::kValidB3, true);
    }

    /* ---- the engine-running state machine, CUSTOM-TX-SPEC.md section 5 ----
     * Clear is immediate and unconditional; set needs three consecutive
     * qualifying frames. The band between kRunClearRpm and kRunSetRpm is
     * neither, so the bit HOLDS there -- that is the hysteresis. */
    if (!rpmValid || (rpmRaw < kRunClearRpm) || (c.cranking != 0U)) {
        c.runStreak = 0U;
        setIgnBit(ram::kIgnEngineRun, false);
    } else if (rpmRaw >= kRunSetRpm) {
        if (c.runStreak < kRunStreakNeeded) {
            c.runStreak = static_cast<std::uint8_t>(c.runStreak + 1U);
        }
        if (c.runStreak >= kRunStreakNeeded) {
            setIgnBit(ram::kIgnEngineRun, true);
        }
    } else {
        /* 300..499 rpm, not cranking: hold both the bit and the streak. */
    }
}

/* ============================================================================
 * 0x003 b4 -- the reverse bit of 0xB2.
 *
 * b4 is the ASCII code of the PRNDL character the cluster displays, and it is
 * used here rather than b2 for the same reason jeepGearFromAscii() uses it:
 * b2 is a lever bitfield in which every AutoStick manual range also reads
 * 0x84, so b2 cannot separate D from a limited range. For REVERSE specifically
 * either byte would do, but reading the same byte as the gear decoder means
 * the two can never disagree about which gear the car is in.
 *
 * Deliberately NOT derived from ram::VehicleState::kFlagReverse: that flag is
 * computed from b2's low nibble, it gates the stock steering-angle decode and
 * the reverse status line, and it is stock behaviour this build does not
 * touch. Two readers, one frame, no coupling.
 * ==========================================================================*/
void customGear(const volatile std::uint8_t* d) noexcept
{
    setIgnBit(ram::kIgnReverse, d[4] == kAsciiR);
}

/* ============================================================================
 * 0x014 b4:b5 -- outside air temperature, the value the cluster displays.
 * Evidence: verified on the car (Jeep WK, CAN-B 83.3k): u16 big-endian,
 * degC = raw * 0.01 - 40; `17 3E` = 5950 = 19.50 degC, cluster showed 19.
 * (The old reading of b4 alone as whole degC was wrong: b4 is the HIGH byte.)
 *
 * 0xFFFF means unavailable (bus waking, module not ready) and a frame shorter
 * than 6 bytes has no b5: both are skipped outright -- previous value AND
 * validity bit left as they were, so a wake-up burst of FFFF cannot blank a
 * good reading. The car already filters the value (follows the sensor while
 * driving, blocks rises when slow or stopped), so there is no filter here.
 *
 * Published at 0.1 degC (see ambientDeciC) as signed decidegrees; the cluster
 * itself shows whole degrees. Max is raw 0xFFFE = 615.34 degC = 6153, far inside s16 and clear of
 * the 0x8000 sentinel, so no clamp is needed.
 * ==========================================================================*/
void customAmbient(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    const std::uint16_t raw = ambientRaw(d);
    if (!ambientUsable(raw, dlc)) {
        return;                       /* keep value and validity */
    }

    ram::app().customSignals.outsideDeciC = ambientDeciC(raw);
    setValid(ram::kValidB5, true);
}

/* ============================================================================
 * 0x015 -- b1 battery voltage at 0.1 V/count.
 * Evidence: id 0x015. Voltage Confirmed (125 = 12.5 V key-on, 143-146 running,
 * 103 cranking). b2 is NOT intake air (the byte map lists it as an unknown
 * temperature), so the 0xB4 intake field is always the reserved 0xFF and b2
 * is not decoded here.
 *
 * 0xB4's validity bit means "at least one field is valid" (updateB4Valid);
 * each field also reports its own loss with its own sentinel.
 *
 * This is a third reader of 0x015 -- jeepSteering() takes b4:b5 and, in a benz
 * build, benzVoltage() takes b1.
 * ==========================================================================*/
void customVoltsIntake(const volatile std::uint8_t* d) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;

    /* Volts has no sentinel on this id: b1 is a live ADC reading in every
     * capture, including key-off (126 = 12.6 V) and cranking (103 = 10.3 V).
     * The happy consequence of x10 is that centivolts always move in steps of
     * ten, so the 0.1 V deadband the spec asks for is satisfied by
     * construction and needs no code. */
    c.centiVolts = static_cast<std::uint16_t>(static_cast<std::uint16_t>(d[1])
                                              * kCentiVoltsPerCount);
    updateB4Valid();
}

/* ============================================================================
 * 0x02C b1 -- coolant temperature. THE TRAP IN THIS WHOLE BLOCK.
 *
 * Evidence: id 0x02C, coolant high confidence, scale probable (a cold start
 * climbs 68 -> 132 raw = 28 -> 92 degC). The car already sends degC+40, which
 * is exactly the 0xB4 encoding, so a valid reading is a VERBATIM byte copy.
 *
 * And that was the trap. 0x02C sends all-FF for unavailable, and before the
 * block reserved a code every byte from 0x00 to 0xFF decoded inside the
 * accepted -40..215 degC -- so 0xFF read as a perfectly plausible 215 degC and
 * would have tripped the consumer's overheat alert.
 *
 * 0xFF IS NOW THE RESERVED "not available" CODE for every +40-biased
 * temperature in this block, which makes the verbatim copy correct in BOTH
 * directions: the car sends 0xFF for unavailable and 0xFF is also what this
 * block means by unavailable. The byte needs no special case at all. The
 * sentinel is still TESTED here, but only to drive the validity bit -- 0xB4's
 * bit belongs to this frame alone.
 *
 * 0xB0 bit 2 is NOT owned by coolant: it says the frame has at least one
 * valid field (updateB4Valid), so a dead coolant sensor at engine off does not
 * make the head unit withdraw fuel and volts that are still being sent.
 *
 * The test is b1 rather than the whole frame. The spec says "02C all-FF", and
 * in the logs the frame does go all-FF together -- but the coolant byte itself
 * is the only one this function reads, so testing it directly is both simpler
 * and strictly safer: it also catches a partial sentinel, which a whole-frame
 * test would pass straight through.
 * ==========================================================================*/
void customCoolant(const volatile std::uint8_t* d) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint8_t  b1 = d[1];

    c.coolantBias40 = b1;             /* already degC+40 -- no arithmetic, and
                                       * 0xFF passes through as the reserved
                                       * "not available" code */
    updateB4Valid();
}

/* ============================================================================
 * 0x0D0 b0 bit 7 -- parking brake engaged.
 * Evidence: id 0x0D0, Confirmed by the 2026-10-13 stationary test
 * (04 00 released -> 84 00 lever pulled).
 *
 * MASK BIT 7 ONLY. b0 bit 2 (the 0x04) was set throughout that session with
 * the brake RELEASED and is the released-state baseline on this truck; every
 * 2015 capture reads a flat 00 00. Testing b0 != 0 would report the brake
 * permanently engaged on a 2026 car and never engaged on a 2015 one.
 *
 * No validity bit of its own: this bit shares 0xB2 with ACC, and 0x0D0 has no
 * "unavailable" encoding. A car that never sends 0x0D0 reports the brake as
 * released, which is the one honest default available in a single bit.
 * ==========================================================================*/
void customParkBrake(const volatile std::uint8_t* d) noexcept
{
    setIgnBit(ram::kIgnParkBrake, (d[0] & kParkBrakeBit) != 0U);
}

/* ============================================================================
 * 0x159 -- b4 validity gate, b5 fuel level at 0.5 L/count.
 * Evidence: id 0x159, byte layout Confirmed across every log in the repo;
 * the litre scale is probable (the largest valid raw ever recorded is 154 =
 * 77.0 L against this truck's 77.6 L tank -- a percentage scale has no reason
 * to stop at 154/255, a litre scale stops exactly where the tank does).
 *
 * b4 IS A HARD GATE, not a hint: b4 == 1 <-> b5 == 0xFF, with zero exceptions
 * across every log, confirmed by two live transitions (ignition-off settles to
 * 00 FF FF FF 01 FF; key-on flips 05 FF FF FF 01 FF -> 05 FF FF FF 00 4D the
 * instant the key reaches ON). Without the gate, key-off would publish a full
 * tank at raw 255.
 *
 * TWO DESTINATIONS, ONE SOURCE. b5 feeds the litres in 0xB8 and the percentage
 * in 0xB4 byte 2. They share this gate but NOT a validity bit: a gated sample
 * clears 0xB8's bit, and 0xB4's bit stays set while any 0xB4 field is valid
 * (the held fuel % counts, and so does volts).
 *
 * ON THE GATE THE PREVIOUS FIGURE IS HELD, not blanked -- the one field in this
 * block that holds rather than publishing a sentinel. That is deliberate and it
 * is physical: the gate is asserted when the ignition goes off, and a parked
 * car's tank does not change. The last reading is the true one, and 0xB0 bit 5
 * is what tells a consumer it is no longer being refreshed. Before the FIRST
 * reading the primed sentinels stand, so a cold boot still says "unknown"
 * rather than "empty".
 *
 * Slosh needs no filtering here: every b5 change in the 2026 drive is exactly
 * +-1 count, already heavily damped at the sender.
 * ==========================================================================*/
void customFuel(const volatile std::uint8_t* d) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;

    if (d[4] == kFuelGateInvalid) {
        setValid(ram::kValidB8, false);
        updateB4Valid();
        return;                       /* hold both destinations */
    }

    const std::uint32_t raw = d[5];

    std::uint32_t pct = (raw * kFuelPctNum + kFuelPctRound) / kFuelPctDen;
    if (pct > kPercentMax) {
        pct = kPercentMax;
    }
    c.fuelPct = static_cast<std::uint8_t>(pct);

    c.fuelDecilitres = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(raw) * kFuelDeciPerCount);
    setValid(ram::kValidB8, true);
    updateB4Valid();
}

/* ============================================================================
 * 0x02C b2 -- accelerator pedal position, 0.4 %/count (250 = 100 %).
 * Evidence: byte map, confirmed (r = 0.9995 against the TCM pedal reading).
 * 0x02C is all-FF when unavailable, so b2 == 0xFF publishes the reserved
 * 0xFF rather than 102 %. Values above 250 clamp to 100.
 *
 * Replaces the earlier 0x2C0 b2 voltage-span guess; 0x2C0 is no longer read.
 * Not engine load -- see customEngineLoad().
 * ==========================================================================*/
void customThrottle(const volatile std::uint8_t* d) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint8_t  b2 = d[2];

    if (b2 == ram::kCustomUnknown8) {
        c.throttlePct = ram::kCustomUnknown8;
        return;
    }

    std::uint32_t pct = (static_cast<std::uint32_t>(b2) * kPedalNum + kPedalRound)
                        / kPedalDen;
    if (pct > kPercentMax) {
        pct = kPercentMax;
    }
    c.throttlePct = static_cast<std::uint8_t>(pct);
}

/* ============================================================================
 * 0x014 b0:b1:b2 -- odometer, 24-bit, 0.1 km/count.
 * Evidence: id 0x014, odometer Confirmed. FFFFFF means unavailable.
 *
 * VERBATIM, like coolant, and for the same happy reason: the car sends
 * 0.1 km/count big-endian and all-ones for unavailable, and all-ones is
 * already this block's unsigned sentinel. Source encoding and wire encoding
 * coincide, so there is no arithmetic and no special case -- only the validity
 * bit needs the test.
 *
 * DO NOT CONVERT TO WHOLE KILOMETRES. benzTrip() divides by 10 because the
 * Benz plugin's field is km; this block has no such constraint, and throwing
 * away the last digit costs a consumer the ability to measure a 100 m leg.
 *
 * DO NOT HOLD ON UNAVAILABLE either, which is the other thing benzTrip() does.
 * That behaviour exists only because the Benz plugin cannot render a blanked
 * field. This block can: write the sentinel through and clear the bit, which
 * is the plain section 4 "had it, lost it" rule.
 *
 * Full scale is 0xFFFFFE = 1 677 721.4 km, so there is no rollover concern on
 * any vehicle. No deadband either -- one count is 100 m, so the payload
 * changes every ~2.8 s at 130 km/h and less often below that. It is the
 * cheapest slot in the table.
 * ==========================================================================*/
void customOdometer(const volatile std::uint8_t* d) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;

    const std::uint32_t raw = (static_cast<std::uint32_t>(d[0]) << 16)
                            | (static_cast<std::uint32_t>(d[1]) << 8)
                            |  static_cast<std::uint32_t>(d[2]);

    c.odometerDeciKm = raw;           /* FFFFFF passes through as the sentinel */
    setValid(ram::kValidBB, raw != ram::kCustomUnknown24);
}

/* ============================================================================
 * 0x1B4 -- b0 compass sector, b1 magnetic heading at 2 deg/count.
 * Evidence: id 0x1B4, direction and heading Confirmed; b0 computed as
 * round(b1*2/45) mod 8 agrees in 99.46 % of 2 953 frames.
 *
 * BOTH BYTES ARE SENT even though b0 is derivable from b1. The 0.54 % that
 * disagree are all boundary-quantisation artefacts, and forwarding the car's
 * own sector byte means the consumer's compass rose agrees with the CLUSTER at
 * a boundary instead of flickering against a formula re-derived downstream.
 * One byte, zero extra slots.
 *
 * AND THE TWO ARE NOT CROSS-CHECKED. This looks like the 0x2C0 b2/b6 mirror
 * and is the opposite of it: there, b6 is a true redundant copy and a mismatch
 * means a bad sample; here a mismatch is EXPECTED at sector boundaries, so
 * discarding on mismatch would throw away one sample in two hundred for
 * nothing. The documented case is b0 = 7 with b1 = 146 (292 deg, half a degree
 * shy of the 292.5 deg NW boundary): the formula says 6/W, the car says 7/NW,
 * and the car wins.
 *
 * Heading is converted and the sector is not, because b1's 2 deg/count is not
 * a physical unit and b0 is already an enum. Resolution stays 2 deg -- do not
 * send tenths and invent precision the sender does not have.
 *
 * No deadband, and that is measured rather than assumed: across all 196
 * consecutive stationary (<= 2 km/h) sample pairs the 500 ms heading delta was
 * exactly 0.0 deg -- mean, median, max and stdev all zero. There is no jitter
 * to filter. Moving, the mean |delta| is 1.9 deg per 500 ms, so the slot is
 * claimed at most twice a second and only while actually turning.
 *
 * NOTE 0 IS A REAL HEADING -- due north -- not a blank. The sentinel is 0xFFFF
 * in the heading field and 0xFF in the sector byte.
 *
 * A CONSUMER TRAP THAT CANNOT BE FIXED HERE: 0x1B4 has no unavailable sentinel
 * and DOES NOT STOP AT KEY-OFF. In playback.log the frame keeps transmitting
 * at its normal 500 ms cadence with an unchanged valid heading straight
 * through the 0x000 ignition-off transition and for ~1000 further lines. So
 * this command's validity bit stays 1 across key-off, CORRECTLY -- the compass
 * really is still reporting. Key-off is 0xB2 bit 0, never the compass.
 * ==========================================================================*/
void customCompass(const volatile std::uint8_t* d) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint8_t  b0 = d[0];
    const std::uint8_t  b1 = d[1];

    /* Both bytes gated together, and the sample rejected WHOLE. Mask nothing:
     * an out-of-range b0 means the byte no longer means what this decoder
     * thinks, and truncating it would publish a confident wrong sector. */
    if ((b0 > kCompassSectorMax) || (b1 > kHeadingCountMax)) {
        setValid(ram::kValidBD, false);
        return;                       /* hold both previous values */
    }

    c.compassSector = b0;             /* already an enum -- verbatim */
    c.headingDeg    = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(b1) * kDegPerCount);
    setValid(ram::kValidBD, true);
}

/* ============================================================================
 * 0x1A7 b0:b1 -- fuel-flow RATE, for 0xBC FLOW. docs/CUSTOM-TX-SPEC.md s10.
 * Evidence: id 0x1A7, rate identity probable; the divisor 128 was fitted
 * against the PCM's own fuel-rate reading (OBD 212B) on two drives. It is not
 * verified at the pump -- which is why this stays a rate and the litres stay
 * the radio's job.
 *
 * Q = (b0 << 8) | b1, unsigned big-endian, and it is ALREADY a rate:
 * L/h = Q / 128, so deci-L/h = Q * 10 / 128. Three ways to get this wrong, all of
 * which the byte map warns about:
 *   - NEVER MULTIPLY BY RPM. Q is not per-revolution; that turns a rate into
 *     a number that tracks the tachometer, not the injectors.
 *   - NEVER PLAIN-SUM FRAMES. The frame cadence is ~100 ms running but ~400 ms
 *     at key-off, so a frame count is a clock that changes with engine state.
 *   - Q == 0 IS A REAL READING. Fuel cut on overrun injects nothing; it must
 *     be integrated as zero, not read as "no data". Only 0xFFFF is absent.
 *
 * THE AVERAGE. On each accepted frame, dt = now - lastFlowMs on the box's own
 * free-running millisecond counter. With no previous frame, or dt > 300 ms
 * (three missed frames: the stream stopped and restarted, and a Q held across
 * that hole would be invented), the interval is NOT integrated -- only the
 * stamp is refreshed. Otherwise sumQdt += Q*dt and sumDt += dt. Q is the NEW
 * frame's value applied backwards over the interval that ended at it, which is
 * what makes "600 ms at Q=1600, then 400 ms at Q=0" come out at exactly 6.0 L/h.
 *
 * A 0xFFFF frame is rejected AND clears haveLast, so the interval after it is
 * not counted either: its dt would otherwise be paired with a Q taken from
 * the far side of a hole. customFuelFlowTick() turns the sums into a value.
 *
 * Overflow: Q <= 0xFFFE and each counted dt <= 300 ms; a window collects at
 * most ~1.3 s of dt, so sumQdt <= ~8.6e7 and u32 is 50x clear of 4.29e9 (5x clear after the x10).
 *
 * No DLC check: the dispatcher hands over the data pointer only, like every
 * other custom decoder here. The id is unique to this signal on the WK body
 * bus.
 * ==========================================================================*/
void customFuelFlow(const volatile std::uint8_t* d) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;

    const std::uint32_t q = (static_cast<std::uint32_t>(d[0]) << 8)
                          |  static_cast<std::uint32_t>(d[1]);
    const std::uint32_t now = ram::app().sched.counter;

    if (q == ram::kCustomUnknown16) {
        c.flowHaveLast = 0U;          /* reject, and void the next interval */
        c.flowLastQ    = ram::kCustomUnknown16;
        return;
    }
    c.flowLastQ = static_cast<std::uint16_t>(q);

    if (c.flowHaveLast != 0U) {
        const std::uint32_t dt = now - c.flowLastMs;   /* wraps correctly */
        if (dt <= kFlowMaxGapMs) {
            c.flowSumQdt += q * dt;
            c.flowSumDt  += dt;
        }
    }
    c.flowLastMs   = now;
    c.flowHaveLast = 1U;
}

/* Engine load 0..100 %, derived from the injection quantity (see kLoadNum).
 * Unavailable (0xFF, which the decoder rejects as > 100) when rpm is absent or
 * below kLoadMinRpm, or Q is absent or stale. Called from customPublish(). */
void customEngineLoad() noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint32_t now = ram::app().sched.counter;
    const std::uint16_t rpm = c.rpm;
    const std::uint16_t q   = c.flowLastQ;

    if ((rpm == ram::kCustomUnknown16) || (rpm < kLoadMinRpm)
        || (q == ram::kCustomUnknown16) || (c.flowHaveLast == 0U)
        || ((now - c.flowLastMs) > kLoadMaxAgeMs)) {
        c.loadPct = ram::kCustomLoadUnknown;
        return;
    }

    const std::uint32_t den = kLoadDen * static_cast<std::uint32_t>(rpm);
    std::uint32_t pct = (kLoadNum * static_cast<std::uint32_t>(q) + (den / 2U)) / den;
    if (pct > kPercentMax) {
        pct = kPercentMax;
    }
    c.loadPct = static_cast<std::uint8_t>(pct);
}

/* The window close. Every 1000 ms: if any interval was integrated, publish
 * round-half-up(sumQdt * 10 / sumDt / 128) deci-L/h clamped to 0xFFFE (0xFFFF is the
 * sentinel and must not be reachable as a reading); otherwise 0xFFFF. Both sums
 * reset either way. Runs from the 8 ms task so a silent bus -- no 0x1A7 at all
 * -- still turns into 0xFFFF after a second instead of holding the last value
 * for ever. Integer half-up: floor((10*S + 64*T) / (128*T)). The 0.1 L/h
 * resolution is the deadband, as 0.1 V is for volts. */
void customFuelFlowTick() noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint32_t now = ram::app().sched.counter;

    if ((now - c.flowWindowMs) < kFlowWindowMs) {
        return;
    }
    c.flowWindowMs = now;

    if (c.flowSumDt > 0U) {
        /* 10 * sumQdt <= ~8.6e8, inside 2^32 */
        std::uint32_t v = (10U * c.flowSumQdt + 64U * c.flowSumDt)
                        / (128U * c.flowSumDt);
        if (v > kFlowMaxDeci) {
            v = kFlowMaxDeci;
        }
        c.flowDeciLph = static_cast<std::uint16_t>(v);
    } else {
        c.flowDeciLph = ram::kCustomUnknown16;
    }
    c.flowSumQdt = 0U;
    c.flowSumDt  = 0U;
}

/* ============================================================================
 * The TPMS poll. docs/TPMS-POLL-SPEC.md.
 *
 * THE ONLY FRAME THIS FIRMWARE SENDS UNPROMPTED. Everything else it transmits
 * is an answer to something -- the 0x516 echo, the 0x7FD handshake, the SWC
 * reply -- or a periodic frame the stock box also sends. That distinction is
 * what makes the bus-presence gate below mandatory rather than defensive.
 *
 * WHY canBusStatus MUST BE CHECKED. svc::can configures the controller with
 * abom = 0 (no automatic bus-off recovery) and nart = 0 (hardware retries a
 * frame for ever). On the car that is harmless, because some other node always
 * acknowledges. ON A BENCH WITH NO SECOND NODE, a transmitted frame is never
 * ACKed, the transmit error counter climbs, and the controller goes BUS-OFF
 * PERMANENTLY until reset. The stock firmware never met this because it only
 * ever transmits in reply to traffic that proves somebody is listening; a poll
 * has no such proof, so it has to ask for itself.
 *
 * Called from inside periodicTxScheduler()'s ACC hold gate, so it also stops
 * 3.2 s after key-off and is silent throughout phase 1 (FR-14) for free.
 * ==========================================================================*/
void customTpmsPoll() noexcept
{
    ram::AppRam&        r = ram::app();
    ram::CustomSignals& c = r.customSignals;

    /* ---- the publish rotation ------------------------------------------
     * One slot carries one corner at a time and steps every 5 s; five slots
     * for five corners would slow every other signal in the table down
     * permanently. The round-robin notices the changed payload and sends it,
     * so no explicit send request is needed. */
    c.tpmsRotateDiv = static_cast<std::uint16_t>(c.tpmsRotateDiv + 1U);
    if (c.tpmsRotateDiv >= kTpmsRotatePasses) {
        c.tpmsRotateDiv = 0U;
        c.tpmsPosition  = static_cast<std::uint8_t>(c.tpmsPosition + 1U);
        if (c.tpmsPosition >= ram::kTyreCount) {
            /* Wraps at 4, not 5: the spare is deferred, and publishing a
             * position the box never fills would show a permanently dead
             * fifth tyre. */
            c.tpmsPosition = ram::kTyrePosLeftFront;
        }
    }

    /* ---- the outstanding request ---------------------------------------- */
    if (c.tpmsPending != 0U) {
        if (c.tpmsDeadline != 0U) {
            c.tpmsDeadline = static_cast<std::uint8_t>(c.tpmsDeadline - 1U);
        }
        if (c.tpmsDeadline == 0U) {
            /* Timed out. No immediate retry: the next request goes out on
             * the normal 10 s tick, and the cycle moves on. */
            tpmsFail();
        }
        return;                       /* one request in flight at a time */
    }

    /* ---- time for the next poll? ---------------------------------------- */
    c.tpmsPollDiv = static_cast<std::uint16_t>(c.tpmsPollDiv + 1U);
    if (c.tpmsPollDiv < kTpmsPollPasses) {
        return;
    }
    c.tpmsPollDiv = 0U;

    /* The gate that keeps a bench alive. Not optional -- see the banner. */
    if (r.canBusStatus != cfg::kBusRunning) {
        return;
    }

    std::uint8_t lid = kTpmsLidPressures;
    if (c.tpmsStep == kTpmsStepSensor) {
        lid = static_cast<std::uint8_t>(kTpmsLidSensorBase + c.tpmsWheel);
    } else if (c.tpmsStep == kTpmsStepDiag) {
        lid = static_cast<std::uint8_t>(kTpmsLidDiagBase + c.tpmsWheel);
    }

    /* Next request in the cycle: A -> B -> C -> (next wheel) A ... */
    if (c.tpmsStep == kTpmsStepDiag) {
        c.tpmsStep  = kTpmsStepPressures;
        c.tpmsWheel = static_cast<std::uint8_t>(
            (c.tpmsWheel + 1U) % ram::kTyreCount);
    } else {
        c.tpmsStep = static_cast<std::uint8_t>(c.tpmsStep + 1U);
    }

    /* txEnqueue copies the bytes before returning, so a local is fine. */
    const std::uint8_t req[kTpmsRequestDlc] = { kTpmsRequestPci, kTpmsService, lid };
    svc::can::txEnqueue(canid::kWcmTpmsRequest, 0U, kTpmsRequestDlc, req);
    c.tpmsLid      = lid;
    c.tpmsPending  = 1U;
    c.tpmsDeadline = kTpmsReplyPasses;
}

/* ============================================================================
 * 0x500 -- the WCM's answer to the poll above.
 *
 *     06 61 3B p1 p2 p3 p4 [junk]
 *
 * p1..p4 are plain psi, 1 psi per count, no offset and no scaling. The order
 * is LF, RF, RR, LR -- the rears are NOT in the order most sources assume.
 *
 * TRUST THE PCI NIBBLE, NOT THE DLC. The byte after the declared length of 6
 * is whatever the responder had in its buffer last; it is neither data nor
 * zero, and reading it as a fifth pressure would publish garbage.
 *
 * In a jeep build this identifier belongs entirely to TPMS: the stock
 * 0x500 -> 0x516 mirror is switched off, see kVendorEcho500Enabled in
 * can_decode.cpp. Anything arriving here that is not a well-formed answer is
 * therefore DISCARDED, not echoed.
 * ==========================================================================*/
void customTpmsReply(const volatile std::uint8_t* d) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;

    if (c.tpmsPending == 0U) {
        return;                       /* nothing was asked for */
    }

    /* A negative response -- 7F 21 xx -- is a real answer, so it closes the
     * request, but it carries no data. */
    if (d[1] == kKwpNegative) {
        tpmsFail();
        return;
    }

    /* Match by the echoed local id; anything else is not our answer and the
     * request stays open. The PCI length must cover what we read. */
    const std::uint8_t lid = c.tpmsLid;
    if ((d[1] != kTpmsReplyService) || (d[2] != lid) || ((d[0] & 0xF0U) != 0U)) {
        return;
    }
    const std::uint8_t pdu = static_cast<std::uint8_t>(d[0] & 0x0FU);
    if (pdu > kTpmsPduMax) {
        return;
    }

    if (lid == kTpmsLidPressures) {
        if (pdu < kTpmsPressuresPdu) {
            return;
        }
        for (std::uint8_t i = 0U; i < ram::kTyreCount; ++i) {
            c.tyreKpa[i] = psiToKpa(d[kTpmsFirstPsiByte + i]);
        }
        c.tpmsPending  = 0U;
        c.tpmsFailures = 0U;
        setValid(ram::kValidB7, true);
        return;
    }

    if (lid < kTpmsLidDiagBase) {
        if (pdu < kTpmsSensorPdu) {
            return;
        }
        const std::uint8_t n = static_cast<std::uint8_t>(lid - kTpmsLidSensorBase);
        const std::uint8_t t = d[kTpmsSensorTempByte];
        const std::uint8_t st = d[kTpmsSensorStateByte];
        const std::uint8_t batt = static_cast<std::uint8_t>(
            (st >> kTpmsBattShift) & kTpmsTwoBits);
        const std::uint8_t trig = static_cast<std::uint8_t>(
            (st >> kTpmsTrigShift) & kTpmsTwoBits);
        std::uint8_t x = static_cast<std::uint8_t>(
            c.tyreExtra[n] & ram::kTyreFlagLowPressure);
        if (batt == kTpmsBattLow) {
            x = static_cast<std::uint8_t>(x | ram::kTyreFlagBatteryLow);
        }
        if (trig == kTpmsTrigPressureLoss) {
            x = static_cast<std::uint8_t>(x | ram::kTyreFlagLeak);
        }
        /* raw is degC + 40 already, same as the 0xB7 field. */
        c.tyreTemp[n]     = (t > kTpmsTempRawMax) ? ram::kCustomUnknown8 : t;
        c.tyreExtra[n]    = x;
        c.tyreMissTemp[n] = 0U;
        c.tpmsPending     = 0U;
        return;
    }

    if (pdu < kTpmsDiagPdu) {
        return;
    }
    const std::uint8_t n = static_cast<std::uint8_t>(lid - kTpmsLidDiagBase);
    std::uint8_t x = static_cast<std::uint8_t>(
        c.tyreExtra[n] & static_cast<std::uint8_t>(~ram::kTyreFlagLowPressure));
    if ((d[kTpmsDiagWarnByte] & kTpmsDiagWarnBit) != 0U) {
        x = static_cast<std::uint8_t>(x | ram::kTyreFlagLowPressure);
    }
    c.tyreExtra[n]    = x;
    c.tyreMissDiag[n] = 0U;
    c.tpmsPending     = 0U;
}

}  // namespace logic
