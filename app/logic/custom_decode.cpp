/* ============================================================================
 * custom_decode.cpp -- see custom_decode.hpp.
 *
 * Evidence for every identifier, byte, scale and sentinel below:
 * docs/JEEP-WK-CAN-ID-byte-map.md, quoted per function. The destination side
 * -- units, sentinels, cadences -- is docs/TELEMETRY-V2-SPEC.md.
 *
 * THE SHAPE OF EVERY DECODER HERE (layout v2). A decoder checks the DLC covers
 * what it reads, copies the raw bytes it owns into ram::CustomSignals and
 * STAMPS ITS SOURCE (lastMs + seen bit). It decides nothing about validity:
 * the 100 ms producer in custom_tx.cpp ages every source and writes a
 * sentinel for a dead one. A frame that fails a decoder's own gate (short,
 * out of range) is not stamped, so a source that only ever sends garbage goes
 * stale on its own.
 *
 * THIS TU IS COMPILED AND TYPE-CHECKED IN EVERY BUILD -- tools/build.py and
 * tools/test.py glob app/logic/, so a benz image compiles these decoders too.
 * The calls are then dropped by `if constexpr (!cfg::kIsBenz)` at the dispatch
 * site and the code itself by --gc-sections at link.
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
 * Source ages: 3 x the nominal period of each id (spec section 6, byte map
 * periods). Indexed by ram::kSrc*. 0x1A7 slows to ~400 ms at key-off, hence
 * 1.2 s rather than 300 ms.
 * ------------------------------------------------------------------------- */
constexpr std::uint16_t kSrcLimitMs[ram::kSrcCount] = {
    300U,    /* 000  100 ms   */
    150U,    /* 002   50 ms   */
    1400U,   /* 003 ~450 ms   */
    300U,    /* 006  100 ms   */
    3000U,   /* 012   ~1 s    */
    1500U,   /* 013  500 ms   */
    300U,    /* 014  100 ms   */
    300U,    /* 015  100 ms   */
    1500U,   /* 02C ~500 ms   */
    3500U,   /* 0D0 ~1.15 s   */
    3000U,   /* 159   ~1 s    */
    1200U,   /* 1A7  100 / 400 ms */
    4400U,   /* 1B2 ~1.45 s   */
    1500U,   /* 1B4  500 ms   */
    300U,    /* 2C0  100 ms   */
};

/* ---------------------------------------------------------------------------
 * The 0x000 b0 ignition ladder. Evidence: id 0x000, Confirmed.
 *     00 no key   01 key in at OFF   63 ACC   85 ON/run   A9 crank
 * ------------------------------------------------------------------------- */
constexpr std::uint8_t kIgnNoKey  = 0x00U;
constexpr std::uint8_t kIgnKeyOff = 0x01U;
constexpr std::uint8_t kIgnAcc    = 0x63U;
constexpr std::uint8_t kIgnRun    = 0x85U;
constexpr std::uint8_t kIgnCrank  = 0xA9U;

/* ---------------------------------------------------------------------------
 * The engine-running hysteresis band. Spec section 5 (unchanged from v1).
 * Neither rpm nor the ignition byte works alone: CRANK (0xA9) reaches 410 rpm
 * and RUN (0x85) also shows 0. Idle on this 3.0 CRD is 577-580, so 300/500 is
 * a clean gap. Slow to claim "running" (three consecutive 0x002 frames,
 * 150 ms), instant to give up.
 * ------------------------------------------------------------------------- */
constexpr std::uint16_t kRunSetRpm       = 500U;
constexpr std::uint16_t kRunClearRpm     = 300U;
constexpr std::uint8_t  kRunStreakNeeded = 3U;

/* Indicator hold, spec section 3.2.1. Longer than the longest flasher off
 * phase (60 flashes/min = 500 ms) plus one 100 ms 0x006 sample plus margin.
 * It works whether 006 b0 bits 0/1 PULSE with the flasher or HOLD while the
 * stalk is out: either way every frame with the bit set reloads the timer and
 * the state clears 700-800 ms after the last one. Unmeasured; one constant. */
