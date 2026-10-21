/* ============================================================================
 * test_can_decode.cpp -- logic::can_decode, the two car profiles.
 *
 * Written from the BEHAVIOURAL SPEC in reference/decompile/app/can_rx_decode.c (the Ghidra
 * decompile) and the commentary in the retired app/port/can_decode.c, NOT from
 * can_decode.cpp. Every identifier, bit position, scale and clamp below is
 * restated here independently; a test that merely echoed the implementation
 * would prove nothing (docs/ARCHITECTURE.md section 7).
 *
 * This is the module where tests pay for themselves: every one of these
 * decoders turns a real frame from a real car into something the head unit
 * displays, and a wrong bit shows a passenger door as a tailgate.
 *
 * Stock functions covered, by original flash address:
 *     0x08007D44 can_profile_dispatch_gate
 *     0x08009010 can_dispatch_profile0_83k   (Jeep WK, 83.333 kbit/s)
 *     0x08009680 can_dispatch_profile1_125k  (FCA variant, 125 kbit/s)
 *
 * Frames are injected by writing the RX ring directly (pushFrame below), the
 * way the CAN_RX0 interrupt does; replies are captured by draining the TX ring
 * into the fake mailbox and reading fake::canFrames().
 * ==========================================================================*/

#include "test_framework.hpp"

#include "can_bus.hpp"
#include "can_decode.hpp"
#include "fake_hal.hpp"
#include "ram_map.hpp"

namespace {

/* ---- the wire, restated from the decompile ------------------------------ */

/* Ring geometry: 50 entries x 16 bytes, id as two little-endian 16-bit halves
 * at +0 and +2, IDE at +4, DLC at +5, payload at +6. */
constexpr std::size_t kStride  = 0x10U;
constexpr std::size_t kEntries = 0x32U;
constexpr std::size_t kIdHi    = 0U;
constexpr std::size_t kIdLo    = 2U;
constexpr std::size_t kIde     = 4U;
constexpr std::size_t kDlc     = 5U;
constexpr std::size_t kData    = 6U;

/* Identifiers, profile 0 (Jeep WK, 83.333 kbit/s). */
constexpr std::uint32_t kJeepIgn    = 0x000U;
constexpr std::uint32_t kJeepRev    = 0x003U;
constexpr std::uint32_t kJeepDoors  = 0x013U;
constexpr std::uint32_t kJeepSteer  = 0x015U;
constexpr std::uint32_t kJeepIllum  = 0x210U;
constexpr std::uint32_t kJeepSwc    = 0x3A0U;
constexpr std::uint32_t kJeepEcho   = 0x500U;

/* Identifiers, profile 1 (FCA, 125 kbit/s). */
constexpr std::uint32_t kFcaSteer   = 0x1E1U;
constexpr std::uint32_t kFcaIgn     = 0x20BU;
constexpr std::uint32_t kFcaGear    = 0x20EU;
constexpr std::uint32_t kFcaRpm     = 0x211U;
constexpr std::uint32_t kFcaDoors   = 0x244U;
constexpr std::uint32_t kFcaIllum   = 0x308U;
constexpr std::uint32_t kFcaSwc     = 0x3A3U;
constexpr std::uint32_t kFcaEcho    = 0x513U;

/* Shared by both profiles. */
constexpr std::uint32_t kSwcService = 0x400U;   /* .. 0x43F */
constexpr std::uint32_t kVendor7fc  = 0x7FCU;
constexpr std::uint32_t kVendor7ff  = 0x7FFU;
constexpr std::uint32_t kTx416      = 0x416U;
constexpr std::uint32_t kTx516      = 0x516U;
constexpr std::uint32_t kTx7fd      = 0x7FDU;
constexpr std::uint32_t kTx7fe      = 0x7FEU;

/* State-block bits, restated. */
constexpr std::uint8_t kFlagAcc     = 0x01U;
constexpr std::uint8_t kFlagIllum   = 0x02U;
constexpr std::uint8_t kFlagReverse = 0x04U;
constexpr std::uint8_t kFlagSpare3  = 0x08U;
constexpr std::uint8_t kFlagIgnRaw  = 0x10U;

/* Door destination bits (state block byte 0x15). */
constexpr std::uint8_t kDstLeftFront  = 0x80U;
constexpr std::uint8_t kDstRightFront = 0x40U;
constexpr std::uint8_t kDstLeftRear   = 0x20U;
constexpr std::uint8_t kDstRightRear  = 0x10U;
constexpr std::uint8_t kDstTailgate   = 0x08U;

/* ---- the injector ------------------------------------------------------- */

/* Write one frame into the RX ring exactly as the CAN_RX0 ISR does and bump
 * the head, so the decoder sees it. `data` is always 8 bytes: the decoders
 * read d[6]/d[7] regardless of DLC, which is what the stock code does. */
void pushFrame(std::uint32_t id, std::uint8_t dlc, const std::uint8_t* data) noexcept
{
    ram::AppRam& r = ram::app();
    volatile std::uint8_t* e = r.canRxRing
                             + static_cast<std::size_t>(r.canRing.rxHead) * kStride;

    e[kIdHi]      = static_cast<std::uint8_t>(id >> 16);
    e[kIdHi + 1U] = static_cast<std::uint8_t>(id >> 24);
    e[kIdLo]      = static_cast<std::uint8_t>(id);
    e[kIdLo + 1U] = static_cast<std::uint8_t>(id >> 8);
    e[kIde]       = 0U;
    e[kDlc]       = dlc;
    for (std::size_t i = 0U; i < 8U; ++i) {
        e[kData + i] = (data != nullptr) ? data[i] : 0U;
    }

    r.canRing.rxHead = (r.canRing.rxHead + 1U) % static_cast<std::uint32_t>(kEntries);
}

/* One-byte-at-a-time convenience for the many single-signal frames. */
void pushByteAt(std::uint32_t id, std::size_t index, std::uint8_t value) noexcept
{
    std::uint8_t d[8] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    d[index] = value;
    pushFrame(id, 8U, d);
}

std::uint8_t flags() noexcept { return ram::app().vehicle.flags(); }

/* Replies are only visible once the TX ring has been pumped into the mailbox. */
void drainTx() noexcept { svc::can::txDrain(); }

}  // namespace

/* ===========================================================================
 * Ignition / ACC -- the flag that gates everything else
 * ======================================================================== */

/* Profile 0, CAN 0x000 d[0]: bit 4 of the state block <- d[0] bit 0, and the
 * ACC bit <- (d[0] & 0xE0) > 0x10, i.e. any of d[0] bits 5..7. */
