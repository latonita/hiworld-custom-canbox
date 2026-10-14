/* ============================================================================
 * test_soft_timer.cpp -- svc::timers, the 64-slot pool and the short
 * countdowns.
 *
 * Written from the BEHAVIOURAL SPEC, not from soft_timer.cpp:
 *   reference/decompile/app/util.c  0x08007CD2 softtimer_arm_at_index
 *                   0x08007CE0 softtimer_clear_current
 *                   0x08007CEE softtimer_tick_all
 *                   0x08007D12 softtimer_scan_active_hi
 *   reference/decompile/app/sys.c   0x080083E2 sys_misc_timers_tick
 *
 * The pool is the SWC key-hold tracker: a slot armed by the CAN decoder ages
 * once per 10 ms and the 4 ms scan publishes whichever slot is still running.
 * If tickAll stops ageing, a pressed steering-wheel key never releases; if the
 * scan's wrap is wrong, the wrong key is reported.
 * ==========================================================================*/

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>

#include "fake_hal.hpp"
#include "ram_map.hpp"
#include "soft_timer.hpp"

namespace {

/* The pool geometry, restated from the retired app/port/util.h rather than read out of
 * ram_map.hpp, so this file cross-checks the map instead of echoing it. */
constexpr std::size_t  kSlots  = 64U;      /* tick_all walks idx < 0x40      */
constexpr std::int32_t kReload = 150;      /* 0x96 counts = 1.5 s at 10 ms   */
constexpr std::uint8_t kIdle   = 0x16U;    /* the "nothing held" sentinel    */
constexpr std::uint8_t kStart  = 0x17U;    /* where the sweep begins         */

std::size_t nonZeroSlots() noexcept
{
    std::size_t n = 0U;
    for (std::size_t i = 0U; i < kSlots; ++i) {
        if (ram::app().softTimers[i] != 0) { ++n; }
    }
    return n;
}

}  // namespace

/* ---------------------------------------------------------------------------
 * Arming and releasing -- 0x08007CD2 / 0x08007CE0
 * ------------------------------------------------------------------------- */

TEST(timers_arm_loads_the_slot_named_by_the_swc_keycode)
{
    fake::reset();

    ram::app().swc.keycode = 0x2AU;
    svc::timers::armForKeycode();

    CHECK_EQ(ram::app().softTimers[0x2A], kReload);
    CHECK_EQ(nonZeroSlots(), 1U);        /* and nothing else */

    /* Re-arming the same code restores the full 1.5 s -- this is what makes a
     * repeating key report look continuously held. */
    svc::timers::tickAll();
    svc::timers::tickAll();
    CHECK_EQ(ram::app().softTimers[0x2A], kReload - 2);
    svc::timers::armForKeycode();
    CHECK_EQ(ram::app().softTimers[0x2A], kReload);
}

TEST(timers_clearCurrent_releases_the_slot_the_scan_reported)
{
    fake::reset();

    ram::app().softTimers[0x1F] = kReload;
    ram::app().softTimers[0x20] = kReload;
    ram::app().swc.keydataRef   = 0x20U;

    svc::timers::clearCurrent();

    CHECK_EQ(ram::app().softTimers[0x20], 0);
    CHECK_EQ(ram::app().softTimers[0x1F], kReload);
}

/* ---------------------------------------------------------------------------
 * Ageing -- 0x08007CEE
 * ------------------------------------------------------------------------- */

TEST(timers_tickAll_ages_every_slot_by_one)
{
    fake::reset();

    for (std::size_t i = 0U; i < kSlots; ++i) {
        ram::app().softTimers[i] = static_cast<std::int32_t>(i) + 1;
    }

    svc::timers::tickAll();

    for (std::size_t i = 0U; i < kSlots; ++i) {
        CHECK_EQ(ram::app().softTimers[i], static_cast<std::int32_t>(i));
    }
}

/* The floor is a test-then-decrement, not a saturating subtract: a slot that
 * has already expired must stay at exactly 0 and never go negative, even
 * though the elements are signed. */
TEST(timers_tickAll_floors_at_zero_and_never_goes_negative)
{
    fake::reset();

    ram::app().softTimers[0] = 1;
    ram::app().softTimers[1] = 0;

    svc::timers::tickAll();
    CHECK_EQ(ram::app().softTimers[0], 0);
    CHECK_EQ(ram::app().softTimers[1], 0);

    for (std::uint32_t i = 0U; i < 100U; ++i) { svc::timers::tickAll(); }
    CHECK_EQ(ram::app().softTimers[0], 0);
    CHECK_EQ(ram::app().softTimers[1], 0);
}

