/* ============================================================================
 * test_soft_clock.cpp -- svc::clock, the soft RTC and the 500 ms heartbeat.
 *
 * Written from the BEHAVIOURAL SPEC, not from soft_clock.cpp:
 *   reference/decompile/app/sys.c   0x08007898 sys_soft_clock_recalc_12h
 *                   0x080078DA sys_soft_clock_init
 *                   0x080078EE sys_soft_clock_tick
 *   reference/decompile/app/misc.c  0x08007858 status_bit_change_detect
 *
 * The clock is the firmware's only time base of record, so these cases are
 * also the check that 1000 interrupts really make a second: the scheduler
 * tests drive the divider chain, and these drive what it lands on.
 * ==========================================================================*/

#include "test_framework.hpp"

#include <cstdint>

#include "fake_hal.hpp"
#include "ram_map.hpp"
#include "soft_clock.hpp"

namespace {

/* Advance the clock by n 100 ms hooks. */
void tickTenths(std::uint32_t n) noexcept
{
    for (std::uint32_t i = 0U; i < n; ++i) { svc::clock::tick(); }
}

void setTime(std::uint8_t h, std::uint8_t m, std::uint8_t s, std::uint8_t t) noexcept
{
    ram::app().softClock.hour24 = h;
    ram::app().softClock.minute = m;
    ram::app().softClock.second = s;
    ram::app().softClock.tenths = t;
}

}  // namespace

/* ---------------------------------------------------------------------------
 * init -- 0x080078DA
 * ------------------------------------------------------------------------- */

TEST(clock_init_zeroes_the_time_and_derives_midnight_as_twelve)
{
    fake::reset();

    setTime(7U, 8U, 9U, 3U);
    svc::clock::init();

    CHECK_EQ(ram::app().softClock.hour24, 0U);
    CHECK_EQ(ram::app().softClock.minute, 0U);
    CHECK_EQ(ram::app().softClock.second, 0U);
    CHECK_EQ(ram::app().softClock.tenths, 0U);
    CHECK_EQ(ram::app().softClock.hour12, 12U);
    CHECK_EQ(ram::app().softClock.flags & ram::kSoftClockPm, 0U);
}

/* The stock init does NOT clear the flags byte at 0x20001100: it leaves that to
 * the scatter-load zero fill, and only ever rewrites the PM bit. A "time
 * changed" flag already raised therefore survives an init. */
TEST(clock_init_leaves_the_changed_flag_alone)
{
    fake::reset();

    ram::app().softClock.flags = ram::kSoftClockChanged;
    svc::clock::init();

    CHECK_EQ(ram::app().softClock.flags & ram::kSoftClockChanged,
             ram::kSoftClockChanged);
}

/* ---------------------------------------------------------------------------
 * 12-hour conversion -- 0x08007898
 * ------------------------------------------------------------------------- */

TEST(clock_12h_conversion_maps_zero_and_twelve_to_twelve)
{
    fake::reset();

    struct { std::uint8_t h24; std::uint8_t h12; bool pm; } const cases[] = {
        { 0U, 12U, false},  /* midnight reads 12 AM */
        { 1U,  1U, false},
        {11U, 11U, false},
        {12U, 12U, true },  /* noon reads 12 PM     */
        {13U,  1U, true },
        {23U, 11U, true },
    };

    for (const auto& c : cases) {
        ram::app().softClock.hour24 = c.h24;
        ram::app().softClock.flags  = 0U;
        svc::clock::recalc12h();

        CHECK_EQ(ram::app().softClock.hour12, c.h12);
        CHECK_EQ((ram::app().softClock.flags & ram::kSoftClockPm) != 0U, c.pm);
    }
}

/* The PM bit is a bit assign, not a toggle: recomputing twice must not flip it
 * back, and it must not disturb the "changed" bit beside it. */