TEST(jeep_ignition_decodes_acc_and_the_raw_bit)
{
    fake::reset();

    pushByteAt(kJeepIgn, 0U, 0x20U);
    logic::decodeProfileJeep();
    CHECK_EQ(flags() & kFlagAcc, kFlagAcc);
    CHECK_EQ(flags() & kFlagIgnRaw, 0U);

    pushByteAt(kJeepIgn, 0U, 0x01U);
    logic::decodeProfileJeep();
    CHECK_EQ(flags() & kFlagAcc, 0U);            /* bits 5..7 all clear */
    CHECK_EQ(flags() & kFlagIgnRaw, kFlagIgnRaw);
}

/* 0x10 is below the threshold AND outside the 0xE0 mask: it must not look like
 * ignition. This is the case a naive "(v & 0xF0) != 0" would get wrong. */
TEST(jeep_ignition_ignores_bit4_of_the_source_byte)
{
    fake::reset();

    pushByteAt(kJeepIgn, 0U, 0x10U);
    logic::decodeProfileJeep();
    CHECK_EQ(flags() & kFlagAcc, 0U);
}

/* Profile 1 carries the same signal on 0x20B -- and must NOT react to 0x000. */
TEST(fca_ignition_decodes_acc_on_its_own_id)
{
    fake::reset();

    pushByteAt(kJeepIgn, 0U, 0xE0U);   /* profile 0 id: ignored here */
    logic::decodeProfileFca();
    CHECK_EQ(flags() & kFlagAcc, 0U);

    pushByteAt(kFcaIgn, 0U, 0xE0U);
    logic::decodeProfileFca();
    CHECK_EQ(flags() & kFlagAcc, kFlagAcc);
}

/* ===========================================================================
 * Doors -- the permutation that is not the identity
 * ======================================================================== */

namespace {

struct DoorCase {
    std::uint8_t source;
    std::uint8_t destination;
    const char*  what;
};

/* source bit 0 -> bit 7, 1 -> 6, 3 -> 5, 2 -> 4, 4 -> 3. Note 2 and 3 CROSS
 * OVER: left rear comes from source bit 3, right rear from source bit 2. */
const DoorCase kDoorCases[5] = {
    {0x01U, kDstLeftFront,  "left front"},
    {0x02U, kDstRightFront, "right front"},
    {0x08U, kDstLeftRear,   "left rear"},
    {0x04U, kDstRightRear,  "right rear"},
    {0x10U, kDstTailgate,   "tailgate"},
};

}  // namespace

/* Profile 0 feeds the door bitfield from 0x013 d[2]. */
TEST(jeep_doors_map_every_source_bit_to_the_right_destination_bit)
{
    for (std::size_t i = 0U; i < 5U; ++i) {
        fake::reset();
        pushByteAt(kJeepDoors, 2U, kDoorCases[i].source);
        logic::decodeProfileJeep();
        CHECK_EQ(ram::app().vehicle.doors(), kDoorCases[i].destination);
    }

    /* All five at once, and nothing else. */
    fake::reset();
    pushByteAt(kJeepDoors, 2U, 0x1FU);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.doors(), 0xF8U);
}

/* Profile 1 feeds the SAME bitfield from a different frame and a different
 * byte: 0x244 d[0]. */
TEST(fca_doors_map_every_source_bit_to_the_right_destination_bit)
{
    for (std::size_t i = 0U; i < 5U; ++i) {
        fake::reset();
        pushByteAt(kFcaDoors, 0U, kDoorCases[i].source);
        logic::decodeProfileFca();
        CHECK_EQ(ram::app().vehicle.doors(), kDoorCases[i].destination);
    }

    fake::reset();
    pushByteAt(kFcaDoors, 0U, 0x1FU);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.doors(), 0xF8U);
}

/* A closed door clears its bit again. */
TEST(doors_clear_when_the_source_bit_goes_away)
{
    fake::reset();

    pushByteAt(kJeepDoors, 2U, 0x1FU);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.doors(), 0xF8U);

    pushByteAt(kJeepDoors, 2U, 0x00U);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.doors(), 0x00U);
}

/* ===========================================================================
 * Steering-wheel buttons
 * ======================================================================== */

namespace {

struct KeyCase {
    std::uint8_t d0;
    std::uint8_t d1;
    std::uint8_t key;
};

/* The key codes the Android plugin (car_key_3009001.json) expects. */
const KeyCase kKeyCases[9] = {
    {0x01U, 0x00U, 0x0BU},   /* MODE / source    */
    {0x02U, 0x00U, 0x01U},   /* volume +         */
    {0x04U, 0x00U, 0x02U},   /* volume -         */
    {0x08U, 0x00U, 0x08U},   /* next / seek up   */
    {0x10U, 0x00U, 0x09U},   /* prev / seek down */
    {0x20U, 0x00U, 0x03U},   /* mute             */
    {0x40U, 0x00U, 0x12U},   /* unmapped group   */
    {0x00U, 0x01U, 0x05U},   /* answer / phone   */
    {0x00U, 0x02U, 0x07U},   /* voice assistant  */
};

}  // namespace

TEST(swc_buttons_report_the_plugin_key_code_for_every_group)
{
    for (std::size_t i = 0U; i < 9U; ++i) {
        fake::reset();
        std::uint8_t d[8] = {kKeyCases[i].d0, kKeyCases[i].d1,
                             0U, 0U, 0U, 0U, 0U, 0U};
        pushFrame(kJeepSwc, 8U, d);
        logic::decodeProfileJeep();
        CHECK_EQ(ram::app().vehicle.swcKey(), kKeyCases[i].key);
    }
}

/* Profile 1 carries the identical table on 0x3A3. */
TEST(fca_swc_buttons_use_their_own_id)
{
    fake::reset();

    std::uint8_t d[8] = {0x20U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    pushFrame(kJeepSwc, 8U, d);          /* profile 0 id: ignored here */
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.swcKey(), 0x00U);

    pushFrame(kFcaSwc, 8U, d);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.swcKey(), 0x03U);
}

/* An all-zero frame is "key released" and clears the slot. */
TEST(swc_buttons_all_zero_frame_clears_the_key)
{
    fake::reset();

    pushByteAt(kJeepSwc, 0U, 0x01U);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.swcKey(), 0x0BU);

    pushFrame(kJeepSwc, 8U, nullptr);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.swcKey(), 0x00U);
}

/* The groups are tested by INDEPENDENT ifs, not a chain, so a multi-bit frame
 * reports the LAST matching group. 0x23 matches MODE (0x0B), then volume +
 * (0x01), then mute (0x03) -- mute wins because it is tested last. */
TEST(swc_buttons_last_match_overwrites_earlier_ones)
{
    fake::reset();

    pushByteAt(kJeepSwc, 0U, 0x23U);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.swcKey(), 0x03U);

    /* d[1] is tested after d[0], so the phone/voice group wins over all of it,
     * and voice (tested last of all) wins over phone. */
    std::uint8_t d[8] = {0x23U, 0x03U, 0U, 0U, 0U, 0U, 0U, 0U};
    pushFrame(kJeepSwc, 8U, d);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.swcKey(), 0x07U);
}