/* 150 counts at 10 ms per count is 1.5 s of hold time, and the slot must still
 * be live on the 149th ageing -- an off-by-one here shortens every key hold. */
TEST(timers_an_armed_slot_lives_for_exactly_150_ticks)
{
    fake::reset();

    ram::app().swc.keycode = 0x30U;
    svc::timers::armForKeycode();

    for (std::int32_t i = 0; i < kReload - 1; ++i) { svc::timers::tickAll(); }
    CHECK_EQ(ram::app().softTimers[0x30], 1);

    svc::timers::tickAll();
    CHECK_EQ(ram::app().softTimers[0x30], 0);
}

/* ---------------------------------------------------------------------------
 * The scan -- 0x08007D12
 * ------------------------------------------------------------------------- */

TEST(timers_scanActive_reports_the_sentinel_when_nothing_is_held)
{
    fake::reset();

    ram::app().swc.keydataRef = 0x3FU;    /* stale value from a previous scan */
    svc::timers::scanActive();

    CHECK_EQ(ram::app().swc.keydataRef, kIdle);
}

TEST(timers_scanActive_starts_at_0x17)
{
    fake::reset();

    ram::app().softTimers[kStart] = 1;
    svc::timers::scanActive();
    CHECK_EQ(ram::app().swc.keydataRef, kStart);

    /* 0x17 wins over everything below it, which is the whole point of starting
     * high: high key codes get priority. */
    ram::app().softTimers[0x00] = kReload;
    ram::app().softTimers[0x05] = kReload;
    ram::app().softTimers[0x16] = kReload;
    svc::timers::scanActive();
    CHECK_EQ(ram::app().swc.keydataRef, kStart);
}

TEST(timers_scanActive_sweeps_the_whole_upper_half_in_order)
{
    fake::reset();

    for (std::uint8_t slot = kStart; slot <= 0x3FU; ++slot) {
        for (std::size_t i = 0U; i < kSlots; ++i) { ram::app().softTimers[i] = 0; }
        ram::app().softTimers[slot] = 1;
        ram::app().swc.keydataRef   = 0U;

        svc::timers::scanActive();
        CHECK_EQ(ram::app().swc.keydataRef, slot);
    }
}

/* The sweep is circular: past 0x3F it wraps to 0x00 and keeps going, so a slot
 * in the LOW half is found too -- it is not a scan of the upper range only. */
TEST(timers_scanActive_wraps_past_0x3F_into_the_low_half)
{
    fake::reset();

    ram::app().softTimers[0x05] = kReload;
    svc::timers::scanActive();
    CHECK_EQ(ram::app().swc.keydataRef, 0x05U);

    /* Slot 0 is reachable: it is the first slot after the wrap. */
    for (std::size_t i = 0U; i < kSlots; ++i) { ram::app().softTimers[i] = 0; }
    ram::app().softTimers[0x00] = kReload;
    svc::timers::scanActive();
    CHECK_EQ(ram::app().swc.keydataRef, 0x00U);

    /* And within the low half the lower index still wins, because the sweep
     * runs 0x00 upward after the wrap. */
    ram::app().softTimers[0x10] = kReload;
    svc::timers::scanActive();
    CHECK_EQ(ram::app().swc.keydataRef, 0x00U);
}

TEST(timers_scanActive_prefers_the_upper_half_over_the_lower)
{
    fake::reset();

    ram::app().softTimers[0x02] = kReload;
    ram::app().softTimers[0x3FU] = kReload;

    svc::timers::scanActive();
    CHECK_EQ(ram::app().swc.keydataRef, 0x3FU);
}

/* 0x16 is the LAST slot examined as well as the sentinel, so a key held in
 * slot 0x16 is indistinguishable from nothing being held. That ambiguity is in
 * the stock image; it is recorded here rather than designed away. */
TEST(timers_scanActive_slot_0x16_is_both_last_examined_and_the_sentinel)
{
    fake::reset();

    ram::app().softTimers[0x16] = kReload;
    svc::timers::scanActive();
    CHECK_EQ(ram::app().swc.keydataRef, kIdle);   /* same answer as "empty" */

    /* 0x15 is examined one step earlier, so it does win. */
    ram::app().softTimers[0x15] = kReload;
    svc::timers::scanActive();
    CHECK_EQ(ram::app().swc.keydataRef, 0x15U);
}

