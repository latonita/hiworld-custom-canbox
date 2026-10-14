/* ============================================================================
 * test_periodic_tx.cpp -- logic::periodicTxScheduler and logic::clusterTextToCan.
 *
 * Written from the BEHAVIOURAL SPEC in the Ghidra decompile, not from
 * periodic_tx.cpp:
 *     reference/decompile/app/can.c   0x08008110  can_periodic_tx_scheduler
 *     reference/decompile/app/uart.c  0x08007DA8  uart_D2_cluster_text_to_can
 *
 * Every identifier, DLC, phase number, clamp bound and byte split below is
 * restated here from the decompile rather than included from the module under
 * test, so a transcription slip on either side shows up as a failure. A test
 * that merely echoed the implementation would prove nothing.
 *
 * WHAT BREAKS IF THIS UNIT IS WRONG
 * This is everything the box TELLS THE CAR. Get a DLC wrong and the cluster
 * ignores the frame; get the phase table wrong and the amplifier frame stops
 * repeating, so the radio's volume setting silently stops being applied; get
 * the 0xD2 digit split wrong and the cluster shows the wrong number.
 * ==========================================================================*/

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>

#include "can_bus.hpp"
#include "fake_hal.hpp"
#include "param_store.hpp"
#include "periodic_tx.hpp"
#include "ram_map.hpp"

namespace {

/* ---- the spec, restated ------------------------------------------------- */

/* Profile 0 (Jeep WK, 83.333 kbit/s) -- `can_tx_enqueue(0x3d0,0,7,...)` etc.
 * Note the decompile prints the 0x190 case in decimal, as `can_tx_enqueue(400,
 * 0,7,p_tx_frame_190)`; 400 == 0x190. */
constexpr std::uint32_t kP0Amp = 0x3D0U;
constexpr std::uint32_t kP0F0  = 0x0F0U;
constexpr std::uint32_t kP0326 = 0x326U;
constexpr std::uint32_t kP0190 = 0x190U;
constexpr std::uint32_t kP018c = 0x18CU;
constexpr std::uint32_t kP09f  = 0x09FU;

/* Profile 1 (FCA, 125 kbit/s). There is no 0x09F equivalent. */
constexpr std::uint32_t kP1Amp = 0x3D9U;
constexpr std::uint32_t kP1F0  = 0x2E9U;
constexpr std::uint32_t kP1326 = 0x290U;
constexpr std::uint32_t kP1190 = 0x291U;
constexpr std::uint32_t kP118c = 0x293U;

/* The phase each frame fires on, and the length it is sent with. */
constexpr std::uint8_t kPhAmp = 0x01U, kDlAmp = 7U;
constexpr std::uint8_t kPhF0  = 0x03U, kDlF0  = 4U;
constexpr std::uint8_t kPh326 = 0x05U, kDl326 = 8U;
constexpr std::uint8_t kPh190 = 0x07U, kDl190 = 7U;
constexpr std::uint8_t kPh18c = 0x09U, kDl18c = 8U;
constexpr std::uint8_t kPh9f  = 0x0BU, kDl9f  = 8U;

/* `if (phase < 0x96) phase++ else phase = 0` -- 0..0x96 inclusive. */
constexpr std::uint8_t  kPhaseLast  = 0x96U;
constexpr std::uint32_t kPhaseCount = 151U;

/* `*p_can_tx_hold_timer = 400` while ACC is on. */
constexpr std::uint32_t kHoldReload = 400U;

/* The ACC shadow the edge detector keeps. Restated here from the decompile
 * (byte 0x12 of the status block) so the test cross-checks ram_map.hpp rather
 * than echoing it. */
constexpr std::size_t kAccShadow = 0x12U;
static_assert(kAccShadow == ram::VehicleState::kAccShadow,
              "the ACC shadow moved; the edge detector would silently use the "
              "wrong byte");

/* Selector bounds and clamps from the stock switch. */
constexpr std::uint8_t kVolumeMax = 0x26U;
constexpr std::uint8_t kToneMin   = 0x01U;
constexpr std::uint8_t kToneMax   = 0x13U;

/* g_status_block[0x2B + sel] and g_param_struct[0x0F + sel], sel = 1..6. */
constexpr std::size_t kStatusAmpBase = 0x2CU;   /* 0x2B + 1 */
constexpr std::size_t kParamAmpBase  = 0x10U;   /* 0x0F + 1 */

/* ---- helpers ------------------------------------------------------------ */

ram::AppRam& R() noexcept { return ram::app(); }

/* ACC on, with the edge detector already settled so a test that is not about
 * the edge is not perturbed by it. */
void accOnSettled() noexcept
{
    R().vehicle.setFlag(ram::VehicleState::kFlagAccOn, true);
    R().vehicle.at(kAccShadow) = 1U;
}

/* Frames only reach fake::canFrames() once the ring has been pumped into the
 * mailbox, which is the 4 ms task. */
void step() noexcept
{
    logic::periodicTxScheduler();
    svc::can::txDrain();
}

/* Run the scheduler with the phase forced, so one phase can be examined in
 * isolation. The counter is PRE-incremented, so to land on `phase` we park the
 * counter one below it. */
void stepOnPhase(std::uint8_t phase) noexcept
{
    R().periodicTxPhase = static_cast<std::uint8_t>(phase - 1U);
    step();
}

std::size_t countId(std::uint32_t id) noexcept
{
    std::size_t n = 0U;
    for (std::size_t i = 0U; i < fake::canFrameCount(); ++i) {
        if (fake::canFrames()[i].id == id) { ++n; }
    }
    return n;
}

void fillBuffer(volatile std::uint8_t* buf, std::size_t len, std::uint8_t seed) noexcept
{
    for (std::size_t i = 0U; i < len; ++i) {
        buf[i] = static_cast<std::uint8_t>(seed + i);
    }
}

/* Check the last captured frame is `id`, `dlc` long and carries the first
 * `dlc` bytes of `buf`. */
void checkFrame(std::uint32_t id, std::uint8_t dlc, const volatile std::uint8_t* buf) noexcept
{
    const fake::CanFrame* f = fake::lastCanFrame();
    CHECK(f != nullptr);
    if (f == nullptr) { return; }
    CHECK_EQ(f->id, id);
    CHECK_EQ(f->ide, 0U);           /* every one of these is a standard id */
    CHECK_EQ(f->dlc, dlc);
    for (std::size_t i = 0U; i < static_cast<std::size_t>(dlc); ++i) {
        CHECK_EQ(f->data[i], buf[i]);
    }
}

}  // namespace