constexpr std::uint32_t kIndicatorHoldMs = 700U;
constexpr std::uint8_t  kTurnLeftBit  = 0x01U;   /* 006 b0 bit 0 */
constexpr std::uint8_t  kTurnRightBit = 0x02U;   /* 006 b0 bit 1 */

/* 0x159 b4 is a hard validity gate: b4 == 1 <-> b5 == 0xFF is not a reading. */
constexpr std::uint8_t kFuelGateInvalid = 0x01U;
/* b5 is 0.5 L/count over a 77.6 L tank: % = b5 * 50 / 77.6 = b5 * 500 / 776,
 * rounded half up. 255 * 500 = 127 500, no overflow anywhere. */
constexpr std::uint32_t kFuelPctNum = 500U;
constexpr std::uint32_t kFuelPctDen = 776U;
constexpr std::uint8_t  kPercentMax = 100U;
constexpr std::uint16_t kFuelDeciPerCount = 5U;

/* 0x1B4: b0 sector 0..7, b1 heading 0..179 (x2 deg). Gated together. */
constexpr std::uint8_t  kCompassSectorMax = 7U;
constexpr std::uint8_t  kHeadingCountMax  = 179U;
constexpr std::uint16_t kDegPerCount      = 2U;

/* The longest sample-and-hold interval the flow integral accepts: the 0x1A7
 * staleness limit. */
constexpr std::uint32_t kFlowHoldMs = 1200U;

/* Minimum DLCs: the highest byte each decoder reads, plus one. A shorter frame
 * is rejected whole and NOT stamped. */
constexpr std::uint8_t kDlc002 = 4U;
constexpr std::uint8_t kDlc003 = 5U;
constexpr std::uint8_t kDlc006 = 2U;
constexpr std::uint8_t kDlc012 = 3U;
constexpr std::uint8_t kDlc013 = 3U;
constexpr std::uint8_t kDlc014 = 3U;
constexpr std::uint8_t kDlc015 = 2U;
constexpr std::uint8_t kDlc02C = 3U;
constexpr std::uint8_t kDlc0D0 = 1U;
constexpr std::uint8_t kDlc159 = 6U;
constexpr std::uint8_t kDlc1A7 = 2U;
constexpr std::uint8_t kDlc1B2 = 2U;
constexpr std::uint8_t kDlc1B4 = 2U;
constexpr std::uint8_t kDlc2C0 = 5U;

std::uint32_t nowMs() noexcept { return ram::app().sched.counter; }

void stamp(std::uint8_t src) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    c.lastMs[src] = nowMs();
    c.srcSeen = static_cast<std::uint16_t>(c.srcSeen | (1U << src));
}

std::uint16_t be16(const volatile std::uint8_t* d, std::size_t i) noexcept
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(d[i]) << 8) | d[i + 1U]);
}

/* Integrate the HELD Q from flowMarkMs up to now, then move the mark. Only
 * while the held Q is a reading and its frame is younger than kFlowHoldMs:
 * a sample is not held across a hole in the stream. */
void flowIntegrate(std::uint32_t now) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint32_t dt = now - c.flowMarkMs;
    c.flowMarkMs = now;

    const bool held = (c.flowQ != ram::kCustomUnknown16)
                   && ((c.srcSeen & (1U << ram::kSrc1A7)) != 0U)
                   && ((now - c.lastMs[ram::kSrc1A7]) <= kFlowHoldMs)
                   && (dt <= kFlowHoldMs);
    if (!held) {
        return;
    }
    c.flowSumQdt += static_cast<std::uint32_t>(c.flowQ) * dt;
    c.flowSumDt  += dt;
}

