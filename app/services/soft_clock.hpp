/* ============================================================================
 * soft_clock.hpp -- the soft real-time clock and the 500 ms heartbeat,
 * namespace svc::clock.
 *
 * Ported from reference/decompile/app/sys.c (0x08007898, 0x080078DA, 0x080078EE) and
 * reference/decompile/app/misc.c (0x08007858), Ghidra output,. Every entry
 * point carries the ORIGINAL flash address of the stock function it reproduces.
 *
 * ---------------------------------------------------------------------------
 * THE CLOCK
 *
 * ram::AppRam::softClock (0x200010FC) holds hour24 / minute / second / tenths
 * plus a flags byte and a derived 12-hour hour. It is advanced once per 100 ms
 * from the TIM2 divider chain, and it is the firmware's only time base of
 * record -- which is the independent confirmation that the TIM2 interrupt
 * really fires at 1 kHz. If the seconds run fast or slow, the tick does too.
 *
 * ram::AppRam::blink (0x20001104) is the 500 ms heartbeat, toggled by tick()
 * and edge-detected on the 4 ms pass by blinkEdgeDetect():
 *     bit 0 (ram::kBlinkState)    the heartbeat itself
 *     bit 1 (ram::kBlinkPrev)     last value of bit 0 the detector saw
 *     bit 2 (ram::kBlinkChanged)  one-shot "bit 0 changed" pulse
 * ==========================================================================*/

#ifndef SOFT_CLOCK_HPP
#define SOFT_CLOCK_HPP

namespace svc {
namespace clock {

/* --- 0x080078DA sys_soft_clock_init ---
 * Zero hour24 / minute / second / tenths and recompute the 12-hour view.
 * Called once from main_loop's init block.
 *
 * It deliberately does NOT clear the flags byte at 0x20001100: the stock code
 * leaves that to the scatter-load zero fill, and recalc12h() rewrites the only
 * bit of it that this module owns anyway. */
void init() noexcept;

/* --- 0x080078EE sys_soft_clock_tick ---
 * The 100 ms hook. Toggles the heartbeat bit on tenths 0 and 5 (i.e. every
 * 500 ms) BEFORE the increment, then advances tenths -> second -> minute ->
 * hour -> day. The "time changed" flag (ram::kSoftClockChanged) is raised on
 * the minute rollover only; nothing in this module ever clears it -- the UART
 * clock-report consumer does. */
void tick() noexcept;

/* --- 0x08007898 sys_soft_clock_recalc_12h ---
 * Derive the AM/PM bit (ram::kSoftClockPm, set for hour24 >= 12) and the
 * 12-hour hour from the 24-hour hour: hour12 = hour24 % 12, with 0 mapped to
 * 12, so both midnight and noon read 12. */
void recalc12h() noexcept;

/* --- 0x08007858 status_bit_change_detect ---
 * The 4 ms heartbeat edge detector. Compares the heartbeat bit against the
 * shadow taken the last time it moved: equal -> clear the one-shot "changed"
 * bit, different -> set it and re-shadow. The consumer therefore sees
 * ram::kBlinkChanged set on exactly one 4 ms pass per toggle. */
void blinkEdgeDetect() noexcept;

}  // namespace clock
}  // namespace svc

#endif /* SOFT_CLOCK_HPP */
