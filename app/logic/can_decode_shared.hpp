/* ============================================================================
 * can_decode_shared.hpp -- the decode steps BOTH car profiles use.
 *
 * PRIVATE to app/logic/can_decode.cpp. Not part of the module API; it exists
 * because the stock image simply DUPLICATED this code inside both decoders
 * (0x08009010 and 0x08009680 each carry their own byte-identical copy of the
 * 0x400..0x43F service block, the 0x7FC handshake and the 0x7FF injection).
 * One copy here, called from both, is the whole point -- see
 * docs/ARCHITECTURE.md section 4.
 *
 * The profiles feed these helpers from different CAN identifiers and sometimes
 * from different payload bytes; see the table in can_decode.hpp.
 *
 * Everything is `inline` and nothing here has a runtime constructor: this image
 * never runs .init_array (docs/ARCHITECTURE.md section 6).
 * ==========================================================================*/

#ifndef CAN_DECODE_SHARED_HPP
#define CAN_DECODE_SHARED_HPP

#include <cstdint>

#include "app_config.hpp"
#include "can_bus.hpp"
#include "can_decode.hpp"
#include "ram_map.hpp"
#include "soft_timer.hpp"

namespace logic {
namespace shared {

/* ===========================================================================
 * Constants the decoders share.
 * ========================================================================= */

/* Each decoder makes at most 20 passes per call (stock guard: "if (0x13 < i)
 * return"), so a flooded bus cannot starve the main loop. */
inline constexpr std::uint8_t kDrainPasses = 0x14U;   /* 20 */

/* The "bus alive" watchdog reload, written on EVERY accepted frame. The two
 * profiles deliberately disagree: 100 on profile 0, 0x14 on profile 1. */
inline constexpr std::uint8_t kBusAliveReloadJeep = 100U;

/* ---------------------------------------------------------------------------
 * PRNDL characters on Jeep CAN 0x003 d[4].
 *
 * Not a magic-number table: d[4] literally carries the ASCII code of the
 * character the instrument cluster is displaying, so these ARE the letters.
 * Evidence: docs/JEEP-WK-CAN-ID-byte-map.md, id 0x003, marked Confirmed.
 * ------------------------------------------------------------------------- */
inline constexpr std::uint8_t kAsciiP = 0x50U;   /* 'P' */
inline constexpr std::uint8_t kAsciiR = 0x52U;   /* 'R' */
inline constexpr std::uint8_t kAsciiN = 0x4EU;   /* 'N' */
inline constexpr std::uint8_t kAsciiD = 0x44U;   /* 'D' */
inline constexpr std::uint8_t kAscii1 = 0x31U;   /* '1' AutoStick upper limit  */
inline constexpr std::uint8_t kAscii2 = 0x32U;   /* '2' */
inline constexpr std::uint8_t kAscii3 = 0x33U;   /* '3' */
inline constexpr std::uint8_t kAscii4 = 0x34U;   /* '4' */

/* The head unit's 0x32[1] table is 1=P 2=N 3=R 4=D 5=S, and ram_map.hpp names
 * the first four. S has no ram:: equivalent because no stock decoder ever
 * produces it. docs/hiworld-radio-injectable.md section B. */
inline constexpr std::uint8_t kGearSport = 5U;
inline constexpr std::uint8_t kBusAliveReloadFca  = 0x14U;   /* 20 */

/* Illumination / dimmer: the raw byte is halved and capped. */
inline constexpr std::uint8_t kIllumPercentMax = 100U;

/* Ignition: the ACC bit is set when ANY of d[0] bits 5..7 is set. The stock
 * test is literally "(v & 0xE0) > 0x10", which is the same thing because
 * v & 0xE0 is always a multiple of 0x20. Reproduced in that form. */
inline constexpr std::uint8_t kIgnRunMask      = 0xE0U;
inline constexpr std::uint8_t kIgnRunThreshold = 0x10U;
inline constexpr std::uint8_t kIgnRawMask      = 0x01U;   /* -> kFlagIgnRaw */

/* Door bitfield, SOURCE side. The permutation to the destination bits in
 * ram::VehicleState::kDoors is NOT the identity -- see decodeDoors. */
inline constexpr std::uint8_t kSrcDoorLeftFront  = 0x01U;
inline constexpr std::uint8_t kSrcDoorRightFront = 0x02U;
inline constexpr std::uint8_t kSrcDoorRightRear  = 0x04U;
inline constexpr std::uint8_t kSrcDoorLeftRear   = 0x08U;
inline constexpr std::uint8_t kSrcDoorTailgate   = 0x10U;

/* SWC button groups in the 0x3A0 / 0x3A3 payload. */
inline constexpr std::uint8_t kBtnModeMask   = 0x01U;  /* d[0] */
inline constexpr std::uint8_t kBtnVolMask    = 0x06U;
inline constexpr std::uint8_t kBtnVolUp      = 0x02U;
inline constexpr std::uint8_t kBtnVolDown    = 0x04U;
inline constexpr std::uint8_t kBtnSeekMask   = 0x18U;
inline constexpr std::uint8_t kBtnSeekNext   = 0x08U;
inline constexpr std::uint8_t kBtnSeekPrev   = 0x10U;
inline constexpr std::uint8_t kBtnMuteMask   = 0x60U;
inline constexpr std::uint8_t kBtnMute       = 0x20U;
inline constexpr std::uint8_t kBtnSpare      = 0x40U;
inline constexpr std::uint8_t kBtnPhoneMask  = 0x01U;  /* d[1] */
inline constexpr std::uint8_t kBtnVoiceMask  = 0x02U;

/* The key codes the Android plugin (assets/car_key_3009001.json) expects. */
inline constexpr std::uint8_t kKeyNone   = 0x00U;
inline constexpr std::uint8_t kKeyVolUp  = 0x01U;
inline constexpr std::uint8_t kKeyVolDn  = 0x02U;
inline constexpr std::uint8_t kKeyMute   = 0x03U;
inline constexpr std::uint8_t kKeyPhone  = 0x05U;
inline constexpr std::uint8_t kKeyVoice  = 0x07U;
inline constexpr std::uint8_t kKeyNext   = 0x08U;
inline constexpr std::uint8_t kKeyPrev   = 0x09U;
inline constexpr std::uint8_t kKeyMode   = 0x0BU;
inline constexpr std::uint8_t kKeySpare  = 0x12U;  /* unmapped in the plugin */

/* 0x400..0x43F service protocol. */
inline constexpr std::uint8_t kSwcKeyReport     = 0xFDU;  /* d[0] marker       */
inline constexpr std::uint8_t kSwcHandshakeKey  = 0x16U;  /* d[1] == keydata   */
inline constexpr std::uint8_t kSwcHandshakeHold = 0x50U;  /* -> swcTimer       */
inline constexpr std::uint8_t kSwcRespModeHello = 9U;     /* -> swcRespMode    */
inline constexpr std::uint8_t kSwcReplyHello    = 0xFDU;  /* swcTxBuf[0]       */
inline constexpr std::uint8_t kSwcReplyKey      = 0xFEU;  /* swcTxBuf[0]       */
inline constexpr std::uint8_t kSwcProbeAccOn    = 0x3FU;  /* d[2]              */
inline constexpr std::uint8_t kSwcProbeAccOff   = 0xBFU;
inline constexpr std::uint8_t kSwcProbeBusDown  = 0xFFU;
inline constexpr std::uint8_t kSwcReplyDlc      = 8U;

/* 0x7FC handshake magic: 3F F3 F2 01 .. .. 3A A3. */
inline constexpr std::uint8_t kMagic7fcD0 = 0x3FU;
inline constexpr std::uint8_t kMagic7fcD1 = 0xF3U;
inline constexpr std::uint8_t kMagic7fcD2 = 0xF2U;
inline constexpr std::uint8_t kMagic7fcD3 = 0x01U;

/* 0x7FF status-injection magic: 7F F7 x x y y 3A A3, with d[2]==d[3] and
 * d[4]==d[5]. */
inline constexpr std::uint8_t kMagic7ffD0 = 0x7FU;
inline constexpr std::uint8_t kMagic7ffD1 = 0xF7U;

/* ...and the trailer both magics share. */
inline constexpr std::uint8_t kMagicTailD6 = 0x3AU;
inline constexpr std::uint8_t kMagicTailD7 = 0xA3U;

/* Steering, both profiles: centre 0x1000, 0.5 deg/LSB, big-endian raw. */
inline constexpr std::uint32_t kSteerCentre = 0x1000U;

/* ===========================================================================
 * Shared decode steps
 * ========================================================================= */

/* Door bitfield -> ram::VehicleState::kDoors (UART 0x12 payload[2]).
 *
 * THE PERMUTATION IS NOT THE IDENTITY, and getting it wrong shows a passenger
 * door as a tailgate:
 *     source bit 0 (LF) -> kDoorLeftFront  (bit 7)
 *     source bit 1 (RF) -> kDoorRightFront (bit 6)
 *     source bit 3 (LR) -> kDoorLeftRear   (bit 5)   <-- 3 and 2 cross over
 *     source bit 2 (RR) -> kDoorRightRear  (bit 4)   <--
 *     source bit 4 (TG) -> kDoorTailgate   (bit 3)
 * Profile 0 feeds this from 0x013 d[2]; profile 1 from 0x244 d[0]. */
inline void decodeDoors(std::uint8_t b) noexcept
{
    ram::VehicleState& v = ram::app().vehicle;

    v.setDoorBit(ram::VehicleState::kDoorLeftFront,  (b & kSrcDoorLeftFront)  != 0U);
    v.setDoorBit(ram::VehicleState::kDoorRightFront, (b & kSrcDoorRightFront) != 0U);
    v.setDoorBit(ram::VehicleState::kDoorLeftRear,   (b & kSrcDoorLeftRear)   != 0U);
    v.setDoorBit(ram::VehicleState::kDoorRightRear,  (b & kSrcDoorRightRear)  != 0U);
    v.setDoorBit(ram::VehicleState::kDoorTailgate,   (b & kSrcDoorTailgate)   != 0U);
}

/* Steering-wheel buttons -> the plugin key code in VehicleState::kSwcKey.
 * Profile 0: CAN 0x3A0; profile 1: CAN 0x3A3.
 *
 * NOTE THE DELIBERATE ORDERING: these are independent `if`s, not a chain, so a
 * LATER match OVERWRITES an earlier one and a multi-bit frame reports the LAST
 * matching group. Preserved verbatim from the stock image. */
inline void decodeSwcButtons(const volatile std::uint8_t* d) noexcept
{
    ram::VehicleState& v = ram::app().vehicle;

    if ((d[0] == 0x00U) && (d[1] == 0x00U)) {
        v.setSwcKey(kKeyNone);
        return;
    }

    if ((d[0] & kBtnModeMask) != 0U) {
        v.setSwcKey(kKeyMode);                     /* MODE / source */
    }

    if ((d[0] & kBtnVolMask) == kBtnVolUp) {
        v.setSwcKey(kKeyVolUp);
    } else if ((d[0] & kBtnVolMask) == kBtnVolDown) {
        v.setSwcKey(kKeyVolDn);
    }

    if ((d[0] & kBtnSeekMask) == kBtnSeekNext) {
        v.setSwcKey(kKeyNext);                     /* next / seek up   */
    } else if ((d[0] & kBtnSeekMask) == kBtnSeekPrev) {
        v.setSwcKey(kKeyPrev);                     /* prev / seek down */
    }

    if ((d[0] & kBtnMuteMask) == kBtnMute) {
        v.setSwcKey(kKeyMute);
    } else if ((d[0] & kBtnMuteMask) == kBtnSpare) {
        v.setSwcKey(kKeySpare);                    /* unmapped in the plugin */
    }

    if ((d[1] & kBtnPhoneMask) != 0U) {
        v.setSwcKey(kKeyPhone);                    /* answer / phone  */
    }

    if ((d[1] & kBtnVoiceMask) != 0U) {
        v.setSwcKey(kKeyVoice);                    /* voice assistant */
    }
}

/* Illumination-on + dimmer percent (best guess; could be fuel level -- see
 * docs/hiworld-firmware-analysis.md 3.8). Profile 0: 0x210 d[1]; profile 1:
 * 0x308 d[1]. The flag bit also drives the ILLUM GPIO (PC15, svc::io). */
inline void decodeIllumLevel(std::uint8_t v) noexcept
{
    ram::VehicleState& vs = ram::app().vehicle;

    vs.setFlag(ram::VehicleState::kFlagIllumOn, v != 0U);

    std::uint8_t percent = static_cast<std::uint8_t>(v >> 1);
    if (percent > kIllumPercentMax) {
        percent = kIllumPercentMax;
    }
    vs.setIllumLevel(percent);
}

/* Ignition / ACC. Profile 0: 0x000 d[0]; profile 1: 0x20B d[0].
 *   kFlagIgnRaw (bit 4) <- d[0] bit 0   -- unidentified, set in every capture
 *   kFlagAccOn  (bit 0) <- (d[0] & 0xE0) > 0x10, i.e. any of d[0] bits 5..7.
 * kFlagAccOn gates the whole TX block, drives the ACC GPIO and is what
 * dispatchGate() runs its bus-present debounce off. */
inline void decodeIgnition(std::uint8_t v) noexcept
{
    ram::VehicleState& vs = ram::app().vehicle;

    vs.setFlag(ram::VehicleState::kFlagIgnRaw, (v & kIgnRawMask) != 0U);
    vs.setFlag(ram::VehicleState::kFlagAccOn,
               static_cast<std::uint8_t>(v & kIgnRunMask) > kIgnRunThreshold);
}

/* Vendor handshake, CAN 0x7FC: magic 3F F3 F2 01 .. .. 3A A3 -> the received
 * payload is echoed straight back as 0x7FD. The compare ORDER is the stock one
 * (d0, d1, d6, d7, then d2, d3); kept so the diff against Ghidra is literal. */
inline void decodeVendor7fc(const volatile std::uint8_t* d) noexcept
{
    if ((d[0] == kMagic7fcD0) && (d[1] == kMagic7fcD1) &&
        (d[6] == kMagicTailD6) && (d[7] == kMagicTailD7) &&
        (d[2] == kMagic7fcD2) && (d[3] == kMagic7fcD3)) {
        /* `d` points straight into the RX ring; txEnqueue takes
         * `const volatile std::uint8_t*`, so no cast is needed. */
        svc::can::txEnqueue(canid::kTxHandshakeReply, 0U, 8U, d);
    }
}

/* Vendor status injection, CAN 0x7FF: magic 7F F7 x x y y 3A A3 with d[2]==d[3]
 * and d[4]==d[5]. Writes the four low flag bits of the state block from d[2]
 * and returns whether the magic matched -- profile 1 appends more work on a
 * match (see fcaVendor7ff).
 *
 * The four destination bits happen to be the four low VehicleState flags in
 * order, so the source masks below ARE the destination masks. */
inline bool decodeVendor7ffCommon(const volatile std::uint8_t* d) noexcept
{
    if (!((d[0] == kMagic7ffD0) && (d[1] == kMagic7ffD1) &&
          (d[6] == kMagicTailD6) && (d[7] == kMagicTailD7) &&
          (d[2] == d[3]) && (d[4] == d[5]))) {
        return false;
    }

    ram::VehicleState& v = ram::app().vehicle;
    v.setFlag(ram::VehicleState::kFlagAccOn,
              (d[2] & ram::VehicleState::kFlagAccOn) != 0U);
    v.setFlag(ram::VehicleState::kFlagIllumOn,
              (d[2] & ram::VehicleState::kFlagIllumOn) != 0U);
    v.setFlag(ram::VehicleState::kFlagReverse,
              (d[2] & ram::VehicleState::kFlagReverse) != 0U);
    v.setFlag(ram::VehicleState::kFlagSpare3,
              (d[2] & ram::VehicleState::kFlagSpare3) != 0U);
    return true;
}

/* Hiworld node-addressing / SWC service protocol on 0x400..0x43F.
 *
 * THE NAMES IN HERE ARE MISLEADING AND ARE KEPT ONLY FOR THE GHIDRA DIFF.
 * On the Jeep WK this range is the OSEK/VDX network-management token ring, and
 * the match is byte for byte: d[0]==0xFD is the NM "ring data / alive" opcode,
 * the low 6 bits of the id are the NODE ADDRESS (id = 0x400 + addr), d[1] is
 * the address of the NEXT node, and the d[2] probe values are NM status --
 * 0x3F awake, 0xBF sleep requested, 0xFF ring down. So this function is the
 * box's NM participation and sleep detection, which is why the 0xBF/0xFF paths
 * drive the ACC and bus-down logic, and why it has never produced a key code
 * on this car. Steering-wheel buttons are NOT in this range.
 * See docs/REQUIREMENTS.md, the note after FR-53, and
 * ../canbus-to-canbox-signal-map.md section 11. Behaviour is correct as-is --
 * do not "fix" anything here.
 *
 * THE LOW 6 BITS OF THE IDENTIFIER ARE THE KEY INDEX: the frame id selects
 * which software-timer slot is armed (svc::timers::armForKeycode reads
 * ram::AppRam::swc.keycode, which is set immediately before the call -- that
 * global IS the argument). d[0] == 0xFD marks a key report, d[1] carries the
 * key data and d[2] is a bus-state probe.
 *
 * Byte-for-byte identical in both profiles; the stock image duplicated the
 * whole block into each decoder. */
inline void decodeSwcService(std::uint32_t canId, const volatile std::uint8_t* d) noexcept
{
    if ((canId & canid::kSwcServiceMask) != canid::kSwcServiceBase) {
        return;
    }

    ram::AppRam& r = ram::app();

    r.swc.keycode = static_cast<std::uint8_t>(canId & canid::kSwcServiceKeyMask);
    svc::timers::armForKeycode();      /* softTimers[keycode] = 150 (1.5 s) */

    if (d[0] != kSwcKeyReport) {
        return;
    }

    r.swc.keydata = d[1];

    /* keydata 0x16 is the handshake. (It shares its value with
     * ram::kSoftTimerIdle, the "nothing held" slot sentinel.) */
    if (r.swc.keydata == kSwcHandshakeKey) {
        r.miscTimers.swcRespMode = kSwcRespModeHello;
        r.swcTxBuf[0] = kSwcReplyHello;
        r.swcTxBuf[1] = r.swc.keydataRef;
    } else if (r.swc.keydata == r.swc.keydataRef) {
        r.swcTxBuf[0] = kSwcReplyKey;
        r.swcTxBuf[1] = r.swc.keydataRef;
        svc::can::txEnqueue(canid::kTxSwcReply, 0U, kSwcReplyDlc, r.swcTxBuf);
    }

    if (r.swc.keycode == r.swc.keycodePrev) {
        if (r.swc.keydata == kSwcHandshakeKey) {
            r.miscTimers.swcTimer = kSwcHandshakeHold;
        } else {
            r.swcTxBuf[0] = kSwcReplyKey;
            r.swcTxBuf[1] = r.swc.keydataRef;
            svc::can::txEnqueue(canid::kTxSwcReply, 0U, kSwcReplyDlc, r.swcTxBuf);
        }
    } else if (r.swc.keycode == r.swc.keydataRef) {
        r.miscTimers.swcFlag = 0U;
    }

    /* The d[2] probe, qualified by the ACC/run flag. */
    const bool acc = r.vehicle.accOn();
    if ((d[2] == kSwcProbeAccOn) && acc) {
        r.swcTxBuf[2] = kSwcProbeAccOn;
    } else if ((d[2] == kSwcProbeAccOff) && !acc) {
        r.swcTxBuf[2] = kSwcProbeAccOff;
    } else if ((d[2] == kSwcProbeBusDown) && !acc) {
        /* Bus declared down: clear the presence state AND drop everything
         * already queued for transmission. */
        r.canBusStatus = cfg::kBusDown;
        svc::can::txFlush();
    }
}

}  // namespace shared
}  // namespace logic

#endif /* CAN_DECODE_SHARED_HPP */