/* Reload one indicator timer. */
void indicatorHold(std::uint8_t bit, std::uint8_t idx, std::uint32_t now) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    c.indUntil[idx] = now + kIndicatorHoldMs;
    c.indActive = static_cast<std::uint8_t>(c.indActive | bit);
}

/* ---------------------------------------------------------------------------
 * TPMS constants and helpers (unchanged from feature/tpms-temp).
 * ------------------------------------------------------------------------- */
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
 *   byte3 air temperature: raw WCM byte, scale UNCONFIRMED. Sent verbatim in
 *         0xB7 byte 3; consumers treat it as degF provisionally
 *   byte5 bits0-1 frame number (ignored), bits2-3 battery (1 = low),
 *         bits4-5 transmission trigger (2 = Pressure Loss),
 *         bits6-7 sensor state (2 = park mode; ignored, temp still sent)
 * The box does no conversion; the head-unit apps own the scale. */
constexpr std::size_t  kTpmsSensorTempByte  = 4U;   /* frame offset of byte3 */
constexpr std::size_t  kTpmsSensorStateByte = 6U;   /* frame offset of byte5 */
constexpr std::uint8_t kTpmsBattShift        = 2U;
constexpr std::uint8_t kTpmsBattLow          = 1U;
constexpr std::uint8_t kTpmsTrigShift        = 4U;
constexpr std::uint8_t kTpmsTrigPressureLoss = 2U;
constexpr std::uint8_t kTpmsTwoBits          = 0x03U;

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
 * Prime every signal to its "no value" state and every source to dead.
 *
 * MUST run before the first transmit. .bss is zeroed at startup, and zero is a
 * plausible reading for most of these (0.0 km, due north, -40 degC). Sources
 * start UNSEEN, so the first packing writes sentinels whatever the raw bytes
 * hold; the raw bytes are primed anyway so nothing reads as a reading by
 * accident.
 * ==========================================================================*/
void customPrimeSignals() noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint32_t now = nowMs();

    for (std::uint8_t i = 0U; i < ram::kSrcCount; ++i) {
        c.lastMs[i] = now;
    }
    c.srcSeen  = 0U;
    c.srcAlive = 0U;

    c.ign            = kIgnNoKey;
    c.rpm            = ram::kCustomUnknown16;
    c.speedRaw       = ram::kCustomUnknown16;
    c.lamps0         = 0U;
    c.lamps1         = 0U;
    c.lever          = 0xC0U;            /* "selector unavailable" */
    c.brake          = 0U;
    c.prndl          = 0U;
    c.lights         = 0U;
    c.rapByte        = 0U;
    c.keyByte        = 0U;
    c.beltByte       = 0U;
    c.doorByte       = 0U;
    c.odometerDeciKm = ram::kCustomUnknown24;
    c.outsideDeciC   = ram::kCustomUnknownS16;
    c.volts          = ram::kCustomUnknown8;
    c.oil            = ram::kCustomUnknown8;
    c.coolant        = ram::kCustomUnknown8;
    c.pedal          = ram::kCustomUnknown8;
    c.parkByte       = 0U;
    c.minutesOff     = ram::kCustomUnknown8;
    c.fuelPct        = ram::kCustomUnknown8;
    c.fuelDecilitres = ram::kCustomUnknown16;
    c.transfer       = 0x07U;            /* "unavailable" */
    c.compassSector  = ram::kCustomUnknown8;
    c.headingDeg     = ram::kCustomUnknown16;
    c.boost          = ram::kCustomUnknown8;

    c.runStreak = 0U;
    c.engineRun = 0U;

    c.flowQ        = ram::kCustomUnknown16;
    c.flowMarkMs   = now;
    c.flowSumQdt   = 0U;
    c.flowSumDt    = 0U;
    c.flowCentiLph = ram::kCustomUnknown16;
    c.loadPct      = ram::kCustomUnknown8;

    for (std::uint8_t i = 0U; i < ram::kIndTimers; ++i) {
        c.indUntil[i] = now;
    }
    c.indActive = 0U;

    c.nextTickMs = now + 100U;
    c.tickCount  = 0U;
    c.stateAge   = 0U;
    for (std::uint8_t i = 0U; i < sizeof(c.stateLast); ++i) {
        c.stateLast[i] = 0U;
    }
    c.heartbeat = 0U;

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
}

