/* ============================================================================
 * can_decode.cpp -- the CAN receive-frame decoders.
 *
 * Three stock functions live here:
 *     0x08007D44  can_profile_dispatch_gate   -> logic::dispatchGate
 *     0x08009010  can_dispatch_profile0_83k   -> logic::decodeProfileJeep
 *     0x08009680  can_dispatch_profile1_125k  -> logic::decodeProfileFca
 *
 * Both profiles drain the SAME 50-entry RX ring and fill the SAME
 * ram::VehicleState block; only the identifiers and a handful of extra signals
 * differ (the table in can_decode.hpp lines the two up). Everything they share
 * is in can_decode_shared.hpp -- the stock image duplicated it instead.
 *
 * Reconstructed from reference/decompile/app/can_rx_decode.c (Ghidra output) via
 * the retired app/port/can_decode.c. EVERY identifier, bit position, mask, scale and clamp
 * below is exactly what the decompile has. Nothing was rounded or "tidied";
 * where the stock code is odd, the oddity is reproduced and the comment says
 * so. Those comments are the reverse-engineering record.
 *
 * The RX ring is read through svc::can (rxAvailable / rxId / rxData /
 * rxAdvance), replies go out through svc::can::txEnqueue, and the SWC key
 * timers are armed through svc::timers::armForKeycode -- no raw addresses, no
 * ring arithmetic in this file.
 * ==========================================================================*/

#include "can_decode.hpp"

#include <cstdint>

#include "app_config.hpp"
#include "can_bus.hpp"
#include "can_decode_shared.hpp"
#include "ram_map.hpp"

namespace logic {
namespace {

using namespace logic::shared;

/* ===========================================================================
 * Profile 0 -- 83.333 kbit/s. THIS box (Jeep WK family, H1N4JPF20A / JPF2).
 *
 * No RPM, PRNDL or vehicle-speed decode exists on this profile: the only gear
 * information is the reverse nibble on 0x003.
 * ========================================================================= */

/* 0x015 d[4]:d[5] -- steering angle, centre 0x1000, 0.5 deg/LSB, big-endian.
 *
 * Only updated while the REVERSE flag is set: the angle exists to drive the
 * reverse-camera trajectory lines, so outside reverse the last value stands.
 * There is NO CLAMP on this profile (profile 1 clamps to +-0x21B), and a raw
 * value of 0 is reported as 0 rather than being run through the centring
 * arithmetic. */
void jeepSteering(const volatile std::uint8_t* d) noexcept
{
    ram::VehicleState& v = ram::app().vehicle;

    if (!v.reverseOn()) {
        return;
    }

    const std::uint16_t raw = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(d[4]) << 8) | d[5]);

    std::uint16_t angle = raw;                 /* raw == 0 -> reported as 0 */
    if (raw != 0U) {
        if (raw < kSteerCentre) {
            angle = static_cast<std::uint16_t>((kSteerCentre - raw) >> 1);
        } else if (raw == kSteerCentre) {
            angle = 0U;                        /* dead centre */
        } else {
            /* Right of centre: the negative half is a plain two-complement
             * negation on this profile. */
            angle = static_cast<std::uint16_t>(0U - ((raw - kSteerCentre) >> 1));
        }
    }

    v.setSteeringAngle(angle);
}

/* 0x003 d[2] low nibble -- reverse gear. Nibble 1 and nibble 9 both mean R. */
void jeepReverse(const volatile std::uint8_t* d) noexcept
{
    const std::uint8_t gear = static_cast<std::uint8_t>(d[2] & 0x0FU);

    ram::app().vehicle.setFlag(ram::VehicleState::kFlagReverse,
                               (gear == 1U) || (gear == 9U));
}

/* 0x500 d[0]:d[1] -- vendor echo, retransmitted as 0x516 ZERO-PADDED TO 8.
 * (Profile 1 echoes four bytes from 0x513; the padding is the same.) */
