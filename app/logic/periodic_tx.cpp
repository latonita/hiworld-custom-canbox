/* ============================================================================
 * periodic_tx.cpp -- the 151-phase periodic CAN transmit table (0x08008110)
 * and the 0xD2 cluster-text translation (0x08007DA8).
 *
 * Ported from reference/decompile/app/can.c and reference/decompile/app/uart.c (Ghidra output) via
 * the retired app/port/app_glue.c. The comments below are the reverse-engineering record:
 * they say what the STOCK image does and why, which is the only authority this
 * rebuild has. Do not drop them.
 *
 * Two pieces of vehicle state this scheduler owns are named in ram_map.hpp:
 * ram::VehicleState::kAccShadow (byte 0x12, the ACC shadow the edge detector
 * keeps as a whole byte, not a flag bit) and kFlagProfileSel (bit 7 of the
 * flags byte, the latched CAN profile republished for the head unit).
 * ==========================================================================*/

#include <cstddef>
#include <cstdint>

#include "app_config.hpp"
#include "ram_map.hpp"

#include "can_bus.hpp"
#include "param_store.hpp"

#include "periodic_tx.hpp"

namespace logic {
namespace {

/* ===========================================================================
 * The CAN identifiers.
 *
 * The same five payload buffers go out under different identifiers depending
 * on which car this box is wired into (ram::AppRam::profileActive, latched by
 * main_loop once the autobaud has settled). Profile 0 is THIS box -- the Jeep
 * WK family at 83.333 kbit/s; profile 1 is the FCA 125 kbit/s variant, which
 * has no equivalent of the 0x09F frame.
 * ========================================================================= */
constexpr std::uint32_t kIdAmpProfile0 = 0x3D0U;   /* amplifier / tone        */
constexpr std::uint32_t kIdF0Profile0  = 0x0F0U;
constexpr std::uint32_t kId326Profile0 = 0x326U;
constexpr std::uint32_t kId190Profile0 = 0x190U;   /* cluster text, class     */
constexpr std::uint32_t kId18cProfile0 = 0x18CU;   /* cluster text, value     */
constexpr std::uint32_t kId9fProfile0  = 0x09FU;   /* cluster text, mirror    */

constexpr std::uint32_t kIdAmpProfile1 = 0x3D9U;
constexpr std::uint32_t kIdF0Profile1  = 0x2E9U;
constexpr std::uint32_t kId326Profile1 = 0x290U;
constexpr std::uint32_t kId190Profile1 = 0x291U;
constexpr std::uint32_t kId18cProfile1 = 0x293U;
/* There is no profile-1 identifier for txFrame9f: phase 0x0B is an explicit
 * empty case in the stock jump table for that profile. */

/* Every frame this unit sends is a standard (11-bit) identifier. */
constexpr std::uint8_t kIdeStandard = 0U;

/* Payload lengths, one per buffer. They are NOT all 8: the amplifier and
 * cluster-class frames are 7 bytes and the 0x0F0 frame is 4, and the stock
 * code passes those lengths explicitly at every call site. */
constexpr std::uint8_t kDlcAmp = 7U;
constexpr std::uint8_t kDlcF0  = 4U;
constexpr std::uint8_t kDlc326 = 8U;
constexpr std::uint8_t kDlc190 = 7U;
constexpr std::uint8_t kDlc18c = 8U;
constexpr std::uint8_t kDlc9f  = 8U;

/* ===========================================================================
 * The phase table.
 *
 * periodicTxPhase free-runs 0..0x96 -- 151 phases -- advancing one per 8 ms
 * call, so a lap takes ~1.2 s and each frame below goes out once per lap. Only
 * the odd phases 1..0x0B carry a frame; 0x0D is an explicit EMPTY case in the
 * stock jump table (not a fall-through), and every phase from 0x0E to 0x96 is
 * dead time that spaces the six frames apart.
 * ========================================================================= */
constexpr std::uint8_t kPhaseAmp   = 0x01U;
constexpr std::uint8_t kPhaseF0    = 0x03U;
constexpr std::uint8_t kPhase326   = 0x05U;
constexpr std::uint8_t kPhase190   = 0x07U;
constexpr std::uint8_t kPhase18c   = 0x09U;
constexpr std::uint8_t kPhase9f    = 0x0BU;
constexpr std::uint8_t kPhaseMax   = 0x96U;           /* last phase, then wrap */
constexpr std::uint8_t kPhaseCount = kPhaseMax + 1U;  /* 151 phases per lap    */
static_assert(kPhaseCount == 151U, "the stock table is 151 phases long");

/* ===========================================================================
 * The post-ACC transmit hold.
 *
 * Pinned at 400 while ACC is on; once ACC drops it counts down one per 8 ms
 * call, i.e. the box keeps talking to the car for 400 * 8 ms = 3.2 s after the
 * key is out and then stops. 32-bit in the stock image (0x2000119C).
 * ========================================================================= */
constexpr std::uint32_t kTxHoldReload = 400U;

/* The two values the ACC edge detector primes txFrame9f[0] with. */
constexpr std::uint8_t kFrame9fAccOn  = 1U;
constexpr std::uint8_t kFrame9fAccOff = 7U;

/* ===========================================================================
 * Vehicle-state bytes this unit owns that ram::VehicleState has no NAME for.
 * ========================================================================= */


/* ===========================================================================
 * The head unit's tone / volume command (UART cmd 0xAD).
 *
 * selector 1 = volume, 2..6 = BAL FAD BASS MID TRE. Each lands in three places
 * at once: the amplifier frame byte the car reads, the vehicle-state byte the
 * head unit reads back, and the parameter page so it survives a power cycle.
 * ========================================================================= */
constexpr std::uint8_t kSelVolume = 1U;
constexpr std::uint8_t kSelLast   = 6U;
constexpr std::uint8_t kVolumeMax = 0x26U;   /* 38 */
constexpr std::uint8_t kToneMin   = 0x01U;
constexpr std::uint8_t kToneMax   = 0x13U;   /* 19 */
constexpr std::uint8_t kToneLimit = 0x14U;   /* first value that clamps high  */

/* The "new data" flag value the UART dispatcher sets and this unit consumes. */
constexpr std::uint8_t kCmdPending = 1U;

/* ---------------------------------------------------------------------------
 * 0xD2 cluster text: ASCII digit helpers.
 *
 * The head unit sends the numeric part of a cluster message as ASCII, so every
 * digit is a plain subtract of the ASCII zero. The stock code does not validate
 * them except in the 0x06/0x07/0x0D/0x0E/0x15/0x16 branch; a non-digit
 * elsewhere silently produces nonsense, exactly as it does in the car.
 * ------------------------------------------------------------------------- */
constexpr std::uint8_t kAsciiZero = 0x30U;
constexpr std::uint8_t kAsciiOne  = 0x31U;
constexpr std::uint8_t kAsciiNine = 0x39U;

[[nodiscard]] std::uint16_t digitOf(std::uint8_t c) noexcept
{
    return static_cast<std::uint16_t>(c - kAsciiZero);
}

[[nodiscard]] bool isAsciiDigit(std::uint8_t c) noexcept
{
    return (c >= kAsciiZero) && (c <= kAsciiNine);
}

/* The class code lives in the LOW FIVE BITS of txFrame190[0] and the sub-code
 * in the low five bits of txFrame190[1]; the top three bits of both are owned
 * by another writer and are always preserved.
 *
 * The stock form is "(old & 0xE0) + code", an ADD rather than an OR. Every code
 * used is < 0x20 so the two are identical here; the add is kept because it is
 * what the image does. */
constexpr std::uint8_t kClassKeepMask = 0xE0U;
constexpr std::uint8_t kClassCodeMask = 0x1FU;

void setClassCode(std::uint8_t code) noexcept
{
    volatile std::uint8_t* f190 = ram::app().txFrame190;
    f190[0] = static_cast<std::uint8_t>((f190[0] & kClassKeepMask) + code);
}

void setSubCode(std::uint8_t code) noexcept
{
    volatile std::uint8_t* f190 = ram::app().txFrame190;
    f190[1] = static_cast<std::uint8_t>((f190[1] & kClassKeepMask) + code);
}

/* The 16-bit numeric value is split the same way in both frames: in txFrame18c
 * the HIGH nibble of the high byte is dropped, because txFrame18c[1]'s top
 * nibble belongs to another field. */
constexpr std::uint8_t kValueHiNibbleMask = 0x0FU;
constexpr std::uint8_t kValueKeepMask     = 0xF0U;

/* txFrame9f[1] is or-ed with 0x20 on the numeric classes (0x00..0x05) but NOT
 * on the 0x06.. group. That asymmetry is in the image; it is reproduced. */
constexpr std::uint8_t kFrame9fValueTag = 0x20U;

void storeValue18c(std::uint16_t value) noexcept
{
    volatile std::uint8_t* f18c = ram::app().txFrame18c;
    const std::uint8_t     hi   = static_cast<std::uint8_t>(value >> 8);

    f18c[1] = static_cast<std::uint8_t>((f18c[1] & kValueKeepMask)
                                        | (hi & kValueHiNibbleMask));
    f18c[2] = static_cast<std::uint8_t>(value & 0xFFU);
}

/* Shared tail of the 0x00..0x05 branches: fold `value` into 0x18C and 0x09F. */
void storeValue(std::uint16_t value) noexcept
{
    ram::AppRam&       r  = ram::app();
    const std::uint8_t hi = static_cast<std::uint8_t>(value >> 8);
    const std::uint8_t lo = static_cast<std::uint8_t>(value & 0xFFU);

    storeValue18c(value);

    r.txFrame9f[0] = static_cast<std::uint8_t>(r.txFrame190[0] & kClassCodeMask);
    r.txFrame9f[1] = static_cast<std::uint8_t>(hi | kFrame9fValueTag);
    r.txFrame9f[2] = lo;
}

/* --- the three/four digit parse shared by classes 0x01..0x05 ---------------
 * cluster[4] == '1' means a four-digit number whose thousands digit is
 * cluster[4] itself; otherwise it is a three-digit number starting at
 * cluster[5]. Either way one byte is SKIPPED: classes 1..3 take the units digit
 * from cluster[8] and classes 4..5 from cluster[7]. That asymmetry is in the
 * image; it is reproduced, not corrected, which is why the index is a
 * parameter rather than a constant. */
[[nodiscard]] std::uint16_t parseDigits(std::size_t unitsIndex) noexcept
{
    const volatile std::uint8_t* c = ram::app().hostCmd.cluster;

    std::uint32_t value = digitOf(c[unitsIndex]);
    if (c[4] == kAsciiOne) {
        value += static_cast<std::uint32_t>(digitOf(c[4])) * 1000U;
    }
    value += static_cast<std::uint32_t>(digitOf(c[5])) * 100U;
    value += static_cast<std::uint32_t>(digitOf(c[6])) * 10U;

    return static_cast<std::uint16_t>(value & 0xFFFFU);
}

/* The 0xD2 message classes, named. Everything not listed is an explicit empty
 * case in the stock jump table: the message is accepted and ignored. */
constexpr std::uint8_t kClsBlank  = 0x00U;
constexpr std::uint8_t kClsNum1   = 0x01U;
constexpr std::uint8_t kClsNum2   = 0x02U;
constexpr std::uint8_t kClsNum3   = 0x03U;
constexpr std::uint8_t kClsAlt1   = 0x04U;
constexpr std::uint8_t kClsAlt2   = 0x05U;
constexpr std::uint8_t kClsText6  = 0x06U;
constexpr std::uint8_t kClsText7  = 0x07U;
constexpr std::uint8_t kClsTextD  = 0x0DU;
constexpr std::uint8_t kClsTextE  = 0x0EU;
constexpr std::uint8_t kClsText15 = 0x15U;
constexpr std::uint8_t kClsText16 = 0x16U;
constexpr std::uint8_t kClsPopupA = 0x0AU;
constexpr std::uint8_t kClsPopupC = 0x0CU;

/* The class codes those map onto in txFrame190. */
constexpr std::uint8_t kCodeBlank    = 0x1DU;
constexpr std::uint8_t kCodeNum      = 0x01U;
constexpr std::uint8_t kCodeAlt      = 0x00U;
constexpr std::uint8_t kCodeText     = 0x04U;
constexpr std::uint8_t kCodePopupA   = 0x08U;
constexpr std::uint8_t kCodePopupC   = 0x06U;
constexpr std::uint8_t kSubCodeText  = 0x03U;
constexpr std::uint8_t kSubCodeText7 = 0x08U;

/* Where the units digit is read from in each of the two numeric groups. */
constexpr std::size_t kUnitsIdxNum = 8U;   /* classes 0x01..0x03 */
constexpr std::size_t kUnitsIdxAlt = 7U;   /* classes 0x04..0x05 */

}  // namespace

/* ===========================================================================
 * 0x08007DA8 -- uart_D2_cluster_text_to_can
 *
 * See periodic_tx.hpp for what this produces. Two pieces of UNCERTAINTY are
 * flagged here rather than hidden:
 *
 *   1. Ghidra shows the stock function taking an `undefined2 param_1`, but its
 *      only call site (the scheduler below) passes nothing: param_1 is an
 *      UNINITIALISED register that the 0x06/0x07/0x0D/0x0E/0x15/0x16 branch
 *      reads when the digit test fails. It is reconstructed as a local
 *      initialised to 0 -- the only DEFINED behaviour available. The stock
 *      image emits whatever happened to be left in that register.
 *   2. The units-digit asymmetry between classes 1..3 and 4..5 (parseDigits).
 * ========================================================================= */
/* 0x08007DA8 */
void clusterTextToCan() noexcept
{
    ram::AppRam&                 r = ram::app();
    const volatile std::uint8_t* c = r.hostCmd.cluster;

    /* The 0xD2 receive state machine is released as soon as the block has been
     * consumed, so the next message can be parsed into it. */
    r.d2ClusterState = 0U;

    const std::uint8_t cls = c[0];

    switch (cls) {
    case kClsBlank:
        setClassCode(kCodeBlank);
        break;

    case kClsNum1:
    case kClsNum2:
    case kClsNum3:
        setClassCode(kCodeNum);
        storeValue(parseDigits(kUnitsIdxNum));
        break;

    case kClsAlt1:
    case kClsAlt2:
        setClassCode(kCodeAlt);
        storeValue(parseDigits(kUnitsIdxAlt));
        break;

    case kClsText6:
    case kClsText7:
    case kClsTextD:
    case kClsTextE:
    case kClsText15:
    case kClsText16: {
        /* See UNCERTAINTY note 1: uninitialised in the stock image. */
        std::uint16_t textVal = 0U;

        setSubCode(kSubCodeText);
        storeValue18c(0U);

        /* The stock code spells this out as a nested if-chain over the same six
         * class values, and every arm of it writes code 4. Class 7 additionally
         * overwrites the sub-code it has just set to 3 with 8. */
        setClassCode(kCodeText);
        if (cls == kClsText7) {
            setSubCode(kSubCodeText7);
        }

        /* Three ASCII digits at cluster[1..3], parsed only if cluster[1] really
         * is a digit. When it is not, 0x18C keeps the zero written above and
         * 0x09F gets the uninitialised value. */
        if (isAsciiDigit(c[1])) {
            textVal = static_cast<std::uint16_t>(
                (static_cast<std::uint32_t>(digitOf(c[1])) * 100U
                 + static_cast<std::uint32_t>(digitOf(c[2])) * 10U
                 + static_cast<std::uint32_t>(digitOf(c[3]))) & 0xFFFFU);
            storeValue18c(textVal);
        }

        /* NOTE: unlike storeValue(), byte 1 is NOT or-ed with 0x20 here. */
        r.txFrame9f[0] = static_cast<std::uint8_t>(r.txFrame190[0] & kClassCodeMask);
        r.txFrame9f[1] = static_cast<std::uint8_t>(textVal >> 8);
        r.txFrame9f[2] = static_cast<std::uint8_t>(textVal & 0xFFU);
        break;
    }

    case kClsPopupA:
        setClassCode(kCodePopupA);
        break;

    case kClsPopupC:
        setClassCode(kCodePopupC);
        r.txFrame9f[0] = static_cast<std::uint8_t>(r.txFrame190[0] & kClassCodeMask);
        break;

    /* 0x08, 0x09, 0x0B, 0x0F..0x14 and 0x17 are explicit empty cases in the
     * stock jump table -- accepted and deliberately ignored. */
    default:
        break;
    }
}

namespace {

/* --- part 3: one phase of the table -------------------------------------- */
void emitPhaseFrame(std::uint8_t profile, std::uint8_t phase) noexcept
{
    ram::AppRam& r = ram::app();

    if (profile == cfg::kProfileJeep83k) {
        switch (phase) {
        case kPhaseAmp:
            svc::can::txEnqueue(kIdAmpProfile0, kIdeStandard, kDlcAmp, r.txFrameAmp);
            break;
        case kPhaseF0:
            svc::can::txEnqueue(kIdF0Profile0, kIdeStandard, kDlcF0, r.txFrameF0);
            break;
        case kPhase326:
            svc::can::txEnqueue(kId326Profile0, kIdeStandard, kDlc326, r.txFrame326);
            break;
        case kPhase190:
            svc::can::txEnqueue(kId190Profile0, kIdeStandard, kDlc190, r.txFrame190);
            break;
        case kPhase18c:
            svc::can::txEnqueue(kId18cProfile0, kIdeStandard, kDlc18c, r.txFrame18c);
            break;
        case kPhase9f:
            svc::can::txEnqueue(kId9fProfile0, kIdeStandard, kDlc9f, r.txFrame9f);
            break;
        default:
            /* includes the stock empty case 0x0D */
            break;
        }
    } else if (profile == cfg::kProfileFca125k) {
        switch (phase) {
        case kPhaseAmp:
            svc::can::txEnqueue(kIdAmpProfile1, kIdeStandard, kDlcAmp, r.txFrameAmp);
            break;
        case kPhaseF0:
            svc::can::txEnqueue(kIdF0Profile1, kIdeStandard, kDlcF0, r.txFrameF0);
            break;
        case kPhase326:
            svc::can::txEnqueue(kId326Profile1, kIdeStandard, kDlc326, r.txFrame326);
            break;
        case kPhase190:
            svc::can::txEnqueue(kId190Profile1, kIdeStandard, kDlc190, r.txFrame190);
            break;
        case kPhase18c:
            svc::can::txEnqueue(kId18cProfile1, kIdeStandard, kDlc18c, r.txFrame18c);
            break;
        default:
            /* 0x0B and 0x0D are both empty cases in the image for profile 1 */
            break;
        }
    } else {
        /* Any other latched profile transmits nothing: the stock tests are
         * `== 0` and `== 1`, with no else branch. */
    }
}

/* --- part 4b: clamp the pending tone/volume value -------------------------
 * Volume (selector 1) is 0..0x26. Everything else is a tone byte and is held
 * inside 1..0x13: zero is pulled up to 1 and anything >= 0x14 is pulled down to
 * 0x13. The clamp is written BACK into the command block, which is where the
 * value is then read from -- so a clamped value is also what the head unit sees
 * echoed on the next read of that byte. */
void clampPendingValue() noexcept
{
    volatile std::uint8_t* cmd = ram::app().hostCmd.periodicTxCfg;

    if (cmd[0] == kSelVolume) {
        if (cmd[1] > kVolumeMax) {
            cmd[1] = kVolumeMax;
        }
    } else if (cmd[1] < kToneLimit) {
        if (cmd[1] == 0U) {
            cmd[1] = kToneMin;
        }
    } else {
        cmd[1] = kToneMax;
    }
}

/* --- part 4b: apply it ---------------------------------------------------
 * Selector 1..6 maps one-for-one onto three parallel arrays:
 *     txFrameAmp[sel - 1]              what the car is told
 *     vehicle[kAmpLevel + sel - 1]     what the head unit reads back
 *     param[kVolume + sel - 1]         what survives a power cycle
 * The stock image is a six-arm switch writing the three addresses literally;
 * the arithmetic form below generates the same stores. Selector 0 and 7..255
 * write nothing at all -- and still trigger the transmit in the caller, which
 * is also stock. */
void applyPendingValue(std::uint8_t sel, std::uint8_t value) noexcept
{
    if ((sel < kSelVolume) || (sel > kSelLast)) {
        return;
    }

    ram::AppRam&      r      = ram::app();
    const std::size_t offset = static_cast<std::size_t>(sel - kSelVolume);

    r.txFrameAmp[offset] = value;
    /* offset 0 is ram::VehicleState::kAmpLevel itself, i.e. setAmpLevel(). */
    r.vehicle.at(ram::VehicleState::kAmpLevel + offset) = value;
    r.param[svc::params::kVolume + offset] = value;
}

}  // namespace

/* ===========================================================================
 * primeTxTemplates -- rebuild every outbound CAN frame template for the
 * profile that is currently latched.                        0x08007B6C part
 *
 * This used to live inside svc::params::validateDefaults(), which is where the
 * stock image put it. It is vehicle protocol, not parameter handling: it writes
 * the SWC 0x416 reply, the amplifier frame, the vehicle block's volume/tone
 * bytes and all five periodic frame templates. Having it behind a name like
 * "validateDefaults" hid a sharp edge -- it is called at boot AND at the
 * phase-1 exit, and a third call (the obvious shape of a runtime profile
 * switch) would stamp over the live cluster-text frames that
 * clusterTextToCan() maintains in txFrame190 / txFrame18c / txFrame9f.
 *
 * Call it immediately after validateDefaults(), which is what latches
 * profileActive. Order matters: everything here reads that latch.
 * ========================================================================= */
void primeTxTemplates() noexcept
{
    ram::AppRam&                 r      = ram::app();
    volatile std::uint8_t* const live   = r.param;
    const std::uint8_t           active = r.profileActive;

    /* CAN 0x416 payload -- constant: FE 16 3F FF FF FF FF FF */
    r.swcTxBuf[0] = 0xFEU;
    r.swcTxBuf[1] = 0x16U;
    r.swcTxBuf[2] = 0x3FU;
    r.swcTxBuf[3] = 0xFFU;
    r.swcTxBuf[4] = 0xFFU;
    r.swcTxBuf[5] = 0xFFU;
    r.swcTxBuf[6] = 0xFFU;
    r.swcTxBuf[7] = 0xFFU;

    /* Stored tone values -> the amplifier CAN frame and the vehicle block.
     * The stock loop runs i = 1..5 over g_tx_frame_amp[i] and g_vstate_mid[i],
     * reading param[0x10 + i]; g_vstate_mid is the vehicle block + 0x17, so
     * g_vstate_mid[1..5] is VehicleState::kTone..kTone+4 and the source bytes
     * are param[svc::params::kToneFirst..svc::params::kToneFirst+4] -- BAL FAD BASS MID TRE. */
    for (std::size_t i = 1U; i <= svc::params::kToneCount; ++i) {
        const std::uint8_t v = live[svc::params::kVolume + i];
        r.txFrameAmp[i] = v;
        r.vehicle.at(ram::VehicleState::kTone + i - 1U) = v;
    }
    /* g_vstate_mid[0] is VehicleState::kVolume @0x20001b60. */
    r.vehicle.at(ram::VehicleState::kVolume) = live[svc::params::kVolume];

    /* The same five values are aliased a second time further into the block,
     * at g_vstate_mid[0x16..0x1A] = VehicleState::kAmpTone..+4 -- this is the
     * documented "0A 0A 0D 0A 0A tail" quirk visible in UART cmd 0x12. Stock
     * re-reads them out of g_tx_frame_amp rather than out of param, which is
     * the same bytes by the loop above; kept in that form so the data flow
     * still matches the decompile. */
    r.vehicle.at(ram::VehicleState::kAmpTone + 0U) = r.txFrameAmp[1];
    r.vehicle.at(ram::VehicleState::kAmpTone + 1U) = r.txFrameAmp[2];
    r.vehicle.at(ram::VehicleState::kAmpTone + 2U) = r.txFrameAmp[3];
    r.vehicle.at(ram::VehicleState::kAmpTone + 3U) = r.txFrameAmp[4];
    r.vehicle.at(ram::VehicleState::kAmpTone + 4U) = r.txFrameAmp[5];

    /* g_vstate_mid[0x15] is g_amp_level @0x20001b75, byte 0 of the UART 0xA6
     * window: 30 on profile 1, 20 on profile 0. */
    r.vehicle.setAmpLevel(static_cast<std::uint8_t>((active == 1U) ? 0x1EU : 0x14U));

    /* --- periodic CAN TX frame templates, profile dependent --------------- */
    r.txFrameF0[0] = 0U;
    r.txFrameF0[1] = 0U;
    r.txFrameF0[2] = 0U;
    r.txFrameF0[3] = 0U;

    if (active == 1U) {
        r.txFrame326[0] = 0U;
        r.txFrame326[1] = 0U;
        r.txFrame326[2] = 0U;
        r.txFrame326[3] = 0U;
    } else {
        r.txFrame326[0] = 0U;
        r.txFrame326[1] = 7U;
        r.txFrame326[2] = 0x0FU;
        r.txFrame326[3] = 7U;
    }
    /* [4..6] are zeroed on both paths in the decompile; hoisted out of the
     * branch here because the two branches wrote identical values. */
    r.txFrame326[4] = 0U;
    r.txFrame326[5] = 0U;
    r.txFrame326[6] = 0U;

    r.txFrame190[0] = 1U;
    r.txFrame190[1] = 0x0DU;
    r.txFrame190[2] = 0x65U;
    r.txFrame190[3] = 0U;
    r.txFrame190[4] = 0x10U;
    r.txFrame190[5] = 0U;
    r.txFrame190[6] = 7U;

    r.txFrame18c[0] = 0x50U;
    r.txFrame18c[1] = 0U;
    r.txFrame18c[2] = 0U;
    r.txFrame18c[3] = 0x20U;
    r.txFrame18c[4] = 0U;
    r.txFrame18c[5] = 0U;
    r.txFrame18c[6] = 0U;
    r.txFrame18c[7] = 0U;

    r.txFrame9f[0] = 1U;
    r.txFrame9f[1] = 0x23U;
    r.txFrame9f[2] = 0x6BU;
    r.txFrame9f[3] = 0U;
    r.txFrame9f[4] = 0U;
    r.txFrame9f[5] = 0U;
    r.txFrame9f[6] = 0U;
    r.txFrame9f[7] = 0x1FU;
}

/* ===========================================================================
 * 0x08008110 -- can_periodic_tx_scheduler
 * ========================================================================= */
/* 0x08008110 */
void periodicTxScheduler() noexcept
{
    ram::AppRam&       r  = ram::app();
    ram::VehicleState& vs = r.vehicle;

    const std::uint8_t acc = vs.accOn() ? 1U : 0U;

    /* --- 1. ACC edge detect ------------------------------------------------
     * The shadow byte holds 0 or 1, not a copy of the whole flags byte. On a
     * change the 0x09F frame is primed at once rather than at the next lap. */
    if ((vs.accShadow() & 1U) != acc) {
        r.txFrame9f[0]    = (acc != 0U) ? kFrame9fAccOn : kFrame9fAccOff;
        vs.setAccShadow(acc);
    }

    /* --- 2. post-ACC hold timer ------------------------------------------ */
    if (acc == 0U) {
        if (r.canTxHoldTimer != 0U) {
            r.canTxHoldTimer = r.canTxHoldTimer - 1U;
        }
    } else {
        r.canTxHoldTimer = kTxHoldReload;
    }

    if (r.canTxHoldTimer == 0U) {
        /* ACC has been off for 3.2 s: stop talking to the car entirely. The
         * phase counter is NOT advanced here, so the table resumes where it
         * stopped rather than from phase 0. */
        return;
    }

    /* --- 3. phase table --------------------------------------------------- */

    /* Republish the latched profile in bit 7 of the flags byte for the head
     * unit. The low seven bits are the decoded vehicle flags and are kept. */
    vs.at(ram::VehicleState::kFlags) = static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(vs.at(ram::VehicleState::kFlags)
                                  & static_cast<std::uint8_t>(~ram::VehicleState::kFlagProfileSel))
        | static_cast<std::uint8_t>(r.profileActive << 7));
    /* ONE BIT. kFlagProfileSel is bit 7, so profileActive must be 0 or 1:
     * 2 << 7 is 0x100 and truncates to 0 in the uint8_t store, i.e. profile 2
     * would tell the head unit it is profile 0. */
    static_assert(cfg::kProfileCount <= 2U,
                  "the profile is published to the head unit in a single bit "
                  "(ram::VehicleState::kFlagProfileSel); a third profile is not "
                  "representable on the wire without a protocol change");