/* ===========================================================================
 * Illumination / dimmer
 * ======================================================================== */

/* Profile 0: 0x210 d[1]. The flag follows "non-zero"; the percent is v >> 1. */
TEST(illum_flag_follows_nonzero_and_percent_is_half_the_raw)
{
    fake::reset();

    pushByteAt(kJeepIllum, 1U, 0x50U);
    logic::decodeProfileJeep();
    CHECK_EQ(flags() & kFlagIllum, kFlagIllum);
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kIllumLevel), 0x28U);

    pushByteAt(kJeepIllum, 1U, 0x00U);
    logic::decodeProfileJeep();
    CHECK_EQ(flags() & kFlagIllum, 0U);
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kIllumLevel), 0U);
}

/* 0xFF >> 1 is 127, which is clamped to 100. */
TEST(illum_percent_clamps_at_100)
{
    fake::reset();

    pushByteAt(kJeepIllum, 1U, 0xFFU);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kIllumLevel), 100U);

    /* 200 >> 1 == 100 exactly: the boundary is not clamped away. */
    pushByteAt(kJeepIllum, 1U, 200U);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kIllumLevel), 100U);
}

/* Profile 1 carries the same signal on 0x308. */
TEST(fca_illum_uses_its_own_id)
{
    fake::reset();

    pushByteAt(kFcaIllum, 1U, 0x50U);
    logic::decodeProfileFca();
    CHECK_EQ(flags() & kFlagIllum, kFlagIllum);
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kIllumLevel), 0x28U);
}

/* ===========================================================================
 * Reverse gear (profile 0) and the steering angle it gates
 * ======================================================================== */

/* 0x003 d[2] low nibble: 1 and 9 both mean reverse. */
TEST(jeep_reverse_nibble_sets_and_clears_the_flag)
{
    fake::reset();

    pushByteAt(kJeepRev, 2U, 0x01U);
    logic::decodeProfileJeep();
    CHECK_EQ(flags() & kFlagReverse, kFlagReverse);

    pushByteAt(kJeepRev, 2U, 0x02U);
    logic::decodeProfileJeep();
    CHECK_EQ(flags() & kFlagReverse, 0U);

    pushByteAt(kJeepRev, 2U, 0xA9U);   /* high nibble is ignored */
    logic::decodeProfileJeep();
    CHECK_EQ(flags() & kFlagReverse, kFlagReverse);
}

namespace {

/* 0x015 d[4]:d[5], big endian. */
void pushJeepSteer(std::uint16_t raw) noexcept
{
    std::uint8_t d[8] = {0U, 0U, 0U, 0U,
                         static_cast<std::uint8_t>(raw >> 8),
                         static_cast<std::uint8_t>(raw & 0xFFU), 0U, 0U};
    pushFrame(kJeepSteer, 8U, d);
}

}  // namespace

/* Centre 0x1000, 0.5 deg/LSB, NO clamp on this profile, and raw == 0 reports 0
 * instead of going through the centring arithmetic. */
TEST(jeep_steering_centre_both_directions_and_the_zero_special_case)
{
    fake::reset();
    ram::app().vehicle.setFlag(kFlagReverse, true);

    pushJeepSteer(0x1000U);                       /* dead centre */
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0x0000U);

    pushJeepSteer(0x0F00U);                       /* left of centre */
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0x0080U);

    pushJeepSteer(0x1100U);                       /* right of centre */
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0xFF80U);

    pushJeepSteer(0x0000U);                       /* raw 0 -> reported 0 */
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0x0000U);

    /* No clamp: full lock left is reported in full. */
    pushJeepSteer(0x0001U);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0x07FFU);
}

/* The angle exists to draw the reverse-camera trajectory, so it is only
 * updated while the reverse flag is set. */
TEST(jeep_steering_writes_nothing_while_not_in_reverse)
{
    fake::reset();
    ram::app().vehicle.setFlag(kFlagReverse, true);
    pushJeepSteer(0x0F00U);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0x0080U);

    ram::app().vehicle.setFlag(kFlagReverse, false);
    pushJeepSteer(0x1100U);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0x0080U);   /* unchanged */
}

/* ===========================================================================
 * Profile 1 steering -- same centre and scale, clamped, odd negative encoding
 * ======================================================================== */

namespace {

/* 0x1E1 d[2]:d[3], big endian. */
void pushFcaSteer(std::uint16_t raw) noexcept
{
    std::uint8_t d[8] = {0U, 0U,
                         static_cast<std::uint8_t>(raw >> 8),
                         static_cast<std::uint8_t>(raw & 0xFFU),
                         0U, 0U, 0U, 0U};
    pushFrame(kFcaSteer, 8U, d);
}

}  // namespace

/* Clamped to +-539 (0x21B). The NEGATIVE side clamps to 0x21C, not 0x21B, and
 * the negative half is encoded as 0x10001 - v (0x10001, not 0x10000), so the
 * clamped negative value reports as 0x10001 - 0x21C = 0xFDE5 = -539. */
TEST(fca_steering_clamps_both_sides_at_0x21b)
{
    fake::reset();
    ram::app().vehicle.setFlag(kFlagReverse, true);

    pushFcaSteer(0x0000U);               /* far left: (0x1000 - 0) >> 1 = 0x800 */
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0x021BU);

    pushFcaSteer(0x2000U);               /* far right: 0x800 -> 0x21C -> encode */
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0xFDE5U);

    /* Just inside the clamp on the left: 0x1000 - 0x0BCA = 0x436, >> 1 = 0x21B. */
    pushFcaSteer(0x0BCAU);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0x021BU);
}

/* The negative half is 0x10001 - v, and only the low 16 bits are stored, so a
 * raw value one LSB right of centre reports 1, not 0. */
TEST(fca_steering_negative_half_uses_the_0x10001_encoding)
{
    fake::reset();
    ram::app().vehicle.setFlag(kFlagReverse, true);

    pushFcaSteer(0x1000U);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0x0000U);   /* dead centre */

    pushFcaSteer(0x1001U);               /* (1 >> 1) == 0 -> 0x10001 -> 0x0001 */
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0x0001U);

    pushFcaSteer(0x1100U);               /* 0x80 -> 0x10001 - 0x80 = 0xFF81 */
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0xFF81U);
}

TEST(fca_steering_writes_nothing_while_not_in_reverse)
{
    fake::reset();

    pushFcaSteer(0x0F00U);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.steeringAngle(), 0x0000U);
}

/* ===========================================================================
 * Profile 1 gear and engine speed
 * ======================================================================== */