TEST(clock_12h_conversion_assigns_the_pm_bit_and_spares_its_neighbour)
{
    fake::reset();

    ram::app().softClock.flags  = ram::kSoftClockChanged | ram::kSoftClockPm;
    ram::app().softClock.hour24 = 9U;
    svc::clock::recalc12h();
    svc::clock::recalc12h();
    CHECK_EQ(ram::app().softClock.flags & ram::kSoftClockPm, 0U);
    CHECK_EQ(ram::app().softClock.flags & ram::kSoftClockChanged,
             ram::kSoftClockChanged);

    ram::app().softClock.hour24 = 21U;
    svc::clock::recalc12h();
    svc::clock::recalc12h();
    CHECK_EQ(ram::app().softClock.flags & ram::kSoftClockPm, ram::kSoftClockPm);
    CHECK_EQ(ram::app().softClock.flags & ram::kSoftClockChanged,
             ram::kSoftClockChanged);
}

/* ---------------------------------------------------------------------------
 * The cascade -- 0x080078EE
 * ------------------------------------------------------------------------- */

TEST(clock_ticks_tenths_then_seconds_then_minutes_then_hours)
{
    fake::reset();
    svc::clock::init();

    tickTenths(9U);
    CHECK_EQ(ram::app().softClock.tenths, 9U);
    CHECK_EQ(ram::app().softClock.second, 0U);

    tickTenths(1U);                       /* the 10th tenth */
    CHECK_EQ(ram::app().softClock.tenths, 0U);
    CHECK_EQ(ram::app().softClock.second, 1U);

    tickTenths(590U);                     /* 59 more seconds */
    CHECK_EQ(ram::app().softClock.second, 0U);
    CHECK_EQ(ram::app().softClock.minute, 1U);

    tickTenths(59U * 600U);               /* 59 more minutes */
    CHECK_EQ(ram::app().softClock.minute, 0U);
    CHECK_EQ(ram::app().softClock.hour24, 1U);
    CHECK_EQ(ram::app().softClock.hour12, 1U);
}

TEST(clock_wraps_from_235959_point_9_to_midnight)
{
    fake::reset();

    setTime(23U, 59U, 59U, 9U);
    ram::app().softClock.flags = 0U;
    svc::clock::recalc12h();
    CHECK_EQ(ram::app().softClock.flags & ram::kSoftClockPm, ram::kSoftClockPm);

    svc::clock::tick();                   /* the tenth that rolls the day */

    CHECK_EQ(ram::app().softClock.tenths, 0U);
    CHECK_EQ(ram::app().softClock.second, 0U);
    CHECK_EQ(ram::app().softClock.minute, 0U);
    CHECK_EQ(ram::app().softClock.hour24, 0U);
    /* The 12-hour view is recomputed on the hour rollover, so midnight must
     * read 12 AM immediately, not 12 PM left over from 23:00. */
    CHECK_EQ(ram::app().softClock.hour12, 12U);
    CHECK_EQ(ram::app().softClock.flags & ram::kSoftClockPm, 0U);
}

/* The "time changed" flag is raised on the MINUTE rollover only -- not on
 * tenths, not on seconds -- and nothing in this module ever clears it. */
TEST(clock_changed_flag_is_raised_only_on_the_minute_rollover)
{
    fake::reset();
    svc::clock::init();

    tickTenths(599U);                     /* 59.9 s: no minute yet */
    CHECK_EQ(ram::app().softClock.flags & ram::kSoftClockChanged, 0U);

    svc::clock::tick();                   /* 60.0 s */
    CHECK_EQ(ram::app().softClock.second, 0U);
    CHECK_EQ(ram::app().softClock.minute, 1U);
    CHECK_EQ(ram::app().softClock.flags & ram::kSoftClockChanged,
             ram::kSoftClockChanged);

    /* It latches: the clock itself never clears it (the UART clock-report
     * consumer does). */
    tickTenths(5U);
    CHECK_EQ(ram::app().softClock.flags & ram::kSoftClockChanged,
             ram::kSoftClockChanged);
}

/* ---------------------------------------------------------------------------
 * The 500 ms heartbeat
 * ------------------------------------------------------------------------- */

/* The toggle is evaluated BEFORE the increment and on (tenths % 5) == 0, i.e.
 * on the hooks that find tenths == 0 and tenths == 5 -- every 500 ms. */