/* ---------------------------------------------------------------------------
 * The phase counter -- `if (*p_periodic_tx_phase < 0x96) ++ else = 0`
 * ------------------------------------------------------------------------- */

TEST(periodic_tx_phase_advances_exactly_one_per_call)
{
    fake::reset();
    accOnSettled();

    for (std::uint32_t i = 1U; i <= 20U; ++i) {
        logic::periodicTxScheduler();
        CHECK_EQ(R().periodicTxPhase, i);
    }
}

TEST(periodic_tx_phase_wraps_to_zero_after_the_last_phase)
{
    fake::reset();
    accOnSettled();

    R().periodicTxPhase = kPhaseLast;
    logic::periodicTxScheduler();
    CHECK_EQ(R().periodicTxPhase, 0U);

    logic::periodicTxScheduler();
    CHECK_EQ(R().periodicTxPhase, 1U);
}

TEST(periodic_tx_lap_is_151_phases_long)
{
    fake::reset();
    accOnSettled();

    /* Starting from 0, exactly 151 calls bring the counter back to 0. */
    for (std::uint32_t i = 0U; i < kPhaseCount; ++i) {
        logic::periodicTxScheduler();
    }
    CHECK_EQ(R().periodicTxPhase, 0U);
}

/* ---------------------------------------------------------------------------
 * Profile 0: the six identifiers, each on its own phase, each with its own
 * length and its own buffer.
 * ------------------------------------------------------------------------- */

TEST(periodic_tx_profile0_amp_frame_is_3d0_dlc7_from_the_amp_buffer)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 0U;
    fillBuffer(R().txFrameAmp, 7U, 0xA0U);

    stepOnPhase(kPhAmp);

    CHECK_EQ(fake::canFrameCount(), 1U);
    checkFrame(kP0Amp, kDlAmp, R().txFrameAmp);
}

TEST(periodic_tx_profile0_f0_frame_is_0f0_dlc4_from_the_f0_buffer)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 0U;
    fillBuffer(R().txFrameF0, 5U, 0xB0U);

    stepOnPhase(kPhF0);

    CHECK_EQ(fake::canFrameCount(), 1U);
    checkFrame(kP0F0, kDlF0, R().txFrameF0);
}

TEST(periodic_tx_profile0_326_frame_is_326_dlc8_from_the_326_buffer)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 0U;
    fillBuffer(R().txFrame326, 8U, 0xC0U);

    stepOnPhase(kPh326);

    CHECK_EQ(fake::canFrameCount(), 1U);
    checkFrame(kP0326, kDl326, R().txFrame326);
}

TEST(periodic_tx_profile0_190_frame_is_190_dlc7_from_the_190_buffer)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 0U;
    fillBuffer(R().txFrame190, 7U, 0xD0U);

    stepOnPhase(kPh190);

    CHECK_EQ(fake::canFrameCount(), 1U);
    checkFrame(kP0190, kDl190, R().txFrame190);
}

TEST(periodic_tx_profile0_18c_frame_is_18c_dlc8_from_the_18c_buffer)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 0U;
    fillBuffer(R().txFrame18c, 8U, 0xE0U);

    stepOnPhase(kPh18c);

    CHECK_EQ(fake::canFrameCount(), 1U);
    checkFrame(kP018c, kDl18c, R().txFrame18c);
}

TEST(periodic_tx_profile0_9f_frame_is_09f_dlc8_from_the_9f_buffer)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 0U;
    fillBuffer(R().txFrame9f, 8U, 0x10U);
    /* The edge detector is settled, so byte 0 is not overwritten here. */

    stepOnPhase(kPh9f);

    CHECK_EQ(fake::canFrameCount(), 1U);
    checkFrame(kP09f, kDl9f, R().txFrame9f);
}

TEST(periodic_tx_even_phases_and_phase_0d_send_nothing)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 0U;

    const std::uint8_t quiet[] = {0x02U, 0x04U, 0x06U, 0x08U, 0x0AU, 0x0CU,
                                  0x0DU, 0x0EU, 0x20U, 0x95U};
    for (std::size_t i = 0U; i < sizeof quiet; ++i) {
        stepOnPhase(quiet[i]);
    }

    CHECK_EQ(fake::canFrameCount(), 0U);
}

/* ---------------------------------------------------------------------------
 * Profile 1: the other identifier set, from the SAME buffers, and no 0x09F.
 * ------------------------------------------------------------------------- */

