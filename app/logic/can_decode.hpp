/* ============================================================================
 * can_decode.hpp -- the CAN receive-frame decoders (the car profiles).
 * Namespace `logic`. See docs/ARCHITECTURE.md section 4.
 *
 * These three functions are the behavioural heart of the box: they drain the
 * CAN RX ring and turn incoming vehicle frames into the ram::VehicleState block
 * the head-unit link reports over UART (ignition, doors, lamps, steering angle,
 * SWC keys, gear, RPM...).
 *
 * Reconstructed from reference/decompile/app/can_rx_decode.c (Ghidra output) via
 * the retired app/port/can_decode.c. Addresses in the comments are the ORIGINAL flash
 * addresses of the stock functions; they are how a future reader diffs this
 * against Ghidra. Keep them.
 *
 * Only dispatchGate() is called from outside (the 8 ms main-loop slot); the two
 * profile decoders are exported so the dispatch can be unit-tested and so the
 * flash-address mapping stays visible.
 * ==========================================================================*/

#ifndef CAN_DECODE_HPP
#define CAN_DECODE_HPP

#include <cstdint>

namespace logic {

/* ===========================================================================
 * The CAN identifiers, named.
 *
 * The two profiles carry the SAME signals under DIFFERENT identifiers, which
 * is exactly the kind of thing a bare hex literal in a switch hides:
 *
 *     signal                 Jeep WK 83.333k      FCA 125k
 *     ---------------------  -------------------  ------------------
 *     ignition / ACC         0x000                0x20B
 *     doors                  0x013 d[2]           0x244 d[0]
 *     steering angle         0x015 d[4]:d[5]      0x1E1 d[2]:d[3]
 *     illumination + level   0x210 d[1]           0x308 d[1]
 *     SWC buttons            0x3A0                0x3A3
 *     vendor echo -> 0x516   0x500 d[0..1]        0x513 d[0..3]
 *     reverse gear           0x003 d[2] nibble    0x20E d[2] bit 3
 *     PRNDL gear             0x003 d[4] ASCII *   0x20E d[2] nibble
 *     engine speed           0x002 d[0]:d[1] *    0x211 d[0]:d[1]
 *
 * Signals the custom telemetry block adds on top, jeep profile only and with
 * no FCA counterpart decoded, because the FCA bus is not this car:
 *
 *     road speed             0x002 d[2]:d[3]      --
 *     outside air            0x014 d[4]           --
 *     battery volts          0x015 d[1]           --
 *     intake air             0x015 d[2]           --
 *     coolant                0x02C d[1]           --
 *     parking brake          0x0D0 d[0] bit 7     --
 *     fuel level             0x159 d[4]:d[5]      --
 *     throttle pedal         0x2C0 d[2] (d[6])    --
 *     odometer               0x014 d[0]:d[1]:d[2] --
 *     compass + heading      0x1B4 d[0], d[1]     --
 *
 * (*) The WK carries both of these and this rebuild decodes both -- but the
 *     STOCK profile-0 decoder ignores them, so they are deviations rather
 *     than a port: section 6.1 B and FR-37a / FR-37b in
 *     docs/REQUIREMENTS.md. Unlike the Benz-only ids below they are decoded
 *     in EVERY build, because the Hiworld protocol has fields for them --
 *     UART 0x32 payload [1] (gear) and [2..3] (rpm), which a stock box
 *     leaves permanently zero.
 *
 * Everything in the `kSwcService*` / `kVendor*` / `kTx*` group below is
 * identical on both profiles.
 * ========================================================================= */
namespace canid {

/* ---- profile 0: Jeep WK family, 83.333 kbit/s -- THIS box --------------- */
inline constexpr std::uint32_t kJeepIgnition    = 0x000U;  /* ACC / run        */
/* NOT a stock decode. 0x002 carries engine RPM on this car and the stock
 * profile-0 decoder ignores it entirely; see docs/REQUIREMENTS.md section
 * 6.1 B / FR-37a. */
inline constexpr std::uint32_t kJeepEngine      = 0x002U;  /* rpm, speed       */
inline constexpr std::uint32_t kJeepReverse     = 0x003U;  /* reverse + PRNDL  */
/* NOT stock decodes either, and decoded ONLY in a Benz build: these carry
 * signals the Hiworld protocol has nowhere to display, so a jeep image would
 * do the work and throw it away. The decoders themselves live in
 * raise_benz_decode.hpp, not here; the dispatch below calls them behind
 * `if constexpr (cfg::kIsBenz)`. Evidence for every byte is in
 * docs/JEEP-WK-CAN-ID-byte-map.md. */
inline constexpr std::uint32_t kJeepTrip        = 0x014U;  /* odo + ambient    */
inline constexpr std::uint32_t kJeepCoolant     = 0x02CU;  /* coolant temp     */
/* ...and three more that are decoded ONLY in a jeep build, for the custom
 * 0xB0-0xB8 telemetry block (docs/CUSTOM-TX-SPEC.md). Same argument in the
 * other direction: they carry signals the BENZ protocol has nowhere to show.
 * The decoders live in custom_decode.hpp; the dispatch below calls them behind
 * `if constexpr (!cfg::kIsBenz)`. None of these is a stock case -- in the
 * stock image they all fall into the 0x400..0x43F service handler, which
 * returns immediately because (id & 0xFC0) != 0x400, so intercepting them
 * costs no stock behaviour. */
inline constexpr std::uint32_t kJeepFuel        = 0x159U;  /* level + gate     */
inline constexpr std::uint32_t kJeepParkBrake   = 0x0D0U;
inline constexpr std::uint32_t kJeepThrottle    = 0x2C0U;  /* pedal + mirror   */
inline constexpr std::uint32_t kJeepCompass     = 0x1B4U;  /* sector + heading */
/* Instantaneous fuel-flow RATE, b0:b1 big-endian, L/h = Q/160, ~100 ms while
 * the engine runs. Feeds 0xBC FLOW (spec section 10). Same no-op argument as
 * the others: 0x1A7 is below 0x400 and falls into decodeSwcService() in the
 * stock image, which returns at once. */
inline constexpr std::uint32_t kJeepFuelFlow    = 0x1A7U;
/* THE ONE FRAME THIS BOX SENDS UNPROMPTED. A KWP readDataByLocalIdentifier to
 * the WCM, whose reply lands on 0x500 -- i.e. on kJeepVendorEcho below, whose
 * name is now misleading in a jeep build and is kept because the Ghidra diff
 * depends on the stock names. docs/TPMS-POLL-SPEC.md. */
inline constexpr std::uint32_t kWcmTpmsRequest  = 0x600U;  /* -> reply on 0x500 */
inline constexpr std::uint32_t kJeepDoors       = 0x013U;
inline constexpr std::uint32_t kJeepSteering    = 0x015U;
inline constexpr std::uint32_t kJeepIllum       = 0x210U;  /* + dimmer percent */
inline constexpr std::uint32_t kJeepSwcButtons  = 0x3A0U;
inline constexpr std::uint32_t kJeepVendorEcho  = 0x500U;

/* ---- profile 1: the FCA variant, 125 kbit/s ---------------------------- */
inline constexpr std::uint32_t kFcaSteering     = 0x1E1U;
inline constexpr std::uint32_t kFcaIgnition     = 0x20BU;
inline constexpr std::uint32_t kFcaGear         = 0x20EU;  /* PRNDL + reverse  */
inline constexpr std::uint32_t kFcaRpm          = 0x211U;
inline constexpr std::uint32_t kFcaDoors        = 0x244U;
inline constexpr std::uint32_t kFcaIllum        = 0x308U;
inline constexpr std::uint32_t kFcaSwcButtons   = 0x3A3U;
inline constexpr std::uint32_t kFcaVendorEcho   = 0x513U;

/* ---- the vendor / SWC service protocol, identical on both profiles ------ */
/* 0x400..0x43F: the low 6 bits of the identifier ARE the key index. */
inline constexpr std::uint32_t kSwcServiceBase    = 0x400U;
inline constexpr std::uint32_t kSwcServiceMask    = 0xFC0U;
inline constexpr std::uint32_t kSwcServiceKeyMask = 0x03FU;
inline constexpr std::uint32_t kVendorHandshake   = 0x7FCU;  /* -> 0x7FD       */
inline constexpr std::uint32_t kVendorStatus      = 0x7FFU;  /* -> 0x7FE (p1)  */

/* ---- what this box transmits ------------------------------------------- */
inline constexpr std::uint32_t kTxSwcReply        = 0x416U;
inline constexpr std::uint32_t kTxEcho            = 0x516U;
inline constexpr std::uint32_t kTxHandshakeReply  = 0x7FDU;
inline constexpr std::uint32_t kTxDisplayMode     = 0x7FEU;

}  // namespace canid

/* ===========================================================================
 * The three stock entry points.
 * ========================================================================= */

/* Decoder entry point, called once per 8 ms main-loop slot. Maintains the
 * bus-present debounce off the ACC flag and then selects the profile from
 * ram::AppRam::profileActive (1 = FCA 125k, anything else = Jeep 83.333k). */
void dispatchGate() noexcept;        /* 0x08007D44 */

/* THIS box: Jeep WK family, 83.333 kbit/s body bus. */
void decodeProfileJeep() noexcept;   /* 0x08009010 */

/* The 125 kbit/s FCA variant. NOT this car. */
void decodeProfileFca() noexcept;    /* 0x08009680 */

}  // namespace logic

#endif /* CAN_DECODE_HPP */
