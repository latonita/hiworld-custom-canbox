/* ============================================================================
 * test_ram_map.cpp -- the state object's internal invariants, and the harness.
 *
 * These used to assert absolute SRAM addresses, because the state was pinned to
 * the stock firmware's map. It is ordinary .bss now, so where it lands is the
 * linker's business and not worth a test.
 *
 * What IS still worth asserting is the geometry the head-unit protocol depends
 * on: four UART messages are overlapping windows onto one run of bytes, and a
 * field moving between windows would tell the head unit something false about
 * the car. Most of that is proved at compile time in ram_map.hpp; these cases
 * cover what a static_assert cannot -- that the named accessors actually land
 * on the bytes the decompile says they do.
 * ==========================================================================*/

#include "test_framework.hpp"

#include "fake_hal.hpp"
#include "ram_map.hpp"

extern "C" {
#include "hal_flash.h"
#include "hal_gpio.h"
}

namespace {

using VS = ram::VehicleState;

/* The window offsets restated independently of ram_map.hpp, from the stock
 * descriptor table (flash 0x08009F2C) and reference/decompile/app/can_rx_decode.c, so this is a
 * cross-check rather than a tautology. */
constexpr std::size_t kWin72 = 0x00U;
constexpr std::size_t kWin12 = 0x13U;
constexpr std::size_t kWin32 = 0x1DU;
constexpr std::size_t kWinA6 = 0x2CU;

/* The byte the old code reached as g_vstate_mid[], which was this block + 0x17. */
constexpr std::size_t kVStateMid = 0x17U;

}  // namespace

TEST(vehicle_state_doors_accessor_writes_byte_2_of_the_0x12_window)
{
    fake::reset();
    ram::VehicleState& v = ram::app().vehicle;

    v.setDoorBit(VS::kDoorLeftFront, true);
    CHECK_EQ(v.window(kWin12)[2], 0x80);

    v.setDoorBit(VS::kDoorTailgate, true);
    CHECK_EQ(v.window(kWin12)[2], 0x88);

    v.setDoorBit(VS::kDoorLeftFront, false);
    CHECK_EQ(v.window(kWin12)[2], 0x08);
}

TEST(vehicle_state_amp_level_is_byte_0_of_the_0xA6_window)
{
    fake::reset();
    ram::VehicleState& v = ram::app().vehicle;

    /* The parameter code wrote this through g_vstate_mid[0x15]; the UART 0xA6
     * slot reads it as payload byte 0. Both must still be the same byte. */
    v.setAmpLevel(0x1E);
    CHECK_EQ(v.window(kWinA6)[0], 0x1E);
    CHECK_EQ(v.at(kVStateMid + 0x15U), 0x1E);
}

TEST(vehicle_state_volume_is_byte_4_of_the_0x12_window)
{
    fake::reset();
    ram::VehicleState& v = ram::app().vehicle;

    /* The old g_vstate_mid[0]. */
    v.at(VS::kVolume) = 0x20;
    CHECK_EQ(v.window(kWin12)[4], 0x20);
    CHECK_EQ(v.at(kVStateMid), 0x20);
}

TEST(vehicle_state_gear_and_rpm_sit_in_the_0x32_window)
{
    fake::reset();
    ram::VehicleState& v = ram::app().vehicle;

    v.setGear(ram::kGearReverse);
    v.setRpm(0x0B, 0xB8);                       /* 3000 rpm */

    CHECK_EQ(v.window(kWin32)[1], ram::kGearReverse);
    CHECK_EQ(v.window(kWin32)[2], 0x0B);
    CHECK_EQ(v.window(kWin32)[3], 0xB8);
    CHECK_EQ(v.rpm(), 3000U);
}

TEST(vehicle_state_flags_and_key_sit_in_the_0x72_window)
{
    fake::reset();
    ram::VehicleState& v = ram::app().vehicle;

    v.setFlag(VS::kFlagAccOn, true);
    v.setSwcKey(0x0B);
    v.setIllumLevel(55U);

    CHECK_EQ(v.window(kWin72)[0], VS::kFlagAccOn);
    CHECK_EQ(v.window(kWin72)[2], 0x0B);
    CHECK_EQ(v.window(kWin72)[3], 55);
}

TEST(vehicle_state_windows_do_not_run_past_the_block)
{
    fake::reset();
    /* A window overrunning the block would make the UART framer read whatever
     * the linker happened to place next. */
    CHECK(kWin72 + VS::kWin72Len <= VS::kSize);
    CHECK(kWin12 + VS::kWin12Len <= VS::kSize);
    CHECK(kWin32 + VS::kWin32Len <= VS::kSize);
    CHECK(kWinA6 + VS::kWinA6Len <= VS::kSize);
    CHECK_EQ(sizeof(ram::VehicleState), VS::kSize);
}

TEST(vehicle_state_steering_angle_is_big_endian)
{
    fake::reset();
    ram::VehicleState& v = ram::app().vehicle;

    v.setSteeringAngle(0x1234U);
    CHECK_EQ(v.at(VS::kSteerHi), 0x12);
    CHECK_EQ(v.at(VS::kSteerLo), 0x34);
    CHECK_EQ(v.steeringAngle(), 0x1234U);
}

