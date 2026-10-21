/* ============================================================================
 * custom_decode.hpp -- the CAN receive decoders for the custom 0xB0-0xBD
 * telemetry block. Namespace `logic`.
 *
 * NOT stock decodes, and the mirror image of raise_benz_decode.hpp: these
 * exist because the Jeep WK puts coolant, intake air, outside air, battery
 * volts, throttle, fuel, road speed, the parking brake, the odometer and a
 * compass on CAN-B and the STOCK Hiworld protocol has nowhere to put any of
 * them. A jeep build adds eight head-unit commands of its own instead --
 * docs/CUSTOM-TX-SPEC.md.
 *
 * ---------------------------------------------------------------------------
 * WHERE THIS SITS
 *
 * Jeep WK CAN frames in, ram::CustomSignals out. Its sibling custom_tx.hpp
 * packs those signals into the 0xB0-0xBD payloads; nothing here touches a wire
 * format or an endianness. can_decode.cpp owns the RX ring and the dispatch
 * switch and calls into here, every call site behind
 * `if constexpr (!cfg::kIsBenz)`.
 *
 * ---------------------------------------------------------------------------
 * WHY THESE WRITE ram::CustomSignals AND NOT ram::VehicleState
 *
 * VehicleState's byte offsets ARE the stock Hiworld wire format: four
 * overlapping message windows over one run of bytes, documented as protocol in
 * ram_map.hpp. Nothing that is not a stock field can go in it. Several of the
 * frames below are already decoded for a stock field too -- 0x002 for rpm,
 * 0x003 for the reverse flag and PRNDL, 0x015 for the steering angle -- so
 * these are SECOND READERS of the same frame, not replacements. Neither side
 * may assume the other ran. The same is true WITHIN this header: customAmbient()
 * and customOdometer() both read 0x014 and are deliberately two functions.
 *
 * ---------------------------------------------------------------------------
 * THE SENTINEL RULE
 *
 * The WK marks "unavailable" with all-ones on several of these ids, and a
 * couple carry an explicit validity byte. Every decoder below therefore does
 * two things on a bad sample: write the field's "no value" encoding (see the
 * sentinel block in ram_map.hpp) and clear that command's bit in the 0xB0
 * validity mask. It never writes a value derived FROM a sentinel -- that is
 * how `0xFF - 40 = 215 degC` reaches a gauge.
 *
 * Evidence for every identifier, byte and scale is in
 * docs/JEEP-WK-CAN-ID-byte-map.md, quoted per function in the .cpp.
 * ==========================================================================*/

#ifndef CUSTOM_DECODE_HPP
#define CUSTOM_DECODE_HPP

#include <cstdint>

namespace logic {

/* Put every signal in its "no value" state. Called once from the boot
 * sequence, BEFORE the first transmit: a zeroed CustomSignals would otherwise
 * publish 0 degC-40 coolant and 0.0 V as though they were readings. */
void customPrimeSignals() noexcept;

/* 0x000 b0 -- ignition state: ACC bit and the crank latch the engine-running
 * state machine needs. */
void customIgnition(std::uint8_t b0) noexcept;

/* 0x002 b0:b1 rpm, b2:b3 road speed, and the engine-running state machine.
 * All-FF in b0:b1 means both are unavailable. */
void customEngine(const volatile std::uint8_t* d) noexcept;

/* 0x003 b4 -- the ASCII PRNDL character; 'R' is the reverse bit. */
void customGear(const volatile std::uint8_t* d) noexcept;

/* 0x014 b4 -- outside air temperature, whole degrees C. */
void customAmbient(const volatile std::uint8_t* d) noexcept;

/* 0x014 b0:b1:b2 -- odometer, 24-bit, 0.1 km/count.
 *
 * SECOND READER OF THE SAME FRAME as customAmbient(), and deliberately a
 * separate function rather than two fields of one. The two have DIFFERENT
 * GATES and DIFFERENT VALIDITY BITS -- b4 can be suspect while the odometer is
 * live, and the odometer can read unavailable while ambient is fine. One
 * function, one validity bit is the rule that keeps this block's ownership
 * clean; merging them would put two owners on one gate. */
void customOdometer(const volatile std::uint8_t* d) noexcept;

/* 0x1B4 b0 compass sector, b1 magnetic heading at 2 deg/count. */
void customCompass(const volatile std::uint8_t* d) noexcept;

/* ---------------------------------------------------------------------------
 * TPMS -- the one exchange this box INITIATES. docs/TPMS-POLL-SPEC.md.
 *
 * Everything else in this header is a passive reader of a frame the car was
 * going to send anyway. These two are a request/response pair: customTpmsPoll()
 * transmits a KWP readDataByLocalIdentifier to the WCM every 30 s and
 * customTpmsReply() consumes the answer.
 * ------------------------------------------------------------------------- */

/* Run the poll state machine. Called from the 8 ms periodic-TX task, INSIDE
 * the ACC hold gate, so polling stops 3.2 s after key-off for free. */
void customTpmsPoll() noexcept;

/* 0x500 -- the WCM's answer. Also runs the staleness bookkeeping. */
void customTpmsReply(const volatile std::uint8_t* d) noexcept;

/* 0x015 b1 battery volts (0.1 V/count), b2 intake air temperature. */
void customVoltsIntake(const volatile std::uint8_t* d) noexcept;

/* 0x02C b1 -- coolant temperature, already degC+40 on the wire. All-FF means
 * unavailable, which is the one sentinel in this block that would otherwise
 * decode as a plausible number. */
void customCoolant(const volatile std::uint8_t* d) noexcept;

/* 0x0D0 b0 bit7 -- parking brake engaged. */
void customParkBrake(const volatile std::uint8_t* d) noexcept;

/* 0x159 b4 validity gate, b5 fuel level at 0.5 L/count. */
void customFuel(const volatile std::uint8_t* d) noexcept;

/* 0x2C0 b2 -- accelerator pedal, with b6 as a redundancy check. */
void customThrottle(const volatile std::uint8_t* d) noexcept;

}  // namespace logic

#endif /* CUSTOM_DECODE_HPP */