void jeepEcho500(const volatile std::uint8_t* d) noexcept
{
    ram::AppRam& r = ram::app();

    r.echoBuf516[0] = d[0];
    r.echoBuf516[1] = d[1];
    r.echoBuf516[2] = 0U;
    r.echoBuf516[3] = 0U;
    r.echoBuf516[4] = 0U;
    r.echoBuf516[5] = 0U;
    r.echoBuf516[6] = 0U;
    r.echoBuf516[7] = 0U;
    svc::can::txEnqueue(canid::kTxEcho, 0U, 8U, r.echoBuf516);
}

/* ===========================================================================
 * Profile 1 -- 125 kbit/s FCA variant (NOT this car).
 * ========================================================================= */


/* Steering clamp, profile 1 only: +-539 (0x21B), matching the trajectory
 * overlay in assets/car_trajectory_3009001.json. */
inline constexpr std::uint32_t kSteerClampPos = 0x21BU;
/* ...and the value the NEGATIVE side clamps to, which is 0x21C, not 0x21B. */
inline constexpr std::uint32_t kSteerClampNeg = 0x21CU;
/* The negative half is encoded as 0x10001 - v. 0x10001, NOT 0x10000. */
inline constexpr std::uint32_t kSteerNegBase = 0x10001U;

/* Engine speed: accepted 1 frame in 21 and rejected at or above 8000 rpm. */
inline constexpr std::uint8_t  kRpmDecimation = 0x14U;   /* counter 0..0x14 */
inline constexpr std::uint32_t kRpmRejectAt   = 8000U;

/* Head-unit display modes (ram::AppRam::hostCmd.cluster[0], the 0xD2 payload)
 * reflected back on 0x7FE. */
inline constexpr std::uint8_t kDisplayMode1 = 1U;
inline constexpr std::uint8_t kDisplayMode2 = 2U;
inline constexpr std::uint8_t kDisplayMode3 = 3U;
inline constexpr std::uint8_t kDisplayMode4 = 4U;
inline constexpr std::uint8_t kDisplayBit1  = 0x01U;
inline constexpr std::uint8_t kDisplayBit2  = 0x02U;
inline constexpr std::uint8_t kDisplayBit3  = 0x04U;

/* 0x1E1 d[2]:d[3] -- steering angle. Same centre and scale as profile 0, but
 * CLAMPED to +-0x21B, and the negative half is encoded as 0x10001 - v.
 *
 * The 0x10001 (not 0x10000) and the 0x21C on the negative-side clamp are what
 * the stock code does. Only bytes 0 and 1 of the result are stored, so the
 * truncation to 16 bits makes the reported value 1 - v.
 *
 * Gated on the reverse flag, exactly like profile 0. */
void fcaSteering(const volatile std::uint8_t* d) noexcept
{
    ram::VehicleState& v = ram::app().vehicle;

    if (!v.reverseOn()) {
        return;
    }

    const std::uint32_t raw = (static_cast<std::uint32_t>(d[2]) << 8)
                            | static_cast<std::uint32_t>(d[3]);

    std::uint32_t angle;
    if (raw < kSteerCentre) {
        angle = (kSteerCentre - raw) >> 1;
        if (angle > kSteerClampPos) {
            angle = kSteerClampPos;
        }
    } else if (raw == kSteerCentre) {
        angle = 0U;                            /* dead centre */
    } else {
        angle = (raw - kSteerCentre) >> 1;
        if (angle > kSteerClampPos) {
            angle = kSteerClampNeg;            /* 0x21C on this side */
        }
        angle = kSteerNegBase - angle;         /* 0x10001 - v */
    }

    v.setSteeringAngle(static_cast<std::uint16_t>(angle));
}

/* d[2] bit 3 of the 0x20E frame: reverse. (Source bit 3 -- NOT the same bit as
 * the kFlagReverse it ends up in, which is bit 2 of the state block.) */
inline constexpr std::uint8_t kFcaGearReverseBit = 0x08U;