    if (r.periodicTxPhase < kPhaseMax) {
        r.periodicTxPhase = static_cast<std::uint8_t>(r.periodicTxPhase + 1U);
    } else {
        r.periodicTxPhase = 0U;
    }

    /* The phase is advanced BEFORE the table is consulted, so the first call
     * after a reset transmits on phase 1, not phase 0. */
    emitPhaseFrame(r.profileActive, r.periodicTxPhase);

    /* --- 4a. pending cluster text ----------------------------------------- *
     * Note this is gated by the hold timer too: with ACC long gone a 0xD2 from
     * the head unit stays pending and is never consumed. Stock. */
    if (r.hostCmd.clusterNewData == kCmdPending) {
        r.hostCmd.clusterNewData = 0U;
        clusterTextToCan();                       /* 0x08007DA8 */
    }

    /* --- 4b. pending tone / volume change --------------------------------- */
    if (r.hostCmd.periodicNewData != kCmdPending) {
        return;
    }
    r.hostCmd.periodicNewData = 0U;

    clampPendingValue();

    const std::uint8_t sel   = r.hostCmd.periodicTxCfg[0];
    const std::uint8_t value = r.hostCmd.periodicTxCfg[1];
    applyPendingValue(sel, value);

    /* Transmitted immediately rather than waiting for phase 1 to come round --
     * a volume change has to be audible now. */
    svc::can::txEnqueue((r.profileActive == cfg::kProfileFca125k) ? kIdAmpProfile1
                                                                 : kIdAmpProfile0,
                        kIdeStandard, kDlcAmp, r.txFrameAmp);
}

}  // namespace logic