TEST(clock_blink_bit_toggles_every_500ms)
{
    fake::reset();
    svc::clock::init();

    std::uint32_t toggles = 0U;
    std::uint8_t  prev    = static_cast<std::uint8_t>(ram::app().blink & ram::kBlinkState);

    /* 10 s = 100 hooks, which must produce exactly 20 toggles. */
    for (std::uint32_t i = 0U; i < 100U; ++i) {
        svc::clock::tick();
        const std::uint8_t now =
            static_cast<std::uint8_t>(ram::app().blink & ram::kBlinkState);
        if (now != prev) {
            ++toggles;
            /* Every toggle lands on a half-second boundary: the hook that
             * toggled saw tenths 0 or 5, so afterwards tenths is 1 or 6. */
            CHECK((ram::app().softClock.tenths == 1U)
                  || (ram::app().softClock.tenths == 6U));
            prev = now;
        }
    }
    CHECK_EQ(toggles, 20U);
}

/* The toggle must touch bit 0 only: bits 1 and 2 belong to the edge detector. */
TEST(clock_blink_toggle_leaves_the_detector_bits_alone)
{
    fake::reset();
    svc::clock::init();

    ram::app().blink = ram::kBlinkPrev | ram::kBlinkChanged;
    svc::clock::tick();                   /* tenths 0 -> toggles */

    CHECK_EQ(ram::app().blink & ram::kBlinkState, ram::kBlinkState);
    CHECK_EQ(ram::app().blink & ram::kBlinkPrev, ram::kBlinkPrev);
    CHECK_EQ(ram::app().blink & ram::kBlinkChanged, ram::kBlinkChanged);
}

/* ---------------------------------------------------------------------------
 * The edge detector -- 0x08007858
 * ------------------------------------------------------------------------- */

TEST(clock_blink_edge_detector_pulses_for_exactly_one_pass)
{
    fake::reset();

    /* Steady state: heartbeat and shadow agree, so no pulse. */
    ram::app().blink = 0U;
    svc::clock::blinkEdgeDetect();
    CHECK_EQ(ram::app().blink & ram::kBlinkChanged, 0U);

    /* The 100 ms hook flips the heartbeat ... */
    ram::app().blink = static_cast<std::uint8_t>(ram::app().blink | ram::kBlinkState);

    /* ... and the next 4 ms pass sees the edge: pulse set, shadow caught up. */
    svc::clock::blinkEdgeDetect();
    CHECK_EQ(ram::app().blink & ram::kBlinkChanged, ram::kBlinkChanged);
    CHECK_EQ(ram::app().blink & ram::kBlinkPrev, ram::kBlinkPrev);

    /* The pass after that clears it again -- one pulse per toggle, no more. */
    svc::clock::blinkEdgeDetect();
    CHECK_EQ(ram::app().blink & ram::kBlinkChanged, 0U);
    svc::clock::blinkEdgeDetect();
    CHECK_EQ(ram::app().blink & ram::kBlinkChanged, 0U);

    /* And the falling edge pulses too. */
    ram::app().blink = static_cast<std::uint8_t>(
        ram::app().blink & static_cast<std::uint8_t>(~ram::kBlinkState));
    svc::clock::blinkEdgeDetect();
    CHECK_EQ(ram::app().blink & ram::kBlinkChanged, ram::kBlinkChanged);
    CHECK_EQ(ram::app().blink & ram::kBlinkPrev, 0U);
    svc::clock::blinkEdgeDetect();
    CHECK_EQ(ram::app().blink & ram::kBlinkChanged, 0U);
}

/* Driven end to end: 100 ms hooks toggling, 4 ms passes detecting. Over one
 * second there are two toggles, so exactly two pulses. */
TEST(clock_blink_edge_detector_pulses_twice_per_second_end_to_end)
{
    fake::reset();
    svc::clock::init();

    std::uint32_t pulses = 0U;

    /* 1 s = 10 clock hooks; between each there are 25 4 ms passes. */
    for (std::uint32_t tenth = 0U; tenth < 10U; ++tenth) {
        svc::clock::tick();
        for (std::uint32_t pass = 0U; pass < 25U; ++pass) {
            svc::clock::blinkEdgeDetect();
            if ((ram::app().blink & ram::kBlinkChanged) != 0U) { ++pulses; }
        }
    }

    CHECK_EQ(pulses, 2U);
}