/* 0x20E d[2] -- PRNDL. Low nibble 0 -> P, 2 -> N, 4 -> D, 9 -> LEAVE ALONE,
 * anything else -> 0 (invalid). Then d[2] bit 3 is the reverse flag and, when
 * set, FORCES the reported gear to R regardless of the nibble. */
void fcaGear(const volatile std::uint8_t* d) noexcept
{
    ram::VehicleState& v = ram::app().vehicle;
    const std::uint8_t nibble = static_cast<std::uint8_t>(d[2] & 0x0FU);

    if (nibble == 0U) {
        v.setGear(ram::kGearPark);
    } else if (nibble == 2U) {
        v.setGear(ram::kGearNeutral);
    } else if (nibble == 4U) {
        v.setGear(ram::kGearDrive);
    } else if (nibble != 9U) {
        v.setGear(ram::kGearInvalid);               /* 9 leaves the gear alone */
    }

    if ((d[2] & kFcaGearReverseBit) == 0U) {
        v.setFlag(ram::VehicleState::kFlagReverse, false);
    } else {
        v.setFlag(ram::VehicleState::kFlagReverse, true);
        v.setGear(ram::kGearReverse);
    }
}

/* 0x211 d[0]:d[1] -- engine speed, big-endian, 1 rpm/LSB.
 *
 * DECIMATED 1-IN-21: the counter runs 0..0x14 and only the pass that finds it
 * already at 0x14 decodes (and resets it), so 20 frames out of every 21 are
 * dropped. Values at or above 8000 rpm are rejected outright. */
void fcaRpm(const volatile std::uint8_t* d) noexcept
{
    ram::AppRam& r = ram::app();

    if (r.rpmDecimator < kRpmDecimation) {
        r.rpmDecimator = static_cast<std::uint8_t>(r.rpmDecimator + 1U);
        return;
    }

    r.rpmDecimator = 0U;

    const std::uint32_t rpm = (static_cast<std::uint32_t>(d[0]) * 0x100U)
                            + static_cast<std::uint32_t>(d[1]);
    if (rpm < kRpmRejectAt) {
        r.vehicle.setRpm(d[0], d[1]);
    }
}

/* 0x513 d[0..3] -- vendor echo, retransmitted as 0x516 zero-padded to 8. */
void fcaEcho513(const volatile std::uint8_t* d) noexcept
{
    ram::AppRam& r = ram::app();

    r.echoBuf516[0] = d[0];
    r.echoBuf516[1] = d[1];
    r.echoBuf516[2] = d[2];
    r.echoBuf516[3] = d[3];
    r.echoBuf516[4] = 0U;
    r.echoBuf516[5] = 0U;
    r.echoBuf516[6] = 0U;
    r.echoBuf516[7] = 0U;
    svc::can::txEnqueue(canid::kTxEcho, 0U, 8U, r.echoBuf516);
}

/* 0x7FF on this profile: the shared status injection, PLUS a PRNDL byte in
 * d[4] mapped into the SWC-KEY slot (yes, the key slot, not the gear slot --
 * that is what the stock code does), PLUS a one-byte reply on 0x7FE carrying
 * the head unit current display mode (the 0xD2 sub-command byte in
 * ram::AppRam::hostCmd.cluster[0]). */