/* 0x20E d[2] low nibble: 0 -> P(1), 2 -> N(2), 4 -> D(4). */
TEST(fca_gear_maps_the_prndl_nibble)
{
    fake::reset();

    pushByteAt(kFcaGear, 2U, 0x00U);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.gear(), 1U);         /* P */

    pushByteAt(kFcaGear, 2U, 0x02U);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.gear(), 2U);         /* N */

    pushByteAt(kFcaGear, 2U, 0x04U);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.gear(), 4U);         /* D */

    pushByteAt(kFcaGear, 2U, 0x05U);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.gear(), 0U);         /* unknown nibble */
}

/* Nibble 9 is the "leave the gear alone" code in the PRNDL chain -- but 9 also
 * has bit 3 set, and bit 3 is the reverse flag, which is tested immediately
 * afterwards and forces R. So the hold branch is real in the chain and
 * unobservable in the result: nibble 9 always ends up reporting R.
 *
 * The visible half of the hold is that no OTHER value is written on the way
 * through: the gear goes from D straight to R, never through 0. */
TEST(fca_gear_nibble_9_holds_in_the_chain_but_bit3_still_forces_r)
{
    fake::reset();

    pushByteAt(kFcaGear, 2U, 0x04U);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.gear(), 4U);

    pushByteAt(kFcaGear, 2U, 0x09U);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.gear(), 3U);         /* R, via bit 3 */
    CHECK_EQ(flags() & kFlagReverse, kFlagReverse);

    /* Every nibble with bit 3 set means reverse, not just 8 and 9. */
    pushByteAt(kFcaGear, 2U, 0x0CU);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.gear(), 3U);
}

/* d[2] bit 3 is reverse. It sets the reverse flag AND forces the gear to R(3)
 * whatever the nibble said -- here the nibble (8) is not even a valid code. */
TEST(fca_gear_reverse_bit_forces_r)
{
    fake::reset();

    pushByteAt(kFcaGear, 2U, 0x08U);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.gear(), 3U);         /* R */
    CHECK_EQ(flags() & kFlagReverse, kFlagReverse);

    /* Reverse released: the flag drops and the nibble takes over again. */
    pushByteAt(kFcaGear, 2U, 0x04U);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.gear(), 4U);
    CHECK_EQ(flags() & kFlagReverse, 0U);
}

/* 0x211 d[0]:d[1], decimated 1-in-21: the counter climbs 0..0x14 and only the
 * pass that finds it already at 0x14 decodes, so 20 of every 21 frames are
 * dropped. */
TEST(fca_rpm_is_decimated_one_frame_in_twenty_one)
{
    fake::reset();

    std::uint8_t d[8] = {0x0AU, 0xF0U, 0U, 0U, 0U, 0U, 0U, 0U};   /* 2800 rpm */

    for (std::size_t i = 0U; i < 20U; ++i) {
        pushFrame(kFcaRpm, 8U, d);
    }
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kRpmHi), 0U);
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kRpmLo), 0U);
    CHECK_EQ(ram::app().rpmDecimator, 0x14U);

    pushFrame(kFcaRpm, 8U, d);          /* the 21st */
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kRpmHi), 0x0AU);
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kRpmLo), 0xF0U);
    CHECK_EQ(ram::app().rpmDecimator, 0U);
}

/* 8000 rpm and above is rejected outright (the counter is still consumed). */
TEST(fca_rpm_rejects_8000_and_above)
{
    fake::reset();

    std::uint8_t tooFast[8] = {0x1FU, 0x40U, 0U, 0U, 0U, 0U, 0U, 0U};  /* 8000 */
    ram::app().rpmDecimator = 0x14U;
    pushFrame(kFcaRpm, 8U, tooFast);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kRpmHi), 0U);
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kRpmLo), 0U);

    std::uint8_t justUnder[8] = {0x1FU, 0x3FU, 0U, 0U, 0U, 0U, 0U, 0U}; /* 7999 */
    ram::app().rpmDecimator = 0x14U;
    pushFrame(kFcaRpm, 8U, justUnder);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kRpmHi), 0x1FU);
    CHECK_EQ(ram::app().vehicle.at(ram::VehicleState::kRpmLo), 0x3FU);
}

/* ===========================================================================
 * The vendor protocol: 0x7FC handshake, 0x7FF injection, 0x500/0x513 echoes
 * ======================================================================== */

namespace {

/* 3F F3 F2 01 .. .. 3A A3 */
const std::uint8_t kMagic7fc[8] =
    {0x3FU, 0xF3U, 0xF2U, 0x01U, 0x11U, 0x22U, 0x3AU, 0xA3U};

/* 7F F7 x x y y 3A A3, with d[2] == d[3] and d[4] == d[5]. */
void make7ff(std::uint8_t (&d)[8], std::uint8_t bits, std::uint8_t mode) noexcept
{
    d[0] = 0x7FU; d[1] = 0xF7U;
    d[2] = bits;  d[3] = bits;
    d[4] = mode;  d[5] = mode;
    d[6] = 0x3AU; d[7] = 0xA3U;
}

}  // namespace

/* The 0x7FC handshake echoes the whole received payload back as 0x7FD, and
 * ONLY on the exact magic. */
TEST(vendor_7fc_echoes_as_7fd_only_on_the_exact_magic)
{
    fake::reset();

    pushFrame(kVendor7fc, 8U, kMagic7fc);
    logic::decodeProfileJeep();
    drainTx();

    CHECK_EQ(fake::canFrameCount(), 1U);
    const fake::CanFrame* f = fake::lastCanFrame();
    CHECK(f != nullptr);
    if (f != nullptr) {
        CHECK_EQ(f->id, kTx7fd);
        CHECK_EQ(f->dlc, 8U);
        CHECK_BYTES(f->data, kMagic7fc, 8U);
    }

    /* One byte of the trailer wrong -> silence. */
    fake::reset();
    std::uint8_t bad[8];
    for (std::size_t i = 0U; i < 8U; ++i) { bad[i] = kMagic7fc[i]; }
    bad[7] = 0xA4U;
    pushFrame(kVendor7fc, 8U, bad);
    logic::decodeProfileJeep();
    drainTx();
    CHECK_EQ(fake::canFrameCount(), 0U);
}

/* Profile 1 carries the identical handshake. */
TEST(fca_vendor_7fc_echoes_as_7fd_too)
{
    fake::reset();

    pushFrame(kVendor7fc, 8U, kMagic7fc);
    logic::decodeProfileFca();
    drainTx();

    CHECK_EQ(fake::canFrameCount(), 1U);
    CHECK_EQ(fake::canFrames()[0].id, kTx7fd);
}

/* 0x7FF writes the four low state bits from d[2] -- but only when the whole
 * magic matches, including d[2]==d[3] and d[4]==d[5]. */