/* ============================================================================
 * Source ages. True while `src` has been seen since boot and its last
 * accepted frame is no older than its limit. Wrap-safe u32 subtraction.
 * ==========================================================================*/
bool customSourceAlive(std::uint8_t src, std::uint32_t now) noexcept
{
    if (src >= ram::kSrcCount) {
        return false;
    }
    const ram::CustomSignals& c = ram::app().customSignals;
    if ((c.srcSeen & (1U << src)) == 0U) {
        return false;
    }
    return (now - c.lastMs[src]) <= kSrcLimitMs[src];
}

std::uint16_t customAgeSources(std::uint32_t now) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    std::uint16_t alive = 0U;

    for (std::uint8_t i = 0U; i < ram::kSrcCount; ++i) {
        if (customSourceAlive(i, now)) {
            alive = static_cast<std::uint16_t>(alive | (1U << i));
        }
    }
    c.srcAlive = alive;

    /* A dead 0x002 must not leave "running" latched: revival needs three
     * fresh frames again. */
    if ((alive & (1U << ram::kSrc002)) == 0U) {
        c.runStreak = 0U;
        c.engineRun = 0U;
    }
    return alive;
}

/* ============================================================================
 * 0x000 b0 -- ignition ladder. Evidence: id 0x000, Confirmed.
 *
 * Kept raw; the enum is mapped at packing. Cranking VETOES "running" at once:
 * a crank reaches 410 rpm. Second reader of a frame the stock decoder handles
 * (shared::decodeIgnition, kFlagAccOn); the two deliberately do not share.
 * ==========================================================================*/
void customIgnition(std::uint8_t b0) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;

    c.ign = b0;
    stamp(ram::kSrc000);

    if (b0 == kIgnCrank) {
        c.runStreak = 0U;
        c.engineRun = 0U;
    }
}

std::uint8_t customIgnitionEnum(std::uint8_t b0) noexcept
{
    switch (b0) {
    case kIgnNoKey:  return 0U;
    case kIgnKeyOff: return 1U;
    case kIgnAcc:    return 2U;
    case kIgnRun:    return 3U;
    case kIgnCrank:  return 4U;
    default:         return ram::kIgnEnumUnknown;
    }
}

/* ============================================================================
 * 0x002 -- b0:b1 rpm (1 rpm/count), b2:b3 speed (km/h x128). DLC 8, 50 ms.
 * Evidence: id 0x002, Confirmed. Each field has its own FFFF sentinel; the
 * packer never computes FFFF / 128.
 *
 * The engine-running state machine (spec section 5) runs here, per frame:
 * set after three consecutive frames >= 500 rpm and not cranking; clear at
 * once below 300 rpm, on FFFF, or cranking; hold in between.
 * ==========================================================================*/
void customEngine(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc002) {
        return;
    }
    ram::CustomSignals& c = ram::app().customSignals;

    const std::uint16_t rpm = be16(d, 0U);
    c.rpm      = rpm;
    c.speedRaw = be16(d, 2U);
    stamp(ram::kSrc002);

    const bool rpmValid = (rpm != ram::kCustomUnknown16);
    const bool cranking = (c.ign == kIgnCrank);

    if (!rpmValid || (rpm < kRunClearRpm) || cranking) {
        c.runStreak = 0U;
        c.engineRun = 0U;
    } else if (rpm >= kRunSetRpm) {
        if (c.runStreak < kRunStreakNeeded) {
            c.runStreak = static_cast<std::uint8_t>(c.runStreak + 1U);
        }
        if (c.runStreak >= kRunStreakNeeded) {
            c.engineRun = 1U;
        }
    } else {
        /* 300..499 rpm, not cranking: hold both the bit and the streak. */
    }
}