TEST(vehicle_state_flag_helpers_do_not_disturb_neighbours)
{
    fake::reset();
    ram::VehicleState& v = ram::app().vehicle;

    v.setFlag(VS::kFlagAccOn, true);
    v.setFlag(VS::kFlagReverse, true);
    CHECK(v.accOn());
    CHECK(v.reverseOn());
    CHECK_FALSE(v.illumOn());

    v.setFlag(VS::kFlagAccOn, false);
    CHECK_FALSE(v.accOn());
    CHECK(v.reverseOn());
}

TEST(vehicle_state_amp_selector_walks_volume_then_the_five_tone_bytes)
{
    fake::reset();
    ram::VehicleState& v = ram::app().vehicle;

    for (std::uint8_t sel = VS::kAmpSelFirst; sel <= VS::kAmpSelLast; ++sel) {
        v.ampByte(sel) = static_cast<std::uint8_t>(0xA0U + sel);
    }

    CHECK_EQ(v.ampLevel(), 0xA1);                  /* selector 1 is the volume */
    CHECK_EQ(v.at(VS::kAmpTone), 0xA2);
    CHECK_EQ(v.at(VS::kAmpTone + 4U), 0xA6);
    /* ...and the whole run is the 0xA6 payload the head unit receives. */
    CHECK_EQ(v.window(kWinA6)[0], 0xA1);
    CHECK_EQ(v.window(kWinA6)[5], 0xA6);
}

TEST(can_rings_are_50_entries_of_16_bytes)
{
    fake::reset();
    CHECK_EQ(sizeof(ram::app().canRxRing), 800U);
    CHECK_EQ(sizeof(ram::app().canTxRing), 800U);
    CHECK_EQ(ram::kCanRingEntries * ram::kCanRingStride, 800U);
    /* An entry must hold its header plus eight data bytes. */
    CHECK(ram::kCanEntryData + 8U <= ram::kCanRingStride);
}

TEST(soft_timer_pool_is_64_signed_slots)
{
    fake::reset();
    CHECK_EQ(ram::kSoftTimerSlots, 64U);
    CHECK_EQ(sizeof(ram::app().softTimers), 64U * 4U);
    /* Signed: the ageing loop floors at zero and must never wrap. */
    ram::app().softTimers[0] = -1;
    CHECK(ram::app().softTimers[0] < 0);
}

TEST(version_string_is_its_own_symbol_outside_the_state_object)
{
    fake::reset();

    /* tools/iap_upload.py --verify resolves `g_versionString` out of the ELF and
     * reads exactly 17 bytes back over SWD to prove the image booted. It must
     * stay a standalone object: folded into AppRam, the uploader would need a
     * hardcoded offset again, which is what this refactor removed. */
    CHECK_EQ(sizeof(ram::g_versionString), 17U);

    const auto* state = reinterpret_cast<const std::uint8_t*>(&ram::app());
    const auto* version = const_cast<const std::uint8_t*>(&ram::g_versionString[0]);
    const bool inside = (version >= state) && (version < state + sizeof(ram::AppRam));
    CHECK_FALSE(inside);
}

TEST(harness_reset_zeroes_the_state_object)
{
    ram::app().vehicle.setAmpLevel(0x5A);
    ram::app().sched.counter = 12345U;
    ram::g_versionString[0] = 'X';

    fake::reset();

    CHECK_EQ(ram::app().vehicle.ampLevel(), 0);
    CHECK_EQ(ram::app().sched.counter, 0U);
    CHECK_EQ(ram::g_versionString[0], 0);
    CHECK_EQ(fake::callCount(), 0U);
}

TEST(harness_records_hal_calls)
{
    fake::reset();

    gpio_write_pin(GPIOB, GPIO_PIN_5, 1);
    gpio_write_pin(GPIOC, GPIO_PIN_15, 0);

    CHECK_EQ(fake::countOf("gpio_write_pin"), 2U);

    const fake::Call* first = fake::nthCall("gpio_write_pin", 0U);
    CHECK(first != nullptr);
    if (first != nullptr) {
        CHECK_EQ(first->a0, 0xB0U);        /* GPIOB */
        CHECK_EQ(first->a1, GPIO_PIN_5);
        CHECK_EQ(first->a2, 1U);
    }
}

TEST(harness_models_the_parameter_page)
{
    fake::reset();

    /* Erased flash reads as ones. */
    CHECK_EQ(fake::paramWord(0U), 0xFFFFFFFFU);

    CHECK_EQ(flash_program_word(PARAM_PAGE_BASE, 0xAAU), FLASH_STS_COMPL);
    CHECK_EQ(fake::paramWord(0U), 0xAAU);
    CHECK_EQ(flash_read_word(PARAM_PAGE_BASE), 0xAAU);

    CHECK_EQ(flash_erase_page(PARAM_PAGE_BASE), FLASH_STS_COMPL);
    CHECK_EQ(fake::paramWord(0U), 0xFFFFFFFFU);
    CHECK_EQ(fake::eraseCount(), 1U);
}

TEST(harness_refuses_flash_writes_outside_the_param_page)
{
    fake::reset();

    /* The app image itself. Writing here destroys the running firmware. */
    CHECK_EQ(flash_program_word(0x08002000U, 0U), FLASH_STS_REFUSED);
    /* The IAP bootloader -- unerasable, and the only way to reflash the box. */
    CHECK_EQ(flash_erase_page(0x08000000U), FLASH_STS_REFUSED);
    CHECK_EQ(fake::eraseCount(), 0U);
}