TEST(vendor_7ff_writes_the_four_status_bits_only_on_the_exact_magic)
{
    fake::reset();

    constexpr std::uint8_t kLowFour =
        static_cast<std::uint8_t>(kFlagAcc | kFlagIllum | kFlagReverse | kFlagSpare3);

    std::uint8_t good[8];
    make7ff(good, kLowFour, 0x00U);
    pushFrame(kVendor7ff, 8U, good);
    logic::decodeProfileJeep();
    CHECK_EQ(flags() & kLowFour, kLowFour);

    /* Only bits 0..3 are touched. */
    fake::reset();
    ram::app().vehicle.setFlag(kFlagIgnRaw, true);
    make7ff(good, 0x05U, 0x00U);
    pushFrame(kVendor7ff, 8U, good);
    logic::decodeProfileJeep();
    CHECK_EQ(flags(), static_cast<std::uint8_t>(kFlagIgnRaw | kFlagAcc | kFlagReverse));

    /* d[2] != d[3]: not the magic, nothing is written. */
    fake::reset();
    std::uint8_t mismatched[8];
    make7ff(mismatched, 0x0FU, 0x00U);
    mismatched[3] = 0x0EU;
    pushFrame(kVendor7ff, 8U, mismatched);
    logic::decodeProfileJeep();
    CHECK_EQ(flags(), 0x00U);

    /* d[4] != d[5]: likewise. */
    fake::reset();
    make7ff(mismatched, 0x0FU, 0x00U);
    mismatched[5] = 0x01U;
    pushFrame(kVendor7ff, 8U, mismatched);
    logic::decodeProfileJeep();
    CHECK_EQ(flags(), 0x00U);
}

/* Profile 0 sends NOTHING back on 0x7FF; only profile 1 answers. */
TEST(jeep_vendor_7ff_does_not_reply)
{
    fake::reset();

    std::uint8_t d[8];
    make7ff(d, 0x0FU, 0x00U);
    pushFrame(kVendor7ff, 8U, d);
    logic::decodeProfileJeep();
    drainTx();
    CHECK_EQ(fake::canFrameCount(), 0U);
}

/* Profile 1 answers 0x7FF with a one-byte 0x7FE carrying the head unit display
 * mode, and maps d[4] into the SWC KEY slot (not the gear slot). */
TEST(fca_vendor_7ff_replies_on_7fe_and_maps_d4_into_the_key_slot)
{
    fake::reset();

    std::uint8_t d[8];
    make7ff(d, 0x00U, 0x03U);              /* d[4] == 3 -> key 0x09 */
    ram::app().hostCmd.cluster[0] = 1U;    /* display mode 1 */
    pushFrame(kVendor7ff, 8U, d);
    logic::decodeProfileFca();
    drainTx();

    CHECK_EQ(ram::app().vehicle.swcKey(), 0x09U);
    CHECK_EQ(fake::canFrameCount(), 1U);
    const fake::CanFrame* f = fake::lastCanFrame();
    CHECK(f != nullptr);
    if (f != nullptr) {
        CHECK_EQ(f->id, kTx7fe);
        CHECK_EQ(f->dlc, 1U);
        CHECK_EQ(f->data[0], 0x01U);
    }
}

/* THE COPY-PASTE SLIP, reproduced deliberately: the stock image tests display
 * mode 4 against BIT 2 (0x04 / 0xFB) again instead of bit 3. Because that last
 * clause also CLEARS bit 2 when the mode is not 4, the net effect is that mode
 * 4 occupies the bit that was meant for mode 3, and mode 3 reports 0x00 --
 * indistinguishable from "no mode". Do not "fix" this. */
TEST(fca_vendor_7ff_display_mode_4_reuses_mode_3_bit)
{
    const std::uint8_t kModes[5]    = {0U, 1U, 2U, 3U, 4U};
    const std::uint8_t kExpected[5] = {0x00U, 0x01U, 0x02U, 0x00U, 0x04U};

    for (std::size_t i = 0U; i < 5U; ++i) {
        fake::reset();
        std::uint8_t d[8];
        make7ff(d, 0x00U, 0x00U);
        ram::app().hostCmd.cluster[0] = kModes[i];
        pushFrame(kVendor7ff, 8U, d);
        logic::decodeProfileFca();
        drainTx();

        const fake::CanFrame* f = fake::lastCanFrame();
        CHECK(f != nullptr);
        if (f != nullptr) {
            CHECK_EQ(f->data[0], kExpected[i]);
        }
    }
}

/* 0x500 d[0]:d[1] is retransmitted as 0x516, zero-padded to 8 bytes -- in a
 * BENZ build. In a jeep build that mirror is switched OFF, because 0x500 is the
 * WCM's KWP response id and a jeep image polls the WCM for tyre pressures
 * (docs/TPMS-POLL-SPEC.md). Both halves are asserted here rather than split
 * across two files, because the point of the test is that the two builds
 * deliberately differ on exactly this frame.
 *
 * Why turning it off is safe: 0x500 appears in ZERO of the 221 853 frames of
 * testlog/drive.log, so the stock mirror has never fired on this car. */
TEST(jeep_echo_500_mirrors_to_516_only_in_a_benz_build)
{
    fake::reset();

    std::uint8_t d[8] = {0xAAU, 0xBBU, 0xCCU, 0xDDU, 0U, 0U, 0U, 0U};
    pushFrame(kJeepEcho, 8U, d);
    logic::decodeProfileJeep();
    drainTx();

    const fake::CanFrame* f = fake::lastCanFrame();

    if constexpr (cfg::kIsBenz) {
        CHECK(f != nullptr);
        if (f != nullptr) {
            CHECK_EQ(f->id, kTx516);
            CHECK_EQ(f->dlc, 8U);
            CHECK_EQ(f->data[0], 0xAAU);
            CHECK_EQ(f->data[1], 0xBBU);
            CHECK_EQ(f->data[2], 0x00U);   /* only TWO bytes are echoed here */
            CHECK_EQ(f->data[7], 0x00U);
        }
    } else {
        /* Nothing at all goes out. A jeep build must not answer 0x500, or
         * every tyre poll would inject a frame onto the radio's KWP response
         * identifier. */
        CHECK_EQ(fake::canFrameCount(), 0U);
    }
}

/* 0x513 echoes FOUR bytes, same 0x516 zero-padded reply. */
TEST(fca_echo_513_retransmits_four_bytes_as_516)
{
    fake::reset();

    std::uint8_t d[8] = {0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0U, 0U, 0U};
    pushFrame(kFcaEcho, 8U, d);
    logic::decodeProfileFca();
    drainTx();

    const fake::CanFrame* f = fake::lastCanFrame();
    CHECK(f != nullptr);
    if (f != nullptr) {
        CHECK_EQ(f->id, kTx516);
        CHECK_EQ(f->dlc, 8U);
        CHECK_EQ(f->data[0], 0xAAU);
        CHECK_EQ(f->data[3], 0xDDU);
        CHECK_EQ(f->data[4], 0x00U);   /* d[4] is NOT echoed */
        CHECK_EQ(f->data[7], 0x00U);
    }
}