TEST(periodic_tx_profile1_sends_the_other_ids_from_the_same_buffers)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 1U;

    fillBuffer(R().txFrameAmp, 7U, 0x40U);
    fillBuffer(R().txFrameF0,  5U, 0x50U);
    fillBuffer(R().txFrame326, 8U, 0x60U);
    fillBuffer(R().txFrame190, 7U, 0x70U);
    fillBuffer(R().txFrame18c, 8U, 0x80U);

    stepOnPhase(kPhAmp);
    checkFrame(kP1Amp, kDlAmp, R().txFrameAmp);

    stepOnPhase(kPhF0);
    checkFrame(kP1F0, kDlF0, R().txFrameF0);

    stepOnPhase(kPh326);
    checkFrame(kP1326, kDl326, R().txFrame326);

    stepOnPhase(kPh190);
    checkFrame(kP1190, kDl190, R().txFrame190);

    stepOnPhase(kPh18c);
    checkFrame(kP118c, kDl18c, R().txFrame18c);

    CHECK_EQ(fake::canFrameCount(), 5U);
}

TEST(periodic_tx_profile1_has_no_frame_on_the_9f_phase)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 1U;

    stepOnPhase(kPh9f);

    CHECK_EQ(fake::canFrameCount(), 0U);
}

TEST(periodic_tx_an_unknown_profile_transmits_nothing_at_all)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 2U;   /* stock tests are `== 0` and `== 1`, no else */

    for (std::uint32_t i = 0U; i < kPhaseCount; ++i) {
        step();
    }

    CHECK_EQ(fake::canFrameCount(), 0U);
}

/* ---------------------------------------------------------------------------
 * A full lap emits each frame exactly once.
 * ------------------------------------------------------------------------- */

TEST(periodic_tx_full_151_phase_sweep_emits_each_profile0_frame_once)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 0U;
    R().periodicTxPhase = 0U;

    for (std::uint32_t i = 0U; i < kPhaseCount; ++i) {
        step();
    }

    CHECK_EQ(fake::canFrameCount(), 6U);
    CHECK_EQ(countId(kP0Amp), 1U);
    CHECK_EQ(countId(kP0F0),  1U);
    CHECK_EQ(countId(kP0326), 1U);
    CHECK_EQ(countId(kP0190), 1U);
    CHECK_EQ(countId(kP018c), 1U);
    CHECK_EQ(countId(kP09f),  1U);
}

TEST(periodic_tx_full_151_phase_sweep_emits_each_profile1_frame_once)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 1U;
    R().periodicTxPhase = 0U;

    for (std::uint32_t i = 0U; i < kPhaseCount; ++i) {
        step();
    }

    CHECK_EQ(fake::canFrameCount(), 5U);
    CHECK_EQ(countId(kP1Amp), 1U);
    CHECK_EQ(countId(kP1F0),  1U);
    CHECK_EQ(countId(kP1326), 1U);
    CHECK_EQ(countId(kP1190), 1U);
    CHECK_EQ(countId(kP118c), 1U);
}

/* ---------------------------------------------------------------------------
 * The ACC edge detector and the 3.2 s post-ACC hold.
 * ------------------------------------------------------------------------- */

TEST(periodic_tx_acc_rising_edge_primes_the_9f_frame_with_1)
{
    fake::reset();
    R().vehicle.setFlag(ram::VehicleState::kFlagAccOn, true);
    R().vehicle.at(kAccShadow) = 0U;
    R().txFrame9f[0] = 0x55U;

    logic::periodicTxScheduler();

    CHECK_EQ(R().txFrame9f[0], 1U);
    CHECK_EQ(R().vehicle.at(kAccShadow), 1U);
}

TEST(periodic_tx_acc_falling_edge_primes_the_9f_frame_with_7)
{
    fake::reset();
    R().vehicle.setFlag(ram::VehicleState::kFlagAccOn, false);
    R().vehicle.at(kAccShadow) = 1U;
    R().txFrame9f[0] = 0x55U;

    logic::periodicTxScheduler();

    CHECK_EQ(R().txFrame9f[0], 7U);
    CHECK_EQ(R().vehicle.at(kAccShadow), 0U);
}

TEST(periodic_tx_hold_timer_is_pinned_at_400_while_acc_is_on)
{
    fake::reset();
    accOnSettled();

    R().canTxHoldTimer = 3U;
    logic::periodicTxScheduler();
    CHECK_EQ(R().canTxHoldTimer, kHoldReload);

    logic::periodicTxScheduler();
    CHECK_EQ(R().canTxHoldTimer, kHoldReload);
}

TEST(periodic_tx_hold_timer_counts_down_one_per_call_once_acc_drops)
{
    fake::reset();
    R().vehicle.at(kAccShadow) = 0U;    /* ACC already off, edge settled */
    R().canTxHoldTimer = kHoldReload;

    for (std::uint32_t i = 1U; i <= 5U; ++i) {
        logic::periodicTxScheduler();
        CHECK_EQ(R().canTxHoldTimer, kHoldReload - i);
    }
}

TEST(periodic_tx_everything_stops_once_the_hold_timer_reaches_zero)
{
    fake::reset();
    R().vehicle.at(kAccShadow) = 0U;
    R().profileActive   = 0U;
    R().canTxHoldTimer  = 0U;
    R().periodicTxPhase = 0U;

    step();

    /* No frame, and -- the part that is easy to get wrong -- the phase counter
     * is NOT advanced, so the table resumes where it stopped. */
    CHECK_EQ(fake::canFrameCount(), 0U);
    CHECK_EQ(R().periodicTxPhase, 0U);
}

