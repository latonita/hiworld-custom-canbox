/* ============================================================================
 * custom_decode.hpp -- the CAN receive decoders for the custom telemetry
 * block, layout v2 (0xB0 0xB2 0xB3 0xB4 0xB5 0xB7 0xB8). Namespace `logic`.
 *
 * NOT stock decodes, and the mirror image of raise_benz_decode.hpp: the Jeep
 * WK puts far more on CAN-B than the stock Hiworld protocol has room for, so a
 * jeep build adds head-unit commands of its own -- docs/TELEMETRY-V2-SPEC.md.
 *
 * ---------------------------------------------------------------------------
 * WHERE THIS SITS
 *
 * Jeep WK CAN frames in, ram::CustomSignals out: raw bytes plus one timestamp
 * per CAN source. custom_tx.cpp ages the sources every 100 ms and packs the
 * payloads, writing a sentinel for every field whose source is dead. Nothing
 * here touches a wire format. can_decode.cpp owns the RX ring and the
 * dispatch switch and calls into here, every call site behind
 * `if constexpr (!cfg::kIsBenz)`.
 *
 * These are SECOND READERS of frames the stock decoder also handles (0x000,
 * 0x002, 0x003, 0x006, 0x013, 0x015): neither side may assume the other ran.
 *
 * Every decoder takes the DLC and rejects (and does not stamp) a frame too
 * short for the bytes it reads.
 *
 * Evidence for every identifier, byte and scale is in
 * docs/JEEP-WK-CAN-ID-byte-map.md, quoted per function in the .cpp.
 * ==========================================================================*/

#ifndef CUSTOM_DECODE_HPP
#define CUSTOM_DECODE_HPP

#include <cstdint>

namespace logic {

/* Put every signal in its "no value" state and every source to dead. Called
 * once from the boot sequence, BEFORE the first transmit. */
void customPrimeSignals() noexcept;

/* ---- the decoders, one per CAN id ---------------------------------------- */
void customIgnition(std::uint8_t b0) noexcept;                               /* 0x000 */
void customEngine(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept; /* 0x002 */
void customCluster(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept;/* 0x003 */
void customLights(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept; /* 0x006 */
void customKey(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept;    /* 0x012 */
void customBody(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept;   /* 0x013 */
void customTrip(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept;   /* 0x014 */
void customVolts(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept;  /* 0x015 */
void customEngineBay(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept; /* 0x02C */
void customParkBrake(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept; /* 0x0D0 */
void customFuel(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept;   /* 0x159 */
void customFuelFlow(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept; /* 0x1A7 */
void customTransfer(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept; /* 0x1B2 */
void customCompass(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept; /* 0x1B4 */
void customBoost(const volatile std::uint8_t* d, std::uint8_t dlc) noexcept;  /* 0x2C0 */

/* ---- helpers for the 100 ms producer (custom_tx.cpp) --------------------- */

/* One source alive at `now` (seen since boot and within 3x its period). */
bool customSourceAlive(std::uint8_t src, std::uint32_t now) noexcept;
/* Refresh ram::CustomSignals::srcAlive for every source and return it. */
std::uint16_t customAgeSources(std::uint32_t now) noexcept;
/* Expire the 700 ms indicator hold timers. */
void customIndicatorTick(std::uint32_t now) noexcept;
/* Close the 0x1A7 window: flow in 0.01 L/h (0xFFFF n/a), Qavg out. */
std::uint16_t customFlowClose(std::uint32_t now, std::uint16_t* qAvg) noexcept;
/* Engine load % from rpm and the window's Qavg; 0xFF n/a. */
std::uint8_t customLoad(std::uint16_t rpm, std::uint16_t qAvg) noexcept;

/* Enum mappings (spec section 3.2). */
std::uint8_t customIgnitionEnum(std::uint8_t b0) noexcept;   /* 0..4, 7 */
std::uint8_t customGearEnum(std::uint8_t lever, std::uint8_t ascii) noexcept; /* 0..7, FF */
std::uint8_t customTransferEnum(std::uint8_t b1) noexcept;   /* 0..3 */

/* ---------------------------------------------------------------------------
 * TPMS -- the one exchange this box INITIATES. docs/TPMS-POLL-SPEC.md.
 * ------------------------------------------------------------------------- */

/* Run the poll state machine. Called from the 8 ms periodic-TX task, INSIDE
 * the ACC hold gate, so polling stops 3.2 s after key-off for free. */
void customTpmsPoll() noexcept;

/* 0x500 -- the WCM's answer. Also runs the staleness bookkeeping. */
void customTpmsReply(const volatile std::uint8_t* d) noexcept;

}  // namespace logic

#endif /* CUSTOM_DECODE_HPP */