/* ===========================================================================
 * The 0x400..0x43F SWC service block (identical in both profiles)
 * ======================================================================== */

/* The low 6 bits of the identifier ARE the key index: they select the software
 * timer slot that gets armed to 150 counts. */
TEST(swc_service_takes_the_key_index_from_the_low_six_bits_of_the_id)
{
    fake::reset();

    std::uint8_t d[8] = {0x00U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    pushFrame(kSwcService + 0x25U, 8U, d);
    logic::decodeProfileJeep();

    CHECK_EQ(ram::app().swc.keycode, 0x25U);
    CHECK_EQ(ram::app().softTimers[0x25], 150);
    CHECK_EQ(ram::app().softTimers[0x24], 0);

    /* The top of the window, 0x43F, is still in range. */
    fake::reset();
    pushFrame(kSwcService + 0x3FU, 8U, d);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().swc.keycode, 0x3FU);
    CHECK_EQ(ram::app().softTimers[0x3F], 150);

    /* 0x440 is outside the 0xFC0 window and must do nothing at all. */
    fake::reset();
    pushFrame(0x440U, 8U, d);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().swc.keycode, 0U);
    CHECK_EQ(ram::app().softTimers[0], 0);
}

/* d[0] must be 0xFD for the frame to be a key report; anything else stops
 * after arming the timer. */
TEST(swc_service_ignores_a_frame_that_is_not_a_key_report)
{
    fake::reset();

    std::uint8_t d[8] = {0x12U, 0x16U, 0U, 0U, 0U, 0U, 0U, 0U};
    pushFrame(kSwcService + 0x05U, 8U, d);
    logic::decodeProfileJeep();

    CHECK_EQ(ram::app().softTimers[5], 150);     /* armed */
    CHECK_EQ(ram::app().swc.keydata, 0U);        /* but not parsed */
    CHECK_EQ(ram::app().miscTimers.swcRespMode, 0U);
}

/* keydata 0x16 is the handshake: it sets the response mode to 9 and primes the
 * reply buffer with 0xFD, without transmitting. */
TEST(swc_service_handshake_sets_the_response_mode)
{
    fake::reset();

    std::uint8_t d[8] = {0xFDU, 0x16U, 0U, 0U, 0U, 0U, 0U, 0U};
    pushFrame(kSwcService + 0x05U, 8U, d);
    logic::decodeProfileJeep();
    drainTx();

    CHECK_EQ(ram::app().swc.keydata, 0x16U);
    CHECK_EQ(ram::app().miscTimers.swcRespMode, 9U);
    CHECK_EQ(ram::app().swcTxBuf[0], 0xFDU);
    CHECK_EQ(fake::canFrameCount(), 0U);
}

/* Same key index as last time plus the handshake code: the 0x50 hold timer is
 * loaded instead of a reply going out. */
TEST(swc_service_repeat_handshake_loads_the_hold_timer)
{
    fake::reset();
    ram::app().swc.keycodePrev = 0x05U;

    std::uint8_t d[8] = {0xFDU, 0x16U, 0U, 0U, 0U, 0U, 0U, 0U};
    pushFrame(kSwcService + 0x05U, 8U, d);
    logic::decodeProfileJeep();
    drainTx();

    CHECK_EQ(ram::app().miscTimers.swcTimer, 0x50U);
    CHECK_EQ(fake::canFrameCount(), 0U);
}

/* A repeated key index with ordinary key data answers on 0x416 with
 * FE <keydataRef>. */
TEST(swc_service_repeat_key_replies_on_0x416)
{
    fake::reset();
    ram::app().swc.keycodePrev = 0x05U;
    ram::app().swc.keydataRef  = 0x2AU;

    std::uint8_t d[8] = {0xFDU, 0x33U, 0U, 0U, 0U, 0U, 0U, 0U};
    pushFrame(kSwcService + 0x05U, 8U, d);
    logic::decodeProfileJeep();
    drainTx();

    CHECK_EQ(fake::canFrameCount(), 1U);
    const fake::CanFrame* f = fake::lastCanFrame();
    CHECK(f != nullptr);
    if (f != nullptr) {
        CHECK_EQ(f->id, kTx416);
        CHECK_EQ(f->dlc, 8U);
        CHECK_EQ(f->data[0], 0xFEU);
        CHECK_EQ(f->data[1], 0x2AU);
    }
}

/* The d[2] probe is qualified by the ACC flag: 0x3F is acknowledged only with
 * ACC on, 0xBF only with ACC off. */
TEST(swc_service_probe_is_qualified_by_the_acc_flag)
{
    fake::reset();
    ram::app().vehicle.setFlag(kFlagAcc, true);

    std::uint8_t on[8] = {0xFDU, 0x33U, 0x3FU, 0U, 0U, 0U, 0U, 0U};
    pushFrame(kSwcService + 0x05U, 8U, on);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().swcTxBuf[2], 0x3FU);

    fake::reset();
    std::uint8_t off[8] = {0xFDU, 0x33U, 0xBFU, 0U, 0U, 0U, 0U, 0U};
    pushFrame(kSwcService + 0x05U, 8U, off);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().swcTxBuf[2], 0xBFU);

    /* 0xBF with ACC on is NOT acknowledged. */
    fake::reset();
    ram::app().vehicle.setFlag(kFlagAcc, true);
    pushFrame(kSwcService + 0x05U, 8U, off);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().swcTxBuf[2], 0x00U);
}

/* The 0xFF probe with ACC off declares the bus down: it clears the presence
 * state AND flushes everything already queued for transmission. */