/* ============================================================================
 * 0x003 -- cluster telltales + gear selector. DLC 7, ~450 ms.
 * Evidence: id 0x003 (b0/b1/b4 = CCN KWP 213F bytes 2/3/7, matched
 * sample-for-sample). b0, b1 lamp bits; b2 lever (C0 = unavailable); b3 bit0
 * brake switch; b4 the PRNDL ASCII the cluster shows.
 *
 * A third reader of 0x003: stock jeepReverse() keeps the reverse flag from
 * b2 and the 0x32 gear from b4. No coupling.
 * ==========================================================================*/
void customCluster(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc003) {
        return;
    }
    ram::CustomSignals& c = ram::app().customSignals;

    c.lamps0 = d[0];
    c.lamps1 = d[1];
    c.lever  = d[2];
    c.brake  = d[3];
    c.prndl  = d[4];
    stamp(ram::kSrc003);
}

std::uint8_t customGearEnum(std::uint8_t lever, std::uint8_t ascii) noexcept
{
    if (lever == 0xC0U) {
        return ram::kCustomUnknown8;    /* selector unavailable */
    }
    switch (ascii) {
    case 0x50U: return 0U;   /* P */
    case 0x52U: return 1U;   /* R */
    case 0x4EU: return 2U;   /* N */
    case 0x44U: return 3U;   /* D */
    case 0x31U: return 4U;   /* AutoStick 1 */
    case 0x32U: return 5U;   /* AutoStick 2 */
    case 0x33U: return 6U;   /* AutoStick 3 */
    case 0x34U: return 7U;   /* AutoStick 4 */
    default:    return ram::kCustomUnknown8;
    }
}

/* ============================================================================
 * 0x006 -- b0 lights and indicators, b1 RAP. DLC 7, 100 ms.
 * Evidence: id 0x006, all bits Confirmed (owner high-beam and fog runs).
 *
 * Bits 3-7 are kept raw. Bits 0/1 feed the debounce (spec section 3.2.1): a
 * frame with bit 0 (1) set reloads the left (right) 700 ms timer, both bits
 * in ONE frame (0x1B pattern) reload the hazard timer as well.
 *
 * LOCK-CONFIRM FLASH. Locking the car flashes 006 b0 = 03 for ~0.2 s, which
 * the hold would stretch into a 0.7 s "hazard". While 0x000 is alive and
 * reads 00 (no key) or 01 (key in at OFF) the indicator bits are ignored.
 * The cost, accepted: hazards switched on with the key out are not shown.
 * (Owner note from the withdrawn 0xBA LIGHTING draft, 2026-11-07.)
 * ==========================================================================*/
void customLights(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc006) {
        return;
    }
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint32_t now = nowMs();
    const std::uint8_t  b0  = d[0];

    c.lights  = b0;
    c.rapByte = d[1];
    stamp(ram::kSrc006);

    const bool keyOut = customSourceAlive(ram::kSrc000, now)
                     && ((c.ign == kIgnNoKey) || (c.ign == kIgnKeyOff));
    if (keyOut) {
        return;
    }
    if ((b0 & kTurnLeftBit) != 0U) {
        indicatorHold(ram::kIndLeft, 0U, now);
    }
    if ((b0 & kTurnRightBit) != 0U) {
        indicatorHold(ram::kIndRight, 1U, now);
    }
    if ((b0 & (kTurnLeftBit | kTurnRightBit)) == (kTurnLeftBit | kTurnRightBit)) {
        indicatorHold(ram::kIndHazard, 2U, now);
    }
}