TEST(periodic_tx_republishes_the_latched_profile_in_bit_7_of_the_status_byte)
{
    fake::reset();
    accOnSettled();

    R().profileActive = 1U;
    logic::periodicTxScheduler();
    CHECK_EQ(R().vehicle.at(ram::VehicleState::kFlags) & 0x80U, 0x80U);
    /* ...without disturbing the decoded flags underneath. */
    CHECK_EQ(R().vehicle.at(ram::VehicleState::kFlags) & 0x7FU,
             ram::VehicleState::kFlagAccOn);

    R().profileActive = 0U;
    logic::periodicTxScheduler();
    CHECK_EQ(R().vehicle.at(ram::VehicleState::kFlags) & 0x80U, 0U);
}

/* ---------------------------------------------------------------------------
 * The head unit's tone / volume command (UART 0xAD).
 *
 * `g_cmd_block[0x10] == 1` is the trigger, [0x0E] the selector and [0x0F] the
 * value; each selector 1..6 writes the amplifier frame, the status block and
 * the parameter page, and the amplifier frame is transmitted immediately.
 * ------------------------------------------------------------------------- */

TEST(periodic_tx_tone_command_lands_in_all_three_places_for_every_selector)
{
    for (std::uint8_t sel = 1U; sel <= 6U; ++sel) {
        fake::reset();
        accOnSettled();
        R().profileActive   = 0U;
        R().periodicTxPhase = 0x40U;      /* a quiet phase, so only the amp
                                           * frame below is transmitted      */

        /* 0x0A is inside BOTH clamp windows (volume 0..0x26, tone 1..0x13),
         * so the same value is legal for every selector. */
        const std::uint8_t value = 0x0AU;
        R().hostCmd.periodicTxCfg[0] = sel;
        R().hostCmd.periodicTxCfg[1] = value;
        R().hostCmd.periodicNewData  = 1U;

        step();

        const std::size_t i = static_cast<std::size_t>(sel - 1U);
        CHECK_EQ(R().txFrameAmp[i], value);
        CHECK_EQ(R().vehicle.at(kStatusAmpBase + i), value);
        CHECK_EQ(R().param[kParamAmpBase + i], value);

        /* the flag is consumed */
        CHECK_EQ(R().hostCmd.periodicNewData, 0U);

        /* and the amplifier frame goes out at once, not at its phase */
        CHECK_EQ(fake::canFrameCount(), 1U);
        checkFrame(kP0Amp, kDlAmp, R().txFrameAmp);
    }
}

TEST(periodic_tx_selector_1_is_the_named_amp_level_byte)
{
    fake::reset();
    accOnSettled();
    R().periodicTxPhase = 0x40U;

    R().hostCmd.periodicTxCfg[0] = 1U;
    R().hostCmd.periodicTxCfg[1] = 0x1EU;
    R().hostCmd.periodicNewData  = 1U;

    step();

    /* Selector 1 writes g_amp_level, which the map names ampLevel(). */
    CHECK_EQ(R().vehicle.ampLevel(), 0x1EU);
}

TEST(periodic_tx_volume_is_clamped_to_0x26)
{
    fake::reset();
    accOnSettled();
    R().periodicTxPhase = 0x40U;

    R().hostCmd.periodicTxCfg[0] = 1U;
    R().hostCmd.periodicTxCfg[1] = 0x7FU;
    R().hostCmd.periodicNewData  = 1U;

    step();

    CHECK_EQ(R().hostCmd.periodicTxCfg[1], kVolumeMax);  /* clamped in place */
    CHECK_EQ(R().txFrameAmp[0], kVolumeMax);
    CHECK_EQ(R().vehicle.ampLevel(), kVolumeMax);
}

TEST(periodic_tx_tone_zero_is_pulled_up_to_one)
{
    fake::reset();
    accOnSettled();
    R().periodicTxPhase = 0x40U;

    R().hostCmd.periodicTxCfg[0] = 3U;
    R().hostCmd.periodicTxCfg[1] = 0U;
    R().hostCmd.periodicNewData  = 1U;

    step();

    CHECK_EQ(R().txFrameAmp[2], kToneMin);
    CHECK_EQ(R().vehicle.at(kStatusAmpBase + 2U), kToneMin);
}

TEST(periodic_tx_tone_at_or_above_0x14_is_pulled_down_to_0x13)
{
    fake::reset();
    accOnSettled();
    R().periodicTxPhase = 0x40U;

    R().hostCmd.periodicTxCfg[0] = 4U;
    R().hostCmd.periodicTxCfg[1] = 0x14U;   /* first value that clamps */
    R().hostCmd.periodicNewData  = 1U;

    step();

    CHECK_EQ(R().txFrameAmp[3], kToneMax);
    CHECK_EQ(R().vehicle.at(kStatusAmpBase + 3U), kToneMax);
}

TEST(periodic_tx_tone_command_uses_the_profile1_amp_id)
{
    fake::reset();
    accOnSettled();
    R().profileActive   = 1U;
    R().periodicTxPhase = 0x40U;

    R().hostCmd.periodicTxCfg[0] = 1U;
    R().hostCmd.periodicTxCfg[1] = 0x05U;
    R().hostCmd.periodicNewData  = 1U;

    step();

    CHECK_EQ(fake::canFrameCount(), 1U);
    checkFrame(kP1Amp, kDlAmp, R().txFrameAmp);
}