TEST(swc_service_bus_down_probe_clears_the_status_and_flushes_tx)
{
    fake::reset();
    ram::app().canBusStatus = 2U;            /* kBusRunning */

    const std::uint8_t queued[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    svc::can::txEnqueue(0x111U, 0U, 8U, queued);
    svc::can::txEnqueue(0x222U, 0U, 8U, queued);
    CHECK_EQ(ram::app().canRing.txHead, 2U);

    std::uint8_t d[8] = {0xFDU, 0x33U, 0xFFU, 0U, 0U, 0U, 0U, 0U};
    pushFrame(kSwcService + 0x05U, 8U, d);
    logic::decodeProfileJeep();

    CHECK_EQ(ram::app().canBusStatus, 0U);
    CHECK_EQ(ram::app().canRing.txTail, ram::app().canRing.txHead);

    drainTx();
    CHECK_EQ(fake::canFrameCount(), 0U);     /* the queue really is gone */
}

/* The whole block is identical on profile 1 -- that is why it is one helper. */
TEST(fca_swc_service_behaves_identically)
{
    fake::reset();
    ram::app().swc.keycodePrev = 0x05U;
    ram::app().swc.keydataRef  = 0x2AU;

    std::uint8_t d[8] = {0xFDU, 0x33U, 0U, 0U, 0U, 0U, 0U, 0U};
    pushFrame(kSwcService + 0x05U, 8U, d);
    logic::decodeProfileFca();
    drainTx();

    CHECK_EQ(ram::app().swc.keycode, 0x05U);
    CHECK_EQ(ram::app().softTimers[5], 150);
    CHECK_EQ(fake::canFrameCount(), 1U);
    CHECK_EQ(fake::canFrames()[0].id, kTx416);
}

/* ===========================================================================
 * Drain shape and the bus-alive reload
 * ======================================================================== */

/* At most 20 frames are consumed per call, so a flooded bus cannot starve the
 * main loop. */
TEST(decoder_consumes_at_most_twenty_frames_per_call)
{
    fake::reset();

    for (std::size_t i = 0U; i < 30U; ++i) {
        pushByteAt(0x100U, 0U, 0U);          /* an id nothing decodes */
    }
    CHECK_EQ(ram::app().canRing.rxHead, 30U);

    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().canRing.rxTail, 20U);

    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().canRing.rxTail, 30U);
}

TEST(fca_decoder_consumes_at_most_twenty_frames_per_call)
{
    fake::reset();

    for (std::size_t i = 0U; i < 30U; ++i) {
        pushByteAt(0x100U, 0U, 0U);
    }
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().canRing.rxTail, 20U);
}

/* An empty ring costs nothing and leaves the indices alone: the stock loop
 * spins its pass guard out rather than returning early. */
TEST(decoder_on_an_empty_ring_changes_nothing)
{
    fake::reset();

    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().canRing.rxTail, 0U);
    CHECK_EQ(ram::app().canRing.rxHead, 0U);
    CHECK_EQ(ram::app().canActivityCtr, 0U);   /* no frame -> no reload */
}

/* The bus-alive watchdog reload differs between the profiles: 100 vs 0x14. */
TEST(bus_alive_reload_differs_between_the_profiles)
{
    fake::reset();
    pushByteAt(kJeepIgn, 0U, 0x20U);
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().canActivityCtr, 100U);

    fake::reset();
    pushByteAt(kFcaIgn, 0U, 0x20U);
    logic::decodeProfileFca();
    CHECK_EQ(ram::app().canActivityCtr, 0x14U);
}

/* ===========================================================================
 * dispatchGate -- profile selection and the bus-present debounce
 * ======================================================================== */

/* profileActive == 1 selects the 125 kbit/s decoder; anything else the
 * 83.333 kbit/s one. Proved by which IGNITION id is understood. */
TEST(dispatch_gate_selects_the_profile_from_profile_active)
{
    fake::reset();
    ram::app().profileActive = 0U;
    pushByteAt(kFcaIgn, 0U, 0xE0U);          /* profile 1 id, profile 0 active */
    logic::dispatchGate();
    CHECK_EQ(flags() & kFlagAcc, 0U);

    fake::reset();
    ram::app().profileActive = 0U;
    pushByteAt(kJeepIgn, 0U, 0xE0U);
    logic::dispatchGate();
    CHECK_EQ(flags() & kFlagAcc, kFlagAcc);
    CHECK_EQ(ram::app().canActivityCtr, 100U);

    fake::reset();
    ram::app().profileActive = 1U;
    pushByteAt(kJeepIgn, 0U, 0xE0U);         /* profile 0 id, profile 1 active */
    logic::dispatchGate();
    CHECK_EQ(flags() & kFlagAcc, 0U);

    fake::reset();
    ram::app().profileActive = 1U;
    pushByteAt(kFcaIgn, 0U, 0xE0U);
    logic::dispatchGate();
    CHECK_EQ(flags() & kFlagAcc, kFlagAcc);
    CHECK_EQ(ram::app().canActivityCtr, 0x14U);

    /* Any other value falls back to profile 0. */
    fake::reset();
    ram::app().profileActive = 0x5AU;
    pushByteAt(kJeepIgn, 0U, 0xE0U);
    logic::dispatchGate();
    CHECK_EQ(flags() & kFlagAcc, kFlagAcc);
}

/* ACC off: the 800-pass debounce is reloaded and bit 4 of the periodic 0x18C
 * frame is SET (the "no ignition" marker). */
TEST(dispatch_gate_reloads_the_debounce_while_acc_is_off)
{
    fake::reset();
    ram::app().busPresentDebounce = 5U;
    ram::app().txFrame18c[0] = 0x01U;

    logic::dispatchGate();

    CHECK_EQ(ram::app().busPresentDebounce, 800U);
    CHECK_EQ(ram::app().txFrame18c[0], 0x11U);
}

/* ACC on with the debounce still running: count down, touch nothing else. */
TEST(dispatch_gate_counts_the_debounce_down_while_acc_is_on)
{
    fake::reset();
    ram::app().vehicle.setFlag(kFlagAcc, true);
    ram::app().busPresentDebounce = 2U;
    ram::app().txFrame18c[0] = 0x10U;

    logic::dispatchGate();
    CHECK_EQ(ram::app().busPresentDebounce, 1U);
    CHECK_EQ(ram::app().txFrame18c[0], 0x10U);   /* still marked */

    logic::dispatchGate();
    CHECK_EQ(ram::app().busPresentDebounce, 0U);
}

/* ACC on and the debounce expired: the marker is cleared and the amplifier TX
 * frame is re-armed from the amplifier level. */
TEST(dispatch_gate_clears_the_marker_when_the_debounce_expires)
{
    fake::reset();
    ram::app().vehicle.setFlag(kFlagAcc, true);
    ram::app().busPresentDebounce = 0U;
    ram::app().txFrame18c[0] = 0x13U;
    ram::app().vehicle.setAmpLevel(0x2BU);

    logic::dispatchGate();

    CHECK_EQ(ram::app().txFrame18c[0], 0x03U);
    CHECK_EQ(ram::app().txFrameAmp[0], 0x2BU);
    CHECK_EQ(ram::app().busPresentDebounce, 0U);
}