/* Expire the indicator timers. Called from the 100 ms tick. */
void customIndicatorTick(std::uint32_t now) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;
    std::uint8_t active = c.indActive;

    for (std::uint8_t i = 0U; i < ram::kIndTimers; ++i) {
        const std::uint8_t bit = static_cast<std::uint8_t>(1U << i);
        if (((active & bit) != 0U)
            && (static_cast<std::int32_t>(now - c.indUntil[i]) >= 0)) {
            active = static_cast<std::uint8_t>(active & static_cast<std::uint8_t>(~bit));
        }
    }
    c.indActive = active;
}

/* ============================================================================
 * 0x012 b2 bit 1 -- valid SKIM key in the ignition. DLC 6, ~1 s.
 * Evidence: id 0x012, Confirmed (WCM KWP 2118 live, flips at key pull).
 * ==========================================================================*/
void customKey(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc012) {
        return;
    }
    ram::app().customSignals.keyByte = d[2];
    stamp(ram::kSrc012);
}

/* ============================================================================
 * 0x013 -- b0 bit 3 driver belt UNfastened, b2 bits 0-4 FL FR RR RL hatch.
 * DLC 5, 500 ms. Evidence: id 0x013, Confirmed. Second reader of the frame
 * stock decodeDoors() uses for 0x12.
 * ==========================================================================*/
void customBody(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc013) {
        return;
    }
    ram::CustomSignals& c = ram::app().customSignals;
    c.beltByte = d[0];
    c.doorByte = d[2];
    stamp(ram::kSrc013);
}

/* ============================================================================
 * 0x014 -- b0:b2 odometer (0.1 km, FFFFFF n/a), b4:b5 outside air
 * (u16 BE, raw x 0.01 - 40 degC, FFFF n/a). DLC 7, 100 ms.
 * Evidence: id 0x014, odometer Confirmed, outside air Probable/high.
 *
 * Odometer: verbatim, the car's FFFFFF is the wire sentinel. Outside air:
 * FFFF or a frame without b5 KEEPS the previous value (the cluster does the
 * same at wake); before the first usable read it stays 0x8000.
 * ==========================================================================*/
void customTrip(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc014) {
        return;
    }
    ram::CustomSignals& c = ram::app().customSignals;

    c.odometerDeciKm = (static_cast<std::uint32_t>(d[0]) << 16)
                     | (static_cast<std::uint32_t>(d[1]) << 8)
                     |  static_cast<std::uint32_t>(d[2]);

    const std::uint16_t raw = ambientRaw(d);
    if (ambientUsable(raw, dlc)) {
        c.outsideDeciC = ambientDeciC(raw);
    }
    stamp(ram::kSrc014);
}

/* ============================================================================
 * 0x015 b1 -- battery volts at 0.1 V/count. DLC 6, 100 ms. Confirmed.
 * Verbatim; 0xFF would be 25.5 V, above any real reading, and is the sentinel.
 * b2 is NOT intake air (byte map: unknown temperature); v2 has no intake field.
 * ==========================================================================*/
void customVolts(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc015) {
        return;
    }
    ram::app().customSignals.volts = d[1];
    stamp(ram::kSrc015);
}

/* ============================================================================
 * 0x02C -- b0 oil pressure (~4 kPa/count gauge, 00 engine off), b1 coolant
 * (degC + 40), b2 accelerator pedal (0.4 %/count, 250 = 100 %). DLC 6,
 * ~500 ms. Evidence: id 0x02C, all Confirmed (oil scale probable; pedal
 * r = 0.9995 vs the TCM). Oil and coolant verbatim: the car's all-FF
 * "unavailable" is the wire's 0xFF. The pedal is scaled at packing.
 * ==========================================================================*/
void customEngineBay(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc02C) {
        return;
    }
    ram::CustomSignals& c = ram::app().customSignals;
    c.oil     = d[0];
    c.coolant = d[1];
    c.pedal   = d[2];
    stamp(ram::kSrc02C);
}