void fcaVendor7ff(const volatile std::uint8_t* d) noexcept
{
    if (!decodeVendor7ffCommon(d)) {
        return;
    }

    ram::AppRam& r = ram::app();

    switch (d[4]) {
    case 0U: r.vehicle.setSwcKey(kKeyNone);  break;
    case 1U: r.vehicle.setSwcKey(kKeyVolDn); break;
    case 2U: r.vehicle.setSwcKey(kKeyVolUp); break;
    case 3U: r.vehicle.setSwcKey(kKeyPrev);  break;
    case 4U: r.vehicle.setSwcKey(kKeyNext);  break;
    case 5U: r.vehicle.setSwcKey(kKeyMode);  break;
    case 6U: r.vehicle.setSwcKey(kKeyMute);  break;
    default: break;                      /* 7..255 leave the slot unchanged */
    }

    /* The display-mode reply byte. Written as four independent set-or-clear
     * steps, exactly as the stock code does. */
    r.prndlTx7fe[0] = 0U;

    if (r.hostCmd.cluster[0] == kDisplayMode1) {
        r.prndlTx7fe[0] = static_cast<std::uint8_t>(r.prndlTx7fe[0] | kDisplayBit1);
    } else {
        r.prndlTx7fe[0] = static_cast<std::uint8_t>(r.prndlTx7fe[0]
                                                    & static_cast<std::uint8_t>(~kDisplayBit1));
    }

    if (r.hostCmd.cluster[0] == kDisplayMode2) {
        r.prndlTx7fe[0] = static_cast<std::uint8_t>(r.prndlTx7fe[0] | kDisplayBit2);
    } else {
        r.prndlTx7fe[0] = static_cast<std::uint8_t>(r.prndlTx7fe[0]
                                                    & static_cast<std::uint8_t>(~kDisplayBit2));
    }

    if (r.hostCmd.cluster[0] == kDisplayMode3) {
        r.prndlTx7fe[0] = static_cast<std::uint8_t>(r.prndlTx7fe[0] | kDisplayBit3);
    } else {
        r.prndlTx7fe[0] = static_cast<std::uint8_t>(r.prndlTx7fe[0]
                                                    & static_cast<std::uint8_t>(~kDisplayBit3));
    }

    /* NOTE: the stock image tests mode 4 against BIT 2 (0x04 / 0xFB) AGAIN
     * instead of bit 3. It looks like a copy-paste slip in the original, but it
     * is reproduced exactly: mode 4 is reported on mode 3 bit, and because this
     * clause also CLEARS that bit when the mode is not 4, mode 3 ends up
     * reporting 0x00 -- indistinguishable from "no mode". Do not "fix" it. */
    if (r.hostCmd.cluster[0] == kDisplayMode4) {
        r.prndlTx7fe[0] = static_cast<std::uint8_t>(r.prndlTx7fe[0] | kDisplayBit3);
    } else {
        r.prndlTx7fe[0] = static_cast<std::uint8_t>(r.prndlTx7fe[0]
                                                    & static_cast<std::uint8_t>(~kDisplayBit3));
    }

    svc::can::txEnqueue(canid::kTxDisplayMode, 0U, 1U, r.prndlTx7fe);
}

}  // namespace

/* ===========================================================================
 * 0x08009010 -- profile 0, Jeep WK family, 83.333 kbit/s. THIS box.
 * ========================================================================= */
void decodeProfileJeep() noexcept
{
    for (std::uint8_t pass = 0U; pass < kDrainPasses; ++pass) {
        if (!svc::can::rxAvailable()) {
            /* STOCK SHAPE: on an empty ring the loop `continue`s and spins the
             * guard out instead of returning early. Kept deliberately -- the
             * pass count, not the ring, is what ends the call. */
            continue;
        }

        /* Bus-alive watchdog reload. 100 here; profile 1 uses 0x14. */
        ram::app().canActivityCtr = kBusAliveReloadJeep;

        const std::uint32_t id = svc::can::rxId();
        const volatile std::uint8_t* const d = svc::can::rxData();

        switch (id) {
        case canid::kJeepIgnition:   decodeIgnition(d[0]);    break; /* ACC / run  */
        case canid::kJeepReverse:    jeepReverse(d);          break; /* reverse    */
        case canid::kJeepDoors:      decodeDoors(d[2]);       break; /* doors      */
        case canid::kJeepSteering:   jeepSteering(d);         break; /* steering   */
        case canid::kJeepIllum:      decodeIllumLevel(d[1]);  break; /* illum + %  */
        case canid::kJeepSwcButtons: decodeSwcButtons(d);     break; /* SWC keys   */
        case canid::kJeepVendorEcho: jeepEcho500(d);          break; /* -> 0x516   */
        case canid::kVendorHandshake: decodeVendor7fc(d);     break; /* -> 0x7FD   */
        case canid::kVendorStatus:
            (void)decodeVendor7ffCommon(d);                   break; /* injection  */
        default:                     decodeSwcService(id, d); break; /* 0x400..43F */
        }

        svc::can::rxAdvance();
    }
}