TEST(periodic_tx_no_tone_command_means_no_extra_amp_frame)
{
    fake::reset();
    accOnSettled();
    R().periodicTxPhase = 0x40U;
    R().hostCmd.periodicNewData = 0U;

    step();

    CHECK_EQ(fake::canFrameCount(), 0U);
}

/* ---------------------------------------------------------------------------
 * The 0xD2 cluster-text path -- 0x08007DA8, driven from part 4a.
 * ------------------------------------------------------------------------- */

TEST(cluster_text_class1_splits_a_four_digit_value_across_18c_and_9f)
{
    fake::reset();

    /* cluster[1..8] = "___1234_": the stock parse for classes 1..3 takes the
     * thousands from [4] when it is '1', hundreds from [5], tens from [6] and
     * -- skipping [7] -- units from [8]. So "xxx1" "2" "3" "x" "4" -> 1234. */
    R().hostCmd.cluster[0] = 0x01U;
    R().hostCmd.cluster[4] = static_cast<std::uint8_t>('1');
    R().hostCmd.cluster[5] = static_cast<std::uint8_t>('2');
    R().hostCmd.cluster[6] = static_cast<std::uint8_t>('3');
    R().hostCmd.cluster[7] = static_cast<std::uint8_t>('9');  /* skipped */
    R().hostCmd.cluster[8] = static_cast<std::uint8_t>('4');

    /* The top three bits of 190[0] and the top nibble of 18c[1] belong to
     * another writer and must survive. */
    R().txFrame190[0] = 0xFFU;
    R().txFrame18c[1] = 0xA5U;

    logic::clusterTextToCan();

    CHECK_EQ(R().txFrame190[0], 0xE1U);          /* (0xFF & 0xE0) + 1 */
    CHECK_EQ(R().txFrame18c[1], 0xA4U);          /* (0xA5 & 0xF0) | (1234>>8 & 0xF) */
    CHECK_EQ(R().txFrame18c[2], 1234U & 0xFFU);  /* 0xD2 */
    CHECK_EQ(R().txFrame9f[0], 0x01U);           /* 190[0] & 0x1F */
    CHECK_EQ(R().txFrame9f[1], 0x24U);           /* (1234>>8) | 0x20 */
    CHECK_EQ(R().txFrame9f[2], 1234U & 0xFFU);
}

TEST(cluster_text_class1_three_digit_form_drops_the_thousands_term)
{
    fake::reset();

    R().hostCmd.cluster[0] = 0x02U;
    R().hostCmd.cluster[4] = static_cast<std::uint8_t>('0');  /* not '1' */
    R().hostCmd.cluster[5] = static_cast<std::uint8_t>('2');
    R().hostCmd.cluster[6] = static_cast<std::uint8_t>('3');
    R().hostCmd.cluster[8] = static_cast<std::uint8_t>('4');

    logic::clusterTextToCan();

    CHECK_EQ(R().txFrame18c[2], 234U);
    CHECK_EQ(R().txFrame18c[1] & 0x0FU, 0U);
    CHECK_EQ(R().txFrame9f[2], 234U);
}

TEST(cluster_text_class4_takes_the_units_digit_from_byte_7_not_byte_8)
{
    fake::reset();

    /* The asymmetry is in the image: classes 4..5 read [7], classes 1..3 read
     * [8], and each skips the other byte. */
    R().hostCmd.cluster[0] = 0x04U;
    R().hostCmd.cluster[4] = static_cast<std::uint8_t>('1');
    R().hostCmd.cluster[5] = static_cast<std::uint8_t>('2');
    R().hostCmd.cluster[6] = static_cast<std::uint8_t>('3');
    R().hostCmd.cluster[7] = static_cast<std::uint8_t>('4');
    R().hostCmd.cluster[8] = static_cast<std::uint8_t>('9');  /* skipped */
    R().txFrame190[0] = 0xFFU;

    logic::clusterTextToCan();

    CHECK_EQ(R().txFrame190[0], 0xE0U);          /* class code 0 */
    CHECK_EQ(R().txFrame18c[2], 1234U & 0xFFU);
    CHECK_EQ(R().txFrame9f[1], 0x24U);
}

TEST(cluster_text_blank_and_popup_classes_only_set_the_class_code)
{
    fake::reset();
    R().txFrame190[0] = 0x1FU;
    R().hostCmd.cluster[0] = 0x00U;
    logic::clusterTextToCan();
    CHECK_EQ(R().txFrame190[0], 0x1DU);

    fake::reset();
    R().hostCmd.cluster[0] = 0x0AU;
    logic::clusterTextToCan();
    CHECK_EQ(R().txFrame190[0], 0x08U);

    /* 0x0C also mirrors the class code into 0x09F byte 0. */
    fake::reset();
    R().hostCmd.cluster[0] = 0x0CU;
    logic::clusterTextToCan();
    CHECK_EQ(R().txFrame190[0], 0x06U);
    CHECK_EQ(R().txFrame9f[0], 0x06U);
}