/* ============================================================================
 * 0x0D0 b0 bit 7 -- parking brake engaged. DLC 2, ~1.15 s. Confirmed.
 * MASK BIT 7 ONLY at packing: bit 2 (0x04) is "ignition RUN", the released
 * baseline while driving.
 * ==========================================================================*/
void customParkBrake(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc0D0) {
        return;
    }
    ram::app().customSignals.parkByte = d[0];
    stamp(ram::kSrc0D0);
}

/* ============================================================================
 * 0x159 -- b0 minutes since RUN (FF transient = n/a), b4 gate, b5 fuel level
 * at 0.5 L/count. DLC 6, ~1 s. Evidence: id 0x159, layout Confirmed, litre
 * scale Probable.
 *
 * b4 == 01 means b5 is not a reading: the previous % and litres are HELD (the
 * gate asserts at key-off and a parked tank does not change). b5 == FF with an
 * open gate is treated the same way. Before the first reading the primed
 * sentinels stand; when 0x159 itself goes stale the packer sends sentinels.
 * ==========================================================================*/
void customFuel(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc159) {
        return;
    }
    ram::CustomSignals& c = ram::app().customSignals;

    c.minutesOff = d[0];
    stamp(ram::kSrc159);

    const std::uint8_t b5 = d[5];
    if ((d[4] == kFuelGateInvalid) || (b5 == ram::kCustomUnknown8)) {
        return;                                   /* hold */
    }

    std::uint32_t pct = (static_cast<std::uint32_t>(b5) * kFuelPctNum
                         + (kFuelPctDen / 2U)) / kFuelPctDen;
    if (pct > kPercentMax) {
        pct = kPercentMax;
    }
    c.fuelPct        = static_cast<std::uint8_t>(pct);
    c.fuelDecilitres = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(b5) * kFuelDeciPerCount);
}

/* ============================================================================
 * 0x1A7 b0:b1 -- fuel flow Q, L/h = Q / 128; FFFF n/a, 0 = overrun cut.
 * DLC 6, 100 ms running, ~400 ms key-off. Evidence: id 0x1A7, Confirmed
 * against the PCM fuel rate (divisor 128, r 0.999).
 *
 * SAMPLE AND HOLD. Each frame first integrates the PREVIOUS Q up to now, then
 * becomes the held value. The 100 ms tick (customFlowClose) integrates the
 * held value up to the tick and turns the window into a time-weighted mean.
 * NEVER multiply by rpm, NEVER sum frames: the frame rate changes with engine
 * state.
 * ==========================================================================*/
void customFuelFlow(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc1A7) {
        return;
    }
    ram::CustomSignals& c = ram::app().customSignals;
    const std::uint32_t now = nowMs();

    flowIntegrate(now);
    c.flowQ = be16(d, 0U);                        /* FFFF = holds nothing */
    stamp(ram::kSrc1A7);
}

/* The window close, every 100 ms tick:
 *   flow (0.01 L/h) = round(Qavg * 100 / 128) = round(25 S / (32 T))
 *   Qavg            = round(S / T)                    (for the load)
 * with S = sum(Q dt), T = sum(dt). No counted time: both FFFF. 25 S stays in
 * u32 while S <= 1e8, which a 100 ms window cannot exceed (65534 x 1200 ms
 * per interval); a stalled loop is handled by halving both sums. */
std::uint16_t customFlowClose(std::uint32_t now, std::uint16_t* qAvg) noexcept
{
    ram::CustomSignals& c = ram::app().customSignals;

    flowIntegrate(now);

    std::uint32_t s = c.flowSumQdt;
    std::uint32_t t = c.flowSumDt;
    c.flowSumQdt = 0U;
    c.flowSumDt  = 0U;

    while (s > 100000000U) {
        s >>= 1U;
        t >>= 1U;
    }
    if (t == 0U) {
        *qAvg = ram::kCustomUnknown16;
        return ram::kCustomUnknown16;
    }
    *qAvg = static_cast<std::uint16_t>((s + (t / 2U)) / t);
    std::uint32_t v = ((25U * s) + (16U * t)) / (32U * t);
    if (v > 0xFFFEU) {
        v = 0xFFFEU;     /* cannot bind (Q <= FFFE gives 51200); kept so the
                          * sentinel is never reachable as a reading */
    }
    return static_cast<std::uint16_t>(v);
}

