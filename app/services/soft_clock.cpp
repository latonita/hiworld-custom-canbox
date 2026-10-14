/* ============================================================================
 * soft_clock.cpp -- see soft_clock.hpp.
 *
 * Ported from reference/decompile/app/sys.c (0x08007898, 0x080078DA, 0x080078EE) and
 * reference/decompile/app/misc.c (0x08007858), Ghidra output,.
 *
 * Pure RAM state; no HAL header is needed.
 *
 * A NOTE ON WIDTHS
 * The stock code reads and writes the flags byte at 0x20001100 as a 32-bit
 * word, with masks 0xFFFFFFFE / 0xFFFFFFFD. Only bits 0 and 1 are ever
 * touched, and the read-modify-write preserves 0x20001101 (the 12-hour hour)
 * either way, so a byte access has an identical memory effect. ram_map.hpp
 * models it as a byte; see docs/misra-cpp.md note N-1.
 * ==========================================================================*/

#include "soft_clock.hpp"

#include <cstdint>

#include "ram_map.hpp"

namespace svc {
namespace clock {

/* --- 0x08007898 sys_soft_clock_recalc_12h ---
 * The stock form is `flags = (flags & ~PM)` for hour < 12 and
 * `flags = (flags & ~PM) + PM` otherwise -- i.e. a plain bit assign. */
void recalc12h() noexcept
{
    ram::SoftClock& c = ram::app().softClock;

    const std::uint8_t flags =
        static_cast<std::uint8_t>(c.flags & static_cast<std::uint8_t>(~ram::kSoftClockPm));

    if (c.hour24 < 12U) {
        c.flags = flags;
    } else {
        c.flags = static_cast<std::uint8_t>(flags | ram::kSoftClockPm);
    }

    /* hour24 % 12 with 0 folded to 12: midnight and noon both read 12. */
    c.hour12 = static_cast<std::uint8_t>(c.hour24 % 12U);
    if (c.hour12 == 0U) {
        c.hour12 = 12U;
    }
}

/* --- 0x080078DA sys_soft_clock_init ---
 * Zero h/m/s/tenths, then recompute the 12-hour view. The flags byte is NOT
 * cleared here -- the stock code leaves that to the scatter-load zero fill. */
void init() noexcept
{
    ram::SoftClock& c = ram::app().softClock;

    c.hour24 = 0U;
    c.minute = 0U;
    c.second = 0U;
    c.tenths = 0U;

    recalc12h();
}

/* --- 0x080078EE sys_soft_clock_tick ---
 * One call per 100 ms, from the TIM2 divider chain.
 *
 * The heartbeat toggle is evaluated BEFORE the increment and on
 * (tenths % 5) == 0, i.e. on tenths 0 and 5 -- every 500 ms. Doing it first is
 * what makes the toggle land on tenths 0 and 5 rather than 1 and 6. */
void tick() noexcept
{
    ram::SoftClock& c = ram::app().softClock;

    if ((c.tenths % 5U) == 0U) {
        ram::app().blink =
            static_cast<std::uint8_t>(ram::app().blink ^ ram::kBlinkState);
    }

    ++c.tenths;
    if (c.tenths > 9U) {
        c.tenths = 0U;
        ++c.second;
        if (c.second > 59U) {
            c.second = 0U;
            ++c.minute;
            /* "time changed", raised on the minute rollover only. The UART
             * clock-report consumer is what clears it; nothing here does. */
            c.flags = static_cast<std::uint8_t>(c.flags | ram::kSoftClockChanged);
            if (c.minute > 59U) {
                c.minute = 0U;
                ++c.hour24;
                if (c.hour24 == 24U) {
                    c.hour24 = 0U;
                }
                recalc12h();
            }
        }
    }
}

/* --- 0x08007858 status_bit_change_detect ---
 * Run from main_loop on the 4 ms flag.
 *
 *   if (bit0 == bit1)  ->  blink &= ~CHANGED
 *   else               ->  blink |= CHANGED;  then  bit1 = bit0
 *
 * The two stores in the else branch are the stock instruction stream (the
 * decompile writes the word twice, the second time re-reading what it just
 * stored), and `blink` is volatile, so both are kept rather than folded. */
void blinkEdgeDetect() noexcept
{
    const std::uint8_t v = ram::app().blink;

    const std::uint8_t state = static_cast<std::uint8_t>(v & ram::kBlinkState);
    const std::uint8_t prev  = static_cast<std::uint8_t>(
        (v & static_cast<std::uint8_t>(ram::kBlinkState | ram::kBlinkPrev)) >> 1);

    if (state == prev) {
        ram::app().blink =
            static_cast<std::uint8_t>(v & static_cast<std::uint8_t>(~ram::kBlinkChanged));
    } else {
        const std::uint8_t pulsed = static_cast<std::uint8_t>(v | ram::kBlinkChanged);
        ram::app().blink = pulsed;
        ram::app().blink = static_cast<std::uint8_t>(
            (pulsed & static_cast<std::uint8_t>(~ram::kBlinkPrev))
            | static_cast<std::uint8_t>((pulsed & ram::kBlinkState) << 1));
    }
}

}  // namespace clock
}  // namespace svc