TEST(cluster_text_text_classes_set_code_4_and_sub_code_3)
{
    fake::reset();

    R().hostCmd.cluster[0] = 0x06U;
    R().hostCmd.cluster[1] = static_cast<std::uint8_t>('1');
    R().hostCmd.cluster[2] = static_cast<std::uint8_t>('2');
    R().hostCmd.cluster[3] = static_cast<std::uint8_t>('7');
    R().txFrame190[0] = 0xE0U;
    R().txFrame190[1] = 0xE0U;

    logic::clusterTextToCan();

    CHECK_EQ(R().txFrame190[0], 0xE4U);
    CHECK_EQ(R().txFrame190[1], 0xE3U);
    CHECK_EQ(R().txFrame18c[2], 127U);
    CHECK_EQ(R().txFrame18c[1] & 0x0FU, 0U);
    CHECK_EQ(R().txFrame9f[0], 0x04U);
    /* NOTE: byte 1 is NOT or-ed with 0x20 on this branch. */
    CHECK_EQ(R().txFrame9f[1], 0U);
    CHECK_EQ(R().txFrame9f[2], 127U);
}

TEST(cluster_text_class7_overwrites_the_sub_code_with_8)
{
    fake::reset();

    R().hostCmd.cluster[0] = 0x07U;
    R().hostCmd.cluster[1] = static_cast<std::uint8_t>('0');
    R().hostCmd.cluster[2] = static_cast<std::uint8_t>('0');
    R().hostCmd.cluster[3] = static_cast<std::uint8_t>('1');

    logic::clusterTextToCan();

    CHECK_EQ(R().txFrame190[0] & 0x1FU, 0x04U);
    CHECK_EQ(R().txFrame190[1] & 0x1FU, 0x08U);
    CHECK_EQ(R().txFrame18c[2], 1U);
}

TEST(cluster_text_text_classes_zero_the_value_when_byte_1_is_not_a_digit)
{
    fake::reset();

    R().txFrame18c[1] = 0x5FU;
    R().txFrame18c[2] = 0x99U;
    R().hostCmd.cluster[0] = 0x0DU;
    R().hostCmd.cluster[1] = static_cast<std::uint8_t>('A');   /* not a digit */

    logic::clusterTextToCan();

    CHECK_EQ(R().txFrame18c[1], 0x50U);   /* low nibble cleared, high kept */
    CHECK_EQ(R().txFrame18c[2], 0U);
}

TEST(cluster_text_ignored_classes_change_nothing)
{
    const std::uint8_t ignored[] = {0x08U, 0x09U, 0x0BU, 0x0FU, 0x10U,
                                    0x11U, 0x12U, 0x13U, 0x14U, 0x17U, 0x40U};

    for (std::size_t i = 0U; i < sizeof ignored; ++i) {
        fake::reset();
        R().txFrame190[0] = 0x3CU;
        R().txFrame190[1] = 0x3DU;
        R().txFrame18c[1] = 0x3EU;
        R().txFrame18c[2] = 0x3FU;
        R().txFrame9f[0]  = 0x41U;
        R().hostCmd.cluster[0] = ignored[i];

        logic::clusterTextToCan();

        CHECK_EQ(R().txFrame190[0], 0x3CU);
        CHECK_EQ(R().txFrame190[1], 0x3DU);
        CHECK_EQ(R().txFrame18c[1], 0x3EU);
        CHECK_EQ(R().txFrame18c[2], 0x3FU);
        CHECK_EQ(R().txFrame9f[0],  0x41U);
    }
}

TEST(cluster_text_releases_the_d2_receive_state)
{
    fake::reset();
    R().d2ClusterState = 3U;
    R().hostCmd.cluster[0] = 0x00U;

    logic::clusterTextToCan();

    CHECK_EQ(R().d2ClusterState, 0U);
}

/* ---- the scheduler drives it, and consumes the flag --------------------- */

TEST(periodic_tx_consumes_the_cluster_new_data_flag_and_runs_the_translation)
{
    fake::reset();
    accOnSettled();
    R().periodicTxPhase = 0x40U;

    R().hostCmd.cluster[0] = 0x01U;
    R().hostCmd.cluster[4] = static_cast<std::uint8_t>('1');
    R().hostCmd.cluster[5] = static_cast<std::uint8_t>('2');
    R().hostCmd.cluster[6] = static_cast<std::uint8_t>('3');
    R().hostCmd.cluster[8] = static_cast<std::uint8_t>('4');
    R().hostCmd.clusterNewData = 1U;

    logic::periodicTxScheduler();

    CHECK_EQ(R().hostCmd.clusterNewData, 0U);
    CHECK_EQ(R().txFrame190[0] & 0x1FU, 0x01U);
    CHECK_EQ(R().txFrame18c[2], 1234U & 0xFFU);
    CHECK_EQ(R().txFrame9f[2], 1234U & 0xFFU);
}

TEST(periodic_tx_leaves_the_cluster_block_alone_when_the_flag_is_clear)
{
    fake::reset();
    accOnSettled();
    R().periodicTxPhase = 0x40U;

    R().hostCmd.cluster[0]     = 0x00U;
    R().hostCmd.clusterNewData = 0U;
    R().txFrame190[0]          = 0x11U;

    logic::periodicTxScheduler();

    CHECK_EQ(R().txFrame190[0], 0x11U);
}