/* Engine load % (byte map "Derived values"):
 *   inj = 1e6 Q / (128 x 180 x rpm) mm3/stroke, load = 100 inj / 90
 *       = 15625 Q / (324 rpm), rounded half up, clamped to 100.
 * (43.4 in the byte map is 1e6 / 23040 rounded.) 15625 x 65534 = 1.02e9,
 * inside u32. 0xFF when rpm is n/a or below 300 (cranking, stalled) or Q is
 * n/a. */
std::uint8_t customLoad(std::uint16_t rpm, std::uint16_t qAvg) noexcept
{
    if ((rpm == ram::kCustomUnknown16) || (rpm < kRunClearRpm)
        || (qAvg == ram::kCustomUnknown16)) {
        return ram::kCustomUnknown8;
    }
    const std::uint32_t den = 324U * static_cast<std::uint32_t>(rpm);
    std::uint32_t pct = ((15625U * static_cast<std::uint32_t>(qAvg)) + (den / 2U)) / den;
    if (pct > kPercentMax) {
        pct = kPercentMax;
    }
    return static_cast<std::uint8_t>(pct);
}

/* ============================================================================
 * 0x1B2 b1 -- transfer-case range: 02 4H, 01 4L, 00 shifting, 07 n/a.
 * DLC 6, ~1.45 s. Evidence: id 0x1B2, Confirmed (owner 4LO run).
 * ==========================================================================*/
void customTransfer(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc1B2) {
        return;
    }
    ram::app().customSignals.transfer = d[1];
    stamp(ram::kSrc1B2);
}

std::uint8_t customTransferEnum(std::uint8_t b1) noexcept
{
    switch (b1) {
    case 0x02U: return 1U;   /* 4H */
    case 0x01U: return 2U;   /* 4L */
    case 0x00U: return 3U;   /* shifting */
    default:    return ram::kTransferUnknown;
    }
}

/* ============================================================================
 * 0x1B4 -- b0 8-way sector (0 N .. 7 NW), b1 heading at 2 deg/count.
 * DLC 6, 500 ms. Confirmed. Both bytes gated TOGETHER: an out-of-range
 * sample is dropped whole and NOT stamped, so the previous values are held
 * until the source goes stale (1.5 s). Sector and heading are not
 * cross-checked: they legitimately disagree at sector boundaries and the
 * car's sector wins. 0x1B4 does not stop at key-off; it is not liveness.
 * ==========================================================================*/
void customCompass(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc1B4) {
        return;
    }
    const std::uint8_t b0 = d[0];
    const std::uint8_t b1 = d[1];
    if ((b0 > kCompassSectorMax) || (b1 > kHeadingCountMax)) {
        return;
    }
    ram::CustomSignals& c = ram::app().customSignals;
    c.compassSector = b0;
    c.headingDeg    = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(b1) * kDegPerCount);
    stamp(ram::kSrc1B4);
}

/* ============================================================================
 * 0x2C0 b4 -- boost / manifold absolute pressure, 8 hPa/count; FE saturated
 * (~2030 hPa), FF n/a. DLC 8, 100 ms. Confirmed (r 0.999 vs PCM 2103, which
 * fits 8.00 x b4 + 4 hPa; the +4 is left out, spec open question 6).
 * ==========================================================================*/
void customBoost(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept
{
    if (dlc < kDlc2C0) {
        return;
    }
    ram::app().customSignals.boost = d[4];
    stamp(ram::kSrc2C0);
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
        /* Raw WCM byte, passed through unchanged (0xFF stays "none"). */
        c.tyreTemp[n]     = t;
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
