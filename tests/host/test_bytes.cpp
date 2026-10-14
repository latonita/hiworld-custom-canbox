/* ============================================================================
 * test_bytes.cpp -- svc::bytes, the four leaf helpers.
 *
 * Written from the behaviour in reference/decompile/app/util.c (the Ghidra decompile), not
 * from app/services/bytes.cpp. Two of these four have a polarity that is the
 * opposite of the obvious one, and both of those have live callers that depend
 * on it -- which is exactly why they are worth pinning down.
 * ==========================================================================*/

#include "test_framework.hpp"

#include "bytes.hpp"
#include "fake_hal.hpp"
#include "ram_map.hpp"

namespace {

constexpr std::uint8_t kClamp = 1U;  /* readability at the call sites below */
constexpr std::uint8_t kWrap  = 0U;

}  // namespace

TEST(bytes_copy_moves_exactly_len_bytes)
{
    fake::reset();

    volatile std::uint8_t src[6] = {1U, 2U, 3U, 4U, 5U, 6U};
    volatile std::uint8_t dst[6] = {0U, 0U, 0U, 0U, 0U, 0U};

    svc::bytes::copy(dst, src, 4U);

    CHECK_EQ(dst[0], 1);
    CHECK_EQ(dst[3], 4);
    /* The tail must be untouched -- the UART shadow copies rely on this. */
    CHECK_EQ(dst[4], 0);
    CHECK_EQ(dst[5], 0);
}

TEST(bytes_copy_of_zero_length_does_nothing)
{
    fake::reset();

    volatile std::uint8_t src[2] = {0xAAU, 0xAAU};
    volatile std::uint8_t dst[2] = {0x00U, 0x00U};

    svc::bytes::copy(dst, src, 0U);

    CHECK_EQ(dst[0], 0);
    CHECK_EQ(dst[1], 0);
}

TEST(bytes_differ_reports_true_on_the_first_difference)
{
    fake::reset();

    volatile std::uint8_t a[4] = {1U, 2U, 3U, 4U};
    volatile std::uint8_t b[4] = {1U, 2U, 9U, 4U};

    /* Polarity check: this is NOT memcmp. Callers use the result directly as
     * "something changed", which is what drives the UART send decision. */
    CHECK(svc::bytes::differ(a, b, 4U));
    CHECK_FALSE(svc::bytes::differ(a, b, 2U));   /* difference is past the end */
    CHECK_FALSE(svc::bytes::differ(a, a, 4U));
}

TEST(bytes_differ_of_zero_length_is_false)
{
    fake::reset();

    volatile std::uint8_t a[1] = {0x11U};
    volatile std::uint8_t b[1] = {0x22U};

    CHECK_FALSE(svc::bytes::differ(a, b, 0U));
}

TEST(bytes_wrap_index_wraps_in_both_directions)
{
    fake::reset();

    /* The UART TX scheduler's use: advance a slot index 0..5, wrapping. */
    CHECK_EQ(svc::bytes::wrapIndex(0, 1, 5, 0, kWrap), 1);
    CHECK_EQ(svc::bytes::wrapIndex(4, 1, 5, 0, kWrap), 5);
    CHECK_EQ(svc::bytes::wrapIndex(5, 1, 5, 0, kWrap), 0);   /* past hi -> lo */

    /* The soft clock's use: step an hour backwards, wrapping. */
    CHECK_EQ(svc::bytes::wrapIndex(0, -1, 23, 0, kWrap), 23); /* below lo -> hi */
    CHECK_EQ(svc::bytes::wrapIndex(23, 1, 23, 0, kWrap), 0);
}

TEST(bytes_wrap_index_clamp_mode_saturates)
{
    fake::reset();

    /* The stock argument is called wrap_mode and its sense is the OPPOSITE of
     * the name: non-zero means clamp. Pinning that down here is the point of
     * this case -- getting it backwards would make the clock jump instead of
     * roll over. */
    CHECK_EQ(svc::bytes::wrapIndex(5, 1, 5, 0, kClamp), 5);
    CHECK_EQ(svc::bytes::wrapIndex(0, -1, 5, 0, kClamp), 0);
    CHECK_EQ(svc::bytes::wrapIndex(3, 1, 5, 0, kClamp), 4);  /* in range: same */
}

TEST(bytes_wrap_index_handles_a_delta_that_overshoots)
{
    fake::reset();

    /* base + delta is computed in int before narrowing, exactly as stock. */
    CHECK_EQ(svc::bytes::wrapIndex(5, 10, 5, 0, kWrap), 0);
    CHECK_EQ(svc::bytes::wrapIndex(0, -10, 5, 0, kWrap), 5);
    CHECK_EQ(svc::bytes::wrapIndex(0, -10, 5, 0, kClamp), 0);
}

TEST(bytes_bit_test_is_a_mask_test_not_an_equality)
{
    fake::reset();

    CHECK(svc::bytes::bitTest(0xF0U, 0x10U));
    CHECK(svc::bytes::bitTest(0xF0U, 0x30U));   /* any bit in the mask counts */
    CHECK_FALSE(svc::bytes::bitTest(0xF0U, 0x0FU));
    CHECK_FALSE(svc::bytes::bitTest(0x00U, 0xFFU));
}

TEST(bytes_bit_test_is_usable_in_a_constant_expression)
{
    fake::reset();

    /* constexpr matters: it is how the decoders' bit checks fold away instead
     * of becoming the out-of-line call the stock image has. */
    static_assert(svc::bytes::bitTest(0x04U, 0x04U), "");
    static_assert(!svc::bytes::bitTest(0x04U, 0x08U), "");
    CHECK(true);
}

TEST(bytes_bit_assign_sets_and_clears_only_its_mask)
{
    fake::reset();

    volatile std::uint8_t v = 0x00U;

    svc::bytes::bitAssign(v, 0x04U, true);
    CHECK_EQ(v, 0x04);

    svc::bytes::bitAssign(v, 0x80U, true);
    CHECK_EQ(v, 0x84);

    svc::bytes::bitAssign(v, 0x04U, false);
    CHECK_EQ(v, 0x80);

    /* Clearing a bit that is already clear must not disturb the others. */
    svc::bytes::bitAssign(v, 0x01U, false);
    CHECK_EQ(v, 0x80);
}

TEST(bytes_helpers_work_through_volatile_pointers)
{
    fake::reset();

    /* Every real call site moves data in or out of the fixed-address RAM
     * block, so the volatile-qualified overloads are the ones that matter. */
    ram::app().vehicle.setAmpLevel(0x1EU);
    ram::app().uartShadows.msgA6[0] = 0x00U;

    const volatile std::uint8_t* live =
        ram::app().vehicle.window(ram::VehicleState::kWinA6Off);

    CHECK(svc::bytes::differ(live, ram::app().uartShadows.msgA6, 1U));

    svc::bytes::copy(ram::app().uartShadows.msgA6, live,
                     static_cast<std::uint8_t>(ram::VehicleState::kWinA6Len));

    CHECK_FALSE(svc::bytes::differ(live, ram::app().uartShadows.msgA6,
                                   static_cast<std::uint8_t>(
                                       ram::VehicleState::kWinA6Len)));
    CHECK_EQ(ram::app().uartShadows.msgA6[0], 0x1E);
}