TEST(periodic_tx_the_three_cluster_frames_reach_the_bus_on_their_phases)
{
    fake::reset();
    accOnSettled();
    R().profileActive = 0U;

    R().hostCmd.cluster[0] = 0x01U;
    R().hostCmd.cluster[4] = static_cast<std::uint8_t>('1');
    R().hostCmd.cluster[5] = static_cast<std::uint8_t>('2');
    R().hostCmd.cluster[6] = static_cast<std::uint8_t>('3');
    R().hostCmd.cluster[8] = static_cast<std::uint8_t>('4');
    R().hostCmd.clusterNewData = 1U;

    /* The command is consumed AFTER the phase table has run (part 4a follows
     * part 3 in the stock function), so the translation has to land on a quiet
     * phase first -- otherwise the frame transmitted on phase 7 would still be
     * the PREVIOUS contents of txFrame190. */
    stepOnPhase(0x40U);
    CHECK_EQ(fake::canFrameCount(), 0U);
    CHECK_EQ(R().hostCmd.clusterNewData, 0U);

    stepOnPhase(kPh190);
    checkFrame(kP0190, kDl190, R().txFrame190);

    stepOnPhase(kPh18c);
    checkFrame(kP018c, kDl18c, R().txFrame18c);

    stepOnPhase(kPh9f);
    checkFrame(kP09f, kDl9f, R().txFrame9f);

    CHECK_EQ(fake::canFrameCount(), 3U);
}

namespace {
/* Helpers shared with tests/host/test_param_store.cpp, copied when the frame
 * priming moved here. The vehicle-block offsets are restated from the stock
 * addresses so these cross-check ram_map.hpp rather than echo it. */
constexpr std::size_t kBytes = 32U;   /* config bytes in the parameter page */
constexpr std::size_t kIdxProfile = 0x01U;  /* == svc::params::kProfile */
static_assert(kIdxProfile == svc::params::kProfile, "profile byte index");
std::uint8_t veh(std::size_t off) noexcept { return ram::app().vehicle.at(off); }
/* Vehicle-block offsets the stock code reached through g_vstate_mid
 * (= the vehicle block + 0x17). Restated as absolute SRAM addresses minus the
 * block base, exactly as globals.h had them. */
constexpr std::size_t kVehVolume  = 0x20001B60U - 0x20001B49U;  /* mid[0]    */
constexpr std::size_t kVehTone0   = 0x20001B61U - 0x20001B49U;  /* mid[1]    */
constexpr std::size_t kVehAmpLvl  = 0x20001B75U - 0x20001B49U;  /* mid[0x15] */
constexpr std::size_t kVehAmpTone = 0x20001B76U - 0x20001B49U;  /* mid[0x16] */

/* The first 32 config bytes of reference/parampage_0x0801F800_2k.bin, i.e. the
 * low byte of each of the first 32 words:
 *     aa 00 00 00  01 00 00 00  ff 00 00 00 ...
 *     [0x40] 10 00 00 00  0a .. 0a .. 0d .. 0a .. 0a ..  00 00 00 00
 * magic AA, profile 1, volume 0x10, tones 0A 0A 0D 0A 0A -- the documented
 * "0A 0A 0D 0A 0A tail" -- and the NUL at [0x16]. */
constexpr std::uint8_t kRealPage[kBytes] = {
    0xAAU, 0x01U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0x10U, 0x0AU, 0x0AU, 0x0DU, 0x0AU, 0x0AU, 0x00U, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
};

/* ---- helpers ------------------------------------------------------------ */

/* Program config byte `i` the way the stock image stores it: one byte per
 * 32-bit word, the other three bytes left at their erased value of 0x00 after
 * a program (see the real dump: "aa 00 00 00"). */
void pageByte(std::size_t i, std::uint8_t v) noexcept
{
    fake::setParamWord(i, v);
}

void writeRealPage() noexcept
{
    for (std::size_t i = 0U; i < kBytes; ++i) {
        pageByte(i, kRealPage[i]);
    }
}
}  // namespace

/* ===========================================================================
 * logic::primeTxTemplates()
 *
 * These cases moved here from tests/host/test_param_store.cpp when the frame
 * priming was split out of svc::params::validateDefaults(). They assert CAN
 * frame contents, so they belong with the module that owns those frames.
 *
 * Each calls validateDefaults() first because that is what latches
 * profileActive, which everything primeTxTemplates() writes depends on -- the
 * same order main_loop uses.
 * ========================================================================= */

/* The 0x416 SWC reply payload is a constant: FE 16 3F FF FF FF FF FF. */
TEST(prime_tx_templates_primes_the_swc_frame)
{
    fake::reset();
    svc::params::load();

    svc::params::validateDefaults();
    logic::primeTxTemplates();

    const std::uint8_t want[8] = {0xFEU, 0x16U, 0x3FU, 0xFFU,
                                  0xFFU, 0xFFU, 0xFFU, 0xFFU};
    for (std::size_t i = 0U; i < 8U; ++i) {
        CHECK_EQ(ram::app().swcTxBuf[i], want[i]);
    }
}

/* The stored volume and the five tone bytes fan out into three places:
 *   g_tx_frame_amp[1..5]   the amplifier CAN frame (0x3D0 / 0x3D9)
 *   g_vstate_mid[0]        the volume byte of UART message 0x12
 *   g_vstate_mid[1..5]     the tone bytes of UART message 0x12
 * Checked with the real page, so the expected values are 0x10 and 0A 0A 0D 0A 0A. */
TEST(prime_tx_templates_primes_volume_tone_and_amp_frame)
{
    fake::reset();
    writeRealPage();
    svc::params::load();

    svc::params::validateDefaults();
    logic::primeTxTemplates();

    CHECK_EQ(veh(kVehVolume), 0x10);

    const std::uint8_t tone[5] = {0x0AU, 0x0AU, 0x0DU, 0x0AU, 0x0AU};
    for (std::size_t i = 0U; i < 5U; ++i) {
        CHECK_EQ(ram::app().txFrameAmp[i + 1U], tone[i]);
        CHECK_EQ(veh(kVehTone0 + i), tone[i]);
    }

    /* txFrameAmp[0] and [6] are not part of the tone fan-out and stay as the
     * scatter table left them. */
    CHECK_EQ(ram::app().txFrameAmp[0], 0);
    CHECK_EQ(ram::app().txFrameAmp[6], 0);
}