/* ===========================================================================
 * Jeep RPM and PRNDL -- NOT stock decoders. See docs/REQUIREMENTS.md section
 * 6.1 B, and FR-37a / FR-37b.
 *
 * Written from docs/JEEP-WK-CAN-ID-byte-map.md (both ids marked Confirmed) and
 * from the head-unit side in docs/hiworld-radio-injectable.md section B, not
 * from app/logic/can_decode.cpp.
 *
 * Restated here independently:
 *   0x002 DLC 8, 50 ms  -- d[0]:d[1] engine RPM, 1 rpm/count BE, all FF = n/a
 *   0x003 DLC 7, 450 ms -- d[4] is the ASCII character the cluster displays
 *   UART 0x32 payload   -- [1] gear 1=P 2=N 3=R 4=D 5=S (0 = blank),
 *                          [2..3] RPM BE 1:1, 0xFFFF = blank
 * ========================================================================= */

namespace {

constexpr std::uint32_t kJeepEngineId = 0x002U;

/* The 0x32 payload the head unit will receive, as a window into the vehicle
 * block -- deliberately reached through the UART window rather than through
 * the gear/rpm accessors, so these tests check what actually goes on the wire. */
const volatile std::uint8_t* win32() noexcept
{
    return ram::app().vehicle.window(ram::VehicleState::kWin32Off);
}

void pushJeepGear(std::uint8_t asciiChar, std::uint8_t leverNibble = 0x80U) noexcept
{
    std::uint8_t d[8] = {0U, 0U, leverNibble, 0U, asciiChar, 0U, 0U, 0U};
    pushFrame(kJeepRev, 7U, d);
}

void pushJeepRpm(std::uint8_t hi, std::uint8_t lo) noexcept
{
    std::uint8_t d[8] = {hi, lo, 0U, 0U, 0U, 0U, 0U, 0U};
    pushFrame(kJeepEngineId, 8U, d);
}

}  // namespace

TEST(jeep_rpm_is_a_verbatim_big_endian_copy_into_the_0x32_window)
{
    fake::reset();

    /* 2000 rpm == 0x07D0, the worked example in the injectable note. */
    pushJeepRpm(0x07U, 0xD0U);
    logic::decodeProfileJeep();

    CHECK_EQ(win32()[2], 0x07U);
    CHECK_EQ(win32()[3], 0xD0U);
    CHECK_EQ(ram::app().vehicle.rpm(), 2000U);
}

TEST(jeep_rpm_passes_the_unavailable_sentinel_through_unchanged)
{
    fake::reset();

    /* The car sends all-FF when the engine is off; the head unit reads 0xFFFF
     * as "blank this field". The two sentinels coincide, so the bytes must go
     * through untouched -- clamping or range-rejecting here would freeze the
     * last live reading on the dash instead of clearing it. */
    pushJeepRpm(0xFFU, 0xFFU);
    logic::decodeProfileJeep();

    CHECK_EQ(win32()[2], 0xFFU);
    CHECK_EQ(win32()[3], 0xFFU);
}

TEST(jeep_rpm_is_not_decimated_or_range_limited)
{
    fake::reset();

    /* Profile 1's 0x211 decoder decimates 1-in-21 and drops anything >= 8000.
     * Profile 0 deliberately does neither -- every frame updates, and a high
     * reading is reported rather than discarded. */
    pushJeepRpm(0x1FU, 0x40U);                  /* 8000 */
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.rpm(), 8000U);

    pushJeepRpm(0x00U, 0xC8U);                  /* 200, immediately after */
    logic::decodeProfileJeep();
    CHECK_EQ(ram::app().vehicle.rpm(), 200U);
}

TEST(jeep_gear_maps_every_cluster_character)
{
    const struct { std::uint8_t ascii; std::uint8_t expect; } cases[] = {
        { 'P', 1U }, { 'N', 2U }, { 'R', 3U }, { 'D', 4U },
        { '1', 5U }, { '2', 5U }, { '3', 5U }, { '4', 5U },
    };
    for (const auto& c : cases) {
        fake::reset();
        pushJeepGear(c.ascii);
        logic::decodeProfileJeep();
        CHECK_EQ(win32()[1], c.expect);
    }
}

TEST(jeep_gear_reports_blank_for_an_unknown_character)
{
    fake::reset();

    pushJeepGear(0x00U);
    logic::decodeProfileJeep();
    CHECK_EQ(win32()[1], 0U);       /* 0 = invalid/blank on the head unit */
}

TEST(jeep_gear_uses_d4_not_d2_so_autostick_ranges_are_distinguishable)
{
    fake::reset();

    /* THE REASON d[4] IS USED. d[2] is a lever bitfield in which D and EVERY
     * AutoStick range both read 0x84, so d[2] alone cannot tell them apart.
     * Same d[2], different d[4] -> different gear. */
    pushJeepGear('D', 0x84U);
    logic::decodeProfileJeep();
    CHECK_EQ(win32()[1], 4U);       /* D */

    fake::reset();
    pushJeepGear('3', 0x84U);
    logic::decodeProfileJeep();
    CHECK_EQ(win32()[1], 5U);       /* S -- an AutoStick range, not D */
}

TEST(jeep_gear_does_not_disturb_the_stock_reverse_flag)
{
    fake::reset();

    /* The reverse flag still comes from d[2]'s low nibble, exactly as stock --
     * it gates the steering decode and the reverse status line, so it must not
     * start following the new gear value. Lever nibble 1 = R. */
    pushJeepGear('R', 0x81U);
    logic::decodeProfileJeep();
    CHECK(ram::app().vehicle.reverseOn());
    CHECK_EQ(win32()[1], 3U);

    fake::reset();
    /* Gear says R but the lever nibble does not: the flag follows the nibble. */
    pushJeepGear('R', 0x80U);
    logic::decodeProfileJeep();
    CHECK_FALSE(ram::app().vehicle.reverseOn());
}

TEST(jeep_rpm_and_gear_land_in_the_0x32_window_and_leave_0x72_alone)
{
    fake::reset();

    pushJeepRpm(0x07U, 0xD0U);
    pushJeepGear('D');
    logic::decodeProfileJeep();

    /* Both fields are inside the message the head unit actually parses... */
    CHECK_EQ(win32()[1], 4U);
    CHECK_EQ(win32()[2], 0x07U);
    CHECK_EQ(win32()[3], 0xD0U);
    /* ...and neither has spilled into the 0x72 status message. */
    const volatile std::uint8_t* w72 =
        ram::app().vehicle.window(ram::VehicleState::kWin72Off);
    for (std::size_t i = 0U; i < ram::VehicleState::kWin72Len; ++i) {
        CHECK_EQ(w72[i], 0U);
    }
}

TEST(jeep_profile_only_the_fca_profile_still_uses_its_own_rpm_path)
{
    fake::reset();

    /* 0x002 must not be decoded by profile 1 -- it is a Jeep-bus id, and on the
     * FCA profile that id is not engine speed. */
    pushJeepRpm(0x07U, 0xD0U);
    logic::decodeProfileFca();

    CHECK_EQ(ram::app().vehicle.rpm(), 0U);
}