/* ===========================================================================
 * 0x08009680 -- profile 1, the 125 kbit/s FCA variant. NOT this car.
 * ========================================================================= */
void decodeProfileFca() noexcept
{
    for (std::uint8_t pass = 0U; pass < kDrainPasses; ++pass) {
        if (!svc::can::rxAvailable()) {
            continue;              /* same stock shape as profile 0 */
        }

        /* NOTE the different reload value: 0x14 here vs 100 on profile 0. */
        ram::app().canActivityCtr = kBusAliveReloadFca;

        const std::uint32_t id = svc::can::rxId();
        const volatile std::uint8_t* const d = svc::can::rxData();

        switch (id) {
        case canid::kFcaSteering:    fcaSteering(d);          break; /* steering   */
        case canid::kFcaIgnition:    decodeIgnition(d[0]);    break; /* ACC / run  */
        case canid::kFcaGear:        fcaGear(d);              break; /* PRNDL + R  */
        case canid::kFcaRpm:         fcaRpm(d);               break; /* engine rpm */
        case canid::kFcaDoors:       decodeDoors(d[0]);       break; /* doors      */
        case canid::kFcaIllum:       decodeIllumLevel(d[1]);  break; /* illum + %  */
        case canid::kFcaSwcButtons:  decodeSwcButtons(d);     break; /* SWC keys   */
        case canid::kFcaVendorEcho:  fcaEcho513(d);           break; /* -> 0x516   */
        case canid::kVendorHandshake: decodeVendor7fc(d);     break; /* -> 0x7FD   */
        case canid::kVendorStatus:   fcaVendor7ff(d);         break; /* + -> 0x7FE */
        default:                     decodeSwcService(id, d); break; /* 0x400..43F */
        }

        svc::can::rxAdvance();
    }
}

/* ===========================================================================
 * 0x08007D44 -- the decoder entry point.
 *
 * Before dispatching it maintains the "bus present" debounce off the ACC/run
 * flag:
 *   - ACC clear -> reload the 800-pass debounce and SET bit 4 of the periodic
 *                  0x18C TX frame (the "no ignition" marker);
 *   - ACC set and the debounce expired -> clear that bit and re-arm the 0x3D0
 *                  amplifier TX frame from the amplifier level;
 *   - otherwise -> count the debounce down.
 *
 * Then the profile byte (ram::AppRam::profileActive, latched from parameter
 * page byte 0x01) selects the decoder: 1 = FCA 125 kbit/s, anything else =
 * Jeep 83.333 kbit/s.
 * ========================================================================= */
void dispatchGate() noexcept
{
    /* Bit 4 of the 0x18C periodic frame: "ignition is off". */
    constexpr std::uint8_t kNoIgnitionMarker = 0x10U;

    ram::AppRam& r = ram::app();

    if (!r.vehicle.accOn()) {
        r.busPresentDebounce = cfg::kBusPresentDebounce;
        r.txFrame18c[0] = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(r.txFrame18c[0]
                                      & static_cast<std::uint8_t>(~kNoIgnitionMarker))
            + kNoIgnitionMarker);
    } else if (r.busPresentDebounce == 0U) {
        r.txFrame18c[0] = static_cast<std::uint8_t>(
            r.txFrame18c[0] & static_cast<std::uint8_t>(~kNoIgnitionMarker));
        r.txFrameAmp[0] = r.vehicle.ampLevel();
    } else {
        r.busPresentDebounce = r.busPresentDebounce - 1U;
    }

    if (r.profileActive == cfg::kProfileFca125k) {
        decodeProfileFca();
    } else {
        decodeProfileJeep();
    }
}

}  // namespace logic