/* The SAME five values are aliased a second time at g_vstate_mid[0x16..0x1A],
 * i.e. inside the UART 0xA6 window -- the documented "0A 0A 0D 0A 0A tail"
 * quirk. This is stock behaviour, not a transcription slip, and the head unit
 * depends on seeing it. */
TEST(prime_tx_templates_duplicates_the_tone_tail)
{
    fake::reset();
    writeRealPage();
    svc::params::load();

    svc::params::validateDefaults();
    logic::primeTxTemplates();

    const std::uint8_t tail[5] = {0x0AU, 0x0AU, 0x0DU, 0x0AU, 0x0AU};
    for (std::size_t i = 0U; i < 5U; ++i) {
        CHECK_EQ(veh(kVehAmpTone + i), tail[i]);
        CHECK_EQ(veh(kVehAmpTone + i), veh(kVehTone0 + i));
    }
}

/* g_vstate_mid[0x15] == g_amp_level @0x20001b75, byte 0 of the UART 0xA6
 * window: 30 (0x1E) on profile 1, 20 (0x14) on profile 0. */
TEST(prime_tx_templates_amp_level_follows_the_profile)
{
    fake::reset();
    writeRealPage();
    pageByte(kIdxProfile, 1U);
    svc::params::load();
    svc::params::validateDefaults();
    logic::primeTxTemplates();
    CHECK_EQ(veh(kVehAmpLvl), 0x1E);
    CHECK_EQ(ram::app().vehicle.ampLevel(), 0x1E);

    fake::reset();
    writeRealPage();
    pageByte(kIdxProfile, 0U);
    svc::params::load();
    svc::params::validateDefaults();
    logic::primeTxTemplates();
    CHECK_EQ(veh(kVehAmpLvl), 0x14);
}

/* The periodic-TX templates. 0x326 is the profile-dependent one: all zero on
 * profile 1, 00 07 0F 07 on profile 0 (then 00 00 00 on both). */
TEST(prime_tx_templates_primes_the_profile_dependent_templates)
{
    fake::reset();
    writeRealPage();
    pageByte(kIdxProfile, 1U);
    svc::params::load();
    svc::params::validateDefaults();
    logic::primeTxTemplates();
    for (std::size_t i = 0U; i < 7U; ++i) {
        CHECK_EQ(ram::app().txFrame326[i], 0);
    }

    fake::reset();
    writeRealPage();
    pageByte(kIdxProfile, 0U);
    svc::params::load();
    svc::params::validateDefaults();
    logic::primeTxTemplates();
    const std::uint8_t want326[7] = {0x00U, 0x07U, 0x0FU, 0x07U,
                                     0x00U, 0x00U, 0x00U};
    for (std::size_t i = 0U; i < 7U; ++i) {
        CHECK_EQ(ram::app().txFrame326[i], want326[i]);
    }
}

/* The profile-independent templates: 0x0F0, 0x190, 0x18C and 0x09F. */
TEST(prime_tx_templates_primes_the_constant_templates)
{
    fake::reset();
    writeRealPage();
    svc::params::load();

    svc::params::validateDefaults();
    logic::primeTxTemplates();

    for (std::size_t i = 0U; i < 4U; ++i) {
        CHECK_EQ(ram::app().txFrameF0[i], 0);
    }

    const std::uint8_t want190[7] = {0x01U, 0x0DU, 0x65U, 0x00U,
                                     0x10U, 0x00U, 0x07U};
    for (std::size_t i = 0U; i < 7U; ++i) {
        CHECK_EQ(ram::app().txFrame190[i], want190[i]);
    }

    const std::uint8_t want18c[8] = {0x50U, 0x00U, 0x00U, 0x20U,
                                     0x00U, 0x00U, 0x00U, 0x00U};
    for (std::size_t i = 0U; i < 8U; ++i) {
        CHECK_EQ(ram::app().txFrame18c[i], want18c[i]);
    }

    const std::uint8_t want9f[8] = {0x01U, 0x23U, 0x6BU, 0x00U,
                                    0x00U, 0x00U, 0x00U, 0x1FU};
    for (std::size_t i = 0U; i < 8U; ++i) {
        CHECK_EQ(ram::app().txFrame9f[i], want9f[i]);
    }
}

/* Defaulting a virgin page gives the centre tone value everywhere the tone
 * bytes are mirrored, and volume 0 -- the state a brand-new box boots in. */
TEST(prime_tx_templates_fans_out_the_default_tones)
{
    fake::reset();
    svc::params::load();           /* erased -> defaults apply */

    svc::params::validateDefaults();
    logic::primeTxTemplates();

    CHECK_EQ(veh(kVehVolume), 0);
    for (std::size_t i = 0U; i < 5U; ++i) {
        CHECK_EQ(ram::app().txFrameAmp[i + 1U], 10);
        CHECK_EQ(veh(kVehTone0 + i), 10);
        CHECK_EQ(veh(kVehAmpTone + i), 10);
    }
    /* Default profile is 1, so the amp level is the profile-1 value. */
    CHECK_EQ(veh(kVehAmpLvl), 0x1E);
}
