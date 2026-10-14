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
 *     PRNDL gear             (not decoded)        0x20E d[2] nibble
 *     engine speed           (not decoded)        0x211 d[0]:d[1]
 *
 * Everything in the `kSwcService*` / `kVendor*` / `kTx*` group below is
 * identical on both profiles.
 * ========================================================================= */
namespace canid {

/* ---- profile 0: Jeep WK family, 83.333 kbit/s -- THIS box --------------- */
inline constexpr std::uint32_t kJeepIgnition    = 0x000U;  /* ACC / run        */
inline constexpr std::uint32_t kJeepReverse     = 0x003U;  /* gear nibble      */
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