/* The scan must terminate on every input: a full pool, an empty pool, and a
 * pool whose only live slot sits immediately before the start index. */
TEST(timers_scanActive_terminates_on_a_full_pool)
{
    fake::reset();

    for (std::size_t i = 0U; i < kSlots; ++i) { ram::app().softTimers[i] = kReload; }
    svc::timers::scanActive();
    CHECK_EQ(ram::app().swc.keydataRef, kStart);
}

/* ---------------------------------------------------------------------------
 * The 10 ms hook -- 0x080083E2
 * ------------------------------------------------------------------------- */

/* TWO DIFFERENT FLOORS, both load-bearing. miscTimer runs down to 0 so its
 * consumer can use a plain "expired" test; the three SWC timers stop at 1 so an
 * "== 1" test in the consumer fires on exactly one pass and then latches. */
TEST(timers_short_countdowns_have_two_different_floors)
{
    fake::reset();

    ram::app().miscTimers.miscTimer   = 3U;
    ram::app().miscTimers.swcRespMode = 3U;
    ram::app().miscTimers.swcFlag     = 3U;
    ram::app().miscTimers.swcTimer    = 3U;

    svc::timers::tickShortTimers();
    CHECK_EQ(ram::app().miscTimers.miscTimer, 2U);
    CHECK_EQ(ram::app().miscTimers.swcRespMode, 2U);
    CHECK_EQ(ram::app().miscTimers.swcFlag, 2U);
    CHECK_EQ(ram::app().miscTimers.swcTimer, 2U);

    svc::timers::tickShortTimers();
    CHECK_EQ(ram::app().miscTimers.miscTimer, 1U);
    CHECK_EQ(ram::app().miscTimers.swcRespMode, 1U);
    CHECK_EQ(ram::app().miscTimers.swcFlag, 1U);
    CHECK_EQ(ram::app().miscTimers.swcTimer, 1U);

    /* Here the two floors part company. */
    svc::timers::tickShortTimers();
    CHECK_EQ(ram::app().miscTimers.miscTimer, 0U);
    CHECK_EQ(ram::app().miscTimers.swcRespMode, 1U);
    CHECK_EQ(ram::app().miscTimers.swcFlag, 1U);
    CHECK_EQ(ram::app().miscTimers.swcTimer, 1U);

    /* And they stay parted, however long the hook runs. */
    for (std::uint32_t i = 0U; i < 50U; ++i) { svc::timers::tickShortTimers(); }
    CHECK_EQ(ram::app().miscTimers.miscTimer, 0U);
    CHECK_EQ(ram::app().miscTimers.swcRespMode, 1U);
    CHECK_EQ(ram::app().miscTimers.swcFlag, 1U);
    CHECK_EQ(ram::app().miscTimers.swcTimer, 1U);
}

/* A SWC timer that starts at 0 must stay at 0: the floor is "> 1", so 0 is
 * below it and is left alone rather than wrapping to 255. */
TEST(timers_short_countdowns_do_not_underflow_from_zero)
{
    fake::reset();

    ram::app().miscTimers.miscTimer   = 0U;
    ram::app().miscTimers.swcRespMode = 0U;
    ram::app().miscTimers.swcFlag     = 0U;
    ram::app().miscTimers.swcTimer    = 0U;

    for (std::uint32_t i = 0U; i < 10U; ++i) { svc::timers::tickShortTimers(); }

    CHECK_EQ(ram::app().miscTimers.miscTimer, 0U);
    CHECK_EQ(ram::app().miscTimers.swcRespMode, 0U);
    CHECK_EQ(ram::app().miscTimers.swcFlag, 0U);
    CHECK_EQ(ram::app().miscTimers.swcTimer, 0U);
}

/* The hook ages the pool too -- that is its only other job, and the reason a
 * pressed SWC key releases 1.5 s after the decoder stops re-arming it. */
TEST(timers_short_timer_hook_also_ages_the_pool)
{
    fake::reset();

    ram::app().swc.keycode = 0x25U;
    svc::timers::armForKeycode();

    for (std::int32_t i = 0; i < kReload; ++i) { svc::timers::tickShortTimers(); }

    CHECK_EQ(ram::app().softTimers[0x25], 0);
    svc::timers::scanActive();
    CHECK_EQ(ram::app().swc.keydataRef, kIdle);   /* the key has released */
}
